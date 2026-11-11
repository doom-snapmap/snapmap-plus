/* modserve.h -- installed mod bundles: the boot-time index, serving through
 * the resource provider, decl registration for editor rows and per-map
 * gameplay activation.
 *
 * Layout read (ONE-SHAPE rules 1 to 4, written by the builder and the store,
 * never here):
 *   <data>\mods\<folder>\mod.json       the author block and the builder block
 *                                       ("snapmap-plus": format 2, salt, build, files, shadows)
 *   <data>\mods\<folder>\assets\...     the files at engine paths
 *   <data>\mods\maps\<segment>\<mod>\   a map's place: the builds a downloaded map,
 *                                       a rawmap or a prefab ZIP brought, served for that map
 * A mod's identity is its salt (its "id" everywhere below): eight characters
 * in the builder block, never the folder name, which is the author's. The
 * root of mods\ (everything but mods\maps\) holds one build per salt, and
 * each place one per salt. A folder in an earlier shape, one not built yet
 * and one whose build failed serve nothing; the cause is logged and listed.
 * <data> is %LOCALAPPDATA%\snapmap-plus.
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
#define SH_MODSERVE_CACHE_FILE  (1u << 20)       /* files below this are cached */
#define SH_MODSERVE_CACHE_TOTAL (8u << 20)       /* the cache's total byte bound */

/* ------------------------------------------------------------------ index */

/* Build the table from `mods_root` (the folder that holds the bundle folders,
 * spelled in UTF-8 as sh_ms_root spells it) and publish it, replacing any
 * earlier table. Logs one MODS: line per skipped bundle and per conflict, then
 * "MODS: index built in N ms (B bundles, F files)". Returns the number of
 * bundles admitted, or -1 when the table could not be allocated (the previous
 * table, if any, stays published). A missing folder is an empty index. */
int sh_modserve_index_build(const char *mods_root);

/* The same for the store's root (sh_ms_root, modstore.h). Called once by the
 * bootstrap before the provider hook is installed. */
int sh_modserve_index_boot(void);

/* Rebuild from the folder of the last build (after an install or a fetch).
 * Decls already registered stay registered; editor rows of new bundles need
 * modserve_palette_rebuild_requested. */
int sh_modserve_index_rescan(void);

/* Free the tables two or more builds behind the published one, when no
 * provider call is in flight on any thread and no plan is alive. A new table
 * carries what later questions need from the one it replaces (the salts
 * registered this session, which bytes the engine already read, the removed
 * paths a refresh still owes), so nothing reads an older table after its
 * build. Every string the index hands out (a lookup hit, a bundle's salt,
 * hash or extra text) therefore stays valid until the second table built
 * after the one that gave it is published and this runs; a consumer that
 * keeps one longer copies it. Main thread, at the start of a registration
 * pass (no product call holds an older pointer there). Returns the tables
 * freed and logs "MODS: index: N earlier table(s) freed". */
int sh_modserve_index_retire(void);

/* Remove from the mods folder's served copies (banks renamed for their hash,
 * rewritten texts) every file the published table does not serve from, and
 * any temporary a crashed write left. Each copy is written beside its name
 * and replaces it whole. Run once at boot (sh_modserve_index_boot); skipped,
 * with one line, while another game instance on this account uses the
 * folder. Returns the files removed. */
int sh_modserve_served_prune(void);

/* Nonzero when the published table serves at least one path. */
int sh_modserve_active(void);

/* Where the backend learns what a map lists (the map scope's hook and
 * source, modmanifest.h and modui.c): a load, a save, the Play entry. */
#define SH_MODSERVE_AT_LOAD 1
#define SH_MODSERVE_AT_SAVE 2
#define SH_MODSERVE_AT_PLAY 3

/* ------------------------------------------------------------------ serve */

/* One lookup result. Pointers stay valid while their table lives
 * (sh_modserve_index_retire). */
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

/* Bank stems of their own : every
 * carried .bnk of a bundle is served as sound/soundbanks/pc/<stem>.<sha16>.bnk,
 * <sha16> the first 16 hex characters of the bank file's own SHA-256, its BKHD
 * bank id rewritten to the FNV-1 of that stem, and every quoted "<stem>",
 * "<stem>.bnk" or ".../<stem>.bnk" in the bundle's served decl texts is
 * rewritten to match. The rewritten bytes are written once under
 * <mods>\.served\, keyed by their own hash (banks\<stem>.<sha16>.bnk,
 * files\<sha16>.<extension>), and served from there. So a changed bank is a
 * new bank to Wwise, two builds carrying one bank's bytes share it, and the
 * ledger and refresh see a new name. */
/* `text` with every quoted "<stem>", "<stem>.bnk" or ".../<stem>.bnk" (ASCII
 * case-insensitive) spelled with `renamed` instead of `stem`. Malloc'd, or
 * NULL when nothing changed. Pure. */
char *sh_modserve_bank_rename_text(const char *text, size_t len, const char *stem, const char *renamed, size_t *out_len);
/* Rewrite a bank's BKHD id to FNV-1 of `stem` in place. 1 done, 0 not a BKHD bank. Pure. */
int  sh_modserve_bank_patch_id(unsigned char *bytes, size_t len, const char *stem);

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
/* A step's rank, the order among steps no need orders. A renderParm comes
 * first: the engine gives a renderParm its slot and kind at its first parse
 * and keeps them on a reparse, so a material or program that resolves the
 * parm before the renderParm's own parse binds a slot whose kind the parse
 * then changes (the render job read floats as a sampler
 * pointer). A material's parm keys are bare names, so no need orders them. */
#define SH_MODSERVE_RANK_RENDERPARM 0
#define SH_MODSERVE_RANK_OTHER      1
#define SH_MODSERVE_RANK_MD6        2
#define SH_MODSERVE_RANK_ENTITYDEF  3
#define SH_MODSERVE_RANK_SEDEF      4
#define SH_MODSERVE_RANK_MAX        4

