/* modstore.c -- see modstore.h. The mods\ folders, install, verification,
 * and the rebuild rule. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "modstore.h"
#include "config_json.h"
#include "backend_log.h"

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

static void ms_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
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

/* `path` is a direct child of `dir`. */
static int ms_child_of(const wchar_t *path, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    return ms_inside(path, dir) && !wcschr(path + n + 1, L'\\');
}

static const char *ms_leaf(const char *folder)
{
    const char *a = strrchr(folder ? folder : "", '\\'), *b = strrchr(folder ? folder : "", '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : (folder ? folder : "");
}

/* --------------------------------------------------------- deletion ---- */

#ifndef FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_FLAG_DELETE                   0x00000001
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS          0x00000002
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010
#endif
typedef struct ms_disposition_ex { DWORD Flags; } ms_disposition_ex;
#define MS_FILE_DISPOSITION_INFO_EX ((FILE_INFO_BY_HANDLE_CLASS)21)

/* Delete one file or link, read-only or not. */
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
 * followed. Refuses anything outside the root and the root itself. */
static int ms_rmtree(const wchar_t *path, const wchar_t *root)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    int ok = 1;
    if (!ms_inside(path, root)) return 0;
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
    SetFileAttributesW(path, FILE_ATTRIBUTE_DIRECTORY);
    if (!RemoveDirectoryW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = 0;
    return ok;
}

int sh_ms_remove_folder(const char *folder)
{
    wchar_t *root = ms_wroot(), *w = folder ? sh_mb_wpath(folder) : NULL;
    int ok = root && w && ms_rmtree(w, root);
    free(root);
    free(w);
    return ok;
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

static int ms_exists(const wchar_t *path)
{
    return path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int ms_is_dir(const wchar_t *path)
{
    DWORD a = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* `leaf` exists in `dir`. */
static int ms_has_file(const wchar_t *dir, const wchar_t *leaf)
{
    wchar_t *p = ms_wjoin(dir, leaf);
    int has = ms_exists(p);
    free(p);
    return has;
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

/* Hard links to this file, or 0 when it cannot be opened. */
static DWORD ms_link_count(const wchar_t *path)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandle(h, &info)) n = info.nNumberOfLinks;
    CloseHandle(h);
    return n;
}

int sh_ms_copy_file(const wchar_t *from, const wchar_t *to)
{
    size_t n;
    wchar_t *tmp;
    int ok;
    if (!from || !to) return 0;
    n = wcslen(to);
    tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 24));
    if (!tmp) return 0;
    _snwprintf_s(tmp, n + 24, _TRUNCATE, L"%s.tmp-%lu", to, GetCurrentProcessId());
    ms_delete_file(tmp);
    ok = ms_mkdirs_parent(to) && CopyFileW(from, tmp, FALSE);
    if (ok) {
        /* A copy of a read-only blob would stay read-only; mod files are the author's. */
        SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
        if (ms_exists(to)) ms_delete_file(to);
        ok = MoveFileExW(tmp, to, MOVEFILE_REPLACE_EXISTING);
    }
    if (!ok) ms_delete_file(tmp);
    free(tmp);
    return ok;
}

/* Read a small file whole (UTF-8 result is raw bytes, NUL-terminated). */
static char *ms_read_text(const wchar_t *path, size_t max, size_t *len)
{
    char *u = path ? sh_mb_utf8(path) : NULL;
    unsigned char *t = u ? sh_mb_read_file(u, max, len) : NULL;
    free(u);
    return (char *)t;
}

static int ms_write_text(const wchar_t *path, const char *text, size_t len)
{
    size_t n = wcslen(path);
    wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 16));
    HANDLE h;
    DWORD put = 0;
    int ok;
    if (!tmp) return 0;
    _snwprintf_s(tmp, n + 16, _TRUNCATE, L"%s.tmp%lu", path, GetCurrentProcessId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(tmp); return 0; }
    ok = WriteFile(h, text, (DWORD)len, &put, NULL) && put == (DWORD)len && FlushFileBuffers(h);
    CloseHandle(h);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}

/* ------------------------------------------------------ build causes ---- */

/* The cause of the last refused build per folder, for this process. */
typedef struct ms_bcause { char *folder; char cause[SH_MB_CAUSE_MAX]; } ms_bcause;
static ms_bcause *g_bcause;
static size_t g_bcause_count, g_bcause_cap;

