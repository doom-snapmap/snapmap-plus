/* modexport.c -- see modexport.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>

#include "modexport.h"
#include "modstore.h"
#include "modbundle.h"
#include "modmanifest.h"
#include "modserve.h"
#include "mp_json.h"
#include "prefab_block.h"
#include "raw_deflate.h"
#include "backend_log.h"

#define ME_NAME_MAX        1024u
#define ME_MAX_ENTRIES     65535u
#define ME_LIMIT           0xFFFFFFFFull
#define ME_CD_MAX          (16u * 1024u * 1024u)
#define ME_INDEX_MAX       (1024u * 1024u)
#define ME_DOS_DATE        0x0021u     /* 1980-01-01: exports of the same content are byte-identical */
#define ME_DOS_TIME        0x0000u
#define ME_FLAG_UTF8       0x0800u

static int me_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static void me_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static double me_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

int modexport_is_zip_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n > 4 && _stricmp(path + n - 4, ".zip") == 0;
}

/* ------------------------------------------------------------ crc32 ----- */

static uint32_t g_crc_table[256];
static INIT_ONCE g_crc_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK me_crc_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    uint32_t i, k, c;
    (void)once; (void)p; (void)ctx;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    return TRUE;
}

static uint32_t me_crc32(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    InitOnceExecuteOnce(&g_crc_once, me_crc_init, NULL, NULL);
    for (i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void me_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void me_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned me_get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t me_get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int me_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

/* ------------------------------------------------------------ writer ---- */

typedef struct me_went { char *name; uint32_t crc, size, offset; unsigned flags; } me_went;

typedef struct me_writer {
    HANDLE h;
    unsigned long long off;
    me_went *e;
    size_t n, cap;
    int failed;
    char cause[256];
} me_writer;

static int me_wraw(me_writer *w, const void *p, size_t n)
{
    size_t off = 0;
    if (w->failed) return 0;
    while (off < n) {
        DWORD chunk = (DWORD)((n - off) > (1u << 24) ? (1u << 24) : (n - off)), put = 0;
        if (!WriteFile(w->h, (const unsigned char *)p + off, chunk, &put, NULL) || put != chunk) {
            w->failed = 1;
            me_cause(w->cause, sizeof w->cause, "writing the ZIP failed (error %lu)", GetLastError());
            return 0;
        }
        off += put;
    }
    w->off += n;
    return 1;
}

static int me_add(me_writer *w, const char *name, const void *data, size_t len)
{
    unsigned char hdr[30];
    size_t nl = strlen(name);
    me_went *e;
    if (w->failed) return 0;
    if (w->n >= ME_MAX_ENTRIES || nl == 0 || nl > ME_NAME_MAX) {
        w->failed = 1;
        return me_cause(w->cause, sizeof w->cause, w->n >= ME_MAX_ENTRIES ? "more than 65535 files" : "a file name is too long");
    }
    if (w->off + 30 + nl + len + 46 + nl + 22 >= ME_LIMIT) {
        w->failed = 1;
        return me_cause(w->cause, sizeof w->cause, "the export would exceed 4 GiB");
    }
    if (w->n == w->cap) {
        size_t c2 = w->cap ? w->cap * 2 : 64;
        me_went *g = (me_went *)realloc(w->e, sizeof *g * c2);
        if (!g) { w->failed = 1; return me_cause(w->cause, sizeof w->cause, "out of memory"); }
        w->e = g;
        w->cap = c2;
    }
    e = &w->e[w->n];
    e->name = _strdup(name);
    if (!e->name) { w->failed = 1; return me_cause(w->cause, sizeof w->cause, "out of memory"); }
    e->crc = me_crc32((const unsigned char *)data, len);
    e->size = (uint32_t)len;
    e->offset = (uint32_t)w->off;
    e->flags = me_ascii(name) ? 0 : ME_FLAG_UTF8;
    w->n++;
    me_put32(hdr, 0x04034b50u);
    me_put16(hdr + 4, 20);
    me_put16(hdr + 6, e->flags);
    me_put16(hdr + 8, 0);
    me_put16(hdr + 10, ME_DOS_TIME);
    me_put16(hdr + 12, ME_DOS_DATE);
    me_put32(hdr + 14, e->crc);
    me_put32(hdr + 18, e->size);
    me_put32(hdr + 22, e->size);
    me_put16(hdr + 26, (unsigned)nl);
    me_put16(hdr + 28, 0);
    return me_wraw(w, hdr, sizeof hdr) && me_wraw(w, name, nl) && me_wraw(w, data, len);
}

static int me_finish(me_writer *w)
{
    unsigned char hdr[46], end[22];
    unsigned long long cd = w->off;
    size_t i;
    for (i = 0; i < w->n && !w->failed; i++) {
        const me_went *e = &w->e[i];
        size_t nl = strlen(e->name);
        me_put32(hdr, 0x02014b50u);
        me_put16(hdr + 4, 20);
        me_put16(hdr + 6, 20);
        me_put16(hdr + 8, e->flags);
        me_put16(hdr + 10, 0);
        me_put16(hdr + 12, ME_DOS_TIME);
        me_put16(hdr + 14, ME_DOS_DATE);
        me_put32(hdr + 16, e->crc);
        me_put32(hdr + 20, e->size);
        me_put32(hdr + 24, e->size);
        me_put16(hdr + 28, (unsigned)nl);
        me_put16(hdr + 30, 0);
        me_put16(hdr + 32, 0);
        me_put16(hdr + 34, 0);
        me_put16(hdr + 36, 0);
        me_put32(hdr + 38, 0);
        me_put32(hdr + 42, e->offset);
        if (!me_wraw(w, hdr, sizeof hdr) || !me_wraw(w, e->name, nl)) return 0;
    }
    if (w->failed) return 0;
    if (w->off >= ME_LIMIT) { w->failed = 1; return me_cause(w->cause, sizeof w->cause, "the export would exceed 4 GiB"); }
    me_put32(end, 0x06054b50u);
    me_put16(end + 4, 0);
    me_put16(end + 6, 0);
    me_put16(end + 8, (unsigned)w->n);
    me_put16(end + 10, (unsigned)w->n);
    me_put32(end + 12, (uint32_t)(w->off - cd));
    me_put32(end + 16, (uint32_t)cd);
    me_put16(end + 20, 0);
    return me_wraw(w, end, sizeof end);
}

static void me_writer_free(me_writer *w)
{
    size_t i;
    for (i = 0; i < w->n; i++) free(w->e[i].name);
    free(w->e);
    w->e = NULL;
    w->n = w->cap = 0;
}

/* ------------------------------------------------------------ reader ---- */

typedef struct me_rent {
    char *name;
    unsigned method, flags;
    uint32_t crc, csize, usize, lho;
} me_rent;

typedef struct me_reader {
    HANDLE h;
    unsigned long long size;
    me_rent *e;
    size_t n, cap;
    size_t *sorted;     /* indexes sorted by name */
    wchar_t *dir;       /* an extracted export: each entry is a file under this folder */
} me_reader;

#define ME_METHOD_FILE 0xFFFFu   /* a folder export's entry: read from the file */

static int me_read_at(HANDLE h, unsigned long long at, void *buf, size_t n)
{
    LARGE_INTEGER li;
    size_t off = 0;
    li.QuadPart = (LONGLONG)at;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    while (off < n) {
        DWORD chunk = (DWORD)((n - off) > (1u << 24) ? (1u << 24) : (n - off)), rd = 0;
        if (!ReadFile(h, (unsigned char *)buf + off, chunk, &rd, NULL) || rd == 0) return 0;
        off += rd;
    }
    return 1;
}

/* A name an import may trust: relative, '/'-separated, no empty, "." or ".."
 * segment, no drive, backslash or control character. */
static int me_name_ok(const char *s, size_t n)
{
    size_t i, seg = 0;
    if (n == 0 || n > ME_NAME_MAX || s[0] == '/') return 0;
    for (i = 0; i <= n; i++) {
        unsigned char c = i < n ? (unsigned char)s[i] : '/';
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return 0;
        if (c == '/') {
            size_t len = i - seg;
            if (len == 0 && i < n) return 0;
            if ((len == 1 && s[seg] == '.') || (len == 2 && s[seg] == '.' && s[seg + 1] == '.')) return 0;
            seg = i + 1;
        }
    }
    return 1;
}

static const me_reader *g_sort_reader;
static int me_sort_cmp(const void *a, const void *b)
{
    return strcmp(g_sort_reader->e[*(const size_t *)a].name, g_sort_reader->e[*(const size_t *)b].name);
}

static void me_reader_close(me_reader *r)
{
    size_t i;
    if (r->h && r->h != INVALID_HANDLE_VALUE) CloseHandle(r->h);
    for (i = 0; i < r->n; i++) free(r->e[i].name);
    free(r->e);
    free(r->sorted);
    free(r->dir);
    memset(r, 0, sizeof *r);
}

static INIT_ONCE g_sort_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_sort_cs;
static BOOL CALLBACK me_sort_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_sort_cs);
    return TRUE;
}

