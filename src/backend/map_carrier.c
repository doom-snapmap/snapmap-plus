/* The "snapmap-plus" root member of a saved map (map_carrier.h): find it,
 * read its parts, strip it, and write it back last at the root. Text only;
 * no engine dependency. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "map_carrier.h"
#include "map_shards.h"

#define CR_ERR(...) do { if (err && err_cap) _snprintf_s(err, err_cap, _TRUNCATE, __VA_ARGS__); } while (0)

static const char *const cr_parts[3] = {"mods", "settings", "nav"};

/* ------------------------------------------------------------ scanning */

static size_t cr_ws(const char *s, size_t n, size_t i)
{
    while (i < n && sh_shard_is_ws(s[i])) i++;
    return i;
}

/* Past the string starting at s[i] == '"'; 0 when unterminated. */
static size_t cr_string(const char *s, size_t n, size_t i)
{
    const char *p, *q;
    if (i >= n || s[i] != '"') return 0;
    p = s + i + 1;
    for (;;) {
        q = (const char *)memchr(p, '"', (size_t)(s + n - p));
        if (!q) return 0;
        {
            /* An odd run of backslashes before the quote escapes it. */
            size_t bs = 0;
            while (q - bs > s + i + 1 && q[-1 - (ptrdiff_t)bs] == '\\') bs++;
            if (!(bs & 1)) return (size_t)(q - s) + 1;
        }
        p = q + 1;
    }
}

/* Past one value starting at s[i] (no leading whitespace); 0 when malformed. */
static size_t cr_value(const char *s, size_t n, size_t i)
{
    size_t depth = 0;
    if (i >= n) return 0;
    if (s[i] == '"') return cr_string(s, n, i);
    if (s[i] != '{' && s[i] != '[') {
        size_t start = i;
        while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && !sh_shard_is_ws(s[i])) i++;
        return i > start ? i : 0;
    }
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            size_t e = cr_string(s, n, i);
            if (!e) return 0;
            i = e;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (depth == 0) return 0;
            if (--depth == 0) return i + 1;
        }
        i++;
    }
    return 0;
}

typedef struct cr_member {
    size_t key_from, key_to;      /* key body, no quotes */
    size_t value_from, value_to;
    size_t prev_end;              /* end of the previous member's value, or the '{' + 1 */
    size_t next_key;              /* start of the next member's key, 0 when last */
} cr_member;

typedef int (*cr_visit_fn)(void *ctx, const char *s, const cr_member *m);

/* Walk the members of the object starting at s[i] == '{'. Returns the index
 * past its '}', 0 when malformed. The visitor sees each member once its
 * successor is known; returning 0 stops the walk (still a success). */
static size_t cr_object(const char *s, size_t n, size_t i, cr_visit_fn visit, void *ctx)
{
    cr_member m, pending;
    int have = 0;
    if (i >= n || s[i] != '{') return 0;
    memset(&m, 0, sizeof m);
    m.prev_end = i + 1;
    i = cr_ws(s, n, i + 1);
    if (i < n && s[i] == '}') return i + 1;
    for (;;) {
        size_t k_end;
        if (i >= n || s[i] != '"') return 0;
        k_end = cr_string(s, n, i);
        if (!k_end) return 0;
        if (have) {
            pending.next_key = i;
            if (visit && !visit(ctx, s, &pending)) visit = NULL;
            have = 0;
        }
        m.key_from = i + 1;
        m.key_to = k_end - 1;
        i = cr_ws(s, n, k_end);
        if (i >= n || s[i] != ':') return 0;
        i = cr_ws(s, n, i + 1);
        m.value_from = i;
        m.value_to = cr_value(s, n, i);
        if (!m.value_to) return 0;
        m.next_key = 0;
        pending = m;
        have = 1;
        m.prev_end = m.value_to;
        i = cr_ws(s, n, m.value_to);
        if (i < n && s[i] == ',') { i = cr_ws(s, n, i + 1); continue; }
        if (i < n && s[i] == '}') {
            if (visit) (void)visit(ctx, s, &pending);
            return i + 1;
        }
        return 0;
    }
}

static int cr_key_is(const char *s, const cr_member *m, const char *key)
{
    size_t kl = strlen(key);
    return m->key_to - m->key_from == kl && !memcmp(s + m->key_from, key, kl);
}

/* ------------------------------------------------------------ the view */

