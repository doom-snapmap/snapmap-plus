/* modstock_test -- stock pointers (Contract 1 file kind "stock") end to end
 * over synthetic installs, and a measurement over the real store.
 *
 * Two fake installs are written under a temporary folder: D1 with
 * gameresources (.resources and .patch, one stored slice and one raw-DEFLATE
 * slice), snap_gameresources holding the same path with other bytes, and two
 * loose files (base\ and the install root); D2, a "different game revision",
 * lacks one entry and holds another with other bytes. Covered:
 *   the bridge: match at the file's own path, a renamed copy, the .patch
 *     store, a loose file; identify, verify (OK, MISMATCH, ABSENT) and read;
 *   the builder: carried copies become pointers (no blob, no size in the
 *     canonical text, not in the size total), a type outside the whitelist
 *     that is installed content becomes a pointer instead of a refusal, the
 *     descriptor's "stock" object (hash filled by the builder, a wrong hash,
 *     an absent source, a carried copy with other bytes, no reader), and the
 *     manifest loads back with the same hash;
 *   the store: no blob and no tree file for a pointer, verification resolves
 *     pointers, and fails with the first cause against D2;
 *   the store client: an upload sends only carried blobs (loopback stub), a
 *     download fetches only carried blobs and is refused before any fetch on
 *     D2;
 *   the campaign-only closure: a decl naming content only gameresources holds
 *     gains it as a pointer, transitively (animWeb -> md6Def -> mesh and
 *     skeleton), stable across a rebuild; snap_gameresources names gain
 *     nothing; a name no archive holds still fails; the file cap fails by name;
 *     derived again by every build (a pointer whose need is gone is dropped,
 *     an author stock entry stays); an effect gains its campaign-only
 *     particle, ribbon, their materials and a transparency atlas, an
 *     entityDef its movie and the movie's atlas, and a virtual-textured
 *     material with an installed row no image;
 *   serving: a pointer is answered with the installed bytes, a carried copy of
 *     the same bytes in another bundle is not a conflict, and a pointer D2
 *     cannot answer falls through.
 *
 * Measurement (skipped unless SH_MODSTOCK_DATA names a mods store; the
 * install is SH_MODSTOCK_DOOM): for the Imp, both cyberdemon packs and the
 * doors pack, files total, files that become pointers and bytes not stored,
 * and the cost (index load, bytes hashed, time).
 */
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modstock.h"
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

#define P_DECL  "generated/decls/entitydef/monster/stock_a.decl"
#define P_IMAGE "generated/image/stock/b.bimage"
#define P_ANIM  "cooked/anim/stock/c.bmd6anim"
#define P_TOME  "maps/game/x/world.tome"
#define P_LOOSE "generated/cm/loose.bcm"
#define P_PAGES "virtualtextures/t/v.pages"

static const char DECL_A[] = "{\r\n}\r\n";
static unsigned char IMG_B[300], IMG_B2[300], IMG_B3[300], ANIM_C[180], TOME_T[90], LOOSE_L[70], PAGES_P[120], OWN[40];

static void hex_of(const void *p, size_t n, char out[65]) { sh_mb_sha256_hex(p, n, out); }

static void make_installs(const char *d1, const char *d2)
{
    size_t i;
    for (i = 0; i < sizeof IMG_B; i++) { IMG_B[i] = (unsigned char)(i * 7 + 1); IMG_B2[i] = (unsigned char)(i * 11 + 3); IMG_B3[i] = (unsigned char)(i * 13 + 5); }
    for (i = 0; i < sizeof ANIM_C; i++) ANIM_C[i] = (unsigned char)(i * 3 + 9);
    for (i = 0; i < sizeof TOME_T; i++) TOME_T[i] = (unsigned char)(255 - i);
    for (i = 0; i < sizeof LOOSE_L; i++) LOOSE_L[i] = (unsigned char)(i ^ 0x5a);
    for (i = 0; i < sizeof PAGES_P; i++) PAGES_P[i] = (unsigned char)(i ^ 0xa5);
    for (i = 0; i < sizeof OWN; i++) OWN[i] = (unsigned char)(i + 100);
    {
        entry g[] = {
            { P_DECL,  (const unsigned char *)DECL_A, sizeof DECL_A - 1, 0, 0 },
            { P_IMAGE, IMG_B, sizeof IMG_B, 0, 1 },
            { P_ANIM,  ANIM_C, sizeof ANIM_C, 1, 1 },
            { P_TOME,  TOME_T, sizeof TOME_T, 1, 0 },
        };
        entry s[] = { { P_IMAGE, IMG_B2, sizeof IMG_B2, 0, 0 } };
        write_container(d1, "gameresources", g, 4);
        write_container(d1, "snap_gameresources", s, 1);
    }
    put_rel(d1, "base\\" P_LOOSE, LOOSE_L, sizeof LOOSE_L);
    put_rel(d1, P_PAGES, PAGES_P, sizeof PAGES_P);
    {
        /* Another revision: the decl is gone and the image has other bytes. */
        entry g[] = {
            { P_IMAGE, IMG_B3, sizeof IMG_B3, 0, 1 },
            { P_ANIM,  ANIM_C, sizeof ANIM_C, 1, 1 },
            { P_TOME,  TOME_T, sizeof TOME_T, 1, 0 },
        };
        write_container(d2, "gameresources", g, 3);
    }
}

/* ------------------------------------------------------------- bridge ---- */

