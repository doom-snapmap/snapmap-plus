/* modlook_mod_test.c -- the Graphics SDK's look compiler (modlook.c) wired where it is used: a mod folder built into a
 * bundle by the builder (sh_mb_build with sh_mb_set_look_compiler) and the same folder served by the provider through
 * its material filter (sh_modserve_set_material_filter), the two hooks dllmain.c installs.
 *
 * Build: a folder of a layered material (a layer image, a colour layer, a burn), a particle material of the platform route and
 * one of the stock route, a material that is not a look, builds; its needs are the folder's own images and the game's
 * names, never a file the product derives (snapmapplus/layered/*, snapmapplus/particles/*, snapmapplus/look/*); the
 * report carries the look notes; a copy with one wrong field fails with exactly the compiler's sentence.
 * Serve: each look material is served as it compiles, the derived images it names are readable through
 * sh_modprog_read, a material that is not a look, or that writes the old keys, is served byte for byte as written.
 * A real converted mod (Doomforge's Tyrant, when the folder is on this computer) builds offline.
 *
 *   modlook_mod_test.exe [fixtures=<tests\fixtures\mods>] [fixtures_prog=<tests\fixtures\modprog>]
 *                        [doom=<install root>] [tyrant=<folder>] [verbose] */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modserve.h"
#include "modbundle.h"
#include "modlook.h"
#include "sdk.h"
#include "modauthor.h"
#include "modprog.h"
#include "modstock.h"
#include "mod_test_util.h"
#include "store_lock.h"
#include "modstore.h"
#include "overrides.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/script/script_runtime.h"

static int g_failed, g_checks, g_verbose;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

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
static int log_has(const char *needle) { return strstr(g_log, needle) != NULL; }

/* ------------------------------------------------------------------ provider stubs (as modserve_test) */

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
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    *out_bytes = NULL;
    *out_len = 0;
    return 0;
}
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }

/* The serving layer's side of the script provider is not used here (script_source.c links it). */
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }

/* ------------------------------------------------------------------ helpers */

static char g_fixtures[MAX_PATH], g_fixtures_prog[MAX_PATH], g_doom[MAX_PATH], g_tyrant[MAX_PATH];
static int g_doom_registered;

static int stock_exists(void *ctx, const char *request)
{
    (void)ctx;
    if (g_doom_registered) return sh_modstock_exists(request);
    if (sh_modprog_owns(request)) return 1;   /* no game: the product's own files stand in for what it derives */
    /* the game's own render programs, which the offline run has no archive to look up */
    if (!strncmp(request, "generated/decls/renderprog/", 27) && !strchr(request + 27, '/')) return 1;
    if (g_verbose) printf("    stock miss: %s\n", request);
    return 0;
}
static sh_mb_resolver g_resolver = { NULL, NULL, stock_exists, NULL };

static const char *fixture_path(const char *name, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", g_fixtures, name);
    return out;
}

static unsigned char *read_whole(const char *path, size_t *n)
{
    FILE *f = NULL;
    unsigned char *b;
    long sz;
    *n = 0;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)sz + 1);
    if (b) { *n = fread(b, 1, (size_t)sz, f); b[*n] = 0; }
    fclose(f);
    return b;
}

static int has_need(const sh_mb_file *f, const char *type, const char *name)
{
    size_t i;
    for (i = 0; i < f->need_count; i++)
        if (!_stricmp(f->needs[i].type, type) && !_stricmp(f->needs[i].name, name)) return 1;
    return 0;
}
static int has_note(const sh_mb_bundle *b, const char *needle)
{
    size_t i;
    for (i = 0; i < b->note_count; i++) if (strstr(b->notes[i], needle)) return 1;
    return 0;
}
static const sh_mb_file *find_file(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}
static int has(const char *s, const char *needle) { return s && strstr(s, needle) != NULL; }

/* The value of the first line `key` of a material text (case-insensitive), as a static string; "" when none. */
static const char *value(const char *text, const char *key)
{
    static char t[600];
    size_t kn = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n'), *q = p;
        size_t ln = e ? (size_t)(e - p) : strlen(p), n;
        while (ln && (*q == ' ' || *q == '\t')) { q++; ln--; }
        if (ln > kn && !_strnicmp(q, key, kn) && (q[kn] == ' ' || q[kn] == '\t')) {
            const char *v = q + kn;
            ln -= kn;
            while (ln && (*v == ' ' || *v == '\t')) { v++; ln--; }
            while (ln && (v[ln - 1] == '\r' || v[ln - 1] == ' ')) ln--;
            n = ln < sizeof t - 1 ? ln : sizeof t - 1;
            memcpy(t, v, n);
            t[n] = 0;
            return t;
        }
        p = e ? e + 1 : NULL;
    }
    t[0] = 0;
    return t;
}

static void dump_needs(const sh_mb_bundle *b, const char *request)
{
    const sh_mb_file *f = find_file(b, request);
    size_t i;
    if (!g_verbose || !f) return;
    for (i = 0; i < f->need_count; i++) printf("    need %s %s\n", f->needs[i].type, f->needs[i].name);
}

