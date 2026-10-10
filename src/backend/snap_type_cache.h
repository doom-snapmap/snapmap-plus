/* SnapMap editor type-info cache completion (idSnapTypeInfoCache). */
#ifndef BACKEND_SNAP_TYPE_CACHE_H
#define BACKEND_SNAP_TYPE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Replace the cache's build and complete its lookups. Each replacement
 * verifies the native instructions it depends on in both supported images;
 * any mismatch installs none and the native cache is unchanged. Returns 1
 * when all three are installed. */
int sh_snap_type_cache_install(const sig_result *results, size_t count, const uint8_t *module_base);
/* 1 once the replacements are installed. */
int sh_snap_type_cache_active(void);

/* The temporary-entity route Build verified: the game-access slot and the
 * context constructor, default arguments, reset and destructor it uses
 * around game +0x448/+0x378/+0x470. 1 with *out filled once installed. */
typedef struct sh_snap_entity_route {
    void **game_access;
    void (*context_ctor)(void *context, int zero);
    void (*context_args)(void *args, int a, int b);
    void (*context_reset)(void *context);
    void (*context_dtor)(void *context);
} sh_snap_entity_route;
int sh_snap_type_cache_entity_route(sh_snap_entity_route *out);

#ifdef SH_SNAP_TYPE_CACHE_TESTING
/* Native layout and callees, supplied by tests with synthetic engine state. */
typedef struct sh_snap_type_cache_native {
    void **game_access;                 /* slot holding the game-access object */
    int (*mode)(void);
    void (*mode_init)(void);
    void (*builder_ctor)(void *builder, int zero);
    void (*builder_args)(void *args, int a, int b);
    void (*builder_reset)(void *builder);
    void (*builder_finalize)(void *builder);
    void (*builder_dtor)(void *builder);
    const int *definition_count;
    void *definition_list;
    void *(*definition_at)(void *list, int index);
    void *entity_manager;
    void *(*entity_find)(void *manager, const char *name, int create);
    void (*slot_assign)(void *slot, void *builder);
    void (*native_build)(void *cache);
    void **(*native_get)(void *cache, void **out, void *decl);
    void *(*native_find)(void *cache, const char **key);
    void *(*heap_get)(void);
    void (*heap_push)(void *self, int heap);
    void (*heap_pop)(void *self);
} sh_snap_type_cache_native;
void sh_snap_type_cache_test_bind(const sh_snap_type_cache_native *native);
void sh_snap_type_cache_test_build(void *cache);
void **sh_snap_type_cache_test_get(void *cache, void **out, void *decl);
void *sh_snap_type_cache_test_find(void *cache, const char **key);
#endif

#endif
