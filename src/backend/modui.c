/* modui.c -- see modui.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "snapmap_plus_iface.h"
#include "../common/text.h"
#include "modui.h"
#include "modclaim.h"
#include "modcache.h"
#include "modauthor.h"  /* sh_ma_engine_name: a Toybox row's entityDef in the engine's spelling */
#include "modpin.h"     /* sh_modpin_row_differs: Update only when the installed version changes what a map serves */
#include "feature_health.h"
#include "modbundle.h"
#include "modstore.h"
#include "modserve.h"
#include "map_carrier.h"
#include "modmanifest.h"
#include "modregistry.h"
#include "decl_tree.h"
#include "backend_log.h"
#include "config_json.h"
#include "modmap.h"
#include "mp_json.h"
#include "editor_delete.h"
#include <shlobj.h>
#include <time.h>
#include "saved_maps.h"
#include "modbankinfo.h"
#include "modzip.h"
#ifndef SH_MODUI_PURE
#include "overrides.h"
#include "modstock.h"
#include "editor_undo.h"
#include "map_settings.h"
#include "config.h"
#include "modsound.h"
#include "script/script_runtime.h"
#endif

/* A stock resource every 13954591 SnapMap install ships (as modcmd.c probes):
 * reading it proves the installed-resource reader works before its answers
 * are trusted. */
#define MU_STOCK_PROBE "generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl"

#define MU_TAG               "mods" /* the search tag a map with mods carries (modpublish.h) */

/* ------------------------------------------------------------------ state */

static SRWLOCK g_lock = SRWLOCK_INIT;   /* guards g_doc, g_doc_len */
static char   *g_doc;
static size_t  g_doc_len;
static volatile LONG g_gen = 1;
static sh_modui_engine g_engine;
static int g_engine_bound;

static void mu_log(const char *fmt, ...)
{
    char *body, *line;
    va_list ap;
    va_start(ap, fmt);
    body = sh_vtextf(fmt, ap);
    va_end(ap);
    line = sh_textf("MODS: ui %s", body ? body : "(out of memory)");
    free(body);
    backend_log(line ? line : "MODS: ui (out of memory)");
    free(line);
}

/* ------------------------------------------------------------------ product defaults */

#ifndef SH_MODUI_PURE
static int mu_on_main(void);

static int mu_default_stock(const char *request)
{
    static int probed;   /* 0 unprobed, 1 ok, -1 unavailable */
    unsigned char *body = NULL;
    size_t len = 0;
    /* Off the main thread (a build the window runs on its worker) only the
     * installed archives answer: the engine's provider is the main thread's. */
    if (!mu_on_main()) return sh_modstock_exists(request);
    if (probed == 0) {
        __try { body = sh_overrides_read_engine_resource(MU_STOCK_PROBE, &len); }
        __except (EXCEPTION_EXECUTE_HANDLER) { body = NULL; }
        probed = body ? 1 : -1;
        if (body) HeapFree(GetProcessHeap(), 0, body);
        body = NULL;
    }
    /* The installed archives themselves answer what the engine provider
     * cannot see (campaign-only content in gameresources). */
    if (probed < 0) return sh_modstock_exists(request);
    __try { body = sh_overrides_read_engine_resource(request, &len); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return sh_modstock_exists(request); }
    if (!body) return sh_modstock_exists(request) == 1 ? 1 : 0;
    HeapFree(GetProcessHeap(), 0, body);
    return 1;
}
#endif

/* The open map's settings (map_settings.h) follow its manifest: they go when
 * the map closes and come back when a launch over the editor is refused. */
#ifndef SH_MODUI_PURE
static void mu_settings_close(void) { sh_map_settings_reset(); }
static void mu_settings_restore(void)
{
    sh_map_settings s;
    if (!sh_map_settings_restore_previous()) return;
    /* The refused map's parse selected its own; the editor map's come back. */
    sh_map_settings_current(&s);
    sh_map_render_select(&s.render);
    sh_map_bounds_select(&s.bounds);
}
#else
static void mu_settings_close(void) {}
static void mu_settings_restore(void) {}
#endif

#ifndef SH_MODUI_PURE
/* The product's script runtime, as the tab reads it (any thread). */
static int mu_default_script_info(const char *salt, sh_modui_script_info *out)
{
    sh_script_mod_info i;
    if (!out || !sh_script_mod_info_get(salt, &i)) return 0;
    memset(out, 0, sizeof *out);
    out->status = i.status == SH_SCRIPT_STOPPED ? SH_MODUI_SCRIPT_STOPPED
                : i.waiting ? SH_MODUI_SCRIPT_WAITING
                : i.status == SH_SCRIPT_RUNNING ? SH_MODUI_SCRIPT_RUNNING
                : i.status == SH_SCRIPT_ENDED ? SH_MODUI_SCRIPT_ENDED : SH_MODUI_SCRIPT_NONE;
    out->lines = i.log_lines;
    out->stop = i.stopped_line ? sh_script_stop_line_dup(salt) : NULL;
    sh_script_mod_info_free(&i);
    return 1;
}
static int mu_default_script_log(const char *salt, sh_modui_line_fn fn, void *context)
{
    return sh_script_log_read(salt, (sh_script_line_fn)fn, context);
}
typedef struct mu_salt_visit { sh_modui_name_fn fn; void *context; } mu_salt_visit;
static void mu_salt_visit_one(void *context, const char *salt, const char *name)
{
    mu_salt_visit *v = (mu_salt_visit *)context;
    (void)name;
    v->fn(v->context, salt);
}
static int mu_default_script_mods(sh_modui_name_fn fn, void *context)
{
    mu_salt_visit v;
    v.fn = fn;
    v.context = context;
    return sh_script_mods(mu_salt_visit_one, &v);
}
#endif

static void mu_defaults(sh_modui_engine *e)
{
    memset(e, 0, sizeof *e);
#ifndef SH_MODUI_PURE
    e->script_info = mu_default_script_info;
    e->script_log = mu_default_script_log;
    e->script_mods = mu_default_script_mods;
    e->restart_banks = sh_modsound_restart_banks;
    e->rescan = sh_modserve_index_rescan;
    e->refresh = sh_modserve_refresh_after_rescan;
    e->palette_rebuild = modserve_palette_rebuild_requested;
    e->stock_exists = mu_default_stock;
    e->scope_apply = sh_modserve_scope_apply;
    e->scope_activate = sh_modserve_scope_activate;
    e->clear_undo = sh_editor_undo_clear;
    e->mark_modified = sh_editor_mark_modified;
    e->undo_pending = sh_editor_delete_native_pending;
    e->launch_preload = sh_modserve_launch_preload;
#endif
}

static void mu_scope_hook(const sh_modmanifest_list *rows, const char *const *names, size_t name_count, int when);
static int mu_scope_armed(void);
static int mu_scope_source(int when);
static int mu_resolve_row(const char *build, const char *salt, sh_modmanifest_row *out);
static int mu_scope_now(const char *phase);
static int mu_row_served_by(const sh_modmanifest_row *row, char *out, size_t cap);
static int mu_scope_listed(char (*out)[SH_MODMANIFEST_HASH_CAP], int cap);
static void mu_mark_modified(const char *label);

static void mu_labeler(const char *const *placed, size_t n, char **labels);
void sh_modui_bind(const sh_modui_engine *engine)
{
    mu_defaults(&g_engine);
    if (engine) {
        if (engine->on_main_thread) g_engine.on_main_thread = engine->on_main_thread;
        if (engine->placed_names) g_engine.placed_names = engine->placed_names;
        if (engine->placement_revision) g_engine.placement_revision = engine->placement_revision;
        if (engine->rescan) g_engine.rescan = engine->rescan;
        if (engine->refresh) g_engine.refresh = engine->refresh;
        if (engine->palette_rebuild) g_engine.palette_rebuild = engine->palette_rebuild;
        if (engine->stock_exists) g_engine.stock_exists = engine->stock_exists;
        if (engine->map_json) g_engine.map_json = engine->map_json;
        if (engine->map_info) g_engine.map_info = engine->map_info;
        if (engine->delete_entities) g_engine.delete_entities = engine->delete_entities;
        if (engine->remove_tag) g_engine.remove_tag = engine->remove_tag;
        if (engine->busy) g_engine.busy = engine->busy;
        if (engine->toast) g_engine.toast = engine->toast;
        if (engine->print) g_engine.print = engine->print;
        if (engine->scope_apply) g_engine.scope_apply = engine->scope_apply;
        if (engine->scope_activate) g_engine.scope_activate = engine->scope_activate;
        if (engine->map_left) g_engine.map_left = engine->map_left;
        if (engine->clear_undo) g_engine.clear_undo = engine->clear_undo;
        if (engine->mark_modified) g_engine.mark_modified = engine->mark_modified;
        if (engine->undo_pending) g_engine.undo_pending = engine->undo_pending;
        if (engine->launch_preload) g_engine.launch_preload = engine->launch_preload;
        if (engine->script_info) g_engine.script_info = engine->script_info;
        if (engine->script_log) g_engine.script_log = engine->script_log;
        if (engine->script_mods) g_engine.script_mods = engine->script_mods;
        if (engine->restart_banks) g_engine.restart_banks = engine->restart_banks;
    }
    g_engine_bound = 1;
    /* The map scope: what the open map lists, and the build a map-level row names. */
    sh_modmanifest_set_scope(mu_scope_hook, mu_scope_armed);
    sh_modmanifest_set_resolver(mu_resolve_row);
    sh_modmanifest_set_listed(mu_scope_listed);
    sh_modmanifest_set_labeler(mu_labeler);
    sh_modserve_set_scope_source(mu_scope_source);
}

static const sh_modui_engine *mu_engine(void)
{
    if (!g_engine_bound) sh_modui_bind(NULL);
    return &g_engine;
}

/* 1 when the call may touch the store: on the main thread, or no check bound. */
static int mu_on_main(void)
{
    const sh_modui_engine *e = mu_engine();
    return !e->on_main_thread || e->on_main_thread() == 1;
}

/* ------------------------------------------------------------------ JSON buffer */

typedef struct mu_buf { char *p; size_t len, cap; int oom; } mu_buf;

static void mu_reserve(mu_buf *b, size_t extra)
{
    size_t want;
    char *np;
    if (b->oom) return;
    want = b->len + extra + 1;
    if (want <= b->cap) return;
    if (want < b->cap * 2) want = b->cap * 2;
    if (want < 4096) want = 4096;
    np = (char *)realloc(b->p, want);
    if (!np) { b->oom = 1; return; }
    b->p = np;
    b->cap = want;
}

static void mu_putn(mu_buf *b, const char *s, size_t n)
{
    mu_reserve(b, n);
    if (b->oom) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void mu_puts(mu_buf *b, const char *s) { mu_putn(b, s, strlen(s)); }

/* A JSON string: '"' and '\\' escaped, control bytes as \u00xx, other bytes
 * (UTF-8 from the store) copied. */
static void mu_str(mu_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    mu_putn(b, "\"", 1);
    for (; *p; p++) {
        char e[7];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; mu_putn(b, e, 2); }
        else if (*p < 0x20) {
            e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hex[*p >> 4]; e[5] = hex[*p & 15];
            mu_putn(b, e, 6);
        } else mu_putn(b, (const char *)p, 1);
    }
    mu_putn(b, "\"", 1);
}

static void mu_num(mu_buf *b, unsigned long long v)
{
    char t[32];
    _snprintf_s(t, sizeof t, _TRUNCATE, "%llu", v);
    mu_puts(b, t);
}

static void mu_int(mu_buf *b, long long v)
{
    char t[32];
    _snprintf_s(t, sizeof t, _TRUNCATE, "%lld", v);
    mu_puts(b, t);
}

static void mu_key(mu_buf *b, const char *k, int first)
{
    if (!first) mu_putn(b, ",", 1);
    mu_str(b, k);
    mu_putn(b, ":", 1);
}

static void mu_kstr(mu_buf *b, const char *k, const char *v, int first) { mu_key(b, k, first); mu_str(b, v); }
static void mu_knum(mu_buf *b, const char *k, unsigned long long v, int first) { mu_key(b, k, first); mu_num(b, v); }
static void mu_kint(mu_buf *b, const char *k, long long v, int first) { mu_key(b, k, first); mu_int(b, v); }
static void mu_kbool(mu_buf *b, const char *k, int v, int first) { mu_key(b, k, first); mu_puts(b, v ? "true" : "false"); }

/* Publish b as the shared document (ownership moves). Returns its length or -1. */
static int mu_publish(mu_buf *b)
{
    int n;
    if (b->oom || !b->p) { free(b->p); return -1; }
    AcquireSRWLockExclusive(&g_lock);
    free(g_doc);
    g_doc = b->p;
    g_doc_len = b->len;
    n = (int)b->len;
    ReleaseSRWLockExclusive(&g_lock);
    b->p = NULL;
    return n;
}

int sh_modui_fetch(char *out, int out_cap, int *needed, unsigned *generation)
{
    int r;
    if (generation) *generation = (unsigned)g_gen;
    AcquireSRWLockShared(&g_lock);
    if (needed) *needed = (int)g_doc_len;
    if (!g_doc) r = 0;
    else if (!out || out_cap <= (int)g_doc_len) r = -1;
    else { memcpy(out, g_doc, g_doc_len + 1); r = 1; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

unsigned sh_modui_generation(void) { return (unsigned)g_gen; }

/* The publishing key's Settings requests and its state (modclaim.h), bound by the publish layer: unbound, the key
 * is not offered. */
static char *(*g_key_action)(const char *arg);
static char *(*g_key_status)(void);

void sh_modui_note_changed(void) { InterlockedIncrement(&g_gen); }

/* ------------------------------------------------------------------ mod scripts */

static const char *mu_script_status_name(int status)
{
    switch (status) {
    case SH_MODUI_SCRIPT_RUNNING: return "running";
    case SH_MODUI_SCRIPT_STOPPED: return "stopped";
    case SH_MODUI_SCRIPT_ENDED:   return "ended";
    case SH_MODUI_SCRIPT_WAITING: return "waiting";
    default:                      return "none";
    }
}

/* {"status","stop","lines"} of `salt`'s scripts in the current or last Play,
 * after a key; nothing when no Play has run them. */
static void mu_put_script_run(mu_buf *b, const char *key, const char *salt)
{
    const sh_modui_engine *e = mu_engine();
    sh_modui_script_info info;
    memset(&info, 0, sizeof info);
    if (!e->script_info || !salt || !salt[0] || e->script_info(salt, &info) != 1) return;
    mu_key(b, key, 0);
    mu_putn(b, "{", 1);
    mu_kstr(b, "status", mu_script_status_name(info.status), 1);
    mu_kstr(b, "stop", info.status == SH_MODUI_SCRIPT_STOPPED && info.stop ? info.stop : "", 0);
    mu_knum(b, "lines", (unsigned)(info.lines < 0 ? 0 : info.lines), 0);
    mu_putn(b, "}", 1);
    free(info.stop);
}

/* ------------------------------------------------------------------ sound banks a restart brings in */

typedef struct mu_banks { mu_buf *b; const char *owner; int count; mu_buf *line; } mu_banks;

static void mu_bank_row(void *context, const char *bank, const char *owner)
{
    mu_banks *v = (mu_banks *)context;
    if (!bank || !bank[0] || (v->owner && (!owner || _stricmp(owner, v->owner)))) return;
    if (v->b) {
        if (v->count) mu_putn(v->b, ",", 1);
        mu_str(v->b, bank);
    } else if (v->line) {
        if (v->count) mu_puts(v->line, ", ");
        mu_puts(v->line, bank);
    }
    v->count++;
}

/* "restartBanks": the mod's sound banks whose new bytes the game hears only
 * after a restart (Wwise keeps the bank it loaded first under a name). */
static void mu_put_restart_banks(mu_buf *b, const char *salt)
{
    const sh_modui_engine *e = mu_engine();
    mu_banks v;
    memset(&v, 0, sizeof v);
    if (!e->restart_banks || !salt || !salt[0]) return;
    mu_puts(b, ",\"restartBanks\":[");
    v.b = b;
    v.owner = salt;
    (void)e->restart_banks(mu_bank_row, &v);
    mu_putn(b, "]", 1);
}

/* "Restart DOOM to hear the new <bank>, <bank>." (malloc'd), or NULL when no
 * bank waits. */
static char *mu_restart_sentence(void)
{
    const sh_modui_engine *e = mu_engine();
    mu_buf names = { 0 };
    mu_banks v;
    char *out = NULL;
    memset(&v, 0, sizeof v);
    v.line = &names;
    if (e->restart_banks && e->restart_banks(mu_bank_row, &v) > 0 && names.p && !names.oom && names.p[0])
        out = sh_textf("Restart DOOM to hear the new %s.", names.p);
    free(names.p);
    return out;
}

typedef struct mu_script_list { mu_buf *b; int count; long skip; long seen; } mu_script_list;

static void mu_script_mod(void *context, const char *salt)
{
    mu_script_list *l = (mu_script_list *)context;
    const sh_modui_engine *e = mu_engine();
    sh_modui_script_info info;
    memset(&info, 0, sizeof info);
    if (!salt || !salt[0] || !e->script_info || e->script_info(salt, &info) != 1) return;
    if (l->count++) mu_putn(l->b, ",", 1);
    mu_putn(l->b, "{", 1);
    mu_kstr(l->b, "salt", salt, 1);
    mu_kstr(l->b, "status", mu_script_status_name(info.status), 0);
    mu_kstr(l->b, "stop", info.status == SH_MODUI_SCRIPT_STOPPED && info.stop ? info.stop : "", 0);
    mu_knum(l->b, "lines", (unsigned)(info.lines < 0 ? 0 : info.lines), 0);
    mu_putn(l->b, "}", 1);
    free(info.stop);
}

static void mu_script_line(void *context, const char *line)
{
    mu_script_list *l = (mu_script_list *)context;
    if (l->seen++ < l->skip) return;   /* before the tail the window shows */
    if (l->count++) mu_putn(l->b, ",", 1);
    mu_str(l->b, line);
}

static void mu_script_count(void *context, const char *line)
{
    (void)line;
    ((mu_script_list *)context)->seen++;
}

int sh_modui_scripts(const char *salt, char *out, int out_cap, int *needed)
{
    const sh_modui_engine *e = mu_engine();
    mu_buf b;
    mu_script_list l;
    int r, full = salt && !strncmp(salt, SH_MODS_SCRIPTS_FULL, sizeof SH_MODS_SCRIPTS_FULL - 1);
    long total = 0;
    char num[32];
    if (full) salt += sizeof SH_MODS_SCRIPTS_FULL - 1;
    memset(&b, 0, sizeof b);
    memset(&l, 0, sizeof l);
    l.b = &b;
    mu_puts(&b, "{\"mods\":[");
    if (e->script_mods) (void)e->script_mods(mu_script_mod, &l);
    mu_putn(&b, "]", 1);
    mu_kstr(&b, "salt", salt ? salt : "", 0);
    if (salt && salt[0] && e->script_log) {
        l.seen = 0;
        (void)e->script_log(salt, mu_script_count, &l);
        total = l.seen;
    }
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"total\":%ld,\"full\":%s", total, full ? "true" : "false");
    mu_puts(&b, num);
    mu_puts(&b, ",\"log\":[");
    l.count = 0;
    l.seen = 0;
    l.skip = full || total <= SH_MODUI_SCRIPT_TAIL ? 0 : total - SH_MODUI_SCRIPT_TAIL;
    if (salt && salt[0] && e->script_log) (void)e->script_log(salt, mu_script_line, &l);
    mu_puts(&b, "]}");
    if (needed) *needed = b.oom ? 0 : (int)b.len;
    if (b.oom || !b.p) r = 0;
    else if (!out || out_cap <= (int)b.len) r = -1;
    else { memcpy(out, b.p, b.len + 1); r = 1; }
    free(b.p);
    return r;
}

#ifndef SH_MODUI_PURE
int sh_modui_scripted_maps_allowed(void)
{
    int on = 1;
    if (sh_config_get_bool("mods.scripted_maps", &on, NULL) != 1) return 1;
    return on != 0;
}
#endif

/* ------------------------------------------------------------------ store model */

/* What the document says about one store entry. */
typedef struct mu_entry {
    const sh_ms_entry *e;
    const char *state;           /* active, unindexed, skipped */
    char *cause;                 /* the plain sentence: what is wrong and what to do (malloc'd; NULL when ready) */
    int editor_rows;             /* snapEditorEntityDef files */
    int shadows;
    int admitted;                /* sh_modserve_bundle_state == 1 */
    int same_as;                 /* another root entry carrying at least half of its files, or -1 */
    unsigned same_count;         /* how many of them */
    char **row_names;            /* its editor rows' display names (strings resolved, else the decl name) */
    char **row_defs;             /* each row's entityDef: what a placed entity of that row inherits */
    int row_count;
} mu_entry;

typedef struct mu_model {
    sh_ms_list list;
    int listed;
    mu_entry *entries;
    const char *root;            /* the store root (sh_ms_root), or NULL */
    int root_ok;
} mu_model;

/* The entry holding the build `hash`, or -1. */
static int mu_find_hash(const mu_model *m, const char *hash)
{
    const sh_ms_entry *e = hash && hash[0] ? sh_ms_resolve(&m->list, hash) : NULL;
    if (!e || !e->loaded) return -1;
    return (int)(e - m->list.items);
}

/* Whether two hashes name the same installed build. */
static int mu_same_bundle(const mu_model *m, const char *a, const char *b)
{
    int x;
    if (!strcmp(a, b)) return 1;
    x = mu_find_hash(m, a);
    return x >= 0 && x == mu_find_hash(m, b);
}

/* The loaded entry of `salt`; -1 when none. */
static int mu_find_salt(const mu_model *m, const char *salt, int except)
{
    size_t i;
    if (!salt || !salt[0]) return -1;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        if ((int)i == except || !e->loaded || _stricmp(e->salt, salt)) continue;
        return (int)i;
    }
    return -1;
}

static const char *mu_leaf(const char *folder)
{
    const char *a = strrchr(folder ? folder : "", '\\'), *b = strrchr(folder ? folder : "", '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : (folder ? folder : "");
}

static void mu_model_free(mu_model *m)
{
    size_t i;
    int k;
    for (i = 0; m->entries && i < m->list.count; i++) {
        for (k = 0; k < m->entries[i].row_count; k++) { free(m->entries[i].row_names[k]); free(m->entries[i].row_defs[k]); }
        free(m->entries[i].row_names);
        free(m->entries[i].row_defs);
        free(m->entries[i].cause);
    }
    if (m->listed) sh_ms_list_free(&m->list);
    free(m->entries);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ sentences */

/* `s` (malloc'd, taken) ended as a sentence: trailing separators dropped,
 * a full stop added when it has none. Returns the sentence (malloc'd). */
static char *mu_sentence_end(char *s)
{
    size_t n;
    char *out;
    if (!s) return NULL;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == ';' || s[n - 1] == ',')) s[--n] = 0;
    if (!n || s[n - 1] == '.' || s[n - 1] == '?' || s[n - 1] == '!') return s;
    out = sh_textf("%s.", s);
    free(s);
    return out;
}

static int mu_starts(const char *s, const char *prefix) { return !_strnicmp(s, prefix, strlen(prefix)); }
static int mu_ends(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !_stricmp(s + n - k, suffix);
}

/* One sentence for an entry's cause (the store's, the builder's or the serving index's): what is wrong and what to
 * do. Causes the window knows the next step for are said here; any other is the raiser's own sentence, shown inside
 * "This mod could not be built: ...". */
static char *mu_plain_cause(const sh_ms_entry *e, const char *cause)
{
    const char *c = cause ? cause : "";
    (void)e;
    if (!c[0]) return NULL;
    if (mu_starts(c, "not built: ")) c += 11;
    if (!strcmp(c, SH_MB_EARLIER))   /* already a plain sentence */
        return sh_text(c);
    if (mu_starts(c, "scripts\\"))
        /* A script's own message names its file and line, what is wrong and what to do. */
        return mu_sentence_end(sh_text(c));
    if (mu_starts(c, "a new mod, not built yet") || mu_starts(c, "not built yet"))
        return sh_text("This mod has not been built yet; press Refresh to build it.");
    if (mu_starts(c, "mod.json is not a valid JSON object")) {
        const char *why = strchr(c, '('), *end = why ? strchr(why, ')') : NULL;
        return sh_textf("Its mod.json is not valid JSON%s%.*s%s; fix it in a text editor and press Refresh.",
                        why && end ? " (" : "", why && end ? (int)(end - why - 1) : 0, why ? why + 1 : "",
                        why && end ? ")" : "");
    }
    /* The builder's own: a mod.json changed since its last build (modbundle.c). */
    if (mu_starts(c, "mod.json was edited after its last build"))
        return sh_text("Its mod.json was changed after it was last built; press Refresh to build it again.");
    if (mu_starts(c, "mod.json: ")) {
        return mu_sentence_end(sh_textf("Its mod.json is not valid JSON (%s); fix it in a text editor and press Refresh", c + 10));
    }
    if (!strcmp(c, "cannot read mod.json"))
        return sh_text("Its mod.json could not be read; close any program that has it open and press Refresh.");
    /* The store's checks of the built files (modstore.c). */
    if (mu_ends(c, " is missing") || mu_ends(c, " is not the size mod.json records") || mu_ends(c, " is missing or unreadable") ||
        mu_ends(c, " does not match mod.json (damaged or edited)")) {
        const char *cut = strstr(c, " is ");
        if (!cut) cut = strstr(c, " does not ");
        return sh_textf("%.*s changed or went away after the mod was built; press Refresh to build it again.",
                        cut ? (int)(cut - c) : 0, c);
    }
    if (strstr(c, "Refresh") || strstr(c, "builds again") || strstr(c, "builds it again"))
        return mu_sentence_end(sh_textf("This mod could not be built: %s", c));
    return sh_textf("This mod could not be built: %s; fix that in its folder and press Refresh.", c);
}

#ifdef SH_MODUI_TESTING
char *sh_modui_test_plain_cause(const char *cause) { return mu_plain_cause(NULL, cause); }
#endif

/* ------------------------------------------------------------------ what a mod carries */

/* A decoded top-level string of the author block's preserved keys (extra),
 * malloc'd, or NULL. */
static char *mu_extra_string(const sh_mb_bundle *b, const char *key)
{
    mpj *x;
    const mpj *v;
    char *out = NULL;
    if (!b->extra) return NULL;
    x = mpj_parse(b->extra, strlen(b->extra));
    v = x ? mpj_get(x, key) : NULL;
    if (v && v->type == MPJ_STR) out = sh_mm_decode(v->text);
    mpj_free(x);
    return out;
}

/* The display name of a folder: its build's name, else (a folder whose build
 * serves nothing, as a refused first build) the "name" its mod.json gives when
 * that file parses, else "" so the tab names the folder. malloc'd or NULL. */
static char *mu_folder_name(const sh_ms_entry *e)
{
    char *path;
    unsigned char *text;
    size_t len = 0;
    mpj *x;
    const mpj *v;
    char *out = NULL;
    if (e->loaded) return e->bundle.name && e->bundle.name[0] ? _strdup(e->bundle.name) : NULL;
    if (!e->folder || !(path = sh_textf("%s\\mod.json", e->folder))) return NULL;
    text = sh_mb_read_file(path, 1u << 20, &len);
    free(path);
    x = text ? mpj_parse((const char *)text, len) : NULL;
    v = x && x->type == MPJ_OBJ ? mpj_get(x, "name") : NULL;
    if (v && v->type == MPJ_STR) out = sh_mm_decode(v->text);
    if (out && !out[0]) { free(out); out = NULL; }
    mpj_free(x);
    free(text);
    return out;
}

/* The text of a "#str_" key in a bundle's strings.en (keys omit the prefix,
 * matched case-insensitively), malloc'd, or NULL. */
static char *mu_bundle_text(const sh_mb_bundle *b, const char *tag)
{
    mpj *x;
    const mpj *s, *en;
    char *out = NULL, *own;
    int i;
    if (!b->extra || !tag) return NULL;
    if (!_strnicmp(tag, "#str_", 5)) tag += 5;
    /* the bundle holds a key of the mod's own as the engine names it (<salt>_<key>, modauthor.h); a decl in the
     * folder writes it as the author does */
    own = b->id[0] ? sh_textf("%s_%s", b->id, tag) : NULL;
    x = mpj_parse(b->extra, strlen(b->extra));
    s = x ? mpj_get(x, "strings") : NULL;
    en = s && s->type == MPJ_OBJ ? mpj_get(s, "en") : NULL;
    for (i = 0; en && en->type == MPJ_OBJ && i < en->n && !out; i++)
        if ((!_stricmp(en->keys[i], tag) || (own && !_stricmp(en->keys[i], own))) && en->items[i] &&
            en->items[i]->type == MPJ_STR)
            out = sh_mm_decode(en->items[i]->text);
    mpj_free(x);
    free(own);
    return out;
}

/* The first `field = "value"` of a decl text, malloc'd, or NULL. */
static char *mu_decl_field(const char *text, const char *field)
{
    size_t fl = strlen(field);
    const char *p = text;
    while (p && (p = strstr(p, field)) != NULL) {
        const char *q = p + fl, *end;
        int word = p == text || !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || p[-1] == '_');
        p = q;
        if (!word) continue;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') continue;
        end = strchr(++q, '"');
        if (!end) return NULL;
        {
            char *out = (char *)malloc((size_t)(end - q) + 1);
            if (!out) return NULL;
            memcpy(out, q, (size_t)(end - q));
            out[end - q] = 0;
            return out;
        }
    }
    return NULL;
}

static char *mu_strdup(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s ? s : "", n + 1);
    return d;
}

/* The editor rows of entry i: each snapEditorEntityDef file's display name (its displayNameTag through the bundle's
 * strings, then any installed mod's, else the decl name) and the entityDef a placed entity of it inherits. */
