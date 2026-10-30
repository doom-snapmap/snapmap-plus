/* modui.c -- see modui.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "snapmap_plus_iface.h"
#include "modui.h"
#include "modbundle.h"
#include "modstore.h"
#include "modserve.h"
#include "modmanifest.h"
#include "backend_log.h"
#include "config_json.h"
#include "modmap.h"
#include "mp_json.h"
#include "editor_delete.h"
#include <shlobj.h>
#include <time.h>
#ifndef SH_MODUI_PURE
#include "overrides.h"
#include "modstock.h"
#include "modmigrate.h"
#endif

/* A stock resource every 13954591 SnapMap install ships (as modcmd.c probes):
 * reading it proves the installed-resource reader works before its answers
 * are trusted. */
#define MU_STOCK_PROBE "generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl"

#define MU_MAX_SHADOW_GROUPS 4000   /* shadow rows sent to the page; the total is always reported */
#define MU_MAX_ROW_NAMES     12     /* names listed per map bundle; the count is always reported */
#define MU_MAX_PLACED        65536  /* distinct placed names read from the map */
#define MU_NAME_MAX          512
#define MU_ORIGIN_MAX        600    /* "overrides\<folder>" or "mods\<folder>" */
#define MU_MAX_CARRIED       200    /* carried paths listed per mod; the page says how many more */
#define MU_MAX_ROW_DECL      (256u * 1024u)   /* an editor row decl read for its display name */
#define MU_MAX_MAP_ENTITIES  64     /* placed entity names listed per mod; the count is always sent */
#define MU_MAX_MAP_REFS      64     /* references listed per mod */
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
    char line[1024];
    va_list ap;
    memcpy(line, "MODS: ui ", 9);
    va_start(ap, fmt);
    _vsnprintf_s(line + 9, sizeof line - 9, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* ------------------------------------------------------------------ product defaults */

#ifndef SH_MODUI_PURE
static int mu_default_stock(const char *request)
{
    static int probed;   /* 0 unprobed, 1 ok, -1 unavailable */
    unsigned char *body = NULL;
    size_t len = 0;
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

static void mu_defaults(sh_modui_engine *e)
{
    memset(e, 0, sizeof *e);
#ifndef SH_MODUI_PURE
    e->rescan = sh_modserve_index_rescan;
    e->refresh = sh_modserve_refresh_after_rescan;
    e->palette_rebuild = modserve_palette_rebuild_requested;
    e->stock_exists = mu_default_stock;
    e->migrate_pending = sh_modmigrate_pending;
    e->migrate_start = sh_modmigrate_start;
    e->migrate_busy = sh_modmigrate_busy;
#endif
}

static int mu_restore(const char *const *hashes, int hash_count, const char *const *names, size_t name_count, int when);
static int mu_bin_armed(void);

void sh_modui_bind(const sh_modui_engine *engine)
{
    mu_defaults(&g_engine);
    if (engine) {
        if (engine->on_main_thread) g_engine.on_main_thread = engine->on_main_thread;
        if (engine->placed_names) g_engine.placed_names = engine->placed_names;
        if (engine->rescan) g_engine.rescan = engine->rescan;
        if (engine->refresh) g_engine.refresh = engine->refresh;
        if (engine->palette_rebuild) g_engine.palette_rebuild = engine->palette_rebuild;
        if (engine->stock_exists) g_engine.stock_exists = engine->stock_exists;
        if (engine->migrate_pending) g_engine.migrate_pending = engine->migrate_pending;
        if (engine->migrate_start) g_engine.migrate_start = engine->migrate_start;
        if (engine->migrate_busy) g_engine.migrate_busy = engine->migrate_busy;
        if (engine->map_json) g_engine.map_json = engine->map_json;
        if (engine->map_info) g_engine.map_info = engine->map_info;
        if (engine->delete_entities) g_engine.delete_entities = engine->delete_entities;
        if (engine->remove_tag) g_engine.remove_tag = engine->remove_tag;
        if (engine->busy) g_engine.busy = engine->busy;
        if (engine->entity_valid) g_engine.entity_valid = engine->entity_valid;
        if (engine->toast) g_engine.toast = engine->toast;
        if (engine->print) g_engine.print = engine->print;
    }
    g_engine_bound = 1;
    /* The points where a map's needs become known reach the recycle bin. */
    sh_modserve_set_restore(mu_restore);
    sh_modmanifest_set_restore(mu_restore, mu_bin_armed);
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

/* ------------------------------------------------------------------ store model */

/* What the document says about one store entry. */
typedef struct mu_entry {
    const sh_ms_entry *e;
    const char *state;           /* active, superseded, unindexed, skipped */
    char cause[SH_MB_CAUSE_MAX];
    int superseded_paths;        /* paths a newer build of the same id serves */
    int superseded_by;           /* entry index, or -1 */
    int editor_rows;             /* snapEditorEntityDef files */
    int shadows;
    int legacy;                  /* a converted first-generation package (its "legacy" record): grandfathered shadows */
    int admitted;                /* sh_modserve_bundle_state == 1 */
    int row;                     /* its row in the tab */
    char origin[MU_ORIGIN_MAX];  /* where it came from, "" when the store cannot tell */
    char **row_names;            /* its editor rows' display names (strings resolved, else the decl name) */
    char **row_defs;             /* each row's entityDef: what a placed entity of that row inherits */
    int row_count;
} mu_entry;

/* One row of the tab: one mod folder. */
typedef struct mu_row {
    int serving;                 /* the entry shown */
    int builds;                  /* entries in the row (always 1: a folder is a row) */
    int duplicate;               /* another folder declares the same id */
} mu_row;

typedef struct mu_model {
    sh_ms_list list;
    int listed;
    mu_entry *entries;
    mu_row *rows;
    size_t row_count;
    char root[MAX_PATH * 4];
    int root_ok;
} mu_model;

/* The entry serving `hash`: its own bundle, or the one an alias of it names
 * (an earlier build of a folder, a deduplicated copy). */
static int mu_find_hash(const mu_model *m, const char *hash)
{
    const sh_ms_entry *e = sh_ms_resolve(&m->list, hash);
    if (!e || !e->loaded) return -1;
    return (int)(e - m->list.items);
}

/* Whether two pinned hashes name the same installed bundle. */
static int mu_same_bundle(const mu_model *m, const char *a, const char *b)
{
    int x;
    if (!strcmp(a, b)) return 1;
    x = mu_find_hash(m, a);
    return x >= 0 && x == mu_find_hash(m, b);
}

static int mu_find_id(const mu_model *m, const char *id, int except)
{
    size_t i;
    for (i = 0; i < m->list.count; i++)
        if ((int)i != except && m->list.items[i].loaded && !strcmp(m->list.items[i].bundle.id, id)) return (int)i;
    return -1;
}

/* The serving index's order between two builds of one mod: the later
 * installedAt wins, then the later folder name (modserve.c ms_newer). */
static const char *mu_leaf(const char *folder)
{
    const char *a = strrchr(folder ? folder : "", '\\'), *b = strrchr(folder ? folder : "", '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : (folder ? folder : "");
}

static int mu_newer(const sh_ms_entry *a, const sh_ms_entry *b)
{
    if (a->installed_at != b->installed_at) return a->installed_at > b->installed_at;
    return strcmp(mu_leaf(a->folder), mu_leaf(b->folder)) > 0;
}

static void mu_model_free(mu_model *m)
{
    size_t i;
    int k;
    for (i = 0; m->entries && i < m->list.count; i++) {
        for (k = 0; k < m->entries[i].row_count; k++) { free(m->entries[i].row_names[k]); free(m->entries[i].row_defs[k]); }
        free(m->entries[i].row_names);
        free(m->entries[i].row_defs);
    }
    if (m->listed) sh_ms_list_free(&m->list);
    free(m->entries);
    free(m->rows);
    memset(m, 0, sizeof *m);
}

/* Where an entry came from: the overrides folder the migration built it from
 * (its "legacy" record), a map that delivered it, a download or a copy the
 * product installed, or "" for a folder the author made in mods\. */
static void mu_origin(const sh_ms_entry *e, char *out, size_t cap)
{
    out[0] = 0;
    if (e->loaded && e->bundle.extra) {
        sh_json_object extra, legacy;
        const char *lv, *fv;
        memset(&extra, 0, sizeof extra);
        memset(&legacy, 0, sizeof legacy);
        if (sh_json_parse_object(e->bundle.extra, strlen(e->bundle.extra), 16, &extra)) {
            lv = sh_json_object_get(&extra, "legacy");
            if (lv && lv[0] == '{' && sh_json_parse_object(lv, strlen(lv), 16, &legacy)) {
                char folder[MU_ORIGIN_MAX];
                size_t n = 0;
                fv = sh_json_object_get(&legacy, "folder");
                if (!e->bundle.delivered && fv && fv[0] == '"' && sh_json_decode_string(fv, strlen(fv), folder, sizeof folder, &n) && n)
                    _snprintf_s(out, cap, _TRUNCATE, "overrides\\%s", folder);   /* in the retired package folder */
                sh_json_object_free(&legacy);
            }
            sh_json_object_free(&extra);
        }
        if (out[0]) return;
    }
    if (!e->loaded) return;
    if (!strcmp(e->bundle.origin, SH_MB_ORIGIN_DELIVERY)) strncpy_s(out, cap, "a map that delivered it", _TRUNCATE);
    else if (!strcmp(e->bundle.origin, SH_MB_ORIGIN_DOWNLOAD)) strncpy_s(out, cap, "a download", _TRUNCATE);
    else if (!strcmp(e->bundle.origin, SH_MB_ORIGIN_INSTALL)) strncpy_s(out, cap, "an installed copy", _TRUNCATE);
}

/* The version a row shows: a version the converter generated for a package
 * a map delivered without one is part of its hash, not something to show. */
static const char *mu_version(const char *v)
{
    return !v || sh_mb_version_generated(v) ? "" : v;
}

/* The product made the folder (a copy Remove deletes); else the author's. */
static int mu_product_folder(const sh_ms_entry *e)
{
    return e->loaded && e->bundle.origin[0];
}

/* Which build of a row is shown: one that serves, then a superseded one,
 * then one waiting for a scan, then a skipped one; the newest among equals. */
static int mu_rank(const mu_entry *u)
{
    if (u->admitted && strcmp(u->state, "superseded")) return 3;
    if (u->admitted) return 2;
    return !strcmp(u->state, "unindexed") ? 1 : 0;
}

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

/* The text of a "#str_" key in a bundle's strings.en (keys omit the prefix,
 * matched case-insensitively), malloc'd, or NULL. */
static char *mu_bundle_text(const sh_mb_bundle *b, const char *tag)
{
    mpj *x;
    const mpj *s, *en;
    char *out = NULL;
    int i;
    if (!b->extra || !tag) return NULL;
    if (!_strnicmp(tag, "#str_", 5)) tag += 5;
    x = mpj_parse(b->extra, strlen(b->extra));
    s = x ? mpj_get(x, "strings") : NULL;
    en = s && s->type == MPJ_OBJ ? mpj_get(s, "en") : NULL;
    for (i = 0; en && en->type == MPJ_OBJ && i < en->n && !out; i++)
        if (!_stricmp(en->keys[i], tag) && en->items[i] && en->items[i]->type == MPJ_STR) out = sh_mm_decode(en->items[i]->text);
    mpj_free(x);
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

/* The editor rows of entry i: each snapEditorEntityDef file's display name
 * (its displayNameTag through the bundle's strings, then any installed
 * mod's, else the decl name) and the entityDef a placed entity of it
 * inherits. */
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
        char path[MAX_PATH * 4], *text = NULL, *tag = NULL, *def = NULL, *name = NULL, *c;
        if (!f->ptype || _stricmp(f->ptype, "snapEditorEntityDef")) continue;
        if (!f->stock) {
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", e->folder, f->path);
            for (c = path; *c; c++) if (*c == '/') *c = '\\';
            text = (char *)sh_mb_read_file(path, MU_MAX_ROW_DECL, NULL);
        }
        if (text) {
            tag = mu_decl_field(text, "displayNameTag");
            def = mu_decl_field(text, "entityDef");
        }
        if (tag) name = mu_bundle_text(&e->bundle, tag);
        for (q = 0; tag && !name && q < m->list.count; q++)
            if (q != i && m->list.items[q].loaded) name = mu_bundle_text(&m->list.items[q].bundle, tag);
        if (!name) name = mu_strdup(f->pname ? f->pname : f->request);
        u->row_names[u->row_count] = name;
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

/* One row per folder. Identity is the id in mod.json: two folders declaring
 * one id are both rows, and both are duplicates ("another installed mod
 * declares this id"). */
static int mu_model_rows(mu_model *m)
{
    size_t i, k, n = m->list.count;
    m->rows = (mu_row *)calloc(n ? n : 1, sizeof *m->rows);
    if (!m->rows) return 0;
    for (i = 0; i < n; i++) {
        const sh_ms_entry *e = m->entries[i].e;
        mu_row *r = &m->rows[m->row_count];
        r->serving = (int)i;
        r->builds = 1;
        r->duplicate = 0;
        for (k = 0; k < n && e->loaded && !r->duplicate; k++)
            r->duplicate = k != i && m->list.items[k].loaded && !strcmp(m->list.items[k].bundle.id, e->bundle.id);
        m->entries[i].row = (int)m->row_count++;
    }
    return 1;
}

static int mu_model_build(mu_model *m)
{
    size_t i, k;
    memset(m, 0, sizeof *m);
    m->root_ok = sh_ms_root(m->root, sizeof m->root);
    m->listed = sh_ms_enumerate(&m->list);
    if (!m->listed) memset(&m->list, 0, sizeof m->list);
    m->entries = (mu_entry *)calloc(m->list.count ? m->list.count : 1, sizeof(mu_entry));
    if (!m->entries) return 0;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        mu_entry *u = &m->entries[i];
        int st;
        u->e = e;
        u->superseded_by = -1;
        u->legacy = e->loaded && e->bundle.legacy;
        mu_origin(e, u->origin, sizeof u->origin);
        if (e->loaded)
            for (k = 0; k < e->bundle.file_count; k++) {
                const sh_mb_file *f = &e->bundle.files[k];
                if (f->ptype && !_stricmp(f->ptype, "snapEditorEntityDef")) u->editor_rows++;
                if (f->shadowed) u->shadows++;
            }
        if (!e->loaded) {
            u->state = "skipped";
            strncpy_s(u->cause, sizeof u->cause, e->cause[0] ? e->cause : "mod.json unreadable", _TRUNCATE);
            continue;
        }
        /* The serving index decides what is served; the store's own check
         * (sizes, the last verification) explains a build the index lacks,
         * and is a warning on one it serves. */
        st = sh_modserve_bundle_state(e->bundle.hash, u->cause, sizeof u->cause);
        if (st < 0) {
            u->state = "skipped";
            if (!u->cause[0]) strncpy_s(u->cause, sizeof u->cause, "skipped by the serving index", _TRUNCATE);
            continue;
        }
        if (st == 0) {
            if (!sh_ms_entry_usable(e)) {
                u->state = "skipped";
                strncpy_s(u->cause, sizeof u->cause, e->cause[0] ? e->cause : "unusable", _TRUNCATE);
            } else {
                u->state = "unindexed";
                strncpy_s(u->cause, sizeof u->cause, "not in the serving index yet; it is used after the next scan", _TRUNCATE);
            }
            continue;
        }
        if (!sh_ms_entry_usable(e) && e->cause[0])
            _snprintf_s(u->cause, sizeof u->cause, _TRUNCATE, "%s; 'sh_mods doctor verify' checks the build", e->cause);
        u->admitted = 1;
        u->state = "active";
    }
    /* Superseded: the serving table answers some of the bundle's paths with a
     * newer build of the same mod. */
    for (i = 0; i < m->list.count; i++) {
        mu_entry *u = &m->entries[i];
        const sh_ms_entry *e = u->e;
        if (!u->admitted) continue;
        for (k = 0; k < e->bundle.file_count; k++) {
            sh_modserve_hit hit;
            int other;
            if (!sh_modserve_lookup(e->bundle.files[k].request, &hit)) continue;
            if (!hit.bundle_hash || !strcmp(hit.bundle_hash, e->bundle.hash)) continue;
            if (!hit.bundle_id || strcmp(hit.bundle_id, e->bundle.id)) continue;
            other = mu_find_hash(m, hit.bundle_hash);
            if (other >= 0) {
                /* Equal bytes are a dedupe, whichever build the table picked. */
                const sh_mb_bundle *ob = &m->list.items[other].bundle;
                size_t q;
                int same = 0;
                for (q = 0; q < ob->file_count && !same; q++)
                    same = !strcmp(ob->files[q].request, e->bundle.files[k].request) &&
                           !strcmp(ob->files[q].blob, e->bundle.files[k].blob);
                if (same) continue;
            }
            u->superseded_paths++;
            if (u->superseded_by < 0) u->superseded_by = other;
        }
        if (u->superseded_paths) u->state = "superseded";
    }
    for (i = 0; i < m->list.count; i++) mu_entry_rows(m, i);
    return mu_model_rows(m);
}

/* ------------------------------------------------------------------ published maps */

/* The record's claims (modsync: a published item's uploaded bundle set) and
 * links (modpublish: which saved map an item was published from). Only the
 * bundle hashes and times are read; a claim's secret is never kept. */
typedef struct mu_claim {
    char ref[160];
    long long updated;
    char (*hashes)[SH_MB_HASH_HEX + 1];
    int count;
} mu_claim;

typedef struct mu_link { char item[160], cloud[160], local[160]; long long at; } mu_link;

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
    for (i = 0; i < r->claim_count; i++) free(r->claims[i].hashes);
    free(r->claims);
    free(r->links);
    memset(r, 0, sizeof *r);
}

static void mu_copy_str(char *out, size_t cap, const mpj *v)
{
    char *d = v && v->type == MPJ_STR ? sh_mm_decode(v->text) : NULL;
    strncpy_s(out, cap, d ? d : "", _TRUNCATE);
    free(d);
}

static void mu_record_load(const mu_model *m, mu_record *r)
{
    char *text = NULL;
    mpj *x;
    int i, k, got;
    memset(r, 0, sizeof *r);
    if (!m->root_ok) return;
    got = sh_mb_record_get(m->root, "claims", &text);
    r->claims_read = got >= 0;
    x = got == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    if (text) { SecureZeroMemory(text, strlen(text)); free(text); text = NULL; }
    if (x && x->type == MPJ_OBJ && x->n) {
        r->claims = (mu_claim *)calloc((size_t)x->n, sizeof *r->claims);
        for (i = 0; r->claims && i < x->n; i++) {
            const mpj *c = x->items[i], *h = mpj_get(c, "bundles");
            mu_claim *cl = &r->claims[r->claim_count];
            if (!c || c->type != MPJ_OBJ || !strncmp(x->keys[i], "publish-pending-", 16)) continue;
            strncpy_s(cl->ref, sizeof cl->ref, x->keys[i], _TRUNCATE);
            cl->updated = mpj_int(mpj_get(c, "updated"));
            if (h && h->type == MPJ_ARR && h->n) {
                cl->hashes = (char (*)[SH_MB_HASH_HEX + 1])calloc((size_t)h->n, sizeof *cl->hashes);
                for (k = 0; cl->hashes && k < h->n; k++)
                    if (h->items[k] && h->items[k]->type == MPJ_STR) {
                        strncpy_s(cl->hashes[cl->count], SH_MB_HASH_HEX + 1, h->items[k]->text, _TRUNCATE);
                        cl->count++;
                    }
            }
            r->claim_count++;
        }
    }
    if (x) {
        /* The tree holds the secrets' text too; clear it before release. */
        for (i = 0; x->type == MPJ_OBJ && i < x->n; i++) {
            mpj *s = mpj_get(x->items[i], "secret");
            if (s && s->type == MPJ_STR && s->text) SecureZeroMemory(s->text, strlen(s->text));
        }
        mpj_free(x);
    }
    x = sh_mb_record_get(m->root, "links", &text) == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    free(text);
    if (x && x->type == MPJ_OBJ && x->n) {
        r->links = (mu_link *)calloc((size_t)x->n, sizeof *r->links);
        for (i = 0; r->links && i < x->n; i++) {
            const mpj *l = x->items[i];
            mu_link *ln = &r->links[r->link_count];
            if (!l || l->type != MPJ_OBJ) continue;
            strncpy_s(ln->item, sizeof ln->item, x->keys[i], _TRUNCATE);
            mu_copy_str(ln->cloud, sizeof ln->cloud, mpj_get(l, "cloud"));
            mu_copy_str(ln->local, sizeof ln->local, mpj_get(l, "local"));
            ln->at = mpj_int(mpj_get(l, "at"));
            r->link_count++;
        }
    }
    mpj_free(x);
}

/* How many claims' bundle sets name entry i's build (directly or through an alias). */
static unsigned mu_published_maps(const mu_model *m, const mu_record *r, int entry)
{
    size_t i;
    int k;
    unsigned n = 0;
    for (i = 0; i < r->claim_count; i++) {
        int hit = 0;
        for (k = 0; k < r->claims[i].count && !hit; k++)
            hit = mu_find_hash(m, r->claims[i].hashes[k]) == entry;
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

/* Payload v3's fields of one mod (webview-ui.md, Mods tab). */
static void mu_put_row_v3(mu_buf *b, const mu_model *m, const mu_record *rec, const mu_entry *u)
{
    const sh_ms_entry *e = u->e;
    const sh_mb_bundle *bd = &e->bundle;
    char *desc = e->loaded ? mu_extra_string(bd, "description") : NULL;
    const char **carried = NULL;
    size_t k, ncar = 0;
    unsigned sounds = 0, movies = 0, modules = 0, vt = 0;
    int i;
    mu_kstr(b, "description", desc ? desc : "", 0);
    free(desc);
    mu_kstr(b, "origin", e->loaded ? bd->origin : "", 0);
    mu_kstr(b, "migratedFrom", !strncmp(u->origin, "overrides\\", 10) ? u->origin : "", 0);   /* the retired package folder */
    mu_kstr(b, "servedBy", !strcmp(u->state, "superseded") && u->superseded_by >= 0 ? m->list.items[u->superseded_by].name : "", 0);
    mu_knum(b, "builtAt", e->loaded && bd->built > 0 ? (unsigned long long)bd->built : 0, 0);
    mu_kstr(b, "bundle", e->loaded ? bd->hash : "", 0);
    if (e->loaded && bd->file_count) carried = (const char **)calloc(bd->file_count, sizeof *carried);
    for (k = 0; e->loaded && k < bd->file_count; k++) {
        const sh_mb_file *f = &bd->files[k];
        if (mu_suffix(f->request, ".bnk")) sounds++;
        if (mu_suffix(f->request, ".bswf")) movies++;
        if (f->is_module) modules++;
        if (mu_suffix(f->request, ".mega2")) vt++;
        if (!f->stock && carried) carried[ncar++] = f->request;
    }
    mu_puts(b, ",\"counts\":{");
    mu_knum(b, "shadows", (unsigned)u->shadows, 1);
    mu_knum(b, "sounds", sounds, 0);
    mu_knum(b, "movies", movies, 0);
    mu_knum(b, "modules", modules, 0);
    mu_knum(b, "vt", vt, 0);
    mu_puts(b, "},\"requirements\":");
    /* Canonical {"cvars":{...}} from the builder (the only supported key). */
    mu_puts(b, e->loaded && bd->requirements && strstr(bd->requirements, "\"cvars\"") ? bd->requirements : "{\"cvars\":{}}");
    mu_puts(b, ",\"editorRowNames\":[");
    for (i = 0; i < u->row_count; i++) {
        if (i) mu_putn(b, ",", 1);
        mu_str(b, u->row_names[i]);
    }
    mu_puts(b, "],\"carriedPaths\":[");
    if (ncar > 1) qsort(carried, ncar, sizeof *carried, mu_path_sort);
    for (k = 0; k < ncar && k < MU_MAX_CARRIED; k++) {
        if (k) mu_putn(b, ",", 1);
        mu_str(b, carried[k]);
    }
    mu_putn(b, "]", 1);
    free((void *)carried);
    if (rec->claims_read) mu_knum(b, "publishedMaps", mu_published_maps(m, rec, (int)(u - m->entries)), 0);
}

/* One row of the tab. "attention" carries the shown build's first cause; a
 * duplicate id is attention whatever its build's state. */
static void mu_put_row(mu_buf *b, const mu_model *m, const mu_record *rec, const mu_row *r)
{
    const mu_entry *u = &m->entries[r->serving];
    const sh_ms_entry *e = u->e;
    const sh_mb_bundle *bd = &e->bundle;
    int ok = u->admitted && !u->cause[0] && !r->duplicate;
    mu_putn(b, "{", 1);
    mu_kstr(b, "key", e->name, 1);
    mu_kstr(b, "id", e->loaded ? bd->id : "", 0);
    mu_kstr(b, "name", e->loaded && bd->name ? bd->name : "", 0);
    mu_kstr(b, "author", e->loaded && bd->author ? bd->author : "", 0);
    mu_kstr(b, "version", e->loaded ? mu_version(bd->version) : "", 0);
    mu_knum(b, "build", e->loaded ? bd->build : 0, 0);
    mu_knum(b, "size", e->loaded ? bd->total_size : 0, 0);
    mu_knum(b, "files", e->loaded ? bd->file_count : 0, 0);
    /* Of the files, stock pointers: served from the installed game, not stored. */
    mu_knum(b, "pointers", e->loaded ? bd->stock_count : 0, 0);
    mu_kstr(b, "state", ok ? "installed" : "attention", 0);
    mu_kstr(b, "cause", ok ? "" : u->cause, 0);
    mu_knum(b, "otherBuilds", 0, 0);   /* a folder is a row; the field stays for the page */
    mu_kbool(b, "duplicateId", r->duplicate, 0);
    mu_knum(b, "editorRows", (unsigned)u->editor_rows, 0);
    mu_kstr(b, "folder", mu_leaf(e->folder), 0);
    mu_put_row_v3(b, m, rec, u);
    mu_putn(b, "}", 1);
}

/* Whether overrides\ still holds entries to migrate, and whether a migration runs. */
static void mu_put_migrate(mu_buf *b)
{
    const sh_modui_engine *e = mu_engine();
    unsigned entries = 0;
    int pending = e->migrate_pending ? e->migrate_pending(&entries) : 0;
    mu_puts(b, ",\"migrate\":{");
    mu_knum(b, "entries", entries, 1);
    mu_knum(b, "pending", (unsigned)(pending > 0 ? pending : 0), 0);
    mu_kbool(b, "running", e->migrate_busy && e->migrate_busy() == 1, 0);
    mu_putn(b, "}", 1);
}

/* ------------------------------------------------------------------ console lines */

typedef struct mu_out { sh_modui_line_fn fn; void *context; } mu_out;

static void mu_say(const mu_out *o, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    if (!o || !o->fn) return;
    memcpy(line, "MODS: doctor: ", 14);
    va_start(ap, fmt);
    _vsnprintf_s(line + 14, sizeof line - 14, _TRUNCATE, fmt, ap);
    va_end(ap);
    o->fn(o->context, line);
}

/* ------------------------------------------------------------------ shadows and conflicts */

typedef struct mu_path { const char *request; int entry; int shadow; int conflict; } mu_path;

static int mu_path_cmp(const void *a, const void *b)
{
    const mu_path *x = (const mu_path *)a, *y = (const mu_path *)b;
    int c = strcmp(x->request, y->request);
    return c ? c : x->entry - y->entry;
}

/* Every path an admitted bundle shadows, and every admitted path nobody serves
 * (two bundles provide it with different bytes), grouped by path: conflicts
 * first, with the copy that serves and why, then stock shadows, then the
 * grandfathered shadows of converted packages (migrated, or delivered by a map). */
static void mu_print_shadows(const mu_out *o, const mu_model *m)
{
    mu_path *paths = NULL;
    size_t n = 0, cap = 0, i, k, printed = 0, groups = 0;
    unsigned stock = 0, grandfathered = 0, conflicts = 0;
    int pass;
    for (i = 0; i < m->list.count; i++) {
        const mu_entry *u = &m->entries[i];
        if (!u->admitted) continue;
        for (k = 0; k < u->e->bundle.file_count; k++) {
            const sh_mb_file *f = &u->e->bundle.files[k];
            /* Several mods provide the path with different bytes (one of
             * them serves it, possibly composed), or nothing serves it. */
            int conflict = sh_modserve_conflict_info(f->request, NULL) || !sh_modserve_lookup(f->request, NULL);
            if (!f->shadowed && !conflict) continue;
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 64;
                mu_path *np = (mu_path *)realloc(paths, nc * sizeof *np);
                if (!np) { free(paths); mu_say(o, "shadows: out of memory"); return; }
                paths = np;
                cap = nc;
            }
            paths[n].request = f->request;
            paths[n].entry = (int)i;
            paths[n].shadow = f->shadowed;
            paths[n].conflict = conflict;
            n++;
        }
    }
    if (n > 1) qsort(paths, n, sizeof *paths, mu_path_cmp);
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < n;) {
            size_t j = i + 1, q, at = 0;
            int conflict = paths[i].conflict, any_legacy = m->entries[paths[i].entry].legacy, served;
            int any_delivered = m->entries[paths[i].entry].legacy && sh_ms_entry_delivered(&m->list.items[paths[i].entry]);
            char who[700];
            sh_modserve_hit hit;
            while (j < n && !strcmp(paths[j].request, paths[i].request)) {
                conflict |= paths[j].conflict;
                any_legacy |= m->entries[paths[j].entry].legacy;
                any_delivered |= m->entries[paths[j].entry].legacy && sh_ms_entry_delivered(&m->list.items[paths[j].entry]);
                j++;
            }
            if (conflict != (pass == 0)) { i = j; continue; }
            if (conflict) conflicts++;
            else if (any_legacy) grandfathered++;
            else stock++;
            groups++;
            if (printed >= MU_MAX_SHADOW_GROUPS) { i = j; continue; }
            who[0] = 0;
            for (q = i; q < j && at < sizeof who - 1; q++) {
                int w = _snprintf_s(who + at, sizeof who - at, _TRUNCATE, "%s%s", q > i ? ", " : "",
                                    m->list.items[paths[q].entry].name);
                if (w < 0) break;
                at += (size_t)w;
            }
            served = sh_modserve_lookup(paths[i].request, &hit) ? mu_find_hash(m, hit.bundle_hash) : -1;
            if (conflict) {
                sh_modserve_conflict ci;
                int known = sh_modserve_conflict_info(paths[i].request, &ci);
                if (!known || served < 0)
                    mu_say(o, "conflict %s: nothing serves it; from %s", paths[i].request, who);
                else if (ci.withdrawn)
                    mu_say(o, "conflict %s: withdrawn, the game copy serves (%s); from %s", paths[i].request, ci.reason, who);
                else if (ci.composed)
                    mu_say(o, "conflict %s: composed, every provider's list entries are served (%s); from %s",
                           paths[i].request, ci.reason, who);
                else
                    mu_say(o, "conflict %s: %s serves it (%s); from %s", paths[i].request, m->list.items[served].name,
                           ci.reason, who);
            } else
                mu_say(o, "%s %s: by %s%s; served by %s%s", any_legacy ? "grandfathered shadow" : "shadow", paths[i].request,
                       who, !any_legacy ? "" : any_delivered ? " (delivered by a map)" : " (migrated)",
                       served >= 0 ? m->list.items[served].name : "nothing",
                       served >= 0 && hit.is_stock ? " (the installed game's own copy)" : "");
            printed++;
            i = j;
        }
    if (groups > printed) mu_say(o, "%zu more shadow or conflict path(s) not printed", groups - printed);
    mu_say(o, "%u conflict(s), %u shadow(s), %u grandfathered shadow(s)", conflicts, stock, grandfathered);
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
    if (len >= MU_NAME_MAX || n->count >= MU_MAX_PLACED) return;
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
    char version[SH_MB_VERSION_MAX + 1];
    unsigned long long size;
    char *names[MU_MAX_ROW_NAMES];
    int name_count;
    int via_needs;            /* reached only through another bundle's needs */
} mu_closure_bundle;

typedef struct mu_closure {
    mu_closure_bundle *items;
    size_t count, cap;
    int oom;
} mu_closure;

static void mu_closure_visit(void *context, const char *bundle_id, const char *version, const char *bundle_hash,
                             unsigned long long size, const char *name)
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
        strncpy_s(cb->version, sizeof cb->version, version ? version : "", _TRUNCATE);
        cb->size = size;
        cb->via_needs = name == NULL;
    }
    if (!name) return;
    cb->via_needs = 0;
    if (cb->name_count < MU_MAX_ROW_NAMES) {
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
    for (i = 0; i < c->count; i++)
        for (k = 0; k < c->items[i].name_count && k < MU_MAX_ROW_NAMES; k++) free(c->items[i].names[k]);
    free(c->items);
    memset(c, 0, sizeof *c);
}

/* A mod the open map uses: a manifest row, a bundle its placed names resolve
 * to now, or both. */
typedef struct mu_mapmod {
    char hash[SH_MODSERVE_HASH_HEX + 1];
    char id[SH_MB_ID_MAX + 1];
    char version[SH_MB_VERSION_MAX + 1];
    unsigned long long size;
    int in_rows;              /* the current manifest has a row for it */
    int in_closure;           /* the placed names resolve to it now */
    int named;                /* reached by a placed name (not only through another mod's needs) */
    int *uids;                /* the entities that use it */
    size_t uid_count, uid_cap;
    int modules;              /* placed modules (Blueprint instances) that use it */
} mu_mapmod;

/* A (placed name, mod) match of the closure. */
typedef struct mu_hit { char *name; int mod; } mu_hit;

/* The current map's view: manifest rows (its load-time record, less the rows
 * the tab removed since), the bundles its placed names resolve to now, and,
 * with the map's JSON, its entities, wires and references, and its details. */
typedef struct mu_map {
    sh_modmanifest_list rows;
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
    char name[256];           /* the saved map's name, "" when unknown */
    long long saved;          /* the last save, Unix seconds; 0 when unknown */
    const mu_link *link;      /* the record's link of this map's published item, or NULL */
    const mu_claim *claim;    /* that item's claim, or NULL */
} mu_map;

static int mu_mapmod_add(mu_map *mp, const char *hash, const char *id, const char *version, unsigned long long size)
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
    strncpy_s(mp->mods[mp->mod_count].version, sizeof mp->mods[0].version, version ? version : "", _TRUNCATE);
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

static void mu_map_visit(void *context, const char *bundle_id, const char *version, const char *bundle_hash,
                         unsigned long long size, const char *name)
{
    mu_visit *v = (mu_visit *)context;
    mu_map *mp = v->mp;
    int mod;
    mu_closure_visit(v->closure, bundle_id, version, bundle_hash, size, name);
    if (!bundle_hash || mp->oom) return;
    mod = mu_mapmod_add(mp, bundle_hash, bundle_id, version, size);
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

/* Which mods each entity and module uses: an entity whose placed names
 * include one a mod provides (the save closure's own matching), and, for a
 * manifest row the closure does not reach (its mod is not installed), one
 * whose placed names include a name the row recorded. */
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
        for (h = 0; h < mp->mod_count && mod < 0; h++) if (!strcmp(mp->mods[h].hash, row->bundle)) mod = (int)h;
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

/* ---- the saved map's details */

static char g_saves_root[MAX_PATH * 4];   /* "" the default */

#ifdef SH_MODUI_TESTING
void sh_modui_test_set_saves_root(const char *path)
{
    g_saves_root[0] = 0;
    if (path) strncpy_s(g_saves_root, sizeof g_saves_root, path, _TRUNCATE);
}
#endif

static int mu_saves_root(char *out, size_t cap)
{
    PWSTR saved = NULL;
    char *utf8;
    if (g_saves_root[0]) { strncpy_s(out, cap, g_saves_root, _TRUNCATE); return 1; }
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_SavedGames, 0, NULL, &saved)) || !saved) return 0;
    utf8 = sh_mb_utf8(saved);
    CoTaskMemFree(saved);
    if (!utf8) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\id Software\\DOOM\\base\\savegame.user", utf8);
    free(utf8);
    return 1;
}

