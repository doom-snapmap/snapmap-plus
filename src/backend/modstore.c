/* modstore.c -- see modstore.h. The mods\ folders, install, verification,
 * the rebuild rule and the upgrade from the retired layout. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "modstore.h"
#include "config_json.h"
#include "mp_json.h"
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

static int ms_is_dir(const wchar_t *path);

int sh_ms_delete_mod_folder(const char *folder, char *cause, size_t cap)
{
    wchar_t *root, *w;
    int ok = 0;
    if (cause && cap) cause[0] = 0;
    if (!folder || !folder[0]) { ms_cause(cause, cap, "no folder named"); return 0; }
    modstore_lock();
    root = ms_wroot();
    w = sh_mb_wpath(folder);
    if (!root || !w) ms_cause(cause, cap, "the store folder cannot be resolved");
    else if (!ms_child_of(w, root)) ms_cause(cause, cap, "%s is not a folder of the store", ms_leaf(folder));
    else if (!ms_is_dir(w)) ms_cause(cause, cap, "%s is no longer in the store", ms_leaf(folder));
    else if (!ms_rmtree(w, root)) ms_cause(cause, cap, "%s could not be deleted completely (error %lu)", ms_leaf(folder),
                                           GetLastError());
    else ok = 1;
    if (ok) ms_log("MODS: store: deleted mods\\%s", ms_leaf(folder));
    free(root);
    free(w);
    modstore_unlock();
    return ok;
}

/* ---------------------------------------------------- the retired bin --- */

int sh_ms_bin_sweep(sh_ms_sweep_line_fn line, void *context)
{
    wchar_t *root = ms_wroot(), *bin = NULL, *pattern = NULL, *slash;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char root8[MAX_PATH * 4], text[MAX_PATH * 4 + 160], *record = NULL;
    int deleted = 0, failed = 0, had_record = 0;
    if (!root) return -1;
    modstore_lock();
    /* removed-mods\ is the store root's sibling: <data>\removed-mods. */
    slash = wcsrchr(root, L'\\');
    if (slash && slash > root) {
        size_t n = (size_t)(slash - root);
        bin = (wchar_t *)malloc((n + 16) * sizeof(wchar_t));
        if (bin) { memcpy(bin, root, n * sizeof(wchar_t)); wcscpy_s(bin + n, 16, L"\\removed-mods"); }
    }
    if (bin && ms_is_dir(bin) && (pattern = ms_wjoin(bin, L"*")) != NULL &&
        (h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0)) != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            char leaf[SH_MS_NAME_MAX * 4 + 1];
            int ok;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            child = ms_wjoin(bin, fd.cFileName);
            if (!child) { failed++; continue; }
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, leaf, (int)sizeof leaf, NULL, NULL)) strcpy_s(leaf, sizeof leaf, "?");
            /* A link is removed as a link; a folder's tree never follows one. */
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ok = ms_rmtree(child, bin);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveDirectoryW(child) != 0;
            else ok = ms_delete_file(child);
            free(child);
            if (ok) {
                deleted++;
                _snprintf_s(text, sizeof text, _TRUNCATE, "MODS: removed-mods: deleted %s", leaf);
            } else {
                failed++;
                _snprintf_s(text, sizeof text, _TRUNCATE, "MODS: removed-mods: %s could not be deleted completely (error %lu)",
                            leaf, GetLastError());
            }
            backend_log(text);
            if (line) line(context, text);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pattern);
    if (bin && ms_is_dir(bin) && !failed) (void)RemoveDirectoryW(bin);
    /* The record's "removed" section goes with the bin. */
    if (sh_ms_root(root8, sizeof root8) && sh_mb_record_get(root8, "removed", &record) == 1 && record) {
        had_record = 1;
        (void)sh_mb_record_put(root8, "removed", NULL);
    }
    free(record);
    if (deleted || failed || had_record) {
        if (failed)
            _snprintf_s(text, sizeof text, _TRUNCATE, "MODS: removed-mods: %d folder(s) deleted, %d could not be deleted; "
                        "the bin is retired", deleted, failed);
        else _snprintf_s(text, sizeof text, _TRUNCATE, "MODS: removed-mods: %d folder(s) deleted; the bin is retired", deleted);
        backend_log(text);
        if (line) line(context, text);
    }
    free(bin);
    free(root);
    modstore_unlock();
    return failed ? -1 : deleted;
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
    free(l->aliases);
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

static int ms_is_hash(const char *h)
{
    size_t i;
    if (!h) return 0;
    for (i = 0; i < SH_MB_HASH_HEX; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f') || (h[i] >= 'A' && h[i] <= 'F'))) return 0;
    return h[SH_MB_HASH_HEX] == 0;
}

/* The entry whose bundle hash is `hash`, a usable one first. */
static const sh_ms_entry *ms_by_hash(const sh_ms_list *l, const char *hash)
{
    const sh_ms_entry *first = NULL;
    size_t i;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!e->bundle.hash[0] || _stricmp(e->bundle.hash, hash)) continue;
        if (sh_ms_entry_usable(e)) return e;
        if (!first) first = e;
    }
    return first;
}

/* The record's aliases into the list (an unreadable section is no aliases). */
static void ms_load_aliases(sh_ms_list *out)
{
    char root[MAX_PATH * 4], *text = NULL;
    sh_json_object o;
    size_t i;
    memset(&o, 0, sizeof o);
    if (!sh_ms_root(root, sizeof root)) return;
    if (sh_mb_record_get(root, "aliases", &text) == 1 && text && sh_json_parse_object(text, strlen(text), 4, &o) &&
        o.count && (out->aliases = (sh_ms_alias *)calloc(o.count, sizeof *out->aliases)) != NULL) {
        for (i = 0; i < o.count; i++) {
            const char *v = o.members[i].value_json;
            sh_ms_alias *a = &out->aliases[out->alias_count];
            if (!ms_is_hash(o.members[i].key) || !v || v[0] != '"' || strlen(v) != SH_MB_HASH_HEX + 2) continue;
            memcpy(a->from, o.members[i].key, SH_MB_HASH_HEX + 1);
            memcpy(a->to, v + 1, SH_MB_HASH_HEX);
            a->to[SH_MB_HASH_HEX] = 0;
            if (ms_is_hash(a->to)) out->alias_count++;
        }
    }
    sh_json_object_free(&o);
    free(text);
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
    ms_load_aliases(out);
    return 1;
}

