/* prefab_anchor.c -- see prefab_anchor.h.
 *
 * Native boundaries, located independently in each executable:
 *
 *   PrefabPopulate              Vulkan 0x54E410  OpenGL 0x54DA80
 *   grab frame (camera x box)   Vulkan 0x5BAB30  OpenGL 0x5BA220
 *
 * Populate reads selection+0x2C once: it requires that entity in the
 * selection (a linear scan of the list at +0x80/+0x88), then passes the
 * entity's body and the editor to the grab-frame helper, which reads the
 * view at editor+0x170/+0x17C and writes grabDistance to prefab+0x24. No
 * callee reads the hover again, so an installed hover affects only the
 * admission check and the frame, exactly as a native hover would.
 *
 * Before the frame, populate moves each selected body from its module into
 * the root frame (see prefab_anchor.h), so the box is tested in world space,
 * not in the module space the live body holds.
 * The helper builds the box from the body: bounds at +0x2C4, or, when the
 * declaration byte +0x3CE has bit 0x08, sizes at +0x400/+0x404/+0x434
 * centered on the origin; origin +0x288; axis rows +0x294. It clips the line
 * origin + t*forward against each axis pair from [-1e30, 1e30], ignores a
 * miss, and takes the midpoint of whatever bounds remain. The view origin is
 * read only there, so a moved origin changes the frame and nothing else.
 */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "prefab_anchor.h"

/* Editor, selection and map layouts, identical in both executables. */
#define ED_MAP_OFF           0x204C8  /* loaded map; null outside an editor session */
#define ED_SEL_OFF           0x204D0  /* selection object */
#define ED_VIEW_OFF          0x170    /* view origin, then angles at +0x17C */
#define SEL_HOVER_OFF        0x2C     /* hovered entity, -1 for none */
#define SEL_IDS_OFF          0x80
#define SEL_COUNT_OFF        0x88
#define MAP_ENTS_OFF         0x6A0    /* entity slot pointers */
#define MAP_ENTCOUNT_OFF     0x6A8
#define MAP_ENT_MODULE_OFF   0x6F0    /* module index per entity */
#define MAP_MODULES_OFF      0x750    /* module records */
#define MAP_MODULE_COUNT_OFF 0x758    /* also the index of the root frame */
#define MODULE_STRIDE        0x98
#define MODULE_TRANSLATION_OFF 0x0C
#define MODULE_ROTATION_OFF  0x18
#define SLOT_DECL_OFF        0x08     /* a released slot has no declaration */

#define BODY_ORIGIN_OFF      0x288    /* body = slot + 8, declaration pointer first */
#define BODY_AXIS_OFF        0x294
#define BODY_BOUNDS_OFF      0x2C4
#define BODY_SIZE_X_OFF      0x400
#define BODY_SIZE_Y_OFF      0x404
#define BODY_SIZE_Z_OFF      0x434
#define DECL_FLAGS_OFF       0x3CE
#define DECL_FIXED_SIZE      0x08
#define DECL_ROW_FLAGS_OFF   0x3CC
#define DECL_SECOND_ROW_MIRROR 0x04

#define ENTITY_LIMIT         1000000  /* stale-map guard on the entity count */
#define MODULE_LIMIT         65536    /* stale-map guard on the module count */
#define LIST_LIMIT           65536    /* stale-object guard on the selection list */

void sh_prefab_anchor_reset(sh_prefab_anchor *anchor)
{
    if (!anchor) return;
    memset(anchor, 0, sizeof *anchor);
    anchor->id = anchor->miss_id = anchor->miss_count = -1;
}

