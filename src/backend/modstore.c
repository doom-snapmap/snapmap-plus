/* modstore.c -- see modstore.h. The mods\ folders and their places, the
 * salt, install, verification and the scan. The store tool's half (the
 * retired layouts, the bin sweep) is modstore_upgrade.c. */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "modstore.h"
#include "config_json.h"
#include "mp_json.h"
#include "backend_log.h"
#include "plain_reason.h"

#include "modstore_internal.h"
/* ------------------------------------------------------------ lock ----- */

/* The store lock: one critical section for every writer of mods\ (installs,
 * downloads, the migration, exports, claims). It is recursive on purpose: the
 * install and verification below take it themselves, and their callers (the
 * migration pass, a download) already hold it around a larger step. */
static INIT_ONCE        g_ms_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_ms_lock;
static volatile LONG    g_ms_lock_waits;
static volatile LONG    g_ms_lock_depth;   /* nested holds of the owner; read from any thread */
static volatile LONG    g_ms_lock_gen;     /* outermost holds released so far */
static volatile LONG    g_ms_lock_waiting; /* threads waiting for it now (the boot build yields to them) */

static BOOL CALLBACK ms_lock_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_ms_lock);
    return TRUE;
}

void modstore_lock(void)
{
    InitOnceExecuteOnce(&g_ms_lock_once, ms_lock_init, NULL, NULL);
    if (!TryEnterCriticalSection(&g_ms_lock)) {
        /* Held by another thread: wait for it rather than fail. The waiting
         * count is raised before the wait count, so a holder that sees a new
         * wait always finds the waiter and yields to it (ms_lock_yield). */
        InterlockedIncrement(&g_ms_lock_waiting);
        InterlockedIncrement(&g_ms_lock_waits);
        backend_log("MODS: store: waiting for another store operation to finish (an install, a download or a build)");
        EnterCriticalSection(&g_ms_lock);
        InterlockedDecrement(&g_ms_lock_waiting);
    }
    InterlockedIncrement(&g_ms_lock_depth);
}

void modstore_unlock(void)
{
    if (InterlockedDecrement(&g_ms_lock_depth) == 0) InterlockedIncrement(&g_ms_lock_gen);
    LeaveCriticalSection(&g_ms_lock);
}

int modstore_lock_busy(unsigned *generation)
{
    if (generation) *generation = (unsigned)InterlockedCompareExchange(&g_ms_lock_gen, 0, 0);
    return InterlockedCompareExchange(&g_ms_lock_depth, 0, 0) > 0;
}

unsigned modstore_lock_waits(void)
{
    return (unsigned)InterlockedCompareExchange(&g_ms_lock_waits, 0, 0);
}

/* The boot build's hand-over between folders: when another thread waits for
 * the store and this thread holds it once (not inside a larger step), the
 * hold is released until the waiter has it, then taken again (after the
 * waiter's operation). 1 when it yielded. */
static int ms_lock_yield(void)
{
    int i;
    if (InterlockedCompareExchange(&g_ms_lock_waiting, 0, 0) <= 0 ||
        InterlockedCompareExchange(&g_ms_lock_depth, 0, 0) != 1)
        return 0;
    modstore_unlock();
    /* The waiter leaves the count once it holds the lock; until then this
     * thread stays off it, so the critical section's unfair hand-over cannot
     * give it straight back. */
    for (i = 0; i < 2000 && InterlockedCompareExchange(&g_ms_lock_waiting, 0, 0) > 0; i++) Sleep(1);
    modstore_lock();
    return 1;
}

static char g_ms_root[MAX_PATH * 4];
static int  g_ms_root_set;

int ms_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* A Win32 failure as a plain reason with the next step (plain_reason.h).
 * `name` is the file or folder; `in_store` names it relative to the store
 * root, which is then prefixed; NULL names the store folder. A full disk
 * names the store's drive and the bytes needed (0: unknown). The text lives
 * in a per-thread buffer: one call per message. */
const char *ms_why(DWORD err, const char *name, int in_store, unsigned long long bytes)
{
    char root[MAX_PATH * 4], path[MAX_PATH * 4 + 300];
    int have_root = sh_ms_root(root, sizeof root);
    if (!name || !name[0]) strcpy_s(path, sizeof path, have_root ? root : "the mods folder");
    else if (in_store && have_root) _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, name);
    else if ((err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) && have_root && name[1] != ':')
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, name);
    else strcpy_s(path, sizeof path, name);
    return sh_reason_win32(err, path, bytes);
}

void ms_log(const char *fmt, ...)
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
    char base[MAX_PATH * 4];
    if (!out || !cap) return 0;
    out[0] = 0;
    if (g_ms_root_set) {
        if (strlen(g_ms_root) >= cap) return 0;
        strcpy_s(out, cap, g_ms_root);
        return 1;
    }
    if (!sh_mb_user_root(base, sizeof base)) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\mods", base);
    return out[0] != 0;
}

/* ------------------------------------------------------------ paths ---- */

wchar_t *ms_wjoin(const wchar_t *dir, const wchar_t *name)
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
wchar_t *ms_wjoin_utf8(const wchar_t *dir, const char *rel)
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

wchar_t *ms_wroot(void)
{
    char root[MAX_PATH * 4];
    if (!sh_ms_root(root, sizeof root)) return NULL;
    return sh_mb_wpath(root);
}

/* Create every missing folder of a \\?\ path. */
int ms_mkdirs(const wchar_t *path)
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
int ms_inside(const wchar_t *path, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    return _wcsnicmp(path, dir, n) == 0 && path[n] == L'\\' && path[n + 1] != 0;
}

