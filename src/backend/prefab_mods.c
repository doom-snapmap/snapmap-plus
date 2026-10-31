/* prefab_mods.c -- see prefab_mods.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prefab_mods.h"
#include "modmanifest.h"
#include "modpublished.h"
#include "modexport.h"
#include "modserve.h"
#include "module_prefab_map.h"
#include "mp_json.h"
#include "backend_log.h"
#include "../common/snapmap_plus_iface.h"

#define PM_MSG_CAP  600
#define PM_LINE_CAP 1200
#define PM_NAME_MAX 120

static sh_prefab_mods_ops g_ops;
static int g_ops_bound;
static void pm_bind_defaults(void);   /* the product's ops (engine section); nothing in a pure build */

static const sh_prefab_mods_ops *pm_ops(void)
{
    if (!g_ops_bound) pm_bind_defaults();
    return &g_ops;
}

void sh_prefab_mods_bind(const sh_prefab_mods_ops *ops)
{
    if (ops) { g_ops = *ops; g_ops_bound = 1; }
    else { memset(&g_ops, 0, sizeof g_ops); g_ops_bound = 0; }
}

static void pm_logf(const char *fmt, ...)
{
    char line[PM_LINE_CAP];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void pm_print(const char *line)
{
    const sh_prefab_mods_ops *o = pm_ops();
    if (line && line[0] && o->print) o->print(line);
}

static void pm_msg(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!out || !cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(out, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ==================================================================== */
/* rows                                                                  */
/* ==================================================================== */

typedef struct pm_closure { sh_modmanifest_list list; int failed; } pm_closure;

static sh_modmanifest_row *pm_find(sh_modmanifest_list *list, const char *bundle)
{
    size_t i;
    for (i = 0; i < list->count; i++) if (!strcmp(list->rows[i].bundle, bundle)) return &list->rows[i];
    return NULL;
}

/* The save step's visitor (modmanifest.c, mm_save_visit): one row per bundle,
 * the placed names it provides on it. */
static void pm_visit(void *context, const char *bundle_id, const char *version, const char *bundle_hash,
                     unsigned long long size, const char *name)
{
    pm_closure *c = (pm_closure *)context;
    sh_modmanifest_row *row;
    if (!c || c->failed || !bundle_id || !bundle_hash) return;
    if (!sh_modmanifest_valid_id(bundle_id) || !sh_modmanifest_valid_hash(bundle_hash, strlen(bundle_hash)) ||
        (version && strlen(version) >= SH_MODMANIFEST_VERSION_CAP)) {
        c->failed = 1;
        return;
    }
    row = pm_find(&c->list, bundle_hash);
    if (!row) {
        sh_modmanifest_row fresh;
        memset(&fresh, 0, sizeof fresh);
        strcpy_s(fresh.id, sizeof fresh.id, bundle_id);
        strcpy_s(fresh.version, sizeof fresh.version, version ? version : "");
        strcpy_s(fresh.bundle, sizeof fresh.bundle, bundle_hash);
        fresh.size = size;
        if (!sh_modmanifest_list_add(&c->list, &fresh)) { c->failed = 1; return; }
        row = &c->list.rows[c->list.count - 1];
    }
    if (name && strlen(name) <= SH_MODMANIFEST_NAME_MAX && !sh_modmanifest_row_add_name(row, name)) c->failed = 1;
}

static int pm_row_order(const void *a, const void *b)
{
    const sh_modmanifest_row *x = (const sh_modmanifest_row *)a, *y = (const sh_modmanifest_row *)b;
    int c = strcmp(x->id, y->id);
    return c ? c : strcmp(x->bundle, y->bundle);
}

static int pm_cstr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void pm_sort(sh_modmanifest_list *list)
{
    size_t i;
    if (list->count > 1) qsort(list->rows, list->count, sizeof *list->rows, pm_row_order);
    for (i = 0; i < list->count; i++)
        if (list->rows[i].name_count > 1)
            qsort(list->rows[i].names, list->rows[i].name_count, sizeof(char *), pm_cstr_cmp);
}

int sh_prefab_mods_closure(const char *text, size_t len, sh_modmanifest_list *out)
{
    char **names = NULL, *engine;
    size_t count, elen = 0;
    int failed = 0;
    pm_closure c;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!text || !len) return 0;
    memset(&c, 0, sizeof c);
    /* The block's own strings are not placed names. */
    engine = sh_prefab_mods_strip_copy(text, len, &elen);
    if (!engine) return -1;
    count = sh_modmanifest_placed_names(engine, elen, &names, &failed);
    free(engine);
    if (failed) return -1;
    if (count) modserve_bundles_for_names_visit((const char *const *)names, count, pm_visit, &c);
    sh_modmanifest_names_free(names, count);
    if (c.failed) { sh_modmanifest_list_free(&c.list); return -1; }
    pm_sort(&c.list);
    *out = c.list;
    return (int)out->count;
}

int sh_prefab_mods_sidecar_rows(const char *side, size_t len, sh_modmanifest_list *out)
{
    mpj *root;
    const mpj *arr;
    int i;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!side || !len) return 0;
    root = mpj_parse(side, len);
    if (!root || root->type != MPJ_OBJ) { mpj_free(root); return -1; }
    arr = mpj_get(root, SH_PREFAB_MODS_KEY);
    if (!arr) { mpj_free(root); return 0; }
    if (arr->type != MPJ_ARR) { mpj_free(root); return -1; }
    for (i = 0; i < arr->n; i++) {
        sh_modmanifest_row row;
        char err[160];
        size_t tl = 0;
        char *text;
        if (!arr->items[i] || arr->items[i]->type != MPJ_OBJ) continue;
        text = mpj_dump(arr->items[i], &tl);
        if (text && sh_modmanifest_parse_value(text, tl, &row, err, sizeof err)) {
            sh_modmanifest_row *have = pm_find(out, row.bundle);
            if (!have) (void)sh_modmanifest_list_add(out, &row);
            else {
                size_t k;
                for (k = 0; k < row.name_count; k++) (void)sh_modmanifest_row_add_name(have, row.names[k]);
            }
            sh_modmanifest_row_free(&row);
        }
        free(text);
    }
    mpj_free(root);
    return (int)out->count;
}

static void pm_obj_remove(mpj *obj, const char *key)
{
    int i;
    if (!obj || obj->type != MPJ_OBJ) return;
    for (i = 0; i < obj->n; i++) {
        if (strcmp(obj->keys[i], key)) continue;
        mpj_free(obj->items[i]);
        free(obj->keys[i]);
        memmove(&obj->items[i], &obj->items[i + 1], sizeof(obj->items[0]) * (size_t)(obj->n - i - 1));
        memmove(&obj->keys[i], &obj->keys[i + 1], sizeof(obj->keys[0]) * (size_t)(obj->n - i - 1));
        obj->n--;
        return;
    }
}

char *sh_prefab_mods_sidecar_set(const char *side, size_t len, const sh_modmanifest_list *rows, size_t *out_len)
{
    static const char FRESH[] = "{\"desc\":\"\",\"tags\":[]}";
    mpj *root, *arr;
    char *out;
    size_t i;
    if (out_len) *out_len = 0;
    if (!side || !len) { side = FRESH; len = sizeof FRESH - 1; }
    root = mpj_parse(side, len);
    if (!root || root->type != MPJ_OBJ) { mpj_free(root); return NULL; }
    if (!rows || !rows->count) pm_obj_remove(root, SH_PREFAB_MODS_KEY);
    else {
        arr = mpj_new(MPJ_ARR);
        if (!arr) { mpj_free(root); return NULL; }
        for (i = 0; i < rows->count; i++) {
            size_t vl = 0;
            char *v = sh_modmanifest_format_value(&rows->rows[i], &vl);
            mpj *node = v ? mpj_parse(v, vl) : NULL;
            free(v);
            if (!node || !mpj_push(arr, node)) { mpj_free(arr); mpj_free(root); return NULL; }
        }
        if (!mpj_set(root, SH_PREFAB_MODS_KEY, arr)) { mpj_free(root); return NULL; }
    }
    out = mpj_dump(root, out_len);
    mpj_free(root);
    return out;
}

int sh_prefab_mods_row_provides(const sh_modmanifest_list *rows, const char *name)
{
    size_t i, k;
    if (!rows || !name || !name[0]) return 0;
    for (i = 0; i < rows->count; i++)
        for (k = 0; k < rows->rows[i].name_count; k++)
            if (rows->rows[i].names[k] && !_stricmp(rows->rows[i].names[k], name)) return 1;
    return 0;
}

void sh_prefab_mods_label(const sh_modmanifest_row *row, char *out, size_t cap)
{
    if (!out || !cap) return;
    if (!row) { out[0] = 0; return; }
    if (row->id[0] && row->version[0]) _snprintf_s(out, cap, _TRUNCATE, "%s %s", row->id, row->version);
    else if (row->id[0]) _snprintf_s(out, cap, _TRUNCATE, "%s", row->id);
    else _snprintf_s(out, cap, _TRUNCATE, "bundle %.16s", row->bundle);
}

typedef struct pm_buf { char *p; size_t n, cap; int bad; } pm_buf;

static void pm_put(pm_buf *b, const char *s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 256;
        char *g;
        while (want < b->n + n + 1) want *= 2;
        g = (char *)realloc(b->p, want);
        if (!g) { b->bad = 1; return; }
        b->p = g;
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void pm_puts(pm_buf *b, const char *s) { pm_put(b, s, strlen(s)); }
static void pm_put_str(pm_buf *b, const char *s)
{
    pm_put(b, "\"", 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; pm_put(b, e, 2); }
        else if (c < 0x20) { char e[8]; _snprintf_s(e, sizeof e, _TRUNCATE, "\\u%04x", c); pm_puts(b, e); }
        else pm_put(b, (const char *)&c, 1);
    }
    pm_put(b, "\"", 1);
}

static int pm_installed(const char *hash)
{
    const sh_prefab_mods_ops *o = pm_ops();
    return o->bundle_state && o->bundle_state(hash) == 1;
}

char *sh_prefab_mods_status_json(const sh_modmanifest_list *rows, size_t *missing_out)
{
    pm_buf b = { 0 };
    size_t i, missing = 0;
    char num[48];
    if (missing_out) *missing_out = 0;
    for (i = 0; rows && i < rows->count; i++) if (!pm_installed(rows->rows[i].bundle)) missing++;
    _snprintf_s(num, sizeof num, _TRUNCATE, "{\"count\":%u,\"missing\":%u,\"mods\":[",
                (unsigned)(rows ? rows->count : 0), (unsigned)missing);
    pm_puts(&b, num);
    for (i = 0; rows && i < rows->count; i++) {
        const sh_modmanifest_row *r = &rows->rows[i];
        if (i) pm_puts(&b, ",");
        pm_puts(&b, "{\"id\":"); pm_put_str(&b, r->id);
        pm_puts(&b, ",\"version\":"); pm_put_str(&b, r->version);
        pm_puts(&b, ",\"bundle\":"); pm_put_str(&b, r->bundle);
        _snprintf_s(num, sizeof num, _TRUNCATE, ",\"size\":%llu", r->size);
        pm_puts(&b, num);
        pm_puts(&b, pm_installed(r->bundle) ? ",\"installed\":true}" : ",\"installed\":false}");
    }
    pm_puts(&b, "]}");
    if (b.bad) { free(b.p); return NULL; }
    if (missing_out) *missing_out = missing;
    return b.p;
}

/* ==================================================================== */
/* files                                                                 */
/* ==================================================================== */

char *sh_prefab_mods_read_file(const char *path, size_t *len)
{
    FILE *f = NULL;
    long size;
    char *buf;
    size_t got;
    if (len) *len = 0;
    if (!path || !path[0] || fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > (long)SH_PREFAB_MODS_FILE_MAX ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

int sh_prefab_mods_write_file(const char *path, const char *data, size_t len)
{
    char tmp[MAX_PATH * 2];
    FILE *f = NULL;
    size_t put;
    if (!path || !path[0] || (!data && len)) return 0;
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.smp-%lu.tmp", path, GetCurrentProcessId());
    if (fopen_s(&f, tmp, "wb") != 0 || !f) return 0;
    put = len ? fwrite(data, 1, len, f) : 0;
    if (fclose(f) != 0 || put != len) { DeleteFileA(tmp); return 0; }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileA(tmp); return 0; }
    return 1;
}

static void pm_rows_text(const sh_modmanifest_list *rows, char *out, size_t cap)
{
    size_t i;
    out[0] = 0;
    for (i = 0; rows && i < rows->count; i++) {
        char label[SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 4];
        sh_prefab_mods_label(&rows->rows[i], label, sizeof label);
        if (i) strncat_s(out, cap, ", ", _TRUNCATE);
        strncat_s(out, cap, label, _TRUNCATE);
    }
}

/* ==================================================================== */
/* the block                                                             */
/* ==================================================================== */

/* The view's members the block keeps outside "summary". */
static int pm_view_own_key(const char *key)
{
    return !strcmp(key, "desc") || !strcmp(key, SH_PREFAB_MODS_KEY) || !strcmp(key, "tags") ||
           !strcmp(key, "description") || !strcmp(key, "format") || !strcmp(key, "summary");
}

static mpj *pm_rows_array(const sh_modmanifest_list *rows)
{
    mpj *arr = mpj_new(MPJ_ARR);
    size_t i;
    if (!arr) return NULL;
    for (i = 0; rows && i < rows->count; i++) {
        size_t vl = 0;
        char *v = sh_modmanifest_format_value(&rows->rows[i], &vl);
        mpj *node = v ? mpj_parse(v, vl) : NULL;
        free(v);
        if (!node || !mpj_push(arr, node)) { mpj_free(arr); return NULL; }
    }
    return arr;
}

char *sh_prefab_mods_block_compose(const char *view, size_t len, const sh_modmanifest_list *rows, size_t *out_len)
{
    mpj *v = (view && len) ? mpj_parse(view, len) : NULL, *b, *summary, *node;
    const mpj *m;
    char *out = NULL;
    int i, ok;
    if (out_len) *out_len = 0;
    if (v && v->type != MPJ_OBJ) { mpj_free(v); v = NULL; }
    b = mpj_new(MPJ_OBJ);
    summary = mpj_new(MPJ_OBJ);
    if (!b || !summary) { mpj_free(b); mpj_free(summary); mpj_free(v); return NULL; }
    ok = mpj_set(b, "format", mpj_new_int(SH_PREFAB_BLOCK_FORMAT));
    m = v ? mpj_get(v, "desc") : NULL;
    node = (m && m->type == MPJ_STR) ? mpj_clone(m) : mpj_new_str("");
    ok = ok && node && mpj_set(b, "description", node);
    m = v ? mpj_get(v, "tags") : NULL;
    if (m && m->type == MPJ_ARR) {
        /* Tags are strings; anything else in a hand-edited sidecar is left out. */
        node = mpj_new(MPJ_ARR);
        for (i = 0; node && i < m->n; i++)
            if (m->items[i] && m->items[i]->type == MPJ_STR && !mpj_push(node, mpj_clone(m->items[i]))) node = NULL;
    } else node = mpj_new(MPJ_ARR);
    ok = ok && node && mpj_set(b, "tags", node);
    if (rows) node = pm_rows_array(rows);
    else {
        m = v ? mpj_get(v, SH_PREFAB_MODS_KEY) : NULL;
        node = (m && m->type == MPJ_ARR) ? mpj_clone(m) : mpj_new(MPJ_ARR);
    }
    ok = ok && node && mpj_set(b, SH_PREFAB_MODS_KEY, node);
    for (i = 0; ok && v && i < v->n; i++) {
        if (pm_view_own_key(v->keys[i])) continue;
        ok = mpj_set(summary, v->keys[i], mpj_clone(v->items[i]));
    }
    if (ok && summary->n) { ok = mpj_set(b, "summary", summary); summary = NULL; }
    if (ok) out = mpj_dump(b, out_len);
    mpj_free(summary);
    mpj_free(b);
    mpj_free(v);
    return out;
}

char *sh_prefab_mods_block_view(const char *block, size_t len, size_t *out_len)
{
    mpj *b, *v;
    const mpj *m, *s;
    char *out = NULL;
    int i, ok;
    if (out_len) *out_len = 0;
    b = (block && len) ? mpj_parse(block, len) : NULL;
    if (!b || b->type != MPJ_OBJ) { mpj_free(b); return NULL; }
    v = mpj_new(MPJ_OBJ);
    if (!v) { mpj_free(b); return NULL; }
    m = mpj_get(b, "description");
    ok = mpj_set(v, "desc", (m && m->type == MPJ_STR) ? mpj_clone(m) : mpj_new_str(""));
    m = mpj_get(b, "tags");
    ok = ok && mpj_set(v, "tags", (m && m->type == MPJ_ARR) ? mpj_clone(m) : mpj_new(MPJ_ARR));
    s = mpj_get(b, "summary");
    for (i = 0; ok && s && s->type == MPJ_OBJ && i < s->n; i++) {
        if (pm_view_own_key(s->keys[i])) continue;
        ok = mpj_set(v, s->keys[i], mpj_clone(s->items[i]));
    }
    m = mpj_get(b, SH_PREFAB_MODS_KEY);
    if (ok && m && m->type == MPJ_ARR && m->n) ok = mpj_set(v, SH_PREFAB_MODS_KEY, mpj_clone(m));
    if (ok) out = mpj_dump(v, out_len);
    mpj_free(v);
    mpj_free(b);
    return out;
}

char *sh_prefab_mods_view_of(const char *text, size_t len, int tail, const char *meta_path, int *source, size_t *out_len)
{
    sh_prefab_block_span span;
    char *view = NULL;
    int found;
    if (source) *source = SH_PREFAB_META_NONE;
    if (out_len) *out_len = 0;
    found = (text && len) ? (tail ? sh_prefab_block_find_tail(text, len, &span) : sh_prefab_block_find(text, len, &span)) : 0;
    if (found == 1) view = sh_prefab_mods_block_view(text + span.value_at, span.value_len, out_len);
    if (view) {
        if (source) *source = SH_PREFAB_META_BLOCK;
        return view;
    }
    /* A prefab saved before the block: its sidecar, read-only. */
    if (meta_path && meta_path[0]) {
        size_t slen = 0;
        view = sh_prefab_mods_read_file(meta_path, &slen);
        if (view && slen) {
            if (source) *source = SH_PREFAB_META_SIDECAR;
            if (out_len) *out_len = slen;
            return view;
        }
        free(view);
    }
    return NULL;
}

int sh_prefab_mods_rows_of(const char *text, size_t len, const char *meta_path, sh_modmanifest_list *out)
{
    size_t vlen = 0;
    char *view = sh_prefab_mods_view_of(text, len, 0, meta_path, NULL, &vlen);
    int n = sh_prefab_mods_sidecar_rows(view, vlen, out);
    free(view);
    return n;
}

char *sh_prefab_mods_embed(const char *text, size_t len, const char *view, size_t view_len,
                           const sh_modmanifest_list *rows, size_t *out_len)
{
    size_t blen = 0;
    char *block, *out;
    if (out_len) *out_len = 0;
    block = sh_prefab_mods_block_compose(view, view_len, rows, &blen);
    if (!block) return NULL;
    out = sh_prefab_block_put(text, len, block, blen, out_len);
    free(block);
    return out;
}

char *sh_prefab_mods_strip_copy(const char *text, size_t len, size_t *out_len)
{
    char *copy;
    if (out_len) *out_len = 0;
    if (!text) return NULL;
    copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = 0;
    len = sh_prefab_block_strip(copy, len);
    if (out_len) *out_len = len;
    return copy;
}

/* The view of the block a text carries, for a caller that just wrote it. */
static char *pm_view_in(const char *text, size_t len)
{
    sh_prefab_block_span span;
    if (sh_prefab_block_find(text, len, &span) != 1) return NULL;
    return sh_prefab_mods_block_view(text + span.value_at, span.value_len, NULL);
}

int sh_prefab_mods_record(const char *text, size_t len, const char *path, char **view_out)
{
    sh_modmanifest_list rows;
    char *engine, *next, list[600];
    size_t elen = 0, nlen = 0;
    int n;
    if (view_out) *view_out = NULL;
    if (!path || !path[0] || !text || !len) return -1;
    engine = sh_prefab_mods_strip_copy(text, len, &elen);
    if (!engine) return -1;
    /* The file Create writes is plain JSON (the host removes the serializer's
     * comments before it writes; a text that still has them loses them here). */
    elen = sh_prefab_block_strip_comments(engine, elen);
    n = sh_prefab_mods_closure(engine, elen, &rows);
    if (n < 0) {
        pm_logf("PREFABS: %s: the prefab's mods could not be read; the file is left as the engine wrote it", path);
        free(engine);
        return -1;
    }
    next = sh_prefab_mods_embed(engine, elen, NULL, 0, &rows, &nlen);
    free(engine);
    if (!next || !sh_prefab_mods_write_file(path, next, nlen)) {
        pm_logf("PREFABS: %s: %s; the prefab's %d mod(s) were not recorded", path,
                next ? "the file could not be written" : "the file is not a JSON object", n);
        free(next);
        sh_modmanifest_list_free(&rows);
        return -1;
    }
    pm_rows_text(&rows, list, sizeof list);
    pm_logf("PREFABS: %s records %d mod(s)%s%s", path, n, n ? ": " : "", list);
    if (view_out) *view_out = pm_view_in(next, nlen);
    free(next);
    sh_modmanifest_list_free(&rows);
    return n;
}

char *sh_prefab_mods_module_file(const char *prefab, size_t len, const char *summary, size_t *out_len, char **view_out,
                                 int *count)
{
    sh_modmanifest_list rows;
    char *out;
    int n;
    if (out_len) *out_len = 0;
    if (view_out) *view_out = NULL;
    n = sh_prefab_mods_closure(prefab, len, &rows);
    if (count) *count = n < 0 ? 0 : n;
    if (n < 0) memset(&rows, 0, sizeof rows);   /* recorded as naming none, as a map's save would */
    out = sh_prefab_mods_embed(prefab, len, summary, summary ? strlen(summary) : 0, &rows, out_len);
    sh_modmanifest_list_free(&rows);
    if (out && view_out) *view_out = pm_view_in(out, out_len ? *out_len : strlen(out));
    return out;
}

int sh_prefab_mods_meta_write(const char *body, size_t len, const char *path, const char *meta_path)
{
    mpj *b = NULL, *v = NULL;
    const mpj *m;
    char *text, *view, *nview = NULL, *next = NULL;
    size_t tlen = 0, vlen = 0, nvlen = 0, nlen = 0;
    int source = 0, ok = 0, i;
    if (!path || !path[0]) return -1;
    text = sh_prefab_mods_read_file(path, &tlen);
    if (!text || !tlen) { free(text); return -1; }
    view = sh_prefab_mods_view_of(text, tlen, 0, meta_path, &source, &vlen);
    v = view ? mpj_parse(view, vlen) : NULL;
    if (!v || v->type != MPJ_OBJ) { mpj_free(v); v = mpj_new(MPJ_OBJ); }
    if (body && len) b = mpj_parse(body, len);
    if (v) {
        m = (b && b->type == MPJ_OBJ) ? mpj_get(b, "desc") : NULL;
        ok = mpj_set(v, "desc", (m && m->type == MPJ_STR) ? mpj_clone(m) : mpj_new_str(""));
        m = (b && b->type == MPJ_OBJ) ? mpj_get(b, "tags") : NULL;
        if (m && m->type == MPJ_ARR) {
            mpj *tags = mpj_new(MPJ_ARR);
            for (i = 0; tags && i < m->n; i++)
                if (m->items[i] && m->items[i]->type == MPJ_STR && !mpj_push(tags, mpj_clone(m->items[i]))) tags = NULL;
            ok = ok && tags && mpj_set(v, "tags", tags);
        } else ok = ok && mpj_set(v, "tags", mpj_new(MPJ_ARR));
        if (ok) nview = mpj_dump(v, &nvlen);
    }
    if (nview) next = sh_prefab_mods_embed(text, tlen, nview, nvlen, NULL, &nlen);
    ok = next && sh_prefab_mods_write_file(path, next, nlen);
    if (ok && source == SH_PREFAB_META_SIDECAR)
        pm_logf("PREFABS: %s: its details moved from its sidecar into the file; the sidecar is left as it was", path);
    mpj_free(b);
    mpj_free(v);
    free(text); free(view); free(nview); free(next);
    return ok ? 1 : -1;
}

/* ==================================================================== */
/* resolve                                                               */
/* ==================================================================== */

static volatile LONG g_seq;          /* last held placement */
static volatile LONG g_wait_seq;     /* the placement the hold is for */
static volatile LONG g_ready_seq;    /* the placement whose mods came */

typedef struct pm_known_ctx { const sh_modmanifest_list *rows; int (*known)(const char *); } pm_known_ctx;

/* The module guard's question with the prefab's rows: a module a row
 * provides is a mod (resolved below), not a missing room. */
static int pm_known(void *ctx, const char *name)
{
    pm_known_ctx *k = (pm_known_ctx *)ctx;
    if (sh_prefab_mods_row_provides(k->rows, name)) return 1;
    return k->known ? k->known(name) : -1;
}

/* Activate the rows' bundles for the open map and attach each one the map
 * does not list. Main thread. */
static void pm_prepare(const sh_modmanifest_list *rows, const char *name)
{
    const sh_prefab_mods_ops *o = pm_ops();
    const char **hashes;
    size_t i;
    if (!rows || !rows->count) return;
    hashes = (const char **)calloc(rows->count, sizeof *hashes);
    if (hashes) {
        for (i = 0; i < rows->count; i++) hashes[i] = rows->rows[i].bundle;
        if (o->activate) (void)o->activate(hashes, (int)rows->count);
        free((void *)hashes);
    }
    for (i = 0; i < rows->count; i++) {
        const sh_modmanifest_row *r = &rows->rows[i];
        char label[SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 4], note[256] = "", line[PM_LINE_CAP];
        int a;
        if (o->listed && o->listed(r->bundle) > 0) continue;
        if (!o->attach) continue;
        sh_prefab_mods_label(r, label, sizeof label);
        a = o->attach(r, note, sizeof note);
        if (a > 0) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "Prefab %s uses %s; this map did not list it, so it was added to "
                        "the map (the Mods tab's Remove from map takes it out).", name && name[0] ? name : "prefab", label);
            pm_print(line);
            pm_logf("PREFABS: %s", line);
        } else if (a < 0) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "Prefab %s uses %s, which could not be added to this map%s%s.",
                        name && name[0] ? name : "prefab", label, note[0] ? ": " : "", note);
            pm_print(line);
            pm_logf("PREFABS: %s", line);
        }
    }
}

