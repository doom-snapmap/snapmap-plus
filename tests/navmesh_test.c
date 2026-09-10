/* Tests smnav1 reassembly, structural AAS validation and map-scoped serving.
 * All AAS2 3.29 fixtures are synthetic; accept the valid payload and refuse
 * each malformed mutation without a crash. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "navmesh.h"
#include "map_shards.h"

static int g_failed;

#define CHECK(expr) do {                                                         \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ---- the pieces navmesh.c links against ------------------------------- */

static char g_log[64][512];
static int  g_log_count;

void backend_log(const char *message)
{
    if (g_log_count < (int)(sizeof g_log / sizeof g_log[0]))
        strncpy_s(g_log[g_log_count++], sizeof g_log[0], message ? message : "", _TRUNCATE);
}

static void log_reset(void) { g_log_count = 0; }

/* sh_navmesh's console sink, captured. */
static char g_report[8192];

static void report_sink(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    strncat_s(g_report, sizeof g_report, line, _TRUNCATE);
}

static void report_capture(void)
{
    g_report[0] = '\0';
    sh_navmesh_report(report_sink);
}

static int log_contains(const char *needle)
{
    int i;
    for (i = 0; i < g_log_count; i++)
        if (strstr(g_log[i], needle)) return 1;
    return 0;
}

/* The config service is not linked in: the keys read as their registered
 * defaults, which is what a fresh install answers. */
int sh_config_get_bool(const char *key, int *out_value, unsigned int *out_flags)
{
    (void)key;
    if (out_flags) *out_flags = 0;
    if (!out_value) return 0;
    *out_value = 1;
    return 1;
}

/* ==================================================================== */
/* a synthetic AAS2 3.29 file                                            */
/* ==================================================================== */

/* Lump order and record sizes are the file format; this mirrors them so the
 * test would notice if navmesh.c's table ever drifted. */
static const unsigned AAS_REC[22] = {
    16, 12, 12, 4, 40, 44, 16, 12, 4, 16, 1, 132, 128, 128, 128, 56, 4, 4, 60, 24, 24, 12
};

enum {
    A_PLANES = 0, A_VERTICES, A_EDGES, A_EDGEINDEX, A_REACH, A_AREAS, A_NODES,
    A_PORTALS, A_PORTALINDEX, A_CLUSTERS, A_OBSTACLEPVS, A_REACHNAMES,
    A_ANIMNAMES, A_DEPNAMES, A_INTERACTNAMES, A_COVER, A_AREACOVERINDEX,
    A_TOUCHCOVERINDEX, A_TRAVPOINTS, A_HINTNODES, A_TREES, A_AREABOUNDS
};

typedef struct aas_build {
    unsigned char *p;
    size_t         len;
    size_t         off[22];      /* first record of each lump */
    unsigned       count[22];
} aas_build;

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}

typedef enum { NODES_TREE, NODES_CYCLE, NODES_CHAIN } node_mode;

/* Synthetic file with dummy/floor areas, four vertices and edges, one linked
 * reachability, cluster and cover record, and a small BSP. Dependency-name
 * padding crosses shard boundaries without adding geometry. */
