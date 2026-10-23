#include "package_sound_native.h"
#include "package_runtime.h"
#include "hook.h"
#include "backend_log.h"
#include <string.h>

typedef void (*psn_initialize)(void *sound);
static psn_initialize g_psn_original;
static void *g_psn_trampoline;
static volatile LONG g_psn_ready;

static void psn_initialize_bound(void *sound)
{
    const char *name;
    uint32_t event;
    /* Both renderers' common helper writes the event after name conversion.
     * Preserve all native initialization before applying the bound association.
     * Native exceptions retain their normal caller semantics. */
    g_psn_original(sound);
    name = *(const char **)((unsigned char *)sound + 8);
    if (sh_package_runtime_sound_event(name, &event))
        *(uint32_t *)((unsigned char *)sound + 0xac) = event;
}

int sh_package_sound_native_ready(void)
{
    return InterlockedCompareExchange(&g_psn_ready, 0, 0) != 0;
}

int sh_package_sound_native_install(const sig_result *results, size_t count)
{
    /* Complete position-independent instructions, ending before stack
     * allocation, verified at this initializer in both renderer images. */
    static const unsigned char prologue[] = {
        0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x48,0xfe,0xff,0xff
    };
    uintptr_t target = 0;
    if (sh_package_sound_native_ready()) return 1;
    if (g_psn_trampoline) {
        if (!hook_unpatch(g_psn_trampoline)) return 0;
        g_psn_trampoline = NULL; g_psn_original = NULL;
    }
    for (size_t i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, "SoundEventInitialize") && results[i].status == SIG_OK)
            target = results[i].addr;
    if (!target) return 0;
    __try {
        if (memcmp((const void *)target, prologue, sizeof(prologue))) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_psn_trampoline = hook_prepare((void *)target, psn_initialize_bound, sizeof(prologue));
    if (!g_psn_trampoline) return 0;
    g_psn_original = (psn_initialize)g_psn_trampoline;
    if (hook_commit(g_psn_trampoline) != B2_PATCH_OK) {
        if (hook_unpatch(g_psn_trampoline)) { g_psn_trampoline = NULL; g_psn_original = NULL; }
        backend_log("[sound] native event binding hook installation failed");
        return 0;
    }
    InterlockedExchange(&g_psn_ready, 1);
    backend_log("[sound] native event binding hook installed");
    return 1;
}
