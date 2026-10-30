/* modui_test.c -- the Mods tab's backend (modui.c) against the real store,
 * serving index and map manifest in a temporary folder: the document's shape
 * on an empty store, an install through the tab's action, one row per mod
 * folder (two folders declaring one id both listed as duplicates, a package a
 * map delivered listed like any other), "installed" and "attention" with their
 * causes, the Migrate state and action with the slot's WAIT, Remove (a folder
 * the product made deleted, the author's own folder moved to removed-mods, a
 * mod the open map uses refused), the open map's manifest rows and placed
 * closure, the conservative removal of covered builds, `sh_mods doctor` (every
 * bundle with its state, conflicts with their winners, shadows, duplicate ids,
 * migration, verification), the main-thread refusal, the slot's size query,
 * and that every key the page's fixture uses is a key the backend writes.
 *
 *   modui_test.exe <tests\fixtures\mods\alex.r2-imp> <tests\fixtures\mods_tab_payload.json>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapmap_plus_iface.h"
#include "modui.h"
#include "modstore.h"
#include "modserve.h"
#include "modmanifest.h"
#include "mod_test_util.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static int g_echo;
void backend_log(const char *message) { if (g_echo && message) printf("    %s\n", message); }

/* The map manifest's load step activates bundles through the serving layer's
 * engine half; the tab only reads the record it keeps. */
static int g_activations;
int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    if (n > 0) g_activations++;
    return 0;
}

/* ---- engine stand-ins ------------------------------------------------------------------------ */

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
static int fake_stock(const char *request)
{
    int i;
    for (i = 0; STOCK[i]; i++) if (!strcmp(STOCK[i], request)) return 1;
    return 0;
}

static char g_root[MAX_PATH];
static int g_rescans;
static int fake_rescan(void) { g_rescans++; return sh_modserve_index_build(g_root); }

static int g_palette_result = 1, g_palette_calls;
static int fake_palette(void) { g_palette_calls++; return g_palette_result; }

static int g_main = 1;
static int fake_on_main(void) { return g_main; }

static const char *g_placed[8];
static int g_placed_count = -1;   /* -1: no editor map */
static int fake_placed(sh_modui_name_fn emit, void *context)
{
    int i;
    if (g_placed_count < 0) return -1;
    for (i = 0; i < g_placed_count; i++) {
        emit(context, g_placed[i]);
        emit(context, g_placed[i]);   /* duplicates collapse */
    }
    return g_placed_count;
}

/* The overrides migration: what it reports pending, and a worker that stays
 * busy for a number of polls after it starts. */
static int g_mig_pending = 0, g_mig_entries = 0, g_mig_started, g_mig_again = -1, g_mig_start_result = 1;
static volatile LONG g_mig_busy_polls;
static int fake_mig_pending(unsigned *entries) { if (entries) *entries = (unsigned)g_mig_entries; return g_mig_pending; }
static int fake_mig_start(int again)
{
    g_mig_again = again;
    if (g_mig_start_result > 0) { g_mig_started++; InterlockedExchange(&g_mig_busy_polls, 3); g_mig_pending = 0; }
    return g_mig_start_result;
}
static int fake_mig_busy(void) { return InterlockedDecrement(&g_mig_busy_polls) >= 0 ? 1 : 0; }

static void bind_fakes(void)
{
    sh_modui_engine e;
    memset(&e, 0, sizeof e);
    e.on_main_thread = fake_on_main;
    e.placed_names = fake_placed;
    e.rescan = fake_rescan;
    e.palette_rebuild = fake_palette;
    e.stock_exists = fake_stock;
    e.migrate_pending = fake_mig_pending;
    e.migrate_start = fake_mig_start;
    e.migrate_busy = fake_mig_busy;
    sh_modui_bind(&e);
}

/* sh_modui_doctor's lines, one per line. */
static char g_doctor[1 << 16];
static void doctor_line(void *context, const char *line)
{
    size_t at = strlen(g_doctor), n = strlen(line);
    (void)context;
    if (g_echo) printf("    %s\n", line);
    if (at + n + 2 < sizeof g_doctor) { memcpy(g_doctor + at, line, n); g_doctor[at + n] = '\n'; g_doctor[at + n + 1] = 0; }
}
static const char *doctor(int verify)
{
    g_doctor[0] = 0;
    return sh_modui_doctor(verify, doctor_line, NULL) == 1 ? g_doctor : NULL;
}