/* ---------------------------------------------------------- aliases ---- */

const sh_ms_entry *sh_ms_resolve(const sh_ms_list *l, const char *hash)
{
    const sh_ms_entry *e;
    size_t i;
    if (!l || !hash || !hash[0]) return NULL;
    if ((e = ms_by_hash(l, hash)) != NULL) return e;
    for (i = 0; i < l->alias_count; i++)
        if (!_stricmp(l->aliases[i].from, hash)) return ms_by_hash(l, l->aliases[i].to);
    return NULL;
}

static int ms_alias_rewrite(const char *old_hash, const char *new_hash, const char *forget, int *count)
{
    char root[MAX_PATH * 4], *text = NULL, *out, value[SH_MB_HASH_HEX + 3], old_value[SH_MB_HASH_HEX + 3];
    char forget_value[SH_MB_HASH_HEX + 3];
    sh_json_object cur, next;
    size_t i;
    int ok = 0, n = 0, dropped = 0, changed = 0;
    if (!sh_ms_root(root, sizeof root)) return 0;
    memset(&cur, 0, sizeof cur);
    memset(&next, 0, sizeof next);
    value[0] = old_value[0] = forget_value[0] = 0;
    if (new_hash) _snprintf_s(value, sizeof value, _TRUNCATE, "\"%s\"", new_hash);
    if (old_hash) _snprintf_s(old_value, sizeof old_value, _TRUNCATE, "\"%s\"", old_hash);
    if (forget) _snprintf_s(forget_value, sizeof forget_value, _TRUNCATE, "\"%s\"", forget);
    sh_mb_record_lock();
    if (sh_mb_record_get(root, "aliases", &text) == 1 && text) sh_json_parse_object(text, strlen(text), 4, &cur);
    for (i = 0; i < cur.count; i++) {
        const sh_json_member *m = &cur.members[i];
        const char *v = m->value_json;
        if (new_hash && !_stricmp(m->key, new_hash)) { changed = 1; continue; }   /* a build returned to that content */
        if (old_hash && !_stricmp(m->key, old_hash)) { changed = 1; continue; }   /* set again below */
        if (forget && !_stricmp(v, forget_value)) { dropped++; changed = 1; continue; }
        if (old_hash && !_stricmp(v, old_value)) { v = value; changed = 1; }    /* flat: one hop */
        if (!sh_json_object_set(&next, m->key, v, 4)) goto done;
        if (new_hash && !_stricmp(v, value)) n++;
    }
    if (old_hash && new_hash) {
        if (!sh_json_object_set(&next, old_hash, value, 4)) goto done;
        n++;
        changed = 1;
    }
    ok = 1;
    if (changed) {
        out = sh_json_serialize_object(&next, 0, NULL);
        ok = out && sh_mb_record_put(root, "aliases", next.count ? out : NULL);
        free(out);
    }
done:
    sh_mb_record_unlock();
    sh_json_object_free(&cur);
    sh_json_object_free(&next);
    free(text);
    if (count) *count = forget ? dropped : n;
    return ok;
}

int sh_ms_alias_note(const char *old_hash, const char *new_hash)
{
    int n = 0;
    if (!ms_is_hash(old_hash) || !ms_is_hash(new_hash) || !_stricmp(old_hash, new_hash)) return 0;
    return ms_alias_rewrite(old_hash, new_hash, NULL, &n) ? n : 0;
}

int sh_ms_alias_forget(const char *id, const char *hash)
{
    sh_ms_list l;
    size_t i;
    int n = 0, keep = 0;
    if (!ms_is_hash(hash)) return 0;
    if (!sh_ms_enumerate(&l)) return 0;
    for (i = 0; i < l.count && !keep; i++) {
        const sh_ms_entry *e = &l.items[i];
        if (!_stricmp(e->bundle.hash, hash) || (id && id[0] && !strcmp(e->bundle.id, id))) keep = 1;
    }
    sh_ms_list_free(&l);
    if (keep) return 0;
    if (!ms_alias_rewrite(NULL, NULL, hash, &n)) return 0;
    if (n) ms_log("MODS: store: %d alias(es) of bundle %.16s dropped: no folder of %s remains", n, hash,
                  id && id[0] ? id : "it");
    return n;
}

