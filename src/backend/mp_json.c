/* mp_json.c -- see mp_json.h. */
#include "mp_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct mpj_in {
    const char *s, *end;
    int depth;
} mpj_in;

static char *mpj_dupn(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

mpj *mpj_new(mpj_type t)
{
    mpj *v = (mpj *)calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

mpj *mpj_new_int(long long x)
{
    char buf[32];
    mpj *v = mpj_new(MPJ_NUM);
    if (!v) return NULL;
    snprintf(buf, sizeof buf, "%lld", x);
    v->text = mpj_dupn(buf, strlen(buf));
    if (!v->text) { free(v); return NULL; }
    return v;
}

mpj *mpj_new_float(double x)
{
    char buf[48];
    mpj *v = mpj_new(MPJ_NUM);
    if (!v) return NULL;
    if (!isfinite(x)) x = 0.0;
    if (x == floor(x) && fabs(x) < 1e15) snprintf(buf, sizeof buf, "%.1f", x);
    else snprintf(buf, sizeof buf, "%.9g", x);
    v->text = mpj_dupn(buf, strlen(buf));
    if (!v->text) { free(v); return NULL; }
    return v;
}

mpj *mpj_new_str(const char *raw)
{
    mpj *v = mpj_new(MPJ_STR);
    if (!v) return NULL;
    v->text = mpj_dupn(raw ? raw : "", raw ? strlen(raw) : 0);
    if (!v->text) { free(v); return NULL; }
    return v;
}

void mpj_free(mpj *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        mpj_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->text);
    free(v);
}

static int mpj_grow(mpj *v)
{
    int cap;
    mpj **items;
    char **keys = NULL;
    if (v->n < v->cap) return 1;
    cap = v->cap ? v->cap * 2 : 8;
    items = (mpj **)realloc(v->items, (size_t)cap * sizeof *items);
    if (!items) return 0;
    v->items = items;
    if (v->type == MPJ_OBJ) {
        keys = (char **)realloc(v->keys, (size_t)cap * sizeof *keys);
        if (!keys) return 0;
        v->keys = keys;
    }
    v->cap = cap;
    return 1;
}

int mpj_push(mpj *arr, mpj *v)
{
    if (!arr || !v || arr->type != MPJ_ARR || !mpj_grow(arr)) { mpj_free(v); return 0; }
    arr->items[arr->n++] = v;
    return 1;
}

mpj *mpj_get(const mpj *obj, const char *key)
{
    int i;
    if (!obj || obj->type != MPJ_OBJ || !key) return NULL;
    for (i = 0; i < obj->n; i++) if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

int mpj_set(mpj *obj, const char *key, mpj *v)
{
    int i;
    char *k;
    if (!obj || obj->type != MPJ_OBJ || !key || !v) { mpj_free(v); return 0; }
    for (i = 0; i < obj->n; i++) {
        if (!strcmp(obj->keys[i], key)) {
            mpj_free(obj->items[i]);
            obj->items[i] = v;
            return 1;
        }
    }
    k = mpj_dupn(key, strlen(key));
    if (!k || !mpj_grow(obj)) { free(k); mpj_free(v); return 0; }
    obj->keys[obj->n] = k;
    obj->items[obj->n++] = v;
    return 1;
}

double mpj_num(const mpj *v) { return (v && v->type == MPJ_NUM && v->text) ? strtod(v->text, NULL) : 0.0; }

long long mpj_int(const mpj *v)
{
    double d;
    if (!v || v->type != MPJ_NUM || !v->text) return 0;
    if (!strpbrk(v->text, ".eE")) return strtoll(v->text, NULL, 10);
    d = strtod(v->text, NULL);
    return (long long)d;
}

int mpj_is_num(const mpj *v) { return v && v->type == MPJ_NUM; }

int mpj_str_is(const mpj *v, const char *s) { return v && v->type == MPJ_STR && s && !strcmp(v->text, s); }

/* ----------------------------------------------------------------- parse */

static void mpj_ws(mpj_in *in)
{
    while (in->s < in->end && (*in->s == ' ' || *in->s == '\t' || *in->s == '\n' || *in->s == '\r')) in->s++;
}

/* Scan a string token at in->s (on the opening quote); returns its escaped body. */
static char *mpj_strtok(mpj_in *in)
{
    const char *start;
    if (in->s >= in->end || *in->s != '"') return NULL;
    start = ++in->s;
    while (in->s < in->end) {
        unsigned char c = (unsigned char)*in->s;
        if (c == '"') { char *r = mpj_dupn(start, (size_t)(in->s - start)); in->s++; return r; }
        if (c == '\\') {
            in->s++;
            if (in->s >= in->end) return NULL;
            if (*in->s == 'u') {
                int k;
                for (k = 1; k <= 4; k++) {
                    char h = in->s + k < in->end ? in->s[k] : 0;
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return NULL;
                }
                in->s += 4;
            } else if (!strchr("\"\\/bfnrt", *in->s)) return NULL;
        } else if (c < 0x20) return NULL;
        in->s++;
    }
    return NULL;
}

static mpj *mpj_value(mpj_in *in);

static mpj *mpj_container(mpj_in *in, int obj)
{
    mpj *v = mpj_new(obj ? MPJ_OBJ : MPJ_ARR);
    char close = obj ? '}' : ']';
    if (!v) return NULL;
    if (++in->depth > MPJ_MAX_DEPTH) { mpj_free(v); return NULL; }
    in->s++;
    mpj_ws(in);
    if (in->s < in->end && *in->s == close) { in->s++; in->depth--; return v; }
    for (;;) {
        char *key = NULL;
        mpj *item;
        mpj_ws(in);
        if (obj) {
            key = mpj_strtok(in);
            if (!key) break;
            mpj_ws(in);
            if (in->s >= in->end || *in->s != ':') { free(key); break; }
            in->s++;
        }
        item = mpj_value(in);
        if (!item || !mpj_grow(v)) { free(key); mpj_free(item); break; }
        if (obj) v->keys[v->n] = key;
        v->items[v->n++] = item;
        mpj_ws(in);
        if (in->s < in->end && *in->s == ',') { in->s++; continue; }
        if (in->s < in->end && *in->s == close) { in->s++; in->depth--; return v; }
        break;
    }
    mpj_free(v);
    return NULL;
}

static mpj *mpj_value(mpj_in *in)
{
    mpj_ws(in);
    if (in->s >= in->end) return NULL;
    switch (*in->s) {
    case '{': return mpj_container(in, 1);
    case '[': return mpj_container(in, 0);
    case '"': {
        mpj *v = mpj_new(MPJ_STR);
        if (!v) return NULL;
        v->text = mpj_strtok(in);
        if (!v->text) { free(v); return NULL; }
        return v;
    }
    case 't': case 'f': case 'n': {
        static const char *words[] = { "true", "false", "null" };
        int k;
        for (k = 0; k < 3; k++) {
            size_t wl = strlen(words[k]);
            if ((size_t)(in->end - in->s) >= wl && !memcmp(in->s, words[k], wl)) {
                mpj *v = mpj_new(k == 2 ? MPJ_NULL : MPJ_BOOL);
                if (!v) return NULL;
                v->b = (k == 0);
                in->s += wl;
                return v;
            }
        }
        return NULL;
    }
    default: {
        const char *start = in->s;
        mpj *v;
        if (*in->s == '-') in->s++;
        if (in->s >= in->end || !(*in->s >= '0' && *in->s <= '9')) return NULL;
        while (in->s < in->end && ((*in->s >= '0' && *in->s <= '9') || *in->s == '.' || *in->s == 'e' ||
                                   *in->s == 'E' || *in->s == '+' || *in->s == '-')) in->s++;
        v = mpj_new(MPJ_NUM);
        if (!v) return NULL;
        v->text = mpj_dupn(start, (size_t)(in->s - start));
        if (!v->text) { free(v); return NULL; }
        return v;
    }
    }
}

mpj *mpj_parse(const char *s, size_t len)
{
    mpj_in in;
    mpj *v;
    if (!s) return NULL;
    in.s = s; in.end = s + len; in.depth = 0;
    v = mpj_value(&in);
    if (!v) return NULL;
    mpj_ws(&in);
    if (in.s != in.end) { mpj_free(v); return NULL; }
    return v;
}

mpj *mpj_clone(const mpj *v)
{
    mpj *c;
    int i;
    if (!v) return NULL;
    c = mpj_new(v->type);
    if (!c) return NULL;
    c->b = v->b;
    if (v->text && !(c->text = mpj_dupn(v->text, strlen(v->text)))) { mpj_free(c); return NULL; }
    for (i = 0; i < v->n; i++) {
        mpj *item = mpj_clone(v->items[i]);
        if (!item || !mpj_grow(c)) { mpj_free(item); mpj_free(c); return NULL; }
        if (v->type == MPJ_OBJ) {
            c->keys[c->n] = mpj_dupn(v->keys[i], strlen(v->keys[i]));
            if (!c->keys[c->n]) { mpj_free(item); mpj_free(c); return NULL; }
        }
        c->items[c->n++] = item;
    }
    return c;
}

/* ----------------------------------------------------------------- dump */

typedef struct mpj_out { char *p; size_t n, cap; int bad; } mpj_out;

static void mpj_put(mpj_out *o, const char *s, size_t n)
{
    if (o->bad) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        char *p;
        while (o->n + n + 1 > cap) cap *= 2;
        p = (char *)realloc(o->p, cap);
        if (!p) { o->bad = 1; return; }
        o->p = p; o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

static void mpj_emit(mpj_out *o, const mpj *v)
{
    int i;
    switch (v->type) {
    case MPJ_NULL: mpj_put(o, "null", 4); break;
    case MPJ_BOOL: if (v->b) mpj_put(o, "true", 4); else mpj_put(o, "false", 5); break;
    case MPJ_NUM: mpj_put(o, v->text, strlen(v->text)); break;
    case MPJ_STR: mpj_put(o, "\"", 1); mpj_put(o, v->text, strlen(v->text)); mpj_put(o, "\"", 1); break;
    case MPJ_ARR:
    case MPJ_OBJ:
        mpj_put(o, v->type == MPJ_OBJ ? "{" : "[", 1);
        for (i = 0; i < v->n; i++) {
            if (i) mpj_put(o, ",", 1);
            if (v->type == MPJ_OBJ) {
                mpj_put(o, "\"", 1);
                mpj_put(o, v->keys[i], strlen(v->keys[i]));
                mpj_put(o, "\":", 2);
            }
            mpj_emit(o, v->items[i]);
        }
        mpj_put(o, v->type == MPJ_OBJ ? "}" : "]", 1);
        break;
    }
}

char *mpj_dump(const mpj *v, size_t *len)
{
    mpj_out o;
    memset(&o, 0, sizeof o);
    if (len) *len = 0;
    if (!v) return NULL;
    mpj_emit(&o, v);
    if (o.bad || !o.p) { free(o.p); return NULL; }
    if (len) *len = o.n;
    return o.p;
}
