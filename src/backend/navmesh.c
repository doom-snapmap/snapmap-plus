/* navmesh.c -- see navmesh.h. The runtime half of baked navigation: read the
 * map's `smnav1.` shards, prove the payloads are walkable, and serve them under
 * the module's own resource names for exactly as long as that map is loaded.
 *
 * Everything here is bounded and defensive by construction. The input is a
 * stranger's map: it arrives over the publish service, it is reassembled from
 * author-controlled base64, and the result is handed to a native binary parser
 * inside the engine. Nothing in this file trusts a length, an index or a count
 * it has not itself checked.
 *
 * The SEH guards sit INSIDE the table lock on purpose. An __except unwind skips
 * whatever follows it, so a guard wrapped around an Acquire/Release pair would
 * leave the table locked for the rest of the session on the one path that most
 * needs the table to keep working.
 *
 * Divergences from the frozen format document, both narrowings whose outcome
 * class (refuse and log) is unchanged:
 *   - the class field is accepted as [a-z0-9_]+ rather than only the three
 *     names the baker emits today, because that character set is what makes the
 *     constructed resource name safe; a class we do not know is simply never
 *     asked for.
 *   - the module field must be lowercase [a-z0-9_/-], carry at least one '/',
 *     and have no empty segment. Every shipped module name satisfies this, and
 *     it is what stops a header from constructing a path.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "navmesh.h"
#include "map_shards.h"
#include "config.h"
#include "backend_log.h"

/* Both are registered in config.c -- unlike `packages.embed_in_saved_maps`,
 * which shipped being read without ever being in the registry, so it always
 * answered "not set" and could not be turned off. */
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
    unsigned char  *payload;     /* HeapAlloc'd: the backend CRT heap, never the map heap */
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
/* Every pass through the deserialize funnel bumps this, whatever it finds --
 * including when the feature is off. It is reported by `sh_navmesh` because it
 * is the only thing that makes the clear-on-every-load rule OBSERVABLE from
 * inside one session: a count that advances while the set count drops to zero
 * IS the rule working, and saying so does not cost a second map load. */
static volatile LONG g_build_count;
static size_t     g_last_build_sets;
static char       g_last_refusal[SH_SMNAV_REASON_CAP];

/* First fault disables the feature for the whole session.
 *
 * The product's fault shield recovers in place, which is exactly wrong for a
 * per-open defect: it would turn one bug into an unbounded stream of
 * half-served opens that no forensics could attribute. One fault, one loud log,
 * feature off, engine serves its own navigation for the rest of the run. */
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

static int nav_parse_uint(const char *p, size_t n, unsigned *out)
{
    unsigned long v = 0;
    size_t i;
    if (n == 0 || n > 7) return 0;
    for (i = 0; i < n; i++) {
        if (!sh_shard_is_digit(p[i])) return 0;
        v = v * 10 + (unsigned long)(p[i] - '0');
    }
    *out = (unsigned)v;
    return 1;
}

/* Parse one header text (magic included, quotes excluded).
 *
 * FIVE trailing dot-separated fields are taken FROM THE RIGHT, and everything
 * between the magic and them is the module field -- which is exactly why a
 * module name containing '/' is safe here and one containing '.' is not
 * representable. Returns 1 only when every field is well formed. */
static int nav_parse_header(const char *p, size_t plen, nav_hdr *h)
{
    const char *f[5];
    size_t fl[5];
    size_t end = plen;
    int i;

    if (plen <= SH_SMNAV_MAGIC_LEN) return 0;
    if (memcmp(p, SH_SMNAV_MAGIC, SH_SMNAV_MAGIC_LEN) != 0) return 0;

    for (i = 4; i >= 0; i--) {
        size_t d = end;
        while (d > 0 && p[d - 1] != '.') d--;
        if (d == 0) return 0;            /* ran out of separators */
        f[i] = p + d;
        fl[i] = end - d;
        end = d - 1;                     /* step over the '.' itself */
    }
    if (end <= SH_SMNAV_MAGIC_LEN) return 0;

    if (!nav_copy_module(p + SH_SMNAV_MAGIC_LEN, end - SH_SMNAV_MAGIC_LEN,
                         h->module, sizeof h->module)) return 0;
    if (!nav_copy_class(f[0], fl[0], h->cls, sizeof h->cls)) return 0;
    if (!nav_parse_uint(f[1], fl[1], &h->idx)) return 0;
    if (!nav_parse_uint(f[2], fl[2], &h->total)) return 0;
    if (h->total == 0 || h->total > SH_SMNAV_MAX_SHARDS) return 0;
    if (!nav_copy_hex(f[3], fl[3], h->digest, SH_SMNAV_DIGEST_CHARS)) return 0;
    if (!nav_copy_hex(f[4], fl[4], h->geom, SH_SMNAV_GEOM_CHARS)) return 0;
    return 1;
}

