/* modbundle_test.c -- mod bundle builder: hashes, canonical manifest, logical
 * names, decl needs, validation refusals and build numbering.
 *
 *   modbundle_test.exe <tests\fixtures\mods\alex.r2-imp> [--write-golden]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbundle.h"
#include "mod_test_util.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)
#define CHECK_CAUSE(b, needle) do { CHECK((b).errors > 0); \
    if (!strstr((b).first_cause, needle)) { fprintf(stderr, "FAIL %s:%d: cause \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, (b).first_cause, needle); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }

/* The fixture's bundle hash. A change here is a change of the canonical form
 * or of the fixture, and the store's validator must agree with it. */
#define GOLDEN_HASH "194e95cd9d4f4c18f5e3f01d8447ea4ef7d07299c3d8a539246a7229f832be17"

static int has_need(const sh_mb_file *f, const char *type, const char *name)
{
    size_t i;
    for (i = 0; i < f->need_count; i++)
        if (!strcmp(f->needs[i].type, type) && !strcmp(f->needs[i].name, name)) return 1;
    return 0;
}

static const sh_mb_file *find_file(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

static void test_sha(void)
{
    char hex[65];
    sh_mb_sha256_hex("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sh_mb_sha256_hex("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    {
        char block[200];
        memset(block, 'a', sizeof block);
        sh_mb_sha256_hex(block, 56, hex);   /* padding crosses a block */
        CHECK(!strcmp(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    }
}

static void test_names(void)
{
    char id[SH_MB_ID_MAX + 1], cause[256], s[SH_MB_ID_MAX + 1];
    char long_id[80];
    CHECK(sh_mb_id_normalize("  Alex.R2-Imp \t", 15, id) && !strcmp(id, "alex.r2-imp"));
    CHECK(sh_mb_id_normalize("a", 1, id));
    CHECK(sh_mb_id_normalize("a_b.c-d9", 8, id));
    CHECK(!sh_mb_id_normalize("", 0, id));
    CHECK(!sh_mb_id_normalize(".a", 2, id));
    CHECK(!sh_mb_id_normalize("a.", 2, id));
    CHECK(!sh_mb_id_normalize("-a", 2, id));
    CHECK(!sh_mb_id_normalize("a..b", 4, id));
    CHECK(!sh_mb_id_normalize("a b", 3, id));
    CHECK(!sh_mb_id_normalize("a/b", 3, id));
    CHECK(!sh_mb_id_normalize("a@b", 3, id));
    memset(long_id, 'a', 65); long_id[65] = 0;
    CHECK(!sh_mb_id_normalize(long_id, 65, id));
    CHECK(sh_mb_id_normalize(long_id, 64, id));
    sh_mb_id_sanitized("alex.r2-imp", s);
    CHECK(!strcmp(s, "alex.r2_imp"));

    CHECK(sh_mb_version_valid("1.0.0"));
    CHECK(sh_mb_version_valid("1.0 beta"));
    CHECK(sh_mb_version_valid("v2-rc1+x"));
    CHECK(!sh_mb_version_valid(""));
    CHECK(!sh_mb_version_valid("1.0."));
    CHECK(!sh_mb_version_valid("1/0"));
    CHECK(!sh_mb_version_valid("1:0"));
    CHECK(!sh_mb_version_valid("a*b"));
    CHECK(!sh_mb_version_valid(" 1"));
    CHECK(!sh_mb_version_valid("123456789012345678901234567890123"));

    CHECK(sh_mb_check_request("generated/decls/entitydef/mods/a/b.decl", cause, sizeof cause));
    CHECK(sh_mb_check_request("maps/my room/a.bmodel", cause, sizeof cause));
    CHECK(!sh_mb_check_request("generated/../x.decl", cause, sizeof cause) && strstr(cause, "traversal"));
    CHECK(!sh_mb_check_request("./x.decl", cause, sizeof cause));
    CHECK(!sh_mb_check_request("/x.decl", cause, sizeof cause) && strstr(cause, "absolute"));
    CHECK(!sh_mb_check_request("c:/x.decl", cause, sizeof cause) && strstr(cause, "absolute"));
    CHECK(!sh_mb_check_request("a\\x.decl", cause, sizeof cause) && strstr(cause, "backslash"));
    CHECK(!sh_mb_check_request("a./x.decl", cause, sizeof cause));
    CHECK(!sh_mb_check_request("a /x.decl", cause, sizeof cause));
    CHECK(!sh_mb_check_request(" a/x.decl", cause, sizeof cause));
    CHECK(!sh_mb_check_request("a//x.decl", cause, sizeof cause));
    CHECK(!sh_mb_check_request("a/x\"y.decl", cause, sizeof cause));

    CHECK(sh_mb_extension_allowed("a/b.decl"));
    CHECK(sh_mb_extension_allowed("a/b.bmd6model"));
    CHECK(sh_mb_extension_allowed("a/b.bmd6skl"));
    CHECK(sh_mb_extension_allowed("a/b.bmd6anim"));
    CHECK(sh_mb_extension_allowed("a/b.bimage"));
    CHECK(sh_mb_extension_allowed("a/b.bmodel"));
    CHECK(sh_mb_extension_allowed("a/b.bcm"));
    CHECK(sh_mb_extension_allowed("a/b.baas_monster48"));
    CHECK(sh_mb_extension_allowed("a/b.bnk"));
    CHECK(sh_mb_extension_allowed("a/b.json"));
    CHECK(sh_mb_extension_allowed("a/b.bin"));
    CHECK(sh_mb_extension_allowed("a/b.vspv"));
    CHECK(sh_mb_extension_allowed("a/b.fspv"));
    CHECK(sh_mb_extension_allowed("a/b.mrpr"));
    CHECK(!sh_mb_extension_allowed("a/b.baas_"));
    CHECK(!sh_mb_extension_allowed("a/b.exe"));
    CHECK(!sh_mb_extension_allowed("a/b.txt"));
    CHECK(!sh_mb_extension_allowed("a/b.png"));
    CHECK(!sh_mb_extension_allowed("a.decl/b"));
}

static void ln(const char *req, const char *type, const char *name)
{
    char t[64], n[512];
    int ok = sh_mb_logical_name(req, t, sizeof t, n, sizeof n);
    if (!ok || strcmp(t, type) || strcmp(n, name)) {
        fprintf(stderr, "FAIL logical name %s -> %s %s (got %d %s %s)\n", req, type, name, ok, ok ? t : "", ok ? n : "");
        failures++;
    }
}

/* Every row of the Contract 1 table. */
static void test_logical_names(void)
{
    char t[64], n[512];
    ln("generated/decls/md6def/mods/alex.pink-imp/imp.md6.decl", "md6Def", "mods/alex.pink-imp/imp.md6");
    ln("generated/decls/entitydef/mods/a/b.decl", "entityDef", "mods/a/b");
    ln("generated/decls/snapeditorentitydef/mods/a/b.decl", "snapEditorEntityDef", "mods/a/b");
    ln("generated/decls/material/mods/a/b.decl", "material", "mods/a/b");
    ln("generated/decls/renderparm/mods/a/b.decl", "renderParm", "mods/a/b");
    CHECK(!sh_mb_logical_name("generated/decls/nosuchtype/a.decl", t, sizeof t, n, sizeof n));
    CHECK(!sh_mb_logical_name("generated/decls/entitydef/.decl", t, sizeof t, n, sizeof n));
    ln("generated/basemodel/md6/mods/a/assets/mesh/imp.bmd6model", "baseModel", "md6/mods/a/assets/mesh/imp.md6mesh");
    ln("generated/skeleton/md6/mods/a/assets/mesh/imp.bmd6skl", "skeleton", "md6/mods/a/assets/mesh/imp.md6skl");
    ln("cooked/anim/md6/mods/a/motion/idle.bmd6anim", "anim", "md6/mods/a/motion/idle.md6anim");
    ln("generated/image/mods/a/icon.bimage", "image", "mods/a/icon");
    ln("maps/modules/mods/a/x.bmodel", "model", "maps/modules/mods/a/x.bmodel");
    ln("cooked/model/models/mods/a/x.bmodel", "model", "cooked/model/models/mods/a/x.bmodel");
    ln("generated/renderprogs/permutations/mods/a/x_1.decl", "renderProg", "generated/renderprogs/permutations/mods/a/x_1.decl");
    ln("generated/spirv/mods/a/001.vspv", "renderProg", "generated/spirv/mods/a/001.vspv");
    ln("generated/cm/models/mods/a/x.bcm", "cm", "generated/cm/models/mods/a/x.bcm");
    ln("generated/maps/mods/a/x.baas_monster48", "aas", "generated/maps/mods/a/x.baas_monster48");
    ln("generated/anim/md6/mods/a/x.bmd6anim", "anim", "generated/anim/md6/mods/a/x.bmd6anim");
    ln("sound/soundbanks/mods/a/x.bnk", "file", "sound/soundbanks/mods/a/x.bnk");
    ln("maps/mods/a/info.json", "file", "maps/mods/a/info.json");
    CHECK(!strcmp(sh_mb_decl_type("SnapEditorEntityDef", 19), "snapEditorEntityDef"));
    CHECK(sh_mb_decl_type("material2", 9) == NULL);   /* 13954591 ships "material" decls */
}

static void nr(const char *type, const char *name, int count, const char *first, const char *second)
{
    char out[2][SH_MB_REQUEST_MAX + 1];
    int n = sh_mb_need_requests(type, name, out, 2);
    if (n != count || (n > 0 && strcmp(out[0], first)) || (n > 1 && strcmp(out[1], second))) {
        fprintf(stderr, "FAIL need requests %s %s (got %d %s %s)\n", type, name, n, n > 0 ? out[0] : "", n > 1 ? out[1] : "");
        failures++;
    }
}

static void test_need_requests(void)
{
    nr("baseModel", "md6/mods/alex.r2-imp/a/assets/mesh/a.md6mesh", 1,
       "generated/basemodel/md6/mods/alex.r2_imp/a/assets/mesh/a.bmd6model", NULL);
    nr("skeleton", "md6/x/Y.md6skl", 1, "generated/skeleton/md6/x/y.bmd6skl", NULL);
    nr("anim", "md6/x/a.md6skl/_default.md6anim", 2, "cooked/anim/md6/x/a.md6skl/_default.bmd6anim",
       "generated/anim/md6/x/a.md6skl/_default.bmd6anim");
    nr("image", "models/monsters/imp/imp.tga", 1, "generated/image/models/monsters/imp/imp.bimage", NULL);
    nr("image", "env/black_px", 1, "generated/image/env/black_px.bimage", NULL);
    nr("model", "models/a/b.lwo", 1, "cooked/model/models/a/b.bmodel", NULL);
    nr("model", "maps/modules/x/y.bmodel", 1, "maps/modules/x/y.bmodel", NULL);
    nr("md6Def", "mods/alex.r2-imp/imp_copy.md6", 1, "generated/decls/md6def/mods/alex.r2-imp/imp_copy.md6.decl", NULL);
    nr("renderProg", "skinvmtr", 1, "generated/decls/renderprog/skinvmtr.decl", NULL);
    nr("nosuchtype", "x", 0, NULL, NULL);
}

static void collect(void *ctx, const char *type, const char *name, size_t n)
{
    char *out = (char *)ctx;
    size_t used = strlen(out);
    _snprintf_s(out + used, 4096 - used, _TRUNCATE, "%s=%.*s;", type, (int)n, name);
}

static void test_scan(void)
{
    char got[4096];
    const char *ent = "{\r\n\tinherit = \"mods/a/parent\";\r\n\tedit = {\r\n\t\tinherit = \"mods/a/not_top\";\r\n"
                      "\t\trenderModelInfo = {\r\n\t\t\tmodel = \"mods/a/x.md6\";\r\n\t\t}\r\n"
                      "\t\tclipModelName = \"ignored.lwo\";\r\n\t\tmodel2 = \"x\";\r\n\t\t// model = \"commented.md6\";\r\n"
                      "\t\t/* entityDef = \"commented\"; */\r\n\t\tentityDef = \"mods/a/other\";\r\n\t}\r\n}";
    const char *mat = "{\r\ndiffusemap\t\"mods/a/d.tga\"\r\nbumpmap\t\"mods/a/n.tga\"\r\nminlodmap\t_black\r\n"
                      "ambientprogram\tskinvmtr\r\nvirtualmapping\t{ 0.1, 0.2 }\r\nbloommask\t\"mods/a/m.tga\"\r\n}";
    got[0] = 0;
    sh_mb_scan_needs("entityDef", (const unsigned char *)ent, strlen(ent), collect, got);
    CHECK(!strcmp(got, "entityDef=mods/a/parent;md6Def=mods/a/x.md6;entityDef=mods/a/other;"));
    got[0] = 0;
    /* A stock parent is not a need. */
    sh_mb_scan_needs("entityDef", (const unsigned char *)"{ inherit = \"ai/demon/imp\"; }", 29, collect, got);
    CHECK(got[0] == 0);
    got[0] = 0;
    sh_mb_scan_needs("material", (const unsigned char *)mat, strlen(mat), collect, got);
    CHECK(!strcmp(got, "image=mods/a/d.tga;image=mods/a/n.tga;renderProg=skinvmtr;image=mods/a/m.tga;"));
    if (strcmp(got, "image=mods/a/d.tga;image=mods/a/n.tga;renderProg=skinvmtr;image=mods/a/m.tga;"))
        fprintf(stderr, "  material scan gave %s\n", got);
}

/* The stock 13954591 names the fixture leans on, plus one shadowable decl. */
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
static unsigned g_stock_misses;
static int stock_exists(void *ctx, const char *request)
{
    int i;
    (void)ctx;
    for (i = 0; STOCK[i]; i++) if (!strcmp(STOCK[i], request)) return 1;
    g_stock_misses++;
    return 0;
}
static const sh_mb_resolver RESOLVER = { NULL, NULL, stock_exists, NULL };

static void test_fixture(const char *fixture, const char *tmp, int write_golden)
{
    sh_mb_bundle b, again;
    char folder[MAX_PATH], path[MAX_PATH], cause[512];
    const sh_mb_file *f;
    size_t i, len = 0;
    unsigned char *text;

    CHECK(sh_mb_build(fixture, &RESOLVER, &b));
    if (b.errors) fprintf(stderr, "  fixture refused: %s\n", b.first_cause);
    if (b.warnings) fprintf(stderr, "  fixture warning: %s\n", b.first_warning);
    CHECK(b.warnings == 0);
    CHECK(!strcmp(b.id, "alex.r2-imp") && !strcmp(b.version, "1.0.0") && b.build == 1);
    CHECK(!strcmp(b.descriptor, "mod.json"));
    CHECK(b.file_count == 13);
    CHECK(b.needs.unresolved == 0 && b.needs.stock == 8 && b.needs.local == b.needs.total - 8);
    if (b.needs.stock != 8) fprintf(stderr, "  stock needs %u of %u\n", b.needs.stock, b.needs.total);
    /* Hashes and sizes against an independent file hash. */
    for (i = 0; i < b.file_count; i++) {
        char hex[65];
        unsigned long long size = 0;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", fixture, b.files[i].path);
        CHECK(sh_mb_sha256_file(path, &size, hex));
        CHECK(!strcmp(hex, b.files[i].blob) && size == b.files[i].size);
        if (i) CHECK(strcmp(b.files[i - 1].path, b.files[i].path) < 0);
    }
    /* md6Def needs: variant A names its new mesh and the stock anims. */
    f = find_file(&b, "generated/decls/md6def/mods/alex.r2-imp/imp_copy.md6.decl");
    CHECK(f && !strcmp(f->ptype, "md6Def") && !strcmp(f->pname, "mods/alex.r2-imp/imp_copy.md6"));
    CHECK(f && f->need_count == 5);
    CHECK(f && has_need(f, "baseModel", "md6/mods/alex.r2-imp/imp_copy/assets/mesh/imp_copy.md6mesh"));
    CHECK(f && has_need(f, "anim", "md6/characters/monsters/imp/base/motion/combat/idle.md6anim"));
    CHECK(f && has_need(f, "anim", "md6/characters/monsters/imp/base/motion/combat/rage_taunt.md6anim"));
    f = find_file(&b, "generated/decls/md6def/mods/alex.r2-imp/imp_copy_b.md6.decl");
    CHECK(f && f->need_count == 5 && has_need(f, "anim", "md6/mods/alex.r2-imp/imp_copy/motion/meleeforward02.md6anim"));
    CHECK(f && has_need(f, "baseModel", "md6/mods/alex.r2-imp/imp_copy_b/assets/mesh/imp_copy_b.md6mesh"));
    /* sedef needs. */
    f = find_file(&b, "generated/decls/snapeditorentitydef/mods/alex.r2-imp/imp_copy.decl");
    CHECK(f && !strcmp(f->ptype, "snapEditorEntityDef"));
    CHECK(f && f->need_count == 4);
    CHECK(f && !has_need(f, "snapEditorEntityDef", "demons/demon_base"));   /* stock parent */
    CHECK(f && has_need(f, "entityDef", "mods/alex.r2-imp/imp_copy"));
    CHECK(f && has_need(f, "md6Def", "mods/alex.r2-imp/imp_copy.md6"));
    CHECK(f && has_need(f, "material", "textures/guis/snapmaps/entity_icons/demons/imp_enc"));
    CHECK(f && has_need(f, "entityDef", "ai/demon/imp_coop"));
    f = find_file(&b, "generated/decls/snapeditorentitydef/mods/alex.r2-imp/imp_copy_b.decl");
    CHECK(f && has_need(f, "material", "mods/alex.r2-imp/imp_icon") && has_need(f, "md6Def", "mods/alex.r2-imp/imp_copy_b.md6"));
    f = find_file(&b, "generated/decls/entitydef/mods/alex.r2-imp/imp_copy.decl");
    CHECK(f && f->need_count == 1 && has_need(f, "md6Def", "mods/alex.r2-imp/imp_copy.md6"));
    f = find_file(&b, "generated/decls/material/mods/alex.r2-imp/imp_icon.decl");
    CHECK(f && f->need_count == 2 && has_need(f, "image", "mods/alex.r2-imp/imp_icon.tga") && has_need(f, "renderProg", "skinvmtr"));
    f = find_file(&b, "generated/basemodel/md6/mods/alex.r2_imp/imp_copy/assets/mesh/imp_copy.bmd6model");
    CHECK(f && !strcmp(f->ptype, "baseModel") && f->need_count == 0);
    /* The legacy strings key travels under extra. */
    CHECK(b.extra && !strcmp(b.extra, "{\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}}}"));
    CHECK(strstr(b.canonical, "\"author\":\"alex\",\"build\":1,\"extra\":{") == b.canonical + 1);
    CHECK(!strchr(b.canonical, '\n') && !strstr(b.canonical, "\": ") && !strstr(b.canonical, ", "));
    {
        char hex[65];
        sh_mb_sha256_hex(b.canonical, b.canonical_len, hex);
        CHECK(!strcmp(hex, b.hash));
    }

    /* Golden canonical text. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s.canonical.json", fixture);
    if (write_golden) {
        FILE *g = NULL;
        fopen_s(&g, path, "wb");
        if (g) { fwrite(b.canonical, 1, b.canonical_len, g); fclose(g); }
        printf("golden written: %s (hash %s)\n", path, b.hash);
    } else {
        text = sh_mb_read_file(path, 1 << 20, &len);
        CHECK(text && len == b.canonical_len && !memcmp(text, b.canonical, len));
        free(text);
        CHECK(!strcmp(b.hash, GOLDEN_HASH));
        if (strcmp(b.hash, GOLDEN_HASH)) fprintf(stderr, "  fixture hash %s\n", b.hash);
    }

    /* Two builds give one hash. */
    CHECK(sh_mb_build(fixture, &RESOLVER, &again));
    CHECK(!strcmp(again.hash, b.hash) && again.canonical_len == b.canonical_len);
    sh_mb_free(&again);

    /* Descriptor key order and whitespace do not change the hash. */
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\reordered", tmp);
    CHECK(mt_copy_tree(fixture, folder));
    mt_write(folder, "mod.json", "{\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}},"
                                 "\"author\":\"alex\",  \"name\":\"R2 Imp copy\",\"version\":\"1.0.0\",\"id\":\"ALEX.R2-IMP\"}");
    CHECK(sh_mb_build(folder, &RESOLVER, &again));
    CHECK(!strcmp(again.hash, b.hash));
    sh_mb_free(&again);
    /* The descriptor may be named package.json (first-generation authors). */
    mt_delete(folder, "mod.json");
    mt_write(folder, "package.json", "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\",\"author\":\"alex\","
                                     "\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}}}");
    CHECK(sh_mb_build(folder, &RESOLVER, &again));
    CHECK(!strcmp(again.hash, b.hash) && !strcmp(again.descriptor, "package.json"));
    sh_mb_free(&again);

    /* Write, then read back: the manifest and the canonical bytes. */
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", folder);
    CHECK(sh_mb_load(path, &again));
    if (again.errors) fprintf(stderr, "  reload: %s\n", again.first_cause);
    CHECK(!strcmp(again.hash, b.hash) && again.file_count == b.file_count && again.build == 1);
    CHECK(again.canonical_len == b.canonical_len && !memcmp(again.canonical, b.canonical, b.canonical_len));
    sh_mb_free(&again);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.canonical.json", folder);
    text = sh_mb_read_file(path, 1 << 20, &len);
    CHECK(text && len == b.canonical_len && !memcmp(text, b.canonical, len));
    free(text);

    /* A tampered manifest is refused. */
    {
        size_t blen = 0;
        char *pretty = sh_mb_bundle_json(&b, &blen), *at;
        CHECK(pretty != NULL);
        if (pretty) {
            at = strstr(pretty, "\"size\": 3001");
            CHECK(at != NULL);
            if (at) at[9] = '2';
            CHECK(!sh_mb_load_text(pretty, blen, &again));
            CHECK_CAUSE(again, "does not match its bundle hash");
            sh_mb_free(&again);
            at = strstr(pretty, "\"path\": \"assets/");
            if (at) memcpy(at + 9, "assets/../", 10);
            CHECK(!sh_mb_load_text(pretty, blen, &again));
            CHECK(again.errors > 0);
            sh_mb_free(&again);
            free(pretty);
        }
    }
    sh_mb_free(&b);
}

/* Build numbers: unchanged content keeps the number, changed content takes the next one. */
static void test_build_numbers(const char *fixture, const char *tmp)
{
    sh_mb_bundle b;
    char folder[MAX_PATH], cause[512], hash1[65];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\numbered", tmp);
    CHECK(mt_copy_tree(fixture, folder));
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 1);
    strcpy_s(hash1, sizeof hash1, b.hash);
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    sh_mb_free(&b);
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 1 && !strcmp(b.hash, hash1));
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    sh_mb_free(&b);
    mt_append(folder, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// edited\r\n");
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 2 && strcmp(b.hash, hash1));
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    sh_mb_free(&b);
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 2);
    sh_mb_free(&b);
    /* A version change is content too. */
    mt_write(folder, "mod.json", "{\"id\":\"alex.r2-imp\",\"name\":\"R2 Imp copy\",\"author\":\"alex\","
                                 "\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}}}");
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 3 && !strcmp(b.version, "0.0.0"));
    sh_mb_free(&b);
}