/* The local save's game.details (savegame.user\<profile>\SNAPMAPS<id>\):
 * its metadata's name and its modified time. 1 when found. */
static int mu_saved_details(const char *local_id, char *name, size_t name_cap, long long *saved)
{
    char root[MAX_PATH * 4], pattern[MAX_PATH * 4 + 8], path[MAX_PATH * 4 + 128];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *text = NULL;
    const char *c;
    size_t i;
    name[0] = 0;
    *saved = 0;
    if (!local_id || !local_id[0] || strlen(local_id) > 40) return 0;
    for (c = local_id; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) return 0;
    if (!mu_saves_root(root, sizeof root)) return 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\SNAPMAPS%s\\game.details", root, fd.cFileName, local_id);
        text = (char *)sh_mb_read_file(path, 1u << 20, NULL);
    } while (!text && FindNextFileA(h, &fd));
    FindClose(h);
    if (!text) return 0;
    /* Lines: checksum=, itemId=, metadata=<JSON>, modified=<epoch ms>. */
    for (c = text; c && *c;) {
        const char *end = strchr(c, '\n');
        size_t len = end ? (size_t)(end - c) : strlen(c);
        if (len > 9 && !strncmp(c, "modified=", 9)) *saved = _strtoi64(c + 9, NULL, 10) / 1000;
        if (len > 9 && !strncmp(c, "metadata=", 9)) {
            mpj *meta = mpj_parse(c + 9, len - 9 - (len > 9 && c[len - 1] == '\r'));
            const mpj *nm = meta ? mpj_get(meta, "name") : NULL;
            if (nm && nm->type == MPJ_STR) mu_copy_str(name, name_cap, nm);
            if (!*saved) *saved = meta ? mpj_int(mpj_get(meta, "lastModified")) / 1000 : 0;
            mpj_free(meta);
        }
        c = end ? end + 1 : NULL;
    }
    for (i = 0; name[i]; i++) if ((unsigned char)name[i] < 0x20) name[i] = ' ';
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
        mu_names_free(&nm);
    } else if (mp->have_mm) {
        mp->names = count;
        if (count && sh_modserve_active()) modserve_bundles_for_names_visit((const char *const *)names, count, mu_map_visit, &visit);
        sh_modmanifest_names_free(names, count);
    }
    if (mp->entities >= 0) {
        size_t i, k;
        for (i = 0; i < mp->rows.count; i++) {
            const sh_modmanifest_row *row = &mp->rows.rows[i];
            int mod = -1;
            /* A row naming an alias of a bundle the closure reached is that mod. */
            for (k = 0; k < mp->mod_count && mod < 0; k++)
                if (mu_same_bundle(m, mp->mods[k].hash, row->bundle)) mod = (int)k;
            if (mod < 0) mod = mu_mapmod_add(mp, row->bundle, row->id, row->version, row->size);
            if (mod >= 0) mp->mods[mod].in_rows = 1;
        }
        if (mp->have_mm) mu_map_attribute(mp);
        if (e->map_info && e->map_info(&mp->info) == 1) {
            mp->have_info = 1;
            (void)mu_saved_details(mp->info.local_id, mp->name, sizeof mp->name, &mp->saved);
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
    mu_closure_free(&mp->closure);
    for (i = 0; i < mp->mod_count; i++) free(mp->mods[i].uids);
    free(mp->mods);
    for (i = 0; i < mp->hit_count; i++) free(mp->hits[i].name);
    free(mp->hits);
    if (mp->have_mm) sh_mm_free(&mp->mm);
    memset(mp, 0, sizeof *mp);
}

/* Whether the open map pins `hash`, directly or through an alias. */
static int mu_map_references(const mu_model *m, const mu_map *mp, const char *hash)
{
    size_t i;
    for (i = 0; i < mp->rows.count; i++) if (mu_same_bundle(m, mp->rows.rows[i].bundle, hash)) return 1;
    for (i = 0; i < mp->closure.count; i++) if (mu_same_bundle(m, mp->closure.items[i].hash, hash)) return 1;
    return 0;
}

/* The mod a map entry's bundle is installed as (entry index), or -1. */
static int mu_mapmod_entry(const mu_model *m, const mu_mapmod *x)
{
    return mu_find_hash(m, x->hash);
}

/* The state a map entry shows: installed, superseded, unindexed, skipped, missing. */
static const char *mu_mapmod_state(const mu_model *m, const mu_mapmod *x)
{
    int st = sh_modserve_bundle_state(x->hash, NULL, 0), at = mu_mapmod_entry(m, x);
    if (st > 0) return at >= 0 && m->entries[at].superseded_paths ? "superseded" : "installed";
    if (st < 0) return "skipped";
    return at >= 0 ? "unindexed" : "missing";
}

/* "<name> #<uniqueId>": the entity's own label, else its editor row's name,
 * else the last part of what it inherits. */
static void mu_entity_label(const mu_model *m, const sh_mm_entity *ent, int uid, char *out, size_t cap)
{
    const char *label = ent && ent->label[0] ? ent->label : NULL;
    if (!label && ent) label = mu_row_name_for(m, ent->inherit);
    if (!label && ent && ent->timeline) label = "Timeline";
    if (!label && ent && ent->inherit[0]) label = strrchr(ent->inherit, '/') ? strrchr(ent->inherit, '/') + 1 : ent->inherit;
    _snprintf_s(out, cap, _TRUNCATE, "%s #%d", label ? label : "Entity", uid);
}

static void mu_ref_name(const mu_model *m, const mu_map *mp, const sh_mm_ref *r, char *out, size_t cap)
{
    char a[300], b[300];
    mu_entity_label(m, sh_mm_find(&mp->mm, r->from_uid), r->from_uid, a, sizeof a);
    if (r->kind == SH_MM_REF_TIMELINE) { strncpy_s(out, cap, a, _TRUNCATE); return; }
    mu_entity_label(m, sh_mm_find(&mp->mm, r->to_uid), r->to_uid, b, sizeof b);
    _snprintf_s(out, cap, _TRUNCATE, "%s to %s", a, b);
}

/* [{kind, name}] of what points at the map entry's entities. */
static size_t mu_put_refs(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    sh_mm_ref *refs = NULL;
    size_t n = mp->have_mm && x->uid_count ? sh_mm_references(&mp->mm, x->uids, x->uid_count, &refs) : 0, i;
    if (n == (size_t)-1) n = 0;
    mu_putn(b, "[", 1);
    for (i = 0; i < n && i < MU_MAX_MAP_REFS; i++) {
        char name[700];
        mu_ref_name(m, mp, &refs[i], name, sizeof name);
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "kind", refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", 1);
        mu_kstr(b, "name", name, 0);
        mu_putn(b, "}", 1);
    }
    mu_putn(b, "]", 1);
    free(refs);
    return n;
}

static void mu_put_entity_names(mu_buf *b, const mu_model *m, const mu_map *mp, const mu_mapmod *x)
{
    size_t i;
    mu_putn(b, "[", 1);
    for (i = 0; i < x->uid_count && i < MU_MAX_MAP_ENTITIES; i++) {
        char name[300];
        mu_entity_label(m, sh_mm_find(&mp->mm, x->uids[i]), x->uids[i], name, sizeof name);
        if (i) mu_putn(b, ",", 1);
        mu_str(b, name);
    }
    mu_putn(b, "]", 1);
}

/* The map entries the payload lists: every manifest row and every mod a
 * placed name reaches (one only another mod needs is listed too). */
static size_t mu_map_mod_count(const mu_map *mp) { return mp->mod_count; }

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
        char cause[SH_MB_CAUSE_MAX];
        int st = sh_modserve_bundle_state(r->bundle, cause, sizeof cause);
        int at = mu_find_hash(m, r->bundle), other = at < 0 ? mu_find_id(m, r->id, -1) : -1;
        const char *state = st > 0 ? "active" : st < 0 ? "skipped" : at >= 0 ? "unindexed" : "missing";
        if (st > 0 && at >= 0 && m->entries[at].superseded_paths) state = "superseded";
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "id", r->id, 1);
        mu_kstr(b, "version", mu_version(r->version), 0);
        mu_kstr(b, "bundle", r->bundle, 0);
        mu_knum(b, "size", r->size, 0);
        mu_kstr(b, "state", state, 0);
        mu_kstr(b, "cause", st < 0 ? cause : at >= 0 && st == 0 ? m->entries[at].cause : "", 0);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 0);
        mu_kstr(b, "otherKey", other >= 0 ? m->list.items[other].name : "", 0);
        mu_knum(b, "nameCount", (unsigned long long)r->name_count, 0);
        mu_key(b, "names", 0);
        mu_putn(b, "[", 1);
        for (k = 0; k < r->name_count && k < MU_MAX_ROW_NAMES; k++) {
            if (k) mu_putn(b, ",", 1);
            mu_str(b, r->names[k]);
        }
        mu_puts(b, "]}");
    }
    mu_puts(b, "],\"placed\":[");
    for (i = 0; i < mp->closure.count; i++) {
        const mu_closure_bundle *cb = &mp->closure.items[i];
        int at = mu_find_hash(m, cb->hash), in_manifest = 0;
        for (k = 0; k < mp->rows.count; k++) if (mu_same_bundle(m, mp->rows.rows[k].bundle, cb->hash)) in_manifest = 1;
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "id", cb->id, 1);
        mu_kstr(b, "version", mu_version(cb->version), 0);
        mu_kstr(b, "bundle", cb->hash, 0);
        mu_knum(b, "size", cb->size, 0);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 0);
        mu_kbool(b, "viaNeeds", cb->via_needs, 0);
        mu_kbool(b, "inManifest", in_manifest, 0);
        mu_knum(b, "nameCount", (unsigned long long)cb->name_count, 0);
        mu_key(b, "names", 0);
        mu_putn(b, "[", 1);
        for (k = 0; k < (size_t)cb->name_count && k < MU_MAX_ROW_NAMES; k++) {
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
        mu_kstr(b, "id", mp->have_info ? mp->info.local_id : "", 0);
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
        }
        if (mp->have_info && mp->info.tagged >= 0) mu_kbool(b, "tagged", mp->info.tagged, 0);
        mu_puts(b, ",\"mods\":[");
        for (i = 0; i < mu_map_mod_count(mp); i++) {
            const mu_mapmod *x = &mp->mods[i];
            int at = mu_mapmod_entry(m, x);
            if (i) mu_putn(b, ",", 1);
            mu_putn(b, "{", 1);
            mu_kstr(b, "key", at >= 0 ? m->list.items[at].name : "", 1);
            mu_kstr(b, "id", x->id, 0);
            mu_kstr(b, "version", mu_version(x->version), 0);
            mu_kstr(b, "state", mu_mapmod_state(m, x), 0);
            mu_key(b, "placedEntities", 0);
            mu_put_entity_names(b, m, mp, x);
            mu_knum(b, "placedCount", (unsigned long long)x->uid_count, 0);
            mu_knum(b, "placedModules", (unsigned)x->modules, 0);
            mu_key(b, "references", 0);
            (void)mu_put_refs(b, m, mp, x);
            mu_knum(b, "size", x->size, 0);
            mu_kbool(b, "inManifest", x->in_rows, 0);
            mu_kbool(b, "viaNeeds", x->in_closure && !x->named, 0);
            mu_putn(b, "}", 1);
        }
        mu_putn(b, "]", 1);
        mu_kbool(b, "vanilla", mp->mod_count == 0, 0);
    }
    mu_putn(b, "}", 1);
}

