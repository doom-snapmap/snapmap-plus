/* Serve installed mods and map compositions through the resource provider. Registration runs on the game thread. */
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

/* Build the table from `mods_root` (UTF-8, as sh_ms_root spells it) and publish it, logging one MODS: line per
 * skipped bundle and per conflict. Returns the bundles admitted, or -1 when the table could not be allocated (the
 * previous table stays published); a missing folder is an empty index. */
int sh_modserve_index_build(const char *mods_root);

/* The same for the store's root (sh_ms_root, modstore.h). Called once by the
 * bootstrap before the provider hook is installed. */
int sh_modserve_index_boot(void);

/* Rebuild from the folder of the last build (after an install or a fetch).
 * Decls already registered stay registered; editor rows of new bundles need
 * modserve_palette_rebuild_requested. */
int sh_modserve_index_rescan(void);

/* Free the tables two or more builds behind the published one, when no provider call is in flight on any thread
 * and no plan is alive. Every string the index hands out stays valid until the second table built after its own is
 * published and this runs; a consumer that keeps one longer copies it. Main thread, at the start of a registration
 * pass; returns the tables freed. */
int sh_modserve_index_retire(void);
/* Keep tables alive across a worker's related serving-index queries. */
void sh_modserve_read_begin(void);
void sh_modserve_read_end(void);

/* Remove the served copies (renamed banks, rewritten texts) the published table does not use, and any temporary
 * a crashed write left. Run once at boot; skipped while another game instance on this account uses the folder.
 * Returns the files removed. */
int sh_modserve_served_prune(void);

/* Nonzero when the published table serves at least one path. */
int sh_modserve_active(void);
int sh_modserve_has_actor_rows(void);
void sh_modserve_set_actor_derives(int (*derives)(const char *cls, const char *base));
int sh_modserve_iterator_uses(const char *actor);
int sh_modserve_actor_enabled(const char *row);
/* Source for a Play map entity. Returns source unchanged for a healthy placement, or inert text; free *owned. */
const char *sh_modserve_actor_play_text(const char *inherit, const char *cls, const char *source, char **owned);
struct sh_modmanifest_list;
int sh_modserve_actor_prefab_pending(void);
int sh_modserve_prefab_has_direct_actors(const char *json, size_t len);
int sh_modserve_prefab_actor_rows(const char *json, size_t len, const struct sh_modmanifest_list *rows,
                                  int (*covered)(const char *), int (*admit)(const char *, const char *));
int sh_modserve_finish_actor_rows(const char *json, size_t len,
                                  int (*covered)(const char *), int (*admit)(const char *, const char *));
void sh_modserve_prepare_prefab_actors(const char *json, size_t len, const struct sh_modmanifest_list *rows);
void sh_modserve_finish_prefab_actors(const char *json, size_t len);
int sh_modserve_refresh_actor_rows(int (*covered)(const char *), int (*admit)(const char *, const char *));
int sh_modserve_restore_actor_rows(int (*covered)(const char *), int (*admit)(const char *, const char *));
int sh_modserve_uses_stock_actor(const char *json, size_t len, int (*covered)(const char *));
int sh_modserve_direct_actor_map_uses(const char *json, size_t len);
int sh_modserve_direct_actor_rows(const char *json, size_t len, int (*exists)(const char *),
                                  int (*admit)(const char *, const char *));
void sh_modserve_prepare_direct_actors(const char *json, size_t len);
void sh_modserve_restore_direct_actors(void);

/* Where the backend learns what a map lists (the map scope's hook and
 * source, modmanifest.h and modui.c): a load, a save, the Play entry. */
#define SH_MODSERVE_AT_LOAD 1
#define SH_MODSERVE_AT_SAVE 2
#define SH_MODSERVE_AT_PLAY 3

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
    int         is_stock;    /* a stock pointer, served from the installed game (size 0) */
    const char *stock_source;/* its installed source, or NULL */
    const char *author_path; /* the source file's path within its mod */
} sh_modserve_hit;

/* Bank stems of their own : every
 * carried .bnk of a bundle is served as sound/soundbanks/pc/<stem>.<sha16>.bnk,
 * <sha16> the first 16 hex characters of the bank file's own SHA-256, its BKHD
 * bank id rewritten to the FNV-1 of that stem, and every quoted "<stem>",
 * "<stem>.bnk" or ".../<stem>.bnk" in the bundle's served decl texts is
 * rewritten to match. The rewritten bytes are written once under
 * <data>\cache\served\, keyed by their own hash (banks\<stem>.<sha16>.bnk,
 * files\<sha16>.<extension>), and served from there. So a changed bank is a
 * new bank to Wwise, two builds carrying one bank's bytes share it, and the
 * ledger and refresh see a new name. */
/* `text` with every quoted "<stem>", "<stem>.bnk" or ".../<stem>.bnk" (ASCII
 * case-insensitive) spelled with `renamed` instead of `stem`. Malloc'd, or
 * NULL when nothing changed. Pure. */
char *sh_modserve_bank_rename_text(const char *text, size_t len, const char *stem, const char *renamed, size_t *out_len);
/* Rewrite a bank's BKHD id to FNV-1 of `stem` in place. 1 done, 0 not a BKHD bank. Pure. */
int  sh_modserve_bank_patch_id(unsigned char *bytes, size_t len, const char *stem);
/* Rewrite every HIRC Event id of a bank that equals from[k] to to[k] in place (the namespace of a mod's sound
 * events: modserve.c ms_sound_events). The count rewritten. Pure. */
int  sh_modserve_bank_events(unsigned char *bytes, size_t len, const uint32_t *from, const uint32_t *to, size_t count);
/* Map the namespace `from` (8 characters) to `to` in place wherever the bytes spell it as a name part: after a '/'
 * or "#str_", before a '/' or '_' (mods/<from>/x, .../<from>_play_x, #str_<from>_key). Length-preserving and
 * byte-exact. The count mapped. Pure. */
size_t sh_modserve_respell(unsigned char *bytes, size_t len, const char *from, const char *to);

/* Constant-time, allocation-free lookup of an engine request path (any case,
 * either separator). Returns 1 and fills *out (when non-NULL) for a served
 * path, else 0. */
int sh_modserve_lookup(const char *request, sh_modserve_hit *out);

/* The lookup without the map scope: what the index holds for `request` even when the open map does not list its
 * bundle. For reports (the Mods tab, sh_mods doctor), never for serving. */
int sh_modserve_lookup_indexed(const char *request, sh_modserve_hit *out);

/* 1 when an admitted bundle serves `module_name` (maps/modules/...decl, any
 * case) as a snapModule decl: the name a saved map's moduleName holds and the
 * private module palette (grid_room_native.c) is asked to find. */
int sh_modserve_module_served(const char *module_name);

/* Serve `request`: 1 with a process-heap copy of the bytes in *bytes (freed with HeapFree), 0 when no bundle serves
 * the name, -1 when it serves it but the file cannot be read at its indexed size (the caller falls through to the
 * engine). A stock pointer's bytes come from the installed game through the stock reader (modstock.c). */
int sh_modserve_open(const char *request, unsigned char **bytes, size_t *length);

