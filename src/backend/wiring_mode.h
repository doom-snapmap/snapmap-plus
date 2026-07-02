/* wiring_mode.h -- the "link any entities" interactive wiring mode (console command sh_target_any).
 *
 * The SnapHak changelog's sh_target_any "link any entities" feature: a toggle that, while ON, lets the
 * player use the NORMAL in-editor wire tool to connect ANY source entity to ANY target entity --
 * including node-less targets such as a timeline host that the stock connect tool refuses -- by laying a
 * direct connection, repeatedly, until the mode is turned OFF again. This EXCEEDS the 2021 SnapHak binary
 * (whose own sh_target_any is only the editor-palette unhide); it
 * is a clean-room build from our own reverse-engineering of the editor connect tool.
 *
 * MECHANISM (two flag-gated inline detours on FSM LEAVES of the wire tool -- NOT on its central pick
 * processor). The pick processor drives the tool's input/camera/escape state machine; intercepting it
 * leaves the tool active and capturing input with its state never advanced (input, including escape, is
 * swallowed). So instead of touching the pick processor we leave the tool's input handling fully intact
 * and only redirect its OUTCOME at two leaves:
 *
 *   - Hook 1, the output-select leaf (engine 0xcdaa30, ABI void(tool, a, world)): the leaf that the
 *     stock tool reaches after the first (source) pick, and which would raise a modal output-node picker.
 *     In wire-mode it instead records the source, selects the direct-edge creator, advances the tool to
 *     target-select, and returns WITHOUT raising the modal -- so input/escape stay alive (the FSM is
 *     never left in an unhandled state).
 *   - Hook 2, the connect creator (engine 0xcdbb40, ABI void(tool, world, idx)): the leaf the tool
 *     reaches on the second (target) pick. In wire-mode it forces the stock direct-edge outcome for ANY
 *     target -- it records the target into the tool's second chain slot and sets the direct-edge flags,
 *     so the tool's own trailing finalize lays a direct source->target edge with no node mediation
 *     (which is what lets a node-less target, e.g. a timeline, be wired).
 *
 *   Both detours pass straight through to the original when the mode is OFF (the default), so the stock
 *   wire tool is completely untouched and turning the mode OFF needs no uninstall and leaves no
 *   half-state. The resulting edge uses the editor's own internal connect path, so it is structurally
 *   identical to one the stock tool would create and saves/reloads normally.
 *
 * PORTABILITY: both detour targets and the tool-reset helper are resolved by SIGNATURE (never a hardcoded
 * base+RVA). The tool-struct field offsets are build-specific; re-derive them per DOOM build by
 * decompiling the two leaves (see wiring_mode.c).
 *
 * Off by default. FAIL-SAFE: if either detour target can't be resolved, neither detour is installed and
 * the toggle refuses cleanly -- no crash, no partial state. Every engine memory touch is SEH-guarded.
 * Clean-room: built from our own reverse-engineering. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_WIRING_MODE_H
#define BACKEND_WIRING_MODE_H

#include <stdint.h>

struct idCmdArgs;

/* sh_target_any: toggle the interactive wire-any mode (1st call ON, 2nd call OFF). */
void h_wiring_mode(struct idCmdArgs *a);

/* Resolve the editor wire tool's two FSM-leaf detour targets + the tool-reset helper by signature and --
 * only if both detour targets resolve -- install the two (flag-gated, off-by-default) detours ONCE. If
 * anything fails it installs nothing and the toggle refuses cleanly. The handler is registered separately
 * by the console-command table. */
void sh_wiring_mode_install(const uint8_t *module_base);

#endif /* BACKEND_WIRING_MODE_H */
