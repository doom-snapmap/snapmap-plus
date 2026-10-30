/* modmodule_test -- a multiplayer map served as a SnapMap module from a bundle
 * (lane P8i), over a synthetic install.
 *
 * The install holds what a multiplayer map ships (render model, collision,
 * navigation, sound BSP, light grid, occlusion tome, a light probe, a material
 * only gameresources has and an own-path mega material) plus a loose .pages
 * virtual texture at the install root. Covered:
 *   names: snapModule typing of maps/modules/<path>.decl (and not of a
 *     palette render model), the snapModuleInfo request a module needs, the
 *     pointer-only kinds, the stock restore rule;
 *   the builder: a module bundle of two authored decls and stock pointers to
 *     every derived name builds with 0 errors, pointers outside the size caps,
 *     restored stock decls outside the namespace without shadows, the loose
 *     .pages pointer hashed, the manifest loads back with the same hash;
 *   refusals: a carried .sbsp/.ambientsh/.tome/.pages that is not installed
 *     content (with and without the stock reader), a module without its
 *     snapModuleInfo, a module outside maps/modules/mods/<id>/, a malformed
 *     module decl, an out-of-namespace pointer that is not a restore;
 *   serving: the module is served as a snapModule, a map's moduleName finds
 *     its bundle, pointers are answered with installed bytes, and the restored
 *     material decls are planned like shadows (left to the provider when the
 *     engine knows them).
 *
 * With SH_MODMODULE_SOURCE naming a source mod folder (the acceptance bundle
 * work/laneP8/mods/alex.mp-basin) and SH_MODMODULE_DOOM the install (default
 * D:\SteamLibrary\steamapps\common\DOOM), the folder is built against the real
 * installed game, bundle.json is written into it, and the counts are printed.
 * SH_MODMODULE_PREFAB may name a module prefab (the acceptance placement file);
 * it is read with the Prefabs tab's own parser and must name the bundle's module.
 */
#include "../src/backend/modbundle.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modstock.h"
#include "../src/backend/module_prefab_map.h"
#include "mod_test_util.h"
#include "mod_stock_fixture.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)
#define CHECK_CAUSE(b, needle) do {                                                    \
    g_checks++;                                                                        \
    if (!strstr((b).first_cause, (needle))) {                                          \
        fprintf(stderr, "%s:%d: first cause '%s' lacks '%s'\n", __FILE__, __LINE__,    \
                (b).first_cause, (needle));                                            \
        g_failed++;                                                                    \
    }                                                                                  \
} while (0)

static char g_log[256 * 1024];
static size_t g_log_n;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

/* ------------------------------------------------------------ fixtures ---- */

#define MP        "maps/game/mp/t_arena/t_arena"
#define S_MODEL   MP "/_combo/_world.bmodel"
#define S_CM      MP "/_combo/world.bcm"
#define S_AAS     "generated/" MP "/t_arena.baas_monster48"
#define S_SBSP    MP "/t_arena.sbsp"
#define S_AMB     MP "/t_arena.ambientsh"
#define S_TOME    MP "/_combo/_world.tome"
#define S_PROBE   MP "/lightprobes/light_probe_1_compressed.bimage"
#define S_MAT     "generated/decls/material/textures/t_arena/wall.decl"
#define S_MEGA    MP "/mega.decl"
#define S_PAGES   "virtualtextures/maps/game/mp/t_arena/t_arena.pages"

#define ID        "t.arena"
#define MOD       "maps/modules/mods/" ID "/arena"
#define R_DECL    MOD ".decl"
#define R_INFO    "generated/decls/snapmoduleinfo/mods/" ID "/arena.decl"
#define R_MODEL   "maps/modules/palettes/mega_blessed/mods/" ID "/arena.bmodel"
#define R_CM      MOD "/_combo/world.bcm"
#define R_AAS     "generated/" MOD "/arena.baas_monster48"
#define R_SBSP    MOD "/arena.sbsp"
#define R_AMB     MOD "/arena.ambientsh"
#define R_TOME    MOD "/_combo/_world.tome"
#define R_PROBE   MOD "/lightprobes/light_probe_1_compressed.bimage"
#define R_MEGA    "generated/decls/material/" S_MEGA ".decl"