static const char *ms_leaf(const char *folder)
{
    const char *a = strrchr(folder ? folder : "", '\\'), *b = strrchr(folder ? folder : "", '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : (folder ? folder : "");
}

/* A folder's name under mods\: its path below the store root when it lies
 * there (a nested folder keeps its organizing folders, "demon-mods\x"), else
 * its leaf. Points into `folder`. */
static const char *ms_rel(const char *folder)
{
    char root[MAX_PATH * 4];
    size_t n;
    if (!folder) return "";
    if (!sh_ms_root(root, sizeof root)) return ms_leaf(folder);
    n = strlen(root);
    if (_strnicmp(folder, root, n) == 0 && (folder[n] == '\\' || folder[n] == '/') && folder[n + 1]) return folder + n + 1;
    return ms_leaf(folder);
}

/* A work folder of an install or a salting: "<name>.staging-<pid>"
 * or "<name>.old-<pid>", the suffix last. A mod named "doom.old-school" is not one.
 * ms_work_at: where the suffix starts, or NULL. */
static const wchar_t *ms_work_at(const wchar_t *name, const wchar_t *tag)
{
    const wchar_t *at = NULL, *p;
    for (p = wcsstr(name, tag); p; p = wcsstr(p + 1, tag)) at = p;
    if (!at) return NULL;
    p = at + wcslen(tag);
    if (!*p) return NULL;
    for (; *p; p++) if (*p < L'0' || *p > L'9') return NULL;
    return at;
}

static int ms_work_suffix(const wchar_t *name, const wchar_t *tag) { return ms_work_at(name, tag) != NULL; }

/* Folders of the store that can be mods: not hidden, not a staging or
 * replaced copy, not the retired blobs\ and legacy\ (the retired layout). */
int ms_candidate(const WIN32_FIND_DATAW *fd)
{
    if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    if (fd->cFileName[0] == L'.') return 0;
    if (!_wcsicmp(fd->cFileName, L"blobs") || !_wcsicmp(fd->cFileName, L"legacy")) return 0;
    if (ms_work_suffix(fd->cFileName, L".staging-") || ms_work_suffix(fd->cFileName, L".old-")) return 0;
    return 1;
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
int ms_delete_file(const wchar_t *path)
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
int ms_rmtree(const wchar_t *path, const wchar_t *root)
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

/* ------------------------------------------------- the Recycle Bin ---- */

#ifndef FOF_NORECURSEREPARSE
#define FOF_NORECURSEREPARSE 0x8000   /* reparse points are items, never containers */
#endif


/* Move a whole folder, retrying briefly when Windows still holds the old name
 * or the new one: right after a folder is moved aside, an indexer, a virus
 * scanner or the engine's own just-closed handles can refuse the next move
 * with ERROR_ACCESS_DENIED or ERROR_SHARING_VIOLATION for a few tens of
 * milliseconds (seen live: "cannot place ... (error 5)" that succeeded on an
 * immediate retry). Any other error, or persistence past the budget, fails. */
static BOOL ms_move_folder(const wchar_t *from, const wchar_t *to)
{
    int attempt;
    for (attempt = 0; ; attempt++) {
        DWORD err;
        if (MoveFileExW(from, to, 0)) return TRUE;
        err = GetLastError();
        if ((err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) || attempt >= 20) {
            SetLastError(err);
            return FALSE;
        }
        Sleep(50);
    }
}


#if defined(SH_MODSTORE_TESTING) || defined(SH_MODUI_TESTING)
static volatile LONG g_ms_recycle_refused;
void sh_ms_recycle_refuse_for_test(int refuse) { InterlockedExchange(&g_ms_recycle_refused, refuse ? 1 : 0); }
#endif

/* Move a folder of the store (a \\?\ path) to the Windows Recycle Bin
 * through the shell: FO_DELETE with FOF_ALLOWUNDO, no confirmation, progress
 * or error UI, reparse points moved as themselves and never entered. Only a
 * fixed drive's folder is offered: on a network or removable drive the shell
 * would delete it outright without saying so. 1 when the folder went to the
 * bin; else 0 with why the bin could not take it in `why`. */
static int ms_recycle(const wchar_t *path, char *why, size_t cap)
{
    const wchar_t *plain = path;
    wchar_t drive[4], *from;
    SHFILEOPSTRUCTW op;
    size_t n;
    int rc;
#if defined(SH_MODSTORE_TESTING) || defined(SH_MODUI_TESTING)
    if (g_ms_recycle_refused) { ms_cause(why, cap, "the shell refused it (test)"); return 0; }
#endif
    if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) { ms_cause(why, cap, "a network folder has no Recycle Bin"); return 0; }
    if (!wcsncmp(path, L"\\\\?\\", 4)) plain = path + 4;
    n = wcslen(plain);
    /* The shell takes plain drive paths shorter than MAX_PATH only. */
    if (n < 3 || plain[1] != L':' || plain[2] != L'\\') { ms_cause(why, cap, "the Recycle Bin cannot take this path"); return 0; }
    if (n >= MAX_PATH) { ms_cause(why, cap, "its path is too long for the Recycle Bin"); return 0; }
    drive[0] = plain[0]; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    if (GetDriveTypeW(drive) != DRIVE_FIXED) { ms_cause(why, cap, "drive %lc: has no Recycle Bin", plain[0]); return 0; }
    from = (wchar_t *)calloc(n + 2, sizeof(wchar_t));   /* double-NUL terminated */
    if (!from) { ms_cause(why, cap, "out of memory"); return 0; }
    memcpy(from, plain, n * sizeof(wchar_t));
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NORECURSEREPARSE;
    rc = SHFileOperationW(&op);
    free(from);
    if (rc) { ms_cause(why, cap, "the shell refused it (error 0x%x)", (unsigned)rc); return 0; }
    if (op.fAnyOperationsAborted) { ms_cause(why, cap, "the shell stopped before it finished"); return 0; }
    if (ms_exists(path)) { ms_cause(why, cap, "the shell left part of it in place"); return 0; }
    return 1;
}

/* Remove a tree under the store root: the Recycle Bin first, the outright
 * delete (ms_rmtree: links removed, never followed) when the bin cannot take
 * it. SH_MS_RECYCLED, SH_MS_DELETED, or 0 when it could not be removed. */
static int ms_retire(const wchar_t *path, const wchar_t *root, char *why, size_t cap)
{
    if (why && cap) why[0] = 0;
    if (!ms_inside(path, root)) return 0;
    if (!ms_exists(path)) return SH_MS_DELETED;   /* nothing there: nothing to say */
    if (ms_recycle(path, why, cap)) return SH_MS_RECYCLED;
    return ms_rmtree(path, root) ? SH_MS_DELETED : 0;
}


static void ms_place_prune(const wchar_t *path, const wchar_t *root);
static int  ms_store_mod_folder(const wchar_t *path, const wchar_t *root);

/* After a folder left the store: an emptied place folder of mods\maps\ goes,
 * with the record's entry for it. */
static void ms_after_removal(const char *folder, const wchar_t *w, const wchar_t *root)
{
    (void)folder;
    if (!w || !root) return;
    ms_place_prune(w, root);
}

static void ms_log_discard(const char *folder, int r, const char *why)
{
    if (r == SH_MS_RECYCLED) ms_log("MODS: store: moved mods\\%s to the Recycle Bin", ms_rel(folder));
    else if (r == SH_MS_DELETED && why && why[0])
        ms_log("MODS: store: deleted mods\\%s outright; the Recycle Bin could not take it (%s)", ms_rel(folder), why);
}

int sh_ms_remove_folder(const char *folder)
{
    wchar_t *root, *w;
    char why[200] = "";
    int r = 0;
    modstore_lock();
    root = ms_wroot();
    w = folder ? sh_mb_wpath(folder) : NULL;
    if (root && w) r = ms_retire(w, root, why, sizeof why);
    ms_log_discard(folder, r, why);
    if (r) ms_after_removal(folder, w, root);
    free(root);
    free(w);
    modstore_unlock();
    return r;
}

int sh_ms_delete_mod_folder(const char *folder, char *cause, size_t cap)
{
    wchar_t *root, *w;
    char why[200] = "";
    int r = 0;
    if (cause && cap) cause[0] = 0;
    if (!folder || !folder[0]) { ms_cause(cause, cap, "no folder named"); return 0; }
    modstore_lock();
    root = ms_wroot();
    w = sh_mb_wpath(folder);
    if (!root || !w) ms_cause(cause, cap, "the store folder cannot be resolved");
    else if (!ms_is_dir(w)) ms_cause(cause, cap, "%s is no longer in the store", ms_rel(folder));
    else if (!ms_store_mod_folder(w, root)) ms_cause(cause, cap, "%s is not a mod folder of the store", ms_rel(folder));
    else if ((r = ms_retire(w, root, why, sizeof why)) == 0)
        ms_cause(cause, cap, "%s could not be deleted completely: %s", ms_rel(folder), ms_why(GetLastError(), ms_rel(folder), 1, 0));
    else if (r == SH_MS_DELETED) ms_cause(cause, cap, "%s", why);
    ms_log_discard(folder, r, why);
    if (r) ms_after_removal(folder, w, root);
    free(root);
    free(w);
    modstore_unlock();
    return r;
}
/* -------------------------------------------------------- file utils ---- */

int ms_file_size(const wchar_t *path, unsigned long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 1;
}

int ms_exists(const wchar_t *path)
{
    return path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

int ms_is_dir(const wchar_t *path)
{
    DWORD a = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* `leaf` exists in `dir`. */
int ms_has_file(const wchar_t *dir, const wchar_t *leaf)
{
    wchar_t *p = ms_wjoin(dir, leaf);
    int has = ms_exists(p);
    free(p);
    return has;
}

int ms_hash_wfile(const wchar_t *path, unsigned long long *size, char out[SH_MB_HASH_HEX + 1])
{
    char *u = sh_mb_utf8(path);
    int ok;
    if (!u) return 0;
    ok = sh_mb_sha256_file(u, size, out);
    free(u);
    return ok;
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
char *ms_read_text(const wchar_t *path, size_t max, size_t *len)
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

/* A folder spelled with or without the long-path prefix is one folder. */
static const char *ms_plain(const char *folder)
{
    return folder && !strncmp(folder, "\\\\?\\", 4) && strncmp(folder, "\\\\?\\UNC\\", 8) ? folder + 4 : folder;
}

static void ms_note_cause(const char *folder, const char *cause)
{
    size_t i;
    folder = ms_plain(folder);
    if (!folder) return;
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

void sh_ms_note_build_failure(const char *folder, const char *cause)
{
    if (folder && cause && cause[0]) ms_note_cause(folder, cause);
}

void sh_ms_build_cause(const char *folder, char *out, size_t cap)
{
    size_t i;
    if (!out || !cap) return;
    out[0] = 0;
    if (!folder) return;
    folder = ms_plain(folder);
    modstore_lock();
    for (i = 0; i < g_bcause_count; i++)
        if (!_stricmp(g_bcause[i].folder, folder)) { strncpy_s(out, cap, g_bcause[i].cause, _TRUNCATE); break; }
    modstore_unlock();
}

int sh_ms_descriptor_unparsable(const char *folder, char *out, size_t cap)
{
    char path[MAX_PATH * 4];
    unsigned char *text;
    size_t len = 0;
    sh_json_kind kind;
    sh_json_error err = { 0, NULL };
    int bad;
    if (out && cap) out[0] = 0;
    if (!folder) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
    text = sh_mb_read_file(path, 16u << 20, &len);
    if (!text) return 0;   /* missing, or unreadable: the build says which */
    bad = !sh_json_validate_ex((const char *)text, len, 16u, &kind, &err) || kind != SH_JSON_OBJECT;
    free(text);
    if (bad && out && cap)
        _snprintf_s(out, cap, _TRUNCATE, "mod.json is not a valid JSON object (%s at byte %zu); fix the file and it "
                    "builds again", err.reason ? err.reason : "not an object", err.offset);
    return bad;
}

/* ------------------------------------------------------------- walk ---- */

int ms_has_descriptor(const wchar_t *folder)
{
    wchar_t *dm = ms_wjoin(folder, L"mod.json"), *dp = ms_wjoin(folder, L"package.json");
    int has = ms_exists(dm) || ms_exists(dp);
    free(dm);
    free(dp);
    return has;
}

/* The place of a folder from its name under mods\ ("maps\<segment>\..."):
 * "maps\<segment>" in `out` (1), or "" at the root (0). */
static int ms_place_of_name(const char *name, char *out, size_t cap)
{
    size_t pl = sizeof SH_MS_PLACES - 1;
    const char *seg, *end;
    if (out && cap) out[0] = 0;
    if (!name || _strnicmp(name, SH_MS_PLACES, pl) || (name[pl] != '\\' && name[pl] != '/')) return 0;
    seg = name + pl + 1;
    end = strpbrk(seg, "\\/");
    if (!end || end == seg || !end[1]) return 0;   /* the place folder itself is not a mod's */
    if (out && cap) _snprintf_s(out, cap, _TRUNCATE, SH_MS_PLACES "\\%.*s", (int)(end - seg), seg);
    return 1;
}

/* The mod folders under a directory, at any depth: a candidate folder with
 * mod.json or package.json is a mod and is not entered; one without is an
 * organizing folder and is entered, down to SH_MS_NEST_MAX levels below the
 * root. At the store root the top folder "maps" is the places: each of its
 * children is a place, walked the same way. `rel` is each folder's path below
 * the root. */
typedef struct ms_found { wchar_t *folder, *rel; } ms_found;
typedef struct ms_found_list { ms_found *items; size_t count, cap; } ms_found_list;

static void ms_found_free(ms_found_list *l)
{
    size_t i;
    for (i = 0; i < l->count; i++) { free(l->items[i].folder); free(l->items[i].rel); }
    free(l->items);
    memset(l, 0, sizeof *l);
}

static void ms_find_mods(ms_found_list *l, const wchar_t *dir, const wchar_t *rel, int depth, int at_root)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t *child, *crel;
        if (!ms_candidate(&fd)) continue;
        child = ms_wjoin(dir, fd.cFileName);
        crel = rel && rel[0] ? ms_wjoin(rel, fd.cFileName) : _wcsdup(fd.cFileName);
        if (!child || !crel) { free(child); free(crel); break; }
        if (at_root && depth == 0 && !_wcsicmp(fd.cFileName, L"" SH_MS_PLACES)) {
            /* The places: mods\maps\<place>\..., each walked as its own area. */
            WIN32_FIND_DATAW pd;
            wchar_t *pp = ms_wjoin(child, L"*");
            HANDLE ph = pp ? FindFirstFileExW(pp, FindExInfoBasic, &pd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
            free(pp);
            if (ph != INVALID_HANDLE_VALUE) {
                do {
                    wchar_t *place, *prel;
                    if (!ms_candidate(&pd)) continue;
                    place = ms_wjoin(child, pd.cFileName);
                    prel = ms_wjoin(crel, pd.cFileName);
                    if (place && prel) ms_find_mods(l, place, prel, 1, 0);
                    free(place);
                    free(prel);
                } while (FindNextFileW(ph, &pd));
                FindClose(ph);
            }
            free(child);
            free(crel);
            continue;
        }
        if (ms_has_descriptor(child)) {
            if (l->count == l->cap) {
                size_t ncap = l->cap ? l->cap * 2 : 16;
                ms_found *n = (ms_found *)realloc(l->items, ncap * sizeof *n);
                if (!n) { free(child); free(crel); break; }
                l->items = n;
                l->cap = ncap;
            }
            l->items[l->count].folder = child;
            l->items[l->count].rel = crel;
            l->count++;
        } else {
            if (depth + 1 < SH_MS_NEST_MAX) ms_find_mods(l, child, crel, depth + 1, 0);
            free(child);
            free(crel);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Every mod folder of the store (root and places). */
static void ms_find_store(ms_found_list *l, const wchar_t *root)
{
    memset(l, 0, sizeof *l);
    ms_find_mods(l, root, L"", 0, 1);
}

/* `path` is a mod folder of the store: strictly inside the root, holding a
 * descriptor, with no mod folder between it and the root (a folder inside a
 * mod is that mod's content, not a mod), and not mods\maps or a place folder
 * itself. */
static int ms_store_mod_folder(const wchar_t *path, const wchar_t *root)
{
    wchar_t *copy, *p;
    size_t n = wcslen(root), pl = sizeof SH_MS_PLACES - 1;
    const wchar_t *below;
    if (!ms_inside(path, root) || !ms_has_descriptor(path)) return 0;
    below = path + n + 1;
    if (!_wcsnicmp(below, L"" SH_MS_PLACES, pl) && (below[pl] == 0 || (below[pl] == L'\\' && !wcschr(below + pl + 1, L'\\'))))
        return 0;
    copy = _wcsdup(path);
    if (!copy) return 0;
    for (p = copy + n + 1; (p = wcschr(p, L'\\')) != NULL; p++) {
        int inner;
        *p = 0;
        inner = ms_has_descriptor(copy);
        *p = L'\\';
        if (inner) { free(copy); return 0; }
    }
    free(copy);
    return 1;
}

/* ------------------------------------------------------- enumeration ---- */

int sh_ms_entry_usable(const sh_ms_entry *e)
{
    return e && e->loaded && e->sizes_ok && e->validated && !e->refused;
}

int sh_ms_entry_matches(const sh_ms_entry *e, const char *spec)
{
    size_t n;
    char norm[SH_MS_NAME_MAX + 1];
    if (!e || !spec || !spec[0]) return 0;
    strncpy_s(norm, sizeof norm, spec, _TRUNCATE);
    for (n = 0; norm[n]; n++) if (norm[n] == '/') norm[n] = '\\';
    if (!_stricmp(e->name, norm)) return 1;
    if (strchr(e->name, '\\') && !strchr(norm, '\\') && !_stricmp(ms_leaf(e->name), norm)) return 1;   /* a nested folder by its own name */
    return e->salt[0] && !_stricmp(e->salt, spec);
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

static void ms_load_entry(sh_ms_list *l, const wchar_t *folder, const wchar_t *rel)
{
    sh_ms_entry *e;
    char *u, why[SH_MB_CAUSE_MAX], recorded[SH_MB_CAUSE_MAX];
    size_t i;
    if (!ms_has_descriptor(folder)) return;
    e = ms_add_entry(l);
    if (!e) return;
    e->folder = sh_mb_utf8(folder);
    u = sh_mb_utf8(rel);
    if (u) {
        strncpy_s(e->name, sizeof e->name, u, _TRUNCATE);
        strncpy_s(e->key, sizeof e->key, u, _TRUNCATE);
        free(u);
    }
    ms_place_of_name(e->name, e->place, sizeof e->place);
    why[0] = recorded[0] = 0;
    e->state = e->folder ? sh_mb_descriptor_state_why(e->folder, why, sizeof why) : SH_MB_DESC_UNREADABLE;
    if (e->folder) sh_ms_build_cause(e->folder, recorded, sizeof recorded);
    switch (e->state) {
    case SH_MB_DESC_FRESH:
        if (e->place[0]) {
            /* A place holds the builds a map brought; nothing is made there. */
            e->refused = 1;
            strcpy_s(e->cause, sizeof e->cause, SH_MB_SHAPE "it was not built by the mod store");
        } else if (recorded[0]) {
            strncpy_s(e->cause, sizeof e->cause, recorded, _TRUNCATE);
        } else {
            strcpy_s(e->cause, sizeof e->cause, "a new mod, not built yet (Refresh in the Mods tab builds it)");
        }
        return;
    case SH_MB_DESC_BUILT:
        break;
    default:
        e->refused = 1;
        if (e->state == SH_MB_DESC_UNREADABLE && e->folder && sh_ms_descriptor_unparsable(e->folder, recorded, sizeof recorded))
            strncpy_s(e->cause, sizeof e->cause, recorded, _TRUNCATE);
        else strncpy_s(e->cause, sizeof e->cause, why[0] ? why : SH_MB_SHAPE "its mod.json cannot be read", _TRUNCATE);
        return;
    }
    e->loaded = sh_mb_load_folder(e->folder, &e->bundle);
    /* The salt is read with the block even when the build is not there. */
    if (sh_salt_valid(e->bundle.id, strlen(e->bundle.id))) {
        strcpy_s(e->salt, sizeof e->salt, e->bundle.id);
        strcpy_s(e->key, sizeof e->key, e->salt);
    }
    e->installed_at = e->bundle.built;
    if (!e->loaded) {
        if (recorded[0]) strncpy_s(e->cause, sizeof e->cause, recorded, _TRUNCATE);
        else if (!strcmp(e->bundle.first_cause, "not built yet"))
            strcpy_s(e->cause, sizeof e->cause, "not built yet (Refresh in the Mods tab builds it)");
        else strncpy_s(e->cause, sizeof e->cause, e->bundle.first_cause[0] ? e->bundle.first_cause : "mod.json unreadable",
                       _TRUNCATE);
        return;
    }
    if (!sh_mb_salt_paths_check(&e->bundle, why, sizeof why)) {
        e->refused = 1;
        strncpy_s(e->cause, sizeof e->cause, why, _TRUNCATE);
        return;
    }
    e->validated = e->bundle.validated;
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

/* The copy rule's order: the first successful build of the salt in its folder
 * ("built"), a folder never built last; ties by folder path. */
static int ms_older(const sh_ms_entry *a, const sh_ms_entry *b)
{
    long long x = a->installed_at > 0 ? a->installed_at : LLONG_MAX, y = b->installed_at > 0 ? b->installed_at : LLONG_MAX;
    if (x != y) return x < y;
    return _stricmp(a->name, b->name) < 0;
}

/* Two folders of one salt in one area: at the root the older keeps the salt
 * and the others wait for the scan's new salt; in a place (only a hand copy
 * makes one) the newer is refused. */
static void ms_mark_duplicates(sh_ms_list *l)
{
    size_t i, k, *keeper;
    if (l->count < 2 || !(keeper = (size_t *)malloc(l->count * sizeof *keeper))) return;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        keeper[i] = (size_t)-1;
        if (!e->salt[0] || e->refused) continue;
        for (k = 0; k < l->count; k++) {
            const sh_ms_entry *o = &l->items[k];
            if (k == i || o->refused || !o->salt[0] || _stricmp(o->salt, e->salt) || _stricmp(o->place, e->place)) continue;
            if (ms_older(o, e) && (keeper[i] == (size_t)-1 || ms_older(o, &l->items[keeper[i]]))) keeper[i] = k;
        }
    }
    for (i = 0; i < l->count; i++) {
        sh_ms_entry *e = &l->items[i];
        if (keeper[i] == (size_t)-1) continue;
        e->refused = 1;
        if (e->place[0]) {
            _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, SH_MB_SHAPE "another folder of this map has the same mod: "
                        "mods\\%s", l->items[keeper[i]].name);
        } else {
            /* A copy the scan could not give a new salt (its binaries name
             * the salt, as an outside converter's do) says why, plainly;
             * any other waits for the scan's new salt. */
            char why[SH_MB_CAUSE_MAX];
            sh_ms_build_cause(e->folder, why, sizeof why);
            if (why[0])
                _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "%s", why);
            else
                _snprintf_s(e->cause, sizeof e->cause, _TRUNCATE, "mods\\%s is a copy of mods\\%s; Refresh in the Mods tab "
                            "makes it a new mod", e->name, l->items[keeper[i]].name);
        }
    }
    free(keeper);
}

static int ms_entry_cmp(const void *a, const void *b)
{
    const sh_ms_entry *x = (const sh_ms_entry *)a, *y = (const sh_ms_entry *)b;
    int c = _stricmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

int sh_ms_folder_is_mod(const char *folder)
{
    wchar_t *w = folder && folder[0] ? sh_mb_wpath(folder) : NULL;
    int r = w && ms_is_dir(w) && ms_has_descriptor(w);
    free(w);
    return r;
}

int sh_ms_find_mods(const char *dir, char ***out, size_t *count)
{
    wchar_t *w, *root;
    ms_found_list found;
    size_t i;
    char **list = NULL;
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!dir || !dir[0] || !out || !count) return 0;
    w = sh_mb_wpath(dir);
    if (!w || !ms_is_dir(w)) { free(w); return 0; }
    root = ms_wroot();
    memset(&found, 0, sizeof found);
    ms_find_mods(&found, w, L"", 0, root && !_wcsicmp(root, w));
    free(root);
    free(w);
    if (found.count) {
        list = (char **)calloc(found.count, sizeof *list);
        if (!list) { ms_found_free(&found); return 0; }
        for (i = 0; i < found.count; i++) list[i] = sh_mb_utf8(found.items[i].folder);
    }
    *out = list;
    *count = found.count;
    ms_found_free(&found);
    return 1;
}

void sh_ms_find_free(char **folders, size_t count)
{
    size_t i;
    if (!folders) return;
    for (i = 0; i < count; i++) free(folders[i]);
    free(folders);
}

int sh_ms_walk(sh_ms_walk_fn fn, void *ctx)
{
    wchar_t *root = ms_wroot();
    ms_found_list found;
    size_t i;
    if (!root) return 0;
    if (!fn || !ms_exists(root)) { free(root); return 1; }
    ms_find_store(&found, root);
    for (i = 0; i < found.count; i++) {
        char *folder = sh_mb_utf8(found.items[i].folder), *name = sh_mb_utf8(found.items[i].rel);
        char place[SH_MS_NAME_MAX + 1];
        int stop = 0;
        if (folder && name) {
            ms_place_of_name(name, place, sizeof place);
            stop = fn(ctx, folder, name, place);
        }
        free(folder);
        free(name);
        if (stop) break;
    }
    ms_found_free(&found);
    free(root);
    return 1;
}

int sh_ms_enumerate(sh_ms_list *out)
{
    wchar_t *root;
    ms_found_list found;
    size_t i;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    root = ms_wroot();
    if (!root) return 0;
    if (!ms_exists(root)) { free(root); return 1; }
    ms_find_store(&found, root);
    for (i = 0; i < found.count; i++) ms_load_entry(out, found.items[i].folder, found.items[i].rel);
    ms_found_free(&found);
    free(root);
    ms_mark_duplicates(out);
    if (out->count > 1) qsort(out->items, out->count, sizeof(sh_ms_entry), ms_entry_cmp);
    return 1;
}

int ms_is_hash(const char *h)
{
    size_t i;
    if (!h) return 0;
    for (i = 0; i < SH_MB_HASH_HEX; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f') || (h[i] >= 'A' && h[i] <= 'F'))) return 0;
    return h[SH_MB_HASH_HEX] == 0;
}

const sh_ms_entry *sh_ms_resolve(const sh_ms_list *l, const char *hash)
{
    const sh_ms_entry *first = NULL;
    size_t i;
    if (!l || !hash || !hash[0]) return NULL;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!e->loaded || !e->bundle.hash[0] || _stricmp(e->bundle.hash, hash)) continue;
        if (sh_ms_entry_usable(e)) return e;
        if (!first) first = e;
    }
    return first;
}

const sh_ms_entry *sh_ms_find_salt(const sh_ms_list *l, const char *salt, const char *place, const char *hash)
{
    const sh_ms_entry *first = NULL;
    size_t i;
    if (!l || !salt || !salt[0]) return NULL;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (e->refused || !e->salt[0] || _stricmp(e->salt, salt)) continue;
        if (!place) { if (e->place[0]) continue; }
        else if (!strcmp(place, "*")) { if (!e->place[0]) continue; }
        else if (_stricmp(e->place, place)) continue;
        if (hash && (!e->loaded || _stricmp(e->bundle.hash, hash))) continue;
        if (sh_ms_entry_usable(e)) return e;
        if (!first) first = e;
    }
    return first;
}

int sh_ms_note_build(const char *folder, const sh_mb_bundle *b)
{
    if (!folder || !b) return 0;
    ms_note_cause(folder, NULL);   /* a build succeeded: no earlier failure stands */
    return 1;
}

/* ------------------------------------------------------------ salts ---- */

/* Every salt a folder of the store holds (built, failed or salted only), read
 * from the blocks alone: a new salt is never one of them. */
typedef struct ms_salts { char (*s)[SH_SALT_LEN + 1]; size_t n, cap; const char *also; } ms_salts;

static void ms_salts_add(ms_salts *h, const char *salt)
{
    if (!sh_salt_valid(salt, strlen(salt))) return;
    if (h->n == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 32;
        char (*grown)[SH_SALT_LEN + 1] = realloc(h->s, cap * sizeof *grown);
        if (!grown) return;
        h->s = grown;
        h->cap = cap;
    }
    strcpy_s(h->s[h->n++], SH_SALT_LEN + 1, salt);
}

/* The block's salt of a mod.json, or "" (no block, or no valid salt). */
static void ms_block_salt(const wchar_t *folder, char out[SH_SALT_LEN + 1])
{
    wchar_t *p = ms_wjoin(folder, L"mod.json");
    size_t len = 0;
    char *text = p ? ms_read_text(p, SH_MB_DESCRIPTOR_MAX * 16, &len) : NULL;
    mpj *j = text ? mpj_parse(text, len) : NULL;
    const mpj *block = j ? mpj_get(j, SH_MB_BLOCK_KEY) : NULL, *s = block ? mpj_get(block, "salt") : NULL;
    out[0] = 0;
    if (s && s->type == MPJ_STR && s->text && sh_salt_valid(s->text, strlen(s->text))) strcpy_s(out, SH_SALT_LEN + 1, s->text);
    mpj_free(j);
    free(text);
    free(p);
}

static void ms_salts_read(ms_salts *h, const char *also)
{
    wchar_t *root = ms_wroot();
    ms_found_list found;
    size_t i;
    memset(h, 0, sizeof *h);
    h->also = also;
    if (!root) return;
    ms_find_store(&found, root);
    for (i = 0; i < found.count; i++) {
        char s[SH_SALT_LEN + 1];
        ms_block_salt(found.items[i].folder, s);
        if (s[0]) ms_salts_add(h, s);
    }
    ms_found_free(&found);
    free(root);
}

static int ms_salt_held(void *ctx, const char *salt)
{
    const ms_salts *h = (const ms_salts *)ctx;
    size_t i;
    if (h->also && !_stricmp(h->also, salt)) return 1;
    for (i = 0; i < h->n; i++) if (!_stricmp(h->s[i], salt)) return 1;
    return 0;
}

/* A new salt no folder of the store holds (nor `also`). */
static int ms_new_salt(char out[SH_SALT_LEN + 1], const char *also)
{
    ms_salts h;
    int ok;
    ms_salts_read(&h, also);
    ok = sh_salt_generate_except(out, ms_salt_held, &h);
    free(h.s);
    return ok;
}

/* sh_ms_assign_salt_as: the salt the next salting takes instead of a new one
 * (under the store lock). */
static char g_ms_fixed_salt[SH_SALT_LEN + 1];

/* The salting's plan (sh_ms_assign_salt): every file of the folder read and
 * rewritten in memory first, mods/<from>/ to mods/<salt>/ in texts and in the
 * folder segments of paths under assets/; a binary file naming mods/<from>/
 * refuses the folder. Nothing of a new folder serves before it is salted and
 * built: an entry serves only when its block records a build. */
#define MS_RK_TEXT_MAX (16u << 20)   /* a file read whole; a larger one is scanned in chunks as a binary */
#define MS_RK_CHUNK    (4u << 20)

typedef struct ms_rk_file {
    char *rel, *new_rel;      /* under the folder, '/'-separated */
    char *text;               /* rewritten bytes, or NULL to keep the file */
    size_t text_len;
    int dir;
} ms_rk_file;

typedef struct ms_rk {
    ms_rk_file *f;
    size_t n, cap;
    const char *leaf, *key;      /* from, and the new salt */
    const sh_mb_strkey *skeys;   /* the strings keys renamed "<salt>_<key>" (DECISIONS 6) */
    size_t nskeys;
    unsigned refs, rewritten, moved, srefs;
    int failed;
    char cause[SH_MB_CAUSE_MAX];
} ms_rk;

static void ms_rk_free(ms_rk *r)
{
    size_t i;
    for (i = 0; i < r->n; i++) { free(r->f[i].rel); free(r->f[i].new_rel); free(r->f[i].text); }
    free(r->f);
    r->f = NULL;
    r->n = r->cap = 0;
}

static void ms_rk_fail(ms_rk *r, const char *fmt, ...)
{
    va_list ap;
    if (r->failed) return;
    r->failed = 1;
    va_start(ap, fmt);
    _vsnprintf_s(r->cause, sizeof r->cause, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* Whether a binary file names the folder; -1 when it cannot be read. */
static int ms_rk_binary_names(const wchar_t *path, const char *leaf)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    size_t keep = strlen(leaf) + 8, have = 0;
    char *buf;
    int hit = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    buf = (char *)malloc(MS_RK_CHUNK + keep);
    if (!buf) { CloseHandle(h); return -1; }
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf + have, MS_RK_CHUNK, &got, NULL)) { hit = -1; break; }
        if (!got) break;
        have += got;
        if (sh_mb_rekey_text(buf, have, leaf, "k", NULL, NULL)) { hit = 1; break; }
        /* Keep the tail, so a name split across two chunks is still seen. */
        if (have > keep) { memmove(buf, buf + have - keep, keep); have = keep; }
    }
    free(buf);
    CloseHandle(h);
    return hit;
}

static ms_rk_file *ms_rk_add(ms_rk *r)
{
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        ms_rk_file *grown = (ms_rk_file *)realloc(r->f, cap * sizeof *grown);
        if (!grown) return NULL;
        r->f = grown;
        r->cap = cap;
    }
    memset(&r->f[r->n], 0, sizeof r->f[r->n]);
    return &r->f[r->n++];
}

/* Read the tree below `dir` (rel "" at the folder itself) into the plan. */
static void ms_rk_walk(ms_rk *r, const wchar_t *dir, const char *rel, int depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            ms_rk_fail(r, "cannot list %s: %s", rel[0] ? rel : "the folder", ms_why(GetLastError(), rel[0] ? rel : "the folder", 0, 0));
        return;
    }
    if (depth > 48) { FindClose(h); ms_rk_fail(r, "folder nesting deeper than 48: %s", rel); return; }
    do {
        char *name, child[SH_MB_REQUEST_MAX + 64], moved[SH_MB_REQUEST_MAX + 128];
        wchar_t *path;
        ms_rk_file *f;
        int changed = 0;
        if (r->failed) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { ms_rk_fail(r, "a file name under %s is not valid Unicode", rel[0] ? rel : "the folder"); break; }
        _snprintf_s(child, sizeof child, _TRUNCATE, "%s%s%s", rel, rel[0] ? "/" : "", name);
        free(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ms_rk_fail(r, "symbolic link or junction not allowed: %s", child); break; }
        /* The descriptor is rewritten on its own (sh_mb_salt_modjson). */
        if (!rel[0] && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            (!_stricmp(child, "mod.json") || !_stricmp(child, "package.json")))
            continue;
        strcpy_s(moved, sizeof moved, child);
        if (!_strnicmp(child, "assets/", 7)) {
            /* "assets/x/mods/<from>/y" -> "assets/x/mods/<salt>/y"; a
             * directory is probed with a trailing '/' so its own last
             * segment counts as a folder segment. */
            char probe[SH_MB_REQUEST_MAX + 130], tail[SH_MB_REQUEST_MAX + 130];
            int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, c;
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s%s", child + 7, dir ? "/" : "");
            c = sh_mb_rekey_path(probe, r->leaf, r->key, tail, sizeof tail);
            if (c < 0) { ms_rk_fail(r, "path too long once renamed: %s", child); break; }
            if (dir) { size_t tl = strlen(tail); if (tl && tail[tl - 1] == '/') tail[tl - 1] = 0; }
            if (c > 0) { _snprintf_s(moved, sizeof moved, _TRUNCATE, "assets/%s", tail); changed = 1; }
        }
        path = ms_wjoin(dir, fd.cFileName);
        f = path ? ms_rk_add(r) : NULL;
        if (!f || !(f->rel = _strdup(child)) || !(f->new_rel = _strdup(moved))) {
            free(path);
            ms_rk_fail(r, "out of memory");
            break;
        }
        if (changed && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) r->moved++;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            f->dir = 1;
            ms_rk_walk(r, path, child, depth + 1);
        } else {
            unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            size_t len = 0;
            char *t = size <= MS_RK_TEXT_MAX ? ms_read_text(path, MS_RK_TEXT_MAX, &len) : NULL;
            if (size <= MS_RK_TEXT_MAX && !t) {
                ms_rk_fail(r, "cannot read %s (%s); the folder is unchanged", child, ms_why(GetLastError(), child, 0, 0));
            } else if (t && !memchr(t, 0, len)) {
                /* A text: decl text and anything else without a NUL byte.
                 * mods/<from>/ first, then the #str_ references of the
                 * renamed strings keys. */
                char *out = NULL, *out2 = NULL;
                size_t out_len = 0, out2_len = 0, s = 0, c = sh_mb_rekey_text(t, len, r->leaf, r->key, &out, &out_len);
                if (c != (size_t)-1 && r->nskeys)
                    s = sh_mb_restr_text(c ? out : t, c ? out_len : len, r->skeys, r->nskeys, &out2, &out2_len);
                if (c == (size_t)-1 || s == (size_t)-1) { free(out); ms_rk_fail(r, "out of memory"); }
                else {
                    if (s) { free(out); out = out2; out_len = out2_len; r->srefs += (unsigned)s; }
                    if (c || s) { f->text = out; f->text_len = out_len; r->refs += (unsigned)c; r->rewritten++; }
                }
            } else {
                int hit = t ? (sh_mb_rekey_text(t, len, r->leaf, r->key, NULL, NULL) > 0) : ms_rk_binary_names(path, r->leaf);
                int shit = t && r->nskeys && sh_mb_restr_text(t, len, r->skeys, r->nskeys, NULL, NULL) > 0;
                if (hit < 0) ms_rk_fail(r, "cannot read %s (%s); the folder is unchanged", child, ms_why(GetLastError(), child, 0, 0));
                else if (hit)
                    ms_rk_fail(r, SH_MB_SHAPE "%s names mods/%s/ inside a binary file, which Snapmap+ cannot rewrite; "
                               "the folder is unchanged", child, r->leaf);
                else if (shit)
                    ms_rk_fail(r, SH_MB_SHAPE "%s names one of its strings (#str_) inside a binary file, which Snapmap+ "
                               "cannot rewrite; the folder is unchanged", child);
            }
            free(t);
        }
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int ms_rk_rel_cmp(const void *a, const void *b)
{
    return _stricmp(((const ms_rk_file *)a)->new_rel, ((const ms_rk_file *)b)->new_rel);
}

/* Build the staged folder: directories, links (or copies), rewritten texts
 * and the new mod.json. */
static int ms_rk_stage(ms_rk *r, const wchar_t *src, const wchar_t *staging, int link, const char *modjson, size_t mlen)
{
    size_t i;
    wchar_t *mj;
    if (!ms_mkdirs(staging)) { ms_rk_fail(r, "cannot create the staging folder: %s", ms_why(GetLastError(), NULL, 0, 0)); return 0; }
    for (i = 0; i < r->n && !r->failed; i++) {
        const ms_rk_file *f = &r->f[i];
        wchar_t *from = ms_wjoin_utf8(src, f->rel), *to = ms_wjoin_utf8(staging, f->new_rel);
        int ok;
        if (!from || !to) ok = 0;
        else if (f->dir) ok = ms_mkdirs(to);
        else if (f->text) ok = ms_mkdirs_parent(to) && ms_write_text(to, f->text, f->text_len);
        else if (link) ok = ms_mkdirs_parent(to) && (CreateHardLinkW(to, from, NULL) || sh_ms_copy_file(from, to));
        else ok = sh_ms_copy_file(from, to);
        if (!ok) ms_rk_fail(r, "cannot stage %s (%s); the folder is unchanged", f->rel, ms_why(GetLastError(), f->rel, 0, 0));
        free(from);
        free(to);
    }
    if (r->failed) return 0;
    mj = ms_wjoin(staging, L"mod.json");
    if (!mj || !ms_write_text(mj, modjson, mlen))
        ms_rk_fail(r, "cannot write the new mod.json (%s); the folder is unchanged", ms_why(GetLastError(), "mod.json", 0, 0));
    free(mj);
    return !r->failed;
}

/* The salting of `folder` (rule 1): in memory first, then staged beside it
 * and swapped in under the same name. `from` is the folder name, the package
 * id it was converted from, or the salt a copy carried. The folder's own
 * mod.json is read unless `given` (a fresh author text) replaces it. With
 * `dest` the salted folder is written there instead (it must not exist) and
 * `folder` is left as it is. Returns 1 with the salt, -1 with the cause (the
 * folder untouched). The caller holds the store lock. */
static int ms_assign_to(const char *folder, const char *given, size_t given_len, const char *from, const char *dest,
                        char salt_out[SH_SALT_LEN + 1], char *cause, size_t ccap)
{
    ms_rk r;
    char salt[SH_SALT_LEN + 1];
    wchar_t *src = NULL, *staging = NULL, *old = NULL, *mj, *parent = NULL, *slash;
    char *author = NULL, *modjson = NULL, *keyed = NULL;
    sh_mb_strkey *skeys = NULL;
    size_t alen = 0, mlen = 0, i, drefs, nskeys = 0, srefs = 0;
    int ok = -1;
    memset(&r, 0, sizeof r);
    if (salt_out) salt_out[0] = 0;
    src = sh_mb_wpath(folder);
    if (!src || !(slash = wcsrchr(src, L'\\')) || !(parent = _wcsdup(src))) { ms_cause(cause, ccap, "out of memory"); goto done; }
    parent[slash - src] = 0;
    if (g_ms_fixed_salt[0]) strcpy_s(salt, sizeof salt, g_ms_fixed_salt);
    else if (!ms_new_salt(salt, from)) { ms_cause(cause, ccap, "no new salt could be drawn (the system's random generator failed)"); goto done; }
    r.leaf = from;
    r.key = salt;
    /* Read and rewrite everything in memory first. */
    if (given) {
        author = (char *)malloc(given_len + 1);
        if (author) { memcpy(author, given, given_len); author[given_len] = 0; alen = given_len; }
        else { ms_cause(cause, ccap, "out of memory"); goto done; }
    } else {
        mj = ms_wjoin(src, L"mod.json");
        author = ms_read_text(mj, SH_MB_DESCRIPTOR_MAX * 16, &alen);
        free(mj);
    }
    if (!author) { ms_cause(cause, ccap, "cannot read mod.json (%s); the folder is unchanged", ms_why(GetLastError(), "mod.json", 0, 0)); goto done; }
    drefs = sh_mb_rekey_text(author, alen, from, salt, NULL, NULL);
    if (drefs == (size_t)-1) { ms_cause(cause, ccap, "out of memory"); goto done; }
    modjson = sh_mb_salt_modjson(author, alen, from, salt, &mlen);
    if (!modjson) { ms_cause(cause, ccap, SH_MB_SHAPE "its mod.json is not a JSON object"); goto done; }
    /* Its strings keys gain the salt's prefix, and every #str_ reference
     * to them in its own text files (mod.json among them) follows. */
    keyed = sh_mb_salt_strings(modjson, mlen, from, salt, &skeys, &nskeys, &i);
    if (!keyed) { ms_cause(cause, ccap, SH_MB_SHAPE "its mod.json does not read as JSON"); goto done; }
    free(modjson);
    modjson = keyed;
    mlen = i;
    keyed = NULL;
    if (nskeys) {
        srefs = sh_mb_restr_text(modjson, mlen, skeys, nskeys, &keyed, &i);
        if (srefs == (size_t)-1) { ms_cause(cause, ccap, "out of memory"); goto done; }
        if (srefs) { free(modjson); modjson = keyed; mlen = i; keyed = NULL; }
    }
    r.skeys = skeys;
    r.nskeys = nskeys;
    r.refs = (unsigned)drefs;
    ms_rk_walk(&r, src, "", 0);
    if (!r.failed && r.n > 1) {
        qsort(r.f, r.n, sizeof *r.f, ms_rk_rel_cmp);
        for (i = 1; i < r.n && !r.failed; i++)
            if (!_stricmp(r.f[i - 1].new_rel, r.f[i].new_rel) && !(r.f[i - 1].dir && r.f[i].dir))
                ms_rk_fail(&r, "%s and %s would both become %s; the folder is unchanged", r.f[i - 1].rel, r.f[i].rel,
                           r.f[i].new_rel);
    }
    if (!r.failed) {
        /* Files under another mod's mods/<x>/ (x neither `from` nor the new
         * salt) would be refused by the build: refuse before anything moves. */
        unsigned files = 0, foreign = 0;
        const char *first = NULL;
        char other[SH_MB_ID_MAX + 1] = "";
        for (i = 0; i < r.n; i++) {
            const char *p, *e;
            if (r.f[i].dir) continue;
            files++;
            p = !_strnicmp(r.f[i].new_rel, "assets/", 7) ? strstr(r.f[i].new_rel, "/mods/") : NULL;
            e = p ? strchr(p + 6, '/') : NULL;
            if (!e || ((size_t)(e - (p + 6)) == SH_SALT_LEN && !_strnicmp(p + 6, salt, SH_SALT_LEN))) continue;
            if (!foreign++) {
                first = r.f[i].rel;
                _snprintf_s(other, sizeof other, _TRUNCATE, "%.*s", (int)(e - (p + 6)), p + 6);
            }
        }
        if (foreign)
            ms_rk_fail(&r, SH_MB_SHAPE "%u of its %u file(s) are under mods/%s/ (the first: %s), not mods/%s/; move them "
                       "under mods/%s/ and Refresh", foreign, files, other, first, from, from);
    }
    if (r.failed) { ms_cause(cause, ccap, "%s", r.cause); goto done; }
    if (dest) {
        /* Written to `dest`; the folder itself is not touched. */
        wchar_t *to = sh_mb_wpath(dest);
        if (!to || ms_exists(to)) {
            ms_cause(cause, ccap, to ? "the work folder %s already exists" : "out of memory", dest);
            free(to);
            goto done;
        }
        if (!ms_rk_stage(&r, src, to, 1, modjson, mlen)) {
            wchar_t *tp = _wcsdup(to), *ts = tp ? wcsrchr(tp, L'\\') : NULL;
            if (ts) { *ts = 0; ms_rmtree(to, tp); }
            free(tp);
            free(to);
            ms_cause(cause, ccap, "%s", r.cause);
            goto done;
        }
        free(to);
        if (salt_out) strcpy_s(salt_out, SH_SALT_LEN + 1, salt);
        ok = 1;
        goto done;
    }
    {
        size_t n = wcslen(src) + 32;
        staging = (wchar_t *)malloc(sizeof(wchar_t) * n);
        old = (wchar_t *)malloc(sizeof(wchar_t) * n);
        if (!staging || !old) { ms_cause(cause, ccap, "out of memory"); goto done; }
        _snwprintf_s(staging, n, _TRUNCATE, L"%s.staging-%lu", src, GetCurrentProcessId());
        _snwprintf_s(old, n, _TRUNCATE, L"%s.old-%lu", src, GetCurrentProcessId());
    }
    /* Assemble beside it (unchanged files hard-linked), then swap. */
    if (ms_exists(staging)) ms_rmtree(staging, parent);
    if (!ms_rk_stage(&r, src, staging, 1, modjson, mlen)) {
        ms_rmtree(staging, parent);
        ms_cause(cause, ccap, "%s", r.cause);
        goto done;
    }
    if (ms_exists(old)) ms_rmtree(old, parent);
    if (!ms_move_folder(src, old)) {
        DWORD err = GetLastError();
        ms_rmtree(staging, parent);
        ms_cause(cause, ccap, "the folder could not be set aside to take its salt (%s); the folder is unchanged",
                 ms_why(err, ms_rel(folder), 0, 0));
        goto done;
    }
    if (!ms_move_folder(staging, src)) {
        DWORD err = GetLastError();
        if (!ms_move_folder(old, src))
            ms_log("MODS: store: %s: the folder could not be put back from %ls (%s)", ms_rel(folder), old,
                   ms_why(GetLastError(), ms_rel(folder), 0, 0));
        ms_rmtree(staging, parent);
        ms_cause(cause, ccap, "the salted folder could not be put in place (%s); the folder is unchanged",
                 ms_why(err, ms_rel(folder), 0, 0));
        goto done;
    }
    {
        /* The folder as it was goes to the Recycle Bin (its files are the
         * new folder's too, hard-linked); outright when the bin cannot take it. */
        char why[200];
        if (!ms_recycle(old, why, sizeof why) && !ms_rmtree(old, parent))
            ms_log("MODS: store: %s: the folder as it was before its salt, %ls, could not be removed completely; it is not "
                   "a mod (its name ends in .old-)", ms_rel(folder), old);
    }
    ms_log("MODS: store: mods\\%s is a new mod: salt %s; %u reference(s) to mods/%s/ rewritten in %u file(s)%s, %u file(s) "
           "moved under mods/%s/; %zu strings key(s) named %s_<key>, %u #str_ reference(s) to them rewritten",
           ms_rel(folder), salt, r.refs, from, r.rewritten + (drefs ? 1u : 0u), drefs ? " (mod.json among them)" : "", r.moved,
           salt, nskeys, salt, r.srefs + (unsigned)srefs);
    if (salt_out) strcpy_s(salt_out, SH_SALT_LEN + 1, salt);
    ok = 1;
done:
    ms_rk_free(&r);
    sh_mb_strkeys_free(skeys, nskeys);
    free(keyed);
    free(author);
    free(modjson);
    free(src);
    free(parent);
    free(staging);
    free(old);
    return ok;
}

static int ms_assign(const char *folder, const char *from, char salt_out[SH_SALT_LEN + 1], char *cause, size_t ccap)
{
    return ms_assign_to(folder, NULL, 0, from, NULL, salt_out, cause, ccap);
}

/* Whether `folder` lies in a place of the store (mods\maps\<segment>\...). */
static int ms_in_place(const char *folder)
{
    char root[MAX_PATH * 4], place[SH_MS_NAME_MAX + 1];
    const char *p = ms_plain(folder);
    size_t n;
    if (!p || !sh_ms_root(root, sizeof root)) return 0;
    n = strlen(root);
    if (_strnicmp(p, root, n) || (p[n] != '\\' && p[n] != '/')) return 0;
    return ms_place_of_name(p + n + 1, place, sizeof place);
}

int sh_ms_assign_salt(const char *folder, const char *from, char salt_out[SH_SALT_LEN + 1], char *cause, size_t ccap)
{
    char why[SH_MB_CAUSE_MAX];
    int state, r;
    if (salt_out) salt_out[0] = 0;
    if (cause && ccap) cause[0] = 0;
    if (!folder || !folder[0] || !from || !from[0]) return ms_cause(cause, ccap, "no folder named") - 1;
    modstore_lock();
    state = sh_mb_descriptor_state_why(folder, why, sizeof why);
    if (state == SH_MB_DESC_BUILT) {
        if (salt_out) {
            wchar_t *w = sh_mb_wpath(folder);
            if (w) ms_block_salt(w, salt_out);
            free(w);
        }
        r = 0;
    } else if (state != SH_MB_DESC_FRESH) {
        ms_cause(cause, ccap, "%s", why);
        r = -1;
    } else if (ms_in_place(folder)) {
        ms_cause(cause, ccap, SH_MB_SHAPE "it was not built by the mod store");
        r = -1;
    } else {
        r = ms_assign_to(folder, NULL, 0, from, NULL, salt_out, cause, ccap);
    }
    if (r < 0) ms_note_cause(folder, cause);
    else if (r > 0) ms_note_cause(folder, NULL);
    modstore_unlock();
    return r;
}

int sh_ms_assign_salt_as(const char *folder, const char *from, const char *salt, char *cause, size_t ccap)
{
    char got[SH_SALT_LEN + 1];
    int r;
    if (!salt || !sh_salt_valid(salt, strlen(salt))) return ms_cause(cause, ccap, "no salt given") - 1;
    modstore_lock();
    strcpy_s(g_ms_fixed_salt, sizeof g_ms_fixed_salt, salt);
    r = sh_ms_assign_salt(folder, from, got, cause, ccap);
    g_ms_fixed_salt[0] = 0;
    modstore_unlock();
    return r;
}

int sh_ms_salt_copy(const char *folder, const char *modjson, size_t modjson_len, const char *from, const char *dest,
                    char salt_out[SH_SALT_LEN + 1], char *cause, size_t ccap)
{
    int r;
    if (salt_out) salt_out[0] = 0;
    if (cause && ccap) cause[0] = 0;
    if (!folder || !folder[0] || !from || !from[0] || !dest || !dest[0] || !modjson)
        return ms_cause(cause, ccap, "no folder named") - 1;
    modstore_lock();
    r = ms_assign_to(folder, modjson, modjson_len, from, dest, salt_out, cause, ccap);
    modstore_unlock();
    return r;
}

/* ------------------------------------------------- the store's names ---- */

int sh_ms_title_segment(const char *title, char *out, size_t cap)
{
    char seg[64 + 8];
    size_t w = 0, i, n, stem;
    int space = 0, device = 0;
    if (out && cap) out[0] = 0;
    if (!title || !out || cap < 2) return 0;
    for (i = 0; title[i] && w < 60; i++) {
        unsigned char c = (unsigned char)title[i];
        size_t len = 1;
        if (c < 0x20 || c == 0x7F || c == ' ' || strchr("<>:\"/\\|?*", c)) { space = w > 0; continue; }
        if (c >= 0x80) {
            /* One whole UTF-8 sequence, or nothing of it. */
            len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
            if (!len) continue;
            for (n = 1; n < len; n++) if (((unsigned char)title[i + n] & 0xC0) != 0x80) break;
            if (n < len) continue;
        }
        if (w + (space ? 1 : 0) + len > 60) break;
        if (space) { seg[w++] = ' '; space = 0; }
        memcpy(seg + w, title + i, len);
        w += len;
        i += len - 1;
    }
    seg[w] = 0;
    /* No leading dot (a dot folder is skipped), no trailing dot or space. */
    for (i = 0; seg[i] == '.' || seg[i] == ' '; i++) {}
    if (i) { memmove(seg, seg + i, w - i + 1); w -= i; }
    while (w && (seg[w - 1] == '.' || seg[w - 1] == ' ')) seg[--w] = 0;
    if (!w) return 0;
    {
        /* The listing skips work folders by these suffixes. */
        char *p;
        while ((p = strstr(seg, ".staging-")) != NULL || (p = strstr(seg, ".old-")) != NULL) *p = '_';
    }
    stem = strcspn(seg, ".");
    if (stem == 3 && (!_strnicmp(seg, "con", 3) || !_strnicmp(seg, "prn", 3) || !_strnicmp(seg, "aux", 3) ||
                      !_strnicmp(seg, "nul", 3)))
        device = 1;
    if (stem == 4 && (!_strnicmp(seg, "com", 3) || !_strnicmp(seg, "lpt", 3)) && seg[3] >= '1' && seg[3] <= '9')
        device = 1;
    if (device) {
        memmove(seg + stem + 1, seg + stem, w - stem + 1);
        seg[stem] = '_';
        w++;
    }
    strncpy_s(out, cap, seg, _TRUNCATE);
    return 1;
}

/* ------------------------------------------------------------ places ---- */

#define MS_PLACES_SECTION "places"

/* The record's "places" section, parsed (an empty object when absent); NULL
 * only when it cannot be read or without memory. Under sh_mb_record_lock. */
static mpj *ms_pl_load(const char *rootu)
{
    char *text = NULL;
    mpj *doc = NULL;
    int got = sh_mb_record_get(rootu, MS_PLACES_SECTION, &text);
    if (got < 0) { free(text); return NULL; }
    if (got == 1 && text) doc = mpj_parse(text, strlen(text));
    free(text);
    if (doc && doc->type != MPJ_OBJ) { mpj_free(doc); doc = NULL; }
    return doc ? doc : mpj_new(MPJ_OBJ);
}

static int ms_pl_save(const char *rootu, const mpj *doc)
{
    size_t len = 0;
    char *text;
    int ok;
    if (!doc) return 0;
    if (!doc->n) return sh_mb_record_put(rootu, MS_PLACES_SECTION, NULL);
    text = mpj_dump(doc, &len);
    ok = text && sh_mb_record_put(rootu, MS_PLACES_SECTION, text);
    free(text);
    return ok;
}

/* `s` as an escaped JSON string body (no quotes), malloc'd. */
static char *ms_pl_escape(const char *s)
{
    size_t n = strlen(s), w = 0, i;
    char *o = (char *)malloc(n * 6 + 1);
    if (!o) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[w++] = '\\'; o[w++] = (char)c; }
        else if (c < 0x20) w += (size_t)_snprintf_s(o + w, 7, _TRUNCATE, "\\u%04x", c);
        else o[w++] = (char)c;
    }
    o[w] = 0;
    return o;
}