typedef struct pm_ready_ctx { const char *name; const sh_modmanifest_list *rows; } pm_ready_ctx;

static int pm_prepare_main(void *ctx)
{
    pm_ready_ctx *r = (pm_ready_ctx *)ctx;
    pm_prepare(r->rows, r->name);
    return 1;
}

/* modpublished.c's ready callback: the held placement's mods are installed. */
static void pm_ready(const char *name, const sh_modmanifest_list *rows)
{
    const sh_prefab_mods_ops *o = pm_ops();
    pm_ready_ctx r;
    r.name = name;
    r.rows = rows;
    /* Inside the scope the game thread's calls use (the tick runs outside it). */
    if (!o->call_main || !o->call_main(pm_prepare_main, &r)) pm_prepare(rows, name);
    InterlockedExchange(&g_ready_seq, InterlockedCompareExchange(&g_wait_seq, 0, 0));
    pm_logf("PREFABS: the mods of prefab '%s' are installed; the placement goes on", name ? name : "");
}

int sh_prefab_mods_poll(int seq)
{
    if (seq <= 0) return 0;
    if (InterlockedCompareExchange(&g_ready_seq, 0, 0) == seq) return 1;
    if (InterlockedCompareExchange(&g_wait_seq, 0, 0) == seq && sh_modpub_prefab_holding()) return 2;
    return 0;
}

