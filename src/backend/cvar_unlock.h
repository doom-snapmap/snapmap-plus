/* Cvar-system layout used by the backend startup unlock worker.
 * Struct offsets describe the supported engine layout. Pinned Vulkan RVAs are
 * fallbacks; runtime resolution normally decodes the CmdSystemLea signature. */
#ifndef B2_CVAR_UNLOCK_H
#define B2_CVAR_UNLOCK_H

#include <stdint.h>

/* Pinned Vulkan cvar-system pointer slot. To rederive it, decode CmdSystemLea
 * to the command-system slot and add 0x10. The runtime has the same signature path. */
#define RVA_CVAR_SYSTEM_PTR  0x55b7290u

/* Pinned Vulkan command-system pointer slot, decoded from CmdSystemLea.
 * Its object exposes getGate/setGate at vtable +0x10/+0x18. */
#define RVA_CMD_SYSTEM_PTR   0x55b7280u

/* FindCvar uses the full table at gate 0 and the developer table otherwise.
 * Each table combines an embedded idList and idHashIndex. */
#define CVARSYS_FULL_LIST_OFF   0x08u   /* idList<idCVar*>, full table */
#define CVARSYS_FULL_HASH_OFF   0x38u   /* idHashIndex,     full table */
#define CVARSYS_DEV_LIST_OFF    0x20u   /* idList<idCVar*>, developer table */
#define CVARSYS_DEV_HASH_OFF    0x60u   /* idHashIndex,     developer table */
#define SIZEOF_IDLIST           0x18u   /* Embedded idList header size. */
#define SIZEOF_IDHASHINDEX      0x28u   /* Embedded idHashIndex header size. */

/* Full-list fields, relative to cvarSys. */
#define CVARSYS_LIST_ARRAY_OFF  0x08u   /* idCVar** -- 8-byte entries */
#define CVARSYS_LIST_COUNT_OFF  0x10u   /* uint32 element count */
#define CVAR_LIST_SANITY_MAX    100000u /* refuse an absurd count (uninitialized / wrong build) */

/* idCVar object */
#define CVAR_FLAGS_OFF          0x58u
#define CVAR_FLAG_NOCHEAT       0x10u      /* dev-set-exempt: idCVar::Set FATALs at gate!=0 WITHOUT it */
#define CVAR_FLAG_EXPOSE        0x200000u  /* developer-table membership (engine normalizes NOCHEAT->EXPOSE) */


/* Start the deferred unlock worker from backend process attachment. */
void sh_cvar_unlock_start(void);

#endif /* B2_CVAR_UNLOCK_H */
