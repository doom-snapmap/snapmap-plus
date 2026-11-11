/* modslice_test.c -- the files of a mod a published map needs (modslice.h):
 * the needed set from placed names (the reference walk over text and binary
 * bytes, the Toybox row naming a placed name, a node path naming its animWeb,
 * the enumerated decl types), every whole-build rule (a map-level row,
 * scripts, nothing placed, a file of an unknown kind, nearly every byte
 * needed), the always and never rules (files at game names, editor tweaks),
 * the units (a texture family, the sound banks, the render programs, a
 * module's own files), what players asked for, and the cut: built on its own,
 * proved, named by its files and its source (two maps with one need share a
 * hash), carrying its source build's canonical manifest; a cut whose files do
 * not stand alone is refused and leaves nothing behind.
 *
 *   modslice_test.exe
 */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbundle.h"
#include "modslice.h"
#include "mod_test_util.h"

#define SALT "pk7salt1"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }

/* The game holds every name outside mods/; nothing under it. */
static int game(void *ctx, const char *request)
{
    (void)ctx;
    return strstr(request, "mods/") ? 0 : 1;
}

static const sh_mb_resolver g_game = { NULL, NULL, game, NULL };

/* A binary whose bytes spell `name` between other bytes, as a mesh names its material. */
static void put_binary(const char *folder, const char *rel, const char *name, size_t pad)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    size_t i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) return;
    for (i = 0; i < pad; i++) fputc((int)(i * 7 % 251), f);
    if (name) { fputc(0, f); fwrite(name, 1, strlen(name), f); fputc(0, f); }
    for (i = 0; i < 64; i++) fputc(0, f);
    fclose(f);
}

/* A pack of objects in `folder`, as a Toybox mod lays them out. */
static void write_pack(const char *folder)
{
    mt_write(folder, "mod.json", "{\n  \"name\": \"Crates and barrels\",\n  \"description\": \"Props.\"\n}\n");
    /* The crate: entityDef -> model (a binary naming its material) -> material -> image. */
    mt_write(folder, "assets\\generated\\decls\\entitydef\\mods\\" SALT "\\crate.decl",
             "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
             "\t\t\tmodel = \"mods/" SALT "/models/crate.lwo\";\n\t\t}\n\t}\n}\n");
    put_binary(folder, "assets\\cooked\\model\\mods\\" SALT "\\models\\crate.bmodel", "mods/" SALT "/crate_skin", 200);
    mt_write(folder, "assets\\generated\\decls\\material\\mods\\" SALT "\\crate_skin.decl",
             "{\ndiffusemap\t\"mods/" SALT "/crate_d.tga\"\n}\n");
    put_binary(folder, "assets\\generated\\image\\mods\\" SALT "\\crate_d.bimage", NULL, 300);
    mt_write(folder, "assets\\generated\\decls\\snapeditorentitydef\\mods\\" SALT "\\crate.decl",
             "{\n\tinherit = \"action/door/open\";\n\tedit = {\n\t\tentityDef = \"mods/" SALT "/crate\";\n\t}\n}\n");
    /* The barrel, the same shape and far larger. */
    mt_write(folder, "assets\\generated\\decls\\entitydef\\mods\\" SALT "\\barrel.decl",
             "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
             "\t\t\tmodel = \"mods/" SALT "/models/barrel.lwo\";\n\t\t}\n\t}\n}\n");
    put_binary(folder, "assets\\cooked\\model\\mods\\" SALT "\\models\\barrel.bmodel", "mods/" SALT "/barrel_skin", 9000);
    mt_write(folder, "assets\\generated\\decls\\material\\mods\\" SALT "\\barrel_skin.decl",
             "{\ndiffusemap\t\"mods/" SALT "/barrel_d.tga\"\n}\n");
    put_binary(folder, "assets\\generated\\image\\mods\\" SALT "\\barrel_d.bimage", NULL, 9000);
    mt_write(folder, "assets\\generated\\decls\\snapeditorentitydef\\mods\\" SALT "\\barrel.decl",
             "{\n\tinherit = \"action/door/open\";\n\tedit = {\n\t\tentityDef = \"mods/" SALT "/barrel\";\n\t}\n}\n");
    /* A lever whose decl names a node inside an animWeb. */
    mt_write(folder, "assets\\generated\\decls\\entitydef\\mods\\" SALT "\\lever.decl",
             "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\tanimWebPath = \"mods/" SALT
             "/web/base/idle\";\n\t}\n}\n");
    mt_write(folder, "assets\\generated\\decls\\animweb\\mods\\" SALT "\\web.decl", "{\n}\n");
    /* An AI score the conductor finds by its number, never by a name. */
    mt_write(folder, "assets\\generated\\decls\\snapaiscore\\mods\\" SALT "\\score.decl",
             "{\n\tedit = {\n\t\taiType = \"8388608\";\n\t}\n}\n");
}