static void test_bridge(const char *d1)
{
    char src[SH_MB_REQUEST_MAX + 1], h[65], got[65];
    unsigned long long size = 0;
    unsigned char *bytes = NULL;
    size_t len = 0;
    sh_modstock_stats ss;

    sh_modstock_set_install(d1);
    hex_of(IMG_B, sizeof IMG_B, h);
    CHECK(sh_modstock_match(P_IMAGE, sizeof IMG_B, h, src, sizeof src) == SH_MB_STOCK_OK && !strcmp(src, P_IMAGE));
    /* A renamed copy: same size and extension elsewhere. */
    CHECK(sh_modstock_match("generated/image/mods/t.stock/b_copy.bimage", sizeof IMG_B, h, src, sizeof src) == SH_MB_STOCK_OK &&
          !strcmp(src, P_IMAGE));
    /* The other container's bytes at the same path. */
    hex_of(IMG_B2, sizeof IMG_B2, h);
    CHECK(sh_modstock_match(P_IMAGE, sizeof IMG_B2, h, src, sizeof src) == SH_MB_STOCK_OK && !strcmp(src, P_IMAGE));
    /* The .patch store, inflated. */
    hex_of(ANIM_C, sizeof ANIM_C, h);
    CHECK(sh_modstock_match(P_ANIM, sizeof ANIM_C, h, src, sizeof src) == SH_MB_STOCK_OK);
    CHECK(sh_modstock_read(P_ANIM, h, &bytes, &len) == SH_MB_STOCK_OK && len == sizeof ANIM_C && !memcmp(bytes, ANIM_C, len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    /* Loose files under base\ and the install root. */
    hex_of(LOOSE_L, sizeof LOOSE_L, h);
    CHECK(sh_modstock_match(P_LOOSE, sizeof LOOSE_L, h, src, sizeof src) == SH_MB_STOCK_OK && !strcmp(src, P_LOOSE));
    hex_of(PAGES_P, sizeof PAGES_P, h);
    CHECK(sh_modstock_verify(P_PAGES, h, &size) == SH_MB_STOCK_OK && size == sizeof PAGES_P);
    CHECK(sh_modstock_read(P_PAGES, h, &bytes, &len) == SH_MB_STOCK_OK && len == sizeof PAGES_P);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    /* Content no entry holds. */
    hex_of(OWN, sizeof OWN, h);
    CHECK(sh_modstock_match("generated/image/mods/t.stock/own.bimage", sizeof OWN, h, src, sizeof src) == SH_MB_STOCK_ABSENT);
    /* identify takes SnapMap's own container first. */
    CHECK(sh_modstock_identify(P_IMAGE, got, &size) == SH_MB_STOCK_OK && size == sizeof IMG_B2);
    hex_of(IMG_B2, sizeof IMG_B2, h);
    CHECK(!strcmp(got, h));
    hex_of(IMG_B, sizeof IMG_B, h);
    CHECK(sh_modstock_verify(P_IMAGE, h, &size) == SH_MB_STOCK_OK);
    CHECK(sh_modstock_verify(P_IMAGE, "0000000000000000000000000000000000000000000000000000000000000000", NULL) == SH_MB_STOCK_MISMATCH);
    CHECK(sh_modstock_verify("generated/image/stock/none.bimage", h, NULL) == SH_MB_STOCK_ABSENT);
    CHECK(sh_modstock_exists(P_TOME) == 1 && sh_modstock_exists(P_PAGES) == 1 && sh_modstock_exists("x/y.decl") == 0);
    CHECK(sh_modstock_read(P_IMAGE, h, &bytes, &len) == SH_MB_STOCK_OK && len == sizeof IMG_B && !memcmp(bytes, IMG_B, len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    sh_modstock_get_stats(&ss);
    CHECK(ss.loaded == 1 && ss.rows == 5 && ss.reads >= 3);
    /* Which part of the install holds a source (the Files view's game files):
     * one archive by name, a name both hold decided by the bytes, a loose file. */
    {
        char where[32];
        CHECK(sh_modstock_container(P_ANIM, NULL, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, "gameresources"));
        hex_of(IMG_B2, sizeof IMG_B2, h);
        CHECK(sh_modstock_container(P_IMAGE, h, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, "snap_gameresources"));
        hex_of(IMG_B, sizeof IMG_B, h);
        CHECK(sh_modstock_container(P_IMAGE, h, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, "gameresources"));
        CHECK(sh_modstock_container(P_LOOSE, NULL, where, sizeof where) == SH_MB_STOCK_OK && !strcmp(where, "loose"));
        CHECK(sh_modstock_container("x/none.decl", NULL, where, sizeof where) == SH_MB_STOCK_ABSENT && !where[0]);
        sh_modstock_register();
        CHECK(sh_mb_stock_container(P_ANIM, NULL, where, sizeof where) && !strcmp(where, "gameresources"));
    }

    /* No installed index: unavailable, never "absent". */
    {
        char empty[MAX_PATH * 2];
        _snprintf_s(empty, sizeof empty, _TRUNCATE, "%s\\..\\nothing-here", d1);
        sh_modstock_set_install(empty);
        CHECK(sh_modstock_match(P_IMAGE, sizeof IMG_B, h, src, sizeof src) == SH_MB_STOCK_UNAVAILABLE);
        CHECK(sh_modstock_exists(P_IMAGE) == -1);
        sh_modstock_get_stats(&ss);
        CHECK(ss.loaded == -1 && ss.cause[0]);
    }
    sh_modstock_set_install(d1);
}

/* ------------------------------------------------------------- builder ---- */

static int stock_exists(void *ctx, const char *request) { (void)ctx; return sh_modstock_exists(request); }
static const sh_mb_resolver RESOLVER = { NULL, NULL, stock_exists, NULL };

static const sh_mb_file *find(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

#define REQ_ALIAS "generated/decls/entitydef/mods/t.stock/alias.decl"
#define REQ_COPY  "generated/image/mods/t.stock/b_copy.bimage"
#define REQ_OWN   "generated/image/mods/t.stock/own.bimage"
#define REQ_TOME  "maps/mods/t.stock/world.tome"

static void make_mod(const char *dir, const char *stock_json)
{
    char desc[1024];
    _snprintf_s(desc, sizeof desc, _TRUNCATE,
                "{\"id\":\"t.stock\",\"name\":\"Stock test\",\"shadows\":[\"" P_IMAGE "\"]%s}", stock_json ? stock_json : "");
    mt_write(dir, "mod.json", desc);
    put_rel(dir, "assets\\" P_IMAGE, IMG_B, sizeof IMG_B);
    put_rel(dir, "assets\\" REQ_COPY, IMG_B, sizeof IMG_B);
    put_rel(dir, "assets\\" REQ_OWN, OWN, sizeof OWN);
    put_rel(dir, "assets\\" REQ_TOME, TOME_T, sizeof TOME_T);
}

#define ALIAS_JSON ",\"stock\":{\"" REQ_ALIAS "\":{\"source\":\"" P_DECL "\"}}"

static void test_builder(const char *tmp)
{
    char dir[MAX_PATH * 2], h[65], json[512], cause[SH_MB_CAUSE_MAX];
    sh_mb_bundle b, back;
    const sh_mb_file *f;

    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod1", tmp);
    make_mod(dir, ALIAS_JSON);
    CHECK(sh_mb_build(dir, &RESOLVER, &b));
    if (b.errors) fprintf(stderr, "  build refused: %s\n", b.first_cause);
    CHECK(b.file_count == 5 && b.stock_count == 4 && b.total_size == sizeof OWN);
    CHECK(b.stock_bytes == sizeof IMG_B * 2 + sizeof TOME_T + (sizeof DECL_A - 1));
    f = find(&b, REQ_COPY);
    CHECK(f && f->stock && f->stock_source && !strcmp(f->stock_source, P_IMAGE));
    /* Additive mods: the installed bytes at the game's own name are a surface,
     * not a shadow, whatever the descriptor's old "shadows" said. */
    f = find(&b, P_IMAGE);
    CHECK(f && f->stock && !f->shadowed && f->cls == SH_MB_CLASS_SURFACE && b.shadow_count == 0);
    f = find(&b, REQ_COPY);
    CHECK(f && f->cls == SH_MB_CLASS_BORROW);
    CHECK(b.classes.surfaces == 1 && b.classes.borrows == 3 && b.classes.adds == 1);
    f = find(&b, REQ_TOME);   /* .tome is outside the whitelist, but it is installed content */
    CHECK(f && f->stock && !strcmp(f->stock_source, P_TOME));
    f = find(&b, REQ_ALIAS);
    CHECK(f && f->stock && f->is_decl && !strcmp(f->stock_source, P_DECL));
    hex_of(DECL_A, sizeof DECL_A - 1, h);
    CHECK(f && !strcmp(f->blob, h));
    f = find(&b, REQ_OWN);
    CHECK(f && !f->stock);
    /* Canonical form: a pointer has stock and neither blob nor size. */
    hex_of(IMG_B, sizeof IMG_B, h);
    _snprintf_s(json, sizeof json, _TRUNCATE, "\"request\":\"%s\",\"stock\":{\"sha256\":\"%s\",\"source\":\"%s\"}}", REQ_COPY, h, P_IMAGE);
    CHECK(b.canonical && strstr(b.canonical, json));
    CHECK(strstr(b.canonical, "{\"needs\":[],\"path\":\"assets/" REQ_COPY "\"") != NULL);
    CHECK(strstr(b.canonical, "\"stock\":{\"sha256\"") && !strstr(b.canonical, "\"descriptor\""));
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    {
        char path[MAX_PATH * 2];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
        CHECK(sh_mb_load(path, &back));
        CHECK(!strcmp(back.hash, b.hash) && back.stock_count == 4 && back.total_size == sizeof OWN);
        f = find(&back, REQ_TOME);
        CHECK(f && f->stock && f->size == 0 && !strcmp(f->stock_source, P_TOME));
        sh_mb_free(&back);
    }
    sh_mb_free(&b);
    /* A rebuild of the same content keeps its build number. */
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.build == 1);
    sh_mb_free(&b);

    /* Refusals. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod2", tmp);
    make_mod(dir, ",\"stock\":{\"" REQ_ALIAS "\":{\"source\":\"" P_DECL "\",\"sha256\":"
                  "\"1111111111111111111111111111111111111111111111111111111111111111\"}}");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "with other bytes (a different game revision)");
    sh_mb_free(&b);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod3", tmp);
    make_mod(dir, ",\"stock\":{\"" REQ_ALIAS "\":{\"source\":\"generated/decls/entitydef/none.decl\"}}");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "which this game does not have");
    sh_mb_free(&b);
    /* Declared over a carried file with other bytes: the entry is ignored and
     * the file's bytes decide (an addition). */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod4", tmp);
    make_mod(dir, ",\"stock\":{\"" REQ_OWN "\":{\"source\":\"" P_IMAGE "\"}}");
    CHECK(sh_mb_build(dir, &RESOLVER, &b));
    f = find(&b, REQ_OWN);
    CHECK(f && !f->stock && f->cls == SH_MB_CLASS_ADD);
    sh_mb_free(&b);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod5", tmp);   /* a .tome that is not installed content */
    make_mod(dir, NULL);
    put_rel(dir, "assets\\maps\\mods\\t.stock\\other.tome", OWN, sizeof OWN);
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "assets/maps/mods/t.stock/other.tome is not installed content: a .sbsp, .ambientsh, .tome or .pages file can only be a stock pointer");
    sh_mb_free(&b);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod6", tmp);
    make_mod(dir, ",\"stock\":{\"assets/x.decl\":{\"source\":\"" P_DECL "\"}}");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "must be a request path, without assets/");
    sh_mb_free(&b);

    /* Without a reader: nothing becomes a pointer and a declared one is refused.
     * The files at game names are then carried replacements, so these cases
     * build as received content (sh_mb_keep_replacements_here; the refusal is modbundle_test's). */
    sh_mb_set_stock_ops(NULL);
    sh_mb_keep_replacements_here(1);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod7", tmp);
    make_mod(dir, NULL);
    mt_delete(dir, "assets\\" REQ_TOME);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.stock_count == 0 && b.total_size == sizeof IMG_B * 2 + sizeof OWN);
    CHECK(b.canonical && !strstr(b.canonical, "\"stock\""));
    sh_mb_free(&b);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod8", tmp);
    make_mod(dir, ALIAS_JSON);
    mt_delete(dir, "assets\\" REQ_TOME);
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "installed-content reader is unavailable");
    sh_mb_free(&b);
    sh_mb_keep_replacements_here(0);
    sh_modstock_register();

    /* Manifest grammar: a pointer may not carry blob or size, and its source
     * is a lower-cased path. */
    {
        static const char *const bad[] = {
            "{\"needs\":[],\"path\":\"assets/" REQ_COPY "\",\"provides\":{\"name\":\"mods/t.stock/b_copy\",\"type\":\"image\"},"
            "\"request\":\"" REQ_COPY "\",\"size\":300,\"stock\":{\"sha256\":\"%s\",\"source\":\"" P_IMAGE "\"}}",
            "{\"needs\":[],\"path\":\"assets/" REQ_COPY "\",\"provides\":{\"name\":\"mods/t.stock/b_copy\",\"type\":\"image\"},"
            "\"request\":\"" REQ_COPY "\",\"stock\":{\"sha256\":\"%s\",\"source\":\"Generated/Image/Stock/B.bimage\"}}",
            "{\"needs\":[],\"path\":\"assets/" REQ_COPY "\",\"provides\":{\"name\":\"mods/t.stock/b_copy\",\"type\":\"image\"},"
            "\"request\":\"" REQ_COPY "\",\"stock\":{\"sha256\":\"%s\",\"source\":\"../x.bimage\"}}",
        };
        int k;
        hex_of(IMG_B, sizeof IMG_B, h);
        for (k = 0; k < 3; k++) {
            char file[1024], text[2048];
            _snprintf_s(file, sizeof file, _TRUNCATE, bad[k], h);
            _snprintf_s(text, sizeof text, _TRUNCATE, "{\"build\":1,\"bundle\":\"%s\",\"files\":[%s],\"format\":1,\"id\":\"t.stock\","
                        "\"name\":\"x\",\"version\":\"0.0.0\"}", h, file);
            CHECK(!sh_mb_load_manifest_text(text, strlen(text), &b));
            CHECK(strstr(b.first_cause, "stock") != NULL);
            sh_mb_free(&b);
        }
    }
}

