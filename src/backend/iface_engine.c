/* Editor operations behind the shared UI interface. Globals resolve from
 * code references; functions come from the shared signature results. Field
 * offsets remain build-dependent. SEH contains access faults, but a readable
 * incompatible layout can still produce incorrect engine state. */
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapmap_plus_iface.h"
#include "iface_engine.h"
#include "apply_engine.h"
#include "signatures.h"
#include "engine_globals.h"
#include "backend_log.h"
#include "typeinfo.h"
#include "preview.h"
#include "imgpreview.h"
#include "megapreview.h"
#include "soundpreview.h"
#include "prefabpreview.h"
#include "valid_class_map.h"
#include "wiring_cleandirect.h"
#include "snapstack.h"

/* Editor layout. */
/* Extraction RVA for re-derivation only; runtime uses editor_singleton.
 * The editor is an inline object, not a pointer slot. Find its in-place
 * constructor and recover the this address passed to it. */
#define EDITOR_SINGLETON_PINNED_RVA   0x3056748u
#define ED_SEL_OBJ_OFF         0x204d0      /* editor+0x204d0 -> selection object ptr */
#define ED_CAMERA_ORIGIN_OFF   0x170        /* Camera origin. Re-derive from the method at editor vtable +0xD8,
 * which returns the address of this vec3. */
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ED_SCREEN_OFF          0x21088      /* editor+0x21088 -> menu-screen object (Toast arg0) */
#define ED_ENTITY_MODE_OFF     0x23618      /* Editor state: 1 ModuleMode, 2 EntityMode. */
/* EntityMode is inline at editor +0x22330. Native selection changes also
 * update mode +0x1AC and dirty +0xBB8; array-only changes break native
 * deselect/Delete/Move behavior. Re-derive from GetMode(state 2) and the
 * OnDeactivate call identified by its idSnapEditorLocal diagnostic string.
 * Historical newer-retail references: GetMode 0x1183C40, SetSelected
 * 0x1255120, SetIdle 0x1255100. These are not extraction-build call RVAs;
 * no universal address delta exists between those builds. */
#define ED_MODE_OBJ_OFF        0x22330      /* editor+0x22330 -> inline EntityMode object (= GetMode(editor, 2)) */
#define MODE_SEL_STATE_OFF     0x1ac        /* mode+0x1ac -> 1 = idle/nothing selected, 2 = an entity is selected */
#define MODE_DIRTY_OFF         0xBB8        /* mode+0xBB8 -> redraw/dirty flag (set alongside every state write) */
#define MODE_STATE_IDLE        1
#define MODE_STATE_SELECTED    2

#define SEL_IDS_OFF            0x80         /* selObj+0x80 -> int* selected ids */
#define SEL_COUNT_OFF          0x88         /* selObj+0x88 -> int selected count */
#define SEL_HOVERED_OFF        0x2c         /* selObj+0x2c -> looked-at/hovered entity id */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
/* Manipulation capture moves selected entities to the scratch layer and
 * stores original layers in a positional snapshot. Changing selection then
 * makes Escape restore records onto the wrong entities. Re-derive from
 * capture's per-ID layer read and scratch-layer comparison (newer-retail
 * reference 0x11B08D0; do not use as an extraction-build call RVA). */
#define MAP_ENT_LAYER_ARR_OFF  0x6f0        /* mapObj+0x6f0 -> int* per-entity layer/module index */
#define MAP_SCRATCH_LAYER_OFF  0x758        /* mapObj+0x758 -> int, the scratch/working layer id */
/* Loaded-map tables used to format module-qualified entity references. */
#define LM_ENTPOS_ARR_OFF      0x708        /* loaded-map+0x708 -> entity-id-by-placement-position array (u32) */
#define LM_ENTPOS_CNT_OFF      0x710        /* loaded-map+0x710 -> placement-position count (u32) */
#define LM_MODBOUND_ARR_OFF    0x720        /* loaded-map+0x720 -> per-module cumulative-position boundaries (s32, sorted) */
#define LM_MODBOUND_CNT_OFF    0x728        /* loaded-map+0x728 -> module-boundary count (s32) */
#define LM_MODTABLE_OFF        0x750        /* loaded-map+0x750 -> module table (stride 0x98) */
#define MOD_STRIDE             0x98         /* module-table entry stride */
#define MOD_NAME_OFF           0x48         /* module entry+0x48 -> module name char* */
#define LM_ENTINST_ARR_OFF     0x6f0        /* Per-entity instance index. The registrar writes it on create and
 * map loading rebuilds it from instanceEntities. Index == module count
 * means global/no-module; spatial position does not establish membership. */
#define LM_INSTANCES_CNT_OFF   0x758        /* loaded-map+0x758 -> instances(modules) COUNT (== the no-module sentinel) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid (the +0x28 rule) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object */

/* Lists hide non-base-layer entities unless the dev-layer cvar is enabled.
 * Native picking also intersects a dev-layer mask. Re-derive by following
 * snapEdit_enableDevLayer registration and the entity +0x160 bit test. */
#define ENT_LAYER_BITS_OFF     0x160        /* entity[id]+0x160 -> layer bitmask (uint) */
#define DEVL_CVAR_VALUE_OFF    0x30         /* idCVar+0x30 -> current integer value (== cvars.c value note) */
#define DEVL_CVAR_NAME_OFF     0x40         /* idCVar+0x40 -> registered name char* (== cvars.c IDCVAR_NAME_OFF) */
/* Audit RVA only; runtime resolves cvar_system_slot from a signed code reference. */
#define DEVL_CVARSYS_SLOT_PINNED_RVA  0x55b7290u
#define DEVL_CVARSYS_ARR_OFF   0x08         /* cvarSys+0x08 -> FULL idCVar** array */
#define DEVL_CVARSYS_CNT_OFF   0x10         /* cvarSys+0x10 -> FULL count (u32) */
#define DEVL_CVAR_LIST_CAP     100000u      /* stale-cvarSys guard */
#define DEVL_CVAR_NAME         "snapEdit_enableDevLayer"
#define ENT_DECL_OFF           0x8          /* *(ent+8) -> the entity's decl object (classname blob root) */
#define DECL_BLOB_A_OFF        0x1c8        /* Decl object to resolved source object. */
#define DECL_BLOB_B_OFF        0x38         /* Resolved source text pointer. */
#define IDSTR_SIZE             0x30         /* sizeof(idStr) for the toast title/text temporaries */

