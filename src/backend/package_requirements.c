/* Apply compiled package requirements with owned native values and readback.
 * Polling and declaration publication run on the engine main thread, outside
 * native parsing. Requirements follow installed resources across map changes;
 * removing their packages restores the values they displaced.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "engine_globals.h"   /* glb_resolve -- the engine load-state word */
#include "engine_cvar_read.h"
#include "package_runtime.h"
#include "package_requirements.h"

/* Pinned Vulkan RVA for audit only. Read the signature-resolved load_state
 * global; this address does not apply to OpenGL.
 */
#define PR_LOAD_STATE_PINNED_RVA 0x6dde198u
#define PR_LOAD_STATE_RUNNING   3

enum {
    PR_STATE_NEW = 0,
    PR_STATE_INSTALLING,
    PR_STATE_WAITING,
    PR_STATE_ARMED,
    PR_STATE_APPLYING,
    PR_STATE_DONE,
    PR_STATE_FAILED
};

typedef void (*pr_buffer_command_fn)(void *cmdsys, const char *text);
typedef void (*pr_execute_buffer_fn)(void *cmdsys);

typedef struct pr_allowed {
    const char *name;
    const char *value;
    int requested;
    void *cvar;
    int owned;
    int original;
} pr_allowed;

/* Deliberately tiny. Expanding this table is a product/security decision. */
static pr_allowed g_allowed[] = {
    { "g_useImageBlackList", "0", 0 },
    { "g_useResourceBlackList", "0", 0 }
};
#define PR_COUNT (sizeof(g_allowed) / sizeof(g_allowed[0]))

static volatile LONG g_state = PR_STATE_NEW;
static const uint8_t *g_module_base;
/* Resolved load-state word, or NULL. Report an unresolved gate once. */
static const uint8_t *g_load_state_at;
static int g_load_state_reported;
static void *g_cmdsys;
static pr_buffer_command_fn g_buffer_command;
static pr_execute_buffer_fn g_execute_buffer;
static const void *g_cvar_system_slot;
/* A failed rollback must be retried before recapturing any displaced values. */
static unsigned int g_recovery_mask;
static int g_recovery_values[PR_COUNT];
static size_t g_requirement_count;
static size_t g_manifest_count;

/* Recapture desired requirements without discarding native value ownership. */
int sh_package_requirements_rearm(const char *data_root, void *execute_command_buffer,
                                  int user_layer_enabled)
{
    if (!g_cmdsys || !g_buffer_command || (!execute_command_buffer && !g_execute_buffer)) {
        backend_log("package-requirements RE-ARM refused: the command system was never captured");
        return 0;
    }
    g_requirement_count = 0;
    g_manifest_count = 0;
    InterlockedExchange(&g_state, PR_STATE_NEW);
    if (!sh_package_requirements_install(data_root, g_module_base, g_cmdsys,
                                         (void *)g_buffer_command,
                                         execute_command_buffer ? execute_command_buffer : (void *)g_execute_buffer,
                                         user_layer_enabled))
        return 0;
    return sh_package_requirements_apply_now(execute_command_buffer);
}

#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
static volatile int *g_test_load_state;
static const void *g_test_cvar_slot;
#endif

static int pr_fail(const char *reason)
{
    char line[512];
    size_t i;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++)
        g_allowed[i].requested = 0;
    g_requirement_count = 0;
    g_manifest_count = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements REFUSED: %s; zero settings admitted",
                reason ? reason : "unknown failure");
    backend_log(line);
    InterlockedExchange(&g_state, PR_STATE_FAILED);
    return 0;
}

static int pr_admit(const char *kind, const char *name, const char *value)
{
    size_t i;
    if (strcmp(kind, "cvar") != 0) return 0;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++) {
        if (strcmp(name, g_allowed[i].name) != 0) continue;
        if (strcmp(value, g_allowed[i].value) != 0) return 0;
        if (!g_allowed[i].requested) {
            g_allowed[i].requested = 1;
            g_requirement_count++;
        }
        /* Combine identical requests; reject conflicting values before queueing. */
        return 1;
    }
    return 0;
}

static int pr_capture(void)
{
    size_t length = 0, i;
    char *json = sh_package_runtime_policy("requirements", &length);
    sh_json_object root = {0}, cvars = {0};
    const char *raw;
    int ok = 0;
    if (!json || !sh_json_parse_object(json, length, 4, &root)) goto done;
    raw = sh_json_object_get(&root, "cvars");
    if (raw && !sh_json_parse_object(raw, strlen(raw), 2, &cvars)) goto done;
    for (i = 0; i < cvars.count; i++)
        if (!pr_admit("cvar", cvars.members[i].key, cvars.members[i].value_json)) goto done;
    g_manifest_count = cvars.count ? 1u : 0u;
    ok = 1;
done:
    sh_json_object_free(&root); sh_json_object_free(&cvars); free(json);
    return ok ? 1 : pr_fail("compiled requirements are unavailable or unsupported");
}