/* ------------------------------------- renamed copies, names, dead needs ---- */
/* A third install (D3): 100 rows of one image size and extension before the
 * one a copy equals (more than the 64 candidates the matcher once stopped
 * at), one name both archives hold with other bytes, an md6 collision model
 * shipped only under generated/model/, and a stock md6Def naming a clip no
 * archive holds. */

#define D3_MANY      100
#define D3_LATE      "generated/image/z/late.bimage"
#define D3_TWO       "generated/image/two/v.bimage"
#define D3_COLLISION "generated/model/md6/m/collision/c.bmodel"
#define D3_MD6       "generated/decls/md6def/m/x.md6.decl"
static const char MD6_STOCK[] = "{\r\n\tanim \"md6/m/dead.md6anim\" {\r\n\t}\r\n}\r\n";
static unsigned char D3_IMG[D3_MANY][300], LATE_B[300], TWO_SNAP[300], TWO_CAMP[300], COLL_B[64];
static char D3_NAMES[D3_MANY][64];

static void make_install3(const char *d3)
{
    entry g[D3_MANY + 4], s[1];
    int i, k;
    for (i = 0; i < D3_MANY; i++) {
        for (k = 0; k < 300; k++) D3_IMG[i][k] = (unsigned char)(k * 5 + i * 17 + 2);
        _snprintf_s(D3_NAMES[i], sizeof D3_NAMES[i], _TRUNCATE, "generated/image/a/%03d.bimage", i);
        g[i].path = D3_NAMES[i]; g[i].bytes = D3_IMG[i]; g[i].n = 300; g[i].patch = 0; g[i].deflate = 0;
    }
    for (k = 0; k < 300; k++) { LATE_B[k] = (unsigned char)(k ^ 0x3c); TWO_SNAP[k] = (unsigned char)(k * 9 + 4); TWO_CAMP[k] = (unsigned char)(k * 19 + 6); }
    for (k = 0; k < 64; k++) COLL_B[k] = (unsigned char)(k + 33);
    g[D3_MANY].path = D3_LATE;           g[D3_MANY].bytes = LATE_B;      g[D3_MANY].n = 300;
    g[D3_MANY + 1].path = D3_TWO;        g[D3_MANY + 1].bytes = TWO_CAMP; g[D3_MANY + 1].n = 300;
    g[D3_MANY + 2].path = D3_COLLISION;  g[D3_MANY + 2].bytes = COLL_B;   g[D3_MANY + 2].n = sizeof COLL_B;
    g[D3_MANY + 3].path = D3_MD6;        g[D3_MANY + 3].bytes = (const unsigned char *)MD6_STOCK; g[D3_MANY + 3].n = sizeof MD6_STOCK - 1;
    for (i = D3_MANY; i < D3_MANY + 4; i++) { g[i].patch = 0; g[i].deflate = 0; }
    s[0].path = D3_TWO; s[0].bytes = TWO_SNAP; s[0].n = 300; s[0].patch = 0; s[0].deflate = 0;
    write_container(d3, "gameresources", g, D3_MANY + 4);
    write_container(d3, "snap_gameresources", s, 1);
}

static void l1c_mod(const char *dir, const char *rel, const char *text)
{
    mt_write(dir, "mod.json", "{\"id\":\"t.l1c\",\"name\":\"L1c test\"}");
    mt_write(dir, rel, text);
}

static void test_renamed_names_dead(const char *tmp)
{
    char d3[MAX_PATH * 2], dir[MAX_PATH * 2], src[SH_MB_REQUEST_MAX + 1], h[65], cut[SH_MB_REQUEST_MAX + 1];
    unsigned long long size = 0;
    sh_mb_bundle b;
    const sh_mb_file *f;

    /* The key cut, both spellings, wherever a path segment starts. */
    CHECK(sh_mb_strip_key("generated/image/mods/olivia_pierce_complete/models/a.bimage", cut, sizeof cut) &&
          !strcmp(cut, "generated/image/models/a.bimage"));
    CHECK(sh_mb_strip_key("generated/decls/md6def/mods/olivia-pierce-complete/zion/x.md6.decl", cut, sizeof cut) &&
          !strcmp(cut, "generated/decls/md6def/zion/x.md6.decl"));
    CHECK(sh_mb_strip_key("md6/mods/local.hell_guard/characters/A.lwo", cut, sizeof cut) && !strcmp(cut, "md6/characters/a.lwo"));
    CHECK(sh_mb_strip_key("mods/k/x.decl", cut, sizeof cut) && !strcmp(cut, "x.decl"));
    CHECK(!sh_mb_strip_key("generated/image/godmods/k/x.bimage", cut, sizeof cut));
    CHECK(!sh_mb_strip_key("generated/image/mods/x.bimage", cut, sizeof cut));
    CHECK(!sh_mb_strip_key("generated/image/stock/b.bimage", cut, sizeof cut));

    _snprintf_s(d3, sizeof d3, _TRUNCATE, "%s\\d3", tmp);
    make_install3(d3);
    sh_modstock_set_install(d3);
    /* A renamed copy past 64 same-size candidates is found (budget only). */
    hex_of(LATE_B, sizeof LATE_B, h);
    CHECK(sh_modstock_match("generated/image/mods/t.l1c/q/elsewhere.bimage", 300, h, src, sizeof src) == SH_MB_STOCK_OK &&
          !strcmp(src, D3_LATE));
    /* The de-namespaced path is tried first, under either key spelling. */
    CHECK(sh_modstock_match("generated/image/mods/t_l1c/z/late.bimage", 300, h, src, sizeof src) == SH_MB_STOCK_OK &&
          !strcmp(src, D3_LATE));
    /* A name both archives hold with other bytes: the variant whose bytes the
     * copy has is the one named, whichever archive holds it. */
    hex_of(TWO_CAMP, sizeof TWO_CAMP, h);
    CHECK(sh_modstock_match("generated/image/mods/t.l1c/two/v.bimage", 300, h, src, sizeof src) == SH_MB_STOCK_OK &&
          !strcmp(src, D3_TWO) && sh_modstock_verify(src, h, &size) == SH_MB_STOCK_OK && size == 300);
    hex_of(TWO_SNAP, sizeof TWO_SNAP, h);
    CHECK(sh_modstock_match("generated/image/mods/t.l1c/two/v.bimage", 300, h, src, sizeof src) == SH_MB_STOCK_OK &&
          !strcmp(src, D3_TWO));
    /* Bytes no row holds stay absent, and the source is left empty. */
    hex_of(D3_IMG[0], 299, h);
    CHECK(sh_modstock_match("generated/image/mods/t.l1c/a/000.bimage", 300, h, src, sizeof src) == SH_MB_STOCK_ABSENT && !src[0]);

    /* An md6 collision model name resolves to generated/model/, as the game ships it. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\l1c1", tmp);
    l1c_mod(dir, "assets\\generated\\decls\\entitydef\\mods\\t.l1c\\death.decl",
            "{\r\n\tedit = {\r\n\t\trenderModelInfo = {\r\n\t\t\tmodel = \"md6/m/collision/c.lwo\";\r\n\t\t}\r\n\t}\r\n}\r\n");
    /* Only the campaign archive holds it here, so it is brought along. */
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.needs.local == 1 && b.needs.unresolved == 0);
    if (b.errors) fprintf(stderr, "  collision model: %s\n", b.first_cause);
    f = find(&b, D3_COLLISION);
    CHECK(f && f->stock && f->closure && b.stock_count == 1);
    sh_mb_free(&b);

    /* A copied stock md6Def keeping a clip no archive holds: a warning naming
     * the stock decl, not an error. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\l1c2", tmp);
    l1c_mod(dir, "assets\\generated\\decls\\md6def\\mods\\t.l1c\\m\\x.md6.decl",
            "{\r\n\tanim \"md6/m/dead.md6anim\" {\r\n\t\t// namespaced copy\r\n\t}\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.warnings == 1 && b.needs.unresolved == 1);
    CHECK(strstr(b.first_warning, "names anim md6/m/dead.md6anim, which no archive holds; the game's own " D3_MD6
                                  " names it too") != NULL);
    f = find(&b, "generated/decls/md6def/mods/t.l1c/m/x.md6.decl");
    CHECK(f && !f->stock && f->cls == SH_MB_CLASS_ADD);
    sh_mb_free(&b);
    /* The same text byte for byte under another stem is a borrow: its text is
     * the game's, and the warning names its source. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\l1c3", tmp);
    l1c_mod(dir, "assets\\generated\\decls\\md6def\\mods\\t.l1c\\other.md6.decl", MD6_STOCK);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.warnings == 1 && b.stock_count == 1);
    CHECK(strstr(b.first_warning, "the game's own " D3_MD6 " names it too") != NULL);
    sh_mb_free(&b);
    /* A dead name only the mod introduces stays an error. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\l1c4", tmp);
    l1c_mod(dir, "assets\\generated\\decls\\md6def\\mods\\t.l1c\\m\\x.md6.decl",
            "{\r\n\tanim \"md6/m/dead.md6anim\" {\r\n\t}\r\n\tanim \"md6/m/mine.md6anim\" {\r\n\t}\r\n}\r\n");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b) && b.errors == 1 && b.warnings == 1);
    CHECK_CAUSE(b, "needs anim md6/m/mine.md6anim, which resolves to nothing");
    sh_mb_free(&b);
    /* With no stock decl of that stem, a dead clip is the mod's own: an error. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\l1c5", tmp);
    l1c_mod(dir, "assets\\generated\\decls\\md6def\\mods\\t.l1c\\m\\y.md6.decl",
            "{\r\n\tanim \"md6/m/dead.md6anim\" {\r\n\t\t// not a copy of y\r\n\t}\r\n}\r\n");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "needs anim md6/m/dead.md6anim, which resolves to nothing");
    sh_mb_free(&b);
}

/* ------------------------------------------ campaign-only closure ---- */
/* A fourth install (D4): an md6Def, its mesh and its skeleton only in
 * gameresources (the campaign archive SnapMap never loads), an image only in
 * snap_gameresources and one both archives hold. A mod's animWeb naming the
 * md6Def brings the chain along as pointers; names SnapMap loads on its own
 * add nothing; a name no archive holds still fails; the file cap fails with
 * the first name past it. */

