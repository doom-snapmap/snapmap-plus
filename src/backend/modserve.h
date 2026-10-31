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

/* Reinstating an uninstalled mod the open map still needs (modui.c owns the
 * recycle bin, removed-mods\, and binds the hook). The points where the
 * backend learns what a map needs call sh_modserve_restore: the map rows at a
 * load (`hashes`, the manifest's bundle hashes), the save observer (`names`,
 * the placed names), a Play entry and the undo watch (neither: the hook reads
 * the open map's placed names). A bundle that resolves to nothing installed
 * but matches a recycled folder is moved back into mods\ and the serving
 * index rebuilt before the call returns; what the running game still needs
 * (registration, the palette, the toast) follows on a later main-thread tick.
 * Returns the number of mods reinstated, 0 when none (or no hook). */
#define SH_MODSERVE_RESTORE_LOAD 1
#define SH_MODSERVE_RESTORE_SAVE 2
#define SH_MODSERVE_RESTORE_PLAY 3
#define SH_MODSERVE_RESTORE_UNDO 4
typedef int (*sh_modserve_restore_fn)(const char *const *hashes, int hash_count, const char *const *names,
                                      size_t name_count, int when);
void sh_modserve_set_restore(sh_modserve_restore_fn fn);
int  sh_modserve_restore(const char *const *hashes, int hash_count, const char *const *names, size_t name_count, int when);

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

/* The same lookup without the map scope (below): what the index holds for
 * `request`, also when its bundle's shadows are withdrawn because the open map
 * does not list it. For reports (the Mods tab, sh_mods doctor), never for
 * serving. */
int sh_modserve_lookup_indexed(const char *request, sh_modserve_hit *out);

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
 * after the load, for the log. `load` may also answer
 * SH_MODSERVE_LOAD_REFUSED: the engine raised a parse error naming the decl's
 * file during the load. An animWeb's parse catches its own error and leaves a
 * truncated web that looks loaded (state bit 0 clear), so the state byte alone
 * cannot tell; the plan withdraws such a file and never loads it again. */
#define SH_MODSERVE_LOAD_REFUSED 2
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
    /* Optional: 1 when the engine's own lookup finds the decl now (DeclFind
     * without makeDefault, the lookup an animWeb's parse makes for each model
     * it names), 0 when it does not, -1 when it cannot tell. When present it
     * decides the model check for a decl the pass loads now: the game
     * archives also hold campaign content SnapMap cannot load. */
    int (*decl_found)(void *context, const char *type, const char *name);
    /* Optional: the engine's error text after `load` answered
     * SH_MODSERVE_LOAD_REFUSED, for the log and the console line. */
    void (*refusal)(void *context, char *out, size_t cap);
} sh_modserve_engine_ops;

/* The console sink for the lines a player should see when a pass skips or
 * refuses a decl (production: the DOOM console). NULL turns it off. */
typedef void (*sh_modserve_console_fn)(const char *line);
void sh_modserve_set_console(sh_modserve_console_fn fn);

/* The model check for the served animWeb at `request`, as a load now would
 * meet it: 1 with the first model its models or modelInfos block names that
 * no bundle serves and the engine cannot find (written to `model`), 0 when
 * every model resolves or the check cannot tell, -1 when `request` is not a
 * served animWeb. */
int sh_modserve_animweb_check(const char *request, const sh_modserve_engine_ops *ops, char *model, size_t cap);

/* Withdraw the file serving `request` (lookups and the probe miss it, a
 * rescan keeps it withdrawn while its bytes are unchanged), logging `why` and
 * printing one console line. 1 when a served file was withdrawn. */
int sh_modserve_withdraw(const char *request, const char *type, const char *name, const char *why);

/* 1 when the engine error text `text` names the decl file of `type`/`name`
 * ("...\generated\decls\animWeb\designsystems\med_station.decl(21): error : ..."),
 * matched without case and with either slash. */
int sh_modserve_error_names_decl(const char *text, const char *type, const char *name);

/* Run `load(resource)` and put the resource's lifetime (+0x28) back as it was.
 * ResourceGenericLoad's failure path reconstructs the object in place, and
 * the constructor writes level 1 or 2, so a permanent decl that failed an
 * in-place re-parse would be destroyed by the next map unload while every
 * permanent consumer still points at it. Returns the level the load left
 * (before the restore); `*was` receives the level before. */
unsigned sh_modserve_load_keep_lifetime(void *resource, void (*load)(void *), unsigned *was);

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

