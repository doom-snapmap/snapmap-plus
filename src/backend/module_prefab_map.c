/* module_prefab_map.c -- see module_prefab_map.h. */
#include "module_prefab_map.h"
#include "mp_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define MP_VAR_TYPES       10
#define MP_VAR_PERSISTENT  9
#define MP_VAR_UNBOUND     0xFFFFFF
#define MP_UID_CAP         (1 << 22)

static const char *const k_var_keys[MP_VAR_TYPES] = {
    "integer", "number", "boolean", "color", "string",
    "playerResource", "teamResource", "cachedEntity", "customEvent", "persistentInteger"
};

/* Copy into a SH_MP_NAME_CAP buffer, truncating. */
static void mp_copy(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > SH_MP_NAME_CAP - 1) n = SH_MP_NAME_CAP - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void mp_err(char *err, int cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap <= 0) return;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)cap, fmt, ap);
    va_end(ap);
}

void sh_mp_summary_free(sh_mp_summary *s)
{
    if (!s) return;
    free(s->modules);
    s->modules = NULL;
}

/* ------------------------------------------------------------ int arrays */

typedef struct mp_ints { long long *v; int n, cap; } mp_ints;

static int ints_push(mp_ints *a, long long x)
{
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 16;
        long long *v = (long long *)realloc(a->v, (size_t)cap * sizeof *v);
        if (!v) return 0;
        a->v = v; a->cap = cap;
    }
    a->v[a->n++] = x;
    return 1;
}

/* Read an array of integers; 0 when not an array of numbers. */
static int ints_read(const mpj *arr, mp_ints *out)
{
    int i;
    memset(out, 0, sizeof *out);
    if (!arr || arr->type != MPJ_ARR) return 0;
    for (i = 0; i < arr->n; i++) {
        if (!mpj_is_num(arr->items[i]) || !ints_push(out, mpj_int(arr->items[i]))) { free(out->v); out->v = NULL; return 0; }
    }
    return 1;
}

static mpj *ints_node(const mp_ints *a)
{
    mpj *arr = mpj_new(MPJ_ARR);
    int i;
    if (!arr) return NULL;
    for (i = 0; i < a->n; i++) if (!mpj_push(arr, mpj_new_int(a->v[i]))) { mpj_free(arr); return NULL; }
    return arr;
}

/* Replace obj.key's array with a. */
static int ints_store(mpj *obj, const char *key, const mp_ints *a)
{
    mpj *n = ints_node(a);
    return n ? mpj_set(obj, key, n) : 0;
}

/* ------------------------------------------------------------ CSR rows */

/* An idIndexMultimap keyed by uniqueId: row u = values[kv[u] .. kv[u+1]). */
typedef struct mp_csr { mp_ints kv, values; } mp_csr;

static int csr_read(const mpj *mm, mp_csr *c)
{
    int i;
    memset(c, 0, sizeof *c);
    if (!mm || mm->type != MPJ_OBJ) return 0;
    if (!ints_read(mpj_get(mm, "keyValues"), &c->kv) || !ints_read(mpj_get(mm, "values"), &c->values)) {
        free(c->kv.v); free(c->values.v); memset(c, 0, sizeof *c); return 0;
    }
    for (i = 0; i < c->kv.n; i++) {
        if (c->kv.v[i] < 0 || c->kv.v[i] > c->values.n || (i && c->kv.v[i] < c->kv.v[i - 1])) {
            free(c->kv.v); free(c->values.v); memset(c, 0, sizeof *c); return 0;
        }
    }
    return 1;
}

static void csr_free(mp_csr *c) { free(c->kv.v); free(c->values.v); memset(c, 0, sizeof *c); }

/* Row bounds for key u; an absent key is an empty row. */
static void csr_row(const mp_csr *c, long long u, long long *b, long long *e)
{
    if (u < 0 || u + 1 >= c->kv.n) { *b = *e = 0; return; }
    *b = c->kv.v[u]; *e = c->kv.v[u + 1];
}

/* ------------------------------------------------------------ uid map */

typedef struct mp_idmap { int *to; long long cap; } mp_idmap;   /* to[old] = new, -1 none */

static int idmap_init(mp_idmap *m, long long cap)
{
    long long i;
    m->cap = cap;
    m->to = (int *)malloc((size_t)(cap > 0 ? cap : 1) * sizeof *m->to);
    if (!m->to) return 0;
    for (i = 0; i < cap; i++) m->to[i] = -1;
    return 1;
}
static int idmap_get(const mp_idmap *m, long long u) { return (u >= 0 && u < m->cap) ? m->to[u] : -1; }

/* ------------------------------------------------------------ helpers */

static int mp_is_map(const mpj *root) { return root && root->type == MPJ_OBJ && mpj_str_is(mpj_get(root, "~type"), "idSnapMap"); }

static long long ent_uid(const mpj *e) { return mpj_int(mpj_get(e, "uniqueId")); }

static int ent_is_nav_box(const mpj *e)
{
    const mpj *def = mpj_get(e, "entityDef"), *state, *edit, *flags, *nf;
    if (!def || !mpj_str_is(mpj_get(def, "inherit"), "snapmaps/volume/blocking")) return 0;
    state = mpj_get(def, "state"); edit = mpj_get(state, "edit"); flags = mpj_get(edit, "flags");
    nf = mpj_get(flags, "noFlood");
    return nf && nf->type == MPJ_BOOL && nf->b;
}

static void vec_read(const mpj *o, float v[3])
{
    v[0] = (float)mpj_num(mpj_get(o, "x"));
    v[1] = (float)mpj_num(mpj_get(o, "y"));
    v[2] = (float)mpj_num(mpj_get(o, "z"));
}

/* Overwrite origin x/y/z, keeping the ~type member and order. */
static int vec_write(mpj *o, const float v[3])
{
    return mpj_set(o, "x", mpj_new_float(v[0])) && mpj_set(o, "y", mpj_new_float(v[1])) &&
           mpj_set(o, "z", mpj_new_float(v[2]));
}

static void summary_add_env(sh_mp_summary *s, const mpj *inst)
{
    const mpj *env = mpj_get(inst, "environmentName");
    int k;
    if (!env || env->type != MPJ_STR) return;
    for (k = 0; k < s->env_count; k++) if (!strcmp(s->envs[k], env->text)) return;
    if (s->env_count < SH_MP_MAX_ENVS) {
        mp_copy(s->envs[s->env_count], env->text);
        s->envs[s->env_count][SH_MP_NAME_CAP - 1] = 0;
        s->env_count++;
    }
}