/* Index the entries by name; two entries of one name refuse the export. */
static int me_reader_sort(me_reader *r, char *cause, size_t cap)
{
    size_t i;
    free(r->sorted);
    r->sorted = (size_t *)malloc(sizeof(size_t) * (r->n ? r->n : 1));
    if (!r->sorted) { me_reader_close(r); return me_cause(cause, cap, "out of memory"); }
    for (i = 0; i < r->n; i++) r->sorted[i] = i;
    InitOnceExecuteOnce(&g_sort_once, me_sort_init, NULL, NULL);
    EnterCriticalSection(&g_sort_cs);
    g_sort_reader = r;
    qsort(r->sorted, r->n, sizeof(size_t), me_sort_cmp);
    g_sort_reader = NULL;
    LeaveCriticalSection(&g_sort_cs);
    for (i = 1; i < r->n; i++)
        if (strcmp(r->e[r->sorted[i - 1]].name, r->e[r->sorted[i]].name) == 0) {
            me_reader_close(r);
            return me_cause(cause, cap, "the export holds two files with one name");
        }
    return 1;
}

static int me_reader_open(me_reader *r, const char *path, char *cause, size_t cap)
{
    wchar_t *w = sh_mb_wpath(path);
    LARGE_INTEGER sz;
    unsigned char tail[65557], *cd = NULL;
    size_t tail_n, i, pos, found = (size_t)-1;
    uint32_t cd_size, cd_off;
    unsigned entries;
    memset(r, 0, sizeof *r);
    r->h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    if (r->h == INVALID_HANDLE_VALUE) { r->h = NULL; return me_cause(cause, cap, "cannot open the export"); }
    if (!GetFileSizeEx(r->h, &sz) || sz.QuadPart < 22) { me_reader_close(r); return me_cause(cause, cap, "the export is not a ZIP file"); }
    r->size = (unsigned long long)sz.QuadPart;
    tail_n = r->size < sizeof tail ? (size_t)r->size : sizeof tail;
    if (!me_read_at(r->h, r->size - tail_n, tail, tail_n)) { me_reader_close(r); return me_cause(cause, cap, "cannot read the export"); }
    for (pos = tail_n - 22 + 1; pos-- > 0;) {
        if (me_get32(tail + pos) == 0x06054b50u && pos + 22 + me_get16(tail + pos + 20) == tail_n) { found = pos; break; }
    }
    if (found == (size_t)-1) { me_reader_close(r); return me_cause(cause, cap, "the export is not a ZIP file (no end record)"); }
    entries = me_get16(tail + found + 10);
    cd_size = me_get32(tail + found + 12);
    cd_off = me_get32(tail + found + 16);
    if (me_get16(tail + found + 4) != 0 || me_get16(tail + found + 6) != 0 || me_get16(tail + found + 8) != entries ||
        entries == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        me_reader_close(r);
        return me_cause(cause, cap, "the export is a split or ZIP64 archive, which is not supported");
    }
    if ((unsigned long long)cd_off + cd_size > r->size - tail_n + found || cd_size > ME_CD_MAX) {
        me_reader_close(r);
        return me_cause(cause, cap, "the export's file list is damaged");
    }
    cd = (unsigned char *)malloc(cd_size ? cd_size : 1);
    r->e = (me_rent *)calloc(entries ? entries : 1, sizeof *r->e);
    if (!cd || !r->e || !me_read_at(r->h, cd_off, cd, cd_size)) {
        free(cd);
        me_reader_close(r);
        return me_cause(cause, cap, "cannot read the export's file list");
    }
    for (i = 0, pos = 0; i < entries; i++) {
        me_rent *e = &r->e[i];
        unsigned nl, xl, cl;
        if (pos + 46 > cd_size || me_get32(cd + pos) != 0x02014b50u) { free(cd); me_reader_close(r); return me_cause(cause, cap, "the export's file list is damaged"); }
        e->flags = me_get16(cd + pos + 8);
        e->method = me_get16(cd + pos + 10);
        e->crc = me_get32(cd + pos + 16);
        e->csize = me_get32(cd + pos + 20);
        e->usize = me_get32(cd + pos + 24);
        nl = me_get16(cd + pos + 28);
        xl = me_get16(cd + pos + 30);
        cl = me_get16(cd + pos + 32);
        e->lho = me_get32(cd + pos + 42);
        if (pos + 46 + nl + xl + cl > cd_size || !me_name_ok((const char *)cd + pos + 46, nl)) {
            free(cd);
            me_reader_close(r);
            return me_cause(cause, cap, "the export holds a file with an unsafe name");
        }
        e->name = (char *)malloc(nl + 1);
        if (!e->name) { free(cd); me_reader_close(r); return me_cause(cause, cap, "out of memory"); }
        memcpy(e->name, cd + pos + 46, nl);
        e->name[nl] = 0;
        r->n++;
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    return me_reader_sort(r, cause, cap);
}

static long me_find(const me_reader *r, const char *name)
{
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(r->e[r->sorted[mid]].name, name);
        if (c == 0) return (long)r->sorted[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* A folder export's file: the entry name is relative to r->dir, '/'-separated. */
static wchar_t *me_dir_path(const me_reader *r, const char *name)
{
    size_t dl = wcslen(r->dir), i;
    int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    wchar_t *out;
    if (wl <= 0) return NULL;
    out = (wchar_t *)malloc(sizeof(wchar_t) * (dl + 1 + (size_t)wl));
    if (!out) return NULL;
    memcpy(out, r->dir, sizeof(wchar_t) * dl);
    out[dl] = L'\\';
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out + dl + 1, wl);
    for (i = dl + 1; out[i]; i++) if (out[i] == L'/') out[i] = L'\\';
    return out;
}

static unsigned char *me_read_dir_entry(const me_reader *r, const me_rent *e, size_t max, size_t *out_len, char *cause,
                                        size_t cap)
{
    wchar_t *w = me_dir_path(r, e->name);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    LARGE_INTEGER sz;
    unsigned char *dst;
    free(w);
    if (h == INVALID_HANDLE_VALUE) { me_cause(cause, cap, "cannot open %s", e->name); return NULL; }
    if (!GetFileSizeEx(h, &sz) || (unsigned long long)sz.QuadPart > max) {
        CloseHandle(h);
        me_cause(cause, cap, "%s is larger than %zu bytes", e->name, max);
        return NULL;
    }
    dst = (unsigned char *)malloc((size_t)sz.QuadPart + 1);
    if (!dst || (sz.QuadPart && !me_read_at(h, 0, dst, (size_t)sz.QuadPart))) {
        free(dst);
        CloseHandle(h);
        me_cause(cause, cap, "cannot read %s", e->name);
        return NULL;
    }
    CloseHandle(h);
    dst[sz.QuadPart] = 0;
    if (out_len) *out_len = (size_t)sz.QuadPart;
    return dst;
}

/* One entry's bytes (malloc'd, NUL-terminated), at most max bytes. */
static unsigned char *me_read_entry(const me_reader *r, long idx, size_t max, size_t *out_len, char *cause, size_t cap)
{
    const me_rent *e;
    unsigned char lh[30], *src = NULL, *dst = NULL;
    unsigned long long data;
    if (out_len) *out_len = 0;
    if (idx < 0 || (size_t)idx >= r->n) { me_cause(cause, cap, "no such file in the export"); return NULL; }
    e = &r->e[idx];
    if (e->method == ME_METHOD_FILE) return me_read_dir_entry(r, e, max, out_len, cause, cap);
    if (e->flags & 1) { me_cause(cause, cap, "%s is encrypted", e->name); return NULL; }
    if (e->method != 0 && e->method != 8) { me_cause(cause, cap, "%s uses an unsupported compression", e->name); return NULL; }
    if (e->usize > max) { me_cause(cause, cap, "%s is larger than %zu bytes", e->name, max); return NULL; }
    if (e->method == 0 && e->csize != e->usize) { me_cause(cause, cap, "%s is damaged", e->name); return NULL; }
    if (!me_read_at(r->h, e->lho, lh, sizeof lh) || me_get32(lh) != 0x04034b50u) { me_cause(cause, cap, "%s is damaged", e->name); return NULL; }
    data = (unsigned long long)e->lho + 30 + me_get16(lh + 26) + me_get16(lh + 28);
    if (data + e->csize > r->size) { me_cause(cause, cap, "%s is damaged", e->name); return NULL; }
    dst = (unsigned char *)malloc((size_t)e->usize + 1);
    if (!dst) { me_cause(cause, cap, "out of memory"); return NULL; }
    if (e->method == 0) {
        if (!me_read_at(r->h, data, dst, e->usize)) { free(dst); me_cause(cause, cap, "cannot read %s", e->name); return NULL; }
    } else {
        src = (unsigned char *)malloc(e->csize ? e->csize : 1);
        if (!src || !me_read_at(r->h, data, src, e->csize) || !sh_inflate_raw_exact(src, e->csize, dst, e->usize)) {
            free(src);
            free(dst);
            me_cause(cause, cap, "%s does not decompress", e->name);
            return NULL;
        }
        free(src);
    }
    if (me_crc32(dst, e->usize) != e->crc) { free(dst); me_cause(cause, cap, "%s fails its checksum", e->name); return NULL; }
    dst[e->usize] = 0;
    if (out_len) *out_len = e->usize;
    return dst;
}

/* ------------------------------------------------------------ helpers --- */

#define ME_MODJSON_MAX   (16u * 1024u * 1024u)
#define ME_MAX_MODS      256u

static void me_join_native(char *out, size_t cap, const char *dir, const char *rel)
{
    size_t i, n;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, rel);
    n = strlen(out);
    for (i = strlen(dir); i < n; i++) if (out[i] == '/') out[i] = '\\';
}

/* The entry a map's export names its map: the ZIP's own base name with .json
 * ("my map.zip" carries "my map.json"). */
static void me_map_entry(const char *zip_path, char *out, size_t cap)
{
    const char *base = zip_path, *p;
    size_t n;
    for (p = zip_path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    n = strlen(base);
    if (n > 4 && !_stricmp(base + n - 4, ".zip")) n -= 4;
    if (!n || n > 200) { strcpy_s(out, cap, MODEXPORT_MAP_FALLBACK); return; }
    _snprintf_s(out, cap, _TRUNCATE, "%.*s.json", (int)n, base);
    if (!me_name_ok(out, strlen(out))) strcpy_s(out, cap, MODEXPORT_MAP_FALLBACK);
}

/* ------------------------------------------------------------ write ----- */

static int mm_list_has(const sh_modmanifest_list *list, const char *bundle)
{
    size_t i;
    for (i = 0; i < list->count; i++) if (!strcmp(list->rows[i].bundle, bundle)) return 1;
    return 0;
}

/* What one export carries besides its mods: a map (whose rows are read from
 * its text and rewritten when a row pins an earlier build) or a prefab (whose
 * rows are given, and whose text is asked for once the carried rows are
 * known). */
typedef struct me_payload {
    const char *entry;                 /* "<name>.json" */
    const char *text;
    size_t len;
    const sh_modmanifest_list *rows;   /* prefab: the rows; map: NULL (read from text) */
    modexport_prefab_fn text_for;      /* prefab: the text to carry for the carried rows */
    void *ctx;
} me_payload;

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap);

int modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *stp,
                    char *cause, size_t cap)
{
    me_payload pl;
    char entry[ME_NAME_MAX];
    memset(&pl, 0, sizeof pl);
    me_map_entry(zip_path ? zip_path : "", entry, sizeof entry);
    pl.entry = entry;
    pl.text = map_json;
    pl.len = len;
    return me_write(&pl, zip_path, stp, cause, cap);
}