/* The bound entityDef's own admitted AI behavior, including same-mod inherits. Caller frees. */
char *sh_modserve_enemy_behavior(const char *salt, const char *entitydef);

/* A served file's bytes as read from disk now, never from the cache: 1 with a HeapFree-able copy, 0 when no bundle
 * serves the name or it is not a plain file (a stock pointer, a composed text), -1 when unreadable. Lets
 * script_source.c check a file against its build. */
int sh_modserve_open_disk(const char *request, unsigned char **bytes, size_t *length);

/* Whether the decl existence probe should answer true for `path`: a served
 * decl request (generated/decls/... or decltree/...). Counts the claim. */
int sh_modserve_probe_claims(const char *path);

/* The decl registration plan: every decl to register in dependency order (other decls, md6Def, entityDef, then
 * snapEditorEntityDef, each after the steps its needs name). A step is blocked when a need names a mods/ identity
 * no admitted bundle provides, and so is every step that needs it. */
/* A step's rank, the order among steps no need orders. A renderParm comes first: the engine fixes its slot and kind
 * at its first parse, so a material parsed earlier binds a slot whose kind then changes (the render job read
 * floats as a sampler pointer). A material's parm keys are bare names, so no need orders them. */
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
    int         rank;        /* SH_MODSERVE_RANK_* */
    int         persist;     /* 1: keep through map purges (lifetime 4), every editor-plan step; 2: an editor-closure
                                decl of another plan (ms_mark_editor_closure), unless the engine holds it permanently */
    int         shadow;      /* always 0: shadows are served by the provider, never planned */
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

/* The engine operations a plan runs through: production binds them to the decl registry (modserve_engine.c), tests
 * supply fakes. Each returns 1 for success; `state` and `lifetime` receive the resource bytes at +0x2c/+0x28 after
 * the load, for the log. */
/* `load` answers SH_MODSERVE_LOAD_REFUSED when the engine raised a parse error naming the decl's file. An animWeb's
 * parse catches its own error and leaves a truncated web that looks loaded (state bit 0 clear), so the plan
 * withdraws the file and never loads it again. */
#define SH_MODSERVE_LOAD_REFUSED 2
/* `load` answers SH_MODSERVE_LOAD_LEFT for a permanent decl (sh_modserve_load_left_to_engine): the pass leaves it
 * to the game, which reads the bundle's bytes through the provider when it parses it. Counted as ready. */
