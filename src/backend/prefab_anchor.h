/* prefab_anchor.h -- the selected entity a native prefab is created around.
 *
 * Native PrefabPopulate admits a selection only through the hovered entity
 * (selection+0x2C), and builds the prefab's grab frame from the editor view:
 * grabDistance is the midpoint where the camera ray (editor+0x170 origin,
 * +0x17C angles) enters and leaves that entity's box, and grabAxis is the
 * entity's axis. Bringing the overlay forward clears the hover, so Create
 * from Selection could never succeed with the mouse.
 *
 * Every editor frame records the last hovered entity that was selected and
 * the view it was hovered from. A selection export without a hover installs
 * that entity as the hover for the populate call alone, only while the native
 * selection still contains it, its slot is live and the view still matches,
 * so the native frame is the one the user saw. Native admission decides the
 * rest. Anything else is refused rather than anchored on an entity the camera
 * ray may miss: a missed box leaves an unclipped ray bound at 1e30.
 *
 * The hover test is not the box test, so even a hovered entity's box can be
 * missed by the view ray, and an axis-aligned or grazing ray clips to nothing.
 * The native helper ignores that miss and keeps the partly clipped bounds, so
 * the grab point lands thousands of units away or near 1e10. Whatever set the
 * hover, when the ray would miss the selected hovered entity's box, the view
 * origin is moved within the view plane for the populate call alone, so the
 * same view direction passes through the box center: grabDistance becomes the
 * anchor's depth along the view, the grab point its box center, and the yaw
 * and axis are unchanged -- the native frame of a ray through the anchor.
 */
#ifndef SH_PREFAB_ANCHOR_H
#define SH_PREFAB_ANCHOR_H

#define SH_PREFAB_ANCHOR_VIEW 6              /* view origin xyz, angles pitch/yaw/roll */
#define SH_PREFAB_ANCHOR_POSITION_TOLERANCE 0.5f
#define SH_PREFAB_ANCHOR_ANGLE_TOLERANCE    0.25f  /* degrees */
/* No grab point inside a map is this far from the camera; a missed box is
 * at least 5e29 away. */
#define SH_PREFAB_ANCHOR_MAX_DISTANCE       1.0e6f

typedef int (*sh_prefab_anchor_selected_fn)(void *context, int id);

typedef struct sh_prefab_anchor {
    const void *map;       /* map the record belongs to; NULL when none */
    int id;                /* last hovered entity that was selected; -1 for none */
    float view[SH_PREFAB_ANCHOR_VIEW];
    int miss_id;           /* last hovered entity found unselected */
    int miss_count;        /* selection count when it was checked */
} sh_prefab_anchor;

void sh_prefab_anchor_reset(sh_prefab_anchor *anchor);
/* Record one frame's native hover; count is the native selection count. */
void sh_prefab_anchor_observe(sh_prefab_anchor *anchor, const void *map, int hovered, int count,
                              const float view[SH_PREFAB_ANCHOR_VIEW],
                              sh_prefab_anchor_selected_fn selected, void *context);
/* Return the recorded entity when it may stand in for the hover now, else -1. */
int sh_prefab_anchor_choose(const sh_prefab_anchor *anchor, const void *map,
                            const float view[SH_PREFAB_ANCHOR_VIEW],
                            sh_prefab_anchor_selected_fn selected, void *context);
/* Whether a populated grab distance came from a ray that met the anchor. */
int sh_prefab_anchor_distance_ok(float distance);

/* The box the grab-frame helper tests: bounds (min xyz, max xyz) in the
 * entity frame, the entity origin, and its axis rows. */
typedef struct sh_prefab_anchor_box {
    float bounds[6];
    float origin[3];
    float axis[9];
} sh_prefab_anchor_box;

/* The ray must meet the box shrunk by this fraction of each half extent, so
 * a grazing ray is not left to rounding. */
#define SH_PREFAB_ANCHOR_RAY_MARGIN (1.0f / 64.0f)

