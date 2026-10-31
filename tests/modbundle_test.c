/* modbundle_test.c -- mod bundle builder: hashes, canonical manifest, logical
 * names, decl needs, validation refusals, render-program checks, build
 * numbering, mod.json (the author block kept byte for byte, the product
 * block), the tree hash of the rebuild rule and the product record.
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

/* The fixture with only the two blacklist gates as requirements, as the
 * builder hashed it while those gates were the only requirements it accepted
 * (main at 1ac4c6e). The general requirements keep that text and hash, so no
 * installed mod of that shape is rebuilt or re-hashed. */
#define GATES_GOLDEN_HASH "372311a1d3f6dbeff4e6258bff02534dff5c362140961cbe1ac7aa6a37d6575b"
#define GATES_CANONICAL   "{\"cvars\":{\"g_useImageBlackList\":\"0\",\"g_useResourceBlackList\":\"0\"}}"

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
    CHECK(sh_mb_extension_allowed("a/b.cspv"));
    CHECK(sh_mb_extension_allowed("generated/discreteanimation/models/mapobjects/hell/smm_pillar/smm_pillar_breakable.dmodel"));
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
    "generated/decls/md6def/monster/imp.md6.decl",
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
    size_t i, len = 0, blen_unused = 0;
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

    /* Write, then read back: package.json becomes mod.json, its bytes the
     * author block, and the canonical text derived from mod.json is the
     * golden one: what the Worker receives does not change. */
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    CHECK(!mt_has(folder, "package.json") && mt_has(folder, "mod.json"));
    CHECK(!mt_has(folder, "bundle.json") && !mt_has(folder, "bundle.canonical.json") && !mt_has(folder, ".state.json"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
    CHECK(sh_mb_load(path, &again));
    if (again.errors) fprintf(stderr, "  reload: %s\n", again.first_cause);
    CHECK(!strcmp(again.hash, b.hash) && again.file_count == b.file_count && again.build == 1 && again.validated);
    CHECK(again.canonical_len == b.canonical_len && !memcmp(again.canonical, b.canonical, b.canonical_len));
    if (!write_golden) {
        /* The text the Worker receives for this unchanged mod, derived from
         * mod.json, is the golden text of the bundle.json era, byte for byte. */
        char golden_path[MAX_PATH];
        size_t glen = 0;
        unsigned char *golden;
        _snprintf_s(golden_path, sizeof golden_path, _TRUNCATE, "%s.canonical.json", fixture);
        golden = sh_mb_read_file(golden_path, 1 << 20, &glen);
        CHECK(golden && glen == again.canonical_len && !memcmp(golden, again.canonical, glen));
        CHECK(!strcmp(again.hash, GOLDEN_HASH));
        free(golden);
    }
    sh_mb_free(&again);
    text = sh_mb_read_file(path, 1 << 20, &len);
    {
        char *own = text ? sh_mb_author_bytes((const char *)text, len, &blen_unused, NULL) : NULL;
        static const char pkg[] = "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\",\"author\":\"alex\","
                                  "\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}}}";
        CHECK(own && blen_unused == sizeof pkg - 1 && !memcmp(own, pkg, sizeof pkg - 1));
        CHECK(text && strstr((const char *)text, "\"snapmap-plus\": {") && strstr((const char *)text, "\"sha256\": \""));
        free(own);
    }
    free(text);

    /* A tampered product block is refused. */
    {
        size_t mlen = 0;
        unsigned char *mod = sh_mb_read_file(path, 1 << 20, &mlen);
        char *at = mod ? strstr((char *)mod, "\"size\": 3001") : NULL;
        CHECK(at != NULL);
        if (at) {
            at[9] = '2';
            CHECK(!sh_mb_load_text((const char *)mod, mlen, &again));
            CHECK_CAUSE(again, "changed since it was built");
            sh_mb_free(&again);
        }
        free(mod);
    }

    /* The bundle.json form (a rawmap ZIP, a download) loads back to the same
     * canonical text; a tampered one is refused. */
    {
        size_t blen = 0;
        char *pretty = sh_mb_bundle_json(&b, &blen), *at;
        CHECK(pretty != NULL);
        if (pretty) {
            CHECK(sh_mb_load_manifest_text(pretty, blen, &again));
            CHECK(again.canonical_len == b.canonical_len && !memcmp(again.canonical, b.canonical, b.canonical_len));
            sh_mb_free(&again);
            at = strstr(pretty, "\"size\": 3001");
            CHECK(at != NULL);
            if (at) at[9] = '2';
            CHECK(!sh_mb_load_manifest_text(pretty, blen, &again));
            CHECK_CAUSE(again, "does not match its bundle hash");
            sh_mb_free(&again);
            at = strstr(pretty, "\"path\": \"assets/");
            if (at) memcpy(at + 9, "assets/../", 10);
            CHECK(!sh_mb_load_manifest_text(pretty, blen, &again));
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
    /* A version change is content too: the author edits mod.json, which
     * keeps its product block (the counter's memory). */
    {
        char path[MAX_PATH];
        size_t len = 0;
        char *text, *at;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
        text = (char *)sh_mb_read_file(path, 1 << 20, &len);
        at = text ? strstr(text, "\"version\": \"1.0.0\",") : NULL;
        CHECK(at != NULL);
        if (at) {
            memmove(at, at + 19, strlen(at + 19) + 1);
            mt_write(folder, "mod.json", text);
        }
        free(text);
    }
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 3 && !strcmp(b.version, "0.0.0"));
    if (b.build != 3) fprintf(stderr, "  build %llu errors %u: %s\n", b.build, b.errors, b.first_cause);
    sh_mb_free(&b);
    /* A mod.json replaced without its product block starts counting again. */
    mt_write(folder, "mod.json", "{\"id\":\"alex.r2-imp\",\"name\":\"R2 Imp copy\",\"author\":\"alex\"}");
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 1);
    sh_mb_free(&b);
}

/* mod.json: the author block is kept byte for byte (any formatting, unknown
 * keys, CRLF), a rebuild of unchanged content rewrites nothing, and a
 * download's manifest gets an author block that reproduces its hash. */
static void test_modjson(const char *fixture, const char *tmp)
{
    static const char author[] =
        "\xef\xbb\xbf{\r\n\t\"name\" :  \"R2 Imp copy\",\r\n\t\"id\":\"alex.r2-imp\", \"version\": \"1.0.0\",\r\n"
        "\t\"author\": \"alex\",\r\n\t\"strings\": {\"english\": {\"#str_mod_alex_r2_imp\": \"Imp copy\"}},\r\n"
        "\t\"x-notes\": [1, 2, {\"kept\": true}]   \r\n}\r\n";
    sh_mb_bundle b, again;
    char folder[MAX_PATH], path[MAX_PATH], cause[512], tree1[65], tree2[65];
    unsigned char *first, *second;
    size_t n1 = 0, n2 = 0, own_len = 0;
    char *own;
    int had = 0;
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\authored", tmp);
    CHECK(mt_copy_tree(fixture, folder));
    mt_write(folder, "mod.json", author);
    CHECK(sh_mb_tree_hash(folder, tree1));
    CHECK(sh_mb_build(folder, &RESOLVER, &b));
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
    first = sh_mb_read_file(path, 1 << 20, &n1);
    own = first ? sh_mb_author_bytes((const char *)first, n1, &own_len, &had) : NULL;
    CHECK(had && own && own_len == sizeof author - 1 && !memcmp(own, author, sizeof author - 1));
    free(own);
    /* CRLF in, CRLF out; the block follows the last author member. */
    CHECK(first && strstr((const char *)first, "\"x-notes\": [1, 2, {\"kept\": true}],\r\n\r\n  \"snapmap-plus\": {\r\n    \"format\": 1,"));
    /* The unknown key travels in the manifest's extra; the hash is the one
     * the same content had without it, plus that key. */
    CHECK(b.extra && strstr(b.extra, "\"x-notes\":[1,2,{\"kept\":true}]"));
    /* The tree hash does not see the product block. */
    CHECK(sh_mb_tree_hash(folder, tree2) && !strcmp(tree1, tree2));
    CHECK(sh_mb_load(path, &again) && !strcmp(again.hash, b.hash) && again.build == 1);
    sh_mb_free(&again);
    /* An unchanged rebuild writes the same bytes (build and built kept). */
    sh_mb_free(&b);
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && b.build == 1);
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    second = sh_mb_read_file(path, 1 << 20, &n2);
    CHECK(first && second && n1 == n2 && !memcmp(first, second, n1));
    free(second);
    /* The state is recorded in the block only. */
    CHECK(sh_mb_set_state(folder, 0, "a file was edited", cause, sizeof cause));
    CHECK(sh_mb_load(path, &again) && !again.validated && !strcmp(again.state_cause, "a file was edited"));
    sh_mb_free(&again);
    second = sh_mb_read_file(path, 1 << 20, &n2);
    own = second ? sh_mb_author_bytes((const char *)second, n2, &own_len, NULL) : NULL;
    CHECK(own && own_len == sizeof author - 1 && !memcmp(own, author, sizeof author - 1));
    CHECK(second && strstr((const char *)second, "\"validated\": false,\r\n    \"cause\": \"a file was edited\""));
    free(own);
    free(second);
    CHECK(sh_mb_set_state(folder, 1, "", cause, sizeof cause));
    /* Editing the author block changes the tree hash; the old block no longer loads. */
    mt_write(folder, "mod.json", "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.1\",\"name\":\"R2 Imp copy\"}");
    CHECK(sh_mb_tree_hash(folder, tree2) && strcmp(tree1, tree2));
    /* Touching a file under assets changes it too. */
    mt_write(folder, "mod.json", author);
    mt_append(folder, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n");
    CHECK(sh_mb_tree_hash(folder, tree2) && strcmp(tree1, tree2));
    free(first);

    /* A manifest alone (a download) gets a synthesized author block; the
     * written mod.json reloads with the manifest's hash. */
    {
        size_t blen = 0, tlen = 0;
        char *pretty = sh_mb_bundle_json(&b, &blen), *modtext;
        sh_mb_bundle m;
        CHECK(pretty && sh_mb_load_manifest_text(pretty, blen, &m));
        modtext = sh_mb_modjson_text(&m, NULL, 0, &tlen);
        CHECK(modtext && sh_mb_load_text(modtext, tlen, &again));
        if (again.errors) fprintf(stderr, "  synthesized: %s\n", again.first_cause);
        CHECK(!strcmp(again.hash, b.hash) && again.canonical_len == b.canonical_len &&
              !memcmp(again.canonical, b.canonical, b.canonical_len));
        sh_mb_free(&again);
        sh_mb_free(&m);
        free(modtext);
        free(pretty);
    }
    sh_mb_free(&b);
}

/* A converted package: its legacy record and grandfathered shadows live in
 * the product block, are build inputs, survive a rebuild and keep the hash. */
static void test_block_legacy(const char *fixture, const char *tmp)
{
    sh_mb_bundle b, again;
    char folder[MAX_PATH], cause[512], hash1[65];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\converted", tmp);
    CHECK(mt_copy_tree(fixture, folder));
    mt_write(folder, "assets\\generated\\decls\\entitydef\\smp\\oldthing.decl", "{\r\n}");
    mt_write(folder, "mod.json", "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\",\"description\":\"old pack\","
                                 "\"snapmap-plus\":{\"origin\":\"migration\",\"legacy\":{\"source\":\"overrides\",\"folder\":\"r2\"},"
                                 "\"shadows\":[\"generated/decls/entitydef/smp/oldthing.decl\"]}}");
    CHECK(sh_mb_build(folder, &RESOLVER, &b));
    if (b.errors) fprintf(stderr, "  converted: %s\n", b.first_cause);
    CHECK(b.legacy && b.block_legacy && !b.delivered && b.shadow_count == 1 && !strcmp(b.origin, "migration"));
    CHECK(b.extra && strstr(b.extra, "\"legacy\":{\"folder\":\"r2\",\"source\":\"overrides\"}"));
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause));
    strcpy_s(hash1, sizeof hash1, b.hash);
    sh_mb_free(&b);
    CHECK(sh_mb_load_folder(folder, &again) && !strcmp(again.hash, hash1) && again.legacy && !strcmp(again.origin, "migration"));
    sh_mb_free(&again);
    CHECK(sh_mb_build(folder, &RESOLVER, &b) && !strcmp(b.hash, hash1) && b.build == 1 && b.shadow_count == 1);
    sh_mb_free(&b);
}