typedef struct cr_root_ctx { sh_carrier_view *v; } cr_root_ctx;

static int cr_root_visit(void *ctx, const char *s, const cr_member *m)
{
    sh_carrier_view *v = ((cr_root_ctx *)ctx)->v;
    if (!cr_key_is(s, m, SH_CARRIER_KEY)) return 1;
    if (v->members++ == 0) {
        v->value_from = m->value_from;
        v->value_to = m->value_to;
        if (m->prev_end > 0 && s[m->prev_end - 1] != '{') {
            /* Not the first member: from the end of the one before it. */
            v->cut_from = m->prev_end;
            v->cut_to = m->value_to;
        } else {
            v->cut_from = m->key_from - 1;
            v->cut_to = m->next_key ? m->next_key : m->value_to;
        }
    }
    return 1;
}

typedef struct cr_inner_ctx { sh_carrier_view *v; int format_ok; int bad; } cr_inner_ctx;

static int cr_inner_visit(void *ctx, const char *s, const cr_member *m)
{
    cr_inner_ctx *c = (cr_inner_ctx *)ctx;
    sh_carrier_view *v = c->v;
    char first = s[m->value_from];
    if (cr_key_is(s, m, "format")) {
        c->format_ok = (m->value_to - m->value_from == 1 && first == '1');
        return 1;
    }
    if (cr_key_is(s, m, "mods")) {
        if (first != '[' || v->mods.present) { c->bad = 1; return 0; }
        v->mods.present = 1; v->mods.from = m->value_from; v->mods.to = m->value_to;
    } else if (cr_key_is(s, m, "settings")) {
        if (first != '{' || v->settings.present) { c->bad = 1; return 0; }
        v->settings.present = 1; v->settings.from = m->value_from; v->settings.to = m->value_to;
    } else if (cr_key_is(s, m, "nav")) {
        if (first != '[' || v->nav.present) { c->bad = 1; return 0; }
        v->nav.present = 1; v->nav.from = m->value_from; v->nav.to = m->value_to;
    }
    return 1;
}

int sh_carrier_view_read(const char *json, size_t len, sh_carrier_view *view)
{
    sh_carrier_view v;
    cr_root_ctx rc;
    size_t i, end;
    memset(&v, 0, sizeof v);
    if (view) memset(view, 0, sizeof *view);
    if (!json || !len) return 0;
    /* The common case: a map that never names the key is not walked. */
    if (!sh_shard_find(json, len, "\"" SH_CARRIER_KEY "\"", SH_CARRIER_KEY_LEN + 2)) {
        i = cr_ws(json, len, 0);
        return i < len && json[i] == '{';
    }
    rc.v = &v;
    i = cr_ws(json, len, 0);
    end = cr_object(json, len, i, cr_root_visit, &rc);
    if (!end || cr_ws(json, len, end) != len) {
        memset(&v, 0, sizeof v);
        if (view) *view = v;
        return 0;
    }
    if (!v.members) { if (view) *view = v; return 1; }
    v.state = SH_CARRIER_PRESENT;
    if (v.members > 1) {
        v.state = SH_CARRIER_MALFORMED;
        _snprintf_s(v.why, sizeof v.why, _TRUNCATE, "the map has %u members named %s", v.members, SH_CARRIER_KEY);
    } else if (json[v.value_from] != '{') {
        v.state = SH_CARRIER_MALFORMED;
        strcpy_s(v.why, sizeof v.why, "it is not an object");
    } else {
        cr_inner_ctx ic;
        memset(&ic, 0, sizeof ic);
        ic.v = &v;
        if (!cr_object(json, v.value_to, v.value_from, cr_inner_visit, &ic) || ic.bad) {
            v.state = SH_CARRIER_MALFORMED;
            strcpy_s(v.why, sizeof v.why, "a part has the wrong type or appears twice");
        } else if (!ic.format_ok) {
            v.state = SH_CARRIER_MALFORMED;
            strcpy_s(v.why, sizeof v.why, "its format is not 1");
        }
    }
    if (v.state == SH_CARRIER_MALFORMED) {
        memset(&v.mods, 0, sizeof v.mods);
        memset(&v.settings, 0, sizeof v.settings);
        memset(&v.nav, 0, sizeof v.nav);
    }
    if (view) *view = v;
    return 1;
}

