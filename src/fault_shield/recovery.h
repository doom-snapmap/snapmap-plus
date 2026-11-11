/* recovery.h -- the bad-load recovery router (survive -> steer the editor back to the My Maps browser). */
#ifndef SHIELD_RECOVERY_H
#define SHIELD_RECOVERY_H

#include <stddef.h>

/* What the player is told after a recovered fault: the text Error(6) carries
 * to the engine's own error report, and the crash record's engine text, which
 * the Snapmap+ window shows with its offer to send a report. No '%'. */
extern const char SHIELD_RECOVERED_TEXT[];

int  recovery_install(void);   /* install the Frame detour; return 1 on success */
void recovery_arm(void);       /* the VEH calls this after surviving a bad-load fault */
/* The VEH notes each Error(6) redirect; recovery_redirected_this_frame() is then 1
 * until that idCommonLocal::Frame call returns, 0 otherwise, -1 without the hook. */
void recovery_note_redirect(void);
int  recovery_redirected_this_frame(void);
void log_engine_error_text(void); /* the VEH's C++-throw path records a masked FatalError's verbatim text */
int  shield_last_engine_msg(char *out, size_t n); /* SEH-guarded read of the engine's last-error text; 1 if non-empty */
/* The reason shown after the save guard dismisses the engine's offer to delete
 * a damaged save (GDM id of that offer). */
const char *shield_damaged_save_text(int gdm);

#endif /* SHIELD_RECOVERY_H */
