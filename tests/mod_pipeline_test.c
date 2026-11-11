/* mod_pipeline_test -- the three mod layers linked together, the way the
 * backend links them: a folder built by the bundle builder (modbundle.c) is
 * admitted by the serving index (modserve.c); a save writes one smpmod1 row
 * through the serving layer's closure (modmanifest.c); the next load strips
 * the row and activates exactly that bundle's gameplay decls; a skins decl
 * outside the salt's namespace is refused; and a cut-content bundle's
 * requirements are applied before its registration.
 *
 * Engine stand-ins: modserve_activate_bundles runs the real plan against
 * fake registry operations and then the real map-row check against a fake
 * palette (lane M6: rows of a named bundle that the built palette lacks are
 * registered by one rebuild before the map parses). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modstore.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[16384];
void backend_log(const char *line)
{
    size_t at = strlen(g_log), n = line ? strlen(line) : 0;
    if (line && at + n + 2 < sizeof g_log) { memcpy(g_log + at, line, n); g_log[at + n] = '\n'; g_log[at + n + 1] = 0; }
}

/* ---- stock names (as modbundle_test) -------------------------------------------------------- */

static const char *const STOCK[] = {
    "generated/decls/snapeditorentitydef/demons/demon_base.decl",
    "generated/decls/material/textures/guis/snapmaps/entity_icons/demons/imp_enc.decl",
    "generated/decls/entitydef/ai/demon/imp_coop.decl",
    "generated/decls/entitydef/snapmaps/placeablesnapaiencounter/imp.decl",
    "generated/decls/renderprog/skinvmtr.decl",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/rage_taunt.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward01.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward02.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/idle.bmd6anim",
    "generated/decls/entitydef/monster/imp.decl",
    NULL
};
static int stock_exists(void *ctx, const char *request)
{
    int i;
    (void)ctx;
    for (i = 0; STOCK[i]; i++) if (!strcmp(STOCK[i], request)) return 1;
    return 0;
}

/* ---- builder entry (modcmd.c in the product) ------------------------------------------------ */

int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = 0;
    if (!sh_mb_build(folder, NULL, &r, &b)) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    else if (!sh_mb_write(&b, folder, cause, sizeof cause)) strncpy_s(first_cause, n, cause, _TRUNCATE);
    else ok = 1;
    if (!ok) printf("    builder refused %s: %s\n", folder, first_cause);
    sh_mb_free(&b);
    return ok;
}

/* ---- registry stand-in ---------------------------------------------------------------------- */

#define MAX_REC 1024
static char g_registered[MAX_REC][160];
static char g_registered_type[MAX_REC][32];
static int g_nregistered, g_loads, g_activations;
static int g_last_steps, g_last_ready, g_last_stock;

/* The engine's own decls: anything without one of these prefixes
 * (acceptance/ is the carrier-1 package's first-generation namespace). */
static int fake_stock(const char *name)
{
    return strncmp(name, "mods/", 5) && strncmp(name, "custom/", 7) && strncmp(name, "local/", 6) &&
           strncmp(name, "acceptance/", 11);
}

/* Typed, as the engine's managers are: an entityDef and a sedef may share a name. */
static int fake_known(const char *type, const char *name)
{
    int i;
    if (fake_stock(name)) return 1;
    for (i = 0; i < g_nregistered; i++)
        if (!_stricmp(g_registered[i], name) && !_stricmp(g_registered_type[i], type)) return 1;
    return 0;
}
static int op_source(void *c, const char *type, const char *name) { (void)c; return fake_known(type, name); }
static int op_register(void *c, const char *type, const char *name, const char *reg)
{
    (void)c; (void)reg;
    if (g_nregistered < MAX_REC) {
        strncpy_s(g_registered_type[g_nregistered], 32, type, _TRUNCATE);
        strncpy_s(g_registered[g_nregistered++], 160, name, _TRUNCATE);
    }
    return 1;
}
static int op_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    (void)c; (void)type; (void)name;
    g_loads++;
    *state = 0;
    *lifetime = persist ? 4u : 1u;
    return 1;
}
static const sh_modserve_engine_ops OPS = { NULL, op_source, op_register, op_load };

