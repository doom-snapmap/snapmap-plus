/* modserve_test.c -- the installed-mod index and provider (modserve.c)
 * against a fixture mods tree written to a temporary folder: admission,
 * failure isolation (size mismatch, unvalidated state, namespace, unbuilt
 * folder), a shadow conflict, two builds of one salt, a map's place,
 * lookups and their cost, provider precedence against navigation, the Grid
 * Room and built-in defaults (the real overrides.c), probe claims, the
 * registration plan's dependency order and blocking, plan runs through fake
 * engine operations, the save-time bundle closure and a rescan. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modserve.h"
#include "decl_tree.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "store_lock.h"
#include "modstore.h"
#include "overrides.h"

void sh_modserve_test_cache(int enabled);
size_t sh_modserve_test_cache_bytes(void);
void sh_modserve_test_reset_registration(void);
void sh_modserve_test_reset_gates(void);
unsigned sh_modserve_test_gates_owned(void);

static int g_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

/* ------------------------------------------------------------------ log capture */

static char g_log[65536];
static size_t g_log_len;

void backend_log(const char *message)
{
    size_t n = message ? strlen(message) : 0;
    if (g_log_len + n + 2 < sizeof g_log) {
        memcpy(g_log + g_log_len, message, n);
        g_log_len += n;
        g_log[g_log_len++] = '\n';
        g_log[g_log_len] = 0;
    }
}
static int log_count_in(const char *hay, const char *needle)
{
    int n = 0;
    while ((hay = strstr(hay, needle)) != NULL) { n++; hay++; }
    return n;
}
static void log_clear(void) { g_log_len = 0; g_log[0] = 0; }
static int log_has(const char *needle) { return strstr(g_log, needle) != NULL; }
static int log_count(const char *needle)
{
    int n = 0;
    const char *p = g_log;
    while ((p = strstr(p, needle)) != NULL) { n++; p++; }
    return n;
}

/* ------------------------------------------------------------------ provider stubs */

#include "../src/backend/nav_bake.h"
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped, unsigned char **out_bytes, size_t *out_len)
{
    (void)name; (void)read_shipped;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}
int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{
    (void)name;
    if (out) *out = NULL;
    if (length) *length = 0;
    return 0;
}
/* Map navigation claims exactly one name, which a bundle also ships. */
#define NAV_NAME "maps/game/snapmap/test/nav_owned.decl"
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    *out_bytes = NULL;
    *out_len = 0;
    if (name && !_stricmp(name, NAV_NAME)) {
        *out_bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 3);
        memcpy(*out_bytes, "NAV", 3);
        *out_len = 3;
        return 1;
    }
    return 0;
}
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }

/* ------------------------------------------------------------------ fixture */

static char g_root[MAX_PATH];   /* ...\mods */

/* Fixture paths are UTF-8, as the store spells them (a root may hold a
 * non-ASCII character: test_store_root_utf8). */
static void fx_wide(const char *path, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, out, cap)) out[0] = 0;
}

static void mkdirs(const char *path)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *p;
    fx_wide(path, tmp, MAX_PATH);
    if (!tmp[0]) return;
    for (p = tmp + 3; *p; ++p)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
    CreateDirectoryW(tmp, NULL);
}

static void write_file(const char *path, const char *body, size_t len)
{
    char dir[MAX_PATH];
    wchar_t wpath[MAX_PATH];
    char *slash;
    FILE *f = NULL;
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; mkdirs(dir); }
    fx_wide(path, wpath, MAX_PATH);
    if (wpath[0]) f = _wfopen(wpath, L"wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); g_failed++; return; }
    fwrite(body, 1, len, f);
    fclose(f);
}

typedef struct fx_file {
    const char *path;      /* under assets/ */
    const char *body;      /* written to the tree (NULL: not written) */
    long long size;        /* -1: the body's size */
    const char *ptype, *pname;
    const char *needs;     /* raw JSON array text or NULL */
    char blob;             /* one hex digit repeated */
} fx_file;

static const char *hash64(char c, char *out)
{
    memset(out, c, 64);
    out[64] = 0;
    return out;
}

/* Raw author-block members (with a trailing comma) the next make_bundle writes. */
static const char *g_bundle_extra;
static const char *g_block_edits;   /* the product block's "edits" of the next make_bundle, or NULL */
/* The author's "name" the next make_bundle writes (NULL: the id). */
static const char *g_bundle_name;
/* A files[] path written as a stock pointer to the game's own bytes under the
 * same name (modbundle.h sh_mb_stock_restore), or NULL. */
static const char *g_fx_restore;
static const char *g_fx_restore2;   /* a second one, or NULL */
/* Write <root>\<folder>\mod.json (an author block and its format-2 builder
 * block, keyed by the salt `id`) and its files. A folder under
 * <root>\maps\<segment>\ sits in that map's place. `state` is the old
 * .state.json shape, mapped to the block's cause and built. */
static void make_bundle(const char *parent, const char *folder, const char *id, char hash_digit,
                        const char *shadows, const fx_file *files, int n, const char *state)
{
    char dir[MAX_PATH], path[MAX_PATH], h[65];
    static char json[32768];
    size_t at = 0;
    int i;
    const char *extra = g_bundle_extra ? g_bundle_extra : "";
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", parent, folder);
    mkdirs(dir);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"name\": \"%s\",\n  %s\n"
                      "  \"snapmap-plus\": {\"format\": 2, \"salt\": \"%s\", \"rules\": 8, \"build\": \"%s\"",
                      g_bundle_name ? g_bundle_name : id, extra, id, hash64(hash_digit, h));
    if (state) {
        const char *c = strstr(state, "\"firstCause\": \""), *t = strstr(state, "\"installedAt\": ");
        if (c) {
            const char *e = strchr(c + 15, '"');
            at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ", \"cause\": \"%.*s\"", (int)(e - c - 15), c + 15);
        }
        if (t) at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ", \"built\": %d", atoi(t + 15));
    }
    if (g_block_edits) at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ",\n    \"edits\": %s", g_block_edits);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ",\n    \"shadows\": [%s],\n    \"files\": [\n", shadows ? shadows : "");
    for (i = 0; i < n; ++i) {
        const fx_file *f = &files[i];
        long long size = f->size >= 0 ? f->size : (long long)strlen(f->body ? f->body : "");
        char b[65];
        if ((g_fx_restore && !strcmp(g_fx_restore, f->path)) || (g_fx_restore2 && !strcmp(g_fx_restore2, f->path)))
            at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                              "      {\"path\": \"assets/%s\", \"stock\": {\"source\": \"%s\", \"sha256\": \"%s\"}, "
                              "\"provides\": {\"type\": \"%s\", \"name\": \"%s\"}, \"needs\": %s}%s\n",
                              f->path, f->path, hash64(f->blob, b), f->ptype, f->pname, f->needs ? f->needs : "[]",
                              i + 1 < n ? "," : "");
        else
        at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                          "      {\"path\": \"assets/%s\", \"sha256\": \"%s\", \"size\": %lld, "
                          "\"provides\": {\"type\": \"%s\", \"name\": \"%s\"}, \"needs\": %s}%s\n",
                          f->path, hash64(f->blob, b), size, f->ptype, f->pname, f->needs ? f->needs : "[]",
                          i + 1 < n ? "," : "");
        if (f->body) {
            char *p;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\%s", dir, f->path);
            for (p = path; *p; ++p) if (*p == '/') *p = '\\';
            write_file(path, f->body, strlen(f->body));
        }
    }
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, "    ]\n  }\n}\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
    write_file(path, json, at);
}

#define A_MD6    "generated/decls/md6def/mods/alexpink/imp.md6.decl"
#define A_MESH   "generated/basemodel/md6/mods/alexpink/mesh/imp.bmd6model"
#define A_ENT    "generated/decls/entitydef/mods/alexpink/imp.decl"
#define A_SEDEF  "generated/decls/snapeditorentitydef/mods/alexpink/imp.decl"
#define A_FIRE   "generated/decls/entitydef/mods/alexpink/fireball.decl"
#define A_MAT    "generated/decls/material2/mods/alexpink/skin.decl"
#define SHADOW   "generated/decls/entitydef/monster/imp.decl"
#define LIFT     "generated/decls/snapeditorentitydef/func/lift.decl"
#define GRID     "generated/decls/snapmoduleinfo/smpgrid/v1/c/512_768_384.decl"
#define B_THING  "generated/decls/entitydef/mods/bobskins/thing.decl"
#define B_SEDEF  "generated/decls/snapeditorentitydef/mods/bobskins/thing.decl"
#define C_ONLY   "generated/decls/entitydef/mods/carolbro/only.decl"
#define F_BLOB   "generated/image/mods/fayblob0/tex.bimage"

static char g_hash_a[65], g_hash_a2[65], g_hash_b[65], g_hash_c[65], g_hash_f[65], g_hash_l[65];

static void build_fixture(void)
{
    char tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%smodserve_test_%lu\\mods", tmp, GetCurrentProcessId());
    mkdirs(g_root);
    {
        static const fx_file a[] = {
            { A_MESH, "MESHBYTES-0123456789", -1, "baseModel", "md6/mods/alexpink/mesh/imp.md6mesh", NULL, '1' },
            { A_MAT, "material2 skin {}", -1, "material2", "mods/alexpink/skin", NULL, '2' },
            { A_MD6, "md6Def imp { mesh \"md6/mods/alexpink/mesh/imp.md6mesh\" }", -1, "md6Def",
              "mods/alexpink/imp.md6",
              "[{\"type\":\"baseModel\",\"name\":\"md6/mods/alexpink/mesh/imp.md6mesh\"},"
              "{\"type\":\"material2\",\"name\":\"mods/alexpink/skin\"},"
              "{\"type\":\"material2\",\"name\":\"models/monsters/imp/imp\"}]", '3' },
            { A_ENT, "entityDef imp { inherit \"snapmaps/placeablesnapaiencounter/imp\" }", -1, "entityDef",
              "mods/alexpink/imp", "[{\"type\":\"md6Def\",\"name\":\"mods/alexpink/imp.md6\"}]", '4' },
            /* Listed before its needs on purpose: the plan reorders. */
            { A_SEDEF, "{ entityDef \"mods/alexpink/imp\" }", -1, "snapEditorEntityDef", "mods/alexpink/imp",
              "[{\"type\":\"entityDef\",\"name\":\"mods/alexpink/imp\"},"
              "{\"type\":\"md6Def\",\"name\":\"mods/alexpink/imp.md6\"}]", '5' },
            { A_FIRE, "entityDef fireball {}", -1, "entityDef", "mods/alexpink/fireball", NULL, '6' },
            { SHADOW, "entityDef pink { }", -1, "entityDef", "monster/imp", NULL, '7' },
            { LIFT, "MODLIFT", -1, "snapEditorEntityDef", "func/lift", NULL, '8' },
            { GRID, "MODGRID", -1, "snapModuleInfo", "smpgrid/v1/c/512_768_384", NULL, '9' },
            { "maps/game/snapmap/test/nav_owned.decl", "MODNAV", -1, "file", "maps/game/snapmap/test/nav_owned.decl", NULL, 'a' },
        };
        /* The NAV shadow path is not namespaced; it is declared a shadow. */
        make_bundle(g_root, "alexpink", "alexpink", 'a',
                    "\"" SHADOW "\", \"" LIFT "\", \"" GRID "\", \"maps/game/snapmap/test/nav_owned.decl\"",
                    a, (int)(sizeof a / sizeof a[0]), "{\"validated\": true, \"installedAt\": 100}");
        hash64('a', g_hash_a);
    }
    {
        /* A later build of the same mod changing one file, in a map's place
         * (a downloaded map pinned it); the root folder serves with no map. */
        static const fx_file a2[] = {
            { A_FIRE, "entityDef fireball { v2 }", -1, "entityDef", "mods/alexpink/fireball", NULL, 'b' },
        };
        make_bundle(g_root, "maps\\Pinks\\alexpink", "alexpink", 'b', NULL, a2, 1,
                    "{\"validated\": true, \"installedAt\": 200}");
        hash64('b', g_hash_a2);
    }
    {
        /* Shadows the same stock entityDef: a conflict. Its editor row needs
         * an md6Def nobody provides: blocked. */
        static const fx_file b[] = {
            { SHADOW, "entityDef bob { }", -1, "entityDef", "monster/imp", NULL, 'c' },
            { B_THING, "entityDef thing {}", -1, "entityDef", "mods/bobskins/thing",
              "[{\"type\":\"md6Def\",\"name\":\"mods/alexpink/imp.md6\"}]", 'd' },
            { B_SEDEF, "{ entityDef \"mods/bobskins/thing\" }", -1, "snapEditorEntityDef", "mods/bobskins/thing",
              "[{\"type\":\"entityDef\",\"name\":\"mods/bobskins/thing\"},"
              "{\"type\":\"md6Def\",\"name\":\"mods/bobskins/missing.md6\"}]", 'e' },
        };
        make_bundle(g_root, "bobskins", "bobskins", 'c', "\"" SHADOW "\"", b, 3, NULL);
        hash64('c', g_hash_b);
    }
    {
        /* Size mismatch: skipped entirely. */
        static const fx_file c[] = {
            { C_ONLY, "entityDef only {}", 999, "entityDef", "mods/carolbro/only", NULL, 'f' },
        };
        make_bundle(g_root, "carolbro", "carolbro", 'd', NULL, c, 1, NULL);
        hash64('d', g_hash_c);
    }
    {
        static const fx_file d[] = {
            { "generated/decls/entitydef/mods/davex000/a.decl", "x", -1, "entityDef", "mods/davex000/a", NULL, '1' },
        };
        make_bundle(g_root, "davex000", "davex000", 'e', NULL, d, 1,
                    "{\"validated\": false, \"firstCause\": \"md6Def mesh missing\"}");
    }
    {
        /* A new name outside the namespace that is not declared a shadow. */
        static const fx_file e[] = {
            { "generated/decls/entitydef/monster/sneaky.decl", "x", -1, "entityDef", "monster/sneaky", NULL, '1' },
        };
        make_bundle(g_root, "erinns00", "erinns00", '1', NULL, e, 1, NULL);
    }
    {
        /* A raw file: its own tree holds it (there is no blob store). */
        static const fx_file f[] = { { F_BLOB, "BLOBBYTES", 9, "image", "mods/fayblob0/tex", NULL, '2' } };
        make_bundle(g_root, "fayblob0", "fayblob0", '2', NULL, f, 1, NULL);
        hash64('2', g_hash_f);
    }
    {
        /* A sound bank is named by its stem: <salt>_<rest>
         * is inside the namespace, any other stem is outside it. */
        static const fx_file g[] = {
            { "sound/soundbanks/pc/gailsnd0_fx.bnk", "BKHD", -1, "soundBank", "sound/soundbanks/pc/gailsnd0_fx.bnk", NULL, '5' },
        };
        static const fx_file h[] = {
            { "sound/soundbanks/pc/other_fx.bnk", "BKHD", -1, "soundBank", "sound/soundbanks/pc/other_fx.bnk", NULL, '6' },
        };
        make_bundle(g_root, "gailsnd0", "gailsnd0", '5', NULL, g, 1, NULL);
        make_bundle(g_root, "hanksnd0", "hanksnd0", '6', NULL, h, 1, NULL);
    }
    {
        /* A mod the player filed under an organizing folder: found by its
         * path under mods\. An empty organizing folder holds nothing. */
        static const fx_file nested[] = {
            { "generated/decls/entitydef/mods/ivyneste/thing.decl", "entityDef nested {}", -1, "entityDef", "mods/ivyneste/thing", NULL, '7' },
        };
        make_bundle(g_root, "demon-mods\\ivyneste", "ivyneste", '8', NULL, nested, 1, NULL);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\empty-group\\deeper", g_root);
        mkdirs(path);
    }
    /* A hand-dropped folder that was never built. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\unbuilt\\mod.json", g_root);
    write_file(path, "{}", 2);
}

/* ------------------------------------------------------------------ helpers */

static int open_text(const char *name, char *out, size_t cap)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    int r = sh_modserve_open(name, &bytes, &len);
    out[0] = 0;
    if (r > 0) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, bytes, n);
        out[n] = 0;
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    return r;
}

static int provider_text(const char *name, unsigned mode, char *out, size_t cap)
{
    void *s = sh_overrides_test_open(name, mode);
    long long n;
    out[0] = 0;
    if (!s) return 0;
    n = sh_overrides_test_stream_length(s);
    if (n > 0) {
        size_t take = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        sh_overrides_test_stream_read(s, out, take);
        out[take] = 0;
    }
    sh_overrides_test_stream_close(s);
    return 1;
}

static int step_index(const sh_modserve_plan *p, const char *type, const char *name)
{
    int i;
    for (i = 0; i < p->count; ++i)
        if (!_stricmp(p->steps[i].type, type) && !strcmp(p->steps[i].name, name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ fake engine */

static char g_calls[8192];
static int g_registered_count;
static const char *g_fail_load;   /* this decl name fails to load */
static const char *g_already;     /* this decl name already has a source */

static void call_log(const char *kind, const char *type, const char *name)
{
    size_t at = strlen(g_calls);
    _snprintf_s(g_calls + at, sizeof g_calls - at, _TRUNCATE, "%s:%s:%s;", kind, type, name);
}
static int fake_source(void *c, const char *type, const char *name)
{
    (void)c; (void)type;
    return g_already && !strcmp(name, g_already);
}
static int fake_register(void *c, const char *type, const char *name, const char *reg)
{
    (void)c;
    call_log("R", type, name);
    if (!strstr(reg, ".decl")) return 0;
    g_registered_count++;
    return 1;
}
static int fake_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    (void)c;
    call_log(persist ? "P" : "L", type, name);
    *lifetime = persist ? 4 : 1;
    if (g_fail_load && !strcmp(name, g_fail_load)) { *state = 0x01; return 0; }
    *state = 0xa0;
    return 1;
}
static const sh_modserve_engine_ops g_fake = { NULL, fake_source, fake_register, fake_load };
static void fake_published(void *c) { (void)c; call_log("S", "", ""); }
static const sh_modserve_engine_ops g_fake_published = { NULL, fake_source, fake_register, fake_load, NULL, fake_published };

static double now_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

/* ------------------------------------------------------------------ tests */

/* Index listeners: every call site keeps its own link, so any number of
 * listeners is called after each table, in the order they were added. */
static int g_heard[8], g_heard_n;
static void heard0(void) { g_heard[g_heard_n++ & 7] = 0; }
static void heard1(void) { g_heard[g_heard_n++ & 7] = 1; }
static void heard2(void) { g_heard[g_heard_n++ & 7] = 2; }
static void heard3(void) { g_heard[g_heard_n++ & 7] = 3; }
static void heard4(void) { g_heard[g_heard_n++ & 7] = 4; }
static void heard5(void) { g_heard[g_heard_n++ & 7] = 5; }

static void test_index_listeners(void)
{
    char missing[MAX_PATH];
    _snprintf_s(missing, sizeof missing, _TRUNCATE, "%s\\does-not-exist", g_root);
    sh_modserve_set_index_listener(heard0);
    sh_modserve_set_index_listener(heard1);
    sh_modserve_set_index_listener(heard2);
    sh_modserve_set_index_listener(heard3);
    sh_modserve_set_index_listener(heard4);
    sh_modserve_set_index_listener(heard5);
    sh_modserve_set_index_listener(heard2);   /* a second time: ignored */
    g_heard_n = 0;
    (void)sh_modserve_index_build(missing);
    CHECK(g_heard_n == 6);
    CHECK(g_heard[0] == 0 && g_heard[1] == 1 && g_heard[2] == 2 && g_heard[3] == 3 && g_heard[4] == 4 && g_heard[5] == 5);
    sh_modserve_set_index_listener(NULL);
    g_heard_n = 0;
    (void)sh_modserve_index_build(missing);
    CHECK(g_heard_n == 0);
}


static void test_empty_index(void)
{
    char missing[MAX_PATH];
    unsigned char *b = (unsigned char *)1;
    size_t n = 7;
    log_clear();
    _snprintf_s(missing, sizeof missing, _TRUNCATE, "%s\\does-not-exist", g_root);
    CHECK(sh_modserve_index_build(missing) == 0);
    CHECK(!sh_modserve_active());
    CHECK(sh_modserve_open(A_ENT, &b, &n) == 0 && b == NULL && n == 0);
    CHECK(log_has("MODS: index built in ") && log_has("(0 bundles, 0 files)"));
}

static void test_index_and_isolation(void)
{
    sh_modserve_stats st;
    log_clear();
    CHECK(sh_modserve_index_build(g_root) == 6);   /* alex (root and a place), bob, fay, gail, ivy (nested) */
    sh_modserve_get_stats(&st);
    CHECK(st.bundles_seen == 11);
    CHECK(st.bundles_admitted == 6);
    CHECK(st.conflicts == 1);
    CHECK(log_has("bundle 'carolbro' skipped: size mismatch: '" C_ONLY "' is 17 bytes, mod.json says 999"));
    CHECK(log_has("bundle 'davex000' skipped: not built: md6Def mesh missing"));
    CHECK(log_has("bundle 'erinns00' skipped: 'generated/decls/entitydef/monster/sneaky.decl' is outside mods/erinns00/"));
    CHECK(log_has("bundle 'unbuilt' skipped: not built yet"));
    CHECK(log_has("bundle 'hanksnd0' skipped: 'sound/soundbanks/pc/other_fx.bnk' is outside mods/hanksnd0/"));
    CHECK(!log_has("bundle 'gailsnd0' skipped"));
    CHECK(log_count("skipped:") == 5);   /* one line per skipped bundle */
    /* Two mods' shadows of one stock decl: with no map, the static winner
     * (no rule separates them: the first salt; the install time never counts;
     * the texts are outside the composable grammar). */
    CHECK(log_has("conflict: shadow '" SHADOW "' is provided by alexpink, bobskins; serving "
                  "alexpink (no rule separates them; the first salt in name order"));
    /* Two builds of one salt (the root and a place) are never a conflict. */
    CHECK(log_has("MODS: mod alexpink: builds ") && log_has("each map serves the build it declares"));
    CHECK(!log_has("superseded"));
    CHECK(log_has("MODS: index built in ") && log_has("(6 bundles, "));
    CHECK(sh_modserve_lookup("generated/decls/entitydef/mods/ivyneste/thing.decl", NULL) != NULL);
    CHECK(sh_modserve_bundle_state(g_hash_a, NULL) == 1);
    {
        char cause[200];
        CHECK(HO(cause, sh_modserve_bundle_state(g_hash_c, HO_P(0))) == -1 && strstr(cause, "size mismatch"));
    }
    CHECK(sh_modserve_bundle_state("0000000000000000000000000000000000000000000000000000000000000000", NULL) == 0);
    printf("modserve_test: fixture index built in %.2f ms (%d bundles seen, %d files, %d keys)\n", st.build_ms,
           st.bundles_seen, st.files, st.keys);
}

/* ------------------------------------------------------------------ early decls */

typedef struct unopened_seen { char order[1024]; int settle_lift; } unopened_seen;
static int unopened_visit(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    unopened_seen *u = (unopened_seen *)context;
    size_t at = strlen(u->order);
    (void)request;
    _snprintf_s(u->order + at, sizeof u->order - at, _TRUNCATE, "%s:%s:%s;", type, name, bundle_id);
    return u->settle_lift && !strcmp(name, "func/lift");
}

/* Shadows the provider has not opened are offered in plan rank order, the
 * conflict winner only; a settled one and an opened one are not offered again. */
static void test_unopened_shadows(void)
{
    unopened_seen u;
    char text[256];
    CHECK(sh_modserve_open_count(SHADOW) == 0 && sh_modserve_open_count(LIFT) == 0);
    memset(&u, 0, sizeof u);
    CHECK(sh_modserve_each_unopened_shadow(unopened_visit, &u) >= 2);
    CHECK(strstr(u.order, "entityDef:monster/imp:alexpink;") && strstr(u.order, "snapEditorEntityDef:func/lift:alexpink;"));
    CHECK(strstr(u.order, "entityDef:monster/imp:") < strstr(u.order, "snapEditorEntityDef:func/lift:"));
    CHECK(!strstr(u.order, "bobskins"));
    CHECK(!strstr(u.order, "mods/"));   /* new identities are registered, never re-parsed */
    /* The lift is settled; the imp is not parsed yet and is offered again. */
    memset(&u, 0, sizeof u); u.settle_lift = 1;
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    memset(&u, 0, sizeof u);
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    CHECK(strstr(u.order, "monster/imp") && !strstr(u.order, "func/lift"));
    /* Once the provider serves the imp it is not offered either. */
    CHECK(open_text(SHADOW, text, sizeof text) == 1 && sh_modserve_open_count(SHADOW) == 1);
    memset(&u, 0, sizeof u);
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    CHECK(!strstr(u.order, "monster/imp") && !strstr(u.order, "func/lift"));
    CHECK(sh_modserve_each_unopened_shadow(NULL, NULL) == 0);
}

/* A rescan that serves the same bytes keeps opened and settled shadows
 * settled: the engine still holds what the earlier table served. */
static void test_unopened_after_rescan(void)
{
    unopened_seen u;
    CHECK(sh_modserve_index_rescan() >= 0);
    memset(&u, 0, sizeof u);
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    CHECK(!strstr(u.order, "monster/imp") && !strstr(u.order, "func/lift"));
}

static void test_lookup(void)
{
    sh_modserve_hit hit;
    char text[256];
    CHECK(sh_modserve_lookup(A_MD6, &hit) && hit.is_decl && !hit.is_shadow && hit.size > 0 &&
          !strcmp(hit.bundle_id, "alexpink"));
    /* Case and separators fold; the engine spells md6Def and baseModel. */
    CHECK(sh_modserve_lookup("generated/decls/md6Def/mods/alexpink/imp.md6.decl", NULL));
    CHECK(sh_modserve_lookup("GENERATED\\baseModel\\md6\\mods\\alexpink\\mesh\\imp.bmd6model", NULL));
    /* The registration source read and the sanitised raw spelling. */
    CHECK(sh_modserve_lookup("decltree/md6def/mods/alexpink/imp.md6.decl", NULL));
    CHECK(sh_modserve_lookup("generated/baseModel/md6/mods/alexpink/mesh/imp.bmd6model", NULL));
    /* Misses: stock, a skipped bundle's file, near names. */
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/monster/hellknight.decl", NULL));
    {
        sh_modserve_hit w;
        sh_modserve_conflict c;
        CHECK(sh_modserve_lookup(SHADOW, &w) && !strcmp(w.bundle_id, "alexpink"));
        CHECK(sh_modserve_conflict_info("GENERATED/decls/entitydef/monster/imp.decl", &c) && c.count == 2 &&
              !c.composed && !strcmp(c.served_id, "alexpink") && strstr(c.reason, "the first salt in name order") &&
              !strcmp(c.providers, "alexpink, bobskins"));
        CHECK(!sh_modserve_conflict_info(A_ENT, &c));
        CHECK(open_text(SHADOW, text, sizeof text) == 1 && !strcmp(text, "entityDef pink { }"));
    }
    CHECK(!sh_modserve_lookup(C_ONLY, NULL));
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/mods/davex000/a.decl", NULL));
    CHECK(!sh_modserve_lookup(A_ENT "x", NULL));
    CHECK(!sh_modserve_lookup("", NULL));
    CHECK(!sh_modserve_lookup(NULL, NULL));
    /* Bytes; the root folder's build over the one in a map's place. */
    CHECK(open_text(A_ENT, text, sizeof text) == 1 && !strcmp(text, "entityDef imp { inherit \"snapmaps/placeablesnapaiencounter/imp\" }"));
    CHECK(open_text(A_FIRE, text, sizeof text) == 1 && !strcmp(text, "entityDef fireball {}"));
    CHECK(open_text(F_BLOB, text, sizeof text) == 1 && !strcmp(text, "BLOBBYTES"));
}

static void test_precedence(void)
{
    char text[256];
    log_clear();
    /* A new mod name reaches the engine through the provider's stream. */
    CHECK(provider_text(A_SEDEF, 0, text, sizeof text) && !strcmp(text, "{ entityDef \"mods/alexpink/imp\" }"));
    CHECK(provider_text("generated/decls/snapEditorEntityDef/mods/alexpink/imp.decl", 1, text, sizeof text));
    /* Mode 2 is never served. */
    CHECK(!provider_text(A_SEDEF, 2, text, sizeof text));
    /* A bundle shadow of a built-in name wins over the built-in default (the
     * player's expanded Lift tile from the retired overrides layer). */
    CHECK(provider_text(LIFT, 0, text, sizeof text) && !strcmp(text, "MODLIFT"));
    /* Map navigation wins. */
    CHECK(provider_text(NAV_NAME, 0, text, sizeof text) && !strcmp(text, "NAV"));
    /* A Grid Room name is claimed by the Grid Room even when it cannot be
     * built here, and never falls to the bundle's bytes. */
    CHECK(!provider_text(GRID, 0, text, sizeof text) || strcmp(text, "MODGRID"));
    /* The bundle still holds those names; only the order decides. */
    CHECK(sh_modserve_lookup(LIFT, NULL) && sh_modserve_lookup(NAV_NAME, NULL) && sh_modserve_lookup(GRID, NULL));
    /* A shadow that is served logs once. */
    CHECK(open_text(LIFT, text, sizeof text) == 1 && open_text(LIFT, text, sizeof text) == 1);
    CHECK(log_count("shadow '" LIFT "' served") == 1);
}

static void test_probe(void)
{
    CHECK(sh_modserve_probe_claims("generated/decls/md6Def/mods/alexpink/imp.md6.decl"));
    CHECK(sh_modserve_probe_claims("generated/decls/snapEditorEntityDef/mods/alexpink/imp.decl"));
    CHECK(!sh_modserve_probe_claims(A_MESH));                 /* raw files are not decls */
    CHECK(sh_modserve_probe_claims(SHADOW));                  /* conflicted, and served by its winner */
    CHECK(!sh_modserve_probe_claims("generated/decls/entitydef/monster/hellknight.decl"));
    CHECK(sh_modserve_is_mod_sedef("mods/alexpink/imp"));
    CHECK(sh_modserve_is_mod_sedef("MODS/alexpink/IMP"));
    CHECK(!sh_modserve_is_mod_sedef("demons/imp_enc"));
}

static void test_plan(void)
{
    sh_modserve_plan p;
    int md6, mat, ent, sedef, bsedef, bthing, i, unknown = 0;
    const char *hashes[3];
    CHECK(sh_modserve_plan_editor(&p) >= 4);
    md6 = step_index(&p, "md6Def", "mods/alexpink/imp.md6");
    mat = step_index(&p, "material2", "mods/alexpink/skin");
    ent = step_index(&p, "entityDef", "mods/alexpink/imp");
    sedef = step_index(&p, "snapEditorEntityDef", "mods/alexpink/imp");
    bsedef = step_index(&p, "snapEditorEntityDef", "mods/bobskins/thing");
    bthing = step_index(&p, "entityDef", "mods/bobskins/thing");
    CHECK(md6 >= 0 && mat >= 0 && ent >= 0 && sedef >= 0 && bsedef >= 0 && bthing >= 0);
    CHECK(mat < md6 && md6 < ent && ent < sedef);           /* dependency order */
    CHECK(md6 < bthing);                                     /* across bundles */
    CHECK(step_index(&p, "entityDef", "mods/alexpink/fireball") < 0);   /* gameplay only */
    CHECK(step_index(&p, "entityDef", "monster/imp") < 0);   /* shadows are never registered */
    for (i = 0; i < p.count; ++i) CHECK(_stricmp(p.steps[i].type, "baseModel"));   /* raw: no step */
    /* The editor plan keeps every step for the session (lane MG2). */
    CHECK(p.steps[sedef].persist == 1 && p.steps[ent].persist == 1 && p.steps[md6].persist == 1);
    CHECK(!strcmp(p.steps[md6].reg_path, "md6def/mods/alexpink/imp.md6.decl"));
    CHECK(p.steps[bsedef].blocked && strstr(p.steps[bsedef].blocked, "mods/bobskins/missing.md6"));
    CHECK(!p.steps[bthing].blocked && !p.steps[sedef].blocked);
    CHECK(p.blocked == 1);
    sh_modserve_plan_free(&p);

    hashes[0] = g_hash_a;
    hashes[1] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    hashes[2] = g_hash_a2;
    CHECK(sh_modserve_plan_bundles(hashes, 3, &p, &unknown) >= 3);
    CHECK(unknown == 2);   /* a2 lies in a place: it serves only the map whose place that is */
    sh_modserve_plan_free(&p);
    CHECK(sh_modserve_place_set("maps\\Pinks") >= 0);
    CHECK(sh_modserve_plan_bundles(hashes, 3, &p, &unknown) >= 3);
    CHECK(unknown == 1);
    CHECK(sh_modserve_place_set(NULL) >= 0);
    CHECK(step_index(&p, "entityDef", "mods/alexpink/fireball") >= 0);
    CHECK(step_index(&p, "snapEditorEntityDef", "mods/alexpink/imp") < 0);
    CHECK(step_index(&p, "md6Def", "mods/alexpink/imp.md6") < step_index(&p, "entityDef", "mods/alexpink/imp"));
    CHECK(step_index(&p, "entityDef", "mods/bobskins/thing") < 0);   /* not named */
    /* A map pass keeps what the editor closure holds unless the engine holds it
     * permanently (2), as the editor pass would: every served file of the
     * root build, which has rows, the fireball included (the row-less 1.1.0
     * build in the place does not serve its path with no map open). */
    for (i = 0; i < p.count; ++i) CHECK(p.steps[i].persist == 2);
    CHECK(p.steps[step_index(&p, "entityDef", "mods/alexpink/fireball")].persist == 2);
    sh_modserve_plan_free(&p);
}

static void test_plan_run(void)
{
    sh_modserve_plan p;
    int ready, total;
    const char *r_last, *l_first;
    sh_modserve_test_reset_registration();
    g_calls[0] = 0;
    g_fail_load = "mods/alexpink/imp";       /* the alex entityDef fails to load */
    g_already = "mods/alexpink/skin";        /* this source is already registered */
    log_clear();
    total = sh_modserve_plan_editor(&p);
    ready = sh_modserve_plan_run(&p, &g_fake_published, "editor");
    /* skin, md6 and bob's entityDef are ready; the alex entityDef fails, so
     * its sedef is not loaded; bob's sedef is blocked. */
    CHECK(ready == 3);
    CHECK(!strstr(g_calls, "R:material2:mods/alexpink/skin"));    /* source existed */
    CHECK(strstr(g_calls, "P:material2:mods/alexpink/skin") != NULL);
    CHECK(strstr(g_calls, "R:md6Def:mods/alexpink/imp.md6") != NULL);
    CHECK(strstr(g_calls, "P:entityDef:mods/alexpink/imp;") != NULL);
    CHECK(strstr(g_calls, "R:snapEditorEntityDef:mods/alexpink/imp;") != NULL);
    CHECK(!strstr(g_calls, "P:snapEditorEntityDef:mods/alexpink/imp;"));
    CHECK(!strstr(g_calls, "snapEditorEntityDef:mods/bobskins/thing"));
    /* Every registration precedes every load. */
    r_last = strrchr(g_calls, 'R');
    l_first = strstr(g_calls, "P:");
    CHECK(r_last && l_first && r_last < l_first);
    /* The early-decl hook runs once, after the registrations and before the loads. */
    CHECK(log_count_in(g_calls, "S::;") == 1 && strstr(g_calls, "S::;") > r_last && strstr(g_calls, "S::;") < l_first);
    CHECK(log_has("editor: entityDef 'mods/alexpink/imp' (alexpink) NOT READY: state 0x01"));
    CHECK(log_has("editor: snapEditorEntityDef 'mods/alexpink/imp' (alexpink) NOT READY: needs entityDef 'mods/alexpink/imp', which failed"));
    CHECK(log_has("editor: snapEditorEntityDef 'mods/bobskins/thing' (bobskins) BLOCKED"));
    CHECK(log_has("editor: entityDef 'mods/bobskins/thing' (bobskins) READY: state 0xa0 lifetime 4"));
    CHECK(log_has("editor: md6Def 'mods/alexpink/imp.md6' (alexpink) READY"));
    CHECK(p.steps[step_index(&p, "entityDef", "mods/bobskins/thing")].ready == 1);
    CHECK(p.steps[step_index(&p, "snapEditorEntityDef", "mods/alexpink/imp")].ready == -1);
    sh_modserve_plan_free(&p);
    (void)total;

    /* A second run repeats nothing that is ready; the failed steps retry. */
    g_calls[0] = 0;
    g_fail_load = NULL;
    sh_modserve_plan_editor(&p);
    ready = sh_modserve_plan_run(&p, &g_fake, "editor");
    CHECK(!strstr(g_calls, "entityDef:mods/bobskins/thing"));
    CHECK(!strstr(g_calls, "md6Def:mods/alexpink/imp.md6"));
    CHECK(strstr(g_calls, "P:entityDef:mods/alexpink/imp;") && strstr(g_calls, "P:snapEditorEntityDef:mods/alexpink/imp;"));
    CHECK(ready == p.count - p.blocked);
    sh_modserve_plan_free(&p);
    g_already = NULL;
}

static void test_bundles_for_names(void)
{
    char out[4][SH_MODSERVE_HASH_HEX + 1];
    const char *names[] = { "mods/alexpink/fireball", "snapmaps/placeablesnapaiencounter/imp", "MODS/bobskins/thing" };
    int n = modserve_bundles_for_names(names, 3, out, 4), i, has_a2 = 0, has_b = 0, has_a = 0;
    /* fireball comes from the root folder's build (the place's 1.1.0 serves
     * only for its map); bob's thing needs alex 1.0.0's md6Def. */
    CHECK(n == 2);
    for (i = 0; i < n && i < 4; ++i) {
        has_a |= !strcmp(out[i], g_hash_a);
        has_a2 |= !strcmp(out[i], g_hash_a2);
        has_b |= !strcmp(out[i], g_hash_b);
    }
    CHECK(has_a && !has_a2 && has_b);
    CHECK(modserve_bundles_for_names(names, 3, out, 1) == 2);   /* counts past cap */
    CHECK(modserve_bundles_for_names(names + 1, 1, out, 4) == 0);   /* stock */
}

static void test_cost(void)
{
    const int N = 1000000;
    const char *hit = "generated/decls/md6Def/mods/alexpink/imp.md6.decl";
    const char *miss = "generated/decls/entitydef/characters/monsters/hellknight/hellknight_base.decl";
    volatile int sink = 0;
    double t0, hit_ns, miss_ns, cold_ns, warm_ns;
    int i;
    unsigned char *b;
    size_t n;
    t0 = now_ns();
    for (i = 0; i < N; ++i) sink += sh_modserve_lookup(hit, NULL);
    hit_ns = (now_ns() - t0) / N;
    t0 = now_ns();
    for (i = 0; i < N; ++i) sink += sh_modserve_lookup(miss, NULL);
    miss_ns = (now_ns() - t0) / N;
    CHECK(sink == N);
    /* Opens: disk read each time versus the small-file cache. */
    sh_modserve_test_cache(0);
    t0 = now_ns();
    for (i = 0; i < 2000; ++i) { CHECK(sh_modserve_open(A_MD6, &b, &n) == 1); HeapFree(GetProcessHeap(), 0, b); }
    cold_ns = (now_ns() - t0) / 2000;
    sh_modserve_test_cache(1);
    t0 = now_ns();
    for (i = 0; i < 2000; ++i) { CHECK(sh_modserve_open(A_MD6, &b, &n) == 1); HeapFree(GetProcessHeap(), 0, b); }
    warm_ns = (now_ns() - t0) / 2000;
    CHECK(sh_modserve_test_cache_bytes() > 0 && sh_modserve_test_cache_bytes() <= SH_MODSERVE_CACHE_TOTAL);
    printf("modserve_test: lookup %.1f ns hit, %.1f ns miss; open %.1f us uncached, %.1f us cached\n",
           hit_ns, miss_ns, cold_ns / 1000.0, warm_ns / 1000.0);
    CHECK(miss_ns < 2000.0);   /* a gross bound; the figure itself is the evidence */
}

static void test_rescan(void)
{
    static const fx_file g[] = {
        { "generated/decls/entitydef/mods/gusnew00/a.decl", "entityDef a {}", -1, "entityDef", "mods/gusnew00/a", NULL, '4' },
    };
    sh_modserve_plan p;
    make_bundle(g_root, "gusnew00", "gusnew00", '4', NULL, g, 1, NULL);
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/mods/gusnew00/a.decl", NULL));
    CHECK(sh_modserve_index_rescan() == 7);
    CHECK(sh_modserve_lookup("generated/decls/entitydef/mods/gusnew00/a.decl", NULL));
    CHECK(sh_modserve_lookup(A_ENT, NULL));
    /* Registrations made before the rescan are kept: nothing re-runs. */
    g_calls[0] = 0;
    sh_modserve_plan_editor(&p);
    sh_modserve_plan_run(&p, &g_fake, "editor");
    CHECK(!strstr(g_calls, "mods/alexpink/imp"));
    sh_modserve_plan_free(&p);
    /* Opened and settled shadows with unchanged bytes stay settled. */
    {
        unopened_seen u;
        memset(&u, 0, sizeof u);
        sh_modserve_each_unopened_shadow(unopened_visit, &u);
        CHECK(!strstr(u.order, "monster/imp") && !strstr(u.order, "func/lift"));
        CHECK(sh_modserve_open_count(SHADOW) > 0);
    }
}

/* ------------------------------------------------------------------ requirements */

/* A fake cvar system: the two blacklist gates as ints (as the engine's
 * matchers read them) and a few other cvars as text, one of them read-only.
 * A queued set lands only on drain; the cvar events, the console lines and
 * the registrations share one event sequence. */
static int g_gate_val[2], g_gate_pending[2], g_gate_has_pending[2];
static int g_gate_mode;          /* SH_MODSERVE_GATE_SET, _QUEUED, or 0 to refuse */
static int g_gate_reads, g_gate_sets, g_gate_drains, g_gate_lines;
#define FAKE_TEXT 4
static const char *const FAKE_NAMES[FAKE_TEXT] = { "com_maxFps", "r_fancy", "ro_locked", "g_label" };
static char g_text_val[FAKE_TEXT][64];
static char g_events[8192];
static void event(const char *e)
{
    size_t at = strlen(g_events);
    _snprintf_s(g_events + at, sizeof g_events - at, _TRUNCATE, "%s;", e);
}
static int gate_index(const char *cvar)
{
    return !strcmp(cvar, "g_useImageBlackList") ? 0 : !strcmp(cvar, "g_useResourceBlackList") ? 1 : -1;
}
static int text_index(const char *cvar)
{
    int i;
    for (i = 0; i < FAKE_TEXT; ++i) if (!strcmp(cvar, FAKE_NAMES[i])) return i;
    return -1;
}
static int gate_read(void *c, const char *cvar, char **value)
{
    int g = gate_index(cvar), t = text_index(cvar);
    (void)c;
    g_gate_reads++;
    if (g >= 0) { ho_setf(value, "%d", g_gate_val[g]); return 1; }
    if (t >= 0) { ho_set(value, g_text_val[t]); return 1; }
    return 0;
}
static int gate_set(void *c, const char *cvar, const char *value)
{
    char e[160];
    int g = gate_index(cvar), t = text_index(cvar);
    (void)c;
    g_gate_sets++;
    _snprintf_s(e, sizeof e, _TRUNCATE, "S:%s=%s", cvar, value);
    event(e);
    if (t == 2) return SH_MODSERVE_GATE_READ_ONLY;
    if ((g < 0 && t < 0) || !g_gate_mode) return 0;
    if (t >= 0) { strncpy_s(g_text_val[t], sizeof g_text_val[t], value, _TRUNCATE); return SH_MODSERVE_GATE_SET; }
    if (g_gate_mode == SH_MODSERVE_GATE_SET) g_gate_val[g] = atoi(value);
    else { g_gate_pending[g] = atoi(value); g_gate_has_pending[g] = 1; }
    return g_gate_mode;
}
static int gate_drain(void *c)
{
    int g;
    (void)c;
    g_gate_drains++;
    event("D");
    for (g = 0; g < 2; ++g) if (g_gate_has_pending[g]) { g_gate_val[g] = g_gate_pending[g]; g_gate_has_pending[g] = 0; }
    return 1;
}
static int gate_line(void *c, const char *line)
{
    char e[300];
    (void)c;
    g_gate_lines++;
    _snprintf_s(e, sizeof e, _TRUNCATE, "C:%s", line);
    event(e);
    return 1;
}
static const sh_modserve_gate_ops g_gates = { NULL, gate_read, gate_set, gate_drain, gate_line };
static const sh_modserve_gate_ops g_gates_nodrain = { NULL, gate_read, gate_set, NULL, gate_line };

/* The registration fakes, recording into the same event sequence. */
static int ev_source(void *c, const char *type, const char *name) { (void)c; (void)type; (void)name; return 0; }
static int ev_register(void *c, const char *type, const char *name, const char *reg)
{
    char e[300];
    (void)c; (void)reg;
    _snprintf_s(e, sizeof e, _TRUNCATE, "R:%s:%s", type, name);
    event(e);
    return 1;
}
static int ev_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    char e[300];
    (void)c;
    _snprintf_s(e, sizeof e, _TRUNCATE, "L:%s:%s", type, name);
    event(e);
    *state = 0;
    *lifetime = persist ? 4 : 1;
    return 1;
}
static const sh_modserve_engine_ops g_ev = { NULL, ev_source, ev_register, ev_load };

static void gates_reset(int image, int resource, int mode)
{
    static const char *const defaults[FAKE_TEXT] = { "200", "0", "0", "" };
    int i;
    g_gate_val[0] = image;
    g_gate_val[1] = resource;
    g_gate_has_pending[0] = g_gate_has_pending[1] = 0;
    for (i = 0; i < FAKE_TEXT; ++i) strcpy_s(g_text_val[i], sizeof g_text_val[i], defaults[i]);
    g_gate_mode = mode;
    g_gate_reads = g_gate_sets = g_gate_drains = g_gate_lines = 0;
    g_events[0] = 0;
    sh_modserve_test_reset_gates();
    sh_modserve_test_reset_registration();
}

/* Offset of the first and last occurrence of `needle` in g_events, or -1. */
static long ev_first(const char *needle) { const char *p = strstr(g_events, needle); return p ? (long)(p - g_events) : -1; }
static long ev_last(const char *needle)
{
    const char *p = g_events, *last = NULL;
    while ((p = strstr(p, needle)) != NULL) { last = p; p++; }
    return last ? (long)(last - g_events) : -1;
}

#define CY_MD6   "generated/decls/md6def/mods/cydcut00/cyber.md6.decl"
#define CY_ENT   "generated/decls/entitydef/mods/cydcut00/cyber.decl"
#define CY_SEDEF "generated/decls/snapeditorentitydef/mods/cydcut00/cyber.decl"
#define IM_SEDEF "generated/decls/snapeditorentitydef/mods/imgonly0/font.decl"
#define PL_SEDEF "generated/decls/snapeditorentitydef/mods/plainmod/thing.decl"

static void make_plain(const char *root)
{
    static const fx_file p[] = {
        { PL_SEDEF, "{ plain }", -1, "snapEditorEntityDef", "mods/plainmod/thing", NULL, '1' },
    };
    g_bundle_extra = NULL;
    make_bundle(root, "plainmod", "plainmod", '7', NULL, p, 1, NULL);
}

static void build_requirements_fixture(const char *root)
{
    static const fx_file cy[] = {
        { CY_MD6, "md6Def cyber {}", -1, "md6Def", "mods/cydcut00/cyber.md6", NULL, '1' },
        { CY_ENT, "entityDef cyber {}", -1, "entityDef", "mods/cydcut00/cyber",
          "[{\"type\":\"md6Def\",\"name\":\"mods/cydcut00/cyber.md6\"}]", '2' },
        { CY_SEDEF, "{ entityDef \"mods/cydcut00/cyber\" }", -1, "snapEditorEntityDef", "mods/cydcut00/cyber",
          "[{\"type\":\"entityDef\",\"name\":\"mods/cydcut00/cyber\"}]", '3' },
    };
    static const fx_file im[] = {
        { IM_SEDEF, "{ font }", -1, "snapEditorEntityDef", "mods/imgonly0/font", NULL, '4' },
    };
    static const fx_file one[] = {
        { "generated/decls/entitydef/mods/badx0000/a.decl", "x", -1, "entityDef", "mods/badx0000/a", NULL, '5' },
    };
    /* Both gates, in the converter's spelling and as a number. */
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": 0}},\n  ";
    make_bundle(root, "cydcut00", "cydcut00", '1', NULL, cy, 3, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useImageBlackList\": \"0\"}},\n  ";
    make_bundle(root, "imgonly0", "imgonly0", '2', NULL, im, 1, NULL);
    /* What the builder refuses: a name that is not a cvar's, a decimal
     * number, a line holding two commands, an unknown key. */
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"9lives\": \"0\"}},\n  ";
    make_bundle(root, "bad cvar", "badx0000", '3', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"com_maxFps\": 0.5}},\n  ";
    make_bundle(root, "bad value", "badx0000", '4', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"dlls\": {\"x\": \"0\"}},\n  ";
    make_bundle(root, "bad key", "badx0000", '5', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"commands\": {\"activate\": [\"echo a; quit\"]}},\n  ";
    make_bundle(root, "bad line", "badx0000", '6', NULL, one, 1, NULL);
    make_plain(root);
    g_bundle_extra = NULL;
}

static void test_requirements(void)
{
    char top[MAX_PATH], req_root[MAX_PATH], plain_root[MAX_PATH];
    char *slash;
    sh_modserve_plan p;
    sh_modserve_gates r;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(req_root, sizeof req_root, _TRUNCATE, "%s\\req\\mods", top);
    _snprintf_s(plain_root, sizeof plain_root, _TRUNCATE, "%s\\plain\\mods", top);
    mkdirs(req_root);
    mkdirs(plain_root);
    build_requirements_fixture(req_root);
    make_plain(plain_root);

    /* Collection at index time: what the builder refuses is that bundle's
     * first cause; each requirement is logged once per bundle. */
    log_clear();
    CHECK(sh_modserve_index_build(req_root) == 3);
    CHECK(log_count("MODS: requirements: g_useResourceBlackList=0 for cydcut00") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for cydcut00") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for imgonly0") == 1);
    CHECK(log_count("MODS: requirements:") == 3);
    CHECK(log_has("bundle 'bad cvar' skipped: requirements.cvars key 9lives is not a cvar name"));
    CHECK(log_has("bundle 'bad value' skipped: requirements.cvars.com_maxFps must be a string, an integer or a boolean"));
    CHECK(log_has("bundle 'bad key' skipped: requirements key dlls is not supported (cvars, commands)"));
    CHECK(log_has("bundle 'bad line' skipped: requirements.commands.activate[0] must be one console command"));
    CHECK(!log_has("plainmod' skipped"));
    /* The distinct cvars the admitted bundles set. */
    CHECK(sh_modserve_requirements() == 2 && sh_modserve_requirements_held() == 1);
    /* A rescan of the same bundles does not repeat the lines. */
    log_clear();
    CHECK(sh_modserve_index_rescan() == 3);
    CHECK(!log_has("MODS: requirements:"));

    /* Queued sets: phase 1 sets, the buffer is drained, phase 2 reads back,
     * and only then does the first DeclRegisterFile run. */
    gates_reset(1, 1, SH_MODSERVE_GATE_QUEUED);
    log_clear();
    CHECK(sh_modserve_plan_editor(&p) == 5);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 5);
    CHECK(r.needed == 2 && r.changed == 2 && r.queued == 2 && r.drained == 1 && r.live == 2 && !r.not_live);
    CHECK(g_gate_val[0] == 0 && g_gate_val[1] == 0 && g_gate_drains == 1 && g_gate_sets == 2);
    CHECK(ev_first("S:g_useImageBlackList=0") >= 0 && ev_first("S:g_useResourceBlackList=0") >= 0);
    CHECK(ev_last("S:") < ev_first("D;") && ev_first("D;") < ev_first("R:"));
    CHECK(ev_first("R:md6Def:mods/cydcut00/cyber.md6") > ev_first("D;"));
    CHECK(log_has("MODS: editor: requirements applied: g_useImageBlackList=0, g_useResourceBlackList=0 "
                  "(queued, live after a command-buffer drain)"));
    CHECK(sh_modserve_test_gates_owned() == 2);
    sh_modserve_plan_free(&p);

    /* Already live: read, never set or drained, and nothing logged. */
    sh_modserve_test_reset_registration();
    g_gate_sets = g_gate_drains = 0;
    log_clear();
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 5);
    CHECK(r.live == 2 && !r.changed && g_gate_sets == 0 && g_gate_drains == 0 && !log_has("requirements applied"));
    sh_modserve_plan_free(&p);

    /* An immediate route needs no drain. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "editor", &r) == 5);
    CHECK(r.live == 2 && r.changed == 2 && !r.queued && !r.drained && g_gate_drains == 0);
    CHECK(ev_last("S:") < ev_first("R:"));
    sh_modserve_plan_free(&p);

    /* Queued with nothing to drain it: the cvars are not live, so the bundles
     * that need them are blocked and register nothing; the plain bundle runs. */
    gates_reset(1, 1, SH_MODSERVE_GATE_QUEUED);
    log_clear();
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "editor", &r) == 1);
    CHECK(r.not_live == 2 && !r.live && !r.drained);
    CHECK(ev_first("R:snapEditorEntityDef:mods/plainmod/thing") >= 0);
    CHECK(ev_first("cydcut00") < 0 && ev_first("imgonly0") < 0);
    CHECK(log_has("queued but the command buffer could not be drained"));
    CHECK(log_has("editor: md6Def 'mods/cydcut00/cyber.md6' (cydcut00) BLOCKED: requirements g_useImageBlackList=0, "
                  "g_useResourceBlackList=0 are not live"));
    CHECK(log_has("(imgonly0) BLOCKED: requirement g_useImageBlackList=0 is not live"));
    sh_modserve_plan_free(&p);

    /* A refused set and an unavailable cvar system block the same way. */
    gates_reset(1, 0, 0);
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 1);
    CHECK(r.not_live == 1 && r.live == 1);
    sh_modserve_plan_free(&p);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_apply(NULL, "map", &r) == -1 && r.not_live == 2);
    CHECK(log_has("MODS: map: requirements g_useImageBlackList=0, g_useResourceBlackList=0 NOT applied"));

    /* Gates are released when no admitted bundle needs them any more, to the
     * value they displaced; after that nothing is touched again. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 1 && sh_modserve_test_gates_owned() == 2);
    CHECK(sh_modserve_index_build(plain_root) == 1);
    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0);
    CHECK(r.restored == 2 && g_gate_val[0] == 1 && g_gate_val[1] == 1 && sh_modserve_test_gates_owned() == 0);
    CHECK(log_has("requirement released: g_useResourceBlackList restored to 1"));
    g_gate_reads = g_gate_sets = 0;
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0 && g_gate_reads == 0 && g_gate_sets == 0);

    /* The empty case: no bundle needs a requirement, so the cvars are never
     * read or set, by the apply or by a gated plan run. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements() == 0 && sh_modserve_requirements_held() == 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0 && r.needed == 0);
    sh_modserve_plan_editor(&p);
    CHECK(p.count == 1 && sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 1);
    CHECK(g_gate_reads == 0 && g_gate_sets == 0 && g_gate_drains == 0 && ev_first("S:") < 0);
    sh_modserve_plan_free(&p);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* Requirements name no cvar or command the product knows: any cvar at any
 * value, console lines when they start and stop, the later install losing a
 * conflict, a cvar the engine refuses blocking only its own bundle. */