static void ms_note_cause(const char *folder, const char *cause)
{
    size_t i;
    modstore_lock();
    for (i = 0; i < g_bcause_count; i++)
        if (!_stricmp(g_bcause[i].folder, folder)) {
            if (cause && cause[0]) strncpy_s(g_bcause[i].cause, sizeof g_bcause[i].cause, cause, _TRUNCATE);
            else {
                free(g_bcause[i].folder);
                g_bcause[i] = g_bcause[--g_bcause_count];
            }
            modstore_unlock();
            return;
        }
    if (cause && cause[0]) {
        if (g_bcause_count == g_bcause_cap) {
            size_t cap = g_bcause_cap ? g_bcause_cap * 2 : 16;
            ms_bcause *grown = (ms_bcause *)realloc(g_bcause, sizeof *grown * cap);
            if (!grown) { modstore_unlock(); return; }
            g_bcause = grown;
            g_bcause_cap = cap;
        }
        g_bcause[g_bcause_count].folder = _strdup(folder);
        if (g_bcause[g_bcause_count].folder) {
            strncpy_s(g_bcause[g_bcause_count].cause, sizeof g_bcause[g_bcause_count].cause, cause, _TRUNCATE);
            g_bcause_count++;
        }
    }
    modstore_unlock();
}

void sh_ms_build_cause(const char *folder, char *out, size_t cap)
{
    size_t i;
    if (!out || !cap) return;
    out[0] = 0;
    if (!folder) return;
    modstore_lock();
    for (i = 0; i < g_bcause_count; i++)
        if (!_stricmp(g_bcause[i].folder, folder)) { strncpy_s(out, cap, g_bcause[i].cause, _TRUNCATE); break; }
    modstore_unlock();
}

/* ------------------------------------------------------- enumeration ---- */

int sh_ms_entry_usable(const sh_ms_entry *e)
{
    return e && e->loaded && e->sizes_ok && e->validated;
}

int sh_ms_entry_matches(const sh_ms_entry *e, const char *spec)
{
    size_t n;
    if (!e || !spec || !spec[0]) return 0;
    if (!_stricmp(e->name, spec) || !_stricmp(e->key, spec)) return 1;
    if (!e->loaded) return 0;
    if (!_stricmp(e->bundle.id, spec)) return 1;
    n = strlen(e->bundle.id);
    return !_strnicmp(spec, e->bundle.id, n) && spec[n] == '@' && !_stricmp(spec + n + 1, e->bundle.version);
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

/* Folders of the store root that can be mods: not hidden, not a staging or
 * replaced copy, not the retired blobs\ and legacy\ (the retired layout). */
static int ms_candidate(const WIN32_FIND_DATAW *fd)
{
    if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (fd->cFileName[0] == L'.') return 0;
    if (!_wcsicmp(fd->cFileName, L"blobs") || !_wcsicmp(fd->cFileName, L"legacy")) return 0;
    if (wcsstr(fd->cFileName, L".staging-") || wcsstr(fd->cFileName, L".old-")) return 0;
    return 1;
}

static int ms_has_descriptor(const wchar_t *folder)
{
    wchar_t *dm = ms_wjoin(folder, L"mod.json"), *dp = ms_wjoin(folder, L"package.json");
    int has = ms_exists(dm) || ms_exists(dp);
    free(dm);
    free(dp);
    return has;
}

static void ms_load_entry(sh_ms_list *l, const wchar_t *folder, const wchar_t *leaf)
{
    sh_ms_entry *e;
    char *u;
    size_t i;
    if (!ms_has_descriptor(folder)) return;
    e = ms_add_entry(l);
    if (!e) return;
    e->folder = sh_mb_utf8(folder);
    u = sh_mb_utf8(leaf);
    if (u) {
        strncpy_s(e->name, sizeof e->name, u, _TRUNCATE);
        strncpy_s(e->key, sizeof e->key, u, _TRUNCATE);
        free(u);
    }
    e->loaded = e->folder && sh_mb_load_folder(e->folder, &e->bundle);
    if (!e->loaded) {
        char built[SH_MB_CAUSE_MAX];
        sh_ms_build_cause(e->folder, built, sizeof built);
        if (built[0]) strncpy_s(e->cause, sizeof e->cause, built, _TRUNCATE);
        else if (strstr(e->bundle.first_cause, "not been built") || !ms_has_file(folder, L"mod.json"))
            _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "not built yet (a rescan of the Mods tab or "
                        "'sh_mods build \"%s\"' builds it)", e->name);
        else strncpy_s(e->cause, sizeof e->cause, e->bundle.first_cause[0] ? e->bundle.first_cause : "mod.json unreadable",
                       _TRUNCATE);
        return;
    }
    _snprintf_s(e->key, sizeof e->key, _TRUNCATE, "%s@%s+%llu", e->bundle.id, e->bundle.version, e->bundle.build);
    e->delivered = e->bundle.delivered;
    e->validated = e->bundle.validated;
    e->installed_at = e->bundle.built;
    e->sizes_ok = 1;
    for (i = 0; i < e->bundle.file_count; i++) {
        unsigned long long size = 0;
        wchar_t *p;
        int present;
        if (e->bundle.files[i].stock) continue;   /* installed content: checked at install and verify */
        p = ms_wjoin_utf8(folder, e->bundle.files[i].path);
        present = p && ms_file_size(p, &size);
        free(p);
        if (!present || size != e->bundle.files[i].size) {
            e->sizes_ok = 0;
            _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "%s is %s", e->bundle.files[i].path,
                        present ? "not the size mod.json records" : "missing");
            break;
        }
    }
    if (!e->validated && e->sizes_ok)
        strncpy_s(e->cause, sizeof e->cause, e->bundle.state_cause[0] ? e->bundle.state_cause : "failed verification",
                  _TRUNCATE);
}

