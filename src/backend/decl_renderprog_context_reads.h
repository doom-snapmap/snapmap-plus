#ifndef SH_DECL_RENDERPROG_CONTEXT_READS_H
#define SH_DECL_RENDERPROG_CONTEXT_READS_H
#include "resource_default_reads.h"
/* Existing context storage only. It never resolves stored handles, executes
 * native callbacks, or qualifies the enclosing parser by itself. */
int sh_decl_renderprog_context_read_check(sh_resource_default_read_receipt *receipt,
                                         const sh_resource_default_read_input *input);
#endif
