/* Read Blocking Boxes and their module ownership from bounded map JSON spans.
 * map_shards supplies the container walk; no DOM is allocated. Reject
 * malformed or over-capacity documents rather than building partial
 * navigation.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>

#include "nav_regions.h"
#include "backend_log.h"
#include "map_shards.h"

/* Module decl names follow maps/modules/<category>/<module>.decl. */
#define NAVR_MODULE_PREFIX  "maps/modules/"
#define NAVR_MODULE_SUFFIX  ".decl"

/* Recognize Blocking Boxes by their inherited decl or idVolume_Blocking
 * class.
 */
#define NAVR_VOLUME_INHERIT "snapmaps/volume/blocking"

/* Shared marker spelling for the map reader and live prefilter. */
#define NAVR_MARKER         "noFlood"
#define NAVR_LEGACY_MARKER  "affectsNavmesh"

/* An omitted clipModelInfo.type uses the box default. */
#define NAVR_CLIPMODEL_BOX  "CLIPMODEL_BOX"

/* Bound number tokens before conversion. */
#define NAVR_NUM_MAX        63

/* ==================================================================== */
/* reading members out of a walked document                              */
/* ==================================================================== */

/* Test direct membership, excluding descendants. Several entity fields share
 * names such as size. Containers are in opening order, so only the following
 * descendant run needs scanning.
 */
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
    return end != text && *end == '\0' && _finite(*out);
}

/* A number member, or its inherited/default value. Native serialization omits
 * unchanged members, including nonzero components inherited from a decl. */
static float navr_num(const char *json, size_t len, const sh_shard_doc *doc,
                      int parent, const char *key, float dflt)
{
    size_t v;
    double d;
    if (!navr_value(json, len, doc, parent, key, &v)) return dflt;
    if (!navr_num_at(json, len, v, &d)) return (float)NAN;
    return (float)d;
}

/* The resolved declaration is native decl syntax, not the default-eliding
 * JSON state. Walk direct members so model scale, strings and comments cannot
 * masquerade as collision dimensions. All spans remain bounded by the idStr. */
typedef struct navr_decl_span { const char *p; size_t n; } navr_decl_span;

static int navr_decl_token(navr_decl_span *rest, navr_decl_span *token)
{
    const char *p = rest->p, *end = p + rest->n, *start;
    for (;;) {
        while (p < end && sh_shard_is_ws(*p)) p++;
        if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
            while (p < end && *p != '\n') p++;
        } else if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (end - p >= 2 && !(p[0] == '*' && p[1] == '/')) p++;
            if (end - p < 2) return 0;
            p += 2;
        } else break;
    }
    if (p == end) return 0;
    start = p++;
    if (*start == '"') {
        while (p < end && *p != '"') {
            if (*p++ == '\\') { if (p == end) return 0; p++; }
        }
        if (p == end) return 0;
        p++;
    } else if (!strchr("{}=;", *start)) {
        while (p < end && !sh_shard_is_ws(*p) && !strchr("{}=;\"", *p) &&
               !(end - p >= 2 && p[0] == '/' && (p[1] == '/' || p[1] == '*'))) p++;
    }
    token->p = start; token->n = (size_t)(p - start);
    rest->p = p; rest->n = (size_t)(end - p);
    return 1;
}

static int navr_decl_equal(navr_decl_span token, const char *text)
{
    return token.n == strlen(text) && !memcmp(token.p, text, token.n);
}

static int navr_decl_member(navr_decl_span scope, const char *key,
                             navr_decl_span *value)
{
    navr_decl_span token;
    int depth = 0;
    while (navr_decl_token(&scope, &token)) {
        if (!depth && navr_decl_equal(token, key)) {
            if (!navr_decl_token(&scope, &token) || !navr_decl_equal(token, "=") ||
                !navr_decl_token(&scope, &token)) return 0;
            *value = token;
            if (!navr_decl_equal(token, "{")) return 1;
            value->p = scope.p;
            depth = 1;
            while (navr_decl_token(&scope, &token)) {
                if (navr_decl_equal(token, "{")) depth++;
                if (navr_decl_equal(token, "}") && --depth == 0) {
                    value->n = (size_t)(token.p - value->p); return 1;
                }
            }
            return 0;
        }
        if (navr_decl_equal(token, "{")) depth++;
        if (navr_decl_equal(token, "}") && --depth < 0) return 0;
    }
    return 0;
}