static char g_report[8192];
static void report_line(void *c, const char *line)
{
    size_t at = strlen(g_report);
    (void)c;
    _snprintf_s(g_report + at, sizeof g_report - at, _TRUNCATE, "%s\n", line);
}

static void make_gen(const char *root, const char *id, char digit, const char *req, int installed)
{
    char folder[64], pname[96], path[160], state[64], extra[512];
    fx_file f[1];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s", id);
    _snprintf_s(pname, sizeof pname, _TRUNCATE, "mods/%s/thing", id);
    _snprintf_s(path, sizeof path, _TRUNCATE, "generated/decls/snapeditorentitydef/mods/%s/thing.decl", id);
    _snprintf_s(state, sizeof state, _TRUNCATE, "\"installedAt\": %d", installed);
    _snprintf_s(extra, sizeof extra, _TRUNCATE, "\"requirements\": %s,\n  ", req);
    f[0].path = path; f[0].body = "{ thing }"; f[0].size = -1; f[0].ptype = "snapEditorEntityDef"; f[0].pname = pname;
    f[0].needs = NULL; f[0].blob = digit;
    g_bundle_extra = extra;
    make_bundle(root, folder, id, digit, NULL, f, 1, state);
    g_bundle_extra = NULL;
}

static void test_requirements_general(void)
{
    char top[MAX_PATH], root[MAX_PATH], root2[MAX_PATH], plain_root[MAX_PATH];
    char *slash;
    sh_modserve_plan p;
    sh_modserve_gates r;
    static const char *const A = "{\"cvars\": {\"r_fancy\": true, \"com_maxFps\": 120}, \"commands\": "
                                 "{\"deactivate\": [\"echo first off\"], \"activate\": [\"echo first on\", \" bind x \\\"say hi\\\" \"]}}";
    static const char *const B = "{\"cvars\": {\"com_maxFps\": \"60\"}, \"commands\": {\"activate\": [\"echo second on\"]}}";
    static const char *const C = "{\"cvars\": {\"ro_locked\": \"1\"}}";
    static const char *const D = "{\"cvars\": {\"com_maxFps\": \"120.0\", \"g_label\": \"two words\"}}";
    static const char *const E = "{\"commands\": {\"deactivate\": [\"echo e off\"]}}";
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\reqgen\\mods", top);
    _snprintf_s(root2, sizeof root2, _TRUNCATE, "%s\\reqgen2\\mods", top);
    _snprintf_s(plain_root, sizeof plain_root, _TRUNCATE, "%s\\plain\\mods", top);
    mkdirs(root);
    mkdirs(root2);
    make_gen(root, "afirst00", 'a', A, 100);
    make_gen(root, "bsecond0", 'b', B, 200);
    make_gen(root, "clocked0", 'c', C, 300);
    make_gen(root, "dsame000", 'd', D, 400);
    make_gen(root, "elines00", 'e', E, 500);
    make_gen(root2, "clocked0", 'c', C, 300);
    make_gen(root2, "dsame000", 'd', D, 400);
    make_gen(root2, "elines00", 'e', E, 500);

    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 5);
    /* The later install asking another value loses, both named. */
    CHECK(log_has("MODS: requirements: conflict: com_maxFps: bsecond0 asks \"60\", afirst00 asks \"120\"; "
                  "bsecond0, the later install, loses"));
    CHECK(log_has("MODS: requirements: com_maxFps=120 for afirst00") && log_has("MODS: requirements: r_fancy=1 for afirst00"));
    CHECK(log_has("MODS: requirements: 2 activate and 1 deactivate command(s) for afirst00"));
    CHECK(log_has("MODS: requirements: com_maxFps=120.0 for dsame000") && !log_has("for bsecond0"));
    CHECK(sh_modserve_requirements() == 4);

    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == -1);
    CHECK(r.needed == 4 && r.live == 3 && r.not_live == 1 && r.commands == 2);
    CHECK(!strcmp(g_text_val[0], "120") && !strcmp(g_text_val[1], "1") && !strcmp(g_text_val[3], "two words") &&
          !strcmp(g_text_val[2], "0"));
    /* Activate lines run after the cvars, in order, trimmed, once. */
    CHECK(ev_first("C:echo first on") > ev_last("S:") && ev_first("C:bind x \"say hi\";") > ev_first("C:echo first on"));
    CHECK(ev_first("C:echo second on") < 0 && ev_first("C:echo e off") < 0 && ev_first("C:echo first off") < 0);
    CHECK(log_has("MODS: requirements: afirst00: command \"echo first on\""));
    CHECK(log_has("MODS: requirements: afirst00: command \"bind x \"say hi\"\""));
    CHECK(log_has("MODS: editor: requirement ro_locked=1 (clocked0) is not live: the engine marks it read-only"));
    /* Only the bundles whose requirements are not live are blocked. */
    sh_modserve_plan_editor(&p);
    CHECK(p.count == 5);
    log_clear();
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 3);
    CHECK(log_has("(bsecond0) BLOCKED: requirement com_maxFps=60 lost to afirst00, installed earlier, which asks \"120\""));
    CHECK(log_has("(clocked0) BLOCKED: requirement ro_locked=1 is not live"));
    CHECK(ev_first("R:snapEditorEntityDef:mods/afirst00/thing") >= 0 && ev_first("R:snapEditorEntityDef:mods/dsame000/thing") >= 0 &&
          ev_first("R:snapEditorEntityDef:mods/elines00/thing") >= 0);
    sh_modserve_plan_free(&p);
    /* Nothing runs twice. */
    CHECK(g_gate_lines == 2 && !log_has("command \""));
    g_report[0] = 0;
    CHECK(sh_modserve_requirements_report(report_line, NULL) >= 8);
    CHECK(strstr(g_report, "requirements: afirst00: cvar com_maxFps=120: live\n") != NULL);
    CHECK(strstr(g_report, "requirements: dsame000: cvar g_label=two words: live\n") != NULL);
    CHECK(strstr(g_report, "requirements: afirst00: activate command \"echo first on\": ran\n") != NULL);
    CHECK(strstr(g_report, "requirements: afirst00: deactivate command \"echo first off\": runs when the mod stops\n") != NULL);
    CHECK(strstr(g_report, "requirements: bsecond0: cvar com_maxFps=60: refused (com_maxFps=60 lost to afirst00") != NULL);
    CHECK(strstr(g_report, "requirements: bsecond0: activate command \"echo second on\": not run") != NULL);
    CHECK(strstr(g_report, "requirements: clocked0: cvar ro_locked=1: NOT live (the engine marks it read-only; its steps are blocked)\n") != NULL);
    CHECK(strstr(g_report, "requirements: elines00: deactivate command \"echo e off\": runs when the mod stops\n") != NULL);

    /* afirst00 and bsecond0 go: afirst00's deactivate line runs, r_fancy is
     * restored, com_maxFps stays (dsame000 asks the same number). */
    CHECK(sh_modserve_index_build(root2) == 3);
    log_clear();
    g_events[0] = 0;
    (void)sh_modserve_requirements_apply(&g_gates, "rescan", &r);
    CHECK(ev_first("C:echo first off") >= 0 && ev_first("C:echo second on") < 0 && r.commands == 1);
    CHECK(log_has("MODS: rescan: requirements of afirst00 stopped; running its deactivate commands"));
    CHECK(!strcmp(g_text_val[1], "0") && log_has("requirement released: r_fancy restored to 0"));
    CHECK(!strcmp(g_text_val[0], "120") && r.restored == 1);

    /* Nothing installed asks anything: com_maxFps and g_label return to what
     * they displaced, elines00's line runs, and nothing is held. */
    CHECK(sh_modserve_index_build(plain_root) == 1);
    g_events[0] = 0;
    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0);
    CHECK(!strcmp(g_text_val[0], "200") && !strcmp(g_text_val[3], "") && ev_first("C:echo e off") >= 0);
    CHECK(sh_modserve_test_gates_owned() == 0 && sh_modserve_requirements_held() == 0);
    g_report[0] = 0;
    CHECK(sh_modserve_requirements_report(report_line, NULL) == 1 && strstr(g_report, "requirements: none"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* ------------------------------------------------------------------ conflicts (lane MG2) */

char *sh_modserve_test_compose(const char *const *texts, int n, int *added);

#define CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define WHITELIST "generated/decls/snappropertyinspector_whitelistencounterdecl/single_encounters_white_list_inspector.decl"
#define CYB_AW    "generated/decls/animweb/zion/boss/cyberdem_ai.decl"
#define NEEDY     "generated/decls/aipaingraph/cyberdem_ai.decl"

#define CONDUCTOR_HEAD \
    "{\r\n\tinherit = \"ai/base/conductor\";\r\n\tclass = \"idAIConductor_Coop\";\r\n" \
    "\teditorVars {\r\n\t\tcolor = ( 1, 1, 1, 1 );\r\n\t}\r\n\tedit = {\r\n\t\taiTypeList = {\r\n"
#define CONDUCTOR_STOCK_ITEMS \
    "\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n" \
    "\t\t\titem[1] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_ARCHVILE\";\r\n\t\t\t\tentityDef = \"ai/demon/archvile_coop\";\r\n\t\t\t}\r\n"
#define CONDUCTOR_TAIL "\t\t}\r\n\t\tglobalAIMods = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}"
#define ITEM(n, type, def) "\t\t\titem[" #n "] = {\r\n\t\t\t\taiType = \"" type "\";\r\n\t\t\t\tentityDef = \"" def "\";\r\n\t\t\t}\r\n"

#define CONDUCTOR_CYBER CONDUCTOR_HEAD "\t\t\tnum = 3;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "524288", "ai/demon/cyberdem_hell") CONDUCTOR_TAIL
#define CONDUCTOR_GUARD CONDUCTOR_HEAD "\t\t\tnum = 4;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "2097152", "ai/demon/talismanguard/staff") ITEM(3, "4194304", "ai/demon/talismanguard/hammer") CONDUCTOR_TAIL
#define CONDUCTOR_SPIDER CONDUCTOR_HEAD "\t\t\tnum = 3;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "1048576", "ai/demon/spider_mastermind_base") CONDUCTOR_TAIL

#define WL_A "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 3;\n\t\t\titem[0] = \"single_archvile\";\n" \
    "\t\t\titem[1] = \"single_zombie\";\n\t\t\titem[2] = \"single_cyberdemon\";\n\t\t}\n\t}\n}\n"
#define WL_B "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 3;\n\t\t\titem[0] = \"single_archvile\";\n" \
    "\t\t\titem[1] = \"single_zombie\";\n\t\t\titem[2] = \"single_spider_mastermind\";\n\t\t}\n\t}\n}\n"

/* animWeb-style text: outside the composable grammar (no '='). */
#define MD6_HEAD "{\n\tinit {\n\t\tinherit \"player/tp_body\"\n\t}\n\tevents {\n"
#define MD6_STOCK "\t\tanim \"md6/stock/walk.md6anim\" {\n\t\t\tevent \"ae_footstep\" {\n\t\t\t\tframe 4\n\t\t\t\trow 0\n\t\t\t\tlocked 0\n\t\t\t}\n\t\t}\n"
#define MD6_TAIL(extra) "\t}\n\taliases {\n" extra "\t}\n\tuserChannelWeightGroupOverride {\n\t\t\"MD6_WEIGHTGROUP_ALL\"\n\t}\n}\n"
#define AW_TEXT(extra) "{\n\tprops {\n\t\tgridSize 32\n\t\tmodelInfos {\n\t\t\t\"zion/characters/monsters/cyberdem/base/cyberdemon.md6\" \"\"\n\t\t}\n\t}\n" extra "}\n"

/* The stock module_lift_door md6Def (bare statement grammar; its meshKits pair
 * `"doors" = ""` has no terminator), split where its events block closes. */
#define LIFT_HEAD \
    "{\r\n" \
    "\tinit {\r\n" \
    "\t\tmesh \"md6/objects/props_set/doors/module_lift_door/assets/mesh/module_lift_door.md6mesh\"\r\n" \
    "\t\toffset ( 0 0 0 )\r\n" \
    "\t\tcalcRefBoundsFromJoints 0\r\n" \
    "\t}\r\n" \
    "\tuserProps {\r\n" \
    "\t}\r\n" \
    "\tjointGroups {\r\n" \
    "\t}\r\n" \
    "\tevents {\r\n" \
    "\t\tanim \"md6/cineractive/maps/lazarus_labs/cyberdrome/cyber_intro/cyberdem_intro_door.md6anim\" {\r\n" \
    "\t\t\tevent \"ae_startFX\" {\r\n" \
    "\t\t\t\tframe 26\r\n" \
    "\t\t\t\trow 0\r\n" \
    "\t\t\t\tlocked 0\r\n" \
    "\t\t\t\tfxCondition_t FX_CINEMATIC_1\r\n" \
    "\t\t\t}\r\n" \
    "\t\t\tevent \"ae_startFX\" {\r\n" \
    "\t\t\t\tframe 158\r\n" \
    "\t\t\t\trow 1\r\n" \
    "\t\t\t\tlocked 0\r\n" \
    "\t\t\t\tfxCondition_t FX_CINEMATIC_2\r\n" \
    "\t\t\t}\r\n" \
    "\t\t}\r\n"
#define LIFT_TAIL \
    "\t}\r\n" \
    "\taliases {\r\n" \
    "\t\talias {\r\n" \
    "\t\t\tname \"open_ico/closed\"\r\n" \
    "\t\t\tanim \"md6/objects/props_set/doors/module_lift_door/motion/closed.md6anim\"\r\n" \
    "\t\t}\r\n" \
    "\t\talias {\r\n" \
    "\t\t\tname \"open_ico/open\"\r\n" \
    "\t\t\tanim \"md6/objects/props_set/doors/module_lift_door/motion/open.md6anim\"\r\n" \
    "\t\t}\r\n" \
    "\t\talias {\r\n" \
    "\t\t\tname \"open_ico/open_into\"\r\n" \
    "\t\t\tanim \"md6/objects/props_set/doors/module_lift_door/motion/open_into.md6anim\"\r\n" \
    "\t\t}\r\n" \
    "\t\talias {\r\n" \
    "\t\t\tname \"open_ico/open_out\"\r\n" \
    "\t\t\tanim \"md6/objects/props_set/doors/module_lift_door/motion/open_out.md6anim\"\r\n" \
    "\t\t}\r\n" \
    "\t\talias {\r\n" \
    "\t\t\tname \"cyber_intro/cyberdem_intro_door\"\r\n" \
    "\t\t\tanim \"md6/cineractive/maps/lazarus_labs/cyberdrome/cyber_intro/cyberdem_intro_door.md6anim\"\r\n" \
    "\t\t}\r\n" \
    "\t}\r\n" \
    "\tprops {\r\n" \
    "\t\tprop \"_info\" {\r\n" \
    "\t\t\ttag \"fx_panel\" {\r\n" \
    "\t\t\t\ttrans ( 13.5252 7.7636 76.9484 )\r\n" \
    "\t\t\t\trot ( 0 0 0 1 )\r\n" \
    "\t\t\t\tparent \"base\"\r\n" \
    "\t\t\t}\r\n" \
    "\t\t\ttag \"fx_door_upper\" {\r\n" \
    "\t\t\t\ttrans ( 1 0 100 )\r\n" \
    "\t\t\t\trot ( 0 0 0 1 )\r\n" \
    "\t\t\t\tparent \"origin\"\r\n" \
    "\t\t\t}\r\n" \
    "\t\t}\r\n" \
    "\t}\r\n" \
    "\teyeInfoCollection 0 {\r\n" \
    "\t}\r\n" \
    "\tmeshKits {\r\n" \
    "\t\tHeads 0 {\r\n" \
    "\t\t}\r\n" \
    "\t\tGear 0 {\r\n" \
    "\t\t}\r\n" \
    "\t\tGore 0 {\r\n" \
    "\t\t}\r\n" \
    "\t\tDoor 1 {\r\n" \
    "\t\t\t\"doors\" = \"\"\r\n" \
    "\t\t}\r\n" \
    "\t\tMelee_Highlight 0 {\r\n" \
    "\t\t}\r\n" \
    "\t}\r\n" \
    "\tuserChannelWeightGroupOverride {\r\n" \
    "\t\t\"MD6_WEIGHTGROUP_ALL\"\r\n" \
    "\t}\r\n" \
    "}"

static int cf_source(void *c, const char *type, const char *name)
{
    (void)c;
    if (!_stricmp(type, "animWeb") && !_stricmp(name, "player/fp_hands_mp")) return 1;
    if (!_stricmp(type, "animWeb") && !_stricmp(name, "player/fp_hands_sp")) return 1;
    if (!_stricmp(type, "md6Def") && !_stricmp(name, "zion/stock/known.md6")) return 1;
    return 0;
}
/* The game archives: the player hands' md6Def is shipped although the
 * registry has not registered its source yet (as live at map activation). */
static int cf_stock(void *c, const char *request)
{
    (void)c;
    return !strcmp(request, "generated/decls/md6def/zion/player/human/base/mp.md6.decl") ? 1 : 0;
}
static const sh_modserve_engine_ops g_cf = { NULL, cf_source, fake_register, fake_load, cf_stock };

/* ------------------------------------------------------------------ gated raw resources */

#define GT_MODEL "generated/basemodel/md6/mods/gtraw000/body.bmd6model"
#define GT_IMAGE "generated/image/mods/gtraw000/skin.bimage"
#define GT_ANIM  "cooked/anim/md6/mods/gtraw000/idle.bmd6anim"
#define GT_LATE  "generated/image/mods/gtraw000/late.bimage"

static int raw_visit(void *context, const char *type, const char *name, const char *request, const char *bundle_id)
{
    char *seen = (char *)context;
    size_t at = strlen(seen);
    (void)request;
    _snprintf_s(seen + at, 1024 - at, _TRUNCATE, "%s:%s:%s;", type, name, bundle_id);
    return 1;
}

/* A model or image a gated bundle serves before its gates are live is
 * offered once the gates are live, once; one first served after the gates
 * were live, and other kinds, are never offered. */
static void test_ungated_raw(void)
{
    static const fx_file gt[] = {
        { GT_MODEL, "MODEL", -1, "baseModel", "md6/mods/gtraw000/body.md6mesh", NULL, '1' },
        { GT_IMAGE, "IMAGE", -1, "image", "mods/gtraw000/skin", NULL, '2' },
        { GT_ANIM, "ANIM", -1, "anim", "md6/mods/gtraw000/idle.md6anim", NULL, '3' },
        { GT_LATE, "LATE", -1, "image", "mods/gtraw000/late", NULL, '4' },
    };
    char top[MAX_PATH], root[MAX_PATH], text[64], seen[1024];
    char *slash;
    sh_modserve_gates r;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\gated\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": \"0\"}},\n  ";
    make_bundle(root, "gtraw000", "gtraw000", '6', NULL, gt, 4, NULL);
    g_bundle_extra = NULL;
    CHECK(sh_modserve_index_build(root) == 1);
    /* The gates cannot be set: not live; the boot-time opens are ungated. */
    gates_reset(1, 1, 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == -1 && r.live == 0);
    CHECK(open_text(GT_MODEL, text, sizeof text) == 1 && open_text(GT_IMAGE, text, sizeof text) == 1 &&
          open_text(GT_ANIM, text, sizeof text) == 1);
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    /* Live now: the model and the image are offered, the anim is not. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 1 && r.live == 2);
    CHECK(open_text(GT_LATE, text, sizeof text) == 1);
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 2);
    CHECK(strstr(seen, "baseModel:md6/mods/gtraw000/body.md6mesh:gtraw000;") && strstr(seen, "image:mods/gtraw000/skin:gtraw000;"));
    CHECK(!strstr(seen, "idle") && !strstr(seen, "late"));
    /* Settled: not offered again, also after a rescan of the same bytes. */
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    CHECK(sh_modserve_index_rescan() == 1);
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    CHECK(sh_modserve_each_ungated_raw(NULL, NULL) == 0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    /* Leave the table test_requirements left. */
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* A surface model the game loaded at boot with the blacklists on (the stock
 * spider mesh, ADD-V7a D1): the gated reload offers it once the bundle's
 * requirements are live, and that reload, outside any watermark, runs inside
 * its own keeper. The surface material the mesh names is still in no plan
 * and no early-decl list (ADD-L11 holds for decls). */
#define GS_MESH "generated/basemodel/md6/characters/monsters/gs/body.bmd6model"
#define GS_MAT  "generated/decls/material/models/monsters/gs/body.decl"
#define GS_ENT  "generated/decls/entitydef/mods/gsmod000/thing.decl"
#define GL_MESH "generated/basemodel/md6/characters/monsters/gl/body.bmd6model"
#define GS_MESH_BYTES "stock gs mesh"
#define GS_MAT_BYTES "{ diffusemap models/monsters/gs/body }"
static int gs_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    const char *body;
    (void)c; (void)sha;
    if (!_stricmp(source, GS_MESH) || !_stricmp(source, GL_MESH)) body = GS_MESH_BYTES;
    else if (!_stricmp(source, GS_MAT)) body = GS_MAT_BYTES;
    else return SH_MB_STOCK_ABSENT;
    *len = strlen(body);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len);
    memcpy(*bytes, body, *len);
    return SH_MB_STOCK_OK;
}
static void test_surface_gated_reload(void)
{
    static const fx_file gs[] = {
        { GS_MESH, GS_MESH_BYTES, -1, "baseModel", "md6/characters/monsters/gs/body.md6mesh",
          "[{\"type\":\"material\",\"name\":\"models/monsters/gs/body\"}]", '1' },
        { GS_MAT, GS_MAT_BYTES, -1, "material", "models/monsters/gs/body", NULL, '2' },
        { GS_ENT, "{ thing }", -1, "entityDef", "mods/gsmod000/thing",
          "[{\"type\":\"material\",\"name\":\"models/monsters/gs/body\"}]", '3' },
    };
    char top[MAX_PATH], root[MAX_PATH], text[64], seen[1024], h[65];
    const char *hashes[1];
    char *slash;
    int n;
    sh_modserve_gates r;
    sh_modserve_plan map, ed;
    sh_mb_stock_ops ops;
    printf("-- a surface model loaded under the blacklists is reloaded, inside a keeper; its surface decl is in no plan\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\gated-surface\\mods", top);
    mkdirs(root);
    memset(&ops, 0, sizeof ops);
    ops.read = gs_read;
    sh_mb_set_stock_ops(&ops);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": \"0\"}},\n  ";
    g_fx_restore = GS_MESH;
    g_fx_restore2 = GS_MAT;
    make_bundle(root, "gsmod000", "gsmod000", 'c', NULL, gs, 3, NULL);
    g_fx_restore = g_fx_restore2 = NULL;
    g_bundle_extra = NULL;
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_is_surface(GS_MESH) == 1 && sh_modserve_is_surface(GS_MAT) == 1 && sh_modserve_is_surface(GS_ENT) == 0);
    /* Boot: the gates are on and cannot be set yet; the game loads the mesh. */
    gates_reset(1, 1, 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == -1 && r.live == 0);
    CHECK(open_text(GS_MESH, text, sizeof text) == 1 && !strcmp(text, GS_MESH_BYTES));
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    /* The map load turns the gates off: the held surface mesh is offered. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements_apply(&g_gates, "map load", &r) == 1 && r.live == 2);
    seen[0] = 0;
    n = sh_modserve_each_ungated_raw(raw_visit, seen);
    CHECK(n == 1 && strstr(seen, "baseModel:md6/characters/monsters/gs/body.md6mesh:gsmod000;") != NULL);
    if (n != 1) printf("  gated reload list (%d): %s\n", n, seen);
    /* That reload runs outside any watermark (the map load's scope apply):
     * it opens its own keeper; inside the editor pass that pass's keeps it. */
    CHECK(sh_modserve_inplace_keeper(0, n) == 1 && sh_modserve_inplace_keeper(1, n) == 0);
    /* Settled: not offered again. */
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    /* The surface material decl (and the mesh) are in no plan; no early-decl re-parse. */
    hashes[0] = hash64('c', h);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &map, NULL) >= 1);
    CHECK(step_index(&map, "material", "models/monsters/gs/body") < 0 && step_index(&map, "entityDef", "mods/gsmod000/thing") >= 0);
    CHECK(step_index(&map, "baseModel", "md6/characters/monsters/gs/body.md6mesh") < 0);
    g_calls[0] = 0;
    g_already = NULL;
    CHECK(sh_modserve_plan_run(&map, &g_fake, "map") == map.count);
    CHECK(!strstr(g_calls, "models/monsters/gs/body"));
    sh_modserve_plan_free(&map);
    CHECK(sh_modserve_plan_editor(&ed) >= 0 && step_index(&ed, "material", "models/monsters/gs/body") < 0);
    sh_modserve_plan_free(&ed);
    seen[0] = 0;
    (void)sh_modserve_each_unopened_shadow(raw_visit, seen);
    CHECK(!strstr(seen, "models/monsters/gs/body"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_mb_set_stock_ops(NULL);
    sh_modserve_test_reset_registration();
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* A boot that builds the mod folders (ADD-V7b D1): the game loads the stock
 * surface mesh while the index is still empty, so the open never reaches the
 * provider and the mesh is never marked ungated. The index built after it
 * offers the never-opened surface once the gates are live, once, and not
 * again after a rescan. Its own mesh path: the earlier tables stay chained
 * (prev), and a mesh an earlier table settled counts as settled. */
static void test_surface_built_after_boot(void)
{
    static const fx_file gs[] = {
        { GL_MESH, GS_MESH_BYTES, -1, "baseModel", "md6/characters/monsters/gl/body.md6mesh",
          "[{\"type\":\"material\",\"name\":\"models/monsters/gs/body\"}]", '1' },
        { GS_MAT, GS_MAT_BYTES, -1, "material", "models/monsters/gs/body", NULL, '2' },
        { GS_ENT, "{ thing }", -1, "entityDef", "mods/gsmod000/thing",
          "[{\"type\":\"material\",\"name\":\"models/monsters/gs/body\"}]", '3' },
    };
    char top[MAX_PATH], root[MAX_PATH], empty[MAX_PATH], seen[1024];
    char *slash;
    int n;
    sh_modserve_gates r;
    sh_mb_stock_ops ops;
    printf("-- a surface mesh the game loaded before the index served it is reloaded once the gates are live\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\gated-surface-late\\mods", top);
    _snprintf_s(empty, sizeof empty, _TRUNCATE, "%s\\gated-surface-empty\\mods", top);
    mkdirs(root);
    mkdirs(empty);
    memset(&ops, 0, sizeof ops);
    ops.read = gs_read;
    sh_mb_set_stock_ops(&ops);
    /* Boot: the folders are not built yet; the index is empty while the game
     * loads the mesh, so nothing is served (or marked) for it. */
    gates_reset(1, 1, 0);
    CHECK(sh_modserve_index_build(empty) == 0);
    CHECK(sh_modserve_is_surface(GL_MESH) == 0);
    /* The boot build finishes and the index is rebuilt with the bundle. */
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": \"0\"}},\n  ";
    g_fx_restore = GL_MESH;
    g_fx_restore2 = GS_MAT;
    make_bundle(root, "gsmod000", "gsmod000", 'c', NULL, gs, 3, NULL);
    g_fx_restore = g_fx_restore2 = NULL;
    g_bundle_extra = NULL;
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_is_surface(GL_MESH) == 1);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == -1 && r.live == 0);
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    /* The map load turns the gates off: the never-opened mesh is offered. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements_apply(&g_gates, "map load", &r) == 1 && r.live == 2);
    seen[0] = 0;
    n = sh_modserve_each_ungated_raw(raw_visit, seen);
    CHECK(n == 1 && strstr(seen, "baseModel:md6/characters/monsters/gl/body.md6mesh:gsmod000;") != NULL);
    if (n != 1) printf("  gated reload list (%d): %s\n", n, seen);
    /* Repaired: not offered again, also after a rescan of the same bytes. */
    seen[0] = 0;
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    CHECK(sh_modserve_index_rescan() == 1);
    CHECK(sh_modserve_each_ungated_raw(raw_visit, seen) == 0 && !seen[0]);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_mb_set_stock_ops(NULL);
    sh_modserve_test_reset_registration();
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* ------------------------------------------------------------------ refresh */

#define RF_SH      "generated/decls/entitydef/monster/zombie.decl"
#define RF_SH_GONE "generated/decls/entitydef/monster/gonezombie.decl"
#define RF_KEEP    "generated/decls/entitydef/mods/rfmod000/keep.decl"
#define RF_NAME    "generated/decls/entitydef/mods/rfmod000/name.decl"
#define RF_OLD     "generated/decls/entitydef/mods/rfmod000/oldname.decl"
#define RF_MODEL   "generated/basemodel/md6/mods/rfmod000/body.bmd6model"
#define RF_IMAGE   "generated/image/mods/rfmod000/skin.bimage"
#define RF_ANIM    "cooked/anim/md6/mods/rfmod000/idle.bmd6anim"
#define RF_NEW     "generated/decls/entitydef/mods/rfmod000/new.decl"
#define RF_ROW     "generated/decls/snapeditorentitydef/mods/rfmod000/new.decl"
#define RF_CHILD   "generated/decls/entitydef/mods/rfmod000/child.decl"
#define RF_GRAND   "generated/decls/entitydef/mods/rfmod000/grandchild.decl"
#define RF_ORPHAN  "generated/decls/entitydef/mods/rfmod000/orphan.decl"
#define RF_DECOY   "generated/decls/entitydef/mods/rfmod000/decoy.decl"
#define RF_OLDROW  "generated/decls/snapeditorentitydef/mods/rfmod000/oldrow.decl"
#define RF_SHROW   "generated/decls/snapeditorentitydef/func/shadowrow.decl"   /* a stock editor row the mod shadows */
#define RF_INHERIT_CHILD  "{\n\tinherit = \"mods/rfmod000/name\";\n}\n"
#define RF_INHERIT_GRAND  "{\n\t// a parent\n\tinherit = \"mods/rfmod000/child\";\n}\n"
#define RF_INHERIT_ORPHAN "{\n\tinherit=\"monster/gonezombie\";\n}\n"
#define RF_INHERIT_DECOY  "{\n\t// inherit = \"mods/rfmod000/name\"\n\tedit = { inherit = \"mods/rfmod000/name\"; }\n}\n"

typedef struct rf_seen { char order[2048]; int keep_raw; int visits; } rf_seen;
static int rf_visit(void *context, const sh_modserve_refresh_item *it)
{
    rf_seen *s = (rf_seen *)context;
    size_t at = strlen(s->order);
    s->visits++;
    _snprintf_s(s->order + at, sizeof s->order - at, _TRUNCATE, "%d:%s:%s:%s%s;", it->kind, it->type, it->name,
                it->gone ? "gone" : "now", it->gates_live ? "" : ":gated");
    return it->kind == SH_MODSERVE_RF_DECL || !s->keep_raw;
}

/* A rescan that changed a bundle's files: the diff against the replaced
 * table classifies changed, new and gone requests; the refresh visit offers
 * exactly the changed decls, models and images and the gone shadows, in plan
 * rank order with gone shadows after the decls and raw files last; marks
 * carry over a rescan of the same bytes; a settled decl is no early decl. */
static void test_refresh(void)
{
    static const fx_file v1[] = {
        { RF_SH, "entityDef z1 {}", -1, "entityDef", "monster/zombie", NULL, '1' },
        { RF_SH_GONE, "entityDef g1 {}", -1, "entityDef", "monster/gonezombie", NULL, '3' },
        { RF_KEEP, "entityDef keep {}", -1, "entityDef", "mods/rfmod000/keep", NULL, '4' },
        { RF_NAME, "entityDef n1 {}", -1, "entityDef", "mods/rfmod000/name", NULL, '5' },
        { RF_OLD, "entityDef old {}", -1, "entityDef", "mods/rfmod000/oldname", NULL, '7' },
        { RF_MODEL, "MODEL1", -1, "baseModel", "md6/mods/rfmod000/body.md6mesh", NULL, '8' },
        { RF_IMAGE, "IMAGE", -1, "image", "mods/rfmod000/skin", NULL, 'a' },
        { RF_ANIM, "ANIM1", -1, "anim", "md6/mods/rfmod000/idle.md6anim", NULL, 'b' },
        { RF_CHILD, RF_INHERIT_CHILD, -1, "entityDef", "mods/rfmod000/child", NULL, 'f' },
        { RF_GRAND, RF_INHERIT_GRAND, -1, "entityDef", "mods/rfmod000/grandchild", NULL, '0' },
        { RF_ORPHAN, RF_INHERIT_ORPHAN, -1, "entityDef", "mods/rfmod000/orphan", NULL, '1' },
        { RF_DECOY, RF_INHERIT_DECOY, -1, "entityDef", "mods/rfmod000/decoy", NULL, '2' },
        { RF_OLDROW, "{ entityDef \"mods/rfmod000/keep\" }", -1, "snapEditorEntityDef", "mods/rfmod000/oldrow",
          "[{\"type\":\"entityDef\",\"name\":\"mods/rfmod000/keep\"}]", '3' },
        { RF_SHROW, "{ entityDef \"monster/zombie\" }", -1, "snapEditorEntityDef", "func/shadowrow",
          "[{\"type\":\"entityDef\",\"name\":\"monster/zombie\"}]", 'g' },
    };
    static const fx_file v2[] = {
        { RF_SH, "entityDef z2 {}", -1, "entityDef", "monster/zombie", NULL, '2' },
        { RF_KEEP, "entityDef keep {}", -1, "entityDef", "mods/rfmod000/keep", NULL, '4' },
        { RF_NAME, "entityDef n2 {}", -1, "entityDef", "mods/rfmod000/name", NULL, '6' },
        { RF_MODEL, "MODEL2", -1, "baseModel", "md6/mods/rfmod000/body.md6mesh", NULL, '9' },
        { RF_IMAGE, "IMAGE", -1, "image", "mods/rfmod000/skin", NULL, 'a' },
        { RF_ANIM, "ANIM2", -1, "anim", "md6/mods/rfmod000/idle.md6anim", NULL, 'c' },
        { RF_NEW, "entityDef new {}", -1, "entityDef", "mods/rfmod000/new", NULL, 'd' },
        { RF_ROW, "{ entityDef \"mods/rfmod000/new\" }", -1, "snapEditorEntityDef", "mods/rfmod000/new",
          "[{\"type\":\"entityDef\",\"name\":\"mods/rfmod000/new\"}]", 'e' },
        { RF_CHILD, RF_INHERIT_CHILD, -1, "entityDef", "mods/rfmod000/child", NULL, 'f' },
        { RF_GRAND, RF_INHERIT_GRAND, -1, "entityDef", "mods/rfmod000/grandchild", NULL, '0' },
        { RF_ORPHAN, RF_INHERIT_ORPHAN, -1, "entityDef", "mods/rfmod000/orphan", NULL, '1' },
        { RF_DECOY, RF_INHERIT_DECOY, -1, "entityDef", "mods/rfmod000/decoy", NULL, '2' },
    };
    char top[MAX_PATH], root[MAX_PATH], text[64];
    char *slash;
    sh_modserve_diff d;
    rf_seen s;
    unopened_seen u;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\refresh\\mods", top);
    mkdirs(root);
    make_bundle(root, "rfmod000", "rfmod000", '1', "\"" RF_SH "\", \"" RF_SH_GONE "\", \"" RF_SHROW "\"", v1, 14, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    /* The first table of this root replaces another root's: every file is
     * new. Shadows the earlier tables served and no table serves since are
     * still pending as gone (the engine holds their bundles' text), and the
     * new shadows are pending too (the engine may hold the game's text for
     * them); new names of the mod are not. Settle them. */
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 0 && d.added == 14 && d.changed_decls == 3 && d.pending > 0);
    /* The mod's own editor row is added; the shadow of a stock row counts as a changed row. */
    CHECK(d.added_rows == 1 && d.changed_rows == 1);
    memset(&s, 0, sizeof s);
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == d.pending);
    CHECK(strstr(s.order, "1:entityDef:monster/imp:gone;") && strstr(s.order, "1:entityDef:monster/zombie:now;") &&
          strstr(s.order, "1:entityDef:monster/gonezombie:now;") && !strstr(s.order, "rfmod000"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* The engine read some of v1 through the provider. */
    CHECK(open_text(RF_SH, text, sizeof text) == 1 && open_text(RF_NAME, text, sizeof text) == 1 &&
          open_text(RF_MODEL, text, sizeof text) == 1);
    /* A map used the mod: its gameplay decls are registered this session. */
    {
        sh_modserve_plan p;
        const char *h[1];
        char hash[65];
        h[0] = hash64('1', hash);
        CHECK(sh_modserve_plan_bundles(h, 1, &p, NULL) > 0);
        CHECK(sh_modserve_plan_run(&p, &g_fake, "map") > 0);
        sh_modserve_plan_free(&p);
        CHECK(sh_modserve_plan_refresh(&p) == 0 && p.count == 0);
        sh_modserve_plan_free(&p);
    }

    /* The author edits the folder; the rebuild rule writes v2; the rescan publishes it. */
    make_bundle(root, "rfmod000", "rfmod000", '2', "\"" RF_SH "\"", v2, 12, NULL);
    log_clear();
    CHECK(sh_modserve_index_rescan() == 1);
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 4 && d.changed_decls == 2 && d.changed_models == 1 && d.changed_images == 0);
    /* A changed anim is reloaded in place at the next map pass (lane L2c), not a restart. */
    CHECK(d.changed_anims == 1 && d.changed_restart == 0 && d.changed_map == 0);
    CHECK(d.added == 2 && d.added_decls == 2 && d.added_rows == 1 && d.added_active == 1 && d.changed_rows == 0);
    /* The shadowed stock editor row that went counts as a gone row too (the
     * palette must be built again without the mod's version of it). */
    CHECK(d.gone == 4 && d.gone_shadows == 2 && d.gone_names == 1 && d.gone_rows == 2);
    CHECK(d.pending == 7 && d.dependents == 0);
    /* The new gameplay decl of the mod in use is planned once; the new row is the palette's. */
    {
        sh_modserve_plan p;
        CHECK(sh_modserve_plan_refresh(&p) == 1 && p.count == 1 && !strcmp(p.steps[0].name, "mods/rfmod000/new"));
        sh_modserve_plan_free(&p);
        CHECK(sh_modserve_plan_refresh(&p) == 0);
        sh_modserve_plan_free(&p);
    }
    /* The decls that inherit from a changed or gone decl follow it, at any
     * depth; a commented or nested inherit is no parent. Once per table. */
    CHECK(sh_modserve_refresh_expand() == 3);
    CHECK(sh_modserve_refresh_expand() == 0);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 10 && d.dependents == 3);
    /* Exactly the changed decls, the gone shadow, the inheriting decls by
     * generation and the changed model and anim, in that order. */
    memset(&s, 0, sizeof s);
    s.keep_raw = 1;
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == 10);
    CHECK(strstr(s.order, "1:snapEditorEntityDef:mods/rfmod000/oldrow:gone;"));
    CHECK(strstr(s.order, "mods/rfmod000/child:now;") && strstr(s.order, "mods/rfmod000/orphan:now;") &&
          strstr(s.order, "mods/rfmod000/grandchild:now;") && !strstr(s.order, "decoy"));
    CHECK(strstr(s.order, "gonezombie:gone;") < strstr(s.order, "rfmod000/child:") &&
          strstr(s.order, "rfmod000/child:") < strstr(s.order, "rfmod000/grandchild:") &&
          strstr(s.order, "rfmod000/grandchild:") < strstr(s.order, "baseModel"));
    CHECK(strstr(s.order, "1:entityDef:monster/zombie:now;") && strstr(s.order, "1:entityDef:mods/rfmod000/name:now;"));
    CHECK(strstr(s.order, "1:entityDef:monster/gonezombie:gone;"));
    CHECK(strstr(s.order, "2:baseModel:md6/mods/rfmod000/body.md6mesh:now;"));
    CHECK(strstr(s.order, "4:anim:md6/mods/rfmod000/idle.md6anim:now;") &&
          strstr(s.order, "rfmod000/grandchild:") < strstr(s.order, "4:anim:"));
    CHECK(strstr(s.order, "mods/rfmod000/name:now;") < strstr(s.order, "gonezombie") &&
          strstr(s.order, "gonezombie") < strstr(s.order, "baseModel"));
    CHECK(!strstr(s.order, "keep") && !strstr(s.order, "skin") &&
          !strstr(s.order, "rfmod000/new") && !strstr(s.order, "oldname") && !strstr(s.order, ":gated"));
    /* The decls are settled and no early decl; the model and the anim are still pending. */
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 2);
    memset(&u, 0, sizeof u);
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    CHECK(!strstr(u.order, "monster/zombie"));
    /* A rescan of the same bytes changes nothing and keeps the pending model and anim. */
    CHECK(sh_modserve_index_rescan() == 1);
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 0 && d.added == 0 && d.gone == 0 && d.pending == 2);
    memset(&s, 0, sizeof s);
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == 2 && strstr(s.order, "baseModel") && strstr(s.order, "4:anim:"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    memset(&s, 0, sizeof s);
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == 0);
    CHECK(sh_modserve_each_refresh(NULL, NULL) == 0);
    /* Leave the table test_requirements left. */
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

