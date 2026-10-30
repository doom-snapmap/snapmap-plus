/* mod_pipeline_test -- the three mod layers linked together, the way the
 * backend links them: a folder built by the bundle builder (modbundle.c) is
 * admitted by the serving index (modserve.c); a save writes one smpmod1 row
 * through the serving layer's closure (modmanifest.c); the next load strips
 * the row and activates exactly that bundle's gameplay decls; a legacy-form
 * bundle (a "legacy" block, non-stock shadows) builds, is admitted, and its
 * new names are registered while its stock shadows are left to the provider;
 * and, when SH_MODLEGACY_FIXTURES names the folder holding the campaign's
 * carrier maps, each real conversion (modlegacy.c) builds with the real
 * builder, rescans the index and is activated in the same parse.
 *
 * Engine stand-ins: modserve_activate_bundles runs the real plan against
 * fake registry operations, and modbundle_build_folder is the real builder
 * with a fixed stock list (the product's own entry in modcmd.c reads the
 * game's archives instead). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modlegacy.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static int g_echo;
void backend_log(const char *line)
{
    if (g_echo && line) printf("    %s\n", line);
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

static int g_builds;
int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = 0;
    g_builds++;
    if (!sh_mb_build(folder, &r, &b)) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    else if (!sh_mb_write(&b, folder, cause, sizeof cause)) strncpy_s(first_cause, n, cause, _TRUNCATE);
    else ok = 1;
    if (!ok) printf("    builder refused %s: %s\n", folder, first_cause);
    sh_mb_free(&b);
    return ok;
}

/* ---- registry stand-in ---------------------------------------------------------------------- */

#define MAX_REC 1024
static char g_registered[MAX_REC][160];
static int g_nregistered, g_loads, g_activations;
static int g_last_steps, g_last_ready, g_last_stock;

static int fake_known(const char *name)
{
    int i;
    /* The engine's own decls: anything without a mods/ or custom/ prefix. */
    if (strncmp(name, "mods/", 5) && strncmp(name, "custom/", 7) && strncmp(name, "local/", 6)) return 1;
    for (i = 0; i < g_nregistered; i++) if (!_stricmp(g_registered[i], name)) return 1;
    return 0;
}
static int op_source(void *c, const char *type, const char *name) { (void)c; (void)type; return fake_known(name); }
static int op_register(void *c, const char *type, const char *name, const char *reg)
{
    (void)c; (void)type; (void)reg;
    if (g_nregistered < MAX_REC) strncpy_s(g_registered[g_nregistered++], 160, name, _TRUNCATE);
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

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    sh_modserve_plan plan;
    int unknown = 0, ready = 0, i;
    if (n <= 0 || !bundle_hashes) return 0;
    g_activations++;
    if (sh_modserve_plan_bundles(bundle_hashes, n, &plan, &unknown) < 0) return -1;
    if (plan.count) ready = sh_modserve_plan_run(&plan, &OPS, "map");
    g_last_steps = plan.count;
    g_last_ready = ready;
    g_last_stock = 0;
    for (i = 0; i < plan.count; i++) if (plan.steps[i].shadow && plan.steps[i].ready == 1) g_last_stock++;
    sh_modserve_plan_free(&plan);
    return ready;
}