/* An escaped string body decoded into `out`. */
static int ms_pl_decode(const char *body, char *out, size_t cap)
{
    char quoted[SH_MS_NAME_MAX * 2 + 8];
    size_t got = 0;
    if (!body || !out || !cap) return 0;
    _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", body);
    return sh_json_decode_string(quoted, strlen(quoted), out, cap, &got) != 0;
}

/* The section's key for a place: its key, else "title:<segment>". */
static int ms_pl_key(const sh_ms_place *place, char *out, size_t cap)
{
    char seg[72];
    if (!place) return 0;
    if (place->map_id && place->map_id[0]) { strncpy_s(out, cap, place->map_id, _TRUNCATE); return 1; }
    if (place->title && sh_ms_title_segment(place->title, seg, sizeof seg)) {
        _snprintf_s(out, cap, _TRUNCATE, "title:%s", seg);
        return 1;
    }
    return 0;
}

/* The folder (decoded) the entry at index i of the section names, or 0. */
static int ms_pl_folder(const mpj *doc, int i, char *out, size_t cap)
{
    const mpj *f = mpj_get(doc->items[i], "folder");
    return f && f->type == MPJ_STR && f->text && ms_pl_decode(f->text, out, cap);
}

/* 1 when an entry of the section other than `self` names `dir`. */
static int ms_pl_taken(const mpj *doc, const char *self_body, const char *dir)
{
    int i;
    for (i = 0; doc && i < doc->n; i++) {
        char f[SH_MS_NAME_MAX + 1];
        if (self_body && !strcmp(doc->keys[i], self_body)) continue;
        if (ms_pl_folder(doc, i, f, sizeof f) && !_stricmp(f, dir)) return 1;
    }
    return 0;
}