int sh_carrier_part(const char *json, size_t len, const char *part, size_t *from, size_t *to)
{
    sh_carrier_view v;
    const sh_carrier_span *sp;
    if (!sh_carrier_view_read(json, len, &v) || v.state == SH_CARRIER_ABSENT) return 0;
    if (v.state == SH_CARRIER_MALFORMED) return -1;
    if (!part) return 0;
    if (!strcmp(part, "mods")) sp = &v.mods;
    else if (!strcmp(part, "settings")) sp = &v.settings;
    else if (!strcmp(part, "nav")) sp = &v.nav;
    else return 0;
    if (!sp->present) return 0;
    if (from) *from = sp->from;
    if (to) *to = sp->to;
    return 1;
}

void sh_carrier_describe(const sh_carrier_view *v, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!v || v->state == SH_CARRIER_ABSENT) return;
    if (v->state == SH_CARRIER_MALFORMED) {
        _snprintf_s(out, cap, _TRUNCATE, "%s member refused (%s); the map loads without it", SH_CARRIER_KEY, v->why);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s member, %zu bytes: %s%s%s%s", SH_CARRIER_KEY,
                v->value_to - v->value_from,
                v->mods.present ? "mods " : "", v->settings.present ? "settings " : "",
                v->nav.present ? "nav " : "",
                (v->mods.present || v->settings.present || v->nav.present) ? "" : "no parts");
}

/* ------------------------------------------------------------ strip / put */

static char *cr_cut(const char *json, size_t len, size_t from, size_t to, size_t *out_len)
{
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, len - (to - from) + 1);
    if (!out) return NULL;
    memcpy(out, json, from);
    memcpy(out + from, json + to, len - to);
    out[len - (to - from)] = 0;
    if (out_len) *out_len = len - (to - from);
    return out;
}

char *sh_carrier_strip(const char *json, size_t len, size_t *out_len)
{
    sh_carrier_view v;
    char *cur = NULL;
    size_t cur_len = len;
    unsigned guard = 0;
    if (out_len) *out_len = 0;
    for (;;) {
        const char *src = cur ? cur : json;
        char *next;
        size_t next_len = 0;
        if (!sh_carrier_view_read(src, cur_len, &v) || v.state == SH_CARRIER_ABSENT || ++guard > 64) break;
        next = cr_cut(src, cur_len, v.cut_from, v.cut_to, &next_len);
        if (!next) {
            if (cur) HeapFree(GetProcessHeap(), 0, cur);
            return NULL;
        }
        if (cur) HeapFree(GetProcessHeap(), 0, cur);
        cur = next;
        cur_len = next_len;
    }
    if (cur && out_len) *out_len = cur_len;
    return cur;
}

char *sh_carrier_put(const char *json, size_t len, const char *part, const char *value, size_t value_len,
                     size_t *out_len, char *err, size_t err_cap)
{
    sh_carrier_view v;
    const char *pv[3] = {NULL, NULL, NULL};
    size_t pl[3] = {0, 0, 0}, need, w, close, before, i;
    int which = -1, any = 0, empty_root;
    char *base = NULL, *out = NULL;
    const char *src;
    size_t src_len;
    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = 0;
    for (i = 0; i < 3; i++) if (part && !strcmp(part, cr_parts[i])) which = (int)i;
    if (which < 0) { CR_ERR("unknown part"); return NULL; }
    if (!json || !len || !sh_carrier_view_read(json, len, &v)) { CR_ERR("the map text did not read"); return NULL; }
    if (v.state == SH_CARRIER_PRESENT) {
        const sh_carrier_span *sp[3] = {&v.mods, &v.settings, &v.nav};
        for (i = 0; i < 3; i++)
            if (sp[i]->present) { pv[i] = json + sp[i]->from; pl[i] = sp[i]->to - sp[i]->from; }
    }
    pv[which] = value;
    pl[which] = value ? value_len : 0;
    for (i = 0; i < 3; i++) if (pv[i]) any = 1;

    if (v.state != SH_CARRIER_ABSENT) {
        base = sh_carrier_strip(json, len, &src_len);
        if (!base) { CR_ERR("no memory to rewrite the map"); return NULL; }
        src = base;
    } else {
        src = json;
        src_len = len;
    }
    if (!any) {
        if (!base) return NULL;              /* nothing to carry, nothing carried */
        if (out_len) *out_len = src_len;
        return base;
    }
    /* The root's closing brace, and whether the root has any member. */
    close = src_len;
    while (close > 0 && sh_shard_is_ws(src[close - 1])) close--;
    if (close == 0 || src[close - 1] != '}') {
        if (base) HeapFree(GetProcessHeap(), 0, base);
        CR_ERR("the map text does not end its root object");
        return NULL;
    }
    close--;
    before = close;
    while (before > 0 && sh_shard_is_ws(src[before - 1])) before--;
    empty_root = (before > 0 && src[before - 1] == '{');

    need = src_len + 64 + SH_CARRIER_KEY_LEN;
    for (i = 0; i < 3; i++) if (pv[i]) need += pl[i] + 16;
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need);
    if (!out) {
        if (base) HeapFree(GetProcessHeap(), 0, base);
        CR_ERR("no memory to write the member");
        return NULL;
    }
    w = 0;
