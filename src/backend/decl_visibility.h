/* decl_visibility.h -- keep published decl identities resolvable across a map load. */
#ifndef BACKEND_DECL_VISIBILITY_H
#define BACKEND_DECL_VISIBILITY_H

#include <stddef.h>
#include <stdint.h>

/* Extend a negative native resource-existence probe only for exact identities
 * in the published decl table. Below map-load state 2, lookup uses the source
 * catalog; later stages probe generated/decls paths before loading through
 * the provider.
 *
 * Call on the main thread after successful registration. Installation
 * requires a clean prologue match equal to the live vtable slot. Diagnostic
 * probe paths do not affect admission. Returns 1 when installed; refusal
 * leaves registration intact but may prevent later map-load lookups of new
 * identities.
 */
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
