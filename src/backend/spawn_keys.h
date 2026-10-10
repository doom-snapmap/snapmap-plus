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
/* Prepared immutable dispatch metadata. Exact classifications with no authored
 * modifier must not fall through to another provider's generic modifier.
 * Creation allocates; publication retains the prepared program without allocation.
 * Readers retain a publication slot until their native operation completes. */
typedef struct sh_spawn_keys_program sh_spawn_keys_program;
sh_spawn_keys_program *sh_spawn_keys_program_create(const sh_package_key_groups *groups,
    const uint32_t *unmodified_types, size_t count);
/* Composed private modifier rows carry their complete classification type.
 * Equality dispatch preserves that type's selected native-parsed row even
 * when its private key set overlaps another type's set. Other types continue
 * to use native/private intersection. Input arrays are copied. */
sh_spawn_keys_program *sh_spawn_keys_program_create_exact(const sh_package_key_groups *groups,
    const uint32_t *unmodified_types, size_t unmodified_count,
    const uint32_t *exact_types, size_t exact_count);
void sh_spawn_keys_program_release(sh_spawn_keys_program *program);
void sh_spawn_keys_publish_program(sh_spawn_keys_program *program);

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
int sh_spawn_keys_test_unmodified(uint32_t type);
int sh_spawn_keys_test_exact(uint32_t type);
uint64_t sh_spawn_keys_test_select(void *manager, uint32_t request);
void sh_spawn_keys_test_modifier(void *conductor, void *ai, uint64_t modifiers);
int sh_spawn_keys_test_filter(void *filter, void *ai);
#endif

#endif
