/* modexport.c -- see modexport.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>

#include "modexport.h"
#include "modsync.h"
#include "modstore.h"
#include "modbundle.h"
#include "modmanifest.h"
#include "modserve.h"
#include "mp_json.h"
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
    size_t n;
    size_t *sorted;     /* indexes sorted by name */
} me_reader;

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

/* One entry's bytes (malloc'd, NUL-terminated), at most max bytes. */
static unsigned char *me_read_entry(const me_reader *r, long idx, size_t max, size_t *out_len, char *cause, size_t cap)
{
    const me_rent *e;
    unsigned char lh[30], *src = NULL, *dst = NULL;
    unsigned long long data;
    if (out_len) *out_len = 0;
    if (idx < 0 || (size_t)idx >= r->n) { me_cause(cause, cap, "no such file in the export"); return NULL; }
    e = &r->e[idx];
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

typedef struct me_buf { char *p; size_t n, cap; int bad; } me_buf;

static void me_put(me_buf *b, const char *s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 512;
        char *g;
        while (want < b->n + n + 1) want *= 2;
        g = (char *)realloc(b->p, want);
        if (!g) { b->bad = 1; return; }
        b->p = g;
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void me_puts(me_buf *b, const char *s) { me_put(b, s, strlen(s)); }
static void me_put_str(me_buf *b, const char *s)
{
    me_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; me_put(b, e, 2); }
        else if (c < 0x20) { char e[8]; _snprintf_s(e, sizeof e, _TRUNCATE, "\\u%04x", c); me_puts(b, e); }
        else me_put(b, (const char *)&c, 1);
    }
    me_put(b, "\"", 1);
}

/* The ZIP folder of a bundle: mods/[legacy/]<id>@<version>+<build>. */
static void me_folder(const sh_mb_bundle *b, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "mods/%s%s@%s+%llu", b->legacy ? "legacy/" : "", b->id, b->version, b->build);
}

static void me_join_native(char *out, size_t cap, const char *dir, const char *rel)
{
    size_t i, n;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, rel);
    n = strlen(out);
    for (i = strlen(dir); i < n; i++) if (out[i] == '/') out[i] = '\\';
}

/* A tiny set of 64-hex hashes. */
typedef struct me_hset { char (*v)[SH_MB_HASH_HEX + 1]; long *slot; size_t n, cap, mask; } me_hset;

static size_t me_hhash(const char *h)
{
    size_t x = 0;
    int i;
    for (i = 0; i < 16; i++) x = x * 16 + (size_t)((h[i] <= '9') ? h[i] - '0' : h[i] - 'a' + 10);
    return x;
}

static int me_hset_init(me_hset *s, size_t want)
{
    size_t cap = 64;
    memset(s, 0, sizeof *s);
    while (cap < want * 2) cap *= 2;
    s->v = (char (*)[SH_MB_HASH_HEX + 1])malloc(sizeof *s->v * (want ? want : 1));
    s->slot = (long *)malloc(sizeof(long) * cap);
    if (!s->v || !s->slot) { free(s->v); free(s->slot); memset(s, 0, sizeof *s); return 0; }
    memset(s->slot, 0xff, sizeof(long) * cap);
    s->cap = want ? want : 1;
    s->mask = cap - 1;
    return 1;
}

/* Index of h, or -1; with add, inserts it (returns the new index). */
static long me_hset_find(me_hset *s, const char *h, int add)
{
    size_t i = me_hhash(h) & s->mask;
    while (s->slot[i] >= 0) {
        if (strcmp(s->v[s->slot[i]], h) == 0) return s->slot[i];
        i = (i + 1) & s->mask;
    }
    if (!add || s->n >= s->cap) return -1;
    strcpy_s(s->v[s->n], SH_MB_HASH_HEX + 1, h);
    s->slot[i] = (long)s->n;
    return (long)s->n++;
}

static void me_hset_free(me_hset *s) { free(s->v); free(s->slot); memset(s, 0, sizeof *s); }

/* ------------------------------------------------------------ write ----- */

