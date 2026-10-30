/* modstore.c -- see modstore.h. The mods\ blob store, bundle folders, install
 * and verification. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "modstore.h"
#include "backend_log.h"

#define MS_CHUNK (1u << 20)

/* ------------------------------------------------------------ lock ----- */

/* The store lock: one critical section for every writer of mods\ (installs,
 * downloads, the migration, exports, claims). It is recursive on purpose: the
 * install and verification below take it themselves, and their callers (the
 * migration pass, a download) already hold it around a larger step. */
static INIT_ONCE        g_ms_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_ms_lock;
static volatile LONG    g_ms_lock_waits;

static BOOL CALLBACK ms_lock_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_ms_lock);
    return TRUE;
}

void modstore_lock(void)
{
    InitOnceExecuteOnce(&g_ms_lock_once, ms_lock_init, NULL, NULL);
    if (TryEnterCriticalSection(&g_ms_lock)) return;
    /* Held by another thread: wait for it rather than fail. */
    InterlockedIncrement(&g_ms_lock_waits);
    backend_log("MODS: store: waiting for another store operation to finish (an install, a download or the migration)");
    EnterCriticalSection(&g_ms_lock);
}

void modstore_unlock(void)
{
    LeaveCriticalSection(&g_ms_lock);
}

unsigned modstore_lock_waits(void)
{
    return (unsigned)InterlockedCompareExchange(&g_ms_lock_waits, 0, 0);
}

static char g_ms_root[MAX_PATH * 4];
static int  g_ms_root_set;
static int  g_ms_force_copy;

#ifdef SH_MODSTORE_TESTING
void sh_ms_test_force_copy(int on) { g_ms_force_copy = on; }
#endif

static int ms_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

int sh_ms_set_root(const char *root)
{
    if (!root || !root[0]) { g_ms_root_set = 0; g_ms_root[0] = 0; return 1; }
    if (strlen(root) >= sizeof g_ms_root) return 0;
    strcpy_s(g_ms_root, sizeof g_ms_root, root);
    g_ms_root_set = 1;
    return 1;
}

int sh_ms_root(char *out, size_t cap)
{
    PWSTR known = NULL;
    char *base;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (g_ms_root_set) {
        if (strlen(g_ms_root) >= cap) return 0;
        strcpy_s(out, cap, g_ms_root);
        return 1;
    }
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) || !known) return 0;
    base = sh_mb_utf8(known);
    CoTaskMemFree(known);
    if (!base) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\mods", base);
    free(base);
    return out[0] != 0;
}

/* ------------------------------------------------------------ paths ---- */

static wchar_t *ms_wjoin(const wchar_t *dir, const wchar_t *name)
{
    size_t a = wcslen(dir), b = wcslen(name);
    wchar_t *p = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!p) return NULL;
    memcpy(p, dir, sizeof(wchar_t) * a);
    p[a] = L'\\';
    memcpy(p + a + 1, name, sizeof(wchar_t) * (b + 1));
    return p;
}

/* dir + "\" + a UTF-8 relative path ('/' or '\'). */
static wchar_t *ms_wjoin_utf8(const wchar_t *dir, const char *rel)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rel, -1, NULL, 0);
    wchar_t *w, *out;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rel, -1, w, n);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    out = ms_wjoin(dir, w);
    free(w);
    return out;
}

static wchar_t *ms_wroot(void)
{
    char root[MAX_PATH * 4];
    if (!sh_ms_root(root, sizeof root)) return NULL;
    return sh_mb_wpath(root);
}

