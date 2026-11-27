/* Shared ABI between the backend and frontend DLLs; both build against this header. The 0x60-byte object has a
 * vtable at +0 and a subobject pointer at +0x58; the 77-slot prefix ends at +0x260 and append-only extensions end at
 * +0x3A0, making the vtable 0x3A8 bytes (the backend records the size, see sh_iface_vtbl_bytes, and the window refuses
 * a backend whose table is not its own size). Slots that hand text or bytes to the window return it, or store it in the
 * call record, as a process-heap NUL-terminated string the window releases with free_text (+0x3A0); NULL means none,
 * and text the window passes in is a NUL-terminated string read before the call returns. */
#ifndef SNAPMAP_PLUS_IFACE_H
#define SNAPMAP_PLUS_IFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registered command handler. Console dispatch calls inline on the engine
 * main thread; queued work instead runs on the thread that drains it.
 * ctx is the registered user pointer; argv remains valid for the call. */
typedef void (*sh_cmd_handler)(void *ctx, int argc, const char **argv);

/* Backend-owned callback signatures. Keep offsets and calling conventions
 * consistent across the matched DLLs; check optional callbacks before use. */
struct sh_iface;

#if defined(__cplusplus)
#define SH_STATIC_ASSERT(expr) static_assert((expr), #expr)
#elif defined(_MSC_VER)
#define SH_STATIC_ASSERT_JOIN_(a, b) a##b
#define SH_STATIC_ASSERT_JOIN(a, b) SH_STATIC_ASSERT_JOIN_(a, b)
#define SH_STATIC_ASSERT(expr) \
    typedef char SH_STATIC_ASSERT_JOIN(sh_static_assert_, __LINE__)[(expr) ? 1 : -1]
#else
#define SH_STATIC_ASSERT(expr) _Static_assert((expr), #expr)
#endif

typedef int          (*sh_get_selection_fn)(struct sh_iface *self, int *out_ids, int max);   /* +0x150 */
typedef void         (*sh_clear_selection_fn)(struct sh_iface *self);                          /* +0x148 */
typedef void         (*sh_add_to_selection_fn)(struct sh_iface *self, int id);                 /* +0x138 */
typedef int          (*sh_hovered_id_fn)(struct sh_iface *self);                               /* +0x198 */
typedef int          (*sh_is_entity_mode_fn)(struct sh_iface *self);                            /* +0x1c0 */
/* Return true for a valid entity. This ABI intentionally reverses the
 * original interface's true-when-invalid convention; keep both DLLs aligned. */
typedef int          (*sh_is_valid_id_fn)(struct sh_iface *self, int id);                      /* +0x28  */
typedef int          (*sh_entity_count_fn)(struct sh_iface *self);                             /* +0x10  */
/* The entity's target-reference string, classname and inherit, owned; NULL for an invalid id. */
typedef char        *(*sh_id_to_string_fn)(struct sh_iface *self, int id);                     /* +0x18  */
typedef char        *(*sh_classname_fn)(struct sh_iface *self, int id);                        /* +0x48  */
typedef char        *(*sh_inherit_fn)(struct sh_iface *self, int id);                          /* +0x50  */
typedef void         (*sh_toast_fn)(struct sh_iface *self, const char *title, const char *text);/* +0x1b8 */

/* Entity, prefab, and timeline operations. */

/* The display name, owned; NULL for an invalid id. */
typedef char        *(*sh_get_displayname_fn)(struct sh_iface *self, int id);                  /* +0x58 */

/* The entity's own decl source, owned; NULL for an invalid id. */
typedef char        *(*sh_get_declsource_fn)(struct sh_iface *self, int id);                   /* +0x30 */

/* Set nonempty classname when the resulting class/inherit pair is not known invalid. */
typedef void         (*sh_set_classname_fn)(struct sh_iface *self, int id, const char *cstr);      /* +0x78 */
/* Set nonempty inherit when the resulting class/inherit pair is not known invalid. */
typedef void         (*sh_set_inherit_fn)(struct sh_iface *self, int id, const char *cstr);        /* +0x80 */
/* Set the full display-name idStr; empty names are allowed. */
typedef void         (*sh_set_displayname_fn)(struct sh_iface *self, int id, const char *cstr);    /* +0x128 */
/* Rebuild canonical decl source from current class/inherit and the supplied body. */
typedef void         (*sh_rebuild_declsource_fn)(struct sh_iface *self, int id, const char *cstr); /* +0x40 */

/* Serialize selected entities as prefab JSON, owned; NULL on failure. */
typedef char        *(*sh_serialize_selection_fn)(struct sh_iface *self);                      /* +0xb0 */

/* %LOCALAPPDATA%/snapmap-plus/<prefix><name> as UTF-8, owned (open it with the wide file calls);
 * the caller supplies separators and suffix. NULL when the folder is unknown. */
typedef char        *(*sh_resolve_prefab_path_fn)(struct sh_iface *self, const char *prefix,
                                                  const char *name);                           /* +0xc0 */

/* Names from the decl manager for a short decl type, owned in *out: each name followed by its NUL, the block ended by
 * an empty name. Sets out_count and returns 1 for a nonempty result; unknown types return 0 with *out NULL. */
typedef int          (*sh_enum_decls_of_resclass_fn)(struct sh_iface *self, const char *res_class,
                                                    char **out, int *out_count);               /* +0x110 */

/* Validate the final class/inherit pair once, then assign the supplied fields (null/empty keeps a field). Returns 1
 * when all writes succeed, 0 on refusal or successful rollback, -2 when rollback also faults; rebuild decl source only
 * after 1. */
typedef int          (*sh_apply_class_inherit_fn)(struct sh_iface *self, int id,
                                                  const char *cls, const char *inh);              /* +0x268 (ext 0) */

/* Classes derived from the inherit's base, packed as enum_decls_of_resclass packs, from collected live type records
 * (static fallback when unavailable); an empty inherit uses idEntity. Returns 1 and the count for a nonempty result. */
typedef int          (*sh_enum_valid_classes_fn)(struct sh_iface *self, const char *inherit,
                                                 char **out, int *out_count);                  /* +0x270 (ext 1) */

/* Sorted, deduplicated loaded entityDef paths, packed the same way; returns 1 and the count, or 0 for frontend
 * fallback. Calls must be serialized (shared enumeration scratch). */
typedef int          (*sh_enum_inherits_fn)(struct sh_iface *self, char **out, int *out_count); /* +0x278 (ext 2) */

/* Hide non-base-layer entities only when the dev-layer cvar is confirmed disabled; an unknown cvar or entity state
 * leaves them visible. This list filter omits the native picker's extra mask. */
typedef int          (*sh_id_dev_layer_hidden_fn)(struct sh_iface *self, int id);                /* +0x280 (ext 3) */

#define SH_APPLY_IN_PROGRESS (-2)
struct sh_apply_item;
/* Returns the completed item count, or SH_APPLY_IN_PROGRESS when the engine started but has not finished by the wait
 * deadline (do not retry or report it as a failure). Unknown thread identity, missing transport and busy slots
 * refuse execution; marshaling owns a complete text copy. */
