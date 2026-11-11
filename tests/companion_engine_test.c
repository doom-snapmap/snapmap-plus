/* companion_engine_test.c -- the engine side of owner-bound companions
 * (companion_engine.c) against a faked engine table: the entityDef manager,
 * the game's create and finish calls (finish runs CallSpawn, so a companion's
 * own spawn re-enters the detour), the owner's physics and a serving index
 * with two builds of one salt. Checks which build an owner's companions come
 * from and that a new index never replaces the tables a spawn loop reads. */
#include "../src/backend/companion_engine.c"

static int t_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        t_failed++;                                                             \
    }                                                                           \
} while (0)

static char g_log[32768];
void backend_log(const char *message)
{
    size_t at = strlen(g_log), n = message ? strlen(message) : 0;
    if (at + n + 2 < sizeof g_log) { memcpy(g_log + at, message, n); g_log[at + n] = '\n'; g_log[at + n + 1] = 0; }
}

/* ------------------------------------------------------------------ link stand-ins */

const sig_entry SH_COMPANION_SIGNATURES[] = { { NULL } };
const sig_entry SH_MODSCENE_SPAWN_SIGNATURES[] = { { NULL } };
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; (void)out; return (sig_status)1; }
void *hook_prepare(void *target, void *detour, size_t stolen) { (void)target; (void)detour; (void)stolen; return NULL; }
const char *hook_last_failure(void) { return ""; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; return (sh_patch_status)0; }
int hook_is_installed(void *tramp) { (void)tramp; return 0; }
int hook_unpatch(void *tramp) { (void)tramp; return 0; }
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{ (void)module_base; (void)name; (void)out_status; return 0; }

/* The serving index: admitted builds with their extra, and which build serves
 * each owner's entityDef for the open map. */
typedef struct fake_build { const char *salt, *hash, *extra; int place; } fake_build;
static fake_build g_fbuilds[4];
static int g_nfbuilds;
static const char *g_owner_request, *g_owner_hash;
static sh_modserve_stats g_stats;
static int g_extra_reads;

int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context)
{
    int i;
    g_extra_reads++;
    for (i = 0; i < g_nfbuilds; i++) fn(context, g_fbuilds[i].salt, g_fbuilds[i].hash, g_fbuilds[i].place, g_fbuilds[i].extra);
    return g_nfbuilds;
}
void sh_modserve_get_stats(sh_modserve_stats *out) { *out = g_stats; }
int sh_modserve_lookup(const char *request, sh_modserve_hit *out)
{
    if (!g_owner_request || _stricmp(request, g_owner_request)) return 0;
    memset(out, 0, sizeof *out);
    out->bundle_hash = g_owner_hash;
    return 1;
}

/* ------------------------------------------------------------------ the faked engine */

typedef struct fe_decl { unsigned char b[0x80]; } fe_decl;          /* +0x08 name, +0x2c state, +0x58 inherit, +0x60 class */
typedef struct fe_ent { unsigned char b[0x900]; } fe_ent;           /* +0x6d0 decl, +0x808 physics, +0x3a8 pose */
static fe_decl g_decls[8];
static const char *g_decl_names[8];
static int g_ndecls;
static fe_ent g_ents[16];
static int g_nents;
static const fe_decl *t_spawned[16];
static int t_nspawned;
static void *g_phys_vt[32];
static void *g_phys[1] = { g_phys_vt };
static void *g_gm_vt[0x460 / 8 + 1];
static void *g_gm[1] = { g_gm_vt };
static void *g_reg_vt[16];
static void *g_reg_obj[1] = { g_reg_vt };
static void *g_reg_slot = g_reg_obj;
static unsigned char g_anchor_bytes[0x40];
static const char *g_change_index_at;   /* the companion whose spawn publishes a new index (another thread's install) */
static int g_built_during_nested;

static const float g_origin[3] = { 0, 0, 0 };
static const float g_axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static const float *fe_get_origin(void *phys, int id) { (void)phys; (void)id; return g_origin; }
static const float *fe_get_axis(void *phys, int id) { (void)phys; (void)id; return g_axis; }