int sh_ms_note_build(const char *folder, const sh_mb_bundle *b)
{
    int ok;
    if (!folder || !b) return 0;
    ok = sh_ms_cache_note(folder, b->hash);
    if (b->prev_hash[0] && _stricmp(b->prev_hash, b->hash)) sh_ms_alias_note(b->prev_hash, b->hash);
    return ok;
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
    sh_ms_note_build(folder, b);
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
    char *author = NULL, *text = NULL, replaced_hash[SH_MB_HASH_HEX + 1] = "";
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
        if (same) strcpy_s(replaced_hash, sizeof replaced_hash, same->bundle.hash);
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
        int earlier = 0;
        ms_rmtree(old, root);
        /* Maps that pin the replaced build (or one before it) load this one. */
        if (replaced_hash[0] && _stricmp(replaced_hash, b.hash)) earlier = sh_ms_alias_note(replaced_hash, b.hash);
        if (earlier)
            ms_log("MODS: replaced the installed %s with a new build of %s@%s+%llu, bundle %.16s; "
                   "%d earlier bundle(s) now resolve to it (alias of %.16s)", name, b.id, b.version, b.build, b.hash,
                   earlier, replaced_hash);
        else
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
                    char lineage[96] = "";
                    st->rebuilt++;
                    if (b.prev_hash[0] && _stricmp(b.prev_hash, b.hash))
                        _snprintf_s(lineage, sizeof lineage, _TRUNCATE, "; bundle %.16s is now an alias of it", b.prev_hash);
                    ms_log("MODS: store: %s built: %s@%s+%llu, bundle %.16s, %zu file(s) (%u stock pointer(s)), %llu ms%s%s",
                           ms_leaf(folder), b.id, b.version, b.build, b.hash, b.file_count, b.stock_count,
                           (unsigned long long)(GetTickCount64() - b0),
                           have[0] && strcmp(have, b.hash) ? ", its tree changed" : (have[0] ? ", unchanged content" : ""),
                           lineage);
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

/* ---------------------------------------------------------- upgrade ---- */
/* The retired layout (Contract 2): <id>@<version>+<build>\ folders with
 * bundle.json, bundle.canonical.json and .state.json, their assets\ hard-linked
 * to blobs\<sha256> where the volume allowed it, legacy\<id>@<digest16>\ for
 * delivered packages, and claims.json, publish-links.json,
 * pending-uploads.json and .migrated.json beside them. */

typedef struct ms_up_item {
    wchar_t *dir;               /* where it is now */
    char from[SH_MS_NAME_MAX + 16];   /* its name before the upgrade ("legacy\..." for a delivered one) */
    char name[SH_MS_NAME_MAX + 1];    /* its name after */
    int legacy;                 /* under legacy\ */
    int product;                /* named <id>@<version>+<build> by the product: renamed */
    int dropped;                /* a delivered copy an installed mod already serves */
    size_t kept;                /* when dropped: the item that serves it */
    sh_mb_bundle b;
    char content[SH_MB_HASH_HEX + 1];
} ms_up_item;

typedef struct ms_up_ctx {
    wchar_t *root, *blobs;
    char rootu[MAX_PATH * 4];
    ms_up_item *items;
    size_t count, cap;
    sh_ms_upgrade_stats *st;
} ms_up_ctx;

/* Something of the retired layout is present. */
static int ms_up_needed(const wchar_t *root)
{
    static const wchar_t *const files[] = { L"blobs", L"legacy", L"claims.json", L"publish-links.json",
                                            L"pending-uploads.json", L".migrated.json", NULL };
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    int i, found = 0;
    for (i = 0; files[i] && !found; i++) found = ms_has_file(root, files[i]);
    if (found) return 1;
    pattern = ms_wjoin(root, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t *child;
        if (!ms_candidate(&fd)) continue;
        child = ms_wjoin(root, fd.cFileName);
        found = child && (ms_has_file(child, L"bundle.json") || ms_has_file(child, L".state.json") ||
                          ms_has_file(child, L"bundle.canonical.json"));
        free(child);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* Put every carried file of `b` in place as a real file: restored from
 * blobs\ when missing or of the wrong size, copied over a hard link. */
static int ms_up_files(ms_up_ctx *c, const wchar_t *dir, const sh_mb_bundle *b, unsigned *unlinked, unsigned *restored,
                       char *cause, size_t cap)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        wchar_t *p, *blob;
        unsigned long long size = 0;
        int ok = 1;
        if (f->stock) continue;
        p = ms_wjoin_utf8(dir, f->path);
        if (!p) return ms_cause(cause, cap, "out of memory");
        if (!ms_file_size(p, &size) || size != f->size) {
            unsigned long long bsize = 0;
            blob = c->blobs ? ms_wjoin_utf8(c->blobs, f->blob) : NULL;
            ok = blob && ms_file_size(blob, &bsize) && bsize == f->size && sh_ms_copy_file(blob, p);
            free(blob);
            if (!ok) { free(p); return ms_cause(cause, cap, "%s is missing and blobs\\ has no copy of it", f->path); }
            (*restored)++;
        } else if (ms_link_count(p) > 1) {
            /* Editing a linked file would edit every copy: the folder gets its own. */
            size_t n = wcslen(p) + 16;
            wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * n);
            ok = 0;
            if (tmp) {
                _snwprintf_s(tmp, n, _TRUNCATE, L"%s.unlink", p);
                ms_delete_file(tmp);
                if (CopyFileW(p, tmp, FALSE)) {
                    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
                    ok = ms_delete_file(p) && MoveFileExW(tmp, p, 0);
                    if (!ok) ms_delete_file(tmp);
                }
                free(tmp);
            }
            if (!ok) { free(p); return ms_cause(cause, cap, "%s is a hard link that could not be replaced by a copy (error %lu)", f->path, GetLastError()); }
            (*unlinked)++;
        } else {
            DWORD a = GetFileAttributesW(p);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
        }
        free(p);
    }
    return 1;
}

/* Convert one folder of the retired layout in place: mod.json written and read
 * back with the bundle's hash, then the old files removed. A folder already
 * converted is loaded. Returns 1 converted or loaded, 0 not a built folder,
 * -1 left as it was (logged). */