/* ------------------------------------------------------------------ the document */

typedef struct mu_bin mu_bin;
static void mu_put_bin_of(mu_buf *b, const char *root);

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
    mu_knum(&b, "v", 3, 1);
    mu_knum(&b, "gen", (unsigned)g_gen, 0);
    mu_kstr(&b, "root", m.root_ok ? m.root : "", 0);
    mu_kbool(&b, "storeReadable", m.listed, 0);
    mu_puts(&b, ",\"mods\":[");
    for (i = 0; i < m.row_count; i++) {
        if (i) mu_putn(&b, ",", 1);
        mu_put_row(&b, &m, &rec, &m.rows[i]);
    }
    mu_putn(&b, "]", 1);
    mu_put_migrate(&b);
    if (m.root_ok) mu_put_bin_of(&b, m.root);
    mu_put_map(&b, &m, &mp);
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
    case SH_MODS_ACT_VERIFY: return "verify";
    case SH_MODS_ACT_RESCAN: return "rescan";
    case SH_MODS_ACT_PALETTE: return "palette";
    case SH_MODS_ACT_PRUNE_LIST: return "pruneList";
    case SH_MODS_ACT_PRUNE_APPLY: return "pruneApply";
    case SH_MODS_ACT_REMOVE: return "remove";
    case SH_MODS_ACT_MIGRATE: return "migrate";
    case SH_MODS_ACT_REBUILD: return "rebuild";
    case SH_MODS_ACT_UNINSTALL: return "uninstall";
    case SH_MODS_ACT_UNINSTALL_PREVIEW: return "uninstallPreview";
    case SH_MODS_ACT_MAP_REMOVE: return "mapRemoveMod";
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW: return "mapRemoveModPreview";
    case SH_MODS_ACT_REMOVED_EMPTY: return "removedEmpty";
    case SH_MODS_ACT_REMOVED_RESTORE: return "removedRestore";
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

/* The rebuild rule, the serving index, then the refresh of what changed
 * under the running game (*rf filled when non-NULL; state -1: not run). */
static int mu_rescan_refresh(char *note, size_t cap, sh_modserve_refresh *rf)
{
    const sh_modui_engine *e = mu_engine();
    sh_ms_sync_stats sync;
    sh_mb_resolver r;
    mu_stock stock = { 0 };
    int n;
    if (rf) { memset(rf, 0, sizeof *rf); rf->state = -1; }
    if (!e->rescan) { strncpy_s(note, cap, "the serving index cannot be rebuilt from here", _TRUNCATE); return -1; }
    /* The rebuild rule first: a folder whose tree changed is built again. */
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    if (sh_ms_sync(&r, &sync) && (sync.rebuilt || sync.failed))
        mu_log("rescan: %u folder(s) built again, %u refused (%llu ms)", sync.rebuilt, sync.failed, sync.ms);
    n = e->rescan();
    InterlockedIncrement(&g_gen);
    if (n < 0) strncpy_s(note, cap, "the serving index could not be rebuilt; try Rescan", _TRUNCATE);
    else _snprintf_s(note, cap, _TRUNCATE, "%d mod(s) active", n);
    if (n >= 0 && e->refresh) {
        sh_modserve_refresh local;
        sh_modserve_refresh *out = rf ? rf : &local;
        memset(out, 0, sizeof *out);
        out->state = e->refresh(out);
    }
    return n;
}

static int mu_rescan(char *note, size_t cap) { return mu_rescan_refresh(note, cap, NULL); }

/* "refresh":{reparsed, reloaded, needsMapReload, needsRestart, note} plus
 * what is still pending and whether it was deferred. */
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
    mu_kstr(b, "note", rf->note, 0);
    mu_putn(b, "}", 1);
}