#define SH_MODSERVE_LOAD_LEFT 3
typedef struct sh_modserve_engine_ops {
    void *context;
    int (*source_registered)(void *context, const char *type, const char *name);
    int (*register_file)(void *context, const char *type, const char *name, const char *reg_path);
    int (*load)(void *context, const char *type, const char *name, int persist,
                unsigned *state, unsigned *lifetime);
    /* Optional: 1 when the player's game archives hold `request` (a lower-cased path), 0 when not, -1 when the
     * reader cannot tell. The registry answers 0 for a stock decl whose source is not registered yet, so the model
     * check asks the archives before withdrawing an animWeb. */
    int (*stock_exists)(void *context, const char *request);
    /* Optional: called once between the source registrations and the loads, so shadows the engine parsed before
     * the serving index can be re-parsed (sh_modserve_each_unopened_shadow) before a new decl that inherits one
     * is parsed. */
    void (*sources_published)(void *context);
    /* Optional: 1 when the engine's own lookup finds the decl (DeclFind without makeDefault, as an animWeb's parse
     * looks up each model), 0 when not, -1 when it cannot tell. It decides the model check for a decl the pass
     * loads, since the game archives also hold campaign content SnapMap cannot load. */
    int (*decl_found)(void *context, const char *type, const char *name);
    /* Optional: the engine's error text after `load` answered
     * SH_MODSERVE_LOAD_REFUSED, for the log and the console line. */
    void (*refusal)(void *context, char *out, size_t cap);
    /* Optional: 1 when the engine already holds a resource of this type and name, with its state byte (+0x2c) and
     * lifetime (+0x28); 0 when none; -1 when it cannot tell. A pure lookup. DeclRegisterFile re-parses an existing
     * resource of its name at once (VK FUN_141559a30 -> FUN_141561af0 -> DeclParseSource), so the registration
     * phase asks this first and leaves a decl to the game as `load` does (sh_modserve_load_left_to_engine). */
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

/* The model check for the served animWeb at `request`: 1 with the first model its models or modelInfos block
 * names that no bundle serves and the engine cannot find (written to `model`), 0 when every model resolves or it
 * cannot tell, -1 when `request` is not a served animWeb. */
int sh_modserve_animweb_check(const char *request, const sh_modserve_engine_ops *ops, char **model);

/* Withdraw the file serving `request` (lookups and the probe miss it, a
 * rescan keeps it withdrawn while its bytes are unchanged), logging `why` and
 * printing one console line. Returns 1 when unavailable, 2 when verified stock can serve, or 0 for no change. */
int sh_modserve_withdraw(const char *request, const char *type, const char *name, const char *why);

/* 1 when the engine error text `text` names the decl file of `type`/`name`
 * ("...\generated\decls\animWeb\designsystems\med_station.decl(21): error : ..."),
 * matched without case and with either slash. */
int sh_modserve_error_names_decl(const char *text, const char *type, const char *name);

/* Run `load(resource)` and restore the resource's lifetime (+0x28). ResourceGenericLoad's failure path rebuilds
 * the object in place at level 1 or 2, so a permanent decl whose in-place re-parse failed would be destroyed by
 * the next map unload while permanent consumers still point at it. Returns the level the load left; `*was`
 * receives the level before. */
unsigned sh_modserve_load_keep_lifetime(void *resource, void (*load)(void *), unsigned *was);

/* 1 when a pass that does not keep its decls (persist 0, the map pass) must not load the resource with level
 * `lifetime` (+0x28) and state byte `state` (+0x2c): a permanent decl (level 4) that has not failed (state bit 0),
 * parsed or created by reference. Parsing binds it to decls the pass created at level 1, which the next map
 * unload frees while the permanent decl keeps the pointers (SetupModelList faulted on the menu collectible
 * 'spidermastermind', VK RVA 0x33a130). Nothing is lost: the boot parse and the refresh already serve it the
 * bundle's bytes. */
int sh_modserve_load_left_to_engine(unsigned lifetime, unsigned state, int persist);

/* Run a plan: DeclRegisterFile for every unblocked step whose source is not registered yet (registration re-parses
 * at once a resource of that name the engine holds), then the loads in plan order. A permanent decl the engine holds
 * is not registered by a pass that does not keep its decls (ops->held, sh_modserve_load_left_to_engine), since the
 * re-parse would bind it to objects the pass creates at level 1. One MODS: line per step. Returns the steps that
 * ended ready, or -1 when ops is unusable. */
int sh_modserve_plan_run(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                         const char *phase);

/* The plan's steps no run settled ready (registered, or left to the game) that have not failed or been withdrawn:
 * what another pass would still do. 0 when a pass already did all of it, as a hub launch's gate checks before its
 * level load (sh_modserve_launch_preload). */
int sh_modserve_plan_unvisited(const sh_modserve_plan *plan);

/* The decls an editor row points at directly, which must live as long as the row does. A snapEditorEntityDef is
 * kept for the session (lifetime 4), but a decl it names that was created after boot sits at level 1, so the next
 * map unload frees it under the row (AV in the animator's node set-up, VK 0x14170a5d3). For every ready row of
 * the plan, calls `fn` once per reference: eventInfo animWebDecl (animWeb), renderInfo renderModelName ending
 * ".md6" (md6Def), previewImage (material). Returns the number of calls. */
typedef void (*sh_modserve_ref_fn)(void *context, const char *type, const char *name, const char *row);
int sh_modserve_editor_refs(const sh_modserve_plan *plan, sh_modserve_ref_fn fn, void *context);

/* The rest of a row's closure, created by the editor pass: an md6Def holds its MD6 model (+0x60), the model its
 * skeleton and joint tables, an animWeb its anims, a material its images, all separate level-1 resources
 * (idJointAnimator's joint blend read a freed model's table, VK 0x14173fb46). The pass takes a watermark of every
 * resource before it runs and raises every resource created since then that is still at level 1 to 4. */
/* Layout read (13954591, both renderers): `head_slot` is the address of the global list-of-lists head (engine
 * global decl_resource_head); each idResourceList holds its class name (char *) at +0x10, the next list at +0x18,
 * its entries at +0x20 and their count (int) at +0x28; each resource holds its level at +0x28. Reads are bounded
 * and fault-protected; main thread only. */
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

/* Every resource a level-4 decl of a served bundle holds is kept with it: from each decl of an admitted, non-surface
 * file the engine holds at level 4, the needs the builder recorded (an animWeb's clips and md6Defs, an md6Def's mesh
 * and clips, an entityDef's models and md6Defs), closed through every file of the index that provides one, are looked
 * up by type (a list's +0x08) and name (folded) and raised from level 1 to 4. Run after every serving pass and
 * before the Play level load; returns 1 when both walks completed. */
typedef struct sh_modserve_holds {
    unsigned roots;       /* decls of served bundles the engine holds at level 4 */
    unsigned names;       /* distinct (type, name) needs of those */
    unsigned raised;      /* resources raised from level 1 to 4 */
    unsigned lists;       /* lists walked */
    char first[300];      /* the raised names, first ones */
} sh_modserve_holds;
int  sh_modserve_holds_raise(const void *head_slot, sh_modserve_holds *out);

/* The render models of that same closure, which the product loads itself in process heap 0 so they are held with
 * the decls that hold them: a mod prop's model is loaded at the entity's spawn in the map's heap, and the Play purge
 * frees it. For each "model" need the index serves in the current scope that the lists hold not at all, as a default
 * (+0x2c bit 0 or 2), or at level 1 under a map heap: fn(context, name, resource or NULL), outside the resource lock;
 * fn returns 0 to stop. Returns the number of calls, -1 when a walk failed. */
#define SH_MODSERVE_HELD_MODEL_TYPE "model"
typedef int (*sh_modserve_held_model_fn)(void *context, const char *name, void *resource);
int  sh_modserve_held_models(const void *head_slot, sh_modserve_held_model_fn fn, void *context,
                             sh_modserve_holds *out);
/* The resource list whose type name (+0x08) is `type`, or NULL. */
void *sh_modserve_resource_list(const void *head_slot, const char *type);

/* What a kept pass looks up. A pass that registers while the previous map's level-1 objects are alive (an in-editor
 * rawmap reload, `sh_mods map add`) parses decls that bind to them, and the Play purge then frees them under the
 * kept decl. While a watermark is open the engine side records every resource ResourceLookup returns on the main
 * thread (`note`); `close` raises each one the lists still hold at level 1 to 4, under the resource lock. */
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

/* What the engine looks up on behalf of a kept object is kept with it. The engine side tracks load nesting on the
 * main thread (a detour on ResourceGenericLoad), records each (parent, child) pair of a load or a ResourceLookup
 * made inside another load (`note`), and keeps the child at once when the parent is kept (`keep` raises it from
 * level 1 to 4 when its load returns). `close` follows the recorded pairs from every kept resource, raises every
 * listed kept one still at level 1 to 4 under the resource lock, and with `forget` set (before the Play level
 * load's purge) forgets all pairs. Both sets are bounded; a full set stops recording and the close says so. */
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
/* The keepability rule: nothing raises a resource to level 4 except sh_modserve_keep, and only what
 * sh_modserve_keepable allows. A resource whose last load ran with a map's heap selected is never kept, since
 * raising its level keeps the object but not its backing data. Renderer-owned decal atlas entries stay at their
 * native lifetime. `keepable` counts and names refusals caused by the map heap
 * for sh_modserve_left_to_map; `keep` raises `resource` to 4 and adds it to the kept set (1 kept or already at 4,
 * 0 left at the engine's level). Main thread. */
int  sh_modserve_keepable(const void *resource);
int  sh_modserve_keep(void *resource);
void sh_modserve_kept_add(const void *resource);
int  sh_modserve_kept_has(const void *resource);
int  sh_modserve_attrib_note(const void *parent, const void *child);
int  sh_modserve_attrib_keep(void *resource);
/* Load nesting reported by the engine side's detour, main thread only. `enter` before a load notes the pair with
 * the innermost open load (*kept: the parent was kept) and returns the depth to hand to `leave`; `found` notes a
 * resource the lookup returned inside it and keeps it when that load is kept. The engine stops at 32 nested loads
 * (ResourceMaterialize); past SH_MODSERVE_NEST_MAX nothing is attributed. `heap` is the heap the load allocates
 * from (0 the process heap, -1 unknown); `left_to_map` returns and clears the count of keeps that left one at level 1. */
#define SH_MODSERVE_NEST_MAX 64
int  sh_modserve_attrib_enter(void *resource, int heap, int *kept);
void sh_modserve_attrib_leave(int depth);
int  sh_modserve_attrib_depth(void);
void sh_modserve_attrib_found(void *resource);
unsigned sh_modserve_left_to_map(char *names, size_t cap);
/* A kept resource's data lives as long as it is kept. A resource already kept at level 4 can be loaded again (a
 * re-parse in place, a load the engine repeats) with a map's heap selected, and that load's allocations would be
 * released with the heap while the object stays listed. 1 when the load of `resource` with `heap` selected must run
 * in process heap 0 instead: it is kept, at level 4, and `heap` is a map's (> 0). */
int  sh_modserve_kept_load_moves(const void *resource, int heap);
int  sh_modserve_attrib_close(const void *head_slot, int forget, sh_modserve_attrib *out);
/* Tests: forget everything. */
void sh_modserve_attrib_reset(void);

/* Editor rows at map load: the palette is built once per process and the map validator (RepairAndMigrate) looks
 * every placed entity up in it. `palette_ready` answers 1 when it exists, `in_palette` 1 when a row for that sedef
 * name is in it, `rebuild` runs the heap-scoped rebuild and returns 1 when the builder ran, else the refusal code. */
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

/* For exactly the named bundles (64-hex hashes): when the palette exists and lacks one of their rows that no editor
 * pass has visited, request one rebuild (nothing is asked of ops when no palette exists yet). Returns 1 when
 * requested, 0 when not, -1 on unusable ops or no memory. */
int sh_modserve_map_rows_sync(const char *const *bundle_hashes, int n, const sh_modserve_palette_ops *ops,
                              sh_modserve_rows_sync *out);

/* A mod.json may carry {"requirements": {"cvars": {"<cvar>": <value>}, "commands": {"activate": ["<line>"],
 * "deactivate": [...]}}}, read through the builder's canonical form (modbundle.h sh_mb_requirements_canonical); a
 * bundle whose requirements the builder refuses is skipped. Bundles are decided in order (the open map's rows in
 * row order, then the others by install order); one that asks a cvar at another value than an earlier one loses,
 * is named once in the log as "MODS: requirements: conflict: <cvar>: ...", and applies none of its requirements.
 * A cvar a requirement sets is global. */

/* The number of distinct cvars the accepted bundles' requirements set. */
int sh_modserve_requirements(void);

/* 1 when a pass has work: an accepted bundle has requirements, or a cvar
 * this layer set, or a bundle whose activate lines ran, waits for release. */
int sh_modserve_requirements_held(void);

/* How cvars are read and set and console lines are run. `read` copies the cvar's value text (NUL-terminated,
 * truncated to cap) and returns 1, or 0 when the engine has no such cvar. `set` returns SH_MODSERVE_GATE_SET when
 * the value is in the cvar on return (idCVar::Set), SH_MODSERVE_GATE_QUEUED when only buffered as console text, 0
 * when refused, or a negative SH_MODSERVE_GATE_*. `drain` executes the command buffer (NULL when sets are never
 * queued); `command` appends one console line, and with NULL no line runs. */
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

/* Make the accepted bundles' requirements live before a registration pass: append the deactivate lines of bundles
 * whose requirements stopped; phase 1 reads each asked cvar and sets it when it differs; phase 2 reads it back, and
 * only the asked value counts as live. A cvar the engine lacks, refuses or does not keep is not live (the cause is
 * logged and reported) and blocks the steps of the bundles asking it. A cvar no accepted bundle asks any more is
 * restored to the value it displaced, then the activate lines of starting bundles are appended. Returns 0 when no
 * cvar is asked, 1 when every asked cvar is live, -1 otherwise. Main thread. */
int sh_modserve_requirements_apply(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

/* sh_modserve_requirements_apply for a point where nothing is registered (the Play level load): the same pass, plus
 * one line when every asked cvar was already live ("MODS: play: requirements live: <cvar>=<value>, ..."). Returns
 * what sh_modserve_requirements_apply returns. */
int sh_modserve_requirements_check(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out);

/* Block the steps of bundles the map declares whose cvar requirements are not live, except steps already registered.
 * An undeclared bundle, or one whose cvar lost a conflict to another the map uses, is not blocked: it is served and
 * only its value is not applied. Returns how many were blocked. */
int sh_modserve_plan_block_unmet(sh_modserve_plan *plan, const sh_modserve_gates *gates);

/* sh_modserve_requirements_apply, then sh_modserve_plan_run: no DeclRegisterFile is issued before the cvars are
 * verified, and steps of a bundle whose requirement is not live are blocked with that cause (unless already
 * registered). An empty plan touches nothing. Returns what sh_modserve_plan_run returns. */
int sh_modserve_plan_run_gated(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                               const sh_modserve_gate_ops *gates, const char *phase, sh_modserve_gates *out);

/* One line per requirement of each admitted bundle with its state for the open map (live, not active for this map,
 * refused, NOT live, restored, not applied yet, command ran), then each cvar waiting to be restored and each
 * stopped bundle whose lines wait, or one "requirements: none" line. For sh_mods status and doctor; any thread. */
typedef void (*sh_modserve_line_fn)(void *context, const char *line);
int sh_modserve_requirements_report(sh_modserve_line_fn fn, void *context);

/* Map scope has two layers: while a map is open or played, the bundles it declares serve, then the game. A map
 * declares a bundle by the hash of its manifest rows and of its placed names; the first hash of a salt in row order
 * declares that salt, and every other build of it is inert. Per path the serving file is chosen among the declared
 * providers by the fewest unprovided needs, then the bytes most providers carry, then row order, never install order
 * (decls that differ only inside lists or bags are composed, modserve.c). */

/* A bundle the map does not declare serves, in the editor with no map open (SH_MODSERVE_MODE_EDITOR), the names
 * under its salt (the Toybox offers its rows) for the root folder's build only, plus its surfaces and never its
 * shadows; in Play and in a map launched outside the editor (SH_MODSERVE_MODE_PLAY) it serves nothing. A bundle whose
 * shadows include editor-time content (a generated/decls/snapeditor*, snappropertyinspector* or snappalette* decl,
 * or the editor's own movies) is the editor-tweak class: its editor tweaks serve in every map, logged once as
 * "MODS: scope: <mod name> shadows editor-time content (<request>) ...". */

/* Only a declared bundle's requirements apply. An undeclared bundle's Toybox rows register without them
 * (sh_modserve_requirements_apply runs its deactivate lines and restores its cvars); once a placement declares it,
 * its requirements apply and every file of it read without them is offered again (sh_modserve_each_unopened_shadow,
 * sh_modserve_each_ungated_raw). */

/* Scope is off until sh_modserve_scope_enable(1) (sh_modserve_index_boot enables it); with it off every bundle
 * serves everything by the static winner rules (sh_modserve_conflict). */

/* When the listing or the mode changes, every shadow decl, model and image whose serving bytes changed is marked
 * pending as a rescan does (sh_modserve_each_refresh, with `gone` set while withdrawn), and so is every name another
 * build of its salt serves with other bytes than last served; the inheritance closure is marked again
 * (sh_modserve_refresh_expand). A withdrawn name is never marked: the engine keeps what it parsed until a map serves
 * the name again. */
#define SH_MODSERVE_SCOPE_MAP    0
#define SH_MODSERVE_SCOPE_ALWAYS 1

#define SH_MODSERVE_MODE_EDITOR  0   /* the editor, the hub, no map: undeclared bundles offer their names */
#define SH_MODSERVE_MODE_PLAY    1   /* Play, a published or My Maps launch: undeclared bundles serve nothing */

void sh_modserve_scope_enable(int on);
int  sh_modserve_scope_enabled(void);

/* Set what the current map declares: `hashes` are the hashes of the builds that serve its rows (sh_modserve_row_build)
 * in row order, then the placed closure; `map_open` is 1 while a map is open or playing and 0 with none (which
 * returns the mode to SH_MODSERVE_MODE_EDITOR). Returns how many bundles changed what they serve or their
 * requirements. Any thread; serialised with index builds. */
int  sh_modserve_scope_set(const char *const *hashes, int nhashes, int map_open);

/* Temporary named files of a pending prefab in this editor; its requirements still follow committed placement. */
int  sh_modserve_prefab_pending(void);
int  sh_modserve_prefab_files(const char (*hashes)[SH_MODSERVE_HASH_HEX + 1], int count);

/* The row resolver. A pinned row resolves only to the content cache's composition of its units, never a local mod.
 * A local row (the player's own map) names no build: it resolves to the root folder of the salt, else with no folder
 * here to the content cache's stand-in composition of the salt (no units), whose stand-ins keep the map's objects in
 * place. A bare hash resolves the same way: the root's folder at the hash, else the composition. Any thread. */
/* The folder mods\maps\: the index never reads it, the store never writes or lists it, and no mod is made there. */
#define SH_MODSERVE_RETIRED_PLACES "maps"
/* The content cache's area (modcache.h). The composition hash identifies the map row and its sidecar selects the files. */
#define SH_MODSERVE_CONTENT   "content"

/* Set the mode (SH_MODSERVE_MODE_*): the Play entry sets PLAY (sh_modserve_scope_source, SH_MODSERVE_AT_PLAY), a map
 * load sets PLAY until the editor shows the map (modui.c), the editor's tick sets EDITOR. Marks as
 * sh_modserve_scope_set does and returns the bundles that changed. */
int  sh_modserve_scope_mode(int mode);
int  sh_modserve_scope_mode_get(void);

/* 1 while the last sh_modserve_scope_set said a map is open. */
int  sh_modserve_scope_map_open(void);

/* 1 when the listing can matter: scope is enabled and an admitted bundle is
 * map-scoped with shadows or requirements, or the listing is not empty. The
 * map manifest reads placed names at a load only then. */
int  sh_modserve_scope_relevant(void);

/* While building snapshot templates, any thread can call this to exclude undeclared mod namespaces,
 * refused entities and inert actor stand-ins. With map scope disabled, it returns false for every name. */
int  sh_modserve_level_hides(const char *name);

/* The stock spawn-type bits a ported demon of the current listing stands in for, read from its conductor row
 * (modregistry.h): `type` is an AI's spawn type; 0 for a stock type, a type no declared mod was given, and with
 * nothing generated. Lock-free; any thread (the kill score's detour, kill_score.h). */
unsigned sh_modserve_spawn_type_standin(unsigned type);
/* The exact actor of a single encounter whose class is shared, malloc'd; NULL for a native type draw. */
char *sh_modserve_shared_actor(const char *encounter);

/* The ported demons' types the current listing gave, as the map pins them
 * (modregistry.h SH_MR_PIN): fn(salt, slot) per type. Returns the count. */
int sh_modserve_spawn_pins(void (*fn)(void *context, const char *salt, const char *slot), void *context);
/* The type system's enum extension (typeinfo.h sh_typeinfo_enum_extend), bound
 * at install: the registry adds the free bits' spawnAIType_t members through it
 * (modregistry.h, the AI Filter's flags). Unbound (the tests) nothing is added. */
typedef int (*sh_modserve_enum_extend_fn)(const char *name, const char *const *names, const long long *values, int n);
void sh_modserve_set_enum_extend(sh_modserve_enum_extend_fn fn);

typedef struct sh_modserve_scope_info {
    int scope;               /* SH_MODSERVE_SCOPE_* in effect */
    int automatic;           /* "always" because it shadows editor-time content */
    int listed;              /* the current map declares it (this build) */
    int rank;                /* its position among the declared bundles (0 the first row), or -1 */
    int inert;               /* the current map declares another build of its salt */
    int map_open;            /* a map is open */
    int play;                /* the open map is played (SH_MODSERVE_MODE_PLAY) */
    int newest;              /* the root folder's build of its salt: it serves with no map open */
    int in_place;            /* it is a composition of the content cache (a pinned row's), not a root folder */
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

/* A mod the open map uses, named as a player names it: its name, its folder, the folder's last part, or its salt,
 * ASCII case-insensitive. 1 with its salt and the name lines use (*label, malloc'd: its name, else its folder),
 * 0 when the open map uses no such mod. */
int  sh_modserve_listed_mod(const char *name, char *salt, size_t salt_cap, char **label);

/* 1 when the admitted bundle `bundle_hash`'s edits of game content apply: map scope is off, the open map declares
 * it, or it is the editor-tweak class (its editor tweaks serve in every map); else 0. A mod's string edits
 * (strkeys.h, strids.c) follow this. */
int  sh_modserve_bundle_edits_live(const char *bundle_hash);

/* For sh_mods status, doctor and map: one header per mod installed at several builds, then one "scope: <mod name>
 * (N names, N shadows, N surfaces): <state>" line per admitted bundle, then per path several bundles provide which
 * layer serves it and why (at most 40 lines), and a "serving: ..." count line. Mods are named, never by salt or hash. */
int  sh_modserve_scope_report(sh_modserve_line_fn fn, void *context);

/* Registries (modregistry.h): the open map's registry choices, the listing's second input. Slots and keys are
 * copied; with `recompute` the listing is recomputed as sh_modserve_scope_set does and the bundles whose serving
 * changed are returned. Cleared when a listing with no map open is set. */
int  sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute);
/* One line per registry path ("registry <request>: generated, N bytes" or "the game's own file"), then what was
 * generated and every clash, then the choices. Returns the lines. */
int  sh_modserve_registry_report(sh_modserve_line_fn fn, void *context);

/* The points that must read the open map itself (the Play entry: Play does not serialize the map) ask the bound
 * source (modui.c) to recompute the listing from the current manifest and the placed names and call
 * sh_modserve_scope_set. `when` is a SH_MODSERVE_AT_* point. Returns the bundles that changed, 0 without a source. */
typedef int (*sh_modserve_scope_source_fn)(int when);
void sh_modserve_set_scope_source(sh_modserve_scope_source_fn fn);
int  sh_modserve_scope_source(int when);

/* 1 installed and admitted, 0 not installed, -1 installed but skipped (the
 * first cause, malloc'd, in *cause when cause is not NULL). */
int sh_modserve_bundle_state(const char *bundle_hash, char **cause);

/* 1 when a root folder of `salt` is installed, admitted or skipped (its build failed): a local row of that salt whose
 * folder serves nothing is then a folder that cannot be used, not one absent from this computer. Lock-free; any
 * thread. */
int sh_modserve_salt_installed(const char *salt);
/* 1 when the root folder of `salt` is here and does not carry `name` (a placed name a local row lists): the row then
 * stands in beside the folder (the content cache's stand-in composition of the salt serves only what the folder
 * lacks). 0 otherwise. */
int sh_modserve_root_lacks(const char *salt, const char *name);
/* Changes when a native refusal withdraws a definition or its dependents. */
int sh_modserve_refusal_version(void);

/* The asset rule: a map, rawmap or prefab needs the builds its manifest rows name. A row is SERVED when its build
 * (sh_modserve_row_build) is admitted and every name the row lists resolves, MISSING when it names none, INCOMPLETE
 * when a listed name does not resolve. *cause (malloc'd, when not NULL) gets the refusal sentence, with `subject`
 * replacing "map". Lock-free; any thread. */
#define SH_MODSERVE_ROW_MISSING    0
#define SH_MODSERVE_ROW_SERVED     1
#define SH_MODSERVE_ROW_INCOMPLETE 2
struct sh_modmanifest_row;
int sh_modserve_row_served(const struct sh_modmanifest_row *row, char **cause);

int sh_modserve_row_served_for(const struct sh_modmanifest_row *row, const char *subject, char **cause);

/* The build that serves a row (the row resolver above). 1 with *out filled, 0 when none (the row is missing and a
 * hold fetches it). `id` is the salt. */
typedef struct sh_modserve_bundle_ref {
    char id[96], hash[SH_MODSERVE_HASH_HEX + 1];
    char *name;                        /* the author's name, "" when none (malloc'd) */
    char *folder;                      /* its folder below the mods root, or below cache/content for a composition (malloc'd) */
    char *place;                       /* SH_MODSERVE_CONTENT for a composition, "" at the root (malloc'd) */
    unsigned long long size;           /* carried bytes, as a manifest row's size */
} sh_modserve_bundle_ref;
/* Filled refs hold malloc'd text: release each with sh_modserve_bundle_ref_free. */
int sh_modserve_row_build(const struct sh_modmanifest_row *row, sh_modserve_bundle_ref *out);

/* A composition (modunit.h) carries only the files its closure found a map's object needs, so a file the game asks
 * for that it lacks is a closure defect: the map plays on and the name is recorded. The provider (overrides.c) calls
 * sh_modserve_note_miss with every request it found nowhere; one under mods/<salt>/ of a composition is logged once
 * per process and handed to the bound reporter (modpublish.c). Returns 1 when noted, 0 otherwise. Any thread. */
/* The folder the content cache composes a row's stand-ins under (modcache.c): a palette row's stand-in is the
 * snapEditorEntityDef mods/<salt>/snapmapplus_standin/<placed name>. Its names are the product's, never a unit's. */
#define SH_MODSERVE_STANDIN_DIR "snapmapplus_standin"
#define SH_MODSERVE_STANDIN_CLASS(cls) "{\n\tinherit = \"snapmaps/unknown\";\n\tclass = \"" cls "\";\n}\n"
/* 1 when `name` (an engine request, a decl name or an author path) lies under a stand-in folder: never a file a unit
 * lost, never asked of the store, never read again once the row's real composition serves. */
int sh_modserve_standin_name(const char *name);
typedef void (*sh_modserve_miss_fn)(const char *salt, const char *unit, const char *request, const char *by);
void sh_modserve_set_miss_reporter(sh_modserve_miss_fn fn);
int  sh_modserve_note_miss(const char *request);
/* The misses noted since the process started: one "<mod name>: <request>"
 * line each (sh_mods status). Returns the count. */
int  sh_modserve_misses_report(sh_modserve_line_fn fn, void *context);
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref);

/* Map placed logical names (entityDef inherit names, or any decl name a bundle provides) to the installed bundles
 * that provide them or their needs. Writes up to `cap` distinct NUL-terminated 64-hex hashes in first-seen order and
 * returns how many were found (which may exceed cap); stock names map to nothing. */
int modserve_bundles_for_names(const char *const *names, int n_names,
                                  char (*out_hashes)[SH_MODSERVE_HASH_HEX + 1], int cap);

/* 1 when the root folder's build of `salt` (the local mod) provides the placed name `name` (whether a map's placed
 * object of that mod is in the installed version), 0 when it does not, -1 without an index. */
int sh_modserve_root_carries(const char *salt, const char *name);

/* Copy the published index's English captions for placed entities. Missing rows keep the map's saved label. */
void sh_modserve_placed_labels(const char *const *names, size_t count, char **titles);

/* The save-time closure in the form the map manifest consumes: `names` are the logical names a map places, matched
 * case-insensitively. `visit` is called once per (bundle, placed name) match, then once with name NULL for each
 * further bundle the needs closure reaches. Returns the distinct bundles reported; stock names match nothing. */
typedef void (*sh_modserve_closure_fn)(void *context, const char *bundle_id, const char *bundle_hash,
                                       unsigned long long size, const char *name);
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context);

