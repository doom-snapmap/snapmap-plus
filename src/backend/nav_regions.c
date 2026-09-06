/* nav_regions.c -- see nav_regions.h. Read the ticked Blocking Boxes out of a
 * map and say which placed module instance each one belongs to.
 *
 * No JSON DOM is built. map_shards' container walker maps every object and
 * array in the document in one forward pass, and everything here reads members
 * out of those byte spans -- the same machinery the shard strip/insert path
 * drives, for the same reason: a map is megabytes and almost none of it is our
 * business. A document the walker refuses (unbalanced, or past its container
 * cap) is refused here too; a half-understood map must not produce half a
 * navigation.
 *
 * The map arrives from the publish service, so nothing here trusts a length, an
 * index or a count it has not checked itself. Every scan is bounded by a span
 * the walker proved, every number is copied into a fixed buffer before it is
 * converted, and a malformed map yields an empty answer rather than a fault.
 */
#include <stdlib.h>
#include <string.h>

#include "nav_regions.h"
#include "map_shards.h"

/* An instance's `moduleName` is a decl path: `maps/modules/<category>/<module>.decl`.
 * Anything else is not a module we can name, and a module we cannot name is a
 * module whose navigation we cannot serve. */
#define NAVR_MODULE_PREFIX  "maps/modules/"
#define NAVR_MODULE_SUFFIX  ".decl"

/* `snapmaps/volume/blocking` is what a Blocking Box inherits; every one of the
 * 2,411 in the reference map carries it, and carries `className`
 * `idVolume_Blocking` with it, so one of the two is enough. */
#define NAVR_VOLUME_INHERIT "snapmaps/volume/blocking"

/* THE MARKER itself, spelled once. The live refresh rejects most entities by
 * looking for this text before it parses anything, and a marker the two spell
 * differently would reject every volume in the map. */
#define NAVR_MARKER         "affectsNavmesh"

/* `clipModelInfo.type` for a box. The decl default, which means the field is
 * ABSENT from the map when it holds -- so absent reads as a box, and only a
 * type we can see and do not know disqualifies a volume. */
#define NAVR_CLIPMODEL_BOX  "CLIPMODEL_BOX"

/* The longest number token we will convert. Nothing the editor writes comes
 * close; a longer one is not a coordinate. */
#define NAVR_NUM_MAX        63

/* ==================================================================== */
/* reading members out of a walked document                              */
/* ==================================================================== */

/* Is `off` DIRECTLY inside `parent` -- inside it, and inside none of its
 * descendants? An entity carries several members called `size`, and only the
 * one under `clipModelInfo` is the box.
 *
 * The walker records containers in the order they open, so a container's
 * descendants are exactly the run that follows it until one opens past its
 * close. Checking that run is what makes this affordable:
 * sh_shard_doc_innermost sweeps the whole document, and this is called for
 * every member of every entity in the map. */
static int navr_direct(const sh_shard_doc *doc, int parent, size_t off)
{
    size_t i, stop;
    if (parent < 0 || (size_t)parent >= doc->count) return 0;
    if (off <= doc->c[parent].open || off >= doc->c[parent].close) return 0;
    stop = doc->c[parent].close;
    for (i = (size_t)parent + 1; i < doc->count && doc->c[i].open < stop; i++)
        if (doc->c[i].open < off && off < doc->c[i].close) return 0;
    return 1;
}

/* Offset of the value of member `key` directly inside `parent`, or 0. */
static int navr_value(const char *json, size_t len, const sh_shard_doc *doc,
                      int parent, const char *key, size_t *voff)
{
    size_t klen = strlen(key), at, stop;

    if (parent < 0 || (size_t)parent >= doc->count) return 0;
    at = doc->c[parent].open;
    stop = doc->c[parent].close;

    while (at < stop) {
        const char *q = sh_shard_find(json + at, stop - at, key, klen);
        size_t koff, v;
        if (!q) return 0;
        koff = (size_t)(q - json);
        at = koff + 1;
        if (koff == 0 || json[koff - 1] != '"') continue;
        if (koff + klen >= len || json[koff + klen] != '"') continue;
        v = koff + klen + 1;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != ':') continue;
        v++;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop) return 0;
        if (!navr_direct(doc, parent, koff)) continue;
        *voff = v;
        return 1;
    }
    return 0;
}

