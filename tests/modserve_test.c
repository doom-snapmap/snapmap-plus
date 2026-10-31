/* modserve_test.c -- the installed-mod index and provider (modserve.c)
 * against a fixture mods tree written to a temporary folder: admission,
 * failure isolation (size mismatch, unvalidated state, namespace, unbuilt
 * folder), a shadow conflict, version supersession, a delivered package,
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
#include "modmanifest.h"
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

/* Raw author-block members (with a trailing comma) the next make_bundle writes. */
static const char *g_bundle_extra;
/* The author's "name" the next make_bundle writes (NULL: the id). */
static const char *g_bundle_name;

/* Write <root>\<folder>\mod.json (an author block and its product block) and
 * its files. A folder written under <root>\legacy stands for a package a map
 * delivered: it goes to <root> itself with a "legacy" record (Contract 5 has
 * no legacy folder); an author "legacy" record elsewhere is a migrated pack's
 * ("source": "overrides"). `state` is the old .state.json shape, mapped to
 * the block's validated, cause and built. */
static void make_bundle(const char *parent, const char *folder, const char *id, const char *version,
                        char hash_digit, const char *shadows, const fx_file *files, int n, const char *state)
{
    char dir[MAX_PATH], path[MAX_PATH], h[65], top[MAX_PATH], author[2048];
    static char json[32768];
    size_t at = 0;
    int i, delivered = 0;
    const char *extra = g_bundle_extra ? g_bundle_extra : "";
    strcpy_s(top, sizeof top, parent);
    {
        size_t tl = strlen(top);
        if (tl > 7 && !_stricmp(top + tl - 7, "\\legacy")) { top[tl - 7] = 0; delivered = 1; }
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", top, folder);
    mkdirs(dir);
    author[0] = 0;
    {
        const char *lg = strstr(extra, "\"legacy\": {}");
        if (lg && !delivered) {
            /* A migrated pack: its legacy record names the overrides source. */
            _snprintf_s(author, sizeof author, _TRUNCATE, "%.*s\"legacy\": {\"source\": \"overrides\"}%s", (int)(lg - extra), extra,
                        lg + 12);
        } else if (!lg && delivered) {
            _snprintf_s(author, sizeof author, _TRUNCATE, "\"legacy\": {}, %s", extra);
        } else strcpy_s(author, sizeof author, extra);
    }
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"id\": \"%s\",\n  \"version\": \"%s\",\n  \"name\": \"%s\",\n  %s\n"
                      "  \"snapmap-plus\": {\"format\": 1, \"build\": 1, \"bundle\": \"%s\"",
                      id, version, g_bundle_name ? g_bundle_name : id, author, hash64(hash_digit, h));
    if (state) {
        const char *v = strstr(state, "\"validated\": false"), *c = strstr(state, "\"firstCause\": \""), *t = strstr(state, "\"installedAt\": ");
        if (v) at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ", \"validated\": false");
        if (c) {
            const char *e = strchr(c + 15, '"');
            at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ", \"cause\": \"%.*s\"", (int)(e - c - 15), c + 15);
        }
        if (t) at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ", \"built\": %d", atoi(t + 15));
    }
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, ",\n    \"shadows\": [%s],\n    \"files\": [\n", shadows ? shadows : "");
    for (i = 0; i < n; ++i) {
        const fx_file *f = &files[i];
        long long size = f->size >= 0 ? f->size : (long long)strlen(f->body ? f->body : "");
        char b[65];
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
    char tmp[MAX_PATH], path[MAX_PATH], legacy[MAX_PATH];
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
        /* A raw file: its own tree holds it (there is no blob store). */
        static const fx_file f[] = { { F_BLOB, "BLOBBYTES", 9, "image", "mods/fay.blob/tex", NULL, '2' } };
        make_bundle(g_root, "fay.blob@1.0.0", "fay.blob", "1.0.0", '2', NULL, f, 1, NULL);
        hash64('2', g_hash_f);
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
    {
        /* A sound bank is named by its stem: <id with '.' and '-' as '_'>_<rest>
         * is inside the namespace, any other stem is outside it. */
        static const fx_file g[] = {
            { "sound/soundbanks/pc/gail_snd_fx.bnk", "BKHD", -1, "soundBank", "sound/soundbanks/pc/gail_snd_fx.bnk", NULL, '5' },
        };
        static const fx_file h[] = {
            { "sound/soundbanks/pc/other_fx.bnk", "BKHD", -1, "soundBank", "sound/soundbanks/pc/other_fx.bnk", NULL, '6' },
        };
        make_bundle(g_root, "gail.snd@1.0.0", "gail.snd", "1.0.0", '5', NULL, g, 1, NULL);
        make_bundle(g_root, "hank.snd@1.0.0", "hank.snd", "1.0.0", '6', NULL, h, 1, NULL);
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
    CHECK(sh_modserve_index_build(g_root) == 6);   /* alex x2, bob, fay, gail, legacy */
    sh_modserve_get_stats(&st);
    CHECK(st.bundles_seen == 11);
    CHECK(st.bundles_admitted == 6);
    CHECK(st.conflicts == 1);
    CHECK(log_has("bundle 'carol.broken@1.0.0' skipped: size mismatch: '" C_ONLY "' is 17 bytes, mod.json says 999"));
    CHECK(log_has("bundle 'dave.x@1.0.0' skipped: not validated: md6Def mesh missing"));
    CHECK(log_has("bundle 'erin.ns@1.0.0' skipped: 'generated/decls/entitydef/monster/sneaky.decl' is outside mods/erin.ns/"));
    CHECK(log_has("bundle 'unbuilt' skipped: not built yet"));
    CHECK(log_has("bundle 'hank.snd@1.0.0' skipped: 'sound/soundbanks/pc/other_fx.bnk' is outside mods/hank.snd/"));
    CHECK(!log_has("bundle 'gail.snd@1.0.0' skipped"));
    CHECK(log_count("skipped:") == 5);   /* one line per skipped bundle */
    /* Two mods' shadows of one stock decl: one serves (here the more
     * recently installed; the texts are outside the composable grammar). */
    CHECK(log_has("conflict: shadow '" SHADOW "' is provided by alex.pink-imp@1.0.0, bob.skins@2.0.0; serving "
                  "alex.pink-imp@1.0.0 (installed most recently)"));
    CHECK(log_has("bundle alex.pink-imp@1.0.0: superseded by alex.pink-imp@1.1.0"));
    CHECK(log_has("MODS: index built in ") && log_has("(6 bundles, "));
    CHECK(sh_modserve_bundle_state(g_hash_a, NULL, 0) == 1);
    {
        char cause[200];
        CHECK(sh_modserve_bundle_state(g_hash_c, cause, sizeof cause) == -1 && strstr(cause, "size mismatch"));
    }
    CHECK(sh_modserve_bundle_state("0000000000000000000000000000000000000000000000000000000000000000", NULL, 0) == 0);
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
    CHECK(strstr(u.order, "entityDef:monster/imp:alex.pink-imp;") && strstr(u.order, "snapEditorEntityDef:func/lift:alex.pink-imp;"));
    CHECK(strstr(u.order, "entityDef:monster/imp:") < strstr(u.order, "snapEditorEntityDef:func/lift:"));
    CHECK(!strstr(u.order, "bob.skins"));
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
    /* Bytes, version supersession and a delivered package. */
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

/* A map pinning an earlier build of a folder: the record's alias names the
 * current build, and the plan resolves the pinned hash through it. */
static void test_plan_alias(void)
{
    static const char OLD[] = "1234567890123456789012345678901234567890123456789012345678901234";
    char aliases[160];
    sh_modserve_plan p;
    const char *hashes[1];
    int unknown = 0, n;
    _snprintf_s(aliases, sizeof aliases, _TRUNCATE, "{\"%s\":\"%s\"}", OLD, g_hash_a);
    CHECK(sh_mb_record_put(g_root, "aliases", aliases));
    n = sh_modserve_index_build(g_root);
    CHECK(n == 6);
    hashes[0] = OLD;
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) >= 1 && unknown == 0);
    CHECK(step_index(&p, "entityDef", "mods/alex.pink-imp/fireball") >= 0);
    sh_modserve_plan_free(&p);
    CHECK(sh_modserve_bundle_state(OLD, NULL, 0) == 1);
    /* Without the alias the pinned hash is unknown. */
    CHECK(sh_mb_record_put(g_root, "aliases", NULL));
    CHECK(sh_modserve_index_build(g_root) == n);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 0 && unknown == 1);
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
    ready = sh_modserve_plan_run(&p, &g_fake_published, "editor");
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
    /* The early-decl hook runs once, after the registrations and before the loads. */
    CHECK(log_count_in(g_calls, "S::;") == 1 && strstr(g_calls, "S::;") > r_last && strstr(g_calls, "S::;") < l_first);
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
    CHECK(sh_modserve_index_rescan() == 7);
    CHECK(sh_modserve_lookup("generated/decls/entitydef/mods/gus.new/a.decl", NULL));
    CHECK(sh_modserve_lookup(A_ENT, NULL));
    /* Registrations made before the rescan are kept: nothing re-runs. */
    g_calls[0] = 0;
    sh_modserve_plan_editor(&p);
    sh_modserve_plan_run(&p, &g_fake, "editor");
    CHECK(!strstr(g_calls, "mods/alex.pink-imp/imp"));
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
static int gate_read(void *c, const char *cvar, char *value, size_t cap)
{
    int g = gate_index(cvar), t = text_index(cvar);
    (void)c;
    g_gate_reads++;
    if (g >= 0) { _snprintf_s(value, cap, _TRUNCATE, "%d", g_gate_val[g]); return 1; }
    if (t >= 0) { strncpy_s(value, cap, g_text_val[t], _TRUNCATE); return 1; }
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
    /* What the builder refuses: a name that is not a cvar's, a decimal
     * number, a line holding two commands, an unknown key. */
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"9lives\": \"0\"}},\n  ";
    make_bundle(root, "bad.cvar@1.0.0", "bad.x", "1.0.0", '3', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"com_maxFps\": 0.5}},\n  ";
    make_bundle(root, "bad.value@1.0.0", "bad.x", "1.0.0", '4', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"dlls\": {\"x\": \"0\"}},\n  ";
    make_bundle(root, "bad.key@1.0.0", "bad.x", "1.0.0", '5', NULL, one, 1, NULL);
    g_bundle_extra = "\"requirements\": {\"commands\": {\"activate\": [\"echo a; quit\"]}},\n  ";
    make_bundle(root, "bad.line@1.0.0", "bad.x", "1.0.0", '6', NULL, one, 1, NULL);
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
    CHECK(log_count("MODS: requirements: g_useResourceBlackList=0 for cyd.cut@1.0.0") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for cyd.cut@1.0.0") == 1);
    CHECK(log_count("MODS: requirements: g_useImageBlackList=0 for img.only@2.0.0") == 1);
    CHECK(log_count("MODS: requirements:") == 3);
    CHECK(log_has("bundle 'bad.cvar@1.0.0' skipped: requirements.cvars key 9lives is not a cvar name"));
    CHECK(log_has("bundle 'bad.value@1.0.0' skipped: requirements.cvars.com_maxFps must be a string, an integer or a boolean"));
    CHECK(log_has("bundle 'bad.key@1.0.0' skipped: requirements key dlls is not supported (cvars, commands)"));
    CHECK(log_has("bundle 'bad.line@1.0.0' skipped: requirements.commands.activate[0] must be one console command"));
    CHECK(!log_has("plain.mod@1.0.0' skipped"));
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
    CHECK(ev_first("R:md6Def:mods/cyd.cut/cyber.md6") > ev_first("D;"));
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
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s@1.0.0", id);
    _snprintf_s(pname, sizeof pname, _TRUNCATE, "mods/%s/thing", id);
    _snprintf_s(path, sizeof path, _TRUNCATE, "generated/decls/snapeditorentitydef/mods/%s/thing.decl", id);
    _snprintf_s(state, sizeof state, _TRUNCATE, "\"installedAt\": %d", installed);
    _snprintf_s(extra, sizeof extra, _TRUNCATE, "\"requirements\": %s,\n  ", req);
    f[0].path = path; f[0].body = "{ thing }"; f[0].size = -1; f[0].ptype = "snapEditorEntityDef"; f[0].pname = pname;
    f[0].needs = NULL; f[0].blob = digit;
    g_bundle_extra = extra;
    make_bundle(root, folder, id, "1.0.0", digit, NULL, f, 1, state);
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
    make_gen(root, "a.first", 'a', A, 100);
    make_gen(root, "b.second", 'b', B, 200);
    make_gen(root, "c.locked", 'c', C, 300);
    make_gen(root, "d.same", 'd', D, 400);
    make_gen(root, "e.lines", 'e', E, 500);
    make_gen(root2, "c.locked", 'c', C, 300);
    make_gen(root2, "d.same", 'd', D, 400);
    make_gen(root2, "e.lines", 'e', E, 500);

    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 5);
    /* The later install asking another value loses, both named. */
    CHECK(log_has("MODS: requirements: conflict: com_maxFps: b.second@1.0.0 asks \"60\", a.first@1.0.0 asks \"120\"; "
                  "b.second@1.0.0, the later install, loses"));
    CHECK(log_has("MODS: requirements: com_maxFps=120 for a.first@1.0.0") && log_has("MODS: requirements: r_fancy=1 for a.first@1.0.0"));
    CHECK(log_has("MODS: requirements: 2 activate and 1 deactivate command(s) for a.first@1.0.0"));
    CHECK(log_has("MODS: requirements: com_maxFps=120.0 for d.same@1.0.0") && !log_has("for b.second@1.0.0"));
    CHECK(sh_modserve_requirements() == 4);

    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "editor", &r) == -1);
    CHECK(r.needed == 4 && r.live == 3 && r.not_live == 1 && r.commands == 2);
    CHECK(!strcmp(g_text_val[0], "120") && !strcmp(g_text_val[1], "1") && !strcmp(g_text_val[3], "two words") &&
          !strcmp(g_text_val[2], "0"));
    /* Activate lines run after the cvars, in order, trimmed, once. */
    CHECK(ev_first("C:echo first on") > ev_last("S:") && ev_first("C:bind x \"say hi\";") > ev_first("C:echo first on"));
    CHECK(ev_first("C:echo second on") < 0 && ev_first("C:echo e off") < 0 && ev_first("C:echo first off") < 0);
    CHECK(log_has("MODS: requirements: a.first: command \"echo first on\""));
    CHECK(log_has("MODS: requirements: a.first: command \"bind x \"say hi\"\""));
    CHECK(log_has("MODS: editor: requirement ro_locked=1 (c.locked@1.0.0) is not live: the engine marks it read-only"));
    /* Only the bundles whose requirements are not live are blocked. */
    sh_modserve_plan_editor(&p);
    CHECK(p.count == 5);
    log_clear();
    CHECK(sh_modserve_plan_run_gated(&p, &g_ev, &g_gates, "editor", &r) == 3);
    CHECK(log_has("(b.second) BLOCKED: requirement com_maxFps=60 lost to a.first@1.0.0, installed earlier, which asks \"120\""));
    CHECK(log_has("(c.locked) BLOCKED: requirement ro_locked=1 is not live"));
    CHECK(ev_first("R:snapEditorEntityDef:mods/a.first/thing") >= 0 && ev_first("R:snapEditorEntityDef:mods/d.same/thing") >= 0 &&
          ev_first("R:snapEditorEntityDef:mods/e.lines/thing") >= 0);
    sh_modserve_plan_free(&p);
    /* Nothing runs twice. */
    CHECK(g_gate_lines == 2 && !log_has("command \""));
    g_report[0] = 0;
    CHECK(sh_modserve_requirements_report(report_line, NULL) >= 8);
    CHECK(strstr(g_report, "requirements: a.first@1.0.0: cvar com_maxFps=120: live\n") != NULL);
    CHECK(strstr(g_report, "requirements: d.same@1.0.0: cvar g_label=two words: live\n") != NULL);
    CHECK(strstr(g_report, "requirements: a.first@1.0.0: activate command \"echo first on\": ran\n") != NULL);
    CHECK(strstr(g_report, "requirements: a.first@1.0.0: deactivate command \"echo first off\": runs when the mod stops\n") != NULL);
    CHECK(strstr(g_report, "requirements: b.second@1.0.0: cvar com_maxFps=60: refused (com_maxFps=60 lost to a.first@1.0.0") != NULL);
    CHECK(strstr(g_report, "requirements: b.second@1.0.0: activate command \"echo second on\": not run") != NULL);
    CHECK(strstr(g_report, "requirements: c.locked@1.0.0: cvar ro_locked=1: NOT live (the engine marks it read-only; its steps are blocked)\n") != NULL);
    CHECK(strstr(g_report, "requirements: e.lines@1.0.0: deactivate command \"echo e off\": runs when the mod stops\n") != NULL);

    /* a.first and b.second go: a.first's deactivate line runs, r_fancy is
     * restored, com_maxFps stays (d.same asks the same number). */
    CHECK(sh_modserve_index_build(root2) == 3);
    log_clear();
    g_events[0] = 0;
    (void)sh_modserve_requirements_apply(&g_gates, "rescan", &r);
    CHECK(ev_first("C:echo first off") >= 0 && ev_first("C:echo second on") < 0 && r.commands == 1);
    CHECK(log_has("MODS: rescan: requirements of a.first@1.0.0 stopped; running its deactivate commands"));
    CHECK(!strcmp(g_text_val[1], "0") && log_has("requirement released: r_fancy restored to 0"));
    CHECK(!strcmp(g_text_val[0], "120") && r.restored == 1);

    /* Nothing installed asks anything: com_maxFps and g_label return to what
     * they displaced, e.lines's line runs, and nothing is held. */
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

/* ------------------------------------------------------------------ gated raw resources */

#define GT_MODEL "generated/basemodel/md6/mods/gt.raw/body.bmd6model"
#define GT_IMAGE "generated/image/mods/gt.raw/skin.bimage"
#define GT_ANIM  "cooked/anim/md6/mods/gt.raw/idle.bmd6anim"
#define GT_LATE  "generated/image/mods/gt.raw/late.bimage"

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
        { GT_MODEL, "MODEL", -1, "baseModel", "md6/mods/gt.raw/body.md6mesh", NULL, '1' },
        { GT_IMAGE, "IMAGE", -1, "image", "mods/gt.raw/skin", NULL, '2' },
        { GT_ANIM, "ANIM", -1, "anim", "md6/mods/gt.raw/idle.md6anim", NULL, '3' },
        { GT_LATE, "LATE", -1, "image", "mods/gt.raw/late", NULL, '4' },
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
    make_bundle(root, "gt.raw@1.0.0", "gt.raw", "1.0.0", '6', NULL, gt, 4, NULL);
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
    CHECK(strstr(seen, "baseModel:md6/mods/gt.raw/body.md6mesh:gt.raw;") && strstr(seen, "image:mods/gt.raw/skin:gt.raw;"));
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

/* ------------------------------------------------------------------ refresh */

#define RF_SH      "generated/decls/entitydef/monster/zombie.decl"
#define RF_SH_GONE "generated/decls/entitydef/monster/gonezombie.decl"
#define RF_KEEP    "generated/decls/entitydef/mods/rf.mod/keep.decl"
#define RF_NAME    "generated/decls/entitydef/mods/rf.mod/name.decl"
#define RF_OLD     "generated/decls/entitydef/mods/rf.mod/oldname.decl"
#define RF_MODEL   "generated/basemodel/md6/mods/rf.mod/body.bmd6model"
#define RF_IMAGE   "generated/image/mods/rf.mod/skin.bimage"
#define RF_ANIM    "cooked/anim/md6/mods/rf.mod/idle.bmd6anim"
#define RF_NEW     "generated/decls/entitydef/mods/rf.mod/new.decl"
#define RF_ROW     "generated/decls/snapeditorentitydef/mods/rf.mod/new.decl"
#define RF_CHILD   "generated/decls/entitydef/mods/rf.mod/child.decl"
#define RF_GRAND   "generated/decls/entitydef/mods/rf.mod/grandchild.decl"
#define RF_ORPHAN  "generated/decls/entitydef/mods/rf.mod/orphan.decl"
#define RF_DECOY   "generated/decls/entitydef/mods/rf.mod/decoy.decl"
#define RF_OLDROW  "generated/decls/snapeditorentitydef/mods/rf.mod/oldrow.decl"
#define RF_INHERIT_CHILD  "{\n\tinherit = \"mods/rf.mod/name\";\n}\n"
#define RF_INHERIT_GRAND  "{\n\t// a parent\n\tinherit = \"mods/rf.mod/child\";\n}\n"
#define RF_INHERIT_ORPHAN "{\n\tinherit=\"monster/gonezombie\";\n}\n"
#define RF_INHERIT_DECOY  "{\n\t// inherit = \"mods/rf.mod/name\"\n\tedit = { inherit = \"mods/rf.mod/name\"; }\n}\n"

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
        { RF_KEEP, "entityDef keep {}", -1, "entityDef", "mods/rf.mod/keep", NULL, '4' },
        { RF_NAME, "entityDef n1 {}", -1, "entityDef", "mods/rf.mod/name", NULL, '5' },
        { RF_OLD, "entityDef old {}", -1, "entityDef", "mods/rf.mod/oldname", NULL, '7' },
        { RF_MODEL, "MODEL1", -1, "baseModel", "md6/mods/rf.mod/body.md6mesh", NULL, '8' },
        { RF_IMAGE, "IMAGE", -1, "image", "mods/rf.mod/skin", NULL, 'a' },
        { RF_ANIM, "ANIM1", -1, "anim", "md6/mods/rf.mod/idle.md6anim", NULL, 'b' },
        { RF_CHILD, RF_INHERIT_CHILD, -1, "entityDef", "mods/rf.mod/child", NULL, 'f' },
        { RF_GRAND, RF_INHERIT_GRAND, -1, "entityDef", "mods/rf.mod/grandchild", NULL, '0' },
        { RF_ORPHAN, RF_INHERIT_ORPHAN, -1, "entityDef", "mods/rf.mod/orphan", NULL, '1' },
        { RF_DECOY, RF_INHERIT_DECOY, -1, "entityDef", "mods/rf.mod/decoy", NULL, '2' },
        { RF_OLDROW, "{ entityDef \"mods/rf.mod/keep\" }", -1, "snapEditorEntityDef", "mods/rf.mod/oldrow",
          "[{\"type\":\"entityDef\",\"name\":\"mods/rf.mod/keep\"}]", '3' },
    };
    static const fx_file v2[] = {
        { RF_SH, "entityDef z2 {}", -1, "entityDef", "monster/zombie", NULL, '2' },
        { RF_KEEP, "entityDef keep {}", -1, "entityDef", "mods/rf.mod/keep", NULL, '4' },
        { RF_NAME, "entityDef n2 {}", -1, "entityDef", "mods/rf.mod/name", NULL, '6' },
        { RF_MODEL, "MODEL2", -1, "baseModel", "md6/mods/rf.mod/body.md6mesh", NULL, '9' },
        { RF_IMAGE, "IMAGE", -1, "image", "mods/rf.mod/skin", NULL, 'a' },
        { RF_ANIM, "ANIM2", -1, "anim", "md6/mods/rf.mod/idle.md6anim", NULL, 'c' },
        { RF_NEW, "entityDef new {}", -1, "entityDef", "mods/rf.mod/new", NULL, 'd' },
        { RF_ROW, "{ entityDef \"mods/rf.mod/new\" }", -1, "snapEditorEntityDef", "mods/rf.mod/new",
          "[{\"type\":\"entityDef\",\"name\":\"mods/rf.mod/new\"}]", 'e' },
        { RF_CHILD, RF_INHERIT_CHILD, -1, "entityDef", "mods/rf.mod/child", NULL, 'f' },
        { RF_GRAND, RF_INHERIT_GRAND, -1, "entityDef", "mods/rf.mod/grandchild", NULL, '0' },
        { RF_ORPHAN, RF_INHERIT_ORPHAN, -1, "entityDef", "mods/rf.mod/orphan", NULL, '1' },
        { RF_DECOY, RF_INHERIT_DECOY, -1, "entityDef", "mods/rf.mod/decoy", NULL, '2' },
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
    make_bundle(root, "rf.mod", "rf.mod", "1.0.0", '1', "\"" RF_SH "\", \"" RF_SH_GONE "\"", v1, 13, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    /* The first table of this root replaces another root's: every file is
     * new. Shadows the earlier tables served and no table serves since are
     * still pending as gone (the engine holds their bundles' text), and the
     * new shadows are pending too (the engine may hold the game's text for
     * them); new names of the mod are not. Settle them. */
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 0 && d.added == 13 && d.changed_decls == 2 && d.pending > 0);
    memset(&s, 0, sizeof s);
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == d.pending);
    CHECK(strstr(s.order, "1:entityDef:monster/imp:gone;") && strstr(s.order, "1:entityDef:monster/zombie:now;") &&
          strstr(s.order, "1:entityDef:monster/gonezombie:now;") && !strstr(s.order, "rf.mod"));
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
    make_bundle(root, "rf.mod", "rf.mod", "1.0.0", '2', "\"" RF_SH "\"", v2, 12, NULL);
    log_clear();
    CHECK(sh_modserve_index_rescan() == 1);
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 4 && d.changed_decls == 2 && d.changed_models == 1 && d.changed_images == 0);
    CHECK(d.changed_restart == 1 && strstr(d.restart_first, RF_ANIM) && d.changed_map == 0);
    CHECK(d.added == 2 && d.added_decls == 2 && d.added_rows == 1 && d.added_active == 1 && d.changed_rows == 0);
    CHECK(d.gone == 3 && d.gone_shadows == 1 && d.gone_names == 1 && d.gone_rows == 1);
    CHECK(d.pending == 5 && d.dependents == 0);
    /* The new gameplay decl of the mod in use is planned once; the new row is the palette's. */
    {
        sh_modserve_plan p;
        CHECK(sh_modserve_plan_refresh(&p) == 1 && p.count == 1 && !strcmp(p.steps[0].name, "mods/rf.mod/new"));
        sh_modserve_plan_free(&p);
        CHECK(sh_modserve_plan_refresh(&p) == 0);
        sh_modserve_plan_free(&p);
    }
    /* The decls that inherit from a changed or gone decl follow it, at any
     * depth; a commented or nested inherit is no parent. Once per table. */
    CHECK(sh_modserve_refresh_expand() == 3);
    CHECK(sh_modserve_refresh_expand() == 0);
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 8 && d.dependents == 3);
    /* Exactly the changed decls, the gone shadow, the inheriting decls by
     * generation and the changed model, in that order. */
    memset(&s, 0, sizeof s);
    s.keep_raw = 1;
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == 8);
    CHECK(strstr(s.order, "1:snapEditorEntityDef:mods/rf.mod/oldrow:gone;"));
    CHECK(strstr(s.order, "mods/rf.mod/child:now;") && strstr(s.order, "mods/rf.mod/orphan:now;") &&
          strstr(s.order, "mods/rf.mod/grandchild:now;") && !strstr(s.order, "decoy"));
    CHECK(strstr(s.order, "gonezombie:gone;") < strstr(s.order, "rf.mod/child:") &&
          strstr(s.order, "rf.mod/child:") < strstr(s.order, "rf.mod/grandchild:") &&
          strstr(s.order, "rf.mod/grandchild:") < strstr(s.order, "baseModel"));
    CHECK(strstr(s.order, "1:entityDef:monster/zombie:now;") && strstr(s.order, "1:entityDef:mods/rf.mod/name:now;"));
    CHECK(strstr(s.order, "1:entityDef:monster/gonezombie:gone;"));
    CHECK(strstr(s.order, "2:baseModel:md6/mods/rf.mod/body.md6mesh:now;"));
    CHECK(strstr(s.order, "mods/rf.mod/name:now;") < strstr(s.order, "gonezombie") &&
          strstr(s.order, "gonezombie") < strstr(s.order, "baseModel"));
    CHECK(!strstr(s.order, "keep") && !strstr(s.order, "skin") && !strstr(s.order, "idle") &&
          !strstr(s.order, "rf.mod/new") && !strstr(s.order, "oldname") && !strstr(s.order, ":gated"));
    /* The decls are settled and no early decl; the model is still pending. */
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 1);
    memset(&u, 0, sizeof u);
    sh_modserve_each_unopened_shadow(unopened_visit, &u);
    CHECK(!strstr(u.order, "monster/zombie"));
    /* A rescan of the same bytes changes nothing and keeps the pending model. */
    CHECK(sh_modserve_index_rescan() == 1);
    sh_modserve_last_diff(&d);
    CHECK(d.changed == 0 && d.added == 0 && d.gone == 0 && d.pending == 1);
    memset(&s, 0, sizeof s);
    CHECK(sh_modserve_each_refresh(rf_visit, &s) == 1 && strstr(s.order, "baseModel"));
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
    g_bundle_extra = "\"legacy\": {},\n  ";
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
        g_bundle_extra = "\"legacy\": {},\n  \"requirements\": {\"cvars\": {\"g_useResourceBlackList\": \"0\"}},\n  ";
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
    CHECK(log_has("conflict: shadow '" CYB_AW "' is provided by cyberdemon@legacy-3d0fabc7bbc8194f, local.cyberdemon@0.0.0, "
                  "local.cyberdemon@0.0.0-05238b2e; serving local.cyberdemon@0.0.0-05238b2e (most providers carry these bytes)"));
    CHECK(sh_modserve_lookup(CYB_AW, &hit) && !strcmp(hit.bundle_id, "local.cyberdemon"));
    CHECK(open_text(CYB_AW, text, sizeof text) == 1 && strstr(text, "AW_CF_IDLE_CYCLE"));
    CHECK(sh_modserve_probe_claims(CYB_AW));
    /* The conductor: every boss's aiTypeList entry is served. */
    CHECK(log_has("conflict: shadow '" CONDUCTOR "' is provided by cyberdemon@legacy-3d0fabc7bbc8194f, local.cyberdemon@0.0.0, "
                  "local.cyberdemon@0.0.0-05238b2e, local.hell-guard@0.0.0, local.spider-mastermind@0.0.0; serving "));
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
    g_bundle_extra = "\"legacy\": {},\n  ";
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
    return !strcmp(name, "designsystems/throws") ? SH_MODSERVE_LOAD_REFUSED : 1;
}
static void rf_refusal(void *c, char *out, size_t cap)
{
    (void)c;
    strncpy_s(out, cap, "S:\\DOOM\\base\\generated\\decls\\animWeb\\designsystems\\throws.decl(9): error : "
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
        { "generated/decls/animweb/designsystems/med_station.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/objects/props_set/kiosks/wallhealthstation.md6\" \"\"\t// modelIndex 0\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "designsystems/med_station", NULL, '1' },
        { "generated/decls/animweb/designsystems/turret.decl",
          "{\n\tprops {\n\t\tmodels {\n\t\t\t\"zion/snap/known.md6\"\n\t\t\t\"zion/objects/weapons/turret_campaign.md6\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "designsystems/turret", NULL, '2' },
        { "generated/decls/animweb/designsystems/fine.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/snap/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "designsystems/fine", NULL, '3' },
        { "generated/decls/animweb/designsystems/throws.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/snap/known.md6\" \"\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "designsystems/throws", NULL, '4' },
        { "generated/decls/animweb/player/fp_hands_mp.decl",
          "{\n\tprops {\n\t\tmodelInfos {\n\t\t\t\"zion/player/human/base/mp.md6\" \"\"\n\t\t}\n\t}\n}\n",
          -1, "animWeb", "player/fp_hands_mp", NULL, '5' },
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
    /* A converted first-generation package (the issue-132 "SP doors"): every
     * name it carries is listed as a shadow, new names included. */
    g_bundle_extra = "\"legacy\": {},\n  ";
    make_bundle(root, "dz.pack@0.0.0+1", "dz.pack", "0.0.0", '9',
                "\"generated/decls/animweb/designsystems/med_station.decl\", \"generated/decls/animweb/designsystems/turret.decl\", "
                "\"generated/decls/animweb/designsystems/fine.decl\", \"generated/decls/animweb/designsystems/throws.decl\", "
                "\"generated/decls/animweb/player/fp_hands_mp.decl\"",
                files, 5, NULL);
    g_bundle_extra = NULL;
    hash64('9', hash);
    hashes[0] = hash;
    CHECK(sh_modserve_index_build(root) == 1);

    /* The check alone: the engine lookup decides a load now. */
    CHECK(sh_modserve_animweb_check("generated/decls/animweb/designsystems/med_station.decl", &g_rf, model, sizeof model) == 1 &&
          !strcmp(model, "zion/objects/props_set/kiosks/wallhealthstation.md6"));
    CHECK(sh_modserve_animweb_check("generated/decls/animweb/designsystems/turret.decl", &g_rf, model, sizeof model) == 1 &&
          !strcmp(model, "zion/objects/weapons/turret_campaign.md6"));
    CHECK(sh_modserve_animweb_check("generated/decls/animweb/designsystems/fine.decl", &g_rf, model, sizeof model) == 0);
    CHECK(sh_modserve_animweb_check("generated/decls/md6def/zion/none.md6.decl", &g_rf, model, sizeof model) == -1);
    /* Without the lookup the archives answer, which is what let issue 132's
     * campaign models through. */
    CHECK(sh_modserve_animweb_check("generated/decls/animweb/designsystems/med_station.decl", &g_rf_old, model, sizeof model) == 0);

    g_console_seen[0] = 0;
    sh_modserve_set_console(rf_console);
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 5 && unknown == 0);
    log_clear();
    g_calls[0] = 0;
    g_rf_loads = 0;
    ready = sh_modserve_plan_run(&p, &g_rf, "map");
    CHECK(ready == 2);
    CHECK(log_has("map: animWeb 'designsystems/med_station' (dz.pack) NOT READY: it names model "
                  "'zion/objects/props_set/kiosks/wallhealthstation.md6', which neither a bundle nor the game provides"));
    CHECK(log_has("map: animWeb 'designsystems/turret' (dz.pack) NOT READY: it names model 'zion/objects/weapons/turret_campaign.md6'"));
    CHECK(log_has("map: animWeb 'designsystems/fine' (dz.pack) READY"));
    CHECK(log_has("map: animWeb 'designsystems/throws' (dz.pack) NOT READY: the engine refused its parse: "
                  "S:\\DOOM\\base\\generated\\decls\\animWeb\\designsystems\\throws.decl(9): error : Could not find animweb decl: 'gone'; "
                  "withdrawn, not loaded again"));
    /* The known stock shadow parses later, on demand: the archives keep it served. */
    CHECK(log_has("map: animWeb 'player/fp_hands_mp' (dz.pack) shadows a stock decl; served by the provider"));
    /* Never loaded: the two whose models SnapMap cannot load. */
    CHECK(!strstr(g_calls, "designsystems/med_station") && !strstr(g_calls, "designsystems/turret"));
    CHECK(g_rf_loads == 2);
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dz.pack: animWeb 'designsystems/med_station' not loaded: it names model "
                                 "'zion/objects/props_set/kiosks/wallhealthstation.md6', which SnapMap cannot load (campaign-only or missing)\n") != NULL);
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dz.pack: animWeb 'designsystems/throws' not loaded: the game refused it: ") != NULL);
    CHECK(!strstr(g_console_seen, "designsystems/fine"));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/designsystems/med_station.decl", NULL));
    CHECK(!sh_modserve_lookup("generated/decls/animweb/designsystems/throws.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/animweb/designsystems/fine.decl", NULL));
    sh_modserve_plan_free(&p);

    /* The next map load with the same bundle parses none of them again. */
    CHECK(sh_modserve_plan_bundles(hashes, 1, &p, &unknown) == 5);
    log_clear();
    g_calls[0] = 0;
    g_rf_loads = 0;
    g_console_seen[0] = 0;
    ready = sh_modserve_plan_run(&p, &g_rf, "map");
    CHECK(ready == 2 && g_rf_loads == 0);
    CHECK(log_has("map: animWeb 'designsystems/throws' (dz.pack) NOT READY: withdrawn earlier in this session"));
    CHECK(log_has("map: animWeb 'designsystems/med_station' (dz.pack) NOT READY: withdrawn earlier in this session"));
    CHECK(g_console_seen[0] == 0);
    sh_modserve_plan_free(&p);

    /* A withdrawal by request (the early re-parse's refusal) names the file once. */
    CHECK(sh_modserve_withdraw("generated/decls/animweb/designsystems/fine.decl", "animWeb", "designsystems/fine",
                               "the game refused its bytes; the game's own decl is restored") == 1);
    CHECK(log_has("MODS: withdrawn: animWeb 'designsystems/fine' (dz.pack): the game refused its bytes"));
    CHECK(strstr(g_console_seen, "Snapmap+ mods: dz.pack: animWeb 'designsystems/fine' not loaded: the game refused its bytes") != NULL);
    CHECK(!sh_modserve_lookup("generated/decls/animweb/designsystems/fine.decl", NULL));
    CHECK(sh_modserve_withdraw("generated/decls/animweb/designsystems/fine.decl", "animWeb", "designsystems/fine", "again") == 0);
    sh_modserve_set_console(NULL);
}

/* sh_modserve_each_bundle_extra: admitted bundles with an extra object only,
 * installed before map-delivered, then id, then version. */
static char g_extra_seen[1024];
static void collect_extra(void *c, const char *id, const char *version, int legacy, const char *extra)
{
    (void)c;
    CHECK(extra && extra[0] == '{' && strstr(extra, "\"strings\""));
    _snprintf_s(g_extra_seen + strlen(g_extra_seen), sizeof g_extra_seen - strlen(g_extra_seen), _TRUNCATE,
                "%s@%s%s;", id, version, legacy ? "(legacy)" : "");
}

static void test_bundle_extra(void)
{
    static const fx_file z[] = { { "generated/decls/entitydef/mods/zed.mod/a.decl", "x", -1, "entityDef", "mods/zed.mod/a", NULL, '1' } };
    static const fx_file a1[] = { { "generated/decls/entitydef/mods/abe.mod/a1.decl", "x", -1, "entityDef", "mods/abe.mod/a1", NULL, '2' } };
    static const fx_file a2[] = { { "generated/decls/entitydef/mods/abe.mod/a2.decl", "x", -1, "entityDef", "mods/abe.mod/a2", NULL, '3' } };
    static const fx_file bad[] = { { "generated/decls/entitydef/mods/bad.size/a.decl", "x", 99, "entityDef", "mods/bad.size/a", NULL, '4' } };
    static const fx_file plain[] = { { "generated/decls/entitydef/mods/plain.mod/a.decl", "x", -1, "entityDef", "mods/plain.mod/a", NULL, '5' } };
    static const fx_file old[] = { { "generated/decls/entitydef/smp/aaa_old.decl", "x", -1, "entityDef", "smp/aaa_old", NULL, '6' } };
    char top[MAX_PATH], mods[MAX_PATH], legacy[MAX_PATH], *slash;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\extra_mods", top);
    _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", mods);
    mkdirs(mods);
    g_bundle_extra = "\"strings\": {\"en\": {\"zed_name\": \"Zed\"}},";
    make_bundle(mods, "zed.mod@1.0.0", "zed.mod", "1.0.0", '7', NULL, z, 1, NULL);
    g_bundle_extra = "\"strings\": {\"en\": {\"abe_name\": \"Abe 2\"}},";
    make_bundle(mods, "abe.mod@2.0.0", "abe.mod", "2.0.0", '8', NULL, a2, 1, NULL);
    g_bundle_extra = "\"hud\": {}, \"strings\": {\"en\": {\"abe_name\": \"Abe 1\"}},";
    make_bundle(mods, "abe.mod@1.0.0", "abe.mod", "1.0.0", '9', NULL, a1, 1, NULL);
    g_bundle_extra = "\"strings\": {\"en\": {\"bad_name\": \"Bad\"}},";
    make_bundle(mods, "bad.size@1.0.0", "bad.size", "1.0.0", '0', NULL, bad, 1, NULL);
    g_bundle_extra = "\"legacy\": {}, \"strings\": {\"en\": {\"old_name\": \"Old\"}},";
    make_bundle(legacy, "aaa.old@0123456789abcdef", "aaa.old", "0123456789abcdef", 'f', NULL, old, 1, NULL);
    g_bundle_extra = NULL;
    make_bundle(mods, "plain.mod@1.0.0", "plain.mod", "1.0.0", 'e', NULL, plain, 1, NULL);
    CHECK(sh_modserve_index_build(mods) == 5);
    g_extra_seen[0] = 0;
    CHECK(sh_modserve_each_bundle_extra(collect_extra, NULL) == 4);
    CHECK(!strcmp(g_extra_seen, "abe.mod@1.0.0;abe.mod@2.0.0;zed.mod@1.0.0;aaa.old@0123456789abcdef(legacy);"));
    if (strcmp(g_extra_seen, "abe.mod@1.0.0;abe.mod@2.0.0;zed.mod@1.0.0;aaa.old@0123456789abcdef(legacy);"))
        fprintf(stderr, "bundle extra order: %s\n", g_extra_seen);
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

/* ------------------------------------------------------------------ map scope (lane P25) */

#define SC_COOP   "generated/decls/entitydef/player/coop.decl"
#define SC_IMAGE  "generated/image/models/hands/arms.bimage"
#define SC_SET    "generated/decls/snapeditorsettings/default.decl"
#define SC_ROWDEF "generated/decls/entitydef/monster/hellknight.decl"
#define SC_ROW    "generated/decls/snapeditorentitydef/mods/r.rows/knight.decl"
#define SC_NEW    "generated/decls/entitydef/mods/n.names/thing.decl"
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
        { SC_ROW, "{ entityDef \"monster/hellknight\" }", -1, "snapEditorEntityDef", "mods/r.rows/knight", NULL, '7' },
    };
    static const fx_file names[] = { { SC_NEW, "entityDef t {}", -1, "entityDef", "mods/n.names/thing", NULL, '8' } };
    char top[MAX_PATH], root[MAX_PATH], h_hands[65], h_rows[65], h_maped[65];
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
    make_bundle(root, "a.hands", "a.hands", "1.0.0", '1', "\"" SC_COOP "\", \"" SC_IMAGE "\"", hands, 2, "{\"installedAt\": 100}");
    hash64('1', h_hands);
    g_bundle_extra = NULL;
    make_bundle(root, "e.editor", "e.editor", "1.0.0", '2', "\"" SC_SET "\"", editor, 1, "{\"installedAt\": 110}");
    g_bundle_extra = "\"scope\": \"always\",\n  ";
    make_bundle(root, "d.declared", "d.declared", "1.0.0", '3', "\"" SC_DECL "\"", declared, 1, "{\"installedAt\": 120}");
    g_bundle_extra = "\"scope\": \"map\",\n  ";
    make_bundle(root, "m.editor", "m.editor", "1.0.0", '4', "\"" SC_MAPED "\"", maped, 1, "{\"installedAt\": 130}");
    hash64('4', h_maped);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"g_label\": \"knights\"}},\n  ";
    make_bundle(root, "r.rows", "r.rows", "1.0.0", '5', "\"" SC_ROWDEF "\"", rows, 2, "{\"installedAt\": 140}");
    hash64('5', h_rows);
    g_bundle_extra = NULL;
    make_bundle(root, "n.names", "n.names", "1.0.0", '6', NULL, names, 1, "{\"installedAt\": 150}");

    printf("-- map scope: shadows and requirements apply only in maps that list their bundle\n");
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_scope_enable(0);
    log_clear();
    CHECK(sh_modserve_index_build(root) == 6);
    /* The new shadows this table brings are pending as at any rescan: settle them first. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    /* Off (as every earlier test): every shadow serves. */
    CHECK(sh_modserve_lookup(SC_COOP, NULL) && sh_modserve_lookup(SC_IMAGE, NULL) && sh_modserve_lookup(SC_MAPED, NULL));
    CHECK(log_has("MODS: scope: e.editor@1.0.0 shadows editor-time content (" SC_SET "); treated as \"scope\": \"always\""));
    CHECK(log_count("MODS: scope:") == 1);   /* m.editor says "map" itself: no line */
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: map scope is off; every shadow serves in every map\n") != NULL);
    CHECK(!sh_modserve_scope_relevant());

    /* On, no map open: map-scoped shadows are withdrawn, new names and "always" serve. */
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_scope_enabled() && !sh_modserve_scope_map_open() && sh_modserve_scope_relevant());
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_lookup(SC_IMAGE, NULL) && !sh_modserve_lookup(SC_ROWDEF, NULL));
    CHECK(sh_modserve_lookup_indexed(SC_COOP, NULL) == 1);
    {
        unsigned char *bytes = NULL;
        size_t len = 0;
        CHECK(sh_modserve_open(SC_COOP, &bytes, &len) == 0 && !bytes);
        CHECK(sh_modserve_probe_claims(SC_COOP) == 0);
    }
    CHECK(sh_modserve_lookup(SC_SET, NULL) && sh_modserve_lookup(SC_DECL, NULL) && sh_modserve_lookup(SC_NEW, NULL) &&
          sh_modserve_lookup(SC_ROW, NULL));
    CHECK(!sh_modserve_lookup(SC_MAPED, NULL));   /* "scope": "map" overrides the editor rule */
    /* The engine is told to read the game's bytes again: gone, no gate. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "a.hands:player/coop:gone=1:gates=1;") && strstr(seen.order, "a.hands:models/hands/arms:gone=1:gates=1;"));
    CHECK(strstr(seen.order, "r.rows:monster/hellknight:gone=1") && strstr(seen.order, "m.editor:other:gone=1"));
    CHECK(!strstr(seen.order, "e.editor") && !strstr(seen.order, "d.declared") && !strstr(seen.order, "n.names"));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* No early-decl offer for a withdrawn shadow. */
    {
        unopened_seen u;
        memset(&u, 0, sizeof u);
        sh_modserve_each_unopened_shadow(unopened_visit, &u);
        CHECK(!strstr(u.order, "player/coop"));
    }
    /* No map: no requirement is live, nothing set, no line run. */
    CHECK(sh_modserve_requirements_apply(&g_gates, "hub", &r) == 0 && r.commands == 0 && !strcmp(g_text_val[1], "0"));
    CHECK(sh_modserve_bundle_scope(h_hands, &si) && si.scope == SH_MODSERVE_SCOPE_MAP && !si.listed && !si.shadows_live &&
          si.shadows == 2 && si.requirements && !si.requirements_live);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: a.hands@1.0.0: map (2 shadows, requirements): not active (no map is open)\n") != NULL);
    CHECK(strstr(g_report, "scope: e.editor@1.0.0: always (shadows editor-time content: " SC_SET ") (1 shadow)\n") != NULL);
    CHECK(strstr(g_report, "scope: d.declared@1.0.0: always (mod.json) (1 shadow)\n") != NULL);
    CHECK(strstr(g_report, "scope: n.names@1.0.0: map: new names only, nothing to scope\n") != NULL);

    /* A map that does not list a.hands: r.rows keeps its requirements (its row is in the palette), not its shadow. */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) == 1);
    CHECK(sh_modserve_scope_map_open() && !sh_modserve_lookup(SC_ROWDEF, NULL));
    g_events[0] = 0;
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 1 && !strcmp(g_text_val[3], "knights") && !strcmp(g_text_val[1], "0"));
    CHECK(ev_first("C:echo hands on") < 0);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: r.rows@1.0.0: map (1 shadow, requirements): not active (the open map does not use it); "
                           "its requirements stay live because it adds Toybox rows\n") != NULL);
    CHECK(strstr(g_report, "scope: a.hands@1.0.0: map (2 shadows, requirements): not active (the open map does not use it)\n") != NULL);

    /* The map lists a.hands (by its row's hash): its shadows serve again, marked to be re-parsed and reloaded. */
    list[0] = h_hands;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) == 1);
    CHECK(sh_modserve_lookup(SC_COOP, NULL) && sh_modserve_lookup(SC_IMAGE, NULL));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "a.hands:player/coop:gone=0:gates=0;") != NULL);   /* waits for its gate */
    g_events[0] = 0;
    log_clear();
    CHECK(sh_modserve_requirements_apply(&g_gates, "map load", &r) == 1 && !strcmp(g_text_val[1], "1"));
    CHECK(ev_first("S:r_fancy=1") >= 0 && ev_first("C:echo hands on") > ev_first("S:r_fancy=1"));
    CHECK(log_has("MODS: requirements: a.hands: command \"echo hands on\""));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "a.hands:player/coop:gone=0:gates=1;") && strstr(seen.order, "a.hands:models/hands/arms:gone=0:gates=1;"));
    CHECK(sh_modserve_bundle_scope(h_hands, &si) && si.listed && si.shadows_live && si.requirements_live);
    /* The same listing again changes nothing. */
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) == 0);
    /* A rescan keeps the listing: the new table serves it at once, nothing pending. */
    CHECK(sh_modserve_index_rescan() == 6 && sh_modserve_lookup(SC_COOP, NULL));
    sh_modserve_last_diff(&d);
    CHECK(d.pending == 0);
    /* By id (a row whose build is not indexed names its mod), and case-insensitive. */
    list[0] = "A.Hands";
    CHECK(sh_modserve_scope_set(NULL, 0, list, 1, 1) == 0 && sh_modserve_lookup(SC_COOP, NULL));
    /* The serving build a map-level row is rewritten to. */
    {
        sh_modserve_bundle_ref ref;
        CHECK(sh_modserve_bundle_serving(NULL, "a.hands", &ref) && !strcmp(ref.hash, h_hands) && !strcmp(ref.version, "1.0.0"));
        CHECK(sh_modserve_bundle_serving(h_rows, NULL, &ref) && !strcmp(ref.id, "r.rows"));
        CHECK(!sh_modserve_bundle_serving(NULL, "nobody.here", &ref));
    }

    /* The map closes (the hub): the hands' deactivate line runs and r_fancy and g_label are restored. */
    g_events[0] = 0;
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) == 2);
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_scope_map_open());
    CHECK(sh_modserve_requirements_apply(&g_gates, "map exit", &r) == 0);
    CHECK(ev_first("C:echo hands off") >= 0 && !strcmp(g_text_val[1], "0") && !strcmp(g_text_val[3], ""));
    CHECK(sh_modserve_requirements_held() == 0);
    (void)h_maped;

    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* Lane P31: a shadow path is served when any provider the open map lists
 * provides it, also when the winner's bundle is not listed (two installs of
 * the same bytes, a renamed or forked copy), and requirement lines say each
 * bundle's own state for the open map. */