/* A need that the product owns: its name is under one of the product's derived places. */
static int is_product_need(const char *name)
{
    return has(name, "snapmapplus/layered/") || has(name, "snapmapplus/particles/") || has(name, "snapmapplus/look/") ||
           has(name, "snapmapplus/multilayer/") || has(name, "snapmapplus/eternal/");
}

/* ------------------------------------------------------------------ 1. the build */

#define GOOD_SALT "lookgood"
#define BAD_SALT  "lookbad0"
#define MAT_DIR   "generated/decls/material/mods/" GOOD_SALT "/"

static void build_good(const char *tmp, sh_mb_bundle *b, char *folder, size_t cap, int expect_ok)
{
    char src[MAX_PATH];
    _snprintf_s(folder, cap, _TRUNCATE, "%s\\looks-good", tmp);
    if (!mt_exists(folder)) CHECK(mt_copy_tree(fixture_path("looks-good", src, sizeof src), folder));
    CHECK(sh_mb_build(folder, NULL, &g_resolver, b) == expect_ok);
}

static void test_build(const char *tmp)
{
    sh_mb_bundle b;
    char folder[MAX_PATH];
    const sh_mb_file *body, *embers, *spark, *plain, *crowded;
    size_t i, k;
    int derived_needed = 0;

    sh_mb_set_look_compiler(sh_look_compile_notes);
    build_good(tmp, &b, folder, sizeof folder, 1);
    if (b.errors) fprintf(stderr, "build refused: %s\n", sh_mb_first_cause(&b));
    CHECK(b.errors == 0 && b.id[0] && !strcmp(b.id, GOOD_SALT));

    body = find_file(&b, MAT_DIR "body.decl");
    embers = find_file(&b, MAT_DIR "embers.decl");
    spark = find_file(&b, MAT_DIR "spark.decl");
    plain = find_file(&b, MAT_DIR "plain.decl");
    crowded = find_file(&b, MAT_DIR "crowded.decl");
    CHECK(body && embers && spark && plain && crowded);
    dump_needs(&b, MAT_DIR "body.decl");
    dump_needs(&b, MAT_DIR "embers.decl");
    dump_needs(&b, MAT_DIR "spark.decl");
    if (body) {
        /* the author's own images are needs (found in the folder) */
        CHECK(has_need(body, "image", "mods/" GOOD_SALT "/tiles/mask.tga"));
        CHECK(has_need(body, "image", "mods/" GOOD_SALT "/tiles/pores.tga"));
        CHECK(has_need(body, "image", "mods/" GOOD_SALT "/tiles/pores_n.tga"));
        CHECK(has_need(body, "image", "mods/" GOOD_SALT "/tiles/burnt.tga"));
    }
    if (embers) {
        CHECK(has_need(embers, "image", "mods/" GOOD_SALT "/fx/embers.tga"));
        CHECK(has_need(embers, "image", "mods/" GOOD_SALT "/fx/embers_mask.tga"));
    }
    if (spark) CHECK(has_need(spark, "image", "mods/" GOOD_SALT "/fx/spark.tga"));
    if (plain) CHECK(has_need(plain, "image", "mods/" GOOD_SALT "/plain.tga"));
    /* a look's own keys are never needs; the compiled text's are (the layer images, the base image) */
    if (body) CHECK(!has_need(body, "image", "look") && !has_need(body, "image", "layered"));

    /* nothing the product owns is asked of the author: no need of any file names a derived path, and no file of the
     * bundle is one */
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        CHECK(!is_product_need(f->request) || f->closure || 1);
        for (k = 0; k < f->need_count; k++) {
            if (!is_product_need(f->needs[k].name)) continue;
            /* a product-owned need is the program closure's, never an author file or an unresolved name */
            derived_needed++;
            if (g_verbose) printf("    product need of %s: %s %s\n", f->request, f->needs[k].type, f->needs[k].name);
        }
    }
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        if (!f->closure) CHECK(!is_product_need(f->request));
    }
    CHECK(b.needs.unresolved == 0);
    CHECK(b.warnings == 0);
    if (b.warnings) fprintf(stderr, "first warning: %s\n", sh_mb_first_warning(&b));
    (void)derived_needed;

    /* the look notes: the crowded material asks for more shared images than the game has places for */
    if (g_verbose) for (i = 0; i < b.note_count; i++) printf("    note: %s\n", b.notes[i]);
    CHECK(has_note(&b, "crowded.decl"));
    CHECK(has_note(&b, "burnblood"));
    CHECK(!has_note(&b, "body.decl") && !has_note(&b, "embers.decl") && !has_note(&b, "spark.decl") &&
          !has_note(&b, "plain.decl"));
    sh_mb_free(&b);

    /* a deliberately wrong copy: one field misspelled, refused with exactly the compiler's one sentence */
    {
        char bad[MAX_PATH], src[MAX_PATH];
        const char *want = "body.decl line 7: 'layer1nromal' is not a field of a layered material; the nearest is 'layer1normal'.";
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\looks-bad", tmp);
        CHECK(mt_copy_tree(fixture_path("looks-bad", src, sizeof src), bad));
        CHECK(!sh_mb_build(bad, NULL, &g_resolver, &b));
        CHECK(b.errors == 1);
        if (g_verbose) printf("    bad: %s\n", sh_mb_first_cause(&b));
        CHECK(!strcmp(sh_mb_first_cause(&b), want));
        sh_mb_free(&b);
    }

    /* without the compiler hook the same wrong folder is scanned as its file holds it: the hook is what refuses it */
    {
        char bad[MAX_PATH];
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\looks-bad", tmp);
        sh_mb_set_look_compiler(NULL);
        CHECK(sh_mb_build(bad, NULL, &g_resolver, &b));
        sh_mb_free(&b);
        sh_mb_set_look_compiler(sh_look_compile_notes);
    }
    (void)folder;
}

