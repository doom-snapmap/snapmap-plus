/* modstock.c -- see modstock.h. The two pindex files of the installed game
 * are parsed into one table of rows (name, container, selector, offset,
 * stored and decoded size); a row's bytes are read with one positional read
 * and, when stored smaller than decoded, inflated. Loose files are read from
 * <doom>\base\<path>, then <doom>\<path>. Nothing here writes a file.
 *
 * pindex layout (13954591, as the retired bridge read it): 0x05 "SER", a
 * big-endian length of the rest past 0x20, a big-endian row count at 0x20,
 * then per row 4 bytes, three length-prefixed (little-endian) ASCII strings
 * (type, name, path) and 21 fixed bytes: offset (be64), decoded size (be32),
 * stored size (be32), four flag bytes and the selector (0 .resources,
 * 1 .patch). An entry's name is its path, or its name when the path is empty.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modstock.h"
#include "modbundle.h"
#include "raw_deflate.h"
#include "backend_log.h"

#define MST_CONTAINERS     2
#define MST_MAX_PINDEX     (64u << 20)
#define MST_MAX_ROWS       1000000u
#define MST_MAX_FIELD      4096u

/* Container order: SnapMap's own archive first, so a path both hold is named
 * by what SnapMap mounts; content found in either is accepted by its hash. */
static const char *const MST_CONTAINER[MST_CONTAINERS] = { "snap_gameresources", "gameresources" };

typedef struct mst_row {
    uint64_t hash;          /* FNV-1a 64 of the folded name */
    uint64_t offset;
    uint32_t name;          /* offset of the folded name in g_names */
    uint32_t size, zsize;   /* decoded and stored */
    uint32_t ext;           /* FNV-1a 32 of the folded extension */
    uint8_t container, selector;
} mst_row;

enum { MST_NEW = 0, MST_READY = 1, MST_FAILED = -1 };

static SRWLOCK g_lock = SRWLOCK_INIT;           /* the table and the root */
static volatile LONG g_state = MST_NEW;
static char g_root[MAX_PATH * 4];
static mst_row *g_rows;
static uint32_t g_nrows;
static char *g_names;
static size_t g_names_len;
static uint32_t *g_by_name;                     /* row indices sorted by hash */
static uint32_t *g_by_size;                     /* row indices sorted by (size, ext, container, name) */
static HANDLE g_files[MST_CONTAINERS][2];
static double g_load_ms;
static char g_cause[160];

static SRWLOCK g_memo_lock = SRWLOCK_INIT;      /* the per-row SHA-256 memo */
static unsigned char (*g_sha)[32];
static unsigned char *g_sha_done;
static volatile LONG g_slices_hashed, g_reads;
static volatile LONG64 g_bytes_hashed;

/* ------------------------------------------------------------ helpers ---- */

static __inline char mst_fold(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c == '\\' ? '/' : c;
}

static uint64_t mst_fnv64(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= (unsigned char)mst_fold(s[i]); h *= 1099511628211ull; }
    return h;
}

static uint32_t mst_ext_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i = n, k;
    while (i > 0 && s[i - 1] != '.' && s[i - 1] != '/') i--;
    if (i == 0 || s[i - 1] != '.') return 0;
    for (k = i; k < n; k++) { h ^= (unsigned char)mst_fold(s[k]); h *= 16777619u; }
    return h ? h : 1;
}

static int mst_name_eq(const char *folded, const char *s)
{
    for (;; folded++, s++) {
        if (*folded != mst_fold(*s)) return 0;
        if (!*folded) return 1;
    }
}

static unsigned mst_be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static unsigned mst_le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static double mst_now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void mst_hex(const unsigned char d[32], char out[SH_MB_HASH_HEX + 1])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}

static int mst_unhex(const char *s, unsigned char d[32])
{
    int i;
    if (!s || strlen(s) != SH_MB_HASH_HEX) return 0;
    for (i = 0; i < 64; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        if (i & 1) d[i / 2] |= (unsigned char)v;
        else d[i / 2] = (unsigned char)(v << 4);
    }
    return 1;
}

static void mst_default_root(char *out, size_t cap)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof exe[0]));
    wchar_t *slash;
    out[0] = 0;
    if (!n || n >= sizeof exe / sizeof exe[0]) return;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, exe, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
}