/* The refusal without the store: console line, toast, the tab's message. */
static void pm_refuse_unasked(const char *name, const sh_modmanifest_list *rows, char *msg, size_t cap)
{
    const sh_prefab_mods_ops *o = pm_ops();
    char line[PM_LINE_CAP], notice[256];
    const char **labels = (const char **)calloc(rows->count ? rows->count : 1, sizeof *labels);
    char (*store)[SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 4] =
        calloc(rows->count ? rows->count : 1, sizeof *store);
    size_t i, n = 0;
    if (!labels || !store) {
        free((void *)labels); free(store);
        pm_msg(msg, cap, "This prefab was not placed. It needs mods you do not have.");
        pm_print(msg);
        return;
    }
    for (i = 0; i < rows->count; i++) {
        if (pm_installed(rows->rows[i].bundle)) continue;
        sh_prefab_mods_label(&rows->rows[i], store[n], sizeof store[n]);
        labels[n] = store[n];
        n++;
    }
    sh_modpublished_prefab_refusal_line(line, sizeof line, name, labels, n, 2);
    sh_modpublished_compose_prefab_refusal(notice, sizeof notice, labels, n, 2);
    free((void *)labels); free(store);
    pm_print(line);
    pm_logf("PREFABS: %s", line);
    if (o->toast) o->toast(notice);
    pm_msg(msg, cap, "%s", notice);
}

