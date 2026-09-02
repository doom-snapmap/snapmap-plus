/* package_conflicts.c -- see package_conflicts.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "package_conflicts.h"

/* The namespaces a package can serve. A package's other contents -- its
 * package.json, its readme -- are not reachable as engine resources, so an
 * overlap there means nothing and is not scanned. Keep this in step with the
 * namespace table in overrides.c: a namespace that resolves but is not scanned
 * would collide silently, which is the whole failure this file exists to end. */
static const char *const g_scanned[] = { "decls", "shaders", "resources", "requirements" };

#define PC_MAX_DEPTH   8
#define PC_MAX_FILES   4096

typedef struct pc_entry {
    char resource[SH_PKG_CONFLICT_PATH_CAP];
    char path[MAX_PATH];
    size_t package;
} pc_entry;

typedef struct pc_scan {
    pc_entry *entries;
    size_t count;
    size_t capacity;
    int truncated;
} pc_scan;

static int pc_is_directory(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Compare two files byte for byte. Sizes first, because a difference in size
 * settles it without reading either. An unreadable file is reported as
 * DIFFERING: claiming two files match when one could not be read would turn a
 * real conflict into a silent one, which is the wrong way to be wrong here. */
static int pc_same_bytes(const char *a, const char *b)
{
    FILE *fa = NULL, *fb = NULL;
    char ba[4096], bb[4096];
    long long sa, sb;
    int same = 0;

    if (fopen_s(&fa, a, "rb") != 0 || !fa) return 0;
    if (fopen_s(&fb, b, "rb") != 0 || !fb) { fclose(fa); return 0; }
    if (_fseeki64(fa, 0, SEEK_END) == 0 && _fseeki64(fb, 0, SEEK_END) == 0) {
        sa = _ftelli64(fa);
        sb = _ftelli64(fb);
        if (sa >= 0 && sa == sb && _fseeki64(fa, 0, SEEK_SET) == 0 &&
            _fseeki64(fb, 0, SEEK_SET) == 0) {
            same = 1;
            for (;;) {
                size_t ra = fread(ba, 1, sizeof ba, fa);
                size_t rb = fread(bb, 1, sizeof bb, fb);
                if (ra != rb || (ra && memcmp(ba, bb, ra) != 0)) { same = 0; break; }
                if (ra == 0) break;
            }
        }
    }
    fclose(fa);
    fclose(fb);
    return same;
}

static int pc_record(pc_scan *scan, const char *resource, const char *path,
                     size_t package)
{
    pc_entry *e;
    if (scan->count >= scan->capacity) { scan->truncated = 1; return 0; }
    e = &scan->entries[scan->count];
    if (strcpy_s(e->resource, sizeof e->resource, resource) != 0 ||
        strcpy_s(e->path, sizeof e->path, path) != 0)
        return 0;
    e->package = package;
    scan->count++;
    return 1;
}

/* Walk one directory, recording every regular file below it under its
 * package-relative name. */
static int pc_walk(const char *directory, const char *relative, unsigned depth,
                   pc_scan *scan, size_t package)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    char child_relative[SH_PKG_CONFLICT_PATH_CAP];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int complete = 1;

    if (depth > PC_MAX_DEPTH) return 0;
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", directory) < 0)
        return 0;
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES ||
               e == ERROR_PATH_NOT_FOUND;
    }
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (_snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s",
                        directory, found.cFileName) < 0) { complete = 0; continue; }
        if (_snprintf_s(child_relative, sizeof child_relative, _TRUNCATE, "%s/%s",
                        relative, found.cFileName) < 0) { complete = 0; continue; }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!pc_walk(child, child_relative, depth + 1, scan, package))
                complete = 0;
            continue;
        }
        if (!pc_record(scan, child_relative, child, package)) complete = 0;
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return complete;
}

