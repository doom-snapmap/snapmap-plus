/* mods_inmap_test.c -- the Mods tab's "In this map" against the save: the document's map.mods names exactly the mods a
 * save of the open map writes, at every step an author takes (docs/webview-ui.md, "A mod and the open map"). Each
 * step's document and save rows go to <out> and are compared with <fixture>, which mods_inmap_test.js runs through
 * the page's rule.
 *
 *   mods_inmap_test.exe <tests\fixtures\mods\alex.r2-imp> <tests\fixtures\mods_inmap_cases.json> <out.json>
 */
#include "cache_paths.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snapmap_plus_iface.h"
#include "modui.h"
#include "saved_maps.h"
#include "modstore.h"
#include "modserve.h"
#include "modmanifest.h"
#include "mod_test_util.h"
#include "editor_delete.h"
#include "mp_json.h"
#include "text.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static int g_echo;
void backend_log(const char *message) { if (g_echo && message) printf("    %s\n", message); }

/* The serving layer's engine half (as modui_test). */
#include "modcache.h"
#include "modpin.h"
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

/* ---- the fake editor ------------------------------------------------------------------------- */

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
    "generated/decls/entitydef/player/mp.decl",
    "generated/decls/snapeditorentitydef/prop/prop_base.decl",
    "generated/decls/entitydef/prop/prop_base.decl",
    NULL
};
static int fake_stock(const char *request)
{
    int i;
    for (i = 0; STOCK[i]; i++) if (!strcmp(STOCK[i], request)) return 1;
    return 0;
}

static char g_root[MAX_PATH];
static int fake_rescan(void) { return sh_modserve_index_build(g_root); }
static int fake_on_main(void) { return 1; }
static int fake_palette(void) { return 1; }
static int fake_refresh(sh_modserve_refresh *out) { if (out) memset(out, 0, sizeof *out); return 0; }

/* The open map: entities (an inherit each, alive or deleted) and module instances. */
typedef struct fake_ent { int uid; const char *inherit; int alive; } fake_ent;
static fake_ent g_ents[16];
static int g_ent_n, g_map_on;
static int g_placement_failed;
static const char *g_module;   /* one module instance's moduleName, or NULL for a stock room */

