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
 * payload v3 (what each mod carries, the open map's details from its saved
 * game.details and the record's links and claims, each mod it uses with its
 * entities, wires and Timeline references), Remove from map and Uninstall
 * with their dry runs against a fake editor that records deletions (Play and
 * a load refused, one batch, the row and the search tag dropped, the next
 * save writing no row), Rebuild, the console verbs with their --yes gate, and
 * that every key the page's v3 fixture uses is a key the backend writes.
 *
 *   modui_test.exe <tests\fixtures\mods\alex.r2-imp> <tests\fixtures\mods_tab_payload_v3.json>
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
#include "editor_delete.h"

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

/* The refresh after a rescan (sh_modserve_refresh_after_rescan): what it
 * reports, and how often it ran. Mode 0 nothing changed, 1 applied, 2 deferred. */
static int g_refresh_mode, g_refreshes;
static int fake_refresh(sh_modserve_refresh *out)
{
    g_refreshes++;
    memset(out, 0, sizeof *out);
    if (g_refresh_mode == 1) {
        out->reparsed = 3; out->reloaded = 1; out->needs_map_reload = 2; out->needs_restart = 1;
        strcpy_s(out->note, sizeof out->note, "3 decl(s) re-parsed, 1 model(s) and image(s) reloaded");
    } else if (g_refresh_mode == 2) {
        out->pending = 4;
        strcpy_s(out->note, sizeof out->note, "4 change(s) apply at the next editor pass or map load (Play is running)");
    } else strcpy_s(out->note, sizeof out->note, "no served file changed");
    return g_refresh_mode;
}

/* The open map as the editor would serialize it: entities by uniqueId, wires
 * and one Timeline's entity reference. A delete releases entities the way the
 * native Delete Selection does (their wires and references go with them). */
typedef struct fake_ent { int uid; const char *inherit, *cls, *label; int alive; } fake_ent;
static fake_ent g_ents[8];
static int g_ent_n, g_map_on;
static int g_wire_src[8], g_wire_dst[8], g_wire_n;
static int g_tl_uid = -1, g_tl_ref = -1;      /* the Timeline and the entity it references */
static int g_module_mod;                      /* 1: an instance whose moduleName the imp mod provides */

static int ent_alive(int uid)
{
    int i;
    for (i = 0; i < g_ent_n; i++) if (g_ents[i].uid == uid) return g_ents[i].alive;
    return 0;
}

static char *map_text(size_t *len)
{
    static char t[8192];
    size_t at = 0;
    int i, first = 1, max = -1;
#define PUT(...) at += (size_t)_snprintf_s(t + at, sizeof t - at, _TRUNCATE, __VA_ARGS__)
    PUT("{\"entities\":[");
    for (i = 0; i < g_ent_n; i++) {
        if (!g_ents[i].alive) continue;
        if (g_ents[i].uid > max) max = g_ents[i].uid;
        PUT("%s{\"displayName\":\"%s\",\"entityDef\":{\"className\":\"%s\",\"inherit\":\"%s\",\"name\":\"\","
            "\"state\":{\"edit\":{\"spawnPosition\":{\"x\":1}}}},\"uniqueId\":%d}", first ? "" : ",", g_ents[i].label,
            g_ents[i].cls, g_ents[i].inherit, g_ents[i].uid);
        first = 0;
    }
    PUT("],\"instances\":[{\"moduleName\":\"%s\"}],\"targets\":{\"connections\":[",
        g_module_mod ? "mods/alex.r2-imp/imp_copy" : "maps/modules/stock/room_a");
    for (i = 0, first = 1; i < g_wire_n; i++) {
        if (!ent_alive(g_wire_src[i]) || !ent_alive(g_wire_dst[i])) continue;
        PUT("%s%d,%d", first ? "" : ",", -(g_wire_src[i] + 1), g_wire_dst[i]);
        first = 0;
    }
    PUT("]},\"references\":{\"entityEntRefs\":{\"keyValues\":[0");
    for (i = 0; i <= (max > g_tl_uid ? max : g_tl_uid); i++) PUT(",%d", i >= g_tl_uid && g_tl_uid >= 0 && ent_alive(g_tl_uid) ? 1 : 0);
    PUT("],\"values\":[");
    if (g_tl_uid >= 0 && ent_alive(g_tl_uid)) PUT("%d", ent_alive(g_tl_ref) ? g_tl_ref : -1);
    PUT("]}},\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}");
#undef PUT
    *len = at;
    return t;
}

static int fake_map_json(char **out, size_t *len)
{
    char *t, *copy;
    if (!g_map_on) return -1;   /* not bound for this map: the tab falls back to the placed names */
    t = map_text(len);
    copy = (char *)HeapAlloc(GetProcessHeap(), 0, *len + 1);
    if (!copy) return -1;
    memcpy(copy, t, *len + 1);
    *out = copy;
    return 1;
}

static int g_tagged = 1;
static int fake_map_info(sh_modui_map_info *out)
{
    if (!g_map_on) return 0;
    memset(out, 0, sizeof *out);
    strcpy_s(out->local_id, sizeof out->local_id, "ABCDEF0123456789ABCD");
    strcpy_s(out->cloud_id, sizeof out->cloud_id, "6ae40b378bf5037d3d63bd23");
    out->tagged = g_tagged;
    return 1;
}

static int g_delete_status = SH_EDITOR_DELETE_DISPATCHED, g_delete_calls, g_deleted[16], g_deleted_n;
static int fake_delete(const int *ids, int count)
{
    int i, k;
    g_delete_calls++;
    if (g_delete_status != SH_EDITOR_DELETE_DISPATCHED) return g_delete_status;
    for (i = 0; i < count; i++) {
        if (g_deleted_n < 16) g_deleted[g_deleted_n++] = ids[i];
        for (k = 0; k < g_ent_n; k++) if (g_ents[k].uid == ids[i]) g_ents[k].alive = 0;
    }
    return SH_EDITOR_DELETE_DISPATCHED;
}