static void test_compose_unit(void)
{
    const char *t[3];
    char *out;
    int added = -1;
    /* Three packs each appending to one list: the union, renumbered. */
    t[0] = CONDUCTOR_CYBER; t[1] = CONDUCTOR_GUARD; t[2] = CONDUCTOR_SPIDER;
    out = sh_modserve_test_compose(t, 3, &added);
    CHECK(out && added == 3);
    if (out) {
        CHECK(strstr(out, "aiTypeList = {\n\t\t\tnum = 6;\n") != NULL);
        CHECK(strstr(out, "item[2] = {\n\t\t\t\taiType = \"524288\";") != NULL);
        CHECK(strstr(out, "item[3] = {\n\t\t\t\taiType = \"2097152\";") != NULL);
        CHECK(strstr(out, "item[5] = {\n\t\t\t\taiType = \"1048576\";\n\t\t\t\tentityDef = \"ai/demon/spider_mastermind_base\";") != NULL);
        CHECK(strstr(out, "globalAIMods = {\n\t\t\tnum = 1;") != NULL);
        CHECK(strstr(out, "color = ( 1, 1, 1, 1 );") != NULL && strstr(out, "editorVars {\n") != NULL);
        CHECK(strstr(out, "inherit = \"ai/base/conductor\";") != NULL);
        /* The result composes again with an input unchanged (a fixed point). */
        {
            int again = -1;
            char *twice;
            t[0] = out; t[1] = CONDUCTOR_GUARD;
            twice = sh_modserve_test_compose(t, 2, &again);
            CHECK(twice && again == 0 && !strcmp(twice, out));
            free(twice);
        }
        free(out);
    }
    /* Scalar items match by value, wherever they sit. */
    t[0] = WL_A; t[1] = WL_B;
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 1 && strstr(out, "num = 4;") && strstr(out, "item[3] = \"single_spider_mastermind\";"));
    free(out);
    /* One entry with two meanings, a scalar that differs outside a list, a
     * member only one text has, and texts outside the grammar: no composition. */
    t[0] = CONDUCTOR_CYBER;
    t[1] = CONDUCTOR_HEAD "\t\t\tnum = 3;\r\n" CONDUCTOR_STOCK_ITEMS ITEM(2, "524288", "ai/demon/something_else") CONDUCTOR_TAIL;
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = "{\n\tinherit = \"a\";\n}\n"; t[1] = "{\n\tinherit = \"b\";\n}\n";
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = "{\n\tinherit = \"a\";\n}\n"; t[1] = "{\n\tinherit = \"a\";\n\tclass = \"x\";\n}\n";
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = AW_TEXT(""); t[1] = AW_TEXT("\tlayers {\n\t}\n");
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = "{ unterminated"; t[1] = WL_A;
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    /* Bare statement grammar: two mods each append event rows and aliases to
     * the player body md6Def; the union keeps the stock clip and both additions. */
    t[0] = MD6_HEAD MD6_STOCK "\t\tanim \"md6/a/pickup.md6anim\" {\n\t\t\tevent \"ae_giveInventoryItem\" {\n\t\t\t\tframe 10\n\t\t\t\trow 1\n\t\t\t\tlocked 0\n\t\t\t\tinventoryItem \"weapon/mp/shotgun\"\n\t\t\t}\n\t\t}\n" MD6_TAIL("");
    t[1] = MD6_HEAD MD6_STOCK "\t\tanim \"md6/mods/b/control.md6anim\" {\n\t\t\tevent \"ae_releasePlayerFromSync\" {\n\t\t\t\tframe 0\n\t\t\t\trow 0\n\t\t\t\tlocked 0\n\t\t\t}\n\t\t}\n" MD6_TAIL("\t\talias {\n\t\t\tname \"mods/b/control\"\n\t\t\tanim \"md6/mods/b/control.md6anim\"\n\t\t}\n");
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 2);
    if (out) {
        CHECK(strstr(out, "anim \"md6/stock/walk.md6anim\" {") != NULL);
        CHECK(strstr(out, "anim \"md6/a/pickup.md6anim\" {") != NULL);
        CHECK(strstr(out, "anim \"md6/mods/b/control.md6anim\" {") != NULL);
        CHECK(strstr(out, "inventoryItem \"weapon/mp/shotgun\"\n") != NULL);
        CHECK(strstr(out, "name \"mods/b/control\"\n") != NULL && strstr(out, "\"MD6_WEIGHTGROUP_ALL\"\n") != NULL);
        CHECK(strstr(out, "inherit \"player/tp_body\"\n") != NULL);
        CHECK(strstr(out, "frame 10\n") != NULL && strstr(out, " = ") == NULL);
        {
            int again = -1;
            char *twice;
            t[0] = out;
            twice = sh_modserve_test_compose(t, 2, &again);
            CHECK(twice && again == 0 && !strcmp(twice, out));
            free(twice);
        }
        free(out);
    }
    /* A winner whose aliases bag holds an entry composes with a text whose
     * aliases bag is empty (the stock text), as the reverse order already did. */
    t[0] = MD6_HEAD MD6_STOCK MD6_TAIL("\t\talias {\n\t\t\tname \"mods/b/control\"\n\t\t\tanim \"md6/mods/b/control.md6anim\"\n\t\t}\n");
    t[1] = MD6_HEAD MD6_STOCK MD6_TAIL("");
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 0 && strstr(out, "name \"mods/b/control\"\n") != NULL);
    free(out);
    /* Two new clips whose rows are identical (a door's kick and its reverse,
     * each hiding the HUD at frame 1) are two entries, matched by name, not one. */
    t[0] = MD6_HEAD MD6_STOCK MD6_TAIL("");
    t[1] = MD6_HEAD MD6_STOCK "\t\tanim \"md6/doors/kick_down_PLAYER.md6anim\" {\n\t\t\tevent \"ae_hideHudForContextPrompt\" {\n\t\t\t\tframe 1\n\t\t\t\trow 30\n\t\t\t\tlocked 0\n\t\t\t}\n\t\t}\n"
           "\t\tanim \"md6/doors/kick_down_reverse_PLAYER.md6anim\" {\n\t\t\tevent \"ae_hideHudForContextPrompt\" {\n\t\t\t\tframe 1\n\t\t\t\trow 30\n\t\t\t\tlocked 0\n\t\t\t}\n\t\t}\n" MD6_TAIL("");
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 2);
    if (out) {
        CHECK(strstr(out, "anim \"md6/doors/kick_down_PLAYER.md6anim\" {") != NULL);
        CHECK(strstr(out, "anim \"md6/doors/kick_down_reverse_PLAYER.md6anim\" {") != NULL);
        {
            int again = -1;
            char *twice;
            t[0] = out;
            twice = sh_modserve_test_compose(t, 2, &again);
            CHECK(twice && again == 0 && !strcmp(twice, out));
            free(twice);
        }
        free(out);
    }
    /* The same clip with a different row set is two meanings; a scalar that
     * differs outside a bag refuses; a bag kind the winner lacks refuses. */
    t[0] = MD6_HEAD MD6_STOCK "\t\tanim \"md6/a/pickup.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t\trow 1\n\t\t\t}\n\t\t}\n" MD6_TAIL("");
    t[1] = MD6_HEAD MD6_STOCK "\t\tanim \"md6/a/pickup.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 2\n\t\t\t\trow 1\n\t\t\t}\n\t\t}\n" MD6_TAIL("");
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = MD6_HEAD MD6_STOCK MD6_TAIL(""); t[1] = "{\n\tinit {\n\t\tinherit \"player/other\"\n\t}\n\tevents {\n" MD6_STOCK "\t}\n\taliases {\n\t}\n\tuserChannelWeightGroupOverride {\n\t\t\"MD6_WEIGHTGROUP_ALL\"\n\t}\n}\n";
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    t[0] = AW_TEXT(""); t[1] = AW_TEXT("\tlayers {\n\t}\n");
    CHECK(sh_modserve_test_compose(t, 2, &added) == NULL);
    /* Two copies of the stock lift door md6Def that differ only in an event
     * bag compose without adding a ';' anywhere: the engine's md6Def parser
     * refuses `"doors" = "";` (issue 160). */
    t[0] = LIFT_HEAD LIFT_TAIL;
    t[1] = LIFT_HEAD "\t\tanim \"md6/mods/f1/kick.md6anim\" {\n\t\t\tevent \"ae_startFX\" {\n\t\t\t\tframe 3\n\t\t\t\trow 0\n\t\t\t\tlocked 0\n\t\t\t}\n\t\t}\n" LIFT_TAIL;
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 1);
    if (out) {
        CHECK(strchr(out, ';') == NULL);
        CHECK(strstr(out, "\tDoor 1 {\n\t\t\t\"doors\" = \"\"\n\t\t}\n") != NULL);
        CHECK(strstr(out, "anim \"md6/mods/f1/kick.md6anim\" {") != NULL);
        CHECK(strstr(out, "fxCondition_t FX_CINEMATIC_2\n") != NULL);
        {
            int again = -1;
            char *twice;
            t[0] = out;
            twice = sh_modserve_test_compose(t, 2, &again);
            CHECK(twice && again == 0 && !strcmp(twice, out));
            free(twice);
        }
        free(out);
    }
    /* A provider that wrote the pair with a ';' (a converted package) is the
     * same text; an appended item takes the convention of the block it joins. */
    t[0] = "{\n\tmeshKits {\n\t\tDoor 1 {\n\t\t\t\"doors\" = \"\"\n\t\t}\n\t}\n}\n";
    t[1] = "{\n\tmeshKits {\n\t\tDoor 1 {\n\t\t\t\"doors\" = \"\";\n\t\t}\n\t}\n}\n";
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 0 && !strcmp(out, t[0]));
    free(out);
    t[0] = "{\n\tmeshKits {\n\t\tDoor 1 {\n\t\t\t\"doors\" = \"\"\n\t\t}\n\t\tGear 0 {\n\t\t}\n\t}\n}\n";
    t[1] = "{\n\tmeshKits {\n\t\tDoor 1 {\n\t\t\t\"doors\" = \"\";\n\t\t}\n\t\tGear 0 {\n\t\t}\n\t\tGear 1 {\n\t\t\t\"visor\" = \"\";\n\t\t}\n\t}\n}\n";
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 1 && strchr(out, ';') == NULL && strstr(out, "\"visor\" = \"\"\n") != NULL);
    free(out);
    /* The assignment grammar keeps its ';', and an item appended from a text
     * written without terminators takes the list's. */
    t[0] = WL_A;
    t[1] = "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 1\n\t\t\titem[0] = \"single_spider_mastermind\"\n\t\t}\n\t}\n}\n";
    out = sh_modserve_test_compose(t, 2, &added);
    CHECK(out && added == 1 && strstr(out, "num = 4;") && strstr(out, "item[3] = \"single_spider_mastermind\";\n"));
    free(out);
}

