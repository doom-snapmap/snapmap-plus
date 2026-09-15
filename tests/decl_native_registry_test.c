#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_native_registry.h"

typedef struct region { void *memory; size_t size; } region;
typedef struct fixture {
    region regions[128];
    size_t count;
    unsigned char *registry, *managers[4];
    int sources[4], source_calls;
    uintptr_t unreadable;
    int mutate;
} fixture;
static void *alloc(fixture *f, size_t size)
{
    void *p = calloc(1, size);
    assert(p && f->count < 128);
    f->regions[f->count++] = (region){p, size}; return p;
}
static void ptr(void *p, size_t offset, const void *value)
{ uintptr_t a = (uintptr_t)value; memcpy((char *)p + offset, &a, sizeof(a)); }
static void num(void *p, size_t offset, int32_t value)
{ memcpy((char *)p + offset, &value, sizeof(value)); }
static char *str(fixture *f, const char *s)
{ char *p = alloc(f, strlen(s) + 1); strcpy(p, s); return p; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context;
    size_t i;
    if (address == f->unreadable) return 0;
    for (i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].memory;
        if (address >= start && address - start <= f->regions[i].size &&
            length <= f->regions[i].size - (size_t)(address - start)) {
            memcpy(out, (void *)address, length);
            if (f->mutate && address == (uintptr_t)f->managers[0] + 0x28) {
                num(f->managers[0], 0x28, 0); f->mutate = 0;
            }
            return 1;
        }
    }
    return 0;
}
static int source_exists(void *context, uintptr_t manager, const char *type, const char *name)
{
    fixture *f = context;
    size_t i;
    assert(type && name && !strcmp(name, "same"));
    f->source_calls++;
    for (i = 0; i < 4; i++) if (manager == (uintptr_t)f->managers[i]) return f->sources[i];
    assert(0); return -1;
}
static void setup(fixture *f)
{
    static const char *types[] = {"weapon", "ammo", "inventoryItem", "unrelated"};
    static const char *classes[] = {"idDeclWeapon", "idDeclAmmo", "idDeclInventory", "idDeclOther"};
    void *array;
    size_t i;
    memset(f, 0, sizeof(*f));
    f->registry = alloc(f, 0x18); array = alloc(f, 4 * sizeof(uintptr_t));
    ptr(f->registry, 8, array); num(f->registry, 0x10, 4);
    for (i = 0; i < 4; i++) {
        f->managers[i] = alloc(f, 0x90);
        ptr(array, i * sizeof(uintptr_t), f->managers[i]);
        ptr(f->managers[i], 8, str(f, types[i]));
        ptr(f->managers[i], 0x10, str(f, classes[i]));
    }
    ptr(f->managers[0], 0x88, f->managers[2]);
    ptr(f->managers[1], 0x88, f->managers[2]);
}
static uintptr_t loaded(fixture *f, size_t manager, const char *name)
{
    void *array = alloc(f, sizeof(uintptr_t));
    void *object = alloc(f, 0x18);
    ptr(object, 8, str(f, name)); ptr(object, 0x10, f->managers[manager]); ptr(array, 0, object);
    ptr(f->managers[manager], 0x20, array); num(f->managers[manager], 0x28, 1);
    return (uintptr_t)object;
}
static sh_decl_native_registry *open_registry(fixture *f, int with_sources)
{
    char error[160];
    sh_decl_registry_source source = {f, read_memory, (uintptr_t)f->registry,
                                      with_sources ? source_exists : NULL};
    return sh_decl_native_registry_open(source, error, sizeof(error));
}
static void cleanup(fixture *f, sh_decl_native_registry *r)
{ size_t i; sh_decl_native_registry_close(r); for (i = 0; i < f->count; i++) free(f->regions[i].memory); }
static void check(sh_decl_native_registry *r, const char *class_name, const char *name,
                   int status, const char *type, uintptr_t resource)
{
    sh_decl_reference ref = {0};
    assert(sh_decl_native_registry_resolve(r, class_name, name, &ref) == status);
    if (status == 1) {
        assert(ref.type && !strcmp(ref.type, type));
        assert(ref.resource == resource && ref.name);
    } else assert(!ref.type && !ref.name && !ref.resource);
    sh_decl_reference_clear(&ref);
    assert(!ref.name && !ref.type && !ref.resource);
}
static void test_lookup_order(void)
{
    fixture f;
    sh_decl_native_registry *r;
    uintptr_t weapon, ammo, inventory, other;
    setup(&f);
    weapon = loaded(&f, 0, "same"); ammo = loaded(&f, 1, "same");
    inventory = loaded(&f, 2, "same"); other = loaded(&f, 3, "same");
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclInventory", "same", 1, "inventoryItem", inventory);
    check(r, "IDDECLAMMO", "same", 1, "ammo", ammo);
    check(r, "idDeclOther", "same", 1, "unrelated", other);
    check(r, "unregisteredClass", "same", 1, "weapon", weapon);
    assert(!f.source_calls); cleanup(&f, r);

    setup(&f); weapon = loaded(&f, 0, "same"); loaded(&f, 1, "same");
    f.sources[2] = 1;
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclInventory", "same", 1, "weapon", weapon);
    assert(!f.source_calls); cleanup(&f, r);

    setup(&f); loaded(&f, 1, "same"); f.sources[0] = 1;
    r = open_registry(&f, 1); assert(r);
    /* Generic fallback finishes the first manager's source lookup before
     * considering a loaded object belonging to a later unrelated manager. */
    check(r, "unregisteredClass", "same", 1, "weapon", 0);
    assert(f.source_calls == 1); cleanup(&f, r);

    setup(&f); f.sources[0] = 1; f.sources[2] = 1;
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclInventory", "same", 1, "inventoryItem", 0);
    cleanup(&f, r);

    setup(&f); f.sources[0] = 1; f.sources[1] = 1;
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclInventory", "same", 1, "weapon", 0);
    cleanup(&f, r);
}
static void test_unknown_and_ancestry(void)
{
    fixture f;
    sh_decl_native_registry *r;
    uintptr_t ammo;
    setup(&f); f.sources[0] = -1; loaded(&f, 1, "same");
    r = open_registry(&f, 1); assert(r);
    check(r, "unregisteredClass", "same", -1, NULL, 0);
    cleanup(&f, r);

    setup(&f); ammo = loaded(&f, 1, "same");
    ptr(f.managers[1], 0x88, f.managers[0]);
    r = open_registry(&f, 0); assert(r);
    check(r, "idDeclInventory", "same", 1, "ammo", ammo);
    check(r, "idDeclOther", "same", -1, NULL, 0);
    cleanup(&f, r);

    setup(&f); r = open_registry(&f, 1); assert(r);
    check(r, "idDeclInventory", "same", 0, NULL, 0);
    cleanup(&f, r);

    setup(&f); ptr(f.managers[2], 0x88, f.managers[0]);
    r = open_registry(&f, 1); assert(!r); cleanup(&f, r);
    setup(&f); ptr(f.managers[2], 0x88, (void *)17);
    r = open_registry(&f, 1); assert(!r); cleanup(&f, r);
}
static void test_memory_and_names(void)
{
    fixture f;
    sh_decl_native_registry *r;
    uintptr_t weapon;
    setup(&f); weapon = loaded(&f, 0, "path/same");
    r = open_registry(&f, 0); assert(r);
    check(r, "idDeclWeapon", "/PATH\\SAME", 1, "weapon", weapon);
    check(r, "idDeclWeapon", "", 0, NULL, 0);
    check(r, "idDeclWeapon", "\xC0", -1, NULL, 0);
    check(r, "idDeclWeapon", "path/same.decl", -1, NULL, 0);
    cleanup(&f, r);
    setup(&f); weapon = loaded(&f, 0, "//path/same.decl");
    r = open_registry(&f, 0); assert(r);
    check(r, "idDeclWeapon", "//PATH/SAME.DECL", 1, "weapon", weapon);
    cleanup(&f, r);

    setup(&f); weapon = loaded(&f, 0, "same"); f.unreadable = weapon + 8;
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclWeapon", "same", -1, NULL, 0); cleanup(&f, r);
    setup(&f); loaded(&f, 0, "same"); f.mutate = 1;
    r = open_registry(&f, 1); assert(r);
    check(r, "idDeclWeapon", "same", -1, NULL, 0); cleanup(&f, r);
    setup(&f); f.unreadable = (uintptr_t)f.registry + 8;
    r = open_registry(&f, 1); assert(!r); cleanup(&f, r);
}
static void test_type_class(void)
{
    fixture f;
    sh_decl_native_registry *r;
    const char *name = "previous";
    setup(&f);
    r = open_registry(&f, 1); assert(r);
    /* Loaded objects can be unavailable; class lookup uses metadata only. */
    f.unreadable = (uintptr_t)f.managers[0] + 0x20;
    assert(sh_decl_native_registry_type_class(r, "WEAPON", &name) == 1);
    assert(!strcmp(name, "idDeclWeapon") && f.source_calls == 0);
    assert(sh_decl_native_registry_type_class(r, "missing", &name) == 0 && !name);
    assert(sh_decl_native_registry_type_class(NULL, "weapon", &name) == -1 && !name);
    cleanup(&f, r);
    setup(&f);
    ptr(f.managers[1], 8, str(&f, "weapon"));
    r = open_registry(&f, 1); assert(r);
    assert(sh_decl_native_registry_type_class(r, "weapon", &name) == -1 && !name);
    cleanup(&f, r);
}

