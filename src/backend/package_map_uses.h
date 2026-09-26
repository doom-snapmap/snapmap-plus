/* Actual typed map occurrences selected against one retained source catalog. */
#ifndef SH_PACKAGE_MAP_USES_H
#define SH_PACKAGE_MAP_USES_H

#include "package_source_graph.h"
#include "package_bundle.h"

typedef struct sh_package_map_uses sh_package_map_uses;
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

#endif
