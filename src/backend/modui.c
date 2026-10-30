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
#ifndef SH_MODUI_PURE
#include "overrides.h"
#include "modstock.h"
#endif

/* A stock resource every 13954591 SnapMap install ships (as modcmd.c probes):
 * reading it proves the installed-resource reader works before its answers
 * are trusted. */
#define MU_STOCK_PROBE "generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl"

#define MU_MAX_SHADOW_GROUPS 4000   /* shadow rows sent to the page; the total is always reported */
#define MU_MAX_ROW_NAMES     12     /* names listed per map bundle; the count is always reported */
#define MU_MAX_PLACED        65536  /* distinct placed names read from the map */
#define MU_NAME_MAX          512

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
    e->palette_rebuild = modserve_palette_rebuild_requested;
    e->stock_exists = mu_default_stock;
#endif
}

void sh_modui_bind(const sh_modui_engine *engine)
{
    mu_defaults(&g_engine);
    if (engine) {
        if (engine->on_main_thread) g_engine.on_main_thread = engine->on_main_thread;
        if (engine->placed_names) g_engine.placed_names = engine->placed_names;
        if (engine->rescan) g_engine.rescan = engine->rescan;
        if (engine->palette_rebuild) g_engine.palette_rebuild = engine->palette_rebuild;
        if (engine->stock_exists) g_engine.stock_exists = engine->stock_exists;
    }
    g_engine_bound = 1;
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
    const char *state;           /* active, legacy, superseded, unindexed, skipped */
    char cause[SH_MB_CAUSE_MAX];
    int superseded_paths;        /* paths a newer build of the same id serves */
    int superseded_by;           /* entry index, or -1 */
    int editor_rows;             /* snapEditorEntityDef files */
    int shadows;
    int legacy;                  /* under legacy\ or a "legacy" descriptor block */
    int admitted;                /* sh_modserve_bundle_state == 1 */
} mu_entry;

typedef struct mu_model {
    sh_ms_list list;
    int listed;
    mu_entry *entries;
    char root[MAX_PATH * 4];
    int root_ok;
} mu_model;

static int mu_find_hash(const mu_model *m, const char *hash)
{
    size_t i;
    if (!hash || !hash[0]) return -1;
    for (i = 0; i < m->list.count; i++)
        if (m->list.items[i].loaded && !strcmp(m->list.items[i].bundle.hash, hash)) return (int)i;
    return -1;
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
    if (m->listed) sh_ms_list_free(&m->list);
    free(m->entries);
    memset(m, 0, sizeof *m);
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
        u->legacy = e->legacy || (e->loaded && e->bundle.legacy);
        if (e->loaded)
            for (k = 0; k < e->bundle.file_count; k++) {
                const sh_mb_file *f = &e->bundle.files[k];
                if (f->ptype && !_stricmp(f->ptype, "snapEditorEntityDef")) u->editor_rows++;
                if (f->shadowed) u->shadows++;
            }
        if (!e->loaded) {
            u->state = "skipped";
            strncpy_s(u->cause, sizeof u->cause, e->cause[0] ? e->cause : "bundle.json unreadable", _TRUNCATE);
            continue;
        }
        /* The serving index decides what is served; the store's own check
         * (sizes, the last verification) explains a build the index lacks,
         * and is a warning on one it serves (it falls back to blob copies). */
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
                strncpy_s(u->cause, sizeof u->cause, "not in the serving index yet; Rescan to use it", _TRUNCATE);
            }
            continue;
        }
        if (!sh_ms_entry_usable(e) && e->cause[0])
            _snprintf_s(u->cause, sizeof u->cause, _TRUNCATE, "%s; Verify to check the build", e->cause);
        u->admitted = 1;
        u->state = u->legacy ? "legacy" : "active";
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
    return 1;
}