static void test_conflicts(void)
{
    char top[MAX_PATH], root[MAX_PATH], text[4096], aw_hash[65];
    char *slash;
    sh_modserve_conflict c;
    sh_modserve_hit hit;
    sh_modserve_plan p;
    sh_modserve_gates r;
    const char *hashes[1];
    int unknown = 0, ready;
    test_compose_unit();
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\conflicts\\mods", top);
    mkdirs(root);
    {
        /* The owner's tree, reduced: one authored boss mod at the root and
         * the build a map pinned in its place, with the same animWeb bytes,
         * another boss pack carrying it with one line different, and two more
         * bosses, all appending to the conductor and the whitelist. */
        static const fx_file cy[] = {
            { CYB_AW, AW_TEXT("\tcustomFlags AW_CF_IDLE_CYCLE\n"), -1, "animWeb", "zion/boss/cyberdem_ai", NULL, 'a' },
            { CONDUCTOR, CONDUCTOR_CYBER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'c' },
            { WHITELIST, WL_A, -1, "snapPropertyInspector_WhiteListEncounterDecl", "single_encounters_white_list_inspector", NULL, 'w' },
        };
        static const fx_file dl[] = {
            { CYB_AW, AW_TEXT(""), -1, "animWeb", "zion/boss/cyberdem_ai", NULL, 'b' },
            { CONDUCTOR, CONDUCTOR_CYBER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'c' },
        };
        static const fx_file gd[] = {
            { CONDUCTOR, CONDUCTOR_GUARD, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'd' },
        };
        static const fx_file sp[] = {
            { CONDUCTOR, CONDUCTOR_SPIDER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'e' },
            { WHITELIST, WL_B, -1, "snapPropertyInspector_WhiteListEncounterDecl", "single_encounters_white_list_inspector", NULL, 'f' },
        };
        make_bundle(root, "local cyber", "localcyb", '1',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\", \"" WHITELIST "\"", cy, 3, "{\"validated\": true, \"installedAt\": 10}");
        make_bundle(root, "maps\\Boss map\\localcyb", "localcyb", '2',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\", \"" WHITELIST "\"", cy, 3, "{\"validated\": true, \"installedAt\": 11}");
        make_bundle(root, "cyberdem", "cyberdem", '3',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\"", dl, 2, "{\"validated\": true, \"installedAt\": 99}");
        make_bundle(root, "local hell", "localhel", '4', "\"" CONDUCTOR "\"", gd, 1,
                    "{\"validated\": true, \"installedAt\": 12}");
        make_bundle(root, "localspi folder", "localspi", '5',
                    "\"" CONDUCTOR "\", \"" WHITELIST "\"", sp, 2, "{\"validated\": true, \"installedAt\": 13}");
    }
    {
        /* Needs decide: the later-installed copy names an md6Def nothing installed provides. */
        static const fx_file n1[] = {
            { NEEDY, "{ pain a }", -1, "aiPainGraph", "cyberdem_ai", "[{\"type\":\"md6Def\",\"name\":\"zion/nowhere.md6\"}]", '1' },
        };
        static const fx_file n2[] = {
            { NEEDY, "{ pain b }", -1, "aiPainGraph", "cyberdem_ai", "[{\"type\":\"md6Def\",\"name\":\"zion/here.md6\"}]", '2' },
            { "generated/decls/md6def/zion/here.md6.decl", "{ md6 }", -1, "md6Def", "zion/here.md6", NULL, '3' },
        };
        make_bundle(root, "needslat", "needslat", '6', "\"" NEEDY "\"", n1, 1,
                    "{\"validated\": true, \"installedAt\": 500}");
        make_bundle(root, "needsok0", "needsok0", '7', "\"" NEEDY "\", \"generated/decls/md6def/zion/here.md6.decl\"",
                    n2, 2, "{\"validated\": true, \"installedAt\": 400}");
    }
    {
        /* animWebs naming models: missing, and provided by the bundle (a
         * model the engine knows serves as well). */
        static const fx_file aw[] = {
            { "generated/decls/animweb/mods/awpack00/med_station.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/objects/props_set/kiosks/wallhealthstation.md6\" \"\"\t// modelIndex 0\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "mods/awpack00/med_station", NULL, '1' },
            { "generated/decls/animweb/mods/awpack00/good.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"mods/awpack00/door.md6\" \"\"\n\t\t\t\"zion/stock/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "mods/awpack00/good", NULL, '2' },
            { "generated/decls/md6def/mods/awpack00/door.md6.decl", "{ md6 }", -1, "md6Def", "mods/awpack00/door.md6", NULL, '3' },
        };
        g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\"}},\n  ";
        make_bundle(root, "awpack00 folder", "awpack00", '8', NULL, aw, 3, NULL);
        hash64('8', aw_hash);
    }
    g_bundle_extra = NULL;

    log_clear();
    CHECK(sh_modserve_index_build(root) == 8);    g_bundle_extra = NULL;

    log_clear();
    CHECK(sh_modserve_index_build(root) == 8);
    /* The animWeb the boss bundles carry: one copy serves, never none. */
    CHECK(log_has("conflict: shadow '" CYB_AW "' is provided by cyberdem, localcyb, "
                  "localcyb; serving localcyb (most providers carry these bytes)"));
    CHECK(sh_modserve_lookup(CYB_AW, &hit) && !strcmp(hit.bundle_id, "localcyb"));
    CHECK(open_text(CYB_AW, text, sizeof text) == 1 && strstr(text, "AW_CF_IDLE_CYCLE"));
    CHECK(sh_modserve_probe_claims(CYB_AW));
    /* The conductor: every boss's aiTypeList entry is served. */
    /* (index order: the root folders, then maps' places) */
    CHECK(log_has("conflict: shadow '" CONDUCTOR "' is provided by cyberdem, localcyb, "
                  "localhel, localspi, localcyb; serving "));
    CHECK(log_has("'s text with the others' list entries (3 added)"));
    CHECK(sh_modserve_conflict_info(CONDUCTOR, &c) && c.composed && c.count == 5 && strstr(c.reason, "composed: 3 list entries"));
    CHECK(open_text(CONDUCTOR, text, sizeof text) == 1 && strstr(text, "num = 6;") && strstr(text, "\"524288\"") &&
          strstr(text, "\"2097152\"") && strstr(text, "\"4194304\"") && strstr(text, "\"1048576\""));
    CHECK(sh_modserve_lookup(CONDUCTOR, &hit) && hit.size == strlen(text));
    CHECK(open_text(WHITELIST, text, sizeof text) == 1 && strstr(text, "single_cyberdemon") && strstr(text, "single_spider_mastermind"));
    {
        sh_modserve_stats st;
        sh_modserve_get_stats(&st);
        CHECK(st.conflicts == 4 && st.conflicts_composed == 2);
    }
    /* Needs before recency. */
    CHECK(log_has("conflict: shadow '" NEEDY "' is provided by needslat, needsok0; serving needsok0 "
                  "(its needs resolve against the installed bundles where the others' do not)"));
    CHECK(open_text(NEEDY, text, sizeof text) == 1 && !strcmp(text, "{ pain b }"));
    CHECK(!log_has("serving neither"));

    /* Registration: an animWeb whose models block names a model nothing
     * provides is withdrawn and never registered. */
    hashes[0] = aw_hash;
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3 && unknown == 0);
    log_clear();
    g_calls[0] = 0;
    ready = sh_modserve_plan_run(&p, &g_cf, "map");
    CHECK(ready == 2);
    CHECK(log_has("map: animWeb 'mods/awpack00/med_station' (awpack00) NOT READY: it names model "
                  "'zion/objects/props_set/kiosks/wallhealthstation.md6', which neither a bundle nor the game provides"));
    CHECK(log_has("map: animWeb 'mods/awpack00/good' (awpack00) READY"));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/mods/awpack00/med_station.decl", NULL));
    CHECK(!sh_modserve_probe_claims("generated/decls/animweb/mods/awpack00/med_station.decl"));
    CHECK(sh_modserve_lookup("generated/decls/animweb/mods/awpack00/good.decl", NULL));
    sh_modserve_plan_free(&p);
    /* A rescan (an install, a conversion) keeps them withdrawn. */
    CHECK(sh_modserve_index_rescan() == 8);
    CHECK(!sh_modserve_lookup("generated/decls/animweb/mods/awpack00/med_station.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/mods/awpack00/good.decl", NULL));
    CHECK(sh_modserve_lookup(CONDUCTOR, &hit) && hit.size > 0);

    /* The Play-time re-check: live gates log once and set nothing; a gate set
     * back to 1 is disarmed again. */
    gates_reset(1, 0, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_check(&g_gates, "play", &r) == 1 && g_gate_sets == 0);
    CHECK(log_has("MODS: play: requirements live: g_useResourceBlackList=0 (unchanged since the last pass)"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_check(&g_gates, "play", &r) == 1 && g_gate_val[1] == 0 && r.changed == 1);
    CHECK(log_has("MODS: play: requirements applied: g_useResourceBlackList=0") && !log_has("requirements live"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* ------------------------------------------------------------------ editor row references (lane MG2) */

static char g_refs[2048];
static void collect_ref(void *c, const char *type, const char *name, const char *row)
{
    size_t at = strlen(g_refs);
    (void)c;
    _snprintf_s(g_refs + at, sizeof g_refs - at, _TRUNCATE, "%s:%s<%s;", type, name, row);
}

static void test_editor_refs(void)
{
    char top[MAX_PATH], root[MAX_PATH];
    char *slash;
    sh_modserve_plan p;
    static const fx_file row[] = {
        { "generated/decls/snapeditorentitydef/mods/localcyb/cyberdem_enc.decl",
          "{\n\tinherit = \"demons/demon_base\";\n\tedit = {\n"
          "\t\tpreviewImage = \"textures/guis/snapmaps/entity_icons/demons/cyberdem_enc\";\n"
          "\t\tentityDef = \"snapmaps/placeablesnapaiencounter/cyberdem\";\n"
          "\t\trenderInfo = {\n\t\t\trenderModelName = \"zion/characters/monsters/cyberdem/base/cyberdem_snapeditor.md6\";\n\t\t}\n"
          "\t\teventInfo = {\n\t\t\tanimWebDecl = \"mods/localcyb/cyberdem_snapeditor\";\n\t\t}\n\t}\n}\n",
          -1, "snapEditorEntityDef", "mods/localcyb/cyberdem_enc", NULL, '1' },
        { "generated/decls/snapeditorentitydef/mods/localcyb/crate.decl",
          "{\n\tedit = {\n\t\trenderInfo = {\n\t\t\trenderModelName = \"models/mapobjects/crate.lwo\";\n\t\t}\n\t}\n}\n",
          -1, "snapEditorEntityDef", "mods/localcyb/crate", NULL, '2' },
        { "generated/decls/animweb/mods/localcyb/cyberdem_snapeditor.decl", "{ web }", -1, "animWeb",
          "mods/localcyb/cyberdem_snapeditor", NULL, '3' },
    };
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\edrefs\\mods", top);
    mkdirs(root);
    make_bundle(root, "local cyber", "localcyb", '9', NULL, row, 3, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_plan_editor(&p) == 2);
    /* Rows that did not get ready point at nothing yet. */
    g_refs[0] = 0;
    CHECK(sh_modserve_editor_refs(&p, collect_ref, NULL) == 0 && !g_refs[0]);
    g_already = NULL;
    CHECK(sh_modserve_plan_run(&p, &g_fake, "editor") == 2);
    g_refs[0] = 0;
    CHECK(sh_modserve_editor_refs(&p, collect_ref, NULL) == 3);
    /* The editor animWeb, the md6 render model and the preview material; the
     * entityDef (re-resolved by name) and a non-md6 model are not decls a row
     * holds this way. */
    CHECK(strstr(g_refs, "animWeb:mods/localcyb/cyberdem_snapeditor<mods/localcyb/cyberdem_enc;") != NULL);
    CHECK(strstr(g_refs, "md6Def:zion/characters/monsters/cyberdem/base/cyberdem_snapeditor.md6<mods/localcyb/cyberdem_enc;") != NULL);
    CHECK(strstr(g_refs, "material:textures/guis/snapmaps/entity_icons/demons/cyberdem_enc<mods/localcyb/cyberdem_enc;") != NULL);
    CHECK(!strstr(g_refs, "crate") && !strstr(g_refs, "entityDef"));
    CHECK(sh_modserve_editor_refs(NULL, collect_ref, NULL) == 0 && sh_modserve_editor_refs(&p, NULL, NULL) == 0);
    sh_modserve_plan_free(&p);
}

/* ------------------------------------------------------------------ resource watermark (lane MG2) */

/* The engine's list-of-lists, as the purge driver walks it: a list holds its
 * class name at +0x10, the next list at +0x18, entries at +0x20, count at
 * +0x28; a resource holds its level at +0x28. */
typedef struct fake_res { unsigned char pad[0x28]; unsigned level; unsigned state; } fake_res;
typedef struct fake_list {
    unsigned char pad[0x10];
    const char *name;
    struct fake_list *next;
    fake_res **entries;
    int count, cap;
} fake_list;

static void test_watermark(void)
{
    static fake_res r[8];
    static fake_res *anims[8], *models[8];
    static fake_list la, lm, bad;
    fake_list *head = &la;
    sh_modserve_watermark w;
    sh_modserve_raised out;
    int i;
    CHECK(offsetof(fake_list, name) == 0x10 && offsetof(fake_list, next) == 0x18 &&
          offsetof(fake_list, entries) == 0x20 && offsetof(fake_list, count) == 0x28 && offsetof(fake_res, level) == 0x28);
    for (i = 0; i < 8; ++i) r[i].level = 4;
    la.name = "anim"; la.next = &lm; la.entries = anims;
    lm.name = "md6Def"; lm.next = NULL; lm.entries = models;
    /* Before the pass: one stock anim (4) and one map-scoped md6Def (1). */
    anims[0] = &r[0]; la.count = 1;
    r[1].level = 1; models[0] = &r[1]; lm.count = 1;
    CHECK(sh_modserve_watermark_take(&head, &w) == 1 && w.ok && w.n == 2);
    /* The pass creates three anims and an md6Def at level 1, and one at 2. */
    r[2].level = 1; r[3].level = 1; r[4].level = 1; r[5].level = 2; r[6].level = 1;
    anims[1] = &r[2]; anims[2] = &r[3]; anims[3] = &r[4]; anims[4] = &r[5]; la.count = 5;
    models[1] = &r[6]; lm.count = 2;
    CHECK(sh_modserve_watermark_raise(&head, &w, &out) == 1);
    CHECK(out.raised == 4 && out.created == 5 && out.lists == 2);
    CHECK(!strcmp(out.summary, "anim 3, md6Def 1"));
    CHECK(r[2].level == 4 && r[3].level == 4 && r[4].level == 4 && r[6].level == 4);
    /* What existed before the pass keeps its level; another level is left. */
    CHECK(r[1].level == 1 && r[5].level == 2 && r[0].level == 4);
    /* A second raise finds nothing new at level 1. */
    CHECK(sh_modserve_watermark_raise(&head, &w, &out) == 1 && out.raised == 0 && out.created == 5);
    sh_modserve_watermark_free(&w);
    CHECK(w.res == NULL && w.n == 0 && !w.ok);
    /* A list that does not read as one stops the walk: no watermark, nothing raised. */
    bad.name = "bad"; bad.next = NULL; bad.entries = NULL; bad.count = 3;
    lm.next = &bad;
    CHECK(sh_modserve_watermark_take(&head, &w) == 0 && !w.ok);
    CHECK(sh_modserve_watermark_raise(&head, &w, &out) == 0 && out.raised == 0);
    sh_modserve_watermark_free(&w);
    CHECK(sh_modserve_watermark_take(NULL, &w) == 0);
    sh_modserve_watermark_free(&w);
    lm.next = NULL;
    /* Membership: what existed when the watermark was taken. */
    CHECK(sh_modserve_watermark_take(&head, &w) == 1);
    CHECK(sh_modserve_watermark_has(&w, &r[6]) && sh_modserve_watermark_has(&w, &r[0]) && !sh_modserve_watermark_has(&w, &r[7]));
    anims[5] = &r[7]; la.count = 6;
    CHECK(!sh_modserve_watermark_has(&w, &r[7]) && !sh_modserve_watermark_has(NULL, &r[0]));
    sh_modserve_watermark_free(&w);
    CHECK(!sh_modserve_watermark_has(&w, &r[0]));
    la.count = 5;
}

/* ------------------------------------------------------------------ what kept decls hold (triage F6) */

/* The engine's lists with their type at +0x08 (the class at +0x10); a
 * resource holds its name at +0x08 and its level at +0x28. */
typedef struct kh_res { unsigned char pad[8]; const char *name; unsigned char pad2[0x18]; unsigned level; unsigned state; } kh_res;
typedef struct kh_list {
    unsigned char pad[8];
    const char *type, *cls;
    struct kh_list *next;
    kh_res **entries;
    int count, cap;
} kh_list;

#define KH_WEB   "generated/decls/animweb/mods/khmod000/s/web.decl"
#define KH_LOOSE "generated/decls/animweb/mods/khmod000/s/loose.decl"
#define KH_MD6   "generated/decls/md6def/mods/khmod000/s/body.md6.decl"
#define KH_MESH  "generated/basemodel/md6/mods/khmod000/body.bmd6model"

/* A pickup's web kept at level 4 by a pass after the map's own load parsed it
 * at level 1, so the clip that parse created is older than the pass's
 * watermark (report X1): everything a level-4 decl names is raised by name,
 * through the files of the index that provide its needs, once it exists. */
static void test_kept_holds(void)
{
    static const fx_file files[] = {
        { KH_WEB, "animWeb web {}", -1, "animWeb", "mods/khmod000/s/web",
          "[{\"type\":\"anim\",\"name\":\"md6/stock/pickups-melee/chainsaw_pre_idle.md6anim\"},"
          "{\"type\":\"md6Def\",\"name\":\"mods/khmod000/s/body.md6\"}]", '1' },
        { KH_MD6, "md6Def body {}", -1, "md6Def", "mods/khmod000/s/body.md6",
          "[{\"type\":\"baseModel\",\"name\":\"md6/mods/khmod000/body.md6mesh\"},"
          "{\"type\":\"skeleton\",\"name\":\"md6/stock/body.md6skl\"},"
          "{\"type\":\"anim\",\"name\":\"md6/stock/body_idle.md6anim\"},"
          "{\"type\":\"anim\",\"name\":\"md6/stock/body.md6skl/_default.md6anim\"}]", '2' },
        { KH_MESH, "MESH", -1, "baseModel", "md6/mods/khmod000/body.md6mesh", NULL, '3' },
        { KH_LOOSE, "animWeb loose {}", -1, "animWeb", "mods/khmod000/s/loose",
          "[{\"type\":\"anim\",\"name\":\"md6/stock/loose.md6anim\"}]", '4' },
    };
    static kh_res web, loose, body, mesh, skel, idle, bind, clip, other, loose_clip, wrong_type, clip2;
    static kh_res *webs[4], *md6s[4], *meshes[4], *skels[4], *anims[8], *images[4];
    static kh_list lw, ld, lm, ls, la, li;
    kh_list *head = &lw;
    char top[MAX_PATH], root[MAX_PATH], *slash;
    sh_modserve_holds h;
    printf("-- kept decls: what a level-4 decl holds is kept with it, by name\n");
    CHECK(offsetof(kh_list, type) == 0x08 && offsetof(kh_list, cls) == 0x10 && offsetof(kh_list, next) == 0x18 &&
          offsetof(kh_list, entries) == 0x20 && offsetof(kh_list, count) == 0x28 &&
          offsetof(kh_res, name) == 0x08 && offsetof(kh_res, level) == 0x28);
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\holds\\mods", top);
    mkdirs(root);
    make_bundle(root, "khmod000", "khmod000", '5', NULL, files, 4, NULL);
    CHECK(sh_modserve_index_build(root) == 1);

    lw.type = "animWeb"; lw.cls = "idDeclAnimWeb"; lw.next = &ld; lw.entries = webs;
    ld.type = "md6Def"; ld.cls = "idDeclMD6"; ld.next = &lm; ld.entries = md6s;
    lm.type = "baseModel"; lm.cls = "idMD6Model"; lm.next = &ls; lm.entries = meshes;
    ls.type = "skeleton"; ls.cls = "idMD6Skel"; ls.next = &la; ls.entries = skels;
    la.type = "anim"; la.cls = "idMD6Anim"; la.next = &li; la.entries = anims;
    li.type = "image"; li.cls = "idImage"; li.next = NULL; li.entries = images;
    /* After the map pass: the web and the md6Def kept (4), what the engine's
     * own load created for them at 1, a web no pass kept and its clip at 1. */
    web.name = "mods/khmod000/s/web"; web.level = 4;
    loose.name = "mods/khmod000/s/loose"; loose.level = 1;
    webs[0] = &web; webs[1] = &loose; lw.count = 2;
    body.name = "mods/khmod000/s/body.md6"; body.level = 4; md6s[0] = &body; ld.count = 1;
    mesh.name = "md6/mods/khmod000/body.md6mesh"; mesh.level = 1; meshes[0] = &mesh; lm.count = 1;
    skel.name = "md6/stock/body.md6skl"; skel.level = 1; skels[0] = &skel; ls.count = 1;
    idle.name = "md6/stock/body_idle.md6anim"; idle.level = 1;
    other.name = "md6/stock/other.md6anim"; other.level = 1;
    loose_clip.name = "md6/stock/loose.md6anim"; loose_clip.level = 1;
    /* The model's default clip, which the builder records beside the mesh
     * and the skeleton the cooked mesh names (triage F7). */
    bind.name = "md6/stock/body.md6skl/_default.md6anim"; bind.level = 1;
    anims[0] = &idle; anims[1] = &other; anims[2] = &loose_clip; anims[3] = &bind; la.count = 4;
    /* A resource of another type under a held name is not the one held. */
    wrong_type.name = "md6/stock/pickups-melee/chainsaw_pre_idle.md6anim"; wrong_type.level = 1;
    images[0] = &wrong_type; li.count = 1;

    /* The md6Def's mesh, skeleton and clip are raised; the web's clip does
     * not exist yet; what nothing kept names stays map-scoped. */
    CHECK(sh_modserve_holds_raise(&head, &h) == 1);
    CHECK(h.roots == 2 && h.names == 6 && h.raised == 4 && h.lists == 6);
    CHECK(mesh.level == 4 && skel.level == 4 && idle.level == 4 && bind.level == 4);
    CHECK(other.level == 1 && loose_clip.level == 1 && loose.level == 1 && wrong_type.level == 1);
    CHECK(strstr(h.first, "md6/mods/khmod000/body.md6mesh") && strstr(h.first, "md6/stock/body.md6skl") &&
          strstr(h.first, "md6/stock/body_idle.md6anim") && strstr(h.first, "md6/stock/body.md6skl/_default.md6anim"));

    /* The web's clip, created after the keep (the editor's spawn, say), at 1,
     * spelled as the engine folds it: the next run raises it. */
    clip.name = "MD6/Stock/pickups_melee/chainsaw_pre_idle.md6anim"; clip.level = 1;
    anims[4] = &clip; la.count = 5;
    CHECK(sh_modserve_holds_raise(&head, &h) == 1 && h.raised == 1 && clip.level == 4 &&
          !strcmp(h.first, "MD6/Stock/pickups_melee/chainsaw_pre_idle.md6anim"));
    CHECK(wrong_type.level == 1 && other.level == 1);
    /* Nothing more to raise: a second run changes nothing. */
    CHECK(sh_modserve_holds_raise(&head, &h) == 1 && h.raised == 0 && h.first[0] == 0);

    /* Released: a web no longer held at level 4 keeps nothing; a clip of its
     * name created after that stays map-scoped. */
    web.level = 1;
    clip2.name = "md6/stock/pickups-melee/chainsaw_pre_idle.md6anim"; clip2.level = 1;
    anims[5] = &clip2; la.count = 6;
    CHECK(sh_modserve_holds_raise(&head, &h) == 1 && h.roots == 1 && h.raised == 0 && clip2.level == 1);
    /* An unreadable list: the walk fails and nothing is raised. */
    li.entries = NULL; li.count = 2;
    web.level = 4;
    CHECK(sh_modserve_holds_raise(&head, &h) == 0 && clip2.level == 1);
    li.entries = images; li.count = 1;
    CHECK(sh_modserve_holds_raise(NULL, &h) == 0);
}

/* ------------------------------------------------------------------ models kept decls hold (X2) */

#define HM_ENT   "generated/decls/entitydef/mods/hmmod000/snapmaps/props/rock.decl"
#define HM_LOOSE "generated/decls/entitydef/mods/hmmod000/snapmaps/props/loose.decl"
#define HM_ROCK  "cooked/model/mods/hmmod000/models/rock.bmodel"
#define HM_SPIRE "cooked/model/mods/hmmod000/models/spire.bmodel"
#define HM_PEB   "cooked/model/mods/hmmod000/models/pebble.bmodel"

typedef struct hm_seen { char order[1024]; int n; void *res[8]; } hm_seen;
static int hm_visit(void *context, const char *name, void *resource)
{
    hm_seen *c = (hm_seen *)context;
    size_t at = strlen(c->order);
    _snprintf_s(c->order + at, sizeof c->order - at, _TRUNCATE, "%s%s;", name, resource ? "" : "(new)");
    if (c->n < 8) c->res[c->n] = resource;
    c->n++;
    return 1;
}

/* A mod prop's model (report X2): the entityDef is kept for the session, its
 * model is loaded per map at the entity's spawn. The product loads each model
 * a kept decl needs while the model's file serves: one not held yet, a default
 * box, or one a map heap holds; never one already held whole, a stock model,
 * one whose file this scope does not serve, or one only an unkept decl needs. */
static void test_held_models(void)
{
    static const fx_file files[] = {
        { HM_ENT, "entityDef rock {}", -1, "entityDef", "mods/hmmod000/snapmaps/props/rock",
          "[{\"type\":\"model\",\"name\":\"mods/hmmod000/models/rock.lwo\"},"
          "{\"type\":\"model\",\"name\":\"mods/hmmod000/models/spire.lwo\"},"
          "{\"type\":\"model\",\"name\":\"models/mapobjects/hell_terrain/stock_rock.lwo\"}]", '1' },
        { HM_LOOSE, "entityDef loose {}", -1, "entityDef", "mods/hmmod000/snapmaps/props/loose",
          "[{\"type\":\"model\",\"name\":\"mods/hmmod000/models/pebble.lwo\"}]", '2' },
        { HM_ROCK, "ROCK", -1, "model", "cooked/model/mods/hmmod000/models/rock.bmodel", NULL, '3' },
        { HM_SPIRE, "SPIRE", -1, "model", "cooked/model/mods/hmmod000/models/spire.bmodel", NULL, '4' },
        { HM_PEB, "PEBBLE", -1, "model", "cooked/model/mods/hmmod000/models/pebble.bmodel", NULL, '6' },
    };
    static kh_res rock, loose, spire, stock, pebble;
    static kh_res *ents[4], *models[8];
    static kh_list le, lm;
    kh_list *head = &le;
    char top[MAX_PATH], root[MAX_PATH], *slash;
    sh_modserve_holds h;
    hm_seen seen;
    int kept = 0, depth;
    printf("-- held models: the models a kept decl needs are loaded for the session while their files serve\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\heldmodels\\mods", top);
    mkdirs(root);
    make_bundle(root, "hmmod000", "hmmod000", '5', NULL, files, 5, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    sh_modserve_attrib_reset();

    le.type = "entityDef"; le.cls = "idDeclEntityDef"; le.next = &lm; le.entries = ents;
    lm.type = "model"; lm.cls = "idStaticModel"; lm.next = NULL; lm.entries = models;
    rock.name = "mods/hmmod000/snapmaps/props/rock"; rock.level = 4;
    loose.name = "mods/hmmod000/snapmaps/props/loose"; loose.level = 1;
    ents[0] = &rock; ents[1] = &loose; le.count = 2;
    stock.name = "models/mapobjects/hell_terrain/stock_rock.lwo"; stock.level = 1; stock.state = 0x64;
    models[0] = &stock; lm.count = 1;

    /* The editor rows registered the decl; nothing has loaded its models:
     * both of its own are loaded, the stock one and the unkept decl's are not. */
    memset(&seen, 0, sizeof seen);
    CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 2);
    CHECK(!strcmp(seen.order, "mods/hmmod000/models/rock.lwo(new);mods/hmmod000/models/spire.lwo(new);"));
    CHECK(h.roots == 1);

    /* Loaded whole and held: nothing more to do, a second run calls nothing. */
    spire.name = "Mods/HMMOD000/models/spire.lwo"; spire.level = 4; spire.state = 0x60;
    rock.state = 0;
    {
        static kh_res rock_model;
        rock_model.name = "mods/hmmod000/models/rock.lwo"; rock_model.level = 4; rock_model.state = 0x70;
        models[1] = &rock_model; models[2] = &spire; lm.count = 3;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 0 && seen.order[0] == 0);

        /* The engine's default box (bit 2), at level 1: loaded again in place. */
        rock_model.level = 1; rock_model.state = 0x64;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 1 && seen.res[0] == &rock_model &&
              !strcmp(seen.order, "mods/hmmod000/models/rock.lwo;"));
        /* A failed load (bit 0) the same way, even at level 4. */
        rock_model.level = 4; rock_model.state = 0x01;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 1 && seen.res[0] == &rock_model);

        /* Whole at level 1, loaded in process heap 0: the holds raise keeps it,
         * no load. Loaded under a map heap: loaded again (F9's gate would leave
         * it to the purge, and the next load may be where its file is not served). */
        rock_model.level = 1; rock_model.state = 0x60;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 0);
        depth = sh_modserve_attrib_enter(&rock_model, 3, &kept);
        sh_modserve_attrib_leave(depth);
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 1 && seen.res[0] == &rock_model);
        /* A load in process heap 0 clears the mark. */
        depth = sh_modserve_attrib_enter(&rock_model, 0, &kept);
        sh_modserve_attrib_leave(depth);
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 0);

        /* Map scope on, a map open that does not declare hmmod000: in the editor
         * its names serve (the Toybox), so a default is loaded again; in Play
         * they do not, and nothing is loaded (it would come out a default). */
        rock_model.state = 0x64;
        sh_modserve_scope_enable(1);
        CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 0);
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 1);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0);
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 0 && seen.order[0] == 0);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) >= 0);
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 1);
        CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
        sh_modserve_scope_enable(0);

        /* The decl released: nothing it needs is loaded. */
        rock.level = 1;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == 0);
        rock.level = 4;
        /* An unreadable list: the walk fails, nothing is called. */
        lm.entries = NULL; lm.count = 2;
        memset(&seen, 0, sizeof seen);
        CHECK(sh_modserve_held_models(&head, hm_visit, &seen, &h) == -1 && seen.n == 0);
        lm.entries = models; lm.count = 3;
        CHECK(sh_modserve_held_models(NULL, hm_visit, &seen, &h) == -1);
    }
    (void)pebble;
    sh_modserve_attrib_reset();
}

/* ------------------------------------------------------------------ editor closure after an in-session install (R-315) */

#define CL_ROW   "generated/decls/snapeditorentitydef/mods/clmod000/tile.decl"
#define CL_ENT   "generated/decls/entitydef/mods/clmod000/tile.decl"
#define CL_MD6   "generated/decls/md6def/mods/clmod000/s/body.md6.decl"
#define CL_MESH  "generated/basemodel/md6/mods/clmod000/body.bmd6model"
#define CL_SCENE "generated/decls/entitydef/mods/clmod000/scene.decl"
#define CL_GAME  "generated/decls/entitydef/mods/clgame00/gameplay.decl"
#define CL_PRIV  "generated/decls/md6def/mods/clmod000/editor_preview/tile.md6.decl"
#define CL_NEW   "generated/decls/entitydef/mods/clgame00/newgame.decl"
#define CL_ROW_V2 "{\n\tedit = {\n\t\teventInfo = { idleEvent = { animInfo = { animHandle = \"ci_editor_preview/idle\"; } } }\n" \
    "\t\trenderInfo = {\n\t\t\trenderModelName = \"mods/clmod000/editor_preview/tile.md6\";\n" \
    "\t\t\trenderModelAttachments = { num = 1; item[0] = { renderModelName = \"zion/objects/props_char/keycard_blue.md6\"; tagName = \"pre_pickup\"; } }\n" \
    "\t\t}\n\t\tfxInfo = { fxDecl = \"fx/titan_hell_switch\"; startCondition = \"FX_NONE\"; }\n" \
    "\t\tentityDef = \"mods/clmod000/tile\";\n\t\tpreviewImage = \"textures/guis/snapmaps/entity_icons/cl\";\n\t}\n}\n"

typedef struct cl_seen { char order[1024]; } cl_seen;
static int cl_visit(void *context, const sh_modserve_refresh_item *it)
{
    cl_seen *c = (cl_seen *)context;
    size_t at = strlen(c->order);
    _snprintf_s(c->order + at, sizeof c->order - at, _TRUNCATE, "%s;", it->name);
    return 1;
}

/* A mod booted with older rows, then installed again in-session with rows
 * that need new decls: every file of a bundle with rows, and everything a row
 * needs, is the editor closure (a row-less bundle's decl is not); a refresh applies that part on its own (inside the engine side's
 * watermark) and the rest after; a map or refresh plan keeps the closure's
 * new decls and runs them first; a later part does not load what needs a
 * failed kept decl; and the keeper visits every reference a registered row
 * names, its attachments' models included. */
