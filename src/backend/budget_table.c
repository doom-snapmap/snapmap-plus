/* budget_table.c -- see budget_table.h. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "budget_table.h"

/* ------------------------------------------------------------ text */

void sh_bt_buf_free(sh_bt_buf *b)
{
    if (!b) return;
    free(b->p);
    b->p = NULL; b->len = b->cap = 0; b->oom = 0;
}

static int bt_reserve(sh_bt_buf *b, size_t extra)
{
    size_t need, cap;
    char *p;
    if (b->oom) return 0;
    need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    p = (char *)realloc(b->p, cap);
    if (!p) { b->oom = 1; return 0; }
    b->p = p; b->cap = cap;
    return 1;
}

static void bt_putn(sh_bt_buf *b, const char *s, size_t n)
{
    if (!bt_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sh_bt_puts(sh_bt_buf *b, const char *s) { if (s) bt_putn(b, s, strlen(s)); }

void sh_bt_put_str(sh_bt_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    bt_putn(b, "\"", 1);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; bt_putn(b, e, 2); }
        else if (c < 0x20) { char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] }; bt_putn(b, e, 6); }
        else bt_putn(b, (const char *)&c, 1);
    }
    bt_putn(b, "\"", 1);
}

void sh_bt_put_int(sh_bt_buf *b, long long v)
{
    char n[32];
    _snprintf_s(n, sizeof n, _TRUNCATE, "%lld", v);
    sh_bt_puts(b, n);
}

void sh_bt_put_block(sh_bt_buf *b, const int block[SH_BT_FIELDS])
{
    int i;
    bt_putn(b, "[", 1);
    for (i = 0; i < SH_BT_FIELDS; ++i) {
        if (i) bt_putn(b, ",", 1);
        sh_bt_put_int(b, block[i]);
    }
    bt_putn(b, "]", 1);
}

/* ------------------------------------------------------------ arithmetic */

void sh_bt_base(int block[SH_BT_FIELDS])
{
    memset(block, 0, sizeof(int) * SH_BT_FIELDS);
    block[SH_BT_GENERAL_KB] = SH_BT_BASE_GENERAL;
}

void sh_bt_add(int block[SH_BT_FIELDS], const int add[SH_BT_FIELDS])
{
    int i;
    for (i = 0; i < SH_BT_FIELDS; ++i) block[i] += add[i];
}

void sh_bt_module_own(int floors, int walls, int portals, int render_kb, int collision_kb,
                      int general_kb, int out[SH_BT_FIELDS])
{
    memset(out, 0, sizeof(int) * SH_BT_FIELDS);
    out[SH_BT_RENDER_MODELS] = floors + 3 * walls + 2 * (portals + 2);
    out[8] = render_kb;
    out[9] = collision_kb;
    out[SH_BT_GENERAL_KB] = general_kb;
}

void sh_bt_add_miss(int block[SH_BT_FIELDS])
{
    block[SH_BT_ENTITIES] += 1;
    block[SH_BT_RENDER_MODELS] += 1;
    block[SH_BT_NETWORK] += 1;
}

unsigned sh_bt_filter_mask(unsigned decl_mask, unsigned sound_mask, unsigned auto_view_mask)
{
    return 1u | (decl_mask & sound_mask) | auto_view_mask;
}

void sh_bt_leaf(const char *decl_name, char *out, size_t cap)
{
    const char *s, *slash, *dot;
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!decl_name) return;
    s = decl_name;
    slash = strrchr(s, '/');
    if (slash) s = slash + 1;
    dot = strrchr(s, '.');
    n = (dot && !strcmp(dot, ".decl")) ? (size_t)(dot - s) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* ------------------------------------------------------------ table */

void sh_bt_table_init(sh_bt_table *t)
{
    memset(t, 0, sizeof *t);
}

void sh_bt_table_free(sh_bt_table *t)
{
    size_t i;
    if (!t) return;
    sh_bt_buf_free(&t->sedefs); sh_bt_buf_free(&t->inherits);
    sh_bt_buf_free(&t->modules); sh_bt_buf_free(&t->aliases);
    for (i = 0; i < t->seen_cap; ++i) free(t->seen[i]);
    free(t->seen);
    memset(t, 0, sizeof *t);
}