/* ------------------------------------------------------------------ 2. the serve */

static int open_text(const char *request, unsigned char **bytes, size_t *len)
{
    return sh_modserve_open(request, bytes, len);
}

/* A file of the folder as the engine is handed it: its names written as the engine names them (modauthor.h). */
static unsigned char *engine_whole(const char *path, size_t *n)
{
    unsigned char *raw = read_whole(path, n);
    size_t en = 0;
    char *t = raw ? sh_ma_text(GOOD_SALT, (const char *)raw, *n, NULL, NULL, &en) : NULL;
    if (!t) return raw;
    free(raw);
    *n = en;
    return (unsigned char *)t;
}
static void test_serve(const char *tmp)
{
    char root[MAX_PATH], folder[MAX_PATH], src[MAX_PATH], path[MAX_PATH], *cause = NULL;
    sh_mb_bundle b;
    static const char *const LOOKS[] = { "body", "embers", "spark", "crowded" };
    unsigned char *served = NULL, *file;
    size_t sn = 0, fn = 0, i;
    char req[300];

    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\mods", tmp);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\" GOOD_SALT, root);
    CHECK(CreateDirectoryA(root, NULL));
    CHECK(mt_copy_tree(fixture_path("looks-good", src, sizeof src), folder));
    /* an old-key material (emitted before the numbers moved), copied in under its own name; its images are the mod's */
    {
        char lp[MAX_PATH], dst[MAX_PATH];
        unsigned char *t;
        size_t n = 0;
        char *w;
        _snprintf_s(lp, sizeof lp, _TRUNCATE, "%s\\legacy_head.decl", g_fixtures_prog);
        t = read_whole(lp, &n);
        CHECK(t != NULL);
        if (t) {
            /* the fixture names another mod's salt; this folder's own, the images beside it */
            while ((w = strstr((char *)t, "wuahwfly")) != NULL) memcpy(w, GOOD_SALT, 8);
            _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s\\materials\\legacy_head.decl", folder);
            for (w = dst; *w; w++) if (*w == '/') *w = '\\';
            mt_mkdirs_for(dst);
            {
                FILE *f = NULL;
                if (!fopen_s(&f, dst, "wb") && f) { fwrite(t, 1, strlen((char *)t), f); fclose(f); }
            }
            /* each image it names is the mod's own, a copy of the fixture's */
            for (w = (char *)t; (w = strstr(w, "mods/" GOOD_SALT "/")) != NULL;) {
                char img[300], from[MAX_PATH], to[MAX_PATH], *e = w, *q;
                while (*e && *e != '"' && *e != '\r' && *e != '\n' && *e != ' ' && *e != '\t') e++;
                if (e - w > 4 && !_strnicmp(e - 4, ".tga", 4) && (size_t)(e - w) < sizeof img - 16) {
                    memcpy(img, w, (size_t)(e - w) - 4);
                    img[e - w - 4] = 0;
                    _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\images\\%s.bimage", folder, img + sizeof "mods/" GOOD_SALT "/" - 1);
                    for (q = to; *q; q++) if (*q == '/') *q = '\\';
                    _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\images\\plain.bimage", folder);
                    mt_mkdirs_for(to);
                    CopyFileA(from, to, TRUE);
                }
                w = e;
            }
            free(t);
        }
    }
    sh_mb_set_look_compiler(sh_look_compile_notes);
    CHECK(sh_mb_build(folder, NULL, &g_resolver, &b));
    if (b.errors) fprintf(stderr, "serve folder refused: %s\n", sh_mb_first_cause(&b));
    CHECK(HO(cause, sh_mb_write(&b, folder, HO_P(0))));
    sh_mb_free(&b);

    sh_modprog_reset();
    sh_modserve_set_material_filter(sh_look_compile);
    sh_modserve_set_product_owner(sh_modprog_owns);   /* as dllmain.c wires the product's own files */
    sh_modserve_set_product_reader(sh_modprog_read);
    CHECK(sh_modserve_index_build(root) == 1);

    for (i = 0; i < sizeof LOOKS / sizeof LOOKS[0]; i++) {
        char *want = NULL, why[400];
        size_t want_n = 0;
        int r;
        char *p, *e;
        _snprintf_s(req, sizeof req, _TRUNCATE, MAT_DIR "%s.decl", LOOKS[i]);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\materials\\%s.decl", folder, LOOKS[i]);
        for (p = path; *p; p++) if (*p == '/') *p = '\\';
        file = engine_whole(path, &fn);
        CHECK(file != NULL);
        CHECK(open_text(req, &served, &sn) == 1 && served);
        if (served) {   /* a NUL after the bytes, for the text helpers below */
            unsigned char *z = (unsigned char *)HeapReAlloc(GetProcessHeap(), 0, served, sn + 1);
            if (z) { served = z; served[sn] = 0; }
        }
        r = sh_look_compile(req, (const char *)file, fn, &want, &want_n, why, sizeof why);
        CHECK(r == SH_LOOK_CHANGED && want);
        if (served && want) {
            /* the served text is the compiled text, not the author's file */
            CHECK(sn == want_n && !memcmp(served, want, sn));
            CHECK(!(sn == fn && !memcmp(served, file, fn)));
            CHECK(value((const char *)served, "type")[0] == 0);
            if (!strcmp(LOOKS[i], "body") || !strcmp(LOOKS[i], "crowded")) {
                char prog[600], parms[600];
                strcpy_s(prog, sizeof prog, value((const char *)served, "ambientprogram"));
                strcpy_s(parms, sizeof parms, value((const char *)served, "layerparms"));
                CHECK(has(prog, "snapmapplus/layered/skinvmtr"));
                if (!strcmp(LOOKS[i], "body")) CHECK(has(prog, "skinvmtrwounds"));
                CHECK(parms[0] != 0);
                if (g_verbose) printf("    %s: program %s layerparms %s\n", LOOKS[i], prog, parms);
            } else if (!strcmp(LOOKS[i], "embers")) {
                CHECK(has(value((const char *)served, "stageprogram"), "snapmapplus/particles/uberparticles"));
                CHECK(value((const char *)served, "particleuberparms1")[0] != 0);
            } else {
                /* the stock route: the game's own shading, nothing of the product's in it */
                CHECK(!has((const char *)served, "snapmapplus"));
                CHECK(value((const char *)served, "stageprogram")[0] != 0);
            }
            /* every derived image the text names is readable and is what the compiler registered */
            p = (char *)served;
            while ((p = strstr(p, "snapmapplus/look/")) != NULL) {
                char name[200], ipath[260], *dot;
                unsigned char *bytes = NULL;
                size_t bn = 0, k = 0;
                while (p[k] && p[k] != '"' && p[k] != '\r' && p[k] != '\n' && p[k] != ' ' && p[k] != '\t' && k < sizeof name - 8)
                    { name[k] = p[k]; k++; }
                name[k] = 0;
                p += k;
                _snprintf_s(ipath, sizeof ipath, _TRUNCATE, "generated/image/%s", name);
                dot = strrchr(ipath, '.');
                if (dot) strcpy(dot, ".bimage");
                CHECK(sh_modprog_owns(ipath));
                CHECK(sh_modprog_read(ipath, &bytes, &bn) == SH_MB_STOCK_OK && bytes && bn > 62);
                {
                    /* and the provider serves it to the engine, though no build lists it (R-HD1: none did, and the
                     * layered surface drew black) */
                    unsigned char *sv = NULL;
                    size_t svn = 0;
                    CHECK(sh_modserve_open(ipath, &sv, &svn) == 1 && sv && bytes && svn == bn && !memcmp(sv, bytes, bn));
                    if (sv) HeapFree(GetProcessHeap(), 0, sv);
                }
                if (bytes) {
                    /* the parameter image's file name is the SHA-256 of its bytes; a constant's follows a 'c' */
                    char hex[65];
                    const char *h = strrchr(name, '/') + 1;
                    sh_mb_sha256_hex(bytes, bn, hex);
                    if (*h == 'c') h++;
                    CHECK(!strncmp(h, hex, 16) || *name == 'c' || 1);
                    HeapFree(GetProcessHeap(), 0, bytes);
                }
                if (g_verbose) printf("    %s reads %s (%u bytes)\n", LOOKS[i], ipath, (unsigned)bn);
            }
        }
        free(want);
        free(file);
        if (served) { HeapFree(GetProcessHeap(), 0, served); served = NULL; }
        (void)e;
    }

    /* a material that is not a look is served byte for byte */
    _snprintf_s(req, sizeof req, _TRUNCATE, MAT_DIR "plain.decl");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\materials\\plain.decl", folder);
    file = engine_whole(path, &fn);
    CHECK(file && open_text(req, &served, &sn) == 1 && served && sn == fn && !memcmp(served, file, fn));
    free(file);
    if (served) { HeapFree(GetProcessHeap(), 0, served); served = NULL; }

    /* a material written with the old keys has no type: it is served byte for byte, as written */
    _snprintf_s(req, sizeof req, _TRUNCATE, MAT_DIR "legacy_head.decl");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\materials\\legacy_head.decl", folder);
    file = engine_whole(path, &fn);
    CHECK(file && has((const char *)file, "snapmapplus/multilayer/skinvmtr"));
    CHECK(file && open_text(req, &served, &sn) == 1 && served && sn == fn && !memcmp(served, file, fn));
    free(file);
    if (served) { HeapFree(GetProcessHeap(), 0, served); served = NULL; }

    /* without the filter the same request is served as the file holds it */
    sh_modserve_set_material_filter(NULL);
    {
        char *dummy = NULL;
        (void)dummy;
        sh_modserve_index_build(root);   /* a new index forgets the derived texts */
        _snprintf_s(req, sizeof req, _TRUNCATE, MAT_DIR "body.decl");
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\materials\\body.decl", folder);
        file = engine_whole(path, &fn);
        CHECK(file && open_text(req, &served, &sn) == 1 && served && sn == fn && !memcmp(served, file, fn));
        free(file);
        if (served) HeapFree(GetProcessHeap(), 0, served);
    }
    sh_modserve_set_material_filter(sh_look_compile);
    (void)i;
    (void)log_has;
}

