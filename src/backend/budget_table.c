/* budget_table.c -- see budget_table.h. */
#include "safe_read.h"
#include "../common/text.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "budget_table.h"

void sh_bt_buf_free(sh_bt_buf *b)
{
    if (!b) return;
    free(b->p);
    b->p = NULL; b->n = b->cap = 0; b->oom = 0;
}

void sh_bt_puts(sh_bt_buf *b, const char *s) { if (s) sh_text_append(b, s, strlen(s)); }

void sh_bt_put_str(sh_bt_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    sh_text_append(b, "\"", 1);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sh_text_append(b, e, 2); }
        else if (c < 0x20) { char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] }; sh_text_append(b, e, 6); }
        else sh_text_append(b, (const char *)&c, 1);
    }
    sh_text_append(b, "\"", 1);
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
    sh_text_append(b, "[", 1);
    for (i = 0; i < SH_BT_FIELDS; ++i) {
        if (i) sh_text_append(b, ",", 1);
        sh_bt_put_int(b, block[i]);
    }
    sh_text_append(b, "]", 1);
}

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

typedef struct bt_sedef {
    struct bt_sedef *next;
    const char *name, *inherit;
    int limits[SH_BT_FIELDS], automation, network_static;
} bt_sedef;

typedef struct bt_module {
    struct bt_module *next;
    const char *name, *alias;
    int own[SH_BT_FIELDS], templ[SH_BT_FIELDS], view[SH_BT_FIELDS];
    unsigned mask;
    int nparts;
    sh_bt_part parts[];
} bt_module;

void sh_bt_table_init(sh_bt_table *t)
{
    memset(t, 0, sizeof *t);
}

void sh_bt_table_free(sh_bt_table *t)
{
    size_t i;
    if (!t) return;
    while (t->rows) {
        bt_sedef *next = t->rows->next;
        free(t->rows);
        t->rows = next;
    }
    while (t->modules) {
        bt_module *next = t->modules->next;
        free(t->modules);
        t->modules = next;
    }
    for (i = 0; i < t->seen_cap; ++i) free(t->seen[i]);
    free(t->seen);
    memset(t, 0, sizeof *t);
}

/* The set owns each unique name until the snapshot is freed. */
static const char *bt_seen_insert(sh_bt_table *t, char ns, const char *key)
{
    size_t len = strlen(key), i, h;
    char *k;
    if (!t->seen || (size_t)(t->nsedef + t->nmodule + t->nalias + t->ninherit + 1) * 2 > t->seen_cap) {
        size_t cap = t->seen_cap ? t->seen_cap * 2 : 1024, j;
        char **n = (char **)calloc(cap, sizeof *n);
        if (!n) { t->oom = 1; return NULL; }
        for (j = 0; j < t->seen_cap; ++j) {
            if (!t->seen[j]) continue;
            h = sh_text_hash(t->seen[j], strlen(t->seen[j])) & (cap - 1);
            while (n[h]) h = (h + 1) & (cap - 1);
            n[h] = t->seen[j];
        }
        free(t->seen);
        t->seen = n; t->seen_cap = cap;
    }
    k = (char *)malloc(len + 2);
    if (!k) { t->oom = 1; return NULL; }
    k[0] = ns; memcpy(k + 1, key, len + 1);
    h = sh_text_hash(k, strlen(k)) & (t->seen_cap - 1);
    for (i = 0; i < t->seen_cap && t->seen[h]; ++i, h = (h + 1) & (t->seen_cap - 1)) {
        if (!strcmp(t->seen[h], k)) { free(k); return 0; }
    }
    if (t->seen[h]) { free(k); return 0; }
    t->seen[h] = k;
    return k + 1;
}

