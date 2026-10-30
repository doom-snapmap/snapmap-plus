/* modserve.h -- installed mod bundles: the boot-time index, serving through
 * the resource provider, decl registration for editor rows and per-map
 * gameplay activation.
 *
 * Layout read (built by the bundle builder, never written here):
 *   <data>\mods\<id>@<version>\bundle.json      the bundle manifest
 *   <data>\mods\<id>@<version>\.state.json      {"validated": bool, "firstCause": ...}
 *   <data>\mods\<id>@<version>\assets\...       the files at engine paths
 *   <data>\mods\blobs\<sha256>                  fallback copy of any file by its blob hash
 *   <data>\mods\legacy\<id>@<digest16>\...      converted first-generation packages
 * <data> is %LOCALAPPDATA%\snapmap-plus.
 *
 * Boot reads every bundle.json, checks each file's size (never its hash) and
 * builds one immutable table from the lowercased request path to the file.
 * A bundle that fails (manifest, state, size, path, namespace) is skipped with
 * one MODS: line naming the first cause; other bundles and the product's own
 * sources are unaffected. Lookups fold case and '\' to '/', allocate nothing
 * and cost one hash and one compare.
 *
 * Precedence in the provider (overrides.c ov_open_body): the navigation grid,
 * map navigation, the Grid Room, built-in defaults, then mods, then the
 * engine. Mode >= 2 opens are never served.
 *
 * Threads: the index is built on the backend's bootstrap thread before the
 * provider hook is published, and replaced only by modserve_index_rescan;
 * readers see one complete table or the previous one. Every registration
 * entry point runs on DOOM's main thread only and refuses elsewhere.
 */
#ifndef SH_MODSERVE_H
#define SH_MODSERVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODSERVE_HASH_HEX    64
#define SH_MODSERVE_MAX_FILES   4096u            /* per bundle, as the builder caps it */
#define SH_MODSERVE_MAX_FILE    (64ull << 20)    /* per file */
#define SH_MODSERVE_MAX_JSON    (1u << 20)       /* bundle.json */
#define SH_MODSERVE_CACHE_FILE  (1u << 20)       /* files below this are cached */
#define SH_MODSERVE_CACHE_TOTAL (8u << 20)       /* the cache's total byte bound */

/* ------------------------------------------------------------------ index */

/* Build the table from `mods_root` (the folder that holds the bundle folders)
 * and publish it, replacing any earlier table. Logs one MODS: line per
 * skipped bundle, per conflict and per superseded version, then
 * "MODS: index built in N ms (B bundles, F files)". Returns the number of
 * bundles admitted, or -1 when the table could not be allocated (the previous
 * table, if any, stays published). A missing folder is an empty index. */
int sh_modserve_index_build(const char *mods_root);

/* The same for %LOCALAPPDATA%\snapmap-plus\mods. Called once by the bootstrap
 * before the provider hook is installed. */
int sh_modserve_index_boot(void);

/* Rebuild from the folder of the last build (after an install or a fetch).
 * The old table is retired, never freed, because a provider open on another
 * thread may still be reading it. Decls already registered stay registered;
 * editor rows of new bundles need modserve_palette_rebuild_requested. */
int sh_modserve_index_rescan(void);

/* Nonzero when the published table serves at least one path. */
int sh_modserve_active(void);

/* ------------------------------------------------------------------ serve */

/* One lookup result. Pointers stay valid for the process. */
typedef struct sh_modserve_hit {
    const char *request;     /* the indexed request path (lowercase) */
    const char *disk_path;   /* the file read at open time */
    uint64_t    size;        /* the size boot checked */
    const char *bundle_id;
    const char *bundle_hash;
    int         is_decl;     /* request is generated/decls/... */
    int         is_shadow;   /* request is a stock path the bundle replaces */
} sh_modserve_hit;

/* Constant-time, allocation-free lookup of an engine request path (any case,
 * either separator). Returns 1 and fills *out (when non-NULL) for a served
 * path, else 0. */
int sh_modserve_lookup(const char *request, sh_modserve_hit *out);

/* Serve `request`: 1 with a process-heap copy of the bytes in *bytes (the
 * caller or the idFile stream frees it with HeapFree), 0 when no bundle
 * serves the name, -1 when a bundle serves it but the file could not be read
 * at its indexed size (logged once per file; the caller falls through to the
 * engine). Shadows are logged on their first serve. */
int sh_modserve_open(const char *request, unsigned char **bytes, size_t *length);

/* Whether the decl existence probe should answer true for `path`: a served
 * decl request (generated/decls/... or decltree/...). Counts the claim. */
int sh_modserve_probe_claims(const char *path);

