/* script_reflect_test.c - the reflection walk of mod scripts over a fake type
 * table shaped like the game's (src/backend/script/script_reflect.h): classes
 * with superclasses, nested structs, enums, idStr, idList, idArray, fixed
 * arrays, pointers to objects and entity references. The objects are real
 * structs in this process, so every address the walk returns is read back and
 * compared with the struct's own member. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/script/script_reflect.h"

static int g_failures, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- the objects ---- */
typedef struct vec3 { float x, y, z; } vec3;
typedef struct idstr { void *vt; int len; char *data; int alloced; char base[20]; } idstr;        /* 0x30 */
typedef struct list { void *ptr; int num; int size; int pad; int pad2; } list;                    /* 0x18 */
typedef struct mods { float healthMod, damageMod; } mods;
typedef struct ref { int spawn_id; int serial; void *cached; char rest[16]; } ref;                /* 32 */
typedef struct decl { void *vt; const char *name; int value; } decl;
typedef struct weapon { int ammo; float spread; } weapon;
typedef struct entity {
    void *vt;
    idstr name;
    vec3 spawnPosition;
    int team;                  /* gameTeam_t */
} entity;
typedef struct demon {
    entity base;
    char spawned;
    ref target;
    decl *encounter;
    int spawnBehavior;         /* snapAISpawnBehavior_t */
    mods encounterMods;
    list inventory;            /* idList < weapon > */
    list names;                /* idList < idStr > */
    list decls;                /* idList < const decl * > */
    weapon slots[3];
    vec3 corners[2];           /* idArray < idVec3 , 2 > */
    unsigned char flags;
    double precise;
    void *opaque;              /* idWeird * */
    short small;
} demon;

/* ---- the fake type table ---- */
#define F(T, name, type, ops) { type, ops, #name, (uint32_t)offsetof(T, name), (uint32_t)sizeof(((T *)0)->name) }
static const sh_rf_field k_vec3[] = { F(vec3, x, "float", ""), F(vec3, y, "float", ""), F(vec3, z, "float", "") };
static const sh_rf_field k_mods[] = { F(mods, healthMod, "float", ""), F(mods, damageMod, "float", "") };
static const sh_rf_field k_weapon[] = { F(weapon, ammo, "int", ""), F(weapon, spread, "float", "") };
static const sh_rf_field k_decl[] = { F(decl, value, "int", "") };
static const sh_rf_field k_entity[] = {
    F(entity, name, "idStr", ""), F(entity, spawnPosition, "idVec3", ""), F(entity, team, "gameTeam_t", "")
};
static const sh_rf_field k_demon[] = {
    F(demon, spawned, "bool", ""),
    F(demon, target, "idManagedClassPtr < idAI2 >", ""),
    F(demon, encounter, "idDeclSnapAIEncounter", "*"),
    F(demon, spawnBehavior, "snapAISpawnBehavior_t", ""),
    F(demon, encounterMods, "encounterAIModifiers_t", ""),
    F(demon, inventory, "idList < weapon_t , TAG_IDLIST , false >", ""),
    F(demon, names, "idList < idStr , TAG_IDLIST , false >", ""),
    F(demon, decls, "idStaticList < const idDeclSnapAIEncounter * , 4 , false , TAG_IDLIST >", ""),
    F(demon, slots, "weapon_t", "[3]"),
    F(demon, corners, "idArray < idVec3 , 2 >", ""),
    F(demon, flags, "unsigned char", ""),
    F(demon, precise, "double", ""),
    F(demon, opaque, "idWeird", "*"),
    F(demon, small, "short", ""),
};
typedef struct cls { const char *name, *super; const sh_rf_field *fields; uint32_t count, size; } cls;
#define C(name, super, arr, T) { name, super, arr, (uint32_t)(sizeof arr / sizeof arr[0]), (uint32_t)sizeof(T) }
static const cls k_classes[] = {
    C("idVec3", "", k_vec3, vec3), C("encounterAIModifiers_t", "", k_mods, mods), C("weapon_t", "", k_weapon, weapon),
    C("idDeclSnapAIEncounter", "", k_decl, decl), C("idEntity", "", k_entity, entity),
    C("idPlaceableSnapAIEncounter", "idEntity", k_demon, demon), C("idAI2", "idEntity", k_demon, demon),
};
typedef struct en { const char *type, *member; long long value; } en;
static const en k_enums[] = {
    { "gameTeam_t", "TEAM_NONE", 0 }, { "gameTeam_t", "TEAM_ONE", 1 }, { "gameTeam_t", "TEAM_TWO", 2 },
    { "snapAISpawnBehavior_t", "SNAP_AI_SPAWN_BEHAVIOR_DEFAULT", 0 }, { "snapAISpawnBehavior_t", "SNAP_AI_SPAWN_BEHAVIOR_HUNT", 2 },
};