typedef int          (*sh_apply_sync_fn)(struct sh_iface *self, const struct sh_apply_item *items,
                                         int count, const char *op_label);                        /* +0x290 (ext 5) */

/* Push IDs with deduplication onto the backend stack shared with console commands. */
typedef void          (*sh_push_to_stack_fn)(struct sh_iface *self, int index, const int *ids, int count); /* +0x2A0 (ext 7) */

/* Clear a backend stack and return its previous ID count. */
typedef int           (*sh_clear_stack_fn)(struct sh_iface *self, int index);                      /* +0x2A8 (ext 8) */
/* Nonzero when the selection-mutation guard refuses: 1 for a grab (manipulation) or certain unreadable states, 2 while
 * a native editor action such as Delete Selection finishes its commit. Check before changing selection; the
 * add/clear/remove callbacks apply the same guard. */
typedef int           (*sh_manipulation_in_progress_fn)(struct sh_iface *self);                     /* +0x2C0 (ext 11) */

/* +0x2D0 (ext 13) Consume the latest asset preview as a `data:image/png;base64,...` URI, owned;
 * NULL when nothing is published. */
typedef char         *(*sh_get_preview_fn)(struct sh_iface *self);

/* Preserve the string-only request ABI: this non-path prefix distinguishes
 * direct Image previews from same-named Materials. */
#define SH_PREVIEW_IMAGE_ROUTE_PREFIX "\x1Fimage:"

/* Request asynchronous preview work (poll get_preview); null/empty cancels staged work and unconsumed output. The
 * route prefix selects an Image and plain names a Material-first lookup; returns whether it was accepted. */
typedef int           (*sh_request_preview_fn)(struct sh_iface *self, const char *name);

/* Asset kind IDs cross the ABI: append only, never renumber. Material and
 * Image retain the container catalog IDs 0 and 1. */
#define SH_ASSET_MATERIAL    0
#define SH_ASSET_IMAGE       1
#define SH_ASSET_MODEL       2
#define SH_ASSET_SOUND       3
#define SH_ASSET_FX          4
#define SH_ASSET_PARTICLE    5
#define SH_ASSET_DECALATLAS  6
#define SH_ASSET_SNAPDEF     7    /* snapEditorEntityDef -- the SnapMap editor's placeable list */
#define SH_ASSET_ENTITYDEF   8
/* Module models pair with combo collision; other baked brush pieces are
 * render-only. Clip models can be applied separately as collision geometry. */
#define SH_ASSET_MODULE      9
#define SH_ASSET_BMODEL      10
#define SH_ASSET_CLIPMODEL   11
/* Material qualifier: atlas names without a material decl. These support
 * virtualmapping but cannot be applied through customMaterial. */
#define SH_ASSET_VTONLY      12
/* Sound qualifier: event|bank rows for local bank filtering. */
#define SH_ASSET_SNDBANK     13
/* Reference catalogs: perk names and SWF paths. SWFs use decl-reference
 * paths (swf/...swf), not generated baked-artifact paths. */
#define SH_ASSET_PERK        14
#define SH_ASSET_SWF         15
/* Light projection/falloff materials: light decls plus atlas-only rows.
 * Kept separate from surface Materials for the lightMaterial field. */
#define SH_ASSET_LIGHT       16
/* PROJECTILE: the `projectile` decl type, reference-only: a weapon or ammo decl names it (`projectileDecl` and
 * siblings) and it is never placed or applied, so it is a name copied and wired by hand. */
#define SH_ASSET_PROJECTILE  17
/* WEAPON: the `weapon` decl type, reference-only like PROJECTILE. It is the id idTarget_FireWeapon and
 * idTarget_DummyFire take: they fire a WEAPON decl, which names the projectile/ammo it launches. */
#define SH_ASSET_WEAPON       18
#define SH_ASSET_COUNT       19

/* Every name of an asset kind, one per line, owned; NULL when the catalog is
 * unavailable. */
typedef char         *(*sh_list_assets_fn)(struct sh_iface *self, int kind);
/* A material's .vmtr atlas rect -> out_xywh = {x,y,w,h} in atlas pixels; 1 if virtual-textured,
 * 0 if it has no rect. Divide by 245760 for the `virtualmapping` renderParm value form. */
typedef int           (*sh_material_rect_fn)(struct sh_iface *self, const char *name, int *out_xywh);
/* Main thread only: audition one sound, replacing any previous preview.
 * Null/empty stops playback; return 1 when playing. */
typedef int           (*sh_sound_preview_fn)(struct sh_iface *self, const char *name);
/* Main thread only: hold audition mode for a browser session. Turning it
 * off stops playback; session scope avoids cvar changes on every click. */
typedef void          (*sh_sound_session_fn)(struct sh_iface *self, int on);

/* Mesh requests are
 * asynchronous and generation-keyed and empty cancels stale work; get consumes one binary completion into *out
 * (owned) and returns its byte count, 0 when none is ready. */
typedef int (*sh_request_prefab_mesh_fn)(struct sh_iface *self, unsigned long generation,
                                         const char *model_name);
typedef int (*sh_get_prefab_mesh_fn)(struct sh_iface *self, void **out);
#ifndef SH_PREFAB_DEFAULT_MODEL
#define SH_PREFAB_DEFAULT_MODEL 0x1
#define SH_PREFAB_DEFAULT_SCALE 0x2
#define SH_PREFAB_DEFAULT_EDITOR 0x4 /* model is the palette row's renderInfo.renderModelName */
#define SH_PREFAB_DEFAULT_SIZE  0x8 /* out_scale[3..5] holds that row's renderInfo.size */
#endif
/* The navigation view. `update` asks for it to be brought up to date on the
 * game's next frame; 0 only reports. Returns {"stale","pending","floors","walls",
 * "red","rooms","refused",...} as JSON, owned. */
typedef char *(*sh_navmesh_view_fn)(struct sh_iface *self, int update);

/* Run fn(ctx) on DOOM's main thread after the next native frame (inline when already there). Returns 1 with
 * *out_result set when fn ran, 0 when it did not start within timeout_ms; once started the caller waits for it, so fn
 * must never wait on the caller's thread. */
typedef int (*sh_main_call_fn)(void *ctx);
typedef int (*sh_run_on_main_fn)(struct sh_iface *self, sh_main_call_fn fn, void *ctx,
                                 int timeout_ms, int *out_result);

/* Delete entities through DOOM's own Delete Selection, with its eligibility, restriction and busy checks; main thread
 * only (call inside run_on_main). Fills out when nonnull and returns its status. DISPATCHED: every target's slot was
 * released before the call returned, and the undo step and return to idle finish on later frames, during which product
 * selection writes are refused; INCOMPLETE: fewer targets than requested were released and they stay deleted; FAULT:
 * an engine call raised, and the editor may be partly changed unless rolled_back = 1. Other statuses are refusals
 * before the action that change nothing, except that a hovered non-target may lose its hover. */