#define K_MD6    "generated/decls/md6def/k/wall.md6.decl"
#define K_MESH   "generated/basemodel/k/wall.bmd6model"
#define K_SKL    "generated/skeleton/k/wall.bmd6skl"
#define K_SNAP   "generated/image/k/snap.bimage"
#define K_BOTH   "generated/image/k/both.bimage"
#define K_WEB    "assets\\generated\\decls\\animweb\\mods\\t.close\\station.decl"
static const char K_MD6_TEXT[] = "{\r\n\tmesh = \"k/wall.md6mesh\";\r\n\tskeleton = \"k/wall.md6skl\";\r\n"
                                 "\tanim \"k/gone.md6anim\" {\r\n\t}\r\n}\r\n";
/* The engine's own animWeb form, as designsystems/med_station_heal holds it. */
static const char K_WEB_TEXT[] = "{\r\n\tprops {\r\n\t\tmodelInfos {\r\n\t\t\t\"k/wall.md6\" \"\"\r\n\t\t}\r\n\t}\r\n}\r\n";
static unsigned char K_MESH_B[90], K_SKL_B[70], K_SNAP_B[50], K_BOTH_S[60], K_BOTH_G[60];

static void make_install4(const char *d4)
{
    size_t k;
    for (k = 0; k < sizeof K_MESH_B; k++) K_MESH_B[k] = (unsigned char)(k * 3 + 1);
    for (k = 0; k < sizeof K_SKL_B; k++) K_SKL_B[k] = (unsigned char)(k * 5 + 2);
    for (k = 0; k < sizeof K_SNAP_B; k++) K_SNAP_B[k] = (unsigned char)(k * 7 + 3);
    for (k = 0; k < sizeof K_BOTH_S; k++) { K_BOTH_S[k] = (unsigned char)(k * 9 + 4); K_BOTH_G[k] = (unsigned char)(k * 11 + 5); }
    {
        entry g[] = {
            { K_MD6,  (const unsigned char *)K_MD6_TEXT, sizeof K_MD6_TEXT - 1, 0, 0 },
            { K_MESH, K_MESH_B, sizeof K_MESH_B, 0, 0 },
            { K_SKL,  K_SKL_B, sizeof K_SKL_B, 1, 1 },
            { K_BOTH, K_BOTH_G, sizeof K_BOTH_G, 0, 0 },
        };
        entry s[] = {
            { K_SNAP, K_SNAP_B, sizeof K_SNAP_B, 0, 0 },
            { K_BOTH, K_BOTH_S, sizeof K_BOTH_S, 0, 0 },
        };
        write_container(d4, "gameresources", g, 4);
        write_container(d4, "snap_gameresources", s, 2);
    }
}

static int has_note(const sh_mb_bundle *b, const char *needle)
{
    size_t i;
    for (i = 0; i < b->note_count; i++) if (strstr(b->notes[i], needle)) return 1;
    return 0;
}

static void test_campaign_closure(const char *tmp)
{
    char d4[MAX_PATH * 2], dir[MAX_PATH * 2], cause[SH_MB_CAUSE_MAX], hash[65];
    sh_mb_bundle b;
    const sh_mb_file *f;
    const char *chain[] = { K_MD6, K_MESH, K_SKL };
    int i;

    _snprintf_s(d4, sizeof d4, _TRUNCATE, "%s\\d4", tmp);
    make_install4(d4);
    sh_modstock_set_install(d4);

    /* animWeb -> md6Def -> mesh and skeleton: the whole chain becomes
     * pointers at the game's own paths, surfaces carried by nothing; the
     * md6Def's clip no archive holds is the game's own hole, not the mod's. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\close1", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.close\",\"name\":\"Closure test\"}");
    mt_write(dir, K_WEB, K_WEB_TEXT);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.warnings == 0);
    if (b.errors) fprintf(stderr, "  closure: %s\n", b.first_cause);
    CHECK(b.file_count == 4 && b.stock_count == 3 && b.total_size == sizeof K_WEB_TEXT - 1);
    CHECK(b.stock_bytes == sizeof K_MD6_TEXT - 1 + sizeof K_MESH_B + sizeof K_SKL_B);
    for (i = 0; i < 3; i++) {
        f = find(&b, chain[i]);
        CHECK(f && f->stock && f->absent && f->closure && f->stock_source && !strcmp(f->stock_source, chain[i]) &&
              f->cls == SH_MB_CLASS_SURFACE);
    }
    f = find(&b, K_MD6);
    CHECK(f && f->need_count == 3);
    CHECK(has_note(&b, "3 game files borrowed for completeness") && has_note(&b, "(first " K_MD6 ")"));
    CHECK(b.canonical && strstr(b.canonical, "\"source\":\"" K_SKL "\"") != NULL);
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    memcpy(hash, b.hash, sizeof hash);
    sh_mb_free(&b);
    /* The product block records them as pointers with no file; a rebuild
     * carries them forward and keeps the hash and the note. */
    {
        size_t len = 0;
        unsigned char *text;
        char path[MAX_PATH * 2];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
        text = sh_mb_read_file(path, 1 << 20, &len);
        CHECK(text && strstr((const char *)text, "\"source\": \"" K_MESH "\"") && strstr((const char *)text, "\"inAssets\": false"));
        if (text) HeapFree(GetProcessHeap(), 0, text);
    }
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.warnings == 0 && !strcmp(b.hash, hash) && b.stock_count == 3);
    CHECK(has_note(&b, "3 game files borrowed for completeness"));
    sh_mb_free(&b);

    /* Names SnapMap loads on its own (snap_gameresources, or both archives)
     * gain nothing. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\close2", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.close\",\"name\":\"Closure test\"}");
    mt_write(dir, "assets\\generated\\decls\\material\\mods\\t.close\\m.decl",
             "{\r\n\tdiffusemap = \"k/snap\";\r\n\tspecularmap = \"k/both\";\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.file_count == 1 && b.stock_count == 0 && b.needs.stock == 2);
    CHECK(!has_note(&b, "borrowed for completeness"));
    sh_mb_free(&b);

    /* A name no archive holds still fails as before. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\close3", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.close\",\"name\":\"Closure test\"}");
    mt_write(dir, K_WEB, "{\r\n\tprops {\r\n\t\tmodelInfos {\r\n\t\t\t\"k/none.md6\" \"\"\r\n\t\t}\r\n\t}\r\n}\r\n");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b) && b.file_count == 1);
    CHECK_CAUSE(b, "needs md6Def k/none.md6, which resolves to nothing");
    sh_mb_free(&b);

    /* The file cap: 4095 declared pointers and the animWeb fill it, so the
     * first name the closure would add fails the build, by name. */
    {
        size_t cap = 96u * 4096u + 256u, w;
        char *desc = (char *)malloc(cap);
        unsigned n;
        CHECK(desc != NULL);
        if (desc) {
            w = (size_t)_snprintf_s(desc, cap, _TRUNCATE, "{\"id\":\"t.close\",\"name\":\"Closure test\",\"stock\":{");
            for (n = 0; n < SH_MB_MAX_FILES - 1; n++)
                w += (size_t)_snprintf_s(desc + w, cap - w, _TRUNCATE, "%s\"generated/image/mods/t.close/%04u.bimage\":"
                                         "{\"source\":\"" K_SNAP "\"}", n ? "," : "", n);
            _snprintf_s(desc + w, cap - w, _TRUNCATE, "}}");
            _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\close4", tmp);
            mt_write(dir, "mod.json", desc);
            mt_write(dir, K_WEB, K_WEB_TEXT);
            CHECK(!sh_mb_build(dir, &RESOLVER, &b));
            CHECK_CAUSE(b, "needs md6Def k/wall.md6, which only the campaign archives hold; bringing it along would take "
                           "the bundle past 4096 files");
            sh_mb_free(&b);
            free(desc);
        }
    }
}