static void test_editor_closure(void)
{
    static const fx_file v1[] = {
        { CL_ROW, "{ edit = { entityDef = \"mods/clmod000/tile\"; } }", -1, "snapEditorEntityDef", "mods/clmod000/tile",
          "[{\"type\":\"entityDef\",\"name\":\"mods/clmod000/tile\"}]", '1' },
        { CL_ENT, "entityDef tile {}", -1, "entityDef", "mods/clmod000/tile",
          "[{\"type\":\"md6Def\",\"name\":\"mods/clmod000/s/body.md6\"}]", '2' },
        { CL_MD6, "md6Def body { mesh 1 }", -1, "md6Def", "mods/clmod000/s/body.md6",
          "[{\"type\":\"baseModel\",\"name\":\"md6/mods/clmod000/body.md6mesh\"}]", '3' },
        { CL_MESH, "MESH1", -1, "baseModel", "md6/mods/clmod000/body.md6mesh", NULL, '4' },
        { CL_SCENE, "entityDef scene1 {}", -1, "entityDef", "mods/clmod000/scene", NULL, '5' },
    };
    static const fx_file g1[] = { { CL_GAME, "entityDef game1 {}", -1, "entityDef", "mods/clgame00/gameplay", NULL, '5' } };
    static const fx_file v2[] = {
        { CL_ROW, CL_ROW_V2, -1, "snapEditorEntityDef", "mods/clmod000/tile",
          "[{\"type\":\"entityDef\",\"name\":\"mods/clmod000/tile\"},"
          "{\"type\":\"md6Def\",\"name\":\"mods/clmod000/editor_preview/tile.md6\"}]", '6' },
        { CL_ENT, "entityDef tile {}", -1, "entityDef", "mods/clmod000/tile",
          "[{\"type\":\"md6Def\",\"name\":\"mods/clmod000/s/body.md6\"}]", '2' },
        { CL_MD6, "md6Def body { mesh 2 }", -1, "md6Def", "mods/clmod000/s/body.md6",
          "[{\"type\":\"baseModel\",\"name\":\"md6/mods/clmod000/body.md6mesh\"}]", '7' },
        { CL_MESH, "MESH2", -1, "baseModel", "md6/mods/clmod000/body.md6mesh", NULL, '8' },
        { CL_SCENE, "entityDef scene2 {}", -1, "entityDef", "mods/clmod000/scene", NULL, '9' },
        { CL_PRIV, "md6Def tile { inherit = \"mods/clmod000/s/body.md6\" }", -1, "md6Def", "mods/clmod000/editor_preview/tile.md6",
          "[{\"type\":\"md6Def\",\"name\":\"mods/clmod000/s/body.md6\"}]", 'a' },
    };
    static const fx_file g2[] = {
        { CL_GAME, "entityDef game2 {}", -1, "entityDef", "mods/clgame00/gameplay", NULL, '9' },
        { CL_NEW, "entityDef newgame {}", -1, "entityDef", "mods/clgame00/newgame",
          "[{\"type\":\"md6Def\",\"name\":\"mods/clmod000/editor_preview/tile.md6\"}]", 'b' },
    };
    char top[MAX_PATH], root[MAX_PATH], hash[65];
    char *slash;
    const char *h[2];
    char hash2[65];
    sh_modserve_plan p, kept, rest;
    sh_modserve_diff d;
    cl_seen seen;
    int i;
    printf("-- editor closure: an in-session install keeps what its rows need, as a boot does\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\closure\\mods", top);
    mkdirs(root);
    make_bundle(root, "clmod000", "clmod000", 'c', NULL, v1, 5, NULL);
    make_bundle(root, "clgame00", "clgame00", 'e', NULL, g1, 1, NULL);
    CHECK(sh_modserve_index_build(root) == 2);
    /* What earlier tests' tables left pending (their gone shadows) is settled first. */
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(cl_visit, &seen);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    sh_modserve_test_reset_registration();
    /* Boot: the editor pass registers the row and its closure; a map uses the mod. */
    CHECK(sh_modserve_plan_editor(&p) == 3);
    for (i = 0; i < p.count; ++i) CHECK(p.steps[i].persist == 1);
    CHECK(sh_modserve_plan_run(&p, &g_fake, "editor") == 3);
    sh_modserve_plan_free(&p);
    h[0] = hash64('c', hash);
    h[1] = hash64('e', hash2);
    CHECK(sh_modserve_plan_bundles(h, 2, &p, NULL) == 4);
    /* A map pass keeps every decl of a bundle with rows unless the engine
     * holds it permanently (2); a row-less bundle's decl stays at the engine's level. */
    CHECK(p.steps[step_index(&p, "entityDef", "mods/clmod000/tile")].persist == 2);
    CHECK(p.steps[step_index(&p, "md6Def", "mods/clmod000/s/body.md6")].persist == 2);
    CHECK(p.steps[step_index(&p, "entityDef", "mods/clmod000/scene")].persist == 2);
    CHECK(p.steps[step_index(&p, "entityDef", "mods/clgame00/gameplay")].persist == 0);
    /* The row bundle's unregistered scene decl is kept; the registered ones and the gameplay decl are not. */
    CHECK(sh_modserve_plan_split(&p, &kept, &rest) == 1 && kept.count == 1 && !strcmp(kept.steps[0].name, "mods/clmod000/scene") &&
          rest.count == 3);
    sh_modserve_plan_free(&kept);
    sh_modserve_plan_free(&rest);
    g_calls[0] = 0;
    CHECK(sh_modserve_plan_run(&p, &g_fake, "map") == 4 && strstr(g_calls, "P:entityDef:mods/clmod000/scene;") &&
          strstr(g_calls, "L:entityDef:mods/clgame00/gameplay;"));
    sh_modserve_plan_free(&p);
    CHECK(sh_modserve_refresh_editor_pending() == 0);

    /* The in-session install: the row gains an editor-only md6Def, an
     * attachment and an FX; the body model changes; a gameplay decl changes. */
    make_bundle(root, "clmod000", "clmod000", 'd', NULL, v2, 6, NULL);
    make_bundle(root, "clgame00", "clgame00", 'f', NULL, g2, 2, NULL);
    CHECK(sh_modserve_index_rescan() == 2);
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 5 && d.changed_rows == 1 && d.added_active == 2 && d.pending == 5);
    /* Row, md6Def, model and the row bundle's scene decl are the closure's
     * pending part; the row-less bundle's gameplay decl is not. */
    CHECK(sh_modserve_refresh_editor_pending() == 4);
    memset(&seen, 0, sizeof seen);
    CHECK(sh_modserve_each_refresh_part(cl_visit, &seen, SH_MODSERVE_PART_EDITOR) == 4);
    CHECK(strstr(seen.order, "mods/clmod000/tile;") && strstr(seen.order, "mods/clmod000/s/body.md6;") &&
          strstr(seen.order, "mods/clmod000/scene;") && strstr(seen.order, "md6/mods/clmod000/body.md6mesh;") &&
          !strstr(seen.order, "gameplay"));
    /* Dependency order inside the part: the md6Def before the row, the model last. */
    CHECK(strstr(seen.order, "body.md6;") < strstr(seen.order, "mods/clmod000/tile;") &&
          strstr(seen.order, "mods/clmod000/tile;") < strstr(seen.order, "body.md6mesh;"));
    CHECK(sh_modserve_refresh_editor_pending() == 0);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 1);
    memset(&seen, 0, sizeof seen);
    CHECK(sh_modserve_each_refresh_part(cl_visit, &seen, SH_MODSERVE_PART_REST) == 1 && !strcmp(seen.order, "mods/clgame00/gameplay;"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    CHECK(sh_modserve_each_refresh_part(cl_visit, &seen, SH_MODSERVE_PART_ALL) == 0);

    /* The new decls: the editor-only md6Def is kept and runs first; the new
     * gameplay decl that needs it runs after, at the engine's level, with the
     * registered md6Def both need. */
    CHECK(sh_modserve_plan_refresh(&p) == 3);
    CHECK(p.steps[step_index(&p, "md6Def", "mods/clmod000/editor_preview/tile.md6")].persist == 2);
    CHECK(p.steps[step_index(&p, "entityDef", "mods/clgame00/newgame")].persist == 0);
    CHECK(sh_modserve_plan_split(&p, &kept, &rest) == 1 && kept.count == 1 && rest.count == 2);
    CHECK(!strcmp(kept.steps[0].name, "mods/clmod000/editor_preview/tile.md6") && step_index(&rest, "entityDef", "mods/clgame00/newgame") >= 0 &&
          step_index(&rest, "md6Def", "mods/clmod000/s/body.md6") >= 0);
    /* The kept part fails: the later part does not load what needs it. */
    g_calls[0] = 0;
    g_fail_load = "mods/clmod000/editor_preview/tile.md6";
    log_clear();
    CHECK(sh_modserve_plan_run(&kept, &g_fake, "refresh") == 0);
    CHECK(sh_modserve_plan_block_failed_needs(&rest) == 1 && rest.blocked == 1);
    CHECK(sh_modserve_plan_run(&rest, &g_fake, "refresh") == 1);   /* the registered md6Def only */
    CHECK(!strstr(g_calls, "newgame") && log_has("refresh: entityDef 'mods/clgame00/newgame' (clgame00) BLOCKED"));
    sh_modserve_plan_merge_ready(&p, &kept);
    sh_modserve_plan_merge_ready(&p, &rest);
    CHECK(p.steps[step_index(&p, "md6Def", "mods/clmod000/editor_preview/tile.md6")].ready == -1 &&
          p.steps[step_index(&p, "entityDef", "mods/clgame00/newgame")].ready == -1 &&
          p.steps[step_index(&p, "md6Def", "mods/clmod000/s/body.md6")].ready == 1);
    sh_modserve_plan_free(&kept);
    sh_modserve_plan_free(&rest);
    CHECK(kept.steps == NULL && rest.count == 0);
    g_fail_load = NULL;
    /* The retry keeps it: persist load, after which nothing is left to keep. */
    g_calls[0] = 0;
    CHECK(sh_modserve_plan_split(&p, &kept, &rest) == 1);
    CHECK(sh_modserve_plan_run(&kept, &g_fake, "refresh") == 1 && strstr(g_calls, "P:md6Def:mods/clmod000/editor_preview/tile.md6;"));
    CHECK(sh_modserve_plan_block_failed_needs(&rest) == 0);
    CHECK(sh_modserve_plan_run(&rest, &g_fake, "refresh") == 2 && strstr(g_calls, "L:entityDef:mods/clgame00/newgame;"));
    sh_modserve_plan_free(&kept);
    sh_modserve_plan_free(&rest);
    CHECK(sh_modserve_plan_split(&p, &kept, &rest) == 0);
    sh_modserve_plan_free(&p);
    CHECK(sh_modserve_plan_split(NULL, &kept, &rest) == -1);

    /* The keeper for the rows this session registered: every reference the
     * changed row names, the attachment's stock md6Def included. */
    CHECK(sh_modserve_plan_editor(&p) == 4);
    g_refs[0] = 0;
    CHECK(sh_modserve_editor_refs(&p, collect_ref, NULL) == 0);
    sh_modserve_plan_mark_registered(&p);
    CHECK(p.steps[step_index(&p, "snapEditorEntityDef", "mods/clmod000/tile")].ready == 1);
    CHECK(sh_modserve_editor_refs(&p, collect_ref, NULL) == 3);
    CHECK(strstr(g_refs, "md6Def:mods/clmod000/editor_preview/tile.md6<mods/clmod000/tile;") &&
          strstr(g_refs, "md6Def:zion/objects/props_char/keycard_blue.md6<mods/clmod000/tile;") &&
          strstr(g_refs, "material:textures/guis/snapmaps/entity_icons/cl<mods/clmod000/tile;"));
    sh_modserve_plan_free(&p);
    /* Leave the table the next test expects. */
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* A renderParm is registered and parsed before any material that names it
 * (snaphak-re ADD-G5): a material's parm keys are bare names, so no need
 * orders them, and the engine keeps a renderParm's slot and kind from its
 * first parse. The material comes first in its bundle's file list; the
 * renderParms (one in the same bundle, one in another declared bundle) still
 * come first in the map plan, its run, the kept part of a split and a refresh. */
#define RP_MAT   "generated/decls/material2/mods/rpmat000/skull_key.decl"
#define RP_OWN   "generated/decls/renderparm/mods/rpmat000/own_parm.decl"
#define RP_OTHER "generated/decls/renderparm/mods/rpparm00/other_parm.decl"
#define RP_SURF  "generated/decls/renderparm/skinsssmap.decl"
static void test_renderparm_first(void)
{
    static const fx_file m1[] = {
        { RP_MAT, "{ mods/rpmat000/own_parm textures/system/constant_color/white mods/rpparm00/other_parm 1 skinsssmap x }", -1, "material2",
          "mods/rpmat000/skull_key", NULL, '1' },
        { RP_OWN, "{ Tex2D textures/system/constant_color/white }", -1, "renderParm", "mods/rpmat000/own_parm", NULL, '2' },
        { RP_SURF, "{ Tex2D }", -1, "renderParm", "skinsssmap", NULL, '6' },
    };
    static const fx_file p1[] = {
        { RP_OTHER, "{ Vec4 }", -1, "renderParm", "mods/rpparm00/other_parm", NULL, '3' },
    };
    static const fx_file m2[] = {
        { RP_MAT, "{ mods/rpmat000/own_parm textures/system/constant_color/black mods/rpparm00/other_parm 1 skinsssmap x }", -1, "material2",
          "mods/rpmat000/skull_key", NULL, '4' },
        { RP_OWN, "{ Tex2D textures/system/constant_color/black }", -1, "renderParm", "mods/rpmat000/own_parm", NULL, '5' },
        { RP_SURF, "{ Tex2D }", -1, "renderParm", "skinsssmap", NULL, '7' },
    };
    char top[MAX_PATH], root[MAX_PATH], ha[65], hb[65];
    char *slash;
    const char *h[2];
    sh_modserve_plan p, kept, rest;
    cl_seen seen;
    int mat, own, other;
    const char *lmat, *lown, *lother;
    printf("-- renderParms first: registered and parsed before a material that names them\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\renderparm\\mods", top);
    mkdirs(root);
    /* The mod's own renderParms, and a stock renderParm the pack carries as a
     * stock pointer at its game path (a surface): the game parses that one
     * itself, so it is in no plan (ADD-L11). */
    g_fx_restore = RP_SURF;
    make_bundle(root, "rpmat000", "rpmat000", 'a', NULL, m1, 3, NULL);
    g_fx_restore = NULL;
    make_bundle(root, "rpparm00", "rpparm00", 'b', NULL, p1, 1, NULL);
    CHECK(sh_modserve_index_build(root) == 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(cl_visit, &seen);   /* what earlier tables left pending */
    sh_modserve_test_reset_registration();
    h[0] = hash64('a', ha);
    h[1] = hash64('b', hb);
    CHECK(sh_modserve_plan_bundles(h, 2, &p, NULL) == 3);
    mat = step_index(&p, "material2", "mods/rpmat000/skull_key");
    own = step_index(&p, "renderParm", "mods/rpmat000/own_parm");
    other = step_index(&p, "renderParm", "mods/rpparm00/other_parm");
    CHECK(mat >= 0 && own >= 0 && other >= 0);
    CHECK(step_index(&p, "renderParm", "skinsssmap") < 0 && sh_modserve_is_surface(RP_SURF));
    CHECK(own < mat && other < mat);
    CHECK(p.steps[own].rank == SH_MODSERVE_RANK_RENDERPARM && p.steps[mat].rank == SH_MODSERVE_RANK_OTHER);
    /* A split runs every unregistered renderParm in the part that runs first. */
    CHECK(sh_modserve_plan_split(&p, &kept, &rest) >= 2);
    CHECK(step_index(&kept, "renderParm", "mods/rpmat000/own_parm") >= 0 &&
          step_index(&kept, "renderParm", "mods/rpparm00/other_parm") >= 0);
    CHECK(step_index(&rest, "renderParm", "mods/rpmat000/own_parm") < 0 &&
          step_index(&rest, "renderParm", "mods/rpparm00/other_parm") < 0);
    sh_modserve_plan_free(&kept);
    sh_modserve_plan_free(&rest);
    /* The run registers and loads them in that order. */
    g_calls[0] = 0;
    CHECK(sh_modserve_plan_run(&p, &g_fake, "map") == 3);
    lmat = strstr(g_calls, ":material2:mods/rpmat000/skull_key;");
    lown = strstr(g_calls, ":renderParm:mods/rpmat000/own_parm;");
    lother = strstr(g_calls, ":renderParm:mods/rpparm00/other_parm;");
    CHECK(lmat && lown && lother && lown < lmat && lother < lmat);
    CHECK(!strstr(g_calls, "skinsssmap"));
    if (!(lown && lmat && lown < lmat)) printf("  calls: %s\n", g_calls);
    sh_modserve_plan_free(&p);
    /* A refresh of both changed decls visits the renderParm first, and in the
     * editor part, which runs before the rest. */
    g_fx_restore = RP_SURF;
    make_bundle(root, "rpmat000", "rpmat000", 'c', NULL, m2, 3, NULL);
    g_fx_restore = NULL;
    CHECK(sh_modserve_index_rescan() >= 1);
    memset(&seen, 0, sizeof seen);
    CHECK(sh_modserve_each_refresh_part(cl_visit, &seen, SH_MODSERVE_PART_EDITOR) >= 1);
    CHECK(strstr(seen.order, "mods/rpmat000/own_parm;") != NULL);
    if (strstr(seen.order, "mods/rpmat000/skull_key;"))
        CHECK(strstr(seen.order, "mods/rpmat000/own_parm;") < strstr(seen.order, "mods/rpmat000/skull_key;"));
    CHECK(strstr(seen.order, "skinsssmap;") == NULL);   /* a surface is never refreshed */
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh_part(cl_visit, &seen, SH_MODSERVE_PART_REST);
    CHECK(strstr(seen.order, "mods/rpmat000/own_parm;") == NULL && strstr(seen.order, "skinsssmap;") == NULL);
    /* Leave the table the next test expects. */
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* ------------------------------------------------------------------ surfaces (ADD-L11) */

#define SF_SURF  "generated/decls/material/models/monsters/sf/body.decl"     /* a surface */
#define SF_SKIN  "generated/decls/material/mods/sfmod000/skin.decl"
#define SF_ENT   "generated/decls/entitydef/mods/sfmod000/thing.decl"
#define SF_GAME  "generated/decls/entitydef/mods/sfmod000/gameplay.decl"
#define SF_ROW   "generated/decls/snapeditorentitydef/mods/sfmod000/thing.decl"
#define SO_ENT   "generated/decls/entitydef/mods/sfother0/rock.decl"
#define SO_ROW   "generated/decls/snapeditorentitydef/mods/sfother0/rock.decl"
#define SF_NEEDS_SURF "[{\"type\":\"material\",\"name\":\"models/monsters/sf/body\"}]"

static void sf_fixture(char *root, size_t cap)
{
    static const fx_file m1[] = {
        { SF_SURF, "{ diffusemap models/monsters/sf/body }", -1, "material", "models/monsters/sf/body", NULL, '1' },
        { SF_SKIN, "{ skin }", -1, "material", "mods/sfmod000/skin", SF_NEEDS_SURF, '2' },
        { SF_ENT, "{ thing }", -1, "entityDef", "mods/sfmod000/thing",
          "[{\"type\":\"material\",\"name\":\"mods/sfmod000/skin\"},{\"type\":\"material\",\"name\":\"models/monsters/sf/body\"}]", '3' },
        { SF_GAME, "{ gameplay }", -1, "entityDef", "mods/sfmod000/gameplay", SF_NEEDS_SURF, '4' },
        { SF_ROW, "{\n\tedit = {\n\t\tentityDef = \"mods/sfmod000/thing\";\n"
                  "\t\tpreviewImage = \"models/monsters/sf/body\";\n\t}\n}\n", -1, "snapEditorEntityDef", "mods/sfmod000/thing",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfmod000/thing\"}]", '5' },
    };
    static const fx_file m2[] = {   /* a rebuild: the surface's pointer and the skin's bytes change */
        { SF_SURF, "{ diffusemap models/monsters/sf/body }", -1, "material", "models/monsters/sf/body", NULL, '6' },
        { SF_SKIN, "{ skin v2 }", -1, "material", "mods/sfmod000/skin", SF_NEEDS_SURF, '7' },
        { SF_ENT, "{ thing }", -1, "entityDef", "mods/sfmod000/thing",
          "[{\"type\":\"material\",\"name\":\"mods/sfmod000/skin\"},{\"type\":\"material\",\"name\":\"models/monsters/sf/body\"}]", '3' },
        { SF_GAME, "{ gameplay }", -1, "entityDef", "mods/sfmod000/gameplay", SF_NEEDS_SURF, '4' },
        { SF_ROW, "{\n\tedit = {\n\t\tentityDef = \"mods/sfmod000/thing\";\n"
                  "\t\tpreviewImage = \"models/monsters/sf/body\";\n\t}\n}\n", -1, "snapEditorEntityDef", "mods/sfmod000/thing",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfmod000/thing\"}]", '5' },
    };
    static const fx_file o1[] = {   /* a mod the launched map does not declare: its row serves for the Toybox */
        { SO_ENT, "{ rock }", -1, "entityDef", "mods/sfother0/rock", NULL, '8' },
        { SO_ROW, "{ entityDef \"mods/sfother0/rock\" }", -1, "snapEditorEntityDef", "mods/sfother0/rock",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfother0/rock\"}]", '9' },
    };
    char top[MAX_PATH];
    char *slash;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, cap, _TRUNCATE, "%s\\surfaces\\mods", top);
    mkdirs(root);
    g_fx_restore = SF_SURF;
    make_bundle(root, "sfmod000", "sfmod000", 'a', NULL, m1, 5, NULL);
    g_fx_restore = NULL;
    make_bundle(root, "sfother0", "sfother0", 'b', NULL, o1, 2, NULL);
    (void)m2;
}

static void sf_rebuild(const char *root)
{
    static const fx_file m1[] = {
        { SF_SURF, "{ diffusemap models/monsters/sf/body }", -1, "material", "models/monsters/sf/body", NULL, '1' },
        { SF_SKIN, "{ skin }", -1, "material", "mods/sfmod000/skin", SF_NEEDS_SURF, '2' },
        { SF_ENT, "{ thing }", -1, "entityDef", "mods/sfmod000/thing",
          "[{\"type\":\"material\",\"name\":\"mods/sfmod000/skin\"},{\"type\":\"material\",\"name\":\"models/monsters/sf/body\"}]", '3' },
        { SF_GAME, "{ gameplay }", -1, "entityDef", "mods/sfmod000/gameplay", SF_NEEDS_SURF, '4' },
        { SF_ROW, "{\n\tedit = {\n\t\tentityDef = \"mods/sfmod000/thing\";\n"
                  "\t\tpreviewImage = \"models/monsters/sf/body\";\n\t}\n}\n", -1, "snapEditorEntityDef", "mods/sfmod000/thing",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfmod000/thing\"}]", '5' },
    };
    static const fx_file m2[] = {   /* a rebuild: the surface's pointer and the skin's bytes change */
        { SF_SURF, "{ diffusemap models/monsters/sf/body }", -1, "material", "models/monsters/sf/body", NULL, '6' },
        { SF_SKIN, "{ skin v2 }", -1, "material", "mods/sfmod000/skin", SF_NEEDS_SURF, '7' },
        { SF_ENT, "{ thing }", -1, "entityDef", "mods/sfmod000/thing",
          "[{\"type\":\"material\",\"name\":\"mods/sfmod000/skin\"},{\"type\":\"material\",\"name\":\"models/monsters/sf/body\"}]", '3' },
        { SF_GAME, "{ gameplay }", -1, "entityDef", "mods/sfmod000/gameplay", SF_NEEDS_SURF, '4' },
        { SF_ROW, "{\n\tedit = {\n\t\tentityDef = \"mods/sfmod000/thing\";\n"
                  "\t\tpreviewImage = \"models/monsters/sf/body\";\n\t}\n}\n", -1, "snapEditorEntityDef", "mods/sfmod000/thing",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfmod000/thing\"}]", '5' },
    };
    static const fx_file o1[] = {   /* a mod the launched map does not declare: its row serves for the Toybox */
        { SO_ENT, "{ rock }", -1, "entityDef", "mods/sfother0/rock", NULL, '8' },
        { SO_ROW, "{ entityDef \"mods/sfother0/rock\" }", -1, "snapEditorEntityDef", "mods/sfother0/rock",
          "[{\"type\":\"entityDef\",\"name\":\"mods/sfother0/rock\"}]", '9' },
    };
    (void)m1;
    (void)o1;
    g_fx_restore = SF_SURF;
    make_bundle(root, "sfmod000", "sfmod000", 'a', NULL, m2, 5, NULL);
    g_fx_restore = NULL;
}

static void sf_leave(void)
{
    char top[MAX_PATH], root[MAX_PATH];
    char *slash;
    sh_modserve_test_reset_registration();
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\plain\\mods", top);
    (void)sh_modserve_index_build(root);
}

/* A surface (a stock pointer at the game's own path: the game's bytes under
 * the game's name) is served but in no plan (map, editor, refresh), no
 * refresh list, no early-shadow or gated-reload list and no row reference;
 * and an in-place reload or re-parse outside a watermark opens its own
 * keeper (report ADD-G6). */
static void test_surfaces_never_parsed(void)
{
    char root[MAX_PATH], h[65], seen_raw[1024];
    const char *hashes[1];
    sh_modserve_plan map, ed, rf;
    rf_seen seen;
    printf("-- surfaces: served, never planned, refreshed or reloaded; in-place work outside a watermark is kept\n");
    CHECK(sh_modserve_inplace_keeper(0, 3) == 1);    /* a scope change's gated reload, a map pass's rest */
    CHECK(sh_modserve_inplace_keeper(1, 3) == 0);    /* inside the editor pass's watermark: that keeper keeps it */
    CHECK(sh_modserve_inplace_keeper(0, 0) == 0);    /* nothing to reload: no resource walk */
    sf_fixture(root, sizeof root);
    CHECK(sh_modserve_index_build(root) == 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_visit, &seen);   /* what earlier tables left pending */
    sh_modserve_test_reset_registration();
    CHECK(sh_modserve_is_surface(SF_SURF) == 1 && sh_modserve_is_surface("GENERATED\\decls\\material\\models\\monsters\\sf\\body.decl") == 1);
    CHECK(sh_modserve_is_surface(SF_SKIN) == 0 && sh_modserve_is_surface("generated/decls/material/none.decl") == 0);
    CHECK(sh_modserve_lookup(SF_SURF, NULL) == 1);     /* still served: the game parses it from these bytes */
    hashes[0] = hash64('a', h);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &map, NULL) == 3);
    CHECK(step_index(&map, "material", "models/monsters/sf/body") < 0);
    CHECK(step_index(&map, "entityDef", "mods/sfmod000/gameplay") >= 0 && step_index(&map, "material", "mods/sfmod000/skin") >= 0);
    g_calls[0] = 0;
    g_already = NULL;
    CHECK(sh_modserve_plan_run(&map, &g_fake, "map") == map.count);
    CHECK(!strstr(g_calls, "models/monsters/sf/body") && strstr(g_calls, "R:entityDef:mods/sfmod000/gameplay;"));
    sh_modserve_plan_free(&map);
    CHECK(sh_modserve_plan_editor(&ed) == 5);
    CHECK(step_index(&ed, "material", "models/monsters/sf/body") < 0);
    g_calls[0] = 0;
    CHECK(sh_modserve_plan_run(&ed, &g_fake, "editor") == ed.count);
    CHECK(!strstr(g_calls, "models/monsters/sf/body"));
    /* The row's preview image is a surface: the keeper never holds or parses it. */
    g_refs[0] = 0;
    CHECK(sh_modserve_editor_refs(&ed, collect_ref, NULL) == 0 && !g_refs[0]);
    sh_modserve_plan_free(&ed);
    seen_raw[0] = 0;
    (void)sh_modserve_each_unopened_shadow(raw_visit, seen_raw);
    (void)sh_modserve_each_ungated_raw(raw_visit, seen_raw);
    CHECK(!strstr(seen_raw, "models/monsters/sf/body"));
    /* A rescan where the surface's pointer changed and the skin's bytes did:
     * the skin is refreshed, the surface is not; no refresh plan holds it. */
    sf_rebuild(root);
    CHECK(sh_modserve_index_rescan() >= 1);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_refresh_expand();
    CHECK(sh_modserve_each_refresh(rf_visit, &seen) >= 1);
    CHECK(strstr(seen.order, ":mods/sfmod000/skin:") != NULL);
    CHECK(strstr(seen.order, "models/monsters/sf/body") == NULL);
    if (strstr(seen.order, "models/monsters/sf/body")) printf("  refresh: %s\n", seen.order);
    CHECK(sh_modserve_plan_refresh(&rf) >= 0);
    CHECK(step_index(&rf, "material", "models/monsters/sf/body") < 0);
    sh_modserve_plan_free(&rf);
    CHECK(sh_modserve_plan_editor(&ed) == 5 && step_index(&ed, "material", "models/monsters/sf/body") < 0);
    sh_modserve_plan_free(&ed);
    sf_leave();
}

/* What a cold hub launch runs, with the engine faked: the parse's map pass,
 * then the gate's pre-load pass (the editor rows and their closure, then
 * what the declared builds still lack), then what the level load runs (its
 * palette's editor pass and any map pass). Only the first two register, and
 * the surface is in none of them. */
static void test_launch_preload(void)
{
    char root[MAX_PATH], h[65];
    const char *hashes[1];
    sh_modserve_plan map, ed, map2, ed2;
    rf_seen seen;
    int n;
    printf("-- a hub launch parses at its gate; its level load registers nothing new\n");
    sf_fixture(root, sizeof root);
    CHECK(sh_modserve_index_build(root) == 2);
    memset(&seen, 0, sizeof seen);
    (void)sh_modserve_each_refresh(rf_visit, &seen);
    sh_modserve_test_reset_registration();             /* a cold session */
    hashes[0] = hash64('a', h);

    /* The parse's map pass (modserve_activate_bundles at the launched map's parse). */
    CHECK(sh_modserve_plan_bundles(hashes, 1, &map, NULL) == 3);
    CHECK(sh_modserve_plan_unvisited(&map) == map.count);
    g_calls[0] = 0;
    g_already = NULL;
    CHECK(sh_modserve_plan_run(&map, &g_fake, "map") == map.count);
    CHECK(sh_modserve_plan_unvisited(&map) == 0);
    sh_modserve_plan_free(&map);

    /* The gate's pre-load pass (sh_modserve_launch_preload): the editor rows
     * first, the declared row and the undeclared one, then the declared
     * builds, which the parse's pass already settled. */
    CHECK(sh_modserve_plan_editor(&ed) == 5);
    n = sh_modserve_plan_unvisited(&ed);
    CHECK(n == 3);   /* the rows and the other mod's entityDef; the declared closure was the map pass's */
    g_calls[0] = 0;
    CHECK(sh_modserve_plan_run(&ed, &g_fake, "launch") == ed.count);
    CHECK(strstr(g_calls, "R:snapEditorEntityDef:mods/sfmod000/thing;") && strstr(g_calls, "R:snapEditorEntityDef:mods/sfother0/rock;"));
    CHECK(!strstr(g_calls, "models/monsters/sf/body"));
    sh_modserve_plan_free(&ed);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &map, NULL) == 3 && sh_modserve_plan_unvisited(&map) == 0);
    sh_modserve_plan_free(&map);

    /* The level load: its palette's editor pass and a map pass register and load nothing. */
    CHECK(sh_modserve_plan_editor(&ed2) == 5 && sh_modserve_plan_unvisited(&ed2) == 0);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &map2, NULL) == 3 && sh_modserve_plan_unvisited(&map2) == 0);
    g_calls[0] = 0;
    g_registered_count = 0;
    CHECK(sh_modserve_plan_run(&ed2, &g_fake, "editor") == ed2.count);
    CHECK(sh_modserve_plan_run(&map2, &g_fake, "map") == map2.count);
    CHECK(g_calls[0] == 0 && g_registered_count == 0);
    if (g_calls[0]) printf("  in-load calls: %s\n", g_calls);
    sh_modserve_plan_free(&ed2);
    sh_modserve_plan_free(&map2);
    sf_leave();
}

/* ------------------------------------------------------------------ refusals (issue 132) */

/* The engine side of the issue-132 pass: the archives hold the campaign md6Defs
 * (stock_exists 1), but the engine's own lookup (decl_found) finds only what
 * SnapMap loaded; one parse the engine refuses although the load reports
 * success. */
static char g_console_seen[4096];
static void rf_console(const char *line)
{
    size_t at = strlen(g_console_seen);
    _snprintf_s(g_console_seen + at, sizeof g_console_seen - at, _TRUNCATE, "%s\n", line);
}
static int rf_source(void *c, const char *type, const char *name)
{
    (void)c;
    return !_stricmp(type, "animWeb") && !_stricmp(name, "player/fp_hands_mp");
}
static int rf_stock(void *c, const char *request)
{
    (void)c;
    return !strncmp(request, "generated/decls/md6def/zion/", 28);
}
static int rf_found(void *c, const char *type, const char *name)
{
    (void)c;
    return !_stricmp(type, "md6Def") && !strcmp(name, "zion/snap/known.md6");
}
static int g_rf_loads;
static int rf_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    (void)c; (void)type;
    g_rf_loads++;
    call_log(persist ? "P" : "L", type, name);
    *lifetime = 1;
    *state = 0;
    return !strcmp(name, "mods/dzpack00/throws") ? SH_MODSERVE_LOAD_REFUSED : 1;
}
static void rf_refusal(void *c, char *out, size_t cap)
{
    (void)c;
    strncpy_s(out, cap, "S:\\DOOM\\base\\generated\\decls\\animWeb\\mods\\dzpack00\\throws.decl(9): error : "
              "Could not find animweb decl: 'gone'", _TRUNCATE);
}
static const sh_modserve_engine_ops g_rf = { NULL, rf_source, fake_register, rf_load, rf_stock, NULL, rf_found, rf_refusal };
/* The same engine without the lookup: the archives decide, as before. */
static const sh_modserve_engine_ops g_rf_old = { NULL, rf_source, fake_register, rf_load, rf_stock };

static void rf_reconstruct(void *res) { ((fake_res *)res)->level = 1; }
static void rf_plain(void *res) { ((fake_res *)res)->state = 0; }

static void test_refusals(void)
{
    char top[MAX_PATH], root[MAX_PATH], hash[65], model[200];
    char *slash;
    sh_modserve_plan p;
    const char *hashes[1];
    int unknown = 0, ready;
    static fake_res res;
    unsigned was = 0;
    static const fx_file files[] = {
        { "generated/decls/animweb/mods/dzpack00/med_station.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/objects/props_set/kiosks/wallhealthstation.md6\" \"\"\t// modelIndex 0\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "mods/dzpack00/med_station", NULL, '1' },
        { "generated/decls/animweb/mods/dzpack00/turret.decl",
          "{\n\tprops {\n\t\tmodels {\n\t\t\t\"zion/snap/known.md6\"\n\t\t\t\"zion/objects/weapons/turret_campaign.md6\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "mods/dzpack00/turret", NULL, '2' },
        { "generated/decls/animweb/mods/dzpack00/fine.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/snap/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "mods/dzpack00/fine", NULL, '3' },
        { "generated/decls/animweb/mods/dzpack00/throws.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/snap/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "mods/dzpack00/throws", NULL, '4' },
    };

    /* The error text names the decl's own file, either slash, any case. */
    CHECK(sh_modserve_error_names_decl("S:\\Steam\\steamapps\\common\\DOOM\\base\\generated\\decls\\animWeb\\designsystems\\"
                                       "med_station.decl(21): error : Could not find model 'x.md6' in 'models' block.",
                                       "animWeb", "designsystems/med_station"));
    CHECK(sh_modserve_error_names_decl("generated/decls/md6def/zion/a/b.md6.decl(3): error : x", "md6Def", "zion/a/b.md6"));
    CHECK(!sh_modserve_error_names_decl("generated\\decls\\animWeb\\designsystems\\med_station_heal.decl(23): error : x",
                                        "animWeb", "designsystems/med_station"));
    CHECK(!sh_modserve_error_names_decl("", "animWeb", "a") && !sh_modserve_error_names_decl(NULL, "animWeb", "a"));

    /* A failed in-place load reconstructs the object, whose constructor writes
     * level 1: the level is put back; a load that keeps it is left alone. */
    res.level = 4;
    CHECK(sh_modserve_load_keep_lifetime(&res, rf_reconstruct, &was) == 1 && was == 4 && res.level == 4);
    res.level = 2;
    CHECK(sh_modserve_load_keep_lifetime(&res, rf_plain, &was) == 2 && was == 2 && res.level == 2);

    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\refusals\\mods", top);
    mkdirs(root);
    /* A pack whose animWebs name models (the issue-132 "SP doors", in the one
     * shape: its own names under its salt). */
    make_bundle(root, "dzpack00 folder", "dzpack00", '9', NULL, files, 4, NULL);
    hash64('9', hash);
    hashes[0] = hash;
    CHECK(sh_modserve_index_build(root) == 1);

    /* The check alone: the engine lookup decides a load now. */
    CHECK(HO(model, sh_modserve_animweb_check("generated/decls/animweb/mods/dzpack00/med_station.decl", &g_rf, HO_P(0))) == 1 &&
          !strcmp(model, "zion/objects/props_set/kiosks/wallhealthstation.md6"));
    CHECK(HO(model, sh_modserve_animweb_check("generated/decls/animweb/mods/dzpack00/turret.decl", &g_rf, HO_P(0))) == 1 &&
          !strcmp(model, "zion/objects/weapons/turret_campaign.md6"));
    CHECK(HO(model, sh_modserve_animweb_check("generated/decls/animweb/mods/dzpack00/fine.decl", &g_rf, HO_P(0))) == 0);
    CHECK(HO(model, sh_modserve_animweb_check("generated/decls/md6def/zion/none.md6.decl", &g_rf, HO_P(0))) == -1);
    /* Without the lookup the archives answer, which is what let issue 132's
     * campaign models through. */
    CHECK(HO(model, sh_modserve_animweb_check("generated/decls/animweb/mods/dzpack00/med_station.decl", &g_rf_old, HO_P(0))) == 0);

    g_console_seen[0] = 0;
    sh_modserve_set_console(rf_console);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 4 && unknown == 0);
    log_clear();
    g_calls[0] = 0;
    g_rf_loads = 0;
    ready = sh_modserve_plan_run(&p, &g_rf, "map");
    CHECK(ready == 1);
    CHECK(log_has("map: animWeb 'mods/dzpack00/med_station' (dzpack00) NOT READY: it names model "
                  "'zion/objects/props_set/kiosks/wallhealthstation.md6', which neither a bundle nor the game provides"));
    CHECK(log_has("map: animWeb 'mods/dzpack00/turret' (dzpack00) NOT READY: it names model 'zion/objects/weapons/turret_campaign.md6'"));
    CHECK(log_has("map: animWeb 'mods/dzpack00/fine' (dzpack00) READY"));
    CHECK(log_has("map: animWeb 'mods/dzpack00/throws' (dzpack00) NOT READY: the engine refused its parse: "
                  "S:\\DOOM\\base\\generated\\decls\\animWeb\\mods\\dzpack00\\throws.decl(9): error : Could not find animweb decl: 'gone'; "
                  "withdrawn, not loaded again"));
    /* Never loaded: the two whose models SnapMap cannot load. */
    CHECK(!strstr(g_calls, "mods/dzpack00/med_station") && !strstr(g_calls, "mods/dzpack00/turret"));
    CHECK(g_rf_loads == 2);
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dzpack00: assets/generated/decls/animweb/mods/dzpack00/med_station.decl was "
                                 "not loaded because it names the model zion/objects/props_set/kiosks/wallhealthstation.md6, "
                                 "which SnapMap cannot load (it is campaign-only or missing). Name a model SnapMap has, then "
                                 "press Refresh.\n") != NULL);
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dzpack00: the game could not read assets/generated/decls/animweb/mods/dzpack00/"
                                 "throws.decl: ") != NULL);
    CHECK(!strstr(g_console_seen, "mods/dzpack00/fine"));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/mods/dzpack00/med_station.decl", NULL));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/mods/dzpack00/throws.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/mods/dzpack00/fine.decl", NULL));
    sh_modserve_plan_free(&p);

    /* The next map load with the same bundle parses none of them again. */
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 4);
    log_clear();
    g_calls[0] = 0;
    g_rf_loads = 0;
    g_console_seen[0] = 0;
    ready = sh_modserve_plan_run(&p, &g_rf, "map");
    CHECK(ready == 1 && g_rf_loads == 0);
    CHECK(log_has("map: animWeb 'mods/dzpack00/throws' (dzpack00) NOT READY: withdrawn earlier in this session"));
    CHECK(log_has("map: animWeb 'mods/dzpack00/med_station' (dzpack00) NOT READY: withdrawn earlier in this session"));
    CHECK(g_console_seen[0] == 0);
    sh_modserve_plan_free(&p);

    /* A withdrawal by request (the early re-parse's refusal) names the file once. */
    CHECK(sh_modserve_withdraw("generated/decls/animweb/mods/dzpack00/fine.decl", "animWeb", "mods/dzpack00/fine",
                               "the game refused its bytes; the game's own decl is restored") == 1);
    CHECK(log_has("MODS: withdrawn: animWeb 'mods/dzpack00/fine' (dzpack00): the game refused its bytes"));
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dzpack00: assets/generated/decls/animweb/mods/dzpack00/fine.decl was not "
                                 "loaded: the game refused its bytes; the game's own decl is restored. Fix the file and press "
                                 "Refresh in the Mods tab.") != NULL);
    CHECK(!sh_modserve_lookup("generated/decls/animweb/mods/dzpack00/fine.decl", NULL));
    CHECK(sh_modserve_withdraw("generated/decls/animweb/mods/dzpack00/fine.decl", "animWeb", "mods/dzpack00/fine", "again") == 0);
    sh_modserve_set_console(NULL);
}

/* ------------------------------------------------------------------ first parse of a permanent decl (lane P52) */

/* The engine side of the P52 crash: the menu collectibles' unlockModel
 * 'spidermastermind' is permanent (level 4), and the md6Def it names was
 * created by this map pass at level 1. In the first crash the bundle arrived
 * after boot (taken here as a decl created by reference, state 2); in the
 * second it was installed at boot, so the game had parsed the unlockModel
 * through the provider before the Init promotion (state 0). DeclSourceFind
 * knows neither decl, so the plan reaches the load. */
typedef struct pf_res { const char *type, *name; unsigned level, state; int parses; } pf_res;
static pf_res g_pf[2];
static int pf_source(void *c, const char *type, const char *name) { (void)c; (void)type; (void)name; return 0; }
static int pf_stock(void *c, const char *request) { (void)c; (void)request; return 1; }
static int pf_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    int i;
    (void)c;
    for (i = 0; i < 2; ++i) {
        pf_res *r = &g_pf[i];
        if (_stricmp(r->type, type) || strcmp(r->name, name)) continue;
        /* As op_load decides it. */
        if (sh_modserve_load_left_to_engine(r->level, r->state, persist)) {
            *state = r->state;
            *lifetime = r->level;
            return SH_MODSERVE_LOAD_LEFT;
        }
        call_log(persist ? "P" : "L", type, name);
        r->state = 0;
        r->parses++;
        *state = r->state;
        *lifetime = r->level;
        return 1;
    }
    return 0;
}
static const sh_modserve_engine_ops g_pf_ops = { NULL, pf_source, fake_register, pf_load, pf_stock };

static void test_permanent_first_parse(void)
{
    char top[MAX_PATH], root[MAX_PATH], hash[65];
    char *slash;
    sh_modserve_plan p;
    const char *hashes[1];
    int unknown = 0, ready;
    static const fx_file files[] = {
        { "generated/decls/unlockmodel/mods/pfspider/spidermastermind.decl",
          "{\n\tedit = {\n\t\tunlockModel = \"mods/pfspider/spider_mastermind.md6\";\n\t}\n}\n",
          -1, "unlockModel", "mods/pfspider/spidermastermind", NULL, '1' },
        { "generated/decls/md6def/mods/pfspider/spider_mastermind.md6.decl",
          "{\n\tinit = {\n\t}\n}\n",
          -1, "md6Def", "mods/pfspider/spider_mastermind.md6", NULL, '2' },
    };

    int round;

    /* The decision: every permanent decl that has not failed, and only in a
     * pass that does not keep its decls. */
    CHECK(sh_modserve_load_left_to_engine(4, 0x02, 0) == 1);       /* created by reference */
    CHECK(sh_modserve_load_left_to_engine(4, 0xc2, 0) == 1);       /* upper state bits are not the parse */
    CHECK(sh_modserve_load_left_to_engine(4, 0x00, 0) == 1);       /* parsed (the second crash) */
    CHECK(sh_modserve_load_left_to_engine(4, 0x50, 0) == 1);
    CHECK(sh_modserve_load_left_to_engine(4, 0x01, 0) == 0);       /* failed or defaulted: as before */
    CHECK(sh_modserve_load_left_to_engine(4, 0x03, 0) == 0);
    CHECK(sh_modserve_load_left_to_engine(1, 0x00, 0) == 0);       /* map-scoped: purged with what it names */
    CHECK(sh_modserve_load_left_to_engine(1, 0x02, 0) == 0);
    CHECK(sh_modserve_load_left_to_engine(2, 0x00, 0) == 0);
    CHECK(sh_modserve_load_left_to_engine(4, 0x00, 1) == 0);       /* the editor pass keeps its closure */
    CHECK(sh_modserve_load_left_to_engine(4, 0x02, 1) == 0);

    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;

    /* Round 0: installed during a play hold (the unlockModel only referenced).
     * Round 1: installed before boot (the unlockModel parsed at boot). */
    for (round = 0; round < 2; ++round) {
        unsigned before = round ? 0x00 : 0x02;
        char digit = round ? '7' : '8';
        char line[400];
        _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\firstparse%d\\mods", top, round);
        mkdirs(root);
        g_bundle_extra = NULL;
        /* The second time the unlockModel is what the real bundle ships: a
         * pointer to the game's own bytes under the same name. */
        g_fx_restore = round ? "generated/decls/unlockmodel/mods/pfspider/spidermastermind.decl" : NULL;
        make_bundle(root, "pfspider folder", "pfspider", digit, NULL, files, 2, NULL);
        g_fx_restore = NULL;
        g_bundle_extra = NULL;
        hash64(digit, hash);
        hashes[0] = hash;
        CHECK(sh_modserve_index_build(root) == 1);
        sh_modserve_test_reset_registration();   /* a new session: nothing remembered as ready */
        /* The refresh and the early re-parse ask this before re-parsing a permanent decl. */
        CHECK(sh_modserve_is_restore("generated/decls/unlockmodel/mods/pfspider/spidermastermind.decl") == round);
        CHECK(sh_modserve_is_restore("generated/decls/md6def/mods/pfspider/spider_mastermind.md6.decl") == 0);
        CHECK(sh_modserve_is_restore("generated/decls/unlockmodel/nothing.decl") == 0);
        g_pf[0].type = "unlockModel"; g_pf[0].name = "mods/pfspider/spidermastermind";
        g_pf[0].level = 4; g_pf[0].state = before; g_pf[0].parses = 0;
        g_pf[1].type = "md6Def"; g_pf[1].name = "mods/pfspider/spider_mastermind.md6";
        g_pf[1].level = 1; g_pf[1].state = 0x00; g_pf[1].parses = 0;

        /* Round 1's unlockModel is a surface (the game's bytes at the game's
         * name): no plan holds it at all (ADD-L11). */
        CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == (round ? 1 : 2) && unknown == 0);
        CHECK((step_index(&p, "unlockModel", "mods/pfspider/spidermastermind") < 0) == round);
        log_clear();
        g_calls[0] = 0;
        ready = sh_modserve_plan_run(&p, &g_pf_ops, "map");
        CHECK(ready == (round ? 1 : 2));
        /* The map pass never parses the permanent collectible... */
        CHECK(g_pf[0].parses == 0 && g_pf[0].state == before && g_pf[0].level == 4);
        CHECK(!strstr(g_calls, "L:unlockModel:mods/pfspider/spidermastermind;") && (!round || !strstr(g_calls, "unlockModel")));
        _snprintf_s(line, sizeof line, _TRUNCATE, "map: unlockModel 'mods/pfspider/spidermastermind' (pfspider) left to the game: "
                    "permanent, %s (state 0x%02x lifetime 4); the provider serves these bytes whenever the game parses it",
                    round ? "already parsed" : "not parsed yet", before);
        CHECK(log_has(line) == !round);
        CHECK(!log_has("map: unlockModel 'mods/pfspider/spidermastermind' (pfspider) READY"));
        /* ...while the map-scoped md6Def is still loaded with the bundle's bytes. */
        CHECK(g_pf[1].parses == 1 &&
              strstr(g_calls, "L:md6Def:mods/pfspider/spider_mastermind.md6;"));
        CHECK(log_has("map: md6Def 'mods/pfspider/spider_mastermind.md6' (pfspider) READY: "
                      "state 0x00 lifetime 1"));
        /* The provider still serves the shadow for the game's own parse. */
        CHECK(sh_modserve_lookup("generated/decls/unlockmodel/mods/pfspider/spidermastermind.decl", NULL));
        sh_modserve_plan_free(&p);

        /* The next map load skips both quietly. */
        CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == (round ? 1 : 2));
        CHECK(sh_modserve_plan_unvisited(&p) == 0);
        log_clear();
        g_calls[0] = 0;
        ready = sh_modserve_plan_run(&p, &g_pf_ops, "map");
        CHECK(ready == (round ? 1 : 2) && g_calls[0] == 0 && g_pf[0].parses == 0 && g_pf[1].parses == 1);
        CHECK(!log_has("left to the game"));
        sh_modserve_plan_free(&p);
    }
}

/* ------------------------------------------------------------------ registration re-parses a held decl (lane P53) */

/* What the engine does that the earlier fake engines did not model:
 * DeclRegisterFile re-parses an existing resource of the registered name on the
 * spot (VK FUN_141559a30 -> FUN_141561af0 -> DeclParseSource), and an
 * unlockModel's model field is set by DeclFind(md6Def, name, create), at the
 * level the pass creates objects at. Observed live with a hardware watchpoint on
 * the unlockModel's +0x70: the store came from op_register_file during the map
 * activation, the md6Def it stored was at level 1, the purge at START MISSION
 * freed it, and the shell map load after Play faulted reading it (VK RVA
 * 0x33a130). This fake engine binds the same way, so the test fails if a pass
 * that does not keep its decls re-parses a permanent decl by registration. */
typedef struct hr_res {
    const char *type, *name;
    unsigned level, state;
    int held;              /* the engine has a resource of this name */
    int parses;
    unsigned bound_level;  /* the unlockModel: level of the md6Def it points at, 0 null */
} hr_res;
static hr_res g_hr[3];     /* 0 the unlockModel, 1 its md6Def, 2 a map-scoped animWeb */
static int hr_source(void *c, const char *type, const char *name) { (void)c; (void)type; (void)name; return 0; }
static int hr_stock(void *c, const char *request) { (void)c; (void)request; return 1; }
static hr_res *hr_find(const char *type, const char *name)
{
    int i;
    for (i = 0; i < 3; ++i)
        if (g_hr[i].type && !_stricmp(g_hr[i].type, type) && !strcmp(g_hr[i].name, name)) return &g_hr[i];
    return NULL;
}
static void hr_parse(hr_res *r)
{
    r->parses++;
    r->state = 0;
    /* The unlockModel's parse resolves its model through DeclFind(create). The
     * decl existence probe admits a name a bundle serves (sh_modserve_probe_claims),
     * so the md6Def is created then, at the level of the pass: 1 in a map pass. */
    if (r == &g_hr[0]) {
        if (!g_hr[1].held) { g_hr[1].held = 1; g_hr[1].level = 1; g_hr[1].state = 2; }
        r->bound_level = g_hr[1].level;
    }
}
static int hr_register(void *c, const char *type, const char *name, const char *reg)
{
    hr_res *r = hr_find(type, name);
    (void)c; (void)reg;
    call_log("R", type, name);
    if (r && r->held) hr_parse(r);            /* FUN_141559a30: an existing resource is re-parsed */
    return 1;
}
static int hr_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    hr_res *r = hr_find(type, name);
    (void)c;
    if (!r) return 0;
    if (!r->held) { r->held = 1; r->level = 1; r->state = 2; }
    if (sh_modserve_load_left_to_engine(r->level, r->state, persist)) {
        *state = r->state;
        *lifetime = r->level;
        return SH_MODSERVE_LOAD_LEFT;
    }
    call_log("L", type, name);
    hr_parse(r);
    *state = r->state;
    *lifetime = r->level;
    return 1;
}
static int hr_held(void *c, const char *type, const char *name, unsigned *state, unsigned *lifetime)
{
    hr_res *r = hr_find(type, name);
    (void)c;
    *state = *lifetime = 0;
    if (!r || !r->held) return 0;
    *state = r->state;
    *lifetime = r->level;
    return 1;
}
static const sh_modserve_engine_ops g_hr_ops = { NULL, hr_source, hr_register, hr_load, hr_stock,
                                                 NULL, NULL, NULL, hr_held };
static const sh_modserve_engine_ops g_hr_ops_unaware = { NULL, hr_source, hr_register, hr_load, hr_stock };

