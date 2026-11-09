/* The open map's Snapmap+ settings rows: read and removed at load, written
 * back at save, held in between (map_settings.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "map_settings.h"
#include "map_shards.h"

void backend_log(const char *message);

#define MS_RENDER 0
#define MS_BOUNDS 1
#define MS_PRODUCT (-1)
#define MS_AUTHOR (-2)
#define MS_VALUE_MAX 192

static const char *const ms_names[2] = {SH_RENDER_VARIABLE, SH_BOUNDS_VARIABLE};
/* Rows Snapmap+ writes and removes itself; no logic node binds to them. */
static const char *const ms_product_prefixes[] = {"smp.", "smnav1.", "smpmod1.", "smpprod1.", "smpkg."};

void sh_map_settings_default(sh_map_settings *s)
{
    if (!s) return;
    sh_map_render_default(&s->render);
    sh_map_bounds_default(&s->bounds);
}

/* ------------------------------------------------------------ the string list */

typedef struct ms_elem {
    size_t from, to;          /* the element, '{' to '}' inclusive-exclusive */
    size_t vfrom, vto;        /* initialValue body (no quotes); vto == 0 when absent */
    int which;                /* MS_RENDER, MS_BOUNDS, MS_PRODUCT or MS_AUTHOR */
} ms_elem;

typedef struct ms_list {
    sh_shard_doc doc;
    int bucket;
    ms_elem *e;
    size_t n;
} ms_list;

static void ms_list_free(ms_list *l)
{
    sh_shard_doc_free(&l->doc);
    if (l->e) HeapFree(GetProcessHeap(), 0, l->e);
    memset(l, 0, sizeof *l);
}

/* The body span of the string value of member `key` directly inside `parent`. */
static int ms_member_string(const char *json, size_t len, const sh_shard_doc *doc, int parent,
                            const char *key, size_t *from, size_t *to)
{
    size_t klen = strlen(key), at, stop, v, q;
    if (parent < 0 || (size_t)parent >= doc->count) return 0;
    at = doc->c[parent].open;
    stop = doc->c[parent].close;
    while (at < stop) {
        const char *k = sh_shard_find(json + at, stop - at, key, klen);
        size_t koff;
        if (!k) return 0;
        koff = (size_t)(k - json);
        at = koff + 1;
        if (koff == 0 || json[koff - 1] != '"' || koff + klen >= len || json[koff + klen] != '"') continue;
        if (sh_shard_doc_innermost(doc, koff) != parent) continue;
        v = koff + klen + 1;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != ':') continue;
        v++;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != '"') return 0;
        for (q = v + 1; q < stop; q++) {
            if (json[q] == '\\') { q++; continue; }
            if (json[q] == '"') break;
        }
        if (q >= stop) return 0;
        *from = v + 1;
        *to = q;
        return 1;
    }
    return 0;
}

static int ms_classify(const char *body, size_t n)
{
    size_t i;
    for (i = 0; i < 2; i++)
        if (n == strlen(ms_names[i]) && !memcmp(body, ms_names[i], n)) return (int)i;
    for (i = 0; i < sizeof ms_product_prefixes / sizeof ms_product_prefixes[0]; i++) {
        size_t m = strlen(ms_product_prefixes[i]);
        if (n >= m && !memcmp(body, ms_product_prefixes[i], m)) return MS_PRODUCT;
    }
    return MS_AUTHOR;
}

/* Every element of variables.string[], classified by its name. 0 when the
 * text does not read as a map with that list. */
static int ms_list_read(const char *json, size_t len, ms_list *l)
{
    int vars;
    size_t i, cap = 0;
    unsigned expected;
    memset(l, 0, sizeof *l);
    l->bucket = -1;
    if (!sh_shard_doc_build(json, len, &l->doc)) return 0;
    vars = sh_shard_doc_member(json, len, &l->doc, 0, "variables");
    if (vars < 0 || l->doc.c[vars].kind != '{') goto fail;
    l->bucket = sh_shard_doc_member(json, len, &l->doc, vars, "string");
    if (l->bucket < 0 || l->doc.c[l->bucket].kind != '[') goto fail;
    expected = sh_shard_doc_array_count(json, &l->doc, l->bucket);
    for (i = (size_t)l->bucket + 1; i < l->doc.count && l->doc.c[i].open < l->doc.c[l->bucket].close; i++) {
        ms_elem *e;
        int info;
        size_t nf, nt;
        if (l->doc.c[i].parent != l->bucket) continue;
        if (l->doc.c[i].kind != '{') goto fail;
        if (l->n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            ms_elem *grown = l->e ? (ms_elem *)HeapReAlloc(GetProcessHeap(), 0, l->e, ncap * sizeof *grown)
                                  : (ms_elem *)HeapAlloc(GetProcessHeap(), 0, ncap * sizeof *grown);
            if (!grown) goto fail;
            l->e = grown;
            cap = ncap;
        }
        e = &l->e[l->n++];
        memset(e, 0, sizeof *e);
        e->from = l->doc.c[i].open;
        e->to = l->doc.c[i].close + 1;
        e->which = MS_AUTHOR;
        info = sh_shard_doc_member(json, len, &l->doc, (int)i, "info");
        if (info >= 0 && ms_member_string(json, len, &l->doc, info, "name", &nf, &nt))
            e->which = ms_classify(json + nf, nt - nf);
        if (!ms_member_string(json, len, &l->doc, (int)i, "initialValue", &e->vfrom, &e->vto))
            e->vfrom = e->vto = 0;
    }
    /* Anything in the list that is not an object is not a map we understand. */
    if (l->n != expected) goto fail;
    return 1;
fail:
    ms_list_free(l);
    return 0;
}