/* ---- cvar stand-in: the product's route (idCVar::Set, immediate) ---------------------------- */

static int g_gate[2] = { 1, 1 };         /* g_useImageBlackList, g_useResourceBlackList */
static int g_gate_reads, g_gate_sets, g_registered_at_first_set = -1;
static int gate_slot(const char *cvar) { return !strcmp(cvar, "g_useImageBlackList") ? 0 : !strcmp(cvar, "g_useResourceBlackList") ? 1 : -1; }
static int gate_read(void *c, const char *cvar, char *value, size_t cap)
{
    int g = gate_slot(cvar);
    (void)c;
    g_gate_reads++;
    if (g < 0) return 0;
    _snprintf_s(value, cap, _TRUNCATE, "%d", g_gate[g]);
    return 1;
}
static int gate_set(void *c, const char *cvar, const char *value)
{
    int g = gate_slot(cvar);
    (void)c;
    if (g < 0) return 0;
    if (!g_gate_sets++) g_registered_at_first_set = g_nregistered;
    g_gate[g] = atoi(value);
    return SH_MODSERVE_GATE_SET;
}
static const sh_modserve_gate_ops GATES = { NULL, gate_read, gate_set, NULL };

/* ---- palette stand-in: the engine's own rows plus what a rebuild admitted -------------------- */

static int g_pal_built, g_pal_count, g_rebuilds, g_rebuild_result = 1;
static char g_pal_rows[MAX_REC][160];
static sh_modserve_rows_sync g_last_sync;
static int g_last_sync_r;

static int pal_ready(void *c) { (void)c; return g_pal_built; }
static int pal_has(void *c, const char *name)
{
    int i;
    (void)c;
    if (fake_stock(name)) return 1;
    for (i = 0; i < g_pal_count; i++) if (!_stricmp(g_pal_rows[i], name)) return 1;
    return 0;
}
/* As the detour does: the editor pass registers pending rows, then the
 * builder admits every ready row. */
static int pal_rebuild(void *c, int *before, int *after)
{
    sh_modserve_plan plan;
    int i;
    (void)c;
    g_rebuilds++;
    *before = 1000 + g_pal_count;
    if (g_rebuild_result != 1) { *after = -1; return g_rebuild_result; }
    if (sh_modserve_plan_editor(&plan) >= 0) {
        sh_modserve_plan_run_gated(&plan, &OPS, &GATES, "editor", NULL);
        for (i = 0; i < plan.count; i++)
            if (plan.steps[i].rank == SH_MODSERVE_RANK_SEDEF && plan.steps[i].ready == 1 && !pal_has(NULL, plan.steps[i].name) &&
                g_pal_count < MAX_REC)
                strncpy_s(g_pal_rows[g_pal_count++], 160, plan.steps[i].name, _TRUNCATE);
        sh_modserve_plan_free(&plan);
    }
    *after = 1000 + g_pal_count;
    return 1;
}
static const sh_modserve_palette_ops PAL_OPS = { NULL, pal_ready, pal_has, pal_rebuild };

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    sh_modserve_plan plan;
    int unknown = 0, ready = 0, i;
    if (n <= 0 || !bundle_hashes) return 0;
    g_activations++;
    if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) return -1;
    if (plan.count) ready = sh_modserve_plan_run_gated(&plan, &OPS, &GATES, "map", NULL);
    g_last_steps = plan.count;
    g_last_ready = ready;
    g_last_stock = 0;
    for (i = 0; i < plan.count; i++) if (plan.steps[i].shadow && plan.steps[i].ready == 1) g_last_stock++;
    sh_modserve_plan_free(&plan);
    g_last_sync_r = sh_modserve_map_rows_sync(bundle_hashes, n, &PAL_OPS, &g_last_sync);
    return ready;
}

static int registered(const char *name)
{
    int i;
    for (i = 0; i < g_nregistered; i++) if (!_stricmp(g_registered[i], name)) return 1;
    return 0;
}

static int registered_as(const char *type, const char *name)
{
    int i;
    for (i = 0; i < g_nregistered; i++)
        if (!_stricmp(g_registered_type[i], type) && !_stricmp(g_registered[i], name)) return 1;
    return 0;
}

