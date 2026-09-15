#ifndef SH_STARTUP_BINDINGS_H
#define SH_STARTUP_BINDINGS_H
#include "signatures.h"

/* One bootstrap owns this pre-install snapshot. Early observers may patch their
 * verified entries, so later scans must not reinterpret those owned prologues. */
typedef struct sh_startup_bindings {
    const sig_entry *database;
    sig_result *results;
    size_t count;
    int priority_finished;
} sh_startup_bindings;

typedef int (*sh_startup_priority_fn)(const char *name);
typedef int (*sh_startup_ready_fn)(const sig_result *results, size_t count);
void sh_startup_bindings_init(sh_startup_bindings *state, const sig_entry *database,
                              sig_result *results, size_t count);
/* Resolve priority entries first. Invoke ready once when all are present,
 * before scanning the rest. A final pass resolves all available non-priority
 * entries even when an observer is unavailable; timeout remains caller-owned. */
size_t sh_startup_bindings_step(sh_startup_bindings *state, const uint8_t *module_base,
                                sh_startup_priority_fn priority, sh_startup_ready_fn ready,
                                int final_pass);
#endif
