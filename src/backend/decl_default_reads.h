/* Per-invocation contracts for the native declaration default. */
#ifndef SH_DECL_DEFAULT_READS_H
#define SH_DECL_DEFAULT_READS_H
#include "resource_default_reads.h"
/* Generic MakeDefault envelope only, including actual manager/text leaf and
 * initialized lexer/validator context. Decodes the text pointer when requested.
 * The caller proves the literal, reached parser and cleanup; this never marks
 * a receipt complete and makes no claim about identifier macro expansion. */
int sh_decl_default_envelope_read_check(sh_resource_default_read_receipt *receipt,
                                        const sh_resource_default_read_input *input,
                                        uintptr_t *text);
/* Shared fresh-body predicate only. The caller separately proves its actual
 * parser and cleanup before setting a complete, single-use receipt. */
int sh_decl_default_common_body_read_check(sh_resource_default_read_receipt *receipt,
                                          const sh_resource_default_read_input *input);
/* The caller proves the fixed literal's actual name-token path. Reject exact
 * global-define shadows; native lookup is case sensitive. This checks the
 * complete live list without a name-length or record-count cutoff and never
 * marks a receipt complete. The supplied names are valid, finite C strings. */
int sh_decl_default_names_unshadowed(sh_resource_default_read_receipt *receipt,
                                     const sh_resource_default_read_input *input,
                                     const char *const *names,size_t count);
int sh_decl_default_read_begin(sh_resource_default_read_receipt *receipt,
                              const sh_resource_default_read_input *input);
#endif
