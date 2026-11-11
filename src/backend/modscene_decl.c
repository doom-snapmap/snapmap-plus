/* modscene_decl.c -- the scenes table: parse, validate, look up, frame math.
 * Pure C; see modscene_decl.h for the schema. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "modscene_decl.h"
#include "../common/text.h"
#include "mp_json.h"

#define SC_PI 3.14159265358979323846

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

int sh_scene_owner_namespace(const char *name)
{
    static const char prefix[] = "mods/";
    size_t i;
    if (!name) return 0;
    for (i = 0; i < sizeof prefix - 1; i++)
        if (lower_sep(name[i]) != prefix[i]) return 0;
    return 1;
}

/* A decl name the engine can look up: printable ASCII without quotes, braces,
 * escapes or spaces, of any length. */
static int plain_name(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '{' || c == '}' || c == ';') return 0;
    }
    return 1;
}

/* Owner names live under mods/<salt>/. */
static int in_namespace(const char *owner, const char *salt)
{
    size_t i, n = strlen(salt);
    if (!sh_scene_owner_namespace(owner)) return 0;
    owner += 5;
    for (i = 0; i < n; i++)
        if (!owner[i] || lower_sep(owner[i]) != lower_sep(salt[i])) return 0;
    return owner[n] == '/' && owner[n + 1] != 0;
}

static void say(sh_scene_log_fn log, void *context, const char *fmt, const char *a, const char *b)
{
    char *line;
    if (!log) return;
    line = sh_textf(fmt, a ? a : "", b ? b : "");
    log(context, line ? line : "its scenes could not be read (out of memory)");
    free(line);
}

static int finite_bounded(double v, double bound)
{
    return v == v && v >= -bound && v <= bound;
}

/* [x, y, z, yaw] */
static int read_pose(const mpj *v, sh_scene_pose *out)
{
    double d[4];
    int i;
    if (!v || v->type != MPJ_ARR || v->n != 4) return 0;
    for (i = 0; i < 4; i++) {
        if (!mpj_is_num(v->items[i])) return 0;
        d[i] = mpj_num(v->items[i]);
        if (!finite_bounded(d[i], i < 3 ? SH_SCENE_COORD_MAX : SH_SCENE_YAW_MAX)) return 0;
    }
    out->x = (float)d[0];
    out->y = (float)d[1];
    out->z = (float)d[2];
    out->yaw = (float)d[3];
    return 1;
}

static int only_keys(const mpj *o, const char *const *known, size_t count)
{
    int i;
    for (i = 0; i < o->n; i++) {
        size_t k, found = 0;
        for (k = 0; k < count; k++)
            if (!strcmp(o->keys[i], known[k])) found = 1;
        if (!found) return 0;      /* an unknown key is a typo, not an extension */
    }
    return 1;
}

static int read_spawn(const mpj *e, const sh_scene_pose *anchor, sh_scene_spawn *out)
{
    static const char *const known[] = { "entityDef", "at" };
    const mpj *def;
    if (!e || e->type != MPJ_OBJ || !only_keys(e, known, 2)) return 0;
    memset(out, 0, sizeof *out);
    def = mpj_get(e, "entityDef");
    if (!def || def->type != MPJ_STR || !plain_name(def->text)) return 0;
    if (!(out->entitydef = _strdup(def->text))) return 0;
    out->at = *anchor;
    return !mpj_get(e, "at") || read_pose(mpj_get(e, "at"), &out->at);
}

static void spawns_free(sh_scene_spawn *spawns, int count)
{
    int i;
    for (i = 0; spawns && i < count; i++) free(spawns[i].entitydef);
    free(spawns);
}

static void entry_free(sh_scene_entry *e)
{
    free(e->node);
    free(e->bundle);
    spawns_free(e->spawns, e->spawn_count);
    memset(e, 0, sizeof *e);
}