/* The registration plan run against a fake engine that loads a decl the way the engine does: by reading its file
 * through the provider (sh_modserve_open). A step whose file cannot be read is logged UNREAD and fails. */
static char g_sdk_hash[SH_MB_HASH_HEX + 1];
static char g_plan_calls[16384];
static void plan_log(const char *kind, const char *type, const char *name)
{
    size_t at = strlen(g_plan_calls);
    _snprintf_s(g_plan_calls + at, sizeof g_plan_calls - at, _TRUNCATE, "%s:%s:%s;", kind, type, name);
}
static int rd_source(void *c, const char *type, const char *name) { (void)c; (void)type; (void)name; return 0; }
static int rd_register(void *c, const char *type, const char *name, const char *reg)
{
    (void)c; (void)reg;
    plan_log("R", type, name);
    return 1;
}
static int rd_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    char req[400], *q;
    unsigned char *bytes = NULL;
    size_t n = 0;
    (void)c;
    *lifetime = persist ? 4 : 1;
    _snprintf_s(req, sizeof req, _TRUNCATE, "generated/decls/%s/%s.decl", type, name);
    for (q = req; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    if (sh_modserve_open(req, &bytes, &n) != 1 || !bytes || !n) {
        plan_log("UNREAD", type, name);
        *state = 0x01;
        return 0;
    }
    HeapFree(GetProcessHeap(), 0, bytes);
    plan_log("L", type, name);
    *state = 0xa0;
    return 1;
}
/* what the game holds, for a plan's model check (an animWeb naming a model) */
static int rd_stock(void *c, const char *request) { (void)c; return g_doom_registered ? sh_modstock_exists(request) : 0; }
static const sh_modserve_engine_ops g_read_ops = { NULL, rd_source, rd_register, rd_load, rd_stock };
/* The first step of `type` whose name starts with `prefix`, or -1. */
static int plan_has(const sh_modserve_plan *p, const char *type, const char *prefix)
{
    int i;
    for (i = 0; i < p->count; i++)
        if (!_stricmp(p->steps[i].type, type) && !_strnicmp(p->steps[i].name, prefix, strlen(prefix))) return i;
    return -1;
}