static void mu_entry_rows(mu_model *m, size_t i)
{
    mu_entry *u = &m->entries[i];
    const sh_ms_entry *e = u->e;
    size_t k, q;
    if (!e->loaded || !u->editor_rows) return;
    u->row_names = (char **)calloc((size_t)u->editor_rows, sizeof(char *));
    u->row_defs = (char **)calloc((size_t)u->editor_rows, sizeof(char *));
    if (!u->row_names || !u->row_defs) return;
    for (k = 0; k < e->bundle.file_count && u->row_count < u->editor_rows; k++) {
        const sh_mb_file *f = &e->bundle.files[k];
        char *path, *text = NULL, *tag = NULL, *def = NULL, *name = NULL, *c;
        if (!f->ptype || _stricmp(f->ptype, "snapEditorEntityDef") || f->cls == SH_MB_CLASS_EDITOR) continue;
        if (!f->stock && (path = sh_textf("%s\\%s", e->folder, f->path)) != NULL) {
            for (c = path; *c; c++) if (*c == '/') *c = '\\';
            text = (char *)sh_mb_read_file(path, (size_t)-1, NULL);
            free(path);
        }
        if (text) {
            tag = mu_decl_field(text, "displayNameTag");
            def = mu_decl_field(text, "entityDef");
        }
        if (tag) name = mu_bundle_text(&e->bundle, tag);
        for (q = 0; tag && !name && q < m->list.count; q++)
            if (q != i && m->list.items[q].loaded) name = mu_bundle_text(&m->list.items[q].bundle, tag);
        if (!name) {
            /* The decl's own name, without the mod's namespace (no salt in a sentence). */
            const char *own = f->pname ? f->pname : f->request;
            name = sh_mb_strip_key(own);
            if (!name) name = mu_strdup(own);
        }
        u->row_names[u->row_count] = name;
        /* The entityDef as the engine names it (what a placed entity's inherit spells: "entities/imp_copy" in the
         * author's spelling is "mods/<salt>/imp_copy"), so mu_row_name_for compares one spelling. */
        if (def && e->salt[0]) {
            char *engine = sh_ma_engine_name(e->salt, def, strlen(def));
            if (engine) { free(def); def = engine; }
        }
        u->row_defs[u->row_count] = def ? def : mu_strdup("");
        u->row_count++;
        free(tag);
        free(text);
    }
}

/* The display name of the editor row whose entityDef is `inherit`, or NULL. */
static const char *mu_row_name_for(const mu_model *m, const char *inherit)
{
    size_t i;
    int k;
    if (!inherit || !inherit[0]) return NULL;
    for (i = 0; i < m->list.count; i++)
        for (k = 0; k < m->entries[i].row_count; k++)
            if (m->entries[i].row_defs[k] && !_stricmp(m->entries[i].row_defs[k], inherit)) return m->entries[i].row_names[k];
    return NULL;
}

/* The save's labels (modmanifest.h sh_modmanifest_set_labeler): each placed name's Toybox row name, as the Mods tab
 * shows it, from the installed mods. */
static int mu_model_build(mu_model *m);
static void mu_labeler(const char *const *placed, size_t n, char **labels)
{
    mu_model m;
    size_t k;
    if (!n || !mu_model_build(&m)) { if (n) mu_model_free(&m); return; }
    for (k = 0; k < n; k++) {
        const char *name = mu_row_name_for(&m, placed[k]);
        if (name && name[0]) labels[k] = mu_strdup(name);
    }
    mu_model_free(&m);
}

/* The carried files' hashes of a loaded entry, sorted (the same-files hint). */
static int mu_blob_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static const char **mu_blobs(const sh_ms_entry *e, size_t *n)
{
    const char **v;
    size_t k;
    *n = 0;
    if (!e->loaded || !e->bundle.file_count) return NULL;
    v = (const char **)malloc(e->bundle.file_count * sizeof *v);
    if (!v) return NULL;
    for (k = 0; k < e->bundle.file_count; k++)
        if (!e->bundle.files[k].stock && e->bundle.files[k].blob[0]) v[(*n)++] = e->bundle.files[k].blob;
    if (*n > 1) qsort(v, *n, sizeof *v, mu_blob_cmp);
    return v;
}

/* For each root mod, the other root mod whose files include the most of its carried files at equal bytes, when that is
 * at least half of them (the scan's blocks; nothing is hashed). Two near-identical mods stay two mods: the tab
 * only says so. */
static void mu_same_files(mu_model *m)
{
    size_t n = m->list.count, i, k, q;
    const char ***blobs = (const char ***)calloc(n ? n : 1, sizeof *blobs);
    size_t *counts = (size_t *)calloc(n ? n : 1, sizeof *counts);
    if (!blobs || !counts) { free((void *)blobs); free(counts); return; }
    for (i = 0; i < n; i++) blobs[i] = mu_blobs(&m->list.items[i], &counts[i]);
    for (i = 0; i < n; i++) {
        unsigned best = 0;
        int at = -1;
        if (!blobs[i] || !counts[i]) continue;
        for (k = 0; k < n; k++) {
            unsigned same = 0;
            if (k == i || !blobs[k] || !counts[k] || !_stricmp(m->list.items[k].salt, m->list.items[i].salt)) continue;
            for (q = 0; q < counts[i]; q++)
                if (bsearch(&blobs[i][q], blobs[k], counts[k], sizeof *blobs[k], mu_blob_cmp)) same++;
            if (same > best) { best = same; at = (int)k; }
        }
        if (at >= 0 && best * 2 >= counts[i]) { m->entries[i].same_as = at; m->entries[i].same_count = best; }
    }
    for (i = 0; i < n; i++) free((void *)blobs[i]);
    free((void *)blobs);
    free(counts);
}

static int mu_model_build(mu_model *m)
{
    size_t i, k;
    memset(m, 0, sizeof *m);
    m->root = sh_ms_root();
    m->root_ok = m->root != NULL;
    m->listed = sh_ms_enumerate(&m->list);
    if (!m->listed) memset(&m->list, 0, sizeof m->list);
    m->entries = (mu_entry *)calloc(m->list.count ? m->list.count : 1, sizeof(mu_entry));
    if (!m->entries) return 0;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        mu_entry *u = &m->entries[i];
        char *why = NULL;
        int st;
        u->e = e;
        u->same_as = -1;
        if (e->loaded)
            for (k = 0; k < e->bundle.file_count; k++) {
                const sh_mb_file *f = &e->bundle.files[k];
                /* A Toybox row the mod adds; an editor tweak of a stock row is not one. */
                if (f->ptype && !_stricmp(f->ptype, "snapEditorEntityDef") && f->cls != SH_MB_CLASS_EDITOR) u->editor_rows++;
                /* An edited copy is counted under edits and an editor tweak under
                 * editor, never as a replaced game file. */
                if (f->shadowed && !f->edited && f->cls != SH_MB_CLASS_EDITOR) u->shadows++;
            }
        if (!e->loaded || e->refused) {
            u->state = "skipped";
            u->cause = mu_plain_cause(e, e->cause[0] ? e->cause : "its mod.json cannot be read");
            continue;
        }
        /* The serving index decides what is served; the store's own check
         * (sizes, the last verification) explains a build the index lacks. */
        st = sh_modserve_bundle_state(e->bundle.hash, &why);
        if (st < 0) {
            u->state = "skipped";
            u->cause = mu_plain_cause(e, why && why[0] ? why : e->cause);
            if (!u->cause) u->cause = sh_text("Snapmap+ could not use this mod. Press Refresh; if it stays, use Send feedback.");
            free(why);
            continue;
        }
        free(why);
        if (st == 0) {
            u->state = sh_ms_entry_usable(e) ? "unindexed" : "skipped";
            if (sh_ms_entry_usable(e)) u->cause = sh_text("Snapmap+ has not read this mod yet; press Refresh.");
            else u->cause = mu_plain_cause(e, e->cause[0] ? e->cause : "not built yet");
            continue;
        }
        if (!sh_ms_entry_usable(e) && e->cause[0]) u->cause = mu_plain_cause(e, e->cause);
        u->admitted = 1;
        u->state = "active";
    }
    for (i = 0; i < m->list.count; i++) mu_entry_rows(m, i);
    mu_same_files(m);
    return 1;
}

/* ------------------------------------------------------------------ published maps */

/* The record's claims (modclaim.h: a published item's claim of its units, with the namespaces they serve) and links
 * (modpublish: which saved map an item was published from). No secret is ever stored. */
typedef struct mu_claim {
    char *ref;                   /* malloc'd */
    long long updated;
    char (*salts)[SH_MODMANIFEST_SALT_CAP];
    int count;
} mu_claim;

typedef struct mu_link { char *item, *cloud, *local; long long at; } mu_link;   /* each malloc'd */

typedef struct mu_record {
    int claims_read;             /* the claims section was readable (absent counts as empty) */
    mu_claim *claims;
    size_t claim_count;
    mu_link *links;
    size_t link_count;
} mu_record;

static void mu_record_free(mu_record *r)
{
    size_t i;
    for (i = 0; i < r->claim_count; i++) { free(r->claims[i].salts); free(r->claims[i].ref); }
    for (i = 0; i < r->link_count; i++) { free(r->links[i].item); free(r->links[i].cloud); free(r->links[i].local); }
    free(r->claims);
    free(r->links);
    memset(r, 0, sizeof *r);
}

/* A string member, decoded and whole (malloc'd; "" when absent). */
static char *mu_copy_str(const mpj *v)
{
    char *d = v && v->type == MPJ_STR ? sh_mm_decode(v->text) : NULL;
    return d ? d : sh_text("");
}

static void mu_record_load(const mu_model *m, mu_record *r)
{
    char *text = NULL;
    mpj *x;
    int i, k, got;
    memset(r, 0, sizeof *r);
    if (!m->root_ok) return;
    got = sh_mb_record_get(m->root, MODCLAIM_CLAIMS_SECTION, &text);
    r->claims_read = got >= 0;
    x = got == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    free(text);
    text = NULL;
    if (x && x->type == MPJ_OBJ && x->n) {
        r->claims = (mu_claim *)calloc((size_t)x->n, sizeof *r->claims);
        for (i = 0; r->claims && i < x->n; i++) {
            const mpj *c = x->items[i], *h = mpj_get(c, "mods");
            mu_claim *cl = &r->claims[r->claim_count];
            if (!c || c->type != MPJ_OBJ) continue;
            if (!(cl->ref = sh_text(x->keys[i]))) continue;
            cl->updated = mpj_int(mpj_get(c, "at"));
            if (h && h->type == MPJ_ARR && h->n) {
                cl->salts = (char (*)[SH_MODMANIFEST_SALT_CAP])calloc((size_t)h->n, sizeof *cl->salts);
                for (k = 0; cl->salts && k < h->n; k++)
                    if (h->items[k] && h->items[k]->type == MPJ_STR && strlen(h->items[k]->text) == SH_MODMANIFEST_SALT_LEN)
                        strcpy_s(cl->salts[cl->count++], SH_MODMANIFEST_SALT_CAP, h->items[k]->text);
            }
            r->claim_count++;
        }
    }
    mpj_free(x);
    x = sh_mb_record_get(m->root, "links", &text) == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    free(text);
    if (x && x->type == MPJ_OBJ && x->n) {
        r->links = (mu_link *)calloc((size_t)x->n, sizeof *r->links);
        for (i = 0; r->links && i < x->n; i++) {
            const mpj *l = x->items[i];
            mu_link *ln = &r->links[r->link_count];
            if (!l || l->type != MPJ_OBJ) continue;
            ln->item = sh_text(x->keys[i]);
            ln->cloud = mu_copy_str(mpj_get(l, "cloud"));
            ln->local = mu_copy_str(mpj_get(l, "local"));
            if (!ln->item || !ln->cloud || !ln->local) { free(ln->item); free(ln->cloud); free(ln->local); continue; }
            ln->at = mpj_int(mpj_get(l, "at"));
            r->link_count++;
        }
    }
    mpj_free(x);
}

/* How many of this computer's published maps claim content of entry i's namespace. */
static unsigned mu_published_maps(const mu_model *m, const mu_record *r, int entry)
{
    size_t i;
    int k;
    unsigned n = 0;
    const sh_ms_entry *e = entry >= 0 && (size_t)entry < m->list.count ? &m->list.items[entry] : NULL;
    if (!e) return 0;
    for (i = 0; i < r->claim_count; i++) {
        int hit = 0;
        for (k = 0; k < r->claims[i].count && !hit; k++) hit = !_stricmp(r->claims[i].salts[k], e->salt);
        n += hit;
    }
    return n;
}

/* ------------------------------------------------------------------ the rows */

static int mu_path_sort(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int mu_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !_stricmp(s + n - k, suffix);
}

static const char *mu_entry_title(const sh_ms_entry *s);

/* ---- what the builder derived */

/* The stock banks the product block's registries "banks" names, each with this process's state: up to 8. A leaf
 * longer than SH_MBI_LEAF_MAX (a Windows file name, modbankinfo.h) names no installed bank. */
static int mu_stock_banks(const sh_mb_bundle *bd, char banks[8][SH_MBI_LEAF_MAX])
{
    mpj *x;
    const mpj *v;
    int k, n = 0;
    if (!bd->registries || !strstr(bd->registries, "\"banks\"")) return 0;
    x = mpj_parse(bd->registries, strlen(bd->registries));
    v = x ? mpj_get(x, "banks") : NULL;
    for (k = 0; v && v->type == MPJ_ARR && k < v->n && n < 8; k++) {
        const mpj *s = v->items[k];
        if (!s || s->type != MPJ_STR || !s->text || strchr(s->text, '\\') || strlen(s->text) >= SH_MBI_LEAF_MAX) continue;
        strcpy_s(banks[n], SH_MBI_LEAF_MAX, s->text);
        n++;
    }
    mpj_free(x);
    return n;
}

/* payload facts.stockBanks: [{bank, state, ms, result}], only when the
 * registries name a bank. */
static void mu_put_stock_banks(mu_buf *b, const sh_mb_bundle *bd)
{
    char banks[8][SH_MBI_LEAF_MAX];
    int n = mu_stock_banks(bd, banks), k;
    if (!n) return;
    mu_puts(b, ",\"stockBanks\":[");
    for (k = 0; k < n; k++) {
        int result = 0;
        double ms = 0;
        int st = sh_mbi_state_get(banks[k], &result, &ms, NULL, 0);
        if (k) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "bank", banks[k], 1);
        mu_kstr(b, "state", sh_mbi_state_word(st), 0);
        mu_knum(b, "ms", ms > 0 ? (unsigned long long)(ms + 0.5) : 0, 0);
        mu_key(b, "result", 0);
        { char num[16]; _snprintf_s(num, sizeof num, _TRUNCATE, "%d", result); mu_puts(b, num); }
        mu_putn(b, "}", 1);
    }
    mu_putn(b, "]", 1);
}

/* What the builder derived from the files (payload "facts"): the classes, the editor tweaks, every replacement (an
 * edited copy or not), the game files the mod edits with operation counts, and the product block's "registries". */
typedef struct mu_edits_ctx { mu_buf *b; int first; unsigned files; } mu_edits_ctx;

static void mu_put_edit_file(void *context, const char *file, const sh_dt_counts *c)
{
    mu_edits_ctx *e = (mu_edits_ctx *)context;
    if (!e->first) mu_putn(e->b, ",", 1);
    e->first = 0;
    e->files++;
    mu_putn(e->b, "{", 1);
    mu_kstr(e->b, "file", file, 1);
    mu_knum(e->b, "ops", (long long)sh_dt_counts_total(c), 0);
    mu_knum(e->b, "set", c->set, 0);
    mu_knum(e->b, "remove", c->remove, 0);
    mu_knum(e->b, "list", c->list, 0);
    mu_knum(e->b, "keyed", c->keyed, 0);
    mu_knum(e->b, "bag", c->bag, 0);
    mu_knum(e->b, "drop", c->drop, 0);
    mu_putn(e->b, "}", 1);
}

static void mu_put_facts(mu_buf *b, const sh_mb_bundle *bd)
{
    size_t k;
    int first = 1;
    mu_edits_ctx ec;
    mu_puts(b, ",\"facts\":{");
    mu_knum(b, "adds", bd->classes.adds, 1);
    mu_knum(b, "borrows", bd->classes.borrows, 0);
    mu_knum(b, "surfaces", bd->classes.surfaces, 0);
    mu_knum(b, "editor", bd->classes.editor, 0);
    mu_knum(b, "replaces", bd->classes.replaces, 0);   /* edited copies are classes.edits, not here */
    /* Editor tweaks and nothing else (sh_mb_editor_only, the rule mapAddMod refuses by): a map
     * never records such a folder, so the tab offers no map action and no player needs it. */
    mu_kbool(b, "editorOnly", sh_mb_editor_only(bd), 0);
    mu_puts(b, ",\"editorTweaks\":[");
    for (k = 0; k < bd->file_count; k++) {
        if (bd->files[k].cls != SH_MB_CLASS_EDITOR) continue;
        if (!first) mu_putn(b, ",", 1);
        mu_str(b, bd->files[k].request);
        first = 0;
    }
    mu_puts(b, "],\"replacements\":[");
    first = 1;
    for (k = 0; k < bd->file_count; k++) {
        const sh_mb_file *f = &bd->files[k];
        int edited = 0;
        if (f->cls != SH_MB_CLASS_REPLACE) continue;
        edited = f->edited;
        if (!first) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "path", f->request, 1);
        mu_kbool(b, "edits", edited, 0);
        mu_putn(b, "}", 1);
        first = 0;
    }
    mu_puts(b, "],\"edits\":[");
    ec.b = b;
    ec.first = 1;
    ec.files = 0;
    if (bd->edits) (void)sh_dt_edits_files(bd->edits, strlen(bd->edits), mu_put_edit_file, &ec);
    mu_puts(b, "],\"registries\":");
    mu_puts(b, bd->registries && bd->registries[0] == '{' ? bd->registries : "{}");
    mu_put_stock_banks(b, bd);
    mu_putn(b, "}", 1);
}

/* What the mod borrows from the game: its game files read from the install
 * (borrows, and the game's own files it brings into view), each named by the
 * game file it reads ("source"), every one of them. */
static void mu_put_borrows(mu_buf *b, const sh_mb_bundle *bd)
{
    size_t k;
    unsigned count = 0;
    mu_puts(b, ",\"borrows\":{\"paths\":[");
    for (k = 0; k < bd->file_count; k++) {
        const sh_mb_file *f = &bd->files[k];
        if (f->cls != SH_MB_CLASS_BORROW && f->cls != SH_MB_CLASS_SURFACE) continue;
        if (count++) mu_putn(b, ",", 1);
        mu_str(b, f->stock_source && f->stock_source[0] ? f->stock_source : f->request);
    }
    mu_putn(b, "]", 1);
    mu_knum(b, "count", count, 0);
    mu_putn(b, "}", 1);
}

/* One row of the tab: one mod folder (payload v4, docs/webview-ui.md). */
static void mu_put_row(mu_buf *b, const mu_model *m, const mu_record *rec, const mu_entry *u)
{
    const sh_ms_entry *e = u->e;
    const sh_mb_bundle *bd = &e->bundle;
    int ok = u->admitted && !u->cause;
    size_t k;
    int i;
    mu_putn(b, "{", 1);
    mu_kstr(b, "key", e->name, 1);
    mu_kstr(b, "salt", e->salt, 0);
    {
        char *name = mu_folder_name(e);
        mu_kstr(b, "name", name ? name : "", 0);
        free(name);
    }
    mu_kstr(b, "author", e->loaded && bd->author ? bd->author : "", 0);
    mu_knum(b, "size", e->loaded ? bd->total_size : 0, 0);
    mu_knum(b, "files", e->loaded ? bd->file_count - bd->stock_count : 0, 0);
    mu_kstr(b, "state", ok ? "ready" : "attention", 0);
    mu_kstr(b, "cause", ok || !u->cause ? "" : u->cause, 0);
    mu_knum(b, "editorRows", (unsigned)u->editor_rows, 0);
    mu_kstr(b, "folder", mu_leaf(e->folder), 0);
    {
        /* The organizing folder it sits in at the root of mods\ ("" at the
         * top): its name without the last part. */
        const char *cut = strrchr(e->name, '\\');
        char *group = sh_textn(e->name, cut ? (size_t)(cut - e->name) : 0);
        mu_kstr(b, "group", group ? group : "", 0);
        free(group);
    }
    {
        char *desc = e->loaded ? mu_extra_string(bd, "description") : NULL;
        mu_kstr(b, "description", desc ? desc : "", 0);
        free(desc);
    }
    mu_puts(b, ",\"requirements\":");
    /* The builder's canonical {"commands":{...},"cvars":{...}}; "cvars" is always there. */
    mu_puts(b, e->loaded && bd->requirements && strstr(bd->requirements, "\"cvars\"") ? bd->requirements : "{\"cvars\":{}}");
    mu_puts(b, ",\"editorRowNames\":[");
    for (i = 0; i < u->row_count; i++) {
        if (i) mu_putn(b, ",", 1);
        mu_str(b, u->row_names[i]);
    }
    mu_putn(b, "]", 1);
    {
        /* The SnapMap modules it adds (its own snapModule decls under map-modules/): with its Toybox rows, what an
         * author can place of it. A mod with neither is used by a map as a whole. */
        unsigned modules = 0;
        for (k = 0; e->loaded && k < bd->file_count; k++)
            if (bd->files[k].is_module && !bd->files[k].stock && !bd->files[k].shadowed) modules++;
        mu_knum(b, "modules", modules, 0);
    }
    if (rec->claims_read) mu_knum(b, "publishedMaps", mu_published_maps(m, rec, (int)(u - m->entries)), 0);
    if (e->loaded) {
        mu_put_facts(b, bd);
        mu_put_borrows(b, bd);
    }
    /* The builder's warnings, in plain words: what the page shows beside the row. */
    mu_key(b, "warnings", 0);
    mu_putn(b, "[", 1);
    for (k = 0; e->loaded && k < bd->warning_list_count; k++) {
        char *plain = mu_sentence_end(sh_text(bd->warning_list[k]));
        if (plain && plain[0] && islower((unsigned char)plain[0])) plain[0] = (char)toupper((unsigned char)plain[0]);
        if (k) mu_putn(b, ",", 1);
        mu_str(b, plain ? plain : bd->warning_list[k]);
        free(plain);
    }
    mu_putn(b, "]", 1);
    /* Its scripts: what the builder's static pass read ("scripts", the block's member as
     * built), and how they ran in the current or last Play ("scriptRun"). */
    if (e->loaded && bd->scripts && bd->scripts[0] == '{') {
        mu_puts(b, ",\"scripts\":");
        mu_puts(b, bd->scripts);
    }
    mu_put_script_run(b, "scriptRun", e->salt);
    mu_put_restart_banks(b, e->salt);
    if (u->same_as >= 0) {
        /* Informational: another of the player's mods carries the same files. */
        const sh_ms_entry *o = &m->list.items[u->same_as];
        char *name = mu_folder_name(o);
        mu_puts(b, ",\"sameFilesAs\":{");
        mu_kstr(b, "name", name ? name : mu_leaf(o->folder), 1);
        mu_kstr(b, "folder", o->name, 0);
        mu_knum(b, "count", u->same_count, 0);
        mu_putn(b, "}", 1);
        free(name);
    }
    mu_putn(b, "}", 1);
}

/* ------------------------------------------------------------------ console lines */

typedef struct mu_out { sh_modui_line_fn fn; void *context; } mu_out;

static void mu_say(const mu_out *o, const char *fmt, ...)
{
    char *body, *line;
    va_list ap;
    if (!o || !o->fn) return;
    va_start(ap, fmt);
    body = sh_vtextf(fmt, ap);
    va_end(ap);
    line = sh_textf("MODS: doctor: %s", body ? body : "(out of memory)");
    free(body);
    o->fn(o->context, line ? line : "MODS: doctor: (out of memory)");
    free(line);
}

/* ------------------------------------------------------------------ shadows and conflicts */

typedef struct mu_path { const char *request; int entry; int shadow; int conflict; int edit; } mu_path;

static int mu_path_cmp(const void *a, const void *b)
{
    const mu_path *x = (const mu_path *)a, *y = (const mu_path *)b;
    int c = strcmp(x->request, y->request);
    return c ? c : x->entry - y->entry;
}

/* Every game file an admitted mod replaces and every path nobody serves (two mods provide it with different bytes),
 * grouped by path: conflicts first (with the serving copy and why), then replaced game files, then edited copies.
 * An edited copy is compared with the game's text at build and its game file is generated per map, never served,
 * so it is neither a replacement nor a conflict. Mods are named by their folders. */
static void mu_print_shadows(const mu_out *o, const mu_model *m)
{
    mu_path *paths = NULL;
    size_t n = 0, cap = 0, i, k, printed = 0, groups = 0;
    unsigned replaced = 0, conflicts = 0, edit_sources = 0, edit_files = 0;
    int pass;
    for (i = 0; i < m->list.count; i++) {
        const mu_entry *u = &m->entries[i];
        if (!u->admitted) continue;
        for (k = 0; k < u->e->bundle.file_count; k++) {
            const sh_mb_file *f = &u->e->bundle.files[k];
            int conflict = !f->edited &&
                           (sh_modserve_conflict_info(f->request, NULL) || !sh_modserve_lookup_indexed(f->request, NULL));
            if (!f->shadowed && !conflict && !f->edited) continue;
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 64;
                mu_path *np = (mu_path *)realloc(paths, nc * sizeof *np);
                if (!np) { free(paths); mu_say(o, "game files: out of memory"); return; }
                paths = np;
                cap = nc;
            }
            paths[n].request = f->request;
            paths[n].entry = (int)i;
            paths[n].shadow = f->shadowed;
            paths[n].conflict = conflict;
            paths[n].edit = f->edited;
            if (f->edited) edit_sources++;
            n++;
        }
    }
    if (n > 1) qsort(paths, n, sizeof *paths, mu_path_cmp);
    /* pass 0: conflicts; 1: replaced game files; 2: edited copies. A path one
     * mod edits and another serves whole is both. */
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < n;) {
            size_t j = i, q;
            int conflict = 0, served, members = 0;
            char *who, *here = NULL;
            mu_buf wb = { 0 };
            sh_modserve_hit hit, open_hit;
            while (j < n && !strcmp(paths[j].request, paths[i].request)) {
                if (paths[j].edit == (pass == 2)) {
                    conflict |= paths[j].conflict;
                    members++;
                }
                j++;
            }
            if (!members || (pass < 2 && conflict != (pass == 0))) { i = j; continue; }
            for (q = i; q < j; q++) {
                if (paths[q].edit != (pass == 2)) continue;
                if (wb.len) mu_puts(&wb, ", ");
                mu_puts(&wb, "mods\\");
                mu_puts(&wb, m->list.items[paths[q].entry].name);
            }
            who = wb.p && !wb.oom ? wb.p : "";
            groups++;
            if (pass == 2) {
                edit_files++;
                mu_say(o, "edited game file %s: by %s; compared with the game's text when built, and written for each map "
                       "that uses the mod (the copy itself is never served)", paths[i].request, who);
                printed++;
                free(wb.p);
                i = j;
                continue;
            }
            if (conflict) conflicts++;
            else replaced++;
            served = sh_modserve_lookup_indexed(paths[i].request, &hit) ? mu_find_hash(m, hit.bundle_hash) : -1;
            /* With map scope on: which provider serves the path for the open
             * map (any provider the map uses, not only the winner). */
            if (sh_modserve_scope_enabled()) {
                int open = sh_modserve_lookup(paths[i].request, &open_hit) ? mu_find_hash(m, open_hit.bundle_hash) : -1;
                if (open >= 0) here = sh_textf("; for the open map: mods\\%s", m->list.items[open].name);
                else here = sh_textf("; for the open map: the game's own file (%s)",
                                     sh_modserve_scope_map_open() ? "the map uses none of these mods" : "no map is open");
            }
            if (conflict) {
                sh_modserve_conflict ci;
                int known = sh_modserve_conflict_info(paths[i].request, &ci);
                if (!known || served < 0)
                    mu_say(o, "conflict %s: nothing serves it; from %s", paths[i].request, who);
                else if (ci.withdrawn)
                    mu_say(o, "conflict %s: the game's own file serves (%s); from %s", paths[i].request, ci.reason, who);
                else if (ci.composed)
                    mu_say(o, "conflict %s: combined, every mod's list entries are served (%s); from %s%s",
                           paths[i].request, ci.reason, who, here ? here : "");
                else
                    mu_say(o, "conflict %s: mods\\%s serves it (%s); from %s%s", paths[i].request, m->list.items[served].name,
                           ci.reason, who, here ? here : "");
            } else
                mu_say(o, "replaced game file %s: by %s; served by %s%s%s", paths[i].request, who,
                       served >= 0 && !hit.is_stock ? "mods\\" : "", served >= 0 && !hit.is_stock ? m->list.items[served].name
                                                                    : "the game's own file", here ? here : "");
            printed++;
            free(here);
            free(wb.p);
            i = j;
        }
    if (groups > printed) mu_say(o, "%zu more game file(s) not printed", groups - printed);
    mu_say(o, "%u conflict(s), %u replaced game file(s), %u edited copy(ies) of %u game file(s) (never served, so neither "
              "a replacement nor a conflict)", conflicts, replaced, edit_sources, edit_files);
    free(paths);
}

/* ------------------------------------------------------------------ the open map */

typedef struct mu_names {
    char **items;
    size_t count, cap;
    unsigned *slots;          /* open addressing: index + 1, 0 empty */
    size_t mask;
    int oom;
} mu_names;