static int build_aas(aas_build *b, unsigned pad, node_mode mode, unsigned chain)
{
    unsigned counts[22];
    unsigned num_nodes = (mode == NODES_TREE) ? 3 : (mode == NODES_CYCLE ? 4 : chain);
    size_t total = 394, off;
    unsigned i;

    memset(b, 0, sizeof *b);
    memset(counts, 0, sizeof counts);
    counts[A_PLANES] = 1;
    counts[A_VERTICES] = 4;
    counts[A_EDGES] = 4;
    counts[A_EDGEINDEX] = 4;
    counts[A_REACH] = 1;
    counts[A_AREAS] = 2;
    counts[A_NODES] = num_nodes;
    counts[A_PORTALS] = 1;
    counts[A_CLUSTERS] = 1;
    counts[A_OBSTACLEPVS] = 2;
    counts[A_DEPNAMES] = pad;
    counts[A_COVER] = 1;
    counts[A_AREACOVERINDEX] = 1;
    counts[A_TREES] = 1;
    counts[A_AREABOUNDS] = 2;

    for (i = 0; i < 22; i++) total += 4 + (size_t)counts[i] * AAS_REC[i];
    b->p = (unsigned char *)calloc(1, total);
    if (!b->p) return 0;
    b->len = total;

    memcpy(b->p, "2SAA", 4);
    b->p[4] = 3;
    b->p[5] = 29;

    off = 394;
    for (i = 0; i < 22; i++) {
        put32(b->p + off, counts[i]);
        off += 4;
        b->off[i] = off;
        b->count[i] = counts[i];
        off += (size_t)counts[i] * AAS_REC[i];
    }

    /* edges reference vertices 0..3 */
    for (i = 0; i < 4; i++) {
        unsigned char *e = b->p + b->off[A_EDGES] + i * 12;
        put32(e, i);
        put32(e + 4, (i + 1) % 4);
    }
    /* edgeIndex references edges 0..3 (the sign is a direction, not an index) */
    for (i = 0; i < 4; i++) put32(b->p + b->off[A_EDGEINDEX] + i * 4, i);
    /* areaCoverIndex references cover 0 */
    put32(b->p + b->off[A_AREACOVERINDEX], 0);

    /* area 0: the dummy. Owns nothing, heads no reachability list. */
    {
        unsigned char *a0 = b->p + b->off[A_AREAS];
        put32(a0 + 0x14, 0xFFFFFFFFu);
        put32(a0 + 0x18, 0xFFFFFFFFu);
    }
    /* area 1: owns every edge and the one cover entry, and heads both lists. */
    {
        unsigned char *a1 = b->p + b->off[A_AREAS] + 44;
        put16(a1 + 0x06, 4);        /* numEdges */
        put32(a1 + 0x08, 0);        /* firstEdgeIndex */
        put16(a1 + 0x0C, 0);        /* cluster */
        put32(a1 + 0x10, 0);        /* firstObstaclePVS */
        put32(a1 + 0x14, 0);        /* firstReachabilityFrom -> reach 0 */
        put32(a1 + 0x18, 0);        /* firstReachabilityTo   -> reach 0 */
        put16(a1 + 0x20, 0);        /* firstAreaCoverIndex */
        put16(a1 + 0x22, 1);        /* numAreaCover */
    }
    /* the one reachability: area 1 to itself, ending both lists */
    {
        unsigned char *r = b->p + b->off[A_REACH];
        put16(r + 0x06, 1);
        put16(r + 0x08, 1);
        put32(r + 0x20, 0xFFFFFFFFu);
        put32(r + 0x24, 0xFFFFFFFFu);
    }

    /* the BSP */
    if (mode == NODES_TREE) {
        unsigned char *n = b->p + b->off[A_NODES];
        put32(n + 8, 1); put32(n + 12, 2);              /* node 0: two children */
        put32(n + 16 + 8, 0xFFFFFFFFu);                 /* node 1: leaf -> area 1 */
        put32(n + 32 + 8, 0xFFFFFFFFu);                 /* node 2: leaf -> area 1 */
    } else if (mode == NODES_CYCLE) {
        /* node 0 is an isolated root; 1 -> 2 -> 3 -> 1 is a closed loop in
         * which every node still has exactly one parent. */
        unsigned char *n = b->p + b->off[A_NODES];
        put32(n + 16 + 8, 2);
        put32(n + 32 + 8, 3);
        put32(n + 48 + 8, 1);
    } else {
        unsigned char *n = b->p + b->off[A_NODES];
        for (i = 0; i + 1 < num_nodes; i++) put32(n + i * 16 + 8, i + 1);
    }
    return 1;
}

static void free_aas(aas_build *b) { free(b->p); b->p = NULL; }

/* ==================================================================== */
/* synthetic maps                                                        */
/* ==================================================================== */

#define BASE_MODULE "ind_dlc/ind_totally_blank_room_4x"

static const char *base_map(const char *module)
{
    static char map[1024];
    _snprintf_s(map, sizeof map, _TRUNCATE,
        "{\"instances\":[{\"moduleName\":\"maps/modules/%s.decl\",\"origin\":{\"x\":0.0},"
        "\"~type\":\"idSnapInstance\"}],"
        "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,16],\"boolean\":[],\"string\":[],"
        "\"~type\":\"idSnapVariables\"},\"~type\":\"idSnapMap\",\"~version\":26}", module);
    return map;
}

