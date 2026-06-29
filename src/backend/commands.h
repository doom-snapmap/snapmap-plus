/* commands.h -- clone of SnapHak's console-COMMAND surface, native C
 * (port of OG XINPUT1_3 FUN_1800229b1's AddCommand spine + the per-command handlers).
 *
 * OG registers 22 console commands with the engine cmd system via idCmdSystemLocal::AddCommand
 * (engine 0x1AA3630), each dispatching to a SnapHak handler. We collapse OG's static-init std::vector
 * machinery to one install fn that calls the engine AddCommand directly per command -- no hardcoded
 * RVAs (the engine fns are signature-resolved by the signature resolver; the idCmdSystemLocal* global is
 * decoded build-portably from a sig'd accessor's RIP-relative MOV, the same LEA-decode idiom sh_strids
 * uses for its table global).
 *
 * All 22 command NAMES are registered (Tier B/C handlers are faithful "not yet implemented in
 * clone" stubs that print the OG help). The trivial handlers wire to the ALREADY-SHIPPED
 * ops: snapHak_rawmaps_on/off -> sh_rawmap_swap_arm; sh_target_any -> sh_unhide_apply. Later tranches
 * fill the decl/entity/type handlers.
 *
 * ABI (DIRECT, evidence/b2-decompiles/{engine_0x1aa3630.txt,xreg_0x229b1.txt,cvar_spine_flush_disasm.txt}):
 *   AddCommand(cmdsys, name, handler, help, argComp, flags)   -- we pass argComp=NULL, flags=0.
 *   handler is __fastcall(idCmdArgs*); argc @ +0x00, argv[N] @ +0x08 + 8*N.
 *   console output goes through idCommon dispatch (Printf sig 0x1A08E80) via the OG wrapper form
 *   (*(engineBase+0x1a08e80))(1, fmt, &va) -- we pre-format then call (1, "%s", &bufptr).
 *
 * Clean-room: ported from our own RE (the engine register-ABI is documented in the
 * AddCommand ABI block above). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B2_COMMANDS_H
#define BACKEND_B2_COMMANDS_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* ---- shared console-handler ABI + utilities (commands.c owns; entity.c reuses) -------------
 * The OG console-handler callback ABI (__fastcall, arg0 only -- uniform across the OG handler bodies):
 *   idCmdArgs { int argc; char _pad[4]; const char* argv[1]; }   (argv[N] @ +0x08 + 8*N).
 * entity.c's moved handlers (sh_spawn/sh_dumpdef/sh_spawninfo) use the SAME struct + the SAME
 * SEH-guarded accessors + the SAME Printf wrapper, so we share them here rather than duplicate-and-drift. */
typedef struct idCmdArgs {
    int          argc;
    char         _pad[4];
    const char  *argv[1];
} idCmdArgs;

/* SEH-guarded idCmdArgs accessors (the engine hands us the args object; never trust its shape). */
int         cmd_argc(idCmdArgs *a);
const char *cmd_argv(idCmdArgs *a, int n);

/* Console output via the resolved engine Printf dispatch (no-op until sh_commands_install caches it). */
void sh_printf(const char *fmt, ...);

/* ---- the shared build-portable global-decode scanner (commands.c owns; entity.c reuses) -----
 * sh_decode_rip_slot scans a sig'd accessor fn's first B2_RIP_SCAN_WINDOW bytes for the FIRST of the
 * four RIP-relative MOV/LEA decode-target opcodes (48 8B 0D / 48 8B 05 / 48 8D 0D / 48 8D 05) and
 * returns the global SLOT address (rip_next + disp32), NOT the dereferenced object. sh_resolve_cmdsys
 * (CmdSystemLea) and sh_resolve_gamemgr (GameMgrLea) BOTH decode through this one helper. sh_safe_read
 * is the SEH-guarded byte copy both decode + deref paths use. */
#define B2_RIP_SCAN_WINDOW 0x40
int            sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n);
const uint8_t *sh_decode_rip_slot(const uint8_t *accessor_fn);

/* Resolve the idCmdSystemLocal* global build-portably from the CmdSystemLea accessor (sig "CmdSystemLea",
 * the engine bot_add/bot_remove registrar): decode its first RIP-relative load opcode
 * (48 8D 0D / 48 8B 0D / 48 8D 05 / 48 8B 05 -- the cmdSystem one is the MOV form 48 8B 0D) to the
 * cmdSystem-global SLOT, then dereference ONCE to get the live object. Returns the live cmdSystem object
 * pointer, or NULL on any failure (logged; the command install then SKIPs gracefully).
 *   results / n   = the resolved signature DB (from sig_resolve_all in dllmain).
 *   module_base   = the DOOM module base (for the *(base+0x55b7280) known-offset FALLBACK).
 */
void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base);

/* Install the 22 console commands. Idempotent (one-shot latch). Guards add_command && cmdsys && printf;
 * logs a SKIPPED reason and returns 0 on any missing dependency. Each engine call is SEH-guarded.
 *   add_command   = resolved engine AddCommand (sig "AddCommand"; 0 => SKIPPED).
 *   cmdsys        = the idCmdSystemLocal* from sh_resolve_cmdsys (NULL => SKIPPED; commands cannot FIRE).
 *   printf_disp   = resolved engine idCommon dispatch (sig "Printf"; 0 => handlers can't print, SKIPPED).
 *   get_decls     = resolved engine GetDeclsOfType (sig "GetDeclsOfType"); cached for sh_target_any's
 *                   sh_unhide_apply call. May be 0 (sh_target_any then logs it can't run).
 *   module_base   = the DOOM module base; cached so the devmode [15][16] handlers resolve the
 *                   SessionDevModeGetter signature at FIRE (the engine-code patch site) off it.
 * Returns the number of commands registered (0 on SKIP). Emits "B2: registered N/22 console commands".
 */
int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base);

#endif /* BACKEND_B2_COMMANDS_H */