static void hr_reset(unsigned unlock_level, unsigned unlock_state, int map_scoped_held)
{
    memset(g_hr, 0, sizeof g_hr);
    g_hr[0].type = "unlockModel"; g_hr[0].name = "mods/hrspider/spidermastermind";
    g_hr[0].level = unlock_level; g_hr[0].state = unlock_state; g_hr[0].held = 1;
    g_hr[1].type = "md6Def"; g_hr[1].name = "mods/hrspider/spider_mastermind.md6";
    g_hr[2].type = "animWeb"; g_hr[2].name = "mods/hrspider/airlock";
    g_hr[2].held = map_scoped_held; g_hr[2].level = 1; g_hr[2].state = 0;
    g_calls[0] = 0;
    sh_modserve_test_reset_registration();   /* a new session: nothing remembered as registered */
}

static void test_registration_reparse_of_held_permanent(void)
{
    char top[MAX_PATH], root[MAX_PATH], hash[65];
    char *slash;
    sh_modserve_plan p;
    const char *hashes[1];
    int unknown = 0, ready;
    static const fx_file files[] = {
        { "generated/decls/unlockmodel/mods/hrspider/spidermastermind.decl",
          "{\n\tedit = {\n\t\tunlockModel = \"mods/hrspider/spider_mastermind.md6\";\n\t}\n}\n",
          -1, "unlockModel", "mods/hrspider/spidermastermind", NULL, '1' },
        { "generated/decls/md6def/mods/hrspider/spider_mastermind.md6.decl",
          "{\n\tinit = {\n\t}\n}\n",
          -1, "md6Def", "mods/hrspider/spider_mastermind.md6", NULL, '2' },
        { "generated/decls/animweb/mods/hrspider/airlock.decl",
          "{\n}\n", -1, "animWeb", "mods/hrspider/airlock", NULL, '3' },
    };

    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\heldreparse\\mods", top);
    mkdirs(root);
    g_bundle_extra = NULL;
    /* The unlockModel carrying its own bytes (a surface, the game's bytes
     * under its name, is never planned at all). */
    g_fx_restore = NULL;
    make_bundle(root, "hrspider folder", "hrspider", '6', NULL, files, 3, NULL);
    g_fx_restore = NULL;
    g_bundle_extra = NULL;
    hash64('6', hash);
    hashes[0] = hash;
    CHECK(sh_modserve_index_build(root) == 1);

    /* The hazard itself, with an engine interface that cannot answer `held`: the
     * registration re-parses the permanent unlockModel and binds it to the md6Def
     * the pass created at level 1 (the live crash). */
    hr_reset(4, 0x00, 1);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3 && unknown == 0);
    log_clear();
    (void)sh_modserve_plan_run(&p, &g_hr_ops_unaware, "map");
    CHECK(strstr(g_calls, "R:unlockModel:mods/hrspider/spidermastermind;") != NULL);
    CHECK(g_hr[0].parses == 1 && g_hr[0].bound_level == 1);
    sh_modserve_plan_free(&p);

    /* The fix: a map pass leaves the held permanent decl unregistered and unparsed. */
    hr_reset(4, 0x00, 1);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3);
    log_clear();
    ready = sh_modserve_plan_run(&p, &g_hr_ops, "map");
    CHECK(ready == 3);
    CHECK(!strstr(g_calls, "R:unlockModel:mods/hrspider/spidermastermind;") && !strstr(g_calls, "L:unlockModel:mods/hrspider/spidermastermind;"));
    CHECK(g_hr[0].parses == 0 && g_hr[0].bound_level == 0 && g_hr[0].level == 4);
    CHECK(log_has("map: unlockModel 'mods/hrspider/spidermastermind' (hrspider) left to the game: permanent, already parsed "
                  "(state 0x00 lifetime 4); not registered by this pass, whose registration would re-parse it in place"));
    CHECK(!log_has("map: unlockModel 'mods/hrspider/spidermastermind' (hrspider) READY"));
    /* The md6Def, new to the engine, is registered and loaded as before (level 1). */
    CHECK(strstr(g_calls, "R:md6Def:mods/hrspider/spider_mastermind.md6;") != NULL);
    CHECK(g_hr[1].held && g_hr[1].level == 1);
    /* A held map-scoped decl is still registered: what it binds dies with it. */
    CHECK(strstr(g_calls, "R:animWeb:mods/hrspider/airlock;") != NULL && g_hr[2].parses >= 1);
    /* The provider still serves the shadow for the game's own parse. */
    CHECK(sh_modserve_lookup("generated/decls/unlockmodel/mods/hrspider/spidermastermind.decl", NULL));
    sh_modserve_plan_free(&p);

    /* The next map load asks again (nothing was remembered as registered) and leaves it again. */
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3);
    log_clear();
    g_calls[0] = 0;
    ready = sh_modserve_plan_run(&p, &g_hr_ops, "map");
    CHECK(ready == 3 && g_hr[0].parses == 0 && !strstr(g_calls, "unlockModel"));
    sh_modserve_plan_free(&p);

    /* Created by reference but never parsed (state 2): the same. */
    hr_reset(4, 0x02, 0);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3);
    log_clear();
    ready = sh_modserve_plan_run(&p, &g_hr_ops, "map");
    CHECK(ready == 3 && g_hr[0].parses == 0);
    CHECK(log_has("map: unlockModel 'mods/hrspider/spidermastermind' (hrspider) left to the game: permanent, not parsed yet "
                  "(state 0x02 lifetime 4); not registered by this pass"));
    /* Not held before: the animWeb is registered (and then loaded at level 1). */
    CHECK(strstr(g_calls, "R:animWeb:mods/hrspider/airlock;") != NULL);
    sh_modserve_plan_free(&p);

    /* A permanent decl whose earlier parse failed is registered as before. */
    hr_reset(4, 0x01, 0);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3);
    log_clear();
    (void)sh_modserve_plan_run(&p, &g_hr_ops, "map");
    CHECK(strstr(g_calls, "R:unlockModel:mods/hrspider/spidermastermind;") != NULL);
    sh_modserve_plan_free(&p);

    /* A map-scoped unlockModel (level 1) is registered too: it is purged with what it names. */
    hr_reset(1, 0x00, 0);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 3);
    log_clear();
    (void)sh_modserve_plan_run(&p, &g_hr_ops, "map");
    CHECK(strstr(g_calls, "R:unlockModel:mods/hrspider/spidermastermind;") != NULL);
    sh_modserve_plan_free(&p);
    sh_modserve_test_reset_registration();
}

/* sh_modserve_each_bundle_extra: admitted bundles with an extra object only,
 * root folders before maps' places, then by salt, then by folder. */
static char g_extra_seen[1024];
static void collect_extra(void *c, const char *id, const char *hash, int place, const char *extra)
{
    (void)c;
    CHECK(hash && strlen(hash) == 64);
    CHECK(extra && extra[0] == '{' && strstr(extra, "\"strings\""));
    _snprintf_s(g_extra_seen + strlen(g_extra_seen), sizeof g_extra_seen - strlen(g_extra_seen), _TRUNCATE,
                "%s%s;", id, place ? "(place)" : "");
}

static void test_bundle_extra(void)
{
    static const fx_file z[] = { { "generated/decls/entitydef/mods/zedmod00/a.decl", "x", -1, "entityDef", "mods/zedmod00/a", NULL, '1' } };
    static const fx_file a1[] = { { "generated/decls/entitydef/mods/abemod00/a1.decl", "x", -1, "entityDef", "mods/abemod00/a1", NULL, '2' } };
    static const fx_file a2[] = { { "generated/decls/entitydef/mods/abemod00/a2.decl", "x", -1, "entityDef", "mods/abemod00/a2", NULL, '3' } };
    static const fx_file bad[] = { { "generated/decls/entitydef/mods/badsize0/a.decl", "x", 99, "entityDef", "mods/badsize0/a", NULL, '4' } };
    static const fx_file plain[] = { { "generated/decls/entitydef/mods/plainmod/a.decl", "x", -1, "entityDef", "mods/plainmod/a", NULL, '5' } };
    static const fx_file old[] = { { "generated/decls/entitydef/mods/aaaold00/a.decl", "x", -1, "entityDef", "mods/aaaold00/a", NULL, '6' } };
    char top[MAX_PATH], mods[MAX_PATH], *slash;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\extra_mods", top);
    mkdirs(mods);
    g_bundle_extra = "\"strings\": {\"en\": {\"zed_name\": \"Zed\"}},";
    make_bundle(mods, "zedmod00", "zedmod00", '7', NULL, z, 1, NULL);
    g_bundle_extra = "\"strings\": {\"en\": {\"abe_name\": \"Abe 2\"}},";
    make_bundle(mods, "abemod00", "abemod00", '8', NULL, a2, 1, NULL);
    g_bundle_extra = "\"hud\": {}, \"strings\": {\"en\": {\"abe_name\": \"Abe 1\"}},";
    make_bundle(mods, "maps\\Abe map\\abemod00", "abemod00", '9', NULL, a1, 1, NULL);
    g_bundle_extra = "\"strings\": {\"en\": {\"bad_name\": \"Bad\"}},";
    make_bundle(mods, "badsize0", "badsize0", '0', NULL, bad, 1, NULL);
    g_bundle_extra = "\"strings\": {\"en\": {\"old_name\": \"Old\"}},";
    make_bundle(mods, "maps\\Old map\\old", "aaaold00", 'f', NULL, old, 1, NULL);
    g_bundle_extra = NULL;
    make_bundle(mods, "plainmod", "plainmod", 'e', NULL, plain, 1, NULL);
    CHECK(sh_modserve_index_build(mods) == 5);
    g_extra_seen[0] = 0;
    CHECK(sh_modserve_each_bundle_extra(collect_extra, NULL) == 4);
    /* Root folders first, then maps' places; by salt, then folder. */
    CHECK(!strcmp(g_extra_seen, "abemod00;zedmod00;aaaold00(place);abemod00(place);"));
    if (strcmp(g_extra_seen, "abemod00;zedmod00;aaaold00(place);abemod00(place);"))
        fprintf(stderr, "bundle extra order: %s\n", g_extra_seen);
}

static void remove_tree_w(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree_w(path);
            else DeleteFileW(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void remove_tree(const char *dir)
{
    wchar_t w[MAX_PATH];
    fx_wide(dir, w, MAX_PATH);
    if (w[0]) remove_tree_w(w);
}

/* ------------------------------------------------------------------ map scope (lane P25, additive phase 2) */

#define SC_COOP   "generated/decls/entitydef/player/coop.decl"
#define SC_IMAGE  "generated/image/models/hands/arms.bimage"
#define SC_SET    "generated/decls/snapeditorsettings/default.decl"
#define SC_ROWDEF "generated/decls/entitydef/monster/hellknight.decl"
#define SC_ROW    "generated/decls/snapeditorentitydef/mods/rrows000/knight.decl"
#define SC_NEW    "generated/decls/entitydef/mods/nnames00/thing.decl"
#define SC_DECL   "generated/decls/entitydef/monster/revenant.decl"
#define SC_MAPED  "generated/decls/snapeditorsettings/other.decl"

typedef struct scope_seen { char order[2048]; } scope_seen;
static int scope_refresh(void *context, const sh_modserve_refresh_item *it)
{
    scope_seen *s = (scope_seen *)context;
    size_t at = strlen(s->order);
    _snprintf_s(s->order + at, sizeof s->order - at, _TRUNCATE, "%s:%s:gone=%d:gates=%d;", it->bundle_id, it->name, it->gone,
                it->gates_live);
    return it->gates_live ? 1 : 0;   /* as the engine: a gated change waits */
}

/* Two layers: a map serves the bundles it declares by hash, then the game.
 * An undeclared bundle offers its names in the editor and with no map open,
 * never its shadows, and serves nothing in Play. A mod.json "scope" no longer
 * chooses; editor-time content still serves in every map. */
static void test_scope(void)
{
    static const fx_file hands[] = {
        { SC_COOP, "entityDef coop { hands }", -1, "entityDef", "player/coop", NULL, '1' },
        { SC_IMAGE, "ARMS", -1, "image", "models/hands/arms", NULL, '2' },
    };
    static const fx_file editor[] = { { SC_SET, "settings {}", -1, "snapEditorSettings", "default", NULL, '3' } };
    static const fx_file declared[] = { { SC_DECL, "entityDef rev {}", -1, "entityDef", "monster/revenant", NULL, '4' } };
    static const fx_file maped[] = { { SC_MAPED, "settings {}", -1, "snapEditorSettings", "other", NULL, '5' } };
    static const fx_file rows[] = {
        { SC_ROWDEF, "entityDef hk {}", -1, "entityDef", "monster/hellknight", NULL, '6' },
        { SC_ROW, "{ entityDef \"monster/hellknight\" }", -1, "snapEditorEntityDef", "mods/rrows000/knight", NULL, '7' },
    };
    static const fx_file names[] = { { SC_NEW, "entityDef t {}", -1, "entityDef", "mods/nnames00/thing", NULL, '8' } };
    char top[MAX_PATH], root[MAX_PATH], h_hands[65], h_rows[65], h_maped[65], h_none[65];
    const char *list[2];
    char *slash;
    sh_modserve_gates r;
    sh_modserve_scope_info si;
    scope_seen seen;
    sh_modserve_diff d;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\scope\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"r_fancy\": true}, \"commands\": {\"activate\": [\"echo hands on\"], "
                     "\"deactivate\": [\"echo hands off\"]}},\n  ";
    make_bundle(root, "ahands00", "ahands00", '1', "\"" SC_COOP "\", \"" SC_IMAGE "\"", hands, 2, "{\"installedAt\": 100}");
    hash64('1', h_hands);
    g_bundle_extra = NULL;
    make_bundle(root, "eeditor0", "eeditor0", '2', "\"" SC_SET "\"", editor, 1, "{\"installedAt\": 110}");
    make_bundle(root, "ddeclare", "ddeclare", '3', "\"" SC_DECL "\"", declared, 1, "{\"installedAt\": 120}");
    make_bundle(root, "meditor0", "meditor0", '4', "\"" SC_MAPED "\"", maped, 1, "{\"installedAt\": 130}");
    hash64('4', h_maped);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_label\": \"knights\"}},\n  ";
    make_bundle(root, "rrows000", "rrows000", '5', "\"" SC_ROWDEF "\"", rows, 2, "{\"installedAt\": 140}");
    hash64('5', h_rows);
    g_bundle_extra = NULL;
    make_bundle(root, "nnames00", "nnames00", '6', NULL, names, 1, "{\"installedAt\": 150}");
    hash64('9', h_none);

    printf("-- map scope: a map serves the bundles it declares, then the game\n");
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_scope_enable(0);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 6);
    /* The new shadows this table brings are pending as at any rescan: settle them first. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    /* Off (as every earlier test): everything serves. */
    CHECK(sh_modserve_lookup(SC_COOP, NULL) && sh_modserve_lookup(SC_IMAGE, NULL) && sh_modserve_lookup(SC_MAPED, NULL));
    CHECK(log_has("MODS: scope: eeditor0 shadows editor-time content (" SC_SET "); its editor tweaks serve in every map"));
    CHECK(log_has("MODS: scope: meditor0 shadows editor-time content (" SC_MAPED "); its editor tweaks serve in every map"));
    CHECK(log_count("MODS: scope:") == 2);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: map scope is off; every bundle serves in every map\n") != NULL);
    CHECK(!sh_modserve_scope_relevant());

    /* On, no map open: undeclared shadows are withdrawn, names and
     * editor-time content serve. */
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_scope_enabled() && !sh_modserve_scope_map_open() && sh_modserve_scope_relevant());
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_lookup(SC_IMAGE, NULL) && !sh_modserve_lookup(SC_ROWDEF, NULL));
    CHECK(!sh_modserve_lookup(SC_DECL, NULL));
    CHECK(sh_modserve_lookup_indexed(SC_COOP, NULL) == 1);
    {
        unsigned char *bytes = NULL;
        size_t len = 0;
        CHECK(sh_modserve_open(SC_COOP, &bytes, &len) == 0 && !bytes);
        CHECK(sh_modserve_probe_claims(SC_COOP) == 0);
    }
    CHECK(sh_modserve_lookup(SC_SET, NULL) && sh_modserve_lookup(SC_MAPED, NULL) && sh_modserve_lookup(SC_NEW, NULL) &&
          sh_modserve_lookup(SC_ROW, NULL));
    /* The engine is told to read the game's bytes again: gone, no gate. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "ahands00:player/coop:gone=1:gates=1;") && strstr(seen.order, "ahands00:models/hands/arms:gone=1:gates=1;"));
    CHECK(strstr(seen.order, "rrows000:monster/hellknight:gone=1") && strstr(seen.order, "ddeclare:monster/revenant:gone=1"));
    CHECK(!strstr(seen.order, "eeditor0") && !strstr(seen.order, "meditor0") && !strstr(seen.order, "nnames00"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* No early-decl offer for a withdrawn shadow. */
    {
        unopened_seen u;
        memset(&u, 0, sizeof u);
        sh_modserve_each_unopened_shadow(unopened_visit, &u);
        CHECK(!strstr(u.order, "player/coop"));
    }
    /* No map: ahands00' requirement is not live; rrows000 keeps its own, because
     * the palette registers its row with no map open too. */
    CHECK(sh_modserve_requirements_apply(&g_gates, "hub", &r) == 1 && r.commands == 0 && !strcmp(g_text_val[1], "0") &&
          !strcmp(g_text_val[3], "knights"));
    CHECK(sh_modserve_bundle_scope(h_hands, &si) && si.scope == SH_MODSERVE_SCOPE_MAP && !si.listed && !si.shadows_live &&
          si.shadows == 2 && si.requirements && !si.requirements_live && si.rank == -1 && !si.inert && si.newest);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: ahands00 (0 names, 2 shadows, 0 surfaces, requirements): not declared "
                           "(no map is open): its names serve for the Toybox and its surfaces serve; its shadows do not\n") != NULL);
    CHECK(strstr(g_report, "scope: eeditor0 (0 names, 1 shadow, 0 surfaces): editor-time content ("
                           SC_SET "): its editor tweaks serve in every map; the rest only in maps that declare it\n") != NULL);
    CHECK(strstr(g_report, "scope: nnames00 (1 name, 0 shadows, 0 surfaces): not declared (no map is "
                           "open): its names serve for the Toybox") != NULL);
    CHECK(strstr(g_report, "scope: rrows000 (1 name, 1 shadow, 0 surfaces, requirements): not declared "
                           "(no map is open): its names serve for the Toybox and its surfaces serve; its shadows do not; its "
                           "requirements stay live because it adds Toybox rows\n") != NULL);
    CHECK(strstr(g_report, "serving: 0 path(s) from mods the open map uses, ") != NULL);

    /* A map that declares nothing, in the editor: the same layers. */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) == 0);
    CHECK(sh_modserve_scope_map_open() && !sh_modserve_lookup(SC_ROWDEF, NULL) && sh_modserve_lookup(SC_NEW, NULL));
    g_events[0] = 0;
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 1 && !strcmp(g_text_val[3], "knights") && !strcmp(g_text_val[1], "0"));
    CHECK(ev_first("C:echo hands on") < 0);
    /* Its Play (ADD-L6 K): no undeclared bundle keeps its requirements,
     * Toybox rows or not, so rrows000' cvar is the game's again; back in the
     * editor its rows need it, and it is applied again. */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) > 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "play", &r) >= 0 && !strcmp(g_text_val[3], ""));
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: rrows000") && !strstr(g_report, "requirements stay live"));
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) > 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) >= 0 && !strcmp(g_text_val[3], "knights"));
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: ahands00 (0 names, 2 shadows, 0 surfaces, requirements): not declared "
                           "by the open map: its names serve") != NULL);

    /* The map declares ahands00 (its row's hash): its shadows serve, marked to be re-parsed and reloaded. */
    list[0] = h_hands;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 1);
    CHECK(sh_modserve_lookup(SC_COOP, NULL) && sh_modserve_lookup(SC_IMAGE, NULL));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "ahands00:player/coop:gone=0:gates=0;") != NULL);   /* waits for its gate */
    g_events[0] = 0;
    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "map load", &r) == 1 && !strcmp(g_text_val[1], "1"));
    CHECK(ev_first("S:r_fancy=1") >= 0 && ev_first("C:echo hands on") > ev_first("S:r_fancy=1"));
    CHECK(log_has("MODS: requirements: ahands00: command \"echo hands on\""));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "ahands00:player/coop:gone=0:gates=1;") && strstr(seen.order, "ahands00:models/hands/arms:gone=0:gates=1;"));
    CHECK(sh_modserve_bundle_scope(h_hands, &si) && si.listed && si.rank == 0 && si.shadows_live && si.requirements_live);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: ahands00 (0 names, 2 shadows, 0 surfaces, requirements): declared by "
                           "the open map (row 1): serves everything it carries\n") != NULL);
    /* The same listing again changes nothing. */
    CHECK(sh_modserve_scope_set(list, 1, 1) == 0);
    /* A rescan keeps the listing: the new table serves it at once, nothing pending. */
    CHECK(sh_modserve_index_rescan() == 6 && sh_modserve_lookup(SC_COOP, NULL));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* A map that declares nothing: the bundle's shadow is withdrawn. */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) == 1 && !sh_modserve_lookup(SC_COOP, NULL));
    list[0] = h_hands;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 1 && sh_modserve_lookup(SC_COOP, NULL));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    /* The build a map-level row is rewritten to: for a stranger's map its own
     * hash, never another build; for the author's own map the root folder's. */
    {
        sh_modserve_bundle_ref ref;
        sh_modmanifest_row row;
        memset(&row, 0, sizeof row);
        strcpy_s(row.salt, sizeof row.salt, "ahands00");
        strcpy_s(row.build, sizeof row.build, h_hands);
        CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, h_hands));
        strcpy_s(row.build, sizeof row.build, h_none);
        CHECK(sh_modserve_place_set("maps\\Their map") == 0);
        CHECK(!sh_modserve_row_build(&row, &ref));   /* a map with a place: that build is not installed */
        CHECK(sh_modserve_place_set(NULL) == 0);
        CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, h_hands));   /* no place: the root's build */
        strcpy_s(row.salt, sizeof row.salt, "rrows000");
        strcpy_s(row.build, sizeof row.build, h_rows);
        CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.id, "rrows000"));
        strcpy_s(row.salt, sizeof row.salt, "nobody.h");
        CHECK(!sh_modserve_row_build(&row, &ref));
    }

    /* Play: the undeclared bundles serve nothing, the declared one all it carries. */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 2);
    CHECK(sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY);
    CHECK(sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_lookup(SC_NEW, NULL) && !sh_modserve_lookup(SC_ROW, NULL));
    CHECK(sh_modserve_lookup(SC_SET, NULL));   /* editor-time content, as before */
    CHECK(sh_modserve_bundle_scope(h_rows, &si) && si.play && !si.names_live);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: nnames00 (1 name, 0 shadows, 0 surfaces): not declared by the map "
                           "being played: serves nothing\n") != NULL);
    /* A withdrawn name is not re-read: the engine keeps what it parsed. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!strstr(seen.order, "nnames00") && !strstr(seen.order, "rrows000"));
    /* Back in the editor: the names serve again, nothing to re-read (the same bytes). */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) >= 2 && sh_modserve_lookup(SC_NEW, NULL));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!strstr(seen.order, "nnames00"));
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) == 0);

    /* The map closes (the hub): the hands' deactivate line runs and r_fancy
     * is restored; g_label stays, rrows000' row is in the palette. */
    g_events[0] = 0;
    CHECK(sh_modserve_scope_set(NULL, 0, 0) == 1);
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_scope_map_open());
    CHECK(sh_modserve_requirements_apply(&g_gates, "map exit", &r) == 1);
    CHECK(ev_first("C:echo hands off") >= 0 && !strcmp(g_text_val[1], "0") && !strcmp(g_text_val[3], "knights"));
    (void)h_maped;

    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* Lane P31, phase 2: a shadow path is served by the provider the open map
 * declares, by hash, also when the static winner is another (two installs of
 * the same bytes); among several declared providers with different bytes the
 * map's row order decides, never the install. */
static void test_scope_providers(void)
{
    static const fx_file bare[] = { { SC_COOP, "entityDef coop { bare }", -1, "entityDef", "player/coop", NULL, '9' } };
    static const fx_file one[] = { { SC_DECL, "entityDef rev { one }", -1, "entityDef", "monster/revenant", NULL, 'a' } };
    static const fx_file two[] = { { SC_DECL, "entityDef rev { two! }", -1, "entityDef", "monster/revenant", NULL, 'b' } };
    char top[MAX_PATH], root[MAX_PATH], h_req[65], h_bare[65], h_one[65], h_two[65];
    const char *list[2];
    char *slash;
    sh_modserve_hit hit;
    sh_modserve_gates r;
    sh_modserve_scope_info si;
    sh_modserve_conflict ci;
    scope_seen seen;
    unsigned char *bytes = NULL;
    size_t len = 0;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\scope2\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"r_fancy\": true}},\n  ";
    make_bundle(root, "abare000", "abare000", 'c', "\"" SC_COOP "\"", bare, 1, "{\"installedAt\": 100}");
    hash64('c', h_bare);
    make_bundle(root, "breq0000", "breq0000", 'd', "\"" SC_COOP "\"", bare, 1, "{\"installedAt\": 200}");
    hash64('d', h_req);
    g_bundle_extra = NULL;
    make_bundle(root, "xone0000", "xone0000", 'e', "\"" SC_DECL "\"", one, 1, "{\"installedAt\": 100}");
    hash64('e', h_one);
    make_bundle(root, "ytwo0000", "ytwo0000", 'f', "\"" SC_DECL "\"", two, 1, "{\"installedAt\": 200}");
    hash64('f', h_two);

    printf("-- map scope: a shadow path is served by the provider the open map declares\n");
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 4);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 0);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_lookup(SC_DECL, NULL));
    CHECK(sh_modserve_lookup_indexed(SC_COOP, &hit) && !strcmp(hit.bundle_id, "abare000"));   /* the static winner */
    CHECK(sh_modserve_conflict_info(SC_DECL, &ci) && ci.game && !strcmp(ci.served_id, "xone0000"));

    /* Only breq0000 is declared (P27: alex.bare-hands-req attached, alex.bare-hands not): its copy serves, re-parsed now. */
    list[0] = h_req;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 1);
    CHECK(sh_modserve_lookup(SC_COOP, &hit) && !strcmp(hit.bundle_id, "breq0000") && strstr(hit.disk_path, "breq0000") != NULL);
    CHECK(sh_modserve_open(SC_COOP, &bytes, &len) == 1 && len == strlen("entityDef coop { bare }") &&
          !memcmp(bytes, "entityDef coop { bare }", len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_probe_claims(SC_COOP) == 1);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "breq0000:player/coop:gone=0") != NULL);
    CHECK(sh_modserve_bundle_scope(h_req, &si) && si.shadows == 1 && si.shadows_live);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: breq0000 (0 names, 1 shadow, 0 surfaces, requirements): declared by the "
                           "open map (row 1): serves everything it carries\n") != NULL);
    CHECK(!strstr(g_report, "serving: " SC_COOP ": ") &&
          strstr(g_report, "serving: 1 path(s) several mods carry with the same bytes; one copy is used when any of them may be\n"));
    CHECK(strstr(g_report, "serving: " SC_DECL ": the game (no mod the map uses provides it); not used: xone0000, "
                           "ytwo0000\n") != NULL);
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 1 && !strcmp(g_text_val[1], "1"));
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: breq0000: cvar r_fancy=1: live\n") != NULL);
    /* abare000 asks the same value but the map does not declare it: not live for it. */
    CHECK(strstr(g_report, "requirements: abare000: cvar r_fancy=1: not active for this map (the open map does not use it; "
                           "the cvar has this value because breq0000 asks it)\n") != NULL);
    CHECK(!strstr(g_report, "abare000: cvar r_fancy=1: live"));
    /* Its gate is live now: the re-parse that waited for it runs. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "breq0000:player/coop:gone=0:gates=1;") != NULL);

    /* Moving the declaration to the other copy of the same bytes changes nothing the engine holds. */
    list[0] = h_bare;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 2);
    CHECK(sh_modserve_lookup(SC_COOP, &hit) && !strcmp(hit.bundle_id, "abare000"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!strstr(seen.order, "player/coop"));

    /* Different bytes: the declared provider's own file serves. */
    list[0] = h_one;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(sh_modserve_lookup(SC_DECL, &hit) && !strcmp(hit.bundle_id, "xone0000"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "xone0000:monster/revenant:gone=0") && strstr(seen.order, "abare000:player/coop:gone=1"));
    /* Both declared: the first of the map's rows, whatever was installed last. */
    list[0] = h_one; list[1] = h_two;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(sh_modserve_lookup(SC_DECL, &hit) && !strcmp(hit.bundle_id, "xone0000"));
    CHECK(sh_modserve_conflict_info(SC_DECL, &ci) && !ci.game && !strcmp(ci.served_id, "xone0000") &&
          !strcmp(ci.reason, "the first of the map's rows"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!strstr(seen.order, "monster/revenant"));   /* the same bytes as before */
    list[0] = h_two; list[1] = h_one;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(sh_modserve_lookup(SC_DECL, &hit) && !strcmp(hit.bundle_id, "ytwo0000"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "ytwo0000:monster/revenant:gone=0") != NULL);   /* other bytes: read again */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 1 && !sh_modserve_lookup(SC_DECL, NULL));

    /* The requirement lines after the map stops using the mods, then after it closes. */
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 0 && !strcmp(g_text_val[1], "0"));
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: breq0000: cvar r_fancy=1: not active for this map (the open map does not use it; "
                           "restored: the cvar is back at the value it displaced)\n") != NULL);
    CHECK(strstr(g_report, "requirements: abare000: cvar r_fancy=1: not active for this map (the open map does not use it)\n") != NULL);
    CHECK(!strstr(g_report, "not applied yet") && !strstr(g_report, ": live"));
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: breq0000: cvar r_fancy=1: not active for this map (no map is open; restored") != NULL);

    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* ------------------------------------------------------------------ additive phase 2 (lane L2a) */

#define AD_P     "generated/decls/entitydef/player/coop.decl"            /* a game name several keys shadow */
#define AD_HELP  "generated/decls/entitydef/mods/zlocal00/helper.decl"
#define AD_THING "generated/decls/entitydef/mods/kfork000/thing.decl"
#define AD_OLD   "generated/decls/entitydef/mods/kfork000/old_only.decl"
#define AD_NEW   "generated/decls/entitydef/mods/kfork000/new_only.decl"
#define AD_ROW   "generated/decls/snapeditorentitydef/mods/kfork000/thing.decl"

/* The serving for one manifest: which bundle serves AD_P for each declaration. */
static void ad_determinism_root(const char *root, int alpha_at, int beta_at, int local)
{
    char state[64];
    static const fx_file alpha[] = {
        { AD_P, "entityDef coop { alpha }", -1, "entityDef", "player/coop",
          "[{\"name\": \"mods/zlocal00/helper\", \"type\": \"entityDef\"}]", '1' },
    };
    static const fx_file beta[] = { { AD_P, "entityDef coop { beta }", -1, "entityDef", "player/coop", NULL, '2' } };
    static const fx_file gamma[] = { { AD_P, "entityDef coop { beta }", -1, "entityDef", "player/coop", NULL, '2' } };
    static const fx_file c1[] = { { SC_DECL, "entityDef rev { c1 }", -1, "entityDef", "monster/revenant", NULL, '4' } };
    static const fx_file c2[] = { { SC_DECL, "entityDef rev { c2 }", -1, "entityDef", "monster/revenant", NULL, '5' } };
    static const fx_file zl[] = {
        { AD_HELP, "entityDef helper {}", -1, "entityDef", "mods/zlocal00/helper", NULL, '6' },
        { AD_P, "entityDef coop { local }", -1, "entityDef", "player/coop", NULL, '7' },
        { SC_DECL, "entityDef rev { local }", -1, "entityDef", "monster/revenant", NULL, '8' },
    };
    mkdirs(root);
    _snprintf_s(state, sizeof state, _TRUNCATE, "{\"installedAt\": %d}", alpha_at);
    make_bundle(root, "alpha", "kalpha00", 'a', "\"" AD_P "\"", alpha, 1, state);
    _snprintf_s(state, sizeof state, _TRUNCATE, "{\"installedAt\": %d}", beta_at);
    make_bundle(root, "beta", "kbeta000", 'b', "\"" AD_P "\"", beta, 1, state);
    make_bundle(root, "gamma", "kgamma00", 'c', "\"" AD_P "\"", gamma, 1, "{\"installedAt\": 50}");
    _snprintf_s(state, sizeof state, _TRUNCATE, "{\"installedAt\": %d}", beta_at);
    make_bundle(root, "c1", "kc100000", 'd', "\"" SC_DECL "\"", c1, 1, state);
    _snprintf_s(state, sizeof state, _TRUNCATE, "{\"installedAt\": %d}", alpha_at);
    make_bundle(root, "c2", "kc200000", 'e', "\"" SC_DECL "\"", c2, 1, state);
    if (local) make_bundle(root, "zlocal00", "zlocal00", 'f', "\"" AD_P "\", \"" SC_DECL "\"", zl, 3, "{\"installedAt\": 999}");
}

/* The text the provider serves for `request` now, "" when nothing does. */
static const char *ad_served(const char *request)
{
    static char text[256];
    if (open_text(request, text, sizeof text) != 1) text[0] = 0;
    return text;
}

static void test_additive_determinism(void)
{
    char top[MAX_PATH], roots[2][MAX_PATH], ha[65], hb[65], hc[65], h1[65], h2[65];
    const char *list[3];
    char *slash;
    int pass;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(roots[0], sizeof roots[0], _TRUNCATE, "%s\\det1\\mods", top);
    _snprintf_s(roots[1], sizeof roots[1], _TRUNCATE, "%s\\det2\\mods", top);
    /* The same bundles, installed in the opposite order; the first machine also has an undeclared local mod. */
    ad_determinism_root(roots[0], 100, 200, 1);
    ad_determinism_root(roots[1], 300, 50, 0);
    hash64('a', ha); hash64('b', hb); hash64('c', hc); hash64('d', h1); hash64('e', h2);
    printf("-- additive: a map's serving follows from its manifest alone\n");
    sh_modserve_scope_enable(1);
    for (pass = 0; pass < 2; ++pass) {
        CHECK(sh_modserve_index_build(roots[pass]) == (pass ? 5 : 6));
        /* alpha needs a name only the undeclared local mod provides: its need
         * does not resolve against the declared set, on either machine. */
        list[0] = ha; list[1] = hb;
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(!strcmp(ad_served(AD_P), "entityDef coop { beta }"));
        list[0] = hb; list[1] = ha;
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(!strcmp(ad_served(AD_P), "entityDef coop { beta }"));
        /* Two declared providers of the bytes beta carries outnumber alpha. */
        list[0] = ha; list[1] = hc; list[2] = hb;
        CHECK(sh_modserve_scope_set(list, 3, 1) >= 0);
        CHECK(!strcmp(ad_served(AD_P), "entityDef coop { beta }"));
        /* Nothing separates c1 and c2 but the rows: the first row serves, whatever the install order. */
        list[0] = h1; list[1] = h2;
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(!strcmp(ad_served(SC_DECL), "entityDef rev { c1 }"));
        list[0] = h2; list[1] = h1;
        CHECK(sh_modserve_scope_set(list, 2, 1) >= 0);
        CHECK(!strcmp(ad_served(SC_DECL), "entityDef rev { c2 }"));
        /* The local mod's shadows never serve in a map that does not declare it. */
        list[0] = h1;
        CHECK(sh_modserve_scope_set(list, 1, 1) >= 0);
        CHECK(!strcmp(ad_served(SC_DECL), "entityDef rev { c1 }") && !ad_served(AD_P)[0]);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0);
        CHECK(!strcmp(ad_served(SC_DECL), "entityDef rev { c1 }") && !ad_served(AD_P)[0]);
        CHECK(!sh_modserve_lookup(AD_HELP, NULL));   /* not even its names, in Play */
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) >= 0);
        CHECK(sh_modserve_lookup(AD_HELP, NULL) == (pass == 0));   /* in the editor its names serve for the Toybox */
        CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    }
    sh_modserve_scope_enable(0);
}

