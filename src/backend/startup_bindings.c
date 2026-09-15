#include "startup_bindings.h"

static int sb_resolved(const sig_result *result)
{ return result->status == SIG_OK || result->status == SIG_OK_HOOKED; }

void sh_startup_bindings_init(sh_startup_bindings *state, const sig_entry *database,
                              sig_result *results, size_t count)
{
    size_t i;
    state->database = database; state->results = results;
    state->count = count; state->priority_finished = 0;
    for (i = 0; i < count; i++) {
        results[i].name = database[i].name;
        results[i].status = SIG_NOT_FOUND;
        results[i].addr = 0; results[i].rva = 0;
    }
}

size_t sh_startup_bindings_step(sh_startup_bindings *state, const uint8_t *module_base,
                                sh_startup_priority_fn priority, sh_startup_ready_fn ready,
                                int final_pass)
{
    size_t i, resolved = 0;
    if (!state->priority_finished) {
        int complete = 1, any = 0;
        for (i = 0; i < state->count; i++) if (priority(state->database[i].name)) {
            any = 1;
            sig_resolve_one(module_base, &state->database[i], &state->results[i]);
            if (!sb_resolved(&state->results[i])) complete = 0;
        }
        if (complete) {
            /* The callback may have a partial installation on failure. Keep
             * its pre-install identities in either case and never retry it. */
            state->priority_finished = 1;
            if (any) ready(state->results, state->count);
        } else if (!final_pass) {
            for (i = 0; i < state->count; i++) resolved += sb_resolved(&state->results[i]);
            return resolved;
        }
    }
    for (i = 0; i < state->count; i++) {
        if (!priority(state->database[i].name))
            sig_resolve_one(module_base, &state->database[i], &state->results[i]);
        resolved += sb_resolved(&state->results[i]);
    }
    return resolved;
}
