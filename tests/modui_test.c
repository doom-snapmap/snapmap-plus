/* modui_test.c -- the Mods tab's backend (modui.c) against the real store,
 * serving index and map manifest in a temporary folder: the document's shape
 * on an empty store, an install through the tab's action, one row per mod
 * folder (two folders declaring one id both listed as duplicates, a package a
 * map delivered listed like any other), "installed" and "attention" with their
 * causes, the Migrate state and action with the slot's WAIT, Remove (a folder
 * the product made and the author's own folder both moved to the Recycle Bin,
 * the outright delete when the bin refuses one and the result saying so, a
 * mod the open map uses refused), the open map's manifest rows and placed
 * closure, the conservative removal of covered builds, `sh_mods doctor` (every
 * bundle with its state, conflicts with their winners, shadows, duplicate ids,
 * migration, verification), the main-thread refusal, the slot's size query,
 * payload v3 (what each mod carries, the open map's details from its saved
 * game.details and the record's links and claims, each mod it uses with its
 * entities, wires and Timeline references), Remove from map and Uninstall
 * with their dry runs against a fake editor that records deletions (Play and
 * a load refused, one batch, the row and the search tag dropped, the next
 * save writing no row), the permanent Uninstall (the saved maps that use the
 * mod counted from their map.decl, the undo history cleared after the
 * delete's own commit, the folder moved to the Recycle Bin, nothing brought back by a load, a
 * save or a tick), the asset rule's row states, Rebuild, the console verbs
 * with their --yes gate, and that every key the page's v3 fixture uses is a
 * key the backend writes.
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
#include "modregistry.h"
#include "mod_test_util.h"
#include "editor_delete.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static int g_echo;
static char g_logbuf[1 << 16];   /* every backend line since the last clear */
static size_t g_logat;
void backend_log(const char *message)
{
    size_t n;
    if (g_echo && message) printf("    %s\n", message);
    if (!message) return;
    n = strlen(message);
    if (g_logat + n + 2 >= sizeof g_logbuf) g_logat = 0;
    memcpy(g_logbuf + g_logat, message, n);
    g_logat += n;
    g_logbuf[g_logat++] = '\n';
    g_logbuf[g_logat] = 0;
}
static void log_clear(void) { g_logat = 0; g_logbuf[0] = 0; }

/* The map manifest's load step activates bundles through the serving layer's
 * engine half; the tab only reads the record it keeps. */
static int g_activations;
static char g_act_last[8][65];   /* the last activation's builds */
static int g_act_last_n;
int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    int i;
    if (n > 0) g_activations++;
    g_act_last_n = n;
    for (i = 0; i < n && i < 8; i++) strcpy_s(g_act_last[i], sizeof g_act_last[i], bundle_hashes[i]);
    return 0;
}

/* ---- engine stand-ins ------------------------------------------------------------------------ */

static const char *const STOCK[] = {
    "generated/decls/snapeditorentitydef/demons/demon_base.decl",
    "generated/decls/material/textures/guis/snapmaps/entity_icons/demons/imp_enc.decl",
    "generated/decls/entitydef/ai/demon/imp_coop.decl",
    "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",   /* an extends target */
    "generated/decls/entitydef/snapmaps/placeablesnapaiencounter/imp.decl",
    "generated/decls/renderprog/skinvmtr.decl",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/rage_taunt.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward01.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward02.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/idle.bmd6anim",
    "generated/decls/entitydef/monster/imp.decl",
    /* what New mod's example crate names: the game's SnapMap crate */
    "cooked/model/models/snapmaps/props/new/crate01.bmodel",
    "generated/decls/material/textures/guis/snapmaps/entity_icons/prop/crate01.decl",
    "generated/decls/snapeditorentitydef/prop/prop_base.decl",
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
static const char *g_opened_from;   /* a rawmap the swap opened, not saved yet (sh_rawmap_open_unsaved) */
static int fake_map_info(sh_modui_map_info *out)
{
    if (!g_map_on) return 0;
    memset(out, 0, sizeof *out);
    /* After a rawmap open the edit still holds the borrowed saved map's ids. */
    strcpy_s(out->local_id, sizeof out->local_id, "ABCDEF0123456789ABCD");
    strcpy_s(out->cloud_id, sizeof out->cloud_id, "6ae40b378bf5037d3d63bd23");
    out->tagged = g_tagged;
    if (g_opened_from) strcpy_s(out->opened_from, sizeof out->opened_from, g_opened_from);
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

/* The undo clear (editor_undo.h): available while a map is open, how often
 * it ran, and the editor's pending delete commit the tick waits for. */
static int g_undo_clears, g_undo_available = 1, g_undo_pending;
static int fake_clear_undo(int probe)
{
    if (!g_map_on) return 0;
    if (!g_undo_available) return -1;
    if (!probe) g_undo_clears++;
    return 1;
}
static int fake_undo_pending(void) { return g_undo_pending; }

/* The editor's dirty mark (editor_undo.h sh_editor_mark_modified): how often, and the last label. */
static int g_marks;
static char g_mark_label[64];
static int fake_mark_modified(const char *label)
{
    g_marks++;
    strncpy_s(g_mark_label, sizeof g_mark_label, label ? label : "", _TRUNCATE);
    return g_map_on ? 1 : 0;
}

/* The map scope's game-side apply (sh_modserve_scope_apply): how often, and the last phase. */
static int g_scope_applies;
static char g_scope_phase[64];
static int fake_scope_apply(const char *phase)
{
    g_scope_applies++;
    strncpy_s(g_scope_phase, sizeof g_scope_phase, phase ? phase : "", _TRUNCATE);
    return 1;
}

/* The activation for an author's change (sh_modserve_scope_activate). */
static int g_scope_activates;
static int fake_scope_activate(const char *phase)
{
    g_scope_activates++;
    return fake_scope_apply(phase);
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

/* A hub launch's pre-load pass (modserve.h sh_modserve_launch_preload):
 * when it ran, and what the launch had set by then. */
static int g_preloads, g_preload_mode = -1;
static char g_preload_state[16], g_preload_why[40];
static int fake_preload(const char *why)
{
    g_preloads++;
    g_preload_mode = sh_modserve_scope_mode_get();
    strcpy_s(g_preload_state, sizeof g_preload_state, sh_modui_test_launch_state());
    strncpy_s(g_preload_why, sizeof g_preload_why, why ? why : "", _TRUNCATE);
    return 0;
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
    e.scope_activate = fake_scope_activate;
    e.map_left = fake_map_left;
    e.clear_undo = fake_clear_undo;
    e.mark_modified = fake_mark_modified;
    e.undo_pending = fake_undo_pending;
    e.launch_preload = fake_preload;
    sh_modui_bind(&e);
}

/* The manifest's substitute op (modpublished.c sh_modpub_editor_substitute):
 * the installed build of the row's key. */
static char g_subst_to[80];
static int fake_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed, char *why, size_t why_cap)
{
    (void)placed;
    (void)row;
    if (why && why_cap) why[0] = 0;
    if (!g_subst_to[0]) return 0;
    strcpy_s(hash_out, hash_cap, g_subst_to);
    return 1;
}

static const char GONE_REQ[] = "00000000000000000000000000000000000000000000000000000000000000ab";

/* An editor load of the open test map with one row of `id` at `gone`
 * (map-level when attached): the manifest substitutes the installed build. */
static int load_gone_row(const char *id, const char *gone, int attached)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    char err[256], *text, *loaded;
    size_t len = 0, tl = 0, ll = 0;
    const char *map;
    memset(&rows, 0, sizeof rows);
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, id);
    strcpy_s(row.version, sizeof row.version, "0.9.0");
    strcpy_s(row.bundle, sizeof row.bundle, gone);
    row.size = 10;
    row.attached = attached;
    if (!sh_modmanifest_list_add(&rows, &row)) return 0;
    map = map_text(&len);
    text = sh_modmanifest_write(map, len, &rows, &tl, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    if (!text) { printf("    write: %s\n", err); return 0; }
    sh_modmanifest_set_substitute(fake_substitute);
    sh_modmanifest_set_editor_load(1);
    loaded = sh_modmanifest_prepare_load(text, tl, &ll);
    sh_modmanifest_set_editor_load(0);
    sh_modmanifest_set_substitute(NULL);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    HeapFree(GetProcessHeap(), 0, text);
    return 1;
}

/* An editor load of the open test map with rows of `ids` at `hashes`; with
 * `subst`, the manifest substitutes each row by `subst_to` of its id. */
static const char *g_sub_ids[4], *g_sub_to[4];
static int fake_substitute_by_id(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed, char *why, size_t why_cap)
{
    (void)placed;
    int i;
    if (why && why_cap) why[0] = 0;
    for (i = 0; i < 4 && g_sub_ids[i]; i++)
        if (!strcmp(row->id, g_sub_ids[i])) { strcpy_s(hash_out, hash_cap, g_sub_to[i]); return 1; }
    return 0;
}
static int load_rows(const char *const *ids, const char *const *hashes, int n, int subst)
{
    sh_modmanifest_list rows;
    char err[256], *text, *loaded;
    size_t len = 0, tl = 0, ll = 0;
    const char *map;
    int i;
    memset(&rows, 0, sizeof rows);
    for (i = 0; i < n; i++) {
        sh_modmanifest_row row;
        memset(&row, 0, sizeof row);
        strcpy_s(row.id, sizeof row.id, ids[i]);
        strcpy_s(row.version, sizeof row.version, "0.9.0");
        strcpy_s(row.bundle, sizeof row.bundle, hashes[i]);
        row.size = 10;
        row.attached = 1;
        if (!sh_modmanifest_list_add(&rows, &row)) return 0;
    }
    map = map_text(&len);
    text = sh_modmanifest_write(map, len, &rows, &tl, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    if (!text) { printf("    write: %s\n", err); return 0; }
    if (subst) sh_modmanifest_set_substitute(fake_substitute_by_id);
    sh_modmanifest_set_editor_load(subst);
    loaded = sh_modmanifest_prepare_load(text, tl, &ll);
    sh_modmanifest_set_editor_load(0);
    sh_modmanifest_set_substitute(NULL);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    HeapFree(GetProcessHeap(), 0, text);
    return 1;
}

/* The full "bundle" hash a result document names first, into `out`. */
static int doc_bundle(const char *doc, char *out, size_t cap)
{
    const char *h = doc ? strstr(doc, "\"bundle\":\"") : NULL, *e;
    if (!h) return 0;
    h += 10;
    e = strchr(h, '"');
    if (!e || (size_t)(e - h) >= cap) return 0;
    memcpy(out, h, (size_t)(e - h));
    out[e - h] = 0;
    return 1;
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

/* Another program holds the record open without read sharing for `ms`. */
typedef struct ui_hold { char path[MAX_PATH]; DWORD ms; HANDLE taken; } ui_hold;
static DWORD WINAPI ui_hold_thread(LPVOID p)
{
    ui_hold *r = (ui_hold *)p;
    HANDLE h = CreateFileA(r->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetEvent(r->taken);
    Sleep(r->ms);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(r);
    return 0;
}
static HANDLE ui_hold_record(const char *path, DWORD ms)
{
    ui_hold *r = (ui_hold *)calloc(1, sizeof *r);
    HANDLE t, taken;
    if (!r) return NULL;
    strncpy_s(r->path, sizeof r->path, path, _TRUNCATE);
    r->ms = ms;
    r->taken = taken = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, ui_hold_thread, r, 0, NULL);
    WaitForSingleObject(taken, 5000);
    CloseHandle(taken);
    return t;
}

static char *run_doc(int action, const char *arg)
{
    if (sh_modui_run(action, arg) < 0) return NULL;
    return fetch();
}

/* Install by hash (DESIGN-additive 4) names the second build of a key's
 * folder <key>@<hash16>, not <key>@<version>: once the test installs it,
 * g_k2 holds that folder name, and K2 rewrites the folder name the
 * expectations below spell as "alex.r2-imp@1.0.1" (never the entry key
 * "alex.r2-imp@1.0.1+1", nor the id@version a "map: mod" line starts with). */
static char g_k2[160];
static const char *K2(const char *lit)
{
    static char ring[8][4096];
    static int at;
    static const char from[] = "alex.r2-imp@1.0.1";
    char *out = ring[at++ & 7];
    const char *p = lit;
    size_t n = 0, fl = sizeof from - 1;
    if (!g_k2[0] || !lit) return lit;
    while (*p && n + 200 < sizeof ring[0]) {
        if (!strncmp(p, from, fl) && p[fl] != '+' && !(p - lit >= 9 && !strncmp(p - 9, "map: mod ", 9))) {
            size_t kl = strlen(g_k2);
            memcpy(out + n, g_k2, kl);
            n += kl;
            p += fl;
        } else out[n++] = *p++;
    }
    out[n] = 0;
    return out;
}
static int has(const char *doc, const char *needle) { needle = K2(needle); return doc && strstr(doc, needle) != NULL; }

/* A string field of the row whose key is `key`, the JSON escapes undone. */
static int row_str(const char *doc, const char *key, const char *field, char *out, size_t cap)
{
    char k[256], f[64];
    const char *at;
    size_t n = 0;
    if (!doc || !cap) return 0;
    _snprintf_s(k, sizeof k, _TRUNCATE, "{\"key\":\"%s\"", key);
    _snprintf_s(f, sizeof f, _TRUNCATE, "\"%s\":\"", field);
    at = strstr(doc, k);
    if (!at || !(at = strstr(at, f))) return 0;
    for (at += strlen(f); *at && *at != '"' && n + 1 < cap; at++) {
        if (*at == '\\' && at[1]) at++;
        out[n++] = *at;
    }
    out[n] = 0;
    return *at == '"';
}

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

/* `needle` anywhere in the row of `key`, nested objects included (up to the next row). */
static int row_has(const char *doc, const char *key, const char *needle)
{
    char k[256];
    const char *at, *next, *hit;
    if (!doc) return 0;
    _snprintf_s(k, sizeof k, _TRUNCATE, "{\"key\":\"%s\"", key);
    if (!(at = strstr(doc, k))) return 0;
    next = strstr(at + 1, "{\"key\":\"");
    hit = strstr(at, needle);
    return hit && (!next || hit < next);
}

static unsigned count_of(const char *doc, const char *needle)
{
    unsigned n = 0;
    const char *p = doc;
    size_t len = strlen(needle);
    while (p && (p = strstr(p, needle)) != NULL) { n++; p += len; }
    return n;
}

/* The documents the fixture's keys are checked against: one document cannot
 * show every state at once (two builds of a key, a map with a product
 * version), so the test keeps the ones that do. */
static char *g_union;
static size_t g_union_len;
static void union_add(const char *doc)
{
    size_t n = doc ? strlen(doc) : 0;
    char *g = n ? (char *)realloc(g_union, g_union_len + n + 2) : NULL;
    if (!g) return;
    g_union = g;
    memcpy(g_union + g_union_len, doc, n);
    g_union_len += n;
    g_union[g_union_len++] = '\n';
    g_union[g_union_len] = 0;
}

/* Every "key": the page's fixture uses must be written by the backend. The
 * members of a "registries" object are a mod's own data, not the payload's
 * keys, and are skipped. */
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
            if (!strcmp(key, "registries")) {
                /* skip the object's members */
                const char *o = strchr(q, '{');
                int depth = 0;
                for (; o && *o; o++) {
                    if (*o == '{') depth++;
                    else if (*o == '}' && --depth == 0) break;
                }
                if (o && *o) { p = o + 1; continue; }
            }
        }
        p = q + 1;
    }
    printf("  fixture keys checked: %u, missing: %u\n", seen, missing);
    CHECK(seen > 25 && missing == 0);
    free(text);
}