static HANDLE mst_open_read(const char *path_utf8, int random)
{
    wchar_t *w = sh_mb_wpath(path_utf8);
    HANDLE h;
    if (!w) return INVALID_HANDLE_VALUE;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    random ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    return h;
}

/* One positional read; safe on a handle shared by threads. */
static int mst_pread(HANDLE h, uint64_t offset, void *buf, uint32_t n)
{
    OVERLAPPED ov;
    DWORD got = 0;
    unsigned char *p = (unsigned char *)buf;
    while (n) {
        DWORD want = n > (1u << 30) ? (1u << 30) : n;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        if (!ReadFile(h, p, want, &got, &ov) || got != want) return 0;
        p += got;
        offset += got;
        n -= got;
    }
    return 1;
}

/* Buffers handed to a caller live on the process heap (modbundle.h read);
 * scratch buffers use the C heap. */
static void *mst_alloc(size_t n, int heap)
{
    return heap ? HeapAlloc(GetProcessHeap(), 0, n ? n : 1u) : malloc(n ? n : 1u);
}

static void mst_free(void *p, int heap)
{
    if (!p) return;
    if (heap) HeapFree(GetProcessHeap(), 0, p);
    else free(p);
}

/* ------------------------------------------------------------- loading ---- */

static void mst_free_table(void)
{
    int c, s;
    for (c = 0; c < MST_CONTAINERS; c++)
        for (s = 0; s < 2; s++) {
            if (g_files[c][s] && g_files[c][s] != INVALID_HANDLE_VALUE) CloseHandle(g_files[c][s]);
            g_files[c][s] = NULL;
        }
    free(g_rows); g_rows = NULL; g_nrows = 0;
    free(g_names); g_names = NULL; g_names_len = 0;
    free(g_by_name); g_by_name = NULL;
    free(g_by_size); g_by_size = NULL;
    AcquireSRWLockExclusive(&g_memo_lock);
    free(g_sha); g_sha = NULL;
    free(g_sha_done); g_sha_done = NULL;
    ReleaseSRWLockExclusive(&g_memo_lock);
}

static int __cdecl mst_name_order(const void *a, const void *b)
{
    const mst_row *x = &g_rows[*(const uint32_t *)a], *y = &g_rows[*(const uint32_t *)b];
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    if (x->container != y->container) return x->container < y->container ? -1 : 1;
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : 1;
}

static int __cdecl mst_size_order(const void *a, const void *b)
{
    const mst_row *x = &g_rows[*(const uint32_t *)a], *y = &g_rows[*(const uint32_t *)b];
    int c;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->ext != y->ext) return x->ext < y->ext ? -1 : 1;
    if (x->container != y->container) return x->container < y->container ? -1 : 1;
    c = strcmp(g_names + x->name, g_names + y->name);
    if (c) return c;
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : 1;
}

static int mst_grow(void **p, size_t *cap, size_t need, size_t elem)
{
    size_t nc;
    void *g;
    if (need <= *cap) return 1;
    nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    g = realloc(*p, nc * elem);
    if (!g) return 0;
    *p = g;
    *cap = nc;
    return 1;
}