static int kept(const sh_mb_bundle *b, const sh_slice_plan *p, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++)
        if (!strcmp(b->files[i].request, request)) return p->keep && p->keep[i];
    fprintf(stderr, "no file %s\n", request);
    return -1;
}

#define REQ_CRATE   "generated/decls/entitydef/mods/" SALT "/crate.decl"
#define REQ_CMODEL  "cooked/model/mods/" SALT "/models/crate.bmodel"
#define REQ_CMAT    "generated/decls/material/mods/" SALT "/crate_skin.decl"
#define REQ_CIMG    "generated/image/mods/" SALT "/crate_d.bimage"
#define REQ_CROW    "generated/decls/snapeditorentitydef/mods/" SALT "/crate.decl"
#define REQ_BARREL  "generated/decls/entitydef/mods/" SALT "/barrel.decl"
#define REQ_BIMG    "generated/image/mods/" SALT "/barrel_d.bimage"
#define REQ_BMODEL  "cooked/model/mods/" SALT "/models/barrel.bmodel"
#define REQ_LEVER   "generated/decls/entitydef/mods/" SALT "/lever.decl"
#define REQ_WEB     "generated/decls/animweb/mods/" SALT "/web.decl"
#define REQ_SCORE   "generated/decls/snapaiscore/mods/" SALT "/score.decl"

static void test_keys(void)
{
    char *k;
    CHECK((k = sh_slice_key("generated/image/mods/" SALT "/a/b-c.tga$mips", 99, SALT)) && !strcmp(k, "mods/" SALT "/a/b_c"));
    free(k);
    CHECK((k = sh_slice_key("x\"MODS/" SALT "/Web/Base/Idle.md6anim\"", 40, SALT)) && !strcmp(k, "mods/" SALT "/web/base/idle"));
    free(k);
    CHECK(!sh_slice_key("mods/" SALT "/", 14, SALT));
    CHECK(!sh_slice_key("mods/othersal/x.decl", 20, SALT));
    CHECK(sh_slice_enumerated_type("snapAIScore") && sh_slice_enumerated_type("snapaiencounter"));
    CHECK(!sh_slice_enumerated_type("entityDef"));
}