static int ms_up_convert(ms_up_ctx *c, ms_up_item *it)
{
    wchar_t *bj = ms_wjoin(it->dir, L"bundle.json");
    char *dir = sh_mb_utf8(it->dir), *text = NULL, cause[SH_MB_CAUSE_MAX];
    size_t len = 0;
    unsigned unlinked = 0, restored = 0;
    int r = -1, author = 0;
    sh_mb_bundle chk;
    memset(&chk, 0, sizeof chk);
    cause[0] = 0;
    if (!bj || !dir) goto out;
    if (!ms_exists(bj)) {
        /* Converted already (an interrupted pass: the leftovers go now), or
         * not a built folder. */
        r = sh_mb_load_folder(dir, &it->b) ? 1 : 0;
        if (!r) { sh_mb_free(&it->b); memset(&it->b, 0, sizeof it->b); }
        else {
            wchar_t *p = ms_wjoin(it->dir, L"bundle.canonical.json");
            if (p && ms_exists(p)) ms_delete_file(p);
            free(p);
            p = ms_wjoin(it->dir, L".state.json");
            if (p && ms_exists(p)) ms_delete_file(p);
            free(p);
        }
        goto out;
    }
    text = ms_read_text(bj, SH_MB_MANIFEST_MAX + 65536, &len);
    if (!text || !sh_mb_load_manifest_text(text, len, &it->b)) {
        _snprintf_s(cause, sizeof cause, _TRUNCATE, "bundle.json does not read: %s", it->b.first_cause);
        goto out;
    }
    {
        /* .state.json carries the verification result and the install time. */
        wchar_t *sp = ms_wjoin(it->dir, L".state.json");
        size_t sl = 0;
        char *s = ms_read_text(sp, 65536, &sl), sc[SH_MB_CAUSE_MAX] = "";
        int validated = 1;
        long long at = 0;
        if (s && sh_mb_state_parse(s, sl, &validated, &at, sc, sizeof sc)) {
            it->b.validated = validated;
            it->b.built = at;
            if (!validated) strncpy_s(it->b.state_cause, sizeof it->b.state_cause, sc, _TRUNCATE);
        }
        free(s);
        free(sp);
    }
    if (!ms_up_files(c, it->dir, &it->b, &unlinked, &restored, cause, sizeof cause)) goto out;
    {
        /* A folder the product laid out (named by its key) or a delivered
         * package is the product's; a folder built in place is the author's. */
        char key[SH_MS_KEY_MAX + 1];
        _snprintf_s(key, sizeof key, _TRUNCATE, "%s@%s+%llu", it->b.id, it->b.version, it->b.build);
        if (it->legacy || it->b.delivered) strcpy_s(it->b.origin, sizeof it->b.origin, SH_MB_ORIGIN_DELIVERY);
        else if (!_stricmp(key, it->name))
            strcpy_s(it->b.origin, sizeof it->b.origin, it->b.legacy ? SH_MB_ORIGIN_MIGRATION : SH_MB_ORIGIN_INSTALL);
    }
    author = ms_has_file(it->dir, L"mod.json") || ms_has_file(it->dir, L"package.json");
    if (!sh_mb_write(&it->b, dir, cause, sizeof cause)) goto out;
    if (!sh_mb_load_folder(dir, &chk) || strcmp(chk.hash, it->b.hash) != 0) {
        /* A hand-made folder whose author block was edited after its last
         * build no longer hashes to it; its block still records that build,
         * and the next rescan builds it again. Anything else stops here. */
        if (!(author && strstr(chk.first_cause, "changed since it was built"))) {
            _snprintf_s(cause, sizeof cause, _TRUNCATE, "the written mod.json does not read back as bundle %.16s: %s",
                        it->b.hash, chk.first_cause[0] ? chk.first_cause : chk.hash);
            goto out;
        }
    }
    {
        static const wchar_t *const gone[] = { L"bundle.json", L"bundle.canonical.json", L".state.json", NULL };
        int i;
        for (i = 0; gone[i]; i++) {
            wchar_t *p = ms_wjoin(it->dir, gone[i]);
            if (p && ms_exists(p)) ms_delete_file(p);
            free(p);
        }
    }
    c->st->unlinked += unlinked;
    c->st->restored += restored;
    {
        char extra[160] = "";
        if (unlinked || restored)
            _snprintf_s(extra, sizeof extra, _TRUNCATE, "; %u hard link(s) replaced by copies, %u file(s) restored from blobs\\",
                        unlinked, restored);
        ms_log("MODS: store: %s: mod.json written for %s@%s+%llu (bundle %.16s, %zu file(s), %u stock pointer(s)); "
               "bundle.json, bundle.canonical.json and .state.json removed%s",
               it->from, it->b.id, it->b.version, it->b.build, it->b.hash, it->b.file_count, it->b.stock_count, extra);
    }
    r = 1;
out:
    if (r < 0) {
        c->st->failed++;
        ms_log("MODS: store: %s: left as it was: %s", it->from, cause[0] ? cause : "it cannot be read");
    }
    sh_mb_free(&chk);
    free(text);
    free(dir);
    free(bj);
    return r;
}

static ms_up_item *ms_up_add(ms_up_ctx *c)
{
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        ms_up_item *grown = (ms_up_item *)realloc(c->items, sizeof *grown * cap);
        if (!grown) return NULL;
        c->items = grown;
        c->cap = cap;
    }
    memset(&c->items[c->count], 0, sizeof c->items[0]);
    return &c->items[c->count++];
}

static void ms_up_scan(ms_up_ctx *c, const wchar_t *dir, int legacy)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ms_up_item *it;
        char *leaf;
        int r;
        if (!ms_candidate(&fd)) continue;
        it = ms_up_add(c);
        if (!it) break;
        it->dir = ms_wjoin(dir, fd.cFileName);
        it->legacy = legacy;
        leaf = sh_mb_utf8(fd.cFileName);
        if (leaf) {
            _snprintf_s(it->from, sizeof it->from, _TRUNCATE, "%s%s", legacy ? "legacy\\" : "", leaf);
            strncpy_s(it->name, sizeof it->name, leaf, _TRUNCATE);
            free(leaf);
        }
        r = it->dir ? ms_up_convert(c, it) : -1;
        if (r != 1) {
            /* Not a built folder, or left as it was: not renamed, not counted. */
            sh_mb_free(&it->b);
            free(it->dir);
            c->count--;
            continue;
        }
        {
            char key[SH_MS_KEY_MAX + 1];
            _snprintf_s(key, sizeof key, _TRUNCATE, "%s@%s+%llu", it->b.id, it->b.version, it->b.build);
            it->product = legacy || !_stricmp(key, it->name);
        }
        sh_mb_content_key(&it->b, it->content);
        if (!legacy) c->st->folders++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* A name for an upgraded product folder: "<id>" when it is the only folder
 * of its id, else "<id>@<version>", then with "+<build>", then with the
 * hash; never one in use or taken by an earlier item. */
