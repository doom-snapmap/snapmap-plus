/* Select retained authored deliveries referenced by map JSON.
 * map_package owns the wire format and install/load path.
 */
#ifndef SNAPMAP_PLUS_MAP_EMBED_H
#define SNAPMAP_PLUS_MAP_EMBED_H

#include <windows.h>
#include <stddef.h>

#include "map_package.h"
#include "package_usage.h"
#include "package_provenance.h"
#include "package_bundle.h"

typedef struct sh_mpkg_use_requirements {
    const sh_package_compilation *compiled;
    const sh_resource_catalog *catalog;
} sh_mpkg_use_requirements;
/* sh_package_map_use_required callback. Uses verified compiled gameplay owners,
 * derived-path and baseline classification, never authored/native name equality
 * as vanilla proof. Borrow the same provider snapshot as the typed selector. */
int sh_mpkg_new_use_required(void *requirements, const char *family, const char *name,
    char *error, size_t capacity);

/* Main-thread preparation, before any compiler snapshot is acquired. Caller
 * supplies an empty result and frees it after use. Missing reader coverage is
 * recorded separately from failure; no entity is constructed or modified. */
int sh_mpkg_prepare_map(const char *json, size_t length, sh_package_references *references,
                        char *error, size_t error_capacity);

/* Pack every authored file and directory into deterministic ZIP delivery.
 * Compression affects transport only. Source bytes are verified before use.
 * Returns a process-heap buffer, or NULL with an error. */
unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len,
                                char *err, size_t err_cap);

/* Resolve typed saved-map roots and known dependencies through compiled
 * ownership, including replacements. SIZE_MAX indicates a failed inventory
 * or traversal; missing observations are separately reported in diagnostics.
 * Initialize *out to NULL. Repeated calls replace it; used_free after use. */
#define SH_MPKG_COMPONENT_CAP 512
typedef struct sh_mpkg_used {
    char id[SH_MPKG_ID_CAP];   /* complete-tree transport identity, not descriptor id */
    char name[SH_PACKAGE_DISPLAY_CAP]; /* authored label for save diagnostics */
    sh_package_bundle *bundle; /* retained exact complete original tree */
    /* D4: bundle is a nested component's delivery built from the enclosing
     * tree source_tree (sh_package_bundle_component). The saved record's
     * contexts (source_tree, component) are rewritten to bundle's own tree so
     * a reader resolves them against the carried delivery. */
    int nested;
    unsigned char source_tree[32];
    char component[SH_MPKG_COMPONENT_CAP];
} sh_mpkg_used;
/* Returned arrays have a null-bundle sentinel. Release all retained sources. */
void sh_mpkg_used_free(sh_mpkg_used *used);
/* The save path's rule for component deliveries (D4): every context of record
 * built from (source_tree, component) of a nested delivery is rewritten to that
 * delivery's own tree, whole, so a reader resolves the saved uses against the
 * carried delivery. */
void sh_mpkg_used_retarget_record(sh_package_provenance *record, const sh_mpkg_used *used, size_t count);
unsigned char *sh_mpkg_pack_used(const sh_mpkg_used *used, size_t *out_len,
                               char *error, size_t capacity);

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used **out, char *error, size_t error_capacity);
/* Select delivery and source bindings from one acquired provider snapshot.
 * Legacy-only saves keep no binding envelope; mixed maps retain the complete
 * historical context alongside their isolated contexts. Failure clears both
 * outputs. Initialize bindings to zero and free with provenance_free. */
size_t sh_mpkg_used_packages_with_bindings(const char *json, size_t len, const char *data_root,
    sh_mpkg_used **out, sh_package_provenance *bindings, char *error, size_t error_capacity);

/* Explicit occurrence route. Caller holds one stable current compiler/catalog
 * snapshot and supplies the typed selection's actual map bytes/references,
 * selected USES (or caller-selected historical bindings), and exact retained
 * original deliveries. Only selected context trees are packed; equivalent
 * compiled resource owners are not a delivery selection. No runtime acquire,
 * native call or editable package scan occurs here.
 *
 * Resources receives the current compiled map's known dependency paths and
 * explicit incomplete flag. Supplied compiled resources are probed; vanilla
 * paths and late/custom dependency completeness still require the caller's
 * availability/coverage proof. This preserves historical partial observation
 * without presenting it as installation success. Initialize outputs to zero;
 * failure clears them and returns SIZE_MAX. */
/* D4: the enclosing files a nested component's closure resolves to -- every
 * source file of a resolved resource that belongs to the same compiler owner
 * but lies outside the component, unique, in closure order. Release the list
 * with sh_mpkg_strings_free. Returns 0 only on allocation failure. */
int sh_mpkg_component_enclosing_files(const sh_package_compilation *compiled, size_t owner, const char *component,
    const sh_package_references *resources, char ***out, size_t *count);
void sh_mpkg_strings_free(char **items, size_t count);

size_t sh_mpkg_used_packages_for_uses(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const char *json, size_t length,
    const sh_package_references *references, const sh_package_provenance *uses,
    const sh_package_bundle_binding *bundles, size_t bundle_count,
    sh_mpkg_used **out, sh_package_references *resources, char *error, size_t capacity);

#endif /* SNAPMAP_PLUS_MAP_EMBED_H */