int sh_pkg_conflicts_scan(const sh_package *packages, size_t count,
                          sh_pkg_conflict *out, size_t capacity,
                          size_t *found, int *truncated)
{
    pc_scan scan;
    size_t i, j, k, n;
    int complete = 1;

    if (found) *found = 0;
    if (truncated) *truncated = 0;
    if (!packages || !out || capacity == 0) return 0;

    scan.entries = (pc_entry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         PC_MAX_FILES * sizeof(pc_entry));
    if (!scan.entries) return 0;
    scan.count = 0;
    scan.capacity = PC_MAX_FILES;
    scan.truncated = 0;

    for (i = 0; i < count; i++) {
        for (n = 0; n < sizeof g_scanned / sizeof g_scanned[0]; n++) {
            char root[MAX_PATH];
            if (_snprintf_s(root, sizeof root, _TRUNCATE, "%s\\%s",
                            packages[i].root, g_scanned[n]) < 0) { complete = 0; continue; }
            if (!pc_is_directory(root)) continue;
            if (!pc_walk(root, g_scanned[n], 0, &scan, i)) complete = 0;
        }
    }

    /* Packages arrive in precedence order, so for any pair the LOWER index is
     * the winner -- the same order the file shadow resolves in. Comparing every
     * later claim against the first one found reports each loser once against
     * the package that actually shadows it, rather than every pair against each
     * other. */
    n = 0;
    for (j = 0; j < scan.count && n < capacity; j++) {
        for (k = 0; k < j; k++) {
            if (scan.entries[k].package == scan.entries[j].package) continue;
            if (_stricmp(scan.entries[k].resource, scan.entries[j].resource) != 0) continue;
            {
                sh_pkg_conflict *c = &out[n];
                memset(c, 0, sizeof *c);
                strcpy_s(c->resource, sizeof c->resource, scan.entries[j].resource);
                strcpy_s(c->winner, sizeof c->winner,
                         packages[scan.entries[k].package].name);
                strcpy_s(c->loser, sizeof c->loser,
                         packages[scan.entries[j].package].name);
                c->identical = pc_same_bytes(scan.entries[k].path, scan.entries[j].path);
                n++;
            }
            break;                       /* one report per losing claim */
        }
    }
    if (n >= capacity && scan.count) scan.truncated = 1;

    if (found) *found = n;
    if (truncated) *truncated = scan.truncated;
    HeapFree(GetProcessHeap(), 0, scan.entries);
    return complete && !scan.truncated;
}

int sh_pkg_conflicts_report(const char *data_root)
{
    sh_package packages[SH_PACKAGES_MAX];
    sh_pkg_conflict *conflicts;
    size_t package_count = 0, found = 0, i;
    int truncated = 0, differing = 0, identical = 0;
    char line[768];

    if (!data_root || !data_root[0]) return 0;
    if (!sh_packages_enumerate(data_root, packages, SH_PACKAGES_MAX, &package_count))
        backend_log("package-conflicts: the package tree could not be read in full; "
                    "the overlap report below may be incomplete");
    if (package_count < 2) return 0;      /* one package cannot collide with itself */

    conflicts = (sh_pkg_conflict *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             SH_PKG_CONFLICT_MAX * sizeof(sh_pkg_conflict));
    if (!conflicts) return 0;

    (void)sh_pkg_conflicts_scan(packages, package_count, conflicts,
                                SH_PKG_CONFLICT_MAX, &found, &truncated);
    for (i = 0; i < found; i++) {
        if (conflicts[i].identical) { identical++; continue; }
        differing++;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "package-conflicts: '%s' is claimed by both '%s' and '%s' with DIFFERENT "
                    "content; '%s' wins by precedence",
                    conflicts[i].resource, conflicts[i].winner, conflicts[i].loser,
                    conflicts[i].winner);
        backend_log(line);
    }
    if (differing || identical || truncated) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "package-conflicts: %u package(s) installed, %d differing overlap(s), "
                    "%d identical overlap(s)%s",
                    (unsigned)package_count, differing, identical,
                    truncated ? " -- LIST TRUNCATED, there may be more" : "");
        backend_log(line);
    }
    HeapFree(GetProcessHeap(), 0, conflicts);
    return differing;
}