/* Create every missing folder of a \\?\ path. */
static int ms_mkdirs(const wchar_t *path)
{
    size_t n = wcslen(path), i, start = 0;
    wchar_t *copy = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
    int ok = 1;
    if (!copy) return 0;
    memcpy(copy, path, sizeof(wchar_t) * (n + 1));
    if (wcsncmp(copy, L"\\\\?\\UNC\\", 8) == 0) {
        /* skip \\?\UNC\server\share\ */
        int seps = 0;
        for (start = 8; start < n && seps < 2; start++) if (copy[start] == L'\\') seps++;
    } else if (wcsncmp(copy, L"\\\\?\\", 4) == 0) {
        start = 7;   /* \\?\C:\ */
    }
    for (i = start; i <= n && ok; i++) {
        if (copy[i] == L'\\' || copy[i] == 0) {
            wchar_t keep = copy[i];
            copy[i] = 0;
            if (i > start && !CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
            copy[i] = keep;
        }
    }
    free(copy);
    return ok;
}

static int ms_mkdirs_parent(const wchar_t *file)
{
    const wchar_t *slash = wcsrchr(file, L'\\');
    wchar_t *dir;
    int ok;
    if (!slash) return 0;
    dir = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(slash - file + 1));
    if (!dir) return 0;
    memcpy(dir, file, sizeof(wchar_t) * (size_t)(slash - file));
    dir[slash - file] = 0;
    ok = ms_mkdirs(dir);
    free(dir);
    return ok;
}

/* `path` lies strictly inside `dir` (both long form). */
static int ms_inside(const wchar_t *path, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    return _wcsnicmp(path, dir, n) == 0 && path[n] == L'\\' && path[n + 1] != 0;
}

/* --------------------------------------------------------- deletion ---- */

#ifndef FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_FLAG_DELETE                   0x00000001
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS          0x00000002
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010
#endif
typedef struct ms_disposition_ex { DWORD Flags; } ms_disposition_ex;
#define MS_FILE_DISPOSITION_INFO_EX ((FILE_INFO_BY_HANDLE_CLASS)21)

/* Delete one file or link without changing a shared read-only attribute when
 * the system allows it; otherwise clear the attribute (callers re-protect
 * blobs afterwards). */
static int ms_delete_file(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ms_disposition_ex d;
        BOOL done;
        d.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                  FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        done = SetFileInformationByHandle(h, MS_FILE_DISPOSITION_INFO_EX, &d, sizeof d);
        CloseHandle(h);
        if (done) return 1;
    }
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

/* Remove a tree under the store root. Reparse points are removed, never
 * followed. Refuses anything outside the root, the root itself and blobs\. */
static int ms_rmtree(const wchar_t *path, const wchar_t *root)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern, *blobs;
    HANDLE h;
    int ok = 1;
    if (!ms_inside(path, root)) return 0;
    blobs = ms_wjoin(root, L"blobs");
    if (!blobs) return 0;
    if (_wcsicmp(path, blobs) == 0 || ms_inside(path, blobs)) { free(blobs); return 0; }
    free(blobs);
    pattern = ms_wjoin(path, L"*");
    if (!pattern) return 0;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            child = ms_wjoin(path, fd.cFileName);
            if (!child) { ok = 0; continue; }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (!ms_rmtree(child, root)) ok = 0;
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirectoryW(child)) ok = 0;
            } else if (!ms_delete_file(child)) {
                ok = 0;
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = 0;
    return ok;
}

static void ms_protect_blobs(const wchar_t *root, const sh_mb_bundle *b)
{
    size_t i;
    wchar_t *blobs = ms_wjoin(root, L"blobs");
    if (!blobs) return;
    for (i = 0; i < b->file_count; i++) {
        wchar_t *p = ms_wjoin_utf8(blobs, b->files[i].blob);
        if (p) {
            DWORD a = GetFileAttributesW(p);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_READONLY))
                SetFileAttributesW(p, a | FILE_ATTRIBUTE_READONLY);
            free(p);
        }
    }
    free(blobs);
}

/* -------------------------------------------------------- file utils ---- */

static int ms_file_size(const wchar_t *path, unsigned long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 1;
}

static int ms_hash_wfile(const wchar_t *path, unsigned long long *size, char out[SH_MB_HASH_HEX + 1])
{
    char *u = sh_mb_utf8(path);
    int ok;
    if (!u) return 0;
    ok = sh_mb_sha256_file(u, size, out);
    free(u);
    return ok;
}

static int ms_same_file(const wchar_t *a, const wchar_t *b)
{
    BY_HANDLE_FILE_INFORMATION x, y;
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    int same = 0;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(ha, &x) && GetFileInformationByHandle(hb, &y))
        same = x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
               x.nFileIndexLow == y.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