int sh_nav_regions_decl_size(const char *text, size_t len, float size[3])
{
    static const char *axis[3] = { "x", "y", "z" };
    navr_decl_span scope = { text, len }, clip, box, token;
    float result[3];
    int i;
    if (!text || !len || !size ||
        !navr_decl_member(scope, "edit", &scope) ||
        !navr_decl_member(scope, "clipModelInfo", &clip) ||
        !navr_decl_member(clip, "type", &token) ||
        !navr_decl_equal(token, "\"CLIPMODEL_BOX\"") ||
        !navr_decl_member(clip, "size", &box)) return 0;
    for (i = 0; i < 3; i++) {
        char number[NAVR_NUM_MAX + 1], *end;
        double d;
        if (!navr_decl_member(box, axis[i], &token) || !token.n || token.n > NAVR_NUM_MAX)
            return 0;
        memcpy(number, token.p, token.n); number[token.n] = 0;
        d = strtod(number, &end);
        if (end != number + token.n || !_finite(d) || d <= 0 || d > FLT_MAX) return 0;
        result[i] = (float)d;
        if (result[i] <= 0) return 0;
    }
    memcpy(size, result, sizeof result); return 1;
}

/* Absent flags.noFlood and blockDemons members default to false. */
static int navr_bool(const char *json, size_t len, const sh_shard_doc *doc,
                     int parent, const char *key)
{
    size_t v;
    if (!navr_value(json, len, doc, parent, key, &v)) return 0;
    if (len - v < 4 || memcmp(json + v, "true", 4) != 0) return 0;
    v += 4;
    return v == len || sh_shard_is_ws(json[v]) || json[v] == ',' || json[v] == '}';
}

static int navr_marked(const char *json, size_t len, const sh_shard_doc *doc, int edit)
{
    return navr_bool(json, len, doc,
                     navr_member(json, len, doc, edit, "flags", '{'), NAVR_MARKER);
}

/* Read a bounded string; refuse overflow instead of accepting a prefix. */
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
    if (!(v >= 0.0) || v > 1073741824.0 || v != floor(v)) return -1;
    return (int)v;
}

/* Read the next flat-array number; stop at the end or an invalid element. */
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

/* Read spawnOrientation over an identity matrix: reflection omits default
 * members, including diagonal ones. Require orthonormality and determinant +1
 * to reject shear, degenerate transforms and reflections.
 */
static int navr_mat3(const char *json, size_t len, const sh_shard_doc *doc,
                     int edit, float m[3][3])
{
    static const char *ROW[3] = { "mat[0]", "mat[1]", "mat[2]" };
    static const char *COMP[3] = { "x", "y", "z" };
    int so, mat, r, c;
    double det;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) m[r][c] = (r == c) ? 1.0f : 0.0f;

    so = navr_member(json, len, doc, edit, "spawnOrientation", '{');
    if (so < 0) return 1;                      /* absent: identity, upright */
    mat = navr_member(json, len, doc, so, "mat", '{');
    if (mat < 0) return 1;

    for (r = 0; r < 3; r++) {
        int row = navr_member(json, len, doc, mat, ROW[r], '{');
        if (row < 0) continue;                 /* elided row: identity */
        for (c = 0; c < 3; c++) {
            size_t v;
            double d;
            if (navr_value(json, len, doc, row, COMP[c], &v)) {
                if (!navr_num_at(json, len, v, &d) || !_finite(d)) return 0;
                m[r][c] = (float)d;            /* elided member: identity */
            }
        }
    }

    for (r = 0; r < 3; r++) {
        double n2 = (double)m[r][0] * m[r][0] + (double)m[r][1] * m[r][1]
                  + (double)m[r][2] * m[r][2];
        if (n2 < 0.99 || n2 > 1.01) return 0;
    }
    for (r = 0; r < 3; r++) {
        int s = (r + 1) % 3;
        double dp = (double)m[r][0] * m[s][0] + (double)m[r][1] * m[s][1]
                  + (double)m[r][2] * m[s][2];
        if (dp < -0.01 || dp > 0.01) return 0;
    }
    det = (double)m[0][0] * ((double)m[1][1] * m[2][2] - (double)m[1][2] * m[2][1])
        - (double)m[0][1] * ((double)m[1][0] * m[2][2] - (double)m[1][2] * m[2][0])
        + (double)m[0][2] * ((double)m[1][0] * m[2][1] - (double)m[1][1] * m[2][0]);
    if (det < 0.99 || det > 1.01) return 0;
    return 1;
}

/* Encode the box as one representative face and its depth. Local x/y span
 * +/-size/2; local z spans [0,size.z]. Stored matrix rows are local basis
 * vectors in module space.
 *
 * Choose the greatest +Z normal; nav_geometry reconstructs the solid and
 * evaluates all faces. Load and legacy live refresh share this conversion.
 */
