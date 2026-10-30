/* mod_seam_stub.c -- link stand-ins for the serving layer (modserve.c and
 * modserve_engine.c) and the bundle builder entry (modcmd.c), for tests that
 * link the map manifest or the legacy converter without them.
 *
 * The serving index reports no bundle as indexed, so every conversion asks
 * for a rescan; g_mod_stub_rescans counts the requests. Define
 * SH_MOD_STUB_NO_BUILDER when the test supplies its own
 * modbundle_build_folder. */
#include <stddef.h>
#include <string.h>

#include "../src/backend/modserve.h"
#include "../src/backend/modbundle.h"

int g_mod_stub_rescans;
int g_mod_stub_activations;

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    if (n > 0) g_mod_stub_activations++;
    return 0;
}

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
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

int sh_modserve_index_rescan(void)
{
    g_mod_stub_rescans++;
    return 0;
}

#ifndef SH_MOD_STUB_NO_BUILDER
int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    (void)folder;
    if (first_cause && n) strncpy_s(first_cause, n, "the bundle builder is not linked into this test", _TRUNCATE);
    return 0;
}
#endif