static int ms_write_text(const wchar_t *path, const char *text)
{
    size_t n = wcslen(path), len = strlen(text);
    wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 16));
    HANDLE h;
    DWORD put = 0;
    int ok;
    if (!tmp) return 0;
    _snwprintf_s(tmp, n + 16, _TRUNCATE, L"%s.tmp%lu", path, GetCurrentProcessId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(tmp); return 0; }
    ok = WriteFile(h, text, (DWORD)len, &put, NULL) && put == (DWORD)len;
    CloseHandle(h);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}

static long long ms_now(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

static int ms_read_state(const wchar_t *folder, int *validated, long long *at, char *cause, size_t cap)
{
    wchar_t *p = ms_wjoin(folder, L".state.json");
    char *u = p ? sh_mb_utf8(p) : NULL;
    size_t len = 0;
    unsigned char *text = u ? sh_mb_read_file(u, 65536, &len) : NULL;
    int ok = text && sh_mb_state_parse((const char *)text, len, validated, at, cause, cap);
    free(text);
    free(u);
    free(p);
    return ok;
}

static int ms_write_state(const wchar_t *folder, int validated, const char *cause, long long at)
{
    wchar_t *p = ms_wjoin(folder, L".state.json");
    char *text = sh_mb_state_format(validated, cause, at);
    int ok = p && text && ms_write_text(p, text);
    free(p);
    free(text);
    return ok;
}

/* ------------------------------------------------------- enumeration ---- */

int sh_ms_entry_usable(const sh_ms_entry *e)
{
    return e && e->loaded && e->sizes_ok && (!e->has_state || e->validated);
}

static sh_ms_entry *ms_add_entry(sh_ms_list *l)
{
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        sh_ms_entry *grown = (sh_ms_entry *)realloc(l->items, sizeof(sh_ms_entry) * cap);
        if (!grown) return NULL;
        l->items = grown;
        l->cap = cap;
    }
    memset(&l->items[l->count], 0, sizeof(sh_ms_entry));
    return &l->items[l->count++];
}

void sh_ms_list_free(sh_ms_list *l)
{
    size_t i;
    if (!l) return;
    for (i = 0; i < l->count; i++) {
        sh_mb_free(&l->items[i].bundle);
        free(l->items[i].folder);
    }
    free(l->items);
    memset(l, 0, sizeof *l);
}

static void ms_load_entry(sh_ms_list *l, const wchar_t *folder, const wchar_t *leaf, int legacy)
{
    wchar_t *bj = ms_wjoin(folder, L"bundle.json"), *dm = ms_wjoin(folder, L"mod.json"),
            *dp = ms_wjoin(folder, L"package.json");
    int has_bundle = bj && GetFileAttributesW(bj) != INVALID_FILE_ATTRIBUTES;
    int has_desc = (dm && GetFileAttributesW(dm) != INVALID_FILE_ATTRIBUTES) ||
                   (dp && GetFileAttributesW(dp) != INVALID_FILE_ATTRIBUTES);
    sh_ms_entry *e;
    char *u;
    size_t i;
    free(dm);
    free(dp);
    if (!has_bundle && !has_desc) { free(bj); return; }
    e = ms_add_entry(l);
    if (!e) { free(bj); return; }
    e->folder = sh_mb_utf8(folder);
    e->legacy = legacy;
    u = sh_mb_utf8(leaf);
    if (u) { strncpy_s(e->key, sizeof e->key, u, _TRUNCATE); free(u); }
    if (!has_bundle) {
        _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "not built yet (sh_mods install \"%s\")", e->key);
        free(bj);
        return;
    }
    u = sh_mb_utf8(bj);
    free(bj);
    e->loaded = u && sh_mb_load(u, &e->bundle);
    free(u);
    if (!e->loaded) {
        strncpy_s(e->cause, sizeof e->cause, e->bundle.first_cause[0] ? e->bundle.first_cause : "bundle.json unreadable",
                  _TRUNCATE);
        return;
    }
    _snprintf_s(e->key, sizeof e->key, _TRUNCATE, "%s@%s+%llu", e->bundle.id, e->bundle.version, e->bundle.build);
    e->sizes_ok = 1;
    for (i = 0; i < e->bundle.file_count; i++) {
        unsigned long long size = 0;
        wchar_t *p = ms_wjoin_utf8(folder, e->bundle.files[i].path);
        int present = p && ms_file_size(p, &size);
        free(p);
        if (!present || size != e->bundle.files[i].size) {
            e->sizes_ok = 0;
            _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "%s is %s", e->bundle.files[i].path,
                        present ? "not the size bundle.json records" : "missing");
            break;
        }
    }
    {
        char state_cause[SH_MB_CAUSE_MAX];
        e->has_state = ms_read_state(folder, &e->validated, &e->installed_at, state_cause, sizeof state_cause);
        if (e->has_state && !e->validated && e->sizes_ok)
            strncpy_s(e->cause, sizeof e->cause, state_cause[0] ? state_cause : "failed verification", _TRUNCATE);
    }
}

