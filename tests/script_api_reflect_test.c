/* script_api_reflect_test.c -- get, set, fields, class and enum of mod
 * scripts (src/backend/script/script_api_reflect.c) over the fake type table
 * of reflect_fake.h and real structs: every kind of value read and written
 * from Lua, on the mod's own entity and on another's, and each refusal's
 * sentence. */
#include "heap_out.h"
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_api_common.h"
#include "script/script_api_reflect.h"
#include "reflect_fake.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define SALT "reflect1"

static char g_text[16384];
static int src_list(void *ctx, const char *salt, sh_script_file_fn fn, void *fnctx)
{
    (void)ctx;
    if (strcmp(salt, SALT)) return 0;
    fn(fnctx, "main.lua");
    return 1;
}
static int src_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    (void)ctx;
    if (strcmp(salt, SALT) || _stricmp(rel, "main.lua")) return 0;
    *len = strlen(g_text);
    *text = (char *)malloc(*len + 1);
    memcpy(*text, g_text, *len + 1);
    return 1;
}
static void src_release(void *ctx, char *text) { (void)ctx; free(text); }
static const sh_script_source g_src = { NULL, src_list, src_read, src_release };

/* ---- the world: the mod's demon and a stock one ---- */
#define ID_OWN   0x10000u
#define ID_OTHER 77u
static demon g_own, g_other;
static decl g_enc = { NULL, "single_tyrant", 9 }, g_enc2 = { NULL, "single_baron", 4 };
static weapon g_inv[2];
static idstr g_strs[2];
static const decl *g_dl[4];
static int g_ready = 1;

