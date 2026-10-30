/* modserve_test.c -- the installed-mod index and provider (modserve.c)
 * against a fixture mods tree written to a temporary folder: admission,
 * failure isolation (size mismatch, unvalidated state, namespace, unbuilt
 * folder), a shadow conflict, version supersession, the blob-store fallback,
 * lookups and their cost, provider precedence against navigation, the Grid
 * Room and built-in defaults (the real overrides.c), probe claims, the
 * registration plan's dependency order and blocking, plan runs through fake
 * engine operations, the save-time bundle closure and a rescan. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modserve.h"
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

static void mkdirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    strcpy_s(tmp, sizeof tmp, path);
    for (p = tmp + 3; *p; ++p)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char *path, const char *body, size_t len)
{
    char dir[MAX_PATH];
    char *slash;
    FILE *f;
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; mkdirs(dir); }
    f = fopen(path, "wb");
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

/* Raw JSON members (with a trailing comma) the next make_bundle writes. */
static const char *g_bundle_extra;

/* Write <root>\<folder>\bundle.json and its files. */
static void make_bundle(const char *parent, const char *folder, const char *id, const char *version,
                        char hash_digit, const char *shadows, const fx_file *files, int n, const char *state)
{
    char dir[MAX_PATH], path[MAX_PATH], h[65];
    static char json[32768];
    size_t at = 0;
    int i;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", parent, folder);
    mkdirs(dir);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"format\": 1,\n  \"id\": \"%s\",\n  \"version\": \"%s\",\n  \"name\": \"%s\",\n"
                      "  \"bundle\": \"%s\",\n  %s\"shadows\": [%s],\n  \"files\": [\n",
                      id, version, id, hash64(hash_digit, h), g_bundle_extra ? g_bundle_extra : "",
                      shadows ? shadows : "");
    for (i = 0; i < n; ++i) {
        const fx_file *f = &files[i];
        long long size = f->size >= 0 ? f->size : (long long)strlen(f->body ? f->body : "");
        char b[65];
        at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                          "    {\"path\": \"assets/%s\", \"blob\": \"%s\", \"size\": %lld, "
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
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, "  ]\n}\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", dir);
    write_file(path, json, at);
    if (state) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.state.json", dir);
        write_file(path, state, strlen(state));
    }
}

#define A_MD6    "generated/decls/md6def/mods/alex.pink-imp/imp.md6.decl"
#define A_MESH   "generated/basemodel/md6/mods/alex.pink-imp/mesh/imp.bmd6model"
#define A_ENT    "generated/decls/entitydef/mods/alex.pink-imp/imp.decl"
#define A_SEDEF  "generated/decls/snapeditorentitydef/mods/alex.pink-imp/imp.decl"
#define A_FIRE   "generated/decls/entitydef/mods/alex.pink-imp/fireball.decl"
#define A_MAT    "generated/decls/material2/mods/alex.pink-imp/skin.decl"
#define SHADOW   "generated/decls/entitydef/monster/imp.decl"
#define LIFT     "generated/decls/snapeditorentitydef/func/lift.decl"
#define GRID     "generated/decls/snapmoduleinfo/smpgrid/v1/c/512_768_384.decl"
#define B_THING  "generated/decls/entitydef/mods/bob.skins/thing.decl"
#define B_SEDEF  "generated/decls/snapeditorentitydef/mods/bob.skins/thing.decl"
#define C_ONLY   "generated/decls/entitydef/mods/carol.broken/only.decl"
#define F_BLOB   "generated/image/mods/fay.blob/tex.bimage"

static char g_hash_a[65], g_hash_a2[65], g_hash_b[65], g_hash_c[65], g_hash_f[65], g_hash_l[65];

