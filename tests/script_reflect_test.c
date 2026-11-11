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

#include "reflect_fake.h"

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
    d.flags = 200; d.precise = 0.25; d.tiny = -3;
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
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "tiny", &loc, err) && loc.kind == SH_RF_INT && loc.size == 2);
    sh_rf_loc_free(&loc);

    /* lists: of structs, of text, of pointers; counts and elements */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "inventory", &loc, err) && loc.kind == SH_RF_LIST && !strcmp(loc.type, "weapon_t") &&
          loc.size == sizeof(weapon) && sh_rf_count(&H, &loc) == 2);
    CHECK(sh_rf_element(&H, &loc, 1, &el, err, sizeof err) && el.kind == SH_RF_STRUCT && el.addr == (const uint8_t *)&inv[1]);
    sh_rf_loc_free(&el);
    CHECK(!sh_rf_element(&H, &loc, 2, &el, err, sizeof err) && strstr(err, "holds 2 elements") != NULL);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "inventory[2].ammo", &loc, err) && loc.kind == SH_RF_INT &&
          *(const int *)loc.addr == 7);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "names[2]", &loc, err) && loc.kind == SH_RF_STR && loc.addr == (const uint8_t *)&strs[1]);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls", &loc, err) && loc.kind == SH_RF_LIST && loc.size == 8 &&
          !strcmp(loc.elem_ops, "*") && !strcmp(loc.type, "idDeclSnapAIEncounter"));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls[2]", &loc, err) && loc.kind == SH_RF_POINTER &&
          *(const void *const *)loc.addr == &enc2);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "decls[2].value", &loc, err) && loc.kind == SH_RF_INT && *(const int *)loc.addr == 4);
    sh_rf_loc_free(&loc);

    /* arrays in place: T x[N] and idArray < T , N > */
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "slots", &loc, err) && loc.kind == SH_RF_ARRAY && loc.count == 3 &&
          loc.size == sizeof(weapon));
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "slots[3].ammo", &loc, err) && *(const int *)loc.addr == 99);
    sh_rf_loc_free(&loc);
    CHECK(at(&d, "idPlaceableSnapAIEncounter", "corners[2].z", &loc, err) && loc.kind == SH_RF_FLOAT &&
          *(const float *)loc.addr == -4.0f);
    sh_rf_loc_free(&loc);
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "slots[4]", &loc, err) && strstr(err, "holds 3 elements") != NULL);

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
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "spawned[1]", &loc, err) && strstr(err, "not a list") != NULL);
    CHECK(!at(&d, "idPlaceableSnapAIEncounter", "slots[x]", &loc, err) && strstr(err, "without a number") != NULL);
    CHECK(!at(NULL, "idPlaceableSnapAIEncounter", "team", &loc, err));

    printf("script_reflect_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
