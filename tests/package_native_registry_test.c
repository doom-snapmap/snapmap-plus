#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_native_registry.h"
#include "native_resource_id.h"

static sh_resource_catalog_entry catalog[] = {
    {"image", "smp/collision/75a3", "one.bimage"},
    {"image", "smp/collision/75a3", "one-permutation.bimage"},
    {"image", "shared", "shared.bimage"}
};
size_t sh_resource_catalog_count(const sh_resource_catalog *value)
{ return value ? sizeof(catalog) / sizeof(catalog[0]) : 0; }
const sh_resource_catalog_entry *sh_resource_catalog_at(const sh_resource_catalog *value, size_t i)
{ return value && i < sh_resource_catalog_count(value) ? catalog + i : NULL; }

typedef struct region { const void *data; size_t length; } region;
typedef struct fixture {
    unsigned char manager[0x30], resident[0x30];
    uintptr_t head, entries[1];
    char type[32], name[64];
    region regions[6];
} fixture;
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context;
    for (size_t i = 0; i < 6; i++) {
        const region *r = f->regions + i;
        uintptr_t base = (uintptr_t)r->data;
        if (address >= base && address - base <= r->length && length <= r->length - (address - base)) {
            memcpy(out, (const void *)address, length); return 1;
        }
    }
    return 0;
}
static void pointer(void *p, size_t offset, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)p + offset, &address, sizeof(address)); }
static void setup(fixture *f, const char *resident)
{
    memset(f, 0, sizeof(*f));
    strcpy_s(f->type, sizeof(f->type), "image");
    f->head = (uintptr_t)f->manager;
    f->regions[0] = (region){f->manager, sizeof(f->manager)};
    f->regions[1] = (region){&f->head, sizeof(f->head)};
    f->regions[2] = (region){f->type, sizeof(f->type)};
    f->regions[3] = (region){f->entries, sizeof(f->entries)};
    f->regions[4] = (region){f->resident, sizeof(f->resident)};
    f->regions[5] = (region){f->name, sizeof(f->name)};
    pointer(f->manager, 8, f->type);
    pointer(f->manager, 0x20, f->entries);
    if (resident) {
        uint32_t id;
        strcpy_s(f->name, sizeof(f->name), resident);
        pointer(f->resident, 8, f->name);
        pointer(f->resident, 0x10, f->manager);
        assert(sh_native_resource_id(f->type, resident, &id));
        memcpy(f->resident + 0x20, &id, sizeof(id));
        f->entries[0] = (uintptr_t)f->resident;
        f->manager[0x28] = 1;
    }
}
static sh_package_native_registry *open_registry(fixture *f, int baseline, char *error, size_t capacity)
{
    sh_native_resource_registry_source source = {f, read_memory, (uintptr_t)&f->head};
    return sh_package_native_registry_open(source, baseline ? (void *)1 : NULL, error, capacity);
}
static int check(sh_package_native_registry *r, const char *name, int original)
{
    sh_package_link_registry adapter = sh_package_native_registry_adapter(r);
    unsigned char binding[32] = {3};
    char error[512] = ""; uint32_t id = 0;
    int result = adapter.check(adapter.context, "image", name, binding, original, &id, error, sizeof(error));
    if (result < 0 || !id) fprintf(stderr, "identity %s failed (%d, %08x): %s\n", name, result, id, error);
    assert(result >= 0 && id); return result;
}
int main(void)
{
    fixture f; char error[512];
    setup(&f, NULL);
    sh_package_native_registry *r = open_registry(&f, 1, error, sizeof(error)); assert(r);
    assert(check(r, "new/private", 0) == SH_PACKAGE_LINK_REGISTRY_AVAILABLE);
    /* A baseline identity reserves its ID even if no native object exists.
     * The duplicate companion row must not count as another asset. */
    assert(check(r, "smp/collision/153f0", 0) == SH_PACKAGE_LINK_REGISTRY_COLLISION);
    assert(check(r, "shared", 0) == SH_PACKAGE_LINK_REGISTRY_COLLISION);
    assert(check(r, "shared", 1) == SH_PACKAGE_LINK_REGISTRY_AVAILABLE);
    assert(sh_package_native_registry_share(r, "image", "shared"));
    assert(check(r, "shared", 0) == SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED);
    assert(sh_package_native_registry_share(r, "image", "smp/collision/153f0"));
    assert(check(r, "smp/collision/153f0", 0) == SH_PACKAGE_LINK_REGISTRY_COLLISION);
    assert(sh_package_native_registry_validate(r, error, sizeof(error)));
    f.head = 0;
    assert(!sh_package_native_registry_validate(r, error, sizeof(error)));
    sh_package_native_registry_close(r);

    setup(&f, "smp/collision/75a3");
    r = open_registry(&f, 0, error, sizeof(error)); assert(r);
    assert(check(r, "smp/collision/153f0", 0) == SH_PACKAGE_LINK_REGISTRY_COLLISION);
    assert(check(r, "smp/collision/75a3", 0) == SH_PACKAGE_LINK_REGISTRY_OCCUPIED);
    assert(check(r, "smp/collision/75a3", 1) == SH_PACKAGE_LINK_REGISTRY_AVAILABLE);
    assert(sh_package_native_registry_share(r, "image", "smp/collision/75a3"));
    assert(check(r, "smp/collision/75a3", 0) == SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED);
    sh_package_link_registry adapter = sh_package_native_registry_adapter(r);
    unsigned char binding[32] = {0}; uint32_t id;
    assert(adapter.check(adapter.context, "unregistered", "x", binding, 0,
        &id, error, sizeof(error)) == SH_PACKAGE_LINK_REGISTRY_ERROR);
    sh_package_native_registry_close(r);
    puts("package baseline and live native identity reservations passed"); return 0;
}
