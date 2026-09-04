/* package_conflicts.h -- which installed packages claim the same file. */
#ifndef BACKEND_PACKAGE_CONFLICTS_H
#define BACKEND_PACKAGE_CONFLICTS_H

#include <stddef.h>
#include "packages.h"

/* Why this exists.
 *
 * Two packages may legitimately carry the same file. A "boss-demons" pack and a
 * "cyberdemon" pack can both ship `decls/entitydef/.../cyberdemon.decl`, and
 * that is not automatically an error -- they may be byte-identical, or the
 * player may deliberately want one to override the other.
 *
 * What is NOT acceptable is deciding it silently. Before this, the file shadow
 * resolved such an overlap by taking the first package in directory order, so
 * the winner came down to the alphabet and nothing told anyone it had happened.
 * A player would see one package's content while believing they had the other's,
 * with no way to find out.
 *
 * So packages are ordered by declared priority (see packages.h) and every
 * overlap is reported here. Identical content is a BENIGN overlap: the bytes
 * served are the same whoever wins, so it is recorded and not warned about.
 * Differing content is a real conflict and is named, with its winner and loser,
 * so the outcome is a visible consequence of a declared order.
 *
 * Nothing here modifies a package. Packages stay exactly as authored -- their
 * digest is their identity, it is what a map matches against to know whether it
 * has what it needs, and rewriting a package on disk to remove a duplicate
 * would break that identity and make the package non-portable. Deduplication is
 * a RESOLUTION-time decision, never an on-disk one.
 */

#define SH_PKG_CONFLICT_MAX        128
#define SH_PKG_CONFLICT_PATH_CAP   256

typedef struct sh_pkg_conflict {
    char resource[SH_PKG_CONFLICT_PATH_CAP];   /* package-relative, '/'-separated */
    char winner[SH_PACKAGE_NAME_CAP];          /* the package resolution picks */
    char loser[SH_PACKAGE_NAME_CAP];           /* the package it shadows */
    int  identical;                            /* 1 = same bytes, benign */
} sh_pkg_conflict;

/* Scan `packages` (already in precedence order) for files more than one package
 * claims. Returns 1 when the scan completed, 0 when it was cut short by a
 * directory it could not read or by capacity; `*found` and `*truncated` are
 * always set. A package that carries no scanned namespace contributes nothing.
 *
 * Cost is bounded: only the namespaces a package can actually serve are walked,
 * to a fixed depth, with a hard cap on files considered. */
int sh_pkg_conflicts_scan(const sh_package *packages, size_t count,
                          sh_pkg_conflict *out, size_t capacity,
                          size_t *found, int *truncated);

/* Scan the installed set and write the result to the backend log: one summary
 * line, then one line per differing conflict naming winner and loser. Benign
 * identical overlaps are counted in the summary and not listed individually.
 * Returns the number of DIFFERING conflicts found. */
int sh_pkg_conflicts_report(const char *data_root);

#endif /* BACKEND_PACKAGE_CONFLICTS_H */