/* One owner's entry. Returns NULL when it is well formed, else the reason. */
static const char *parse_entry(const mpj *e, sh_scene_entry *out)
{
    static const char *const known[] = { "plays", "anchor", "player", "spawn", "reaim", "return" };
    const mpj *v;
    int i;
    if (!e || e->type != MPJ_OBJ) return "an entry must be an object";
    if (!only_keys(e, known, sizeof known / sizeof known[0])) return "an entry has an unknown key";
    v = mpj_get(e, "plays");
    if (v) {
        if (mpj_str_is(v, "at_node")) out->at_node = 1;
        else if (!mpj_str_is(v, "at_player")) return "\"plays\" must be \"at_player\" or \"at_node\"";
    }
    v = mpj_get(e, "anchor");
    if (v && !read_pose(v, &out->anchor)) return "\"anchor\" must be [x, y, z, yaw]";
    v = mpj_get(e, "player");
    if (v) {
        if (!read_pose(v, &out->player)) return "\"player\" must be [x, y, z, yaw]";
        if (out->at_node) return "\"player\" applies only to a scene that plays at the player";
        out->has_player = 1;
    }
    v = mpj_get(e, "spawn");
    if (v) {
        if (v->type != MPJ_ARR || v->n < 1) return "\"spawn\" needs a list of entities";
        if (!(out->spawns = (sh_scene_spawn *)calloc((size_t)v->n, sizeof *out->spawns)))
            return "its spawns could not be read (out of memory)";
        out->spawn_count = v->n;   /* every row is freed with the entry, read or not */
        for (i = 0; i < v->n; i++)
            if (!read_spawn(v->items[i], &out->anchor, &out->spawns[i])) return "a \"spawn\" entity is malformed";
    }
    out->reaim = out->has_player;
    v = mpj_get(e, "reaim");
    if (v) {
        if (v->type != MPJ_BOOL) return "\"reaim\" must be true or false";
        out->reaim = v->b ? 1 : 0;
        if (out->reaim && !out->has_player) return "\"reaim\" needs \"player\" and a scene that plays at the player";
    }
    out->return_player = !out->at_node && !out->spawn_count;
    v = mpj_get(e, "return");
    if (v) {
        if (v->type != MPJ_BOOL) return "\"return\" must be true or false";
        out->return_player = v->b ? 1 : 0;
    }
    return NULL;
}

static int grow(sh_scene_table *t, int need)
{
    sh_scene_entry *p;
    int cap;
    if (need <= t->cap) return 1;
    cap = t->cap ? t->cap * 2 : 8;
    while (cap < need) cap *= 2;
    p = (sh_scene_entry *)realloc(t->rows, (size_t)cap * sizeof *p);
    if (!p) return 0;
    t->rows = p;
    t->cap = cap;
    return 1;
}

int sh_scene_add_bundle(sh_scene_table *t, const char *salt, const char *extra_json, size_t length,
                        sh_scene_log_fn log, void *context)
{
    mpj *root, *block;
    int i, j, start, added = 0;
    if (!t || !salt || !extra_json || !length) return 0;
    root = mpj_parse(extra_json, length);
    if (!root || root->type != MPJ_OBJ) {
        mpj_free(root);
        return 0;                  /* the builder already accepted this text; nothing to read */
    }
    block = mpj_get(root, "scenes");
    if (!block) {
        mpj_free(root);
        return 0;
    }
    if (block->type != MPJ_OBJ) {
        say(log, context, "its \"scenes\" entry must be an object of scene nodes%s%s", NULL, NULL);
        goto refused;
    }
    start = t->count;
    for (i = 0; i < block->n; i++) {
        const char *owner = block->keys[i];
        const char *why;
        sh_scene_entry *e;
        if (!plain_name(owner) || !in_namespace(owner, salt)) {
            say(log, context, "its scenes entry '%s' names a node outside this mod (it must start with mods/%s/)", owner, salt);
            goto rollback;
        }
        for (j = start; j < t->count; j++)
            if (name_equal(t->rows[j].node, owner)) {
                say(log, context, "its scenes entry '%s' is listed twice%s", owner, NULL);
                goto rollback;
            }
        if (!grow(t, t->count + 1)) {
            say(log, context, "its scenes could not be read (out of memory)%s%s", NULL, NULL);
            goto rollback;
        }
        e = &t->rows[t->count];
        memset(e, 0, sizeof *e);
        if (!(e->node = _strdup(owner)) || !(e->bundle = _strdup(salt))) {
            entry_free(e);
            say(log, context, "its scenes could not be read (out of memory)%s%s", NULL, NULL);
            goto rollback;
        }
        why = parse_entry(block->items[i], e);
        if (why) {
            entry_free(e);
            say(log, context, "its scenes entry '%s': %s", owner, why);
            goto rollback;
        }
        for (j = 0; j < e->spawn_count; j++)
            if (name_equal(e->spawns[j].entitydef, owner)) {
                entry_free(e);
                say(log, context, "its scenes entry '%s' spawns itself%s", owner, NULL);
                goto rollback;
            }

        t->count++;
        added++;
    }
    mpj_free(root);
    return added;
rollback:
    while (t->count > start) entry_free(&t->rows[--t->count]);
refused:
    t->refused++;
    mpj_free(root);
    return -1;
}