void sh_prefab_anchor_observe(sh_prefab_anchor *anchor, const void *map, int hovered, int count,
                              const float view[SH_PREFAB_ANCHOR_VIEW],
                              sh_prefab_anchor_selected_fn selected, void *context)
{
    if (!anchor) return;
    if (!map || !view) { sh_prefab_anchor_reset(anchor); return; }
    if (anchor->map != map) { sh_prefab_anchor_reset(anchor); anchor->map = map; }
    if (hovered < 0) return;
    /* Still pointing at the anchor: keep the view the frame would use. */
    if (hovered == anchor->id) { memcpy(anchor->view, view, sizeof anchor->view); return; }
    if (hovered == anchor->miss_id && count == anchor->miss_count) return;
    if (selected && selected(context, hovered)) {
        anchor->id = hovered;
        memcpy(anchor->view, view, sizeof anchor->view);
        anchor->miss_id = anchor->miss_count = -1;
    } else {
        anchor->miss_id = hovered; anchor->miss_count = count;
    }
}

static int angle_close(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360.0f);
    if (d > 180.0f) d = 360.0f - d;
    return d <= SH_PREFAB_ANCHOR_ANGLE_TOLERANCE;
}

int sh_prefab_anchor_choose(const sh_prefab_anchor *anchor, const void *map,
                            const float view[SH_PREFAB_ANCHOR_VIEW],
                            sh_prefab_anchor_selected_fn selected, void *context)
{
    if (!anchor || !map || anchor->map != map || anchor->id < 0 || !view || !selected ||
        !selected(context, anchor->id)) return -1;
    /* NaN compares false, so a corrupt view never matches. */
    for (int i = 0; i < 3; i++)
        if (!(fabsf(view[i] - anchor->view[i]) <= SH_PREFAB_ANCHOR_POSITION_TOLERANCE)) return -1;
    for (int i = 3; i < SH_PREFAB_ANCHOR_VIEW; i++)
        if (!angle_close(view[i], anchor->view[i])) return -1;
    return anchor->id;
}

int sh_prefab_anchor_distance_ok(float distance)
{
    return distance == distance && fabsf(distance) <= SH_PREFAB_ANCHOR_MAX_DISTANCE;
}

static float pa_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* One side of a slab: keep the t with p*t >= q, as the native clip does. */
static int pa_clip(float p, float q, float *t0, float *t1)
{
    float t;
    if (p == 0.0f) return q <= 0.0f;
    t = q / p;
    if (p > 0.0f) { if (t > *t1) return 0; if (t > *t0) *t0 = t; }
    else          { if (t < *t0) return 0; if (t < *t1) *t1 = t; }
    return 1;
}

/* The grab-frame line test against a box given by center, axis rows and half
 * extents, in the native slab order. */
static int pa_line(const float center[3], const float axis[9], const float extent[3],
                   const float origin[3], const float dir[3], float *t0, float *t1)
{
    float rel[3] = { origin[0] - center[0], origin[1] - center[1], origin[2] - center[2] };
    *t0 = -1.0e30f; *t1 = 1.0e30f;
    for (int i = 0; i < 3; i++) {
        float r = pa_dot(rel, axis + 3 * i), p = pa_dot(dir, axis + 3 * i);
        if (!pa_clip(p, -r - extent[i], t0, t1) || !pa_clip(-p, r - extent[i], t0, t1)) return 0;
    }
    return 1;
}

