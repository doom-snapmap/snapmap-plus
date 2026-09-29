#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_native_registry.h"
#include "../src/backend/native_resource_id.h"

typedef struct region { void *memory; size_t size; } region;
typedef struct fixture {
    region regions[128];
    size_t count;
    unsigned char *registry, *managers[4];
    int sources[4], source_calls;
    uintptr_t unreadable;
    uintptr_t mutate_at, change_at;
    uint32_t change_value;
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
            if (address == f->mutate_at) {
                memcpy((void *)f->change_at, &f->change_value, sizeof(f->change_value)); f->mutate_at = 0;
            }
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
    void *object = alloc(f, 0x30);
    ptr(object, 8, str(f, name)); ptr(object, 0x10, f->managers[manager]); ptr(array, 0, object);
    ptr(f->managers[manager], 0x20, array); num(f->managers[manager], 0x28, 1);
    return (uintptr_t)object;
}

static uintptr_t pointer_at(const void *memory, size_t offset)
{ uintptr_t value; memcpy(&value, (const char *)memory + offset, sizeof(value)); return value; }

static uintptr_t loaded_id(fixture *f, size_t manager, const char *name)
{
    uintptr_t object = loaded(f, manager, name);
    const char *type = (const char *)pointer_at(f->managers[manager], 8);
    uint32_t id;
    assert(sh_native_resource_id(type, name, &id));
    memcpy((void *)(object + 0x20), &id, sizeof(id)); return object;
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
    /* Class lookups take the first manager in registry order, whatever the
     * spelling of a later one (the indexed lookup keeps that order). */
    setup(&f);
    ptr(f.managers[3], 0x10, str(&f, "IDDECLWEAPON"));
    r = open_registry(&f, 1); assert(r);
    assert(sh_decl_native_registry_class_type(r, "iddeclweapon", &name) == 1 && !strcmp(name, "weapon"));
    cleanup(&f, r);
    setup(&f);
    ptr(f.managers[0], 0x10, str(&f, "idDeclRenamed"));
    ptr(f.managers[3], 0x10, str(&f, "IDDECLWEAPON"));
    r = open_registry(&f, 1); assert(r);
    assert(sh_decl_native_registry_class_type(r, "idDeclWeapon", &name) == 1 && !strcmp(name, "unrelated"));
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

static void test_decl_identity(void)
{
    fixture f;
    sh_decl_native_registry *r;
    sh_native_resource_identity out = {0};
    char error[256];
    setup(&f);
    ptr(f.managers[0], 8, str(&f, "entityDef"));
    uintptr_t resident = loaded_id(&f, 0, "abcd");
    r = open_registry(&f, 1); assert(r);
    assert(sh_decl_native_registry_identity(r, "ENTITYDEF", "/ABCD", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_SAME_NAME);
    assert(!strcmp(out.type, "entityDef") && !strcmp(out.name, "abcd"));
    assert(out.id == UINT32_C(0x8c39921c) && out.resident_id == out.id && out.resource == resident);
    assert(out.manager == (uintptr_t)f.managers[0] && !strcmp(out.resident_name, "abcd"));
    sh_native_resource_identity_clear(&out);
    assert(sh_decl_native_registry_identity(r, "entitydef", "abcde", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
    assert(out.id == UINT32_C(0x3118df15) && !out.resource && !out.resident_id && !out.resident_name);
    sh_native_resource_identity_clear(&out);
    assert(sh_decl_native_registry_identity(r, "missing", "abcde", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_UNREGISTERED);
    assert(!out.type && !out.name && !out.id && !f.source_calls);
    assert(sh_decl_native_registry_identity_validate(r, error, sizeof(error)));
    /* A same-name resident with an inconsistent stored ID is unavailable,
     * never reported as equal or reusable. */
    num((void *)resident, 0x20, 17);
    assert(!sh_decl_native_registry_identity_validate(r, error, sizeof(error)) && *error);
    assert(sh_decl_native_registry_identity(r, "entityDef", "abcd", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_ERROR);
    assert(!out.type && !out.name && !out.id); cleanup(&f, r);
    setup(&f); loaded_id(&f, 0, "same"); r = open_registry(&f, 1); assert(r);
    ptr((void *)pointer_at(f.registry, 8), 0, f.managers[1]);
    assert(!sh_decl_native_registry_identity_validate(r, error, sizeof(error)));
    cleanup(&f, r);
}

static uintptr_t native_setup(fixture *f)
{
    setup(f);
    ptr(f->managers[0], 8, str(f, "image"));
    ptr(f->managers[1], 8, str(f, "model"));
    ptr(f->managers[2], 8, str(f, "entityDef"));
    /* Deliberately invalid declaration metadata: the identity-only registry
     * must not depend on class names, parents or declaration source lookup. */
    for (size_t i = 0; i < 3; i++) {
        ptr(f->managers[i], 0x10, (void *)17); ptr(f->managers[i], 0x88, (void *)23);
        ptr(f->managers[i], 0x18, i < 2 ? f->managers[i + 1] : NULL);
    }
    void *head = alloc(f, sizeof(uintptr_t)); ptr(head, 0, f->managers[0]); return (uintptr_t)head;
}

static sh_native_resource_registry *native_open(fixture *f, uintptr_t head)
{
    char error[256];
    sh_native_resource_registry_source source = {f, read_memory, head};
    return sh_native_resource_registry_open(source, error, sizeof(error));
}

static void test_native_identity_collisions(void)
{
    fixture f;
    uintptr_t head = native_setup(&f);
    uintptr_t first = loaded_id(&f, 0, "smp/collision/75a3");
    uintptr_t model = loaded_id(&f, 1, "abc");
    sh_native_resource_registry *r = native_open(&f, head); assert(r);
    sh_native_resource_identity out = {0};
    char error[256];
    assert(sh_native_resource_registry_identity(r, "IMAGE", "smp/collision/153f0", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_COLLISION);
    assert(out.id == UINT32_C(0x21287b03) && out.resident_id == out.id && out.resource == first);
    assert(!strcmp(out.resident_name, "smp/collision/75a3")); sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_identity(r, "image", "smp/collision/75a3", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_SAME_NAME);
    assert(out.resource == first); sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_identity(r, "model", "ABC", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_SAME_NAME);
    assert(out.resource == model && out.id == UINT32_C(0x309b8c0b)); sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_identity(r, "entitydef", "abc", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
    assert(!strcmp(out.type, "entityDef") && out.id == UINT32_C(0xb59955bb)); sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_validate(r, error, sizeof(error)) && !f.source_calls);
    sh_native_resource_registry_close(r);
    uintptr_t second = loaded_id(&f, 0, "smp/collision/153f0");
    void *array = alloc(&f, 2 * sizeof(uintptr_t));
    ptr(array, 0, (void *)first); ptr(array, sizeof(uintptr_t), (void *)second);
    ptr(f.managers[0], 0x20, array); num(f.managers[0], 0x28, 2);
    r = native_open(&f, head); assert(r);
    /* A same-name hit must not conceal another object's colliding ID. */
    assert(sh_native_resource_registry_identity(r, "image", "smp/collision/75a3", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_COLLISION);
    assert(out.resource == second && out.resident_id == out.id); sh_native_resource_identity_clear(&out);
    sh_native_resource_registry_close(r); cleanup(&f, NULL);
}

/* The engine creates some resources unnamed (live: a font request with no
 * face opened "fonts//12.dat" and left font "" with ID 0xec305aad). Such an
 * entry keeps the snapshot usable; its ID must still be the unnamed one. */
static void test_native_identity_unnamed(void)
{
    fixture f;
    uintptr_t head = native_setup(&f);
    sh_native_resource_identity out = {0};
    char error[256];
    uint32_t id = 0, font = 0;
    assert(sh_native_resource_unnamed_id("font", &font) && font == UINT32_C(0xec305aad));
    assert(!sh_native_resource_unnamed_id("", &id) && !id && !sh_native_resource_id("font", "", &id));
    ptr(f.managers[0], 8, str(&f, "font"));
    uintptr_t unnamed = loaded(&f, 0, "");
    memcpy((void *)(unnamed + 0x20), &font, sizeof(font));
    sh_native_resource_registry *r = native_open(&f, head); assert(r);
    assert(sh_native_resource_registry_identity(r, "font", "zero threes", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
    sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_validate(r, error, sizeof(error)));
    sh_native_resource_registry_close(r);
    id = font + 1; memcpy((void *)(unnamed + 0x20), &id, sizeof(id));
    r = native_open(&f, head); assert(r);
    assert(sh_native_resource_registry_identity(r, "font", "zero threes", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_ERROR);
    sh_native_resource_registry_close(r); cleanup(&f, NULL);
}

static void test_native_identity_changes(void)
{
    for (int fault = 0; fault < 8; fault++) {
        fixture f;
        uintptr_t head = native_setup(&f), object = loaded_id(&f, 0, "same");
        sh_native_resource_registry *r = native_open(&f, head); assert(r);
        sh_native_resource_identity out = {0};
        char error[256];
        assert(sh_native_resource_registry_identity(r, "image", "unused", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
        sh_native_resource_identity_clear(&out);
        if (fault == 0) num((void *)object, 0x20, 17);
        if (fault == 1) strcpy((char *)pointer_at((void *)object, 8), "fake");
        if (fault == 2) ptr((void *)object, 0x10, f.managers[1]);
        if (fault == 3) {
            uintptr_t replacement = loaded_id(&f, 1, "same");
            ptr((void *)pointer_at(f.managers[0], 0x20), 0, (void *)replacement);
        }
        if (fault == 4) ptr((void *)head, 0, f.managers[1]);
        if (fault == 5) ptr(f.managers[0], 0x18, f.managers[2]);
        if (fault == 6) ptr(f.managers[0], 8, str(&f, "Image"));
        if (fault == 7) f.unreadable = object + 0x20;
        assert(!sh_native_resource_registry_validate(r, error, sizeof(error)) && *error);
        assert(sh_native_resource_registry_identity(r, "image", "unused", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_ERROR);
        assert(!out.name && !out.id && !out.resource);
        sh_native_resource_registry_close(r); cleanup(&f, NULL);
    }
    fixture f;
    uintptr_t head = native_setup(&f), object = loaded_id(&f, 0, "same");
    sh_native_resource_registry *r = native_open(&f, head); assert(r);
    sh_native_resource_identity out = {0};
    char error[256];
    f.mutate_at = object + 0x20; f.change_at = f.mutate_at; f.change_value = 17;
    assert(sh_native_resource_registry_identity(r, "image", "same", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_ERROR);
    assert(!out.name && !out.id && *error);
    sh_native_resource_registry_close(r); cleanup(&f, NULL);
}

/* Repeated queries of one snapshot revalidate only what each read: the manager
 * header and the entries sharing the queried ID. Registration, removal, array
 * replacement and a changed bucket entry fail; the full check covers the rest. */
static void test_native_identity_bucket_validation(void)
{
    for (int fault = 0; fault < 5; fault++) {
        fixture f;
        uintptr_t head = native_setup(&f), object = loaded_id(&f, 0, "same");
        sh_native_resource_registry *r = native_open(&f, head); assert(r);
        sh_native_resource_identity same = {0}, unused = {0};
        char error[256];
        assert(sh_native_resource_registry_identity(r, "image", "same", &same, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_SAME_NAME);
        assert(sh_native_resource_registry_identity(r, "image", "unused", &unused, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
        assert(sh_native_resource_registry_validate_identity(r, &same, error, sizeof(error)));
        assert(sh_native_resource_registry_validate_identity(r, &unused, error, sizeof(error)));
        void *array = (void *)pointer_at(f.managers[0], 0x20);
        if (fault == 0) num((void *)object, 0x20, 17);                  /* bucket entry ID */
        if (fault == 1) num(f.managers[0], 0x28, 2);                     /* registration count */
        if (fault == 2) {                                                /* replaced array */
            void *copy = alloc(&f, sizeof(uintptr_t)); ptr(copy, 0, (void *)object);
            ptr(f.managers[0], 0x20, copy);
        }
        if (fault == 3) ptr(array, 0, (void *)loaded_id(&f, 1, "same")); /* swapped entry */
        if (fault == 4) ptr(f.managers[0], 8, str(&f, "Image"));        /* manager identity */
        if (fault == 0 || fault == 3) {
            /* The bucket owner changed; the absent name's own reads did not. */
            assert(!sh_native_resource_registry_validate_identity(r, &same, error, sizeof(error)) && *error);
        } else {
            assert(!sh_native_resource_registry_validate_identity(r, &same, error, sizeof(error)) && *error);
            assert(!sh_native_resource_registry_validate_identity(r, &unused, error, sizeof(error)) && *error);
        }
        assert(!sh_native_resource_registry_validate(r, error, sizeof(error)));
        sh_native_resource_identity_clear(&same); sh_native_resource_identity_clear(&unused);
        sh_native_resource_registry_close(r); cleanup(&f, NULL);
    }
}

static void test_native_identity_empty_and_malformed(void)
{
    fixture f;
    uintptr_t head = native_setup(&f);
    sh_native_resource_identity out = {0};
    char error[256];
    ptr((void *)head, 0, NULL);
    sh_native_resource_registry *r = native_open(&f, head); assert(r);
    assert(sh_native_resource_registry_identity(r, "image", "same", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_UNREGISTERED);
    assert(sh_native_resource_registry_validate(r, error, sizeof(error)));
    sh_native_resource_registry_close(r);
    ptr((void *)head, 0, f.managers[0]);
    void *array = alloc(&f, sizeof(uintptr_t));
    ptr(f.managers[0], 0x20, array); num(f.managers[0], 0x28, 1);
    r = native_open(&f, head); assert(r);
    assert(sh_native_resource_registry_identity(r, "image", "same", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_FREE);
    sh_native_resource_identity_clear(&out);
    assert(sh_native_resource_registry_validate(r, error, sizeof(error))); sh_native_resource_registry_close(r);
    ptr(f.managers[2], 0x18, f.managers[0]);
    assert(!native_open(&f, head)); ptr(f.managers[2], 0x18, NULL);
    ptr(f.managers[1], 8, str(&f, "IMAGE"));
    r = native_open(&f, head); assert(r);
    assert(sh_native_resource_registry_identity(r, "image", "same", &out, error, sizeof(error)) == SH_NATIVE_RESOURCE_ID_ERROR);
    assert(!out.name && !out.id); sh_native_resource_registry_close(r);
    cleanup(&f, NULL);
}

typedef struct candidates { const char *families[4]; size_t count, stop; } candidates;
static int candidate(void *context, const char *family, const char *name)
{
    candidates *c = context;
    assert(c->count < 4 && !strcmp(name, "missing/target"));
    c->families[c->count++] = family;
    return c->count != c->stop;
}
static void test_candidate_observation(void)
{
    fixture f; setup(&f); sh_decl_native_registry *r = open_registry(&f, 1); assert(r);
    candidates c = {0};
    assert(sh_decl_native_registry_source_candidates(r, "idDeclInventory", "/MISSING\\TARGET", candidate, &c));
    assert(c.count == 3 && !strcmp(c.families[0], "inventoryItem") &&
        !strcmp(c.families[1], "weapon") && !strcmp(c.families[2], "ammo"));
    assert(!f.source_calls);
    memset(&c, 0, sizeof(c));
    assert(sh_decl_native_registry_source_candidates(r, "genericClass", "missing/target", candidate, &c));
    assert(c.count == 4 && !f.source_calls);
    memset(&c, 0, sizeof(c)); c.stop = 1;
    assert(!sh_decl_native_registry_source_candidates(r, "idDeclInventory", "missing/target", candidate, &c));
    assert(c.count == 1);
    assert(!sh_decl_native_registry_source_candidates(r, "idDeclInventory", "\xC0", candidate, &c));
    assert(sh_decl_native_registry_source_candidates(r, "idDeclInventory", "", candidate, &c));
    assert(c.count == 1 && !f.source_calls); cleanup(&f, r);
}

int main(void)
{
    test_lookup_order(); test_unknown_and_ancestry(); test_memory_and_names(); test_type_class();
    test_candidate_sources(); test_candidate_observation();
    test_decl_identity(); test_native_identity_collisions(); test_native_identity_unnamed(); test_native_identity_changes();
    test_native_identity_bucket_validation(); test_native_identity_empty_and_malformed();
    puts("decl_native_registry_test: PASS"); return 0;
}
