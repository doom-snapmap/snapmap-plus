/* modunit_test.c -- the unit (modunit.h): the closure of one placed object inside one mod build.
 *
 * Name keys, the closure of an object and its independence from the rest of the pack, the identity (a change in
 * another object's files leaves the hash alone), the proof that refuses a changed file, the registry and enumerated
 * decls, boards cut at the square, scripts, what players asked for, the mod.json share, composition and storing.
 *
 *   modunit_test.exe
 */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modauthor.h"
#include "modbundle.h"
#include "modunit.h"
#include "mod_test_util.h"

#define SALT "pk7salt1"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static char g_last_log[512];
void backend_log(const char *message) { if (message) strcpy_s(g_last_log, sizeof g_last_log, message); }

/* The game holds every name outside mods/; nothing under it. */
static int game_exists(void *ctx, const char *request)
{
    (void)ctx;
    return strstr(request, "mods/") ? 0 : 1;
}

static const sh_mb_resolver g_game = { NULL, NULL, game_exists, NULL };

/* The unit environment's game: the same answer with a fixed hash. */
static int game_where(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}

static int script_ok(const char *file, int entry, const char *text, size_t len, int *line, char **what, char **todo)
{
    (void)file; (void)entry; (void)text; (void)len; (void)line; (void)what; (void)todo;
    return 1;
}

/* A binary whose bytes spell `name` between other bytes, as a mesh names its material. */
static void put_binary(const char *folder, const char *rel, const char *name, size_t pad, unsigned seed)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    size_t i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) return;
    for (i = 0; i < pad; i++) fputc((int)((i * 7 + seed) % 251), f);
    if (name) { fputc(0, f); fwrite(name, 1, strlen(name), f); fputc(0, f); }
    for (i = 0; i < 64; i++) fputc(0, f);
    fclose(f);
}

/* One object as a Toybox mod lays it out: entityDef -> model (a binary naming its material) -> material -> image,
 * and a Toybox row (decls/snapeditorentitydef/<n>_row.decl) naming the entityDef. `mat_extra` goes into the material. */
static void add_object(const char *folder, const char *n, size_t pad, const char *mat_extra)
{
    char rel[MAX_PATH], text[1024], spell[128];
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "entities\\%s.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
                "\t\t\tmodel = \"mods/" SALT "/models/%s.lwo\";\n\t\t}\n\t}\n}\n", n);
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "meshes\\models\\%s.bmodel", n);
    _snprintf_s(spell, sizeof spell, _TRUNCATE, "mods/" SALT "/%s_skin", n);
    put_binary(folder, rel, spell, pad, 1);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "materials\\%s_skin.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\ndiffusemap\t\"mods/" SALT "/%s_d.tga\"\n%s}\n", n,
                mat_extra ? mat_extra : "");
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "images\\%s_d.bimage", n);
    put_binary(folder, rel, NULL, pad, 3);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "decls\\snapeditorentitydef\\%s_row.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tinherit = \"action/door/open\";\n\tedit = {\n\t\tentityDef = \"mods/" SALT "/%s\";\n"
                "\t\tname = \"#str_%s_name\";\n\t}\n}\n", n, n);
    mt_write(folder, rel, text);
}

static void write_modjson(const char *folder)
{
    mt_write(folder, "mod.json",
             "{\n  \"name\": \"Crates and barrels\",\n  \"description\": \"Props.\",\n"
             "  \"strings\": { \"en\": { \"crate_name\": \"Crate\", \"barrel_name\": \"Barrel\", \"lever_name\": \"Lever\" } }\n}\n");
}

static void write_pack(const char *folder)
{
    write_modjson(folder);
    add_object(folder, "crate", 200, NULL);
    add_object(folder, "barrel", 3000, NULL);
}

static int build(const char *folder, sh_mb_bundle *b)
{
    memset(b, 0, sizeof *b);
    if (!sh_mb_build(folder, SALT, &g_game, b) || b->errors) {
        fprintf(stderr, "build of %s: %s\n", folder, sh_mb_first_cause(b) ? sh_mb_first_cause(b) : "(no cause)");
        failures++;
        return 0;
    }
    return 1;
}

static sh_unit_env make_env(const sh_unit_asked *asked, size_t n)
{
    sh_unit_env e;
    memset(&e, 0, sizeof e);
    e.game = game_where;
    e.asked = asked;
    e.asked_count = n;
    return e;
}