static unsigned mu_hash_ci(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void mu_names_add(void *context, const char *name)
{
    mu_names *n = (mu_names *)context;
    size_t len, at;
    char *copy;
    if (!n || n->oom || !name || !name[0]) return;
    len = strlen(name);
    if (!n->slots || (n->count + 1) * 2 > n->mask + 1) {
        size_t size = n->slots ? (n->mask + 1) * 2 : 256, i;
        unsigned *ns = (unsigned *)calloc(size, sizeof *ns);
        if (!ns) { n->oom = 1; return; }
        for (i = 0; i < n->count; i++) {
            size_t p = mu_hash_ci(n->items[i]) & (size - 1);
            while (ns[p]) p = (p + 1) & (size - 1);
            ns[p] = (unsigned)i + 1;
        }
        free(n->slots);
        n->slots = ns;
        n->mask = size - 1;
    }
    at = mu_hash_ci(name) & n->mask;
    while (n->slots[at]) {
        if (!_stricmp(n->items[n->slots[at] - 1], name)) return;
        at = (at + 1) & n->mask;
    }
    if (n->count == n->cap) {
        size_t nc = n->cap ? n->cap * 2 : 256;
        char **ni = (char **)realloc(n->items, nc * sizeof *ni);
        if (!ni) { n->oom = 1; return; }
        n->items = ni;
        n->cap = nc;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) { n->oom = 1; return; }
    memcpy(copy, name, len + 1);
    n->items[n->count] = copy;
    n->slots[at] = (unsigned)n->count + 1;
    n->count++;
}

static void mu_names_free(mu_names *n)
{
    size_t i;
    for (i = 0; i < n->count; i++) free(n->items[i]);
    free(n->items);
    free(n->slots);
    memset(n, 0, sizeof *n);
}

/* One bundle of the placed closure. */
typedef struct mu_closure_bundle {
    char hash[SH_MODSERVE_HASH_HEX + 1];
    char id[SH_MB_ID_MAX + 1];
    unsigned long long size;
    char **names;             /* every placed name it provides (malloc'd) */
    int name_count, name_cap;
    int via_needs;            /* reached only through another bundle's needs */
} mu_closure_bundle;

typedef struct mu_closure {
    mu_closure_bundle *items;
    size_t count, cap;
    int oom;
} mu_closure;

static void mu_closure_visit(void *context, const char *bundle_id, const char *bundle_hash, unsigned long long size,
                             const char *name)
{
    mu_closure *c = (mu_closure *)context;
    mu_closure_bundle *cb = NULL;
    size_t i;
    if (!c || c->oom || !bundle_hash) return;
    for (i = 0; i < c->count; i++)
        if (!strcmp(c->items[i].hash, bundle_hash)) { cb = &c->items[i]; break; }
    if (!cb) {
        if (c->count == c->cap) {
            size_t nc = c->cap ? c->cap * 2 : 16;
            mu_closure_bundle *ni = (mu_closure_bundle *)realloc(c->items, nc * sizeof *ni);
            if (!ni) { c->oom = 1; return; }
            c->items = ni;
            c->cap = nc;
        }
        cb = &c->items[c->count++];
        memset(cb, 0, sizeof *cb);
        strncpy_s(cb->hash, sizeof cb->hash, bundle_hash, _TRUNCATE);
        strncpy_s(cb->id, sizeof cb->id, bundle_id ? bundle_id : "", _TRUNCATE);
        cb->size = size;
        cb->via_needs = name == NULL;
    }
    if (!name) return;
    cb->via_needs = 0;
    if (cb->name_count == cb->name_cap) {
        int nc = cb->name_cap ? cb->name_cap * 2 : 8;
        char **nn = (char **)realloc(cb->names, (size_t)nc * sizeof *nn);
        if (!nn) { c->oom = 1; return; }
        cb->names = nn;
        cb->name_cap = nc;
    }
    {
        size_t len = strlen(name);
        cb->names[cb->name_count] = (char *)malloc(len + 1);
        if (cb->names[cb->name_count]) memcpy(cb->names[cb->name_count], name, len + 1);
        else { c->oom = 1; return; }
    }
    cb->name_count++;
}

static void mu_closure_free(mu_closure *c)
{
    size_t i;
    int k;
    for (i = 0; i < c->count; i++) {
        for (k = 0; k < c->items[i].name_count; k++) free(c->items[i].names[k]);
        free(c->items[i].names);
    }
    free(c->items);
    memset(c, 0, sizeof *c);
}

/* A mod the open map uses: a manifest row, a bundle its placed names resolve
 * to now, or both. */
typedef struct mu_mapmod {
    char hash[SH_MODSERVE_HASH_HEX + 1];
    char id[SH_MB_ID_MAX + 1];
    unsigned long long size;
    int in_rows;              /* the current manifest has a row for it */
    int attached;             /* that row is map-level: the map uses the mod without placing it */
    int in_closure;           /* the placed names resolve to it now */
    int named;                /* reached by a placed name (not only through another mod's needs) */
    int *uids;                /* the entities that use it */
    size_t uid_count, uid_cap;
    int modules;              /* placed modules (Blueprint instances) that use it */
    /* Its row's declared build when the editor serves the row through this
     * (installed) build of its salt (an editor load that substituted it). */
    char declared[SH_MODMANIFEST_HASH_CAP];
    int pinned;               /* the save writes its row with units (a published or downloaded map's content) */
    const sh_modmanifest_row *row;   /* the row the save writes for it (in mu_map.save), or NULL */
} mu_mapmod;

/* A (placed name, mod) match of the closure. */
typedef struct mu_hit { char *name; int mod; } mu_hit;

/* The current map's view: manifest rows (its load-time record, less the rows
 * the tab removed since), the bundles its placed names resolve to now, and,
 * with the map's JSON, its entities, wires and references, and its details. */
typedef struct mu_map {
    sh_modmanifest_list rows;
    /* The rows a save would write now (sh_modmanifest_rows_for over the live placed names): the mods the map uses
     * are exactly these, so the tab never says a map uses a mod the save would drop, or misses one it would add. */
    sh_modmanifest_list save;
    int have_save;
    mu_closure closure;
    int entities;             /* entities read, -1 no editor map, -2 not available */
    size_t names;
    int have_mm;              /* mm holds the parsed map */
    sh_mm_map mm;
    mu_mapmod *mods;
    size_t mod_count, mod_cap;
    mu_hit *hits;
    size_t hit_count, hit_cap;
    int oom;
    int have_info;            /* info holds the editor's saved-map ids and tag */
    sh_modui_map_info info;
    char *name;               /* the saved map's name (malloc'd), NULL when unknown */
    long long saved;          /* the last save, Unix seconds; 0 when unknown */
    const mu_link *link;      /* the record's link of this map's published item, or NULL */
    const mu_claim *claim;    /* that item's claim, or NULL */
} mu_map;

static int mu_mapmod_add(mu_map *mp, const char *hash, const char *id, unsigned long long size)
{
    size_t i;
    for (i = 0; i < mp->mod_count; i++) if (!strcmp(mp->mods[i].hash, hash)) return (int)i;
    if (mp->mod_count == mp->mod_cap) {
        size_t nc = mp->mod_cap ? mp->mod_cap * 2 : 8;
        mu_mapmod *nm = (mu_mapmod *)realloc(mp->mods, nc * sizeof *nm);
        if (!nm) { mp->oom = 1; return -1; }
        mp->mods = nm;
        mp->mod_cap = nc;
    }
    memset(&mp->mods[mp->mod_count], 0, sizeof *mp->mods);
    strncpy_s(mp->mods[mp->mod_count].hash, sizeof mp->mods[0].hash, hash, _TRUNCATE);
    strncpy_s(mp->mods[mp->mod_count].id, sizeof mp->mods[0].id, id ? id : "", _TRUNCATE);
    mp->mods[mp->mod_count].size = size;
    return (int)mp->mod_count++;
}

static void mu_mapmod_uid(mu_map *mp, int mod, int uid)
{
    mu_mapmod *x = &mp->mods[mod];
    size_t i;
    for (i = 0; i < x->uid_count; i++) if (x->uids[i] == uid) return;
    if (x->uid_count == x->uid_cap) {
        size_t nc = x->uid_cap ? x->uid_cap * 2 : 16;
        int *nu = (int *)realloc(x->uids, nc * sizeof *nu);
        if (!nu) { mp->oom = 1; return; }
        x->uids = nu;
        x->uid_cap = nc;
    }
    x->uids[x->uid_count++] = uid;
}

typedef struct mu_visit { mu_closure *closure; mu_map *mp; } mu_visit;

static void mu_map_visit(void *context, const char *bundle_id, const char *bundle_hash, unsigned long long size,
                         const char *name)
{
    mu_visit *v = (mu_visit *)context;
    mu_map *mp = v->mp;
    int mod;
    mu_closure_visit(v->closure, bundle_id, bundle_hash, size, name);
    if (!bundle_hash || mp->oom) return;
    mod = mu_mapmod_add(mp, bundle_hash, bundle_id, size);
    if (mod < 0) return;
    mp->mods[mod].in_closure = 1;
    if (!name) return;
    mp->mods[mod].named = 1;
    if (mp->hit_count == mp->hit_cap) {
        size_t nc = mp->hit_cap ? mp->hit_cap * 2 : 64;
        mu_hit *nh = (mu_hit *)realloc(mp->hits, nc * sizeof *nh);
        if (!nh) { mp->oom = 1; return; }
        mp->hits = nh;
        mp->hit_cap = nc;
    }
    mp->hits[mp->hit_count].name = mu_strdup(name);
    mp->hits[mp->hit_count].mod = mod;
    if (!mp->hits[mp->hit_count].name) { mp->oom = 1; return; }
    mp->hit_count++;
}

/* Which mods each entity and module uses: those whose placed names include a name a mod provides (the save closure's
 * own matching) or, for a manifest row the closure does not reach (its mod is not installed), a name the row recorded. */
static void mu_map_attribute(mu_map *mp)
{
    size_t i, k, h, r;
    for (i = 0; i < mp->mm.ent_count; i++) {
        const sh_mm_entity *e = &mp->mm.ents[i];
        for (k = 0; k < e->name_count; k++)
            for (h = 0; h < mp->hit_count; h++)
                if (!_stricmp(e->names[k], mp->hits[h].name)) mu_mapmod_uid(mp, mp->hits[h].mod, e->uid);
    }
    for (i = 0; i < mp->mm.module_count; i++)
        for (h = 0; h < mp->hit_count; h++)
            if (!_stricmp(mp->mm.modules[i], mp->hits[h].name)) { mp->mods[mp->hits[h].mod].modules++; break; }
    for (r = 0; r < mp->rows.count; r++) {
        const sh_modmanifest_row *row = &mp->rows.rows[r];
        int mod = -1;
        for (h = 0; h < mp->mod_count && mod < 0; h++) if (!strcmp(mp->mods[h].hash, row->build)) mod = (int)h;
        if (mod < 0 || mp->mods[mod].in_closure) continue;
        for (i = 0; i < mp->mm.ent_count; i++) {
            const sh_mm_entity *e = &mp->mm.ents[i];
            for (k = 0; k < e->name_count; k++) {
                size_t q;
                int hit = 0;
                for (q = 0; q < row->name_count && !hit; q++) hit = !_stricmp(e->names[k], row->names[q]);
                if (hit) { mu_mapmod_uid(mp, mod, e->uid); break; }
            }
        }
        for (i = 0; i < mp->mm.module_count; i++) {
            size_t q;
            for (q = 0; q < row->name_count; q++)
                if (!_stricmp(mp->mm.modules[i], row->names[q])) { mp->mods[mod].modules++; break; }
        }
    }
}

/* FindFirstFileW on a UTF-8 pattern. */
static HANDLE mu_find_first(const char *pattern, WIN32_FIND_DATAW *fd)
{
    wchar_t *w = sh_mb_wpath(pattern);
    HANDLE h = w ? FindFirstFileW(w, fd) : INVALID_HANDLE_VALUE;
    free(w);
    return h;
}

/* ---- the saved map's details */

/* The local save's game.details (saved_maps.h): its metadata's name and its modified time. 1 when found. */
static int mu_saved_details(const char *local_id, char **name, long long *saved)
{
    char *text;
    const char *c;
    size_t i;
    *name = NULL;
    *saved = 0;
    if ((text = sh_saved_map_details(local_id)) == NULL) return 0;
    /* Lines: checksum=, itemId=, metadata=<JSON>, modified=<epoch ms>. */
    for (c = text; c && *c;) {
        const char *end = strchr(c, '\n');
        size_t len = end ? (size_t)(end - c) : strlen(c);
        if (len > 9 && !strncmp(c, "modified=", 9)) *saved = _strtoi64(c + 9, NULL, 10) / 1000;
        if (len > 9 && !strncmp(c, "metadata=", 9)) {
            mpj *meta = mpj_parse(c + 9, len - 9 - (len > 9 && c[len - 1] == '\r'));
            const mpj *nm = meta ? mpj_get(meta, "name") : NULL;
            if (nm && nm->type == MPJ_STR) { free(*name); *name = mu_copy_str(nm); }
            if (!*saved) *saved = meta ? mpj_int(mpj_get(meta, "lastModified")) / 1000 : 0;
            mpj_free(meta);
        }
        c = end ? end + 1 : NULL;
    }
    for (i = 0; *name && (*name)[i]; i++) if ((unsigned char)(*name)[i] < 0x20) (*name)[i] = ' ';
    free(text);
    return 1;
}

static void mu_map_build(mu_map *mp, const mu_model *m, const mu_record *rec)
{
    const sh_modui_engine *e = mu_engine();
    mu_visit visit;
    char **names = NULL;
    size_t count = 0;
    memset(mp, 0, sizeof *mp);
    sh_modmanifest_current(&mp->rows);
    mp->entities = -2;
    visit.closure = &mp->closure;
    visit.mp = mp;
    if (e->map_json) {
        char *json = NULL, err[160];
        size_t len = 0;
        int r = e->map_json(&json, &len);
        if (r == 0) mp->entities = -1;
        else if (r > 0 && json) {
            int failed = 0;
            if (sh_mm_parse(json, len, &mp->mm, err, sizeof err)) {
                mp->have_mm = 1;
                mp->entities = (int)mp->mm.ent_count;
                count = sh_modmanifest_placed_names(json, len, &names, &failed);
                if (failed) { count = 0; names = NULL; }
            } else mu_log("map: the open map's JSON could not be read (%s)", err);
        }
        if (json) HeapFree(GetProcessHeap(), 0, json);
    }
    if (!mp->have_mm && mp->entities != -1 && e->placed_names) {
        mu_names nm;
        memset(&nm, 0, sizeof nm);
        mp->entities = e->placed_names(mu_names_add, &nm);
        mp->names = nm.count;
        if (mp->entities >= 0 && nm.count && sh_modserve_active())
            modserve_bundles_for_names_visit((const char *const *)nm.items, nm.count, mu_map_visit, &visit);
        if (mp->entities >= 0 && !nm.oom)
            mp->have_save = sh_modmanifest_rows_for((const char *const *)nm.items, nm.count, &mp->save) >= 0;
        mu_names_free(&nm);
    } else if (mp->have_mm) {
        mp->names = count;
        if (count && sh_modserve_active()) modserve_bundles_for_names_visit((const char *const *)names, count, mu_map_visit, &visit);
        mp->have_save = sh_modmanifest_rows_for((const char *const *)names, count, &mp->save) >= 0;
        sh_modmanifest_names_free(names, count);
    }
    if (mp->entities >= 0) {
        size_t i, k;
        /* The mods the map uses are the rows its save would write; the load's rows stand in only when that could not
         * be computed (out of memory), and then a row the save would drop still shows until the next save. */
        const sh_modmanifest_list *src = mp->have_save ? &mp->save : &mp->rows;
        if (!mp->have_save) mu_log("map: the rows a save would write could not be computed; the tab shows the map's last rows");
        for (i = 0; i < src->count; i++) {
            const sh_modmanifest_row *row = &src->rows[i];
            char sub_ok[SH_MODMANIFEST_HASH_CAP];
            int mod = -1;
            sh_modserve_bundle_ref ref;
            memset(&ref, 0, sizeof ref);
            /* A row of a build the closure reached is that mod. */
            for (k = 0; k < mp->mod_count && mod < 0; k++)
                if (mu_same_bundle(m, mp->mods[k].hash, row->build)) mod = (int)k;
            /* The asset rule: a row is its mod by salt, whatever build serves it. */
            for (k = 0; k < mp->mod_count && mod < 0; k++)
                if (mp->mods[k].in_closure && !_stricmp(mp->mods[k].id, row->salt)) mod = (int)k;
            /* A row served by another build of its salt (the root folder of
             * an own map, or the build the editor substituted): that build is
             * the mod. */
            sub_ok[0] = 0;
            if (!sh_modmanifest_substituted(row->build, sub_ok, sizeof sub_ok) || strlen(sub_ok) != SH_MODSERVE_HASH_HEX)
                sub_ok[0] = 0;
            if (!sub_ok[0] && sh_modserve_row_build(row, &ref) && strcmp(ref.hash, row->build) && strlen(ref.hash) == SH_MODSERVE_HASH_HEX)
                strcpy_s(sub_ok, sizeof sub_ok, ref.hash);
            sh_modserve_bundle_ref_free(&ref);
            if (sub_ok[0]) {
                for (k = 0; k < mp->mod_count && mod < 0; k++)
                    if (mu_same_bundle(m, mp->mods[k].hash, sub_ok)) mod = (int)k;
                if (mod < 0) mod = mu_mapmod_add(mp, sub_ok, row->salt, row->size);
            }
            if (mod < 0) mod = mu_mapmod_add(mp, row->build, row->salt, row->size);
            if (mod >= 0) {
                mp->mods[mod].in_rows = 1;
                if (row->attached) mp->mods[mod].attached = 1;
                if (row->pinned) mp->mods[mod].pinned = 1;
                if (!mp->mods[mod].row) mp->mods[mod].row = row;
                if (sub_ok[0] && strcmp(row->build, mp->mods[mod].hash))
                    strcpy_s(mp->mods[mod].declared, sizeof mp->mods[mod].declared, row->build);
            }
        }
        if (mp->have_mm) mu_map_attribute(mp);
        /* A bundle the placed names reached that the save writes no row of is not used by the map: the version rule
         * folds it into the installed version's row of the same mod, whose entities and modules it serves, so they
         * count there. One mod, one entry, as the saved map has it. */
        for (i = 0; mp->have_save && i < mp->mod_count;) {
            mu_mapmod *x = &mp->mods[i];
            size_t j, w;
            if (x->in_rows) { i++; continue; }
            for (j = 0; j < mp->mod_count; j++) {
                mu_mapmod *y = &mp->mods[j];
                if (j == i || !y->in_rows || _stricmp(y->id, x->id)) continue;
                for (w = 0; w < x->uid_count; w++) mu_mapmod_uid(mp, (int)j, x->uids[w]);
                y->modules += x->modules;
                if (x->named) y->named = 1;
                break;
            }
            free(x->uids);
            memmove(x, x + 1, (mp->mod_count - i - 1) * sizeof *x);
            mp->mod_count--;
        }
        if (e->map_info && e->map_info(&mp->info) == 1) {
            mp->have_info = 1;
            /* A rawmap opened as a new map: the ids are the borrowed saved
             * map's until the first save names it. */
            if (mp->info.opened_from[0]) mp->info.local_id[0] = mp->info.cloud_id[0] = 0;
            (void)mu_saved_details(mp->info.local_id, &mp->name, &mp->saved);
            for (i = 0; rec && i < rec->link_count && !mp->link; i++) {
                const mu_link *l = &rec->links[i];
                if ((mp->info.local_id[0] && !strcmp(l->local, mp->info.local_id)) ||
                    (mp->info.cloud_id[0] && !strcmp(l->cloud, mp->info.cloud_id))) mp->link = l;
            }
            for (i = 0; mp->link && i < rec->claim_count && !mp->claim; i++)
                if (!strcmp(rec->claims[i].ref, mp->link->item)) mp->claim = &rec->claims[i];
        }
    }
}

static void mu_map_free(mu_map *mp)
{
    size_t i;
    sh_modmanifest_list_free(&mp->rows);
    sh_modmanifest_list_free(&mp->save);
    mu_closure_free(&mp->closure);
    for (i = 0; i < mp->mod_count; i++) free(mp->mods[i].uids);
    free(mp->mods);
    for (i = 0; i < mp->hit_count; i++) free(mp->hits[i].name);
    free(mp->hits);
    if (mp->have_mm) sh_mm_free(&mp->mm);
    free(mp->name);
    memset(mp, 0, sizeof *mp);
}

/* Whether the manifest row is the map entry's: its build, or a build of its
 * salt (the asset rule: a row another build serves is that mod's row). */
static int mu_row_is_mod(const mu_model *m, const sh_modmanifest_row *row, const mu_mapmod *x)
{
    return mu_same_bundle(m, row->build, x->hash) || (x->id[0] && !_stricmp(row->salt, x->id));
}

/* The installed folder (entry index) a mod of the map is, or -1 when it is not installed: the folder whose build
 * serves it, else the folder of the build its save row names, else the installed folder of its namespace (a
 * composition is no folder, so the last step finds the copy installed here). */
static int mu_mapmod_entry(const mu_model *m, const mu_mapmod *x)
{
    int at = mu_find_hash(m, x->hash);
    /* the build the save writes for it (a row moved to the installed version while a composition still serves) */
    if (at < 0 && x->row) at = mu_find_hash(m, x->row->build);
    /* a row of the save names the mod by its namespace: a local row follows the installed folder of it, and a pinned
     * row is served by its composition with that folder alongside in the editor */
    if (at < 0 && x->in_rows) at = mu_find_salt(m, x->id, -1);
    return at;
}

/* A manifest row's state under the asset rule (modserve.h
 * sh_modserve_row_served): active, incomplete, skipped (a folder of its salt
 * that the index did not admit), missing. */
static const char *mu_row_state(const mu_model *m, const sh_modmanifest_row *r, char **cause, int *at_out)
{
    int st = sh_modserve_row_served(r, cause), at = mu_find_hash(m, r->build);
    sh_modserve_bundle_ref ref;
    memset(&ref, 0, sizeof ref);
    if (at < 0 && sh_modserve_row_build(r, &ref)) at = mu_find_hash(m, ref.hash);
    sh_modserve_bundle_ref_free(&ref);
    if (at < 0) at = mu_find_salt(m, r->salt, -1);
    if (at_out) *at_out = at;
    if (st == SH_MODSERVE_ROW_SERVED) {
        if (cause) { free(*cause); *cause = NULL; }
        return "active";
    }
    if (st == SH_MODSERVE_ROW_INCOMPLETE) return "incomplete";
    return at >= 0 ? "skipped" : "missing";
}

/* The slots the open map's declared mods set in their edits (modregistry.h, "<game file>/<path>") that two or more
 * keys set to different values, with the providers in row order, the map's choice and the key that serves. */
typedef struct mu_slot {
    char *slot;                          /* malloc'd */
    char (*keys)[SH_MB_ID_MAX + 1];      /* the providers' salts, in row order (malloc'd, n of cap) */
    unsigned long long *digest;
    int n, cap;
} mu_slot;

typedef struct mu_slot_ctx { mu_slot *slots; int ns, cap, oom; const char *key; const sh_mb_bundle *bd; } mu_slot_ctx;

static void mu_slots_free(mu_slot_ctx *c)
{
    int i;
    for (i = 0; i < c->ns; i++) { free(c->slots[i].slot); free(c->slots[i].keys); free(c->slots[i].digest); }
    free(c->slots);
    c->slots = NULL;
    c->ns = c->cap = 0;
}

static void mu_slot_add(void *context, const char *slot, unsigned long long digest)
{
    mu_slot_ctx *c = (mu_slot_ctx *)context;
    mu_slot *s;
    int i, k;
    if (c->oom || strlen(c->key) > SH_MB_ID_MAX) return;
    for (i = 0; i < c->ns; i++) if (!_stricmp(c->slots[i].slot, slot)) break;
    if (i == c->ns) {
        if (c->ns == c->cap) {
            int nc = c->cap ? c->cap * 2 : 64;
            mu_slot *g = (mu_slot *)realloc(c->slots, (size_t)nc * sizeof *g);
            if (!g) { c->oom = 1; return; }
            c->slots = g;
            c->cap = nc;
        }
        memset(&c->slots[i], 0, sizeof c->slots[i]);
        if (!(c->slots[i].slot = _strdup(slot))) { c->oom = 1; return; }
        c->ns++;
    }
    s = &c->slots[i];
    for (k = 0; k < s->n; k++) if (!strcmp(s->keys[k], c->key)) return;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        void *gk = realloc(s->keys, (size_t)nc * sizeof *s->keys), *gd;
        if (!gk) { c->oom = 1; return; }
        s->keys = (char (*)[SH_MB_ID_MAX + 1])gk;
        if (!(gd = realloc(s->digest, (size_t)nc * sizeof *s->digest))) { c->oom = 1; return; }
        s->digest = (unsigned long long *)gd;
        s->cap = nc;
    }
    strcpy_s(s->keys[s->n], sizeof s->keys[0], c->key);
    s->digest[s->n++] = digest;
}

static void mu_slot_file(void *context, const char *file, const sh_dt_counts *counts)
{
    mu_slot_ctx *c = (mu_slot_ctx *)context;
    (void)counts;
    (void)sh_dt_edits_slots(file, c->bd->edits, strlen(c->bd->edits), mu_slot_add, c);
}

/* The entry that serves a declared row's edits, as serving resolves it
 * (sh_modserve_row_build), else the build the editor load substituted for it
 * (sh_modmanifest_substituted), or -1. */
static int mu_row_serving_entry(const mu_model *m, const sh_modmanifest_row *row)
{
    char sub[SH_MODMANIFEST_HASH_CAP];
    sh_modserve_bundle_ref ref;
    int at;
    memset(&ref, 0, sizeof ref);
    at = sh_modserve_row_build(row, &ref) ? mu_find_hash(m, ref.hash) : -1;
    sh_modserve_bundle_ref_free(&ref);
    if (at >= 0) return at;
    if ((at = mu_find_hash(m, row->build)) >= 0) return at;
    if (sh_modmanifest_substituted(row->build, sub, sizeof sub) && (at = mu_find_hash(m, sub)) >= 0) return at;
    return -1;
}

static void mu_put_clashes(mu_buf *b, const mu_model *m, const mu_map *mp)
{
    mu_slot_ctx c;
    int i, k, first = 1;
    size_t r;
    memset(&c, 0, sizeof c);
    mu_puts(b, ",\"registryClashes\":[");
    for (r = 0; r < mp->rows.count; r++) {
        int at = mu_row_serving_entry(m, &mp->rows.rows[r]);
        const sh_mb_bundle *bd;
        if (at < 0) continue;
        bd = &m->list.items[at].bundle;
        if (!bd->edits) continue;
        c.key = bd->id;
        c.bd = bd;
        (void)sh_dt_edits_files(bd->edits, strlen(bd->edits), mu_slot_file, &c);
    }
    for (i = 0; c.slots && i < c.ns; i++) {
        char chosen[SH_MB_ID_MAX + 1] = "";
        const char *serving;
        int differ = 0;
        mu_slot *s = &c.slots[i];
        if (s->n < 2) continue;
        for (k = 1; k < s->n; k++) differ |= s->digest[k] != s->digest[0];
        if (!differ) continue;   /* identical edits merge */
        (void)sh_modregistry_choices_get(s->slot, chosen, sizeof chosen);
        serving = s->keys[0];
        for (k = 0; k < s->n; k++) if (chosen[0] && !strcmp(s->keys[k], chosen)) serving = s->keys[k];
        if (!first) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "slot", s->slot, 1);
        mu_puts(b, ",\"providers\":[");
        for (k = 0; k < s->n; k++) {
            int at = mu_find_salt(m, s->keys[k], -1);
            const char *name = at >= 0 ? mu_entry_title(&m->list.items[at]) : "";
            size_t q;
            /* Not installed: the name the map saved for it. */
            for (q = 0; !name[0] && q < mp->rows.count; q++)
                if (!_stricmp(mp->rows.rows[q].salt, s->keys[k])) name = sh_modmanifest_row_name(&mp->rows.rows[q]);
            if (k) mu_putn(b, ",", 1);
            mu_putn(b, "{", 1);
            mu_kstr(b, "id", s->keys[k], 1);
            mu_kstr(b, "name", name[0] ? name : "a mod that is not installed", 0);
            mu_putn(b, "}", 1);
        }
        mu_putn(b, "]", 1);
        mu_kstr(b, "chosen", chosen, 0);
        mu_kstr(b, "serving", serving, 0);
        mu_putn(b, "}", 1);
        first = 0;
    }
    mu_putn(b, "]", 1);
    mu_slots_free(&c);
}

/* The state a map entry shows: installed, incomplete, skipped, missing. A mod the placed names reach is installed; one only a row names
 * is judged by that row. */
static const char *mu_mapmod_state(const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    size_t i;
    int at = mu_mapmod_entry(m, x);
    if (x->in_closure) return "installed";
    for (i = 0; i < mp->rows.count; i++)
        if (!strcmp(mp->rows.rows[i].build, x->hash) || !_stricmp(mp->rows.rows[i].salt, x->id)) {
            const char *s = mu_row_state(m, &mp->rows.rows[i], NULL, NULL);
            return !strcmp(s, "active") ? "installed" : s;
        }
    return at >= 0 ? "installed" : "missing";
}

/* "<name> #<uniqueId>": the entity's own label, else its editor row's name,
 * else the last part of what it inherits. */
/* malloc'd, or NULL. */
static char *mu_entity_label(const mu_model *m, const sh_mm_entity *ent, int uid)
{
    const char *label = ent && ent->label[0] ? ent->label : NULL;
    if (!label && ent) label = mu_row_name_for(m, ent->inherit);
    if (!label && ent && ent->timeline) label = "Timeline";
    if (!label && ent && ent->inherit[0]) label = strrchr(ent->inherit, '/') ? strrchr(ent->inherit, '/') + 1 : ent->inherit;
    return sh_textf("%s #%d", label ? label : "Entity", uid);
}