static const char MAT_TEXT[] = "{\r\n\tdiffusemap \"textures/t_arena/wall\"\r\n}\r\n";
static const char MEGA_TEXT[] = "{\r\nlandPageFile \"maps/game/mp/t_arena/t_arena\"\r\n}\r\n";
static const char MODULE_TEXT[] =
    "{\r\n\tedit = {\r\nmodule = {\r\n\tname = \"modules/mods/" ID "/arena.map\";\r\n"
    "\tportals = {\r\n\t\tnum = 1;\r\n\t}\r\n\tentities = {\r\n\t\tnum = 1;\r\n\t\titem[0] = {\r\n"
    "\t\t\tname = \"light_probe_1\";\r\n\t\t\ttext = \"{\\r\\n\\t\\tinherit = \\\"light/probe\\\";\\r\\n\\t}\";\r\n"
    "\t\t}\r\n\t}\r\n}\r\n\t}\r\n}\r\n";
static const char INFO_TEXT[] = "{\r\n\tedit = {\r\n\t\tmoduleType = \"MODULE_TYPE_C\";\r\n\t}\r\n}\r\n";
static unsigned char MODEL[400], CM[300], AAS[200], SBSP[120], AMB[160], TOME[90], PROBE[250], PAGES[180], JUNK[64];

static void make_install(const char *d)
{
    size_t i;
    for (i = 0; i < sizeof MODEL; i++) MODEL[i] = (unsigned char)(i * 7 + 1);
    for (i = 0; i < sizeof CM; i++) CM[i] = (unsigned char)(i * 5 + 2);
    for (i = 0; i < sizeof AAS; i++) AAS[i] = (unsigned char)(i * 3 + 3);
    for (i = 0; i < sizeof SBSP; i++) SBSP[i] = (unsigned char)(i ^ 0x33);
    for (i = 0; i < sizeof AMB; i++) AMB[i] = (unsigned char)(i ^ 0x44);
    for (i = 0; i < sizeof TOME; i++) TOME[i] = (unsigned char)(255 - i);
    for (i = 0; i < sizeof PROBE; i++) PROBE[i] = (unsigned char)(i * 11 + 4);
    for (i = 0; i < sizeof PAGES; i++) PAGES[i] = (unsigned char)(i ^ 0xa5);
    for (i = 0; i < sizeof JUNK; i++) JUNK[i] = (unsigned char)(i * 13 + 99);
    {
        entry g[] = {
            { S_MODEL, MODEL, sizeof MODEL, 0, 1 },
            { S_CM,    CM, sizeof CM, 1, 0 },
            { S_AAS,   AAS, sizeof AAS, 1, 1 },
            { S_SBSP,  SBSP, sizeof SBSP, 0, 0 },
            { S_AMB,   AMB, sizeof AMB, 0, 0 },
            { S_TOME,  TOME, sizeof TOME, 1, 0 },
            { S_PROBE, PROBE, sizeof PROBE, 0, 0 },
            { S_MAT,   (const unsigned char *)MAT_TEXT, sizeof MAT_TEXT - 1, 0, 0 },
            { S_MEGA,  (const unsigned char *)MEGA_TEXT, sizeof MEGA_TEXT - 1, 1, 0 },
        };
        entry s[] = { { "generated/decls/material/textures/snap/only.decl", (const unsigned char *)MAT_TEXT, sizeof MAT_TEXT - 1, 0, 0 } };
        write_container(d, "gameresources", g, (int)(sizeof g / sizeof g[0]));
        write_container(d, "snap_gameresources", s, 1);
    }
    put_rel(d, S_PAGES, PAGES, sizeof PAGES);   /* loose, at the install root like a real .pages */
}

static int stock_exists(void *ctx, const char *request) { (void)ctx; return sh_modstock_exists(request); }
static const sh_mb_resolver RESOLVER = { NULL, NULL, stock_exists, NULL };