int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *stp,
                           char *cause, size_t cap)
{
    me_payload pl;
    sh_modmanifest_list none;
    char entry[ME_NAME_MAX];
    size_t n;
    memset(&pl, 0, sizeof pl);
    memset(&none, 0, sizeof none);
    if (stp) memset(stp, 0, sizeof *stp);
    if (!name || !name[0]) return me_cause(cause, cap, "the prefab has no name");
    n = strlen(name);
    if (n > 200 || strchr(name, '/') || strchr(name, '\\') || !me_name_ok(name, n))
        return me_cause(cause, cap, "the prefab's name cannot name a file in the ZIP");
    if (len > MODEXPORT_PREFAB_MAX) return me_cause(cause, cap, "the prefab is larger than %u bytes", MODEXPORT_PREFAB_MAX);
    _snprintf_s(entry, sizeof entry, _TRUNCATE, "%s.json", name);
    pl.entry = entry;
    pl.text = prefab;
    pl.len = len;
    pl.rows = rows ? rows : &none;
    pl.text_for = text_for;
    pl.ctx = ctx;
    return me_write(&pl, zip_path, stp, cause, cap);
}

/* One installed mod as its store folder: mods/<name>/mod.json as it is on
 * disk, then every file its bundle carries under assets/, each checked
 * against the hash mod.json records. A stock pointer is an entry of mod.json
 * and has no file here: installed game content never leaves the machine. */