/* ---- helpers -------------------------------------------------------------------------------- */

static char *read_all(const char *path, size_t *len)
{
    FILE *f = NULL;
    long n;
    char *p;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char *)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[n] = 0; if (len) *len = (size_t)n; }
    return p;
}

typedef struct visit_rec { int calls, named; char hash[65]; char id[65]; unsigned long long size; } visit_rec;
static void on_visit(void *ctx, const char *id, const char *hash, unsigned long long size, const char *name)
{
    visit_rec *v = (visit_rec *)ctx;
    v->calls++;
    if (name) v->named++;
    strcpy_s(v->hash, sizeof v->hash, hash);
    strcpy_s(v->id, sizeof v->id, id);
    v->size = size;
}

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alexrimp/imp_copy\","
    "\"name\":\"\",\"state\":{}}},"
    "{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/visblockers/industrial/cap_01\","
    "\"name\":\"\",\"state\":{}}}],"
    "\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\"}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}}";

/* ---- 1: a fresh mod, built, served, saved, loaded ------------------------------------------- */

static void fresh_mod(const char *fixture, const char *root)
{
    char mods[MAX_PATH], folder[MAX_PATH], cause[SH_MB_CAUSE_MAX];
    sh_mb_bundle b;
    sh_modserve_hit hit;
    sh_modserve_plan plan;
    visit_rec v;
    const char *names[2] = { "mods/alexrimp/imp_copy", "snapmaps/visblockers/industrial/cap_01" };
    char *saved, *loaded;
    size_t saved_len = 0, loaded_len = 0;
    sh_modmanifest_list rows = {0};
    int i, sedefs = 0;

    printf("-- a built mod is admitted, saved as one row, and activated on load\n");
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\alex.r2-imp", mods);
    CreateDirectoryA(mods, NULL);
    CHECK(mt_copy_tree(fixture, folder));
    CHECK(modbundle_build_folder(folder, cause, sizeof cause) == 1);
    _snprintf_s(cause, sizeof cause, _TRUNCATE, "%s\\mod.json", folder);
    CHECK(sh_mb_load(cause, &b) == 1);

    /* The serving index admits what the builder accepted, including raw
     * files under the engine's '_' spelling of the id. */
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sh_modserve_bundle_state(b.hash, NULL, 0) == 1);
    for (i = 0; i < (int)b.file_count; i++) {
        CHECK(sh_modserve_lookup(b.files[i].request, &hit) == 1);
        if (strstr(b.files[i].request, "alexrimp")) CHECK(hit.size == b.files[i].size);
    }

    /* Editor rows: both sedefs, after the decls they need. */
    CHECK(sh_modserve_plan_editor(&plan) > 0);
    for (i = 0; i < plan.count; i++) if (plan.steps[i].rank == SH_MODSERVE_RANK_SEDEF) sedefs++;
    CHECK(sedefs == 2 && plan.blocked == 0);
    if (sedefs != 2 || plan.blocked)
        for (i = 0; i < plan.count; i++)
            printf("    step %s '%s' rank %d%s%s\n", plan.steps[i].type, plan.steps[i].name, plan.steps[i].rank,
                   plan.steps[i].blocked ? " BLOCKED: " : "", plan.steps[i].blocked ? plan.steps[i].blocked : "");
    sh_modserve_plan_free(&plan);

    /* The closure in the manifest's form: one named visit, the bundle's size. */
    memset(&v, 0, sizeof v);
    CHECK(modserve_bundles_for_names_visit(names, 2, on_visit, &v) == 1);
    CHECK(v.calls == 1 && v.named == 1 && !strcmp(v.hash, b.hash) && !strcmp(v.id, "alexrimp") &&
          v.size == b.total_size);

    /* Save: one row naming the bundle, in the map's snapmap-plus member. */
    saved = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &saved_len);
    CHECK(saved != NULL);
    if (saved) {
        CHECK(!strstr(saved, "smpmod1.") && strstr(saved, "\"snapmap-plus\":{\"format\":1,\"mods\":[") != NULL);
        CHECK(sh_modmanifest_read(saved, saved_len, &rows, NULL) == 1 && rows.count == 1 &&
              !strcmp(rows.rows[0].build, b.hash) && !strcmp(rows.rows[0].salt, "alexrimp") && rows.rows[0].size == b.total_size &&
              rows.rows[0].name_count == 1 && !strcmp(rows.rows[0].names[0], "mods/alexrimp/imp_copy"));
        sh_modmanifest_list_free(&rows);
        /* Load: the row is read (the member itself is stripped last, by the
         * parse detour, so this step removes nothing) and exactly that bundle's gameplay decls
         * register. No palette has been built yet (the first editor entry's
         * build registers the rows), so no rebuild is asked for. */
        g_activations = 0;
        g_nregistered = 0;
        g_pal_built = 0;
        g_rebuilds = 0;
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded == NULL);
        CHECK(g_activations == 1 && g_last_steps > 0 && g_last_ready == g_last_steps);
        CHECK(registered("mods/alexrimp/imp_copy") && registered("mods/alexrimp/imp_copy.md6") &&
              registered("mods/alexrimp/imp_copy_b.md6"));
        CHECK(!registered_as("snapEditorEntityDef", "mods/alexrimp/imp_copy") &&
              !registered_as("snapEditorEntityDef", "mods/alexrimp/imp_copy_b"));
        CHECK(g_last_sync_r == 0 && g_last_sync.rows == 2 && g_last_sync.missing == 0 && g_rebuilds == 0);
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);

        /* The palette exists and lacks both rows (the bundle arrived after
         * the build), but the editor holds an entity: the rebuild is refused,
         * reported, and the rows stay unvisited, so each later load asks
         * again, once. */
        printf("-- a refused rebuild is asked once per load\n");
        g_pal_built = 1;
        g_pal_count = 0;
        g_rebuild_result = -1;
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded == NULL && g_last_sync_r == 1 && g_last_sync.rebuild == -1 && g_last_sync.missing == 2 &&
              g_last_sync.registered == 0 && g_rebuilds == 1);
        CHECK(!registered_as("snapEditorEntityDef", "mods/alexrimp/imp_copy"));
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded == NULL && g_last_sync_r == 1 && g_rebuilds == 2);
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        g_rebuild_result = 1;

        /* Released: the same load registers them through one rebuild. */
        printf("-- a map naming a bundle whose editor rows the built palette lacks registers them by one rebuild\n");
        g_activations = 0;
        g_rebuilds = 0;
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded == NULL && g_activations == 1);
        CHECK(g_last_sync_r == 1 && g_rebuilds == 1 && g_last_sync.rebuild == 1);
        CHECK(g_last_sync.rows == 2 && g_last_sync.missing == 2 && g_last_sync.registered == 2 &&
              g_last_sync.admitted == 2 && g_last_sync.rows_after == g_last_sync.rows_before + 2);
        CHECK(registered_as("snapEditorEntityDef", "mods/alexrimp/imp_copy") &&
              registered_as("snapEditorEntityDef", "mods/alexrimp/imp_copy_b"));
        CHECK(pal_has(NULL, "mods/alexrimp/imp_copy") && pal_has(NULL, "mods/alexrimp/imp_copy_b"));
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);

        /* The palette already holds them: nothing is requested. */
        printf("-- the same map again: the palette holds its rows, nothing is requested\n");
        g_rebuilds = 0;
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded == NULL && g_last_sync_r == 0 && g_last_sync.missing == 0 && g_rebuilds == 0);
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);

        HeapFree(GetProcessHeap(), 0, saved);
    }
    /* A map without rows costs nothing: no activation, no palette check. */
    g_activations = 0;
    g_rebuilds = 0;
    g_last_sync_r = -7;
    loaded = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &loaded_len);
    CHECK(loaded == NULL && g_activations == 0 && g_rebuilds == 0 && g_last_sync_r == -7);
    /* The check itself asks nothing for an empty list. */
    {
        sh_modserve_rows_sync res;
        CHECK(sh_modserve_map_rows_sync(NULL, 0, &PAL_OPS, &res) == 0 && res.rows == 0 && g_rebuilds == 0);
    }
    g_pal_built = 0;
    sh_mb_free(&b);
}

