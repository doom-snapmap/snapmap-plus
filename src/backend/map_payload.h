/* Native map transport accepts positive lengths representable by idStr. */
#ifndef SH_MAP_PAYLOAD_H
#define SH_MAP_PAYLOAD_H
#include "signatures.h"

/* Bind save and saved/published read repairs together before maps can open or
 * save. No author setting controls this repair. Failed writes roll back both. */
int sh_map_payload_install(const sig_result *results, size_t count);
int sh_map_payload_remove(void);
/* The install's preflight: both sites resolved and holding the native bytes the repair expects. NULL when they do,
 * else the name of the first that does not; sites[] receive the patch addresses. The install and the image test run it. */
const char *sh_map_payload_check_sites(const sig_result *results, size_t count, sig_result sites[2]);
#endif