static int mst_parse_pindex(int container, const unsigned char *d, size_t len, size_t *rows_cap, size_t *names_cap)
{
    unsigned rows, r;
    size_t pos = 0x24;
    if (len < 0x24 || d[0] != 0x05 || memcmp(d + 1, "SER", 3) != 0 || mst_be32(d + 4) != len - 0x20) return 0;
    rows = mst_be32(d + 0x20);
    if (rows > MST_MAX_ROWS) return 0;
    for (r = 0; r < rows; r++) {
        const unsigned char *s[3];
        unsigned n[3], k, i;
        const unsigned char *fixed;
        const char *nm;
        unsigned nl;
        mst_row *row;
        if (len - pos < 4) return 0;
        pos += 4;
        for (k = 0; k < 3; k++) {
            if (len - pos < 4) return 0;
            n[k] = mst_le32(d + pos);
            pos += 4;
            if (n[k] > MST_MAX_FIELD || n[k] > len - pos) return 0;
            for (i = 0; i < n[k]; i++) if (d[pos + i] == 0 || d[pos + i] > 0x7e) return 0;
            s[k] = d + pos;
            pos += n[k];
        }
        if (len - pos < 21) return 0;
        fixed = d + pos;
        pos += 21;
        nm = (const char *)(n[2] ? s[2] : s[1]);
        nl = n[2] ? n[2] : n[1];
        if (!nl || fixed[20] > 1) continue;
        if (!mst_be32(fixed + 8)) continue;          /* no bytes: nothing a bundle file can equal */
        if (!mst_grow((void **)&g_rows, rows_cap, (size_t)g_nrows + 1, sizeof(mst_row)) ||
            !mst_grow((void **)&g_names, names_cap, g_names_len + nl + 1, 1) || g_names_len + nl + 1 > 0xffffffffu)
            return -1;
        row = &g_rows[g_nrows++];
        row->hash = mst_fnv64(nm, nl);
        row->ext = mst_ext_hash(nm, nl);
        row->name = (uint32_t)g_names_len;
        for (i = 0; i < nl; i++) g_names[g_names_len + i] = mst_fold(nm[i]);
        g_names[g_names_len + nl] = 0;
        g_names_len += nl + 1;
        row->offset = ((uint64_t)mst_be32(fixed) << 32) | mst_be32(fixed + 4);
        row->size = mst_be32(fixed + 8);
        row->zsize = mst_be32(fixed + 12);
        row->container = (uint8_t)container;
        row->selector = fixed[20];
    }
    return pos == len;
}

static unsigned char *mst_read_whole(const char *path, size_t max, size_t *len, int heap)
{
    HANDLE h = mst_open_read(path, 0);
    LARGE_INTEGER sz;
    unsigned char *buf;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (unsigned long long)sz.QuadPart > max) { CloseHandle(h); return NULL; }
    buf = (unsigned char *)mst_alloc((size_t)sz.QuadPart, heap);
    if (!buf || !mst_pread(h, 0, buf, (uint32_t)sz.QuadPart)) { mst_free(buf, heap); CloseHandle(h); return NULL; }
    CloseHandle(h);
    *len = (size_t)sz.QuadPart;
    return buf;
}

/* Called with g_lock held exclusively. */
static void mst_load_locked(void)
{
    char path[MAX_PATH * 4 + 64];
    size_t rows_cap = 0, names_cap = 0;
    double t0 = mst_now_ms();
    int c, any = 0;
    uint32_t i;
    if (!g_root[0]) mst_default_root(g_root, sizeof g_root);
    for (c = 0; c < MST_CONTAINERS; c++) {
        size_t len = 0;
        unsigned char *d;
        int ok;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\base\\%s.pindex", g_root, MST_CONTAINER[c]);
        d = mst_read_whole(path, MST_MAX_PINDEX, &len, 0);
        if (!d) continue;
        ok = mst_parse_pindex(c, d, len, &rows_cap, &names_cap);
        free(d);
        if (ok < 0) { strcpy_s(g_cause, sizeof g_cause, "out of memory reading the installed indexes"); goto fail; }
        if (!ok) { _snprintf_s(g_cause, sizeof g_cause, _TRUNCATE, "%s.pindex does not read as an index", MST_CONTAINER[c]); goto fail; }
        any = 1;
    }
    if (!any) {
        _snprintf_s(g_cause, sizeof g_cause, _TRUNCATE, "no installed index under %.100s\\base", g_root);
        goto fail;
    }
    g_by_name = (uint32_t *)malloc(sizeof(uint32_t) * (g_nrows ? g_nrows : 1));
    g_by_size = (uint32_t *)malloc(sizeof(uint32_t) * (g_nrows ? g_nrows : 1));
    if (!g_by_name || !g_by_size) { strcpy_s(g_cause, sizeof g_cause, "out of memory sorting the installed indexes"); goto fail; }
    for (i = 0; i < g_nrows; i++) g_by_name[i] = g_by_size[i] = i;
    qsort(g_by_name, g_nrows, sizeof(uint32_t), mst_name_order);
    qsort(g_by_size, g_nrows, sizeof(uint32_t), mst_size_order);
    g_load_ms = mst_now_ms() - t0;
    InterlockedExchange(&g_state, MST_READY);
    {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MODS: stock: installed indexes read in %.1f ms (%u rows with bytes, %llu KiB held)", g_load_ms,
                    g_nrows, (unsigned long long)(g_nrows * (sizeof(mst_row) + 8ull) + g_names_len) >> 10);
        backend_log(line);
    }
    return;