static char *map_text(size_t *len)
{
    static char t[8192];
    size_t at = 0;
    int i, first = 1;
#define PUT(...) at += (size_t)_snprintf_s(t + at, sizeof t - at, _TRUNCATE, __VA_ARGS__)
    PUT("{\"entities\":[");
    for (i = 0; i < g_ent_n; i++) {
        if (!g_ents[i].alive) continue;
        PUT("%s{\"displayName\":\"\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"%s\",\"name\":\"\","
            "\"state\":{\"edit\":{\"spawnPosition\":{\"x\":1}}}},\"uniqueId\":%d}", first ? "" : ",", g_ents[i].inherit,
            g_ents[i].uid);
        first = 0;
    }
    PUT("],\"instances\":[{\"moduleName\":\"%s\"}],\"targets\":{\"connections\":[]},"
        "\"references\":{\"entityEntRefs\":{\"keyValues\":[0],\"values\":[]}},"
        "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}", g_module ? g_module : "maps/modules/stock/room_a");
#undef PUT
    *len = at;
    return t;
}
static int fake_map_json(char **out, size_t *len)
{
    char *t, *copy;
    if (!g_map_on) return 0;
    t = map_text(len);
    copy = (char *)HeapAlloc(GetProcessHeap(), 0, *len + 1);
    if (!copy) return -1;
    memcpy(copy, t, *len + 1);
    *out = copy;
    return 1;
}
/* As the engine's walk: the quoted names of every live entity's decl (here, its inherit). */
static int fake_placed(sh_modui_name_fn emit, void *context)
{
    int i, n = 0;
    if (!g_map_on || g_placement_failed) return -1;
    if (g_module) emit(context, g_module);
    for (i = 0; i < g_ent_n; i++) if (g_ents[i].alive) { emit(context, g_ents[i].inherit); n++; }
    return n;
}
static int fake_map_info(sh_modui_map_info *out)
{
    if (!g_map_on) return 0;
    memset(out, 0, sizeof *out);
    strcpy_s(out->local_id, sizeof out->local_id, "ABCDEF0123456789ABCD");
    out->tagged = 1;
    return 1;
}
static int fake_delete(const int *ids, int count)
{
    int i, k;
    for (i = 0; i < count; i++) for (k = 0; k < g_ent_n; k++) if (g_ents[k].uid == ids[i]) g_ents[k].alive = 0;
    return SH_EDITOR_DELETE_DISPATCHED;
}
static int fake_remove_tag(const char *tag) { (void)tag; return 1; }
static int fake_busy(char *cause, size_t cap) { (void)cause; (void)cap; return 0; }
static void fake_toast(const char *title, const char *text) { (void)title; (void)text; }
static void fake_print(const char *line) { (void)line; }
static int g_image = 1, g_resource = 1, g_scope_changes;
static int *test_cvar(const char *s)
{
    if (!_stricmp(s, "g_useImageBlackList")) return &g_image;
    if (!_stricmp(s, "g_useResourceBlackList")) return &g_resource;
    return NULL;
}
static int cvar_read(void *ctx, const char *s, char **value)
{
    int *v = test_cvar(s);
    (void)ctx;
    *value = v ? _strdup(*v ? "1" : "0") : NULL;
    return *value != NULL;
}
static int cvar_set(void *ctx, const char *s, const char *value)
{
    int *v = test_cvar(s);
    (void)ctx;
    if (!v) return 0;
    *v = atoi(value);
    return SH_MODSERVE_GATE_SET;
}
static int fake_scope_apply(const char *phase)
{
    sh_modserve_gate_ops ops = { NULL, cvar_read, cvar_set, NULL, NULL };
    sh_modserve_gates result;
    g_scope_changes++;
    return sh_modserve_requirements_apply(&ops, phase, &result);
}
static int fake_map_left(void) { return 1; }
static int fake_clear_undo(int probe) { (void)probe; return g_map_on ? 1 : 0; }
static int fake_undo_pending(void) { return 0; }
static int fake_mark_modified(const char *label) { (void)label; return g_map_on ? 1 : 0; }
static int fake_preload(const char *why) { (void)why; return 0; }

/* The publish's pin step reads the author's folder and the served bytes, and names DOOM's files by a fixed hash (as
 * parity_test). */
static char *author_folder(void *ctx, const sh_modmanifest_row *row)
{
    (void)ctx;
    if (!_stricmp(row->salt, "alexrimp")) return sh_textf("%s\\alex.r2-imp", g_root);
    if (!_stricmp(row->salt, "cratebox")) return sh_textf("%s\\crates", g_root);
    return NULL;
}
static char *author_read(void *ctx, const char *request, size_t *len)
{
    unsigned char *bytes = NULL;
    size_t n = 0;
    char *out;
    (void)ctx;
    if (sh_modserve_open(request, &bytes, &n) != 1 || !bytes) return NULL;
    out = (char *)malloc(n + 1);
    if (out) { memcpy(out, bytes, n); out[n] = 0; *len = n; }
    HeapFree(GetProcessHeap(), 0, bytes);
    return out;
}
static int fixture_game(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}
static int stock_cb(void *ctx, const char *request) { (void)ctx; return fake_stock(request); }
static sh_mb_resolver g_resolver = { NULL, NULL, stock_cb, NULL };

static void bind_fakes(void)
{
    sh_modui_engine e;
    memset(&e, 0, sizeof e);
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
    e.scope_activate = fake_scope_apply;
    e.map_left = fake_map_left;
    e.clear_undo = fake_clear_undo;
    e.mark_modified = fake_mark_modified;
    e.undo_pending = fake_undo_pending;
    e.launch_preload = fake_preload;
    sh_modui_bind(&e);
}

/* What the map uses of Snapmap+ itself: on for the last case. */
static unsigned g_uses;
static unsigned fake_uses(const char *json, size_t length, const char *const *names, size_t count)
{
    (void)json; (void)length; (void)names; (void)count;
    return g_uses;
}

/* ---- documents -------------------------------------------------------------------------------- */

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
static char *list_doc(void) { return sh_modui_list() < 0 ? NULL : fetch(); }
static char *run_doc(int action, const char *arg) { return sh_modui_run(action, arg) < 0 ? NULL : fetch(); }
static int has(const char *doc, const char *needle) { return doc && strstr(doc, needle) != NULL; }