/* A path several admitted bundles of different salts provide with different bytes. With map scope on, the map scope
 * rules above pick the provider per map; the static winner, which serves with map scope off, has the fewest needs no
 * admitted bundle provides, then the bytes most providers carry, then salt and hash order. When every provider's
 * text is a decl that differs only inside lists, the winner's text is served with the others' list entries
 * appended (composed). Pointers stay valid while their table lives (sh_modserve_index_retire). */
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
 * when it has none), and `place` 1 for a composition of the content cache. */
typedef void (*sh_modserve_bundle_fn)(void *context, const char *id,
                                      const char *hash, int admitted, const char *cause,
                                      int files, int place);
void sh_modserve_each_bundle(sh_modserve_bundle_fn fn, void *context);
/* The bundle at `bundle_hash` as a status line names it (malloc'd): its name
 * and folder ("<name> (mods\<folder>)"), else its folder; "a mod" when no
 * bundle has that hash. */
char *sh_modserve_bundle_label(const char *bundle_hash);
/* The bundle at `bundle_hash` as a player sentence names it (malloc'd): its name, else its folder's own name; "a mod"
 * when no bundle has that hash. Never a salt, a hash or a path. */
char *sh_modserve_bundle_name(const char *bundle_hash);

/* Visit every admitted bundle whose mod.json author block carries keys the product does not read itself (the
 * manifest's "extra"): root folders before compositions, then by salt, then by folder. `place` is 1 for a
 * composition; `extra` is JSON text valid while its table lives. Returns the number visited, or -1 on no memory. */
