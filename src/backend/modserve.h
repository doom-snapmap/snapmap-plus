/* modserve.h -- installed mod bundles: the boot-time index, serving through
 * the resource provider, decl registration for editor rows and per-map
 * gameplay activation.
 *
 * Layout read (Contract 5, written by the builder and the store, never here):
 *   <data>\mods\<folder>\mod.json      the author block and the product block
 *                                      ("snapmap-plus": hash, files, shadows, validated)
 *   <data>\mods\<folder>\assets\...    the files at engine paths
 *   <data>\mods\snapmap-plus.json      the product record; its "aliases" map a
 *                                      dropped delivered copy's hash to the kept bundle
 * <data> is %LOCALAPPDATA%\snapmap-plus. A delivered package (a "legacy"
 * object without "source": "overrides", the retired package folder) is an ordinary folder.
 *
 * Boot reads every mod.json, checks each file's size (never its hash) and
 * builds one immutable table from the lowercased request path to the file.
 * A stock pointer (modbundle.h) has no file to check: it was resolved at
 * install and is read from the installed game when first opened.
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
#define SH_MODSERVE_MAX_JSON    (4u << 20)       /* mod.json */
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
    int         is_stock;    /* a stock pointer: served from the installed game (size 0) */
    const char *stock_source;/* its installed source, or NULL */
} sh_modserve_hit;

/* Constant-time, allocation-free lookup of an engine request path (any case,
 * either separator). Returns 1 and fills *out (when non-NULL) for a served
 * path, else 0. */
int sh_modserve_lookup(const char *request, sh_modserve_hit *out);

/* 1 when an admitted bundle serves `module_name` (maps/modules/...decl, any
 * case) as a snapModule decl: the name a saved map's moduleName holds and the
 * private module palette (grid_room_native.c) is asked to find. */
int sh_modserve_module_served(const char *module_name);

/* Serve `request`: 1 with a process-heap copy of the bytes in *bytes (the
 * caller or the idFile stream frees it with HeapFree), 0 when no bundle
 * serves the name, -1 when a bundle serves it but the file could not be read
 * at its indexed size (logged once per file; the caller falls through to the
 * engine). A stock pointer is answered with the installed entry's bytes read
 * through the registered stock reader (modstock.c), accepted only with the
 * pointer's SHA-256; a pointer whose bytes are equal to another provider's
 * is the same file for the conflict rules. Shadows are logged on their first
 * serve. */
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
    int         shadow;      /* a grandfathered bundle's shadow, or a restored stock decl (modbundle.h
                                sh_mb_stock_restore): skipped as stock when the engine has its source */
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
    /* Optional: 1 when the player's game archives hold `request` (a lower-cased
     * request path), 0 when they do not, -1 when the reader cannot tell. The
     * registry answers 0 for a stock decl whose source is not registered yet,
     * so the model check asks the archives before withdrawing an animWeb. */
    int (*stock_exists)(void *context, const char *request);
    /* Optional: called once between the source registrations and the loads,
     * so shadows the engine parsed before the serving index can be re-parsed
     * (sh_modserve_each_unopened_shadow) after the new identities they name
     * have sources and before a new decl that inherits one is parsed. */
    void (*sources_published)(void *context);
} sh_modserve_engine_ops;

/* Run a plan: first DeclRegisterFile for every unblocked step whose source is
 * not registered yet (registration only publishes the source, so a decl that
 * names another during its parse finds it), then the loads in plan order.
 * Steps already registered by an earlier plan are skipped. One MODS: line per
 * step with the result and the state byte. Returns the number of steps that
 * ended ready (registered now or earlier), or -1 when ops is unusable. */
int sh_modserve_plan_run(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                         const char *phase);

/* The decls an editor row points at directly, which must live as long as the
 * row does. A snapEditorEntityDef is kept for the session (lifetime 4) and its
 * parse stores pointers to the decls its edit block names; a decl the engine
 * created for one of those names after boot sits at level 1, and the purge at
 * the next map unload (entering Play, and leaving it) frees the object while
 * the row still points at it. The editor entity rebuilt after Play then binds
 * the freed animWeb (lane MG2: AV in the animator's node set-up, VK
 * 0x14170a5d3). Stock rows never meet this: everything they name was alive at
 * the boot promotion. For every ready row of the plan, this calls `fn` once per
 * reference: eventInfo animWebDecl (animWeb), renderInfo renderModelName ending
 * ".md6" (md6Def), previewImage (material). Returns the number of calls. */
