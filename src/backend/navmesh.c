/* Read, validate and serve map-scoped baked navigation payloads. Keep SEH inside the
 * table lock so error handling still releases it. Header paths and payload
 * sizes are bounded before native parsing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "navmesh.h"
#include "aas_edit.h"
#include "map_shards.h"
#include "map_carrier.h"
#include "config.h"
#include "backend_log.h"

/* Persistent navigation configuration keys registered in config.c. */
#define NAV_CONFIG_ENABLED  "navmesh.enabled"
#define NAV_CONFIG_EMBED    "navmesh.embed_in_saved_maps"

/* ==================================================================== */
/* state                                                                 */
/* ==================================================================== */

/* One (module, class) shard set, and its payload if it survived delivery.
 *
 * `payload` is RETAINED whenever the shards reassembled and the digest matched,
 * even when the module is refused for serving -- that is what lets a save
 * preserve a bake this client will not itself serve, instead of quietly
 * deleting the author's work. `served` is the narrower question. */
typedef struct nav_set {
    char     module[SH_SMNAV_MODULE_CAP];
    char     cls[SH_SMNAV_CLASS_CAP];
    char     digest[SH_SMNAV_DIGEST_CHARS + 1];
    char     geom[SH_SMNAV_GEOM_CHARS + 1];
    unsigned total;
    unsigned present;
    int      consistent;         /* headers agree, no duplicate or out-of-range index */
    sh_shard_chunk *chunks;      /* [total], pointers INTO the map buffer; scan-time only */
    unsigned char  *payload;     /* process-heap buffer, never engine map-heap storage */
    size_t          payload_len;
    int      valid;              /* passed the structural gate */
    int      served;             /* ...and its module was accepted */
    char     res_name[SH_SMNAV_RESNAME_CAP];    /* maps/modules/.../<m>.aas_<class> */
    char     cook_name[SH_SMNAV_RESNAME_CAP];   /* generated/maps/.../<m>.baas_<class> */
    char     reason[SH_SMNAV_REASON_CAP];       /* why it is not served */
} nav_set;

typedef struct nav_module {
    char     name[SH_SMNAV_MODULE_CAP];
    unsigned instances;      /* placements found in the map JSON */
    int      ok;             /* every class validated, and the module is placed once */
    char     reason[SH_SMNAV_REASON_CAP];
} nav_module;

static int nav_held_count(void);

static SRWLOCK    g_nav_lock = SRWLOCK_INIT;
static nav_set    g_sets[SH_SMNAV_MAX_SETS];
static size_t     g_set_count;
static nav_module g_modules[SH_SMNAV_MAX_MODULES];
static size_t     g_module_count;
static size_t     g_held_bytes;
static volatile LONG g_serve_count;
/* Advance on every deserialize, including disabled and empty maps, to expose
 * table reset activity in diagnostics.
 */
static volatile LONG g_build_count;
static size_t     g_last_build_sets;
static char       g_last_refusal[SH_SMNAV_REASON_CAP];

/* A map's member "nav" part as it was read, so a save carries it back
 * unchanged when nothing is held to re-embed: the feature was off at load, or
 * turned off since (process heap; NULL when none). Under g_nav_lock. */
static char      *g_raw_nav;
static size_t     g_raw_nav_len;

/* Disable serving after the first fault to avoid repeated partial opens. */
static volatile LONG g_faulted;
static volatile LONG g_installed;

static void nav_fault(const char *where)
{
    if (InterlockedExchange(&g_faulted, 1) == 0) {
        char line[224];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: DISABLED for this session -- a fault was caught in %s. The engine's own "
                    "navigation is served from here on, and nothing is retried.", where);
        backend_log(line);
    }
}

static void nav_refuse(const char *fmt, ...)
{
    char text[SH_SMNAV_REASON_CAP];
    char line[SH_SMNAV_REASON_CAP + 32];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    strncpy_s(g_last_refusal, sizeof g_last_refusal, text, _TRUNCATE);
    _snprintf_s(line, sizeof line, _TRUNCATE, "NAV: REFUSED -- %s", text);
    backend_log(line);
}

static int nav_config_on(const char *key)
{
    /* Both keys default ON: an unreadable config must not silently change what
     * a map does. */
    int on = 1;
    (void)sh_config_get_bool(key, &on, NULL);
    return on;
}

int sh_navmesh_enabled(void)
{
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) return 0;
    return nav_config_on(NAV_CONFIG_ENABLED);
}

/* ==================================================================== */
/* the header grammar                                                    */
/* ==================================================================== */

typedef struct nav_hdr {
    char     module[SH_SMNAV_MODULE_CAP];
    char     cls[SH_SMNAV_CLASS_CAP];
    char     digest[SH_SMNAV_DIGEST_CHARS + 1];
    char     geom[SH_SMNAV_GEOM_CHARS + 1];
    unsigned idx, total;
} nav_hdr;

static int nav_is_class_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int nav_is_module_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '/';
}

/* The module field is `category/module`, and may carry further '/' segments.
 * It must not be able to construct a path: no '.', no backslash, no empty
 * segment, no leading or trailing '/', and at least one '/' -- a bare module
 * name cannot reconstruct the resource names. */
