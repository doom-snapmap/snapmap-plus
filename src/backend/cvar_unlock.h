/* Cvar-system layout used by the backend startup unlock worker. The cvar
 * system itself is located through the globals table (cvar_system_slot). */
#ifndef B2_CVAR_UNLOCK_H
#define B2_CVAR_UNLOCK_H

#include <stdint.h>
#include "engine_build.h"

/* FindCvar uses the full table at gate 0 and the developer table otherwise; each is an embedded
 * idList<idCVar*> plus idHashIndex. */
#define CVARSYS_FULL_LIST_OFF   0x08u
#define CVARSYS_FULL_HASH_OFF   0x38u
#define CVARSYS_DEV_LIST_OFF    0x20u
#define CVARSYS_DEV_HASH_OFF    0x60u
#define SIZEOF_IDLIST           0x18u
#define SIZEOF_IDHASHINDEX      0x28u

/* idList and idHashIndex fields, relative to the embedded header. */
#define IDLIST_NUM_OFF          0x08u   /* int32 count; capacity follows at +0x0C */
#define IDHASH_HASH_SIZE_OFF    0x10u   /* int32 bucket count; mask at +0x1C, lookup mask at +0x20 */

/* Full-list fields, relative to cvarSys; the sanity max refuses an absurd count. */
#define CVARSYS_LIST_ARRAY_OFF  0x08u
#define CVARSYS_LIST_COUNT_OFF  0x10u
#define CVAR_LIST_SANITY_MAX    100000u

/* idCVar flags: SH_IDCVAR_FLAGS (engine_build.h). */
#define CVAR_FLAG_NOCHEAT       0x10u      /* dev-set-exempt: idCVar::Set FATALs at gate!=0 WITHOUT it */
#define CVAR_FLAG_EXPOSE        0x200000u  /* developer-table membership (engine normalizes NOCHEAT->EXPOSE) */


/* Start the deferred unlock worker from backend process attachment. */
void sh_cvar_unlock_start(void);

#endif /* B2_CVAR_UNLOCK_H */