static int me_add_folder(me_writer *w, const sh_ms_entry *e, modexport_stats *st, char *cause, size_t cap)
{
    const sh_mb_bundle *b = &e->bundle;
    char name[ME_NAME_MAX + 1], path[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1];
    unsigned char *bytes;
    size_t blen = 0, k;
    sh_mb_bundle check;
    int same;
    me_join_native(path, sizeof path, e->folder, "mod.json");
    bytes = sh_mb_read_file(path, ME_MODJSON_MAX, &blen);
    if (!bytes) return me_cause(cause, cap, "mods\\%s\\mod.json cannot be read", e->name);
    memset(&check, 0, sizeof check);
    same = sh_mb_load_text((const char *)bytes, blen, &check) && !strcmp(check.hash, b->hash);
    sh_mb_free(&check);
    if (!same) {
        free(bytes);
        return me_cause(cause, cap, "mods\\%s\\mod.json changed since the store was read; rescan and export again", e->name);
    }
    _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/mod.json", e->name);
    me_add(w, name, bytes, blen);
    free(bytes);
    for (k = 0; k < b->file_count && !w->failed; k++) {
        const sh_mb_file *f = &b->files[k];
        if (f->stock) continue;
        me_join_native(path, sizeof path, e->folder, f->path);
        bytes = sh_mb_read_file(path, (size_t)SH_MB_MAX_FILE, &blen);
        if (bytes) {
            sh_mb_sha256_hex(bytes, blen, actual);
            if (blen != f->size || strcmp(actual, f->blob) != 0) { free(bytes); bytes = NULL; }
        }
        if (!bytes) return me_cause(cause, cap, "%s %s is missing or damaged (run sh_mods doctor verify)", e->name, f->path);
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/%s", e->name, f->path);
        me_add(w, name, bytes, blen);
        free(bytes);
        st->files++;
    }
    if (w->failed) return me_cause(cause, cap, "%s", w->cause);
    return 1;
}

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap)
{
    const char *map_json = pl->text;
    size_t len = pl->len;
    const int prefab = pl->rows != NULL;
    char *prefab_text = NULL;
    size_t moved_rows = 0;
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_modmanifest_list rows;
    sh_ms_list l;
    const sh_ms_entry **picked = NULL;
    me_writer w;
    wchar_t *wfinal = NULL, *wtmp = NULL;
    char tmp[MAX_PATH * 4];
    unsigned rejected = 0;
    size_t i, k;
    double t0 = me_now_ms();
    int ok = 0, listed = 0, locked = 0;
    char *moved_map = NULL;

    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    memset(&w, 0, sizeof w);
    w.h = INVALID_HANDLE_VALUE;
    if (cause && cap) cause[0] = 0;
    if (!map_json || !len || len > MODEXPORT_MAP_MAX || !zip_path || !zip_path[0])
        return me_cause(cause, cap, prefab ? "no prefab text or no destination" : "no map text or no destination");
    if (prefab) {
        for (i = 0; i < pl->rows->count; i++)
            if (!mm_list_has(&rows, pl->rows->rows[i].bundle) && !sh_modmanifest_list_add(&rows, &pl->rows->rows[i])) {
                sh_modmanifest_list_free(&rows);
                return me_cause(cause, cap, "out of memory");
            }
    } else sh_modmanifest_read(map_json, len, &rows, &rejected);
    st->bundles = (unsigned)rows.count;
    st->map_bytes = len;

    modstore_lock();
    locked = 1;
    listed = sh_ms_enumerate(&l);
    if (!listed) { me_cause(cause, cap, "the mods store cannot be listed"); goto done; }
    picked = (const sh_ms_entry **)calloc(rows.count ? rows.count : 1, sizeof *picked);
    if (!picked) { me_cause(cause, cap, "out of memory"); goto done; }
    for (i = 0; i < rows.count; i++) {
        picked[i] = sh_ms_resolve(&l, rows.rows[i].bundle);
        if (picked[i] && !sh_ms_entry_usable(picked[i])) picked[i] = NULL;
        if (!picked[i]) {
            me_cause(cause, cap, "the %s needs %s@%s (bundle %.16s), which is not installed or failed verification",
                     prefab ? "prefab" : "map", rows.rows[i].id, rows.rows[i].version, rows.rows[i].bundle);
            goto done;
        }
    }
    {
        /* A row may pin an earlier build (an alias): the export carries the
         * folder serving it now, and the exported rows name that build, so the
         * importer needs no alias. */
        sh_modmanifest_list fixed;
        size_t moved = 0, map_len = 0;
        char err[160] = "";
        for (i = 0; i < rows.count; i++) if (strcmp(picked[i]->bundle.hash, rows.rows[i].bundle)) moved++;
        if (moved) {
            memset(&fixed, 0, sizeof fixed);
            for (i = 0; i < rows.count; i++) {
                const sh_mb_bundle *b = &picked[i]->bundle;
                sh_modmanifest_row *r = NULL;
                for (k = 0; k < fixed.count && !r; k++) if (!strcmp(fixed.rows[k].bundle, b->hash)) r = &fixed.rows[k];
                if (r) {
                    for (k = 0; k < rows.rows[i].name_count; k++)
                        if (!sh_modmanifest_row_add_name(r, rows.rows[i].names[k])) { r = NULL; break; }
                    if (!r) { sh_modmanifest_list_free(&fixed); me_cause(cause, cap, "out of memory"); goto done; }
                    continue;
                }
                if (!sh_modmanifest_list_add(&fixed, &rows.rows[i])) {
                    sh_modmanifest_list_free(&fixed);
                    me_cause(cause, cap, "out of memory");
                    goto done;
                }
                r = &fixed.rows[fixed.count - 1];
                strcpy_s(r->bundle, sizeof r->bundle, b->hash);
                strncpy_s(r->id, sizeof r->id, b->id, _TRUNCATE);
                strncpy_s(r->version, sizeof r->version, b->version, _TRUNCATE);
                r->size = b->total_size;
            }
            if (prefab) {
                /* A prefab's rows live in its block, written below from the rows carried. */
                moved_rows = moved;
                me_log("MODS: export: %zu row(s) pinned an earlier build; the export carries and names the current one(s)", moved);
                sh_modmanifest_list_free(&rows);
                rows = fixed;
                for (i = 0; i < rows.count; i++) picked[i] = sh_ms_resolve(&l, rows.rows[i].bundle);
                st->bundles = (unsigned)rows.count;
                goto rows_done;
            }
            moved_map = sh_modmanifest_write(map_json, len, &fixed, &map_len, err, sizeof err);
            if (!moved_map) {
                sh_modmanifest_list_free(&fixed);
                me_cause(cause, cap, "the map's rows could not be moved to the current builds: %s", err[0] ? err : "unknown");
                goto done;
            }
            me_log("MODS: export: %zu row(s) pinned an earlier build; the export carries and names the current one(s)", moved);
            map_json = moved_map;
            len = map_len;
            sh_modmanifest_list_free(&rows);
            rows = fixed;
            for (i = 0; i < rows.count; i++) picked[i] = sh_ms_resolve(&l, rows.rows[i].bundle);
            st->bundles = (unsigned)rows.count;
            st->map_bytes = len;
        }
    }
rows_done:
    if (prefab && pl->text_for) {
        prefab_text = pl->text_for(pl->ctx, &rows, moved_rows > 0, &len);
        if (!prefab_text || !len || len > MODEXPORT_PREFAB_MAX) { me_cause(cause, cap, "the prefab's mods could not be written into it"); goto done; }
        map_json = prefab_text;
        st->map_bytes = len;
    }

    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.smp-%lu.tmp", zip_path, GetCurrentProcessId());
    wfinal = sh_mb_wpath(zip_path);
    wtmp = sh_mb_wpath(tmp);
    if (!wfinal || !wtmp) { me_cause(cause, cap, "the destination path is not usable"); goto done; }
    w.h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w.h == INVALID_HANDLE_VALUE) { me_cause(cause, cap, "cannot create %s (error %lu)", zip_path, GetLastError()); goto done; }

    me_add(&w, pl->entry, map_json, len);
    for (i = 0; i < rows.count && !w.failed; i++) {
        int dup = 0;
        for (k = 0; k < i && !dup; k++) dup = picked[k] == picked[i];
        if (dup) continue;
        if (!me_add_folder(&w, picked[i], st, cause, cap)) goto done;
    }
    me_finish(&w);
    if (w.failed) { me_cause(cause, cap, "%s", w.cause); goto done; }
    if (!FlushFileBuffers(w.h)) { me_cause(cause, cap, "writing the ZIP failed (error %lu)", GetLastError()); goto done; }
    CloseHandle(w.h);
    w.h = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        me_cause(cause, cap, "cannot replace %s (error %lu)", zip_path, GetLastError());
        goto done;
    }
    st->entries = (unsigned)w.n;
    st->zip_bytes = w.off;
    ok = 1;
done:
    if (w.h != INVALID_HANDLE_VALUE) CloseHandle(w.h);
    if (!ok && wtmp) DeleteFileW(wtmp);
    me_writer_free(&w);
    free(picked);
    free(moved_map);
    free(prefab_text);
    free(wfinal);
    free(wtmp);
    if (listed) sh_ms_list_free(&l);
    if (locked) modstore_unlock();
    sh_modmanifest_list_free(&rows);
    st->ms = me_now_ms() - t0;
    return ok;
}

/* ------------------------------------------------------------ read ------ */

enum { ME_KIND_MAP = 1, ME_KIND_PREFAB = 2 };

static sh_mb_exists_fn g_stock_fn;
static void *g_stock_ctx;

void modexport_set_stock(sh_mb_exists_fn stock, void *ctx)
{
    g_stock_ctx = ctx;
    g_stock_fn = stock;
}

/* One mod folder an export carries that a row names. */
typedef struct me_carry {
    char name[SH_MS_NAME_MAX + 1];      /* the folder's name under mods/ */
    sh_mb_bundle b;                     /* its mod.json, loaded: the bundle hash it records */
    int present;                        /* the store serves that bundle already */
    int installed;                      /* this import installed it, as `stored` */
    char stored[SH_MS_KEY_MAX + 1];
    char stage[MAX_PATH * 4];           /* its copy outside the store, which the install builds */
    char why[SH_MB_CAUSE_MAX];          /* the last install refusal */
} me_carry;

typedef struct me_import {
    me_reader zr;
    me_carry *c;
    size_t nc, cap;
    int kind;                           /* what the export holds */
} me_import;