/* ------------------------------------------------------------------ plan */

/* The decl registration plan: every decl to register, in dependency order
 * (raw files need nothing and are never steps; material2, skins and other
 * decls first, then md6Def, then entityDef, then snapEditorEntityDef, and
 * within that every step after the steps its needs name). A step whose need
 * names a mods/ identity that no admitted bundle provides is blocked, and so
 * is every step that needs a blocked step. */
typedef struct sh_modserve_step {
    const char *type;        /* engine spelling, e.g. "md6Def" */
    const char *name;        /* decl name, e.g. "mods/alex.pink-imp/imp.md6" */
    const char *reg_path;    /* DeclRegisterFile source, e.g. "md6def/mods/.../imp.md6.decl" */
    const char *bundle_id;
    int         rank;        /* 0 other decls, 1 md6Def, 2 entityDef, 3 snapEditorEntityDef */
    int         persist;     /* 1: keep through map purges (lifetime 4) */
    int         shadow;      /* a grandfathered bundle's shadow: skipped as stock when the engine has its source */
    const char *blocked;     /* NULL, or the first cause (static or index-owned text) */
    int         file;        /* index-internal file number */
    int         ready;       /* set by sh_modserve_plan_run: 1 ready, -1 not ready, 0 not run */
} sh_modserve_step;

typedef struct sh_modserve_plan {
    sh_modserve_step *steps; /* malloc-owned */
    int count;
    int blocked;
} sh_modserve_plan;

/* Editor plan: every snapEditorEntityDef of every admitted bundle and the
 * closure of bundle-provided decls it needs. Returns the step count or -1. */
int sh_modserve_plan_editor(sh_modserve_plan *out);

/* Gameplay plan for exactly the named bundles (64-hex bundle hashes): every
 * non-editor, non-shadow decl they provide and the closure of bundle-provided
 * decls those need. Unknown hashes are skipped and counted in *unknown. */
int sh_modserve_plan_bundles(const char *const *bundle_hashes, int n, sh_modserve_plan *out,
                             int *unknown);

void sh_modserve_plan_free(sh_modserve_plan *plan);

/* The engine operations a plan runs through. Production binds them to the
 * decl registry (modserve_engine.c); tests supply fakes. Each returns 1 for
 * success. `state` and `lifetime` receive the resource bytes at +0x2c/+0x28
 * after the load, for the log. */
typedef struct sh_modserve_engine_ops {
    void *context;
    int (*source_registered)(void *context, const char *type, const char *name);
    int (*register_file)(void *context, const char *type, const char *name, const char *reg_path);
    int (*load)(void *context, const char *type, const char *name, int persist,
                unsigned *state, unsigned *lifetime);
} sh_modserve_engine_ops;

/* Run a plan: first DeclRegisterFile for every unblocked step whose source is
 * not registered yet (registration only publishes the source, so a decl that
 * names another during its parse finds it), then the loads in plan order.
 * Steps already registered by an earlier plan are skipped. One MODS: line per
 * step with the result and the state byte. Returns the number of steps that
 * ended ready (registered now or earlier), or -1 when ops is unusable. */
int sh_modserve_plan_run(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                         const char *phase);

/* Editor rows at map load (lane M6). The palette is built once per process,
 * at editor init, and the map validator (RepairAndMigrate) looks every placed
 * entity up in it; a bundle converted or named mid-session can bring
 * snapEditorEntityDef rows that build never saw. `palette_ready` answers 1
 * when the editor palette exists, `in_palette` 1 when a row for that sedef
 * name is in it, and `rebuild` runs the heap-scoped palette rebuild (whose
 * editor pass registers pending rows first) and returns 1 when the builder
 * ran, else the refusal code. */
typedef struct sh_modserve_palette_ops {
    void *context;
    int (*palette_ready)(void *context);
    int (*in_palette)(void *context, const char *sedef_name);
    int (*rebuild)(void *context, int *rows_before, int *rows_after);
} sh_modserve_palette_ops;

typedef struct sh_modserve_rows_sync {
    int rows;          /* editor rows the named bundles provide */
    int missing;       /* of them, absent from the palette and never registered */
    int rebuild;       /* 0 not requested, else what ops->rebuild returned */
    int registered;    /* missing rows registered by the rebuild's editor pass */
    int admitted;      /* missing rows in the palette after the rebuild */
    int rows_before, rows_after;   /* palette row counts around the rebuild (-1 unknown) */
} sh_modserve_rows_sync;

