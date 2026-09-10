/* Discover override package folders and their resolution order. */
#ifndef BACKEND_PACKAGES_H
#define BACKEND_PACKAGES_H

#include <windows.h>
#include <stddef.h>

#define SH_PACKAGES_MAX      64
#define SH_PACKAGE_NAME_CAP  160
/* Bound grouping-folder depth to keep enumeration finite. */
#define SH_PACKAGES_MAX_DEPTH 8

/* A package is a folder below overrides with a package.json marker. Folders
 * without a marker are grouping folders; enumeration stops descending at each
 * package. Decl identities follow decls/<type>/<logical-name>.decl.
 *
 * Shadow lookup uses descending package priority (default 0), then case-
 * insensitive name. package_conflicts reports overlapping files. New decl
 * publication instead rejects differing bodies for the same identity and
 * combines identical duplicates. Direct overrides/<engine-name> lookup
 * remains a separate shadow path.
 */
typedef struct sh_package {
    char name[SH_PACKAGE_NAME_CAP];  /* path below overrides\, '/'-separated */
    char root[MAX_PATH];             /* absolute path to the package folder */
    int  priority;                   /* package.json "priority", default 0 */
} sh_package;

/* Enumerate below <data_root>/overrides by descending priority, then case-
 * insensitive name. Skip reparse points and stop descending at package
 * markers. Returns 1 for a complete result, including empty; 0 for read or
 * capacity failure. Always sets count; callers requiring a complete snapshot
 * must reject partial results.
 */
int sh_packages_enumerate(const char *data_root, sh_package *out, size_t capacity,
                          size_t *count);

/* Join `<package root>\<subdirectory>` into `out`. Returns 0 when it would not
 * fit. */
int sh_package_subdir(const sh_package *package, const char *subdirectory,
                      char *out, size_t out_size);

#ifdef SH_PACKAGES_TESTING
typedef struct sh_packages_test_find_api {
    HANDLE (WINAPI *find_first)(LPCSTR pattern, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_next)(HANDLE search, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_close)(HANDLE search);
    DWORD (WINAPI *get_attributes)(LPCSTR path);
} sh_packages_test_find_api;

void sh_packages_test_set_api(const sh_packages_test_find_api *api);
void sh_packages_test_reset_api(void);
#endif

#endif /* BACKEND_PACKAGES_H */