static int navr_volume_face(const char *json, size_t len, const sh_shard_doc *doc,
                            int edit, sh_nav_region *r, const float *resolved)
{
    static const float SU[4] = { -1.0f, +1.0f, +1.0f, -1.0f };
    static const float SV[4] = { -1.0f, -1.0f, +1.0f, +1.0f };
    char text[64];
    int clip, box, at, i, k, best, a, u, v;
    float sx, sy, sz, cx, cy, cz, m[3][3], half[3], centre[3], s;
    float bestz, bestarea;
    double sh;
    size_t value;

    clip = navr_member(json, len, doc, edit, "clipModelInfo", '{');
    if (clip < 0 && navr_value(json, len, doc, edit, "clipModelInfo", &value)) return 0;
    if (navr_value(json, len, doc, clip, "type", &value) &&
        (!navr_str(json, len, doc, clip, "type", text, sizeof text) ||
         strcmp(text, NAVR_CLIPMODEL_BOX) != 0)) return 0;

    box = navr_member(json, len, doc, clip, "size", '{');
    if (box < 0 && navr_value(json, len, doc, clip, "size", &value)) return 0;
    sx = navr_num(json, len, doc, box, "x", resolved ? resolved[0] : NAN);
    sy = navr_num(json, len, doc, box, "y", resolved ? resolved[1] : NAN);
    sz = navr_num(json, len, doc, box, "z", resolved ? resolved[2] : NAN);
    if (!_finite(sx) || !_finite(sy) || !_finite(sz) ||
        sx <= 0.0f || sy <= 0.0f || sz <= 0.0f) return 0;

    at = navr_member(json, len, doc, edit, "spawnPosition", '{');
    cx = navr_num(json, len, doc, at, "x", 0.0f);
    cy = navr_num(json, len, doc, at, "y", 0.0f);
    cz = navr_num(json, len, doc, at, "z", 0.0f);
    if (!_finite(cx) || !_finite(cy) || !_finite(cz)) return 0;

    if (!navr_mat3(json, len, doc, edit, m)) return 0;

    half[0] = sx / 2.0f;
    half[1] = sy / 2.0f;
    half[2] = sz / 2.0f;
    /* spawnPosition is the box BOTTOM in LOCAL z, so the centre sits half a
     * height along the local z axis -- which in world is row 2. */
    centre[0] = cx + m[2][0] * half[2];
    centre[1] = cy + m[2][1] * half[2];
    centre[2] = cz + m[2][2] * half[2];

    best = -1; bestz = -2.0f; bestarea = -1.0f;
    for (i = 0; i < 6; i++) {
        int ax = i >> 1;
        float sg = (i & 1) ? -1.0f : 1.0f;
        float nz = m[ax][2] * sg;
        float ar = half[(ax + 1) % 3] * half[(ax + 2) % 3];
        /* Ties -- a box at exactly 45 degrees about one axis has two faces at
         * 0.707 -- break toward the larger face, then the lower index, so the
         * choice is deterministic rather than dependent on float noise. */
        if (nz > bestz + 1e-4f || (nz > bestz - 1e-4f && ar > bestarea)) {
            best = i; bestz = nz; bestarea = ar;
        }
    }

    a = best >> 1;
    s = (best & 1) ? -1.0f : 1.0f;
    u = (a + 1) % 3;
    v = (a + 2) % 3;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 3; k++)
            r->c[i][k] = centre[k] + s * half[a] * m[a][k]
                       + SU[i] * half[u] * m[u][k]
                       + SV[i] * half[v] * m[v][k];
    for (k = 0; k < 3; k++) r->n[k] = m[a][k] * s;
    r->face = best;
    /* Depth reconstructs the occupied solid behind the face for collision tests. */
    r->depth = 2.0f * half[a];

    /* Clockwise from +Z requires negative XY shoelace area. Reject degenerate
     * projections.
     */
    sh = 0.0;
    for (i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        sh += (double)r->c[i][0] * r->c[j][1] - (double)r->c[j][0] * r->c[i][1];
    }
    if (sh > -1e-3 && sh < 1e-3) return 0;
    if (sh > 0.0) {
        float t[3];
        memcpy(t, r->c[1], sizeof t);
        memcpy(r->c[1], r->c[3], sizeof t);
        memcpy(r->c[3], t, sizeof t);
    }
    return 1;
}

/* ==================================================================== */
/* the volumes the load pass saw                                         */
/* ==================================================================== */

/* Legacy live-refresh ownership cache for all Blocking Boxes in the last
 * parsed map, including unmarked boxes. Newly created entities require a
 * complete-map snapshot to refresh this attribution.
 *
 * The cache belongs only to g_loaded_map. Callers serialize parsing and
 * refresh under their lock. Volumes beyond the cap are unavailable to the
 * legacy refresh.
 */
#define NAVR_MAX_VOLUMES    4096

typedef struct navr_volume {
    unsigned entity;    /* index in the map's entities array (NOT the live id) */
    int      uid;       /* uniqueId, which is what instanceEntities addresses */
    int      instance;  /* -1 until the multimap says otherwise */
} navr_volume;

