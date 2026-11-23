#ifndef SH_SERIALIZED_ENTITIES_GUARD_H
#define SH_SERIALIZED_ENTITIES_GUARD_H

#include "../backend/signatures.h"

/* Install during backend startup, before map execution. The caller must not
 * race an engine list walk. No address fallback or runtime setting is used. */
int sh_serialized_entities_guard_install(const sig_result *results, size_t count);

/* The install's own byte check at the resolved instruction, for the build-image test: 1 when the site holds the
 * index write the guard replaces. */
int sh_serialized_entities_site_ok(const uint8_t *site);

#endif