/* The unit of `name`; prints the reason when it fails. */
static int unit_with(const sh_mb_bundle *b, const char *folder, const char *name, const sh_unit_env *env, sh_unit *u)
{
    char *why = NULL;
    int ok = sh_unit_of(b, folder, name, env, u, &why);
    if (!ok) fprintf(stderr, "unit %s: %s\n", name, why ? why : "(no reason)");
    free(why);
    return ok;
}

static int unit(const sh_mb_bundle *b, const char *folder, const char *name, sh_unit *u)
{
    sh_unit_env env = make_env(NULL, 0);
    return unit_with(b, folder, name, &env, u);
}

/* 1 kept, 0 not kept, -1 no such file (reported). `path` is the mod-relative path. */
static int kept(const sh_mb_bundle *b, const sh_unit *u, const char *path)
{
    size_t i;
    for (i = 0; i < b->file_count; i++)
        if (!_stricmp(b->files[i].path, path)) return u->keep && u->keep[i] ? 1 : 0;
    fprintf(stderr, "no file %s in the build\n", path);
    failures++;
    return -1;
}

static size_t count_of(const char *hay, const char *needle)
{
    size_t n = 0, l = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += l; }
    return n;
}

/* ------------------------------------------------------------------ 1 */

static void test_keys(void)
{
    char *k;
    CHECK((k = sh_unit_key("generated/image/mods/" SALT "/a/b-c.tga$mips", 99, SALT, SALT)) && !strcmp(k, "mods/" SALT "/a/b_c"));
    free(k);
    CHECK((k = sh_unit_key("mods/othersal/x.decl", 20, "othersal", SALT)) && !strcmp(k, "mods/" SALT "/x"));
    free(k);
    CHECK(!sh_unit_key("mods/" SALT "/", 14, SALT, SALT));
    CHECK(sh_unit_enumerated_type("snapAIScore") && sh_unit_enumerated_type("snapaiencounter"));
    CHECK(!sh_unit_enumerated_type("entityDef"));
}

/* ------------------------------------------------------------------ 2 */

static void test_closure(const char *top)
{
    char folder[MAX_PATH * 2], hex[SH_MB_HASH_HEX + 1];
    sh_mb_bundle b;
    sh_unit u, row;
    size_t n;
    const char *tail = ",\"spelled\":\"" SALT "\"}";

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\closure", top);
    write_pack(folder);
    if (!build(folder, &b)) return;

    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(u.kind == SH_UNIT_ENTITY);
        CHECK(u.root && !strcmp(u.root, "entities/crate"));
        CHECK(kept(&b, &u, "entities/crate.decl") == 1);
        CHECK(kept(&b, &u, "meshes/models/crate.bmodel") == 1);
        CHECK(kept(&b, &u, "materials/crate_skin.decl") == 1);
        CHECK(kept(&b, &u, "images/crate_d.bimage") == 1);
        CHECK(kept(&b, &u, "decls/snapeditorentitydef/crate_row.decl") == 1);
        CHECK(kept(&b, &u, "entities/barrel.decl") == 0);
        CHECK(kept(&b, &u, "meshes/models/barrel.bmodel") == 0);
        CHECK(kept(&b, &u, "materials/barrel_skin.decl") == 0);
        CHECK(kept(&b, &u, "images/barrel_d.bimage") == 0);
        CHECK(kept(&b, &u, "decls/snapeditorentitydef/barrel_row.decl") == 0);
        CHECK(u.files == 5);
        CHECK(u.manifest && u.manifest_len == strlen(u.manifest));
        if (u.manifest) {
            n = u.manifest_len;
            CHECK(!strncmp(u.manifest, "{\"files\":[", 10));
            CHECK(strstr(u.manifest, "\"format\":1") != NULL);
            CHECK(strstr(u.manifest, "\"root\":{\"kind\":\"entity\",\"name\":\"entities/crate\"}") != NULL);
            /* the binaries spell the salt: it ends the manifest, and nowhere else is it written */
            CHECK(n > strlen(tail) && !strcmp(u.manifest + n - strlen(tail), tail));
            CHECK(count_of(u.manifest, SALT) == 1);
            CHECK(!strstr(u.manifest, "description") && !strstr(u.manifest, "Crates and barrels"));
            sh_mb_sha256_hex(u.manifest, u.manifest_len, hex);
            CHECK(!strcmp(hex, u.hash));
        }
        CHECK(!strcmp(u.spelled, SALT));
        sh_unit_free(&u);
    }

    /* the Toybox row is a root of its own: the same files, kind toybox */
    if (unit(&b, folder, "mods/" SALT "/crate_row", &row)) {
        CHECK(row.kind == SH_UNIT_TOYBOX);
        CHECK(kept(&b, &row, "decls/snapeditorentitydef/crate_row.decl") == 1);
        CHECK(kept(&b, &row, "entities/crate.decl") == 1);
        CHECK(kept(&b, &row, "images/crate_d.bimage") == 1);
        CHECK(kept(&b, &row, "entities/barrel.decl") == 0);
        CHECK(kept(&b, &row, "decls/snapeditorentitydef/barrel_row.decl") == 0);
        sh_unit_free(&row);
    }
    /* an author name asks for the same unit as the placed name */
    if (unit(&b, folder, "entities/crate", &u)) {
        CHECK(u.kind == SH_UNIT_ENTITY);
        sh_unit_free(&u);
    }
    {
        char *why = NULL;
        CHECK(!sh_unit_of(&b, folder, "mods/" SALT "/nothing", &(sh_unit_env){ game_where, NULL, NULL, 0 }, &u, &why) && why);
        free(why);
    }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 3, 4 */