static int pr_read_load_state(int *value)
{
#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
    if (g_test_load_state) {
        *value = *g_test_load_state;
        return 1;
    }
#endif
    if (!g_module_base) return 0;
    /* Resolve load_state through the globals table. On failure, keep the
     * RUNNING gate closed rather than reading a build-specific address.
     */
    if (!g_load_state_at) {
        g_load_state_at = (const uint8_t *)glb_resolve(g_module_base, "load_state", NULL);
        if (!g_load_state_at) {
            if (!g_load_state_reported) {
                g_load_state_reported = 1;
                backend_log("package-requirements: the engine load-state word did not resolve on this "
                            "build -- the RUNNING gate stays shut and no requirement is applied");
            }
            return 0;
        }
    }
    __try {
        *value = *(const volatile int *)g_load_state_at;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_package_requirements_install(const char *data_root,
                                    const uint8_t *module_base,
                                    void *cmdsys,
                                    void *buffer_command,
                                    void *execute_command_buffer,
                                    int user_layer_enabled)
{
    char line[256];
    size_t i;
    /* Retain command pointers even for an empty launch snapshot; later
     * package installs may need them.
     */
    if (module_base) g_module_base = module_base;
    if (cmdsys) g_cmdsys = cmdsys;
    if (buffer_command) g_buffer_command = (pr_buffer_command_fn)buffer_command;
    if (execute_command_buffer) g_execute_buffer = (pr_execute_buffer_fn)execute_command_buffer;
    if (InterlockedCompareExchange(&g_state, PR_STATE_INSTALLING, PR_STATE_NEW) != PR_STATE_NEW)
        return 0;
    /* A rearm is a new capture. Previously admitted rows must not suppress
     * the count or survive after their package is no longer selected. */
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++)
        g_allowed[i].requested = 0;
    g_requirement_count = 0;
    g_manifest_count = 0;
    (void)data_root;
    if (!user_layer_enabled) {
        backend_log("package-requirements disabled for this launch with the user override layer");
    } else {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        int available = compiled != NULL;
        sh_package_runtime_release();
        if (!available) {
            /* Bootstrap binds services before the main-thread compiler pass.
             * Rearm captures policy once that complete snapshot exists. */
            InterlockedExchange(&g_state, PR_STATE_WAITING);
            return 1;
        }
        if (!pr_capture()) return 0;
    }
    if (!g_requirement_count && !g_recovery_mask) {
        for (i = 0; i < PR_COUNT; i++) if (g_allowed[i].owned) break;
        if (i == PR_COUNT) {
            InterlockedExchange(&g_state, PR_STATE_DONE);
            return 1;
        }
    }
#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
    if ((!module_base && !g_test_load_state) || !cmdsys || !buffer_command || !g_execute_buffer)
#else
    if (!module_base || !cmdsys || !buffer_command || !g_execute_buffer)
#endif
        return pr_fail("command-system or load-state dependency missing");

    g_module_base = module_base;
    g_cmdsys = cmdsys;
    g_buffer_command = (pr_buffer_command_fn)buffer_command;
    InterlockedExchange(&g_state, PR_STATE_ARMED);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements captured: %zu policy section(s), %zu safe cvar(s); waiting for publication",
                g_manifest_count, g_requirement_count);
    backend_log(line);
    /* Installation can run on the bootstrap worker. Only the main-thread
     * publication boundary or frame poll may execute native commands. */
    return 1;
}

static int pr_read(size_t index, int *value)
{
    const void *slot = g_cvar_system_slot;
#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
    if (g_test_cvar_slot) slot = g_test_cvar_slot;
#endif
    if (!slot && g_module_base)
        slot = g_cvar_system_slot = (const void *)glb_resolve(g_module_base, "cvar_system_slot", NULL);
    if (!g_allowed[index].cvar)
        g_allowed[index].cvar = sh_engine_cvar_find(slot, g_allowed[index].name);
    return sh_engine_cvar_read_int(g_allowed[index].cvar, value);
}

/* Build both apply and rollback batches from already-read integer values.
 * Authored console text never reaches this path. */