static const cls *find_cls(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++) if (!strcmp(k_classes[i].name, name)) return &k_classes[i];
    return NULL;
}
static int h_size(void *c, const char *type, uint32_t *size)
{
    const cls *k = find_cls(type);
    (void)c;
    if (k && size) *size = k->size;
    return k != NULL;
}
static const char *h_super(void *c, const char *type) { const cls *k = find_cls(type); (void)c; return k ? k->super : NULL; }
static int h_field(void *c, const char *type, uint32_t index, sh_rf_field *out)
{
    const cls *k = find_cls(type);
    (void)c;
    if (!k || index >= k->count) return 0;
    *out = k->fields[index];
    return 1;
}
static int h_enum_value(void *c, const char *type, const char *member, long long *value)
{
    size_t i;
    (void)c;
    for (i = 0; i < sizeof k_enums / sizeof k_enums[0]; i++) {
        if (strcmp(k_enums[i].type, type)) continue;
        if (!member) return 1;
        if (!strcmp(k_enums[i].member, member)) { if (value) *value = k_enums[i].value; return 1; }
    }
    return 0;
}
static const char *h_enum_name(void *c, const char *type, long long value)
{
    size_t i;
    (void)c;
    for (i = 0; i < sizeof k_enums / sizeof k_enums[0]; i++)
        if (!strcmp(k_enums[i].type, type) && k_enums[i].value == value) return k_enums[i].member;
    return NULL;
}
static int h_read(void *c, const void *addr, void *out, size_t n) { (void)c; if (!addr) return 0; memcpy(out, addr, n); return 1; }
static demon *g_target;
static int h_follow(void *c, const void *addr, const char *declared, const void **object, const char **clsname)
{
    const ref *r = (const ref *)addr;
    (void)c; (void)declared;
    if (r->spawn_id != 77 || !g_target) return 0;
    *object = g_target;
    *clsname = "idAI2";
    return 1;
}
static const sh_rf_host H = { NULL, h_size, h_super, h_field, h_enum_value, h_enum_name, h_read, h_follow, NULL };

static int at(const void *object, const char *clsname, const char *path, sh_rf_loc *loc, char *err)
{
    err[0] = 0;
    return sh_rf_resolve(&H, object, clsname, path, loc, err, 256);
}

