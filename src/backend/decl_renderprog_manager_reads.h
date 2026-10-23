#pragma once
#include "resource_default_reads.h"
/* Current native Create/Include, permutation unlink/re-find and cooked-sampler
 * manager storage only. Re-find also requires the caller's native load/parse,
 * GenericReload, deleting-resource and engine-context contracts.
 * Requires the caller's owned DeclFind and actual resource-return observers,
 * file/Source/allocator/diagnostic contracts. Never completes a receipt. */
int sh_decl_renderprog_manager_read_check(sh_resource_default_read_receipt *receipt,
                                          const sh_resource_default_read_input *input);