static int mm_list_has(const sh_modmanifest_list *list, const char *bundle)
{
    size_t i;
    for (i = 0; i < list->count; i++) if (!strcmp(list->rows[i].bundle, bundle)) return 1;
    return 0;
}

/* What one export carries besides its bundles: a map (whose rows are read from
 * its text and rewritten when a row pins an earlier build) or a prefab (whose
 * rows are given, and whose sidecar is written from the rows the export
 * carries). */
typedef struct me_payload {
    const char *entry;                 /* MODEXPORT_MAP_ENTRY or MODEXPORT_PREFAB_ENTRY */
    const char *text;
    size_t len;
    const sh_modmanifest_list *rows;   /* prefab: the rows; map: NULL (read from text) */
    const char *name;                  /* prefab: its name in export.json */
    modexport_meta_fn meta;            /* prefab: the sidecar for the carried rows */
    void *meta_ctx;
} me_payload;

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap);

int modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *stp,
                    char *cause, size_t cap)
{
    me_payload pl;
    memset(&pl, 0, sizeof pl);
    pl.entry = MODEXPORT_MAP_ENTRY;
    pl.text = map_json;
    pl.len = len;
    return me_write(&pl, zip_path, stp, cause, cap);
}

int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_meta_fn meta, void *meta_ctx, const char *zip_path, modexport_stats *stp,
                           char *cause, size_t cap)
{
    me_payload pl;
    sh_modmanifest_list none;
    memset(&pl, 0, sizeof pl);
    memset(&none, 0, sizeof none);
    if (!name || !name[0]) return me_cause(cause, cap, "the prefab has no name");
    if (len > MODEXPORT_PREFAB_MAX) return me_cause(cause, cap, "the prefab is larger than %u bytes", MODEXPORT_PREFAB_MAX);
    pl.entry = MODEXPORT_PREFAB_ENTRY;
    pl.text = prefab;
    pl.len = len;
    pl.rows = rows ? rows : &none;
    pl.name = name;
    pl.meta = meta;
    pl.meta_ctx = meta_ctx;
    return me_write(&pl, zip_path, stp, cause, cap);
}

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap)
{
    const char *map_json = pl->text;
    size_t len = pl->len;
    const int prefab = pl->rows != NULL;
    char *meta_text = NULL;
    size_t meta_len = 0;
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_modmanifest_list rows;
    sh_ms_list l;
    const sh_ms_entry **picked = NULL;
    me_writer w;
    me_hset written;
    me_buf index = {0};
    wchar_t *wfinal = NULL, *wtmp = NULL;
    char tmp[MAX_PATH * 4];
    unsigned rejected = 0;
    size_t i, k, total_files = 0;
    double t0 = me_now_ms();
    int ok = 0, listed = 0, locked = 0;
    char *moved_map = NULL;

    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    memset(&w, 0, sizeof w);
    memset(&written, 0, sizeof written);
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

    modsync_store_lock();
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
         * build serving it now, and the exported map's rows name that build,
         * so the importer needs no alias. */
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
                /* A prefab's rows live in its sidecar, written below from the rows carried. */
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
    if (prefab && pl->meta) {
        meta_text = pl->meta(pl->meta_ctx, &rows, &meta_len);
        if (!meta_text) { me_cause(cause, cap, "the prefab's details could not be written"); goto done; }
    }
    for (i = 0; i < rows.count; i++) total_files += picked[i]->bundle.file_count;
    if (!me_hset_init(&written, total_files + 1)) { me_cause(cause, cap, "out of memory"); goto done; }

    /* export.json: what the importer installs, in row order. */
    me_puts(&index, "{\"bundles\":[");
    for (i = 0; i < rows.count; i++) {
        char folder[MAX_PATH], num[32];
        const sh_mb_bundle *b = &picked[i]->bundle;
        me_folder(b, folder, sizeof folder);
        _snprintf_s(num, sizeof num, _TRUNCATE, "%llu", b->build);
        if (i) me_puts(&index, ",");
        me_puts(&index, "{\"build\":"); me_puts(&index, num);
        me_puts(&index, ",\"bundle\":"); me_put_str(&index, b->hash);
        me_puts(&index, ",\"folder\":"); me_put_str(&index, folder);
        me_puts(&index, ",\"id\":"); me_put_str(&index, b->id);
        me_puts(&index, ",\"version\":"); me_put_str(&index, b->version);
        me_puts(&index, "}");
    }
    if (prefab) {
        me_puts(&index, "],\"format\":1,\"kind\":\"prefab\",\"name\":");
        me_put_str(&index, pl->name);
        me_puts(&index, ",\"prefab\":\"" MODEXPORT_PREFAB_ENTRY "\"}");
    } else me_puts(&index, "],\"format\":1,\"map\":\"" MODEXPORT_MAP_ENTRY "\"}");
    if (index.bad) { me_cause(cause, cap, "out of memory"); goto done; }

    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.smp-%lu.tmp", zip_path, GetCurrentProcessId());
    wfinal = sh_mb_wpath(zip_path);
    wtmp = sh_mb_wpath(tmp);
    if (!wfinal || !wtmp) { me_cause(cause, cap, "the destination path is not usable"); goto done; }
    w.h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w.h == INVALID_HANDLE_VALUE) { me_cause(cause, cap, "cannot create %s (error %lu)", zip_path, GetLastError()); goto done; }

    me_add(&w, pl->entry, map_json, len);
    if (meta_text) me_add(&w, MODEXPORT_PREFAB_META, meta_text, meta_len);
    me_add(&w, MODEXPORT_INDEX, index.p, index.n);
    for (i = 0; i < rows.count && !w.failed; i++) {
        const sh_ms_entry *e = picked[i];
        const sh_mb_bundle *b = &e->bundle;
        char folder[MAX_PATH], name[MAX_PATH * 2], path[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1];
        unsigned char *bytes;
        size_t blen = 0;
        int dup = 0;
        for (k = 0; k < i && !dup; k++) dup = picked[k] == e;
        if (dup) continue;
        me_folder(b, folder, sizeof folder);
        /* The ZIP keeps the bundle.json form: both texts are derived from
         * mod.json in memory; the canonical one is the exact text whose
         * SHA-256 is the bundle hash. */
        {
            char *pretty = sh_mb_bundle_json(b, &blen);
            if (!pretty) { me_cause(cause, cap, "out of memory"); goto done; }
            _snprintf_s(name, sizeof name, _TRUNCATE, "%s/bundle.json", folder);
            me_add(&w, name, pretty, blen);
            free(pretty);
        }
        _snprintf_s(name, sizeof name, _TRUNCATE, "%s/bundle.canonical.json", folder);
        me_add(&w, name, b->canonical, b->canonical_len);
        for (k = 0; k < b->file_count && !w.failed; k++) {
            const sh_mb_file *f = &b->files[k];
            if (f->stock) continue;   /* installed content: the ZIP carries only the pointer */
            if (me_hset_find(&written, f->blob, 0) >= 0) { st->blobs_deduped++; continue; }
            me_join_native(path, sizeof path, e->folder, f->path);
            bytes = sh_mb_read_file(path, (size_t)SH_MB_MAX_FILE, &blen);
            if (bytes) {
                sh_mb_sha256_hex(bytes, blen, actual);
                if (blen != f->size || strcmp(actual, f->blob) != 0) { free(bytes); bytes = NULL; }
            }
            if (!bytes) { me_cause(cause, cap, "%s %s is missing or damaged (run sh_mods doctor verify)", e->name, f->path); goto done; }
            _snprintf_s(name, sizeof name, _TRUNCATE, "%s/%s", folder, f->path);
            me_add(&w, name, bytes, blen);
            free(bytes);
            me_hset_find(&written, f->blob, 1);
            st->blobs_written++;
        }
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
    me_hset_free(&written);
    free(index.p);
    free(picked);
    free(moved_map);
    free(meta_text);
    free(wfinal);
    free(wtmp);
    if (listed) sh_ms_list_free(&l);
    if (locked) modsync_store_unlock();
    sh_modmanifest_list_free(&rows);
    st->ms = me_now_ms() - t0;
    return ok;
}