/* malloc'd, or NULL. */
static char *mu_ref_name(const mu_model *m, const mu_map *mp, const sh_mm_ref *r)
{
    char *a = mu_entity_label(m, sh_mm_find(&mp->mm, r->from_uid), r->from_uid), *b, *out;
    if (!a || r->kind == SH_MM_REF_TIMELINE) return a;
    b = mu_entity_label(m, sh_mm_find(&mp->mm, r->to_uid), r->to_uid);
    out = b ? sh_textf("%s to %s", a, b) : NULL;
    free(a);
    free(b);
    return out;
}

/* [{kind, name}] of what points at the map entry's entities. */
static size_t mu_put_refs(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    sh_mm_ref *refs = NULL;
    size_t n = mp->have_mm && x->uid_count ? sh_mm_references(&mp->mm, x->uids, x->uid_count, &refs) : 0, i;
    if (n == (size_t)-1) n = 0;
    mu_putn(b, "[", 1);
    for (i = 0; i < n; i++) {
        char *name = mu_ref_name(m, mp, &refs[i]);
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "kind", refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", 1);
        mu_kstr(b, "name", name ? name : "", 0);
        mu_putn(b, "}", 1);
        free(name);
    }
    mu_putn(b, "]", 1);
    free(refs);
    return n;
}

static void mu_put_entity_names(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    size_t i;
    mu_putn(b, "[", 1);
    for (i = 0; i < x->uid_count; i++) {
        char *name = mu_entity_label(m, sh_mm_find(&mp->mm, x->uids[i]), x->uids[i]);
        if (i) mu_putn(b, ",", 1);
        mu_str(b, name ? name : "");
        free(name);
    }
    mu_putn(b, "]", 1);
}

/* "asked": what players of the published map asked of a mod and did not get because the map carried only part of it:
 * the record's "asked" section under the item, [{"mod","request","by"}], the mod named by its name. */
static void mu_put_asked(mu_buf *b, const mu_model *m, const char *item)
{
    char *text = NULL;
    mpj *x = m->root_ok && sh_mb_record_get(m->root, "asked", &text) == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    const mpj *list = x && x->type == MPJ_OBJ && item ? mpj_get(x, item) : NULL;
    size_t k;
    int i, first = 1;
    free(text);
    mu_puts(b, ",\"asked\":[");
    for (i = 0; list && list->type == MPJ_ARR && i < list->n; i++) {
        char *salt = mu_copy_str(mpj_get(list->items[i], "salt")), *request = mu_copy_str(mpj_get(list->items[i], "request"));
        char *by = mu_copy_str(mpj_get(list->items[i], "by")), *name = NULL;
        for (k = 0; salt && k < m->list.count && !name; k++)
            if (!_stricmp(m->list.items[k].salt, salt)) name = mu_folder_name(&m->list.items[k]);
        if (salt && request && request[0]) {
            if (!first) mu_putn(b, ",", 1);
            first = 0;
            mu_putn(b, "{", 1);
            mu_kstr(b, "mod", name ? name : "", 1);
            mu_kstr(b, "request", request, 0);
            mu_kstr(b, "by", by ? by : "", 0);
            mu_putn(b, "}", 1);
        }
        free(name);
        free(salt);
        free(request);
        free(by);
    }
    mu_putn(b, "]", 1);
    mpj_free(x);
}

/* The map entries the payload lists: every manifest row and every mod a
 * placed name reaches (one only another mod needs is listed too). */
static size_t mu_map_mod_count(const mu_map *mp) { return mp->mod_count; }

/* A placed name of a mod (mods/<salt>/<rest>) as the Toybox names it: the row of that mod whose entityDef is it,
 * written as the engine names it or as its author does (entities/<rest>); else its last part. */
static const char *mu_placed_title(const mu_model *m, const char *salt, const char *name)
{
    const char *rest = NULL, *leaf;
    size_t i, sl = salt ? strlen(salt) : 0;
    int k;
    if (!_strnicmp(name, "mods/", 5) && sl && !_strnicmp(name + 5, salt, sl) && name[5 + sl] == '/') rest = name + 6 + sl;
    for (i = 0; i < m->list.count; i++) {
        if (_stricmp(m->list.items[i].salt, salt ? salt : "")) continue;
        for (k = 0; k < m->entries[i].row_count; k++) {
            const char *def = m->entries[i].row_defs[k];
            if (!def || !def[0]) continue;
            if (!_stricmp(def, name) || (rest && !_strnicmp(def, "entities/", 9) && !_stricmp(def + 9, rest)))
                return m->entries[i].row_names[k];
        }
    }
    leaf = strrchr(name, '/');
    return leaf ? leaf + 1 : name;
}

static const char *mu_update_name(const mu_model *m, const mu_map *mp, const char *salt);
static void mu_put_map(mu_buf *b, const mu_model *m, const mu_map *mp)
{
    size_t i, k;
    mu_puts(b, ",\"map\":{");
    mu_kint(b, "entities", mp->entities, 1);
    mu_knum(b, "names", (unsigned long long)mp->names, 0);
    mu_key(b, "rows", 0);
    mu_putn(b, "[", 1);
    for (i = 0; i < mp->rows.count; i++) {
        const sh_modmanifest_row *r = &mp->rows.rows[i];
        char *cause = NULL;
        int at = -1;
        const char *state = mu_row_state(m, r, &cause, &at);
        /* key: the folder that serves the row (its own build, or the one the
         * asset rule plays). */
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "salt", r->salt, 1);
        mu_kstr(b, "name", sh_modmanifest_row_name(r), 0);
        mu_kstr(b, "build", r->build, 0);
        mu_knum(b, "size", r->size, 0);
        mu_kstr(b, "state", state, 0);
        mu_kstr(b, "cause", cause ? cause : "", 0);
        free(cause);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 0);
        mu_knum(b, "nameCount", (unsigned long long)r->name_count, 0);
        mu_key(b, "names", 0);
        mu_putn(b, "[", 1);
        for (k = 0; k < r->name_count; k++) {
            if (k) mu_putn(b, ",", 1);
            mu_str(b, r->names[k]);
        }
        mu_putn(b, "]", 1);
        mu_kbool(b, "attached", r->attached, 0);
        mu_putn(b, "}", 1);
    }
    mu_puts(b, "],\"placed\":[");
    for (i = 0; i < mp->closure.count; i++) {
        const mu_closure_bundle *cb = &mp->closure.items[i];
        int at = mu_find_hash(m, cb->hash), in_manifest = 0;
        for (k = 0; k < mp->rows.count; k++) if (mu_same_bundle(m, mp->rows.rows[k].build, cb->hash)) in_manifest = 1;
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "salt", cb->id, 1);
        mu_kstr(b, "build", cb->hash, 0);
        mu_knum(b, "size", cb->size, 0);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 0);
        mu_kbool(b, "viaNeeds", cb->via_needs, 0);
        mu_kbool(b, "inManifest", in_manifest, 0);
        mu_knum(b, "nameCount", (unsigned long long)cb->name_count, 0);
        mu_key(b, "names", 0);
        mu_putn(b, "[", 1);
        for (k = 0; k < (size_t)cb->name_count; k++) {
            if (k) mu_putn(b, ",", 1);
            mu_str(b, cb->names[k]);
        }
        mu_puts(b, "]}");
    }
    mu_putn(b, "]", 1);
    if (mp->entities >= 0) {
        /* Payload v3: what the map is and which mods it uses. Fields the
         * product cannot know are left out. */
        mu_kstr(b, "name", mp->name, 0);
        mu_knum(b, "saved", mp->saved > 0 ? (unsigned long long)mp->saved : 0, 0);
        if (mp->link) {
            /* A link is written when an item is published with mods from this
             * installation and dropped at its unpublish. */
            mu_puts(b, ",\"published\":{");
            mu_kstr(b, "state", "published", 1);
            mu_knum(b, "at", mp->link->at > 0 ? (unsigned long long)mp->link->at : 0, 0);
            mu_puts(b, "},\"uploaded\":{");
            mu_kbool(b, "claimed", mp->claim != NULL, 1);
            mu_knum(b, "at", mp->claim && mp->claim->updated > 0 ? (unsigned long long)mp->claim->updated : 0, 0);
            mu_putn(b, "}", 1);
            mu_put_asked(b, m, mp->link->item);
        }
        mu_puts(b, ",\"mods\":[");
        for (i = 0; i < mu_map_mod_count(mp); i++) {
            const mu_mapmod *x = &mp->mods[i];
            int at = mu_mapmod_entry(m, x);
            if (i) mu_putn(b, ",", 1);
            mu_putn(b, "{", 1);
            mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 1);
            mu_kstr(b, "salt", x->id, 0);
            {
                /* The installed folder's name, else the row's. */
                char *name = at >= 0 ? mu_folder_name(&m->list.items[at]) : NULL;
                const char *row_name = "";
                for (k = 0; !name && k < mp->rows.count && !row_name[0]; k++)
                    if (!_stricmp(mp->rows.rows[k].salt, x->id)) row_name = mp->rows.rows[k].name;
                mu_kstr(b, "name", name ? name : row_name, 0);
                free(name);
            }
            mu_kstr(b, "state", mu_mapmod_state(m, mp, x), 0);
            mu_key(b, "placedEntities", 0);
            mu_put_entity_names(b, m, mp, x);
            mu_knum(b, "placedCount", (unsigned long long)x->uid_count, 0);
            mu_knum(b, "placedModules", (unsigned)x->modules, 0);
            mu_key(b, "references", 0);
            (void)mu_put_refs(b, m, mp, x);
            mu_knum(b, "size", x->size, 0);
            mu_kbool(b, "inManifest", x->in_rows, 0);
            mu_kbool(b, "viaNeeds", x->in_closure && !x->named, 0);
            mu_kbool(b, "attached", x->attached, 0);
            /* A map-level row and nothing of it placed: "declared, nothing placed". */
            mu_kbool(b, "nothingPlaced", x->attached && !x->uid_count && !x->modules, 0);
            mu_kbool(b, "pinned", x->pinned, 0);
            {
                /* The objects of the mod the map places, once each, as the Toybox names them (else the name's last
                 * part): the row the save writes names them, and the map carries their pieces. */
                size_t q, w;
                int n = 0;
                mu_key(b, "objects", 0);
                mu_putn(b, "[", 1);
                for (q = 0; x->row && q < x->row->name_count; q++) {
                    const char *pn = x->row->names[q], *show;
                    int dup = 0;
                    if (!pn || !pn[0]) continue;
                    show = mu_placed_title(m, x->id, pn);
                    for (w = 0; w < q && !dup; w++)
                        dup = x->row->names[w] && x->row->names[w][0] && !_stricmp(mu_placed_title(m, x->id, x->row->names[w]), show);
                    if (dup) continue;
                    if (n++) mu_putn(b, ",", 1);
                    mu_str(b, show);
                }
                mu_putn(b, "]", 1);
            }
            mu_kstr(b, "build", x->hash, 0);
            {
                /* 5.10: a row of another version than the installed mod offers Update. */
                const char *other = mu_update_name(m, mp, x->id);
                char *line = other ? sh_textf("This map uses another version of %s than the one you have.",
                                              other[0] ? other : "this mod") : NULL;
                mu_kstr(b, "update", line ? line : "", 0);
                free(line);
            }
            mu_putn(b, "}", 1);
        }
        mu_putn(b, "]", 1);
        mu_kbool(b, "vanilla", mp->mod_count == 0, 0);
        mu_put_clashes(b, m, mp);
    }
    mu_putn(b, "}", 1);
}

/* ------------------------------------------------------------------ the document */

int sh_modui_list(void)
{
    mu_model m;
    mu_map mp;
    mu_record rec;
    mu_buf b;
    size_t i;
    int n;
    if (!mu_on_main()) return -1;
    memset(&b, 0, sizeof b);
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    mu_putn(&b, "{", 1);
    mu_knum(&b, "v", 4, 1);
    mu_knum(&b, "gen", (unsigned)g_gen, 0);
    mu_kstr(&b, "root", m.root_ok ? m.root : "", 0);
    mu_kbool(&b, "storeReadable", m.listed, 0);
    mu_puts(&b, ",\"mods\":[");
    for (i = 0; i < m.list.count; i++) {
        if (i) mu_putn(&b, ",", 1);
        mu_put_row(&b, &m, &rec, &m.entries[i]);
    }
    mu_putn(&b, "]", 1);
    mu_put_map(&b, &m, &mp);
    if (g_key_status) {
        /* the publishing key's state: the backup offer after the first publish shows from it (SPEC 4.2) */
        char *k = g_key_status();
        if (k && k[0] == '{') { mu_puts(&b, ",\"publishingKey\":"); mu_puts(&b, k); }
        free(k);
    }
    mu_putn(&b, "}", 1);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
    n = mu_publish(&b);
    return n;
}

/* ------------------------------------------------------------------ actions */

static const char *mu_action_name(int action)
{
    switch (action) {
    case SH_MODS_ACT_INSTALL: return "install";
    case SH_MODS_ACT_RESCAN: return "rescan";
    case SH_MODS_ACT_PALETTE: return "palette";
    case SH_MODS_ACT_APPLY: return "apply";
    case SH_MODS_ACT_UNINSTALL: return "uninstall";
    case SH_MODS_ACT_UNINSTALL_PREVIEW: return "uninstallPreview";
    case SH_MODS_ACT_MAP_REMOVE: return "mapRemoveMod";
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW: return "mapRemoveModPreview";
    case SH_MODS_ACT_MAP_ADD: return "mapAddMod";
    case SH_MODS_ACT_CHOOSE: return "chooseRegistry";
    case SH_MODS_ACT_FILES: return "modFiles";
    case SH_MODS_ACT_FILE_TEXT: return "modFileText";
    case SH_MODS_ACT_NEW_MOD: return "newMod";
    case SH_MODS_ACT_ADD_MINE: return "addToMine";
    case SH_MODS_ACT_CONTENT: return "content";
    case SH_MODS_ACT_MAP_UPDATE: return "mapUpdate";
    case SH_MODS_ACT_KEY: return "publishingKey";
    default: return "unknown";
    }
}

static void mu_result_head(mu_buf *b, int action, int ok, const char *message)
{
    mu_putn(b, "{", 1);
    mu_kstr(b, "action", mu_action_name(action), 1);
    mu_kbool(b, "ok", ok, 0);
    mu_kstr(b, "message", message, 0);
}

static int mu_result_tail(mu_buf *b)
{
    mu_knum(b, "gen", (unsigned)g_gen, 0);
    mu_putn(b, "}", 1);
    return mu_publish(b);
}

void sh_modui_set_key(char *(*action)(const char *arg), char *(*status)(void))
{
    g_key_action = action;
    g_key_status = status;
}

static int mu_simple_result(int action, int ok, const char *message);

/* SH_MODS_ACT_KEY: the request's answer (an object with ok, message and key) as the action's result. The arg carries
 * a passphrase: it is never logged. */
static int mu_key_request(const char *arg)
{
    char *doc = g_key_action ? g_key_action(arg) : NULL;
    size_t n = doc ? strlen(doc) : 0;
    mu_buf b;
    if (!doc || n < 2 || doc[0] != '{' || doc[n - 1] != '}') {
        free(doc);
        return mu_simple_result(SH_MODS_ACT_KEY, 0, "Snapmap+ could not reach your publishing key.");
    }
    memset(&b, 0, sizeof b);
    mu_putn(&b, "{", 1);
    mu_kstr(&b, "action", "publishingKey", 1);
    mu_putn(&b, ",", 1);
    mu_putn(&b, doc + 1, n - 2);
    free(doc);
    return mu_result_tail(&b);
}

static int mu_simple_result(int action, int ok, const char *message)
{
    mu_buf b;
    memset(&b, 0, sizeof b);
    mu_result_head(&b, action, ok, message);
    return mu_result_tail(&b);
}

/* ---- install */

typedef struct mu_stock { int unavailable; } mu_stock;

static int mu_stock_exists(void *context, const char *request)
{
    mu_stock *s = (mu_stock *)context;
    const sh_modui_engine *e = mu_engine();
    int r;
    if (!e->stock_exists) { s->unavailable = 1; return -1; }
    r = e->stock_exists(request);
    if (r < 0) s->unavailable = 1;
    return r;
}

/* The file part of a rescan, on any thread: the rebuild rule (sh_ms_sync: a
 * new folder gets its salt, a folder whose tree changed is built again), then
 * each mod's builder warnings in the log. */
static void mu_sync(void)
{
    sh_ms_sync_stats sync;
    sh_mb_resolver r;
    mu_stock stock = { 0 };
    sh_ms_list l;
    size_t i;
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    if (sh_ms_sync(&r, &sync) && (sync.rebuilt || sync.failed || sync.salted || sync.resalted))
        mu_log("rescan: %u new mod(s), %u copy(ies) made mods of their own, %u folder(s) built, %u refused (%llu ms)",
               sync.salted, sync.resalted, sync.rebuilt, sync.failed, sync.ms);
    InterlockedIncrement(&g_gen);
    if (!sh_ms_enumerate(&l)) return;
    for (i = 0; i < l.count; i++) {
        const sh_ms_entry *x = &l.items[i];
        if (x->loaded && x->bundle.warnings)
            mu_log("rescan: mods\\%s: %u warning(s), first: %s", x->name, x->bundle.warnings, sh_mb_first_warning(&x->bundle));
    }
    sh_ms_list_free(&l);
}

/* The game's part, on the main thread: the serving index read again, then
 * what changed brought into the running game (*rf; state -1 when that did not
 * run). Returns the mods in use, or -1 with the plain reason in `why`. */
static int mu_apply(sh_modserve_refresh *rf, char *why, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    int n;
    memset(rf, 0, sizeof *rf);
    rf->state = -1;
    why[0] = 0;
    if (!e->rescan) {
        strncpy_s(why, cap, "The game cannot read the mods folder again from here.", _TRUNCATE);
        return -1;
    }
    n = e->rescan();
    InterlockedIncrement(&g_gen);
    if (n < 0) {
        strncpy_s(why, cap, "Snapmap+ could not read the mods folder; press Refresh to try again.", _TRUNCATE);
        return -1;
    }
    if (e->refresh) rf->state = e->refresh(rf);
    mu_log("rescan: %d mod(s) in use; in the game: %s", n, rf->state < 0 ? "no refresh bound" : rf->note);
    return n;
}

/* Both, on the main thread (an uninstall). */
static int mu_rescan(sh_modserve_refresh *rf, char *why, size_t cap)
{
    mu_sync();
    return mu_apply(rf, why, cap);
}

/* What the player must still do for a change to show, as one sentence, or
 * "" when it shows now. */
static int g_restart_said;   /* banks waiting for a restart that a result has named */

/* The sentence (malloc'd), or NULL when the change shows now. */
static char *mu_refresh_sentence(const sh_modserve_refresh *rf)
{
    const sh_modui_engine *e = mu_engine();
    int waiting = e->restart_banks ? e->restart_banks(NULL, NULL) : 0;
    if (!rf || rf->state < 0) return NULL;
    if (waiting > g_restart_said) {
        /* A sound bank this change replaced: the one thing it needs is named. */
        char *banks;
        g_restart_said = waiting;
        if ((banks = mu_restart_sentence()) != NULL) return banks;
    }
    if (rf->needs_restart > 0) return sh_text("Some of the changes take effect only after you restart DOOM.");
    if (rf->needs_map_reload > 0) return sh_text("Some of the changes take effect when you next open the map.");
    if (rf->state == 2 || rf->pending > 0) return sh_text("Some of the changes take effect when you are back in the editor.");
    return NULL;
}

/* "refresh":{reparsed, reloaded, needsMapReload, needsRestart, pending, deferred}. */
static void mu_put_refresh(mu_buf *b, const sh_modserve_refresh *rf)
{
    mu_key(b, "refresh", 0);
    mu_putn(b, "{", 1);
    mu_knum(b, "reparsed", (unsigned)(rf->reparsed < 0 ? 0 : rf->reparsed), 1);
    mu_knum(b, "reloaded", (unsigned)(rf->reloaded < 0 ? 0 : rf->reloaded), 0);
    mu_knum(b, "needsMapReload", (unsigned)(rf->needs_map_reload < 0 ? 0 : rf->needs_map_reload), 0);
    mu_knum(b, "needsRestart", (unsigned)(rf->needs_restart < 0 ? 0 : rf->needs_restart), 0);
    mu_knum(b, "pending", (unsigned)(rf->pending < 0 ? 0 : rf->pending), 0);
    mu_kbool(b, "deferred", rf->state == 2, 0);
    mu_putn(b, "}", 1);
}

/* ---- the file actions: Install, New mod, Refresh and Add to my mods */

/* The file work (copies, salts, builds) runs on the calling thread, which may be the window's worker, and is never
 * failed for taking long. Off the main thread the result is kept here and "apply": true asks for SH_MODS_ACT_APPLY,
 * which runs the game's part (mu_apply) and finishes it; on the main thread both run in one call. */
static int  g_owed_action;
static int  g_owed_ok;
static char *g_owed_msg;     /* malloc'd, or NULL */
static char *g_owed_extra;

static int mu_file_action(int action)
{
    return action == SH_MODS_ACT_INSTALL || action == SH_MODS_ACT_NEW_MOD || action == SH_MODS_ACT_RESCAN ||
           action == SH_MODS_ACT_ADD_MINE;
}

/* The finished result: the action's message, then the sentence a change
 * still needs (or why the game could not take it), its own members and what
 * the game did. */
static int mu_finish(int action, int ok, const char *msg, const char *extra, const sh_modserve_refresh *rf, const char *why)
{
    char *sentence = mu_refresh_sentence(rf), *full;
    const char *tail = why[0] ? why : sentence ? sentence : "";
    mu_buf b;
    memset(&b, 0, sizeof b);
    full = sh_textf("%s%s%s", msg ? msg : "", msg && msg[0] && tail[0] ? " " : "", tail);
    mu_result_head(&b, action, ok, full ? full : msg ? msg : "");
    free(full);
    free(sentence);
    if (extra) mu_puts(&b, extra);
    if (rf->state >= 0) mu_put_refresh(&b, rf);
    return mu_result_tail(&b);
}

/* A file action's result: with `changed` the game's part follows, at once on
 * the main thread, else owed ("apply": true). A failed game part fails a
 * Refresh, whose whole point it is; an install that landed stays installed. */
static int mu_file_result(int action, int ok, const char *msg, mu_buf *extra, int changed)
{
    sh_modserve_refresh rf;
    char why[200] = "";
    const char *members = extra->p && !extra->oom ? extra->p : NULL;
    int r;
    memset(&rf, 0, sizeof rf);
    rf.state = -1;
    if (changed && !mu_on_main()) {
        mu_buf b;
        memset(&b, 0, sizeof b);
        free(g_owed_extra);
        g_owed_extra = members ? _strdup(members) : NULL;
        g_owed_action = action;
        g_owed_ok = ok;
        free(g_owed_msg);
        g_owed_msg = _strdup(msg ? msg : "");
        mu_result_head(&b, action, ok, msg ? msg : "");
        if (members) mu_puts(&b, members);
        mu_kbool(&b, "apply", 1, 0);
        free(extra->p);
        return mu_result_tail(&b);
    }
    if (changed && mu_apply(&rf, why, sizeof why) < 0 && action == SH_MODS_ACT_RESCAN) ok = 0;
    r = mu_finish(action, ok, msg, members, &rf, why);
    free(extra->p);
    return r;
}

/* SH_MODS_ACT_APPLY, on the main thread: the game's part of the file action
 * that ran on the worker. */
static int mu_apply_owed(void)
{
    sh_modserve_refresh rf;
    char why[200] = "";
    int r, ok = g_owed_ok;
    if (!g_owed_action) return mu_simple_result(SH_MODS_ACT_APPLY, 1, "");
    if (mu_apply(&rf, why, sizeof why) < 0 && g_owed_action == SH_MODS_ACT_RESCAN) ok = 0;
    r = mu_finish(g_owed_action, ok, g_owed_msg, g_owed_extra, &rf, why);
    g_owed_action = 0;
    free(g_owed_extra);
    g_owed_extra = NULL;
    free(g_owed_msg);
    g_owed_msg = NULL;
    return r;
}

static int mu_absolute(const char *folder)
{
    return folder && ((folder[0] && folder[1] == ':') || (folder[0] == '\\' && folder[1] == '\\'));
}

/* The record (mods\snapmap-plus.json) could not be read or written during the action: the player reads which file
 * and what to do (modbundle.h sh_mb_record_why), not only the backend log. */
static char *mu_record_note(char *msg);

/* An install refusal: the store's own (where it cannot copy or write) as it
 * says it, a build's or a shape's as the folder's cause. malloc'd. */
static char *mu_install_cause(const char *cause)
{
    static const char *const store[] = { "that folder", "the store folder", "cannot create", "no free folder", "cannot copy",
                                         "the copy did not verify", "cannot write", "no mod folder given", "out of memory", NULL };
    int i;
    for (i = 0; store[i]; i++)
        if (mu_starts(cause, store[i])) {
            char *out = sh_text(cause);
            if (out && out[0] && islower((unsigned char)out[0])) out[0] = (char)toupper((unsigned char)out[0]);
            return mu_sentence_end(out);
        }
    return mu_plain_cause(NULL, cause);
}

/* `msg` (malloc'd, taken) with the record's trouble said after it, when the
 * record could not be read or written during the action. */
static char *mu_record_note(char *msg)
{
    const char *why = sh_mb_record_why();
    char *out;
    size_t n;
    if (!why || !why[0] || !msg) return msg;
    n = strlen(msg);
    out = sh_textf("%s%s %s", msg, n && msg[n - 1] != '.' ? "." : "", why);
    if (!out) return msg;
    free(msg);
    return out;
}

/* The display name of an installed folder (its build's, else its folder's). Borrowed. */
static const char *mu_name_of(const sh_mb_bundle *bd, const char *key)
{
    return bd && bd->name && bd->name[0] ? bd->name : mu_leaf(key);
}

static int mu_install_one(const char *folder)
{
    sh_mb_resolver r;
    sh_ms_install_stats st;
    sh_mb_bundle bd;
    mu_stock stock = { 0 };
    char *key = NULL, *cause = NULL, *plain = NULL, *msg;
    const char *name;
    int ok, rows = 0, rc;
    size_t k;
    mu_buf x;
    memset(&x, 0, sizeof x);
    mu_log("install %s: building", folder);
    memset(&st, 0, sizeof st);
    memset(&bd, 0, sizeof bd);
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    sh_mb_record_why_clear();
    ok = sh_ms_install_from_folder(folder, &r, &key, &st, &bd, &cause);
    for (k = 0; k < bd.file_count; k++)
        if (bd.files[k].ptype && !_stricmp(bd.files[k].ptype, "snapEditorEntityDef") && bd.files[k].cls != SH_MB_CLASS_EDITOR)
            rows++;
    name = mu_name_of(&bd, ok && key ? key : folder);
    if (!ok) {
        mu_log("install %s: refused: %s", folder, cause ? cause : "");
        plain = mu_install_cause(cause && cause[0] ? cause : "the mod could not be built");
        msg = sh_textf("%s was not installed. %s", name, plain ? plain : "");
    } else {
        mu_log("install %s: installed as mods\\%s (salt %s, build %s)", folder, key, bd.id, bd.hash);
        mu_sync();
        if (st.present) msg = sh_textf("%s is already installed.", name);
        else if (st.resalted)
            msg = sh_textf("Installed %s as a mod of its own, beside the one you already have%s.", name,
                           rows ? "; its Toybox rows are in the palette" : "");
        else msg = sh_textf("Installed %s%s.", name, rows ? "; its Toybox rows are in the palette" : "");
    }
    msg = mu_record_note(msg);
    mu_kstr(&x, "key", ok && key ? key : "", 0);
    mu_kstr(&x, "cause", ok || !plain ? "" : plain, 0);
    mu_knum(&x, "files", bd.file_count, 0);
    mu_knum(&x, "size", bd.total_size, 0);
    mu_knum(&x, "editorRows", (unsigned)rows, 0);
    mu_knum(&x, "warnings", bd.warnings, 0);
    mu_kbool(&x, "replaced", st.replaced, 0);
    mu_kbool(&x, "inPlace", st.in_place, 0);
    mu_kbool(&x, "stockUnchecked", stock.unavailable, 0);
    rc = mu_file_result(SH_MODS_ACT_INSTALL, ok, msg ? msg : "", &x, ok);
    sh_mb_free(&bd);
    free(key);
    free(cause);
    free(plain);
    free(msg);
    return rc;
}

/* The install action's argument: absolute folders separated by newlines. A folder holding mod.json is one mod; any
 * other is searched for every mod folder below it. One mod keeps the single result above; anything else answers
 * with the bulk result (installed and refused counts, each refusal's cause, "key" the first installed folder). */
typedef struct mu_bulk_item { char *folder; int found; const char *why; } mu_bulk_item;

static int mu_bulk_add(mu_bulk_item **items, size_t *n, size_t *cap, const char *folder, int found)
{
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        mu_bulk_item *t = (mu_bulk_item *)realloc(*items, ncap * sizeof *t);
        if (!t) return 0;
        *items = t;
        *cap = ncap;
    }
    (*items)[*n].folder = _strdup(folder);
    (*items)[*n].found = found;
    (*items)[*n].why = NULL;
    if (!(*items)[*n].folder) return 0;
    (*n)++;
    return 1;
}

static void mu_remove_tree(const wchar_t *dir);

/* A picked path that names a ZIP (the Prefab and map exports share mods so). */
static int mu_zip_name(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !_stricmp(path + n - 4, ".zip");
}


/* Unpack the ZIP at `zip` into a new folder under %TEMP% (`dir`), every
 * entry at its own path (the reader refuses an unsafe name). 1, else 0 with
 * the plain cause and nothing left behind. */