static void ms_scan_dir(sh_ms_list *l, const wchar_t *dir, int legacy)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h;
    if (!pattern) return;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t *child;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!legacy && (_wcsicmp(fd.cFileName, L"blobs") == 0 || _wcsicmp(fd.cFileName, L"legacy") == 0)) continue;
        if (wcsstr(fd.cFileName, L".staging-") || wcsstr(fd.cFileName, L".old-")) continue;
        child = ms_wjoin(dir, fd.cFileName);
        if (child) { ms_load_entry(l, child, fd.cFileName, legacy); free(child); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int ms_entry_cmp(const void *a, const void *b)
{
    const sh_ms_entry *x = (const sh_ms_entry *)a, *y = (const sh_ms_entry *)b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    return strcmp(x->folder ? x->folder : "", y->folder ? y->folder : "");
}

int sh_ms_enumerate(sh_ms_list *out)
{
    wchar_t *root, *legacy;
    DWORD a;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    root = ms_wroot();
    if (!root) return 0;
    a = GetFileAttributesW(root);
    if (a == INVALID_FILE_ATTRIBUTES) { free(root); return 1; }
    ms_scan_dir(out, root, 0);
    legacy = ms_wjoin(root, L"legacy");
    if (legacy) { ms_scan_dir(out, legacy, 1); free(legacy); }
    free(root);
    if (out->count > 1) qsort(out->items, out->count, sizeof(sh_ms_entry), ms_entry_cmp);
    return 1;
}

/* ---------------------------------------------------------- resolve ---- */

static int ms_file_matches(const sh_mb_file *f, const char *type, const char *name,
                           char cand[][SH_MB_REQUEST_MAX + 1], int ncand)
{
    int i;
    if (f->ptype && f->pname && _stricmp(f->ptype, type) == 0 && _stricmp(f->pname, name) == 0) return 1;
    for (i = 0; i < ncand; i++) if (strcmp(f->request, cand[i]) == 0) return 1;
    return 0;
}

int sh_ms_resolve_need(const sh_ms_list *l, const char *type, const char *name, sh_ms_hit *hit)
{
    char cand[2][SH_MB_REQUEST_MAX + 1];
    int ncand;
    size_t i, k;
    const sh_mb_file *first = NULL;
    int differ = 0, decl = 0;
    sh_ms_hit h;
    memset(&h, 0, sizeof h);
    h.status = SH_MS_MISS;
    if (!l || !type || !name) { if (hit) *hit = h; return SH_MS_MISS; }
    ncand = sh_mb_need_requests(type, name, cand, 2);
    if (ncand < 0) ncand = 0;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!sh_ms_entry_usable(e)) continue;
        for (k = 0; k < e->bundle.file_count; k++) {
            const sh_mb_file *f = &e->bundle.files[k];
            if (!ms_file_matches(f, type, name, cand, ncand)) continue;
            if (!first) {
                first = f;
                h.entry = i;
                h.file = k;
                decl = f->is_decl;
            } else {
                if (h.providers == 1) h.other_entry = i;
                if (f->is_decl || strcmp(f->blob, first->blob) != 0) differ = 1;
                if (f->is_decl) decl = 1;
            }
            h.providers++;
            break;   /* one provider per bundle */
        }
    }
    if (!first) h.status = SH_MS_MISS;
    else if (h.providers == 1) h.status = SH_MS_HIT;
    else h.status = (decl || differ) ? SH_MS_CONFLICT : SH_MS_DEDUPE;
    if (hit) *hit = h;
    return h.status;
}

