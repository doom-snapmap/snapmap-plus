/* editor_frame.h -- a main-thread execution point at a frame boundary, and the requests it serves.
 *
 * Hooks the editor's own per-frame Think and services a small request queue immediately after the
 * engine's frame work returns. That is a real frame boundary on the real thread. The two nearby
 * execution points are NOT equivalent: the console drain runs INSIDE the frame, so a lifecycle call
 * made from it re-enters and deadlocks; the vtable work-queue drain (+0x1a0) is reached from the
 * frontend's think loop, which is the UI thread.
 *
 * OPENING A RAWMAP. LoadMap takes a saved-map id, and the reload passes one off disk. Inside that
 * one LoadMap call the borrowed map's files are never read (sh_editor_frame_carrier_install) and
 * the load detour gives the editor's parse the rawmap's checked text (rawmap.c), so the id only
 * names a slot the engine has listed. A request records
 * the map that was open and the load generation, and is dropped on the frame if either changed: a
 * reload never outlives the map it was asked for.
 *
 * SAFETY. Every engine read is SEH-guarded, and every precondition is re-checked on the frame we
 * fire from rather than when the request was made. A fault inside LoadMap turns off only the
 * reload for the session; the other per-frame features keep running. */
#ifndef BACKEND_EDITOR_FRAME_H
#define BACKEND_EDITOR_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Install the detour on the editor's per-frame Think. Returns 1 if installed. Idempotent.
 *   `frame_fn`         = resolved "EditorFrame". NULL => logs SKIPPED, returns 0.
 *   `status_ok`        = 1 iff the resolve was a CLEAN scan hit (SIG_OK), not the known_rva
 *                        fallback: never patch over a prologue that is already a detour.
 *   `load_map_fn`      = resolved "EditorLoadMap", or NULL for the frame hook with no reload
 *                        capability (still useful as an execution point).
 *   `module_base`      = host image base, for resolving the editor singleton. */
int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            const uint8_t *module_base);

/* Detour ReadLocalSavedMapText so the reload never reads the saved map whose id it borrows:
 * inside that one LoadMap call, a read of that id gets a placeholder text (the load detour then
 * parses the rawmap instead) and the slot's files are never opened. Without it a borrowed map the
 * savegame manager failed to read was marked unreadable, and the My Maps scan deleted it, so
 * opening a rawmap is refused unless this is installed. `assign_cstr_fn` is IdStrAssignCStr. */
int sh_editor_frame_carrier_install(void *read_text_fn, int status_ok, void *assign_cstr_fn);

/* The editor Think entry as resolved, or NULL before install ran or on a build that could not
 * resolve it. Valid whether or not the hook went in, and it is the PRE-hook entry: the install
 * overwrites the prologue, so a later signature scan for this function finds nothing. */
void *sh_editor_frame_target(void);

/* Could a rawmap save happen right now? 1 = yes, and it changes NOTHING either way -- ask before
 * applying a destination of your own, so a refusal cannot leave the save path moved. */
int sh_editor_frame_can_rawmap_save(char *out_msg, int msg_capacity);

/* Queue "write the OPEN map to a rawmap file" for the next editor frame. NULL uses the current save
 * path; an explicit path becomes the target only when that same map services the save. 1 = queued,
 * with the outcome told to the player when it lands; 0 = refused, reason in out_msg. */
int sh_editor_frame_request_rawmap_save_to(const char *destination, char *out_msg, int msg_capacity);

/* Could the editor open a rawmap right now? Side-effect free. 1 = yes; 0 with one sentence for the
 * player in `out_msg`. */
int sh_editor_frame_can_reload(char *out_msg, int msg_capacity);

/* Queue an in-place reload that opens the load path on the next editor frame. 1 = queued; 0 =
 * refused with one sentence for the player in `out_msg`. The frame re-checks everything and tells
 * the player if the open fails. */
int sh_editor_frame_request_reload(char *out_msg, int msg_capacity);

#endif /* BACKEND_EDITOR_FRAME_H */