#define SH_DELETE_DISPATCHED   1
#define SH_DELETE_UNAVAILABLE  0     /* engine boundaries unresolved */
#define SH_DELETE_NOT_MAIN    (-1)
#define SH_DELETE_NOT_READY   (-2)   /* no editor/map/screen, or a state change */
#define SH_DELETE_NOT_ENTITY  (-3)   /* the editor is not in object (entity) editing */
#define SH_DELETE_BUSY        (-4)   /* a native action, grab or queued input owns the selection */
#define SH_DELETE_INVALID     (-5)   /* empty/oversized request, or a dead or unknown ID */
#define SH_DELETE_RESTRICTED  (-6)   /* the map's editor restrictions block Delete */
#define SH_DELETE_PORTAL      (-7)   /* a target is a portal door or frame */
#define SH_DELETE_PROTECTED   (-8)   /* the engine reports a target is not deletable */
#define SH_DELETE_REFERENCED  (-9)   /* the engine's referenced-entity refusal */
#define SH_DELETE_FAULT       (-10)  /* an engine call faulted */
#define SH_DELETE_INCOMPLETE  (-11)  /* the action released fewer targets than requested */
/* busy detail for SH_DELETE_BUSY */
#define SH_DELETE_BUSY_SUBSTATE     1   /* placing, editing properties or another mode */
#define SH_DELETE_BUSY_TRANSITION   2   /* leaving such a mode has not finished */
#define SH_DELETE_BUSY_PHASE        3   /* a native action is still committing */
#define SH_DELETE_BUSY_QUEUED       4   /* native input is queued for the next frame */
#define SH_DELETE_BUSY_MANIPULATION 5   /* a grab or hold owns the selection */
typedef struct sh_delete_result {
    int status;         /* SH_DELETE_*, also the return value */
    int busy;           /* SH_DELETE_BUSY_* detail when status is SH_DELETE_BUSY */
    int requested;      /* IDs supplied */
    int targets;        /* distinct valid targets */
    int released;       /* targets released by the action */
    int first_id;       /* offending ID for INVALID/PORTAL, else -1 */
    int deselected;     /* other selected entities dropped by a dispatch */
    int hover_cleared;  /* a hovered non-target lost its hover */
    int wire_cleared;   /* a hovered connection touching a target was dropped */
    int rolled_back;    /* a refusal restored the previous selection */
} sh_delete_result;
typedef int (*sh_delete_entities_fn)(struct sh_iface *self, const int *ids, int count,
                                     sh_delete_result *out);                  /* +0x350 (ext 29) */

/* What the grab-frame guard did during the latest serialize_selection; status is SH_PREFAB_GRAB_* (backend/prefab_grab.h):
 * 0 unchecked, 1 native frame hit, 2 corrected from the hovered box, 3 no finite frame (serialize_selection returned 0). */
typedef struct sh_prefab_grab_info {
    int   status;
    float native_distance;  /* grabDistance the engine computed */
    float distance;         /* grabDistance written to the prefab */
    float outside;          /* native grab point's distance outside the hovered box, <0 unknown */
} sh_prefab_grab_info;
typedef int (*sh_prefab_grab_status_fn)(struct sh_iface *self, sh_prefab_grab_info *out); /* +0x358 (ext 30) */
/* Module prefabs: Blueprint modules saved with their entities and placed in another map's Blueprint mode
 * (backend/module_prefab.c, prefab_mods.c). Details and the mods a prefab needs live in the prefab file's "snapmapPlus"
 * block (backend/prefab_block.h), with earlier prefabs refused, and cross as a
 * flat view {"desc","tags",...summary...,"mods"}. SAVE, CHECK, MODS_RESOLVE, EXPORT and IMPORT marshal to the game
 * thread and wait, so never call them from it; the other ops run on any thread. */
/* Fills blueprint (Blueprint state 1) and entity_mode (Object Mode, state 2); 1, or 0 when no editor is open. */
#define SH_MODULE_PREFAB_STATE  1
/* Cut the selected modules out of the open map and write them to path with a snapmapPlus block; out is the view.
 * 1 saved, -1 failed, -2 not in Blueprint, -3 no module selected. */
#define SH_MODULE_PREFAB_SAVE   2
/* Validate text (a module prefab file) against the editor's module palette, Grid Room sizes and served mod modules,
 * merge it into the open map and stage the install. 2 accepted (poll STATUS with seq), -1 unreadable, -4 not in
 * Blueprint, -6 needs a module the editor cannot build (toast text in out, nothing sent to the engine), 0 refused. */
#define SH_MODULE_PREFAB_PLACE  3
/* For the placement seq: 2 pending, 1 placed and grabbed, 0 refused by the engine, -5 over budget, -1 unreadable. */
#define SH_MODULE_PREFAB_STATUS 4
/* The PLACE module check for text without placing; out is {"checked":true,"count":N,"missing":[...],"status":"..."}
 * or {"checked":false}. Returns the missing count, -1 when it could not check; a module provided by a row of text's
 * snapmapPlus block is not missing. */
#define SH_MODULE_PREFAB_CHECK  5
/* text: the prefab the engine just serialized; path: its file. Writes a fresh block of mod rows (the map manifest's
 * save-time closure over the prefab) into the file; out is the view. Returns the row count, -1 failed (file unchanged). */
#define SH_MODULE_PREFAB_MODS_RECORD  6
/* text: a view; out is {"count":N,"missing":M,"mods":[{"id","salt","build","size","installed","state","cause"}...]}
 * "id" is the display name, "installed" means the prefab's required content is here. Returns M, -1 unreadable. */
#define SH_MODULE_PREFAB_MODS_STATUS  7
/* text: the prefab file; name: its name. Returns 1 place now, 2 held while local content is prepared
 * (poll MODS_POLL with seq), -6 a module the player cannot get (DLC), -7 refused,
 * 0 no answer; out is the message for -6, -7 and 2. The caller strips the block before the engine sees the text. */
#define SH_MODULE_PREFAB_MODS_RESOLVE 8
/* seq from MODS_RESOLVE: 2 waiting, 1 ready (place now), 0 refused or dropped (the game showed why). */
#define SH_MODULE_PREFAB_MODS_POLL    9
/* text: a view from the page ("" clears), of which only desc and tags are taken; rewrites path's block in place
 * (starting from the sidecar when the file has none; the sidecar is unchanged). 1 written, -1 failed. */
#define SH_MODULE_PREFAB_META_WRITE   10
/* path: the prefab file; name (UTF-8); zip_path. Writes the prefab with its mods as one ZIP
 * (modexport.h); out is the message. 1 written, -1 refused. */
#define SH_MODULE_PREFAB_EXPORT       11
/* Import the ZIP or JSON at zip_path into the library at path, caching any included mod content.
 * Write {"ok","added","name","message"} to out and return 1 for a new prefab, 0 for an existing prefab, or -1 on refusal. */
#define SH_MODULE_PREFAB_IMPORT       12
/* text: the prefab file, or its end when tail is 1. out is the view (empty without details).
 * Returns the source: 1 the block, 2 an earlier shape, 0 neither. */