typedef void (*sh_modserve_extra_fn)(void *context, const char *id, const char *hash, int place, const char *extra);
int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context);
/* Installed editor documents in permanent salt order. */
int sh_modserve_each_local_editor(sh_modserve_extra_fn fn, void *context);
/* Reproject conditional property sheets after a local setting changes. */
int sh_modserve_editor_preferences_changed(void);
/* Native row with inherited property sheets before layout, for authoring IDs. The caller frees the text. */
char *sh_modserve_editor_row_native(const char *row, size_t *len);

/* Visit every admitted bundle in index order with its salt, hash and its product block's "registries" object (JSON
 * text, NULL when none, valid while its table lives). Returns the number visited. Used by modsound.c for the stock
 * banks a bundle needs. */
typedef void (*sh_modserve_registries_fn)(void *context, const char *id, const char *hash, const char *registries);
int sh_modserve_each_bundle_registries(sh_modserve_registries_fn fn, void *context);

/* Visit every file the published table serves (admitted bundles, conflict winners, not withdrawn), in index order;
 * the strings stay valid while their table lives. Used by modvt.c to find the bundles' virtual-texture members. */
typedef void (*sh_modserve_file_fn)(void *context, const char *request, uint64_t size, const char *bundle_id,
                                    const char *bundle_hash);