static int registered(const char *name)
{
    int i;
    for (i = 0; i < g_nregistered; i++) if (!_stricmp(g_registered[i], name)) return 1;
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
static void on_visit(void *ctx, const char *id, const char *version, const char *hash, unsigned long long size,
                     const char *name)
{
    visit_rec *v = (visit_rec *)ctx;
    (void)version;
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
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alex.r2-imp/imp_copy\","
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
    const char *names[2] = { "mods/alex.r2-imp/imp_copy", "snapmaps/visblockers/industrial/cap_01" };
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
    _snprintf_s(cause, sizeof cause, _TRUNCATE, "%s\\bundle.json", folder);
    CHECK(sh_mb_load(cause, &b) == 1);

    /* The serving index admits what the builder accepted, including raw
     * files under the engine's '_' spelling of the id. */
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sh_modserve_bundle_state(b.hash, NULL, 0) == 1);
    for (i = 0; i < (int)b.file_count; i++) {
        CHECK(sh_modserve_lookup(b.files[i].request, &hit) == 1);
        if (strstr(b.files[i].request, "alex.r2_imp")) CHECK(hit.size == b.files[i].size);
    }

    /* Editor rows: both sedefs, after the decls they need. */
    CHECK(sh_modserve_plan_editor(&plan) > 0);
    for (i = 0; i < plan.count; i++) if (plan.steps[i].rank == 3) sedefs++;
    CHECK(sedefs == 2 && plan.blocked == 0);
    if (sedefs != 2 || plan.blocked)
        for (i = 0; i < plan.count; i++)
            printf("    step %s '%s' rank %d%s%s\n", plan.steps[i].type, plan.steps[i].name, plan.steps[i].rank,
                   plan.steps[i].blocked ? " BLOCKED: " : "", plan.steps[i].blocked ? plan.steps[i].blocked : "");
    sh_modserve_plan_free(&plan);

    /* The closure in the manifest's form: one named visit, the bundle's size. */
    memset(&v, 0, sizeof v);
    CHECK(modserve_bundles_for_names_visit(names, 2, on_visit, &v) == 1);
    CHECK(v.calls == 1 && v.named == 1 && !strcmp(v.hash, b.hash) && !strcmp(v.id, "alex.r2-imp") &&
          v.size == b.total_size);

    /* Save: one smpmod1 row naming the bundle. */
    saved = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &saved_len);
    CHECK(saved != NULL);
    if (saved) {
        CHECK(sh_modmanifest_read(saved, saved_len, &rows, NULL) == 1 && rows.count == 1 &&
              !strcmp(rows.rows[0].bundle, b.hash) && rows.rows[0].size == b.total_size &&
              rows.rows[0].name_count == 1 && !strcmp(rows.rows[0].names[0], "mods/alex.r2-imp/imp_copy"));
        sh_modmanifest_list_free(&rows);
        /* Load: the row is stripped and exactly that bundle's gameplay decls register. */
        g_activations = 0;
        g_nregistered = 0;
        loaded = sh_modmanifest_prepare_load(saved, saved_len, &loaded_len);
        CHECK(loaded != NULL && !strstr(loaded, "smpmod1."));
        CHECK(g_activations == 1 && g_last_steps > 0 && g_last_ready == g_last_steps);
        CHECK(registered("mods/alex.r2-imp/imp_copy") && registered("mods/alex.r2-imp/imp_copy.md6") &&
              registered("mods/alex.r2-imp/imp_copy_b.md6"));
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    /* A map without rows costs nothing. */
    g_activations = 0;
    loaded = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &loaded_len);
    CHECK(loaded == NULL && g_activations == 0);
    sh_mb_free(&b);
}

/* ---- 2: a legacy-form bundle ---------------------------------------------------------------- */

static const char LEGACY_DESC[] =
    "{\"id\":\"local.thing\",\"version\":\"legacy-0123456789abcdef\",\"name\":\"Thing\","
    "\"shadows\":[\"generated/decls/entitydef/custom/thing.decl\",\"generated/decls/entitydef/monster/imp.decl\"],"
    "\"legacy\":{\"digest16\":\"0123456789abcdef\",\"key\":\"local.thing\",\"keyForm\":\"descriptor\",\"layout\":\"C\"}}";
static const char NO_LEGACY_DESC[] =
    "{\"id\":\"local.thing\",\"version\":\"legacy-0123456789abcdef\",\"name\":\"Thing\","
    "\"shadows\":[\"generated/decls/entitydef/custom/thing.decl\",\"generated/decls/entitydef/monster/imp.decl\"]}";

