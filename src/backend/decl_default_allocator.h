/* Actual allocator interface used by native declaration defaults. */
#ifndef SH_DECL_DEFAULT_ALLOCATOR_H
#define SH_DECL_DEFAULT_ALLOCATOR_H
#include "resource_default_reads.h"
int sh_decl_default_allocator_read_check(sh_resource_default_read_receipt *receipt,
                                         const sh_resource_default_read_input *input);
/* Generic SourceParser LoadMemory uses native operator new. Validate its
 * actual recovery handler and the separate malloc/calloc new-mode boundary.
 * This never completes the receipt or changes native allocator behavior. */
int sh_decl_default_crt_allocation_read_check(sh_resource_default_read_receipt *receipt,
                                              const sh_resource_default_read_input *input);
/* The caller proves construction by native parser UnreadToken/define clone.
 * This validates that constructed type's actual static deleting-destructor
 * dispatch; it never certifies arbitrary token objects or completes a receipt. */
int sh_decl_default_token_read_check(sh_resource_default_read_receipt *receipt,
                                     const sh_resource_default_read_input *input);
#endif