fail:
    mst_free_table();
    InterlockedExchange(&g_state, MST_FAILED);
    {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: stock: installed content unavailable: %s", g_cause);
        backend_log(line);
    }
}

static int mst_ready(void)
{
    LONG s = InterlockedCompareExchange(&g_state, 0, 0);
    if (s != MST_NEW) return s == MST_READY;
    AcquireSRWLockExclusive(&g_lock);
    if (InterlockedCompareExchange(&g_state, 0, 0) == MST_NEW) mst_load_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return InterlockedCompareExchange(&g_state, 0, 0) == MST_READY;
}

/* ------------------------------------------------------------- slices ---- */

static HANDLE mst_archive(const mst_row *r)
{
    HANDLE h = g_files[r->container][r->selector];
    if (h) return h;
    AcquireSRWLockExclusive(&g_lock);
    h = g_files[r->container][r->selector];
    if (!h) {
        char path[MAX_PATH * 4 + 64];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\base\\%s.%s", g_root, MST_CONTAINER[r->container],
                    r->selector ? "patch" : "resources");
        h = mst_open_read(path, 1);
        g_files[r->container][r->selector] = h;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

/* The decoded bytes of one row (malloc'd), or NULL. */
static unsigned char *mst_slice(const mst_row *r, int heap)
{
    HANDLE h;
    unsigned char *out, *stored;
    if (r->size > SH_MODSTOCK_MAX_SLICE || r->zsize > SH_MODSTOCK_MAX_SLICE || !r->zsize) return NULL;
    h = mst_archive(r);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    out = (unsigned char *)mst_alloc(r->size, heap);
    if (!out) return NULL;
    if (r->zsize == r->size) {
        if (!mst_pread(h, r->offset, out, r->size)) { mst_free(out, heap); return NULL; }
        return out;
    }
    stored = (unsigned char *)malloc(r->zsize);
    if (!stored || !mst_pread(h, r->offset, stored, r->zsize) ||
        sh_inflate_raw(stored, r->zsize, out, r->size) != r->size) {
        free(stored);
        mst_free(out, heap);
        return NULL;
    }
    free(stored);
    return out;
}

/* Remember a row's SHA-256. */
static void mst_memo_put(uint32_t index, const unsigned char sha[32])
{
    InterlockedIncrement(&g_slices_hashed);
    InterlockedAdd64(&g_bytes_hashed, g_rows[index].size);
    AcquireSRWLockExclusive(&g_memo_lock);
    if (!g_sha) {
        g_sha = (unsigned char (*)[32])malloc((size_t)g_nrows * 32u);
        g_sha_done = (unsigned char *)calloc(g_nrows ? g_nrows : 1, 1);
        if (!g_sha || !g_sha_done) { free(g_sha); free(g_sha_done); g_sha = NULL; g_sha_done = NULL; }
    }
    if (g_sha) { memcpy(g_sha[index], sha, 32); g_sha_done[index] = 1; }
    ReleaseSRWLockExclusive(&g_memo_lock);
}

static int mst_memo_get(uint32_t index, unsigned char out[32])
{
    int done = 0;
    AcquireSRWLockShared(&g_memo_lock);
    if (g_sha_done && g_sha_done[index]) { memcpy(out, g_sha[index], 32); done = 1; }
    ReleaseSRWLockShared(&g_memo_lock);
    return done;
}

/* The row's SHA-256 (computed once). Returns 1, or 0 when it cannot be read. */
static int mst_row_sha(uint32_t index, unsigned char out[32])
{
    unsigned char *bytes;
    char hex[SH_MB_HASH_HEX + 1];
    if (mst_memo_get(index, out)) return 1;
    bytes = mst_slice(&g_rows[index], 0);
    if (!bytes) return 0;
    sh_mb_sha256_hex(bytes, g_rows[index].size, hex);
    free(bytes);
    mst_unhex(hex, out);
    mst_memo_put(index, out);
    return 1;
}

/* The first index in g_by_name whose row has this name's hash, and the count. */
static uint32_t mst_name_range(const char *name, uint32_t *count)
{
    uint64_t h = mst_fnv64(name, strlen(name));
    uint32_t lo = 0, hi = g_nrows, first;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_rows[g_by_name[mid]].hash < h) lo = mid + 1;
        else hi = mid;
    }
    first = lo;
    while (lo < g_nrows && g_rows[g_by_name[lo]].hash == h) lo++;
    *count = lo - first;
    return first;
}