static void test_scope_providers(void)
{
    static const fx_file bare[] = { { SC_COOP, "entityDef coop { bare }", -1, "entityDef", "player/coop", NULL, '9' } };
    static const fx_file one[] = { { SC_DECL, "entityDef rev { one }", -1, "entityDef", "monster/revenant", NULL, 'a' } };
    static const fx_file two[] = { { SC_DECL, "entityDef rev { two! }", -1, "entityDef", "monster/revenant", NULL, 'b' } };
    char top[MAX_PATH], root[MAX_PATH], h_req[65];
    const char *list[2];
    char *slash;
    sh_modserve_hit hit;
    sh_modserve_gates r;
    sh_modserve_scope_info si;
    scope_seen seen;
    unsigned char *bytes = NULL;
    size_t len = 0;
    strcpy_s(top, sizeof top, g_root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\scope2\\mods", top);
    mkdirs(root);
    g_bundle_extra = "\"requirements\": {\"cvars\": {\"r_fancy\": true}},\n  ";
    make_bundle(root, "a.bare", "a.bare", "1.0.0", 'c', "\"" SC_COOP "\"", bare, 1, "{\"installedAt\": 100}");
    make_bundle(root, "b.req", "b.req", "1.0.0", 'd', "\"" SC_COOP "\"", bare, 1, "{\"installedAt\": 200}");
    hash64('d', h_req);
    g_bundle_extra = NULL;
    make_bundle(root, "x.one", "x.one", "1.0.0", 'e', "\"" SC_DECL "\"", one, 1, "{\"installedAt\": 100}");
    make_bundle(root, "y.two", "y.two", "1.0.0", 'f', "\"" SC_DECL "\"", two, 1, "{\"installedAt\": 200}");

    printf("-- map scope: a shadow path is served by any provider the open map lists\n");
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 4);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) >= 0);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!sh_modserve_lookup(SC_COOP, NULL) && !sh_modserve_lookup(SC_DECL, NULL));
    CHECK(sh_modserve_lookup_indexed(SC_COOP, &hit) && !strcmp(hit.bundle_id, "a.bare"));   /* the winner */

    /* Only b.req is listed (P27: alex.bare-hands-req attached, alex.bare-hands not): its copy serves, re-parsed now. */
    list[0] = h_req;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) == 1);
    CHECK(sh_modserve_lookup(SC_COOP, &hit) && !strcmp(hit.bundle_id, "b.req") && strstr(hit.disk_path, "b.req") != NULL);
    CHECK(sh_modserve_open(SC_COOP, &bytes, &len) == 1 && len == strlen("entityDef coop { bare }") &&
          !memcmp(bytes, "entityDef coop { bare }", len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_probe_claims(SC_COOP) == 1);
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "b.req:player/coop:gone=0") != NULL);
    CHECK(sh_modserve_bundle_scope(h_req, &si) && si.shadows == 1 && si.shadows_live);
    g_report[0] = 0;
    sh_modserve_scope_report(report_line, NULL);
    CHECK(strstr(g_report, "scope: b.req@1.0.0: map (1 shadow, requirements): active in the open map\n") != NULL);
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 1 && !strcmp(g_text_val[1], "1"));
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: b.req@1.0.0: cvar r_fancy=1: live\n") != NULL);
    /* a.bare asks the same value but the map does not use it: not live for it. */
    CHECK(strstr(g_report, "requirements: a.bare@1.0.0: cvar r_fancy=1: not active for this map (the open map does not use it; "
                           "the cvar has this value because b.req@1.0.0 asks it)\n") != NULL);
    CHECK(!strstr(g_report, "a.bare@1.0.0: cvar r_fancy=1: live"));
    /* Its gate is live now: the re-parse that waited for it runs. */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "b.req:player/coop:gone=0:gates=1;") != NULL);

    /* Moving the listing to the other copy of the same bytes changes nothing the engine holds. */
    list[0] = "a.bare";
    CHECK(sh_modserve_scope_set(NULL, 0, list, 1, 1) == 2);
    CHECK(sh_modserve_lookup(SC_COOP, &hit) && !strcmp(hit.bundle_id, "a.bare"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(!strstr(seen.order, "player/coop"));

    /* Different bytes: the listed provider's own file serves; the winner when both are listed. */
    list[0] = "x.one";
    CHECK(sh_modserve_scope_set(NULL, 0, list, 1, 1) >= 1);
    CHECK(sh_modserve_lookup(SC_DECL, &hit) && !strcmp(hit.bundle_id, "x.one"));
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "x.one:monster/revenant:gone=0") && strstr(seen.order, "a.bare:player/coop:gone=1"));
    list[0] = "x.one"; list[1] = "y.two";
    CHECK(sh_modserve_scope_set(NULL, 0, list, 2, 1) >= 1);
    CHECK(sh_modserve_lookup(SC_DECL, &hit) && !strcmp(hit.bundle_id, "y.two"));   /* installed most recently */
    memset(&seen, 0, sizeof seen);
    sh_modserve_each_refresh(scope_refresh, &seen);
    CHECK(strstr(seen.order, "y.two:monster/revenant:gone=0") != NULL);   /* other bytes: read again */
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 1) >= 1 && !sh_modserve_lookup(SC_DECL, NULL));

    /* The requirement lines after the map stops using the mods, then after it closes. */
    CHECK(sh_modserve_requirements_apply(&g_gates, "map", &r) == 0 && !strcmp(g_text_val[1], "0"));
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: b.req@1.0.0: cvar r_fancy=1: not active for this map (the open map does not use it; "
                           "restored: the cvar is back at the value it displaced)\n") != NULL);
    CHECK(strstr(g_report, "requirements: a.bare@1.0.0: cvar r_fancy=1: not active for this map (the open map does not use it)\n") != NULL);
    CHECK(!strstr(g_report, "not applied yet") && !strstr(g_report, ": live"));
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    g_report[0] = 0;
    sh_modserve_requirements_report(report_line, NULL);
    CHECK(strstr(g_report, "requirements: b.req@1.0.0: cvar r_fancy=1: not active for this map (no map is open; restored") != NULL);

    sh_modserve_scope_enable(0);
    gates_reset(1, 1, SH_MODSERVE_GATE_SET);
}