/* The closure is derived from each build's needs: a pointer one build added
 * is gone after the decl that needed it is removed, and the hash changes. */
static void test_closure_recomputed(const char *tmp)
{
    char d4[MAX_PATH * 2], dir[MAX_PATH * 2], path[MAX_PATH * 2], cause[SH_MB_CAUSE_MAX], hash[65];
    sh_mb_bundle b;
    size_t len = 0;
    unsigned char *text;

    _snprintf_s(d4, sizeof d4, _TRUNCATE, "%s\\d4", tmp);
    sh_modstock_set_install(d4);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\close5", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
    mt_write(dir, "mod.json", "{\"id\":\"t.close\",\"name\":\"Closure test\"}");
    mt_write(dir, K_WEB, K_WEB_TEXT);
    mt_write(dir, "assets\\generated\\decls\\material\\mods\\t.close\\m.decl", "{\r\n\tdiffusemap = \"k/snap\";\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 3);
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    memcpy(hash, b.hash, sizeof hash);
    sh_mb_free(&b);
    /* The product block marks the derived pointers. */
    text = sh_mb_read_file(path, 1 << 20, &len);
    CHECK(text && strstr((const char *)text, "\"closure\": true") && strstr((const char *)text, K_MD6));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    /* An unchanged rebuild derives the same set: same hash and build. */
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 3 && !strcmp(b.hash, hash) && b.build == 1);
    CHECK(has_note(&b, "3 game files borrowed for completeness"));
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    sh_mb_free(&b);
    /* The animWeb is removed: nothing needs the chain, so it is gone. */
    mt_delete(dir, K_WEB);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.file_count == 1 && b.stock_count == 0 &&
          strcmp(b.hash, hash) != 0 && b.build == 2);
    CHECK(!find(&b, K_MD6) && !find(&b, K_MESH) && !find(&b, K_SKL) && !has_note(&b, "borrowed for completeness"));
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    sh_mb_free(&b);
    text = sh_mb_read_file(path, 1 << 20, &len);
    CHECK(text && !strstr((const char *)text, K_MD6) && !strstr((const char *)text, "\"closure\""));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    /* A folder built by rules 3, which wrote no mark: its pointers at the
     * game's own path to campaign-only content are derived again too. */
    mt_write(dir, K_WEB, K_WEB_TEXT);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 3);
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    sh_mb_free(&b);
    text = sh_mb_read_file(path, 1 << 20, &len);
    CHECK(text != NULL);
    if (text) {
        char *t3 = (char *)malloc(len + 1), *q;
        size_t n3 = 0, j;
        const char *mark = "\"closure\": true";
        CHECK(t3 != NULL);
        /* Cut each mark with the comma and line break before it. */
        for (j = 0; t3 && j < len; j++) {
            if (len - j >= strlen(mark) && !memcmp(text + j, mark, strlen(mark))) {
                while (n3 && t3[n3 - 1] != ',') n3--;
                if (n3) n3--;
                j += strlen(mark) - 1;
                continue;
            }
            t3[n3++] = (char)text[j];
        }
        if (t3) {
            char rules[32];
            t3[n3] = 0;
            _snprintf_s(rules, sizeof rules, _TRUNCATE, "\"rules\": %d", SH_MB_RULES);
            q = strstr(t3, rules);
            CHECK(q && !strstr(t3, "closure"));
            if (q) q[9] = '3';
            mt_write(dir, "mod.json", t3);
            free(t3);
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    mt_delete(dir, K_WEB);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 0 && !find(&b, K_MD6) && !find(&b, K_MESH));
    sh_mb_free(&b);
    /* An author stock entry is the author's: it stays when nothing needs it. */
    mt_write(dir, "mod.json", "{\"id\":\"t.close\",\"name\":\"Closure test\",\"stock\":{\"" K_MESH "\":{\"source\":\"" K_MESH "\"}}}");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 1 && find(&b, K_MESH) && !find(&b, K_MESH)->closure);
    CHECK(sh_mb_write(&b, dir, cause, sizeof cause));
    sh_mb_free(&b);
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 1 && find(&b, K_MESH));
    sh_mb_free(&b);
}

/* ------------------------------------- effects, movies and images ---- */
/* A fifth install (D5), everything in gameresources only unless noted: a
 * particle and a ribbon (a bare name, as the game names its ribbons), the
 * materials they draw with, one naming a transparency atlas; a movie and its
 * atlas; two virtual-textured materials whose source images have standalone
 * rows, one with an installed virtual-texture row; a particle SnapMap holds. */

#define E_PRT     "generated/decls/particle/k/eyes.decl"
#define E_RIB     "generated/decls/ribbon/k_trail.decl"
#define E_GLOW    "generated/decls/material/k/glow.decl"
#define E_TRAIL   "generated/decls/material/k/trailmat.decl"
#define E_ATLAS   "generated/transsortatlas/k/beam.bimage"
#define E_SWF     "generated/swf/k/panel.bswf"
#define E_SWFIMG  "generated/swf/k/panel.bimage"
#define E_VT      "generated/decls/material/k/vtmat.decl"
#define E_VTIMG   "generated/image/k/vtimg.bimage"
#define E_VT2     "generated/decls/material/k/vtmat2.decl"
#define E_VT2IMG  "generated/image/k/vtimg2.bimage"
#define E_SNAPPRT "generated/decls/particle/k/snapsparks.decl"
static const char E_PRT_TEXT[] = "{\r\n\tedit = {\r\n\t\tstages = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n"
                                 "\t\t\t\tsystemProperties = {\r\n\t\t\t\t\tmaterial = \"k/glow\";\r\n\t\t\t\t}\r\n"
                                 "\t\t\t}\r\n\t\t}\r\n\t}\r\n}\r\n";
static const char E_RIB_TEXT[] = "{\r\n\tedit = {\r\n\t\tmaterial = \"k/trailmat\";\r\n\t\tduration = 1000;\r\n\t}\r\n}\r\n";
static const char E_GLOW_TEXT[] = "{\r\nstageprogram\ttranssortblend\r\ntransatlasmap\t\"k/beam.tga\"\r\n}\r\n";
static const char E_TRAIL_TEXT[] = "{\r\nstageprogram\tca_ribbonblend\r\n}\r\n";
static const char E_VT_TEXT[] = "{\r\ndiffusemap\t\"k/vtimg\"\r\nusevirtualmapping\t1.000000\r\n}\r\n";
static const char E_VT2_TEXT[] = "{\r\ndiffusemap\t\"k/vtimg2\"\r\nusevirtualmapping\t1.000000\r\n}\r\n";
static const char E_VMTR[] = "26\t// file format version\r\n2\t// num vmtrs\r\n"
                             "//   x      y width  hght flags  timeStamp    mtrCheck\r\n"
                             "     0      0  1920  1920     0          0   -46041280 \"models/other\"\r\n"
                             "  1920      0  1920  1920     0          0  1117842188 \"k/vtmat\"\r\n";
