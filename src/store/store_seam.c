/* store_seam.c -- what the store tool links in place of the game's serving
 * layer (modserve.c, modserve_engine.c) and the builder entry with the game's
 * resource reader (modcmd.c). The tool serves nothing: no bundle is active,
 * nothing is indexed, and a rescan has nothing to rebuild. The legacy
 * converter calls the builder entry only for a map's carrier, which the tool
 * never converts. */
#include <stddef.h>
#include <string.h>

#include "modserve.h"
#include "modbundle.h"

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    (void)n;
    return 0;
}

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count, sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

int sh_modserve_bundle_state(const char *bundle_hash, char *cause, size_t cause_cap)
{
    (void)bundle_hash;
    if (cause && cause_cap) cause[0] = 0;
    return 0;
}

int sh_modserve_active(void) { return 0; }

int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out)
{
    (void)bundle_hash; (void)id;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}

int sh_modserve_index_rescan(void) { return 0; }

int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    (void)folder;
    if (first_cause && n) strncpy_s(first_cause, n, "the store tool does not convert a map's carriers", _TRUNCATE);
    return 0;
}