static struct {
    const sh_nav_map *owner;
    navr_volume       v[NAVR_MAX_VOLUMES];
    int               count;
} g_loaded;

/* Look up ownership by uniqueId, which indexes both the sparse live entity
 * table and instanceEntities. The position in the JSON entities array is a
 * different identifier.
 */
static int navr_loaded_owner(int uid)
{
    int i;
    for (i = 0; i < g_loaded.count; i++)
        if (g_loaded.v[i].uid == uid) return g_loaded.v[i].instance;
    return -1;
}

/* ==================================================================== */
/* attribution                                                           */
/* ==================================================================== */

/* instanceEntities is a CSR multimap: bucket b uses
 * values[keyValues[b]..keyValues[b+1]), containing entity uniqueIds. It has
 * one bucket per instance plus an orphan bucket, so keyValues has
 * instance_count+2 entries.
 *
 * Do not attribute the orphan bucket by coordinates: repeated modules share
 * local space. The same walk records ownership for marked and unmarked boxes.
 */
static void navr_attribute(const char *json, size_t len, const sh_shard_doc *doc,
                           int mm, sh_nav_map *out, const int *uid)
{
    int kv[SH_NAVR_MAX_INSTANCES + 2];
    int kv_count = 0, arr, bucket = 0, pos = 0;
    size_t p, stop;
    double v;

    arr = navr_member(json, len, doc, mm, "keyValues", '[');
    if (arr < 0) { out->invalid_geometry=1; return; }
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    while (kv_count < (int)(sizeof kv / sizeof kv[0]) &&
           navr_flat_next(json, &p, stop, &v)) {
        int at = navr_index(v);
        if (at < 0) break;
        kv[kv_count++] = at;
    }
    if(kv_count!=out->instance_count+2 || kv[0]!=0) {
        out->invalid_geometry=1; return;
    }
    for(arr=1;arr<kv_count;arr++)if(kv[arr]<kv[arr-1]) {
        out->invalid_geometry=1; return;
    }

    arr = navr_member(json, len, doc, mm, "values", '[');
    if (arr < 0) { out->invalid_geometry=1; return; }
    p = doc->c[arr].open + 1;
    stop = doc->c[arr].close;
    while (navr_flat_next(json, &p, stop, &v)) {
        int id = navr_index(v), r;
        while (bucket < out->instance_count && bucket + 1 < kv_count &&
               pos >= kv[bucket + 1]) bucket++;
        /* past the last instance we recorded: the rest of `values` is the
         * orphan bucket, or buckets for instances a cap cut off */
        if (id < 0) { out->invalid_geometry=1; return; }
        if (bucket >= out->instance_count || bucket + 1 >= kv_count) {
            for(r=0;r<out->region_count;r++)if(uid[r]==id) {
                if(out->regions[r].instance!=-1)out->invalid_geometry=1;
                out->regions[r].instance=-2; /* explicitly orphaned */
            }
            pos++;continue;
        }
        if (id >= 0 && pos >= kv[bucket]) {
            for (r = 0; r < out->region_count; r++) {
                if (uid[r] == id) {
                    /* Shared IDs are usable only when every reference names
                     * the same room. Attribute every shape with that ID. */
                    if(out->regions[r].instance!=-1 &&
                       out->regions[r].instance!=bucket)out->invalid_geometry=1;
                    out->regions[r].instance = bucket;
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
    if(pos!=kv[kv_count-1])out->invalid_geometry=1;
}

/* ==================================================================== */
/* the map                                                               */
/* ==================================================================== */

typedef struct navr_patch {
    size_t at, remove;
    const char *text;
} navr_patch;

static int navr_patch_order(const void *a, const void *b)
{
    const navr_patch *pa = (const navr_patch *)a, *pb = (const navr_patch *)b;
    return pa->at < pb->at ? -1 : pa->at > pb->at;
}

/* Migrate only the known Blocking Box entity state. Preserve unrelated fields
 * and an explicit new marker (including false). Clear the native legacy flag
 * even when the new marker is present, so later toggle-off cannot resurrect it.
 * Collect all splices before writing: a refusal never partially migrates a map. */
char *sh_nav_regions_migrate(const char *json, size_t len, size_t *out_len)
{
    sh_shard_doc doc;
    navr_patch *patches = NULL;
    char *out = NULL;
    size_t count = 0, total = len, used = 0, read_at = 0, p;
    int arr, i;

    if (out_len) *out_len = len;
    if (!json || !len || !sh_shard_find(json, len, NAVR_LEGACY_MARKER,
                                       sizeof NAVR_LEGACY_MARKER - 1)) return NULL;
    if (!sh_shard_doc_build(json, len, &doc)) return NULL;
    if (doc.c[0].kind != '{') goto done;
    arr = navr_member(json, len, &doc, 0, "entities", '[');
    if (arr < 0 || doc.count > (size_t)-1 / sizeof *patches) goto done;
    patches = (navr_patch *)malloc(doc.count * sizeof *patches);
    if (!patches) goto done;
    for (i = arr + 1; (size_t)i < doc.count && doc.c[i].open < doc.c[arr].close; i++) {
        int ed, edit, flags;
        size_t legacy, value, first;
        char inherit[64];
        if (doc.c[i].parent != arr || doc.c[i].kind != '{') continue;
        ed = navr_member(json, len, &doc, i, "entityDef", '{');
        if (!navr_str(json, len, &doc, ed, "inherit", inherit, sizeof inherit) ||
            strcmp(inherit, NAVR_VOLUME_INHERIT) != 0) continue;
        edit = navr_member(json, len, &doc,
            navr_member(json, len, &doc, ed, "state", '{'), "edit", '{');
        if (!navr_bool(json, len, &doc, edit, NAVR_LEGACY_MARKER) ||
            !navr_value(json, len, &doc, edit, NAVR_LEGACY_MARKER, &legacy)) continue;
        if (count + 2 > doc.count) goto done;
        flags = navr_member(json, len, &doc, edit, "flags", '{');
        if (flags < 0) {
            /* A present non-object flags value cannot be replaced safely. */
            if (navr_value(json, len, &doc, edit, "flags", &value)) goto done;
            patches[count++] = (navr_patch){doc.c[edit].open + 1, 0,
                                            "\"flags\":{\"noFlood\":true},"};
        } else if (!navr_value(json, len, &doc, flags, NAVR_MARKER, &value)) {
            first = doc.c[flags].open + 1;
            while (first < doc.c[flags].close && sh_shard_is_ws(json[first])) first++;
            patches[count++] = (navr_patch){doc.c[flags].open + 1, 0,
                first == doc.c[flags].close ? "\"noFlood\":true" : "\"noFlood\":true,"};
        } else {
            /* Never make an invalid native bool look valid by dropping it. */
            size_t n = navr_bool(json, len, &doc, flags, NAVR_MARKER) ? 4 : 5;
            if (n == 5 && (len - value < 5 || memcmp(json + value, "false", 5))) goto done;
            value += n;
            if (value < len && !sh_shard_is_ws(json[value]) && json[value] != ',' &&
                json[value] != '}') goto done;
        }
        patches[count++] = (navr_patch){legacy, 4, "false"};
    }
    if (!count) goto done;
    qsort(patches, count, sizeof *patches, navr_patch_order);
    for (p = 0; p < count; p++) {
        size_t n = strlen(patches[p].text);
        if (patches[p].at < read_at || patches[p].at > len ||
            patches[p].remove > len - patches[p].at) goto done;
        read_at = patches[p].at + patches[p].remove;
        if (total - patches[p].remove > (size_t)-1 - n - 1) goto done;
        total = total - patches[p].remove + n;
    }
    out = (char *)HeapAlloc(GetProcessHeap(), 0, total + 1);
    if (!out) goto done;
    read_at = 0;
    for (p = 0; p < count; p++) {
        size_t copy = patches[p].at - read_at, n = strlen(patches[p].text);
        memcpy(out + used, json + read_at, copy); used += copy;
        memcpy(out + used, patches[p].text, n); used += n;
        read_at = patches[p].at + patches[p].remove;
    }
    memcpy(out + used, json + read_at, len - read_at);
    out[total] = '\0';
    if (out_len) *out_len = total;
done:
    free(patches);
    sh_shard_doc_free(&doc);
    return out;
}

int sh_nav_regions_read(const char *json, size_t len, sh_nav_map *out)
{
    return sh_nav_regions_read_resolved(json, len, out, NULL, NULL);
}

int sh_nav_regions_read_resolved(const char *json, size_t len, sh_nav_map *out,
                                sh_navr_entity_size read_size, void *ctx)
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
        /* Preserve instance slots even for invalid names: instanceEntities
         * addresses them by position.
         */
        if (!navr_str(json, len, &doc, i, "moduleName", decl, sizeof decl) ||
            !navr_module_name(decl, in->module, sizeof in->module))
            in->module[0] = '\0';
        origin = navr_member(json, len, &doc, i, "origin", '{');
        navr_vec3(json, len, &doc, origin, in->origin);
        if(!_finite(in->origin[0])||!_finite(in->origin[1])||!_finite(in->origin[2]))
            out->invalid_geometry=1;
        {
            int orientation=navr_member(json,len,&doc,i,"orientation",'{');
            float value=(orientation>=0
                ? navr_num(json,len,&doc,orientation,"value",0.0f)
                : navr_num(json,len,&doc,i,"orientation",0.0f));
            if(!_finite(value)||value<0||value>7||value!=floor(value))out->invalid_geometry=1;
            else in->orientation=(int)value;
        }
    }

    arr = navr_member(json, len, &doc, 0, "entities", '[');
    for (i = arr + 1; arr >= 0 && (size_t)i < doc.count &&
                      doc.c[i].open < doc.c[arr].close; i++) {
        char text[64];
        sh_nav_region region;
        int ed, edit, vuid;
        unsigned self;
        float resolved[3];

        if (doc.c[i].parent != arr || doc.c[i].kind != '{') continue;
        /* Diagnostic index counts object elements in entities. */
        self = index++;

        ed = navr_member(json, len, &doc, i, "entityDef", '{');
        if (ed < 0) continue;
        if (!navr_str(json, len, &doc, ed, "inherit", text, sizeof text)) continue;
        if (strcmp(text, NAVR_VOLUME_INHERIT) != 0) continue;
        edit = navr_member(json, len, &doc,
                           navr_member(json, len, &doc, ed, "state", '{'), "edit", '{');
        if (edit < 0) continue;

        /* Cache unmarked boxes too for the legacy live-refresh path. */
        vuid = navr_index(navr_num(json, len, &doc, i, "uniqueId", -1.0f));
        if (vuid < 0) out->ids_unusable = 1;
        for (keep = 0; keep < g_loaded.count; keep++)
            if (g_loaded.v[keep].uid == vuid) out->ids_unusable = 1;
        if (g_loaded.count < NAVR_MAX_VOLUMES) {
            navr_volume *v = &g_loaded.v[g_loaded.count++];
            v->entity = self;
            v->uid = vuid;
            v->instance = -1;
        }

        /* Only marked volumes contribute support; unmarked boxes may be obstacles. */
        if (!navr_marked(json, len, &doc, edit) &&
            !navr_bool(json, len, &doc, edit, "blockDemons")) continue;

        if ((read_size && !read_size(self, resolved, ctx)) ||
            !navr_volume_face(json, len, &doc, edit, &region, read_size ? resolved : NULL)) {
            char note[160];
            char kind[64];
            int clip = navr_member(json, len, &doc, edit, "clipModelInfo", 0x7b);
            if (!navr_str(json, len, &doc, clip, "type", kind, sizeof kind))
                strcpy_s(kind, sizeof kind, "a box");
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "NAV: volume %u has no shape this reader can use (%s)",
                        self, kind);
            backend_log(note);
            out->invalid_geometry = 1;
            continue;
        }

        if (out->region_count >= SH_NAVR_MAX_REGIONS) {
            out->truncated = 1;
            break;
        }
        region.instance = -1;
        region.marked = navr_marked(json,len,&doc,edit);
        region.block_demons = navr_bool(json, len, &doc, edit, "blockDemons");
        region.entity = self;
        out->regions[out->region_count] = region;
        uid[out->region_count] = vuid;
        out->region_count++;
    }

    arr = navr_member(json, len, &doc, 0, "instanceEntities", '{');
    /* Attribute unmarked boxes for later legacy marker refresh. */
    if (arr >= 0 && (out->region_count > 0 || g_loaded.count > 0))
        navr_attribute(json, len, &doc, arr, out, uid);
    else if(out->region_count>0)out->invalid_geometry=1;

    /* Drop regions without a named owning module, including orphans. */
    for (i = 0, keep = 0; i < out->region_count; i++) {
        int owner = out->regions[i].instance;
        if(owner==-1)out->invalid_geometry=1;
        if (owner < 0 || owner >= out->instance_count) continue;
        if (out->instances[owner].module[0] == '\0') continue;
        if(!out->regions[i].marked) {
            out->obstacles[out->obstacle_count++]=out->regions[i];
            continue;
        }
        if (keep != i) out->regions[keep] = out->regions[i];
        out->instances[owner].region_count++;
        keep++;
    }
    if (keep < out->region_count)
        memset(&out->regions[keep], 0,
               (size_t)(out->region_count - keep) * sizeof out->regions[0]);
    out->region_count = keep;

    /* This map, and only this one, may now be refreshed from the live editor,
     * and only while every box can be addressed by its own id. */
    if (out->ids_unusable) g_loaded.count = 0;
    g_loaded.owner = out;

    sh_shard_doc_free(&doc);
    return 1;
}

/* ==================================================================== */
/* the live editor                                                       */
/* ==================================================================== */

/* Bound serialized live-entity JSON; oversized entities are skipped. */
#define NAVR_LIVE_JSON_CAP  (16 * 1024)

/* Cap the live id scan so refresh cannot block Play indefinitely. */
#define NAVR_LIVE_SCAN_MAX  65536

/* Build a temporary region set and commit only after the scan succeeds. */
typedef struct navr_live {
    char          json[NAVR_LIVE_JSON_CAP];
    sh_nav_region regions[SH_NAVR_MAX_REGIONS];
    int           count;
    int           capped;
} navr_live;

/* Read one reflected entity document as a marked box. Its root is the entity
 * object; geometry parsing is shared with the map reader.
 */
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
             * volume simply has no `flags.noFlood` member to read. */
            if (edit >= 0 && navr_marked(json, len, &doc, edit) &&
                navr_volume_face(json, len, &doc, edit, r, NULL)) {
                r->block_demons = navr_bool(json, len, &doc, edit, "blockDemons");
                ok = 1;
            }
        }
    }
    sh_shard_doc_free(&doc);
    return ok;
}

