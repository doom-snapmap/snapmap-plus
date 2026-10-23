/* Package cache reclamation (package_cache.c): the owner lock of a live
 * extraction directory, the startup sweep of unowned map-* directories, and
 * the eviction of unreferenced content-addressed resources. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_cache.h"

static int failures;
static char root[MAX_PATH];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void path(char *out, size_t capacity, const char *relative)
{
    size_t i;
    snprintf(out, capacity, "%s\\%s", root, relative);
    for (i = strlen(root); out[i]; i++) if (out[i] == '/') out[i] = '\\';
}
static void make_dirs(const char *relative)
{
    char full[MAX_PATH * 2];
    size_t i;
    path(full, sizeof(full), relative);
    for (i = strlen(root) + 1; ; i++) if (full[i] == '\\' || !full[i]) {
        char saved = full[i];
        full[i] = 0;
        CHECK(CreateDirectoryA(full, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
        full[i] = saved;
        if (!saved) break;
    }
}
static void write_file(const char *relative, const char *text, int read_only)
{
    char full[MAX_PATH * 2], *slash;
    FILE *file = NULL;
    path(full, sizeof(full), relative);
    slash = strrchr(full, '\\'); *slash = 0; make_dirs(full + strlen(root) + 1); *slash = '\\';
    CHECK(!fopen_s(&file, full, "wb") && file);
    if (!file) return;
    fwrite(text, 1, strlen(text), file); fclose(file);
    if (read_only) CHECK(SetFileAttributesA(full, FILE_ATTRIBUTE_READONLY));
}
static int exists(const char *relative)
{
    char full[MAX_PATH * 2];
    path(full, sizeof(full), relative);
    return GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES;
}
static HANDLE hold_open(const char *relative)
{
    char full[MAX_PATH * 2];
    path(full, sizeof(full), relative);
    return CreateFileA(full, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
}
static size_t entries(const char *relative)
{
    char full[MAX_PATH * 2]; WIN32_FIND_DATAA data; size_t count = 0;
    path(full, sizeof(full), relative); strcat_s(full, sizeof(full), "\\*");
    HANDLE find = FindFirstFileA(full, &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do { if (strcmp(data.cFileName, ".") && strcmp(data.cFileName, "..")) count++; } while (FindNextFileA(find, &data));
    FindClose(find); return count;
}
static void remove_tree(const char *relative)
{
    char full[MAX_PATH * 2], pattern[MAX_PATH * 2], child[MAX_PATH * 2];
    WIN32_FIND_DATAA data;
    path(full, sizeof(full), relative);
    snprintf(pattern, sizeof(pattern), "%s\\*", full);
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s\\%s", relative, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else {
                char file[MAX_PATH * 2];
                path(file, sizeof(file), child);
                SetFileAttributesA(file, FILE_ATTRIBUTE_NORMAL); DeleteFileA(file);
            }
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    RemoveDirectoryA(full);
}

#define STALE "package-cache\\maps\\map-0123456789abcdef0123456789abcdef"
#define LIVE "package-cache\\maps\\map-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BUSY "package-cache\\maps\\map-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHORT_NAME "package-cache\\maps\\map-0123456789abcdef"
#define UPPER "package-cache\\maps\\map-ABCDEF0123456789ABCDEF0123456789"
#define STALE_TREE "package-cache\\maps\\tree-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define LIVE_TREE "package-cache\\maps\\tree-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHORT_TREE "package-cache\\maps\\tree-0123456789abcdef0123456789abcdef"

static void test_owner_lock(void)
{
    char live[MAX_PATH * 2];
    make_dirs(LIVE); path(live, sizeof(live), LIVE);
    CHECK(!exists(LIVE "\\.owner"));
    CHECK(sh_package_cache_directory_hold(live));
    CHECK(exists(LIVE "\\.owner"));
    CHECK(sh_package_cache_directory_hold(live)); /* a held directory stays held: one owner entry */
    {   /* The lock is exclusive to its holder. */
        HANDLE other = hold_open(LIVE "\\.owner");
        CHECK(other == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
        if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
    }
    sh_package_cache_directory_drop(live);
    CHECK(!exists(LIVE "\\.owner")); /* gone with the handle: hold and drop leave nothing */
    sh_package_cache_directory_drop(live); /* harmless */
    CHECK(entries(LIVE) == 0);
    CHECK(!sh_package_cache_directory_hold("relative\\path") && !sh_package_cache_directory_hold(NULL));
    remove_tree(LIVE);
}