static int mu_install(const char *folder)
{
    sh_mb_resolver r;
    sh_ms_install_stats st;
    sh_mb_bundle bd;
    mu_stock stock = { 0 };
    char key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX], msg[SH_MB_CAUSE_MAX + 256], note[160];
    int ok, rows = 0, absolute;
    size_t k;
    mu_buf b;
    memset(&b, 0, sizeof b);
    absolute = folder && ((folder[0] && folder[1] == ':') || (folder[0] == '\\' && folder[1] == '\\'));
    if (!absolute) return mu_simple_result(SH_MODS_ACT_INSTALL, 0, "Choose a mod folder to install.");
    mu_log("install %s: building", folder);
    memset(&st, 0, sizeof st);
    key[0] = cause[0] = note[0] = 0;
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    ok = sh_ms_install_from_folder(folder, &r, key, sizeof key, &st, &bd, cause, sizeof cause);
    for (k = 0; k < bd.file_count; k++)
        if (bd.files[k].ptype && !_stricmp(bd.files[k].ptype, "snapEditorEntityDef")) rows++;
    if (!ok) {
        mu_log("install %s: refused: %s", folder, cause);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Not installed: %s", cause[0] ? cause : "the mod could not be built");
    } else {
        mu_log("install %s: installed and verified as %s (bundle %s)", folder, key, bd.hash);
        mu_rescan(note, sizeof note);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Installed %s%s.", key,
                    rows ? "; its editor rows appear in the palette after the game restarts" : "");
    }
    mu_result_head(&b, SH_MODS_ACT_INSTALL, ok, msg);
    mu_kstr(&b, "key", ok ? key : "", 0);
    mu_kstr(&b, "id", bd.id, 0);
    mu_kstr(&b, "bundle", ok ? bd.hash : "", 0);
    mu_kstr(&b, "cause", ok ? "" : cause, 0);
    mu_knum(&b, "files", bd.file_count, 0);
    mu_knum(&b, "size", bd.total_size, 0);
    mu_knum(&b, "editorRows", (unsigned)rows, 0);
    mu_knum(&b, "warnings", bd.warnings, 0);
    mu_kstr(&b, "firstWarning", bd.first_warning, 0);
    mu_knum(&b, "unresolved", bd.needs.unresolved, 0);
    mu_kbool(&b, "replaced", st.replaced, 0);
    mu_kbool(&b, "inPlace", st.in_place, 0);
    mu_kbool(&b, "stockUnchecked", stock.unavailable, 0);
    mu_kstr(&b, "index", note, 0);
    sh_mb_free(&bd);
    return mu_result_tail(&b);
}

/* ---- verify */

/* Rehash `key` (id@version+build, or id@version for every build of it), or
 * every loaded build when key is empty, adding the results up. Each failure
 * is also printed to `o` when given. Returns 0 when the store cannot be read. */
static int mu_verify_core(const char *key, const mu_out *o, sh_ms_verify_stats *sum, unsigned *passed,
                          unsigned *failed, char *first, size_t first_cap)
{
    sh_ms_list list;
    sh_ms_verify_stats st;
    char cause[SH_MB_CAUSE_MAX];
    size_t i;
    memset(sum, 0, sizeof *sum);
    *passed = *failed = 0;
    first[0] = 0;
    if (key && key[0]) {
        int ok;
        memset(&st, 0, sizeof st);
        ok = sh_ms_verify(key, &st, cause, sizeof cause);
        *sum = st;
        if (ok) *passed = st.entries ? st.entries : 1;
        else {
            *failed = 1;
            _snprintf_s(first, first_cap, _TRUNCATE, "%s: %s", key, cause);
            mu_say(o, "verify %s: FAILED: %s", key, cause);
        }
        mu_log("verify %s: %s%s%s", key, ok ? "pass" : "FAILED", ok ? "" : ": ", ok ? "" : cause);
        return 1;
    }
    if (!sh_ms_enumerate(&list)) return 0;
    for (i = 0; i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        int ok;
        if (!e->loaded) continue;
        memset(&st, 0, sizeof st);
        ok = sh_ms_verify(e->name, &st, cause, sizeof cause);
        sum->files += st.files;
        sum->bytes_mb += st.bytes_mb;
        sum->pointers += st.pointers;
        sum->entries += st.entries;
        if (ok) (*passed)++;
        else {
            (*failed)++;
            if (!first[0]) _snprintf_s(first, first_cap, _TRUNCATE, "%s: %s", e->name, cause);
            mu_say(o, "verify %s: FAILED: %s", e->name, cause);
        }
        mu_log("verify %s: %s%s%s", e->name, ok ? "pass" : "FAILED", ok ? "" : ": ", ok ? "" : cause);
    }
    sh_ms_list_free(&list);
    return 1;
}

static void mu_verify_message(unsigned passed, unsigned failed, const char *first, char *msg, size_t cap)
{
    if (!passed && !failed) _snprintf_s(msg, cap, _TRUNCATE, "No installed mod to verify.");
    else if (!failed) _snprintf_s(msg, cap, _TRUNCATE, "Verified %u build(s): every file matches.", passed);
    else _snprintf_s(msg, cap, _TRUNCATE, "%u of %u build(s) failed verification. First: %s", failed, passed + failed, first);
}

static int mu_verify(const char *key)
{
    sh_ms_verify_stats sum;
    char first[SH_MB_CAUSE_MAX + SH_MS_KEY_MAX + 4], msg[SH_MB_CAUSE_MAX + SH_MS_KEY_MAX + 128];
    unsigned passed = 0, failed = 0;
    mu_buf b;
    memset(&b, 0, sizeof b);
    if (!mu_verify_core(key, NULL, &sum, &passed, &failed, first, sizeof first))
        return mu_simple_result(SH_MODS_ACT_VERIFY, 0, "The mods folder cannot be read.");
    InterlockedIncrement(&g_gen);
    mu_verify_message(passed, failed, first, msg, sizeof msg);
    mu_result_head(&b, SH_MODS_ACT_VERIFY, failed == 0 && passed > 0, msg);
    mu_knum(&b, "passed", passed, 0);
    mu_knum(&b, "failed", failed, 0);
    mu_knum(&b, "filesChecked", sum.files, 0);
    mu_knum(&b, "mib", sum.bytes_mb, 0);
    return mu_result_tail(&b);
}

/* ---- removal of covered builds */

typedef struct mu_candidate { int entry; int newer; unsigned long long freed; } mu_candidate;

/* 1 when every request path of `a` is also a path of `b`. */
static int mu_covers(const sh_mb_bundle *b, const sh_mb_bundle *a)
{
    size_t i, k;
    for (i = 0; i < a->file_count; i++) {
        int found = 0;
        for (k = 0; k < b->file_count && !found; k++) found = !strcmp(a->files[i].request, b->files[k].request);
        if (!found) return 0;
    }
    return 1;
}

/* A build may be removed when it is a folder the product made (a download or
 * an installed copy, not delivered by a map), a newer build of the same mod
 * provides every one of its paths, and neither the open map's manifest nor
 * its placed names reference it. Builds of a mod nothing newer covers, skipped
 * builds, the author's own folders and delivered packages are never candidates. */
static size_t mu_candidates(const mu_model *m, const mu_map *mp, mu_candidate *out)
{
    size_t i, k, n = 0;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        int newer = -1;
        if (!e->loaded || e->delivered || !sh_ms_entry_usable(e)) continue;
        if (!mu_product_folder(e)) continue;
        if (mu_map_references(m, mp, e->bundle.hash)) continue;
        for (k = 0; k < m->list.count; k++) {
            const sh_ms_entry *o = &m->list.items[k];
            if (k == i || !sh_ms_entry_usable(o) || strcmp(o->bundle.id, e->bundle.id)) continue;
            if (!mu_newer(o, e) || !mu_covers(&o->bundle, &e->bundle)) continue;
            if (newer < 0 || mu_newer(o, &m->list.items[newer])) newer = (int)k;
        }
        if (newer < 0) continue;
        out[n].entry = (int)i;
        out[n].newer = newer;
        out[n].freed = 0;
        n++;
    }
    return n;
}

static void mu_put_candidates(mu_buf *b, const mu_model *m, const mu_candidate *c, size_t n)
{
    size_t i;
    mu_puts(b, ",\"candidates\":[");
    for (i = 0; i < n; i++) {
        const sh_ms_entry *e = &m->list.items[c[i].entry];
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "key", e->name, 1);
        mu_kstr(b, "bundle", e->bundle.hash, 0);
        mu_knum(b, "size", e->bundle.total_size, 0);
        mu_knum(b, "files", e->bundle.file_count, 0);
        mu_kstr(b, "coveredBy", m->list.items[c[i].newer].name, 0);
        mu_putn(b, "}", 1);
    }
    mu_putn(b, "]", 1);
}

#ifdef _WIN32
/* Remove a tree without following links: a junction or symbolic link inside
 * it is removed as a link. Read-only attributes are cleared first. */
static int mu_remove_tree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    size_t dl = wcslen(dir);
    wchar_t *pattern = (wchar_t *)malloc((dl + 3) * sizeof(wchar_t));
    HANDLE h;
    int ok = 1;
    if (!pattern) return 0;
    memcpy(pattern, dir, dl * sizeof(wchar_t));
    memcpy(pattern + dl, L"\\*", 3 * sizeof(wchar_t));
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t cl;
            wchar_t *child;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            cl = wcslen(fd.cFileName);
            child = (wchar_t *)malloc((dl + cl + 2) * sizeof(wchar_t));
            if (!child) { ok = 0; continue; }
            memcpy(child, dir, dl * sizeof(wchar_t));
            child[dl] = L'\\';
            memcpy(child + dl + 1, fd.cFileName, (cl + 1) * sizeof(wchar_t));
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ok = RemoveDirectoryW(child) && ok;
                else ok = mu_remove_tree(child) && ok;
            } else ok = DeleteFileW(child) && ok;
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir) && ok;
}
#endif

static int mu_key_listed(const char *keys, const char *key)
{
    const char *p = keys;
    size_t kl = strlen(key);
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if (len == kl && !strncmp(p, key, kl)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

/* What one removal did. */
typedef struct mu_removal {
    size_t removed, failed, blobs, kept;
    unsigned long long freed;
    char first_fail[SH_MS_KEY_MAX + 80];
    char kept_at[MAX_PATH * 4];  /* where the last folder went (in removed-mods, absolute) */
    char moved[SH_MS_NAME_MAX + 1];   /* its name in removed-mods */
    char note[160];              /* the rescan's note */
} mu_removal;

static const char *mu_entry_title(const sh_ms_entry *s);

/* removed-mods beside the store folder (<root>\..\removed-mods). */
static int mu_removed_dir(const char *root, char *out, size_t cap)
{
    const char *a = strrchr(root, '\\'), *b = strrchr(root, '/');
    const char *s = a > b ? a : b;
    if (!s || s == root) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%.*s\\removed-mods", (int)(s - root), root);
    return 1;
}

/* ---- the recycle bin: removed-mods\ beside the store, and the record's
 * "removed" section. An uninstalled folder is never deleted: it moves here
 * and the record keeps, under its name here, what brings it back (the bundle
 * hash and the aliases that answered as it, the id and version, the folder's
 * name in mods\, its title, when). A map that still needs one of those
 * bundles reinstates it (mu_restore); emptying the bin deletes the folders. */

#define MU_BIN_HASHES 16

typedef struct mu_bin_item {
    char folder[SH_MS_NAME_MAX + 1];      /* its name in removed-mods\ */
    char from[SH_MS_NAME_MAX + 1];        /* its name in mods\ before */
    char id[SH_MB_ID_MAX + 1];
    char version[SH_MB_VERSION_MAX + 1];
    char name[256];                       /* the title the tab showed */
    char hashes[MU_BIN_HASHES][SH_MB_HASH_HEX + 1];
    int hash_count;
    long long at;                         /* Unix seconds */
    long long seq;                        /* removal order: the newest is the largest */
    unsigned long long bytes;
    int recorded, present;
} mu_bin_item;

struct mu_bin {
    char dir[MAX_PATH * 4];
    mu_bin_item *items;
    size_t count;
    unsigned long long bytes;
    int dir_ok;
};

static void mu_bin_free(mu_bin *b) { free(b->items); memset(b, 0, sizeof *b); }

static mu_bin_item *mu_bin_add(mu_bin *b)
{
    mu_bin_item *n = (mu_bin_item *)realloc(b->items, (b->count + 1) * sizeof *n);
    if (!n) return NULL;
    b->items = n;
    memset(&n[b->count], 0, sizeof *n);
    return &n[b->count++];
}

static mu_bin_item *mu_bin_find(mu_bin *b, const char *folder)
{
    size_t i;
    for (i = 0; i < b->count; i++) if (!_stricmp(b->items[i].folder, folder)) return &b->items[i];
    return NULL;
}

static wchar_t *mu_wchild(const wchar_t *dir, const wchar_t *leaf)
{
    size_t dl = wcslen(dir), cl = wcslen(leaf);
    wchar_t *child = (wchar_t *)malloc((dl + cl + 2) * sizeof(wchar_t));
    if (!child) return NULL;
    memcpy(child, dir, dl * sizeof(wchar_t));
    child[dl] = L'\\';
    memcpy(child + dl + 1, leaf, (cl + 1) * sizeof(wchar_t));
    return child;
}

/* Bytes under dir, links not followed. */
static unsigned long long mu_tree_bytes(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mu_wchild(dir, L"*");
    unsigned long long total = 0;
    HANDLE h;
    if (!pattern) return 0;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t *child;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            child = mu_wchild(dir, fd.cFileName);
            if (child) total += mu_tree_bytes(child);
            free(child);
        } else total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

/* The record's entries into b (folder present or not). Under the record lock. */
static void mu_bin_read_record(const char *root, mu_bin *b)
{
    char *text = NULL;
    mpj *x;
    int i, k;
    x = sh_mb_record_get(root, "removed", &text) == 1 && text ? mpj_parse(text, strlen(text)) : NULL;
    free(text);
    for (i = 0; x && x->type == MPJ_OBJ && i < x->n; i++) {
        const mpj *v = x->items[i], *h = mpj_get(v, "hashes");
        char *key = sh_mm_decode(x->keys[i]);
        mu_bin_item *it;
        if (!key || !key[0] || !v || v->type != MPJ_OBJ || strlen(key) > SH_MS_NAME_MAX || mu_bin_find(b, key) ||
            !(it = mu_bin_add(b))) { free(key); continue; }
        strncpy_s(it->folder, sizeof it->folder, key, _TRUNCATE);
        free(key);
        mu_copy_str(it->from, sizeof it->from, mpj_get(v, "from"));
        mu_copy_str(it->id, sizeof it->id, mpj_get(v, "id"));
        mu_copy_str(it->version, sizeof it->version, mpj_get(v, "version"));
        mu_copy_str(it->name, sizeof it->name, mpj_get(v, "name"));
        it->at = mpj_int(mpj_get(v, "removedAt"));
        it->seq = mpj_int(mpj_get(v, "seq"));
        it->bytes = (unsigned long long)mpj_int(mpj_get(v, "bytes"));
        for (k = 0; h && h->type == MPJ_ARR && k < h->n && it->hash_count < MU_BIN_HASHES; k++)
            if (h->items[k] && h->items[k]->type == MPJ_STR && strlen(h->items[k]->text) == SH_MB_HASH_HEX)
                strcpy_s(it->hashes[it->hash_count++], SH_MB_HASH_HEX + 1, h->items[k]->text);
        it->recorded = 1;
    }
    mpj_free(x);
}

/* -1 unknown, else the number of recorded entries whose folder is present. */
static volatile LONG g_bin_records = -1;

/* Write the section from b's recorded entries whose folder is present (an
 * entry whose folder went is dropped). Under the record lock. */
static int mu_bin_write_record(const char *root, const mu_bin *b)
{
    mu_buf t;
    size_t i, k, n = 0;
    int ok;
    memset(&t, 0, sizeof t);
    mu_putn(&t, "{", 1);
    for (i = 0; i < b->count; i++) {
        const mu_bin_item *it = &b->items[i];
        if (!it->recorded || !it->present) continue;
        if (n++) mu_putn(&t, ",", 1);
        mu_str(&t, it->folder);
        mu_puts(&t, ":{");
        mu_kstr(&t, "id", it->id, 1);
        mu_kstr(&t, "version", it->version, 0);
        mu_kstr(&t, "name", it->name, 0);
        mu_kstr(&t, "from", it->from, 0);
        mu_puts(&t, ",\"hashes\":[");
        for (k = 0; k < (size_t)it->hash_count; k++) { if (k) mu_putn(&t, ",", 1); mu_str(&t, it->hashes[k]); }
        mu_putn(&t, "]", 1);
        mu_knum(&t, "bytes", it->bytes, 0);
        mu_kint(&t, "removedAt", it->at, 0);
        mu_kint(&t, "seq", it->seq, 0);
        mu_putn(&t, "}", 1);
    }
    mu_putn(&t, "}", 1);
    if (t.oom) { free(t.p); return 0; }
    ok = sh_mb_record_put(root, "removed", n ? t.p : NULL);
    free(t.p);
    if (ok) InterlockedExchange(&g_bin_records, (LONG)n);
    return ok;
}

/* What removed-mods\ holds: every folder in it, with what the record says of
 * it, sorted by folder name. 0 when the store root has no parent. */
static int mu_bin_load(const char *root, mu_bin *b)
{
    WIN32_FIND_DATAW fd;
    wchar_t *wd, *pattern;
    HANDLE h;
    size_t i, n = 0;
    memset(b, 0, sizeof *b);
    if (!root || !root[0] || !mu_removed_dir(root, b->dir, sizeof b->dir)) return 0;
    b->dir_ok = 1;
    sh_mb_record_lock();
    mu_bin_read_record(root, b);
    sh_mb_record_unlock();
    wd = sh_mb_wpath(b->dir);
    pattern = wd ? mu_wchild(wd, L"*") : NULL;
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char leaf[SH_MS_NAME_MAX * 4 + 1];
            mu_bin_item *it;
            wchar_t *child;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, leaf, (int)sizeof leaf, NULL, NULL) ||
                strlen(leaf) > SH_MS_NAME_MAX)
                continue;
            it = mu_bin_find(b, leaf);
            if (!it && (it = mu_bin_add(b)) != NULL) strncpy_s(it->folder, sizeof it->folder, leaf, _TRUNCATE);
            if (!it) continue;
            it->present = 1;
            child = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? NULL : mu_wchild(wd, fd.cFileName);
            if (child) it->bytes = mu_tree_bytes(child);
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(wd);
    /* Entries whose folder is gone are neither shown nor restorable. */
    for (i = 0; i < b->count;) {
        if (!b->items[i].present) { b->items[i] = b->items[--b->count]; continue; }
        if (b->items[i].recorded) n++;
        b->bytes += b->items[i].bytes;
        if (!b->items[i].id[0]) strncpy_s(b->items[i].id, sizeof b->items[i].id, b->items[i].folder, _TRUNCATE);
        i++;
    }
    for (i = 1; i < b->count; i++) {   /* a handful: insertion sort by folder */
        mu_bin_item t = b->items[i];
        size_t k = i;
        while (k > 0 && _stricmp(b->items[k - 1].folder, t.folder) > 0) { b->items[k] = b->items[k - 1]; k--; }
        b->items[k] = t;
    }
    InterlockedExchange(&g_bin_records, (LONG)n);
    return 1;
}

