/* modui_test.c -- the Mods tab's backend (modui.c) against the real store,
 * serving index and map manifest in a temporary folder (the one shape: mods
 * keyed by salt, the player's own at the root, a downloaded map's in its
 * place): the document's shape on an empty store (payload v4, no migration
 * state), the main-thread refusal, Refresh's result, an install through the
 * tab, a hand-made folder salted at Refresh, a copy made a mod of its own, the
 * informational same-files hint, the plain causes of a failed build and of
 * folders in an earlier shape, a downloaded map's place group, the open map's rows by salt with the asset rule's
 * states, Use in this map, Remove from map and Uninstall with their dry runs
 * against a fake editor, the console verbs with their --yes gate, the Files
 * view, New mod (the message saying Refresh picks up edits), the doctor, the
 * vocabulary rule over every sentence a player reads (no salt, hash,
 * "bundle", "key", "pointer", "surface", "legacy", "migrate" or build number),
 * and that every key the page's fixture uses is a key the backend writes.
 *
 *   modui_test.exe <tests\fixtures\mods\alex.r2-imp> <tests\fixtures\mods_tab_payload_v4.json>
 */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "snapmap_plus_iface.h"
#include "modui.h"
#include "modstore.h"
#include "modserve.h"
#include "modmanifest.h"
#include "modregistry.h"
#include "mod_test_util.h"
#include "editor_delete.h"
#include "modzip.h"
#include "map_carrier.h"

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
static int stock_cb(void *context, const char *request) { (void)context; return fake_stock(request); }

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
        g_module_mod ? "mods/alexrimp/imp_copy" : "maps/modules/stock/room_a");
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

/* g_delete_keeps: the delete dispatches but its objects are back by the commit (an editor that did not
 * finish it, or a Ctrl+Z before the row would go). */
static int g_delete_status = SH_EDITOR_DELETE_DISPATCHED, g_delete_calls, g_deleted[16], g_deleted_n, g_delete_keeps;
static int fake_delete(const int *ids, int count)
{
    int i, k;
    g_delete_calls++;
    if (g_delete_status != SH_EDITOR_DELETE_DISPATCHED) return g_delete_status;
    for (i = 0; i < count; i++) {
        if (g_deleted_n < 16) g_deleted[g_deleted_n++] = ids[i];
        for (k = 0; k < g_ent_n && !g_delete_keeps; k++) if (g_ents[k].uid == ids[i]) g_ents[k].alive = 0;
    }
    return SH_EDITOR_DELETE_DISPATCHED;
}

/* Whether the open map's list of mods (the current manifest) has a row of `salt`. */
static int current_has(const char *salt)
{
    sh_modmanifest_list rows;
    size_t i;
    int hit = 0;
    if (!sh_modmanifest_current(&rows)) return 0;
    for (i = 0; i < rows.count; i++) hit |= !_stricmp(rows.rows[i].salt, salt);
    sh_modmanifest_list_free(&rows);
    return hit;
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
        { 1, "mods/alexrimp/imp_copy", "idSnapEntity", "", 1 },
        { 2, "mods/alexrimp/imp_copy", "idSnapEntity", "Gate imp", 1 },
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

/* Mod scripts: the builder's compile check (script_compile.c in the product)
 * fails a text holding BROKEN at line 4 as Lua would say it; the runtime's
 * state, log and the sound banks a restart brings in are the test's. */
static int fake_script_check(const char *file, int entry, const char *text, size_t len, int *line, char **what, char **todo)
{
    (void)file; (void)entry; (void)len;
    if (!strstr(text, "BROKEN")) return 1;
    *line = 4;
    ho_set(what, "\"end\" expected to close \"function\" on line 2");
    ho_set(todo, "correct it, then press Refresh");
    return 0;
}
static int g_script_on;                 /* the runtime knows g_script_salt */
static char g_script_salt[16];
static sh_modui_script_info g_script;
static int fake_script_info(const char *salt, sh_modui_script_info *out)
{
    if (!g_script_on || _stricmp(salt, g_script_salt)) return 0;
    *out = g_script;
    out->stop = g_script.stop ? _strdup(g_script.stop) : NULL;   /* the reader frees its copy */
    return 1;
}
static int fake_script_log(const char *salt, sh_modui_line_fn fn, void *context)
{
    if (!g_script_on || _stricmp(salt, g_script_salt)) return 0;
    fn(context, "door: switch 1 of 3");
    fn(context, "a line with \"quotes\" and a \\ backslash");
    if (g_script.stop) fn(context, g_script.stop);
    return g_script.stop ? 3 : 2;
}
static int fake_script_mods(sh_modui_name_fn fn, void *context)
{
    if (!g_script_on) return 0;
    fn(context, g_script_salt);
    return 1;
}
static char g_bank[64], g_bank_owner[16];
static int fake_restart_banks(void (*fn)(void *context, const char *bank, const char *owner), void *context)
{
    if (!g_bank[0]) return 0;
    if (fn) fn(context, g_bank, g_bank_owner);
    return 1;
}

static void bind_fakes(void)
{
    sh_modui_engine e;
    memset(&e, 0, sizeof e);
    e.script_info = fake_script_info;
    e.script_log = fake_script_log;
    e.script_mods = fake_script_mods;
    e.restart_banks = fake_restart_banks;
    e.map_json = fake_map_json;
    e.map_info = fake_map_info;
    e.delete_entities = fake_delete;
    e.remove_tag = fake_remove_tag;
    e.busy = fake_busy;
    e.toast = fake_toast;
    e.print = fake_print;
    e.on_main_thread = fake_on_main;
    e.placed_names = fake_placed;
    e.rescan = fake_rescan;
    e.refresh = fake_refresh;
    e.palette_rebuild = fake_palette;
    e.stock_exists = fake_stock;
    e.scope_apply = fake_scope_apply;
    e.scope_activate = fake_scope_activate;
    e.map_left = fake_map_left;
    e.clear_undo = fake_clear_undo;
    e.mark_modified = fake_mark_modified;
    e.undo_pending = fake_undo_pending;
    e.launch_preload = fake_preload;
    sh_modui_bind(&e);
}

/* An editor load of the open test map with rows of `salts` at `builds`
 * (map-level), named `names`. */
static int load_rows(const char *const *salts, const char *const *builds, const char *const *names, int n)
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
        strcpy_s(row.salt, sizeof row.salt, salts[i]);
        strcpy_s(row.build, sizeof row.build, builds[i]);
        sh_modmanifest_row_set_name(&row, names[i]);
        row.size = 10;
        row.attached = 1;
        if (!sh_modmanifest_list_add(&rows, &row)) return 0;
    }
    map = map_text(&len);
    text = sh_modmanifest_write(map, len, &rows, &tl, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    if (!text) { printf("    write: %s\n", err); return 0; }
    sh_modmanifest_set_editor_load(1);
    loaded = sh_modmanifest_prepare_load(text, tl, &ll);
    sh_modmanifest_set_editor_load(0);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    HeapFree(GetProcessHeap(), 0, text);
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

static int has(const char *doc, const char *needle) { return doc && strstr(doc, needle) != NULL; }

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
 * members of "registries" and "requirements" are a mod's own data (cvar
 * names, bank names), not the payload's keys, and are skipped; so are the
 * members of the shapes this suite's store does not make (a game sound bank's
 * state, a clash between two mods' edits, a derived edit's counts and a
 * received folder's replacements: modserve_test, modregistry_test and
 * modsound_test build those), whose own key is still checked. */
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
            /* stockBanks is written only for a mod whose registries name a game sound bank (modsound_test) */
            if (strcmp(key, "stockBanks") && !strstr(doc, probe)) {
                fprintf(stderr, "  fixture key %s is not in the backend document\n", key);
                missing++;
            }
            if (!strcmp(key, "registries") || !strcmp(key, "requirements") || !strcmp(key, "stockBanks") ||
                !strcmp(key, "registryClashes") || !strcmp(key, "edits") || !strcmp(key, "replacements")) {
                /* skip the value's members */
                const char *o = q + 2;
                int depth = 0;
                while (*o == ' ') o++;
                if (*o == '{' || *o == '[') {
                    for (; *o; o++) {
                        if (*o == '{' || *o == '[') depth++;
                        else if ((*o == '}' || *o == ']') && --depth == 0) break;
                    }
                    if (*o) { p = o + 1; continue; }
                }
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
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alexrimp/imp_copy\","
    "\"name\":\"\",\"state\":{}}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}}";

/* Load a map whose manifest names `salt` at `build` (an installed build or not). */
static void load_map_naming(const char *salt, const char *build, const char *name, const char *placed)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    char err[256], *text, *loaded;
    size_t tl = 0, ll = 0;
    memset(&rows, 0, sizeof rows);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, salt);
    strcpy_s(row.build, sizeof row.build, build);
    sh_modmanifest_row_set_name(&row, name);
    row.size = 1234;
    CHECK(sh_modmanifest_row_add_name(&row, placed ? placed : "mods/alexrimp/imp_copy"));
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