/* The product record: sections replaced and removed one at a time, others
 * kept; an unreadable record is moved aside, not lost. */
static void test_record(const char *tmp)
{
    char root[MAX_PATH], *text = NULL, path[MAX_PATH];
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\recordmods", tmp);
    CHECK(sh_mb_record_get(root, "claims", &text) == 0 && !text);
    CHECK(sh_mb_record_put(root, "claims", "{\"map-1\":{\"claim\":\"c1\",\"secret\":\"s1\"}}"));
    CHECK(sh_mb_record_put(root, "cache", "{\"aa\":\"bb\"}"));
    CHECK(sh_mb_record_put(root, "claims", "{\"map-2\":{\"claim\":\"c2\"}}"));
    CHECK(sh_mb_record_get(root, "claims", &text) == 1 && text && !strcmp(text, "{\"map-2\":{\"claim\":\"c2\"}}"));
    free(text);
    CHECK(sh_mb_record_get(root, "cache", &text) == 1 && text && !strcmp(text, "{\"aa\":\"bb\"}"));
    free(text);
    CHECK(sh_mb_record_put(root, "cache", NULL));
    CHECK(sh_mb_record_get(root, "cache", &text) == 0);
    free(text);
    CHECK(!sh_mb_record_put(root, "format", "2") && !sh_mb_record_put(root, "links", "{not json"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", root);
    text = (char *)sh_mb_read_file(path, 1 << 20, NULL);
    CHECK(text && !strncmp(text, "{\n  \"format\": 1,\n  \"claims\": {", 30));
    free(text);
    mt_write(root, "snapmap-plus.json", "{broken");
    CHECK(sh_mb_record_get(root, "claims", &text) == -1);
    CHECK(sh_mb_record_put(root, "links", "{}"));
    CHECK(sh_mb_record_get(root, "links", &text) == 1);
    free(text);
    {
        WIN32_FIND_DATAA fd;
        char pattern[MAX_PATH];
        HANDLE h;
        _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\snapmap-plus.json.unreadable-*", root);
        h = FindFirstFileA(pattern, &fd);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
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
static void m_bad_cvar(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"g-god\":\"1\"}}")); }
static void m_bad_cvar_value(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"r_fov\":90.5}}")); }
static void m_bad_cvar_twice(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"r_fov\":1,\"R_FOV\":2}}")); }
static void m_bad_req_key(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"dlls\":{}}")); }
static void m_bad_line(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"commands\":{\"activate\":[\"echo a; quit\"]}}")); }
static void m_bad_line_ctrl(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"commands\":{\"deactivate\":[\"echo a\\nquit\"]}}")); }
static void m_bad_line_key(const char *f) { mt_write(f, "mod.json", DESC(",\"requirements\":{\"commands\":{\"start\":[\"echo\"]}}")); }
static void m_bad_line_count(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"requirements\":{\"commands\":{\"activate\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\","
                                 "\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\"]}}"));
}
static void m_gates_only(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":\"0\",\"g_useResourceBlackList\":\"0\"}}"));
}
static void m_req_general(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"requirements\":{\"commands\":{\"deactivate\":[\"echo bye\"],"
                                 "\"activate\":[\"  echo hi \",\"bind x \\\"say hi\\\"\"]},"
                                 "\"cvars\":{\"r_label\":\"two words\",\"g_god\":true,\"com_maxFps\":120,\"g_off\":false}}"));
}
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
/* A shadow whose text keeps a stock animation name the game ships without. */
static void m_shadow_dangling(const char *f)
{
    mt_write(f, "mod.json", DESC(",\"shadows\":[\"generated/decls/md6def/monster/imp.md6.decl\"]"));
    mt_write(f, "assets\\generated\\decls\\md6def\\monster\\imp.md6.decl",
             "{\r\n\tevents {\r\n\t\tanim \"md6/monster/imp/nothing_here.md6anim\" {\r\n\t\t}\r\n\t}\r\n}");
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
    refused(fixture, tmp, m_bad_cvar, &RESOLVER, "requirements.cvars key g-god is not a cvar name");
    refused(fixture, tmp, m_bad_cvar_value, &RESOLVER, "requirements.cvars.r_fov must be a string, an integer or a boolean");
    refused(fixture, tmp, m_bad_cvar_twice, &RESOLVER, "names r_fov twice");
    refused(fixture, tmp, m_bad_req_key, &RESOLVER, "requirements key dlls is not supported (cvars, commands)");
    refused(fixture, tmp, m_bad_line, &RESOLVER, "requirements.commands.activate[0] must be one console command");
    refused(fixture, tmp, m_bad_line_ctrl, &RESOLVER, "requirements.commands.deactivate[0] must be one console command");
    refused(fixture, tmp, m_bad_line_key, &RESOLVER, "requirements.commands key start is not supported");
    refused(fixture, tmp, m_bad_line_count, &RESOLVER, "requirements.commands.activate has more than 16 lines");
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
    /* A shadow keeping a stock name the game lacks is reported, not refused;
     * the same dangling name in a mod's own decl still refuses (m_missing_need). */
    accepted(fixture, tmp, m_shadow_dangling, &b);
    if (b.errors || b.warnings != 1) fprintf(stderr, "  dangling shadow: %s / %s\n", b.first_cause, b.first_warning);
    CHECK(b.errors == 0 && b.warnings == 1 && b.needs.unresolved == 1 &&
          strstr(b.first_warning, "resolves to nothing (a replaced stock file") != NULL &&
          find_file(&b, "generated/decls/md6def/monster/imp.md6.decl")->shadowed);
    sh_mb_free(&b);
    /* Values normalise to strings; unknown keys travel under extra. */
    accepted(fixture, tmp, m_requirements, &b);
    CHECK(b.requirements && !strcmp(b.requirements, GATES_CANONICAL));
    CHECK(b.extra && strstr(b.extra, "\"composes\":{\"hands\":true}") && b.homepage);
    sh_mb_free(&b);
    /* A bundle carrying only the two gates keeps the canonical text and the
     * hash the gates-only builder gave it. */
    accepted(fixture, tmp, m_gates_only, &b);
    CHECK(b.requirements && !strcmp(b.requirements, GATES_CANONICAL));
    CHECK(strstr(b.canonical, ",\"requirements\":" GATES_CANONICAL ",") != NULL);
    CHECK(!strcmp(b.hash, GATES_GOLDEN_HASH));
    if (strcmp(b.hash, GATES_GOLDEN_HASH)) fprintf(stderr, "  gates-only hash %s\n", b.hash);
    sh_mb_free(&b);
    /* Any cvar and any console line: sorted names, string values (true "1",
     * false "0", integers as digits), lines trimmed and in the author's order,
     * "commands" before "cvars". */
    accepted(fixture, tmp, m_req_general, &b);
    CHECK(b.requirements && !strcmp(b.requirements,
          "{\"commands\":{\"activate\":[\"echo hi\",\"bind x \\\"say hi\\\"\"],\"deactivate\":[\"echo bye\"]},"
          "\"cvars\":{\"com_maxFps\":\"120\",\"g_god\":\"1\",\"g_off\":\"0\",\"r_label\":\"two words\"}}"));
    if (b.requirements) printf("modbundle_test: general requirements canonical %s\n", b.requirements);
    CHECK(strcmp(b.hash, GATES_GOLDEN_HASH) != 0);
    sh_mb_free(&b);
    /* The canonicaliser on its own: empty lists vanish, "cvars" stays. */
    {
        static const char *const empty_lists = "{\"commands\":{\"activate\":[],\"deactivate\":[]},\"cvars\":{\"b\":false,\"a\":\"x\"}}";
        static const char *const accented = "{\"cvars\":{\"a\":\"\\u00e9\"}}";
        char cause[SH_MB_CAUSE_MAX];
        char *c = sh_mb_requirements_canonical(empty_lists, strlen(empty_lists), cause, sizeof cause);
        CHECK(c && !strcmp(c, "{\"cvars\":{\"a\":\"x\",\"b\":\"0\"}}"));
        free(c);
        c = sh_mb_requirements_canonical("{}", 2, cause, sizeof cause);
        CHECK(c && !strcmp(c, "{\"cvars\":{}}"));
        free(c);
        c = sh_mb_requirements_canonical(accented, strlen(accented), cause, sizeof cause);
        CHECK(!c && strstr(cause, "printable ASCII"));
        free(c);
        c = sh_mb_requirements_canonical("[]", 2, cause, sizeof cause);
        CHECK(!c && strstr(cause, "must be an object"));
    }

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

/* ------------------------------------------------------ render programs ---- */

static void test_md5fold(void)
{
    unsigned char d[16];
    char a[120];
    sh_mb_md5("abc", 3, d);
    CHECK(d[0] == 0x90 && d[1] == 0x01 && d[15] == 0x72);
    CHECK(sh_mb_md5fold("", 0) == 0x3b75655eu);
    CHECK(sh_mb_md5fold("abc", 3) == 0x275fa452u);
    memset(a, 'a', sizeof a);
    CHECK(sh_mb_md5fold(a, 56) == 0x1fc94972u);    /* padding crosses a block */
    CHECK(sh_mb_md5fold(a, 64) == 0x994388a2u);
    CHECK(sh_mb_md5fold(a, 119) == 0x4709fc8au);
}

static void test_spirv_names(void)
{
    char out[512], prog[256], mask[17];
    CHECK(sh_mb_spirv_request("mods/a/tint", ".vspv", out, sizeof out) && !strcmp(out, "generated/spirv/mods/a/tint.vspv"));
    CHECK(sh_mb_spirv_request("generated/renderprogs/permutations/mods/a/tint_0000000000000001.decl", ".fspv", out, sizeof out) &&
          !strcmp(out, "generated/spirv/generated/renderprogs/permutations/mods/a/tint_0000000000000001.fspv"));
    /* The spellings the archive and the engine use (19- and 17-character names). */
    CHECK(sh_mb_spirv_request("generated/renderprogs/permutations/cyberdemonshockwave_0000000000000001.decl", ".fspv", out, sizeof out) &&
          !strcmp(out, "generated/spirv/generated/renderprogs/permutations/cyberdemonshockwave_0000000000/000001.fspv"));
    CHECK(sh_mb_spirv_request("generated/renderprogs/permutations/abcdefghijklmnopq_0000000000000001.decl", ".vspv", out, sizeof out) &&
          !strcmp(out, "generated/spirv/generated/renderprogs/permutations/abcdefghijklmnopq_000000000000/0001.vspv"));
    /* 37 characters with the extension stay whole; 38 split. */
    CHECK(sh_mb_spirv_request("mods/a/abcdefghijklmnopqrstuvwxyz012345", ".vspv", out, sizeof out) &&
          !strcmp(out, "generated/spirv/mods/a/abcdefghijklmnopqrstuvwxyz012345.vspv"));
    CHECK(sh_mb_spirv_request("mods/a/abcdefghijklmnopqrstuvwxyz0123456", ".vspv", out, sizeof out) &&
          !strcmp(out, "generated/spirv/mods/a/abcdefghijklmnopqrstuvwxyz0123/456.vspv"));
    CHECK(sh_mb_rp_permutation("generated/renderprogs/permutations/mods/a/tint_0000000000000001.decl", prog, sizeof prog, mask) &&
          !strcmp(prog, "mods/a/tint") && !strcmp(mask, "0000000000000001"));
    CHECK(!sh_mb_rp_permutation("generated/renderprogs/permutations/mods/a/x_1.decl", prog, sizeof prog, mask));
    CHECK(!sh_mb_rp_permutation("generated/renderprogs/permutations/mods/a/tint_000000000000000g.decl", prog, sizeof prog, mask));
    CHECK(!sh_mb_rp_permutation("generated/renderprogs/permutations/_0000000000000001.decl", prog, sizeof prog, mask));
    CHECK(!sh_mb_rp_permutation("generated/renderprogs/mods/a/tint_0000000000000001.decl", prog, sizeof prog, mask));
}

static void rp_put(const char *folder, const char *rel, const void *data, size_t n)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    for (char *p = path + strlen(folder) + 1; *p; p++) if (*p == '/') *p = '\\';
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(data, 1, n, f); fclose(f); }
}