/* A string member's text (escapes as written), or "". */
static const char *sv(const mpj *o, const char *key)
{
    const mpj *v = o ? mpj_get(o, key) : NULL;
    return v && v->type == MPJ_STR && v->text ? v->text : "";
}
static int bv(const mpj *o, const char *key)
{
    const mpj *v = o ? mpj_get(o, key) : NULL;
    return v && v->type == MPJ_BOOL && v->b;
}
static void put(mpj *o, const char *key, mpj *v) { if (v) (void)mpj_set(o, key, v); }
static mpj *str_of(const char *s) { return mpj_new_str(s ? s : ""); }
static mpj *bool_of(int b) { mpj *v = mpj_new(MPJ_BOOL); if (v) v->b = b ? 1 : 0; return v; }
static mpj *copy_of(const mpj *o, const char *key) { const mpj *v = o ? mpj_get(o, key) : NULL; return v ? mpj_clone(v) : NULL; }

/* Case-insensitive membership of a salt in a list of salts. */
static int salt_in(const char *const *list, int n, const char *salt)
{
    int i;
    for (i = 0; i < n; i++) if (!_stricmp(list[i], salt)) return 1;
    return 0;
}

static mpj *g_cases;
static unsigned g_gen_before;

/* One step: read the document, save the map, and hold the document's map.mods to the save's rows. */
static void step(const char *name)
{
    char *doc, *saved, *json;
    size_t jl = 0, sl = 0, i, k;
    mpj *d = NULL, *c, *mods_out, *map_out, *save_out;
    const mpj *data, *map, *mm, *mods;
    sh_modmanifest_list rows;
    const char *doc_salts[16], *save_salts[16];
    int nd = 0, ns = 0;
    unsigned rej = 0;

    printf("-- %s\n", name);
    doc = list_doc();
    CHECK(doc != NULL);
    d = doc ? mpj_parse(doc, strlen(doc)) : NULL;
    CHECK(d != NULL);
    data = d;
    map = data ? mpj_get(data, "map") : NULL;
    mm = map ? mpj_get(map, "mods") : NULL;
    mods = data ? mpj_get(data, "mods") : NULL;

    /* The save the author makes next, through the product's own save step. */
    memset(&rows, 0, sizeof rows);
    json = (char *)map_text(&jl);
    saved = g_map_on ? sh_modmanifest_on_save(json, jl, &sl) : NULL;
    if (saved) {
        (void)sh_modmanifest_read(saved, sl, &rows, &rej);
        CHECK(rej == 0);
    }

    c = mpj_new(MPJ_OBJ);
    put(c, "name", str_of(name));
    mods_out = mpj_new(MPJ_ARR);
    for (i = 0; mods && mods->type == MPJ_ARR && i < (size_t)mods->n; i++) {
        const mpj *b = mods->items[i], *facts = mpj_get(b, "facts");
        mpj *o = mpj_new(MPJ_OBJ);
        put(o, "key", copy_of(b, "key"));
        put(o, "salt", copy_of(b, "salt"));
        put(o, "name", copy_of(b, "name"));
        put(o, "state", copy_of(b, "state"));
        put(o, "editorRows", copy_of(b, "editorRows"));
        put(o, "modules", copy_of(b, "modules"));
        put(o, "place", copy_of(b, "place"));
        put(o, "placeTitle", copy_of(b, "placeTitle"));
        if (facts) {
            mpj *f = mpj_new(MPJ_OBJ);
            put(f, "editorOnly", bool_of(bv(facts, "editorOnly")));
            put(o, "facts", f);
        }
        mpj_push(mods_out, o);
    }
    put(c, "mods", mods_out);
    map_out = mpj_new(MPJ_OBJ);
    put(map_out, "entities", copy_of(map, "entities"));
    put(map_out, "name", copy_of(map, "name"));
    {
        mpj *arr = mpj_new(MPJ_ARR);
        for (i = 0; mm && mm->type == MPJ_ARR && i < (size_t)mm->n; i++) {
            const mpj *x = mm->items[i];
            mpj *o = mpj_new(MPJ_OBJ);
            static const char *const keys[] = { "key", "salt", "name", "state", "placedCount", "placedModules", "attached",
                                                "nothingPlaced", "viaNeeds", "pinned", "objects", "update", NULL };
            int q;
            for (q = 0; keys[q]; q++) put(o, keys[q], copy_of(x, keys[q]));
            mpj_push(arr, o);
            if (nd < 16) doc_salts[nd++] = sv(x, "salt");
        }
        put(map_out, "mods", arr);
    }
    put(c, "map", map_out);
    save_out = mpj_new(MPJ_ARR);
    for (i = 0; i < rows.count; i++) {
        const sh_modmanifest_row *r = &rows.rows[i];
        mpj *o = mpj_new(MPJ_OBJ), *names = mpj_new(MPJ_ARR);
        put(o, "salt", str_of(r->salt));
        put(o, "attached", bool_of(r->attached));
        put(o, "pinned", bool_of(r->pinned));
        for (k = 0; k < r->name_count; k++) mpj_push(names, str_of(r->names[k]));
        put(o, "names", names);
        mpj_push(save_out, o);
        if (ns < 16) save_salts[ns++] = r->salt;
    }
    put(c, "save", save_out);
    mpj_push(g_cases, c);

    /* The document's mods are the save's mods, no more and no fewer. */
    for (i = 0; i < (size_t)nd; i++) {
        if (!salt_in(save_salts, ns, doc_salts[i])) printf("    the document names %s, which the save does not write\n", doc_salts[i]);
        CHECK(salt_in(save_salts, ns, doc_salts[i]));
    }
    for (i = 0; i < (size_t)ns; i++) {
        if (!salt_in(doc_salts, nd, save_salts[i])) printf("    the save writes %s, which the document does not name\n", save_salts[i]);
        CHECK(salt_in(doc_salts, nd, save_salts[i]));
    }
    /* One entry per mod. */
    for (i = 0; i < (size_t)nd; i++) for (k = i + 1; k < (size_t)nd; k++) CHECK(_stricmp(doc_salts[i], doc_salts[k]));
    /* A row written with units is said to be one; a map-level row is said to be one. */
    for (i = 0; mm && mm->type == MPJ_ARR && i < (size_t)mm->n; i++) {
        const mpj *x = mm->items[i];
        for (k = 0; k < rows.count; k++) {
            if (_stricmp(rows.rows[k].salt, sv(x, "salt"))) continue;
            CHECK(bv(x, "pinned") == (rows.rows[k].pinned != 0));
            CHECK(bv(x, "attached") == (rows.rows[k].attached != 0));
        }
    }
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    sh_modmanifest_list_free(&rows);
    mpj_free(d);
    free(doc);
}