int sh_ms_place_dir(const sh_ms_place *place, char *out, size_t cap)
{
    char rootu[MAX_PATH * 4], key[SH_MS_NAME_MAX + 1], seg[72], dir[SH_MS_NAME_MAX + 1], *ek;
    mpj *doc;
    const mpj *entry;
    int ok = 0, round;
    if (out && cap) out[0] = 0;
    if (!place || !out || !cap || !sh_ms_root(rootu, sizeof rootu) || !ms_pl_key(place, key, sizeof key)) return 0;
    if (!place->title || !sh_ms_title_segment(place->title, seg, sizeof seg)) strcpy_s(seg, sizeof seg, "map");
    if (!(ek = ms_pl_escape(key))) return 0;
    sh_mb_record_lock();
    doc = ms_pl_load(rootu);
    entry = doc ? mpj_get(doc, ek) : NULL;
    if (entry && entry->type == MPJ_OBJ) {
        int i;
        for (i = 0; i < doc->n; i++)
            if (doc->items[i] == entry && ms_pl_folder(doc, i, dir, sizeof dir)) { strncpy_s(out, cap, dir, _TRUNCATE); ok = 1; }
    }
    for (round = 0; !ok && doc && round < 12; round++) {
        char kseg[72];
        if (round == 0) _snprintf_s(dir, sizeof dir, _TRUNCATE, SH_MS_PLACES "\\%s", seg);
        else if (round == 1) {
            /* Another map has the title: this map's place carries its key. */
            const char *k = place->map_id && place->map_id[0] ? place->map_id : NULL;
            if (!k || !sh_ms_title_segment(!strncmp(k, "zip:", 4) ? k + 4 : k, kseg, 41)) continue;
            _snprintf_s(dir, sizeof dir, _TRUNCATE, SH_MS_PLACES "\\%s (%s)", seg, kseg);
        } else _snprintf_s(dir, sizeof dir, _TRUNCATE, SH_MS_PLACES "\\%s (%d)", seg, round);
        if (!ms_pl_taken(doc, ek, dir)) { strncpy_s(out, cap, dir, _TRUNCATE); ok = 1; }
    }
    mpj_free(doc);
    sh_mb_record_unlock();
    free(ek);
    return ok;
}