static fe_decl *fe_decl_add(const char *name, const char *inherit)
{
    fe_decl *d = &g_decls[g_ndecls];
    memset(d, 0, sizeof *d);
    g_decl_names[g_ndecls++] = name;
    *(const char **)(d->b + 0x08) = name;
    *(const char **)(d->b + 0x58) = inherit;
    *(const char **)(d->b + 0x60) = "idProp_Coop";
    return d;
}
static void *fe_find_decl(void *manager, const char *name, unsigned char create)
{
    int i;
    (void)manager; (void)create;
    for (i = 0; i < g_ndecls; i++) if (!strcmp(g_decl_names[i], name)) return &g_decls[i];
    return NULL;
}
static void *fe_type(void *r, const char *type) { (void)r; (void)type; return (void *)1; }
static unsigned char fe_register(void *r, const void *p, void *x) { (void)r; (void)p; (void)x; return 1; }

static fe_ent *fe_entity(fe_decl *decl)
{
    fe_ent *e = &g_ents[g_nents++];
    memset(e, 0, sizeof *e);
    *(void **)(e->b + 0x6d0) = decl;
    *(void **)(e->b + 0x808) = g_phys;
    return e;
}
static void *fe_cast(void *obj) { return obj; }
static void *fe_create(void *gm, void *decl, void *args) { (void)gm; (void)args; return fe_entity((fe_decl *)decl); }
/* finish registers the companion and runs CallSpawn: the detour again, one level down. */
static void fe_finish(void *gm, void *ent, int entnum, unsigned long long spawn_id, int arg5, void *arg6)
{
    const fe_decl *d = *(const fe_decl **)((unsigned char *)ent + 0x6d0);
    (void)gm; (void)entnum; (void)spawn_id; (void)arg5; (void)arg6;
    t_spawned[t_nspawned++] = d;
    if (g_change_index_at && !strcmp(*(const char **)(d->b + 0x08), g_change_index_at)) {
        int reads = g_extra_reads;
        g_stats.build_ms += 1.0;   /* published by another thread while the owner's loop runs */
        callspawn_detour(ent);
        if (g_extra_reads != reads) g_built_during_nested = 1;
        return;
    }
    callspawn_detour(ent);
}
static void fe_callspawn(void *obj) { (void)obj; }
static void fe_bind(void *ent, void *master, unsigned flags) { (void)ent; (void)master; (void)flags; }
static void fe_add_target(void *ent, void *target) { (void)ent; (void)target; }
static unsigned char fe_is_client(void *gm) { (void)gm; return 0; }
void *sh_entity_gamemgr(void) { return g_gm; }

static void fe_reset(void)
{
    int32_t disp;
    memset(g_decls, 0, sizeof g_decls);
    g_ndecls = g_nents = t_nspawned = 0;
    g_phys_vt[0x80 / 8] = (void *)fe_get_origin;
    g_phys_vt[0x88 / 8] = (void *)fe_get_axis;
    g_gm_vt[0x448 / 8] = (void *)fe_create;
    g_gm_vt[0x458 / 8] = (void *)fe_finish;
    g_reg_vt[7] = (void *)fe_register;
    g_reg_vt[11] = (void *)fe_type;
    memset(g_anchor_bytes, 0, sizeof g_anchor_bytes);
    memcpy(g_anchor_bytes + 0x10, "\x48\x8b\x0d", 3);
    disp = (int32_t)((intptr_t)&g_reg_slot - (intptr_t)(g_anchor_bytes + 0x17));
    memcpy(g_anchor_bytes + 0x13, &disp, 4);
    g_anchor = g_anchor_bytes;
    g_register = (void *)fe_register;
    g_type = (type_fn)fe_type;
    g_decl = (find_decl_fn)fe_find_decl;
    g_create = (create_fn)fe_create;
    g_finish = (finish_fn)fe_finish;
    g_is_entity = (cast_fn)fe_cast;
    g_is_client = (is_client_fn)fe_is_client;
    g_bind = (bind_fn)fe_bind;
    g_add_target = (add_target_fn)fe_add_target;
    g_orig = (callspawn_fn)fe_callspawn;
    g_resolved = 1;
    g_change_index_at = NULL;
    g_built_during_nested = 0;
    g_log_lines = 0;
    g_log[0] = 0;
}

#define ROOT_HASH  "1111111111111111111111111111111111111111111111111111111111111111"
#define PLACE_HASH "2222222222222222222222222222222222222222222222222222222222222222"