static void me_import_free(me_import *im)
{
    size_t i;
    for (i = 0; i < im->nc; i++) sh_mb_free(&im->c[i].b);
    free(im->c);
    me_reader_close(&im->zr);
    memset(im, 0, sizeof *im);
}

/* The entry is a top-level .json (no folder). */
static int me_top_json(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && !strchr(name, '/') && !_stricmp(name + n - 5, ".json");
}

static int me_reader_add(me_reader *r, const char *name, unsigned long long size)
{
    me_rent *e;
    if (r->n >= ME_MAX_ENTRIES || size > ME_LIMIT) return 0;
    if (r->n == r->cap) {
        size_t c2 = r->cap ? r->cap * 2 : 64;
        me_rent *g = (me_rent *)realloc(r->e, sizeof *g * c2);
        if (!g) return 0;
        r->e = g;
        r->cap = c2;
    }
    e = &r->e[r->n];
    memset(e, 0, sizeof *e);
    e->name = _strdup(name);
    if (!e->name) return 0;
    e->method = ME_METHOD_FILE;
    e->usize = (uint32_t)size;
    r->n++;
    return 1;
}

static int me_w_to_utf8(const wchar_t *w, char *out, size_t cap)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

/* Add every file under abs (the folder rel names) as an entry. Links and
 * junctions are not followed. */
static int me_walk(me_reader *r, const wchar_t *abs, const char *rel, int depth, char *cause, size_t cap)
{
    wchar_t pattern[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 32) return me_cause(cause, cap, "%s is nested too deeply", rel);
    _snwprintf_s(pattern, MAX_PATH * 2, _TRUNCATE, L"%s\\*", abs);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        wchar_t child[MAX_PATH * 2];
        char leaf[MAX_PATH * 3], name[ME_NAME_MAX + 1];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!me_w_to_utf8(fd.cFileName, leaf, sizeof leaf)) continue;
        _snprintf_s(name, sizeof name, _TRUNCATE, "%s/%s", rel, leaf);
        if (!me_name_ok(name, strlen(name))) continue;
        _snwprintf_s(child, MAX_PATH * 2, _TRUNCATE, L"%s\\%s", abs, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!me_walk(r, child, name, depth + 1, cause, cap)) { FindClose(h); return 0; }
        } else if (!me_reader_add(r, name, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) {
            FindClose(h);
            return me_cause(cause, cap, "the export folder holds too many files");
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* The retired bundle form of earlier releases: a bundle.canonical.json
 * anywhere, or an export.json that is an index rather than a prefab or map
 * named "export". */
static int me_retired(const me_reader *r)
{
    static const char LEAF[] = "bundle.canonical.json";
    const size_t ll = sizeof LEAF - 1;
    size_t i;
    long idx;
    for (i = 0; i < r->n; i++) {
        const char *name = r->e[i].name;
        size_t n = strlen(name);
        if (n >= ll && !_stricmp(name + n - ll, LEAF) && (n == ll || name[n - ll - 1] == '/')) return 1;
    }
    idx = me_find(r, MODEXPORT_INDEX);
    if (idx >= 0) {
        char ignored[64], type[64];
        size_t len = 0;
        unsigned char *text = me_read_entry(r, idx, ME_INDEX_MAX, &len, ignored, sizeof ignored);
        int t = text ? sh_prefab_block_top_type((const char *)text, len, type, sizeof type) : -1;
        free(text);
        if (t != 1) return 1;
    }
    return 0;
}

/* What a carried .json is: a prefab (an idSnapEntityPrefab, or an idSnapMap
 * with a block), a map (an idSnapMap or an object without ~type, without a
 * block), 0 not a JSON object, -1 an object of another type. */
static int me_classify(const char *text, size_t len)
{
    sh_prefab_block_span span;
    char type[64];
    int t = sh_prefab_block_top_type(text, len, type, sizeof type);
    if (t < 0) return 0;
    if (t == 1 && strcmp(type, "idSnapEntityPrefab") && strcmp(type, "idSnapMap")) return -1;
    if (t == 1 && !strcmp(type, "idSnapEntityPrefab")) return ME_KIND_PREFAB;
    return sh_prefab_block_find(text, len, &span) == 1 ? ME_KIND_PREFAB : ME_KIND_MAP;
}

static const char *me_kind_word(int kind) { return kind == ME_KIND_PREFAB ? "prefab" : "map"; }

static void me_open_fail(me_import *im, char **text)
{
    free(*text);
    *text = NULL;
    me_import_free(im);
}

/* Open an export for `want` and read its .json: a ZIP, or (a prefab only)
 * the folder of a picked .json, whose mods/ folder is walked into entries as
 * the ZIP's would be. *text (malloc'd) and *idx are the carried .json. The
 * .json is read and checked before anything is installed: a damaged or
 * mistaken export installs nothing. */
static int me_open(me_import *im, const char *path, int want, char **text, size_t *len, long *idx, char *cause, size_t cap)
{
    size_t n = path ? strlen(path) : 0, i;
    int kind, dir = 0;
    memset(im, 0, sizeof *im);
    *text = NULL;
    *len = 0;
    *idx = -1;
    if (want == ME_KIND_PREFAB && n > 5 && !_stricmp(path + n - 5, ".json")) {
        wchar_t *w = sh_mb_wpath(path), *slash;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        char leaf[MAX_PATH * 3];
        if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            free(w);
            return me_cause(cause, cap, "cannot open the prefab file");
        }
        slash = wcsrchr(w, L'\\');
        if (!slash) slash = wcsrchr(w, L'/');
        if (!slash || !me_w_to_utf8(slash + 1, leaf, sizeof leaf) || !me_name_ok(leaf, strlen(leaf))) {
            free(w);
            return me_cause(cause, cap, "the prefab file's path is not usable");
        }
        *slash = 0;
        im->zr.dir = w;
        im->zr.h = NULL;
        if (!me_reader_add(&im->zr, leaf, ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow)) {
            me_import_free(im);
            return me_cause(cause, cap, "out of memory");
        }
        *idx = 0;
        dir = 1;
    } else {
        if (!me_reader_open(&im->zr, path, cause, cap)) return 0;
        if (me_retired(&im->zr)) {
            me_import_free(im);
            return me_cause(cause, cap, "%s", want == ME_KIND_PREFAB ? MODEXPORT_RETIRED_PREFAB : MODEXPORT_RETIRED_MAP);
        }
        for (i = 0; i < im->zr.n; i++) {
            if (!me_top_json(im->zr.e[i].name)) continue;
            if (*idx >= 0) {
                me_import_free(im);
                return me_cause(cause, cap, "the export holds more than one %s", me_kind_word(want));
            }
            *idx = (long)i;
        }
        if (*idx < 0) { me_import_free(im); return me_cause(cause, cap, "the export holds no %s", me_kind_word(want)); }
    }
    *text = (char *)me_read_entry(&im->zr, *idx, MODEXPORT_MAP_MAX, len, cause, cap);
    if (!*text) { me_import_free(im); return 0; }
    kind = me_classify(*text, *len);
    if (kind != want) {
        const char *file = im->zr.e[*idx].name;
        if (kind == 0)
            me_cause(cause, cap, want == ME_KIND_PREFAB ? "the export's prefab is not prefab JSON" : "the export's map is not rawmap JSON");
        else if (want == ME_KIND_MAP)
            me_cause(cause, cap, kind == ME_KIND_PREFAB ? "this is a prefab's export, not a map's" : "the export's map is not rawmap JSON");
        else if (kind < 0) me_cause(cause, cap, "%s is not a prefab", file);
        else if (dir) me_cause(cause, cap, "%s is a map, not a prefab", file);
        else me_cause(cause, cap, "this is a map's export, not a prefab's");
        me_open_fail(im, text);
        return 0;
    }
    if (want == ME_KIND_PREFAB && *len > MODEXPORT_PREFAB_MAX) {
        me_open_fail(im, text);
        return me_cause(cause, cap, "the prefab is larger than %u bytes", MODEXPORT_PREFAB_MAX);
    }
    im->kind = want;
    if (dir) {
        /* The extracted export's mods/ folder. Sorting moves the entries, so
         * the picked .json (the only top-level one) is found again. */
        wchar_t mods[MAX_PATH * 2];
        _snwprintf_s(mods, MAX_PATH * 2, _TRUNCATE, L"%s\\mods", im->zr.dir);
        if (!me_walk(&im->zr, mods, "mods", 0, cause, cap) || !me_reader_sort(&im->zr, cause, cap)) {
            me_open_fail(im, text);
            return 0;
        }
        for (i = 0; i < im->zr.n; i++) if (me_top_json(im->zr.e[i].name)) { *idx = (long)i; break; }
        if (me_retired(&im->zr)) {
            me_open_fail(im, text);
            return me_cause(cause, cap, "%s", MODEXPORT_RETIRED_PREFAB);
        }
    }
    return 1;
}

static int me_rows_name(const sh_modmanifest_list *rows, const char *hash)
{
    size_t i;
    for (i = 0; rows && i < rows->count; i++) if (!_stricmp(rows->rows[i].bundle, hash)) return 1;
    return 0;
}

static int me_carried(const me_import *im, const char *hash)
{
    size_t i;
    for (i = 0; i < im->nc; i++) if (!_stricmp(im->c[i].b.hash, hash)) return 1;
    return 0;
}

/* The carried folders, each mods/<name>/mod.json, whose bundle a row names
 * (every one when `all`). A folder no row names is left out. */
static int me_collect(me_import *im, const sh_modmanifest_list *rows, int all, char *cause, size_t cap)
{
    size_t i;
    for (i = 0; i < im->zr.n; i++) {
        const char *name = im->zr.e[i].name, *seg, *slash;
        size_t sl, len = 0;
        char *text;
        me_carry *c;
        sh_mb_bundle b;
        if (strncmp(name, "mods/", 5)) continue;
        seg = name + 5;
        slash = strchr(seg, '/');
        if (!slash || strcmp(slash, "/mod.json")) continue;
        sl = (size_t)(slash - seg);
        if (!sl || sl > SH_MS_NAME_MAX) continue;
        text = (char *)me_read_entry(&im->zr, (long)i, ME_MODJSON_MAX, &len, cause, cap);
        if (!text) return 0;
        memset(&b, 0, sizeof b);
        if (!sh_mb_load_text(text, len, &b)) {
            me_log("MODS: import: %s is not a built mod (%s); left out", name, b.first_cause);
            sh_mb_free(&b);
            free(text);
            continue;
        }
        free(text);
        if ((!all && !me_rows_name(rows, b.hash)) || me_carried(im, b.hash)) {
            me_log("MODS: import: mods/%.*s (%s@%s, bundle %.16s) is carried but the %s does not name it; left out",
                   (int)sl, seg, b.id, b.version, b.hash, me_kind_word(im->kind));
            sh_mb_free(&b);
            continue;
        }
        if (im->nc >= ME_MAX_MODS) { sh_mb_free(&b); return me_cause(cause, cap, "the export carries too many mods"); }
        if (im->nc == im->cap) {
            size_t c2 = im->cap ? im->cap * 2 : 8;
            me_carry *g = (me_carry *)realloc(im->c, sizeof *g * c2);
            if (!g) { sh_mb_free(&b); return me_cause(cause, cap, "out of memory"); }
            im->c = g;
            im->cap = c2;
        }
        c = &im->c[im->nc++];
        memset(c, 0, sizeof *c);
        memcpy(c->name, seg, sl);
        c->name[sl] = 0;
        c->b = b;
    }
    return 1;
}

/* ------------------------------------------------------------ install --- */

static wchar_t *me_wjoin(const char *dir, const char *rel)
{
    char path[MAX_PATH * 4];
    me_join_native(path, sizeof path, dir, rel);
    return sh_mb_wpath(path);
}

static void me_wmkdirs_for(wchar_t *file)
{
    wchar_t *p = file;
    if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    if (p[0] && p[1] == L':') p += 3;
    for (; *p; p++)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(file, NULL); *p = L'\\'; }
}

