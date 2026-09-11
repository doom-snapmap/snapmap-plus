/* Shared ABI between the backend and frontend DLLs. The backend owns the
 * interface object and callbacks; both DLLs must build against this header.
 * The 0x60-byte object has a vtable at +0 and subobject pointer at +0x58.
 * The original 77-slot prefix ends at +0x260; append-only extensions end
 * at +0x320, making the current vtable 0x328 bytes.
 * Engine layouts remain private to backend implementations. Reserved cells
 * preserve ABI offsets; optional callbacks can be null. */
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
typedef const char  *(*sh_id_to_string_fn)(struct sh_iface *self, int id, char *buf, int cap); /* +0x18  */
typedef const char  *(*sh_classname_fn)(struct sh_iface *self, int id, char *buf, int cap);    /* +0x48  */
typedef const char  *(*sh_inherit_fn)(struct sh_iface *self, int id, char *buf, int cap);      /* +0x50  */
typedef void         (*sh_toast_fn)(struct sh_iface *self, const char *title, const char *text);/* +0x1b8 */

/* Entity, prefab, and timeline operations. */

/* Copy display name into caller storage and return buf. */
typedef const char  *(*sh_get_displayname_fn)(struct sh_iface *self, int id, char *buf, int cap);  /* +0x58 */

/* Copy canonical entity decl source into caller storage and return buf. */
typedef const char  *(*sh_get_declsource_fn)(struct sh_iface *self, int id, char *buf, int cap);   /* +0x30 */

/* Set nonempty classname when the resulting class/inherit pair is not known invalid. */
typedef void         (*sh_set_classname_fn)(struct sh_iface *self, int id, const char *cstr);      /* +0x78 */
/* Set nonempty inherit when the resulting class/inherit pair is not known invalid. */
typedef void         (*sh_set_inherit_fn)(struct sh_iface *self, int id, const char *cstr);        /* +0x80 */
/* Set the full display-name idStr; empty names are allowed. */
typedef void         (*sh_set_displayname_fn)(struct sh_iface *self, int id, const char *cstr);    /* +0x128 */
/* Rebuild canonical decl source from current class/inherit and the supplied body. */
typedef void         (*sh_rebuild_declsource_fn)(struct sh_iface *self, int id, const char *cstr); /* +0x40 */

/* Serialize selected entities as prefab JSON. Return copied bytes (up to cap - 1), or zero. */
typedef int          (*sh_serialize_selection_fn)(struct sh_iface *self, char *out_json, int cap);  /* +0xb0 */

/* Join the local app-data snapmap-plus root, prefix, and name into out_path.
 * Caller supplies separators and suffix. Return 1 when formatting succeeds. */
typedef int          (*sh_resolve_prefab_path_fn)(struct sh_iface *self, const char *prefix,
                                                  const char *name, char *out_path, int cap);       /* +0xc0 */

/* Remove an ID from selection unless the manipulation guard refuses. */
typedef void         (*sh_remove_from_selection_fn)(struct sh_iface *self, int id);                 /* +0x130 */

/* Enumerate names from the decl manager for a short decl type. Pack NUL-
 * terminated names in out_buf and set out_count; return 1 for a nonempty
 * result. Unknown/unavailable types return zero for editable-text fallback. */
typedef int          (*sh_enum_decls_of_resclass_fn)(struct sh_iface *self, const char *res_class,
                                                    char *out_buf, int cap, int *out_count);       /* +0x110 */

/* Validate the final class/inherit pair once, then assign supplied fields.
 * Null/empty retains a field. Return 1 only when all requested writes succeed,
 * zero on refusal or successful rollback, and -2 when rollback also faults.
 * Rebuild decl source only after result 1. */
typedef int          (*sh_apply_class_inherit_fn)(struct sh_iface *self, int id,
                                                  const char *cls, const char *inh);              /* +0x268 (ext 0) */

