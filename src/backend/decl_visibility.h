/* decl_visibility.h -- keep published decl identities resolvable across a map load. */
#ifndef BACKEND_DECL_VISIBILITY_H
#define BACKEND_DECL_VISIBILITY_H

#include <stddef.h>
#include <stdint.h>

/* Answer the engine's decl-resource existence probe for exactly the identities
 * the dynamic decl server published, and only after the engine's own answer is
 * "no".
 *
 * Native source registration is consulted only below map-load state 2. From
 * state 2 upward -- which covers every gameplay map load -- the engine decides
 * whether an absent decl identity exists by asking the decl-resource manager
 * instead, so a lookup with makeDefault=0 refuses identities this process
 * registered and materialized minutes earlier. Answering that one probe lets
 * the engine create and load the decl through the file-system open slot this
 * product already provides.
 *
 * The slot must already hold the pinned method for the supported build, or
 * nothing is installed: a different method would take different arguments and
 * forwarding the wrong shape would corrupt the engine's stack. The two probe
 * paths are retained for diagnostics only. Call this on the engine main thread,
 * after registration has succeeded and before the first gameplay map load.
 *
 * Returns 1 when the hook is installed, 0 on any refusal. Refusal is never
 * fatal to registration: it only means new identities stay editor-only. */
int sh_decl_visibility_install(const uint8_t *module_base,
                               const char *existing_probe_path,
                               const char *absent_probe_path);

/* Restore the original method. Idempotent; returns 1 when a hook was removed. */
int sh_decl_visibility_uninstall(void);

#ifdef SH_DECL_VISIBILITY_TESTING
/* Map an engine probe path to the published-table key, without consulting the
 * table. Returns 1 and fills `key` when the path is inside the engine's decl
 * directory and fits; 0 otherwise. */
int sh_decl_visibility_test_probe_key(const char *path, char *key, size_t key_size);
#endif

#endif /* BACKEND_DECL_VISIBILITY_H */
