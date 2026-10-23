#ifndef SH_DECL_SOURCE_PREPROCESS_READS_H
#define SH_DECL_SOURCE_PREPROCESS_READS_H
#include "resource_default_reads.h"
/* Adjunct for constructor-owned Source/Script objects on the pinned native
 * path. The caller establishes that lineage and observes actual resource reads;
 * this does not inspect a future stack Source, execute the parser, impose a
 * macro/include-name allowlist, or complete the enclosing receipt. */
int sh_decl_source_preprocess_read_check(sh_resource_default_read_receipt *receipt,
                                        const sh_resource_default_read_input *input);
#endif