/* For exactly the named bundles (64-hex hashes): when the palette exists and
 * lacks at least one of their rows that no editor pass has visited, request
 * one rebuild. Nothing is asked of ops for n <= 0 or bundles without rows,
 * and nothing past palette_ready when no palette exists yet (the first
 * editor entry's build registers every row). Returns 1 when the rebuild was
 * requested, 0 when not, -1 on unusable ops or no memory. */
int sh_modserve_map_rows_sync(const char *const *bundle_hashes, int n, const sh_modserve_palette_ops *ops,
                              sh_modserve_rows_sync *out);

/* ------------------------------------------------------------------ requirements */

/* A bundle.json may carry {"requirements": {"cvars": {...}}} naming the two
 * cut-content blacklist gates with the value "0" (or 0): nothing else. The
 * index collects them per bundle; a bundle naming any other key, cvar or value
 * is skipped with that first cause. Each admitted bundle's requirements are
 * logged once, the first time an index carries it:
 *   MODS: requirements: g_useResourceBlackList=0 for <id>@<version>
 * Disarming a gate is global (S1 consequence 16): with g_useImageBlackList 0
 * the five stock font-atlas directories load as well. */
#define SH_MODSERVE_REQ_IMAGE    1u   /* g_useImageBlackList 0 */
#define SH_MODSERVE_REQ_RESOURCE 2u   /* g_useResourceBlackList 0 */
#define SH_MODSERVE_REQ_COUNT    2

/* The cvar name of requirement bit `index` (0 or 1), else NULL. */
const char *sh_modserve_requirement_cvar(int index);

/* The union of the admitted bundles' requirement bits. */
unsigned sh_modserve_requirements(void);

/* How the gates are read and set. `read` returns 1 with the integer value.
 * `set` returns SH_MODSERVE_GATE_SET when the value is in the cvar on return
 * (idCVar::Set), SH_MODSERVE_GATE_QUEUED when it was only buffered as console
 * text, or 0 when refused. `drain` executes the command buffer and returns 1;
 * it may be NULL for a route whose sets are never queued. */
#define SH_MODSERVE_GATE_SET    1
#define SH_MODSERVE_GATE_QUEUED 2
typedef struct sh_modserve_gate_ops {
    void *context;
    int (*read)(void *context, const char *cvar, int *value);
    int (*set)(void *context, const char *cvar, const char *value);
    int (*drain)(void *context);
} sh_modserve_gate_ops;

typedef struct sh_modserve_gates {
    unsigned needed;     /* union of admitted bundles' requirements */
    unsigned changed;    /* gates set by this call (needed or restored) */
    unsigned queued;     /* of them, sets that were only buffered */
    int drained;         /* 1 when the command buffer was drained between the phases */
    unsigned live;       /* needed gates read back as 0 */
    unsigned not_live;   /* needed & ~live */
    unsigned restored;   /* gates no bundle needs any more, returned to their displaced value */
} sh_modserve_gates;

/* Make the union live before a registration pass, in two phases. Phase 1
 * (disarm) reads each needed gate and sets it to 0 when it is not; when any
 * set was queued, the command buffer is drained before phase 2. Phase 2
 * (verify) reads every gate back; only a read-back 0 counts as live. A gate
 * this layer changed and no admitted bundle needs any more is restored to the
 * value it displaced, unless someone else changed it meanwhile. Returns 0
 * when no gate is needed (and when none is held either, ops is never
 * called), 1 when every needed gate is live, -1 otherwise. Main thread only. */
int sh_modserve_requirements_apply(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

/* Block the steps of bundles whose requirements `gates` reports not live
 * (first cause "requirement <cvar>=0 is not live"), except steps already
 * registered. Returns how many were blocked. */
int sh_modserve_plan_block_unmet(sh_modserve_plan *plan, const sh_modserve_gates *gates);

/* sh_modserve_requirements_apply, then sh_modserve_plan_run: no
 * DeclRegisterFile is issued before the gates are verified. Steps of a bundle
 * whose requirement is not live are blocked with that first cause (unless
 * already registered), and steps needing them fail with it. An empty plan
 * touches nothing. Returns what sh_modserve_plan_run returns. */
int sh_modserve_plan_run_gated(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                               const sh_modserve_gate_ops *gates, const char *phase, sh_modserve_gates *out);

/* ------------------------------------------------------------------ queries */

/* 1 installed and admitted, 0 not installed, -1 installed but skipped (the
 * first cause is copied to `cause` when non-NULL). */
int sh_modserve_bundle_state(const char *bundle_hash, char *cause, size_t cause_cap);

/* Map placed logical names (entityDef inherit names, or any decl name a
 * bundle provides) to the installed bundles that provide them, including the
 * bundles their needs come from. Writes up to `cap` distinct 64-hex hashes
 * (NUL-terminated) in first-seen order and returns how many were found
 * (which may exceed cap). Stock names map to nothing. */
int modserve_bundles_for_names(const char *const *names, int n_names,
                                  char (*out_hashes)[SH_MODSERVE_HASH_HEX + 1], int cap);

/* The save-time closure in the form the map manifest consumes (Contract 3):
 * `names` are the logical names a map places, untyped and as the map spells
 * them, matched case-insensitively (entityDef first, then any decl name an
 * admitted bundle provides). `visit` is called once per (bundle, placed name)
 * match, then once with name NULL for each further bundle the needs closure
 * reaches. `size` is the sum of the bundle's file sizes. Returns the number of
 * distinct bundles reported. Stock names match nothing. */
typedef void (*sh_modserve_closure_fn)(void *context, const char *bundle_id, const char *version,
                                       const char *bundle_hash, unsigned long long size, const char *name);
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context);