static void test_needed_and_cut(const char *top)
{
    char folder[MAX_PATH * 2], cut1[MAX_PATH * 2], cut2[MAX_PATH * 2], cut3[MAX_PATH * 2], bad[MAX_PATH * 2], *why = NULL;
    const char *crate[] = { "mods/" SALT "/crate" }, *lever[] = { "mods/" SALT "/lever" }, *both[2];
    const char *asked[] = { "Generated/Image/Mods/" SALT "/Barrel_D.bimage" };
    sh_mb_bundle src, c1, c2, c3, back;
    sh_slice_plan p;
    unsigned char *text;
    size_t len = 0;
    char sha[65];

    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\pack", top);
    write_pack(folder);
    memset(&src, 0, sizeof src);
    CHECK(sh_mb_build(folder, SALT, &g_game, &src));
    if (src.errors) { fprintf(stderr, "build: %s\n", sh_mb_first_cause(&src)); return; }
    CHECK(sh_mb_write(&src, folder, &why));
    free(why);
    why = NULL;

    /* A map that places the crate: the crate's chain, its Toybox row and the AI score. */
    CHECK(sh_slice_needed(&src, folder, crate, 1, 0, NULL, 0, &p) && !p.whole);
    CHECK(kept(&src, &p, REQ_CRATE) == 1);
    CHECK(kept(&src, &p, REQ_CMODEL) == 1);   /* a recorded need */
    CHECK(kept(&src, &p, REQ_CMAT) == 1);     /* named only in the model's bytes */
    CHECK(kept(&src, &p, REQ_CIMG) == 1);     /* the material's image */
    CHECK(kept(&src, &p, REQ_CROW) == 1);     /* the Toybox row naming the placed entityDef */
    CHECK(kept(&src, &p, REQ_SCORE) == 1);    /* found by number, never by name */
    CHECK(kept(&src, &p, REQ_BARREL) == 0 && kept(&src, &p, REQ_BIMG) == 0 && kept(&src, &p, REQ_BMODEL) == 0);
    CHECK(kept(&src, &p, REQ_LEVER) == 0 && kept(&src, &p, REQ_WEB) == 0);
    CHECK(p.kept_files == 6 && p.kept_bytes < p.bytes / 4);

    /* The cut: built against the game alone, proved, its mod.json naming the source. */
    _snprintf_s(cut1, sizeof cut1, _TRUNCATE, "%s\\cut1", top);
    _snprintf_s(cut2, sizeof cut2, _TRUNCATE, "%s\\cut2", top);
    _snprintf_s(cut3, sizeof cut3, _TRUNCATE, "%s\\cut3", top);
    memset(&c1, 0, sizeof c1);
    CHECK(sh_slice_cut(&src, folder, &p, cut1, &g_game, &c1, &why));
    if (why) { fprintf(stderr, "cut: %s\n", why); free(why); why = NULL; }
    CHECK(!strcmp(c1.slice_of, src.hash) && !strcmp(c1.id, SALT) && strcmp(c1.hash, src.hash));
    CHECK(c1.total_size == p.kept_bytes);
    CHECK(sh_mb_load_folder(cut1, &back) && !strcmp(back.hash, c1.hash) && !strcmp(back.slice_of, src.hash));
    sh_mb_free(&back);
    CHECK(mt_has(cut1, "source.json") && !mt_has(cut1, "assets\\generated\\image\\mods\\" SALT "\\barrel_d.bimage"));
    {
        char path[MAX_PATH * 2];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\source.json", cut1);
        text = sh_mb_read_file(path, (size_t)-1, &len);
        CHECK(text != NULL);
        if (text) { sh_mb_sha256_hex(text, len, sha); CHECK(!strcmp(sha, src.hash)); free(text); }
    }
    /* Two maps that need the same files of one build: one hash. */
    memset(&c2, 0, sizeof c2);
    CHECK(sh_slice_cut(&src, folder, &p, cut2, &g_game, &c2, NULL) && !strcmp(c1.hash, c2.hash));
    sh_slice_plan_free(&p);

    /* A node path inside an animWeb names the animWeb. */
    CHECK(sh_slice_needed(&src, folder, lever, 1, 0, NULL, 0, &p) && !p.whole);
    CHECK(kept(&src, &p, REQ_LEVER) == 1 && kept(&src, &p, REQ_WEB) == 1 && kept(&src, &p, REQ_CRATE) == 0);
    sh_slice_plan_free(&p);

    /* What players of an earlier cut asked for goes with the next one; another set, another hash. */
    CHECK(sh_slice_needed(&src, folder, crate, 1, 0, asked, 1, &p) && !p.whole);
    CHECK(kept(&src, &p, REQ_BIMG) == 1 && kept(&src, &p, REQ_BARREL) == 0);
    memset(&c3, 0, sizeof c3);
    CHECK(sh_slice_cut(&src, folder, &p, cut3, &g_game, &c3, NULL) && strcmp(c3.hash, c1.hash));
    sh_slice_plan_free(&p);

    /* The whole build ships: a map-level row, nothing placed, nearly every byte needed. */
    CHECK(sh_slice_needed(&src, folder, crate, 1, 1, NULL, 0, &p) && p.whole && strstr(p.why, "as a whole"));
    sh_slice_plan_free(&p);
    CHECK(sh_slice_needed(&src, folder, NULL, 0, 0, NULL, 0, &p) && p.whole && strstr(p.why, "places nothing"));
    sh_slice_plan_free(&p);
    both[0] = "mods/" SALT "/crate";
    both[1] = "mods/" SALT "/barrel";
    CHECK(sh_slice_needed(&src, folder, both, 2, 0, NULL, 0, &p) && p.whole && strstr(p.why, "three quarters"));
    sh_slice_plan_free(&p);

    /* A cut whose files do not stand alone is refused, and nothing is left behind. */
    CHECK(sh_slice_needed(&src, folder, crate, 1, 0, NULL, 0, &p) && !p.whole && p.keep);
    {
        size_t i;
        for (i = 0; i < src.file_count; i++) if (!strcmp(src.files[i].request, REQ_CMODEL)) p.keep[i] = 0;
    }
    _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad", top);
    memset(&back, 0, sizeof back);
    CHECK(!sh_slice_cut(&src, folder, &p, bad, &g_game, &back, &why));
    CHECK(why && (strstr(why, "does not build on its own") || strstr(why, "leaves out")));
    CHECK(!mt_exists(bad));
    free(why);
    why = NULL;
    /* An existing folder is never written into. */
    CHECK(!sh_slice_cut(&src, folder, &p, cut1, &g_game, &back, &why) && why);
    free(why);
    sh_slice_plan_free(&p);
    sh_mb_free(&c1);
    sh_mb_free(&c2);
    sh_mb_free(&c3);
    sh_mb_free(&src);
}