static void ms_up_pick(ms_up_ctx *c, size_t k)
{
    ms_up_item *it = &c->items[k];
    size_t i, same_id = 0;
    int round;
    for (i = 0; i < c->count; i++) if (!c->items[i].dropped && !strcmp(c->items[i].b.id, it->b.id)) same_id++;
    for (round = same_id > 1 ? 1 : 0; round < 5; round++) {
        char leaf[SH_MS_NAME_MAX + 1];
        wchar_t *p;
        int taken = 0;
        if (round == 0) strncpy_s(leaf, sizeof leaf, it->b.id, _TRUNCATE);
        else if (sh_mb_version_generated(it->b.version)) {
            if (round < 3) continue;
            _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s-%.*s", it->b.id, round == 3 ? 8 : 16, it->b.hash);
        }
        else if (round == 1) _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s", it->b.id, it->b.version);
        else if (round == 2) _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s+%llu", it->b.id, it->b.version, it->b.build);
        else _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s-%.*s", it->b.id, it->b.version, round == 3 ? 8 : 16, it->b.hash);
        if (!it->legacy && !_stricmp(leaf, it->name)) return;   /* already there */
        /* Folders are renamed one at a time, so the disk says what is taken. */
        p = ms_wjoin_utf8(c->root, leaf);
        taken = !p || ms_exists(p);
        free(p);
        if (!taken) { strncpy_s(it->name, sizeof it->name, leaf, _TRUNCATE); return; }
    }
}

/* Replace every quoted occurrence of `from` with `to` (record folder names). */
static char *ms_up_rename_text(char *text, const char *from, const char *to)
{
    char qf[SH_MS_NAME_MAX + 24], qt[SH_MS_NAME_MAX + 24];
    size_t lf, lt;
    char *hit;
    _snprintf_s(qf, sizeof qf, _TRUNCATE, "\"%s\"", from);
    _snprintf_s(qt, sizeof qt, _TRUNCATE, "\"%s\"", to);
    lf = strlen(qf);
    lt = strlen(qt);
    while (text && (hit = strstr(text, qf)) != NULL) {
        size_t at = (size_t)(hit - text), n = strlen(text);
        char *next = (char *)malloc(n - lf + lt + 1);
        if (!next) break;
        memcpy(next, text, at);
        memcpy(next + at, qt, lt);
        memcpy(next + at + lt, hit + lf, n - at - lf + 1);
        free(text);
        text = next;
    }
    return text;
}

/* Fold one retired product file into the record and delete it. `member`
 * names the object inside it that becomes the section (NULL: the whole). */
static int ms_up_fold(ms_up_ctx *c, const wchar_t *leaf, const char *member, const char *section, int renames)
{
    wchar_t *p = ms_wjoin(c->root, leaf);
    size_t len = 0, i;
    char *text = p ? ms_read_text(p, 64u << 20, &len) : NULL, *value = NULL;
    sh_json_object o;
    int ok = 0;
    memset(&o, 0, sizeof o);
    if (!text) { free(p); return 0; }
    if (sh_json_parse_object(text, len, 32, &o)) {
        const char *v = member ? sh_json_object_get(&o, member) : NULL;
        value = member ? (v && v[0] == '{' ? _strdup(v) : NULL) : _strdup(text);
    }
    if (value && renames) {
        for (i = 0; i < c->count; i++) {
            char from[SH_MS_NAME_MAX + 24];
            const ms_up_item *it = &c->items[i];
            const char *to = it->name;
            size_t k;
            if (it->dropped) to = c->items[it->kept].name;
            strcpy_s(from, sizeof from, it->from);
            for (k = 0; from[k]; k++) if (from[k] == '\\') from[k] = '/';
            if (strcmp(from, to)) value = ms_up_rename_text(value, from, to);
        }
    }
    if (value && sh_mb_record_put(c->rootu, section, value)) {
        ok = 1;
        ms_delete_file(p);
        c->st->records++;
        ms_log("MODS: store: %ls folded into snapmap-plus.json (%s) and removed", leaf, section);
    } else {
        ms_log("MODS: store: %ls could not be folded into snapmap-plus.json; it is left in place", leaf);
    }
    sh_json_object_free(&o);
    free(value);
    free(text);
    free(p);
    return ok;
}

static unsigned long long ms_tree_bytes(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    unsigned long long n = 0;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* ------------------------------------------------- the second step ---- */

int sh_ms_entry_delivered(const sh_ms_entry *e)
{
    return e && e->loaded && sh_mb_is_delivered(&e->bundle);
}

const sh_ms_entry *sh_ms_delivered_counterpart(const sh_ms_list *l, const sh_mb_bundle *d, int *rule)
{
    const sh_ms_entry *cover = NULL, *usable = NULL, *any = NULL, *other = NULL;
    size_t i;
    if (rule) *rule = 0;
    if (!l || !d || !d->id[0]) return NULL;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!e->loaded || sh_ms_entry_delivered(e)) continue;
        if (!strcmp(e->bundle.id, d->id)) {
            if (!cover && sh_mb_requests_cover(&e->bundle, d)) cover = e;
            if (!usable && sh_ms_entry_usable(e)) usable = e;
            if (!any) any = e;
        } else if (sh_mb_requests_cover(&e->bundle, d) &&
                   (!other || e->bundle.file_count < other->bundle.file_count)) {
            other = e;
        }
    }
    if (cover || usable || any) {
        if (rule) *rule = 1;
        return cover ? cover : usable ? usable : any;
    }
    if (other && rule) *rule = 2;
    return other;
}