static void mu_put_bundle(mu_buf *b, const mu_model *m, size_t i)
{
    const mu_entry *u = &m->entries[i];
    const sh_ms_entry *e = u->e;
    const sh_mb_bundle *bd = &e->bundle;
    mu_putn(b, "{", 1);
    mu_kstr(b, "key", e->key, 1);
    mu_kstr(b, "id", e->loaded ? bd->id : "", 0);
    mu_kstr(b, "name", e->loaded && bd->name ? bd->name : "", 0);
    mu_kstr(b, "author", e->loaded && bd->author ? bd->author : "", 0);
    mu_kstr(b, "version", e->loaded ? bd->version : "", 0);
    mu_knum(b, "build", e->loaded ? bd->build : 0, 0);
    mu_kstr(b, "hash", e->loaded ? bd->hash : "", 0);
    mu_knum(b, "size", e->loaded ? bd->total_size : 0, 0);
    mu_knum(b, "files", e->loaded ? bd->file_count : 0, 0);
    /* Of the files, stock pointers: served from the installed game, not stored. */
    mu_knum(b, "pointers", e->loaded ? bd->stock_count : 0, 0);
    mu_kstr(b, "state", u->state, 0);
    mu_kstr(b, "cause", u->cause, 0);
    mu_kbool(b, "legacy", u->legacy, 0);
    mu_kbool(b, "verified", e->has_state && e->validated, 0);
    mu_kint(b, "installedAt", e->installed_at, 0);
    mu_knum(b, "editorRows", (unsigned)u->editor_rows, 0);
    mu_knum(b, "shadows", (unsigned)u->shadows, 0);
    mu_knum(b, "supersededPaths", (unsigned)u->superseded_paths, 0);
    mu_kstr(b, "supersededBy", u->superseded_by >= 0 ? m->list.items[u->superseded_by].key : "", 0);
    mu_kstr(b, "folder", mu_leaf(e->folder), 0);
    mu_putn(b, "}", 1);
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
 * (two bundles provide it with different bytes), grouped by path. */
static void mu_put_shadows(mu_buf *b, const mu_model *m)
{
    mu_path *paths = NULL;
    size_t n = 0, cap = 0, i, k, groups = 0, sent = 0;
    unsigned stock = 0, legacy = 0, conflicts = 0;
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
                if (!np) { b->oom = 1; free(paths); return; }
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
    mu_puts(b, ",\"shadows\":[");
    for (i = 0; i < n;) {
        size_t j = i + 1, q;
        int conflict = paths[i].conflict, any_legacy = m->entries[paths[i].entry].legacy, shadow = paths[i].shadow;
        const char *kind;
        sh_modserve_hit hit;
        while (j < n && !strcmp(paths[j].request, paths[i].request)) {
            conflict |= paths[j].conflict;
            shadow |= paths[j].shadow;
            any_legacy |= m->entries[paths[j].entry].legacy;
            j++;
        }
        kind = conflict ? "conflict" : any_legacy ? "legacy" : "stock";
        if (conflict) conflicts++;
        else if (any_legacy) legacy++;
        else stock++;
        groups++;
        if (sent < MU_MAX_SHADOW_GROUPS) {
            int served = sh_modserve_lookup(paths[i].request, &hit) ? mu_find_hash(m, hit.bundle_hash) : -1;
            if (sent) mu_putn(b, ",", 1);
            mu_putn(b, "{", 1);
            mu_kstr(b, "request", paths[i].request, 1);
            mu_kstr(b, "kind", kind, 0);
            mu_kbool(b, "shadow", shadow, 0);
            mu_kstr(b, "served", served >= 0 ? m->list.items[served].key : "", 0);
            /* The serving copy is a stock pointer: the installed game's bytes. */
            mu_kbool(b, "installed", served >= 0 && hit.is_stock, 0);
            if (conflict) {
                sh_modserve_conflict ci;
                int known = sh_modserve_conflict_info(paths[i].request, &ci);
                mu_kstr(b, "resolution", !known ? "neither" : ci.withdrawn ? "withdrawn" : ci.composed ? "composed" : "chosen", 0);
                mu_kstr(b, "reason", known ? ci.reason : "", 0);
            }
            mu_key(b, "bundles", 0);
            mu_putn(b, "[", 1);
            for (q = i; q < j; q++) {
                if (q > i) mu_putn(b, ",", 1);
                mu_str(b, m->list.items[paths[q].entry].key);
            }
            mu_puts(b, "]}");
            sent++;
        }
        i = j;
    }
    mu_puts(b, "],\"shadowTotals\":{");
    mu_knum(b, "stock", stock, 1);
    mu_knum(b, "legacy", legacy, 0);
    mu_knum(b, "conflict", conflicts, 0);
    mu_knum(b, "total", (unsigned long long)groups, 0);
    mu_knum(b, "sent", (unsigned long long)sent, 0);
    mu_putn(b, "}", 1);
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

/* The current map's view: manifest rows (its load-time record) and the
 * bundles its placed names resolve to now. */
typedef struct mu_map {
    sh_modmanifest_list rows;
    mu_closure closure;
    int entities;             /* entities read, -1 no editor map, -2 not available */
    size_t names;
} mu_map;

static void mu_map_build(mu_map *mp)
{
    const sh_modui_engine *e = mu_engine();
    memset(mp, 0, sizeof *mp);
    sh_modmanifest_current(&mp->rows);
    mp->entities = -2;
    if (e->placed_names) {
        mu_names names;
        memset(&names, 0, sizeof names);
        mp->entities = e->placed_names(mu_names_add, &names);
        mp->names = names.count;
        if (mp->entities >= 0 && names.count && sh_modserve_active())
            modserve_bundles_for_names_visit((const char *const *)names.items, names.count, mu_closure_visit,
                                             &mp->closure);
        mu_names_free(&names);
    }
}

static void mu_map_free(mu_map *mp)
{
    sh_modmanifest_list_free(&mp->rows);
    mu_closure_free(&mp->closure);
}

static int mu_map_references(const mu_map *mp, const char *hash)
{
    size_t i;
    for (i = 0; i < mp->rows.count; i++) if (!strcmp(mp->rows.rows[i].bundle, hash)) return 1;
    for (i = 0; i < mp->closure.count; i++) if (!strcmp(mp->closure.items[i].hash, hash)) return 1;
    return 0;
}

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
        mu_kstr(b, "version", r->version, 0);
        mu_kstr(b, "bundle", r->bundle, 0);
        mu_knum(b, "size", r->size, 0);
        mu_kstr(b, "state", state, 0);
        mu_kstr(b, "cause", st < 0 ? cause : at >= 0 && st == 0 ? m->entries[at].cause : "", 0);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].key : "", 0);
        mu_kstr(b, "otherKey", other >= 0 ? m->list.items[other].key : "", 0);
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
        for (k = 0; k < mp->rows.count; k++) if (!strcmp(mp->rows.rows[k].bundle, cb->hash)) in_manifest = 1;
        if (i) mu_putn(b, ",", 1);
        mu_putn(b, "{", 1);
        mu_kstr(b, "id", cb->id, 1);
        mu_kstr(b, "version", cb->version, 0);
        mu_kstr(b, "bundle", cb->hash, 0);
        mu_knum(b, "size", cb->size, 0);
        mu_kstr(b, "key", at >= 0 ? m->list.items[at].key : "", 0);
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
    mu_puts(b, "]}");
}