void sh_modserve_each_served(sh_modserve_file_fn fn, void *context);
/* The same for the open map: per path, the file of the build that serves it (a map's own copy of a salt, not the
 * root folder's), and no path nothing serves in this scope; with no map open, the root folder's builds. Used by
 * modvt.c for the families a map's textures page from. */
void sh_modserve_each_served_here(sh_modserve_file_fn fn, void *context);

/* Mod scripts (modbundle.h sh_mb_script_request): every script of `salt` the index serves, from the build that serves
 * it, in request order. fn gets the script's path below scripts\ (lower case), its request, its size, the SHA-256 the
 * build recorded (what sh_modserve_open must answer with) and the serving build's hash. The runtime reads scripts
 * only this way, never from a folder (script_source.c). Returns the number visited. */
typedef void (*sh_modserve_script_fn)(void *context, const char *relpath, const char *request, uint64_t size,
                                      const char *sha256, const char *bundle_hash);
int sh_modserve_script_files(const char *salt, sh_modserve_script_fn fn, void *context);

/* Early decls: the serving index is published after the backend's signature scan, while the engine already parses
 * boot decls, so a shadow parsed before then holds the game's bytes. Visit, in plan rank order, every served shadow
 * decl the provider has never opened and every served decl first opened while its bundle's cvar requirements were
 * not live. `fn` returns 1 when it re-parsed the decl and 0 when the engine has not parsed it yet (offered again). */