static void test_additive_same_key(void)
{
    static const fx_file b1[] = {
        { AD_THING, "entityDef thing { one }", -1, "entityDef", "mods/kfork000/thing", NULL, '1' },
        { AD_OLD, "entityDef old {}", -1, "entityDef", "mods/kfork000/old_only", NULL, '2' },
        { AD_ROW, "{ entityDef \"mods/kfork000/thing\" }", -1, "snapEditorEntityDef", "mods/kfork000/thing", NULL, '5' },
    };
    static const fx_file b2[] = {
        { AD_THING, "entityDef thing { two }", -1, "entityDef", "mods/kfork000/thing", NULL, '3' },
        { AD_NEW, "entityDef new {}", -1, "entityDef", "mods/kfork000/new_only", NULL, '4' },
        { AD_ROW, "{ entityDef \"mods/kfork000/thing\" }", -1, "snapEditorEntityDef", "mods/kfork000/thing", NULL, '5' },
    };
    char top[MAX_PATH], root[MAX_PATH], h1[65], h2[65], h3[65], cause[400], out[4][SH_MODSERVE_HASH_HEX + 1];
    const char *list[2], *placed[1] = { "mods/kfork000/thing" };
    char *slash;
    scope_seen seen;
    sh_modserve_scope_info si;
    sh_modserve_bundle_ref ref;
    sh_modserve_plan plan;
    sh_modmanifest_row row;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\samekey\\mods", top);
    mkdirs(root);
    g_bundle_name = "Fork";
    make_bundle(root, "maps\\Fork map\\fork-a", "kfork000", '6', NULL, b1, 3, "{\"installedAt\": 100}");
    make_bundle(root, "fork-b", "kfork000", '7', NULL, b2, 3, "{\"installedAt\": 200}");
    g_bundle_name = NULL;
    hash64('6', h1); hash64('7', h2); hash64('8', h3);
    printf("-- additive: one salt at the root and in a map's place, each map served by its own build\n");
    sh_modserve_scope_enable(0);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 2);
    CHECK(log_has("MODS: mod kfork000: builds 6666666666666666 (maps\\Fork map\\fork-a) and 7777777777777777 (fork-b) both "
                  "provide paths"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    sh_modserve_scope_enable(1);
    /* No map: the root folder's build serves its names, and only its names. */
    CHECK(!strcmp(ad_served(AD_THING), "entityDef thing { two }"));
    CHECK(sh_modserve_lookup(AD_NEW, NULL) && !sh_modserve_lookup(AD_OLD, NULL));
    CHECK(modserve_bundles_for_names(placed, 1, out, 4) == 1 && !strcmp(out[0], h2));
    /* The editor plan registers the root build's row only. */
    CHECK(sh_modserve_plan_editor(&plan) >= 1);
    {
        int k, rows = 0;
        for (k = 0; k < plan.count; ++k) if (!strcmp(plan.steps[k].type, "snapEditorEntityDef")) rows++;
        CHECK(rows == 1);
    }
    sh_modserve_plan_free(&plan);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: Fork is installed 2 times: your own copy and the copies downloaded maps brought. "
                           "Each map uses its own copy.\n") != NULL);
    CHECK(strstr(g_report, "scope: Fork (3 names, 0 shadows, 0 surfaces): not declared (no map is "
                           "open): a build in maps\\Fork map; it serves only for that map\n") != NULL);
    CHECK(strstr(g_report, "scope: Fork (3 names, 0 shadows, 0 surfaces): not declared (no map is "
                           "open): its names serve for the Toybox") != NULL);

    /* Map A, whose place holds the first build, pins it: its bytes, its own
     * names; the other build is inert. */
    CHECK(sh_modserve_place_set("maps\\Fork map") == 0);
    list[0] = h1;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 2);
    CHECK(!strcmp(ad_served(AD_THING), "entityDef thing { one }"));
    CHECK(sh_modserve_lookup(AD_OLD, NULL) && !sh_modserve_lookup(AD_NEW, NULL));
    CHECK(sh_modserve_bundle_scope(h2, &si) && si.inert && !si.listed && !si.names_live);
    CHECK(sh_modserve_bundle_scope(h1, &si) && si.listed && si.rank == 0);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "kfork000:mods/kfork000/thing:gone=0") != NULL);   /* the other build's bytes: read again */
    /* The save-time closure keeps the map's pin; the row's own build answers. */
    CHECK(modserve_bundles_for_names(placed, 1, out, 4) == 1 && !strcmp(out[0], h1));
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: Fork (3 names, 0 shadows, 0 surfaces): not used here: this map "
                           "uses its own copy of this mod\n") != NULL);
    CHECK(strstr(g_report, "serving: " AD_THING ": Fork, declared (row 1): the only provider; not "
                           "used: Fork\n") != NULL);
    /* The registration plan of the map's bundle names its own paths. */
    list[0] = h1;
    CHECK(sh_modserve_plan_bundles(list, 1, &plan, NULL) >= 2);
    CHECK(step_index(&plan, "entityDef", "mods/kfork000/thing") >= 0 && step_index(&plan, "entityDef", "mods/kfork000/old_only") >= 0);
    CHECK(step_index(&plan, "entityDef", "mods/kfork000/new_only") < 0);
    sh_modserve_plan_free(&plan);
    /* In Play the same. */
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0 && !strcmp(ad_served(AD_THING), "entityDef thing { one }"));
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) >= 0);

    /* Map B pins the second build. */
    list[0] = h2;
    CHECK(sh_modserve_scope_set(list, 1, 1) == 2);
    CHECK(sh_modserve_place_set(NULL) == 0);
    CHECK(!strcmp(ad_served(AD_THING), "entityDef thing { two }"));
    CHECK(!sh_modserve_lookup(AD_OLD, NULL) && sh_modserve_lookup(AD_NEW, NULL));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "kfork000:mods/kfork000/thing:gone=0") != NULL);
    /* Both hashes in one listing (a row and a stale closure) of map A: the first row's build. */
    CHECK(sh_modserve_place_set("maps\\Fork map") >= 0);
    list[0] = h1; list[1] = h2;
    CHECK(sh_modserve_scope_set(list, 2, 1) == 2 && !strcmp(ad_served(AD_THING), "entityDef thing { one }"));
    CHECK(sh_modserve_bundle_scope(h2, &si) && si.inert);

    /* The one place resolver (DECISIONS 1 and 11): the root at the row's
     * hash first, then the map's own place at that hash, never another
     * map's place; a map without a place follows the root folder whatever
     * it pins. */
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "kfork000");
    strcpy_s(row.build, sizeof row.build, h1);
    CHECK(sh_modserve_place_set("maps\\Other map") >= 0);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING);
    CHECK(HO(cause, sh_modserve_row_served_in(&row, "Fork map", NULL, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
    CHECK(sh_modserve_place_set("maps\\Fork map") >= 0);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, h1) && !strcmp(ref.place, "maps\\Fork map"));
    strcpy_s(row.build, sizeof row.build, h2);   /* the root holds this one: the root answers first */
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, h2) && !ref.place[0]);
    strcpy_s(row.build, sizeof row.build, h3);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING);
    CHECK(!strcmp(cause, "This map uses Fork, which is not installed."));
    CHECK(!sh_modserve_row_build(&row, &ref));
    CHECK(sh_modserve_place_set(NULL) >= 0);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, h2) && !ref.place[0]);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
    /* Not a place: refused, and nothing changes. */
    CHECK(sh_modserve_place_set("maps\\..\\..\\Windows") == -1 && sh_modserve_place_set("..") == -1);
    CHECK(sh_modserve_place_set("maps\\con") == -1);
    {
        char now[HO_TEXT];
        HOS(now, sh_modserve_place_get());
        CHECK(!now[0]);
        /* A parse remembers the place it replaces; a failed one puts it back. */
        CHECK(sh_modserve_place_load("Fork map") >= 0);
        HOS(now, sh_modserve_place_get());
        CHECK(!strcmp(now, "maps\\Fork map"));
        CHECK(sh_modserve_place_restore() == 1);
        HOS(now, sh_modserve_place_get());
        CHECK(!now[0] && sh_modserve_place_restore() == 0);
    }
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
}

/* Two builds of one salt whose shared paths need different things: the
 * root folder's md6Def needs skeleton A, a map's own copy needs skeleton B and
 * a decl only that copy carries. With the map that brought its copy open, the
 * holds by name keep B (not A) and the plan orders the copy's needs first; with
 * no map the root folder's needs answer. */
#define C3_MD6 "generated/decls/md6def/mods/c3mod000/body.md6.decl"
#define C3_ENT "generated/decls/entitydef/mods/c3mod000/thing.decl"
#define C3_DEP "generated/decls/entitydef/mods/c3mod000/dep.decl"
static void test_serving_build_needs(void)
{
    static const fx_file root_files[] = {
        { C3_MD6, "md6Def body { a }", -1, "md6Def", "mods/c3mod000/body.md6",
          "[{\"type\":\"skeleton\",\"name\":\"md6/stock/a.md6skl\"}]", '1' },
        { C3_ENT, "entityDef thing { a }", -1, "entityDef", "mods/c3mod000/thing",
          "[{\"type\":\"md6Def\",\"name\":\"mods/c3mod000/body.md6\"}]", '2' },
    };
    static const fx_file place_files[] = {
        { C3_MD6, "md6Def body { b }", -1, "md6Def", "mods/c3mod000/body.md6",
          "[{\"type\":\"skeleton\",\"name\":\"md6/stock/b.md6skl\"},"
          "{\"type\":\"entityDef\",\"name\":\"mods/c3mod000/dep\"}]", '3' },
        { C3_ENT, "entityDef thing { b }", -1, "entityDef", "mods/c3mod000/thing",
          "[{\"type\":\"md6Def\",\"name\":\"mods/c3mod000/body.md6\"}]", '4' },
        { C3_DEP, "entityDef dep {}", -1, "entityDef", "mods/c3mod000/dep", NULL, '5' },
    };
    static kh_res body, ska, skb;
    static kh_res *md6s[2], *skels[2];
    static kh_list ld, ls;
    kh_list *head = &ld;
    char top[MAX_PATH], root[MAX_PATH], hroot[65], hplace[65], *slash;
    const char *list[1];
    sh_modserve_holds h;
    sh_modserve_plan plan;
    printf("-- two builds of one salt: holds and plans follow the build the open map serves\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\servingbuild\\mods", top);
    mkdirs(root);
    make_bundle(root, "c3 mod", "c3mod000", '6', NULL, root_files, 2, "{\"installedAt\": 200}");
    make_bundle(root, "maps\\C3 map\\c3 mod", "c3mod000", '7', NULL, place_files, 3, "{\"installedAt\": 100}");
    hash64('6', hroot); hash64('7', hplace);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 2);
    ld.type = "md6Def"; ld.cls = "idDeclMD6"; ld.next = &ls; ld.entries = md6s; ld.count = 1;
    ls.type = "skeleton"; ls.cls = "idMD6Skel"; ls.next = NULL; ls.entries = skels; ls.count = 2;
    body.name = "mods/c3mod000/body.md6"; md6s[0] = &body;
    ska.name = "md6/stock/a.md6skl"; skb.name = "md6/stock/b.md6skl"; skels[0] = &ska; skels[1] = &skb;
    sh_modserve_attrib_reset();
    sh_modserve_scope_enable(1);

    /* The map that brought its own copy is open: its build serves the path. */
    CHECK(sh_modserve_place_set("maps\\C3 map") >= 0);
    list[0] = hplace;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    body.level = 4; ska.level = 1; skb.level = 1;
    CHECK(sh_modserve_holds_raise(&head, &h) == 1);
    CHECK(skb.level == 4 && ska.level == 1);
    CHECK(sh_modserve_plan_bundles(list, 1, &plan, NULL) >= 3);
    CHECK(step_index(&plan, "entityDef", "mods/c3mod000/dep") >= 0 &&
          step_index(&plan, "entityDef", "mods/c3mod000/dep") < step_index(&plan, "md6Def", "mods/c3mod000/body.md6"));
    CHECK(step_index(&plan, "md6Def", "mods/c3mod000/body.md6") < step_index(&plan, "entityDef", "mods/c3mod000/thing"));
    sh_modserve_plan_free(&plan);

    /* No map: the root folder's build answers. */
    CHECK(sh_modserve_place_set(NULL) >= 0);
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    ska.level = 1; skb.level = 1;
    CHECK(sh_modserve_holds_raise(&head, &h) == 1);
    CHECK(ska.level == 4 && skb.level == 1);
    sh_modserve_scope_enable(0);
    sh_modserve_attrib_reset();
}

/* Two builds of one salt, both with requirements (V1 run 2, in the one
 * shape: the root folder's build and a map's place holding an older one).
 * Paths whose bytes the two builds share have one static winner; every step
 * must be gated by the build that serves its path for the listing, not by
 * the static winner's. */
#define TB_MD6   "generated/decls/md6def/mods/twobuild/gun.md6.decl"
#define TB_ENT   "generated/decls/entitydef/mods/twobuild/gun.decl"
#define TB_SEDEF "generated/decls/snapeditorentitydef/mods/twobuild/gun.decl"
#define TB_OLD   "generated/decls/entitydef/mods/twobuild/old_only.decl"
#define TB_NEW   "generated/decls/entitydef/mods/twobuild/new_only.decl"
static void test_two_builds_requirements(void)
{
    static const fx_file b_old[] = {
        { TB_MD6, "md6Def gun {}", -1, "md6Def", "mods/twobuild/gun.md6", NULL, '1' },
        { TB_ENT, "entityDef gun { old }", -1, "entityDef", "mods/twobuild/gun",
          "[{\"type\":\"md6Def\",\"name\":\"mods/twobuild/gun.md6\"}]", '2' },
        { TB_OLD, "entityDef old_only {}", -1, "entityDef", "mods/twobuild/old_only", NULL, '3' },
        { TB_SEDEF, "{ entityDef \"mods/twobuild/gun\" }", -1, "snapEditorEntityDef", "mods/twobuild/gun",
          "[{\"type\":\"entityDef\",\"name\":\"mods/twobuild/gun\"}]", '5' },
    };
    static const fx_file b_new[] = {
        { TB_MD6, "md6Def gun {}", -1, "md6Def", "mods/twobuild/gun.md6", NULL, '1' },
        { TB_ENT, "entityDef gun { new }", -1, "entityDef", "mods/twobuild/gun",
          "[{\"type\":\"md6Def\",\"name\":\"mods/twobuild/gun.md6\"}]", '6' },
        { TB_NEW, "entityDef new_only {}", -1, "entityDef", "mods/twobuild/new_only", NULL, '4' },
        { TB_SEDEF, "{ entityDef \"mods/twobuild/gun\" }", -1, "snapEditorEntityDef", "mods/twobuild/gun",
          "[{\"type\":\"entityDef\",\"name\":\"mods/twobuild/gun\"}]", '5' },
    };
    char top[MAX_PATH], root[MAX_PATH], h_old[65], h_new[65], h_other[65];
    const char *list[1];
    char *slash;
    int k;
    sh_modserve_plan p;
    sh_modserve_gates r;
    sh_modserve_scope_info si;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\twobuilds\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": \"0\"}},\n  ";
    make_bundle(root, "maps\\Old map\\a-owner", "twobuild", 'a', NULL, b_old, 4, "{\"installedAt\": 100}");
    make_bundle(root, "b-beside", "twobuild", 'b', NULL, b_new, 4, "{\"installedAt\": 200}");
    g_bundle_extra = NULL;
    hash64('a', h_old); hash64('b', h_new); hash64('c', h_other);
    printf("-- the root build and a place build of one salt with requirements: rows ready, gates applied once, each map its build\n");
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 2);
    sh_modserve_scope_enable(1);

    /* No map open (boot, the editor's palette): the root build's rows, all ready. */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 0);
    CHECK(sh_modserve_bundle_scope(h_new, &si) && si.newest && si.requirements_live);
    CHECK(sh_modserve_bundle_scope(h_old, &si) && !si.newest && !si.requirements_live);
    CHECK(!strcmp(ad_served(TB_ENT), "entityDef gun { new }"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_plan_editor(&p) == 3);
    CHECK(step_index(&p, "snapEditorEntityDef", "mods/twobuild/gun") >= 0);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "editor", &r) == 3);
    CHECK(p.blocked == 0 && r.live == 2 && !r.not_live && !log_has("BLOCKED"));
    for (k = 0; k < p.count; ++k) CHECK(p.steps[k].ready == 1);
    /* The gates are applied once: each cvar set once, one bundle's requirements accepted. */
    CHECK(g_gate_sets == 2 && g_gate_val[0] == 0 && g_gate_val[1] == 0);
    CHECK(log_count("MODS: editor: requirements applied:") == 1);
    sh_modserve_plan_free(&p);

    /* A map pinning the place's build: that build's bytes and names, all ready. */
    CHECK(sh_modserve_place_set("maps\\Old map") == 0);
    list[0] = h_old;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(sh_modserve_bundle_scope(h_old, &si) && si.listed && si.requirements_live);
    CHECK(sh_modserve_bundle_scope(h_new, &si) && si.inert && !si.requirements_live);
    CHECK(!strcmp(ad_served(TB_ENT), "entityDef gun { old }"));
    CHECK(sh_modserve_lookup(TB_OLD, NULL) && !sh_modserve_lookup(TB_NEW, NULL));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_plan_bundles(list, 1, &p, NULL) == 3);
    CHECK(step_index(&p, "entityDef", "mods/twobuild/old_only") >= 0 && step_index(&p, "entityDef", "mods/twobuild/new_only") < 0);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "map load", &r) == 3);
    CHECK(p.blocked == 0 && r.live == 2 && !log_has("BLOCKED"));
    CHECK(g_gate_sets == 2);
    sh_modserve_plan_free(&p);

    /* A map pinning the root build: the same, the other way round. */
    CHECK(sh_modserve_place_set(NULL) >= 0);
    list[0] = h_new;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(!strcmp(ad_served(TB_ENT), "entityDef gun { new }"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_plan_bundles(list, 1, &p, NULL) == 3);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "map load", &r) == 3);
    CHECK(p.blocked == 0 && !log_has("BLOCKED"));
    sh_modserve_plan_free(&p);

    /* A map pinning a third build that is not installed declares nothing
     * here (the listing holds the builds that serve its rows): the editor
     * serves the root build's names as with no map open. */
    list[0] = h_other;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 0);
    CHECK(!strcmp(ad_served(TB_ENT), "entityDef gun { new }"));
    CHECK(sh_modserve_bundle_scope(h_new, &si) && !si.listed && si.requirements_live);
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* Derived edits among the declared bundles only (DESIGN-additive 2.4): each
 * bundle's edited copy of the conductor is never served; the game's text with
 * the declared bundles' edits is, never an undeclared local mod's. */
/* The game's conductor, the text the edits apply to (modregistry.h). */
#define CONDUCTOR_BASE CONDUCTOR_HEAD "\t\t\tnum = 2;\r\n" CONDUCTOR_STOCK_ITEMS CONDUCTOR_TAIL
static int cs_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    (void)c;
    if (_stricmp(source, CONDUCTOR)) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex((const unsigned char *)CONDUCTOR_BASE, strlen(CONDUCTOR_BASE), sha);
    *size = strlen(CONDUCTOR_BASE);
    return SH_MB_STOCK_OK;
}
static int cs_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    (void)c; (void)sha;
    if (_stricmp(source, CONDUCTOR)) return SH_MB_STOCK_ABSENT;
    *len = strlen(CONDUCTOR_BASE);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len);
    memcpy(*bytes, CONDUCTOR_BASE, *len);
    return SH_MB_STOCK_OK;
}

static void test_additive_compose(void)
{
    sh_mb_stock_ops cops;
    static const fx_file cy[] = { { CONDUCTOR, CONDUCTOR_CYBER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'c' } };
    static const fx_file gd[] = { { CONDUCTOR, CONDUCTOR_GUARD, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'd' } };
    static const fx_file sp[] = { { CONDUCTOR, CONDUCTOR_SPIDER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'e' } };
    char top[MAX_PATH], root[MAX_PATH], hc[65], hg[65], text[4096];
    const char *list[2];
    char *slash;
    sh_modserve_conflict ci;
    scope_seen seen;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\compose\\mods", top);
    mkdirs(root);
    {
        /* Each bundle's edits, as the builder derives them from its copy. */
        static const char *const copies[3] = { CONDUCTOR_CYBER, CONDUCTOR_GUARD, CONDUCTOR_SPIDER };
        static char edits[3][4096];
        int k;
        for (k = 0; k < 3; ++k) {
            char cause[200], *one = sh_dt_diff(CONDUCTOR, CONDUCTOR_BASE, strlen(CONDUCTOR_BASE), copies[k], strlen(copies[k]),
                                              NULL, cause, sizeof cause);
            CHECK(one != NULL);
            _snprintf_s(edits[k], sizeof edits[k], _TRUNCATE, "[%s]", one ? one : "");
            free(one);
        }
        g_block_edits = edits[0];
        make_bundle(root, "cy", "kcy00000", '1', "\"" CONDUCTOR "\"", cy, 1, "{\"installedAt\": 100}");
        g_block_edits = edits[1];
        make_bundle(root, "gd", "kgd00000", '2', "\"" CONDUCTOR "\"", gd, 1, "{\"installedAt\": 200}");
        g_block_edits = edits[2];
        make_bundle(root, "sp", "ksp00000", '3', "\"" CONDUCTOR "\"", sp, 1, "{\"installedAt\": 300}");
        g_block_edits = NULL;
    }
    hash64('1', hc); hash64('2', hg);
    printf("-- additive: lists compose among the declared bundles only\n");
    memset(&cops, 0, sizeof cops);
    cops.identify = cs_identify;
    cops.read = cs_read;
    sh_mb_set_stock_ops(&cops);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 3);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    /* Off: nothing is generated and no copy is served; the game's file answers. */
    CHECK(open_text(CONDUCTOR, text, sizeof text) != 1);
    sh_modserve_scope_enable(1);
    list[0] = hc; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 2);
    CHECK(open_text(CONDUCTOR, text, sizeof text) == 1 && strstr(text, "cyberdem_hell") && strstr(text, "talismanguard/hammer") &&
          !strstr(text, "spider_mastermind_base"));
    /* The conductor is generated (modregistry.h): the game's list plus the
     * declared bundles' items, the spawn-type bits assigned per map; the
     * copies themselves are no conflict, since none is served. */
    CHECK(!sh_modserve_conflict_info(CONDUCTOR, &ci));
    /* One declared: the game's list plus its rows, at the bits this map assigns. */
    list[0] = hg;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(open_text(CONDUCTOR, text, sizeof text) == 1 && strstr(text, "num = 4;") &&
          strstr(text, "aiType = \"524288\";\r\n\t\t\t\tentityDef = \"ai/demon/talismanguard/hammer\"") &&
          strstr(text, "aiType = \"1048576\";\r\n\t\t\t\tentityDef = \"ai/demon/talismanguard/staff\"") &&
          !strstr(text, "cyberdem"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "ai/conductor/coop/snapmap_default:gone=0") != NULL);   /* other bytes than the composed text */
    /* The same pair again: the memoized text, the same bytes. */
    list[0] = hc; list[1] = hg;
    CHECK(sh_modserve_scope_set(list, 2, 1) >= 1);
    CHECK(open_text(CONDUCTOR, text, sizeof text) == 1 && strstr(text, "talismanguard/hammer") && !strstr(text, "spider_mastermind"));
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    /* Converted first-generation packages (their Toybox rows shadow editor
     * decls) follow the declared rule like any mod (owner 2026-11-09): in
     * Play of a map that declares neither, nothing of them serves; a map
     * that declares both composes them. */
    {
        static const fx_file lg1[] = {
            { CONDUCTOR, CONDUCTOR_GUARD, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'd' },
            { "generated/decls/snapeditorentitydef/demons/guard_enc.decl", "{ }", -1, "snapEditorEntityDef", "demons/guard_enc", NULL, '6' },
        };
        static const fx_file lg2[] = {
            { CONDUCTOR, CONDUCTOR_SPIDER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'e' },
            { "generated/decls/snapeditorentitydef/demons/spider_enc.decl", "{ }", -1, "snapEditorEntityDef", "demons/spider_enc", NULL, '7' },
        };
        char groot[MAX_PATH];
        _snprintf_s(groot, sizeof groot, _TRUNCATE, "%s\\compose-legacy\\mods", top);
        mkdirs(groot);
        make_bundle(groot, "localgua", "localgua", '4',
                    "\"" CONDUCTOR "\", \"generated/decls/snapeditorentitydef/demons/guard_enc.decl\"", lg1, 2, NULL);
        make_bundle(groot, "localsp1", "localsp1", '5',
                    "\"" CONDUCTOR "\", \"generated/decls/snapeditorentitydef/demons/spider_enc.decl\"", lg2, 2, NULL);
        g_bundle_extra = NULL;
        char h4[65], h5[65];
        const char *both[2];
        sh_modserve_scope_info gi;
        CHECK(sh_modserve_index_build(groot) == 2);
        hash64('4', h4); hash64('5', h5);
        CHECK(sh_modserve_bundle_scope(h4, &gi) && gi.scope == SH_MODSERVE_SCOPE_ALWAYS);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0 && sh_modserve_scope_set(NULL, 0, 1) >= 0);
        CHECK(open_text(CONDUCTOR, text, sizeof text) != 1 || (!strstr(text, "talismanguard/staff") && !strstr(text, "spider_mastermind_base")));
        CHECK(sh_modserve_bundle_scope(h4, &gi) && !gi.names_live);
        both[0] = h4; both[1] = h5;
        CHECK(sh_modserve_scope_set(both, 2, 1) >= 1);
        CHECK(open_text(CONDUCTOR, text, sizeof text) == 1 && strstr(text, "talismanguard/staff") && strstr(text, "spider_mastermind_base"));
        CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    }
    sh_modserve_scope_enable(0);
    sh_mb_set_stock_ops(NULL);
}

/* ONE-SHAPE rules 1, 2, 4 and 6 at the serving index: a mod is its salt,
 * whatever its folder is called; the root holds one build per salt and each
 * map's place one; a stranger's map is served by its place's build at the
 * row's hash, else the root's when it is at that hash, the author's own map
 * by the root's at any hash, and either by any place's at the row's hash; a
 * folder in an earlier shape or whose build failed serves nothing, with a
 * plain cause. */
#define SP_THING "generated/decls/entitydef/mods/kown0000/thing.decl"
static void sp_modjson(const char *root, const char *folder, const char *text)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", root, folder);
    write_file(path, text, strlen(text));
}

static void test_salt_and_place(void)
{
    static const fx_file mine[] = {
        { SP_THING, "entityDef thing { mine }", -1, "entityDef", "mods/kown0000/thing", NULL, '1' },
    };
    static const fx_file theirs[] = {
        { SP_THING, "entityDef thing { downloaded }", -1, "entityDef", "mods/kown0000/thing", NULL, '2' },
    };
    static const fx_file stray[] = {
        { SP_THING, "entityDef thing { stray }", -1, "entityDef", "mods/kown0000/thing", NULL, '3' },
    };
    char top[MAX_PATH], root[MAX_PATH], path[MAX_PATH], hm[65], hd[65], hx[65], cause[400];
    char *slash;
    sh_modserve_bundle_ref ref;
    sh_modserve_scope_info si;
    sh_modserve_hit hit;
    sh_modmanifest_row row;
    const char *list[1];
    printf("-- one shape: the salt, the root and maps' places, earlier shapes refused\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\saltplace\\mods", top);
    mkdirs(root);
    hash64('a', hm); hash64('b', hd); hash64('c', hx);
    /* The author's folder, named as they like; the build a downloaded map
     * pinned, in its place and in a second map's place. */
    g_bundle_name = "Doors Pack";
    make_bundle(root, "My doors (work)", "kown0000", 'a', NULL, mine, 1, "{\"installedAt\": 100}");
    make_bundle(root, "maps\\Doors\\Doors Pack", "kown0000", 'b', NULL, theirs, 1, "{\"installedAt\": 900}");
    make_bundle(root, "maps\\Gates\\Doors Pack", "kown0000", 'b', NULL, theirs, 1, "{\"installedAt\": 950}");
    /* A copy of the author's folder (same salt, built later) and a second
     * folder of the salt in one map's place: one build per salt per area. */
    make_bundle(root, "My doors (copy)", "kown0000", 'e', NULL, stray, 1, "{\"installedAt\": 300}");
    make_bundle(root, "maps\\Doors\\Doors Pack (2)", "kown0000", 'f', NULL, stray, 1, "{\"installedAt\": 990}");
    g_bundle_name = NULL;
    /* Earlier shapes and unbuilt or failed folders. */
    sp_modjson(root, "format-one", "{\"name\": \"Old\", \"snapmap-plus\": {\"format\": 1, \"key\": \"old\", \"bundle\": \""
               "0000000000000000000000000000000000000000000000000000000000000000\", \"files\": []}}");
    sp_modjson(root, "retired-id", "{\"id\": \"local.thing\", \"name\": \"Retired\"}");
    sp_modjson(root, "hud-schema", "{\"name\": \"Hud\", \"hud\": {\"schema\": 1}}");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\package-only\\package.json", root);
    write_file(path, "{}", 2);
    sp_modjson(root, "bad-salt", "{\"name\": \"Bad\", \"snapmap-plus\": {\"format\": 2, \"salt\": \"1abcdefg\"}}");
    sp_modjson(root, "salted-only", "{\"name\": \"Salted\", \"snapmap-plus\": {\"format\": 2, \"salt\": \"saltonly\"}}");
    sp_modjson(root, "failed", "{\"name\": \"Failed\", \"snapmap-plus\": {\"format\": 2, \"salt\": \"failedmd\", "
               "\"rules\": 8, \"cause\": \"generated/decls/entitydef/mods/failedmd/a.decl: parse error\"}}");
    sp_modjson(root, "fresh", "{\"name\": \"Fresh\"}");
    sp_modjson(root, "maps\\Doors\\fresh", "{\"name\": \"Fresh\"}");
    {
        /* A build whose files sit under another salt's namespace. */
        static const fx_file other[] = {
            { SP_THING, "entityDef thing { other }", -1, "entityDef", "mods/kown0000/thing", NULL, '4' },
        };
        make_bundle(root, "elsewhere", "zother00", 'd', NULL, other, 1, NULL);
    }
    sh_modserve_scope_enable(0);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 3);
    CHECK(log_has("bundle 'My doors (copy)' skipped: it is a copy of mods\\My doors (work) (the same salt kown0000); Refresh "
                  "in the Mods tab makes it a new mod"));
    CHECK(log_has("bundle 'maps\\Doors\\Doors Pack (2)' skipped: This map's folder holds this mod twice (also in "
                  "maps\\Doors\\Doors Pack). Delete one of the two folders."));
    CHECK(log_has("bundle 'format-one' skipped: " "This mod was made for an earlier version of Snapmap+. Run the Snapmap+ installer again and it converts it."));
    CHECK(log_has("bundle 'retired-id' skipped: " SH_MB_EARLIER));
    CHECK(log_has("bundle 'hud-schema' skipped: " SH_MB_EARLIER));
    CHECK(log_has("bundle 'package-only' skipped: " "This mod was made for an earlier version of Snapmap+. Run the Snapmap+ installer again and it converts it."));
    CHECK(log_has("bundle 'bad-salt' skipped: The part of mod.json Snapmap+ writes is damaged. Delete the \"snapmap-plus\" "
                  "block and press Refresh to build the mod again."));
    CHECK(log_has("bundle 'salted-only' skipped: not built yet (Refresh in the Mods tab builds it)"));
    CHECK(log_has("bundle 'failed' skipped: not built: generated/decls/entitydef/mods/failedmd/a.decl: parse error"));
    CHECK(log_has("bundle 'fresh' skipped: not built yet (Refresh in the Mods tab builds it)"));
    CHECK(log_has("bundle 'maps\\Doors\\fresh' skipped: " SH_MODSERVE_PLACE_FRESH));
    CHECK(log_has("bundle 'elsewhere' skipped: '" SP_THING "' is outside mods/zother00/ (its salt)"));
    CHECK(!log_has("repair"));
    sh_modserve_scope_enable(1);

    /* No map: the root folder's build serves; a place build only for its map. */
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    CHECK(!strcmp(ad_served(SP_THING), "entityDef thing { mine }"));
    CHECK(sh_modserve_bundle_scope(hm, &si) && si.newest && !si.in_place && si.names_live);
    CHECK(sh_modserve_bundle_scope(hd, &si) && !si.newest && si.in_place && !si.names_live);

    /* Rows of a map with a place: the root at the row's hash; another map's
     * place never serves it, and another hash is missing (the root serves
     * such a map only at the row's hash). */
    CHECK(sh_modserve_place_set("maps\\Elsewhere") >= 0);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "kown0000");
    row.name = (char *)"Doors Pack";   /* borrowed: this test never frees the row */
    strcpy_s(row.build, sizeof row.build, hm);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, hm) && !ref.place[0] && !strcmp(ref.id, "kown0000") &&
          !strcmp(ref.folder, "My doors (work)") && !strcmp(ref.name, "Doors Pack"));
    strcpy_s(row.build, sizeof row.build, hd);
    CHECK(!sh_modserve_row_build(&row, &ref));
    strcpy_s(row.build, sizeof row.build, hx);
    CHECK(!sh_modserve_row_build(&row, &ref));
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING &&
          !strcmp(cause, "This map uses Doors Pack, which is not installed."));
    /* In a map's place: that place's folder first (the segment alone names it too). */
    CHECK(sh_modserve_place_set("maps\\Gates") >= 0);
    strcpy_s(row.build, sizeof row.build, hd);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.place, "maps\\Gates") && !strcmp(ref.folder, "maps\\Gates\\Doors Pack"));
    CHECK(sh_modserve_place_set("Doors") >= 0);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.place, "maps\\Doors"));
    strcpy_s(row.build, sizeof row.build, hm);   /* the place does not hold it: the root at that hash */
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, hm) && !ref.place[0]);
    strcpy_s(row.build, sizeof row.build, hx);
    CHECK(!sh_modserve_row_build(&row, &ref));
    /* The map's listing serves the place's folder of the declared build. */
    list[0] = hd;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 1);
    CHECK(!strcmp(ad_served(SP_THING), "entityDef thing { downloaded }"));
    CHECK(sh_modserve_lookup(SP_THING, &hit) && strstr(hit.disk_path, "\\maps\\Doors\\Doors Pack\\"));
    CHECK(sh_modserve_bundle_scope(hm, &si) && si.inert);
    CHECK(sh_modserve_place_set("maps\\Gates\\") >= 0);   /* the same build from the other place */
    CHECK(sh_modserve_lookup(SP_THING, &hit) && strstr(hit.disk_path, "\\maps\\Gates\\Doors Pack\\"));
    /* A map without a place (the player's own) follows the root folder
     * whatever it pins. */
    CHECK(sh_modserve_place_set(NULL) >= 0);
    strcpy_s(row.build, sizeof row.build, hd);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, hm));
    strcpy_s(row.build, sizeof row.build, hx);
    CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.hash, hm));
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
    /* A salt whose only folder is skipped: the folder's cause. */
    CHECK(sh_modserve_place_set(NULL) >= 0);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "saltonly");
    strcpy_s(row.build, sizeof row.build, hx);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING &&
          !strcmp(cause, "This map uses Salted, which is installed but cannot be used: not built yet (Refresh in the Mods tab "
                         "builds it)."));
    /* A row that names no salt (an earlier shape the detour let through). */
    memset(&row, 0, sizeof row);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING &&
          !strcmp(cause, "This map names a mod this version does not read."));
    CHECK(sh_modserve_scope_set(NULL, 0, 0) >= 0);
    sh_modserve_scope_enable(0);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\saltplace", top);
    remove_tree(path);
    CHECK(sh_modserve_index_build(g_root) >= 0);
}

/* The list walks take the engine's resource lock; a callback runs after it is released. */
static CRITICAL_SECTION g_fake_cs;
static CRITICAL_SECTION *g_fake_lock_slot = &g_fake_cs;
static HANDLE g_lock_taken;
static int g_held_in_cb, g_cb_calls;
static void ad_lock_cb(void *context, const char *list_class, void *resource, const char *name)
{
    (void)context; (void)list_class; (void)resource; (void)name;
    g_cb_calls++;
    if (g_fake_cs.RecursionCount > 0 && (DWORD)(uintptr_t)g_fake_cs.OwningThread == GetCurrentThreadId()) g_held_in_cb++;
}
static DWORD WINAPI ad_lock_holder(LPVOID p)
{
    (void)p;
    EnterCriticalSection(&g_fake_cs);
    SetEvent(g_lock_taken);
    Sleep(250);
    LeaveCriticalSection(&g_fake_cs);
    return 0;
}

static void test_resource_lock(void)
{
    typedef struct named_res { unsigned char pad[8]; const char *name; unsigned char rest[0x30]; } named_res;
    static named_res nr[2];
    static fake_res *ents[2];
    static fake_list la;
    fake_list *head = &la;
    sh_modserve_watermark w;
    HANDLE t;
    double t0;
    printf("-- additive: the resource list walks hold the engine's lock\n");
    InitializeCriticalSection(&g_fake_cs);
    nr[0].name = "a"; nr[1].name = "b";
    ents[0] = (fake_res *)&nr[0]; ents[1] = (fake_res *)&nr[1];
    la.name = "anim"; la.next = NULL; la.entries = ents; la.count = 2;
    sh_modserve_set_resource_lock(&g_fake_lock_slot);
    g_cb_calls = g_held_in_cb = 0;
    CHECK(sh_modserve_resource_each(&head, ad_lock_cb, NULL) == 1 && g_cb_calls == 2 && g_held_in_cb == 0);
    CHECK(g_fake_cs.RecursionCount == 0);
    /* Another thread holds the lock (a list add in flight): the walk waits for it. */
    g_lock_taken = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, ad_lock_holder, NULL, 0, NULL);
    WaitForSingleObject(g_lock_taken, 5000);
    t0 = now_ns();
    CHECK(sh_modserve_watermark_take(&head, &w) == 1 && w.n == 2);
    CHECK(now_ns() - t0 >= 150e6);
    sh_modserve_watermark_free(&w);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    CloseHandle(g_lock_taken);
    CHECK(g_fake_cs.RecursionCount == 0);
    sh_modserve_set_resource_lock(NULL);
    DeleteCriticalSection(&g_fake_cs);
}

/* The asset rule (sh_modserve_row_served): a row is served by the admitted
 * folder at its hash that provides every name it lists; missing with no
 * folder at that hash (worded as any missing mod, also when the key is
 * installed at another hash); incomplete, naming the first missing names,
 * when the folder lacks one. */
static void test_row_served(void)
{
    char tmp[MAX_PATH], root[MAX_PATH], top[MAX_PATH], cause[400], *slash;
    sh_modmanifest_row row;
    char *names[5];
    static const fx_file HG[] = {
        { "generated/decls/entitydef/mods/localhel/hell_guard_combo_enc.decl", "{ inherit = \"x\"; }", -1,
          "entityDef", "mods/localhel/hell_guard_combo_enc", NULL, '1' },
        { "generated/decls/entitydef/mods/localhel/hell_guard_shield.decl", "{ inherit = \"y\"; }", -1,
          "entityDef", "mods/localhel/hell_guard_shield", NULL, '2' },
    };
    static const fx_file HG_NEW[] = {   /* recreated: the same paths, new bytes */
        { "generated/decls/entitydef/mods/localhel/hell_guard_combo_enc.decl", "{ inherit = \"x2\"; }", -1,
          "entityDef", "mods/localhel/hell_guard_combo_enc", NULL, '3' },
        { "generated/decls/entitydef/mods/localhel/hell_guard_shield.decl", "{ inherit = \"y2\"; }", -1,
          "entityDef", "mods/localhel/hell_guard_shield", NULL, '4' },
    };
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%smodserve_rows_%lu\\mods", tmp, GetCurrentProcessId());
    strcpy_s(top, sizeof top, root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    remove_tree(top);
    mkdirs(root);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "localhel");
    hash64('d', row.build);                 /* the build the map was saved with */
    names[0] = "mods/localhel/hell_guard_combo_enc";
    names[1] = "MODS/localhel/hell_guard_shield";   /* names fold case */
    row.names = names;
    row.name_count = 2;
    /* A map with a place: the root serves it only at the row's build. */
    CHECK(sh_modserve_place_set("maps\\Their map") >= 0);

    /* No folder of the salt: missing. */
    CHECK(sh_modserve_index_build(root) >= 0);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING);
    CHECK(!strcmp(cause, "This map uses a mod that is not installed."));
    CHECK(HO(cause, sh_modserve_row_served_for(&row, "prefab", HO_P(0))) == SH_MODSERVE_ROW_MISSING &&
          !strcmp(cause, "This prefab uses a mod that is not installed."));

    /* The folder at the row's hash: served. */
    g_bundle_name = "Hell Guards";
    make_bundle(root, "localhel", "localhel", 'd', NULL, HG, 2, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED && !cause[0]);

    /* Recreated with the same salt and paths but new bytes (a new hash): another build, not this map's. */
    remove_tree(top);
    mkdirs(root);
    make_bundle(root, "hand-made", "localhel", 'e', NULL, HG_NEW, 2, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_MISSING);
    CHECK(!strcmp(cause, "This map uses Hell Guards, which is not installed."));
    /* The player's own map saved before the author edited and rebuilt the
     * folder: it follows the root at any build, in the editor, at Play and
     * from My Maps alike (no place: the root). */
    {
        sh_modserve_bundle_ref ref;
        char e_hash[65];
        hash64('e', e_hash);
        CHECK(HO(cause, sh_modserve_row_served_in(&row, "", "map", HO_P(0))) == SH_MODSERVE_ROW_SERVED && !cause[0]);
        CHECK(sh_modserve_row_build_in(&row, "", &ref) && !strcmp(ref.hash, e_hash) && !strcmp(ref.folder, "hand-made"));
        CHECK(sh_modserve_place_set(NULL) >= 0);
        CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0);
        CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
        CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) >= 0);
        CHECK(sh_modserve_place_set("maps\\Their map") >= 0);
    }

    /* The folder at the hash that lacks one listed path: incomplete, naming it. */
    remove_tree(top);
    mkdirs(root);
    make_bundle(root, "localhel", "localhel", 'd', NULL, HG + 1, 1, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_INCOMPLETE);
    /* Named under the mod, never by its salt. */
    CHECK(!strcmp(cause, "Hell Guards is installed but does not provide "
                         "hell_guard_combo_enc, which this map uses."));
    /* More than three missing: the first three and the count. */
    names[2] = "mods/localhel/a";
    names[3] = "mods/localhel/b";
    names[4] = "mods/localhel/c";
    row.name_count = 5;
    CHECK(HO(cause, sh_modserve_row_served_for(&row, "prefab", HO_P(0))) == SH_MODSERVE_ROW_INCOMPLETE);
    CHECK(!strcmp(cause, "Hell Guards is installed but does not provide "
                         "hell_guard_combo_enc, a, b and 1 more, "
                         "which this prefab uses."));
    /* A row that lists no names (a map-level row) is served by its build alone. */
    row.name_count = 0;
    CHECK(HO(cause, sh_modserve_row_served(&row, HO_P(0))) == SH_MODSERVE_ROW_SERVED);
    /* Another salt's folder providing the names does not serve the row. */
    remove_tree(top);
    mkdirs(root);
    g_bundle_name = NULL;
    row.name_count = 2;
    {
        static const fx_file OTHER[] = {
            { "generated/decls/entitydef/mods/localoth/x.decl", "{ }", -1, "entityDef", "mods/localoth/x", NULL, '5' },
        };
        make_bundle(root, "localoth", "localoth", 'c', NULL, OTHER, 1, NULL);
    }
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_row_served(&row, NULL) == SH_MODSERVE_ROW_MISSING);
    CHECK(sh_modserve_place_set(NULL) >= 0);
    remove_tree(top);
    CHECK(sh_modserve_index_build(g_root) >= 0);
}