int sh_ms_request_exists(void *ctx, const char *request)
{
    const sh_ms_list *l = (const sh_ms_list *)ctx;
    size_t i, k;
    if (!l || !request) return 0;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!sh_ms_entry_usable(e)) continue;
        for (k = 0; k < e->bundle.file_count; k++)
            if (strcmp(e->bundle.files[k].request, request) == 0) return 1;
    }
    return 0;
}

/* ----------------------------------------------------------- verify ---- */

/* Rehash every file of one bundle folder, and its blob when the blob is a
 * separate file. Writes .state.json. */
static int ms_verify_folder(const wchar_t *root, const wchar_t *folder, const sh_mb_bundle *b,
                            sh_ms_verify_stats *st, char *cause, size_t cap)
{
    wchar_t *blobs = ms_wjoin(root, L"blobs");
    char first[SH_MB_CAUSE_MAX];
    int ok = 1, validated_before = 0;
    long long at = 0;
    unsigned long long bytes = 0;
    size_t i;
    first[0] = 0;
    if (!blobs) return ms_cause(cause, cap, "out of memory");
    for (i = 0; i < b->file_count && ok; i++) {
        const sh_mb_file *f = &b->files[i];
        wchar_t *p = ms_wjoin_utf8(folder, f->path), *bp = ms_wjoin_utf8(blobs, f->blob);
        char hex[SH_MB_HASH_HEX + 1];
        unsigned long long size = 0;
        if (!p || !bp) { ok = 0; strcpy_s(first, sizeof first, "out of memory"); }
        else if (!ms_hash_wfile(p, &size, hex)) {
            ok = 0;
            _snprintf_s(first, sizeof first, _TRUNCATE, "%s is missing or unreadable", f->path);
        } else if (size != f->size || strcmp(hex, f->blob) != 0) {
            ok = 0;
            _snprintf_s(first, sizeof first, _TRUNCATE, "%s does not match bundle.json (damaged or edited)", f->path);
        } else if (GetFileAttributesW(bp) == INVALID_FILE_ATTRIBUTES) {
            ok = 0;
            _snprintf_s(first, sizeof first, _TRUNCATE, "blob %.16s for %s is missing from the store", f->blob, f->path);
        } else if (!ms_same_file(p, bp)) {
            if (!ms_hash_wfile(bp, &size, hex) || size != f->size || strcmp(hex, f->blob) != 0) {
                ok = 0;
                _snprintf_s(first, sizeof first, _TRUNCATE, "blob %.16s for %s is damaged", f->blob, f->path);
            }
            if (st) st->blobs_checked++;
        }
        if (ok) { bytes += f->size; if (st) st->files++; }
        free(p);
        free(bp);
    }
    free(blobs);
    if (st) st->bytes_mb += (unsigned)(bytes >> 20);
    if (!ms_read_state(folder, &validated_before, &at, NULL, 0) || at == 0) at = ms_now();
    ms_write_state(folder, ok, ok ? "" : first, at);
    if (!ok) return ms_cause(cause, cap, "%s", first);
    return 1;
}

static int ms_verify_locked(const char *key, sh_ms_verify_stats *st, char *cause, size_t cap);

int sh_ms_verify(const char *key, sh_ms_verify_stats *st, char *cause, size_t cap)
{
    int ok;
    modstore_lock();
    ok = ms_verify_locked(key, st, cause, cap);
    modstore_unlock();
    return ok;
}