typedef struct sc_load {
    sh_scene_table *t;
    int (*serves_now)(const char *);
    sh_scene_log_fn log;
    void *context;
    const char *salt;      /* the mod being read, for its refusal line */
} sc_load;

static void sc_refusal(void *context, const char *clause)
{
    sc_load *c = (sc_load *)context;
    char *line = sh_textf("SCENES: %s: %s; its scene nodes play at the player", c->salt, clause);
    c->log(c->context, line ? line : "SCENES: a mod's scenes could not be read (out of memory)");
    free(line);
}

static void sc_visit(void *context, const char *salt, const char *hash, int place, const char *extra)
{
    sc_load *c = (sc_load *)context;
    int i;
    (void)place;
    if (!salt || !hash || !extra || (c->serves_now && !c->serves_now(hash))) return;
    for (i = 0; i < c->t->count; i++) if (!strcmp(c->t->rows[i].bundle, salt)) return;   /* one build per salt */
    c->salt = salt;
    (void)sh_scene_add_bundle(c->t, salt, extra, strlen(extra), c->log ? sc_refusal : NULL, c);
}

int sh_scene_table_load(sh_scene_table *t, sh_scene_each_fn each, int (*serves_now)(const char *bundle_hash),
                        sh_scene_log_fn log, void *context)
{
    sc_load c;
    if (!t) return -1;
    sh_scene_table_free(t);
    memset(&c, 0, sizeof c);
    c.t = t;
    c.serves_now = serves_now;
    c.log = log;
    c.context = context;
    if (!each || each(sc_visit, &c) < 0) {
        sh_scene_table_free(t);
        return -1;
    }
    return t->count;
}

const sh_scene_entry *sh_scene_find(const sh_scene_table *t, const char *name)

{
    int i;
    if (!t || !name) return NULL;
    for (i = 0; i < t->count; i++)
        if (name_equal(t->rows[i].node, name)) return &t->rows[i];
    return NULL;
}

void sh_scene_table_free(sh_scene_table *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->count; i++) entry_free(&t->rows[i]);
    free(t->rows);
    t->rows = NULL;
    t->count = t->cap = t->refused = 0;
}

/* ---- spawns queued until the sync starts ------------------------------ */

int sh_scene_pending_set(sh_scene_pending_table *t, const void *node, const char *node_name,
                         const sh_scene_spawn *spawns, int count)
{
    sh_scene_pending copy, *row = NULL;
    int i;
    if (!t || !node || !spawns || count < 1) return 0;
    memset(&copy, 0, sizeof copy);
    copy.node = node;
    copy.node_name = _strdup(node_name ? node_name : "a scene node");
    copy.spawns = (sh_scene_spawn *)calloc((size_t)count, sizeof *copy.spawns);
    copy.count = count;
    if (!copy.node_name || !copy.spawns) { sh_scene_pending_release(&copy); return 0; }
    for (i = 0; i < count; i++) {
        copy.spawns[i].at = spawns[i].at;
        if (!(copy.spawns[i].entitydef = _strdup(spawns[i].entitydef ? spawns[i].entitydef : ""))) {
            sh_scene_pending_release(&copy);
            return 0;
        }
    }
    for (i = 0; i < t->count && !row; i++)
        if (t->rows[i].node == node) row = &t->rows[i];
    if (!row) {
        if (t->count == t->capacity) {
            int capacity = t->capacity ? t->capacity * 2 : 16;
            sh_scene_pending *grown = (sh_scene_pending *)realloc(t->rows, (size_t)capacity * sizeof *grown);
            if (!grown) { sh_scene_pending_release(&copy); return 0; }
            t->rows = grown;
            t->capacity = capacity;
        }
        row = &t->rows[t->count++];
        memset(row, 0, sizeof *row);
    }
    sh_scene_pending_release(row);
    *row = copy;
    return 1;
}