/* A folder name the second step changed: `from` now answers as `to`. */
typedef struct ms_pair {
    char from[SH_MS_NAME_MAX + 1];
    char to[SH_MS_NAME_MAX + 1];
} ms_pair;

/* The 64-hex tree a delivered package's legacy record keys it by
 * ("delivery.<tree>"), or 0. */
static int ms_delivery_tree(const sh_mb_bundle *b, char tree[SH_MB_HASH_HEX + 1])
{
    mpj *x = b->extra ? mpj_parse(b->extra, strlen(b->extra)) : NULL;
    const mpj *lg = x ? mpj_get(x, "legacy") : NULL, *k = lg ? mpj_get(lg, "key") : NULL;
    int ok = k && k->type == MPJ_STR && k->text && !strncmp(k->text, "delivery.", 9) && ms_is_hash(k->text + 9);
    if (ok) memcpy(tree, k->text + 9, SH_MB_HASH_HEX + 1);
    mpj_free(x);
    return ok;
}

static const char *ms_pair_apply(const ms_pair *p, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp(p[i].from, name)) name = p[i].to;
    return name;
}

static int ms_outcome_done(const mpj *row)
{
    const mpj *o = mpj_get(row, "outcome");
    return mpj_str_is(o, "built") || mpj_str_is(o, "converted") || mpj_str_is(o, "reused");
}

/* The migration section after the drops and renames; collapsed once complete. */
static void ms_tidy_migration(const char *rootu, const ms_pair *p, size_t np, mpj *trees_add, sh_ms_upgrade_stats *st)
{
    char *text = NULL, *out;
    mpj *m, *next;
    size_t len = 0;
    int i, k, complete, dropped = 0;
    if (sh_mb_record_get(rootu, "migration", &text) != 1 || !text) { free(text); return; }
    m = mpj_parse(text, strlen(text));
    free(text);
    if (!m || m->type != MPJ_OBJ || !(next = mpj_new(MPJ_OBJ))) { mpj_free(m); return; }
    {
        const mpj *c = mpj_get(m, "complete");
        complete = c && c->type == MPJ_BOOL && c->b;
    }
    for (i = 0; i < m->n; i++) {
        const char *key = m->keys[i];
        const mpj *v = m->items[i];
        mpj *copy = NULL;
        if (!strcmp(key, "trees") && v->type == MPJ_OBJ) {
            copy = mpj_new(MPJ_OBJ);
            for (k = 0; copy && k < v->n; k++) {
                const mpj *to = v->items[k];
                const char *name = to->type == MPJ_STR && to->text ? ms_pair_apply(p, np, to->text) : NULL;
                if (!trees_add || !mpj_get(trees_add, v->keys[k]))
                    mpj_set(copy, v->keys[k], name ? mpj_new_str(name) : mpj_clone(to));
            }
            for (k = 0; copy && trees_add && k < trees_add->n; k++) {
                const mpj *to = trees_add->items[k];
                mpj_set(copy, trees_add->keys[k], to->type == MPJ_STR && to->text ? mpj_new_str(ms_pair_apply(p, np, to->text))
                                                                                  : mpj_clone(to));
            }
        } else if (!strcmp(key, "folders") && v->type == MPJ_ARR) {
            copy = mpj_new(MPJ_ARR);
            for (k = 0; copy && k < v->n; k++) {
                mpj *row;
                const mpj *store;
                if (complete && ms_outcome_done(v->items[k])) { dropped++; continue; }
                row = mpj_clone(v->items[k]);
                store = row ? mpj_get(row, "store") : NULL;
                if (store && store->type == MPJ_STR && store->text && strcmp(ms_pair_apply(p, np, store->text), store->text))
                    mpj_set(row, "store", mpj_new_str(ms_pair_apply(p, np, store->text)));
                if (row) mpj_push(copy, row);
            }
        } else if (!complete || !strcmp(key, "complete") || !strcmp(key, "migratedAt") || !strcmp(key, "source")) {
            copy = mpj_clone(v);
        } else {
            dropped++;   /* a counter of the finished pass */
        }
        if (copy) mpj_set(next, key, copy);
    }
    if (complete && !mpj_get(next, "folders")) mpj_set(next, "folders", mpj_new(MPJ_ARR));
    out = mpj_dump(next, &len);
    if (out && sh_mb_record_put(rootu, "migration", out)) st->migration_collapsed = complete && dropped > 0;
    free(out);
    mpj_free(next);
    mpj_free(m);
}

/* Rename one product folder; 1 when it moved. */
static int ms_tidy_move(const char *folder, const char *rootu, const char *to)
{
    char path[MAX_PATH * 4];
    wchar_t *from = sh_mb_wpath(folder), *dst;
    int ok;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", rootu, to);
    dst = sh_mb_wpath(path);
    ok = from && dst && !ms_exists(dst) && MoveFileExW(from, dst, 0);
    free(from);
    free(dst);
    return ok;
}

