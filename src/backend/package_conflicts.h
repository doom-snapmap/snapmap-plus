/* package_conflicts.h -- which installed packages claim the same file. */
#ifndef BACKEND_PACKAGE_CONFLICTS_H
#define BACKEND_PACKAGE_CONFLICTS_H

#include <stddef.h>
#include "packages.h"

/* Report overlapping engine resources without changing package files or their
 * digests. Input order supplies precedence: higher priority, then name.
 * Identical overlaps are counted; differing bodies name the winner and loser.
 */

#define SH_PKG_CONFLICT_MAX        128
#define SH_PKG_CONFLICT_PATH_CAP   256

typedef struct sh_pkg_conflict {
    char resource[SH_PKG_CONFLICT_PATH_CAP];   /* package-relative, '/'-separated */
    char winner[SH_PACKAGE_NAME_CAP];          /* the package resolution picks */
    char loser[SH_PACKAGE_NAME_CAP];           /* the package it shadows */
    int  identical;                            /* 1 = same bytes, benign */
} sh_pkg_conflict;

/* Scan packages in precedence order within the served namespaces, bounded
 * depth and file capacity. Returns 1 if complete, 0 if a read or capacity
 * limit cuts it short. Always sets found and truncated.
 */
int sh_pkg_conflicts_scan(const sh_package *packages, size_t count,
                          sh_pkg_conflict *out, size_t capacity,
                          size_t *found, int *truncated);

/* Scan the installed set and write the result to the backend log: one summary
 * line, then one line per differing conflict naming winner and loser. Benign
 * identical overlaps are counted in the summary and not listed individually.
 * Returns the number of DIFFERING conflicts found. */
int sh_pkg_conflicts_report(const char *data_root);

#endif /* BACKEND_PACKAGE_CONFLICTS_H */