void sh_scene_pending_release(sh_scene_pending *row)
{
    if (!row) return;
    free(row->node_name);
    spawns_free(row->spawns, row->count);
    memset(row, 0, sizeof *row);
}

int sh_scene_pending_take(sh_scene_pending_table *t, const void *node, sh_scene_pending *out)
{
    int i;
    if (!t || !node) return 0;
    for (i = 0; i < t->count; i++) {
        if (t->rows[i].node != node) continue;
        if (out) *out = t->rows[i];
        else sh_scene_pending_release(&t->rows[i]);
        t->rows[i] = t->rows[--t->count];   /* the last row fills the hole; order does not matter */
        memset(&t->rows[t->count], 0, sizeof t->rows[t->count]);
        return 1;
    }
    return 0;
}

void sh_scene_pending_clear(sh_scene_pending_table *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->count; i++) sh_scene_pending_release(&t->rows[i]);
    free(t->rows);
    memset(t, 0, sizeof *t);
}

/* ---- frame math ------------------------------------------------------- */

static double wrap_deg(double d)
{
    d = fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

sh_scene_pose sh_scene_compose(sh_scene_pose a, sh_scene_pose b)
{
    sh_scene_pose r;
    double t = (double)a.yaw * (SC_PI / 180.0), c = cos(t), s = sin(t);
    r.x = (float)(a.x + c * b.x - s * b.y);
    r.y = (float)(a.y + s * b.x + c * b.y);
    r.z = a.z + b.z;
    r.yaw = (float)wrap_deg((double)a.yaw + b.yaw);
    return r;
}

sh_scene_pose sh_scene_inverse(sh_scene_pose a)
{
    sh_scene_pose r;
    double t = (double)a.yaw * (SC_PI / 180.0), c = cos(t), s = sin(t);
    r.x = (float)(-(c * a.x + s * a.y));
    r.y = (float)(s * a.x - c * a.y);
    r.z = -a.z;
    r.yaw = (float)wrap_deg(-(double)a.yaw);
    return r;
}

sh_scene_pose sh_scene_node_frame(const sh_scene_entry *e, sh_scene_pose node)
{
    static const sh_scene_pose identity = { 0.0f, 0.0f, 0.0f, 0.0f };
    return sh_scene_compose(node, sh_scene_inverse(e ? e->anchor : identity));
}

sh_scene_pose sh_scene_play_frame(const sh_scene_entry *e, sh_scene_pose node, sh_scene_pose player, int *reaimed)
{
    sh_scene_pose first = player;
    if (reaimed) *reaimed = 0;
    if (!e) return player;                       /* R313: the player's feet and view yaw */
    if (e->at_node) return sh_scene_node_frame(e, node);
    if (!e->has_player) return player;
    if (e->reaim) {
        /* From its first pose the scene sees the anchor at bearing beta; turn
         * that pose so the node stands there. */
        sh_scene_pose rel = sh_scene_compose(sh_scene_inverse(e->player), e->anchor);
        double dx = (double)node.x - player.x, dy = (double)node.y - player.y;
        if (dx * dx + dy * dy >= 1.0 && (rel.x != 0.0f || rel.y != 0.0f)) {
            double beta = atan2((double)rel.y, (double)rel.x) * (180.0 / SC_PI);
            first.yaw = (float)wrap_deg(atan2(dy, dx) * (180.0 / SC_PI) - beta);
            if (reaimed) *reaimed = 1;
        }
    }
    /* The scene's first player pose lands on the player's feet. */
    return sh_scene_compose(first, sh_scene_inverse(e->player));
}
