#ifndef SH_DECL_RENDERPROG_RELEASE_READS_H
#define SH_DECL_RENDERPROG_RELEASE_READS_H
#include "resource_default_reads.h"
/* Inspect the actual release dispatch, GPU interface and reached owned
 * storage. Never destroys a handle, changes storage, or completes a default. */
int sh_decl_renderprog_release_read_check(sh_resource_default_read_receipt *receipt,
                                        const sh_resource_default_read_input *input);
#endif