static int mu_unzip(const char *zip, char **dir, char **cause)
{
    sh_zip_reader r;
    DWORD need;
    wchar_t *tmp;
    char *tmpu = NULL, *why = NULL;
    size_t i;
    int ok = 1;
    wchar_t *wdir;
    memset(&r, 0, sizeof r);
    *dir = NULL;
    if (!sh_zip_reader_open(&r, zip, &why)) {
        sh_text_set(cause, "it cannot be read as a ZIP (%s)", why ? why : "out of memory");
        free(why);
        return 0;
    }
    need = GetTempPathW(0, NULL);
    tmp = need ? (wchar_t *)malloc(((size_t)need + 1) * sizeof *tmp) : NULL;
    if (!tmp || !GetTempPathW(need + 1, tmp) || (tmpu = sh_mb_utf8(tmp)) == NULL) {
        free(tmp);
        sh_zip_reader_close(&r);
        sh_text_set(cause, "the temporary folder cannot be found");
        return 0;
    }
    free(tmp);
    *dir = sh_textf("%ssnapmap-plus-zip-%lu-%llu", tmpu, GetCurrentProcessId(), GetTickCount64());
    free(tmpu);
    wdir = *dir ? sh_mb_wpath(*dir) : NULL;
    if (!wdir || !CreateDirectoryW(wdir, NULL)) {
        free(wdir);
        free(*dir);
        *dir = NULL;
        sh_zip_reader_close(&r);
        sh_text_set(cause, "the temporary folder cannot be written");
        return 0;
    }
    for (i = 0; ok && i < r.n; i++) {
        const char *name = r.e[i].name;
        size_t nl = strlen(name), len = 0, k;
        char *path;
        unsigned char *bytes;
        wchar_t *wp;
        HANDLE h;
        DWORD put = 0;
        if (!nl || name[nl - 1] == '/') continue;   /* a folder entry */
        if (!(path = sh_textf("%s\\%s", *dir, name))) { sh_text_set(cause, "out of memory"); ok = 0; break; }
        for (k = strlen(*dir); path[k]; k++) {
            if (path[k] != '/') continue;
            path[k] = 0;
            wp = sh_mb_wpath(path);
            if (wp) CreateDirectoryW(wp, NULL);
            free(wp);
            path[k] = '\\';
        }
        bytes = sh_zip_reader_read(&r, (long)i, (size_t)-1, &len, &why);
        if (!bytes) {
            sh_text_set(cause, "%s in it cannot be read (%s)", name, why ? why : "out of memory");
            free(path);
            ok = 0;
            break;
        }
        wp = sh_mb_wpath(path);
        free(path);
        h = wp ? CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (h == INVALID_HANDLE_VALUE) ok = 0;
        /* In pieces: one WriteFile moves at most 4 GB. */
        for (k = 0; ok && k < len; k += put) {
            DWORD piece = len - k > (1u << 30) ? (1u << 30) : (DWORD)(len - k);
            if (!WriteFile(h, bytes + k, piece, &put, NULL) || !put) ok = 0;
        }
        if (!ok) {
            mu_log("unpack %s: the write failed (error %lu)", name, GetLastError());
            sh_text_set(cause, "%s could not be unpacked; the ZIP may be damaged. Download it again and install it.", name);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(wp);
        free(bytes);
    }
    sh_zip_reader_close(&r);
    free(why);
    if (!ok) { mu_remove_tree(wdir); free(*dir); *dir = NULL; }
    free(wdir);
    return ok;
}

static int mu_install(const char *arg)
{
    mu_bulk_item *items = NULL;
    size_t n = 0, cap = 0, i, picked = 0;
    const char *p = arg ? arg : "";
    char *first_key = NULL, *msg = NULL;
    mu_buf refused_all = { 0 };
    unsigned installed = 0, refused = 0, rows = 0;
    int rc, single_mod = 0;
    char **unpacked = NULL;
    size_t zips = 0;
    mu_buf x, items_json;
    memset(&x, 0, sizeof x);
    memset(&items_json, 0, sizeof items_json);
    mu_puts(&refused_all, "");
    sh_mb_record_why_clear();

    /* Expand the picked folders into mod folders. */
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *folder;
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        folder = sh_textn(p, len);
        p = e ? e + 1 : p + strlen(p);
        if (!folder || !len) { free(folder); continue; }
        picked++;
        if (!mu_absolute(folder)) { mu_bulk_add(&items, &n, &cap, folder, 0); free(folder); continue; }
        if (mu_zip_name(folder)) {
            /* A ZIP: unpacked under %TEMP%, then every mod folder in it, as a folder of mods. */
            char *dir = NULL, *why = NULL, **mods = NULL, **grown;
            size_t count = 0, k;
            if (!mu_unzip(folder, &dir, &why) ||
                !(grown = (char **)realloc(unpacked, (zips + 1) * sizeof *unpacked))) {
                if (mu_bulk_add(&items, &n, &cap, folder, -1)) items[n - 1].why = _strdup(why ? why : "out of memory");
                if (dir) { wchar_t *w = sh_mb_wpath(dir); if (w) mu_remove_tree(w); free(w); }
                free(dir);
                free(why);
                free(folder);
                single_mod = 0;
                continue;
            }
            free(why);
            unpacked = grown;
            unpacked[zips++] = dir;
            if (sh_ms_folder_is_mod(dir)) {
                mu_bulk_add(&items, &n, &cap, dir, 1);
                count = 1;
            } else if (sh_ms_find_mods(dir, &mods, &count) && count)
                for (k = 0; k < count; k++) mu_bulk_add(&items, &n, &cap, mods[k], 2);
            sh_ms_find_free(mods, count);
            mu_log("install %s: a ZIP of %zu mod(s)", folder, count);
            if (!count && mu_bulk_add(&items, &n, &cap, folder, -1))
                items[n - 1].why = _strdup("it holds no mod: a mod is a folder with a mod.json");
            single_mod = picked == 1 && count == 1;
            free(folder);
            continue;
        }
        if (sh_ms_folder_is_mod(folder)) {
            mu_bulk_add(&items, &n, &cap, folder, 1);
            if (picked == 1) single_mod = 1;
            free(folder);
            continue;
        }
        single_mod = 0;
        {
            char **mods = NULL;
            size_t count = 0, k;
            if (sh_ms_find_mods(folder, &mods, &count) && count) {
                for (k = 0; k < count; k++) mu_bulk_add(&items, &n, &cap, mods[k], 2);
                mu_log("install %s: a folder of %zu mod(s)", folder, count);
            } else {
                mu_bulk_add(&items, &n, &cap, folder, 0);
            }
            sh_ms_find_free(mods, count);
        }
        free(folder);
    }
    if (picked == 0) { free(items); return mu_simple_result(SH_MODS_ACT_INSTALL, 0, "Choose a mod folder or a mod ZIP to install."); }
    if (picked == 1 && n == 1 && items[0].found <= 0) {
        char *why = items[0].found < 0
            ? sh_textf("That ZIP was not installed: %s.", items[0].why ? items[0].why : "it cannot be read")
            : sh_text(mu_absolute(items[0].folder)
                ? "That folder holds no mod: a mod folder has a mod.json; choose one, or a folder of mod folders."
                : "Choose a mod folder or a mod ZIP to install.");
        rc = mu_simple_result(SH_MODS_ACT_INSTALL, 0, why ? why : "That was not installed.");
        free(why);
        goto out;
    }
    if (picked == 1 && single_mod && n == 1) {
        rc = mu_install_one(items[0].folder);
        goto out;
    }

    /* Bulk: install each, rescan once. */
    mu_putn(&items_json, "[", 1);
    for (i = 0; i < n; i++) {
        sh_mb_resolver r;
        sh_ms_install_stats st;
        sh_mb_bundle bd;
        mu_stock stock = { 0 };
        char *key = NULL, *cause = NULL, *plain = NULL;
        int ok = 0;
        size_t k;
        unsigned mod_rows = 0;
        memset(&bd, 0, sizeof bd);
        memset(&st, 0, sizeof st);
        if (items[i].found) {
            r.installed = NULL;
            r.installed_context = NULL;
            r.stock = mu_stock_exists;
            r.stock_context = &stock;
            mu_log("install %s: building", items[i].folder);
            ok = sh_ms_install_from_folder(items[i].folder, &r, &key, &st, &bd, &cause);
            for (k = 0; k < bd.file_count; k++)
                if (bd.files[k].ptype && !_stricmp(bd.files[k].ptype, "snapEditorEntityDef") &&
                    bd.files[k].cls != SH_MB_CLASS_EDITOR) mod_rows++;
        } else if (items[i].found < 0) {
            cause = _strdup(items[i].why ? items[i].why : "it cannot be read as a ZIP");
        } else {
            cause = _strdup(mu_absolute(items[i].folder) ? "it holds no mod.json, and no folder below it does"
                                                          : "it is not a folder on this computer");
        }
        if (ok) {
            installed++;
            rows += mod_rows;
            if (!first_key && key) first_key = _strdup(key);
            mu_log("install %s: installed as mods\\%s (salt %s, build %s)", items[i].folder, key ? key : "", bd.id, bd.hash);
        } else {
            refused++;
            mu_log("install %s: refused: %s", items[i].folder, cause ? cause : "");
            if (items[i].found > 0) plain = mu_install_cause(cause ? cause : "the mod could not be built");
            else plain = sh_textf("It was not installed: %s.", cause ? cause : "");
            /* Every refusal is said, not only the first. */
            mu_puts(&refused_all, " ");
            mu_puts(&refused_all, mu_name_of(&bd, items[i].folder));
            mu_puts(&refused_all, ": ");
            mu_puts(&refused_all, plain ? plain : "");
        }
        mu_putn(&items_json, i ? ",{" : "{", i ? 2 : 1);
        mu_kstr(&items_json, "folder", items[i].folder, 1);
        mu_kbool(&items_json, "ok", ok, 0);
        mu_kstr(&items_json, "key", ok && key ? key : "", 0);
        mu_kstr(&items_json, "cause", ok || !plain ? "" : plain, 0);
        mu_knum(&items_json, "editorRows", mod_rows, 0);
        mu_kbool(&items_json, "replaced", st.replaced, 0);
        mu_putn(&items_json, "}", 1);
        sh_mb_free(&bd);
        free(key);
        free(cause);
        free(plain);
    }
    mu_putn(&items_json, "]", 1);
    if (installed) mu_sync();
    {
        const char *all = refused_all.p && !refused_all.oom ? refused_all.p : "";
        if (installed && !refused)
            msg = sh_textf("Installed %u mod%s%s.", installed, installed == 1 ? "" : "s",
                           rows ? "; their Toybox rows are in the palette" : "");
        else if (installed)
            msg = sh_textf("Installed %u mod%s; %u %s not installed.%s", installed, installed == 1 ? "" : "s",
                           refused, refused == 1 ? "was" : "were", all);
        else msg = sh_textf("Nothing was installed.%s", all);
    }
    msg = mu_record_note(msg);
    mu_kstr(&x, "key", first_key ? first_key : "", 0);
    mu_knum(&x, "installedCount", installed, 0);
    mu_knum(&x, "refusedCount", refused, 0);
    mu_knum(&x, "editorRows", rows, 0);
    mu_key(&x, "items", 0);
    mu_putn(&x, items_json.p && !items_json.oom ? items_json.p : "[]", items_json.p && !items_json.oom ? items_json.len : 2);
    free(items_json.p);
    rc = mu_file_result(SH_MODS_ACT_INSTALL, installed > 0, msg ? msg : "", &x, installed > 0);
out:
    for (i = 0; i < n; i++) { free(items[i].folder); free((void *)items[i].why); }
    free(items);
    for (i = 0; i < zips; i++) {
        wchar_t *w = unpacked[i] ? sh_mb_wpath(unpacked[i]) : NULL;
        if (w) mu_remove_tree(w);
        free(w);
        free(unpacked[i]);
    }
    free(unpacked);
    free(first_key);
    free(msg);
    free(refused_all.p);
    return rc;
}

/* ---- verify */

/* `sh_mods doctor verify`: rehash every loaded folder, adding the results
 * up. Each failure is also printed to `o`. Returns 0 when the store cannot be
 * read. */
static int mu_verify_all(const mu_out *o, sh_ms_verify_stats *sum, unsigned *passed, unsigned *failed, char **first)
{
    sh_ms_list list;
    sh_ms_verify_stats st;
    char *cause = NULL;
    size_t i;
    memset(sum, 0, sizeof *sum);
    *passed = *failed = 0;
    *first = NULL;
    if (!sh_ms_enumerate(&list)) return 0;
    for (i = 0; i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        int ok;
        if (!e->loaded) continue;
        memset(&st, 0, sizeof st);
        free(cause);
        cause = NULL;
        ok = sh_ms_verify(e->name, &st, &cause);
        sum->files += st.files;
        sum->bytes_mb += st.bytes_mb;
        sum->pointers += st.pointers;
        sum->entries += st.entries;
        if (ok) (*passed)++;
        else {
            (*failed)++;
            if (!*first) *first = sh_textf("%s: %s", e->name, cause ? cause : "");
            mu_say(o, "verify %s: FAILED: %s", e->name, cause ? cause : "");
        }
        mu_log("verify %s: %s%s%s", e->name, ok ? "pass" : "FAILED", ok ? "" : ": ", ok || !cause ? "" : cause);
    }
    free(cause);
    sh_ms_list_free(&list);
    return 1;
}

/* malloc'd. */
static char *mu_verify_message(unsigned passed, unsigned failed, const char *first)
{
    if (!passed && !failed) return sh_text("No installed mod to check.");
    if (!failed) return sh_textf("Checked %u mod%s: every file is as it was built.", passed, passed == 1 ? "" : "s");
    return sh_textf("%u of %u mods have files that changed since they were built; press Refresh to build them again. "
                    "First: %s", failed, passed + failed, first ? first : "");
}

/* What one removal did. */
typedef struct mu_removal {
    size_t removed, failed;
    unsigned long long freed;
    char *first_fail;            /* malloc'd, or NULL */
    char *deleted;               /* the last folder removed (its name in mods\; malloc'd), or NULL */
    size_t outright;             /* removed folders the Recycle Bin could not take, so deleted */
    char *outright_why;          /* why the bin could not take the first of them (malloc'd), or NULL */
} mu_removal;

static void mu_removal_free(mu_removal *r)
{
    free(r->first_fail);
    free(r->deleted);
    free(r->outright_why);
    r->first_fail = r->deleted = r->outright_why = NULL;
}

static const char *mu_entry_title(const sh_ms_entry *s);

/* Move the marked entries' folders to the Windows Recycle Bin under the store lock (sh_ms_delete_mod_folder: a
 * direct child of the store root; links are removed, never followed; deleted outright, counted in `outright`, when
 * the bin cannot take it). An entry whose folder cannot be removed is unmarked and counted as failed. */
static void mu_remove_marked(const mu_model *m, unsigned char *removing, mu_removal *out)
{
    size_t n = m->list.count, i;
    memset(out, 0, sizeof *out);
    modstore_lock();
    for (i = 0; i < n; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        char *cause = NULL;
        int how;
        if (!removing[i]) continue;
        if ((how = sh_ms_delete_mod_folder(e->folder, &cause)) == 0) {
            out->failed++;
            if (!out->first_fail) out->first_fail = _strdup(cause ? cause : "");
            removing[i] = 0;
            mu_log("remove %s: the folder could not be deleted: %s", e->name, cause ? cause : "");
            free(cause);
            continue;
        }
        out->removed++;
        out->freed += e->bundle.total_size;
        free(out->deleted);
        out->deleted = _strdup(mu_leaf(e->folder));
        if (how == SH_MS_DELETED) {
            if (!out->outright++) out->outright_why = _strdup(cause ? cause : "");
            mu_log("remove %s: mods\\%s deleted outright; the Recycle Bin could not take it (%s)", e->name, mu_leaf(e->folder),
                   cause ? cause : "");
        } else {
            mu_log("remove %s: mods\\%s moved to the Recycle Bin", e->name, mu_leaf(e->folder));
        }
        free(cause);
    }
    if (out->removed) {
        sh_modserve_refresh rf;
        char why[200];
        if (mu_rescan(&rf, why, sizeof why) < 0) mu_log("uninstall: %s", why);
        sh_cache_sweep_async();   /* a mod uninstalled: the content cache is swept */
    }
    modstore_unlock();
}

/* ---- the open map: removal of one mod, and uninstall */

/* One console line, "MODS: " and the text (the console verbs share these
 * actions with the tab). */
static void mu_line(const mu_out *o, const char *fmt, ...)
{
    char *body, *line;
    va_list ap;
    if (!o || !o->fn) return;
    va_start(ap, fmt);
    body = sh_vtextf(fmt, ap);
    va_end(ap);
    line = sh_textf("MODS: %s", body ? body : "(out of memory)");
    free(body);
    o->fn(o->context, line ? line : "MODS: (out of memory)");
    free(line);
}

/* Set *msg (malloc'd, the old one freed) to the sentence, whole. */
static void mu_msg(char **msg, const char *fmt, ...)
{
    va_list ap;
    char *text;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    if (text) { free(*msg); *msg = text; }
}

/* A row another build of its mod serves (the root folder of the player's own map, or a build an editor load
 * substituted): the next save records that build, and Remove from map drops the row of that mod whatever build
 * it names. */
static void mu_say_substituted(const mu_out *o, const mu_model *m, const char *prefix, const char *salt,
                               const char *declared, const char *serving)
{
    int at = mu_find_hash(m, serving), named = mu_find_salt(m, salt, -1);
    const sh_ms_entry *e = at >= 0 ? &m->list.items[at] : named >= 0 ? &m->list.items[named] : NULL;
    (void)declared;
    mu_line(o, "%s%s: the map was saved with an earlier copy of it; mods\\%s serves it now, and the next save "
               "records that one", prefix, e ? mu_entry_title(e) : "a mod", e ? e->name : "?");
}

/* Each row of the open map another build serves. */
static void mu_say_substitutions(const mu_out *o, const mu_model *m, const char *prefix)
{
    sh_modmanifest_list rows;
    size_t i;
    if (!sh_modmanifest_current(&rows)) return;
    for (i = 0; i < rows.count; i++) {
        char sub[SH_MODMANIFEST_HASH_CAP];
        if (sh_modmanifest_substituted(rows.rows[i].build, sub, sizeof sub) ||
            mu_row_served_by(&rows.rows[i], sub, sizeof sub))
            mu_say_substituted(o, m, prefix, rows.rows[i].salt, rows.rows[i].build, sub);
    }
    sh_modmanifest_list_free(&rows);
}

/* 1 with the cause when the game cannot take a change now. */
static int mu_busy(char *cause, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    cause[0] = 0;
    return e->busy && e->busy(cause, cap) == 1;
}

/* ---- the undo history (Uninstall) */

/* The open map's undo and redo history is cleared after an Uninstall so no snapshot older than its delete remains
 * (Ctrl+Z would bring back entities of the gone mod). The native "Delete Selection" commit lands on later editor
 * frames, so the tick (sh_modui_poll_due, every frame while a clear is owed) waits for it. */
static volatile LONG g_undo_owed;
static ULONGLONG g_undo_owed_at;
static char *g_undo_owed_key;   /* malloc'd, or NULL */
static ULONGLONG g_poll_next;
#define MU_UNDO_WAIT_MS 10000

/* A map remove's owed row drop (mu_drop_tick, with mu_map_apply). */
static volatile LONG g_drop_owed;
static ULONGLONG g_drop_owed_at;
static char *g_drop_key;        /* malloc'd, or NULL */
static char g_drop_hash[SH_MODSERVE_HASH_HEX + 1], g_drop_salt[SH_MB_ID_MAX + 1];
static char (*g_drop_builds)[SH_MODMANIFEST_HASH_CAP];   /* malloc'd: every row the remove drops */
static int g_drop_n, g_drop_tag;
static void mu_drop_tick(void);

/* Clear now. 1 cleared, 0 no editor map (nothing to clear), -1 failed. */
static int mu_undo_clear_now(const char *key)
{
    const sh_modui_engine *e = mu_engine();
    int r;
    if (!e->clear_undo) return -1;
    r = e->clear_undo(0);
    mu_log("uninstall %s: undo history %s", key && key[0] ? key : "?",
           r > 0 ? "cleared to the current state" : r == 0 ? "not cleared: no map is open in the editor" : "could NOT be cleared");
    return r;
}

/* The tick's part: once the editor's delete has committed, clear. */
static void mu_undo_tick(void)
{
    const sh_modui_engine *e = mu_engine();
    int pending;
    if (!InterlockedCompareExchange(&g_undo_owed, 0, 0)) return;
    pending = e->undo_pending ? e->undo_pending() : 0;
    if (pending > 0 && GetTickCount64() - g_undo_owed_at < MU_UNDO_WAIT_MS) return;
    if (pending > 0) mu_log("uninstall %s: the editor's delete did not commit within %u ms; clearing now",
                            g_undo_owed_key ? g_undo_owed_key : "?", (unsigned)MU_UNDO_WAIT_MS);
    InterlockedExchange(&g_undo_owed, 0);
    (void)mu_undo_clear_now(g_undo_owed_key);
    InterlockedIncrement(&g_gen);
}

static void mu_undo_owe(const char *key)
{
    free(g_undo_owed_key);
    g_undo_owed_key = _strdup(key ? key : "");
    g_undo_owed_at = GetTickCount64();
    g_poll_next = 0;
    InterlockedExchange(&g_undo_owed, 1);
}

/* ---- saved maps that use a mod (the Uninstall confirmation) */

typedef struct mu_salt_use { const char *salt; int count; } mu_salt_use;

static void mu_salt_use_one(void *ctx, const char *local_id, const char *item_id, const char *text, size_t len)
{
    mu_salt_use *u = (mu_salt_use *)ctx;
    sh_modmanifest_list rows;
    size_t i;
    int uses = 0;
    (void)local_id;
    (void)item_id;
    memset(&rows, 0, sizeof rows);
    if (sh_modmanifest_read(text, len, &rows, NULL))
        for (i = 0; i < rows.count && !uses; i++) uses = !_stricmp(rows.rows[i].salt, u->salt);
    sh_modmanifest_list_free(&rows);
    u->count += uses;
}

/* How many local saves carry a manifest row of `salt`; -1 when the saves
 * folder cannot be read. */
static int mu_saved_maps_using(const char *salt)
{
    mu_salt_use u;
    if (!salt || !salt[0]) return -1;
    u.salt = salt;
    u.count = 0;
    return sh_saved_maps_each(mu_salt_use_one, &u, NULL) < 0 ? -1 : u.count;
}

/* ---- map scope (modserve.h): what the open map lists */

/* A save changed the listing (applied on the next editor tick), and whether
 * the editor has shown the map loaded last (the tick clears the listing once
 * that editor is gone). */
static volatile LONG g_scope_owed;
static volatile LONG g_scope_editor;
static volatile LONG g_placed_reset = 1;

/* A hub launch's listing and what the product has seen of its Play, so an idle poll between the gate and the level
 * load is not taken for the end of the Play. NONE -> LAUNCHED (the gate passed; its rows are declared) -> LOADING
 * (busy seen) -> PLAYING (the Play-begin gate ran: sh_modserve_play_begin asks mu_scope_source at AT_PLAY) -> RUNNING
 * (busy seen after the Play began) -> ENDED (the first idle poll after RUNNING: the tick clears the listing and the
 * rows, back to NONE). The editor's return ends a launch without the hub clear; a refused launch
 * (sh_modui_scope_launch_refused) clears or restores at once. */
enum { MU_LAUNCH_NONE, MU_LAUNCH_LAUNCHED, MU_LAUNCH_LOADING, MU_LAUNCH_PLAYING, MU_LAUNCH_RUNNING, MU_LAUNCH_ENDED };
static volatile LONG g_launch;

#ifdef SH_MODUI_TESTING
static const char *mu_launch_name(LONG state)
{
    switch (state) {
    case MU_LAUNCH_LAUNCHED: return "launched";
    case MU_LAUNCH_LOADING: return "loading";
    case MU_LAUNCH_PLAYING: return "playing";
    case MU_LAUNCH_RUNNING: return "running";
    case MU_LAUNCH_ENDED: return "ended";
    default: return "none";
    }
}
const char *sh_modui_test_launch_state(void) { return mu_launch_name(InterlockedCompareExchange(&g_launch, 0, 0)); }
#endif

/* The busy callback answered 1 (Play or a load) while a launch listing holds. */
static void mu_launch_busy(void)
{
    LONG at = InterlockedCompareExchange(&g_launch, 0, 0);
    if (at == MU_LAUNCH_LAUNCHED) InterlockedCompareExchange(&g_launch, MU_LAUNCH_LOADING, MU_LAUNCH_LAUNCHED);
    else if (at == MU_LAUNCH_PLAYING) InterlockedCompareExchange(&g_launch, MU_LAUNCH_RUNNING, MU_LAUNCH_PLAYING);
}

/* The Play-begin gate ran: the launched map's Play is being built. */
static void mu_launch_play_begin(void)
{
    LONG at = InterlockedCompareExchange(&g_launch, 0, 0);
    if ((at == MU_LAUNCH_LAUNCHED || at == MU_LAUNCH_LOADING) &&
        InterlockedCompareExchange(&g_launch, MU_LAUNCH_PLAYING, at) == at)
        mu_log("scope: the launched map's Play began; its rows hold until that Play is over");
}

/* An idle poll away from the editor: 1 when the launched Play ran and is over (RUNNING becomes ENDED); any other
 * state is unchanged. */
static int mu_launch_idle(void)
{
    return InterlockedCompareExchange(&g_launch, MU_LAUNCH_ENDED, MU_LAUNCH_RUNNING) == MU_LAUNCH_RUNNING;
}

/* The build the row resolver (sh_modserve_row_build) names for
 * `row` when it is not the row's own: 1 with its hash in `out`. */
static int mu_row_served_by(const sh_modmanifest_row *row, char *out, size_t cap)
{
    sh_modserve_bundle_ref ref;
    int other;
    memset(&ref, 0, sizeof ref);
    other = sh_modserve_row_build(row, &ref) && strlen(ref.hash) == SH_MODSERVE_HASH_HEX && _stricmp(ref.hash, row->build) &&
            strlen(ref.hash) < cap;
    if (other) strcpy_s(out, cap, ref.hash);
    sh_modserve_bundle_ref_free(&ref);
    return other;
}

/* The listing from manifest rows and placed names (modmanifest.h sh_modmanifest_listing, the one rule every mode
 * declares by), set on the serving layer. Returns the bundles that changed. */
static int mu_listing_set(const sh_modmanifest_list *rows, const char *const *names, size_t name_count, int map_open)
{
    char (*hashes)[SH_MODMANIFEST_HASH_CAP] = NULL;
    const char **list = NULL;
    int n = sh_modmanifest_listing(rows, names, name_count, &hashes), i, changed;
    if (n < 0 || (n > 0 && !(list = (const char **)malloc((size_t)n * sizeof *list)))) {
        free(hashes);
        mu_log("scope: the map's listing could not be computed (out of memory); the listing in force is kept");
        return -1;
    }
    for (i = 0; i < n; i++) list[i] = hashes[i];
    changed = sh_modserve_scope_set(list, n, map_open);
    free((void *)list);
    free(hashes);
    return changed;
}

/* The editor uses the same attached, pinned and placed rows its next save would write. */
static int mu_listing_placed(const mu_names *placed)
{
    const char *const *names = (const char *const *)placed->items;
    sh_modmanifest_list rows;
    int changed;
    if (sh_modmanifest_version_rule(names, placed->count) > 0) InterlockedIncrement(&g_gen);
    if (sh_modmanifest_rows_for(names, placed->count, &rows) < 0) return -1;
    changed = mu_listing_set(&rows, NULL, 0, 1);
    sh_modmanifest_list_free(&rows);
    return changed;
}

static int mu_listing_live(void)
{
    const sh_modui_engine *e = mu_engine();
    mu_names placed = {0};
    int changed = 0;
    if (e->placed_names && e->placed_names(mu_names_add, &placed) >= 0 && !placed.oom)
        changed = mu_listing_placed(&placed);
    mu_names_free(&placed);
    return changed > 0 ? changed : 0;
}

static int mu_scope_apply(const char *phase)
{
    const sh_modui_engine *e = mu_engine();
    InterlockedExchange(&g_scope_owed, 0);
    return e->scope_apply ? e->scope_apply(phase) : 0;
}

static void mu_scope_activate(const char *phase)
{
    const sh_modui_engine *e = mu_engine();
    if (e->scope_activate) {
        InterlockedExchange(&g_scope_owed, 0);
        (void)e->scope_activate(phase);
    } else (void)mu_scope_apply(phase);
}

/* After a Mods tab or console change to the open map: applied at once. */
static int mu_scope_now(const char *phase)
{
    int changed;
    if (!sh_modserve_scope_enabled()) return 0;
    changed = mu_listing_live();
    if (changed) (void)mu_scope_apply(phase);
    return changed;
}

/* A mod the author added to the open map: its decls are
 * registered and its registries served at once, as a load would. */
static int mu_scope_now_declared(const char *phase)
{
    int changed;
    if (!sh_modserve_scope_enabled()) return 0;
    changed = mu_listing_live();
    /* A composition can become ready while the map still lists the same bundle. */
    mu_scope_activate(phase);
    return changed;
}

int sh_modui_scope_refresh(const char *why) { return mu_on_main() ? mu_scope_now_declared(why) : -1; }

void sh_modui_prefab_finished(void)
{
    if (!sh_modserve_prefab_pending()) return;
    (void)sh_modui_scope_refresh("prefab placement");
    (void)sh_modserve_prefab_files(NULL, 0);
}

static int mu_scope_armed(void) { return sh_modserve_scope_relevant(); }

/* The listing a load's hook just set, for that load's activation
 * (modmanifest.h sh_modmanifest_set_listed); with the scope off, none. */
static int mu_scope_listed(char (*out)[SH_MODMANIFEST_HASH_CAP], int cap)
{
    if (!sh_modserve_scope_enabled()) return 0;
    return sh_modserve_scope_hashes(out, cap);
}

/* The manifest's hook: a load applies at once (before activation), a save on
 * the next editor tick. */
