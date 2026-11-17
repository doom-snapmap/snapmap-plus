/* script_api_body_test.c -- sync_partner, wounds_open and break_constraint of
 * mod scripts (src/backend/script/script_api_body.c) over a fake type table
 * shaped like the game's (idSyncEntity.activeParticipants, the gore
 * component's wound lists, an idAnimator_AF's physics constraints) and real
 * structs: a helper body bound in a sync with its target and the player, a
 * stock entity with none of the parts, and the constraint removal. */
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
#include "script/script_api_body.h"
#include "script/script_reflect.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define SALT "body1"

/* ---- the objects ---- */
typedef struct list { void *ptr; int num; int size; int pad; int pad2; } list;          /* 0x18 */
typedef struct ref { int spawn_id; int serial; void *cached; char rest[16]; } ref;      /* 32 */
typedef struct node { void *vt; const char *name; int v; } node;                       /* a gore node: name at +8 */
typedef struct wound { node *woundNode; char rest[16]; } wound;
typedef struct gorecomp { list activeWounds; list pendingWounds; } gorecomp;
typedef struct afc { void *vt; const char *name; int kind; } afc;                       /* idAFConstraint */
typedef struct phys { int pad; list constraints; } phys;
typedef struct animaf { int pad; phys physicsObj; } animaf;
typedef struct ent { void *vt; int pad; } ent;
typedef struct goreent { ent base; gorecomp goreComponent; animaf afProperties; } goreent;
typedef struct aient { ent base; gorecomp goreComponent; } aient;
typedef struct sp { int type; } sp;                                                    /* idSyncParticipant */
typedef struct asp { ref instance; sp *data; } asp;                                    /* idActiveSyncParticipant */
typedef struct syncent { ent base; list activeParticipants; } syncent;

#define F(T, name, type, ops) { type, ops, #name, (uint32_t)offsetof(T, name), (uint32_t)sizeof(((T *)0)->name) }
static const sh_rf_field k_wound[] = { F(wound, woundNode, "idDeclGoreNode", "*") };
static const sh_rf_field k_gore[] = { F(gorecomp, activeWounds, "idList < woundData_t , TAG_IDLIST , false >", ""),
                                      F(gorecomp, pendingWounds, "idList < woundData_t , TAG_IDLIST , false >", "") };
static const sh_rf_field k_afc[] = { F(afc, name, "idAtomicString", "") };
static const sh_rf_field k_phys[] = { F(phys, constraints, "idList < idAFConstraint * , TAG_AF , false >", "") };
static const sh_rf_field k_animaf[] = { F(animaf, physicsObj, "idPhysics_AF", "") };
static const sh_rf_field k_goreent[] = { F(goreent, goreComponent, "idGoreComponent", ""),
                                         F(goreent, afProperties, "idAnimator_AF", "") };
static const sh_rf_field k_aient[] = { F(aient, goreComponent, "idGoreComponent", "") };
static const sh_rf_field k_sp[] = { F(sp, type, "idSyncEntity::participantType_t", "") };
static const sh_rf_field k_asp[] = { F(asp, instance, "idManagedClassPtr < idAnimatedEntity >", ""),
                                     F(asp, data, "idSyncParticipant", "*") };
static const sh_rf_field k_sync[] = { F(syncent, activeParticipants,
                                        "idList < idActiveSyncParticipant , TAG_IDLIST , false >", "") };
static const sh_rf_field k_none[] = { { "int", "", "pad", 8, 4 } };

