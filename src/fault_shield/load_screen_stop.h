/* Stop the loading-screen thread before the engine leaves a failed map load. An error during a load unwinds
 * past idLoadScreen::LevelLoadCompleted, so CancelMapChange (common vtable +0x28) would draw the load GUI
 * while that thread still draws: a deadlock or a race. The CancelMapChange detour runs LevelLoadCompleted
 * first when the thread is still running (byte +0x4B2) on the engine main thread, then the original;
 * it arms only on a clean, verified prologue. */
#ifndef SHIELD_LOAD_SCREEN_STOP_H
#define SHIELD_LOAD_SCREEN_STOP_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* The stolen window: MOV [RSP+0x10],RBX / MOV [RSP+0x18],RBP / MOV [RSP+0x20],RSI,
 * identical on both build 13954591 executables and position-independent. */
#define LOAD_SCREEN_STOP_STOLEN 15
static const uint8_t k_cancel_map_change_prologue[LOAD_SCREEN_STOP_STOLEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,
    0x48, 0x89, 0x6C, 0x24, 0x18,
    0x48, 0x89, 0x74, 0x24, 0x20
};

/* Return 1 when armed (or already armed), 0 when not installed. */
int sh_load_screen_stop_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Map-end subscribers run on the engine main thread at every CancelMapChange, in add order, before the
 * original frees the game; during_load is 1 when the loading-screen thread was still drawing. Returns 1 when
 * subscribed (a repeat keeps one), 0 when fn is NULL or all four places are taken. */
typedef void (*sh_map_end_fn)(int during_load);
int sh_load_screen_stop_add_map_end(sh_map_end_fn fn);

/* 1 once the detour is committed, else 0. The redirect policy reads it. */
int sh_load_screen_stop_armed(void);

#endif /* SHIELD_LOAD_SCREEN_STOP_H */