static void test_identity(const char *top)
{
    char folder[MAX_PATH * 2], crate1[SH_MB_HASH_HEX + 1], barrel1[SH_MB_HASH_HEX + 1], whole1[SH_MB_HASH_HEX + 1];
    sh_mb_bundle b, b2, b3;
    sh_unit u;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\identity", top);
    write_pack(folder);
    if (!build(folder, &b)) return;
    crate1[0] = barrel1[0] = whole1[0] = 0;
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) { strcpy_s(crate1, sizeof crate1, u.hash); sh_unit_free(&u); }
    if (unit(&b, folder, "mods/" SALT "/barrel", &u)) { strcpy_s(barrel1, sizeof barrel1, u.hash); sh_unit_free(&u); }
    if (unit(&b, folder, SH_UNIT_WHOLE, &u)) {
        CHECK(u.kind == SH_UNIT_MOD && !strcmp(u.root, "*"));
        strcpy_s(whole1, sizeof whole1, u.hash);
        sh_unit_free(&u);
    }
    CHECK(crate1[0] && barrel1[0] && whole1[0] && strcmp(crate1, barrel1));

    /* another object's file changes: the crate's unit does not */
    put_binary(folder, "images\\barrel_d.bimage", NULL, 3500, 9);
    if (build(folder, &b2)) {
        if (unit(&b2, folder, "mods/" SALT "/crate", &u)) { CHECK(!strcmp(u.hash, crate1)); sh_unit_free(&u); }
        if (unit(&b2, folder, "mods/" SALT "/barrel", &u)) { CHECK(strcmp(u.hash, barrel1) != 0); sh_unit_free(&u); }
        if (unit(&b2, folder, SH_UNIT_WHOLE, &u)) { CHECK(strcmp(u.hash, whole1) != 0); sh_unit_free(&u); }

        /* an object is added: the crate's unit is still the same one */
        add_object(folder, "lever", 500, NULL);
        if (build(folder, &b3)) {
            if (unit(&b3, folder, "mods/" SALT "/crate", &u)) { CHECK(!strcmp(u.hash, crate1)); sh_unit_free(&u); }
            if (unit(&b3, folder, "mods/" SALT "/lever", &u)) {
                CHECK(strcmp(u.hash, crate1) != 0 && kept(&b3, &u, "images/lever_d.bimage") == 1);
                sh_unit_free(&u);
            }
            sh_mb_free(&b3);
        }
        /* the crate's own image changes: its unit does */
        put_binary(folder, "images\\crate_d.bimage", NULL, 250, 5);
        sh_mb_free(&b2);
        if (build(folder, &b2)) {
            if (unit(&b2, folder, "mods/" SALT "/crate", &u)) { CHECK(strcmp(u.hash, crate1) != 0); sh_unit_free(&u); }
            sh_mb_free(&b2);
        }
    }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 5 */

static void test_proof(const char *top)
{
    char folder[MAX_PATH * 2], *why = NULL;
    sh_mb_bundle b;
    sh_unit u;
    sh_unit_env env = make_env(NULL, 0);

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\proof", top);
    write_pack(folder);
    if (!build(folder, &b)) return;
    put_binary(folder, "images\\crate_d.bimage", NULL, 222, 11);   /* not rebuilt */
    CHECK(!sh_unit_of(&b, folder, "mods/" SALT "/crate", &env, &u, &why));
    CHECK(why && strstr(why, "changed since the mod was built"));
    if (why) fprintf(stderr, "  (proof said: %s)\n", why);
    free(why);
    if (unit(&b, folder, "mods/" SALT "/barrel", &u)) sh_unit_free(&u);
    sh_mb_free(&b);
}