/* ctx == NULL: every smnav shard. ctx == a set: only that set's own shards,
 * which is what makes a re-embed replace rather than accumulate. */
static int nav_shard_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    nav_hdr parsed;
    const nav_set *only = (const nav_set *)ctx;
    if (!nav_parse_header(hdr, hdr_len, &parsed)) return 0;
    if (!only) return 1;
    return strcmp(parsed.module, only->module) == 0 && strcmp(parsed.cls, only->cls) == 0;
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

/* The 22 arrays, in the exact order the engine's writer emits them, with the
 * record size each one uses. Order and sizes ARE the file format; nothing here
 * is negotiable or version-tolerant. */
enum {
    L_PLANES = 0, L_VERTICES, L_EDGES, L_EDGEINDEX, L_REACH, L_AREAS, L_NODES,
    L_PORTALS, L_PORTALINDEX, L_CLUSTERS, L_OBSTACLEPVS, L_REACHNAMES,
    L_ANIMNAMES, L_DEPNAMES, L_INTERACTNAMES, L_COVER, L_AREACOVERINDEX,
    L_TOUCHCOVERINDEX, L_TRAVPOINTS, L_HINTNODES, L_TREES, L_AREABOUNDS
};

typedef struct nav_lump_def { const char *name; unsigned record; } nav_lump_def;

static const nav_lump_def NAV_LUMPS[SH_AAS_LUMPS] = {
    { "planes",                  16 },
    { "vertices",                12 },
    { "edges",                   12 },
    { "edgeIndex",                4 },
    { "reachabilities",          40 },
    { "areas",                   44 },
    { "nodes",                   16 },
    { "portals",                 12 },
    { "portalIndex",              4 },
    { "clusters",                16 },
    { "obstaclePVS",              1 },
    { "reachNames",             132 },
    { "traversalAnimNames",     128 },
    { "dependencyNames",        128 },
    { "interactionEntityNames", 128 },
    { "cover",                   56 },
    { "areaCoverIndex",           4 },
    { "touchingCoverIndex",       4 },
    { "traversalPoints",         60 },
    { "hintNodes",               24 },
    { "trees",                   24 },
    { "areaBounds",              12 }
};

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

#define NAV_REC(a, lump, i) ((a)->p + (a)->off[lump] + (size_t)(i) * NAV_LUMPS[lump].record)

/* The BSP walk: every node reachable, no node reachable twice, no path longer
 * than the engine's own limit.
 *
 * A shipped BSP is a tree, so a node is the child of at most one other node and
 * every node hangs off some root. Enforcing both is what turns "walk the tree"
 * into a terminating operation on hostile input: a cyclic node graph is
 * precisely the crafted shape whose engine behaviour nobody has measured, and a
 * depth counter alone would never reach a cycle that sits below a shallow
 * subtree. */