/* Pack classes derived from the inherit's base into NUL-separated output.
 * Uses collected live type records, with a static fallback if unavailable;
 * empty/unresolved inherit uses idEntity. Return 1 and count for a nonempty
 * result. Caller capacity can truncate the set. */
typedef int          (*sh_enum_valid_classes_fn)(struct sh_iface *self, const char *inherit,
                                                 char *out_buf, int cap, int *out_count);          /* +0x270 (ext 1) */

/* Pack sorted, deduplicated loaded entityDef paths into NUL-separated output.
 * Return 1 and count for a nonempty result; zero permits frontend fallback.
 * Shared enumeration scratch requires serialized calls. */
typedef int          (*sh_enum_inherits_fn)(struct sh_iface *self,
                                            char *out_buf, int cap, int *out_count);               /* +0x278 (ext 2) */

/* Hide non-base-layer entities only when the dev-layer cvar is confirmed
 * disabled. Unknown cvar or entity state leaves them visible. This list
 * filter omits the native picker's extra mask. */
typedef int          (*sh_id_dev_layer_hidden_fn)(struct sh_iface *self, int id);                /* +0x280 (ext 3) */
typedef int          (*sh_wire_edit_generation_fn)(struct sh_iface *self);                       /* +0x288 (ext 4) */

#define SH_APPLY_IN_PROGRESS (-2)
struct sh_apply_item;
/* Return the completed item count, or SH_APPLY_IN_PROGRESS when the engine
 * started but has not finished by the wait deadline. That result must not be
 * retried or reported as failure. Unknown thread identity, missing transport,
 * and busy slots refuse execution. Marshaling owns a complete text copy. */
typedef int          (*sh_apply_sync_fn)(struct sh_iface *self, const struct sh_apply_item *items,
                                         int count, const char *op_label);                        /* +0x290 (ext 5) */

/* Normalize palette timelines from snapmaps/editor_only/placeholder_target
 * to portable snapmaps/unknown without changing class or numeric formatting.
 * Gate on the source blob, which can lag a raw inherit assignment by one
 * commit, so rescans may retry. Execution requires a verified main thread or
 * successful marshaling, as for apply_sync. */
typedef int          (*sh_normalize_timeline_inherit_fn)(struct sh_iface *self, int id);          /* +0x298 (ext 6) */

/* Push IDs with deduplication onto the backend stack shared with console commands. */
typedef void          (*sh_push_to_stack_fn)(struct sh_iface *self, int index, const int *ids, int count); /* +0x2A0 (ext 7) */

/* Clear a backend stack and return its previous ID count. */
typedef int           (*sh_clear_stack_fn)(struct sh_iface *self, int index);                      /* +0x2A8 (ext 8) */
/* Return 1 when the selection-mutation guard detects manipulation or certain
 * unreadable states. A positional snapshot must keep its selection stable
 * until Escape/commit restores it. Check before changing selection to explain
 * a refusal; add/clear/remove callbacks also apply this guard. */
typedef int           (*sh_manipulation_in_progress_fn)(struct sh_iface *self);                     /* +0x2C0 (ext 11) */


/* +0x2D0 (ext 13) Consume the latest asset preview as a `data:image/png;base64,...` URI. Returns
 * length, 0 if nothing is published, or -(required size) when `cap` is too small. A successful copy
 * releases the backend buffer; an undersized probe leaves it available for the required retry. */
typedef int           (*sh_get_preview_fn)(struct sh_iface *self, char *out, int cap);

/* Preserve the string-only request ABI: this non-path prefix distinguishes
 * direct Image previews from same-named Materials. */
#define SH_PREVIEW_IMAGE_ROUTE_PREFIX "\x1Fimage:"

/* Request asynchronous preview work; poll get_preview for completion.
 * Null/empty cancels staged work and unconsumed output. The route prefix
 * selects an Image; plain names use Material-first lookup. Return acceptance. */