static char *insert_shards(const char *json, const sh_shard_out *parts, size_t count,
                           size_t *out_len)
{
    char err[256];
    char *out = sh_shard_insert(json, strlen(json), parts, count, out_len, err, sizeof err);
    if (!out) fprintf(stderr, "insert_shards failed: %s\n", err);
    return out;
}

/* Cut `payload` into wire shards and splice them into `json`. `bad` selects one
 * of the delivery faults a hostile or damaged map can present. */
typedef enum {
    MAP_GOOD = 0,
    MAP_BAD_DIGEST,
    MAP_MISSING_SHARD,
    MAP_DUPLICATE_SHARD,
    MAP_OVERSIZE_CHUNK
} map_fault;

static char *make_map(const char *json, const char *module, const char *cls,
                      const unsigned char *payload, size_t payload_len,
                      map_fault fault, size_t *out_len)
{
    char digest[SH_SHARD_DIGEST_CHARS + 1];
    char *b64;
    size_t b64_len, shards, i;
    sh_shard_out *parts;
    char *headers, *out;
    char *fat = NULL;

    b64 = (char *)malloc(((payload_len + 2) / 3) * 4 + 1);
    if (!b64) return NULL;
    b64_len = sh_shard_b64_encode(payload, payload_len, b64);
    sh_shard_digest16(payload, payload_len, digest);
    if (fault == MAP_BAD_DIGEST) digest[0] = (digest[0] == 'a') ? 'b' : 'a';

    shards = (b64_len + SH_SHARD_CHARS - 1) / SH_SHARD_CHARS;
    if (shards == 0) shards = 1;
    if (fault == MAP_MISSING_SHARD && shards < 2) { free(b64); return NULL; }

    parts = (sh_shard_out *)calloc(shards + 1, sizeof *parts);
    headers = (char *)calloc(shards + 1, SH_SMNAV_HEADER_CAP);
    if (!parts || !headers) { free(b64); free(parts); free(headers); return NULL; }

    for (i = 0; i < shards; i++) {
        char *h = headers + i * SH_SMNAV_HEADER_CAP;
        size_t chunk_len = b64_len - i * SH_SHARD_CHARS;
        if (chunk_len > SH_SHARD_CHARS) chunk_len = SH_SHARD_CHARS;
        _snprintf_s(h, SH_SMNAV_HEADER_CAP, _TRUNCATE, "smnav1.%s.%s.%u.%u.%s.%s",
                    module, cls, (unsigned)i, (unsigned)shards, digest,
                    "fedcba9876543210");
        parts[i].header = h;
        parts[i].chunk = b64 + i * SH_SHARD_CHARS;
        parts[i].chunk_len = chunk_len;
    }
    if (fault == MAP_MISSING_SHARD) {
        shards--;                                  /* drop the last one, keep `total` */
    } else if (fault == MAP_DUPLICATE_SHARD) {
        memcpy(headers + shards * SH_SMNAV_HEADER_CAP, headers, SH_SMNAV_HEADER_CAP);
        parts[shards].header = headers + shards * SH_SMNAV_HEADER_CAP;
        parts[shards].chunk = parts[0].chunk;
        parts[shards].chunk_len = parts[0].chunk_len;
        shards++;
    } else if (fault == MAP_OVERSIZE_CHUNK) {
        size_t fat_len = SH_SHARD_MAX_CHUNK + 8;
        fat = (char *)malloc(fat_len + 1);
        if (!fat) { free(b64); free(parts); free(headers); return NULL; }
        memset(fat, 'A', fat_len);
        fat[fat_len] = '\0';
        parts[0].chunk = fat;
        parts[0].chunk_len = fat_len;
        shards = 1;
        _snprintf_s(headers, SH_SMNAV_HEADER_CAP, _TRUNCATE, "smnav1.%s.%s.0.1.%s.%s",
                    module, cls, digest, "fedcba9876543210");
    }

    out = insert_shards(json, parts, shards, out_len);
    free(b64);
    free(fat);
    free(parts);
    free(headers);
    return out;
}

/* ==================================================================== */
/* the validator                                                         */
/* ==================================================================== */

