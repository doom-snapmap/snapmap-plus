#ifndef SH_SERIALIZED_ENTITIES_GUARD_H
#define SH_SERIALIZED_ENTITIES_GUARD_H

#include "../backend/signatures.h"

/* Install during backend startup, before map execution. The caller must not
 * race an engine list walk. No address fallback or runtime setting is used. */
int sh_serialized_entities_guard_install(const sig_result *results, size_t count);

#endif
