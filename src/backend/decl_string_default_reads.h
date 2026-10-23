/* Per-invocation native string-owned declaration default contracts. */
#ifndef SH_DECL_STRING_DEFAULT_READS_H
#define SH_DECL_STRING_DEFAULT_READS_H
#include "resource_default_reads.h"
int sh_decl_string_default_read_begin(sh_resource_default_read_receipt *receipt,
                                      const sh_resource_default_read_input *input);
#endif