typedef struct cls { const char *name, *super; const sh_rf_field *fields; uint32_t count, size; } cls;
#define C(name, super, arr, T) { name, super, arr, (uint32_t)(sizeof arr / sizeof arr[0]), (uint32_t)sizeof(T) }
static const cls k_classes[] = {
    C("idEntity", "", k_none, ent), C("idAnimatedEntity", "idEntity", k_none, ent),
    C("idGoreEntity", "idAnimatedEntity", k_goreent, goreent), C("idAI2", "idAnimatedEntity", k_aient, aient),
    C("idSyncEntity", "idAnimatedEntity", k_sync, syncent), C("idGoreComponent", "", k_gore, gorecomp),
    C("woundData_t", "", k_wound, wound), C("idDeclGoreNode", "", k_none, node), C("idAFConstraint", "", k_afc, afc),
    C("idPhysics_AF", "", k_phys, phys), C("idAnimator_AF", "", k_animaf, animaf),
    C("idSyncParticipant", "", k_sp, sp), C("idActiveSyncParticipant", "", k_asp, asp),
};
typedef struct en { const char *type, *member; long long value; } en;
static const en k_enums[] = {
    { "idSyncEntity::participantType_t", "INSTIGATOR", 0 }, { "idSyncEntity::participantType_t", "TARGET", 1 },
    { "idSyncEntity::participantType_t", "HELPER", 2 },
};
static const cls *find_cls(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++) if (!strcmp(k_classes[i].name, name)) return &k_classes[i];
    return NULL;
}
static int g_ready = 1;
static int h_size(void *c, const char *type, uint32_t *size)
{
    const cls *k = find_cls(type);
    (void)c;
    if (k && size) *size = k->size;
    return g_ready && k != NULL;
}
static const char *h_super(void *c, const char *type) { const cls *k = find_cls(type); (void)c; return k ? k->super : NULL; }
static int h_field(void *c, const char *type, uint32_t index, sh_rf_field *out)
{
    const cls *k = find_cls(type);
    (void)c;
    if (!k || index >= k->count || k->fields == k_none) return 0;
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

/* ---- the world ---- */
#define ID_BODY   0x10000u    /* the mod's helper body (idGoreEntity) */
#define ID_DEMON  0x10001u    /* the mod's demon, the sync's target */
#define ID_PLAYER 5u
#define ID_SYNC   40u
#define ID_STOCK  77u         /* a stock entity: no gore, no figure, no sync */
static goreent g_body;
static aient g_demon;
static ent g_player, g_stock;
static syncent g_sync;
static node g_nodes[3] = { { NULL, "head_severed", 0 }, { NULL, "gk_eye_stab", 0 }, { NULL, "chest_chunk_left", 0 } };
static wound g_active[2], g_pending[2], g_body_active[1];
static afc g_c[3];
static afc *g_cl[3];
static sp g_types[3];
static asp g_parts[3];

static void *f_resolve(uint32_t id)
{
    return id == ID_BODY ? (void *)&g_body : id == ID_DEMON ? (void *)&g_demon : id == ID_PLAYER ? (void *)&g_player
         : id == ID_SYNC ? (void *)&g_sync : id == ID_STOCK ? (void *)&g_stock : NULL;
}
static uint32_t f_spawn_id(void *e)
{
    return e == &g_body ? ID_BODY : e == &g_demon ? ID_DEMON : e == &g_player ? ID_PLAYER : e == &g_sync ? ID_SYNC
         : e == &g_stock ? ID_STOCK : SH_SCRIPT_SPAWN_ID_NONE;
}
static const uint32_t k_slots[] = { ID_PLAYER, ID_STOCK, ID_DEMON, ID_SYNC, ID_BODY };
static int f_slots(void) { return 5; }
static uint32_t f_slot_spawn_id(int n) { return n >= 0 && n < 5 ? k_slots[n] : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_kind(void *e) { return e == &g_demon ? SH_SCRIPT_KIND_AI : e == &g_player ? SH_SCRIPT_KIND_PLAYER : SH_SCRIPT_KIND_OTHER; }
static sh_script_engine g_engine_fake;

static const char *o_class(void *c, void *e)
{
    (void)c;
    return e == &g_body ? "idGoreEntity" : e == &g_demon ? "idAI2" : e == &g_sync ? "idSyncEntity" : "idEntity";
}
static const char *o_res_name(void *c, const void *object) { (void)c; return ((const node *)object)->name; }
static void o_push_entity(lua_State *L, void *c, void *e) { (void)c; sh_script_api_push_engine_entity(L, e); }
static const sh_rf_host k_host = { NULL, h_size, h_super, h_field, h_enum_value, h_enum_name, h_read, NULL, NULL };
static const sh_script_reflect_ops k_ops = { NULL, o_class, NULL, NULL, NULL, o_res_name, NULL, o_push_entity, NULL, NULL };

/* the game's removal, faked: the constraint whose name field is given leaves the list */
static char g_deleted[256];
static int g_delete_runs;
static int fake_delete(void *physics, void *name_field)
{
    phys *p = (phys *)physics;
    int i, k;
    g_delete_runs++;
    for (i = 0; i < p->constraints.num; i++) {
        afc *c = ((afc **)p->constraints.ptr)[i];
        if ((void *)&c->name != name_field) continue;
        strcat_s(g_deleted, sizeof g_deleted, c->name);
        strcat_s(g_deleted, sizeof g_deleted, ";");
        for (k = i; k + 1 < p->constraints.num; k++) ((afc **)p->constraints.ptr)[k] = ((afc **)p->constraints.ptr)[k + 1];
        p->constraints.num--;
        return 1;
    }
    return 1;
}

static void world(int in_sync)
{
    int i;
    memset(&g_body, 0, sizeof g_body); memset(&g_demon, 0, sizeof g_demon); memset(&g_sync, 0, sizeof g_sync);
    g_active[0].woundNode = &g_nodes[0]; g_active[1].woundNode = &g_nodes[1];
    g_pending[0].woundNode = &g_nodes[2]; g_pending[1].woundNode = &g_nodes[0];   /* pending and open: once */
    g_demon.goreComponent.activeWounds.ptr = g_active; g_demon.goreComponent.activeWounds.num = 2;
    g_demon.goreComponent.pendingWounds.ptr = g_pending; g_demon.goreComponent.pendingWounds.num = 2;
    g_body_active[0].woundNode = &g_nodes[1];
    g_body.goreComponent.activeWounds.ptr = g_body_active; g_body.goreComponent.activeWounds.num = 1;
    g_c[0].name = "cstorsohorizontala_spine_part03_md";
    g_c[1].name = "cshead_head_part01_md";
    g_c[2].name = "csleglowerrt_leg_lower_rt";
    for (i = 0; i < 3; i++) g_cl[i] = &g_c[i];
    g_body.afProperties.physicsObj.constraints.ptr = g_cl; g_body.afProperties.physicsObj.constraints.num = 3;
    g_types[0].type = 0; g_types[1].type = 1; g_types[2].type = 2;
    g_parts[0].instance.spawn_id = (int)ID_PLAYER; g_parts[0].data = &g_types[0];
    g_parts[1].instance.spawn_id = (int)ID_DEMON; g_parts[1].data = &g_types[1];
    g_parts[2].instance.spawn_id = (int)ID_BODY; g_parts[2].data = &g_types[2];
    g_sync.activeParticipants.ptr = g_parts; g_sync.activeParticipants.num = in_sync ? 3 : 0;
    g_deleted[0] = 0;
    g_delete_runs = 0;
}

/* ---- the runtime ---- */
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

static int t_ent(lua_State *L)
{
    static const uint32_t ids[] = { ID_BODY, ID_DEMON, ID_PLAYER, ID_SYNC, ID_STOCK };
    lua_Integer n = luaL_checkinteger(L, 1);
    uint32_t id = (n >= 0 && n < 5) ? ids[n] : ID_STOCK;
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
static int has(const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    if (!ok) printf("  missing \"%s\" in:\n%s\n", needle, hay);
    return ok;
}

static void run(const char *body, int in_sync)
{
    sh_script_mod_desc d;
    const char *names[1];
    sh_script_map_end();
    world(in_sync);
    strcpy_s(g_text, sizeof g_text, body);
    d.salt = SALT;
    d.name = "Body Test";
    sh_script_map_start(&d, 1, &g_src, 3, 5.0);
    names[0] = "mods/" SALT "/characters/body";
    sh_script_hooks_record(ID_BODY, names, 1, NULL);
    names[0] = "mods/" SALT "/ai/demon";
    sh_script_hooks_record(ID_DEMON, names, 1, NULL);
    sh_script_api_frame();
    sh_script_tick(5.0, 0.016);
}

static void test_sync_partner(void)
{
    run("api = 1\n"
        "local b, d, p, s, k = ent(0), ent(1), ent(2), ent(3), ent(4)\n"
        "local t, i, se = b:sync_partner()\n"
        "print('body', t == d, i == p, se == s)\n"
        "local t2, i2 = d:sync_partner() print('demon', t2 == d, i2 == p)\n"
        "print('stock', k:sync_partner())\n", 1);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "body\ttrue\ttrue\ttrue"));
    CHECK(has(g_log, "demon\ttrue\ttrue"));
    CHECK(has(g_log, "stock\tnil\tthe entity is in no sync now"));
    run("api = 1\nprint('ended', ent(0):sync_partner())\n", 0);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "ended\tnil\tthe entity is in no sync now"));
}