int sh_ms_place_folder(const sh_ms_place *place, const char *name, const char *salt, char *out, size_t cap)
{
    char rootu[MAX_PATH * 4], dir[SH_MS_NAME_MAX + 1], seg[72], path[MAX_PATH * 4];
    wchar_t *w;
    int round;
    if (out && cap) out[0] = 0;
    if (!out || !cap || !sh_ms_root(rootu, sizeof rootu) || !sh_ms_place_dir(place, dir, sizeof dir)) return 0;
    if (salt && salt[0]) {
        /* One build per salt per place: the place's folder of it is the target. */
        ms_found_list found;
        size_t i;
        int hit = 0;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", rootu, dir);
        w = sh_mb_wpath(path);
        memset(&found, 0, sizeof found);
        if (w && ms_is_dir(w)) ms_find_mods(&found, w, L"", 1, 0);
        for (i = 0; i < found.count && !hit; i++) {
            char s[SH_SALT_LEN + 1], *rel;
            ms_block_salt(found.items[i].folder, s);
            if (!s[0] || _stricmp(s, salt) || !(rel = sh_mb_utf8(found.items[i].rel))) continue;
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, rel);
            free(rel);
            hit = 1;
        }
        ms_found_free(&found);
        free(w);
        if (hit) return 2;
    }
    if (!name || !sh_ms_title_segment(name, seg, sizeof seg)) strcpy_s(seg, sizeof seg, "mod");
    for (round = 1; round < 64; round++) {
        char leaf[96];
        if (round == 1) strcpy_s(leaf, sizeof leaf, seg);
        else _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s (%d)", seg, round);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\%s", rootu, dir, leaf);
        w = sh_mb_wpath(path);
        if (w && !ms_exists(w)) {
            free(w);
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, leaf);
            return 1;
        }
        free(w);
    }
    return 0;
}

int sh_ms_place_note(const sh_ms_place *place, const char *dir_in)
{
    char rootu[MAX_PATH * 4], key[SH_MS_NAME_MAX + 1], dir[SH_MS_NAME_MAX + 1], *ek = NULL, *et = NULL, *ef = NULL;
    mpj *doc, *entry;
    int ok = 0;
    if (!place || !sh_ms_root(rootu, sizeof rootu) || !ms_pl_key(place, key, sizeof key)) return 0;
    if (dir_in && dir_in[0]) {
        char *c;
        strncpy_s(dir, sizeof dir, dir_in, _TRUNCATE);
        for (c = dir; *c; c++) if (*c == '/') *c = '\\';
        {
            /* A mod's folder in the place names the place it lies in. */
            char up[SH_MS_NAME_MAX + 1];
            if (ms_place_of_name(dir, up, sizeof up)) strcpy_s(dir, sizeof dir, up);
        }
    } else if (!sh_ms_place_dir(place, dir, sizeof dir)) return 0;
    ek = ms_pl_escape(key);
    et = ms_pl_escape(place->title ? place->title : "");
    ef = ms_pl_escape(dir);
    if (!ek || !et || !ef) goto out;
    sh_mb_record_lock();
    doc = ms_pl_load(rootu);
    entry = doc ? mpj_new(MPJ_OBJ) : NULL;
    if (entry) {
        (void)mpj_set(entry, "title", mpj_new_str(et));
        (void)mpj_set(entry, "folder", mpj_new_str(ef));
        ok = mpj_set(doc, ek, entry) && ms_pl_save(rootu, doc);
    }
    if (!ok) ms_log("MODS: record: the place of map %s (mods\\%s) could not be noted", key, dir);
    mpj_free(doc);
    sh_mb_record_unlock();
out:
    free(ek);
    free(et);
    free(ef);
    return ok;
}

int sh_ms_place_of(const char *key, char *dir, size_t dir_cap, char *title, size_t title_cap)
{
    char rootu[MAX_PATH * 4], *ek;
    mpj *doc;
    int i, found = 0;
    if (dir && dir_cap) dir[0] = 0;
    if (title && title_cap) title[0] = 0;
    if (!key || !key[0] || !sh_ms_root(rootu, sizeof rootu) || !(ek = ms_pl_escape(key))) return 0;
    sh_mb_record_lock();
    doc = ms_pl_load(rootu);
    sh_mb_record_unlock();
    for (i = 0; doc && i < doc->n && !found; i++) {
        const mpj *t;
        if (strcmp(doc->keys[i], ek)) continue;
        found = 1;
        if (dir && dir_cap) found = ms_pl_folder(doc, i, dir, dir_cap);
        t = mpj_get(doc->items[i], "title");
        if (found && title && title_cap && t && t->type == MPJ_STR && t->text) ms_pl_decode(t->text, title, title_cap);
    }
    mpj_free(doc);
    free(ek);
    return found;
}

int sh_ms_place_title(const char *folder, char *title, size_t cap)
{
    char rootu[MAX_PATH * 4], place[SH_MS_NAME_MAX + 1], name[SH_MS_NAME_MAX + 1], *c;
    mpj *doc;
    int i, found = 0;
    if (title && cap) title[0] = 0;
    if (!folder || !sh_ms_root(rootu, sizeof rootu)) return 0;
    /* An absolute folder is named by its path under mods\; a name is taken as it is. */
    strncpy_s(name, sizeof name, folder[0] && (folder[1] == ':' || folder[0] == '\\') ? ms_rel(ms_plain(folder)) : folder,
              _TRUNCATE);
    for (c = name; *c; c++) if (*c == '/') *c = '\\';
    if (!ms_place_of_name(name, place, sizeof place)) {
        /* The place folder itself. */
        size_t pl = sizeof SH_MS_PLACES - 1;
        if (_strnicmp(name, SH_MS_PLACES "\\", pl + 1) || !name[pl + 1] || strchr(name + pl + 1, '\\')) return 0;
        strcpy_s(place, sizeof place, name);
    }
    sh_mb_record_lock();
    doc = ms_pl_load(rootu);
    sh_mb_record_unlock();
    for (i = 0; doc && i < doc->n && !found; i++) {
        char f[SH_MS_NAME_MAX + 1];
        const mpj *t;
        if (!ms_pl_folder(doc, i, f, sizeof f) || _stricmp(f, place)) continue;
        t = mpj_get(doc->items[i], "title");
        if (t && t->type == MPJ_STR && t->text && t->text[0]) found = ms_pl_decode(t->text, title, cap);
    }
    mpj_free(doc);
    return found;
}

/* After a folder in a place left: the place folder goes once nothing is left
 * in it (RemoveDirectoryW removes only an empty folder), and mods\maps\ when
 * it is empty too; the record forgets a place whose folder is gone. */
static void ms_place_prune(const wchar_t *path, const wchar_t *root)
{
    size_t rl = wcslen(root), pl = sizeof SH_MS_PLACES - 1;
    wchar_t *copy, *slash;
    char rootu[MAX_PATH * 4];
    mpj *doc;
    int i, changed = 0;
    if (!ms_inside(path, root) || _wcsnicmp(path + rl + 1, L"" SH_MS_PLACES, pl) || path[rl + 1 + pl] != L'\\') return;
    if (!(copy = _wcsdup(path))) return;
    while ((slash = wcsrchr(copy, L'\\')) != NULL && (size_t)(slash - copy) > rl) {
        *slash = 0;
        if (!RemoveDirectoryW(copy)) break;
    }
    free(copy);
    if (!sh_ms_root(rootu, sizeof rootu)) return;
    sh_mb_record_lock();
    doc = ms_pl_load(rootu);
    for (i = 0; doc && i < doc->n; i++) {
        char f[SH_MS_NAME_MAX + 1], full[MAX_PATH * 4];
        wchar_t *w;
        int gone;
        if (!ms_pl_folder(doc, i, f, sizeof f)) continue;
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", rootu, f);
        w = sh_mb_wpath(full);
        gone = w && !ms_exists(w);
        free(w);
        if (!gone) continue;
        mpj_free(doc->items[i]);
        free(doc->keys[i]);
        memmove(&doc->items[i], &doc->items[i + 1], sizeof *doc->items * (size_t)(doc->n - i - 1));
        memmove(&doc->keys[i], &doc->keys[i + 1], sizeof *doc->keys * (size_t)(doc->n - i - 1));
        doc->n--;
        i--;
        changed = 1;
    }
    if (doc && changed) (void)ms_pl_save(rootu, doc);
    mpj_free(doc);
    sh_mb_record_unlock();
}

/* ---------------------------------------------------------- new mod ---- */

/* New mod's example (DESIGN-additive 2.2): a crate that places and plays
 * untouched. Its entityDef is the game's SnapMap crate01 under the mod's salt,
 * its Toybox row is the game's crate01 row (snapeditorentitydef
 * prop/new/crate01: the prop_base parent, its preview, size, arrow and sounds)
 * pointing at it, and mod.json's strings give the row its name, its
 * description and a Toybox category named after the mod. Nothing binary: the
 * model and the preview image are the game's. The row draws the entity's own
 * model as the stock prop rows do; it names no renderInfo.renderModelName,
 * which only rows whose entity has no model of its own carry (pickups,
 * encounters, logic); the earlier example's flooded the editor log with
 * "modelMatrix invert failed on model .../crate01.lwo" every frame (V13).
 * The entityDef is the game's text whole, editorVars included: V14 saw the
 * same line while the Toybox previewed or the crosshair hovered the crate
 * with the block left out. */