/* 1 when a recorded folder could still be needed (cheap after the first
 * read; the save step's gate). */
static int mu_bin_armed(void)
{
    LONG n = InterlockedCompareExchange(&g_bin_records, 0, 0);
    if (n < 0) {
        char root[MAX_PATH * 4];
        mu_bin b;
        if (!sh_ms_root(root, sizeof root)) return 0;
        (void)mu_bin_load(root, &b);
        mu_bin_free(&b);
        n = InterlockedCompareExchange(&g_bin_records, 0, 0);
    }
    return n > 0;
}

/* A free name under dir: `want`, else want-2, want-3, ... */
static int mu_free_name(const char *dir, const char *want, char *out, size_t cap)
{
    int k;
    for (k = 1; k < 1000; k++) {
        char path[MAX_PATH * 4];
        wchar_t *w;
        DWORD at;
        if (k == 1) strncpy_s(out, cap, want, _TRUNCATE);
        else _snprintf_s(out, cap, _TRUNCATE, "%s-%d", want, k);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, out);
        w = sh_mb_wpath(path);
        at = w ? GetFileAttributesW(w) : 0;
        free(w);
        if (at == INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

/* Take the marked entries out of the store, under the store lock. Nothing
 * is deleted: every folder, the product's or the author's own, moves to
 * removed-mods\ (a clash there gets a suffix) and the record's "removed"
 * section keeps what brings it back. An entry whose folder cannot be moved
 * is unmarked and counted as failed. */
static void mu_remove_marked(const mu_model *m, unsigned char *removing, mu_removal *out)
{
    size_t n = m->list.count, i, k;
    char dir[MAX_PATH * 4];
    mu_bin bin;
    wchar_t *wd;
    memset(out, 0, sizeof *out);
    if (!mu_removed_dir(m->root, dir, sizeof dir)) {
        for (i = 0; i < n; i++) if (removing[i]) { removing[i] = 0; out->failed++; }
        strncpy_s(out->first_fail, sizeof out->first_fail, "the store folder has no parent for removed-mods", _TRUNCATE);
        return;
    }
    modstore_lock();
    sh_mb_record_lock();
    (void)mu_bin_load(m->root, &bin);
    wd = sh_mb_wpath(dir);
    if (wd) CreateDirectoryW(wd, NULL);
    free(wd);
    for (i = 0; i < n; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        const char *leaf = mu_leaf(e->folder);
        char dest[SH_MS_NAME_MAX + 1], to_path[MAX_PATH * 4];
        wchar_t *from, *to = NULL;
        int moved;
        if (!removing[i]) continue;
        if (mu_free_name(dir, leaf, dest, sizeof dest)) {
            _snprintf_s(to_path, sizeof to_path, _TRUNCATE, "%s\\%s", dir, dest);
            to = sh_mb_wpath(to_path);
        }
        from = sh_mb_wpath(e->folder);
        moved = from && to && MoveFileExW(from, to, 0);
        if (!moved) {
            out->failed++;
            if (!out->first_fail[0])
                _snprintf_s(out->first_fail, sizeof out->first_fail, _TRUNCATE, "%s could not be moved", e->name);
            removing[i] = 0;
            mu_log("remove %s: the folder could not be moved (error %lu)", e->name, GetLastError());
        } else {
            mu_bin_item *it = mu_bin_add(&bin);
            out->removed++;
            out->kept++;
            out->freed += e->bundle.total_size;
            strncpy_s(out->kept_at, sizeof out->kept_at, to_path, _TRUNCATE);
            strncpy_s(out->moved, sizeof out->moved, dest, _TRUNCATE);
            if (it) {
                strncpy_s(it->folder, sizeof it->folder, dest, _TRUNCATE);
                strncpy_s(it->from, sizeof it->from, leaf, _TRUNCATE);
                strncpy_s(it->id, sizeof it->id, e->loaded && e->bundle.id[0] ? e->bundle.id : leaf, _TRUNCATE);
                if (e->loaded && !sh_mb_version_generated(e->bundle.version))
                    strncpy_s(it->version, sizeof it->version, e->bundle.version, _TRUNCATE);
                strncpy_s(it->name, sizeof it->name, mu_entry_title(e), _TRUNCATE);
                if (e->loaded && e->bundle.hash[0]) {
                    strcpy_s(it->hashes[it->hash_count++], SH_MB_HASH_HEX + 1, e->bundle.hash);
                    for (k = 0; k < m->list.alias_count && it->hash_count < MU_BIN_HASHES; k++)
                        if (!_stricmp(m->list.aliases[k].to, e->bundle.hash))
                            strcpy_s(it->hashes[it->hash_count++], SH_MB_HASH_HEX + 1, m->list.aliases[k].from);
                }
                it->at = (long long)time(NULL);
                {
                    size_t q;
                    for (q = 0; q + 1 < bin.count; q++) if (bin.items[q].seq >= it->seq) it->seq = bin.items[q].seq + 1;
                }
                it->bytes = mu_tree_bytes(to);
                it->recorded = it->present = 1;
            }
            mu_log("remove %s: moved to removed-mods\\%s (kept until the removed mods are emptied)", e->name, dest);
        }
        free(from);
        free(to);
    }
    if (out->removed && !mu_bin_write_record(m->root, &bin))
        mu_log("remove: the record's removed section could not be written; the moved folders stay in removed-mods");
    sh_mb_record_unlock();
    mu_bin_free(&bin);
    /* Aliases naming a removed build stay while a folder of its id remains
     * (a map pinning it loads that folder's build); else they are dropped,
     * and the removed section keeps them for a reinstatement. */
    for (i = 0; i < n; i++)
        if (removing[i] && m->list.items[i].loaded)
            sh_ms_alias_forget(m->list.items[i].bundle.id, m->list.items[i].bundle.hash);
    if (out->removed) mu_rescan(out->note, sizeof out->note);
    modstore_unlock();
}

static void mu_put_removed(mu_buf *b, const mu_model *m, const unsigned char *removing, const mu_removal *rm)
{
    size_t i, k;
    mu_puts(b, ",\"removed\":[");
    for (i = 0, k = 0; i < m->list.count; i++) {
        if (!removing[i]) continue;
        if (k++) mu_putn(b, ",", 1);
        mu_str(b, m->list.items[i].name);
    }
    mu_putn(b, "]", 1);
    mu_knum(b, "bytes", rm->freed, 0);
    mu_knum(b, "blobs", (unsigned long long)rm->blobs, 0);
    mu_kstr(b, "movedTo", rm->kept_at, 0);
    mu_kstr(b, "index", rm->note, 0);
}

static int mu_prune(int apply, const char *keys)
{
    mu_model m;
    mu_map mp;
    mu_candidate *c;
    unsigned char *removing;
    size_t n, i;
    char msg[512];
    mu_buf b;
    memset(&b, 0, sizeof b);
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_map_build(&mp, &m, NULL);
    c = (mu_candidate *)calloc(m.list.count ? m.list.count : 1, sizeof *c);
    removing = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
    if (!c || !removing) {
        free(c); free(removing);
        mu_map_free(&mp); mu_model_free(&m);
        return -1;
    }
    n = mu_candidates(&m, &mp, c);
    if (!apply) {
        unsigned long long total = 0;
        for (i = 0; i < n; i++) total += m.list.items[c[i].entry].bundle.total_size;
        if (n) _snprintf_s(msg, sizeof msg, _TRUNCATE, "%zu build(s) can be removed; a newer build of the same mod covers each one.", n);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "No build can be removed: every installed build is the newest of its mod, is in use by the open map, is your own folder, or was delivered by a map.");
        mu_result_head(&b, SH_MODS_ACT_PRUNE_LIST, 1, msg);
        mu_put_candidates(&b, &m, c, n);
        mu_knum(&b, "bytes", total, 0);
    } else {
        mu_removal rm;
        /* Only the keys the person confirmed, and only while still candidates. */
        for (i = 0; i < n; i++) {
            const sh_ms_entry *e = &m.list.items[c[i].entry];
            if (keys && mu_key_listed(keys, e->name)) removing[c[i].entry] = 1;
        }
        mu_remove_marked(&m, removing, &rm);
        if (!rm.removed && !rm.failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Nothing removed: none of the chosen builds can be removed now.");
        else if (!rm.failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %zu build(s).", rm.removed);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %zu build(s); %zu could not be removed (%s).", rm.removed, rm.failed, rm.first_fail);
        mu_log("remove: %zu build(s) removed, %zu failed", rm.removed, rm.failed);
        mu_result_head(&b, SH_MODS_ACT_PRUNE_APPLY, rm.failed == 0 && rm.removed > 0, msg);
        mu_put_removed(&b, &m, removing, &rm);
    }
    free(c);
    free(removing);
    mu_map_free(&mp);
    mu_model_free(&m);
    return mu_result_tail(&b);
}

/* ---- removal of one listed mod */

/* Every build in the row that `key` belongs to, unless the open map uses one. */
static int mu_remove(const char *key)
{
    mu_model m;
    mu_map mp;
    unsigned char *removing;
    mu_removal rm;
    int row = -1, used = 0, ok;
    size_t i;
    const char *shown;
    char msg[MAX_PATH * 4 + 512];
    mu_buf b;
    memset(&b, 0, sizeof b);
    memset(&rm, 0, sizeof rm);
    if (!key || !key[0]) return mu_simple_result(SH_MODS_ACT_REMOVE, 0, "Select a mod to remove.");
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    for (i = 0; i < m.list.count && row < 0; i++)
        if (m.entries[i].row >= 0 && !strcmp(m.list.items[i].name, key)) row = m.entries[i].row;
    if (row < 0) { mu_model_free(&m); return mu_simple_result(SH_MODS_ACT_REMOVE, 0, "That mod is no longer installed."); }
    removing = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
    if (!removing) { mu_model_free(&m); return -1; }
    mu_map_build(&mp, &m, NULL);
    {
        const sh_ms_entry *s = &m.list.items[m.rows[row].serving];
        shown = s->loaded && s->bundle.name && s->bundle.name[0] ? s->bundle.name : s->loaded && s->bundle.id[0] ? s->bundle.id : s->name;
    }
    for (i = 0; i < m.list.count; i++) {
        if (m.entries[i].row != row) continue;
        removing[i] = 1;
        if (m.list.items[i].loaded && mu_map_references(&m, &mp, m.list.items[i].bundle.hash)) used = 1;
    }
    if (used) {
        for (i = 0; i < m.list.count; i++) removing[i] = 0;
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s is used by the open map. Delete its entities and save the map, or open "
                    "another map, then remove it.", shown);
        mu_log("remove %s: refused, the open map uses it", key);
        ok = 0;
    } else {
        mu_remove_marked(&m, removing, &rm);
        mu_log("remove %s: %zu folder(s) removed, %zu failed", key, rm.removed, rm.failed);
        ok = rm.removed > 0 && rm.failed == 0;
        if (ok) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %s. Its folder was moved to %s.", shown, rm.kept_at);
        else if (rm.removed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %zu of %zu builds of %s; %s.", rm.removed,
                                         rm.removed + rm.failed, shown, rm.first_fail);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s could not be removed: %s. Close any program using its folder and try again.",
                         shown, rm.first_fail[0] ? rm.first_fail : "nothing was moved");
    }
    mu_result_head(&b, SH_MODS_ACT_REMOVE, ok, msg);
    mu_kbool(&b, "inUse", used, 0);
    mu_put_removed(&b, &m, removing, &rm);
    free(removing);
    mu_map_free(&mp);
    mu_model_free(&m);
    return mu_result_tail(&b);
}

/* ---- the open map: removal of one mod, and uninstall */

/* One console line, "MODS: " and the text (the console verbs share these
 * actions with the tab). */
static void mu_line(const mu_out *o, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    if (!o || !o->fn) return;
    memcpy(line, "MODS: ", 6);
    va_start(ap, fmt);
    _vsnprintf_s(line + 6, sizeof line - 6, _TRUNCATE, fmt, ap);
    va_end(ap);
    o->fn(o->context, line);
}

/* 1 with the cause when the game cannot take a change now. */
static int mu_busy(char *cause, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    cause[0] = 0;
    return e->busy && e->busy(cause, cap) == 1;
}

/* "12.3 MB", as the page writes sizes. */
static void mu_size_text(unsigned long long n, char *out, size_t cap)
{
    if (n < 1024) _snprintf_s(out, cap, _TRUNCATE, "%llu B", n);
    else if (n < 1024ull * 1024) _snprintf_s(out, cap, _TRUNCATE, n < 10240 ? "%.1f KB" : "%.0f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) _snprintf_s(out, cap, _TRUNCATE, n < 10485760 ? "%.1f MB" : "%.0f MB", n / 1048576.0);
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f GB", n / 1073741824.0);
}

/* The rebuild rule and the serving index, without the game-side refresh. */
static int mu_rescan_index(char *note, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    sh_ms_sync_stats sync;
    sh_mb_resolver r;
    mu_stock stock = { 0 };
    int n;
    if (!e->rescan) { strncpy_s(note, cap, "the serving index cannot be rebuilt from here", _TRUNCATE); return -1; }
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    if (sh_ms_sync(&r, &sync) && (sync.rebuilt || sync.failed))
        mu_log("rescan: %u folder(s) built again, %u refused (%llu ms)", sync.rebuilt, sync.failed, sync.ms);
    n = e->rescan();
    InterlockedIncrement(&g_gen);
    if (n < 0) strncpy_s(note, cap, "the serving index could not be rebuilt; try Rescan", _TRUNCATE);
    else _snprintf_s(note, cap, _TRUNCATE, "%d mod(s) active", n);
    return n;
}

/* ---- automatic reinstatement */

/* What the next main-thread tick owes after a reinstatement made where the
 * game could not take it (a load, a save, a Play entry): the game-side
 * refresh, then one toast and one console line naming the mods. */
static volatile LONG g_owed_refresh;
static char g_announce[600];
static int g_announce_count;
static SRWLOCK g_announce_lock = SRWLOCK_INIT;

static void mu_announce_add(const char *title)
{
    size_t at;
    AcquireSRWLockExclusive(&g_announce_lock);
    at = strlen(g_announce);
    if (at + strlen(title) + 8 < sizeof g_announce)
        _snprintf_s(g_announce + at, sizeof g_announce - at, _TRUNCATE, "%s%s", at ? "\n" : "", title);
    g_announce_count++;
    ReleaseSRWLockExclusive(&g_announce_lock);
}

/* "Hell Guards was reinstalled because the map still uses it." for one,
 * "A, B and C were reinstalled because the map still uses them." for more. */
static int mu_announce_take(char *out, size_t cap)
{
    char titles[sizeof g_announce], *parts[16];
    int n = 0, i;
    size_t at = 0;
    char *p, *ctx = NULL;
    AcquireSRWLockExclusive(&g_announce_lock);
    strcpy_s(titles, sizeof titles, g_announce);
    g_announce[0] = 0;
    g_announce_count = 0;
    ReleaseSRWLockExclusive(&g_announce_lock);
    if (!titles[0]) return 0;
    for (p = strtok_s(titles, "\n", &ctx); p && n < 16; p = strtok_s(NULL, "\n", &ctx)) parts[n++] = p;
    out[0] = 0;
    for (i = 0; i < n; i++) {
        int w = _snprintf_s(out + at, cap - at, _TRUNCATE, "%s%s", i == 0 ? "" : i == n - 1 ? " and " : ", ", parts[i]);
        if (w < 0) break;
        at += (size_t)w;
    }
    _snprintf_s(out + at, cap - at, _TRUNCATE, n == 1 ? " was reinstalled because the map still uses it."
                                                      : " were reinstalled because the map still uses them.");
    return 1;
}

/* Undo can bring back entities of a mod Uninstall took out of the open map.
 * Their ids are watched with the bundle hash they came from: one coming back
 * reinstates that bundle (whatever its names look like); a watched id with
 * no hash makes the tick read the map's placed names. */
#define MU_WATCH_MAX 256
static int g_watch_uid[MU_WATCH_MAX];
static unsigned char g_watch_on[MU_WATCH_MAX];
static char g_watch_hash[MU_WATCH_MAX][SH_MB_HASH_HEX + 1];
static int g_watch_n;
static ULONGLONG g_poll_next;

static void mu_watch_add(const int *uids, size_t n, const char *hash)
{
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < g_watch_n && g_watch_uid[k] != uids[i]; k++) { }
        if (k >= g_watch_n) {
            if (g_watch_n >= MU_WATCH_MAX) break;
            g_watch_uid[g_watch_n++] = uids[i];
        }
        g_watch_on[k] = 0;
        strncpy_s(g_watch_hash[k], sizeof g_watch_hash[k], hash ? hash : "", _TRUNCATE);
    }
}