static const sh_mb_file *find(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

/* mod.json with the derived-name pointers, plus `extra` stock entries and top-level keys. */
static void make_module_mod(const char *dir, const char *id, const char *decl_req, int with_info,
                            const char *module_text, const char *extra_stock)
{
    char desc[8192];
    _snprintf_s(desc, sizeof desc, _TRUNCATE,
        "{\"id\":\"%s\",\"name\":\"Arena\",\"version\":\"1.0.0\",\"stock\":{"
        "\"" R_MODEL "\":{\"source\":\"" S_MODEL "\"},"
        "\"" R_CM "\":{\"source\":\"" S_CM "\"},"
        "\"" R_AAS "\":{\"source\":\"" S_AAS "\"},"
        "\"" R_SBSP "\":{\"source\":\"" S_SBSP "\"},"
        "\"" R_AMB "\":{\"source\":\"" S_AMB "\"},"
        "\"" R_TOME "\":{\"source\":\"" S_TOME "\"},"
        "\"" R_PROBE "\":{\"source\":\"" S_PROBE "\"},"
        "\"" S_MAT "\":{\"source\":\"" S_MAT "\"},"
        "\"" R_MEGA "\":{\"source\":\"" S_MEGA "\"},"
        "\"" S_PAGES "\":{\"source\":\"" S_PAGES "\"}%s}}",
        id, extra_stock ? extra_stock : "");
    mt_write(dir, "mod.json", desc);
    {
        char rel[512];
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\%s", decl_req);
        put_rel(dir, rel, module_text, strlen(module_text));
    }
    if (with_info) put_rel(dir, "assets\\" R_INFO, INFO_TEXT, sizeof INFO_TEXT - 1);
}

/* --------------------------------------------------------------- names ---- */

static void test_names(void)
{
    char type[64], name[512], info[512];
    CHECK(sh_mb_logical_name(R_DECL, type, sizeof type, name, sizeof name) && !strcmp(type, "snapModule") && !strcmp(name, R_DECL));
    CHECK(sh_mb_logical_name("maps/modules/classic/classic_90.decl", type, sizeof type, name, sizeof name) &&
          !strcmp(type, "snapModule"));
    /* The render model and the palette's own mega material are not modules. */
    CHECK(sh_mb_logical_name(R_MODEL, type, sizeof type, name, sizeof name) && !strcmp(type, "model"));
    CHECK(sh_mb_logical_name("maps/modules/palettes/mega_blessed/mega.decl", type, sizeof type, name, sizeof name) &&
          !strcmp(type, "file"));
    CHECK(sh_mb_logical_name(R_INFO, type, sizeof type, name, sizeof name) && !strcmp(type, "snapModuleInfo") &&
          !strcmp(name, "mods/" ID "/arena"));
    CHECK(sh_mb_logical_name(R_MEGA, type, sizeof type, name, sizeof name) && !strcmp(type, "material") && !strcmp(name, S_MEGA));
    CHECK(sh_mb_module_info_request(R_DECL, info, sizeof info) && !strcmp(info, R_INFO));
    CHECK(sh_mb_module_info_request("Maps/Modules/Mods/" ID "/Arena.decl", info, sizeof info) && !strcmp(info, R_INFO));
    CHECK(!sh_mb_module_info_request(R_MODEL, info, sizeof info));
    CHECK(!sh_mb_module_info_request("maps/modules/palettes/mega_blessed/mega.decl", info, sizeof info));
    CHECK(!sh_mb_module_info_request("maps/game/mp/x.decl", info, sizeof info));

    CHECK(sh_mb_pointer_only_kind(R_SBSP) && sh_mb_pointer_only_kind(R_AMB) && sh_mb_pointer_only_kind(R_TOME) &&
          sh_mb_pointer_only_kind(S_PAGES) && sh_mb_pointer_only_kind("X/Y.SBSP"));
    CHECK(!sh_mb_pointer_only_kind(R_PROBE) && !sh_mb_pointer_only_kind(R_CM) && !sh_mb_pointer_only_kind("a.sbsp/b.decl"));
    CHECK(!sh_mb_extension_allowed(R_SBSP) && !sh_mb_extension_allowed(S_PAGES));

    CHECK(sh_mb_stock_restore(S_MAT, S_MAT));
    CHECK(sh_mb_stock_restore(R_MEGA, S_MEGA));
    CHECK(sh_mb_stock_restore(S_PAGES, S_PAGES));
    CHECK(!sh_mb_stock_restore(R_CM, S_CM));                                   /* an alias, not a restore */
    CHECK(!sh_mb_stock_restore("generated/decls/material/" S_MEGA, S_MEGA));   /* missing the ".decl" the engine adds */
    CHECK(!sh_mb_stock_restore("generated/decls/notatype/" S_MEGA ".decl", S_MEGA));
    CHECK(!sh_mb_stock_restore("generated/decls/material/" S_MAT ".decl", S_MAT));   /* a generated source is not own-path */
    CHECK(!sh_mb_stock_restore(R_MEGA, NULL) && !sh_mb_stock_restore("", ""));
}

/* ------------------------------------------------------------- builder ---- */

static void test_builder(const char *tmp, const char *install)
{
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], h[65];
    sh_mb_bundle b, back;
    const sh_mb_file *f;
    size_t i;

    sh_modstock_set_install(install);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena", tmp);
    make_module_mod(dir, ID, R_DECL, 1, MODULE_TEXT, NULL);
    CHECK(sh_mb_build(dir, &RESOLVER, &b));
    if (b.errors) fprintf(stderr, "  module build refused: %s\n", b.first_cause);
    CHECK(b.errors == 0 && b.file_count == 12 && b.stock_count == 10);
    CHECK(b.total_size == sizeof MODULE_TEXT - 1 + sizeof INFO_TEXT - 1);
    f = find(&b, R_DECL);
    CHECK(f && !f->stock && f->is_module && !f->is_decl && !strcmp(f->ptype, "snapModule") && !strcmp(f->pname, R_DECL));
    CHECK(f && f->need_count == 0);
    f = find(&b, R_INFO);
    CHECK(f && !f->stock && f->is_decl && !strcmp(f->ptype, "snapModuleInfo"));
    f = find(&b, R_SBSP);
    CHECK(f && f->stock && !strcmp(f->stock_source, S_SBSP) && f->size == sizeof SBSP);
    f = find(&b, R_TOME);
    CHECK(f && f->stock && !strcmp(f->stock_source, S_TOME));
    f = find(&b, R_PROBE);
    CHECK(f && f->stock && !strcmp(f->ptype, "file"));   /* a probe image is requested by its path */
    f = find(&b, R_MODEL);
    CHECK(f && f->stock && !strcmp(f->ptype, "model"));
    f = find(&b, S_PAGES);
    sh_mb_sha256_hex(PAGES, sizeof PAGES, h);
    CHECK(f && f->stock && !strcmp(f->blob, h) && f->size == sizeof PAGES);
    /* Restored stock decls: outside the namespace, no shadows entry, typed. */
    f = find(&b, S_MAT);
    CHECK(f && f->stock && f->is_decl && !f->shadowed && !strcmp(f->ptype, "material"));
    f = find(&b, R_MEGA);
    CHECK(f && f->stock && f->is_decl && !strcmp(f->pname, S_MEGA) && !strcmp(f->stock_source, S_MEGA));
    CHECK(b.shadow_count == 0);
    CHECK(b.canonical && strstr(b.canonical, "\"provides\":{\"name\":\"" R_DECL "\",\"type\":\"snapModule\"}"));
    {
        char cause[SH_MB_CAUSE_MAX];
        CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", dir);
    CHECK(sh_mb_load(path, &back));
    CHECK(!strcmp(back.hash, b.hash) && back.stock_count == 10 && back.file_count == 12);
    f = find(&back, R_DECL);
    CHECK(f && f->is_module);
    for (i = 0; i < back.file_count; i++) if (back.files[i].stock) CHECK(back.files[i].size == 0 || back.files[i].stock_source);
    sh_mb_free(&back);
    sh_mb_free(&b);

    /* A carried copy of an installed .sbsp is a pointer; one with other bytes is refused. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_copy", tmp);
    make_module_mod(dir, ID, R_DECL, 1, MODULE_TEXT, NULL);
    put_rel(dir, "assets\\" MOD "/copy.sbsp", SBSP, sizeof SBSP);
    CHECK(sh_mb_build(dir, &RESOLVER, &b));
    CHECK(b.errors == 0 && (f = find(&b, MOD "/copy.sbsp")) && f->stock && !strcmp(f->stock_source, S_SBSP));
    sh_mb_free(&b);
    {
        static const char *const kinds[] = { MOD "/arena2.sbsp", MOD "/arena2.ambientsh", MOD "/_combo/x.tome",
                                             "virtualtextures/mods/" ID "/arena.pages", NULL };
        int k;
        for (k = 0; kinds[k]; k++) {
            char rel[512];
            _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_carry%d", tmp, k);
            make_module_mod(dir, ID, R_DECL, 1, MODULE_TEXT, NULL);
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\%s", kinds[k]);
            put_rel(dir, rel, JUNK, sizeof JUNK);
            CHECK(!sh_mb_build(dir, &RESOLVER, &b));
            CHECK(b.errors == 1);
            CHECK_CAUSE(b, "can only be a stock pointer to the installed game, never carried");
            sh_mb_free(&b);
        }
    }
    /* Without the stock reader the carried kind is refused at the walk. */
    {
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_noreader", tmp);
        mt_write(dir, "mod.json", "{\"id\":\"" ID "\",\"name\":\"Arena\"}");
        put_rel(dir, "assets\\" MOD "/arena.sbsp", SBSP, sizeof SBSP);
        sh_mb_set_stock_ops(NULL);
        CHECK(!sh_mb_build(dir, NULL, &b));
        CHECK(b.errors >= 1);
        CHECK_CAUSE(b, "can only be a stock pointer to the installed game, never carried");
        sh_mb_free(&b);
        sh_modstock_register();
        sh_modstock_set_install(install);
    }
    /* A module without its snapModuleInfo. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_noinfo", tmp);
    make_module_mod(dir, ID, R_DECL, 0, MODULE_TEXT, NULL);
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK(b.errors == 1);
    CHECK_CAUSE(b, "a snapModule needs its snapModuleInfo assets/" R_INFO " in the same bundle");
    sh_mb_free(&b);
    /* A module outside maps/modules/mods/<id>/: this rule first, then its missing snapModuleInfo. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_ns", tmp);
    make_module_mod(dir, ID, "maps/modules/other/" ID "/arena.decl", 0, MODULE_TEXT, NULL);
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK(b.errors == 2);
    CHECK_CAUSE(b, "a snapModule decl must be maps/modules/mods/" ID "/<name>.decl");
    sh_mb_free(&b);
    /* A malformed module decl. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_bad", tmp);
    make_module_mod(dir, ID, R_DECL, 1, "{\r\n\tedit = {\r\n", NULL);
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK(b.errors == 1);
    CHECK_CAUSE(b, "is not a well-formed decl");
    sh_mb_free(&b);
    /* A pointer outside the namespace that only aliases (not a restore) needs shadows. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\arena_alias", tmp);
    make_module_mod(dir, ID, R_DECL, 1, MODULE_TEXT,
                    ",\"generated/decls/material/textures/t_arena/alias.decl\":{\"source\":\"" S_MAT "\"}");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK(b.errors == 1);
    CHECK_CAUSE(b, "is outside mods/" ID "/ and is not listed in shadows");
    sh_mb_free(&b);
}

/* ------------------------------------------------------------- serving ---- */

typedef struct visit_ctx { int calls; char id[80]; } visit_ctx;
static void visit(void *context, const char *bundle_id, const char *version, const char *bundle_hash,
                  unsigned long long size, const char *name)
{
    visit_ctx *v = (visit_ctx *)context;
    (void)version; (void)bundle_hash; (void)size;
    if (name) { v->calls++; strcpy_s(v->id, sizeof v->id, bundle_id); }
}

static void test_serving(const char *tmp, const char *install)
{
    char dir[MAX_PATH * 2], store[MAX_PATH * 2], key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX], hash[65];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    sh_modserve_hit hit;
    unsigned char *bytes = NULL;
    size_t len = 0;
    sh_modserve_plan plan;
    const char *names[1] = { "Maps/Modules/Mods/" ID "/Arena.decl" };
    const char *hashes[1];
    visit_ctx v = { 0, "" };
    int i, mat = 0, mega = 0, info = 0;

    sh_modstock_set_install(install);
    _snprintf_s(store, sizeof store, _TRUNCATE, "%s\\store", tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\src", tmp);
    make_module_mod(dir, ID, R_DECL, 1, MODULE_TEXT, NULL);
    CHECK(sh_ms_set_root(store));
    CHECK(sh_ms_install_from_folder(dir, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    if (!key[0]) fprintf(stderr, "  install refused: %s\n", cause);
    CHECK(st.stock_files == 10 && st.blobs_written == 2);
    strcpy_s(hash, sizeof hash, b.hash);
    sh_mb_free(&b);
    CHECK(sh_modserve_index_build(store) == 1);

    /* The private module palette's question (grid_room_native.c). */
    CHECK(sh_modserve_module_served(R_DECL) == 1);
    CHECK(sh_modserve_module_served("MAPS\\MODULES\\MODS\\" ID "\\ARENA.DECL") == 1);
    CHECK(sh_modserve_module_served(R_INFO) == 0);          /* the info is a decl, not a module */
    CHECK(sh_modserve_module_served(R_CM) == 0);
    CHECK(sh_modserve_module_served("maps/modules/classic/classic_90.decl") == 0);
    CHECK(sh_modserve_lookup(R_CM, NULL) == 1 && sh_modserve_lookup("decltree/snapmoduleinfo/mods/" ID "/arena.decl", NULL) == 1);

    /* Pointers are answered with the installed bytes; the module decl with its own. */
    CHECK(sh_modserve_lookup(R_SBSP, &hit) && hit.is_stock && !strcmp(hit.stock_source, S_SBSP));
    CHECK(sh_modserve_open(R_SBSP, &bytes, &len) == 1 && len == sizeof SBSP && !memcmp(bytes, SBSP, len));
    HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open(R_AAS, &bytes, &len) == 1 && len == sizeof AAS && !memcmp(bytes, AAS, len));
    HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open(S_PAGES, &bytes, &len) == 1 && len == sizeof PAGES);
    HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open(R_DECL, &bytes, &len) == 1 && len == sizeof MODULE_TEXT - 1);
    HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open("decltree/material/" S_MEGA ".decl", &bytes, &len) == 1 && len == sizeof MEGA_TEXT - 1);
    HeapFree(GetProcessHeap(), 0, bytes);

    /* A saved map's moduleName names the bundle (save closure). */
    CHECK(modserve_bundles_for_names_visit(names, 1, visit, &v) == 1 && v.calls == 1 && !strcmp(v.id, ID));

    /* The map plan: the info and the restored materials, the restores marked like shadows. */
    hashes[0] = hash;
    CHECK(sh_modserve_plan_bundles(hashes, 1, &plan, NULL) == 3);
    for (i = 0; i < plan.count; i++) {
        const sh_modserve_step *s = &plan.steps[i];
        if (!strcmp(s->name, "textures/t_arena/wall")) mat = s->shadow && !strcmp(s->type, "material");
        if (!strcmp(s->name, S_MEGA)) mega = s->shadow && !strcmp(s->reg_path, "material/" S_MEGA ".decl");
        if (!strcmp(s->name, "mods/" ID "/arena")) info = !s->shadow && !strcmp(s->type, "snapModuleInfo");
    }
    CHECK(mat && mega && info);
    sh_modserve_plan_free(&plan);
}