/* The names a map's placed entities give the save closure: a model an edit
 * state names reaches the bundle that carries or unlocks it; a shadow of
 * editor-time content names no bundle; a gameplay shadow still does. */
typedef struct dep_seen { char names[512]; int bundles; } dep_seen;
static void dep_visit(void *context, const char *id, const char *hash, unsigned long long size,
                      const char *name)
{
    dep_seen *s = (dep_seen *)context;
    (void)hash; (void)size;
    if (name) { strcat_s(s->names, sizeof s->names, name); strcat_s(s->names, sizeof s->names, ";"); }
    else s->bundles++;
    (void)id;
}

static void test_dependency_names(void)
{
    char top[MAX_PATH], root[MAX_PATH], out[4][SH_MODSERVE_HASH_HEX + 1], ha[65], hc[65];
    char *slash;
    dep_seen seen;
    static const fx_file ivy[] = {
        { "cooked/model/models/modular/airlock/air_lock_shell.bmodel", "AIRLOCKBYTES", -1, "model",
          "cooked/model/models/modular/airlock/air_lock_shell.bmodel", NULL, 'a' },
    };
    static const fx_file jo[] = { { LIFT, "JOLIFT", -1, "snapEditorEntityDef", "func/lift", NULL, 'b' } };
    static const fx_file kay[] = { { SHADOW, "entityDef kay {}", -1, "entityDef", "monster/imp", NULL, 'c' } };
    printf("-- dependency names: models in edit state, editor-time shadows\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\deps\\mods", top);
    mkdirs(root);
    g_bundle_extra = NULL;
    make_bundle(root, "ivyairlo", "ivyairlo", 'a',
                "\"cooked/model/models/modular/airlock/air_lock_shell.bmodel\"", ivy, 1, NULL);
    make_bundle(root, "joeditor", "joeditor", 'b', "\"" LIFT "\"", jo, 1, NULL);
    make_bundle(root, "kaygame0", "kaygame0", 'c', "\"" SHADOW "\"", kay, 1, NULL);
    hash64('a', ha); hash64('c', hc);
    CHECK(sh_modserve_index_build(root) == 3);

    {
        const char *lwo[] = { "models/modular/airlock/air_lock_shell.lwo" };
        const char *upper[] = { "MODELS/Modular/Airlock/Air_Lock_Shell.LWO" };
        const char *cooked[] = { "cooked/model/models/modular/airlock/air_lock_shell.bmodel" };
        const char *other[] = { "models/modular/other/thing.lwo" };
        const char *lift[] = { "func/lift" };
        const char *imp[] = { "monster/imp" };
        const char *all[] = { "func/lift", "models/modular/airlock/air_lock_shell.lwo", "monster/imp", "snapmaps/props/stock" };
        CHECK(modserve_bundles_for_names(lwo, 1, out, 4) == 1 && !strcmp(out[0], ha));
        CHECK(modserve_bundles_for_names(upper, 1, out, 4) == 1 && !strcmp(out[0], ha));
        CHECK(modserve_bundles_for_names(cooked, 1, out, 4) == 1 && !strcmp(out[0], ha));
        CHECK(modserve_bundles_for_names(other, 1, out, 4) == 0);
        CHECK(modserve_bundles_for_names(lift, 1, out, 4) == 0);   /* an editor-time shadow is no dependency */
        CHECK(modserve_bundles_for_names(imp, 1, out, 4) == 1 && !strcmp(out[0], hc));
        memset(&seen, 0, sizeof seen);
        CHECK(modserve_bundles_for_names_visit(all, 4, dep_visit, &seen) == 2);
        CHECK(strstr(seen.names, "models/modular/airlock/air_lock_shell.lwo;") && strstr(seen.names, "monster/imp;"));
        CHECK(!strstr(seen.names, "func/lift") && !strstr(seen.names, "stock") && seen.bundles == 0);
    }
    CHECK(sh_modserve_index_build(g_root) >= 6);
}

/* Per-build bank stems (DESIGN-additive 6): a bundle's bank is served under
 * <stem>.<hash16>.bnk with its BKHD id rewritten, and its decl naming the
 * bank follows; another build of the key is another bank to Wwise. */
static void test_bank_stems(void)
{
    static const char DECL[] = "sound mods/bbank000/hit {\r\n\tbank = \"bbank000_sfx.bnk\";\r\n\tevent = \"play_hit\";\r\n}";
    static const fx_file files[] = {
        { "sound/soundbanks/pc/bbank000_sfx.bnk", NULL, 24, "soundBank", "sound/soundbanks/pc/bbank000_sfx.bnk", NULL, '1' },
        { "generated/decls/sound/mods/bbank000/hit.decl", DECL, -1, "sound", "mods/bbank000/hit", NULL, '2' },
    };
    char top[MAX_PATH], root[MAX_PATH], path[MAX_PATH], req[160], h[65], stem[64];
    unsigned char bank[24], *bytes = NULL;
    size_t len = 0;
    uint32_t v, id;
    char *slash, *t;
    int pass;
    printf("-- additive: bank stems keyed by the bank's own hash\n");
    /* The text rewrite alone. */
    t = sh_modserve_bank_rename_text("a = \"bbank000_sfx\"; c = \"x/bbank000_sfx.bnk\"; d = \"BBANK000_SFX.bnk\"; e = \"bbank000_sfxy\";",
                                     strlen("a = \"bbank000_sfx\"; c = \"x/bbank000_sfx.bnk\"; d = \"BBANK000_SFX.bnk\"; e = \"bbank000_sfxy\";"),
                                     "bbank000_sfx", "bbank000_sfx.1111111111111111", &len);
    CHECK(t && !strcmp(t, "a = \"bbank000_sfx.1111111111111111\"; c = \"x/bbank000_sfx.1111111111111111.bnk\"; "
                          "d = \"bbank000_sfx.1111111111111111.bnk\"; e = \"bbank000_sfxy\";"));
    free(t);
    CHECK(sh_modserve_bank_rename_text("nothing here", 12, "bbank000_sfx", "bbank000_sfx.1", &len) == NULL);
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\banks\\mods", top);
    mkdirs(root);
    for (pass = 0; pass < 2; pass++) {
        char digit = pass ? '6' : '5';
        memset(bank, 0, sizeof bank);
        memcpy(bank, "BKHD", 4);
        v = 16; memcpy(bank + 4, &v, 4);
        v = 0x71; memcpy(bank + 8, &v, 4);
        v = sh_mb_fnv1_lower("bbank000_sfx", 12); memcpy(bank + 12, &v, 4);
        make_bundle(root, "bbank000", "bbank000", digit, NULL, files, 2, NULL);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bbank000\\assets\\sound\\soundbanks\\pc\\bbank000_sfx.bnk", root);
        write_file(path, (const char *)bank, sizeof bank);
        sh_modserve_scope_enable(0);
        log_clear();
        CHECK(sh_modserve_index_build(root) == 1);
        /* The bank file's own hash names it: both builds carry the same bank. */
        _snprintf_s(stem, sizeof stem, _TRUNCATE, "bbank000_sfx.%.16s", hash64('1', h));
        _snprintf_s(req, sizeof req, _TRUNCATE, "sound/soundbanks/pc/%s.bnk", stem);
        CHECK(!sh_modserve_lookup("sound/soundbanks/pc/bbank000_sfx.bnk", NULL));
        {
            sh_modserve_hit hit;
            CHECK(sh_modserve_lookup(req, &hit) && hit.disk_path && strstr(hit.disk_path, "\\.served\\banks\\bbank000_sfx.1111111111111111.bnk") && hit.size == 24);
        }
        CHECK(log_has("bank bbank000_sfx.bnk served as bbank000_sfx."));
        CHECK(sh_modserve_open(req, &bytes, &len) == 1 && len == 24 && !memcmp(bytes, "BKHD", 4));
        if (bytes) {
            memcpy(&id, bytes + 12, 4);
            CHECK(id == sh_mb_fnv1_lower(stem, strlen(stem)) && id != sh_mb_fnv1_lower("bbank000_sfx", 12));
            HeapFree(GetProcessHeap(), 0, bytes);
            bytes = NULL;
        }
        {
            char want[200];
            _snprintf_s(want, sizeof want, _TRUNCATE, "bank = \"%s.bnk\";", stem);
            CHECK(strstr(ad_served("generated/decls/sound/mods/bbank000/hit.decl"), want) != NULL);
        }
    }
    /* The served copies no build needs any more, and a temporary a crashed
     * write left, are removed; the bank the table serves stays. */
    {
        char stray[MAX_PATH], tmp[MAX_PATH], live[MAX_PATH];
        sh_modserve_hit hit;
        CHECK(sh_modserve_lookup(req, &hit) && hit.disk_path);
        strcpy_s(live, sizeof live, hit.disk_path);
        _snprintf_s(stray, sizeof stray, _TRUNCATE, "%s%s", root, "/.served/files/0123456789abcdef.decl");
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.77.tmp", live);
        for (t = stray; *t; ++t) if (*t == '/') *t = '\\';
        write_file(stray, "old", 3);
        write_file(tmp, "part", 4);
        {
            /* A game serving another mods folder (the player's own, under
             * the name every game used before, or a mark named for another
             * folder) does not stop the prune of this one. */
            HANDLE other = CreateMutexW(NULL, FALSE, L"Local\\snapmap-plus-served");
            HANDLE elsewhere = CreateMutexW(NULL, FALSE, L"Local\\snapmap-plus-served-0123456789abcdef");
            CHECK(other && elsewhere);
            CHECK(sh_modserve_served_prune() == 2);
            CloseHandle(other);
            CloseHandle(elsewhere);
        }
        CHECK(GetFileAttributesA(stray) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesA(live) != INVALID_FILE_ATTRIBUTES);
        CHECK(sh_modserve_open(req, &bytes, &len) == 1 && len == 24);
        if (bytes) { HeapFree(GetProcessHeap(), 0, bytes); bytes = NULL; }
    }
}

/* Index tables two builds back are freed at a pass's start once nothing holds
 * them: not while a plan or a provider call is alive. What a later question
 * needs (which bytes the engine already read) is carried into the new table. */
static void test_index_retire(void)
{
    static const fx_file files[] = {
        { "generated/decls/entitydef/mods/retire00/thing.decl", "entityDef thing {}", -1, "entityDef",
          "mods/retire00/thing", NULL, '1' },
    };
    char top[MAX_PATH], root[MAX_PATH], *slash, *t;
    unsigned char *bytes = NULL;
    size_t len = 0;
    sh_modserve_plan plan;
    int i;
    printf("-- index tables two builds back are freed once nothing holds them\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s%s", top, "/retire/mods");
    for (t = root; *t; ++t) if (*t == '/') *t = '\\';
    mkdirs(root);
    make_bundle(root, "retire", "retire00", '7', NULL, files, 1, NULL);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_open("generated/decls/entitydef/mods/retire00/thing.decl", &bytes, &len) == 1);
    if (bytes) { HeapFree(GetProcessHeap(), 0, bytes); bytes = NULL; }
    for (i = 0; i < 3; ++i) CHECK(sh_modserve_index_build(root) == 1);
    /* A plan names the published table's strings: nothing is freed. */
    CHECK(sh_modserve_plan_editor(&plan) >= 0);
    CHECK(sh_modserve_index_retire() == 0);
    sh_modserve_plan_free(&plan);
    log_clear();
    CHECK(sh_modserve_index_retire() >= 2);
    CHECK(log_has("MODS: index: ") && log_has("earlier table(s) freed"));
    CHECK(sh_modserve_index_retire() == 0);
    CHECK(sh_modserve_open("generated/decls/entitydef/mods/retire00/thing.decl", &bytes, &len) == 1 && len == 18);
    if (bytes) { HeapFree(GetProcessHeap(), 0, bytes); bytes = NULL; }
    sh_modserve_attrib_reset();
}

/* An undeclared folder mixing an editor tweak with an addition and a cvar
 * requirement (modbundle_test test_classes builds one): the tweak serves in
 * every map, Play included; the addition and the requirement follow the
 * declared rule, so in Play of a map that declares nothing they are off
 * (DESIGN-additive 2.3 and 2.6). Declared, everything serves. */
#define MX_SET  "generated/decls/snapeditorsettings/mixed.decl"
#define MX_NEW  "generated/decls/entitydef/mods/xmixed00/thing.decl"
#define PU_NEW  "generated/decls/entitydef/mods/ypure000/thing.decl"
static void test_mixed_editor_bundle(void)
{
    static const fx_file mixed[] = {
        { MX_SET, "settings {}", -1, "snapEditorSettings", "mixed", NULL, '1' },
        { MX_NEW, "entityDef t {}", -1, "entityDef", "mods/xmixed00/thing", NULL, '2' },
    };
    static const fx_file pure[] = { { PU_NEW, "entityDef u {}", -1, "entityDef", "mods/ypure000/thing", NULL, '3' } };
    char top[MAX_PATH], root[MAX_PATH], h_mixed[65];
    const char *list[1];
    char *slash;
    sh_modserve_gates r;
    sh_modserve_scope_info si;
    scope_seen seen;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\mixed\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_label\": \"mixed\"}},\n  ";
    make_bundle(root, "xmixed00", "xmixed00", '7', "\"" MX_SET "\"", mixed, 2, "{\"installedAt\": 100}");
    hash64('7', h_mixed);
    g_bundle_extra = NULL;
    make_bundle(root, "ypure000", "ypure000", '8', NULL, pure, 1, "{\"installedAt\": 110}");
    printf("-- editor tweaks: a mixed folder's tweak serves in every map, its addition only where declared\n");
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_scope_enable(0);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 2);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(log_has("MODS: scope: xmixed00 shadows editor-time content (" MX_SET "); its editor tweaks serve in every "
                  "map this editor opens, the rest only in maps that declare it"));
    sh_modserve_scope_enable(1);
    /* No map: the tweak and the Toybox names serve; the cvar is not accepted. */
    strcpy_s(g_text_val[3], sizeof g_text_val[3], "stock");
    CHECK(sh_modserve_lookup(MX_SET, NULL) && sh_modserve_lookup(MX_NEW, NULL) && sh_modserve_lookup(PU_NEW, NULL));
    CHECK(sh_modserve_requirements_apply(&g_gates, "hub", &r) >= 0 && !strcmp(g_text_val[3], "stock"));
    CHECK(sh_modserve_bundle_scope(h_mixed, &si) && si.scope == SH_MODSERVE_SCOPE_ALWAYS && si.automatic &&
          si.shadows_live && si.names_live && !si.requirements_live);
    /* Play of a map that declares nothing: only the tweak. */
    CHECK(sh_modserve_scope_set(NULL, 0, 1) >= 0);
    CHECK(sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) >= 0 && sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY);
    CHECK(sh_modserve_lookup(MX_SET, NULL));
    CHECK(!sh_modserve_lookup(MX_NEW, NULL) && !sh_modserve_lookup(PU_NEW, NULL));
    CHECK(sh_modserve_requirements_apply(&g_gates, "play", &r) >= 0 && !strcmp(g_text_val[3], "stock"));
    CHECK(sh_modserve_bundle_scope(h_mixed, &si) && si.play && si.shadows_live && !si.names_live && !si.requirements_live);
    /* Declared: everything it carries, its cvar included. */
    list[0] = h_mixed;
    CHECK(sh_modserve_scope_set(list, 1, 1) >= 0);
    CHECK(sh_modserve_lookup(MX_SET, NULL) && sh_modserve_lookup(MX_NEW, NULL) && !sh_modserve_lookup(PU_NEW, NULL));
    CHECK(sh_modserve_requirements_apply(&g_gates, "play", &r) >= 0 && !strcmp(g_text_val[3], "mixed"));
    CHECK(sh_modserve_bundle_scope(h_mixed, &si) && si.listed && si.names_live && si.requirements_live);
    (void)sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR);
    (void)sh_modserve_scope_set(NULL, 0, 0);
    (void)sh_modserve_requirements_apply(&g_gates, "hub", &r);
    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* A store whose root holds a character outside ASCII (a profile folder named
 * with an accent): the index boots from the store's own UTF-8 root and reads
 * a map's place under it, as the store wrote it. */
static void test_store_root_utf8(void)
{
    static const fx_file plain[] = {
        { "generated/decls/entitydef/mods/uplain00/p.decl", "entityDef p {}", -1, "entityDef", "mods/uplain00/p", NULL, '6' },
    };
    static const fx_file placed[] = {
        { "generated/decls/entitydef/mods/usnap000/x.decl", "entityDef x { good }", -1, "entityDef", "mods/usnap000/x", NULL, '2' },
    };
    char top[MAX_PATH], root[MAX_PATH], path[MAX_PATH], h8[65];
    char *slash, *accent;
    printf("-- the store root holds a non-ASCII character: a map's place is read\n");
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    accent = sh_mb_utf8(L"sm\x00e9");   /* "sme" with an acute accent, in UTF-8 */
    CHECK(accent && strlen(accent) == 4);
    if (!accent) return;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\%s\\mods", top, accent);
    free(accent);
    mkdirs(root);
    make_bundle(root, "plain", "uplain00", '7', NULL, plain, 1, NULL);
    make_bundle(root, "maps\\Caf\xc3\xa9\\snap", "usnap000", '8', NULL, placed, 1, NULL);
    hash64('8', h8);

    /* The boot resolves the store's root, as the product's boot does. */
    CHECK(sh_ms_set_root(root));
    log_clear();
    CHECK(sh_modserve_index_boot() == 2);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_rescan() == 2);   /* the rescan reuses the boot's root */
    {
        sh_modserve_bundle_ref ref;
        sh_modmanifest_row row;
        CHECK(!strcmp(ad_served("generated/decls/entitydef/mods/uplain00/p.decl"), "entityDef p {}"));
        memset(&row, 0, sizeof row);
        strcpy_s(row.salt, sizeof row.salt, "usnap000");
        strcpy_s(row.build, sizeof row.build, h8);
        CHECK(sh_modserve_place_set("maps\\Caf\xc3\xa9") >= 0);
        CHECK(sh_modserve_row_build(&row, &ref) && !strcmp(ref.place, "maps\\Caf\xc3\xa9"));
        CHECK(sh_modserve_place_set(NULL) >= 0);
    }
    sh_ms_set_root(NULL);
    strcpy_s(path, sizeof path, root);
    slash = strrchr(path, '\\');
    if (slash) *slash = 0;
    remove_tree(path);
    CHECK(sh_modserve_index_build(g_root) >= 5);
}

/* ------------------------------------------------------------------ what a kept pass looks up (triage F8b) */

/* An in-editor pass registers while the previous map's level-1 objects are
 * alive; a decl it keeps binds to them through the engine's lookup. The
 * engine side's ResourceLookup detour notes each return while a pass is
 * open (the stand-in here calls note as the hook does); the close raises
 * what is still listed at level 1, and the set is emptied. */
static void test_pass_lookups(void)
{
    static kh_res old_skel, old_mat, stock, freed, later;
    static kh_res *skels[4], *mats[4];
    static kh_list ls, lm, bad;
    kh_list *head = &ls;
    sh_modserve_lookups out;
    int i;
    old_skel.name = "md6/old/body.md6skl"; old_skel.level = 1;
    old_mat.name = "models/old/body"; old_mat.level = 1;
    stock.name = "models/stock/permanent"; stock.level = 4;
    freed.name = "models/old/freed"; freed.level = 1;
    later.name = "models/old/later"; later.level = 1;
    ls.type = "md6skl"; ls.cls = "idMD6Skel"; ls.next = &lm; ls.entries = skels; ls.count = 1;
    lm.type = "material"; lm.cls = "idMaterial"; lm.next = NULL; lm.entries = mats; lm.count = 3;
    skels[0] = &old_skel;
    mats[0] = &old_mat; mats[1] = &stock; mats[2] = &later;
    /* Outside a pass: nothing is recorded and a close raises nothing. */
    CHECK(!sh_modserve_lookups_recording());
    sh_modserve_lookups_note(&old_skel);
    CHECK(sh_modserve_lookups_close(&head, &out) == 0 && out.raised == 0 && old_skel.level == 1);
    /* A pass: what it looks up is raised with it, once each; a stock object
     * already at 4 costs nothing; an object no longer listed is left alone. */
    sh_modserve_lookups_open();
    CHECK(sh_modserve_lookups_recording());
    for (i = 0; i < 3; ++i) { sh_modserve_lookups_note(&old_skel); sh_modserve_lookups_note(&old_mat); }
    sh_modserve_lookups_note(&stock);
    sh_modserve_lookups_note(&freed);
    sh_modserve_lookups_note(NULL);
    CHECK(sh_modserve_lookups_close(&head, &out) == 1);
    CHECK(out.looked_up == 4 && out.live == 3 && out.raised == 2 && out.lists == 2 && !out.overflow);
    CHECK(old_skel.level == 4 && old_mat.level == 4 && stock.level == 4 && freed.level == 1 && later.level == 1);
    CHECK(strstr(out.first, "md6/old/body.md6skl") && strstr(out.first, "models/old/body") && !strstr(out.first, "stock"));
    CHECK(!sh_modserve_lookups_recording());
    /* The set cleared: the next pass starts empty, and a lookup after the
     * close is not carried into it. */
    sh_modserve_lookups_note(&later);
    sh_modserve_lookups_open();
    CHECK(sh_modserve_lookups_close(&head, &out) == 1 && out.looked_up == 0 && out.raised == 0 && later.level == 1);
    /* Many distinct lookups grow the set; each is raised once. */
    {
        static kh_res many[5000];
        static kh_res *many_e[5000];
        static kh_list lmany;
        for (i = 0; i < 5000; ++i) { many[i].name = "models/many"; many[i].level = 1; many_e[i] = &many[i]; }
        lmany.type = "material"; lmany.cls = "idMaterial"; lmany.next = NULL; lmany.entries = many_e; lmany.count = 5000;
        lm.next = &lmany;
        sh_modserve_lookups_open();
        for (i = 0; i < 5000; ++i) { sh_modserve_lookups_note(&many[i]); sh_modserve_lookups_note(&many[4999 - i]); }
        CHECK(sh_modserve_lookups_close(&head, &out) == 1 && out.looked_up == 5000 && out.raised == 5000);
        for (i = 0; i < 5000 && many[i].level == 4; ++i) {}
        CHECK(i == 5000);
        lm.next = NULL;
    }
    /* Lists that do not read as lists: the close reports it, nothing more is
     * raised, and the set still clears. */
    bad.type = "bad"; bad.cls = "bad"; bad.next = NULL; bad.entries = NULL; bad.count = 2;
    lm.next = &bad;
    later.level = 1;
    sh_modserve_lookups_open();
    sh_modserve_lookups_note(&later);
    (void)sh_modserve_lookups_close(&head, &out);
    CHECK(out.looked_up == 1 && !sh_modserve_lookups_recording());
    lm.next = NULL;
    later.level = 1;
    sh_modserve_lookups_open();
    CHECK(sh_modserve_lookups_close(&head, &out) == 1 && out.looked_up == 0 && later.level == 1);
}

/* ------------------------------------------------------------------ what kept objects load (triage F9) */

/* The engine side's detours report the engine's load nesting (enter/leave
 * around ResourceGenericLoad) and what ResourceLookup returns inside a load
 * (found); the stand-ins here call them as the hooks do. A Play's spawn loads
 * the chainsaw gore md6Def, whose load loads its mesh, whose load creates
 * its material; nothing is kept then. A later pass keeps the md6Def by name,
 * and the close follows the recorded pairs to the mesh and the material. */
static void test_load_attribution(void)
{
    static kh_res def, mesh, mat, img, stock, perm, other, child, found, outside, gone, nest[70];
    static kh_res *defs[2], *meshes[2], *mats[8], *imgs[2], *nests[70];
    static kh_list ld, lmesh, lmat, limg, lnest, bad;
    kh_list *head = &ld;
    sh_modserve_attrib out;
    int i, kept, depth, d0;
    sh_modserve_attrib_reset();
    def.name = "zion/gore/chainsaw1_scientist_right_bloody.md6"; def.level = 1;
    mesh.name = "md6/gore/chainsaw1_scientist_right_bloody.md6mesh"; mesh.level = 1;
    mat.name = "models/monsters/unwilling/unwilling_scientist_chainsaw"; mat.level = 1;
    img.name = "models/monsters/unwilling/blood.tga"; img.level = 1;
    stock.name = "models/stock/permanent"; stock.level = 4;
    perm.name = "models/stock/engine_own"; perm.level = 4;
    other.name = "models/map/unrelated"; other.level = 1;
    child.name = "models/kept/child"; child.level = 1;
    found.name = "models/kept/found"; found.level = 1;
    outside.name = "models/outside"; outside.level = 1;
    gone.name = "models/gone"; gone.level = 1;
    ld.type = "md6Def"; ld.cls = "idDeclMD6"; ld.next = &lmesh; ld.entries = defs; ld.count = 1;
    lmesh.type = "baseModel"; lmesh.cls = "idMD6Model"; lmesh.next = &lmat; lmesh.entries = meshes; lmesh.count = 1;
    lmat.type = "material"; lmat.cls = "idMaterial"; lmat.next = &limg; lmat.entries = mats; lmat.count = 6;
    limg.type = "image"; limg.cls = "idImage"; limg.next = NULL; limg.entries = imgs; limg.count = 2;
    defs[0] = &def; meshes[0] = &mesh; imgs[0] = &img; imgs[1] = &perm;
    mats[0] = &mat; mats[1] = &stock; mats[2] = &other; mats[3] = &child; mats[4] = &found; mats[5] = &outside;

    /* The Play: nested loads and a lookup inside the material's load are
     * recorded; a lookup outside any load is not; nothing is kept. */
    CHECK(sh_modserve_attrib_depth() == 0);
    d0 = sh_modserve_attrib_enter(&def, 0, &kept);
    CHECK(d0 == 0 && !kept && sh_modserve_attrib_depth() == 1);
    depth = sh_modserve_attrib_enter(&mesh, 0, &kept);
    CHECK(depth == 1 && !kept);
    {
        int dm = sh_modserve_attrib_enter(&mat, 0, &kept);
        sh_modserve_attrib_found(&img);
        sh_modserve_attrib_found(&stock);
        sh_modserve_attrib_found(&mat);      /* itself: no pair */
        sh_modserve_attrib_leave(dm);
    }
    sh_modserve_attrib_leave(depth);
    sh_modserve_attrib_leave(d0);
    CHECK(sh_modserve_attrib_depth() == 0);
    sh_modserve_attrib_found(&other);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1);
    CHECK(out.pairs == 4 && out.raised == 0 && out.followed == 0 && out.kept == 0 && out.lists == 4 && !out.overflow);
    CHECK(def.level == 1 && mesh.level == 1 && mat.level == 1 && img.level == 1 && other.level == 1);

    /* A pass keeps the md6Def by name: the close follows the pairs from it,
     * transitively; the stock material stays as it is, the unrelated one at 1. */
    def.level = 4;
    sh_modserve_kept_add(&def);
    CHECK(sh_modserve_kept_has(&def) && !sh_modserve_kept_has(&mesh));
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1);
    CHECK(out.pairs == 4 && out.followed == 4 && out.raised == 3 && out.kept == 5);
    CHECK(mesh.level == 4 && mat.level == 4 && img.level == 4 && stock.level == 4 && other.level == 1);
    CHECK(strstr(out.first, "chainsaw1_scientist_right_bloody.md6mesh") && strstr(out.first, "unwilling_scientist_chainsaw") &&
          !strstr(out.first, "stock") && !strstr(out.first, "unrelated"));
    CHECK(sh_modserve_kept_has(&mat) && sh_modserve_kept_has(&img) && !sh_modserve_kept_has(&other));
    /* A rerun raises nothing; the followed pairs are not kept. */
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.raised == 0 && out.followed == 0 && out.pairs == 0);
    /* A permanent object's pairs are not kept past a close: the engine's own
     * level-4 objects are never raised, so nothing could follow them. */
    d0 = sh_modserve_attrib_enter(&perm, 0, &kept);
    sh_modserve_attrib_found(&other);
    sh_modserve_attrib_leave(d0);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.pairs == 1 && out.raised == 0);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.pairs == 0 && other.level == 1);

    /* A kept object's load: what it loads and looks up is kept as it loads,
     * the loaded child when its own load returns; a lookup outside is not. */
    d0 = sh_modserve_attrib_enter(&def, 0, &kept);
    CHECK(!kept);                            /* nothing open around it */
    depth = sh_modserve_attrib_enter(&child, 0, &kept);
    CHECK(kept && child.level == 1);         /* raised when its load returns */
    sh_modserve_attrib_leave(depth);
    CHECK(sh_modserve_attrib_keep(&child) == 1 && child.level == 4);
    sh_modserve_attrib_found(&found);
    CHECK(found.level == 4 && sh_modserve_kept_has(&found));
    sh_modserve_attrib_leave(d0);
    sh_modserve_attrib_found(&outside);
    CHECK(outside.level == 1 && !sh_modserve_kept_has(&outside));
    CHECK(sh_modserve_attrib_keep(&outside) == 0 && outside.level == 1);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.raised_at_load == 2 && out.raised == 0);

    /* A load that allocates from a map heap is never raised, by any keep: the
     * map's next load releases what it allocated (the gore skeleton's joints,
     * G1). The keep names it; once it loads again in process heap 0 it is. */
    {
        static kh_res gore;
        char left[300];
        gore.name = "md6/gore/chainsaw1_scientist_right_bloody.md6skl"; gore.level = 1;
        mats[6] = &gore; lmat.count = 7;
        CHECK(sh_modserve_left_to_map(left, sizeof left) == 0);
        d0 = sh_modserve_attrib_enter(&def, 0, &kept);
        depth = sh_modserve_attrib_enter(&gore, 2, &kept);
        CHECK(kept);
        sh_modserve_attrib_leave(depth);
        CHECK(sh_modserve_attrib_keep(&gore) == 0 && gore.level == 1);
        sh_modserve_attrib_leave(d0);
        CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.raised == 0 && gore.level == 1);
        CHECK(sh_modserve_left_to_map(left, sizeof left) == 2 && strstr(left, "right_bloody.md6skl"));
        CHECK(sh_modserve_left_to_map(left, sizeof left) == 0 && !left[0]);
        /* The one keep every level write uses refuses it and names it, and
         * keeps a resource no map heap owns. */
        CHECK(!sh_modserve_keepable(&gore) && sh_modserve_keep(&gore) == 0 && gore.level == 1);
        CHECK(sh_modserve_left_to_map(left, sizeof left) == 2 && strstr(left, "right_bloody.md6skl"));
        {
            static kh_res plain;
            plain.name = "models/plain"; plain.level = 1;
            CHECK(sh_modserve_keepable(&plain) && sh_modserve_keep(&plain) == 1 && plain.level == 4 &&
                  sh_modserve_kept_has(&plain));
            CHECK(sh_modserve_keep(&plain) == 1 && plain.level == 4 && sh_modserve_left_to_map(NULL, 0) == 0);
        }
        /* The holds by name and a pass's lookups leave it too. */
        sh_modserve_lookups_open();
        sh_modserve_lookups_note(&gore);
        CHECK(sh_modserve_lookups_close(&head, NULL) == 1 && gore.level == 1);
        CHECK(sh_modserve_left_to_map(NULL, 0) == 1);
        /* Loaded again in process heap 0 (a pass's scope): kept. */
        d0 = sh_modserve_attrib_enter(&def, 0, &kept);
        depth = sh_modserve_attrib_enter(&gore, 0, &kept);
        sh_modserve_attrib_leave(depth);
        CHECK(kept && sh_modserve_attrib_keep(&gore) == 1 && gore.level == 4);
        sh_modserve_attrib_leave(d0);
        /* An unknown heap (-1) neither marks nor clears. */
        gore.level = 1;
        depth = sh_modserve_attrib_enter(&gore, -1, &kept);
        sh_modserve_attrib_leave(depth);
        CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && gore.level == 4 && sh_modserve_left_to_map(NULL, 0) == 0);
        lmat.count = 6;
    }

    /* Depth: 70 nested loads; past the bound nothing is attributed, the
     * depth is still counted, and a leave restores it however deep the load
     * that skipped its own leave was (the detour's __finally). */
    for (i = 0; i < 70; ++i) { nest[i].name = "models/nest"; nest[i].level = 1; nests[i] = &nest[i]; }
    lnest.type = "material"; lnest.cls = "idMaterial"; lnest.next = NULL; lnest.entries = nests; lnest.count = 70;
    limg.next = &lnest;
    d0 = sh_modserve_attrib_enter(&def, 0, &kept);
    for (i = 0; i < 70; ++i) (void)sh_modserve_attrib_enter(&nest[i], 0, &kept);
    CHECK(sh_modserve_attrib_depth() == 71);
    sh_modserve_attrib_leave(d0 + 3);        /* a throw out of the fourth load */
    CHECK(sh_modserve_attrib_depth() == 3);
    sh_modserve_attrib_leave(d0);
    CHECK(sh_modserve_attrib_depth() == 0);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1);
    for (i = 0; i < SH_MODSERVE_NEST_MAX; ++i) CHECK(nest[i].level == 4);
    for (i = SH_MODSERVE_NEST_MAX; i < 70; ++i) CHECK(nest[i].level == 1);
    limg.next = NULL;

    /* A kept address no list holds any longer is dropped: a resource the
     * engine later creates there is not kept. */
    sh_modserve_kept_add(&gone);
    CHECK(sh_modserve_kept_has(&gone));
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && !sh_modserve_kept_has(&gone));
    mats[6] = &gone; lmat.count = 7;
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && gone.level == 1);

    /* Before the purge the pairs are forgotten: a parent kept after it does
     * not reach what an earlier load of that address recorded. */
    other.level = 1; outside.level = 1;
    d0 = sh_modserve_attrib_enter(&other, 0, &kept);
    sh_modserve_attrib_found(&outside);
    sh_modserve_attrib_leave(d0);
    CHECK(sh_modserve_attrib_close(&head, 1, &out) == 1 && out.pairs >= 1 && out.raised == 0 && outside.level == 1);
    sh_modserve_kept_add(&other);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.pairs == 0 && out.followed == 0 && outside.level == 1);
    other.level = 1;
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && other.level == 4);   /* kept itself: raised */

    /* Bounds: past a million pairs recording stops and the close says so;
     * the close then starts afresh. */
    for (i = 0; i < (1 << 20) + 8; ++i)
        (void)sh_modserve_attrib_note((const void *)(uintptr_t)(0x100000000ull + (uint64_t)i * 64),
                                      (const void *)(uintptr_t)(0x900000000ull + (uint64_t)i * 64));
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && out.overflow && out.pairs == (1u << 20));
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 1 && !out.overflow && out.pairs == 0);

    /* Lists that do not read as lists: the close reports it and raises nothing. */
    bad.type = "bad"; bad.cls = "bad"; bad.next = NULL; bad.entries = NULL; bad.count = 2;
    limg.next = &bad;
    outside.level = 1;
    sh_modserve_kept_add(&outside);
    CHECK(sh_modserve_attrib_close(&head, 0, &out) == 0 && outside.level == 1);
    limg.next = NULL;
    sh_modserve_attrib_reset();
}

int main(void)
{
    char top[MAX_PATH];
    char *slash;
    build_fixture();
    test_index_listeners();
    test_empty_index();

    test_index_and_isolation();
    test_unopened_shadows();
    test_unopened_after_rescan();
    test_lookup();
    test_precedence();
    test_probe();
    test_plan();
    test_plan_run();
    test_bundles_for_names();
    test_cost();
    test_rescan();
    test_requirements();
    test_ungated_raw();
    test_surface_gated_reload();
    test_surface_built_after_boot();
    test_requirements_general();
    test_refresh();
    test_conflicts();
    test_editor_refs();
    test_watermark();
    test_kept_holds();
    test_held_models();
    test_pass_lookups();
    test_load_attribution();
    test_editor_closure();
    test_renderparm_first();
    test_surfaces_never_parsed();
    test_launch_preload();
    test_refusals();
    test_permanent_first_parse();
    test_registration_reparse_of_held_permanent();
    test_bundle_extra();
    test_scope();
    test_mixed_editor_bundle();
    test_scope_providers();
    test_additive_determinism();
    test_additive_same_key();
    test_two_builds_requirements();
    test_serving_build_needs();
    test_additive_compose();
    test_salt_and_place();
    test_resource_lock();
    test_row_served();
    test_dependency_names();
    test_bank_stems();
    test_index_retire();
    test_store_root_utf8();
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) { *slash = 0; remove_tree(top); }
    if (g_failed) {
        fprintf(stderr, "modserve_test: %d check(s) failed\n--- log ---\n%s", g_failed, g_log);
        return 1;
    }
    printf("modserve_test: all checks passed\n");
    return 0;
}