static int nav_copy_module(const char *p, size_t n, char *out, size_t cap)
{
    size_t i;
    int slashes = 0;
    if (n == 0 || n >= cap) return 0;
    if (p[0] == '/' || p[n - 1] == '/') return 0;
    for (i = 0; i < n; i++) {
        if (!nav_is_module_char(p[i])) return 0;
        if (p[i] == '/') {
            if (i && p[i - 1] == '/') return 0;
            slashes++;
        }
    }
    if (slashes == 0) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int nav_copy_class(const char *p, size_t n, char *out, size_t cap)
{
    size_t i;
    if (n == 0 || n >= cap) return 0;
    for (i = 0; i < n; i++) if (!nav_is_class_char(p[i])) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int nav_copy_hex(const char *p, size_t n, char *out, size_t want)
{
    size_t i;
    if (n != want) return 0;
    for (i = 0; i < n; i++) if (!sh_shard_is_hex(p[i])) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* ==================================================================== */
/* the structural AAS gate                                               */
/* ==================================================================== */

static uint32_t nav_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t nav_be16(const unsigned char *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

static int32_t nav_be32s(const unsigned char *p) { return (int32_t)nav_be32(p); }

typedef char sh_aas_lumps_agree[(SH_AAS_LUMPS == SH_AAS_L__COUNT) ? 1 : -1];

typedef struct nav_aas {
    const unsigned char *p;
    size_t   len;
    size_t   off[SH_AAS_LUMPS];
    uint32_t count[SH_AAS_LUMPS];
} nav_aas;

static void nav_verr(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

#define NAV_REC(a, lump, i) ((a)->p + (a)->off[lump] + (size_t)(i) * SH_AAS_LUMP_DEFS[lump].record)

/* Require every BSP node to be reachable once from a root within the native
 * depth limit. Reject cycles, multiple parents and unreachable nodes.
 */
static int nav_walk_bsp(const nav_aas *a, char *err, size_t err_cap)
{
    enum { ST_REF = 1, ST_REFMANY = 2, ST_SEEN = 4 };
    uint32_t num_nodes = a->count[SH_AAS_L_NODES];
    uint32_t num_areas = a->count[SH_AAS_L_AREAS];
    uint32_t num_planes = a->count[SH_AAS_L_PLANES];
    unsigned char *state;
    uint32_t visited = 0, n;
    int rc = 0;
    /* Depth is capped before children are pushed, and a binary tree holds at
     * most one pending sibling per level, so the stack cannot outgrow the depth
     * limit. It is checked anyway. */
    struct { uint32_t node; uint32_t depth; } stack[SH_AAS_MAX_DEPTH + 4];

    if (num_nodes == 0) return 1;

    state = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, num_nodes);
    if (!state) {
        nav_verr(err, err_cap, "out of memory validating the node tree");
        return 0;
    }

    /* Pass 1: every plane index and every child is in range, and no node is
     * claimed by two parents. */
    for (n = 0; n < num_nodes; n++) {
        const unsigned char *rec = NAV_REC(a, SH_AAS_L_NODES, n);
        uint32_t plane = nav_be32(rec);
        int k;
        if (plane >= num_planes) {
            nav_verr(err, err_cap, "node %u references plane %u of %u", n, plane, num_planes);
            goto done;
        }
        for (k = 0; k < 2; k++) {
            int32_t child = nav_be32s(rec + 8 + 4 * k);
            if (child > 0) {
                if ((uint32_t)child >= num_nodes) {
                    nav_verr(err, err_cap, "node %u child %d is out of range (%u nodes)",
                             n, child, num_nodes);
                    goto done;
                }
                state[child] |= (unsigned char)((state[child] & ST_REF) ? ST_REFMANY : ST_REF);
            } else if (child < 0) {
                int64_t area = -(int64_t)child;
                if (area >= (int64_t)num_areas) {
                    nav_verr(err, err_cap, "node %u leaf references area %lld of %u",
                             n, (long long)area, num_areas);
                    goto done;
                }
            }
            /* child == 0 is "no child"; node 0 is never anybody's child */
        }
    }
    for (n = 0; n < num_nodes; n++) {
        if (state[n] & ST_REFMANY) {
            nav_verr(err, err_cap, "node %u has more than one parent; the BSP is not a tree", n);
            goto done;
        }
    }

    /* Pass 2: depth-first from every root, bounded. */
    for (n = 0; n < num_nodes; n++) {
        size_t sp = 0;
        if (state[n] & ST_REF) continue;         /* not a root */
        stack[sp].node = n;
        stack[sp].depth = 1;
        sp++;
        while (sp) {
            uint32_t node, depth;
            const unsigned char *rec;
            int k;
            sp--;
            node = stack[sp].node;
            depth = stack[sp].depth;
            if (state[node] & ST_SEEN) {
                nav_verr(err, err_cap, "node %u is reachable twice; the BSP is not a tree", node);
                goto done;
            }
            state[node] |= ST_SEEN;
            visited++;
            if (depth > SH_AAS_MAX_DEPTH) {
                nav_verr(err, err_cap, "the BSP tree is deeper than the %u the engine allows",
                         (unsigned)SH_AAS_MAX_DEPTH);
                goto done;
            }
            rec = NAV_REC(a, SH_AAS_L_NODES, node);
            for (k = 0; k < 2; k++) {
                int32_t child = nav_be32s(rec + 8 + 4 * k);
                if (child <= 0) continue;
                if (sp >= sizeof stack / sizeof stack[0]) {
                    nav_verr(err, err_cap, "the BSP walk exceeded its bounded stack");
                    goto done;
                }
                stack[sp].node = (uint32_t)child;
                stack[sp].depth = depth + 1;
                sp++;
            }
        }
    }
    if (visited != num_nodes) {
        nav_verr(err, err_cap, "%u of %u BSP nodes are unreachable; the graph contains a cycle",
                 num_nodes - visited, num_nodes);
        goto done;
    }
    rc = 1;

done:
    HeapFree(GetProcessHeap(), 0, state);
    return rc;
}

/* Validate per-area from/to chains against repeated reachabilities and
 * unbounded walks. Unlisted records are checked separately because the engine
 * rebuilds these lists.
 */
static int nav_walk_reach_chains(const nav_aas *a, char *err, size_t err_cap)
{
    uint32_t num_reach = a->count[SH_AAS_L_REACHABILITIES];
    uint32_t num_areas = a->count[SH_AAS_L_AREAS];
    size_t bytes = (size_t)((num_reach + 7u) / 8u);
    unsigned char *seen;
    unsigned *degrees;
    uint32_t area;
    int list, rc = 0;

    if (num_reach == 0) return 1;

    seen = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes * 2);
    if (!seen) {
        nav_verr(err, err_cap, "out of memory validating the reachability lists");
        return 0;
    }
    degrees = (unsigned *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (size_t)num_areas * sizeof *degrees);
    if (!degrees) {
        nav_verr(err, err_cap, "out of memory validating outgoing reachabilities");
        goto done;
    }
    /* Load rebuilds these lists from the records. An omitted list head must
     * not hide an overflow from this gate. Cross-indices were checked above. */
    for (area = 0; area < num_reach; area++) {
        unsigned from = nav_be16(NAV_REC(a, SH_AAS_L_REACHABILITIES, area) + 6);
        if (++degrees[from] > SH_AAS_MAX_AREA_REACHABILITIES) {
            nav_verr(err, err_cap, "area %u has more than %u outgoing reachabilities",
                     from, SH_AAS_MAX_AREA_REACHABILITIES);
            HeapFree(GetProcessHeap(), 0, degrees);
            goto done;
        }
    }
    HeapFree(GetProcessHeap(), 0, degrees);

    for (list = 0; list < 2; list++) {
        unsigned char *bits = seen + (size_t)list * bytes;
        size_t head_off = (list == 0) ? 0x14 : 0x18;
        size_t link_off = (list == 0) ? 0x20 : 0x24;
        for (area = 0; area < num_areas; area++) {
            unsigned degree = 0;
            int32_t at = nav_be32s(NAV_REC(a, SH_AAS_L_AREAS, area) + head_off);
            while (at != -1) {
                uint32_t idx;
                if (list == 0 && ++degree > SH_AAS_MAX_AREA_REACHABILITIES) {
                    nav_verr(err, err_cap,
                             "area %u has more than %u outgoing reachabilities",
                             area, SH_AAS_MAX_AREA_REACHABILITIES);
                    goto done;
                }
                if (at < 0 || (uint32_t)at >= num_reach) {
                    nav_verr(err, err_cap,
                             "area %u reachability list %d leaves the array at %d of %u",
                             area, list, at, num_reach);
                    goto done;
                }
                idx = (uint32_t)at;
                if (bits[idx >> 3] & (unsigned char)(1u << (idx & 7))) {
                    nav_verr(err, err_cap,
                             "reachability %u appears twice in list %d; the links form a cycle",
                             idx, list);
                    goto done;
                }
                bits[idx >> 3] |= (unsigned char)(1u << (idx & 7));
                at = nav_be32s(NAV_REC(a, SH_AAS_L_REACHABILITIES, idx) + link_off);
            }
        }
    }
    rc = 1;

done:
    HeapFree(GetProcessHeap(), 0, seen);
    return rc;
}

int sh_navmesh_validate_aas(const unsigned char *p, size_t len, char *err, size_t err_cap)
{
    nav_aas a;
    size_t off = SH_AAS_PREAMBLE_BYTES;
    uint32_t i, n;

    if (err && err_cap) err[0] = '\0';
    if (!p) { nav_verr(err, err_cap, "no payload"); return 0; }
    if (len > SH_AAS_MAX_BAKE_PAYLOAD) {
        nav_verr(err, err_cap, "the payload is %zu bytes, over the %u-byte budget",
                 len, (unsigned)SH_AAS_MAX_BAKE_PAYLOAD);
        return 0;
    }
    if (len < SH_AAS_PREAMBLE_BYTES + (size_t)SH_AAS_LUMPS * 4) {
        nav_verr(err, err_cap, "the payload is %zu bytes, too small to be an AAS file", len);
        return 0;
    }
    if (memcmp(p, SH_AAS_MAGIC, 4) != 0) {
        nav_verr(err, err_cap, "the payload does not start with the AAS magic");
        return 0;
    }
    if (p[4] != SH_AAS_MAJOR || p[5] != SH_AAS_MINOR) {
        nav_verr(err, err_cap, "the AAS version is %u.%u, not %u.%u",
                 (unsigned)p[4], (unsigned)p[5],
                 (unsigned)SH_AAS_MAJOR, (unsigned)SH_AAS_MINOR);
        return 0;
    }

    a.p = p;
    a.len = len;

    /* Require 22 bounded count-prefixed lumps ending exactly at EOF. */
    for (i = 0; i < SH_AAS_LUMPS; i++) {
        uint64_t bytes;
        if (len - off < 4) {
            nav_verr(err, err_cap, "lump %s: no room for its count", SH_AAS_LUMP_DEFS[i].name);
            return 0;
        }
        a.count[i] = nav_be32(p + off);
        off += 4;
        if (a.count[i] > SH_AAS_MAX_RECORDS) {
            nav_verr(err, err_cap, "lump %s: a count of %u is beyond any real file",
                     SH_AAS_LUMP_DEFS[i].name, a.count[i]);
            return 0;
        }
        bytes = (uint64_t)a.count[i] * SH_AAS_LUMP_DEFS[i].record;
        if (bytes > (uint64_t)(len - off)) {
            nav_verr(err, err_cap, "lump %s: %u records overrun the payload",
                     SH_AAS_LUMP_DEFS[i].name, a.count[i]);
            return 0;
        }
        a.off[i] = off;
        off += (size_t)bytes;
    }
    if (off != len) {
        nav_verr(err, err_cap, "%zu trailing bytes after the last lump", len - off);
        return 0;
    }

    if (a.count[SH_AAS_L_AREAS] > SH_AAS_MAX_AREAS) {
        nav_verr(err, err_cap, "%u areas, over the %u a u16 index can reach",
                 a.count[SH_AAS_L_AREAS], (unsigned)SH_AAS_MAX_AREAS);
        return 0;
    }
    if (a.count[SH_AAS_L_CLUSTERS] > 0xFFFFu) {
        nav_verr(err, err_cap, "%u clusters, over the %u a u16 index can reach",
                 a.count[SH_AAS_L_CLUSTERS], 0xFFFFu);
        return 0;
    }
    /* An area's first traversal point is a u16, so the table indexes at most 65536 points. */
    if (a.count[SH_AAS_L_TRAVERSALPOINTS] > SH_AAS_MAX_TRAVERSAL_POINTS) {
        nav_verr(err, err_cap, "%u traversal points, over the %u a u16 index can reach",
                 a.count[SH_AAS_L_TRAVERSALPOINTS], (unsigned)SH_AAS_MAX_TRAVERSAL_POINTS);
        return 0;
    }
    if (a.count[SH_AAS_L_AREABOUNDS] != a.count[SH_AAS_L_AREAS]) {
        nav_verr(err, err_cap, "areaBounds holds %u records for %u areas",
                 a.count[SH_AAS_L_AREABOUNDS], a.count[SH_AAS_L_AREAS]);
        return 0;
    }

    /* edge -> vertex */
    for (n = 0; n < a.count[SH_AAS_L_EDGES]; n++) {
        const unsigned char *rec = NAV_REC(&a, SH_AAS_L_EDGES, n);
        uint32_t v0 = nav_be32(rec), v1 = nav_be32(rec + 4);
        if (v0 >= a.count[SH_AAS_L_VERTICES] || v1 >= a.count[SH_AAS_L_VERTICES]) {
            nav_verr(err, err_cap, "edge %u references a vertex outside the %u present",
                     n, a.count[SH_AAS_L_VERTICES]);
            return 0;
        }
    }

    /* edgeIndex -> edges. The value is signed: the sign carries the traversal
     * direction, so the magnitude is what has to be in range. */
    for (n = 0; n < a.count[SH_AAS_L_EDGEINDEX]; n++) {
        int32_t v = nav_be32s(NAV_REC(&a, SH_AAS_L_EDGEINDEX, n));
        int64_t mag = (v < 0) ? -(int64_t)v : (int64_t)v;
        if (mag >= (int64_t)a.count[SH_AAS_L_EDGES]) {
            nav_verr(err, err_cap, "edgeIndex %u references edge %d of %u",
                     n, v, a.count[SH_AAS_L_EDGES]);
            return 0;
        }
    }

    /* areaCoverIndex -> cover */
    for (n = 0; n < a.count[SH_AAS_L_AREACOVERINDEX]; n++) {
        uint32_t v = nav_be32(NAV_REC(&a, SH_AAS_L_AREACOVERINDEX, n));
        if (v >= a.count[SH_AAS_L_COVER]) {
            nav_verr(err, err_cap, "areaCoverIndex %u references cover %u of %u",
                     n, v, a.count[SH_AAS_L_COVER]);
            return 0;
        }
    }

    /* areas: the spans they own, and the cluster they belong to */
    for (n = 0; n < a.count[SH_AAS_L_AREAS]; n++) {
        const unsigned char *rec = NAV_REC(&a, SH_AAS_L_AREAS, n);
        uint32_t num_edges   = nav_be16(rec + 0x06);
        uint32_t first_edge  = nav_be32(rec + 0x08);
        uint32_t cluster     = nav_be16(rec + 0x0C);
        uint32_t first_pvs   = nav_be32(rec + 0x10);
        uint32_t first_cover = nav_be16(rec + 0x20);
        uint32_t num_cover   = nav_be16(rec + 0x22);
        uint32_t first_trav  = nav_be16(rec + 0x24);
        uint32_t num_trav    = nav_be16(rec + 0x26);
        if (first_trav + num_trav > a.count[SH_AAS_L_TRAVERSALPOINTS]) {
            nav_verr(err, err_cap, "area %u claims traversal points %u..%u of %u",
                     n, first_trav, first_trav + num_trav, a.count[SH_AAS_L_TRAVERSALPOINTS]);
            return 0;
        }
        if ((uint64_t)first_edge + num_edges > a.count[SH_AAS_L_EDGEINDEX]) {
            nav_verr(err, err_cap, "area %u claims edgeIndex %u..%u of %u",
                     n, first_edge, first_edge + num_edges, a.count[SH_AAS_L_EDGEINDEX]);
            return 0;
        }
        if (cluster >= a.count[SH_AAS_L_CLUSTERS]) {
            nav_verr(err, err_cap, "area %u is in cluster %u of %u",
                     n, cluster, a.count[SH_AAS_L_CLUSTERS]);
            return 0;
        }
        if (first_pvs > a.count[SH_AAS_L_OBSTACLEPVS]) {
            nav_verr(err, err_cap, "area %u starts its obstacle PVS at %u of %u",
                     n, first_pvs, a.count[SH_AAS_L_OBSTACLEPVS]);
            return 0;
        }
        if ((uint64_t)first_cover + num_cover > a.count[SH_AAS_L_AREACOVERINDEX]) {
            nav_verr(err, err_cap, "area %u claims areaCoverIndex %u..%u of %u",
                     n, first_cover, first_cover + num_cover, a.count[SH_AAS_L_AREACOVERINDEX]);
            return 0;
        }
    }

    /* reachabilities: the areas they join */
    for (n = 0; n < a.count[SH_AAS_L_REACHABILITIES]; n++) {
        const unsigned char *rec = NAV_REC(&a, SH_AAS_L_REACHABILITIES, n);
        uint32_t from = nav_be16(rec + 0x06), to = nav_be16(rec + 0x08);
        if (from >= a.count[SH_AAS_L_AREAS] || to >= a.count[SH_AAS_L_AREAS]) {
            nav_verr(err, err_cap, "reachability %u joins areas %u and %u of %u",
                     n, from, to, a.count[SH_AAS_L_AREAS]);
            return 0;
        }
    }

    if (!nav_walk_reach_chains(&a, err, err_cap)) return 0;
    if (!nav_walk_bsp(&a, err, err_cap)) return 0;
    return 1;
}

/* ==================================================================== */
/* the map's own module placements                                       */
/* ==================================================================== */

/* Count module placements for payloads served under the module's shared
 * name. Repeated instances cannot receive distinct bytes through that name. Unknown spellings count
 * as zero; the engine will not request an unplaced module.
 */
static unsigned nav_count_instances(const char *json, size_t len, const char *module)
{
    char want[SH_SMNAV_RESNAME_CAP];
    size_t want_len, pos = 0;
    unsigned found = 0;

    if (_snprintf_s(want, sizeof want, _TRUNCATE, "maps/modules/%s.decl", module) < 0) return 0;
    want_len = strlen(want);

    while (pos < len) {
        const char *k = sh_shard_find(json + pos, len - pos, "\"moduleName\"", 12);
        size_t off;
        if (!k) break;
        off = (size_t)(k - json) + 12;
        pos = off;
        while (off < len && sh_shard_is_ws(json[off])) off++;
        if (off >= len || json[off] != ':') continue;
        off++;
        while (off < len && sh_shard_is_ws(json[off])) off++;
        if (off >= len || json[off] != '"') continue;
        off++;
        if (want_len > len - off) continue;
        if (memcmp(json + off, want, want_len) == 0 &&
            off + want_len < len && json[off + want_len] == '"') found++;
    }
    return found;
}

/* ==================================================================== */
/* the serving table                                                     */
/* ==================================================================== */

/* Caller holds the lock exclusively. */
static void nav_clear_locked(void)
{
    size_t i;
    for (i = 0; i < g_set_count; i++) {
        if (g_sets[i].chunks)  HeapFree(GetProcessHeap(), 0, g_sets[i].chunks);
        if (g_sets[i].payload) HeapFree(GetProcessHeap(), 0, g_sets[i].payload);
    }
    memset(g_sets, 0, sizeof g_sets);
    memset(g_modules, 0, sizeof g_modules);
    g_set_count = 0;
    g_module_count = 0;
    g_held_bytes = 0;
}

static nav_set *nav_find_set(const char *module, const char *cls)
{
    size_t i;
    for (i = 0; i < g_set_count; i++)
        if (strcmp(g_sets[i].module, module) == 0 && strcmp(g_sets[i].cls, cls) == 0)
            return &g_sets[i];
    return NULL;
}

static nav_module *nav_find_module(const char *name)
{
    size_t i;
    for (i = 0; i < g_module_count; i++)
        if (strcmp(g_modules[i].name, name) == 0) return &g_modules[i];
    return NULL;
}

/* The last path segment of `category/module` is the module, and the files
 * inside the module folder carry that same name. */
static const char *nav_leaf(const char *module)
{
    const char *slash = strrchr(module, '/');
    return slash ? slash + 1 : module;
}

/* Serve source .aas_<class> and cooked .baas_<class> aliases. The engine
 * tries cooked first, so a wrong or truncated alias silently falls back to
 * stock data; reject truncated names.
 */
static int nav_build_names(nav_set *s)
{
    const char *leaf = nav_leaf(s->module);
    if (_snprintf_s(s->res_name, sizeof s->res_name, _TRUNCATE,
                    "maps/modules/%s/%s.aas_%s", s->module, leaf, s->cls) < 0) return 0;
    if (_snprintf_s(s->cook_name, sizeof s->cook_name, _TRUNCATE,
                    "generated/maps/modules/%s/%s.baas_%s", s->module, leaf, s->cls) < 0) return 0;
    return 1;
}

/* Pass 1, one chunk: group it with its (module, class) set. Returns 0 when
 * the table is full. */
static int nav_collect_one(const nav_hdr *hp, const char *chunk, size_t chunk_len)
{
    const nav_hdr h = *hp;
    nav_set *s;
    {
        s = nav_find_set(h.module, h.cls);
        if (!s) {
            if (g_set_count >= SH_SMNAV_MAX_SETS) return 0;
            s = &g_sets[g_set_count++];
            memset(s, 0, sizeof *s);
            strcpy_s(s->module, sizeof s->module, h.module);
            strcpy_s(s->cls, sizeof s->cls, h.cls);
            strcpy_s(s->digest, sizeof s->digest, h.digest);
            strcpy_s(s->geom, sizeof s->geom, h.geom);
            s->total = h.total;
            s->consistent = 1;
            s->chunks = (sh_shard_chunk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                    (size_t)h.total * sizeof *s->chunks);
            if (!s->chunks) {
                s->consistent = 0;
                strcpy_s(s->reason, sizeof s->reason, "there was no memory to collect its parts");
                return 1;
            }
        }
        if (!s->chunks) return 1;

        if (s->total != h.total || strcmp(s->digest, h.digest) != 0 ||
            strcmp(s->geom, h.geom) != 0) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "the map carries two different bakes for it");
            return 1;
        }
        if (h.idx >= s->total) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a part index is out of range");
            return 1;
        }
        if (!chunk) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a part's value could not be read");
            return 1;
        }
        if (s->chunks[h.idx].filled) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a part index appears twice");
            return 1;
        }
        s->chunks[h.idx].p = chunk;
        s->chunks[h.idx].n = chunk_len;
        s->chunks[h.idx].filled = 1;
        s->present++;
    }
    return 1;
}

