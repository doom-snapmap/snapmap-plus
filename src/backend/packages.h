/* Discover override package folders and their resolution order. */
#ifndef BACKEND_PACKAGES_H
#define BACKEND_PACKAGES_H

#include <windows.h>
#include <stddef.h>

#define SH_PACKAGE_NAME_CAP  MAX_PATH

/* Discover outer package folders. A folder with package.json is one authored
 * delivery unit; nested components are inventoried by package_sources. Unmarked
 * folders are groups, never direct engine-provider roots. */
typedef struct sh_package {
    char name[SH_PACKAGE_NAME_CAP];  /* UTF-8 path below overrides\, '/'-separated */
    char root[MAX_PATH];             /* absolute UTF-8 path to the package folder */
    /* A marked folder the runtime cannot address: its path does not fit these
     * buffers or its name is not valid Unicode. The package is reported, never
     * silently omitted; name and root hold a shortened spelling for messages. */
    const char *problem;
} sh_package;

/* Enumerate below <data_root>/overrides by case-insensitive folder name with
 * wide-character APIs, so Unicode and long group paths are found. Skip
 * reparse points and stop descending at package markers. Returns 1 for a
 * complete result, including empty; 0 for a read or allocation failure of the
 * discovery tree itself. Initialize *out to NULL; a repeat call releases the
 * prior inventory. Free *out when finished. Failure clears both outputs. */
int sh_packages_enumerate(const char *data_root, sh_package **out, size_t *count);

/* Join `<package root>\<subdirectory>` into `out`. Returns 0 when it would not
 * fit. */
int sh_package_subdir(const sh_package *package, const char *subdirectory,
                      char *out, size_t out_size);

#ifdef SH_PACKAGES_TESTING
typedef struct sh_packages_test_find_api {
    HANDLE (WINAPI *find_first)(LPCWSTR pattern, LPWIN32_FIND_DATAW found);
    BOOL (WINAPI *find_next)(HANDLE search, LPWIN32_FIND_DATAW found);
    BOOL (WINAPI *find_close)(HANDLE search);
    DWORD (WINAPI *get_attributes)(LPCWSTR path);
} sh_packages_test_find_api;

void sh_packages_test_set_api(const sh_packages_test_find_api *api);
void sh_packages_test_reset_api(void);
#endif

#endif /* BACKEND_PACKAGES_H */