int sh_prefab_mods_resolve_main(const char *text, size_t len, const sh_modmanifest_list *rows, const char *name,
                                int *seq, char *msg, size_t cap)
{
    const sh_prefab_mods_ops *o = pm_ops();
    size_t i, missing = 0;
    int r;
    if (seq) *seq = 0;
    if (msg && cap) msg[0] = 0;
    if (!rows || !rows->count) return SH_PREFAB_MODS_PLACE;
    /* A module prefab: a room no row provides and the palette lacks is DLC
     * the player does not own. Nothing can install it, so nothing is fetched. */
    if (text && sh_mp_is_module_file(text, len)) {
        sh_mp_summary sum;
        sh_mp_module_check check;
        pm_known_ctx k;
        char err[256];
        size_t elen = 0;
        char *engine = sh_prefab_mods_strip_copy(text, len, &elen);
        memset(&sum, 0, sizeof sum);
        if (engine && sh_mp_inspect(engine, elen, &sum, err, (int)sizeof err)) {
            k.rows = rows;
            k.known = o->module_known;
            if (sh_mp_check_modules(&sum, pm_known, &k, &check) > 0) {
                char line[PM_LINE_CAP];
                sh_mp_missing_console_line(&check, line, sizeof line);
                pm_print(line);
                pm_logf("PREFABS: placement of '%s' refused: %.300s", name ? name : "", line);
                if (msg && cap) sh_mp_missing_toast(&check, msg, cap);
                sh_mp_summary_free(&sum);
                free(engine);
                return SH_PREFAB_MODS_NEEDS_MODULE;
            }
            sh_mp_summary_free(&sum);
        }
        free(engine);
    }
    for (i = 0; i < rows->count; i++) if (!pm_installed(rows->rows[i].bundle)) missing++;
    if (missing && o->restore) {
        /* A mod in the recycle bin comes back, as a map load would bring it. */
        const char **hashes = (const char **)calloc(missing, sizeof *hashes);
        int n = 0;
        if (hashes) {
            for (i = 0; i < rows->count; i++) if (!pm_installed(rows->rows[i].bundle)) hashes[n++] = rows->rows[i].bundle;
            if (o->restore(hashes, n) > 0) {
                missing = 0;
                for (i = 0; i < rows->count; i++) if (!pm_installed(rows->rows[i].bundle)) missing++;
            }
            free((void *)hashes);
        }
    }
    if (!missing) {
        pm_prepare(rows, name);
        return SH_PREFAB_MODS_PLACE;
    }
    if (sh_modpublished_holding()) {
        pm_msg(msg, cap, sh_modpub_prefab_holding()
            ? "Another prefab is waiting for its mods. Answer that first, then place this one."
            : "A map is waiting for its mods. Answer that first, then place this prefab.");
        return SH_PREFAB_MODS_REFUSED;
    }
    sh_modpub_set_prefab_ready(pm_ready);
    r = sh_modpub_prefab_hold(rows, name);
    if (r == 0) {
        pm_prepare(rows, name);
        return SH_PREFAB_MODS_PLACE;
    }
    if (r == -2) {
        pm_refuse_unasked(name, rows, msg, cap);
        return SH_PREFAB_MODS_REFUSED;
    }
    if (r < 0) {
        pm_msg(msg, cap, "This prefab needs mods that could not be prepared. The prefab was not placed.");
        pm_print(msg);
        return SH_PREFAB_MODS_REFUSED;
    }
    {
        LONG s = InterlockedIncrement(&g_seq);
        InterlockedExchange(&g_wait_seq, s);
        if (seq) *seq = (int)s;
    }
    pm_msg(msg, cap, "This prefab needs %u mod%s you do not have; checking the mod store.", (unsigned)missing,
           missing == 1 ? "" : "s");
    return SH_PREFAB_MODS_HELD;
}