static void nav_overflow(void)
{
    nav_refuse("the map carries more baked module/class sets than the %d this build vets; "
               "the surplus is ignored", (int)SH_SMNAV_MAX_SETS);
}

/* One string member of a member set object, copied with the grammar's checks. */
static int nav_member_field(const char *obj, const char *end, const char *key, char *out, size_t cap,
                            int (*copy)(const char *, size_t, char *, size_t))
{
    const char *body;
    size_t n;
    if (sh_carrier_object_string(obj, end, key, &body, &n) != 1) return 0;
    return copy(body, n, out, cap);
}

static int nav_copy_digest(const char *p, size_t n, char *out, size_t cap)
{
    (void)cap;
    return nav_copy_hex(p, n, out, SH_SMNAV_DIGEST_CHARS);
}

static int nav_copy_geom(const char *p, size_t n, char *out, size_t cap)
{
    (void)cap;
    return nav_copy_hex(p, n, out, SH_SMNAV_GEOM_CHARS);
}

/* Pass 1 from the member's "nav" part [from, to) (map_carrier.h): one object
 * per set, its "data" the base64 chunks in order. A set object that does not
 * read is refused by name when it has one. */
static void nav_collect_member(const char *from, const char *to)
{
    const char *cursor = from, *set;
    size_t set_len;
    int overflow = 0, r;
    while ((r = sh_carrier_array_next(&cursor, to, &set, &set_len)) == 1) {
        const char *end = set + set_len, *data_cursor = NULL, *chunk;
        size_t chunk_len;
        unsigned total = 0, idx = 0;
        nav_hdr h;
        memset(&h, 0, sizeof h);
        if (*set != '{' ||
            !nav_member_field(set, end, "module", h.module, sizeof h.module, nav_copy_module) ||
            !nav_member_field(set, end, "class", h.cls, sizeof h.cls, nav_copy_class) ||
            !nav_member_field(set, end, "digest", h.digest, sizeof h.digest, nav_copy_digest) ||
            !nav_member_field(set, end, "geom", h.geom, sizeof h.geom, nav_copy_geom)) {
            nav_refuse("a navigation set in the map's member does not read; it is ignored");
            continue;
        }
        /* The "data" array: find it, count it, then collect it. */
        {
            const char *v = NULL;
            size_t vn = 0;
            if (sh_carrier_object_member(set, end, "data", &v, &vn) == 1 && *v == '[') data_cursor = v;
        }
        if (data_cursor) {
            const char *c = data_cursor;
            while (sh_carrier_array_next(&c, end, &chunk, &chunk_len) == 1) total++;
        }
        if (!data_cursor || total == 0 || total > SH_SMNAV_MAX_SHARDS) {
            nav_refuse("'%s' (%s) is not served -- its data in the map's member does not read", h.module, h.cls);
            continue;
        }
        h.total = total;
        while (sh_carrier_array_next(&data_cursor, end, &chunk, &chunk_len) == 1) {
            h.idx = idx++;
            if (*chunk == '"' && chunk_len >= 2) {
                if (!nav_collect_one(&h, chunk + 1, chunk_len - 2)) overflow = 1;
            } else if (!nav_collect_one(&h, NULL, 0)) overflow = 1;
        }
    }
    if (r < 0) nav_refuse("the navigation list in the map's member does not read; what read is kept");
    if (overflow) nav_overflow();
}

