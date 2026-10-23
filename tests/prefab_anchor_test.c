/* Prefab creation from the overlay: the recorded selected anchor stands in for
 * the native hover only while the native selection and view still allow it. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/prefab_anchor.c"

static int failed;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

/* Native layouts, as the engine side reads them. */
static unsigned char editor[0x24000];
static unsigned char selection[0x100];
static unsigned char map_a[0x800], map_b[0x800];
static unsigned char bodies[100][0x480];
static void *slots[100];
static int ids[16];
static unsigned char decl[0x400], decl_fixed[0x400];
static int modules_of[100];                  /* module per entity; 1, the count, is the root */
static unsigned char modules[MODULE_STRIDE];

static int *hover(void) { return (int *)(selection + SEL_HOVER_OFF); }
static float *view(void) { return (float *)(editor + ED_VIEW_OFF); }
static float *body_f(int id, int off) { return (float *)(bodies[id] + SLOT_DECL_OFF + off); }

/* M114/M115 live values: the P trigger (entity 69), in its module's frame, and
 * the view the daemon framed on that module-local origin. Here P stands in the
 * root frame unless a control moves it into a module. */
static const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static const float p_bounds[6] = { -3.0f, -12.0f, 0.0f, 3.0f, 12.0f, 24.0f };
static const float p_origin[3] = { -1237.229f, 1361.392f, 64.304f };
static const float p_center[3] = { -1237.229f, 1361.392f, 76.304f };
static const float framed[6] = { -1400.6212f, 1109.7913f, 238.3041f, 26.565052f, 57.0f, 0.0f };
/* A synthetic view with P behind it, missing the y slab; its clipped bounds
 * also give 2700.23. The live 2700.23 came from the module frame (below). */
static const float drifted[6] = { -2552.5f, 4053.8f, 238.3041f, 26.565052f, 57.0f, 0.0f };
/* The framed view moved 30 units sideways: P is ahead, the ray passes beside it. */
static const float beside[6] = { -1425.7818f, 1126.1295f, 238.3041f, 26.565052f, 57.0f, 0.0f };
/* Axis-aligned along +y at P's origin height: the ray grazes the bottom face. */
static const float grazing[6] = { -1237.229f, 1061.392f, 64.304f, 0.0f, 90.0f, 0.0f };
/* M115: P's module 0 is translated (1152, -2944, 0) without rotation, and the
 * daemon framed the module-local origin as a world point. */
static const float m115_translation[3] = { 1152.0f, -2944.0f, 0.0f };
static const float m115_view[6] = { -1400.6212f, 1109.7913f, 226.3041f, 26.565052f, 57.0f, 0.0f };
static const float p_root_center[3] = { -85.229f, -1582.608f, 76.304f };

static void put_box(int id, const float bounds[6], const float origin[3], const float axis[9])
{
    memcpy(body_f(id, BODY_BOUNDS_OFF), bounds, 6 * sizeof(float));
    memcpy(body_f(id, BODY_ORIGIN_OFF), origin, 3 * sizeof(float));
    memcpy(body_f(id, BODY_AXIS_OFF), axis, 9 * sizeof(float));
}

static sh_prefab_anchor_box make_box(const float bounds[6], const float origin[3], const float axis[9])
{
    sh_prefab_anchor_box box;
    memcpy(box.bounds, bounds, sizeof box.bounds);
    memcpy(box.origin, origin, sizeof box.origin);
    memcpy(box.axis, axis, sizeof box.axis);
    return box;
}

static void forward(const float v[6], float dir[3])
{
    dir[0] = cosf(v[3] * 0.017453292f) * cosf(v[4] * 0.017453292f);
    dir[1] = cosf(v[3] * 0.017453292f) * sinf(v[4] * 0.017453292f);
    dir[2] = -sinf(v[3] * 0.017453292f);
}