/* ---- maps ------------------------------------------------------------------------------------ */

/* A local save's map.decl as the game writes it: a 4-byte checksum, then a
 * zlib stream (here stored blocks) of the map text. */
static void put_saved_decl(const char *dir, const char *text, size_t n)
{
    char path[MAX_PATH * 2];
    FILE *fp;
    unsigned a = 1, b = 0;
    size_t i, at = 0;
    unsigned char head[6] = { 0x52, 0xA9, 0x05, 0xD8, 0x78, 0x01 }, tail[4];
    for (i = 0; i < n; i++) { a = (a + (unsigned char)text[i]) % 65521; b = (b + a) % 65521; }
    tail[0] = (unsigned char)(b >> 8); tail[1] = (unsigned char)b; tail[2] = (unsigned char)(a >> 8); tail[3] = (unsigned char)a;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\map.decl", dir);
    if (fopen_s(&fp, path, "wb") || !fp) { CHECK(0); return; }
    fwrite(head, 1, sizeof head, fp);
    do {
        size_t len = n - at > 65535 ? 65535 : n - at;
        unsigned char bh[5];
        bh[0] = at + len == n ? 1 : 0;
        bh[1] = (unsigned char)len; bh[2] = (unsigned char)(len >> 8);
        bh[3] = (unsigned char)~len; bh[4] = (unsigned char)(~len >> 8);
        fwrite(bh, 1, sizeof bh, fp);
        fwrite(text + at, 1, len, fp);
        at += len;
    } while (at < n);
    fwrite(tail, 1, sizeof tail, fp);
    fclose(fp);
}

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

/* The game's text of the decl the edits case copies (a fake installed-content reader). */
static const char IMP_STOCK[] = "{\r\n\tinherit = \"ai/demon/imp\";\r\n}";
static int imp_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    (void)c;
    if (_stricmp(source, "generated/decls/entitydef/monster/imp.decl")) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(IMP_STOCK, strlen(IMP_STOCK), sha);
    *size = strlen(IMP_STOCK);
    return SH_MB_STOCK_OK;
}
static int imp_read(void *c, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    (void)c; (void)sha;
    if (_stricmp(source, "generated/decls/entitydef/monster/imp.decl")) return SH_MB_STOCK_ABSENT;
    *len = strlen(IMP_STOCK);
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len);
    if (!*bytes) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, IMP_STOCK, *len);
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops g_imp_ops = { NULL, NULL, imp_identify, NULL, imp_read };