static void test_validator(void)
{
    aas_build b;
    char err[256];

    /* the good payload is ACCEPTED -- the rule every other case rests on */
    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 1);
    if (err[0]) fprintf(stderr, "good payload refused: %s\n", err);

    /* magic and version are exact */
    b.p[0] = 'X';
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "magic") != NULL);
    b.p[0] = '2';
    b.p[5] = 28;
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "version") != NULL);
    b.p[5] = 29;
    b.p[4] = 4;
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    b.p[4] = 3;
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 1);

    /* truncation, at every scale */
    CHECK(sh_navmesh_validate_aas(b.p, b.len - 1, err, sizeof err) == 0);
    CHECK(sh_navmesh_validate_aas(b.p, 40, err, sizeof err) == 0);
    CHECK(sh_navmesh_validate_aas(b.p, 0, err, sizeof err) == 0);
    CHECK(sh_navmesh_validate_aas(NULL, 100, err, sizeof err) == 0);

    /* trailing bytes: the walk must land on exactly the last byte */
    {
        unsigned char *big = (unsigned char *)malloc(b.len + 4);
        memcpy(big, b.p, b.len);
        memset(big + b.len, 0, 4);
        CHECK(sh_navmesh_validate_aas(big, b.len + 4, err, sizeof err) == 0);
        CHECK(strstr(err, "trailing") != NULL);
        free(big);
    }

    /* a lump table that overruns the payload */
    {
        put32(b.p + b.off[A_VERTICES] - 4, 0x4000);
        CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
        CHECK(strstr(err, "overrun") != NULL);
        put32(b.p + b.off[A_VERTICES] - 4, 4);
        CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 1);
    }

    /* a count nothing could hold */
    {
        put32(b.p + b.off[A_VERTICES] - 4, 0xFFFFFFFFu);
        CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
        put32(b.p + b.off[A_VERTICES] - 4, 4);
    }

    /* every cross-lump index the loader dereferences */
    put32(b.p + b.off[A_EDGES], 9);                       /* edge -> vertex */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "vertex") != NULL);
    put32(b.p + b.off[A_EDGES], 0);

    put32(b.p + b.off[A_EDGEINDEX], 0xFFFFFFF0u);         /* edgeIndex -> edges, signed */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "edgeIndex") != NULL);
    put32(b.p + b.off[A_EDGEINDEX], 0);

    put16(b.p + b.off[A_AREAS] + 44 + 0x06, 99);          /* area -> edgeIndex span */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "edgeIndex") != NULL);
    put16(b.p + b.off[A_AREAS] + 44 + 0x06, 4);

    put16(b.p + b.off[A_AREAS] + 44 + 0x0C, 7);           /* area -> cluster */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "cluster") != NULL);
    put16(b.p + b.off[A_AREAS] + 44 + 0x0C, 0);

    put16(b.p + b.off[A_AREAS] + 44 + 0x22, 9);           /* area -> areaCoverIndex span */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    put16(b.p + b.off[A_AREAS] + 44 + 0x22, 1);

    put16(b.p + b.off[A_REACH] + 0x06, 5);                /* reach -> area */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "reachability") != NULL);
    put16(b.p + b.off[A_REACH] + 0x06, 1);

    put32(b.p + b.off[A_NODES], 3);                       /* node -> plane */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "plane") != NULL);
    put32(b.p + b.off[A_NODES], 0);

    put32(b.p + b.off[A_NODES] + 8, 99);                  /* node -> child */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "out of range") != NULL);
    put32(b.p + b.off[A_NODES] + 8, 0xFFFFFF00u);         /* node -> area leaf */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "area") != NULL);
    put32(b.p + b.off[A_NODES] + 8, 1);

    /* Remove the final areaBounds record while preserving the file end to
     * isolate its required one-record-per-area count. */
    put32(b.p + b.off[A_AREABOUNDS] - 4, 1);
    CHECK(sh_navmesh_validate_aas(b.p, b.len - 12, err, sizeof err) == 0);
    CHECK(strstr(err, "areaBounds") != NULL);
    put32(b.p + b.off[A_AREABOUNDS] - 4, 2);

    /* a reachability list that loops back on itself is a hang, not a refusal
     * the engine would ever report */
    put32(b.p + b.off[A_REACH] + 0x20, 0);
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "cycle") != NULL);
    put32(b.p + b.off[A_REACH] + 0x20, 0xFFFFFFFFu);
    /* ...and one that leaves the array */
    put32(b.p + b.off[A_AREAS] + 44 + 0x14, 9);
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    put32(b.p + b.off[A_AREAS] + 44 + 0x14, 0);
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 1);
    free_aas(&b);

    /* a cyclic BSP: every node still has exactly one parent, so only
     * reachability catches it */
    CHECK(build_aas(&b, 0, NODES_CYCLE, 0));
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "cycle") != NULL);
    free_aas(&b);

    /* the depth limit, from both sides */
    CHECK(build_aas(&b, 0, NODES_CHAIN, 0x80));
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 1);
    free_aas(&b);
    CHECK(build_aas(&b, 0, NODES_CHAIN, 0x81));
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "deeper") != NULL);
    free_aas(&b);

    /* a shared child is not a tree */
    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    put32(b.p + b.off[A_NODES] + 12, 1);       /* node 0's two children are both node 1 */
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err) == 0);
    CHECK(strstr(err, "parent") != NULL);
    free_aas(&b);
}