static void test_wounds_open(void)
{
    run("api = 1\n"
        "local w = ent(1):wounds_open() print('demon', #w, table.concat(w, ','))\n"
        "local v = ent(0):wounds_open() print('body', #v, v[1])\n"
        "print('stock', ent(4):wounds_open())\n", 1);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "demon\t3\thead_severed,gk_eye_stab,chest_chunk_left"));
    CHECK(has(g_log, "body\t1\tgk_eye_stab"));
    CHECK(has(g_log, "stock\tnil\tthis entity has no gore component"));
}

static void test_break_constraint(void)
{
    run("api = 1\n"
        "local b = ent(0)\n"
        "print('one', b:break_constraint('CSHead_Head_Part01_Md', { x = 1, y = 1, z = 0 }))\n"
        "print('again', b:break_constraint('cshead_head_part01_md'))\n"
        "print('stock', ent(4):break_constraint('x'))\n", 1);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "one\ttrue"));
    CHECK(has(g_log, "again\tfalse\tthe articulated figure has no constraint named \"cshead_head_part01_md\" (it may be broken already)"));
    CHECK(has(g_log, "stock\tfalse\tthis entity has no articulated figure"));
    CHECK(!strcmp(g_deleted, "cshead_head_part01_md;"));
    CHECK(g_delete_runs == 1);
    CHECK(g_body.afProperties.physicsObj.constraints.num == 2);

    sh_script_api_body_set_delete(NULL);
    run("api = 1\nprint('nofn', ent(0):break_constraint('cshead_head_part01_md'))\n", 1);
    CHECK(has(log_now(), "nofn\tfalse\tthis Snapmap+ could not find the game's constraint removal in this executable"));
    CHECK(g_delete_runs == 0);
    sh_script_api_body_set_delete(fake_delete);

    run("api = 1\nent(0):break_constraint(12)\n", 1);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
}

static void test_no_types(void)
{
    g_ready = 0;
    run("api = 1\nprint('types', ent(0):sync_partner())\nprint('w', ent(1):wounds_open())\n"
        "print('c', ent(0):break_constraint('a'))\n", 1);
    g_ready = 1;
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "types\tnil\tthe game's type information could not be read in this session"));
    CHECK(has(g_log, "w\tnil\tthe game's type information"));
    CHECK(has(g_log, "c\tfalse\tthe game's type information"));
}

static void test_has_gore(void)
{
    CHECK(sh_script_body_has_gore(&g_body));
    CHECK(sh_script_body_has_gore(&g_demon));
    CHECK(!sh_script_body_has_gore(&g_stock));
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
    sh_script_api_body_register();
    sh_script_api_body_set_delete(fake_delete);
    sh_script_handles_set_resolver(h_resolve);

    test_sync_partner();
    test_wounds_open();
    test_break_constraint();
    test_no_types();
    test_has_gore();
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_body_test: FAILED\n" : "script_api_body_test: ok\n");
    return g_fail;
}