/* An empty idIndexMultimap with the source's ~type spelling. */
static mpj *empty_multimap(void)
{
    mpj *o = mpj_new(MPJ_OBJ);
    if (!o) return NULL;
    if (!mpj_set(o, "keyValues", mpj_new(MPJ_ARR)) || !mpj_set(o, "values", mpj_new(MPJ_ARR)) ||
        !mpj_set(o, "~type", mpj_new_str("idIndexMultimap"))) { mpj_free(o); return NULL; }
    return o;
}

/* Decode the connection run-length list into (source, target) pairs. */
static int conn_pairs(const mpj *arr, mp_ints *src, mp_ints *dst)
{
    mp_ints raw;
    long long cur = -1;
    int i;
    memset(src, 0, sizeof *src); memset(dst, 0, sizeof *dst);
    if (!arr) return 1;
    if (!ints_read(arr, &raw)) return 0;
    for (i = 0; i < raw.n; i++) {
        if (raw.v[i] < 0) { cur = -raw.v[i] - 1; continue; }
        if (cur < 0 || !ints_push(src, cur) || !ints_push(dst, raw.v[i])) { free(raw.v); return 0; }
    }
    free(raw.v);
    return 1;
}

/* Append pairs to a run-length list, grouping consecutive equal sources. */
static int conn_encode(mp_ints *out, const mp_ints *src, const mp_ints *dst)
{
    long long cur = -1;
    int i;
    for (i = 0; i < src->n; i++) {
        if (src->v[i] != cur) { cur = src->v[i]; if (!ints_push(out, -(cur + 1))) return 0; }
        if (!ints_push(out, dst->v[i])) return 0;
    }
    return 1;
}

/* Portal slot owners: slot i belongs to the instance whose group holds its door
 * entity. Slots must run in instance order. first[k]/count[k] per instance. */
static int door_slots(const mp_ints *doors, const int *group_of, long long uid_cap, int ninst,
                      int *first, int *count, char *err, int errcap)
{
    int i, last = -1;
    for (i = 0; i < ninst; i++) { first[i] = -1; count[i] = 0; }
    for (i = 0; i < doors->n; i++) {
        long long d = doors->v[i];
        int g = (d >= 0 && d < uid_cap) ? group_of[d] : -1;
        if (g < 0 || g >= ninst) { mp_err(err, errcap, "portal %d has no door entity in a module", i); return 0; }
        if (g < last) { mp_err(err, errcap, "portal slots are not in module order (slot %d)", i); return 0; }
        if (first[g] < 0) first[g] = i;
        else if (first[g] + count[g] != i) { mp_err(err, errcap, "module %d portal slots are not contiguous", g); return 0; }
        count[g]++;
        last = g;
    }
    return 1;
}

/* ------------------------------------------------------------ cut */