/* A hand-made mod folder (no builder block): one decl under mods/<folder>/,
 * which the first Refresh rewrites to the salt it gives the folder. */
static void put_fresh_mod(const char *folder, const char *name, const char *leaf, const char *body)
{
    char desc[512], rel[300];
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "{\"name\":\"%s\",\"version\":\"1.0.0\",\"author\":\"tester\","
                "\"description\":\"A test mod.\"}", name);
    CreateDirectoryA(folder, NULL);
    mt_write(folder, "mod.json", desc);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\twin.decl", leaf);
    mt_write(folder, rel, body);
}

/* A ZIP at `path` holding `n` entries (name, text). */
static int put_zip(const char *path, const char *const *names, const char *const *texts, int n)
{
    sh_zip_writer w;
    wchar_t wpath[MAX_PATH * 2];
    int i, ok;
    memset(&w, 0, sizeof w);
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2) || !sh_zip_writer_open(&w, wpath)) return 0;
    for (i = 0; i < n; i++) sh_zip_writer_add(&w, names[i], texts[i], strlen(texts[i]));
    ok = sh_zip_writer_finish(&w);
    sh_zip_writer_free(&w);
    return ok;
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

/* Every sentence a player reads: no salt, no hash, none of the internal
 * words. `salts` are the store's salts this run made. */
static const char *g_salts[16];
static int g_salt_n;
static void note_salt(const char *s) { if (s && s[0] && g_salt_n < 16) g_salts[g_salt_n++] = _strdup(s); }
static int prose_ok(const char *text)
{
    static const char *const words[] = { "bundle", "pointer", "surface", "legacy", "migrat", " key", "key ", "salt", NULL };
    char low[4096];
    size_t i, n = text ? strlen(text) : 0, run = 0;
    int k;
    if (!text) return 1;
    for (i = 0; i < n && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)text[i]);
    low[i] = 0;
    for (k = 0; words[k]; k++) if (strstr(low, words[k])) { printf("    vocabulary: \"%s\" in: %s\n", words[k], text); return 0; }
    for (i = 0; i < n; i++) {
        run = isxdigit((unsigned char)text[i]) ? run + 1 : 0;
        if (run >= 16) { printf("    vocabulary: a hash in: %s\n", text); return 0; }
    }
    for (k = 0; k < g_salt_n; k++) if (strstr(text, g_salts[k])) { printf("    vocabulary: salt %s in: %s\n", g_salts[k], text); return 0; }
    return 1;
}
/* Every "message", "cause" and warning string of a document. */
static int doc_prose_ok(const char *doc)
{
    static const char *const fields[] = { "\"message\":\"", "\"cause\":\"", NULL };
    int k, ok = 1;
    for (k = 0; fields[k] && doc; k++) {
        const char *p = doc;
        while ((p = strstr(p, fields[k])) != NULL) {
            char one[2048];
            size_t n = 0;
            for (p += strlen(fields[k]); *p && *p != '"' && n + 1 < sizeof one; p++) {
                if (*p == '\\' && p[1]) p++;
                one[n++] = *p;
            }
            one[n] = 0;
            ok &= prose_ok(one);
        }
    }
    {
        const char *w = doc ? strstr(doc, "\"warnings\":[") : NULL;
        while (w) {
            const char *end = strchr(w, ']');
            char one[2048];
            size_t n = end ? (size_t)(end - w) : 0;
            if (n >= sizeof one) n = sizeof one - 1;
            memcpy(one, w, n);
            one[n] = 0;
            ok &= prose_ok(one + 12);
            w = strstr(w + 1, "\"warnings\":[");
        }
    }
    return ok;
}
/* The doctor's and the console's own lines (the serving index's scope and
 * registry reports are modserve.c's, outside this suite's vocabulary). */
static int lines_prose_ok(const char *lines)
{
    const char *p = lines;
    int ok = 1;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        char one[2048];
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= sizeof one) n = sizeof one - 1;
        memcpy(one, p, n);
        one[n] = 0;
        if (!strstr(one, ": scope:") && !strstr(one, "registry") && !strstr(one, "requirements") && !strstr(one, "map: scope") &&
            !strstr(one, "serving:") && !strstr(one, "cvar")) ok &= prose_ok(one + (strncmp(one, "MODS: ", 6) ? 0 : 6));
        p = end ? end + 1 : NULL;
    }
    return ok;
}

/* The salt the document gives the row of `key`. */
static void row_salt(const char *doc, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    (void)row_str(doc, key, "salt", out, cap);
}
static void row_build(const char *key, char *out, size_t cap)
{
    sh_ms_list l;
    size_t i;
    out[0] = 0;
    if (!sh_ms_enumerate(&l)) return;
    for (i = 0; i < l.count; i++)
        if (!strcmp(l.items[i].name, key) && l.items[i].loaded) strncpy_s(out, cap, l.items[i].bundle.hash, _TRUNCATE);
    sh_ms_list_free(&l);
}

/* ---- the tests ------------------------------------------------------------------------------- */

/* ---- the places cleanup (DECISIONS 22) ------------------------------------------------------ */

/* A directory's creation time set `days` back. */
static void age_dir(const char *dir, unsigned days)
{
    wchar_t *w = sh_mb_wpath(dir);
    HANDLE h = w ? CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL) : INVALID_HANDLE_VALUE;
    FILETIME now;
    ULARGE_INTEGER t;
    GetSystemTimeAsFileTime(&now);
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= (ULONGLONG)days * 86400ULL * 10000000ULL;
    now.dwLowDateTime = t.LowPart;
    now.dwHighDateTime = t.HighPart;
    CHECK(h != INVALID_HANDLE_VALUE && SetFileTime(h, &now, NULL, NULL));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(w);
}

