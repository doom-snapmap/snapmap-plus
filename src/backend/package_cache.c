#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_cache.h"
#include "package_sources.h"

/* ---- Owner locks of live extraction directories ---- */
typedef struct pc_owner {
    wchar_t *directory;
    HANDLE handle;
    struct pc_owner *next;
} pc_owner;
static SRWLOCK g_owner_lock = SRWLOCK_INIT;
static pc_owner *g_owners;

static wchar_t *pc_join(const wchar_t *directory, const wchar_t *leaf)
{
    size_t a = wcslen(directory), b = wcslen(leaf);
    wchar_t *out = malloc((a + b + 2) * sizeof(*out));
    if (!out) return NULL;
    memcpy(out, directory, a * sizeof(*out));
    out[a] = L'\\';
    memcpy(out + a + 1, leaf, (b + 1) * sizeof(*out));
    return out;
}
/* The lock file, open for nobody else and gone with the last handle. An
 * existing file (a lock left by a power loss) is taken over the same way. */
static HANDLE pc_owner_open(const wchar_t *directory)
{
    wchar_t *path = pc_join(directory, L".owner");
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!path) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    { DWORD last = GetLastError(); free(path); SetLastError(last); }
    return handle;
}
int sh_package_cache_directory_hold(const char *directory)
{
    wchar_t *wide = sh_package_source_wide_path(directory);
    pc_owner *owner;
    int held = 0;
    if (!wide) return 0;
    AcquireSRWLockExclusive(&g_owner_lock);
    for (owner = g_owners; owner; owner = owner->next) if (!_wcsicmp(owner->directory, wide)) { held = 1; break; }
    if (!held) {
        HANDLE handle = pc_owner_open(wide);
        if (handle != INVALID_HANDLE_VALUE) {
            owner = malloc(sizeof(*owner));
            if (owner) { owner->directory = wide; owner->handle = handle; owner->next = g_owners; g_owners = owner; wide = NULL; held = 1; }
            else CloseHandle(handle);
        }
    }
    ReleaseSRWLockExclusive(&g_owner_lock);
    free(wide);
    return held;
}
void sh_package_cache_directory_drop(const char *directory)
{
    wchar_t *wide = sh_package_source_wide_path(directory);
    pc_owner *found = NULL, **link;
    if (!wide) return;
    AcquireSRWLockExclusive(&g_owner_lock);
    for (link = &g_owners; *link; link = &(*link)->next) if (!_wcsicmp((*link)->directory, wide)) {
        found = *link; *link = found->next; break;
    }
    ReleaseSRWLockExclusive(&g_owner_lock);
    if (found) { CloseHandle(found->handle); free(found->directory); free(found); }
    free(wide);
}

/* ---- Directory removal ---- */
static int pc_hex(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }
/* An extraction directory (map-<32 hex>) or a process's shared copy of a complete tree (tree-<64 hex>, the tree's
 * digest; map_package.c lays it out once and hard-links it into each context). Both are swept alike. */
static int pc_map_name(const wchar_t *name)
{
    size_t i, length = wcslen(name), prefix;
    if (length == 36 && !wcsncmp(name, L"map-", 4)) prefix = 4;
    else if (length == 69 && !wcsncmp(name, L"tree-", 5)) prefix = 5;
    else return 0;
    for (i = prefix; i < length; i++) if (!pc_hex(name[i])) return 0;
    return 1;
}
/* Remove everything below path, never following links; keep_owner leaves the
 * lock file for its holder to close. 1 when nothing is left but that file. */
