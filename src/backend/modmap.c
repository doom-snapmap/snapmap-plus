/* modmap.c -- see modmap.h. */
#include "modmap.h"
#include "mp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_UID_CAP (1 << 22)

static void mm_err(char *err, size_t cap, const char *text)
{
    if (err && cap) { strncpy_s(err, cap, text, _TRUNCATE); }
}

static void mm_utf8(char **w, unsigned cp)
{
    char *p = *w;
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *p++ = (char)(0xF0 | (cp >> 18)); *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F));
    }
    *w = p;
}

static int mm_hex4(const char *s, unsigned *v)
{
    int i;
    *v = 0;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        *v = (*v << 4) | d;
    }
    return 1;
}

char *sh_mm_decode(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *out = (char *)malloc(n + 1), *w = out;
    if (!out) return NULL;
    while (s && *s) {
        if (*s != '\\' || !s[1]) { *w++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'n': *w++ = '\n'; s++; break;
        case 't': *w++ = '\t'; s++; break;
        case 'r': *w++ = '\r'; s++; break;
        case 'b': *w++ = '\b'; s++; break;
        case 'f': *w++ = '\f'; s++; break;
        case 'u': {
            unsigned cp;
            if (strlen(s + 1) >= 4 && mm_hex4(s + 1, &cp)) {
                s += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && s[0] == '\\' && s[1] == 'u') {
                    unsigned lo;
                    if (strlen(s + 2) >= 4 && mm_hex4(s + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s += 6;
                    }
                }
                mm_utf8(&w, cp);   /* at most 4 bytes for the 6 or 12 read */
            } else *w++ = *s++;
            break;
        }
        default: *w++ = *s++; break;   /* \" \\ \/ */
        }
    }
    *w = 0;
    return out;
}

static char *mm_str(const mpj *v)
{
    return v && v->type == MPJ_STR ? sh_mm_decode(v->text) : sh_mm_decode("");
}

/* ------------------------------------------------------------ names */

typedef struct mm_names { char **v; size_t n, cap; int oom; } mm_names;

static void mm_names_add(mm_names *s, const mpj *str)
{
    char *name;
    size_t i;
    if (s->oom || !str || str->type != MPJ_STR || !str->text[0]) return;
    name = sh_mm_decode(str->text);
    if (!name) { s->oom = 1; return; }
    if (!name[0]) { free(name); return; }
    for (i = 0; i < s->n; i++) if (!_stricmp(s->v[i], name)) { free(name); return; }
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8;
        char **nv = (char **)realloc(s->v, nc * sizeof *nv);
        if (!nv) { free(name); s->oom = 1; return; }
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n++] = name;
}

/* The save closure's rule (sh_modmanifest_placed_names) on one subtree: a
 * string keyed inherit or moduleName, and every string inside an object
 * keyed edit. */
static void mm_walk(const mpj *v, int in_edit, mm_names *s, int depth)
{
    int i;
    if (!v || depth > MPJ_MAX_DEPTH) return;
    if (v->type == MPJ_STR) { if (in_edit) mm_names_add(s, v); return; }
    if (v->type == MPJ_ARR) { for (i = 0; i < v->n; i++) mm_walk(v->items[i], in_edit, s, depth + 1); return; }
    if (v->type != MPJ_OBJ) return;
    for (i = 0; i < v->n; i++) {
        const char *k = v->keys[i];
        const mpj *c = v->items[i];
        if (!in_edit && c && c->type == MPJ_STR && (!strcmp(k, "inherit") || !strcmp(k, "moduleName"))) mm_names_add(s, c);
        else mm_walk(c, in_edit || !strcmp(k, "edit"), s, depth + 1);
    }
}

/* ------------------------------------------------------------ ints */