/* The id a placed name lives under (mods/<id>/...), lowercase, or "". */
static void mu_name_id(const char *name, char *out, size_t cap)
{
    const char *p = name, *end;
    size_t n;
    out[0] = 0;
    for (;;) {
        if (!_strnicmp(p, "mods/", 5)) break;
        p = strchr(p, '/');
        if (!p) return;
        p++;
    }
    p += 5;
    end = strchr(p, '/');
    if (!end) return;
    n = (size_t)(end - p);
    if (!n || n >= cap) return;
    memcpy(out, p, n);
    out[n] = 0;
    _strlwr_s(out, cap);
}

/* A namespace segment names `id` as spelled, or as the engine spells binary
 * names ('-' becomes '_'). */
static int mu_seg_is_id(const char *seg, const char *id)
{
    size_t i;
    if (!_stricmp(seg, id)) return 1;
    if (strlen(seg) != strlen(id)) return 0;
    for (i = 0; id[i]; i++) {
        char a = (char)tolower((unsigned char)seg[i]), b = (char)tolower((unsigned char)id[i]);
        if (a != b && !(a == '_' && b == '-')) return 0;
    }
    return 1;
}

static int mu_id_installed(const sh_ms_list *l, const char *id)
{
    size_t i;
    for (i = 0; i < l->count; i++)
        if (l->items[i].loaded && !_stricmp(l->items[i].bundle.id, id)) return 1;
    return 0;
}

/* Move one folder of the bin back into mods\: its old name, else its id,
 * else a free name from the id. The aliases it held are recorded again.
 * Under the store and record locks. 1 moved (`as` names it). */
static int mu_bin_restore_one(const char *root, mu_bin *bin, mu_bin_item *it, char *as, size_t cap)
{
    char from[MAX_PATH * 4], to[MAX_PATH * 4];
    wchar_t *wf, *wt;
    int ok, k;
    const char *want = it->from[0] ? it->from : it->id;
    _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", root, want);
    wt = sh_mb_wpath(to);
    if (!wt || GetFileAttributesW(wt) != INVALID_FILE_ATTRIBUTES) {
        free(wt);
        if (!mu_free_name(root, it->id[0] ? it->id : it->folder, as, cap)) return 0;
        _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", root, as);
        wt = sh_mb_wpath(to);
    } else strncpy_s(as, cap, want, _TRUNCATE);
    _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\%s", bin->dir, it->folder);
    wf = sh_mb_wpath(from);
    ok = wf && wt && MoveFileExW(wf, wt, 0);
    if (!ok) mu_log("restore %s: removed-mods\\%s could not be moved back (error %lu)", it->id, it->folder, GetLastError());
    free(wf);
    free(wt);
    if (!ok) return 0;
    for (k = 1; k < it->hash_count; k++) sh_ms_alias_note(it->hashes[k], it->hashes[0]);
    it->present = 0;   /* its record entry goes with the next write */
    return 1;
}

/* The restore hook (modserve.h sh_modserve_restore_fn): reinstates every
 * recycled mod a needed bundle hash or a placed name asks for. Hashes match
 * a recorded hash that resolves to nothing installed; a placed name under
 * mods/<id>/ matches the newest recorded folder of an id no installed folder
 * declares. With neither, the open map's placed names are read. */
static int mu_restore(const char *const *hashes, int hash_count, const char *const *names, size_t name_count, int when)
{
    static const char *const WHEN[] = { "", "map load", "save", "Play entry", "undo" };
    const sh_modui_engine *e = mu_engine();
    sh_ms_list list;
    mu_bin bin;
    mu_names placed;
    unsigned char *want = NULL;
    char root[MAX_PATH * 4], note[160];
    size_t i, k;
    int restored = 0, h;
    if (!mu_bin_armed() || !mu_on_main() || !sh_ms_root(root, sizeof root)) return 0;
    memset(&placed, 0, sizeof placed);
    if (!hashes && !names) {
        if (!e->placed_names || e->placed_names(mu_names_add, &placed) < 0) { mu_names_free(&placed); return 0; }
        names = (const char *const *)placed.items;
        name_count = placed.count;
    }
    modstore_lock();
    sh_mb_record_lock();
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    if (mu_bin_load(root, &bin) && bin.count) want = (unsigned char *)calloc(bin.count, 1);
    for (h = 0; want && hashes && h < hash_count; h++) {
        if (!hashes[h] || strlen(hashes[h]) != SH_MB_HASH_HEX || sh_ms_resolve(&list, hashes[h])) continue;
        for (i = 0; i < bin.count; i++)
            for (k = 0; bin.items[i].recorded && k < (size_t)bin.items[i].hash_count; k++)
                if (!_stricmp(bin.items[i].hashes[k], hashes[h])) want[i] = 1;
    }
    for (k = 0; want && names && k < name_count; k++) {
        char seg[SH_MB_ID_MAX + 1];
        size_t best = (size_t)-1;
        if (!names[k]) continue;
        mu_name_id(names[k], seg, sizeof seg);
        if (!seg[0]) continue;
        for (i = 0; i < bin.count; i++) {
            const mu_bin_item *it = &bin.items[i];
            if (!it->recorded || !mu_seg_is_id(seg, it->id) || mu_id_installed(&list, it->id)) continue;
            if (best == (size_t)-1 || it->seq > bin.items[best].seq) best = i;
        }
        if (best != (size_t)-1) want[best] = 1;
    }
    for (i = 0; want && i < bin.count; i++) {
        char as[SH_MS_NAME_MAX + 1];
        mu_bin_item *it = &bin.items[i];
        if (!want[i] || !mu_bin_restore_one(root, &bin, it, as, sizeof as)) continue;
        restored++;
        mu_log("restore %s@%s: the %s found the map still uses it; removed-mods\\%s moved back to mods\\%s", it->id,
               it->version, WHEN[when >= 0 && when <= 4 ? when : 0], it->folder, as);
        mu_announce_add(it->name[0] ? it->name : it->id);
    }
    if (restored) (void)mu_bin_write_record(root, &bin);
    sh_mb_record_unlock();
    if (restored) {
        note[0] = 0;
        /* After an undo the tick is the editor's own: everything at once.
         * Elsewhere only the index, so the load or save in progress sees the
         * mod; the game-side refresh follows on the next editor tick. */
        if (when == SH_MODSERVE_RESTORE_UNDO) (void)mu_rescan(note, sizeof note);
        else { (void)mu_rescan_index(note, sizeof note); InterlockedExchange(&g_owed_refresh, 1); }
        mu_log("restore: %d mod(s) reinstated (%s)", restored, note);
    }
    modstore_unlock();
    free(want);
    mu_bin_free(&bin);
    sh_ms_list_free(&list);
    mu_names_free(&placed);
    return restored;
}

int sh_modui_poll_due(void)
{
    ULONGLONG now;
    if (!InterlockedCompareExchange(&g_owed_refresh, 0, 0) && !g_watch_n && !g_announce[0]) return 0;
    now = GetTickCount64();
    if (now < g_poll_next) return 0;
    g_poll_next = now + 250;
    return 1;
}

int sh_modui_poll_main(void *context)
{
    const sh_modui_engine *e = mu_engine();
    sh_modui_map_info info;
    char cause[200], text[700];
    int i, back = 0;
    (void)context;
    if (!mu_on_main() || mu_busy(cause, sizeof cause)) return 0;   /* Play or a load: later */
    if (!e->map_info || e->map_info(&info) != 1) return 0;          /* the editor, with a map */
    if (InterlockedExchange(&g_owed_refresh, 0) && e->refresh) {
        sh_modserve_refresh rf;
        memset(&rf, 0, sizeof rf);
        (void)e->refresh(&rf);
        InterlockedIncrement(&g_gen);
    }
    {
        const char *hashes[MU_WATCH_MAX];
        int nh = 0, k, unhashed = 0;
        for (i = 0; e->entity_valid && i < g_watch_n; i++) {
            int on = e->entity_valid(g_watch_uid[i]) == 1;
            if (on && !g_watch_on[i]) {
                back++;
                if (!g_watch_hash[i][0]) unhashed = 1;
                else {
                    for (k = 0; k < nh && strcmp(hashes[k], g_watch_hash[i]); k++) { }
                    if (k == nh) hashes[nh++] = g_watch_hash[i];
                }
            }
            g_watch_on[i] = (unsigned char)on;
        }
        if (back) {
            mu_log("undo watch: %d entit%s Uninstall deleted from the open map %s back", back, back == 1 ? "y" : "ies",
                   back == 1 ? "is" : "are");
            if (nh) (void)mu_restore(hashes, nh, NULL, 0, SH_MODSERVE_RESTORE_UNDO);
            if (unhashed) (void)mu_restore(NULL, 0, NULL, 0, SH_MODSERVE_RESTORE_UNDO);
        }
    }
    if (mu_announce_take(text, sizeof text)) {
        char line[760];
        if (e->toast) e->toast("Mods", text);
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: %s", text);
        if (e->print) e->print(line);
        backend_log(line);
        InterlockedIncrement(&g_gen);
    }
    if (!mu_bin_armed()) g_watch_n = 0;
    return 1;
}

/* ---- the bin's actions: empty it, put one back by hand */

static void mu_put_bin(mu_buf *b, const mu_bin *bin)
{
    size_t i;
    mu_key(b, "removed", 0);
    mu_putn(b, "{", 1);
    mu_kstr(b, "folder", bin->dir, 1);
    mu_knum(b, "count", (unsigned long long)bin->count, 0);
    mu_knum(b, "bytes", bin->bytes, 0);
    mu_puts(b, ",\"items\":[");
    for (i = 0; i < bin->count; i++) {
        const mu_bin_item *it = &bin->items[i];
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "folder", it->folder, 1);
        mu_kstr(b, "id", it->id, 0);
        mu_kstr(b, "version", it->version, 0);
        mu_kstr(b, "name", it->name, 0);
        mu_knum(b, "bytes", it->bytes, 0);
        mu_kint(b, "at", it->at, 0);
        mu_kbool(b, "restorable", it->recorded && it->hash_count > 0, 0);
        mu_putn(b, "}", 1);
    }
    mu_puts(b, "]}");
}

static void mu_put_bin_of(mu_buf *b, const char *root)
{
    mu_bin bin;
    if (mu_bin_load(root, &bin)) mu_put_bin(b, &bin);
    mu_bin_free(&bin);
}

/* Empty the bin: every folder in it (or only the newline-separated `keys`
 * the confirmation listed), and their record entries. apply 0 lists. */
static int mu_bin_empty(int apply, const char *keys, const mu_out *o)
{
    mu_bin bin;
    mu_buf b;
    char root[MAX_PATH * 4], msg[400], size[32];
    size_t i, deleted = 0, failed = 0, chosen = 0;
    unsigned long long freed = 0;
    int ok;
    memset(&b, 0, sizeof b);
    if (!sh_ms_root(root, sizeof root) || !mu_bin_load(root, &bin)) {
        mu_line(o, "removed: the store folder cannot be resolved");
        return mu_simple_result(SH_MODS_ACT_REMOVED_EMPTY, 0, "The removed-mods folder cannot be found.");
    }
    modstore_lock();
    sh_mb_record_lock();
    for (i = 0; i < bin.count; i++) {
        mu_bin_item *it = &bin.items[i];
        char path[MAX_PATH * 4];
        wchar_t *w;
        if (keys && keys[0] && !mu_key_listed(keys, it->folder)) continue;
        chosen++;
        if (!apply) {
            mu_size_text(it->bytes, size, sizeof size);
            mu_line(o, "removed empty: deletes removed-mods\\%s (%s, %s)", it->folder, it->id, size);
            continue;
        }
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", bin.dir, it->folder);
        w = sh_mb_wpath(path);
        if (w && mu_remove_tree(w)) {
            deleted++;
            freed += it->bytes;
            it->present = 0;
            mu_log("removed: deleted removed-mods\\%s (%s)", it->folder, it->id);
        } else {
            failed++;
            if (w && GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) it->present = 0;
            mu_log("removed: removed-mods\\%s could not be deleted completely (error %lu)", it->folder, GetLastError());
        }
        free(w);
    }
    if (apply) (void)mu_bin_write_record(root, &bin);
    sh_mb_record_unlock();
    modstore_unlock();
    mu_size_text(apply ? freed : bin.bytes, size, sizeof size);
    if (!apply) {
        if (chosen) mu_line(o, "removed empty: dry run; add --yes to delete %zu folder(s), %s", chosen, size);
        else mu_line(o, "removed empty: removed-mods holds no folder");
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%zu folder(s) can be deleted.", chosen);
        ok = 1;
    } else if (!chosen) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Nothing to empty: removed-mods holds no folder.");
        ok = 0;
    } else if (!failed) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Deleted %zu removed mod folder(s), %s.", deleted, size);
        ok = 1;
    } else {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Deleted %zu removed mod folder(s), %s; %zu could not be deleted completely. "
                    "Close any program using them and try again.", deleted, size, failed);
        ok = 0;
    }
    if (apply) mu_line(o, "removed empty: %s", msg);
    InterlockedIncrement(&g_gen);
    mu_result_head(&b, SH_MODS_ACT_REMOVED_EMPTY, ok, msg);
    mu_knum(&b, "deleted", (unsigned long long)deleted, 0);
    mu_knum(&b, "failed", (unsigned long long)failed, 0);
    mu_knum(&b, "freed", freed, 0);
    mu_bin_free(&bin);
    return mu_result_tail(&b);
}

/* Put one folder back by hand: `key` is an id (its newest folder in the
 * bin), else a folder name in the bin. Then the full rescan. */
static int mu_bin_restore_key(const char *key, const mu_out *o)
{
    mu_bin bin;
    mu_buf b;
    char root[MAX_PATH * 4], msg[MAX_PATH + 400], cause[200], as[SH_MS_NAME_MAX + 1], note[160];
    mu_bin_item *it = NULL;
    size_t i;
    int ok = 0;
    memset(&b, 0, sizeof b);
    if (!key || !key[0]) {
        mu_line(o, "removed restore: usage: sh_mods removed restore \"<id>\"");
        return mu_simple_result(SH_MODS_ACT_REMOVED_RESTORE, 0, "Name a removed mod to put back.");
    }
    if (mu_busy(cause, sizeof cause)) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Not now: %s.", cause);
        mu_line(o, "removed restore %s: refused: %s", key, cause);
        return mu_simple_result(SH_MODS_ACT_REMOVED_RESTORE, 0, msg);
    }
    if (!sh_ms_root(root, sizeof root) || !mu_bin_load(root, &bin))
        return mu_simple_result(SH_MODS_ACT_REMOVED_RESTORE, 0, "The removed-mods folder cannot be found.");
    modstore_lock();
    sh_mb_record_lock();
    for (i = 0; i < bin.count; i++)
        if (!_stricmp(bin.items[i].id, key) && (!it || bin.items[i].seq > it->seq)) it = &bin.items[i];
    if (!it) it = mu_bin_find(&bin, key);
    if (!it) _snprintf_s(msg, sizeof msg, _TRUNCATE, "removed-mods holds no folder of %s.", key);
    else if (!mu_bin_restore_one(root, &bin, it, as, sizeof as))
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "removed-mods\\%s could not be moved back. Close any program using it and try again.",
                    it->folder);
    else {
        ok = 1;
        (void)mu_bin_write_record(root, &bin);
        mu_log("restore %s: by hand; removed-mods\\%s moved back to mods\\%s", it->id, it->folder, as);
    }
    sh_mb_record_unlock();
    if (ok) {
        note[0] = 0;
        (void)mu_rescan(note, sizeof note);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Reinstalled %s: removed-mods\\%s is mods\\%s again (%s).",
                    it->name[0] ? it->name : it->id, it->folder, as, note);
    }
    modstore_unlock();
    mu_line(o, "removed restore %s: %s", key, msg);
    InterlockedIncrement(&g_gen);
    mu_result_head(&b, SH_MODS_ACT_REMOVED_RESTORE, ok, msg);
    mu_kstr(&b, "key", ok ? as : "", 0);
    mu_bin_free(&bin);
    return mu_result_tail(&b);
}