typedef struct sh_modserve_step {
    const char *type;        /* engine spelling, e.g. "md6Def" */
    const char *name;        /* decl name, e.g. "mods/alex.pink-imp/imp.md6" */
    const char *reg_path;    /* DeclRegisterFile source, e.g. "md6def/mods/.../imp.md6.decl" */
    const char *bundle_id;
    int         rank;        /* SH_MODSERVE_RANK_*: 0 renderParm, 1 other decls, 2 md6Def, 3 entityDef,
                                4 snapEditorEntityDef */
    int         persist;     /* 1: keep through map purges (lifetime 4), every editor-plan step; 2: in
                                any other plan, a decl of the editor closure (modserve.c
                                ms_mark_editor_closure), kept unless the engine already holds it
                                permanently, which is left to the game as a map pass leaves it */
    int         shadow;      /* always 0: shadows are served by the provider, never planned. A surface (a stock pointer at the game's own path, modbundle.h
                                sh_mb_stock_restore) is never a step of any plan. */
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
/* `load` may also answer SH_MODSERVE_LOAD_LEFT: the decl is permanent
 * (sh_modserve_load_left_to_engine), so the pass leaves it to the game, which
 * reads the bundle's bytes through the provider whenever it parses the decl.
 * Counted as ready. */
#define SH_MODSERVE_LOAD_LEFT 3
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
    /* Optional: 1 when the engine already holds a resource of this type and
     * name, with its state byte (+0x2c) and lifetime (+0x28); 0 when it holds
     * none; -1 when it cannot tell. A pure lookup: nothing is created, loaded
     * or parsed. DeclRegisterFile re-parses an existing resource of the name
     * it registers on the spot (VK FUN_141559a30 -> FUN_141561af0 ->
     * DeclParseSource), so the registration phase asks this first and leaves
     * a decl to the game the way `load` does (sh_modserve_load_left_to_engine). */
    int (*held)(void *context, const char *type, const char *name, unsigned *state, unsigned *lifetime);
} sh_modserve_engine_ops;

/* The console sink for the lines a player should see when a pass skips or
 * refuses a decl (production: the DOOM console). NULL turns it off. */
typedef void (*sh_modserve_console_fn)(const char *line);
void sh_modserve_set_console(sh_modserve_console_fn fn);

/* The store's memory of the last refused build of a folder (absolute path),
 * so a folder without a product block reports why its build was refused
 * rather than "not built yet": the cause, malloc'd, or NULL. NULL turns it off. */
typedef char *(*sh_modserve_build_cause_fn)(const char *folder);
void sh_modserve_set_build_cause(sh_modserve_build_cause_fn fn);

/* The model check for the served animWeb at `request`, as a load now would
 * meet it: 1 with the first model its models or modelInfos block names that
 * no bundle serves and the engine cannot find (written to `model`), 0 when
 * every model resolves or the check cannot tell, -1 when `request` is not a
 * served animWeb. */
int sh_modserve_animweb_check(const char *request, const sh_modserve_engine_ops *ops, char **model);

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

/* 1 when a pass that does not keep its decls (persist 0, the map pass) must
 * not load the resource with level `lifetime` (+0x28) and state byte `state`
 * (+0x2c): any permanent decl (level 4) that has not failed (state bit 0),
 * parsed or only created by reference. Parsing it binds it to the decls it
 * names, and inside a map pass those can be objects the pass created at level
 * 1, so the next map unload frees them while the permanent decl keeps the
 * pointers. The menu collectibles' unlockModel 'spidermastermind', parsed at
 * boot, was re-parsed by the map pass after the pass had created
 * spider_mastermind.md6 at level 1, and SetupModelList faulted on the freed
 * md6Def at the next shell map load (VK RVA 0x33a130). Nothing is lost by
 * leaving it: a permanent decl was parsed through the provider, or by the
 * early re-parse and the refresh, which already serve it the bundle's bytes. */
int sh_modserve_load_left_to_engine(unsigned lifetime, unsigned state, int persist);

/* Run a plan: first DeclRegisterFile for every unblocked step whose source is
 * not registered yet (registration publishes the source, so a decl that names
 * another during its parse finds it; when the engine already holds a resource
 * of that name it also re-parses it at once), then the loads in plan order.
 * A step the engine holds as a permanent decl is not registered by a pass that
 * does not keep its decls (ops->held, sh_modserve_load_left_to_engine): the
 * re-parse inside the registration binds it to objects the pass creates at
 * level 1, which is how the menu collectible 'spidermastermind' came to point
 * at a map-scoped spider_mastermind.md6 (observed with a hardware
 * watchpoint on the unlockModel's md6Def field).
 * Steps already registered by an earlier plan are skipped. One MODS: line per
 * step with the result and the state byte. Returns the number of steps that
 * ended ready (registered now or earlier), or -1 when ops is unusable. */
int sh_modserve_plan_run(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                         const char *phase);

/* The plan's steps no earlier run settled ready (registered, or left to the
 * game) and that have not failed or been withdrawn: what a pass of the same
 * steps would still do. 0 when another pass already did all of it, as a hub
 * launch's gate does before its level load (sh_modserve_launch_preload). */
int sh_modserve_plan_unvisited(const sh_modserve_plan *plan);

/* The decls an editor row points at directly, which must live as long as the
 * row does. A snapEditorEntityDef is kept for the session (lifetime 4) and its
 * parse stores pointers to the decls its edit block names; a decl the engine
 * created for one of those names after boot sits at level 1, and the purge at
 * the next map unload (entering Play, and leaving it) frees the object while
 * the row still points at it. The editor entity rebuilt after Play then binds
 * the freed animWeb (AV in the animator's node set-up, VK
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
 * loaded the row (the second site, idJointAnimator's joint blend
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
/* 1 when `resource` existed when `w` was taken. */
int  sh_modserve_watermark_has(const sh_modserve_watermark *w, const void *resource);

/* Every resource a level-4 decl of a served bundle holds is kept with it:
 * from each decl of an admitted, non-surface file the engine holds at level
 * 4, the needs the builder recorded (an animWeb's clips and md6Defs, an
 * md6Def's mesh and clips, an entityDef's models and md6Defs), closed through
 * every file of the index that provides one, are looked up by type (a list's
 * +0x08) and name (folded) and raised from level 1 to 4. A watermark keeps
 * only what a pass creates; a decl kept by a pass may have been parsed by
 * the engine's own map load before it, and what that parse created is older
 * than the watermark. Run after every serving pass and before
 * the Play level load; a named resource the engine creates later is raised
 * by the next run. Returns 1 when both walks completed. */
typedef struct sh_modserve_holds {
    unsigned roots;       /* decls of served bundles the engine holds at level 4 */
    unsigned names;       /* distinct (type, name) needs of those */
    unsigned raised;      /* resources raised from level 1 to 4 */
    unsigned lists;       /* lists walked */
    char first[300];      /* the raised names, first ones */
} sh_modserve_holds;
int  sh_modserve_holds_raise(const void *head_slot, sh_modserve_holds *out);

