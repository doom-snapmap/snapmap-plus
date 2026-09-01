/* palette_refresh.h -- one-shot rebuild of the live SnapMap entity palette. */
#ifndef BACKEND_PALETTE_REFRESH_H
#define BACKEND_PALETTE_REFRESH_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Resolve and arm the native palette builder. The builder is not called here:
 * the complete-registration command invokes the one-shot service on DOOM's
 * main thread after its final native scan succeeds. */
int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base);

/* Consume the one-shot state synchronously after complete native registration
 * and new snapEditorEntityDef materialization. Returns 1 only when the native
 * rebuild completed; every refusal or exception is terminal and returns 0. */
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