/* The Files view's reader: the same game decl, verified, and the archive that holds it. */
static int files_verify(void *c, const char *source, const char *sha, unsigned long long *size)
{
    char want[SH_MB_HASH_HEX + 1];
    (void)c;
    if (_stricmp(source, "generated/decls/entitydef/monster/imp.decl")) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(IMP_STOCK, strlen(IMP_STOCK), want);
    if (_stricmp(sha, want)) return SH_MB_STOCK_MISMATCH;
    if (size) *size = strlen(IMP_STOCK);
    return SH_MB_STOCK_OK;
}
static int files_container(void *c, const char *source, const char *sha, char *out, size_t cap)
{
    (void)c; (void)sha;
    if (_stricmp(source, "generated/decls/entitydef/monster/imp.decl")) return SH_MB_STOCK_ABSENT;
    strcpy_s(out, cap, "gameresources");
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops g_files_ops = { NULL, NULL, imp_identify, files_verify, imp_read, files_container };

static void put_shadow_mod(const char *folder, const char *id, const char *body)
{
    char desc[512];
    _snprintf_s(desc, sizeof desc, _TRUNCATE,
                "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"Shade\",\"author\":\"tester\","
                "\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"],"
                "\"legacy\":{\"source\":\"overrides\"}}", id);   /* a migrated package keeps its shadows */
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
    CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 shadow(s), 0 declared shadow(s), 0 edit source(s) of 0 game file(s) (never served, so neither a shadow nor a conflict)\nMODS: doctor: done\n"));

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
    CHECK(has(doc, "its Toybox rows are in the palette"));   /* the rescan's refresh rebuilt it in place */
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
    /* The asset rule: a row pins its build; the installed folder of the key
     * at another hash is another build, inert for the map (additive 2.5),
     * and the row is not served until its own build is fetched. */
    load_map_naming("alex.r2-imp", "00000000000000000000000000000000000000000000000000000000000000aa");
    doc = list_doc();
    CHECK(!has(doc, "\"state\":\"active\",\"cause\":\"\",\"key\":\"\",\"otherKey\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"cause\":\"This map uses R2 Imp copy (alex.r2-imp), which is not installed.\""));
    CHECK(has(doc, "\"inManifest\":false"));
    /* Its own state, not "skipped": installed at another build; nothing offers to move the map. */
    CHECK(has(doc, "\"state\":\"otherBuild\",\"cause\":\"This map uses"));
    CHECK(!has(doc, "\"upgrade\""));
    free(doc);
    /* No folder declares the id: missing, with the refusal's sentence. */
    load_map_naming("alex.other", "00000000000000000000000000000000000000000000000000000000000000bb");
    doc = list_doc();
    CHECK(has(doc, "\"state\":\"missing\",\"cause\":\"This map uses alex.other, which is not installed.\",\"key\":\"\","
                   "\"otherKey\":\"\""));
    free(doc);
    /* A folder declares the id but lacks a name the row lists: incomplete. */
    {
        sh_modmanifest_list rows;
        sh_modmanifest_row row;
        char err[256], *text, *loaded;
        size_t tl = 0, ll = 0;
        memset(&rows, 0, sizeof rows);
        memset(&row, 0, sizeof row);
        strcpy_s(row.id, sizeof row.id, "alex.r2-imp");
        strcpy_s(row.version, sizeof row.version, "1.0.0");
        strcpy_s(row.bundle, sizeof row.bundle, hash1);
        CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.r2-imp/imp_copy"));
        CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.r2-imp/imp_gone"));
        CHECK(sh_modmanifest_list_add(&rows, &row));
        sh_modmanifest_row_free(&row);
        text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
        loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        if (text) HeapFree(GetProcessHeap(), 0, text);
        sh_modmanifest_list_free(&rows);
        doc = list_doc();
        CHECK(has(doc, "\"state\":\"incomplete\",\"cause\":\"R2 Imp copy (alex.r2-imp) is installed but does not provide "
                       "mods/alex.r2-imp/imp_gone, which this map uses.\""));
        free(doc);
        sh_modmanifest_reset();
    }

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
    {
        const char *k = doc ? strstr(doc, "\"key\":\"alex.r2-imp@") : NULL;
        size_t kl = 0;
        if (k) { k += 7; while (k[kl] && k[kl] != '"' && kl + 1 < sizeof g_k2) kl++; memcpy(g_k2, k, kl); g_k2[kl] = 0; }
    }
    CHECK(has(doc, "\"ok\":true") && g_k2[0] && strlen(g_k2) == strlen("alex.r2-imp@") + 16);
    strcpy_s(key2, sizeof key2, g_k2);
    {
        const char *h = doc ? strstr(doc, "\"bundle\":\"") : NULL;
        if (h) { memcpy(hash2, h + 10, 64); hash2[64] = 0; }
    }
    free(doc);
    doc = list_doc();
    /* Two builds of one key are not attention (DESIGN-additive 2.5): one row
     * per hash, the newest marked, each listing every build of the key. */
    CHECK(bundle_has(doc, key2, "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false"));
    CHECK(bundle_has(doc, key1, "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false"));
    CHECK(count_of(doc, "\"newest\":true,\"builds\":[") == 1 && count_of(doc, "\"newest\":false,\"builds\":[") == 1);
    {
        /* The newest build serves the key with no map open; its folder is named
         * <key>@<hash16> since installs beside are by hash. */
        char served[128];
        _snprintf_s(served, sizeof served, _TRUNCATE, "\"servedBy\":\"%s\"", key2);
        CHECK(bundle_has(doc, key1, served));
    }
    CHECK(count_of(doc, "\"newest\":true,\"failed\":false") == 2);   /* the newest build, in each row's list */
    CHECK(count_of(doc, "\"otherBuilds\":") == 2);
    free(doc);
    dr = doctor(0);
    CHECK(has(dr, "MODS: doctor: bundle mods\\alex.r2-imp (alex.r2-imp@1.0.0+1): another bundle of its key, alex.r2-imp@1.0.1, "
                  "serves 1 of its path(s) with no map open; a map saved with this bundle still serves this one; from an "
                  "installed copy\n"));
    CHECK(has(dr, "MODS: doctor: bundle mods\\alex.r2-imp@1.0.1 (alex.r2-imp@1.0.1+1): active; from an installed copy\n"));
    CHECK(has(dr, "MODS: doctor: key alex.r2-imp: installed as several bundles: mods\\alex.r2-imp (an installed copy), "
                  "mods\\alex.r2-imp@1.0.1 (an installed copy); each map serves the bundle it was saved with (one row per hash "
                  "follows under scope)\n"));
    /* While the open map's record names the first it is kept. */
    load_map_naming("alex.r2-imp", hash1);
    doc = run_doc(SH_MODS_ACT_PRUNE_LIST, NULL);
    CHECK(has(doc, "\"action\":\"pruneList\",\"ok\":true") && has(doc, "\"candidates\":[]"));
    free(doc);
    /* The map pins the older build, installed beside a newer one with no
     * alias between them (a download lands so): an editor open keeps the pin,
     * and the tab offers nothing (owner 2026-11-09: no build management). */
    sh_modmanifest_set_editor_load(1);
    load_map_naming("alex.r2-imp", hash1);
    sh_modmanifest_set_editor_load(0);
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].bundle, hash1));
        sh_modmanifest_list_free(&cur);
    }
    doc = list_doc();
    CHECK(!has(doc, "\"upgrade\""));
    union_add(doc);   /* builds of one key */
    free(doc);
    g_marks = 0;
    /* A choice between mods' edits goes through the manifest; an invalid slot is refused. */
    doc = run_doc(SH_MODS_ACT_CHOOSE, "entitydef/player/coop/edit/handsModelMPUseDoomHands\talex.r2-imp");
    CHECK(has(doc, "\"ok\":true") && has(doc, "entitydef/player/coop/edit/handsModelMPUseDoomHands now comes from alex.r2-imp"));
    free(doc);
    CHECK(g_marks == 1 && !strcmp(g_mark_label, "Choose Mod"));
    {
        char chosen[80];
        CHECK(sh_modregistry_choices_get("entitydef/player/coop/edit/handsModelMPUseDoomHands", chosen, sizeof chosen) && !strcmp(chosen, "alex.r2-imp"));
    }
    doc = run_doc(SH_MODS_ACT_CHOOSE, "bad slot\talex.r2-imp");
    CHECK(has(doc, "\"ok\":false"));
    free(doc);
    CHECK(g_marks == 1);   /* refused: not marked */
    sh_modmanifest_reset();
    g_placed_count = 0;
    doc = run_doc(SH_MODS_ACT_PRUNE_LIST, NULL);
    CHECK(has(doc, "\"candidates\":[{\"key\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"coveredBy\":\"alex.r2-imp@1.0.1\""));
    CHECK(!has(doc, "\"key\":\"alex.r2-imp@1.0.1\""));
    free(doc);
    /* Only confirmed keys, and the newest build never. */
    doc = run_doc(SH_MODS_ACT_PRUNE_APPLY, K2("alex.r2-imp@1.0.1\n"));
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
        CHECK(bundle_has(doc, "twin-a", "\"state\":\"installed\"") &&
              bundle_has(doc, "twin-a", "\"otherBuilds\":0,\"duplicateId\":false"));
        CHECK(bundle_has(doc, "twin-b", "\"state\":\"installed\"") &&
              bundle_has(doc, "twin-b", "\"otherBuilds\":0,\"duplicateId\":false"));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: key alex.twin: installed as several bundles: mods\\twin-a (your folder), mods\\twin-b "
                      "(your folder); each map serves the bundle it was saved with (one row per hash follows under scope)\n"));
        CHECK(has(dr, "MODS: doctor: bundle mods\\twin-a (alex.twin@1.0.0+1): another bundle of its key, twin-b, serves 1 of its "
                      "path(s) with no map open; a map saved with this bundle still serves this one\n"));
        /* Remove one: the author's own folder goes to the Recycle Bin too; the product keeps no bin. */
        doc = run_doc(SH_MODS_ACT_REMOVE, "twin-b");
        CHECK(has(doc, "\"action\":\"remove\",\"ok\":true") && has(doc, "Removed Twin. Its folder was moved to the Recycle Bin.") &&
              has(doc, "\"inUse\":false,\"removed\":[\"twin-b\"]"));
        free(doc);
        _snprintf_s(moved, sizeof moved, _TRUNCATE, "%s\\store\\removed-mods", tmp);
        CHECK(!mt_exists(moved) && !mt_exists(b));
        doc = list_doc();
        CHECK(bundle_has(doc, "twin-a", "\"state\":\"installed\",\"cause\":\"\",\"otherBuilds\":0,\"duplicateId\":false"));
        CHECK(!has(doc, "\"key\":\"twin-b\"") && !has(doc, "\"removed\":{"));
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
        /* The shell refuses: the folder is deleted outright and the result says so. */
        sh_ms_recycle_refuse_for_test(1);
        doc = run_doc(SH_MODS_ACT_REMOVE, "old.pack");
        sh_ms_recycle_refuse_for_test(0);
        CHECK(has(doc, "\"ok\":true") && has(doc, "Its folder was deleted because the Recycle Bin could not take it "
                                                  "(the shell refused it (test)).\""));
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
        CHECK(has(dr, "MODS: doctor: declared shadow generated/decls/entitydef/monster/imp.decl: by alex.shade-a "
                      "(migrated); served by alex.shade-a\n"));
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 shadow(s), 1 declared shadow(s), 0 edit source(s) of 0 game file(s) (never served, so neither a shadow nor a conflict)\n"));
        put_shadow_mod(b, "alex.shade-b", "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"2\";\r\n}");
        doc = run_doc(SH_MODS_ACT_INSTALL, b);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        /* Two mods' shadows with different bytes: with no map the static
         * winner serves (no rule separates them, so the first key; the install
         * order never counts), and the doctor says why. */
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: conflict generated/decls/entitydef/monster/imp.decl: alex.shade-a serves it "
                      "(no rule separates them; the first key in name order (a map's rows decide while it is open)); "
                      "from alex.shade-a, alex.shade-b\n"));
        CHECK(has(dr, "MODS: doctor: 1 conflict(s), 0 shadow(s), 0 declared shadow(s), 0 edit source(s) of 0 game file(s) (never served, so neither a shadow nor a conflict)\n"));
        /* Every section populated for the key comparison with the page's fixture. */
        load_map_naming("alex.r2-imp", hash2);
        g_placed[0] = "mods/alex.r2-imp/imp_copy";
        g_placed_count = 1;
        doc = list_doc();
        CHECK(has(doc, "\"rows\":[{\"id\":\"alex.r2-imp\"") && has(doc, "\"placed\":[{\"id\":\"alex.r2-imp\""));
        CHECK(bundle_has(doc, "alex.shade-a", "\"state\":\"installed\""));   /* a conflict is the doctor's, not the tab's */
        /* Derived facts: a grandfathered replacement is no edited copy. */
        CHECK(has(doc, "\"replacements\":[{\"path\":\"generated/decls/entitydef/monster/imp.decl\",\"edits\":false}],"
                       "\"edits\":[],\"registries\":{}"));
        CHECK(has(doc, "\"facts\":{\"adds\":"));
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
        CHECK(bundle_has(doc, "by-hand", "\"id\":\"\",\"name\":\"By hand\""));   /* its mod.json names it before any build */
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
        CHECK(bundle_has(doc, "alex.shade-b", "is missing its required name"));   /* the id is optional now */
        CHECK(bundle_has(doc, "alex.shade-a", "\"state\":\"installed\""));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: bundle mods\\alex.shade-b (alex.shade-b): skipped; "));
        CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 shadow(s), 1 declared shadow(s), 0 edit source(s) of 0 game file(s) (never served, so neither a shadow nor a conflict)\n"));
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

    printf("-- remove a folder the product made: to the Recycle Bin\n");
    {
        unsigned rescans = (unsigned)g_rescans;
        sh_modmanifest_reset();
        g_placed_count = 0;
        doc = run_doc(SH_MODS_ACT_REMOVE, "alex.shade-a");
        CHECK(has(doc, "\"action\":\"remove\",\"ok\":true") && has(doc, "\"message\":\"Removed Shade. Its folder was moved to the Recycle Bin.\"") &&
              has(doc, "\"removed\":[\"alex.shade-a\"]") && has(doc, "\"blobs\":0"));
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
        /* Editor tweaks (a stock row changed in this editor) are not Toybox rows the mod adds;
         * a row under its key is. */
        {
            char tw[MAX_PATH * 2];
            _snprintf_s(tw, sizeof tw, _TRUNCATE, "%s\\authoring\\alex.tweak", tmp);
            CreateDirectoryA(tw, NULL);
            mt_write(tw, "mod.json", "{\"id\":\"alex.tweak\",\"version\":\"1.0.0\",\"name\":\"Tweaks\",\"author\":\"tester\"}");
            mt_write(tw, "assets\\generated\\decls\\snapeditorentitydef\\demons\\demon_base.decl", "{\r\n\t// tweaked\r\n}\r\n");
            mt_write(tw, "assets\\generated\\decls\\snapeditorentitydef\\mods\\alex.tweak\\row.decl", "{\r\n}\r\n");
            doc = run_doc(SH_MODS_ACT_INSTALL, tw);
            CHECK(has(doc, "\"ok\":true") && has(doc, "\"key\":\"alex.tweak\"") && has(doc, "\"editorRows\":1"));
            free(doc);
            doc = list_doc();
            {
                const char *p = strstr(doc, "\"key\":\"alex.tweak\""), *e = p ? strstr(p + 1, "\"key\":") : NULL;
                const char *rows = p ? strstr(p, "\"editorRows\":1,") : NULL, *names = p ? strstr(p, "\"editorRowNames\":[\"mods/alex.tweak/row\"]") : NULL;
                const char *tweak = p ? strstr(p, "\"editor\":1,") : NULL;
                CHECK(p && rows && names && tweak && (!e || (rows < e && names < e && tweak < e)));
                {   /* An editor tweak is not a replaced stock file: the tab would say "Replaces 1 stock file". */
                    const char *c0 = p ? strstr(p, "\"counts\":{\"shadows\":0,") : NULL;
                    CHECK(c0 && (!e || c0 < e));
                }
            }
            free(doc);
            doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.tweak");
            free(doc);
        }
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
                                          "\"migratedFrom\":\"\",\"downloadedFor\":\"\",\"servedBy\":\"\",\"builtAt\":"));
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
        CHECK(has(doc, "\"vanilla\":false,\"registryClashes\":[]"));
        union_add(doc);
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
            CHECK(saved && strstr(saved, "\"snapmap-plus\":{\"format\":1,\"mods\":[{") && !strstr(saved, "smpmod1."));
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
        CHECK(has(doc, "\"tagged\":false,\"mods\":[],\"vanilla\":true,\"registryClashes\":[]"));
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

        printf("-- uninstall: the dry run (map part, Toybox rows, the folder deleted, saved maps, undo), then in order\n");
        g_tagged = 1;
        /* Two saved maps: one carries a row of alex.r2-imp, the other none. */
        {
            sh_modmanifest_list rows;
            sh_modmanifest_row row;
            char err[256], *text, other[MAX_PATH * 2];
            size_t tl = 0;
            memset(&rows, 0, sizeof rows);
            memset(&row, 0, sizeof row);
            strcpy_s(row.id, sizeof row.id, "alex.r2-imp");
            strcpy_s(row.version, sizeof row.version, "1.0.0");
            strcpy_s(row.bundle, sizeof row.bundle, hash1);
            CHECK(sh_modmanifest_list_add(&rows, &row));
            text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
            CHECK(text != NULL);
            if (text) { put_saved_decl(path, text, tl); HeapFree(GetProcessHeap(), 0, text); }
            sh_modmanifest_list_free(&rows);
            _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\76561198000000000\\SNAPMAPS00000000000000000001", saves);
            mt_write(other, "game.details", "checksum=1\r\n");
            put_saved_decl(other, MAP, sizeof MAP - 1);
        }
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "alex.req");
        CHECK(has(doc, "\"preview\":{\"action\":\"uninstall\",\"key\":\"alex.req\",\"name\":\"Gates\",\"map\":null,"
                       "\"install\":{\"folder\":\"alex.req\",\"disposition\":\"recycle\",\"savedMaps\":0,\"undoCleared\":true},"
                       "\"editorRows\":[]}"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, key2);
        CHECK(has(doc, "\"action\":\"uninstall\",\"key\":\"alex.r2-imp@1.0.1\""));
        CHECK(has(doc, "\"entityCount\":2,") && has(doc, "\"tagRemoved\":true,\"vanillaAfter\":true"));
        CHECK(has(doc, "\"install\":{\"folder\":\"alex.r2-imp@1.0.1\",\"disposition\":\"recycle\",\"savedMaps\":1,"
                       "\"undoCleared\":true},"
                       "\"editorRows\":[\"mods/alex.r2-imp/imp_copy\",\"mods/alex.r2-imp/imp_copy_b\"]}"));
        free(doc);
        CHECK(g_undo_clears == 0);
        /* A build that cannot clear the undo history does not uninstall while a map is open. */
        g_undo_available = 0;
        doc = run_doc(SH_MODS_ACT_UNINSTALL, key2);
        CHECK(has(doc, "\"ok\":false,\"message\":\"R2 Imp copy was not uninstalled: the editor's undo history cannot be "
                       "cleared on this build. Close the map, then uninstall it.\""));
        free(doc);
        g_undo_available = 1;
        /* The editor refuses the delete (busy): nothing is deleted, nothing cleared. */
        g_delete_status = SH_EDITOR_DELETE_BUSY;
        g_delete_calls = 0;
        doc = run_doc(SH_MODS_ACT_UNINSTALL, key2);
        CHECK(has(doc, "\"ok\":false,\"message\":\"R2 Imp copy was not uninstalled: the editor is busy"));
        free(doc);
        g_delete_status = SH_EDITOR_DELETE_DISPATCHED;
        {
            char kept[MAX_PATH * 2];
            _snprintf_s(kept, sizeof kept, _TRUNCATE, "%s\\%s", g_root, key2);
            CHECK(g_delete_calls == 1 && mt_exists(kept) && ent_alive(1) && ent_alive(2) && g_undo_clears == 0);
        }
        CHECK(ent_alive(1) && ent_alive(2));
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "nobody");
        CHECK(has(doc, "\"ok\":false") && has(doc, "nobody is not installed."));
        free(doc);

        printf("-- the console verbs share the dry runs and actions; --yes performs them\n");
        {
            const char *map_argv[] = { "map" };
            const char *rm_argv[] = { "map", "remove", g_k2 };
            const char *un_argv[] = { "uninstall", g_k2 };
            const char *un_yes[] = { "uninstall", g_k2, "--yes" };
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
            CHECK(has(g_doctor, "MODS: uninstall: moves the folder mods\\alex.r2-imp@1.0.1 to the Windows Recycle Bin; restore it "
                                "from there by hand\n"));
            CHECK(has(g_doctor, "MODS: uninstall: 1 of your saved maps use it; they will not open until a mod folder provides "
                                "its assets again\n"));
            CHECK(has(g_doctor, "MODS: uninstall: Undo history is cleared.\n"));
            CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp@1.0.1: dry run; add --yes to do it\n"));
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, key2);
            CHECK(mt_exists(path) && ent_alive(1));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, bad, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods map"));
            CHECK(sh_modui_command(1, other, doctor_line, NULL) == 0);
            g_main = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == -1);
            g_main = 1;
            /* --yes: the map cleaned first, the undo history owed until the
             * delete commits, then the folder deleted, then the rescan. */
            g_doctor[0] = 0;
            g_deleted_n = 0;
            g_undo_pending = 1;
            {
                unsigned rescans = (unsigned)g_rescans;
                CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1);
                CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp@1.0.1: Uninstalled R2 Imp copy and removed it from the open map. "
                                    "Its folder was moved to the Recycle Bin and the undo history was cleared.\n"));
                CHECK((unsigned)g_rescans == rescans + 1);
            }
            CHECK(g_deleted_n == 2 && !ent_alive(1) && !ent_alive(2) && g_tagged == 0);
            CHECK(!mt_exists(path));
            doc = list_doc();
            CHECK(!has(doc, "\"key\":\"alex.r2-imp@1.0.1\"") && has(doc, "\"mods\":[],\"vanilla\":true,"));
            free(doc);
        }

        printf("-- uninstall is permanent: no bin, no record; the tick clears the undo history once the delete committed\n");
        {
            char bin[MAX_PATH * 2], *rec = NULL;
            _snprintf_s(bin, sizeof bin, _TRUNCATE, "%s\\store\\removed-mods", tmp);
            CHECK(!mt_exists(bin));
            CHECK(sh_mb_record_get(g_root, "removed", &rec) != 1 || !rec);
            free(rec);
            CHECK(g_undo_clears == 0);                                /* the delete has not committed yet */
            CHECK(sh_modui_poll_due() == 1 && sh_modui_poll_main(NULL) == 1 && g_undo_clears == 0);
            g_busy = "Play is running";                               /* the tick waits out Play */
            g_undo_pending = 0;
            CHECK(sh_modui_poll_main(NULL) == 0 && g_undo_clears == 0);
            g_busy = NULL;
            CHECK(sh_modui_poll_due() == 1 && sh_modui_poll_main(NULL) == 1 && g_undo_clears == 1);
            CHECK(sh_modui_poll_main(NULL) == 1 && g_undo_clears == 1);   /* once */
            /* Ctrl+Z can bring back nothing older than the delete; even entities
             * that reappear bring back no folder. */
            g_ents[1].alive = g_ents[2].alive = 1;
            g_placed[0] = "mods/alex.r2-imp/imp_copy";
            g_placed[1] = "player/coop/player_start";
            g_placed_count = 2;
            CHECK(sh_modui_poll_main(NULL) == 1 && !mt_exists(path) && g_toasts == 0);
            /* A save that still places its names writes no row for it, and brings nothing back. */
            {
                size_t len = 0, out_len = 0;
                char *t = map_text(&len), *saved = sh_modmanifest_on_save(t, len, &out_len);
                CHECK(!mt_exists(path) && (!saved || !strstr(saved, hash2)));
                if (saved) HeapFree(GetProcessHeap(), 0, saved);
            }
            /* A map load that names it: the row is missing, with the refusal's sentence. */
            load_map_naming("alex.r2-imp", hash2);
            CHECK(!mt_exists(path));
            doc = list_doc();
            CHECK(has(doc, "\"state\":\"missing\",\"cause\":\"This map uses alex.r2-imp, which is not installed.\""));
            free(doc);
            sh_modmanifest_reset();
        }

        printf("-- the retired bin: no console verb, no action\n");
        {
            const char *ls[] = { "removed" };
            CHECK(sh_modui_command(1, ls, doctor_line, NULL) == 0);   /* not a verb of the tab any more */
            doc = run_doc(SH_MODS_ACT_REMOVED_EMPTY, "");
            CHECK(has(doc, "\"action\":\"unknown\",\"ok\":false,\"message\":\"Uninstalled mods go to the Windows Recycle Bin.\""));
            free(doc);
            doc = run_doc(SH_MODS_ACT_REMOVED_RESTORE, "alex.r2-imp");
            CHECK(has(doc, "\"ok\":false"));
            free(doc);
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
            g_scope_activates = 0;
            g_marks = 0;
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"action\":\"mapAddMod\",\"ok\":true,\"message\":\"Gates is now used by the whole of Imp Arena."));
            free(doc);
            CHECK(g_marks == 1 && !strcmp(g_mark_label, "Add Mod To Map"));
            /* Its requirements were out of scope with no map listing it; now they apply at once, through the
             * activation a map load runs (its decls registered, its registries served). */
            CHECK(g_scope_applies == applies + 1 && g_scope_activates == 1 && !strcmp(g_scope_phase, "map add"));
            CHECK(sh_modmanifest_current_attached(&att) == 1 && sh_modserve_bundle_state(att.rows[0].bundle, NULL, 0) == 1 &&
                  !strcmp(att.rows[0].id, "alex.req"));
            sh_modmanifest_list_free(&att);
            doc = list_doc();
            CHECK(has(doc, "\"key\":\"alex.req\",\"id\":\"alex.req\"") && has(doc, "\"attached\":true,\"nothingPlaced\":true,\"bundle\":\"") &&
                  has(doc, "\"scope\":\"map\",\"active\":true"));
            CHECK(has(doc, "\"otherKey\":\"\",\"nameCount\":0,\"names\":[],\"attached\":true}"));
            CHECK(has(doc, "\"scope\":\"map\",\"scopeAuto\":false,\"scoped\":true"));
            CHECK(has(doc, "\"vanilla\":false"));
            free(doc);
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"ok\":true,\"message\":\"Gates is already used by the whole of Imp Arena.\""));
            free(doc);
            CHECK(g_marks == 1);   /* already attached: not marked again */
            /* A folder of editor tweaks only (an author's editor overrides): nothing in it
             * plays, every map opened here has them, so a map never lists it (DESIGN-additive 2.6). */
            {
                char tw[MAX_PATH * 2];
                _snprintf_s(tw, sizeof tw, _TRUNCATE, "%s\\authoring\\alex.tweakonly", tmp);
                CreateDirectoryA(tw, NULL);
                mt_write(tw, "mod.json", "{\"id\":\"alex.tweakonly\",\"version\":\"1.0.0\",\"name\":\"Only tweaks\",\"author\":\"tester\"}");
                mt_write(tw, "assets\\generated\\decls\\snapeditorentitydef\\demons\\demon_base.decl", "{\r\n\t// tweaked\r\n}\r\n");
                doc = run_doc(SH_MODS_ACT_INSTALL, tw);
                CHECK(has(doc, "\"ok\":true") && has(doc, "\"key\":\"alex.tweakonly\""));
                free(doc);
                doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.tweakonly");
                CHECK(has(doc, "\"action\":\"mapAddMod\",\"ok\":false,\"message\":\"Only tweaks changes only this computer's "
                               "editor (1 editor tweak); every map you open here already has them, and a map never lists them.\""));
                free(doc);
                CHECK(g_marks == 1);   /* refused: not marked */
                /* The list says so (facts.editorOnly, by the same rule), so the tab offers no
                 * map action and no "every player needs" line; a mod with a new decl is not one. */
                doc = list_doc();
                CHECK(row_has(doc, "alex.tweakonly", "\"editor\":1,\"replaces\":0,\"editorOnly\":true,"));
                CHECK(row_has(doc, "alex.req", "\"editorOnly\":false,"));
                free(doc);
                doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.tweakonly");
                free(doc);
            }
            /* A rawmap opened in its place (sh_rawmaps load) is a new map until its first save:
             * the borrowed saved map's name and id are not the open map's. */
            g_opened_from = "v2b-empty.rawmap.json";
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: map: not saved yet (opened from the rawmap v2b-empty.rawmap.json; the first save names "
                                "it), 5 entities\n"));
            CHECK(!has(g_doctor, "Imp Arena") && !has(g_doctor, "ABCDEF0123456789ABCD"));
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"message\":\"Gates is already used by the whole of the open map.\""));
            free(doc);
            g_opened_from = NULL;
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1 && has(g_doctor, "MODS: map: Imp Arena (id ABCDEF0123456789ABCD)"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(3, add_argv, doctor_line, NULL) == 1 &&
                  has(g_doctor, "MODS: map add alex.req: Gates is already used by the whole of Imp Arena.\n"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, bad_add, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods map add"));
            g_doctor[0] = 0;
            CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "(alex.req, installed): map-level (attached to the whole map), 0 placed entities, 0 wire or "
                                "timeline reference(s); scope map, active\n"));
            CHECK(has(g_doctor, "MODS: map: scope: alex.req@1.0.0 (") &&
                  has(g_doctor, "0 shadows, 0 surfaces, requirements): declared by the open map (row 1): serves everything it "
                                "carries\n"));
            /* The save writes its row as a map-level row; nothing places it. */
            t = map_text(&len);
            saved = sh_modmanifest_on_save(t, len, &out_len);
            CHECK(saved && strstr(saved, "\"snapmap-plus\":{\"format\":1,\"mods\":[{") && strstr(saved, "alex.req") &&
                  strstr(saved, "\"attached\":true") && !strstr(saved, "smpmod1."));
            if (saved) HeapFree(GetProcessHeap(), 0, saved);
            /* The dry run and the removal: nothing to delete, the row goes. */
            doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, "alex.req");
            CHECK(has(doc, "\"entityCount\":0") && has(doc, "\"manifestRow\":true") && has(doc, "\"published\":true,\"attached\":true}"));
            free(doc);
            applies = g_scope_applies;
            g_delete_calls = 0;
            g_marks = 0;
            doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "alex.req");
            CHECK(has(doc, "\"ok\":true,\"message\":\"Removed Gates from Imp Arena: no entity used it, so only its row was dropped"));
            free(doc);
            CHECK(g_marks == 1 && !strcmp(g_mark_label, "Remove Mod From Map"));   /* a row alone: marked */
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
            /* The editor map's rows go with its listing, and a hub launch
             * declares exactly the launched map's rows (ADD-L6 H). */
            {
                sh_modmanifest_list cur;
                char got[8][SH_MODSERVE_HASH_HEX + 1];
                sh_modserve_bundle_ref ref;
                const char *placed0 = g_placed[0];
                int placed_n = g_placed_count;
                size_t ll = 0;
                char *l;
                CHECK(sh_modmanifest_current(&cur) == 0);
                sh_modmanifest_list_free(&cur);
                /* A Play entry at the hub reads nothing from the editor, whose
                 * entities still name a mod. */
                g_placed[0] = "mods/alex.req/twin";
                g_placed_count = 1;
                (void)sh_modserve_scope_source(SH_MODSERVE_AT_PLAY);
                CHECK(sh_modserve_scope_hashes(got, 8) == 0);
                /* A My Maps play of a map that declares nothing: nothing is declared in its Play. */
                l = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &ll);
                if (l) HeapFree(GetProcessHeap(), 0, l);
                g_preloads = 0;
                (void)sh_modui_scope_launch("My Maps play");
                /* The gate's pre-load pass ran once, first, before the launch
                 * set its state (ADD-L11). (The Play entry above left the hub
                 * in PLAY; the pass itself returns its rows to the editor's
                 * mode, sh_modserve_launch_preload.) */
                CHECK(g_preloads == 1 && !strcmp(g_preload_state, "none") && !strcmp(g_preload_why, "My Maps play"));
                CHECK(sh_modserve_scope_hashes(got, 8) == 0 && sh_modserve_scope_map_open());
                CHECK(sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY);
                (void)sh_modserve_scope_source(SH_MODSERVE_AT_PLAY);
                CHECK(sh_modserve_scope_hashes(got, 8) == 0);
                CHECK(sh_modserve_bundle_serving(NULL, "alex.req", &ref));
                {
                    sh_modserve_scope_info si;
                    CHECK(sh_modserve_bundle_scope(ref.hash, &si) && !si.listed && si.play);
                }
                /* One that declares alex.req: exactly its row, whatever the editor shows. */
                load_map_naming("alex.req", ref.hash);
                /* The launch before reached its Play entry but never ran: this one replaces it. */
                CHECK(!strcmp(sh_modui_test_launch_state(), "playing"));
                (void)sh_modui_scope_launch("My Maps play");
                CHECK(sh_modserve_scope_hashes(got, 8) == 1 && !_stricmp(got[0], ref.hash));
                CHECK(!strcmp(sh_modui_test_launch_state(), "launched"));
                /* A new launch: its own pre-load pass. */
                CHECK(g_preloads == 2 && !strcmp(g_preload_state, "playing"));
                /* A second boundary of the same launch (launched) does not run it again. */
                (void)sh_modui_scope_launch("published launch");
                CHECK(g_preloads == 2 && !strcmp(sh_modui_test_launch_state(), "launched"));
                /* The launch's rows hold until its Play has run and ended (ADD-V4a H2): an idle poll
                 * between the gate and the level load is not the end of the Play. */
                applies = g_scope_applies;
                CHECK(sh_modui_poll_main(NULL) == 0);
                CHECK(sh_modserve_scope_hashes(got, 8) == 1 && sh_modserve_scope_map_open() && g_scope_applies == applies);
                CHECK(!strcmp(sh_modui_test_launch_state(), "launched"));
                /* The level load. */
                g_busy = "a map is loading (state 3); try again when it has loaded";
                CHECK(sh_modui_poll_main(NULL) == 0 && !strcmp(sh_modui_test_launch_state(), "loading"));
                g_busy = NULL;
                CHECK(sh_modui_poll_main(NULL) == 0 && sh_modserve_scope_hashes(got, 8) == 1);
                /* The Play-begin gate (SnapBuild): the launched Play is being built. */
                (void)sh_modserve_scope_source(SH_MODSERVE_AT_PLAY);
                CHECK(sh_modserve_scope_hashes(got, 8) == 1 && !strcmp(sh_modui_test_launch_state(), "playing"));
                CHECK(sh_modui_poll_main(NULL) == 0 && sh_modserve_scope_hashes(got, 8) == 1);
                /* Play runs. */
                g_busy = "Play is running; return to the editor first";
                CHECK(sh_modui_poll_main(NULL) == 0 && !strcmp(sh_modui_test_launch_state(), "running"));
                CHECK(sh_modserve_scope_hashes(got, 8) == 1 && g_scope_applies == applies);
                g_busy = NULL;
                /* The leave: its Play over, back at the hub: nothing is declared, in the editor mode. */
                log_clear();
                CHECK(sh_modui_poll_main(NULL) == 0);
                CHECK(sh_modserve_scope_hashes(got, 8) == 0 && !sh_modserve_scope_map_open());
                CHECK(!strcmp(g_scope_phase, "hub") && sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_EDITOR);
                CHECK(has(g_logbuf, "scope: the launched map's Play is over; nothing is declared at the hub"));
                CHECK(!strcmp(sh_modui_test_launch_state(), "none"));
                CHECK(sh_modmanifest_current(&cur) == 0);
                sh_modmanifest_list_free(&cur);
                CHECK(sh_modui_poll_main(NULL) == 0 && g_scope_applies == applies + 1);   /* once */
                /* A launch the gate refuses (ADD-V4a H3): its parse made the map's rows current and
                 * the load's hook listed them; nothing was launched, so nothing stays declared at
                 * the browser. */
                load_map_naming("alex.req", ref.hash);
                CHECK(sh_modserve_scope_hashes(got, 8) == 1 && sh_modmanifest_current(&cur) == 1);
                sh_modmanifest_list_free(&cur);
                applies = g_scope_applies;
                log_clear();
                (void)sh_modui_scope_launch_refused("My Maps play");
                CHECK(sh_modserve_scope_hashes(got, 8) == 0 && !sh_modserve_scope_map_open());
                CHECK(sh_modmanifest_current(&cur) == 0);
                sh_modmanifest_list_free(&cur);
                CHECK(g_scope_applies == applies + 1 && !strcmp(g_scope_phase, "launch refused"));
                CHECK(sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_EDITOR && !strcmp(sh_modui_test_launch_state(), "none"));
                CHECK(has(g_logbuf, "scope: My Maps play was refused; nothing was launched, so nothing is declared"));
                CHECK(sh_modui_poll_main(NULL) == 0 && sh_modserve_scope_hashes(got, 8) == 0 && g_scope_applies == applies + 1);
                /* A published launch held at its boundary and then dropped, after the gate's
                 * listing was set: the same. */
                load_map_naming("alex.req", ref.hash);
                (void)sh_modui_scope_launch("published launch");
                (void)sh_modui_scope_launch_refused("published launch");
                CHECK(sh_modserve_scope_hashes(got, 8) == 0 && sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_EDITOR);
                CHECK(!strcmp(sh_modui_test_launch_state(), "none"));
                /* Refused with the editor still showing its map: that map's rows come back. */
                g_map_on = 1;
                load_map_naming("alex.req", ref.hash);
                CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open());
                l = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &ll);   /* the launched map: no rows */
                if (l) HeapFree(GetProcessHeap(), 0, l);
                CHECK(sh_modmanifest_current(&cur) == 0);
                sh_modmanifest_list_free(&cur);
                log_clear();
                (void)sh_modui_scope_launch_refused("My Maps play");
                CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].id, "alex.req"));
                sh_modmanifest_list_free(&cur);
                CHECK(sh_modserve_scope_hashes(got, 8) >= 1 && sh_modserve_scope_map_open());
                {
                    int q, found = 0, nq = sh_modserve_scope_hashes(got, 8);
                    for (q = 0; q < nq && q < 8; q++) found += !_stricmp(got[q], ref.hash);
                    CHECK(found == 1);
                }
                CHECK(has(g_logbuf, "scope: My Maps play was refused; nothing was launched, so the editor map's rows are "
                                    "declared again"));
                CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open());
                /* Back to the hub for what follows. */
                g_map_on = 0;
                g_map_left = 1;
                CHECK(sh_modui_poll_main(NULL) == 0 && !sh_modserve_scope_map_open());
                g_placed[0] = placed0;
                g_placed_count = placed_n;
            }
            {
            int placed = g_placed_count;
            g_placed_count = -1;
            doc = run_doc(SH_MODS_ACT_MAP_ADD, "alex.req");
            CHECK(has(doc, "\"ok\":false,\"message\":\"No map is open in the editor.\""));
            free(doc);
            g_placed_count = placed;
            }
            /* Back in the editor, which opens the map again: the listing is the open map's. */
            {
                sh_modserve_bundle_ref req;
                CHECK(sh_modserve_bundle_serving(NULL, "alex.req", &req));
                load_map_naming("alex.req", req.hash);
            }
            g_map_on = 1;
            CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open() && !strcmp(g_scope_phase, "map load"));
            /* Remove from map of a placed mod: the entities' delete commits after the call, so the
             * placed names still name them while the listing is computed; the mod is withdrawn anyway. */
            {
                char before[8][SH_MODSERVE_HASH_HEX + 1], after[8][SH_MODSERVE_HASH_HEX + 1];
                int nb, na, i, req_before = 0, req_after = 0;
                sh_modserve_bundle_ref ref;
                const char *placed0 = g_placed[0];
                int placed_n = g_placed_count;
                open_test_map();
                g_ents[1].inherit = "mods/alex.req/twin";
                g_ents[2].inherit = "mods/alex.req/twin";
                g_placed[0] = "mods/alex.req/twin";
                g_placed_count = 1;
                CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
                CHECK(sh_modui_poll_main(NULL) == 1 && sh_modserve_scope_map_open());
                nb = sh_modserve_scope_hashes(before, 8);
                for (i = 0; i < nb && i < 8; i++)
                    req_before += sh_modserve_bundle_serving(before[i], NULL, &ref) && !strcmp(ref.id, "alex.req");
                g_delete_calls = 0;
                doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "alex.req");
                if (!has(doc, "\"ok\":true")) printf("    %s\n", doc);
                CHECK(has(doc, "\"action\":\"mapRemoveMod\",\"ok\":true"));
                free(doc);
                CHECK(g_delete_calls == 1 && !strcmp(g_placed[0], "mods/alex.req/twin"));   /* not committed yet */
                na = sh_modserve_scope_hashes(after, 8);
                for (i = 0; i < na && i < 8; i++)
                    req_after += sh_modserve_bundle_serving(after[i], NULL, &ref) && !strcmp(ref.id, "alex.req");
                CHECK(req_before == 1 && req_after == 0 && na == nb - 1);
                g_placed[0] = placed0;
                g_placed_count = placed_n;
            }
            /* A row on a build that is gone, which the editor serves through the installed
             * build of its key (the SUBSTITUTE verdict): Remove from map drops it by key,
             * placed or map-level, and says both builds. */
            {
                const char *placed0 = g_placed[0];
                int placed_n = g_placed_count, pass;
                sh_modserve_bundle_ref ref;
                char want[400], sub[80];
                CHECK(sh_modserve_bundle_serving(NULL, "alex.req", &ref));
                strcpy_s(g_subst_to, sizeof g_subst_to, ref.hash);
                for (pass = 0; pass < 2; pass++) {
                    sh_modmanifest_list cur;
                    const char *rm_argv[] = { "map", "remove", "alex.req", "--yes" };
                    open_test_map();
                    if (!pass) {
                        g_ents[1].inherit = "mods/alex.req/twin";
                        g_ents[2].inherit = "mods/alex.req/twin";
                        g_placed[0] = "mods/alex.req/twin";
                        g_placed_count = 1;
                    } else {
                        g_placed[0] = placed0;
                        g_placed_count = placed_n;
                    }
                    CHECK(load_gone_row("alex.req", GONE_REQ, pass));
                    CHECK(sh_modmanifest_substituted(GONE_REQ, sub, sizeof sub) && !strcmp(sub, ref.hash));
                    /* sh_mods map and the doctor say why: the row's build, and the build that serves it. */
                    _snprintf_s(want, sizeof want, _TRUNCATE, "alex.req: served in the editor by mods\\alex.req; the next "
                                "save records it, and Remove from map drops the row by its key");
                    g_doctor[0] = 0;
                    CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
                    CHECK(has(g_doctor, "MODS: map:   ") && has(g_doctor, want));
                    CHECK(has(g_doctor, pass ? "(alex.req, otherBuild)" : "(alex.req, installed)"));
                    g_doctor[0] = 0;
                    CHECK(sh_modui_doctor(0, doctor_line, NULL) == 1);
                    CHECK(has(g_doctor, "MODS: doctor: map row ") && has(g_doctor, want));
                    log_clear();
                    g_doctor[0] = 0;
                    CHECK(sh_modui_command(4, rm_argv, doctor_line, NULL) == 1);
                    _snprintf_s(want, sizeof want, _TRUNCATE, "1 manifest row(s) dropped (alex.req declared at build %.16s, "
                                "served by build %.16s)", GONE_REQ, ref.hash);
                    if (!has(g_logbuf, want)) printf("    %s%s\n", g_logbuf, g_doctor);
                    CHECK(has(g_logbuf, want));
                    CHECK(sh_modmanifest_current(&cur) == 0);
                    sh_modmanifest_list_free(&cur);
                    CHECK(!sh_modmanifest_substituted(GONE_REQ, sub, sizeof sub));
                    CHECK(g_delete_calls >= 0);
                }
                g_placed[0] = placed0;
                g_placed_count = placed_n;
                g_subst_to[0] = 0;
            }
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
            {
                sh_modserve_bundle_ref before, after;
                sh_modmanifest_list cur;
                CHECK(sh_modserve_bundle_serving(NULL, "alex.req", &before));
                mt_append(path, "assets\\generated\\decls\\entitydef\\mods\\alex.req\\twin.decl", "// changed\r\n");
                doc = run_doc(SH_MODS_ACT_REBUILD, "alex.req");
                CHECK(has(doc, "\"ok\":true,\"message\":\"Rebuilt alex.req: 1 files.\""));
                free(doc);
                /* The author's own folder, rebuilt in place: a map saved against
                 * the earlier build follows it when it opens in the editor, with
                 * one log line, no prompt and nothing marked modified; the Mods
                 * tab offers nothing. */
                CHECK(sh_modserve_bundle_serving(NULL, "alex.req", &after) && strcmp(before.hash, after.hash));
                CHECK(sh_modserve_row_follow(before.hash, NULL) == 1);
                log_clear();
                g_marks = 0;
                {
                    const char *ids[1], *hashes[1];
                    ids[0] = "alex.req";
                    hashes[0] = before.hash;
                    CHECK(load_rows(ids, hashes, 1, 1));   /* an editor load */
                }
                CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].bundle, after.hash));
                sh_modmanifest_list_free(&cur);
                CHECK(has(g_logbuf, "MODS: map: alex.req: follows its own folder, rebuilt since the map was saved"));
                CHECK(g_marks == 0);
                doc = list_doc();
                CHECK(doc && !has(doc, "\"upgrade\""));
                free(doc);
                sh_modmanifest_reset();
            }
            CHECK((unsigned)g_rescans == rescans + 2);
            /* A refused Rebuild is the folder's state, as a refused rescan build is:
             * the row names the cause until a build succeeds (DESIGN-additive 2.8). */
            {
                static const char failed[] =
                    "\"buildFailed\":{\"cause\":\"empty file: assets/generated/decls/entitydef/mods/alex.req/empty.decl\"";
                char why[SH_MB_CAUSE_MAX], bad[MAX_PATH];
                mt_put(path, "assets\\generated\\decls\\entitydef\\mods\\alex.req\\empty.decl", "", "wb");
                doc = run_doc(SH_MODS_ACT_REBUILD, "alex.req");
                CHECK(has(doc, "\"ok\":false,\"message\":\"Not rebuilt: empty file: "));
                free(doc);
                sh_ms_build_cause(path, why, sizeof why);
                CHECK(!strcmp(why, "empty file: assets/generated/decls/entitydef/mods/alex.req/empty.decl"));
                doc = list_doc();
                {
                    const char *row = doc ? strstr(doc, "{\"key\":\"alex.req\"") : NULL;
                    const char *next = row ? strstr(row + 1, "{\"key\":\"") : NULL;
                    const char *hit = row ? strstr(row, failed) : NULL;
                    CHECK(hit && (!next || hit < next));
                }
                free(doc);
                _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\alex.req\\empty.decl", path);
                CHECK(DeleteFileA(bad));
                doc = run_doc(SH_MODS_ACT_REBUILD, "alex.req");
                CHECK(has(doc, "\"ok\":true,\"message\":\"Rebuilt alex.req: nothing changed (1 files).\""));
                free(doc);
                sh_ms_build_cause(path, why, sizeof why);
                CHECK(!why[0]);
                doc = list_doc();
                CHECK(doc && !has(doc, "\"buildFailed\""));
                free(doc);
            }
            doc = run_doc(SH_MODS_ACT_REBUILD, "nobody");
            CHECK(has(doc, "\"ok\":false") && has(doc, "no longer installed"));
            free(doc);
            g_busy = "a map is loading";
            doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.req");
            CHECK(has(doc, "\"ok\":false,\"message\":\"Not now: a map is loading.\"") && mt_exists(path));
            free(doc);
            g_busy = NULL;
            /* The open map does not use it: the history is cleared at once, the folder recycled. */
            {
                int clears = g_undo_clears;
                doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.req");
                CHECK(has(doc, "\"action\":\"uninstall\",\"ok\":true,\"message\":\"Uninstalled Gates. Its folder was moved to the Recycle Bin and "
                               "the undo history was cleared.\"") && !mt_exists(path));
                free(doc);
                CHECK(g_undo_clears == clears + 1);
            }
        }

        printf("-- the document carries no bin\n");
        doc = list_doc();
        CHECK(!has(doc, "\"removed\":{"));
        free(doc);
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
        /* The folder watcher's rescan: deferred while the game is busy or a store write runs. */
        g_busy = "Play is running";
        doc = run_doc(SH_MODS_ACT_WATCH_RESCAN, "");
        CHECK(has(doc, "\"action\":\"watchRescan\",\"ok\":false,\"message\":\"Deferred: Play is running.\",\"deferred\":true"));
        free(doc);
        g_busy = NULL;
        modstore_lock();
        doc = run_doc(SH_MODS_ACT_WATCH_RESCAN, "");
        CHECK(has(doc, "\"deferred\":true") && has(doc, "a store write is in progress"));
        free(doc);
        modstore_unlock();
        doc = run_doc(SH_MODS_ACT_WATCH_RESCAN, "");
        CHECK(has(doc, "\"action\":\"watchRescan\",\"ok\":true,\"message\":\"Mods folder reread: ") && !has(doc, "\"deferred\":true"));
        free(doc);
        /* The store's state, for the watcher (any thread). */
        {
            unsigned gen0;
            call.op = SH_MODS_STORE;
            CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed == 0);
            gen0 = call.generation;
            modstore_lock();
            CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed == 1 && call.generation == gen0);
            modstore_lock();                              /* recursive */
            modstore_unlock();
            CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed == 1 && call.generation == gen0);
            modstore_unlock();
            CHECK(sh_modui_slot(NULL, &call) == 1 && call.needed == 0 && call.generation == gen0 + 1);
        }
        call.op = 99;
        CHECK(sh_modui_slot(NULL, &call) == 0);
        CHECK(sh_modui_slot(NULL, NULL) == 0);
    }

    printf("-- install through the tab: a folder of mods, and several picked folders\n");
    {
        char bulk[MAX_PATH], one[MAX_PATH], two[MAX_PATH], bad[MAX_PATH], arg[MAX_PATH * 2 + 2];
        _snprintf_s(bulk, sizeof bulk, _TRUNCATE, "%s\\authoring\\bulk", tmp);
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s\\group\\bulk.one", bulk);
        _snprintf_s(two, sizeof two, _TRUNCATE, "%s\\bulk.two", bulk);
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad", bulk);
        mt_write(one, "mod.json", "{\"id\":\"bulk.one\",\"version\":\"1.0.0\",\"name\":\"Bulk one\",\"author\":\"t\"}");
        mt_write(one, "assets\\generated\\image\\mods\\bulk.one\\a.bimage", "bytes of one");
        mt_write(two, "mod.json", "{\"id\":\"bulk.two\",\"version\":\"1.0.0\",\"name\":\"Bulk two\",\"author\":\"t\"}");
        mt_write(two, "assets\\generated\\image\\mods\\bulk.two\\a.bimage", "bytes of two");
        mt_write(bad, "mod.json", "{}");
        g_rescans = 0;
        doc = run_doc(SH_MODS_ACT_INSTALL, bulk);
        CHECK(has(doc, "\"action\":\"install\",\"ok\":true"));
        CHECK(has(doc, "\"installedCount\":2,\"refusedCount\":1"));
        CHECK(has(doc, "Installed 2 mods; 1 refused: bad ("));
        CHECK(has(doc, "\"key\":\"bulk."));
        CHECK(has(doc, "\"folder\":\"") && has(doc, "\\\\group\\\\bulk.one\",\"ok\":true,\"key\":\"bulk.one\""));
        CHECK(has(doc, "\\\\bulk.two\",\"ok\":true,\"key\":\"bulk.two\""));
        CHECK(has(doc, "\\\\bad\",\"ok\":false,\"key\":\"\""));
        CHECK(g_rescans == 1);
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "bulk.one", "\"state\":\"installed\"") && bundle_has(doc, "bulk.two", "\"state\":\"installed\""));
        free(doc);
        /* Several picked folders, newline-separated: each a mod or a folder of mods. */
        _snprintf_s(arg, sizeof arg, _TRUNCATE, "%s\n%s", one, two);
        g_rescans = 0;
        doc = run_doc(SH_MODS_ACT_INSTALL, arg);
        CHECK(has(doc, "\"action\":\"install\",\"ok\":true") && has(doc, "\"installedCount\":2,\"refusedCount\":0"));
        CHECK(has(doc, "\"message\":\"Installed 2 mods.\""));
        CHECK(g_rescans == 1);
        free(doc);
        /* A folder with nothing below it is refused with the reason. */
        _snprintf_s(arg, sizeof arg, _TRUNCATE, "%s\\empty", bulk);
        CreateDirectoryA(arg, NULL);
        doc = run_doc(SH_MODS_ACT_INSTALL, arg);
        CHECK(has(doc, "\"action\":\"install\",\"ok\":false") && has(doc, "no mod.json in it or in any folder below it"));
        free(doc);
        /* A single picked mod folder keeps the single-mod result. */
        doc = run_doc(SH_MODS_ACT_INSTALL, two);
        CHECK(has(doc, "\"action\":\"install\",\"ok\":true") && has(doc, "\"key\":\"bulk.two\"") && !has(doc, "installedCount"));
        free(doc);
    }

    printf("-- the record held by another program: install and uninstall say which file and what to do\n");
    {
        char held[MAX_PATH], rec[MAX_PATH];
        HANDLE t;
        _snprintf_s(held, sizeof held, _TRUNCATE, "%s\\authoring\\held.one", tmp);
        _snprintf_s(rec, sizeof rec, _TRUNCATE, "%s\\snapmap-plus.json", g_root);
        mt_write(held, "mod.json", "{\"id\":\"held.one\",\"version\":\"1.0.0\",\"name\":\"Held one\",\"author\":\"t\"}");
        mt_write(held, "assets\\generated\\image\\mods\\held.one\\a.bimage", "bytes of held");
        CHECK(mt_exists(rec));
        t = ui_hold_record(rec, 6000);
        doc = run_doc(SH_MODS_ACT_INSTALL, held);
        if (!has(doc, "close the program holding it")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(has(doc, "\"action\":\"install\""));
        CHECK(has(doc, "Some of its bookkeeping was not saved: ") && has(doc, "snapmap-plus.json could not be used (") &&
              has(doc, "close the program holding it (a backup, sync or antivirus tool, or an editor) and try again"));
        free(doc);
        WaitForSingleObject(t, 10000);
        CloseHandle(t);
        /* Released: the next action reads the record again and says nothing extra. */
        doc = run_doc(SH_MODS_ACT_INSTALL, held);
        CHECK(has(doc, "\"action\":\"install\"") && !has(doc, "bookkeeping was not saved"));
        free(doc);
        t = ui_hold_record(rec, 6000);
        doc = run_doc(SH_MODS_ACT_UNINSTALL, "held.one");
        if (!has(doc, "close the program holding it")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(has(doc, "snapmap-plus.json could not be used (") && has(doc, "close the program holding it"));
        free(doc);
        WaitForSingleObject(t, 10000);
        CloseHandle(t);
    }

    printf("-- an old descriptor's extends partial is refused with what to put instead\n");
    {
        char part[MAX_PATH];
        _snprintf_s(part, sizeof part, _TRUNCATE, "%s\\authoring\\alex.part", tmp);
        mt_write(part, "mod.json", "{\"id\":\"alex.part\",\"version\":\"1.0.0\",\"name\":\"Partial\",\"author\":\"t\","
                                   "\"extends\":{\"entitydef/ai/conductor/coop/snapmap_default\":\"mods/alex.part/conductor_entries\"}}");
        mt_write(part, "assets\\generated\\decls\\entitydef\\mods\\alex.part\\conductor_entries.decl",
                 "{\r\n\tedit = {\r\n\t\taiTypeList = {\r\n\t\t\tnum = 1;\r\n\t\t\titem[0] = {\r\n\t\t\t\taiType = \"1048576\";\r\n"
                 "\t\t\t\tentityDef = \"ai/demon/imp_coop\";\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_INSTALL, part);
        if (!has(doc, "this is a partial")) printf("    %s\n", doc);
        CHECK(has(doc, "\"ok\":false") && has(doc, "this is a partial; put the whole edited copy of "
                                                  "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl at its game name"));
        free(doc);
    }

    printf("-- an edited copy of a game file: the tab's facts list the file and its operations\n");
    {
        char ed[MAX_PATH];
        sh_mb_set_stock_ops(&g_imp_ops);
        _snprintf_s(ed, sizeof ed, _TRUNCATE, "%s\\authoring\\alex.edits", tmp);
        mt_write(ed, "mod.json", "{\"id\":\"alex.edits\",\"version\":\"1.0.0\",\"name\":\"Edits\",\"author\":\"t\"}");
        mt_write(ed, "assets\\generated\\decls\\entitydef\\monster\\imp.decl",
                 "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"3\";\r\n}");
        doc = run_doc(SH_MODS_ACT_INSTALL, ed);
        if (!has(doc, "\"ok\":true")) printf("    %s\n", doc);
        CHECK(has(doc, "\"ok\":true"));
        free(doc);
        doc = list_doc();
        CHECK(has(doc, "\"replacements\":[{\"path\":\"generated/decls/entitydef/monster/imp.decl\",\"edits\":true}],"
                                            "\"edits\":[{\"file\":\"generated/decls/entitydef/monster/imp.decl\",\"ops\":2,\"set\":2,"));
        union_add(doc);
        free(doc);

        printf("-- a clash between two mods' edits shows its choice when a row pins a build that is not installed\n");
        {
            char ed2[MAX_PATH], h_one[80] = "", h_two[80] = "";
            static const char GONE_TWO[] = "00000000000000000000000000000000000000000000000000000000000000cd";
            const char *ids[2] = { "alex.edits", "alex.edits2" }, *gone[2] = { GONE_REQ, GONE_TWO };
            int pass, was_on = g_map_on, was_placed = g_placed_count;
            doc = list_doc();
            {
                const char *at = doc ? strstr(doc, "\"key\":\"alex.edits\"") : NULL;
                CHECK(at && doc_bundle(at, h_one, sizeof h_one));
            }
            free(doc);
            _snprintf_s(ed2, sizeof ed2, _TRUNCATE, "%s\\authoring\\alex.edits2", tmp);
            mt_write(ed2, "mod.json", "{\"id\":\"alex.edits2\",\"version\":\"1.0.0\",\"name\":\"Edits Two\",\"author\":\"t\"}");
            mt_write(ed2, "assets\\generated\\decls\\entitydef\\monster\\imp.decl",
                     "{\r\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\r\n\tsize = \"5\";\r\n}");
            doc = run_doc(SH_MODS_ACT_INSTALL, ed2);
            if (!doc_bundle(doc, h_two, sizeof h_two)) printf("    %s\n", doc);
            CHECK(has(doc, "\"ok\":true") && doc_bundle(doc, h_two, sizeof h_two));
            free(doc);
            /* Two edited copies of one game file: edit sources, never a conflict, and never a
             * replaced stock file (counts.shadows, facts.replaces). */
            {
                const char *dr = doctor(0);
                CHECK(dr && strstr(dr, "MODS: doctor: edit source generated/decls/entitydef/monster/imp.decl: from alex.edits2, "
                                       "alex.edits; diffed at build against the game's text, generated per map that declares "
                                       "it, the copy is never served\n"));
                CHECK(dr && !strstr(dr, "conflict generated/decls/entitydef/monster/imp.decl"));
                CHECK(dr && strstr(dr, "MODS: doctor: 0 conflict(s), ") && strstr(dr, ", 2 edit source(s) of 1 game file(s) "));
                if (getenv("MODUI_DEBUG")) printf("    doctor: %s\n", dr ? dr : "(refused)");
            }
            doc = list_doc();
            {
                /* the whole row of alex.edits2, up to the next row */
                const char *row = doc ? strstr(doc, "{\"key\":\"alex.edits2\"") : NULL;
                const char *next = row ? strstr(row + 1, "{\"key\":\"") : NULL;
                size_t n = row ? (next ? (size_t)(next - row) : strlen(row)) : 0;
                char *one = row ? (char *)malloc(n + 1) : NULL;
                if (one) { memcpy(one, row, n); one[n] = 0; }
                CHECK(one && strstr(one, "\"counts\":{\"shadows\":0,") && strstr(one, "\"replaces\":0,") &&
                      strstr(one, "\"replacements\":[{\"path\":\"generated/decls/entitydef/monster/imp.decl\",\"edits\":true}]"));
                free(one);
            }
            free(doc);
            open_test_map();
            g_placed_count = 0;
            /* pass 0: both rows pin builds that are not installed and nothing
             * substituted them (the index's other build of the key serves);
             * pass 1: the editor load substituted each row's installed build. */
            for (pass = 0; pass < 2; pass++) {
                g_sub_ids[0] = ids[0]; g_sub_to[0] = h_one;
                g_sub_ids[1] = ids[1]; g_sub_to[1] = h_two;
                CHECK(load_rows(ids, gone, 2, pass));
                doc = list_doc();
                if (!has(doc, "\"registryClashes\":[{\"slot\":\"entitydef/monster/imp/size\"")) printf("    %s\n", doc ? strstr(doc, "\"map\":{") : "(no doc)");
                CHECK(has(doc, "\"registryClashes\":[{\"slot\":\"entitydef/monster/imp/size\",\"providers\":"
                               "[{\"id\":\"alex.edits\",\"name\":\"Edits\"},{\"id\":\"alex.edits2\",\"name\":\"Edits Two\"}],"
                               "\"chosen\":\"\",\"serving\":\"alex.edits\"}]"));
                free(doc);
            }
            memset(g_sub_ids, 0, sizeof g_sub_ids);
            g_map_on = was_on;
            g_placed_count = was_placed;
        }
        sh_mb_set_stock_ops(NULL);
    }

    printf("-- a builder warning reaches the tab's row, the doctor and the rescan lines, not only the build\n");
    {
        char wd[MAX_PATH];
        _snprintf_s(wd, sizeof wd, _TRUNCATE, "%s\\authoring\\alex.warn", tmp);
        mt_write(wd, "mod.json", "{\"id\":\"alex.warn\",\"version\":\"1.0.0\",\"name\":\"Warn\",\"author\":\"t\","
                                 "\"displayName\":\"Warn\"}");
        mt_write(wd, "assets\\generated\\image\\mods\\alex.warn\\w.bimage", "warn bytes");
        doc = run_doc(SH_MODS_ACT_INSTALL, wd);
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"warnings\":1") &&
              has(doc, "\"firstWarning\":\"mod.json key \\\"displayName\\\": the mod store refuses it at publish"));
        free(doc);
        /* The store's copy is read back from its mod.json: the warning is derived again. */
        doc = list_doc();
        CHECK(has(doc, "\"warnings\":[\"mod.json key \\\"displayName\\\": the mod store refuses it at publish "
                       "(\\\"bad extra key\\\"); author keys must be 1-64 characters") &&
              has(doc, "\"warningCount\":1,\"firstWarning\":\"mod.json key \\\"displayName\\\""));
        /* A mod without warnings says so with an empty list. */
        CHECK(has(doc, "\"warnings\":[],\"warningCount\":0,\"firstWarning\":\"\""));
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: bundle mods\\alex.warn: warning 1 of 1: mod.json key \"displayName\": the mod store "
                      "refuses it at publish (\"bad extra key\")"));
        g_logat = 0;
        g_logbuf[0] = 0;
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(g_logbuf, "MODS: ui rescan: mods\\alex.warn (alex.warn): 1 warning(s), first: mod.json key \"displayName\""));
        free(doc);
        /* The tab's Rebuild says it too, and the list after it still carries it. */
        doc = run_doc(SH_MODS_ACT_REBUILD, "alex.warn");
        CHECK(has(doc, "\"action\":\"rebuild\",\"ok\":true") && has(doc, "\"warnings\":1") &&
              has(doc, "\"firstWarning\":\"mod.json key \\\"displayName\\\""));
        CHECK(has(g_logbuf, "MODS: ui rebuild alex.warn: 1 warning(s), first: mod.json key \"displayName\""));
        free(doc);
        doc = list_doc();
        CHECK(has(doc, "\"warningCount\":1,\"firstWarning\":\"mod.json key \\\"displayName\\\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.warn");
        free(doc);
    }

    keys_match_fixture(payload, g_union ? g_union : "");
    free(g_union);
    g_union = NULL;

    mt_remove_tree(tmp);
    printf("-- uninstall by key from the console: every build of the key; <key>@<hash16>: that build\n");
    {
        char twin[MAX_PATH], beside[200] = "", h1[17] = "", h2[17] = "", one[240], line[400], gone[MAX_PATH * 2];
        const char *by_key[] = { "uninstall", "twin.key" };
        const char *by_key_yes[] = { "uninstall", "twin.key", "--yes" };
        const char *by_build[] = { "uninstall", one };
        const char *k, *h;
        size_t kl = 0;
        _snprintf_s(twin, sizeof twin, _TRUNCATE, "%s\\authoring\\twin.key", tmp);
        mt_write(twin, "mod.json", "{\"id\":\"twin.key\",\"version\":\"1.0.0\",\"name\":\"Twin\",\"author\":\"t\"}");
        mt_write(twin, "assets\\generated\\image\\mods\\twin.key\\a.bimage", "the first build's bytes");
        mt_write(twin, "assets\\generated\\decls\\entitydef\\mods\\twin.key\\thing.decl",
                 "{\n\tinherit = \"snapmaps/placeablesnapaiencounter/imp\";\n}\n");
        doc = run_doc(SH_MODS_ACT_INSTALL, twin);
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"key\":\"twin.key\""));
        h = doc ? strstr(doc, "\"bundle\":\"") : NULL;
        if (h) memcpy(h1, h + 10, 16);
        free(doc);
        mt_write(twin, "assets\\generated\\image\\mods\\twin.key\\a.bimage", "the second build's other bytes");
        doc = run_doc(SH_MODS_ACT_INSTALL, twin);
        CHECK(has(doc, "\"ok\":true"));
        k = doc ? strstr(doc, "\"key\":\"twin.key@") : NULL;
        if (k) { k += 7; while (k[kl] && k[kl] != '"' && kl + 1 < sizeof beside) kl++; memcpy(beside, k, kl); beside[kl] = 0; }
        h = doc ? strstr(doc, "\"bundle\":\"") : NULL;
        if (h) memcpy(h2, h + 10, 16);
        free(doc);
        CHECK(beside[0] && h1[0] && h2[0] && strcmp(h1, h2));
        /* <key>@<hash16> names the first build alone, whose folder is the bare key. */
        _snprintf_s(one, sizeof one, _TRUNCATE, "twin.key@%s", h1);
        g_doctor[0] = 0;
        CHECK(sh_modui_command(2, by_build, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: uninstall: moves the folder mods\\twin.key to the Windows Recycle Bin"));
        CHECK(!has(g_doctor, "builds of twin.key"));
        /* Its consequences are the key's: the other build stays and serves the maps. */
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: uninstall: another folder of twin.key stays installed (mods\\%s); "
                    "maps that use it open with it\n", beside);
        CHECK(has(g_doctor, line));
        CHECK(!has(g_doctor, "saved maps use it") && !has(g_doctor, "will not open"));
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "twin.key");   /* the tab names the folder */
        _snprintf_s(line, sizeof line, _TRUNCATE, "\"keptBuild\":\"%s\"", beside);
        if (!has(doc, line)) fprintf(stderr, "  twin preview: %s\n", doc ? doc : "(null)");
        CHECK(has(doc, line));
        free(doc);
        /* The key: every build, each folder named, in the dry run. */
        g_doctor[0] = 0;
        CHECK(sh_modui_command(2, by_key, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: uninstall: removes all 2 folders of twin.key; to keep one, uninstall the others as "
                            "\"twin.key@<hash16>\"\n"));
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: uninstall:   mods\\twin.key (twin.key 1.0.0, bundle %s)\n", h1);
        CHECK(has(g_doctor, line));
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: uninstall:   mods\\%s (twin.key 1.0.0, bundle %s)\n", beside, h2);
        CHECK(has(g_doctor, line));
        CHECK(has(g_doctor, "MODS: uninstall twin.key: dry run; add --yes to do it\n"));
        /* A map load lists, for its placed names, the build the new map
         * serves (its rows' build, else the newest), never the build the
         * previous map declared: a rawmap opened over a map pinning the
         * first build saved that build (ADD-L6 F2). */
        {
            static const char TWIN_MAP[] =
                "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/twin.key/thing\","
                "\"name\":\"\",\"state\":{}}}],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
            sh_modmanifest_list rows, cur, saved_rows;
            sh_modmanifest_row row;
            sh_modserve_bundle_ref first, newest;
            char got[8][SH_MODSERVE_HASH_HEX + 1], err[256], *text, *loaded, *saved;
            size_t tl = 0, ll = 0, sl = 0;
            unsigned rejected = 0;
            int n;
            CHECK(sh_modserve_bundle_serving(NULL, "twin.key", &newest) && !_strnicmp(newest.hash, h2, 16));
            {
                /* The first build by its folder's hash. */
                sh_ms_list all;
                size_t q;
                memset(&first, 0, sizeof first);
                CHECK(sh_ms_enumerate(&all));
                for (q = 0; q < all.count; q++)
                    if (all.items[q].loaded && !strcmp(all.items[q].name, "twin.key"))
                        strcpy_s(first.hash, sizeof first.hash, all.items[q].bundle.hash);
                sh_ms_list_free(&all);
                CHECK(first.hash[0] && !_strnicmp(first.hash, h1, 16));
            }
            sh_modserve_scope_enable(1);
            /* The map open before pins the first build by its row. */
            memset(&rows, 0, sizeof rows);
            memset(&row, 0, sizeof row);
            strcpy_s(row.id, sizeof row.id, "twin.key");
            strcpy_s(row.version, sizeof row.version, "1.0.0");
            strcpy_s(row.bundle, sizeof row.bundle, first.hash);
            CHECK(sh_modmanifest_row_add_name(&row, "mods/twin.key/thing"));
            CHECK(sh_modmanifest_list_add(&rows, &row));
            sh_modmanifest_row_free(&row);
            text = sh_modmanifest_write(TWIN_MAP, sizeof TWIN_MAP - 1, &rows, &tl, err, sizeof err);
            sh_modmanifest_list_free(&rows);
            CHECK(text != NULL);
            loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
            if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
            if (text) HeapFree(GetProcessHeap(), 0, text);
            n = sh_modserve_scope_hashes(got, 8);
            CHECK(n == 1 && !_stricmp(got[0], first.hash));
            CHECK(g_act_last_n == 1 && !_stricmp(g_act_last[0], first.hash));   /* its row's build */
            /* The map opened over it carries no row and places the tile. */
            log_clear();
            loaded = sh_modmanifest_prepare_load(TWIN_MAP, sizeof TWIN_MAP - 1, &ll);
            if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
            CHECK(sh_modmanifest_current(&cur) == 0);
            sh_modmanifest_list_free(&cur);
            n = sh_modserve_scope_hashes(got, 8);
            CHECK(n == 1 && !_stricmp(got[0], newest.hash));
            /* The load activates the placed closure as a saved map's load activates its rows
             * (ADD-V4a R): its decls, materials, models and registries register now, so Play from
             * the unsaved map serves them. */
            CHECK(g_act_last_n == 1 && !_stricmp(g_act_last[0], newest.hash));
            CHECK(has(g_logbuf, "MODS: map: 1 bundle(s) its entities place without a row were activated with its rows"));
            /* Its first save records the newest build. */
            saved = sh_modmanifest_on_save(TWIN_MAP, sizeof TWIN_MAP - 1, &sl);
            CHECK(saved != NULL);
            memset(&saved_rows, 0, sizeof saved_rows);
            if (saved) {
                sh_modmanifest_read(saved, sl, &saved_rows, &rejected);
                HeapFree(GetProcessHeap(), 0, saved);
            }
            CHECK(saved_rows.count == 1 && !_stricmp(saved_rows.rows[0].bundle, newest.hash));
            sh_modmanifest_list_free(&saved_rows);
            CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
            sh_modmanifest_reset();
            sh_modserve_scope_enable(0);
        }
        /* The tab names one folder, as before. */
        doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "twin.key");
        CHECK(has(doc, "\"install\":{\"folder\":\"twin.key\",") && !has(doc, "\"folders\":"));
        free(doc);
        /* --yes removes both; the key is then not installed. */
        g_doctor[0] = 0;
        CHECK(sh_modui_command(3, by_key_yes, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: uninstall twin.key: Uninstalled Twin (2 of 2 builds)"));
        CHECK(has(g_doctor, "Their folders were moved to the Recycle Bin"));
        _snprintf_s(gone, sizeof gone, _TRUNCATE, "%s\\twin.key", g_root);
        CHECK(!mt_exists(gone));
        _snprintf_s(gone, sizeof gone, _TRUNCATE, "%s\\%s", g_root, beside);
        CHECK(!mt_exists(gone));
        g_doctor[0] = 0;
        CHECK(sh_modui_command(2, by_key, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: uninstall twin.key: twin.key is not installed.\n"));
    }

    printf("-- an organizing folder: its mods' rows name it as their group\n");
    {
        char nested[MAX_PATH * 2];
        _snprintf_s(nested, sizeof nested, _TRUNCATE, "%s\\group1\\alex.nested", g_root);
        put_plain_mod(nested, "alex.nested", "1.0.0", "{\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_RESCAN, "");
        free(doc);
        doc = list_doc();
        CHECK(has(doc, "\"key\":\"group1\\\\alex.nested\",") && bundle_has(doc, "group1\\\\alex.nested", "\"folder\":\"alex.nested\",\"group\":\"group1\""));
        {
            /* About's Folder link sends group\folder: under mods\ that names this mod's folder */
            char group[MAX_PATH], folder[MAX_PATH], joined[MAX_PATH * 3];
            DWORD at;
            CHECK(row_str(doc, "group1\\\\alex.nested", "group", group, sizeof group) &&
                  row_str(doc, "group1\\\\alex.nested", "folder", folder, sizeof folder));
            _snprintf_s(joined, sizeof joined, _TRUNCATE, "%s\\%s\\%s", g_root, group, folder);
            at = GetFileAttributesA(joined);
            CHECK(at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && _stricmp(joined, nested) == 0);
        }
        free(doc);
    }

    printf("-- a refused build that serves nothing is named by its mod.json, the folder only when that does not parse\n");
    {
        /* V13: v13-group\v13_broken (a decl with unbalanced braces, no last good build) was
         * listed as "v13-group\v13_broken" although its mod.json named it. */
        char broken[MAX_PATH * 2], bad[MAX_PATH * 2];
        _snprintf_s(broken, sizeof broken, _TRUNCATE, "%s\\group2", g_root);
        CreateDirectoryA(broken, NULL);
        _snprintf_s(broken, sizeof broken, _TRUNCATE, "%s\\group2\\alex.broken", g_root);
        mt_write(broken, "mod.json", "{\"id\":\"alex.broken\",\"version\":\"1.0.0\",\"name\":\"Broken thing\",\"author\":\"t\"}");
        mt_write(broken, "assets\\generated\\decls\\entitydef\\mods\\alex.broken\\thing.decl", "{\r\n\tinherit = \"x\";\r\n");
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\group2\\alex.badjson", g_root);
        mt_write(bad, "mod.json", "{\"id\":\"alex.badjson\",\"name\":\"Never shown\"");
        mt_write(bad, "assets\\generated\\decls\\entitydef\\mods\\alex.badjson\\thing.decl", "{\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_RESCAN, "");
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "group2\\\\alex.broken", "\"state\":\"attention\""));
        CHECK(bundle_has(doc, "group2\\\\alex.broken", "\"name\":\"Broken thing\""));
        CHECK(bundle_has(doc, "group2\\\\alex.badjson", "\"name\":\"\""));   /* the tab shows the folder */
        if (!bundle_has(doc, "group2\\\\alex.broken", "\"name\":\"Broken thing\"")) printf("    %s\n", doc ? doc : "(null)");
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILES, "group2\\alex.broken");
        CHECK(has(doc, "\"key\":\"group2\\\\alex.broken\",\"name\":\"Broken thing\""));
        free(doc);
        mt_remove_tree(broken);
        mt_remove_tree(bad);
        doc = run_doc(SH_MODS_ACT_RESCAN, "");
        free(doc);
    }

    printf("-- the Files view: every file with its class, a game file's archive, a refused file's cause\n");
    {
        char fm[MAX_PATH * 2], arg[MAX_PATH * 2];
        sh_mb_set_stock_ops(&g_files_ops);
        _snprintf_s(fm, sizeof fm, _TRUNCATE, "%s\\alex.files", g_root);
        mt_write(fm, "mod.json", "{\"id\":\"alex.files\",\"version\":\"1.0.0\",\"name\":\"Files\",\"author\":\"t\","
                                 "\"stock\":{\"generated/decls/entitydef/mods/alex.files/imp_copy.decl\":"
                                 "{\"source\":\"generated/decls/entitydef/monster/imp.decl\"}}}");
        mt_write(fm, "assets\\generated\\decls\\entitydef\\mods\\alex.files\\own.decl",
                 "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_RESCAN, "");
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILES, "alex.files");
        if (!has(doc, "\"class\":\"borrow\"")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(has(doc, "\"action\":\"modFiles\",\"ok\":true,\"message\":\"\",\"key\":\"alex.files\",\"name\":\"Files\",\"cause\":\"\""));
        CHECK(has(doc, "{\"path\":\"mod.json\",\"size\":"));
        CHECK(has(doc, "\"kind\":\"text\",\"class\":\"descriptor\",\"game\":false,\"container\":\"\",\"inFolder\":true,\"cause\":\"\"}"));
        CHECK(has(doc, "{\"path\":\"assets/generated/decls/entitydef/mods/alex.files/own.decl\",\"size\":"));
        CHECK(has(doc, "\"kind\":\"text\",\"class\":\"add\",\"game\":false,\"container\":\"\",\"inFolder\":true,\"cause\":\"\"}"));
        /* the author's pointer: the folder does not hold it; the install's campaign and multiplayer archive does */
        CHECK(has(doc, "{\"path\":\"assets/generated/decls/entitydef/mods/alex.files/imp_copy.decl\",\"size\":"));
        CHECK(has(doc, "\"kind\":\"text\",\"class\":\"borrow\",\"game\":true,\"container\":\"gameresources\",\"inFolder\":false,\"cause\":\"\"}"));
        CHECK(has(doc, "\"truncated\":false"));
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "alex.files", "\"folder\":\"alex.files\",\"group\":\"\""));   /* at the root */
        free(doc);

        printf("-- one file's text: bounded to the folder, text kinds only, mod.json without its product block\n");
        _snprintf_s(arg, sizeof arg, _TRUNCATE, "alex.files\tassets/generated/decls/entitydef/mods/alex.files/own.decl");
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, arg);
        CHECK(has(doc, "\"action\":\"modFileText\",\"ok\":true") &&
              has(doc, "\"text\":\"{\\u000d\\u000a\\u0009inherit = \\\"ai/demon/imp_coop\\\";\\u000d\\u000a}\\u000d\\u000a\","
                       "\"productBlock\":false,\"cause\":\"\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\tmod.json");
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"productBlock\":true") && has(doc, "\\\"name\\\":\\\"Files\\\"") &&
              !has(doc, "snapmap-plus\\\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\tassets/generated/decls/entitydef/mods/alex.files/imp_copy.decl");
        CHECK(has(doc, "\"ok\":false,\"message\":\"The mod's folder does not hold this file; it is read from your install.\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\t..\\alex.req\\mod.json");
        CHECK(has(doc, "\"ok\":false,\"message\":\"That file is not inside the mod's folder.\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\tC:\\Windows\\win.ini");
        CHECK(has(doc, "\"ok\":false,\"message\":\"That file is not inside the mod's folder.\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\tassets/generated/image/x.bimage");
        CHECK(has(doc, "\"ok\":false,\"message\":\"This file is binary; it has no text to show.\""));
        free(doc);
        mt_put(fm, "assets\\notes.txt", "a\x01" "b\xff", "wb");
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "alex.files\tassets/notes.txt");
        CHECK(has(doc, "\"ok\":false,\"message\":\"This file is not plain text; it cannot be shown here.\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "nobody\tmod.json");
        CHECK(has(doc, "\"ok\":false") && has(doc, "no longer installed"));
        free(doc);

        /* A refused file: the latest build's cause names it; the last good build keeps serving. */
        mt_put(fm, "assets\\generated\\decls\\entitydef\\mods\\alex.files\\broken.decl", "", "wb");
        doc = run_doc(SH_MODS_ACT_REBUILD, "alex.files");
        CHECK(has(doc, "\"ok\":false"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILES, "alex.files");
        if (!has(doc, "\"cause\":\"empty file: assets/generated/decls/entitydef/mods/alex.files/broken.decl\"}")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(has(doc, "\"key\":\"alex.files\",\"name\":\"Files\",\"cause\":\"empty file: assets/generated/decls/entitydef/mods/alex.files/broken.decl\""));
        CHECK(has(doc, "{\"path\":\"assets/generated/decls/entitydef/mods/alex.files/broken.decl\",\"size\":0,\"kind\":\"text\",\"class\":\"\","
                       "\"game\":false,\"container\":\"\",\"inFolder\":true,\"cause\":\"empty file: assets/generated/decls/entitydef/mods/alex.files/broken.decl\"}"));
        CHECK(has(doc, "\"class\":\"add\",\"game\":false,\"container\":\"\",\"inFolder\":true,\"cause\":\"\"}"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILES, "nobody");
        CHECK(has(doc, "\"ok\":false") && has(doc, "no longer installed"));
        free(doc);
        sh_mb_set_stock_ops(NULL);
    }

    printf("-- New mod: a folder with a working example, built at once, its Toybox row named\n");
    {
        char key[SH_MB_ID_MAX + 1] = "", row[MAX_PATH * 2], want[400];
        const char *k, *f;
        const sh_ms_entry *e = NULL;
        size_t i;
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "My first mod");
        if (!has(doc, "Toybox.\"")) printf("    %s\n", doc ? doc : "(null)");
        k = doc ? strstr(doc, "\"key\":\"") : NULL;
        if (k) { k += 7; f = strchr(k, '"'); if (f && (size_t)(f - k) < sizeof key) { memcpy(key, k, (size_t)(f - k)); key[f - k] = 0; } }
        /* the salted key, recorded at creation and kept by the first build */
        CHECK(!strncmp(key, "my_first_mod_", 13) && strlen(key) == 13 + SH_MB_KEY_SUFFIX);
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"message\":\"Created My first mod in mods\\\\%s. Its Example crate is in the Toybox.\"", key);
        CHECK(has(doc, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"file\":\"assets/generated/decls/snapeditorentitydef/mods/%s/example_crate.decl\"", key);
        CHECK(has(doc, want));
        free(doc);
        CHECK(sh_ms_enumerate(&list));
        for (i = 0; i < list.count && !e; i++) if (!strcmp(list.items[i].name, key)) e = &list.items[i];
        CHECK(e && sh_ms_entry_usable(e) && !e->build_failed[0] && e->bundle.errors == 0 && !strcmp(e->bundle.id, key) &&
              e->bundle.key_source == SH_MB_KEY_FROM_BLOCK && e->bundle.classes.adds == 2 && e->bundle.file_count == 2);
        if (e && e->bundle.warnings) printf("    new mod warning: %s\n", e->bundle.first_warning);
        sh_ms_list_free(&list);
        doc = list_doc();
        CHECK(bundle_has(doc, key, "\"name\":\"My first mod\"") && bundle_has(doc, key, "\"state\":\"installed\"") &&
              bundle_has(doc, key, "\"editorRows\":1"));
        CHECK(has(doc, "\"editorRowNames\":[\"Example crate\"]"));   /* the row's name from the mod's strings */
        free(doc);
        _snprintf_s(row, sizeof row, _TRUNCATE, "%s\tassets/generated/decls/snapeditorentitydef/mods/%s/example_crate.decl", key, key);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, row);
        _snprintf_s(want, sizeof want, _TRUNCATE, "entityDef = \\\"mods/%s/example_crate\\\";", key);
        CHECK(has(doc, "\"ok\":true") && has(doc, want));
        free(doc);
        /* The example is the game's crate01, field for field (V13: the earlier row's
         * renderInfo.renderModelName drew the model a second time in the editor, which logged
         * "modelMatrix invert failed" every frame). The stock texts are the game's own decls
         * snapeditorentitydef/prop/new/crate01.decl and entitydef/snapmaps/props/new/crate01.decl: the row differs only in its names, the
         * entityDef is the game's text whole, editorVars included (V14 saw the hover warning
         * with the block left out). */
        {
            static const char stock_row[] =
                "{\n\tinherit = \"prop/prop_base\";\n\tedit = {\n"
                "\t\tpreviewImage = \"textures/guis/snapmaps/entity_icons/prop/crate01\";\n"
                "\t\tdisplayNameTag = \"#str_snapentity_prop_crate01\";\n"
                "\t\tdisplayDescriptionTag = \"#str_snapentity_prop_crate01_disp_desc\";\n"
                "\t\tentityDef = \"snapmaps/props/new/crate01\";\n"
                "\t\trenderInfo = {\n\t\t\tsize = {\n\t\t\t\tx = 54.3767967224;\n\t\t\t\ty = 54.3779754639;\n\t\t\t\tz = 53.595500946;\n\t\t\t}\n"
                "\t\t\tarrowSize = {\n\t\t\t\tx = 2;\n\t\t\t\ty = 2;\n\t\t\t\tz = 1;\n\t\t\t}\n\t\t}\n"
                "\t\teventInfo = {\n\t\t\tselectedEvent = {\n\t\t\t\tsound = \"play_pickup_heavy_crates\";\n\t\t\t}\n"
                "\t\t\tplacedEvent = {\n\t\t\t\tsound = \"play_putdown_heavy_crates\";\n\t\t\t}\n\t\t}\n\t}\n}";
            static const char stock_def_edit[] =
                "\teditorVars {\n\t\trotatable = false;\n\t\tshowOrientation = false;\n\t\tshowFixedBox = false;\n"
                "\t\tshowInRenderMode = false;\n\t\tplaceable = false;\n\t\tplaceableInLayers = false;\n"
                "\t\tcolor = ( 1, 1, 1, 1 );\n\t\toriginOffset = ( 0, 0, 0 );\n\t\tmaterial = \"\";\n"
                "\t\teditorName = \"\";\n\t\tdescription = \"\";\n\t}\n"
                "\tedit = {\n\t\trenderModelInfo = {\n\t\t\tmodel = \"models/snapmaps/props/new/crate01.lwo\";\n\t\t}\n"
                "\t\tclipModelInfo = {\n\t\t\ttype = \"CLIPMODEL_AUTO\";\n\t\t}\n\t\tdynamicObstacle = true;\n\t}\n}";
            char path[MAX_PATH * 2], expect[4096], name[200], *got, *p, *q;
            /* the stock row with the example's names, and the mod's category line after previewImage */
            _snprintf_s(name, sizeof name, _TRUNCATE, "\t\tcategoryNameTag = \"#str_%s_category\";\n", key);
            p = strstr(stock_row, "\t\tdisplayNameTag");
            _snprintf_s(expect, sizeof expect, _TRUNCATE, "%.*s%s", (int)(p - stock_row), stock_row, name);
            q = (char *)p;
            _snprintf_s(expect + strlen(expect), sizeof expect - strlen(expect), _TRUNCATE,
                        "\t\tdisplayNameTag = \"#str_%s_example_crate_name\";\n"
                        "\t\tdisplayDescriptionTag = \"#str_%s_example_crate_desc\";\n"
                        "\t\tentityDef = \"mods/%s/example_crate\";\n%s", key, key, key, strstr(q, "\t\trenderInfo"));
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s/assets/generated/decls/snapeditorentitydef/mods/%s/example_crate.decl", g_root, key, key);
            got = (char *)sh_mb_read_file(path, 1u << 20, NULL);
            if (!got) printf("    cannot read %s\n", path);
            if (got) { char *w = got, *r = got; for (; *r; r++) if (*r != '\r') *w++ = *r; *w = 0; while (w > got && w[-1] == '\n') *--w = 0; }
            CHECK(got && !strcmp(got, expect));
            if (got && strcmp(got, expect)) printf("    example row:\n%s\n    expected:\n%s\n", got, expect);
            CHECK(got && !strstr(got, "renderModelName"));
            free(got);
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s/assets/generated/decls/entitydef/mods/%s/example_crate.decl", g_root, key, key);
            got = (char *)sh_mb_read_file(path, 1u << 20, NULL);
            if (!got) printf("    cannot read %s\n", path);
            if (got) { char *w = got, *r = got; for (; *r; r++) if (*r != '\r') *w++ = *r; *w = 0; while (w > got && w[-1] == '\n') *--w = 0; }
            CHECK(got && !strncmp(got, "{\n\tclass = \"idProp_Coop\";\n", 26) && !strcmp(got + 26, stock_def_edit));
            free(got);
        }
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "   ");
        CHECK(has(doc, "\"ok\":false,\"message\":\"No mod was created: a new mod needs a name.\""));
        free(doc);
        g_busy = "Play is running";
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "Later");
        CHECK(has(doc, "\"ok\":false,\"message\":\"Not now: Play is running.\""));
        free(doc);
        g_busy = NULL;
    }

    printf("modui_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
