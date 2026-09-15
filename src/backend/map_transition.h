/* A package handoff inside an already requested native level allocation. */
#ifndef BACKEND_MAP_TRANSITION_H
#define BACKEND_MAP_TRANSITION_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

typedef struct sh_map_transition_callbacks {
    /* Positive: this exact native allocation belongs to a retained request.
     * Zero: unchanged native path. Negative: cancel the owned native load. */
    int (*pending)(void *resource_manager, const void *level_parameters);
    /* Called after the native unload and its purge have completed, before the
     * allocation creates new resources. Zero cancels through the native
     * map-change owner; it never reaches the stock fatal allocation branch. */
    int (*activate)(void);
    /* Finalize the pending whole-package installation only after native map
     * finalization succeeds. Refusal uses the same owned cancellation path. */
    int (*commit)(void);
    /* Exactly once after native finalization or owned cancellation completes.
     * Native faults still propagate after ownership is notified of failure. */
    void (*finished)(int loaded);
} sh_map_transition_callbacks;

int sh_map_transition_bind(const sig_result *results, size_t count, const uint8_t *base);
int sh_map_transition_install(const sig_result *results, size_t count,
    const uint8_t *base, const sh_map_transition_callbacks *callbacks);
/* True only while the main-thread activation callback is running. */
int sh_map_transition_at_boundary(void);
int sh_map_transition_ready(void);
#endif