int sh_prefab_anchor_equivalent(const sh_prefab_anchor_box *box, const float view[SH_PREFAB_ANCHOR_VIEW],
                                float origin[3], float *depth)
{
    const float rad = 0.017453292f;
    float local[3], center[3], extent[3], shrunk[3], dir[3], to[3], t0, t1, s;
    if (!box || !view || !origin || !depth) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    for (int i = 0; i < SH_PREFAB_ANCHOR_VIEW; i++) if (!isfinite(view[i])) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    for (int i = 0; i < 9; i++) if (!isfinite(box->axis[i])) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    for (int i = 0; i < 3; i++) {
        local[i] = (box->bounds[i] + box->bounds[i + 3]) * 0.5f;
        extent[i] = box->bounds[i + 3] - local[i];
        shrunk[i] = extent[i] * (1.0f - SH_PREFAB_ANCHOR_RAY_MARGIN);
        if (!isfinite(local[i]) || !isfinite(extent[i]) || extent[i] < 0.0f || !isfinite(box->origin[i]))
            return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    }
    /* Box center in the world: the origin plus the bounds center along the axis rows. */
    for (int i = 0; i < 3; i++)
        center[i] = box->origin[i] + local[0] * box->axis[i] + local[1] * box->axis[3 + i] +
                    local[2] * box->axis[6 + i];
    dir[0] = cosf(view[3] * rad) * cosf(view[4] * rad);
    dir[1] = cosf(view[3] * rad) * sinf(view[4] * rad);
    dir[2] = -sinf(view[3] * rad);
    if (pa_line(center, box->axis, shrunk, view, dir, &t0, &t1)) return SH_PREFAB_ANCHOR_RAY_MEETS;
    for (int i = 0; i < 3; i++) to[i] = center[i] - view[i];
    s = pa_dot(to, dir);
    if (!isfinite(s) || !(s > 0.0f)) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    for (int i = 0; i < 3; i++) origin[i] = center[i] - dir[i] * s;
    /* Through the center the clipped bounds are symmetric about s. */
    if (!pa_line(center, box->axis, extent, origin, dir, &t0, &t1) || !isfinite(t0) || !isfinite(t1))
        return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    *depth = s;
    return SH_PREFAB_ANCHOR_RAY_EQUIVALENT;
}

int sh_prefab_anchor_native_frame(const sh_prefab_anchor_box *box, const float origin[3],
                                  const float view[SH_PREFAB_ANCHOR_VIEW], float *mid)
{
    const float rad = 0.017453292f;
    float local[3], center[3], extent[3], dir[3], t0, t1;
    int met;
    if (!box || !origin || !view || !mid) return 0;
    for (int i = 0; i < 3; i++) { local[i] = (box->bounds[i] + box->bounds[i + 3]) * 0.5f; extent[i] = box->bounds[i + 3] - local[i]; }
    for (int i = 0; i < 3; i++)
        center[i] = box->origin[i] + local[0] * box->axis[i] + local[1] * box->axis[3 + i] +
                    local[2] * box->axis[6 + i];
    dir[0] = cosf(view[3] * rad) * cosf(view[4] * rad);
    dir[1] = cosf(view[3] * rad) * sinf(view[4] * rad);
    dir[2] = -sinf(view[3] * rad);
    met = pa_line(center, box->axis, extent, origin, dir, &t0, &t1);
    *mid = (t0 + t1) * 0.5f;
    return met;
}

int sh_prefab_anchor_frame_matches(float distance, float expect)
{
    float tolerance = fmaxf(0.05f, fabsf(expect) * 1.0e-3f);
    return distance == distance && expect == expect && fabsf(distance - expect) <= tolerance;
}

void sh_prefab_anchor_rotation(int index, float m[9])
{
    /* Rows of the module rotation, as VK 0x511460 writes them. */
    static const float table[8][9] = {
        {  1,  0, 0,   0,  1, 0,  0, 0, 1 },
        {  0,  1, 0,  -1,  0, 0,  0, 0, 1 },
        { -1,  0, 0,   0, -1, 0,  0, 0, 1 },
        {  0, -1, 0,   1,  0, 0,  0, 0, 1 },
        { -1,  0, 0,   0,  1, 0,  0, 0, 1 },
        {  0,  1, 0,   1,  0, 0,  0, 0, 1 },
        {  1,  0, 0,   0, -1, 0,  0, 0, 1 },
        {  0, -1, 0,  -1,  0, 0,  0, 0, 1 },
    };
    memcpy(m, table[index >= 1 && index <= 7 ? index : 0], 9 * sizeof(float));
}