int sh_mp_cut(const char *map, size_t len, const int *sel, int nsel,
              char **out, size_t *out_len, sh_mp_summary *sum, char *err, int errcap)
{
    mpj *root = NULL, *frag = NULL, *instances, *ie, *ents, *dac, *targets, *refs, *vars;
    mp_ints ie_kv = {0}, ie_vals = {0}, doors = {0}, frames = {0}, csrc = {0}, cdst = {0};
    mp_ints nk = {0}, nv = {0}, ndoors = {0}, nframes = {0}, ncsrc = {0}, ncdst = {0}, nconn = {0};
    mp_csr er = {0}, vr = {0};
    mp_ints ner_kv = {0}, ner_v = {0}, nvr_kv = {0}, nvr_v = {0};
    int *group_of = NULL, *in_set = NULL, *dfirst = NULL, *dcount = NULL, *chosen = NULL;
    int *var_new[MP_VAR_TYPES] = {0};
    int var_len[MP_VAR_TYPES] = {0};
    long long uid_cap = 0, max_uid = -1;
    int ninst, i, k, ok = 0;
    float anchor[3];

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (sum) memset(sum, 0, sizeof *sum);
    if (!map || !sel || nsel <= 0 || !out || !sum) { mp_err(err, errcap, "nothing to cut"); return 0; }
    if (nsel > SH_MP_MAX_MODULES) { mp_err(err, errcap, "too many modules (%d)", nsel); return 0; }
    root = mpj_parse(map, len);
    if (!mp_is_map(root)) { mp_err(err, errcap, "the map JSON could not be read"); goto done; }
    instances = mpj_get(root, "instances");
    ie = mpj_get(root, "instanceEntities");
    ents = mpj_get(root, "entities");
    dac = mpj_get(root, "doorsAndCaps");
    targets = mpj_get(root, "targets");
    refs = mpj_get(root, "references");
    vars = mpj_get(root, "variables");
    if (!instances || instances->type != MPJ_ARR || !ents || ents->type != MPJ_ARR || !ie || !dac || !refs || !vars) {
        mp_err(err, errcap, "the map is missing a required member"); goto done;
    }
    ninst = instances->n;
    if (!ints_read(mpj_get(ie, "keyValues"), &ie_kv) || !ints_read(mpj_get(ie, "values"), &ie_vals) ||
        ie_kv.n != ninst + 2) { mp_err(err, errcap, "instanceEntities does not match the %d modules", ninst); goto done; }
    for (i = 0; i < ents->n; i++) { long long u = ent_uid(ents->items[i]); if (u > max_uid) max_uid = u; }
    if (max_uid < 0 || max_uid >= MP_UID_CAP) { mp_err(err, errcap, "entity ids are out of range"); goto done; }
    if (!csr_read(mpj_get(refs, "entityEntRefs"), &er) || !csr_read(mpj_get(refs, "entityVarRefs"), &vr)) {
        mp_err(err, errcap, "the reference tables could not be read"); goto done;
    }
    uid_cap = max_uid + 1;
    group_of = (int *)malloc((size_t)uid_cap * sizeof *group_of);
    in_set = (int *)calloc((size_t)uid_cap, sizeof *in_set);
    chosen = (int *)calloc((size_t)ninst + 1, sizeof *chosen);
    dfirst = (int *)malloc(((size_t)ninst + 1) * sizeof *dfirst);
    dcount = (int *)malloc(((size_t)ninst + 1) * sizeof *dcount);
    if (!group_of || !in_set || !chosen || !dfirst || !dcount) { mp_err(err, errcap, "out of memory"); goto done; }
    for (i = 0; i < uid_cap; i++) group_of[i] = -1;
    for (k = 0; k <= ninst; k++) {
        long long b = ie_kv.v[k], e = ie_kv.v[k + 1];
        if (b < 0 || e < b || e > ie_vals.n) { mp_err(err, errcap, "instanceEntities offsets are invalid"); goto done; }
        for (; b < e; b++) { long long u = ie_vals.v[b]; if (u >= 0 && u < uid_cap) group_of[u] = k; }
    }
    for (i = 0; i < nsel; i++) {
        if (sel[i] < 0 || sel[i] >= ninst) { mp_err(err, errcap, "module %d is not in this map", sel[i]); goto done; }
        if (chosen[sel[i]]) { mp_err(err, errcap, "module %d was chosen twice", sel[i]); goto done; }
        chosen[sel[i]] = i + 1;
    }
    for (i = 0; i < uid_cap; i++) if (group_of[i] >= 0 && group_of[i] < ninst && chosen[group_of[i]]) in_set[i] = 1;
    if (!ints_read(mpj_get(dac, "portalDoors"), &doors) || !ints_read(mpj_get(dac, "portalFrames"), &frames) ||
        doors.n != frames.n) { mp_err(err, errcap, "the door table could not be read"); goto done; }
    if (!door_slots(&doors, group_of, uid_cap, ninst, dfirst, dcount, err, errcap)) goto done;
    if (!conn_pairs(targets ? mpj_get(targets, "connections") : NULL, &csrc, &cdst)) {
        mp_err(err, errcap, "the wire list could not be read"); goto done;
    }

    /* --- the fragment, from a copy of the whole map */
    frag = mpj_clone(root);
    if (!frag) { mp_err(err, errcap, "out of memory"); goto done; }
    sum->modules = (sh_mp_module *)calloc((size_t)nsel, sizeof *sum->modules);
    if (!sum->modules) { mp_err(err, errcap, "out of memory"); goto done; }
    vec_read(mpj_get(instances->items[sel[0]], "origin"), anchor);
    {
        mpj *ninsts = mpj_new(MPJ_ARR);
        if (!ninsts) goto oom;
        for (i = 0; i < nsel; i++) {
            mpj *inst = mpj_clone(instances->items[sel[i]]), *org;
            float o[3];
            const mpj *nm;
            if (!inst) { mpj_free(ninsts); goto oom; }
            org = mpj_get(inst, "origin");
            vec_read(org, o);
            o[0] -= anchor[0]; o[1] -= anchor[1]; o[2] -= anchor[2];
            if (!org || !vec_write(org, o)) { mpj_free(inst); mpj_free(ninsts); goto oom; }
            nm = mpj_get(inst, "moduleName");
            if (nm && nm->type == MPJ_STR) mp_copy(sum->modules[i].name, nm->text);
            memcpy(sum->modules[i].origin, o, sizeof o);
            sum->modules[i].rotation = (int)mpj_int(mpj_get(inst, "orientation"));
            summary_add_env(sum, inst);
            if (!mpj_push(ninsts, inst)) { mpj_free(ninsts); goto oom; }
        }
        if (!mpj_set(frag, "instances", ninsts)) goto oom;
    }
    sum->module_count = nsel;

    /* instanceEntities: selected groups in selection order, an empty global group */
    if (!ints_push(&nk, 0)) goto oom;
    for (i = 0; i < nsel; i++) {
        long long b = ie_kv.v[sel[i]], e = ie_kv.v[sel[i] + 1];
        for (; b < e; b++) if (!ints_push(&nv, ie_vals.v[b])) goto oom;
        if (!ints_push(&nk, nv.n)) goto oom;
    }
    if (!ints_push(&nk, nv.n)) goto oom;
    {
        mpj *nie = mpj_get(frag, "instanceEntities");
        if (!ints_store(nie, "keyValues", &nk) || !ints_store(nie, "values", &nv)) goto oom;
    }

    /* entities, in source order */
    {
        mpj *nents = mpj_new(MPJ_ARR);
        if (!nents) goto oom;
        for (i = 0; i < ents->n; i++) {
            long long u = ent_uid(ents->items[i]);
            if (u < 0 || u >= uid_cap || !in_set[u]) continue;
            if (ent_is_nav_box(ents->items[i])) sum->has_nav_boxes = 1;
            if (!mpj_push(nents, mpj_clone(ents->items[i]))) { mpj_free(nents); goto oom; }
            sum->entity_count++;
        }
        if (!mpj_set(frag, "entities", nents)) goto oom;
    }

    /* doors and caps of the chosen instances */
    for (i = 0; i < nsel; i++) {
        int s = sel[i], j;
        for (j = 0; j < dcount[s]; j++) {
            if (!ints_push(&ndoors, doors.v[dfirst[s] + j]) || !ints_push(&nframes, frames.v[dfirst[s] + j])) goto oom;
        }
    }
    sum->door_count = ndoors.n;
    {
        mpj *ndac = mpj_get(frag, "doorsAndCaps");
        if (!ints_store(ndac, "portalDoors", &ndoors) || !ints_store(ndac, "portalFrames", &nframes)) goto oom;
    }

    /* wires inside the set */
    for (i = 0; i < csrc.n; i++) {
        int a = csrc.v[i] >= 0 && csrc.v[i] < uid_cap && in_set[csrc.v[i]];
        int b = cdst.v[i] >= 0 && cdst.v[i] < uid_cap && in_set[cdst.v[i]];
        if (a && b) { if (!ints_push(&ncsrc, csrc.v[i]) || !ints_push(&ncdst, cdst.v[i])) goto oom; }
        else if (a || b) sum->dropped_wires++;
    }
    if (!conn_encode(&nconn, &ncsrc, &ncdst)) goto oom;
    {
        mpj *nt = mpj_get(frag, "targets");
        if (nt) {
            if (!ints_store(nt, "connections", &nconn)) goto oom;
            if (mpj_get(nt, "entitySources") && !mpj_set(nt, "entitySources", empty_multimap())) goto oom;
            if (mpj_get(nt, "entityTargets") && !mpj_set(nt, "entityTargets", empty_multimap())) goto oom;
        }
    }

    /* variables referenced from the set: mark, then number them in source order */
    for (k = 0; k < MP_VAR_TYPES; k++) {
        const mpj *arr = mpj_get(vars, k_var_keys[k]);
        int n = arr && arr->type == MPJ_ARR ? arr->n : 0, j;
        var_len[k] = n;
        var_new[k] = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
        if (!var_new[k]) goto oom;
        for (j = 0; j < n; j++) var_new[k][j] = -1;
    }
    for (i = 0; i < uid_cap; i++) {
        long long b, e;
        if (!in_set[i]) continue;
        csr_row(&vr, i, &b, &e);
        for (; b < e; b++) {
            long long v = vr.values.v[b];
            int t = (int)((v >> 24) & 0xff), x = (int)(v & 0xffffff);
            if (x == MP_VAR_UNBOUND || t >= MP_VAR_TYPES || t == MP_VAR_PERSISTENT) continue;
            if (x < var_len[t]) var_new[t][x] = 0;
        }
    }
    {
        mpj *nvars = mpj_get(frag, "variables");
        mp_ints alloc = {0};
        int have_alloc = ints_read(mpj_get(vars, "allocCount"), &alloc) && alloc.n >= MP_VAR_TYPES;
        for (k = 0; k < MP_VAR_TYPES; k++) {
            const mpj *arr = mpj_get(vars, k_var_keys[k]);
            mpj *narr;
            int j, next = 0;
            if (!arr || arr->type != MPJ_ARR || k == MP_VAR_PERSISTENT) continue;
            narr = mpj_new(MPJ_ARR);
            if (!narr) { free(alloc.v); goto oom; }
            for (j = 0; j < arr->n; j++) {
                if (var_new[k][j] < 0) continue;
                var_new[k][j] = next++;
                if (!mpj_push(narr, mpj_clone(arr->items[j]))) { mpj_free(narr); free(alloc.v); goto oom; }
            }
            sum->variable_count += next;
            if (!mpj_set(nvars, k_var_keys[k], narr)) { free(alloc.v); goto oom; }
            if (have_alloc) alloc.v[k] = next;
        }
        if (have_alloc && !ints_store(nvars, "allocCount", &alloc)) { free(alloc.v); goto oom; }
        free(alloc.v);
    }

    /* references: every set entity keeps its slot counts */
    if (!ints_push(&ner_kv, 0) || !ints_push(&nvr_kv, 0)) goto oom;
    for (i = 0; i < uid_cap; i++) {
        long long b, e;
        if (in_set[i]) {
            csr_row(&er, i, &b, &e);
            for (; b < e; b++) {
                long long v = er.values.v[b];
                if (v != -1 && !(v >= 0 && v < uid_cap && in_set[v])) { v = -1; sum->dropped_refs++; }
                if (!ints_push(&ner_v, v)) goto oom;
            }
            csr_row(&vr, i, &b, &e);
            for (; b < e; b++) {
                long long v = vr.values.v[b];
                int t = (int)((v >> 24) & 0xff), x = (int)(v & 0xffffff);
                if (x != MP_VAR_UNBOUND && t < MP_VAR_TYPES && t != MP_VAR_PERSISTENT) {
                    if (x < var_len[t] && var_new[t][x] >= 0) v = ((long long)t << 24) | var_new[t][x];
                    else { v = ((long long)t << 24) | MP_VAR_UNBOUND; sum->dropped_refs++; }
                }
                if (!ints_push(&nvr_v, v)) goto oom;
            }
        }
        if (!ints_push(&ner_kv, ner_v.n) || !ints_push(&nvr_kv, nvr_v.n)) goto oom;
    }
    {
        mpj *nrefs = mpj_get(frag, "references");
        mpj *ner = mpj_get(nrefs, "entityEntRefs"), *nvr = mpj_get(nrefs, "entityVarRefs");
        if (!ints_store(ner, "keyValues", &ner_kv) || !ints_store(ner, "values", &ner_v) ||
            !ints_store(nvr, "keyValues", &nvr_kv) || !ints_store(nvr, "values", &nvr_v)) goto oom;
    }

    *out = mpj_dump(frag, out_len);
    if (!*out) goto oom;
    ok = 1;
    goto done;
oom:
    mp_err(err, errcap, "out of memory");
done:
    if (!ok && sum) sh_mp_summary_free(sum);
    mpj_free(root); mpj_free(frag);
    free(ie_kv.v); free(ie_vals.v); free(doors.v); free(frames.v); free(csrc.v); free(cdst.v);
    free(nk.v); free(nv.v); free(ndoors.v); free(nframes.v); free(ncsrc.v); free(ncdst.v); free(nconn.v);
    free(ner_kv.v); free(ner_v.v); free(nvr_kv.v); free(nvr_v.v);
    csr_free(&er); csr_free(&vr);
    free(group_of); free(in_set); free(dfirst); free(dcount); free(chosen);
    for (k = 0; k < MP_VAR_TYPES; k++) free(var_new[k]);
    return ok;
}