/* A stranger's map that brought its own copy of the salt spawns that copy's
 * companions, not the root folder's; with the root's build serving the owner,
 * the root's. */
static void test_companions_from_serving_build(void)
{
    fe_decl *owner;
    fe_ent *e;
    fe_reset();
    owner = fe_decl_add("mods/abcdefgh/door", NULL);
    fe_decl_add("mods/abcdefgh/light_root", NULL);
    fe_decl_add("mods/abcdefgh/light_place", NULL);
    g_nfbuilds = 2;
    g_fbuilds[0].salt = "abcdefgh"; g_fbuilds[0].hash = ROOT_HASH; g_fbuilds[0].place = 0;
    g_fbuilds[0].extra = "{\"companions\": {\"mods/abcdefgh/door\": [{\"entityDef\": \"mods/abcdefgh/light_root\"}]}}";
    g_fbuilds[1].salt = "abcdefgh"; g_fbuilds[1].hash = PLACE_HASH; g_fbuilds[1].place = 1;
    g_fbuilds[1].extra = "{\"companions\": {\"mods/abcdefgh/door\": [{\"entityDef\": \"mods/abcdefgh/light_place\"}]}}";
    g_owner_request = "generated/decls/entitydef/mods/abcdefgh/door.decl";
    g_owner_hash = PLACE_HASH;
    CHECK(build_table() == 2);
    e = fe_entity(owner);
    callspawn_detour(e);
    CHECK(t_nspawned == 1 && t_spawned[0] == &g_decls[2]);
    g_owner_hash = ROOT_HASH;
    t_nspawned = 0;
    callspawn_detour(fe_entity(owner));
    CHECK(t_nspawned == 1 && t_spawned[0] == &g_decls[1]);
    /* Nothing serves the owner now: no companions. */
    g_owner_hash = "3333333333333333333333333333333333333333333333333333333333333333";
    t_nspawned = 0;
    callspawn_detour(fe_entity(owner));
    CHECK(t_nspawned == 0);
}

/* A new index published while an owner's companions spawn (a companion in the
 * mods namespace re-enters the detour from its own CallSpawn) is not read
 * there: the owner's loop finishes over the tables it started with, and the
 * next outermost spawn reads the new index. */
static void test_rebuild_waits_for_the_outer_spawn(void)
{
    fe_decl *owner;
    fe_reset();
    owner = fe_decl_add("mods/abcdefgh/door", NULL);
    fe_decl_add("mods/abcdefgh/c1", NULL);
    fe_decl_add("mods/abcdefgh/c2", NULL);
    fe_decl_add("mods/abcdefgh/c3", NULL);
    g_nfbuilds = 1;
    g_fbuilds[0].salt = "abcdefgh"; g_fbuilds[0].hash = ROOT_HASH; g_fbuilds[0].place = 0;
    g_fbuilds[0].extra = "{\"companions\": {\"mods/abcdefgh/door\": [{\"entityDef\": \"mods/abcdefgh/c1\"}, "
                         "{\"entityDef\": \"mods/abcdefgh/c2\"}, {\"entityDef\": \"mods/abcdefgh/c3\"}]}}";
    g_owner_request = "generated/decls/entitydef/mods/abcdefgh/door.decl";
    g_owner_hash = ROOT_HASH;
    InterlockedExchange(&g_stale, 0);
    CHECK(build_table() == 1);
    g_change_index_at = "mods/abcdefgh/c1";
    g_extra_reads = 0;
    callspawn_detour(fe_entity(owner));
    CHECK(!g_built_during_nested && g_extra_reads == 0);
    CHECK(t_nspawned == 3 && t_spawned[0] == &g_decls[1] && t_spawned[1] == &g_decls[2] && t_spawned[2] == &g_decls[3]);
    /* The next outermost spawn reads the new index. */
    g_change_index_at = NULL;
    t_nspawned = 0;
    callspawn_detour(fe_entity(owner));
    CHECK(g_extra_reads == 1 && t_nspawned == 3);
}

int main(void)
{
    test_companions_from_serving_build();
    test_rebuild_waits_for_the_outer_spawn();
    free_tables();
    if (t_failed) {
        fprintf(stderr, "companion_engine_test: %d check(s) failed\n--- log ---\n%s", t_failed, g_log);
        return 1;
    }
    printf("companion_engine_test: all checks passed\n");
    return 0;
}