/* ------------------------------------------------------------- loose files ---- */

static int mst_loose_path(const char *source, int which, char *out, size_t cap)
{
    char cause[SH_MB_CAUSE_MAX];
    char *p;
    if (!sh_mb_check_source(source, cause, sizeof cause)) return 0;
    if (_snprintf_s(out, cap, _TRUNCATE, which ? "%s\\%s" : "%s\\base\\%s", g_root, source) < 0) return 0;
    for (p = out; *p; p++) if (*p == '/') *p = '\\';
    return 1;
}

/* Size of a loose regular file, or -1. */
static long long mst_loose_size(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    int ok;
    if (!w) return -1;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok || (a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return -1;
    return (long long)(((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

static int mst_loose_sha(const char *path, char hex[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    if (!sh_mb_sha256_file(path, size, hex)) return 0;
    InterlockedAdd64(&g_bytes_hashed, (LONG64)*size);
    return 1;
}

/* --------------------------------------------------------------- the ops ---- */

static int mst_copy_source(const char *name, char *out, size_t cap)
{
    char cause[SH_MB_CAUSE_MAX];
    if (!sh_mb_check_source(name, cause, sizeof cause) || strlen(name) >= cap) return 0;
    strcpy_s(out, cap, name);
    return 1;
}

int sh_modstock_match(const char *request, unsigned long long size, const char *sha256,
                      char *source, size_t source_cap)
{
    unsigned char want[32], got[32];
    uint32_t first, count, i, candidates = 0;
    unsigned long long budget = 0;
    char path[MAX_PATH * 4 + 64];
    uint32_t ext;
    int which;
    if (source && source_cap) source[0] = 0;
    if (!request || !sha256 || !source || !mst_unhex(sha256, want) || !size) return SH_MB_STOCK_ABSENT;
    if (!mst_ready()) return SH_MB_STOCK_UNAVAILABLE;
    /* The file's own path: every row of that name and size. */
    first = mst_name_range(request, &count);
    for (i = first; i < first + count; i++) {
        uint32_t r = g_by_name[i];
        if (g_rows[r].size != size || !mst_name_eq(g_names + g_rows[r].name, request)) continue;
        if (mst_row_sha(r, got) && !memcmp(got, want, 32))
            return mst_copy_source(g_names + g_rows[r].name, source, source_cap) ? SH_MB_STOCK_OK : SH_MB_STOCK_ABSENT;
    }
    /* A loose file at that path. */
    for (which = 0; which < 2; which++) {
        char hex[SH_MB_HASH_HEX + 1];
        unsigned long long n = 0;
        if (!mst_loose_path(request, which, path, sizeof path) || mst_loose_size(path) != (long long)size) continue;
        if (mst_loose_sha(path, hex, &n) && n == size && !strcmp(hex, sha256))
            return mst_copy_source(request, source, source_cap) ? SH_MB_STOCK_OK : SH_MB_STOCK_ABSENT;
    }
    /* A renamed copy: rows of the same decoded size and extension. */
    if (size > 0xffffffffull) return SH_MB_STOCK_ABSENT;
    ext = mst_ext_hash(request, strlen(request));
    {
        uint32_t lo = 0, hi = g_nrows;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const mst_row *m = &g_rows[g_by_size[mid]];
            if (m->size < size || (m->size == size && m->ext < ext)) lo = mid + 1;
            else hi = mid;
        }
        for (i = lo; i < g_nrows; i++) {
            const mst_row *m = &g_rows[g_by_size[i]];
            uint32_t k;
            int seen = 0;
            if (m->size != size || m->ext != ext || i - lo >= 16u * SH_MODSTOCK_RENAMED_MAX) break;
            /* One slice can be named by several rows; hash it once. */
            for (k = lo; k < i && !seen; k++) {
                const mst_row *o = &g_rows[g_by_size[k]];
                seen = o->container == m->container && o->selector == m->selector && o->offset == m->offset;
            }
            if (seen) continue;
            if (++candidates > SH_MODSTOCK_RENAMED_MAX || budget + size > SH_MODSTOCK_RENAMED_BYTES) break;
            budget += size;
            if (mst_row_sha(g_by_size[i], got) && !memcmp(got, want, 32) &&
                mst_copy_source(g_names + m->name, source, source_cap))
                return SH_MB_STOCK_OK;
        }
    }
    return SH_MB_STOCK_ABSENT;
}

int sh_modstock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    uint32_t first, count, i;
    char path[MAX_PATH * 4 + 64];
    int which;
    if (sha256) sha256[0] = 0;
    if (size) *size = 0;
    if (!source || !sha256 || !size) return SH_MB_STOCK_ABSENT;
    if (!mst_ready()) return SH_MB_STOCK_UNAVAILABLE;
    first = mst_name_range(source, &count);
    for (i = first; i < first + count; i++) {
        uint32_t r = g_by_name[i];
        unsigned char d[32];
        if (!mst_name_eq(g_names + g_rows[r].name, source)) continue;
        if (!mst_row_sha(r, d)) continue;
        mst_hex(d, sha256);
        *size = g_rows[r].size;
        return SH_MB_STOCK_OK;
    }
    for (which = 0; which < 2; which++) {
        if (!mst_loose_path(source, which, path, sizeof path) || mst_loose_size(path) < 0) continue;
        if (mst_loose_sha(path, sha256, size)) return SH_MB_STOCK_OK;
    }
    return SH_MB_STOCK_ABSENT;
}

/* Find bytes of `source` hashing to `sha256`: the row index (>= 0), -2 for a
 * loose file (path filled), -1 when nothing matches; *exists tells whether
 * the source exists at all. */
static long long mst_locate(const char *source, const unsigned char want[32], const char *sha256,
                            char *loose, size_t loose_cap, int *exists, unsigned long long *size)
{
    uint32_t first, count, i;
    int which;
    *exists = 0;
    first = mst_name_range(source, &count);
    for (i = first; i < first + count; i++) {
        uint32_t r = g_by_name[i];
        unsigned char d[32];
        if (!mst_name_eq(g_names + g_rows[r].name, source)) continue;
        *exists = 1;
        if (mst_row_sha(r, d) && !memcmp(d, want, 32)) { *size = g_rows[r].size; return r; }
    }
    for (which = 0; which < 2; which++) {
        char hex[SH_MB_HASH_HEX + 1];
        unsigned long long n = 0;
        if (!mst_loose_path(source, which, loose, loose_cap) || mst_loose_size(loose) < 0) continue;
        *exists = 1;
        if (mst_loose_sha(loose, hex, &n) && !strcmp(hex, sha256)) { *size = n; return -2; }
    }
    return -1;
}

int sh_modstock_verify(const char *source, const char *sha256, unsigned long long *size)
{
    unsigned char want[32];
    char loose[MAX_PATH * 4 + 64];
    unsigned long long n = 0;
    int exists = 0;
    if (size) *size = 0;
    if (!source || !mst_unhex(sha256, want)) return SH_MB_STOCK_ABSENT;
    if (!mst_ready()) return SH_MB_STOCK_UNAVAILABLE;
    if (mst_locate(source, want, sha256, loose, sizeof loose, &exists, &n) == -1)
        return exists ? SH_MB_STOCK_MISMATCH : SH_MB_STOCK_ABSENT;
    if (size) *size = n;
    return SH_MB_STOCK_OK;
}

int sh_modstock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len)
{
    unsigned char want[32], got[32];
    char loose[MAX_PATH * 4 + 64], hex[SH_MB_HASH_HEX + 1];
    uint32_t first, count, i;
    int exists = 0, which;
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!source || !bytes || !len || !mst_unhex(sha256, want)) return SH_MB_STOCK_ABSENT;
    if (!mst_ready()) return SH_MB_STOCK_UNAVAILABLE;
    /* Each candidate row is decoded once: a remembered hash decides without
     * decoding; otherwise the decoded bytes are hashed and handed over. */
    first = mst_name_range(source, &count);
    for (i = first; i < first + count; i++) {
        uint32_t r = g_by_name[i];
        unsigned char *plain;
        int known;
        if (!mst_name_eq(g_names + g_rows[r].name, source)) continue;
        exists = 1;
        known = mst_memo_get(r, got);
        if (known && memcmp(got, want, 32) != 0) continue;
        plain = mst_slice(&g_rows[r], 1);
        if (!plain) continue;
        if (!known) {
            sh_mb_sha256_hex(plain, g_rows[r].size, hex);
            mst_unhex(hex, got);
            mst_memo_put(r, got);
            if (memcmp(got, want, 32) != 0) { mst_free(plain, 1); continue; }
        }
        *bytes = plain;
        *len = g_rows[r].size;
        InterlockedIncrement(&g_reads);
        return SH_MB_STOCK_OK;
    }
    /* A loose file is hashed on every read: it can change between reads. */
    for (which = 0; which < 2; which++) {
        unsigned char *plain;
        size_t n = 0;
        if (!mst_loose_path(source, which, loose, sizeof loose) || mst_loose_size(loose) < 0) continue;
        exists = 1;
        plain = mst_read_whole(loose, (size_t)SH_MODSTOCK_MAX_SLICE, &n, 1);
        if (!plain) continue;
        sh_mb_sha256_hex(plain, n, hex);
        InterlockedAdd64(&g_bytes_hashed, (LONG64)n);
        if (strcmp(hex, sha256) != 0) { mst_free(plain, 1); continue; }
        *bytes = plain;
        *len = n;
        InterlockedIncrement(&g_reads);
        return SH_MB_STOCK_OK;
    }
    return exists ? SH_MB_STOCK_MISMATCH : SH_MB_STOCK_ABSENT;
}

int sh_modstock_exists(const char *path)
{
    uint32_t first, count, i;
    char loose[MAX_PATH * 4 + 64];
    int which;
    if (!path || !path[0]) return 0;
    if (!mst_ready()) return -1;
    first = mst_name_range(path, &count);
    for (i = first; i < first + count; i++)
        if (mst_name_eq(g_names + g_rows[g_by_name[i]].name, path)) return 1;
    for (which = 0; which < 2; which++)
        if (mst_loose_path(path, which, loose, sizeof loose) && mst_loose_size(loose) >= 0) return 1;
    return 0;
}

/* ------------------------------------------------------- registration ---- */

static int op_match(void *c, const char *request, unsigned long long size, const char *sha, char *src, size_t cap)
{ (void)c; return sh_modstock_match(request, size, sha, src, cap); }
static int op_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{ (void)c; return sh_modstock_identify(source, sha, size); }
static int op_verify(void *c, const char *source, const char *sha, unsigned long long *size)
{ (void)c; return sh_modstock_verify(source, sha, size); }
static int op_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{ (void)c; return sh_modstock_read(source, sha, bytes, len); }

void sh_modstock_register(void)
{
    sh_mb_stock_ops ops;
    ops.context = NULL;
    ops.match = op_match;
    ops.identify = op_identify;
    ops.verify = op_verify;
    ops.read = op_read;
    sh_mb_set_stock_ops(&ops);
}

void sh_modstock_set_install(const char *doom_root)
{
    AcquireSRWLockExclusive(&g_lock);
    mst_free_table();
    if (doom_root && doom_root[0]) strncpy_s(g_root, sizeof g_root, doom_root, _TRUNCATE);
    else g_root[0] = 0;
    g_cause[0] = 0;
    InterlockedExchange(&g_state, MST_NEW);
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_modstock_get_stats(sh_modstock_stats *out)
{
    LONG s;
    if (!out) return;
    memset(out, 0, sizeof *out);
    s = InterlockedCompareExchange(&g_state, 0, 0);
    out->loaded = s == MST_READY ? 1 : s == MST_FAILED ? -1 : 0;
    if (s == MST_READY) {
        out->rows = g_nrows;
        out->table_bytes = (unsigned long long)g_nrows * (sizeof(mst_row) + 8u) + g_names_len;
        out->load_ms = g_load_ms;
    }
    out->slices_hashed = (unsigned)InterlockedCompareExchange(&g_slices_hashed, 0, 0);
    out->bytes_hashed = (unsigned long long)InterlockedCompareExchange64(&g_bytes_hashed, 0, 0);
    out->reads = (unsigned)InterlockedCompareExchange(&g_reads, 0, 0);
    strcpy_s(out->cause, sizeof out->cause, g_cause);
}

void sh_modstock_reset(void)
{
    sh_modstock_set_install(NULL);
    InterlockedExchange(&g_slices_hashed, 0);
    InterlockedExchange(&g_reads, 0);
    InterlockedExchange64(&g_bytes_hashed, 0);
}
