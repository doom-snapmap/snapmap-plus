#ifndef SH_RESOURCE_METADATA_READS_H
#define SH_RESOURCE_METADATA_READS_H
#include <stdint.h>
/* Read-only native metadata contract. Binding installs no hook or barrier. */
int sh_resource_metadata_reads_bind(const uint8_t *base);
int sh_resource_metadata_reads_ready(void);
#endif