/* Entity-state fields. */
#define ENT_DISPLAYNAME_LEN_OFF 0x178      /* Display name length. */
#define ENT_DISPLAYNAME_PTR_OFF 0x180      /* entity[id]+0x180 -> displayname data ptr */
#define ENT_DISPLAYNAME_FIELD   0x170      /* entity[id]+0x170 -> displayname idStr field (SET target) */
#define DEFSUB_SRC_PTR_OFF      0x140      /* defsub+0x140 -> canonical decl-source text data ptr (vt+0x30 get) */
#define DEFSUB_SRC_LEN_OFF      0x138      /* defsub+0x138 -> canonical decl-source text len (s32) */
#define DEFSUB_CLASS_OFF        0x60       /* Interned classname pointer slot. */
#define DEFSUB_INHERIT_OFF      0x58       /* Interned inherit pointer slot. */
#define ED_SEL_OBJ_OFF_C3       0x204d0    /* editor+0x204d0 -> selection object (Delete guard) */

/* Audit RVA only. Resolve RemoveFromSelection by its signed wrapper,
 * including add rcx,0x5E0 before the selection-subobject call. */
#define REMOVE_FROM_SEL_PINNED_RVA     0x59fda0u

/* Display name is a full idStr: assign through IdStrAssignCStr to maintain
 * length, data, and SSO/heap ownership. Class/inherit are pooled pointer
 * slots and use IdStrAssign instead. These helpers are not interchangeable.
 * Audit RVA only; runtime uses the shared signature result. */
#define IDSTR_OPASSIGN_PINNED_RVA      0x19fd5f0u

/* Asset registry node layout; decl dropdowns use the separate typeinfo walk. */
#define DECLNODE_ARRAY_OFF      0x20        /* decl-manager node -> decl-pointer array */
#define DECLNODE_COUNT_OFF      0x28        /* decl-manager node -> decl count (u32) */
#define DECL_NAME_OFF           0x08        /* decl object -> name char* (*decl + 8) */
#define DECLNODE_COUNT_CAP      (1u << 20)  /* stale-node guard (same cap sh_listres uses) */

#define SEL_MAX_IDS            65536        /* sanity cap on a selection/array count (stale-obj guard) */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

/* Engine call signatures. */
typedef void  (*add_to_sel_fn)(void *selObj, int id);
typedef void  (*clear_sel_fn)(void *selObj);
typedef void  (*toast_fn)(void *screen, void *titleIdStr, void *textIdStr);
typedef void *(*idstr_ctor_fn)(void *self, const char *cstr);
typedef void  (*idstr_dtor_fn)(void *self);

typedef void  (*idstr_assign_fn)(void *dstField, const char *cstr);          /* Interned pointer assignment for class/inherit. */
typedef void  (*idstr_opassign_fn)(void *idStrField, const char *cstr);      /* Full idStr assignment for display name. */
typedef void  (*decl_src_rebuild_fn)(void *defsub, const char *src, int one);
typedef void  (*remove_from_sel_fn)(void *selObj, int id);
typedef void *(*get_decls_fn)(const char *type_name);

/* Resolved engine state. */
static const uint8_t *g_editor   = NULL;   /* glb_resolve("editor_singleton"); NULL = unresolved */
static add_to_sel_fn  g_add_sel  = NULL;
static clear_sel_fn   g_clear_sel= NULL;
static toast_fn       g_toast    = NULL;
static idstr_ctor_fn  g_idstr_ctor = NULL;
static idstr_dtor_fn  g_idstr_dtor = NULL;

static idstr_assign_fn    g_idstr_assign = NULL;   /* +0x78/+0x80 set className/inherit (idPoolStr fields) */
static idstr_opassign_fn  g_idstr_opassign = NULL; /* +0x128 set displayName (FULL idStr field entity+0x170) */
static decl_src_rebuild_fn g_decl_rebuild = NULL;  /* +0x40 Save-to-Decl rebuild */
static remove_from_sel_fn g_remove_sel    = NULL;  /* +0x130 Delete */
static get_decls_fn       g_get_decls     = NULL;  /* +0x110 enum-decls-of-resclass (GetDeclsOfType) */
static const uint8_t     *g_cvarsys_slot  = NULL;  /* glb_resolve("cvar_system_slot"); NULL = unresolved */
static void              *g_devlayer_cvar = NULL;  /* cached snapEdit_enableDevLayer idCVar* (lazy; dev-layer gate) */
static volatile LONG  g_installed = 0;

const uint8_t *sh_iface_engine_editor_base(void)
{
    return g_editor;
}

