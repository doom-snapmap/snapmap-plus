/* companion.c -- the companions table: parse, validate, look up, compose.
 * Pure C; see companion.h for the schema. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "companion.h"
#include "mp_json.h"

static char lower_sep(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    if (c == '\\') return '/';
    return c;
}

static int name_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (lower_sep(*a) != lower_sep(*b)) return 0;
        if (!*a) return 1;
    }
}

int sh_companion_owner_namespace(const char *name)
{
    static const char prefix[] = "mods/";
    size_t i;
    if (!name) return 0;
    for (i = 0; i < sizeof prefix - 1; i++)
        if (lower_sep(name[i]) != prefix[i]) return 0;
    return 1;
}

/* A decl name the engine can look up: printable ASCII without quotes, braces,
 * escapes or spaces, shorter than the cap. mp_json keeps escapes verbatim, so
 * a backslash here means the author escaped something and is refused. */
static int plain_name(const char *s)
{
    size_t n = 0;
    if (!s || !*s) return 0;
    for (; *s; s++, n++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '{' || c == '}' || c == ';') return 0;
    }
    return n < SH_COMPANION_NAME_CAP;
}

/* Owner names live under mods/<id>/; ids may be spelled with '_' for '-'. */
static int in_namespace(const char *owner, const char *bundle_id)
{
    size_t i, n = strlen(bundle_id);
    if (!sh_companion_owner_namespace(owner)) return 0;
    owner += 5;
    for (i = 0; i < n; i++) {
        char a = lower_sep(owner[i]), b = lower_sep(bundle_id[i]);
        if (a == '_') a = '-';
        if (b == '_') b = '-';
        if (!owner[i] || a != b) return 0;
    }
    return owner[n] == '/' && owner[n + 1] != 0;
}

static void say(sh_companion_log_fn log, void *context, const char *fmt, const char *a, const char *b)
{
    char line[512];
    if (!log) return;
    _snprintf_s(line, sizeof line, _TRUNCATE, fmt, a ? a : "", b ? b : "");
    log(context, line);
}

static int finite_bounded(double v, double bound)
{
    return v == v && v >= -bound && v <= bound;
}

static int read_vec3(const mpj *v, float out[3], double bound)
{
    int i;
    if (!v || v->type != MPJ_ARR || v->n != 3) return 0;
    for (i = 0; i < 3; i++) {
        double d;
        if (!mpj_is_num(v->items[i])) return 0;
        d = mpj_num(v->items[i]);
        if (!finite_bounded(d, bound)) return 0;
        out[i] = (float)d;
    }
    return 1;
}

/* Rows of unit length, mutually orthogonal and right-handed, within tolerance. */
static int axis_valid(const float m[9])
{
    int i, j;
    double det;
    for (i = 0; i < 3; i++)
        for (j = i; j < 3; j++) {
            double dot = (double)m[i * 3] * m[j * 3] + (double)m[i * 3 + 1] * m[j * 3 + 1] +
                         (double)m[i * 3 + 2] * m[j * 3 + 2];
            if (fabs(dot - (i == j ? 1.0 : 0.0)) > SH_COMPANION_AXIS_TOL) return 0;
        }
    det = (double)m[0] * (m[4] * m[8] - m[5] * m[7]) - (double)m[1] * (m[3] * m[8] - m[5] * m[6]) +
          (double)m[2] * (m[3] * m[7] - m[4] * m[6]);
    return det > 0.0;
}

static int read_bool(const mpj *v, int fallback, int *ok)
{
    if (!v) return fallback;
    if (v->type != MPJ_BOOL) { *ok = 0; return fallback; }
    return v->b ? 1 : 0;
}

static int parse_entry(const mpj *e, sh_companion_entry *out)
{
    const mpj *def, *axis;
    int i, ok = 1;
    static const char *known[] = { "entityDef", "origin", "axis", "activate", "bind" };
    if (!e || e->type != MPJ_OBJ) return 0;
    for (i = 0; i < e->n; i++) {
        size_t k, found = 0;
        for (k = 0; k < sizeof known / sizeof known[0]; k++)
            if (!strcmp(e->keys[i], known[k])) found = 1;
        if (!found) return 0;      /* an unknown key is a typo, not an extension */
    }
    memset(out, 0, sizeof *out);
    def = mpj_get(e, "entityDef");
    if (!def || def->type != MPJ_STR || !plain_name(def->text)) return 0;
    strncpy_s(out->entitydef, sizeof out->entitydef, def->text, _TRUNCATE);
    if (mpj_get(e, "origin") && !read_vec3(mpj_get(e, "origin"), out->origin, SH_COMPANION_ORIGIN_MAX)) return 0;
    out->axis[0] = out->axis[4] = out->axis[8] = 1.0f;
    axis = mpj_get(e, "axis");
    if (axis) {
        if (axis->type != MPJ_ARR || axis->n != 3) return 0;
        for (i = 0; i < 3; i++)
            if (!read_vec3(axis->items[i], out->axis + i * 3, 2.0)) return 0;
        if (!axis_valid(out->axis)) return 0;
    }
    out->activate = read_bool(mpj_get(e, "activate"), 1, &ok);
    out->bind = read_bool(mpj_get(e, "bind"), 1, &ok);
    return ok;
}

