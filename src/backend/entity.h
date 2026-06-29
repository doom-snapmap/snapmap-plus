/* entity.h -- the entity/spawn console commands (sh_dumpdef, sh_spawninfo,
 * sh_spawn), native C. Ports of OG XINPUT1_3 FUN_180021e60 (sh_dumpdef), FUN_180024d90 (sh_spawninfo),
 * FUN_180021c90 (sh_spawn).
 *
 * These three commands need engine state the rest of the command surface does not: the gameMgr (game/entity
 * manager) global, plus three vtable-slot virtual calls (cmdSystem +0x48 ExecuteCommandText, gameMgr
 * +0x498 FindEntity, idEntity +0x340 GetOrigin) and -- for sh_spawn -- SpawnByEntityDef@0x315AF0.
 *
 * Build portability (the reference-entity-layout trap):
 *   - gameMgr is decoded build-portably from the GameMgrLea accessor's RIP-relative MOV (reusing
 *     sh_commands' sh_decode_rip_slot), with a *(base+0x56ffb90) known-offset FALLBACK.
 *   - the vtable slot INDICES (+0x48/+0x498/+0x340) are decompile-verified but NOT in the source-of-
 *     record -- GetOrigin@+0x340 especially is OG-2021 evidence only, so sh_spawn GUARDS its teleport
 *     against a bogus vec3 (NaN or |coord|>1e6 -> skip the teleport, print a warning). Every engine
 *     deref is SEH-guarded; a wrong offset degrades to a clean no-op + log, never a crash/teleport-to-
 *     garbage.
 *   - the sh_dumpdef ent+0x6d0 -> +0x140 resolved-def-text chain is source-of-record-confirmed
 *     (idlib_schema.json) but build-sensitive -> SEH-guarded + non-null gated.
 *
 * Clean-room: ported from our own RE (the b2-t3-re foundation + the OG command decompiles). Zero OG bytes.
 */
#ifndef BACKEND_B2_ENTITY_H
#define BACKEND_B2_ENTITY_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Resolve the gameMgr (game/entity manager) global SLOT build-portably from the GameMgrLea accessor (sig
 * "GameMgrLea"): decode its first RIP-relative MOV opcode (48 8B 05 at byte offset 0) to the gameMgr-
 * global SLOT via the SHARED sh_decode_rip_slot scanner. Returns the SLOT (the address of the pointer),
 * NOT the deref'd object -- the idGameLocal manager is constructed only when a game/map loads, so the
 * value is NULL at our early deferred-install time; the handlers deref the slot LAZILY at invocation.
 * Returns the slot, or NULL on any failure (logged). FALLBACK on decode failure: module_base + 0x56ffb90.
 *   results / n   = the resolved signature DB (from sig_resolve_all in dllmain).
 *   module_base   = the DOOM module base (for the base+0x56ffb90 known-offset FALLBACK).
 */
const uint8_t *sh_resolve_gamemgr_slot(const sig_result *results, size_t n, const uint8_t *module_base);

/* Install the entity/spawn handler dependencies. Idempotent (one-shot latch). Resolves +
 * caches gameMgr (via sh_resolve_gamemgr), cmdSystem (passed in -- already decoded by sh_commands), and
 * SpawnByEntityDef (sig). Each handler then runs on console invocation using the cached deps; a missing
 * dep makes that handler print a graceful "unresolved" line rather than crash. Emits a
 * "B2: entity install ..." marker. Call AFTER sh_commands_install (the handlers are already registered
 * by the sh_commands CMD_TABLE; this only wires their engine deps).
 *   results / n   = the resolved signature DB.
 *   module_base   = the DOOM module base (gameMgr FALLBACK).
 *   cmdsys        = the idCmdSystemLocal* already decoded by sh_resolve_cmdsys (reused for the +0x48
 *                   ExecuteCommandText virtual call; may be NULL -> sh_spawn/sh_spawninfo log + skip).
 * Returns 1 on a successful install pass (even with some deps NULL -- the handlers degrade), 0 if it
 * was already installed (latch) or the module base is NULL.
 */
int sh_entity_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys);

#endif /* BACKEND_B2_ENTITY_H */
