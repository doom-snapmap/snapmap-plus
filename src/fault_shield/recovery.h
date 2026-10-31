/* recovery.h -- the bad-LOAD recovery router (survive -> steer the editor back to the My-Maps browser). */
#ifndef SHIELD_RECOVERY_H
#define SHIELD_RECOVERY_H

int  recovery_install(void);   /* install the Frame detour; return 1 on success */
void recovery_arm(void);       /* the VEH calls this after surviving a bad-LOAD fault */
/* The VEH notes each Error(6) redirect; recovery_redirected_this_frame() is then 1
 * until that idCommonLocal::Frame call returns, 0 otherwise, -1 without the hook. */
void recovery_note_redirect(void);
int  recovery_redirected_this_frame(void);
void notice_request(void);     /* request a generic editor toast; current Class-A recovery is silent */
void notice_request_msg(void); /* Class-B/Error(6): toast carries the engine's harvested error text if present */
void log_engine_error_text(void); /* the VEH's C++-throw path calls this to record a masked FatalError's verbatim text */
int  shield_last_engine_msg(char *out, size_t n); /* SEH-guarded read of the engine's last-error text; 1 if non-empty */

#endif /* SHIELD_RECOVERY_H */