/* ------------------------------------------------------ acceptance build ---- */

static void inspect_prefab(const char *path, const sh_mb_bundle *b)
{
    FILE *f = NULL;
    char *text = NULL, err[256] = "";
    long n = 0;
    sh_mp_summary sum;
    size_t i;
    int named = 0;
    if (fopen_s(&f, path, "rb") || !f) { CHECK(!"prefab readable"); return; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    text = (char *)malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) text[n] = 0; else { free(text); text = NULL; }
    fclose(f);
    if (!text) { CHECK(!"prefab read"); return; }
    memset(&sum, 0, sizeof sum);
    CHECK(sh_mp_is_module_file(text, (size_t)n) == 1);
    CHECK(sh_mp_inspect(text, (size_t)n, &sum, err, (int)sizeof err) == 1);
    if (err[0]) printf("  prefab: %s\n", err);
    for (i = 0; i < b->file_count && sum.module_count == 1; i++)
        if (b->files[i].is_module && !_stricmp(b->files[i].request, sum.modules[0].name)) named = 1;
    printf("modmodule_test: prefab %s: %d module(s) %s, %d entities, %d door slot(s), environments %d\n", path,
           sum.module_count, sum.module_count ? sum.modules[0].name : "-", sum.entity_count, sum.door_count, sum.env_count);
    CHECK(sum.module_count == 1 && sum.door_count == 1 && named);
    sh_mp_summary_free(&sum);
    free(text);
}