/* ==================================================================== */
/* the header grammar and the serving table                              */
/* ==================================================================== */

/* Keep source and cooked resource names as independent literals. Cooking
 * prefixes the whole extension with b (.baas_monster48); a wrong name silently
 * falls through to shipped navigation. */
/* The load count and current set count expose whether a new load cleared
 * the previous map's navigation. */
static void test_report(void)
{
    aas_build b;
    char *map;
    size_t map_len = 0;

    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    sh_navmesh_test_reset();
    report_capture();
    CHECK(strstr(g_report, "0 map load(s) seen") != NULL);
    CHECK(strstr(g_report, "carries no bake") != NULL);

    map = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        sh_navmesh_build_from_map(map, map_len);
        report_capture();
        CHECK(strstr(g_report, "1 map load(s) seen") != NULL);
        CHECK(strstr(g_report, "the last found 1 baked set(s)") != NULL);
        CHECK(strstr(g_report, "ind_totally_blank_room_4x.baas_monster48") != NULL);

        /* the same session, one more load, nothing baked: the counter advances
         * and the table is empty -- the stale-clear rule, said out loud */
        sh_navmesh_build_from_map(base_map(BASE_MODULE), strlen(base_map(BASE_MODULE)));
        report_capture();
        CHECK(strstr(g_report, "2 map load(s) seen") != NULL);
        CHECK(strstr(g_report, "the last found 0 baked set(s)") != NULL);
        CHECK(strstr(g_report, "carries no bake") != NULL);
        free(map);
    }
    free_aas(&b);
}

static void test_resource_names(void)
{
    aas_build b;
    char *map;
    size_t map_len = 0;

    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    sh_navmesh_test_reset();
    map = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        sh_navmesh_build_from_map(map, map_len);
        CHECK(sh_navmesh_test_served_count() == 1);
        CHECK(strcmp(sh_navmesh_test_served_name(0),
                     "maps/modules/ind_dlc/ind_totally_blank_room_4x/"
                     "ind_totally_blank_room_4x.aas_monster48") == 0);
        CHECK(strcmp(sh_navmesh_test_served_name(1),
                     "generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/"
                     "ind_totally_blank_room_4x.baas_monster48") == 0);

        /* ...and both of them answer */
        CHECK(sh_navmesh_open("maps/modules/ind_dlc/ind_totally_blank_room_4x/"
                              "ind_totally_blank_room_4x.aas_monster48",
                              &bytes, &len) == 1);
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
        bytes = NULL; len = 0;
        CHECK(sh_navmesh_open("generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/"
                              "ind_totally_blank_room_4x.baas_monster48",
                              &bytes, &len) == 1);
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);

        /* the spelling this shipped with, which the engine never asks for */
        bytes = NULL; len = 0;
        CHECK(sh_navmesh_open("generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/"
                              "ind_totally_blank_room_4x.bmonster48",
                              &bytes, &len) == 0);
        free(map);
    }
    free_aas(&b);
}

