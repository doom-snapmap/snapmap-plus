/* palette_refresh.h -- rebuild of the live SnapMap entity palette after registration. */
#ifndef BACKEND_PALETTE_REFRESH_H
#define BACKEND_PALETTE_REFRESH_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Resolve and arm the native palette builder. The builder is not called here:
 * the complete-registration command invokes this service on DOOM's main thread
 * after its final native scan succeeds. Install happens once per process. */
int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base);

/* Rebuild the entity palette after each successful
 * registration/materialization pass. The map validator and by-name consumers
 * use this derived catalog, so runtime additions need another rebuild.
 * Returns 1 on completion. Refusal or exception is terminal for this process.
 */
int sh_palette_refresh_after_decl_registration(void);

#ifdef SH_PALETTE_REFRESH_TESTING
enum sh_palette_refresh_test_state {
    SH_PALETTE_REFRESH_TEST_IDLE = 0,
    SH_PALETTE_REFRESH_TEST_PENDING,
    SH_PALETTE_REFRESH_TEST_APPLIED,
    SH_PALETTE_REFRESH_TEST_REFUSED
};

void sh_palette_refresh_test_reset(void);
void sh_palette_refresh_test_bind(const uint8_t *module_base, void *builder);
int sh_palette_refresh_test_state(void);
int sh_palette_refresh_test_call_count(void);
#endif

#endif /* BACKEND_PALETTE_REFRESH_H */