/* ==================================================================== */
/* export, import                                                        */
/* ==================================================================== */

int sh_prefab_mods_valid_name(const char *name)
{
    static const char *const DEVICES[] = { "con", "prn", "aux", "nul", NULL };
    size_t n, i, stem;
    char low[8];
    if (!name) return 0;
    n = strlen(name);
    if (!n || n > PM_NAME_MAX) return 0;
    if (name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.') return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return 0;
    }
    if (n >= 5 && !_stricmp(name + n - 5, ".meta")) return 0;
    for (stem = 0; stem < n && name[stem] != '.'; stem++) {}
    if (stem == 3 || stem == 4) {
        for (i = 0; i < stem; i++) low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
        low[stem] = 0;
        for (i = 0; DEVICES[i]; i++) if (stem == 3 && !strcmp(low, DEVICES[i])) return 0;
        if (stem == 4 && (!strncmp(low, "com", 3) || !strncmp(low, "lpt", 3)) && low[3] >= '1' && low[3] <= '9') return 0;
    }
    return 1;
}

static int pm_acp_to_utf8(const char *acp, char *out, size_t cap)
{
    wchar_t wide[MAX_PATH * 2];
    if (!acp || !acp[0] || !MultiByteToWideChar(CP_ACP, 0, acp, -1, wide, MAX_PATH * 2)) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

static int pm_utf8_to_acp(const char *utf8, char *out, size_t cap)
{
    wchar_t wide[MAX_PATH * 2];
    if (!utf8 || !utf8[0] || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH * 2)) return 0;
    return WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

/* What the ZIP carries as <name>.json: the file's bytes, unless its block has
 * to be written for the rows the ZIP carries. */
typedef struct pm_export_ctx {
    const char *text;
    size_t len;
    const char *view;
    size_t view_len;
    int rewrite;   /* no block, or rows found by the closure */
} pm_export_ctx;

static char *pm_export_text(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len)
{
    pm_export_ctx *x = (pm_export_ctx *)ctx;
    char *out;
    if (!x->rewrite && !moved) {
        out = (char *)malloc(x->len + 1);
        if (!out) return NULL;
        memcpy(out, x->text, x->len);
        out[x->len] = 0;
        *len = x->len;
        return out;
    }
    return sh_prefab_mods_embed(x->text, x->len, x->view, x->view_len, rows, len);
}

int sh_prefab_mods_export(const char *prefab_path, const char *meta_path, const char *name, const char *zip_utf8,
                          char *msg, size_t cap)
{
    modexport_stats st;
    sh_modmanifest_list rows;
    pm_export_ctx x;
    char *text, *view, cause[400];
    size_t len = 0, vlen = 0;
    int n, ok, source = 0;
    const char *nm = name && name[0] ? name : "prefab";
    if (msg && cap) msg[0] = 0;
    if (!zip_utf8 || !zip_utf8[0]) { pm_msg(msg, cap, "Prefab %s was not exported: no ZIP was named.", nm); return -1; }
    if (!modexport_is_zip_path(zip_utf8)) { pm_msg(msg, cap, "Prefab %s was not exported: the file must end in .zip.", nm); return -1; }
    text = sh_prefab_mods_read_file(prefab_path, &len);
    if (!text || !len) { free(text); pm_msg(msg, cap, "Prefab %s was not exported: its file could not be read.", nm); return -1; }
    view = sh_prefab_mods_view_of(text, len, 0, meta_path, &source, &vlen);
    n = sh_prefab_mods_sidecar_rows(view, vlen, &rows);
    memset(&x, 0, sizeof x);
    x.text = text;
    x.len = len;
    x.view = view;
    x.view_len = vlen;
    x.rewrite = source != SH_PREFAB_META_BLOCK;
    if (n <= 0) {
        /* Saved before its mods were recorded: the mods installed now that its
         * entities and modules use. */
        sh_modmanifest_list_free(&rows);
        if (sh_prefab_mods_closure(text, len, &rows) < 0) memset(&rows, 0, sizeof rows);
        if (rows.count) x.rewrite = 1;
    }
    ok = modexport_write_prefab(nm, text, len, &rows, pm_export_text, &x, zip_utf8, &st, cause, sizeof cause);
    if (ok) {
        if (st.bundles)
            pm_msg(msg, cap, "Exported prefab %s with %u mod%s to %s (%llu KB).", nm, st.bundles, st.bundles == 1 ? "" : "s",
                   zip_utf8, (st.zip_bytes + 1023) / 1024);
        else pm_msg(msg, cap, "Exported prefab %s to %s; it uses no mods.", nm, zip_utf8);
        pm_logf("PREFABS: export %s: %u bundle(s), %u file(s) carried, %u entries, %llu byte(s)%s in %.0f ms",
                zip_utf8, st.bundles, st.files, st.entries, st.zip_bytes,
                x.rewrite ? " (its block written for the export)" : "", st.ms);
    } else {
        pm_msg(msg, cap, "Prefab %s was not exported: %s.", nm, cause);
        pm_logf("PREFABS: export %s refused: %s", zip_utf8, cause);
    }
    sh_modmanifest_list_free(&rows);
    free(text);
    free(view);
    return ok ? 1 : -1;
}

/* The source's file name without its folder and extension. */
static void pm_file_stem(const char *path, char *out, size_t cap)
{
    const char *base = path, *p;
    size_t n;
    for (p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    n = strlen(base);
    if (n > 4 && !_stricmp(base + n - 4, ".zip")) n -= 4;
    else if (n > 5 && !_stricmp(base + n - 5, ".json")) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

static int pm_same_bytes(const char *path, const char *data, size_t len)
{
    size_t have = 0;
    char *disk = sh_prefab_mods_read_file(path, &have);
    int same = disk && have == len && !memcmp(disk, data, len);
    free(disk);
    return same;
}

/* The importer's rows: the block's (an export carries no sidecar). */
static int pm_import_rows(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows)
{
    int n;
    (void)ctx;
    n = sh_prefab_mods_rows_of(prefab, len, NULL, rows);
    return n < 0 ? 0 : n;
}

int sh_prefab_mods_import(const char *src_utf8, const char *prefabs_dir, char *name_out, size_t name_cap,
                          char *msg, size_t cap)
{
    const sh_prefab_mods_ops *o = pm_ops();
    modexport_stats st;
    sh_modmanifest_list rows;
    char *text = NULL, given[256] = "", name[PM_NAME_MAX + 16], acp[PM_NAME_MAX * 2 + 16];
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], cause[400], extra[400] = "";
    size_t len = 0, i, absent = 0, dl;
    int k, result = -1;
    if (name_out && name_cap) name_out[0] = 0;
    if (msg && cap) msg[0] = 0;
    if (!src_utf8 || !src_utf8[0] || !prefabs_dir || !prefabs_dir[0]) { pm_msg(msg, cap, "No file was named."); return -1; }
    if (!modexport_read_prefab(src_utf8, pm_import_rows, NULL, &text, &len, given, sizeof given, &st, cause, sizeof cause)) {
        pm_msg(msg, cap, "The prefab was not imported: %s.", cause);
        pm_logf("PREFABS: import %s refused: %s", src_utf8, cause);
        return -1;
    }
    /* The name: the .json's, else the source's file name, else a plain one. */
    if (!sh_prefab_mods_valid_name(given)) pm_file_stem(src_utf8, given, sizeof given);
    if (!sh_prefab_mods_valid_name(given)) strcpy_s(given, sizeof given, "Imported prefab");
    if (!pm_utf8_to_acp(given, acp, sizeof acp)) strcpy_s(acp, sizeof acp, "Imported prefab");
    strncpy_s(dir, sizeof dir, prefabs_dir, _TRUNCATE);
    dl = strlen(dir);
    if (dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/') strncat_s(dir, sizeof dir, "\\", _TRUNCATE);
    CreateDirectoryA(dir, NULL);
    for (k = 1; k <= 99; k++) {
        if (k == 1) strcpy_s(name, sizeof name, acp);
        else _snprintf_s(name, sizeof name, _TRUNCATE, "%s (%d)", acp, k);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s%s.json", dir, name);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { result = 1; break; }
        if (pm_same_bytes(path, text, len)) { result = 0; break; }
    }
    if (result < 0) {
        pm_msg(msg, cap, "The prefab was not imported: every name from \"%s\" to \"%s (99)\" is taken.", acp, acp);
        free(text);
        return -1;
    }
    if (result == 1 && !sh_prefab_mods_write_file(path, text, len)) {
        DeleteFileA(path);
        pm_msg(msg, cap, "The prefab was not imported: %s could not be written.", path);
        free(text);
        return -1;
    }
    if (name_out && name_cap) strncpy_s(name_out, name_cap, name, _TRUNCATE);
    /* Rows the export names but does not carry: the placement asks for them. */
    if (sh_prefab_mods_rows_of(text, len, NULL, &rows) > 0) {
        for (i = 0; i < rows.count; i++) {
            char label[SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 4];
            if (pm_installed(rows.rows[i].bundle)) continue;
            sh_prefab_mods_label(&rows.rows[i], label, sizeof label);
            strncat_s(extra, sizeof extra, absent ? ", " : " It also needs ", _TRUNCATE);
            strncat_s(extra, sizeof extra, label, _TRUNCATE);
            absent++;
        }
        if (absent) strncat_s(extra, sizeof extra, ", which the export does not carry.", _TRUNCATE);
    }
    sh_modmanifest_list_free(&rows);
    if (result == 1)
        pm_msg(msg, cap, "Imported prefab %s: %u mod%s installed, %u already installed.%s", name, st.bundles_installed,
               st.bundles_installed == 1 ? "" : "s", st.bundles_present, extra);
    else if (st.bundles_installed)
        pm_msg(msg, cap, "Prefab %s is already in your library; %u mod%s installed.%s", name, st.bundles_installed,
               st.bundles_installed == 1 ? " was" : "s were", extra);
    else pm_msg(msg, cap, "Prefab %s and its mods are already here; nothing was changed.%s", name, extra);
    pm_logf("PREFABS: import %s -> %s: %u bundle(s) (%u installed, %u present), %u file(s) copied out, %llu byte(s) in %.0f ms",
            src_utf8, result == 1 ? path : "(unchanged)", st.bundles, st.bundles_installed, st.bundles_present,
            st.files, st.zip_bytes, st.ms);
    if (st.bundles_installed && o->refresh) (void)o->refresh();
    free(text);
    return result;
}

/* ==================================================================== */
/* the slot                                                            */
/* ==================================================================== */

static void pm_out(sh_module_prefab_call *call, const char *text)
{
    if (call->out && call->out_cap > 0) strncpy_s(call->out, (size_t)call->out_cap, text ? text : "", _TRUNCATE);
}

typedef struct pm_resolve_job {
    const char *text;
    size_t len;
    const sh_modmanifest_list *rows;
    const char *name;
    int result, seq;
    char msg[PM_MSG_CAP];
} pm_resolve_job;

static int pm_resolve_job_main(void *ctx)
{
    pm_resolve_job *j = (pm_resolve_job *)ctx;
    j->result = sh_prefab_mods_resolve_main(j->text, j->len, j->rows, j->name, &j->seq, j->msg, sizeof j->msg);
    return 1;
}

static void pm_json_put_kv(pm_buf *b, const char *key, const char *value, int first)
{
    if (!first) pm_puts(b, ",");
    pm_put_str(b, key);
    pm_puts(b, ":");
    pm_put_str(b, value);
}

int sh_prefab_mods_slot(struct sh_module_prefab_call *call)
{
    const sh_prefab_mods_ops *o = pm_ops();
    if (!call) return 0;
    switch (call->op) {
    case SH_MODULE_PREFAB_MODS_RECORD: {
        char *view = NULL;
        int n = sh_prefab_mods_record(call->text, call->text_len, call->path, &view);
        pm_out(call, view ? view : "");
        free(view);
        return n;
    }
    case SH_MODULE_PREFAB_META_READ: {
        int source = SH_PREFAB_META_NONE;
        char *view = sh_prefab_mods_view_of(call->text, call->text_len, call->tail, call->meta_path, &source, NULL);
        pm_out(call, view ? view : "");
        free(view);
        return source;
    }
    case SH_MODULE_PREFAB_MODS_STATUS: {
        sh_modmanifest_list rows;
        size_t missing = 0;
        char *json;
        int n = sh_prefab_mods_sidecar_rows(call->text, call->text_len, &rows);
        if (n < 0) { pm_out(call, "{\"count\":0,\"missing\":0,\"mods\":[]}"); return -1; }
        json = sh_prefab_mods_status_json(&rows, &missing);
        pm_out(call, json ? json : "{\"count\":0,\"missing\":0,\"mods\":[]}");
        free(json);
        sh_modmanifest_list_free(&rows);
        return (int)missing;
    }
    case SH_MODULE_PREFAB_MODS_RESOLVE: {
        sh_modmanifest_list rows;
        pm_resolve_job job;
        int n;
        pm_out(call, "");
        call->seq = 0;
        /* The block's rows, else a sidecar's (a prefab saved before the block). */
        n = sh_prefab_mods_rows_of(call->text, call->text_len, call->meta_path, &rows);
        /* No rows: the placement goes on exactly as before. */
        if (n <= 0) { sh_modmanifest_list_free(&rows); return SH_PREFAB_MODS_PLACE; }
        memset(&job, 0, sizeof job);
        job.text = call->text;
        job.len = call->text_len;
        job.rows = &rows;
        job.name = call->name ? call->name : "";
        if (!o->call_main || !o->call_main(pm_resolve_job_main, &job)) {
            sh_modmanifest_list_free(&rows);
            pm_out(call, "The game did not answer, so this prefab's mods could not be checked. Try again.");
            return SH_PREFAB_MODS_NO_ANSWER;
        }
        sh_modmanifest_list_free(&rows);
        call->seq = job.seq;
        pm_out(call, job.msg);
        return job.result;
    }
    case SH_MODULE_PREFAB_MODS_POLL:
        return sh_prefab_mods_poll(call->seq);
    case SH_MODULE_PREFAB_META_WRITE:
        return sh_prefab_mods_meta_write(call->text, call->text_len, call->path, call->meta_path);
    case SH_MODULE_PREFAB_EXPORT: {
        char msg[PM_MSG_CAP];
        int r = sh_prefab_mods_export(call->path, call->meta_path, call->name, call->zip_path, msg, sizeof msg);
        pm_out(call, msg);
        return r;
    }
    case SH_MODULE_PREFAB_IMPORT: {
        char msg[PM_MSG_CAP], name[PM_NAME_MAX + 16];
        pm_buf b = { 0 };
        int r = sh_prefab_mods_import(call->zip_path, call->path, name, sizeof name, msg, sizeof msg);
        pm_puts(&b, r >= 0 ? "{\"ok\":true" : "{\"ok\":false");
        pm_puts(&b, r == 1 ? ",\"added\":true," : ",\"added\":false,");
        pm_json_put_kv(&b, "name", r >= 0 ? name : "", 1);
        pm_json_put_kv(&b, "message", msg, 0);
        pm_puts(&b, "}");
        pm_out(call, b.bad ? "{\"ok\":false,\"message\":\"out of memory\"}" : b.p);
        free(b.p);
        return r;
    }
    default:
        return 0;
    }
}

/* ==================================================================== */
/* the console                                                           */
/* ==================================================================== */

static char g_root[MAX_PATH * 2];

void sh_prefab_mods_set_root(const char *dir)
{
    if (dir) strncpy_s(g_root, sizeof g_root, dir, _TRUNCATE);
    else g_root[0] = 0;
}

static int pm_default_root(char *out, size_t cap);   /* engine section */

static int pm_root(char *out, size_t cap)
{
    size_t n;
    if (g_root[0]) strncpy_s(out, cap, g_root, _TRUNCATE);
    else if (!pm_default_root(out, cap)) return 0;
    n = strlen(out);
    if (n && out[n - 1] != '\\' && out[n - 1] != '/') strncat_s(out, cap, "\\", _TRUNCATE);
    return 1;
}

/* "<name>" or "<folder>/<name>": the root first, then each folder. */
static int pm_find_prefab(const char *key, char *path, size_t pcap, char *meta, size_t mcap, char *name, size_t ncap)
{
    char root[MAX_PATH * 2], folder[MAX_PATH], pattern[MAX_PATH * 2];
    const char *slash = strchr(key, '/');
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!slash) slash = strchr(key, '\\');
    if (!pm_root(root, sizeof root)) return 0;
    if (slash) {
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%.*s", (int)(slash - key), key);
        strncpy_s(name, ncap, slash + 1, _TRUNCATE);
        if (!sh_prefab_mods_valid_name(folder) || !sh_prefab_mods_valid_name(name)) return 0;
        _snprintf_s(path, pcap, _TRUNCATE, "%s%s\\%s.json", root, folder, name);
        _snprintf_s(meta, mcap, _TRUNCATE, "%s%s\\%s.meta.json", root, folder, name);
        return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    strncpy_s(name, ncap, key, _TRUNCATE);
    if (!sh_prefab_mods_valid_name(name)) return 0;
    _snprintf_s(path, pcap, _TRUNCATE, "%s%s.json", root, name);
    _snprintf_s(meta, mcap, _TRUNCATE, "%s%s.meta.json", root, name);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        _snprintf_s(path, pcap, _TRUNCATE, "%s%s\\%s.json", root, fd.cFileName, name);
        _snprintf_s(meta, mcap, _TRUNCATE, "%s%s\\%s.meta.json", root, fd.cFileName, name);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { FindClose(h); return 1; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static void pm_linef(const char *fmt, ...)
{
    char line[PM_LINE_CAP];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    pm_print(line);
}

void sh_prefab_mods_command(int argc, const char *const *argv)
{
    const char *verb = argc > 0 ? argv[0] : NULL;
    char path[MAX_PATH * 2], meta[MAX_PATH * 2], name[PM_NAME_MAX + 16], msg[PM_MSG_CAP], zip[MAX_PATH * 4];
    if (!verb || !verb[0] || !_stricmp(verb, "help") || !strcmp(verb, "?")) {
        pm_print(SH_PREFAB_CMD_HELP);
        return;
    }
    if (!_stricmp(verb, "mods")) {
        sh_modmanifest_list rows;
        char *text;
        size_t tlen = 0, i;
        int n;
        if (argc < 2 || !pm_find_prefab(argv[1], path, sizeof path, meta, sizeof meta, name, sizeof name)) {
            pm_linef("sh_prefab mods: no prefab named \"%s\".", argc > 1 ? argv[1] : "");
            return;
        }
        text = sh_prefab_mods_read_file(path, &tlen);
        if (!text) { pm_linef("Prefab %s: its file cannot be read.", name); return; }
        n = sh_prefab_mods_rows_of(text, tlen, meta, &rows);
        free(text);
        if (n < 0) { pm_linef("Prefab %s: its details are not JSON, so its mods cannot be read.", name); return; }
        if (!n) pm_linef("Prefab %s records no mods.", name);
        else pm_linef("Prefab %s needs %d mod%s:", name, n, n == 1 ? "" : "s");
        for (i = 0; i < rows.count; i++) {
            char label[SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 4];
            sh_prefab_mods_label(&rows.rows[i], label, sizeof label);
            pm_linef("  %s (bundle %.16s): %s", label, rows.rows[i].bundle,
                     pm_installed(rows.rows[i].bundle) ? "installed" : "not installed");
        }
        sh_modmanifest_list_free(&rows);
        return;
    }
    if (!_stricmp(verb, "export")) {
        char name8[PM_NAME_MAX * 3 + 16];
        if (argc < 3) { pm_print("usage: sh_prefab export \"<name>\" \"<path.zip>\""); return; }
        if (!pm_find_prefab(argv[1], path, sizeof path, meta, sizeof meta, name, sizeof name)) {
            pm_linef("sh_prefab export: no prefab named \"%s\".", argv[1]);
            return;
        }
        if (!pm_acp_to_utf8(argv[2], zip, sizeof zip)) { pm_print("sh_prefab export: the ZIP path is not usable."); return; }
        if (!pm_acp_to_utf8(name, name8, sizeof name8)) strncpy_s(name8, sizeof name8, name, _TRUNCATE);
        (void)sh_prefab_mods_export(path, meta, name8, zip, msg, sizeof msg);
        pm_print(msg);
        return;
    }
    if (!_stricmp(verb, "import")) {
        char root[MAX_PATH * 2];
        if (argc < 2) { pm_print("usage: sh_prefab import \"<path.zip or path.json>\""); return; }
        if (!pm_acp_to_utf8(argv[1], zip, sizeof zip)) { pm_print("sh_prefab import: the path is not usable."); return; }
        if (!pm_root(root, sizeof root)) { pm_print("sh_prefab import: the prefabs folder cannot be found."); return; }
        (void)sh_prefab_mods_import(zip, root, name, sizeof name, msg, sizeof msg);
        pm_print(msg);
        return;
    }
    pm_linef("sh_prefab: unknown verb \"%s\". %s", verb, SH_PREFAB_CMD_HELP);
}

#ifdef SH_PREFAB_MODS_TESTING
void sh_prefab_mods_test_reset(void)
{
    InterlockedExchange(&g_seq, 0);
    InterlockedExchange(&g_wait_seq, 0);
    InterlockedExchange(&g_ready_seq, 0);
    g_root[0] = 0;
}
#endif

/* ==================================================================== */
/* engine                                                                */
/* ==================================================================== */
#ifdef SH_PREFAB_MODS_PURE
static void pm_bind_defaults(void) { g_ops_bound = 1; }
static int pm_default_root(char *out, size_t cap) { (void)out; (void)cap; return 0; }
#else
#include <shlobj.h>
#include "commands.h"
#include "modui.h"
#include "grid_room_native.h"
#include "apply_engine.h"
#include "ui_bridge.h"

static int eng_bundle_state(const char *hash) { return sh_modserve_bundle_state(hash, NULL, 0); }
static int eng_restore(const char *const *hashes, int n)
{
    return sh_modserve_restore(hashes, n, NULL, 0, SH_MODSERVE_RESTORE_LOAD);
}

/* Does the open map list this bundle (its manifest rows, or the map scope's
 * listing, which adds the placed entities)? */
static int eng_listed(const char *hash)
{
    sh_modserve_scope_info info;
    sh_modserve_bundle_ref want, have;
    sh_modmanifest_list cur;
    size_t i;
    int listed = 0;
    if (sh_modserve_scope_enabled() && sh_modserve_bundle_scope(hash, &info) && info.listed) return 1;
    if (!sh_modserve_bundle_serving(hash, NULL, &want)) memset(&want, 0, sizeof want);
    (void)sh_modmanifest_current(&cur);
    for (i = 0; i < cur.count && !listed; i++) {
        if (!strcmp(cur.rows[i].bundle, hash)) listed = 1;
        else if (want.hash[0] && sh_modserve_bundle_serving(cur.rows[i].bundle, NULL, &have) && !strcmp(have.hash, want.hash))
            listed = 1;
    }
    sh_modmanifest_list_free(&cur);
    return listed;
}

/* The Mods tab's Use in this map, for the build that serves the row. */
static int eng_attach(const sh_modmanifest_row *row, char *note, size_t cap)
{
    sh_modserve_bundle_ref ref;
    const char *key = row->id;
    if (sh_modserve_bundle_serving(row->bundle, row->id, &ref) && ref.id[0]) key = ref.id;
    (void)sh_modui_run(SH_MODS_ACT_MAP_ADD, key);
    if (eng_listed(row->bundle)) return 1;
    if (note && cap) strncpy_s(note, cap, "the Mods tab can add it with Use in this map", _TRUNCATE);
    return -1;
}

static int eng_activate(const char *const *hashes, int n) { return modserve_activate_bundles(hashes, n); }
static int eng_module_known(const char *name) { return sh_grid_native_module_known(name); }
static void eng_print(const char *line) { sh_printf("%s\n", line); }
static void eng_toast(const char *text)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, "Prefabs", text);
}

static int eng_call_main(int (*fn)(void *), void *ctx)
{
    sh_run_on_main_fn run = NULL;
    int r = 0;
    sh_apply_engine_get_run_on_main(&run);
    if (!run) return 0;
    return run(NULL, (sh_main_call_fn)fn, ctx, 3000, &r) == 1;
}

static int eng_refresh_main(void *ctx)
{
    (void)ctx;
    return sh_modui_run(SH_MODS_ACT_RESCAN, "") >= 0;
}
static int eng_refresh(void) { return eng_call_main(eng_refresh_main, NULL); }

static void pm_bind_defaults(void)
{
    sh_prefab_mods_ops o;
    memset(&o, 0, sizeof o);
    o.bundle_state = eng_bundle_state;
    o.restore = eng_restore;
    o.listed = eng_listed;
    o.attach = eng_attach;
    o.activate = eng_activate;
    o.module_known = eng_module_known;
    o.print = eng_print;
    o.toast = eng_toast;
    o.refresh = eng_refresh;
    o.call_main = eng_call_main;
    g_ops = o;
    g_ops_bound = 1;
}

static int pm_default_root(char *out, size_t cap)
{
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\prefabs\\", base);
    return 1;
}

void h_sh_prefab(idCmdArgs *a)
{
    const char *argv[8];
    int argc = cmd_argc(a) - 1, i;
    if (argc < 0) argc = 0;
    if (argc > 8) argc = 8;
    for (i = 0; i < argc; i++) argv[i] = cmd_argv(a, i + 1);
    sh_prefab_mods_command(argc, argv);
}
#endif
