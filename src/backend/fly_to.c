/* fly_to.c -- "fly to entity": put the editor in Object Mode and aim the camera at
 * an entity so the centre-screen cursor rests on it.
 *
 * The request is set from any thread (apply kind 4) and serviced on the game
 * thread by the EditorFrame detour, one phase per frame:
 *   SWITCH  in Blueprint, reproduce TAB (three field writes and
 *           GridSetEditorState(2)); nothing to do when already in Object Mode.
 *   PLACE   compute the entity's world bounds centre, put the eye a short
 *           distance away looking slightly down, write the six camera floats.
 *   SETTLE  the walk camera sinks to the floor under gravity, so re-aim the
 *           two angle floats each frame until the eye stops moving or the
 *           budget runs out. Position and angle writes persist: the mover
 *           only adds input deltas.
 * Every engine read is SEH-guarded and a fault retires the module. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "fly_to.h"
#include "fly_to_math.h"
#include "signatures.h"
#include "backend_log.h"
#include "snapmap_plus_iface.h"

/* editor */
#define ED_STATE_OFF        0x23618   /* int: editor state, 1 Blueprint (ModuleMode), 2 Object (EntityMode) */
#define ED_CAMERA_MODE_OFF  0x3d8     /* int: camera mode, 2 = the FPS view */
#define ED_CAMERA_OFF       0x170     /* idSnapCamera: eye +0, pitch +0xc, yaw +0x10, roll +0x14 (degrees), fly +0x18 */
#define ED_MAP_OFF          0x204c8   /* loaded map object */
#define ED_SEL_OFF          0x204d0   /* selection object */
#define ED_MODULE_MODE_OFF  0x212b8   /* ModuleMode state object */
#define ED_ENTITY_MODE_OFF  0x22330   /* EntityMode state object */
#define MM_IDLE_OFF         0x2c      /* ModuleMode: 1 while its idle substate runs */
#define MM_ENTER_FLAG_OFF   0x1070    /* written 1 by TAB before the state change */
#define SEL_TAB_CLEAR_OFF   0x70      /* cleared by TAB and by EntityMode enter */
#define EM_SUBSTATE_OFF     0x1ac     /* EntityMode: 1 idle, 2 the other quiet substate */
/* map */
#define MAP_ENTS_OFF        0x6a0     /* entity pointer array */
#define MAP_ENT_COUNT_OFF   0x6a8
#define MAP_OWNERS_OFF      0x6f0     /* int per entity: its module index (== count for the root) */
#define MAP_MODULES_OFF     0x750     /* module records, stride 0x98 */
#define MAP_MODULE_COUNT_OFF 0x758
#define MODULE_STRIDE       0x98
#define MODULE_TRANSLATION  0x0c
#define MODULE_ROTATION     0x18
/* entity */
#define ENT_ORIGIN_OFF      0x290
#define ENT_AXIS_OFF        0x29c
#define ENT_BMIN_OFF        0x2cc
#define ENT_BMAX_OFF        0x2d8
#define ENT_COUNT_CAP       65536
#define MODULE_COUNT_CAP    8192

#define FLY_SETTLE_FRAMES   60
#define FLY_STILL_FRAMES    3

typedef void (*set_state_fn)(void *editor, int state);

enum { FLY_IDLE = 0, FLY_SWITCH, FLY_VERIFY, FLY_PLACE, FLY_SETTLE };

static set_state_fn   g_set_state = NULL;
static sh_iface      *g_iface     = NULL;
static volatile LONG  g_faulted   = 0;
static volatile LONG  g_phase     = FLY_IDLE;
static volatile LONG  g_id        = -1;
static sh_fly_vec3    g_centre;
static float          g_last_z    = 0.0f;
static int            g_still     = 0;
static int            g_frames    = 0;