static void build_fixture(void)
{
    char tmp[MAX_PATH], path[MAX_PATH], legacy[MAX_PATH], blobs[MAX_PATH], h[65];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%smodserve_test_%lu\\mods", tmp, GetCurrentProcessId());
    mkdirs(g_root);
    {
        static const fx_file a[] = {
            { A_MESH, "MESHBYTES-0123456789", -1, "baseModel", "md6/mods/alex.pink-imp/mesh/imp.md6mesh", NULL, '1' },
            { A_MAT, "material2 skin {}", -1, "material2", "mods/alex.pink-imp/skin", NULL, '2' },
            { A_MD6, "md6Def imp { mesh \"md6/mods/alex.pink-imp/mesh/imp.md6mesh\" }", -1, "md6Def",
              "mods/alex.pink-imp/imp.md6",
              "[{\"type\":\"baseModel\",\"name\":\"md6/mods/alex.pink-imp/mesh/imp.md6mesh\"},"
              "{\"type\":\"material2\",\"name\":\"mods/alex.pink-imp/skin\"},"
              "{\"type\":\"material2\",\"name\":\"models/monsters/imp/imp\"}]", '3' },
            { A_ENT, "entityDef imp { inherit \"snapmaps/placeablesnapaiencounter/imp\" }", -1, "entityDef",
              "mods/alex.pink-imp/imp", "[{\"type\":\"md6Def\",\"name\":\"mods/alex.pink-imp/imp.md6\"}]", '4' },
            /* Listed before its needs on purpose: the plan reorders. */
            { A_SEDEF, "{ entityDef \"mods/alex.pink-imp/imp\" }", -1, "snapEditorEntityDef", "mods/alex.pink-imp/imp",
              "[{\"type\":\"entityDef\",\"name\":\"mods/alex.pink-imp/imp\"},"
              "{\"type\":\"md6Def\",\"name\":\"mods/alex.pink-imp/imp.md6\"}]", '5' },
            { A_FIRE, "entityDef fireball {}", -1, "entityDef", "mods/alex.pink-imp/fireball", NULL, '6' },
            { SHADOW, "entityDef pink { }", -1, "entityDef", "monster/imp", NULL, '7' },
            { LIFT, "MODLIFT", -1, "snapEditorEntityDef", "func/lift", NULL, '8' },
            { GRID, "MODGRID", -1, "snapModuleInfo", "smpgrid/v1/c/512_768_384", NULL, '9' },
            { "maps/game/snapmap/test/nav_owned.decl", "MODNAV", -1, "file", "maps/game/snapmap/test/nav_owned.decl", NULL, 'a' },
        };
        /* The NAV shadow path is not namespaced; it is declared a shadow. */
        make_bundle(g_root, "alex.pink-imp@1.0.0", "alex.pink-imp", "1.0.0", 'a',
                    "\"" SHADOW "\", \"" LIFT "\", \"" GRID "\", \"maps/game/snapmap/test/nav_owned.decl\"",
                    a, (int)(sizeof a / sizeof a[0]), "{\"validated\": true, \"installedAt\": 100}");
        hash64('a', g_hash_a);
    }
    {
        /* A later version of the same mod changing one file. */
        static const fx_file a2[] = {
            { A_FIRE, "entityDef fireball { v2 }", -1, "entityDef", "mods/alex.pink-imp/fireball", NULL, 'b' },
        };
        make_bundle(g_root, "alex.pink-imp@1.1.0", "alex.pink-imp", "1.1.0", 'b', NULL, a2, 1,
                    "{\"validated\": true, \"installedAt\": 200}");
        hash64('b', g_hash_a2);
    }
    {
        /* Shadows the same stock entityDef: a conflict. Its editor row needs
         * an md6Def nobody provides: blocked. */
        static const fx_file b[] = {
            { SHADOW, "entityDef bob { }", -1, "entityDef", "monster/imp", NULL, 'c' },
            { B_THING, "entityDef thing {}", -1, "entityDef", "mods/bob.skins/thing",
              "[{\"type\":\"md6Def\",\"name\":\"mods/alex.pink-imp/imp.md6\"}]", 'd' },
            { B_SEDEF, "{ entityDef \"mods/bob.skins/thing\" }", -1, "snapEditorEntityDef", "mods/bob.skins/thing",
              "[{\"type\":\"entityDef\",\"name\":\"mods/bob.skins/thing\"},"
              "{\"type\":\"md6Def\",\"name\":\"mods/bob.skins/missing.md6\"}]", 'e' },
        };
        make_bundle(g_root, "bob.skins@2.0.0", "bob.skins", "2.0.0", 'c', "\"" SHADOW "\"", b, 3, NULL);
        hash64('c', g_hash_b);
    }
    {
        /* Size mismatch: skipped entirely. */
        static const fx_file c[] = {
            { C_ONLY, "entityDef only {}", 999, "entityDef", "mods/carol.broken/only", NULL, 'f' },
        };
        make_bundle(g_root, "carol.broken@1.0.0", "carol.broken", "1.0.0", 'd', NULL, c, 1, NULL);
        hash64('d', g_hash_c);
    }
    {
        static const fx_file d[] = {
            { "generated/decls/entitydef/mods/dave.x/a.decl", "x", -1, "entityDef", "mods/dave.x/a", NULL, '1' },
        };
        make_bundle(g_root, "dave.x@1.0.0", "dave.x", "1.0.0", 'e', NULL, d, 1,
                    "{\"validated\": false, \"firstCause\": \"md6Def mesh missing\"}");
    }
    {
        /* A new name outside the namespace that is not declared a shadow. */
        static const fx_file e[] = {
            { "generated/decls/entitydef/monster/sneaky.decl", "x", -1, "entityDef", "monster/sneaky", NULL, '1' },
        };
        make_bundle(g_root, "erin.ns@1.0.0", "erin.ns", "1.0.0", '1', NULL, e, 1, NULL);
    }
    {
        /* A file missing from the tree but present in the blob store. */
        static const fx_file f[] = { { F_BLOB, NULL, 9, "image", "mods/fay.blob/tex", NULL, '2' } };
        make_bundle(g_root, "fay.blob@1.0.0", "fay.blob", "1.0.0", '2', NULL, f, 1, NULL);
        hash64('2', g_hash_f);
        _snprintf_s(blobs, sizeof blobs, _TRUNCATE, "%s\\blobs\\%s", g_root, hash64('2', h));
        write_file(blobs, "BLOBBYTES", 9);
    }
    {
        /* Converted first-generation package: its names are not namespaced. */
        static const fx_file l[] = {
            { "generated/decls/entitydef/smp/oldthing.decl", "entityDef old {}", -1, "entityDef", "smp/oldthing", NULL, '3' },
        };
        _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", g_root);
        make_bundle(legacy, "old.pkg@0123456789abcdef", "old.pkg", "0123456789abcdef", '3', NULL, l, 1, NULL);
        hash64('3', g_hash_l);
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

static double now_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

/* ------------------------------------------------------------------ tests */

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
    CHECK(sh_modserve_index_build(g_root) == 5);   /* alex x2, bob, fay, legacy */
    sh_modserve_get_stats(&st);
    CHECK(st.bundles_seen == 9);
    CHECK(st.bundles_admitted == 5);
    CHECK(st.conflicts == 1);
    CHECK(log_has("bundle 'carol.broken@1.0.0' skipped: size mismatch: '" C_ONLY "' is 17 bytes, bundle.json says 999"));
    CHECK(log_has("bundle 'dave.x@1.0.0' skipped: not validated: md6Def mesh missing"));
    CHECK(log_has("bundle 'erin.ns@1.0.0' skipped: 'generated/decls/entitydef/monster/sneaky.decl' is outside mods/erin.ns/"));
    CHECK(log_has("bundle 'unbuilt' skipped: no bundle.json"));
    CHECK(log_count("skipped:") == 4);   /* one line per skipped bundle */
    /* Two mods' shadows of one stock decl: one serves (here the more
     * recently installed; the texts are outside the composable grammar). */
    CHECK(log_has("conflict: shadow '" SHADOW "' is provided by alex.pink-imp@1.0.0, bob.skins@2.0.0; serving "
                  "alex.pink-imp@1.0.0 (installed most recently)"));
    CHECK(log_has("bundle alex.pink-imp@1.0.0: superseded by alex.pink-imp@1.1.0"));
    CHECK(log_has("MODS: index built in ") && log_has("(5 bundles, "));
    CHECK(sh_modserve_bundle_state(g_hash_a, NULL, 0) == 1);
    {
        char cause[200];
        CHECK(sh_modserve_bundle_state(g_hash_c, cause, sizeof cause) == -1 && strstr(cause, "size mismatch"));
    }
    CHECK(sh_modserve_bundle_state("0000000000000000000000000000000000000000000000000000000000000000", NULL, 0) == 0);
    printf("modserve_test: fixture index built in %.2f ms (%d bundles seen, %d files, %d keys)\n", st.build_ms,
           st.bundles_seen, st.files, st.keys);
}

static void test_lookup(void)
{
    sh_modserve_hit hit;
    char text[256];
    CHECK(sh_modserve_lookup(A_MD6, &hit) && hit.is_decl && !hit.is_shadow && hit.size > 0 &&
          !strcmp(hit.bundle_id, "alex.pink-imp"));
    /* Case and separators fold; the engine spells md6Def and baseModel. */
    CHECK(sh_modserve_lookup("generated/decls/md6Def/mods/alex.pink-imp/imp.md6.decl", NULL));
    CHECK(sh_modserve_lookup("GENERATED\\baseModel\\md6\\mods\\alex.pink-imp\\mesh\\imp.bmd6model", NULL));
    /* The registration source read and the sanitised raw spelling. */
    CHECK(sh_modserve_lookup("decltree/md6def/mods/alex.pink-imp/imp.md6.decl", NULL));
    CHECK(sh_modserve_lookup("generated/baseModel/md6/mods/alex.pink_imp/mesh/imp.bmd6model", NULL));
    /* Misses: stock, a skipped bundle's file, near names. */
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/monster/hellknight.decl", NULL));
    {
        sh_modserve_hit w;
        sh_modserve_conflict c;
        CHECK(sh_modserve_lookup(SHADOW, &w) && !strcmp(w.bundle_id, "alex.pink-imp"));
        CHECK(sh_modserve_conflict_info("GENERATED/decls/entitydef/monster/imp.decl", &c) && c.count == 2 &&
              !c.composed && !strcmp(c.served_id, "alex.pink-imp") && !strcmp(c.reason, "installed most recently") &&
              !strcmp(c.providers, "alex.pink-imp@1.0.0, bob.skins@2.0.0"));
        CHECK(!sh_modserve_conflict_info(A_ENT, &c));
        CHECK(open_text(SHADOW, text, sizeof text) == 1 && !strcmp(text, "entityDef pink { }"));
    }
    CHECK(!sh_modserve_lookup(C_ONLY, NULL));
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/mods/dave.x/a.decl", NULL));
    CHECK(!sh_modserve_lookup(A_ENT "x", NULL));
    CHECK(!sh_modserve_lookup("", NULL));
    CHECK(!sh_modserve_lookup(NULL, NULL));
    /* Bytes, version supersession and the blob-store fallback. */
    CHECK(open_text(A_ENT, text, sizeof text) == 1 && !strcmp(text, "entityDef imp { inherit \"snapmaps/placeablesnapaiencounter/imp\" }"));
    CHECK(open_text(A_FIRE, text, sizeof text) == 1 && !strcmp(text, "entityDef fireball { v2 }"));
    CHECK(open_text(F_BLOB, text, sizeof text) == 1 && !strcmp(text, "BLOBBYTES"));
    CHECK(open_text("generated/decls/entitydef/smp/oldthing.decl", text, sizeof text) == 1);
}

static void test_precedence(void)
{
    char text[256];
    log_clear();
    /* A new mod name reaches the engine through the provider's stream. */
    CHECK(provider_text(A_SEDEF, 0, text, sizeof text) && !strcmp(text, "{ entityDef \"mods/alex.pink-imp/imp\" }"));
    CHECK(provider_text("generated/decls/snapEditorEntityDef/mods/alex.pink-imp/imp.decl", 1, text, sizeof text));
    /* Mode 2 is never served. */
    CHECK(!provider_text(A_SEDEF, 2, text, sizeof text));
    /* Built-in defaults win over a bundle shadow of the same name. */
    CHECK(provider_text(LIFT, 0, text, sizeof text) && text[0] == '{' && strcmp(text, "MODLIFT"));
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
    CHECK(sh_modserve_probe_claims("generated/decls/md6Def/mods/alex.pink-imp/imp.md6.decl"));
    CHECK(sh_modserve_probe_claims("generated/decls/snapEditorEntityDef/mods/alex.pink-imp/imp.decl"));
    CHECK(!sh_modserve_probe_claims(A_MESH));                 /* raw files are not decls */
    CHECK(sh_modserve_probe_claims(SHADOW));                  /* conflicted, and served by its winner */
    CHECK(!sh_modserve_probe_claims("generated/decls/entitydef/monster/hellknight.decl"));
    CHECK(sh_modserve_is_mod_sedef("mods/alex.pink-imp/imp"));
    CHECK(sh_modserve_is_mod_sedef("MODS/alex.pink-imp/IMP"));
    CHECK(!sh_modserve_is_mod_sedef("demons/imp_enc"));
}

static void test_plan(void)
{
    sh_modserve_plan p;
    int md6, mat, ent, sedef, bsedef, bthing, i, unknown = 0;
    const char *hashes[3];
    CHECK(sh_modserve_plan_editor(&p) >= 4);
    md6 = step_index(&p, "md6Def", "mods/alex.pink-imp/imp.md6");
    mat = step_index(&p, "material2", "mods/alex.pink-imp/skin");
    ent = step_index(&p, "entityDef", "mods/alex.pink-imp/imp");
    sedef = step_index(&p, "snapEditorEntityDef", "mods/alex.pink-imp/imp");
    bsedef = step_index(&p, "snapEditorEntityDef", "mods/bob.skins/thing");
    bthing = step_index(&p, "entityDef", "mods/bob.skins/thing");
    CHECK(md6 >= 0 && mat >= 0 && ent >= 0 && sedef >= 0 && bsedef >= 0 && bthing >= 0);
    CHECK(mat < md6 && md6 < ent && ent < sedef);           /* dependency order */
    CHECK(md6 < bthing);                                     /* across bundles */
    CHECK(step_index(&p, "entityDef", "mods/alex.pink-imp/fireball") < 0);   /* gameplay only */
    CHECK(step_index(&p, "entityDef", "monster/imp") < 0);   /* shadows are never registered */
    for (i = 0; i < p.count; ++i) CHECK(_stricmp(p.steps[i].type, "baseModel"));   /* raw: no step */
    /* The editor plan keeps every step for the session (lane MG2). */
    CHECK(p.steps[sedef].persist == 1 && p.steps[ent].persist == 1 && p.steps[md6].persist == 1);
    CHECK(!strcmp(p.steps[md6].reg_path, "md6def/mods/alex.pink-imp/imp.md6.decl"));
    CHECK(p.steps[bsedef].blocked && strstr(p.steps[bsedef].blocked, "mods/bob.skins/missing.md6"));
    CHECK(!p.steps[bthing].blocked && !p.steps[sedef].blocked);
    CHECK(p.blocked == 1);
    sh_modserve_plan_free(&p);

    hashes[0] = g_hash_a;
    hashes[1] = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    hashes[2] = g_hash_a2;
    CHECK(sh_modserve_plan_bundles(hashes, 3, &p, &unknown) >= 3);
    CHECK(unknown == 1);
    CHECK(step_index(&p, "entityDef", "mods/alex.pink-imp/fireball") >= 0);
    CHECK(step_index(&p, "snapEditorEntityDef", "mods/alex.pink-imp/imp") < 0);
    CHECK(step_index(&p, "md6Def", "mods/alex.pink-imp/imp.md6") < step_index(&p, "entityDef", "mods/alex.pink-imp/imp"));
    CHECK(step_index(&p, "entityDef", "mods/bob.skins/thing") < 0);   /* not named */
    for (i = 0; i < p.count; ++i) CHECK(!p.steps[i].persist);
    sh_modserve_plan_free(&p);
}

static void test_plan_run(void)
{
    sh_modserve_plan p;
    int ready, total;
    const char *r_last, *l_first;
    sh_modserve_test_reset_registration();
    g_calls[0] = 0;
    g_fail_load = "mods/alex.pink-imp/imp";       /* the alex entityDef fails to load */
    g_already = "mods/alex.pink-imp/skin";        /* this source is already registered */
    log_clear();
    total = sh_modserve_plan_editor(&p);
    ready = sh_modserve_plan_run(&p, &g_fake, "editor");
    /* skin, md6 and bob's entityDef are ready; the alex entityDef fails, so
     * its sedef is not loaded; bob's sedef is blocked. */
    CHECK(ready == 3);
    CHECK(!strstr(g_calls, "R:material2:mods/alex.pink-imp/skin"));    /* source existed */
    CHECK(strstr(g_calls, "P:material2:mods/alex.pink-imp/skin") != NULL);
    CHECK(strstr(g_calls, "R:md6Def:mods/alex.pink-imp/imp.md6") != NULL);
    CHECK(strstr(g_calls, "P:entityDef:mods/alex.pink-imp/imp;") != NULL);
    CHECK(strstr(g_calls, "R:snapEditorEntityDef:mods/alex.pink-imp/imp;") != NULL);
    CHECK(!strstr(g_calls, "P:snapEditorEntityDef:mods/alex.pink-imp/imp;"));
    CHECK(!strstr(g_calls, "snapEditorEntityDef:mods/bob.skins/thing"));
    /* Every registration precedes every load. */
    r_last = strrchr(g_calls, 'R');
    l_first = strstr(g_calls, "P:");
    CHECK(r_last && l_first && r_last < l_first);
    CHECK(log_has("editor: entityDef 'mods/alex.pink-imp/imp' (alex.pink-imp) NOT READY: state 0x01"));
    CHECK(log_has("editor: snapEditorEntityDef 'mods/alex.pink-imp/imp' (alex.pink-imp) NOT READY: needs entityDef 'mods/alex.pink-imp/imp', which failed"));
    CHECK(log_has("editor: snapEditorEntityDef 'mods/bob.skins/thing' (bob.skins) BLOCKED"));
    CHECK(log_has("editor: entityDef 'mods/bob.skins/thing' (bob.skins) READY: state 0xa0 lifetime 4"));
    CHECK(log_has("editor: md6Def 'mods/alex.pink-imp/imp.md6' (alex.pink-imp) READY"));
    CHECK(p.steps[step_index(&p, "entityDef", "mods/bob.skins/thing")].ready == 1);
    CHECK(p.steps[step_index(&p, "snapEditorEntityDef", "mods/alex.pink-imp/imp")].ready == -1);
    sh_modserve_plan_free(&p);
    (void)total;

    /* A second run repeats nothing that is ready; the failed steps retry. */
    g_calls[0] = 0;
    g_fail_load = NULL;
    sh_modserve_plan_editor(&p);
    ready = sh_modserve_plan_run(&p, &g_fake, "editor");
    CHECK(!strstr(g_calls, "entityDef:mods/bob.skins/thing"));
    CHECK(!strstr(g_calls, "md6Def:mods/alex.pink-imp/imp.md6"));
    CHECK(strstr(g_calls, "P:entityDef:mods/alex.pink-imp/imp;") && strstr(g_calls, "P:snapEditorEntityDef:mods/alex.pink-imp/imp;"));
    CHECK(ready == p.count - p.blocked);
    sh_modserve_plan_free(&p);
    g_already = NULL;
}

static void test_bundles_for_names(void)
{
    char out[4][SH_MODSERVE_HASH_HEX + 1];
    const char *names[] = { "mods/alex.pink-imp/fireball", "snapmaps/placeablesnapaiencounter/imp", "MODS/bob.skins/thing" };
    int n = modserve_bundles_for_names(names, 3, out, 4), i, has_a2 = 0, has_b = 0, has_a = 0;
    /* fireball comes from 1.1.0; bob's thing needs alex 1.0.0's md6Def. */
    CHECK(n == 3);
    for (i = 0; i < n && i < 4; ++i) {
        has_a |= !strcmp(out[i], g_hash_a);
        has_a2 |= !strcmp(out[i], g_hash_a2);
        has_b |= !strcmp(out[i], g_hash_b);
    }
    CHECK(has_a && has_a2 && has_b);
    CHECK(modserve_bundles_for_names(names, 3, out, 1) == 3);   /* counts past cap */
    CHECK(modserve_bundles_for_names(names + 1, 1, out, 4) == 0);   /* stock */
}

static void test_cost(void)
{
    const int N = 1000000;
    const char *hit = "generated/decls/md6Def/mods/alex.pink-imp/imp.md6.decl";
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
        { "generated/decls/entitydef/mods/gus.new/a.decl", "entityDef a {}", -1, "entityDef", "mods/gus.new/a", NULL, '4' },
    };
    sh_modserve_plan p;
    make_bundle(g_root, "gus.new@1.0.0", "gus.new", "1.0.0", '4', NULL, g, 1, NULL);
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/mods/gus.new/a.decl", NULL));
    CHECK(sh_modserve_index_rescan() == 6);
    CHECK(sh_modserve_lookup("generated/decls/entitydef/mods/gus.new/a.decl", NULL));
    CHECK(sh_modserve_lookup(A_ENT, NULL));
    /* Registrations made before the rescan are kept: nothing re-runs. */
    g_calls[0] = 0;
    sh_modserve_plan_editor(&p);
    sh_modserve_plan_run(&p, &g_fake, "editor");
    CHECK(!strstr(g_calls, "mods/alex.pink-imp/imp"));
    sh_modserve_plan_free(&p);
}

