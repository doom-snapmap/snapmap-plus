/* Auditions one browser sound through sound-world slot +0x30 and keeps its emitter handle for stopping.
 * Public calls queue their mutations through the native command buffer for main-thread execution. */
#ifndef BACKEND_SOUNDPREVIEW_H
#define BACKEND_SOUNDPREVIEW_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Resolve the world slot, preview call and command-buffer functions by signature (StopSound binds from
 * the live vtable on first use). Idempotent; returns 1 when they resolve; cmdsys queues playback. */
int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys);

/* Validate the catalog name, queue stopping the current emitter, then start one preview. Returns 1 when
 * accepted for the main thread (not when audio is confirmed), 0 on immediate refusal; see sh_soundpreview_refusal. */
int sh_soundpreview_play(const char *name);

/* Why the latest audition was refused, at once or later on the main thread, as owned interface text;
 * NULL when accepted. Any thread. */
char *sh_soundpreview_refusal(void);

/* Stop the current preview. With a session open only the emitter stops (the mode stays up for an instant
 * next Play); otherwise the mode drops too. Any thread. */
void sh_soundpreview_stop(void);

/* Hold preview mode while the browser is open (on=1), avoiding audio-engine suspend/resume between clicks;
 * on=0 stops playback and restores the player's sound settings. Any thread. */
void sh_soundpreview_set_session(int on);

#endif /* BACKEND_SOUNDPREVIEW_H */