static int navr_live_commit(sh_nav_map *m, navr_live *w,
                            int answered, int volumes, int marked);

void sh_nav_regions_adopt(sh_nav_map *m)
{
    if (m && g_loaded.owner) g_loaded.owner = m;
}

/* One entity id, folded into the work-in-progress refresh. Returns -1 when the
 * region table is full and the scan must stop, 0 otherwise. */
static int navr_live_one(sh_nav_map *m, navr_live *w, int id,
                         sh_navr_entity_valid valid, sh_navr_entity_json get_json,
                         void *ctx, int *answered, int *volumes, int *marked)
{
    sh_nav_region r;
    int n, owner;

    if (!valid(id, ctx)) return 0;
    n = get_json(id, w->json, (int)sizeof w->json, ctx);
    if (n <= 0) return 0;
    (*answered)++;
    /* A full buffer may be truncated; refuse it before parsing. */
    if (n >= (int)sizeof w->json) return 0;

    /* Cheap text prefilter before the more expensive container walk. */
    if (!sh_shard_find(w->json, (size_t)n, NAVR_VOLUME_INHERIT,
                       sizeof NAVR_VOLUME_INHERIT - 1)) return 0;
    /* Count boxes before markers to distinguish unticked boxes from the
     * wrong live surface.
     */
    (*volumes)++;
    if (!sh_shard_find(w->json, (size_t)n, NAVR_MARKER,
                       sizeof NAVR_MARKER - 1)) return 0;

    memset(&r, 0, sizeof r);
    if (!navr_live_region(w->json, (size_t)n, &r)) return 0;
    /* Stop at capacity so reported counts match stored regions. */
    if (w->count >= SH_NAVR_MAX_REGIONS) { w->capped = 1; return -1; }
    (*marked)++;

    /* A per-entity refresh cannot place ids absent from its ownership cache.
     * Skip them; complete-map snapshots handle newly created boxes.
     */
    owner = navr_loaded_owner(id);
    if (owner < 0 || owner >= m->instance_count) return 0;
    if (m->instances[owner].module[0] == '\0') return 0;

    r.instance = owner;
    r.entity = (unsigned)id;
    w->regions[w->count++] = r;
    return 0;
}