#define MS_EXAMPLE_DECL "example_crate"
#define MS_EXAMPLE_DEF_DIR "assets\\generated\\decls\\entitydef\\mods\\"
#define MS_EXAMPLE_ROW_DIR "assets\\generated\\decls\\snapeditorentitydef\\mods\\"

static const char MS_EXAMPLE_DEF[] =
    "{\r\n"
    "\tclass = \"idProp_Coop\";\r\n"
    "\teditorVars {\r\n"
    "\t\trotatable = false;\r\n"
    "\t\tshowOrientation = false;\r\n"
    "\t\tshowFixedBox = false;\r\n"
    "\t\tshowInRenderMode = false;\r\n"
    "\t\tplaceable = false;\r\n"
    "\t\tplaceableInLayers = false;\r\n"
    "\t\tcolor = ( 1, 1, 1, 1 );\r\n"
    "\t\toriginOffset = ( 0, 0, 0 );\r\n"
    "\t\tmaterial = \"\";\r\n"
    "\t\teditorName = \"\";\r\n"
    "\t\tdescription = \"\";\r\n"
    "\t}\r\n"
    "\tedit = {\r\n"
    "\t\trenderModelInfo = {\r\n"
    "\t\t\tmodel = \"models/snapmaps/props/new/crate01.lwo\";\r\n"
    "\t\t}\r\n"
    "\t\tclipModelInfo = {\r\n"
    "\t\t\ttype = \"CLIPMODEL_AUTO\";\r\n"
    "\t\t}\r\n"
    "\t\tdynamicObstacle = true;\r\n"
    "\t}\r\n"
    "}\r\n";

/* Each %s is the salt. */
static const char MS_EXAMPLE_ROW[] =
    "{\r\n"
    "\tinherit = \"prop/prop_base\";\r\n"
    "\tedit = {\r\n"
    "\t\tpreviewImage = \"textures/guis/snapmaps/entity_icons/prop/crate01\";\r\n"
    "\t\tcategoryNameTag = \"#str_%s_category\";\r\n"
    "\t\tdisplayNameTag = \"#str_%s_" MS_EXAMPLE_DECL "_name\";\r\n"
    "\t\tdisplayDescriptionTag = \"#str_%s_" MS_EXAMPLE_DECL "_desc\";\r\n"
    "\t\tentityDef = \"mods/%s/" MS_EXAMPLE_DECL "\";\r\n"
    "\t\trenderInfo = {\r\n"
    "\t\t\tsize = {\r\n"
    "\t\t\t\tx = 54.3767967224;\r\n"
    "\t\t\t\ty = 54.3779754639;\r\n"
    "\t\t\t\tz = 53.595500946;\r\n"
    "\t\t\t}\r\n"
    "\t\t\tarrowSize = {\r\n"
    "\t\t\t\tx = 2;\r\n"
    "\t\t\t\ty = 2;\r\n"
    "\t\t\t\tz = 1;\r\n"
    "\t\t\t}\r\n"
    "\t\t}\r\n"
    "\t\teventInfo = {\r\n"
    "\t\t\tselectedEvent = {\r\n"
    "\t\t\t\tsound = \"play_pickup_heavy_crates\";\r\n"
    "\t\t\t}\r\n"
    "\t\t\tplacedEvent = {\r\n"
    "\t\t\t\tsound = \"play_putdown_heavy_crates\";\r\n"
    "\t\t\t}\r\n"
    "\t\t}\r\n"
    "\t}\r\n"
    "}\r\n";

static int ms_write_example(const wchar_t *dir, const char *salt, char *rel_out, size_t rel_cap)
{
    char rel[MAX_PATH], text[sizeof MS_EXAMPLE_ROW + 4 * SH_MB_ID_MAX];
    wchar_t *path;
    int ok;
    _snprintf_s(rel, sizeof rel, _TRUNCATE, MS_EXAMPLE_DEF_DIR "%s\\" MS_EXAMPLE_DECL ".decl", salt);
    path = ms_wjoin_utf8(dir, rel);
    ok = path && ms_mkdirs_parent(path) && ms_write_text(path, MS_EXAMPLE_DEF, sizeof MS_EXAMPLE_DEF - 1);
    free(path);
    if (!ok) return 0;
    _snprintf_s(text, sizeof text, _TRUNCATE, MS_EXAMPLE_ROW, salt, salt, salt, salt);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, MS_EXAMPLE_ROW_DIR "%s\\" MS_EXAMPLE_DECL ".decl", salt);
    path = ms_wjoin_utf8(dir, rel);
    ok = path && ms_mkdirs_parent(path) && ms_write_text(path, text, strlen(text));
    free(path);
    if (ok && rel_out && rel_cap) {
        char *c;
        strncpy_s(rel_out, rel_cap, rel, _TRUNCATE);
        for (c = rel_out; *c; c++) if (*c == '\\') *c = '/';
    }
    return ok;
}

static int ms_new_mod(const char *name, int example, char salt_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                      char *example_out, size_t example_cap, char *cause, size_t cap)
{
    char salt[SH_SALT_LEN + 1], esc[SH_MB_NAME_MAX * 6 + 1], seg[72], leaf[96], *u;
    char text[SH_MB_NAME_MAX * 12 + 4 * SH_MB_ID_MAX + 1024], strings[SH_MB_NAME_MAX * 6 + 4 * SH_MB_ID_MAX + 512];
    wchar_t *root = NULL, *dir = NULL, *assets = NULL, *desc = NULL;
    size_t i, n = 0, points = 0;
    int round, ok = 0;
    if (salt_out) salt_out[0] = 0;
    if (folder_out && folder_cap) folder_out[0] = 0;
    if (example_out && example_cap) example_out[0] = 0;
    while (name && (*name == ' ' || *name == '\t')) name++;
    if (!name || !name[0]) return ms_cause(cause, cap, "a new mod needs a name");
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c & 0xC0) != 0x80) points++;
        if (n + 7 >= sizeof esc) break;
        if (c == '"' || c == '\\') { esc[n++] = '\\'; esc[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)_snprintf_s(esc + n, sizeof esc - n, _TRUNCATE, "\\u%04x", c);
        else esc[n++] = (char)c;
    }
    esc[n] = 0;
    while (n && esc[n - 1] == ' ') esc[--n] = 0;
    if (points > SH_MB_NAME_MAX) return ms_cause(cause, cap, "a mod name is at most %u characters", SH_MB_NAME_MAX);
    root = ms_wroot();
    if (!root) return ms_cause(cause, cap, "the store folder cannot be resolved");
    if (!sh_ms_title_segment(name, seg, sizeof seg)) strcpy_s(seg, sizeof seg, "mod");
    /* "maps" at the top of mods\ is the places' folder, never a mod's. */
    if (!_stricmp(seg, SH_MS_PLACES)) strcat_s(seg, sizeof seg, " mod");
    modstore_lock();
    for (round = 1; round < 64 && !dir; round++) {
        if (round == 1) strcpy_s(leaf, sizeof leaf, seg);
        else _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s (%d)", seg, round);
        dir = ms_wjoin_utf8(root, leaf);
        if (dir && ms_exists(dir)) { free(dir); dir = NULL; }
    }
    if (!dir) { ms_cause(cause, cap, "no free folder name under mods\\ for \"%s\"", name); goto done; }
    if (!ms_new_salt(salt, NULL)) { ms_cause(cause, cap, "no new salt could be drawn (the system's random generator failed)"); goto done; }
    assets = ms_wjoin(dir, L"assets");
    desc = ms_wjoin(dir, L"mod.json");
    if (!assets || !desc || !ms_mkdirs(assets)) {
        ms_cause(cause, cap, "cannot create the folder mods\\%s: %s", leaf, ms_why(GetLastError(), leaf, 1, 0));
        goto done;
    }
    /* The author's part holds what the files cannot say; the block the salt,
     * which never changes afterwards. The example's Toybox text is author
     * text: its strings, keyed <salt>_<name> (no '/' in a string key). */
    strings[0] = 0;
    if (example)
        _snprintf_s(strings, sizeof strings, _TRUNCATE,
                    "  \"strings\": {\r\n    \"en\": {\r\n"
                    "      \"%s_category\": \"%s\",\r\n"
                    "      \"%s_" MS_EXAMPLE_DECL "_name\": \"Example crate\",\r\n"
                    "      \"%s_" MS_EXAMPLE_DECL "_desc\": \"A crate your new mod adds. Change it in the mod's files.\"\r\n"
                    "    }\r\n  },\r\n", salt, esc, salt, salt);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\r\n  \"name\": \"%s\",\r\n  \"description\": \"\",\r\n  \"author\": \"\",\r\n%s"
                "\r\n  \"" SH_MB_BLOCK_KEY "\": {\r\n    \"format\": %d,\r\n    \"salt\": \"%s\"\r\n  }\r\n}\r\n",
                esc, strings, SH_MB_FORMAT, salt);
    if (!ms_write_text(desc, text, strlen(text))) { ms_cause(cause, cap, "cannot write mods\\%s\\mod.json", leaf); goto done; }
    if (example && !ms_write_example(dir, salt, example_out, example_cap)) {
        ms_cause(cause, cap, "cannot write the example files of mods\\%s: %s", leaf, ms_why(GetLastError(), leaf, 1, 0));
        goto done;
    }
    u = sh_mb_utf8(dir);
    if (folder_out && folder_cap && u) strncpy_s(folder_out, folder_cap, ms_plain(u), _TRUNCATE);   /* without the long-path prefix */
    free(u);
    if (salt_out) strcpy_s(salt_out, SH_MB_ID_MAX + 1, salt);
    ms_log("MODS: store: new mod \"%s\" created as mods\\%s, salt %s", name, leaf, salt);
    ok = 1;
done:
    modstore_unlock();
    free(root);
    free(dir);
    free(assets);
    free(desc);
    return ok;
}

int sh_ms_new_mod(const char *name, char salt_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                  char *cause, size_t cap)
{
    return ms_new_mod(name, 0, salt_out, folder_out, folder_cap, NULL, 0, cause, cap);
}

