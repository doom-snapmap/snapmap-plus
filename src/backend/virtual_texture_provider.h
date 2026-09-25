/* Private virtual-texture page providers inside the engine's fixed table. */
#ifndef SH_VIRTUAL_TEXTURE_PROVIDER_H
#define SH_VIRTUAL_TEXTURE_PROVIDER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* The engine keeps 32 inline providers of one renderer-specific stride, then
 * a table of their addresses. Slot 1 is the global _vmtr atlas. A material
 * naming a missing page source makes the native selector release slot 31
 * before it takes the lowest empty slot, so private providers only occupy
 * verified empty slots 2..30 and never rely on the selector to create them. */
enum { SH_VT_PROVIDER_SLOTS = 32, SH_VT_PRIVATE_FIRST = 2, SH_VT_PRIVATE_LAST = 30 };

typedef struct sh_vt_native {
    unsigned char *system;
    size_t stride;
    /* SetSource(provider, name, path, mode, kind, direct, reader). */
    unsigned char (*set_source)(void *provider, const char *name, const char *path,
        int mode, int kind, unsigned char direct, void *reader);
    void (*release)(void *provider);
    void (*quiesce)(void *system);
    /* OpenGL converts a nonzero mode to sparse mode 4 when both are nonzero.
     * Vulkan has no conversion; both pointers are then NULL. */
    const volatile uint64_t *sparse_function;
    const volatile int32_t *sparse_enabled;
    /* Declaration resource list-of-lists, for the live material scan. */
    void *const *resources;
} sh_vt_native;

/* Decode and verify the native entry points from resolved signatures; the
 * caller supplies the declaration resource head. No hooks or engine calls. */
int sh_vt_provider_decode(const sig_result *results, size_t count, void *const *resources,
    sh_vt_native *out);
/* Bind decoded (or test) native entry points and verify the provider table. */
int sh_vt_provider_bind(const sh_vt_native *native);
/* Production: decode, bind, and hook the engine's VT initialization so
 * providers wanted before it runs are built right after it. */
int sh_vt_provider_install(const sig_result *results, size_t count, void *const *resources);
int sh_vt_provider_ready(void);
/* The global _vmtr provider exists, so the engine's VT system is initialized. */
int sh_vt_provider_initialized(void);

/* 1 for a name a private provider may carry: at most 31 printable bytes (the
 * engine's referenced-name field), no quote or backslash, and no "_vmtr",
 * which the engine reserves for its global atlas classification. */
int sh_vt_provider_name_valid(const char *name);

/* Main thread, in the resident render/heap scope or right after the engine's
 * VT initialization, once the files naming these providers are served and
 * before any of their materials parse. Each provider is kind 2 (it owns a
 * VMTR owner) in mode 2, the engine's unique-VMTR page cache, or OpenGL's
 * sparse mode 4 where the engine would convert it. Owned providers are reused
 * after their construction is re-verified. Atomic: on failure every provider
 * created by this call is released. Returns 1, or 0 with an error. */
int sh_vt_provider_acquire(const char *const *names, size_t count, char *error, size_t capacity);
/* Release owned providers keep() rejects, but only when no live material
 * still selects their slot. Returns the number of providers still held. */
size_t sh_vt_provider_collect(int (*keep)(void *, const char *), void *context);
/* Called after the engine re-initializes its providers. Owned records no
 * longer describe live objects; a provider still carrying an owned name is
 * released before the caller acquires again. */
void sh_vt_provider_reinitialized(void);
/* Wanted names are re-acquired after engine initialization. The runtime
 * supplies them; NULL clears the callback. */
typedef int (*sh_vt_provider_wanted_fn)(char *error, size_t capacity);
void sh_vt_provider_on_initialize(sh_vt_provider_wanted_fn wanted);

#ifdef SH_VT_PROVIDER_TESTING
void sh_vt_provider_test_reset(void);
#endif
#endif