/* ------------------------------------------------------------------ requirements */

/* A fake cvar system with both gates. A queued set lands only on drain; the
 * gate events and the registrations share one event sequence. */
static int g_gate_val[2], g_gate_pending[2], g_gate_has_pending[2];
static int g_gate_mode;          /* SH_MODSERVE_GATE_SET, _QUEUED, or 0 to refuse */
static int g_gate_reads, g_gate_sets, g_gate_drains;
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
static int gate_read(void *c, const char *cvar, int *value)
{
    int g = gate_index(cvar);
    (void)c;
    g_gate_reads++;
    if (g < 0) return 0;
    *value = g_gate_val[g];
    return 1;
}
static int gate_set(void *c, const char *cvar, const char *value)
{
    char e[96];
    int g = gate_index(cvar);
    (void)c;
    g_gate_sets++;
    _snprintf_s(e, sizeof e, _TRUNCATE, "S:%s=%s", cvar, value);
    event(e);
    if (g < 0 || !g_gate_mode) return 0;
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
static const sh_modserve_gate_ops g_gates = { NULL, gate_read, gate_set, gate_drain };
static const sh_modserve_gate_ops g_gates_nodrain = { NULL, gate_read, gate_set, NULL };

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
    g_gate_val[0] = image;
    g_gate_val[1] = resource;
    g_gate_has_pending[0] = g_gate_has_pending[1] = 0;
    g_gate_mode = mode;
    g_gate_reads = g_gate_sets = g_gate_drains = 0;
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

#define CY_MD6   "generated/decls/md6def/mods/cyd.cut/cyber.md6.decl"
#define CY_ENT   "generated/decls/entitydef/mods/cyd.cut/cyber.decl"
#define CY_SEDEF "generated/decls/snapeditorentitydef/mods/cyd.cut/cyber.decl"
#define IM_SEDEF "generated/decls/snapeditorentitydef/mods/img.only/font.decl"
#define PL_SEDEF "generated/decls/snapeditorentitydef/mods/plain.mod/thing.decl"

static void make_plain(const char *root)
{
    static const fx_file p[] = {
        { PL_SEDEF, "{ plain }", -1, "snapEditorEntityDef", "mods/plain.mod/thing", NULL, '1' },
    };
    g_bundle_extra = NULL;
    make_bundle(root, "plain.mod@1.0.0", "plain.mod", "1.0.0", '7', NULL, p, 1, NULL);
}

static void build_requirements_fixture(const char *root)
{
    static const fx_file cy[] = {
        { CY_MD6, "md6Def cyber {}", -1, "md6Def", "mods/cyd.cut/cyber.md6", NULL, '1' },
        { CY_ENT, "entityDef cyber {}", -1, "entityDef", "mods/cyd.cut/cyber",
          "[{\"type\":\"md6Def\",\"name\":\"mods/cyd.cut/cyber.md6\"}]", '2' },
        { CY_SEDEF, "{ entityDef \"mods/cyd.cut/cyber\" }", -1, "snapEditorEntityDef", "mods/cyd.cut/cyber",
          "[{\"type\":\"entityDef\",\"name\":\"mods/cyd.cut/cyber\"}]", '3' },
    };
    static const fx_file im[] = {
        { IM_SEDEF, "{ font }", -1, "snapEditorEntityDef", "mods/img.only/font", NULL, '4' },
    };
    static const fx_file one[] = {
        { "generated/decls/entitydef/mods/bad.x/a.decl", "x", -1, "entityDef", "mods/bad.x/a", NULL, '5' },
    };
    /* Both gates, in the converter's spelling and as a number. */
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\", \"g_useImageBlackList\": 0}},\n  ";
    make_bundle(root, "cyd.cut@1.0.0", "cyd.cut", "1.0.0", '1', NULL, cy, 3, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useImageBlackList\": \"0\"}},\n  ";
    make_bundle(root, "img.only@2.0.0", "img.only", "2.0.0", '2', NULL, im, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"com_maxfps\": \"0\"}},\n  ";
    make_bundle(root, "bad.cvar@1.0.0", "bad.x", "1.0.0", '3', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"1\"}},\n  ";
    make_bundle(root, "bad.value@1.0.0", "bad.x", "1.0.0", '4', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"dlls\": {\"x\": \"0\"}},\n  ";
    make_bundle(root, "bad.key@1.0.0", "bad.x", "1.0.0", '5', NULL, one, 1, NULL);
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

    /* Collection at index time: only the two gates at "0"; anything else is
     * that bundle's first cause; each requirement is logged once per bundle. */
    log_clear();
    CHECK(sh_modserve_index_build(req_root) == 3);
    CHECK(log_count("MODS: requirements: g_useResourceBlackList=0 for cyd.cut@1.0.0") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for cyd.cut@1.0.0") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for img.only@2.0.0") == 1);
    CHECK(log_count("MODS: requirements:") == 3);
    CHECK(log_has("bundle 'bad.cvar@1.0.0' skipped: requirement com_maxfps=\"0\" is refused (this build applies only "
                  "g_useImageBlackList=0 and g_useResourceBlackList=0)"));
    CHECK(log_has("bundle 'bad.value@1.0.0' skipped: requirement g_useResourceBlackList=\"1\" is refused"));
    CHECK(log_has("bundle 'bad.key@1.0.0' skipped: requirement 'dlls' is refused (only cvars are supported)"));
    CHECK(!log_has("plain.mod@1.0.0' skipped"));
    /* Union of the admitted bundles. */
    CHECK(sh_modserve_requirements() == (SH_MODSERVE_REQ_IMAGE | SH_MODSERVE_REQ_RESOURCE));
    CHECK(!strcmp(sh_modserve_requirement_cvar(0), "g_useImageBlackList") &&
          !strcmp(sh_modserve_requirement_cvar(1), "g_useResourceBlackList") && !sh_modserve_requirement_cvar(2));
    /* A rescan of the same bundles does not repeat the lines. */
    log_clear();
    CHECK(sh_modserve_index_rescan() == 3);
    CHECK(!log_has("MODS: requirements:"));

    /* Queued sets: phase 1 disarms, the buffer is drained, phase 2 reads back,
     * and only then does the first DeclRegisterFile run. */
    gates_reset(1, 1, SH_MODSERVE_GATE_QUEUED);
    log_clear();
    CHECK(sh_modserve_plan_editor(&p) == 5);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 5);
    CHECK(r.needed == 3 && r.changed == 3 && r.queued == 3 && r.drained == 1 && r.live == 3 && !r.not_live);
    CHECK(g_gate_val[0] == 0 && g_gate_val[1] == 0 && g_gate_drains == 1 && g_gate_sets == 2);
    CHECK(ev_first("S:g_useImageBlackList=0") >= 0 && ev_first("S:g_useResourceBlackList=0") >= 0);
    CHECK(ev_last("S:") < ev_first("D;") && ev_first("D;") < ev_first("R:"));
    CHECK(ev_first("R:md6Def:mods/cyd.cut/cyber.md6") > ev_first("D;"));
    CHECK(log_has("MODS: editor: requirements applied: g_useImageBlackList=0, g_useResourceBlackList=0 "
                  "(queued, live after a command-buffer drain)"));
    CHECK(sh_modserve_test_gates_owned() == 3);
    sh_modserve_plan_free(&p);

    /* Already live: read, never set or drained, and nothing logged. */
    sh_modserve_test_reset_registration();
    g_gate_sets = g_gate_drains = 0;
    log_clear();
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 5);
    CHECK(r.live == 3 && !r.changed && g_gate_sets == 0 && g_gate_drains == 0 && !log_has("requirements applied"));
    sh_modserve_plan_free(&p);

    /* An immediate route needs no drain. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "editor", &r) == 5);
    CHECK(r.live == 3 && r.changed == 3 && !r.queued && !r.drained && g_gate_drains == 0);
    CHECK(ev_last("S:") < ev_first("R:"));
    sh_modserve_plan_free(&p);

    /* Queued with nothing to drain it: the gates are not live, so the bundles
     * that need them are blocked and register nothing; the plain bundle runs. */
    gates_reset(1, 1, SH_MODSERVE_GATE_QUEUED);
    log_clear();
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates_nodrain, "editor", &r) == 1);
    CHECK(r.not_live == 3 && !r.live && !r.drained);
    CHECK(ev_first("R:snapEditorEntityDef:mods/plain.mod/thing") >= 0);
    CHECK(ev_first("cyd.cut") < 0 && ev_first("img.only") < 0);
    CHECK(log_has("queued but the command buffer could not be drained"));
    CHECK(log_has("editor: md6Def 'mods/cyd.cut/cyber.md6' (cyd.cut) BLOCKED: requirements g_useImageBlackList=0, "
                  "g_useResourceBlackList=0 are not live"));
    CHECK(log_has("(img.only) BLOCKED: requirement g_useImageBlackList=0 is not live"));
    sh_modserve_plan_free(&p);

    /* A refused set and an unavailable cvar system block the same way. */
    gates_reset(1, 0, 0);
    sh_modserve_plan_editor(&p);
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 1);
    CHECK(r.not_live == SH_MODSERVE_REQ_IMAGE && r.live == SH_MODSERVE_REQ_RESOURCE);
    sh_modserve_plan_free(&p);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_apply(NULL, "map", &r) == -1 && r.not_live == 3);
    CHECK(log_has("MODS: map: requirements g_useImageBlackList=0, g_useResourceBlackList=0 NOT applied"));

    /* Gates are released when no admitted bundle needs them any more, to the
     * value they displaced; after that nothing is touched again. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 1 && sh_modserve_test_gates_owned() == 3);
    CHECK(sh_modserve_index_build(plain_root) == 1);
    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0);
    CHECK(r.restored == 3 && g_gate_val[0] == 1 && g_gate_val[1] == 1 && sh_modserve_test_gates_owned() == 0);
    CHECK(log_has("requirement released: g_useResourceBlackList restored to 1"));
    g_gate_reads = g_gate_sets = 0;
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0 && g_gate_reads == 0 && g_gate_sets == 0);

    /* The empty case: no bundle needs a requirement, so the cvars are never
     * read or set, by the apply or by a gated plan run. */
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    CHECK(sh_modserve_requirements() == 0);
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == 0 && r.needed == 0);
    sh_modserve_plan_editor(&p);
    CHECK(p.count == 1 && sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 1);
    CHECK(g_gate_reads == 0 && g_gate_sets == 0 && g_gate_drains == 0 && ev_first("S:") < 0);
    sh_modserve_plan_free(&p);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* ------------------------------------------------------------------ conflicts (lane MG2) */

