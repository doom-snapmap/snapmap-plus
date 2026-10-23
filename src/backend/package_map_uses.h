/* Actual typed map occurrences selected against one retained source catalog. */
#ifndef SH_PACKAGE_MAP_USES_H
#define SH_PACKAGE_MAP_USES_H

#include "package_source_graph.h"
#include "package_bundle.h"

typedef struct sh_package_map_uses sh_package_map_uses;
/* Only for incoming documents with no package delivery or saved ownership.
 * Resolve old public custom names against the installed library, never an
 * active map. native returns 1 for a shipped/product identity, 0 for custom,
 * negative on failure. Native names and registration bits remain untouched.
 * If old names cannot distinguish variants, stable source context order picks
 * one; every variant remains independently available in the library. Normal
 * native capture records the selected private identity on the next save.
 * Returns malloc-owned bytes; unrelated string bytes remain exact. */
char *sh_package_map_uses_bind_local(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *library,
    int (*native)(void *, const char *, const char *), void *context,
    size_t *output_length, char *error, size_t capacity);
/* The same local binding, recording in *projected (R-249) each inherited
 * member it inserts into a placed copy's state: a use at "<member>#projected"
 * naming the supplier it bound, in original delivery contexts of bundles. A
 * capture of the loaded document then tells that synthesized value from an
 * authored one of the same spelling. projected is empty when nothing was
 * inserted. */
char *sh_package_map_uses_bind_local_projected(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *library,
    const sh_package_bundle_binding *bundles, size_t bundle_count,
    int (*native)(void *, const char *, const char *), void *context,
    size_t *output_length, sh_package_provenance *projected, char *error, size_t capacity);
/* Verified current gameplay/baseline classification for a NEW reference only:
 * 1 requires a source, 0 remains vanilla, negative is an inspection failure.
 * A same-name authored catalog row alone cannot establish this distinction. */
typedef int (*sh_package_map_use_required)(void *context, const char *family,
    const char *name, char *error, size_t capacity);
/* Native capture only: certify that the unchanged saved binding and every
 * selected context still have their exact retained native resources. Return
 * 1 with a borrowed name, 0 to use portable resolution, or negative on failure.
 * Never install this callback when rebinding an incoming portable document. */
typedef int (*sh_package_map_use_retained)(void *context,
    const sh_package_provenance *saved, size_t use, const char **replacement);

/* Inputs are borrowed until close. saved is an exported occurrence document or
 * historical binding record, never the current catalog masquerading as uses.
 * Each selected tree must have its exact original delivery in bundles. New
 * references select one equivalent current context by stable context bytes;
 * saved choices always win. No authored-name/descriptor-ID fallback occurs. */
sh_package_map_uses *sh_package_map_uses_open(const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    char *error, size_t capacity);
void sh_package_map_uses_close(sh_package_map_uses *selection);

/* Source-reader callbacks. Lookup must never record ownership: parent/class
 * reads can carry their child's inherit site. Observe is for actual final
 * typed values only. Both use the ordinary map binding callback contract. */
/* The keys the saved map recorded at one value site ("<site>#<key>" uses of
 * the key family). Returns how many exist (writing at most capacity), or -1. */
int sh_package_map_uses_saved_keys(void *selection, const sh_package_provenance_site *site,
    uint32_t *keys, size_t capacity);
int sh_package_map_uses_lookup(void *selection, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement);
int sh_package_map_uses_observe(void *selection, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement);

/* Commit only after the whole typed walk succeeds. Output starts zeroed and
 * is replaced on success; failure preserves it. The result has modern actual
 * sites, including sites retaining historical COMPOSED contributor contexts.
 * references contains actual current typed names, including vanilla names.
 * This is observation, not a certificate of complete gameplay dependencies. */
int sh_package_map_uses_finish(sh_package_map_uses *selection,
    sh_package_provenance *uses, sh_package_references *references,
    char *error, size_t capacity);

/* Full-map convenience. The caller supplies immutable source/reflection
 * evidence through reader. Returns malloc-owned rebound bytes and commits
 * outputs only on complete success. Unrelated native bytes remain exact.
 * Deleted/changed sites are pruned; no unobserved saved use is carried forward. */
char *sh_package_map_uses_select(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    size_t *output_length, sh_package_provenance *uses,
    sh_package_references *references, sh_package_source_binding *inspection,
    char *error, size_t capacity);

char *sh_package_map_uses_select_retained(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    sh_package_map_use_retained retained, void *retained_context,
    size_t *output_length, sh_package_provenance *uses,
    sh_package_references *references, sh_package_source_binding *inspection,
    char *error, size_t capacity);
/* One map-local play declaration use (SH_PACKAGE_MAP_LOCAL_FAMILY): the typed
 * site that holds it (owner "entity/N", typed value path) and the generated
 * name of the exact placed occurrence the binder bound it to, spelled for that
 * occurrence's bound inherit, as the output document holds it. */
typedef struct sh_package_map_local_use { char *owner, *reference, *name; } sh_package_map_local_use;
typedef struct sh_package_map_local_uses { sh_package_map_local_use *items; size_t count; } sh_package_map_local_uses;
void sh_package_map_local_uses_free(sh_package_map_local_uses *uses);
/* As above, also returning the map-local play declaration uses the map's
 * entity states bind, one per typed site. They are no resources: references
 * never lists them, and the named occurrence's own inherit carries its package
 * use. */
char *sh_package_map_uses_select_local(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    sh_package_map_use_retained retained, void *retained_context,
    size_t *output_length, sh_package_provenance *uses,
    sh_package_references *references, sh_package_map_local_uses *locals,
    sh_package_source_binding *inspection, char *error, size_t capacity);

#endif