static int mm_ints(const mpj *arr, long long **out, size_t *n)
{
    int i;
    *out = NULL;
    *n = 0;
    if (!arr) return 1;
    if (arr->type != MPJ_ARR) return 0;
    if (!arr->n) return 1;
    *out = (long long *)malloc((size_t)arr->n * sizeof **out);
    if (!*out) return 0;
    for (i = 0; i < arr->n; i++) {
        if (!mpj_is_num(arr->items[i])) { free(*out); *out = NULL; return 0; }
        (*out)[i] = mpj_int(arr->items[i]);
    }
    *n = (size_t)arr->n;
    return 1;
}

void sh_mm_free(sh_mm_map *m)
{
    size_t i, k;
    if (!m) return;
    for (i = 0; i < m->ent_count; i++) {
        sh_mm_entity *e = &m->ents[i];
        free(e->label); free(e->inherit); free(e->class_name);
        for (k = 0; k < e->name_count; k++) free(e->names[k]);
        free(e->names);
    }
    free(m->ents);
    for (i = 0; i < m->module_count; i++) free(m->modules[i]);
    free(m->modules);
    free(m->wire_src); free(m->wire_dst);
    free(m->ref_kv); free(m->ref_v);
    memset(m, 0, sizeof *m);
}

int sh_mm_parse(const char *json, size_t length, sh_mm_map *out, char *err, size_t err_cap)
{
    mpj *root;
    const mpj *ents, *insts, *targets, *refs;
    int i;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!json || !length) { mm_err(err, err_cap, "no map text"); return 0; }
    root = mpj_parse(json, length);
    if (!root || root->type != MPJ_OBJ) { mpj_free(root); mm_err(err, err_cap, "the map JSON could not be read"); return 0; }
    ents = mpj_get(root, "entities");
    if (!ents || ents->type != MPJ_ARR) { mpj_free(root); mm_err(err, err_cap, "the map has no entities array"); return 0; }
    if (ents->n) {
        out->ents = (sh_mm_entity *)calloc((size_t)ents->n, sizeof *out->ents);
        if (!out->ents) goto oom;
    }
    for (i = 0; i < ents->n; i++) {
        const mpj *e = ents->items[i], *def = mpj_get(e, "entityDef");
        sh_mm_entity *x = &out->ents[out->ent_count];
        mm_names s;
        long long uid = mpj_int(mpj_get(e, "uniqueId"));
        if (!e || e->type != MPJ_OBJ || uid < 0 || uid >= MM_UID_CAP) continue;
        memset(&s, 0, sizeof s);
        x->uid = (int)uid;
        x->label = mm_str(mpj_get(e, "displayName"));
        x->inherit = mm_str(def ? mpj_get(def, "inherit") : NULL);
        x->class_name = mm_str(def ? mpj_get(def, "className") : NULL);
        out->ent_count++;
        if (!x->label || !x->inherit || !x->class_name) goto oom;
        x->timeline = !strcmp(x->class_name, SH_MM_TIMELINE_CLASS);
        mm_walk(e, 0, &s, 0);
        x->names = s.v;
        x->name_count = s.n;
        if (s.oom) goto oom;
    }
    insts = mpj_get(root, "instances");
    if (insts && insts->type == MPJ_ARR && insts->n) {
        out->modules = (char **)calloc((size_t)insts->n, sizeof *out->modules);
        if (!out->modules) goto oom;
        for (i = 0; i < insts->n; i++) {
            const mpj *nm = mpj_get(insts->items[i], "moduleName");
            if (!nm || nm->type != MPJ_STR) continue;
            if (!(out->modules[out->module_count] = sh_mm_decode(nm->text))) goto oom;
            out->module_count++;
        }
    }
    targets = mpj_get(root, "targets");
    {
        long long *raw = NULL, cur = -1;
        size_t n = 0, k;
        if (!mm_ints(targets ? mpj_get(targets, "connections") : NULL, &raw, &n)) {
            sh_mm_free(out); mpj_free(root); mm_err(err, err_cap, "the wire list could not be read"); return 0;
        }
        if (n) {
            out->wire_src = (int *)malloc(n * sizeof(int));
            out->wire_dst = (int *)malloc(n * sizeof(int));
            if (!out->wire_src || !out->wire_dst) { free(raw); goto oom; }
        }
        for (k = 0; k < n; k++) {
            if (raw[k] < 0) { cur = -raw[k] - 1; continue; }
            if (cur < 0) continue;
            out->wire_src[out->wire_count] = (int)cur;
            out->wire_dst[out->wire_count] = (int)raw[k];
            out->wire_count++;
        }
        free(raw);
    }
    refs = mpj_get(root, "references");
    {
        const mpj *er = refs ? mpj_get(refs, "entityEntRefs") : NULL;
        if (!mm_ints(er ? mpj_get(er, "keyValues") : NULL, &out->ref_kv, &out->ref_kv_n) ||
            !mm_ints(er ? mpj_get(er, "values") : NULL, &out->ref_v, &out->ref_v_n)) {
            sh_mm_free(out); mpj_free(root); mm_err(err, err_cap, "the reference table could not be read"); return 0;
        }
    }
    mpj_free(root);
    return 1;