/* The render models of that same closure the product loads itself, in
 * process heap 0, so that they are held for the session with the decls that
 * hold them: a mod prop's model is not loaded by its entityDef's parse but at
 * the entity's spawn, in the map's heap, and the Play purge frees it; the
 * next Play of a map that does not declare the bundle (whose files it then
 * does not serve) loads it again from the kept decl and gets the engine's
 * default box, which no editor map change purges. For each
 * "model" need whose providing file the index serves in the current scope
 * and which the lists hold not at all, as a default (+0x2c bit 0 or 2), or at
 * level 1 loaded under a map heap: fn(context, name, resource or NULL),
 * outside the resource lock; fn returns 0 to stop. A stock model (no
 * provider) is the game's bytes in every scope and is left to the engine.
 * Returns the number of calls, -1 when a walk failed. */
#define SH_MODSERVE_HELD_MODEL_TYPE "model"
typedef int (*sh_modserve_held_model_fn)(void *context, const char *name, void *resource);
int  sh_modserve_held_models(const void *head_slot, sh_modserve_held_model_fn fn, void *context,
                             sh_modserve_holds *out);
/* The resource list whose type name (+0x08) is `type`, or NULL. */
void *sh_modserve_resource_list(const void *head_slot, const char *type);

/* What a kept pass looks up. A pass that registers while the previous map's
 * level-1 objects are still alive (an in-editor rawmap reload, `sh_mods map
 * add`) parses decls that bind to those objects: they are older than its
 * watermark and no recorded need names them (an entityDef records its own
 * md6Def, not its attachments), so the Play purge freed them under the kept
 * decl. While a watermark is open the engine side records every
 * resource the engine's ResourceLookup returns on the main thread
 * (`note`); `close` raises each one the lists still hold at level 1 to 4,
 * under the resource lock, and empties the set. Opened at the outermost
 * watermark, closed at its end; `note` outside an open pass does nothing. */
typedef struct sh_modserve_lookups {
    unsigned looked_up;   /* distinct resources the pass looked up */
    unsigned live;        /* of those, still on a list at the close */
    unsigned raised;      /* raised from level 1 to 4 */
    unsigned lists;       /* lists walked */
    int overflow;         /* the set reached its bound; later lookups were not recorded */
    char first[300];      /* the raised names, first ones */
} sh_modserve_lookups;
void sh_modserve_lookups_open(void);
int  sh_modserve_lookups_recording(void);
void sh_modserve_lookups_note(const void *resource);
int  sh_modserve_lookups_close(const void *head_slot, sh_modserve_lookups *out);

/* What the engine looks up on behalf of a kept object is kept with it. The
 * kept set holds every resource the product keeps (a pass's watermark, the
 * holds by name, a pass's lookups, an editor row's decl) by address. The
 * engine side tracks the engine's load nesting on the main thread (a detour
 * on ResourceGenericLoad) and reports each load it opens inside another and
 * each resource ResourceLookup returns inside a load (`note`): the pair is
 * recorded, and when the parent is kept the child joins the kept set at once
 * (`keep` raises it from level 1 to 4 when its load returns). A Play's spawns
 * load objects no kept decl held yet (the chainsaw gore md6Defs, their meshes
 * and the meshes' materials); a later pass keeps the md6Defs by
 * name, and `close` then follows the recorded pairs from every kept resource,
 * transitively, so what they loaded is kept too. The close walks the lists
 * once, drops kept and recorded addresses no list holds any longer, raises
 * every listed kept resource still at level 1 to 4 under the resource lock,
 * and keeps only the pairs a later keep can still follow (a parent neither
 * kept nor at level 4); with `forget` set (before the Play level load's
 * purge, which frees the level-1 parents they name) it forgets them all.
 * Both sets are bounded; a full set stops recording and the close says so. */
typedef struct sh_modserve_attrib {
    unsigned kept;        /* kept resources still listed after the close */
    unsigned pairs;       /* recorded (parent, child) pairs before the close */
    unsigned followed;    /* resources the pairs added to the kept set in this close */
    unsigned raised;      /* raised from level 1 to 4 by this close */
    unsigned raised_at_load; /* raised by `keep` since the previous close */
    unsigned lists;       /* lists walked */
    int overflow;         /* a set reached its bound since the previous close */
    char first[300];      /* the names this close raised, first ones */
} sh_modserve_attrib;
/* The keepability rule. Nothing raises a resource to level 4 except
 * sh_modserve_keep, and it raises only what sh_modserve_keepable allows: a
 * resource whose last load ran with a map's heap selected is never kept,
 * since raising its level keeps the object but not what that heap held for it
 * (the next Play load frees the heap). `keepable` counts and names each
 * refusal for sh_modserve_left_to_map. `keep` raises `resource` to 4 and adds
 * it to the kept set; 1 kept (or already at 4), 0 left at the engine's level.
 * Main thread. */
int  sh_modserve_keepable(const void *resource);
int  sh_modserve_keep(void *resource);
void sh_modserve_kept_add(const void *resource);
int  sh_modserve_kept_has(const void *resource);
int  sh_modserve_attrib_note(const void *parent, const void *child);
int  sh_modserve_attrib_keep(void *resource);
/* The load nesting the engine side's detour reports, main thread only:
 * `enter` before a load (notes the pair with the innermost open load; *kept
 * says whether the parent was kept) returns the depth to hand back to
 * `leave`, which the detour calls however the load leaves; `found` notes a
 * resource the lookup returned inside the innermost load and keeps it when
 * that load is kept. The engine stops at 32 nested loads (ResourceMaterialize,
 * "Maximum resource recursion level"); past SH_MODSERVE_NEST_MAX nothing is
 * attributed and the depth is still counted. `heap` is the heap the load
 * allocates from (the engine's heap stack top, 0 the process heap, -1
 * unknown): a resource loaded with another heap selected is never raised by
 * any keep (watermark, holds, lookups, attribution), since raising keeps its
 * object but not what the map's heap held for it; `left_to_map` returns and
 * clears how many keeps left one at level 1, with their first names. */
#define SH_MODSERVE_NEST_MAX 64
int  sh_modserve_attrib_enter(void *resource, int heap, int *kept);
void sh_modserve_attrib_leave(int depth);
int  sh_modserve_attrib_depth(void);
void sh_modserve_attrib_found(void *resource);
unsigned sh_modserve_left_to_map(char *names, size_t cap);
int  sh_modserve_attrib_close(const void *head_slot, int forget, sh_modserve_attrib *out);
/* Tests: forget everything. */
void sh_modserve_attrib_reset(void);