/* Counts for the status command and tests. */
typedef struct sh_modserve_stats {
    int bundles_seen, bundles_admitted, files, keys, conflicts, shadows;
    unsigned long opens_served, open_failures, probe_claims, cache_hits;
    double build_ms;
    int editor_rows_registered, editor_rows_failed;
} sh_modserve_stats;
void sh_modserve_get_stats(sh_modserve_stats *out);

/* Visit every scanned bundle (admitted or skipped). */
typedef void (*sh_modserve_bundle_fn)(void *context, const char *id, const char *version,
                                      const char *hash, int admitted, const char *cause,
                                      int files, int legacy);
void sh_modserve_each_bundle(sh_modserve_bundle_fn fn, void *context);

/* Whether `name` is a snapEditorEntityDef that an admitted bundle provides
 * (palette reporting). */
int sh_modserve_is_mod_sedef(const char *name);

/* ------------------------------------------------------------------ engine */
#ifndef SH_MODSERVE_PURE
#include "signatures.h"

/* Bind the decl registry, the process-heap scope and the main-thread identity
 * (as timeline_tile.c does) and detour the decl existence probe
 * (DeclResourceProbe, vtable +0x78, seven arguments forwarded) when the index
 * holds decls. Call after the override provider is installed. Returns 1 when
 * registration is available. */
int sh_modserve_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by the Timeline tile's SnapPaletteBuild detour before the original
 * builder runs, on the main thread: registers the editor plan's steps that
 * are not registered yet (the first build registers every editor row), and
 * logs "MODS: editor rows registered in N ms (R rows)". Like every pass here
 * (per-map activation, the map-rows sync and the runtime rebuild, which both
 * come back through this point), it first applies the admitted bundles'
 * requirements with sh_modserve_requirements_apply through idCVar::Set,
 * which is immediate (no drain needed), and blocks what they would not let
 * load. */
void sh_modserve_palette_before_build(void);

/* Called after the original builder: logs the row count and how many rows
 * hold mod sedefs. */
void sh_modserve_palette_after_build(void *palette);

/* Rebuild the live editor palette so rows of bundles indexed after the first
 * build appear, the way the engine's own callers build it (lane R3): main
 * thread only, refused while the editor holds an entity or a wire, inside
 * process heap 0 (verified on entry and exit), pending registrations first,
 * then SnapPaletteBuild(editor+0x20660, NULL) through the detour, then the
 * product's palette caches are invalidated. Logs one MODS: line with the row
 * count before and after. Returns 1 when the builder ran, 0 when refused. */
int modserve_palette_rebuild_requested(void);

/* Register the gameplay decls of exactly these bundles (64-hex hashes) at the
 * engine's own lifetime (level 1, purged at map unload) when they are not
 * registered yet. Called by the map manifest (modmanifest.c) on every parse,
 * main thread only. n <= 0 returns 0 and logs nothing, so a map without
 * manifest rows costs nothing. Logs "MODS: activated B bundles (S steps, R
 * ready) in N ms". Then, before the map parses, the bundles' editor rows the
 * built palette lacks are registered and the palette is rebuilt once
 * (sh_modserve_map_rows_sync; refused while an entity or a wire is held),
 * logged as one "MODS: map rows: ..." line. Returns the number of ready
 * steps, or -1 when refused (not the main thread, registry unavailable). */
int modserve_activate_bundles(const char *const *bundle_hashes, int n);

/* The serving subcommands of the sh_mods console command (modcmd.c owns the
 * command and dispatches here): status, rescan, palette rebuild. `args` is
 * the engine's idCmdArgs. Returns 1 when `verb` is one of them (handled),
 * 0 otherwise. */
int sh_modserve_command(void *args, const char *verb);
#endif


#ifdef __cplusplus
}
#endif

#endif