static void test_reader(void)
{
    aas_build b;
    char *map;
    size_t map_len = 0;

    /* A module name with more than one '/': the header carries
     * `category/module`, and the resource name is built from its last segment,
     * so the parse must take five fields FROM THE RIGHT and leave the rest
     * alone. */
    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    sh_navmesh_test_reset();
    log_reset();
    map = make_map(base_map("ind_dlc/deep/room_4x"), "ind_dlc/deep/room_4x", "monster48",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        sh_navmesh_build_from_map(map, map_len);
        CHECK(sh_navmesh_test_served_count() == 1);
        CHECK(strcmp(sh_navmesh_test_served_name(0),
                     "maps/modules/ind_dlc/deep/room_4x/room_4x.aas_monster48") == 0);
        CHECK(strcmp(sh_navmesh_test_served_name(1),
                     "generated/maps/modules/ind_dlc/deep/room_4x/room_4x.baas_monster48") == 0);
        free(map);
    }

    /* Serving: both names, the exact bytes, and tolerant of how the engine
     * spells a path. */
    sh_navmesh_test_reset();
    map = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster128",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        sh_navmesh_build_from_map(map, map_len);
        CHECK(sh_navmesh_test_served_count() == 1);
        CHECK(sh_navmesh_open(
                  "maps/modules/" BASE_MODULE "/ind_totally_blank_room_4x.aas_monster128",
                  &bytes, &len) == 1);
        CHECK(len == b.len);
        if (bytes && len == b.len) CHECK(memcmp(bytes, b.p, len) == 0);
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);

        bytes = NULL; len = 0;
        CHECK(sh_navmesh_open(
                  "GENERATED\\maps\\modules\\" BASE_MODULE
                  "\\ind_totally_blank_room_4x.baas_monster128", &bytes, &len) == 1);
        CHECK(len == b.len);
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);

        bytes = NULL; len = 0;
        CHECK(sh_navmesh_open("maps/modules/other/other/other.aas_monster128",
                              &bytes, &len) == 0);
        CHECK(bytes == NULL);

        /* THE STALE-CLEAR RULE. A map with no bake, loaded next, must not
         * inherit the previous map's navigation. */
        sh_navmesh_build_from_map(base_map(BASE_MODULE), strlen(base_map(BASE_MODULE)));
        CHECK(sh_navmesh_test_served_count() == 0);
        CHECK(sh_navmesh_test_held_count() == 0);
        CHECK(sh_navmesh_open(
                  "maps/modules/" BASE_MODULE "/ind_totally_blank_room_4x.aas_monster128",
                  &bytes, &len) == 0);

        /* strip: the envelope never becomes map state */
        {
            size_t stripped_len = 0;
            char *stripped;
            sh_navmesh_build_from_map(map, map_len);
            stripped = sh_navmesh_strip(map, map_len, &stripped_len);
            CHECK(stripped != NULL);
            if (stripped) {
                CHECK(strstr(stripped, "smnav1.") == NULL);
                CHECK(stripped_len < map_len);
                CHECK(strlen(stripped) == stripped_len);

                /* ...and a save puts it back, byte for byte */
                {
                    size_t back_len = 0;
                    char *back = sh_navmesh_embed_all(stripped, stripped_len, &back_len);
                    CHECK(back != NULL);
                    if (back) {
                        unsigned char *again = NULL;
                        size_t again_len = 0;
                        sh_navmesh_build_from_map(back, back_len);
                        CHECK(sh_navmesh_test_served_count() == 1);
                        CHECK(sh_navmesh_open(
                                  "maps/modules/" BASE_MODULE
                                  "/ind_totally_blank_room_4x.aas_monster128",
                                  &again, &again_len) == 1);
                        CHECK(again_len == b.len);
                        if (again && again_len == b.len)
                            CHECK(memcmp(again, b.p, again_len) == 0);
                        if (again) HeapFree(GetProcessHeap(), 0, again);
                        HeapFree(GetProcessHeap(), 0, back);
                    }
                }
                HeapFree(GetProcessHeap(), 0, stripped);
            }
        }
        free(map);
    }
    free_aas(&b);

    /* Reassembly across a shard boundary. The padding pushes the payload past
     * the 8192-character shard cap, so this exercises ordering as well as
     * decoding. */
    CHECK(build_aas(&b, 60, NODES_TREE, 0));
    CHECK(b.len > 6144);
    sh_navmesh_test_reset();
    map = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        sh_navmesh_build_from_map(map, map_len);
        CHECK(sh_navmesh_test_served_count() == 1);
        CHECK(sh_navmesh_open("maps/modules/" BASE_MODULE
                              "/ind_totally_blank_room_4x.aas_monster48", &bytes, &len) == 1);
        CHECK(len == b.len);
        if (bytes && len == b.len) CHECK(memcmp(bytes, b.p, len) == 0);
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
        free(map);
    }

    /* every delivery fault: refused, held nothing, and said so */
    {
        struct { map_fault fault; const char *needle; } cases[] = {
            { MAP_BAD_DIGEST,      "digest" },
            { MAP_MISSING_SHARD,   "shards are present" },
            { MAP_DUPLICATE_SHARD, "twice" },
            { MAP_OVERSIZE_CHUNK,  "could not be read" }
        };
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            size_t len2 = 0;
            char *m = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                               b.p, b.len, cases[i].fault, &len2);
            CHECK(m != NULL);
            if (!m) continue;
            sh_navmesh_test_reset();
            log_reset();
            sh_navmesh_build_from_map(m, len2);
            CHECK(sh_navmesh_test_served_count() == 0);
            CHECK(sh_navmesh_test_held_count() == 0);
            CHECK(log_contains(cases[i].needle));
            /* the module's own refusal is the one that lands last, and it is
             * the one an author reads first */
            CHECK(strstr(sh_navmesh_test_last_refusal(), "serves NO navigation") != NULL);
            free(m);
        }
    }
    free_aas(&b);

    /* a payload that is not a walkable AAS is refused, and the module serves
     * nothing at all */
    CHECK(build_aas(&b, 0, NODES_CYCLE, 0));
    sh_navmesh_test_reset();
    log_reset();
    map = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                   b.p, b.len, MAP_GOOD, &map_len);
    CHECK(map != NULL);
    if (map) {
        sh_navmesh_build_from_map(map, map_len);
        CHECK(sh_navmesh_test_served_count() == 0);
        CHECK(sh_navmesh_test_held_count() == 1);        /* held, so a save keeps it */
        CHECK(log_contains("serves NO navigation"));
        free(map);
    }
    free_aas(&b);

    /* all-or-nothing per module: one bad class takes the good one with it */
    {
        aas_build good, bad;
        char *one, *two;
        size_t one_len = 0, two_len = 0;
        CHECK(build_aas(&good, 0, NODES_TREE, 0));
        CHECK(build_aas(&bad, 0, NODES_CYCLE, 0));
        one = make_map(base_map(BASE_MODULE), BASE_MODULE, "monster48",
                       good.p, good.len, MAP_GOOD, &one_len);
        CHECK(one != NULL);
        if (one) {
            two = make_map(one, BASE_MODULE, "monster128", bad.p, bad.len, MAP_GOOD, &two_len);
            CHECK(two != NULL);
            if (two) {
                sh_navmesh_test_reset();
                sh_navmesh_build_from_map(two, two_len);
                CHECK(sh_navmesh_test_served_count() == 0);
                CHECK(sh_navmesh_test_held_count() == 2);
                HeapFree(GetProcessHeap(), 0, two);
            }
            free(one);
        }
        free_aas(&good);
        free_aas(&bad);
    }

    /* a module the map places twice cannot be served: one resource name cannot
     * answer two instances differently */
    {
        char twice[1536];
        char *m;
        size_t len2 = 0;
        CHECK(build_aas(&b, 0, NODES_TREE, 0));
        _snprintf_s(twice, sizeof twice, _TRUNCATE,
            "{\"instances\":[{\"moduleName\":\"maps/modules/%s.decl\",\"~type\":\"idSnapInstance\"},"
            "{\"moduleName\":\"maps/modules/%s.decl\",\"~type\":\"idSnapInstance\"}],"
            "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,16],\"string\":[],"
            "\"~type\":\"idSnapVariables\"},\"~type\":\"idSnapMap\"}",
            BASE_MODULE, BASE_MODULE);
        m = make_map(twice, BASE_MODULE, "monster48", b.p, b.len, MAP_GOOD, &len2);
        CHECK(m != NULL);
        if (m) {
            sh_navmesh_test_reset();
            log_reset();
            sh_navmesh_build_from_map(m, len2);
            CHECK(sh_navmesh_test_served_count() == 0);
            CHECK(log_contains("places it 2 times"));
            free(m);
        }
        free_aas(&b);
    }
}

