/* wiring_mode.h -- the "link any entities" interactive wiring mode (console command sh_target_any).
 *
 * The OG SnapHak changelog's sh_target_any "link any entities" feature: a toggle that, while ON, lets
 * the player use the NORMAL in-editor wire tool to connect ANY source entity to ANY target entity --
 * including node-less targets such as a timeline host that the stock connect tool refuses -- by laying a
 * direct connection, repeatedly, until the mode is turned OFF again. This EXCEEDS the 2021 SnapHak binary
 * (whose own sh_target_any is only the editor-palette unhide, which the clone exposes as `sh_unhide`); it
 * is a clean-room build from our own reverse-engineering of the editor connect tool.
 *
 * MECHANISM (a flag-gated inline detour on the wire tool's pick processor -- supersedes the earlier
 * instance-filter-lever approach, which still routed picks through the stock node-mediation):
 *   The editor wire tool routes every entity pick through a single central pick processor
 *   (FUN_140cdad70 @ 0xcdad70; a vtable slot, also called internally on deactivate/re-pick). We install
 *   ONE inline detour on its prologue at startup and gate the new behavior on a flag:
 *     - mode OFF (default): the detour passes straight through to the original processor -- the stock
 *       wire tool is completely untouched. Because the detour stays installed and merely passes through
 *       when the flag is 0, turning the mode OFF needs NO uninstall and leaves NO half-state.
 *     - mode ON: a two-pick direct-edge state machine -- the first picked entity is remembered as the
 *       source, the second lays a direct connection from the source to it via the editor's own connection
 *       primitive (FUN_1405a70d0), marks the map dirty, and resets. A deselect (index -1) is ignored.
 *   The connection primitive is the same one the editor uses internally, so the resulting edge is
 *   structurally identical to one the stock tool would create and saves/reloads normally.
 *
 * PORTABILITY: the pick processor and the tool-reset helper are resolved by SIGNATURE (never a hardcoded
 * base+RVA). The connection primitive shares its prologue with a sibling edge-remove routine, so it is
 * resolved by decoding the single call to it from the uniquely-signatured output creator (a version-
 * portable anchor-and-decode, mirroring how the cmd-system / render-world globals are recovered).
 *
 * Off by default. FAIL-SAFE: if any of the three engine functions can't be resolved, the detour is never
 * installed and the toggle refuses cleanly -- no crash, no partial state. Every engine memory touch is
 * SEH-guarded. Clean-room: built from our own reverse-engineering. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_WIRING_MODE_H
#define BACKEND_WIRING_MODE_H

#include <stdint.h>

struct idCmdArgs;

/* sh_target_any: toggle the interactive wire-any mode (1st call ON, 2nd call OFF). */
void h_wiring_mode(struct idCmdArgs *a);

/* Resolve the three editor connect-tool functions by signature and -- only if all resolve -- install the
 * (flag-gated, off-by-default) pick-processor detour ONCE. If anything fails it installs nothing and the
 * toggle refuses cleanly. The handler is registered separately by the console-command table. */
void sh_wiring_mode_install(const uint8_t *module_base);

#endif /* BACKEND_WIRING_MODE_H */