/* Editor rows at map load. The palette is built once per process,
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
 *   MODS: requirements: g_useResourceBlackList=0 for <mod name>
 *   MODS: requirements: 1 activate and 1 deactivate command(s) for <mod name>
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
    int (*read)(void *context, const char *cvar, char **value);   /* *value: malloc'd, the whole value */
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
 * open map ("requirements: <mod name>: cvar <name>=<value>: live", "not
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

/* Two layers: while a map is open or played, the
 * bundles it declares serve, then the game. A map declares a bundle by the
 * hash of its manifest rows (a placed mod's row or a map-level row,
 * modmanifest.h) and of its placed names (the save-time closure); the first
 * hash of a salt in row order declares that salt, and every other build of
 * the salt is inert for the map. A hash names the root's folder at that
 * build, else the folder of the loaded map's place (sh_modserve_place_set).
 *
 * Per path, the file that serves it for the open map is chosen among the
 * providers the map declares: the fewest needs the declared bundles do not
 * provide, then the bytes most declared providers carry, then the order of the
 * map's rows. When every declared provider's text is a decl that differs only
 * inside lists or bags, their entries are composed (modserve.c). Nothing about
 * the install counts: the result is the same for any install order and any
 * other local mods.
 *
 * A bundle the map does not declare: in the editor (SH_MODSERVE_MODE_EDITOR)
 * and with no map open, the names under its salt serve (the Toybox offers its
 * rows; placing one declares it at the next save) for the root folder's build
 * only, and its surfaces (the game's own bytes at the game's name) serve;
 * its shadows never. A build in a map's place serves only for a map that
 * declares it. In Play and in a map launched outside the editor
 * (SH_MODSERVE_MODE_PLAY) it serves nothing. A bundle whose shadows include
 * editor-time content (a generated/decls/snapeditor*, snappropertyinspector*
 * or snappalette* decl, or the editor's own movies) is the editor-tweak class:
 * its editor tweaks serve in every map, logged once:
 *   MODS: scope: <mod name> shadows editor-time content (<request>); its editor tweaks serve in every map ...
 *
 * Requirements: a declared bundle's apply; an undeclared bundle that adds
 * Toybox rows (its root folder's build) keeps them live outside Play, because the
 * palette registers its rows in every editor map and with none open (in Play
 * no undeclared bundle's are accepted); any other undeclared
 * bundle's are not accepted (sh_modserve_requirements_apply runs its
 * deactivate lines and restores its cvars).
 *
 * Scope is off until sh_modserve_scope_enable(1) (the product enables it in
 * sh_modserve_index_boot); with it off every bundle serves everything, by the
 * static winner rules (sh_modserve_conflict).
 *
 * When the listing or the mode changes, every shadow decl, model and image
 * whose serving bytes changed (withdrawn, admitted, or served by another
 * provider with other bytes) is marked pending exactly as a changed or gone
 * shadow is at a rescan (sh_modserve_each_refresh, with `gone` set while
 * withdrawn), and so is every name another build of its salt now serves with
 * other bytes than the ones last served; the inheritance closure is marked
 * again (sh_modserve_refresh_expand). A withdrawn name is never marked: the
 * engine keeps what it parsed until a map serves the name again. */
#define SH_MODSERVE_SCOPE_MAP    0
#define SH_MODSERVE_SCOPE_ALWAYS 1

#define SH_MODSERVE_MODE_EDITOR  0   /* the editor, the hub, no map: undeclared bundles offer their names */
#define SH_MODSERVE_MODE_PLAY    1   /* Play, a published or My Maps launch: undeclared bundles serve nothing */

void sh_modserve_scope_enable(int on);
int  sh_modserve_scope_enabled(void);

/* Set what the current map declares: `hashes` are the hashes of the builds
 * that serve its rows (sh_modserve_row_build) in row order, then the placed
 * closure (a salt alone declares nothing), `map_open` 1 while a
 * map is open in the editor or playing and 0 with none (0 also returns the
 * mode to SH_MODSERVE_MODE_EDITOR). Marks the files whose served bytes
 * changed (above) and returns how many bundles changed what they serve or
 * their requirements. Any thread; serialised with index builds. */
int  sh_modserve_scope_set(const char *const *hashes, int nhashes, int map_open);

/* Where the loaded map finds its mods (DECISIONS 1 and 11): its place.
 *
 * A place is "" (the root of mods\: the player's own maps) or
 * "maps\<segment>" (mods\maps\<segment>\, a map that arrived with its mods:
 * a download, a rawmap ZIP import). The place is a fact of the map: the
 * saved map records it in its member ("place", map_carrier.h; written by
 * every save, rawmap.c), and every load sets it from that record
 * (sh_modserve_place_load at each parse), after which the published gates
 * set a downloaded map's place from its item (modpublished.c). There is no
 * other global setting: the editor's exit and the end of a launched Play
 * return it to the root.
 *
 * The one resolver. A row "<salt>@<build hash>" of the loaded map resolves to
 *   1. the root folder of the salt at the row's build hash; else
 *   2. with a place: that place's folder of the salt at the row's hash;
 *      without one: the root folder of the salt at any build.
 * Never another map's place. A bare hash (the listing, plans, bundle state)
 * resolves the same way: the root's folder at the hash, else the place's.
 * sh_modserve_row_build and sh_modserve_row_served use the loaded map's
 * place; the _in forms take a place and change nothing (a gate counting a
 * map's rows before that map loads).
 *
 * sh_modserve_place_norm: the place as stored, malloc'd ("" or
 *   "maps\<segment>"; "maps/<segment>" and a bare segment are accepted),
 *   NULL when `place` is not one: the segment must be exactly what
 *   sh_ms_title_segment makes of it (no separator, no dot step, no
 *   character Windows refuses), so a hostile map cannot point outside
 *   mods\maps\.
 * sh_modserve_place_set: make `place` the loaded map's (a gate, a launch).
 *   Returns the bundles whose serving changed, or -1 when it is not a place
 *   (nothing changes).
 * sh_modserve_place_load: a map parse's place (an invalid one is the root),
 *   remembering the place it replaces; sh_modserve_place_restore puts that
 *   back (a parse that failed, a launch that was refused) and returns 1, or
 *   0 when nothing is remembered.
 * sh_modserve_place_get: the loaded map's place (malloc'd; NULL when memory
 *   runs out).
 * Any thread. */
#define SH_MODSERVE_PLACES    "maps"   /* the places area under the store root */
/* The cause of a folder in a map's place that the product did not receive:
 * a place holds only the builds a map brought, and nothing there is built. */
#define SH_MODSERVE_PLACE_FRESH \
    "This folder sits among a downloaded map's mods but did not come with the map. Move it out of the maps folder " \
    "into your own mods and press Refresh."

char *sh_modserve_place_norm(const char *place);
int  sh_modserve_place_set(const char *place);
int  sh_modserve_place_load(const char *place);
int  sh_modserve_place_restore(void);
char *sh_modserve_place_get(void);

/* Set the mode (SH_MODSERVE_MODE_*): the Play entry sets PLAY
 * (sh_modserve_scope_source with SH_MODSERVE_AT_PLAY), a map load sets PLAY
 * until the editor shows the map (modui.c), the editor's tick sets EDITOR.
 * Marks as sh_modserve_scope_set does and returns the bundles that changed. */
int  sh_modserve_scope_mode(int mode);
int  sh_modserve_scope_mode_get(void);

/* 1 while the last sh_modserve_scope_set said a map is open. */
int  sh_modserve_scope_map_open(void);

/* 1 when the listing can matter: scope is enabled and an admitted bundle is
 * map-scoped with shadows or requirements, or the listing is not empty. The
 * map manifest reads placed names at a load only then. */
int  sh_modserve_scope_relevant(void);

typedef struct sh_modserve_scope_info {
    int scope;               /* SH_MODSERVE_SCOPE_* in effect */
    int automatic;           /* "always" because it shadows editor-time content */
    int listed;              /* the current map declares it (this build) */
    int rank;                /* its position among the declared bundles (0 the first row), or -1 */
    int inert;               /* the current map declares another build of its salt */
    int map_open;            /* a map is open */
    int play;                /* the open map is played (SH_MODSERVE_MODE_PLAY) */
    int newest;              /* the root folder's build of its salt: it serves with no map open */
    int in_place;            /* it sits in a map's place (mods\maps\<segment>) */
    int names;               /* files under its salt (new names) */
    int shadows;             /* shadow files it serves */
    int surfaces;            /* surfaces (the game's bytes at the game's name) */
    int names_live, shadows_live, surfaces_live;   /* 1 each class is served now */
    int requirements;        /* 1 it has requirements */
    int requirements_live;   /* 1 its requirements are accepted now (whether each cvar is live is the report's) */
    const char *editor_path; /* the first editor-time shadow, when automatic (the index's; NULL when none) */
} sh_modserve_scope_info;

/* The scope of the admitted bundle `bundle_hash` (in the open map's context). 1 filled, 0 not admitted. */
int  sh_modserve_bundle_scope(const char *bundle_hash, sh_modserve_scope_info *out);

/* A mod the open map uses, named as a player names it: its name (mod.json
 * "name"), its folder, the folder's last part, or its salt, ASCII
 * case-insensitive. 1 with its salt and the name lines use (*label,
 * malloc'd: its name, else its folder), 0 when the open map uses no such mod. */
int  sh_modserve_listed_mod(const char *name, char *salt, size_t salt_cap, char **label);

/* 1 when the admitted bundle `bundle_hash`'s edits of game content apply now:
 * map scope is off, the open map declares it, or it is the editor-tweak class
 * (its editor tweaks serve in every map); else 0. A mod's string edits
 * (strkeys.h, strids.c) follow this. */
int  sh_modserve_bundle_edits_live(const char *bundle_hash);

/* For sh_mods status, doctor and map: one header per mod installed at several
 * builds, then one line per admitted bundle with what it serves for the open
 * map. A mod is named by its name (else its folder), never by its salt or a
 * hash:
 *   scope: mod <mod name>: 2 builds installed (the root folder and maps' places); ...
 *   scope: <mod name> (N names, N shadows, N surfaces[, requirements]): <state>
 * where the state is "declared by the open map (row N): serves everything it
 * carries", "inert: the open map declares another build of its salt", "not
 * declared by the map being played: serves nothing", "not declared (no map is
 * open): its names serve for the Toybox ...", "editor-time content (...)" or
 * "map scope is off: ..."; then, per path several bundles provide, which
 * layer serves it and why (at most 40 lines), and one count line:
 *   serving: <request>: <mod name>, declared (row N): <why>; not served: <others>
 *   serving: <request>: the game (no declared bundle provides it); not served: <providers>
 *   serving: N path(s) from bundles the open map declares, M from undeclared bundles (...), K left to the game
 * Returns the line count. */
int  sh_modserve_scope_report(sh_modserve_line_fn fn, void *context);

/* Registries (modregistry.h): the open map's registry choices, the listing's
 * second input (the map manifest gives them at every load, before the
 * listing, and at every choice). Slots and keys are copied; with `recompute`
 * the listing is recomputed and marked as sh_modserve_scope_set does, and the
 * bundles whose serving changed are returned (a change of generated text
 * counts one). Cleared when a listing with no map open is set. */
int  sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute);
/* One line per registry path ("registry <request>: generated, N bytes" or
 * "the game's own file"), then what was generated and every clash
 * ("registry: conductor: aiType 524288 -> ...", "registry: <game file>/<path>: set
 * by ...; serving ..."), then the choices. Returns the lines. */