static void test_candidate_sources(void)
{
    fixture f;
    sh_decl_native_registry *r;
    sh_decl_reference ref = {0};
    setup(&f); loaded(&f, 0, "same"); loaded(&f, 1, "same");
    f.sources[2] = 1;
    r = open_registry(&f, 1); assert(r);
    /* Existing weapon/ammo objects belong to a different context. Candidate
     * inventory has the base family, so no loaded descendant may capture it. */
    f.unreadable = (uintptr_t)f.managers[0] + 0x20;
    assert(sh_decl_native_registry_resolve_source(r, "idDeclInventory", "/SAME", &ref) == 1);
    assert(!strcmp(ref.type, "inventoryItem") && !strcmp(ref.name, "same") && !ref.resource);
    sh_decl_reference_clear(&ref);
    f.sources[2] = 0; f.sources[1] = 1;
    assert(sh_decl_native_registry_resolve_source(r, "idDeclInventory", "same", &ref) == 1);
    assert(!strcmp(ref.type, "ammo") && !ref.resource); sh_decl_reference_clear(&ref);
    f.sources[1] = 0;
    assert(sh_decl_native_registry_resolve_source(r, "idDeclInventory", "same", &ref) == 0);
    assert(!ref.type && !ref.name && !ref.resource);
    f.sources[0] = -1; f.sources[1] = 1;
    assert(sh_decl_native_registry_resolve_source(r, "idDeclInventory", "same", &ref) == -1);
    assert(!ref.type && !ref.name && !ref.resource);
    f.sources[0] = 0;
    assert(sh_decl_native_registry_resolve_source(r, "unregisteredClass", "same", &ref) == 1);
    assert(!strcmp(ref.type, "ammo") && !ref.resource); sh_decl_reference_clear(&ref);
    assert(sh_decl_native_registry_resolve_source(r, "idDeclInventory", "same.decl", &ref) == -1);
    cleanup(&f, r);
    setup(&f); loaded(&f, 0, "same"); r = open_registry(&f, 0); assert(r);
    assert(sh_decl_native_registry_resolve_source(r, "idDeclWeapon", "same", &ref) == -1);
    assert(!ref.type && !ref.name && !ref.resource); cleanup(&f, r);
}

int main(void)
{
    test_lookup_order(); test_unknown_and_ancestry(); test_memory_and_names(); test_type_class();
    test_candidate_sources();
    puts("decl_native_registry_test: PASS"); return 0;
}