/* Apply one change to a fresh copy and expect a refusal naming `needle`. */
typedef void (*mutate_fn)(const char *folder);
static int g_case;
static void refused(const char *fixture, const char *tmp, mutate_fn mutate, const sh_mb_resolver *r, const char *needle)
{
    sh_mb_bundle b;
    char folder[MAX_PATH], cause[256];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\case%d", tmp, ++g_case);
    CHECK(mt_copy_tree(fixture, folder));
    mutate(folder);
    CHECK(!sh_mb_build(folder, r, &b));
    CHECK_CAUSE(b, needle);
    CHECK(!sh_mb_write(&b, folder, cause, sizeof cause));
    sh_mb_free(&b);
}

static void accepted(const char *fixture, const char *tmp, mutate_fn mutate, sh_mb_bundle *out)
{
    char folder[MAX_PATH];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\case%d", tmp, ++g_case);
    CHECK(mt_copy_tree(fixture, folder));
    mutate(folder);
    CHECK(sh_mb_build(folder, &RESOLVER, out));
    if (out->errors) fprintf(stderr, "  case %d refused: %s\n", g_case, out->first_cause);
}

#define DESC(extra) "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\"" extra "}"
static void m_bad_id(const char *f) { mt_write(f, "mod.json", "{\"id\":\"Alex R2!\",\"name\":\"x\"}"); }
static void m_no_id(const char *f) { mt_write(f, "mod.json", "{\"name\":\"x\"}"); }
static void m_no_name(const char *f) { mt_write(f, "mod.json", "{\"id\":\"alex.r2-imp\"}"); }
static void m_bad_json(const char *f) { mt_write(f, "mod.json", "{\"id\":\"alex.r2-imp\",\"id\":\"x\",\"name\":\"x\"}"); }
static void m_bad_version(const char *f) { mt_write(f, "mod.json", "{\"id\":\"alex.r2-imp\",\"version\":\"1/2\",\"name\":\"x\"}"); }
static void m_reserved(const char *f) { mt_write(f, "mod.json", DESC(",\"files\":[]")); }
static void m_float(const char *f) { mt_write(f, "mod.json", DESC(",\"hud\":{\"scale\":1.5}")); }
static void m_bad_gate(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"g_god\":\"1\"}}")); }
static void m_bad_home(const char *f) { mt_write(f, "mod.json", DESC(",\"homepage\":\"ftp://x\"")); }
static void m_both(const char *f) { mt_write(f, "package.json", DESC("")); }
static void m_no_desc(const char *f) { mt_delete(f, "mod.json"); }
static void m_bad_ext(const char *f) { mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\notes.txt", "x"); }
static void m_outside(const char *f) { mt_write(f, "readme.md", "x"); }
static void m_empty(const char *f) { mt_write(f, "assets\\generated\\image\\mods\\alex.r2-imp\\empty.bimage", ""); }
static void m_unns_decl(const char *f) { mt_write(f, "assets\\generated\\decls\\entitydef\\other\\thing.decl", "{\r\n}"); }
static void m_unns_raw(const char *f) { mt_write(f, "assets\\generated\\image\\other\\x.bimage", "bytes"); }
static void m_bad_type(const char *f) { mt_write(f, "assets\\generated\\decls\\nosuchtype\\mods\\alex.r2-imp\\x.decl", "{\r\n}"); }
static void m_ill_formed(const char *f) { mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\broken.decl", "{\r\n\tinherit = \"x;\r\n"); }
static void m_missing_need(const char *f)
{
    mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\needy.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/nothing_here\";\r\n}");
}
static void m_shadow_ok(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"]"));
    mt_write(f, "assets\\generated\\decls\\entitydef\\monster\\imp.decl", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n}");
}
static void m_shadow_not_stock(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"shadows\":[\"generated/decls/entitydef/monster/not_stock.decl\"]"));
    mt_write(f, "assets\\generated\\decls\\entitydef\\monster\\not_stock.decl", "{\r\n}");
}
static void m_shadow_not_shipped(const char *f) { mt_write(f, "mod.json", DESC(",\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"]")); }
static void m_shadow_traversal(const char *f) { mt_write(f, "mod.json", DESC(",\"shadows\":[\"generated/../decls/x.decl\"]")); }
static void m_requirements(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0,\"g_useImageBlackList\":\"0\"}},"
                                 "\"homepage\":\"https://example.org/imp\",\"composes\":{\"hands\":true}"));
}
static void m_nothing(const char *f) { (void)f; }
/* Two levels of mod inheritance: child -> mid -> imp_copy. */
static void m_chain(const char *f)
{
    mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\mid.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\tedit = {\r\n\t\thealth = 100;\r\n\t}\r\n}");
    mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\child.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/mid\";\r\n\tedit = {\r\n\t}\r\n}");
}
static void m_chain_broken(const char *f)
{
    mt_write(f, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\child.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/mid\";\r\n}");
}

