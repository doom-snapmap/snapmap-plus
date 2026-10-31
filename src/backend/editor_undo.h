/* editor_undo.h -- empty the editor's undo and redo history down to the
 * current state, for a permanent Uninstall (modui.c).
 *
 * The undo list is an idList<idSnapMapEdit *> at *(editor+0x10): data +0,
 * count +8, capacity +0xc, cursor +0x18 (a pointer into data). Every entry is
 * a 0xDD0-byte snapshot of the edit map. The engine's own push (Vulkan RVA
 * 0x12039F0, OpenGL RVA 0x1202CE0; EditorCommitEdit calls it through a
 * two-instruction wrapper at +0x20 in both executables) advances the cursor,
 * destroys every entry from the cursor to the end (the snapshot destructor,
 * then the engine's sized free), compacts the list, and appends a fresh copy
 * of the live edit map with the cursor on it. With the cursor set one slot
 * before the first entry, that push destroys every snapshot and leaves exactly
 * one: the current state. No other engine address is needed, and the engine
 * itself frees what it allocated.
 *
 * Main thread only, inside the process-heap scope native undo uses (the push
 * allocates the new snapshot on the current heap). */
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

/* Truncate the list at *(editor+0x10) to the current state through `push`.
 * Validates the header first (a readable list, 0 <= count <= capacity, the
 * cursor inside the entries). Returns 1 with exactly one entry left and the
 * cursor on it, 0 when the header is not a list this code knows (nothing
 * changed), -1 when the push faulted or left another shape. *before and
 * *after receive the entry counts (-1 unknown). Every read and the call are
 * SEH-guarded. */
int sh_editor_undo_truncate(uint8_t *editor, sh_editor_undo_push_fn push, int *before, int *after);

/* The push's address from EditorCommitEdit's body: the rel32 CALL at +0x20
 * must reach `48 8B 09 E9 rel32` (mov rcx,[rcx]; jmp push). 0 when either
 * shape differs. `image`/`image_size` bound both targets. */
uintptr_t sh_editor_undo_decode_push(const uint8_t *commit, const uint8_t *image, size_t image_size);

/* Bind the push (decoded from the EditorCommitEdit result) and the process
 * heap scope. Returns 1 when the clear is available. */
int sh_editor_undo_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* The Mods tab's clear (modui.h clear_undo). probe 1: 1 when an editor map
 * is open and the clear is bound, 0 when no editor map is open, -1 when the
 * clear is unavailable on this build. probe 0: 1 cleared, 0 no editor map,
 * -1 failed (logged). Main thread. */
int sh_editor_undo_clear(int probe);

#endif /* BACKEND_EDITOR_UNDO_H */