enum {
    SH_PREFAB_ANCHOR_RAY_MEETS = 0,       /* the view ray meets the box; the native frame stands */
    SH_PREFAB_ANCHOR_RAY_EQUIVALENT = 1,  /* origin moved so the ray passes the box center */
    SH_PREFAB_ANCHOR_RAY_UNUSABLE = -1    /* box behind the view, or no finite frame */
};
/* Decide the populate view for a box. EQUIVALENT writes the moved origin and
 * the box center's depth along the view. */
int sh_prefab_anchor_equivalent(const sh_prefab_anchor_box *box, const float view[SH_PREFAB_ANCHOR_VIEW],
                                float origin[3], float *depth);
/* The grab-frame helper's own result for a box, a view origin and the view
 * angles: 1 when the line meets the box, and the midpoint it writes either way. */
int sh_prefab_anchor_native_frame(const sh_prefab_anchor_box *box, const float origin[3],
                                  const float view[SH_PREFAB_ANCHOR_VIEW], float *mid);
/* Whether a populated grab distance is the one predicted for the frame. */
int sh_prefab_anchor_frame_matches(float distance, float expect);

/* Populate first moves every selected entity out of its module into the root
 * frame (VK 0x551360, GL 0x550820), and the helper tests that moved copy. An
 * entity's module is map+0x6F0[id]; the module count at map+0x758 means the
 * root. A module record (map+0x750, stride 0x98) holds a translation at +0x0C
 * and a rotation index at +0x18: origin' = origin * R + t, each axis row' =
 * row * R. Rotation indices 0..3 turn about z in quarter turns and 4..7 are
 * their mirror images (VK 0x511460). A mirror negates the first axis row of a
 * fixed-size declaration (+0x3CE bit 0x08), the second row of one flagged at
 * +0x3CC bit 0x04 otherwise, and no row of any other declaration. */
void sh_prefab_anchor_rotation(int index, float m[9]);
void sh_prefab_anchor_root_frame(const float origin[3], const float axis[9], const float translation[3],
                                 int rotation, unsigned char flags3cc, unsigned char flags3ce,
                                 float out_origin[3], float out_axis[9]);

/* Engine side. Field layouts are identical in both executables. */
typedef struct sh_prefab_anchor_hold {
    void *selection;       /* selection object the hover was written to */
    int anchor;            /* installed hover */
    int previous;          /* hover before installation */
    int entity;            /* hovered entity whose box was tested; -1 for none */
    void *view;            /* view origin moved for populate; NULL when unchanged */
    float origin[3];       /* view origin before the move */
    float moved[3];        /* equivalent origin written */
    float depth;           /* box center depth along the view */
    float seen[SH_PREFAB_ANCHOR_VIEW];  /* view populate would have used */
    float expect;          /* grabDistance the native helper should write; NaN when not predicted */
} sh_prefab_anchor_hold;

enum {
    SH_PREFAB_ANCHOR_NATIVE = 0,     /* a native hover exists; populate decides */
    SH_PREFAB_ANCHOR_INSTALLED = 1,
    SH_PREFAB_ANCHOR_EMPTY = -1,     /* nothing selected */
    SH_PREFAB_ANCHOR_NONE = -2       /* no selected entity can anchor the frame */
};

/* Main thread, once per editor frame after the native update. */
void sh_prefab_anchor_frame(void *editor);
/* Main thread, immediately before populate. Restore before returning to the
 * engine whenever this reports SH_PREFAB_ANCHOR_INSTALLED. */
int sh_prefab_anchor_install(void *editor, sh_prefab_anchor_hold *hold);
/* Main thread, after install reports NATIVE or INSTALLED: test the populate
 * ray against the hovered entity's box and move the view origin when it would
 * miss. Returns a SH_PREFAB_ANCHOR_RAY_* value; restore undoes the move. */
int sh_prefab_anchor_aim(void *editor, sh_prefab_anchor_hold *hold);
void sh_prefab_anchor_restore(const sh_prefab_anchor_hold *hold);

#endif
