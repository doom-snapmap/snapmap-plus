/* Published readers inspect; native launch requests own package preflight. */
#ifndef BACKEND_MAP_PUBLISHED_H
#define BACKEND_MAP_PUBLISHED_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Read-only signature/layout binding, also exercised against both mapped PEs. */
int sh_map_published_bind(const sig_result *results, size_t count, const uint8_t *base);
/* Install the reader and four launch gates as one group. Partial publication
 * stays in pass-through mode until all original callbacks are available. */
int sh_map_published_install(const sig_result *results, size_t count, const uint8_t *base);
/* Retry owner cleanup on the native main thread, never from a download worker. */
void sh_map_published_poll(void);
#endif