/* The editor poll (sh_modui_poll_main), once its throttle has passed: whether the document's generation moved. */
static int poll_moved(void)
{
    unsigned before = sh_modui_generation();
    Sleep(1050);
    (void)sh_modui_poll_main(NULL);
    return sh_modui_generation() != before;
}

static void put_mod(const char *dir, const char *json, const char *rel, const char *body)
{
    CreateDirectoryA(dir, NULL);
    mt_write(dir, "mod.json", json);
    if (rel) mt_write(dir, rel, body);
}

static int install(const char *src)
{
    char *doc = run_doc(SH_MODS_ACT_INSTALL, src);
    int ok = has(doc, "\"ok\":true");
    if (!ok) printf("    install %s: %s\n", src, doc ? doc : "(null)");
    free(doc);
    return ok;
}

/* The open map as a map whose rows of `salt` hold content (a published or downloaded map's): the map is saved, the
 * rows of that mod are pinned to their objects' units (modpin, as a publish does), the text with them is loaded in the
 * editor, and the content cache composes them, with the installed folder of the same mod alongside. The
 * other rows stay as the save wrote them. */
static void load_as_content(const char *salt)
{
    sh_modmanifest_list rows, pin;
    sh_modpin_result pr;
    char err[256], *text, *saved, content[MAX_PATH * 2], **placed = NULL;
    size_t tl = 0, jl = 0, sl = 0, np, i;
    int failed = 0, pinned = 0;
    unsigned rej = 0;
    const char *json = map_text(&jl);
    saved = sh_modmanifest_on_save(json, jl, &sl);
    CHECK(saved != NULL);
    memset(&rows, 0, sizeof rows);
    memset(&pin, 0, sizeof pin);
    memset(&pr, 0, sizeof pr);
    if (saved) (void)sh_modmanifest_read(saved, sl, &rows, &rej);
    for (i = 0; i < rows.count; i++) if (!_stricmp(rows.rows[i].salt, salt)) CHECK(sh_modmanifest_list_add(&pin, &rows.rows[i]));
    _snprintf_s(content, sizeof content, _TRUNCATE, "%s\\content", mt_cache_dir(g_root));
    CHECK(sh_modpin_rows(&pin, saved ? saved : json, saved ? sl : jl, content, &pr) >= 0);
    for (i = 0; i < pin.count; i++) pinned += pin.rows[i].pinned != 0;
    CHECK(pin.count == 1 && pinned == 1);
    /* the map's rows: the pinned row of the mod, every other row as saved */
    for (i = 0; i < rows.count; i++) if (_stricmp(rows.rows[i].salt, salt)) CHECK(sh_modmanifest_list_add(&pin, &rows.rows[i]));
    text = sh_modmanifest_write(json, jl, &pin, &tl, err, sizeof err);
    CHECK(text != NULL);
    if (text) {
        sh_modmanifest_prepare_load(text, tl);
        HeapFree(GetProcessHeap(), 0, text);
    }
    np = sh_modmanifest_placed_names(json, jl, &placed, &failed);
    sh_cache_on_load(&pin, placed, np);
    sh_modmanifest_names_free(placed, np);
    CHECK(sh_modserve_index_build(g_root) >= 0);
    sh_modpin_result_free(&pr);
    sh_modmanifest_list_free(&pin);
    sh_modmanifest_list_free(&rows);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = NULL;
    char *buf;
    long n;
    *len = 0;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; *len = (size_t)n; }
    return buf;
}