static void fly_logf(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void fly_toast(const char *text)
{
    sh_iface *iface = g_iface;
    if (iface && iface->vtbl && iface->vtbl->toast) iface->vtbl->toast(iface, "Fly to entity", text);
    fly_logf("FLY: %s", text);
}

static void fly_done(void) { InterlockedExchange(&g_phase, FLY_IDLE); }

static void fly_fail(const char *why)
{
    fly_toast(why);
    fly_done();
}

/* Read the entity's world bounds centre and radius. 0 when the map or id is unusable. */
static int fly_read_target(unsigned char *ed, int id, sh_fly_vec3 *centre, float *radius)
{
    unsigned char *map, **ents, *ent, *records;
    int *owners, count, modules, module, rotation;
    float origin[3], axis[9], bmin[3], bmax[3], translation[3];
    sh_fly_vec3 local;
    __try {
        map = *(unsigned char **)(ed + ED_MAP_OFF);
        if (!map) return 0;
        ents = *(unsigned char ***)(map + MAP_ENTS_OFF);
        count = *(int *)(map + MAP_ENT_COUNT_OFF);
        if (!ents || id < 0 || id >= count || count > ENT_COUNT_CAP) return 0;
        ent = ents[id];
        if (!ent) return 0;
        memcpy(origin, ent + ENT_ORIGIN_OFF, sizeof origin);
        memcpy(axis, ent + ENT_AXIS_OFF, sizeof axis);
        memcpy(bmin, ent + ENT_BMIN_OFF, sizeof bmin);
        memcpy(bmax, ent + ENT_BMAX_OFF, sizeof bmax);
        owners = *(int **)(map + MAP_OWNERS_OFF);
        modules = *(int *)(map + MAP_MODULE_COUNT_OFF);
        if (!owners || modules < 0 || modules > MODULE_COUNT_CAP) return 0;
        module = owners[id];
        if (module < 0 || module > modules) return 0;
        local = sh_fly_local_centre(origin, axis, bmin, bmax);
        *radius = sh_fly_radius(bmin, bmax);
        if (module == modules) { *centre = local; }
        else {
            records = *(unsigned char **)(map + MAP_MODULES_OFF);
            if (!records) return 0;
            memcpy(translation, records + (size_t)module * MODULE_STRIDE + MODULE_TRANSLATION, sizeof translation);
            rotation = *(int *)(records + (size_t)module * MODULE_STRIDE + MODULE_ROTATION);
            if (rotation < 0 || rotation > 7) return 0;
            *centre = sh_fly_module_to_world(local, rotation, translation);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        return 0;
    }
    if (!(centre->x == centre->x) || !(centre->y == centre->y) || !(centre->z == centre->z)) return 0;
    return 1;
}

int sh_fly_to_install(const sig_result *results, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (results[i].name && !strcmp(results[i].name, "GridSetEditorState") && results[i].status == SIG_OK) {
            g_set_state = (set_state_fn)results[i].addr;
            return 1;
        }
    }
    backend_log("FLY: GridSetEditorState unresolved; fly to entity works only from Object Mode");
    return 0;
}

int sh_fly_to_request(int id, sh_iface *iface)
{
    if (InterlockedCompareExchange(&g_faulted, 0, 0)) return 0;
    if (id < 0) return 0;
    if (iface) g_iface = iface;
    InterlockedExchange(&g_id, id);
    InterlockedExchange(&g_phase, FLY_SWITCH);
    return 1;
}

int sh_fly_to_active(void) { return InterlockedCompareExchange(&g_phase, 0, 0) != FLY_IDLE; }

void sh_fly_to_tick(void *editor)
{
    unsigned char *ed = (unsigned char *)editor;
    LONG phase = InterlockedCompareExchange(&g_phase, 0, 0);
    int id, state, cam_mode;
    float *cam;
    if (phase == FLY_IDLE || !ed) return;
    if (InterlockedCompareExchange(&g_faulted, 0, 0)) { fly_done(); return; }
    id = (int)InterlockedCompareExchange(&g_id, 0, 0);
    __try {
        state = *(int *)(ed + ED_STATE_OFF);
        cam_mode = *(int *)(ed + ED_CAMERA_MODE_OFF);
        cam = (float *)(ed + ED_CAMERA_OFF);
        if (phase == FLY_SWITCH) {
            float radius;
            if (!*(unsigned char **)(ed + ED_MAP_OFF)) { fly_fail("No map is open."); return; }
            if (!fly_read_target(ed, id, &g_centre, &radius)) { fly_fail("That entity could not be located."); return; }
            if (state == 2) {
                int sub = *(int *)(ed + ED_ENTITY_MODE_OFF + EM_SUBSTATE_OFF);
                if (sub != 1 && sub != 2) { fly_fail("Finish what you are holding or placing first."); return; }
                InterlockedExchange(&g_phase, FLY_PLACE);
                /* fall through to PLACE this frame */
            } else if (state == 1) {
                unsigned char *sel = *(unsigned char **)(ed + ED_SEL_OFF);
                if (!g_set_state) { fly_fail("Press Tab to enter Object Mode first."); return; }
                if (*(int *)(ed + ED_MODULE_MODE_OFF + MM_IDLE_OFF) != 1) { fly_fail("Finish the Blueprint action first."); return; }
                if (sel) *(int *)(sel + SEL_TAB_CLEAR_OFF) = 0;
                *(int *)(ed + ED_MODULE_MODE_OFF + MM_IDLE_OFF) = 1;
                *(int *)(ed + ED_MODULE_MODE_OFF + MM_ENTER_FLAG_OFF) = 1;
                g_set_state(ed, 2);
                InterlockedExchange(&g_phase, FLY_VERIFY);
                return;
            } else {
                fly_fail("Close the open panel first.");
                return;
            }
        }
        if (phase == FLY_VERIFY) {
            if (state != 2 || cam_mode != 2) { fly_fail("Object Mode did not open."); return; }
            InterlockedExchange(&g_phase, FLY_PLACE);
        }
        phase = InterlockedCompareExchange(&g_phase, 0, 0);
        if (phase == FLY_PLACE) {
            float radius = 0.0f;
            sh_fly_vec3 eye;
            float yaw = cam[4];
            if (state != 2) { fly_fail("Object Mode did not open."); return; }
            if (!fly_read_target(ed, id, &g_centre, &radius)) { fly_fail("That entity could not be located."); return; }
            eye = sh_fly_eye(g_centre, sh_fly_distance(radius), SH_FLY_PITCH_DEG, yaw);
            cam[0] = eye.x; cam[1] = eye.y; cam[2] = eye.z;
            cam[3] = SH_FLY_PITCH_DEG; cam[4] = yaw; cam[5] = 0.0f;
            g_last_z = eye.z; g_still = 0; g_frames = 0;
            fly_logf("FLY: entity %d centre (%.1f %.1f %.1f) r=%.1f eye (%.1f %.1f %.1f) yaw %.1f",
                        id, g_centre.x, g_centre.y, g_centre.z, radius, eye.x, eye.y, eye.z, yaw);
            InterlockedExchange(&g_phase, FLY_SETTLE);
            return;
        }
        if (phase == FLY_SETTLE) {
            float pitch, yaw;
            sh_fly_vec3 eye = sh_fly_v3(cam[0], cam[1], cam[2]);
            if (state != 2) { fly_done(); return; }
            sh_fly_aim(eye, g_centre, &pitch, &yaw);
            cam[3] = pitch; cam[4] = yaw;
            g_frames++;
            if (eye.z == g_last_z) g_still++; else { g_still = 0; g_last_z = eye.z; }
            if (g_still >= FLY_STILL_FRAMES || g_frames >= FLY_SETTLE_FRAMES) {
                fly_logf("FLY: settled after %d frames at (%.1f %.1f %.1f) pitch %.1f yaw %.1f",
                            g_frames, eye.x, eye.y, eye.z, pitch, yaw);
                fly_done();
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        backend_log("FLY: faulted; fly to entity retired for this process");
        fly_done();
    }
}