/* ------------------------------------------------------------ read ------ */

typedef struct me_ibundle {
    int present;        /* installed and usable before the import */
    char hash[SH_MB_HASH_HEX + 1];
    char folder[MAX_PATH];
    char *canonical;
    size_t len;
    sh_mb_bundle b;
} me_ibundle;

typedef struct me_import {
    me_reader zr;
    me_ibundle *bundles;
    size_t nb;
    me_hset blobs;      /* blob -> slot; entry index in entry_of */
    long *entry_of;
} me_import;

static void me_import_free(me_import *im)
{
    size_t i;
    for (i = 0; i < im->nb; i++) { free(im->bundles[i].canonical); sh_mb_free(&im->bundles[i].b); }
    free(im->bundles);
    me_hset_free(&im->blobs);
    free(im->entry_of);
    me_reader_close(&im->zr);
    memset(im, 0, sizeof *im);
}

static char *me_with_bundle(const char *canonical, size_t len, const char *hash)
{
    size_t need = len + 96;
    char *out;
    if (len < 2 || canonical[0] != '{') return NULL;
    out = (char *)malloc(need);
    if (!out) return NULL;
    _snprintf_s(out, need, _TRUNCATE, "{\"bundle\":\"%s\"%s%.*s", hash, canonical[1] != '}' ? "," : "",
                (int)(len - 1), canonical + 1);
    return out;
}

