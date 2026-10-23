/* Native identity reservations for one unpublished package compilation. */
#ifndef SH_PACKAGE_NATIVE_REGISTRY_H
#define SH_PACKAGE_NATIVE_REGISTRY_H
#include "package_linker.h"
#include "decl_native_registry.h"
#include "resource_catalog.h"

typedef struct sh_package_native_registry sh_package_native_registry;
/* Catalog and the native registry must remain stable until close. Capture is
 * read-only; no resource is loaded or replaced by this helper. */
sh_package_native_registry *sh_package_native_registry_open(
    sh_native_resource_registry_source source, const sh_resource_catalog *catalog,
    char *error, size_t capacity);
void sh_package_native_registry_close(sh_package_native_registry *registry);
/* Only the bound contribution planner may reserve an original shared slot.
 * This records a publication obligation, never equality to its old contents. */
int sh_package_native_registry_share(sh_package_native_registry *registry,
    const char *family, const char *name);
/* Same-name occupancy is distinct from a numeric collision. The linker alone
 * may certify an occupied private name from its retained exact binding proof. */
sh_package_link_registry sh_package_native_registry_adapter(sh_package_native_registry *registry);
int sh_package_native_registry_validate(sh_package_native_registry *registry,
    char *error, size_t capacity);
/* Observe every decision this registry makes (R-344 stage B): each share in call order with its result, each
 * identity check with the family, name, original flag, result (SH_PACKAGE_LINK_REGISTRY_*) and ID, and the
 * validation. A warm start replays them, in order, on a fresh registry of this process. NULL stops. */
typedef struct sh_package_native_registry_journal {
    void *context;
    void (*share)(void *context, const char *family, const char *name, int result);
    void (*check)(void *context, const char *family, const char *name, int original, int result, uint32_t id);
    void (*validate)(void *context, int ok);
} sh_package_native_registry_journal;
void sh_package_native_registry_observe(sh_package_native_registry *registry,
    const sh_package_native_registry_journal *journal);
/* The linker adapter's check, called directly (the binding fingerprint plays no part in it). */
int sh_package_native_registry_check(sh_package_native_registry *registry, const char *family, const char *name,
    int original, uint32_t *id, char *error, size_t capacity);
#endif
