/* Read contracts at the native MD6 final rig refresh. */
#ifndef SH_DECL_MD6_RIG_READS_H
#define SH_DECL_MD6_RIG_READS_H
#include "resource_default_reads.h"

/* Pure same-invocation state1/unsupported continuations. Neither executes
 * native code or completes the receipt. The caller requires the current owned
 * final-wrapper observer: it validates the original stolen16 on installation
 * and the live exact detour; these predicates pin the untouched continuation.
 * Run both immediately before the native wrapper, with its actual MD6 object.
 * Bound inputs are the actual current resource+60/model+138 values, observed
 * with their existing lifetime tokens by the enclosing resource observer. */
int sh_decl_md6_rig_read_check(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input);
int sh_decl_md6_rig_bound_read_check(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input, uintptr_t actual_model,
    uintptr_t actual_skeleton);
#endif