/* A map whose prose merely contains the magic is not perturbed: the scanner
 * only accepts a header that is the value of a "name" key inside a
 * snapVarInfo_t, and entity names are author-controlled free text. */
static void test_prose(void)
{
    static const char *padding =
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000";
    char map[4096];
    static const char *hdr =
        "smnav1.ind_dlc/ind_totally_blank_room_4x.monster48.0.1."
        "0123456789abcdef.fedcba9876543210";

    _snprintf_s(map, sizeof map, _TRUNCATE,
        "{\"instances\":[{\"moduleName\":\"maps/modules/%s.decl\",\"~type\":\"idSnapInstance\"}],"
        "\"entities\":[{\"displayName\":\"%s\",\"note\":\"%s\",\"~type\":\"idSnapEntity\"},"
        "{\"name\":\"%s\",\"pad\":\"%s\",\"~type\":\"idSnapEntity\"}],"
        "\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,16],\"string\":[],"
        "\"~type\":\"idSnapVariables\"},\"~type\":\"idSnapMap\"}",
        BASE_MODULE, hdr, padding, hdr, padding);

    sh_navmesh_test_reset();
    log_reset();
    sh_navmesh_build_from_map(map, strlen(map));
    CHECK(sh_navmesh_test_served_count() == 0);
    CHECK(sh_navmesh_test_held_count() == 0);
    CHECK(g_log_count == 0);                       /* not even a refusal: no shard was seen */
    CHECK(sh_navmesh_strip(map, strlen(map), NULL) == NULL);   /* and nothing is removed */
}