typedef int (*sh_modserve_unopened_fn)(void *context, const char *type, const char *name, const char *request,
                                       const char *bundle_id);
int sh_modserve_each_unopened_shadow(sh_modserve_unopened_fn fn, void *context);

/* A bundle's cvar requirements (such as the cut-content blacklists) apply at the first registration pass, after
 * DOOM loaded its boot resources, so a model or image served before then was resolved with the old values. Visit
 * every served baseModel and image first opened while its requirements were not live, surfaces included
 * (sh_modserve_is_surface), once they are; `fn` returns 1 when it reloaded the resource. Returns the number visited. */
int sh_modserve_each_ungated_raw(sh_modserve_unopened_fn fn, void *context);

/* Refresh: a rescan replaces the table while the engine keeps every decl it parsed and every model and image it
 * loaded from the old bytes. Each publication compares, per request path, the new table with the one it replaced:
 * "changed" (bytes differ), "new" (only the new table serves it) or "gone" (only the old). Changed and gone-shadow
 * decls, and changed models and images, are marked pending and stay marked while bytes stay the same, so changes
 * recorded during Play apply at the next safe point. New requests are never applied: a new decl name registers at
 * the next map load that names its bundle, a new editor row at 'sh_mods palette rebuild'. */
#define SH_MODSERVE_RF_DECL  1
#define SH_MODSERVE_RF_MODEL 2   /* baseModel, and a static model (provides type "model", a .bmodel) */
/* 1 when `list_name`, a name the engine's static model list ("model", idStaticModel) holds (models/x.lwo), is the
 * model whose cooked request is `request` (cooked/model/models/x.bmodel), by the builder's model keys
 * (sh_mb_model_keys); a refresh reloads that list entry in place. */
int sh_modserve_static_model_same(const char *request, const char *list_name);
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

/* The published table's diff against the table it replaced (zero for the first table), plus the pending items. */
void sh_modserve_last_diff(sh_modserve_diff *out);

/* Mark, once per table, every served decl that inherits (top-level `inherit`, any depth) from a changed or gone
 * decl, since the engine resolves a parent when the child is parsed. Reads those types' served decls, stock
 * pointers included. Main thread; returns the number marked. */
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

/* Visit every pending item: changed decls in plan rank order, then gone shadow decls, then changed models and images.
 * `fn` returns 1 when the item is settled (re-parsed, reloaded, or not held by the engine) and 0 to keep it pending;
 * a decl settled here is not offered to the early-decl visit. Returns the number visited. */
typedef int (*sh_modserve_refresh_fn)(void *context, const sh_modserve_refresh_item *item);
int sh_modserve_each_refresh(sh_modserve_refresh_fn fn, void *context);

/* The editor closure. A boot-time editor pass registers every editor row and every served file a row needs inside
 * its resource watermark, so what the engine creates for them is raised to level 4 and survives the Play purge. A
 * refresh runs outside any watermark, so it applies its editor-closure part first inside a watermark of its own, then
 * the rest. PART_ALL visits everything (the editor pass), PART_EDITOR only closure files, PART_REST every other item. */
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

/* Split a plan into the steps that keep their decls (persist) and are not registered yet, and the rest, both in
 * plan order. Returns the number of kept steps (0: nothing to keep, both outputs empty), or -1. The parts own their
 * steps (sh_modserve_plan_free each). */
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

/* Walk the engine's resource lists (decl_resource_head): fn(context, the list's class name, the resource, its name).
 * Entries are collected under the engine's resource lock (sh_modserve_set_resource_lock) and `fn` runs after it is
 * released. 1 when the walk completed. */
typedef void (*sh_modserve_resource_fn)(void *context, const char *list_class, void *resource, const char *name);
int sh_modserve_resource_each(const void *head_slot, sh_modserve_resource_fn fn, void *context);

/* The engine's resource lock: `lock` is the address of the static struct whose first qword is the CRITICAL_SECTION
 * pointer every list add, remove and find takes (13954591: VK RVA 0x5D79238, GL 0x467E8B8). Every walk of the lists
 * here holds it while it reads the entry arrays. NULL (the default) walks without it. */
void sh_modserve_set_resource_lock(void *lock);

/* How many times the provider served `request` from the published table. */
long sh_modserve_open_count(const char *request);

/* Served-bytes listener for the ledger (modledger.h): called by sh_modserve_open on every successful serve, on the
 * serving thread, with the indexed request (lowercase, valid for the process), the serving bundle's hash, the file's
 * recorded SHA-256 (NULL for a composed text or none) and the bytes. NULL removes it. */
typedef void (*sh_modserve_serve_fn)(const char *request, const char *bundle_hash, const char *file_hash,
                                     const unsigned char *bytes, size_t length);
void sh_modserve_set_serve_listener(sh_modserve_serve_fn fn);

/* The text a mod's material decl (a material or material2 file of a folder, not a stock pointer) is served as: the
 * filter gets the request, the file's text and a buffer for a sentence, and answers 0 (served as it is), 1 (a malloc'd
 * text in *out) or 2 (a mistake: the sentence is logged and the file is served as held). The backend sets it to
 * modlook.h sh_look_compile before the first index. The text is derived once per indexed file and served like a
 * composed text, so the serve listener gets no file hash for it. */
typedef int (*sh_modserve_material_fn)(const char *path, const char *text, size_t len, char **out, size_t *out_len,
                                       char *why, size_t why_len);
void sh_modserve_set_material_filter(sh_modserve_material_fn fn);

/* Which requests the product derives itself (modprog.h sh_modprog_owns, set by the backend before the first index): a
 * stock pointer to such a decl restores a name no archive lists, so every pass registers it as it registers a mod's
 * own decls. Unset (the native tests), the name alone decides (SH_MB_PRODUCT_ROOT, modbundle.h sh_mb_product_decl). */
void sh_modserve_set_product_owner(int (*owns)(const char *request));

/* How the product's own bytes are read (modprog.h sh_modprog_read), set by the backend with the owner. With both set,
 * sh_modserve_open answers a request no build lists that names a file the product derives by its bytes' hash
 * (modbundle.h sh_mb_product_hashed) once a served material that names it has compiled in this process. The file then
 * serves wherever its material serves, in every mode, and no lookup, identity or serve listener sees it. */
void sh_modserve_set_product_reader(int (*read)(const char *request, unsigned char **bytes, size_t *len));

/* What sh_modserve_open would serve for `request`, without serving it: 1 with the bundle hash and the file's SHA-256
 * (*file_hash) or, for a composed text, its bytes (*body, *body_len; valid for the process), 0 when nothing serves
 * it. Both NULL when the file records no hash. */