static unsigned char E_ATLAS_B[80], E_SWF_B[40], E_SWFIMG_B[60], E_VTIMG_B[50], E_VT2IMG_B[52];

static void make_install5(const char *d5)
{
    size_t k;
    for (k = 0; k < sizeof E_ATLAS_B; k++) E_ATLAS_B[k] = (unsigned char)(k * 3 + 7);
    for (k = 0; k < sizeof E_SWF_B; k++) E_SWF_B[k] = (unsigned char)(k * 5 + 1);
    for (k = 0; k < sizeof E_SWFIMG_B; k++) E_SWFIMG_B[k] = (unsigned char)(k * 9 + 2);
    for (k = 0; k < sizeof E_VTIMG_B; k++) E_VTIMG_B[k] = (unsigned char)(k * 11 + 3);
    for (k = 0; k < sizeof E_VT2IMG_B; k++) E_VT2IMG_B[k] = (unsigned char)(k * 13 + 4);
    {
        entry g[] = {
            { E_PRT,    (const unsigned char *)E_PRT_TEXT, sizeof E_PRT_TEXT - 1, 0, 0 },
            { E_RIB,    (const unsigned char *)E_RIB_TEXT, sizeof E_RIB_TEXT - 1, 0, 0 },
            { E_GLOW,   (const unsigned char *)E_GLOW_TEXT, sizeof E_GLOW_TEXT - 1, 0, 0 },
            { E_TRAIL,  (const unsigned char *)E_TRAIL_TEXT, sizeof E_TRAIL_TEXT - 1, 0, 0 },
            { E_ATLAS,  E_ATLAS_B, sizeof E_ATLAS_B, 0, 0 },
            { E_SWF,    E_SWF_B, sizeof E_SWF_B, 0, 0 },
            { E_SWFIMG, E_SWFIMG_B, sizeof E_SWFIMG_B, 0, 0 },
            { E_VT,     (const unsigned char *)E_VT_TEXT, sizeof E_VT_TEXT - 1, 0, 0 },
            { E_VTIMG,  E_VTIMG_B, sizeof E_VTIMG_B, 0, 0 },
            { E_VT2,    (const unsigned char *)E_VT2_TEXT, sizeof E_VT2_TEXT - 1, 0, 0 },
            { E_VT2IMG, E_VT2IMG_B, sizeof E_VT2IMG_B, 0, 0 },
        };
        entry s[] = { { E_SNAPPRT, (const unsigned char *)E_PRT_TEXT, sizeof E_PRT_TEXT - 1, 0, 0 } };
        write_container(d5, "gameresources", g, (int)(sizeof g / sizeof g[0]));
        write_container(d5, "snap_gameresources", s, 1);
    }
    put_rel(d5, "virtualtextures\\_vmtr.vmtr", E_VMTR, sizeof E_VMTR - 1);
}