/* ------------------------------------------------------------ merge */

int sh_mp_merge(const char *map, size_t len, const char *frag_text, size_t flen, const float at[3],
                char **out, size_t *out_len, int *first_instance, int *added, char *err, int errcap)
{
    mpj *root = NULL, *frag = NULL;
    mpj *t_inst, *t_ie, *t_ents, *t_dac, *t_tgt, *t_refs, *t_vars;
    mpj *f_inst, *f_ie, *f_ents, *f_dac, *f_tgt, *f_refs, *f_vars;
    mp_ints tk = {0}, tv = {0}, fk = {0}, fv = {0}, nk = {0}, nv = {0};
    mp_ints td = {0}, tf = {0}, fd = {0}, ff = {0}, tconn = {0}, fsrc = {0}, fdst = {0};
    mp_csr ter = {0}, tvr = {0}, fer = {0}, fvr = {0};
    mp_ints ner_kv = {0}, ner_v = {0}, nvr_kv = {0}, nvr_v = {0};
    mp_idmap idm = {0};
    int var_off[MP_VAR_TYPES] = {0}, var_add[MP_VAR_TYPES] = {0};
    long long t_max = -1, f_max = -1, next, new_max = -1, rows;
    int t_n, f_n, i, k, ok = 0;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!map || !frag_text || !out || !at) { mp_err(err, errcap, "nothing to merge"); return 0; }
    root = mpj_parse(map, len);
    frag = mpj_parse(frag_text, flen);
    if (!mp_is_map(root)) { mp_err(err, errcap, "the open map's JSON could not be read"); goto done; }
    if (!mp_is_map(frag)) { mp_err(err, errcap, "the prefab is not a module prefab"); goto done; }
    t_inst = mpj_get(root, "instances"); f_inst = mpj_get(frag, "instances");
    t_ie = mpj_get(root, "instanceEntities"); f_ie = mpj_get(frag, "instanceEntities");
    t_ents = mpj_get(root, "entities"); f_ents = mpj_get(frag, "entities");
    t_dac = mpj_get(root, "doorsAndCaps"); f_dac = mpj_get(frag, "doorsAndCaps");
    t_tgt = mpj_get(root, "targets"); f_tgt = mpj_get(frag, "targets");
    t_refs = mpj_get(root, "references"); f_refs = mpj_get(frag, "references");
    t_vars = mpj_get(root, "variables"); f_vars = mpj_get(frag, "variables");
    if (!t_inst || t_inst->type != MPJ_ARR || !f_inst || f_inst->type != MPJ_ARR || !t_ents || t_ents->type != MPJ_ARR ||
        !f_ents || f_ents->type != MPJ_ARR || !t_ie || !f_ie || !t_dac || !f_dac || !t_refs || !f_refs || !t_vars) {
        mp_err(err, errcap, "a map is missing a required member"); goto done;
    }
    t_n = t_inst->n; f_n = f_inst->n;
    if (f_n <= 0) { mp_err(err, errcap, "the prefab holds no modules"); goto done; }
    if (!ints_read(mpj_get(t_ie, "keyValues"), &tk) || !ints_read(mpj_get(t_ie, "values"), &tv) || tk.n != t_n + 2 ||
        !ints_read(mpj_get(f_ie, "keyValues"), &fk) || !ints_read(mpj_get(f_ie, "values"), &fv) || fk.n != f_n + 2) {
        mp_err(err, errcap, "instanceEntities does not match the modules"); goto done;
    }
    for (i = 0; i < tk.n; i++) if (tk.v[i] < 0 || tk.v[i] > tv.n || (i && tk.v[i] < tk.v[i - 1])) { mp_err(err, errcap, "open map instanceEntities is invalid"); goto done; }
    for (i = 0; i < fk.n; i++) if (fk.v[i] < 0 || fk.v[i] > fv.n || (i && fk.v[i] < fk.v[i - 1])) { mp_err(err, errcap, "prefab instanceEntities is invalid"); goto done; }
    for (i = 0; i < t_ents->n; i++) { long long u = ent_uid(t_ents->items[i]); if (u > t_max) t_max = u; }
    for (i = 0; i < f_ents->n; i++) { long long u = ent_uid(f_ents->items[i]); if (u < 0) { mp_err(err, errcap, "prefab entity id is invalid"); goto done; } if (u > f_max) f_max = u; }
    if (t_max >= MP_UID_CAP || f_max >= MP_UID_CAP) { mp_err(err, errcap, "entity ids are out of range"); goto done; }
    if (!csr_read(mpj_get(t_refs, "entityEntRefs"), &ter) || !csr_read(mpj_get(t_refs, "entityVarRefs"), &tvr) ||
        !csr_read(mpj_get(f_refs, "entityEntRefs"), &fer) || !csr_read(mpj_get(f_refs, "entityVarRefs"), &fvr)) {
        mp_err(err, errcap, "the reference tables could not be read"); goto done;
    }
    if (!ints_read(mpj_get(t_dac, "portalDoors"), &td) || !ints_read(mpj_get(t_dac, "portalFrames"), &tf) ||
        !ints_read(mpj_get(f_dac, "portalDoors"), &fd) || !ints_read(mpj_get(f_dac, "portalFrames"), &ff) ||
        td.n != tf.n || fd.n != ff.n) { mp_err(err, errcap, "the door tables could not be read"); goto done; }
    if (t_tgt && mpj_get(t_tgt, "connections") && !ints_read(mpj_get(t_tgt, "connections"), &tconn)) {
        mp_err(err, errcap, "the open map's wire list could not be read"); goto done;
    }
    if (!conn_pairs(f_tgt ? mpj_get(f_tgt, "connections") : NULL, &fsrc, &fdst)) { mp_err(err, errcap, "the prefab wire list could not be read"); goto done; }

    /* fresh ids above every id in use and above any stale reference row */
    if (!idmap_init(&idm, f_max + 1)) goto oom;
    next = t_max + 1;
    for (i = 0; i < f_ents->n; i++) {
        long long u = ent_uid(f_ents->items[i]), b1, e1, b2, e2;
        if (idm.to[u] >= 0) { mp_err(err, errcap, "the prefab repeats entity id %lld", u); goto done; }
        for (;;) {
            csr_row(&ter, next, &b1, &e1);
            csr_row(&tvr, next, &b2, &e2);
            if (e1 == b1 && e2 == b2) break;
            next++;
        }
        if (next >= MP_UID_CAP) { mp_err(err, errcap, "no free entity ids"); goto done; }
        idm.to[u] = (int)next;
        if (next > new_max) new_max = next;
        next++;
    }

    /* variables: append the prefab's, remember each type's offset */
    if (f_vars) {
        mp_ints alloc = {0};
        int have_alloc = ints_read(mpj_get(t_vars, "allocCount"), &alloc) && alloc.n >= MP_VAR_TYPES;
        for (k = 0; k < MP_VAR_TYPES; k++) {
            mpj *ta, *fa = mpj_get(f_vars, k_var_keys[k]);
            int j, before;
            if (k == MP_VAR_PERSISTENT || !fa || fa->type != MPJ_ARR || !fa->n) continue;
            ta = mpj_get(t_vars, k_var_keys[k]);
            if (!ta) { if (!mpj_set(t_vars, k_var_keys[k], mpj_new(MPJ_ARR))) { free(alloc.v); goto oom; } ta = mpj_get(t_vars, k_var_keys[k]); }
            if (ta->type != MPJ_ARR) { free(alloc.v); mp_err(err, errcap, "variables.%s is not a list", k_var_keys[k]); goto done; }
            before = ta->n;
            var_off[k] = before;
            for (j = 0; j < fa->n; j++) if (!mpj_push(ta, mpj_clone(fa->items[j]))) { free(alloc.v); goto oom; }
            var_add[k] = fa->n;
            if (have_alloc) {
                /* strings are read by count, so the count grows with the list */
                if (k == 4) alloc.v[k] += var_add[k];
                else if (alloc.v[k] < ta->n) alloc.v[k] = ta->n;
            }
        }
        if (have_alloc && !ints_store(t_vars, "allocCount", &alloc)) { free(alloc.v); goto oom; }
        free(alloc.v);
    }

    /* instances at the anchor */
    for (i = 0; i < f_n; i++) {
        mpj *inst = mpj_clone(f_inst->items[i]), *org;
        float o[3];
        if (!inst) goto oom;
        org = mpj_get(inst, "origin");
        if (!org) { mpj_free(inst); mp_err(err, errcap, "a prefab module has no origin"); goto done; }
        vec_read(org, o);
        o[0] += at[0]; o[1] += at[1]; o[2] += at[2];
        if (!vec_write(org, o) || !mpj_push(t_inst, inst)) goto oom;
    }

    /* instanceEntities: open map groups, prefab groups, then the global group */
    if (!ints_push(&nk, 0)) goto oom;
    for (k = 0; k < t_n; k++) {
        long long b;
        for (b = tk.v[k]; b < tk.v[k + 1]; b++) if (!ints_push(&nv, tv.v[b])) goto oom;
        if (!ints_push(&nk, nv.n)) goto oom;
    }
    for (k = 0; k < f_n; k++) {
        long long b;
        for (b = fk.v[k]; b < fk.v[k + 1]; b++) {
            int m = idmap_get(&idm, fv.v[b]);
            if (m < 0) { mp_err(err, errcap, "prefab group names a missing entity %lld", fv.v[b]); goto done; }
            if (!ints_push(&nv, m)) goto oom;
        }
        if (!ints_push(&nk, nv.n)) goto oom;
    }
    {
        long long b;
        for (b = tk.v[t_n]; b < tk.v[t_n + 1]; b++) if (!ints_push(&nv, tv.v[b])) goto oom;
        if (!ints_push(&nk, nv.n)) goto oom;
    }
    if (!ints_store(t_ie, "keyValues", &nk) || !ints_store(t_ie, "values", &nv)) goto oom;

    /* entities */
    for (i = 0; i < f_ents->n; i++) {
        mpj *e = mpj_clone(f_ents->items[i]);
        if (!e || !mpj_set(e, "uniqueId", mpj_new_int(idm.to[ent_uid(f_ents->items[i])])) || !mpj_push(t_ents, e)) goto oom;
    }

    /* doors and caps */
    for (i = 0; i < fd.n; i++) {
        int d = idmap_get(&idm, fd.v[i]);
        int f = ff.v[i] == -1 ? -1 : idmap_get(&idm, ff.v[i]);
        if (d < 0) { mp_err(err, errcap, "prefab portal %d has no door entity", i); goto done; }
        if (!ints_push(&td, d) || !ints_push(&tf, f)) goto oom;
    }
    if (!ints_store(t_dac, "portalDoors", &td) || !ints_store(t_dac, "portalFrames", &tf)) goto oom;

    /* wires */
    {
        mp_ints ms = {0}, md = {0};
        for (i = 0; i < fsrc.n; i++) {
            int a = idmap_get(&idm, fsrc.v[i]), b = idmap_get(&idm, fdst.v[i]);
            if (a < 0 || b < 0) continue;
            if (!ints_push(&ms, a) || !ints_push(&md, b)) { free(ms.v); free(md.v); goto oom; }
        }
        if (!conn_encode(&tconn, &ms, &md)) { free(ms.v); free(md.v); goto oom; }
        free(ms.v); free(md.v);
    }
    if (t_tgt && !ints_store(t_tgt, "connections", &tconn)) goto oom;

    /* references: open map rows unchanged, then the new ids' rows */
    rows = ter.kv.n - 1;
    if (tvr.kv.n - 1 > rows) rows = tvr.kv.n - 1;
    if (new_max + 1 > rows) rows = new_max + 1;
    {
        /* inverse id map for the new rows */
        long long u;
        int *from = (int *)malloc((size_t)(rows > 0 ? rows : 1) * sizeof(int));
        if (!from) goto oom;
        for (u = 0; u < rows; u++) from[u] = -1;
        for (u = 0; u <= f_max; u++) if (idm.to[u] >= 0) from[idm.to[u]] = (int)u;
        if (!ints_push(&ner_kv, 0) || !ints_push(&nvr_kv, 0)) { free(from); goto oom; }
        for (u = 0; u < rows; u++) {
            long long b, e;
            if (from[u] >= 0) {
                csr_row(&fer, from[u], &b, &e);
                for (; b < e; b++) {
                    long long v = fer.values.v[b];
                    if (v != -1) { int m = idmap_get(&idm, v); v = m < 0 ? -1 : m; }
                    if (!ints_push(&ner_v, v)) { free(from); goto oom; }
                }
                csr_row(&fvr, from[u], &b, &e);
                for (; b < e; b++) {
                    long long v = fvr.values.v[b];
                    int t = (int)((v >> 24) & 0xff), x = (int)(v & 0xffffff);
                    if (x != MP_VAR_UNBOUND && t < MP_VAR_TYPES && t != MP_VAR_PERSISTENT) {
                        if (x < var_add[t]) v = ((long long)t << 24) | (x + var_off[t]);
                        else v = ((long long)t << 24) | MP_VAR_UNBOUND;
                    }
                    if (!ints_push(&nvr_v, v)) { free(from); goto oom; }
                }
            } else {
                csr_row(&ter, u, &b, &e);
                for (; b < e; b++) if (!ints_push(&ner_v, ter.values.v[b])) { free(from); goto oom; }
                csr_row(&tvr, u, &b, &e);
                for (; b < e; b++) if (!ints_push(&nvr_v, tvr.values.v[b])) { free(from); goto oom; }
            }
            if (!ints_push(&ner_kv, ner_v.n) || !ints_push(&nvr_kv, nvr_v.n)) { free(from); goto oom; }
        }
        free(from);
    }
    {
        mpj *ner = mpj_get(t_refs, "entityEntRefs"), *nvr = mpj_get(t_refs, "entityVarRefs");
        if (!ints_store(ner, "keyValues", &ner_kv) || !ints_store(ner, "values", &ner_v) ||
            !ints_store(nvr, "keyValues", &nvr_kv) || !ints_store(nvr, "values", &nvr_v)) goto oom;
    }

    *out = mpj_dump(root, out_len);
    if (!*out) goto oom;
    if (first_instance) *first_instance = t_n;
    if (added) *added = f_n;
    ok = 1;
    goto done;