/* Only Snapmap+ rows follow element k. */
static int ms_removable(const ms_list *l, size_t k)
{
    size_t j;
    for (j = k + 1; j < l->n; j++) if (l->e[j].which == MS_AUTHOR) return 0;
    return 1;
}

static int ms_decode(const char *json, const ms_elem *e, sh_map_settings *s)
{
    char text[MS_VALUE_MAX + 1];
    size_t n = e->vto - e->vfrom;
    if (!e->vto || n == 0 || n > MS_VALUE_MAX || memchr(json + e->vfrom, '\\', n)) return 0;
    memcpy(text, json + e->vfrom, n);
    text[n] = 0;
    if (e->which == MS_RENDER) {
        sh_map_render r;
        if (!sh_map_render_decode(text, &r) || !sh_map_render_valid(&r)) return 0;
        s->render = r;
        return 1;
    }
    {
        sh_map_bounds b;
        if (!sh_map_bounds_decode(text, &b) || !sh_map_bounds_valid(&b)) return 0;
        s->bounds = b;
        return 1;
    }
}

/* Rebuild the text with element k dropped when drop[k], and element k's value
 * body replaced by value[k] when value[k] is not NULL. allocCount[4] follows
 * the new count when anything was dropped. */
static char *ms_rebuild(const char *json, size_t len, const ms_list *l, const unsigned char *drop,
                        const char *const *value, size_t *out_len)
{
    size_t need = len + 1, w = 0, k, open = l->doc.c[l->bucket].open, close = l->doc.c[l->bucket].close;
    int first = 1, dropped = 0;
    char *out;
    for (k = 0; k < l->n; k++) if (value[k]) need += strlen(value[k]);
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 16);
    if (!out) return NULL;
#define MS_EMIT(p, n) do { memcpy(out + w, (p), (n)); w += (n); } while (0)
    MS_EMIT(json, open + 1);
    for (k = 0; k < l->n; k++) {
        const ms_elem *e = &l->e[k];
        if (drop[k]) { dropped = 1; continue; }
        /* The leading space when it is now the first, else the separator
         * that preceded it originally (every separator is a comma). */
        if (first) MS_EMIT(json + open + 1, l->e[0].from - (open + 1));
        else MS_EMIT(json + l->e[k - 1].to, e->from - l->e[k - 1].to);
        first = 0;
        if (value[k] && e->vto) {
            MS_EMIT(json + e->from, e->vfrom - e->from);
            MS_EMIT(value[k], strlen(value[k]));
            MS_EMIT(json + e->vto, e->to - e->vto);
        } else MS_EMIT(json + e->from, e->to - e->from);
    }
    if (l->n) MS_EMIT(json + l->e[l->n - 1].to, close - l->e[l->n - 1].to);
    else MS_EMIT(json + open + 1, close - (open + 1));
    MS_EMIT(json + close, len - close);
#undef MS_EMIT
    out[w] = 0;
    if (dropped) {
        sh_shard_doc d;
        int vars, bucket, alloc;
        size_t from, to;
        char number[16];
        int digits;
        if (!sh_shard_doc_build(out, w, &d)) { HeapFree(GetProcessHeap(), 0, out); return NULL; }
        vars = sh_shard_doc_member(out, w, &d, 0, "variables");
        bucket = vars >= 0 ? sh_shard_doc_member(out, w, &d, vars, "string") : -1;
        alloc = vars >= 0 ? sh_shard_doc_member(out, w, &d, vars, "allocCount") : -1;
        if (bucket >= 0 && alloc >= 0 && sh_shard_doc_flat_element(out, &d, alloc, 4, &from, &to)) {
            digits = _snprintf_s(number, sizeof number, _TRUNCATE, "%u", sh_shard_doc_array_count(out, &d, bucket));
            if (digits < 0 || w - (to - from) + (size_t)digits > need + 15) {
                sh_shard_doc_free(&d); HeapFree(GetProcessHeap(), 0, out); return NULL;
            }
            memmove(out + from + digits, out + to, w - to + 1);
            memcpy(out + from, number, (size_t)digits);
            w = w - (to - from) + (size_t)digits;
        }
        sh_shard_doc_free(&d);
    }
    if (out_len) *out_len = w;
    return out;
}