static void mu_scope_hook(const sh_modmanifest_list *rows, const char *const *names, size_t name_count, int when)
{
    int changed;
    /* A map is opened: an open Mods tab gets a fresh document (the window's pushed refresh). */
    if (when == SH_MODSERVE_AT_LOAD) {
        (void)sh_modserve_prefab_files(NULL, 0);
        InterlockedIncrement(&g_gen);
        InterlockedExchange(&g_placed_reset, 1);
    }
    if (!sh_modserve_scope_enabled()) return;
    if (when == SH_MODSERVE_AT_LOAD) InterlockedExchange(&g_scope_editor, 0);
    changed = mu_listing_set(rows, names, name_count, 1);
    if (changed <= 0) return;
    if (when == SH_MODSERVE_AT_LOAD && mu_on_main()) (void)mu_scope_apply("map load");
    else InterlockedExchange(&g_scope_owed, 1);
}

/* The serving layer's source at the Play entry: only the editor's Play reads the open map live; a hub launch keeps
 * the listing its gate set (sh_modui_scope_launch), since the editor's entities and manifest are not the launched
 * map's. */
static int mu_scope_source(int when)
{
    if (!sh_modserve_scope_enabled() || !mu_on_main()) return 0;
    if (when == SH_MODSERVE_AT_PLAY) mu_launch_play_begin();
    if (!InterlockedCompareExchange(&g_scope_editor, 0, 0)) return 0;
    return mu_listing_live();
}

int sh_modui_placed_names(char ***names, size_t *count)
{
    const sh_modui_engine *e = mu_engine();
    mu_names placed;
    if (!names || !count) return -1;
    *names = NULL;
    *count = 0;
    if (!mu_on_main() || !e->placed_names) return -1;
    memset(&placed, 0, sizeof placed);
    if (e->placed_names(mu_names_add, &placed) < 0 || placed.oom) { mu_names_free(&placed); return -1; }
    free(placed.slots);
    *names = placed.items;
    *count = placed.count;
    return (int)placed.count;
}

int sh_modui_scope_launch(const char *why)
{
    const sh_modui_engine *e = mu_engine();
    sh_modmanifest_list rows;
    int changed = 0, moved;
    const char *at = why && why[0] ? why : "launch";
    if (!mu_on_main()) return 0;
    /* Once per launch, at the gate, before anything else of it is set: the decls the level load would parse are parsed
     * here at the hub, in editor mode, as an editor entry parses them. A second boundary of the same launch
     * (launched or loading) does not repeat it. */
    if (e->launch_preload && InterlockedCompareExchange(&g_launch, 0, 0) != MU_LAUNCH_LAUNCHED &&
        InterlockedCompareExchange(&g_launch, 0, 0) != MU_LAUNCH_LOADING) {
        int r = e->launch_preload(at);
        if (r < 0) mu_log("scope: %s: the pre-load pass was refused; the level load parses the mods' decls", at);
    }
    if (!sh_modserve_scope_enabled()) {
        if (sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) > 0) (void)mu_scope_apply(at);
        return 0;
    }
    InterlockedExchange(&g_scope_editor, 0);
    InterlockedExchange(&g_launch, MU_LAUNCH_LAUNCHED);
    (void)sh_modmanifest_current(&rows);
    /* The rows alone: a launched map declares what it carries rows for. */
    changed = mu_listing_set(&rows, NULL, 0, 1);
    if (changed < 0) changed = 0;
    moved = sh_modserve_scope_mode(SH_MODSERVE_MODE_PLAY) > 0;
    mu_log("scope: %s: the launched map declares %zu row(s)%s; they hold until its Play has run and ended", at,
           rows.count, rows.count ? "; nothing else serves in its Play" : "; no mod serves in its Play");
    sh_modmanifest_list_free(&rows);
    if (changed || moved) (void)mu_scope_apply(at);
    return changed;
}

int sh_modui_editor_shows_map(void)
{
    const sh_modui_engine *e = mu_engine();
    sh_modui_map_info info;
    if (!e->map_info) return -1;
    return e->map_info(&info) == 1;
}

int sh_modui_game_busy(void)
{
    char cause[200];
    return mu_busy(cause, sizeof cause);
}

int sh_modui_scope_launch_refused(const char *why)
{
    const sh_modui_engine *e = mu_engine();
    sh_modui_map_info info;
    const char *at = why && why[0] ? why : "launch";
    int editor, changed = 0, moved;
    if (!mu_on_main()) return 0;
    InterlockedExchange(&g_launch, MU_LAUNCH_NONE);
    moved = sh_modserve_scope_mode_get() != SH_MODSERVE_MODE_EDITOR && sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR) > 0;
    if (!sh_modserve_scope_enabled()) {
        if (moved) (void)mu_scope_apply("launch refused");
        return 0;
    }
    /* The editor still shows its map (the launch was asked over it): that map's rows, which the launched map's parse
     * replaced, come back. Elsewhere (the browser, the hub) nothing was launched, so nothing is declared. */
    editor = e->map_info && e->map_info(&info) == 1;
    if (editor && sh_modmanifest_current_restore_previous() >= 0) {
        mu_settings_restore();
        InterlockedExchange(&g_scope_editor, 1);
        changed = mu_listing_live();
        mu_log("scope: %s was refused; nothing was launched, so the editor map's rows are declared again", at);
    } else {
        sh_modmanifest_current_close();
        mu_settings_close();
        InterlockedExchange(&g_scope_editor, 0);
        changed = sh_modserve_scope_set(NULL, 0, 0);
        mu_log("scope: %s was refused; nothing was launched, so nothing is declared", at);
    }
    if (changed || moved) (void)mu_scope_apply("launch refused");
    return changed;
}

/* The build that serves a row at save (modmanifest.h
 * sh_modmanifest_set_resolver): the one sh_modserve_row_build names in the
 * open map's context. */
static int mu_resolve_row(const char *build, const char *salt, sh_modmanifest_row *out)
{
    sh_modserve_bundle_ref ref;
    sh_modmanifest_row probe;
    int ok;
    /* A salt and a build hash have one width each (modmanifest.h). */
    if (!out || !salt || !build || strlen(salt) >= sizeof probe.salt || strlen(build) >= sizeof probe.build) return 0;
    memset(&probe, 0, sizeof probe);
    memset(&ref, 0, sizeof ref);
    strcpy_s(probe.salt, sizeof probe.salt, salt);
    strcpy_s(probe.build, sizeof probe.build, build);
    ok = sh_modserve_row_build(&probe, &ref) && strlen(ref.id) < sizeof out->salt && strlen(ref.hash) < sizeof out->build;
    if (ok) {
        strcpy_s(out->salt, sizeof out->salt, ref.id);
        strcpy_s(out->build, sizeof out->build, ref.hash);
        ok = sh_modmanifest_row_set_name(out, ref.name);
        out->size = ref.size;
    }
    sh_modserve_bundle_ref_free(&ref);
    return ok;
}

/* The tick's part: in the editor, a save's change is applied and an editor map without a listing gets one; once the
 * editor has left the map (the hub or the browser) its listing goes. A menu screen over the open map also takes the
 * editor screen down, so the exit waits for map_left. */
static void mu_scope_tick(int editor_map)
{
    const sh_modui_engine *e = mu_engine();
    if (!sh_modserve_scope_enabled()) return;
    if (!editor_map) {
        if (InterlockedCompareExchange(&g_scope_editor, 0, 0) && sh_modserve_scope_map_open() &&
            !(e->map_left && e->map_left() == 0)) {
            /* The rows go with the listing: a hub launch or a Play entry must
             * not read the editor map's manifest as its own. */
            InterlockedExchange(&g_scope_editor, 0);
            InterlockedExchange(&g_placed_reset, 1);
            InterlockedIncrement(&g_gen);
            sh_modmanifest_current_close();
            mu_settings_close();
            if (sh_modserve_scope_set(NULL, 0, 0)) {
                mu_log("scope: the editor left the map; its mods' rows, shadows and requirements are withdrawn");
                (void)mu_scope_apply("map exit");
            }
        } else if (mu_launch_idle()) {
            /* ENDED: the launched Play began, the game was busy with it, then idle, so that Play is over. An idle poll
             * before it ran (the gap between the gate and the level load) does not end it. */
            InterlockedExchange(&g_launch, MU_LAUNCH_NONE);
            sh_modmanifest_current_close();
            mu_settings_close();
            if (sh_modserve_scope_set(NULL, 0, 0)) {
                mu_log("scope: the launched map's Play is over; nothing is declared at the hub");
                (void)mu_scope_apply("hub");
            }
        }
        return;
    }
    InterlockedExchange(&g_launch, MU_LAUNCH_NONE);
    InterlockedExchange(&g_scope_editor, 1);
    /* Back in the editor (after Play): undeclared bundles offer their names again. */
    if (sh_modserve_scope_mode_get() != SH_MODSERVE_MODE_EDITOR && sh_modserve_scope_mode(SH_MODSERVE_MODE_EDITOR))
        (void)mu_scope_apply("editor");
    if (!sh_modserve_scope_map_open() && sh_modserve_scope_relevant()) {
        if (mu_listing_live()) mu_scope_activate("editor");
    } else if (InterlockedCompareExchange(&g_scope_owed, 0, 0)) (void)mu_scope_apply("save");
}

int sh_modui_poll_due(void)
{
    ULONGLONG now;
    /* An owed undo clear is checked after every native frame: it must land
     * before the next frame's input can step the history back. */
    if (InterlockedCompareExchange(&g_undo_owed, 0, 0) || InterlockedCompareExchange(&g_drop_owed, 0, 0)) return 1;
    if (!(sh_modserve_scope_enabled() && (InterlockedCompareExchange(&g_scope_owed, 0, 0) || sh_modserve_scope_map_open() ||
                                          sh_modserve_scope_relevant())))
        return 0;
    now = GetTickCount64();
    if (now < g_poll_next) return 0;
    g_poll_next = now + 250;
    return 1;
}

int sh_modui_poll_main(void *context)
{
    const sh_modui_engine *e = mu_engine();
    sh_modui_map_info info;
    char cause[200];
    (void)context;
    if (!mu_on_main()) return 0;
    if (mu_busy(cause, sizeof cause)) {                             /* Play or a load: later */
        mu_launch_busy();
        return 0;
    }
    if (!e->map_info || e->map_info(&info) != 1) {                  /* the editor, with a map */
        /* The map the clear was owed for is gone; so is its history. */
        if (InterlockedExchange(&g_undo_owed, 0))
            mu_log("uninstall %s: the editor left the map before the clear", g_undo_owed_key ? g_undo_owed_key : "");
        if (InterlockedExchange(&g_drop_owed, 0))
            mu_log("map remove %s: the editor left the map before the delete committed", g_drop_key ? g_drop_key : "");
        mu_scope_tick(0);
        return 0;
    }
    mu_drop_tick();
    mu_undo_tick();
    mu_scope_tick(1);
    {
        /* Native placement, deletion and undo change scope independently of saves and the Mods tab.
         * Only a changed set of mod names needs its manifest closure and engine requirements applied. */
        static ULONGLONG next;
        static const void *last_snapshot;
        static unsigned long last_revision;
        static unsigned long long last;
        static int known;
        ULONGLONG now = GetTickCount64();
        if (InterlockedExchange(&g_placed_reset, 0)) { known = 0; next = 0; }
        if (e->placed_names && sh_modserve_active() && now >= next) {
            mu_names nm;
            int r;
            unsigned long revision = 0;
            const void *snapshot = e->placement_revision ? e->placement_revision(&revision) : NULL;
            next = now + 1000;
            if (known && snapshot && snapshot == last_snapshot && revision == last_revision) return 1;
            memset(&nm, 0, sizeof nm);
            r = e->placed_names(mu_names_add, &nm);
            if (r >= 0 && !nm.oom) {
                unsigned long long sig = 0, n = 0;
                size_t i;
                for (i = 0; i < nm.count; i++) {
                    const char *p;
                    for (p = nm.items[i]; *p && _strnicmp(p, "mods/", 5); p++) {}
                    if (!*p) continue;
                    sig += (unsigned long long)mu_hash_ci(nm.items[i]) * 2654435761ull + 1;
                    n++;
                }
                sig ^= n << 48;
                if (known ? sig != last : n > 0) {
                    int changed = sh_modserve_scope_enabled() ? mu_listing_placed(&nm) : 0;
                    if (changed < 0) { mu_names_free(&nm); return 1; }
                    if (changed > 0) mu_scope_activate("editor placement");
                    InterlockedIncrement(&g_gen);
                }
                last_snapshot = snapshot;
                last_revision = revision;
                last = sig;
                known = 1;
            }
            mu_names_free(&nm);
        }
    }
    return 1;
}

/* The map entry `key` names: the installed folder's build, else a row of
 * the folder's salt that another build serves, else (a mod that is not
 * installed) a row whose salt is `key`. -1 when the map does not use it. */
static int mu_map_find(const mu_model *m, const mu_map *mp, const char *key)
{
    size_t i;
    int entry = -1;
    for (i = 0; i < m->list.count && entry < 0; i++)
        if (!strcmp(m->list.items[i].name, key) && m->list.items[i].loaded) entry = (int)i;
    for (i = 0; entry >= 0 && i < mp->mod_count; i++)
        if (mu_same_bundle(m, mp->mods[i].hash, m->list.items[entry].bundle.hash)) return (int)i;
    for (i = 0; entry >= 0 && i < mp->mod_count; i++)
        if (!_stricmp(mp->mods[i].id, m->list.items[entry].salt)) return (int)i;
    for (i = 0; i < mp->mod_count; i++)
        if (mu_mapmod_entry(m, &mp->mods[i]) < 0 && !_stricmp(mp->mods[i].id, key)) return (int)i;
    return -1;
}

/* What removing one mod from the open map does. */
typedef struct mu_plan {
    int mod;                  /* the map entry, -1 when the map does not use it */
    char refusal[400];        /* why it cannot be done, "" when it can */
    sh_mm_ref *refs;
    size_t ref_count;
    int manifest_row, tag_removed, vanilla_after, published;
    int attached;             /* its row is map-level */
} mu_plan;

/* A map entry that only another mod's needs reach: it goes with that mod. */
static int mu_mapmod_follows(const mu_mapmod *x)
{
    return x->in_closure && !x->named && !x->uid_count && !x->modules;
}

static void mu_plan_build(const mu_model *m, const mu_map *mp, const char *key, mu_plan *p)
{
    const mu_mapmod *x;
    size_t i, others = 0;
    memset(p, 0, sizeof *p);
    p->mod = mp->entities >= 0 ? mu_map_find(m, mp, key) : -1;
    if (p->mod < 0) return;
    x = &mp->mods[p->mod];
    if (!mp->have_mm)
        strncpy_s(p->refusal, sizeof p->refusal, "Snapmap+ cannot read this map's objects in this game version; use Send "
                  "feedback", _TRUNCATE);
    else if (x->modules)
        _snprintf_s(p->refusal, sizeof p->refusal, _TRUNCATE, "the map places %d of its modules; delete them in Blueprint mode first",
                    x->modules);
    else if (mu_mapmod_follows(x))
        strncpy_s(p->refusal, sizeof p->refusal, "another mod this map uses needs it; remove that mod from the map first", _TRUNCATE);
    else if (x->uid_count > SH_EDITOR_DELETE_MAX)
        _snprintf_s(p->refusal, sizeof p->refusal, _TRUNCATE, "it has %zu placed objects; delete some of them in the editor "
                    "first (at most %d go in one step)", x->uid_count, SH_EDITOR_DELETE_MAX);
    if (x->uid_count && mp->have_mm) {
        p->ref_count = sh_mm_references(&mp->mm, x->uids, x->uid_count, &p->refs);
        if (p->ref_count == (size_t)-1) { p->ref_count = 0; p->refs = NULL; }
    }
    for (i = 0; i < mp->mod_count; i++) if ((int)i != p->mod && !mu_mapmod_follows(&mp->mods[i])) others++;
    p->manifest_row = x->in_rows;
    p->attached = x->attached;
    p->vanilla_after = others == 0;
    p->tag_removed = p->vanilla_after && mp->have_info && mp->info.tagged == 1;
    p->published = mp->link != NULL;
}

static void mu_plan_free(mu_plan *p) { free(p->refs); p->refs = NULL; }

static const char *mu_delete_cause(int status)
{
    switch (status) {
    case SH_EDITOR_DELETE_NOT_MAIN:    return "the editor did not answer; try again";
    case SH_EDITOR_DELETE_NOT_READY:   return "the editor is not ready; open the map in the editor and try again";
    case SH_EDITOR_DELETE_NOT_ENTITY:  return "the editor is not in Object mode; switch to it and try again";
    case SH_EDITOR_DELETE_BUSY:        return "the editor is busy (an entity or a wire is held, or an edit is finishing); try again";
    case SH_EDITOR_DELETE_INVALID:     return "an entity is no longer in the map; refresh and try again";
    case SH_EDITOR_DELETE_RESTRICTED:  return "this map does not allow deleting entities";
    case SH_EDITOR_DELETE_PORTAL:      return "one of its entities is a portal door or cap; delete it in the editor";
    case SH_EDITOR_DELETE_PROTECTED:   return "the editor does not allow deleting one of its entities";
    case SH_EDITOR_DELETE_REFERENCED:  return "another object refers to these entities and the editor asks before deleting them; "
                                              "delete them in the editor, then try again";
    case SH_EDITOR_DELETE_FAULT:       return "the editor stopped the delete with an error; save your map, check it, and "
                                              "use Send feedback if it repeats";
    case SH_EDITOR_DELETE_INCOMPLETE:  return "the delete did not take every entity; check the map";
    default:                           return "Snapmap+ cannot delete objects in this game version; delete them in the "
                                              "editor yourself";
    }
}

/* The rows a map remove drops are never dropped before the map holds none of the mod's objects: the native delete
 * commits its undo step on later editor frames, so the drop is owed to the tick (mu_drop_tick), which reads the map
 * again and drops the rows only when no object of the mod is left. Main thread (the state sits with the undo
 * history's, above). */

/* Drop the noted rows, with the last mod the search tag, and bring the
 * change into the game. */
static void mu_drop_rows(const char *key, char builds[][SH_MODMANIFEST_HASH_CAP], int n, int untag)
{
    const sh_modui_engine *e = mu_engine();
    size_t rows = 0;
    int i, tag = 0;
    for (i = 0; i < n; i++) rows += sh_modmanifest_current_remove(builds[i]);
    if (untag && e->remove_tag) tag = e->remove_tag(MU_TAG);
    (void)mu_scope_now("map remove");
    InterlockedIncrement(&g_gen);
    mu_log("map remove %s: %zu row(s) of the map's list of mods dropped%s", key, rows,
           !untag ? "" : tag > 0 ? ", search tag 'mods' removed" : tag == 0 ? ", the search tag was already absent"
                                                                   : ", the search tag could not be removed");
}

/* The tick's part: once the delete has committed (or MU_UNDO_WAIT_MS went
 * by), the owed drop happens when the map holds none of the mod's objects. */
static void mu_drop_tick(void)
{
    const sh_modui_engine *e = mu_engine();
    mu_model m;
    mu_map mp;
    size_t i;
    int left = 0;
    if (!InterlockedCompareExchange(&g_drop_owed, 0, 0)) return;
    if (e->undo_pending && e->undo_pending() > 0 && GetTickCount64() - g_drop_owed_at < MU_UNDO_WAIT_MS) return;
    InterlockedExchange(&g_drop_owed, 0);
    if (!mu_model_build(&m)) { mu_model_free(&m); return; }
    mu_map_build(&mp, &m, NULL);
    for (i = 0; i < mp.mod_count; i++) {
        const mu_mapmod *x = &mp.mods[i];
        if ((g_drop_hash[0] && mu_same_bundle(&m, x->hash, g_drop_hash)) || (g_drop_salt[0] && !_stricmp(x->id, g_drop_salt)))
            left += (int)x->uid_count + x->modules;
    }
    if (mp.entities < 0 || left)
        mu_log("map remove %s: %d of its objects are still in the map, so its row stays", g_drop_key ? g_drop_key : "", left);
    else mu_drop_rows(g_drop_key ? g_drop_key : "", g_drop_builds, g_drop_n, g_drop_tag);
    mu_map_free(&mp);
    mu_model_free(&m);
}

/* Remove the planned mod from the open map: its entities in one native Delete Selection (one undo step), then its
 * manifest row and, for the last mod, the search tag, at once or once the delete commits (mu_drop_tick). 1 done,
 * 0 refused (msg says why; nothing changed). */
static int mu_map_apply(const mu_model *m, mu_map *mp, const mu_plan *p, const char *key, char *msg, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    const mu_mapmod *x = &mp->mods[p->mod];
    char (*builds)[SH_MODMANIFEST_HASH_CAP];
    size_t i;
    int n = 0;
    if (p->refusal[0]) { _snprintf_s(msg, cap, _TRUNCATE, "%s", p->refusal); return 0; }
    /* Every row the remove drops, however many the map has. */
    builds = (char (*)[SH_MODMANIFEST_HASH_CAP])calloc(mp->rows.count ? mp->rows.count : 1, sizeof *builds);
    if (!builds) { strncpy_s(msg, cap, "Snapmap+ ran out of memory; nothing was changed. Restart DOOM and try again.", _TRUNCATE); return 0; }
    if (x->uid_count) {
        int st;
        if (!e->delete_entities) {
            strncpy_s(msg, cap, mu_delete_cause(SH_EDITOR_DELETE_UNAVAILABLE), _TRUNCATE);
            free(builds);
            return 0;
        }
        st = e->delete_entities(x->uids, (int)x->uid_count);
        if (st != SH_EDITOR_DELETE_DISPATCHED) {
            mu_log("map remove %s: the delete of %zu entit%s was refused (status %d)", key, x->uid_count,
                   x->uid_count == 1 ? "y" : "ies", st);
            strncpy_s(msg, cap, mu_delete_cause(st), _TRUNCATE);
            free(builds);
            return 0;
        }
    }
    /* Its row goes; with the last mod, so do the rows of mods only it needed. */
    for (i = 0; i < mp->rows.count; i++) {
        const sh_modmanifest_row *row = &mp->rows.rows[i];
        size_t k;
        int drop = 0;
        /* By salt: a row another build serves is the mod's row too. */
        for (k = 0; k < mp->mod_count && !drop; k++)
            if (mu_row_is_mod(m, row, &mp->mods[k]))
                drop = (int)k == p->mod || (p->vanilla_after && mu_mapmod_follows(&mp->mods[k]));
        if (drop) strcpy_s(builds[n++], SH_MODMANIFEST_HASH_CAP, row->build);
    }
    if (!x->uid_count) {
        mu_log("map remove %s: nothing of it is placed", key);
        mu_drop_rows(key, builds, n, p->tag_removed);
        free(builds);
    } else {
        mu_log("map remove %s: %zu entit%s deleted in one undo step; the row goes once the delete has committed", key,
               x->uid_count, x->uid_count == 1 ? "y" : "ies");
        free(g_drop_key);
        g_drop_key = _strdup(key);
        strncpy_s(g_drop_hash, sizeof g_drop_hash, x->hash, _TRUNCATE);
        strncpy_s(g_drop_salt, sizeof g_drop_salt, x->id, _TRUNCATE);
        free(g_drop_builds);
        g_drop_builds = builds;   /* owned until the next remove */
        g_drop_n = n;
        g_drop_tag = p->tag_removed;
        g_drop_owed_at = GetTickCount64();
        g_poll_next = 0;
        InterlockedExchange(&g_drop_owed, 1);
        InterlockedIncrement(&g_gen);
    }
    msg[0] = 0;
    return 1;
}

static void mu_put_entities_json(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    mu_put_entity_names(b, m, mp, x);
}

/* data.preview's map part. */
static void mu_put_plan(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_plan *p)
{
    const mu_mapmod *x = &mp->mods[p->mod];
    size_t i;
    mu_putn(b, "{", 1);
    mu_kstr(b, "name", mp->name ? mp->name : "", 1);
    mu_key(b, "entities", 0);
    mu_put_entities_json(b, m, mp, x);
    mu_knum(b, "entityCount", (unsigned long long)x->uid_count, 0);
    mu_puts(b, ",\"references\":[");
    for (i = 0; i < p->ref_count; i++) {
        char *name = mu_ref_name(m, mp, &p->refs[i]);
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "kind", p->refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", 1);
        mu_kstr(b, "name", name ? name : "", 0);
        mu_putn(b, "}", 1);
        free(name);
    }
    mu_putn(b, "]", 1);
    mu_kbool(b, "manifestRow", p->manifest_row, 0);
    mu_kbool(b, "tagRemoved", p->tag_removed, 0);
    mu_kbool(b, "vanillaAfter", p->vanilla_after, 0);
    mu_kbool(b, "published", p->published, 0);
    if (p->attached) mu_kbool(b, "attached", 1, 0);
    mu_putn(b, "}", 1);
}

