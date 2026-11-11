/* modzip.c -- see modzip.h. Moved from modexport.c unchanged in behaviour. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "modzip.h"
#include "modbundle.h"      /* sh_mb_wpath */
#include "raw_deflate.h"
#include "plain_reason.h"

#define ZIP_CD_MAX         (16u * 1024u * 1024u)
#define ZIP_DOS_DATE       0x0021u     /* 1980-01-01: archives of the same content are byte-identical */
#define ZIP_DOS_TIME       0x0000u
#define ZIP_FLAG_UTF8      0x0800u

/* The next step of every refusal of an archive's structure. */
#define ZIP_AGAIN ". Ask whoever shared it to export it again"

static int zip_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* ------------------------------------------------------------ crc32 ----- */

static uint32_t g_zip_crc_table[256];
static INIT_ONCE g_zip_crc_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK zip_crc_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    uint32_t i, k, c;
    (void)once; (void)p; (void)ctx;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_zip_crc_table[i] = c;
    }
    return TRUE;
}

static uint32_t zip_crc32(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    InitOnceExecuteOnce(&g_zip_crc_once, zip_crc_init, NULL, NULL);
    for (i = 0; i < n; i++) c = g_zip_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void zip_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void zip_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned zip_get16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t zip_get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zip_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

/* ------------------------------------------------------------ writer ---- */

static int zip_wraw(sh_zip_writer *w, const void *p, size_t n)
{
    size_t off = 0;
    if (w->failed) return 0;
    while (off < n) {
        DWORD chunk = (DWORD)((n - off) > (1u << 24) ? (1u << 24) : (n - off)), put = 0;
        if (!WriteFile(w->h, (const unsigned char *)p + off, chunk, &put, NULL) || put != chunk) {
            w->failed = 1;
            zip_cause(w->cause, sizeof w->cause, "writing the ZIP failed: %s", sh_reason_win32(GetLastError(), "the ZIP", chunk));
            return 0;
        }
        off += put;
    }
    w->off += n;
    return 1;
}

int sh_zip_writer_open(sh_zip_writer *w, const wchar_t *path)
{
    memset(w, 0, sizeof *w);
    w->h = path ? CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    return w->h != INVALID_HANDLE_VALUE;
}

int sh_zip_writer_add(sh_zip_writer *w, const char *name, const void *data, size_t len)
{
    unsigned char hdr[30];
    size_t nl = strlen(name);
    sh_zip_went *e;
    if (w->failed) return 0;
    if (w->n >= SH_ZIP_MAX_ENTRIES || nl == 0 || nl > SH_ZIP_NAME_MAX) {
        w->failed = 1;
        return zip_cause(w->cause, sizeof w->cause, w->n >= SH_ZIP_MAX_ENTRIES ? "more than 65535 files" : "a file name is too long");
    }
    if (w->off + 30 + nl + len + 46 + nl + 22 >= SH_ZIP_LIMIT) {
        w->failed = 1;
        return zip_cause(w->cause, sizeof w->cause, "the export would exceed 4 GiB");
    }
    if (w->n == w->cap) {
        size_t c2 = w->cap ? w->cap * 2 : 64;
        sh_zip_went *g = (sh_zip_went *)realloc(w->e, sizeof *g * c2);
        if (!g) { w->failed = 1; return zip_cause(w->cause, sizeof w->cause, "out of memory"); }
        w->e = g;
        w->cap = c2;
    }
    e = &w->e[w->n];
    e->name = _strdup(name);
    if (!e->name) { w->failed = 1; return zip_cause(w->cause, sizeof w->cause, "out of memory"); }
    e->crc = zip_crc32((const unsigned char *)data, len);
    e->size = (uint32_t)len;
    e->offset = (uint32_t)w->off;
    e->flags = zip_ascii(name) ? 0 : ZIP_FLAG_UTF8;
    w->n++;
    zip_put32(hdr, 0x04034b50u);
    zip_put16(hdr + 4, 20);
    zip_put16(hdr + 6, e->flags);
    zip_put16(hdr + 8, 0);
    zip_put16(hdr + 10, ZIP_DOS_TIME);
    zip_put16(hdr + 12, ZIP_DOS_DATE);
    zip_put32(hdr + 14, e->crc);
    zip_put32(hdr + 18, e->size);
    zip_put32(hdr + 22, e->size);
    zip_put16(hdr + 26, (unsigned)nl);
    zip_put16(hdr + 28, 0);
    return zip_wraw(w, hdr, sizeof hdr) && zip_wraw(w, name, nl) && zip_wraw(w, data, len);
}

int sh_zip_writer_finish(sh_zip_writer *w)
{
    unsigned char hdr[46], end[22];
    unsigned long long cd = w->off;
    size_t i;
    for (i = 0; i < w->n && !w->failed; i++) {
        const sh_zip_went *e = &w->e[i];
        size_t nl = strlen(e->name);
        zip_put32(hdr, 0x02014b50u);
        zip_put16(hdr + 4, 20);
        zip_put16(hdr + 6, 20);
        zip_put16(hdr + 8, e->flags);
        zip_put16(hdr + 10, 0);
        zip_put16(hdr + 12, ZIP_DOS_TIME);
        zip_put16(hdr + 14, ZIP_DOS_DATE);
        zip_put32(hdr + 16, e->crc);
        zip_put32(hdr + 20, e->size);
        zip_put32(hdr + 24, e->size);
        zip_put16(hdr + 28, (unsigned)nl);
        zip_put16(hdr + 30, 0);
        zip_put16(hdr + 32, 0);
        zip_put16(hdr + 34, 0);
        zip_put16(hdr + 36, 0);
        zip_put32(hdr + 38, 0);
        zip_put32(hdr + 42, e->offset);
        if (!zip_wraw(w, hdr, sizeof hdr) || !zip_wraw(w, e->name, nl)) return 0;
    }
    if (w->failed) return 0;
    if (w->off >= SH_ZIP_LIMIT) { w->failed = 1; return zip_cause(w->cause, sizeof w->cause, "the export would exceed 4 GiB"); }
    zip_put32(end, 0x06054b50u);
    zip_put16(end + 4, 0);
    zip_put16(end + 6, 0);
    zip_put16(end + 8, (unsigned)w->n);
    zip_put16(end + 10, (unsigned)w->n);
    zip_put32(end + 12, (uint32_t)(w->off - cd));
    zip_put32(end + 16, (uint32_t)cd);
    zip_put16(end + 20, 0);
    return zip_wraw(w, end, sizeof end);
}

void sh_zip_writer_free(sh_zip_writer *w)
{
    size_t i;
    if (w->h && w->h != INVALID_HANDLE_VALUE) CloseHandle(w->h);
    w->h = INVALID_HANDLE_VALUE;
    for (i = 0; i < w->n; i++) free(w->e[i].name);
    free(w->e);
    w->e = NULL;
    w->n = w->cap = 0;
}

/* ------------------------------------------------------------ reader ---- */

static int zip_read_at(HANDLE h, unsigned long long at, void *buf, size_t n)
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
int sh_zip_name_ok(const char *s, size_t n)
{
    size_t i, seg = 0;
    if (n == 0 || n > SH_ZIP_NAME_MAX || s[0] == '/') return 0;
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

/* Entry names compare without case first (DECISIONS 13: names are the
 * game's, and two names one Windows folder cannot tell apart are one file),
 * then byte for byte, so the order is total and find agrees with sort. */
static int zip_name_cmp(const char *a, const char *b)
{
    int c = _stricmp(a, b);
    return c ? c : strcmp(a, b);
}

static const sh_zip_reader *g_zip_sort_reader;
static int zip_sort_cmp(const void *a, const void *b)
{
    return zip_name_cmp(g_zip_sort_reader->e[*(const size_t *)a].name, g_zip_sort_reader->e[*(const size_t *)b].name);
}

void sh_zip_reader_close(sh_zip_reader *r)
{
    size_t i;
    if (r->h && r->h != INVALID_HANDLE_VALUE) CloseHandle(r->h);
    for (i = 0; i < r->n; i++) free(r->e[i].name);
    free(r->e);
    free(r->sorted);
    free(r->dir);
    memset(r, 0, sizeof *r);
}

static INIT_ONCE g_zip_sort_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_zip_sort_cs;
static BOOL CALLBACK zip_sort_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_zip_sort_cs);
    return TRUE;
}