/* ---- document access ------------------------------------------------------------------------- */

static char *fetch(void)
{
    int needed = 0;
    unsigned gen = 0;
    char *buf;
    if (sh_modui_fetch(NULL, 0, &needed, &gen) != -1 || needed <= 0) return NULL;
    buf = (char *)malloc((size_t)needed + 1);
    if (!buf) return NULL;
    if (sh_modui_fetch(buf, needed + 1, &needed, &gen) != 1) { free(buf); return NULL; }
    return buf;
}

static char *list_doc(void)
{
    if (sh_modui_list() < 0) return NULL;
    return fetch();
}

static char *run_doc(int action, const char *arg)
{
    if (sh_modui_run(action, arg) < 0) return NULL;
    return fetch();
}

static int has(const char *doc, const char *needle) { return doc && strstr(doc, needle) != NULL; }

/* The JSON object of the row whose key is `key` inside doc's "mods". */
static int bundle_has(const char *doc, const char *key, const char *needle)
{
    char k[256];
    const char *at, *end;
    if (!doc) return 0;
    _snprintf_s(k, sizeof k, _TRUNCATE, "{\"key\":\"%s\"", key);
    at = strstr(doc, k);
    if (!at) return 0;
    end = strchr(at, '}');
    if (!end) return 0;
    {
        size_t n = (size_t)(end - at);
        char *one = (char *)malloc(n + 1);
        int r;
        if (!one) return 0;
        memcpy(one, at, n);
        one[n] = 0;
        r = strstr(one, needle) != NULL;
        free(one);
        return r;
    }
}

static unsigned count_of(const char *doc, const char *needle)
{
    unsigned n = 0;
    const char *p = doc;
    size_t len = strlen(needle);
    while (p && (p = strstr(p, needle)) != NULL) { n++; p += len; }
    return n;
}

/* Every "key": the page's fixture uses must be written by the backend. */
static void keys_match_fixture(const char *fixture_path, const char *doc)
{
    size_t len = 0;
    unsigned char *text = sh_mb_read_file(fixture_path, 1u << 20, &len);
    const char *p;
    unsigned seen = 0, missing = 0;
    CHECK(text != NULL);
    if (!text) return;
    for (p = (const char *)text; (p = strchr(p, '"')) != NULL;) {
        const char *q = strchr(p + 1, '"');
        char key[80];
        size_t n;
        if (!q) break;
        n = (size_t)(q - p - 1);
        if (q[1] == ':' && n > 0 && n < sizeof key - 3) {
            char probe[90];
            memcpy(key, p + 1, n);
            key[n] = 0;
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "\"%s\":", key);
            seen++;
            if (!strstr(doc, probe)) { fprintf(stderr, "  fixture key %s is not in the backend document\n", key); missing++; }
        }
        p = q + 1;
    }
    printf("  fixture keys checked: %u, missing: %u\n", seen, missing);
    CHECK(seen > 25 && missing == 0);
    free(text);
}

/* ---- maps ------------------------------------------------------------------------------------ */

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alex.r2-imp/imp_copy\","
    "\"name\":\"\",\"state\":{}}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}}";

/* Load a map whose manifest names `hash` (a real bundle or not). */
static void load_map_naming(const char *id, const char *hash)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    char err[256], *text, *loaded;
    size_t tl = 0, ll = 0;
    memset(&rows, 0, sizeof rows);
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, id);
    strcpy_s(row.version, sizeof row.version, "1.0.0");
    strcpy_s(row.bundle, sizeof row.bundle, hash);
    row.size = 1234;
    CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.r2-imp/imp_copy"));
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
    CHECK(text != NULL);
    if (text) {
        loaded = sh_modmanifest_prepare_load(text, tl, &ll);
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        HeapFree(GetProcessHeap(), 0, text);
    }
    sh_modmanifest_list_free(&rows);
}

/* A mod declaring `id` at `version` with one namespaced decl. */
static void put_plain_mod(const char *folder, const char *id, const char *version, const char *body)
{
    char desc[512], rel[256];
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"%s\",\"name\":\"Twin\",\"author\":\"tester\"}", id, version);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", desc);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\twin.decl", id);
    mt_write(folder, rel, body);
}

