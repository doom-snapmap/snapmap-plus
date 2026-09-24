/* Authored component lookup, independent of delivery grouping and IDs. */
#ifndef SH_PACKAGE_SCOPE_H
#define SH_PACKAGE_SCOPE_H
#include "package_sources.h"

typedef struct sh_package_scope {
    /* Borrows all author data. Only files/components arrays are owned here.
     * Original indices are retained so compiled ownership needs no remapping. */
    sh_package_sources sources;
    size_t component;
} sh_package_scope;

/* A component sees its own assets, then enclosing component assets, then the
 * engine baseline supplied to the compiler. Siblings and other delivery units
 * never fill a missing dependency. The nearest authored path wins within this
 * lexical scope; no metadata ID, local/delivered flag or folder ordering enters
 * resolution. Ordinary folders without descriptors introduce no new scope.
 * Source packages remain exact delivery units, including shadowed members.
 * Initialize out to zero; replacement/failure frees only the borrowed view. */
int sh_package_scope_open(const sh_package_sources *sources, size_t component,
    sh_package_scope *out, char *error, size_t capacity);
/* A historical map context composes exactly its recorded delivery owners.
 * Retain duplicate contributions for the old compiler's collection rules;
 * do not apply isolated component shadowing. Unselected local/new packages
 * contribute neither files nor descriptor policy. Empty owner sets are valid.
 * The view retains original indices and reports component == SIZE_MAX. */
int sh_package_scope_composed(const sh_package_sources *sources,
    const size_t *owners, size_t count, sh_package_scope *out, char *error, size_t capacity);
/* Reconstruct that composed context from persistent complete-tree identities.
 * Match every requested tree against this inventory only. Identical delivery
 * copies retain all owners; duplicate requests collapse. Missing trees refuse
 * the whole context, never substitute a same-ID package from another source.
 * The caller supplies the private map inventory, not the installed library. */
int sh_package_scope_composed_keys(const sh_package_sources *sources,
    const unsigned char (*fingerprints)[32], size_t count,
    sh_package_scope *out, char *error, size_t capacity);

/* Portable source provenance uses the complete authored tree fingerprint and
 * exact component-relative path. Neither installed folder nor descriptor ID is
 * a lookup key. Return all matching component indices, including identical
 * delivery copies; graph equivalence subsequently deduplicates their assets.
 * NULL indices queries the required count. SIZE_MAX means invalid inventory,
 * invalid input or insufficient capacity; no partial result is written. */
size_t sh_package_scope_find(const sh_package_sources *sources,
    const unsigned char fingerprint[32], const char *relative, size_t *indices, size_t capacity);
/* Only historical carriers may resolve a missing original identity through
 * their recorded normalized tree. All matching old contributors survive.
 * Modern typed uses must call the strict original-tree APIs above. */
size_t sh_package_scope_find_legacy(const sh_package_sources *sources,
    const unsigned char fingerprint[32], const char *relative, size_t *indices, size_t capacity);
int sh_package_scope_composed_keys_legacy(const sh_package_sources *sources,
    const unsigned char (*fingerprints)[32], size_t count,
    sh_package_scope *out, char *error, size_t capacity);
void sh_package_scope_close(sh_package_scope *scope);
#endif