typedef void (*sh_modserve_ref_fn)(void *context, const char *type, const char *name, const char *row);
int sh_modserve_editor_refs(const sh_modserve_plan *plan, sh_modserve_ref_fn fn, void *context);

/* The rest of a row's closure: what the editor pass created. Keeping the
 * decls a row names is not enough: an md6Def holds its MD6 model (+0x60), the
 * model its skeleton and joint tables, an animWeb its anims, a material its
 * images, all separate resources the engine created at level 1 while the pass
 * loaded the row (lane MG2: the second site, idJointAnimator's joint blend
 * reading a freed model's table, VK 0x14173fb46). The engine records no such
 * closure, so the pass takes a watermark of every resource before it runs and
 * raises every resource created since then that is still at level 1 to 4, as
 * a boot-time row's closure was raised by the boot promotion.
 *
 * Layout read (13954591, both renderers: the purge driver and GetDeclsOfType
 * walk it): `head_slot` is the address of the global list-of-lists head
 * (engine global decl_resource_head); each idResourceList holds its class
 * name (char *) at +0x10, the next list at +0x18, its entries at +0x20 and
 * their count (int) at +0x28; each resource holds its level at +0x28. Reads
 * are bounded and fault-protected; main thread only, like every pass here. */
typedef struct sh_modserve_watermark {
    uintptr_t *res;   /* sorted resource addresses (malloc) */
    size_t n;
    int ok;           /* 1 when the walk completed */
} sh_modserve_watermark;

typedef struct sh_modserve_raised {
    unsigned raised;      /* new resources raised from level 1 to 4 */
    unsigned created;     /* new resources seen (any level) */
    unsigned lists;       /* lists walked */
    char summary[200];    /* "anim 40, md6Def 2, ..." by class, largest first */
} sh_modserve_raised;

int  sh_modserve_watermark_take(const void *head_slot, sh_modserve_watermark *w);
/* Raise what the walk now finds beyond `w`; returns 1 when the walk completed. */
int  sh_modserve_watermark_raise(const void *head_slot, const sh_modserve_watermark *w, sh_modserve_raised *out);
void sh_modserve_watermark_free(sh_modserve_watermark *w);

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

/* A mod.json may carry {"requirements": {"cvars": {...}}} naming the two
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

/* sh_modserve_requirements_apply for a point where nothing is registered
 * (the Play level load): the same disarm and verify, plus one line when every
 * needed gate was already live, so the log shows the gates held:
 *   MODS: play: requirements live: g_useImageBlackList=0, g_useResourceBlackList=0 (unchanged since the last pass)
 * Returns what sh_modserve_requirements_apply returns. */
