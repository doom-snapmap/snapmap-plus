/* navmesh.h -- baked AI navigation carried inside a map, and served to the
 * engine in place of the module's shipped navmesh.
 *
 * WHAT THIS IS
 * ------------
 * SnapMap's AI routes on a per-module AAS navmesh that ships with the game, so
 * anything an author builds -- a platform, a stack of blocks -- is invisible to
 * demons. The fix is not to teach the engine anything: it is to hand it
 * DIFFERENT bytes for the same resource. The engine loads a module's navigation
 * by resource name through the resource-provider open-by-name virtual at vtable
 * +0xf8 -- exactly the slot the overrides file-shadow already replaces
 * (overrides.c). Serving substitute bytes for
 *
 *     maps/modules/<category>/<module>/<module>.aas_<class>
 *
 * and its cooked sibling
 *
 *     generated/maps/modules/<category>/<module>/<module>.baas_<class>
 *
 * therefore changes the navigation, with no new engine hook and no new
 * signature. Both names are served whenever either is, so a stale cooked
 * artefact on disk can never win.
 *
 * The bytes are baked at authoring time and travel inside the map as `smnav1.`
 * shards -- the same envelope map_package.c uses for `smpkg.`, over the shared
 * machinery in map_shards.c, with a deliberately DIFFERENT magic: every
 * released Snapmap+ treats an `smpkg.` header as a required installed package
 * and refuses to load a map carrying an unknown one, while `smnav1.` rides
 * through an older client (and a vanilla client) as inert string variables.
 * The wire format is frozen; the header is
 *
 *     smnav1.<category>/<module>.<class>.<idx>.<total>.<digest16>.<geom16>
 *
 * parsed as FIVE trailing dot-separated fields from the right, because the
 * module field carries `category/module` and may itself contain '/'.
 *
 * WHY THE VALIDATOR IS NOT OPTIONAL
 * ---------------------------------
 * A malformed AAS is FATAL: an access violation inside the engine's own binary
 * loader, recovered only through Error(6), which contaminates the run. Maps are
 * downloaded from other players, so this feature is the first thing in the
 * product that makes the engine parse ATTACKER-CONTROLLED bytes. The digest
 * proves the payload is the one the author's tool embedded; it proves nothing
 * about whether the engine can walk it. sh_navmesh_validate_aas is the answer:
 * an integer-only structural gate over the AAS2 3.29 binary layout -- header,
 * the 22 count-prefixed lumps walking to exactly the payload end, count caps,
 * the cross-lump indices the loader dereferences, and a cycle-guarded BSP walk.
 * Every rule it enforces is an invariant of shipped data, so a correct payload
 * can never be falsely refused. Semantic quality ("is this GOOD navigation")
 * stays at bake time, where a failure can be shown to the author.
 *
 * LIFETIME -- THE RULE THAT PREVENTS THE WORST FAILURE
 * ----------------------------------------------------
 * The serving table is MAP-SCOPED and rebuilt FROM EMPTY on every pass through
 * the deserialize funnel. Otherwise a map with no bake, loaded after one with a
 * bake, inherits the previous map's navigation for the same module, and demons
 * route onto platforms that are not there -- a silent failure nobody would
 * attribute. State lives in backend CRT-heap memory (HeapAlloc), never the
 * engine's map heap, which is HeapDestroy'd at map load.
 *
 * Serving is ALL-OR-NOTHING PER MODULE: a missing shard, a digest mismatch, a
 * failed structural check or a module placed more than once means that module
 * serves NOTHING and the refusal is logged. A carrier that produced one bad
 * payload is not trusted for its siblings.
 *
 * Shards are stripped on load and re-embedded on save, mirroring `smpkg`, so a
 * load-then-save without a fresh bake does not silently discard the author's
 * navigation -- and so ~190 8 KiB shard variables never become engine map state
 * (a map that still carries its payload cannot be playtested at all).
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
#define SH_AAS_MAX_RECORDS      0x1000000u  /* belt-and-braces per-lump cap */

/* One-shot install: report the configured state into the backend log. Registers
 * nothing with the engine -- the serving path is the overrides open hook, which
 * is already installed by then, and the load/save path is the rawmap funnel. */
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

/* THE STRUCTURAL GATE. Returns 1 when `payload` is a binary AAS2 3.29 file the
 * engine's loader cannot be walked out of bounds by, else 0 with the reason in
 * `err`. Integer-only; allocates only the BSP walk's own bookkeeping. This is
 * the whole of the runtime's opinion about a payload's contents. */
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