static int me_put_file(const char *dir, const char *rel, const void *data, size_t n)
{
    wchar_t *w = me_wjoin(dir, rel);
    HANDLE h;
    size_t off = 0;
    int ok = 1;
    if (!w) return 0;
    me_wmkdirs_for(w);
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (ok && off < n) {
        DWORD chunk = (DWORD)((n - off) > (1u << 24) ? (1u << 24) : (n - off)), put = 0;
        ok = WriteFile(h, (const unsigned char *)data + off, chunk, &put, NULL) && put == chunk;
        off += put;
    }
    CloseHandle(h);
    return ok;
}

/* Remove a folder this import made under the temporary directory. */
static void me_rmtree(const wchar_t *dir)
{
    size_t dl = wcslen(dir), cap = dl + 4;
    wchar_t *pattern = (wchar_t *)malloc(sizeof(wchar_t) * cap);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!pattern) return;
    _snwprintf_s(pattern, cap, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t ccap = dl + wcslen(fd.cFileName) + 2;
            wchar_t *child;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            child = (wchar_t *)malloc(sizeof(wchar_t) * ccap);
            if (!child) continue;
            _snwprintf_s(child, ccap, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                me_rmtree(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(child);
            else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* A fresh folder under the temporary directory for this import's copies. */
static int me_stage_root(char *out, size_t cap)
{
    static volatile LONG seq;
    wchar_t tmp[MAX_PATH + 1];
    char base[MAX_PATH * 4];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    wchar_t *w;
    int made;
    size_t bl;
    if (!n || n > MAX_PATH || !me_w_to_utf8(tmp, base, sizeof base)) return 0;
    bl = strlen(base);
    if (bl && (base[bl - 1] == '\\' || base[bl - 1] == '/')) base[bl - 1] = 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus-import-%lu-%ld", base, GetCurrentProcessId(),
                InterlockedIncrement(&seq));
    w = sh_mb_wpath(out);
    if (!w) return 0;
    me_rmtree(w);
    made = CreateDirectoryW(w, NULL) != 0;
    free(w);
    return made;
}

/* Copy one carried folder out of the export (mod.json and assets/), then
 * put back, from this player's game, the bytes of each stock pointer whose
 * file the folder held (the export never carries game content), so the
 * install builds the same pointers. */
static int me_stage(me_import *im, me_carry *c, const char *stage, size_t index, modexport_stats *st, char *cause,
                    size_t cap)
{
    char prefix[SH_MS_NAME_MAX + 8];
    size_t pl, i;
    _snprintf_s(c->stage, sizeof c->stage, _TRUNCATE, "%s\\%zu", stage, index);
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "mods/%s/", c->name);
    pl = strlen(prefix);
    for (i = 0; i < im->zr.n; i++) {
        const char *name = im->zr.e[i].name, *rel;
        unsigned char *bytes;
        size_t len = 0;
        int put;
        if (strncmp(name, prefix, pl)) continue;
        rel = name + pl;
        if (strcmp(rel, "mod.json") && strncmp(rel, "assets/", 7)) {
            me_log("MODS: import: %s is not part of a mod folder; left out", name);
            continue;
        }
        bytes = me_read_entry(&im->zr, (long)i, (size_t)SH_MB_MAX_FILE, &len, cause, cap);
        if (!bytes) return 0;
        put = me_put_file(c->stage, rel, bytes, len);
        free(bytes);
        if (!put) return me_cause(cause, cap, "cannot copy %s out of the export (error %lu)", name, GetLastError());
        st->files++;
    }
    for (i = 0; i < c->b.file_count; i++) {
        const sh_mb_file *f = &c->b.files[i];
        unsigned char *bytes = NULL;
        size_t len = 0;
        wchar_t *w;
        int have, put;
        if (!f->stock || f->absent) continue;
        w = me_wjoin(c->stage, f->path);
        have = w && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
        free(w);
        if (have) continue;
        if (cause && cap) cause[0] = 0;
        if (!f->stock_source || sh_mb_stock_read(f->stock_source, f->blob, &bytes, &len, f->path, cause, cap) != SH_MB_STOCK_OK) {
            if (!cause || !cap || !cause[0]) me_cause(cause, cap, "%s names game content this game does not have", f->path);
            return 0;
        }
        put = me_put_file(c->stage, f->path, bytes, len);
        HeapFree(GetProcessHeap(), 0, bytes);
        if (!put) return me_cause(cause, cap, "cannot copy %s out of the game (error %lu)", f->path, GetLastError());
    }
    return 1;
}

/* A carried folder name the store may keep: one Windows folder name. */
static int me_name_safe(const char *name)
{
    size_t n = strlen(name);
    if (!n || name[n - 1] == '.' || name[n - 1] == ' ' || name[0] == ' ') return 0;
    return strpbrk(name, "<>\"|?*/\\:") == NULL;
}

static int me_folder_taken(const char *root, const char *name)
{
    wchar_t *w = me_wjoin(root, name);
    int taken = !w || GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return taken;
}

/* Remove what this import installed, and only that. */
static void me_rollback(me_import *im, const char *root)
{
    size_t i;
    for (i = 0; i < im->nc; i++) {
        char path[MAX_PATH * 4];
        if (!im->c[i].installed) continue;
        me_join_native(path, sizeof path, root, im->c[i].stored);
        if (sh_ms_remove_folder(path)) me_log("MODS: import: mods\\%s removed again", im->c[i].stored);
        else me_log("MODS: import: mods\\%s could not be removed again; remove it by hand", im->c[i].stored);
        im->c[i].installed = 0;
    }
}

/* The asset rule for a carried folder (modserve.h sh_modserve_row_served):
 * an installed folder declares its id and the serving index resolves every
 * name the rows that name its bundle list (a prefab's block rows, a map's
 * rows). */
static int me_carry_served(const me_carry *c, const sh_modmanifest_list *rows)
{
    sh_modmanifest_row row;
    char **names = NULL;
    size_t i, k, n = 0, total = 0;
    int st;
    memset(&row, 0, sizeof row);
    if (strlen(c->b.id) >= sizeof row.id) return 0;
    strcpy_s(row.id, sizeof row.id, c->b.id);
    for (i = 0; rows && i < rows->count; i++)
        if (!_stricmp(rows->rows[i].bundle, c->b.hash)) total += rows->rows[i].name_count;
    if (total && !(names = (char **)calloc(total, sizeof *names))) return 0;
    for (i = 0; rows && i < rows->count; i++)
        if (!_stricmp(rows->rows[i].bundle, c->b.hash))
            for (k = 0; k < rows->rows[i].name_count; k++) names[n++] = rows->rows[i].names[k];
    row.names = names;
    row.name_count = n;
    st = sh_modserve_row_served(&row, NULL, 0);
    free(names);
    return st == SH_MODSERVE_ROW_SERVED;
}

/* A folder name beside the installed folders of an id: "<id>@<version>",
 * else "<id>@<version>-2", "-3", ... */
static int me_beside_name(const char *root, const char *id, const char *version, char *out, size_t cap)
{
    int k;
    for (k = 1; k < 100; k++) {
        if (k == 1) _snprintf_s(out, cap, _TRUNCATE, "%s@%s", id, version && version[0] ? version : "0.0.0");
        else _snprintf_s(out, cap, _TRUNCATE, "%s@%s-%d", id, version && version[0] ? version : "0.0.0", k);
        if (me_name_safe(out) && !me_folder_taken(root, out)) return 1;
    }
    return 0;
}

/* The ordinary install of each carried folder the store does not already
 * serve. A carried folder whose bundle the store holds (by hash or alias), or
 * whose id an installed folder declares and whose listed names that folder
 * provides (the asset rule, whatever the bytes), is skipped and counted as
 * already installed. Every other one is copied out of the export and
 * installed as a folder from elsewhere is (sh_ms_install_as: built, copied
 * in, verified), under its own name when no folder of its id is installed and
 * that name is free, else beside the installed folders of its id under
 * "<id>@<version>" (a free variant when that is taken). The built bundle must
 * be the one its own mod.json names. A refusal removes what the import
 * installed; nothing that was installed before is ever written. */
static int me_install(me_import *im, const sh_modmanifest_list *rows, modexport_stats *st, char *cause, size_t cap)
{
    sh_ms_list l;
    sh_mb_resolver r;
    char root[MAX_PATH * 4], stage[MAX_PATH * 4] = "";
    size_t i, k, left = 0;
    int ok = 0, listed = 0, progress, staged = 0;
    const char *what = me_kind_word(im->kind);

    st->bundles = (unsigned)im->nc;
    st->entries = (unsigned)im->zr.n;
    st->zip_bytes = im->zr.size;
    if (!im->nc) return 1;
    modstore_lock();
    if (!sh_ms_root(root, sizeof root)) { me_cause(cause, cap, "the mods folder cannot be resolved"); goto done; }
    listed = sh_ms_enumerate(&l);
    if (!listed) { me_cause(cause, cap, "the mods folder cannot be listed"); goto done; }
    for (i = 0; i < im->nc; i++) {
        me_carry *c = &im->c[i];
        const sh_ms_entry *e = sh_ms_resolve(&l, c->b.hash);
        if ((e && sh_ms_entry_usable(e)) || me_carry_served(c, rows)) {
            c->present = 1;
            st->bundles_present++;
            me_log("MODS: import: mods/%s skipped: an installed folder of %s provides what the %s uses", c->name, c->b.id, what);
            continue;
        }
        if (e) {
            me_cause(cause, cap, "the installed %s %s needs attention (%s); run sh_mods doctor verify", c->b.id, c->b.version,
                     e->cause[0] ? e->cause : "it does not verify");
            goto done;
        }
        left++;
    }
    sh_ms_list_free(&l);
    listed = 0;
    if (!left) { ok = 1; goto done; }

    if (!me_stage_root(stage, sizeof stage)) { me_cause(cause, cap, "no temporary folder could be made for the export's mods"); goto done; }
    staged = 1;
    for (i = 0; i < im->nc; i++)
        if (!im->c[i].present && !me_stage(im, &im->c[i], stage, i, st, cause, cap)) goto done;

    /* A mod that needs another carried one builds once that one is installed. */
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = g_stock_fn;
    r.stock_context = g_stock_ctx;
    do {
        progress = 0;
        for (i = 0; i < im->nc && left; i++) {
            me_carry *c = &im->c[i];
            sh_ms_install_stats is;
            sh_mb_bundle built;
            const char *as = NULL;
            char beside[SH_MS_NAME_MAX + 1];
            int same_id = 0;
            if (c->present || c->installed) continue;
            if (sh_ms_enumerate(&l)) {
                for (k = 0; k < l.count && !same_id; k++) same_id = l.items[k].loaded && !strcmp(l.items[k].bundle.id, c->b.id);
                sh_ms_list_free(&l);
            }
            if (!same_id && me_name_safe(c->name) && !me_folder_taken(root, c->name)) as = c->name;
            /* Beside an installed folder of the id, never over it (the store's
             * own name for the same id and version would replace it). */
            else if (same_id) {
                if (!me_beside_name(root, c->b.id, c->b.version, beside, sizeof beside)) {
                    me_cause(cause, cap, "no free folder name beside the installed %s", c->b.id);
                    goto rollback;
                }
                as = beside;
            }
            if (!sh_ms_install_as(c->stage, as, &r, c->stored, sizeof c->stored, &is, &built, c->why, sizeof c->why)) {
                sh_mb_free(&built);
                continue;
            }
            c->installed = 1;
            left--;
            progress = 1;
            if (strcmp(built.hash, c->b.hash) != 0 || !built.validated) {
                me_log("MODS: import: mods/%s built to bundle %.16s, not %.16s as its mod.json records", c->name, built.hash,
                       c->b.hash);
                me_cause(cause, cap, "%s %s in the export does not build to the bundle the %s names", c->b.id, c->b.version, what);
                sh_mb_free(&built);
                goto rollback;
            }
            me_log("MODS: import: mods/%s installed as mods\\%s (%s@%s+%llu, bundle %.16s)%s", c->name, c->stored, built.id,
                   built.version, built.build, built.hash, same_id ? ", beside the installed folder of that id" : "");
            sh_mb_free(&built);
            st->bundles_installed++;
        }
    } while (progress && left);
    if (left) {
        for (i = 0; i < im->nc; i++) {
            const me_carry *c = &im->c[i];
            if (c->present || c->installed) continue;
            me_cause(cause, cap, "%s %s from the export does not install: %s", c->b.id, c->b.version, c->why);
            break;
        }
        goto rollback;
    }
    ok = 1;
    if (st->bundles_installed && sh_modserve_index_rescan() < 0)
        me_log("MODS: import: mods installed, but the serving index could not be rebuilt");
    goto done;
rollback:
    me_rollback(im, root);
    st->bundles_installed = 0;
done:
    if (listed) sh_ms_list_free(&l);
    modstore_unlock();
    if (staged) {
        wchar_t *w = sh_mb_wpath(stage);
        if (w) { me_rmtree(w); free(w); }
    }
    return ok;
}

/* Rows that name a bundle the export does not carry and the store does not
 * serve (by its hash or an alias of it). */
static void me_count_absent(const me_import *im, const sh_modmanifest_list *rows, modexport_stats *st)
{
    sh_ms_list l;
    size_t i;
    int listed;
    modstore_lock();
    listed = sh_ms_enumerate(&l);
    for (i = 0; i < rows->count; i++) {
        const sh_ms_entry *e;
        if (me_carried(im, rows->rows[i].bundle)) continue;
        e = listed ? sh_ms_resolve(&l, rows->rows[i].bundle) : NULL;
        if (e && sh_ms_entry_usable(e)) continue;
        if (sh_modserve_row_served(&rows->rows[i], NULL, 0) == SH_MODSERVE_ROW_SERVED) continue;
        st->bundles_absent++;
        me_log("MODS: import: the %s names %s@%s (bundle %.16s), which the export does not carry",
               me_kind_word(im->kind), rows->rows[i].id, rows->rows[i].version, rows->rows[i].bundle);
    }
    if (listed) sh_ms_list_free(&l);
    modstore_unlock();
}

int modexport_probe(const char *zip_path, char *cause, size_t cap)
{
    me_import im;
    char *text = NULL;
    size_t len = 0;
    long idx;
    if (cause && cap) cause[0] = 0;
    if (!me_open(&im, zip_path, ME_KIND_MAP, &text, &len, &idx, cause, cap)) return 0;
    free(text);
    me_import_free(&im);
    return 1;
}

char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    sh_modmanifest_list rows;
    char *text = NULL;
    size_t len = 0;
    unsigned rejected = 0;
    long idx;
    double t0 = me_now_ms();
    if (out_len) *out_len = 0;
    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    if (cause && cap) cause[0] = 0;
    if (!me_open(&im, zip_path, ME_KIND_MAP, &text, &len, &idx, cause, cap)) { st->ms = me_now_ms() - t0; return NULL; }
    sh_modmanifest_read(text, len, &rows, &rejected);
    if (!me_collect(&im, &rows, 0, cause, cap) || !me_install(&im, &rows, st, cause, cap)) {
        free(text);
        text = NULL;
        len = 0;
    } else {
        me_count_absent(&im, &rows, st);
        st->map_bytes = len;
    }
    sh_modmanifest_list_free(&rows);
    me_import_free(&im);
    st->ms = me_now_ms() - t0;
    if (out_len) *out_len = len;
    return text;
}