/* Guarded primitive reads. */
static int ie_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ie_read_s32(const void *src, int *out)
{
    __try { *out = *(const int *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ie_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Return the editor when its loaded-map pointer is readable and nonnull.
 * This pointer can persist in the HUB; it is not a visibility/readiness gate. */
static const uint8_t *editor_session(void)
{
    if (!g_editor) return NULL;
    void *mapObj = NULL;
    if (!ie_read_ptr(g_editor + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return g_editor;
}

/* Window visibility follows the editor screen pointer. Map and selection
 * pointers can persist in the HUB and produce false positives. Re-derive
 * by comparing the editor session fields in the editor and after HUB exit. */
static int slot_editor_ready(sh_iface *self)
{
    (void)self;
    if (!g_editor) return 0;
    void *screen = NULL;
    if (!ie_read_ptr(g_editor + ED_SCREEN_OFF, &screen)) return 0;
    return screen != NULL ? 1 : 0;
}

/* EntityMode is state 2. The loaded-map guard alone does not prove the
 * editor window is active; visibility uses slot_editor_ready separately. */
static int slot_is_entity_mode(sh_iface *self)
{
    (void)self;
    const uint8_t *ed = editor_session();
    if (!ed) return 0;
    int m = 0;
    if (!ie_read_s32(ed + ED_ENTITY_MODE_OFF, &m)) return 0;
    return m == 2 ? 1 : 0;
}


static void *selection_object(void)
{
    const uint8_t *ed = editor_session();
    if (!ed) return NULL;
    void *sel = NULL;
    if (!ie_read_ptr(ed + ED_SEL_OBJ_OFF, &sel)) return NULL;
    return sel;
}

/* Write the camera origin through its inline vec3. */
static void slot_set_editor_vec3(sh_iface *self, const float *xyz)
{
    (void)self;
    const uint8_t *ed = editor_session();
    if (!ed || !xyz) return;
    float *dst = (float *)((uintptr_t)ed + ED_CAMERA_ORIGIN_OFF);
    __try { dst[0] = xyz[0]; dst[1] = xyz[1]; dst[2] = xyz[2]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Read camera origin; clear output when the read is unavailable. */
static void slot_get_editor_vec3(sh_iface *self, float *out_xyz)
{
    (void)self;
    if (!out_xyz) return;
    out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.0f;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    const float *src = (const float *)(ed + ED_CAMERA_ORIGIN_OFF);
    __try { out_xyz[0] = src[0]; out_xyz[1] = src[1]; out_xyz[2] = src[2]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* the entity-ptr array {array, count} off the loaded-map object; 0 on no map / fault. */
static int entity_array(void **out_array, uint32_t *out_count)
{
    const uint8_t *ed = editor_session();
    if (!ed) return 0;
    void *arrObj = NULL;
    if (!ie_read_ptr(ed + ED_MAP_OBJ_OFF, &arrObj) || arrObj == NULL) return 0;
    void    *array = NULL;
    uint32_t count = 0;
    if (!ie_read_ptr((const uint8_t *)arrObj + ARR_ENT_ARRAY_OFF, &array)) return 0;
    if (!ie_read_u32((const uint8_t *)arrObj + ARR_ENT_COUNT_OFF, &count)) return 0;
    if (array == NULL || count > ENT_COUNT_CAP) return 0;
    *out_array = array;
    *out_count = count;
    return 1;
}

/* one entity ptr by id (the 8-byte array slot); NULL if out of range / fault. */
static void *entity_ptr(void *array, uint32_t count, int id)
{
    if (id < 0 || (uint32_t)id >= count) return NULL;
    void *e = NULL;
    if (!ie_read_ptr((const uint8_t *)array + (size_t)id * 8, &e)) return NULL;
    return e;
}

/* Copy a bounded decl prefix, then find key followed by an assigned quoted
 * value. This is a textual field lookup, not a decl parser. */
static const char *parse_decl_field(const void *blob_ptr_addr, const char *key, char *buf, int cap)
{
    if (cap > 0) buf[0] = '\0';
    void *blob = NULL;
    if (!ie_read_ptr(blob_ptr_addr, &blob) || blob == NULL) return buf;
    char text[1024];
    int got = 0;
    __try {
        const char *p = (const char *)blob;
        int i = 0;
        for (; i < (int)sizeof(text) - 1 && p[i]; i++) text[i] = p[i];
        text[i] = '\0';
        got = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { got = 0; }
    if (!got) return buf;


    const char *k = strstr(text, key);
    if (!k) return buf;
    const char *eq = strchr(k, '=');
    if (!eq) return buf;
    const char *q1 = strchr(eq, '"');
    if (!q1) return buf;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return buf;
    int len = (int)(q2 - (q1 + 1));
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    memcpy(buf, q1 + 1, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* Shared interface slots. */

/* Copy at most max selected IDs and return the count written. */
static int slot_get_selection(sh_iface *self, int *out_ids, int max)
{
    (void)self;
    void *sel = selection_object();
    if (!sel || !out_ids || max <= 0) return 0;
    int count = 0;
    if (!ie_read_s32((const uint8_t *)sel + SEL_COUNT_OFF, &count)) return 0;
    if (count <= 0 || count > SEL_MAX_IDS) return 0;
    void *arr = NULL;
    if (!ie_read_ptr((const uint8_t *)sel + SEL_IDS_OFF, &arr) || arr == NULL) return 0;
    int n = count < max ? count : max;
    int written = 0;
    for (int i = 0; i < n; i++) {

        uint32_t id = 0;
        if (!ie_read_u32((const uint8_t *)arr + (size_t)i * 4, &id)) break;
        out_ids[written++] = (int)id;
    }
    return written;
}


/* Detect a positional manipulation snapshot through selected scratch-layer
 * entities. Selection mutation would corrupt Escape restoration. Most read
 * errors refuse mutation; an absent or unreadable selection pointer returns
 * no active snapshot. */
static int manipulation_in_progress(void)
{
    const uint8_t *ed = editor_session();
    if (!ed) return 1;                       /* Unknown editor: refuse mutation. */
    void *mapObj = NULL, *sel = NULL;
    if (!ie_read_ptr(ed + ED_MAP_OBJ_OFF, &mapObj) || !mapObj) return 1;
    if (!ie_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) || !sel) return 0;   /* Absent or unreadable selection: no snapshot reported. */

    int count = 0;
    if (!ie_read_s32((const uint8_t *)sel + SEL_COUNT_OFF, &count)) return 1;
    if (count <= 0) return 0;
    if (count > SEL_MAX_IDS) return 1;

    void *ids = NULL, *layers = NULL;
    if (!ie_read_ptr((const uint8_t *)sel + SEL_IDS_OFF, &ids) || !ids) return 1;
    if (!ie_read_ptr((const uint8_t *)mapObj + MAP_ENT_LAYER_ARR_OFF, &layers) || !layers) return 1;
    int scratch = 0;
    if (!ie_read_s32((const uint8_t *)mapObj + MAP_SCRATCH_LAYER_OFF, &scratch)) return 1;

    for (int i = 0; i < count; i++) {
        uint32_t id = 0;
        if (!ie_read_u32((const uint8_t *)ids + (size_t)i * 4, &id)) return 1;
        int layer = 0;
        if (!ie_read_s32((const uint8_t *)layers + (size_t)id * 4, &layer)) return 1;
        if (layer == scratch) return 1;
    }
    return 0;
}

/* Expose the selection-mutation guard so the UI can explain refusals. */
static int slot_manipulation_in_progress(sh_iface *self)
{
    (void)self;
    return manipulation_in_progress() ? 1 : 0;
}

/* Cached material lookup only; do not invoke the fatal-on-miss loader. */
static int slot_find_material(sh_iface *self, const char *name, char *out_info, int cap)
{
    (void)self;
    return sh_typeinfo_find_material(name, out_info, (size_t)cap);
}

/* Consume published preview bytes; negative required size preserves them for retry. */
static int slot_get_preview(sh_iface *self, char *out, int cap)
{
    (void)self;
    return sh_preview_get(out, (size_t)(cap > 0 ? cap : 0));
}

/* Stage asynchronous preview work; null/empty cancels and releases pending output. */
static int slot_request_preview(sh_iface *self, const char *name)
{
    (void)self;
    if (!name || !*name) { sh_preview_cancel(); return 1; }
    const char *asset = name;
    int kind = SH_PREVIEW_KIND_AUTO;
    const size_t image_prefix_len = sizeof(SH_PREVIEW_IMAGE_ROUTE_PREFIX) - 1;
    if (strncmp(asset, SH_PREVIEW_IMAGE_ROUTE_PREFIX, image_prefix_len) == 0) {
        asset += image_prefix_len;
        kind = SH_ASSET_IMAGE;
    }
    if (!*asset) { sh_preview_cancel(); return 1; }
    sh_preview_request_kind(asset, kind);
    sh_megapreview_wake();
    return 1;
}

/* Page the installed material index without touching engine state. */
static int slot_list_materials(sh_iface *self, int start, char *out, int cap)
{
    (void)self;
    if (!out || cap <= 1) return 0;
    return sh_imgpreview_list(SH_ASSET_MATERIAL, (unsigned)(start > 0 ? start : 0), out, (size_t)cap);
}

/* Page the selected asset catalog; invalid kinds return an empty result. */
static int slot_list_assets(sh_iface *self, int kind, int start, char *out, int cap)
{
    (void)self;
    if (!out || cap <= 1) return 0;
    return sh_imgpreview_list(kind, (unsigned)(start > 0 ? start : 0), out, (size_t)cap);
}

/* Read the material's atlas rectangle from installed data. */
static int slot_material_rect(sh_iface *self, const char *name, int *out_xywh)
{
    (void)self;
    return sh_megapreview_rect(name, out_xywh);
}

/* Main thread only: audition an indexed sound, or stop on null/empty name. */
static int slot_sound_preview(sh_iface *self, const char *name)
{
    (void)self;
    if (!name || !name[0]) { sh_soundpreview_stop(); return 0; }
    return sh_soundpreview_play(name);
}

/* Main thread only: hold audition cvars for the browser session to avoid
 * audio suspend/resume on every preview click. */
static void slot_sound_session(sh_iface *self, int on)
{
    (void)self;
    sh_soundpreview_set_session(on);
}

/* Resolve prefab models from decl/installed data; mesh extraction is asynchronous. */
static int slot_resolve_prefab_model(sh_iface *self, const char *inherit_name,
                                     char *out_model, int out_capacity)
{
    (void)self;
    if (out_capacity <= 0) return 0;
    /* Spawners can inherit an editor helper model. Prefer the installed
 * spawnerEntityPair.entityStatic route to display the actual pickup. */
    if (inherit_name && strstr(inherit_name, "spawner") &&
        sh_prefabpreview_resolve_model(inherit_name, out_model, (size_t)out_capacity)) return 1;
    if (sh_typeinfo_inherit_model(inherit_name, out_model, (size_t)out_capacity)) return 1;
    /* Follow the installed spawner-to-pickup model chain if live lookup missed. */
    return sh_prefabpreview_resolve_model(inherit_name, out_model, (size_t)out_capacity);
}

static int slot_request_prefab_mesh(sh_iface *self, unsigned long generation, const char *model_name)
{
    (void)self;
    return sh_prefabpreview_request(generation, model_name);
}

static int slot_get_prefab_mesh(sh_iface *self, void *out_blob, int out_capacity)
{
    (void)self;
    return sh_prefabpreview_get(out_blob, out_capacity);
}

/* Sparse prefab state omits inherited scale; resolve defaults for preview dimensions. */
static int slot_resolve_prefab_defaults(sh_iface *self, const char *inherit_name,
                                        char *out_model, int out_capacity,
                                        float *out_scale, int out_scale_count)
{
    if (!out_model || out_capacity <= 0 || !out_scale || out_scale_count < 3) return 0;
    int flags = sh_prefabpreview_resolve_defaults(inherit_name, out_model,
                                                  (size_t)out_capacity, out_scale);
    /* Keep live-model preference while the installed resolver supplies scale. */
    if (slot_resolve_prefab_model(self, inherit_name, out_model, out_capacity))
        flags |= SH_PREFAB_DEFAULT_MODEL;
    else {
        out_model[0] = '\0'; flags &= ~SH_PREFAB_DEFAULT_MODEL;
    }
    return flags;
}

static void mode_set_selection_state(int state)
{
    const uint8_t *ed = editor_session();
    if (!ed) return;
    int editor_state = 0;
    if (!ie_read_s32(ed + ED_ENTITY_MODE_OFF, &editor_state)) return;
    if (editor_state != 2) return;

    /* Only transition between idle and selected. Other substates own gesture
 * or screen bookkeeping that a direct state write would bypass. */
    int cur = 0;
    if (!ie_read_s32(ed + ED_MODE_OBJ_OFF + MODE_SEL_STATE_OFF, &cur)) return;
    if (cur != MODE_STATE_IDLE && cur != MODE_STATE_SELECTED) return;
    if (cur == state) return;

    __try {
        *(int *)((uintptr_t)ed + ED_MODE_OBJ_OFF + MODE_SEL_STATE_OFF) = state;
        *(uint8_t *)((uintptr_t)ed + ED_MODE_OBJ_OFF + MODE_DIRTY_OFF) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Clear selection and synchronize EntityMode to idle when its state permits. */
static void slot_clear_selection(sh_iface *self)
{
    (void)self;
    /* Selection must remain stable while a positional manipulation snapshot exists. */
    if (manipulation_in_progress()) return;
    void *sel = selection_object();
    if (!sel || !g_clear_sel) return;
    __try { g_clear_sel(sel); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    mode_set_selection_state(MODE_STATE_IDLE);
}

/* Add an ID and synchronize EntityMode so native deselect/Delete/Move see it. */
static void slot_add_to_selection(sh_iface *self, int id)
{
    (void)self;
    if (manipulation_in_progress()) return;
    void *sel = selection_object();
    if (!sel || !g_add_sel) return;
    __try { g_add_sel(sel, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    mode_set_selection_state(MODE_STATE_SELECTED);
}

/* Return hovered ID, or -1 when unavailable. */
static int slot_hovered_id(sh_iface *self)
{
    (void)self;
    void *sel = selection_object();
    if (!sel) return -1;
    int hovered = -1;
    if (!ie_read_s32((const uint8_t *)sel + SEL_HOVERED_OFF, &hovered)) return -1;
    return hovered;
}

/* A valid entity has a nonnull decl pointer at +8. */
static int slot_is_valid_id(sh_iface *self, int id)
{
    (void)self;
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return 0;
    void *vflag = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_VALID_OFF, &vflag)) return 0;
    return vflag != NULL;
}


static int slot_entity_count(sh_iface *self)
{
    (void)self;
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    return (int)count;
}

/* Read authoritative instance membership. Index == module count is global;
 * return -1 for that sentinel or an unavailable/out-of-range index. */
static int id_module_index(const uint8_t *lm, uint32_t id)
{
    void *idxArr = NULL; int instIdx = 0, modCnt = 0;
    if (!ie_read_ptr(lm + LM_ENTINST_ARR_OFF, &idxArr) || idxArr == NULL)  return -1;
    if (!ie_read_s32((const uint8_t *)idxArr + (size_t)id * 4, &instIdx))  return -1;
    if (!ie_read_s32(lm + LM_INSTANCES_CNT_OFF, &modCnt) || modCnt <= 0)   return -1;
    return (instIdx >= 0 && instIdx < modCnt) ? instIdx : -1;   /* instIdx == modCnt => global/no-module */
}

/* Format module_index_module_name/inherit_id. The table entry contains a
 * module-object pointer whose +0x48 is the name pointer. Without a module,
 * show inherit/class and ID with an explicit no-module marker. */
static const char *slot_id_to_string(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (!buf || cap <= 0) return "";
    buf[0] = '\0';
    char clsbuf[128] = {0};
    char inhbuf[160] = {0};
    __try {

        void *array = NULL; uint32_t count = 0;
        if (entity_array(&array, &count)) {
            void *ent = entity_ptr(array, count, id), *defsub = NULL, *cp = NULL, *ip = NULL;
            if (ent && ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) && defsub) {
                if (ie_read_ptr((const uint8_t *)defsub + DEFSUB_CLASS_OFF, &cp) && cp) {
                    const char *c = (const char *)cp;
                    int k = 0; for (; k < (int)sizeof(clsbuf) - 1 && c[k]; k++) clsbuf[k] = c[k];
                }
                if (ie_read_ptr((const uint8_t *)defsub + DEFSUB_INHERIT_OFF, &ip) && ip) {
                    const char *c = (const char *)ip;
                    int k = 0; for (; k < (int)sizeof(inhbuf) - 1 && c[k]; k++) inhbuf[k] = c[k];
                }
            }
        }

        const uint8_t *ed = editor_session();
        void *lm = NULL;
        if (ed && ie_read_ptr(ed + ED_MAP_OBJ_OFF, &lm) && lm) {
            int modIdx = id_module_index((const uint8_t *)lm, (uint32_t)id);
            if (modIdx >= 0) {
                /* Dereference the table entry, then its module-name pointer. */
                const char *modName = NULL;
                void *modTable = NULL, *modObj = NULL, *np = NULL;
                if (ie_read_ptr((const uint8_t *)lm + LM_MODTABLE_OFF, &modTable) && modTable &&
                    ie_read_ptr((const uint8_t *)modTable + (size_t)modIdx * MOD_STRIDE, &modObj) && modObj &&
                    ie_read_ptr((const uint8_t *)modObj + MOD_NAME_OFF, &np))
                    modName = (const char *)np;
                if (modName && modName[0]) {
                    _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%d_%s/%s_%d",
                                modIdx, modName, inhbuf[0] ? inhbuf : "NULL", id);
                    if (buf[0]) return buf;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    /* Global or unresolved membership is explicit and is not a usable target reference. */
    if (inhbuf[0])      _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s_%d (no module)", inhbuf, id);
    else if (clsbuf[0]) _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s_%d (no module)", clsbuf, id);
    else                _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%d (no module)", id);
    return buf;
}

/* Target-reference wrapper; callers reject the no-module form. */
const char *ie_resolve_id_string(int id, char *buf, int cap)
{
    return slot_id_to_string(NULL, id, buf, cap);
}

/* Read the pooled classname directly so morphs appear before resolved
 * source blobs refresh. */
static const char *slot_get_classname(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return buf;
    __try {
        const char *cn = *(const char * const *)((const uint8_t *)defsub + DEFSUB_CLASS_OFF);
        if (cn) lstrcpynA(buf, cn, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (cap > 0) buf[0] = '\0'; }
    return buf;
}

/* Read inherit from the defsub source blob, which can lag a raw-field change. */
static const char *slot_get_inherit(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return buf;
    return parse_decl_field((const uint8_t *)defsub + DECL_BLOB_B_OFF, "inherit", buf, cap);
}

/* Construct title/text idStr objects, show the toast, then destroy both. */
static void slot_toast(sh_iface *self, const char *title, const char *text)
{
    (void)self;
    /* Log before editor guards so unavailable/short-lived toasts remain observable. */
    {
        char _tl[256];
        _snprintf_s(_tl, sizeof _tl, _TRUNCATE, "C2 toast: \"%s\" / \"%s\"", title ? title : "", text ? text : "");
        backend_log(_tl);
    }
    if (!g_toast || !g_idstr_ctor || !g_idstr_dtor) return;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    void *screen = NULL;
    if (!ie_read_ptr(ed + ED_SCREEN_OFF, &screen) || screen == NULL) return;

    uint8_t tStr[IDSTR_SIZE], xStr[IDSTR_SIZE];
    memset(tStr, 0, sizeof tStr);
    memset(xStr, 0, sizeof xStr);
    __try {
        g_idstr_ctor(tStr, title ? title : "");
        g_idstr_ctor(xStr, text  ? text  : "");
        g_toast(screen, tStr, xStr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    /* Destructors release heap storage and preserve inline buffers. */
    __try { g_idstr_dtor(xStr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { g_idstr_dtor(tStr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Entity-state editing. */


static void *defsub_for_id(int id)
{
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return NULL;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return NULL;
    void *defsub = NULL;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return NULL;
    return defsub;
}

/* Copy display name using the full idStr length/data fields. */
static const char *slot_get_displayname(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return buf;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return buf;
    uint32_t len = 0;
    void *data = NULL;
    if (!ie_read_u32((const uint8_t *)ent + ENT_DISPLAYNAME_LEN_OFF, &len)) return buf;
    if (!ie_read_ptr((const uint8_t *)ent + ENT_DISPLAYNAME_PTR_OFF, &data) || data == NULL) return buf;
    if (len == 0 || len > (uint32_t)(cap - 1)) len = (len > (uint32_t)(cap - 1)) ? (uint32_t)(cap - 1) : len;
    __try {
        uint32_t i = 0;
        const char *p = (const char *)data;
        for (; i < len; i++) buf[i] = p[i];
        buf[i] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    return buf;
}

/* Copy canonical source text used by Save to Decl. */
static const char *slot_get_declsource(sh_iface *self, int id, char *buf, int cap)
{
    (void)self;
    if (cap > 0) buf[0] = '\0';
    void *defsub = defsub_for_id(id);
    if (!defsub) return buf;
    int len = 0;
    void *data = NULL;
    if (!ie_read_s32((const uint8_t *)defsub + DEFSUB_SRC_LEN_OFF, &len)) return buf;
    if (!ie_read_ptr((const uint8_t *)defsub + DEFSUB_SRC_PTR_OFF, &data) || data == NULL) return buf;
    if (len <= 0) return buf;
    if (len > cap - 1) len = cap - 1;
    __try {
        const char *p = (const char *)data;
        int i = 0;
        for (; i < len; i++) buf[i] = p[i];
        buf[i] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
    return buf;
}


/* Reject known class/inherit derivation mismatches before engine parsing.
 * Null/empty arguments retain live values. Missing types/decls fail open;
 * this checks the known fatal mismatch, not all declaration validity. */
int sh_iface_class_inherit_ok(int id, const char *newClass, const char *newInherit)
{
    void *defsub = defsub_for_id(id);
    if (!defsub) return 1;
    /* Read live pooled fields for whichever values the caller did not supply. */
    char clsbuf[256], inhbuf[256]; clsbuf[0] = '\0'; inhbuf[0] = '\0';
    const char *cls = (newClass   && newClass[0])   ? newClass   : NULL;
    const char *inh = (newInherit && newInherit[0]) ? newInherit : NULL;
    if (!cls || !inh) {
        __try {
            if (!cls) { const char *p = *(const char * const *)((const uint8_t *)defsub + DEFSUB_CLASS_OFF);
                        if (p) { lstrcpynA(clsbuf, p, (int)sizeof clsbuf); cls = clsbuf[0] ? clsbuf : NULL; } }
            if (!inh) { const char *p = *(const char * const *)((const uint8_t *)defsub + DEFSUB_INHERIT_OFF);
                        if (p) { lstrcpynA(inhbuf, p, (int)sizeof inhbuf); inh = inhbuf[0] ? inhbuf : NULL; } }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!cls || !inh) return 1;                       /* can't determine the resulting pair -> fail-open */
    char ybuf[256];
    const char *Y = sh_typeinfo_inherit_base(inh, ybuf, sizeof ybuf);
    if (!Y || !Y[0]) return 1;
    if (strcmp(cls, Y) == 0) return 1;
    if (sh_typeinfo_class_derives(cls, Y) == 0) {     /* Known incompatible pair. */
        char msg[360];
        _snprintf_s(msg, sizeof msg, _TRUNCATE,
            "B2 iface: class/inherit change REJECTED -- class '%s' does not derive from inherit '%s's base "
            "type '%s' (would fatally fault the engine decl reparse); apply skipped.", cls, inh, Y);
        backend_log(msg);
        return 0;
    }
    return 1;
}

static void slot_set_classname(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_assign || !cstr || !cstr[0]) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    if (!sh_iface_class_inherit_ok(id, cstr, NULL)) return;
    __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}


static void slot_set_inherit(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_assign || !cstr || !cstr[0]) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    if (!sh_iface_class_inherit_ok(id, NULL, cstr)) return;
    __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Validate the final class/inherit pair once, avoiding invalid intermediate
 * checks during cross-family morphs. Null/empty leaves a field unchanged.
 * Writes have separate fault guards: 1 means at least one write succeeded,
 * not an atomic transaction. The caller rebuilds once after success. */
static int slot_apply_class_inherit(sh_iface *self, int id, const char *cls, const char *inh)
{
    (void)self;
    if (!g_idstr_assign) return 0;
    void *defsub = defsub_for_id(id);
    if (!defsub) return 0;
    const char *c = (cls && cls[0]) ? cls : NULL;
    const char *h = (inh && inh[0]) ? inh : NULL;
    if (!c && !h) return 0;
    if (!sh_iface_class_inherit_ok(id, c, h)) return 0;
    int wrote = 0;
    /* Write inherit before class, then let the caller rebuild the source header. */
    if (h) { __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, h); wrote = 1; }
             __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (c) { __try { g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF,   c); wrote = 1; }
             __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return wrote;
}

/* Assign the full display-name idStr through IdStrAssignCStr; empty names are allowed. */
static void slot_set_displayname(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_idstr_opassign) return;
    void *array = NULL; uint32_t count = 0;
    if (!entity_array(&array, &count)) return;
    void *ent = entity_ptr(array, count, id);
    if (!ent) return;
    __try { g_idstr_opassign((uint8_t *)ent + ENT_DISPLAYNAME_FIELD, cstr ? cstr : ""); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Rebuild canonical source headers from the current class/inherit and edit body. */
static void slot_rebuild_declsource(sh_iface *self, int id, const char *cstr)
{
    (void)self;
    if (!g_decl_rebuild || !cstr) return;
    void *defsub = defsub_for_id(id);
    if (!defsub) return;
    __try { g_decl_rebuild(defsub, cstr, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Remove the ID from selection; synchronize idle state if selection becomes empty. */
static void slot_remove_from_selection(sh_iface *self, int id)
{
    (void)self;
    if (!g_remove_sel || id == -1) return;
    if (manipulation_in_progress()) return;
    const uint8_t *ed = editor_session();
    if (!ed) return;
    void *sel = NULL;
    if (!ie_read_ptr(ed + ED_SEL_OBJ_OFF_C3, &sel) || sel == NULL) return;
    __try { g_remove_sel(sel, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    int remaining = 0;
    if (ie_read_s32((const uint8_t *)sel + SEL_COUNT_OFF, &remaining) && remaining <= 0)
        mode_set_selection_state(MODE_STATE_IDLE);
}

/* Pack decl names for a short decl type into NUL-separated output. */
static int slot_enum_decls_of_resclass(sh_iface *self, const char *res_class, char *out_buf, int cap,
                                       int *out_count)
{
    (void)self;
    /* Use the non-logging decl-manager enumerator. GetDeclsOfType belongs to
 * the asset registry and logs errors for these decl-type names. */
    return sh_typeinfo_enum_decls_of_type(res_class, out_buf, cap, out_count);
}

/* Append a NUL-terminated name while reserving the final arena terminator. */
static int vcm_pack(char *out_buf, int cap, int *pw, const char *s)
{
    int nlen = (int)strlen(s);
    if (*pw + nlen + 1 > cap - 1) return 0;
    memcpy(out_buf + *pw, s, (size_t)nlen);
    out_buf[*pw + nlen] = '\0';
    *pw += nlen + 1;
    return 1;
}

/* Sort collected type records and walk superclass names locally. This avoids
 * reflect-based engine calls from the dropdown's UI thread. */
static int ec_rec_cmp(const void *a, const void *b)
{
    return strcmp(((const sh_ti_record *)a)->name, ((const sh_ti_record *)b)->name);
}
static const char *ec_super_of(const sh_ti_record *sorted, int n, const char *name)
{
    sh_ti_record key; key.name = name; key.super = NULL;
    const sh_ti_record *r = (const sh_ti_record *)bsearch(&key, sorted, (size_t)n, sizeof(sh_ti_record), ec_rec_cmp);
    return r ? r->super : NULL;
}
/* Follow at most 128 superclass links, including equality. */
static int ec_derives(const sh_ti_record *sorted, int n, const char *C, const char *Y)
{
    const char *cur = C;
    for (int g = 0; cur && cur[0] && g < 128; g++) {
        if (strcmp(cur, Y) == 0) return 1;
        cur = ec_super_of(sorted, n, cur);
    }
    return 0;
}
/* Static corpus fallback when the live registry cannot be collected. */
static int ec_fallback_valid_classes(const char *inherit, char *out_buf, int cap, int *written, int *names)
{
    const char *ey = NULL;
    for (int i = 0; i < SH_VCM_INHERIT_Y_N; i++)
        if (strcmp(SH_VCM_INHERIT_Y[i].inherit, inherit) == 0) { ey = SH_VCM_INHERIT_Y[i].y; break; }
    if (!ey) return 0;
    for (int i = 0; i < SH_VCM_Y_CLASSES_N; i++)
        if (strcmp(SH_VCM_Y_CLASSES[i].y, ey) == 0) {
            const vcm_yc *e = &SH_VCM_Y_CLASSES[i];
            for (int j = 0; j < e->n; j++) { if (!vcm_pack(out_buf, cap, written, e->classes[j])) break; (*names)++; }
            return 1;
        }
    return 0;
}

/* Enumerate classes derived from the inherit's base using collected live
 * type records; use idEntity when inherit is empty/unresolved. Fall back to
 * the static corpus if collection fails. Output is bounded by caller capacity. */
static int slot_enum_valid_classes(sh_iface *self, const char *inherit, char *out_buf, int cap, int *out_count)
{
    (void)self;
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!out_buf || cap <= 1) return 0;

    char ybuf[256];
    const char *Y = NULL;
    if (inherit && inherit[0]) Y = sh_typeinfo_inherit_base(inherit, ybuf, sizeof ybuf);
    if (!Y || !Y[0]) Y = "idEntity";

    static sh_ti_record recs[SH_REGISTRY_MAX];   /* Shared scratch: callers must serialize dropdown enumeration. */
    int k = sh_typeinfo_collect_records(recs, SH_REGISTRY_MAX);
    int written = 0, names = 0;
    if (k > 0) {
        qsort(recs, (size_t)k, sizeof(sh_ti_record), ec_rec_cmp);
        for (int i = 0; i < k; i++) {
            const char *C = recs[i].name;
            if (C && C[0] && ec_derives(recs, k, C, Y)) {
                if (!vcm_pack(out_buf, cap, &written, C)) break;
                names++;
            }
        }
    } else if (inherit && inherit[0]) {
        ec_fallback_valid_classes(inherit, out_buf, cap, &written, &names);
    }

    out_buf[written] = '\0';               /* double-NUL end marker */
    if (out_count) *out_count = names;
    return names > 0 ? 1 : 0;
}

/* Collect loaded entityDef names, sort/deduplicate, and pack into caller storage.
 * An unreachable manager returns zero for the frontend's static fallback. */
static int ec_cstr_ptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static int slot_enum_inherits(sh_iface *self, char *out_buf, int cap, int *out_count)
{
    (void)self;
    if (out_count) *out_count = 0;
    if (cap > 0 && out_buf) out_buf[0] = '\0';
    if (!out_buf || cap <= 1) return 0;

    static const char *names[SH_REGISTRY_MAX];   /* Shared scratch: callers must serialize dropdown enumeration. */
    int k = sh_typeinfo_collect_inherits(names, SH_REGISTRY_MAX);
    if (k <= 0) return 0;
    qsort((void *)names, (size_t)k, sizeof(const char *), ec_cstr_ptr_cmp);   /* Sort for adjacent deduplication; cast removes top-level pointer const. */
    int written = 0, cnt = 0;
    for (int i = 0; i < k; i++) {
        if (i > 0 && strcmp(names[i], names[i - 1]) == 0) continue;
        if (!vcm_pack(out_buf, cap, &written, names[i])) break;
        cnt++;
    }
    out_buf[written] = '\0';
    if (out_count) *out_count = cnt;
    return cnt > 0 ? 1 : 0;
}

/* Join the local app-data root, snapmap-plus, prefix, and name. The caller
 * supplies separators and any .json suffix. This is path formatting only. */
static int slot_resolve_prefab_path(sh_iface *self, const char *prefix, const char *name,
                                    char *out_path, int cap)
{
    (void)self;
    if (!out_path || cap <= 0) return 0;
    out_path[0] = '\0';
    char base[MAX_PATH];
    base[0] = '\0';

    HRESULT hr = SHGetFolderPathA(NULL, 0x1c /*CSIDL_LOCAL_APPDATA*/, NULL, 0, base);
    if (FAILED(hr)) return 0;
    _snprintf_s(out_path, (size_t)cap, _TRUNCATE, "%s/snapmap-plus/%s%s",
                base, prefix ? prefix : "", name ? name : "");
    return out_path[0] != '\0';
}

/* Lazily find the dev-layer cvar in the full registry. A miss leaves it
 * unresolved; the visibility caller then behaves as though it were disabled. */
static void *resolve_devlayer_cvar(void)
{
    if (g_devlayer_cvar) return g_devlayer_cvar;
    if (!g_cvarsys_slot) return NULL;
    void *cvarSys = NULL;
    if (!ie_read_ptr(g_cvarsys_slot, &cvarSys) || cvarSys == NULL) return NULL;
    void    *arr = NULL;
    uint32_t cnt = 0;
    if (!ie_read_ptr((const uint8_t *)cvarSys + DEVL_CVARSYS_ARR_OFF, &arr) || arr == NULL) return NULL;
    if (!ie_read_u32((const uint8_t *)cvarSys + DEVL_CVARSYS_CNT_OFF, &cnt) || cnt == 0 ||
        cnt > DEVL_CVAR_LIST_CAP) return NULL;
    for (uint32_t i = 0; i < cnt; i++) {
        void *cv = NULL;
        if (!ie_read_ptr((const uint8_t *)arr + (size_t)i * 8, &cv) || cv == NULL) continue;
        void *namep = NULL;
        if (!ie_read_ptr((const uint8_t *)cv + DEVL_CVAR_NAME_OFF, &namep) || namep == NULL) continue;
        __try {
            if (strcmp((const char *)namep, DEVL_CVAR_NAME) == 0) { g_devlayer_cvar = cv; return cv; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {   }
    }
    return NULL;
}

/* Return 1 for a non-base-layer entity unless the cvar is confirmed enabled.
 * Missing/unreadable cvar values act as disabled; entity read failures show it. */
static int slot_id_dev_layer_hidden(sh_iface *self, int id)
{
    (void)self;
    if (id < 0) return 0;
    /* Only a readable, enabled cvar bypasses the base-layer test. */
    void *cv = resolve_devlayer_cvar();
    if (cv != NULL) {
        int enabled = 0;
        if (ie_read_s32((const uint8_t *)cv + DEVL_CVAR_VALUE_OFF, &enabled) && enabled != 0) return 0;
    }
    void    *array = NULL;
    uint32_t count = 0;
    if (!entity_array(&array, &count)) return 0;
    void *ent = entity_ptr(array, count, id);
    if (ent == NULL) return 0;
    uint32_t bits = 0;
    if (!ie_read_u32((const uint8_t *)ent + ENT_LAYER_BITS_OFF, &bits)) return 0;
    return (bits & 1u) == 0 ? 1 : 0;   /* No base-layer bit. */
}

/* A wire edit can change labels without changing entity count. The UI uses
 * this generation counter to trigger another entity-list read. */
static int slot_wire_edit_generation(sh_iface *self) { (void)self; return sh_wiring_cleandirect_generation(); }

/* Push to the backend-owned stack shared with console commands. */
static void slot_push_to_stack(sh_iface *self, int index, const int *ids, int count)
{
    (void)self;
    sh_snapstack_push_ids_backend(index, ids, count);
}

/* Clear the backend-owned stack and return its previous size. */
static int slot_clear_stack(sh_iface *self, int index)
{
    (void)self;
    return sh_snapstack_clear_stack_backend(index);
}

/* Installation. */

int sh_iface_engine_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    if (module_base) {
        glb_status est = GLB_OK;
        /* Decode both globals from code references; misses remain null. */
        g_editor        = (const uint8_t *)glb_resolve(module_base, "editor_singleton", &est);
        g_cvarsys_slot  = (const uint8_t *)glb_resolve(module_base, "cvar_system_slot", NULL);
        if (!g_editor) {
            char el[192];
            _snprintf_s(el, sizeof el, _TRUNCATE,
                "C2: the editor singleton did not resolve (status=%d, pinned 0x%x) -- every editor slot "
                "will return its empty answer rather than read a pinned address",
                (int)est, (unsigned)EDITOR_SINGLETON_PINNED_RVA);
            backend_log(el);
        }
    }

    g_add_sel    = (add_to_sel_fn)sig_addr_by_name(results, n, "AddToSelection");
    g_clear_sel  = (clear_sel_fn)sig_addr_by_name(results, n, "ClearSelection");
    g_toast      = (toast_fn)sig_addr_by_name(results, n, "Toast");
    g_idstr_ctor = (idstr_ctor_fn)sig_addr_by_name(results, n, "IdStrCtor");
    g_idstr_dtor = (idstr_dtor_fn)sig_addr_by_name(results, n, "IdStrDtor");
    /* Read shared signature results; callbacks check unresolved dependencies. */
    g_idstr_assign = (idstr_assign_fn)    sig_addr_by_name(results, n, "IdStrAssign");
    g_decl_rebuild = (decl_src_rebuild_fn)sig_addr_by_name(results, n, "DeclSourceRebuild");
    g_get_decls    = (get_decls_fn)       sig_addr_by_name(results, n, "GetDeclsOfType"); /* +0x110 */
    g_remove_sel     = (remove_from_sel_fn)sig_addr_by_name(results, n, "RemoveFromSelection"); /* +0x130 */
    g_idstr_opassign = (idstr_opassign_fn) sig_addr_by_name(results, n, "IdStrAssignCStr");     /* +0x128 */

    /* Bind callbacks even when some engine dependencies are unavailable. */
    sh_iface_engine_slots slots;
    memset(&slots, 0, sizeof slots);
    slots.set_editor_vec3    = slot_set_editor_vec3;          /* +0x00  (camera) */
    slots.get_editor_vec3    = slot_get_editor_vec3;          /* +0x08  (camera) */
    slots.entity_count       = slot_entity_count;
    slots.id_to_string       = slot_id_to_string;
    slots.is_valid_id        = slot_is_valid_id;
    slots.editor_ready_poll  = slot_editor_ready;            /* +0x88  (window gate) */
    slots.get_classname_copy = slot_get_classname;
    slots.get_inherit_copy   = slot_get_inherit;
    slots.add_to_selection   = slot_add_to_selection;
    slots.clear_selection    = slot_clear_selection;
    slots.get_selection      = slot_get_selection;
    slots.hovered_id         = slot_hovered_id;
    slots.is_entity_mode     = slot_is_entity_mode;          /* +0x1c0 (Create-New-Timeline gate / button gray-out) */
    slots.toast              = slot_toast;

    slots.get_declsource_copy    = slot_get_declsource;       /* +0x30  */
    slots.rebuild_set_declsource = slot_rebuild_declsource;   /* +0x40  */
    slots.get_displayname        = slot_get_displayname;      /* +0x58  */
    slots.set_classname          = slot_set_classname;        /* +0x78  */
    slots.set_inherit            = slot_set_inherit;          /* +0x80  */
    slots.set_displayname        = slot_set_displayname;      /* +0x128 */
    slots.resolve_prefab_path    = slot_resolve_prefab_path;  /* +0xc0  */
    slots.remove_from_selection  = slot_remove_from_selection;/* +0x130 */

    slots.enum_decls_of_resclass = slot_enum_decls_of_resclass;/* +0x110 */

    slots.apply_class_inherit    = slot_apply_class_inherit;  /* +0x268 ext 0 */

    slots.enum_valid_classes     = slot_enum_valid_classes;   /* +0x270 ext 1 */

    slots.enum_inherits          = slot_enum_inherits;        /* +0x278 ext 2 */

    slots.id_dev_layer_hidden    = slot_id_dev_layer_hidden;  /* +0x280 ext 3 */

    slots.wire_edit_generation   = slot_wire_edit_generation; /* +0x288 ext 4 */
    /* Apply installation runs first; collect its checked callbacks into this binding. */
    sh_apply_engine_get_slots(&slots.serialize_entity, &slots.apply_edit, &slots.read_prefab,
                              &slots.apply_sync,        /* +0x290 synchronous apply, marshaled when available. */
                              &slots.normalize_timeline_inherit); /* +0x298 palette-timeline portable-inherit */

    sh_apply_engine_get_serialize_selection(&slots.serialize_selection);

    slots.push_to_stack          = slot_push_to_stack;        /* +0x2A0 ext 7 */

    slots.clear_stack            = slot_clear_stack;          /* +0x2A8 ext 8 */

    slots.manipulation_in_progress = slot_manipulation_in_progress;  /* +0x2C0 ext 11 */

    slots.find_material           = slot_find_material;              /* +0x2C8 ext 12 */
    slots.get_preview             = slot_get_preview;                /* +0x2D0 ext 13 */
    slots.request_preview         = slot_request_preview;            /* +0x2D8 ext 14 */
    slots.list_materials          = slot_list_materials;             /* +0x2E0 ext 15 */
    slots.list_assets             = slot_list_assets;                /* +0x2E8 ext 16 */
    slots.material_rect           = slot_material_rect;              /* +0x2F0 ext 17 */
    slots.sound_preview           = slot_sound_preview;              /* +0x2F8 ext 18 */
    slots.sound_session           = slot_sound_session;              /* +0x300 ext 19 */
    slots.resolve_prefab_model    = slot_resolve_prefab_model;       /* +0x308 ext 20 */
    slots.request_prefab_mesh     = slot_request_prefab_mesh;        /* +0x310 ext 21 */
    slots.get_prefab_mesh         = slot_get_prefab_mesh;            /* +0x318 ext 22 */
    slots.resolve_prefab_defaults = slot_resolve_prefab_defaults;    /* +0x320 ext 23 */
    sh_iface_bind_engine_slots(&slots);

    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2: iface engine slots bound (editor=%p add_sel=%p clear_sel=%p toast=%p idstr=%p/%p)",
        (void *)g_editor, (void *)g_add_sel, (void *)g_clear_sel, (void *)g_toast,
        (void *)g_idstr_ctor, (void *)g_idstr_dtor);
    backend_log(line);
    return 10;
}