int sh_bt_table_add_sedef(sh_bt_table *t, const char *name, const char *inherit,
                          const int limits[SH_BT_FIELDS], int automation, int network_static)
{
    bt_sedef *row;
    if (!t || !name || !*name || t->oom) return 0;
    name = bt_seen_insert(t, 's', name);
    if (!name) return 0;
    row = (bt_sedef *)calloc(1, sizeof *row);
    if (!row) { t->oom = 1; return 0; }
    row->name = name;
    row->inherit = inherit && *inherit ? bt_seen_insert(t, 'i', inherit) : NULL;
    row->automation = automation;
    row->network_static = network_static != 0;
    memcpy(row->limits, limits, sizeof row->limits);
    if (t->last_row) t->last_row->next = row;
    else t->rows = row;
    t->last_row = row;
    t->nsedef++;
    if (row->inherit) t->ninherit++;
    return row->inherit != NULL;
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
    bt_module *m;
    int n = parts && !parts->overflow ? parts->count : -1;
    if (!t || !decl_name || !*decl_name || t->oom) return;
    if (n < -1 || n > SH_BT_MAX_PARTS) { t->oom = 1; return; }
    decl_name = bt_seen_insert(t, 'm', decl_name);
    if (!decl_name) return;
    m = (bt_module *)calloc(1, sizeof *m + (n > 0 ? n * sizeof *m->parts : 0));
    if (!m) { t->oom = 1; return; }
    m->name = decl_name;
    m->mask = mask;
    m->nparts = n;
    memcpy(m->own, own, sizeof m->own);
    memcpy(m->templ, templ, sizeof m->templ);
    memcpy(m->view, view, sizeof m->view);
    if (n > 0) memcpy(m->parts, parts->part, n * sizeof *m->parts);
    sh_bt_leaf(decl_name, leaf, sizeof leaf);
    if (leaf[0]) m->alias = bt_seen_insert(t, 'a', leaf);
    if (t->last_module) t->last_module->next = m;
    else t->modules = m;
    t->last_module = m;
    t->nmodule++;
    if (m->alias) t->nalias++;
}

static void bt_put_module(sh_bt_buf *out, const bt_module *m)
{
    sh_bt_put_str(out, m->name);
    sh_bt_puts(out, ":{\"own\":");
    sh_bt_put_block(out, m->own);
    sh_bt_puts(out, ",\"templ\":");
    sh_bt_put_block(out, m->templ);
    sh_bt_puts(out, ",\"view\":");
    sh_bt_put_block(out, m->view);
    sh_bt_puts(out, ",\"mask\":");
    sh_bt_put_int(out, (long long)m->mask);
    if (m->nparts >= 0) {
        int i;
        sh_bt_puts(out, ",\"parts\":[");
        for (i = 0; i < m->nparts; ++i) {
            sh_bt_puts(out, i ? ",[" : "[");
            sh_bt_put_int(out, (long long)m->parts[i].mask);
            sh_bt_puts(out, ",");
            sh_bt_put_int(out, m->parts[i].kind);
            sh_bt_puts(out, ",");
            sh_bt_put_block(out, m->parts[i].block);
            sh_bt_puts(out, "]");
        }
        sh_bt_puts(out, "]");
    }
    sh_bt_puts(out, "}");
}