/* 6: a native/ edited copy of the conductor list. The builder derives the copy's edits against the game's own text,
 * read here from a fake installed-content reader. The pack's edits name both objects; a composition of the crate's
 * unit alone keeps only the crate's entry (R-DS5: a unit's copy of a changed DOOM decl keeps the entries for what it
 * carries, so the conductor never names a demon the map was not given). */
#define CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
static const char g_conductor_stock[] =
    "{\n\tinherit = \"ai/base/conductor\";\n\tedit = {\n\t\taiTypeList = {\n\t\t\tnum = 1;\n"
    "\t\t\titem[0] = {\n\t\t\t\taiType = \"1\";\n\t\t\t\tentityDef = \"ai/demon/imp\";\n\t\t\t}\n\t\t}\n\t}\n}\n";
static int cs_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    (void)c;
    if (_stricmp(source, CONDUCTOR)) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(g_conductor_stock, strlen(g_conductor_stock), sha);
    *size = strlen(g_conductor_stock);
    return SH_MB_STOCK_OK;
}
static int cs_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    (void)c; (void)sha;
    if (_stricmp(source, CONDUCTOR)) return SH_MB_STOCK_ABSENT;
    *len = strlen(g_conductor_stock);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len + 1);
    if (!*bytes) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, g_conductor_stock, *len);
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops g_conductor_ops = { NULL, NULL, cs_identify, NULL, cs_read };

/* 6b: a native/ copy of a DOOM material, unchanged (a surface: the mod serves DOOM's own bytes at DOOM's name). The
 * sweep of the owner's mods found one named only by a DOOM mesh the mod borrows, which no edge of the mod's own files
 * reaches: it travels in every unit, as a pointer, as the editor serves it wherever the mod is used. */
#define SURFACE_MAT "generated/decls/material/models/mapobjects/rock_kit.decl"
static const char g_surface_text[] = "{\ndiffusemap\t\"models/mapobjects/rock_kit\"\n}\n";
static int sf_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    (void)c;
    if (_stricmp(source, SURFACE_MAT)) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(g_surface_text, strlen(g_surface_text), sha);
    *size = strlen(g_surface_text);
    return SH_MB_STOCK_OK;
}
static int sf_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    (void)c; (void)sha;
    if (_stricmp(source, SURFACE_MAT)) return SH_MB_STOCK_ABSENT;
    *len = strlen(g_surface_text);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len + 1);
    if (!*bytes) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, g_surface_text, *len);
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops g_surface_ops = { NULL, NULL, sf_identify, NULL, sf_read };

static void test_surface(const char *top)
{
    char folder[MAX_PATH * 2];
    sh_mb_bundle b;
    sh_unit u;
    size_t i;
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\surface", top);
    write_pack(folder);
    mt_write(folder, "native\\generated\\decls\\material\\models\\mapobjects\\rock_kit.decl", g_surface_text);
    sh_mb_set_stock_ops(&g_surface_ops);
    if (!build(folder, &b)) { sh_mb_set_stock_ops(NULL); return; }
    for (i = 0; i < b.file_count; i++)
        if (!_stricmp(b.files[i].path, "native/" SURFACE_MAT)) CHECK(b.files[i].cls == SH_MB_CLASS_SURFACE);
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(kept(&b, &u, "native/" SURFACE_MAT) == 1);
        CHECK(u.manifest && strstr(u.manifest, "\"at\":\"native/" SURFACE_MAT "\""));
        CHECK(u.manifest && !strstr(u.manifest, "{\"path\":\"native/"));   /* a pointer, never carried */
        sh_unit_free(&u);
    }
    if (unit(&b, folder, "mods/" SALT "/barrel", &u)) {
        CHECK(kept(&b, &u, "native/" SURFACE_MAT) == 1);
        sh_unit_free(&u);
    }
    sh_mb_free(&b);
    sh_mb_set_stock_ops(NULL);
}

typedef struct src_ctx { const char *folder, *swap_path, *swap_file; } src_ctx;
static char *source_of(void *context, const char *path);

