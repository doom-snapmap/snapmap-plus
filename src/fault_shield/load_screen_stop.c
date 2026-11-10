/* Stop the loading-screen thread at CancelMapChange entry (load_screen_stop.h).
 *
 * Build 13954591, both renderers (snaphak-re campaign-interactables-triage F3b,
 * decompiled from logs/shared-ghidra/triage-F3b-*):
 *   - idLoadScreen::LevelLoadCompleted (VK 0x141676770 / GL 0x1416774B0) sets the
 *     thread's stop byte +0x4B1, waits for it (idSysThread::WaitForThread on its
 *     manual-reset done event, so a repeat call does not block), syncs the renderer
 *     (vtable +0xF0, +0xB8) and frees the load GUI with null checks;
 *   - the thread's Run (VK 0x1416772D0) sets +0x4B2, draws until +0x4B1, then clears both;
 *   - CancelMapChange (VK 0x141586720 / GL 0x1415877B0) reaches the renderer sync
 *     (common vtable +0x60) and the menu load, whose own LevelLoadCompleted comes
 *     only after that sync, with the old thread still drawing.
 * Engine calls stay outside every __try so the engine's C++ exceptions pass through. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "load_screen_stop.h"
#include "fault_record.h"
#include "hook.h"
#include "../backend/engine_globals.h"

#define LOAD_SCREEN_STOP_BYTE    0x4B1
#define LOAD_SCREEN_RUNNING_BYTE 0x4B2

typedef void (*cancel_map_change_fn)(void *common, uint8_t arg);
typedef void (*level_load_completed_fn)(void *load_screen);

static cancel_map_change_fn    g_orig_cancel = NULL;
static level_load_completed_fn g_level_load_completed = NULL;
static const uint8_t          *g_load_screen = NULL;   /* idLoadScreen; its first field is the thread */
static const uint32_t         *g_main_tid    = NULL;   /* the engine's main-thread id */
static volatile LONG           g_armed = 0;
static volatile LONG           g_stops = 0;

/* 1 on the engine main thread with the loading-screen thread drawing, else 0. */
static int thread_still_drawing(void)
{
    __try {
        const uint8_t *thread;
        uint32_t main_tid = *(const volatile uint32_t *)g_main_tid;
        if (main_tid == 0 || main_tid != GetCurrentThreadId()) return 0;
        thread = *(const uint8_t *const volatile *)g_load_screen;
        if (thread == NULL) return 0;
        return *(const volatile uint8_t *)(thread + LOAD_SCREEN_RUNNING_BYTE) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void sh_cancel_map_change_detour(void *common, uint8_t arg)
{
    if (thread_still_drawing()) {
        LONG n = InterlockedIncrement(&g_stops);
        if (n <= 8) {
            shield_fault f = { "load", -1,
                "load-screen-stop: the map load ended without stopping its loading screen; "
                "stopped it (idLoadScreen::LevelLoadCompleted) before returning to the menu", 0, 0 };
            shield_emit(&f);
        }
        g_level_load_completed((void *)g_load_screen);
    }
    g_orig_cancel(common, arg);
}

static uintptr_t clean_address(const sig_result *results, size_t count, const char *name)
{
    size_t i;
    if (results == NULL) return 0;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0)
            return results[i].status == SIG_OK ? results[i].addr : 0;
    return 0;
}

static int prologue_matches(const uint8_t *target)
{
    __try {
        return memcmp(target, k_cancel_map_change_prologue, LOAD_SCREEN_STOP_STOLEN) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int not_installed(const char *why)
{
    char m[224];
    shield_fault f = { "load", -1, m, 0, 0 };
    _snprintf_s(m, sizeof m, _TRUNCATE,
        "load-screen-stop: %s -- NOT installed; a fault during a map load stays declined", why);
    shield_emit(&f);
    return 0;
}

int sh_load_screen_stop_install(const uint8_t *module_base, const sig_result *results, size_t count)
{
    uint8_t *target;
    void *tramp;

    if (g_armed) return 1;
    if (module_base == NULL) return 0;
    if (g_orig_cancel) {   /* an earlier attempt left a trampoline: release it before retrying */
        if (!hook_unpatch((void *)g_orig_cancel)) return 0;
        g_orig_cancel = NULL;
    }

    target = (uint8_t *)clean_address(results, count, "CancelMapChange");
    g_level_load_completed = (level_load_completed_fn)clean_address(results, count, "LevelLoadCompleted");
    g_load_screen = (const uint8_t *)glb_resolve(module_base, "load_screen", NULL);
    g_main_tid = (const uint32_t *)glb_resolve(module_base, "main_thread_id", NULL);
    if (target == NULL || g_level_load_completed == NULL)
        return not_installed("CancelMapChange or LevelLoadCompleted did not resolve to a unique signature");
    if (g_load_screen == NULL || g_main_tid == NULL)
        return not_installed("the load_screen or main_thread_id global did not resolve");
    if (!prologue_matches(target))
        return not_installed("CancelMapChange's prologue differs (already hooked, or another build)");

    tramp = hook_prepare(target, (void *)sh_cancel_map_change_detour, LOAD_SCREEN_STOP_STOLEN);
    if (tramp == NULL) return not_installed(hook_last_failure());
    g_orig_cancel = (cancel_map_change_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig_cancel = NULL;
        return not_installed("committing the detour failed");
    }
    InterlockedExchange(&g_armed, 1);
    {
        char m[160];
        shield_fault f = { "load", -1, m, (uintptr_t)(target - module_base), 0 };
        _snprintf_s(m, sizeof m, _TRUNCATE,
            "load-screen-stop: armed on CancelMapChange (RVA 0x%X); a fault during a map load "
            "now returns to the menu", (unsigned)(target - module_base));
        shield_emit(&f);
    }
    return 1;
}

int sh_load_screen_stop_armed(void)
{
    return g_armed ? 1 : 0;
}
