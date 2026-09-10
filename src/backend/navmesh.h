/* Read and serve the smnav1 embedded-navigation format. Live editor baking is
 * handled separately by nav_bake.
 *
 * Header:
 * smnav1.<category>/<module>.<class>.<idx>.<total>.<digest16>.<geom16>. Parse
 * the five trailing fields from the right. Resource aliases use
 * maps/modules/.../<module>.aas_<class> and
 * generated/maps/modules/.../<module>.baas_<class>.
 *
 * Rebuild the serving table from empty on every map load. Verify delivery and
 * AAS structure, then admit all classes together per module; repeated module
 * placements cannot use this shared-name format. Retain verified payloads for
 * save re-embedding even if serving is refused. Table storage and served
 * copies use the process heap.
 */
#ifndef SNAPMAP_PLUS_NAVMESH_H
#define SNAPMAP_PLUS_NAVMESH_H

#include <stddef.h>

/* The native router packs an outgoing reachability ordinal into eight bits. */
#define SH_AAS_MAX_AREA_REACHABILITIES 256u

/* ---- the frozen wire format ------------------------------------------- */

#define SH_SMNAV_MAGIC          "smnav1."
#define SH_SMNAV_MAGIC_LEN      7
#define SH_SMNAV_DIGEST_CHARS   16          /* sha256 prefix over the payload */
#define SH_SMNAV_GEOM_CHARS     16          /* sha256 prefix over the source geometry */
#define SH_SMNAV_MODULE_CAP     128         /* "category/module" + NUL */
#define SH_SMNAV_CLASS_CAP      32          /* "monster128" + NUL */
#define SH_SMNAV_MAX_SHARDS     2048        /* shards per (module, class) */
#define SH_SMNAV_HEADER_CAP     224
#define SH_SMNAV_RESNAME_CAP    320
#define SH_SMNAV_REASON_CAP     192

/* ---- budgets ----------------------------------------------------------- */

#define SH_SMNAV_MAX_PAYLOAD    (8u * 1024u * 1024u)    /* one AAS payload */
#define SH_SMNAV_MAX_TOTAL      (32u * 1024u * 1024u)   /* all of them, per map */
#define SH_SMNAV_MAX_SETS       192                     /* (module, class) pairs per map */
#define SH_SMNAV_MAX_MODULES    64

/* ---- the AAS2 3.29 binary shape the validator walks -------------------- */

#define SH_AAS_MAGIC            "2SAA"      /* 0x41415332 written little-endian */
#define SH_AAS_MAJOR            3
#define SH_AAS_MINOR            29
#define SH_AAS_HEADER_BYTES     30
#define SH_AAS_SETTINGS_BYTES   364
#define SH_AAS_PREAMBLE_BYTES   (SH_AAS_HEADER_BYTES + SH_AAS_SETTINGS_BYTES)   /* 0x18A */
#define SH_AAS_LUMPS            22
#define SH_AAS_MAX_DEPTH        0x80        /* the engine's own tree-depth limit */
#define SH_AAS_MAX_AREAS        0xFFFEu     /* area indices are u16 everywhere */
#define SH_AAS_MAX_RECORDS      0x1000000u  /* per-lump record cap */

/* Log configured state. Serving uses the existing override-provider hook;
 * load/save handling uses the rawmap funnel.
 */
void sh_navmesh_install(void);

/* THE LIFECYCLE. Called from the deserialize funnel on EVERY buffer the engine
 * is about to parse, before the parse. Clears the serving table and rebuilds it
 * from this map's shards; a map with no shards leaves it empty. Never throws:
 * a fault inside disables the feature for the session. */
void sh_navmesh_build_from_map(const char *json, size_t len);

/* Remove every smnav shard variable from a map buffer, AFTER the table has been
 * built from it. Returns a new NUL-terminated HeapAlloc'd buffer (caller
 * HeapFrees) with *out_len set, or NULL meaning "use the original". */
char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len);

/* THE AUTHOR SIDE. Re-embed every payload the current map is holding, so a
 * load-then-save with no fresh bake preserves the navigation the map arrived
 * with. Returns a new HeapAlloc'd buffer with *out_len set, or NULL for "leave
 * this save alone" (nothing held, disabled, or anything went wrong). */
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len);

/* THE SERVE PATH, called from the overrides open hook. Returns 1 when `name` is
 * a resource this map's bake replaces, with *out_bytes a HeapAlloc'd COPY the
 * caller owns (the table may be rebuilt while the engine still reads a stream,
 * so a served stream never aliases table memory) and *out_len its length.
 * Returns 0 for every other name, which is the overwhelmingly common case and
 * costs one bounded compare per served entry. */
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len);

/* Is the feature live (configured on, and not disabled by a fault)? */
int sh_navmesh_enabled(void);

/* Print what is being served, for which module and classes, or why nothing is.
 * `out` is a printf-like sink (the console's sh_printf in production). */
void sh_navmesh_report(void (*out)(const char *fmt, ...));

/* Validate AAS2 3.29 layout, bounds, cross-indices and BSP/reachability
 * walks. Returns 1 on success, or 0 with err. This checks structure, not
 * navigation quality or complete engine semantics.
 */
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