static void test_registry_edits(const char *top)
{
    char folder[MAX_PATH * 2], dest[MAX_PATH * 2], *why = NULL;
    sh_mb_bundle b, out;
    sh_unit u;
    const char *manifests[1];
    size_t lens[1];
    src_ctx ctx;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\registry", top);
    _snprintf_s(dest, sizeof dest, _TRUNCATE, "%s\\registry_comp", top);
    write_pack(folder);
    mt_write(folder, "native\\generated\\decls\\entitydef\\ai\\conductor\\coop\\snapmap_default.decl",
             "{\n\tinherit = \"ai/base/conductor\";\n\tedit = {\n\t\taiTypeList = {\n\t\t\tnum = 3;\n"
             "\t\t\titem[0] = {\n\t\t\t\taiType = \"1\";\n\t\t\t\tentityDef = \"ai/demon/imp\";\n\t\t\t}\n"
             "\t\t\titem[1] = {\n\t\t\t\taiType = \"2\";\n\t\t\t\tentityDef = \"mods/" SALT "/crate\";\n\t\t\t}\n"
             "\t\t\titem[2] = {\n\t\t\t\taiType = \"4\";\n\t\t\t\tentityDef = \"mods/" SALT "/barrel\";\n\t\t\t}\n"
             "\t\t}\n\t}\n}\n");
    sh_mb_set_stock_ops(&g_conductor_ops);
    if (!build(folder, &b)) { sh_mb_set_stock_ops(NULL); return; }
    if (!b.edits) fprintf(stderr, "  pack edits: none (%s)\n", sh_mb_first_warning(&b) ? sh_mb_first_warning(&b) : "-");
    /* The pack carries both objects: both entries are its edits. */
    CHECK(b.edits && strstr(b.edits, "mods/" SALT "/crate") && strstr(b.edits, "mods/" SALT "/barrel"));
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(kept(&b, &u, "native/" CONDUCTOR) == 1);
        manifests[0] = u.manifest;
        lens[0] = u.manifest_len;
        ctx.folder = folder;
        ctx.swap_path = NULL;
        ctx.swap_file = NULL;
        memset(&out, 0, sizeof out);
        CHECK(sh_unit_compose(manifests, lens, 1, source_of, &ctx, SALT, "Composed", &g_game, dest, &out, &why));
        if (why) { fprintf(stderr, "compose: %s\n", why); free(why); why = NULL; }
        CHECK(out.edits && strstr(out.edits, "mods/" SALT "/crate") && !strstr(out.edits, "mods/" SALT "/barrel"));
        CHECK(out.edits && strstr(out.edits, "\"counts\":{\"set\":0,\"remove\":0,\"list\":1,"));
        if (out.edits && strstr(out.edits, "barrel")) fprintf(stderr, "  composed edits: %s\n", out.edits);
        sh_mb_free(&out);
        sh_unit_free(&u);
    }
    sh_mb_free(&b);
    sh_mb_set_stock_ops(NULL);
}

/* ------------------------------------------------------------------ 7 */

static void test_boards(const char *top)
{
    char folder[MAX_PATH * 2];
    sh_mb_bundle b;
    sh_unit u;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\boards", top);
    write_modjson(folder);
    add_object(folder, "crate", 200,
               "landpagefile \"mods/" SALT "/board\"\nvirtualmapping { 0.25, 0.25, 0.0, 0.0 }\n");
    add_object(folder, "barrel", 300,
               "landpagefile \"mods/" SALT "/board\"\nvirtualmapping { 0.25, 0.25, 0.25, 0.25 }\n");
    add_object(folder, "lever", 400, "landpagefile \"mods/" SALT "/board\"\n");
    put_binary(folder, "virtual-textures\\board_sq1.mega2", NULL, 100, 1);
    put_binary(folder, "virtual-textures\\board_sq6.mega2", NULL, 100, 2);
    put_binary(folder, "virtual-textures\\board_sq11.mega2", NULL, 100, 3);
    if (!build(folder, &b)) return;
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(kept(&b, &u, "virtual-textures/board_sq1.mega2") == 1);
        CHECK(kept(&b, &u, "virtual-textures/board_sq6.mega2") == 0);
        CHECK(kept(&b, &u, "virtual-textures/board_sq11.mega2") == 0);
        sh_unit_free(&u);
    }
    /* D-A29: an object drawing from square 6 alone keeps square 1 too, the only source of the family's coarsest
     * page when it has no _q members; no other square comes with it. */
    if (unit(&b, folder, "mods/" SALT "/barrel", &u)) {
        CHECK(kept(&b, &u, "virtual-textures/board_sq1.mega2") == 1);
        CHECK(kept(&b, &u, "virtual-textures/board_sq6.mega2") == 1);
        CHECK(kept(&b, &u, "virtual-textures/board_sq11.mega2") == 0);
        sh_unit_free(&u);
    }
    if (unit(&b, folder, "mods/" SALT "/lever", &u)) {
        CHECK(kept(&b, &u, "virtual-textures/board_sq1.mega2") == 1);
        CHECK(kept(&b, &u, "virtual-textures/board_sq6.mega2") == 1);
        CHECK(kept(&b, &u, "virtual-textures/board_sq11.mega2") == 1);
        sh_unit_free(&u);
    }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 8 */