static void legacy_form(const char *root)
{
    char legacy[MAX_PATH], folder[MAX_PATH], cause[SH_MB_CAUSE_MAX], mods[MAX_PATH];
    sh_mb_bundle b;
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_modserve_plan plan;
    const char *hashes[1];
    char *desc_path;
    int unknown = 0, ready, i, custom_loaded = 0;

    printf("-- a legacy bundle: non-stock shadows build, new names register, stock shadows stay served\n");
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", mods);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\local.thing@0123456789abcdef", legacy);
    CreateDirectoryA(legacy, NULL);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "assets\\generated\\decls\\entitydef\\custom\\thing.decl",
             "{\r\n\tinherit = \"monster/imp\";\r\n\tedit = {\r\n\t\trenderModelInfo = {\r\n\t\t\tmodel = \"custom/missing.md6\";\r\n\t\t}\r\n\t}\r\n}");
    mt_write(folder, "assets\\generated\\decls\\entitydef\\monster\\imp.decl", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n}");

    /* Without the legacy block the same descriptor is refused. */
    mt_write(folder, "mod.json", NO_LEGACY_DESC);
    CHECK(sh_mb_build(folder, &r, &b) == 0 && strstr(b.first_cause, "not a stock resource"));
    sh_mb_free(&b);
    /* With it: built, the unresolved heuristic need only warns. */
    mt_write(folder, "mod.json", LEGACY_DESC);
    CHECK(sh_mb_build(folder, &r, &b) == 1 && b.legacy == 1 && b.errors == 0 && b.warnings >= 1);
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause) == 1);
    hashes[0] = b.hash;
    desc_path = cause;
    _snprintf_s(desc_path, sizeof cause, _TRUNCATE, "%s\\bundle.json", folder);
    {
        sh_mb_bundle back;
        CHECK(sh_mb_load(desc_path, &back) == 1 && back.legacy == 1);
        sh_mb_free(&back);
    }

    /* Admitted; both files are shadows; the gameplay plan holds both, and
     * only the new name is registered and loaded. */
    CHECK(sh_modserve_index_build(mods) >= 1);
    CHECK(sh_modserve_bundle_state(b.hash, NULL, 0) == 1);
    {
        sh_modserve_hit hit;
        CHECK(sh_modserve_lookup("generated/decls/entitydef/monster/imp.decl", &hit) == 1 && hit.is_shadow);
        CHECK(sh_modserve_lookup("generated/decls/entitydef/custom/thing.decl", &hit) == 1 && hit.is_shadow);
    }
    CHECK(sh_modserve_plan_bundles(hashes, 1, &plan, &unknown) == 2 && unknown == 0 && plan.blocked == 0);
    g_nregistered = 0;
    g_loads = 0;
    ready = sh_modserve_plan_run(&plan, &OPS, "map");
    CHECK(ready == 2);
    for (i = 0; i < plan.count; i++)
        if (!strcmp(plan.steps[i].name, "custom/thing")) custom_loaded = plan.steps[i].ready == 1;
    CHECK(custom_loaded && registered("custom/thing") && !registered("monster/imp") && g_loads == 1);
    sh_modserve_plan_free(&plan);
    sh_mb_free(&b);
}

/* ---- 3: skins and the '_' spelling of the id ------------------------------------------------- */

static void skins_namespace(const char *root)
{
    char folder[MAX_PATH], mods[MAX_PATH];
    sh_mb_bundle b;
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    printf("-- a skins decl passes by its mesh path's namespace, in either spelling of the id\n");
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\skinmods", root);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\alex.skin-test", mods);
    CreateDirectoryA(mods, NULL);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", "{\"id\":\"alex.skin-test\",\"name\":\"Skin test\"}");
    mt_write(folder, "assets\\generated\\decls\\skins\\md6\\mods\\alex.skin_test\\imp.md6mesh.decl", "{\r\n}");
    CHECK(sh_mb_build(folder, &r, &b) == 1);
    if (b.errors) printf("    %s\n", b.first_cause);
    {
        char cause[SH_MB_CAUSE_MAX];
        CHECK(sh_mb_write(&b, folder, cause, sizeof cause) == 1);
    }
    sh_mb_free(&b);
    CHECK(sh_modserve_index_build(mods) == 1);
    /* A skins decl outside the namespace is still refused. */
    mt_write(folder, "assets\\generated\\decls\\skins\\md6\\other\\imp.md6mesh.decl", "{\r\n}");
    CHECK(sh_mb_build(folder, &r, &b) == 0 && strstr(b.first_cause, "skins decl"));
    sh_mb_free(&b);
}