int  sh_modserve_registry_report(sh_modserve_line_fn fn, void *context);

/* The points that must read the open map itself (the Play entry: Play does
 * not serialize the map) ask the bound source (modui.c) to recompute the
 * listing from the current manifest and the placed names and call
 * sh_modserve_scope_set. `when` is a SH_MODSERVE_AT_* point. Returns
 * what the source returns (the bundles that changed), 0 without one. */
typedef int (*sh_modserve_scope_source_fn)(int when);
void sh_modserve_set_scope_source(sh_modserve_scope_source_fn fn);
int  sh_modserve_scope_source(int when);

/* ------------------------------------------------------------------ queries */

/* 1 installed and admitted, 0 not installed, -1 installed but skipped (the
 * first cause, malloc'd, in *cause when cause is not NULL). */
int sh_modserve_bundle_state(const char *bundle_hash, char **cause);

/* The asset rule. A map, rawmap or prefab needs the builds its manifest rows
 * name. A row is SERVED when the build sh_modserve_row_build names for it is
 * admitted and every name the row lists resolves, in that build or through
 * the serving index; MISSING when it names none (a stranger's map whose
 * place and root do not hold the row's build: the hold fetches it into the
 * map's place); INCOMPLETE when the build is admitted but a listed name does
 * not resolve. *cause (when cause is not NULL; malloc'd) gets the sentence a refusal shows,
 * for a MISSING row "This map uses Hell Guards, which is not installed." or
 * "... which is installed but cannot be used: <cause>." (a folder of that
 * salt was skipped), and for an INCOMPLETE one "Hell Guards is installed but
 * does not provide <the first three names>[ and N more], which this map
 * uses."; `subject` replaces "map" ("rawmap", "prefab"). The title is the
 * installed folder's name, else the row's, else the salt. Lock-free; any
 * thread. */