static void build_acceptance(const char *source, const char *doom, const char *prefab)
{
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    size_t i, modules = 0, restores = 0;
    sh_modstock_stats ms;
    double t0 = (double)GetTickCount64();
    sh_modstock_set_install(doom);
    if (!sh_mb_build(source, &RESOLVER, &b)) { CHECK(!"acceptance build ran"); return; }
    for (i = 0; i < b.file_count; i++) {
        if (b.files[i].is_module) modules++;
        if (b.files[i].stock && sh_mb_stock_restore(b.files[i].request, b.files[i].stock_source)) restores++;
    }
    sh_modstock_get_stats(&ms);
    printf("modmodule_test: acceptance %s: %u error(s), %u warning(s), %zu files, %u stock pointers (%zu restores), "
           "%llu bytes carried, %llu bytes pointed at, %zu module(s), manifest %zu bytes, %u needs (%u stock, %u unresolved), "
           "hash %s, %.0f ms, %llu bytes hashed\n",
           source, b.errors, b.warnings, b.file_count, b.stock_count, restores, b.total_size, b.stock_bytes, modules,
           b.canonical_len, b.needs.total, b.needs.stock, b.needs.unresolved, b.hash,
           (double)GetTickCount64() - t0, ms.bytes_hashed);
    if (b.errors) printf("  first cause: %s\n", b.first_cause);
    if (b.warnings) printf("  first warning: %s\n", b.first_warning);
    CHECK(b.errors == 0);
    if (!b.errors) CHECK(sh_mb_write(&b, source, cause, sizeof cause));
    if (prefab && prefab[0]) inspect_prefab(prefab, &b);
    sh_mb_free(&b);
}