char *sh_modserve_test_compose(const char *const *texts, int n, int *added);

#define CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define WHITELIST "generated/decls/snappropertyinspector_whitelistencounterdecl/single_encounters_white_list_inspector.decl"
#define CYB_AW    "generated/decls/animweb/zion/boss/cyberdemon_ai.decl"
#define NEEDY     "generated/decls/aipaingraph/cyberdemon_ai.decl"

#define CONDUCTOR_HEAD \
    "{\r\n\tinherit = \"ai/base/conductor\";\r\n\tclass = \"idAIConductor_Coop\";\r\n" \
    "\teditorVars {\r\n\t\tcolor = ( 1, 1, 1, 1 );\r\n\t}\r\n\tedit = {\r\n\t\taiTypeList = {\r\n"
#define CONDUCTOR_STOCK_ITEMS \
    "\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n" \
    "\t\t\titem[1] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_ARCHVILE\";\r\n\t\t\t\tentityDef = \"ai/demon/archvile_coop\";\r\n\t\t\t}\r\n"
#define CONDUCTOR_TAIL "\t\t}\r\n\t\tglobalAIMods = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"SPAWN_AI_TYPE_IMP\";\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}"
#define ITEM(n, type, def) "\t\t\titem[" #n "] = {\r\n\t\t\t\taiType = \"" type "\";\r\n\t\t\t\tentityDef = \"" def "\";\r\n\t\t\t}\r\n"

