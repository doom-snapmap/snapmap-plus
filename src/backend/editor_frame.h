/* editor_frame.h -- a main-thread execution point at a frame boundary, and the map reload it exists
 * for.
 *
 * WHY THIS EXISTS
 * ---------------
 * The product had no way to run engine work on DOOM's own main thread at a safe moment. It had two
 * things that look like one and are not:
 *
 *   - the console command buffer's drain callback. Genuinely the main thread, and fine for short
 *     work, but heavy lifecycle calls made from inside it deadlock: the drain runs inside the
 *     frame, and a call that pumps the frame itself re-enters. This is why firing the editor's Play
 *     and LoadMap handlers from a console command froze DOOM, and why an "ESC froze the editor"
 *     report was once misattributed to the exit logic rather than to the firing context.
 *   - the interface vtable's work-queue drain (+0x1a0). Reached from the frontend's ~30 Hz think
 *     loop, which is the UI thread, NOT the engine's. Several backend headers say "call from the
 *     engine tick (main thread)" about functions that arrive through it; that is the conflation
 *     upstream issue #61 warns about, and it is not fixed by this file.
 *
 * So this hooks the editor's own per-frame Think and services a small request queue immediately
 * after the engine's frame work returns. That is a real frame boundary on the real thread: nothing
 * of the engine's is mid-iteration, and we are not nested inside a drain that is nested inside the
 * frame.
 *
 * WHAT IT DRIVES TODAY
 * --------------------
 * One request: reload the editor's map in place. Combined with the rawmap load swap, that is what
 * makes "Load Rawmap" in the File menu actually open the chosen file, instead of staging it and
 * asking the person to go and open a map by hand.
 *
 * WHY THE RELOAD ARMS THE SWAP ITSELF
 * -----------------------------------
 * LoadMap takes a saved-map id, and the reload passes one from disk. While the swap is armed that
 * does not matter -- whichever map the engine goes to fetch, our bytes are substituted into the
 * parse, so the id only has to name something loadable. With the swap disarmed the same call would
 * genuinely open that other map and discard whatever is open, unsaved.
 *
 * So the arm is not optional, and it was a mistake to make the person supply it. The reload arms the
 * swap immediately before its own LoadMap call and restores the previous state on every path out,
 * fault included. It means the arm can no longer be left switched on across a session, silently
 * substituting ordinary map loads.
 *
 * A WINDOW, not a one-shot count -- and the reason is that a window assumes nothing about how many
 * times the engine parses. Measured, it parses exactly ONCE per reload (six reloads across two
 * sessions, each a clean arm -> one fire -> disarm), so a one-shot would work today; it would stop
 * working the day that count changed, and would silently leave the rest of a multi-parse load
 * vanilla. Restoring state we saved cannot be wrong by a count.
 *
 * An earlier version of this comment claimed the engine parses twice per reload. It does not. That
 * came from a build whose arm stayed switched on, where hand-driven map loads fired the swap too and
 * were miscounted as part of the reload -- which is exactly the collateral this window removed.
 *
 * What the request checks instead is the staged file: sh_rawmap_source_ok, on the click and again on
 * the frame. "The bytes will be accepted" is the property that makes the reload safe. The arm never
 * was -- it was only ever a thing the person could forget.
 *
 * SAFETY
 * ------
 * Every engine read is SEH-guarded and every precondition is checked on the frame we fire from, not
 * when the request was made -- the editor can leave a usable state in between. One fault disables
 * the whole module for the session: a frame hook that can fault once will fault every frame.
 */
#ifndef BACKEND_EDITOR_FRAME_H
#define BACKEND_EDITOR_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Install the detour on the editor's per-frame Think.
 *   `frame_fn`   = resolved address of the "EditorFrame" signature. NULL => logs SKIPPED, returns 0.
 *   `status_ok`  = 1 iff the resolve was a CLEAN scan hit (SIG_OK), not the hook-tolerant known_rva
 *                  fallback. Same policy as the rawmap detours: never patch over a prologue that is
 *                  already a detour.
 *   `load_map_fn`= resolved address of "EditorLoadMap", or NULL to install the frame hook with no
 *                  reload capability (the hook is still useful as an execution point).
 *   `module_base`= host image base, for resolving the editor singleton through engine_globals.
 * Returns 1 if the detour was installed. Idempotent. */
int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            const uint8_t *module_base);

/* Request an in-place map reload on the next editor frame.
 *
 * Returns 1 when the request was accepted for the next frame, 0 when it was refused outright --
 * the module is not installed, the staged rawmap is unusable, no loadable saved-map id exists, a request is
 * already pending, or the module has faulted. `out_msg` always receives a short human-readable
 * reason, suitable for showing in the UI. Accepting is not a promise the reload happened: the frame
 * hook re-checks the editor's preconditions and may still decline. Poll sh_editor_frame_reload().
 */
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

/* How many editor frames the hook has serviced. Zero after install means the hook is not firing --
 * the single most useful thing to know when a reload never happens, because it separates "the
 * editor is not running our hook" from "the reload was declined". */
unsigned long sh_editor_frame_ticks(void);

/* The full path of the most recently written local SnapMap save folder, and its 20-hex id.
 *
 * Exported from here because this file already owns the layout knowledge and the two mistakes that
 * layout invites (the per-account folder level, and "Saved Games" being its own known folder rather
 * than the parent of Documents) -- see the comment block above the finder. `out_id` may be NULL.
 * Returns 1 when a folder was found, 0 otherwise; touches no engine state and needs no frame.
 *
 * "Most recently written" is a HEURISTIC for "the map you are working on", not proof of it. It is
 * the right one in practice, because the folder's write time is the moment the map was last saved,
 * and it is honest about being wrong when someone has two maps on the go. Callers that show the
 * result to a person should name the id. */
int sh_editor_frame_saved_map_dir(char *out, size_t cap, char *out_id, size_t id_cap);

#endif /* BACKEND_EDITOR_FRAME_H */
