/* reflect_fake.h -- a type table shaped like the game's, for the reflection
 * tests: classes with superclasses, nested structs, enums, idStr, idList,
 * idArray, fixed arrays, pointers to objects and entity references. The
 * objects are real structs of the test's process. Include after
 * script_reflect.h; `H` is the host. */
#ifndef REFLECT_FAKE_H
#define REFLECT_FAKE_H

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
    short tiny;
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
    F(demon, tiny, "short", ""),
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

#endif