#define SH_MODULE_PREFAB_META_READ    13
typedef struct sh_module_prefab_call {
    int         op;
    const char *path;        /* SAVE, MODS_RECORD, META_WRITE, EXPORT: prefab file; IMPORT: prefabs folder */
    int         editor_context; /* STATE out; requested map for PLACE and MODS_RESOLVE */
    const char *text;        /* PLACE, CHECK, MODS_RECORD, MODS_RESOLVE, META_READ: prefab file text;
                                MODS_STATUS, META_WRITE: a view */
    size_t      text_len;
    char       *out;         /* out, owned (NULL in): SAVE, MODS_RECORD, META_READ: the view;
                                STATUS/PLACE: message */
    int         seq;         /* PLACE out; STATUS in; MODS_RESOLVE held token out or resuming token in; MODS_POLL in */
    int         blueprint;   /* STATE out */
    int         entity_mode; /* STATE out */
    const char *name;        /* MODS_RESOLVE, EXPORT: the prefab's name */
    const char *zip_path;    /* EXPORT, IMPORT: the ZIP (IMPORT: or a .json in an extracted export) */
    int         tail;        /* META_READ in: text is the end of a larger file */
    int         map_open;    /* STATE out: the editor holds a map object (0 at the hub or after the editor left it) */
} sh_module_prefab_call;
typedef int (*sh_module_prefab_fn)(struct sh_iface *self, sh_module_prefab_call *call); /* +0x360 (ext 31) */
/* Editor budget table for the Prefabs tab's Limits section (backend/budget_table.c); out is UTF-8 JSON, owned. TABLE
 * (any thread) copies the table built on the game thread after the last palette build (limits, usage, reached, layers,
 * sedefs per palette row, inherits inherit -> row, modules, aliases leaf name -> module) and returns 1, or 0 when none
 * is built yet. USAGE (any thread) copies {"limits","usage","reached","gen"} from the last editor frame and returns 1,
 * or 0 before the first frame; both set generation, which changes whenever the table is rebuilt. */
#define SH_BUDGET_TABLE 1
#define SH_BUDGET_USAGE 2
#define SH_BUDGET_GENERATION 3  /* Returns the generation without copying the table. */
typedef struct sh_budget_call {
    int          op;
    char        *out;          /* out, owned (NULL in) */
    unsigned int generation;   /* out: table generation, 0 = none built */
    int          reached;      /* out: the limits-reached byte (USAGE) */
} sh_budget_call;
typedef int (*sh_budget_fn)(struct sh_iface *self, sh_budget_call *call); /* +0x370 (ext 33) */
/* Mods tab (backend/modui.c). Text crosses as UTF-8 JSON through one buffer the backend keeps: LIST and RUN fill it
 * and FETCH hands out a copy in out (owned). Every op sets generation, which changes after each action that can
 * change the store or the serving index. */
/* Main thread (inside run_on_main): builds the document of mod folders (one row each) and the open map's manifest
 * rows and placed-name closure. Returns 1, or 0 when refused (off the main thread, no memory). */
#define SH_MODS_LIST    1
/* Runs `action` with `arg` and keeps its result document; returns 1 when the action ran (the document's "ok" says
 * whether it succeeded), 0 when refused before it ran. Main thread, except INSTALL, NEW_MOD, RESCAN and ADD_MINE,
 * which may run on any thread with no time limit and, off the main thread, answer "apply": true to ask for APPLY on
 * the main thread; KEY runs on any thread, touches no engine, and its arg (a passphrase) is never logged. */
#define SH_MODS_RUN     2
/* Any thread: the buffer in out; 1, or 0 when nothing is built yet. */
#define SH_MODS_FETCH   3
/* Any thread: the store folder as UTF-8 in out; 1, or 0 when unresolved. */
#define SH_MODS_ROOT    4
/* Any thread: 1 with busy = 1 while a store write holds the store lock (else 0) and generation = the store lock's
 * released holds (not the document generation). */
#define SH_MODS_STORE   6
/* Any thread, read only; arg: a mod's salt, or empty. out (not the shared buffer) is the mod scripts' state of the
 * current or last Play: every mod whose scripts ran, with its stop line, and arg's script log (modui.h
 * sh_modui_scripts). Returns 1, or 0 on failure. */
#define SH_MODS_SCRIPTS 7
#define SH_MODS_GENERATION 8   /* any thread: 1 with generation set, nothing else */
/* SH_MODS_SCRIPTS answers a log's tail; an arg of this prefix and the salt asks for the whole log (Copy log). */
#define SH_MODS_SCRIPTS_FULL "full:"
/* Op 5 and actions 2, 5 to 9, 14, 15, 17 and 19 are unassigned; the slot answers them "Unknown action.". */
#define SH_MODS_ACT_INSTALL     1    /* arg: absolute folders, newline-separated: install them (any thread) */
#define SH_MODS_ACT_RESCAN      3    /* Refresh: build every changed folder, then bring the changes into the game (any thread) */
#define SH_MODS_ACT_PALETTE     4
#define SH_MODS_ACT_UNINSTALL          10    /* arg: a folder: clean the open map of it, clear its undo history, delete the folder */
#define SH_MODS_ACT_UNINSTALL_PREVIEW  11    /* arg: a folder: what UNINSTALL would do; changes nothing */
#define SH_MODS_ACT_MAP_REMOVE         12    /* arg: a folder (or the salt of a row not installed): remove it from the open map */
#define SH_MODS_ACT_MAP_REMOVE_PREVIEW 13    /* arg: as MAP_REMOVE: what it would do; changes nothing */
#define SH_MODS_ACT_MAP_ADD            16    /* arg: a folder: attach the mod to the open map (a map-level row) */
#define SH_MODS_ACT_CHOOSE             18    /* arg: "<slot>\t<salt>": the open map's registry choice (an empty salt clears) */
#define SH_MODS_ACT_FILES              20    /* arg: a folder: its files (read-only; no fresh list follows) */
#define SH_MODS_ACT_FILE_TEXT          21    /* arg: "<folder>\t<path below the folder>": one text file (read-only; no list) */
#define SH_MODS_ACT_NEW_MOD            22    /* arg: a display name: a new mod folder with a working example, then the rescan (any thread) */
#define SH_MODS_ACT_APPLY              23    /* main thread: finish the file action that ran off it (its "apply": true) */
#define SH_MODS_ACT_ADD_MINE           24    /* arg: a content line's row: Add to my mods from a map's units, a new mod of your own (any thread) */
#define SH_MODS_ACT_MAP_UPDATE         26    /* arg: a salt: move the open map's row of that mod to the installed version */
#define SH_MODS_ACT_KEY                27    /* arg: the publishing key's Settings request (modclaim.h modclaim_key_action; any thread, no list follows) */
#define SH_MODS_ACT_EDITOR_IDS         28    /* arg: native Toybox tile ID */
#define SH_MODS_ACT_CONTENT            25    /* arg: "" map content on disk; "removePlayed": Remove content of maps you only played, then it (any thread) */
typedef struct sh_mods_call {
    int          op;
    int          action;       /* RUN */
    const char  *arg;          /* RUN, SCRIPTS */
    char        *out;          /* out, owned (NULL in): FETCH, ROOT, SCRIPTS */
    int          busy;         /* out: STORE */
    unsigned int generation;   /* out */
} sh_mods_call;
typedef int (*sh_mods_fn)(struct sh_iface *self, sh_mods_call *call); /* +0x378 (ext 34) */