typedef int           (*sh_request_preview_fn)(struct sh_iface *self, const char *name);

typedef int           (*sh_find_material_fn)(struct sh_iface *self, const char *name,
                                             char *out_info, int cap);                              /* +0x2C8: cached material lookup; write a short result and return 1 on a hit. */

/* Page newline-separated material names. Advance start by the returned count;
 * zero ends the listing. */
typedef int           (*sh_list_materials_fn)(struct sh_iface *self, int start, char *out, int cap);

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
/* PROJECTILE: the `projectile` decl type. Reference-only for the same reason PERK is -- a
 * projectile is named by a weapon/ammo decl's `projectileDecl` (and siblings: `subProjectile`,
 * `meleeProjectile`, `detonateProjectile`, `fullyChargedProjectileDecl`, ...), never placed or
 * applied directly, so this is a name you copy and wire by hand. */
#define SH_ASSET_PROJECTILE  17
/* WEAPON: the `weapon` decl type, 204 of them. Reference-only, same shape as PROJECTILE -- named
 * by a `weaponDecl`/`declWeapon` field, not placed or applied. This is the id idTarget_FireWeapon
 * and idTarget_DummyFire actually take: those SnapMap actions fire a WEAPON decl (which in turn
 * names the projectile/ammo it launches), there is no logic-side way to fire a projectile decl
 * directly. */
#define SH_ASSET_WEAPON       18
#define SH_ASSET_COUNT       19

/* Page an asset kind, skipping start names. list_materials remains the
 * kind-0 compatibility slot. */
typedef int           (*sh_list_assets_fn)(struct sh_iface *self, int kind, int start, char *out, int cap);
/* A material's .vmtr atlas rect -> out_xywh = {x,y,w,h} in atlas pixels; 1 if virtual-textured,
 * 0 if it has no rect. Divide by 245760 for the `virtualmapping` renderParm value form. */
typedef int           (*sh_material_rect_fn)(struct sh_iface *self, const char *name, int *out_xywh);
/* Main thread only: audition one sound, replacing any previous preview.
 * Null/empty stops playback; return 1 when playing. */
typedef int           (*sh_sound_preview_fn)(struct sh_iface *self, const char *name);
/* Main thread only: hold audition mode for a browser session. Turning it
 * off stops playback; session scope avoids cvar changes on every click. */
typedef void          (*sh_sound_session_fn)(struct sh_iface *self, int on);

/* Resolve prefab model/defaults through live or installed decl data. Mesh
 * requests are asynchronous and generation-keyed; empty cancels stale work,
 * and get consumes one bounded binary completion. */
typedef int (*sh_resolve_prefab_model_fn)(struct sh_iface *self, const char *inherit_name,
                                          char *out_model, int out_capacity);
typedef int (*sh_request_prefab_mesh_fn)(struct sh_iface *self, unsigned long generation,
                                         const char *model_name);
typedef int (*sh_get_prefab_mesh_fn)(struct sh_iface *self, void *out_blob, int out_capacity);
#ifndef SH_PREFAB_DEFAULT_MODEL
#define SH_PREFAB_DEFAULT_MODEL 0x1
#define SH_PREFAB_DEFAULT_SCALE 0x2
#endif
typedef int (*sh_resolve_prefab_defaults_fn)(struct sh_iface *self, const char *inherit_name,
                                             char *out_model, int out_capacity,
                                             float *out_scale, int out_scale_count);

/* +0x2B0/+0x2B8 (ext 9/10) backend-owned persistent configuration. Values cross the matched-pair
 * boundary as complete UTF-8 JSON fragments so future booleans/numbers/objects do not need new ABI
 * slots. `get` returns the required byte count excluding NUL; a NULL/zero buffer is a size query and an
 * undersized buffer is untouched. The registry in backend/config.c enforces frontend access. */
