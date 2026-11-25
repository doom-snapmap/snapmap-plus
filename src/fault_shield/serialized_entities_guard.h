#ifndef SH_SERIALIZED_ENTITIES_GUARD_H
#define SH_SERIALIZED_ENTITIES_GUARD_H

#include "../backend/signatures.h"

/* Install during backend startup, before map execution; the caller must not race an engine list walk. */
int sh_serialized_entities_guard_install(const sig_result *results, size_t count);

/* 1 when the resolved site holds the index write the guard replaces. */
int sh_serialized_entities_site_ok(const uint8_t *site);

#endif