static void test_scripts(const char *top)
{
    char folder[MAX_PATH * 2];
    sh_mb_bundle b;
    sh_unit u;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\scripts", top);
    write_pack(folder);
    mt_write(folder, "scripts\\crate.lua", "api = 1\nlocal crate_lib = require(\"crate_lib\")\nlocal spec = { attacks = \"crate_attacks\" }\nlocal attacks = require(spec.attacks)\nlocal def = \"entities/crate\"\n");
    mt_write(folder, "scripts\\lib\\crate_lib.lua", "local extra = \"materials/crate_extra\"\nreturn extra\n");
    /* a module the script requires by a name it passes on (R-LV2) */
    mt_write(folder, "scripts\\lib\\crate_attacks.lua", "return {}\n");
    mt_write(folder, "materials\\crate_extra.decl", "{\n}\n");
    if (!build(folder, &b)) return;
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(kept(&b, &u, "scripts/crate.lua") == 1);
        CHECK(kept(&b, &u, "scripts/lib/crate_lib.lua") == 1);
        CHECK(kept(&b, &u, "materials/crate_extra.decl") == 1);
        CHECK(kept(&b, &u, "scripts/lib/crate_attacks.lua") == 1);
        sh_unit_free(&u);
    }
    if (unit(&b, folder, "mods/" SALT "/barrel", &u)) {
        CHECK(kept(&b, &u, "scripts/crate.lua") == 0);
        CHECK(kept(&b, &u, "scripts/lib/crate_lib.lua") == 0);
        CHECK(kept(&b, &u, "scripts/lib/crate_attacks.lua") == 0);
        CHECK(kept(&b, &u, "materials/crate_extra.decl") == 0);
        sh_unit_free(&u);
    }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 9 */

static void test_asked(const char *top)
{
    char folder[MAX_PATH * 2];
    sh_mb_bundle b;
    sh_unit u;
    sh_unit_asked any = { "generated/image/mods/othersal/spare.bimage", "" };
    sh_unit_asked by_barrel = { "generated/image/mods/othersal/spare.bimage", "mods/" SALT "/barrel" };
    sh_unit_env e_any, e_by;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\asked", top);
    write_pack(folder);
    put_binary(folder, "images\\spare.bimage", NULL, 80, 4);
    if (!build(folder, &b)) return;
    e_any = make_env(&any, 1);
    e_by = make_env(&by_barrel, 1);
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) { CHECK(kept(&b, &u, "images/spare.bimage") == 0); sh_unit_free(&u); }
    if (unit_with(&b, folder, "mods/" SALT "/crate", &e_any, &u)) { CHECK(kept(&b, &u, "images/spare.bimage") == 1); sh_unit_free(&u); }
    if (unit_with(&b, folder, "mods/" SALT "/crate", &e_by, &u)) { CHECK(kept(&b, &u, "images/spare.bimage") == 0); sh_unit_free(&u); }
    if (unit_with(&b, folder, "mods/" SALT "/barrel", &e_by, &u)) { CHECK(kept(&b, &u, "images/spare.bimage") == 1); sh_unit_free(&u); }
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 10 */