static void test_native_reachability_limit(void)
{
    aas_build b;
    unsigned i;
    size_t extra = 256 * 40, old_end;
    unsigned char *r, *area;
    char err[256];
    CHECK(build_aas(&b, 0, NODES_TREE, 0));
    old_end = b.off[A_REACH] + 40;
    b.p = (unsigned char *)realloc(b.p, b.len + extra);
    CHECK(b.p != NULL);
    if (!b.p) return;
    memmove(b.p + old_end + extra, b.p + old_end, b.len - old_end);
    memset(b.p + old_end, 0, extra);
    b.len += extra;
    for (i = A_REACH + 1; i < 22; i++) b.off[i] += extra;
    put32(b.p + b.off[A_REACH] - 4, 257);
    for (i = 0; i < 257; i++) {
        r = b.p + b.off[A_REACH] + i * 40;
        put16(r + 6, 1); put16(r + 8, 1);
        put32(r + 0x20, i == 256 ? 0xffffffffu : i + 1);
        put32(r + 0x24, i == 256 ? 0xffffffffu : i + 1);
    }
    CHECK(!sh_navmesh_validate_aas(b.p, b.len, err, sizeof err));
    CHECK(strstr(err, "256 outgoing reachabilities") != NULL);
    put32(b.p + b.off[A_AREAS] + 44 + 0x14, 0xffffffffu);
    CHECK(!sh_navmesh_validate_aas(b.p, b.len, err, sizeof err));
    CHECK(strstr(err, "256 outgoing reachabilities") != NULL);
    put32(b.p + b.off[A_AREAS] + 44 + 0x14, 0);
    /* 256 outgoing and 257 incoming is legal: only the outgoing ordinal is
     * packed into eight bits. The extra route originates in area 0. */
    r = b.p + b.off[A_REACH] + 255 * 40;
    put32(r + 0x20, 0xffffffffu);
    r += 40;
    put16(r + 6, 0);
    area = b.p + b.off[A_AREAS];
    put32(area + 0x14, 256);
    CHECK(sh_navmesh_validate_aas(b.p, b.len, err, sizeof err));
    free_aas(&b);
}

int main(void)
{
    test_validator();
    test_native_reachability_limit();
    test_resource_names();
    test_report();
    test_reader();
    test_prose();

    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("navmesh_test: all checks passed\n");
    return 0;
}
