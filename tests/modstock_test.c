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
    f = find(&b, P_IMAGE);
    CHECK(f && f->stock && f->shadowed);
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
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod4", tmp);   /* declared over a carried file with other bytes */
    make_mod(dir, ",\"stock\":{\"" REQ_OWN "\":{\"source\":\"" P_IMAGE "\"}}");
    CHECK(!sh_mb_build(dir, &RESOLVER, &b));
    CHECK_CAUSE(b, "is carried with other bytes");
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

    /* Without a reader: nothing becomes a pointer and a declared one is refused. */
    sh_mb_set_stock_ops(NULL);
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
        CHECK(sh_ms_install_from_folder(other, &RESOLVER, key2, sizeof key2, &st, NULL, cause, sizeof cause));
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