static int grow(sh_companion_table *t, int need)
{
    sh_companion_owner *p;
    int cap;
    if (need <= t->cap) return 1;
    cap = t->cap ? t->cap * 2 : 8;
    while (cap < need) cap *= 2;
    p = (sh_companion_owner *)realloc(t->owners, (size_t)cap * sizeof *p);
    if (!p) return 0;
    t->owners = p;
    t->cap = cap;
    return 1;
}

int sh_companion_add_bundle(sh_companion_table *t, const char *bundle_id, const char *extra_json,
                            size_t length, sh_companion_log_fn log, void *context)
{
    mpj *root, *block;
    int i, j, start, added = 0;
    if (!t || !bundle_id || !extra_json || !length) return 0;
    root = mpj_parse(extra_json, length);
    if (!root || root->type != MPJ_OBJ) {
        mpj_free(root);
        return 0;                  /* the builder already accepted this text; nothing to read */
    }
    block = mpj_get(root, "companions");
    if (!block) {
        mpj_free(root);
        return 0;
    }
    if (block->type != MPJ_OBJ) {
        say(log, context, "COMPANIONS: %s REFUSED: \"companions\" must be an object of owners%s", bundle_id, NULL);
        goto refused;
    }
    start = t->count;
    for (i = 0; i < block->n; i++) {
        const char *owner = block->keys[i];
        const mpj *list = block->items[i];
        sh_companion_owner *o;
        if (!plain_name(owner) || !in_namespace(owner, bundle_id)) {
            say(log, context, "COMPANIONS: %s REFUSED: owner '%s' is not one of the bundle's own names", bundle_id, owner);
            goto rollback;
        }
        if (!list || list->type != MPJ_ARR || list->n < 1 || list->n > SH_COMPANION_MAX_PER_OWNER) {
            say(log, context, "COMPANIONS: %s REFUSED: owner '%s' needs a list of 1 to 16 companions", bundle_id, owner);
            goto rollback;
        }
        for (j = start; j < t->count; j++)
            if (name_equal(t->owners[j].owner, owner)) {
                say(log, context, "COMPANIONS: %s REFUSED: owner '%s' is listed twice", bundle_id, owner);
                goto rollback;
            }
        if (sh_companion_find(t, owner)) {
            say(log, context, "COMPANIONS: %s: owner '%s' is already declared by an earlier bundle; kept that one",
                bundle_id, owner);
            t->refused++;
            continue;
        }
        if (t->count >= SH_COMPANION_MAX_OWNERS) {
            say(log, context, "COMPANIONS: %s REFUSED: more than 256 owners in all%s", bundle_id, NULL);
            goto rollback;
        }
        if (!grow(t, t->count + 1)) {
            say(log, context, "COMPANIONS: %s REFUSED: out of memory%s", bundle_id, NULL);
            goto rollback;
        }
        o = &t->owners[t->count];
        memset(o, 0, sizeof *o);
        strncpy_s(o->owner, sizeof o->owner, owner, _TRUNCATE);
        strncpy_s(o->bundle, sizeof o->bundle, bundle_id, _TRUNCATE);
        for (j = 0; j < list->n; j++) {
            if (!parse_entry(list->items[j], &o->entries[j])) {
                say(log, context, "COMPANIONS: %s REFUSED: owner '%s' has a malformed companion", bundle_id, owner);
                goto rollback;
            }
            if (name_equal(o->entries[j].entitydef, owner)) {
                say(log, context, "COMPANIONS: %s REFUSED: owner '%s' lists itself as a companion", bundle_id, owner);
                goto rollback;
            }
        }
        o->count = list->n;
        t->count++;
        added++;
    }
    mpj_free(root);
    return added;
rollback:
    t->count = start;
refused:
    t->refused++;
    mpj_free(root);
    return -1;
}

const sh_companion_owner *sh_companion_find(const sh_companion_table *t, const char *name)
{
    int i;
    if (!t || !name) return NULL;
    for (i = 0; i < t->count; i++)
        if (name_equal(t->owners[i].owner, name)) return &t->owners[i];
    return NULL;
}

void sh_companion_compose(const float owner_origin[3], const float owner_axis[9],
                          const sh_companion_entry *e, float out_origin[3], float out_axis[9])
{
    int i, j;
    for (j = 0; j < 3; j++)
        out_origin[j] = owner_origin[j] + e->origin[0] * owner_axis[j] + e->origin[1] * owner_axis[3 + j] +
                        e->origin[2] * owner_axis[6 + j];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out_axis[i * 3 + j] = e->axis[i * 3] * owner_axis[j] + e->axis[i * 3 + 1] * owner_axis[3 + j] +
                                  e->axis[i * 3 + 2] * owner_axis[6 + j];
}

void sh_companion_table_free(sh_companion_table *t)
{
    if (!t) return;
    free(t->owners);
    t->owners = NULL;
    t->count = t->cap = t->refused = 0;
}