#define CONDUCTOR_CYBER CONDUCTOR_HEAD "\t\t\tnum = 3;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "524288", "ai/demon/cyberdemon_hell") CONDUCTOR_TAIL
#define CONDUCTOR_GUARD CONDUCTOR_HEAD "\t\t\tnum = 4;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "2097152", "ai/demon/talismanguard/staff") ITEM(3, "4194304", "ai/demon/talismanguard/hammer") CONDUCTOR_TAIL
#define CONDUCTOR_SPIDER CONDUCTOR_HEAD "\t\t\tnum = 3;\r\n" CONDUCTOR_STOCK_ITEMS \
    ITEM(2, "1048576", "ai/demon/spider_mastermind_base") CONDUCTOR_TAIL

#define WL_A "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 3;\n\t\t\titem[0] = \"single_archvile\";\n" \
    "\t\t\titem[1] = \"single_zombie\";\n\t\t\titem[2] = \"single_cyberdemon\";\n\t\t}\n\t}\n}\n"
#define WL_B "{\n\tedit = {\n\t\tvalidEncounters = {\n\t\t\tnum = 3;\n\t\t\titem[0] = \"single_archvile\";\n" \
    "\t\t\titem[1] = \"single_zombie\";\n\t\t\titem[2] = \"single_spider_mastermind\";\n\t\t}\n\t}\n}\n"