/* The dry run's lines for the console. */
static void mu_say_plan(const mu_out *o, const char *verb, const mu_model *m, const mu_map *mp, const mu_plan *p)
{
    const mu_mapmod *x = &mp->mods[p->mod];
    size_t i;
    const char *where = mp->name && mp->name[0] ? mp->name : "the open map";
    if (x->uid_count) mu_line(o, "%s: deletes %zu placed entit%s from %s (one undo step)", verb, x->uid_count,
                              x->uid_count == 1 ? "y" : "ies", where);
    for (i = 0; i < x->uid_count; i++) {
        char *name = mu_entity_label(m, sh_mm_find(&mp->mm, x->uids[i]), x->uids[i]);
        mu_line(o, "%s:   %s", verb, name ? name : "");
        free(name);
    }
    if (p->ref_count) mu_line(o, "%s: severs %zu wire or timeline reference(s) to those entities", verb, p->ref_count);
    for (i = 0; i < p->ref_count; i++) {
        char *name = mu_ref_name(m, mp, &p->refs[i]);
        mu_line(o, "%s:   %s: %s", verb, p->refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", name ? name : "");
        free(name);
    }
    if (p->manifest_row) mu_line(o, "%s: removes the mod's row from the map's list of mods%s", verb,
                                 p->attached ? " (it is attached to the whole map)" : "");
    if (p->tag_removed) mu_line(o, "%s: removes the search tag mods (the last mod the map uses)", verb);
    if (p->vanilla_after) mu_line(o, "%s: the map then needs no mods and plays without Snapmap+", verb);
    if (p->published) mu_line(o, "%s: the published map is unaffected until you publish the map again", verb);
}

/* A mod's name where a sentence names it: its build's name, else (an unnamed
 * mod) its folder's own name, never its salt. */
static const char *mu_entry_title(const sh_ms_entry *s)
{
    return s->loaded && s->bundle.name && s->bundle.name[0] ? s->bundle.name : mu_leaf(s->name);
}

/* The one folder an uninstall of `key` removes, in the tab and the console alike: the folder of that name under
 * mods\, else (the console, by_key) the first folder sh_ms_entry_matches finds for it (a nested folder's own name,
 * or a salt). Another folder of the same mod stays, and the dry run says so. Returns the entry, or -1. */
static int mu_uninstall_target(const mu_model *m, const char *key, int by_key)
{
    size_t i;
    for (i = 0; i < m->list.count; i++) if (!strcmp(m->list.items[i].name, key)) return (int)i;
    for (i = 0; by_key && i < m->list.count; i++) if (sh_ms_entry_matches(&m->list.items[i], key)) return (int)i;
    return -1;
}

/* Whether the entry at `at` adds a Toybox row of that display name. */
static int mu_entry_has_row(const mu_model *m, size_t at, const char *name)
{
    int k;
    for (k = 0; name && k < m->entries[at].row_count; k++)
        if (m->entries[at].row_names[k] && !strcmp(m->entries[at].row_names[k], name)) return 1;
    return 0;
}

/* mapRemoveModPreview, mapRemoveMod, uninstallPreview and uninstall, for the tab and the console (o non-NULL prints
 * the same consequences; by_key also finds a mod by a nested folder's own name or its salt). */
static int mu_map_action(int action, const char *key, const mu_out *o, int by_key)
{
    int uninstall = action == SH_MODS_ACT_UNINSTALL || action == SH_MODS_ACT_UNINSTALL_PREVIEW;
    int apply = action == SH_MODS_ACT_UNINSTALL || action == SH_MODS_ACT_MAP_REMOVE;
    const char *verb = uninstall ? "uninstall" : "map remove";
    const sh_modui_engine *e = mu_engine();
    mu_model m;
    mu_map mp;
    mu_record rec;
    mu_plan p;
    mu_buf b;
    char *msg = NULL, cause[300];
    const char *title, *mapname;
    int entry = -1, ok = 1;
    unsigned char *targets = NULL;
    size_t i, t;
    memset(&b, 0, sizeof b);
    if (!key || !key[0]) {
        mu_line(o, "%s: no mod named", verb);
        return mu_simple_result(action, 0, "Select a mod first.");
    }
    if (mu_busy(cause, sizeof cause)) {
        int r;
        mu_msg(&msg, "Not now: %s.", cause);
        mu_line(o, "%s %s: refused: %s", verb, key, cause);
        r = mu_simple_result(action, 0, msg ? msg : "Not now.");
        free(msg);
        return r;
    }
    sh_mb_record_why_clear();   /* a record read or write this action cannot make is named in its message */
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    targets = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
    if (!targets) { mu_map_free(&mp); mu_record_free(&rec); mu_model_free(&m); return -1; }
    if (uninstall) {
        entry = mu_uninstall_target(&m, key, by_key);
        if (entry >= 0) targets[entry] = 1;
    } else {
        for (i = 0; i < m.list.count && entry < 0; i++) if (!strcmp(m.list.items[i].name, key)) entry = (int)i;
    }
    /* The open map's part goes by the first build's folder (its rows match by key). */
    mu_plan_build(&m, &mp, uninstall && entry >= 0 ? m.list.items[entry].name : key, &p);
    if (entry >= 0) title = mu_entry_title(&m.list.items[entry]);
    else {
        /* A row whose mod is not installed: the name the map saved for it. */
        const char *row_name = "";
        for (i = 0; p.mod >= 0 && i < mp.rows.count && !row_name[0]; i++)
            if (!_stricmp(mp.rows.rows[i].salt, mp.mods[p.mod].id)) row_name = sh_modmanifest_row_name(&mp.rows.rows[i]);
        title = row_name[0] ? row_name : p.mod >= 0 ? "this mod" : key;
    }
    mapname = mp.name && mp.name[0] ? mp.name : "the open map";
    if (uninstall && entry < 0) {
        ok = 0;
        mu_msg(&msg, "%s is no longer installed; press Refresh.", mu_leaf(key));
    } else if (!uninstall && p.mod < 0) {
        ok = 0;
        mu_msg(&msg, mp.entities < 0 ? "No map is open in the editor." : "The open map does not use %s.", title);
    } else if (p.mod >= 0 && p.refusal[0]) {
        ok = 0;
        mu_msg(&msg, "%s cannot be removed from %s: %s.", title, mapname,
                    p.refusal);
    } else if (uninstall && mp.entities >= 0 && (!e->clear_undo || e->clear_undo(1) < 0)) {
        /* Uninstall is permanent and clears the open map's undo history; a
         * build that cannot clear it does not uninstall while a map is open. */
        ok = 0;
        mu_msg(&msg, "%s was not uninstalled: the editor's undo history cannot be cleared on this "
                    "build. Close the map, then uninstall it.", title);
    }
    if (!ok) {
        mu_line(o, "%s %s: %s", verb, key, msg ? msg : "");
        mu_result_head(&b, action, 0, msg ? msg : "");
    } else if (!apply) {
        /* The dry run: the consequence list the confirmation shows, computed per mod. When another folder of the mod
         * stays installed (the copy a map brought, or the player's own), the maps that use it open with that one,
         * and only the Toybox rows no staying folder adds leave the editor. */
        const sh_ms_entry *s = entry >= 0 ? &m.list.items[entry] : NULL;
        const sh_ms_entry *stay = NULL;
        size_t stay_at = 0;
        int saved;
        for (t = 0; uninstall && s && s->loaded && t < m.list.count && !stay; t++)
            if (!targets[t] && m.list.items[t].loaded && !_stricmp(m.list.items[t].salt, s->salt)) {
                stay = &m.list.items[t];
                stay_at = t;
            }
        saved = uninstall && s && s->loaded && !stay ? mu_saved_maps_using(s->salt) : stay ? 0 : -1;
        mu_result_head(&b, action, 1, "");
        mu_puts(&b, ",\"preview\":{");
        mu_kstr(&b, "action", uninstall ? "uninstall" : "mapRemoveMod", 1);
        mu_kstr(&b, "key", key, 0);
        mu_kstr(&b, "name", title, 0);
        mu_key(&b, "map", 0);
        if (p.mod >= 0) mu_put_plan(&b, &m, &mp, &p); else mu_puts(&b, "null");
        mu_key(&b, "install", 0);
        if (!uninstall) mu_puts(&b, "null");
        else {
            /* The folder goes to the Recycle Bin, and the open map's undo
             * history is cleared. */
            mu_putn(&b, "{", 1);
            mu_kstr(&b, "folder", s->name, 1);
            mu_kstr(&b, "disposition", "recycle", 0);
            mu_kint(&b, "savedMaps", saved, 0);
            mu_kbool(&b, "undoCleared", mp.entities >= 0, 0);
            if (stay) mu_kstr(&b, "keptBuild", stay->name, 0);
            mu_putn(&b, "}", 1);
            mu_puts(&b, ",\"editorRows\":[");
            {
                int k = 0;
                for (t = 0; t < m.list.count; t++) {
                    if (!targets[t]) continue;
                    for (i = 0; i < (size_t)m.entries[t].row_count; i++) {
                        if (stay && mu_entry_has_row(&m, stay_at, m.entries[t].row_names[i])) continue;
                        if (k++) mu_putn(&b, ",", 1);
                        mu_str(&b, m.entries[t].row_names[i]);
                    }
                }
            }
            mu_putn(&b, "]", 1);
        }
        mu_putn(&b, "}", 1);
        if (o) {
            if (p.mod >= 0) mu_say_plan(o, verb, &m, &mp, &p);
            else if (uninstall) mu_line(o, "%s: the open map does not use it", verb);
            if (uninstall) {
                int rows = 0;
                for (t = 0; t < m.list.count; t++)
                    for (i = 0; targets[t] && i < (size_t)m.entries[t].row_count; i++)
                        if (!stay || !mu_entry_has_row(&m, stay_at, m.entries[t].row_names[i])) rows++;
                if (rows) mu_line(o, "%s: removes %d Toybox row(s) from the editor", verb, rows);
                for (t = 0; t < m.list.count; t++)
                    for (i = 0; targets[t] && i < (size_t)m.entries[t].row_count; i++)
                        if (!stay || !mu_entry_has_row(&m, stay_at, m.entries[t].row_names[i]))
                            mu_line(o, "%s:   %s", verb, m.entries[t].row_names[i]);
                mu_line(o, "%s: moves its folder, %s, to the Windows Recycle Bin; restore it from there by hand", verb,
                        s->name);
                if (stay)
                    mu_line(o, "%s: another folder of %s, %s, stays installed; maps that use it open with it", verb,
                            title, stay->name);
                else if (saved > 0)
                    mu_line(o, saved == 1 ? "%s: %d of your saved maps uses it; it opens with stand-ins for its objects "
                                            "until you install the mod again"
                                          : "%s: %d of your saved maps use it; they open with stand-ins for its objects "
                                            "until you install the mod again", verb, saved);
                else if (saved == 0) mu_line(o, "%s: none of your saved maps use it", verb);
                if (mp.entities >= 0) mu_line(o, "%s: Undo history is cleared.", verb);
            }
            mu_line(o, "%s %s: dry run; add --yes to do it", verb, key);
        }
    } else {
        /* In order: the open map, then the folder, then the rescan (serving
         * index, palette rows, strings). */
        char mapmsg[400] = "";
        int removed_from_map = 0;
        if (p.mod >= 0) {
            ok = mu_map_apply(&m, &mp, &p, key, mapmsg, sizeof mapmsg);
            removed_from_map = ok;
        }
        if (!ok) {
            mu_msg(&msg, "%s was not %s: %s.", title, uninstall ? "uninstalled" : "removed from the map",
                        mapmsg);
        } else if (!uninstall) {
            const mu_mapmod *x = &mp.mods[p.mod];
            /* A delete of placed entities commits its own undo step; a row
             * dropped alone changes nothing the editor sees. */
            if (!x->uid_count) mu_mark_modified("Remove Mod From Map");
            if (!x->uid_count)
                mu_msg(&msg, "Removed %s from %s; nothing placed used it%s.",
                            title, mapname, p.vanilla_after ? "; the map now plays without Snapmap+" : "");
            else
                mu_msg(&msg, "Removed %s from %s: %zu object%s deleted%s. Ctrl+Z in the editor brings %s back.",
                            title, mapname, x->uid_count, x->uid_count == 1 ? "" : "s",
                            p.vanilla_after ? "; the map now plays without Snapmap+" : "", x->uid_count == 1 ? "it" : "them");
        } else {
            unsigned char *removing = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
            mu_removal rm;
            int deleted_entities = removed_from_map && mp.mods[p.mod].uid_count > 0, undo = 0;
            memset(&rm, 0, sizeof rm);
            /* The undo history: no snapshot older than the delete may stay. After an entity delete the native commit
             * lands on a later editor frame and the tick clears once it has; otherwise the clear is immediate. */
            if (mp.entities >= 0) {
                if (deleted_entities) { mu_undo_owe(key); undo = 1; }
                else undo = mu_undo_clear_now(key);
            }
            if (!removing) ok = 0;
            else {
                for (t = 0; t < m.list.count; t++) removing[t] = targets[t];
                mu_remove_marked(&m, removing, &rm);
                ok = rm.removed > 0 && rm.failed == 0;
            }
            mu_log("uninstall %s: %s%s", key, !ok ? "the folder could not be removed" : rm.outright ? "the folder was deleted outright"
                   : "the folder was moved to the Recycle Bin", removed_from_map ? " after the open map was cleaned of it" : "");
            if (ok && rm.outright)
                mu_msg(&msg, "Uninstalled %s%s. %s deleted because the Recycle Bin could not "
                            "take it (%s)%s", title, removed_from_map ? " and removed it from the open map" : "",
                            rm.removed > 1 ? "Their folders were" : "Its folder was", rm.outright_why ? rm.outright_why : "",
                            mp.entities < 0 ? "." : undo > 0 ? ", and the undo history was cleared."
                                                             : "; the undo history could not be cleared.");
            else if (ok)
                mu_msg(&msg, "Uninstalled %s%s. %s moved to the Recycle Bin%s", title,
                            removed_from_map ? " and removed it from the open map" : "",
                            rm.removed > 1 ? "Their folders were" : "Its folder was",
                            mp.entities < 0 ? "." : undo > 0 ? " and the undo history was cleared."
                                                             : "; the undo history could not be cleared.");
            else
                mu_msg(&msg, "%s could not be uninstalled: %s. Close any program using its folder and "
                            "try again.%s", title, rm.first_fail && rm.first_fail[0] ? rm.first_fail : "nothing was deleted",
                            removed_from_map ? " It was removed from the open map." : "");
            free(removing);
            mu_removal_free(&rm);
        }
        msg = mu_record_note(msg);
        mu_line(o, "%s %s: %s", verb, key, msg ? msg : "");
        mu_result_head(&b, action, ok, msg ? msg : "");
    }
    free(targets);
    free(msg);
    mu_plan_free(&p);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
    return mu_result_tail(&b);
}

/* The map's mod rows and choices live outside the edit map, so a change to them alone would leave it unmodified;
 * each change commits one undo step, as a placed entity does, so the editor asks to save. */
static void mu_mark_modified(const char *label)
{
    const sh_modui_engine *e = mu_engine();
    int r = e->mark_modified ? e->mark_modified(label) : -1;
    if (r < 0) mu_log("%s: the open map could not be marked modified; save it from the editor's menu to keep the change", label);
}

/* ---- map add: a map-level row */

/* Attach `key` (a folder as sh_ms_entry_matches reads it, or a salt) to the open map: the current manifest gets a
 * map-level row naming the build of the mod's root folder, applied at once and written by the next save. A folder a
 * downloaded map or prefab brought attaches the root folder of its salt, else the player is sent to Add to my mods. */
static int mu_map_add(const char *key, const mu_out *o)
{
    const int action = SH_MODS_ACT_MAP_ADD;
    mu_model m;
    mu_map mp;
    mu_record rec;
    mu_buf b;
    char *msg = NULL, cause[300], *why = NULL;
    const char *where, *title;
    int entry = -1, ok = 0, r, st;
    size_t i;
    memset(&b, 0, sizeof b);
    if (!key || !key[0]) {
        mu_line(o, "map add: no mod named");
        return mu_simple_result(action, 0, "Select a mod first.");
    }
    if (mu_busy(cause, sizeof cause)) {
        mu_msg(&msg, "Not now: %s.", cause);
        mu_line(o, "map add %s: refused: %s", key, cause);
        r = mu_simple_result(action, 0, msg ? msg : "Not now.");
        free(msg);
        return r;
    }
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    for (i = 0; i < m.list.count && entry < 0; i++)
        if (!strcmp(m.list.items[i].name, key) && m.list.items[i].loaded) entry = (int)i;
    for (i = 0; i < m.list.count && entry < 0; i++)
        if (sh_ms_entry_matches(&m.list.items[i], key) && m.list.items[i].loaded) entry = (int)i;
    title = entry >= 0 ? mu_entry_title(&m.list.items[entry]) : key;
    where = mp.name && mp.name[0] ? mp.name : "the open map";
    st = entry >= 0 ? sh_modserve_bundle_state(m.list.items[entry].bundle.hash, &why) : 0;
    free(why);
    if (mp.entities < 0)
        mu_msg(&msg, mp.entities == -1 ? "No map is open in the editor."
                                                                   : "Snapmap+ cannot read this map in this game version; use Send feedback.");
    else if (entry < 0) mu_msg(&msg, "%s is no longer installed; press Refresh.", mu_leaf(key));
    else if (sh_mb_editor_only(&m.list.items[entry].bundle))
        /* Editor tweaks are this computer's: every map opened in
         * this editor already has them, a map never records them. */
        mu_msg(&msg, "%s changes only this computer's editor (%u editor tweak%s); every "
                    "map you open here already has them, and a map never lists them.", title,
                    m.list.items[entry].bundle.classes.editor, m.list.items[entry].bundle.classes.editor == 1 ? "" : "s");
    else if (st <= 0 || m.entries[entry].cause)
        mu_msg(&msg, "%s cannot be used yet. %s", title,
                    m.entries[entry].cause ? m.entries[entry].cause : "Snapmap+ has not read it yet; press Refresh.");
    else {
        const sh_ms_entry *e = &m.list.items[entry];
        sh_modmanifest_row row;
        const mu_mapmod *x = NULL;
        int found = 0;
        memset(&row, 0, sizeof row);
        /* A row the map already has for this mod (another build of it) is the one marked. */
        for (i = 0; i < mp.rows.count && !found; i++)
            if (mu_same_bundle(&m, mp.rows.rows[i].build, e->bundle.hash) || !_stricmp(mp.rows.rows[i].salt, e->salt)) {
                row = mp.rows.rows[i];
                found = 1;
            }
        if (!found) {
            strncpy_s(row.salt, sizeof row.salt, e->salt, _TRUNCATE);
            strncpy_s(row.build, sizeof row.build, e->bundle.hash, _TRUNCATE);
            row.name = e->bundle.name;   /* borrowed: the attach copies it */
            row.size = e->bundle.total_size;
        }
        row.names = NULL;
        row.name_count = 0;
        row.choices = NULL;
        for (i = 0; i < mp.mod_count && !x; i++) if (mu_same_bundle(&m, mp.mods[i].hash, e->bundle.hash)) x = &mp.mods[i];
        r = sh_modmanifest_current_attach(&row);
        if (r < 0) mu_msg(&msg, "%s could not be added to %s; press Refresh and try again.", title, where);
        else if (r == 0) {
            ok = 1;
            mu_msg(&msg, "%s is already used by the whole of %s.", title, where);
        } else {
            ok = 1;
            (void)mu_scope_now_declared("map add");
            InterlockedIncrement(&g_gen);
            mu_mark_modified("Add Mod To Map");
            mu_log("map add %s: %s (%.16s) attached to %s as a map-level row%s", key, row.salt, row.build,
                   where, x && x->uid_count ? " (it also has placed entities)" : "");
            mu_msg(&msg, "%s is now used by the whole of %s; save the map to keep it.", title, where);
        }
    }
    mu_line(o, "map add %s: %s", key, msg ? msg : "");
    mu_result_head(&b, action, ok, msg ? msg : "");
    free(msg);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
    return mu_result_tail(&b);
}

/* ---- update: the version rule by hand */

/* The root folder's loaded build of `salt` (the installed version), or -1. */
static int mu_root_entry(const mu_model *m, const char *salt)
{
    size_t i;
    for (i = 0; salt && i < m->list.count; i++)
        if (m->list.items[i].loaded && !_stricmp(m->list.items[i].salt, salt)) return (int)i;
    return -1;
}

/* The name of a pinned map row of `salt` whose installed version would change what the map serves (an object's unit,
 * or the whole-mod unit of a map-level row, differs from the pinned one: sh_modpin_row_differs), or NULL. An author's
 * own unchanged published map is never told it uses another version. */
static const char *mu_update_name(const mu_model *m, const mu_map *mp, const char *salt)
{
    size_t k;
    for (k = 0; k < mp->rows.count; k++)
        if (mp->rows.rows[k].pinned && !_stricmp(mp->rows.rows[k].salt, salt) && mu_root_entry(m, salt) >= 0 &&
            sh_modpin_row_differs(&mp->rows.rows[k]) == 1)
            return sh_modmanifest_row_name(&mp->rows.rows[k]);
    return NULL;
}

/* Update: every placed object of the mod `salt` moves to the installed version's files, unless that version lacks
 * one of them: then nothing moves and the sentence says which. Nothing is saved until the author saves. */
static int mu_map_update(const char *salt, const mu_out *o)
{
    const int action = SH_MODS_ACT_MAP_UPDATE;
    const sh_modui_engine *e = mu_engine();
    mu_model m;
    mu_map mp;
    mu_record rec;
    mu_names placed;
    char *msg = NULL, cause[300], prefix[SH_MODMANIFEST_SALT_CAP + 8];
    const char *title, *gone = NULL;
    const char **mine = NULL;
    size_t i, n = 0;
    int entry, ok = 0, r;
    if (!salt || !salt[0]) return mu_simple_result(action, 0, "Select a mod first.");
    if (mu_busy(cause, sizeof cause)) {
        mu_msg(&msg, "Not now: %s.", cause);
        r = mu_simple_result(action, 0, msg ? msg : "Not now.");
        free(msg);
        return r;
    }
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    memset(&placed, 0, sizeof placed);
    entry = mu_root_entry(&m, salt);
    title = mu_update_name(&m, &mp, salt);
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "mods/%s/", salt);
    if (mp.entities < 0) mu_msg(&msg, "No map is open in the editor.");
    else if (entry < 0 || !title) mu_msg(&msg, "This map has no other version of that mod to update.");
    else if (!e->placed_names || e->placed_names(mu_names_add, &placed) < 0 || placed.oom)
        mu_msg(&msg, "Snapmap+ could not read the open map's objects; try again.");
    else if (!(mine = (const char **)calloc(placed.count + 1, sizeof *mine)))
        mu_msg(&msg, "Snapmap+ ran out of memory; try again.");
    else {
        const char *mod = title[0] ? title : mu_entry_title(&m.list.items[entry]);
        for (i = 0; i < placed.count; i++)
            if (!_strnicmp(placed.items[i], prefix, strlen(prefix))) {
                mine[n++] = placed.items[i];
                if (!gone && sh_modserve_root_carries(salt, placed.items[i]) != 1) gone = placed.items[i];
            }
        if (gone) {
            const char *leaf = strrchr(gone, '/');
            mu_msg(&msg, "%s was not updated: %s is not in the version you have installed.", mod, leaf ? leaf + 1 : gone);
        } else if ((r = sh_modmanifest_current_make_local(salt, m.list.items[entry].bundle.hash, mine, n)) <= 0)
            mu_msg(&msg, r < 0 ? "Snapmap+ ran out of memory; try again." : "This map has no other version of that mod to update.");
        else {
            ok = 1;
            (void)mu_scope_now_declared("map update");
            InterlockedIncrement(&g_gen);
            mu_mark_modified("Update Mod In Map");
            if (!n) mu_msg(&msg, "This map now uses the version of %s you have installed.", mod);
            else if (n == 1) mu_msg(&msg, "This map now uses the version of %s you have installed for its object.", mod);
            else mu_msg(&msg, "This map now uses the version of %s you have installed for all %zu of its objects.", mod, n);
            mu_log("map update %s: the row moved to the installed build %.16s (%zu placed object(s))", salt,
                   m.list.items[entry].bundle.hash, n);
        }
    }
    mu_line(o, "map update %s: %s", salt, msg ? msg : "");
    r = mu_simple_result(action, ok, msg ? msg : "");
    free(msg);
    free((void *)mine);
    mu_names_free(&placed);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
    return r;
}

/* ---- registry choice */

/* A slot ("<game file>/<path>", modregistry.h) as the page shows it: "<path>
 * in <game file>", the path starting at the first top-level decl member. */
/* malloc'd. */
static char *mu_slot_text(const char *slot)
{
    static const char *const top[] = { "edit", "editorVars", "inherit", "events", "aliases", "init", "class", NULL };
    const char *p = slot ? slot : "";
    int seg = 0, k;
    for (; *p; p++) {
        size_t n;
        if (p != slot && p[-1] != '/') continue;
        if (seg++ < 2) continue;
        n = strcspn(p, "/");
        for (k = 0; top[k]; k++)
            if (strlen(top[k]) == n && !strncmp(p, top[k], n))
                return sh_textf("%s in %.*s", p, (int)(p - slot - 1), slot);
    }
    return sh_text(slot ? slot : "");
}

/* The open map's registry choice for a slot several declared mods set
 * (`sh_mods choose`, the tab's clash select): applied at once, written into
 * the row of its key at the map's next save (modmanifest.c mm_choices_save). */
static int mu_choose(const char *slot, const char *key, const mu_out *o)
{
    const int action = SH_MODS_ACT_CHOOSE;
    char *line = NULL, cause[300], *msg = NULL, *name = NULL, *where;
    const char *said;
    int r, rc;
    if (mu_busy(cause, sizeof cause)) {
        mu_msg(&msg, "Not now: %s.", cause);
        mu_line(o, "choose %s: refused: %s", slot ? slot : "", cause);
        rc = mu_simple_result(action, 0, msg ? msg : "Not now.");
        free(msg);
        return rc;
    }
    r = sh_modregistry_choose_command(slot, key, &line);
    said = line ? line : "MODS: choose: out of memory";
    if (r > 0) (void)mu_scope_apply("choose");
    if (r >= 0) InterlockedIncrement(&g_gen);
    if (r >= 0) mu_mark_modified("Choose Mod");
    if (o && o->fn) o->fn(o->context, said);
    else mu_log("%s", said + (strncmp(said, "MODS: ", 6) ? 0 : 6));
    {
        /* The page names the chosen mod; the line's salt is for the log. */
        sh_ms_list l;
        if (key && key[0] && sh_ms_enumerate(&l)) {
            const sh_ms_entry *e = sh_ms_find_salt(&l, key, NULL);
            if (e) name = _strdup(mu_entry_title(e));
            sh_ms_list_free(&l);
        }
        where = mu_slot_text(slot);
        if (r < 0)
            msg = mu_sentence_end(sh_textf("That choice was refused: %s", said + (strncmp(said, "MODS: choose: ", 14) ? 0 : 14)));
        else if (key && key[0])
            msg = sh_textf("This map now uses %s's value for %s; save the map to keep it.",
                           name && name[0] ? name : "the chosen mod", where ? where : "");
        else msg = sh_textf("The choice for %s is cleared; the first mod the map lists decides it.", where ? where : "");
        free(where);
    }
    rc = mu_simple_result(action, r >= 0, msg ? msg : "");
    free(msg);
    free(name);
    free(line);
    return rc;
}

/* ---- the Files view (read-only) and New mod */


/* The kinds the view shows as text; every other file is binary. */
static int mu_text_kind(const char *path)
{
    static const char *const ext[] = { ".decl", ".json", ".txt", ".md", ".cfg", ".entities", ".lua", NULL };
    int i;
    for (i = 0; ext[i]; i++) if (mu_suffix(path, ext[i])) return 1;
    return 0;
}

typedef struct mu_disk_file { char *path; unsigned long long size; } mu_disk_file;
typedef struct mu_disk { mu_disk_file *v; size_t n, cap; } mu_disk;

/* Every file under `abs` (UTF-8), with its path below the mod folder ('/'),
 * at any depth (a heap list of folders still to read, no recursion). Dot
 * folders and files (.builds, .git) and reparse points are skipped. */
typedef struct mu_walk_dir { char *abs, *rel; } mu_walk_dir;

static void mu_walk(const char *abs, const char *rel, mu_disk *d)
{
    mu_walk_dir *todo = NULL;
    size_t n = 0, cap = 0;
    if (!(todo = (mu_walk_dir *)malloc(sizeof *todo))) return;
    cap = 1;
    todo[0].abs = _strdup(abs);
    todo[0].rel = _strdup(rel);
    if (!todo[0].abs || !todo[0].rel) { free(todo[0].abs); free(todo[0].rel); free(todo); return; }
    n = 1;
    while (n) {
        mu_walk_dir dir = todo[--n];
        char *pattern = sh_textf("%s\\*", dir.abs);
        WIN32_FIND_DATAW fd;
        HANDLE h = pattern ? mu_find_first(pattern, &fd) : INVALID_HANDLE_VALUE;
        free(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char *name, *crel, *cabs;
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                if (!(name = sh_mb_utf8(fd.cFileName))) continue;
                crel = sh_textf("%s%s%s", dir.rel, dir.rel[0] ? "/" : "", name);
                cabs = sh_textf("%s\\%s", dir.abs, name);
                free(name);
                if (!crel || !cabs) { free(crel); free(cabs); continue; }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (n == cap) {
                        size_t nc = cap * 2;
                        mu_walk_dir *g = (mu_walk_dir *)realloc(todo, nc * sizeof *g);
                        if (g) { todo = g; cap = nc; }
                    }
                    if (n < cap) { todo[n].abs = cabs; todo[n].rel = crel; n++; continue; }
                    free(crel);
                    free(cabs);
                    continue;
                }
                free(cabs);
                if (d->n == d->cap) {
                    size_t nc = d->cap ? d->cap * 2 : 64;
                    mu_disk_file *nv = (mu_disk_file *)realloc(d->v, nc * sizeof *nv);
                    if (nv) { d->v = nv; d->cap = nc; }
                }
                if (d->n < d->cap) {
                    d->v[d->n].path = crel;
                    d->v[d->n].size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    d->n++;
                } else free(crel);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(dir.abs);
        free(dir.rel);
    }
    free(todo);
}

static void mu_disk_free(mu_disk *d)
{
    size_t i;
    for (i = 0; i < d->n; i++) free(d->v[i].path);
    free(d->v);
    memset(d, 0, sizeof *d);
}

/* Whether `cause` names `path` as a whole path (case-insensitive): the
 * builder's causes start with the file's assets/ path, or name its request. */
static int mu_cause_names(const char *cause, const char *path)
{
    size_t n = path ? strlen(path) : 0;
    const char *p;
    if (!cause || !n) return 0;
    for (p = cause; *p; p++) {
        char before = p > cause ? p[-1] : ' ', after;
        if (_strnicmp(p, path, n)) continue;
        after = p[n];
        if (isalnum((unsigned char)before) || strchr("_./-\\", before)) continue;
        /* the path ends there: the end, punctuation, or a full stop ending the sentence */
        if (!after || strchr(" \t:,;)'\"", after) || (after == '.' && !isalnum((unsigned char)p[n + 1]))) return 1;
    }
    return 0;
}

/* The cause that belongs to one file of the folder, or "". */
static const char *mu_file_cause(const char *cause, const char *path)
{
    char *back, *c;
    int named;
    if (mu_cause_names(cause, path)) return cause;
    /* A script's messages name it as Windows spells it: scripts\<name>.lua. */
    if (!(back = _strdup(path))) return "";
    for (c = back; *c; c++) if (*c == '/') *c = '\\';
    named = mu_cause_names(cause, back);
    free(back);
    return named ? cause : "";
}

/* The mod's standing cause: why the folder serves nothing (a failed build
 * included: there is no last good build), or "". Raw: the Files view matches
 * it against file paths before saying it plainly. */
static const char *mu_entry_cause(const sh_ms_entry *e)
{
    if (!sh_ms_entry_usable(e)) return e->cause;
    return "";
}

static const char *mu_class_name(const sh_mb_file *f)
{
    switch (f->cls) {
    case SH_MB_CLASS_ADD:     return "add";
    case SH_MB_CLASS_BORROW:  return "borrow";
    case SH_MB_CLASS_REPLACE: return f->edited || f->edit ? "edit" : "replace";
    case SH_MB_CLASS_EDITOR:  return "editor";
    case SH_MB_CLASS_SURFACE: return "surface";
    default:                  return "";
    }
}

static const sh_ms_entry *mu_entry_named(const sh_ms_list *l, const char *key)
{
    size_t i;
    for (i = 0; key && key[0] && i < l->count; i++) if (!_stricmp(l->items[i].name, key)) return &l->items[i];
    return NULL;
}

static void mu_put_file(mu_buf *b, int first, const char *path, unsigned long long size, int in_folder,
                        const sh_mb_file *f, const char *cause)
{
    char where[32] = "";
    int descriptor = !_stricmp(path, "mod.json") || !_stricmp(path, "package.json");
    /* The archive's name: one of the game's own few (modstock.h). */
    if (f && f->stock && f->stock_source) sh_mb_stock_container(f->stock_source, f->blob, where, sizeof where);
    if (!first) mu_putn(b, ",", 1);
    mu_putn(b, "{", 1);
    mu_kstr(b, "path", path, 1);
    mu_knum(b, "size", size, 0);
    mu_kstr(b, "kind", mu_text_kind(path) ? "text" : "binary", 0);
    mu_kstr(b, "class", descriptor ? "descriptor" : f ? mu_class_name(f) : "", 0);
    mu_kbool(b, "game", f && f->stock, 0);
    mu_kstr(b, "container", where, 0);
    mu_kbool(b, "inFolder", in_folder, 0);
    mu_kstr(b, "cause", cause, 0);
    mu_putn(b, "}", 1);
}

/* modFiles: every file of the folder and every game file its build reads from the install, each with its class,
 * whether it is the game's own bytes, which archive holds them, and the plain cause when the folder's cause names it. */