oom:
    sh_mm_free(out);
    mpj_free(root);
    mm_err(err, err_cap, "out of memory");
    return 0;
}

const sh_mm_entity *sh_mm_find(const sh_mm_map *m, int uid)
{
    size_t i;
    if (!m) return NULL;
    for (i = 0; i < m->ent_count; i++) if (m->ents[i].uid == uid) return &m->ents[i];
    return NULL;
}

static int mm_int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int mm_in(const int *sorted, size_t n, long long v)
{
    int key;
    if (v < 0 || v >= MM_UID_CAP) return 0;
    key = (int)v;
    return bsearch(&key, sorted, n, sizeof *sorted, mm_int_cmp) != NULL;
}

static int mm_push(sh_mm_ref **v, size_t *n, size_t *cap, int kind, int from, int to)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        sh_mm_ref *nv = (sh_mm_ref *)realloc(*v, nc * sizeof *nv);
        if (!nv) return 0;
        *v = nv;
        *cap = nc;
    }
    (*v)[*n].kind = kind;
    (*v)[*n].from_uid = from;
    (*v)[*n].to_uid = to;
    (*n)++;
    return 1;
}

size_t sh_mm_references(const sh_mm_map *m, const int *set, size_t n, sh_mm_ref **out)
{
    int *sorted = NULL;
    sh_mm_ref *v = NULL;
    size_t count = 0, cap = 0, i;
    if (out) *out = NULL;
    if (!m || !out || !set || !n) return 0;
    sorted = (int *)malloc(n * sizeof *sorted);
    if (!sorted) return (size_t)-1;
    memcpy(sorted, set, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, mm_int_cmp);
    for (i = 0; i < m->wire_count; i++) {
        int a = mm_in(sorted, n, m->wire_src[i]), b = mm_in(sorted, n, m->wire_dst[i]);
        if (a != b && !mm_push(&v, &count, &cap, SH_MM_REF_WIRE, m->wire_src[i], m->wire_dst[i])) goto oom;
    }
    for (i = 0; i < m->ent_count; i++) {
        const sh_mm_entity *e = &m->ents[i];
        long long b, end;
        if (!e->timeline || mm_in(sorted, n, e->uid)) continue;
        if ((size_t)e->uid + 1 >= m->ref_kv_n) continue;
        b = m->ref_kv[e->uid];
        end = m->ref_kv[e->uid + 1];
        if (b < 0 || end < b || (size_t)end > m->ref_v_n) continue;
        for (; b < end; b++)
            if (mm_in(sorted, n, m->ref_v[b])) {
                if (!mm_push(&v, &count, &cap, SH_MM_REF_TIMELINE, e->uid, (int)m->ref_v[b])) goto oom;
                break;
            }
    }
    free(sorted);
    *out = v;
    return count;
oom:
    free(sorted);
    free(v);
    return (size_t)-1;
}
