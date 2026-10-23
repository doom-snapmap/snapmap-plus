/* Per-invocation custom declaration default read predicates. */
#ifndef SH_DECL_CUSTOM_DEFAULT_READS_H
#define SH_DECL_CUSTOM_DEFAULT_READS_H
#include "resource_default_reads.h"
/* Predicate only for the known token type constructed by native parser
 * UnreadToken/define clone. Caller proves that construction provenance and its
 * reached lexer route; this never certifies arbitrary token objects or marks
 * the receipt complete. */
int sh_decl_custom_default_token_read_check(sh_resource_default_read_receipt *receipt,
                                             const sh_resource_default_read_input *input);
int sh_decl_custom_default_read_begin(sh_resource_default_read_receipt *receipt,
                                     const sh_resource_default_read_input *input);
#endif