static int pc_discard_tree(const wchar_t *path, int keep_owner)
{
    wchar_t *pattern = pc_join(path, L"*");
    WIN32_FIND_DATAW found;
    HANDLE search;
    int ok = 1;
    if (!pattern) return 0;
    search = FindFirstFileW(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        wchar_t *child;
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        if (keep_owner && !wcscmp(found.cFileName, L".owner")) continue;
        child = pc_join(path, found.cFileName);
        if (!child) { ok = 0; continue; }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(child, found.dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !pc_discard_tree(child, 0)) ok = 0;
            else if (!RemoveDirectoryW(child)) ok = 0;
        } else if (!DeleteFileW(child)) ok = 0;
        free(child);
    } while (FindNextFileW(search, &found));
    if (GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    FindClose(search);
    return ok;
}
static wchar_t *pc_cache_path(const char *data_root, const wchar_t *below)
{
    wchar_t *root = sh_package_source_wide_path(data_root), *cache, *out;
    if (!root) return NULL;
    cache = pc_join(root, L"package-cache"); free(root);
    if (!cache) return NULL;
    out = pc_join(cache, below); free(cache);
    return out;
}
size_t sh_package_cache_sweep_maps(const char *data_root, size_t *held, size_t *failed)
{
    wchar_t *maps = pc_cache_path(data_root, L"maps"), *pattern;
    WIN32_FIND_DATAW found;
    HANDLE search;
    size_t removed = 0;
    if (held) *held = 0;
    if (failed) *failed = 0;
    if (!maps) return 0;
    pattern = pc_join(maps, L"*-*");
    search = pattern ? FindFirstFileW(pattern, &found) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) { free(maps); return 0; }
    do {
        wchar_t *directory;
        HANDLE lock;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !pc_map_name(found.cFileName)) continue;
        directory = pc_join(maps, found.cFileName);
        if (!directory) { if (failed) (*failed)++; continue; }
        lock = pc_owner_open(directory);
        if (lock == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_SHARING_VIOLATION) { if (held) (*held)++; }
            else if (failed) (*failed)++;
            free(directory); continue;
        }
        /* Taken: no process owns it. The lock file goes with our handle, then
         * the emptied directory. A file held open inside leaves the directory
         * (unlocked again) for a later sweep. */
        {
            int emptied = pc_discard_tree(directory, 1);
            CloseHandle(lock);
            if (emptied && RemoveDirectoryW(directory)) removed++;
            else if (failed) (*failed)++;
        }
        free(directory);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    free(maps);
    return removed;
}

/* ---- Content-addressed resources ---- */
/* The names this product writes into resources: a content name (64 hex digits,
 * with a .N suffix for a damaged or colliding occupant) and its temporary
 * output names. Anything else was not written by it. */
static int pc_own_name(const wchar_t *name)
{
    size_t i, length = wcslen(name);
    if (!wcsncmp(name, L".building.", 10) || !wcsncmp(name, L".private.", 9)) return 1;
    if (length < 64) return 0;
    for (i = 0; i < 64; i++) if (!pc_hex(name[i])) return 0;
    if (length == 64) return 1;
    if (length < 66 || name[64] != L'.' || name[65] < L'1' || name[65] > L'9') return 0;
    for (i = 66; i < length; i++) if (name[i] < L'0' || name[i] > L'9') return 0;
    return 1;
}
size_t sh_package_cache_evict_resources(const char *data_root, sh_package_cache_keep keep, void *context,
    size_t *kept, size_t *held, size_t *foreign, unsigned long long *bytes_freed)
{
    wchar_t *resources = pc_cache_path(data_root, L"resources"), *pattern;
    WIN32_FIND_DATAW found;
    HANDLE search;
    size_t removed = 0;
    if (kept) *kept = 0;
    if (held) *held = 0;
    if (foreign) *foreign = 0;
    if (bytes_freed) *bytes_freed = 0;
    if (!resources || !keep) { free(resources); return 0; }
    pattern = pc_join(resources, L"*");
    search = pattern ? FindFirstFileW(pattern, &found) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) { free(resources); return 0; }
    do {
        char leaf[256];
        wchar_t *path;
        size_t i, length;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!pc_own_name(found.cFileName)) { if (foreign) (*foreign)++; continue; }
        length = wcslen(found.cFileName);
        if (length >= sizeof(leaf)) { if (foreign) (*foreign)++; continue; }
        for (i = 0; i <= length; i++) leaf[i] = (char)found.cFileName[i]; /* own names are ASCII */
        if (keep(context, leaf)) { if (kept) (*kept)++; continue; }
        path = pc_join(resources, found.cFileName);
        if (path && DeleteFileW(path)) {
            removed++;
            if (bytes_freed) *bytes_freed += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        } else if (held) (*held)++;
        free(path);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    free(resources);
    return removed;
}