int sh_modserve_requirements_check(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

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

/* A path several admitted bundles of different mods provide with different
 * bytes. One provider always serves it (the path is never left to the
 * engine, whose SnapMap archives may hold no copy of campaign content). The
 * winner is the provider with the fewest needs no admitted bundle provides,
 * then the bytes most providers carry, then an installed mod over a copy a
 * map delivered (legacy\), then the most recently installed. When every
 * provider's text is a decl that differs only inside lists (a block of
 * `num = N;` and `item[i]` members, such as the encounter conductor's
 * aiTypeList or a whitelist inspector's validEncounters), the winner's text
 * is served with the other providers' list entries appended (composed). One
 * MODS: line per conflict names the providers, the winner and why:
 *   MODS: conflict: shadow '<path>' is provided by a@1, b@2; serving a@1 (<reason>)
 *   MODS: conflict: shadow '<path>' is provided by ...; serving a@1's text with the others' list entries (N added)
 * Pointers stay valid for the process. */
typedef struct sh_modserve_conflict {
    const char *request, *providers, *reason;
    const char *served_id, *served_version, *served_hash;
    int count;        /* providers */
    int composed;     /* 1: list entries of every provider merged */
    int withdrawn;    /* 1: a registration pass withdrew the winner (see plan_run) */
} sh_modserve_conflict;

/* 1 and *out filled when `request` (any case) is such a path, else 0. */
int sh_modserve_conflict_info(const char *request, sh_modserve_conflict *out);

/* Counts for the status command and tests. */
typedef struct sh_modserve_stats {
    int bundles_seen, bundles_admitted, files, keys, conflicts, conflicts_composed, shadows;
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

/* Visit every admitted bundle whose mod.json author block carries keys the
 * product does not read itself (the manifest's "extra", such as "strings" and
 * "hud"), in a fixed order: installed mods before bundles a map delivered, then
 * by id (case-insensitive), then by version. `extra` is the object's JSON
 * text; the strings stay valid for the process. Returns the number visited,
 * or -1 when the order could not be allocated (nothing is visited). Used by
 * strids.c for the bundle string layer. */
typedef void (*sh_modserve_extra_fn)(void *context, const char *id, const char *version, int legacy,
                                     const char *extra);
int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context);

/* Visit every file the published table serves (admitted bundles, conflict
 * winners, not withdrawn), in index order. The strings stay valid for the
 * process. Used by modvt.c to find the bundles' virtual-texture members. */
typedef void (*sh_modserve_file_fn)(void *context, const char *request, uint64_t size, const char *bundle_id,
                                    const char *version, const char *bundle_hash);
void sh_modserve_each_served(sh_modserve_file_fn fn, void *context);

/* Early decls. The serving index and the provider are published after the
 * backend's signature scan, while the engine is already parsing boot decls,
 * so a shadow parsed before then holds the game's bytes. Visit, in plan rank
 * order, every served shadow decl (type and name in the engine's spelling)
 * that the provider has never opened and that no earlier visit settled; `fn`
 * returns 1 when it re-parsed the decl (the file is not offered again) and 0
 * when the engine has not parsed it yet (offered again at the next pass). A
 * rescan keeps both marks for files whose bytes did not change. Returns the
 * number visited. */
typedef int (*sh_modserve_unopened_fn)(void *context, const char *type, const char *name, const char *request,
                                       const char *bundle_id);
int sh_modserve_each_unopened_shadow(sh_modserve_unopened_fn fn, void *context);

/* A bundle's requirement gates (the cut-content blacklists) are applied at
 * the first registration pass, after DOOM loaded its boot resources: a model
 * or image served before then was resolved against the blacklists (the Spider
 * Mastermind mesh held no materials and drew nothing). Visit every served
 * baseModel and image first opened while one of its bundle's gates was not
 * live, once those gates are live; `fn` returns 1 when it reloaded the
 * resource (not offered again). The early-decl visit above also offers decls
 * first opened before their gates were live. Returns the number visited. */
int sh_modserve_each_ungated_raw(sh_modserve_unopened_fn fn, void *context);

/* Refresh. A rescan replaces the table while the engine keeps every decl it
 * parsed and every model and image it loaded from the bytes the old table
 * served. Each table publication compares, per served request path, the new
 * table with the one it replaced: a path whose bytes differ is "changed", one
 * only the new table serves is "new" and one only the old table served is
 * "gone". Changed decls, models and images, and gone shadow decls (which
 * return to the game's bytes), are marked pending; marks carry over to later
 * tables while the bytes stay the same, so changes recorded during Play are
 * applied at the next safe point. New requests are reported, never applied:
 * a new decl name registers at the next map load of a map that names its
 * bundle, a new editor row at 'sh_mods palette rebuild'. */
#define SH_MODSERVE_RF_DECL  1
#define SH_MODSERVE_RF_MODEL 2   /* baseModel */
#define SH_MODSERVE_RF_IMAGE 3

typedef struct sh_modserve_diff {
    int changed;                          /* served requests whose bytes changed */
    int changed_decls, changed_models, changed_images;
    int changed_map;                      /* other files the next map load reads again (generated/swf/) */
    int changed_restart;                  /* other files nothing reloads in this process (sound banks, anims, ...) */
    int added, added_decls, added_rows;   /* new requests; of them new decl names; of those editor rows */
    int added_active;                     /* new gameplay decl names of mods registered this session */
    int changed_rows;                     /* changed editor rows (snapEditorEntityDef) */
    int gone, gone_shadows, gone_names;   /* gone requests; shadow decls; decls of new names (kept registered) */
    int gone_rows;                        /* gone editor rows: re-parsed without a source, then left out of the palette */
    int dependents;                       /* served decls marked by sh_modserve_refresh_expand */
    int pending;                          /* items still waiting (every table), counted at the call */
    char restart_first[200];              /* the first files that need a restart, for the log */
} sh_modserve_diff;

/* The published table's diff against the table it replaced (zero for the
 * first table), plus the pending items now. */
void sh_modserve_last_diff(sh_modserve_diff *out);

/* Mark, once per table, every served decl that inherits (top-level
 * `inherit`, any depth) from a changed or gone decl, since the engine
 * resolves a parent when the child is parsed. Reads those types' served
 * decls (stock pointers from the installed game). Main thread. Returns the
 * number marked. */
int sh_modserve_refresh_expand(void);

/* The gameplay decls a rescan added to a mod this session already
 * registered (diff added_active), as a registration plan; the marks are
 * consumed. Returns the step count or -1. Main thread. */
int sh_modserve_plan_refresh(sh_modserve_plan *out);

typedef struct sh_modserve_refresh_item {
    const char *type, *name, *request, *bundle_id;
    int kind;          /* SH_MODSERVE_RF_* */
    int gone;          /* 1: no bundle serves the request now; the engine's own bytes answer it */
    int shadow;        /* a stock path */
    int gates_live;    /* the bundle's requirement gates are live (1 when it has none) */
} sh_modserve_refresh_item;

/* Visit every pending item: changed decls in plan rank order, then gone
 * shadow decls, then changed models and images. `fn` returns 1 when the item
 * is settled (re-parsed, reloaded, or not held by the engine, whose next load
 * reads the current bytes) and 0 to keep it pending. A decl settled here is
 * not offered to the early-decl visit. Returns the number visited. */
typedef int (*sh_modserve_refresh_fn)(void *context, const sh_modserve_refresh_item *item);
int sh_modserve_each_refresh(sh_modserve_refresh_fn fn, void *context);

/* What a refresh did (modserve_engine.c fills it; the Mods tab's rescan
 * result carries it as {reparsed, reloaded, needsMapReload, needsRestart, note}). */
typedef struct sh_modserve_refresh {
    int state;             /* 0 nothing changed, 1 applied, 2 deferred to the next safe point */
    int reparsed;          /* decls re-parsed */
    int reloaded;          /* models and images reloaded in place */
    int needs_map_reload;  /* new decl names and editor rows, files the next map load reads */
    int needs_restart;     /* changes nothing reloads in this process */
    int registered;        /* new decls registered */
    int palette;           /* 1 the palette was rebuilt, 0 not needed, -1 refused */
    int pending;           /* items left for the next safe point */
    int failed;            /* re-parses or reloads that failed or faulted */
    char note[300];
} sh_modserve_refresh;

/* Walk the engine's resource lists (decl_resource_head): fn(context, the
 * list's class name, the resource, its name). 1 when the walk completed. */
typedef void (*sh_modserve_resource_fn)(void *context, const char *list_class, void *resource, const char *name);
int sh_modserve_resource_each(const void *head_slot, sh_modserve_resource_fn fn, void *context);

/* How many times the provider served `request` from the published table. */
long sh_modserve_open_count(const char *request);

/* Functions called after every table this layer publishes (boot, rescan),
 * on the thread that built it, outside the build lock, in the order they
 * were added (up to four; adding one twice is ignored). NULL clears all. */
typedef void (*sh_modserve_index_fn)(void);
void sh_modserve_set_index_listener(sh_modserve_index_fn fn);

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

/* Called before the Play level load (the SnapBuild detour in nav_play.c),
 * on the main thread: re-checks the admitted bundles' requirements with
 * sh_modserve_requirements_check (phase "play"). The engine does not reset
 * either gate between the editor and the level load on 13954591 (lane MG2:
 * each gate's int has one reader, its matcher, and no engine writer; both are
 * plain bool cvars that no shipped config names), so this normally logs
 * "requirements live"; a gate someone set back to 1 is disarmed again before
 * the level's decls load. No-op when no bundle has requirements. */
void sh_modserve_play_begin(void);

/* After a rescan published a table: apply its pending changes now (main
 * thread, not during Play) or leave them for the next registration pass
 * (the editor palette pass, a map's activation), which applies them between
 * publishing its sources and running its loads. Logs one MODS: refresh: line
 * per category and a summary. Fills *out (when non-NULL) and returns its
 * state: 0 nothing changed, 1 applied, 2 deferred. */
int sh_modserve_refresh_after_rescan(sh_modserve_refresh *out);

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
