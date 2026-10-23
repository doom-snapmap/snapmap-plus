/* Native AnimWeb declaration default ownership/read predicate. */
#ifndef SH_DECL_ANIMWEB_DEFAULT_READS_H
#define SH_DECL_ANIMWEB_DEFAULT_READS_H
#include "resource_default_reads.h"
/* Inspect the same state1/unsupported invocation. Native execution and receipt
 * consumption remain with the caller and the existing finish API. */
int sh_decl_animweb_default_read_begin(sh_resource_default_read_receipt *receipt,
                                      const sh_resource_default_read_input *input);
#endif