int sh_modserve_serve_identity(const char *request, const char **bundle_hash, const char **file_hash,
                               const unsigned char **body, size_t *body_len, uint64_t *size);

/* 1 when the file the index serves for `request` is a stock pointer that restores the game's own bytes under their
 * own name (modbundle.h sh_mb_stock_restore): the engine holds those bytes parsed already, so an in-place re-parse
 * changes only what the decl points at. */
int sh_modserve_is_restore(const char *request);

/* 1 when the file the index keys at `request` (the path's static winner) is a surface (a stock pointer at the game's
 * own path). No pass registers, re-parses or refreshes a surface and the game parses it from its own served bytes,
 * except the gated reload (sh_modserve_each_ungated_raw). */
int sh_modserve_is_surface(const char *request);

/* Whether an in-place reload or re-parse (the gated reload, a refresh part) opens a keeper of its own (a resource
 * watermark raised to the session's level when it ends): 1 when it has work and runs outside every other watermark
 * (mark_depth 0). What a reload of a held resource creates must live as long as the held resource, or a purge frees
 * images a permanent material points at. */
int sh_modserve_inplace_keeper(int mark_depth, int work);

/* Functions called after every table this layer publishes (boot, rescan), on the thread that built it, outside the
 * build lock, in the order added; adding one twice is ignored, NULL clears all. Each call site brings its own link
 * (the static the macro declares), so no listener is ever dropped. */
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

/* Declared for every build: the pure tests link their own definitions (modserve_engine.c, modserve.c).
 * modserve_activate_bundles registers the gameplay decls of exactly these bundles (64-hex hashes) at the engine's own
 * lifetime (level 1, purged at map unload) when not registered yet; the map manifest (modmanifest.c) calls it on
 * every parse, main thread only, and n <= 0 returns 0 and logs nothing. It then registers the editor rows the built
 * palette lacks and rebuilds the palette once (sh_modserve_map_rows_sync). Returns the number of ready steps, or -1
 * when refused (not the main thread, registry unavailable). */
/* Make the named prefab objects available for native paste. Every requested entity must own a live palette row. */
int sh_modserve_prefab_rows(const char *const *inherits, size_t count, const char **missing);
int modserve_activate_bundles(const char *const *bundle_hashes, int n);
/* The listing's declared builds (the last sh_modserve_scope_set), in row order: up to cap copied into out (NULL to
 * count). Returns the count. */
int sh_modserve_scope_hashes(char (*out)[SH_MODSERVE_HASH_HEX + 1], int cap);
#ifndef SH_MODSERVE_PURE
#include "signatures.h"

/* Bind the decl registry, the process-heap scope and the main-thread identity (as timeline_tile.c does) and detour
 * the decl existence probe (DeclResourceProbe, vtable +0x78, seven arguments forwarded) when the index holds decls.
 * Call after the override provider is installed. Returns 1 when registration is available. */
int sh_modserve_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by the Timeline tile's SnapPaletteBuild detour before the original builder runs, on the main thread:
 * registers the editor plan's steps not registered yet and logs "MODS: editor rows registered in N ms (R rows)". Like
 * every pass here, it first applies the admitted bundles' requirements through idCVar::Set (immediate, no drain). */
void sh_modserve_palette_before_build(void);

/* Called after the original builder: logs the row count and how many rows
 * hold mod sedefs. */
void sh_modserve_palette_after_build(void *palette);

/* Rebuild the live editor palette so rows of bundles indexed after the first build appear: main thread only, refused
 * while the editor holds an entity or a wire, inside process heap 0 (verified on entry and exit); pending registrations
 * first, then SnapPaletteBuild(editor+0x20660, NULL) through the detour. Returns 1 when the builder ran, 0 when refused. */
int modserve_palette_rebuild_requested(void);

/* The pre-load pass of a hub launch (a published map or a My Maps play), called once per launch at its gate before the
 * launch's original runs (modui.c sh_modui_scope_launch): the game sits at the hub with no level load in progress.
 * Inside process heap 0 it does the decl work the level load would otherwise do: every editor row and its closure,
 * then any step of the launched map's declared builds the map pass did not settle. Main thread, not in Play; returns
 * the steps it ran, 0 when nothing was left, -1 refused. */
int sh_modserve_launch_preload(const char *why);

/* Called before the Play level load (the SnapBuild detour in nav_play.c), on the main thread: re-checks the accepted
 * bundles' requirements with sh_modserve_requirements_check (phase "play"), since the engine never resets the
 * blacklist gates between the editor and the level load; a cvar someone changed back is set again. */
void sh_modserve_play_begin(void);

/* 1 during SnapMap Play, 0 otherwise, -1 unknown (the editor's deactivation reason). */
int sh_modserve_engine_in_play(void);

/* Set one engine cvar by name through the requirements gate (immediate); 1 when set. */
int sh_modserve_engine_set_cvar(const char *cvar, const char *value);

/* 1 on DOOM's main thread, 0 on another, -1 unknown. */
int sh_modserve_engine_on_main(void);
/* The decl of `type` named `name` as the engine's own lookups find it (DeclFind without makeDefault): answers from
 * the resource lists and loads on demand a name the serving layer admits, at the engine's level. NULL when there is
 * none, the load failed or ended in a default, or off the main thread. */
void *sh_modserve_engine_decl(const char *type, const char *name);
/* Find only: 1 when the engine holds a resource of `type` named `name` (*out its object, *state its state byte,
 * *lifetime its level), 0 when not, -1 when the list cannot be read. Creates and loads nothing; any thread (it takes
 * the engine's resource lock). */
int sh_modserve_engine_decl_held(const char *type, const char *name, void **out, unsigned *state,
                                 unsigned *lifetime);

/* A scope change (sh_modserve_scope_set) brought into the running game, at a map load, the map exit, the Play entry
 * and in the editor after a save or a map-level change: the requirements pass first, then the pending re-parses and
 * reloads (me_refresh_apply) and the gated reloads. Main thread. Logs one "MODS: scope: <phase>: ..." line. Returns 1
 * applied, 0 nothing pending, -1 refused (off the main thread). */
int sh_modserve_scope_apply(const char *phase);

/* A declared set the author changed in the editor ("Use in this map"): the map load's activation for the listing's
 * builds, then the scope apply, so decls not registered this session are registered and every other pending change
 * is re-parsed or reloaded in place. Main thread, not in Play; elsewhere it is sh_modserve_scope_apply, whose
 * result it returns. */
int sh_modserve_scope_activate(const char *phase);

/* After a rescan published a table: apply its pending changes (main thread, not during Play) or leave them for the
 * next registration pass, which applies them between publishing its sources and running its loads. Fills *out (when
 * non-NULL) and returns its state: 0 nothing changed, 1 applied, 2 deferred. */
int sh_modserve_refresh_after_rescan(sh_modserve_refresh *out);

/* The serving subcommands of the sh_mods console command (modcmd.c owns the command and dispatches here): status,
 * rescan, palette rebuild. `args` is the engine's idCmdArgs. Returns 1 when `verb` is one of them (handled), 0
 * otherwise. */
int sh_modserve_command(void *args, const char *verb);
#endif


#ifdef __cplusplus
}
#endif

#endif