oom:
    mp_err(err, errcap, "out of memory");
done:
    mpj_free(root); mpj_free(frag);
    free(tk.v); free(tv.v); free(fk.v); free(fv.v); free(nk.v); free(nv.v);
    free(td.v); free(tf.v); free(fd.v); free(ff.v); free(tconn.v); free(fsrc.v); free(fdst.v);
    csr_free(&ter); csr_free(&tvr); csr_free(&fer); csr_free(&fvr);
    free(ner_kv.v); free(ner_v.v); free(nvr_kv.v); free(nvr_v.v);
    free(idm.to);
    return ok;
}

/* ------------------------------------------------------------ inspect */

int sh_mp_inspect(const char *text, size_t flen, sh_mp_summary *sum, char *err, int errcap)
{
    mpj *root;
    const mpj *inst, *ents, *dac, *vars;
    int i, k;
    if (!sum) return 0;
    memset(sum, 0, sizeof *sum);
    root = mpj_parse(text, flen);
    if (!mp_is_map(root)) { mpj_free(root); mp_err(err, errcap, "not a module prefab"); return 0; }
    inst = mpj_get(root, "instances"); ents = mpj_get(root, "entities");
    dac = mpj_get(root, "doorsAndCaps"); vars = mpj_get(root, "variables");
    if (!inst || inst->type != MPJ_ARR || !ents || ents->type != MPJ_ARR) { mpj_free(root); mp_err(err, errcap, "the prefab is incomplete"); return 0; }
    sum->modules = (sh_mp_module *)calloc((size_t)(inst->n > 0 ? inst->n : 1), sizeof *sum->modules);
    if (!sum->modules) { mpj_free(root); mp_err(err, errcap, "out of memory"); return 0; }
    sum->module_count = inst->n;
    for (i = 0; i < inst->n; i++) {
        const mpj *nm = mpj_get(inst->items[i], "moduleName");
        if (nm && nm->type == MPJ_STR) mp_copy(sum->modules[i].name, nm->text);
        vec_read(mpj_get(inst->items[i], "origin"), sum->modules[i].origin);
        sum->modules[i].rotation = (int)mpj_int(mpj_get(inst->items[i], "orientation"));
        summary_add_env(sum, inst->items[i]);
    }
    sum->entity_count = ents->n;
    for (i = 0; i < ents->n; i++) if (ent_is_nav_box(ents->items[i])) { sum->has_nav_boxes = 1; break; }
    if (dac) { const mpj *d = mpj_get(dac, "portalDoors"); sum->door_count = d && d->type == MPJ_ARR ? d->n : 0; }
    if (vars) for (k = 0; k < MP_VAR_TYPES; k++) {
        const mpj *a = mpj_get(vars, k_var_keys[k]);
        if (k != MP_VAR_PERSISTENT && a && a->type == MPJ_ARR) sum->variable_count += a->n;
    }
    mpj_free(root);
    return 1;
}