static void test_refusals(const char *fixture, const char *tmp)
{
    sh_mb_bundle b;
    sh_mb_resolver no_stock = { NULL, NULL, NULL, NULL };
    refused(fixture, tmp, m_bad_id, &RESOLVER, "id must be");
    refused(fixture, tmp, m_no_id, &RESOLVER, "missing its required id");
    refused(fixture, tmp, m_no_name, &RESOLVER, "missing its required name");
    refused(fixture, tmp, m_bad_json, &RESOLVER, "invalid JSON");
    refused(fixture, tmp, m_bad_version, &RESOLVER, "version must be");
    refused(fixture, tmp, m_reserved, &RESOLVER, "may not contain files or bundle");
    refused(fixture, tmp, m_float, &RESOLVER, "not a plain integer");
    refused(fixture, tmp, m_bad_gate, &RESOLVER, "not a supported gate");
    refused(fixture, tmp, m_bad_home, &RESOLVER, "homepage");
    refused(fixture, tmp, m_both, &RESOLVER, "both mod.json and package.json");
    refused(fixture, tmp, m_no_desc, &RESOLVER, "no mod.json or package.json");
    refused(fixture, tmp, m_bad_ext, &RESOLVER, "file type not allowed");
    refused(fixture, tmp, m_outside, &RESOLVER, "only the descriptor and assets/");
    refused(fixture, tmp, m_empty, &RESOLVER, "empty file");
    refused(fixture, tmp, m_unns_decl, &RESOLVER, "outside mods/alex.r2-imp/");
    refused(fixture, tmp, m_unns_raw, &RESOLVER, "does not contain /mods/alex.r2-imp/");
    refused(fixture, tmp, m_bad_type, &RESOLVER, "names no decl type");
    refused(fixture, tmp, m_ill_formed, &RESOLVER, "not a well-formed decl");
    refused(fixture, tmp, m_missing_need, &RESOLVER, "resolves to nothing");
    refused(fixture, tmp, m_shadow_not_stock, &RESOLVER, "not a stock resource");
    refused(fixture, tmp, m_shadow_not_shipped, &RESOLVER, "is not in the bundle");
    refused(fixture, tmp, m_shadow_traversal, &RESOLVER, "traversal");
    refused(fixture, tmp, m_nothing, &no_stock, "cannot be checked against the game");

    refused(fixture, tmp, m_chain_broken, &RESOLVER, "needs entityDef mods/alex.r2-imp/mid, which resolves to nothing");
    accepted(fixture, tmp, m_chain, &b);
    {
        const sh_mb_file *child = find_file(&b, "generated/decls/entitydef/mods/alex.r2-imp/child.decl");
        const sh_mb_file *mid = find_file(&b, "generated/decls/entitydef/mods/alex.r2-imp/mid.decl");
        CHECK(child && child->need_count == 1 && has_need(child, "entityDef", "mods/alex.r2-imp/mid"));
        CHECK(mid && mid->need_count == 1 && has_need(mid, "entityDef", "mods/alex.r2-imp/imp_copy"));
        CHECK(strstr(b.canonical, "{\"blob\":\"") && strstr(b.canonical, "\"needs\":[{\"name\":\"mods/alex.r2-imp/mid\",\"type\":\"entityDef\"}]"));
    }
    sh_mb_free(&b);

    /* A declared shadow of a stock decl is accepted. */
    accepted(fixture, tmp, m_shadow_ok, &b);
    CHECK(b.errors == 0 && b.shadow_count == 1 && find_file(&b, "generated/decls/entitydef/monster/imp.decl")->shadowed);
    CHECK(strstr(b.canonical, ",\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"],") != NULL);
    sh_mb_free(&b);
    /* Supported gates normalise to "0"; unknown keys travel under extra. */
    accepted(fixture, tmp, m_requirements, &b);
    CHECK(b.requirements && !strcmp(b.requirements, "{\"cvars\":{\"g_useImageBlackList\":\"0\",\"g_useResourceBlackList\":\"0\"}}"));
    CHECK(b.extra && strstr(b.extra, "\"composes\":{\"hands\":true}") && b.homepage);
    sh_mb_free(&b);

    /* A symbolic link inside assets/ (needs symlink rights; skipped without them). */
    {
        char folder[MAX_PATH], target[MAX_PATH], link[MAX_PATH];
        wchar_t wl[MAX_PATH], wt[MAX_PATH];
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\case%d", tmp, ++g_case);
        CHECK(mt_copy_tree(fixture, folder));
        _snprintf_s(target, sizeof target, _TRUNCATE, "%s\\mod.json", folder);
        _snprintf_s(link, sizeof link, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\link.decl", folder);
        MultiByteToWideChar(CP_UTF8, 0, link, -1, wl, MAX_PATH);
        MultiByteToWideChar(CP_UTF8, 0, target, -1, wt, MAX_PATH);
        if (CreateSymbolicLinkW(wl, wt, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            CHECK(!sh_mb_build(folder, &RESOLVER, &b));
            CHECK_CAUSE(b, "symbolic link");
            sh_mb_free(&b);
        } else {
            printf("modbundle_test: symbolic link case skipped (no symlink rights, error %lu)\n", GetLastError());
        }
    }
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH];
    const char *fixture = argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp";
    int write_golden = argc > 2 && !strcmp(argv[2], "--write-golden");
    char full[MAX_PATH];
    if (!GetFullPathNameA(fixture, sizeof full, full, NULL)) return 2;
    if (!mt_temp_dir("modbundle", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    test_sha();
    test_names();
    test_logical_names();
    test_need_requests();
    test_scan();
    test_fixture(full, tmp, write_golden);
    test_build_numbers(full, tmp);
    test_refusals(full, tmp);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modbundle_test: %d failure(s)\n", failures); return 1; }
    printf("modbundle_test: hashes, canonical form, logical names, needs, %d refusal/acceptance cases and build numbers pass\n", g_case);
    return 0;
}