static void test_places_stay(const char *tmp)
{
    char root[MAX_PATH * 2], src[MAX_PATH * 2], dir[MAX_PATH * 2], saves[MAX_PATH * 2], cause[HO_TEXT], *doc;
    static const char *const keeps[] = { "Old Keep", "Other Keep" };
    sh_mb_bundle b;
    size_t i;
    printf("-- places: nothing removes a downloaded map's mods by itself; the short view's Remove removes one folder\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\stay\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\stay\\built", tmp);
    mt_write(src, "mod.json", "{\"name\":\"Crate\"}");
    mt_write(src, "assets\\generated\\image\\mods\\cratebox\\a.bimage", "crate bytes");
    memset(&b, 0, sizeof b);
    CHECK(sh_mb_build(src, "cratebox", NULL, &b) && HO(cause, sh_mb_write(&b, src, HO_P(0))));
    sh_mb_free(&b);
    for (i = 0; i < 2; i++) {
        sh_ms_place pl;
        char note[MAX_PATH];
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\%s\\Crate", root, keeps[i]);
        mt_mkdirs_for(dir);
        CHECK(mt_copy_tree(src, dir));
        memset(&pl, 0, sizeof pl);
        pl.title = keeps[i];
        _snprintf_s(note, sizeof note, _TRUNCATE, "maps\\%s", keeps[i]);
        CHECK(sh_ms_place_note(&pl, note));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\%s", root, keeps[i]);
        age_dir(dir, 365);   /* arrived a year ago, never played since */
    }
    /* No saved map of the player records either place. */
    _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\stay\\saves", tmp);
    CreateDirectoryA(saves, NULL);
    sh_modui_test_set_saves_root(saves);
    /* What the product does at start: both places are still there. */
    (void)sh_ms_recover_work();
    free(run_doc(SH_MODS_ACT_RESCAN, NULL));
    for (i = 0; i < 2; i++) {
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\%s\\Crate\\mod.json", root, keeps[i]);
        CHECK(mt_exists(dir));
    }
    /* The player's Remove of one of them removes exactly that folder. */
    sh_ms_recycle_refuse_for_test(1);
    doc = run_doc(SH_MODS_ACT_UNINSTALL, "maps\\Old Keep\\Crate");
    CHECK(has(doc, "\"ok\":true"));
    free(doc);
    sh_ms_recycle_refuse_for_test(0);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\Old Keep\\Crate", root);
    CHECK(!mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\Other Keep\\Crate\\mod.json", root);
    CHECK(mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\built\\mod.json", root);
    CHECK(!mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\mod.json", src);
    CHECK(mt_exists(dir));
    sh_modui_test_set_saves_root(NULL);
    CHECK(sh_ms_set_root(g_root));
    free(run_doc(SH_MODS_ACT_RESCAN, NULL));
}

int main(int argc, char **argv)
{
    char fixture[MAX_PATH], payload[MAX_PATH], tmp[MAX_PATH], src[MAX_PATH], path[MAX_PATH * 2], saves[MAX_PATH * 2];
    char salt1[16] = "", hash1[80] = "", salt_a[16] = "", salt_b[16] = "";
    char *doc;
    const char *dr;

    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp", sizeof fixture, fixture, NULL)) return 2;
    if (!GetFullPathNameA(argc > 2 ? argv[2] : "fixtures\\mods_tab_payload_v4.json", sizeof payload, payload, NULL)) return 2;
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
    sh_mb_set_script_check(fake_script_check);
    note_salt("alexrimp");

    printf("-- an empty store: payload v4, every section present and empty, no migration state\n");
    CHECK(sh_modserve_index_build(g_root) == 0);
    doc = list_doc();
    CHECK(doc != NULL);
    CHECK(has(doc, "{\"v\":4,\"gen\":"));
    CHECK(has(doc, "\"storeReadable\":true"));
    CHECK(has(doc, "\"mods\":[]"));
    CHECK(!has(doc, "\"migrate\""));
    CHECK(has(doc, "\"map\":{\"entities\":-1,\"names\":0,\"rows\":[],\"placed\":[]}"));
    CHECK(has(doc, "\"root\":\"") && has(doc, "store\\\\mods\""));
    free(doc);
    dr = doctor(0);
    CHECK(has(dr, "MODS: doctor: mods folder ") && has(dr, ": 0 mod folder(s), 0 of them downloaded for maps; all listed in the Mods tab"));
    CHECK(!has(dr, "migration"));
    CHECK(has(dr, "MODS: doctor: 0 conflict(s), 0 replaced game file(s), 0 edited copy(ies) of 0 game file(s)"));
    CHECK(has(dr, "MODS: doctor: done\n"));

    printf("-- the main thread only: refused elsewhere, the previous document kept\n");
    g_main = 0;
    CHECK(sh_modui_list() == -1);
    CHECK(sh_modui_run(SH_MODS_ACT_MAP_ADD, "x") == -1);
    CHECK(sh_modui_run(SH_MODS_ACT_UNINSTALL, "x") == -1);
    CHECK(sh_modui_doctor(0, doctor_line, NULL) == -1);
    {
        sh_mods_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODS_LIST;
        CHECK(sh_modui_slot(NULL, &call) == 0);
        call.op = SH_MODS_FETCH;
        CHECK(sh_modui_slot(NULL, &call) == -1 && call.needed > 0);   /* the earlier document remains */
        call.op = 5;                                                   /* the retired WAIT */
        CHECK(sh_modui_slot(NULL, &call) == 0);
    }
    g_main = 1;

    printf("-- Refresh: always says what a change still needs (a restart, the next map open), else nothing\n");
    g_refresh_mode = 1;
    g_refreshes = 0;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(g_refreshes == 1);
    CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"Some of the changes take effect only after you restart DOOM.\""));
    CHECK(has(doc, "\"refresh\":{\"reparsed\":3,\"reloaded\":1,\"needsMapReload\":2,\"needsRestart\":1,"));
    CHECK(!has(doc, "decl(s)") && !has(doc, "\"note\"") && doc_prose_ok(doc));
    free(doc);
    g_refresh_mode = 2;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(has(doc, "\"message\":\"Some of the changes take effect when you are back in the editor.\""));
    free(doc);
    g_refresh_mode = 0;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"\"") && doc_prose_ok(doc));
    free(doc);

    printf("-- off the main thread: the file work runs there with no time limit, the game's part waits for APPLY\n");
    g_main = 0;
    g_refreshes = 0;
    g_refresh_mode = 1;
    doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
    CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"\"") && has(doc, "\"apply\":true"));
    CHECK(g_refreshes == 0 && !has(doc, "\"refresh\""));
    free(doc);
    CHECK(sh_modui_run(SH_MODS_ACT_APPLY, NULL) == -1);   /* the game's part is the main thread's */
    g_main = 1;
    doc = run_doc(SH_MODS_ACT_APPLY, NULL);
    CHECK(g_refreshes == 1);
    CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"Some of the changes take effect only after you restart DOOM.\""));
    CHECK(has(doc, "\"refresh\":{") && !has(doc, "\"apply\""));
    free(doc);
    doc = run_doc(SH_MODS_ACT_APPLY, NULL);   /* nothing owed */
    CHECK(has(doc, "\"action\":\"apply\",\"ok\":true") && g_refreshes == 1);
    free(doc);
    g_refresh_mode = 0;
    {
        sh_modui_engine e;
        memset(&e, 0, sizeof e);
        e.on_main_thread = fake_on_main;
        sh_modui_bind(&e);   /* no rescan bound in a SH_MODUI_PURE build */
        g_refreshes = 0;
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(doc, "\"ok\":false,\"message\":\"The game cannot read the mods folder again from here.\""));
        CHECK(!has(doc, "\"refresh\"") && g_refreshes == 0);
        free(doc);
        bind_fakes();
    }
    /* the retired action numbers (verify, prune, remove, migrate, the recycle bin's) are unknown */
    {
        static const int retired[] = { 2, 5, 6, 7, 8, 9, 14, 15, 17, 19 };
        int r;
        for (r = 0; r < (int)(sizeof retired / sizeof retired[0]); r++) {
            doc = run_doc(retired[r], NULL);
            CHECK(has(doc, "\"ok\":false,\"message\":\"Unknown action.\""));
            free(doc);
        }
    }
    g_rescans = 0;

    printf("-- install through the tab: named by the mod, ready, its rows in the palette\n");
    doc = run_doc(SH_MODS_ACT_INSTALL, "relative\\folder");
    CHECK(has(doc, "\"action\":\"install\",\"ok\":false,\"message\":\"Choose a mod folder or a mod ZIP to install.\""));
    free(doc);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\authoring", tmp);
    {
        char empty[MAX_PATH * 2];
        _snprintf_s(empty, sizeof empty, _TRUNCATE, "%s\\authoring\\empty", tmp);
        CreateDirectoryA(empty, NULL);
        doc = run_doc(SH_MODS_ACT_INSTALL, empty);
        CHECK(has(doc, "\"ok\":false,\"message\":\"That folder holds no mod: a mod folder has a mod.json; choose one, or a "
                       "folder of mod folders.\""));
        free(doc);
    }
    doc = run_doc(SH_MODS_ACT_INSTALL, src);
    if (!has(doc, "\"ok\":true")) printf("    %s\n", doc ? doc : "(null)");
    CHECK(has(doc, "\"action\":\"install\",\"ok\":true,\"message\":\"Installed R2 Imp copy; its Toybox rows are in the palette.\""));
    CHECK(has(doc, "\"key\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"editorRows\":2"));
    CHECK(!has(doc, "\"bundle\"") && !has(doc, "\"id\":") && doc_prose_ok(doc));
    CHECK(g_rescans == 1);
    free(doc);
    doc = list_doc();
    row_salt(doc, "alex.r2-imp", salt1, sizeof salt1);
    CHECK(!strcmp(salt1, "alexrimp"));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"salt\":\"alexrimp\",\"name\":\"R2 Imp copy\",\"author\":\"alex\""));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"state\":\"ready\",\"cause\":\"\",\"editorRows\":2,\"folder\":\"alex.r2-imp\","
                                         "\"group\":\"\",\"place\":\"\",\"placeTitle\":\"\""));
    CHECK(row_has(doc, "alex.r2-imp", "\"editorRowNames\":[") && row_has(doc, "alex.r2-imp", "\"facts\":{\"adds\":"));
    CHECK(row_has(doc, "alex.r2-imp", "\"borrows\":{\"paths\":["));
    CHECK(!row_has(doc, "alex.r2-imp", "\"sameFilesAs\""));
    CHECK(!has(doc, "\"bundle\"") && !has(doc, "\"builtAt\"") && !has(doc, "\"origin\"") && !has(doc, "\"duplicateId\""));
    CHECK(doc_prose_ok(doc));
    union_add(doc);
    free(doc);
    row_build("alex.r2-imp", hash1, sizeof hash1);
    CHECK(strlen(hash1) == 64);
    /* Installing it again: already installed. */
    doc = run_doc(SH_MODS_ACT_INSTALL, src);
    CHECK(has(doc, "\"ok\":true") && (has(doc, "R2 Imp copy is already installed.") || has(doc, "Installed R2 Imp copy")));
    CHECK(doc_prose_ok(doc));
    free(doc);

    printf("-- install a ZIP: its mods are unpacked and installed; a ZIP without a mod or not a ZIP says so\n");
    {
        char zip[MAX_PATH * 2];
        static const char *const names[] = { "zipped/mod.json", "zipped/assets/generated/decls/entitydef/mods/zipped/zipped.decl",
                                             "readme.txt" };
        static const char *const texts[] = { "{\"name\":\"Zipped\",\"author\":\"tester\"}",
                                             "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tzipped = 1;\r\n}\r\n", "hello" };
        _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\authoring\\zipped.zip", tmp);
        CHECK(put_zip(zip, names, texts, 3));
        g_main = 0;   /* the window's worker */
        doc = run_doc(SH_MODS_ACT_INSTALL, zip);
        CHECK(has(doc, "\"action\":\"install\",\"ok\":true,\"message\":\"Installed Zipped.\"") && has(doc, "\"apply\":true"));
        free(doc);
        g_main = 1;
        free(run_doc(SH_MODS_ACT_APPLY, NULL));
        doc = list_doc();
        CHECK(bundle_has(doc, "zipped", "\"state\":\"ready\""));
        {
            char s[16] = "";
            row_salt(doc, "zipped", s, sizeof s);
            note_salt(s);
        }
        free(doc);
        free(run_doc(SH_MODS_ACT_UNINSTALL, "zipped"));
        _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\authoring\\empty.zip", tmp);
        CHECK(put_zip(zip, names + 2, texts + 2, 1));
        doc = run_doc(SH_MODS_ACT_INSTALL, zip);
        CHECK(has(doc, "\"ok\":false,\"message\":\"That ZIP was not installed: it holds no mod: a mod is a folder with a "
                       "mod.json.\""));
        free(doc);
        _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\authoring\\fake.zip", tmp);
        mt_write(tmp, "authoring\\fake.zip", "not a zip");
        doc = run_doc(SH_MODS_ACT_INSTALL, zip);
        CHECK(has(doc, "\"ok\":false,\"message\":\"That ZIP was not installed: it cannot be read as a ZIP"));
        free(doc);
    }

    printf("-- a hand-made folder is a new mod at Refresh; two with the same files carry the hint, both serve\n");
    {
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        static const char BODY[] = "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n}\r\n";
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\twin_a", g_root);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\twin_b", g_root);
        put_fresh_mod(a, "Twin A", "twin_a", BODY);
        put_fresh_mod(b, "Twin B", "twin_b", BODY);
        doc = list_doc();   /* before the scan: not built yet, said plainly */
        CHECK(bundle_has(doc, "twin_a", "\"state\":\"attention\",\"cause\":\"This mod has not been built yet; press Refresh to build it.\""));
        CHECK(bundle_has(doc, "twin_a", "\"name\":\"Twin A\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
        doc = list_doc();
        row_salt(doc, "twin_a", salt_a, sizeof salt_a);
        row_salt(doc, "twin_b", salt_b, sizeof salt_b);
        note_salt(salt_a);
        note_salt(salt_b);
        CHECK(sh_salt_valid(salt_a, strlen(salt_a)) && sh_salt_valid(salt_b, strlen(salt_b)) && strcmp(salt_a, salt_b));
        CHECK(bundle_has(doc, "twin_a", "\"state\":\"ready\"") && bundle_has(doc, "twin_b", "\"state\":\"ready\""));
        CHECK(row_has(doc, "twin_a", "\"sameFilesAs\":{\"name\":\"Twin B\",\"folder\":\"twin_b\",\"count\":1}"));
        CHECK(row_has(doc, "twin_b", "\"sameFilesAs\":{\"name\":\"Twin A\",\"folder\":\"twin_a\",\"count\":1}"));
        CHECK(!row_has(doc, "alex.r2-imp", "\"sameFilesAs\""));
        CHECK(doc_prose_ok(doc));
        union_add(doc);
        free(doc);
        /* Its reference followed the salt; the folder kept its name. */
        _snprintf_s(path, sizeof path, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\twin.decl", salt_a);
        CHECK(mt_has(a, path));
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: mod mods\\twin_a (Twin A): ready\n"));
        CHECK(has(dr, "MODS: doctor: mod mods\\twin_a: carries 1 of the same files as mods\\twin_b; both are mods of their own\n"));
        CHECK(lines_prose_ok(dr));
    }

    printf("-- attention: a failed build, earlier shapes, each cause one plain sentence with the next step\n");
    {
        char bad[MAX_PATH * 2], old[MAX_PATH * 2], pkg[MAX_PATH * 2], cause[600];
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\broken", g_root);
        put_fresh_mod(bad, "Broken", "broken", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n");   /* unbalanced */
        _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\old_shape", g_root);
        CreateDirectoryA(old, NULL);
        mt_write(old, "mod.json", "{\"id\":\"alex.old\",\"name\":\"Old\",\"version\":\"1.0.0\"}");
        mt_write(old, "assets\\generated\\decls\\entitydef\\mods\\alex.old\\x.decl", "{\r\n}\r\n");
        _snprintf_s(pkg, sizeof pkg, _TRUNCATE, "%s\\packaged", g_root);
        CreateDirectoryA(pkg, NULL);
        mt_write(pkg, "package.json", "{\"id\":\"alex.pkg\",\"version\":\"1.0.0\"}");
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "broken", "\"state\":\"attention\""));
        CHECK(row_str(doc, "broken", "cause", cause, sizeof cause) && !strncmp(cause, "This mod could not be built: ", 29));
        if (cause[0]) printf("    broken: %s\n", cause);
        /* An earlier shape: the one sentence, never advice to edit mod.json by hand. */
        CHECK(row_str(doc, "old_shape", "cause", cause, sizeof cause) &&
              !strcmp(cause, "This mod was made for an earlier version of Snapmap+. Run the Snapmap+ installer again and it "
                             "converts it."));
        if (cause[0]) printf("    old_shape: %s\n", cause);
        CHECK(row_str(doc, "packaged", "cause", cause, sizeof cause) &&
              !strcmp(cause, "This mod was made for an earlier version of Snapmap+. Run the Snapmap+ installer again and it "
                             "converts it."));
        {
            /* An earlier version's builder part: the same sentence. */
            char older[MAX_PATH * 2];
            _snprintf_s(older, sizeof older, _TRUNCATE, "%s\\older_block", g_root);
            CreateDirectoryA(older, NULL);
            mt_write(older, "mod.json", "{\"name\":\"Older\",\"snapmap-plus\":{\"format\":1,\"bundle\":\"x\"}}");
            free(run_doc(SH_MODS_ACT_RESCAN, NULL));
            free(doc);
            doc = list_doc();
            CHECK(row_str(doc, "older_block", "cause", cause, sizeof cause) &&
                  !strncmp(cause, "This mod was made for an earlier version of Snapmap+.", 53));
            if (cause[0]) printf("    older_block: %s\n", cause);
            mt_remove_tree(older);
        }
        CHECK(doc_prose_ok(doc));
        union_add(doc);
        free(doc);
        /* The Files view names the cause too, plainly. */
        doc = run_doc(SH_MODS_ACT_FILES, "broken");
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"cause\":\"This mod could not be built: ") && doc_prose_ok(doc));
        free(doc);
        mt_remove_tree(bad);
        mt_remove_tree(old);
        mt_remove_tree(pkg);
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
    }

    printf("-- the plain words: salts, hashes and the internal words never reach a sentence\n");
    {
        CHECK(prose_ok("Install a folder."));
        CHECK(!prose_ok("the bundle 0123456789abcdef0123 is gone"));
    }

    printf("-- a downloaded map's place: its own group, kept as the map needs it\n");
    {
        char built[MAX_PATH * 2], name[400] = "", cause[HO_TEXT] = "";
        sh_ms_install_stats st;
        sh_ms_place place;
        sh_mb_bundle bd;
        _snprintf_s(built, sizeof built, _TRUNCATE, "%s\\authoring\\received", tmp);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp", g_root);
        CHECK(mt_copy_tree(path, built));
        memset(&st, 0, sizeof st);
        memset(&place, 0, sizeof place);
        memset(&bd, 0, sizeof bd);
        place.title = "Hell Arena";
        place.map_id = "6ae40b378bf5037d3d63bd99";
        {
            sh_mb_resolver r;
            memset(&r, 0, sizeof r);
            r.stock = stock_cb;
            CHECK(HO2(name, cause, sh_ms_install_placed(built, &place, &r, HO_P(0), &st, &bd, HO_P(1))));
        }
        if (cause[0]) printf("    placed: %s\n", cause);
        sh_mb_free(&bd);
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
        doc = list_doc();
        CHECK(has(doc, "\"place\":\"maps\\\\Hell Arena\",\"placeTitle\":\"Hell Arena\""));
        {
            char ekey[800];
            size_t q, w = 0;
            for (q = 0; name[q] && w + 2 < sizeof ekey; q++) { if (name[q] == '\\') ekey[w++] = '\\'; ekey[w++] = name[q]; }
            ekey[w] = 0;
            CHECK(!strncmp(name, "maps\\Hell Arena\\", 16));
            CHECK(bundle_has(doc, ekey, "\"state\":\"ready\"") && bundle_has(doc, ekey, "\"group\":\"\""));
        }
        CHECK(doc_prose_ok(doc));
        union_add(doc);
        free(doc);
        /* Use in this map with the map's copy named: the root folder of its salt is attached. */
        open_test_map();
        doc = run_doc(SH_MODS_ACT_MAP_ADD, name);
        CHECK(has(doc, "\"ok\":true,\"message\":\"R2 Imp copy is now used by the whole of "));
        free(doc);
        g_map_on = 0;
        sh_modmanifest_reset();
        /* Add to my mods: the salt is already yours, so that folder is the one. */
        doc = run_doc(SH_MODS_ACT_ADD_MINE, name);
        CHECK(has(doc, "\"action\":\"addToMine\",\"ok\":true,\"message\":\"R2 Imp copy is already one of your mods, in "
                       "mods\\\\alex.r2-imp.\"") && has(doc, "\"key\":\"alex.r2-imp\""));
        free(doc);
        /* The console's uninstall removes one folder, as the tab's does: yours first; the map's copy stays. */
        {
            const char *un_argv[] = { "uninstall", "alex.r2-imp" };
            g_doctor[0] = 0;
            CHECK(sh_modui_command(2, un_argv, doctor_line, NULL) == 1);
            CHECK(has(g_doctor, "MODS: uninstall: moves the folder mods\\alex.r2-imp to the Windows Recycle Bin"));
            CHECK(has(g_doctor, "MODS: uninstall: another folder of R2 Imp copy stays installed (mods\\maps\\Hell Arena\\"));
            CHECK(!has(g_doctor, "folders of"));
            CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp: dry run; add --yes to do it\n"));
            CHECK(lines_prose_ok(g_doctor));
        }
        doc = run_doc(SH_MODS_ACT_UNINSTALL, name);
        CHECK(has(doc, "\"ok\":true,\"message\":\"Uninstalled R2 Imp copy."));
        free(doc);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, name);
        CHECK(!mt_exists(path));
    }

    printf("-- Add to my mods: a downloaded map's mod copied to the root, off the game's thread\n");
    {
        char solo[MAX_PATH * 2], recv[MAX_PATH * 2], name[400] = "", cause[HO_TEXT] = "", key[600], esc[800];
        sh_ms_install_stats st;
        sh_ms_place place;
        sh_mb_bundle bd;
        sh_mb_resolver r;
        size_t q, w = 0;
        _snprintf_s(solo, sizeof solo, _TRUNCATE, "%s\\solo", g_root);
        put_fresh_mod(solo, "Solo", "solo", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tsolo = 1;\r\n}\r\n");
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
        _snprintf_s(recv, sizeof recv, _TRUNCATE, "%s\\authoring\\solo_received", tmp);
        CHECK(mt_copy_tree(solo, recv));
        memset(&st, 0, sizeof st);
        memset(&place, 0, sizeof place);
        memset(&bd, 0, sizeof bd);
        memset(&r, 0, sizeof r);
        r.stock = stock_cb;
        place.title = "Solo Arena";
        place.map_id = "6ae40b378bf5037d3d63bd77";
        CHECK(HO2(name, cause, sh_ms_install_placed(recv, &place, &r, HO_P(0), &st, &bd, HO_P(1))));
        sh_mb_free(&bd);
        CHECK(HO(cause, sh_ms_delete_mod_folder(solo, HO_P(0))) != 0);
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
        for (q = 0; name[q] && w + 2 < sizeof esc; q++) { if (name[q] == '\\') esc[w++] = '\\'; esc[w++] = name[q]; }
        esc[w] = 0;
        /* Only the map's copy: Use in this map sends the player to Add to my mods, which is there. */
        open_test_map();
        doc = run_doc(SH_MODS_ACT_MAP_ADD, name);
        CHECK(has(doc, "\"ok\":false,\"message\":\"Solo came with the map Solo Arena. Add it to your mods first (Add to my "
                       "mods); then your own maps can use it.\""));
        free(doc);
        g_main = 0;
        g_refreshes = 0;
        doc = run_doc(SH_MODS_ACT_ADD_MINE, name);
        CHECK(has(doc, "\"action\":\"addToMine\",\"ok\":true,\"message\":\"Added Solo to your mods, in mods\\\\Solo; your own "
                       "maps can use it now.\"") && has(doc, "\"apply\":true") && g_refreshes == 0);
        free(doc);
        g_main = 1;
        doc = run_doc(SH_MODS_ACT_APPLY, NULL);
        CHECK(has(doc, "\"action\":\"addToMine\",\"ok\":true") && has(doc, "\"key\":\"Solo\"") && g_refreshes == 1);
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "Solo", "\"state\":\"ready\"") && bundle_has(doc, "Solo", "\"place\":\"\""));
        CHECK(bundle_has(doc, esc, "\"state\":\"ready\""));   /* the map's copy stays for its map */
        {
            char a[16] = "", b[16] = "";
            row_salt(doc, "Solo", a, sizeof a);
            row_salt(doc, esc, b, sizeof b);
            CHECK(a[0] && !strcmp(a, b));   /* one mod: the same salt at the root and in the place */
            note_salt(a);
        }
        CHECK(doc_prose_ok(doc));
        free(doc);
        /* Now the map's copy names your own folder, which the map uses. */
        doc = run_doc(SH_MODS_ACT_MAP_ADD, name);
        CHECK(has(doc, "\"ok\":true,\"message\":\"Solo is now used by the whole of "));
        free(doc);
        g_map_on = 0;
        sh_modmanifest_reset();
        /* Your own mod: nothing to add. */
        doc = run_doc(SH_MODS_ACT_ADD_MINE, "Solo");
        CHECK(has(doc, "\"ok\":true,\"message\":\"Solo is already one of your mods.\""));
        free(doc);
        strcpy_s(key, sizeof key, name);
        free(run_doc(SH_MODS_ACT_UNINSTALL, key));
        free(run_doc(SH_MODS_ACT_UNINSTALL, "Solo"));
        mt_remove_tree(recv);
    }

    printf("-- the open map: rows by salt, the asset rule's states, plain causes\n");
    load_map_naming("alexrimp", hash1, "R2 Imp copy", NULL);
    CHECK(g_activations >= 1);
    g_placed[0] = "mods/alexrimp/imp_copy";
    g_placed[1] = "snapmaps/visblockers/industrial/cap_01";
    g_placed_count = 2;
    doc = list_doc();
    CHECK(has(doc, "\"map\":{\"entities\":2,\"names\":2,"));
    CHECK(has(doc, "\"rows\":[{\"salt\":\"alexrimp\",\"name\":\"R2 Imp copy\""));
    CHECK(has(doc, "\"state\":\"active\",\"cause\":\"\",\"key\":\"alex.r2-imp\""));
    CHECK(has(doc, "\"placed\":[{\"salt\":\"alexrimp\""));
    CHECK(doc_prose_ok(doc));
    free(doc);
    load_map_naming("zzothers", "00000000000000000000000000000000000000000000000000000000000000bb", "Hell Guards", "mods/zzothers/x");
    doc = list_doc();
    CHECK(has(doc, "\"state\":\"missing\",\"cause\":\"This map uses Hell Guards, which is not installed.\",\"key\":\"\""));
    CHECK(doc_prose_ok(doc));
    free(doc);
    sh_modmanifest_reset();
    g_placed_count = -1;

    printf("-- Use in this map, Remove from map, Uninstall: dry runs drive the confirmations\n");
    _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\saves", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\76561198000000000\\SNAPMAPSABCDEF0123456789ABCD", saves);
    mt_write(path, "game.details", "checksum=1\r\nitemId=6ae40b378bf5037d3d63bd23\r\nmetadata={\"localId\":\"ABCDEF0123456789ABCD\","
                                   "\"name\":\"Imp Arena\",\"lastModified\":1793364363900,\"tags\":[\"mods\"]}\r\nmodified=1793364363900\r\n");
    sh_modui_test_set_saves_root(saves);
    {
        char claims[400];
        _snprintf_s(claims, sizeof claims, _TRUNCATE,
                    "{\"item123\":{\"bundles\":[\"%s\"],\"claim\":\"c\",\"secret\":\"s\",\"updated\":1793164663}}", hash1);
        CHECK(sh_mb_record_put(g_root, "claims", claims));
        CHECK(sh_mb_record_put(g_root, "links", "{\"item123\":{\"cloud\":\"6ae40b378bf5037d3d63bd23\","
                                                "\"local\":\"ABCDEF0123456789ABCD\",\"at\":1793164563}}"));
    }
    {
        /* One saved map carries a row of the imp mod. */
        sh_modmanifest_list rows;
        sh_modmanifest_row row;
        char err[256], *text;
        size_t tl = 0;
        memset(&rows, 0, sizeof rows);
        memset(&row, 0, sizeof row);
        strcpy_s(row.salt, sizeof row.salt, "alexrimp");
        strcpy_s(row.build, sizeof row.build, hash1);
        sh_modmanifest_row_set_name(&row, "R2 Imp copy");
        CHECK(sh_modmanifest_list_add(&rows, &row));
        text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
        CHECK(text != NULL);
        if (text) { put_saved_decl(path, text, tl); HeapFree(GetProcessHeap(), 0, text); }
        sh_modmanifest_list_free(&rows);
    }
    open_test_map();
    g_tagged = 1;
    load_map_naming("alexrimp", hash1, "R2 Imp copy", NULL);
    doc = list_doc();
    CHECK(has(doc, "\"name\":\"Imp Arena\",\"saved\":"));
    CHECK(has(doc, "\"published\":{\"state\":\"published\",\"at\":1793164563},\"uploaded\":{\"claimed\":true,"));
    CHECK(bundle_has(doc, "alex.r2-imp", "\"state\":\"ready\"") && row_has(doc, "alex.r2-imp", "\"publishedMaps\":1"));
    CHECK(has(doc, "\"mods\":[{\"key\":\"alex.r2-imp\",\"salt\":\"alexrimp\",\"name\":\"R2 Imp copy\","
                   "\"state\":\"installed\",\"placedEntities\":["));
    CHECK(has(doc, "\"placedCount\":2,"));
    CHECK(has(doc, "\"vanilla\":false"));
    CHECK(doc_prose_ok(doc));
    union_add(doc);
    free(doc);
    /* Use in this map: twin_a, nothing placed. */
    g_marks = 0;
    doc = run_doc(SH_MODS_ACT_MAP_ADD, "twin_a");
    CHECK(has(doc, "\"ok\":true,\"message\":\"Twin A is now used by the whole of Imp Arena; save the map to keep it.\""));
    CHECK(g_marks == 1 && doc_prose_ok(doc));
    free(doc);
    doc = run_doc(SH_MODS_ACT_MAP_ADD, "twin_a");
    CHECK(has(doc, "\"ok\":true,\"message\":\"Twin A is already used by the whole of Imp Arena.\""));
    free(doc);
    doc = list_doc();
    CHECK(has(doc, "\"key\":\"twin_a\",\"salt\":\"") && has(doc, "\"attached\":true,\"nothingPlaced\":true"));
    union_add(doc);
    free(doc);
    doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, "twin_a");
    CHECK(has(doc, "\"preview\":{\"action\":\"mapRemoveMod\",\"key\":\"twin_a\",\"name\":\"Twin A\""));
    CHECK(has(doc, "\"entityCount\":0,") && has(doc, "\"manifestRow\":true"));
    free(doc);
    doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "twin_a");
    CHECK(has(doc, "\"ok\":true,\"message\":\"Removed Twin A from Imp Arena; nothing placed used it.\""));
    free(doc);
    /* Remove from map never leaves objects without a row: the row goes once the delete has committed and
     * the map holds none of the mod's objects; objects still there keep it. */
    g_delete_keeps = 1;
    g_undo_pending = 1;
    log_clear();
    doc = run_doc(SH_MODS_ACT_MAP_REMOVE, "alex.r2-imp");
    CHECK(has(doc, "\"ok\":true") && current_has("alexrimp"));
    free(doc);
    CHECK(sh_modui_poll_due() == 1);
    (void)sh_modui_poll_main(NULL);
    CHECK(current_has("alexrimp"));                 /* not committed yet: nothing dropped */
    g_undo_pending = 0;
    (void)sh_modui_poll_main(NULL);
    CHECK(current_has("alexrimp") && has(g_logbuf, "of its objects are still in the map, so its row stays"));
    CHECK(ent_alive(1) && ent_alive(2) && g_tagged == 1);
    g_delete_keeps = 0;
    g_deleted_n = 0;
    /* The imp mod: the dry run of Remove from map and of Uninstall. */
    doc = run_doc(SH_MODS_ACT_MAP_REMOVE_PREVIEW, "alex.r2-imp");
    CHECK(has(doc, "\"entityCount\":2,") && has(doc, "\"tagRemoved\":true,\"vanillaAfter\":true"));
    free(doc);
    doc = run_doc(SH_MODS_ACT_UNINSTALL_PREVIEW, "alex.r2-imp");
    CHECK(has(doc, "\"install\":{\"folder\":\"alex.r2-imp\",\"disposition\":\"recycle\",\"savedMaps\":1,\"undoCleared\":true}"));
    CHECK(has(doc, "\"editorRows\":[") && doc_prose_ok(doc));
    free(doc);
    g_undo_available = 0;
    doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.r2-imp");
    CHECK(has(doc, "\"ok\":false,\"message\":\"R2 Imp copy was not uninstalled: the editor's undo history cannot be "
                   "cleared on this build. Close the map, then uninstall it.\""));
    free(doc);
    g_undo_available = 1;
    g_delete_status = SH_EDITOR_DELETE_BUSY;
    doc = run_doc(SH_MODS_ACT_UNINSTALL, "alex.r2-imp");
    CHECK(has(doc, "\"ok\":false,\"message\":\"R2 Imp copy was not uninstalled: the editor is busy"));
    free(doc);
    g_delete_status = SH_EDITOR_DELETE_DISPATCHED;
    CHECK(ent_alive(1) && ent_alive(2));

    printf("-- the console verbs share the dry runs and actions in the same words; --yes performs them\n");
    {
        const char *map_argv[] = { "map" };
        const char *rm_argv[] = { "map", "remove", "alex.r2-imp" };
        const char *un_yes[] = { "uninstall", "alex.r2-imp", "--yes" };
        const char *bad[] = { "map", "frob" };
        g_doctor[0] = 0;
        CHECK(sh_modui_command(1, map_argv, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: map: Imp Arena, 5 entities, last saved "));
        CHECK(has(g_doctor, "MODS: map: mod R2 Imp copy (mods\\alex.r2-imp, ready): 2 placed entities, 3 wire or "
                            "timeline reference(s)\n"));
        CHECK(has(g_doctor, "MODS: map:   entity Gate imp #2\n"));
        CHECK(lines_prose_ok(g_doctor));
        g_doctor[0] = 0;
        CHECK(sh_modui_command(3, rm_argv, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: map remove: deletes 2 placed entities from Imp Arena (one undo step)\n"));
        CHECK(has(g_doctor, "MODS: map remove alex.r2-imp: dry run; add --yes to do it\n"));
        CHECK(lines_prose_ok(g_doctor));
        g_doctor[0] = 0;
        CHECK(sh_modui_command(2, bad, doctor_line, NULL) == 1 && has(g_doctor, "usage: sh_mods map"));
        g_doctor[0] = 0;
        g_deleted_n = 0;
        g_undo_pending = 1;
        CHECK(sh_modui_command(3, un_yes, doctor_line, NULL) == 1);
        CHECK(has(g_doctor, "MODS: uninstall alex.r2-imp: Uninstalled R2 Imp copy and removed it from the open map."));
        CHECK(lines_prose_ok(g_doctor));
        CHECK(g_deleted_n == 2 && !ent_alive(1) && !ent_alive(2));
        CHECK(current_has("alexrimp") && g_tagged == 1);   /* the delete has not committed: the row and the tag stay */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp", g_root);
        CHECK(!mt_exists(path));
        /* The row, the tag and the undo history go once the delete committed. */
        g_undo_clears = 0;
        g_undo_pending = 0;
        CHECK(sh_modui_poll_due() == 1);
        (void)sh_modui_poll_main(NULL);
        CHECK(g_undo_clears == 1 && !current_has("alexrimp") && g_tagged == 0);
    }
    g_map_on = 0;
    sh_modmanifest_reset();
    sh_modui_test_set_saves_root(NULL);

    printf("-- the Files view: the folder's files, mod.json without the part Snapmap+ writes\n");
    {
        char arg[200];
        doc = run_doc(SH_MODS_ACT_FILES, "twin_a");
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"key\":\"twin_a\",\"name\":\"Twin A\",\"cause\":\"\""));
        CHECK(has(doc, "\"path\":\"mod.json\"") && has(doc, "\"class\":\"descriptor\""));
        _snprintf_s(path, sizeof path, _TRUNCATE, "\"path\":\"assets/generated/decls/entitydef/mods/%s/twin.decl\"", salt_a);
        CHECK(has(doc, path));   /* the salt appears in the Files view's paths */
        free(doc);
        _snprintf_s(arg, sizeof arg, _TRUNCATE, "twin_a\tmod.json");
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, arg);
        CHECK(has(doc, "\"ok\":true") && has(doc, "\"productBlock\":true") && !has(doc, "snapmap-plus"));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, "twin_a\t..\\x.json");
        CHECK(has(doc, "\"ok\":false,\"message\":\"That file is not inside the mod's folder.\""));
        free(doc);
    }

    printf("-- New mod: a folder named after it, built at once, and Refresh named as the way to pick up edits\n");
    {
        char want[600];
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "Crate Pack");
        if (!has(doc, "\"ok\":true")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(has(doc, "\"ok\":true,\"message\":\"Created Crate Pack in mods\\\\Crate Pack. Its Example crate is in the Toybox. "
                       "After you change its files, press Refresh to see the change.\""));
        CHECK(has(doc, "\"key\":\"Crate Pack\""));
        CHECK(has(doc, "\"file\":\"assets/generated/decls/snapeditorentitydef/mods/"));
        CHECK(doc_prose_ok(doc));
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "Crate Pack", "\"name\":\"Crate Pack\"") && bundle_has(doc, "Crate Pack", "\"state\":\"ready\""));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"editorRowNames\":[\"Example crate\"]");
        CHECK(row_has(doc, "Crate Pack", want));
        {
            char s[16];
            row_salt(doc, "Crate Pack", s, sizeof s);
            note_salt(s);
        }
        union_add(doc);
        free(doc);
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "   ");
        CHECK(has(doc, "\"ok\":false,\"message\":\"No mod was created: "));
        free(doc);
        /* Off the main thread (the window's worker): made and built there, finished by APPLY. */
        g_main = 0;
        doc = run_doc(SH_MODS_ACT_NEW_MOD, "Later");
        CHECK(has(doc, "\"ok\":true,\"message\":\"Created Later in mods\\\\Later.") && has(doc, "\"apply\":true"));
        free(doc);
        g_main = 1;
        doc = run_doc(SH_MODS_ACT_APPLY, NULL);
        CHECK(has(doc, "\"action\":\"newMod\",\"ok\":true,\"message\":\"Created Later in mods\\\\Later.") &&
              has(doc, "\"key\":\"Later\"") && !has(doc, "\"apply\""));
        free(doc);
    }


    printf("-- an organizing folder: its mods name it as their group\n");
    {
        char grp[MAX_PATH * 2];
        _snprintf_s(grp, sizeof grp, _TRUNCATE, "%s\\Packs\\twin_c", g_root);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Packs", g_root);
        CreateDirectoryA(path, NULL);
        put_fresh_mod(grp, "Twin C", "twin_c", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tsize = 2;\r\n}\r\n");
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
        doc = list_doc();
        CHECK(bundle_has(doc, "Packs\\\\twin_c", "\"folder\":\"twin_c\",\"group\":\"Packs\""));
        union_add(doc);
        free(doc);
    }

    printf("-- an unnamed mod is named by its folder, never by its salt\n");
    {
        char nn[MAX_PATH * 2], s[16] = "";
        _snprintf_s(nn, sizeof nn, _TRUNCATE, "%s\\noname", g_root);
        CreateDirectoryA(nn, NULL);
        mt_write(nn, "mod.json", "{\"name\":\"\"}");
        mt_write(nn, "assets\\generated\\decls\\entitydef\\mods\\noname\\nn.decl", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n}\r\n");
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
        doc = list_doc();
        row_salt(doc, "noname", s, sizeof s);
        note_salt(s);
        free(doc);
        dr = doctor(0);
        CHECK(has(dr, "MODS: doctor: mod mods\\noname (noname)"));
        CHECK(lines_prose_ok(dr));
        mt_remove_tree(nn);
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
    }

    printf("-- the doctor: every folder, plainly\n");
    dr = doctor(1);
    CHECK(has(dr, "MODS: doctor: mod mods\\twin_b (Twin B): ready\n"));
    {
        /* A mod with console settings and commands: the document carries them as the builder wrote them. */
        char req[MAX_PATH * 2];
        _snprintf_s(req, sizeof req, _TRUNCATE, "%s\\gates", g_root);
        put_fresh_mod(req, "Gates", "gates", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tgates = 1;\r\n}\r\n");
        mt_write(req, "mod.json", "{\"name\":\"Gates\",\"version\":\"0.3.0\",\"requirements\":{\"cvars\":"
                                  "{\"g_useImageBlackList\":\"0\"},\"commands\":{\"activate\":[\"echo gates on\"],"
                                  "\"deactivate\":[\"echo gates off\"]}}}");
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        free(doc);
        doc = list_doc();
        CHECK(row_has(doc, "gates", "\"requirements\":{\"commands\":{\"activate\":[\"echo gates on\"],\"deactivate\":"
                                    "[\"echo gates off\"]},\"cvars\":{\"g_useImageBlackList\":\"0\"}}"));
        union_add(doc);
        free(doc);
    }
    CHECK(has(dr, "MODS: doctor: verify: "));
    CHECK(lines_prose_ok(dr));

    printf("-- mod scripts: what they drive in the row, their text and errors in the Files view, their log and state\n");
    {
        char sm[MAX_PATH * 2], s[16] = "", lua[400], *p;
        static const char DOOR[] =
            "api = 1\r\nlocal on_count = 0\r\non.input(\"switch_used\", function(switch_node, activator)\r\n"
            "  on_count = on_count + 1\r\n  if on_count == 3 then node(\"open_door\"):fire(activator) end\r\nend)\r\n"
            "on.think(\"pickups/spin\", function(self, dt) self:set_yaw(self:yaw() + 90 * dt) end)\r\n"
            "on.spawn(\"pickups/spin\", function(self) end)\r\n"
            "local which = \"pickups/spin\"\r\non.damage(which, function(self, hit) end)\r\n";   /* a name set while running */
        _snprintf_s(sm, sizeof sm, _TRUNCATE, "%s\\doorlogic", g_root);
        put_fresh_mod(sm, "Door Logic", "doorlogic", "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tdoor = 1;\r\n}\r\n");
        mt_write(sm, "assets\\generated\\decls\\entitydef\\mods\\doorlogic\\pickups\\spin.decl",
                 "{\r\n\tinherit = \"ai/demon/imp_coop\";\r\n\tspin = 1;\r\n}\r\n");
        mt_write(sm, "assets\\mods\\doorlogic\\scripts\\door.lua", DOOR);
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
        doc = list_doc();
        row_salt(doc, "doorlogic", s, sizeof s);
        note_salt(s);
        if (!row_has(doc, "doorlogic", "\"scripts\":{")) printf("    %s\n", doc ? doc : "(null)");
        /* the block's member as built: the static pass's names, no salt */
        CHECK(row_has(doc, "doorlogic", "\"state\":\"ready\""));
        CHECK(row_has(doc, "doorlogic", "\"scripts\":{\"dynamic\":1,\"entities\":[{\"events\":[\"spawn\",\"think\"],"
                                        "\"name\":\"pickups/spin\"}],\"files\":1,\"inputs\":[\"switch_used\"],\"node\":true,\"outputs\":[\"open_door\"]}"));
        CHECK(!row_has(doc, "doorlogic", "\"scriptRun\""));   /* no Play has run them */
        CHECK(!row_has(doc, "doorlogic", "\"restartBanks\":[\""));
        CHECK(doc_prose_ok(doc));
        free(doc);

        /* The Files view: the script is text, its path under the salt; its text reads. */
        doc = run_doc(SH_MODS_ACT_FILES, "doorlogic");
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "\"path\":\"assets/mods/%s/scripts/door.lua\",", s);
        p = doc ? strstr(doc, lua) : NULL;
        CHECK(p != NULL);
        CHECK(p && !strncmp(strstr(p, "\"kind\":"), "\"kind\":\"text\"", 13));
        free(doc);
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "doorlogic\tassets/mods/%s/scripts/door.lua", s);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, lua);
        CHECK(has(doc, "\"ok\":true") && has(doc, "on.input(\\\"switch_used\\\"") && has(doc, "\"cause\":\"\""));
        free(doc);

        /* A syntax error: the row's cause is the script's own sentence with file and line, and the
         * Files view puts it on that file. */
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "assets\\mods\\%s\\scripts\\door.lua", s);
        mt_write(sm, lua, "api = 1\r\non.input(\"switch_used\", function()\r\n  BROKEN\r\n");
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
        doc = list_doc();
        if (!row_has(doc, "doorlogic", "\"cause\":\"scripts\\\\door.lua line 4:")) printf("    %s\n", doc ? doc : "(null)");
        CHECK(row_has(doc, "doorlogic", "\"state\":\"attention\",\"cause\":\"scripts\\\\door.lua line 4: \\\"end\\\" expected to "
                                        "close \\\"function\\\" on line 2. Correct it, then press Refresh.\""));
        CHECK(doc_prose_ok(doc));
        free(doc);
        doc = run_doc(SH_MODS_ACT_FILES, "doorlogic");
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "\"path\":\"assets/mods/%s/scripts/door.lua\",", s);
        p = doc ? strstr(doc, lua) : NULL;
        CHECK(p && strstr(p, "\"cause\":\"scripts\\\\door.lua line 4: ") && strstr(p, "\"cause\":\"scripts") < strstr(p, "}"));
        free(doc);
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "doorlogic\tassets/mods/%s/scripts/door.lua", s);
        doc = run_doc(SH_MODS_ACT_FILE_TEXT, lua);
        CHECK(has(doc, "\"cause\":\"scripts\\\\door.lua line 4: "));
        free(doc);
        _snprintf_s(lua, sizeof lua, _TRUNCATE, "assets\\mods\\%s\\scripts\\door.lua", s);
        mt_write(sm, lua, DOOR);
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));

        /* A Play stopped the scripts: the row carries the stop line until the next Play. */
        g_script_on = 1;
        strcpy_s(g_script_salt, sizeof g_script_salt, s);
        memset(&g_script, 0, sizeof g_script);
        g_script.status = SH_MODUI_SCRIPT_STOPPED;
        g_script.lines = 3;
        g_script.stop = "Script stopped: Door Logic, scripts\\door.lua line 5: \"open_door\" is "
                        "nothing here. Check the name you pass.";
        doc = list_doc();
        CHECK(row_has(doc, "doorlogic", "\"scriptRun\":{\"status\":\"stopped\",\"stop\":\"Script stopped: Door Logic, "
                                        "scripts\\\\door.lua line 5: \\\"open_door\\\" is nothing here. Check the name you pass.\","
                                        "\"lines\":3}"));
        CHECK(row_has(doc, "doorlogic", "\"state\":\"ready\""));   /* still usable: the stop is the Play's */
        CHECK(doc_prose_ok(doc));
        union_add(doc);
        free(doc);
        g_script.status = SH_MODUI_SCRIPT_RUNNING;
        doc = list_doc();
        CHECK(row_has(doc, "doorlogic", "\"scriptRun\":{\"status\":\"running\",\"stop\":\"\",\"lines\":3}"));
        free(doc);

        /* The script log op: any thread, its own buffer, a size query first. */
        {
            sh_mods_call call;
            char *out;
            g_script.status = SH_MODUI_SCRIPT_STOPPED;
            g_main = 0;
            memset(&call, 0, sizeof call);
            call.op = SH_MODS_SCRIPTS;
            call.arg = s;
            CHECK(sh_modui_slot(NULL, &call) == -1 && call.needed > 0);
            out = (char *)malloc((size_t)call.needed + 1);
            call.out = out;
            call.out_cap = call.needed + 1;
            CHECK(out && sh_modui_slot(NULL, &call) == 1);
            _snprintf_s(lua, sizeof lua, _TRUNCATE, "{\"mods\":[{\"salt\":\"%s\",\"status\":\"stopped\",\"stop\":\"Script stopped: Door "
                        "Logic, scripts\\\\door.lua line 5:", s);
            CHECK(out && !strncmp(out, lua, strlen(lua)));
            CHECK(has(out, "\"log\":[\"door: switch 1 of 3\",\"a line with \\\"quotes\\\" and a \\\\ backslash\","
                           "\"Script stopped: Door Logic,"));
            free(out);
            /* no salt: every mod's state, no log; a salt the runtime does not know: an empty log */
            memset(&call, 0, sizeof call);
            call.op = SH_MODS_SCRIPTS;
            call.arg = "";
            CHECK(sh_modui_slot(NULL, &call) == -1);
            out = (char *)malloc((size_t)call.needed + 1);
            call.out = out;
            call.out_cap = call.needed + 1;
            CHECK(out && sh_modui_slot(NULL, &call) == 1 && has(out, "\"salt\":\"\",\"total\":0,\"full\":false,\"log\":[]}"));
            free(out);
            /* the tail the window shows, and the whole log Copy log asks for */
            {
                char full[64];
                memset(&call, 0, sizeof call);
                call.op = SH_MODS_SCRIPTS;
                call.arg = s;
                CHECK(sh_modui_slot(NULL, &call) == -1);
                out = (char *)malloc((size_t)call.needed + 1);
                call.out = out;
                call.out_cap = call.needed + 1;
                CHECK(out && sh_modui_slot(NULL, &call) == 1 && has(out, "\"total\":3,\"full\":false,\"log\":["));
                free(out);
                _snprintf_s(full, sizeof full, _TRUNCATE, "%s%s", SH_MODS_SCRIPTS_FULL, s);
                memset(&call, 0, sizeof call);
                call.op = SH_MODS_SCRIPTS;
                call.arg = full;
                CHECK(sh_modui_slot(NULL, &call) == -1);
                out = (char *)malloc((size_t)call.needed + 1);
                call.out = out;
                call.out_cap = call.needed + 1;
                CHECK(out && sh_modui_slot(NULL, &call) == 1 && has(out, "\"total\":3,\"full\":true,\"log\":[\"door: switch 1 of 3\""));
                free(out);
            }
            g_script_on = 0;
            {
                char tiny[256];
                memset(&call, 0, sizeof call);
                call.op = SH_MODS_SCRIPTS;
                call.arg = s;
                call.out = tiny;
                call.out_cap = (int)sizeof tiny;
                CHECK(sh_modui_slot(NULL, &call) == 1 && has(tiny, "{\"mods\":[],") && has(tiny, "\"log\":[]}"));
            }
            g_main = 1;
        }
        doc = list_doc();
        CHECK(!row_has(doc, "doorlogic", "\"scriptRun\""));
        free(doc);

        /* A sound bank the game loaded before a change: the row says it, and the Refresh that brought
         * the change says it once. */
        strcpy_s(g_bank, sizeof g_bank, "doorlogic_sfx.bnk");
        strcpy_s(g_bank_owner, sizeof g_bank_owner, s);
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"Restart DOOM to hear the new doorlogic_sfx.bnk.\""));
        free(doc);
        doc = run_doc(SH_MODS_ACT_RESCAN, NULL);
        CHECK(has(doc, "\"action\":\"rescan\",\"ok\":true,\"message\":\"\""));   /* already said */
        free(doc);
        doc = list_doc();
        CHECK(row_has(doc, "doorlogic", "\"restartBanks\":[\"doorlogic_sfx.bnk\"]"));
        CHECK(row_has(doc, "gates", "\"restartBanks\":[]"));   /* another mod's bank is not its line */
        union_add(doc);
        free(doc);
        g_bank[0] = 0;

        /* The download approval's sentence for scripted mods. */
        sh_modui_scripts_approval(2, lua, sizeof lua);
        CHECK(!strcmp(lua, "These mods include scripts (2 files). Scripts run only inside this map and cannot read or change "
                           "your files."));
        sh_modui_scripts_approval(1, lua, sizeof lua);
        CHECK(has(lua, "(1 file)."));
        CHECK(prose_ok(SH_MODUI_SCRIPTS_OFF));
        mt_remove_tree(sm);
        free(run_doc(SH_MODS_ACT_RESCAN, NULL));
    }

    test_places_stay(tmp);

    printf("-- the page's fixture keys are the backend's\n");
    keys_match_fixture(payload, g_union ? g_union : "");
    free(g_union);
    g_union = NULL;

    printf("modui_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