/* The container that opens at `voff`, if it is of `kind`. Containers are stored
 * in open order, so this is a search over a sorted key. */
static int navr_container(const sh_shard_doc *doc, size_t voff, char kind)
{
    size_t lo = 0, hi = doc->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (doc->c[mid].open == voff) return doc->c[mid].kind == kind ? (int)mid : -1;
        if (doc->c[mid].open < voff) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

/* Member `key` of `parent`, when its value is a container of `kind`. */
static int navr_member(const char *json, size_t len, const sh_shard_doc *doc,
                       int parent, const char *key, char kind)
{
    size_t v;
    if (!navr_value(json, len, doc, parent, key, &v)) return -1;
    if (json[v] != kind) return -1;
    return navr_container(doc, v, kind);
}

/* One number, copied out before it is converted: the buffer is not
 * NUL-terminated and strtod would read off the end of it. */
static int navr_num_at(const char *json, size_t len, size_t off, double *out)
{
    char text[NAVR_NUM_MAX + 1];
    size_t n = 0;
    char *end;

    while (off < len && n < NAVR_NUM_MAX) {
        char c = json[off];
        if (!sh_shard_is_digit(c) && c != '-' && c != '+' && c != '.' &&
            c != 'e' && c != 'E') break;
        text[n++] = c;
        off++;
    }
    if (n == 0) return 0;
    text[n] = '\0';
    *out = strtod(text, &end);
    return end != text;
}

/* A number member, or `dflt`. The editor omits a vector component that is zero,
 * so an absent member is not a malformed one. */
static float navr_num(const char *json, size_t len, const sh_shard_doc *doc,
                      int parent, const char *key, float dflt)
{
    size_t v;
    double d;
    if (!navr_value(json, len, doc, parent, key, &v)) return dflt;
    if (!navr_num_at(json, len, v, &d)) return dflt;
    return (float)d;
}

/* A bool member. ABSENT IS FALSE -- that is the decl default for both
 * `affectsNavmesh` and `blockDemons`, and it is why a map nobody has ticked
 * anything in yields no regions at all. */
static int navr_bool(const char *json, size_t len, const sh_shard_doc *doc,
                     int parent, const char *key)
{
    size_t v;
    if (!navr_value(json, len, doc, parent, key, &v)) return 0;
    return len - v >= 4 && memcmp(json + v, "true", 4) == 0;
}

/* A string member, into a bounded buffer. A value that does not fit is not one
 * of ours, so it reads as absent rather than as a prefix of itself. */
static int navr_str(const char *json, size_t len, const sh_shard_doc *doc,
                    int parent, const char *key, char *out, size_t cap)
{
    size_t v, n = 0;

    if (cap == 0) return 0;
    if (!navr_value(json, len, doc, parent, key, &v)) return 0;
    if (json[v] != '"') return 0;
    for (v++; v < len; v++) {
        char c = json[v];
        if (c == '\\') {
            /* nothing we compare against escapes anything; taking the next byte
             * literally keeps the scan in step with the closing quote */
            if (v + 1 >= len) return 0;
            c = json[++v];
        } else if (c == '"') {
            out[n] = '\0';
            return 1;
        }
        if (n + 1 >= cap) return 0;
        out[n++] = c;
    }
    return 0;
}

static void navr_vec3(const char *json, size_t len, const sh_shard_doc *doc,
                      int obj, float out[3])
{
    out[0] = navr_num(json, len, doc, obj, "x", 0.0f);
    out[1] = navr_num(json, len, doc, obj, "y", 0.0f);
    out[2] = navr_num(json, len, doc, obj, "z", 0.0f);
}

/* An array index or identifier as read from the map: a non-negative whole
 * number small enough to compare against a position without overflowing. */
static int navr_index(double v)
{
    if (!(v >= 0.0) || v > 1073741824.0) return -1;
    return (int)v;
}

/* The next element of a FLAT number array, and the position after it. Returns 0
 * at the end of the array and at the first element that is not a number, which
 * is how a mangled multimap stops attribution instead of misdirecting it. */
static int navr_flat_next(const char *json, size_t *p, size_t stop, double *out)
{
    size_t at = *p;
    while (at < stop && (sh_shard_is_ws(json[at]) || json[at] == ',')) at++;
    if (at >= stop) return 0;
    if (!navr_num_at(json, stop, at, out)) return 0;
    while (at < stop && json[at] != ',') at++;
    *p = at;
    return 1;
}

/* `maps/modules/ind_dlc/ind_totally_blank_room_4x.decl` -> `ind_dlc/ind_totally_blank_room_4x`.
 *
 * The category is everything before the LAST '/', so a nested category survives
 * intact -- the same right-to-left split the shard header uses. */
static int navr_module_name(const char *decl, char *out, size_t cap)
{
    size_t plen = sizeof NAVR_MODULE_PREFIX - 1;
    size_t slen = sizeof NAVR_MODULE_SUFFIX - 1;
    const char *body, *slash;
    size_t n;

    if (strncmp(decl, NAVR_MODULE_PREFIX, plen) != 0) return 0;
    body = decl + plen;
    n = strlen(body);
    if (n <= slen || strcmp(body + n - slen, NAVR_MODULE_SUFFIX) != 0) return 0;
    n -= slen;
    if (n == 0 || n >= cap) return 0;
    memcpy(out, body, n);
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (!slash || slash == out || slash[1] == '\0') {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* The walkable rectangle of one Blocking Box, read out of its `edit` object
 * into `r` -- everything about a region except whose it is. Returns 0 for a
 * volume with no top face to derive: a shape that is not a box, or a box with
 * no area.
 *
 * Both the load pass and the live refresh come through here, so the map on disk
 * and the map in the editor can never disagree about where a volume's walkable
 * surface is. They read the same two members for the same reason:
 * `spawnPosition` is the box's CENTRE in x and y and its BOTTOM in z, so the
 * surface is the top face, and a positive size leaves the rectangle already
 * normalised. */
static int navr_volume_rect(const char *json, size_t len, const sh_shard_doc *doc,
                            int edit, sh_nav_region *r)
{
    char text[64];
    int clip, box, at;
    float sx, sy, sz, cx, cy, cz;

    clip = navr_member(json, len, doc, edit, "clipModelInfo", '{');
    if (clip >= 0 && navr_str(json, len, doc, clip, "type", text, sizeof text) &&
        strcmp(text, NAVR_CLIPMODEL_BOX) != 0) return 0;    /* no top face to derive */

    box = navr_member(json, len, doc, clip, "size", '{');
    sx = navr_num(json, len, doc, box, "x", 0.0f);
    sy = navr_num(json, len, doc, box, "y", 0.0f);
    sz = navr_num(json, len, doc, box, "z", 0.0f);
    if (sx <= 0.0f || sy <= 0.0f) return 0;    /* degenerate: no surface at all */

    at = navr_member(json, len, doc, edit, "spawnPosition", '{');
    cx = navr_num(json, len, doc, at, "x", 0.0f);
    cy = navr_num(json, len, doc, at, "y", 0.0f);
    cz = navr_num(json, len, doc, at, "z", 0.0f);

    r->x0 = cx - sx / 2.0f;
    r->y0 = cy - sy / 2.0f;
    r->x1 = cx + sx / 2.0f;
    r->y1 = cy + sy / 2.0f;
    r->top_z = cz + sz;
    return 1;
}

/* ==================================================================== */
/* the volumes the load pass saw                                         */
/* ==================================================================== */

/* EVERY Blocking Box in the map that was read, ticked or not, and the instance
 * the map said owns it.
 *
 * The live refresh needs this and cannot get it from the region table. An
 * entity carries no instance of its own -- `instanceEntities` is the map's own
 * record of ownership and it is gone by the time a bake runs -- and the volume
 * an author ticks during a session is, by definition, one nobody had ticked
 * when the map was read, so it is not a region and would have nowhere to
 * belong. Recording every blocking volume here is exactly what makes ticking an
 * EXISTING box take effect immediately, and it is also what makes a box CREATED
 * this session honestly unattributable: it was never in the map that was read,
 * so it is not in here, so it is skipped rather than guessed at.
 *
 * This is the reader's bookkeeping, not part of the map, which is why it is
 * module-local rather than a field on sh_nav_map. It belongs to the map most
 * recently read AND TO NO OTHER: a refresh of a different sh_nav_map is refused
 * rather than attributed from a table that describes something else. Both entry
 * points run under the caller's own lock (nav_bake serializes load against
 * bake), which is what makes one shared table safe.
 *
 * The reference map holds 2,411 blocking volumes among 7,556 entities, so this
 * cap is the largest map anyone has published with room to spare. A volume past
 * it keeps working exactly as it did before -- it just does not get the live
 * refresh. */
#define NAVR_MAX_VOLUMES    4096

typedef struct navr_volume {
    unsigned entity;    /* index in the map's entities array == the live id */
    int      uid;       /* uniqueId, which is what instanceEntities addresses */
    int      instance;  /* -1 until the multimap says otherwise */
} navr_volume;

static struct {
    const sh_nav_map *owner;
    navr_volume       v[NAVR_MAX_VOLUMES];
    int               count;
} g_loaded;

/* The instance the load pass attributed the volume at entity index `entity` to,
 * or -1 for a volume it never saw. */
static int navr_loaded_owner(unsigned entity)
{
    int i;
    for (i = 0; i < g_loaded.count; i++)
        if (g_loaded.v[i].entity == entity) return g_loaded.v[i].instance;
    return -1;
}

/* ==================================================================== */
/* attribution                                                           */
/* ==================================================================== */

/* Give every region the instance that OWNS its volume, from `instanceEntities`.
 *
 * That member is an idIndexMultimap in CSR form: bucket b holds
 * `values[keyValues[b] .. keyValues[b + 1])`, and `values` holds entity
 * `uniqueId`s -- NOT positions in the entities array. There is one bucket per
 * instance and then a trailing ORPHAN bucket, so `keyValues` is
 * instance_count + 2 long (13 instances, 15 keyValues, 7,556 entities in the
 * reference map).
 *
 * The orphan bucket is deliberately never attributed. An entity in it belongs
 * to no instance, and the coordinates cannot rescue it: they are module-local,
 * so two instances of one module hold volumes at identical coordinates and
 * containment cannot tell them apart. Such a volume is dropped silently -- it
 * is not a cap, which is the only thing `truncated` reports.
 *
 * One walk answers the same question twice: for the regions this map already
 * has, and for every blocking volume in it, ticked or not, so a volume ticked
 * later in the session can still be given the owner the map recorded for it. */
static void navr_attribute(const char *json, size_t len, const sh_shard_doc *doc,
                           int mm, sh_nav_map *out, const int *uid)
{
    int kv[SH_NAVR_MAX_INSTANCES + 2];
    int kv_count = 0, arr, bucket = 0, pos = 0;
    size_t p, stop;
    double v;

    arr = navr_member(json, len, doc, mm, "keyValues", '[');
    if (arr < 0) return;
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    while (kv_count < (int)(sizeof kv / sizeof kv[0]) &&
           navr_flat_next(json, &p, stop, &v)) {
        int at = navr_index(v);
        if (at < 0) break;
        kv[kv_count++] = at;
    }

    arr = navr_member(json, len, doc, mm, "values", '[');
    if (arr < 0) return;
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    while (navr_flat_next(json, &p, stop, &v)) {
        int id = navr_index(v), r;
        while (bucket < out->instance_count && bucket + 1 < kv_count &&
               pos >= kv[bucket + 1]) bucket++;
        /* past the last instance we recorded: the rest of `values` is the
         * orphan bucket, or buckets for instances a cap cut off */
        if (bucket >= out->instance_count || bucket + 1 >= kv_count) return;
        if (id >= 0 && pos >= kv[bucket]) {
            for (r = 0; r < out->region_count; r++) {
                if (out->regions[r].instance < 0 && uid[r] == id) {
                    out->regions[r].instance = bucket;
                    break;
                }
            }
            for (r = 0; r < g_loaded.count; r++) {
                if (g_loaded.v[r].instance < 0 && g_loaded.v[r].uid == id) {
                    g_loaded.v[r].instance = bucket;
                    break;
                }
            }
        }
        pos++;
    }
}

/* ==================================================================== */
/* the map                                                               */
/* ==================================================================== */

int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out)
{
    /* the volume's uniqueId, parallel to out->regions, until attribution */
    int uid[SH_NAVR_MAX_REGIONS];
    sh_shard_doc doc;
    int arr, i, keep;
    unsigned index = 0;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    /* Disown the previous map's volumes before anything can fail: a refresh may
     * only ever run against a map this function finished reading. */
    g_loaded.owner = NULL;
    g_loaded.count = 0;
    if (!json || len == 0) return 0;
    if (!sh_shard_doc_build(json, len, &doc)) return 0;
    if (doc.c[0].kind != '{') {
        sh_shard_doc_free(&doc);
        return 0;
    }

    /* A map places modules. Without that list there is nothing an entity could
     * belong to, and this is not a map document. */
    arr = navr_member(json, len, &doc, 0, "instances", '[');
    if (arr < 0) {
        sh_shard_doc_free(&doc);
        return 0;
    }

    for (i = arr + 1; (size_t)i < doc.count && doc.c[i].open < doc.c[arr].close; i++) {
        char decl[SH_NAVR_MODULE_CAP + 32];
        sh_nav_instance *in;
        int origin;

        if (doc.c[i].parent != arr || doc.c[i].kind != '{') continue;
        if (out->instance_count >= SH_NAVR_MAX_INSTANCES) {
            out->truncated = 1;
            break;
        }
        in = &out->instances[out->instance_count++];
        /* An instance we cannot name still takes its slot. instanceEntities
         * addresses instances BY POSITION, so dropping one here would silently
         * re-attribute every volume in every bucket after it; its regions are
         * dropped at the end instead. */
        if (!navr_str(json, len, &doc, i, "moduleName", decl, sizeof decl) ||
            !navr_module_name(decl, in->module, sizeof in->module))
            in->module[0] = '\0';
        origin = navr_member(json, len, &doc, i, "origin", '{');
        navr_vec3(json, len, &doc, origin, in->origin);
        in->orientation = (int)navr_num(json, len, &doc, i, "orientation", 0.0f);
    }

    arr = navr_member(json, len, &doc, 0, "entities", '[');
    for (i = arr + 1; arr >= 0 && (size_t)i < doc.count &&
                      doc.c[i].open < doc.c[arr].close; i++) {
        char text[64];
        sh_nav_region region;
        int ed, edit, vuid;
        unsigned self;

        if (doc.c[i].parent != arr || doc.c[i].kind != '{') continue;
        /* `entity` is diagnostics only, so counting object elements is close
         * enough: no shipped map puts anything else in this array. */
        self = index++;

        ed = navr_member(json, len, &doc, i, "entityDef", '{');
        if (ed < 0) continue;
        if (!navr_str(json, len, &doc, ed, "inherit", text, sizeof text)) continue;
        if (strcmp(text, NAVR_VOLUME_INHERIT) != 0) continue;
        edit = navr_member(json, len, &doc,
                           navr_member(json, len, &doc, ed, "state", '{'), "edit", '{');
        if (edit < 0) continue;

        /* Every blocking volume is remembered, ticked or not, because the
         * author may tick this one after the map has been read and the only
         * record of who owns it is the one being walked right now. */
        vuid = navr_index(navr_num(json, len, &doc, i, "uniqueId", -1.0f));
        if (g_loaded.count < NAVR_MAX_VOLUMES) {
            navr_volume *v = &g_loaded.v[g_loaded.count++];
            v->entity = self;
            v->uid = vuid;
            v->instance = -1;
        }

        /* THE MARKER. Only a volume the author ticked "AI Navigation" on is a
         * region; every other blocking volume in the map is left alone. */
        if (!navr_bool(json, len, &doc, edit, NAVR_MARKER)) continue;

        if (!navr_volume_rect(json, len, &doc, edit, &region)) continue;

        if (out->region_count >= SH_NAVR_MAX_REGIONS) {
            out->truncated = 1;
            break;
        }
        region.instance = -1;
        region.block_demons = navr_bool(json, len, &doc, edit, "blockDemons");
        region.entity = self;
        out->regions[out->region_count] = region;
        uid[out->region_count] = vuid;
        out->region_count++;
    }

    arr = navr_member(json, len, &doc, 0, "instanceEntities", '{');
    /* The volumes nobody has ticked need attributing too, so the walk is worth
     * making for a map with no regions at all -- which is every map an author
     * is about to tick their first volume in. */
    if (arr >= 0 && (out->region_count > 0 || g_loaded.count > 0))
        navr_attribute(json, len, &doc, arr, out, uid);

    /* Keep what an instance owns and can be named for. A volume in the orphan
     * bucket, or one owned by an instance whose moduleName made no sense, has
     * no module whose AAS it could be merged into. */
    for (i = 0, keep = 0; i < out->region_count; i++) {
        int owner = out->regions[i].instance;
        if (owner < 0 || owner >= out->instance_count) continue;
        if (out->instances[owner].module[0] == '\0') continue;
        if (keep != i) out->regions[keep] = out->regions[i];
        out->instances[owner].region_count++;
        keep++;
    }
    if (keep < out->region_count)
        memset(&out->regions[keep], 0,
               (size_t)(out->region_count - keep) * sizeof out->regions[0]);
    out->region_count = keep;

    /* This map, and only this one, may now be refreshed from the live editor. */
    g_loaded.owner = out;

    sh_shard_doc_free(&doc);
    return 1;
}

/* ==================================================================== */
/* the live editor                                                       */
/* ==================================================================== */

/* One entity's serialized JSON. A Blocking Box is a few hundred bytes; this is
 * generous headroom for one, and an entity that does not fit in it is a
 * timeline or an encounter rather than a box, so refusing it costs nothing. */
#define NAVR_LIVE_JSON_CAP  (16 * 1024)

/* The scan stops here however high the caller's `highest_id` goes. A map's
 * entity budget is nowhere near it (7,556 in the reference map), and pressing
 * Play is not the moment to walk an unbounded id space. */
#define NAVR_LIVE_SCAN_MAX  65536

/* Scratch for one refresh, heap-held for the call and freed before it returns.
 * The regions are built here rather than in `m` because `m` may not be touched
 * at all unless the whole scan succeeds. */
typedef struct navr_live {
    char          json[NAVR_LIVE_JSON_CAP];
    sh_nav_region regions[SH_NAVR_MAX_REGIONS];
    int           count;
    int           capped;
} navr_live;

/* Read one live entity's JSON as a marked Blocking Box, filling everything
 * about `r` except whose it is. The document is one entity serialized by the
 * engine's own reflection, so it is the same shape the load pass walks per
 * entity -- with the entity object as the root instead of an array element. */
static int navr_live_region(const char *json, size_t len, sh_nav_region *r)
{
    sh_shard_doc doc;
    char text[64];
    int ed, edit, ok = 0;

    if (!sh_shard_doc_build(json, len, &doc)) return 0;
    if (doc.c[0].kind == '{') {
        ed = navr_member(json, len, &doc, 0, "entityDef", '{');
        if (ed >= 0 && navr_str(json, len, &doc, ed, "inherit", text, sizeof text) &&
            strcmp(text, NAVR_VOLUME_INHERIT) == 0) {
            edit = navr_member(json, len, &doc,
                               navr_member(json, len, &doc, ed, "state", '{'), "edit", '{');
            /* ABSENT IS FALSE here exactly as it is in the map: an untouched
             * volume simply has no `affectsNavmesh` member to read. */
            if (edit >= 0 && navr_bool(json, len, &doc, edit, NAVR_MARKER) &&
                navr_volume_rect(json, len, &doc, edit, r)) {
                r->block_demons = navr_bool(json, len, &doc, edit, "blockDemons");
                ok = 1;
            }
        }
    }
    sh_shard_doc_free(&doc);
    return ok;
}

int sh_nav_regions_refresh_live(sh_nav_map *m, int highest_id,
                                sh_navr_entity_valid valid,
                                sh_navr_entity_json get_json, void *ctx)
{
    navr_live *w;
    int id, top, answered = 0, marked = 0, i;

    /* Without both callbacks there is no live surface to read, and without the
     * map this reader last read there is no attribution for what it would say.
     * Both are -1 rather than an empty answer, because -1 is what leaves the
     * caller with the flags the map was loaded with instead of with none. */
    if (!m || !valid || !get_json) return -1;
    if (m != g_loaded.owner) return -1;

    top = highest_id;
    if (top > NAVR_LIVE_SCAN_MAX) top = NAVR_LIVE_SCAN_MAX;

    w = (navr_live *)malloc(sizeof *w);
    if (!w) return -1;
    w->count = 0;
    w->capped = 0;

    for (id = 0; id <= top; id++) {
        sh_nav_region r;
        int n, owner;

        if (!valid(id, ctx)) continue;
        n = get_json(id, w->json, (int)sizeof w->json, ctx);
        if (n <= 0) continue;
        answered++;
        /* A result that fills the buffer is what a truncating writer looks like
         * from here, and half a document is not one. */
        if (n >= (int)sizeof w->json) continue;

        /* Refuse the overwhelming majority before the container walk. A marked
         * Blocking Box carries both of these texts; anything missing either is
         * not one, and the walk is the expensive part of this loop. */
        if (!sh_shard_find(w->json, (size_t)n, NAVR_VOLUME_INHERIT,
                           sizeof NAVR_VOLUME_INHERIT - 1)) continue;
        if (!sh_shard_find(w->json, (size_t)n, NAVR_MARKER,
                           sizeof NAVR_MARKER - 1)) continue;

        memset(&r, 0, sizeof r);
        if (!navr_live_region(w->json, (size_t)n, &r)) continue;
        /* The table is full, so the scan stops rather than counting volumes it
         * cannot hand over: past here the return value would say more than
         * `regions` contains, and a cap is what `truncated` is for. */
        if (w->count >= SH_NAVR_MAX_REGIONS) {
            w->capped = 1;
            break;
        }
        marked++;

        /* WHOSE IT IS comes from the map, never from the entity. A volume the
         * load pass never saw was created this session, has no owner anywhere,
         * and is SKIPPED rather than guessed at -- coordinates are module-local,
         * so guessing would silently give one instance another's geometry. The
         * skipped ones are the difference between this function's return value
         * and `m->region_count`; they are not a cap, so `truncated` says
         * nothing about them. */
        owner = navr_loaded_owner((unsigned)id);
        if (owner < 0 || owner >= m->instance_count) continue;
        if (m->instances[owner].module[0] == '\0') continue;

        r.instance = owner;
        r.entity = (unsigned)id;
        w->regions[w->count++] = r;
    }

    /* Not one entity answered. That is a surface we could not read rather than
     * an editor holding nothing, and the two want the same treatment anyway:
     * leave the map exactly as it was loaded. */
    if (answered == 0) {
        free(w);
        return -1;
    }

    /* Commit. The instance table keeps its identities AND ITS POSITIONS --
     * attribution addresses instances by position, so moving one would
     * re-attribute every volume after it. Only `region_count` is rewritten,
     * because it counts the region table and would otherwise be counting a
     * region table that no longer exists. */
    for (i = 0; i < m->instance_count && i < SH_NAVR_MAX_INSTANCES; i++)
        m->instances[i].region_count = 0;
    for (i = 0; i < w->count; i++) {
        m->regions[i] = w->regions[i];
        m->instances[w->regions[i].instance].region_count++;
    }
    if (m->region_count > w->count)
        memset(&m->regions[w->count], 0,
               (size_t)(m->region_count - w->count) * sizeof m->regions[0]);
    m->region_count = w->count;
    if (w->capped) m->truncated = 1;

    free(w);
    return marked;
}

int sh_nav_regions_nth_instance(const sh_nav_map *m, const char *module, int n)
{
    int i, count, seen = 0;

    if (!m || !module || n < 0) return -1;
    count = m->instance_count;
    if (count > SH_NAVR_MAX_INSTANCES) count = SH_NAVR_MAX_INSTANCES;
    for (i = 0; i < count; i++) {
        if (m->instances[i].module[0] == '\0') continue;
        if (strcmp(m->instances[i].module, module) != 0) continue;
        if (seen++ == n) return i;
    }
    return -1;
}