static int pr_write(unsigned int mask, const int *values)
{
    char command[192] = "";
    size_t used = 0, i;
    if (!mask) return 1;
    if (!g_cmdsys || !g_buffer_command || !g_execute_buffer) return 0;
    for (i = 0; i < PR_COUNT; i++) {
        int n;
        if (!(mask & (1u << i))) continue;
        n = _snprintf_s(command + used, sizeof(command) - used, _TRUNCATE,
                        "%s %d\n", g_allowed[i].name, values[i]);
        if (n < 0) return 0;
        used += (size_t)n;
    }
    __try {
        g_buffer_command(g_cmdsys, command);
        g_execute_buffer(g_cmdsys);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    for (i = 0; i < PR_COUNT; i++) {
        int current;
        if ((mask & (1u << i)) && (!pr_read(i, &current) || current != values[i])) return 0;
    }
    return 1;
}

static int pr_recover(void)
{
    if (!g_recovery_mask) return 1;
    if (!pr_write(g_recovery_mask, g_recovery_values)) return 0;
    g_recovery_mask = 0;
    backend_log("package-requirements: previous native values restored after failed application");
    return 1;
}

static void pr_apply(const char *when)
{
    int before[PR_COUNT] = {0}, desired[PR_COUNT] = {0};
    int original[PR_COUNT] = {0};
    unsigned int changed = 0;
    size_t i;
    char line[224];
    if (InterlockedCompareExchange(&g_state, PR_STATE_APPLYING, PR_STATE_ARMED) != PR_STATE_ARMED)
        return;
    if (!pr_recover()) {
        pr_fail("prior native values could not be restored; requirement changes remain refused");
        return;
    }
    /* Preflight every affected cvar before changing any of them. Preserve
     * baseline ownership across rescans, including identical requests. */
    for (i = 0; i < PR_COUNT; i++) {
        pr_allowed *row = &g_allowed[i];
        if (!row->requested && !row->owned) continue;
        if (!pr_read(i, &before[i])) {
            pr_fail("an affected native cvar could not be read; no values changed");
            return;
        }
        original[i] = row->owned ? row->original : before[i];
        desired[i] = row->requested ? 0 : (before[i] == 0 ? original[i] : before[i]);
        if (before[i] != desired[i]) changed |= 1u << i;
    }
    if (changed) {
        memcpy(g_recovery_values, before, sizeof(before));
        g_recovery_mask = changed;
        if (!pr_write(changed, desired)) {
            int restored = pr_recover();
            pr_fail(restored ? "native requirement application failed; previous values restored" :
                              "native requirement application and restoration failed; retry must restore first");
            return;
        }
        g_recovery_mask = 0;
    }
    for (i = 0; i < PR_COUNT; i++) {
        g_allowed[i].owned = g_allowed[i].requested;
        g_allowed[i].original = g_allowed[i].requested ? original[i] : 0;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements applied: %zu safe cvar(s), native values verified %s",
                g_requirement_count, when);
    backend_log(line);
    InterlockedExchange(&g_state, PR_STATE_DONE);
}

/* The engine can instantiate prepared editor resources during any map load.
 * Keep their gates open until the installed requirement set changes; map
 * selection controls delivery and gameplay policy, not native residency. */
void sh_package_requirements_poll(void)
{
    int load_state = -1;
    if (InterlockedCompareExchange(&g_state, PR_STATE_ARMED, PR_STATE_ARMED) != PR_STATE_ARMED)
        return;
    if (!pr_read_load_state(&load_state) || load_state != PR_LOAD_STATE_RUNNING)
        return;
    pr_apply("at load-state RUNNING");
}

/* Apply at the decl server's quiescent pre-promotion boundary, after startup
 * parsing. Blacklist gates are checked before type parsing, so they must be
 * live before publication. A NULL callback reuses the captured native drain.
 */
int sh_package_requirements_apply_now(void *execute_command_buffer)
{
    LONG state = InterlockedCompareExchange(&g_state, PR_STATE_ARMED, PR_STATE_ARMED);
    if (execute_command_buffer) g_execute_buffer = (pr_execute_buffer_fn)execute_command_buffer;
    if (state == PR_STATE_DONE) return 1;
    if (state != PR_STATE_ARMED) return 0;
    pr_apply("at declaration publication");
    return InterlockedCompareExchange(&g_state, PR_STATE_DONE, PR_STATE_DONE) == PR_STATE_DONE;
}

#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
void sh_package_requirements_test_reset(void)
{
    size_t i;
    for (i = 0; i < PR_COUNT; i++) {
        g_allowed[i].requested = 0;
        g_allowed[i].cvar = NULL;
        g_allowed[i].owned = 0;
        g_allowed[i].original = 0;
    }
    g_module_base = NULL;
    g_load_state_at = NULL;
    g_load_state_reported = 0;
    g_cmdsys = NULL;
    g_buffer_command = NULL;
    g_execute_buffer = NULL;
    g_cvar_system_slot = NULL;
    g_test_cvar_slot = NULL;
    g_recovery_mask = 0;
    memset(g_recovery_values, 0, sizeof(g_recovery_values));
    g_test_load_state = NULL;
    g_requirement_count = 0;
    g_manifest_count = 0;
    InterlockedExchange(&g_state, PR_STATE_NEW);
}

void sh_package_requirements_test_set_load_state(volatile int *state)
{
    g_test_load_state = state;
}

void sh_package_requirements_test_set_cvar_slot(const void *slot)
{
    g_test_cvar_slot = slot;
}

size_t sh_package_requirements_test_count(void)
{
    return g_requirement_count;
}
#endif