/* Index the entries by name; two entries of one name refuse the export. */
int sh_zip_reader_sort(sh_zip_reader *r, char *cause, size_t cap)
{
    size_t i;
    free(r->sorted);
    r->sorted = (size_t *)malloc(sizeof(size_t) * (r->n ? r->n : 1));
    if (!r->sorted) { sh_zip_reader_close(r); return zip_cause(cause, cap, "out of memory"); }
    for (i = 0; i < r->n; i++) r->sorted[i] = i;
    InitOnceExecuteOnce(&g_zip_sort_once, zip_sort_init, NULL, NULL);
    EnterCriticalSection(&g_zip_sort_cs);
    g_zip_sort_reader = r;
    qsort(r->sorted, r->n, sizeof(size_t), zip_sort_cmp);
    g_zip_sort_reader = NULL;
    LeaveCriticalSection(&g_zip_sort_cs);
    for (i = 1; i < r->n; i++)
        if (_stricmp(r->e[r->sorted[i - 1]].name, r->e[r->sorted[i]].name) == 0) {
            sh_zip_reader_close(r);
            return zip_cause(cause, cap, "the export holds two files with one name" ZIP_AGAIN);
        }
    return 1;
}

/* The refusal of a damaged directory: the reader closed, the cause set. */
static int zip_damaged(sh_zip_reader *r, unsigned char *cd, char *cause, size_t cap)
{
    free(cd);
    sh_zip_reader_close(r);
    return zip_cause(cause, cap, "the export's file list is damaged" ZIP_AGAIN);
}