static int nav_walk_bsp(const nav_aas *a, char *err, size_t err_cap)
{
    enum { ST_REF = 1, ST_REFMANY = 2, ST_SEEN = 4 };
    uint32_t num_nodes = a->count[L_NODES];
    uint32_t num_areas = a->count[L_AREAS];
    uint32_t num_planes = a->count[L_PLANES];
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
        const unsigned char *rec = NAV_REC(a, L_NODES, n);
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
            rec = NAV_REC(a, L_NODES, node);
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

/* The two per-area reachability chains the engine walks at runtime.
 *
 * Each reachability is pushed onto exactly one "from" list and one "to" list
 * when the file is built, so seeing one twice means the links were crafted --
 * and an unbounded walk is a hang, which is not a failure the engine would ever
 * report. Reachabilities that are on no list are left alone: nothing walks
 * them, so refusing them would be a rule the data does not owe us. */
static int nav_walk_reach_chains(const nav_aas *a, char *err, size_t err_cap)
{
    uint32_t num_reach = a->count[L_REACH];
    uint32_t num_areas = a->count[L_AREAS];
    size_t bytes = (size_t)((num_reach + 7u) / 8u);
    unsigned char *seen;
    uint32_t area;
    int list, rc = 0;

    if (num_reach == 0) return 1;

    seen = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes * 2);
    if (!seen) {
        nav_verr(err, err_cap, "out of memory validating the reachability lists");
        return 0;
    }

    for (list = 0; list < 2; list++) {
        unsigned char *bits = seen + (size_t)list * bytes;
        size_t head_off = (list == 0) ? 0x14 : 0x18;
        size_t link_off = (list == 0) ? 0x20 : 0x24;
        for (area = 0; area < num_areas; area++) {
            int32_t at = nav_be32s(NAV_REC(a, L_AREAS, area) + head_off);
            while (at != -1) {
                uint32_t idx;
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
                at = nav_be32s(NAV_REC(a, L_REACH, idx) + link_off);
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
    if (len > SH_SMNAV_MAX_PAYLOAD) {
        nav_verr(err, err_cap, "the payload is %zu bytes, over the %u-byte budget",
                 len, (unsigned)SH_SMNAV_MAX_PAYLOAD);
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

    /* The lump walk. This one rule -- 22 counts, fixed record sizes, landing on
     * exactly the last byte -- kills the whole truncation and overrun class,
     * because every check after it indexes an array whose extent is now known. */
    for (i = 0; i < SH_AAS_LUMPS; i++) {
        uint64_t bytes;
        if (len - off < 4) {
            nav_verr(err, err_cap, "lump %s: no room for its count", NAV_LUMPS[i].name);
            return 0;
        }
        a.count[i] = nav_be32(p + off);
        off += 4;
        if (a.count[i] > SH_AAS_MAX_RECORDS) {
            nav_verr(err, err_cap, "lump %s: a count of %u is beyond any real file",
                     NAV_LUMPS[i].name, a.count[i]);
            return 0;
        }
        bytes = (uint64_t)a.count[i] * NAV_LUMPS[i].record;
        if (bytes > (uint64_t)(len - off)) {
            nav_verr(err, err_cap, "lump %s: %u records overrun the payload",
                     NAV_LUMPS[i].name, a.count[i]);
            return 0;
        }
        a.off[i] = off;
        off += (size_t)bytes;
    }
    if (off != len) {
        nav_verr(err, err_cap, "%zu trailing bytes after the last lump", len - off);
        return 0;
    }

    if (a.count[L_AREAS] > SH_AAS_MAX_AREAS) {
        nav_verr(err, err_cap, "%u areas, over the %u a u16 index can reach",
                 a.count[L_AREAS], (unsigned)SH_AAS_MAX_AREAS);
        return 0;
    }
    if (a.count[L_CLUSTERS] > 0xFFFFu) {
        nav_verr(err, err_cap, "%u clusters, over the %u a u16 index can reach",
                 a.count[L_CLUSTERS], 0xFFFFu);
        return 0;
    }
    if (a.count[L_AREABOUNDS] != a.count[L_AREAS]) {
        nav_verr(err, err_cap, "areaBounds holds %u records for %u areas",
                 a.count[L_AREABOUNDS], a.count[L_AREAS]);
        return 0;
    }

    /* edge -> vertex */
    for (n = 0; n < a.count[L_EDGES]; n++) {
        const unsigned char *rec = NAV_REC(&a, L_EDGES, n);
        uint32_t v0 = nav_be32(rec), v1 = nav_be32(rec + 4);
        if (v0 >= a.count[L_VERTICES] || v1 >= a.count[L_VERTICES]) {
            nav_verr(err, err_cap, "edge %u references a vertex outside the %u present",
                     n, a.count[L_VERTICES]);
            return 0;
        }
    }

    /* edgeIndex -> edges. The value is signed: the sign carries the traversal
     * direction, so the magnitude is what has to be in range. */
    for (n = 0; n < a.count[L_EDGEINDEX]; n++) {
        int32_t v = nav_be32s(NAV_REC(&a, L_EDGEINDEX, n));
        int64_t mag = (v < 0) ? -(int64_t)v : (int64_t)v;
        if (mag >= (int64_t)a.count[L_EDGES]) {
            nav_verr(err, err_cap, "edgeIndex %u references edge %d of %u",
                     n, v, a.count[L_EDGES]);
            return 0;
        }
    }

    /* areaCoverIndex -> cover */
    for (n = 0; n < a.count[L_AREACOVERINDEX]; n++) {
        uint32_t v = nav_be32(NAV_REC(&a, L_AREACOVERINDEX, n));
        if (v >= a.count[L_COVER]) {
            nav_verr(err, err_cap, "areaCoverIndex %u references cover %u of %u",
                     n, v, a.count[L_COVER]);
            return 0;
        }
    }

    /* areas: the spans they own, and the cluster they belong to */
    for (n = 0; n < a.count[L_AREAS]; n++) {
        const unsigned char *rec = NAV_REC(&a, L_AREAS, n);
        uint32_t num_edges   = nav_be16(rec + 0x06);
        uint32_t first_edge  = nav_be32(rec + 0x08);
        uint32_t cluster     = nav_be16(rec + 0x0C);
        uint32_t first_pvs   = nav_be32(rec + 0x10);
        uint32_t first_cover = nav_be16(rec + 0x20);
        uint32_t num_cover   = nav_be16(rec + 0x22);
        if ((uint64_t)first_edge + num_edges > a.count[L_EDGEINDEX]) {
            nav_verr(err, err_cap, "area %u claims edgeIndex %u..%u of %u",
                     n, first_edge, first_edge + num_edges, a.count[L_EDGEINDEX]);
            return 0;
        }
        if (cluster >= a.count[L_CLUSTERS]) {
            nav_verr(err, err_cap, "area %u is in cluster %u of %u",
                     n, cluster, a.count[L_CLUSTERS]);
            return 0;
        }
        if (first_pvs > a.count[L_OBSTACLEPVS]) {
            nav_verr(err, err_cap, "area %u starts its obstacle PVS at %u of %u",
                     n, first_pvs, a.count[L_OBSTACLEPVS]);
            return 0;
        }
        if ((uint64_t)first_cover + num_cover > a.count[L_AREACOVERINDEX]) {
            nav_verr(err, err_cap, "area %u claims areaCoverIndex %u..%u of %u",
                     n, first_cover, first_cover + num_cover, a.count[L_AREACOVERINDEX]);
            return 0;
        }
    }

    /* reachabilities: the areas they join */
    for (n = 0; n < a.count[L_REACH]; n++) {
        const unsigned char *rec = NAV_REC(&a, L_REACH, n);
        uint32_t from = nav_be16(rec + 0x06), to = nav_be16(rec + 0x08);
        if (from >= a.count[L_AREAS] || to >= a.count[L_AREAS]) {
            nav_verr(err, err_cap, "reachability %u joins areas %u and %u of %u",
                     n, from, to, a.count[L_AREAS]);
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

/* How many times does this map place `module`?
 *
 * The shadow is keyed by resource name, so two instances of one module cannot
 * be served different navigation. The bake refuses to produce such a payload,
 * and this re-checks it on load rather than trusting the carrier. An instance
 * is a `"moduleName": "maps/modules/<category>/<module>.decl"` member of the
 * map's instance list. A spelling we do not recognise reads as zero, which
 * serves rather than refuses: the engine simply never asks for a module the map
 * does not place, so a string compare is not allowed to reject a map. */
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

/* The two names one baked class answers to.
 *
 * The cooked name prefixes 'b' to the WHOLE extension -- `.b` + `aas_monster48`
 * = `.baas_monster48` -- not to the class. That is what the game's own archive
 * index spells, and getting it wrong is invisible rather than loud: the engine
 * asks for the COOKED name first, so a wrong cooked name simply misses, the
 * shipped payload answers, and the source name is never requested at all. The
 * table then reports six names happily served while nothing has been served,
 * and the map plays on its shipped navigation. Both spellings are pinned by
 * literal-string assertions in navmesh_test.c for exactly that reason.
 *
 * A truncated name would fail the same silent way, so a truncation refuses the
 * set rather than leaving it looking served. */
static int nav_build_names(nav_set *s)
{
    const char *leaf = nav_leaf(s->module);
    if (_snprintf_s(s->res_name, sizeof s->res_name, _TRUNCATE,
                    "maps/modules/%s/%s.aas_%s", s->module, leaf, s->cls) < 0) return 0;
    if (_snprintf_s(s->cook_name, sizeof s->cook_name, _TRUNCATE,
                    "generated/maps/modules/%s/%s.baas_%s", s->module, leaf, s->cls) < 0) return 0;
    return 1;
}

/* Pass 1: group every shard in the buffer by (module, class). */
static void nav_collect(const char *json, size_t len)
{
    size_t pos = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    nav_hdr h;
    int overflow = 0;

    while (sh_shard_next(json, len, SH_SMNAV_MAGIC, SH_SMNAV_MAGIC_LEN, &pos,
                         &hdr, &hdr_len, &chunk, &chunk_len)) {
        nav_set *s;
        if (!nav_parse_header(hdr, hdr_len, &h)) continue;   /* prose, not a shard */

        s = nav_find_set(h.module, h.cls);
        if (!s) {
            if (g_set_count >= SH_SMNAV_MAX_SETS) { overflow = 1; continue; }
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
                strcpy_s(s->reason, sizeof s->reason, "there was no memory to collect its shards");
                continue;
            }
        }
        if (!s->chunks) continue;

        if (s->total != h.total || strcmp(s->digest, h.digest) != 0 ||
            strcmp(s->geom, h.geom) != 0) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "the map carries two different bakes for it");
            continue;
        }
        if (h.idx >= s->total) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a shard index is out of range");
            continue;
        }
        if (!chunk) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a shard's value could not be read");
            continue;
        }
        if (s->chunks[h.idx].filled) {
            s->consistent = 0;
            strcpy_s(s->reason, sizeof s->reason, "a shard index appears twice");
            continue;
        }
        s->chunks[h.idx].p = chunk;
        s->chunks[h.idx].n = chunk_len;
        s->chunks[h.idx].filled = 1;
        s->present++;
    }

    if (overflow)
        nav_refuse("the map carries more baked module/class sets than the %d this build vets; "
                   "the surplus is ignored", (int)SH_SMNAV_MAX_SETS);
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
                strcpy_s(s->reason, sizeof s->reason, "its shard headers disagree");
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        if (s->present != s->total) {
            _snprintf_s(s->reason, sizeof s->reason, _TRUNCATE,
                        "only %u of its %u shards are present", s->present, s->total);
            nav_refuse("'%s' (%s) is not served -- %s", s->module, s->cls, s->reason);
            continue;
        }
        payload = sh_shard_b64_decode(s->chunks, s->total, SH_SMNAV_MAX_PAYLOAD,
                                      &payload_len, &reason);
        if (!payload) {
            strcpy_s(s->reason, sizeof s->reason,
                     reason == SH_SHARD_B64_TOO_BIG ? "its payload is over the size budget" :
                     reason == SH_SHARD_B64_NOMEM   ? "there was not enough memory to decode it" :
                                                      "its shards are not valid base64");
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

/* The scan itself, guarded, with the lock already held. */
static void nav_build_locked(const char *json, size_t len)
{
    __try {
        /* The cheap reject. Almost no map carries a bake, and one that does not
         * pays a single substring sweep -- but it must still get this far,
         * because clearing the table is the whole reason this runs every load. */
        if (json && len && sh_shard_find(json, len, SH_SMNAV_MAGIC, SH_SMNAV_MAGIC_LEN)) {
            nav_collect(json, len);
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
    /* Off means off, including the table: a map loaded while the feature is
     * disabled must not leave the previous map's navigation live. */
    if (on) nav_build_locked(json, len);
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

    if (sets == 0) return;   /* silence is correct for the common map */

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: this map carries %zu baked module/class set(s); serving %zu of them for "
                "%zu module(s), holding %zu bytes", sets, served, modules, held);
    backend_log(line);
}

/* ==================================================================== */
/* strip                                                                 */
/* ==================================================================== */

static char *nav_strip_inner(const char *json, size_t len, size_t *out_len)
{
    unsigned elements = 0, runs = 0;
    int doc_failed = 0;
    size_t w = 0;
    char *out;
    char line[224];

    __try {
        out = sh_shard_strip(json, len, SH_SMNAV_MAGIC, SH_SMNAV_MAGIC_LEN,
                             nav_shard_filter, NULL, SH_SMNAV_MAX_SHARDS,
                             &w, &elements, &runs, &doc_failed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nav_fault("the shard strip");
        return NULL;
    }
    if (!out) {
        if (doc_failed)
            backend_log("NAV: shard strip SKIPPED -- the map's JSON structure did not read "
                        "cleanly; handing the engine the original buffer");
        return NULL;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: navigation shards STRIPPED -- %u variable(s) removed in %u run(s), "
                "%zu -> %zu bytes; the delivery envelope never becomes map state",
                elements, runs, len, w);
    backend_log(line);
    if (out_len) *out_len = w;
    return out;
}

char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!json || len == 0) return NULL;
    /* Disabled means the shards stay where an older client leaves them: inert
     * map variables, which is exactly how a map baked by a newer release
     * behaves on a client that does not know about it. */
    if (!sh_navmesh_enabled()) return NULL;
    return nav_strip_inner(json, len, out_len);
}

/* ==================================================================== */
/* embed                                                                 */
/* ==================================================================== */

/* Re-embed one held set, replacing its own shards and leaving every other
 * set's -- and every package's -- alone. */
static char *nav_embed_set(const char *json, size_t len, nav_set *s,
                           size_t *out_len, char *err, size_t err_cap)
{
    char *base = NULL, *b64 = NULL, *headers = NULL, *out = NULL;
    sh_shard_out *parts = NULL;
    size_t b64_len, shards, i, w = 0, base_len = 0;
    const char *src;
    size_t src_len;

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';

    base = sh_shard_strip(json, len, SH_SMNAV_MAGIC, SH_SMNAV_MAGIC_LEN,
                          nav_shard_filter, s, SH_SMNAV_MAX_SHARDS,
                          &base_len, NULL, NULL, NULL);
    src = base ? base : json;
    src_len = base ? base_len : len;

    b64_len = ((s->payload_len + 2) / 3) * 4;
    b64 = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64) {
        strncpy_s(err, err_cap, "there was no memory to encode the payload", _TRUNCATE);
        goto done;
    }
    b64_len = sh_shard_b64_encode(s->payload, s->payload_len, b64);

    shards = (b64_len + SH_SHARD_CHARS - 1) / SH_SHARD_CHARS;
    if (shards == 0) shards = 1;
    if (shards > SH_SMNAV_MAX_SHARDS) {
        strncpy_s(err, err_cap, "the payload needs more shards than the format allows", _TRUNCATE);
        goto done;
    }

    parts = (sh_shard_out *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, shards * sizeof *parts);
    headers = (char *)HeapAlloc(GetProcessHeap(), 0, shards * SH_SMNAV_HEADER_CAP);
    if (!parts || !headers) {
        strncpy_s(err, err_cap, "there was no memory to build the map", _TRUNCATE);
        goto done;
    }
    for (i = 0; i < shards; i++) {
        char *h = headers + i * SH_SMNAV_HEADER_CAP;
        size_t chunk_len = b64_len - i * SH_SHARD_CHARS;
        if (chunk_len > SH_SHARD_CHARS) chunk_len = SH_SHARD_CHARS;
        if (_snprintf_s(h, SH_SMNAV_HEADER_CAP, _TRUNCATE, "%s%s.%s.%u.%u.%s.%s",
                        SH_SMNAV_MAGIC, s->module, s->cls, (unsigned)i, (unsigned)shards,
                        s->digest, s->geom) < 0) {
            strncpy_s(err, err_cap, "its header does not fit the wire format", _TRUNCATE);
            goto done;
        }
        parts[i].header = h;
        parts[i].chunk = b64 + i * SH_SHARD_CHARS;
        parts[i].chunk_len = chunk_len;
    }

    out = sh_shard_insert(src, src_len, parts, shards, &w, err, err_cap);
    if (out && out_len) *out_len = w;

done:
    if (parts)   HeapFree(GetProcessHeap(), 0, parts);
    if (headers) HeapFree(GetProcessHeap(), 0, headers);
    if (b64)     HeapFree(GetProcessHeap(), 0, b64);
    if (base)    HeapFree(GetProcessHeap(), 0, base);
    return out;
}

/* Guarded, with the lock already held. */
static char *nav_embed_locked(const char *json, size_t len,
                              size_t *cur_len_out, size_t *embedded_out)
{
    char *cur = NULL;
    size_t cur_len = len, i, embedded = 0;

    __try {
        for (i = 0; i < g_set_count; i++) {
            nav_set *s = &g_sets[i];
            char err[SH_SMNAV_REASON_CAP];
            size_t next_len = 0;
            char *next;
            if (!s->payload) continue;
            next = nav_embed_set(cur ? cur : json, cur_len, s, &next_len, err, sizeof err);
            if (!next) {
                char warn[SH_SMNAV_REASON_CAP + 160];
                _snprintf_s(warn, sizeof warn, _TRUNCATE,
                            "NAV: '%s' (%s) could NOT be written back into this save (%s); the "
                            "map is being saved WITHOUT it", s->module, s->cls, err);
                backend_log(warn);
                continue;
            }
            if (cur) HeapFree(GetProcessHeap(), 0, cur);
            cur = next;
            cur_len = next_len;
            embedded++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nav_fault("the save-time re-embed");
        if (cur) HeapFree(GetProcessHeap(), 0, cur);
        cur = NULL;
        embedded = 0;
    }
    *cur_len_out = cur_len;
    *embedded_out = embedded;
    return cur;
}

char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len)
{
    char *cur;
    size_t cur_len = len, embedded = 0;
    char line[224];

    if (out_len) *out_len = 0;
    if (!json || len == 0) return NULL;
    if (!sh_navmesh_enabled()) return NULL;
    if (!nav_config_on(NAV_CONFIG_EMBED)) return NULL;
    if (nav_held_count() == 0) return NULL;

    AcquireSRWLockExclusive(&g_nav_lock);
    cur = nav_embed_locked(json, len, &cur_len, &embedded);
    ReleaseSRWLockExclusive(&g_nav_lock);

    if (!cur) return NULL;
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: the saved map CARRIES its baked navigation again -- %zu module/class "
                "set(s), %zu -> %zu bytes", embedded, len, cur_len);
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
            /* A COPY, always. The table is rebuilt on the next map load and
             * this buffer may still be being read by the engine; aliasing table
             * memory into a stream is a use-after-free waiting for a fast
             * loader. */
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

    /* The map-load line goes first and is printed unconditionally: "did the
     * map load actually reach this code, and what did it find" is a different
     * question from "what is in the table now", and it is the one a session
     * that can only load a single map is still able to answer. */
    out("navigation: %lu map load(s) seen this session; the last found %zu baked set(s).\n",
        (unsigned long)InterlockedCompareExchange(&g_build_count, 0, 0), g_last_build_sets);

    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) {
        out("navigation: DISABLED for this session after a fault; the engine serves its own.\n");
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
            if (s->served) {
                out("  [served] %s (%zu bytes)\n", s->res_name, s->payload_len);
                out("  [served] %s\n", s->cook_name);
            } else {
                out("  [held]   %s %s -- %s\n", s->module, s->cls,
                    s->reason[0] ? s->reason : "its module was refused");
            }
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
        ? "NAV: baked-navigation serving ARMED -- a map carrying validated smnav1 shards has its "
          "modules' navmeshes replaced for as long as it is loaded"
        : "NAV: baked-navigation serving OFF by config; maps keep their shards and the engine "
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
