/* editor_frame.h -- a main-thread execution point at a frame boundary, and the requests it serves.
 *
 * Hooks the editor's own per-frame Think and services a small request queue immediately after the
 * engine's frame work returns. That is a real frame boundary on the real thread. The two nearby
 * execution points are NOT equivalent: the console drain runs INSIDE the frame, so a lifecycle call
 * made from it re-enters and deadlocks; the vtable work-queue drain (+0x1a0) is reached from the
 * frontend's think loop, which is the UI thread.
 *
 * THE RELOAD ARMS THE SWAP ITSELF. LoadMap takes a saved-map id, and the reload passes one off disk;
 * armed, the id only has to name something loadable, because our bytes are substituted into the
 * parse. Disarmed, the same call would really open that other map and discard unsaved work. So the
 * arm is not optional and is not the caller's to supply: the reload arms around its own LoadMap and
 * restores the previous state on every path out, fault included.
 *
 * A WINDOW, not a one-shot count. The engine parses exactly once per reload [DIRECT: six reloads
 * across two sessions, each a clean arm -> one fire -> disarm], so a one-shot works today and would
 * silently leave the rest of a multi-parse load vanilla if that ever changed. Restoring saved state
 * cannot be wrong by a count. What the request checks instead is sh_rawmap_source_ok, on the click
 * and again on the frame.
 *
 * SAFETY. Every engine read is SEH-guarded, and every precondition is re-checked on the frame we
 * fire from rather than when the request was made. One fault disables the module for the session: a
 * frame hook that can fault once will fault every frame. */
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
 *   `add_branch_tag_fn`= idSnapMap::AddTag("map:branch"). Optional; null only costs the tag that
 *                        makes a substituted map save as a NEW map.
 *   `module_base`      = host image base, for resolving the editor singleton. */
int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            void *add_branch_tag_fn,
                            const uint8_t *module_base);

/* Could a rawmap save happen right now? 1 = yes, and it changes NOTHING either way -- ask before
 * applying a destination of your own, so a refusal cannot leave the save path moved. */
int sh_editor_frame_can_rawmap_save(char *out_msg, int msg_capacity);

/* Queue "write the OPEN map to the rawmap file" for the next editor frame. Only QUEUES it: 1 =
 * accepted, with the outcome in the log and the next status refresh; 0 = refused, reason in
 * out_msg (usually no map open, or a build that could not resolve the serializer). */
int sh_editor_frame_request_rawmap_save(char *out_msg, int msg_capacity);

/* Request an in-place map reload on the next editor frame. 1 = accepted, 0 = refused (not installed,
 * the staged rawmap is unusable, no loadable saved-map id, a request is already pending, or the
 * module has faulted). `out_msg` always gets a reason fit to show in the UI.
 *
 * Accepted is not a promise: the frame hook re-checks preconditions and may still decline. Poll
 * sh_editor_frame_reload(). */
int sh_editor_frame_request_reload(char *out_msg, int msg_capacity);

typedef enum sh_reload_state {
    SH_RELOAD_IDLE = 0,     /* nothing requested since install */
    SH_RELOAD_PENDING,      /* queued, waiting for a frame whose preconditions hold */
    SH_RELOAD_DONE,         /* LoadMap ran and returned success */
    SH_RELOAD_FAILED        /* LoadMap ran and reported failure, or the attempt faulted */
} sh_reload_state;

/* Where the most recent reload request got to. `out_msg` (optional) receives the last detail line
 * the module logged about it. */
sh_reload_state sh_editor_frame_reload(char *out_msg, int msg_capacity);

/* How many editor frames the hook has serviced. Zero after install means the hook is not firing,
 * which separates "our hook never runs" from "the reload was declined". */
unsigned long sh_editor_frame_ticks(void);

/* The most recently written local SnapMap save folder, and its 20-hex id. `out_id` may be NULL.
 * Returns 1 when a folder was found; touches no engine state and needs no frame.
 *
 * "Most recently written" is a HEURISTIC for "the map you are working on" -- wrong when someone has
 * two maps on the go, so a caller showing the result to a person should name the id. */
int sh_editor_frame_saved_map_dir(char *out, size_t cap, char *out_id, size_t id_cap);

#endif /* BACKEND_EDITOR_FRAME_H */