static int ms_tidy(sh_ms_upgrade_stats *st, const char *rootu)
{
    char *text = NULL;
    sh_ms_list l;
    ms_pair *pairs = NULL;
    size_t np = 0, i;
    mpj *trees_add;
    ULONGLONG t0 = GetTickCount64();
    int done = sh_mb_record_get(rootu, "layout", &text) == 1 && text && atoi(text) >= 2;
    free(text);
    if (done) return 0;
    modstore_lock();
    st->tidied = 1;
    trees_add = mpj_new(MPJ_OBJ);
    if (!sh_ms_enumerate(&l)) { modstore_unlock(); mpj_free(trees_add); return -1; }
    pairs = (ms_pair *)calloc(l.count * 2 + 1, sizeof *pairs);
    if (!pairs) { sh_ms_list_free(&l); modstore_unlock(); mpj_free(trees_add); return -1; }

    /* 1. A package a map delivered that copies an installed mod goes; its
     * hash answers as the installed mod's. */
    for (i = 0; i < l.count; i++) {
        const sh_ms_entry *d = &l.items[i], *m;
        char tree[SH_MB_HASH_HEX + 1];
        int rule = 0;
        if (!sh_ms_entry_delivered(d)) continue;
        m = sh_ms_delivered_counterpart(&l, &d->bundle, &rule);
        if (!m) continue;
        if (!sh_ms_remove_folder(d->folder)) {
            st->tidy_failed++;
            ms_log("MODS: store: %s: a copy of %s that a map delivered, but it could not be removed; it stays", d->name, m->name);
            continue;
        }
        if (_stricmp(d->bundle.hash, m->bundle.hash)) sh_ms_alias_note(d->bundle.hash, m->bundle.hash);
        strcpy_s(pairs[np].from, sizeof pairs[np].from, d->name);
        strcpy_s(pairs[np].to, sizeof pairs[np].to, m->name);
        np++;
        if (trees_add && ms_delivery_tree(&d->bundle, tree)) mpj_set(trees_add, tree, mpj_new_str(m->name));
        if (rule == 1) {
            st->same_id++;
            ms_log("MODS: store: %s: dropped, delivered by a map under the id of the installed %s; bundle %.16s now "
                   "resolves to %.16s", d->name, m->name, d->bundle.hash, m->bundle.hash);
        } else {
            st->covered++;
            ms_log("MODS: store: %s: dropped, delivered by a map as %s; the installed %s (%s) provides all %zu of its "
                   "path(s); bundle %.16s now resolves to %.16s", d->name, d->bundle.id, m->name, m->bundle.id,
                   d->bundle.file_count, d->bundle.hash, m->bundle.hash);
        }
    }
    sh_ms_list_free(&l);

    /* 2. Names: a product folder alone with its id is "<id>"; a generated
     * version never names a folder. */
    if (sh_ms_enumerate(&l)) {
        for (i = 0; i < l.count; i++) {
            const sh_ms_entry *e = &l.items[i];
            char to[SH_MS_NAME_MAX + 1], prefix[SH_MB_ID_MAX + 2];
            size_t k, same = 0, pl;
            int generated;
            if (!e->loaded || !e->bundle.origin[0] || !e->bundle.id[0] || !_stricmp(e->name, e->bundle.id)) continue;
            for (k = 0; k < l.count; k++) if (l.items[k].loaded && !strcmp(l.items[k].bundle.id, e->bundle.id)) same++;
            _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "%s@", e->bundle.id);
            pl = strlen(prefix);
            generated = strstr(e->name, "@legacy-") != NULL;
            if (same == 1 && !_strnicmp(e->name, prefix, pl) && ms_tidy_move(e->folder, rootu, e->bundle.id))
                strcpy_s(to, sizeof to, e->bundle.id);
            else if (generated) {
                _snprintf_s(to, sizeof to, _TRUNCATE, "%s-%.8s", e->bundle.id, e->bundle.hash);
                if (!ms_tidy_move(e->folder, rootu, to)) {
                    _snprintf_s(to, sizeof to, _TRUNCATE, "%s-%.16s", e->bundle.id, e->bundle.hash);
                    if (!ms_tidy_move(e->folder, rootu, to)) {
                        st->tidy_failed++;
                        ms_log("MODS: store: %s could not be renamed (error %lu); it keeps its name", e->name, GetLastError());
                        continue;
                    }
                }
            } else continue;
            st->tidy_renamed++;
            ms_log("MODS: store: %s -> %s", e->name, to);
            strcpy_s(pairs[np].from, sizeof pairs[np].from, e->name);
            strcpy_s(pairs[np].to, sizeof pairs[np].to, to);
            np++;
        }
        sh_ms_list_free(&l);
    }

    /* 3. The migration record. */
    ms_tidy_migration(rootu, pairs, np, trees_add, st);
    if (!st->tidy_failed) sh_mb_record_put(rootu, "layout", "2");
    st->tidy_ms = GetTickCount64() - t0;
    ms_log("MODS: store: tidied in %llu ms: %u package(s) a map delivered dropped as copies of an installed mod (%u by "
           "id, %u by paths), %u folder(s) renamed, the migration record %s%s",
           st->tidy_ms, st->same_id + st->covered, st->same_id, st->covered, st->tidy_renamed,
           st->migration_collapsed ? "keeps only the folders not yet done" : "unchanged",
           st->tidy_failed ? "; some folders were left as they were, so this runs again at the next start" : "");
    modstore_unlock();
    free(pairs);
    mpj_free(trees_add);
    return 1;
}