static int mp_name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

int sh_mp_check_modules(const sh_mp_summary *s, sh_mp_module_known_fn known, void *ctx,
                        sh_mp_module_check *out)
{
    int i, j;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!s || !known || (s->module_count > 0 && !s->modules)) return -1;
    for (i = 0; i < s->module_count; i++) {
        const char *name = s->modules[i].name;
        int seen = 0, k;
        for (j = 0; j < i && !seen; j++) seen = mp_name_eq(s->modules[j].name, name);
        if (seen) continue;
        k = name[0] ? known(ctx, name) : 0;
        if (k < 0) { out->unknown++; continue; }
        out->checked++;
        if (k > 0) continue;
        if (out->listed < SH_MP_CHECK_NAMES) mp_copy(out->missing[out->listed++], name);
        out->missing_count++;
    }
    return out->checked ? out->missing_count : -1;
}

void sh_mp_module_short_name(const char *name, char *out, size_t cap)
{
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!name) return;
    if (!name[0]) { snprintf(out, cap, "(no module name)"); return; }
    if (!_strnicmp(name, "maps/modules/", 13)) name += 13;
    n = strlen(name);
    if (n > 5 && !_stricmp(name + n - 5, ".decl")) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
}

/* names[0..listed) joined by ", ", short or in full, then ", ..." when more were missing. */
static void mp_join_missing(const sh_mp_module_check *c, int shorten, char *out, size_t cap)
{
    size_t used = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < c->listed && used + 1 < cap; i++) {
        char shortname[SH_MP_NAME_CAP];
        const char *name = c->missing[i];
        if (shorten || !name[0]) { sh_mp_module_short_name(name, shortname, sizeof shortname); name = shortname; }
        used += (size_t)snprintf(out + used, cap - used, "%s%s", i ? ", " : "", name);
        if (used >= cap) used = cap - 1;
    }
    if (c->missing_count > c->listed && used + 1 < cap) snprintf(out + used, cap - used, ", ...");
}