static int ms_verify_locked(const char *key, sh_ms_verify_stats *st, char *cause, size_t cap)
{
    sh_ms_list l;
    wchar_t *root;
    size_t i, matched = 0;
    int ok = 1;
    char first[SH_MB_CAUSE_MAX];
    first[0] = 0;
    if (st) memset(st, 0, sizeof *st);
    if (!key || !key[0]) return ms_cause(cause, cap, "no bundle named (use id@version+build or id@version)");
    root = ms_wroot();
    if (!root) return ms_cause(cause, cap, "the store folder cannot be resolved");
    if (!sh_ms_enumerate(&l)) { free(root); return ms_cause(cause, cap, "the store cannot be listed"); }
    for (i = 0; i < l.count; i++) {
        sh_ms_entry *e = &l.items[i];
        wchar_t *folder;
        char one[SH_MB_CAUSE_MAX];
        if (_stricmp(e->key, key) != 0) {
            /* "id@version" names every installed build of that version. */
            size_t kl = strlen(key);
            if (strchr(key, '+') || _strnicmp(e->key, key, kl) != 0 || e->key[kl] != '+') continue;
        }
        matched++;
        if (st) st->entries++;
        if (!e->loaded) {
            if (ok) strncpy_s(first, sizeof first, e->cause, _TRUNCATE);
            ok = 0;
            continue;
        }
        folder = sh_mb_wpath(e->folder);
        if (!folder || !ms_verify_folder(root, folder, &e->bundle, st, one, sizeof one)) {
            if (ok) strncpy_s(first, sizeof first, folder ? one : "out of memory", _TRUNCATE);
            ok = 0;
        }
        free(folder);
    }
    sh_ms_list_free(&l);
    free(root);
    if (!matched) return ms_cause(cause, cap, "no installed bundle is named %s", key);
    if (!ok) return ms_cause(cause, cap, "%s", first);
    return 1;
}

/* ---------------------------------------------------------- install ---- */

typedef struct ms_chain {
    const sh_mb_resolver *user;
    sh_ms_list *list;
} ms_chain;

static int ms_chain_installed(void *ctx, const char *request)
{
    ms_chain *c = (ms_chain *)ctx;
    if (sh_ms_request_exists(c->list, request) == 1) return 1;
    if (c->user && c->user->installed) return c->user->installed(c->user->installed_context, request);
    return 0;
}

/* Put one source file into blobs\<hash>, write-once. */
static int ms_put_blob(const wchar_t *blobs, const wchar_t *src, const sh_mb_file *f,
                       sh_ms_install_stats *st, char *cause, size_t cap)
{
    wchar_t *dst = ms_wjoin_utf8(blobs, f->blob), *tmp;
    unsigned long long size = 0;
    char hex[SH_MB_HASH_HEX + 1];
    size_t n;
    if (!dst) return ms_cause(cause, cap, "out of memory");
    if (ms_file_size(dst, &size)) {
        free(dst);
        if (size != f->size)
            return ms_cause(cause, cap, "blob %.16s in the store has the wrong size; it is damaged (run sh_mods verify)", f->blob);
        st->blobs_reused++;
        return 1;
    }
    n = wcslen(dst);
    tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 24));
    if (!tmp) { free(dst); return ms_cause(cause, cap, "out of memory"); }
    _snwprintf_s(tmp, n + 24, _TRUNCATE, L"%s.tmp-%lu", dst, GetCurrentProcessId());
    ms_delete_file(tmp);
    if (!CopyFileW(src, tmp, FALSE)) {
        DWORD err = GetLastError();
        free(tmp); free(dst);
        return ms_cause(cause, cap, "cannot copy %s into the store (error %lu)", f->path, err);
    }
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    /* Hash the copy itself: the source may change between build and copy. */
    if (!ms_hash_wfile(tmp, &size, hex) || size != f->size || strcmp(hex, f->blob) != 0) {
        ms_delete_file(tmp);
        free(tmp); free(dst);
        return ms_cause(cause, cap, "%s changed while it was installed; build again", f->path);
    }
    if (!MoveFileExW(tmp, dst, 0)) {
        DWORD err = GetLastError();
        ms_delete_file(tmp);
        free(tmp);
        if (err == ERROR_ALREADY_EXISTS && ms_file_size(dst, &size) && size == f->size) {
            free(dst);
            st->blobs_reused++;
            return 1;
        }
        free(dst);
        return ms_cause(cause, cap, "cannot place blob %.16s (error %lu)", f->blob, err);
    }
    SetFileAttributesW(dst, FILE_ATTRIBUTE_READONLY);
    free(tmp);
    free(dst);
    st->blobs_written++;
    return 1;
}