/* ---- 4: the campaign's carrier maps -------------------------------------------------------- */

static void carrier_conversion(const char *root)
{
    static const struct { const char *file; unsigned carriers; } maps[] = {
        { "carrier-1-shard_map.decl", 1 },
        { "carrier-21-shards_map.decl", 1 },
        { "carrier-1054-shards_map.decl", 2 },
    };
    char dir[MAX_PATH], path[MAX_PATH * 2], mods[MAX_PATH];
    size_t i, k;
    DWORD got = GetEnvironmentVariableA("SH_MODLEGACY_FIXTURES", dir, sizeof dir);
    printf("-- the carrier maps convert with the real builder and are active in the same parse\n");
    if (!got || got >= sizeof dir) {
        printf("  [skip] SH_MODLEGACY_FIXTURES does not name the carrier maps' folder\n");
        return;
    }
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    CHECK(sh_modserve_index_build(mods) >= 0);   /* the index the boot would have built */
    sh_modlegacy_set_data_root(root);
    sh_modlegacy_reset();
    for (i = 0; i < sizeof maps / sizeof maps[0]; i++) {
        size_t n = 0, on = 0, ln = 0, verdicts;
        char *text, *out, *loaded;
        sh_modlegacy_verdict v[4];
        unsigned converted = 0, indexed = 0;
        ULONGLONG t0;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, maps[i].file);
        text = read_all(path, &n);
        if (!text) { printf("  [skip] %s is not present\n", maps[i].file); continue; }
        g_builds = 0;
        g_echo = getenv("MOD_PIPELINE_ECHO") != NULL;
        t0 = GetTickCount64();
        out = sh_modlegacy_process(text, n, &on);
        verdicts = sh_modlegacy_last_report(v, 4);
        for (k = 0; k < verdicts && k < 4; k++) {
            if (v[k].verdict == SH_MODLEGACY_CONVERT) converted++;
            else printf("    %s: CLEAN -- %s\n", maps[i].file, v[k].reason);
            /* The rescan the conversion asked for made each bundle visible. */
            if (v[k].verdict == SH_MODLEGACY_CONVERT && sh_modserve_bundle_state(v[k].bundle, NULL, 0) == 1) indexed++;
        }
        CHECK(out != NULL && verdicts == maps[i].carriers && converted == maps[i].carriers && indexed == converted);
        /* The manifest step of the same parse activates what was converted. */
        if (out) {
            g_activations = 0;
            loaded = sh_modmanifest_prepare_load(out, on, &ln);
            CHECK(loaded != NULL && g_activations == 1 && g_last_steps > 0 && g_last_ready == g_last_steps);
            printf("  %s: %u converted (%d build(s)), %llu ms; activation %d step(s), %d ready, %d stock shadow(s)\n",
                   maps[i].file, converted, g_builds, (unsigned long long)(GetTickCount64() - t0), g_last_steps,
                   g_last_ready, g_last_stock);
            if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
            HeapFree(GetProcessHeap(), 0, out);
        }
        free(text);
    }
    sh_modlegacy_set_data_root(NULL);
}

int main(int argc, char **argv)
{
    char root[MAX_PATH];
    if (argc < 2) { fprintf(stderr, "usage: mod_pipeline_test <fixture folder>\n"); return 2; }
    if (!mt_temp_dir("mod_pipeline", root, sizeof root)) { fprintf(stderr, "no temp folder\n"); return 2; }
    fresh_mod(argv[1], root);
    legacy_form(root);
    skins_namespace(root);
    carrier_conversion(root);
    mt_remove_tree(root);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
