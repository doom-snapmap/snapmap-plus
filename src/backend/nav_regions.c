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
 * is not a cap, which is the only thing `truncated` reports. */
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
        sh_nav_region *r;
        int ed, edit, clip, box, at;
        float sx, sy, sz, cx, cy, cz;
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

        /* THE MARKER. Only a volume the author ticked "AI Navigation" on is a
         * region; every other blocking volume in the map is left alone. */
        if (!navr_bool(json, len, &doc, edit, "affectsNavmesh")) continue;

        clip = navr_member(json, len, &doc, edit, "clipModelInfo", '{');
        if (clip >= 0 && navr_str(json, len, &doc, clip, "type", text, sizeof text) &&
            strcmp(text, NAVR_CLIPMODEL_BOX) != 0) continue;   /* no top face to derive */

        box = navr_member(json, len, &doc, clip, "size", '{');
        sx = navr_num(json, len, &doc, box, "x", 0.0f);
        sy = navr_num(json, len, &doc, box, "y", 0.0f);
        sz = navr_num(json, len, &doc, box, "z", 0.0f);
        if (sx <= 0.0f || sy <= 0.0f) continue;    /* degenerate: no surface at all */

        at = navr_member(json, len, &doc, edit, "spawnPosition", '{');
        cx = navr_num(json, len, &doc, at, "x", 0.0f);
        cy = navr_num(json, len, &doc, at, "y", 0.0f);
        cz = navr_num(json, len, &doc, at, "z", 0.0f);

        if (out->region_count >= SH_NAVR_MAX_REGIONS) {
            out->truncated = 1;
            break;
        }
        r = &out->regions[out->region_count];
        /* spawnPosition is the box's CENTRE in x and y and its BOTTOM in z, so
         * the walkable surface is the top face, and a positive size leaves the
         * rectangle already normalised. */
        r->x0 = cx - sx / 2.0f;
        r->y0 = cy - sy / 2.0f;
        r->x1 = cx + sx / 2.0f;
        r->y1 = cy + sy / 2.0f;
        r->top_z = cz + sz;
        r->instance = -1;
        r->block_demons = navr_bool(json, len, &doc, edit, "blockDemons");
        r->entity = self;
        uid[out->region_count] = navr_index(navr_num(json, len, &doc, i, "uniqueId", -1.0f));
        out->region_count++;
    }

    arr = navr_member(json, len, &doc, 0, "instanceEntities", '{');
    if (arr >= 0 && out->region_count > 0)
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

    sh_shard_doc_free(&doc);
    return 1;
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
