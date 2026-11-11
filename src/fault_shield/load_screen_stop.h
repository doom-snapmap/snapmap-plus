/* Stop the loading-screen thread before the engine leaves a failed map load.
 *
 * An error during a map load (the engine's own recoverable Error, or the shield's
 * Error(6) redirect) unwinds past idLoadScreen::LevelLoadCompleted, the only code
 * that stops the loading-screen thread. idCommonLocal::Frame's catch then calls
 * idCommonLocal::CancelMapChange (common vtable +0x28), whose menu load syncs the
 * renderer and draws the same load GUI while that thread is still drawing: a
 * deadlock (a load that never finishes) or a race on the shared GUI.
 *
 * The detour on CancelMapChange runs LevelLoadCompleted first when, on the engine
 * main thread, the thread is still running (idLoadScreen thread byte +0x4B2). That
 * is the engine's own order on a load that completes: stop the thread with no lock
 * held, sync the renderer, free the load GUI, then load the next map. The original
 * CancelMapChange then runs unchanged. Install once at startup from the bootstrap's
 * signature results; armed only on a clean, verified prologue. */
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

/* The map-end subscriber (mod scripts, script/script_hooks.h): called on the
 * engine main thread at every CancelMapChange, before the loading screen is
 * stopped and before the original runs, with during_load 1 when the
 * loading-screen thread was still drawing (a failed load). Set once at boot;
 * NULL clears. Called only while the detour is armed. */
typedef void (*sh_map_end_fn)(int during_load);
void sh_load_screen_stop_set_map_end(sh_map_end_fn fn);

/* 1 once the detour is committed, else 0. The redirect policy reads it. */
int sh_load_screen_stop_armed(void);

#endif /* SHIELD_LOAD_SCREEN_STOP_H */