static void rp_be32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

#define RP_VS "#version 450 core\nvoid main() { gl_Position = vec4( 0.0 ); }\n"
#define RP_FS "#version 450 core\nlayout( location = 0 ) out vec4 o;\nvoid main() { o = vec4( 1.0 ); }\n"
#define RP_DECL "{\r\n\tparms {\r\n\t\tstageSort\tsortEmit\r\n\t}\r\n}"
#define RP_PERM "{\r\n\tpermutations {\r\n\t\tdefault AMD_VK_EXTENSIONS\r\n\t}\r\n}"
#define RP_MAT "{\r\nstageprogram\t\"mods/t.rp/tint\"\r\ntransmap\t_white\r\n}\r\n"

/* A cooked program: magic, checksum of `decl`, 16 bytes standing in for the
 * parm blocks, three stage records and the 36-byte trailer. */
static size_t rp_bin(unsigned char *out, const char *decl, unsigned checksum_xor, unsigned magic)
{
    const char *st[3] = { RP_VS, RP_FS, "" };
    size_t pos = 0, k, n = strlen(decl);
    while (n && (decl[n - 1] == '\n' || decl[n - 1] == '\r' || decl[n - 1] == ' ')) n--;
    rp_be32(out, magic);
    rp_be32(out + 4, sh_mb_md5fold(decl, n) ^ checksum_xor);
    pos = 8;
    memset(out + pos, 0, 16); pos += 16;
    for (k = 0; k < 3; k++) {
        size_t ln = strlen(st[k]);
        memset(out + pos, 0xff, 10); out[pos + 8] = 0; out[pos + 9] = (unsigned char)(k == 1 ? 1 : 3); pos += 10;
        out[pos] = (unsigned char)ln; out[pos + 1] = (unsigned char)(ln >> 8); out[pos + 2] = 0; out[pos + 3] = 0; pos += 4;
        memcpy(out + pos, st[k], ln); pos += ln;
    }
    memset(out + pos, 0xff, 36); pos += 36;
    return pos;
}

