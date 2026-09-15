#include <stdio.h>
#include <string.h>
#include "../src/backend/startup_bindings.h"

static const sig_entry database[] = {
    {"late-a", "A", 10}, {"observe-a", "B", 20}, {"late-b", "C", 30},
    {"observe-b", "D", 40}, {"late-c", "E", 50}
};
static sig_status available[5];
static unsigned calls[5], ready_calls;
static int failures, ready_success = 1;
static const uint8_t *module = (const uint8_t *)0x12340000;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

sig_status sig_resolve_one(const uint8_t *base, const sig_entry *entry, sig_result *out)
{
    size_t index = (size_t)(entry - database);
    CHECK(base == module && index < 5);
    calls[index]++;
    out->name = entry->name; out->status = available[index];
    out->rva = out->status == SIG_OK || out->status == SIG_OK_HOOKED ? 1000 + (uint32_t)index : 0;
    out->addr = out->rva ? (uintptr_t)base + out->rva : 0;
    return out->status;
}
static int priority(const char *name) { return !strncmp(name, "observe-", 8); }
static int ready(const sig_result *results, size_t count)
{
    CHECK(count == 5 && !calls[0] && !calls[2] && !calls[4]);
    CHECK(results[1].addr == (uintptr_t)module + 1001);
    CHECK(results[3].addr == (uintptr_t)module + 1003);
    CHECK(results[0].name == database[0].name && results[0].status == SIG_NOT_FOUND);
    ready_calls++;
    /* Owned prologues no longer match their original signatures after either
     * a successful install or a partially installed transparent call-through. */
    available[1] = available[3] = SIG_NOT_FOUND;
    return ready_success;
}
static void reset(sh_startup_bindings *state, sig_result *results)
{
    size_t i;
    memset(calls, 0, sizeof(calls)); ready_calls = 0; ready_success = 1;
    for (i = 0; i < 5; i++) available[i] = SIG_OK;
    memset(results, 0xcd, 5 * sizeof(*results));
    sh_startup_bindings_init(state, database, results, 5);
}
int main(void)
{
    sh_startup_bindings state;
    sig_result results[5];
    reset(&state, results);
    available[3] = SIG_NOT_FOUND;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 0) == 1);
    CHECK(!ready_calls && !calls[0] && !calls[2] && !calls[4]);
    CHECK(!results[0].addr && results[0].status == SIG_NOT_FOUND);
    available[3] = SIG_OK; available[4] = SIG_NOT_FOUND;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 0) == 4);
    CHECK(ready_calls == 1 && calls[1] == 2 && calls[3] == 2);
    CHECK(results[1].status == SIG_OK && results[1].rva == 1001);
    CHECK(results[3].status == SIG_OK && results[3].rva == 1003);
    available[4] = SIG_OK;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 0) == 5);
    CHECK(ready_calls == 1 && calls[1] == 2 && calls[3] == 2);
    CHECK(calls[0] == 2 && calls[2] == 2 && calls[4] == 2);

    reset(&state, results); ready_success = 0;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 0) == 5);
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 1) == 5);
    CHECK(ready_calls == 1 && calls[1] == 1 && calls[3] == 1);

    /* A final pass still supplies unrelated features when an observer's
     * dependencies are unavailable or ambiguous. Never manufacture readiness. */
    reset(&state, results); available[1] = SIG_AMBIGUOUS; available[3] = SIG_NOT_FOUND;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 1) == 3);
    CHECK(!ready_calls && !state.priority_finished);
    CHECK(results[1].status == SIG_AMBIGUOUS && !results[1].addr);
    CHECK(results[3].status == SIG_NOT_FOUND && !results[3].addr);
    CHECK(results[0].status == SIG_OK && results[2].status == SIG_OK && results[4].status == SIG_OK);

    /* Externally hooked entries retain their distinct status. The observer
     * can decline them; they are never relabeled as a clean scan. */
    reset(&state, results); available[1] = SIG_OK_HOOKED;
    CHECK(sh_startup_bindings_step(&state, module, priority, ready, 0) == 5);
    CHECK(results[1].status == SIG_OK_HOOKED && ready_calls == 1);
    CHECK(results[1].rva == 1001); /* Shifted image address, not known_rva20. */

    if (failures) return 1;
    puts("early observer binding and pre-install snapshot checks passed"); return 0;
}