#define SH_CONFIG_SET_VOLATILE  (-1)
#define SH_CONFIG_SET_REJECTED  0
#define SH_CONFIG_SET_PERSISTED 1

#define SH_CONFIG_STATUS_RECOVERED_CORRUPT  0x01u
#define SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA 0x02u
#define SH_CONFIG_STATUS_VOLATILE           0x04u
#define SH_CONFIG_STATUS_REPAIRED           0x08u

typedef int (*sh_config_get_json_fn)(struct sh_iface *self, const char *key,
                                     char *out_json, int out_capacity,
                                     unsigned int *out_flags);               /* +0x2B0 (ext 9) */
typedef int (*sh_config_set_json_fn)(struct sh_iface *self, const char *key,
                                     const char *value_json);                /* +0x2B8 (ext 10) */

/* Rawmap file slots for the File menu. Status crosses as a JSON fragment, so
 * both paths, the arm state and the save counter travel in one call. */
typedef int (*sh_rawmap_status_fn)(struct sh_iface *self,
                                   char *out_json, int out_capacity);         /* +0x328 (ext 24) */

/* A nonempty load path stages a validated file; an empty one restores the default.
 * A nonempty save path queues an export of the live map to that file; an empty
 * one releases the chosen destination. NULL leaves that side unchanged.
 * arm: -1 leave, 0/1 gate off/on, 2 stage one load, 3 keep the current save path,
 * 4 release it, 5 queue a save to the current backend destination. Refused saves
 * neither change the destination nor arm a future save. */
typedef int (*sh_rawmap_configure_fn)(struct sh_iface *self,
                                      const char *load_path, const char *save_path, int arm,
                                      char *out_msg, int msg_capacity);       /* +0x330 (ext 25) */

/* Reload the editor's map on the next frame, so a staged rawmap opens. Returns 1
 * when the request was accepted, not when the map is open -- the frame hook
 * re-checks and may decline. The reload brackets its own call with the swap arm
 * and requires overwrite protection unless the user explicitly disabled it. */
typedef int (*sh_rawmap_load_now_fn)(struct sh_iface *self,
                                     char *out_msg, int msg_capacity);        /* +0x338 (ext 26) */

/* Reflection and apply callbacks. Engine work prefers the main-thread
 * command drain; apply_sync documents its inline compatibility fallback. */

/* Serialize a full idSnapEntity into NUL-terminated caller storage.
 * Return copied bytes, up to cap - 1, or zero when unavailable. */
typedef int          (*sh_serialize_entity_fn)(struct sh_iface *self, int id, char *out_json, int cap); /* +0xc8 */

/* Shared apply item. Kind 0 commits entity text; 1 stages prefab text;
 * 2 stages and arms native paste; 3 writes a target reference. Scheduling
 * deep-copies text into a single pending batch, replacing older pending work. */
typedef struct sh_apply_item {
    int         kind;       /* 0 entity edit, 1 prefab stage, 2 stage/queue paste, 3 target write. */
    int         id;         /* Live entity ID for kinds 0/3; ignored for prefab staging. */
    const char *text;       /* Entity/prefab JSON, or target ID text for kind 3. */
} sh_apply_item;

/* Deep-copy into the pending batch and enqueue the main-thread drain.
 * Return 1 when command text was submitted; execution completes later.
 * op_label identifies result reporting. An occupied queue refuses new work. */
typedef int          (*sh_schedule_apply_fn)(struct sh_iface *self, const sh_apply_item *items, int count,
                                             const char *op_label);                              /* +0xd0 */

/* Serialize the pending prefab into caller storage. Return copied bytes
 * up to cap - 1, or zero on failure. */
typedef int          (*sh_read_prefab_fn)(struct sh_iface *self, char *out_json, int cap);      /* +0xb8 */

/* Work record executed on the drain caller's thread, normally the UI worker. */
typedef struct sh_work_item {
    sh_cmd_handler  handler;
    void           *ctx;
    int             argc;
    char          **argv;       /* heap-owned copy of the parsed argv; freed after the handler runs */
} sh_work_item;