static int mu_files(const char *key)
{
    const int action = SH_MODS_ACT_FILES;
    sh_ms_list l;
    const sh_ms_entry *e;
    mu_disk d;
    mu_buf b;
    size_t i, k;
    int first = 1;
    unsigned char *seen;
    const char *cause;
    char *plain;
    if (!key || !key[0]) return mu_simple_result(action, 0, "Select a mod to see its files.");
    if (!sh_ms_enumerate(&l)) return mu_simple_result(action, 0, "Snapmap+ cannot read your mods folder. Check that it exists and is not locked, then press Refresh.");
    e = mu_entry_named(&l, key);
    if (!e || !e->folder) { sh_ms_list_free(&l); return mu_simple_result(action, 0, "That mod is no longer installed."); }
    memset(&d, 0, sizeof d);
    mu_walk(e->folder, "", &d);
    seen = (unsigned char *)calloc(e->loaded && e->bundle.file_count ? e->bundle.file_count : 1, 1);
    cause = mu_entry_cause(e);
    plain = mu_plain_cause(e, cause);
    if (!plain) plain = _strdup("");
    memset(&b, 0, sizeof b);
    mu_result_head(&b, action, 1, "");
    mu_kstr(&b, "key", e->name, 0);
    {
        char *name = mu_folder_name(e);
        mu_kstr(&b, "name", name ? name : "", 0);
        free(name);
    }
    mu_kstr(&b, "cause", plain ? plain : "", 0);
    mu_key(&b, "files", 0);
    mu_putn(&b, "[", 1);
    for (i = 0; i < d.n; i++) {
        const sh_mb_file *f = NULL;
        for (k = 0; e->loaded && k < e->bundle.file_count && !f; k++)
            if (!_stricmp(e->bundle.files[k].path, d.v[i].path)) { f = &e->bundle.files[k]; if (seen) seen[k] = 1; }
        mu_put_file(&b, first, d.v[i].path, d.v[i].size, 1, f, mu_file_cause(cause, d.v[i].path)[0] && plain ? plain : "");
        first = 0;
    }
    /* Game files the folder does not hold ("inFolder": false): read from the install. */
    for (k = 0; e->loaded && k < e->bundle.file_count; k++) {
        const sh_mb_file *f = &e->bundle.files[k];
        if (seen && seen[k]) continue;
        mu_put_file(&b, first, f->path, f->size, 0, f, mu_file_cause(cause, f->path)[0] && plain ? plain : "");
        first = 0;
    }
    mu_putn(&b, "]", 1);
    free(seen);
    free(plain);
    mu_disk_free(&d);
    sh_ms_list_free(&l);
    return mu_result_tail(&b);
}

/* A path below a mod folder: relative, no drive, no empty, "." or ".." part. */
static int mu_rel_ok(const char *rel)
{
    const char *s = rel;
    if (!rel || !rel[0] || strchr(rel, ':') || rel[0] == '/' || rel[0] == '\\') return 0;
    while (*s) {
        size_t n = strcspn(s, "/\\");
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n;
        if (*s) s++;
        if (!*s && (s[-1] == '/' || s[-1] == '\\')) return 0;
    }
    return 1;
}

/* Well-formed UTF-8 without NUL bytes: what the page can show as text. */
static int mu_utf8_text(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t k, len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!c || !len || (len == 2 && c < 0xC2) || i + len > n) return 0;
        for (k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return 0;
        i += len;
    }
    return 1;
}

/* modFileText ("<key>\t<path>"): one text file of the folder, at most 1 MiB.
 * mod.json comes without the product block, which Snapmap+ writes. */
static int mu_file_text(const char *arg)
{
    const int action = SH_MODS_ACT_FILE_TEXT;
    char *key, *rel, *full, *msg, *c;
    const char *tab = arg ? strchr(arg, '\t') : NULL;
    sh_ms_list l;
    const sh_ms_entry *e;
    unsigned char *text = NULL;
    char *author = NULL;
    size_t len = 0, alen = 0;
    int had_block = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t *w;
    mu_buf b;
    int rc;
    if (!tab) return mu_simple_result(action, 0, "Choose a file of a mod.");
    key = sh_textn(arg, (size_t)(tab - arg));
    rel = _strdup(tab + 1);
    if (!key || !rel) { free(key); free(rel); return -1; }
    if (!mu_rel_ok(rel) || !mu_text_kind(rel) || !sh_ms_enumerate(&l)) {
        rc = mu_simple_result(action, 0, !mu_rel_ok(rel) ? "That file is not inside the mod's folder."
                                         : !mu_text_kind(rel) ? "This file is binary; it has no text to show."
                                         : "Snapmap+ cannot read your mods folder. Check that it exists and is not locked, then press Refresh.");
        free(key);
        free(rel);
        return rc;
    }
    e = mu_entry_named(&l, key);
    free(key);
    if (!e || !e->folder || !(full = sh_textf("%s\\%s", e->folder, rel))) {
        sh_ms_list_free(&l);
        free(rel);
        return mu_simple_result(action, 0, "That mod is no longer installed.");
    }
    for (c = full; *c; c++) if (*c == '/') *c = '\\';
    w = sh_mb_wpath(full);
    if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        free(w);
        free(full);
        free(rel);
        sh_ms_list_free(&l);
        return mu_simple_result(action, 0, "The mod's folder does not hold this file; it is read from your install.");
    }
    free(w);
    text = sh_mb_read_file(full, (size_t)-1, &len);
    free(full);
    if (!text) {
        sh_ms_list_free(&l);
        msg = sh_textf("%s could not be read; close any program that holds it and try again.", rel);
        free(rel);
        rc = mu_simple_result(action, 0, msg ? msg : "That file could not be read.");
        free(msg);
        return rc;
    }
    {
        /* A UTF-8 byte order mark is not part of the text. */
        size_t skip = len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF ? 3 : 0;
        if (skip) { memmove(text, text + 3, len - 3); len -= 3; text[len] = 0; }
    }
    if (!mu_utf8_text(text, len)) {
        free(text);
        free(rel);
        sh_ms_list_free(&l);
        return mu_simple_result(action, 0, "This file is not plain text; it cannot be shown here.");
    }
    if (!strchr(rel, '/') && !strchr(rel, '\\') && !_stricmp(rel, "mod.json"))
        author = sh_mb_author_bytes((const char *)text, len, &alen, &had_block);
    memset(&b, 0, sizeof b);
    mu_result_head(&b, action, 1, "");
    mu_kstr(&b, "key", e->name, 0);
    mu_kstr(&b, "path", rel, 0);
    mu_knum(&b, "size", len, 0);
    mu_key(&b, "text", 0);
    if (author && had_block) {
        char *t = (char *)malloc(alen + 1);
        if (t) { memcpy(t, author, alen); t[alen] = 0; }
        mu_str(&b, t ? t : "");
        free(t);
    } else mu_str(&b, (const char *)text);
    mu_kbool(&b, "productBlock", author && had_block, 0);
    {
        char *plain = mu_file_cause(mu_entry_cause(e), rel)[0] ? mu_plain_cause(e, mu_entry_cause(e)) : NULL;
        mu_kstr(&b, "cause", plain ? plain : "", 0);
        free(plain);
    }
    free(author);
    free(text);
    free(rel);
    sh_ms_list_free(&l);
    return mu_result_tail(&b);
}

/* ---- Add to my mods */

/* Remove a tree this file made (wide long path), links removed and never followed. */
static void mu_remove_tree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t n = wcslen(dir);
    wchar_t *pattern = (wchar_t *)malloc((n + 3) * sizeof *pattern);
    if (!pattern) return;
    _snwprintf_s(pattern, n + 3, _TRUNCATE, L"%ls\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t nl = wcslen(fd.cFileName);
            wchar_t *p;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            p = (wchar_t *)malloc((n + nl + 2) * sizeof *p);
            if (!p) continue;
            _snwprintf_s(p, n + nl + 2, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
            SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                mu_remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);
            else DeleteFileW(p);
            free(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* content: the map content on disk, one line per kept or played map's mod and a storage line, from this computer
 * alone: "lines" the texts and "rows" the token Add to my mods takes per line ("" for storage). With arg
 * "removePlayed", the content of maps only played is removed first. */
typedef struct mu_lines { mu_buf *b, *rows; int n; } mu_lines;
static void mu_content_line(void *context, const char *line, const char *row)
{
    mu_lines *l = (mu_lines *)context;
    if (l->n++) { mu_putn(l->b, ",", 1); mu_putn(l->rows, ",", 1); }
    mu_str(l->b, line);
    mu_str(l->rows, row ? row : "");
}

static int mu_content(const char *arg)
{
    const int action = SH_MODS_ACT_CONTENT;
    char *storage, *message = NULL;
    mu_buf b, rows;
    mu_lines lines;
    int swept = arg && !strcmp(arg, "removePlayed");
    if (swept) {
        sh_cache_sweep_stats st;
        double mb;
        memset(&st, 0, sizeof st);
        (void)sh_cache_sweep_kept(1, &st);
        mb = (double)st.bytes_removed / (1024.0 * 1024.0);
        message = st.bytes_removed ? sh_textf("Removed %.1f MB of content of maps you only played; playing one of them again gets it back.", mb)
                                   : sh_text("No map you only played had content to remove.");
    }
    storage = sh_cache_storage_line();
    memset(&b, 0, sizeof b);
    mu_result_head(&b, action, 1, message ? message : storage ? storage : "");
    mu_kstr(&b, "storage", storage ? storage : "", 0);
    mu_key(&b, "lines", 0);
    mu_putn(&b, "[", 1);
    memset(&rows, 0, sizeof rows);
    lines.b = &b;
    lines.rows = &rows;
    lines.n = 0;
    (void)sh_cache_report(mu_content_line, &lines);
    mu_putn(&b, "]", 1);
    mu_key(&b, "rows", 0);
    mu_putn(&b, "[", 1);
    if (rows.len) mu_putn(&b, rows.p, rows.len);
    mu_putn(&b, "]", 1);
    free(rows.p);
    free(storage);
    free(message);
    return mu_result_tail(&b);
}
/* addToMine (arg: a content line's row, mu_content): the row's units made a mod folder of the player's own, "<name>
 * (my copy)", with a new salt, built by the rescan. Only the objects that map has. */
static int mu_add_mine(const char *row)
{
    const int action = SH_MODS_ACT_ADD_MINE;
    sh_cache_copy copy;
    char *why = NULL, *made = NULL, *cause = NULL, *msg = NULL;
    const char *shown;
    int ok = 0, rc;
    mu_buf x;
    memset(&x, 0, sizeof x);
    if (!row || !row[0]) return mu_simple_result(action, 0, "Select a map's mod first.");
    if (!sh_cache_stage_copy(row, &copy, &why)) {
        mu_log("add to my mods %.16s: refused: %s", row, why ? why : "");
        rc = mu_simple_result(action, 0, why ? why : "This mod could not be copied.");
        free(why);
        return rc;
    }
    /* A row with no name is copied under its salt: a sentence never says it. */
    shown = copy.salt[0] && copy.name && !strncmp(copy.name, copy.salt, strlen(copy.salt)) ? "This mod" : copy.name;
    mu_log("add to my mods %.16s: copying %s (%zu object(s)) into your mods", row, copy.name, copy.objects);
    ok = sh_ms_add_copy(copy.dir, copy.modjson, copy.modjson_len, copy.salt, copy.name, &made, &cause);
    if (ok) {
        mu_log("add to my mods %.16s: now mods\\%s", row, made ? made : "");
        mu_sync();
        if (copy.objects)
            mu_msg(&msg, "%s is now one of your mods: the %zu object%s from this map.", shown, copy.objects,
                   copy.objects == 1 ? "" : "s");
        else mu_msg(&msg, "%s is now one of your mods: what this map uses of it.", shown);
    } else {
        mu_log("add to my mods %.16s: refused: %s", row, cause ? cause : "");
        msg = mu_sentence_end(sh_textf("%s was not added to your mods: %s", shown, cause && cause[0] ? cause : "it could not be copied"));
    }
    sh_cache_copy_free(&copy);
    mu_kstr(&x, "key", ok && made ? made : "", 0);
    rc = mu_file_result(action, ok, msg ? msg : "", &x, ok);
    free(made);
    free(cause);
    free(msg);
    return rc;
}

/* newMod: a mod folder named after the mod with a working example (an entityDef and its Toybox row), built at once
 * by the rescan so the list shows it ready. There is no folder watcher: the message says Refresh picks up the
 * author's edits. */
static int mu_new_mod(const char *name)
{
    const int action = SH_MODS_ACT_NEW_MOD;
    char salt[SH_MB_ID_MAX + 1], *folder = NULL, *example = NULL, *cause = NULL, *plain = NULL, *msg = NULL, *key;
    sh_ms_list l;
    const sh_ms_entry *e = NULL;
    int ok, rc;
    size_t i;
    mu_buf x;
    memset(&x, 0, sizeof x);
    if (!sh_ms_new_mod_example(name, salt, &folder, &example, &cause)) {
        mu_log("new mod: refused: %s", cause ? cause : "");
        msg = mu_sentence_end(sh_textf("No mod was created: %s", cause ? cause : "out of memory"));
        rc = mu_simple_result(action, 0, msg ? msg : "No mod was created.");
        free(msg);
        free(cause);
        free(folder);
        free(example);
        return rc;
    }
    mu_log("new mod: %s (salt %s) with its example %s", folder, salt, example);
    mu_sync();
    ok = sh_ms_enumerate(&l);
    for (i = 0; ok && i < l.count && !e; i++)
        if (l.items[i].folder && !_stricmp(l.items[i].folder, folder)) e = &l.items[i];
    if (!e && ok) e = sh_ms_find_salt(&l, salt, NULL);
    key = _strdup(e ? e->name : mu_leaf(folder));
    if (e && sh_ms_entry_usable(e))
        msg = sh_textf("Created %s in your mods folder, as %s. Its Example crate is in the Toybox. After you change its "
                       "files, press Refresh to see the change.", name, key ? key : "");
    else {
        plain = mu_plain_cause(e, e ? e->cause : "not built yet");
        msg = sh_textf("Created %s in your mods folder, as %s, but it cannot be used yet. %s", name, key ? key : "",
                       plain ? plain : "");
    }
    mu_kstr(&x, "key", key ? key : "", 0);
    mu_kstr(&x, "file", example ? example : "", 0);
    if (ok) sh_ms_list_free(&l);
    rc = mu_file_result(action, 1, msg ? msg : "", &x, 1);
    free(key);
    free(plain);
    free(msg);
    free(cause);
    free(folder);
    free(example);
    return rc;
}

/* ---- the console verbs */

static void mu_say_scope(void *context, const char *line) { mu_line((const mu_out *)context, "map: %s", line); }

static void mu_say_map(const mu_out *o)
{
    mu_model m;
    mu_map mp;
    mu_record rec;
    size_t i, k;
    if (!mu_model_build(&m)) { mu_model_free(&m); mu_line(o, "map: out of memory"); return; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    if (mp.entities < 0) mu_line(o, "map: %s", mp.entities == -1 ? "no map is open in the editor" : "the open map cannot be read on this build");
    else {
        char when[64] = "unknown";
        if (mp.saved > 0) {
            time_t t = (time_t)mp.saved;
            struct tm tm;
            if (!localtime_s(&tm, &t)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        }
        if (mp.have_info && mp.info.opened_from[0])
            mu_line(o, "map: not saved yet (opened from the rawmap %s; the first save names it), %d entities",
                    mp.info.opened_from, mp.entities);
        else
            mu_line(o, "map: %s, %d entities, last saved %s", mp.name && mp.name[0] ? mp.name : "(not saved yet)", mp.entities,
                    when);
        if (mp.link) mu_line(o, "map: published; its mods %s", mp.claim ? "are uploaded with it" : "are not uploaded");
        else mu_line(o, "map: no publish of it with mods is recorded on this computer");
        mu_line(o, "map: search tag mods: %s", !mp.have_info || mp.info.tagged < 0 ? "unknown" : mp.info.tagged ? "present" : "absent");
        if (!mp.mod_count) mu_line(o, "map: uses no mods; it plays without Snapmap+");
        for (i = 0; i < mp.mod_count; i++) {
            const mu_mapmod *x = &mp.mods[i];
            int at = mu_mapmod_entry(&m, x);
            sh_mm_ref *refs = NULL;
            size_t n = mp.have_mm && x->uid_count ? sh_mm_references(&mp.mm, x->uids, x->uid_count, &refs) : 0;
            if (n == (size_t)-1) n = 0;
            const char *row_name = "", *state = mu_mapmod_state(&m, &mp, x);
            for (k = 0; at < 0 && k < mp.rows.count && !row_name[0]; k++)
                if (!_stricmp(mp.rows.rows[k].salt, x->id)) row_name = sh_modmanifest_row_name(&mp.rows.rows[k]);
            mu_line(o, "map: mod %s (%s%s, %s): %s%zu placed entit%s%s, %zu wire or timeline reference(s)%s%s",
                    at >= 0 ? mu_entry_title(&m.list.items[at]) : row_name[0] ? row_name : "a mod",
                    at >= 0 ? "mods\\" : "", at >= 0 ? m.list.items[at].name : "not installed",
                    !strcmp(state, "installed") ? "ready" : state,
                    x->attached ? "used by the whole map, " : "", x->uid_count,
                    x->uid_count == 1 ? "y" : "ies", x->modules ? ", placed modules" : "", n,
                    x->in_rows ? "" : ", not listed yet (the next save lists it)", mu_mapmod_follows(x) ? ", needed by another mod" : "");
            if (x->declared[0]) mu_say_substituted(o, &m, "map:   ", x->id, x->declared, x->hash);
            for (k = 0; k < x->uid_count; k++) {
                char *name = mu_entity_label(&m, sh_mm_find(&mp.mm, x->uids[k]), x->uids[k]);
                mu_line(o, "map:   entity %s", name ? name : "");
                free(name);
            }
            for (k = 0; k < n; k++) {
                char *name = mu_ref_name(&m, &mp, &refs[k]);
                mu_line(o, "map:   %s %s", refs[k].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", name ? name : "");
                free(name);
            }
            free(refs);
        }
    }
    (void)sh_modserve_scope_report(mu_say_scope, (void *)o);
    (void)sh_modserve_registry_report(mu_say_scope, (void *)o);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
}

int sh_modui_command(int argc, const char *const *argv, sh_modui_line_fn line, void *context)
{
    mu_out o;
    const char *verb = argc > 0 ? argv[0] : NULL;
    const char *key = NULL;
    int yes = 0, i, first;
    o.fn = line;
    o.context = context;
    if (!verb) return 0;
    if (_stricmp(verb, "map") && _stricmp(verb, "uninstall") && _stricmp(verb, "choose")) return 0;
    if (!mu_on_main()) return -1;
    if (!_stricmp(verb, "choose")) {
        if (argc < 2 || argc > 3 || !argv[1][0]) {
            mu_line(&o, "choose: usage: sh_mods choose \"<setting>\" \"<mod name>\" (no name clears the choice)");
            return 1;
        }
        /* The mod by its name or folder, among the mods the open map uses
         * (sh_modregistry_choose_command resolves it). */
        (void)mu_choose(argv[1], argc == 3 ? argv[2] : "", &o);
        return 1;
    }
    if (!_stricmp(verb, "map") && argc == 1) { mu_say_map(&o); return 1; }
    first = !_stricmp(verb, "map") ? 2 : 1;
    if (!_stricmp(verb, "map") && !_stricmp(argv[1], "add")) {
        if (argc != 3 || !argv[2][0]) { mu_line(&o, "map: usage: sh_mods map add \"<mod folder>\""); return 1; }
        (void)mu_map_add(argv[2], &o);
        return 1;
    }
    if (!_stricmp(verb, "map") && _stricmp(argv[1], "remove")) {
        mu_line(&o, "map: usage: sh_mods map, sh_mods map add \"<mod folder>\", or sh_mods map "
                    "remove \"<mod folder>\" [--yes]");
        return 1;
    }
    for (i = first; i < argc; i++) {
        if (!_stricmp(argv[i], "--yes")) yes = 1;
        else if (!key) key = argv[i];
        else { key = NULL; break; }
    }
    if (!key || !key[0]) {
        mu_line(&o, "%s: usage: sh_mods %s \"<mod folder>\" [--yes]", verb, !_stricmp(verb, "map") ? "map remove" : "uninstall");
        return 1;
    }
    if (!_stricmp(verb, "map")) (void)mu_map_action(yes ? SH_MODS_ACT_MAP_REMOVE : SH_MODS_ACT_MAP_REMOVE_PREVIEW, key, &o, 0);
    else (void)mu_map_action(yes ? SH_MODS_ACT_UNINSTALL : SH_MODS_ACT_UNINSTALL_PREVIEW, key, &o, 1);
    return 1;
}

/* ------------------------------------------------------------------ the doctor */

static void mu_print_entry(const mu_out *o, const mu_model *m, size_t i)
{
    const mu_entry *u = &m->entries[i];
    const sh_ms_entry *e = u->e;
    mu_say(o, "mod mods\\%s (%s): %s%s%s", e->name, mu_entry_title(e),
           u->admitted && !u->cause ? "ready" : !strcmp(u->state, "unindexed") ? "not read yet" : "needs attention",
           u->cause ? ": " : "", u->cause ? u->cause : "");
    if (e->loaded) {
        size_t k;
        for (k = 0; k < e->bundle.warning_list_count; k++) {
            mu_say(o, "mod mods\\%s: warning %zu of %u: %s", e->name, k + 1, e->bundle.warnings, e->bundle.warning_list[k]);
        }
    }
    if (u->same_as >= 0)
        mu_say(o, "mod mods\\%s: carries %u of the same files as mods\\%s; both are mods of their own", e->name, u->same_count,
               m->list.items[u->same_as].name);
}

static void mu_say_requirement(void *context, const char *line) { mu_say((const mu_out *)context, "%s", line); }

int sh_modui_doctor(int verify, sh_modui_line_fn line, void *context)
{
    mu_model m;
    mu_out o;
    sh_modserve_stats st;
    size_t i;
    if (!mu_on_main()) return -1;
    o.fn = line;
    o.context = context;
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    sh_modserve_get_stats(&st);
    mu_say(&o, "mods folder %s: %zu mod folder(s); all listed in the Mods tab%s",
           m.root_ok ? m.root : "(unresolved)", m.list.count, m.listed ? "" : "; the folder cannot be read");
    mu_say(&o, "in use: %d of %d mod(s), %d file(s), %d replaced game file(s), %d conflict(s) (%d combined), %d Toybox row(s)",
           st.bundles_admitted, st.bundles_seen, st.files, st.shadows, st.conflicts, st.conflicts_composed, st.editor_rows_registered);
    for (i = 0; i < m.list.count; i++) mu_print_entry(&o, &m, i);
    (void)sh_modserve_scope_report(mu_say_requirement, &o);
    mu_say_substitutions(&o, &m, "doctor: map row ");             /* rows served through another build */
    (void)sh_modserve_registry_report(mu_say_requirement, &o);   /* what the registries generate, every clash */
    for (i = 0; i < m.list.count; i++) {                          /* stock banks borrowed sound events need */
        char banks[8][SH_MBI_LEAF_MAX];
        int nb, q;
        if (!m.list.items[i].loaded) continue;
        nb = mu_stock_banks(&m.list.items[i].bundle, banks);
        for (q = 0; q < nb; q++) {
            int result = 0;
            double ms = 0;
            int st = sh_mbi_state_get(banks[q], &result, &ms, NULL, 0);
            char how[200];
            switch (st) {
            case SH_MBI_ST_LOADED: _snprintf_s(how, sizeof how, _TRUNCATE, "loaded in this session (%.0f ms)", ms); break;
            case SH_MBI_ST_LOADING: strcpy_s(how, sizeof how, "loading (asked for at a map load; no answer yet)"); break;
            case SH_MBI_ST_FAILED: _snprintf_s(how, sizeof how, _TRUNCATE, "did not load (AKRESULT %d); not retried this session", result); break;
            case SH_MBI_ST_REFUSED: strcpy_s(how, sizeof how, "not loaded: the stock-bank route refused it (see the MODS: sound: lines)"); break;
            default: strcpy_s(how, sizeof how, "not loaded yet; it loads at the first map load that declares the mod"); break;
            }
            mu_say(&o, "sound: mods\\%s needs the game's sound bank %s, which SnapMap skips: %s", m.list.items[i].name,
                   banks[q], how);
        }
    }
    (void)sh_modserve_requirements_report(mu_say_requirement, &o);
    (void)sh_modserve_misses_report(mu_say_requirement, &o);   /* files a map's part of a mod left out */
    mu_print_shadows(&o, &m);
    if (verify) {
        sh_ms_verify_stats sum;
        unsigned passed = 0, failed = 0;
        char *first = NULL, *msg;
        mu_say(&o, "verify: checking every mod's files");
        if (!mu_verify_all(&o, &sum, &passed, &failed, &first)) mu_say(&o, "verify: the mods folder cannot be read");
        else {
            msg = mu_verify_message(passed, failed, first);
            mu_say(&o, "verify: %s (%u file(s), %u MiB)", msg ? msg : "", sum.files, sum.bytes_mb);
            free(msg);
        }
        free(first);
        InterlockedIncrement(&g_gen);
    }
    mu_say(&o, "done");
    mu_model_free(&m);
    return 1;
}

int sh_modui_run(int action, const char *arg)
{
    const sh_modui_engine *e = mu_engine();
    char note[160];
    /* Off the main thread only the file actions run, and only their file part; the key's requests touch no engine. */
    if (action == SH_MODS_ACT_KEY) return mu_key_request(arg);
    if (!mu_on_main() && !mu_file_action(action)) return -1;
    /* Reading a mod's files needs no engine hook; every other action does. */
    if (action != SH_MODS_ACT_FILES && action != SH_MODS_ACT_FILE_TEXT &&
        sh_feature_refusal(SH_FEATURE_MODS, note, sizeof note))
        return mu_simple_result(action, 0, note);
    switch (action) {
    case SH_MODS_ACT_INSTALL:
        return mu_install(arg);
    case SH_MODS_ACT_RESCAN: {
        /* Refresh: every changed folder built again, then the game's part.
         * Its message is only the sentence a change still needs. */
        mu_buf x;
        memset(&x, 0, sizeof x);
        mu_sync();
        return mu_file_result(action, 1, "", &x, 1);
    }
    case SH_MODS_ACT_APPLY:
        return mu_apply_owed();
    case SH_MODS_ACT_PALETTE: {
        int r;
        if (!e->palette_rebuild) return mu_simple_result(action, 0, "The editor palette cannot be rebuilt from here.");
        r = e->palette_rebuild();
        InterlockedIncrement(&g_gen);
        mu_log("palette rebuild: %s", r == 1 ? "ran" : "refused");
        return mu_simple_result(action, r == 1, r == 1
            ? "Editor palette rebuilt; new mod rows are in the editor."
            : "Palette not rebuilt: open a map in the editor and release any held entity or wire, then try again.");
    }
    case SH_MODS_ACT_UNINSTALL:
    case SH_MODS_ACT_UNINSTALL_PREVIEW:
    case SH_MODS_ACT_MAP_REMOVE:
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW:
        return mu_map_action(action, arg, NULL, 0);
    case SH_MODS_ACT_MAP_ADD:
        return mu_map_add(arg, NULL);
    case SH_MODS_ACT_CHOOSE: {
        const char *tab = arg ? strchr(arg, '\t') : NULL;
        char *slot;
        int r;
        if (!tab) return mu_simple_result(action, 0, "Choose which mod's value this map uses.");
        if (!(slot = sh_textn(arg, (size_t)(tab - arg)))) return -1;
        r = mu_choose(slot, tab + 1, NULL);
        free(slot);
        return r;
    }
    case SH_MODS_ACT_FILES:
        return mu_files(arg);
    case SH_MODS_ACT_FILE_TEXT:
        return mu_file_text(arg);
    case SH_MODS_ACT_NEW_MOD:
        return mu_new_mod(arg);
    case SH_MODS_ACT_ADD_MINE:
        return mu_add_mine(arg);
    case SH_MODS_ACT_CONTENT:
        return mu_content(arg);
    case SH_MODS_ACT_MAP_UPDATE:
        return mu_map_update(arg, NULL);
    default:
        return mu_simple_result(action, 0, "Unknown action.");
    }
}

/* ------------------------------------------------------------------ the slot */

/* A size-query reader (sh_modui_fetch, sh_modui_scripts) as owned interface
 * text in *out: asked again until the text fits, since it can grow between
 * the two reads. Returns the reader's answer (1 copied, 0 nothing). */
typedef int (*mu_reader)(void *ctx, char *out, int cap, int *needed);
static int mu_owned(mu_reader read, void *ctx, char **out)
{
    int needed = 0, r = read(ctx, NULL, 0, &needed);
    *out = NULL;
    while (r == -1 && needed >= 0) {
        char *text = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)needed + 1);
        if (!text) return 0;
        r = read(ctx, text, needed + 1, &needed);
        if (r == 1) { *out = text; return 1; }
        HeapFree(GetProcessHeap(), 0, text);
    }
    return r == 1 ? 1 : 0;
}

static int mu_read_fetch(void *ctx, char *out, int cap, int *needed)
{
    return sh_modui_fetch(out, cap, needed, (unsigned *)ctx);
}

static int mu_read_scripts(void *ctx, char *out, int cap, int *needed)
{
    return sh_modui_scripts((const char *)ctx, out, cap, needed);
}

int sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call)
{
    int n;
    (void)self;
    if (!call) return 0;
    call->out = NULL;
    call->generation = (unsigned)g_gen;
    switch (call->op) {
    case SH_MODS_LIST:
        n = sh_modui_list();
        call->generation = (unsigned)g_gen;
        return n >= 0;
    case SH_MODS_RUN:
        n = sh_modui_run(call->action, call->arg);
        call->generation = (unsigned)g_gen;
        return n >= 0;
    case SH_MODS_FETCH:
        return mu_owned(mu_read_fetch, &call->generation, &call->out);
    case SH_MODS_SCRIPTS:
        /* Read only and on any thread: the runtime's own state, not the shared buffer. */
        return mu_owned(mu_read_scripts, (void *)(call->arg ? call->arg : ""), &call->out);
    case SH_MODS_STORE: {
        unsigned gen = 0;
        call->busy = modstore_lock_busy(&gen) ? 1 : 0;
        call->generation = gen;
        return 1;
    }
    case SH_MODS_GENERATION:
        return 1;
    case SH_MODS_ROOT: {
        const char *root = sh_ms_root();
        if (!root) return 0;
        return (call->out = sh_iface_text(root)) != NULL;
    }
    default:
        return 0;
    }
}
