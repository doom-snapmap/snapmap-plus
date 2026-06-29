/* linkany.h -- the "link any entities" toggle (console command sh_target_any).
 *
 * The OG SnapHak changelog's sh_target_any "link any entities" MODE: a toggle that, while ON, lets the
 * editor's "Add Logic" tool connect ANY entity as a target -- bypassing the decl output/input
 * compatibility gate -- while KEEPING the native node-mediation (source -> listener-node -> action-node
 * -> target) so the result is a real BLUE/logic wire that persists + serializes normally.
 *
 * This feature does NOT exist in the 2021-03-27 SnapHak binary (whose sh_target_any IS the unhide, now
 * exposed by the clone as `sh_unhide`); it is a clean-room build from our own RE of the engine's editor
 * connect-tool, RE'd DIRECT from our own decompile.
 *
 * MECHANISM (option A -- the clean instance-filter lever; supersedes the v1 raw-edge detour, which made
 * GREEN/path wires by mistake): the connect tool gates each candidate target through a single .bss lever
 *   ((target+0x164 & 0x20) == 0) || (DAT_14571c660 != 0)
 * DAT_14571c660 (RVA 0x571c660) has 41 xrefs ALL-READ and no writer (Ghidra-verified) -- so while ON we
 * write it to 1 (the OR short-circuits true -> any instance is a valid target) and while OFF we write 0
 * (stock filter restored). The lever is resolved PORTABLY: sig "ConnectOutputCreator" (FUN_140cdbb40),
 * then RIP-decode the lever from its first `cmp dword[rip+disp],0`. The native creators still build the
 * node-mediated logic wire, so wires stay blue. Off by default; FAIL-SAFE (refuses, never crashes).
 *
 * LIMITATION: opens LAYER 1 only; a node-less target (no input nodes) still shows an empty picker -- the
 * deferred "node injection" is what would reach those ~1200 decls.
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B2_LINKANY_H
#define BACKEND_B2_LINKANY_H

#include <stdint.h>

struct idCmdArgs;

/* sh_target_any: toggle the link-any mode (1st call writes the instance-filter lever to 1, 2nd to 0). */
void h_link_any(struct idCmdArgs *a);

/* Cache the DOOM module base (one-shot) so h_link_any can resolve the creator sig + decode the lever at
 * FIRE. Installs NOTHING (off by default). The handler is registered by sh_commands' CMD_TABLE. */
void sh_linkany_install(const uint8_t *module_base);

#endif /* BACKEND_B2_LINKANY_H */