/* The native frame for a view: whether the line met the box, and the midpoint. */
static int native_mid(const float bounds[6], const float origin[3], const float axis[9],
                      const float o[3], const float angles_view[6], float *mid)
{
    float local[3], center[3], extent[3], dir[3], t0, t1;
    int met;
    for (int i = 0; i < 3; i++) { local[i] = (bounds[i] + bounds[i + 3]) * 0.5f; extent[i] = bounds[i + 3] - local[i]; }
    for (int i = 0; i < 3; i++) center[i] = origin[i] + local[0] * axis[i] + local[1] * axis[3 + i] + local[2] * axis[6 + i];
    forward(angles_view, dir);
    met = pa_line(center, axis, extent, o, dir, &t0, &t1);
    *mid = (t0 + t1) * 0.5f;
    return met;
}

static int near3(const float *a, const float *b, float tolerance)
{
    for (int i = 0; i < 3; i++) if (!(fabsf(a[i] - b[i]) <= tolerance)) return 0;
    return 1;
}

static int same9(const float *a, const float *b)
{
    for (int i = 0; i < 9; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Put P's module 0 at a translation and rotation. */
static void p_in_module(const float translation[3], int rotation)
{
    modules_of[69] = 0;
    memcpy(modules + MODULE_TRANSLATION_OFF, translation, 3 * sizeof(float));
    memcpy(modules + MODULE_ROTATION_OFF, &rotation, sizeof rotation);
}

static void select_ids(const int *list, int count)
{
    memcpy(ids, list, (size_t)count * sizeof(int));
    *(int **)(selection + SEL_IDS_OFF) = ids;
    *(int *)(selection + SEL_COUNT_OFF) = count;
}

static void use_map(unsigned char *map)
{
    *(void **)(editor + ED_MAP_OFF) = map;
    *(void ***)(map + MAP_ENTS_OFF) = slots;
    *(int *)(map + MAP_ENTCOUNT_OFF) = 100;
    *(int **)(map + MAP_ENT_MODULE_OFF) = modules_of;
    *(void **)(map + MAP_MODULES_OFF) = modules;
    *(int *)(map + MAP_MODULE_COUNT_OFF) = 1;
}

static void reset(void)
{
    static const int component[] = { 61, 68, 69, 70 };
    static const float camera[SH_PREFAB_ANCHOR_VIEW] = { 1200.0f, -340.0f, 96.0f, 12.5f, 271.0f, 0.0f };
    memset(editor, 0, sizeof editor);
    memset(selection, 0, sizeof selection);
    memset(bodies, 0, sizeof bodies);
    memset(decl, 0, sizeof decl); memset(decl_fixed, 0, sizeof decl_fixed);
    memset(modules, 0, sizeof modules);
    for (int i = 0; i < 100; i++) modules_of[i] = 1;
    decl_fixed[DECL_FLAGS_OFF] = DECL_FIXED_SIZE;
    for (int i = 0; i < 100; i++) { slots[i] = bodies[i]; *(const void **)(bodies[i] + SLOT_DECL_OFF) = decl; }
    put_box(69, p_bounds, p_origin, identity);
    put_box(70, p_bounds, p_origin, identity);
    *(void **)(editor + ED_SEL_OFF) = selection;
    use_map(map_a);
    select_ids(component, 4);
    memcpy(view(), camera, sizeof camera);
    *hover() = -1;
    sh_prefab_anchor_reset(&g_anchor);
}

/* Hover an entity for one frame, then lose the hover to the overlay. */
static void hover_then_leave(int id)
{
    *hover() = id; sh_prefab_anchor_frame(editor);
    *hover() = -1; sh_prefab_anchor_frame(editor);
}

static int install(sh_prefab_anchor_hold *hold)
{
    return sh_prefab_anchor_install(editor, hold);
}

int main(void)
{
    sh_prefab_anchor_hold hold;

    /* The M113 route: P hovered, its component selected, then the overlay took focus. */
    reset();
    hover_then_leave(69);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED && hold.anchor == 69 && hold.previous == -1);
    CHECK(*hover() == 69 && hold.selection == selection);
    sh_prefab_anchor_restore(&hold);
    CHECK(*hover() == -1);
    /* The record survives an export; a second create anchors the same way. */
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED && hold.anchor == 69);
    sh_prefab_anchor_restore(&hold);

    /* A native hover is left to native admission, selected or not. */
    *hover() = 70;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE && *hover() == 70 && !hold.selection);
    *hover() = 5;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE && *hover() == 5);
    *hover() = -1;

    /* The last selected entity hovered wins, and an unselected hover does not replace it. */
    reset();
    hover_then_leave(61);
    view()[4] = 275.0f;
    *hover() = 70; sh_prefab_anchor_frame(editor);
    *hover() = 12; sh_prefab_anchor_frame(editor);
    *hover() = -1; sh_prefab_anchor_frame(editor);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED && hold.anchor == 70);
    sh_prefab_anchor_restore(&hold);
    /* Looking away after the last selected hover leaves nothing under the view. */
    view()[4] = 300.0f;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);

    /* Hovering an entity before it is selected records it once selection follows. */
    reset();
    { static const int none[] = { 0 }; select_ids(none, 0); }
    *hover() = 69; sh_prefab_anchor_frame(editor);
    { static const int component[] = { 61, 68, 69, 70 }; select_ids(component, 4); }
    sh_prefab_anchor_frame(editor);
    *hover() = -1; sh_prefab_anchor_frame(editor);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED && hold.anchor == 69);
    sh_prefab_anchor_restore(&hold);

    /* The view must still meet the anchor; a settling camera within tolerance does. */
    reset();
    hover_then_leave(69);
    view()[0] += 0.25f; view()[4] += 0.1f;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    sh_prefab_anchor_restore(&hold);
    view()[0] += 2.0f;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);
    reset();
    hover_then_leave(69);
    view()[4] += 360.0f;  /* the same yaw */
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    sh_prefab_anchor_restore(&hold);
    view()[3] = NAN;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);

    /* Stale selection: the anchor was deselected, deleted, or belongs to another map. */
    reset();
    hover_then_leave(69);
    { static const int rest[] = { 61, 68, 70 }; select_ids(rest, 3); }
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);
    reset();
    hover_then_leave(69);
    *(const void **)(bodies[69] + SLOT_DECL_OFF) = NULL;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);
    reset();
    hover_then_leave(69);
    slots[69] = NULL;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    reset();
    hover_then_leave(69);
    *(int *)(map_a + MAP_ENTCOUNT_OFF) = 50;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    reset();
    hover_then_leave(69);
    use_map(map_b);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    sh_prefab_anchor_frame(editor);  /* a frame on the other map forgets the record */
    use_map(map_a);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    reset();
    hover_then_leave(69);
    *(void **)(editor + ED_MAP_OFF) = NULL; sh_prefab_anchor_frame(editor);
    use_map(map_a);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);

    /* Nothing selected, nothing recorded, or no usable selection object. */
    reset();
    hover_then_leave(69);
    { static const int none[] = { 0 }; select_ids(none, 0); }
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_EMPTY);
    reset();
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);
    reset();
    hover_then_leave(69);
    *(int **)(selection + SEL_IDS_OFF) = NULL;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    *(int **)(selection + SEL_IDS_OFF) = (int *)(uintptr_t)0x10;  /* faults on read */
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE && *hover() == -1);
    sh_prefab_anchor_frame(editor);
    reset();
    hover_then_leave(69);
    *(int *)(selection + SEL_COUNT_OFF) = LIST_LIMIT + 1;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NONE);
    CHECK(sh_prefab_anchor_install(NULL, &hold) == SH_PREFAB_ANCHOR_NONE);

    /* Restore removes only the installed hover. */
    reset();
    hover_then_leave(69);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    *hover() = 3;
    sh_prefab_anchor_restore(&hold);
    CHECK(*hover() == 3);
    sh_prefab_anchor_restore(NULL);

    /* The native frame: the framed view meets P near its center, the drifted
     * view misses it and keeps the x slab's bounds, the grazing view clips to
     * nothing on the bottom face. */
    {
        float mid = 0.0f;
        CHECK(native_mid(p_bounds, p_origin, identity, framed, framed, &mid) && fabsf(mid - 338.5f) < 0.5f);
        CHECK(!native_mid(p_bounds, p_origin, identity, drifted, drifted, &mid) && fabsf(mid - 2700.23f) < 0.5f);
    }

    /* The equivalent origin: same direction through the box center, depth s. */
    {
        sh_prefab_anchor_box box = make_box(p_bounds, p_origin, identity);
        float origin[3], depth = 0.0f, dir[3], at[3], mid = 0.0f, moved[6];
        CHECK(sh_prefab_anchor_equivalent(&box, framed, origin, &depth) == SH_PREFAB_ANCHOR_RAY_MEETS);
        /* P behind the drifted view: refused rather than framed at 2700. */
        CHECK(sh_prefab_anchor_equivalent(&box, drifted, origin, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        CHECK(!native_mid(p_bounds, p_origin, identity, beside, beside, &mid));
        CHECK(sh_prefab_anchor_equivalent(&box, beside, origin, &depth) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT);
        forward(beside, dir);
        for (int i = 0; i < 3; i++) at[i] = origin[i] + dir[i] * depth;
        CHECK(near3(at, p_center, 0.01f));
        CHECK(fabsf(depth - ((p_center[0] - beside[0]) * dir[0] + (p_center[1] - beside[1]) * dir[1] +
                             (p_center[2] - beside[2]) * dir[2])) < 0.01f);
        memcpy(moved, origin, sizeof origin); memcpy(moved + 3, beside + 3, 3 * sizeof(float));
        CHECK(native_mid(p_bounds, p_origin, identity, moved, moved, &mid) && fabsf(mid - depth) < 0.05f);
        /* The axis-aligned grazing ray. */
        CHECK(sh_prefab_anchor_equivalent(&box, grazing, origin, &depth) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT);
        CHECK(fabsf(depth - 300.0f) < 0.01f);
        forward(grazing, dir);
        for (int i = 0; i < 3; i++) at[i] = origin[i] + dir[i] * depth;
        CHECK(near3(at, p_center, 0.01f));
        memcpy(moved, origin, sizeof origin); memcpy(moved + 3, grazing + 3, 3 * sizeof(float));
        CHECK(native_mid(p_bounds, p_origin, identity, moved, moved, &mid) && fabsf(mid - 300.0f) < 0.05f);
    }

    /* A rotated box: a ray through it stands, a ray beside it is moved onto its center. */
    {
        const float c = cosf(30.0f * 0.017453292f), sn = sinf(30.0f * 0.017453292f);
        const float axis[9] = { c, sn, 0, -sn, c, 0, 0, 0, 1 };
        const float bounds[6] = { -10, -5, 0, 10, 5, 20 }, origin[3] = { 100, 50, 0 };
        const float through[6] = { -200, 50, 10, 0, 0, 0 }, beside[6] = { -200, 80, 10, 0, 0, 0 };
        sh_prefab_anchor_box box = make_box(bounds, origin, axis);
        float moved[3], depth = 0.0f, at[3], dir[3], center[3] = { 100, 50, 10 };
        CHECK(sh_prefab_anchor_equivalent(&box, through, moved, &depth) == SH_PREFAB_ANCHOR_RAY_MEETS);
        CHECK(sh_prefab_anchor_equivalent(&box, beside, moved, &depth) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT);
        forward(beside, dir);
        for (int i = 0; i < 3; i++) at[i] = moved[i] + dir[i] * depth;
        CHECK(near3(at, center, 0.01f) && fabsf(depth - 300.0f) < 0.01f);
    }

    /* Behind the view, not finite, or inverted bounds: no frame. */
    {
        const float behind_bounds[6] = { -8, -8, 0, 8, 8, 16 }, behind_origin[3] = { -500, 40, 0 };
        const float ahead[6] = { 0, 0, 8, 0, 0, 0 };
        const float inverted[6] = { 3, 12, 24, -3, -12, 0 };
        sh_prefab_anchor_box box = make_box(behind_bounds, behind_origin, identity);
        float moved[3], depth = 0.0f, bad[6];
        CHECK(sh_prefab_anchor_equivalent(&box, ahead, moved, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        box = make_box(p_bounds, p_origin, identity);
        memcpy(bad, drifted, sizeof bad); bad[4] = NAN;
        CHECK(sh_prefab_anchor_equivalent(&box, bad, moved, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        box.bounds[5] = INFINITY;
        CHECK(sh_prefab_anchor_equivalent(&box, drifted, moved, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        box = make_box(inverted, p_origin, identity);
        CHECK(sh_prefab_anchor_equivalent(&box, drifted, moved, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        CHECK(sh_prefab_anchor_equivalent(NULL, drifted, moved, &depth) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    }

    /* Engine side, the M114 route: the recorded anchor is installed and a view
     * passing beside it is moved for populate, then both are put back. */
    reset();
    memcpy(view(), beside, sizeof beside);
    hover_then_leave(69);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT && hold.entity == 69);
    CHECK(sh_prefab_anchor_frame_matches(hold.depth, hold.expect));
    {
        float dir[3], at[3];
        forward(beside, dir);
        for (int i = 0; i < 3; i++) at[i] = view()[i] + dir[i] * hold.depth;
        CHECK(near3(at, p_center, 0.01f) && !memcmp(view() + 3, beside + 3, 3 * sizeof(float)));
    }
    sh_prefab_anchor_restore(&hold);
    CHECK(!memcmp(view(), beside, sizeof beside) && *hover() == -1);
    /* The drifted view leaves P behind: no frame, nothing moved. */
    reset();
    memcpy(view(), drifted, sizeof drifted);
    hover_then_leave(69);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);
    CHECK(!memcmp(view(), drifted, sizeof drifted));
    sh_prefab_anchor_restore(&hold);
    CHECK(*hover() == -1);

    /* A view that meets the box is left alone. */
    reset();
    memcpy(view(), framed, sizeof framed);
    hover_then_leave(69);
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_INSTALLED);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_MEETS && !hold.view && hold.entity == 69);
    CHECK(fabsf(hold.expect - 338.5f) < 0.5f);
    CHECK(!memcmp(view(), framed, sizeof framed));
    sh_prefab_anchor_restore(&hold);
    CHECK(*hover() == -1);

    /* A native hover gets the same rule; an unselected one is left to populate. */
    reset();
    memcpy(view(), grazing, sizeof grazing);
    *hover() = 69;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT && hold.entity == 69);
    CHECK(fabsf(view()[0] - p_center[0]) < 0.01f && fabsf(view()[2] - p_center[2]) < 0.01f);
    sh_prefab_anchor_restore(&hold);
    CHECK(!memcmp(view(), grazing, sizeof grazing) && *hover() == 69);
    *hover() = 5;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_MEETS && hold.entity == -1 && !hold.view);
    CHECK(hold.expect != hold.expect);  /* populate refuses; nothing predicted */

    /* A fixed-size declaration centers the box on the origin, so the grazing
     * ray now passes through it. */
    reset();
    memcpy(view(), grazing, sizeof grazing);
    *(const void **)(bodies[69] + SLOT_DECL_OFF) = decl_fixed;
    *body_f(69, BODY_SIZE_X_OFF) = 6.0f; *body_f(69, BODY_SIZE_Y_OFF) = 24.0f; *body_f(69, BODY_SIZE_Z_OFF) = 24.0f;
    *hover() = 69;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_MEETS && !hold.view);

    /* Restore keeps an origin the engine changed; a box behind the view or an
     * unreadable declaration gives no frame and leaves the view alone. */
    reset();
    memcpy(view(), beside, sizeof beside);
    *hover() = 69;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT);
    view()[0] += 5.0f;
    {
        float kept = view()[0];
        sh_prefab_anchor_restore(&hold);
        CHECK(view()[0] == kept);
    }
    reset();
    {
        const float behind[6] = { -1207.229f, 1661.392f, 76.304f, 0.0f, 90.0f, 0.0f };
        memcpy(view(), behind, sizeof behind);
        *hover() = 69;
        CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);
        CHECK(!memcmp(view(), behind, sizeof behind));
        memcpy(view(), beside, sizeof beside);
        *(const void **)(bodies[69] + SLOT_DECL_OFF) = (const void *)(uintptr_t)0x10;  /* faults on read */
        CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);
        CHECK(sh_prefab_anchor_aim(NULL, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
        CHECK(sh_prefab_anchor_aim(editor, NULL) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    }

    /* Module rotations (VK 0x511460) and the root-frame move populate makes
     * before the grab frame: origin * R + t, each axis row * R, and a mirror
     * negates one row by declaration flags. */
    {
        static const float r1[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
        static const float r2[9] = { -1, 0, 0, 0, -1, 0, 0, 0, 1 };
        static const float r4[9] = { -1, 0, 0, 0, 1, 0, 0, 0, 1 };
        static const float t[3] = { 10, 20, 30 }, at[3] = { 1, 2, 3 }, moved[3] = { 8, 21, 33 };
        float m[9], o[3], a[9];
        sh_prefab_anchor_rotation(0, m); CHECK(same9(m, identity));
        sh_prefab_anchor_rotation(1, m); CHECK(same9(m, r1));
        sh_prefab_anchor_rotation(4, m); CHECK(same9(m, r4));
        sh_prefab_anchor_rotation(8, m); CHECK(same9(m, identity));
        sh_prefab_anchor_rotation(-1, m); CHECK(same9(m, identity));
        sh_prefab_anchor_root_frame(at, identity, t, 1, 0, 0, o, a);
        CHECK(near3(o, moved, 1e-4f) && same9(a, r1));
        sh_prefab_anchor_root_frame(at, identity, t, 4, 0, 0, o, a);
        CHECK(same9(a, r4));
        sh_prefab_anchor_root_frame(at, identity, t, 4, 0, DECL_FIXED_SIZE, o, a);
        CHECK(same9(a, identity));
        sh_prefab_anchor_root_frame(at, identity, t, 4, DECL_SECOND_ROW_MIRROR, 0, o, a);
        CHECK(same9(a, r2));
        sh_prefab_anchor_root_frame(at, identity, t, 4, DECL_SECOND_ROW_MIRROR, DECL_FIXED_SIZE, o, a);
        CHECK(same9(a, identity));
        sh_prefab_anchor_root_frame(at, identity, t, 2, DECL_SECOND_ROW_MIRROR, DECL_FIXED_SIZE, o, a);
        CHECK(same9(a, r2));
    }

    /* M115: the module-local framing meets P's module-local box near 335, but
     * populate tests the root-frame box, which the line misses: the clipped x
     * slab gives the live 2700.2336. */
    {
        float mid = 0.0f, root_origin[3], root_axis[9];
        CHECK(native_mid(p_bounds, p_origin, identity, m115_view, m115_view, &mid) && fabsf(mid - 335.41f) < 0.05f);
        sh_prefab_anchor_root_frame(p_origin, identity, m115_translation, 0, 0, 0, root_origin, root_axis);
        CHECK(!native_mid(p_bounds, root_origin, root_axis, m115_view, m115_view, &mid) &&
              fabsf(mid - 2700.2336f) < 0.05f);
        {
            sh_prefab_anchor_box box = make_box(p_bounds, root_origin, root_axis);
            CHECK(sh_prefab_anchor_native_frame(&box, m115_view, m115_view, &mid) == 0 &&
                  fabsf(mid - 2700.2336f) < 0.05f);
        }
    }
    /* Engine side: that view now finds P behind it and gives no frame. */
    reset();
    p_in_module(m115_translation, 0);
    memcpy(view(), m115_view, sizeof m115_view);
    *hover() = 69;
    CHECK(install(&hold) == SH_PREFAB_ANCHOR_NATIVE);
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);
    CHECK(!memcmp(view(), m115_view, sizeof m115_view));
    /* Framed on the world position, the same view meets P and predicts 335.41. */
    {
        float world[6], dir[3], at[3];
        for (int i = 0; i < 3; i++) world[i] = m115_view[i] + m115_translation[i];
        memcpy(world + 3, m115_view + 3, 3 * sizeof(float));
        memcpy(view(), world, sizeof world);
        CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_MEETS && !hold.view);
        CHECK(fabsf(hold.expect - 335.41f) < 0.05f && sh_prefab_anchor_frame_matches(335.41f, hold.expect));
        CHECK(!sh_prefab_anchor_frame_matches(2700.2336f, hold.expect));
        /* Beside it, the moved origin passes through the root-frame center. */
        for (int i = 0; i < 3; i++) world[i] = beside[i] + m115_translation[i];
        memcpy(world + 3, beside + 3, 3 * sizeof(float));
        memcpy(view(), world, sizeof world);
        CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_EQUIVALENT);
        CHECK(sh_prefab_anchor_frame_matches(hold.depth, hold.expect));
        forward(world, dir);
        for (int i = 0; i < 3; i++) at[i] = view()[i] + dir[i] * hold.depth;
        CHECK(near3(at, p_root_center, 0.02f));
        sh_prefab_anchor_restore(&hold);
        CHECK(!memcmp(view(), world, sizeof world));
    }
    /* A rotated module turns the box with it. */
    reset();
    p_in_module(m115_translation, 1);
    {
        const float center[3] = { -209.392f, -4181.229f, 76.304f };
        float v[6] = { 0, 0, 0, 10.0f, 30.0f, 0.0f }, dir[3];
        forward(v, dir);
        for (int i = 0; i < 3; i++) v[i] = center[i] - dir[i] * 300.0f;
        memcpy(view(), v, sizeof v);
        *hover() = 69;
        CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_MEETS && fabsf(hold.expect - 300.0f) < 0.05f);
    }
    /* An index past the root, or unreadable module data: no frame. */
    reset();
    memcpy(view(), framed, sizeof framed);
    *hover() = 69;
    modules_of[69] = 2;
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);
    modules_of[69] = -1;
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    modules_of[69] = 0;
    *(void **)(map_a + MAP_MODULES_OFF) = NULL;
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    *(int *)(map_a + MAP_MODULE_COUNT_OFF) = MODULE_LIMIT + 1;
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    reset();
    memcpy(view(), framed, sizeof framed);
    *hover() = 69;
    *(int **)(map_a + MAP_ENT_MODULE_OFF) = NULL;
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE);
    *(int **)(map_a + MAP_ENT_MODULE_OFF) = (int *)(uintptr_t)0x10;  /* faults on read */
    CHECK(sh_prefab_anchor_aim(editor, &hold) == SH_PREFAB_ANCHOR_RAY_UNUSABLE && !hold.view);

    /* The populated distance must be the predicted one. */
    CHECK(sh_prefab_anchor_frame_matches(2700.2336f, 2700.2336f) && sh_prefab_anchor_frame_matches(10.0f, 10.04f));
    CHECK(!sh_prefab_anchor_frame_matches(10.0f, 10.06f) && sh_prefab_anchor_frame_matches(3000.0f, 3002.9f));
    CHECK(!sh_prefab_anchor_frame_matches(3000.0f, 3003.5f) && !sh_prefab_anchor_frame_matches(NAN, 10.0f) &&
          !sh_prefab_anchor_frame_matches(10.0f, NAN));

    /* A ray that missed the box leaves a 1e30 bound in the midpoint. */
    CHECK(sh_prefab_anchor_distance_ok(194.06f) && sh_prefab_anchor_distance_ok(0.0f) &&
          sh_prefab_anchor_distance_ok(-12.0f));
    CHECK(!sh_prefab_anchor_distance_ok(5.0e29f) && !sh_prefab_anchor_distance_ok(-5.0e29f) &&
          !sh_prefab_anchor_distance_ok(INFINITY) && !sh_prefab_anchor_distance_ok(NAN));

    if (failed) { printf("prefab_anchor_test: %d failure(s)\n", failed); return 1; }
    puts("prefab_anchor_test OK");
    return 0;
}