static void put_shadow_mod(const char *folder, const char *id, const char *body)
{
    char desc[512];
    _snprintf_s(desc, sizeof desc, _TRUNCATE,
                "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"Shade\",\"author\":\"tester\","
                "\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"]}", id);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", desc);
    mt_write(folder, "assets\\generated\\decls\\entitydef\\monster\\imp.decl", body);
}

/* ---- the tests ------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char fixture[MAX_PATH], payload[MAX_PATH], tmp[MAX_PATH], src[MAX_PATH], path[MAX_PATH * 2];
    char key1[160] = "", key2[160] = "", hash1[80] = "", hash2[80] = "";
    char *doc;
    const char *dr;
    sh_ms_list list;

    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp", sizeof fixture, fixture, NULL)) return 2;
    if (!GetFullPathNameA(argc > 2 ? argv[2] : "fixtures\\mods_tab_payload.json", sizeof payload, payload, NULL)) return 2;
    g_echo = GetEnvironmentVariableA("SH_MODUI_ECHO", NULL, 0) > 0;
    if (!mt_temp_dir("modui", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%s\\store\\mods", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\store", tmp);
    CreateDirectoryA(path, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\authoring", tmp);
    CreateDirectoryA(path, NULL);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\alex.r2-imp", tmp);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(sh_ms_set_root(g_root));
    bind_fakes();

    printf("-- an empty store: every section present and empty\n");
    CHECK(sh_modserve_index_build(g_root) == 0);
    doc = list_doc();
    CHECK(doc != NULL);
    CHECK(has(doc, "{\"v\":2,\"gen\":"));
    CHECK(has(doc, "\"storeReadable\":true"));
    CHECK(has(doc, "\"mods\":[]"));
    CHECK(has(doc, "\"migrate\":{\"entries\":0,\"pending\":0,\"running\":false}"));
    CHECK(has(doc, "\"map\":{\"entities\":-1,\"names\":0,\"rows\":[],\"placed\":[]}"));
    CHECK(!has(doc, "\"bundles\":") && !has(doc, "\"shadows\":") && !has(doc, "\"shadowTotals\":"));
    CHECK(has(doc, "\"root\":\"") && has(doc, "store\\\\mods\""));
    free(doc);
    dr = doctor(0);
    CHECK(has(dr, "MODS: doctor: store ") && has(dr, ": 0 mod folder(s), 0 of them delivered by maps; all listed in the Mods tab"));
    CHECK(has(dr, "MODS: doctor: migration: no overrides folder, or it is empty\n"));
    CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 shadow(s), 0 legacy shadow(s)\nMODS: doctor: done\n"));

    printf("-- the main thread only: refused elsewhere, the previous document kept\n");
    g_main = 0;
    CHECK(sh_modui_list() == -1);
    CHECK(sh_modui_run(SH_MODS_ACT_RESCAN, NULL) == -1);
    CHECK(sh_modui_doctor(0, doctor_line, NULL) == -1);
    {
        sh_mods_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODS_LIST;
        CHECK(sh_modui_slot(NULL, &call) == 0);
        call.op = SH_MODS_FETCH;
        CHECK(sh_modui_slot(NULL, &call) == -1 && call.needed > 0);   /* the earlier document remains */
    }
    g_main = 1;

    printf("-- install through the tab: verified, indexed, installed\n");
    doc = run_doc(SH_MODS_ACT_INSTALL, "relative\\folder");
    CHECK(has(doc, "\"action\":\"install\",\"ok\":false"));
    free(doc);
    doc = run_doc(SH_MODS_ACT_INSTALL, src);
    CHECK(has(doc, "\"action\":\"install\",\"ok\":true"));
    CHECK(has(doc, "\"key\":\"alex.r2-imp\""));   /* the store folder: mods\<id> */
    CHECK(has(doc, "\"editorRows\":2"));
    CHECK(has(doc, "its editor rows appear in the palette after the game restarts"));
    CHECK(has(doc, "\"stockUnchecked\":false"));
    CHECK(g_rescans == 1);
    {
        const char *h = doc ? strstr(doc, "\"bundle\":\"") : NULL;
        if (h) { memcpy(hash1, h + 10, 64); hash1[64] = 0; }
        CHECK(strlen(hash1) == 64);
    }
    free(doc);
    doc = list_doc();
    CHECK(bundle_has(doc, "alex.r2-imp", "\"id\":\"alex.r2-imp\",\"name\":\"R2 Imp copy\",\"author\":\"alex\",\"version\":\"1.0.0\",\"build\":1"));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"files\":13"));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false,\"editorRows\":2"));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"folder\":\"alex.r2-imp\""));
    strcpy_s(key1, sizeof key1, "alex.r2-imp");
    free(doc);

    printf("-- the open map: manifest rows and the placed closure\n");
    load_map_naming("alex.r2-imp", hash1);
    CHECK(g_activations == 1);
    g_placed[0] = "mods/alex.r2-imp/imp_copy";
    g_placed[1] = "snapmaps/visblockers/industrial/cap_01";
    g_placed_count = 2;
    doc = list_doc();
    CHECK(has(doc, "\"map\":{\"entities\":2,\"names\":2,"));
    CHECK(has(doc, "\"rows\":[{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\""));
    CHECK(count_of(doc, "\"state\":\"active\"") == 1);   /* the manifest row */
    CHECK(has(doc, "\"key\":\"alex.r2-imp\",\"otherKey\":\"\",\"nameCount\":1,\"names\":[\"mods/alex.r2-imp/imp_copy\"]"));
    CHECK(has(doc, "\"placed\":[{\"id\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"viaNeeds\":false,\"inManifest\":true,\"nameCount\":1,\"names\":[\"mods/alex.r2-imp/imp_copy\"]"));
    free(doc);
    /* Removing a mod the open map uses is refused, and nothing moves. */
    doc = run_doc(SH_MODS_ACT_REMOVE, key1);
    CHECK(has(doc, "\"action\":\"remove\",\"ok\":false") && has(doc, "is used by the open map") &&
          has(doc, "\"inUse\":true,\"removed\":[]"));
    free(doc);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, key1);
    CHECK(mt_exists(path));
    /* A row naming a bundle that is not installed, while a build of the mod is. */
    load_map_naming("alex.r2-imp", "00000000000000000000000000000000000000000000000000000000000000aa");
    doc = list_doc();
    CHECK(has(doc, "\"state\":\"missing\",\"cause\":\"\",\"key\":\"\",\"otherKey\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"inManifest\":false"));
    free(doc);
    load_map_naming("alex.other", "00000000000000000000000000000000000000000000000000000000000000bb");
    doc = list_doc();
    CHECK(has(doc, "\"state\":\"missing\",\"cause\":\"\",\"key\":\"\",\"otherKey\":\"\""));
    free(doc);

    printf("-- a second version beside the first: two rows of one id; the first is removable\n");
    mt_append(src, "assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy_b.md6.decl", "\r\n// changed\r\n");
    {
        char *desc, *v;
        char mj[MAX_PATH * 2];
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\mod.json", src);
        desc = (char *)sh_mb_read_file(mj, 1u << 20, NULL);
        v = desc ? strstr(desc, "1.0.0") : NULL;
        if (v) v[4] = '1';
        mt_write(src, "mod.json", desc ? desc : "");
        free(desc);
    }
    doc = run_doc(SH_MODS_ACT_INSTALL, src);
    CHECK(has(doc, "\"ok\":true") && has(doc, "\"key\":\"alex.r2-imp@1.0.1\""));
    strcpy_s(key2, sizeof key2, "alex.r2-imp@1.0.1");
    {
        const char *h = doc ? strstr(doc, "\"bundle\":\"") : NULL;
        if (h) { memcpy(hash2, h + 10, 64); hash2[64] = 0; }
    }
    free(doc);
    doc = list_doc();
    CHECK(bundle_has(doc, key2, "\"state\":\"attention\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":true"));
    CHECK(bundle_has(doc, key1, "\"state\":\"attention\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":true"));
    CHECK(count_of(doc, "\"otherBuilds\":") == 2);
    free(doc);
    dr = doctor(0);
    CHECK(has(dr, "MODS: doctor: bundle mods\\alex.r2-imp (alex.r2-imp@1.0.0+1): superseded, alex.r2-imp@1.0.1 serves 1 of its "
                  "path(s); from an installed copy\n"));
    CHECK(has(dr, "MODS: doctor: bundle mods\\alex.r2-imp@1.0.1 (alex.r2-imp@1.0.1+1): active; from an installed copy\n"));
    CHECK(has(dr, "MODS: doctor: duplicate id alex.r2-imp: mods\\alex.r2-imp (an installed copy), mods\\alex.r2-imp@1.0.1 "
                  "(an installed copy); another installed mod declares this id; change one id in its mod.json\n"));
    /* While the open map's record names the first it is kept. */
    load_map_naming("alex.r2-imp", hash1);
    doc = run_doc(SH_MODS_ACT_PRUNE_LIST, NULL);
    CHECK(has(doc, "\"action\":\"pruneList\",\"ok\":true") && has(doc, "\"candidates\":[]"));
    free(doc);
    sh_modmanifest_reset();
    g_placed_count = 0;
    doc = run_doc(SH_MODS_ACT_PRUNE_LIST, NULL);
    CHECK(has(doc, "\"candidates\":[{\"key\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"coveredBy\":\"alex.r2-imp@1.0.1\""));
    CHECK(!has(doc, "\"key\":\"alex.r2-imp@1.0.1\""));
    free(doc);
    /* Only confirmed keys, and the newest build never. */
    doc = run_doc(SH_MODS_ACT_PRUNE_APPLY, "alex.r2-imp@1.0.1\n");
    CHECK(has(doc, "\"ok\":false") && has(doc, "\"removed\":[]"));
    free(doc);
    {
        unsigned rescans = (unsigned)g_rescans;
        doc = run_doc(SH_MODS_ACT_PRUNE_APPLY, "alex.r2-imp\r\n");
        CHECK(has(doc, "\"action\":\"pruneApply\",\"ok\":true") && has(doc, "\"removed\":[\"alex.r2-imp\"]"));
        CHECK(has(doc, "\"blobs\":0"));      /* no blob store: the folder held its own files */
        CHECK((unsigned)g_rescans == rescans + 1);
        free(doc);
    }
    CHECK(sh_ms_enumerate(&list));
    CHECK(list.count == 1 && !strcmp(list.items[0].name, key2));
    sh_ms_list_free(&list);
    {
        char gone[MAX_PATH * 2];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        _snprintf_s(gone, sizeof gone, _TRUNCATE, "%s\\%s", g_root, key1);
        CHECK(GetFileAttributesA(gone) == INVALID_FILE_ATTRIBUTES);
        _snprintf_s(gone, sizeof gone, _TRUNCATE, "%s\\.old-*", g_root);
        h = FindFirstFileA(gone, &fd);
        CHECK(h == INVALID_HANDLE_VALUE);     /* the moved-aside folder was deleted too */
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
    doc = run_doc(SH_MODS_ACT_VERIFY, "");
    CHECK(has(doc, "\"action\":\"verify\",\"ok\":true") && has(doc, "\"passed\":1,\"failed\":0"));
    free(doc);
    doc = list_doc();
    CHECK(bundle_has(doc, key2, "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false"));
    free(doc);

    printf("-- two folders declaring one id: a row each, both marked\n");
    {
        char a[MAX_PATH * 2], b[MAX_PATH * 2], moved[MAX_PATH * 2];
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\twin-a", g_root);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\twin-b", g_root);
        put_plain_mod(a, "alex.twin", "1.0.0", "{\r\n}\r\n");
        put_plain_mod(b, "alex.twin", "2.0.0", "{\r\n\t// the other one\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_INSTALL, a);       /* built in place: the folder is in the store */
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"inPlace\":true"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_INSTALL, b);
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"inPlace\":true"));
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "twin-a", "\"state\":\"attention\"") &&
              bundle_has(doc, "twin-a", "\"otherBuilds\":0,\"duplicateId\":true"));
        CHECK(bundle_has(doc, "twin-b", "\"state\":\"attention\"") &&
              bundle_has(doc, "twin-b", "\"otherBuilds\":0,\"duplicateId\":true"));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: duplicate id alex.twin: mods\\twin-a (your folder), mods\\twin-b (your folder); "
                      "another installed mod declares this id; change one id in its mod.json\n"));
        CHECK(has(dr, "MODS: doctor: bundle mods\\twin-a (alex.twin@1.0.0+1): superseded, twin-b serves 1 of its path(s)\n"));
        /* Remove one: the author's own folder, so it is moved out of the store, not deleted. */
        doc = run_doc(SH_MODS_ACT_REMOVE, "twin-b");
        CHECK(has(doc, "\"action\":\"remove\",\"ok\":true") && has(doc, "Removed Twin. Its folder was moved to ") &&
              has(doc, "\"inUse\":false,\"removed\":[\"twin-b\"]") && has(doc, "removed-mods\\\\twin-b\""));
        free(doc);
        _snprintf_s(moved, sizeof moved, _TRUNCATE, "%s\\store\\removed-mods\\twin-b\\mod.json", tmp);
        CHECK(mt_exists(moved) && !mt_exists(b));
        doc = list_doc();
        CHECK(bundle_has(doc, "twin-a", "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false"));
        CHECK(!has(doc, "twin-b"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_REMOVE, "twin-b");
        CHECK(has(doc, "\"ok\":false") && has(doc, "no longer installed"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_REMOVE, "twin-a");
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
    }

    printf("-- a package a map delivered is an ordinary row; the doctor says where it came from\n");
    {
        char leg[MAX_PATH * 2];
        _snprintf_s(leg, sizeof leg, _TRUNCATE, "%s\\old.pack", g_root);
        put_plain_mod(leg, "old.pack", "1.0.0", "{\r\n}\r\n");
        mt_write(leg, "mod.json", "{\"id\":\"old.pack\",\"version\":\"1.0.0\",\"name\":\"Old\",\"legacy\":{\"keyForm\":\"delivery\"},"
                                  "\"snapmap-plus\":{\"origin\":\"delivery\"}}");
        doc = run_doc(SH_MODS_ACT_INSTALL, leg);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        CHECK(sh_ms_enumerate(&list));
        {
            size_t i;
            int seen = 0;
            for (i = 0; i < list.count; i++) seen |= list.items[i].delivered;
            CHECK(seen);
        }
        sh_ms_list_free(&list);
        doc = list_doc();
        CHECK(bundle_has(doc, "old.pack", "\"state\":\"installed\""));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: delivered bundle mods\\old.pack (old.pack@1.0.0+1): active; from a map that delivered it\n"));
        CHECK(has(dr, "1 of them delivered by maps"));
        doc = run_doc(SH_MODS_ACT_REMOVE, "old.pack");
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"movedTo\":\"\""));   /* the product made it: deleted */
        free(doc);
        CHECK(!mt_exists(leg));
    }

    printf("-- migrate: the button's state, the action, and the slot's WAIT\n");
    g_mig_entries = 34;
    g_mig_pending = 2;
    doc = list_doc();
    CHECK(has(doc, "\"migrate\":{\"entries\":34,\"pending\":2,\"running\":false}"));
    free(doc);
    dr = doctor(0);
    CHECK(has(dr, "MODS: doctor: migration: overrides holds 34 entries, 2 not migrated (Migrate in the Mods tab"));
    doc = run_doc(SH_MODS_ACT_MIGRATE, NULL);
    CHECK(has(doc, "\"action\":\"migrate\",\"ok\":true") && has(doc, "\"started\":true"));
    CHECK(g_mig_started == 1 && g_mig_again == 1);
    free(doc);
    {
        sh_mods_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODS_WAIT;
        call.out_cap = 0;
        CHECK(sh_modui_slot(NULL, &call) == 0);      /* still running, no time to wait */
        call.out_cap = 2000;
        CHECK(sh_modui_slot(NULL, &call) == 1);      /* finished within the wait */
    }
    doc = list_doc();
    CHECK(has(doc, "\"migrate\":{\"entries\":34,\"pending\":0,\"running\":false}"));
    free(doc);
    g_mig_start_result = 0;
    doc = run_doc(SH_MODS_ACT_MIGRATE, NULL);
    CHECK(has(doc, "\"ok\":true") && has(doc, "Nothing to migrate.") && has(doc, "\"started\":false"));
    free(doc);
    g_mig_start_result = -1;
    doc = run_doc(SH_MODS_ACT_MIGRATE, NULL);
    CHECK(has(doc, "\"ok\":false") && has(doc, "already running"));
    free(doc);
    g_mig_start_result = 1;
    g_mig_entries = 0;

    printf("-- stock shadows and a conflict between two mods: the doctor prints them\n");
    {
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\authoring\\alex.shade-a", tmp);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\authoring\\alex.shade-b", tmp);
        put_shadow_mod(a, "alex.shade-a", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n}");
        doc = run_doc(SH_MODS_ACT_INSTALL, a);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: shadow generated/decls/entitydef/monster/imp.decl: by alex.shade-a; served by alex.shade-a\n"));
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 1 shadow(s), 0 legacy shadow(s)\n"));
        put_shadow_mod(b, "alex.shade-b", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"2\";\r\n}");
        doc = run_doc(SH_MODS_ACT_INSTALL, b);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        /* Two mods' shadows with different bytes: one serves (here the later
         * install; the texts differ outside lists), and the doctor says why. */
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: conflict generated/decls/entitydef/monster/imp.decl: alex.shade-b serves it "
                      "(installed most recently); from alex.shade-a, alex.shade-b\n"));
        CHECK(has(dr, "MODS: doctor: 1 conflict(s), 0 shadow(s), 0 legacy shadow(s)\n"));
        /* Every section populated for the key comparison with the page's fixture. */
        load_map_naming("alex.r2-imp", hash2);
        g_placed[0] = "mods/alex.r2-imp/imp_copy";
        g_placed_count = 1;
        doc = list_doc();
        CHECK(has(doc, "\"rows\":[{\"id\":\"alex.r2-imp\"") && has(doc, "\"placed\":[{\"id\":\"alex.r2-imp\""));
        CHECK(bundle_has(doc, "alex.shade-a", "\"state\":\"installed\""));   /* a conflict is the doctor's, not the tab's */
        keys_match_fixture(payload, doc);
        free(doc);
        dr = doctor(1);
        CHECK(has(dr, "MODS: doctor: verify: rehashing every build\nMODS: doctor: verify: Verified 3 build(s): every file matches."));
    }

    printf("-- attention: a warning, an unbuilt folder, an unindexed build\n");
    {
        char hand[MAX_PATH * 2], file[MAX_PATH * 2];
        /* A store file changed size after install: the index built before
         * still serves the build, and the store's check is the warning. */
        _snprintf_s(file, sizeof file, _TRUNCATE, "%s\\%s\\assets\\generated\\decls\\entitydef\\monster\\imp.decl", g_root,
                    "alex.shade-b");
        SetFileAttributesA(file, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(file);
        {
            FILE *f = NULL;
            fopen_s(&f, file, "wb");
            if (f) { fputs("{}", f); fclose(f); }
        }
        /* A descriptor dropped in by hand and never built. */
        _snprintf_s(hand, sizeof hand, _TRUNCATE, "%s\\by-hand", g_root);
        CreateDirectoryA(hand, NULL);
        mt_write(hand, "mod.json", "{\"id\":\"alex.by-hand\",\"version\":\"1.0.0\",\"name\":\"By hand\"}");
        doc = list_doc();
        CHECK(bundle_has(doc, "alex.shade-b", "\"state\":\"attention\",\"cause\":\"assets/generated/decls/entitydef/monster/imp.decl "
                                              "is not the size mod.json records; 'sh_mods doctor verify' checks the build\""));
        CHECK(bundle_has(doc, "by-hand", "\"id\":\"\",\"name\":\"\""));
        CHECK(bundle_has(doc, "by-hand", "\"state\":\"attention\",\"cause\":\"not built yet"));
        free(doc);
        dr = doctor(1);
        CHECK(has(dr, "MODS: doctor: verify alex.shade-b: FAILED: "));
        CHECK(has(dr, "MODS: doctor: verify: 1 of 3 build(s) failed verification. First: alex.shade-b: "));
        /* A damaged mod.json: the tab's rescan cannot build it again, so the
         * store and the index skip it, which ends the conflict. */
        _snprintf_s(file, sizeof file, _TRUNCATE, "%s\\alex.shade-b", g_root);
        mt_write(file, "mod.json", "{\"format\":1}");
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true") && has(doc, "Mods folder reread"));
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "alex.shade-b", "\"state\":\"attention\",\"cause\":\""));
        CHECK(!bundle_has(doc, "alex.shade-b", "\"cause\":\"\""));
        CHECK(bundle_has(doc, "alex.shade-b", "is missing its required id"));
        CHECK(bundle_has(doc, "alex.shade-a", "\"state\":\"installed\""));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: bundle mods\\alex.shade-b (alex.shade-b): skipped; "));
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 1 shadow(s), 0 legacy shadow(s)\n"));
        /* Installed while the index is stale: unindexed until a rescan. */
        sh_modserve_index_build(g_root);
        {
            sh_modui_engine e;
            char c[MAX_PATH * 2];
            memset(&e, 0, sizeof e);
            e.on_main_thread = fake_on_main;
            e.stock_exists = fake_stock;
            sh_modui_bind(&e);                          /* no rescan or migration bound in this build */
            _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\authoring\\alex.shade-c", tmp);
            put_shadow_mod(c, "alex.shade-c", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"3\";\r\n}");
            doc = run_doc(SH_MODS_ACT_INSTALL, c);
            CHECK(has(doc, "\"ok\":true") && has(doc, "\"index\":\"the serving index cannot be rebuilt from here\""));
            free(doc);
            doc = list_doc();
            CHECK(bundle_has(doc, "alex.shade-c", "\"state\":\"attention\",\"cause\":\"not in the serving index yet; it is used after the next scan\""));
            CHECK(has(doc, "\"map\":{\"entities\":-2,"));   /* no placed-name reader bound */
            CHECK(has(doc, "\"migrate\":{\"entries\":0,\"pending\":0,\"running\":false}"));
            free(doc);
            dr = doctor(0);
            CHECK(has(dr, "MODS: doctor: migration: not available in this build\n"));
            doc = run_doc(SH_MODS_ACT_MIGRATE, NULL);
            CHECK(has(doc, "\"action\":\"migrate\",\"ok\":false") && has(doc, "not available"));
            free(doc);
            CHECK(sh_modui_wait(0) == 1);                /* nothing to wait for */
            doc = run_doc(SH_MODS_ACT_PALETTE, NULL);
            CHECK(has(doc, "\"action\":\"palette\",\"ok\":false"));
            free(doc);
            bind_fakes();
        }
    }

    printf("-- remove a folder the product made: deleted\n");
    {
        unsigned rescans = (unsigned)g_rescans;
        sh_modmanifest_reset();
        g_placed_count = 0;
        doc = run_doc(SH_MODS_ACT_REMOVE, "alex.shade-a");
        CHECK(has(doc, "\"action\":\"remove\",\"ok\":true") && has(doc, "\"message\":\"Removed Shade.\"") &&
              has(doc, "\"removed\":[\"alex.shade-a\"]") && has(doc, "\"movedTo\":\"\"") && has(doc, "\"blobs\":0"));
        CHECK((unsigned)g_rescans == rescans + 1);
        free(doc);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.shade-a", g_root);
        CHECK(!mt_exists(path));
        doc = run_doc(SH_MODS_ACT_REMOVE, "");
        CHECK(has(doc, "\"ok\":false") && has(doc, "Select a mod to remove."));
        free(doc);
    }

    printf("-- palette rebuild reports refusal and success\n");
    g_palette_result = 0;
    doc = run_doc(SH_MODS_ACT_PALETTE, NULL);
    CHECK(has(doc, "\"ok\":false") && has(doc, "release any held entity"));
    free(doc);
    g_palette_result = 1;
    doc = run_doc(SH_MODS_ACT_PALETTE, NULL);
    CHECK(has(doc, "\"ok\":true") && g_palette_calls == 2);
    free(doc);

    printf("-- the slot: size query, fetch, root, generation\n");
    {
        sh_mods_call call;
        char tiny[8];
        unsigned before = sh_modui_generation();
        memset(&call, 0, sizeof call);
        call.op = SH_MODS_RUN;
        call.action = SH_MODS_ACT_RESCAN;
        CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed > 0 && call.generation == before + 1);
        call.op = SH_MODS_FETCH;
        call.out = tiny;
        call.out_cap = (int)sizeof tiny;
        CHECK(sh_modui_slot(NULL, &call) == -1 && call.needed > (int)sizeof tiny);
        call.op = SH_MODS_LIST;
        CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed > 100);
        call.op = SH_MODS_ROOT;
        call.out = path;
        call.out_cap = (int)sizeof path;
        CHECK(sh_modui_slot(NULL, &call) == 1 && !strcmp(path, g_root));
        call.op = 99;
        CHECK(sh_modui_slot(NULL, &call) == 0);
        CHECK(sh_modui_slot(NULL, NULL) == 0);
    }

    mt_remove_tree(tmp);
    printf("modui_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
