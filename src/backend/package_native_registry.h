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
sh_package_link_registry sh_package_native_registry_adapter(sh_package_native_registry *registry);
int sh_package_native_registry_validate(sh_package_native_registry *registry,
    char *error, size_t capacity);
#endif
