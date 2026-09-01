/* packages_test.c -- per-package override discovery.
 *
 * The contract these tests pin is what makes "drag the folder in" safe: a
 * directory only counts as a package once it says so with a package.json, the
 * pre-package shared tree keeps working, and the order two machines see is the
 * same order. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "packages.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static int make_dir(const char *path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int touch(const char *path)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(file);
    return 1;
}

static void join(char *out, size_t size, const char *a, const char *b)
{
    _snprintf_s(out, size, _TRUNCATE, "%s\\%s", a, b);
}

static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 ||
                strcmp(found.cFileName, "..") == 0) continue;
            join(child, sizeof(child), path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}

/* Create <overrides>\<relative> (making every parent), and its package.json
 * unless `marked` is 0. `relative` uses '/' separators. */
static void install(const char *overrides, const char *relative, int marked)
{
    char dir[MAX_PATH], marker[MAX_PATH];
    size_t i;
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", overrides, relative);
    for (i = 0; dir[i]; i++) {
        if (dir[i] != '/') continue;
        dir[i] = 0;
        CHECK(make_dir(dir));
        dir[i] = '\\';
    }
    CHECK(make_dir(dir));
    if (marked) {
        join(marker, sizeof(marker), dir, "package.json");
        CHECK(touch(marker));
    }
}

static int index_of(const sh_package *packages, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (strcmp(packages[i].name, name) == 0) return (int)i;
    return -1;
}

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], overrides[MAX_PATH];
    char sub[MAX_PATH], expected[MAX_PATH];
    sh_package packages[SH_PACKAGES_MAX];
    size_t count = 0;
    DWORD pid = GetCurrentProcessId();

    GetTempPathA(sizeof(temp), temp);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssh_packages_test_%lu",
                temp, (unsigned long)pid);
    remove_tree(root);
    CHECK(make_dir(root));

    /* A data root with no overrides directory at all is a complete, empty
     * enumeration -- a fresh install must not look like a read failure. */
    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 0);

    join(overrides, sizeof(overrides), root, "overrides");
    CHECK(make_dir(overrides));
    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 0);

    install(overrides, "cyberdemon", 1);
    install(overrides, "four-demon-runes", 1);
    install(overrides, "notes", 0);          /* no marker: not a package */
    install(overrides, "generated", 0);      /* no marker: a grouping folder like any other */
    install(overrides, "shader_includes", 0);/* reserved: never a package */
    join(sub, sizeof(sub), overrides, "loose-file.txt");
    CHECK(touch(sub));                       /* a file is never a package */

    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 2);
    CHECK(index_of(packages, count, "notes") < 0);
    CHECK(index_of(packages, count, "shader_includes") < 0);
    CHECK(index_of(packages, count, "loose-file.txt") < 0);
    CHECK(index_of(packages, count, "cyberdemon") == 0);
    CHECK(index_of(packages, count, "four-demon-runes") == 1);
    CHECK(index_of(packages, count, "generated") < 0);

    /* A user may organise installs into grouping folders to any depth. A
     * grouping folder is searched, never returned, and its name becomes part of
     * the package's identity so two groups may hold like-named packages. */
    install(overrides, "editor/lifts", 1);
    install(overrides, "editor/toybox", 1);
    install(overrides, "editor/scratch", 0);
    install(overrides, "demons/hell/imps", 1);
    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 5);
    CHECK(index_of(packages, count, "editor") < 0);
    CHECK(index_of(packages, count, "editor/scratch") < 0);
    CHECK(index_of(packages, count, "editor/lifts") >= 0);
    CHECK(index_of(packages, count, "editor/toybox") >= 0);
    CHECK(index_of(packages, count, "demons/hell/imps") >= 0);
    /* Sorted by full name, so a group's members stay adjacent and in order. */
    CHECK(index_of(packages, count, "demons/hell/imps") <
          index_of(packages, count, "editor/lifts"));
    CHECK(index_of(packages, count, "editor/lifts") <
          index_of(packages, count, "editor/toybox"));

    /* A package is a leaf: anything below it is its own content, never another
     * package, so its layout always means what the package layout says. */
    install(overrides, "cyberdemon/decls", 1);
    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 5);
    CHECK(index_of(packages, count, "cyberdemon/decls") < 0);

    /* The root each package reports is its own folder, and subdirectories are
     * joined below it -- this is the whole isolation guarantee. */
    CHECK(sh_package_subdir(&packages[0], "decls", sub, sizeof(sub)) == 1);
    join(expected, sizeof(expected), packages[0].root, "decls");
    CHECK(strcmp(sub, expected) == 0);
    CHECK(sh_package_subdir(&packages[0], "decls", sub, 8) == 0);
    CHECK(sh_package_subdir(NULL, "decls", sub, sizeof(sub)) == 0);

    /* Bad arguments report failure and still leave the count defined. */
    count = 99;
    CHECK(sh_packages_enumerate(NULL, packages, SH_PACKAGES_MAX, &count) == 0);
    CHECK(count == 0);
    count = 99;
    CHECK(sh_packages_enumerate(root, NULL, SH_PACKAGES_MAX, &count) == 0);
    CHECK(count == 0);
    CHECK(sh_packages_enumerate(root, packages, 0, &count) == 0);

    /* More packages than the array holds is an incomplete enumeration, not a
     * silent truncation: the caller is told so it can refuse. */
    CHECK(sh_packages_enumerate(root, packages, 2, &count) == 0);
    CHECK(count == 2);

    /* A tree deeper than the bound is reported incomplete rather than silently
     * abandoned partway. */
    install(overrides, "a/b/c/d/e/f/g/h/i/deep", 1);
    CHECK(sh_packages_enumerate(root, packages, SH_PACKAGES_MAX, &count) == 0);

    remove_tree(root);
    if (g_failed) {
        fprintf(stderr, "packages_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("packages_test: ok\n");
    return 0;
}