int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char *name, size_t name_cap, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    sh_modmanifest_list rows;
    char *text = NULL;
    size_t len = 0;
    long idx = -1;
    int ok = 0;
    double t0 = me_now_ms();
    memset(&rows, 0, sizeof rows);
    if (prefab) *prefab = NULL;
    if (prefab_len) *prefab_len = 0;
    if (name && name_cap) name[0] = 0;
    memset(st, 0, sizeof *st);
    if (cause && cap) cause[0] = 0;
    if (!prefab || !path || !path[0]) return me_cause(cause, cap, "nothing to read");
    if (!me_open(&im, path, ME_KIND_PREFAB, &text, &len, &idx, cause, cap)) { st->ms = me_now_ms() - t0; return 0; }
    if (name && name_cap) {
        size_t nl = strlen(im.zr.e[idx].name) - 5;
        if (nl >= name_cap) nl = name_cap - 1;
        memcpy(name, im.zr.e[idx].name, nl);
        name[nl] = 0;
    }
    if (rows_of && rows_of(ctx, text, len, &rows) < 0) {
        me_cause(cause, cap, "the prefab's mods cannot be read");
        goto done;
    }
    if (!me_collect(&im, &rows, rows_of == NULL, cause, cap) || !me_install(&im, &rows, st, cause, cap)) goto done;
    me_count_absent(&im, &rows, st);
    st->map_bytes = len;
    ok = 1;