int main(void)
{
    char tmp[MAX_PATH], install[MAX_PATH * 2], source[MAX_PATH * 2], doom[MAX_PATH * 2], prefab[MAX_PATH * 2];
    DWORD n;
    CHECK(mt_temp_dir("modmodule", tmp, sizeof tmp));
    _snprintf_s(install, sizeof install, _TRUNCATE, "%s\\doom", tmp);
    make_install(install);
    sh_modstock_register();
    test_names();
    test_builder(tmp, install);
    test_serving(tmp, install);
    sh_modstock_reset();
    mt_remove_tree(tmp);

    n = GetEnvironmentVariableA("SH_MODMODULE_SOURCE", source, sizeof source);
    if (n && n < sizeof source) {
        n = GetEnvironmentVariableA("SH_MODMODULE_DOOM", doom, sizeof doom);
        if (!n || n >= sizeof doom) strcpy_s(doom, sizeof doom, "D:\\SteamLibrary\\steamapps\\common\\DOOM");
        n = GetEnvironmentVariableA("SH_MODMODULE_PREFAB", prefab, sizeof prefab);
        if (!n || n >= sizeof prefab) prefab[0] = 0;
        sh_modstock_register();
        build_acceptance(source, doom, prefab);
        sh_modstock_reset();
    } else {
        printf("modmodule_test: acceptance build skipped (set SH_MODMODULE_SOURCE to a mod folder, SH_MODMODULE_DOOM to the install)\n");
    }
    sh_mb_set_stock_ops(NULL);
    if (g_failed) { fprintf(stderr, "modmodule_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("modmodule_test: module names, pointer-only kinds, stock restores, module builds and refusals, serving and plans pass (%d checks)\n",
           g_checks);
    return 0;
}