int sh_zip_reader_open(sh_zip_reader *r, const char *path, char *cause, size_t cap)
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
    if (r->h == INVALID_HANDLE_VALUE) { r->h = NULL; return zip_cause(cause, cap, "cannot open the export"); }
    if (!GetFileSizeEx(r->h, &sz) || sz.QuadPart < 22) {
        sh_zip_reader_close(r);
        return zip_cause(cause, cap, "the export is not a ZIP file" ZIP_AGAIN);
    }
    r->size = (unsigned long long)sz.QuadPart;
    tail_n = r->size < sizeof tail ? (size_t)r->size : sizeof tail;
    if (!zip_read_at(r->h, r->size - tail_n, tail, tail_n)) { sh_zip_reader_close(r); return zip_cause(cause, cap, "cannot read the export"); }
    for (pos = tail_n - 22 + 1; pos-- > 0;) {
        if (zip_get32(tail + pos) == 0x06054b50u && pos + 22 + zip_get16(tail + pos + 20) == tail_n) { found = pos; break; }
    }
    if (found == (size_t)-1) {
        sh_zip_reader_close(r);
        return zip_cause(cause, cap, "the export is not a ZIP file (no end record)" ZIP_AGAIN);
    }
    entries = zip_get16(tail + found + 10);
    cd_size = zip_get32(tail + found + 12);
    cd_off = zip_get32(tail + found + 16);
    if (zip_get16(tail + found + 4) != 0 || zip_get16(tail + found + 6) != 0 || zip_get16(tail + found + 8) != entries ||
        entries == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
        sh_zip_reader_close(r);
        return zip_cause(cause, cap, "the export is a split or ZIP64 archive, which is not supported" ZIP_AGAIN);
    }
    /* The directory lies whole before the end record, and every entry's
     * record and data before the directory. */
    if ((unsigned long long)cd_off + cd_size > r->size - tail_n + found || cd_size > ZIP_CD_MAX ||
        (unsigned long long)entries * 46u > cd_size)
        return zip_damaged(r, NULL, cause, cap);
    cd = (unsigned char *)malloc(cd_size ? cd_size : 1);
    r->e = (sh_zip_rentry *)calloc(entries ? entries : 1, sizeof *r->e);
    if (!cd || !r->e || !zip_read_at(r->h, cd_off, cd, cd_size)) {
        free(cd);
        sh_zip_reader_close(r);
        return zip_cause(cause, cap, "cannot read the export's file list");
    }
    for (i = 0, pos = 0; i < entries; i++) {
        sh_zip_rentry *e = &r->e[i];
        unsigned nl, xl, cl;
        if (pos + 46 > cd_size || zip_get32(cd + pos) != 0x02014b50u) return zip_damaged(r, cd, cause, cap);
        e->flags = zip_get16(cd + pos + 8);
        e->method = zip_get16(cd + pos + 10);
        e->crc = zip_get32(cd + pos + 16);
        e->csize = zip_get32(cd + pos + 20);
        e->usize = zip_get32(cd + pos + 24);
        nl = zip_get16(cd + pos + 28);
        xl = zip_get16(cd + pos + 30);
        cl = zip_get16(cd + pos + 32);
        e->lho = zip_get32(cd + pos + 42);
        if ((size_t)pos + 46 + nl + xl + cl > cd_size) return zip_damaged(r, cd, cause, cap);
        /* Every field a later read trusts is checked here, once: the method
         * (stored or deflate only; a folder reader's marker never comes from
         * a file), no encryption, sizes that are not ZIP64 markers, a stored
         * entry's two sizes equal, and its local record inside the archive
         * before the directory. */
        if (e->flags & 1) {
            free(cd);
            sh_zip_reader_close(r);
            return zip_cause(cause, cap, "the export holds an encrypted file, which Snapmap+ cannot read" ZIP_AGAIN);
        }
        if ((e->method != 0 && e->method != 8) || e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu ||
            e->lho == 0xFFFFFFFFu) {
            free(cd);
            sh_zip_reader_close(r);
            return zip_cause(cause, cap, "the export holds a file packed in a way Snapmap+ cannot read" ZIP_AGAIN);
        }
        if ((e->method == 0 && e->csize != e->usize) ||
            (unsigned long long)e->lho + 30 + nl + (unsigned long long)e->csize > cd_off)
            return zip_damaged(r, cd, cause, cap);
        e->name = (char *)malloc(nl + 1);
        if (!e->name) { free(cd); sh_zip_reader_close(r); return zip_cause(cause, cap, "out of memory"); }
        memcpy(e->name, cd + pos + 46, nl);
        e->name[nl] = 0;
        r->n++;
        if (memchr(e->name, 0, nl) || !sh_zip_name_ok(e->name, nl)) {
            char shown[120];
            unsigned k, sw = 0;
            for (k = 0; k < nl && sw + 1 < sizeof shown; k++)
                shown[sw++] = (unsigned char)e->name[k] < 0x20 || (unsigned char)e->name[k] == 0x7f ? '?' : e->name[k];
            shown[sw] = 0;
            free(cd);
            sh_zip_reader_close(r);
            return zip_cause(cause, cap, "the export holds a file with an unsafe name, \"%s%s\" (an absolute path, a "
                            "'..' step, a backslash, a colon or a control character); it was not imported. Export it "
                            "again with Snapmap+, or remove that entry from the ZIP", shown, nl >= sizeof shown ? "..." : "");
        }
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    return sh_zip_reader_sort(r, cause, cap);
}