int sh_ms_new_mod_example(const char *name, char salt_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                          char *example_out, size_t example_cap, char *cause, size_t cap)
{
    return ms_new_mod(name, 1, salt_out, folder_out, folder_cap, example_out, example_cap, cause, cap);
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
    if (!spec || !spec[0]) return ms_cause(cause, cap, "no mod named (use its folder or its salt)");
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

/* Build a root folder of the store in place and write its block. A new mod
 * gets its salt first (`from`: the name its own references use; NULL: its
 * folder name). A failed build writes the block of a failed build (its
 * cause): the folder serves nothing until fixed (rule 2). A place folder is a
 * received build and is never built here (R3). */
static int ms_build_in_place(const char *folder, const char *from, const sh_mb_resolver *resolver, sh_mb_bundle *b,
                             char *cause, size_t cap)
{
    sh_ms_list list;
    sh_mb_resolver chain_resolver;
    ms_chain chain;
    char one[SH_MB_CAUSE_MAX], why[SH_MB_CAUSE_MAX], salt[SH_SALT_LEN + 1];
    int state;
    memset(b, 0, sizeof *b);
    if (ms_in_place(folder)) {
        ms_cause(cause, cap, "mods\\%s is a build a map brought; it is kept as the map pins it and never built again",
                 ms_rel(folder));
        strncpy_s(b->first_cause, sizeof b->first_cause, cause ? cause : "", _TRUNCATE);
        return 0;
    }
    state = sh_mb_descriptor_state_why(folder, why, sizeof why);
    if (state == SH_MB_DESC_FRESH) {
        if (ms_assign(folder, from && from[0] ? from : ms_leaf(folder), salt, one, sizeof one) < 0) {
            ms_note_cause(folder, one);
            strncpy_s(b->first_cause, sizeof b->first_cause, one, _TRUNCATE);
            return ms_cause(cause, cap, "%s", one);
        }
    } else if (state != SH_MB_DESC_BUILT) {
        ms_note_cause(folder, why);
        strncpy_s(b->first_cause, sizeof b->first_cause, why, _TRUNCATE);
        return ms_cause(cause, cap, "%s", why);
    }
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    ms_chain_init(&chain_resolver, &chain, resolver, &list);
    if (!sh_mb_build(folder, NULL, &chain_resolver, b)) {
        sh_ms_list_free(&list);
        /* The cause goes into the block: the Mods tab shows it, and the folder
         * serves nothing until it builds again. */
        if (sh_salt_valid(b->id, strlen(b->id)) && !sh_mb_write_failed(b, folder, one, sizeof one))
            ms_log("MODS: store: %s: the failed build could not be recorded in mod.json: %s", ms_rel(folder), one);
        ms_note_cause(folder, b->first_cause);
        return ms_cause(cause, cap, "%s", b->first_cause);
    }
    sh_ms_list_free(&list);
    if (!sh_mb_write(b, folder, one, sizeof one)) {
        ms_note_cause(folder, one);
        return ms_cause(cause, cap, "%s", one);
    }
    sh_ms_note_build(folder, b);
    return 1;
}

/* Copy every file and folder below `src` into `dst` (created), real copies,
 * no link followed; everything below assets\ is named in lower case, the game
 * names the builder records (DECISIONS 13). 1 when all of it was copied. */
static int ms_copy_tree(const wchar_t *src, const wchar_t *dst, int depth, int lower, char *cause, size_t cap)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    int ok = 1;
    if (depth > 48) return ms_cause(cause, cap, "folder nesting deeper than 48");
    if (!ms_mkdirs(dst)) return ms_cause(cause, cap, "cannot create a folder in the store: %s", ms_why(GetLastError(), NULL, 0, 0));
    pattern = ms_wjoin(src, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 1 : ms_cause(cause, cap, "cannot list the folder");
    do {
        wchar_t *from, *to;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = ms_cause(cause, cap, "a symbolic link or junction is not allowed in a mod folder"); break; }
        {
            int sub = lower || (depth == 0 && !_wcsicmp(fd.cFileName, L"assets"));
            wchar_t name[MAX_PATH];
            size_t k;
            wcsncpy_s(name, MAX_PATH, fd.cFileName, _TRUNCATE);
            for (k = 0; sub && name[k]; k++) if (name[k] >= L'A' && name[k] <= L'Z') name[k] = (wchar_t)(name[k] - L'A' + L'a');
            from = ms_wjoin(src, fd.cFileName);
            to = ms_wjoin(dst, name);
            if (!from || !to) ok = ms_cause(cause, cap, "out of memory");
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = ms_copy_tree(from, to, depth + 1, sub, cause, cap);
            else if (!sh_ms_copy_file(from, to)) {
                char *u = sh_mb_utf8(fd.cFileName);
                ok = ms_cause(cause, cap, "cannot copy %s into the store: %s", u ? u : "a file", ms_why(GetLastError(), u ? u : "", 0, 0));
                free(u);
            }
        }
        free(from);
        free(to);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* A free folder name at the root for `leaf`: the leaf, else "<leaf> (2)" ... */
static int ms_free_root_name(const wchar_t *root, const char *leaf, char *out, size_t cap)
{
    int round;
    for (round = 1; round < 64; round++) {
        wchar_t *p;
        if (round == 1) strncpy_s(out, cap, leaf, _TRUNCATE);
        else _snprintf_s(out, cap, _TRUNCATE, "%s (%d)", leaf, round);
        if (!_stricmp(out, SH_MS_PLACES)) continue;
        p = ms_wjoin_utf8(root, out);
        if (p && !ms_exists(p)) { free(p); return 1; }
        free(p);
    }
    return 0;
}

/* The work folders of an install into `final`: staging and the replaced copy. */
static int ms_work_names(const wchar_t *final, wchar_t **staging, wchar_t **old)
{
    size_t n = wcslen(final) + 48;
    *staging = (wchar_t *)malloc(sizeof(wchar_t) * n);
    *old = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!*staging || !*old) { free(*staging); free(*old); *staging = *old = NULL; return 0; }
    _snwprintf_s(*staging, n, _TRUNCATE, L"%s.staging-%lu", final, GetCurrentProcessId());
    _snwprintf_s(*old, n, _TRUNCATE, L"%s.old-%lu", final, GetCurrentProcessId());
    return 1;
}

/* Swap a staged folder in as `final`, the folder there (if any) moved aside
 * and retired after. 1 on success; on failure the staging is removed and the
 * folder there put back. */
static int ms_swap_in(const wchar_t *staging, const wchar_t *final, const wchar_t *old, const wchar_t *root, const char *name,
                      int *replaced, char *cause, size_t cap)
{
    *replaced = 0;
    if (ms_exists(final)) {
        if (ms_exists(old)) ms_rmtree(old, root);
        if (!ms_move_folder(final, old)) {
            DWORD err = GetLastError();
            ms_rmtree(staging, root);
            return ms_cause(cause, cap, "cannot replace mods\\%s: %s", name, ms_why(err, name, 1, 0));
        }
        *replaced = 1;
    }
    if (!ms_move_folder(staging, final)) {
        DWORD err = GetLastError();
        if (*replaced) ms_move_folder(old, final);
        ms_rmtree(staging, root);
        return ms_cause(cause, cap, "cannot place mods\\%s: %s", name, ms_why(err, name, 1, 0));
    }
    if (*replaced) {
        char why[200];
        if (ms_retire(old, root, why, sizeof why) == 0)
            ms_log("MODS: store: the replaced copy of mods\\%s could not be removed completely; it is not a mod (its name "
                   "ends in .old-)", name);
    }
    return 1;
}

static int ms_install_locked(const char *folder, const char *as_name, const sh_ms_place *place,
                             const sh_mb_resolver *resolver, char *name_out, size_t name_cap,
                             sh_ms_install_stats *st, sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_mb_bundle b;
    sh_mb_resolver chain_resolver;
    ms_chain chain;
    sh_ms_list list;
    wchar_t *root = NULL, *src = NULL, *final = NULL, *staging = NULL, *old = NULL;
    char rootu[MAX_PATH * 4], name[SH_MS_NAME_MAX + 1], one[SH_MB_CAUSE_MAX], why[SH_MB_CAUSE_MAX], leaf[MAX_PATH * 4];
    char *author = NULL, *text = NULL, dir[SH_MS_NAME_MAX + 1] = "";
    size_t i, author_len = 0, text_len = 0;
    int ok = 0, state, copy_of_root = 0, replaced = 0;

    memset(st, 0, sizeof *st);
    memset(&b, 0, sizeof b);
    memset(&list, 0, sizeof list);
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
    if (!ms_mkdirs(root)) { ms_cause(cause, cap, "cannot create the store folder: %s", ms_why(GetLastError(), NULL, 0, 0)); goto done; }

    if (ms_store_mod_folder(src, root)) {
        /* A folder of the store, at any depth: built in place. */
        if (place) { ms_cause(cause, cap, "mods\\%s is already in the store", ms_rel(folder)); goto done; }
        st->in_place = 1;
        if (!ms_build_in_place(folder, NULL, resolver, &b, cause, cap)) goto done;
        for (i = 0; i < b.file_count; i++)
            if (b.files[i].stock) { st->stock_files++; st->stock_bytes += b.files[i].size; }
        strncpy_s(name, sizeof name, ms_rel(folder), _TRUNCATE);
        ok = 1;
        goto done;
    }
    if (ms_inside(src, root)) {
        ms_cause(cause, cap, ms_has_descriptor(src) ? "that folder is inside a mod of the store, not a mod itself"
                                                    : "that folder of the store has no mod.json");
        goto done;
    }
    {
        const wchar_t *s = wcsrchr(src, L'\\');
        char *u = sh_mb_utf8(s ? s + 1 : src);
        strncpy_s(leaf, sizeof leaf, u ? u : "mod", _TRUNCATE);
        free(u);
    }
    state = sh_mb_descriptor_state_why(folder, why, sizeof why);
    if (state == SH_MB_DESC_FRESH) {
        /* A new mod from elsewhere: copied in whole under its own name, salted
         * with its references rewritten (the source is only read), built; a
         * refused build leaves nothing installed. */
        char *fu;
        if (place) { ms_cause(cause, cap, SH_MB_SHAPE "it was not built by the mod store"); goto done; }
        if (as_name && as_name[0]) strncpy_s(name, sizeof name, as_name, _TRUNCATE);
        else if (!ms_free_root_name(root, leaf, name, sizeof name)) { ms_cause(cause, cap, "no free folder name under mods\\ for %s", leaf); goto done; }
        final = ms_wjoin_utf8(root, name);
        if (!final || !ms_work_names(final, &staging, &old)) { ms_cause(cause, cap, "out of memory"); goto done; }
        if (ms_exists(staging)) ms_rmtree(staging, root);
        if (!ms_copy_tree(src, staging, 0, 0, cause, cap)) { ms_rmtree(staging, root); goto done; }
        if (!ms_swap_in(staging, final, old, root, name, &replaced, cause, cap)) goto done;
        st->replaced = replaced;
        fu = sh_mb_utf8(final);
        if (!fu || !ms_build_in_place(ms_plain(fu), leaf, resolver, &b, cause, cap)) {
            ms_rmtree(final, root);
            if (fu) ms_note_cause(ms_plain(fu), NULL);
            free(fu);
            goto done;
        }
        free(fu);
        for (i = 0; i < b.file_count; i++) {
            if (b.files[i].stock) { st->stock_files++; st->stock_bytes += b.files[i].size; }
            else st->copied++;
        }
        ok = 1;
        goto done;
    }
    if (state != SH_MB_DESC_BUILT) { ms_cause(cause, cap, "%s", why); goto done; }

    /* A built mod from elsewhere: built in memory, the source is not written. */
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    ms_chain_init(&chain_resolver, &chain, resolver, &list);
    if (!sh_mb_build(folder, NULL, &chain_resolver, &b)) { ms_cause(cause, cap, "%s", b.first_cause); goto done; }
    if (place) {
        const sh_ms_entry *have;
        int r = sh_ms_place_folder(place, b.name, b.id, name, sizeof name);
        if (!r) { ms_cause(cause, cap, "no free folder for %s in its map's place under mods\\" SH_MS_PLACES, b.name ? b.name : b.id); goto done; }
        if (!ms_place_of_name(name, dir, sizeof dir)) dir[0] = 0;
        have = r == 2 ? sh_ms_find_salt(&list, b.id, dir, b.hash) : NULL;
        if (have && sh_ms_entry_usable(have) && !_stricmp(have->name, name)) {
            st->present = 1;
            ms_log("MODS: store: %s (salt %s, build %.16s) is already in mods\\%s; nothing was copied", b.name ? b.name : b.id,
                   b.id, b.hash, name);
        }
    } else {
        const sh_ms_entry *here = sh_ms_find_salt(&list, b.id, NULL, NULL);
        if (as_name && as_name[0]) strncpy_s(name, sizeof name, as_name, _TRUNCATE);
        else if (here && here->loaded && !_stricmp(here->bundle.hash, b.hash) && sh_ms_entry_usable(here)) {
            strncpy_s(name, sizeof name, here->name, _TRUNCATE);
            st->present = 1;
            ms_log("MODS: store: %s (salt %s, build %.16s) is already installed as mods\\%s; nothing was copied",
                   b.name ? b.name : b.id, b.id, b.hash, name);
        } else if (!ms_free_root_name(root, leaf, name, sizeof name)) {
            ms_cause(cause, cap, "no free folder name under mods\\ for %s", leaf);
            goto done;
        }
        /* The arriving folder of a salt the root holds is the newer copy (R9). */
        if (!st->present && here && _stricmp(here->name, name)) copy_of_root = 1;
    }
    if (st->present) {
        for (i = 0; i < b.file_count; i++)
            if (b.files[i].stock) { st->stock_files++; st->stock_bytes += b.files[i].size; }
        ok = 1;
        goto done;
    }
    /* DECISIONS 10: every file the folder holds is installed, the game's own
     * files included (the builder decides what is a borrow); only a pointer
     * the folder does not carry (the closure's) has nothing to copy. */
    for (i = 0; i < b.file_count; i++) {
        if (!b.files[i].stock) continue;
        st->stock_files++;
        st->stock_bytes += b.files[i].size;
    }
    final = ms_wjoin_utf8(root, name);
    if (!final || !ms_work_names(final, &staging, &old)) { ms_cause(cause, cap, "out of memory"); goto done; }
    if (ms_exists(staging)) ms_rmtree(staging, root);
    if (!ms_mkdirs(staging)) { ms_cause(cause, cap, "cannot create mods\\%s: %s", name, ms_why(GetLastError(), name, 1, 0)); goto done; }
    for (i = 0; i < b.file_count; i++) {
        wchar_t *from, *to;
        int copied;
        if (b.files[i].stock && b.files[i].absent) continue;
        from = ms_wjoin_utf8(src, b.files[i].path);
        to = ms_wjoin_utf8(staging, b.files[i].path);
        copied = from && to && sh_ms_copy_file(from, to);
        free(from);
        free(to);
        if (!copied) {
            DWORD err = GetLastError();
            ms_rmtree(staging, root);
            ms_cause(cause, cap, "cannot copy %s into the store: %s", b.files[i].path, ms_why(err, name, 1, b.files[i].size));
            goto done;
        }
        st->copied++;
    }
    {
        wchar_t *mj = ms_wjoin(src, L"mod.json");
        author = ms_read_text(mj, SH_MB_DESCRIPTOR_MAX * 16, &author_len);
        free(mj);
    }
    text = sh_mb_modjson_text(&b, author, author_len, &text_len);
    {
        wchar_t *mj = ms_wjoin(staging, L"mod.json");
        int wrote = mj && text && ms_write_text(mj, text, text_len);
        free(mj);
        if (!wrote) { ms_rmtree(staging, root); ms_cause(cause, cap, "cannot write mod.json for mods\\%s", name); goto done; }
    }
    /* The copy is what serves: hash it (the source may have changed since the build). */
    if (!ms_verify_folder(staging, &b, NULL, one, sizeof one)) {
        ms_rmtree(staging, root);
        ms_cause(cause, cap, "the copy did not verify: %s; install it again", one);
        goto done;
    }
    if (!ms_swap_in(staging, final, old, root, name, &replaced, cause, cap)) goto done;
    st->replaced = replaced;
    if (replaced) ms_log("MODS: store: replaced mods\\%s with %s (salt %s, build %.16s)", name, b.name ? b.name : b.id, b.id, b.hash);
    {
        char *fu = sh_mb_utf8(final);
        if (fu) ms_note_cause(ms_plain(fu), NULL);
        if (fu && copy_of_root) {
            /* A second folder of a root salt: it becomes a new mod now. */
            char salt[SH_SALT_LEN + 1], was[SH_SALT_LEN + 1];
            const sh_ms_entry *here = sh_ms_find_salt(&list, b.id, NULL, NULL);
            strcpy_s(was, sizeof was, b.id);
            if (ms_assign(ms_plain(fu), was, salt, cause, cap) < 0) {
                ms_rmtree(final, root);
                free(fu);
                goto done;
            }
            ms_log("MODS: store: mods\\%s is a copy of mods\\%s; it is a new mod now", name, here ? here->name : "?");
            st->resalted = 1;
            sh_mb_free(&b);
            if (!ms_build_in_place(ms_plain(fu), NULL, resolver, &b, cause, cap)) {
                ms_rmtree(final, root);
                free(fu);
                goto done;
            }
        }
        free(fu);
    }
    if (place) sh_ms_place_note(place, dir[0] ? dir : NULL);
    ok = 1;
done:
    if (ok && name_out) strncpy_s(name_out, name_cap, name, _TRUNCATE);
    if (ok && place && st->present) sh_ms_place_note(place, dir[0] ? dir : NULL);
    if (bundle_out) { *bundle_out = b; memset(&b, 0, sizeof b); }
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    free(author);
    free(text);
    free(root); free(src); free(final); free(staging); free(old);
    return ok;
}

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
    ok = ms_install_locked(folder, as_name, NULL, resolver, name_out, name_cap, stats ? stats : &local, bundle_out, cause, cap);
    modstore_unlock();
    return ok;
}

int sh_ms_install_placed(const char *folder, const sh_ms_place *place,
                         const sh_mb_resolver *resolver, char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                         sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_ms_install_stats local;
    int ok;
    if (!place) return ms_cause(cause, cap, "no map named for the place");
    modstore_lock();
    ok = ms_install_locked(folder, NULL, place, resolver, name_out, name_cap, stats ? stats : &local, bundle_out, cause, cap);
    modstore_unlock();
    return ok;
}

/* ------------------------------------------------------------- sync ---- */

#ifdef SH_MODSTORE_TESTING
void (*sh_ms_test_between_folders)(unsigned built);
#endif

/* One root folder as the scan sees it. */
typedef struct ms_sf {
    char *folder;               /* absolute UTF-8, plain */
    char name[SH_MS_NAME_MAX + 1];
    char salt[SH_SALT_LEN + 1];
    int  state;
    long long built;            /* the block's "built", 0 when none */
    int  current;               /* built, under these rules, from this very tree */
} ms_sf;

