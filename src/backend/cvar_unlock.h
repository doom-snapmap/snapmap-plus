/* engine_layout.h -- DOOM 2016 cvar-system memory layout for the clean-room cvar-unlock.
 *
 * All RVAs are image-base-relative (DOOMx64vk.exe ImageBase 0x140000000); the loader adds the
 * runtime base from GetModuleHandle. All OFFSETS are struct-relative and version-stable.
 *
 * BUILD-LOCKED: the RVAs target the pinned demoted DOOM depot.
 * Re-derive the RVAs on a DOOM patch (the offsets are stable; only the singleton-pointer RVAs move).
 * The per-build RE-DERIVE recipe for the two singleton-pointer RVAs is written inline below (RVA_CVAR_SYSTEM_PTR
 * / RVA_CMD_SYSTEM_PTR: decode the CmdSystemLea sig's RIP-relative load to the cmdSystem .data slot, then
 * cvarSys == cmdSystem + 0x10). The backend XINPUT1_3 ALREADY resolves cvarSys this way at runtime
 * (cvars.c sh_resolve_cvarsys -- CmdSystemLea decode +0x10, base+RVA fallback). This standalone dinput8
 * proxy does NOT yet carry a sig layer of its own -- adding one is the genuine remaining portability work
 * item (NOT a runtime liability: these RVAs are
 * build-locked-with-recipe per the portability discipline).
 *
 * PROVENANCE (all DIRECT decompiles):
 *   - the +cvar boot-arg apply mechanism (FindCvar's gate-keyed
 *     two-table lookup FUN_141a05a90, idCVar::Set's dev-gate guard FUN_141a06e80).
 *   - the gate-0/gate-1 tables (CVAR_NOCHEAT/CVAR_EXPOSE).
 *   - the reference implementation unlockCvars/setCvarsSettable (the live-validated 2026-06-14 mechanism this
 *     header's constants are taken from verbatim).
 */
#ifndef B2_CVAR_UNLOCK_H
#define B2_CVAR_UNLOCK_H

#include <stdint.h>

#define DOOM_MODULE_NAME "DOOMx64vk.exe"

/* idCvarSystem singleton POINTER: a DATA global holding the cvarSys object's address.
 * cvarSys = *(imageBase + RVA_CVAR_SYSTEM_PTR).
 * BUILD-SPECIFIC literal. RE-DERIVE on a new DOOM build (auto-patcher / version bump): the two .data slots
 * below are adjacent -- cvarSys == cmdSystem + 0x10. Re-find cmdSystem the way the backend does: the
 * CmdSystemLea signature (the backend signature resolver) anchors the RIP-relative `lea rXX,[cmdSystem]`
 * in a cvar/cmd accessor; decode its disp32 to the .data-slot RVA, then RVA_CVAR_SYSTEM_PTR = that + 0x10.
 * (Standalone dinput8 has no sig layer yet -- portability hardening.) */
#define RVA_CVAR_SYSTEM_PTR  0x55b7290u

/* idCmdSystem singleton POINTER: holds the developer "exec gate" that FindCvar/Set key off.
 * Its vtable: +0x10 = getGate(), +0x18 = setGate(int). (Used only by the verify helper.)
 * BUILD-SPECIFIC literal. RE-DERIVE: == the backend CmdSystemLea-decoded cmdSystem .data slot (see above);
 * RVA_CVAR_SYSTEM_PTR = this + 0x10. */
#define RVA_CMD_SYSTEM_PTR   0x55b7280u

/* idCvarSystem two-table layout. FindCvar (FUN_141a05a90) selects the table by the dev-gate:
 *   gate == 0 -> FULL table (every registered cvar)
 *   gate != 0 -> DEV  table (only CVAR_EXPOSE cvars) -- what the ~console AND the startup +cvar apply use.
 * Each "table" is an embedded {idList, idHashIndex} pair on the cvarSys object. */
#define CVARSYS_FULL_LIST_OFF   0x08u   /* idList<idCVar*>, full table */
#define CVARSYS_FULL_HASH_OFF   0x38u   /* idHashIndex,     full table */
#define CVARSYS_DEV_LIST_OFF    0x20u   /* idList<idCVar*>, developer table */
#define CVARSYS_DEV_HASH_OFF    0x60u   /* idHashIndex,     developer table */
#define SIZEOF_IDLIST           0x18u   /* embedded idList struct size (the reference implementation: readByteArray(0x18)) */
#define SIZEOF_IDHASHINDEX      0x28u   /* embedded idHashIndex struct size (the reference implementation: readByteArray(0x28)) */

/* Inside the FULL idList (at cvarSys + CVARSYS_FULL_LIST_OFF): the idCVar* array pointer and the count.
 * the reference implementation setCvarsSettable: arr = *(cvarSys+0x08); count = *(uint32*)(cvarSys+0x10). */
#define CVARSYS_LIST_ARRAY_OFF  0x08u   /* idCVar** -- 8-byte entries */
#define CVARSYS_LIST_COUNT_OFF  0x10u   /* uint32 element count */
#define CVAR_LIST_SANITY_MAX    100000u /* refuse an absurd count (uninitialized / wrong build) */

/* idCVar object */
#define CVAR_FLAGS_OFF          0x58u
#define CVAR_FLAG_NOCHEAT       0x10u      /* dev-set-exempt: idCVar::Set FATALs at gate!=0 WITHOUT it */
#define CVAR_FLAG_EXPOSE        0x200000u  /* developer-table membership (engine normalizes NOCHEAT->EXPOSE) */


/* Backend entry (merged from the former standalone dinput8 cvar-unlock; bundle minimization).
 * Spawns the deferred cvar-unlock thread from the backend DllMain (DLL_PROCESS_ATTACH). */
void sh_cvar_unlock_start(void);

#endif /* B2_CVAR_UNLOCK_H */