void sh_prefab_anchor_root_frame(const float origin[3], const float axis[9], const float translation[3],
                                 int rotation, unsigned char flags3cc, unsigned char flags3ce,
                                 float out_origin[3], float out_axis[9])
{
    float m[9], o[3], row[9];
    int mirrored = -1;
    sh_prefab_anchor_rotation(rotation, m);
    for (int j = 0; j < 3; j++)
        o[j] = origin[0] * m[j] + origin[1] * m[3 + j] + origin[2] * m[6 + j] + translation[j];
    for (int r = 0; r < 3; r++)
        for (int j = 0; j < 3; j++)
            row[3 * r + j] = axis[3 * r] * m[j] + axis[3 * r + 1] * m[3 + j] + axis[3 * r + 2] * m[6 + j];
    if (rotation > 3) {
        if (flags3ce & DECL_FIXED_SIZE) mirrored = 0;
        else if (flags3cc & DECL_SECOND_ROW_MIRROR) mirrored = 1;
    }
    if (mirrored >= 0)
        for (int j = 0; j < 3; j++) row[3 * mirrored + j] = -row[3 * mirrored + j];
    memcpy(out_origin, o, sizeof o);
    memcpy(out_axis, row, sizeof row);
}

/* ---- engine side: main thread only, every access guarded ---- */

static sh_prefab_anchor g_anchor = { NULL, -1, { 0 }, -1, -1 };

typedef struct pa_list { const int *ids; int count; } pa_list;

