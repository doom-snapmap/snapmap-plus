#ifndef SH_DECL_SOURCE_FILE_READS_H
#define SH_DECL_SOURCE_FILE_READS_H
#include "resource_default_reads.h"
/* Pure same-invocation adjunct; never runs native code or completes receipt.
 * Actual returned include streams are checked before their native caller
 * resumes, through the existing provider opening/receipt owner. */
int sh_decl_source_file_read_check(sh_resource_default_read_receipt *receipt,
                                  const sh_resource_default_read_input *input);
/* Full RenderProg adjunct adds cooked ReadString, backend whole-buffer input,
 * include metadata, GenericReload Seek70/C8/D8 and the proved native output factory/effect route. */
int sh_decl_renderprog_file_read_check(sh_resource_default_read_receipt *receipt,
                                      const sh_resource_default_read_input *input);
/* Install after the existing override provider is armed. Readiness validates
 * current receiver/slot/observer ownership, never a saved ready flag alone. */
int sh_decl_source_file_reads_install(const unsigned char *base);
int sh_decl_source_file_reads_ready(void);
int sh_decl_source_file_reads_uninstall(void);
#endif