/* Fixed vtable: 77 legacy cells (+0x00..+0x260) followed by extensions.
 * Append new slots; never move existing cells. Typed callbacks are bound by
 * the backend, while void-pointer cells reserve legacy offsets. Historical
 * function RVAs in parentheses identify the original interface mapping. */
typedef struct sh_iface sh_iface;

/* Set/read the editor camera-origin vec3. */
typedef void (*sh_set_editor_vec3_fn)(struct sh_iface *self, const float *xyz);   /* +0x00 (0x64a0) */
typedef void (*sh_get_editor_vec3_fn)(struct sh_iface *self, float *out_xyz);      /* +0x08 (0x6500) */
typedef int  (*sh_editor_ready_fn)(struct sh_iface *self);                         /* +0x88 (0x6b40) editor-ready */

typedef struct sh_iface_vtbl {
    /* Legacy editor-operation cells; untyped entries reserve ABI offsets. */
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
    sh_remove_from_selection_fn selection_guard; /* +0x130 (0x73C0) selection removal */
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
    void (*unregister_cmd)(sh_iface *self, const char *name);
    sh_hovered_id_fn hovered_id;    /* +0x198 (0x7D30) */
    /* +0x1A0 (0x7D50) detach under lock, then execute on caller thread */
    void (*drain_work_queue)(sh_iface *self);
    void *input_state_b;            /* +0x1a8 (0x7e30) */
    void *input_state_a;            /* +0x1b0 (0x7e50) */
    sh_toast_fn toast;              /* +0x1B8 (0x7E70) */
    sh_is_entity_mode_fn is_entity_mode;  /* +0x1C0 (0x7F30) EntityMode query */
    void *is_module_mode;           /* +0x1c8 (0x7f50) */
    void *is_entering_entity_mode;  /* +0x1d0 (0x7f70) */
    void *declmgr_lookup_void;      /* +0x1d8 (0x7f90) */
    void *declmgr_lookup;           /* +0x1e0 (0x7fe0) */
    /* Reserved legacy accessor tail. Extensions begin after +0x260. */
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

    /* Append-only extensions. Both DLLs rebuild together; the object still
 * contains only a vtable pointer, so its own size remains 0x60. */
    sh_apply_class_inherit_fn apply_class_inherit;   /* +0x268 (ext 0) final-pair validation and writes */
    sh_enum_valid_classes_fn  enum_valid_classes;    /* +0x270 (ext 1) class-dropdown enumerator */
    sh_enum_inherits_fn       enum_inherits;         /* +0x278 (ext 2) inherit-dropdown enumerator */
    sh_id_dev_layer_hidden_fn id_dev_layer_hidden;   /* +0x280 (ext 3) dev-layer entity-hidden query */
    sh_wire_edit_generation_fn wire_edit_generation; /* +0x288 (ext 4) wire-edit generation */
    sh_apply_sync_fn           apply_sync;           /* +0x290 (ext 5) synchronous apply */
    sh_normalize_timeline_inherit_fn normalize_timeline_inherit; /* +0x298 (ext 6) portable timeline inherit */
    sh_push_to_stack_fn        push_to_stack;        /* +0x2A0 (ext 7) stack push */
    sh_clear_stack_fn          clear_stack;          /* +0x2A8 (ext 8) stack clear */
    sh_config_get_json_fn      config_get_json;      /* +0x2B0 (ext 9) registered setting -> JSON */
    sh_config_set_json_fn      config_set_json;      /* +0x2B8 (ext 10) validate + persist JSON */
    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) selection-mutation guard */
    sh_find_material_fn        find_material;        /* +0x2C8 (ext 12) cached material lookup */
    sh_get_preview_fn          get_preview;          /* +0x2D0 (ext 13) consume preview URI */
    sh_request_preview_fn      request_preview;      /* +0x2D8 (ext 14) request preview */
    sh_list_materials_fn       list_materials;       /* +0x2E0 (ext 15) material catalog */
    sh_list_assets_fn          list_assets;          /* +0x2E8 (ext 16) asset catalog */
    sh_material_rect_fn        material_rect;        /* +0x2F0 (ext 17) material atlas rectangle */
    sh_sound_preview_fn        sound_preview;        /* +0x2F8 (ext 18) sound audition */
    sh_sound_session_fn        sound_session;        /* +0x300 (ext 19) sound-preview session */
    sh_resolve_prefab_model_fn resolve_prefab_model; /* +0x308 (ext 20) entityDef inherit -> model */
    sh_request_prefab_mesh_fn  request_prefab_mesh;  /* +0x310 (ext 21) async installed geometry */
    sh_get_prefab_mesh_fn      get_prefab_mesh;      /* +0x318 (ext 22) consume geometry blob */
    sh_resolve_prefab_defaults_fn resolve_prefab_defaults; /* +0x320 (ext 23) model + scale defaults */
    sh_rawmap_status_fn        rawmap_status;        /* +0x328 (ext 24) staged paths + arm state */
    sh_rawmap_configure_fn     rawmap_configure;     /* +0x330 (ext 25) choose those paths / arm */
    sh_rawmap_load_now_fn      rawmap_load_now;      /* +0x338 (ext 26) reload the map now */
} sh_iface_vtbl;

SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_get_json) == 0x2B0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_set_json) == 0x2B8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, manipulation_in_progress) == 0x2C0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, find_material) == 0x2C8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_preview) == 0x2D0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_preview) == 0x2D8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_materials) == 0x2E0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_assets) == 0x2E8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, material_rect) == 0x2F0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_preview) == 0x2F8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_session) == 0x300);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, resolve_prefab_model) == 0x308);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_prefab_mesh) == 0x310);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_prefab_mesh) == 0x318);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, resolve_prefab_defaults) == 0x320);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_status) == 0x328);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_configure) == 0x330);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, rawmap_load_now) == 0x338);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x340);

/* Fixed object layout: vtable +0, reserved bytes +0x08..+0x57, subobject
 * pointer +0x58. Reserved bytes stay zero; private subobject storage holds
 * the live lock. Frontends access state through callbacks. */
#define SH_IFACE_VTBL_OFF   0x00
#define SH_IFACE_MTX_OFF    0x08
#define SH_IFACE_SUB_OFF    0x58    /* Pointer to the fixed 0x78-byte subobject header. */

/* Fixed subobject header. Private backend registry/locking storage follows
 * it; SnapStack stacks live separately in the backend. */
typedef struct sh_iface_sub {
    void          *map_nil;         /* sub+0x00: legacy map field; backend stores its registry pointer here. */
    void          *map_root;        /* sub+0x08 */
    uint64_t       map_size;        /* sub+0x10 */
    uint8_t        _mtx2[0x48];     /* sub+0x18: reserved legacy mutex bytes. */
    /* Queue vector; drain detaches it before executing handlers. */
    sh_work_item  *wq_begin;        /* sub+0x60 */
    sh_work_item  *wq_end;          /* sub+0x68 */
    sh_work_item  *wq_cap;          /* sub+0x70 */

} sh_iface_sub;

struct sh_iface {
    const sh_iface_vtbl *vtbl;      /* +0x00 */
    uint8_t              mtx[0x50]; /* +0x08..+0x57: reserved. */
    sh_iface_sub        *sub;       /* +0x58  -> the cmd-map + work-queue sub-object */
};

SH_STATIC_ASSERT(offsetof(sh_iface, sub) == 0x58);
SH_STATIC_ASSERT(sizeof(sh_iface) == 0x60);

/* Frontend thread-init block: output-state slot, argc, argv, and the
 * backend-owned interface. Preserve alignment and field order across DLLs. */