#define SH_MODSERVE_ROW_MISSING    0
#define SH_MODSERVE_ROW_SERVED     1
#define SH_MODSERVE_ROW_INCOMPLETE 2
struct sh_modmanifest_row;
int sh_modserve_row_served(const struct sh_modmanifest_row *row, char **cause);

int sh_modserve_row_served_for(const struct sh_modmanifest_row *row, const char *subject, char **cause);
int sh_modserve_row_served_in(const struct sh_modmanifest_row *row, const char *place, const char *subject,
                              char **cause);

/* The build that serves a row now: the one place resolver above, for the
 * loaded map's place, or (sh_modserve_row_build_in) for `place`. 1 with *out
 * filled, 0 when none (the row is missing and a hold fetches it). `id` is the
 * salt. */
typedef struct sh_modserve_bundle_ref {
    char id[96], hash[SH_MODSERVE_HASH_HEX + 1];
    char *name;                        /* the author's name, "" when none (malloc'd) */
    char *folder;                      /* its folder below the store root (malloc'd) */
    char *place;                       /* "maps\<segment>" for a place's folder, "" at the root (malloc'd) */
    unsigned long long size;           /* carried bytes, as a manifest row's size */
} sh_modserve_bundle_ref;
/* Filled refs hold malloc'd text: release each with sh_modserve_bundle_ref_free. */
int sh_modserve_row_build(const struct sh_modmanifest_row *row, sh_modserve_bundle_ref *out);

/* A published map that uses part of a mod (modslice.h) carries only the
 * files its cut found it needs. A file the game then asks for that the cut
 * left out is a defect of the cut: the map plays on if the game can, and
 * the asked name is recorded. The provider (overrides.c) calls
 * sh_modserve_note_miss with every request it found nowhere; one under
 * mods/<salt>/ of a mod the open map declares by a cut is noted once per map,
 * with the name of the load that asked for it when the main thread's open
 * loads say (`by`, else ""), logged:
 *   MODS: <mod name> came with this map without <request>, which <by> asked for; the map goes on without it
 * and handed to the bound reporter (modpublished.c: the store keeps it for
 * the map's author, who sees it beside the published map and corrects it by
 * publishing again). Returns 1 when noted now, 0 otherwise. Any thread. */
typedef void (*sh_modserve_miss_fn)(const char *salt, const char *cut_hash, const char *request, const char *by);
void sh_modserve_set_miss_reporter(sh_modserve_miss_fn fn);
int  sh_modserve_note_miss(const char *request);
/* The misses noted since the process started: one "<mod name>: <request>"
 * line each (sh_mods status). Returns the count. */
int  sh_modserve_misses_report(sh_modserve_line_fn fn, void *context);
int sh_modserve_row_build_in(const struct sh_modmanifest_row *row, const char *place, sh_modserve_bundle_ref *out);
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref);

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
typedef void (*sh_modserve_closure_fn)(void *context, const char *bundle_id, const char *bundle_hash,
                                       unsigned long long size, const char *name);
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context);

/* A path several admitted bundles of different salts provide with different
 * bytes. With map scope on, which provider serves it is decided per map
 * among the declared providers (the map scope rules above; `served_*`, `game`,
 * `composed` and `reason` describe the open map). The static winner, which
 * serves with map scope off, is the provider with the fewest needs no
 * admitted bundle provides, then the bytes most providers carry, then the salt
 * and hash in name order (nothing about the install counts). When every
 * provider's text is a decl that differs only inside lists (a block of
 * `num = N;` and `item[i]` members, such as the encounter conductor's
 * aiTypeList or a whitelist inspector's validEncounters), the winner's text
 * is served with the other providers' list entries appended (composed). One
 * MODS: line per conflict at the index build names the providers, the static
 * winner and why:
 *   MODS: conflict: shadow '<path>' is provided by A, B; serving A (<reason>)
 *   MODS: conflict: shadow '<path>' is provided by ...; serving A's text with the others' list entries (N added)
 * Pointers stay valid while their table lives (sh_modserve_index_retire). */
typedef struct sh_modserve_conflict {
    const char *request, *providers, *reason;
    const char *served_id, *served_hash;
    int count;        /* providers */
    int composed;     /* 1: list entries of every provider merged (the declared ones, with map scope on) */
    int withdrawn;    /* 1: a registration pass withdrew the winner (see plan_run) */
    int game;         /* 1: the open map declares none of its providers; the game's bytes answer */
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

/* Visit every scanned bundle (admitted or skipped): its salt (the folder
 * when it has none), and `place` 1 for a folder in a map's place. */
typedef void (*sh_modserve_bundle_fn)(void *context, const char *id,
                                      const char *hash, int admitted, const char *cause,
                                      int files, int place);
void sh_modserve_each_bundle(sh_modserve_bundle_fn fn, void *context);
/* The bundle at `bundle_hash` as a status line names it (malloc'd): its name
 * and folder ("<name> (mods\<folder>)"), else its folder; "a mod" when no
 * bundle has that hash. */
char *sh_modserve_bundle_label(const char *bundle_hash);

/* Visit every admitted bundle whose mod.json author block carries keys the
 * product does not read itself (the manifest's "extra", such as "strings" and
 * "hud"), in a fixed order: root folders before maps' places, then by salt,
 * then by folder; `place` is 1 for a folder in a map's place. `extra` is the object's JSON
 * text; the strings stay valid while their table lives. Returns the number visited,
 * or -1 when the order could not be allocated (nothing is visited). Used by
 * strids.c for the bundle string layer. */
typedef void (*sh_modserve_extra_fn)(void *context, const char *id, const char *hash, int place, const char *extra);
int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context);