static void test_closure_effects(const char *tmp)
{
    char d5[MAX_PATH * 2], dir[MAX_PATH * 2];
    sh_mb_bundle b;
    const sh_mb_file *f;
    const char *fx_added[] = { E_PRT, E_RIB, E_GLOW, E_TRAIL, E_ATLAS };
    int i;

    _snprintf_s(d5, sizeof d5, _TRUNCATE, "%s\\d5", tmp);
    make_install5(d5);
    sh_modstock_set_install(d5);

    /* An effect naming a campaign-only particle and ribbon gains them, the
     * materials they draw with and the transparency atlas one names; a
     * particle SnapMap holds adds nothing. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\fx1", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.fx\",\"name\":\"Effect test\"}");
    mt_write(dir, "assets\\generated\\decls\\fx\\mods\\t.fx\\glow.decl",
             "{\r\n\tedit = {\r\n\t\tevents = {\r\n\t\t\tnum = 3;\r\n"
             "\t\t\titem[0] = {\r\n\t\t\t\ttype = \"FX_PARTICLE\";\r\n\t\t\t\tdeclPrt = \"k/eyes\";\r\n\t\t\t}\r\n"
             "\t\t\titem[1] = {\r\n\t\t\t\ttype = \"FX_RIBBON\";\r\n\t\t\t\tdeclRibbon = \"k_trail\";\r\n\t\t\t}\r\n"
             "\t\t\titem[2] = {\r\n\t\t\t\ttype = \"FX_PARTICLE\";\r\n\t\t\t\tdeclPrt = \"k/snapsparks\";\r\n\t\t\t}\r\n"
             "\t\t}\r\n\t}\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0);
    if (b.errors) fprintf(stderr, "  effects: %s\n", b.first_cause);
    CHECK(b.stock_count == 5 && b.file_count == 6);
    for (i = 0; i < 5; i++) {
        f = find(&b, fx_added[i]);
        CHECK(f && f->stock && f->absent && f->closure && !strcmp(f->stock_source, fx_added[i]));
    }
    CHECK(!find(&b, E_SNAPPRT) && has_note(&b, "5 game files borrowed for completeness"));
    sh_mb_free(&b);

    /* An entityDef whose guiData names a campaign-only movie gains the cooked
     * movie and its atlas. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\swf1", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.fx\",\"name\":\"Movie test\"}");
    mt_write(dir, "assets\\generated\\decls\\entitydef\\mods\\t.fx\\panel.decl",
             "{\r\n\tedit = {\r\n\t\tguiData = {\r\n\t\t\tswfFile = \"swf/k/panel.swf\";\r\n\t\t}\r\n\t}\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.stock_count == 2);
    f = find(&b, E_SWF);
    CHECK(f && f->closure && f->ptype && !strcmp(f->ptype, "swf"));
    f = find(&b, E_SWFIMG);
    CHECK(f && f->closure);
    sh_mb_free(&b);

    /* A virtual-textured material with an installed row gains no image (the
     * engine samples its pages); one without a row gains its image. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\vt1", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.fx\",\"name\":\"Virtual texture test\"}");
    mt_write(dir, "assets\\generated\\decls\\ribbon\\mods\\t.fx\\a.decl", "{\r\n\tedit = {\r\n\t\tmaterial = \"k/vtmat\";\r\n\t}\r\n}\r\n");
    mt_write(dir, "assets\\generated\\decls\\ribbon\\mods\\t.fx\\b.decl", "{\r\n\tedit = {\r\n\t\tmaterial = \"k/vtmat2\";\r\n\t}\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0);
    CHECK(find(&b, E_VT) && find(&b, E_VT2) && !find(&b, E_VTIMG) && find(&b, E_VT2IMG) && b.stock_count == 3);
    sh_mb_free(&b);

    /* A particle no archive holds is reported, not refused. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\fx2", tmp);
    mt_write(dir, "mod.json", "{\"id\":\"t.fx\",\"name\":\"Effect test\"}");
    mt_write(dir, "assets\\generated\\decls\\fx\\mods\\t.fx\\gone.decl",
             "{\r\n\tedit = {\r\n\t\tevents = {\r\n\t\t\tnum = 1;\r\n"
             "\t\t\titem[0] = {\r\n\t\t\t\tdeclPrt = \"k/none\";\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}\r\n");
    CHECK(sh_mb_build(dir, &RESOLVER, &b) && b.errors == 0 && b.warnings == 1 && b.stock_count == 0);
    CHECK(strstr(b.first_warning, "names particle k/none, which no archive holds") != NULL);
    sh_mb_free(&b);
}

/* ---------------------------------------------- closure, store path ---- */
/* A sixth install (D6), the campaign archive: a door model (an md6Def
 * naming only its mesh, the mesh whose cooked header names its skeleton, the
 * skeleton and its default clip) and the door's animWeb, which a hand-made
 * folder carries as its only file (a surface). */

#define M6_WEB  "generated/decls/animweb/k6/door_web.decl"
#define M6_MD6  "generated/decls/md6def/k6/door.md6.decl"
#define M6_MESH "generated/basemodel/k6/door.bmd6model"
#define M6_SKL  "generated/skeleton/k6/door.bmd6skl"
#define M6_DEF  "cooked/anim/k6/door.md6skl/_default.bmd6anim"
static const char M6_WEB_TEXT[] = "{\r\n\tprops {\r\n\t\tmodelInfos {\r\n\t\t\t\"k6/door.md6\" \"\"\r\n\t\t}\r\n\t}\r\n}\r\n";
static const char M6_MD6_TEXT[] = "{\r\n\tinit {\r\n\t\tmesh \"k6/door.md6mesh\"\r\n\t}\r\n}\r\n";
static unsigned char M6_MESH_B[64], M6_SKL_B[40], M6_DEF_B[36], OTHER6_B[8];

/* A cooked mesh header: magic and version, two timestamps, the skeleton name
 * as a little-endian u32 length and bytes, then geometry. */
static void m6_mesh(unsigned char *d, size_t n, const char *skeleton)
{
    size_t k, s = strlen(skeleton);
    for (k = 0; k < n; k++) d[k] = (unsigned char)(k * 13 + 7);
    d[0] = 0x2b; d[1] = 0x02; d[2] = 'M'; d[3] = 'M';
    d[12] = (unsigned char)s; d[13] = d[14] = d[15] = 0;
    memcpy(d + 16, skeleton, s);
}

static void make_install6(const char *d6)
{
    size_t k;
    m6_mesh(M6_MESH_B, sizeof M6_MESH_B, "k6/door.md6skl");
    for (k = 0; k < sizeof M6_SKL_B; k++) M6_SKL_B[k] = (unsigned char)(k * 3 + 9);
    for (k = 0; k < sizeof M6_DEF_B; k++) M6_DEF_B[k] = (unsigned char)(k * 5 + 1);
    for (k = 0; k < sizeof OTHER6_B; k++) OTHER6_B[k] = (unsigned char)(k * 7 + 2);
    {
        entry g[] = {
            { M6_WEB,  (const unsigned char *)M6_WEB_TEXT, sizeof M6_WEB_TEXT - 1, 0, 0 },
            { M6_MD6,  (const unsigned char *)M6_MD6_TEXT, sizeof M6_MD6_TEXT - 1, 0, 0 },
            { M6_MESH, M6_MESH_B, sizeof M6_MESH_B, 0, 0 },
            { M6_SKL,  M6_SKL_B, sizeof M6_SKL_B, 1, 0 },
            { M6_DEF,  M6_DEF_B, sizeof M6_DEF_B, 0, 1 },
        };
        write_container(d6, "gameresources", g, 5);
        write_container(d6, "snap_gameresources", g, 0);
    }
}

/* The store's own build (the rescan's, in place, with every installed bundle
 * in its resolver) derives the same closure as a bare build: another mod
 * pointing at the same campaign files is served only where a map declares
 * it, so it does not stand in for this one's pointers (V2: the SP doors
 * folder's live rebuild wrote none). A hand-made folder whose only file is a
 * surface (the game's own animWeb) is built by the rescan with its pointers,
 * and an unchanged rebuild, with its own earlier build installed, keeps them. */
static void test_closure_store_path(const char *tmp)
{
    char d6[MAX_PATH * 2], store[MAX_PATH * 2], other[MAX_PATH * 2], surf[MAX_PATH * 2], key[SH_MS_KEY_MAX + 1];
    char cause[SH_MB_CAUSE_MAX], hash[65];
    const char *chain[] = { M6_MD6, M6_MESH };
    sh_ms_install_stats st;
    sh_ms_sync_stats ss;
    sh_mb_bundle b;
    const sh_mb_file *f;
    int i;

    _snprintf_s(d6, sizeof d6, _TRUNCATE, "%s\\d6", tmp);
    make_install6(d6);
    sh_modstock_set_install(d6);
    _snprintf_s(store, sizeof store, _TRUNCATE, "%s\\store6", tmp);
    _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\other", store);
    _snprintf_s(surf, sizeof surf, _TRUNCATE, "%s\\surf", store);
    CHECK(sh_ms_set_root(store));
    /* An installed mod that already points at the door's model. */
    mt_write(other, "mod.json", "{\"id\":\"t.other6\",\"name\":\"Other\",\"stock\":{"
             "\"" M6_MD6 "\":{\"source\":\"" M6_MD6 "\"},\"" M6_MESH "\":{\"source\":\"" M6_MESH "\"},"
             "\"" M6_SKL "\":{\"source\":\"" M6_SKL "\"},\"" M6_DEF "\":{\"source\":\"" M6_DEF "\"}}}");
    put_rel(other, "assets/generated/image/mods/t.other6/own.bimage", OTHER6_B, sizeof OTHER6_B);
    CHECK(sh_ms_install_from_folder(other, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    if (!key[0]) fprintf(stderr, "  other refused: %s\n", cause);
    /* The hand-made folder: the game's animWeb, byte for byte. */
    mt_write(surf, "mod.json", "{\"id\":\"t.surf6\",\"name\":\"Surface\"}");
    put_rel(surf, "assets\\" M6_WEB, M6_WEB_TEXT, sizeof M6_WEB_TEXT - 1);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    CHECK(sh_mb_load_folder(surf, &b) && b.rules == SH_MB_RULES && b.file_count == 3 && b.stock_count == 3);
    f = find(&b, M6_WEB);
    CHECK(f && f->stock && f->cls == SH_MB_CLASS_SURFACE && !f->closure);
    for (i = 0; i < 2; i++) {
        f = find(&b, chain[i]);
        CHECK(f && f->stock && f->closure && f->stock_source && !strcmp(f->stock_source, chain[i]));
    }
    memcpy(hash, b.hash, sizeof hash);
    sh_mb_free(&b);
    /* A second rescan finds it current; a forced rebuild in place, with its
     * own earlier build installed, derives the same set and hash. */
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.failed == 0);
    CHECK(sh_ms_install_from_folder(surf, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    CHECK(b.stock_count == 3 && !strcmp(b.hash, hash) && find(&b, M6_MESH) && find(&b, M6_MESH)->closure);
    sh_mb_free(&b);
    sh_ms_set_root(NULL);
}

/* ------------------------------------------------- store, sync, serve ---- */

typedef struct blob_src { int calls; } blob_src;
static int serve_blob(void *ctx, const char *sha, unsigned long long size, unsigned char **bytes, size_t *len,
                      char *cause, size_t cap)
{
    blob_src *s = (blob_src *)ctx;
    char h[65];
    (void)cap; (void)cause;
    s->calls++;
    hex_of(OWN, sizeof OWN, h);
    if (strcmp(sha, h) != 0 || size != sizeof OWN) return 0;
    *bytes = (unsigned char *)malloc(sizeof OWN);
    memcpy(*bytes, OWN, sizeof OWN);
    *len = sizeof OWN;
    return 1;
}

/* Files below `dir`, recursively. */
static int count_files(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], sub[MAX_PATH * 2];
    HANDLE h;
    int n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            n += count_files(sub);
        } else n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void test_store(const char *tmp, const char *d1, const char *d2)
{
    char dir[MAX_PATH * 2], store[MAX_PATH * 2], store2[MAX_PATH * 2], store3[MAX_PATH * 2], path[MAX_PATH * 2];
    char key[SH_MS_KEY_MAX + 1], key2[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX], url[64], hash[65];
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    sh_mb_bundle b;
    unsigned char *bytes = NULL;
    size_t len = 0;
    sh_modserve_hit hit;
    modsync_stats ss;

    sh_modstock_set_install(d1);
    _snprintf_s(store, sizeof store, _TRUNCATE, "%s\\store", tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\src1", tmp);
    make_mod(dir, ALIAS_JSON);
    CHECK(sh_ms_set_root(store));
    CHECK(sh_ms_install_from_folder(dir, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    if (!key[0]) fprintf(stderr, "  install refused: %s\n", cause);
    CHECK(st.stock_files == 4 && st.copied == 1);
    strcpy_s(hash, sizeof hash, b.hash);
    sh_mb_free(&b);
    /* The store folder holds the one carried file; the pointers are in its mod.json only. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets", store, key);
    CHECK(count_files(path) == 1);
    {
        char *text;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", store, key);
        text = (char *)sh_mb_read_file(path, 1u << 20, NULL);
        CHECK(text && strstr(text, "\"inAssets\": false") && strstr(text, "\"source\": \"" P_IMAGE "\""));
        free(text);
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\" REQ_COPY, store, key);
    { char *q; for (q = path; *q; q++) if (*q == '/') *q = '\\'; }
    CHECK(!mt_exists(path));
    CHECK(sh_ms_verify(key, &vs, cause, sizeof cause) && vs.files == 5 && vs.pointers == 4);

    /* Serving: the pointer is answered with the installed bytes. */
    CHECK(sh_modserve_index_build(store) == 1);
    CHECK(sh_modserve_lookup(REQ_COPY, &hit) && hit.is_stock && !strcmp(hit.stock_source, P_IMAGE));
    CHECK(sh_modserve_open(REQ_COPY, &bytes, &len) == 1 && len == sizeof IMG_B && !memcmp(bytes, IMG_B, len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open(REQ_ALIAS, &bytes, &len) == 1 && len == sizeof DECL_A - 1 && !memcmp(bytes, DECL_A, len));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_open(REQ_OWN, &bytes, &len) == 1 && len == sizeof OWN);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    CHECK(sh_modserve_lookup(REQ_OWN, &hit) && !hit.is_stock);

    /* A second mod carrying the same bytes at the shadowed path (built
     * without the reader, so it is carried) is the same file, not a conflict. */
    {
        char other[MAX_PATH * 2];
        _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\src2", tmp);
        mt_write(other, "mod.json", "{\"id\":\"t.other\",\"name\":\"Other\",\"shadows\":[\"" P_IMAGE "\"]}");
        put_rel(other, "assets\\" P_IMAGE, IMG_B, sizeof IMG_B);
        sh_mb_set_stock_ops(NULL);
        sh_mb_keep_replacements_here(1);   /* carried at a game name: a replacement */
        CHECK(sh_ms_install_from_folder(other, &RESOLVER, key2, sizeof key2, &st, NULL, cause, sizeof cause));
        sh_mb_keep_replacements_here(0);
        sh_modstock_register();
        CHECK(st.stock_files == 0 && st.copied == 1);
        CHECK(sh_modserve_index_build(store) == 2);
        CHECK(!sh_modserve_conflict_info(P_IMAGE, NULL));
        CHECK(sh_modserve_open(P_IMAGE, &bytes, &len) == 1 && len == sizeof IMG_B && !memcmp(bytes, IMG_B, len));
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    }

    /* Another revision: verification fails with the first cause, and the
     * pointer it cannot answer falls through to the engine. */
    sh_modstock_set_install(d2);
    CHECK(!sh_ms_verify(key, &vs, cause, sizeof cause));
    CHECK(strstr(cause, "assets/" REQ_ALIAS " points at installed content " P_DECL ", which this game does not have") != NULL);
    CHECK(sh_modserve_index_build(store) == 1);   /* the failed verification is recorded in mod.json */
    sh_modstock_set_install(d1);
    CHECK(sh_ms_verify(key, &vs, cause, sizeof cause));
    CHECK(sh_modserve_index_build(store) == 2);
    sh_modstock_set_install(d2);
    CHECK(sh_modserve_open(REQ_COPY, &bytes, &len) == -1);
    CHECK(strstr(g_log, "points at installed content 'generated/image/stock/b.bimage'") != NULL);
    sh_modstock_set_install(d1);

    /* The store client: upload sends only the carried blob. */
    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(modnet_set_base_url(url));
    {
        const char *one[1];
        char own[65], img[65];
        one[0] = hash;
        hex_of(OWN, sizeof OWN, own);
        hex_of(IMG_B, sizeof IMG_B, img);
        CHECK(modsync_upload_bundles(one, 1, "map-stock", NULL, &ss));
        CHECK(ss.blobs_total == 1 && ss.stock_files == 4 && stub_has_blob(own) && !stub_has_blob(img) && stub_has_bundle(hash));
    }
    /* Download into an empty store: one blob fetched, pointers resolved. */
    _snprintf_s(store2, sizeof store2, _TRUNCATE, "%s\\store2", tmp);
    CHECK(sh_ms_set_root(store2));
    CHECK(sh_modserve_index_build(store2) == 0);
    CHECK(modsync_fetch_bundle(hash, NULL, &ss));
    if (ss.code != MODNET_OK) fprintf(stderr, "  fetch: %s\n", ss.cause);
    CHECK(ss.blobs_transferred == 1 && ss.blobs_total == 1);
    if (ss.blobs_transferred != 1 || ss.blobs_total != 1)
        fprintf(stderr, "  fetch: %u of %u transferred, already %d, key %s\n", ss.blobs_transferred, ss.blobs_total,
                ss.already_installed, ss.key);
    CHECK(sh_modserve_bundle_state(hash, NULL, 0) == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets", store2, ss.key);
    CHECK(count_files(path) == 1);
    /* A player on another revision: refused before anything is fetched. */
    _snprintf_s(store3, sizeof store3, _TRUNCATE, "%s\\store3", tmp);
    CHECK(sh_ms_set_root(store3));
    sh_modstock_set_install(d2);
    {
        int gets = g_stub.blob_gets;
        CHECK(!modsync_fetch_bundle(hash, NULL, &ss));
        CHECK(strstr(ss.cause, "refused: assets/" REQ_ALIAS " points at installed content") != NULL);
        CHECK(g_stub.blob_gets == gets);
    }
    {
        /* The same through the install core directly: the source is never asked. */
        blob_src bs = { 0 };
        modsync_install_info info;
        sh_ms_entry *e = NULL;
        sh_ms_list l;
        size_t i;
        CHECK(sh_ms_set_root(store));
        CHECK(sh_ms_enumerate(&l));
        for (i = 0; i < l.count; i++) if (!strcmp(l.items[i].bundle.hash, hash)) e = &l.items[i];
        CHECK(e != NULL);
        CHECK(sh_ms_set_root(store3));
        if (e) {
            CHECK(!modsync_install_manifest(e->bundle.canonical, e->bundle.canonical_len, hash, serve_blob, &bs, &info, cause, sizeof cause));
            CHECK(bs.calls == 0 && strstr(cause, "refused:") != NULL);
            sh_modstock_set_install(d1);
            CHECK(modsync_install_manifest(e->bundle.canonical, e->bundle.canonical_len, hash, serve_blob, &bs, &info, cause, sizeof cause));
            CHECK(bs.calls == 1 && info.blobs_fetched == 1 && info.stock_files == 4);
        }
        sh_ms_list_free(&l);
    }
    stub_stop();
    sh_ms_set_root(NULL);
    sh_modstock_set_install(d1);
}

/* --------------------------------------------------------- measurement ---- */

static void measure(const char *data, const char *doom)
{
    LARGE_INTEGER fq, t0, t1;
    sh_modstock_stats ss;
    sh_ms_list l;
    size_t i;
    QueryPerformanceFrequency(&fq);
    sh_modstock_set_install(doom);
    QueryPerformanceCounter(&t0);
    CHECK(sh_modstock_exists("generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl") == 1);
    QueryPerformanceCounter(&t1);
    sh_modstock_get_stats(&ss);
    printf("modstock_test: measure: %s: index %u rows, %llu KiB held, loaded in %.1f ms (first question %.1f ms)\n", doom,
           ss.rows, ss.table_bytes >> 10, ss.load_ms, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
    printf("modstock_test: measure: %-36s %6s %8s %12s %12s %12s %9s\n", "folder", "files", "pointers", "bytes", "not stored",
           "hashed", "ms");
    sh_ms_set_root(data);
    if (!sh_ms_enumerate(&l)) memset(&l, 0, sizeof l);
    for (i = 0; i < l.count; i++) {
        const sh_mb_bundle *b = &l.items[i].bundle;
        size_t k;
        unsigned ptr = 0, same = 0, renamed = 0;
        unsigned long long bytes = 0, kept = 0, hashed0;
        if (!l.items[i].loaded) { printf("modstock_test: measure: %s: not loadable: %s\n", l.items[i].name, l.items[i].cause); continue; }
        sh_modstock_get_stats(&ss);
        hashed0 = ss.bytes_hashed;
        QueryPerformanceCounter(&t0);
        for (k = 0; k < b->file_count; k++) {
            const sh_mb_file *f = &b->files[k];
            char src[SH_MB_REQUEST_MAX + 1];
            if (f->stock) { ptr++; continue; }
            bytes += f->size;
            if (sh_modstock_match(f->request, f->size, f->blob, src, sizeof src) == SH_MB_STOCK_OK) {
                ptr++;
                kept += f->size;
                if (!strcmp(src, f->request)) same++;
                else renamed++;
            }
        }
        QueryPerformanceCounter(&t1);
        sh_modstock_get_stats(&ss);
        printf("modstock_test: measure: %-36s %6zu %8u %12llu %12llu %12llu %9.1f  (same path %u, renamed %u)\n", l.items[i].name,
               b->file_count, ptr, bytes, kept, ss.bytes_hashed - hashed0,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, same, renamed);
    }
    sh_ms_list_free(&l);
    sh_ms_set_root(NULL);
    sh_modstock_set_install(NULL);
}

int main(void)
{
    char tmp[MAX_PATH], d1[MAX_PATH * 2], d2[MAX_PATH * 2], data[MAX_PATH * 2], doom[MAX_PATH * 2];
    DWORD n;
    CHECK(mt_temp_dir("modstock", tmp, sizeof tmp));
    _snprintf_s(d1, sizeof d1, _TRUNCATE, "%s\\d1", tmp);
    _snprintf_s(d2, sizeof d2, _TRUNCATE, "%s\\d2", tmp);
    make_installs(d1, d2);
    sh_modstock_register();
    test_bridge(d1);
    test_builder(tmp);
    test_renamed_names_dead(tmp);
    test_campaign_closure(tmp);
    test_closure_recomputed(tmp);
    test_closure_effects(tmp);
    test_closure_store_path(tmp);
    sh_modstock_set_install(d1);
    test_store(tmp, d1, d2);
    sh_modstock_reset();
    mt_remove_tree(tmp);

    n = GetEnvironmentVariableA("SH_MODSTOCK_DATA", data, sizeof data);
    if (n && n < sizeof data) {
        n = GetEnvironmentVariableA("SH_MODSTOCK_DOOM", doom, sizeof doom);
        if (!n || n >= sizeof doom) strcpy_s(doom, sizeof doom, "D:\\SteamLibrary\\steamapps\\common\\DOOM");
        measure(data, doom);
    } else {
        printf("modstock_test: measurement skipped (set SH_MODSTOCK_DATA to a mods store, SH_MODSTOCK_DOOM to the install)\n");
    }
    sh_mb_set_stock_ops(NULL);
    if (g_failed) { fprintf(stderr, "modstock_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("modstock_test: bridge, builder, descriptor pointers, store, upload, download, serving and revision refusals pass (%d checks)\n",
           g_checks);
    return 0;
}