/* ------------------------------------------------------------------ the document */

int sh_modui_list(void)
{
    mu_model m;
    mu_map mp;
    mu_buf b;
    sh_modserve_stats st;
    size_t i;
    int n;
    if (!mu_on_main()) return -1;
    memset(&b, 0, sizeof b);
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_map_build(&mp);
    sh_modserve_get_stats(&st);
    mu_putn(&b, "{", 1);
    mu_knum(&b, "v", 1, 1);
    mu_knum(&b, "gen", (unsigned)g_gen, 0);
    mu_kstr(&b, "root", m.root_ok ? m.root : "", 0);
    mu_kbool(&b, "storeReadable", m.listed, 0);
    mu_puts(&b, ",\"index\":{");
    mu_kint(&b, "bundles", st.bundles_seen, 1);
    mu_kint(&b, "admitted", st.bundles_admitted, 0);
    mu_kint(&b, "files", st.files, 0);
    mu_kint(&b, "conflicts", st.conflicts, 0);
    mu_kint(&b, "shadows", st.shadows, 0);
    mu_kint(&b, "editorRowsRegistered", st.editor_rows_registered, 0);
    mu_putn(&b, "}", 1);
    mu_puts(&b, ",\"bundles\":[");
    for (i = 0; i < m.list.count; i++) {
        if (i) mu_putn(&b, ",", 1);
        mu_put_bundle(&b, &m, i);
    }
    mu_putn(&b, "]", 1);
    mu_put_map(&b, &m, &mp);
    mu_put_shadows(&b, &m);
    mu_putn(&b, "}", 1);
    mu_map_free(&mp);
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

static int mu_rescan(char *note, size_t cap)
{
    const sh_modui_engine *e = mu_engine();
    int n;
    if (!e->rescan) { strncpy_s(note, cap, "the serving index cannot be rebuilt from here", _TRUNCATE); return -1; }
    n = e->rescan();
    InterlockedIncrement(&g_gen);
    if (n < 0) strncpy_s(note, cap, "the serving index could not be rebuilt; try Rescan", _TRUNCATE);
    else _snprintf_s(note, cap, _TRUNCATE, "%d mod(s) active", n);
    return n;
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
                    rows ? "; its editor rows appear after Rebuild palette" : "");
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

static int mu_verify(const char *key)
{
    sh_ms_list list;
    sh_ms_verify_stats st, sum;
    char cause[SH_MB_CAUSE_MAX], first[SH_MB_CAUSE_MAX + SH_MS_KEY_MAX + 4], msg[SH_MB_CAUSE_MAX + 128];
    unsigned passed = 0, failed = 0;
    size_t i;
    mu_buf b;
    memset(&b, 0, sizeof b);
    memset(&sum, 0, sizeof sum);
    first[0] = 0;
    if (key && key[0]) {
        int ok = sh_ms_verify(key, &st, cause, sizeof cause);
        sum = st;
        if (ok) passed = st.entries ? st.entries : 1;
        else { failed = 1; _snprintf_s(first, sizeof first, _TRUNCATE, "%s: %s", key, cause); }
        mu_log("verify %s: %s%s%s", key, ok ? "pass" : "FAILED", ok ? "" : ": ", ok ? "" : cause);
    } else {
        if (!sh_ms_enumerate(&list)) return mu_simple_result(SH_MODS_ACT_VERIFY, 0, "The mods folder cannot be read.");
        for (i = 0; i < list.count; i++) {
            const sh_ms_entry *e = &list.items[i];
            int ok;
            if (!e->loaded) continue;
            memset(&st, 0, sizeof st);
            ok = sh_ms_verify(e->key, &st, cause, sizeof cause);
            sum.files += st.files;
            sum.bytes_mb += st.bytes_mb;
            sum.blobs_checked += st.blobs_checked;
            sum.entries += st.entries;
            if (ok) passed++;
            else {
                failed++;
                if (!first[0]) _snprintf_s(first, sizeof first, _TRUNCATE, "%s: %s", e->key, cause);
            }
            mu_log("verify %s: %s%s%s", e->key, ok ? "pass" : "FAILED", ok ? "" : ": ", ok ? "" : cause);
        }
        sh_ms_list_free(&list);
    }
    InterlockedIncrement(&g_gen);
    if (!passed && !failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "No installed mod to verify.");
    else if (!failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Verified %u build(s): every file matches.", passed);
    else _snprintf_s(msg, sizeof msg, _TRUNCATE, "%u of %u build(s) failed verification. First: %s", failed,
                     passed + failed, first);
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

static int mu_blob_used_elsewhere(const mu_model *m, const char *blob, const unsigned char *removing)
{
    size_t i, k;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        if (removing[i] || !e->loaded) continue;
        for (k = 0; k < e->bundle.file_count; k++)
            if (!e->bundle.files[k].stock && !strcmp(e->bundle.files[k].blob, blob)) return 1;
    }
    return 0;
}

/* A build may be removed when it is an ordinary (not legacy) installed build
 * under the store root, a newer build of the same mod provides every one of
 * its paths, and neither the open map's manifest nor its placed names
 * reference it. Builds of a mod nothing newer covers, skipped builds and
 * legacy conversions are never candidates. */
static size_t mu_candidates(const mu_model *m, const mu_map *mp, mu_candidate *out)
{
    size_t i, k, n = 0;
    for (i = 0; i < m->list.count; i++) {
        const sh_ms_entry *e = &m->list.items[i];
        int newer = -1;
        if (!e->loaded || m->entries[i].legacy || !sh_ms_entry_usable(e)) continue;
        if (_stricmp(mu_leaf(e->folder), e->key)) continue;
        if (mu_map_references(mp, e->bundle.hash)) continue;
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
        mu_kstr(b, "key", e->key, 1);
        mu_kstr(b, "bundle", e->bundle.hash, 0);
        mu_knum(b, "size", e->bundle.total_size, 0);
        mu_knum(b, "files", e->bundle.file_count, 0);
        mu_kstr(b, "coveredBy", m->list.items[c[i].newer].key, 0);
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

/* The store's blob file for `blob` (64 hex), or NULL. */
static wchar_t *mu_blob_path(const char *root, const char *blob)
{
    char p[MAX_PATH * 4 + 80];
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\blobs\\%s", root, blob);
    return sh_mb_wpath(p);
}

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

static int mu_prune(int apply, const char *keys)
{
    mu_model m;
    mu_map mp;
    mu_candidate *c;
    unsigned char *removing;
    char (*staged)[MAX_PATH * 4];
    wchar_t **to;
    size_t n, i, k, removed = 0, failed = 0, blobs = 0;
    unsigned long long freed = 0;
    int all_loaded = 1;
    char msg[512], note[160], first_fail[SH_MS_KEY_MAX + 80];
    mu_buf b;
    memset(&b, 0, sizeof b);
    note[0] = first_fail[0] = 0;
    if (!mu_model_build(&m)) { mu_model_free(&m); return -1; }
    mu_map_build(&mp);
    c = (mu_candidate *)calloc(m.list.count ? m.list.count : 1, sizeof *c);
    removing = (unsigned char *)calloc(m.list.count ? m.list.count : 1, 1);
    staged = (char (*)[MAX_PATH * 4])calloc(m.list.count ? m.list.count : 1, sizeof *staged);
    to = (wchar_t **)calloc(m.list.count ? m.list.count : 1, sizeof *to);
    if (!c || !removing || !staged || !to) {
        free(c); free(removing); free(staged); free(to);
        mu_map_free(&mp); mu_model_free(&m);
        return -1;
    }
    n = mu_candidates(&m, &mp, c);
    for (i = 0; i < m.list.count; i++) if (!m.list.items[i].loaded) all_loaded = 0;
    if (!apply) {
        unsigned long long total = 0;
        for (i = 0; i < n; i++) total += m.list.items[c[i].entry].bundle.total_size;
        if (n) _snprintf_s(msg, sizeof msg, _TRUNCATE, "%zu build(s) can be removed; a newer build of the same mod covers each one.", n);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "No build can be removed: every installed build is the newest of its mod, is in use by the open map, or is a legacy conversion.");
        mu_result_head(&b, SH_MODS_ACT_PRUNE_LIST, 1, msg);
        mu_put_candidates(&b, &m, c, n);
        mu_knum(&b, "bytes", total, 0);
    } else {
        /* Only the keys the person confirmed, and only while still candidates. */
        for (i = 0; i < n; i++) {
            const sh_ms_entry *e = &m.list.items[c[i].entry];
            if (keys && mu_key_listed(keys, e->key)) removing[c[i].entry] = 1;
        }
        for (i = 0; i < m.list.count; i++) {
            const sh_ms_entry *e = &m.list.items[i];
            wchar_t *from;
            if (!removing[i]) continue;
            /* Out of both scans first: the store skips ".old-" folders and the
             * serving index skips names starting with '.'. */
            _snprintf_s(staged[i], MAX_PATH * 4, _TRUNCATE, "%s\\.old-%s-%lu", m.root, e->key, GetTickCount());
            from = sh_mb_wpath(e->folder);
            to[i] = sh_mb_wpath(staged[i]);
            if (!from || !to[i] || !MoveFileExW(from, to[i], 0)) {
                failed++;
                if (!first_fail[0]) _snprintf_s(first_fail, sizeof first_fail, _TRUNCATE, "%s could not be moved aside", e->key);
                removing[i] = 0;
                mu_log("remove %s: the folder could not be moved aside (error %lu)", e->key, GetLastError());
            } else {
                removed++;
                freed += e->bundle.total_size;
                mu_log("remove %s: moved aside (covered by a newer build)", e->key);
            }
            free(from);
        }
        if (removed) mu_rescan(note, sizeof note);
        for (i = 0; i < m.list.count; i++) {
            const sh_ms_entry *e = &m.list.items[i];
            if (!removing[i]) continue;
            if (!mu_remove_tree(to[i]))
                mu_log("remove %s: some files of the moved folder could not be deleted; the store ignores it", e->key);
            /* Blobs no remaining build references. Skipped when a folder in
             * the store could not be read, since its files are unknown. */
            if (!all_loaded) continue;
            for (k = 0; k < e->bundle.file_count; k++) {
                const char *blob = e->bundle.files[k].blob;
                wchar_t *w;
                size_t q;
                int dup = 0;
                if (e->bundle.files[k].stock) continue;   /* installed content: no blob in the store */
                if (!blob[0] || mu_blob_used_elsewhere(&m, blob, removing)) continue;
                for (q = 0; q < k && !dup; q++) dup = !strcmp(e->bundle.files[q].blob, blob);
                if (dup) continue;
                w = mu_blob_path(m.root, blob);
                if (w) {
                    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
                    if (DeleteFileW(w)) blobs++;
                }
                free(w);
            }
        }
        if (!removed && !failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Nothing removed: none of the chosen builds can be removed now.");
        else if (!failed) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %zu build(s).", removed);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "Removed %zu build(s); %zu could not be removed (%s).", removed, failed, first_fail);
        mu_log("remove: %zu build(s) removed, %zu failed, %zu blob(s) deleted", removed, failed, blobs);
        mu_result_head(&b, SH_MODS_ACT_PRUNE_APPLY, failed == 0 && removed > 0, msg);
        mu_puts(&b, ",\"removed\":[");
        for (i = 0, k = 0; i < m.list.count; i++) {
            if (!removing[i]) continue;
            if (k++) mu_putn(&b, ",", 1);
            mu_str(&b, m.list.items[i].key);
        }
        mu_putn(&b, "]", 1);
        mu_knum(&b, "bytes", freed, 0);
        mu_knum(&b, "blobs", (unsigned long long)blobs, 0);
        mu_kstr(&b, "index", note, 0);
    }
    for (i = 0; i < m.list.count; i++) free(to[i]);
    free(to);
    free(staged);
    free(c);
    free(removing);
    mu_map_free(&mp);
    mu_model_free(&m);
    return mu_result_tail(&b);
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
        char msg[200];
        note[0] = 0;
        n = mu_rescan(note, sizeof note);
        mu_log("rescan: %s", note);
        if (n >= 0) _snprintf_s(msg, sizeof msg, _TRUNCATE, "Mods folder reread: %s.", note);
        else _snprintf_s(msg, sizeof msg, _TRUNCATE, "Rescan failed: %s.", note);
        return mu_simple_result(action, n >= 0, msg);
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
}
#endif