static size_t bt_hash(const char *s)
{
    size_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* Insert key (with a one-letter namespace) into the set. Returns 1 when it
 * was new, 0 when present or out of memory. */
static int bt_seen_insert(sh_bt_table *t, char ns, const char *key)
{
    size_t len = strlen(key), i, h;
    char *k;
    if (!t->seen || (size_t)(t->nsedef + t->nmodule + t->nalias + t->ninherit + 1) * 2 > t->seen_cap) {
        size_t cap = t->seen_cap ? t->seen_cap * 2 : 1024, j;
        char **n = (char **)calloc(cap, sizeof *n);
        if (!n) return 0;
        for (j = 0; j < t->seen_cap; ++j) {
            if (!t->seen[j]) continue;
            h = bt_hash(t->seen[j]) & (cap - 1);
            while (n[h]) h = (h + 1) & (cap - 1);
            n[h] = t->seen[j];
        }
        free(t->seen);
        t->seen = n; t->seen_cap = cap;
    }
    k = (char *)malloc(len + 2);
    if (!k) return 0;
    k[0] = ns; memcpy(k + 1, key, len + 1);
    h = bt_hash(k) & (t->seen_cap - 1);
    for (i = 0; i < t->seen_cap && t->seen[h]; ++i, h = (h + 1) & (t->seen_cap - 1)) {
        if (!strcmp(t->seen[h], k)) { free(k); return 0; }
    }
    if (t->seen[h]) { free(k); return 0; }
    t->seen[h] = k;
    return 1;
}

int sh_bt_table_add_sedef(sh_bt_table *t, const char *name, const char *inherit,
                          const int limits[SH_BT_FIELDS], int automation, int network_static)
{
    if (!t || !name || !*name) return 0;
    if (!bt_seen_insert(t, 's', name)) return 0;   /* a row name is keyed once */
    sh_bt_puts(&t->sedefs, t->nsedef++ ? "," : "");
    sh_bt_put_str(&t->sedefs, name);
    sh_bt_puts(&t->sedefs, ":{\"l\":");
    sh_bt_put_block(&t->sedefs, limits);
    sh_bt_puts(&t->sedefs, ",\"auto\":");
    sh_bt_put_int(&t->sedefs, automation);
    sh_bt_puts(&t->sedefs, ",\"ns\":");
    sh_bt_put_int(&t->sedefs, network_static ? 1 : 0);
    sh_bt_puts(&t->sedefs, "}");
    if (inherit && *inherit && bt_seen_insert(t, 'i', inherit)) {
        sh_bt_puts(&t->inherits, t->ninherit++ ? "," : "");
        sh_bt_put_str(&t->inherits, inherit);
        sh_bt_puts(&t->inherits, ":");
        sh_bt_put_str(&t->inherits, name);
        return 1;
    }
    return 0;
}

void sh_bt_parts_add(sh_bt_parts *ps, unsigned mask, int kind, const int block[SH_BT_FIELDS])
{
    int i;
    if (!ps || ps->overflow) return;
    for (i = 0; i < ps->count; ++i)
        if (ps->part[i].mask == mask && ps->part[i].kind == kind) { sh_bt_add(ps->part[i].block, block); return; }
    if (ps->count >= SH_BT_MAX_PARTS) { ps->overflow = 1; return; }
    ps->part[ps->count].mask = mask;
    ps->part[ps->count].kind = kind;
    memcpy(ps->part[ps->count].block, block, sizeof(int) * SH_BT_FIELDS);
    ++ps->count;
}

void sh_bt_table_add_module(sh_bt_table *t, const char *decl_name, const int own[SH_BT_FIELDS],
                            const int templ[SH_BT_FIELDS], const int view[SH_BT_FIELDS], unsigned mask,
                            const sh_bt_parts *parts)
{
    char leaf[260];
    if (!t || !decl_name || !*decl_name) return;
    if (!bt_seen_insert(t, 'm', decl_name)) return;
    sh_bt_puts(&t->modules, t->nmodule++ ? "," : "");
    sh_bt_put_str(&t->modules, decl_name);
    sh_bt_puts(&t->modules, ":{\"own\":");
    sh_bt_put_block(&t->modules, own);
    sh_bt_puts(&t->modules, ",\"templ\":");
    sh_bt_put_block(&t->modules, templ);
    sh_bt_puts(&t->modules, ",\"view\":");
    sh_bt_put_block(&t->modules, view);
    sh_bt_puts(&t->modules, ",\"mask\":");
    sh_bt_put_int(&t->modules, (long long)mask);
    if (parts && !parts->overflow) {
        int i;
        sh_bt_puts(&t->modules, ",\"parts\":[");
        for (i = 0; i < parts->count; ++i) {
            sh_bt_puts(&t->modules, i ? ",[" : "[");
            sh_bt_put_int(&t->modules, (long long)parts->part[i].mask);
            sh_bt_puts(&t->modules, ",");
            sh_bt_put_int(&t->modules, parts->part[i].kind);
            sh_bt_puts(&t->modules, ",");
            sh_bt_put_block(&t->modules, parts->part[i].block);
            sh_bt_puts(&t->modules, "]");
        }
        sh_bt_puts(&t->modules, "]");
    }
    sh_bt_puts(&t->modules, "}");
    sh_bt_leaf(decl_name, leaf, sizeof leaf);
    if (leaf[0] && bt_seen_insert(t, 'a', leaf)) {
        sh_bt_puts(&t->aliases, t->nalias++ ? "," : "");
        sh_bt_put_str(&t->aliases, leaf);
        sh_bt_puts(&t->aliases, ":");
        sh_bt_put_str(&t->aliases, decl_name);
    }
}

char *sh_bt_table_finish(sh_bt_table *t, const sh_bt_head *head)
{
    sh_bt_buf out = {0};
    static const int zero[SH_BT_FIELDS];
    if (!t || !head) return NULL;
    sh_bt_puts(&out, "{\"gen\":");
    sh_bt_put_int(&out, head->generation);
    sh_bt_puts(&out, ",\"limits\":");
    sh_bt_put_block(&out, head->limits ? head->limits : zero);
    sh_bt_puts(&out, ",\"usage\":");
    sh_bt_put_block(&out, head->usage ? head->usage : zero);
    sh_bt_puts(&out, ",\"reached\":");
    sh_bt_put_int(&out, head->reached);
    sh_bt_puts(&out, ",\"layers\":{\"autoView\":");
    sh_bt_put_int(&out, (long long)head->auto_view_mask);
    sh_bt_puts(&out, ",\"sound\":");
    sh_bt_put_int(&out, (long long)head->sound_mask);
    sh_bt_puts(&out, ",\"source\":");
    sh_bt_put_str(&out, head->layers_source ? head->layers_source : "fallback");
    sh_bt_puts(&out, "},\"sedefs\":{");
    if (t->sedefs.p) sh_bt_puts(&out, t->sedefs.p);
    sh_bt_puts(&out, "},\"inherits\":{");
    if (t->inherits.p) sh_bt_puts(&out, t->inherits.p);
    sh_bt_puts(&out, "},\"modules\":{");
    if (t->modules.p) sh_bt_puts(&out, t->modules.p);
    sh_bt_puts(&out, "},\"aliases\":{");
    if (t->aliases.p) sh_bt_puts(&out, t->aliases.p);
    sh_bt_puts(&out, "}}");
    if (out.oom || t->sedefs.oom || t->inherits.oom || t->modules.oom || t->aliases.oom) {
        sh_bt_buf_free(&out);
        return NULL;
    }
    return out.p;
}

int sh_bt_usage_json(char *out, int cap, const int limits[SH_BT_FIELDS], const int usage[SH_BT_FIELDS],
                     int reached, unsigned generation)
{
    sh_bt_buf b = {0};
    int n;
    sh_bt_puts(&b, "{\"limits\":");
    sh_bt_put_block(&b, limits);
    sh_bt_puts(&b, ",\"usage\":");
    sh_bt_put_block(&b, usage);
    sh_bt_puts(&b, ",\"reached\":");
    sh_bt_put_int(&b, reached);
    sh_bt_puts(&b, ",\"gen\":");
    sh_bt_put_int(&b, generation);
    sh_bt_puts(&b, "}");
    if (b.oom || !b.p) { sh_bt_buf_free(&b); return -1; }
    n = (int)b.len;
    if (!out || cap <= n) { sh_bt_buf_free(&b); return out ? -1 : n; }
    memcpy(out, b.p, b.len + 1);
    sh_bt_buf_free(&b);
    return n;
}

char *sh_bt_error_json(const char *message, unsigned generation)
{
    sh_bt_buf b = {0};
    sh_bt_puts(&b, "{\"gen\":");
    sh_bt_put_int(&b, generation);
    sh_bt_puts(&b, ",\"error\":");
    sh_bt_put_str(&b, message ? message : "unavailable");
    sh_bt_puts(&b, "}");
    if (b.oom) { sh_bt_buf_free(&b); return NULL; }
    return b.p;
}

#ifndef SH_BUDGET_TABLE_PURE
#include <windows.h>
#include "backend_log.h"

#define ED_INIT_OFF         0x8
#define ED_PALETTE_OFF      0x20660
#define ED_MODULES_OFF      0x206c0
#define ED_LIMITS_OFF       0x23670
#define ED_USAGE_OFF        0x236a0
#define ED_REACHED_OFF      0x236d0
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          0x6f0
#define PAL_MAX             100000
#define ENTRY_RECORD_OFF    0x150
#define ENTRY_STATIC_OFF    0x318
#define RECORD_INHERIT_OFF  0x58
#define SEDEF_NAME_OFF      0x08
#define SEDEF_AUTO_OFF      0x3c8
#define SEDEF_LIMITS_OFF    0x3d8
#define MP_POOL_OFF         0x68
#define MP_POOL_COUNT_OFF   0x70
#define WRAPPER_STRIDE      0x98
#define WRAPPER_MAX         4096
#define WRAPPER_CARD_OFF    0x80
#define WRAPPER_TEMPL_OFF   0x88
#define DECL_NAME_OFF       0x08
#define DECL_PORTALS_OFF    0xf0
#define DECL_LAYERS_OFF     0x100
#define CARD_FLOORS_OFF     0x118
#define CARD_WALLS_OFF      0x130
#define CARD_KB_OFF         0x174
#define TB_GAME_OFF         0x30
#define TB_VIEW_OFF         0x48
#define TB_MISS_OFF         0x60
#define TB_LIST_COUNT       0x08
#define TEMPLATE_MASK_OFF   0x158
#define MISS_STRIDE         0xa8
#define MISS_MASK_OFF       0x98
#define LIST_MAX            100000
#define LAYERS_SOUND_OFF    0x8c
#define LAYERS_AUTOVIEW_OFF 0x94
#define NAME_CAP            260
/* idDeclSnapEditorEntity::renderInfo (snapEntityRenderInfo_t), the model the
 * editor draws for a row: renderModelName is an idStr whose data pointer is at
 * +0x10, size an idVec3 at +0x94. */
#define SEDEF_RENDERINFO_OFF 0x1e0
#define RI_MODEL_DATA_OFF    0x10
#define RI_SIZE_OFF          0x94

typedef void *(*type_fn)(void *, const char *);
typedef void *(*find_decl_fn)(void *, const char *, unsigned char);

static const unsigned char *g_anchor;
static type_fn g_type;
static find_decl_fn g_decl;
static void *g_register_fn;

static SRWLOCK g_lock = SRWLOCK_INIT;
static char *g_table;                   /* under g_lock */
static size_t g_table_len;
static unsigned g_generation;           /* under g_lock */
static int g_limits[SH_BT_FIELDS], g_usage[SH_BT_FIELDS], g_reached, g_usage_ok;

/* Per inherit, the owning palette row and the model it draws (under g_lock). */
typedef struct bt_row_model { char *inherit, *sedef, *model; float size[3]; int have_size; } bt_row_model;
typedef struct bt_row_models { bt_row_model *row; int count, cap, with_model; } bt_row_models;
static bt_row_models g_rows;

static volatile LONG g_dirty = 1;
static void *g_built_editor;
static const void *g_built_entries, *g_built_pool;
static int g_built_count = -1, g_built_pool_count = -1;
static int g_logged;

static int rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static int rd_block(const void *p, int out[SH_BT_FIELDS])
{
    __try { memcpy(out, p, sizeof(int) * SH_BT_FIELDS); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int rd_str(const void *p, char *out, size_t cap)
{
    out[0] = 0;
    if (!p) return 0;
    __try { strncpy_s(out, cap, (const char *)p, _TRUNCATE); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
}

static int rd_floats(const void *p, float *out, int n)
{
    __try { memcpy(out, p, sizeof(float) * (size_t)n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void bt_rows_free(bt_row_models *rows)
{
    int i;
    for (i = 0; i < rows->count; ++i) {
        free(rows->row[i].inherit); free(rows->row[i].sedef); free(rows->row[i].model);
    }
    free(rows->row);
    memset(rows, 0, sizeof *rows);
}

static char *bt_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Record the row that owns `inherit` and its renderInfo model and size. A
 * model string that is not plain path text is dropped; the caller then falls
 * back to the installed sedef text. */
static void bt_rows_add(bt_row_models *rows, const char *inherit, const char *name,
                        const unsigned char *sedef)
{
    void *mp = NULL;
    char model[NAME_CAP];
    float size[3];
    int have_size, i, printable = 1;
    bt_row_model *r;
    model[0] = 0;
    if (rd_ptr(sedef + SEDEF_RENDERINFO_OFF + RI_MODEL_DATA_OFF, &mp) && mp)
        (void)rd_str(mp, model, sizeof model);
    for (i = 0; model[i]; ++i)
        if ((unsigned char)model[i] < 0x21 || (unsigned char)model[i] > 0x7e) { printable = 0; break; }
    if (!printable) model[0] = 0;
    have_size = rd_floats(sedef + SEDEF_RENDERINFO_OFF + RI_SIZE_OFF, size, 3);
    for (i = 0; have_size && i < 3; ++i)
        if (!(size[i] > 0.0f && size[i] < 100000.0f)) have_size = 0;
    if (rows->count == rows->cap) {
        int cap = rows->cap ? rows->cap * 2 : 512;
        bt_row_model *n = (bt_row_model *)realloc(rows->row, (size_t)cap * sizeof *n);
        if (!n) return;
        rows->row = n; rows->cap = cap;
    }
    r = &rows->row[rows->count];
    memset(r, 0, sizeof *r);
    r->inherit = bt_dup(inherit);
    r->sedef = bt_dup(name);
    r->model = bt_dup(model);
    if (!r->inherit || !r->sedef || !r->model) {
        free(r->inherit); free(r->sedef); free(r->model);
        return;
    }
    if (have_size) { memcpy(r->size, size, sizeof size); r->have_size = 1; }
    if (model[0]) rows->with_model++;
    rows->count++;
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

int sh_budget_table_install(const sig_result *results, size_t count)
{
    g_anchor = (const unsigned char *)clean(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl = (find_decl_fn)clean(results, count, "DeclFind");
    g_register_fn = clean(results, count, "DeclRegisterFile");
    if (!g_anchor || !g_type || !g_decl || !g_register_fn) {
        backend_log("BUDGET: snapLayers lookup unavailable; module filters fall back to every layer");
        return 0;
    }
    return 1;
}

void sh_budget_table_palette_rebuilt(void)
{
    InterlockedExchange(&g_dirty, 1);
}

/* The decl registry, as timeline_tile.c decodes it from its anchor. */
static void *bt_registry(void)
{
    int32_t displacement;
    void *r;
    void **vt;
    if (!g_anchor || memcmp(g_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
    memcpy(&displacement, g_anchor + 0x13, 4);
    r = *(void **)(g_anchor + 0x17 + displacement);
    if (!r) return NULL;
    vt = *(void ***)r;
    if (!vt || vt[7] != g_register_fn || vt[11] != (void *)g_type) return NULL;
    return r;
}

/* snaplayers/layers.decl, the one idDeclSnapLayers the editor reads through
 * its type's first decl. Its reflection has no shift on this build. */
static int bt_layers(unsigned *auto_view, unsigned *sound)
{
    void *r, *manager, *decl = NULL;
    int av = 0, snd = 0;
    __try {
        r = bt_registry();
        if (!r) return 0;
        manager = g_type(r, "snapLayers");
        if (!manager) return 0;
        decl = g_decl(manager, "layers", 0);
        if (!decl) return 0;
        av = *(const int *)((const unsigned char *)decl + LAYERS_AUTOVIEW_OFF);
        snd = *(const int *)((const unsigned char *)decl + LAYERS_SOUND_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!av) return 0;
    *auto_view = (unsigned)av;
    *sound = (unsigned)snd;
    return 1;
}

static void bt_sum_templates(const unsigned char *list, unsigned filter, int view, int out[SH_BT_FIELDS],
                             sh_bt_parts *parts)
{
    void *data = NULL;
    int count = 0, i;
    if (!rd_ptr(list, &data) || !rd_i32(list + TB_LIST_COUNT, &count) || !data || count <= 0 || count > LIST_MAX)
        return;
    for (i = 0; i < count; ++i) {
        const unsigned char *t = (const unsigned char *)data + (size_t)i * PAL_STRIDE;
        void *sedef = NULL;
        int mask = 0, l[SH_BT_FIELDS];
        if (!rd_ptr(t, &sedef) || !sedef || !rd_i32(t + TEMPLATE_MASK_OFF, &mask)) continue;
        if (!rd_block((const unsigned char *)sedef + SEDEF_LIMITS_OFF, l)) continue;
        /* VIEW templates become room entities, which the entity rule counts
         * with the network-static byte; GAME templates never subtract it. */
        if (view && t[ENTRY_STATIC_OFF]) l[SH_BT_NETWORK] = 0;
        sh_bt_parts_add(parts, (unsigned)mask, view ? SH_BT_PART_VIEW : SH_BT_PART_GAME, l);
        if ((unsigned)mask & filter) sh_bt_add(out, l);
    }
}

static void bt_build(unsigned char *ed, sh_bt_table *t, sh_bt_head *head, int limits[SH_BT_FIELDS],
                     int usage[SH_BT_FIELDS], const char **error, bt_row_models *rows)
{
    unsigned char *pal = ed + ED_PALETTE_OFF, *mp = ed + ED_MODULES_OFF;
    void *entries = NULL, *pool = NULL;
    int count = 0, pool_count = 0, i;
    unsigned auto_view = 0, sound = 0;
    int have_layers;
    *error = NULL;
    if (!rd_block(ed + ED_LIMITS_OFF, limits) || !rd_block(ed + ED_USAGE_OFF, usage)) { *error = "limits unreadable"; return; }
    head->reached = ed[ED_REACHED_OFF];
    have_layers = bt_layers(&auto_view, &sound);
    head->auto_view_mask = have_layers ? auto_view : 0xffffffffu;
    head->sound_mask = have_layers ? sound : 0xffffffffu;
    head->layers_source = have_layers ? "decl" : "fallback";
    if (!rd_ptr(pal + PAL_ENTRIES_OFF, &entries) || !rd_i32(pal + PAL_COUNT_OFF, &count) ||
        !entries || count <= 0 || count > PAL_MAX) { *error = "entity palette unreadable"; return; }
    g_built_entries = entries; g_built_count = count;
    for (i = 0; i < count; ++i) {
        const unsigned char *e = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        void *sedef = NULL, *np = NULL, *rec = NULL, *ip = NULL;
        char name[NAME_CAP], inherit[NAME_CAP];
        int l[SH_BT_FIELDS], automation = 0;
        if (!rd_ptr(e, &sedef) || !sedef || !rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &np) ||
            !rd_str(np, name, sizeof name) || !name[0])
            continue;
        if (!rd_block((unsigned char *)sedef + SEDEF_LIMITS_OFF, l)) continue;
        (void)rd_i32((unsigned char *)sedef + SEDEF_AUTO_OFF, &automation);
        inherit[0] = 0;
        if (rd_ptr(e + ENTRY_RECORD_OFF, &rec) && rec && rd_ptr((unsigned char *)rec + RECORD_INHERIT_OFF, &ip))
            (void)rd_str(ip, inherit, sizeof inherit);
        if (sh_bt_table_add_sedef(t, name, inherit, l, automation, e[ENTRY_STATIC_OFF] != 0))
            bt_rows_add(rows, inherit, name, (const unsigned char *)sedef);
    }
    if (!rd_ptr(mp + MP_POOL_OFF, &pool) || !rd_i32(mp + MP_POOL_COUNT_OFF, &pool_count) ||
        pool_count < 0 || pool_count > WRAPPER_MAX) { *error = "module palette unreadable"; return; }
    g_built_pool = pool; g_built_pool_count = pool_count;
    for (i = 0; pool && i < pool_count; ++i) {
        const unsigned char *w = (const unsigned char *)pool + (size_t)i * WRAPPER_STRIDE;
        void *decl = NULL, *card = NULL, *tb = NULL, *np = NULL, *misses = NULL;
        char name[NAME_CAP];
        int floors = 0, walls = 0, portals = 0, kb[3] = {0, 0, 0}, decl_mask = 0, nmiss = 0, k;
        int own[SH_BT_FIELDS], templ[SH_BT_FIELDS], view[SH_BT_FIELDS];
        static sh_bt_parts parts;   /* game thread only; too large for the stack */
        unsigned filter;
        if (!rd_ptr(w, &decl) || !decl || !rd_ptr((unsigned char *)decl + DECL_NAME_OFF, &np) ||
            !rd_str(np, name, sizeof name) || !name[0])
            continue;
        if (!rd_ptr(w + WRAPPER_CARD_OFF, &card) || !card || !rd_ptr(w + WRAPPER_TEMPL_OFF, &tb) || !tb) continue;
        if (!rd_i32((unsigned char *)card + CARD_FLOORS_OFF, &floors) ||
            !rd_i32((unsigned char *)card + CARD_WALLS_OFF, &walls) ||
            !rd_i32((unsigned char *)decl + DECL_PORTALS_OFF, &portals) ||
            !rd_i32((unsigned char *)decl + DECL_LAYERS_OFF, &decl_mask))
            continue;
        for (k = 0; k < 3; ++k) (void)rd_i32((unsigned char *)card + CARD_KB_OFF + 4 * k, &kb[k]);
        sh_bt_module_own(floors, walls, portals, kb[0], kb[1], kb[2], own);
        filter = have_layers ? sh_bt_filter_mask((unsigned)decl_mask, sound, auto_view) : 0xffffffffu;
        sh_bt_base(templ);
        memset(view, 0, sizeof view);
        parts.count = parts.overflow = 0;
        bt_sum_templates((unsigned char *)tb + TB_GAME_OFF, filter, 0, templ, &parts);
        bt_sum_templates((unsigned char *)tb + TB_VIEW_OFF, filter, 1, view, &parts);
        if (rd_ptr((unsigned char *)tb + TB_MISS_OFF, &misses) &&
            rd_i32((unsigned char *)tb + TB_MISS_OFF + TB_LIST_COUNT, &nmiss) && misses && nmiss > 0 && nmiss <= LIST_MAX) {
            for (k = 0; k < nmiss; ++k) {
                int mask = 0;
                int one[SH_BT_FIELDS];
                if (!rd_i32((unsigned char *)misses + (size_t)k * MISS_STRIDE + MISS_MASK_OFF, &mask)) continue;
                memset(one, 0, sizeof one);
                sh_bt_add_miss(one);
                sh_bt_parts_add(&parts, (unsigned)mask, SH_BT_PART_MISS, one);
                if ((unsigned)mask & filter) sh_bt_add_miss(templ);
            }
        }
        sh_bt_table_add_module(t, name, own, templ, view, filter, &parts);
    }
    if (!g_logged) {
        g_logged = 1;
        {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "BUDGET: table built: %d rows, %d modules, layers %s (autoView 0x%x, sound 0x%x); "
                        "%d of %d inherits draw a renderInfo model",
                        t->nsedef, t->nmodule, head->layers_source, head->auto_view_mask, head->sound_mask,
                        rows->with_model, rows->count);
            backend_log(line);
        }
    }
}

static int bt_palette_moved(unsigned char *ed)
{
    void *entries = NULL, *pool = NULL;
    int count = 0, pool_count = 0;
    if (!rd_ptr(ed + ED_PALETTE_OFF + PAL_ENTRIES_OFF, &entries) || !rd_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, &count) ||
        !rd_ptr(ed + ED_MODULES_OFF + MP_POOL_OFF, &pool) || !rd_i32(ed + ED_MODULES_OFF + MP_POOL_COUNT_OFF, &pool_count))
        return 0;
    return entries != g_built_entries || count != g_built_count || pool != g_built_pool || pool_count != g_built_pool_count;
}

void sh_budget_table_tick(void *editor)
{
    unsigned char *ed = (unsigned char *)editor;
    int limits[SH_BT_FIELDS], usage[SH_BT_FIELDS], reached = 0;
    if (!ed) return;
    /* The usage block is current after the engine's frame. */
    if (rd_block(ed + ED_LIMITS_OFF, limits) && rd_block(ed + ED_USAGE_OFF, usage)) {
        __try { reached = ed[ED_REACHED_OFF]; } __except (EXCEPTION_EXECUTE_HANDLER) { reached = 0; }
        AcquireSRWLockExclusive(&g_lock);
        memcpy(g_limits, limits, sizeof limits);
        memcpy(g_usage, usage, sizeof usage);
        g_reached = reached;
        g_usage_ok = 1;
        ReleaseSRWLockExclusive(&g_lock);
    }
    __try { if (ed[ED_INIT_OFF] != 1) return; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!InterlockedCompareExchange(&g_dirty, 0, 0) && ed == g_built_editor && !bt_palette_moved(ed)) return;
    {
        sh_bt_table t;
        sh_bt_head head;
        bt_row_models rows;
        const char *error = NULL;
        char *json;
        unsigned gen;
        InterlockedExchange(&g_dirty, 0);
        g_built_editor = ed;
        sh_bt_table_init(&t);
        memset(&head, 0, sizeof head);
        memset(&rows, 0, sizeof rows);
        AcquireSRWLockShared(&g_lock);
        gen = g_generation + 1;
        ReleaseSRWLockShared(&g_lock);
        if (!gen) gen = 1;
        head.generation = gen;
        __try { bt_build(ed, &t, &head, limits, usage, &error, &rows); }
        __except (EXCEPTION_EXECUTE_HANDLER) { error = "the palette walk faulted"; }
        head.limits = limits; head.usage = usage;
        json = error ? sh_bt_error_json(error, gen) : sh_bt_table_finish(&t, &head);
        sh_bt_table_free(&t);
        if (error) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE, "BUDGET: table unavailable: %s", error);
            backend_log(line);
        }
        if (!json) { bt_rows_free(&rows); return; }
        AcquireSRWLockExclusive(&g_lock);
        free(g_table);
        g_table = json;
        g_table_len = strlen(json);
        g_generation = gen;
        if (!error) { bt_row_models old = g_rows; g_rows = rows; rows = old; }
        ReleaseSRWLockExclusive(&g_lock);
        bt_rows_free(&rows);
    }
}

int sh_budget_table_row_model(const char *inherit, char *sedef, size_t sedef_cap,
                              char *model, size_t model_cap, float size[3])
{
    int i, flags = 0;
    if (sedef && sedef_cap) sedef[0] = 0;
    if (model && model_cap) model[0] = 0;
    if (!inherit || !*inherit) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_rows.count; ++i) {
        const bt_row_model *r = &g_rows.row[i];
        if (_stricmp(r->inherit, inherit)) continue;
        flags = SH_BT_ROW_FOUND;
        if (sedef && sedef_cap) strncpy_s(sedef, sedef_cap, r->sedef, _TRUNCATE);
        if (r->model[0] && model && model_cap > strlen(r->model)) {
            memcpy(model, r->model, strlen(r->model) + 1);
            flags |= SH_BT_ROW_MODEL;
        }
        if (r->have_size && size) { memcpy(size, r->size, sizeof r->size); flags |= SH_BT_ROW_SIZE; }
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return flags;
}

int sh_budget_table_slot(struct sh_iface *self, sh_budget_call *call)
{
    int r = 0;
    (void)self;
    if (!call) return 0;
    call->needed = 0;
    AcquireSRWLockShared(&g_lock);
    call->generation = g_generation;
    call->reached = g_reached;
    if (call->op == SH_BUDGET_TABLE && g_table) {
        call->needed = (int)g_table_len;
        if (!call->out || call->out_cap <= (int)g_table_len) r = -1;
        else { memcpy(call->out, g_table, g_table_len + 1); r = 1; }
    } else if (call->op == SH_BUDGET_USAGE && g_usage_ok) {
        int n = sh_bt_usage_json(NULL, 0, g_limits, g_usage, g_reached, g_generation);
        call->needed = n < 0 ? 0 : n;
        if (n < 0) r = 0;
        else if (!call->out || call->out_cap <= n) r = -1;
        else r = sh_bt_usage_json(call->out, call->out_cap, g_limits, g_usage, g_reached, g_generation) >= 0 ? 1 : 0;
    }
    ReleaseSRWLockShared(&g_lock);
    return r;
}
#endif
