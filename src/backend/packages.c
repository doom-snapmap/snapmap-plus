/* packages.c -- see packages.h.
 *
 * WHY THERE IS NO COMPILE STEP
 *
 * It is tempting to let users author isolated package folders and then "compile"
 * them into the single shared tree the loader used to read. Nothing requires
 * that. DOOM never sees this directory layout: the decl server derives a decl's
 * type and logical name from its path relative to a decls root, and the resource
 * bridge and requirements reader each glob one subdirectory. All three take a
 * root and append a fixed suffix, so supporting many packages -- nested to any
 * depth -- is reading N roots instead of one. No staging, no generated copies to
 * go stale, no bookkeeping about which package wrote which file, and deleting a
 * folder really does uninstall it. A compile step would buy nothing and would
 * add every one of those failure modes back.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "packages.h"

#define PK_OVERRIDES_SUFFIX "\\overrides"
#define PK_MARKER           "package.json"
/* Engine-meaningful, never a package or a grouping folder: the file shadow
 * serves ".inc" shader includes straight out of it. */
#define PK_RESERVED_NAME    "shader_includes"

typedef HANDLE (WINAPI *pk_find_first_fn)(LPCSTR, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *pk_find_next_fn)(HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *pk_find_close_fn)(HANDLE);
typedef DWORD (WINAPI *pk_get_attributes_fn)(LPCSTR);

static pk_find_first_fn g_find_first = FindFirstFileA;
static pk_find_next_fn g_find_next = FindNextFileA;
static pk_find_close_fn g_find_close = FindClose;
static pk_get_attributes_fn g_get_attributes = GetFileAttributesA;

int sh_package_subdir(const sh_package *package, const char *subdirectory,
                      char *out, size_t out_size)
{
    if (!package || !subdirectory || !out || out_size == 0) return 0;
    return _snprintf_s(out, out_size, _TRUNCATE, "%s\\%s",
                       package->root, subdirectory) >= 0;
}

static int pk_is_directory(const char *path)
{
    DWORD attributes = g_get_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int pk_is_file(const char *path)
{
    DWORD attributes = g_get_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Read `"priority": N` out of a package.json, defaulting to 0.
 *
 * Deliberately a narrow scan rather than a JSON parse: this runs on the engine
 * file-open path during capture, the only field needed is one integer, and a
 * malformed or absent marker must yield the default rather than fail a package.
 * Anything unparseable is simply priority 0, which is where an unmarked package
 * would have sorted anyway. */
static int pk_read_priority(const char *directory)
{
    char marker[MAX_PATH];
    char body[1024];
    FILE *file = NULL;
    size_t read_bytes;
    const char *key;
    int sign = 1, value = 0, digits = 0;

    if (_snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\%s",
                    directory, PK_MARKER) < 0) return 0;
    if (fopen_s(&file, marker, "rb") != 0 || !file) return 0;
    read_bytes = fread(body, 1, sizeof(body) - 1, file);
    fclose(file);
    body[read_bytes] = '\0';

    key = strstr(body, "\"priority\"");
    if (!key) return 0;
    key += 10;                                     /* past the quoted key */
    while (*key == ' ' || *key == '	' || *key == ':') key++;
    if (*key == '-') { sign = -1; key++; }
    while (*key >= '0' && *key <= '9' && digits < 9) {
        value = value * 10 + (*key - '0');
        key++;
        digits++;
    }
    return digits ? sign * value : 0;
}

static int pk_has_marker(const char *directory)
{
    char marker[MAX_PATH];
    if (_snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\%s",
                    directory, PK_MARKER) < 0) return 0;
    return pk_is_file(marker);
}

static int pk_append(sh_package *out, size_t capacity, size_t *count,
                     const char *name, const char *root, int priority)
{
    size_t i;
    if (*count >= capacity) return 0;
    for (i = 0; i < *count; i++)
        if (_stricmp(out[i].name, name) == 0) return 1;   /* already present */
    if (strcpy_s(out[*count].name, sizeof(out[*count].name), name) != 0 ||
        strcpy_s(out[*count].root, sizeof(out[*count].root), root) != 0)
        return 0;
    out[*count].priority = priority;
    (*count)++;
    return 1;
}