/* Open an export and read its index and manifests (nothing installed). `main`
 * is the entry the export must carry (the map, or a prefab); a prefab's name
 * from export.json goes to `name` when it is not NULL. */
static int me_import_open_as(me_import *im, const char *zip_path, int manifests, const char *main, char *name,
                             size_t name_cap, char *cause, size_t cap);
static int me_import_open(me_import *im, const char *zip_path, int manifests, char *cause, size_t cap)
{
    return me_import_open_as(im, zip_path, manifests, MODEXPORT_MAP_ENTRY, NULL, 0, cause, cap);
}
static int me_import_open_as(me_import *im, const char *zip_path, int manifests, const char *main, char *name,
                             size_t name_cap, char *cause, size_t cap)
{
    unsigned char *text;
    size_t len = 0, i, k, files = 0;
    mpj *root = NULL;
    const mpj *arr;
    long idx;
    memset(im, 0, sizeof *im);
    if (!me_reader_open(&im->zr, zip_path, cause, cap)) return 0;
    if (me_find(&im->zr, main) < 0) {
        int other = me_find(&im->zr, strcmp(main, MODEXPORT_MAP_ENTRY) ? MODEXPORT_MAP_ENTRY : MODEXPORT_PREFAB_ENTRY) >= 0;
        me_import_free(im);
        if (other) return me_cause(cause, cap, strcmp(main, MODEXPORT_MAP_ENTRY) ? "this is a map's export, not a prefab's"
                                                                                 : "this is a prefab's export, not a map's");
        return me_cause(cause, cap, "the export has no %s", main);
    }
    idx = me_find(&im->zr, MODEXPORT_INDEX);
    if (idx < 0) { me_import_free(im); return me_cause(cause, cap, "the export has no %s", MODEXPORT_INDEX); }
    text = me_read_entry(&im->zr, idx, ME_INDEX_MAX, &len, cause, cap);
    if (!text) { me_import_free(im); return 0; }
    root = mpj_parse((const char *)text, len);
    free(text);
    arr = root && root->type == MPJ_OBJ ? mpj_get(root, "bundles") : NULL;
    if (!root || root->type != MPJ_OBJ || !mpj_get(root, "format") || mpj_int(mpj_get(root, "format")) != MODEXPORT_FORMAT ||
        !arr || arr->type != MPJ_ARR || arr->n > MODSYNC_MAX_BUNDLES) {
        mpj_free(root);
        me_import_free(im);
        return me_cause(cause, cap, "%s is not an export index of format %d", MODEXPORT_INDEX, MODEXPORT_FORMAT);
    }
    if (name && name_cap) {
        const mpj *nm = mpj_get(root, "name");
        name[0] = 0;
        if (nm && nm->type == MPJ_STR && nm->text) {
            /* The escaped body, decoded: \" and \\ only (a \u escape leaves the name empty). */
            size_t o = 0;
            const char *q;
            for (q = nm->text; *q && o + 1 < name_cap; q++) {
                if (*q == '\\' && q[1]) { q++; if (*q == 'u') { o = 0; break; } }
                name[o++] = *q;
            }
            name[o] = 0;
        }
    }
    im->bundles = (me_ibundle *)calloc((size_t)(arr->n ? arr->n : 1), sizeof *im->bundles);
    if (!im->bundles) { mpj_free(root); me_import_free(im); return me_cause(cause, cap, "out of memory"); }
    for (i = 0; i < (size_t)arr->n; i++) {
        const mpj *o = arr->items[i], *h = mpj_get(o, "bundle"), *f = mpj_get(o, "folder");
        me_ibundle *ib = &im->bundles[im->nb];
        if (!h || h->type != MPJ_STR || !h->text || strlen(h->text) != SH_MB_HASH_HEX ||
            !f || f->type != MPJ_STR || !f->text || strlen(f->text) >= sizeof ib->folder || strchr(f->text, '\\')) {
            mpj_free(root);
            me_import_free(im);
            return me_cause(cause, cap, "%s lists a bundle without a hash or folder", MODEXPORT_INDEX);
        }
        strcpy_s(ib->hash, sizeof ib->hash, h->text);
        strcpy_s(ib->folder, sizeof ib->folder, f->text);
        im->nb++;
    }
    mpj_free(root);
    if (!manifests) return 1;
    for (i = 0; i < im->nb; i++) {
        me_ibundle *ib = &im->bundles[i];
        char name[MAX_PATH * 2], actual[SH_MB_HASH_HEX + 1], expect[MAX_PATH];
        char *with;
        _snprintf_s(name, sizeof name, _TRUNCATE, "%s/bundle.canonical.json", ib->folder);
        ib->canonical = (char *)me_read_entry(&im->zr, me_find(&im->zr, name), SH_MB_MANIFEST_MAX, &ib->len, cause, cap);
        if (!ib->canonical) { me_import_free(im); return 0; }
        sh_mb_sha256_hex(ib->canonical, ib->len, actual);
        if (strcmp(actual, ib->hash) != 0) {
            me_import_free(im);
            return me_cause(cause, cap, "%s does not hash to the bundle %.16s it is listed as", name, ib->hash);
        }
        with = me_with_bundle(ib->canonical, ib->len, ib->hash);
        if (!with || !sh_mb_load_manifest_text(with, strlen(with), &ib->b)) {
            free(with);
            me_cause(cause, cap, "%s is not a bundle this release accepts: %s", name, ib->b.first_cause);
            me_import_free(im);
            return 0;
        }
        free(with);
        me_folder(&ib->b, expect, sizeof expect);
        if (strcmp(expect, ib->folder) != 0) {
            me_import_free(im);
            return me_cause(cause, cap, "%s is filed under %s, not %s", ib->hash, ib->folder, expect);
        }
        files += ib->b.file_count;
    }
    if (!me_hset_init(&im->blobs, files + 1) || !(im->entry_of = (long *)malloc(sizeof(long) * (files + 1)))) {
        me_import_free(im);
        return me_cause(cause, cap, "out of memory");
    }
    for (i = 0; i < im->nb; i++) {
        const sh_mb_bundle *b = &im->bundles[i].b;
        for (k = 0; k < b->file_count; k++) {
            char name[MAX_PATH * 2];
            long e, s;
            if (b->files[k].stock) continue;
            if (me_hset_find(&im->blobs, b->files[k].blob, 0) >= 0) continue;
            _snprintf_s(name, sizeof name, _TRUNCATE, "%s/%s", im->bundles[i].folder, b->files[k].path);
            e = me_find(&im->zr, name);
            if (e < 0) continue;
            s = me_hset_find(&im->blobs, b->files[k].blob, 1);
            if (s >= 0) im->entry_of[s] = e;
        }
    }
    return 1;
}

