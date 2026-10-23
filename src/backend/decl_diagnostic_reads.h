/* Reached native diagnostic receiver read predicate. */
#ifndef SH_DECL_DIAGNOSTIC_READS_H
#define SH_DECL_DIAGNOSTIC_READS_H
#include "resource_default_reads.h"
/* Inspect severity1/info or severity4/warning on the same state1/unsupported
 * invocation. Reuses native envelope checks; never completes the receipt,
 * executes native code, changes severity or suppresses a diagnostic. */
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *receipt,
                                  const sh_resource_default_read_input *input,
                                  unsigned severity);
/* Shared native output transport factory/mount/client contract. This does not
 * emit diagnostics or mark file bytes as input. The caller proves its actual
 * write-only operation and checks its additional filesystem dispatch slots. */
int sh_decl_file_output_factory_read_check(sh_resource_default_read_receipt *receipt,
                                          const sh_resource_default_read_input *input);
typedef int (*sh_decl_file_client_read_inspector)(void *context,
    sh_resource_default_read_receipt *receipt,const sh_resource_default_read_input *input,
    uintptr_t actual_client);
/* Uses the same finite registry traversal. The input consumer additionally
 * qualifies each actual cached client's reached read dispatch. Borrowed only;
 * the inspector must not execute native code or complete the receipt. Caller
 * composes its own envelope/allocator contract; this helper never recurses. */
int sh_decl_file_factory_read_check(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input,sh_decl_file_client_read_inspector inspector,void *context);
#endif