/* The Prefabs tab's watch on the editor (backend/iface_engine.c), read about three times a second on the host's own
 * thread: every field is a guarded read of the last editor frame and nothing waits for the game. Returns 1, or 0
 * when no editor is open (the fields are then zero). */
#define SH_PREFAB_HELD_NONE     0
#define SH_PREFAB_HELD_OBJECTS  1
#define SH_PREFAB_HELD_MODULES  2
#define SH_PREFAB_MAP_CHANGED "The map changed while the prefab was being prepared. Place it in the open map when ready."
#define SH_PREFAB_PLACE_MAP_CHANGED (-1)
#define SH_PREFAB_PLACE_FAILED  0
#define SH_PREFAB_PLACE_STAGED  1   /* staged only: Ctrl+V in the 3D view places it */
#define SH_PREFAB_PLACE_QUEUED  2   /* the native paste was requested: it arrives in hand */
typedef struct sh_prefab_watch {
    int  modules_selected;  /* modules selected in Blueprint, 0 outside it, -1 unreadable: the Create gate */
    int  held;              /* SH_PREFAB_HELD_*: a Load / Place held in the 3D view; the editor budget counts it */
    int  place_seq;         /* object pastes Load / Place asked for; changes once the game thread decides each */
    int  place_result;      /* that decision: SH_PREFAB_PLACE_* */
    char *place_reason;     /* for STAGED, why the paste was not queued, as a phrase for the page; owned, or NULL */
} sh_prefab_watch;
typedef int (*sh_prefab_watch_fn)(struct sh_iface *self, sh_prefab_watch *out); /* +0x380 (ext 35) */

/* +0x388 (ext 36): the features that are off this session, as the one line the
 * Snapmap+ window shows ("These features are unavailable in this session:
 * ..."), owned; NULL when every feature is available. Any thread. */
typedef char *(*sh_features_off_fn)(struct sh_iface *self);

/* +0x390 (ext 37): why the latest image preview request (request_preview)
 * failed, as one plain sentence, owned; NULL while it is pending, published or
 * cancelled. Any thread. */
typedef char *(*sh_preview_failure_fn)(struct sh_iface *self);

/* +0x398 (ext 38): why the latest sound audition (sound_preview) was refused,
 * at once or later on the game thread ("Sounds play only inside a map."),
 * owned; NULL when it was accepted. Any thread. */
typedef char *(*sh_sound_refusal_fn)(struct sh_iface *self);

/* +0x3A0 (ext 39): release text a slot handed over (NULL is ignored). Any thread. */
typedef void (*sh_free_text_fn)(struct sh_iface *self, void *text);

/* +0x320 (ext 23): the preview model (*out_model, owned, with SH_PREFAB_DEFAULT_MODEL) and renderModelInfo.scale for
 * an entityDef inherit, as SH_PREFAB_DEFAULT_* bits. When the def chain names no .lwo/.bmodel/.md6, the model is the
 * one the editor draws for the inherit's palette row (SH_PREFAB_DEFAULT_EDITOR), and with out_scale_count >= 6
 * out_scale[3..5] receives that row's renderInfo.size (SH_PREFAB_DEFAULT_SIZE). */
typedef int (*sh_resolve_prefab_defaults_fn)(struct sh_iface *self, const char *inherit_name,
                                             char **out_model,
                                             float *out_scale, int out_scale_count);

/* +0x2B0/+0x2B8 (ext 9/10): backend-owned persistent configuration crossing as complete UTF-8 JSON fragments.
 * `get` stores the value in *out_json, owned, and returns its byte count excluding the NUL, or a negative refusal
 * (*out_json NULL); the registry in backend/config.c enforces frontend access. */
#define SH_CONFIG_SET_VOLATILE  (-1)
#define SH_CONFIG_SET_REJECTED  0
#define SH_CONFIG_SET_PERSISTED 1

#define SH_CONFIG_STATUS_RECOVERED_CORRUPT  0x01u
#define SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA 0x02u
#define SH_CONFIG_STATUS_VOLATILE           0x04u
#define SH_CONFIG_STATUS_REPAIRED           0x08u

typedef int (*sh_config_get_json_fn)(struct sh_iface *self, const char *key,
                                     char **out_json, unsigned int *out_flags); /* +0x2B0 (ext 9) */
typedef int (*sh_config_set_json_fn)(struct sh_iface *self, const char *key,
                                     const char *value_json);                /* +0x2B8 (ext 10) */

/* Rawmap file slots for the File menu. Status crosses as a UTF-8 JSON fragment,
 * owned, so both paths and the save counters travel in one call. */
typedef char *(*sh_rawmap_status_fn)(struct sh_iface *self);                 /* +0x328 (ext 24) */

/* Paths are UTF-8 and NULL leaves that side unchanged. arm: -1 leave; 2 open the nonempty load path now (checked, and
 * refused when the editor cannot open it; an empty load path restores the default); 3 keep the current save path;
 * 4 release it; 5 queue a save to the current backend destination; 0 and 1 are refused. A nonempty save path queues an
 * export of the live map to that file and an empty one releases the destination; refused saves neither change the
 * destination nor queue a later save. *out_msg receives the player's sentence, owned (NULL when none). */
typedef int (*sh_rawmap_configure_fn)(struct sh_iface *self,
                                      const char *load_path, const char *save_path, int arm,
                                      char **out_msg);                        /* +0x330 (ext 25) */

/* Open the load path in the editor on the next frame; returns 1 when the request is accepted, not when the map is
 * open (the frame hook re-checks and tells the player of a failure). Requires overwrite protection unless the user
 * disabled it; *out_msg receives the sentence, owned. */
typedef int (*sh_rawmap_load_now_fn)(struct sh_iface *self, char **out_msg);  /* +0x338 (ext 26) */

/* Reflection and apply callbacks. Engine work prefers the main-thread
 * command drain; apply_sync documents its inline compatibility fallback. */

/* Serialize a full idSnapEntity as JSON, owned; NULL when unavailable. */
typedef char        *(*sh_serialize_entity_fn)(struct sh_iface *self, int id);                /* +0xc8 */

/* Shared apply item. Kind 0 commits entity text; 1 stages prefab text;
 * 2 stages and arms native paste; 3 writes a target reference. Scheduling
 * deep-copies text into a single pending batch, replacing older pending work. */
#define SH_APPLY_PLACE_IN_MAP 5
typedef struct sh_apply_item {
    int         kind;       /* 0 entity edit, 1 stage, 2 stage/paste, 3 target write, 4 fly to entity, 5 place in map id. */
    int         id;         /* Live entity ID for kinds 0/3/4; ignored for prefab staging. */
    const char *text;       /* Entity/prefab JSON, or target ID text for kind 3; "" for kind 4. */
} sh_apply_item;