int sh_mp_missing_console_line(const sh_mp_module_check *c, char *out, size_t cap)
{
    char names[1024];
    int n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!c || c->missing_count <= 0) return 0;
    mp_join_missing(c, 0, names, sizeof names);
    if (c->missing_count == 1)
        n = snprintf(out, cap, "Module prefab: needs module %s; this room is not in your palette "
                               "(DLC or a mod you do not have)", names);
    else
        n = snprintf(out, cap, "Module prefab: needs modules %s; these rooms are not in your palette "
                               "(DLC or a mod you do not have)", names);
    return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}

int sh_mp_missing_toast(const sh_mp_module_check *c, char *out, size_t cap)
{
    char names[512];
    int n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!c || c->missing_count <= 0) return 0;
    mp_join_missing(c, 1, names, sizeof names);
    if (c->missing_count == 1)
        n = snprintf(out, cap, "Not placed: this prefab needs module %s, which is not in your palette "
                               "(DLC or a mod you do not have)", names);
    else
        n = snprintf(out, cap, "Not placed: this prefab needs %d modules that are not in your palette "
                               "(DLC or a mod you do not have): %s", c->missing_count, names);
    return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}

int sh_mp_missing_status(const sh_mp_module_check *c, char *out, size_t cap)
{
    char names[512];
    int n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!c || c->missing_count <= 0) return 0;
    mp_join_missing(c, 1, names, sizeof names);
    n = snprintf(out, cap, "Cannot be placed here: needs %s %s (not in your palette: DLC or a mod you do not have)",
                 c->missing_count == 1 ? "module" : "modules", names);
    return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}