static int pa_read(const void *p, void *out, size_t size)
{
    __try { memcpy(out, p, size); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int pa_write(void *p, int value)
{
    __try { *(volatile int *)p = value; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int pa_write_origin(void *p, const float origin[3])
{
    __try {
        volatile float *f = (volatile float *)p;
        f[0] = origin[0]; f[1] = origin[1]; f[2] = origin[2];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* The native admission predicate: a linear scan of the selection list. */
static int pa_selected(void *context, int id)
{
    const pa_list *list = (const pa_list *)context;
    __try {
        for (int i = 0; i < list->count; i++)
            if (list->ids[i] == id) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

/* Read the map, selection and view; 0 when the editor has none usable. */
static int pa_state(void *editor, const void **map, uint8_t **sel, int *hovered, pa_list *list,
                    float view[SH_PREFAB_ANCHOR_VIEW])
{
    const uint8_t *ed = (const uint8_t *)editor;
    const int *ids = NULL;
    int count = 0;
    *map = NULL; *sel = NULL; *hovered = -1; list->ids = NULL; list->count = 0;
    if (!ed || !pa_read(ed + ED_MAP_OFF, (void *)map, sizeof *map) || !*map ||
        !pa_read(ed + ED_SEL_OFF, sel, sizeof *sel) || !*sel ||
        !pa_read(*sel + SEL_HOVER_OFF, hovered, sizeof *hovered) ||
        !pa_read(*sel + SEL_IDS_OFF, &ids, sizeof ids) ||
        !pa_read(*sel + SEL_COUNT_OFF, &count, sizeof count) ||
        count < 0 || count > LIST_LIMIT || (count && !ids) ||
        !pa_read(ed + ED_VIEW_OFF, view, SH_PREFAB_ANCHOR_VIEW * sizeof(float))) return 0;
    list->ids = ids; list->count = count;
    return 1;
}

/* Populate copies the slot body at +8; a released slot has no declaration. */
static const uint8_t *pa_body(const void *map, int id, const uint8_t **decl_out)
{
    const uint8_t *slots = NULL, *slot = NULL, *decl = NULL;
    int count = 0;
    if (id >= 0 &&
        pa_read((const uint8_t *)map + MAP_ENTS_OFF, &slots, sizeof slots) && slots &&
        pa_read((const uint8_t *)map + MAP_ENTCOUNT_OFF, &count, sizeof count) &&
        count > 0 && count <= ENTITY_LIMIT && id < count &&
        pa_read(slots + (size_t)id * sizeof(void *), &slot, sizeof slot) && slot &&
        pa_read(slot + SLOT_DECL_OFF, &decl, sizeof decl) && decl) {
        if (decl_out) *decl_out = decl;
        return slot + SLOT_DECL_OFF;
    }
    return NULL;
}
static int pa_live(const void *map, int id) { return pa_body(map, id, NULL) != NULL; }

/* Move a body's origin and axis into the root frame, as populate does first. */
static int pa_root(const void *map, int id, uint8_t flags3cc, uint8_t flags3ce, sh_prefab_anchor_box *box)
{
    const uint8_t *modules_of = NULL, *modules = NULL;
    int module = -1, count = -1, rotation = 0;
    float translation[3];
    if (!pa_read((const uint8_t *)map + MAP_ENT_MODULE_OFF, &modules_of, sizeof modules_of) || !modules_of ||
        !pa_read(modules_of + (size_t)id * sizeof(int), &module, sizeof module) ||
        !pa_read((const uint8_t *)map + MAP_MODULE_COUNT_OFF, &count, sizeof count) ||
        count < 0 || count > MODULE_LIMIT || module < 0 || module > count) return 0;
    if (module == count) return 1;
    if (!pa_read((const uint8_t *)map + MAP_MODULES_OFF, &modules, sizeof modules) || !modules ||
        !pa_read(modules + (size_t)module * MODULE_STRIDE + MODULE_TRANSLATION_OFF, translation, sizeof translation) ||
        !pa_read(modules + (size_t)module * MODULE_STRIDE + MODULE_ROTATION_OFF, &rotation, sizeof rotation)) return 0;
    sh_prefab_anchor_root_frame(box->origin, box->axis, translation, rotation, flags3cc, flags3ce, box->origin, box->axis);
    return 1;
}

/* The box the grab-frame helper builds for an entity, in the root frame. */
static int pa_box(const void *map, int id, sh_prefab_anchor_box *box)
{
    const uint8_t *decl = NULL, *body = pa_body(map, id, &decl);
    uint8_t flags = 0, row_flags = 0;
    float size[3];
    if (!body || !pa_read(decl + DECL_FLAGS_OFF, &flags, sizeof flags) ||
        !pa_read(decl + DECL_ROW_FLAGS_OFF, &row_flags, sizeof row_flags) ||
        !pa_read(body + BODY_ORIGIN_OFF, box->origin, sizeof box->origin) ||
        !pa_read(body + BODY_AXIS_OFF, box->axis, sizeof box->axis) ||
        !pa_root(map, id, row_flags, flags, box)) return 0;
    if (!(flags & DECL_FIXED_SIZE)) return pa_read(body + BODY_BOUNDS_OFF, box->bounds, sizeof box->bounds);
    if (!pa_read(body + BODY_SIZE_X_OFF, &size[0], sizeof size[0]) ||
        !pa_read(body + BODY_SIZE_Y_OFF, &size[1], sizeof size[1]) ||
        !pa_read(body + BODY_SIZE_Z_OFF, &size[2], sizeof size[2])) return 0;
    for (int i = 0; i < 3; i++) { box->bounds[i] = size[i] * -0.5f; box->bounds[i + 3] = size[i] * 0.5f; }
    return 1;
}

void sh_prefab_anchor_frame(void *editor)
{
    const void *map;
    uint8_t *sel;
    int hovered;
    pa_list list;
    float view[SH_PREFAB_ANCHOR_VIEW];
    if (!pa_state(editor, &map, &sel, &hovered, &list, view)) { sh_prefab_anchor_reset(&g_anchor); return; }
    sh_prefab_anchor_observe(&g_anchor, map, hovered, list.count, view, pa_selected, &list);
}

int sh_prefab_anchor_install(void *editor, sh_prefab_anchor_hold *hold)
{
    const void *map;
    uint8_t *sel;
    int hovered, anchor;
    pa_list list;
    float view[SH_PREFAB_ANCHOR_VIEW];
    if (!hold) return SH_PREFAB_ANCHOR_NONE;
    hold->selection = NULL; hold->anchor = hold->previous = hold->entity = -1;
    hold->view = NULL; hold->depth = 0.0f; hold->expect = NAN;
    memset(hold->seen, 0, sizeof hold->seen);
    if (!pa_state(editor, &map, &sel, &hovered, &list, view)) return SH_PREFAB_ANCHOR_NONE;
    if (!list.count) return SH_PREFAB_ANCHOR_EMPTY;
    if (hovered != -1) return SH_PREFAB_ANCHOR_NATIVE;
    anchor = sh_prefab_anchor_choose(&g_anchor, map, view, pa_selected, &list);
    if (anchor < 0 || !pa_live(map, anchor) || !pa_write(sel + SEL_HOVER_OFF, anchor))
        return SH_PREFAB_ANCHOR_NONE;
    hold->selection = sel; hold->anchor = anchor; hold->previous = hovered;
    return SH_PREFAB_ANCHOR_INSTALLED;
}

int sh_prefab_anchor_aim(void *editor, sh_prefab_anchor_hold *hold)
{
    const void *map;
    uint8_t *sel;
    int hovered, decision;
    pa_list list;
    float view[SH_PREFAB_ANCHOR_VIEW], origin[3], depth = 0.0f;
    sh_prefab_anchor_box box;
    if (!hold) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    hold->entity = -1; hold->view = NULL; hold->depth = 0.0f; hold->expect = NAN;
    memset(hold->seen, 0, sizeof hold->seen);
    if (!pa_state(editor, &map, &sel, &hovered, &list, view)) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    memcpy(hold->seen, view, sizeof hold->seen);
    /* Populate refuses an unselected hover before it builds a frame. */
    if (hovered < 0 || !pa_selected(&list, hovered)) return SH_PREFAB_ANCHOR_RAY_MEETS;
    hold->entity = hovered;
    if (!pa_box(map, hovered, &box)) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    decision = sh_prefab_anchor_equivalent(&box, view, origin, &depth);
    if (decision == SH_PREFAB_ANCHOR_RAY_UNUSABLE) return decision;
    /* The frame populate will write, so that a different one is caught afterwards. */
    if (!sh_prefab_anchor_native_frame(&box, decision == SH_PREFAB_ANCHOR_RAY_EQUIVALENT ? origin : view,
                                       view, &hold->expect)) return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    if (decision != SH_PREFAB_ANCHOR_RAY_EQUIVALENT) return decision;
    if (!pa_write_origin((uint8_t *)editor + ED_VIEW_OFF, origin)) {
        (void)pa_write_origin((uint8_t *)editor + ED_VIEW_OFF, view);
        return SH_PREFAB_ANCHOR_RAY_UNUSABLE;
    }
    hold->view = (uint8_t *)editor + ED_VIEW_OFF;
    memcpy(hold->origin, view, sizeof hold->origin);
    memcpy(hold->moved, origin, sizeof hold->moved);
    hold->depth = depth;
    return SH_PREFAB_ANCHOR_RAY_EQUIVALENT;
}

void sh_prefab_anchor_restore(const sh_prefab_anchor_hold *hold)
{
    int current = -1;
    float origin[3];
    if (!hold) return;
    /* Put back only what this hold wrote; keep anything the engine set meanwhile. */
    if (hold->view && pa_read(hold->view, origin, sizeof origin) &&
        !memcmp(origin, hold->moved, sizeof origin))
        (void)pa_write_origin(hold->view, hold->origin);
    if (!hold->selection) return;
    /* Keep a hover the engine set meanwhile; remove only the installed one. */
    if (pa_read((const uint8_t *)hold->selection + SEL_HOVER_OFF, &current, sizeof current) &&
        current == hold->anchor)
        (void)pa_write((uint8_t *)hold->selection + SEL_HOVER_OFF, hold->previous);
}