static int ms_lay_out(const wchar_t *blobs, const wchar_t *dir, const sh_mb_bundle *b,
                      sh_ms_install_stats *st, char *cause, size_t cap)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        wchar_t *dst = ms_wjoin_utf8(dir, f->path), *blob = ms_wjoin_utf8(blobs, f->blob);
        int ok = dst && blob && ms_mkdirs_parent(dst);
        if (ok && !g_ms_force_copy && CreateHardLinkW(dst, blob, NULL)) {
            st->linked++;
        } else if (ok && CopyFileW(blob, dst, TRUE)) {
            /* A copy keeps the blob's read-only attribute, which guards it the same way. */
            st->copied++;
        } else {
            DWORD err = GetLastError();
            free(dst); free(blob);
            return ms_cause(cause, cap, "cannot place %s (error %lu)", f->path, err);
        }
        free(dst);
        free(blob);
    }
    return 1;
}

static int ms_install_locked(const char *folder, const sh_mb_resolver *resolver,
                             char *key_out, size_t key_cap, sh_ms_install_stats *stats,
                             sh_mb_bundle *bundle_out, char *cause, size_t cap);

/* Every install runs under the store lock: two installs share the staging and
 * blob temporary names (both are named by process id). */
int sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                              char *key_out, size_t key_cap, sh_ms_install_stats *stats,
                              sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    int ok;
    modstore_lock();
    ok = ms_install_locked(folder, resolver, key_out, key_cap, stats, bundle_out, cause, cap);
    modstore_unlock();
    return ok;
}

