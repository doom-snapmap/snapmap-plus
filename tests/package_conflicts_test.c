/* package_conflicts_test.c -- overlap detection and its benign/real split.
 *
 * The question this answers is the one a player would ask: if two packages I
 * installed both carry the same file, which one am I actually running, and does
 * anything tell me? Every case here builds a real directory tree and scans it,
 * because the whole mechanism is filesystem shape.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "package_conflicts.h"

static int failed;
static char g_root[MAX_PATH];

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
    }                                                                           \
} while (0)

void backend_log(const char *message) { (void)message; }

static void rm_rf(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                continue;
            _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_rf(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}

static void make_dirs(const char *path)
{
    char build[MAX_PATH];
    size_t i;
    strcpy_s(build, sizeof build, path);
    for (i = 0; build[i]; i++) {
        if (build[i] == '\\' && i > 3) {
            build[i] = '\0';
            CreateDirectoryA(build, NULL);
            build[i] = '\\';
        }
    }
    CreateDirectoryA(build, NULL);
}

static void write_file(const char *path, const char *body)
{
    char dir[MAX_PATH];
    char *slash;
    FILE *f = NULL;
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    if (slash) { *slash = '\0'; make_dirs(dir); }
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
    }
}

/* A package is a folder with package.json; `decls/...` is what it can serve. */
static void make_package(const char *name, const char *decl_relative, const char *body,
                         const char *marker_body)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides\\%s\\package.json", g_root, name);
    write_file(path, marker_body ? marker_body : "{}");
    if (decl_relative) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides\\%s\\decls\\%s",
                    g_root, name, decl_relative);
        write_file(path, body);
    }
}

static void fresh(void)
{
    char overrides[MAX_PATH];
    _snprintf_s(overrides, sizeof overrides, _TRUNCATE, "%s\\overrides", g_root);
    rm_rf(overrides);
    make_dirs(overrides);
}

int main(void)
{
    sh_package packages[SH_PACKAGES_MAX];
    sh_pkg_conflict conflicts[SH_PKG_CONFLICT_MAX];
    size_t count = 0, found = 0;
    int truncated = 0;
    char temp[MAX_PATH];

    GetTempPathA(sizeof temp, temp);
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%ssnapmap_plus_conflicts_test", temp);
    rm_rf(g_root);
    make_dirs(g_root);

    /* Two packages, no overlap: nothing to report. */
    fresh();
    make_package("alpha", "entitydef\\a.decl", "A", NULL);
    make_package("beta",  "entitydef\\b.decl", "B", NULL);
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 2);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 0);

    /* Same file, SAME bytes: a benign overlap. Whoever wins, the player gets the
     * same content, so it is recorded and not treated as a problem. */
    fresh();
    make_package("alpha", "entitydef\\shared.decl", "IDENTICAL", NULL);
    make_package("beta",  "entitydef\\shared.decl", "IDENTICAL", NULL);
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 1);
    CHECK(conflicts[0].identical == 1);

    /* Same file, DIFFERENT bytes: a real conflict, and the winner is the package
     * resolution will actually serve. */
    fresh();
    make_package("alpha", "entitydef\\shared.decl", "ONE", NULL);
    make_package("beta",  "entitydef\\shared.decl", "TWO", NULL);
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 1);
    CHECK(conflicts[0].identical == 0);
    CHECK(strcmp(conflicts[0].winner, "alpha") == 0);   /* name order, equal priority */
    CHECK(strcmp(conflicts[0].loser, "beta") == 0);
    CHECK(strstr(conflicts[0].resource, "shared.decl") != NULL);

    /* PRIORITY BEATS THE ALPHABET. This is the case the old order got wrong: a
     * package called `beta` could never outrank `alpha` no matter what the
     * player wanted, because the sort was the name and nothing else. */
    fresh();
    make_package("alpha", "entitydef\\shared.decl", "ONE", "{}");
    make_package("beta",  "entitydef\\shared.decl", "TWO", "{ \"priority\": 10 }");
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 2);
    CHECK(strcmp(packages[0].name, "beta") == 0);       /* higher priority sorts first */
    CHECK(packages[0].priority == 10);
    CHECK(packages[1].priority == 0);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 1);
    CHECK(strcmp(conflicts[0].winner, "beta") == 0);
    CHECK(strcmp(conflicts[0].loser, "alpha") == 0);

    /* A negative priority sorts BELOW the default, so a package can be pushed
     * under everything without renaming it. */
    fresh();
    make_package("alpha", "entitydef\\shared.decl", "ONE", "{ \"priority\": -5 }");
    make_package("beta",  "entitydef\\shared.decl", "TWO", "{}");
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(strcmp(packages[0].name, "beta") == 0);
    CHECK(packages[1].priority == -5);

    /* A malformed marker is priority 0, never a failed package: a broken
     * package.json must not make a package vanish from the install. */
    fresh();
    make_package("alpha", "entitydef\\a.decl", "A", "{ \"priority\": not-a-number }");
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(count == 1);
    CHECK(packages[0].priority == 0);

    /* Files OUTSIDE a servable namespace cannot collide, because nothing can ask
     * for them. Two packages sharing a readme is not a conflict. */
    fresh();
    make_package("alpha", NULL, NULL, NULL);
    make_package("beta",  NULL, NULL, NULL);
    {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\overrides\\alpha\\readme.txt", g_root);
        write_file(p, "hello");
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\overrides\\beta\\readme.txt", g_root);
        write_file(p, "different");
    }
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 0);

    /* One package cannot conflict with itself, however many files it carries. */
    fresh();
    make_package("solo", "entitydef\\a.decl", "A", NULL);
    {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\overrides\\solo\\decls\\entitydef\\b.decl", g_root);
        write_file(p, "A");                        /* same bytes, same package */
    }
    CHECK(sh_packages_enumerate(g_root, packages, SH_PACKAGES_MAX, &count) == 1);
    CHECK(sh_pkg_conflicts_scan(packages, count, conflicts, SH_PKG_CONFLICT_MAX,
                                &found, &truncated) == 1);
    CHECK(found == 0);

    rm_rf(g_root);
    if (failed) {
        fprintf(stderr, "%d package-conflicts test(s) failed\n", failed);
        return 1;
    }
    puts("package conflicts tests passed");
    return 0;
}