int sh_nav_regions_refresh_live(sh_nav_map *m, int highest_id,
                                sh_navr_entity_valid valid,
                                sh_navr_entity_json get_json, void *ctx)
{
    navr_live *w;
    int id, top, answered = 0, volumes = 0, marked = 0;

    /* Missing callbacks or mismatched attribution refuse the refresh without
     * clearing the map.
     */
    if (!m || !valid || !get_json) return -1;
    if (m != g_loaded.owner) return -1;
    /* Without cached volumes, every ownership lookup would fail; skip engine
     * serialization.
     */
    if (g_loaded.count == 0) return -1;

    top = highest_id;
    if (top > NAVR_LIVE_SCAN_MAX) top = NAVR_LIVE_SCAN_MAX;

    w = (navr_live *)malloc(sizeof *w);
    if (!w) return -1;
    w->count = 0;
    w->capped = 0;

    for (id = 0; id <= top; id++)
        if (navr_live_one(m, w, id, valid, get_json, ctx,
                          &answered, &volumes, &marked) < 0) break;

    return navr_live_commit(m, w, answered, volumes, marked);
}

/* The shape a complete snapshot recorded for this box.
 *
 * A live entity carries its current flags, but its spawnPosition is the value
 * it was created with: the editor writes a moved box out only when it
 * serializes the whole map. Reading shape from the entity draws the box where
 * it used to be. Measured on a box dragged 312 units: the entity kept
 * reporting its original spawnPosition indefinitely while the map reported the
 * new one. */