done:
    me_import_free(&im);
    sh_modmanifest_list_free(&rows);
    if (ok) {
        *prefab = text;
        if (prefab_len) *prefab_len = len;
    } else {
        free(text);
        if (name && name_cap) name[0] = 0;
    }
    st->ms = me_now_ms() - t0;
    return ok;
}

/* ------------------------------------------------------------ rawmap ---- */

static int me_acp_to_utf8(const char *acp, char *out, size_t cap)
{
    wchar_t wide[MAX_PATH * 2];
    if (!acp || !acp[0] || !MultiByteToWideChar(CP_ACP, 0, acp, -1, wide, MAX_PATH * 2)) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

unsigned long long modexport_write_text(const char *data, size_t len, const char *path_acp)
{
    char cause[320];
    return modexport_write_text_report(data, len, path_acp, NULL, cause, sizeof cause);
}

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path_acp,
                                               unsigned *bundles, char *cause_out, size_t cause_cap)
{
    modexport_stats st;
    char path[MAX_PATH * 4], cause[320];
    if (bundles) *bundles = 0;
    if (cause_out && cause_cap) cause_out[0] = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) {
        me_log("MODS: export: the destination path is not usable");
        if (cause_out && cause_cap) strncpy_s(cause_out, cause_cap, "the destination path is not usable", _TRUNCATE);
        return 0;
    }
    if (!modexport_write(data, len, path, &st, cause, sizeof cause)) {
        me_log("MODS: export %s: refused: %s", path, cause);
        if (bundles) *bundles = st.bundles;
        if (cause_out && cause_cap) strncpy_s(cause_out, cause_cap, cause, _TRUNCATE);
        return 0;
    }
    if (bundles) *bundles = st.bundles;
    me_log("MODS: export %s: map %llu byte(s), %u mod folder(s), %u file(s) carried, %u entr%s, %llu byte(s) in %.0f ms",
           path, st.map_bytes, st.bundles, st.files, st.entries, st.entries == 1 ? "y" : "ies", st.zip_bytes, st.ms);
    return st.zip_bytes;
}

char *modexport_read_text(const char *path_acp, size_t *out_len)
{
    modexport_stats st;
    char path[MAX_PATH * 4], cause[320];
    char *text, *heap;
    size_t len = 0;
    if (out_len) *out_len = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) { me_log("MODS: import: the source path is not usable"); return NULL; }
    text = modexport_read(path, &len, &st, cause, sizeof cause);
    if (!text) { me_log("MODS: import %s: refused: %s", path, cause); return NULL; }
    me_log("MODS: import %s: %u mod folder(s) (%u installed, %u already installed), %u file(s) copied out, map %llu byte(s), "
           "%llu byte(s) read in %.0f ms%s", path, st.bundles, st.bundles_installed, st.bundles_present, st.files,
           st.map_bytes, st.zip_bytes, st.ms,
           st.bundles_absent ? "; the map also names mods the export does not carry" : "");
    if (!len || len > (size_t)INT_MAX) { free(text); return NULL; }
    heap = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (heap) { memcpy(heap, text, len); heap[len] = 0; if (out_len) *out_len = len; }
    free(text);
    return heap;
}

int modexport_validate_source(const char *path_acp, char *out_msg, int msg_capacity)
{
    char path[MAX_PATH * 4], cause[320];
    int ok;
    if (out_msg && msg_capacity > 0) out_msg[0] = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, "no path given", _TRUNCATE);
        return 0;
    }
    ok = modexport_probe(path, cause, sizeof cause);
    if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, ok ? "ok" : cause, _TRUNCATE);
    return ok;
}
