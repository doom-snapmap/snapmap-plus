/* mod_seam_stub.c -- link stand-ins for the serving layer (modserve.c and
 * modserve_engine.c) and the bundle builder entry (modcmd.c), for tests that
 * link the map manifest or the legacy converter without them.
 *
 * The serving index reports no bundle as indexed, so every conversion asks
 * for a rescan; g_mod_stub_rescans counts the requests. Define
 * SH_MOD_STUB_NO_BUILDER when the test supplies its own
 * modbundle_build_folder. The rawmap ZIP export (modexport.c) is not linked:
 * a .zip path is recognised and refused. */
#include <stddef.h>
#include <string.h>

#include "../src/backend/modserve.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/modexport.h"

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

/* The build that serves a row: none installed (the asset rule's activation). */
int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out)
{
    (void)bundle_hash; (void)id;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}

int sh_modserve_index_rescan(void)
{
    g_mod_stub_rescans++;
    return 0;
}

int modexport_is_zip_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n > 4 && _stricmp(path + n - 4, ".zip") == 0;
}

unsigned long long modexport_write_text(const char *data, size_t len, const char *path_acp)
{
    (void)data; (void)len; (void)path_acp;
    return 0;
}

/* The rawmap layer's companion export: g_mod_stub_export_result is what the
 * write returns (0 refuses with a fixed cause); the calls and the last path
 * are recorded. */
unsigned long long g_mod_stub_export_result;
int g_mod_stub_export_calls;
char g_mod_stub_export_path[520];

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path_acp,
                                               unsigned *bundles, char *cause, size_t cause_cap)
{
    (void)data; (void)len;
    g_mod_stub_export_calls++;
    strncpy_s(g_mod_stub_export_path, sizeof g_mod_stub_export_path, path_acp ? path_acp : "", _TRUNCATE);
    if (bundles) *bundles = 0;
    if (g_mod_stub_export_result) return g_mod_stub_export_result;
    if (cause && cause_cap) strncpy_s(cause, cause_cap, "the rawmap export is not linked into this test", _TRUNCATE);
    return 0;
}

char *modexport_read_text(const char *path_acp, size_t *out_len)
{
    (void)path_acp;
    if (out_len) *out_len = 0;
    return NULL;
}

int modexport_validate_source(const char *path_acp, char *out_msg, int msg_capacity)
{
    (void)path_acp;
    if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, "the rawmap export is not linked into this test", _TRUNCATE);
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