static void test_mod_share(const char *top)
{
    char folder[MAX_PATH * 2];
    sh_mb_bundle b;
    sh_unit u;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\share", top);
    write_pack(folder);
    if (!build(folder, &b)) return;
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        CHECK(strstr(u.manifest, "crate_name") != NULL);
        CHECK(!strstr(u.manifest, "barrel_name") && !strstr(u.manifest, "lever_name"));
        CHECK(!strstr(u.manifest, "description") && !strstr(u.manifest, "Crates and barrels") && !strstr(u.manifest, "\"name\":\"Crates"));
        sh_unit_free(&u);
    }
    sh_mb_free(&b);

    /* D-A16: an author's "version" (or any key nothing serves) is never part of a unit, so bumping it keeps the
     * unit's hash. */
    {
        char hash1[SH_MB_HASH_HEX + 1] = "", hash2[SH_MB_HASH_HEX + 1] = "";
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\share_version", top);
        write_pack(folder);
        mt_write(folder, "mod.json",
                 "{\n  \"name\": \"Crates and barrels\",\n  \"version\": \"1.0\",\n  \"notes\": {\"a\": 1},\n"
                 "  \"strings\": { \"en\": { \"crate_name\": \"Crate\", \"barrel_name\": \"Barrel\" } }\n}\n");
        if (build(folder, &b)) {
            if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
                CHECK(!strstr(u.manifest, "version") && !strstr(u.manifest, "notes"));
                memcpy(hash1, u.hash, sizeof hash1);
                sh_unit_free(&u);
            }
            sh_mb_free(&b);
        }
        mt_write(folder, "mod.json",
                 "{\n  \"name\": \"Crates and barrels\",\n  \"version\": \"2.7\",\n  \"notes\": {\"b\": 2},\n"
                 "  \"strings\": { \"en\": { \"crate_name\": \"Crate\", \"barrel_name\": \"Barrel\" } }\n}\n");
        if (build(folder, &b)) {
            if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
                memcpy(hash2, u.hash, sizeof hash2);
                sh_unit_free(&u);
            }
            sh_mb_free(&b);
        }
        CHECK(hash1[0] && !strcmp(hash1, hash2));
    }
}

/* ------------------------------------------------------------------ 11 */

static char *source_of(void *ctx, const char *path)
{
    const src_ctx *c = (const src_ctx *)ctx;
    char buf[MAX_PATH * 2], *p;
    if (c->swap_path && !_stricmp(path, c->swap_path)) _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s", c->swap_file);
    else _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\\%s", c->folder, path);
    for (p = buf; *p; p++) if (*p == '/') *p = '\\';
    return _strdup(buf);
}