/* animWeb-style text: outside the composable grammar (no '='). */
#define AW_TEXT(extra) "{\n\tprops {\n\t\tgridSize 32\n\t\tmodelInfos {\n\t\t\t\"zion/characters/monsters/cyberdemon/base/cyberdemon.md6\" \"\"\n\t\t}\n\t}\n" extra "}\n"

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
}

static void test_conflicts(void)
{
    char top[MAX_PATH], root[MAX_PATH], legacy[MAX_PATH], text[4096], aw_hash[65];
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
    _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", root);
    mkdirs(legacy);
    g_bundle_extra = "\"extra\": {\"legacy\": {}},\n  ";
    {
        /* The owner's tree, reduced: two builds of one authored boss mod with
         * the same animWeb bytes, the map-delivered copy of it with one line
         * different, and two more bosses, all appending to the conductor and
         * the whitelist. */
        static const fx_file cy[] = {
            { CYB_AW, AW_TEXT("\tcustomFlags AW_CF_IDLE_CYCLE\n"), -1, "animWeb", "zion/boss/cyberdemon_ai", NULL, 'a' },
            { CONDUCTOR, CONDUCTOR_CYBER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'c' },
            { WHITELIST, WL_A, -1, "snapPropertyInspector_WhiteListEncounterDecl", "single_encounters_white_list_inspector", NULL, 'w' },
        };
        static const fx_file dl[] = {
            { CYB_AW, AW_TEXT(""), -1, "animWeb", "zion/boss/cyberdemon_ai", NULL, 'b' },
            { CONDUCTOR, CONDUCTOR_CYBER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'c' },
        };
        static const fx_file gd[] = {
            { CONDUCTOR, CONDUCTOR_GUARD, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'd' },
        };
        static const fx_file sp[] = {
            { CONDUCTOR, CONDUCTOR_SPIDER, -1, "entityDef", "ai/conductor/coop/snapmap_default", NULL, 'e' },
            { WHITELIST, WL_B, -1, "snapPropertyInspector_WhiteListEncounterDecl", "single_encounters_white_list_inspector", NULL, 'f' },
        };
        make_bundle(root, "local.cyberdemon@0.0.0+1", "local.cyberdemon", "0.0.0", '1',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\", \"" WHITELIST "\"", cy, 3, "{\"validated\": true, \"installedAt\": 10}");
        make_bundle(root, "local.cyberdemon@0.0.0-05238b2e+1", "local.cyberdemon", "0.0.0-05238b2e", '2',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\", \"" WHITELIST "\"", cy, 3, "{\"validated\": true, \"installedAt\": 11}");
        make_bundle(legacy, "cyberdemon@3d0fabc7bbc8194f", "cyberdemon", "legacy-3d0fabc7bbc8194f", '3',
                    "\"" CYB_AW "\", \"" CONDUCTOR "\"", dl, 2, "{\"validated\": true, \"installedAt\": 99}");
        make_bundle(root, "local.hell-guard@0.0.0+1", "local.hell-guard", "0.0.0", '4', "\"" CONDUCTOR "\"", gd, 1,
                    "{\"validated\": true, \"installedAt\": 12}");
        make_bundle(root, "local.spider-mastermind@0.0.0+1", "local.spider-mastermind", "0.0.0", '5',
                    "\"" CONDUCTOR "\", \"" WHITELIST "\"", sp, 2, "{\"validated\": true, \"installedAt\": 13}");
    }
    {
        /* Needs decide: the later-installed copy names an md6Def nothing installed provides. */
        static const fx_file n1[] = {
            { NEEDY, "{ pain a }", -1, "aiPainGraph", "cyberdemon_ai", "[{\"type\":\"md6Def\",\"name\":\"zion/nowhere.md6\"}]", '1' },
        };
        static const fx_file n2[] = {
            { NEEDY, "{ pain b }", -1, "aiPainGraph", "cyberdemon_ai", "[{\"type\":\"md6Def\",\"name\":\"zion/here.md6\"}]", '2' },
            { "generated/decls/md6def/zion/here.md6.decl", "{ md6 }", -1, "md6Def", "zion/here.md6", NULL, '3' },
        };
        make_bundle(root, "needs.late@1.0.0", "needs.late", "1.0.0", '6', "\"" NEEDY "\"", n1, 1,
                    "{\"validated\": true, \"installedAt\": 500}");
        make_bundle(root, "needs.ok@1.0.0", "needs.ok", "1.0.0", '7', "\"" NEEDY "\", \"generated/decls/md6def/zion/here.md6.decl\"",
                    n2, 2, "{\"validated\": true, \"installedAt\": 400}");
    }
    {
        /* animWebs naming models: missing (a new name and a stock shadow),
         * provided by the bundle, and known to the engine. */
        static const fx_file aw[] = {
            { "generated/decls/animweb/designsystems/med_station.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/objects/props_set/kiosks/wallhealthstation.md6\" \"\"\t// modelIndex 0\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "designsystems/med_station", NULL, '1' },
            { "generated/decls/animweb/designsystems/good.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/props/door.md6\" \"\"\n\t\t\t\"zion/stock/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "designsystems/good", NULL, '2' },
            { "generated/decls/md6def/zion/props/door.md6.decl", "{ md6 }", -1, "md6Def", "zion/props/door.md6", NULL, '3' },
            { "generated/decls/animweb/player/fp_hands_mp.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/player/human/base/mp.md6\" \"\"\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "player/fp_hands_mp", NULL, '4' },
            { "generated/decls/animweb/player/fp_hands_sp.decl",
              "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/player/human/base/sp_gone.md6\" \"\"\n\t\t}\n\t}\n}\n",
              -1, "animWeb", "player/fp_hands_sp", NULL, '5' },
        };
        g_bundle_extra = "\"extra\": {\"legacy\": {}},\n  \"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\"}},\n  ";
        make_bundle(root, "aw.pack@0.0.0+1", "aw.pack", "0.0.0", '8',
                    "\"generated/decls/animweb/designsystems/med_station.decl\", \"generated/decls/animweb/designsystems/good.decl\", "
                    "\"generated/decls/md6def/zion/props/door.md6.decl\", \"generated/decls/animweb/player/fp_hands_mp.decl\", "
                    "\"generated/decls/animweb/player/fp_hands_sp.decl\"",
                    aw, 5, NULL);
        hash64('8', aw_hash);
    }
    g_bundle_extra = NULL;

    log_clear();
    CHECK(sh_modserve_index_build(root) == 8);
    /* The animWeb the boss bundles carry: one copy serves, never none. */
    CHECK(log_has("conflict: shadow '" CYB_AW "' is provided by local.cyberdemon@0.0.0, local.cyberdemon@0.0.0-05238b2e, "
                  "cyberdemon@legacy-3d0fabc7bbc8194f; serving local.cyberdemon@0.0.0-05238b2e (most providers carry these bytes)"));
    CHECK(sh_modserve_lookup(CYB_AW, &hit) && !strcmp(hit.bundle_id, "local.cyberdemon"));
    CHECK(open_text(CYB_AW, text, sizeof text) == 1 && strstr(text, "AW_CF_IDLE_CYCLE"));
    CHECK(sh_modserve_probe_claims(CYB_AW));
    /* The conductor: every boss's aiTypeList entry is served. */
    CHECK(log_has("conflict: shadow '" CONDUCTOR "' is provided by local.cyberdemon@0.0.0, local.cyberdemon@0.0.0-05238b2e, "
                  "local.hell-guard@0.0.0, local.spider-mastermind@0.0.0, cyberdemon@legacy-3d0fabc7bbc8194f; serving "));
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
    CHECK(log_has("conflict: shadow '" NEEDY "' is provided by needs.late@1.0.0, needs.ok@1.0.0; serving needs.ok@1.0.0 "
                  "(its needs resolve against the installed bundles where the others' do not)"));
    CHECK(open_text(NEEDY, text, sizeof text) == 1 && !strcmp(text, "{ pain b }"));
    CHECK(!log_has("serving neither"));

    /* Registration: an animWeb whose models block names a model nothing
     * provides is withdrawn; the stock one is served by the game again. A
     * model the registry does not know yet but the game archives hold (the
     * player hands) keeps its shadow served. */
    hashes[0] = aw_hash;
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 5 && unknown == 0);
    log_clear();
    g_calls[0] = 0;
    ready = sh_modserve_plan_run(&p, &g_cf, "map");
    CHECK(ready == 4);
    CHECK(log_has("map: animWeb 'designsystems/med_station' (aw.pack) NOT READY: it names model "
                  "'zion/objects/props_set/kiosks/wallhealthstation.md6', which neither a bundle nor the game provides"));
    CHECK(log_has("map: animWeb 'player/fp_hands_sp' (aw.pack) shadow WITHDRAWN: it names model 'zion/player/human/base/sp_gone.md6'"));
    CHECK(log_has("map: animWeb 'player/fp_hands_mp' (aw.pack) shadows a stock decl; served by the provider"));
    CHECK(!log_has("'player/fp_hands_mp' (aw.pack) shadow WITHDRAWN"));
    CHECK(log_has("map: animWeb 'designsystems/good' (aw.pack) READY"));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/designsystems/med_station.decl", NULL));
    CHECK(!sh_modserve_probe_claims("generated/decls/animweb/designsystems/med_station.decl"));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/player/fp_hands_sp.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/player/fp_hands_mp.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/designsystems/good.decl", NULL));
    sh_modserve_plan_free(&p);
    /* A rescan (an install, a conversion) keeps them withdrawn. */
    CHECK(sh_modserve_index_rescan() == 8);
    CHECK(!sh_modserve_lookup("generated/decls/animweb/designsystems/med_station.decl", NULL));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/player/fp_hands_sp.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/player/fp_hands_mp.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/designsystems/good.decl", NULL));
    CHECK(sh_modserve_lookup(CONDUCTOR, &hit) && hit.size > 0);

    /* The Play-time re-check: live gates log once and set nothing; a gate set
     * back to 1 is disarmed again. */
    gates_reset(1, 0, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_check(&g_gates, "play", &r) == 1 && g_gate_sets == 0);
    CHECK(log_has("MODS: play: requirements live: g_useResourceBlackList=0 (unchanged since the last pass)"));
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_requirements_check(&g_gates, "play", &r) == 1 && g_gate_val[1] == 0 && r.changed == SH_MODSERVE_REQ_RESOURCE);
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
        { "generated/decls/snapeditorentitydef/demons/cyberdemon_enc.decl",
          "{\n\tinherit = \"demons/demon_base\";\n\tedit = {\n"
          "\t\tpreviewImage = \"textures/guis/snapmaps/entity_icons/demons/cyberdemon_enc\";\n"
          "\t\tentityDef = \"snapmaps/placeablesnapaiencounter/cyberdemon\";\n"
          "\t\trenderInfo = {\n\t\t\trenderModelName = \"zion/characters/monsters/cyberdemon/base/cyberdemon_snapeditor.md6\";\n\t\t}\n"
          "\t\teventInfo = {\n\t\t\tanimWebDecl = \"zion/snapmap/demons/cyberdemon_snapeditor\";\n\t\t}\n\t}\n}\n",
          -1, "snapEditorEntityDef", "demons/cyberdemon_enc", NULL, '1' },
        { "generated/decls/snapeditorentitydef/prop/crate.decl",
          "{\n\tedit = {\n\t\trenderInfo = {\n\t\t\trenderModelName = \"models/mapobjects/crate.lwo\";\n\t\t}\n\t}\n}\n",
          -1, "snapEditorEntityDef", "prop/crate", NULL, '2' },
        { "generated/decls/animweb/zion/snapmap/demons/cyberdemon_snapeditor.decl", "{ web }", -1, "animWeb",
          "zion/snapmap/demons/cyberdemon_snapeditor", NULL, '3' },
    };
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\edrefs\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"extra\": {\"legacy\": {}},\n  ";
    make_bundle(root, "local.cyberdemon@0.0.0+1", "local.cyberdemon", "0.0.0", '9',
                "\"generated/decls/snapeditorentitydef/demons/cyberdemon_enc.decl\", "
                "\"generated/decls/snapeditorentitydef/prop/crate.decl\", "
                "\"generated/decls/animweb/zion/snapmap/demons/cyberdemon_snapeditor.decl\"", row, 3, NULL);
    g_bundle_extra = NULL;
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
    CHECK(strstr(g_refs, "animWeb:zion/snapmap/demons/cyberdemon_snapeditor<demons/cyberdemon_enc;") != NULL);
    CHECK(strstr(g_refs, "md6Def:zion/characters/monsters/cyberdemon/base/cyberdemon_snapeditor.md6<demons/cyberdemon_enc;") != NULL);
    CHECK(strstr(g_refs, "material:textures/guis/snapmaps/entity_icons/demons/cyberdemon_enc<demons/cyberdemon_enc;") != NULL);
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
}

static void remove_tree(const char *dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
            else DeleteFileA(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

int main(void)
{
    char top[MAX_PATH];
    char *slash;
    build_fixture();
    test_empty_index();
    test_index_and_isolation();
    test_lookup();
    test_precedence();
    test_probe();
    test_plan();
    test_plan_run();
    test_bundles_for_names();
    test_cost();
    test_rescan();
    test_requirements();
    test_conflicts();
    test_editor_refs();
    test_watermark();
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
