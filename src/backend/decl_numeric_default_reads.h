/* Native numerical declaration default read and cleanup predicates. */
#ifndef SH_DECL_NUMERIC_DEFAULT_READS_H
#define SH_DECL_NUMERIC_DEFAULT_READS_H
#include "resource_default_reads.h"
/* Continues the same state1/unsupported invocation. This only inspects actual
 * native state; it neither invokes the default nor changes native behavior.
 * The caller consumes the resulting receipt through the existing finish API. */
int sh_decl_numeric_default_read_begin(sh_resource_default_read_receipt *receipt,
                                      const sh_resource_default_read_input *input);
#endif
