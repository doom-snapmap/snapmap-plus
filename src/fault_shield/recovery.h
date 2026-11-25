/* recovery.h -- the bad-load recovery router (survive -> steer the editor back to the My Maps browser). */
#ifndef SHIELD_RECOVERY_H
#define SHIELD_RECOVERY_H

#include <stddef.h>

/* The text Error(6) carries to the engine's error report and the crash record's engine text. No '%'. */
extern const char SHIELD_RECOVERED_TEXT[];

int  recovery_install(void);   /* install the Frame detour; return 1 on success */
void recovery_arm(void);       /* the VEH calls this after surviving a bad-load fault */
/* The VEH notes each Error(6) redirect; recovery_redirected_this_frame() is then 1
 * until that idCommonLocal::Frame call returns, 0 otherwise, -1 without the hook. */
void recovery_note_redirect(void);
int  recovery_redirected_this_frame(void);
/* The install's byte checks: Frame's stolen prologue and the FatalError level byte (7); 1 when as expected. */
int recovery_frame_prologue_ok(const void *frame);
int recovery_fatalerror_site_ok(const void *fatalerror);
void log_engine_error_text(void); /* the VEH's C++-throw path records a masked FatalError's verbatim text */
int  shield_last_engine_msg(char *out, size_t n); /* SEH-guarded read of the engine's last-error text; 1 if non-empty */
/* The sentence shown after the save guard dismisses the engine's offer to delete a save it could not
 * load (GDM 0x1D); no button ran, so nothing was deleted. */
extern const char SHIELD_DAMAGED_SAVE_TEXT[];
/* Show one sentence in the engine's own notice on a later frame (the newest
 * wins). Main thread. */
void shield_tell(const char *text);
/* Snapmap+ refused the map being loaded: for the next frames the guard dismisses the engine's "damaged
 * save" notice and its offer to replace the map with the published version. Any thread. */
void shield_note_refused_load(void);

#endif /* SHIELD_RECOVERY_H */
