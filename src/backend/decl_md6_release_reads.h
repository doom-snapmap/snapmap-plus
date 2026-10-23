#ifndef SH_DECL_MD6_RELEASE_READS_H
#define SH_DECL_MD6_RELEASE_READS_H
#include "resource_default_reads.h"
/* Same active state1/unsupported invocation; qualify the exact reached native
 * MD6 Release ownership/read contract. Never execute native code or complete
 * the receipt. The caller separately qualifies its parser and actual finish. */
int sh_decl_md6_release_read_check(sh_resource_default_read_receipt *receipt,
                                  const sh_resource_default_read_input *input);
#endif
