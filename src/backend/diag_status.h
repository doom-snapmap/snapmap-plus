/* diag_status.h -- the diagnostic build's status export for the development daemon. Built only with
 * build.ps1 -Diag (SH_DIAG); a release build has none of this code. */
#ifndef BACKEND_DIAG_STATUS_H
#define BACKEND_DIAG_STATUS_H

#ifdef SH_DIAG
#include <stddef.h>

#define SH_DIAG_STATUS_PROTOCOL 1

/* Exported at ordinal 300 (xinput1_4_diag.def); callable from any thread. Writes one JSON object into
 * out and returns its length, or the length needed when cap is too small (out is then left empty). The
 * object carries protocol, version, iface_vtbl, features_off and rawmap {opens, generation, pending,
 * faulted, last {seq, path, state, rc, reason, bytes, mods}}; last.state is "" and last.seq 0 before the first open. */
int sh_diag_status(char *out, int cap);

/* The object, malloc'd and sized to it; NULL when memory runs out. */
char *sh_diag_status_build(void);

/* Notes from mod scripts for the daemon's entity inspector: a script asks noting() and hands lines to
 * note(text) (script_api_world.c). sh_diag_note_listening (data, ordinal 302) is set non-zero by a
 * listener; sh_diag_note (ordinal 301, game thread) only counts, and the listener hooks its entry to read
 * mod (the mod's salt), text (NUL-terminated) and len. */
extern volatile long sh_diag_note_listening;
extern volatile long sh_diag_note_count;
void sh_diag_note(const char *mod, const char *text, size_t len);

/* A development hold of one demon (inspector `inspect_arm hold=`), exported as data at ordinal 303. While
 * the listener-written entity address matches, the script runtime gives that entity no think (no step,
 * order or trigger) but still delivers damage, pain, death and frame rows; NULL releases, a map's
 * close clears it. The game thread reads; one entity at a time. */
extern void *volatile sh_diag_hold_entity;
#endif

#endif /* BACKEND_DIAG_STATUS_H */