static void skins_namespace(const char *root)
{
    char folder[MAX_PATH], mods[MAX_PATH];
    sh_mb_bundle b;
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    printf("-- a skins decl passes by its mesh path's namespace, the salt\n");
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\skinmods", root);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\alex.skin-test", mods);
    CreateDirectoryA(mods, NULL);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", "{\"name\":\"Skin test\",\"snapmap-plus\":{\"format\":2,\"salt\":\"alexskin\"}}");
    mt_write(folder, "assets\\generated\\decls\\skins\\md6\\mods\\alexskin\\imp.md6mesh.decl", "{\r\n}");
    CHECK(sh_mb_build(folder, NULL, &r, &b) == 1);
    if (b.errors) printf("    %s\n", b.first_cause);
    {
        char cause[SH_MB_CAUSE_MAX];
        CHECK(sh_mb_write(&b, folder, cause, sizeof cause) == 1);
    }
    sh_mb_free(&b);
    CHECK(sh_modserve_index_build(mods) == 1);
    /* A skins decl outside the namespace is still refused. */
    mt_write(folder, "assets\\generated\\decls\\skins\\md6\\other\\imp.md6mesh.decl", "{\r\n}");
    CHECK(sh_mb_build(folder, NULL, &r, &b) == 0 && strstr(b.first_cause, "skins decl"));
    sh_mb_free(&b);
}