static int ms_mentions(const char *json, size_t len)
{
    return sh_shard_find(json, len, SH_RENDER_VARIABLE, strlen(SH_RENDER_VARIABLE)) ||
           sh_shard_find(json, len, SH_BOUNDS_VARIABLE, strlen(SH_BOUNDS_VARIABLE));
}

char *sh_map_settings_take(const char *json, size_t len, sh_map_settings *out, size_t *out_len,
                           sh_map_settings_report *report)
{
    sh_map_settings_report r;
    sh_map_settings s;
    ms_list l;
    unsigned char *drop = NULL;
    const char **value = NULL;
    unsigned count[2] = {0, 0}, good[2] = {0, 0};
    size_t k;
    char *result = NULL;
    memset(&r, 0, sizeof r);
    sh_map_settings_default(&s);
    if (out_len) *out_len = 0;
    if (!json || !len || !ms_mentions(json, len)) goto done;
    if (!ms_list_read(json, len, &l)) { r.failed = 1; goto done; }
    if (l.n) {
        drop = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l.n);
        value = (const char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l.n * sizeof *value);
        if (!drop || !value) { r.failed = 1; goto free_list; }
    }
    for (k = 0; k < l.n; k++) {
        int which = l.e[k].which;
        if (which != MS_RENDER && which != MS_BOUNDS) continue;
        r.found++;
        count[which]++;
        if (ms_decode(json, &l.e[k], &s)) good[which]++;
        if (ms_removable(&l, k)) { drop[k] = 1; r.removed++; }
        else r.kept++;
    }
    /* A duplicated or unreadable setting is off, as earlier builds read it. */
    if (count[MS_RENDER] && (count[MS_RENDER] > 1 || !good[MS_RENDER])) {
        sh_map_render_default(&s.render);
        r.refused += count[MS_RENDER];
    }
    if (count[MS_BOUNDS] && (count[MS_BOUNDS] > 1 || !good[MS_BOUNDS])) {
        sh_map_bounds_default(&s.bounds);
        r.refused += count[MS_BOUNDS];
    }
    if (r.removed) {
        result = ms_rebuild(json, len, &l, drop, value, out_len);
        if (!result) { r.failed = 1; r.kept += r.removed; r.removed = 0; }
    }
free_list:
    if (drop) HeapFree(GetProcessHeap(), 0, drop);
    if (value) HeapFree(GetProcessHeap(), 0, (void *)value);
    ms_list_free(&l);
done:
    if (out) *out = s;
    if (report) *report = r;
    return result;
}

/* The row text for a setting that is on; NULL when it is off. */
static const char *ms_text(int which, const sh_map_settings *s, char *buf, size_t cap)
{
    if (which == MS_RENDER) return sh_map_render_encode(&s->render, buf, cap) ? buf : NULL;
    if (!sh_map_bounds_overrides(&s->bounds)) return NULL;
    return sh_map_bounds_encode(&s->bounds, buf, cap) ? buf : NULL;
}