/* Visit every admitted bundle in index order with its salt, its hash and its
 * product block's "registries" object (JSON text, NULL when the block has
 * none). The strings stay valid while their table lives. Returns the number visited.
 * Used by modsound.c for the stock banks a bundle needs ("banks"). */
typedef void (*sh_modserve_registries_fn)(void *context, const char *id, const char *hash, const char *registries);
int sh_modserve_each_bundle_registries(sh_modserve_registries_fn fn, void *context);

/* Visit every file the published table serves (admitted bundles, conflict
 * winners, not withdrawn), in index order. The strings stay valid while their
 * table lives. Used by modvt.c to find the bundles' virtual-texture members. */
typedef void (*sh_modserve_file_fn)(void *context, const char *request, uint64_t size, const char *bundle_id,
                                    const char *bundle_hash);
void sh_modserve_each_served(sh_modserve_file_fn fn, void *context);
/* The same for the open map: per path, the file of the build that serves it
 * now (a map's own copy of a salt, not the root folder's), and no path that
 * nothing serves in this scope. With no map open, the root folder's builds.
 * Used by modvt.c for the families a map's textures page from. */
void sh_modserve_each_served_here(sh_modserve_file_fn fn, void *context);

/* Mod scripts (modbundle.h sh_mb_script_request): every script of `salt` that
 * the index serves now, from the build that serves it (the map scope's choice:
 * a stranger's map its own place's copy, the author's own map the root
 * folder), in request order. fn gets the script's path below scripts\ ("x.lua"
 * or "lib/x.lua", lower case), its request, its size and the SHA-256 the
 * build recorded (the bytes sh_modserve_open must answer with), and the
 * serving build's hash. Returns the number visited. The strings stay valid
 * while their table lives. The runtime reads scripts only this way, never from a
 * folder (script_source.c). */
typedef void (*sh_modserve_script_fn)(void *context, const char *relpath, const char *request, uint64_t size,
                                      const char *sha256, const char *bundle_hash);