/* ---- 5: a cut-content bundle's requirements ----------------------------------------------------- */

static const char CUT_DESC[] =
    "{\"name\":\"Cut\","
    "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":\"0\",\"g_useImageBlackList\":\"0\"}},"
    "\"snapmap-plus\":{\"format\":2,\"salt\":\"localcut\"}}";

static void cut_content(const char *root)
{
    char mods[MAX_PATH], folder[MAX_PATH], cause[SH_MB_CAUSE_MAX];
    sh_mb_bundle b;
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    const char *hashes[1];
    printf("-- a bundle requiring both blacklist gates: collected at index time, applied before its registration\n");
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\cutmods", root);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\cut", mods);
    mt_write(folder, "assets\\generated\\decls\\entitydef\\mods\\localcut\\cut.decl", "{\r\n\tinherit = \"monster/imp\";\r\n}");
    mt_write(folder, "mod.json", CUT_DESC);
    CHECK(sh_mb_build(folder, NULL, &r, &b) == 1 && b.requirements != NULL);
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause) == 1);
    g_log[0] = 0;
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(strstr(g_log, "MODS: requirements: g_useImageBlackList=0 for Cut") &&
          strstr(g_log, "MODS: requirements: g_useResourceBlackList=0 for Cut"));
    CHECK(sh_modserve_requirements() == 2);
    g_gate[0] = g_gate[1] = 1;
    g_gate_sets = 0;
    g_registered_at_first_set = -1;
    g_nregistered = 0;
    hashes[0] = b.hash;
    CHECK(modserve_activate_bundles(hashes, 1) == 1);
    /* Both gates were set before the first DeclRegisterFile, and are 0. */
    CHECK(g_gate_sets == 2 && g_registered_at_first_set == 0 && g_gate[0] == 0 && g_gate[1] == 0);
    CHECK(registered("mods/localcut/cut"));
    CHECK(strstr(g_log, "MODS: map: requirements applied: g_useImageBlackList=0, g_useResourceBlackList=0 "
                        "(set directly, live immediately)") != NULL);
    sh_mb_free(&b);
}

int main(int argc, char **argv)
{
    char root[MAX_PATH];
    if (argc < 2) { fprintf(stderr, "usage: mod_pipeline_test <fixture folder>\n"); return 2; }
    if (!mt_temp_dir("mod_pipeline", root, sizeof root)) { fprintf(stderr, "no temp folder\n"); return 2; }
    fresh_mod(argv[1], root);
    skins_namespace(root);
    /* No bundle so far needs a requirement: the gates were never read or set. */
    CHECK(g_gate_reads == 0 && g_gate_sets == 0);
    cut_content(root);
    mt_remove_tree(root);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