/* Pass 2: reassemble and verify the digest. A set that survives is HELD -- so a
 * save preserves it -- whether or not it ends up served. */
static void nav_reassemble(void)
{
    size_t i;
    for (i = 0; i < g_set_count; i++) {
        nav_set *s = &g_sets[i];
        unsigned char *payload;
        size_t payload_len = 0;
        char got[SH_SMNAV_DIGEST_CHARS + 1];
        int reason = 0;

        if (!s->consistent) {
            if (!s->reason[0])
                strcpy_s(s->reason, sizeof s->reason, "its part headers disagree");
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        if (s->present != s->total) {
            _snprintf_s(s->reason, sizeof s->reason, _TRUNCATE,
                        "only %u of its %u parts are present", s->present, s->total);
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        payload = sh_shard_b64_decode(s->chunks, s->total, SH_SMNAV_MAX_PAYLOAD,
                                      &payload_len, &reason);
        if (!payload) {
            strcpy_s(s->reason, sizeof s->reason,
                     reason == SH_SHARD_B64_TOO_BIG ? "its payload is over the size budget" :
                     reason == SH_SHARD_B64_NOMEM   ? "there was not enough memory to decode it" :
                                                      "its parts are not valid base64");
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        sh_shard_digest16(payload, payload_len, got);
        if (strcmp(got, s->digest) != 0) {
            HeapFree(GetProcessHeap(), 0, payload);
            _snprintf_s(s->reason, sizeof s->reason, _TRUNCATE,
                        "its digest is %s but its header says %s", got, s->digest);
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        if (payload_len > (size_t)SH_SMNAV_MAX_TOTAL - g_held_bytes) {
            HeapFree(GetProcessHeap(), 0, payload);
            strcpy_s(s->reason, sizeof s->reason,
                     "this map's bakes together exceed the memory budget");
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        s->payload = payload;
        s->payload_len = payload_len;
        g_held_bytes += payload_len;
    }
}

/* Pass 3: validate, then decide per MODULE. All or nothing: a carrier that
 * produced one bad payload is not trusted for its siblings. */
static void nav_accept(const char *json, size_t len)
{
    size_t i;

    for (i = 0; i < g_set_count; i++) {
        nav_set *s = &g_sets[i];
        nav_module *m = nav_find_module(s->module);
        if (!m) {
            if (g_module_count >= SH_SMNAV_MAX_MODULES) continue;
            m = &g_modules[g_module_count++];
            memset(m, 0, sizeof *m);
            strcpy_s(m->name, sizeof m->name, s->module);
            m->ok = 1;
            m->instances = nav_count_instances(json, len, m->name);
            if (m->instances > 1) {
                m->ok = 0;
                _snprintf_s(m->reason, sizeof m->reason, _TRUNCATE,
                            "the map places it %u times, and one resource name cannot serve two "
                            "instances different navigation", m->instances);
            }
        }
        if (!s->payload) {
            m->ok = 0;
            if (!m->reason[0])
                _snprintf_s(m->reason, sizeof m->reason, _TRUNCATE,
                            "its %s bake did not survive delivery", s->cls);
            continue;
        }
        {
            char verr[SH_SMNAV_REASON_CAP];
            if (sh_navmesh_validate_aas(s->payload, s->payload_len, verr, sizeof verr)) {
                s->valid = 1;
            } else {
                s->valid = 0;
                strcpy_s(s->reason, sizeof s->reason, verr);
                m->ok = 0;
                if (!m->reason[0])
                    _snprintf_s(m->reason, sizeof m->reason, _TRUNCATE,
                                "its %s bake is not a walkable AAS file (%s)", s->cls, verr);
            }
        }
    }

    for (i = 0; i < g_module_count; i++) {
        if (g_modules[i].ok) continue;
        nav_refuse("module '%s' serves NO navigation -- %s", g_modules[i].name,
                   g_modules[i].reason[0] ? g_modules[i].reason : "one of its bakes was refused");
    }

    for (i = 0; i < g_set_count; i++) {
        nav_set *s = &g_sets[i];
        nav_module *m = nav_find_module(s->module);
        if (!s->valid || !m || !m->ok) continue;
        if (!nav_build_names(s)) {
            strcpy_s(s->reason, sizeof s->reason, "its resource name is too long to serve");
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        s->served = 1;
    }
}

/* The scan itself, guarded, with the lock already held: the member's "nav"
 * part. */
static void nav_build_locked(const char *json, size_t len)
{
    __try {
        size_t from = 0, to = 0;
        if (json && len && sh_carrier_part(json, len, "nav", &from, &to) == 1) {
            nav_collect_member(json + from, json + to);
            nav_reassemble();
            nav_accept(json, len);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nav_fault("the map-load scan");
        nav_clear_locked();
    }
}

void sh_navmesh_build_from_map(const char *json, size_t len)
{
    size_t i, sets, served = 0, modules = 0, held;
    char line[256];
    int on = sh_navmesh_enabled();

    InterlockedIncrement(&g_build_count);

    AcquireSRWLockExclusive(&g_nav_lock);
    nav_clear_locked();
    g_last_refusal[0] = '\0';
    if (g_raw_nav) { HeapFree(GetProcessHeap(), 0, g_raw_nav); g_raw_nav = NULL; g_raw_nav_len = 0; }
    /* Off means off, including the table: a map loaded while the feature is
     * disabled must not leave the previous map's navigation live. Its member
     * part is kept as text so a save does not drop the author's bake, whether
     * the feature is still off at save time or has been turned on since. */
    if (on) nav_build_locked(json, len);
    else if (json && len) {
        size_t from = 0, to = 0;
        if (sh_carrier_part(json, len, "nav", &from, &to) == 1) {
            g_raw_nav = (char *)HeapAlloc(GetProcessHeap(), 0, to - from + 1);
            if (g_raw_nav) {
                memcpy(g_raw_nav, json + from, to - from);
                g_raw_nav[to - from] = 0;
                g_raw_nav_len = to - from;
            }
        }
    }
    for (i = 0; i < g_set_count; i++) {
        if (g_sets[i].chunks) {
            HeapFree(GetProcessHeap(), 0, g_sets[i].chunks);   /* they point into the map buffer */
            g_sets[i].chunks = NULL;
        }
        if (g_sets[i].served) served++;
    }
    for (i = 0; i < g_module_count; i++) if (g_modules[i].ok) modules++;
    sets = g_set_count;
    held = g_held_bytes;
    g_last_build_sets = sets;
    ReleaseSRWLockExclusive(&g_nav_lock);

    if (sets == 0) return;   /* No detailed report for a map without saved navigation. */

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: this map carries %zu baked module/class set(s); serving %zu of them for "
                "%zu module(s), holding %zu bytes", sets, served, modules, held);
    backend_log(line);
}

/* ==================================================================== */
/* embed                                                                 */
/* ==================================================================== */

/* The member's "nav" value for every held set (map_carrier.h): an array of
 * {"module","class","digest","geom","data":[chunks]} in table order. Process
 * heap, or NULL with the reason in err. *sets_out counts the sets written; a
 * set that cannot be written is logged and left out. */
static char *nav_part_locked(size_t *out_len, size_t *sets_out, char *err, size_t err_cap)
{
    size_t i, need = 2, w = 0, sets = 0;
    char *out;
    for (i = 0; i < g_set_count; i++) {
        const nav_set *s = &g_sets[i];
        size_t b64 = ((s->payload_len + 2) / 3) * 4, chunks;
        if (!s->payload) continue;
        chunks = (b64 + SH_SMNAV_CARRIER_CHUNK - 1) / SH_SMNAV_CARRIER_CHUNK;
        need += b64 + chunks * 3 + SH_SMNAV_MODULE_CAP + SH_SMNAV_CLASS_CAP + 128;
    }
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!out) {
        strncpy_s(err, err_cap, "there was no memory to encode the navigation", _TRUNCATE);
        return NULL;
    }
    out[w++] = '[';
    for (i = 0; i < g_set_count; i++) {
        const nav_set *s = &g_sets[i];
        char *b64;
        size_t b64_len, chunks, c;
        int n;
        if (!s->payload) continue;
        b64 = (char *)HeapAlloc(GetProcessHeap(), 0, ((s->payload_len + 2) / 3) * 4 + 1);
        if (!b64) {
            /* All or nothing: a save never silently drops one set of several. */
            HeapFree(GetProcessHeap(), 0, out);
            _snprintf_s(err, err_cap, _TRUNCATE,
                        "there was no memory to encode the navigation of '%s' (%s)", s->module, s->cls);
            return NULL;
        }
        b64_len = sh_shard_b64_encode(s->payload, s->payload_len, b64);
        /* A held payload is at most SH_SMNAV_MAX_PAYLOAD, which fits well within
         * SH_SMNAV_MAX_SHARDS chunks. */
        chunks = (b64_len + SH_SMNAV_CARRIER_CHUNK - 1) / SH_SMNAV_CARRIER_CHUNK;
        if (chunks == 0) chunks = 1;
        n = _snprintf_s(out + w, need + 1 - w, _TRUNCATE,
                        "%s{\"module\":\"%s\",\"class\":\"%s\",\"digest\":\"%s\",\"geom\":\"%s\",\"data\":[",
                        sets ? "," : "", s->module, s->cls, s->digest, s->geom);
        if (n < 0) {
            HeapFree(GetProcessHeap(), 0, b64);
            HeapFree(GetProcessHeap(), 0, out);
            strncpy_s(err, err_cap, "the navigation did not fit its buffer", _TRUNCATE);
            return NULL;
        }
        w += (size_t)n;
        for (c = 0; c < chunks; c++) {
            size_t at = c * SH_SMNAV_CARRIER_CHUNK, len = b64_len - at;
            if (len > SH_SMNAV_CARRIER_CHUNK) len = SH_SMNAV_CARRIER_CHUNK;
            if (c) out[w++] = ',';
            out[w++] = '"';
            memcpy(out + w, b64 + at, len);
            w += len;
            out[w++] = '"';
        }
        out[w++] = ']';
        out[w++] = '}';
        HeapFree(GetProcessHeap(), 0, b64);
        sets++;
    }
    out[w++] = ']';
    out[w] = 0;
    *out_len = w;
    *sets_out = sets;
    return out;
}

/* Guarded, with the lock already held: the member's "nav" part put (removed
 * when nothing is held). */
static char *nav_embed_locked(const char *json, size_t len,
                              size_t *cur_len_out, size_t *embedded_out)
{
    char *part = NULL, *out = NULL;
    size_t part_len = 0, out_len = 0, sets = 0;
    char err[SH_SMNAV_REASON_CAP] = "";

    *cur_len_out = len;
    *embedded_out = 0;
    __try {
        part = nav_part_locked(&part_len, &sets, err, sizeof err);
        if (!part) {
            char warn[SH_SMNAV_REASON_CAP + 96];
            _snprintf_s(warn, sizeof warn, _TRUNCATE,
                        "NAV: the navigation could NOT be written into this save (%s)", err);
            backend_log(warn);
        } else {
            out = sh_carrier_put(json, len, "nav",
                                 sets ? part : NULL, sets ? part_len : 0, &out_len, err, sizeof err);
            if (!out && err[0]) {
                char warn[SH_SMNAV_REASON_CAP + 96];
                _snprintf_s(warn, sizeof warn, _TRUNCATE,
                            "NAV: the navigation could NOT be written into this save (%s)", err);
                backend_log(warn);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nav_fault("the save-time re-embed");
        if (out) HeapFree(GetProcessHeap(), 0, out);
        out = NULL;
        sets = 0;
    }
    if (part) HeapFree(GetProcessHeap(), 0, part);
    if (out) {
        *cur_len_out = out_len;
        *embedded_out = sets;
    }
    return out;
}

char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len, int *failed)
{
    char *cur;
    size_t cur_len = len, embedded = 0;
    char line[224];

    if (out_len) *out_len = 0;
    if (failed) *failed = 0;
    if (!json || len == 0) return NULL;
    if (nav_held_count() == 0 || !nav_config_on(NAV_CONFIG_EMBED)) {
        /* Nothing held (the feature was off when the map loaded): the member part the map
         * arrived with goes back as it was read, whatever the setting is now. */
        char err[160] = "";
        size_t w = 0;
        AcquireSRWLockShared(&g_nav_lock);
        cur = g_raw_nav ? sh_carrier_put(json, len, "nav", g_raw_nav, g_raw_nav_len, &w, err, sizeof err)
                        : NULL;
        ReleaseSRWLockShared(&g_nav_lock);
        if (cur && out_len) *out_len = w;
        return cur;
    }

    AcquireSRWLockExclusive(&g_nav_lock);
    cur = nav_embed_locked(json, len, &cur_len, &embedded);
    ReleaseSRWLockExclusive(&g_nav_lock);

    if (!cur) {
        if (failed) *failed = 1;
        return NULL;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: the saved map CARRIES its baked navigation again -- %zu module/class "
                "set(s) in the snapmap-plus member, %zu -> %zu bytes", embedded, len, cur_len);
    backend_log(line);
    if (out_len) *out_len = cur_len;
    return cur;
}

/* ==================================================================== */
/* serve                                                                 */
/* ==================================================================== */

/* Path-tolerant compare: case-insensitive, '/' == '\\'. The engine asks with
 * forward slashes; be robust to either. */
static int nav_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

/* Guarded, with the shared lock already held. */
static int nav_open_locked(const char *name, unsigned char **out_bytes, size_t *out_len,
                           const char **which)
{
    size_t i;
    int hit = 0;

    __try {
        for (i = 0; i < g_set_count; i++) {
            nav_set *s = &g_sets[i];
            unsigned char *copy;
            if (!s->served || !s->payload) continue;
            if (nav_name_eq(name, s->res_name)) *which = "module";
            else if (nav_name_eq(name, s->cook_name)) *which = "cooked";
            else continue;
            /* Return a copy: a later map load can rebuild the table while the
             * engine still owns the stream.
             */
            copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                              s->payload_len ? s->payload_len : 1);
            if (!copy) break;
            memcpy(copy, s->payload, s->payload_len);
            *out_bytes = copy;
            *out_len = s->payload_len;
            hit = 1;
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nav_fault("the resource serve path");
        hit = 0;
    }
    return hit;
}

int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    const char *which = "module";
    char line[SH_SMNAV_RESNAME_CAP + 160];
    int hit;

    if (!name || !out_bytes || !out_len) return 0;
    *out_bytes = NULL;
    *out_len = 0;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) return 0;

    AcquireSRWLockShared(&g_nav_lock);
    hit = nav_open_locked(name, out_bytes, out_len, &which);
    ReleaseSRWLockShared(&g_nav_lock);

    if (!hit) {
        if (*out_bytes) HeapFree(GetProcessHeap(), 0, *out_bytes);
        *out_bytes = NULL;
        *out_len = 0;
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: SERVED baked navigation [%s] for '%s' (%zu bytes) [#%lu]",
                which, name, *out_len,
                (unsigned long)InterlockedIncrement(&g_serve_count));
    backend_log(line);
    return 1;
}

/* ==================================================================== */
/* report + install                                                      */
/* ==================================================================== */

void sh_navmesh_report(void (*out)(const char *fmt, ...))
{
    size_t i;
    if (!out) return;

    /* Always report the last map-load scan before current serving state. */
    out("navigation: %lu map load(s) this session; the last found %zu baked set(s)\n",
        (unsigned long)InterlockedCompareExchange(&g_build_count, 0, 0), g_last_build_sets);

    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) {
        out("navigation: custom navigation turned itself off after an error this session; "
            "restart DOOM to use it again.\n");
        return;
    }
    if (!nav_config_on(NAV_CONFIG_ENABLED)) {
        out("navigation: turned off by config (%s); the engine serves its own.\n",
            NAV_CONFIG_ENABLED);
        return;
    }

    AcquireSRWLockShared(&g_nav_lock);
    if (g_set_count == 0) {
        out("navigation: this map carries no bake; every module uses its shipped navmesh.\n");
    } else {
        out("navigation: %zu module/class set(s) from this map, %zu bytes held.\n",
            g_set_count, g_held_bytes);
        for (i = 0; i < g_module_count; i++) {
            const nav_module *m = &g_modules[i];
            out("  module %s -- %s\n", m->name,
                m->ok ? "serving" : (m->reason[0] ? m->reason : "refused"));
        }
        for (i = 0; i < g_set_count; i++) {
            const nav_set *s = &g_sets[i];
            if (s->served)
                out("  %s %s: in use (%zu bytes)\n", s->module, s->cls, s->payload_len);
            else
                out("  %s %s: not used (%s); a save keeps it\n", s->module, s->cls,
                    s->reason[0] ? s->reason : "its module was refused");
        }
    }
    ReleaseSRWLockShared(&g_nav_lock);
    out("  served %lu resource open(s) this session.\n",
        (unsigned long)InterlockedCompareExchange(&g_serve_count, 0, 0));
}

void sh_navmesh_install(void)
{
    if (InterlockedExchange(&g_installed, 1) != 0) return;
    backend_log(nav_config_on(NAV_CONFIG_ENABLED)
        ? "NAV: baked-navigation serving ARMED -- a map carrying validated baked navigation has its "
          "modules' navmeshes replaced for as long as it is loaded"
        : "NAV: baked-navigation serving OFF by config; maps keep their baked navigation and the engine "
          "serves its own navigation");
}

/* ==================================================================== */
/* internal + test seams                                                 */
/* ==================================================================== */

static int nav_held_count(void)
{
    size_t i, n = 0;
    AcquireSRWLockShared(&g_nav_lock);
    for (i = 0; i < g_set_count; i++) if (g_sets[i].payload) n++;
    ReleaseSRWLockShared(&g_nav_lock);
    return (int)n;
}

#ifdef SH_NAVMESH_TESTING
void sh_navmesh_test_reset(void)
{
    AcquireSRWLockExclusive(&g_nav_lock);
    nav_clear_locked();
    g_last_refusal[0] = '\0';
    ReleaseSRWLockExclusive(&g_nav_lock);
    InterlockedExchange(&g_faulted, 0);
    InterlockedExchange(&g_serve_count, 0);
    InterlockedExchange(&g_build_count, 0);
    g_last_build_sets = 0;
}

int sh_navmesh_test_served_count(void)
{
    size_t i;
    int n = 0;
    AcquireSRWLockShared(&g_nav_lock);
    for (i = 0; i < g_set_count; i++) if (g_sets[i].served) n++;
    ReleaseSRWLockShared(&g_nav_lock);
    return n;
}

/* Two names per served set: the module resource, then its cooked sibling. */
const char *sh_navmesh_test_served_name(int index)
{
    size_t i;
    int n = 0;
    for (i = 0; i < g_set_count; i++) {
        if (!g_sets[i].served) continue;
        if (n == index) return g_sets[i].res_name;
        n++;
        if (n == index) return g_sets[i].cook_name;
        n++;
    }
    return "";
}

int sh_navmesh_test_held_count(void)
{
    return nav_held_count();
}

const char *sh_navmesh_test_last_refusal(void)
{
    return g_last_refusal;
}
#endif