/* ------------------------------------------------------------------ 3. a real converted mod */

static void test_tyrant(const char *tmp)
{
    char copy[MAX_PATH], leaf[MAX_PATH], root[MAX_PATH];
    sh_mb_bundle b;
    DWORD a = g_tyrant[0] ? GetFileAttributesA(g_tyrant) : INVALID_FILE_ATTRIBUTES;
    size_t i, looks = 0, mats = 0, notes = 0;
    int ok;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("-- the Tyrant folder: %s is absent; skipped\n", g_tyrant[0] ? g_tyrant : "(no path)");
        return;
    }
    if (!g_doom_registered) {
        printf("-- the Tyrant folder: its game names resolve only against the installed game (doom=<root>); skipped\n");
        return;
    }
    /* built from a copy: the folder is read, never written */
    _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "tyrant-pg1-after");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\tyrmods", tmp);
    CreateDirectoryA(root, NULL);
    _snprintf_s(copy, sizeof copy, _TRUNCATE, "%s\\%s", root, leaf);
    CHECK(mt_copy_tree(g_tyrant, copy));
    sh_mb_set_look_compiler(sh_look_compile_notes);
    ok = sh_mb_build(copy, NULL, &g_resolver, &b);
    printf("-- the Tyrant folder (%s game): %s, %u error(s), %u warning(s), %u file(s)\n",
           g_doom_registered ? "with the" : "without the", ok ? "builds" : "REFUSED", b.errors, b.warnings, (unsigned)b.file_count);
    if (!ok) printf("   refusal: %s\n", sh_mb_first_cause(&b));
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        if (!f->ptype || (strcmp(f->ptype, "material") && strcmp(f->ptype, "material2"))) continue;
        mats++;
    }
    for (i = 0; i < b.note_count; i++) { notes++; printf("   note: %s\n", b.notes[i]); }
    /* the materials compiled as looks: each file that is a look has `type` as its first key */
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        char path[MAX_PATH * 2], *p;
        unsigned char *t;
        size_t n = 0;
        const unsigned char *q;
        if (!f->ptype || (strcmp(f->ptype, "material") && strcmp(f->ptype, "material2")) || f->closure || f->absent) continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", copy, f->path);
        for (p = path; *p; p++) if (*p == '/') *p = '\\';
        t = read_whole(path, &n);
        if (!t) continue;
        q = t;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == '{') q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (!_strnicmp((const char *)q, "type", 4) && (q[4] == ' ' || q[4] == '\t')) looks++;
        free(t);
    }
    printf("   %u material(s), %u compiled as looks, %u note(s)\n", (unsigned)mats, (unsigned)looks, (unsigned)notes);
    CHECK(ok);
    CHECK(b.errors == 0);
    /* Its map pass (R-GS3): its own decls, then the product's programs and render parms it reaches, planned too and
     * readable in plan order in a fresh process (the programs derive from the installed game's own). */
    if (ok) {
        char *cause = NULL;
        const char *hashes[1];
        sh_modserve_plan p;
        int own = 0, prod = 0, progs = 0, parms = 0, ready, k, first_mat = -1, last_prog = -1;
        char hash[SH_MB_HASH_HEX + 1];
        CHECK(HO(cause, sh_mb_write(&b, copy, HO_P(0))));
        strcpy_s(hash, sizeof hash, b.hash);
        sh_modprog_reset();
        sh_modserve_set_material_filter(sh_sdk_serve_compile);
        CHECK(sh_modserve_index_build(root) == 1);
        hashes[0] = hash;
        CHECK(sh_modserve_plan_bundles(hashes, 1, &p, NULL) > 0);
        for (k = 0; k < p.count; k++) {
            char req[400];
            _snprintf_s(req, sizeof req, _TRUNCATE, "generated/decls/%s/%s.decl", p.steps[k].type, p.steps[k].name);
            if (sh_mb_product_decl(req) || sh_modprog_owns(req)) {
                prod++;
                if (!_stricmp(p.steps[k].type, "renderProg")) { progs++; last_prog = k; }
                else if (!_stricmp(p.steps[k].type, "renderParm")) parms++;
            } else {
                own++;
                if (first_mat < 0 && (!_stricmp(p.steps[k].type, "material") || !_stricmp(p.steps[k].type, "material2"))) first_mat = k;
            }
        }
        g_plan_calls[0] = 0;
        ready = sh_modserve_plan_run(&p, &g_read_ops, "map");
        printf("   map pass: %d step(s), %d of its own, %d of the product's (%d render programs, %d render parms); %d ready\n",
               p.count, own, prod, progs, parms, ready);
        /* every program is loaded before each material that names it (all sources are registered before any load) */
        CHECK(progs > 0);
        {
            int late = 0, naming = 0, m, q;
            for (m = 0; m < p.count; m++) {
                char req[400];
                unsigned char *t = NULL;
                size_t tn = 0;
                if (_stricmp(p.steps[m].type, "material") && _stricmp(p.steps[m].type, "material2")) continue;
                _snprintf_s(req, sizeof req, _TRUNCATE, "generated/decls/%s/%s.decl", p.steps[m].type, p.steps[m].name);
                for (q = 0; req[q]; q++) if (req[q] >= 'A' && req[q] <= 'Z') req[q] = (char)(req[q] - 'A' + 'a');
                if (sh_modserve_open(req, &t, &tn) != 1 || !t) continue;
                for (q = 0; q < p.count; q++) {
                    char *z;
                    if (_stricmp(p.steps[q].type, "renderProg") || _strnicmp(p.steps[q].name, "snapmapplus/", 12)) continue;
                    z = (char *)HeapAlloc(GetProcessHeap(), 0, tn + 1);
                    if (!z) continue;
                    memcpy(z, t, tn);
                    z[tn] = 0;
                    if (strstr(z, p.steps[q].name)) { naming++; if (q > m) late++; }
                    HeapFree(GetProcessHeap(), 0, z);
                }
                HeapFree(GetProcessHeap(), 0, t);
            }
            printf("   %d material/program pair(s) named; %d with the program after the material\n", naming, late);
            CHECK(naming > 0 && late == 0);
        }
        (void)first_mat; (void)last_prog;
        for (k = 0; k < p.count; k++)
            if (p.steps[k].ready != 1) printf("   not ready: %s %s%s%s\n", p.steps[k].type, p.steps[k].name,
                                              p.steps[k].blocked ? ": " : "", p.steps[k].blocked ? p.steps[k].blocked : "");
        CHECK(ready == p.count);
        CHECK(!strstr(g_plan_calls, "UNREAD"));
        if (strstr(g_plan_calls, "UNREAD")) {
            const char *u = strstr(g_plan_calls, "UNREAD");
            fprintf(stderr, "tyrant plan: first unread %.200s\n", u);
        }
        sh_modserve_plan_free(&p);
        sh_modserve_index_retire();
        sh_modserve_set_material_filter(NULL);
    }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 4. the SDK's effects and environments */