long sh_zip_reader_find(const sh_zip_reader *r, const char *name)
{
    size_t lo = 0, hi = r->n;
    if (!r->sorted || !name) return -1;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = zip_name_cmp(r->e[r->sorted[mid]].name, name);
        if (c == 0) return (long)r->sorted[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* A folder export's file: the entry name is relative to r->dir, '/'-separated. */
static wchar_t *zip_dir_path(const sh_zip_reader *r, const char *name)
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

static unsigned char *zip_read_dir_entry(const sh_zip_reader *r, const sh_zip_rentry *e, size_t max, size_t *out_len, char *cause,
                                        size_t cap)
{
    wchar_t *w = zip_dir_path(r, e->name);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    LARGE_INTEGER sz;
    unsigned char *dst;
    free(w);
    if (h == INVALID_HANDLE_VALUE) { zip_cause(cause, cap, "cannot open %s", e->name); return NULL; }
    if (!GetFileSizeEx(h, &sz) || (unsigned long long)sz.QuadPart > max) {
        CloseHandle(h);
        zip_cause(cause, cap, "%s is larger than %zu bytes", e->name, max);
        return NULL;
    }
    dst = (unsigned char *)malloc((size_t)sz.QuadPart + 1);
    if (!dst || (sz.QuadPart && !zip_read_at(h, 0, dst, (size_t)sz.QuadPart))) {
        free(dst);
        CloseHandle(h);
        zip_cause(cause, cap, "cannot read %s", e->name);
        return NULL;
    }
    CloseHandle(h);
    dst[sz.QuadPart] = 0;
    if (out_len) *out_len = (size_t)sz.QuadPart;
    return dst;
}

/* One entry's bytes (malloc'd, NUL-terminated), at most max bytes. */
unsigned char *sh_zip_reader_read(const sh_zip_reader *r, long idx, size_t max, size_t *out_len, char *cause, size_t cap)
{
    const sh_zip_rentry *e;
    unsigned char lh[30], *src = NULL, *dst = NULL;
    unsigned long long data;
    if (out_len) *out_len = 0;
    if (idx < 0 || (size_t)idx >= r->n) { zip_cause(cause, cap, "no such file in the export"); return NULL; }
    e = &r->e[idx];
    /* A folder reader's entries are its own; an archive's were checked at open. */
    if (r->dir) return zip_read_dir_entry(r, e, max, out_len, cause, cap);
    if (e->usize > max) { zip_cause(cause, cap, "%s is larger than %zu bytes", e->name, max); return NULL; }
    if (!zip_read_at(r->h, e->lho, lh, sizeof lh) || zip_get32(lh) != 0x04034b50u) {
        zip_cause(cause, cap, "%s is damaged" ZIP_AGAIN, e->name);
        return NULL;
    }
    data = (unsigned long long)e->lho + 30 + zip_get16(lh + 26) + zip_get16(lh + 28);
    if (data + e->csize > r->size) { zip_cause(cause, cap, "%s is damaged" ZIP_AGAIN, e->name); return NULL; }
    dst = (unsigned char *)malloc((size_t)e->usize + 1);
    if (!dst) { zip_cause(cause, cap, "out of memory"); return NULL; }
    if (e->method == 0) {
        if (!zip_read_at(r->h, data, dst, e->usize)) { free(dst); zip_cause(cause, cap, "cannot read %s", e->name); return NULL; }
    } else {
        src = (unsigned char *)malloc(e->csize ? e->csize : 1);
        if (!src || !zip_read_at(r->h, data, src, e->csize) || !sh_inflate_raw_exact(src, e->csize, dst, e->usize)) {
            free(src);
            free(dst);
            zip_cause(cause, cap, "%s does not decompress" ZIP_AGAIN, e->name);
            return NULL;
        }
        free(src);
    }
    if (zip_crc32(dst, e->usize) != e->crc) { free(dst); zip_cause(cause, cap, "%s fails its checksum" ZIP_AGAIN, e->name); return NULL; }
    dst[e->usize] = 0;
    if (out_len) *out_len = e->usize;
    return dst;
}


int sh_zip_reader_add(sh_zip_reader *r, const char *name, unsigned long long size)
{
    sh_zip_rentry *e;
    if (!r->dir || !name || r->n >= SH_ZIP_MAX_ENTRIES || size > SH_ZIP_LIMIT) return 0;
    if (r->n == r->cap) {
        size_t c2 = r->cap ? r->cap * 2 : 64;
        sh_zip_rentry *g = (sh_zip_rentry *)realloc(r->e, sizeof *g * c2);
        if (!g) return 0;
        r->e = g;
        r->cap = c2;
    }
    e = &r->e[r->n];
    memset(e, 0, sizeof *e);
    e->name = _strdup(name);
    if (!e->name) return 0;
    e->method = SH_ZIP_METHOD_FILE;
    e->usize = (uint32_t)size;
    r->n++;
    return 1;
}