int sh_ms_upgrade(sh_ms_upgrade_stats *stats)
{
    sh_ms_upgrade_stats local;
    ms_up_ctx c;
    wchar_t *legacy;
    ULONGLONG t0 = GetTickCount64();
    size_t i, k;
    sh_json_object aliases;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    memset(&c, 0, sizeof c);
    memset(&aliases, 0, sizeof aliases);
    c.st = stats;
    c.root = ms_wroot();
    if (!c.root || !sh_ms_root(c.rootu, sizeof c.rootu)) { free(c.root); return -1; }
    if (!ms_exists(c.root)) { free(c.root); return 0; }
    if (!ms_up_needed(c.root)) {
        int tidied = ms_tidy(stats, c.rootu);
        free(c.root);
        return tidied;
    }
    modstore_lock();
    stats->ran = 1;
    c.blobs = ms_wjoin(c.root, L"blobs");
    if (c.blobs && !ms_is_dir(c.blobs)) { free(c.blobs); c.blobs = NULL; }
    ms_log("MODS: store: upgrading %s to the mod.json layout (Contract 5)", c.rootu);

    /* 1. Every built folder, then every delivered package, converted in place. */
    ms_up_scan(&c, c.root, 0);
    legacy = ms_wjoin(c.root, L"legacy");
    if (legacy && ms_is_dir(legacy)) ms_up_scan(&c, legacy, 1);

    /* 2. A delivered copy that an installed mod already serves is dropped: the
     * same bundle hash, or the same content (an alias keeps the maps that name
     * the copy's hash resolving to the kept one). */
    {
        char *text = NULL;
        if (sh_mb_record_get(c.rootu, "aliases", &text) == 1) sh_json_parse_object(text, strlen(text), 4, &aliases);
        free(text);
    }
    for (i = 0; i < c.count; i++) {
        ms_up_item *it = &c.items[i];
        if (!it->legacy) continue;
        for (k = 0; k < c.count && !it->dropped; k++) {
            const ms_up_item *o = &c.items[k];
            if (k == i || o->dropped || (o->legacy && k > i)) continue;
            if (!strcmp(o->b.hash, it->b.hash) || (!strcmp(o->b.id, it->b.id) && !strcmp(o->content, it->content))) {
                char value[SH_MB_HASH_HEX + 3], *dir = sh_mb_utf8(it->dir);
                it->dropped = 1;
                it->kept = k;
                if (strcmp(o->b.hash, it->b.hash)) {
                    _snprintf_s(value, sizeof value, _TRUNCATE, "\"%s\"", o->b.hash);
                    sh_json_object_set(&aliases, it->b.hash, value, 4);
                }
                ms_log("MODS: store: %s: dropped, %s serves the same content%s%.16s%s%.16s%s", it->from, o->from,
                       strcmp(o->b.hash, it->b.hash) ? " (alias " : " (same bundle ", it->b.hash,
                       strcmp(o->b.hash, it->b.hash) ? " -> " : "", strcmp(o->b.hash, it->b.hash) ? o->b.hash : "", ")");
                if (dir) { sh_ms_remove_folder(dir); free(dir); }
                stats->dropped++;
            }
        }
    }
    /* 3. Names: product folders become <id> (or <id>@<version>); delivered
     * packages move out of legacy\ the same way. */
    for (i = 0; i < c.count; i++) {
        ms_up_item *it = &c.items[i];
        wchar_t *to;
        char before[SH_MS_NAME_MAX + 16];
        if (it->dropped || !it->product) continue;
        strcpy_s(before, sizeof before, it->name);
        ms_up_pick(&c, i);
        if (!it->legacy && !_stricmp(before, it->name)) continue;
        to = ms_wjoin_utf8(c.root, it->name);
        if (to && MoveFileExW(it->dir, to, 0)) {
            free(it->dir);
            it->dir = to;
            if (it->legacy) stats->delivered++;
            else stats->renamed++;
            ms_log("MODS: store: %s -> %s", it->from, it->name);
        } else {
            DWORD err = GetLastError();
            free(to);
            strcpy_s(it->name, sizeof it->name, before);
            ms_log("MODS: store: %s could not be renamed to its new folder (error %lu); it keeps its name", it->from, err);
            if (it->legacy) stats->failed++;
        }
    }
    if (legacy && ms_is_dir(legacy) && RemoveDirectoryW(legacy)) ms_log("MODS: store: legacy\\ removed (empty)");

    /* 4. blobs\ goes when every folder holds its own files. */
    if (c.blobs) {
        if (!stats->failed) {
            stats->blob_bytes = ms_tree_bytes(c.blobs);
            stats->blobs_removed = ms_rmtree(c.blobs, c.root);
            ms_log("MODS: store: blobs\\ %s (%llu MiB)", stats->blobs_removed ? "removed" : "could not be removed completely",
                   stats->blob_bytes >> 20);
        } else {
            ms_log("MODS: store: blobs\\ kept: %u folder(s) of the retired layout still need it", stats->failed);
        }
    }

    /* 5. The product's files become sections of the one record. */
    if (ms_has_file(c.root, L"claims.json")) ms_up_fold(&c, L"claims.json", "claims", "claims", 0);
    if (ms_has_file(c.root, L"publish-links.json")) ms_up_fold(&c, L"publish-links.json", "links", "links", 0);
    if (ms_has_file(c.root, L"pending-uploads.json")) ms_up_fold(&c, L"pending-uploads.json", "pending", "pending", 0);
    if (ms_has_file(c.root, L".migrated.json")) ms_up_fold(&c, L".migrated.json", NULL, "migration", 1);
    if (aliases.count) {
        char *out = sh_json_serialize_object(&aliases, 0, NULL);
        if (out) { sh_mb_record_put(c.rootu, "aliases", out); free(out); }
    }

    /* 6. The build cache: every converted folder is current. */
    for (i = 0; i < c.count; i++) {
        char *dir;
        if (c.items[i].dropped || !c.items[i].dir) continue;
        dir = sh_mb_utf8(c.items[i].dir);
        if (dir) { sh_ms_cache_note(dir, c.items[i].b.hash); free(dir); }
    }
    stats->ms = GetTickCount64() - t0;
    ms_log("MODS: store: upgraded to the mod.json layout in %llu ms: %u folder(s) converted (%u renamed), %u delivered "
           "package(s) moved out of legacy\\, %u dropped as copies of an installed mod, %u hard link(s) replaced by copies, "
           "%u file(s) restored from blobs\\, blobs\\ %s, %u record file(s) folded into snapmap-plus.json, %u folder(s) "
           "left as they were",
           stats->ms, stats->folders, stats->renamed, stats->delivered, stats->dropped, stats->unlinked, stats->restored,
           stats->blobs_removed ? "removed" : (c.blobs ? "kept" : "absent"), stats->records, stats->failed);
    ms_tidy(stats, c.rootu);
    modstore_unlock();
    for (i = 0; i < c.count; i++) { sh_mb_free(&c.items[i].b); free(c.items[i].dir); }
    free(c.items);
    free(legacy);
    free(c.blobs);
    free(c.root);
    sh_json_object_free(&aliases);
    return 1;
}
