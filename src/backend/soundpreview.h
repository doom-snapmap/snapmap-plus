/* soundpreview.h -- audition a sound decl from the asset browser.
 *
 * The obvious route, the `testSound` console command, does not work for a browser: it discards the
 * emitter handle the play returns, so a preview cannot be stopped and every click piles another
 * voice on top of the last. Confirmed in game as well as in the disassembly.
 *
 * This uses the editor's own audition path instead -- sound-world vtable +0x30 -- which solos the
 * preview, forces the listener so it plays at the ear, and hands back the emitter handle. Holding
 * that handle is what makes the difference: one preview at a time, stoppable on demand.
 *
 * The engine entry points and the sound-world global are resolved by byte signature, while StopSound
 * is verified and bound from the live sound-world vtable. Engine calls are SEH-guarded.
 *
 * THREADING: the public functions may be called from the UI thread. They queue every sound-world
 * mutation through DOOM's command buffer so the registered drain executes it on the main thread.
 */
#ifndef BACKEND_SOUNDPREVIEW_H
#define BACKEND_SOUNDPREVIEW_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Resolve the sound-world global and the play/stop pair. Idempotent. `cmdsys` is the already-decoded
 * idCmdSystemLocal used to set the two cvars this feature needs (see sh_soundpreview_play); NULL is
 * tolerated and only costs the background-audio convenience. Returns 1 if a preview can be played. */
int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys);

/* Stop whatever is previewing, then audition `name` (a soundshader decl path as it appears in the
 * browser catalog). Only one preview exists at a time by construction.
 *
 * Enters preview mode if it is not already up (s_playSoundInBackground 1, because the whole point
 * is to hear it while the Snapmap+ window -- not DOOM -- has focus). With a session open that is
 * already done, so the play itself changes no cvars at all.
 *
 * The name is validated against our own asset index FIRST. That is not politeness: the engine
 * resolves the name with a find-or-create that fatals on a miss, so an unchecked name is a crash.
 *
 * Returns 1 if the preview was accepted for main-thread execution, 0 if it was refused synchronously
 * (not installed, unknown name, missing world, or unavailable command buffer). */
int sh_soundpreview_play(const char *name);

/* Stop the current preview. While a session is open (below) this silences the emitter and nothing
 * else, leaving preview mode up so the next Play is instant; with no session it also drops the mode
 * and DOOM goes quiet again. Safe to call from any thread with nothing playing. */
void sh_soundpreview_stop(void);

/* Hold preview mode open for as long as the asset browser is on screen (on=1), and tear it down on
 * the way out (on=0, which also stops anything playing).
 *
 * This exists because toggling the mode per click was audibly wrong: each preview ran
 * s_playSoundInBackground 0 then 1, suspending and resuming DOOM's whole audio engine and
 * re-entering solo every time. Sounds faded in, and short ones could finish before the resume did.
 * Establishing the mode once, before the first click, removes that. Safe to call from any thread. */
void sh_soundpreview_set_session(int on);

/* Is something previewing right now? For the UI's play/stop button state. Thread-safe (plain read). */
int sh_soundpreview_active(void);

#endif /* BACKEND_SOUNDPREVIEW_H */
