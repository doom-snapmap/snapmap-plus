/* Enumerate marked package roots directly; no staged or merged tree is
 * needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "packages.h"

#define PK_OVERRIDES_SUFFIX "\\overrides"
typedef HANDLE (WINAPI *pk_find_first_fn)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *pk_find_next_fn)(HANDLE, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *pk_find_close_fn)(HANDLE);
typedef DWORD (WINAPI *pk_get_attributes_fn)(LPCWSTR);

static pk_find_first_fn g_find_first = FindFirstFileW;
static pk_find_next_fn g_find_next = FindNextFileW;
static pk_find_close_fn g_find_close = FindClose;
static pk_get_attributes_fn g_get_attributes = GetFileAttributesW;

int sh_package_subdir(const sh_package *package, const char *subdirectory,
                      char *out, size_t out_size)
{
    if (!package || !subdirectory || !out || out_size == 0) return 0;
    return _snprintf_s(out, out_size, _TRUNCATE, "%s\\%s",
                       package->root, subdirectory) >= 0;
}

/* UTF-8 path to a wide path. Absolute drive paths use the extended-length form,
 * so group depth is not limited to MAX_PATH; that form needs single
 * backslashes. Returns NULL for invalid UTF-8 or allocation failure. */
static wchar_t *pk_wide(const char *path, const wchar_t *suffix)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    size_t suffix_length = wcslen(suffix), prefix = 0, i, j;
    wchar_t *out;
    if (length <= 0) return NULL;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) prefix = 4;
    if ((size_t)length > (SIZE_MAX / sizeof(wchar_t)) - prefix - suffix_length) return NULL;
    out = (wchar_t *)malloc((prefix + (size_t)length + suffix_length) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, L"\\\\?\\", prefix * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out + prefix, length)) {
        free(out); return NULL;
    }
    for (i = j = prefix; out[i]; i++) {
        wchar_t c = out[i] == L'/' ? L'\\' : out[i];
        if (prefix && c == L'\\' && j > prefix && out[j - 1] == L'\\') continue;
        out[j++] = c;
    }
    memcpy(out + j, suffix, (suffix_length + 1) * sizeof(wchar_t));
    return out;
}

/* 1 converted, 0 not valid Unicode, -1 allocation failure. */
static int pk_utf8(const wchar_t *text, char **out)
{
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    *out = NULL;
    if (length <= 0) return 0;
    *out = (char *)malloc((size_t)length);
    if (!*out) return -1;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, *out, length, NULL, NULL)) {
        free(*out); *out = NULL; return 0;
    }
    return 1;
}

static char *pk_join(const char *a, const char *separator, const char *b)
{
    size_t la = strlen(a), ls = strlen(separator), lb = strlen(b);
    char *out;
    if (la > SIZE_MAX - ls - lb - 1) return NULL;
    out = (char *)malloc(la + ls + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la); memcpy(out + la, separator, ls); memcpy(out + la + ls, b, lb + 1);
    return out;
}

/* 1 marker file, 0 none, -1 unreadable: an unknown marker is not a group. */
static int pk_has_marker(const char *directory)
{
    wchar_t *marker = pk_wide(directory, L"\\package.json");
    DWORD attributes, error;
    if (!marker) return -1;
    attributes = g_get_attributes(marker);
    error = GetLastError();
    free(marker);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    return !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

typedef struct pk_inventory { sh_package *items; size_t count, capacity; } pk_inventory;

static void pk_copy_shortened(char *out, size_t capacity, const char *text)
{
    size_t length = strlen(text);
    if (length >= capacity) {
        length = capacity - 1;
        while (length && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
    }
    memcpy(out, text, length); out[length] = 0;
}

static int pk_append(pk_inventory *inventory, const char *name, const char *root, const char *problem)
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
    entry = &inventory->items[inventory->count++];
    memset(entry, 0, sizeof(*entry));
    if (strlen(name) >= sizeof(entry->name) || strlen(root) >= sizeof(entry->root))
        problem = "package path is too long for Snapmap+ to load; move the package to a shorter folder path";
    pk_copy_shortened(entry->name, sizeof(entry->name), name);
    pk_copy_shortened(entry->root, sizeof(entry->root), root);
    entry->problem = problem;
    return 1;
}

/* Stable display order; conflicts are resolved by the compiler. */
static int pk_compare(const void *a, const void *b)
{
    return _stricmp(((const sh_package *)a)->name, ((const sh_package *)b)->name);
}

/* Search `directory` (whose path below overrides\ is `prefix`) for packages.
 * A directory carrying the marker IS a package and is not descended into; any
 * other directory is a grouping folder and is searched. Returns 0 if any part
 * of the discovery tree could not be read or recorded. */
static int pk_scan(const char *directory, const char *prefix, pk_inventory *inventory)
{
    wchar_t *pattern = pk_wide(directory, L"\\*");
    WIN32_FIND_DATAW found;
    HANDLE search;
    int complete = 1;

    if (!pattern) return 0;
    search = g_find_first(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
    }
    do {
        char *name = NULL, *child = NULL, *child_name = NULL;
        int converted, marked;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        converted = pk_utf8(found.cFileName, &name);
        if (converted < 0) { complete = 0; break; }
        if (!converted) {
            /* Packages below an unrepresentable name cannot be addressed;
             * report the folder instead of omitting it. */
            child = pk_join(directory, "\\", "?");
            child_name = pk_join(prefix, prefix[0] ? "/" : "", "?");
            complete = child && child_name && pk_append(inventory, child_name, child,
                "folder name is not valid Unicode; rename it");
        } else {
            child = pk_join(directory, "\\", name);
            child_name = pk_join(prefix, prefix[0] ? "/" : "", name);
            marked = child && child_name ? pk_has_marker(child) : -1;
            if (marked < 0) complete = 0;
            else if (marked) complete = pk_append(inventory, child_name, child, NULL);
            else complete = pk_scan(child, child_name, inventory);
        }
        free(name); free(child); free(child_name);
        if (!complete) break;
    } while (g_find_next(search, &found));
    /* FindNextFile also returns FALSE when enumeration was interrupted. Read
     * its error before FindClose can overwrite it: a partial package set is
     * not a complete snapshot, even if earlier entries were admitted. */
    if (complete && GetLastError() != ERROR_NO_MORE_FILES) complete = 0;
    g_find_close(search);
    return complete;
}

int sh_packages_enumerate(const char *data_root, sh_package **out, size_t *count)
{
    char *overrides;
    wchar_t *wide;
    DWORD attributes, error;
    int complete;
    pk_inventory inventory = {0};

    if (out) { free(*out); *out = NULL; }
    if (count) *count = 0;
    if (!data_root || !data_root[0] || !out || !count) return 0;
    overrides = pk_join(data_root, "", PK_OVERRIDES_SUFFIX);
    wide = overrides ? pk_wide(overrides, L"") : NULL;
    if (!wide) { free(overrides); return 0; }
    attributes = g_get_attributes(wide);
    error = GetLastError();
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        free(overrides);
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(overrides); return 0;
    }

    complete = pk_scan(overrides, "", &inventory);
    free(overrides);
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
    g_find_first = FindFirstFileW;
    g_find_next = FindNextFileW;
    g_find_close = FindClose;
    g_get_attributes = GetFileAttributesW;
}
#endif