static int navr_shape_of(const sh_nav_map *m, unsigned entity, sh_nav_region *r)
{
    const sh_nav_region *src = NULL;
    int i;
    for (i = 0; i < m->region_count && !src; i++)
        if (m->regions[i].entity == entity) src = &m->regions[i];
    for (i = 0; i < m->obstacle_count && !src; i++)
        if (m->obstacles[i].entity == entity) src = &m->obstacles[i];
    if (!src) return 0;
    memcpy(r->c, src->c, sizeof r->c);
    memcpy(r->n, src->n, sizeof r->n);
    r->face = src->face;
    r->depth = src->depth;
    return 1;
}

int sh_nav_regions_refresh_known(sh_nav_map *m,
                                 sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json, void *ctx,
                                 int *read_count, const char **why)
{
    sh_nav_map *next;
    char *json;
    int i, ok = 0;
    const char *ignored;
    if (!why) why = &ignored;
    *why = "the cached box inventory is unavailable";
    if (read_count) *read_count = 0;
    if (!m || !valid || !get_json || m != g_loaded.owner ||
        !g_loaded.count || g_loaded.count >= NAVR_MAX_VOLUMES) return 0;
    next = (sh_nav_map *)malloc(sizeof *next);
    json = (char *)malloc(NAVR_LIVE_JSON_CAP);
    if (!next || !json) { *why = "allocation failed"; goto done; }
    *next = *m;
    memset(next->regions, 0, sizeof next->regions);
    memset(next->obstacles, 0, sizeof next->obstacles);
    next->region_count = next->obstacle_count = 0;
    for (i = 0; i < next->instance_count; i++) next->instances[i].region_count = 0;
    for (i = 0; i < g_loaded.count; i++) {
        const navr_volume *v = &g_loaded.v[i];
        sh_shard_doc doc;
        sh_nav_region r;
        char inherit[64];
        int n, ed, state, at, parsed = 0;
        *why = "a cached box no longer answers; a complete snapshot is required";
        if (v->uid < 0 || v->uid > NAVR_LIVE_SCAN_MAX || !valid(v->uid, ctx)) goto done;
        n = get_json(v->uid, json, NAVR_LIVE_JSON_CAP, ctx);
        if (n <= 0 || n >= NAVR_LIVE_JSON_CAP ||
            !sh_shard_doc_build(json, (size_t)n, &doc)) goto done;
        memset(&r, 0, sizeof r);
        ed = navr_member(json, n, &doc, 0, "entityDef", '{');
        state = navr_member(json, n, &doc, ed, "state", '{');
        at = navr_member(json, n, &doc, state, "edit", '{');
        if (doc.c[0].kind == '{' && at >= 0 &&
            navr_str(json, n, &doc, ed, "inherit", inherit, sizeof inherit) &&
            !strcmp(inherit, NAVR_VOLUME_INHERIT)) {
            r.marked = navr_marked(json, n, &doc, at);
            r.block_demons = navr_bool(json, n, &doc, at, "blockDemons");
            parsed = 1;
        }
        sh_shard_doc_free(&doc);
        if (!parsed) goto done;
        if (read_count) (*read_count)++;
        if (!r.marked && !r.block_demons) continue;
        /* A box with no recorded shape was neither a floor nor a wall when
         * the map was last read, so only a complete snapshot can place it. */
        *why = "a box became a floor or a wall; a complete snapshot is required";
        if (!navr_shape_of(m, v->entity, &r)) goto done;
        if (v->instance < 0 || v->instance >= next->instance_count ||
            !next->instances[v->instance].module[0]) continue;
        *why = "the geometry capacity requires a complete snapshot";
        if (next->region_count + next->obstacle_count >= SH_NAVR_MAX_REGIONS) goto done;
        r.instance = v->instance;
        r.entity = v->entity;
        if (r.marked) {
            next->regions[next->region_count++] = r;
            next->instances[r.instance].region_count++;
        } else next->obstacles[next->obstacle_count++] = r;
    }
    *m = *next;
    *why = "read";
    ok = 1;
done:
    free(json); free(next);
    return ok;
}

/* Refuse or commit a finished per-entity scan. A refusal leaves the map as it
 * was; the caller answers it with a complete snapshot. */
static int navr_live_commit(sh_nav_map *m, navr_live *w,
                            int answered, int volumes, int marked)
{
    int i;

    /* No readable entities means no trustworthy refresh; retain the prior map. */
    if (answered == 0) {
        free(w);
        return -1;
    }

    /* No Blocking Boxes means this may be the Play entity table, not the
     * editor's. Unticking markers leaves boxes present, so retain the prior
     * map on refusal.
     */
    if (volumes == 0) {
        free(w);
        return -1;
    }

    /* Found markers with no attributable owners indicate a mismatched id set.
     * Refuse the refresh instead of committing an empty plan.
     */
    if (marked > 0 && w->count == 0) {
        free(w);
        return -1;
    }

    /* Commit only regions. Preserve instance positions because ownership uses
     * their array indices.
     */
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