/* Resolution precedence: higher priority first, then name. The name is only a
 * tie-break -- it used to be the WHOLE rule, which meant a package called
 * `boss-demons` silently beat one called `cyberdemon` for any file they both
 * carried, purely because b sorts before c. Insertion sort is stable, and the
 * comparison is total, so the order is identical on every machine. */
static int pk_before(const sh_package *a, const sh_package *b)
{
    if (a->priority != b->priority) return a->priority > b->priority;
    return _stricmp(a->name, b->name) < 0;
}

static void pk_sort(sh_package *out, size_t count)
{
    size_t i, j;
    for (i = 1; i < count; i++) {
        sh_package key = out[i];
        j = i;
        while (j > 0 && pk_before(&key, &out[j - 1])) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
}

/* Search `directory` (whose path below overrides\ is `prefix`) for packages.
 * A directory carrying the marker IS a package and is not descended into; any
 * other directory is a grouping folder and is searched. Returns 0 if any part
 * of the subtree could not be read or did not fit. */
static int pk_scan(const char *directory, const char *prefix, unsigned depth,
                   sh_package *out, size_t capacity, size_t *count)
{
    char pattern[MAX_PATH];
    char child[MAX_PATH];
    char name[SH_PACKAGE_NAME_CAP];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int complete = 1;

    if (depth > SH_PACKAGES_MAX_DEPTH) return 0;
    if (_snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", directory) < 0)
        return 0;
    search = g_find_first(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
    }
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (strcmp(found.cFileName, ".") == 0 ||
            strcmp(found.cFileName, "..") == 0) continue;
        if (depth == 0 && _stricmp(found.cFileName, PK_RESERVED_NAME) == 0) continue;

        if (_snprintf_s(child, sizeof(child), _TRUNCATE, "%s\\%s",
                        directory, found.cFileName) < 0) { complete = 0; continue; }
        if (_snprintf_s(name, sizeof(name), _TRUNCATE, "%s%s%s",
                        prefix, prefix[0] ? "/" : "", found.cFileName) < 0) {
            complete = 0; continue;
        }

        /* The marker is the ONLY thing that makes a package. The pre-package
         * `generated` tree used to be special-cased here as a nameless package;
         * the installer now migrates it into a real one, so this stays a single
         * rule instead of a rule plus an exception. A directory without a marker
         * is a grouping folder and is searched, exactly like any other. */
        if (pk_has_marker(child)) {
            if (!pk_append(out, capacity, count, name, child,
                           pk_read_priority(child))) complete = 0;
            continue;                       /* a package is a leaf */
        }
        if (!pk_scan(child, name, depth + 1, out, capacity, count)) complete = 0;
    } while (g_find_next(search, &found));
    g_find_close(search);
    return complete;
}

int sh_packages_enumerate(const char *data_root, sh_package *out, size_t capacity,
                          size_t *count)
{
    char overrides[MAX_PATH];
    int complete;

    if (count) *count = 0;
    if (!data_root || !data_root[0] || !out || capacity == 0 || !count) return 0;
    if (_snprintf_s(overrides, sizeof(overrides), _TRUNCATE, "%s%s",
                    data_root, PK_OVERRIDES_SUFFIX) < 0) return 0;
    if (!pk_is_directory(overrides)) return 1;   /* nothing installed yet */

    complete = pk_scan(overrides, "", 0, out, capacity, count);
    pk_sort(out, *count);
    return complete;
}

#ifdef SH_PACKAGES_TESTING
void sh_packages_test_set_api(const sh_packages_test_find_api *api)
{
    if (!api) return;
    g_find_first = api->find_first;
    g_find_next = api->find_next;
    g_find_close = api->find_close;
    g_get_attributes = api->get_attributes;
}

void sh_packages_test_reset_api(void)
{
    g_find_first = FindFirstFileA;
    g_find_next = FindNextFileA;
    g_find_close = FindClose;
    g_get_attributes = GetFileAttributesA;
}
#endif
