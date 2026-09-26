/* Package spawn-type keys at native dispatch (package_keys.h). */
#ifndef BACKEND_SPAWN_KEYS_H
#define BACKEND_SPAWN_KEYS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "package_keys.h"

/* Replace the spawn manager's entry selection, the global modifier lookup and
 * the AI filter test with the private-key contract. Each replacement verifies
 * the native instructions it reproduces in both supported images; any
 * mismatch installs none and packages keep native bits. Returns 1 when all
 * three are installed. */
int sh_spawn_keys_install(const sig_result *results, size_t count);
/* 1 once all three replacements are installed. */
int sh_spawn_keys_active(void);
/* Make these generations' group codes the ones the replacements read: the
 * installed library's and, while a map provider is active, the map's. 0 when
 * the two give one code different sets (nothing changes then). */
int sh_spawn_keys_publish_groups(const sh_package_key_groups *library, const sh_package_key_groups *map);

#ifdef SH_SPAWN_KEYS_TESTING
/* Exercise the replacements on synthetic engine state with test helpers. */
typedef uint32_t (*sh_spawn_keys_type_fn)(void *conductor, void *ai);
typedef void (*sh_spawn_keys_health_fn)(void *conductor, void *ai, float value);
typedef void *(*sh_spawn_keys_resolve_fn)(void *owner, int handle);
typedef uint32_t (*sh_spawn_keys_ai_type_fn)(void *filter, void *ai);
void sh_spawn_keys_test_bind(void **game_slot, sh_spawn_keys_type_fn type, sh_spawn_keys_health_fn health,
    sh_spawn_keys_resolve_fn resolve, sh_spawn_keys_ai_type_fn ai_type, float low, float high);
/* Hold the published groups as a replacement does, from entry to return. */
const sh_package_key_groups *sh_spawn_keys_test_hold(void);
void sh_spawn_keys_test_release(void);
uint64_t sh_spawn_keys_test_select(void *manager, uint32_t request);
void sh_spawn_keys_test_modifier(void *conductor, void *ai, uint64_t modifiers);
int sh_spawn_keys_test_filter(void *filter, void *ai);
#endif

#endif
