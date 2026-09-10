/* Audition one browser sound through sound-world slot +0x30 and retain its
 * emitter handle for stopping. Resolve the world/play entry points by
 * signature and bind StopSound from the verified live vtable on first use.
 * Public calls queue mutations through the native command buffer for main-
 * thread execution.
 */
#ifndef BACKEND_SOUNDPREVIEW_H
#define BACKEND_SOUNDPREVIEW_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Resolve the world slot, preview call and command-buffer functions.
 * Idempotent; returns 1 when those dependencies resolve. cmdsys supplies the
 * command system required to queue playback and set preview cvars. StopSound
 * binds from the live world on first use.
 */
int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys);

/* Validate the catalog name, queue stopping the current emitter, then start
 * one preview in background-audio mode. Returns 1 when accepted for main-
 * thread execution, not when audio is confirmed; synchronous refusal returns
 * 0.
 */
int sh_soundpreview_play(const char *name);

/* Stop the current preview. While a session is open (below) this silences the emitter and nothing
 * else, leaving preview mode up so the next Play is instant; with no session it also drops the mode
 * and DOOM goes quiet again. Safe to call from any thread with nothing playing. */
void sh_soundpreview_stop(void);

/* Hold preview mode while the browser is open (on=1). Closing it (on=0) stops
 * playback and restores normal audio. Keeping the mode between clicks avoids
 * suspending and resuming the whole audio engine. Callable from any thread.
 */
void sh_soundpreview_set_session(int on);

/* Return whether a preview handle is retained, for the UI play/stop state. */
int sh_soundpreview_active(void);

#endif /* BACKEND_SOUNDPREVIEW_H */