/* Mark the bundles the store already holds (installed and usable). */
static void me_installed_mark(me_import *im)
{
    sh_ms_list l;
    size_t i;
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        for (i = 0; i < im->nb; i++) {
            /* A hash an alias names here (an earlier build of an installed
             * folder) is present: the imported map loads what it resolves to. */
            const sh_ms_entry *e = sh_ms_resolve(&l, im->bundles[i].hash);
            im->bundles[i].present = e && sh_ms_entry_usable(e);
        }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
}

static int me_carried(const me_import *im, const char *hash)
{
    size_t i;
    for (i = 0; i < im->nb; i++) if (strcmp(im->bundles[i].hash, hash) == 0) return 1;
    return 0;
}

typedef struct me_src { me_import *im; unsigned fetched; } me_src;

static int me_zip_source(void *ctx, const char *sha, unsigned long long size, unsigned char **bytes, size_t *len,
                         char *cause, size_t cap)
{
    me_src *s = (me_src *)ctx;
    long slot = me_hset_find(&s->im->blobs, sha, 0);
    if (slot < 0) return me_cause(cause, cap, "the export does not carry it");
    *bytes = me_read_entry(&s->im->zr, s->im->entry_of[slot], (size_t)size, len, cause, cap);
    if (!*bytes) return 0;
    s->fetched++;
    return 1;
}