typedef struct sh_ui_argblock {
    void     *out_slot;             /* [0] frontend writes the loop-state obj address here */
    int       argc;                 /* [1] Argument count. */
    char    **argv;                 /* [2] Argument vector. */
    sh_iface *iface;                /* [3] the shared interface object (backend-owned) */
} sh_ui_argblock;

/* Allocate the backend-owned object and private registry/queue state.
 * Bind registry/drain callbacks immediately; return null on allocation failure. */
sh_iface *sh_iface_create(void);

/* Set the optional callback run before each UI queue drain. Null clears it;
 * registration avoids an engine link dependency in the common module. */
void sh_iface_set_tick_hook(void (*fn)(void));

/* Locked command lookup: fill handler/context and return 1 on a hit, else 0. */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx);

/* Deep-copy argv into the queue for execution by the drain caller, normally
 * the UI worker. Return 1 on success. Console dispatch instead calls inline
 * on the engine main thread; this queue is not a main-thread transport. */
int sh_iface_enqueue_work(sh_iface *self, sh_cmd_handler handler, void *ctx,
                          int argc, const char **argv);

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
    sh_remove_from_selection_fn remove_from_selection;/* +0x130 */

    sh_enum_decls_of_resclass_fn enum_decls_of_resclass;/* +0x110 */

    sh_apply_class_inherit_fn    apply_class_inherit;   /* +0x268 (ext 0) */

    sh_enum_valid_classes_fn     enum_valid_classes;    /* +0x270 (ext 1) */

    sh_enum_inherits_fn          enum_inherits;         /* +0x278 (ext 2) */

    sh_id_dev_layer_hidden_fn    id_dev_layer_hidden;   /* +0x280 (ext 3) */

    sh_wire_edit_generation_fn   wire_edit_generation;  /* +0x288 (ext 4) */

    sh_apply_sync_fn             apply_sync;            /* +0x290 (ext 5) */

    sh_normalize_timeline_inherit_fn normalize_timeline_inherit; /* +0x298 (ext 6) */

    sh_push_to_stack_fn          push_to_stack;         /* +0x2A0 (ext 7) */

    sh_clear_stack_fn            clear_stack;           /* +0x2A8 (ext 8) */

    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) */

    sh_find_material_fn        find_material;               /* +0x2C8 (ext 12) */
    sh_get_preview_fn          get_preview;                 /* +0x2D0 (ext 13) */
    sh_request_preview_fn      request_preview;             /* +0x2D8 (ext 14) */
    sh_list_materials_fn       list_materials;              /* +0x2E0 (ext 15) */
    sh_list_assets_fn          list_assets;                 /* +0x2E8 (ext 16) */
    sh_material_rect_fn        material_rect;               /* +0x2F0 (ext 17) */
    sh_sound_preview_fn        sound_preview;               /* +0x2F8 (ext 18) */
    sh_sound_session_fn        sound_session;               /* +0x300 (ext 19) */
    sh_resolve_prefab_model_fn resolve_prefab_model;        /* +0x308 (ext 20) */
    sh_request_prefab_mesh_fn  request_prefab_mesh;         /* +0x310 (ext 21) */
    sh_get_prefab_mesh_fn      get_prefab_mesh;             /* +0x318 (ext 22) */
    sh_resolve_prefab_defaults_fn resolve_prefab_defaults;  /* +0x320 (ext 23) */
    /* clone-extension: the File menu's rawmap load/save file surface. */
    sh_rawmap_status_fn        rawmap_status;               /* +0x328 (ext 24) */
    sh_rawmap_configure_fn     rawmap_configure;            /* +0x330 (ext 25) */
    sh_rawmap_load_now_fn      rawmap_load_now;             /* +0x338 (ext 26) */
} sh_iface_engine_slots;

void sh_iface_bind_engine_slots(const sh_iface_engine_slots *slots);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_IFACE_H */