/* `sh_mods removed`: what the bin holds. */
static void mu_say_bin(const mu_out *o)
{
    mu_bin bin;
    char root[MAX_PATH * 4], size[32];
    size_t i;
    if (!sh_ms_root(root, sizeof root) || !mu_bin_load(root, &bin)) { mu_line(o, "removed: the store folder cannot be resolved"); return; }
    mu_size_text(bin.bytes, size, sizeof size);
    mu_line(o, "removed: %zu folder(s), %s, in %s", bin.count, size, bin.dir);
    for (i = 0; i < bin.count; i++) {
        const mu_bin_item *it = &bin.items[i];
        char when[64] = "unknown";
        if (it->at > 0) {
            time_t t = (time_t)it->at;
            struct tm tm;
            if (!localtime_s(&tm, &t)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        }
        mu_size_text(it->bytes, size, sizeof size);
        mu_line(o, "removed:   %s: %s%s%s%s%s%s, %s, removed %s%s", it->folder, it->id, it->version[0] ? "@" : "", it->version,
                it->name[0] ? " (" : "", it->name, it->name[0] ? ")" : "", size, when,
                it->recorded ? "" : "; not recorded, so no map brings it back");
    }
    if (bin.count) mu_line(o, "removed: 'sh_mods removed empty --yes' deletes them, 'sh_mods removed restore \"<id>\"' puts one back");
    mu_bin_free(&bin);
}

/* The map entry `key` names: the installed folder's build (or an alias of
 * it), else a row whose mod is not installed, by id. -1 when the map does
 * not use it. */
static int mu_map_find(const mu_model *m, const mu_map *mp, const char *key)
{
    size_t i;
    int entry = -1;
    for (i = 0; i < m->list.count && entry < 0; i++)
        if (!strcmp(m->list.items[i].name, key) && m->list.items[i].loaded) entry = (int)i;
    for (i = 0; entry >= 0 && i < mp->mod_count; i++)
        if (mu_same_bundle(m, mp->mods[i].hash, m->list.items[entry].bundle.hash)) return (int)i;
    for (i = 0; i < mp->mod_count; i++)
        if (mu_mapmod_entry(m, &mp->mods[i]) < 0 && !strcmp(mp->mods[i].id, key)) return (int)i;
    return -1;
}

/* What removing one mod from the open map does. */
typedef struct mu_plan {
    int mod;                  /* the map entry, -1 when the map does not use it */
    char refusal[400];        /* why it cannot be done, "" when it can */
    sh_mm_ref *refs;
    size_t ref_count;
    int manifest_row, tag_removed, vanilla_after, published;
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
        strncpy_s(p->refusal, sizeof p->refusal, "the open map's entities cannot be read on this build", _TRUNCATE);
    else if (x->modules)
        _snprintf_s(p->refusal, sizeof p->refusal, _TRUNCATE, "the map places %d of its modules; delete them in Blueprint mode first",
                    x->modules);
    else if (mu_mapmod_follows(x))
        strncpy_s(p->refusal, sizeof p->refusal, "another mod this map uses needs it; remove that mod from the map first", _TRUNCATE);
    else if (x->uid_count > SH_EDITOR_DELETE_MAX)
        _snprintf_s(p->refusal, sizeof p->refusal, _TRUNCATE, "it has %zu placed entities, more than one delete takes (%d)",
                    x->uid_count, SH_EDITOR_DELETE_MAX);
    if (x->uid_count && mp->have_mm) {
        p->ref_count = sh_mm_references(&mp->mm, x->uids, x->uid_count, &p->refs);
        if (p->ref_count == (size_t)-1) { p->ref_count = 0; p->refs = NULL; }
    }
    for (i = 0; i < mp->mod_count; i++) if ((int)i != p->mod && !mu_mapmod_follows(&mp->mods[i])) others++;
    p->manifest_row = x->in_rows;
    p->vanilla_after = others == 0;
    p->tag_removed = p->vanilla_after && mp->have_info && mp->info.tagged == 1;
    p->published = mp->link != NULL;
}

static void mu_plan_free(mu_plan *p) { free(p->refs); p->refs = NULL; }

static const char *mu_delete_cause(int status)
{
    switch (status) {
    case SH_EDITOR_DELETE_NOT_MAIN:    return "the request did not run on the game's main thread";
    case SH_EDITOR_DELETE_NOT_READY:   return "the editor is not ready; open the map in the editor and try again";
    case SH_EDITOR_DELETE_NOT_ENTITY:  return "the editor is not in Object mode; switch to it and try again";
    case SH_EDITOR_DELETE_BUSY:        return "the editor is busy (an entity or a wire is held, or an edit is finishing); try again";
    case SH_EDITOR_DELETE_INVALID:     return "an entity is no longer in the map; refresh and try again";
    case SH_EDITOR_DELETE_RESTRICTED:  return "this map does not allow deleting entities";
    case SH_EDITOR_DELETE_PORTAL:      return "one of its entities is a portal door or cap; delete it in the editor";
    case SH_EDITOR_DELETE_PROTECTED:   return "the editor does not allow deleting one of its entities";
    case SH_EDITOR_DELETE_REFERENCED:  return "another object refers to these entities and the editor asks before deleting them; "
                                              "delete them in the editor, then try again";
    case SH_EDITOR_DELETE_FAULT:       return "the delete faulted; check the map";
    case SH_EDITOR_DELETE_INCOMPLETE:  return "the delete did not take every entity; check the map";
    default:                           return "the editor's delete is not available on this build";
    }
}

/* Remove the planned mod from the open map: its entities in one native
 * Delete Selection (one undo step; the editor drops the wires and references
 * to them with the entities), its manifest row, and with the last mod the
 * search tag. 1 done, 0 refused (msg says why; nothing changed). */
static int mu_map_apply(const mu_model *m, mu_map *mp, const mu_plan *p, const char *key, char *msg, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    const mu_mapmod *x = &mp->mods[p->mod];
    size_t i, rows = 0;
    int tag = 0;
    if (p->refusal[0]) { _snprintf_s(msg, cap, _TRUNCATE, "%s", p->refusal); return 0; }
    if (x->uid_count) {
        int st;
        if (!e->delete_entities) { strncpy_s(msg, cap, mu_delete_cause(SH_EDITOR_DELETE_UNAVAILABLE), _TRUNCATE); return 0; }
        st = e->delete_entities(x->uids, (int)x->uid_count);
        if (st != SH_EDITOR_DELETE_DISPATCHED) {
            mu_log("map remove %s: the delete of %zu entit%s was refused (status %d)", key, x->uid_count,
                   x->uid_count == 1 ? "y" : "ies", st);
            strncpy_s(msg, cap, mu_delete_cause(st), _TRUNCATE);
            return 0;
        }
    }
    /* Its row goes; with the last mod, so do the rows of mods only it needed. */
    for (i = 0; i < mp->rows.count; i++) {
        size_t k;
        int drop = 0;
        for (k = 0; k < mp->mod_count && !drop; k++)
            if (mu_same_bundle(m, mp->rows.rows[i].bundle, mp->mods[k].hash))
                drop = (int)k == p->mod || (p->vanilla_after && mu_mapmod_follows(&mp->mods[k]));
        if (drop) rows += sh_modmanifest_current_remove(mp->rows.rows[i].bundle);
    }
    if (p->tag_removed && e->remove_tag) tag = e->remove_tag(MU_TAG);
    InterlockedIncrement(&g_gen);
    mu_log("map remove %s: %zu entit%s deleted%s, %zu manifest row(s) dropped%s", key, x->uid_count,
           x->uid_count == 1 ? "y" : "ies", x->uid_count ? " in one undo step" : "", rows,
           !p->tag_removed ? "" : tag > 0 ? ", search tag 'mods' removed" : tag == 0 ? ", the search tag was already absent"
                                                                             : ", the search tag could not be removed");
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
    mu_kstr(b, "name", mp->name, 1);
    mu_key(b, "entities", 0);
    mu_put_entities_json(b, m, mp, x);
    mu_knum(b, "entityCount", (unsigned long long)x->uid_count, 0);
    mu_puts(b, ",\"references\":[");
    for (i = 0; i < p->ref_count && i < MU_MAX_MAP_REFS; i++) {
        char name[700];
        mu_ref_name(m, mp, &p->refs[i], name, sizeof name);
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "kind", p->refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", 1);
        mu_kstr(b, "name", name, 0);
        mu_putn(b, "}", 1);
    }
    mu_putn(b, "]", 1);
    mu_kbool(b, "manifestRow", p->manifest_row, 0);
    mu_kbool(b, "tagRemoved", p->tag_removed, 0);
    mu_kbool(b, "vanillaAfter", p->vanilla_after, 0);
    mu_kbool(b, "published", p->published, 0);
    mu_putn(b, "}", 1);
}

/* The dry run's lines for the console. */
static void mu_say_plan(const mu_out *o, const char *verb, const mu_model *m, const mu_map *mp, const mu_plan *p)
{
    const mu_mapmod *x = &mp->mods[p->mod];
    size_t i;
    const char *where = mp->name[0] ? mp->name : "the open map";
    if (x->uid_count) mu_line(o, "%s: deletes %zu placed entit%s from %s (one undo step)", verb, x->uid_count,
                              x->uid_count == 1 ? "y" : "ies", where);
    for (i = 0; i < x->uid_count && i < MU_MAX_MAP_ENTITIES; i++) {
        char name[300];
        mu_entity_label(m, sh_mm_find(&mp->mm, x->uids[i]), x->uids[i], name, sizeof name);
        mu_line(o, "%s:   %s", verb, name);
    }
    if (p->ref_count) mu_line(o, "%s: severs %zu wire or timeline reference(s) to those entities", verb, p->ref_count);
    for (i = 0; i < p->ref_count && i < MU_MAX_MAP_REFS; i++) {
        char name[700];
        mu_ref_name(m, mp, &p->refs[i], name, sizeof name);
        mu_line(o, "%s:   %s: %s", verb, p->refs[i].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", name);
    }
    if (p->manifest_row) mu_line(o, "%s: removes the mod's row from the map's list of mods", verb);
    if (p->tag_removed) mu_line(o, "%s: removes the search tag mods (the last mod the map uses)", verb);
    if (p->vanilla_after) mu_line(o, "%s: the map then needs no mods and plays without Snapmap+", verb);
    if (p->published) mu_line(o, "%s: the published copy on the server is unaffected until you publish the map again", verb);
}

static const char *mu_entry_title(const sh_ms_entry *s)
{
    return s->loaded && s->bundle.name && s->bundle.name[0] ? s->bundle.name : s->loaded && s->bundle.id[0] ? s->bundle.id : s->name;
}

/* mapRemoveModPreview, mapRemoveMod, uninstallPreview and uninstall, for the
 * tab and the console (o non-NULL prints the same consequences). */
static int mu_map_action(int action, const char *key, const mu_out *o)
{
    int uninstall = action == SH_MODS_ACT_UNINSTALL || action == SH_MODS_ACT_UNINSTALL_PREVIEW;
    int apply = action == SH_MODS_ACT_UNINSTALL || action == SH_MODS_ACT_MAP_REMOVE;
    const char *verb = uninstall ? "uninstall" : "map remove";
    mu_model m;
    mu_map mp;
    mu_record rec;
    mu_plan p;
    mu_buf b;
    char msg[MAX_PATH * 4 + 600], cause[300], title[300];
    int entry = -1, ok = 1;
    size_t i;
    memset(&b, 0, sizeof b);
    if (!key || !key[0]) {
        mu_line(o, "%s: no mod named", verb);
        return mu_simple_result(action, 0, "Select a mod first.");
    }
    if (mu_busy(cause, sizeof cause)) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Not now: %s.", cause);
        mu_line(o, "%s %s: refused: %s", verb, key, cause);
        return mu_simple_result(action, 0, msg);
    }
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_record_load(&m, &rec);
    mu_map_build(&mp, &m, &rec);
    for (i = 0; i < m.list.count && entry < 0; i++) if (!strcmp(m.list.items[i].name, key)) entry = (int)i;
    mu_plan_build(&m, &mp, key, &p);
    if (entry >= 0) strncpy_s(title, sizeof title, mu_entry_title(&m.list.items[entry]), _TRUNCATE);
    else if (p.mod >= 0) strncpy_s(title, sizeof title, mp.mods[p.mod].id, _TRUNCATE);
    else strncpy_s(title, sizeof title, key, _TRUNCATE);
    msg[0] = 0;
    if (uninstall && entry < 0) {
        ok = 0;
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s is not installed.", key);
    } else if (!uninstall && p.mod < 0) {
        ok = 0;
        _snprintf_s(msg, sizeof msg, _TRUNCATE, mp.entities < 0 ? "No map is open in the editor." : "The open map does not use %s.", title);
    } else if (p.mod >= 0 && p.refusal[0]) {
        ok = 0;
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s cannot be removed from %s: %s.", title, mp.name[0] ? mp.name : "the open map",
                    p.refusal);
    }
    if (!ok) {
        mu_line(o, "%s %s: %s", verb, key, msg);
        mu_result_head(&b, action, 0, msg);
    } else if (!apply) {
        /* The dry run: the consequence list the confirmation shows. */
        const sh_ms_entry *s = entry >= 0 ? &m.list.items[entry] : NULL;
        char moved[MAX_PATH], dir[MAX_PATH * 4], dest[SH_MS_NAME_MAX + 1];
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
            /* Every origin moves to removed-mods; a clash there gets a suffix. */
            if (!mu_removed_dir(m.root, dir, sizeof dir) || !mu_free_name(dir, mu_leaf(s->folder), dest, sizeof dest))
                strncpy_s(dest, sizeof dest, mu_leaf(s->folder), _TRUNCATE);
            _snprintf_s(moved, sizeof moved, _TRUNCATE, "removed-mods\\%s", dest);
            mu_putn(&b, "{", 1);
            mu_kstr(&b, "folder", mu_leaf(s->folder), 1);
            mu_kstr(&b, "disposition", "move", 0);
            mu_kstr(&b, "movedTo", moved, 0);
            mu_putn(&b, "}", 1);
            mu_puts(&b, ",\"editorRows\":[");
            for (i = 0; i < (size_t)m.entries[entry].row_count; i++) {
                if (i) mu_putn(&b, ",", 1);
                mu_str(&b, m.entries[entry].row_names[i]);
            }
            mu_putn(&b, "]", 1);
        }
        mu_putn(&b, "}", 1);
        if (o) {
            if (p.mod >= 0) mu_say_plan(o, verb, &m, &mp, &p);
            else if (uninstall) mu_line(o, "%s: the open map does not use it", verb);
            if (uninstall) {
                if (m.entries[entry].row_count)
                    mu_line(o, "%s: removes %d Toybox row(s) from the editor", verb, m.entries[entry].row_count);
                for (i = 0; i < (size_t)m.entries[entry].row_count; i++) mu_line(o, "%s:   %s", verb, m.entries[entry].row_names[i]);
                mu_line(o, "%s: moves the folder mods\\%s to removed-mods\\%s (kept there until the removed mods are "
                        "emptied)", verb, mu_leaf(s->folder), strcmp(dest, mu_leaf(s->folder)) ? dest : "");
                mu_line(o, "%s: other saved maps that use it are not changed; opening one, or undoing this in the editor, "
                        "puts it back", verb);
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
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s was not %s: %s.", title, uninstall ? "uninstalled" : "removed from the map",
                        mapmsg);
        } else if (!uninstall) {
            const mu_mapmod *x = &mp.mods[p.mod];
            if (!x->uid_count)
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %s from %s: no entity used it, so only its row was dropped%s.",
                            title, mp.name[0] ? mp.name : "the open map", p.vanilla_after ? "; the map now plays without Snapmap+" : "");
            else
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %s from %s: %zu entit%s deleted%s. Ctrl+Z in the editor brings %s back.",
                            title, mp.name[0] ? mp.name : "the open map", x->uid_count, x->uid_count == 1 ? "y" : "ies",
                            p.vanilla_after ? "; the map now plays without Snapmap+" : "", x->uid_count == 1 ? "it" : "them");
        } else {
            unsigned char *removing = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
            mu_removal rm;
            memset(&rm, 0, sizeof rm);
            if (!removing) ok = 0;
            else {
                removing[entry] = 1;
                mu_remove_marked(&m, removing, &rm);
                ok = rm.removed > 0 && rm.failed == 0;
            }
            mu_log("uninstall %s: %s%s", key, ok ? "moved to removed-mods" : "the folder could not be moved",
                   removed_from_map ? " after the open map was cleaned of it" : "");
            /* Ctrl+Z can bring its entities back: watch for them. */
            if (ok && removed_from_map && mp.mods[p.mod].uid_count)
                mu_watch_add(mp.mods[p.mod].uids, mp.mods[p.mod].uid_count, m.list.items[entry].loaded ? m.list.items[entry].bundle.hash : NULL);
            if (ok)
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "Uninstalled %s%s. Its folder was moved to removed-mods\\%s.", title,
                            removed_from_map ? " and removed it from the open map" : "", rm.moved);
            else
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "%s could not be uninstalled: %s. Close any program using its folder and "
                            "try again.%s", title, rm.first_fail[0] ? rm.first_fail : "nothing was moved",
                            removed_from_map ? " It was removed from the open map." : "");
            free(removing);
        }
        mu_line(o, "%s %s: %s", verb, key, msg);
        mu_result_head(&b, action, ok, msg);
    }
    mu_plan_free(&p);
    mu_map_free(&mp);
    mu_record_free(&rec);
    mu_model_free(&m);
    return mu_result_tail(&b);
}

/* ---- rebuild */

/* Build one folder again from its files (as 'sh_mods build'), write its
 * product block, then rescan so the game serves the new build. */
