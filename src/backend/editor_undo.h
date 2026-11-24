/* editor_undo.h -- empty the editor's undo and redo history down to the current state, for a permanent
 * Uninstall (modui.c). Main thread only, inside the process-heap scope native undo uses.
 *
 * The undo list is an idList<idSnapMapEdit *> at *(editor+0x10): data +0, count +8, capacity +0xc, cursor
 * +0x18. The engine's push (VK RVA 0x12039F0, GL RVA 0x1202CE0; EditorCommitEdit calls it through a wrapper
 * at +0x20 in both) destroys every entry from the cursor on and appends a copy of the live edit map, so
 * with the cursor one slot before the first entry it leaves exactly the current state. */
#ifndef BACKEND_EDITOR_UNDO_H
#define BACKEND_EDITOR_UNDO_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_EDITOR_UNDO_LIST_OFF    0x10    /* editor -> idList * */
#define SH_EDITOR_UNDO_COUNT_OFF   0x08
#define SH_EDITOR_UNDO_CAP_OFF     0x0c
#define SH_EDITOR_UNDO_CURSOR_OFF  0x18
#define SH_EDITOR_UNDO_COMMIT_CALL 0x20    /* EditorCommitEdit: E8 rel32 to the push wrapper */

/* The push: (list, editor). */
typedef void (*sh_editor_undo_push_fn)(void *list, void *editor);

/* Truncate the list at *(editor+0x10) to the current state through `push`, after validating the header.
 * Returns 1 with one entry left, 0 for an unknown header (nothing changed), -1 when the push faulted or
 * left another shape; *before and *after get the entry counts (-1 unknown). SEH-guarded. */
int sh_editor_undo_truncate(uint8_t *editor, sh_editor_undo_push_fn push, int *before, int *after);

/* The push's address from EditorCommitEdit's body: the rel32 CALL at +0x20 must reach
 * `48 8B 09 E9 rel32` (mov rcx,[rcx]; jmp push), else 0. `image`/`image_size` bound both targets. */
uintptr_t sh_editor_undo_decode_push(const uint8_t *commit, const uint8_t *image, size_t image_size);

/* Bind the push (decoded from the EditorCommitEdit result) and the process
 * heap scope. Returns 1 when the clear is available. */
int sh_editor_undo_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* The Mods tab's clear (modui.h clear_undo). probe 1: 1 map open and clear bound, 0 no map, -1 unavailable
 * on this build. probe 0: 1 cleared, 0 no map, -1 failed (logged). Main thread. */
int sh_editor_undo_clear(int probe);

/* Mark the open editor map modified as a placed entity does: EditorCommitEdit(editor, idStr label)
 * (bound by sh_editor_undo_install) pushes one undo step named `label`, for changes the edit map does not
 * hold (mod rows and choices, modui.c). 1 marked, 0 no map, -1 unavailable (logged once). Main thread. */
int sh_editor_mark_modified(const char *label);

/* Mark the open editor map modified without an undo step, for a change undo cannot restore (a map setting
 * in the map's text, map_render_editor.c), via vt+0x160(editor, 1). 1 marked, 0 no map, -1 unavailable. Main thread. */
int sh_editor_mark_modified_no_undo(void);

#endif /* BACKEND_EDITOR_UNDO_H */