static int ms_entry_cmp(const void *a, const void *b)
{
    const sh_ms_entry *x = (const sh_ms_entry *)a, *y = (const sh_ms_entry *)b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    return _stricmp(x->name, y->name);
}

int sh_ms_enumerate(sh_ms_list *out)
{
    WIN32_FIND_DATAW fd;
    wchar_t *root, *pattern;
    HANDLE h;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    root = ms_wroot();
    if (!root) return 0;
    if (!ms_exists(root)) { free(root); return 1; }
    pattern = ms_wjoin(root, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (!ms_candidate(&fd)) continue;
            child = ms_wjoin(root, fd.cFileName);
            if (child) { ms_load_entry(out, child, fd.cFileName); free(child); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
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

/* Rehash every carried file of one folder; resolve every pointer. */
static int ms_verify_folder(const wchar_t *folder, const sh_mb_bundle *b, sh_ms_verify_stats *st,
                            char *cause, size_t cap)
{
    char first[SH_MB_CAUSE_MAX];
    int ok = 1;
    unsigned long long bytes = 0;
    size_t i;
    first[0] = 0;
    for (i = 0; i < b->file_count && ok; i++) {
        const sh_mb_file *f = &b->files[i];
        wchar_t *p;
        char hex[SH_MB_HASH_HEX + 1];
        unsigned long long size = 0;
        if (f->stock) {
            /* A stock pointer resolves against this player's installed game:
             * a different revision lacks the entry or holds other bytes. */
            if (sh_mb_stock_verify(f->stock_source, f->blob, &size, f->path, first, sizeof first) != SH_MB_STOCK_OK) ok = 0;
            else if (st) { st->files++; st->pointers++; }
            continue;
        }
        p = ms_wjoin_utf8(folder, f->path);
        if (!p) { ok = 0; strcpy_s(first, sizeof first, "out of memory"); }
        else if (!ms_hash_wfile(p, &size, hex)) {
            ok = 0;
            _snprintf_s(first, sizeof first, _TRUNCATE, "%s is missing or unreadable", f->path);
        } else if (size != f->size || strcmp(hex, f->blob) != 0) {
            ok = 0;
            _snprintf_s(first, sizeof first, _TRUNCATE, "%s does not match mod.json (damaged or edited)", f->path);
        }
        if (ok) { bytes += f->size; if (st) st->files++; }
        free(p);
    }
    if (st) st->bytes_mb += (unsigned)(bytes >> 20);
    if (!ok) return ms_cause(cause, cap, "%s", first);
    return 1;
}

int sh_ms_verify(const char *spec, sh_ms_verify_stats *st, char *cause, size_t cap)
{
    sh_ms_list l;
    size_t i, matched = 0;
    int ok = 1;
    char first[SH_MB_CAUSE_MAX];
    first[0] = 0;
    if (st) memset(st, 0, sizeof *st);
    if (!spec || !spec[0]) return ms_cause(cause, cap, "no mod named (use its folder, id, id@version or id@version+build)");
    modstore_lock();
    if (!sh_ms_enumerate(&l)) { modstore_unlock(); return ms_cause(cause, cap, "the store cannot be listed"); }
    for (i = 0; i < l.count; i++) {
        sh_ms_entry *e = &l.items[i];
        wchar_t *folder;
        char one[SH_MB_CAUSE_MAX], why[SH_MB_CAUSE_MAX];
        int passed;
        if (!sh_ms_entry_matches(e, spec)) continue;
        matched++;
        if (st) st->entries++;
        if (!e->loaded) {
            if (ok) strncpy_s(first, sizeof first, e->cause, _TRUNCATE);
            ok = 0;
            continue;
        }
        folder = sh_mb_wpath(e->folder);
        passed = folder && ms_verify_folder(folder, &e->bundle, st, one, sizeof one);
        if (!folder) strcpy_s(one, sizeof one, "out of memory");
        if (!passed) {
            if (ok) strncpy_s(first, sizeof first, one, _TRUNCATE);
            ok = 0;
        }
        if (!sh_mb_set_state(e->folder, passed, passed ? "" : one, why, sizeof why))
            ms_log("MODS: verify %s: the result could not be recorded in mod.json: %s", e->name, why);
        free(folder);
    }
    sh_ms_list_free(&l);
    modstore_unlock();
    if (!matched) return ms_cause(cause, cap, "no installed mod is named %s", spec);
    if (!ok) return ms_cause(cause, cap, "%s", first);
    return 1;
}

/* ------------------------------------------------------------ cache ---- */

int sh_ms_cache_note(const char *folder, const char *bundle_hash)
{
    char root[MAX_PATH * 4], tree[SH_MB_HASH_HEX + 1], value[SH_MB_HASH_HEX + 3];
    char *text = NULL, *out;
    sh_json_object cache;
    int ok = 0;
    if (!folder || !bundle_hash || !sh_ms_root(root, sizeof root) || !sh_mb_tree_hash(folder, tree)) return 0;
    memset(&cache, 0, sizeof cache);
    sh_mb_record_lock();
    if (sh_mb_record_get(root, "cache", &text) == 1) sh_json_parse_object(text, strlen(text), 4, &cache);
    _snprintf_s(value, sizeof value, _TRUNCATE, "\"%s\"", bundle_hash);
    if (sh_json_object_set(&cache, tree, value, 4) && (out = sh_json_serialize_object(&cache, 0, NULL)) != NULL) {
        ok = sh_mb_record_put(root, "cache", out);
        free(out);
    }
    sh_mb_record_unlock();
    sh_json_object_free(&cache);
    free(text);
    return ok;
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

static void ms_chain_init(sh_mb_resolver *out, ms_chain *chain, const sh_mb_resolver *user, sh_ms_list *list)
{
    chain->user = user;
    chain->list = list;
    out->installed = ms_chain_installed;
    out->installed_context = chain;
    out->stock = user ? user->stock : NULL;
    out->stock_context = user ? user->stock_context : NULL;
}

/* Build a folder of the store in place and write its product block. */
static int ms_build_in_place(const char *folder, const sh_mb_resolver *resolver, sh_mb_bundle *b,
                             char *cause, size_t cap)
{
    sh_ms_list list;
    sh_mb_resolver chain_resolver;
    ms_chain chain;
    char one[SH_MB_CAUSE_MAX];
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    ms_chain_init(&chain_resolver, &chain, resolver, &list);
    if (!sh_mb_build(folder, &chain_resolver, b)) {
        sh_ms_list_free(&list);
        ms_note_cause(folder, b->first_cause);
        /* A block that still loads keeps its files, marked not validated. */
        sh_mb_set_state(folder, 0, b->first_cause, one, sizeof one);
        return ms_cause(cause, cap, "%s", b->first_cause);
    }
    sh_ms_list_free(&list);
    if (!sh_mb_write(b, folder, one, sizeof one)) {
        ms_note_cause(folder, one);
        return ms_cause(cause, cap, "%s", one);
    }
    ms_note_cause(folder, NULL);
    sh_ms_cache_note(folder, b->hash);
    return 1;
}

static int ms_install_locked(const char *folder, const char *as_name, const sh_mb_resolver *resolver,
                             char *name_out, size_t name_cap, sh_ms_install_stats *st,
                             sh_mb_bundle *bundle_out, char *cause, size_t cap);

int sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                              char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                              sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    return sh_ms_install_as(folder, NULL, resolver, name_out, name_cap, stats, bundle_out, cause, cap);
}

int sh_ms_install_as(const char *folder, const char *as_name, const sh_mb_resolver *resolver,
                     char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                     sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_ms_install_stats local;
    int ok;
    modstore_lock();
    ok = ms_install_locked(folder, as_name, resolver, name_out, name_cap, stats ? stats : &local, bundle_out, cause, cap);
    modstore_unlock();
    return ok;
}

/* The mod.json author text of a source folder (mod.json, else package.json). */
static char *ms_author_text(const wchar_t *src, size_t *len)
{
    wchar_t *p = ms_wjoin(src, L"mod.json");
    char *t = ms_read_text(p, 16u << 20, len);
    free(p);
    if (t) return t;
    p = ms_wjoin(src, L"package.json");
    t = ms_read_text(p, SH_MB_DESCRIPTOR_MAX, len);
    free(p);
    return t;
}

static int ms_install_locked(const char *folder, const char *as_name, const sh_mb_resolver *resolver,
                             char *name_out, size_t name_cap, sh_ms_install_stats *st,
                             sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_mb_bundle b;
    sh_mb_resolver chain_resolver;
    ms_chain chain;
    sh_ms_list list;
    wchar_t *root = NULL, *src = NULL, *final = NULL, *staging = NULL, *old = NULL;
    char rootu[MAX_PATH * 4], name[SH_MS_NAME_MAX + 1], one[SH_MB_CAUSE_MAX];
    char *author = NULL, *text = NULL;
    size_t i, author_len = 0, text_len = 0;
    int ok = 0, have_list;

    memset(st, 0, sizeof *st);
    memset(&b, 0, sizeof b);
    if (name_out && name_cap) name_out[0] = 0;
    if (bundle_out) memset(bundle_out, 0, sizeof *bundle_out);
    name[0] = 0;
    root = ms_wroot();
    src = folder ? sh_mb_wpath(folder) : NULL;
    if (!root || !src || !sh_ms_root(rootu, sizeof rootu)) {
        ms_cause(cause, cap, root ? "no mod folder given" : "the store folder cannot be resolved");
        goto done;
    }
    if (_wcsicmp(src, root) == 0) { ms_cause(cause, cap, "that folder is the store itself, not a mod"); goto done; }
    if (!ms_mkdirs(root)) { ms_cause(cause, cap, "cannot create the store folder (error %lu)", GetLastError()); goto done; }

    if (ms_child_of(src, root)) {
        /* A folder of the store: built in place. */
        st->in_place = 1;
        if (!ms_build_in_place(folder, resolver, &b, cause, cap)) goto done;
        for (i = 0; i < b.file_count; i++)
            if (b.files[i].stock) { st->stock_files++; st->stock_bytes += b.files[i].size; }
        strncpy_s(name, sizeof name, ms_leaf(folder), _TRUNCATE);
        ok = 1;
        goto done;
    }
    if (ms_inside(src, root)) { ms_cause(cause, cap, "that folder is inside a mod of the store, not a mod itself"); goto done; }

    /* From elsewhere: built in memory, the source is not written. */
    have_list = sh_ms_enumerate(&list);
    if (!have_list) memset(&list, 0, sizeof list);
    ms_chain_init(&chain_resolver, &chain, resolver, &list);
    if (!sh_mb_build(folder, &chain_resolver, &b)) {
        sh_ms_list_free(&list);
        ms_cause(cause, cap, "%s", b.first_cause);
        goto done;
    }
    {
        /* The build counter continues from the installed folder of the same
         * id and version: unchanged content keeps its number, changed content
         * takes the next one (the source folder carries no product block). */
        const sh_ms_entry *same = NULL;
        for (i = 0; i < list.count && !same; i++) {
            const sh_ms_entry *e = &list.items[i];
            if (e->loaded && !strcmp(e->bundle.id, b.id) && !strcmp(e->bundle.version, b.version) &&
                (!as_name || !as_name[0] || !_stricmp(e->name, as_name))) same = e;
        }
        if (same && same->bundle.build) {
            sh_mb_renumber(&b, same->bundle.build);
            if (strcmp(b.hash, same->bundle.hash) != 0) sh_mb_renumber(&b, same->bundle.build + 1);
            else b.built = same->bundle.built;
        }
        if (same && (!as_name || !as_name[0])) strncpy_s(name, sizeof name, same->name, _TRUNCATE);
    }
    if (as_name && as_name[0]) {
        strncpy_s(name, sizeof name, as_name, _TRUNCATE);
    } else {
        if (!name[0] && !sh_mb_folder_name(rootu, b.id, b.version, b.hash, name, sizeof name)) {
            sh_ms_list_free(&list);
            ms_cause(cause, cap, "no free folder name for %s@%s", b.id, b.version);
            goto done;
        }
    }
    sh_ms_list_free(&list);
    if (!b.origin[0]) strcpy_s(b.origin, sizeof b.origin, SH_MB_ORIGIN_INSTALL);
    for (i = 0; i < b.file_count; i++) {
        if (!b.files[i].stock) continue;
        b.files[i].absent = 1;   /* the store never receives installed content */
        st->stock_files++;
        st->stock_bytes += b.files[i].size;
    }
    {
        wchar_t suffix[48];
        size_t n;
        final = ms_wjoin_utf8(root, name);
        if (!final) { ms_cause(cause, cap, "out of memory"); goto done; }
        n = wcslen(final) + 48;
        staging = (wchar_t *)malloc(sizeof(wchar_t) * n);
        old = (wchar_t *)malloc(sizeof(wchar_t) * n);
        if (!staging || !old) { ms_cause(cause, cap, "out of memory"); goto done; }
        _snwprintf_s(suffix, 48, _TRUNCATE, L".staging-%lu", GetCurrentProcessId());
        _snwprintf_s(staging, n, _TRUNCATE, L"%s%s", final, suffix);
        _snwprintf_s(old, n, _TRUNCATE, L"%s.old-%lu", final, GetCurrentProcessId());
    }
    if (ms_exists(staging)) ms_rmtree(staging, root);
    if (!ms_mkdirs(staging)) { ms_cause(cause, cap, "cannot create %s (error %lu)", name, GetLastError()); goto done; }
    for (i = 0; i < b.file_count; i++) {
        wchar_t *from, *to;
        int copied;
        if (b.files[i].stock) continue;
        from = ms_wjoin_utf8(src, b.files[i].path);
        to = ms_wjoin_utf8(staging, b.files[i].path);
        copied = from && to && sh_ms_copy_file(from, to);
        free(from);
        free(to);
        if (!copied) {
            DWORD err = GetLastError();
            ms_rmtree(staging, root);
            ms_cause(cause, cap, "cannot copy %s into the store (error %lu)", b.files[i].path, err);
            goto done;
        }
        st->copied++;
    }
    author = ms_author_text(src, &author_len);
    text = sh_mb_modjson_text(&b, author, author_len, &text_len);
    {
        wchar_t *mj = ms_wjoin(staging, L"mod.json");
        int wrote = mj && text && ms_write_text(mj, text, text_len);
        free(mj);
        if (!wrote) { ms_rmtree(staging, root); ms_cause(cause, cap, "cannot write mod.json for %s", name); goto done; }
    }
    /* The copy is what serves: hash it (the source may have changed since the build). */
    if (!ms_verify_folder(staging, &b, NULL, one, sizeof one)) {
        ms_rmtree(staging, root);
        ms_cause(cause, cap, "the copy did not verify: %s; build again", one);
        goto done;
    }
    if (ms_exists(final)) {
        if (ms_exists(old)) ms_rmtree(old, root);
        if (!MoveFileExW(final, old, 0)) {
            DWORD err = GetLastError();
            ms_rmtree(staging, root);
            ms_cause(cause, cap, "cannot replace the installed %s; is a file in it open? (error %lu)", name, err);
            goto done;
        }
        st->replaced = 1;
    }
    if (!MoveFileExW(staging, final, 0)) {
        DWORD err = GetLastError();
        if (st->replaced) MoveFileExW(old, final, 0);
        ms_rmtree(staging, root);
        ms_cause(cause, cap, "cannot place %s (error %lu)", name, err);
        goto done;
    }
    if (st->replaced) {
        ms_rmtree(old, root);
        ms_log("MODS: replaced the installed %s with a new build of %s@%s", name, b.id, b.version);
    }
    {
        char *fu = sh_mb_utf8(final);
        if (fu) { sh_ms_cache_note(fu, b.hash); ms_note_cause(fu, NULL); free(fu); }
    }
    ok = 1;
done:
    if (ok && name_out) strncpy_s(name_out, name_cap, name, _TRUNCATE);
    if (bundle_out) { *bundle_out = b; memset(&b, 0, sizeof b); }
    sh_mb_free(&b);
    free(author);
    free(text);
    free(root); free(src); free(final); free(staging); free(old);
    return ok;
}

/* ------------------------------------------------------------- sync ---- */

int sh_ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *st)
{
    sh_ms_sync_stats local;
    WIN32_FIND_DATAW fd;
    wchar_t *root, *pattern;
    HANDLE h;
    char rootu[MAX_PATH * 4], *text = NULL, *out;
    sh_json_object cache, next;
    ULONGLONG t0 = GetTickCount64();
    int changed = 0;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    root = ms_wroot();
    if (!root || !sh_ms_root(rootu, sizeof rootu)) { free(root); return 0; }
    if (!ms_exists(root)) { free(root); return 1; }
    memset(&cache, 0, sizeof cache);
    memset(&next, 0, sizeof next);
    modstore_lock();
    if (sh_mb_record_get(rootu, "cache", &text) == 1) sh_json_parse_object(text, strlen(text), 4, &cache);
    free(text);
    pattern = ms_wjoin(root, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            char *folder, tree[SH_MB_HASH_HEX + 1], value[SH_MB_HASH_HEX + 3], have[SH_MB_HASH_HEX + 1];
            const char *cached;
            sh_mb_bundle b;
            int current = 0;
            if (!ms_candidate(&fd)) continue;
            child = ms_wjoin(root, fd.cFileName);
            if (!child || !ms_has_descriptor(child)) { free(child); continue; }
            folder = sh_mb_utf8(child);
            free(child);
            if (!folder) continue;
            st->folders++;
            have[0] = 0;
            if (sh_mb_load_folder(folder, &b)) strcpy_s(have, sizeof have, b.hash);
            sh_mb_free(&b);
            if (!sh_mb_tree_hash(folder, tree)) { free(folder); continue; }
            cached = sh_json_object_get(&cache, tree);
            _snprintf_s(value, sizeof value, _TRUNCATE, "\"%s\"", have);
            current = have[0] && cached && !strcmp(cached, value);
            if (!current) {
                char cause[SH_MB_CAUSE_MAX];
                ULONGLONG b0 = GetTickCount64();
                if (ms_build_in_place(folder, resolver, &b, cause, sizeof cause)) {
                    st->rebuilt++;
                    ms_log("MODS: store: %s built: %s@%s+%llu, bundle %.16s, %zu file(s) (%u stock pointer(s)), %llu ms%s",
                           ms_leaf(folder), b.id, b.version, b.build, b.hash, b.file_count, b.stock_count,
                           (unsigned long long)(GetTickCount64() - b0),
                           have[0] && strcmp(have, b.hash) ? ", its tree changed" : (have[0] ? ", unchanged content" : ""));
                    strcpy_s(have, sizeof have, b.hash);
                    sh_mb_tree_hash(folder, tree);   /* a package.json became mod.json: same author bytes */
                    _snprintf_s(value, sizeof value, _TRUNCATE, "\"%s\"", have);
                    changed = 1;
                } else {
                    st->failed++;
                    have[0] = 0;
                    ms_log("MODS: store: %s refused: %s", ms_leaf(folder), cause);
                }
                sh_mb_free(&b);
            }
            if (have[0]) {
                sh_json_object_set(&next, tree, value, 4);
                if (!cached || strcmp(cached, value)) changed = 1;
            }
            free(folder);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (next.count != cache.count) changed = 1;
    if (changed && (out = sh_json_serialize_object(&next, 0, NULL)) != NULL) {
        sh_mb_record_put(rootu, "cache", out);
        free(out);
    }
    modstore_unlock();
    sh_json_object_free(&cache);
    sh_json_object_free(&next);
    free(root);
    st->ms = GetTickCount64() - t0;
    return 1;
}
