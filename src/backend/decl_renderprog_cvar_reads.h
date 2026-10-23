#ifndef SH_DECL_RENDERPROG_CVAR_READS_H
#define SH_DECL_RENDERPROG_CVAR_READS_H
#include "resource_default_reads.h"
/* Validate actual shader cvar reads in this invocation. This does not mark
 * the enclosing default complete or execute any native callback. */
int sh_decl_renderprog_cvar_read_check(sh_resource_default_read_receipt *receipt,
                                     const sh_resource_default_read_input *input);
#endif