/* Deep-copy into the pending batch and enqueue the main-thread drain.
 * Return 1 when command text was submitted; execution completes later.
 * op_label identifies result reporting. An occupied queue refuses new work. */
typedef int          (*sh_schedule_apply_fn)(struct sh_iface *self, const sh_apply_item *items, int count,
                                             const char *op_label);                              /* +0xd0 */

/* Serialize the pending prefab as JSON, owned; NULL on failure. */
typedef char        *(*sh_read_prefab_fn)(struct sh_iface *self);                              /* +0xb8 */

/* Fixed vtable: 77 original cells (+0x00..+0x260) followed by append-only extensions; existing cells never move.
 * Typed callbacks are bound by the backend and void-pointer cells reserve unused offsets; the parenthesized RVAs are
 * the original interface's function addresses. */
typedef struct sh_iface sh_iface;

/* Set/read the editor camera-origin vec3. */
typedef void (*sh_set_editor_vec3_fn)(struct sh_iface *self, const float *xyz);   /* +0x00 (0x64a0) */
typedef void (*sh_get_editor_vec3_fn)(struct sh_iface *self, float *out_xyz);      /* +0x08 (0x6500) */
typedef int  (*sh_editor_ready_fn)(struct sh_iface *self);                         /* +0x88 (0x6b40) editor-ready */

typedef struct sh_iface_vtbl {
    /* Original editor-operation cells; untyped entries reserve ABI offsets. */
    sh_set_editor_vec3_fn set_editor_vec3;  /* +0x00 (0x64A0) camera origin */
    sh_get_editor_vec3_fn get_editor_vec3;  /* +0x08 (0x6500) */
    sh_entity_count_fn entity_count;/* +0x10 (0x6550) */
    sh_id_to_string_fn id_to_string;/* +0x18 (0x6580) */
    void *module_index_of;          /* +0x20  (0x6e50) */
    sh_is_valid_id_fn is_valid_id;  /* +0x28 (0x6E60) true when valid */
    sh_get_declsource_fn get_declsource_copy; /* +0x30 (0x65B0) */
    void *get_declsource_ptr;       /* +0x38  (0x6640) */
    sh_rebuild_declsource_fn rebuild_set_declsource; /* +0x40 (0x6850) */
    sh_classname_fn get_classname_copy; /* +0x48 (0x68E0) */
    sh_inherit_fn get_inherit_copy; /* +0x50 (0x6980) */
    sh_get_displayname_fn get_displayname; /* +0x58 (0x7230) */
    void *get_classname_ptr;        /* +0x60  (0x8150) */
    void *get_inherit_ptr;          /* +0x68  (0x81b0) */
    void *get_displayname_ptr;      /* +0x70  (0x8210) */
    sh_set_classname_fn set_classname; /* +0x78 (0x6A20) */
    sh_set_inherit_fn set_inherit;  /* +0x80 (0x6AB0) */
    sh_editor_ready_fn editor_ready_poll; /* +0x88 (0x6B40) window visibility gate */
    void *enqueue_cmd_record;       /* +0x90 (0x66A0) reserved command-record slot */
    void *enqueue_cmd_fmt;          /* +0x98 (0x67B0) reserved formatted-command slot */
    void *engine_call_a;            /* +0xa0  (0x6b60) */
    void *engine_call_b;            /* +0xa8  (0x6b80) */
    sh_serialize_selection_fn serialize_selection; /* +0xB0 (0x6BA0) */
    sh_read_prefab_fn read_prefab;  /* +0xB8 (0x6BF0) pending-prefab readback */
    sh_resolve_prefab_path_fn resolve_prefab_path; /* +0xC0 (0x6BC0) */
    sh_serialize_entity_fn serialize_entity; /* +0xC8 (0x6D50) */
    sh_schedule_apply_fn apply_edit;/* +0xD0 (0x6D70) deferred apply */
    void *catalog_count;            /* +0xd8  (0x6d80) */
    void *catalog_class_u32;        /* +0xe0  (0x6db0) */
    void *catalog_class_name;       /* +0xe8  (0x6dd0) */
    void *catalog_event_name;       /* +0xf0  (0x6df0) */
    void *catalog_event_desc;       /* +0xf8  (0x6e20) */
    void *enum_decl_list;           /* +0x100 (0x6eb0) */
    void *enum_decls_of_restype;    /* +0x108 (0x6ff0) */
    sh_enum_decls_of_resclass_fn enum_decls_of_resclass; /* +0x110 (0x70B0) */
    void *parse_json_file;          /* +0x118 (0x7190) */
    void *spawn_idsnapentity;       /* +0x120 (0x71a0) */
    sh_set_displayname_fn set_entity_0x170; /* +0x128 (0x72A0) display-name setter */
    void *reserved_0x130;           /* +0x130 (0x73C0) no caller */
    sh_add_to_selection_fn add_to_selection; /* +0x138 (0x73F0) */
    void *remove_from_selection;    /* +0x140 (0x7420) */
    sh_clear_selection_fn clear_selection;   /* +0x148 (0x7450) */
    sh_get_selection_fn get_selection;       /* +0x150 (0x7480) */
    void *id_guarded_0x51f890;      /* +0x158 (0x74b0) */
    void *const_0x37c;              /* +0x160 (0x7510) */
    void *classname_by_index;       /* +0x168 (0x7520) */
    void *or_render_flags;          /* +0x170 (0x7530) reserved render-flags slot */
    void *clipboard_write;          /* +0x178 (0x75D0) reserved */
    void *clipboard_read;           /* +0x180 (0x75E0) reserved */

    /* Registry and UI queue operations. */
    /* +0x188 (0x7A00) */
    void (*register_cmd)(sh_iface *self, const char *name, sh_cmd_handler handler, void *ctx);
    /* +0x190 (0x7BA0) */
    void *reserved_190;
    sh_hovered_id_fn hovered_id;    /* +0x198 (0x7D30) */
    /* +0x1A0 (0x7D50) detach under lock, then execute on caller thread */
    void *reserved_drain;
    void *input_state_b;            /* +0x1a8 (0x7e30) */
    void *input_state_a;            /* +0x1b0 (0x7e50) */
    sh_toast_fn toast;              /* +0x1B8 (0x7E70) */
    sh_is_entity_mode_fn is_entity_mode;  /* +0x1C0 (0x7F30) EntityMode query */
    void *is_module_mode;           /* +0x1c8 (0x7f50) */
    void *is_entering_entity_mode;  /* +0x1d0 (0x7f70) */
    void *declmgr_lookup_void;      /* +0x1d8 (0x7f90) */
    void *declmgr_lookup;           /* +0x1e0 (0x7fe0) */
    /* Reserved accessor tail. Extensions begin after +0x260. */
    void *acc_0x1e8;                /* +0x1e8 (0x8030) */
    void *acc_0x1f0;                /* +0x1f0 */
    void *acc_0x1f8;                /* +0x1f8 */
    void *acc_0x200;                /* +0x200 */
    void *acc_0x208;                /* +0x208 */
    void *acc_0x210;                /* +0x210 declMgr lookup (guarded) */
    void *acc_0x218;                /* +0x218 */
    void *acc_0x220;                /* +0x220 */
    void *acc_0x228;                /* +0x228 */
    void *acc_0x230;                /* +0x230 */
    void *acc_0x238;                /* +0x238 */
    void *acc_0x240;                /* +0x240 */
    void *acc_0x248;                /* +0x248 */
    void *acc_0x250;                /* +0x250 declMgr lookup (guarded) */
    void *acc_0x258;                /* +0x258 is-entity-array-readable (IsBadReadPtr) */
    void *acc_0x260;                /* +0x260 double -> idStr (LAST OG slot, idx 76) */

    /* Append-only extensions; both DLLs rebuild together, and the object keeps only a vtable pointer (size 0x60). */
    sh_apply_class_inherit_fn apply_class_inherit;   /* +0x268 (ext 0) final-pair validation and writes */
    sh_enum_valid_classes_fn  enum_valid_classes;    /* +0x270 (ext 1) class-dropdown enumerator */
    sh_enum_inherits_fn       enum_inherits;         /* +0x278 (ext 2) inherit-dropdown enumerator */
    sh_id_dev_layer_hidden_fn id_dev_layer_hidden;   /* +0x280 (ext 3) dev-layer entity-hidden query */
    void *reserved_288;
    sh_apply_sync_fn           apply_sync;           /* +0x290 (ext 5) synchronous apply */
    void *reserved_298;
    sh_push_to_stack_fn        push_to_stack;        /* +0x2A0 (ext 7) stack push */
    sh_clear_stack_fn          clear_stack;          /* +0x2A8 (ext 8) stack clear */
    sh_config_get_json_fn      config_get_json;      /* +0x2B0 (ext 9) registered setting -> JSON */
    sh_config_set_json_fn      config_set_json;      /* +0x2B8 (ext 10) validate + persist JSON */
    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) selection-mutation guard */
    void *reserved_2c8;
    sh_get_preview_fn          get_preview;          /* +0x2D0 (ext 13) consume preview URI */
    sh_request_preview_fn      request_preview;      /* +0x2D8 (ext 14) request preview */
    void *reserved_2e0;
    sh_list_assets_fn          list_assets;          /* +0x2E8 (ext 16) asset catalog */
    sh_material_rect_fn        material_rect;        /* +0x2F0 (ext 17) material atlas rectangle */
    sh_sound_preview_fn        sound_preview;        /* +0x2F8 (ext 18) sound audition */
    sh_sound_session_fn        sound_session;        /* +0x300 (ext 19) sound-preview session */
    void *reserved_308;
    sh_request_prefab_mesh_fn  request_prefab_mesh;  /* +0x310 (ext 21) async installed geometry */
    sh_get_prefab_mesh_fn      get_prefab_mesh;      /* +0x318 (ext 22) consume geometry blob */
    sh_resolve_prefab_defaults_fn resolve_prefab_defaults; /* +0x320 (ext 23) model + scale defaults */
    sh_rawmap_status_fn        rawmap_status;        /* +0x328 (ext 24) rawmap paths + last save */
    sh_rawmap_configure_fn     rawmap_configure;     /* +0x330 (ext 25) choose paths / open / save */
    sh_rawmap_load_now_fn      rawmap_load_now;      /* +0x338 (ext 26) open the load path now */
    sh_navmesh_view_fn         navmesh_view;         /* +0x340 (ext 27) the navigation view */
    sh_run_on_main_fn          run_on_main;          /* +0x348 (ext 28) main-thread call */
    sh_delete_entities_fn      delete_entities;      /* +0x350 (ext 29) native batch delete */
    sh_prefab_grab_status_fn   prefab_grab_status;   /* +0x358 (ext 30) Create grab-frame guard result */
    sh_module_prefab_fn        module_prefab;        /* +0x360 (ext 31) Blueprint module prefabs */
    void *reserved_368;
    sh_budget_fn               budget;               /* +0x370 (ext 33) editor budget table */
    sh_mods_fn                 mods;                 /* +0x378 (ext 34) Mods tab */
    sh_prefab_watch_fn         prefab_watch;         /* +0x380 (ext 35) Prefabs tab editor watch */
    sh_features_off_fn         features_off;         /* +0x388 (ext 36) features off this session */
    sh_preview_failure_fn      preview_failure;      /* +0x390 (ext 37) why an image preview failed */
    sh_sound_refusal_fn        sound_refusal;        /* +0x398 (ext 38) why a sound audition was refused */
    sh_free_text_fn            free_text;            /* +0x3A0 (ext 39) release text a slot handed over */
} sh_iface_vtbl;

SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_get_json) == 0x2B0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_set_json) == 0x2B8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, manipulation_in_progress) == 0x2C0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, reserved_2c8) == 0x2C8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_preview) == 0x2D0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_preview) == 0x2D8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, reserved_2e0) == 0x2E0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_assets) == 0x2E8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, material_rect) == 0x2F0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_preview) == 0x2F8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_session) == 0x300);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, reserved_308) == 0x308);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_prefab_mesh) == 0x310);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_prefab_mesh) == 0x318);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, resolve_prefab_defaults) == 0x320);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_status) == 0x328);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_configure) == 0x330);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_load_now) == 0x338);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, navmesh_view) == 0x340);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, run_on_main) == 0x348);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, delete_entities) == 0x350);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, prefab_grab_status) == 0x358);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, module_prefab) == 0x360);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, reserved_368) == 0x368);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, budget) == 0x370);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, mods) == 0x378);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, prefab_watch) == 0x380);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, features_off) == 0x388);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, preview_failure) == 0x390);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_refusal) == 0x398);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, free_text) == 0x3A0);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x3A8);
SH_STATIC_ASSERT(sizeof(sh_prefab_watch) == 24);
SH_STATIC_ASSERT(sizeof(sh_prefab_grab_info) == 16);
SH_STATIC_ASSERT(sizeof(sh_delete_result) == 40);

/* Fixed subobject header. Private backend registry/locking storage follows
 * it; SnapStack stacks live separately in the backend. */
typedef struct sh_iface_sub {
    uint8_t reserved[0x60];
    void *reserved_queue[3];
} sh_iface_sub;

struct sh_iface {
    const sh_iface_vtbl *vtbl;      /* +0x00 */
    uint8_t              mtx[0x50]; /* +0x08..+0x57: reserved. */
    sh_iface_sub        *sub;       /* +0x58 command registry */
};

SH_STATIC_ASSERT(offsetof(sh_iface, sub) == 0x58);
SH_STATIC_ASSERT(sizeof(sh_iface) == 0x60);

/* The backend records its vtable size in the first reserved cell. */
#define SH_IFACE_VTBL_MARK 0x53485654u  /* "SHVT" */

static __inline uint32_t sh_iface_vtbl_bytes(const struct sh_iface *self)
{
    const uint32_t *cell;
    if (!self) return 0;
    cell = (const uint32_t *)(const void *)self->mtx;
    return cell[0] == SH_IFACE_VTBL_MARK ? cell[1] : 0;
}