static int g_tag_removals;
static int fake_remove_tag(const char *tag)
{
    if (strcmp(tag, "mods")) return -1;
    g_tag_removals++;
    if (!g_tagged) return 0;
    g_tagged = 0;
    return 1;
}

static char g_toast[600], g_print[600];
static int g_toasts;
static void fake_toast(const char *title, const char *text) { g_toasts++; _snprintf_s(g_toast, sizeof g_toast, _TRUNCATE, "%s: %s", title, text); }
static void fake_print(const char *line) { strncpy_s(g_print, sizeof g_print, line, _TRUNCATE); }
static int fake_entity_valid(int uid) { return g_map_on && ent_alive(uid); }

/* The map scope's game-side apply (sh_modserve_scope_apply): how often, and the last phase. */
static int g_scope_applies;
static char g_scope_phase[64];
static int fake_scope_apply(const char *phase)
{
    g_scope_applies++;
    strncpy_s(g_scope_phase, sizeof g_scope_phase, phase ? phase : "", _TRUNCATE);
    return 1;
}

/* modui.h map_left while the editor screen is down: 1 left (hub), 0 a menu over the map, -1 unknown. */
static int g_map_left = 1;
static int fake_map_left(void) { return g_map_left; }

static const char *g_busy;
static int fake_busy(char *cause, size_t cap)
{
    if (!g_busy) return 0;
    strcpy_s(cause, cap, g_busy);
    return 1;
}

/* The map the tests open: a vanilla start, two entities of the imp mod
 * (one named by the author), a relay wired to both, and a Timeline that
 * references the second. */
static void open_test_map(void)
{
    static const fake_ent ents[] = {
        { 0, "player/coop/player_start", "idSnapMapGameEntity_ComboStart", "", 1 },
        { 1, "mods/alex.r2-imp/imp_copy", "idSnapEntity", "", 1 },
        { 2, "mods/alex.r2-imp/imp_copy", "idSnapEntity", "Gate imp", 1 },
        { 3, "snapmaps/logic/relay", "idSnapEntity", "Relay", 1 },
        { 4, "snapmaps/unknown", "idTarget_Timeline", "Wave 2", 1 },
    };
    memcpy(g_ents, ents, sizeof ents);
    g_ent_n = 5;
    g_wire_n = 2;
    g_wire_src[0] = 3; g_wire_dst[0] = 1;   /* relay -> imp */
    g_wire_src[1] = 2; g_wire_dst[1] = 3;   /* gate imp -> relay */
    g_tl_uid = 4;
    g_tl_ref = 2;
    g_module_mod = 0;
    g_map_on = 1;
}