int sh_mp_is_module_file(const char *text, size_t len)
{
    /* Walk the top-level object only, skipping nested values, for "~type". */
    size_t i = 0, key_b = 0, key_e = 0;
    int depth = 0, want = 0;   /* want: 0 key, 1 colon, 2 value, 3 rest of the value */
    if (!text) return 0;
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    if (i >= len || text[i] != '{') return 0;
    for (; i < len; i++) {
        char c = text[i];
        if (c == '"') {
            size_t b = i + 1, e = b;
            while (e < len && text[e] != '"') { if (text[e] == '\\') e++; e++; }
            if (e >= len) return 0;
            if (depth == 1 && want == 0) { key_b = b; key_e = e; want = 1; }
            else if (depth == 1 && want == 2) {
                if (key_e - key_b == 5 && !memcmp(text + key_b, "~type", 5))
                    return (e - b == 9 && !memcmp(text + b, "idSnapMap", 9)) ? 1 : 0;
                want = 3;
            }
            i = e;
            continue;
        }
        if (c == ':' && depth == 1 && want == 1) { want = 2; continue; }
        if (c == '{' || c == '[') { if (depth == 1 && want == 2) want = 3; depth++; continue; }
        if (c == '}' || c == ']') { depth--; if (depth <= 0) return 0; continue; }
        if (c == ',' && depth == 1) { want = 0; continue; }
    }
    return 0;
}

/* ------------------------------------------------------------ sidecar */

typedef struct mp_buf { char *p; size_t n, cap; int bad; } mp_buf;

static void buf_put(mp_buf *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    if (b->bad) return;
    for (;;) {
        size_t room = b->cap - b->n;
        va_start(ap, fmt);
        need = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? room : 0, fmt, ap);
        va_end(ap);
        if (need < 0) { b->bad = 1; return; }
        if (b->p && (size_t)need < room) { b->n += (size_t)need; return; }
        {
            size_t cap = b->cap ? b->cap * 2 : 1024;
            char *p;
            while (cap < b->n + (size_t)need + 1) cap *= 2;
            p = (char *)realloc(b->p, cap);
            if (!p) { b->bad = 1; return; }
            b->p = p; b->cap = cap;
        }
    }
}

static void buf_num(mp_buf *b, double v)
{
    if (!isfinite(v)) v = 0;
    if (v == floor(v) && fabs(v) < 1e15) buf_put(b, "%.0f", v);
    else buf_put(b, "%.3f", v);
}

static void buf_vec(mp_buf *b, const float *v)
{
    buf_put(b, "["); buf_num(b, v[0]); buf_put(b, ","); buf_num(b, v[1]); buf_put(b, ","); buf_num(b, v[2]); buf_put(b, "]");
}

/* Names are decl paths and environment names: escape quotes, backslashes and controls. */
static void buf_str(mp_buf *b, const char *s)
{
    buf_put(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') buf_put(b, "\\%c", c);
        else if (c < 0x20) buf_put(b, "\\u%04x", c);
        else buf_put(b, "%c", c);
    }
    buf_put(b, "\"");
}

char *sh_mp_sidecar_json(const sh_mp_summary *s, const float (*bounds)[6], const char *const *labels)
{
    mp_buf b = {0};
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int i, k;
    if (!s) return NULL;
    for (i = 0; i < s->module_count; i++) {
        float mn[3], mx[3];
        for (k = 0; k < 3; k++) {
            mn[k] = bounds ? bounds[i][k] : s->modules[i].origin[k];
            mx[k] = bounds ? bounds[i][k + 3] : s->modules[i].origin[k];
            if (!i || mn[k] < lo[k]) lo[k] = mn[k];
            if (!i || mx[k] > hi[k]) hi[k] = mx[k];
        }
    }
    buf_put(&b, "{\"desc\":\"\",\"tags\":[],\"kind\":\"modules\",\"moduleCount\":%d,\"entityCount\":%d,"
                "\"doorCount\":%d,\"variableCount\":%d,\"droppedWires\":%d,\"droppedRefs\":%d,"
                "\"hasNavigationBoxes\":%s,\"environments\":[",
            s->module_count, s->entity_count, s->door_count, s->variable_count,
            s->dropped_wires, s->dropped_refs, s->has_nav_boxes ? "true" : "false");
    for (i = 0; i < s->env_count; i++) { if (i) buf_put(&b, ","); buf_str(&b, s->envs[i]); }
    buf_put(&b, "],\"bounds\":{\"min\":"); buf_vec(&b, lo); buf_put(&b, ",\"max\":"); buf_vec(&b, hi);
    buf_put(&b, "},\"modules\":[");
    for (i = 0; i < s->module_count; i++) {
        float size[3], mn[3], mx[3];
        for (k = 0; k < 3; k++) {
            mn[k] = bounds ? bounds[i][k] : s->modules[i].origin[k];
            mx[k] = bounds ? bounds[i][k + 3] : s->modules[i].origin[k];
            size[k] = mx[k] - mn[k];
        }
        if (i) buf_put(&b, ",");
        buf_put(&b, "{\"name\":"); buf_str(&b, s->modules[i].name);
        buf_put(&b, ",\"label\":"); buf_str(&b, labels && labels[i] ? labels[i] : "");
        buf_put(&b, ",\"origin\":"); buf_vec(&b, s->modules[i].origin);
        buf_put(&b, ",\"rotation\":%d,\"size\":", s->modules[i].rotation); buf_vec(&b, size);
        buf_put(&b, ",\"bounds\":{\"min\":"); buf_vec(&b, mn); buf_put(&b, ",\"max\":"); buf_vec(&b, mx);
        buf_put(&b, "}}");
    }
    buf_put(&b, "]}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

char *sh_mp_check_json(const sh_mp_module_check *c)
{
    mp_buf b = {0};
    int i;
    if (!c || !c->checked) {
        buf_put(&b, "{\"checked\":false}");
    } else {
        char status[768];
        sh_mp_missing_status(c, status, sizeof status);
        buf_put(&b, "{\"checked\":true,\"count\":%d,\"missing\":[", c->missing_count);
        for (i = 0; i < c->listed; i++) { if (i) buf_put(&b, ","); buf_str(&b, c->missing[i]); }
        buf_put(&b, "],\"status\":");
        buf_str(&b, status);
        buf_put(&b, "}");
    }
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}