char *sh_map_settings_put(const char *json, size_t len, const sh_map_settings *settings,
                          size_t *out_len, sh_map_settings_report *report)
{
    static const char *const off_text[2] = {"4;8192", "1;0"};
    sh_map_settings_report r;
    char text[2][MS_VALUE_MAX + 1];
    const char *want[2];
    int placed[2] = {0, 0}, i;
    char *body = NULL, *result = NULL;
    size_t body_len = len, k;
    memset(&r, 0, sizeof r);
    if (out_len) *out_len = 0;
    if (!json || !len || !settings) { r.failed = 1; goto done; }
    for (i = 0; i < 2; i++) want[i] = ms_text(i, settings, text[i], sizeof text[i]);
    if (ms_mentions(json, len)) {
        ms_list l;
        unsigned char *drop;
        const char **value;
        int changed = 0;
        if (!ms_list_read(json, len, &l)) { r.failed = 1; goto done; }
        drop = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l.n ? l.n : 1);
        value = (const char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (l.n ? l.n : 1) * sizeof *value);
        if (!drop || !value) {
            if (drop) HeapFree(GetProcessHeap(), 0, drop);
            if (value) HeapFree(GetProcessHeap(), 0, (void *)value);
            ms_list_free(&l);
            r.failed = 1;
            goto done;
        }
        for (k = 0; k < l.n; k++) {
            int which = l.e[k].which;
            const char *t;
            size_t n;
            if (which != MS_RENDER && which != MS_BOUNDS) continue;
            r.found++;
            if (ms_removable(&l, k)) { drop[k] = 1; r.removed++; changed = 1; continue; }
            /* An author's variable follows: the row keeps its index. */
            r.kept++;
            placed[which] = 1;
            t = want[which] ? want[which] : off_text[which];
            n = l.e[k].vto - l.e[k].vfrom;
            if (l.e[k].vto && n == strlen(t) && !memcmp(json + l.e[k].vfrom, t, n)) continue;
            if (!l.e[k].vto) continue;    /* no initialValue to rewrite; left as it is */
            value[k] = t;
            r.written++;
            changed = 1;
        }
        if (changed) {
            body = ms_rebuild(json, len, &l, drop, value, &body_len);
            if (!body) r.failed = 1;
        }
        HeapFree(GetProcessHeap(), 0, drop);
        HeapFree(GetProcessHeap(), 0, (void *)value);
        ms_list_free(&l);
        if (r.failed) goto done;
    }
    {
        sh_shard_out rows[2];
        size_t n = 0;
        char err[160];
        for (i = 0; i < 2; i++) if (want[i] && !placed[i]) {
            rows[n].header = ms_names[i];
            rows[n].chunk = want[i];
            rows[n].chunk_len = strlen(want[i]);
            n++;
        }
        if (n) {
            result = sh_shard_insert(body ? body : json, body ? body_len : len, rows, n, out_len, err, sizeof err);
            if (!result) { r.failed = 1; goto done; }
            r.written += (unsigned)n;
            if (body) HeapFree(GetProcessHeap(), 0, body);
            body = NULL;
        } else if (body) {
            result = body;
            body = NULL;
            if (out_len) *out_len = body_len;
        }
    }
done:
    if (body) HeapFree(GetProcessHeap(), 0, body);
    if (r.failed && out_len) *out_len = 0;
    if (report) *report = r;
    return r.failed ? NULL : result;
}

/* ------------------------------------------------------------ the open map */

static SRWLOCK g_lock = SRWLOCK_INIT;
static sh_map_settings g_current = {{0}, {0}};
static sh_map_settings g_pending = {{0}, {0}};
static sh_map_settings g_previous = {{0}, {0}};
static int g_previous_valid;

static void ms_describe(const sh_map_settings *s, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "Unlock View Distance %s, Unlock Map Bounds %s",
                sh_map_render_unlocked(&s->render) ? "on" : "off",
                sh_map_bounds_enabled(&s->bounds) ? "on" : "off");
}

char *sh_map_settings_prepare_load(const char *json, size_t len, size_t *out_len)
{
    sh_map_settings s;
    sh_map_settings_report r;
    char *out, line[320], what[96];
    out = sh_map_settings_take(json, len, &s, out_len, &r);
    AcquireSRWLockExclusive(&g_lock);
    g_pending = s;
    ReleaseSRWLockExclusive(&g_lock);
    if (r.found || r.failed) {
        ms_describe(&s, what, sizeof what);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "SETTINGS: map read: %s; %u settings row(s), %u removed before parse, %u kept in place "
            "(an author variable follows), %u unreadable%s",
            what, r.found, r.removed, r.kept, r.refused,
            r.failed ? "; the map text did not read, its rows stay as string variables" : "");
        backend_log(line);
    }
    return out;
}

void sh_map_settings_loaded(int ok)
{
    if (!ok) return;
    AcquireSRWLockExclusive(&g_lock);
    g_previous = g_current;
    g_previous_valid = 1;
    g_current = g_pending;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_current(sh_map_settings *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_lock);
    *out = g_current;
    ReleaseSRWLockShared(&g_lock);
}

void sh_map_settings_set_render(const sh_map_render *render)
{
    if (!render || !sh_map_render_valid(render)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_current.render = *render;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_set_bounds(const sh_map_bounds *bounds)
{
    if (!bounds || !sh_map_bounds_valid(bounds)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_current.bounds = *bounds;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_map_settings_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    sh_map_settings_default(&g_current);
    g_previous_valid = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int sh_map_settings_restore_previous(void)
{
    int ok;
    AcquireSRWLockExclusive(&g_lock);
    ok = g_previous_valid;
    if (ok) g_current = g_previous;
    g_previous_valid = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

char *sh_map_settings_on_save(const char *json, size_t len, size_t *out_len)
{
    sh_map_settings s;
    sh_map_settings_report r;
    char *out, line[320], what[96];
    sh_map_settings_current(&s);
    out = sh_map_settings_put(json, len, &s, out_len, &r);
    if (r.found || r.written || r.failed) {
        ms_describe(&s, what, sizeof what);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "SETTINGS: map saved: %s; %u row(s) written, %u stale row(s) removed, %u kept in place%s",
            what, r.written, r.removed, r.kept,
            r.failed ? "; FAILED: the saved text did not read, it carries the rows it had" : "");
        backend_log(line);
    }
    return out;
}
