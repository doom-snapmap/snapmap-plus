/* Enumerate marked package roots directly; no staged or merged tree is
 * needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "packages.h"

#define PK_OVERRIDES_SUFFIX "\\overrides"
#define PK_MARKER           "package.json"
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

static int pk_is_file(const char *path)
{
    DWORD attributes = g_get_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int pk_has_marker(const char *directory)
{
    char marker[MAX_PATH];
    if (_snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\%s",
                    directory, PK_MARKER) < 0) return 0;
    return pk_is_file(marker);
}

typedef struct pk_inventory { sh_package *items; size_t count, capacity; } pk_inventory;

static int pk_append(pk_inventory *inventory, const char *name, const char *root)
{
    sh_package *grown, *entry;
    size_t capacity;
    if (inventory->count == inventory->capacity) {
        capacity = inventory->capacity ? inventory->capacity * 2 : 16;
        if (capacity < inventory->capacity || capacity > SIZE_MAX / sizeof(*grown)) return 0;
        grown = (sh_package *)realloc(inventory->items, capacity * sizeof(*grown));
        if (!grown) return 0;
        inventory->items = grown; inventory->capacity = capacity;
    }
    entry = &inventory->items[inventory->count];
    if (strcpy_s(entry->name, sizeof(entry->name), name) ||
        strcpy_s(entry->root, sizeof(entry->root), root)) return 0;
    inventory->count++; return 1;
}

/* Stable display order; conflicts are resolved by the compiler. */
static int pk_compare(const void *a, const void *b)
{
    return _stricmp(((const sh_package *)a)->name, ((const sh_package *)b)->name);
}

/* Search `directory` (whose path below overrides\ is `prefix`) for packages.
 * A directory carrying the marker IS a package and is not descended into; any
 * other directory is a grouping folder and is searched. Returns 0 if any part
 * of the subtree could not be read or did not fit. */
static int pk_scan(const char *directory, const char *prefix, pk_inventory *inventory)
{
    char pattern[MAX_PATH];
    char child[MAX_PATH];
    char name[SH_PACKAGE_NAME_CAP];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int complete = 1;

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

        if (_snprintf_s(child, sizeof(child), _TRUNCATE, "%s\\%s",
                        directory, found.cFileName) < 0) { complete = 0; continue; }
        if (_snprintf_s(name, sizeof(name), _TRUNCATE, "%s%s%s",
                        prefix, prefix[0] ? "/" : "", found.cFileName) < 0) {
            complete = 0; continue;
        }

        /* Only package.json marks a package. Unmarked folders remain
         * searchable groups.
         */
        if (pk_has_marker(child)) {
            if (!pk_append(inventory, name, child)) complete = 0;
            continue;                       /* a package is a leaf */
        }
        if (!pk_scan(child, name, inventory)) complete = 0;
    } while (g_find_next(search, &found));
    /* FindNextFile also returns FALSE when enumeration was interrupted. Read
     * its error before FindClose can overwrite it: a partial package set is
     * not a complete snapshot, even if earlier entries were admitted. */
    if (GetLastError() != ERROR_NO_MORE_FILES) complete = 0;
    g_find_close(search);
    return complete;
}

int sh_packages_enumerate(const char *data_root, sh_package **out, size_t *count)
{
    char overrides[MAX_PATH];
    DWORD attributes;
    int complete;
    pk_inventory inventory = {0};

    if (out) { free(*out); *out = NULL; }
    if (count) *count = 0;
    if (!data_root || !data_root[0] || !out || !count) return 0;
    if (_snprintf_s(overrides, sizeof(overrides), _TRUNCATE, "%s%s",
                    data_root, PK_OVERRIDES_SUFFIX) < 0) return 0;
    attributes = g_get_attributes(overrides);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;

    complete = pk_scan(overrides, "", &inventory);
    if (!complete) {
        /* No consumer may mistake an admitted prefix for an inventory. */
        free(inventory.items); return 0;
    }
    if (inventory.count) qsort(inventory.items, inventory.count, sizeof(*inventory.items), pk_compare);
    *out = inventory.items; *count = inventory.count;
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