int main(void)
{
    demon d, other;
    decl enc = { NULL, "single_tyrant", 9 }, enc2 = { NULL, "single_baron", 4 };
    weapon inv[2] = { { 30, 0.5f }, { 7, 2.0f } };
    idstr strs[2];
    const decl *dl[4] = { &enc, &enc2, NULL, NULL };
    sh_rf_loc loc, el;
    char err[256];

    memset(&d, 0, sizeof d);
    memset(&other, 0, sizeof other);
    memset(strs, 0, sizeof strs);
    d.base.team = 2;
    d.base.spawnPosition.y = 12.5f;
    d.base.name.len = 6; d.base.name.data = "tyrant";
    d.spawned = 1;
    d.encounter = &enc;
    d.spawnBehavior = 2;
    d.encounterMods.healthMod = 1.5f;
    d.inventory.ptr = inv; d.inventory.num = 2;
    strs[1].len = 2; strs[1].data = "hi";
    d.names.ptr = strs; d.names.num = 2;
    d.decls.ptr = (void *)dl; d.decls.num = 2;
    d.slots[2].ammo = 99;
    d.corners[1].z = -4.0f;
    d.flags = 200; d.precise = 0.25; d.small = -3;
    d.target.spawn_id = 77;
    other.base.team = 1;
    other.encounterMods.damageMod = 3.0f;
    g_target = &other;

    /* the object itself */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "", &loc, err) && loc.kind == SH_RF_STRUCT && loc.addr == (const uint8_t *)&d);
    sh_rf_loc_free(&loc);

    /* a field of the class, of its superclass, in any case */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "spawned", &loc, err) && loc.kind == SH_RF_BOOL && loc.size == 1 &&
          loc.addr == (const uint8_t *)&d.spawned);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "team", &loc, err) && loc.kind == SH_RF_ENUM && !strcmp(loc.type, "gameTeam_t") &&
          loc.addr == (const uint8_t *)&d.base.team);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "SpawnBehavior", &loc, err) && loc.kind == SH_RF_ENUM &&
          loc.addr == (const uint8_t *)&d.spawnBehavior);
    sh_rf_loc_free(&loc);
    CHECK(H.enum_name(NULL, "snapAISpawnBehavior_t", d.spawnBehavior) &&
          !strcmp(H.enum_name(NULL, "snapAISpawnBehavior_t", d.spawnBehavior), "SNAP_AI_SPAWN_BEHAVIOR_HUNT"));

    /* nested structs, text, numbers of every width */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "encounterMods.healthMod", &loc, err) && loc.kind == SH_RF_FLOAT &&
          loc.addr == (const uint8_t *)&d.encounterMods.healthMod);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "spawnPosition.y", &loc, err) && loc.kind == SH_RF_FLOAT &&
          *(const float *)loc.addr == 12.5f);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "encounterMods", &loc, err) && loc.kind == SH_RF_STRUCT &&
          !strcmp(loc.type, "encounterAIModifiers_t") && loc.size == sizeof(mods));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "name", &loc, err) && loc.kind == SH_RF_STR && loc.addr == (const uint8_t *)&d.base.name);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "flags", &loc, err) && loc.kind == SH_RF_UINT && loc.size == 1);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "precise", &loc, err) && loc.kind == SH_RF_DOUBLE && loc.size == 8);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "small", &loc, err) && loc.kind == SH_RF_INT && loc.size == 2);
    sh_rf_loc_free(&loc);

    /* lists: of structs, of text, of pointers; counts and elements */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "inventory", &loc, err) && loc.kind == SH_RF_LIST && !strcmp(loc.type, "weapon_t") &&
          loc.size == sizeof(weapon) && sh_rf_count(&H, &loc) == 2);
    CHECK(sh_rf_element(&H, &loc, 1, &el, err, sizeof err) && el.kind == SH_RF_STRUCT && el.addr == (const uint8_t *)&inv[1]);
    sh_rf_loc_free(&el);
    CHECK(!sh_rf_element(&H, &loc, 2, &el, err, sizeof err) && strstr(err, "holds 2 elements") != NULL);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "inventory[1].ammo", &loc, err) && loc.kind == SH_RF_INT &&
          *(const int *)loc.addr == 7);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "names[1]", &loc, err) && loc.kind == SH_RF_STR && loc.addr == (const uint8_t *)&strs[1]);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls", &loc, err) && loc.kind == SH_RF_LIST && loc.size == 8 &&
          !strcmp(loc.elem_ops, "*") && !strcmp(loc.type, "idDeclSnapAIEncounter"));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls[1]", &loc, err) && loc.kind == SH_RF_POINTER &&
          *(const void *const *)loc.addr == &enc2);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls[1].value", &loc, err) && loc.kind == SH_RF_INT && *(const int *)loc.addr == 4);
    sh_rf_loc_free(&loc);

    /* arrays in place: T x[N] and idArray < T , N > */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "slots", &loc, err) && loc.kind == SH_RF_ARRAY && loc.count == 3 &&
          loc.size == sizeof(weapon));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "slots[2].ammo", &loc, err) && *(const int *)loc.addr == 99);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "corners[1].z", &loc, err) && loc.kind == SH_RF_FLOAT &&
          *(const float *)loc.addr == -4.0f);
    sh_rf_loc_free(&loc);
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "slots[3]", &loc, err) && strstr(err, "holds 3 elements") != NULL);

    /* a pointer is a value, and is followed inside a path */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "encounter", &loc, err) && loc.kind == SH_RF_POINTER &&
          !strcmp(loc.type, "idDeclSnapAIEncounter"));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "encounter.value", &loc, err) && *(const int *)loc.addr == 9);
    sh_rf_loc_free(&loc);
    d.encounter = NULL;
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "encounter.value", &loc, err) && !strcmp(err, "encounter is empty, so it has no value"));
    d.encounter = &enc;
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "opaque", &loc, err) && loc.kind == SH_RF_POINTER && !strcmp(loc.type, "idWeird"));
    sh_rf_loc_free(&loc);

    /* an entity reference is followed to the entity it names now, with that entity's own class */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "target", &loc, err) && loc.kind == SH_RF_REF && !strcmp(loc.type, "idAI2"));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "target.encounterMods.damageMod", &loc, err) && *(const float *)loc.addr == 3.0f);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "target.team", &loc, err) && *(const int *)loc.addr == 1);
    sh_rf_loc_free(&loc);
    d.target.spawn_id = 5;
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "target.team", &loc, err) && !strcmp(err, "target names no entity now, so it has no team"));

    /* sentences for what is not there */
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "helth", &loc, err) && !strcmp(err, "idPlaceableSnapAIEncounter has no field named helth"));
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "encounterMods.x", &loc, err) && !strcmp(err, "encounterAIModifiers_t has no field named x"));
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "spawned.x", &loc, err) && !strcmp(err, "spawned is a true or false value, so it has no x"));
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "spawned[0]", &loc, err) && strstr(err, "not a list") != NULL);
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "slots[x]", &loc, err) && strstr(err, "without a number") != NULL);
    CHECK(!at(NULL, "idPlaceableSnapAIEncounter", "team", &loc, err));

    printf("script_reflect_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