static void test_sweep(void)
{
    char live[MAX_PATH * 2];
    size_t held = 0, failed = 0, removed;
    HANDLE busy;
    char live_tree[MAX_PATH];
    /* Nothing to sweep: no package cache, then an empty maps directory. */
    CHECK(sh_package_cache_sweep_maps(root, &held, &failed) == 0 && !held && !failed);
    make_dirs("package-cache\\maps");
    CHECK(sh_package_cache_sweep_maps(root, &held, &failed) == 0 && !held && !failed);
    /* A stale directory with a nested tree and a read-only file; a live one
     * held by this process (another process looks the same: a sharing
     * violation on the lock); a directory whose file is open elsewhere; names
     * that are not extraction directories. */
    write_file(STALE "\\overrides\\0\\package.json", "{}", 0);
    write_file(STALE "\\overrides\\0\\assets\\generated\\decls\\a\\b\\c.decl", "{ }", 1);
    write_file(STALE "\\stray.bin", "x", 0);
    write_file(LIVE "\\overrides\\0\\package.json", "{}", 0);
    path(live, sizeof(live), LIVE);
    CHECK(sh_package_cache_directory_hold(live));
    write_file(BUSY "\\overrides\\0\\package.json", "{}", 0);
    write_file(BUSY "\\overrides\\0\\held.bin", "held", 0);
    busy = hold_open(BUSY "\\overrides\\0\\held.bin"); CHECK(busy != INVALID_HANDLE_VALUE);
    write_file(SHORT_NAME "\\package.json", "{}", 0);
    write_file(UPPER "\\package.json", "{}", 0);
    write_file(STALE_TREE "\\tree\\package.json", "{}", 0);
    write_file(LIVE_TREE "\\tree\\package.json", "{}", 0);
    path(live_tree, sizeof(live_tree), LIVE_TREE);
    CHECK(sh_package_cache_directory_hold(live_tree));
    write_file(SHORT_TREE "\\tree\\package.json", "{}", 0);
    write_file("package-cache\\maps\\notes.txt", "a file, not a directory", 0);
    write_file("package-cache\\maps\\map-0123456789abcdef0123456789abcdee", "a file named like a directory", 0);
    removed = sh_package_cache_sweep_maps(root, &held, &failed);
    CHECK(removed == 2 && held == 2 && failed == 1);
    CHECK(!exists(STALE) && !exists(STALE_TREE));
    CHECK(exists(LIVE_TREE "\\tree\\package.json") && exists(LIVE_TREE "\\.owner") && exists(SHORT_TREE "\\tree\\package.json"));
    CHECK(exists(LIVE "\\overrides\\0\\package.json") && exists(LIVE "\\.owner"));
    CHECK(exists(BUSY "\\overrides\\0\\held.bin") && !exists(BUSY "\\overrides\\0\\package.json") && !exists(BUSY "\\.owner"));
    CHECK(exists(SHORT_NAME "\\package.json") && exists(UPPER "\\package.json"));
    CHECK(exists("package-cache\\maps\\notes.txt") && exists("package-cache\\maps\\map-0123456789abcdef0123456789abcdee"));
    /* The owner closes its context; the busy file is released: both go. */
    sh_package_cache_directory_drop(live); sh_package_cache_directory_drop(live_tree);
    CloseHandle(busy);
    removed = sh_package_cache_sweep_maps(root, &held, &failed);
    CHECK(removed == 3 && held == 0 && failed == 0);
    CHECK(!exists(LIVE) && !exists(BUSY) && !exists(LIVE_TREE));
    CHECK(sh_package_cache_sweep_maps(root, &held, &failed) == 0 && !held && !failed);
    CHECK(sh_package_cache_sweep_maps(root, NULL, NULL) == 0);
    remove_tree("package-cache\\maps");
}