/* A mod.json may carry
 *   {"requirements": {"cvars": {"<cvar>": <value>, ...},
 *                     "commands": {"activate": ["<line>", ...], "deactivate": [...]}}}
 * read through the builder's canonical form (modbundle.h,
 * sh_mb_requirements_canonical): any cvar at any value, any single console
 * line, within the builder's bounds; this layer names none of them. A bundle
 * whose requirements the builder refuses is skipped with that first cause.
 * Bundles are taken in install order (built time, then folder name); one that
 * asks a cvar an earlier accepted bundle asks at another value loses, both
 * named once in the log, and none of its requirements is applied (its steps
 * are blocked):
 *   MODS: requirements: conflict: <cvar>: <later> asks "<a>", <earlier> asks "<b>"; <later>, the later install, loses ...
 * Each accepted bundle's requirements are logged once, the first time an
 * index carries it with that text:
 *   MODS: requirements: g_useResourceBlackList=0 for <id>@<version>
 *   MODS: requirements: 1 activate and 1 deactivate command(s) for <id>@<version>
 * A cvar a requirement sets is global (with g_useImageBlackList 0 the five
 * stock font-atlas directories load as well, S1 consequence 16). */

/* The number of distinct cvars the accepted bundles' requirements set. */
int sh_modserve_requirements(void);

/* 1 when a pass has work: an accepted bundle has requirements, or a cvar
 * this layer set, or a bundle whose activate lines ran, waits for release. */
int sh_modserve_requirements_held(void);

/* How cvars are read and set and console lines are run. `read` copies the
 * cvar's value text (NUL-terminated, truncated to cap) and returns 1, or 0
 * when the engine has no such cvar. `set` returns SH_MODSERVE_GATE_SET when
 * the value is in the cvar on return (idCVar::Set), SH_MODSERVE_GATE_QUEUED
 * when it was only buffered as console text, 0 when the engine refused it,
 * or a negative SH_MODSERVE_GATE_* refusal. `drain` executes the command
 * buffer and returns 1; it may be NULL for a route whose sets are never
 * queued. `command` appends one console line to the engine's command buffer
 * and returns 1; with NULL no line runs (they wait for a pass that has it). */
#define SH_MODSERVE_GATE_SET        1
#define SH_MODSERVE_GATE_QUEUED     2
#define SH_MODSERVE_GATE_READ_ONLY  (-1)   /* the cvar is flagged read-only */
#define SH_MODSERVE_GATE_INIT_ONLY  (-2)   /* the cvar is settable only on the command line */
typedef struct sh_modserve_gate_ops {
    void *context;
    int (*read)(void *context, const char *cvar, char *value, size_t cap);
    int (*set)(void *context, const char *cvar, const char *value);
    int (*drain)(void *context);
    int (*command)(void *context, const char *line);
} sh_modserve_gate_ops;

typedef struct sh_modserve_gates {
    int needed;      /* distinct cvars the accepted bundles ask */
    int changed;     /* cvars set by this call (asked or restored) */
    int queued;      /* of them, sets that were only buffered */
    int drained;     /* 1 when the command buffer was drained between the phases */
    int live;        /* asked cvars read back at the asked value */
    int not_live;    /* needed - live */
    int restored;    /* cvars no bundle asks any more, returned to the value they displaced */
    int commands;    /* console lines appended by this call */
} sh_modserve_gates;

/* Make the accepted bundles' requirements live before a registration pass,
 * in two phases. First the deactivate lines of every bundle whose
 * requirements stopped (uninstalled, removed, dropped by a rescan, lost a
 * conflict) are appended. Phase 1 reads each asked cvar and sets it when it
 * differs; when any set was queued, the command buffer is drained before
 * phase 2. Phase 2 reads every cvar back; only the asked value read back
 * (the same text, or the same number) counts as live. A cvar the engine has
 * not, refuses or does not keep is not live, with the cause in the log and
 * in sh_modserve_requirements_report, and the steps of the bundles asking it
 * are blocked. A cvar this layer changed and no accepted bundle asks any
 * more is restored to the value it displaced, unless someone else changed it
 * meanwhile. Then the activate lines of bundles whose requirements start now
 * are appended, each logged as
 *   MODS: requirements: <id>: command "<line>"
 * Returns 0 when no cvar is asked (and when nothing is held either, ops is
 * never called), 1 when every asked cvar is live, -1 otherwise. Main thread. */