int sh_modserve_script_files(const char *salt, sh_modserve_script_fn fn, void *context);

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
 * reloaded the resource (not offered again). A surface model or image (the
 * game's own bytes under its own name, sh_modserve_is_surface) is offered
 * too: the stock spider mesh the game holds from boot is one, and it gets
 * its blacklisted materials only from this reload, which runs inside its own
 * keeper (sh_modserve_inplace_keeper). A surface model or image never
 * opened through the provider (the game loaded it before an index served
 * it, as on a boot that builds the mod folders) is offered once too, as the
 * early-decl visit offers a never-opened decl. This is the one exception to "a
 * surface is never touched": no plan registers or re-parses a surface decl.
 * The early-decl visit above also offers decls first opened before then.
 * Returns the number visited. */
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
#define SH_MODSERVE_RF_ANIM  4   /* anim (cooked/anim/<stem>.bmd6anim): reloaded at a map pass only */

typedef struct sh_modserve_diff {
    int changed;                          /* served requests whose bytes changed */
    int changed_decls, changed_models, changed_images;
    int changed_anims;                    /* anims, reloaded in place at the next map load */
    int changed_map;                      /* other files the next map load reads again (generated/swf/) */
    int changed_restart;                  /* other files nothing reloads in this process (sound banks, skeletons, ...) */
    int changed_scripts;                  /* mod scripts (added or changed): read again at the next Play */
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

/* The editor closure (13954591, both renderers). A boot-time editor pass
 * registers every editor row and every served file a row needs, at any
 * depth, inside its resource watermark, so everything the engine creates for
 * them (an md6Def's MD6 model and anims, the idDeclFX an fxInfo names, the
 * md6Def of an attachment) is raised to level 4 and survives the Play purge.
 * After an in-session install the same work happened outside any watermark:
 * the rescan re-parsed the changed rows and registered the new decls they
 * need at level 1 before the palette rebuild took its watermark, whose keeper
 * then raised only the decl objects a row names, not what they hold, and the
 * Play purge freed the rest under them. A refresh therefore applies its
 * editor-closure part first, inside a watermark of its own, and the rest
 * after. Part SH_MODSERVE_PART_ALL visits everything (the editor pass, whose
 * watermark covers it all); PART_EDITOR only closure files; PART_REST every
 * other item, gone shadows included. */
#define SH_MODSERVE_PART_ALL    0
#define SH_MODSERVE_PART_EDITOR 1
#define SH_MODSERVE_PART_REST   2
int sh_modserve_each_refresh_part(sh_modserve_refresh_fn fn, void *context, int part);

/* Pending editor-closure work of a refresh: closure files still marked
 * changed (new closure decls are kept by their plan, sh_modserve_plan_split). */
int sh_modserve_refresh_editor_pending(void);
/* Anims marked for refresh and not settled yet: they wait for a map pass,
 * which reloads them in place (or finds the bytes already held) and settles
 * them. The engine side says so once they are all settled. */
int sh_modserve_refresh_pending_anims(void);

/* Split a plan into the steps that keep their decls (persist) and are not
 * registered yet, and the rest, both in plan order. Returns the number of
 * kept steps (0: nothing to keep, both outputs empty), or -1. The parts own
 * their steps (sh_modserve_plan_free each). */
int sh_modserve_plan_split(const sh_modserve_plan *plan, sh_modserve_plan *kept, sh_modserve_plan *rest);
/* Copy the parts' ready marks back into the plan they came from. */
void sh_modserve_plan_merge_ready(sh_modserve_plan *plan, const sh_modserve_plan *part);
/* Block the steps whose needed decl failed in an earlier part. Returns the count. */
int sh_modserve_plan_block_failed_needs(sh_modserve_plan *plan);
/* Mark each step ready when its file is registered this session (so
 * sh_modserve_editor_refs visits rows a refresh did not register itself). */
void sh_modserve_plan_mark_registered(sh_modserve_plan *plan);

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
 * list's class name, the resource, its name). The entries are collected under
 * the engine's resource lock (sh_modserve_set_resource_lock) and `fn` runs
 * after it is released. 1 when the walk completed. */
typedef void (*sh_modserve_resource_fn)(void *context, const char *list_class, void *resource, const char *name);
int sh_modserve_resource_each(const void *head_slot, sh_modserve_resource_fn fn, void *context);

/* The engine's resource lock: `lock` is the address of the
 * static struct whose first qword is the CRITICAL_SECTION pointer every list
 * add, remove and find takes (13954591: VK RVA 0x5D79238, GL 0x467E8B8).
 * Every walk of the lists here (watermarks, sh_modserve_resource_each) holds
 * it while it reads the entry arrays. NULL (the default) walks without it. */
void sh_modserve_set_resource_lock(void *lock);

/* How many times the provider served `request` from the published table. */
long sh_modserve_open_count(const char *request);

/* Served bytes (the ledger, modledger.h). The listener is called by
 * sh_modserve_open on every successful serve, on the serving thread, with the
 * indexed request (lowercase; the string lives for the process), the hash of
 * the bundle that served it, the file's recorded SHA-256 (NULL for a composed
 * text or a file without one) and the bytes served. NULL removes it. */
typedef void (*sh_modserve_serve_fn)(const char *request, const char *bundle_hash, const char *file_hash,
                                     const unsigned char *bytes, size_t length);
void sh_modserve_set_serve_listener(sh_modserve_serve_fn fn);

/* What sh_modserve_open would serve for `request` now, without serving it:
 * 1 with the bundle hash and either the file's SHA-256 (*file_hash) or, for a
 * composed text, its bytes (*body, *body_len; index memory, valid for the
 * process); both NULL when the file records no hash. 0 when nothing serves it. */
int sh_modserve_serve_identity(const char *request, const char **bundle_hash, const char **file_hash,
                               const unsigned char **body, size_t *body_len, uint64_t *size);

/* 1 when the file the index serves for `request` is a stock pointer that
 * restores the game's own bytes under their own name (modbundle.h
 * sh_mb_stock_restore): the engine holds those bytes parsed already, so an
 * in-place re-parse changes nothing but what the decl points at. */
int sh_modserve_is_restore(const char *request);

/* 1 when the file the index keys at `request` (the path's static winner,
 * whatever the open map serves now) is a surface (a stock pointer at the
 * game's own path). No pass
 * registers, re-parses or refreshes a surface: plans, refresh lists, the
 * editor-row keeper and the ledger sweep all skip it, and the game parses it
 * from the served (its own) bytes when it needs it. The one exception is the
 * gated reload (sh_modserve_each_ungated_raw): a surface model or image the
 * game loaded before its bundle's requirements were live is loaded again,
 * inside a keeper, once they are. */
int sh_modserve_is_surface(const char *request);

/* Whether an in-place reload or re-parse (the gated reload, a refresh part)
 * opens a keeper of its own (a resource watermark raised to the session's
 * level when it ends): 1 when it has work and runs outside every other
 * watermark (mark_depth 0, the engine side's nesting count). What a reload
 * of a held resource creates (the images its materials resolve) must live as
 * long as the held resource; a purge would otherwise free images a permanent
 * material still points at. */
int sh_modserve_inplace_keeper(int mark_depth, int work);

/* Functions called after every table this layer publishes (boot, rescan),
 * on the thread that built it, outside the build lock, in the order they
 * were added; adding one twice is ignored, NULL clears all. Each call site
 * brings its own link (the static the macro declares), so the list has room
 * for every listener the program registers and none is ever dropped. */
typedef void (*sh_modserve_index_fn)(void);
typedef struct sh_modserve_index_link {
    sh_modserve_index_fn fn;
    struct sh_modserve_index_link *volatile next;
    int linked;
} sh_modserve_index_link;
void sh_modserve_index_link_add(sh_modserve_index_link *link, sh_modserve_index_fn fn);
#define sh_modserve_set_index_listener(fn) \
    do { static sh_modserve_index_link sh_index_link_; sh_modserve_index_link_add(&sh_index_link_, (fn)); } while (0)

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
 * build appear, the way the engine's own callers build it: main
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

/* The pre-load pass of a hub launch (a published map or a My Maps play),
 * called once per launch at its gate, after the gate let it go and before
 * the launch's original runs (modui.c sh_modui_scope_launch, from
 * modpublished.c): the game sits at the hub and no level load is in
 * progress. Inside process heap 0 it does the decl work that would
 * otherwise run inside the level load, as an editor entry does it: every
 * editor row and its closure (the palette's pass, which the level load runs),
 * then any step of the launched map's declared builds the parse's map pass
 * did not settle, with their stock banks. The level load's palette pass and
 * any map pass after it then register nothing new. Main thread, not in Play;
 * returns the decl steps it ran, 0 when nothing was left, -1 refused. */
int sh_modserve_launch_preload(const char *why);

/* Called before the Play level load (the SnapBuild detour in nav_play.c),
 * on the main thread: re-checks the accepted bundles' requirements with
 * sh_modserve_requirements_check (phase "play"). The engine does not reset
 * the blacklist gates between the editor and the level load on 13954591
 * (each gate's int has one reader, its matcher, and no engine
 * writer; both are plain bool cvars that no shipped config names), so this
 * normally logs "requirements live"; a cvar someone changed back is set
 * again before the level's decls load. No-op when nothing is asked or held
 * (sh_modserve_requirements_held). */
void sh_modserve_play_begin(void);

/* 1 during SnapMap Play, 0 otherwise, -1 unknown (the editor's deactivation reason). */
int sh_modserve_engine_in_play(void);

/* Set one engine cvar by name through the requirements gate (immediate); 1 when set. */
int sh_modserve_engine_set_cvar(const char *cvar, const char *value);

/* A scope change (sh_modserve_scope_set) brought into the running game, at
 * a map load, the map exit, the Play entry and in the editor after a save or
 * a map-level change: the requirements pass first (stopped bundles' cvars
 * restored and deactivate lines run, started ones set and their activate
 * lines run), then the pending re-parses and reloads (me_refresh_apply) and
 * the gated reloads. Main thread. Logs one "MODS: scope: <phase>: ..." line.
 * Returns 1 applied, 0 nothing pending, -1 refused (off the main thread). */
int sh_modserve_scope_apply(const char *phase);

/* The listing's declared builds (the last sh_modserve_scope_set), in row
 * order: up to cap copied into out (NULL to count). Returns the count. */
int sh_modserve_scope_hashes(char (*out)[SH_MODSERVE_HASH_HEX + 1], int cap);

/* A declared set the author changed in the editor ("Use in this map"):
 * the map load's activation for the listing's builds, then the
 * scope apply. Their decls not registered this session are registered (a
 * name the map now uses resolves at the next Play without a reload), the
 * generated registries and every other pending change are re-parsed and
 * reloaded in place, and the requirements are applied. Main thread, not in
 * Play; elsewhere it is sh_modserve_scope_apply. Returns what that returns. */
int sh_modserve_scope_activate(const char *phase);

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