static void *f_resolve(uint32_t id) { return id == ID_OWN ? (void *)&g_own : id == ID_OTHER ? (void *)&g_other : NULL; }
static uint32_t f_spawn_id(void *ent) { return ent == &g_own ? ID_OWN : ent == &g_other ? ID_OTHER : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_slots(void) { return 2; }
static uint32_t f_slot_spawn_id(int n) { return n == 0 ? ID_OWN : n == 1 ? ID_OTHER : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_kind(void *ent) { (void)ent; return SH_SCRIPT_KIND_AI; }
static sh_script_engine g_engine_fake;

/* ---- what values need beyond the walk ---- */
static const char *o_class(void *c, void *ent) { (void)c; return ent == &g_other ? "idAI2" : "idPlaceableSnapAIEncounter"; }
static int o_write(void *c, void *addr, const void *in, size_t n) { (void)c; if (!addr) return 0; memcpy(addr, in, n); return 1; }
static char *o_str_read(void *c, const void *s, size_t *len)
{
    const idstr *str = (const idstr *)s;
    char *out = (char *)malloc((size_t)str->len + 1);
    (void)c;
    if (str->len) memcpy(out, str->data, (size_t)str->len);
    out[str->len] = 0;
    *len = (size_t)str->len;
    return out;
}
static char g_pool[8][64];
static int g_pool_n;
static int o_str_assign(void *c, void *s, const char *text)
{
    idstr *str = (idstr *)s;
    char *slot = g_pool[g_pool_n++ % 8];
    (void)c;
    strcpy_s(slot, 64, text);
    str->data = slot;
    str->len = (int)strlen(text);
    return 1;
}
static const char *o_res_name(void *c, const void *object) { (void)c; return ((const decl *)object)->name; }
static void *o_res_find(void *c, const char *cls, const char *name)
{
    (void)c;
    if (strcmp(cls, "idDeclSnapAIEncounter")) return NULL;
    if (!strcmp(name, g_enc.name)) return &g_enc;
    if (!strcmp(name, g_enc2.name)) return &g_enc2;
    return NULL;
}
static void o_push_entity(lua_State *L, void *c, void *ent) { (void)c; sh_script_api_push_engine_entity(L, ent); }
static void *o_to_entity(lua_State *L, void *c, int idx)
{
    (void)c;
    return luaL_testudata(L, idx, "entity") ? sh_script_api_entity(L, idx, "set", NULL) : NULL;
}
static int o_enum_member(void *c, const char *type, uint32_t index, const char **member, long long *value)
{
    size_t i;
    uint32_t seen = 0;
    (void)c;
    for (i = 0; i < sizeof k_enums / sizeof k_enums[0]; i++) {
        if (strcmp(k_enums[i].type, type)) continue;
        if (seen++ == index) { *member = k_enums[i].member; *value = k_enums[i].value; return 1; }
    }
    return 0;
}
static int o_size(void *c, const char *type, uint32_t *size) { return g_ready && h_size(c, type, size); }
static const char *o_object_class(void *c, const void *object, const char *declared)
{
    (void)c; (void)declared;
    return object == &g_other ? "idAI2" : NULL;
}
static int o_follow(void *c, const void *addr, const char *declared, const void **object, const char **clsname)
{
    const ref *r = (const ref *)addr;
    (void)c; (void)declared;
    if ((uint32_t)r->spawn_id != ID_OTHER) return 0;
    *object = &g_other;
    *clsname = "idAI2";
    return 1;
}
static const sh_rf_host k_host = { NULL, o_size, h_super, h_field, h_enum_value, h_enum_name, h_read, o_follow, o_object_class };
static const sh_script_reflect_ops k_ops = { NULL, o_class, o_write, o_str_read, o_str_assign, o_res_name, o_res_find,
                                             o_push_entity, o_to_entity, o_enum_member };

static int t_ent(lua_State *L)
{
    uint32_t id = luaL_checkinteger(L, 1) ? ID_OTHER : ID_OWN;
    sh_script_handle h = sh_script_handles_find(id, NULL);
    if (!h) h = sh_script_handles_ref(id);
    sh_script_push_entity(L, h);
    return 1;
}

static char g_log[65536];
static void log_cat(void *ctx, const char *line)
{
    (void)ctx;
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) { strcat_s(g_log, sizeof g_log, line); strcat_s(g_log, sizeof g_log, "\n"); }
}
static const char *log_now(void)
{
    g_log[0] = 0;
    sh_script_log_read(SALT, log_cat, NULL);
    return g_log;
}
static char g_stop[4096];
static const char *stop_now(void)
{
    g_stop[0] = 0;
    HOS(g_stop, sh_script_stop_line_dup(SALT));
    return g_stop;
}
static int has(const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    if (!ok) printf("  missing \"%s\" in:\n%s\n", needle, hay);
    return ok;
}

static void world(void)
{
    memset(&g_own, 0, sizeof g_own);
    memset(&g_other, 0, sizeof g_other);
    memset(g_strs, 0, sizeof g_strs);
    g_inv[0].ammo = 30; g_inv[0].spread = 0.5f; g_inv[1].ammo = 7; g_inv[1].spread = 2.0f;
    g_dl[0] = &g_enc; g_dl[1] = &g_enc2; g_dl[2] = g_dl[3] = NULL;
    g_own.base.team = 2;
    g_own.base.spawnPosition.y = 12.5f;
    g_own.base.name.len = 6; g_own.base.name.data = "tyrant";
    g_own.spawned = 1;
    g_own.encounter = &g_enc;
    g_own.spawnBehavior = 2;
    g_own.encounterMods.healthMod = 1.5f;
    g_own.inventory.ptr = g_inv; g_own.inventory.num = 2;
    g_strs[0].len = 2; g_strs[0].data = "yo";
    g_strs[1].len = 2; g_strs[1].data = "hi";
    g_own.names.ptr = g_strs; g_own.names.num = 2;
    g_own.decls.ptr = (void *)g_dl; g_own.decls.num = 2;
    g_own.slots[2].ammo = 99;
    g_own.corners[1].z = -4.0f;
    g_own.flags = 200; g_own.precise = 0.25; g_own.tiny = -3;
    g_own.target.spawn_id = (int)ID_OTHER;
    g_other.base.team = 1;
    g_other.base.name.len = 4; g_other.base.name.data = "imp1";
}

static void run(const char *body)
{
    sh_script_mod_desc d;
    const char *names[1];
    sh_script_map_end();
    world();
    strcpy_s(g_text, sizeof g_text, body);
    d.salt = SALT;
    d.name = "Reflect Test";
    sh_script_map_start(&d, 1, &g_src, 3, 5.0);
    names[0] = "mods/" SALT "/ai/tyrant";
    sh_script_hooks_record(ID_OWN, names, 1, NULL);
    names[0] = "ai/demon/imp";
    sh_script_hooks_record(ID_OTHER, names, 1, NULL);
    sh_script_api_frame();
    sh_script_tick(5.0, 0.016);
}

static void refused(const char *body, const char *want)
{
    run(body);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
    CHECK(has(stop_now(), want));
}

static void test_reads(void)
{
    run("api = 1\n"
        "local d, o = ent(0), ent(1)\n"
        "print('class', d:class(), o:class())\n"
        "print('team', d:get('team'), o:get('team'))\n"
        "print('behavior', d:get('spawnBehavior'))\n"
        "print('mods', d:get('encounterMods.healthMod'), d:get('encounterMods').damageMod)\n"
        "print('name', d:get('name'), o:get('name'))\n"
        "local p = d:get('spawnPosition') print('pos', p.x, p.y, p.z)\n"
        "print('spawned', d:get('spawned'))\n"
        "print('target', d:get('target') == o, d:get('target.team'), d:get('target.name'))\n"
        "print('encounter', d:get('encounter'), d:get('encounter.value'))\n"
        "local inv = d:get('inventory') print('inv', #inv, inv[1].ammo, inv[2].spread, d:get('inventory[2].ammo'))\n"
        "print('names', table.concat(d:get('names'), ','))\n"
        "print('decls', table.concat(d:get('decls'), ','))\n"
        "print('slots', #d:get('slots'), d:get('slots[3].ammo'))\n"
        "print('corners', d:get('corners[2].z'), #d:get('corners'))\n"
        "print('tiny', d:get('flags'), d:get('precise'), d:get('tiny'), d:get('opaque'))\n"
        "local all = d:get() print('all', all.team, all.encounterMods.healthMod, all.name, all.inventory[1].ammo)\n"
        "local n, seen = 0, {} for _, f in ipairs(d:fields()) do n = n + 1 seen[f.name] = f.kind .. ':' .. f.type end\n"
        "print('fields', n, seen.name, seen.team, seen.encounterMods, seen.inventory, seen.target, seen.slots)\n"
        "local m = d:fields('encounterMods') print('mods fields', #m, m[1].name, m[2].name)\n"
        "print('leaf', d:fields('team').kind, d:fields('team').type)\n"
        "local e = enum('gameTeam_t') print('enum', e.TEAM_NONE, e.TEAM_ONE, e.TEAM_TWO)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "class\tidPlaceableSnapAIEncounter\tidAI2"));
    CHECK(has(g_log, "team\tTEAM_TWO\tTEAM_ONE"));
    CHECK(has(g_log, "behavior\tSNAP_AI_SPAWN_BEHAVIOR_HUNT"));
    CHECK(has(g_log, "mods\t1.5\t0.0"));
    CHECK(has(g_log, "name\ttyrant\timp1"));
    CHECK(has(g_log, "pos\t0.0\t12.5\t0.0"));
    CHECK(has(g_log, "spawned\ttrue"));
    CHECK(has(g_log, "target\ttrue\tTEAM_ONE\timp1"));
    CHECK(has(g_log, "encounter\tsingle_tyrant\t9"));
    CHECK(has(g_log, "inv\t2\t30\t2.0\t7"));
    CHECK(has(g_log, "names\tyo,hi"));
    CHECK(has(g_log, "decls\tsingle_tyrant,single_baron"));
    CHECK(has(g_log, "slots\t3\t99"));
    CHECK(has(g_log, "corners\t-4.0\t2"));
    CHECK(has(g_log, "tiny\t200\t0.25\t-3\tnil"));
    CHECK(has(g_log, "all\tTEAM_TWO\t1.5\ttyrant\t30"));
    CHECK(has(g_log, "fields\t17\ttext:idStr\tenum:gameTeam_t\tobject:encounterAIModifiers_t\t"
                     "list:idList < weapon_t , TAG_IDLIST , false >\tentity:idManagedClassPtr < idAI2 >\tlist:weapon_t[3]"));
    CHECK(has(g_log, "mods fields\t2\thealthMod\tdamageMod"));
    CHECK(has(g_log, "leaf\tenum\tgameTeam_t"));
    CHECK(has(g_log, "enum\t0\t1\t2"));
}

static void test_writes(void)
{
    run("api = 1\n"
        "local d, o = ent(0), ent(1)\n"
        "print(d:set('team', 'TEAM_ONE'))\n"
        "d:set('spawnBehavior', 0)\n"
        "d:set('encounterMods.healthMod', 2.5)\n"
        "d:set('encounterMods', { damageMod = 3 })\n"
        "d:set('name', 'boss')\n"
        "d:set('spawnPosition', { x = 1, z = 3 })\n"
        "d:set('spawned', false)\n"
        "d:set('encounter', 'single_baron')\n"
        "d:set('inventory[1].ammo', 5)\n"
        "d:set('slots', { { ammo = 1 }, { ammo = 2, spread = 4 }, {} })\n"
        "d:set('names[2]', 'there')\n"
        "d:set('decls[1]', 'single_baron')\n"
        "d:set('corners[2]', { x = 8 })\n"
        "d:set('flags', 7) d:set('precise', 1.5) d:set('tiny', -9)\n"
        "o:set('team', 'TEAM_TWO')\n"                 /* another's entity: the map's logic may */
        "o:set('name', 'renamed')\n"
        "print('after', d:get('team'), d:get('name'), o:get('team'))\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "true"));
    CHECK(has(g_log, "after\tTEAM_ONE\tboss\tTEAM_TWO"));
    CHECK(g_own.base.team == 1 && g_own.spawnBehavior == 0);
    CHECK(g_own.encounterMods.healthMod == 2.5f && g_own.encounterMods.damageMod == 3.0f);
    CHECK(g_own.base.name.len == 4 && !strcmp(g_own.base.name.data, "boss"));
    CHECK(g_own.base.spawnPosition.x == 1 && g_own.base.spawnPosition.y == 12.5f && g_own.base.spawnPosition.z == 3);
    CHECK(g_own.spawned == 0);
    CHECK(g_own.encounter == &g_enc2);
    CHECK(g_inv[0].ammo == 5 && g_inv[1].ammo == 7);
    CHECK(g_own.slots[0].ammo == 1 && g_own.slots[1].ammo == 2 && g_own.slots[1].spread == 4 && g_own.slots[2].ammo == 99);
    CHECK(!strcmp(g_strs[1].data, "there") && !strcmp(g_strs[0].data, "yo"));
    CHECK(g_dl[0] == &g_enc2);
    CHECK(g_own.corners[1].x == 8 && g_own.corners[1].z == -4.0f);
    CHECK(g_own.flags == 7 && g_own.precise == 1.5 && g_own.tiny == -9);
    CHECK(g_other.base.team == 2 && !strcmp(g_other.base.name.data, "renamed"));

    run("api = 1\nent(0):set('encounter', nil) print('cleared', ent(0):get('encounter'))\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "cleared\tnil") && g_own.encounter == NULL);
}

static void test_refusals(void)
{
    refused("api = 1\nent(0):get('helth')\n",
            "\"get\" could not read \"helth\": idPlaceableSnapAIEncounter has no field named helth");
    refused("api = 1\nent(0):set('team', 'TEAM_NINE')\n",
            "\"set\" could not write \"team\": gameTeam_t has no member named TEAM_NINE");
    refused("api = 1\nent(0):set('team', true)\n",
            "\"set\" could not write \"team\": it takes a member of gameTeam_t by name or number, not true or false");
    refused("api = 1\nent(0):set('encounterMods.healthMod', 'big')\n",
            "\"set\" could not write \"encounterMods.healthMod\": it takes a number, not text");
    refused("api = 1\nent(0):set('spawned', 1)\n", "\"set\" could not write \"spawned\": it takes true or false, not a number");
    refused("api = 1\nent(0):set('name', 5)\n", "\"set\" could not write \"name\": it takes text, not a number");
    refused("api = 1\nent(0):set('tiny', 1.5)\n", "\"set\" could not write \"tiny\": it takes a whole number, not a number");
    refused("api = 1\nent(0):set('encounter', 'single_none')\n",
            "\"set\" could not write \"encounter\": the game has no idDeclSnapAIEncounter named single_none");
    refused("api = 1\nent(0):set('encounterMods', { helthMod = 2 })\n",
            "\"set\" could not write \"encounterMods\": encounterAIModifiers_t has no field named helthMod");
    refused("api = 1\nent(0):set('spawnPosition', { x = 'far' })\n",
            "\"set\" could not write \"spawnPosition\": x: it takes a number, not text");
    refused("api = 1\nent(0):set('slots', { {} })\n",
            "\"set\" could not write \"slots\": the list holds 3 elements and the value has 1; a script sets a list's "
            "elements, not its length");
    refused("api = 1\nent(0):set('target', ent(1))\n",
            "\"set\" could not write \"target\": it is a reference the game keeps by itself");
    refused("api = 1\nent(0):set('opaque', 1)\n", "\"set\" could not write \"opaque\": it points at a idWeird");
    refused("api = 1\nent(0):get('inventory[0]')\n", "a list's first element is [1]");
    refused("api = 1\nent(0):get('inventory[3]')\n",
            "\"get\" could not read \"inventory[3]\": inventory: the list holds 2 elements, so it has no [3]");
    refused("api = 1\nent(0):set('', 1)\n", "\"set\" needs the field to write, such as \"team\"");
    refused("api = 1\nenum('noSuch_t')\n", "\"enum\" found no enum named noSuch_t in the game");
    g_ready = 0;
    refused("api = 1\nent(0):get('team')\n",
            "\"get\" is unavailable: the game's type information could not be read in this session");
    g_ready = 1;
}

static void *h_resolve(uint32_t id) { return f_resolve(id); }

int main(void)
{
    g_engine_fake.resolve = f_resolve;
    g_engine_fake.spawn_id = f_spawn_id;
    g_engine_fake.slots = f_slots;
    g_engine_fake.slot_spawn_id = f_slot_spawn_id;
    g_engine_fake.kind = f_kind;
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ent", t_ent);
    sh_script_api_set_engine(&g_engine_fake);
    sh_script_api_install(NULL, 0, NULL);
    sh_script_api_reflect_register();
    sh_script_api_reflect_set(&k_host, &k_ops);
    sh_script_handles_set_resolver(h_resolve);

    test_reads();
    test_writes();
    test_refusals();
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_reflect_test: FAILED\n" : "script_api_reflect_test: ok\n");
    return g_fail;
}
