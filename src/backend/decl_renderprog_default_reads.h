#ifndef SH_DECL_RENDERPROG_DEFAULT_READS_H
#define SH_DECL_RENDERPROG_DEFAULT_READS_H
#include "resource_default_reads.h"
/* Same-invocation native shader default composition. Actual resource reads
 * are retained by the current native observer frame, never inferred by name. */
int sh_decl_renderprog_default_read_begin(sh_resource_default_read_receipt *receipt,
                                         const sh_resource_default_read_input *input);
#endif