int modexport_probe(const char *zip_path, char *cause, size_t cap)
{
    me_import im;
    unsigned char *text;
    size_t len = 0, i = 0;
    if (cause && cap) cause[0] = 0;
    if (!me_import_open(&im, zip_path, 0, cause, cap)) return 0;
    text = me_read_entry(&im.zr, me_find(&im.zr, MODEXPORT_MAP_ENTRY), MODEXPORT_MAP_MAX, &len, cause, cap);
    me_import_free(&im);
    if (!text) return 0;
    if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) i = 3;
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    if (i >= len || text[i] != '{') { free(text); return me_cause(cause, cap, "the export's map is not rawmap JSON"); }
    free(text);
    return 1;
}

/* Install every bundle the export carries that the store does not hold
 * (verifying hashes), then rebuild the serving index when one was installed.
 * Nothing already installed is written again. */
static int me_install_all(me_import *im, modexport_stats *st, char *cause, size_t cap)
{
    me_src src;
    size_t i;
    st->bundles = (unsigned)im->nb;
    st->entries = (unsigned)im->zr.n;
    st->zip_bytes = im->zr.size;
    src.im = im;
    src.fetched = 0;
    /* One listing decides what is installed already, so a reload of an
     * imported export (the map load reads it again) costs one scan. */
    me_installed_mark(im);
    for (i = 0; i < im->nb; i++) {
        modsync_install_info info;
        char one[320];
        if (im->bundles[i].present) { st->bundles_present++; continue; }
        if (!modsync_install_manifest(im->bundles[i].canonical, im->bundles[i].len, im->bundles[i].hash, me_zip_source, &src,
                                      &info, one, sizeof one))
            return me_cause(cause, cap, "%s: %s", im->bundles[i].folder, one);
        if (info.already_installed) st->bundles_present++;
        else st->bundles_installed++;
    }
    st->blobs_written = src.fetched;
    if (st->bundles_installed && sh_modserve_index_rescan() < 0)
        me_log("MODS: import: bundles installed, but the serving index could not be rebuilt");
    return 1;
}