/* ------------------------------------------------- the rules on a bundle as loaded ---- */

typedef struct fx { const char *request, *ptype, *bytes; int cls, stock; unsigned long long size; } fx;

/* A bundle of these files (not built: the rules read only what a loaded
 * block gives), their bytes written into `folder`. */
static void fx_bundle(sh_mb_bundle *b, const char *folder, const fx *files, size_t n)
{
    size_t i;
    memset(b, 0, sizeof *b);
    strcpy_s(b->id, sizeof b->id, SALT);
    b->files = (sh_mb_file *)calloc(n, sizeof *b->files);
    b->file_count = b->file_cap = n;
    for (i = 0; i < n; i++) {
        sh_mb_file *f = &b->files[i];
        char rel[MAX_PATH * 2], *s;
        f->request = _strdup(files[i].request);
        f->path = (char *)malloc(strlen(files[i].request) + 8);
        sprintf_s(f->path, strlen(files[i].request) + 8, "assets/%s", files[i].request);
        f->ptype = _strdup(files[i].ptype);
        f->pname = _strdup(files[i].request);
        f->cls = files[i].cls;
        f->stock = files[i].stock;
        f->is_decl = !strncmp(files[i].request, "generated/decls/", 16);
        f->size = files[i].size ? files[i].size : (files[i].bytes ? strlen(files[i].bytes) : 1);
        if (!f->stock) b->total_size += f->size;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s", f->path);
        for (s = rel; *s; s++) if (*s == '/') *s = '\\';
        mt_write(folder, rel, files[i].bytes ? files[i].bytes : "x");
    }
}

static int fx_kept(const sh_mb_bundle *b, const sh_slice_plan *p, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return p->keep ? p->keep[i] : -1;
    return -1;
}

#define BIG 100000ull

