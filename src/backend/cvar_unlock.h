/* Cvar-system layout used by the backend startup unlock worker.
 * Struct offsets describe the supported engine layout; the cvar system itself
 * is found by decoding the CmdSystemLea signature. */
#ifndef B2_CVAR_UNLOCK_H
#define B2_CVAR_UNLOCK_H

#include <stddef.h>
#include <stdint.h>

/* FindCvar uses the full table at gate 0 and the developer table otherwise.
 * Each table combines an embedded idList and idHashIndex. */
#define CVARSYS_FULL_LIST_OFF   0x08u   /* idList<idCVar*>, full table */
#define CVARSYS_FULL_HASH_OFF   0x38u   /* idHashIndex,     full table */
#define CVARSYS_DEV_LIST_OFF    0x20u   /* idList<idCVar*>, developer table */
#define CVARSYS_DEV_HASH_OFF    0x60u   /* idHashIndex,     developer table */

/* Full-list fields, relative to cvarSys. */
#define CVARSYS_LIST_ARRAY_OFF  0x08u   /* idCVar** -- 8-byte entries */
#define CVARSYS_LIST_COUNT_OFF  0x10u   /* uint32 element count */
#define CVAR_LIST_SANITY_MAX    100000u /* refuse an absurd count (uninitialized / wrong build) */

/* idCVar object */
#define CVAR_FLAGS_OFF          0x58u
#define CVAR_FLAG_NOCHEAT       0x10u      /* dev-set-exempt: the setter refuses at gate!=0 WITHOUT it unless Dev Mode is on */
#define CVAR_FLAG_EXPOSE        0x200000u  /* developer-table membership (engine normalizes NOCHEAT->EXPOSE) */


/* Start the deferred unlock worker from backend process attachment. */
void sh_cvar_unlock_start(void);

/* Every cvar-system function picks its table from the command system's gate
 * (vtable +0x10): a mov of the developer offset (list 0x20, hash 0x60) that
 * CMOVZ replaces with the full offset (list 0x08, hash 0x38) at gate 0. Both
 * renderer images carry the same eleven selections. Rewrite each developer
 * immediate in a readable .text span to its full value, list selections before
 * hash selections; a site already holding its full value is left alone. Return
 * the site count; 0 without writing when a site is absent or holds another
 * immediate; -1 without writing when a site matches more than once, or after
 * undoing the writes of a failed rewrite. unproven, if not NULL, receives a bit
 * for each site not matched exactly once. */
#define SH_CVAR_UNLOCK_GATE_SITES 11
int sh_cvar_unlock_rewrite_gates(uint8_t *text, size_t size, unsigned *unproven);

#endif /* B2_CVAR_UNLOCK_H */
