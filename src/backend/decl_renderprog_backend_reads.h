#pragma once
#include "resource_default_reads.h"
/* Qualify the native backend code and current GPU dispatch bindings. This is
 * a state-1 adjunct: the caller still owns Source/file/allocator/diagnostic
 * composition, context lineage, actual resource-read observation and finish. */
int sh_decl_renderprog_backend_read_check(sh_resource_default_read_receipt *receipt,
                                         const sh_resource_default_read_input *input);