static const char *const keep_leaves[] = {
    "1111111111111111111111111111111111111111111111111111111111111111",
    "2222222222222222222222222222222222222222222222222222222222222222.1",
};
static int keep(void *context, const char *leaf)
{
    size_t *asked = context;
    (*asked)++;
    for (size_t i = 0; i < sizeof(keep_leaves) / sizeof(*keep_leaves); i++) if (!strcmp(keep_leaves[i], leaf)) return 1;
    return 0;
}
static void test_evict(void)
{
    size_t kept = 0, held = 0, foreign = 0, asked = 0, removed;
    unsigned long long bytes = 0;
    HANDLE busy;
    CHECK(sh_package_cache_evict_resources(root, keep, &asked, &kept, &held, &foreign, &bytes) == 0 && !asked);
    make_dirs("package-cache\\resources");
    CHECK(sh_package_cache_evict_resources(root, keep, &asked, &kept, &held, &foreign, &bytes) == 0 && !asked);
    /* Referenced, stale (plain, collision-suffixed, temporary, legacy private),
     * held open, foreign, and a subdirectory. */
    write_file("package-cache\\resources\\1111111111111111111111111111111111111111111111111111111111111111", "referenced", 0);
    write_file("package-cache\\resources\\2222222222222222222222222222222222222222222222222222222222222222.1", "referenced too", 0);
    write_file("package-cache\\resources\\3333333333333333333333333333333333333333333333333333333333333333", "stale 1234567890", 0);
    write_file("package-cache\\resources\\4444444444444444444444444444444444444444444444444444444444444444.2", "stale", 0);
    write_file("package-cache\\resources\\.building.1.2.3", "interrupted output", 0);
    write_file("package-cache\\resources\\.private.4.5.6", "legacy proof", 0);
    write_file("package-cache\\resources\\5555555555555555555555555555555555555555555555555555555555555555", "held", 0);
    write_file("package-cache\\resources\\readme.txt", "foreign", 0);
    write_file("package-cache\\resources\\AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "foreign case", 0);
    write_file("package-cache\\resources\\6666666666666666666666666666666666666666666666666666666666666666.01", "foreign suffix", 0);
    write_file("package-cache\\resources\\sub\\7777777777777777777777777777777777777777777777777777777777777777", "in a subdirectory", 0);
    write_file("package-cache\\overrides-like\\8888888888888888888888888888888888888888888888888888888888888888", "outside resources", 0);
    busy = hold_open("package-cache\\resources\\5555555555555555555555555555555555555555555555555555555555555555");
    CHECK(busy != INVALID_HANDLE_VALUE);
    removed = sh_package_cache_evict_resources(root, keep, &asked, &kept, &held, &foreign, &bytes);
    CHECK(removed == 4 && kept == 2 && held == 1 && foreign == 3 && asked == 7);
    CHECK(bytes == strlen("stale 1234567890") + strlen("stale") + strlen("interrupted output") + strlen("legacy proof"));
    CHECK(exists("package-cache\\resources\\1111111111111111111111111111111111111111111111111111111111111111"));
    CHECK(exists("package-cache\\resources\\2222222222222222222222222222222222222222222222222222222222222222.1"));
    CHECK(!exists("package-cache\\resources\\3333333333333333333333333333333333333333333333333333333333333333"));
    CHECK(!exists("package-cache\\resources\\4444444444444444444444444444444444444444444444444444444444444444.2"));
    CHECK(!exists("package-cache\\resources\\.building.1.2.3") && !exists("package-cache\\resources\\.private.4.5.6"));
    CHECK(exists("package-cache\\resources\\5555555555555555555555555555555555555555555555555555555555555555"));
    CHECK(exists("package-cache\\resources\\readme.txt"));
    CHECK(exists("package-cache\\resources\\AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    CHECK(exists("package-cache\\resources\\6666666666666666666666666666666666666666666666666666666666666666.01"));
    CHECK(exists("package-cache\\resources\\sub\\7777777777777777777777777777777777777777777777777777777777777777"));
    CHECK(exists("package-cache\\overrides-like\\8888888888888888888888888888888888888888888888888888888888888888"));
    CloseHandle(busy);
    removed = sh_package_cache_evict_resources(root, keep, &asked, &kept, &held, &foreign, &bytes);
    CHECK(removed == 1 && kept == 2 && held == 0 && foreign == 3 && bytes == 4);
    CHECK(sh_package_cache_evict_resources(root, NULL, NULL, &kept, &held, &foreign, &bytes) == 0);
    remove_tree("package-cache");
}

int main(void)
{
    char temp[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temp), temp));
    snprintf(root, sizeof(root), "%ssmp-cache-sweep-%lu-%llu", temp, GetCurrentProcessId(), GetTickCount64());
    CHECK(CreateDirectoryA(root, NULL));
    test_owner_lock(); test_sweep(); test_evict();
    CHECK(RemoveDirectoryA(root)); /* fails while anything is left behind */
    if (failures) { fprintf(stderr, "package_cache_sweep_test: %d failure(s)\n", failures); return 1; }
    puts("package_cache_sweep_test: OK");
    return 0;
}