char *sh_bt_table_finish(sh_bt_table *t, const sh_bt_head *head)
{
    sh_bt_buf out = {0};
    static const int zero[SH_BT_FIELDS];
    const bt_sedef *row;
    const bt_module *m;
    int written = 0;
    if (!t || !head || t->oom) return NULL;
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
    for (row = t->rows; row; row = row->next) {
        if (row != t->rows) sh_bt_puts(&out, ",");
        sh_bt_put_str(&out, row->name);
        sh_bt_puts(&out, ":{\"l\":");
        sh_bt_put_block(&out, row->limits);
        sh_bt_puts(&out, ",\"auto\":");
        sh_bt_put_int(&out, row->automation);
        sh_bt_puts(&out, ",\"ns\":");
        sh_bt_put_int(&out, row->network_static);
        sh_bt_puts(&out, "}");
    }
    sh_bt_puts(&out, "},\"inherits\":{");
    for (row = t->rows; row; row = row->next) if (row->inherit) {
        if (written++) sh_bt_puts(&out, ",");
        sh_bt_put_str(&out, row->inherit);
        sh_bt_puts(&out, ":");
        sh_bt_put_str(&out, row->name);
    }
    sh_bt_puts(&out, "},\"modules\":{");
    for (m = t->modules; m; m = m->next) {
        if (m != t->modules) sh_bt_puts(&out, ",");
        bt_put_module(&out, m);
    }
    sh_bt_puts(&out, "},\"aliases\":{");
    written = 0;
    for (m = t->modules; m; m = m->next) if (m->alias) {
        if (written++) sh_bt_puts(&out, ",");
        sh_bt_put_str(&out, m->alias);
        sh_bt_puts(&out, ":");
        sh_bt_put_str(&out, m->name);
    }
    sh_bt_puts(&out, "}}");
    if (out.oom) {
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
    n = (int)b.n;
    if (!out || cap <= n) { sh_bt_buf_free(&b); return out ? -1 : n; }
    memcpy(out, b.p, b.n + 1);
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
/* idDeclSnapEditorEntity::renderInfo (snapEntityRenderInfo_t), the model the editor draws for a row:
 * renderModelName is an idStr whose data pointer is at +0x10, size an idVec3 at +0x94. */
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
typedef struct bt_snapshot {
    LONG refs;
    SRWLOCK render_lock;
    sh_bt_table table;
    sh_bt_head head;
    int limits[SH_BT_FIELDS], usage[SH_BT_FIELDS];
    const char *error;
    char *json;
    size_t json_len;
} bt_snapshot;
static bt_snapshot *g_snapshot;          /* under g_lock */
static unsigned g_generation;           /* under g_lock */
static int g_limits[SH_BT_FIELDS], g_usage[SH_BT_FIELDS], g_reached, g_usage_ok;

/* Per inherit, the owning palette row and the model it draws (under g_lock). */
typedef struct bt_row_model { char *inherit, *sedef, *model; float size[3]; int have_size; } bt_row_model;
typedef struct bt_row_models { bt_row_model *row; int count, cap, with_model; } bt_row_models;
static bt_row_models g_rows;

static volatile LONG g_dirty = 1;
/* Time of the editor frame's last read of the usage block. The frame stops when the player leaves the editor, so a
 * reading older than BT_USAGE_FRESH_MS is not reported. */
#define BT_USAGE_FRESH_MS 1000u
static volatile LONG64 g_usage_at;
static void *g_built_editor;
static const void *g_built_entries, *g_built_pool;
static int g_built_count = -1, g_built_pool_count = -1;
static int g_logged;

static void bt_snapshot_release(bt_snapshot *s)
{
    if (!s || InterlockedDecrement(&s->refs)) return;
    sh_bt_table_free(&s->table);
    free(s->json);
    free(s);
}

/* A reader holds this palette's snapshot while the game can publish its next generation. */
static char *bt_snapshot_json(bt_snapshot *s)
{
    char *copy;
    AcquireSRWLockExclusive(&s->render_lock);
    if (!s->json) {
        s->json = s->error ? sh_bt_error_json(s->error, s->head.generation) : sh_bt_table_finish(&s->table, &s->head);
        if (s->json) {
            s->json_len = strlen(s->json);
            sh_bt_table_free(&s->table);
        }
    }
    copy = s->json ? sh_iface_textn(s->json, s->json_len) : NULL;
    ReleaseSRWLockExclusive(&s->render_lock);
    return copy;
}

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

/* Records the row that owns `inherit` and its renderInfo model and size. A model string that is not plain path text
 * is dropped. */
static void bt_rows_add(bt_row_models *rows, const char *inherit, const char *name,
                        const unsigned char *sedef)
{
    void *mp = NULL;
    char model[NAME_CAP];
    float size[3];
    int have_size, i, printable = 1;
    bt_row_model *r;
    model[0] = 0;
    if (sh_read_ptr(sedef + SEDEF_RENDERINFO_OFF + RI_MODEL_DATA_OFF, &mp) && mp)
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
    r->inherit = sh_text(inherit);
    r->sedef = sh_text(name);
    r->model = sh_text(model);
    if (!r->inherit || !r->sedef || !r->model) {
        free(r->inherit); free(r->sedef); free(r->model);
        return;
    }
    if (have_size) { memcpy(r->size, size, sizeof size); r->have_size = 1; }
    if (model[0]) rows->with_model++;
    rows->count++;
}

int sh_budget_table_install(const sig_result *results, size_t count)
{
    g_anchor = (const unsigned char *)sig_pointer(results, count, "DeclRegistryAnchor");
    g_type = (type_fn)sig_pointer(results, count, "DeclTypeByName");
    g_decl = (find_decl_fn)sig_pointer(results, count, "DeclFind");
    g_register_fn = sig_pointer(results, count, "DeclRegisterFile");
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

/* Decodes the decl registry from its anchor. */
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

/* Reads the masks of snaplayers/layers.decl, the one idDeclSnapLayers the editor reads through its type's first
 * decl. */
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
    if (!sh_read_ptr(list, &data) || !sh_read_i32(list + TB_LIST_COUNT, &count) || !data || count <= 0 || count > LIST_MAX)
        return;
    for (i = 0; i < count; ++i) {
        const unsigned char *t = (const unsigned char *)data + (size_t)i * PAL_STRIDE;
        void *sedef = NULL;
        int mask = 0, l[SH_BT_FIELDS];
        if (!sh_read_ptr(t, &sedef) || !sedef || !sh_read_i32(t + TEMPLATE_MASK_OFF, &mask)) continue;
        if (!rd_block((const unsigned char *)sedef + SEDEF_LIMITS_OFF, l)) continue;
        /* VIEW templates become room entities, counted with the network-static byte; GAME templates never subtract
         * it. */
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
    if (!sh_read_ptr(pal + PAL_ENTRIES_OFF, &entries) || !sh_read_i32(pal + PAL_COUNT_OFF, &count) ||
        !entries || count <= 0 || count > PAL_MAX) { *error = "entity palette unreadable"; return; }
    g_built_entries = entries; g_built_count = count;
    for (i = 0; i < count; ++i) {
        const unsigned char *e = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        void *sedef = NULL, *np = NULL, *rec = NULL, *ip = NULL;
        char name[NAME_CAP], inherit[NAME_CAP];
        int l[SH_BT_FIELDS], automation = 0;
        if (!sh_read_ptr(e, &sedef) || !sedef || !sh_read_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &np) ||
            !rd_str(np, name, sizeof name) || !name[0])
            continue;
        if (!rd_block((unsigned char *)sedef + SEDEF_LIMITS_OFF, l)) continue;
        (void)sh_read_i32((unsigned char *)sedef + SEDEF_AUTO_OFF, &automation);
        inherit[0] = 0;
        if (sh_read_ptr(e + ENTRY_RECORD_OFF, &rec) && rec && sh_read_ptr((unsigned char *)rec + RECORD_INHERIT_OFF, &ip))
            (void)rd_str(ip, inherit, sizeof inherit);
        if (sh_bt_table_add_sedef(t, name, inherit, l, automation, e[ENTRY_STATIC_OFF] != 0))
            bt_rows_add(rows, inherit, name, (const unsigned char *)sedef);
    }
    if (!sh_read_ptr(mp + MP_POOL_OFF, &pool) || !sh_read_i32(mp + MP_POOL_COUNT_OFF, &pool_count) ||
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
        if (!sh_read_ptr(w, &decl) || !decl || !sh_read_ptr((unsigned char *)decl + DECL_NAME_OFF, &np) ||
            !rd_str(np, name, sizeof name) || !name[0])
            continue;
        if (!sh_read_ptr(w + WRAPPER_CARD_OFF, &card) || !card || !sh_read_ptr(w + WRAPPER_TEMPL_OFF, &tb) || !tb) continue;
        if (!sh_read_i32((unsigned char *)card + CARD_FLOORS_OFF, &floors) ||
            !sh_read_i32((unsigned char *)card + CARD_WALLS_OFF, &walls) ||
            !sh_read_i32((unsigned char *)decl + DECL_PORTALS_OFF, &portals) ||
            !sh_read_i32((unsigned char *)decl + DECL_LAYERS_OFF, &decl_mask))
            continue;
        for (k = 0; k < 3; ++k) (void)sh_read_i32((unsigned char *)card + CARD_KB_OFF + 4 * k, &kb[k]);
        sh_bt_module_own(floors, walls, portals, kb[0], kb[1], kb[2], own);
        filter = have_layers ? sh_bt_filter_mask((unsigned)decl_mask, sound, auto_view) : 0xffffffffu;
        sh_bt_base(templ);
        memset(view, 0, sizeof view);
        parts.count = parts.overflow = 0;
        bt_sum_templates((unsigned char *)tb + TB_GAME_OFF, filter, 0, templ, &parts);
        bt_sum_templates((unsigned char *)tb + TB_VIEW_OFF, filter, 1, view, &parts);
        if (sh_read_ptr((unsigned char *)tb + TB_MISS_OFF, &misses) &&
            sh_read_i32((unsigned char *)tb + TB_MISS_OFF + TB_LIST_COUNT, &nmiss) && misses && nmiss > 0 && nmiss <= LIST_MAX) {
            for (k = 0; k < nmiss; ++k) {
                int mask = 0;
                int one[SH_BT_FIELDS];
                if (!sh_read_i32((unsigned char *)misses + (size_t)k * MISS_STRIDE + MISS_MASK_OFF, &mask)) continue;
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
    if (!sh_read_ptr(ed + ED_PALETTE_OFF + PAL_ENTRIES_OFF, &entries) || !sh_read_i32(ed + ED_PALETTE_OFF + PAL_COUNT_OFF, &count) ||
        !sh_read_ptr(ed + ED_MODULES_OFF + MP_POOL_OFF, &pool) || !sh_read_i32(ed + ED_MODULES_OFF + MP_POOL_COUNT_OFF, &pool_count))
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
        InterlockedExchange64(&g_usage_at, (LONG64)GetTickCount64());
    }
    __try { if (ed[ED_INIT_OFF] != 1) return; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!InterlockedCompareExchange(&g_dirty, 0, 0) && ed == g_built_editor && !bt_palette_moved(ed)) return;
    {
        bt_snapshot *snapshot = (bt_snapshot *)calloc(1, sizeof *snapshot), *old;
        bt_row_models rows;
        unsigned gen;
        if (!snapshot) return;
        snapshot->refs = 1;
        InterlockedExchange(&g_dirty, 0);
        g_built_editor = ed;
        memset(&rows, 0, sizeof rows);
        AcquireSRWLockShared(&g_lock);
        gen = g_generation + 1;
        ReleaseSRWLockShared(&g_lock);
        if (!gen) gen = 1;
        snapshot->head.generation = gen;
        snapshot->head.limits = snapshot->limits;
        snapshot->head.usage = snapshot->usage;
        __try { bt_build(ed, &snapshot->table, &snapshot->head, snapshot->limits,
                         snapshot->usage, &snapshot->error, &rows); }
        __except (EXCEPTION_EXECUTE_HANDLER) { snapshot->error = "the palette walk faulted"; }
        if (snapshot->table.oom) snapshot->error = "the budget table ran out of memory";
        if (snapshot->error) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE, "BUDGET: table unavailable: %s", snapshot->error);
            backend_log(line);
        }
        AcquireSRWLockExclusive(&g_lock);
        old = g_snapshot;
        g_snapshot = snapshot;
        g_generation = gen;
        if (!snapshot->error) { bt_row_models prior = g_rows; g_rows = rows; rows = prior; }
        ReleaseSRWLockExclusive(&g_lock);
        bt_snapshot_release(old);
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
    int r = 0, fresh, usage_ready = 0;
    int limits[SH_BT_FIELDS], usage[SH_BT_FIELDS];
    bt_snapshot *snapshot = NULL;
    (void)self;
    if (!call) return 0;
    call->out = NULL;
    fresh = (ULONGLONG)GetTickCount64() - (ULONGLONG)InterlockedCompareExchange64(&g_usage_at, 0, 0) <= BT_USAGE_FRESH_MS;
    AcquireSRWLockShared(&g_lock);
    call->generation = g_generation;
    call->reached = fresh ? g_reached : 0;
    if (call->op == SH_BUDGET_GENERATION) {
        r = 1;
    } else if (call->op == SH_BUDGET_TABLE && g_snapshot) {
        snapshot = g_snapshot;
        InterlockedIncrement(&snapshot->refs);
    } else if (call->op == SH_BUDGET_USAGE && g_usage_ok && fresh) {
        memcpy(limits, g_limits, sizeof limits);
        memcpy(usage, g_usage, sizeof usage);
        usage_ready = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    if (snapshot) {
        r = (call->out = bt_snapshot_json(snapshot)) != NULL;
        bt_snapshot_release(snapshot);
    } else if (usage_ready) {
        int n = sh_bt_usage_json(NULL, 0, limits, usage, call->reached, call->generation);
        if (n >= 0 && (call->out = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)n + 1)) != NULL &&
            sh_bt_usage_json(call->out, (size_t)n + 1, limits, usage, call->reached, call->generation) >= 0)
            r = 1;
        else { sh_iface_text_free(call->out); call->out = NULL; }
    }
    return r;
}
#endif