#define CR_EMIT(p, n) do { memcpy(out + w, (p), (n)); w += (n); } while (0)
#define CR_LIT(s) CR_EMIT((s), sizeof(s) - 1)
    CR_EMIT(src, before);
    if (!empty_root) CR_LIT(",");
    CR_LIT("\"" SH_CARRIER_KEY "\":{\"format\":1");
    for (i = 0; i < 3; i++) {
        if (!pv[i]) continue;
        CR_LIT(",\"");
        CR_EMIT(cr_parts[i], strlen(cr_parts[i]));
        CR_LIT("\":");
        CR_EMIT(pv[i], pl[i]);
    }
    CR_LIT("}");
    CR_EMIT(src + before, src_len - before);
#undef CR_LIT
#undef CR_EMIT
    out[w] = 0;
    if (base) HeapFree(GetProcessHeap(), 0, base);
    if (w == len && !memcmp(out, json, len)) {
        HeapFree(GetProcessHeap(), 0, out);
        return NULL;                           /* already that */
    }
    if (out_len) *out_len = w;
    return out;
}

/* ------------------------------------------------------------ small readers */

int sh_carrier_array_next(const char **cursor, const char *end, const char **elem, size_t *elem_len)
{
    const char *base = *cursor;
    size_t n = (size_t)(end - base), i = cr_ws(base, n, 0), after;
    if (i >= n) return -1;
    if (base[i] == '[') {
        i = cr_ws(base, n, i + 1);
        if (i < n && base[i] == ']') { *cursor = base + i + 1; return 0; }
    } else if (base[i] == ',') {
        i = cr_ws(base, n, i + 1);
    } else if (base[i] == ']') {
        *cursor = base + i + 1;
        return 0;
    } else return -1;
    after = cr_value(base, n, i);
    if (!after) return -1;
    *elem = base + i;
    *elem_len = after - i;
    i = cr_ws(base, n, after);
    if (i >= n || (base[i] != ',' && base[i] != ']')) return -1;
    *cursor = base + i;
    return 1;
}

typedef struct cr_find_ctx { const char *key; const char *value; size_t value_len; int found; } cr_find_ctx;

static int cr_find_visit(void *ctx, const char *s, const cr_member *m)
{
    cr_find_ctx *c = (cr_find_ctx *)ctx;
    if (!cr_key_is(s, m, c->key)) return 1;
    c->value = s + m->value_from;
    c->value_len = m->value_to - m->value_from;
    c->found = 1;
    return 0;
}

int sh_carrier_object_member(const char *obj, const char *end, const char *key, const char **value,
                             size_t *value_len)
{
    cr_find_ctx c;
    size_t n = (size_t)(end - obj), i;
    memset(&c, 0, sizeof c);
    c.key = key;
    i = cr_ws(obj, n, 0);
    if (!cr_object(obj, n, i, cr_find_visit, &c)) return -1;
    if (c.found) {
        if (value) *value = c.value;
        if (value_len) *value_len = c.value_len;
    }
    return c.found;
}

int sh_carrier_object_string(const char *obj, const char *end, const char *key, const char **body,
                             size_t *body_len)
{
    const char *v = NULL;
    size_t n = 0;
    int got = sh_carrier_object_member(obj, end, key, &v, &n);
    if (got != 1) return got;
    if (*v != '"' || n < 2) return -1;
    if (body) *body = v + 1;
    if (body_len) *body_len = n - 2;
    return 1;
}