/* The asset rule (sh_modserve_row_served): a row is served by any admitted
 * folder of its id that provides every name it lists, whatever its bytes;
 * missing with no folder of the id; incomplete, naming the first missing
 * names, when the folder lacks one. */
static void test_row_served(void)
{
    char tmp[MAX_PATH], root[MAX_PATH], top[MAX_PATH], cause[400], *slash;
    sh_modmanifest_row row;
    char *names[5];
    static const fx_file HG[] = {
        { "generated/decls/entitydef/mods/local.hell-guard/hell_guard_combo_enc.decl", "{ inherit = \"x\"; }", -1,
          "entityDef", "mods/local.hell-guard/hell_guard_combo_enc", NULL, '1' },
        { "generated/decls/entitydef/mods/local.hell-guard/hell_guard_shield.decl", "{ inherit = \"y\"; }", -1,
          "entityDef", "mods/local.hell-guard/hell_guard_shield", NULL, '2' },
    };
    static const fx_file HG_NEW[] = {   /* recreated: the same paths, new bytes */
        { "generated/decls/entitydef/mods/local.hell-guard/hell_guard_combo_enc.decl", "{ inherit = \"x2\"; }", -1,
          "entityDef", "mods/local.hell-guard/hell_guard_combo_enc", NULL, '3' },
        { "generated/decls/entitydef/mods/local.hell-guard/hell_guard_shield.decl", "{ inherit = \"y2\"; }", -1,
          "entityDef", "mods/local.hell-guard/hell_guard_shield", NULL, '4' },
    };
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%smodserve_rows_%lu\\mods", tmp, GetCurrentProcessId());
    strcpy_s(top, sizeof top, root);
    slash = strrchr(top, '\\');
    if (slash) *slash = 0;
    remove_tree(top);
    mkdirs(root);
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "local.hell-guard");
    strcpy_s(row.version, sizeof row.version, "1.0.0");
    hash64('9', row.bundle);                 /* a hash no folder carries */
    names[0] = "mods/local.hell-guard/hell_guard_combo_enc";
    names[1] = "MODS/local.hell-guard/hell_guard_shield";   /* names fold case */
    row.names = names;
    row.name_count = 2;

    /* No folder of the id: missing. */
    CHECK(sh_modserve_index_build(root) >= 0);
    CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_MISSING);
    CHECK(!strcmp(cause, "This map uses local.hell-guard, which is not installed."));
    CHECK(sh_modserve_row_served_for(&row, "prefab", cause, sizeof cause) == SH_MODSERVE_ROW_MISSING &&
          !strcmp(cause, "This prefab uses local.hell-guard, which is not installed."));

    /* The folder: served, though its hash is not the row's. */
    g_bundle_name = "Hell Guards";
    make_bundle(root, "local.hell-guard", "local.hell-guard", "1.0.0", 'd', NULL, HG, 2, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_SERVED && !cause[0]);

    /* Recreated with the same id and paths but new bytes (a new hash): still served. */
    remove_tree(top);
    mkdirs(root);
    make_bundle(root, "hand-made", "local.hell-guard", "2.0.0", 'e', NULL, HG_NEW, 2, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_SERVED);

    /* A folder of the id that lacks one listed path: incomplete, naming it. */
    remove_tree(top);
    mkdirs(root);
    make_bundle(root, "local.hell-guard", "local.hell-guard", "1.0.0", 'd', NULL, HG + 1, 1, NULL);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_INCOMPLETE);
    CHECK(!strcmp(cause, "Hell Guards (local.hell-guard) is installed but does not provide "
                         "mods/local.hell-guard/hell_guard_combo_enc, which this map uses."));
    /* More than three missing: the first three and the count. */
    names[2] = "mods/local.hell-guard/a";
    names[3] = "mods/local.hell-guard/b";
    names[4] = "mods/local.hell-guard/c";
    row.name_count = 5;
    CHECK(sh_modserve_row_served_for(&row, "prefab", cause, sizeof cause) == SH_MODSERVE_ROW_INCOMPLETE);
    CHECK(!strcmp(cause, "Hell Guards (local.hell-guard) is installed but does not provide "
                         "mods/local.hell-guard/hell_guard_combo_enc, mods/local.hell-guard/a, mods/local.hell-guard/b and 1 more, "
                         "which this prefab uses."));
    /* A row that lists no names (a map-level row) is served by the id alone. */
    row.name_count = 0;
    CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_SERVED);
    /* Another id's folder providing the names does not serve the row. */
    remove_tree(top);
    mkdirs(root);
    g_bundle_name = NULL;
    row.name_count = 2;
    {
        static const fx_file OTHER[] = {
            { "generated/decls/entitydef/mods/local.other/x.decl", "{ }", -1, "entityDef", "mods/local.other/x", NULL, '5' },
        };
        make_bundle(root, "local.other", "local.other", "1.0.0", 'c', NULL, OTHER, 1, NULL);
    }
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(sh_modserve_row_served(&row, NULL, 0) == SH_MODSERVE_ROW_MISSING);
    remove_tree(top);
    CHECK(sh_modserve_index_build(g_root) >= 0);
}

int main(void)
{
    char top[MAX_PATH];
    char *slash;
    build_fixture();
    test_empty_index();
    test_index_and_isolation();
    test_unopened_shadows();
    test_unopened_after_rescan();
    test_lookup();
    test_precedence();
    test_probe();
    test_plan();
    test_plan_alias();
    test_plan_run();
    test_bundles_for_names();
    test_cost();
    test_rescan();
    test_requirements();
    test_ungated_raw();
    test_requirements_general();
    test_refresh();
    test_conflicts();
    test_editor_refs();
    test_watermark();
    test_refusals();
    test_bundle_extra();
    test_scope();
    test_scope_providers();
    test_row_served();
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