char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    unsigned char *text = NULL;
    size_t len = 0, i;
    double t0 = me_now_ms();
    if (out_len) *out_len = 0;
    memset(st, 0, sizeof *st);
    if (cause && cap) cause[0] = 0;
    if (!me_import_open(&im, zip_path, 1, cause, cap)) { st->ms = me_now_ms() - t0; return NULL; }
    if (!me_install_all(&im, st, cause, cap)) {
        me_import_free(&im);
        st->ms = me_now_ms() - t0;
        return NULL;
    }
    text = me_read_entry(&im.zr, me_find(&im.zr, MODEXPORT_MAP_ENTRY), MODEXPORT_MAP_MAX, &len, cause, cap);
    if (text) {
        /* Rows that name a bundle the export did not carry and the store lacks. */
        sh_modmanifest_list rows;
        unsigned rejected = 0;
        memset(&rows, 0, sizeof rows);
        sh_modmanifest_read((const char *)text, len, &rows, &rejected);
        for (i = 0; i < rows.count; i++)
            if (!me_carried(&im, rows.rows[i].bundle) && !modsync_find_installed(rows.rows[i].bundle, NULL, 0)) {
                st->bundles_absent++;
                me_log("MODS: import: the map names %s@%s (bundle %.16s), which the export does not carry",
                       rows.rows[i].id, rows.rows[i].version, rows.rows[i].bundle);
            }
        sh_modmanifest_list_free(&rows);
        st->map_bytes = len;
    }
    me_import_free(&im);
    st->ms = me_now_ms() - t0;
    if (out_len) *out_len = len;
    return (char *)text;
}

int modexport_read_prefab(const char *zip_path, char **prefab, size_t *prefab_len, char **meta, size_t *meta_len,
                          char *name, size_t name_cap, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    unsigned char *text = NULL, *side = NULL;
    size_t len = 0, slen = 0, i = 0;
    long at;
    double t0 = me_now_ms();
    if (prefab) *prefab = NULL;
    if (prefab_len) *prefab_len = 0;
    if (meta) *meta = NULL;
    if (meta_len) *meta_len = 0;
    if (name && name_cap) name[0] = 0;
    memset(st, 0, sizeof *st);
    if (cause && cap) cause[0] = 0;
    if (!prefab) return me_cause(cause, cap, "nothing to read into");
    if (!me_import_open_as(&im, zip_path, 1, MODEXPORT_PREFAB_ENTRY, name, name_cap, cause, cap)) {
        st->ms = me_now_ms() - t0;
        return 0;
    }
    /* The prefab is read before anything is installed: a damaged export
     * installs nothing. */
    text = me_read_entry(&im.zr, me_find(&im.zr, MODEXPORT_PREFAB_ENTRY), MODEXPORT_PREFAB_MAX, &len, cause, cap);
    if (text) {
        if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) i = 3;
        while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
        if (i >= len || text[i] != '{') { free(text); text = NULL; me_cause(cause, cap, "the export's prefab is not prefab JSON"); }
    }
    at = me_find(&im.zr, MODEXPORT_PREFAB_META);
    if (text && at >= 0) {
        side = me_read_entry(&im.zr, at, ME_INDEX_MAX, &slen, cause, cap);
        if (!side) { free(text); text = NULL; }
    }
    if (!text || !me_install_all(&im, st, cause, cap)) {
        free(text);
        free(side);
        me_import_free(&im);
        st->ms = me_now_ms() - t0;
        return 0;
    }
    st->map_bytes = len;
    me_import_free(&im);
    *prefab = (char *)text;
    if (prefab_len) *prefab_len = len;
    if (meta) *meta = (char *)side; else free(side);
    if (meta_len) *meta_len = side ? slen : 0;
    st->ms = me_now_ms() - t0;
    return 1;
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
    me_log("MODS: export %s: map %llu byte(s), %u bundle(s), %u blob(s) stored, %u deduplicated, %u entr%s, %llu byte(s) in %.0f ms",
           path, st.map_bytes, st.bundles, st.blobs_written, st.blobs_deduped, st.entries, st.entries == 1 ? "y" : "ies",
           st.zip_bytes, st.ms);
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
    me_log("MODS: import %s: %u bundle(s) (%u installed, %u already present), %u blob(s) installed, map %llu byte(s), "
           "%llu byte(s) read in %.0f ms%s", path, st.bundles, st.bundles_installed, st.bundles_present, st.blobs_written,
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