int main(int argc, char **argv)
{
    char fixture[MAX_PATH], cases_path[MAX_PATH], out_path[MAX_PATH], tmp[MAX_PATH], dir[MAX_PATH * 2], src[MAX_PATH * 2];
    char *doc;
    int i;

    if (argc < 4) { fprintf(stderr, "usage: mods_inmap_test <alex.r2-imp> <fixture.json> <out.json>\n"); return 2; }
    if (!GetFullPathNameA(argv[1], sizeof fixture, fixture, NULL) || !GetFullPathNameA(argv[2], sizeof cases_path, cases_path, NULL) ||
        !GetFullPathNameA(argv[3], sizeof out_path, out_path, NULL)) return 2;
    g_echo = GetEnvironmentVariableA("SH_MODUI_ECHO", NULL, 0) > 0;
    if (!mt_temp_dir("inmap", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%s\\store\\mods", tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\store", tmp);
    CreateDirectoryA(dir, NULL);
    CreateDirectoryA(g_root, NULL);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\authoring", tmp);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\saves", tmp);
    CreateDirectoryA(dir, NULL);
    sh_saved_maps_set_root(dir);
    CHECK(sh_ms_set_root(g_root));
    bind_fakes();
    sh_modmanifest_set_uses(fake_uses);
    sh_modmanifest_set_carries(sh_modserve_root_carries);   /* the version rule, as the product binds it */
    sh_modpin_set_folder(author_folder, NULL);
    sh_modpin_set_read(author_read, NULL);
    sh_modpin_set_game(fixture_game, NULL);
    sh_cache_set_resolver(&g_resolver);
    sh_cache_set_mods_root(g_root);
    CHECK(sh_modserve_index_build(g_root) == 0);
    g_cases = mpj_new(MPJ_ARR);

    /* The mods: an imp pack with two Toybox rows; an escort whose object needs the imp; a mod with nothing to place
     * (a player-side change, as Bare Hands); a mod of editor tweaks only; a crate mod a downloaded map also brought. */
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\alex.r2-imp", tmp);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(install(src));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\imp-escort", tmp);
    put_mod(src, "{\"name\":\"Imp Escort\",\"snapmap-plus\":{\"format\":2,\"salt\":\"impescrt\"}}", "entities\\escort.decl",
            "{\r\n\tinherit = \"mods/alexrimp/imp_copy\";\r\n}\r\n");
    CHECK(install(src));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\bare-hands", tmp);
    put_mod(src, "{\"name\":\"Bare Hands\",\"requirements\":{\"cvars\":{\"g_weaponDoomMPUseDoomHands\":\"1\"}},"
                 "\"snapmap-plus\":{\"format\":2,\"salt\":\"barehand\"}}", "entities\\body.decl",
            "{\r\n\tinherit = \"player/mp\";\r\n}\r\n");
    CHECK(install(src));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\editor-grid", tmp);
    put_mod(src, "{\"name\":\"Editor Grid\",\"snapmap-plus\":{\"format\":2,\"salt\":\"edgridtw\"}}",
            "native\\generated\\decls\\snapeditorentitydef\\prop\\prop_base.decl",
            "{\r\n\tinherit = \"prop/prop_base\";\r\n\tedit = { gridSnap = 8; }\r\n}\r\n");
    CHECK(install(src));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\crates", tmp);
    put_mod(src, "{\"name\":\"Crates\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":\"0\","
                 "\"g_useResourceBlackList\":\"0\"}},\"snapmap-plus\":{\"format\":2,\"salt\":\"cratebox\"}}", "entities\\crate.decl",
            "{\r\n\tinherit = \"prop/prop_base\";\r\n}\r\n");
    CHECK(install(src));
    free(run_doc(SH_MODS_ACT_RESCAN, NULL));
    doc = list_doc();
    CHECK(has(doc, "\"key\":\"alex.r2-imp\"") && has(doc, "\"key\":\"imp-escort\"") && has(doc, "\"key\":\"bare-hands\"") &&
          has(doc, "\"key\":\"editor-grid\"") && has(doc, "\"key\":\"crates\""));
    CHECK(has(doc, "\"editorRows\":2,") || has(doc, "\"editorRows\":2}"));
    CHECK(has(doc, "\"modules\":0"));
    if (failures) printf("%s\n", doc ? doc : "(null)");
    free(doc);

    /* The editor opens a new map: a player start. */
    g_ents[0].uid = 0; g_ents[0].inherit = "player/coop/player_start"; g_ents[0].alive = 1;
    g_ent_n = 1;
    g_map_on = 1;
    sh_modserve_scope_enable(1);
    (void)sh_modui_poll_main(NULL);
    CHECK(g_image == 1 && g_resource == 1);
    g_ents[1] = (fake_ent){ 65, "mods/cratebox/crate", 1 };
    g_ent_n = 2;
    CHECK(poll_moved() && g_image == 0 && g_resource == 0);
    i = g_scope_changes;
    g_ents[2] = (fake_ent){ 66, "mods/cratebox/crate", 1 };
    g_ent_n = 3;
    CHECK(!poll_moved() && g_scope_changes == i);
    g_ents[1].alive = 0;
    CHECK(!poll_moved() && g_image == 0 && g_resource == 0);
    {
        size_t len, written;
        char *map = map_text(&len);
        char *saved = sh_modmanifest_on_save(map, len, &written);
        CHECK(saved != NULL);
        if (saved) HeapFree(GetProcessHeap(), 0, saved);
    }
    g_ents[2].alive = 0;
    g_placement_failed = 1;
    CHECK(!poll_moved() && g_image == 0 && g_resource == 0);
    g_placement_failed = 0;
    CHECK(poll_moved() && g_image == 1 && g_resource == 1);    /* last saved object deleted */
    g_ents[2].alive = 1;
    CHECK(poll_moved() && g_image == 0 && g_resource == 0);    /* native undo */
    free(run_doc(SH_MODS_ACT_MAP_ADD, "crates"));
    g_ents[2].alive = 0;
    CHECK(poll_moved() && g_image == 0 && g_resource == 0);    /* still attached */
    g_map_on = 0;
    (void)sh_modui_poll_main(NULL);
    CHECK(g_image == 1 && g_resource == 1);
    sh_modserve_scope_enable(0);
    g_ent_n = 1;
    g_map_on = 1;
    (void)sh_modui_poll_main(NULL);
    step("nothing placed");

    g_ents[1].uid = 1; g_ents[1].inherit = "mods/alexrimp/imp_copy"; g_ents[1].alive = 1;
    g_ent_n = 2;
    CHECK(poll_moved());   /* the placement moves the document */
    step("a mod's object placed, before any save");
    CHECK(!poll_moved());  /* nothing changed since */

    g_ents[2].uid = 2; g_ents[2].inherit = "mods/alexrimp/imp_copy"; g_ents[2].alive = 1;
    g_ent_n = 3;
    step("a second object of the same mod");

    g_ents[3].uid = 3; g_ents[3].inherit = "mods/impescrt/escort"; g_ents[3].alive = 1;
    g_ent_n = 4;
    g_ents[1].alive = g_ents[2].alive = 0;
    CHECK(poll_moved());
    step("a mod reached only through another mod's object");

    free(run_doc(SH_MODS_ACT_MAP_ADD, "bare-hands"));
    step("a mod used by the whole map (Use in this map)");

    g_ents[3].alive = 0;
    g_ents[1].alive = 1;
    step("a saved mod's object placed again");
    g_ents[1].alive = 0;
    CHECK(poll_moved());   /* the delete moves the document */
    step("the last object of a saved mod deleted, before the next save");

    g_ents[1].alive = 1;
    CHECK(poll_moved());   /* the undo moves it again */
    step("that delete undone");

    g_ents[4].uid = 4; g_ents[4].inherit = "prop/prop_base"; g_ents[4].alive = 1;
    g_ent_n = 5;
    step("a stock object placed that a mod of editor tweaks changes");

    g_ents[5].uid = 5; g_ents[5].inherit = "mods/cratebox/crate"; g_ents[5].alive = 1;
    g_ent_n = 6;
    /* A downloaded map opened in the editor: its crates are its own content (in the content cache), and the player
     * has the same mod installed. The installed folder shows as in this map, which holds its own copy. */
    load_as_content("cratebox");
    doc = list_doc();
    CHECK(has(doc, "\"key\":\"crates\",\"salt\":\"cratebox\""));
    free(doc);
    step("a downloaded map's content of a mod the player has installed too");

    free(run_doc(SH_MODS_ACT_MAP_REMOVE, "bare-hands"));
    step("Remove from map of the mod used by the whole map");

    /* The author publishes the map: its imp row holds the published units, and the published text opened in the
     * editor composes them, with the installed folder of the same mod alongside. */
    for (i = 0; i < g_ent_n; i++) g_ents[i].alive = i <= 1;
    load_as_content("alexrimp");
    doc = list_doc();
    CHECK(has(doc, "\"key\":\"alex.r2-imp\",\"salt\":\"alexrimp\""));   /* the installed folder is the map's mod */
    CHECK(has(doc, "\"pinned\":true"));
    free(doc);
    step("a published map's content, the installed folder of the mod alongside");

    free(run_doc(SH_MODS_ACT_MAP_UPDATE, "alexrimp"));
    step("Update to the installed version");

    g_uses = 1;
    step("a map that uses Snapmap+'s own features (its entry is no mod)");
    g_uses = 0;

    g_map_on = 0;
    step("no map open");

    {
        /* The page's half reads these cases (tests/mods_inmap_test.js): written here, held to the committed copy. */
        size_t len = 0, have_len = 0;
        char *out = mpj_dump(g_cases, &len), *have;
        FILE *f = NULL;
        CHECK(out != NULL);
        if (out && !fopen_s(&f, out_path, "wb") && f) { fwrite(out, 1, len, f); fputc('\n', f); fclose(f); }
        have = read_all(cases_path, &have_len);
        while (have && have_len && (have[have_len - 1] == '\n' || have[have_len - 1] == '\r')) have[--have_len] = 0;
        if (!have || !out || have_len != len || memcmp(have, out, len)) {
            printf("  the cases differ from %s; after reading the change, copy %s over it\n", cases_path, out_path);
            CHECK(!"the committed cases are this run's");
        }
        free(have);
        free(out);
    }
    mpj_free(g_cases);
    sh_saved_maps_set_root(NULL);
    mt_remove_tree(tmp);
    printf("mods_inmap_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
