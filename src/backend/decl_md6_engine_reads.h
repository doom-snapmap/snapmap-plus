/* Actual common/engine context routes used by native declaration parsing. */
#ifndef SH_DECL_MD6_ENGINE_READS_H
#define SH_DECL_MD6_ENGINE_READS_H
#include "resource_default_reads.h"
enum {SH_DECL_ENGINE_READ_80=1,SH_DECL_ENGINE_READ_90=2};
/* Pure same-invocation state1/unsupported predicates; never call an engine
 * virtual, construct a singleton, skip native work, or complete a receipt.
 * Host TLS and existing kernel32 export metadata are read-only. The normal
 * native return/source observation remains the caller's completion authority. */
int sh_decl_engine_read_check(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input,unsigned methods);
int sh_decl_md6_engine_read_check(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input);
#endif