#define SH_IFACE_HAS_SLOT(self, member)                                          \
    ((self) && (self)->vtbl &&                                                   \
     offsetof(sh_iface_vtbl, member) + sizeof(void *) <= sh_iface_vtbl_bytes(self) && \
     (self)->vtbl->member != NULL)

/* Frontend thread-init block: output-state slot, argc, argv, and the
 * backend-owned interface. Preserve alignment and field order across DLLs. */
typedef struct sh_ui_argblock {
    void     *out_slot;             /* [0] frontend writes the loop-state obj address here */
    int       argc;                 /* [1] Argument count. */
    char    **argv;                 /* [2] Argument vector. */
    sh_iface *iface;                /* [3] the shared interface object (backend-owned) */
} sh_ui_argblock;

/* Allocate the backend interface and its command registry. */
sh_iface *sh_iface_create(void);

/* The backend's side of owned text: a process-heap copy of s (n bytes, or a formatted string) with its NUL, released
 * by free_text; NULL for NULL or when memory runs out. sh_iface_text_set replaces *slot (freeing the old text) and
 * returns 0 when memory runs out. */
char *sh_iface_text(const char *s);
char *sh_iface_textn(const void *s, size_t n);
char *sh_iface_textf(const char *fmt, ...);
int   sh_iface_text_set(char **slot, const char *s);
void  sh_iface_text_free(void *text);

int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx);

/* Bind engine-independent configuration before starting the frontend. */
typedef struct sh_iface_config_slots {
    sh_config_get_json_fn config_get_json;
    sh_config_set_json_fn config_set_json;
} sh_iface_config_slots;

void sh_iface_bind_config_slots(const sh_iface_config_slots *slots);

/* Engine callback binding record, including reflection/apply operations.
 * Copy into the shared vtable after dependency resolution; null callbacks
 * clear their corresponding slots. Configuration binds separately. */
typedef struct sh_iface_engine_slots {
    sh_set_editor_vec3_fn   set_editor_vec3;     /* +0x00 */
    sh_get_editor_vec3_fn   get_editor_vec3;     /* +0x08 */
    sh_entity_count_fn      entity_count;        /* +0x10 */
    sh_id_to_string_fn      id_to_string;        /* +0x18 */
    sh_is_valid_id_fn       is_valid_id;         /* +0x28 */
    sh_editor_ready_fn      editor_ready_poll;   /* +0x88 (window gate) */
    sh_classname_fn         get_classname_copy;  /* +0x48 */
    sh_inherit_fn           get_inherit_copy;    /* +0x50 */
    sh_add_to_selection_fn  add_to_selection;    /* +0x138 */
    sh_clear_selection_fn   clear_selection;     /* +0x148 */
    sh_get_selection_fn     get_selection;       /* +0x150 */
    sh_hovered_id_fn        hovered_id;          /* +0x198 */
    sh_is_entity_mode_fn    is_entity_mode;      /* +0x1c0 (Create-New-Timeline gate / button gray-out) */
    sh_toast_fn             toast;               /* +0x1b8 */

    sh_serialize_entity_fn  serialize_entity;    /* +0xc8 */
    sh_schedule_apply_fn    apply_edit;          /* +0xd0 */
    sh_read_prefab_fn       read_prefab;         /* +0xb8 */

    sh_get_declsource_fn       get_declsource_copy;   /* +0x30  */
    sh_rebuild_declsource_fn   rebuild_set_declsource;/* +0x40  */
    sh_get_displayname_fn      get_displayname;       /* +0x58  */
    sh_set_classname_fn        set_classname;         /* +0x78  */
    sh_set_inherit_fn          set_inherit;           /* +0x80  */
    sh_set_displayname_fn      set_displayname;       /* +0x128 */
    sh_serialize_selection_fn  serialize_selection;   /* +0xb0  */
    sh_resolve_prefab_path_fn  resolve_prefab_path;   /* +0xc0  */

    sh_enum_decls_of_resclass_fn enum_decls_of_resclass;/* +0x110 */

    sh_apply_class_inherit_fn    apply_class_inherit;   /* +0x268 (ext 0) */

    sh_enum_valid_classes_fn     enum_valid_classes;    /* +0x270 (ext 1) */

    sh_enum_inherits_fn          enum_inherits;         /* +0x278 (ext 2) */

    sh_id_dev_layer_hidden_fn    id_dev_layer_hidden;   /* +0x280 (ext 3) */

    sh_apply_sync_fn             apply_sync;            /* +0x290 (ext 5) */

    sh_push_to_stack_fn          push_to_stack;         /* +0x2A0 (ext 7) */

    sh_clear_stack_fn            clear_stack;           /* +0x2A8 (ext 8) */

    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) */

    sh_get_preview_fn          get_preview;                 /* +0x2D0 (ext 13) */
    sh_request_preview_fn      request_preview;             /* +0x2D8 (ext 14) */
    sh_list_assets_fn          list_assets;                 /* +0x2E8 (ext 16) */
    sh_material_rect_fn        material_rect;               /* +0x2F0 (ext 17) */
    sh_sound_preview_fn        sound_preview;               /* +0x2F8 (ext 18) */
    sh_sound_session_fn        sound_session;               /* +0x300 (ext 19) */
    sh_request_prefab_mesh_fn  request_prefab_mesh;         /* +0x310 (ext 21) */
    sh_get_prefab_mesh_fn      get_prefab_mesh;             /* +0x318 (ext 22) */
    sh_resolve_prefab_defaults_fn resolve_prefab_defaults;  /* +0x320 (ext 23) */
    /* the File menu's rawmap load/save file surface. */
    sh_rawmap_status_fn        rawmap_status;               /* +0x328 (ext 24) */
    sh_rawmap_configure_fn     rawmap_configure;            /* +0x330 (ext 25) */
    sh_rawmap_load_now_fn      rawmap_load_now;             /* +0x338 (ext 26) */
    sh_navmesh_view_fn         navmesh_view;                /* +0x340 (ext 27) */
    sh_run_on_main_fn          run_on_main;                 /* +0x348 (ext 28) */
    sh_delete_entities_fn      delete_entities;             /* +0x350 (ext 29) */
    sh_prefab_grab_status_fn   prefab_grab_status;          /* +0x358 (ext 30) */
    sh_module_prefab_fn        module_prefab;               /* +0x360 (ext 31) */
    sh_budget_fn               budget;                      /* +0x370 (ext 33) */
    sh_mods_fn                 mods;                        /* +0x378 (ext 34) */
    sh_prefab_watch_fn         prefab_watch;                /* +0x380 (ext 35) */
    sh_features_off_fn         features_off;                /* +0x388 (ext 36) */
    sh_preview_failure_fn      preview_failure;             /* +0x390 (ext 37) */
    sh_sound_refusal_fn        sound_refusal;               /* +0x398 (ext 38) */
} sh_iface_engine_slots;

void sh_iface_bind_engine_slots(const sh_iface_engine_slots *slots);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_IFACE_H */
