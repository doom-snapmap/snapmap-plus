/* Native map transport accepts positive lengths representable by idStr. */
#ifndef SH_MAP_PAYLOAD_H
#define SH_MAP_PAYLOAD_H
#include "signatures.h"

/* Bind save and saved/published read repairs together before maps can open or
 * save. No author setting controls this repair. Failed writes roll back both. */
int sh_map_payload_install(const sig_result *results, size_t count);
int sh_map_payload_remove(void);
#endif