static void test_rules(const char *top)
{
    char folder[MAX_PATH * 2];
    const char *thing[] = { "mods/" SALT "/thing" };
    sh_mb_bundle b;
    sh_slice_plan p;

    /* Always: a file at a game name (an edited copy) and what it names; never: an editor tweak. */
    {
        const fx files[] = {
            { "generated/decls/entitydef/mods/" SALT "/thing.decl", "entityDef", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", "entityDef",
              "aiTypeList { item[0] = { entityDef = \"mods/" SALT "/demon\"; } }", SH_MB_CLASS_REPLACE, 0, 0 },
            { "generated/decls/entitydef/mods/" SALT "/demon.decl", "entityDef", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/snapeditorentitydef/monster/imp.decl", "snapEditorEntityDef", "{ }", SH_MB_CLASS_EDITOR, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules1", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        CHECK(sh_slice_needed(&b, folder, thing, 1, 0, NULL, 0, &p) && !p.whole);
        CHECK(fx_kept(&b, &p, files[1].request) == 1 && fx_kept(&b, &p, files[2].request) == 1);
        CHECK(fx_kept(&b, &p, files[3].request) == 0 && fx_kept(&b, &p, files[4].request) == 0);
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
    /* Units: a reached material's texture family whole, every bank once a sound decl is kept,
     * every render program once one is kept. */
    {
        const fx files[] = {
            { "generated/decls/entitydef/mods/" SALT "/thing.decl", "entityDef",
              "{ material = \"mods/" SALT "/lit\"; sound = \"mods/" SALT "/roar\"; }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/material/mods/" SALT "/lit.decl", "material",
              "{ landpagefile mods/" SALT "/vt/board program mods/" SALT "/prog }", SH_MB_CLASS_ADD, 0, 0 },
            { "virtualtextures/mods/" SALT "/vt/board_sq1.mega2", "file", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "virtualtextures/mods/" SALT "/vt/board_sq2.mega2", "file", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/sound/mods/" SALT "/roar.decl", "sound", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "sound/soundbanks/pc/" SALT "_a.bnk", "soundBank", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "sound/soundbanks/pc/" SALT "_b.bnk", "soundBank", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/renderprog/mods/" SALT "/prog.decl", "renderProg", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/renderprogs/mods/" SALT "/prog_pc_vulkan.bin", "renderProg", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        size_t i;
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules2", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        CHECK(sh_slice_needed(&b, folder, thing, 1, 0, NULL, 0, &p) && !p.whole);
        for (i = 0; i + 1 < sizeof files / sizeof *files; i++) CHECK(fx_kept(&b, &p, files[i].request) == 1);
        CHECK(fx_kept(&b, &p, files[9].request) == 0);
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
    /* A module brings every file below its own name. */
    {
        const fx files[] = {
            { "maps/modules/mods/" SALT "/room.decl", "snapModule", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/decls/snapmoduleinfo/mods/" SALT "/room.decl", "snapModuleInfo", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/aas/mods/" SALT "/room/room.baas_monster48", "aas", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        const char *room[] = { "maps/modules/mods/" SALT "/room.decl" };
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules3", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        b.files[0].is_module = 1;
        CHECK(sh_slice_needed(&b, folder, room, 1, 0, NULL, 0, &p) && !p.whole);
        CHECK(fx_kept(&b, &p, files[0].request) == 1 && fx_kept(&b, &p, files[1].request) == 1);
        CHECK(fx_kept(&b, &p, files[2].request) == 1 && fx_kept(&b, &p, files[3].request) == 0);
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
    /* The whole build: a reached file of a kind the walk does not read, a file that cannot
     * be read, and scripts. */
    {
        const fx files[] = {
            { "generated/decls/entitydef/mods/" SALT "/thing.decl", "entityDef", "{ x = \"mods/" SALT "/odd\"; }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/mods/" SALT "/odd.json", "file", NULL, SH_MB_CLASS_ADD, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules4", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        CHECK(sh_slice_needed(&b, folder, thing, 1, 0, NULL, 0, &p) && p.whole && strstr(p.why, "does not read"));
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
    {
        const fx files[] = {
            { "generated/decls/entitydef/mods/" SALT "/thing.decl", "entityDef", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        char gone[MAX_PATH * 2];
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules5", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        _snprintf_s(gone, sizeof gone, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\" SALT "\\thing.decl", folder);
        DeleteFileA(gone);
        CHECK(sh_slice_needed(&b, folder, thing, 1, 0, NULL, 0, &p) && p.whole && strstr(p.why, "could not be read"));
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
    {
        const fx files[] = {
            { "generated/decls/entitydef/mods/" SALT "/thing.decl", "entityDef", "{ }", SH_MB_CLASS_ADD, 0, 0 },
            { "mods/" SALT "/scripts/main.lua", "script", "api = 1\n", SH_MB_CLASS_ADD, 0, 0 },
            { "generated/image/mods/" SALT "/other.bimage", "image", NULL, SH_MB_CLASS_ADD, 0, BIG },
        };
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rules6", top);
        fx_bundle(&b, folder, files, sizeof files / sizeof *files);
        CHECK(sh_slice_needed(&b, folder, thing, 1, 0, NULL, 0, &p) && p.whole && strstr(p.why, "scripts"));
        sh_slice_plan_free(&p);
        sh_mb_free(&b);
    }
}

int main(void)
{
    char top[MAX_PATH];
    if (!mt_temp_dir("modslice", top, sizeof top)) { fprintf(stderr, "no temp folder\n"); return 1; }
    test_keys();
    test_needed_and_cut(top);
    test_rules(top);
    mt_remove_tree(top);
    if (failures) {
        fprintf(stderr, "modslice_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("modslice_test: all checks passed\n");
    return 0;
}
