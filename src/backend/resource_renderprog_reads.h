/* Actual direct inputs reached by the paired RenderProg parser. */
#ifndef SH_RESOURCE_RENDERPROG_READS_H
#define SH_RESOURCE_RENDERPROG_READS_H
#include <stdint.h>
#include <stddef.h>
/* Synchronous actual-origin capture; NULL reports an unknown reached input.
 * The callback must not call native code or mutate/reenter this module. */
int sh_resource_renderprog_reads_install(const uint8_t *base,int (*observe)(void *));
int sh_resource_renderprog_reads_ready(void);
/* Existing DeclFind hook invokes this only at a real normal return. */
void sh_resource_renderprog_reads_find_return(void *resource,uintptr_t caller);
#endif
