/* Reads and serves navigation baked into a map; live editor baking is nav_bake's. The data is the "nav" part of
 * the map's "snapmap-plus" member (map_carrier.h): one {"module","class","digest","geom","data":[base64 chunks]}
 * object per (module, class), served under maps/modules/.../<module>.aas_<class> and the matching .baas_ name.
 *
 * The serving table is rebuilt on every map load, admitting all classes of a module together once verified.
 * Verified payloads are kept for save re-embedding even when serving is refused; with navmesh.enabled off the
 * member's part is written back unchanged as text. */
#ifndef SNAPMAP_PLUS_NAVMESH_H
#define SNAPMAP_PLUS_NAVMESH_H

#include "map_carrier.h"
#include <stddef.h>

/* The native router packs an outgoing reachability ordinal into eight bits. */
#define SH_AAS_MAX_AREA_REACHABILITIES 256u

/* Wire format. */

#define SH_SMNAV_DIGEST_CHARS   16          /* sha256 prefix over the payload */
#define SH_SMNAV_GEOM_CHARS     16          /* sha256 prefix over the source geometry */
#define SH_SMNAV_MODULE_CAP     128         /* "category/module" + NUL */
#define SH_SMNAV_CLASS_CAP      32          /* "monster128" + NUL */
#define SH_SMNAV_MAX_SHARDS     2048        /* parts per (module, class) */
#define SH_SMNAV_RESNAME_CAP    320
#define SH_SMNAV_REASON_CAP     192
/* Base64 characters per "data" chunk. Native deserialization fails on string variables of 16384 characters,
 * but a 1048576-character string in an engine-received member loads, plays and survives a save; a payload
 * within SH_SMNAV_MAX_PAYLOAD needs at most 11 chunks. */
#define SH_SMNAV_CARRIER_CHUNK  1048576u

/* Budgets. */

#define SH_SMNAV_MAX_PAYLOAD    (8u * 1024u * 1024u)    /* one baked payload */
#define SH_SMNAV_MAX_TOTAL      (32u * 1024u * 1024u)   /* all of them, per map */
#define SH_SMNAV_MAX_SETS       192                     /* (module, class) pairs per map */
#define SH_SMNAV_MAX_MODULES    64

/* The product's own bound on one AAS file the bake produces and serves, since LoadBinary allocates each lump's
 * count with no file-size check. Past it the bake sheds routes, then floors. */
#define SH_AAS_MAX_BAKE_PAYLOAD (16u * 1024u * 1024u)

/* The AAS2 3.29 binary shape the validator walks. */

#define SH_AAS_MAGIC            "2SAA"      /* 0x41415332, little-endian */
#define SH_AAS_MAJOR            3
#define SH_AAS_MINOR            29
#define SH_AAS_HEADER_BYTES     30
#define SH_AAS_SETTINGS_BYTES   364
#define SH_AAS_PREAMBLE_BYTES   (SH_AAS_HEADER_BYTES + SH_AAS_SETTINGS_BYTES)   /* 0x18A */
#define SH_AAS_LUMPS            22
#define SH_AAS_MAX_DEPTH        0x80        /* the engine's own tree-depth limit */
#define SH_AAS_MAX_AREAS        0xFFFEu     /* area indices are u16 */
#define SH_AAS_MAX_RECORDS      0x1000000u  /* per-lump record cap */
#define SH_AAS_MAX_TRAVERSAL_POINTS 0x10000u /* a first traversal point is a u16 */

/* Logs the configured state. Serving uses the override-provider hook; load/save handling uses the rawmap funnel. */
void sh_navmesh_install(void);

/* Called from the deserialize funnel on every buffer before the engine parses it. Clears the serving table and
 * rebuilds it from this map's saved parts; a fault inside disables the feature for the session. */
void sh_navmesh_build_from_map(const char *json, size_t len, const sh_carrier_span *nav);

/* Re-embeds every payload the current map holds, so a save with no fresh bake preserves its navigation. Returns
 * a new HeapAlloc'd buffer with *out_len set, or NULL to leave the save alone; *failed (may be NULL) is 1 when
 * the map held navigation that could not be written. */
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len, int *failed);

/* Called from the overrides open hook. Returns 1 when `name` is a resource this map's bake replaces, with
 * *out_bytes a HeapAlloc'd copy the caller owns (the table may be rebuilt while the engine reads a stream) and
 * *out_len its length; 0 for every other name. */
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len);

/* Is the feature live (configured on, and not disabled by a fault)? */
int sh_navmesh_enabled(void);

/* Print what is being served, for which module and classes, or why nothing is.
 * `out` is a printf-like sink (the console's sh_printf in production). */
void sh_navmesh_report(void (*out)(const char *fmt, ...));

/* Validates AAS2 3.29 layout, bounds, cross-indices and BSP/reachability walks. Returns 1 on success, or 0 with
 * err; it checks structure, not navigation quality. */
int sh_navmesh_validate_aas(const unsigned char *payload, size_t len,
                            char *err, size_t err_cap);

#ifdef SH_NAVMESH_TESTING
void        sh_navmesh_test_reset(void);
int         sh_navmesh_test_served_count(void);
const char *sh_navmesh_test_served_name(int index);
int         sh_navmesh_test_held_count(void);       /* payloads retained for re-embed */
const char *sh_navmesh_test_last_refusal(void);
#endif

#endif /* SNAPMAP_PLUS_NAVMESH_H */