/* A SPIR-V file: the engine's header and 8 bytes of module. */
static size_t rp_spv(unsigned char *out, const char *stage, unsigned version, unsigned hash_xor, unsigned len_add)
{
    rp_be32(out, version);
    rp_be32(out + 4, sh_mb_md5fold(stage, strlen(stage)) ^ hash_xor);
    rp_be32(out + 8, 0x12345678u);
    rp_be32(out + 12, 8 + len_add);
    memcpy(out + 16, "\x03\x02\x23\x07\x00\x00\x01\x00", 8);
    return 24;
}

enum { RP_OK, RP_BAD_SUM, RP_BAD_MAGIC, RP_BAD_HASH, RP_BAD_VERSION, RP_BAD_LENGTH, RP_NO_FSPV, RP_NO_PERM_SPV,
       RP_NO_PERM, RP_NO_BIN, RP_BARE_NAME, RP_DOT_NAME, RP_INCLUDE, RP_PIPELINES, RP_GLSLBIN, RP_GLSL,
       RP_ORPHAN_PERM, RP_GL_BIN_SUM, RP_GL_BIN_OK, RP_ORPHAN_SPV };

static void rp_case(const char *tmp, int which, const char *needle)
{
    char folder[MAX_PATH];
    unsigned char buf[2048];
    size_t n;
    sh_mb_bundle b;
    const char *prog = which == RP_DOT_NAME ? "mods/t.rp/ti.nt" : "mods/t.rp/tint";
    char rel[512];
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\case%d", tmp, ++g_case);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", "{\"id\":\"t.rp\",\"name\":\"render program probe\"}");
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/decls/renderprog/%s.decl", prog);
    rp_put(folder, rel, RP_DECL, strlen(RP_DECL));
    if (which != RP_NO_BIN) {
        n = rp_bin(buf, RP_DECL, which == RP_BAD_SUM ? 1u : 0u, which == RP_BAD_MAGIC ? 0xB68800AEu : 0xB68800ADu);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/renderprogs/%s_pc_vulkan.bin", prog);
        rp_put(folder, rel, buf, n);
    }
    n = rp_spv(buf, RP_VS, 4, 0, 0);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/%s.vspv", prog);
    rp_put(folder, rel, buf, n);
    if (which != RP_NO_FSPV) {
        n = rp_spv(buf, RP_FS, which == RP_BAD_VERSION ? 3u : 4u, which == RP_BAD_HASH ? 1u : 0u, which == RP_BAD_LENGTH ? 4u : 0u);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/%s.fspv", prog);
        rp_put(folder, rel, buf, n);
    }
    if (which != RP_NO_PERM) {
        const char *pp = which == RP_ORPHAN_PERM ? "mods/t.rp/other" : prog;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/renderprogs/permutations/%s_0000000000000001.decl", pp);
        rp_put(folder, rel, RP_PERM, strlen(RP_PERM));
        n = rp_bin(buf, RP_PERM, 0, 0xB68800ADu);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/renderprogs/permutations/%s_0000000000000001_pc_vulkan.bin", pp);
        rp_put(folder, rel, buf, n);
        n = rp_spv(buf, RP_VS, 4, 0, 0);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/generated/renderprogs/permutations/%s_0000000000000001.vspv", pp);
        rp_put(folder, rel, buf, n);
        if (which != RP_NO_PERM_SPV) {
            n = rp_spv(buf, RP_FS, 4, 0, 0);
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/generated/renderprogs/permutations/%s_0000000000000001.fspv", pp);
            rp_put(folder, rel, buf, n);
        }
    }
    if (which == RP_ORPHAN_PERM) {   /* the program's own permutation, so only the orphan is wrong */
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/renderprogs/permutations/%s_0000000000000001.decl", prog);
        rp_put(folder, rel, RP_PERM, strlen(RP_PERM));
        n = rp_bin(buf, RP_PERM, 0, 0xB68800ADu);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/renderprogs/permutations/%s_0000000000000001_pc_vulkan.bin", prog);
        rp_put(folder, rel, buf, n);
        n = rp_spv(buf, RP_VS, 4, 0, 0);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/generated/renderprogs/permutations/%s_0000000000000001.vspv", prog);
        rp_put(folder, rel, buf, n);
        n = rp_spv(buf, RP_FS, 4, 0, 0);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets/generated/spirv/generated/renderprogs/permutations/%s_0000000000000001.fspv", prog);
        rp_put(folder, rel, buf, n);
    }
    if (which == RP_GL_BIN_SUM || which == RP_GL_BIN_OK) {
        n = rp_bin(buf, RP_DECL, which == RP_GL_BIN_SUM ? 2u : 0u, 0xB68800ADu);
        rp_put(folder, "assets/generated/renderprogs/mods/t.rp/tint_pc.bin", buf, n);
    }
    if (which == RP_ORPHAN_SPV) {
        n = rp_spv(buf, RP_FS, 4, 0, 0);
        rp_put(folder, "assets/generated/spirv/mods/t.rp/unused.fspv", buf, n);
    }
    if (which != RP_DOT_NAME) {
        if (which == RP_BARE_NAME)
            mt_write(folder, "assets\\generated\\decls\\material\\mods\\t.rp\\tintmat.decl",
                     "{\r\nstageprogram\tmods/t.rp/tint\r\ntransmap\t_white\r\n}\r\n");
        else
            mt_write(folder, "assets\\generated\\decls\\material\\mods\\t.rp\\tintmat.decl", RP_MAT);
    }
    if (which == RP_INCLUDE) rp_put(folder, "assets/decls/renderprogs/global.inc", "x", 1);
    if (which == RP_PIPELINES) rp_put(folder, "assets/generated/vulkan_pipelines.bin", "x", 1);
    if (which == RP_GLSLBIN) rp_put(folder, "assets/generated/glslbinaries/mods/t.rp/tint.bin", "x", 1);
    if (which == RP_GLSL) rp_put(folder, "assets/generated/glsl/mods/t.rp/tint.fspv", "x", 1);

    if (!needle) {
        CHECK(sh_mb_build(folder, &RESOLVER, &b));
        if (b.errors) fprintf(stderr, "  render-program case %d refused: %s\n", g_case, b.first_cause);
        CHECK(b.errors == 0);
        if (which == RP_ORPHAN_SPV) CHECK(b.warnings == 1 && strstr(b.first_warning, "no render program in the bundle binds it"));
        else CHECK(b.warnings == 0);
        if (which == RP_OK) {
            const sh_mb_file *p = find_file(&b, "generated/renderprogs/permutations/mods/t.rp/tint_0000000000000001.decl");
            const sh_mb_file *m = find_file(&b, "generated/decls/material/mods/t.rp/tintmat.decl");
            CHECK(p && !strcmp(p->ptype, "renderProg") && !strcmp(p->pname, p->request) && !p->is_decl);
            CHECK(m && has_need(m, "renderProg", "mods/t.rp/tint"));
            CHECK(b.file_count == 9);
        }
    } else {
        CHECK(!sh_mb_build(folder, &RESOLVER, &b));
        CHECK_CAUSE(b, needle);
    }
    sh_mb_free(&b);
}