static int ms_install_locked(const char *folder, const sh_mb_resolver *resolver,
                             char *key_out, size_t key_cap, sh_ms_install_stats *stats,
                             sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_ms_install_stats st_local;
    sh_ms_install_stats *st = stats ? stats : &st_local;
    sh_mb_bundle b;
    sh_mb_resolver chain_resolver;
    ms_chain chain;
    sh_ms_list list;
    wchar_t *root = NULL, *blobs = NULL, *src = NULL, *final = NULL, *staging = NULL, *old = NULL;
    char key[SH_MS_KEY_MAX + 1], one[SH_MB_CAUSE_MAX];
    int ok = 0, in_place, have_list;
    size_t i;

    memset(st, 0, sizeof *st);
    memset(&b, 0, sizeof b);
    if (key_out && key_cap) key_out[0] = 0;
    if (bundle_out) memset(bundle_out, 0, sizeof *bundle_out);
    root = ms_wroot();
    src = folder ? sh_mb_wpath(folder) : NULL;
    if (!root || !src) { ms_cause(cause, cap, root ? "no mod folder given" : "the store folder cannot be resolved"); goto done; }
    blobs = ms_wjoin(root, L"blobs");
    if (!blobs || !ms_mkdirs(blobs)) { ms_cause(cause, cap, "cannot create the store folder (error %lu)", GetLastError()); goto done; }
    if (_wcsicmp(src, root) == 0 || _wcsicmp(src, blobs) == 0 || ms_inside(src, blobs)) {
        ms_cause(cause, cap, "that folder is part of the store itself, not a mod");
        goto done;
    }
    in_place = ms_inside(src, root);
    st->in_place = in_place;

    have_list = sh_ms_enumerate(&list);
    if (!have_list) memset(&list, 0, sizeof list);
    chain.user = resolver;
    chain.list = &list;
    chain_resolver.installed = ms_chain_installed;
    chain_resolver.installed_context = &chain;
    chain_resolver.stock = resolver ? resolver->stock : NULL;
    chain_resolver.stock_context = resolver ? resolver->stock_context : NULL;
    if (!sh_mb_build(folder, &chain_resolver, &b)) {
        sh_ms_list_free(&list);
        ms_cause(cause, cap, "%s", b.first_cause);
        if (in_place) ms_write_state(src, 0, b.first_cause, ms_now());
        goto done;
    }
    sh_ms_list_free(&list);
    _snprintf_s(key, sizeof key, _TRUNCATE, "%s@%s+%llu", b.id, b.version, b.build);
    if (!sh_mb_write(&b, folder, one, sizeof one)) { ms_cause(cause, cap, "%s", one); goto done; }

    for (i = 0; i < b.file_count; i++) {
        wchar_t *p = ms_wjoin_utf8(src, b.files[i].path);
        int put;
        if (!p) { ms_cause(cause, cap, "out of memory"); goto done; }
        put = ms_put_blob(blobs, p, &b.files[i], st, one, sizeof one);
        free(p);
        if (!put) { ms_cause(cause, cap, "%s", one); goto done; }
    }

    if (in_place) {
        final = (wchar_t *)malloc(sizeof(wchar_t) * (wcslen(src) + 1));
        if (!final) { ms_cause(cause, cap, "out of memory"); goto done; }
        wcscpy_s(final, wcslen(src) + 1, src);
    } else {
        wchar_t suffix[40];
        sh_mb_bundle prior;
        int had_prior = 0;
        final = ms_wjoin_utf8(root, key);
        if (!final) { ms_cause(cause, cap, "out of memory"); goto done; }
        _snwprintf_s(suffix, 40, _TRUNCATE, L".staging-%lu", GetCurrentProcessId());
        staging = (wchar_t *)malloc(sizeof(wchar_t) * (wcslen(final) + 41));
        old = (wchar_t *)malloc(sizeof(wchar_t) * (wcslen(final) + 41));
        if (!staging || !old) { ms_cause(cause, cap, "out of memory"); goto done; }
        _snwprintf_s(staging, wcslen(final) + 41, _TRUNCATE, L"%s%s", final, suffix);
        _snwprintf_s(old, wcslen(final) + 41, _TRUNCATE, L"%s.old-%lu", final, GetCurrentProcessId());
        if (GetFileAttributesW(staging) != INVALID_FILE_ATTRIBUTES) ms_rmtree(staging, root);
        if (!ms_mkdirs(staging)) { ms_cause(cause, cap, "cannot create %s (error %lu)", key, GetLastError()); goto done; }
        {
            char *su = sh_mb_utf8(staging);
            int wrote = su && sh_mb_write(&b, su, one, sizeof one);
            free(su);
            if (!wrote) { ms_rmtree(staging, root); ms_cause(cause, cap, "%s", one); goto done; }
        }
        if (!ms_lay_out(blobs, staging, &b, st, one, sizeof one)) {
            ms_rmtree(staging, root);
            ms_protect_blobs(root, &b);
            ms_cause(cause, cap, "%s", one);
            goto done;
        }
        ms_write_state(staging, 0, "not verified yet", ms_now());
        memset(&prior, 0, sizeof prior);
        if (GetFileAttributesW(final) != INVALID_FILE_ATTRIBUTES) {
            wchar_t *pj = ms_wjoin(final, L"bundle.json");
            char *pu = pj ? sh_mb_utf8(pj) : NULL;
            had_prior = pu && sh_mb_load(pu, &prior);
            free(pu);
            free(pj);
            if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES) ms_rmtree(old, root);
            if (!MoveFileExW(final, old, 0)) {
                DWORD err = GetLastError();
                ms_rmtree(staging, root);
                ms_protect_blobs(root, &b);
                sh_mb_free(&prior);
                ms_cause(cause, cap, "cannot replace the installed %s; is a file in it open? (error %lu)", key, err);
                goto done;
            }
            st->replaced = 1;
        }
        if (!MoveFileExW(staging, final, 0)) {
            DWORD err = GetLastError();
            if (st->replaced) MoveFileExW(old, final, 0);
            ms_rmtree(staging, root);
            ms_protect_blobs(root, &b);
            sh_mb_free(&prior);
            ms_cause(cause, cap, "cannot place %s (error %lu)", key, err);
            goto done;
        }
        if (st->replaced) {
            ms_rmtree(old, root);
            if (had_prior) ms_protect_blobs(root, &prior);
            ms_protect_blobs(root, &b);
            backend_log("MODS: replaced an earlier build of the same id@version");
        }
        sh_mb_free(&prior);
    }

    {
        sh_ms_verify_stats vs;
        memset(&vs, 0, sizeof vs);
        if (!ms_verify_folder(root, final, &b, &vs, one, sizeof one)) {
            ms_cause(cause, cap, "installed, but verification failed: %s", one);
            goto done;
        }
        /* The state now records the install time of this build. */
        ms_write_state(final, 1, "", ms_now());
    }
    if (key_out) strncpy_s(key_out, key_cap, key, _TRUNCATE);
    ok = 1;
done:
    if (bundle_out) { *bundle_out = b; memset(&b, 0, sizeof b); }
    sh_mb_free(&b);
    free(root); free(blobs); free(src); free(final); free(staging); free(old);
    return ok;
}