static void test_compose(const char *top)
{
    char folder[MAX_PATH * 2], dest[MAX_PATH * 2], bad[MAX_PATH * 2], other[MAX_PATH * 2], *why = NULL;
    sh_mb_bundle b, out, back;
    sh_unit u;
    const char *manifests[1];
    size_t lens[1], i;
    src_ctx ctx;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\comp_src", top);
    _snprintf_s(dest, sizeof dest, _TRUNCATE, "%s\\comp_dest", top);
    _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\comp_bad", top);
    _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\other.bin", top);
    write_pack(folder);
    if (!build(folder, &b)) return;
    if (!unit(&b, folder, "mods/" SALT "/crate", &u)) { sh_mb_free(&b); return; }
    manifests[0] = u.manifest;
    lens[0] = u.manifest_len;
    ctx.folder = folder;
    ctx.swap_path = NULL;
    ctx.swap_file = NULL;

    memset(&out, 0, sizeof out);
    CHECK(sh_unit_compose(manifests, lens, 1, source_of, &ctx, SALT, "Composed", &g_game, dest, &out, &why));
    if (why) { fprintf(stderr, "compose: %s\n", why); free(why); why = NULL; }
    CHECK(out.units_of_count == 1 && out.units_of && !strcmp(out.units_of[0], u.hash));
    CHECK(out.file_count == u.files);
    for (i = 0; i < b.file_count; i++)
        if (u.keep[i] && !b.files[i].stock && !b.files[i].absent) {
            size_t j;
            int have = 0;
            for (j = 0; j < out.file_count; j++) have |= !_stricmp(out.files[j].path, b.files[i].path);
            if (!have) { fprintf(stderr, "composed build lacks %s\n", b.files[i].path); failures++; }
        }
    memset(&back, 0, sizeof back);
    CHECK(sh_mb_load_folder(dest, &back));
    CHECK(back.units_of_count == 1 && back.units_of && !strcmp(back.units_of[0], u.hash));
    sh_mb_free(&back);
    sh_mb_free(&out);

    /* an existing folder is never written into */
    memset(&out, 0, sizeof out);
    CHECK(!sh_unit_compose(manifests, lens, 1, source_of, &ctx, SALT, "Composed", &g_game, dest, &out, &why) && why);
    free(why);
    why = NULL;
    CHECK(mt_has(dest, "mod.json"));

    /* a source that hands other bytes fails, with a reason, and leaves no folder */
    mt_write(top, "other.bin", "not the crate's image");
    ctx.swap_path = "images/crate_d.bimage";
    ctx.swap_file = other;
    memset(&out, 0, sizeof out);
    CHECK(!sh_unit_compose(manifests, lens, 1, source_of, &ctx, SALT, "Composed", &g_game, bad, &out, &why));
    CHECK(why && why[0]);
    free(why);
    CHECK(!mt_exists(bad));

    /* N32: two units carrying one path with other bytes compose; the first in object order keeps the path, with one
     * log line */
    {
        char clash[MAX_PATH * 2], *second, *at, have[SH_MB_HASH_HEX + 1], want[SH_MB_HASH_HEX + 1];
        const char *two[2];
        size_t two_lens[2], k;
        unsigned long long size = 0;
        int done = 0;
        _snprintf_s(clash, sizeof clash, _TRUNCATE, "%s\\comp_clash", top);
        ctx.swap_path = NULL;
        second = (char *)malloc(u.manifest_len + 1);
        CHECK(second != NULL);
        if (second) {
            memcpy(second, u.manifest, u.manifest_len);
            second[u.manifest_len] = 0;
            /* the image's SHA-256 in the second manifest becomes another file's */
            at = strstr(second, "\"path\":\"images/crate_d.bimage\",\"sha256\":\"");
            CHECK(at != NULL);
            if (at) {
                at += strlen("\"path\":\"images/crate_d.bimage\",\"sha256\":\"");
                memcpy(want, at, SH_MB_HASH_HEX);
                want[SH_MB_HASH_HEX] = 0;
                for (k = 0; k < SH_MB_HASH_HEX; k++) at[k] = 'f';
                two[0] = u.manifest; two[1] = second;
                two_lens[0] = u.manifest_len; two_lens[1] = u.manifest_len;
                memset(&out, 0, sizeof out);
                g_last_log[0] = 0;
                done = sh_unit_compose(two, two_lens, 2, source_of, &ctx, SALT, "Composed", &g_game, clash, &out, &why);
                if (why) { fprintf(stderr, "clash compose: %s\n", why); free(why); why = NULL; }
                CHECK(done);
                CHECK(strstr(g_last_log, "two objects of this mod carry images/crate_d.bimage with other bytes; the first "
                                         "object's file is kept") != NULL);
                {
                    char path[MAX_PATH * 2];
                    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\images\\crate_d.bimage", clash);
                    CHECK(sh_mb_sha256_file(path, &size, have) && !strcmp(have, want));
                }
                if (done) sh_mb_free(&out);
            }
            free(second);
        }
    }
    sh_unit_free(&u);
    sh_mb_free(&b);
}

/* ------------------------------------------------------------------ 12 */

static void test_store(const char *top)
{
    char folder[MAX_PATH * 2], root[MAX_PATH * 2], path[MAX_PATH * 2], *why = NULL;
    sh_mb_bundle b;
    sh_unit u;

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\store_src", top);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\content", top);
    write_pack(folder);
    if (!build(folder, &b)) return;
    if (unit(&b, folder, "mods/" SALT "/crate", &u)) {
        unsigned char *bytes;
        size_t len = 0;
        CHECK(sh_unit_store(&u, root, &why));
        free(why);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\units\\%s.json", root, u.hash);
        bytes = sh_mb_read_file(path, (size_t)-1, &len);
        CHECK(bytes && len == u.manifest_len && !memcmp(bytes, u.manifest, len));
        free(bytes);
        why = NULL;
        CHECK(sh_unit_store(&u, root, &why));
        free(why);
        sh_unit_free(&u);
    }
    sh_mb_free(&b);
}

int main(void)
{
    char top[MAX_PATH];
    if (!mt_temp_dir("modunit", top, sizeof top)) { fprintf(stderr, "no temp folder\n"); return 1; }
    sh_mb_set_script_check(script_ok);
    test_keys();
    test_closure(top);
    test_identity(top);
    test_proof(top);
    test_registry_edits(top);
    test_surface(top);
    test_boards(top);
    test_scripts(top);
    test_asked(top);
    test_mod_share(top);
    test_compose(top);
    test_store(top);
    mt_remove_tree(top);
    if (failures) {
        fprintf(stderr, "modunit_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("modunit_test: all checks passed\n");
    return 0;
}
