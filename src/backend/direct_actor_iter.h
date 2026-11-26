#ifndef SNAPMAP_PLUS_DIRECT_ACTOR_ITER_H
#define SNAPMAP_PLUS_DIRECT_ACTOR_ITER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Only map-projected actor declarations can acquire a runtime identity. */
void sh_direct_iter_reset(void);
int sh_direct_iter_awaiting_spawn(void);
void sh_direct_iter_place(const char *map_name, const char *inherit);
void sh_direct_iter_after_spawn(void *entity, const char *map_name, const char *inherit);
int sh_direct_iter_install(const sig_result *results, size_t count, const uint8_t *image);

/* A direct placement has one actor generation per Play. */
typedef struct sh_direct_lifetime {
    void *entity;
    uint32_t generation;
    int slot;
    const void *world;
} sh_direct_lifetime;

#ifdef SH_DIRECT_ITER_TESTING
typedef struct sh_direct_iter_ops {
    void (*original)(void *);
    void (*fire)(void *, unsigned, void *);
    int (*identity)(void *, int, sh_direct_lifetime *);
    int (*current)(const sh_direct_lifetime *);
    int (*living)(const sh_direct_lifetime *);
    int (*tracked)(const sh_direct_lifetime *);
} sh_direct_iter_ops;
void sh_direct_iter_test_bind(const sh_direct_iter_ops *ops);
void sh_direct_iter_test_run(void *iterator);
#endif
#endif