static void test_renderprogs(const char *tmp)
{
    test_md5fold();
    test_spirv_names();
    {
        unsigned char buf[2048];
        size_t off[3], n[3], len = rp_bin(buf, RP_DECL, 0, 0xB68800ADu);
        CHECK(sh_mb_rp_stages(buf, len, off, n) == 1 && n[0] == strlen(RP_VS) && n[1] == strlen(RP_FS) && n[2] == 0 &&
              !memcmp(buf + off[1], RP_FS, n[1]));
        CHECK(sh_mb_rp_stages(buf, len - 1, off, n) == 0);
        CHECK(sh_mb_rp_stages(buf, 20, off, n) == 0);
    }
    rp_case(tmp, RP_OK, NULL);
    rp_case(tmp, RP_GL_BIN_OK, NULL);
    rp_case(tmp, RP_ORPHAN_SPV, NULL);
    rp_case(tmp, RP_BAD_SUM, "has checksum");                 /* check 1 */
    rp_case(tmp, RP_BAD_MAGIC, "magic 0xB68800AD missing");
    rp_case(tmp, RP_GL_BIN_SUM, "tint_pc.bin has checksum");
    rp_case(tmp, RP_BAD_HASH, "is not MD5fold of its stage text");   /* check 2 */
    rp_case(tmp, RP_BAD_VERSION, "version is not 4");
    rp_case(tmp, RP_BAD_LENGTH, "header length");
    rp_case(tmp, RP_NO_FSPV, "tint.fspv (its fragment stage)");     /* check 3 */
    rp_case(tmp, RP_NO_PERM_SPV, "permutations/mods/t.rp/tint_0000000000000001.fspv (its fragment stage)");
    rp_case(tmp, RP_NO_PERM, "ships no permutation decl");
    rp_case(tmp, RP_NO_BIN, "tint_pc_vulkan.bin: Vulkan needs the cooked program");
    rp_case(tmp, RP_ORPHAN_PERM, "permutation of mods/t.rp/other, which the bundle does not ship");
    rp_case(tmp, RP_BARE_NAME, "must be quoted");                   /* check 6 */
    rp_case(tmp, RP_DOT_NAME, "'.' in its last component");
    rp_case(tmp, RP_INCLUDE, "shared render-program include");      /* check 7 */
    rp_case(tmp, RP_PIPELINES, "pipeline warm-up list");
    rp_case(tmp, RP_GLSLBIN, "driver's binary cache");
    rp_case(tmp, RP_GLSL, "debug dump of generated GLSL");
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
    test_modjson(full, tmp);
    test_block_legacy(full, tmp);
    test_record(tmp);
    test_refusals(full, tmp);
    test_renderprogs(tmp);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modbundle_test: %d failure(s)\n", failures); return 1; }
    printf("modbundle_test: hashes, canonical form, logical names, needs, %d refusal/acceptance cases, build numbers, "
           "mod.json round trip, tree hash and product record pass\n", g_case);
    return 0;
}