static void bind_fakes(void)
{
    sh_modui_engine e;
    memset(&e, 0, sizeof e);
    e.map_json = fake_map_json;
    e.map_info = fake_map_info;
    e.delete_entities = fake_delete;
    e.remove_tag = fake_remove_tag;
    e.busy = fake_busy;
    e.entity_valid = fake_entity_valid;
    e.toast = fake_toast;
    e.print = fake_print;
    e.on_main_thread = fake_on_main;
    e.placed_names = fake_placed;
    e.rescan = fake_rescan;
    e.refresh = fake_refresh;
    e.palette_rebuild = fake_palette;
    e.stock_exists = fake_stock;
    e.migrate_pending = fake_mig_pending;
    e.migrate_start = fake_mig_start;
    e.migrate_busy = fake_mig_busy;
    e.scope_apply = fake_scope_apply;
    e.map_left = fake_map_left;
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
    if (!GetFullPathNameA(argc > 2 ? argv[2] : "fixtures\\mods_tab_payload_v3.json", sizeof payload, payload, NULL)) return 2;
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
    CHECK(has(doc, "{\"v\":3,\"gen\":"));
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
    CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 shadow(s), 0 grandfathered shadow(s)\nMODS: doctor: done\n"));

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

    printf("-- rescan: the result carries what the refresh did in the game\n");
    g_refresh_mode = 1;
    g_refreshes = 0;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(g_refreshes == 1);
    CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true"));
    CHECK(has(doc, "In the game: 3 decl(s) re-parsed, 1 model(s) and image(s) reloaded."));
    CHECK(has(doc, "\"refresh\":{\"reparsed\":3,\"reloaded\":1,\"needsMapReload\":2,\"needsRestart\":1,"
                   "\"pending\":0,\"deferred\":false,\"note\":\"3 decl(s) re-parsed, 1 model(s) and image(s) reloaded\"}"));
    free(doc);
    g_refresh_mode = 2;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(has(doc, "\"refresh\":{\"reparsed\":0,\"reloaded\":0,\"needsMapReload\":0,\"needsRestart\":0,"
                   "\"pending\":4,\"deferred\":true,\"note\":\"4 change(s) apply at the next editor pass or map load (Play is running)\"}"));
    free(doc);
    g_refresh_mode = 0;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(has(doc, "\"deferred\":false,\"note\":\"no served file changed\"}") && has(doc, "Mods folder reread: 0 mod(s) active."));
    CHECK(!has(doc, "In the game:"));
    free(doc);
    /* A refused rescan runs no refresh and carries none. */
    g_refreshes = 0;
    {
        sh_modui_engine e;
        memset(&e, 0, sizeof e);
        e.on_main_thread = fake_on_main;
        sh_modui_bind(&e);   /* no rescan bound in a SH_MODUI_PURE build */
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(doc, "\"ok\":false") && !has(doc, "\"refresh\"") && g_refreshes == 0);
        free(doc);
        bind_fakes();
    }
    g_rescans = 0;

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
        CHECK(!has(doc, "\"key\":\"twin-b\""));
        CHECK(has(doc, "\"removed\":{\"folder\":\"") && has(doc, "{\"folder\":\"twin-b\",\"id\":\"alex.twin\",\"version\":\"2.0.0\","
                                                              "\"name\":\"Twin\",\"bytes\":"));
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
        CHECK(has(dr, "MODS: doctor: bundle delivered by a map, mods\\old.pack (old.pack@1.0.0+1): active\n"));
        CHECK(has(dr, "1 of them delivered by maps"));
        /* One delivered without a version: the converter's generated version
         * is in its hash, never in its folder name, the page or the doctor. */
        {
            char gen[MAX_PATH * 2];
            _snprintf_s(gen, sizeof gen, _TRUNCATE, "%s\\authoring\\old.gen", tmp);
            put_plain_mod(gen, "old.gen", "1.0.0", "{\r\n}\r\n");
            mt_write(gen, "mod.json", "{\"id\":\"old.gen\",\"version\":\"legacy-0123456789abcdef\",\"name\":\"Gen\","
                                      "\"legacy\":{\"keyForm\":\"delivery\"},\"snapmap-plus\":{\"origin\":\"delivery\"}}");
            doc = run_doc(SH_MODS_ACT_INSTALL, gen);
            CHECK(has(doc, "\"ok\":true"));
            free(doc);
            doc = list_doc();
            CHECK(bundle_has(doc, "old.gen", "\"version\":\"\""));
            CHECK(!has(doc, "legacy-0123456789abcdef"));
            free(doc);
            dr = doctor(0);
            CHECK(has(dr, "MODS: doctor: bundle delivered by a map, mods\\old.gen (old.gen+1): active\n"));
            CHECK(!has(dr, "legacy"));
            doc = run_doc(SH_MODS_ACT_REMOVE, "old.gen");
            CHECK(has(doc, "\"ok\":true"));
            free(doc);
        }
        doc = run_doc(SH_MODS_ACT_REMOVE, "old.pack");
        CHECK(has(doc, "\"ok\":true") && has(doc, "removed-mods\\\\old.pack\""));   /* the product made it: moved too */
        free(doc);
        CHECK(!mt_exists(leg));
        doc = list_doc();
        CHECK(has(doc, "{\"folder\":\"old.gen\",\"id\":\"old.gen\",\"version\":\"\",\"name\":\"Gen\""));   /* no generated version */
        free(doc);
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
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 1 shadow(s), 0 grandfathered shadow(s)\n"));
        put_shadow_mod(b, "alex.shade-b", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"2\";\r\n}");
        doc = run_doc(SH_MODS_ACT_INSTALL, b);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        /* Two mods' shadows with different bytes: one serves (here the later
         * install; the texts differ outside lists), and the doctor says why. */
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: conflict generated/decls/entitydef/monster/imp.decl: alex.shade-b serves it "
                      "(installed most recently); from alex.shade-a, alex.shade-b\n"));
        CHECK(has(dr, "MODS: doctor: 1 conflict(s), 0 shadow(s), 0 grandfathered shadow(s)\n"));
        /* Every section populated for the key comparison with the page's fixture. */
        load_map_naming("alex.r2-imp", hash2);
        g_placed[0] = "mods/alex.r2-imp/imp_copy";
        g_placed_count = 1;
        doc = list_doc();
        CHECK(has(doc, "\"rows\":[{\"id\":\"alex.r2-imp\"") && has(doc, "\"placed\":[{\"id\":\"alex.r2-imp\""));
        CHECK(bundle_has(doc, "alex.shade-a", "\"state\":\"installed\""));   /* a conflict is the doctor's, not the tab's */
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
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 1 shadow(s), 0 grandfathered shadow(s)\n"));
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
        CHECK(has(doc, "\"action\":\"remove\",\"ok\":true") && has(doc, "\"message\":\"Removed Shade. Its folder was moved to ") &&
              has(doc, "\"removed\":[\"alex.shade-a\"]") && has(doc, "removed-mods\\\\alex.shade-a\"") && has(doc, "\"blobs\":0"));
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

    printf("-- payload v3: what each mod carries, and the open map\n");
    {
        char req[MAX_PATH * 2], saves[MAX_PATH * 2], details[512];
        _snprintf_s(req, sizeof req, _TRUNCATE, "%s\\authoring\\alex.req", tmp);
        put_plain_mod(req, "alex.req", "1.0.0", "{\r\n}\r\n");
        mt_write(req, "mod.json", "{\"id\":\"alex.req\",\"version\":\"1.0.0\",\"name\":\"Gates\",\"author\":\"tester\","
                                  "\"description\":\"Turns the two blacklists off.\","
                                  "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":\"0\",\"g_useResourceBlackList\":\"0\"}}}");
        doc = run_doc(SH_MODS_ACT_INSTALL, req);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        /* The saved map's game.details, and the record's link and claim for its published item. */
        _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\saves", tmp);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\76561198000000000\\SNAPMAPSABCDEF0123456789ABCD", saves);
        _snprintf_s(details, sizeof details, _TRUNCATE,
                    "checksum=1\r\nitemId=6ae40b378bf5037d3d63bd23\r\nmetadata={\"localId\":\"ABCDEF0123456789ABCD\","
                    "\"name\":\"Imp Arena\",\"lastModified\":1793364363900,\"tags\":[\"map:dlc1\",\"mods\"]}\r\nmodified=1793364363900\r\n");
        mt_write(path, "game.details", details);
        sh_modui_test_set_saves_root(saves);
        {
            char claims[400];
            _snprintf_s(claims, sizeof claims, _TRUNCATE,
                        "{\"item123\":{\"bundles\":[\"%s\"],\"claim\":\"c\",\"secret\":\"s\",\"updated\":1793164663},"
                        "\"publish-pending-0123456789abcdef\":{\"bundles\":[\"%s\"],\"updated\":1}}", hash2, hash2);
            CHECK(sh_mb_record_put(g_root, "claims", claims));
            CHECK(sh_mb_record_put(g_root, "links", "{\"item123\":{\"cloud\":\"6ae40b378bf5037d3d63bd23\","
                                                    "\"local\":\"ABCDEF0123456789ABCD\",\"at\":1793164563}}"));
        }
        load_map_naming("alex.r2-imp", hash2);
        open_test_map();
        g_tagged = 1;
        doc = list_doc();
        CHECK(has(doc, "{\"v\":3,"));
        CHECK(bundle_has(doc, "alex.req", "\"description\":\"Turns the two blacklists off.\",\"origin\":\"install\","
                                          "\"migratedFrom\":\"\",\"servedBy\":\"\",\"builtAt\":"));
        CHECK(has(doc, "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":\"0\",\"g_useResourceBlackList\":\"0\"}}"));
        CHECK(bundle_has(doc, key2, "\"counts\":{\"shadows\":0,\"sounds\":0,\"movies\":0,\"modules\":0,\"vt\":0"));
        CHECK(has(doc, "\"editorRowNames\":[\"mods/alex.r2-imp/imp_copy\",\"mods/alex.r2-imp/imp_copy_b\"]"));
        CHECK(has(doc, "\"carriedPaths\":[\"cooked/anim/md6/mods/alex.r2_imp/imp_copy/motion/idle.bmd6anim\""));
        CHECK(has(doc, "\"publishedMaps\":1"));          /* the provisional claim does not count */
        CHECK(has(doc, "\"bundle\":\"") && has(doc, hash2));
        /* The map object: details from the saved map and the record, then its mods. */
        CHECK(has(doc, "\"name\":\"Imp Arena\",\"id\":\"ABCDEF0123456789ABCD\",\"saved\":1793364363,"
                       "\"published\":{\"state\":\"published\",\"at\":1793164563},\"uploaded\":{\"claimed\":true,\"at\":1793164663},"
                       "\"tagged\":true,\"mods\":[{\"key\":\"alex.r2-imp@1.0.1\",\"id\":\"alex.r2-imp\""));
        CHECK(has(doc, "\"state\":\"installed\",\"placedEntities\":[\"mods/alex.r2-imp/imp_copy #1\",\"Gate imp #2\"],"
                       "\"placedCount\":2,\"placedModules\":0,\"references\":[{\"kind\":\"wire\",\"name\":\"Relay #3 to "
                       "mods/alex.r2-imp/imp_copy #1\"},{\"kind\":\"wire\",\"name\":\"Gate imp #2 to Relay #3\"},"
                       "{\"kind\":\"timeline\",\"name\":\"Wave 2 #4\"}]"));
        CHECK(has(doc, "\"vanilla\":false}"));
        keys_match_fixture(payload, doc);
        free(doc);

        printf("-- map remove: the dry run, refusals, then the removal\n");
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, key2);
        CHECK(has(doc, "\"action\":\"mapRemoveModPreview\",\"ok\":true"));
        CHECK(has(doc, "\"preview\":{\"action\":\"mapRemoveMod\",\"key\":\"alex.r2-imp@1.0.1\",\"name\":\"R2 Imp copy\","
                       "\"map\":{\"name\":\"Imp Arena\",\"entities\":[\"mods/alex.r2-imp/imp_copy #1\",\"Gate imp #2\"],"
                       "\"entityCount\":2,\"references\":[{\"kind\":\"wire\""));
        CHECK(has(doc, "\"manifestRow\":true,\"tagRemoved\":true,\"vanillaAfter\":true,\"published\":true},\"install\":null}"));
        CHECK(!has(doc, "\"editorRows\":["));            /* Toybox rows are not a map consequence */
        free(doc);
        CHECK(g_delete_calls == 0 && ent_alive(1));      /* a dry run changes nothing */
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, "alex.req");
        CHECK(has(doc, "\"ok\":false") && has(doc, "The open map does not use Gates."));
        free(doc);
        /* Play or a load: refused with the cause, before anything is read. */
        g_busy = "Play is running";
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE, key2);
        CHECK(has(doc, "\"action\":\"mapRemoveMod\",\"ok\":false,\"message\":\"Not now: Play is running.\""));
        free(doc);
        g_busy = NULL;
        CHECK(g_delete_calls == 0);
        /* The editor refuses the delete: nothing else changes. */
        g_delete_status = SH_EDITOR_DELETE_REFERENCED;
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE, key2);
        CHECK(has(doc, "\"ok\":false") && has(doc, "the editor asks before deleting them"));
        free(doc);
        CHECK(g_delete_calls == 1 && g_tagged == 1);
        {
            sh_modmanifest_list cur;
            CHECK(sh_modmanifest_current(&cur) == 1);
            sh_modmanifest_list_free(&cur);
        }
        g_delete_status = SH_EDITOR_DELETE_DISPATCHED;
        /* Before: the save writes the mod's row. */
        {
            size_t len = 0, out_len = 0;
            char *t = map_text(&len), *saved = sh_modmanifest_on_save(t, len, &out_len);
            CHECK(saved && strstr(saved, "smpmod1.0.1"));
            if (saved) HeapFree(GetProcessHeap(), 0, saved);
        }
        g_delete_calls = 0;
        g_deleted_n = 0;
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE, key2);
        CHECK(has(doc, "\"action\":\"mapRemoveMod\",\"ok\":true,\"message\":\"Removed R2 Imp copy from Imp Arena: 2 entities "
                       "deleted; the map now plays without Snapmap+. Ctrl+Z in the editor brings them back.\""));
        free(doc);
        CHECK(g_delete_calls == 1 && g_deleted_n == 2 && g_deleted[0] == 1 && g_deleted[1] == 2);   /* one batch: one undo step */
        CHECK(g_tagged == 0 && g_tag_removals == 1);
        {
            sh_modmanifest_list cur;
            CHECK(sh_modmanifest_current(&cur) == 0);   /* the row is gone from the map's record */
            sh_modmanifest_list_free(&cur);
        }
        /* After: the save rebuilds rows from the placed entities, and writes none. */
        {
            size_t len = 0, out_len = 0;
            char *t = map_text(&len), *saved = sh_modmanifest_on_save(t, len, &out_len);
            CHECK(saved == NULL);
            if (saved) HeapFree(GetProcessHeap(), 0, saved);
        }
        doc = list_doc();
        CHECK(has(doc, "\"tagged\":false,\"mods\":[],\"vanilla\":true}"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE, key2);
        CHECK(has(doc, "\"ok\":false") && has(doc, "The open map does not use R2 Imp copy."));
        free(doc);

        printf("-- a row whose mod is not installed: its entities found by the names the row recorded\n");
        open_test_map();
        g_ents[1].inherit = "mods/alex.gone/thing";
        g_ents[2].alive = 0;
        {
            sh_modmanifest_list rows;
            sh_modmanifest_row row;
            char err[256], *text, *loaded;
            size_t tl = 0, ll = 0;
            memset(&rows, 0, sizeof rows);
            memset(&row, 0, sizeof row);
            strcpy_s(row.id, sizeof row.id, "alex.gone");
            strcpy_s(row.version, sizeof row.version, "1.0.0");
            strcpy_s(row.bundle, sizeof row.bundle, "00000000000000000000000000000000000000000000000000000000000000cc");
            row.size = 77;
            CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.gone/thing"));
            CHECK(sh_modmanifest_list_add(&rows, &row));
            sh_modmanifest_row_free(&row);
            text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
            loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
            if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
            if (text) HeapFree(GetProcessHeap(), 0, text);
            sh_modmanifest_list_free(&rows);
        }
        doc = list_doc();
        CHECK(has(doc, "{\"key\":\"\",\"id\":\"alex.gone\",\"version\":\"1.0.0\",\"state\":\"missing\",\"placedEntities\":"
                       "[\"thing #1\"],\"placedCount\":1"));
        free(doc);
        g_deleted_n = 0;
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "alex.gone");
        CHECK(has(doc, "\"ok\":true") && g_deleted_n == 1 && g_deleted[0] == 1);
        free(doc);

        printf("-- a mod placed as a module, and one the map only needs through another: refused\n");
        load_map_naming("alex.r2-imp", hash2);
        open_test_map();
        g_module_mod = 1;
        doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, key2);
        CHECK(has(doc, "\"ok\":false") && has(doc, "the map places 1 of its modules; delete them in Blueprint mode first"));
        free(doc);
        g_module_mod = 0;

        printf("-- uninstall: the dry run (map part, Toybox rows, folder), then in order\n");
        g_tagged = 1;
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "alex.req");
        CHECK(has(doc, "\"preview\":{\"action\":\"uninstall\",\"key\":\"alex.req\",\"name\":\"Gates\",\"map\":null,"
                       "\"install\":{\"folder\":\"alex.req\",\"disposition\":\"move\",\"movedTo\":\"removed-mods\\\\alex.req\"},"
                       "\"editorRows\":[]}"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, key2);
        CHECK(has(doc, "\"action\":\"uninstall\",\"key\":\"alex.r2-imp@1.0.1\""));
        CHECK(has(doc, "\"entityCount\":2,") && has(doc, "\"tagRemoved\":true,\"vanillaAfter\":true"));
        CHECK(has(doc, "\"install\":{\"folder\":\"alex.r2-imp@1.0.1\",\"disposition\":\"move\","
                       "\"movedTo\":\"removed-mods\\\\alex.r2-imp@1.0.1\"},"
                       "\"editorRows\":[\"mods/alex.r2-imp/imp_copy\",\"mods/alex.r2-imp/imp_copy_b\"]}"));
        free(doc);
        CHECK(ent_alive(1) && ent_alive(2));
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "nobody");
        CHECK(has(doc, "\"ok\":false") && has(doc, "nobody is not installed."));
        free(doc);

        printf("-- the console verbs share the dry runs and actions; --yes performs them\n");
        {
            const char *map_argv[] = { "map" };
            const char *rm_argv[] = { "map", "remove", "alex.r2-imp@1.0.1" };
            const char *un_argv[] = { "uninstall", "alex.r2-imp@1.0.1" };
            const char *un_yes[] = { "uninstall", "alex.r2-imp@1.0.1", "--yes" };
            const char *bad[] = { "map", "frob" };
            const char *other[] = { "list" };
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: map: Imp Arena (id ABCDEF0123456789ABCD), 5 entities, last saved "));
            CHECK(has(g_doctor, "MODS: map: published as item item123; its mods are uploaded (a claim is stored)\n"));
            CHECK(has(g_doctor, "MODS: map: search tag mods: present\n"));
            CHECK(has(g_doctor, "MODS: map: mod alex.r2-imp@1.0.1 (alex.r2-imp@1.0.1, installed): 2 placed entities, 3 wire or "
                                "timeline reference(s)\n"));
            CHECK(has(g_doctor, "MODS: map:   entity Gate imp #2\n") && has(g_doctor, "MODS: map:   timeline Wave 2 #4\n"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(3, rm_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: map remove: deletes 2 placed entities from Imp Arena (one undo step)\n"));
            CHECK(has(g_doctor, "MODS: map remove: severs 3 wire or timeline reference(s) to those entities\n"));
            CHECK(has(g_doctor, "MODS: map remove: removes the search tag mods (the last mod the map uses)\n"));
            CHECK(has(g_doctor, "MODS: map remove alex.r2-imp@1.0.1: dry run; add --yes to do it\n"));
            CHECK(ent_alive(1) && ent_alive(2));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, un_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: uninstall: removes 2 Toybox row(s) from the editor\n"));
            CHECK(has(g_doctor, "MODS: uninstall: moves the folder mods\\alex.r2-imp@1.0.1 to removed-mods\\ (kept there until "
                                "the removed mods are emptied)\n"));
            CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp@1.0.1: dry run; add --yes to do it\n"));
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, key2);
            CHECK(mt_exists(path) && ent_alive(1));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, bad, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods map"));
            CHECK(sh_modui_command(1, other, doctor_line, NULL) == 0);
            g_main = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == -1);
            g_main = 1;
            /* --yes: the map cleaned first, then the folder, then the rescan. */
            g_doctor[0] = 0;
            g_deleted_n = 0;
            {
                unsigned rescans = (unsigned)g_rescans;
                CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1);
                CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp@1.0.1: Uninstalled R2 Imp copy and removed it from the open map. "
                                    "Its folder was moved to removed-mods\\alex.r2-imp@1.0.1.\n"));
                CHECK((unsigned)g_rescans == rescans + 1);
            }
            CHECK(g_deleted_n == 2 && !ent_alive(1) && !ent_alive(2) && g_tagged == 0);
            CHECK(!mt_exists(path));
            doc = list_doc();
            CHECK(!has(doc, "\"key\":\"alex.r2-imp@1.0.1\"") && has(doc, "\"mods\":[],\"vanilla\":true}"));
            free(doc);
        }

        printf("-- the recycle bin: the uninstalled folder is kept and recorded\n");
        {
            char binned[MAX_PATH * 2], *rec = NULL;
            _snprintf_s(binned, sizeof binned, _TRUNCATE, "%s\\store\\removed-mods\\alex.r2-imp@1.0.1\\mod.json", tmp);
            CHECK(mt_exists(binned));
            CHECK(sh_mb_record_get(g_root, "removed", &rec) == 1 && rec && strstr(rec, "\"alex.r2-imp@1.0.1\"") &&
                  strstr(rec, "\"R2 Imp copy\"") && strstr(rec, hash2) && strstr(rec, "\"removedAt\"") && strstr(rec, "\"seq\""));
            free(rec);
            doc = list_doc();
            CHECK(has(doc, "{\"folder\":\"alex.r2-imp@1.0.1\",\"id\":\"alex.r2-imp\",\"version\":\"1.0.1\",\"name\":\"R2 Imp copy\","));
            CHECK(has(doc, "\"restorable\":true"));
            free(doc);
        }

        printf("-- Ctrl+Z brings the entities back: the tick reinstates the mod, once\n");
        {
            unsigned rescans = (unsigned)g_rescans, refreshes = (unsigned)g_refreshes;
            g_toast[0] = g_print[0] = 0;
            g_toasts = 0;
            /* Nothing back yet: nothing happens. */
            CHECK(sh_modui_poll_due() == 1 && sh_modui_poll_main(NULL) == 1);
            CHECK(!mt_exists(path) && (unsigned)g_rescans == rescans && g_toasts == 0);
            /* The undo, during Play: the tick waits for the editor. */
            g_ents[1].alive = g_ents[2].alive = 1;
            g_placed[0] = "mods/alex.r2-imp/imp_copy";     /* what the editor's entities name again */
            g_placed[1] = "player/coop/player_start";
            g_placed_count = 2;
            g_busy = "Play is running";
            CHECK(sh_modui_poll_main(NULL) == 0 && !mt_exists(path));
            g_busy = NULL;
            CHECK(sh_modui_poll_main(NULL) == 1);
            CHECK(mt_exists(path));                                   /* mods\alex.r2-imp@1.0.1 again */
            CHECK((unsigned)g_rescans == rescans + 1 && (unsigned)g_refreshes == refreshes + 1);
            CHECK(g_toasts == 1 && !strcmp(g_toast, "Mods: R2 Imp copy was reinstalled because the map still uses it."));
            CHECK(!strcmp(g_print, "MODS: R2 Imp copy was reinstalled because the map still uses it."));
            CHECK(sh_modui_poll_main(NULL) == 1 && g_toasts == 1);   /* once */
            doc = list_doc();
            CHECK(bundle_has(doc, key2, "\"state\":\"installed\""));
            CHECK(has(doc, "\"removed\":{") && !has(doc, "{\"folder\":\"alex.r2-imp@1.0.1\""));
            free(doc);
            /* The save writes its row again. */
            {
                size_t len = 0, out_len = 0;
                char *t = map_text(&len), *saved = sh_modmanifest_on_save(t, len, &out_len);
                CHECK(saved && strstr(saved, "smpmod1.0.1") && strstr(saved, hash2));
                if (saved) HeapFree(GetProcessHeap(), 0, saved);
            }
        }

        printf("-- a map load that names the bundle, and a save that places its names, reinstate it too\n");
        {
            const char *un_yes[] = { "uninstall", "alex.r2-imp@1.0.1", "--yes" };
            const char *hashes[1];
            unsigned refreshes;
            hashes[0] = hash2;
            CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1 && !mt_exists(path));
            /* The load: the index at once, the game-side refresh and the toast at the next editor tick. */
            refreshes = (unsigned)g_refreshes;
            g_toasts = 0;
            CHECK(sh_modserve_restore(hashes, 1, NULL, 0, SH_MODSERVE_RESTORE_LOAD) == 1 && mt_exists(path));
            CHECK((unsigned)g_refreshes == refreshes && g_toasts == 0);
            CHECK(sh_modserve_restore(hashes, 1, NULL, 0, SH_MODSERVE_RESTORE_LOAD) == 0);   /* installed: nothing */
            CHECK(sh_modui_poll_main(NULL) == 1 && (unsigned)g_refreshes == refreshes + 1 && g_toasts == 1);
            /* The save: the placed names are under mods/alex.r2-imp/, no installed folder declares the id. */
            g_ents[1].alive = g_ents[2].alive = 1;
            open_test_map();
            CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1 && !mt_exists(path));
            g_ents[1].alive = g_ents[2].alive = 1;
            {
                size_t len = 0, out_len = 0;
                char *t = map_text(&len), *saved = sh_modmanifest_on_save(t, len, &out_len);
                CHECK(mt_exists(path));
                CHECK(saved && strstr(saved, "smpmod1.0.1") && strstr(saved, hash2));
                if (saved) HeapFree(GetProcessHeap(), 0, saved);
            }
            CHECK(sh_modui_poll_main(NULL) == 1 && g_toasts == 2);
            /* The folder name taken meanwhile: it comes back under a free name. */
            open_test_map();
            CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1 && !mt_exists(path));
            CreateDirectoryA(path, NULL);
            mt_write(path, "notes.txt", "someone else's folder");
            CHECK(sh_modserve_restore(hashes, 1, NULL, 0, SH_MODSERVE_RESTORE_LOAD) == 1);
            {
                char again[MAX_PATH * 2];
                _snprintf_s(again, sizeof again, _TRUNCATE, "%s\\alex.r2-imp\\mod.json", g_root);
                CHECK(mt_exists(again));                               /* its id */
                mt_remove_tree(path);
                _snprintf_s(again, sizeof again, _TRUNCATE, "%s\\alex.r2-imp", g_root);
                CHECK(MoveFileA(again, path));
                doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
                free(doc);
            }
            CHECK(sh_modui_poll_main(NULL) == 1);
        }

        printf("-- the console: removed, removed empty, removed restore\n");
        {
            const char *un_yes[] = { "uninstall", "alex.r2-imp@1.0.1", "--yes" };
            const char *ls[] = { "removed" };
            const char *empty_dry[] = { "removed", "empty" };
            const char *restore[] = { "removed", "restore", "alex.r2-imp" };
            const char *bad[] = { "removed", "frob" };
            open_test_map();
            CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1 && !mt_exists(path));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, ls, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: removed: ") && has(g_doctor, "MODS: removed:   alex.r2-imp@1.0.1: alex.r2-imp@1.0.1 (R2 Imp copy), "));
            CHECK(has(g_doctor, "MODS: removed:   twin-b: alex.twin@2.0.0 (Twin), "));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, empty_dry, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: removed empty: deletes removed-mods\\alex.r2-imp@1.0.1 (alex.r2-imp, "));
            CHECK(has(g_doctor, "dry run; add --yes to delete "));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(3, restore, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: removed restore alex.r2-imp: Reinstalled R2 Imp copy: removed-mods\\alex.r2-imp@1.0.1 is "
                                "mods\\alex.r2-imp@1.0.1 again"));
            CHECK(mt_exists(path));
            g_doctor[0] = 0;
            {
                const char *nobody[] = { "removed", "restore", "nobody" };
                CHECK(sh_modui_command(3, nobody, doctor_line, NULL) == 1 && has(g_doctor, "removed-mods holds no folder of nobody."));
            }
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, bad, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods removed"));
        }

        printf("-- map add: a mod used by the whole map without placing anything, and the map scope that follows it\n");
        {
            const char *add_argv[] = { "map", "add", "alex.req" };
            const char *map_argv[] = { "map" };
            const char *bad_add[] = { "map", "add" };
            sh_modmanifest_list att;
            char *t, *saved;
            size_t len = 0, out_len = 0;
            int applies;
            sh_modmanifest_reset();
            open_test_map();
            sh_modserve_scope_enable(1);
            applies = g_scope_applies;
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"action\":\"mapAddMod\",\"ok\":true,\"message\":\"Gates is now used by the whole of Imp Arena."));
            free(doc);
            /* Its requirements were out of scope with no map listing it; now they apply at once. */
            CHECK(g_scope_applies == applies + 1 && !strcmp(g_scope_phase, "map add"));
            CHECK(sh_modmanifest_current_attached(&att) == 1 && sh_modserve_bundle_state(att.rows[0].bundle, NULL, 0) == 1 &&
                  !strcmp(att.rows[0].id, "alex.req"));
            sh_modmanifest_list_free(&att);
            doc = list_doc();
            CHECK(has(doc, "\"key\":\"alex.req\",\"id\":\"alex.req\"") && has(doc, "\"attached\":true,\"scope\":\"map\",\"active\":true"));
            CHECK(has(doc, "\"otherKey\":\"\",\"nameCount\":0,\"names\":[],\"attached\":true}"));
            CHECK(has(doc, "\"scope\":\"map\",\"scopeAuto\":false,\"scoped\":true"));
            CHECK(has(doc, "\"vanilla\":false"));
            free(doc);
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"ok\":true,\"message\":\"Gates is already used by the whole of Imp Arena.\""));
            free(doc);
            g_doctor[0] = 0;
            CHECK(sh_modui_command(3, add_argv, doctor_line, NULL) == 1 &&
                  has(g_doctor, "MODS: map add alex.req: Gates is already used by the whole of Imp Arena.\n"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, bad_add, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods map add"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "(alex.req, installed): map-level (attached to the whole map), 0 placed entities, 0 wire or "
                                "timeline reference(s); scope map, active\n"));
            CHECK(has(g_doctor, "MODS: map: scope: alex.req@1.0.0: map (0 shadows, requirements): active in the open map\n"));
            /* The save writes its row as a map-level row; nothing places it. */
            t = map_text(&len);
            saved = sh_modmanifest_on_save(t, len, &out_len);
            CHECK(saved && strstr(saved, "smpmod1.") && strstr(saved, "alex.req") && strstr(saved, "\\\"attached\\\":true"));
            if (saved) HeapFree(GetProcessHeap(), 0, saved);
            /* The dry run and the removal: nothing to delete, the row goes. */
            doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, "alex.req");
            CHECK(has(doc, "\"entityCount\":0") && has(doc, "\"manifestRow\":true") && has(doc, "\"published\":true,\"attached\":true}"));
            free(doc);
            applies = g_scope_applies;
            g_delete_calls = 0;
            doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "alex.req");
            CHECK(has(doc, "\"ok\":true,\"message\":\"Removed Gates from Imp Arena: no entity used it, so only its row was dropped"));
            free(doc);
            CHECK(g_delete_calls == 0 && sh_modmanifest_current_attached(&att) == 0 && g_scope_applies == applies + 1);
            sh_modmanifest_list_free(&att);
            /* Refusals. */
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "nobody");
            CHECK(has(doc, "\"ok\":false,\"message\":\"nobody is not installed.\""));
            free(doc);
            g_busy = "Play is running; return to the editor first";
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"ok\":false,\"message\":\"Not now: Play is running; return to the editor first.\""));
            free(doc);
            g_busy = NULL;
            /* The editor tick: in the map the listing holds; once the editor has left it (the hub) it goes. */
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            free(doc);
            CHECK(sh_modserve_scope_map_open());
            CHECK(sh_modui_poll_main(NULL) == 1);
            applies = g_scope_applies;
            /* Settings from the Start menu (P27): the editor screen goes down but its state keeps
             * running over the open map, so the listing stays and nothing is applied, on the way
             * there or back. */
            g_map_on = 0;
            g_map_left = 0;
            CHECK(sh_modui_poll_main(NULL) == 0 && sh_modui_poll_main(NULL) == 0);
            CHECK(sh_modserve_scope_map_open() && g_scope_applies == applies);
            g_map_on = 1;
            CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open() && g_scope_applies == applies);
            /* The hub: the editor no longer runs a state. */
            g_map_on = 0;
            g_map_left = 1;
            CHECK(sh_modui_poll_main(NULL) == 0);
            CHECK(!sh_modserve_scope_map_open() && g_scope_applies == applies + 1 && !strcmp(g_scope_phase, "map exit"));
            {
            int placed = g_placed_count;
            g_placed_count = -1;
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"ok\":false,\"message\":\"No map is open in the editor.\""));
            free(doc);
            g_placed_count = placed;
            }
            /* Back in the editor without a load: the listing is set again from the open map. */
            g_map_on = 1;
            CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open() && !strcmp(g_scope_phase, "editor"));
            sh_modserve_scope_enable(0);
            sh_modmanifest_reset();
        }

        printf("-- rebuild: the folder built again from its files, then a rescan\n");
        {
            unsigned rescans = (unsigned)g_rescans;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.req", g_root);
            doc = run_doc(SH_MODS_ACT_REBUILD, "alex.req");
            CHECK(has(doc, "\"action\":\"rebuild\",\"ok\":true,\"message\":\"Rebuilt alex.req: nothing changed (1 files).\""));
            free(doc);
            mt_append(path, "assets\\generated\\decls\\entitydef\\mods\\alex.req\\twin.decl", "// changed\r\n");
            doc = run_doc(SH_MODS_ACT_REBUILD, "alex.req");
            CHECK(has(doc, "\"ok\":true,\"message\":\"Rebuilt alex.req: build 2, 1 files.\""));
            free(doc);
            CHECK((unsigned)g_rescans == rescans + 2);
            doc = run_doc(SH_MODS_ACT_REBUILD, "nobody");
            CHECK(has(doc, "\"ok\":false") && has(doc, "no longer installed"));
            free(doc);
            g_busy = "a map is loading";
            doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.req");
            CHECK(has(doc, "\"ok\":false,\"message\":\"Not now: a map is loading.\"") && mt_exists(path));
            free(doc);
            g_busy = NULL;
            /* A folder of that name already in removed-mods: the new one gets a suffix. */
            {
                char clash[MAX_PATH * 2];
                _snprintf_s(clash, sizeof clash, _TRUNCATE, "%s\\store\\removed-mods\\alex.req", tmp);
                CreateDirectoryA(clash, NULL);
                mt_write(clash, "readme.txt", "an older copy");
            }
            doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "alex.req");
            CHECK(has(doc, "\"movedTo\":\"removed-mods\\\\alex.req-2\""));
            free(doc);
            doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.req");
            CHECK(has(doc, "\"action\":\"uninstall\",\"ok\":true,\"message\":\"Uninstalled Gates. Its folder was moved to "
                           "removed-mods\\\\alex.req-2.\"") && !mt_exists(path));
            free(doc);
        }

        printf("-- Empty: only the folders the confirmation listed, then all; the records go with them\n");
        {
            char probe[MAX_PATH * 2], *rec = NULL;
            doc = list_doc();
            CHECK(has(doc, "{\"folder\":\"alex.req\",\"id\":\"alex.req\",\"version\":\"\",\"name\":\"\",\"bytes\":13,"));   /* not recorded */
            CHECK(has(doc, "{\"folder\":\"alex.req-2\",\"id\":\"alex.req\",\"version\":\"1.0.0\",\"name\":\"Gates\""));
            free(doc);
            doc = run_doc(SH_MODS_ACT_REMOVED_EMPTY, "twin-b\nalex.req\n");
            CHECK(has(doc, "\"action\":\"removedEmpty\",\"ok\":true,\"message\":\"Deleted 2 removed mod folder(s), ") &&
                  has(doc, "\"deleted\":2,\"failed\":0"));
            free(doc);
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\store\\removed-mods\\twin-b", tmp);
            CHECK(!mt_exists(probe));
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\store\\removed-mods\\alex.req-2", tmp);
            CHECK(mt_exists(probe));
            CHECK(sh_mb_record_get(g_root, "removed", &rec) == 1 && rec && !strstr(rec, "\"twin-b\"") && strstr(rec, "\"alex.req-2\""));
            free(rec);
            rec = NULL;
            doc = run_doc(SH_MODS_ACT_REMOVED_EMPTY, "");
            CHECK(has(doc, "\"ok\":true") && has(doc, "\"failed\":0"));
            free(doc);
            CHECK(!mt_exists(probe));
            CHECK(sh_mb_record_get(g_root, "removed", &rec) != 1 || !rec || !strstr(rec, "\"id\""));
            free(rec);
            doc = list_doc();
            CHECK(has(doc, "\"removed\":{\"folder\":\"") && has(doc, "\"count\":0,\"bytes\":0,\"items\":[]}"));
            free(doc);
            doc = run_doc(SH_MODS_ACT_REMOVED_EMPTY, "");
            CHECK(has(doc, "\"ok\":false,\"message\":\"Nothing to empty: removed-mods holds no folder.\""));
            free(doc);
            /* An empty bin arms nothing: the save step does not read the map for it. */
            CHECK(sh_modserve_restore(NULL, 0, NULL, 0, SH_MODSERVE_RESTORE_PLAY) == 0);
        }
        g_map_on = 0;
        sh_modui_test_set_saves_root(NULL);
    }

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