/* The root folders, each with its block read (no file under assets\ is read). */
static size_t ms_sync_scan(const wchar_t *root, ms_sf **out)
{
    ms_found_list found;
    ms_sf *v;
    size_t i, n = 0;
    *out = NULL;
    ms_find_store(&found, root);
    v = found.count ? (ms_sf *)calloc(found.count, sizeof *v) : NULL;
    for (i = 0; v && i < found.count; i++) {
        ms_sf *f = &v[n];
        char *rel = sh_mb_utf8(found.items[i].rel), *u = sh_mb_utf8(found.items[i].folder), place[8];
        sh_mb_bundle b;
        if (!rel || !u || ms_place_of_name(rel, place, sizeof place)) { free(rel); free(u); continue; }   /* places are never built */
        f->folder = _strdup(ms_plain(u));
        strncpy_s(f->name, sizeof f->name, rel, _TRUNCATE);
        free(rel);
        free(u);
        if (!f->folder) continue;
        f->state = sh_mb_descriptor_state(f->folder);
        if (f->state == SH_MB_DESC_BUILT) {
            char tree[SH_MB_HASH_HEX + 1];
            int loaded = sh_mb_load_folder(f->folder, &b);
            if (sh_salt_valid(b.id, strlen(b.id))) strcpy_s(f->salt, sizeof f->salt, b.id);
            f->built = b.built;
            f->current = loaded && b.rules == SH_MB_RULES && b.tree[0] && sh_mb_tree_hash(f->folder, tree) && !strcmp(tree, b.tree);
            sh_mb_free(&b);
        }
        n++;
    }
    ms_found_free(&found);
    *out = v;
    return n;
}

static void ms_sf_free(ms_sf *v, size_t n)
{
    size_t i;
    for (i = 0; v && i < n; i++) free(v[i].folder);
    free(v);
}

static int ms_sf_older(const ms_sf *a, const ms_sf *b)
{
    long long x = a->built > 0 ? a->built : LLONG_MAX, y = b->built > 0 ? b->built : LLONG_MAX;
    if (x != y) return x < y;
    return _stricmp(a->name, b->name) < 0;
}

/* The folder of v[i]'s salt that keeps it (the oldest), or (size_t)-1 when v[i] is that one. */
static size_t ms_sf_keeper(const ms_sf *v, size_t n, size_t i)
{
    size_t k, keep = (size_t)-1;
    if (!v[i].salt[0]) return keep;
    for (k = 0; k < n; k++) {
        if (k == i || !v[k].salt[0] || _stricmp(v[k].salt, v[i].salt) || !ms_sf_older(&v[k], &v[i])) continue;
        if (keep == (size_t)-1 || ms_sf_older(&v[k], &v[keep])) keep = k;
    }
    return keep;
}

/* The scan. With `yield` (the boot build) the store is handed to any thread
 * waiting for it between two folders, so the build never holds up a store
 * operation for longer than one folder's build. */
static int ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *st, int yield)
{
    sh_ms_sync_stats local;
    wchar_t *root;
    ms_sf *v = NULL;
    size_t n, i;
    ULONGLONG t0 = GetTickCount64();
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    root = ms_wroot();
    if (!root) return 0;
    if (!ms_exists(root)) { free(root); return 1; }
    modstore_lock();
    n = ms_sync_scan(root, &v);
    st->folders = (unsigned)n;
    /* 1. A new mod gets its salt; its folder keeps its name. */
    for (i = 0; i < n; i++) {
        char salt[SH_SALT_LEN + 1], cause[SH_MB_CAUSE_MAX];
        if (v[i].state != SH_MB_DESC_FRESH) continue;
        if (ms_assign(v[i].folder, ms_leaf(v[i].folder), salt, cause, sizeof cause) < 0) {
            st->failed++;
            ms_note_cause(v[i].folder, cause);
            ms_log("MODS: store: %s refused: %s", v[i].name, cause);
            continue;
        }
        ms_note_cause(v[i].folder, NULL);
        st->salted++;
        v[i].state = SH_MB_DESC_BUILT;
        strcpy_s(v[i].salt, sizeof v[i].salt, salt);
        v[i].built = 0;
        v[i].current = 0;
    }
    /* 2. A salt in two root folders: the one built first keeps it. Keepers
     * are settled before anything changes, so three copies resolve at once. */
    {
        size_t *keep = n ? (size_t *)malloc(n * sizeof *keep) : NULL;
        for (i = 0; keep && i < n; i++) keep[i] = v[i].state == SH_MB_DESC_BUILT ? ms_sf_keeper(v, n, i) : (size_t)-1;
        for (i = 0; keep && i < n; i++) {
            char salt[SH_SALT_LEN + 1], was[SH_SALT_LEN + 1], cause[SH_MB_CAUSE_MAX];
            if (keep[i] == (size_t)-1) continue;
            strcpy_s(was, sizeof was, v[i].salt);
            if (ms_assign(v[i].folder, was, salt, cause, sizeof cause) < 0) {
                st->failed++;
                ms_note_cause(v[i].folder, cause);
                ms_log("MODS: store: %s is a copy of %s and could not become a new mod: %s", v[i].name, v[keep[i]].name, cause);
                v[i].state = 0;   /* not built: it serves nothing while it carries another folder's salt */
                continue;
            }
            ms_log("MODS: store: mods\\%s is a copy of mods\\%s; it is a new mod now", v[i].name, v[keep[i]].name);
            st->resalted++;
            strcpy_s(v[i].salt, sizeof v[i].salt, salt);
            v[i].built = 0;
            v[i].current = 0;
        }
        free(keep);
    }
    /* 3. The rebuild rule: a root folder not built from its current tree under
     * these rules is built again; one whose last build failed too. */
    for (i = 0; i < n; i++) {
        char cause[SH_MB_CAUSE_MAX], classes[160], why[SH_MB_CAUSE_MAX];
        sh_mb_bundle b;
        ULONGLONG b0;
        if (v[i].state != SH_MB_DESC_BUILT) {
            if (v[i].state && v[i].state != SH_MB_DESC_FRESH) {
                /* An earlier shape: listed with its reason, nothing written. */
                sh_mb_descriptor_state_why(v[i].folder, why, sizeof why);
                if (sh_ms_descriptor_unparsable(v[i].folder, cause, sizeof cause)) strcpy_s(why, sizeof why, cause);
                st->failed++;
                ms_log("MODS: store: %s refused: %s", v[i].name, why);
            }
            continue;
        }
        if (v[i].current) { ms_note_cause(v[i].folder, NULL); continue; }
#ifdef SH_MODSTORE_TESTING
        if (yield && st->rebuilt + st->failed && sh_ms_test_between_folders) sh_ms_test_between_folders(st->rebuilt);
#endif
        if (yield && st->rebuilt + st->failed) ms_lock_yield();
        b0 = GetTickCount64();
        if (ms_build_in_place(v[i].folder, NULL, resolver, &b, cause, sizeof cause)) {
            st->rebuilt++;
            sh_mb_classes_text(&b.classes, classes, sizeof classes);
            ms_log("MODS: store: %s built: %s (salt %s), build %.16s, %zu file(s) (%u stock pointer(s)), %llu ms%s; %s",
                   v[i].name, b.name ? b.name : "", b.id, b.hash, b.file_count, b.stock_count,
                   (unsigned long long)(GetTickCount64() - b0),
                   b.prev_hash[0] && strcmp(b.prev_hash, b.hash) ? ", its tree changed" : (b.prev_hash[0] ? ", unchanged content" : ""),
                   classes);
        } else {
            st->failed++;
            ms_log("MODS: store: %s refused: %s", v[i].name, cause);
        }
        sh_mb_free(&b);
    }
    ms_sf_free(v, n);
    modstore_unlock();
    free(root);
    st->ms = GetTickCount64() - t0;
    return 1;
}

int sh_ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *st) { return ms_sync(resolver, st, 0); }

int sh_ms_sync_pending(void)
{
    wchar_t *root;
    ms_sf *v = NULL;
    size_t n, i;
    int pending = 0;
    root = ms_wroot();
    if (!root) return -1;
    if (!ms_exists(root)) { free(root); return 0; }
    modstore_lock();
    n = ms_sync_scan(root, &v);
    for (i = 0; i < n; i++) {
        if (v[i].state == SH_MB_DESC_FRESH) pending++;
        else if (v[i].state == SH_MB_DESC_BUILT && (!v[i].current || ms_sf_keeper(v, n, i) != (size_t)-1)) pending++;
    }
    ms_sf_free(v, n);
    modstore_unlock();
    free(root);
    return pending;
}

static sh_mb_resolver g_boot_resolver;
static sh_ms_rescan_fn g_boot_rescan;

/* The startup install's release of the store (sh_ms_boot_release): a manual
 * reset event the boot build waits on before it touches the store. */
static INIT_ONCE g_boot_release_once = INIT_ONCE_STATIC_INIT;
static HANDLE    g_boot_release;
static volatile LONG g_boot_released;

static BOOL CALLBACK ms_boot_release_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    g_boot_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    return TRUE;
}

static HANDLE ms_boot_release_event(void)
{
    InitOnceExecuteOnce(&g_boot_release_once, ms_boot_release_init, NULL, NULL);
    return g_boot_release;
}

void sh_ms_boot_release(void)
{
    HANDLE h = ms_boot_release_event();
    InterlockedExchange(&g_boot_released, 1);
    if (h) SetEvent(h);
}

int sh_ms_boot_released(void) { return InterlockedCompareExchange(&g_boot_released, 0, 0) != 0; }

#ifdef SH_MODSTORE_TESTING
void sh_ms_test_boot_rearm(void)
{
    HANDLE h = ms_boot_release_event();
    InterlockedExchange(&g_boot_released, 0);
    if (h) ResetEvent(h);
}
#endif

static void ms_boot_run(int pending, int yield)
{
    sh_ms_sync_stats st;
    ULONGLONG t0 = GetTickCount64();
    int n = -1;
    if (pending < 0) pending = sh_ms_sync_pending();
    if (pending <= 0) return;
    ms_log("MODS: store: boot: %d folder(s) not built yet (or changed since their build); building them, as a rescan "
           "would", pending);
    if (!ms_sync(&g_boot_resolver, &st, yield)) { ms_log("MODS: store: boot: the store could not be read; nothing built"); return; }
    if (st.rebuilt && g_boot_rescan) n = g_boot_rescan();
    ms_log("MODS: store: boot: %u of %u folder(s) built (%u refused) in %llu ms%s", st.rebuilt, st.folders, st.failed,
           (unsigned long long)(GetTickCount64() - t0),
           !st.rebuilt ? "" : n >= 0 ? "; the serving index was rebuilt with them" : "; the serving index could not be rebuilt");
}

/* How long the boot build waits for the startup install to release the
 * store before it builds anyway (a startup that never reaches its end). */
#define MS_BOOT_RELEASE_WAIT_MS 120000

static DWORD WINAPI ms_boot_thread(LPVOID unused)
{
    HANDLE h = ms_boot_release_event();
    ULONGLONG t0 = GetTickCount64();
    (void)unused;
    /* The startup install takes the store (the publish launch, the gates);
     * the build waits until it is done, so the startup never waits on it. */
    if (h && WaitForSingleObject(h, MS_BOOT_RELEASE_WAIT_MS) != WAIT_OBJECT_0)
        ms_log("MODS: store: boot: the startup install did not release the store within %u s; checking the folders now",
               MS_BOOT_RELEASE_WAIT_MS / 1000);
    else
        ms_log("MODS: store: boot: the startup install released the store after %llu ms; checking the folders",
               (unsigned long long)(GetTickCount64() - t0));
    ms_boot_run(-1, 1);
    return 0;
}

int sh_ms_boot_sync(const sh_mb_resolver *resolver, sh_ms_rescan_fn rescan, int background)
{
    int pending;
    HANDLE h;
    memset(&g_boot_resolver, 0, sizeof g_boot_resolver);
    if (resolver) g_boot_resolver = *resolver;
    g_boot_rescan = rescan;
    if (background) {
        /* The check reads every folder's tree (about 170 ms for 15 converted
         * folders), so it runs on the thread too, once the startup install
         * has released the store (sh_ms_boot_release). */
        (void)ms_boot_release_event();
        h = CreateThread(NULL, 0, ms_boot_thread, NULL, 0, NULL);
        if (!h) { ms_log("MODS: store: boot: the build thread could not start; the Mods tab's rescan builds them"); return -1; }
        CloseHandle(h);
        return 1;
    }
    pending = sh_ms_sync_pending();
    if (pending > 0) ms_boot_run(pending, 0);
    return pending;
}

/* ------------------------------------------- crash leftovers ---- */
/* Installs, downloads and salting assemble a folder as
 * "<name>.staging-<pid>" and move a replaced one aside as "<name>.old-<pid>"
 * before the swap; a session that ends between the steps leaves them. */

/* The process that named a work folder is still running (this one included;
 * a process this one may not open is taken as running). */
static int ms_pid_alive(DWORD pid)
{
    HANDLE h;
    DWORD code = 0;
    int alive;
    if (pid == GetCurrentProcessId()) return 1;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

static void ms_recover_dir(const wchar_t *dir, const wchar_t *rel, const wchar_t *root, int depth, int enter, int *put_back)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *old_at, *at;
        wchar_t *child, *crel;
        char *what;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        old_at = ms_work_at(fd.cFileName, L".old-");
        at = old_at ? old_at : ms_work_at(fd.cFileName, L".staging-");
        if (!at) {
            /* An organizing folder is looked into, as the listing does; a mod is not. */
            if (!enter || !ms_candidate(&fd) || depth + 1 >= SH_MS_NEST_MAX) continue;
            child = ms_wjoin(dir, fd.cFileName);
            crel = rel && rel[0] ? ms_wjoin(rel, fd.cFileName) : _wcsdup(fd.cFileName);
            if (child && crel && !ms_has_descriptor(child)) ms_recover_dir(child, crel, root, depth + 1, 1, put_back);
            free(child);
            free(crel);
            continue;
        }
        if (ms_pid_alive((DWORD)wcstoul(wcschr(at + 1, L'-') + 1, NULL, 10))) continue;
        child = ms_wjoin(dir, fd.cFileName);
        crel = rel && rel[0] ? ms_wjoin(rel, fd.cFileName) : _wcsdup(fd.cFileName);
        what = crel ? sh_mb_utf8(crel) : NULL;
        if (child && what && old_at) {
            wchar_t *target = _wcsdup(child);
            if (target) target[wcslen(dir) + 1 + (size_t)(old_at - fd.cFileName)] = 0;
            if (target && !ms_exists(target)) {
                char *tu = sh_mb_utf8(target);
                if (ms_move_folder(child, target)) {
                    (*put_back)++;
                    ms_log("MODS: store: put %s back from %s: an earlier session ended while it was being replaced",
                           tu ? ms_rel(ms_plain(tu)) : "?", what);
                } else {
                    ms_log("MODS: store: %s holds the folder an earlier session was replacing, and it could not be put "
                           "back (error %lu); rename it without its .old- suffix to use it again", what, GetLastError());
                }
                free(tu);
                free(target);
                free(child);
                free(crel);
                free(what);
                continue;
            }
            free(target);
        }
        if (child && what) {
            if (ms_rmtree(child, root))
                ms_log("MODS: store: removed %s, a work folder an earlier session left when it ended mid-install", what);
            else
                ms_log("MODS: store: %s, a work folder an earlier session left, could not be removed completely; it is "
                       "not a mod and is tried again at the next boot", what);
        }
        free(child);
        free(crel);
        free(what);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}


int sh_ms_recover_work(void)
{
    wchar_t *root = ms_wroot();
    int put_back = 0;
    if (!root) return -1;
    if (!ms_exists(root)) { free(root); return 0; }
    modstore_lock();
    ms_recover_dir(root, L"", root, 0, 1, &put_back);
    modstore_unlock();
    free(root);
    return put_back;
}
