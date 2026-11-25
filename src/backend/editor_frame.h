/* editor_frame.h -- a main-thread execution point at a frame boundary, and the requests it serves.
 * Hooks the editor's per-frame Think and services a small request queue right after it returns (the
 * console drain runs inside the frame and deadlocks on lifecycle calls; the +0x1a0 drain is the UI thread). */

/* Opening a rawmap passes LoadMap a saved-map id; the borrowed map's files are never read and the load
 * detour feeds the editor the rawmap's checked text (rawmap.c). A request is dropped if the open map or
 * load generation changed, and a LoadMap fault turns off only the reload for the session. */
#ifndef BACKEND_EDITOR_FRAME_H
#define BACKEND_EDITOR_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Install the detour on the editor's per-frame Think; returns 1 if installed (idempotent). `frame_fn` is
 * the resolved "EditorFrame" (NULL logs SKIPPED, returns 0); `status_ok` is 1 only for a clean scan hit,
 * never the known_rva fallback, so a prologue that is already a detour is not patched. `load_map_fn` is
 * "EditorLoadMap" or NULL for the hook without reload; `module_base` resolves the editor singleton. */
int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            const uint8_t *module_base);

/* Detour ReadLocalSavedMapText so the reload never reads the saved map whose id it borrows: inside that
 * one LoadMap call a read of that id gets a placeholder text and the slot's files are never opened. A
 * borrowed map the savegame manager failed to read is marked unreadable and deleted by the My Maps scan,
 * so opening a rawmap is refused without this. `assign_cstr_fn` is IdStrAssignCStr. */
int sh_editor_frame_carrier_install(void *read_text_fn, int status_ok, void *assign_cstr_fn);

/* After each successful saved-map text read through that detour, `filter` may replace the text (`text` is
 * the engine's idStr, `caller` the read's return address), e.g. the cloud save's read of a publish's slot
 * (modpublish.c). NULL removes it. */
void sh_editor_frame_set_read_filter(void (*filter)(const char *id, void *text, const void *caller));
/* 1 when the detour is installed and a filter is set. */
int sh_editor_frame_read_filter_live(void);

/* Could a rawmap save happen right now? 1 = yes. Changes nothing either way, so ask before applying a
 * destination of your own and a refusal cannot leave the save path moved. */
int sh_editor_frame_can_rawmap_save(char **msg);

/* Queue "write the open map to a rawmap file" for the next editor frame. NULL uses the current save path;
 * an explicit path becomes the target only when that same map services the save. 1 = queued (the outcome
 * is told to the player), 0 = refused; either way *msg (malloc'd) is the player's sentence. */
int sh_editor_frame_request_rawmap_save_to(const char *destination, char **msg);

/* Could the editor open a rawmap right now? Side-effect free; 0 leaves one sentence in *msg (malloc'd). */
int sh_editor_frame_can_reload(char **msg);

/* Queue an in-place reload for the next editor frame; 1 = queued, 0 = refused, and either way *msg
 * (malloc'd) is one sentence for the player. The frame re-checks everything and reports a failed open. */
int sh_editor_frame_request_reload(char **msg);

#endif /* BACKEND_EDITOR_FRAME_H */