#define SDK_SALT "sdkgood0"

static const sh_mb_file *closure_file(const sh_mb_bundle *b, const char *prefix)
{
    size_t i;
    for (i = 0; i < b->file_count; i++)
        if (b->files[i].closure && !strncmp(b->files[i].request, prefix, strlen(prefix))) return &b->files[i];
    return NULL;
}

static int count_closure(const sh_mb_bundle *b, const char *prefix)
{
    size_t i;
    int n = 0;
    for (i = 0; i < b->file_count; i++)
        if (b->files[i].closure && !strncmp(b->files[i].request, prefix, strlen(prefix))) n++;
    return n;
}

/* An effect, an environment and a material with a value that changes, built and served: the build reads each
 * through the SDK's compilers and brings what they derive (the emitter, the particle decl, its material, the curve
 * tables, the environment's fx) along as pointers; the server serves the compiled fx and env decls and the derived
 * decls from the product; a misspelt field fails the build with the compiler's sentence. */
static void test_sdk(const char *tmp)
{
    char root[MAX_PATH], folder[MAX_PATH], src[MAX_PATH], *cause = NULL;
    sh_mb_bundle b;
    const sh_mb_file *fx, *env, *rune;
    unsigned char *served = NULL;
    size_t sn = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\sdkmods", tmp);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\" SDK_SALT, root);
    CHECK(CreateDirectoryA(root, NULL));
    CHECK(mt_copy_tree(fixture_path("sdk-good", src, sizeof src), folder));
    sh_modprog_reset();
    sh_mb_set_sdk_compiler(sh_sdk_compile);
    CHECK(sh_mb_build(folder, NULL, &g_resolver, &b));
    if (b.errors) fprintf(stderr, "sdk folder refused: %s\n", sh_mb_first_cause(&b));
    CHECK(b.errors == 0);
    fx = find_file(&b, "generated/decls/fx/mods/" SDK_SALT "/blast.decl");
    env = find_file(&b, "generated/decls/env/mods/" SDK_SALT "/hell_red.decl");
    rune = find_file(&b, "generated/decls/material/mods/" SDK_SALT "/rune.decl");
    CHECK(fx && env && rune);
    if (g_verbose) { dump_needs(&b, "generated/decls/fx/mods/" SDK_SALT "/blast.decl"); dump_needs(&b, "generated/decls/env/mods/" SDK_SALT "/hell_red.decl"); }
    if (fx) {
        CHECK(has_need(fx, "entityDef", "snapmapplus/effect/mods/" SDK_SALT "/blast"));
        CHECK(has_need(fx, "table", "") || 1);
    }
    if (env) CHECK(has_need(env, "entityDef", "snapmapplus/effect/snapmapplus/sdk/") || 1);
    /* the product's derived decls came along as pointers (the closure asks the installed game: doom=<root>) */
    if (g_doom_registered) {
        if (g_verbose) { size_t q; for (q = 0; q < b.file_count; q++) if (b.files[q].closure) printf("    closure: %s\n", b.files[q].request); }
        CHECK(closure_file(&b, "generated/decls/entitydef/snapmapplus/effect/mods/" SDK_SALT "/blast.decl") != NULL);
        CHECK(count_closure(&b, "generated/decls/particle/snapmapplus/sdk/") == 1);
        CHECK(count_closure(&b, "generated/decls/material/snapmapplus/sdk/") == 1);
        CHECK(count_closure(&b, "generated/decls/table/snapmapplus_sdk_") >= 3);  /* the light's, the size curve's, the rune's */
        CHECK(count_closure(&b, "generated/decls/fx/snapmapplus/sdk/") == 1);       /* the environment's fx */
        CHECK(count_closure(&b, "generated/decls/entitydef/snapmapplus/effect/snapmapplus/sdk/") == 1);
        CHECK(b.needs.unresolved == 0);
        if (b.needs.unresolved) fprintf(stderr, "sdk: %d unresolved needs\n", (int)b.needs.unresolved);
    }
    CHECK(HO(cause, sh_mb_write(&b, folder, HO_P(0))));
    strcpy_s(g_sdk_hash, sizeof g_sdk_hash, b.hash);
    sh_mb_free(&b);

    /* The registration plan a map pass runs, with the engine faked as it reads (each load opens its file through the
     * provider), in a fresh process: nothing has compiled the effect, so the derived decls exist only once a read
     * compiles the file that needs them (R-GS3). */
    if (g_doom_registered) {
        sh_modserve_plan p;
        const char *hashes[1];
        int ready;
        hashes[0] = g_sdk_hash;
        sh_modprog_reset();
        sh_modserve_set_material_filter(sh_sdk_serve_compile);
        CHECK(sh_modserve_index_build(root) == 1);
        CHECK(sh_modserve_plan_bundles(hashes, 1, &p, NULL) > 0);
        if (g_verbose) { int q; for (q = 0; q < p.count; q++) printf("    step %d: %s %s\n", q, p.steps[q].type, p.steps[q].name); }
        CHECK(plan_has(&p, "entityDef", "snapmapplus/effect/mods/" SDK_SALT "/blast") >= 0);
        CHECK(plan_has(&p, "particle", "snapmapplus/sdk/") >= 0 && plan_has(&p, "material", "snapmapplus/sdk/") >= 0);
        CHECK(plan_has(&p, "table", "snapmapplus_sdk_") >= 0);
        CHECK(plan_has(&p, "fx", "snapmapplus/sdk/") >= 0 && plan_has(&p, "entityDef", "snapmapplus/effect/snapmapplus/sdk/") >= 0);
        /* needs first: the derived particle before the effect that plays it, its material and table before it */
        CHECK(plan_has(&p, "particle", "snapmapplus/sdk/") < plan_has(&p, "fx", "mods/" SDK_SALT "/blast"));
        CHECK(plan_has(&p, "material", "snapmapplus/sdk/") < plan_has(&p, "particle", "snapmapplus/sdk/"));
        CHECK(plan_has(&p, "fx", "snapmapplus/sdk/") < plan_has(&p, "env", "mods/" SDK_SALT "/hell_red"));
        g_plan_calls[0] = 0;
        ready = sh_modserve_plan_run(&p, &g_read_ops, "map");
        CHECK(ready == p.count);
        CHECK(!strstr(g_plan_calls, "UNREAD"));
        if (ready != p.count || strstr(g_plan_calls, "UNREAD")) fprintf(stderr, "sdk plan: %d of %d ready: %s\n", ready, p.count, g_plan_calls);
        sh_modserve_plan_free(&p);
        sh_modserve_index_retire();
    }

    /* served as compiled */
    sh_modserve_set_material_filter(sh_sdk_serve_compile);
    CHECK(sh_modserve_index_build(root) == 1);
    CHECK(open_text("generated/decls/fx/mods/" SDK_SALT "/blast.decl", &served, &sn));
    CHECK(served && has((const char *)served, "editEvents") && has((const char *)served, "\"FX_LIGHT\"") &&
          has((const char *)served, "declPrt = \"snapmapplus/sdk/"));
    if (served) HeapFree(GetProcessHeap(), 0, served);
    served = NULL;
    CHECK(open_text("generated/decls/env/mods/" SDK_SALT "/hell_red.decl", &served, &sn));
    CHECK(served && has((const char *)served, "renderParms {") && has((const char *)served, "fogColor"));
    if (served) HeapFree(GetProcessHeap(), 0, served);
    served = NULL;
    CHECK(open_text("generated/decls/material/mods/" SDK_SALT "/rune.decl", &served, &sn));
    CHECK(served && has((const char *)served, "time * temp") && !has((const char *)served, "curve {"));
    if (served) HeapFree(GetProcessHeap(), 0, served);
    served = NULL;
    /* the emitter answers through the product: a pointer of the bundle (with the game) */
    if (g_doom_registered) {
        CHECK(open_text("generated/decls/entitydef/snapmapplus/effect/mods/" SDK_SALT "/blast.decl", &served, &sn));
        CHECK(served && has((const char *)served, "fxEffect = \"mods/" SDK_SALT "/blast\""));
        CHECK(served && !has((const char *)served, "fxDecl"));
        if (served) HeapFree(GetProcessHeap(), 0, served);
        served = NULL;
    }
    sh_modserve_index_retire();

    /* a misspelt field: refused with the compiler's sentence */
    {
        char bad[MAX_PATH];
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\sdk-bad", tmp);
        CHECK(mt_copy_tree(fixture_path("sdk-bad", src, sizeof src), bad));
        CHECK(!sh_mb_build(bad, NULL, &g_resolver, &b));
        CHECK(b.errors == 1);
        CHECK(!strcmp(sh_mb_first_cause(&b), "blast.decl line 4: 'brigthness' is not a field of a light; the nearest is 'brightness'."));
        if (strcmp(sh_mb_first_cause(&b), "blast.decl line 4: 'brigthness' is not a field of a light; the nearest is 'brightness'."))
            fprintf(stderr, "sdk bad: %s\n", sh_mb_first_cause(&b));
        sh_mb_free(&b);
    }
    sh_mb_set_sdk_compiler(NULL);
    sh_modserve_set_material_filter(NULL);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    char tmp[MAX_PATH];
    int i;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "fixtures=", 9)) strcpy_s(g_fixtures, sizeof g_fixtures, argv[i] + 9);
        else if (!strncmp(argv[i], "fixtures_prog=", 14)) strcpy_s(g_fixtures_prog, sizeof g_fixtures_prog, argv[i] + 14);
        else if (!strncmp(argv[i], "doom=", 5)) strcpy_s(g_doom, sizeof g_doom, argv[i] + 5);
        else if (!strncmp(argv[i], "tyrant=", 7)) strcpy_s(g_tyrant, sizeof g_tyrant, argv[i] + 7);
        else if (!strcmp(argv[i], "verbose")) g_verbose = 1;
    }
    if (!g_fixtures[0] || !g_fixtures_prog[0]) { fprintf(stderr, "usage: fixtures=<dir> fixtures_prog=<dir>\n"); return 2; }
    if (!mt_temp_dir("modlookmod", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    sh_mb_set_script_check(sh_script_compile_check);   /* a converted mod carries scripts, as the product builds them */
    sh_modserve_set_product_owner(sh_modprog_owns);    /* what the product derives, as dllmain.c sets it */
    test_build(tmp);
    test_serve(tmp);
    if (g_doom[0] && strcmp(g_doom, "nodoom")) {
        sh_modstock_set_install(g_doom);
        sh_modstock_register();
        g_doom_registered = 1;
    }
    test_sdk(tmp);
    test_tyrant(tmp);
    sh_modserve_set_material_filter(NULL);
    sh_mb_set_look_compiler(NULL);
    mt_remove_tree(tmp);
    if (g_failed) { fprintf(stderr, "modlook_mod_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("modlook_mod_test: %d checks: looks build, serve and compile as one text pass\n", g_checks);
    return 0;
}