int sh_modserve_requirements_apply(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

/* sh_modserve_requirements_apply for a point where nothing is registered
 * (the Play level load): the same pass, plus one line when every asked cvar
 * was already live, so the log shows them held:
 *   MODS: play: requirements live: g_useImageBlackList=0, g_useResourceBlackList=0 (unchanged since the last pass)
 * Returns what sh_modserve_requirements_apply returns. */
int sh_modserve_requirements_check(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

/* Block the steps of bundles whose cvar requirements are not live (first
 * cause "requirement <cvar>=<value> is not live", or the conflict it lost),
 * except steps already registered. Returns how many were blocked. */
int sh_modserve_plan_block_unmet(sh_modserve_plan *plan, const sh_modserve_gates *gates);

/* sh_modserve_requirements_apply, then sh_modserve_plan_run: no
 * DeclRegisterFile is issued before the cvars are verified. Steps of a bundle
 * whose requirement is not live are blocked with that first cause (unless
 * already registered), and steps needing them fail with it. An empty plan
 * touches nothing. Returns what sh_modserve_plan_run returns. */
int sh_modserve_plan_run_gated(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                               const sh_modserve_gate_ops *gates, const char *phase, sh_modserve_gates *out);

/* One line per requirement of each admitted bundle and its state for the
 * open map ("requirements: <id>@<version>: cvar <name>=<value>: live", "not
 * active for this map (...)", "refused (<cause>)", "NOT live (<cause>; ...)",
 * "restored (...)", "not applied yet (...)"; "... activate command
 * \"<line>\": ran"; "... deactivate command ...: runs when the mod stops" or
 * "ran when it stopped"), then each
 * cvar waiting to be restored and each stopped bundle whose lines wait, or
 * one "requirements: none" line. For sh_mods status and sh_mods doctor. Any
 * thread. Returns the number of lines. */
typedef void (*sh_modserve_line_fn)(void *context, const char *line);
int sh_modserve_requirements_report(sh_modserve_line_fn fn, void *context);

/* ------------------------------------------------------------------ map scope */

/* A bundle's shadows (the stock files it replaces) and its requirements apply
 * only while a map that lists the bundle is open in the editor or playing. A
 * map lists a bundle by its manifest rows (a placed mod's row or a map-level
 * row, modmanifest.h) and by its placed names (the save-time closure). New
 * names (mods/<id>/...) are not scoped: they activate per map through the
 * plan, and a lookup of one is answered whatever map is open.
 *
 * A shadow path is admitted when any bundle that provides it is listed (or
 * always-scoped): its winner serves when the winner's bundle is listed, else
 * the first listed provider in winner order (the same bytes when another
 * install carries them, a renamed or forked copy; for different bytes the
 * conflict rules' next choice, uncomposed). A path none of whose providers is
 * listed is withdrawn: sh_modserve_lookup, sh_modserve_open and the decl
 * probe miss it, so the engine's own bytes answer, and the early-decl and
 * gated-reload visits skip it. An unlisted bundle's requirements are not accepted (sh_modserve_requirements_apply runs
 * its deactivate lines and restores its cvars), except that in a map a bundle
 * that adds Toybox rows keeps its requirements live, because the palette
 * registers its rows in every map. With no map (the hub, and at boot) no
 * map-scoped shadow is served and no map-scoped requirement is live.
 *
 * mod.json's author key "scope" chooses: "map" (the default) or "always" (the
 * shadows and requirements apply whatever map is open, as before map scope).
 * A bundle without the key whose shadows include editor-time content (a
 * generated/decls/snapeditor*, snappropertyinspector* or snappalette* decl,
 * or the editor's own movies generated/swf/snapmaps/snap_editor, entity_menu
 * and list_view) is treated as "always", logged once:
 *   MODS: scope: <id>@<version> shadows editor-time content (<request>); treated as "scope": "always" ...
 *
 * Scope is off until sh_modserve_scope_enable(1) (the product enables it in
 * sh_modserve_index_boot); with it off every shadow serves as it always did.
 *
 * When the listing changes (sh_modserve_scope_set), every shadow decl, model
 * and image whose serving bytes changed (withdrawn, admitted, or served by
 * another provider with other bytes) is marked pending
 * exactly as a changed or gone shadow is at a rescan (sh_modserve_each_refresh,
 * with `gone` set while withdrawn), and the inheritance closure is marked
 * again (sh_modserve_refresh_expand), so the refresh primitives re-parse or
 * reload them: the bundle's bytes come in, or the game's come back. */
#define SH_MODSERVE_SCOPE_MAP    0
#define SH_MODSERVE_SCOPE_ALWAYS 1

void sh_modserve_scope_enable(int on);
int  sh_modserve_scope_enabled(void);

/* Set what the current map lists: `hashes` are bundle hashes (manifest rows,
 * the placed closure; aliases resolve), `ids` bundle ids (a row whose bundle is
 * not indexed names its mod by id), `map_open` 1 while a map is open in the
 * editor or playing and 0 with none. Marks the files of the bundles that
 * changed (above) and returns how many bundles changed, shadows or
 * requirements. Any thread; serialised with index builds. */
int  sh_modserve_scope_set(const char *const *hashes, int nhashes, const char *const *ids, int nids, int map_open);

/* 1 while the last sh_modserve_scope_set said a map is open. */
int  sh_modserve_scope_map_open(void);

/* 1 when the listing can matter: scope is enabled and an admitted bundle is
 * map-scoped with shadows or requirements, or the listing is not empty. The
 * map manifest reads placed names at a load only then. */
int  sh_modserve_scope_relevant(void);

typedef struct sh_modserve_scope_info {
    int scope;               /* SH_MODSERVE_SCOPE_* in effect */
    int declared;            /* mod.json "scope": 0 absent, 1 "map", 2 "always" */
    int automatic;           /* "always" because it shadows editor-time content */
    int listed;              /* the current map lists it */
    int map_open;            /* a map is open */
    int shadows;             /* shadow files it serves */
    int shadows_live;        /* 1 its shadows are served now */
    int requirements;        /* 1 it has requirements */
    int requirements_live;   /* 1 its requirements are accepted now (whether each cvar is live is the report's) */
    char editor_path[160];   /* the first editor-time shadow, when automatic */
} sh_modserve_scope_info;

/* The scope of the admitted bundle `bundle_hash` (aliases resolve). 1 filled, 0 not admitted. */
int  sh_modserve_bundle_scope(const char *bundle_hash, sh_modserve_scope_info *out);

/* One line per admitted bundle, for sh_mods status, doctor and map:
 *   scope: <id>@<version>: map: active in the open map (3 shadows, requirements)
 *   scope: <id>@<version>: map: not active (the open map does not use it)
 *   scope: <id>@<version>: map: not active (no map is open)
 *   scope: <id>@<version>: always (mod.json)
 *   scope: <id>@<version>: always (shadows editor-time content: <request>)
 *   scope: <id>@<version>: map: new names only, nothing to scope
 * then "scope: map scope is off" when it is disabled. Returns the line count. */
int  sh_modserve_scope_report(sh_modserve_line_fn fn, void *context);

/* The points that must read the open map itself (the Play entry: Play does
 * not serialize the map) ask the bound source (modui.c) to recompute the
 * listing from the current manifest and the placed names and call
 * sh_modserve_scope_set. `when` is a SH_MODSERVE_RESTORE_* point. Returns
 * what the source returns (the bundles that changed), 0 without one. */
typedef int (*sh_modserve_scope_source_fn)(int when);
void sh_modserve_set_scope_source(sh_modserve_scope_source_fn fn);
int  sh_modserve_scope_source(int when);

/* ------------------------------------------------------------------ queries */

/* 1 installed and admitted, 0 not installed, -1 installed but skipped (the
 * first cause is copied to `cause` when non-NULL). */
int sh_modserve_bundle_state(const char *bundle_hash, char *cause, size_t cause_cap);

/* The admitted build that serves a mod: the bundle `bundle_hash` names
 * (aliases resolve), else the newest admitted bundle of `id`; when a newer
 * admitted build of the same id is installed it is that one, as the save-time
 * closure and activation pick. 1 with *out filled, 0 when none. */
typedef struct sh_modserve_bundle_ref {
    char id[96], version[160], hash[SH_MODSERVE_HASH_HEX + 1];
    unsigned long long size;   /* carried bytes, as a manifest row's size */
} sh_modserve_bundle_ref;
int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out);

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

/* A bundle's cvar requirements (for example the cut-content blacklists) are
 * applied at the first registration pass, after DOOM loaded its boot
 * resources: a model or image served before then was resolved with the old
 * values (the Spider Mastermind mesh held no materials and drew nothing).
 * Visit every served baseModel and image first opened while its bundle's
 * cvar requirements were not live, once they are live; `fn` returns 1 when it
 * reloaded the resource (not offered again). The early-decl visit above also
 * offers decls first opened before then. Returns the number visited. */
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
    int gates_live;    /* the bundle's cvar requirements are live (1 when it has none) */
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
 * on the main thread: re-checks the accepted bundles' requirements with
 * sh_modserve_requirements_check (phase "play"). The engine does not reset
 * the blacklist gates between the editor and the level load on 13954591
 * (lane MG2: each gate's int has one reader, its matcher, and no engine
 * writer; both are plain bool cvars that no shipped config names), so this
 * normally logs "requirements live"; a cvar someone changed back is set
 * again before the level's decls load. No-op when nothing is asked or held
 * (sh_modserve_requirements_held). */
void sh_modserve_play_begin(void);

/* A scope change (sh_modserve_scope_set) brought into the running game, at
 * a map load, the map exit, the Play entry and in the editor after a save or
 * a map-level change: the requirements pass first (stopped bundles' cvars
 * restored and deactivate lines run, started ones set and their activate
 * lines run), then the pending re-parses and reloads (me_refresh_apply) and
 * the gated reloads. Main thread. Logs one "MODS: scope: <phase>: ..." line.
 * Returns 1 applied, 0 nothing pending, -1 refused (off the main thread). */
int sh_modserve_scope_apply(const char *phase);

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