static int mu_rebuild(const char *key)
{
    mu_model m;
    sh_mb_resolver r;
    sh_mb_bundle bd;
    mu_stock stock = { 0 };
    char folder[MAX_PATH * 4], cause[SH_MB_CAUSE_MAX], msg[SH_MB_CAUSE_MAX + 400], note[160], before[SH_MB_HASH_HEX + 1] = "";
    int entry = -1, ok;
    size_t i;
    if (!key || !key[0]) return mu_simple_result(SH_MODS_ACT_REBUILD, 0, "Select a mod to rebuild.");
    if (mu_busy(cause, sizeof cause)) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Not now: %s.", cause);
        return mu_simple_result(SH_MODS_ACT_REBUILD, 0, msg);
    }
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    for (i = 0; i < m.list.count && entry < 0; i++) if (!strcmp(m.list.items[i].name, key)) entry = (int)i;
    if (entry < 0) { mu_model_free(&m); return mu_simple_result(SH_MODS_ACT_REBUILD, 0, "That mod is no longer installed."); }
    strncpy_s(folder, sizeof folder, m.list.items[entry].folder, _TRUNCATE);
    if (m.list.items[entry].loaded) strncpy_s(before, sizeof before, m.list.items[entry].bundle.hash, _TRUNCATE);
    r.installed = sh_ms_request_exists;
    r.installed_context = &m.list;
    r.stock = mu_stock_exists;
    r.stock_context = &stock;
    mu_log("rebuild %s: building", key);
    ok = sh_mb_build(folder, &r, &bd);
    cause[0] = 0;
    if (!ok) strncpy_s(cause, sizeof cause, bd.first_cause[0] ? bd.first_cause : "the mod could not be built", _TRUNCATE);
    else {
        modstore_lock();
        ok = sh_mb_write(&bd, folder, cause, sizeof cause);
        if (ok) sh_ms_note_build(folder, &bd);
        modstore_unlock();
    }
    mu_model_free(&m);
    note[0] = 0;
    if (!ok) {
        mu_log("rebuild %s: refused: %s", key, cause);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Not rebuilt: %s", cause);
    } else {
        int same = !strcmp(before, bd.hash);
        mu_log("rebuild %s: %s@%s+%llu, bundle %s%s", key, bd.id, bd.version, bd.build, bd.hash, same ? " (unchanged)" : "");
        mu_rescan(note, sizeof note);
        if (same) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Rebuilt %s: nothing changed (%zu files).", key, bd.file_count);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "Rebuilt %s: build %llu, %zu files.", key, bd.build, bd.file_count);
    }
    InterlockedIncrement(&g_gen);
    {
        mu_buf b;
        memset(&b, 0, sizeof b);
        mu_result_head(&b, SH_MODS_ACT_REBUILD, ok, msg);
        mu_kstr(&b, "key", key, 0);
        mu_kstr(&b, "bundle", ok ? bd.hash : "", 0);
        mu_kstr(&b, "cause", ok ? "" : cause, 0);
        mu_kstr(&b, "index", note, 0);
        sh_mb_free(&bd);
        return mu_result_tail(&b);
    }
}

/* ---- the console verbs */

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
        mu_line(o, "map: %s (id %s), %d entities, last saved %s", mp.name[0] ? mp.name : "(not saved yet)",
                mp.have_info && mp.info.local_id[0] ? mp.info.local_id : "none", mp.entities, when);
        if (mp.link) mu_line(o, "map: published as item %s; its mods %s", mp.link->item, mp.claim ? "are uploaded (a claim is stored)" : "have no stored claim");
        else mu_line(o, "map: no publish of it with mods is recorded on this computer");
        mu_line(o, "map: search tag mods: %s", !mp.have_info || mp.info.tagged < 0 ? "unknown" : mp.info.tagged ? "present" : "absent");
        if (!mp.mod_count) mu_line(o, "map: uses no mods; it plays without Snapmap+");
        for (i = 0; i < mp.mod_count; i++) {
            const mu_mapmod *x = &mp.mods[i];
            int at = mu_mapmod_entry(&m, x);
            sh_mm_ref *refs = NULL;
            size_t n = mp.have_mm && x->uid_count ? sh_mm_references(&mp.mm, x->uids, x->uid_count, &refs) : 0;
            if (n == (size_t)-1) n = 0;
            mu_line(o, "map: mod %s%s%s (%s, %s): %zu placed entit%s%s, %zu wire or timeline reference(s)%s%s", x->id,
                    x->version[0] && mu_version(x->version)[0] ? "@" : "", mu_version(x->version),
                    at >= 0 ? m.list.items[at].name : "not installed", mu_mapmod_state(&m, x), x->uid_count,
                    x->uid_count == 1 ? "y" : "ies", x->modules ? ", placed modules" : "", n,
                    x->in_rows ? "" : ", no row yet (the next save adds it)", mu_mapmod_follows(x) ? ", needed by another mod" : "");
            for (k = 0; k < x->uid_count && k < MU_MAX_MAP_ENTITIES; k++) {
                char name[300];
                mu_entity_label(&m, sh_mm_find(&mp.mm, x->uids[k]), x->uids[k], name, sizeof name);
                mu_line(o, "map:   entity %s", name);
            }
            for (k = 0; k < n && k < MU_MAX_MAP_REFS; k++) {
                char name[700];
                mu_ref_name(&m, &mp, &refs[k], name, sizeof name);
                mu_line(o, "map:   %s %s", refs[k].kind == SH_MM_REF_TIMELINE ? "timeline" : "wire", name);
            }
            free(refs);
        }
    }
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
    if (_stricmp(verb, "map") && _stricmp(verb, "uninstall") && _stricmp(verb, "removed")) return 0;
    if (!mu_on_main()) return -1;
    if (!_stricmp(verb, "removed")) {
        if (argc == 1) { mu_say_bin(&o); return 1; }
        if (!_stricmp(argv[1], "empty")) {
            for (i = 2; i < argc; i++) if (!_stricmp(argv[i], "--yes")) yes = 1;
            (void)mu_bin_empty(yes, NULL, &o);
            return 1;
        }
        if (!_stricmp(argv[1], "restore") && argc == 3) { (void)mu_bin_restore_key(argv[2], &o); return 1; }
        mu_line(&o, "removed: usage: sh_mods removed, sh_mods removed empty [--yes], or sh_mods removed restore \"<id>\"");
        return 1;
    }
    if (!_stricmp(verb, "map") && argc == 1) { mu_say_map(&o); return 1; }
    first = !_stricmp(verb, "map") ? 2 : 1;
    if (!_stricmp(verb, "map") && _stricmp(argv[1], "remove")) {
        mu_line(&o, "map: usage: sh_mods map, or sh_mods map remove \"<id>\" [--yes]");
        return 1;
    }
    for (i = first; i < argc; i++) {
        if (!_stricmp(argv[i], "--yes")) yes = 1;
        else if (!key) key = argv[i];
        else { key = NULL; break; }
    }
    if (!key || !key[0]) {
        mu_line(&o, "%s: usage: sh_mods %s \"<id>\" [--yes]", verb, !_stricmp(verb, "map") ? "map remove" : "uninstall");
        return 1;
    }
    if (!_stricmp(verb, "map")) (void)mu_map_action(yes ? SH_MODS_ACT_MAP_REMOVE : SH_MODS_ACT_MAP_REMOVE_PREVIEW, key, &o);
    else (void)mu_map_action(yes ? SH_MODS_ACT_UNINSTALL : SH_MODS_ACT_UNINSTALL_PREVIEW, key, &o);
    return 1;
}

/* ---- migration */

static int mu_migrate(void)
{
    const sh_modui_engine *e = mu_engine();
    int r;
    mu_buf b;
    memset(&b, 0, sizeof b);
    if (!e->migrate_start) return mu_simple_result(SH_MODS_ACT_MIGRATE, 0, "Migration is not available in this build.");
    modstore_lock();   /* the pass's scan waits for a store step in progress */
    r = e->migrate_start(1);
    modstore_unlock();
    InterlockedIncrement(&g_gen);
    mu_log("migrate: %s", r > 0 ? "started on the background thread" : r == 0 ? "nothing to do" : "already running or could not start");
    mu_result_head(&b, SH_MODS_ACT_MIGRATE, r >= 0, r > 0
        ? "Moved your mods from the old overrides folder into the mods folder."
        : r == 0 ? "Nothing to migrate." : "A migration is already running; try again when it finishes.");
    mu_kbool(&b, "started", r > 0, 0);
    return mu_result_tail(&b);
}

int sh_modui_wait(unsigned ms)
{
    const sh_modui_engine *e = mu_engine();
    ULONGLONG until = GetTickCount64() + ms;
    if (!e->migrate_busy) return 1;
    while (e->migrate_busy() == 1) {
        if (GetTickCount64() >= until) return 0;
        Sleep(20);
    }
    return 1;
}

/* ------------------------------------------------------------------ the doctor */

static void mu_print_entry(const mu_out *o, const mu_model *m, size_t i)
{
    const mu_entry *u = &m->entries[i];
    const sh_ms_entry *e = u->e;
    char state[SH_MS_KEY_MAX + 96], key[SH_MS_KEY_MAX + 1];
    /* id@version+build, without a version the converter generated. */
    if (e->loaded && sh_mb_version_generated(e->bundle.version))
        _snprintf_s(key, sizeof key, _TRUNCATE, "%s+%llu", e->bundle.id, e->bundle.build);
    else strncpy_s(key, sizeof key, e->key, _TRUNCATE);
    if (!strcmp(u->state, "superseded"))
        _snprintf_s(state, sizeof state, _TRUNCATE, "superseded, %s serves %d of its path(s)",
                    u->superseded_by >= 0 ? m->list.items[u->superseded_by].name : "a newer build", u->superseded_paths);
    else strncpy_s(state, sizeof state, u->state, _TRUNCATE);
    mu_say(o, "%s mods\\%s (%s): %s%s%s%s%s", e->delivered ? "bundle delivered by a map," : u->legacy ? "migrated bundle" : "bundle",
           e->name, key, state, u->cause[0] ? "; " : "", u->cause, u->origin[0] && !e->delivered ? "; from " : "",
           e->delivered ? "" : u->origin);
}

int sh_modui_doctor(int verify, sh_modui_line_fn line, void *context)
{
    const sh_modui_engine *e = mu_engine();
    mu_model m;
    mu_out o;
    sh_modserve_stats st;
    size_t i, k, delivered = 0;
    unsigned entries = 0;
    int pending;
    if (!mu_on_main()) return -1;
    o.fn = line;
    o.context = context;
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    sh_modserve_get_stats(&st);
    for (i = 0; i < m.list.count; i++) if (m.list.items[i].delivered) delivered++;
    mu_say(&o, "store %s: %zu mod folder(s), %zu of them delivered by maps; all listed in the Mods tab%s",
           m.root_ok ? m.root : "(unresolved)", m.list.count, delivered, m.listed ? "" : "; the folder cannot be read");
    mu_say(&o, "serving index: %d of %d bundle(s) active, %d file(s), %d shadow(s), %d conflict(s) (%d composed), %d editor row(s) registered",
           st.bundles_admitted, st.bundles_seen, st.files, st.shadows, st.conflicts, st.conflicts_composed, st.editor_rows_registered);
    for (i = 0; i < m.list.count; i++) mu_print_entry(&o, &m, i);
    for (i = 0; i < m.row_count; i++) {
        const char *id = m.list.items[m.rows[i].serving].bundle.id;
        char keys[800];
        size_t at = 0;
        int earlier = 0;
        if (!m.rows[i].duplicate) continue;
        for (k = 0; k < i && !earlier; k++)
            earlier = m.rows[k].duplicate && !strcmp(m.list.items[m.rows[k].serving].bundle.id, id);
        if (earlier) continue;
        keys[0] = 0;
        for (k = i; k < m.row_count; k++) {
            const mu_entry *u = &m.entries[m.rows[k].serving];
            int w;
            if (!m.rows[k].duplicate || strcmp(u->e->bundle.id, id)) continue;
            w = _snprintf_s(keys + at, sizeof keys - at, _TRUNCATE, "%smods\\%s (%s)", at ? ", " : "", u->e->name,
                            u->origin[0] ? u->origin : "your folder");
            if (w < 0) break;
            at += (size_t)w;
        }
        mu_say(&o, "duplicate id %s: %s; another installed mod declares this id; change one id in its mod.json", id, keys);
    }
    pending = e->migrate_pending ? e->migrate_pending(&entries) : -2;
    if (pending == -2) mu_say(&o, "migration: not available in this build");
    else if (pending < 0) mu_say(&o, "migration: the data folder cannot be resolved");
    else if (!entries) mu_say(&o, "migration: no overrides folder, or it is empty");
    else mu_say(&o, "migration: overrides holds %u entr%s, %d not migrated%s%s", entries, entries == 1 ? "y" : "ies", pending,
                pending ? " (Migrate in the Mods tab, or 'sh_mods migrate --again'; the migration section of mods\\snapmap-plus.json has each one's outcome)" : "",
                e->migrate_busy && e->migrate_busy() == 1 ? "; a migration is running" : "");
    mu_print_shadows(&o, &m);
    if (verify) {
        sh_ms_verify_stats sum;
        unsigned passed = 0, failed = 0;
        char first[SH_MB_CAUSE_MAX + SH_MS_KEY_MAX + 4], msg[SH_MB_CAUSE_MAX + SH_MS_KEY_MAX + 128];
        mu_say(&o, "verify: rehashing every build");
        if (!mu_verify_core("", &o, &sum, &passed, &failed, first, sizeof first)) mu_say(&o, "verify: the mods folder cannot be read");
        else {
            mu_verify_message(passed, failed, first, msg, sizeof msg);
            mu_say(&o, "verify: %s (%u file(s), %u MiB)", msg, sum.files, sum.bytes_mb);
        }
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
    if (!mu_on_main()) return -1;
    switch (action) {
    case SH_MODS_ACT_INSTALL:
        return mu_install(arg);
    case SH_MODS_ACT_VERIFY:
        return mu_verify(arg);
    case SH_MODS_ACT_RESCAN: {
        int n;
        char msg[520];
        sh_modserve_refresh rf;
        mu_buf b;
        note[0] = 0;
        n = mu_rescan_refresh(note, sizeof note, &rf);
        mu_log("rescan: %s", note);
        if (n >= 0 && rf.state > 0)
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "Mods folder reread: %s. In the game: %s.", note, rf.note);
        else if (n >= 0) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Mods folder reread: %s.", note);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "Rescan failed: %s.", note);
        if (n < 0 || rf.state < 0) return mu_simple_result(action, n >= 0, msg);
        memset(&b, 0, sizeof b);
        mu_result_head(&b, action, 1, msg);
        mu_put_refresh(&b, &rf);
        return mu_result_tail(&b);
    }
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
    case SH_MODS_ACT_PRUNE_LIST:
        return mu_prune(0, NULL);
    case SH_MODS_ACT_PRUNE_APPLY:
        return mu_prune(1, arg);
    case SH_MODS_ACT_REMOVE:
        return mu_remove(arg);
    case SH_MODS_ACT_MIGRATE:
        return mu_migrate();
    case SH_MODS_ACT_REBUILD:
        return mu_rebuild(arg);
    case SH_MODS_ACT_UNINSTALL:
    case SH_MODS_ACT_UNINSTALL_PREVIEW:
    case SH_MODS_ACT_MAP_REMOVE:
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW:
        return mu_map_action(action, arg, NULL);
    case SH_MODS_ACT_REMOVED_EMPTY:
        return mu_bin_empty(1, arg, NULL);
    case SH_MODS_ACT_REMOVED_RESTORE:
        return mu_bin_restore_key(arg, NULL);
    default:
        return mu_simple_result(action, 0, "Unknown action.");
    }
}

/* ------------------------------------------------------------------ the slot */

int sh_modui_slot(struct sh_iface *self, struct sh_mods_call *call)
{
    int n;
    (void)self;
    if (!call) return 0;
    call->generation = (unsigned)g_gen;
    switch (call->op) {
    case SH_MODS_LIST:
        n = sh_modui_list();
        call->needed = n < 0 ? 0 : n;
        call->generation = (unsigned)g_gen;
        return n >= 0;
    case SH_MODS_RUN:
        n = sh_modui_run(call->action, call->arg);
        call->needed = n < 0 ? 0 : n;
        call->generation = (unsigned)g_gen;
        return n >= 0;
    case SH_MODS_FETCH:
        return sh_modui_fetch(call->out, call->out_cap, &call->needed, &call->generation);
    case SH_MODS_WAIT:
        call->needed = 0;
        return sh_modui_wait(call->out_cap > 0 ? (unsigned)call->out_cap : 0);
    case SH_MODS_ROOT: {
        char root[MAX_PATH * 4];
        size_t len;
        call->needed = 0;
        if (!sh_ms_root(root, sizeof root)) return 0;
        len = strlen(root);
        call->needed = (int)len;
        if (!call->out || call->out_cap <= (int)len) return -1;
        memcpy(call->out, root, len + 1);
        return 1;
    }
    default:
        return 0;
    }
}

#ifdef SH_MODUI_TESTING
void sh_modui_test_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_doc);
    g_doc = NULL;
    g_doc_len = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_engine_bound = 0;
    memset(&g_engine, 0, sizeof g_engine);
    g_watch_n = 0;
    g_poll_next = 0;
    InterlockedExchange(&g_owed_refresh, 0);
    InterlockedExchange(&g_bin_records, -1);
    g_announce[0] = 0;
}
#endif
