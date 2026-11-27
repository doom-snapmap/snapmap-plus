/* script_api_ai.c -- the AI calls of mod scripts (script_api_ai.h).
 *
 * The Lua side checks every argument and the caller's ownership first, then makes its engine calls through
 * the seam (sh_script_ai_engine) and never raises after one. The engine side (below SH_SCRIPT_PURE) fills
 * the seam on build 13954591, both renderers.
 *
 * Engine paths (VK / GL):
 *   commands      the catalogue events, via ProcessEventArgs (VK 0x140B47350 / GL 0x140B46940)
 *   park          IsRunningScript (VK 0x14042FBC0 / GL 0x14042EE60) detoured; release marks the slot
 *   anim_variable the anim web's set-variable-by-name (VK 0x14170A950 / GL 0x14170B3C0), animator at ai+0x222B8
 *   enemy         idEntityPtr at ai+0x25050, resolved by VK 0x1403935E0 / GL 0x140392E10
 *   awareness     GetAwareness(ai+0x29A88, entity) VK 0x1404A9E30 / GL 0x1404A85B0
 *   sight         the isEntityVisible event; can_see adds the last-seen time (VK 0x1404AA3E0) and a world trace
 *   move status   *(*(ai+0x23198)+0xD20), as Shared_MoveDone (VK 0x1403D7DF0) reads it; >= 5 is failed
 *   path          the move FSM's path query (VK 0x140515300 / GL 0x140514030): 0 or 1 found, 2 searching, >2 none
 *   trigger       the weapon's trigger word (weapon +0x8F8 + 4 * [+0x8D4], 0 = pulled)
 *   recoil        the float at ai+0x24EB0, multiplied into the recoil animator's weight (VK 0x1403D9300)
 *   weapon_ready  the weapon holder's next-pull time against the game clock (VK 0x1404439D0)
 *   aim_point     the set-focus-to-position of ae_selectAimFocusPoint (VK 0x14041BBA0 / GL 0x14041AD60)
 *   animation_fits  the node's animations (VK 0x141712C60) and root motion (VK 0x1415C5290), then traces
 *   path_points   the path query's computed path, read through a detour of the path length
 *                 (VK 0x14050EA60 / GL 0x14050D790)
 *   monster_type  the getMonsterType event (VK 0x14040D070)
 *   pain_string   the pain request's string (VK 0x1403F5440 / GL 0x1403F4920), swapped for a stub
 *   block_pain    a detour of the threat table's row check (VK 0x1406E08D0 / GL 0x1406DFD00)
 *   wound_damage_counts  a detour of idGoreComponent::DamageActivatesWound (VK 0x1408FC8E0 / GL 0x1408FB910) */
#include <windows.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/text.h"
#include "script_api_ai.h"
#include "script_runtime.h"
#include "script_api_common.h"
#include "script_api_combat.h"
#include "script_internal.h"
#include "../../third_party/lua/lauxlib.h"

#define AI_PI 3.14159265358979323846

const sh_ai_event_def SH_AI_EVENTS[SH_AI_EV_COUNT] = {
    { "ssaction_MoveToPoint",        "vifbv" },
    { "ssaction_Idle",               "ii" },
    { "ssaction_SetWalkState",       "i" },
    { "ssaction_SetPlayerFocus",     "ibb" },
    { "ssaction_ChangeAnimState",    "i1i" },
    { "ssaction_ChangeAnimStateVia", "i11i" },
    { "ssaction_ForceAnimState",     "i1ii" },
    { "setFocus",                    "ebbi" },
    { "clearAimFocus",               "" },
    { "clearLookFocus",              "" },
    { "setEnemy",                    "eb" },
    { "setPlayerEnemy",              "i" },
    { "isEntityVisible",             "e" },
    { "hasAwarenessOfEnemy",         "" },
    { "pullTriggerRight",            "" },
    { "releaseTriggerRight",         "" },
    { "equipInventoryItemByIndex",   "i" },
    { "setLaserState",               "bs" },
    { "ae_startLaserSweep",          "4s" },
    { "ae_setOverrideFocusTagName",  "4is" },
    { "ae_clearOverrideFocusTag",    "4i" },
    { "setScriptAbort",              "bb" },
    { "setScriptAbortFlags",         "bb" },
    { "ae_setLaserState",            "4bs" },
    { "getMonsterType",              "" },
    { "pullTriggerLeft",             "" },
    { "releaseTriggerLeft",          "" },
    { "setFireMode",                 "i" },
    { "ae_aIPullTrigger",            "4sb" },
    { "enableBodyRotation",          "b" },
    { "enableHeadTracking",          "b" },
    { "tempMemory_Remove",           "s" },
};

/* ------------------------------------------------------------------ pure */

const char *const SH_AI_ORDER_KEYS[4] = {
    NULL, "snapmap_forceFollowPath", "snapmap_forceFollowEntity", "snapmap_forceMoveToPosition",
};

int sh_ai_order_find(const char *const *keys, const uint32_t *values, int n, sh_ai_order *out)
{
    int i, k;
    if (!keys || !values || !out) return 0;
    memset(out, 0, sizeof *out);
    for (i = 0; i < n; ++i) {
        if (!keys[i]) continue;
        for (k = SH_AI_ORDER_PATH; k <= SH_AI_ORDER_POSITION; ++k) {
            if (strcmp(keys[i], SH_AI_ORDER_KEYS[k])) continue;
            if (k == SH_AI_ORDER_POSITION) {
                if (i + 2 >= n || !keys[i + 1] || !keys[i + 2] || strcmp(keys[i + 1], keys[i]) ||
                    strcmp(keys[i + 2], keys[i])) break;
                memcpy(out->pos, values + i, sizeof out->pos);
            } else out->spawn_id = values[i];
            out->kind = k;
            return 1;
        }
    }
    return 0;
}

int sh_ai_args_fit(const char *format, const sh_ai_arg *args, int n)
{
    int i;
    if (!format || n != (int)strlen(format) || n > 8 || (n && !args)) return 0;
    for (i = 0; i < n; i++) {
        char f = format[i], t = args[i].type;
        if (f != t) return 0;
        if ((f == 's' || f == '1') && !args[i].s) return 0;
        if (f == 'e' && !args[i].ent) return 0;
        if (f == '4' && args[i].ent) return 0;
        if (f != 'v' && f != 'i' && f != 'f' && f != 'b' && f != 's' && f != '1' && f != 'e' && f != '4') return 0;
    }
    return 1;
}

int sh_ai_event_format_ok(int ev, const char *registry_format)
{
    if (ev < 0 || ev >= SH_AI_EV_COUNT || !registry_format) return 0;
    return strcmp(SH_AI_EVENTS[ev].format, registry_format) == 0;
}

const char *const SH_AI_AIMPOINT_NAMES[13] = {
    "ORIGIN", "HEAD", "TORSO", "CENTER", "LEGS", "FEET", "LEFT_SHOULDER", "RIGHT_SHOULDER", "LEFT_FOOT", "RIGHT_FOOT",
    "BEST", "EYELEVEL", "LOOKAHEAD"
};

int sh_ai_order_interrupt(int kind)
{
    switch (kind) {
    case SH_AI_ORDER_CHANGE:    return SH_AI_INTR_BLEND_MULTI;   /* DOOM Eternal's command default */
    case SH_AI_ORDER_FORCE_VIA: return SH_AI_INTR_BLEND_MULTI;   /* ForceAndChangeState */
    default:                    return -1;                       /* ForceState's YES; the via's second leg */
    }
}

int sh_ai_force_via_next(unsigned flags, int asked, uint32_t frames_since_issue, uint32_t give_up_frames)
{
    if (flags & SH_SCRIPT_ANIM_DEST) return SH_AI_FORCE_VIA_DONE;
    if (flags & SH_SCRIPT_ANIM_VIA_IN) return asked ? SH_AI_FORCE_VIA_WAIT : SH_AI_FORCE_VIA_ASK_DEST;
    return frames_since_issue > give_up_frames ? SH_AI_FORCE_VIA_DONE : SH_AI_FORCE_VIA_WAIT;
}

int sh_ai_fsm_code_yields(int code)
{
    return code == 232 || code == 233 || code == 237 || code == 238 || code == 242;
}

int sh_ai_pain_take_next(int alive, int playing, int now_ms, int until_ms)
{
    return alive && playing == SH_COMBAT_PAIN_NONE && now_ms <= until_ms;
}

int sh_ai_request_pain_admits(int reaction, int current, const char **why)
{
    const char *w = NULL;
    if (reaction <= SH_COMBAT_PAIN_NONE || reaction >= SH_AI_PAIN_REACTIONS) w = "it is not a pain reaction";
    else if (reaction >= SH_AI_PAIN_DEATH) w = "a death is not a pain to ask for";
    else if (current >= reaction) w = "the demon already plays a pain at least as strong";
    if (why) *why = w;
    return w == NULL;
}

int sh_ai_fsm_state_find(const char *const *names, int n, const char *want)
{
    int i;
    if (!names || !want) return -1;
    for (i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], want)) return i;
    return -1;
}

static int av_only(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static void *vfn(void *self, size_t slot)
{
    __try {
        const uint8_t *vt;
        if (!self) return NULL;
        vt = *(const uint8_t * const *)self;
        return vt ? *(void * const *)(vt + slot) : NULL;
    } __except (av_only(GetExceptionCode())) { return NULL; }
}

#define FSM_STATES_MAX 256

static const char *state_name_of(const uint8_t *state, uint32_t data_offset, uint32_t name_offset)
{
    const uint8_t *data = state ? *(const uint8_t *const *)(state + data_offset) : NULL;
    return data ? *(const char *const *)(data + name_offset) : NULL;
}

static const uint8_t *named_fsm_state(const uint8_t *fsm, const char *name, const sh_ai_path_fields *fields)
{
    const uint8_t *const *states = *(const uint8_t *const *const *)(fsm + SH_AI_FSM_STATES);
    int i, count = *(const int *)(fsm + SH_AI_FSM_STATE_COUNT);
    if (!states || count < 0 || count > FSM_STATES_MAX) return NULL;
    for (i = 0; i < count; i++) {
        const char *actual = state_name_of(states[i], fields->data, fields->name);
        if (actual && !strcmp(actual, name)) return states[i];
    }
    return NULL;
}

static int state_is(const uint8_t *state, const char *name)
{
    typedef const char *const *(*type_fn)(const void *);
    type_fn get_type = (type_fn)vfn((void *)state, 0);
    const char *const *type = get_type ? get_type(state) : NULL;
    return type && *type && !strcmp(*type, name);
}

int sh_ai_fsm_has_native_path(const uint8_t *alert, const sh_ai_path_fields *fields)
{
    typedef const uint8_t *(*find_fn)(void *, const char *);
    const uint8_t *branch, *data, *linked, *child, *follow;
    const char *name;
    void *manager;
    find_fn find;
    if (!alert || !fields || !named_fsm_state(alert, "ac_snapmap_check", fields)) return 0;
    branch = named_fsm_state(alert, "behaviors_snapmap", fields);
    if (!state_is(branch, "idShared_ExecuteChildFSM")) return 0;
    data = *(const uint8_t *const *)(branch + fields->data);
    name = *(const char *const *)(data + fields->selector);
    if (!name || !*name) return 0;
    /* ExecuteChildFSM looks up its declared name on the parent manager before binding childFSM. */
    manager = *(void *const *)(alert + fields->manager);
    find = (find_fn)vfn(manager, SH_AI_FSM_FIND_CHILD_SLOT);
    child = find ? find(manager, name) : NULL;
    linked = *(const uint8_t *const *)(branch + fields->linked);
    if (!child || (linked && linked != child)) return 0;
    follow = named_fsm_state(child, "sm_force_followPath", fields);
    return state_is(follow, "idSnapMap_ForceFollowPath");
}

void sh_ai_fsm_move_init(unsigned char rec[SH_AI_FSM_MOVE_SIZE], const void *code_vtable, const void *target)
{
    int code = SH_AI_FSM_MOVE_IMMEDIATE;
    memset(rec, 0, SH_AI_FSM_MOVE_SIZE);
    memcpy(rec + SH_AI_FSM_MOVE_TARGET, &target, sizeof target);
    memcpy(rec + SH_AI_FSM_MOVE_CODE_VT, &code_vtable, sizeof code_vtable);
    memcpy(rec + SH_AI_FSM_MOVE_CODE, &code, sizeof code);
}

int sh_ai_park_holds(int alive, int pain_requested, int pain_reaction)
{
    return alive && !pain_requested && pain_reaction == SH_COMBAT_PAIN_NONE;
}

int sh_ai_park_drives(const sh_ai_park_slot *s, uint32_t id_now)
{
    return s && s->ai && id_now != SH_SCRIPT_SPAWN_ID_NONE && id_now == (uint32_t)s->spawn_id && !s->released;
}

void sh_ai_park_release(sh_ai_park_slot *s)
{
    InterlockedExchange(&s->released, 1);
    InterlockedExchange(&s->pain_until, 0);
}

void sh_ai_park_take(sh_ai_park_slot *s) { InterlockedExchange(&s->released, 0); }

void sh_ai_move_issue(sh_ai_move *m, uint32_t frame)
{
    m->state = SH_AI_MOVE_RUNNING;
    m->seen = 0;
    m->issued = frame;
}

int sh_ai_move_step(sh_ai_move *m, int status, uint32_t frame)
{
    if (m->state != SH_AI_MOVE_RUNNING) return m->state;
    if (status >= 5) { m->state = SH_AI_MOVE_FAILED; return m->state; }
    if (status > 0) { m->seen = 1; return m->state; }
    if (status == 0) {
        if (m->seen || frame - m->issued >= SH_AI_MOVE_SETTLE) m->state = SH_AI_MOVE_ARRIVED;
    }
    return m->state;
}

const char *sh_ai_awareness_name(int level)
{
    switch (level) {
    case SH_AI_AWARE_UNAWARE: return "unaware";
    case SH_AI_AWARE_LOST: return "lost";
    case SH_AI_AWARE_SUSPECTED: return "suspected";
    case SH_AI_AWARE_CONFIRMED: return "confirmed";
    default: return NULL;
    }
}

const char *sh_ai_respect_name(int respect)
{
    static const char *const k[] = { "min", "peon", "minion", "peer", "leader", "worship", "max" };
    return respect >= 0 && respect < (int)(sizeof k / sizeof k[0]) ? k[respect] : NULL;
}

const char *sh_ai_emotion_name(int emotion)
{
    static const char *const k[] = { "min", "terrified", "fear", "wary", "destroy_at_all_costs", "destroy", "hate",
                                     "anger", "dislike", "annoyed", "neutral", "friendly", "squadmate", "partner",
                                     "max" };
    return emotion >= 0 && emotion < (int)(sizeof k / sizeof k[0]) ? k[emotion] : NULL;
}

float sh_ai_angle_to(const float from[3], float yaw, const float to[3])
{
    double dx = to[0] - from[0], dy = to[1] - from[1], a;
    if (dx == 0 && dy == 0) return 0.0f;
    a = atan2(dy, dx) * 180.0 / AI_PI - yaw;
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return (float)a;
}

void sh_ai_ring(const float center[3], const float toward[3], float r, int count, float (*out)[3])
{
    double base = atan2(toward[1] - center[1], toward[0] - center[0]);
    int i;
    for (i = 0; i < count; i++) {
        double a = base + 2.0 * AI_PI * i / count;
        out[i][0] = (float)(center[0] + r * cos(a));
        out[i][1] = (float)(center[1] + r * sin(a));
        out[i][2] = center[2];
    }
}

float sh_ai_position_score(float dist_to_target, float ideal, float path_len, float path_weight)
{
    return fabsf(dist_to_target - ideal) + path_len * path_weight;
}

/* The index of `word` in `names` (n entries, the first `prefix` characters
 * of each optional), -1 for none. */
static int enum_parse(const char *const *names, int n, size_t prefix, const char *word)
{
    int i;
    if (!word || !*word) return -1;
    for (i = 0; i < n; i++)
        if (!_stricmp(word, names[i]) || !_stricmp(word, names[i] + prefix)) return i;
    return -1;
}

int sh_ai_fire_mode_parse(const char *word) { return enum_parse(SH_AI_FIREMODE_NAMES, SH_AI_FIREMODE_MAX, 11, word); }
int sh_ai_idle_turn_parse(const char *word) { return enum_parse(SH_AI_IDLETURN_NAMES, SH_AI_IDLETURN_MAX, 11, word); }

const char *sh_ai_fire_mode_name(int mode)
{
    return mode >= 0 && mode <= SH_AI_FIREMODE_MAX ? SH_AI_FIREMODE_NAMES[mode] : NULL;
}

const char *sh_ai_idle_turn_name(int turn)
{
    return turn >= 0 && turn <= SH_AI_IDLETURN_MAX ? SH_AI_IDLETURN_NAMES[turn] : NULL;
}

int sh_ai_muzzle_mode_parse(const char *word)
{
    if (!word) return -1;
    if (!_stricmp(word, "pull")) return SH_AI_MUZZLE_PULL;
    if (!_stricmp(word, "firing")) return SH_AI_MUZZLE_FIRING;
    if (!_stricmp(word, "plain")) return SH_AI_MUZZLE_PLAIN;
    return -1;
}

/* ------------------------------------------------------------- per demon */

#define AI_ANY         3.0e38f   /* no upper bound: a setting may be as large as the script asks */
#define AI_FOLLOW_MIN  32.0f     /* move_to_entity re-aims when the target moved this far */

typedef struct ai_rec {
    sh_script_handle handle;
    int        native_path;
    int        empty_order;
    uint32_t   order_frame;
    int        aborts_sent;      /* setScriptAbort 0 0 and setScriptAbortFlags 0 0 posted */
    int        walk;             /* walk state last set, 0 none */
    sh_ai_move move;
    int        has_dest;
    float      dest[3];
    float      tolerance;
    int        facing;           /* a face order is standing: 1 an entity, 2 a position */
    sh_script_handle face_ent;
    float      face_pos[3];
    unsigned   hand_noted;       /* equip notes already given, a bit per hand */
    unsigned   turn_noted;       /* hold notes already given, a bit per idle turn type */
    sh_script_handle focus_noted; /* the last entity the focus guard refused and reported */
    char      *wait_dest;        /* a held-back animation order (malloc'd node names; NULL: none) */
    char      *wait_via;
    int        wait_kind;        /* its SH_AI_ORDER_ kind */
    int        eternal_interrupts; /* ai:eternal_interrupts(): orders carry DOOM Eternal's interrupt mode */
} ai_rec;

#define AI_TURN_DONE_DEG 10.0f   /* turn_done: facing within this many degrees of the face target */

/* One record per demon a script drives, each allocated once and reused when
 * its demon is gone; the list grows with the demons alive. A record's index
 * is stable (it keys the path queries). Game thread only. */
static ai_rec **g_recs;
static int g_nrecs, g_crecs;
static const sh_script_ai_engine *g_engine;

void sh_script_ai_set_engine(const sh_script_ai_engine *engine) { g_engine = engine; }

/* ---- posed legs (script_api_ai.h) */

static void legq_mul(const float a[4], const float b[4], float o[4])
{
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

static void legq_rot(const float q[4], const float v[3], float o[3])
{
    float ax = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
    float ay = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
    float az = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
    float x = v[0] + 2.0f * (q[1] * az - q[2] * ay);
    float y = v[1] + 2.0f * (q[2] * ax - q[0] * az);
    float z = v[2] + 2.0f * (q[0] * ay - q[1] * ax);
    o[0] = x; o[1] = y; o[2] = z;
}

/* A joint's model transform: its local one under each parent's in turn. */
static int leg_model(const sh_ai_leg_pose *p, int joint, float t[3], float q[4])
{
    int at = joint, steps = 0;
    if (joint < 0 || joint >= p->joints) return 0;
    memcpy(t, p->lt + (size_t)joint * (size_t)p->lt_stride, 3 * sizeof(float));
    memcpy(q, p->lq + (size_t)joint * (size_t)p->lq_stride, 4 * sizeof(float));
    for (;;) {
        float pt[3], pq[4];
        int up = p->parents[at];
        if (up < 0) return 1;
        if (up >= p->joints || ++steps > p->joints) return 0;
        memcpy(pt, p->lt + (size_t)up * (size_t)p->lt_stride, sizeof pt);
        memcpy(pq, p->lq + (size_t)up * (size_t)p->lq_stride, sizeof pq);
        legq_rot(pq, t, t);
        t[0] += pt[0]; t[1] += pt[1]; t[2] += pt[2];
        legq_mul(pq, q, q);
        at = up;
    }
}

int sh_ai_fk_reference_named(const void *chain_address, int effector)
{
    const uint8_t *sub, *effs;
    int count;
    if (!chain_address || effector < 0) return 0;
    sub = *(const uint8_t * const *)chain_address;
    if (!sub) return 0;
    effs = *(const uint8_t * const *)(sub + SH_AI_FK_SUBGRAPH_EFFECTORS);
    count = *(const int *)(sub + SH_AI_FK_SUBGRAPH_EFFECTORS + 8);
    if (!effs || effector >= count) return 0;
    return *(const int *)(effs + (size_t)effector * SH_AI_FK_EFFECTOR_SIZE + SH_AI_FK_EFFECTOR_REF_LEN) > 0;
}

int sh_ai_posed_leg(const sh_ai_leg_pose *p, const sh_ai_leg_joints *j, sh_ai_leg_solve_fn solve, void *leg)
{
    float hip_t[3], hip_q[4], par_t[3], par_q[4], thigh_t[3], thigh_q[4], foot_t[3], foot_q[4];
    float base_t[3] = { 0, 0, 0 }, base_q[4] = { 0, 0, 0, 1 }, inv[4], solved[4], d[4], ax[3], v[3], out_t[3], out_q[4];
    float len, half, pole[4];
    int up;
    if (!p || !j || !solve || !p->parents || !p->lt || !p->lq || p->joints <= 0) return 0;
    if (j->target < 0 || j->target >= p->joints || j->pole < 0 || j->pole >= p->joints) return 0;
    if (!leg_model(p, j->hip, hip_t, hip_q) || !leg_model(p, j->parent, par_t, par_q) ||
        !leg_model(p, j->thigh, thigh_t, thigh_q) || !leg_model(p, j->foot, foot_t, foot_q))
        return 0;
    ax[0] = foot_t[0] - hip_t[0]; ax[1] = foot_t[1] - hip_t[1]; ax[2] = foot_t[2] - hip_t[2];
    len = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (!(len > 1e-6f)) return 0;
    ax[0] /= len; ax[1] /= len; ax[2] /= len;
    if (!solve(leg, hip_t, par_q, foot_t, foot_q, solved)) return 0;
    /* what turns the solved thigh to the posed one, and its part about the hip-to-target line */
    inv[0] = -solved[0]; inv[1] = -solved[1]; inv[2] = -solved[2]; inv[3] = solved[3];
    legq_mul(thigh_q, inv, d);
    half = atan2f(d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2], d[3]);
    pole[0] = sinf(half); pole[1] = 0; pole[2] = 0; pole[3] = cosf(half);
    up = p->parents[j->target];
    if (up >= 0 && !leg_model(p, up, base_t, base_q)) return 0;
    inv[0] = -base_q[0]; inv[1] = -base_q[1]; inv[2] = -base_q[2]; inv[3] = base_q[3];
    v[0] = foot_t[0] - base_t[0]; v[1] = foot_t[1] - base_t[1]; v[2] = foot_t[2] - base_t[2];
    legq_rot(inv, v, out_t);
    legq_mul(inv, foot_q, out_q);
    memcpy(p->lt + (size_t)j->target * (size_t)p->lt_stride, out_t, sizeof out_t);
    memcpy(p->lq + (size_t)j->target * (size_t)p->lq_stride, out_q, sizeof out_q);
    memcpy(p->lq + (size_t)j->pole * (size_t)p->lq_stride, pole, sizeof pole);
    return 1;
}

static void force_via_reset(void);
static void wound_owner_gone(sh_script_handle h, uint32_t spawn_id);
typedef struct wound_effect_row {
    struct wound_effect_row *next;
    void *owner, *wound;
    uint32_t spawn_id;
    int condition;
} wound_effect_row;
static SRWLOCK g_effect_lock = SRWLOCK_INIT;
static wound_effect_row *g_wound_effects;

int sh_ai_set_wound_effect(void *owner, uint32_t id, void *wound, int condition)
{
    wound_effect_row *row;
    if (!owner || !wound || id == SH_SCRIPT_SPAWN_ID_NONE || condition < 0) return 0;
    AcquireSRWLockExclusive(&g_effect_lock);
    for (row = g_wound_effects; row; row = row->next)
        if (row->owner == owner && row->spawn_id == id && row->wound == wound) break;
    if (!row) {
        row = (wound_effect_row *)malloc(sizeof *row);
        if (row) {
            row->owner = owner; row->spawn_id = id; row->wound = wound;
            row->next = g_wound_effects; g_wound_effects = row;
        }
    }
    if (row) row->condition = condition;
    ReleaseSRWLockExclusive(&g_effect_lock);
    return row != NULL;
}

int sh_ai_wound_effect_at(void *owner, uint32_t id, void *wound, int show,
                          const void *call_return, const void *accepted_return)
{
    wound_effect_row *row;
    int effect = -1;
    if (!show || !accepted_return || call_return != accepted_return) return -1;
    AcquireSRWLockShared(&g_effect_lock);
    for (row = g_wound_effects; row; row = row->next) {
        if (row->owner == owner && row->spawn_id == id && row->wound == wound) {
            effect = row->condition;
            break;
        }
    }
    ReleaseSRWLockShared(&g_effect_lock);
    return effect;
}

void sh_ai_remove_wound_effects(uint32_t id)
{
    wound_effect_row **link, *row;
    AcquireSRWLockExclusive(&g_effect_lock);
    link = &g_wound_effects;
    while ((row = *link) != NULL) {
        if (id == SH_SCRIPT_SPAWN_ID_NONE || row->spawn_id == id) {
            *link = row->next;
            free(row);
        } else link = &row->next;
    }
    ReleaseSRWLockExclusive(&g_effect_lock);
}
#ifndef SH_SCRIPT_PURE
static void wound_meshes_remove(uint32_t spawn_id);
#endif
static void anim_wait_drop(ai_rec *r)
{
    free(r->wait_dest);
    free(r->wait_via);
    r->wait_dest = r->wait_via = NULL;
}

void sh_script_ai_reset(void)
{
    int i;
    for (i = 0; i < g_nrecs; i++) {
        anim_wait_drop(g_recs[i]);
        memset(g_recs[i], 0, sizeof *g_recs[i]);
    }
#ifndef SH_SCRIPT_PURE
    wound_meshes_remove(SH_SCRIPT_SPAWN_ID_NONE);
#endif
    sh_ai_remove_wound_effects(SH_SCRIPT_SPAWN_ID_NONE);
    force_via_reset();
    sh_ai_pain_records_reset();
    sh_wound_count_reset();
}

/* The record of `h` (made when missing); a record whose handle no longer
 * belongs to this map is reused, else the list grows. NULL only without
 * memory. */
static int rec_slot(sh_script_handle h)
{
    int i, free_slot = -1;
    for (i = 0; i < g_nrecs; i++) {
        if (g_recs[i]->handle == h) return i;
        if (free_slot < 0 && (!g_recs[i]->handle || !sh_script_handle_known(g_recs[i]->handle))) free_slot = i;
    }
    if (free_slot < 0) {
        ai_rec *r = (ai_rec *)calloc(1, sizeof *r);
        if (!r) return -1;
        if (g_nrecs == g_crecs) {
            int cap = g_crecs ? g_crecs * 2 : 64;
            ai_rec **grown = (ai_rec **)realloc(g_recs, (size_t)cap * sizeof *grown);
            if (!grown) { free(r); return -1; }
            g_recs = grown;
            g_crecs = cap;
        }
        g_recs[g_nrecs] = r;
        free_slot = g_nrecs++;
    }
    anim_wait_drop(g_recs[free_slot]);
    memset(g_recs[free_slot], 0, sizeof *g_recs[free_slot]);
    g_recs[free_slot]->handle = h;
    return free_slot;
}

static ai_rec *rec_of(sh_script_handle h)
{
    int i = rec_slot(h);
    return i < 0 ? NULL : g_recs[i];
}

static int rec_index(const ai_rec *r)
{
    int i;
    for (i = 0; i < g_nrecs; i++) if (g_recs[i] == r) return i;
    return 0;
}

/* ------------------------------------------------------------- arguments */

static sh_ai_arg arg_i(int v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'i'; a.i = v; return a; }
static sh_ai_arg arg_b(int v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'b'; a.i = v ? 1 : 0; return a; }
static sh_ai_arg arg_f(float v) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'f'; a.f = v; return a; }
static sh_ai_arg arg_s(char t, const char *s) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = t; a.s = s; return a; }
static sh_ai_arg arg_e(void *e) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = 'e'; a.ent = e; return a; }
static sh_ai_arg arg_anim(void) { sh_ai_arg a; memset(&a, 0, sizeof a); a.type = '4'; return a; }
static sh_ai_arg arg_v(const float v[3])
{
    sh_ai_arg a;
    memset(&a, 0, sizeof a);
    a.type = 'v';
    a.v[0] = v[0]; a.v[1] = v[1]; a.v[2] = v[2];
    return a;
}

/* Run a catalogue event; 1 when the engine ran it. */
static int run(void *ent, int ev, const sh_ai_arg *args, int n, double *ret)
{
    if (!sh_ai_args_fit(SH_AI_EVENTS[ev].format, args, n)) return 0;
    return g_engine->event(ent, ev, args, n, ret);
}

/* A position at `idx`: a table {x=, y=, z=} or {x, y, z}. Raises a plain
 * error naming `call` otherwise. */
static void check_vec(lua_State *L, int idx, const char *call, float out[3])
{
    static const char *keys[3] = { "x", "y", "z" };
    int k;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs a position (a table with x, y and z), got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    for (k = 0; k < 3; k++) {
        int isnum = 0;
        lua_Number v;
        lua_getfield(L, idx, keys[k]);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, idx, k + 1); }
        v = lua_tonumberx(L, -1, &isnum);
        lua_pop(L, 1);
        if (!isnum || v != v || v > 1e7 || v < -1e7)
            sh_script_refuse(L, "\"%s\" needs a position with a number for %s", call, keys[k]);
        out[k] = (float)v;
    }
}

static void push_vec(lua_State *L, const float v[3])
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
}

static int is_entity(lua_State *L, int idx)
{
    return lua_type(L, idx) == LUA_TUSERDATA && luaL_testudata(L, idx, "entity") != NULL;
}

/* The demon a call acts on: argument 1 must be an idAI2 (any demon of
 * the map). Returns NULL when the entity was removed (the call then
 * answers nil). Raises a plain error otherwise. */
static void *self_ai(lua_State *L, const char *call, sh_script_handle *h_out)
{
    uint64_t h = sh_script_check_entity(L, 1, call);
    void *ent;
    if (!g_engine)
        sh_script_refuse(L, "\"%s\" is not available in this session (Snapmap+ could not find the game's AI "
                         "functions; see the backend log)", call);
    if (!h) return NULL;
    ent = g_engine->entity((sh_script_handle)h);
    if (!ent) return NULL;
    if (!g_engine->is_ai(ent))
        sh_script_refuse(L, "\"%s\" works only on a demon, and this entity is not one", call);
    if (h_out) *h_out = (sh_script_handle)h;
    return ent;
}

/* An entity argument at `idx` that may be any entity (a player, another
 * demon). NULL when it was removed. */
static void *other_entity(lua_State *L, int idx, const char *call)
{
    uint64_t h = sh_script_check_entity(L, idx, call);
    return h ? g_engine->entity((sh_script_handle)h) : NULL;
}

/* The focus guard: an entity is a focus only when its origin is finite and within +-1e7 and it is not the
 * demon itself. idFocusInfo::Update (VK 0x1404C78C0 / 0x1404C9380, GL 0x1404C6290 / 0x1404C7D50) checks for
 * NaN only, so an absurd point reaches the joint chain and logs "has an NaN focus point!" every frame. */
static int focus_entity_ok(void *ai, void *ent)
{
    float o[3];
    int k;
    if (ent == ai || !g_engine->origin(ent, o)) return 0;
    for (k = 0; k < 3; k++) if (o[k] != o[k] || o[k] > 1e7f || o[k] < -1e7f) return 0;
    return 1;
}

/* The guard's refusal, said once per refused entity in the mod's script log: the call, the entity's decl name and
 * the origin it read, so a bench can tell a refused focus from one never asked for. */
static void focus_refused(lua_State *L, ai_rec *r, void *ai, void *ent, const char *call)
{
    const sh_script_engine *ce = sh_script_api_engine();
    sh_script_handle h = g_engine->handle_of(ent);
    char name[160] = "?", line[400];
    float o[3] = { 0, 0, 0 };
    int got;
    if (!r || r->focus_noted == h) return;
    r->focus_noted = h;
    if (ce && ce->def_name) ce->def_name(ent, name, sizeof name);
    got = g_engine->origin(ent, o);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "\"%s\": %s is no focus for this demon (%s), so the aim focus is cleared instead", call,
                ent == ai ? "the demon itself" : name,
                ent == ai ? "a zero direction" : !got ? "its origin cannot be read" : "its origin is not a point of the world");
    if (ent != ai && got) {
        size_t n = strlen(line);
        _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "; origin %g %g %g", o[0], o[1], o[2]);
    }
    shs_log(shs_mod_of(L), line);
}

/* A target at `idx`: an entity (its origin) or a position. Returns 1 with
 * the point, 0 when the entity was removed. *ent_out gets the entity. */
static int target_point(lua_State *L, int idx, const char *call, float out[3], void **ent_out)
{
    if (ent_out) *ent_out = NULL;
    if (is_entity(L, idx)) {
        void *e = other_entity(L, idx, call);
        if (!e || !g_engine->origin(e, out)) return 0;
        if (ent_out) *ent_out = e;
        return 1;
    }
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs an entity or a position, got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    check_vec(L, idx, call, out);
    return 1;
}

/* Check that `idx` holds an entity or a position, without the engine. */
static void check_target(lua_State *L, int idx, const char *call)
{
    float v[3];
    if (is_entity(L, idx)) { sh_script_check_entity(L, idx, call); return; }
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" needs an entity or a position, got %s", call,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    check_vec(L, idx, call, v);
}

static int opt_bool_field(lua_State *L, int idx, const char *key)
{
    int v;
    lua_getfield(L, idx, key);
    v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static float opt_num_field(lua_State *L, int idx, const char *key, float dflt, const char *call, float lo, float hi)
{
    float v = dflt;
    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1)) {
        int isnum = 0;
        lua_Number n = lua_tonumberx(L, -1, &isnum);
        if (!isnum || n != n || n < lo || n > hi) {
            if (hi >= AI_ANY) sh_script_refuse(L, "\"%s\": %s must be a number of at least %g", call, key, (double)lo);
            sh_script_refuse(L, "\"%s\": %s must be a number from %g to %g", call, key, (double)lo, (double)hi);
        }
        v = (float)n;
    }
    lua_pop(L, 1);
    return v;
}

static float opt_tolerance(lua_State *L, int idx, const char *call)
{
    lua_Number t;
    if (lua_isnoneornil(L, idx)) return 0.0f;
    if (lua_type(L, idx) != LUA_TNUMBER)
        sh_script_refuse(L, "\"%s\" needs a number of inches as its tolerance, got %s", call, luaL_typename(L, idx));
    t = lua_tonumber(L, idx);
    if (t != t || t < 0 || t > 4096)
        sh_script_refuse(L, "\"%s\" needs a tolerance from 0 to 4096 inches", call);
    return (float)t;
}

/* The flags table of move_to and move_to_entity. */
typedef struct move_flags { int run, aim; } move_flags;
static move_flags check_flags(lua_State *L, int idx, const char *call)
{
    move_flags f = { 0, 0 };
    if (lua_isnoneornil(L, idx)) return f;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"%s\" takes its options as a table, as { run = true }, got %s", call, luaL_typename(L, idx));
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        if (!k || (strcmp(k, "run") && strcmp(k, "strafe") && strcmp(k, "aim_while_moving")))
            sh_script_refuse(L, "\"%s\" knows the options run, strafe and aim_while_moving, not \"%s\"", call,
                             k ? k : "?");
        lua_pop(L, 1);
    }
    f.run = opt_bool_field(L, idx, "run");
    f.aim = opt_bool_field(L, idx, "strafe") || opt_bool_field(L, idx, "aim_while_moving");
    return f;
}

/* ------------------------------------------------- a step of the engine's */

int sh_ai_step_in_flight(const sh_ai_step_state *s)
{
    return s && s->moving && s->waiting &&
           (s->stage == SH_AI_STAGE_MOVE_STEP || s->stage == SH_AI_STAGE_ARRIVAL ||
            s->stage == SH_AI_STAGE_ARRIVAL_SOFT);
}

/* 1 with the state when a step animation of the engine's is in flight on the
 * demon (a turn in place, a step or an arrival of a move). */
static int step_in_flight(void *ai, sh_ai_step_state *st)
{
    memset(st, 0, sizeof *st);
    st->idle_turn = -1;
    if (!g_engine->step_state || g_engine->step_state(ai, st) != 1) return 0;
    return sh_ai_step_in_flight(st);
}

/* ----------------------------------------------------------- the park */

/* Park the demon for script control: the engine side marks it, and the
 * abort flags are cleared once. Returns its record, or NULL when the
 * engine refused to park it (the call then does nothing). */
static ai_rec *park(void *ai, sh_script_handle h)
{
    ai_rec *r = rec_of(h);
    if (!r || r->native_path) return NULL;
    if (!g_engine->park(ai)) return NULL;
    if (!r->aborts_sent) {
        sh_ai_arg a[2];
        a[0] = arg_b(0); a[1] = arg_b(0);
        if (run(ai, SH_AI_EV_SET_SCRIPT_ABORT, a, 2, NULL) & run(ai, SH_AI_EV_SET_SCRIPT_ABORT_FLAGS, a, 2, NULL))
            r->aborts_sent = 1;
    }
    return r;
}

static void set_walk(void *ai, ai_rec *r, int state)
{
    sh_ai_arg a = arg_i(state);
    if (r->walk == state) return;
    if (run(ai, SH_AI_EV_SET_WALK_STATE, &a, 1, NULL)) r->walk = state;
}

/* Aim at the current enemy while moving (2016 locomotion strafes when the
 * aim focus is off the travel direction). */
static void aim_enemy(void *ai)
{
    void *enemy = g_engine->enemy(ai);
    sh_ai_arg a[4];
    if (!enemy) return;
    a[0] = arg_e(enemy); a[1] = arg_b(1); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
    run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL);
}

static int issue_move(void *ai, ai_rec *r, const float dest[3], float tol, move_flags f)
{
    sh_ai_arg a[5];
    float zero[3] = { 0, 0, 0 };
    set_walk(ai, r, f.run ? SH_AI_WALKSTATE_RUNNING : SH_AI_WALKSTATE_WALKING);
    if (f.aim) aim_enemy(ai);
    a[0] = arg_v(dest); a[1] = arg_i(SH_AI_ARRIVAL_STOP); a[2] = arg_f(tol > 0 ? tol : 0.1f);
    a[3] = arg_b(0); a[4] = arg_v(zero);
    if (!run(ai, SH_AI_EV_MOVE_TO_POINT, a, 5, NULL)) return 0;
    anim_wait_drop(r);     /* the move's own animations take the web: an order still held back is over */
    sh_ai_move_issue(&r->move, g_engine->frame());
    r->has_dest = 1;
    memcpy(r->dest, dest, sizeof r->dest);
    r->tolerance = tol;
    return 1;
}

/* ---------------------------------------------------------- the calls */

/* ai:move_to(pos, [tolerance], [flags]) -> true when the order was given */
static int l_move_to(lua_State *L)
{
    sh_script_handle h = 0;
    float dest[3], tol;
    move_flags f;
    void *ai;
    ai_rec *r;
    check_vec(L, 2, "move_to", dest);
    tol = opt_tolerance(L, 3, "move_to");
    f = check_flags(L, 4, "move_to");
    ai = self_ai(L, "move_to", &h);
    if (!ai || !(r = park(ai, h))) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, issue_move(ai, r, dest, tol, f));
    return 1;
}

/* ai:move_to_entity(e, [tolerance], [flags]): a move to the entity's position, reissued when it has moved
 * AI_FOLLOW_MIN inches (or the tolerance, if larger) from the last order's point or the last move ended. */
static int l_move_to_entity(lua_State *L)
{
    sh_script_handle h = 0;
    float to[3], tol;
    move_flags f;
    void *ai, *target;
    ai_rec *r;
    double dx, dy, dz, gap;
    sh_script_check_entity(L, 2, "move_to_entity");
    tol = opt_tolerance(L, 3, "move_to_entity");
    f = check_flags(L, 4, "move_to_entity");
    ai = self_ai(L, "move_to_entity", &h);
    if (!ai) { lua_pushboolean(L, 0); return 1; }
    target = other_entity(L, 2, "move_to_entity");
    if (!target || !g_engine->origin(target, to) || !(r = park(ai, h))) { lua_pushboolean(L, 0); return 1; }
    if (r->has_dest && r->move.state == SH_AI_MOVE_RUNNING) {
        dx = to[0] - r->dest[0]; dy = to[1] - r->dest[1]; dz = to[2] - r->dest[2];
        gap = tol > AI_FOLLOW_MIN ? tol : AI_FOLLOW_MIN;
        if (dx * dx + dy * dy + dz * dz < gap * gap) { lua_pushboolean(L, 1); return 1; }
    }
    lua_pushboolean(L, issue_move(ai, r, to, tol, f));
    return 1;
}

/* ai:stop() */
static int l_stop(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "stop", &h);
    ai_rec *r;
    sh_ai_arg a[2];
    if (!ai || !(r = park(ai, h))) return 0;
    a[0] = arg_i(0); a[1] = arg_i(SH_AI_IDLETURN_COMBAT);
    if (run(ai, SH_AI_EV_IDLE, a, 2, NULL)) {
        r->move.state = SH_AI_MOVE_NONE;
        r->has_dest = 0;
    }
    return 0;
}

/* ai:move_done() -> done, failed */
static int l_move_done(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "move_done", &h);
    ai_rec *r;
    int st, status;
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (r->move.state == SH_AI_MOVE_NONE) { lua_pushboolean(L, 1); lua_pushboolean(L, 0); return 2; }
    status = g_engine->move_status(ai);
    st = sh_ai_move_step(&r->move, status < 0 ? 0 : status, g_engine->frame());
    if (st == SH_AI_MOVE_RUNNING && r->tolerance > 0 && r->has_dest) {
        float at[3];
        if (g_engine->origin(ai, at)) {
            double dx = at[0] - r->dest[0], dy = at[1] - r->dest[1];
            if (dx * dx + dy * dy <= (double)r->tolerance * r->tolerance) st = r->move.state = SH_AI_MOVE_ARRIVED;
        }
    }
    lua_pushboolean(L, st != SH_AI_MOVE_RUNNING);
    lua_pushboolean(L, st == SH_AI_MOVE_FAILED);
    return 2;
}

/* face's options: { travel = inches }, how far a turn clip's root may step from where it starts. 0 when absent. */
static double face_travel(lua_State *L, int idx)
{
    double travel = 0;
    if (lua_isnoneornil(L, idx)) return 0;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"face\" takes its options as a table, such as { travel = 40 }, got %s", luaL_typename(L, idx));
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        if (!k || strcmp(k, "travel"))
            sh_script_refuse(L, "\"face\" knows the option travel, not \"%s\"", k ? k : luaL_typename(L, -2));
        if (!lua_isnumber(L, -1) || (travel = lua_tonumber(L, -1)) < 0 || travel != travel || travel > 1e6)
            sh_script_refuse(L, "\"face\" needs travel as a distance in inches, 0 or more");
        lua_pop(L, 1);
    }
    return travel;
}

/* ai:face(target, [options]): an entity sets the focus (or, with travel, turns toward where it stands); a
 * position turns with a move to the demon's own origin so the engine picks a turn clip. The move tolerance is
 * the clip query's arrival radius (transitionQueryParms_t +0x6C, VK 0x1404FC0B0 / GL 0x1404FADE0); a turn in
 * place rejects a clip ending past 1.0 to 1.3 times max(1, radius) (VK 0x141AD9E90 / GL 0x141AF40E0), and
 * travel passes its inches as that radius (default 0.1). */
static int l_face(lua_State *L)
{
    sh_script_handle h = 0;
    float p[3];
    void *ai, *ent = NULL;
    ai_rec *r;
    int is_ent = is_entity(L, 2);
    double travel;
    if (!is_ent) check_vec(L, 2, "face", p);
    else sh_script_check_entity(L, 2, "face");
    travel = face_travel(L, 3);
    ai = self_ai(L, "face", &h);
    if (!ai) return 0;
    if (is_ent && !(ent = other_entity(L, 2, "face"))) return 0;
    if (ent && !focus_entity_ok(ai, ent)) { focus_refused(L, rec_of(h), ai, ent, "face"); return 0; }
    if (ent && travel > 0) {
        if (!g_engine->origin(ent, p)) return 0;
        ent = NULL;
    }
    if (!(r = park(ai, h))) return 0;
    if (ent) {
        sh_ai_arg a[4];
        a[0] = arg_e(ent); a[1] = arg_b(0); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
        if (run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL)) {
            r->facing = 1;
            r->face_ent = g_engine->handle_of(ent);
        }
    } else {
        float at[3], dir[3];
        double len;
        sh_ai_arg a[5];
        if (!g_engine->origin(ai, at)) return 0;
        dir[0] = p[0] - at[0]; dir[1] = p[1] - at[1]; dir[2] = 0;
        len = sqrt((double)dir[0] * dir[0] + (double)dir[1] * dir[1]);
        if (len < 1e-3) return 0;
        dir[0] = (float)(dir[0] / len); dir[1] = (float)(dir[1] / len);
        a[0] = arg_v(at); a[1] = arg_i(SH_AI_ARRIVAL_STOP); a[2] = arg_f(travel > 0.1 ? (float)travel : 0.1f);
        a[3] = arg_b(1); a[4] = arg_v(dir);
        if (run(ai, SH_AI_EV_MOVE_TO_POINT, a, 5, NULL)) {
            sh_ai_move_issue(&r->move, g_engine->frame());
            r->has_dest = 1;
            memcpy(r->dest, at, sizeof at);
            r->tolerance = 0;
            r->facing = 2;
            memcpy(r->face_pos, p, sizeof p);
        }
    }
    return 0;
}

/* ai:turn_done() -> the demon faces its last face() target within AI_TURN_DONE_DEG degrees (true when no face
 * order stands; nil while the target is gone). Computed from the facing, as 2016's turn has no completion flag. */
static int l_turn_done(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai = self_ai(L, "turn_done", &h);
    ai_rec *r;
    float at[3], to[3], yaw;
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (!r->facing) { lua_pushboolean(L, 1); return 1; }
    if (r->facing == 1) {
        void *t = g_engine->entity(r->face_ent);
        if (!t || !g_engine->origin(t, to)) { lua_pushnil(L); return 1; }
    } else {
        memcpy(to, r->face_pos, sizeof to);
    }
    if (!g_engine->origin(ai, at) || !g_engine->yaw(ai, &yaw)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, fabsf(sh_ai_angle_to(at, yaw, to)) <= AI_TURN_DONE_DEG);
    return 1;
}

/* ai:aim_at(e) aims the weapon at an entity; ai:aim_at(nil) stops aiming. */
static int l_aim_at(lua_State *L)
{
    sh_script_handle h = 0;
    ai_rec *r;
    void *ai, *ent = NULL;
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "aim_at");
    ai = self_ai(L, "aim_at", &h);
    if (!ai) return 0;
    if (!lua_isnoneornil(L, 2) && !(ent = other_entity(L, 2, "aim_at"))) return 0;
    if (!(r = park(ai, h))) return 0;
    if (ent && !focus_entity_ok(ai, ent)) { focus_refused(L, r, ai, ent, "aim_at"); ent = NULL; }
    if (ent) {   /* the focus guard: focus_entity_ok */
        sh_ai_arg a[4];
        a[0] = arg_e(ent); a[1] = arg_b(1); a[2] = arg_b(1); a[3] = arg_i(SH_AI_AIMPOINT_CENTER);
        run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL);
    } else {
        run(ai, SH_AI_EV_CLEAR_AIM_FOCUS, NULL, 0, NULL);
    }
    return 0;
}

/* ai:set_enemy([e]): e, or with no argument the closest player. */
static int l_set_enemy(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai, *ent = NULL;
    sh_ai_arg a[2];
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "set_enemy");
    ai = self_ai(L, "set_enemy", &h);
    if (!ai) return 0;
    if (!lua_isnoneornil(L, 2) && !(ent = other_entity(L, 2, "set_enemy"))) return 0;
    if (!park(ai, h)) return 0;
    if (ent) {
        a[0] = arg_e(ent); a[1] = arg_b(1);
        run(ai, SH_AI_EV_SET_ENEMY, a, 2, NULL);
    } else {
        a[0] = arg_i(SH_AI_PLAYER_CLOSEST);
        run(ai, SH_AI_EV_SET_PLAYER_ENEMY, a, 1, NULL);
    }
    return 0;
}

/* ai:enemy() -> entity or nil */
static int l_enemy(lua_State *L)
{
    void *ai = self_ai(L, "enemy", NULL), *e;
    if (!ai || !(e = g_engine->enemy(ai))) { lua_pushnil(L); return 1; }
    sh_script_push_entity(L, g_engine->handle_of(e));
    return 1;
}

#define AI_SIGHT_RAISE 40.0f   /* can_see and find_position aim this far above the target's origin */

/* The world-trace half of can_see and find_position: from `from` (or the
 * demon's eye) to AI_SIGHT_RAISE above the target's origin. 1 clear, 0
 * blocked, -1 the engine cannot say. */
static int sight_to(void *ai, const float *from, void *target, const float target_at[3])
{
    float eye[3], to[3];
    if (!g_engine->sight) return -1;
    if (!from) {
        if (!g_engine->eye || !g_engine->eye(ai, eye)) return -1;
        from = eye;
    }
    to[0] = target_at[0]; to[1] = target_at[1]; to[2] = target_at[2] + AI_SIGHT_RAISE;
    return g_engine->sight(ai, from, to, target);
}

/* ai:can_see(e) -> the perception saw e within the last second and no world geometry blocks the eyes' view */
static int l_can_see(lua_State *L)
{
    void *ai, *e;
    double v = 0;
    float at[3];
    int s;
    sh_script_check_entity(L, 2, "can_see");
    ai = self_ai(L, "can_see", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    e = other_entity(L, 2, "can_see");
    if (!e || !g_engine->origin(e, at)) { lua_pushboolean(L, 0); return 1; }
    {
        sh_ai_arg a = arg_e(e);
        if (!run(ai, SH_AI_EV_IS_ENTITY_VISIBLE, &a, 1, &v)) { lua_pushnil(L); return 1; }
    }
    if (v == 0) { lua_pushboolean(L, 0); return 1; }
    s = sight_to(ai, NULL, e, at);
    if (s < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, s == 1);
    return 1;
}

/* ai:angle_to(target) -> degrees from the demon's facing, -180..180, + to its left */
static int l_angle_to(lua_State *L)
{
    float at[3], to[3], yaw;
    void *ai;
    check_target(L, 2, "angle_to");
    ai = self_ai(L, "angle_to", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "angle_to", to, NULL) || !g_engine->origin(ai, at) || !g_engine->yaw(ai, &yaw)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, sh_ai_angle_to(at, yaw, to));
    return 1;
}

/* ai:aware() -> the demon is aware of its enemy */
static int l_aware(lua_State *L)
{
    void *ai = self_ai(L, "aware", NULL);
    double v = 0;
    if (!ai || !run(ai, SH_AI_EV_HAS_AWARENESS_OF_ENEMY, NULL, 0, &v)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, v != 0);
    return 1;
}

/* ai:awareness([e]) -> "unaware" | "lost" | "suspected" | "confirmed" of e
 * (its enemy when e is omitted); nil when there is no one to be aware of. */
static int l_awareness(lua_State *L)
{
    void *ai, *e;
    const char *name;
    if (!lua_isnoneornil(L, 2)) sh_script_check_entity(L, 2, "awareness");
    ai = self_ai(L, "awareness", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    e = lua_isnoneornil(L, 2) ? g_engine->enemy(ai) : other_entity(L, 2, "awareness");
    if (!e || !(name = sh_ai_awareness_name(g_engine->awareness(ai, e)))) { lua_pushnil(L); return 1; }
    lua_pushstring(L, name);
    return 1;
}

/* The path query for `goal` under key slot `n` of the demon's record. */
static int path_of(void *ai, sh_script_handle h, const float goal[3], int n, float *len)
{
    ai_rec *r = rec_of(h);
    int key;
    if (!r) return -1;
    key = 0x50000000 | ((rec_index(r) & 0xFFF) << 14) | (n % SH_AI_KEY_SPAN);
    return g_engine->path(ai, goal, key, len);
}

/* ai:path_length(pos) -> inches along the navigation; nil, "searching" while the engine finds the path, nil,
 * "no path" when it cannot get there. */
static int l_path_length(lua_State *L)
{
    sh_script_handle h = 0;
    float goal[3], len = 0;
    void *ai;
    int st;
    check_target(L, 2, "path_length");
    ai = self_ai(L, "path_length", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "path_length", goal, NULL)) { lua_pushnil(L); lua_pushstring(L, "no path"); return 2; }
    st = path_of(ai, h, goal, 0, &len);
    if (st == 0) { lua_pushnumber(L, len); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, st == 1 ? "searching" : st == 2 ? "no path" : "unavailable");
    return 2;
}

/* ai:can_reach(pos) -> true, false, or nil while the engine is searching */
static int l_can_reach(lua_State *L)
{
    sh_script_handle h = 0;
    float goal[3], len = 0;
    void *ai;
    int st;
    check_target(L, 2, "can_reach");
    ai = self_ai(L, "can_reach", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "can_reach", goal, NULL)) { lua_pushboolean(L, 0); return 1; }
    st = path_of(ai, h, goal, 0, &len);
    if (st == 1) lua_pushnil(L);
    else lua_pushboolean(L, st == 0);
    return 1;
}

/* ai:find_position(parms) -> pos, or nil, "searching" | "none". Scores `count` points on a circle of radius
 * `ideal` around the target (default its enemy) by |distance - ideal| + path length * path_weight, keeping
 * those that see the target (unless visible is false) and have a path no longer than max_path. */
static int l_find_position(lua_State *L)
{
    sh_script_handle h = 0;
    float center[3], at[3], (*ring)[3], best[3];
    float mn, mx, ideal, weight, max_path, best_score = 0, head = 64.0f;
    int count, i, have = 0, searching = 0, has_target, visible;
    void *ai, *target = NULL;
    if (!lua_isnoneornil(L, 2) && lua_type(L, 2) != LUA_TTABLE)
        sh_script_refuse(L, "\"find_position\" takes a table of settings, as { ideal = 400 }, got %s", luaL_typename(L, 2));
    if (lua_isnoneornil(L, 2)) { lua_settop(L, 1); lua_newtable(L); }
    mn = opt_num_field(L, 2, "min", 0, "find_position", 0, AI_ANY);
    mx = opt_num_field(L, 2, "max", 2048, "find_position", 0, AI_ANY);
    if (mx < mn) sh_script_refuse(L, "\"find_position\": max must not be less than min");
    ideal = opt_num_field(L, 2, "ideal", (mn + mx) / 2, "find_position", mn, mx);
    count = (int)opt_num_field(L, 2, "count", 8, "find_position", 2, 2147483520.0f);
    weight = opt_num_field(L, 2, "path_weight", 0.5f, "find_position", 0, AI_ANY);
    max_path = opt_num_field(L, 2, "max_path", 0, "find_position", 0, AI_ANY);
    lua_getfield(L, 2, "visible");
    visible = lua_isnil(L, -1) ? 1 : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "target");
    has_target = !lua_isnil(L, -1);
    if (has_target && !is_entity(L, -1) && lua_type(L, -1) != LUA_TTABLE)
        sh_script_refuse(L, "\"find_position\": target must be an entity or a position");
    if (has_target && lua_type(L, -1) == LUA_TTABLE) check_vec(L, lua_gettop(L), "find_position", center);
    ai = self_ai(L, "find_position", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (has_target && is_entity(L, -1)) {
        if (!target_point(L, lua_gettop(L), "find_position", center, &target)) { lua_pushnil(L); lua_pushstring(L, "none"); return 2; }
    } else if (!has_target) {
        target = g_engine->enemy(ai);
        if (!target || !g_engine->origin(target, center)) { lua_pushnil(L); lua_pushstring(L, "none"); return 2; }
    }
    if (!g_engine->origin(ai, at)) { lua_pushnil(L); return 1; }
    {
        float eye[3];
        if (g_engine->eye && g_engine->eye(ai, eye) && eye[2] > at[2]) head = eye[2] - at[2];
    }
    /* the candidates, as many as asked, in memory the collector frees */
    ring = (float (*)[3])lua_newuserdatauv(L, (size_t)count * sizeof *ring, 0);
    sh_ai_ring(center, at, ideal, count, ring);
    for (i = 0; i < count; i++) {
        float len = 0, d, score;
        int st;
        if (visible) {
            float from[3];
            from[0] = ring[i][0]; from[1] = ring[i][1]; from[2] = ring[i][2] + head;
            if (sight_to(ai, from, target, center) != 1) continue;   /* no line of sight from there */
        }
        st = path_of(ai, h, ring[i], 1 + i, &len);
        if (st == 1) { searching = 1; continue; }
        if (st != 0 || (max_path > 0 && len > max_path)) continue;
        d = ideal;   /* every candidate is on the ideal circle */
        score = sh_ai_position_score(d, ideal, len, weight);
        if (!have || score < best_score) { have = 1; best_score = score; memcpy(best, ring[i], sizeof best); }
    }
    if (have) { push_vec(L, best); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, searching ? "searching" : "none");
    return 2;
}

/* ---- the weapon: which hand, which item
 *
 * The game's fire control runs only while the hand named by weaponSlot in the aibehavior decl (default right)
 * holds a weapon, and equip puts an item in the hand its decl's equipSlot names. So trigger, trigger_pulled
 * and weapon_ready act on the named hand's weapon, else the weapon hand. */

static const char *hand_name(int hand)
{
    return hand == SH_AI_HAND_LEFT ? "left" : hand == SH_AI_HAND_RIGHT ? "right" : NULL;
}

/* An optional hand at `idx`: 0 (the demon's weapon hand) when absent. */
static int check_hand(lua_State *L, int idx, const char *call)
{
    const char *s;
    if (lua_isnoneornil(L, idx)) return 0;
    s = lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : NULL;
    if (s && !strcmp(s, "left")) return SH_AI_HAND_LEFT;
    if (s && !strcmp(s, "right")) return SH_AI_HAND_RIGHT;
    return sh_script_refuse(L, "\"%s\" needs the hand as \"left\" or \"right\" (or nothing for the demon's weapon "
                            "hand), got %s", call, s ? s : luaL_typename(L, idx));
}

/* The hand a call acts on: `hand`, or the demon's weapon hand; 0 when that
 * is not a hand the game has a trigger for. */
static int hand_of(void *ai, int hand)
{
    if (!hand && g_engine->weapon_hand) hand = g_engine->weapon_hand(ai);
    return hand == SH_AI_HAND_LEFT || hand == SH_AI_HAND_RIGHT ? hand : 0;
}

/* ai:trigger(on, [hand]) pulls (true) or releases (false) the trigger of the weapon in `hand`. */
static int l_trigger(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    int on, hand;
    luaL_checkany(L, 2);
    on = lua_toboolean(L, 2);
    hand = check_hand(L, 3, "trigger");
    ai = self_ai(L, "trigger", &h);
    if (!ai) return 0;
    if (!(hand = hand_of(ai, hand)))
        return sh_script_refuse(L, "\"trigger\": this demon's weapon hand (weaponSlot in its aibehavior decl) is "
                                "not a hand; name the hand, \"left\" or \"right\"");
    if (!park(ai, h)) return 0;
    if (hand == SH_AI_HAND_LEFT) run(ai, on ? SH_AI_EV_PULL_TRIGGER_LEFT : SH_AI_EV_RELEASE_TRIGGER_LEFT, NULL, 0, NULL);
    else run(ai, on ? SH_AI_EV_PULL_TRIGGER : SH_AI_EV_RELEASE_TRIGGER, NULL, 0, NULL);
    return 0;
}

/* ai:trigger_pulled([hand]) -> the trigger of the weapon in `hand` is held
 * (a burst running); nil when that hand holds no weapon */
static int l_trigger_pulled(lua_State *L)
{
    int hand = check_hand(L, 2, "trigger_pulled");
    void *ai = self_ai(L, "trigger_pulled", NULL);
    int st;
    if (g_engine && !g_engine->trigger_state)
        sh_script_refuse(L, "\"trigger_pulled\" is not available in this session (Snapmap+ could not find the game "
                         "function it needs; see the backend log)");
    if (!ai || !(hand = hand_of(ai, hand))) { lua_pushnil(L); return 1; }
    st = g_engine->trigger_state(ai, hand);
    if (st < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, st == 1);
    return 1;
}

/* The mod's own name of a decl: "mods/<salt>/x" as "x". */
static const char *own_name(lua_State *L, const char *full)
{
    const char *salt = sh_script_salt(L);
    size_t n = salt ? strlen(salt) : 0;
    if (n && !_strnicmp(full, "mods/", 5) && !_strnicmp(full + 5, salt, n) && full[5 + n] == '/' && full[6 + n])
        return full + 6 + n;
    return full;
}

/* ai:equip(item) -> the hand it went to. `item` is the decl of an inventory
 * item the demon carries (as its entityDef's startingInventory names it), or
 * its inventory index (0 first). */
static int l_equip(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    lua_Integer idx = -1;
    const char *want = NULL;
    char decl[512];
    int hand = -1, weapon_hand, r, i;
    sh_ai_arg a;
    ai_rec *rec;
    decl[0] = 0;
    if (lua_isinteger(L, 2)) {
        if ((idx = lua_tointeger(L, 2)) < 0 || idx > INT_MAX)
            sh_script_refuse(L, "\"equip\" needs an inventory index of 0 or more, got %s", lua_tostring(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING && lua_rawlen(L, 2)) {
        want = lua_tostring(L, 2);
    } else {
        sh_script_refuse(L, "\"equip\" needs the item (its decl name, as the demon's startingInventory names it, or "
                         "its inventory index), got %s", lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    }
    ai = self_ai(L, "equip", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (g_engine->item) {
        if (want) {
            luaL_Buffer have;
            int n = 0;
            luaL_buffinit(L, &have);
            for (i = 0; (r = g_engine->item(ai, i, decl, sizeof decl, &hand)) == 1; i++) {
                const char *mine = own_name(L, decl);
                if (!_stricmp(decl, want) || !_stricmp(mine, want)) { idx = i; break; }
                if (n++) luaL_addstring(&have, ", ");
                luaL_addstring(&have, mine);
            }
            if (idx < 0) {
                luaL_pushresult(&have);
                if (r < 0) { lua_pushnil(L); return 1; }
                return sh_script_refuse(L, "\"equip\": this demon carries no \"%s\" (it carries: %s)", want,
                                        n ? lua_tostring(L, -1) : "nothing");
            }
        } else if ((r = g_engine->item(ai, (int)idx, decl, sizeof decl, &hand)) == 0) {
            return sh_script_refuse(L, "\"equip\": this demon has no inventory item %d", (int)idx);
        } else if (r < 0) hand = -1;
    } else if (want) {
        return sh_script_refuse(L, "\"equip\" by name is not available in this session (Snapmap+ could not find the "
                                "game's inventory function; see the backend log); give the inventory index");
    }
    if (!park(ai, h)) { lua_pushnil(L); return 1; }
    a = arg_i((int)idx);
    run(ai, SH_AI_EV_EQUIP_ITEM, &a, 1, NULL);
    /* The fire control fires nothing unless the weapon hand holds a weapon. */
    weapon_hand = g_engine->weapon_hand ? g_engine->weapon_hand(ai) : -1;
    if (hand_name(hand) && weapon_hand >= 0 && weapon_hand != hand && (rec = rec_of(h)) != NULL &&
        !(rec->hand_noted & (1u << hand))) {
        char line[768];
        rec->hand_noted |= 1u << hand;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "\"equip\": %s goes in the demon's %s hand, but the game fires a demon's weapons only while the "
                    "hand its aibehavior decl names in weaponSlot holds one (now %s); set weaponSlot = \"%s\" in that "
                    "decl, or the demon never fires.", own_name(L, decl), hand_name(hand),
                    weapon_hand == SH_AI_HAND_LEFT ? "the left hand" : weapon_hand == SH_AI_HAND_RIGHT ? "the right hand"
                                                                                                 : "no hand",
                    hand == SH_AI_HAND_LEFT ? "EQUIP_LEFT_HAND" : "EQUIP_RIGHT_HAND");
        shs_log(shs_mod_of(L), line);
    }
    if (hand_name(hand)) lua_pushstring(L, hand_name(hand)); else lua_pushnil(L);
    return 1;
}

static const char *check_tag(lua_State *L, int idx, const char *call, const char *what)
{
    size_t n = 0;
    const char *s = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &n) : NULL;
    size_t i;
    if (!s || !n) sh_script_refuse(L, "\"%s\" needs %s", call, what);
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '_' || s[i] == '/' || s[i] == '.'))
            sh_script_refuse(L, "\"%s\": \"%s\" is not a usable name (letters, digits, _ / and . only)", call, s);
    return s;
}

/* ai:laser(tag, on) asks the laser targeter's lasers at `tag` to show or hide. */
static int l_laser(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = check_tag(L, 2, "laser", "a laser tag");
    int on;
    void *ai;
    sh_ai_arg a[2];
    luaL_checkany(L, 3);
    on = lua_toboolean(L, 3);
    ai = self_ai(L, "laser", &h);
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_b(on); a[1] = arg_s('s', tag);
    run(ai, SH_AI_EV_SET_LASER_STATE, a, 2, NULL);
    return 0;
}

/* ai:laser_state(tag, on): the frame-command form of the same request
 * (ae_setLaserState), as the attack clips send it. */
static int l_laser_state(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = check_tag(L, 2, "laser_state", "a laser tag");
    int on;
    void *ai;
    sh_ai_arg a[3];
    luaL_checkany(L, 3);
    on = lua_toboolean(L, 3);
    ai = self_ai(L, "laser_state", &h);
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_b(on); a[2] = arg_s('s', tag);
    run(ai, SH_AI_EV_AE_SET_LASER_STATE, a, 3, NULL);
    return 0;
}

/* ai:laser_sweep(name) starts the named sweep of the demon's laser targeter decl. */
static int l_laser_sweep(lua_State *L)
{
    sh_script_handle h = 0;
    const char *name = check_tag(L, 2, "laser_sweep", "a sweep name");
    void *ai = self_ai(L, "laser_sweep", &h);
    sh_ai_arg a[2];
    if (!ai || !park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_s('s', name);
    run(ai, SH_AI_EV_START_LASER_SWEEP, a, 2, NULL);
    return 0;
}

/* ai:focus_tag(tag) aims from the named tag; ai:focus_tag(nil) goes back to the default. */
static int l_focus_tag(lua_State *L)
{
    sh_script_handle h = 0;
    const char *tag = lua_isnoneornil(L, 2) ? NULL : check_tag(L, 2, "focus_tag", "a tag name");
    void *ai = self_ai(L, "focus_tag", &h);
    const sh_script_engine *ce = sh_script_api_engine();
    sh_ai_arg a[3];
    float pos[3];
    if (!ai) return 0;
    /* The engine focuses on md6Def tags only; a joint name or a misspelling
     * would leave the demon aiming nowhere with a console warning. */
    if (tag && ce && ce->tag && ce->tag(ai, tag, pos, NULL) == 0)
        return sh_script_refuse(L, "\"focus_tag\": \"%s\" is not a tag of this demon's md6Def (the game aims only at "
                                "tags, not joints)", tag);
    if (!park(ai, h)) return 0;
    a[0] = arg_anim(); a[1] = arg_i(SH_AI_FOCI_AIM);
    if (tag) { a[2] = arg_s('s', tag); run(ai, SH_AI_EV_SET_FOCUS_TAG, a, 3, NULL); }
    else run(ai, SH_AI_EV_CLEAR_FOCUS_TAG, a, 2, NULL);
    return 0;
}

/* ------------------------------------------- the brain's calls: pure parts */

static const struct { const char *word; unsigned bits; } k_tests[] = {
    { "TF_NAV_DIRECT", SH_AI_TF_NAV_DIRECT },
    { "TF_NAV_ENDPOINT", SH_AI_TF_NAV_ENDPOINT },
    { "TF_NAV_GROUND", SH_AI_TF_NAV_ENDPOINT },
    { "TF_CLAMP_TO_GROUND", SH_AI_TF_CLAMP_TO_GROUND },
    { "TF_BOUNDS", SH_AI_TF_BOUNDS },
    { "TF_EYE_VISIBILITY", SH_AI_TF_EYE_VISIBILITY },
    { "TF_MUZZLE_VISIBILITY", SH_AI_TF_MUZZLE_VISIBILITY },
    { "TF_TRANSLATION_TO_ENEMY_FORCE", SH_AI_TF_TO_ENEMY_FORCE },
    { "TF_AVOID_ENEMY_FOV", SH_AI_TF_AVOID_ENEMY_FOV },
    { "VALIDATE_ANIM_DELTA", SH_AI_TF_ANIM_DELTA },
};

unsigned sh_ai_tests_parse(const char *words, char *bad, size_t cap)
{
    unsigned bits = 0;
    const char *p = words;
    if (bad && cap) bad[0] = 0;
    if (!words) return 0;
    while (*p) {
        const char *s;
        size_t n, k;
        int found = 0;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '|') p++;
        if (!*p) break;
        s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '|') p++;
        n = (size_t)(p - s);
        for (k = 0; k < sizeof k_tests / sizeof k_tests[0]; k++)
            if (strlen(k_tests[k].word) == n && !_strnicmp(s, k_tests[k].word, n)) { bits |= k_tests[k].bits; found = 1; }
        if (!found) {
            if (bad && cap) _snprintf_s(bad, cap, _TRUNCATE, "%.*s", (int)n, s);
            return 0;
        }
    }
    return bits;
}

const char *sh_ai_monster_name(uint32_t value)
{
    int k;
    if (value == 0) return SH_AI_MONSTER_NAMES[0];
    for (k = 1; k < SH_AI_MONSTER_TYPES; k++) if (value == (1u << (k - 1))) return SH_AI_MONSTER_NAMES[k];
    return NULL;
}

void sh_ai_line_offset(const float a[3], const float b[3], float t, float offset, float roll_deg, float out[3])
{
    double d[3], h, left[3], dir[3], r = roll_deg * AI_PI / 180.0;
    int k;
    for (k = 0; k < 3; k++) d[k] = (double)b[k] - a[k];
    h = sqrt(d[0] * d[0] + d[1] * d[1]);
    if (h > 1e-6) { left[0] = -d[1] / h; left[1] = d[0] / h; } else { left[0] = 0; left[1] = 1; }
    left[2] = 0;
    dir[0] = sin(r) * left[0];
    dir[1] = sin(r) * left[1];
    dir[2] = cos(r);
    for (k = 0; k < 3; k++) out[k] = (float)(a[k] + d[k] * t + dir[k] * offset);
}

float sh_ai_table_at(const sh_ai_table *tb, float t)
{
    int i;
    if (!tb || tb->n <= 0) return 0.0f;
    if (t < tb->t[0]) {
        if (tb->t[0] <= 0.0f) return tb->v[0];
        return t <= 0.0f ? 0.0f : tb->v[0] * (t / tb->t[0]);           /* from 0 at the start */
    }
    for (i = 0; i + 1 < tb->n; i++) {
        if (t <= tb->t[i + 1]) {
            float span = tb->t[i + 1] - tb->t[i];
            float f = span > 1e-6f ? (t - tb->t[i]) / span : 1.0f;
            return tb->v[i] + (tb->v[i + 1] - tb->v[i]) * f;
        }
    }
    if (tb->t[tb->n - 1] >= 1.0f) return tb->v[tb->n - 1];
    return t >= 1.0f ? 0.0f : tb->v[tb->n - 1] * ((1.0f - t) / (1.0f - tb->t[tb->n - 1]));   /* to 0 at the end */
}

int sh_ai_spline_points(const float from[3], const float to[3], const sh_ai_table *tb, float roll_deg,
                        float (*out)[3], int cap)
{
    double dx = (double)to[0] - from[0], dy = (double)to[1] - from[1];
    float h = (float)sqrt(dx * dx + dy * dy);
    int n = 0, i;
    if (cap < 2) return 0;
    memcpy(out[n++], from, sizeof out[0]);
    for (i = 0; tb && i < tb->n && n < cap - 1; i++) {
        float t = tb->t[i];
        if (!(t > 0.0f && t < 1.0f)) continue;
        sh_ai_line_offset(from, to, t, tb->v[i] * h, roll_deg, out[n++]);
    }
    memcpy(out[n++], to, sizeof out[0]);
    return n;
}

int sh_ai_position_kind(const char *word)
{
    if (!word || !_stricmp(word, "any")) return SH_AI_POS_ANY;
    if (!_stricmp(word, "new")) return SH_AI_POS_NEW;
    if (!_stricmp(word, "superior")) return SH_AI_POS_SUPERIOR;
    if (!_stricmp(word, "reposition")) return SH_AI_POS_REPOSITION;
    return -1;
}

/* ------------------------------------ per-demon records other threads read */

/* One record per demon with a pain string or a block standing. Engine threads read the records without a lock,
 * so the table never moves: it holds one record per entity the game can hold (SH_SCRIPT_ENGINE_ENTITIES),
 * readers stay below the high-water mark, and a gone demon's record is reused. */
#define AI_PAIN_RECORDS SH_SCRIPT_ENGINE_ENTITIES
typedef struct ai_pain_rec {
    void *volatile        ai;
    volatile LONG         spawn_id;
    const char *volatile  pain_string;
    volatile LONG         until[SH_AI_PAIN_REACTIONS];   /* game ms; 0 none */
} ai_pain_rec;
static ai_pain_rec g_pain[AI_PAIN_RECORDS];
static volatile LONG g_pain_used;   /* high-water mark: engine threads read only below it */

void sh_ai_pain_records_reset(void)
{
    int i;
    int used = (int)InterlockedExchange(&g_pain_used, 0);
    for (i = 0; i < used; i++) InterlockedExchangePointer((PVOID volatile *)&g_pain[i].ai, NULL);
    memset(g_pain, 0, (size_t)used * sizeof g_pain[0]);
}

static ai_pain_rec *pain_find(void *ai, uint32_t id)
{
    int i, used = (int)g_pain_used;
    if (!used || !ai || id == SH_SCRIPT_SPAWN_ID_NONE) return NULL;
    for (i = 0; i < used; i++)
        if (g_pain[i].ai == ai && (uint32_t)g_pain[i].spawn_id == id) return &g_pain[i];
    return NULL;
}

/* Game thread only: the record of (ai, id), made when missing (a slot whose
 * demon is another one may be reused; the writer publishes the key last). */
static ai_pain_rec *pain_make(void *ai, uint32_t id)
{
    ai_pain_rec *r = pain_find(ai, id);
    const sh_script_engine *e = sh_script_api_engine();
    int i, used = (int)g_pain_used;
    if (r || !ai || id == SH_SCRIPT_SPAWN_ID_NONE) return r;
    for (i = 0; i < used && !r; i++) {
        /* a free record, one with nothing standing, or one whose demon is gone */
        int k, idle = !g_pain[i].ai || g_pain[i].pain_string == NULL;
        for (k = 0; k < SH_AI_PAIN_REACTIONS && idle && g_pain[i].ai; k++) if (g_pain[i].until[k]) idle = 0;
        if (idle || (e && e->resolve && e->resolve((uint32_t)g_pain[i].spawn_id) != g_pain[i].ai)) r = &g_pain[i];
    }
    if (!r && used < AI_PAIN_RECORDS) r = &g_pain[used];
    if (!r) return NULL;                /* every entity the game can hold is a demon with a standing record */
    InterlockedExchangePointer((PVOID volatile *)&r->ai, NULL);
    r->pain_string = NULL;
    for (i = 0; i < SH_AI_PAIN_REACTIONS; i++) r->until[i] = 0;
    InterlockedExchange(&r->spawn_id, (LONG)id);
    InterlockedExchangePointer((PVOID volatile *)&r->ai, ai);
    if (r - g_pain >= used) InterlockedExchange(&g_pain_used, (LONG)(r - g_pain) + 1);
    return r;
}

int sh_ai_pain_string_set(void *ai, uint32_t id, const char *interned)
{
    ai_pain_rec *r = interned ? pain_make(ai, id) : pain_find(ai, id);
    if (!r) return interned == NULL;
    InterlockedExchangePointer((PVOID volatile *)&r->pain_string, (PVOID)interned);
    return 1;
}

const char *sh_ai_pain_string_get(void *ai, uint32_t id)
{
    ai_pain_rec *r = pain_find(ai, id);
    return r ? r->pain_string : NULL;
}

int sh_ai_block_set(void *ai, uint32_t id, int reaction, int until_ms)
{
    ai_pain_rec *r;
    if (reaction <= 0 || reaction >= SH_AI_PAIN_REACTIONS) return 0;
    r = pain_make(ai, id);
    if (!r) return 0;
    InterlockedExchange(&r->until[reaction], (LONG)(until_ms ? until_ms : 1));
    return 1;
}

int sh_ai_blocked(void *ai, uint32_t id, int reaction, int now_ms)
{
    ai_pain_rec *r;
    LONG until;
    if (reaction <= 0 || reaction >= SH_AI_PAIN_REACTIONS) return 0;
    r = pain_find(ai, id);
    if (!r) return 0;
    until = r->until[reaction];
    return until != 0 && now_ms < (int)until;
}

/* ------------------------------------------- wound damage counts (script_api_ai.h) */

float sh_wound_count_threshold(float total, float relative, float max_health)
{
    return relative < 0.0f ? total : relative * max_health;
}

int sh_wound_count_step(float threshold, float *sum, float damage, int gates_passed)
{
    if (!gates_passed || !sum) return 0;
    if (damage > 0.0f && damage <= 3.402823466e+38f) *sum += damage;
    if (!(threshold > 0.0f)) return 1;
    return *sum >= threshold;
}

uint32_t sh_wound_count_mark(const void *record)
{
    const uint8_t *p = (const uint8_t *)record;
    uint32_t h = 2166136261u;
    int i;
    if (!p) return 0;
    for (i = 0; i < 16; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* One record per demon that asked; few demons ask (a ported demon's script),
 * and a record whose demon is gone is reused. */
#define AI_WCOUNT_DEMONS 64
typedef struct {
    const void *wound;
    float       sum;
    const void *last_record;   /* the hit last counted, so the same hit offered again counts once */
    uint32_t    last_mark;
} ai_wcount_row;
typedef struct {
    void          *ai;
    uint32_t       spawn_id;
    int            n;
    ai_wcount_row  row[SH_WOUND_COUNT_WOUNDS];
} ai_wcount_rec;
static ai_wcount_rec g_wcount[AI_WCOUNT_DEMONS];
static int g_wcount_used;
static SRWLOCK g_wcount_lock = SRWLOCK_INIT;

static ai_wcount_rec *wcount_find(void *ai, uint32_t id)
{
    int i;
    if (!ai || id == SH_SCRIPT_SPAWN_ID_NONE) return NULL;
    for (i = 0; i < g_wcount_used; i++)
        if (g_wcount[i].ai == ai && g_wcount[i].spawn_id == id) return &g_wcount[i];
    return NULL;
}

void sh_wound_count_reset(void)
{
    AcquireSRWLockExclusive(&g_wcount_lock);
    memset(g_wcount, 0, (size_t)g_wcount_used * sizeof g_wcount[0]);
    g_wcount_used = 0;
    ReleaseSRWLockExclusive(&g_wcount_lock);
}

int sh_wound_count_set(void *ai, uint32_t id, int on)
{
    const sh_script_engine *e = sh_script_api_engine();
    ai_wcount_rec *r;
    int i, ok = 1;
    if (!ai || id == SH_SCRIPT_SPAWN_ID_NONE) return 0;
    AcquireSRWLockExclusive(&g_wcount_lock);
    r = wcount_find(ai, id);
    if (!on) {
        if (r) memset(r, 0, sizeof *r);
    } else if (!r) {
        for (i = 0; i < g_wcount_used && !r; i++)
            if (!g_wcount[i].ai || (e && e->resolve && e->resolve(g_wcount[i].spawn_id) != g_wcount[i].ai))
                r = &g_wcount[i];
        if (!r && g_wcount_used < AI_WCOUNT_DEMONS) r = &g_wcount[g_wcount_used++];
        if (r) {
            memset(r, 0, sizeof *r);
            r->ai = ai;
            r->spawn_id = id;
        } else ok = 0;
    }
    ReleaseSRWLockExclusive(&g_wcount_lock);
    return ok;
}

int sh_wound_count_on(void *ai, uint32_t id)
{
    int on;
    AcquireSRWLockShared(&g_wcount_lock);
    on = wcount_find(ai, id) != NULL;
    ReleaseSRWLockShared(&g_wcount_lock);
    return on;
}

int sh_wound_count_hit(void *ai, uint32_t id, const void *wound, const void *record, uint32_t mark,
                       float threshold, float damage, int gates_passed)
{
    ai_wcount_rec *r;
    ai_wcount_row *w = NULL;
    int i, st = -1;
    if (!wound) return -1;
    AcquireSRWLockExclusive(&g_wcount_lock);
    r = wcount_find(ai, id);
    if (r) {
        for (i = 0; i < r->n; i++) if (r->row[i].wound == wound) { w = &r->row[i]; break; }
        if (!w && r->n < SH_WOUND_COUNT_WOUNDS) {
            w = &r->row[r->n++];
            memset(w, 0, sizeof *w);
            w->wound = wound;
        }
    }
    if (w) {
        if (gates_passed && record && w->last_record == record && w->last_mark == mark) damage = 0.0f;
        st = sh_wound_count_step(threshold, &w->sum, damage, gates_passed);
        if (gates_passed) { w->last_record = record; w->last_mark = mark; }
    }
    ReleaseSRWLockExclusive(&g_wcount_lock);
    return st;
}

float sh_wound_count_sum(void *ai, uint32_t id, const void *wound)
{
    ai_wcount_rec *r;
    float s = 0.0f;
    int i;
    AcquireSRWLockShared(&g_wcount_lock);
    r = wcount_find(ai, id);
    for (i = 0; r && i < r->n; i++) if (r->row[i].wound == wound) { s = r->row[i].sum; break; }
    ReleaseSRWLockShared(&g_wcount_lock);
    return s;
}

void sh_wound_count_gone(uint32_t id)
{
    int i;
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return;
    AcquireSRWLockExclusive(&g_wcount_lock);
    for (i = 0; i < g_wcount_used; i++)
        if (g_wcount[i].ai && g_wcount[i].spawn_id == id) memset(&g_wcount[i], 0, sizeof g_wcount[i]);
    ReleaseSRWLockExclusive(&g_wcount_lock);
}

static void wound_owner_gone(sh_script_handle h, uint32_t spawn_id)
{
    (void)h;
#ifndef SH_SCRIPT_PURE
    if (spawn_id != SH_SCRIPT_SPAWN_ID_NONE) wound_meshes_remove(spawn_id);
#endif
    sh_wound_count_gone(spawn_id);
    if (spawn_id != SH_SCRIPT_SPAWN_ID_NONE) sh_ai_remove_wound_effects(spawn_id);
}

/* --------------------------------------------- compositions over primitives */

#define AI_KNEE        24.0f     /* the low line of TF_BOUNDS */
#define AI_CHEST       64.0f     /* the high line of TF_BOUNDS */
#define AI_EYE_DEFAULT 64.0f     /* the eye height when the engine cannot say */
#define AI_MUZZLE_PART 0.75f     /* weapon height as a part of the eye height */
#define AI_SPLINE_RAISE 32.0f    /* spline_clear traces this far above each point */
#define AI_STEP        48.0f     /* the most a clip's end may sit above or below the ground under it */
#define AI_TARGET_RAISE 40.0f    /* sights aim this far above the target's origin */
#define AI_LINE_MIN    1.0f      /* TF_NAV_LINE: a clip whose end lies closer than this needs no line */

static float dist3(const float a[3], const float b[3])
{
    double x = (double)a[0] - b[0], y = (double)a[1] - b[1], z = (double)a[2] - b[2];
    return (float)sqrt(x * x + y * y + z * z);
}

static float dist2(const float a[3], const float b[3])
{
    double x = (double)a[0] - b[0], y = (double)a[1] - b[1];
    return (float)sqrt(x * x + y * y);
}

/* The eye's height above the demon's origin. */
static float eye_height(const sh_script_ai_engine *e, void *ai, const float origin[3])
{
    float eye[3];
    if (e->eye && e->eye(ai, eye) && eye[2] > origin[2]) return eye[2] - origin[2];
    return AI_EYE_DEFAULT;
}

/* 1 when from `from` the target point is seen (nothing in between, or the
 * target is the first thing hit); 0 when not; -1 when the engine cannot say. */
static int sees(const sh_script_ai_engine *e, void *ai, const float from[3], const float to[3], void *target)
{
    if (!e->sight) return -1;
    return e->sight(ai, from, to, target);
}

int sh_ai_firing_run(const sh_script_ai_engine *e, void *ai, const sh_ai_position_query *q, int key_base,
                     float out[3])
{
    float at[3], center[3], (*ring)[3], best[3], head, cur_score = 1e30f, best_score = 0;
    int count, i, have = 0, searching = 0, r;
    if (!e || !ai || !q || !e->origin || !e->path || !e->origin(ai, at)) return -1;
    memcpy(center, q->at, sizeof center);
    if (q->target && !e->origin(q->target, center)) return 2;
    count = q->count < 2 ? 2 : q->count;
    ring = (float (*)[3])malloc((size_t)count * sizeof *ring);   /* as many candidates as asked */
    if (!ring) return -1;
    head = eye_height(e, ai, at);
    /* the current place: within the band, and (when asked) seeing the target */
    {
        float d = dist2(at, center), from[3], to[3];
        int ok = d >= q->min && d <= q->max;
        if (ok && q->visible) {
            from[0] = at[0]; from[1] = at[1]; from[2] = at[2] + head;
            to[0] = center[0]; to[1] = center[1]; to[2] = center[2] + AI_TARGET_RAISE;
            ok = sees(e, ai, from, to, q->target) == 1;
        }
        if (ok) cur_score = sh_ai_position_score(d, q->ideal, 0, q->path_weight);
    }
    if (q->kind == SH_AI_POS_REPOSITION) {
        /* the far side: the ring begins opposite the demon */
        float away[3];
        away[0] = 2 * center[0] - at[0]; away[1] = 2 * center[1] - at[1]; away[2] = at[2];
        sh_ai_ring(center, away, q->ideal, count, ring);
    } else {
        sh_ai_ring(center, at, q->ideal, count, ring);
    }
    for (i = 0; i < count; i++) {
        float p[3], g[3], len = 0, score, from[3], to[3];
        int st;
        memcpy(p, ring[i], sizeof p);
        if (q->kind != SH_AI_POS_ANY && dist2(p, at) < SH_AI_POS_MOVE_MIN) continue;
        if (q->kind == SH_AI_POS_REPOSITION) {
            /* only the half of the ring away from the demon */
            double ax = (double)at[0] - center[0], ay = (double)at[1] - center[1];
            double px = (double)p[0] - center[0], py = (double)p[1] - center[1];
            if (ax * px + ay * py > 0) continue;
        }
        if (e->ground) {
            float above[3];
            above[0] = p[0]; above[1] = p[1]; above[2] = p[2] + AI_CHEST;
            if (!e->ground(above, g)) continue;   /* nothing to stand on there */
            p[2] = g[2];
        }
        if (q->visible) {
            from[0] = p[0]; from[1] = p[1]; from[2] = p[2] + head;
            to[0] = center[0]; to[1] = center[1]; to[2] = center[2] + AI_TARGET_RAISE;
            if (sees(e, ai, from, to, q->target) != 1) continue;
        }
        st = e->path(ai, p, key_base + i % SH_AI_KEY_SPAN, &len);
        if (st == 1) { searching = 1; continue; }
        if (st != 0) continue;
        score = sh_ai_position_score(dist2(p, center), q->ideal, len, q->path_weight);
        if (!have || score < best_score) { have = 1; best_score = score; memcpy(best, p, sizeof best); }
    }
    free(ring);
    if (q->kind == SH_AI_POS_ANY && cur_score < 1e29f && (!have || cur_score <= best_score)) {
        memcpy(out, at, sizeof best);
        return 0;
    }
    if (have && q->kind == SH_AI_POS_SUPERIOR && !(best_score + SH_AI_POS_MARGIN < cur_score)) have = 0;
    if (have) { memcpy(out, best, sizeof best); return 0; }
    r = searching ? 1 : 2;
    return r;
}

int sh_ai_spline_run(const sh_script_ai_engine *e, void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap,
                     int *n)
{
    float from[3];
    int count, i;
    if (n) *n = 0;
    if (!e || !ai || !q || !out || cap < 2 || !e->origin || !e->origin(ai, from)) return -1;
    count = sh_ai_spline_points(from, q->to, &q->table, q->roll, out, cap);
    if (q->nav_ground) {
        if (!e->ground) return -1;
        for (i = 0; i < count; i++) {
            float above[3], g[3];
            above[0] = out[i][0]; above[1] = out[i][1]; above[2] = out[i][2] + AI_SPLINE_RAISE;
            if (!e->ground(above, g) || fabsf(g[2] - out[i][2]) > AI_STEP * 2) return 0;
            out[i][2] = g[2];
        }
    }
    for (i = 0; i + 1 < count; i++) {
        float a[3], b[3];
        int s;
        memcpy(a, out[i], sizeof a); a[2] += AI_SPLINE_RAISE;
        memcpy(b, out[i + 1], sizeof b); b[2] += AI_SPLINE_RAISE;
        s = sees(e, ai, a, b, q->target);
        if (s < 0) return -1;
        if (!s) return 0;
    }
    if (n) *n = count;
    return 1;
}

/* SH_AI_TF_NAV_LINE: the clip's end is on the world's floor (within a step) and the straight line to it is
 * clear of world geometry at knee and chest height, actors ignored. A clip ending where it starts needs
 * neither test. 1 fits, 0 not. */
static int nav_line(const sh_script_ai_engine *e, void *ai, const float at[3], const float end[3])
{
    float above[3], g[3], from[3], to[3], h[2] = { AI_KNEE, AI_CHEST };
    float dx = end[0] - at[0], dy = end[1] - at[1];
    int k;
    if (!e->world_ground || !e->world_clear) return 0;
    if (dx * dx + dy * dy < AI_LINE_MIN * AI_LINE_MIN && fabsf(end[2] - at[2]) <= AI_STEP) return 1;
    above[0] = end[0]; above[1] = end[1]; above[2] = end[2] + AI_STEP;
    if (!e->world_ground(ai, above, g) || fabsf(g[2] - end[2]) > AI_STEP) return 0;
    for (k = 0; k < 2; k++) {
        from[0] = at[0]; from[1] = at[1]; from[2] = at[2] + h[k];
        to[0] = end[0]; to[1] = end[1]; to[2] = g[2] + h[k];
        if (e->world_clear(ai, from, to) != 1) return 0;
    }
    return 1;
}

/* Test the same world line as an animation, up to a world point or actor's feet. */
static int l_clear_walk_line(lua_State *L)
{
    float from[3], to[3];
    void *ai;
    check_target(L, 2, "clear_walk_line");
    ai = self_ai(L, "clear_walk_line", NULL);
    if (!ai || !g_engine->origin(ai, from) || !target_point(L, 2, "clear_walk_line", to, NULL)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, nav_line(g_engine, ai, from, to));
    return 1;
}

int sh_ai_anim_key(int key_base, const char *node, int index)
{
    unsigned h = 2166136261u;                                   /* FNV-1a of the node's name */
    for (; node && *node; node++) { h ^= (unsigned char)*node; h *= 16777619u; }
    return key_base + (int)((h % (SH_AI_KEY_SPAN / 64)) * 64) + (index & 63);
}

/* Mirrors DOOM Eternal's Shared_AnimationValid (0x1409FCA40): the node's animations are tried in a shuffled
 * order and the first that passes wins; `seed` sets the order (0: in order). */
int sh_ai_anim_valid_run(const sh_script_ai_engine *e, void *ai, const char *node, unsigned tests, int side,
                         unsigned seed, int key_base, int *index)
{
    float at[3], d[3], yaw = 0, head, enemy_at[3];
    void *enemy;
    int count = 0, first, last, n, i, k, pending = 0, order[64];
    if (index) *index = -1;
    if (!e || !ai || !node || !e->anim_delta || !e->origin || !e->origin(ai, at)) return -1;
    if (e->anim_delta(ai, node, 0, d, &count) < 0) return -1;
    if (count <= 0) return 0;                                   /* a node with no animation never fits */
    if (e->yaw) e->yaw(ai, &yaw);
    head = eye_height(e, ai, at);
    enemy = e->enemy ? e->enemy(ai) : NULL;
    if (enemy && !e->origin(enemy, enemy_at)) enemy = NULL;
    if ((tests & SH_AI_TF_TO_ENEMY_FORCE) && enemy)             /* the clip is played facing the enemy */
        yaw = (float)(atan2((double)enemy_at[1] - at[1], (double)enemy_at[0] - at[0]) * 180.0 / AI_PI);
    first = side >= 0 ? side % count : 0;
    last = side >= 0 ? first : count - 1;
    if (last > 63) last = 63;
    for (n = 0; n <= last - first; n++) order[n] = first + n;
    if (side < 0 && seed) {                                     /* Fisher-Yates over the seed's LCG */
        unsigned x = seed;
        for (i = n - 1; i > 0; i--) {
            int j, t;
            x = x * 1664525u + 1013904223u;
            j = (int)((x >> 8) % (unsigned)(i + 1));
            t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }
    for (k = 0; k < n; k++) {
        float end[3], c, s, from[3], to[3], g[3];
        i = order[k];
        double r = yaw * AI_PI / 180.0;
        int ok = 1;
        if (e->anim_delta(ai, node, i, d, &count) != 1) continue;
        c = (float)cos(r); s = (float)sin(r);
        end[0] = at[0] + d[0] * c - d[1] * s;
        end[1] = at[1] + d[0] * s + d[1] * c;
        end[2] = at[2] + d[2];
        if (tests & (SH_AI_TF_NAV_ENDPOINT | SH_AI_TF_CLAMP_TO_GROUND)) {
            float above[3];
            above[0] = end[0]; above[1] = end[1]; above[2] = end[2] + AI_STEP;
            if (e->ground && e->ground(above, g)) {
                if ((tests & SH_AI_TF_NAV_ENDPOINT) && fabsf(g[2] - end[2]) > AI_STEP) ok = 0;
                end[2] = g[2];
            } else if (tests & SH_AI_TF_NAV_ENDPOINT) {
                ok = 0;
            }
        }
        if (ok && (tests & SH_AI_TF_BOUNDS)) {
            float h[2] = { AI_KNEE, AI_CHEST };
            int k;
            for (k = 0; k < 2 && ok; k++) {
                from[0] = at[0]; from[1] = at[1]; from[2] = at[2] + h[k];
                to[0] = end[0]; to[1] = end[1]; to[2] = end[2] + h[k];
                if (sees(e, ai, from, to, NULL) != 1) ok = 0;
            }
        }
        if (ok && (tests & SH_AI_TF_NAV_LINE))
            ok = nav_line(e, ai, at, end);
        if (ok && (tests & (SH_AI_TF_EYE_VISIBILITY | SH_AI_TF_MUZZLE_VISIBILITY))) {
            if (!enemy) ok = 0;
            else {
                to[0] = enemy_at[0]; to[1] = enemy_at[1]; to[2] = enemy_at[2] + AI_TARGET_RAISE;
                from[0] = end[0]; from[1] = end[1];
                if (tests & SH_AI_TF_EYE_VISIBILITY) {
                    from[2] = end[2] + head;
                    if (sees(e, ai, from, to, enemy) != 1) ok = 0;
                }
                if (ok && (tests & SH_AI_TF_MUZZLE_VISIBILITY)) {
                    from[2] = end[2] + head * AI_MUZZLE_PART;
                    if (sees(e, ai, from, to, enemy) != 1) ok = 0;
                }
            }
        }
        if (ok && (tests & SH_AI_TF_AVOID_ENEMY_FOV) && enemy && e->yaw) {
            float ey;
            if (e->yaw(enemy, &ey) && fabsf(sh_ai_angle_to(enemy_at, ey, end)) < SH_AI_FOV_CENTRE_DEG) ok = 0;
        }
        if (ok && (tests & SH_AI_TF_NAV_DIRECT)) {
            float len = 0;
            int st = e->path ? e->path(ai, end, sh_ai_anim_key(key_base, node, i), &len) : -1;
            if (st == 1) { pending = 1; continue; }
            if (st != 0 || len > dist2(at, end) * 1.5f + AI_STEP) ok = 0;
        }
        if (ok) {
            if (index) *index = i;
            return 1;
        }
    }
    return pending ? 2 : 0;
}

/* ------------------------------------------------ the brain's calls: Lua */

/* The engine operation `op` of the seam, or a plain refusal naming `call`
 * when this session's engine side lacks it. */
#define NEED_OP(L, op, call) \
    do { if (g_engine && !g_engine->op) sh_script_refuse(L, "\"%s\" is not available in this session (Snapmap+ could not find " \
                                             "the game function it needs; see the backend log)", call); } while (0)

/* ai:weapon_use(name): selects the entry of that name in the behaviour decl's weaponUseBehaviorList; true when
 * selected, false when the list has no such entry. */
static int l_weapon_use(lua_State *L)
{
    const char *name;
    void *ai;
    int st;
    if (lua_type(L, 2) != LUA_TSTRING) sh_script_refuse(L, "\"weapon_use\" needs the entry's name (a string)");
    name = lua_tostring(L, 2);
    NEED_OP(L, weapon_use, "weapon_use");
    ai = self_ai(L, "weapon_use", NULL);
    st = ai ? g_engine->weapon_use(ai, name) : 0;
    if (st < 0) sh_script_refuse(L, "\"weapon_use\" could not read this demon's behaviour");
    lua_pushboolean(L, st > 0);
    return 1;
}

/* ai:recoil(alpha): the weight of the weapon's recoil animation, 0 none .. 1 full */
static int l_recoil(lua_State *L)
{
    double a = sh_script_api_check_number(L, 2, "recoil", "the recoil weight");
    void *ai;
    if (a < 0 || a > 1) sh_script_refuse(L, "\"recoil\" needs a weight from 0 (no recoil) to 1 (full), got %g", a);
    NEED_OP(L, set_recoil, "recoil");
    ai = self_ai(L, "recoil", NULL);
    if (ai) g_engine->set_recoil(ai, (float)a);
    return 0;
}

/* ai:weapon_ready([hand]) -> the weapon in `hand` may be fired again (its
 * wait between bursts has passed); nil when that hand holds no weapon */
static int l_weapon_ready(lua_State *L)
{
    void *ai;
    int st, hand = check_hand(L, 2, "weapon_ready");
    NEED_OP(L, weapon_ready, "weapon_ready");
    ai = self_ai(L, "weapon_ready", NULL);
    if (!ai || !(hand = hand_of(ai, hand)) || (st = g_engine->weapon_ready(ai, hand)) < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, st == 1);
    return 1;
}

/* ai:aim_point(pos) aims the weapon at a position; ai:aim_point(nil) stops aiming */
static int l_aim_point(lua_State *L)
{
    sh_script_handle h = 0;
    float p[3];
    int has = !lua_isnoneornil(L, 2);
    void *ai;
    if (has) check_vec(L, 2, "aim_point", p);
    NEED_OP(L, aim_point, "aim_point");
    ai = self_ai(L, "aim_point", &h);
    if (!ai || !park(ai, h)) return 0;
    if (has) g_engine->aim_point(ai, p);
    else run(ai, SH_AI_EV_CLEAR_AIM_FOCUS, NULL, 0, NULL);
    return 0;
}

/* ---- the engine's own fire control, aim focus and idle: the fire control pulls the trigger when the fire mode
 * asks and the fire check passes, holds it for a burst from the weapon decl's skillSettings, then releases it. */

/* ai:fire_mode(mode): sets the fire control's aiFireMode_t (by name, with or without AIFIREMODE_, or number)
 * with the setFireMode event; true when it ran. ai:fire_mode() reads the script-set and animation-set modes. */
static int l_fire_mode(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    int mode = -1, state = 0, anim = 0;
    sh_ai_arg a;
    if (lua_isnoneornil(L, 2)) {
        NEED_OP(L, fire_modes, "fire_mode");
        ai = self_ai(L, "fire_mode", NULL);
        if (!ai || g_engine->fire_modes(ai, &state, &anim) < 0) { lua_pushnil(L); return 1; }
        lua_pushstring(L, sh_ai_fire_mode_name(state) ? sh_ai_fire_mode_name(state) : "?");
        lua_pushstring(L, sh_ai_fire_mode_name(anim) ? sh_ai_fire_mode_name(anim) : "?");
        return 2;
    }
    if (lua_type(L, 2) == LUA_TSTRING) {
        mode = sh_ai_fire_mode_parse(lua_tostring(L, 2));
        if (mode < 0)
            sh_script_refuse(L, "\"fire_mode\" does not know the fire mode \"%s\" (the game's AIFIREMODE_ names: "
                             "HOLDFIRE, WEAPON_EXPLICIT_RELEASE, WEAPON, FIREWHENREADY, SPRAYWHENREADY)",
                             lua_tostring(L, 2));
    } else if (lua_isinteger(L, 2)) {
        lua_Integer v = lua_tointeger(L, 2);
        if (v < 0 || v >= SH_AI_FIREMODE_MAX)
            sh_script_refuse(L, "\"fire_mode\" needs a fire mode from 0 (HOLDFIRE) to 4 (SPRAYWHENREADY), got %s",
                             lua_tostring(L, 2));
        mode = (int)v;
    } else {
        sh_script_refuse(L, "\"fire_mode\" needs a fire mode name such as \"FIREWHENREADY\", got %s",
                         luaL_typename(L, 2));
    }
    ai = self_ai(L, "fire_mode", &h);
    if (!ai || !park(ai, h)) { lua_pushnil(L); return 1; }
    a = arg_i(mode);
    lua_pushboolean(L, run(ai, SH_AI_EV_SET_FIRE_MODE, &a, 1, NULL));
    return 1;
}

static int refuse_no_hand(lua_State *L, const char *call)
{
    return sh_script_refuse(L, "\"%s\": this demon's weapon hand (weaponSlot in its aibehavior decl) is not a hand; "
                            "name the hand, \"left\" or \"right\"", call);
}

/* ai:pull_trigger([check], [hand]): pulls the weapon's trigger through the fire control with the ae_aIPullTrigger
 * event; check = true runs CheckPullTrigger's fire gate first. The bare pullTrigger events skip the fire
 * control, so a WEAPON-mode demon releases without a shot. Answers whether the trigger is held (nil: no weapon). */
static int l_pull_trigger(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    int check = 0, hand, st = -1;
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) != LUA_TBOOLEAN)
            sh_script_refuse(L, "\"pull_trigger\" needs true or false for whether the game's fire check must pass "
                             "first, got %s", luaL_typename(L, 2));
        check = lua_toboolean(L, 2);
    }
    hand = check_hand(L, 3, "pull_trigger");
    ai = self_ai(L, "pull_trigger", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!(hand = hand_of(ai, hand))) return refuse_no_hand(L, "pull_trigger");
    if (!park(ai, h)) { lua_pushnil(L); return 1; }
    {
        sh_ai_arg a[3];
        a[0] = arg_anim();
        /* the slot as the equip slot parser reads it: "left_hand" or "right_hand" (EQUIP_LEFT_HAND is refused) */
        a[1] = arg_s('s', hand == SH_AI_HAND_LEFT ? "left_hand" : "right_hand");
        a[2] = arg_b(check);
        run(ai, SH_AI_EV_AE_PULL_TRIGGER, a, 3, NULL);
    }
    if (g_engine->trigger_state) st = g_engine->trigger_state(ai, hand);
    if (st < 0) lua_pushnil(L); else lua_pushboolean(L, st == 1);
    return 1;
}

/* ai:release_trigger([hand]): the engine releases the trigger of the weapon in `hand`. */
static int l_release_trigger(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    int hand = check_hand(L, 2, "release_trigger");
    ai = self_ai(L, "release_trigger", &h);
    if (!ai) return 0;
    if (!(hand = hand_of(ai, hand))) return refuse_no_hand(L, "release_trigger");
    if (!park(ai, h)) return 0;
    run(ai, hand == SH_AI_HAND_LEFT ? SH_AI_EV_RELEASE_TRIGGER_LEFT : SH_AI_EV_RELEASE_TRIGGER, NULL, 0, NULL);
    return 0;
}

/* ai:muzzle_on_target([test], [hand]) -> on_target, info. Runs the engine's muzzle test (a ray from the muzzle
 * tag against a sphere at the aim focus) with radius weight "pull" (default), "firing" or "plain"; info holds
 * the focus state it reads. on_target is nil when the test cannot be called in this session. */
static int l_muzzle_on_target(lua_State *L)
{
    static const char *const precision[3] = { "NORMAL", "PRECISE", "MAX" };
    void *ai;
    sh_ai_focus_state fs;
    int mode = SH_AI_MUZZLE_PULL, hand, on = -1;
    if (!lua_isnoneornil(L, 2)) {
        mode = lua_type(L, 2) == LUA_TSTRING ? sh_ai_muzzle_mode_parse(lua_tostring(L, 2)) : -1;
        if (mode < 0)
            sh_script_refuse(L, "\"muzzle_on_target\" needs the test as \"pull\", \"firing\" or \"plain\", got %s",
                             lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : luaL_typename(L, 2));
    }
    hand = check_hand(L, 3, "muzzle_on_target");
    NEED_OP(L, focus_state, "muzzle_on_target");
    ai = self_ai(L, "muzzle_on_target", NULL);
    memset(&fs, 0, sizeof fs);
    if (!ai || g_engine->focus_state(ai, &fs) < 0) { lua_pushnil(L); return 1; }
    hand = hand_of(ai, hand);
    if (g_engine->muzzle_test && hand) on = g_engine->muzzle_test(ai, hand, mode);
    if (on < 0) lua_pushnil(L); else lua_pushboolean(L, on == 1);
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, fs.valid); lua_setfield(L, -2, "focus_valid");
    push_vec(L, fs.point); lua_setfield(L, -2, "point");
    lua_pushnumber(L, fs.radius); lua_setfield(L, -2, "radius");
    lua_pushstring(L, fs.precision >= 0 && fs.precision < 3 ? precision[fs.precision] : "?");
    lua_setfield(L, -2, "precision");
    if (fs.require_muzzle >= 0) { lua_pushboolean(L, fs.require_muzzle); lua_setfield(L, -2, "require_muzzle"); }
    lua_pushboolean(L, fs.tracking); lua_setfield(L, -2, "tracking");
    lua_pushboolean(L, fs.body_rotation); lua_setfield(L, -2, "body_rotation");
    return 2;
}

/* ai:focus(target, [aim_point]): sets the aim focus the fire gate needs (without one the muzzle test fails).
 * An entity is tracked at the named aimPoint_t (default CENTER); a position is a fixed aim point; nil clears
 * it. An entity failing focus_entity_ok clears the focus and answers false, else true. */
static int l_focus(lua_State *L)
{
    sh_script_handle h = 0;
    float p[3];
    void *ai, *ent = NULL;
    ai_rec *r;
    int kind = 0, aim = SH_AI_AIMPOINT_CENTER;
    if (!lua_isnoneornil(L, 3)) {
        const char *n;
        int i;
        if (lua_type(L, 3) != LUA_TSTRING)
            sh_script_refuse(L, "\"focus\" needs an aim point name, as \"BEST\", got %s", luaL_typename(L, 3));
        n = lua_tostring(L, 3);
        if (!_strnicmp(n, "AIMPOINT_", 9)) n += 9;
        for (aim = -1, i = 0; i < 13; i++) if (!_stricmp(n, SH_AI_AIMPOINT_NAMES[i])) aim = i;
        if (aim < 0) sh_script_refuse(L, "\"focus\" does not know the aim point \"%s\"", lua_tostring(L, 3));
    }
    if (!lua_isnoneornil(L, 2)) {
        if (is_entity(L, 2)) { sh_script_check_entity(L, 2, "focus"); kind = 1; }
        else if (lua_type(L, 2) == LUA_TTABLE) { check_vec(L, 2, "focus", p); kind = 2; }
        else sh_script_refuse(L, "\"focus\" needs an entity, a position or nil, got %s", luaL_typename(L, 2));
    }
    if (kind == 2) NEED_OP(L, aim_point, "focus");
    ai = self_ai(L, "focus", &h);
    if (!ai) return 0;
    if (kind == 1 && !(ent = other_entity(L, 2, "focus"))) return 0;
    if (!(r = park(ai, h))) return 0;
    if (kind == 1 && !focus_entity_ok(ai, ent)) {
        focus_refused(L, r, ai, ent, "focus");
        run(ai, SH_AI_EV_CLEAR_AIM_FOCUS, NULL, 0, NULL);
        lua_pushboolean(L, 0);
        return 1;
    }
    if (kind == 1) {
        sh_ai_arg a[4];
        a[0] = arg_e(ent); a[1] = arg_b(1); a[2] = arg_b(1); a[3] = arg_i(aim);
        run(ai, SH_AI_EV_SET_FOCUS, a, 4, NULL);
    } else if (kind == 2) {
        g_engine->aim_point(ai, p);
    } else {
        run(ai, SH_AI_EV_CLEAR_AIM_FOCUS, NULL, 0, NULL);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ai:hold([turn]): ends the pending move with ClearMove and orders the engine's idle with a turn type
 * ("RELAXED", "COMBAT" default, "INCOVER"), so the body turns toward the aim focus past the tracker's yaw
 * limit. The idle has no end (time -1): a timed idle of 0 expires at its first update and never turns. Answers
 * true when a turnParms entry for the type exists and body rotation is on, else false and which fails. */
static int l_hold(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    ai_rec *r;
    sh_ai_arg a[2];
    sh_ai_focus_state fs;
    sh_ai_step_state st;
    int turn = SH_AI_IDLETURN_COMBAT, ran, entry = -1, body = 1;
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) == LUA_TSTRING) turn = sh_ai_idle_turn_parse(lua_tostring(L, 2));
        else turn = -1;
        if (turn < 0)
            sh_script_refuse(L, "\"hold\" needs the turn as \"RELAXED\", \"COMBAT\" or \"INCOVER\" (the game's "
                             "AIIDLETURN_ names), got %s",
                             lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : luaL_typename(L, 2));
    }
    ai = self_ai(L, "hold", &h);
    if (!ai || !(r = park(ai, h))) { lua_pushnil(L); return 1; }
    if (step_in_flight(ai, &st) && st.idle_turn == turn) {
        /* held with this turn type already and its turn in place is in flight: the order stands */
        ran = 1;
    } else {
        if (g_engine->clear_move) g_engine->clear_move(ai);
        a[0] = arg_i(-1); a[1] = arg_i(turn);
        ran = run(ai, SH_AI_EV_IDLE, a, 2, NULL);
    }
    if (ran) {
        r->move.state = SH_AI_MOVE_NONE;
        r->has_dest = 0;
        r->facing = 0;
    }
    if (g_engine->idle_turn_entry) entry = g_engine->idle_turn_entry(ai, turn);
    if (g_engine->focus_state) {
        memset(&fs, 0, sizeof fs);
        if (g_engine->focus_state(ai, &fs) >= 0) body = fs.body_rotation != 0;
    }
    if (entry == 0 && !(r->turn_noted & (1u << turn))) {
        char line[512];
        r->turn_noted |= 1u << turn;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "\"hold\": the demon's aibehavior decl has no turnParms entry for %s, so the game stands it "
                    "still but never turns it in place; add one (turnParms item[%d]) for it to turn.",
                    sh_ai_idle_turn_name(turn), turn);
        shs_log(shs_mod_of(L), line);
    }
    lua_pushboolean(L, ran && entry != 0 && body);
    if (ran && entry != 0 && body) return 1;
    lua_pushstring(L, !ran ? "the idle order did not run"
                           : entry == 0 ? "the demon's aibehavior decl has no turnParms entry for that turn"
                                        : "body rotation is off");
    return 2;
}

/* ai:tracking([on]) and ai:body_rotation([on]): the head and aim tracking switch (the aim point is valid only
 * while on) and the body rotation switch (turn in place needs it), via enableHeadTracking and
 * enableBodyRotation. With no argument they answer the switch. */
static int switch_call(lua_State *L, const char *call, int ev, int body)
{
    sh_script_handle h = 0;
    void *ai;
    sh_ai_focus_state fs;
    sh_ai_arg a;
    if (lua_isnoneornil(L, 2)) {
        NEED_OP(L, focus_state, call);
        ai = self_ai(L, call, NULL);
        memset(&fs, 0, sizeof fs);
        if (!ai || g_engine->focus_state(ai, &fs) < 0) { lua_pushnil(L); return 1; }
        lua_pushboolean(L, body ? fs.body_rotation : fs.tracking);
        return 1;
    }
    if (lua_type(L, 2) != LUA_TBOOLEAN)
        sh_script_refuse(L, "\"%s\" needs true or false, got %s", call, luaL_typename(L, 2));
    ai = self_ai(L, call, &h);
    if (!ai || !park(ai, h)) { lua_pushnil(L); return 1; }
    a = arg_b(lua_toboolean(L, 2));
    lua_pushboolean(L, run(ai, ev, &a, 1, NULL));
    return 1;
}
static int l_tracking(lua_State *L) { return switch_call(L, "tracking", SH_AI_EV_ENABLE_HEAD_TRACKING, 0); }
static int l_body_rotation(lua_State *L) { return switch_call(L, "body_rotation", SH_AI_EV_ENABLE_BODY_ROTATION, 1); }

/* ai:laser_add(tag, [ribbon]): gives the laser targeter a laser on model tag `tag`, drawn as a ribbon (the
 * aicomponent_lasertargeter decl's laserPinpoint_ribbonDecl, or `ribbon`) from the tag to what it hits. The
 * game makes laser list entries only in two stock state classes, so other demons have none. It starts off;
 * answers true, or false and why. */
static int l_laser_add(lua_State *L)
{
    const char *tag = check_tag(L, 2, "laser_add", "a model tag");
    const char *ribbon = NULL;
    void *ai;
    int st;
    if (!lua_isnoneornil(L, 3)) ribbon = sh_script_check_name(L, 3, "laser_add", 1, NULL);
    NEED_OP(L, laser_add, "laser_add");
    ai = self_ai(L, "laser_add", NULL);
    st = ai ? g_engine->laser_add(ai, tag, ribbon) : -1;
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, st == 0 ? "the demon has no laser targeter (its entityDef names no aicomponent_lasertargeter decl)"
                     : st == -2 ? "no ribbon decl of that name is loaded"
                                : "the game's laser list could not be reached");
    return 2;
}

/* ai:laser_remove(tag): take the laser on `tag` away again. */
static int l_laser_remove(lua_State *L)
{
    const char *tag = check_tag(L, 2, "laser_remove", "a model tag");
    void *ai;
    NEED_OP(L, laser_remove, "laser_remove");
    ai = self_ai(L, "laser_remove", NULL);
    lua_pushboolean(L, ai && g_engine->laser_remove(ai, tag) == 1);
    return 1;
}

/* ai:rear_footsteps(front, rear): plays footstep table `rear` instead of `front` for ae_leftRearFoot and
 * ae_rightRearFoot (the game plays one table per gait for all four feet). Both are projectileImpactEffect
 * decls; call it once per gait table. Answers true, or false and why. */
static int l_rear_footsteps(lua_State *L)
{
    const char *front, *rear;
    char *keep;
    void *ai;
    int st;
    front = sh_script_check_name(L, 2, "rear_footsteps", 1, NULL);
    keep = _strdup(front);                 /* the next name may reuse the buffer */
    rear = sh_script_check_name(L, 3, "rear_footsteps", 1, NULL);
    if (g_engine && !g_engine->rear_footsteps) {
        free(keep);
        NEED_OP(L, rear_footsteps, "rear_footsteps");
    }
    ai = self_ai(L, "rear_footsteps", NULL);
    st = (ai && keep) ? g_engine->rear_footsteps(ai, keep, rear) : -1;
    free(keep);
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, st == -2 ? "one of the two footstep tables is not a loaded projectileImpactEffect decl"
                               : "the game's footstep table choice could not be reached");
    return 2;
}

/* ai:held([hand]) -> the decl of the item held in `hand` (the mod's own name, as equip takes it), or nil when
 * empty. It asks the engine because a clip's ae_equipInventoryItem can change the item. */
static int l_held(lua_State *L)
{
    char decl[512];
    void *ai;
    int hand = check_hand(L, 2, "held"), r;
    NEED_OP(L, held, "held");
    ai = self_ai(L, "held", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    if (!(hand = hand_of(ai, hand))) return refuse_no_hand(L, "held");
    decl[0] = 0;
    r = g_engine->held(ai, hand, decl, sizeof decl);
    if (r == 1 && decl[0]) lua_pushstring(L, own_name(L, decl));
    else lua_pushnil(L);
    return 1;
}

/* ai:posed_legs(): for a demon whose clips pose thigh, shin and foot themselves (as Eternal's do), takes the
 * leg rig's targets and poles every frame from the blended clips' legs before the game grounds them, so added
 * layers carry through and feet still follow slopes. Call once after spawn; answers true, or false and why. */
static int l_posed_legs(lua_State *L)
{
    void *ai;
    int st;
    NEED_OP(L, posed_legs, "posed_legs");
    ai = self_ai(L, "posed_legs", NULL);
    st = ai ? g_engine->posed_legs(ai) : -1;
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, st == 0 ? "the demon's model has no leg rig (its skeleton lacks the rig's joints)"
                              : "the game's leg rig could not be reached");
    return 2;
}

/* ai:fk_reference_pose(): the fk effectors take their reference pose from the poseReferenceAnim their fkGraph
 * names, as Eternal's do, instead of the web's current leaf (which makes head and cannon jump when a blend's
 * new clip turns heavier). Call once after spawn; answers true, or false and why. */
static int l_fk_reference_pose(lua_State *L)
{
    void *ai;
    int st;
    NEED_OP(L, fk_reference_pose, "fk_reference_pose");
    ai = self_ai(L, "fk_reference_pose", NULL);
    st = ai ? g_engine->fk_reference_pose(ai) : -1;
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, "the game's fk reference pose could not be reached");
    return 2;
}

/* ai:wound_damage_counts(true|false): with true, each gore wound opens on the damage that landed in its own
 * clipping spheres (a per-wound counter against its threshold), where the game counts the whole damage
 * group's lost health (script_api_ai.h, "Wound damage counts"). false keeps the game's rule and the counts. */
static int l_wound_damage_counts(lua_State *L)
{
    void *ai;
    int st;
    if (lua_type(L, 2) != LUA_TBOOLEAN)
        sh_script_refuse(L, "\"wound_damage_counts\" needs true or false, got %s", luaL_typename(L, 2));
    NEED_OP(L, wound_damage_counts, "wound_damage_counts");
    ai = self_ai(L, "wound_damage_counts", NULL);
    st = ai ? g_engine->wound_damage_counts(ai, lua_toboolean(L, 2)) : -1;
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, st == 0 ? "too many demons count their wounds' damage in this map"
                              : "the game's wound test could not be reached");
    return 2;
}

/* ai:eternal_interrupts(): the demon's animation orders carry Eternal's interrupt mode (sh_ai_order_interrupt),
 * so a change and a forced via nest over a running blend instead of waiting or jumping the pose. Nodes with
 * AW_CF_USE_MULTIPLE_INTERRUPT nest in either mode. Call once after spawn; answers true, or false and why. */
static int l_eternal_interrupts(lua_State *L)
{
    sh_script_handle h = 0;
    void *ai;
    ai_rec *r;
    NEED_OP(L, interrupt_blend, "eternal_interrupts");
    ai = self_ai(L, "eternal_interrupts", &h);
    if (!ai || !(r = park(ai, h))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the demon could not be taken over");
        return 2;
    }
    r->eternal_interrupts = 1;
    lua_pushboolean(L, 1);
    return 1;
}

/* Whether a non-demon entity has a gore component wound_hides can serve (an idGoreEntity of a mod). Set by
 * script_api_body.c; NULL keeps wound_hides to demons. */
static int (*g_gore_owner_test)(void *ent);
void sh_script_ai_set_gore_owner_test(int (*fn)(void *ent)) { g_gore_owner_test = fn; }

/* ai:wound_hides(wound, { mesh, ... }): hides the named meshes of the model, as a damage group's armour
 * hideMeshes does, when the game shows the gorewounds decl `wound` (which has no hide list of its own).
 * Call once per wound after spawn; answers true, or false and why. */
static int l_wound_hides(lua_State *L)
{
    sh_script_handle h = (sh_script_handle)sh_script_check_entity(L, 1, "wound_hides");
    const char *wound = sh_script_check_name(L, 2, "wound_hides", 1, NULL);
    const char **names;
    lua_Unsigned count;
    void *ai;
    int st, i;

    NEED_OP(L, wound_hides, "wound_hides");
    if (!g_engine)
        sh_script_refuse(L, "\"wound_hides\" is not available in this session (Snapmap+ could "
                            "not find the game's AI functions; see the backend log)");
    if (lua_type(L, 3) != LUA_TTABLE)
        return luaL_error(L, "\"wound_hides\" needs a list of mesh names here, got %s", luaL_typename(L, 3));
    wound = lua_pushstring(L, wound);
    count = lua_rawlen(L, 3);
    if (count > INT_MAX - 2 || count > SIZE_MAX / sizeof *names)
        return luaL_error(L, "\"wound_hides\" mesh list is too large");
    luaL_checkstack(L, (int)count + 2, "wound_hides mesh list");
    names = (const char **)lua_newuserdatauv(L, (size_t)count * sizeof *names, 0);
    for (i = 0; i < (int)count; i++) {
        lua_rawgeti(L, 3, i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "\"wound_hides\" needs mesh names in its list, got %s at %d", luaL_typename(L, -1), i + 1);
        names[i] = lua_tostring(L, -1);
    }
    ai = h ? g_engine->entity(h) : NULL;
    if (ai && !g_engine->is_ai(ai) && !(g_gore_owner_test && g_gore_owner_test(ai))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "this entity is neither a demon nor an entity with a gore component");
        return 2;
    }
    st = ai ? g_engine->wound_hides(ai, wound, names, (int)count) : -1;
    lua_pushboolean(L, st == 1);
    if (st == 1) return 1;
    lua_pushstring(L, st == -2 ? "the wound is not a loaded gorewounds decl"
                    : st == -3 ? "the demon's model has no mesh of one of these names"
                               : "the game's wound display could not be reached");
    return 2;
}

static int l_wound_effect(lua_State *L)
{
    void *ent = sh_script_api_owned(L, 1, "wound_effect", "register wound effects on", NULL);
    const char *condition = sh_script_api_check_text(L, 3, "wound_effect", "an effect condition", SH_SCRIPT_TEXT_ANY);
    const char *wound = sh_script_check_name(L, 2, "wound_effect", 1, NULL);
    int result;
    NEED_OP(L, wound_effect, "wound_effect");
    if (!g_engine) return sh_script_refuse(L, "\"wound_effect\" is unavailable in this session");
    if (ent && !g_engine->is_ai(ent) && !(g_gore_owner_test && g_gore_owner_test(ent))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "this entity has no gore component");
        return 2;
    }
    result = ent ? g_engine->wound_effect(ent, wound, condition) : -1;
    lua_pushboolean(L, result == 1);
    if (result == 1) return 1;
    lua_pushstring(L, result == -2 ? "the wound is not a loaded gorewounds decl" :
                      result == -3 ? "the effect condition is unavailable" : "the game's wound display could not be reached");
    return 2;
}

static const char *check_node(lua_State *L, int idx, const char *call, const char *what)
{
    size_t n = 0, i;
    const char *s = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &n) : NULL;
    if (!s || !n)
        sh_script_refuse(L, "\"%s\" needs %s as \"subweb/state\" text, got %s", call, what,
                         lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '_' || s[i] == '/' || s[i] == '.'))
            sh_script_refuse(L, "\"%s\": \"%s\" is not an animWeb node name (letters, digits, _ / and . only)", call, s);
    return s;
}

/* ai:animation_fits(node, tests, [side], [seed]) -> true, index | false | nil, "searching". Tests the node's
 * animations' root motion, turned by the demon's facing, with the TF_* words of `tests` (see k_tests); `side`
 * picks one animation, else all are tried in order or shuffled by `seed` (above 0). */
static int l_anim_valid(lua_State *L)
{
    sh_script_handle h = 0;
    const char *node = check_node(L, 2, "animation_fits", "the animWeb node");
    const char *words = lua_isnoneornil(L, 3) ? "VALIDATE_ANIM_DELTA" : NULL;
    char bad[64];
    unsigned tests;
    int side = -1, st, index = -1;
    unsigned seed = 0;
    void *ai;
    ai_rec *r;
    if (!words) {
        if (lua_type(L, 3) != LUA_TSTRING)
            sh_script_refuse(L, "\"animation_fits\" needs its tests as words, as \"TF_BOUNDS TF_NAV_ENDPOINT\", got %s",
                             luaL_typename(L, 3));
        words = lua_tostring(L, 3);
    }
    tests = sh_ai_tests_parse(words, bad, sizeof bad);
    if (!tests && bad[0])
        sh_script_refuse(L, "\"animation_fits\" does not know the test \"%s\"; use TF_NAV_DIRECT, TF_NAV_ENDPOINT, "
                         "TF_CLAMP_TO_GROUND, TF_BOUNDS, TF_EYE_VISIBILITY, TF_MUZZLE_VISIBILITY, "
                         "TF_TRANSLATION_TO_ENEMY_FORCE, TF_AVOID_ENEMY_FOV or VALIDATE_ANIM_DELTA", bad);
    if (!lua_isnoneornil(L, 4)) {
        if (!lua_isinteger(L, 4) || lua_tointeger(L, 4) < 0 || lua_tointeger(L, 4) > 63)
            sh_script_refuse(L, "\"animation_fits\" needs the animation to try as a whole number from 0 to 63");
        side = (int)lua_tointeger(L, 4);
    }
    if (!lua_isnoneornil(L, 5)) {
        if (!lua_isinteger(L, 5) || lua_tointeger(L, 5) < 1 || lua_tointeger(L, 5) > 0x7FFFFFFF)
            sh_script_refuse(L, "\"animation_fits\" needs its order seed as a whole number from 1 to 2147483647");
        seed = (unsigned)lua_tointeger(L, 5);
    }
    NEED_OP(L, anim_valid, "animation_fits");
    ai = self_ai(L, "animation_fits", &h);
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    st = g_engine->anim_valid(ai, node, tests, side, seed, &index);
    if (st == 1) { lua_pushboolean(L, 1); lua_pushinteger(L, index); return 2; }
    if (st == 0) { lua_pushboolean(L, 0); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, st == 2 ? "searching" : "no such node");
    return 2;
}

/* ------------------------------------------- forced animation through a via */

#define AI_FORCE_VIA_FRAMES 600   /* a via node not seen this many frames is given up */
typedef struct ai_force_via {
    sh_script_handle handle;
    char *dest;              /* malloc'd node names */
    char *via;
    uint32_t issued;
    int asked;               /* the destination was asked of the web */
} ai_force_via;
/* One per demon with a forced via standing; the list grows. Game thread. */
static ai_force_via *g_force_via;
static int g_nforce, g_cforce;

static void force_via_drop(int i)
{
    free(g_force_via[i].dest);
    free(g_force_via[i].via);
    g_force_via[i] = g_force_via[--g_nforce];
}

static void force_via_reset(void) { while (g_nforce) force_via_drop(g_nforce - 1); }

/* Every frame: once a forced via node plays, the web is asked for the destination (ssaction_ChangeAnimState)
 * and goes there when the clip ends (sh_ai_force_via_next). */
static uint32_t g_seen_frame;

static void force_via_step(void)
{
    int i;
    uint32_t frame;
    if (!g_engine) return;
    /* the frame count restarts with each map, which drops the per-demon records */
    frame = g_engine->frame();
    if (frame < g_seen_frame) {
        sh_ai_pain_records_reset();
        sh_wound_count_reset();
        force_via_reset();
    }
    g_seen_frame = frame;
    for (i = 0; i < g_nforce; i++) {
        ai_force_via *f = &g_force_via[i];
        void *ai;
        int requested = 0;
        unsigned flags;
        char *dest = sh_script_api_anim_dest_of(f->handle);
        ai = g_engine->entity(f->handle);
        if (!ai || !dest || _stricmp(dest, f->dest)) {
            force_via_drop(i--);                      /* gone, or another animation was asked for since */
            continue;
        }
        flags = sh_script_api_anim_flags(f->handle, ai, &requested);
        switch (sh_ai_force_via_next(flags, f->asked, g_engine->frame() - f->issued, AI_FORCE_VIA_FRAMES)) {
        case SH_AI_FORCE_VIA_DONE:
            force_via_drop(i--);
            break;
        case SH_AI_FORCE_VIA_ASK_DEST:
            /* the forced node plays: asking for the destination now lets the web leave it when the clip ends */
            f->asked = 1;
            (void)sh_script_ai_anim(ai, f->handle, f->dest, NULL, SH_AI_ORDER_VIA_DEST, 0);
            break;
        default:
            break;
        }
    }
}

/* ai:force_animation_via(dest, via, [blend]): plays `via` now whatever the web is doing (blending `blend`
 * seconds), then goes to `dest`; animation_event reports VIA_BLEND_IN_START, VIA_BLEND_OUT_START, DEST_REACHED. */
static int l_on_ground(lua_State *L)
{
    void *ai = self_ai(L, "on_ground", NULL);
    int value = ai && g_engine->on_ground ? g_engine->on_ground(ai) : -1;
    if (value < 0) lua_pushnil(L);
    else lua_pushboolean(L, value);
    return 1;
}

static int l_reset_motion_correction(lua_State *L)
{
    void *ai = self_ai(L, "reset_motion_correction", NULL);
    if (!ai) { lua_pushboolean(L, 0); return 1; }
    if (!g_engine->reset_motion_correction)
        return sh_script_refuse(L, "The game's animation correction reset is unavailable in this session");
    lua_pushboolean(L, g_engine->reset_motion_correction(ai));
    return 1;
}

static int l_anim_force_via(lua_State *L)
{
    sh_script_handle h = 0;
    const char *dest = check_node(L, 2, "force_animation_via", "the node to end in");
    const char *via = check_node(L, 3, "force_animation_via", "the node to force first");
    double blend = 0;
    void *ai;
    int i, slot = -1;
    char *d, *v;
    if (sh_script_api_opt_number(L, 4, "force_animation_via", "the blend time in seconds", &blend) && !(blend >= 0))
        sh_script_refuse(L, "\"force_animation_via\" needs a blend time of 0 seconds or more, got %g", blend);
    ai = self_ai(L, "force_animation_via", &h);
    if (!ai) return 0;
    for (i = 0; i < g_nforce; i++) if (g_force_via[i].handle == h) { slot = i; break; }
    sh_script_api_anim_request(h, dest, via);
    if (!sh_script_ai_anim(ai, h, via, NULL, SH_AI_ORDER_FORCE_VIA, (int)(blend * 1000.0 + 0.5))) return 0;
    d = _strdup(dest);
    v = _strdup(via);
    if (!d || !v) { free(d); free(v); return luaL_error(L, "not enough memory"); }
    if (slot < 0) {
        if (g_nforce == g_cforce) {
            int cap = g_cforce ? g_cforce * 2 : 16;
            ai_force_via *grown = (ai_force_via *)realloc(g_force_via, (size_t)cap * sizeof *grown);
            if (!grown) { free(d); free(v); return luaL_error(L, "not enough memory"); }
            g_force_via = grown;
            g_cforce = cap;
        }
        slot = g_nforce++;
    } else {
        free(g_force_via[slot].dest);
        free(g_force_via[slot].via);
    }
    memset(&g_force_via[slot], 0, sizeof g_force_via[slot]);
    g_force_via[slot].handle = h;
    g_force_via[slot].dest = d;
    g_force_via[slot].via = v;
    g_force_via[slot].issued = g_engine->frame();
    return 0;
}

/* ----------------------------------------------------- places and routes */

static void push_points(lua_State *L, float (*p)[3], int n)
{
    int i;
    lua_createtable(L, n, 0);
    for (i = 0; i < n; i++) { push_vec(L, p[i]); lua_rawseti(L, -2, i + 1); }
}

/* ai:path_points(pos[, with_length]) -> the navigation path and optionally its measured length,
 * nil, "searching" while the engine finds it, nil, "no path" when there is none. */
static int l_path_points(lua_State *L)
{
    sh_script_handle h = 0;
    float goal[3], length = 0, (*pts)[3] = NULL, (*held)[3];
    void *ai;
    ai_rec *r;
    int st, n = 0, with_length = lua_toboolean(L, 3);
    check_target(L, 2, "path_points");
    NEED_OP(L, path_points, "path_points");
    ai = self_ai(L, "path_points", &h);
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (!target_point(L, 2, "path_points", goal, NULL)) { lua_pushnil(L); lua_pushstring(L, "no path"); return 2; }
    st = g_engine->path_points(ai, goal, 0x50000000 | ((rec_index(r) & 0xFFF) << 14), &pts, &n, &length);
    if (st == 0) {
        /* every point of the path, held by the collector while they are pushed */
        held = (float (*)[3])lua_newuserdatauv(L, (size_t)(n > 0 ? n : 1) * sizeof *held, 0);
        if (n > 0 && pts) memcpy(held, pts, (size_t)n * sizeof *held);
        free(pts);
        if (n <= 0) { memcpy(held[0], goal, sizeof held[0]); n = 1; }   /* already there: the goal alone */
        push_points(L, held, n);
        if (with_length) { lua_pushnumber(L, length); return 2; }
        return 1;
    }
    free(pts);
    lua_pushnil(L);
    lua_pushstring(L, st == 1 ? "searching" : st == 2 || st == 0 ? "no path" : "unavailable");
    return 2;
}

/* ai:firing_position(parms) -> pos | nil, "searching" | nil, "none". parms: target (default its enemy), kind
 * ("any", "new", "superior", "reposition"), min, max, ideal, visible (default true), count, path_weight and
 * parms (carried, not read). */
static int l_firing_position(lua_State *L)
{
    sh_script_handle h = 0;
    sh_ai_position_query q;
    float out[3];
    void *ai;
    ai_rec *r;
    int st, has_target;
    memset(&q, 0, sizeof q);
    if (lua_type(L, 2) != LUA_TTABLE)
        sh_script_refuse(L, "\"firing_position\" takes a table of settings, as { kind = \"new\", ideal = 600 }, got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        static const char *const known[] = { "target", "kind", "min", "max", "ideal", "visible", "count",
                                             "path_weight", "parms", NULL };
        int j, ok = 0;
        for (j = 0; k && known[j]; j++) if (!strcmp(k, known[j])) ok = 1;
        if (!ok)
            sh_script_refuse(L, "\"firing_position\" knows target, kind, min, max, ideal, visible, count, path_weight "
                             "and parms, not \"%s\"", k ? k : "?");
        lua_pop(L, 1);
    }
    lua_getfield(L, 2, "kind");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING || (q.kind = sh_ai_position_kind(lua_tostring(L, -1))) < 0)
            sh_script_refuse(L, "\"firing_position\": kind is \"any\", \"new\", \"superior\" or \"reposition\"");
    }
    lua_pop(L, 1);
    q.min = opt_num_field(L, 2, "min", 0, "firing_position", 0, AI_ANY);
    q.max = opt_num_field(L, 2, "max", 2048, "firing_position", 0, AI_ANY);
    if (q.max < q.min) sh_script_refuse(L, "\"firing_position\": max must not be less than min");
    q.ideal = opt_num_field(L, 2, "ideal", (q.min + q.max) / 2, "firing_position", q.min, q.max);
    q.count = (int)opt_num_field(L, 2, "count", 8, "firing_position", 2, 2147483520.0f);
    q.path_weight = opt_num_field(L, 2, "path_weight", 0.5f, "firing_position", 0, AI_ANY);
    lua_getfield(L, 2, "visible");
    q.visible = lua_isnil(L, -1) ? 1 : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "target");
    has_target = !lua_isnil(L, -1);
    if (has_target && !is_entity(L, -1) && lua_type(L, -1) != LUA_TTABLE)
        sh_script_refuse(L, "\"firing_position\": target must be an entity or a position");
    if (has_target && lua_type(L, -1) == LUA_TTABLE) check_vec(L, lua_gettop(L), "firing_position", q.at);
    NEED_OP(L, firing_position, "firing_position");
    ai = self_ai(L, "firing_position", &h);
    if (!ai || !(r = rec_of(h))) { lua_pushnil(L); return 1; }
    if (has_target && is_entity(L, -1)) {
        if (!target_point(L, lua_gettop(L), "firing_position", q.at, &q.target)) {
            lua_pushnil(L); lua_pushstring(L, "none"); return 2;
        }
    } else if (!has_target) {
        q.target = g_engine->enemy(ai);
        if (!q.target || !g_engine->origin(q.target, q.at)) { lua_pushnil(L); lua_pushstring(L, "none"); return 2; }
    }
    st = g_engine->firing_position(ai, &q, out);
    if (st == 0) { push_vec(L, out); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, st == 1 ? "searching" : st == 2 ? "none" : "unavailable");
    return 2;
}

/* The length of the array a Lua list field holds (its [1], [2], ... or "times[1]" keys), -1 when not a table. */
static int list_len(lua_State *L, int idx, const char *field, const char *prefix)
{
    int n = 0;
    lua_getfield(L, idx, field);
    if (lua_type(L, -1) != LUA_TTABLE) { lua_pop(L, 1); return -1; }
    for (;;) {
        char key[48];
        int none;
        lua_rawgeti(L, -1, n + 1);
        none = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (none) {
            _snprintf_s(key, sizeof key, _TRUNCATE, "%s[%d]", prefix, n + 1);
            lua_getfield(L, -1, key);
            none = lua_isnil(L, -1);
            lua_pop(L, 1);
        }
        if (none) break;
        n++;
    }
    lua_pop(L, 1);
    return n;
}

/* A control table's knots from { times = {...}, values = {...} } (arrays, or "times[1]" keys). A value missing
 * for the last time is 0. Reads `cap` at most. */
static int read_list(lua_State *L, int idx, const char *field, const char *prefix, float *out, int cap)
{
    int n = 0, i;
    lua_getfield(L, idx, field);
    if (lua_type(L, -1) != LUA_TTABLE) { lua_pop(L, 1); return -1; }
    for (i = 1; i <= cap; i++) {
        char key[48];
        int isnum = 0;
        lua_Number v;
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            _snprintf_s(key, sizeof key, _TRUNCATE, "%s[%d]", prefix, i);
            lua_getfield(L, -1, key);
        }
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        v = lua_tonumberx(L, -1, &isnum);
        lua_pop(L, 1);
        if (!isnum || v != v) { lua_pop(L, 1); return -2; }
        out[n++] = (float)v;
    }
    lua_pop(L, 1);
    return n;
}

/* Knots as a list { {t = 0.2, v = 0.1}, ... } (as a brain keeps its control
 * tables) into the table (room for every one); the count, or -1 when the
 * list is not knots. */
static int read_knots(lua_State *L, int idx, sh_ai_table *tb)
{
    int n = 0, i, cap = (int)lua_rawlen(L, idx);
    for (i = 1; i <= cap; i++) {
        int t_ok = 0, v_ok = 0;
        lua_Number t, v;
        if (lua_rawgeti(L, idx, i) == LUA_TNIL) { lua_pop(L, 1); break; }
        if (lua_type(L, -1) != LUA_TTABLE) { lua_pop(L, 1); return -1; }
        lua_getfield(L, -1, "t"); t = lua_tonumberx(L, -1, &t_ok); lua_pop(L, 1);
        lua_getfield(L, -1, "v"); v = lua_tonumberx(L, -1, &v_ok); lua_pop(L, 1);
        lua_pop(L, 1);
        if (!t_ok || t != t) return -1;
        tb->t[n] = (float)t;
        tb->v[n] = v_ok && v == v ? (float)v : 0.0f;
        n++;
    }
    return n;
}

/* The spline at idx into q; its knot arrays are a userdata left on the
 * stack (the collector frees them after the call). */
static void check_spline(lua_State *L, int idx, sh_ai_spline_query *q)
{
    int tbl, field, nt, nv, i, room;
    float *v, *mem;
    if (lua_type(L, idx) != LUA_TTABLE)
        sh_script_refuse(L, "\"spline_clear\" needs the spline as a table { times = {...}, values = {...}, roll = 90 }, "
                         "got %s", lua_isnoneornil(L, idx) ? "nothing" : luaL_typename(L, idx));
    lua_getfield(L, idx, "table");
    field = lua_gettop(L);
    tbl = lua_type(L, -1) == LUA_TTABLE ? field : idx;
    if (lua_rawgeti(L, tbl, 1) == LUA_TTABLE) {
        lua_pop(L, 1);
        room = (int)lua_rawlen(L, tbl);
        mem = (float *)lua_newuserdatauv(L, (size_t)(room > 0 ? room : 1) * 2 * sizeof(float), 0);
        lua_insert(L, field);                 /* below the table field: kept to the end of the call */
        if (tbl == field) tbl++;
        field++;
        q->table.t = mem;
        q->table.v = mem + (room > 0 ? room : 1);
        nt = read_knots(L, tbl, &q->table);
        if (nt < 1)
            sh_script_refuse(L, "\"spline_clear\" needs the spline's knots as { {t = 0.5, v = 0.2}, ... } (t a number)");
    } else {
        int lt, lv;
        lua_pop(L, 1);
        lt = list_len(L, tbl, "times", "times");
        lv = list_len(L, tbl, "values", "values");
        room = (lt > 0 ? lt : 1) + (lv > 0 ? lv : 1);
        mem = (float *)lua_newuserdatauv(L, (size_t)(room + (lt > 0 ? lt : 1)) * sizeof(float), 0);
        lua_insert(L, field);                 /* below the table field: kept to the end of the call */
        if (tbl == field) tbl++;
        field++;
        q->table.t = mem;
        q->table.v = mem + (lt > 0 ? lt : 1);
        v = q->table.v + (lt > 0 ? lt : 1);
        nt = read_list(L, tbl, "times", "times", q->table.t, lt > 0 ? lt : 0);
        nv = read_list(L, tbl, "values", "values", v, lv > 0 ? lv : 0);
        if (nt < 1 || nv < 1 || nv > nt)
            sh_script_refuse(L, "\"spline_clear\" needs the spline's times and values (numbers, one value for each "
                             "time but the last)");
        for (i = 0; i < nt; i++) q->table.v[i] = i < nv ? v[i] : 0.0f;
    }
    for (i = 0; i < nt; i++)
        if (q->table.t[i] < 0 || q->table.t[i] > 1 || (i && q->table.t[i] < q->table.t[i - 1]))
            sh_script_refuse(L, "\"spline_clear\" needs the spline's times rising from 0 to 1");
    q->table.n = nt;
    lua_pop(L, 1);
    lua_getfield(L, idx, "roll");
    if (lua_type(L, -1) == LUA_TTABLE) { lua_getfield(L, -1, "value"); lua_remove(L, -2); }
    if (!lua_isnil(L, -1)) {
        int isnum = 0;
        lua_Number r = lua_tonumberx(L, -1, &isnum);
        if (!isnum || r != r || r < -360 || r > 360)
            sh_script_refuse(L, "\"spline_clear\": the spline's roll is degrees from -360 to 360");
        q->roll = (float)r;
    } else {
        q->roll = 90.0f;
    }
    lua_pop(L, 1);
}

/* ai:spline_clear(target, spline, seconds, [flags]) -> true, points | false. Tests the curve from the demon to
 * where the target will be `seconds` ahead, bent by the spline's table (roll 90 left, -90 right): clear when no
 * world geometry blocks it and, with "TF_NAV_GROUND", every point stands on the ground. */
static int l_spline_clear(lua_State *L)
{
    sh_script_handle h = 0;
    sh_ai_spline_query q;
    float (*pts)[3];
    double secs;
    void *ai, *t;
    int st, n = 0;
    memset(&q, 0, sizeof q);
    lua_settop(L, 5);   /* the arguments keep their places; the knot arrays go above them */
    sh_script_check_entity(L, 2, "spline_clear");
    check_spline(L, 3, &q);
    secs = sh_script_api_check_number(L, 4, "spline_clear", "the seconds ahead to predict the target");
    if (!(secs >= 0)) sh_script_refuse(L, "\"spline_clear\" needs the seconds ahead, 0 or more, got %g", secs);
    if (!lua_isnoneornil(L, 5)) {
        const char *f = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : NULL;
        char bad[64];
        unsigned bits = f ? sh_ai_tests_parse(f, bad, sizeof bad) : 0;
        if (!f || (!bits && bad[0]) || (bits & ~SH_AI_TF_NAV_ENDPOINT))
            sh_script_refuse(L, "\"spline_clear\" knows the flag \"TF_NAV_GROUND\" only");
        q.nav_ground = bits != 0;
    }
    NEED_OP(L, spline_clear, "spline_clear");
    ai = self_ai(L, "spline_clear", &h);
    if (!ai) { lua_pushnil(L); return 1; }
    t = other_entity(L, 2, "spline_clear");
    if (!t || !g_engine->origin(t, q.to)) { lua_pushboolean(L, 0); return 1; }
    q.target = t;
    if (secs > 0) {
        sh_script_earg v;
        if (sh_script_api_event(t, "getLinearVelocity", NULL, 0, &v) && v.type == 'v') {
            q.to[0] += (float)(v.v[0] * secs);
            q.to[1] += (float)(v.v[1] * secs);
            q.to[2] += (float)(v.v[2] * secs);
        }
    }
    /* every knot's point and the two ends: the curve is never cut */
    pts = (float (*)[3])lua_newuserdatauv(L, (size_t)(q.table.n + 2) * sizeof *pts, 0);
    st = g_engine->spline_clear(ai, &q, pts, q.table.n + 2, &n);
    if (st == 1) { lua_pushboolean(L, 1); push_points(L, pts, n); return 2; }
    if (st == 0) { lua_pushboolean(L, 0); return 1; }
    lua_pushnil(L);
    return 1;
}

/* ------------------------------------------------------- what it is, pain */

/* e:monster_type() -> the engine's monster type of a demon ("AI_MONSTER_BARON"),
 * nil for an entity that is not a demon */
static int l_monster_type(lua_State *L)
{
    uint64_t h = sh_script_check_entity(L, 1, "monster_type");
    void *e;
    double v = 0;
    const char *name;
    if (!g_engine)
        sh_script_refuse(L, "\"monster_type\" is not available in this session (Snapmap+ could not find the game's AI "
                         "functions; see the backend log)");
    if (!h || !(e = g_engine->entity((sh_script_handle)h)) || !g_engine->is_ai(e)) { lua_pushnil(L); return 1; }
    if (!run(e, SH_AI_EV_GET_MONSTER_TYPE, NULL, 0, &v) || !(name = sh_ai_monster_name((uint32_t)v))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, name);
    return 1;
}

/* ai:pain_string(text): the demon's pain clips are picked among those whose
 * pain graph string is `text` (as an attack sets it); ai:pain_string(nil)
 * gives the choice back to the engine's own string. */
static int l_pain_string(lua_State *L)
{
    const char *text = NULL;
    void *ai;
    if (!lua_isnoneornil(L, 2)) {
        size_t n = 0, i;
        if (lua_type(L, 2) != LUA_TSTRING) sh_script_refuse(L, "\"pain_string\" needs text or nil, got %s", luaL_typename(L, 2));
        text = lua_tolstring(L, 2, &n);
        for (i = 0; i < n; i++)
            if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
                  (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '.'))
                sh_script_refuse(L, "\"pain_string\": \"%s\" is not a usable string (letters, digits, _ and . only)", text);
        if (!n) text = NULL;
    }
    NEED_OP(L, pain_string, "pain_string");
    ai = self_ai(L, "pain_string", NULL);
    if (ai) g_engine->pain_string(ai, text);
    return 0;
}

/* ai:block_pain(reaction, seconds): the engine's damage table may not choose
 * `reaction` ("PAIN_FALTER", ...) for the demon for that long. */
static int l_block_pain(lua_State *L)
{
    const char *name;
    double secs;
    int reaction = -1, i;
    void *ai;
    if (lua_type(L, 2) != LUA_TSTRING)
        sh_script_refuse(L, "\"block_pain\" needs a pain reaction name, as \"PAIN_FALTER\", got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    name = lua_tostring(L, 2);
    for (i = 1; i < SH_AI_PAIN_REACTIONS; i++) {
        const char *n = sh_script_combat_pain_name(i);
        if (n && (!_stricmp(name, n) || !_stricmp(name, n + 5))) reaction = i;
    }
    if (reaction < 0) sh_script_refuse(L, "\"block_pain\" does not know the pain reaction \"%s\"", name);
    secs = sh_script_api_check_number(L, 3, "block_pain", "the seconds to block it");
    if (secs < 0 || secs > 600) sh_script_refuse(L, "\"block_pain\" needs seconds from 0 to 600, got %g", secs);
    NEED_OP(L, block_pain, "block_pain");
    ai = self_ai(L, "block_pain", NULL);
    if (ai) g_engine->block_pain(ai, reaction, (int)(secs * 1000.0 + 0.5));
    return 0;
}

/* ai:request_pain(reaction [, damage]): asks the engine for a pain reaction as its own pain start does for a hit
 * (the pain graph picks the clip, `damage` sets the intensity). Refused while a pain at least as strong plays or
 * the reaction is blocked (block_pain). true when the alert cycle took it; false and why. */
static int l_request_pain(lua_State *L)
{
    const char *name;
    char why[96] = "";
    double damage = 0.0;
    int reaction = -1, i, r;
    void *ai;
    if (lua_type(L, 2) != LUA_TSTRING)
        sh_script_refuse(L, "\"request_pain\" needs a pain reaction name, as \"PAIN_FALTER\", got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    name = lua_tostring(L, 2);
    for (i = 1; i < SH_AI_PAIN_REACTIONS; i++) {
        const char *n = sh_script_combat_pain_name(i);
        if (n && (!_stricmp(name, n) || !_stricmp(name, n + 5))) reaction = i;
    }
    if (reaction < 0) sh_script_refuse(L, "\"request_pain\" does not know the pain reaction \"%s\"", name);
    if (!lua_isnoneornil(L, 3)) {
        damage = sh_script_api_check_number(L, 3, "request_pain", "the damage the pain answers");
        if (!(damage >= 0.0 && damage <= 1e9)) sh_script_refuse(L, "\"request_pain\" needs a damage from 0 to 1e9, got %g", damage);
    }
    NEED_OP(L, request_pain, "request_pain");
    ai = self_ai(L, "request_pain", NULL);
    if (!ai) { lua_pushboolean(L, 0); lua_pushstring(L, "the demon is gone"); return 2; }
    r = g_engine->request_pain(ai, reaction, (float)damage, why, sizeof why);
    if (r > 0) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0);
    lua_pushstring(L, r < 0 ? "the game's pain start is unavailable in this session" : why);
    return 2;
}

int sh_script_ai_park(void *ai, sh_script_handle h)
{
    return g_engine && ai && park(ai, h) != NULL;
}

/* The order as the engine's event; a demon with eternal_interrupts then gets the web's interrupt blend mode
 * Eternal's command would carry (sh_ai_order_interrupt). */
static int anim_send(void *ai, ai_rec *r, const char *dest, const char *via, int kind, int blend_ms)
{
    sh_ai_arg a[4];
    int ok, mode;
    a[0] = arg_i(SH_AI_ANIMWEB_BODY);
    a[1] = arg_s('1', dest);
    if (kind == SH_AI_ORDER_FORCE || kind == SH_AI_ORDER_FORCE_VIA) {
        a[2] = arg_i(blend_ms);
        a[3] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
        ok = run(ai, SH_AI_EV_FORCE_ANIM_STATE, a, 4, NULL);
    } else if (via) {
        a[2] = arg_s('1', via);
        a[3] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
        ok = run(ai, SH_AI_EV_CHANGE_ANIM_STATE_VIA, a, 4, NULL);
    } else {
        a[2] = arg_i(SH_AI_ANIMWAIT_DONT_WAIT);
        ok = run(ai, SH_AI_EV_CHANGE_ANIM_STATE, a, 3, NULL);
    }
    mode = sh_ai_order_interrupt(kind);
    if (ok && r && r->eternal_interrupts && mode >= 0 && g_engine->interrupt_blend)
        (void)g_engine->interrupt_blend(ai, mode);
    return ok;
}

/* An order that is not forced. An idle order that stands ends with it, and a turn in place asked for under it
 * and not begun is ended first so no step begins behind the order. */
static int anim_give(void *ai, ai_rec *r, const char *dest, const char *via, int kind)
{
    sh_ai_step_state st;
    memset(&st, 0, sizeof st);
    st.idle_turn = -1;
    if (g_engine->step_state && g_engine->step_state(ai, &st) == 1 && st.moving && st.idle_turn >= 0 &&
        g_engine->clear_move)
        g_engine->clear_move(ai);
    return anim_send(ai, r, dest, via, kind, 0);
}

/* A step stage of the engine's animFSM ends when its animWeb command ends, but a script order sends the web
 * away so the command never ends and the FSM stalls for a second. So an order is held back while a step is in
 * flight and given when it ends; a forced order is given at once and the step's command is ended. */
int sh_script_ai_anim(void *ai, sh_script_handle h, const char *dest, const char *via, int force, int blend_ms)
{
    ai_rec *r;
    sh_ai_step_state st;
    if (!g_engine || !ai || !dest || !(r = park(ai, h))) return 0;
    if (force == SH_AI_ORDER_FORCE || force == SH_AI_ORDER_FORCE_VIA) {
        int in_flight = step_in_flight(ai, &st);
        anim_wait_drop(r);
        if (!anim_send(ai, r, dest, NULL, force, blend_ms)) return 0;
        if (in_flight && g_engine->step_interrupt) (void)g_engine->step_interrupt(ai);
        return 1;
    }
    if (step_in_flight(ai, &st)) {
        char *d = _strdup(dest), *v = via ? _strdup(via) : NULL;
        if (!d || (via && !v)) { free(d); free(v); return 0; }
        anim_wait_drop(r);
        r->wait_dest = d;
        r->wait_via = v;
        r->wait_kind = force;
        return 1;
    }
    anim_wait_drop(r);
    return anim_give(ai, r, dest, via, force);
}

/* Every frame: an order held back is given once its step has ended. */
static void anim_wait_step(void)
{
    int i;
    if (!g_engine) return;
    for (i = 0; i < g_nrecs; i++) {
        ai_rec *r = g_recs[i];
        sh_ai_step_state st;
        void *ai;
        if (!r->wait_dest) continue;
        ai = r->handle ? g_engine->entity(r->handle) : NULL;
        if (!ai || !park(ai, r->handle)) { anim_wait_drop(r); continue; }
        if (step_in_flight(ai, &st)) continue;
        (void)anim_give(ai, r, r->wait_dest, r->wait_via, r->wait_kind);
        anim_wait_drop(r);
    }
}

/* ---- destructibles: DOOM Eternal's activation of a gore breakable's pieces (script_api_ai.h) */

/* The map's destructibles: any number of defs, each with any number of pieces. */
typedef struct { char *def; sh_ai_destruct_spec spec; } ai_destruct_def;
static ai_destruct_def *g_dd;
static int g_dd_cap;
static volatile LONG g_dd_n;
static SRWLOCK g_dd_lock = SRWLOCK_INIT;

int sh_ai_destructible_add(const char *def, const sh_ai_destruct_spec *spec)
{
    int i, n, ok = 0;
    sh_ai_destruct_piece *pieces;
    char *name = NULL;
    if (!def || !def[0] || !spec || spec->n < 1 || !spec->piece) return 0;
    pieces = (sh_ai_destruct_piece *)malloc(sizeof *pieces * (size_t)spec->n);
    if (!pieces) return 0;
    memcpy(pieces, spec->piece, sizeof *pieces * (size_t)spec->n);
    AcquireSRWLockExclusive(&g_dd_lock);
    n = (int)g_dd_n;
    for (i = 0; i < n; i++) if (!_stricmp(g_dd[i].def, def)) break;
    if (i == n && n == g_dd_cap) {
        int cap = g_dd_cap ? g_dd_cap * 2 : 16;
        ai_destruct_def *grown = (ai_destruct_def *)realloc(g_dd, sizeof *grown * (size_t)cap);
        if (grown) { g_dd = grown; g_dd_cap = cap; }
    }
    if (i < n || (n < g_dd_cap && (name = _strdup(def)) != NULL)) {
        if (i < n) free(g_dd[i].spec.piece);
        else g_dd[i].def = name;
        g_dd[i].spec = *spec;
        g_dd[i].spec.piece = pieces;
        pieces = NULL;
        if (i == n) InterlockedExchange(&g_dd_n, n + 1);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_dd_lock);
    free(pieces);
    return ok;
}

int sh_ai_destructible_find(const char *const *names, int n, sh_ai_destruct_spec *out)
{
    int i, k, used, found = 0;
    if (!g_dd_n || !names || n <= 0) return 0;
    AcquireSRWLockShared(&g_dd_lock);
    used = (int)g_dd_n;
    for (k = 0; k < n && !found; k++) {
        if (!names[k]) continue;
        for (i = 0; i < used; i++) {
            if (_stricmp(g_dd[i].def, names[k])) continue;
            if (out) *out = g_dd[i].spec;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_dd_lock);
    return found;
}

void sh_ai_destructible_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_dd_lock);
    for (i = 0; i < (int)g_dd_n; i++) { free(g_dd[i].def); free(g_dd[i].spec.piece); }
    InterlockedExchange(&g_dd_n, 0);
    ReleaseSRWLockExclusive(&g_dd_lock);
}

/* idAngles::ToMat3 in both games (DOOM Eternal 0x1403ABBE0): degrees; rows forward, left, up. */
static void dd_angles(float pitch, float yaw, float roll, float m[9])
{
    const float k = 0.017453292f;
    float sp = sinf(pitch * k), cp = cosf(pitch * k), sy = sinf(yaw * k), cy = cosf(yaw * k);
    float sr = sinf(roll * k), cr = cosf(roll * k);
    m[0] = cp * cy; m[1] = cp * sy; m[2] = -sp;
    m[3] = sr * sp * cy - cr * sy; m[4] = sr * sp * sy + cr * cy; m[5] = sr * cp;
    m[6] = cr * sp * cy + sr * sy; m[7] = cr * sp * sy - sr * cy; m[8] = cr * cp;
}

/* v * m: v taken in the rows' frame (both games' idVec3 * idMat3). */
static void dd_vmul(const float v[3], const float m[9], float out[3])
{
    int c;
    for (c = 0; c < 3; c++) out[c] = v[0] * m[c] + v[1] * m[3 + c] + v[2] * m[6 + c];
}

static float dd_len(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void dd_unit(float v[3])
{
    float l = dd_len(v);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* A direction's frame (DOOM Eternal 0x1403B9F20): rows the direction, its horizontal normal, their cross. */
static void dd_dir_frame(const float d[3], float m[9])
{
    float h = d[0] * d[0] + d[1] * d[1], hl;
    if (h <= 1e-12f) {
        float s = d[2] >= 0.0f ? 1.0f : -1.0f;
        m[0] = 0.0f; m[1] = 0.0f; m[2] = s; m[3] = 1.0f; m[4] = 0.0f; m[5] = 0.0f; m[6] = 0.0f; m[7] = s; m[8] = 0.0f;
        return;
    }
    m[0] = d[0]; m[1] = d[1]; m[2] = d[2];
    dd_unit(m);
    hl = 1.0f / sqrtf(h);
    m[3] = -d[1] * hl; m[4] = d[0] * hl; m[5] = 0.0f;
    m[6] = m[5] * m[1] - m[4] * m[2];           /* the first row crossed with the second */
    m[7] = m[3] * m[2] - m[5] * m[0];
    m[8] = m[4] * m[0] - m[3] * m[1];
}

static float dd_between(sh_ai_rand_fn rnd, void *rs, float lo, float hi) { return lo + rnd(rs) * (hi - lo); }

int sh_ai_destruct_fx_model(unsigned char *phys)
{
    unsigned char *fx;
    void *model;
    int n, i, given = 0;
    if (!phys) return 0;
    fx = *(unsigned char **)(phys + SH_AI_FXB_EXPLOSION_FX);
    n = *(const int *)(phys + SH_AI_FXB_EXPLOSION_FX + 8);
    model = *(void **)(phys + SH_AI_FXB_MODEL);
    if (!fx || !model || n <= 0 || n > 64) return 0;
    for (i = 0; i < n; i++) {
        void **rm = (void **)(fx + (size_t)i * SH_AI_FXM_SIZE + SH_AI_FXM_RM);
        if (!*rm) { *rm = model; given++; }
    }
    return given;
}

void sh_ai_destruct_velocity(const sh_ai_destruct_piece *p, const float org[3], const float axis[9], const float ext[3],
                             const float piece[3], float max_speed, float max_spin, sh_ai_rand_fn rnd, void *rs,
                             float lin[3], float ang[3])
{
    float d[3] = { 0.0f, 0.0f, 1.0f }, m[9], e[9], f[3], w[3], s_lin = 0.0f, s_spin = 0.0f, l;
    int k, drawn = 0;
    dd_dir_frame(ext, e);
    switch (p->preset) {
    case SH_AI_PRESET_RANDOM:                   /* (0, 0, 1) plus a random forward, normalised */
        dd_angles(rnd(rs) * 200.0f - 100.0f, rnd(rs) * 360.0f, 0.0f, m);
        for (k = 0; k < 3; k++) d[k] += m[k];
        dd_unit(d);
        drawn = 1;
        break;
    case SH_AI_PRESET_CONE: {
        float yaw = dd_between(rnd, rs, -p->yaw * 0.5f, p->yaw * 0.5f);
        float pitch = dd_between(rnd, rs, -p->pitch * 0.5f, p->pitch * 0.5f);
        dd_angles(pitch, yaw, 0.0f, m);
        dd_vmul(e, m, d);
        drawn = 1;
        break;
    }
    case SH_AI_PRESET_SPHERICAL: {              /* any direction above restrictBottomAngle below the horizon */
        float r = p->bottom < 0.0f ? 0.0f : (p->bottom > 90.0f ? 90.0f : p->bottom);
        dd_angles(rnd(rs) * ((90.0f - r) + 90.0f) - 90.0f, rnd(rs) * 360.0f, 0.0f, m);
        d[0] = m[0]; d[1] = m[1]; d[2] = m[2];
        drawn = 1;
        break;
    }
    case SH_AI_PRESET_ANGEL_WINGS:              /* the owner's left (or the hit frame's third row), level, to the piece's side */
        if (p->owner_frame) { w[0] = axis[3]; w[1] = axis[4]; }
        else { w[0] = e[6]; w[1] = e[7]; }
        w[2] = 0.0f;
        if (p->random_dir) {
            if (rnd(rs) < 0.5f) { w[0] = -w[0]; w[1] = -w[1]; }
        } else if ((piece[0] - org[0]) * w[0] + (piece[1] - org[1]) * w[1] < 0.0f) {
            w[0] = -w[0]; w[1] = -w[1];
        }
        dd_unit(w);
        dd_angles(dd_between(rnd, rs, -p->pitch * 0.5f, p->pitch * 0.5f), dd_between(rnd, rs, -p->yaw * 0.5f, p->yaw * 0.5f),
                  0.0f, m);
        dd_vmul(w, m, f);
        if (dd_len(f) > 0.0f) { d[0] = f[0]; d[1] = f[1]; d[2] = f[2]; }
        drawn = 1;
        break;
    default:                                    /* custom (and a preset not carried): no preset speed */
        break;
    }
    if (drawn) {
        s_lin = dd_between(rnd, rs, p->lin_min, p->lin_max);
        s_spin = dd_between(rnd, rs, p->spin_min, p->spin_max);
    }
    dd_unit(d);
    f[0] = p->dir[0]; f[1] = p->dir[1]; f[2] = p->dir[2];
    dd_unit(f);
    dd_vmul(f, axis, w);                        /* the force's direction in the owner's axes */
    for (k = 0; k < 3; k++) {
        lin[k] = d[k] * s_lin + w[k] * p->speed;
        ang[k] = d[k] * s_spin + w[k] * p->spin;    /* the preset's own spin axis is never read: its direction */
    }
    if (max_speed > 0.0f && (l = dd_len(lin)) > max_speed) for (k = 0; k < 3; k++) lin[k] *= max_speed / l;
    if (max_spin > 0.0f && (l = dd_len(ang)) > max_spin) for (k = 0; k < 3; k++) ang[k] *= max_spin / l;
}

static float dd_field(lua_State *L, int idx, const char *key, float lo, float hi)
{
    return opt_num_field(L, idx, key, 0.0f, "destructible", lo, hi);
}

/* Register the per-piece impulses to apply when this entity breaks. */
static int l_destructible(lua_State *L)
{
    const char *def = sh_script_check_name(L, 1, "destructible", 0, NULL);
    sh_ai_destruct_spec s;
    int i, n;
    memset(&s, 0, sizeof s);
    if (lua_type(L, 2) != LUA_TTABLE)
        sh_script_refuse(L, "\"destructible\" needs a table describing the destructible, got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    s.ignore_external = opt_bool_field(L, 2, "ignore_external");
    s.max_speed = dd_field(L, 2, "max_speed", 0.0f, 1e6f);
    s.max_spin = dd_field(L, 2, "max_spin", 0.0f, 1e4f);
    lua_getfield(L, 2, "pieces");
    if (lua_type(L, -1) != LUA_TTABLE)
        sh_script_refuse(L, "\"destructible\" needs pieces, a list of the pieces' motions");
    n = (int)lua_rawlen(L, -1);
    if (n < 1) sh_script_refuse(L, "\"destructible\" needs at least one piece in pieces");
    /* the pieces live in a userdata under the list while they are read (a refusal frees them with it); the registry
     * keeps its own copy */
    s.piece = (sh_ai_destruct_piece *)lua_newuserdatauv(L, sizeof *s.piece * (size_t)n, 0);
    memset(s.piece, 0, sizeof *s.piece * (size_t)n);
    lua_insert(L, -2);
    for (i = 0; i < n; i++) {
        sh_ai_destruct_piece *p = &s.piece[i];
        int top = lua_gettop(L);
        lua_rawgeti(L, -1, i + 1);
        if (lua_type(L, -1) != LUA_TTABLE)
            sh_script_refuse(L, "\"destructible\" needs a table for piece %d, got %s", i + 1, luaL_typename(L, -1));
        p->preset = (int)dd_field(L, top + 1, "preset", 0.0f, 5.0f);
        p->lin_min = dd_field(L, top + 1, "min", 0.0f, 1e6f);
        p->lin_max = dd_field(L, top + 1, "max", 0.0f, 1e6f);
        p->spin_min = dd_field(L, top + 1, "spin_min", 0.0f, 1e4f);
        p->spin_max = dd_field(L, top + 1, "spin_max", 0.0f, 1e4f);
        p->pitch = dd_field(L, top + 1, "pitch", 0.0f, 360.0f);
        p->yaw = dd_field(L, top + 1, "yaw", 0.0f, 360.0f);
        p->bottom = dd_field(L, top + 1, "bottom", 0.0f, 180.0f);
        p->random_dir = opt_bool_field(L, top + 1, "random");
        lua_getfield(L, top + 1, "owner");
        p->owner_frame = lua_isnil(L, -1) ? 1 : lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, top + 1, "dir");
        if (!lua_isnil(L, -1)) check_vec(L, lua_gettop(L), "destructible", p->dir);
        lua_pop(L, 1);
        p->speed = dd_field(L, top + 1, "speed", -1e6f, 1e6f);
        p->spin = dd_field(L, top + 1, "spin", -1e4f, 1e4f);
        lua_settop(L, top);
        if (p->preset == SH_AI_PRESET_THRU_SQUARE) {
            lua_pushboolean(L, 0);
            lua_pushfstring(L, "piece %d uses DOOM Eternal's thru-square preset, which Snapmap+ does not carry", i + 1);
            return 2;
        }
    }
    s.n = n;
    lua_pop(L, 1);   /* the list; the pieces' userdata stays on the stack until the call returns */
    NEED_OP(L, destructible, "destructible");
    if (!g_engine || g_engine->destructible() != 1) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the game's gore breakable spawn could not be reached");
        return 2;
    }
    if (!sh_ai_destructible_add(def, &s)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "there was not enough memory to keep the destructible");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ai:snapmap_order(): takes the order SnapMap's Follow action gave the demon out of its temp memory: nil when
 * none waits, else { kind = "path" (target: first node), "entity" (target) or "position" (pos) }. The stock
 * FSM never sees the entry. */
static int l_snapmap_order(lua_State *L)
{
    static const char *const kinds[4] = { NULL, "path", "entity", "position" };
    sh_ai_order o;
    sh_ai_arg a;
    void *ai;
    sh_script_handle h;
    ai_rec *r;
    NEED_OP(L, snapmap_order, "snapmap_order");
    ai = self_ai(L, "snapmap_order", &h);
    r = ai ? rec_of(h) : NULL;
    if (r && (r->native_path || (r->empty_order && g_engine->frame && r->order_frame == g_engine->frame()))) {
        r->empty_order = 0;
        lua_pushnil(L);
        return 1;
    }
    if (!ai || g_engine->snapmap_order(ai, &o) != 1) { lua_pushnil(L); return 1; }
    memset(&a, 0, sizeof a);
    a.type = 's';
    a.s = SH_AI_ORDER_KEYS[o.kind];
    (void)run(ai, SH_AI_EV_TEMP_MEMORY_REMOVE, &a, 1, NULL);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, kinds[o.kind]);
    lua_setfield(L, -2, "kind");
    if (o.kind == SH_AI_ORDER_POSITION) {
        push_vec(L, o.pos);
        lua_setfield(L, -2, "pos");
    } else {
        sh_script_handle h = sh_script_handles_find(o.spawn_id, NULL);
        if (!h) h = sh_script_handles_ref(o.spawn_id);
        if (h) { sh_script_push_entity(L, h); lua_setfield(L, -2, "target"); }
    }
    return 1;
}

/* ai:release(): hands the demon back to its own behaviour (the park, above) until the script's next command,
 * dropping any held order, forced via, facing or move. */
static int release_body(void *ai, sh_script_handle h)
{
    int i;
    if (g_engine->release(ai) != 1) return 0;
    for (i = 0; i < g_nrecs; i++) {
        ai_rec *r = g_recs[i];
        if (r->handle != h) continue;
        anim_wait_drop(r);
        r->walk = 0;
        r->facing = 0;
        r->has_dest = 0;
        r->move.state = SH_AI_MOVE_ARRIVED;
    }
    for (i = 0; i < g_nforce; i++)
        if (g_force_via[i].handle == h) force_via_drop(i--);
    return 1;
}

static int l_release(lua_State *L)
{
    sh_script_handle h;
    void *ai;
    NEED_OP(L, release, "release");
    ai = self_ai(L, "release", &h);
    lua_pushboolean(L, ai && release_body(ai, h));
    return 1;
}

/* Leave a Follow Path entry for the stock FSM to take. The Enemy SDK yields until that FSM returns the body. */
static int l_native_follow_path(lua_State *L)
{
    sh_script_handle h;
    sh_ai_order o;
    ai_rec *r;
    void *ai;
    int pending;
    if (!g_engine || !g_engine->snapmap_order || !g_engine->native_body_busy || !g_engine->release) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ai = self_ai(L, "native_follow_path", &h);
    if (!ai || !(r = rec_of(h))) { lua_pushboolean(L, 0); return 1; }
    pending = g_engine->snapmap_order(ai, &o);
    r->empty_order = pending == 0 && g_engine->frame != NULL;
    if (r->empty_order) r->order_frame = g_engine->frame();
    if (pending == 1 && o.kind == SH_AI_ORDER_PATH) {
        if (!r->native_path && g_engine->native_body_busy(ai, 1) >= 0)
            r->native_path = release_body(ai, h);
    } else if (r->native_path && pending >= 0 && g_engine->native_body_busy(ai, 0) == 0) {
        r->native_path = 0;
    }
    lua_pushboolean(L, r->native_path);
    return 1;
}

/* ai:anim_variable(name, value): sets the anim web variable `name` (as "hangoutIndex" in anim[hangoutIndex])
 * by the engine's set-by-name; true when set, false when the web has no such variable. */
static int l_anim_variable(lua_State *L)
{
    const char *name;
    size_t n = 0, i;
    double v;
    void *ai;
    int st;
    if (lua_type(L, 2) != LUA_TSTRING)
        sh_script_refuse(L, "\"anim_variable\" needs the variable's name (a string), got %s",
                         lua_isnoneornil(L, 2) ? "nothing" : luaL_typename(L, 2));
    name = lua_tolstring(L, 2, &n);
    if (!n) sh_script_refuse(L, "\"anim_variable\" needs the variable's name, got an empty string");
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            sh_script_refuse(L, "\"anim_variable\": \"%s\" is not a variable name (letters, digits and _ only)", name);
    v = sh_script_api_check_number(L, 3, "anim_variable", "the value");
    if (v != v || v > 1e9 || v < -1e9) sh_script_refuse(L, "\"anim_variable\" needs a value from -1e9 to 1e9, got %g", v);
    NEED_OP(L, anim_variable, "anim_variable");
    ai = self_ai(L, "anim_variable", NULL);
    st = ai ? g_engine->anim_variable(ai, name, (float)v) : 0;
    if (st < 0) sh_script_refuse(L, "\"anim_variable\" could not reach this demon's anim web");
    lua_pushboolean(L, st > 0);
    return 1;
}

/* ai:relations() -> { { entity =, respect =, emotion =, awareness = [, unlimited = true] }, ... }: the senses'
 * relation table (script_api_ai.h, sh_ai_relation) in the engine's order (most hostile first) with the demon's
 * awareness of each entity. nil when the table cannot be read. */
static int l_relations(lua_State *L)
{
    sh_ai_relation rows[SH_AI_REL_MAX];
    void *ai;
    int n, i, k = 0;
    NEED_OP(L, relations, "relations");
    ai = self_ai(L, "relations", NULL);
    if (!ai) { lua_pushnil(L); return 1; }
    n = g_engine->relations(ai, rows, SH_AI_REL_MAX);
    if (n < 0) { lua_pushnil(L); return 1; }
    lua_createtable(L, n, 0);
    for (i = 0; i < n; i++) {
        const char *respect = sh_ai_respect_name(rows[i].respect), *emotion = sh_ai_emotion_name(rows[i].emotion);
        const char *aware = g_engine->awareness ? sh_ai_awareness_name(g_engine->awareness(ai, rows[i].entity)) : NULL;
        sh_script_handle h = g_engine->handle_of(rows[i].entity);
        if (!h || !respect || !emotion) continue;
        lua_createtable(L, 0, 5);
        sh_script_push_entity(L, h); lua_setfield(L, -2, "entity");
        lua_pushstring(L, respect); lua_setfield(L, -2, "respect");
        lua_pushstring(L, emotion); lua_setfield(L, -2, "emotion");
        lua_pushstring(L, aware ? aware : "unaware"); lua_setfield(L, -2, "awareness");
        if (rows[i].unlimited) { lua_pushboolean(L, 1); lua_setfield(L, -2, "unlimited"); }
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

/* e:off_limits() -> true when the engine marks the actor (a demon, a player, a bot) off-limits to the demons around
 * it (a sync interaction's instigator), false when not; nil when the entity is no actor or is gone. */
static int l_off_limits(lua_State *L)
{
    uint64_t h = sh_script_check_entity(L, 1, "off_limits");
    void *ent;
    int st;
    NEED_OP(L, off_limits, "off_limits");
    ent = h ? g_engine->entity((sh_script_handle)h) : NULL;
    st = ent ? g_engine->off_limits(ent) : -1;
    if (st < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, st == 1);
    return 1;
}

static const struct { const char *name; lua_CFunction fn; } k_calls[] = {
    { "move_to", l_move_to },           { "move_to_entity", l_move_to_entity },
    { "stop", l_stop },                 { "move_done", l_move_done },
    { "face", l_face },                 { "aim_at", l_aim_at },
    { "set_enemy", l_set_enemy },       { "enemy", l_enemy },
    { "can_see", l_can_see },
    { "angle_to", l_angle_to },         { "aware", l_aware },
    { "awareness", l_awareness },       { "path_length", l_path_length },
    { "can_reach", l_can_reach },       { "find_position", l_find_position },
    { "trigger", l_trigger },           { "trigger_pulled", l_trigger_pulled },
    { "equip", l_equip },               { "laser", l_laser },
    { "laser_sweep", l_laser_sweep },   { "focus_tag", l_focus_tag },
    { "turn_done", l_turn_done },       { "laser_state", l_laser_state },
    { "recoil", l_recoil },             { "weapon_ready", l_weapon_ready },
    { "weapon_use", l_weapon_use },
    { "aim_point", l_aim_point },       { "animation_fits", l_anim_valid },
    { "force_animation_via", l_anim_force_via }, { "firing_position", l_firing_position },
    { "path_points", l_path_points },   { "spline_clear", l_spline_clear },
    { "clear_walk_line", l_clear_walk_line },
    { "reset_motion_correction", l_reset_motion_correction },
    { "on_ground", l_on_ground },
    { "monster_type", l_monster_type }, { "pain_string", l_pain_string },
    { "block_pain", l_block_pain }, { "request_pain", l_request_pain },
    { "fire_mode", l_fire_mode },       { "pull_trigger", l_pull_trigger },
    { "release_trigger", l_release_trigger }, { "muzzle_on_target", l_muzzle_on_target },
    { "focus", l_focus },               { "hold", l_hold },
    { "tracking", l_tracking },         { "body_rotation", l_body_rotation },
    { "laser_add", l_laser_add },       { "laser_remove", l_laser_remove },
    { "rear_footsteps", l_rear_footsteps },
    { "wound_hides", l_wound_hides },
    { "wound_effect", l_wound_effect },
    { "held", l_held },
    { "posed_legs", l_posed_legs },
    { "fk_reference_pose", l_fk_reference_pose }, { "eternal_interrupts", l_eternal_interrupts },
    { "wound_damage_counts", l_wound_damage_counts },
    { "snapmap_order", l_snapmap_order }, { "native_follow_path", l_native_follow_path },
    { "release", l_release },           { "anim_variable", l_anim_variable },
    { "relations", l_relations },       { "off_limits", l_off_limits },
};

void sh_script_ai_spawned(void *ent)
{
    if (!ent || !g_engine || !g_engine->gore_init || !g_engine->is_ai || !g_engine->is_ai(ent)) return;
    (void)g_engine->gore_init(ent);
}

int sh_script_ai_register(void)
{
    static int done;
    int i, n = 0;
    if (done) return 0;
    done = 1;
    sh_script_api_on_spawn(sh_script_ai_spawned);
    sh_script_api_on_frame(force_via_step);
    sh_script_api_on_frame(anim_wait_step);
    sh_script_handles_on_gone(wound_owner_gone);
    for (i = 0; i < (int)(sizeof k_calls / sizeof k_calls[0]); i++)
        n += sh_script_api_add(SH_SCRIPT_API_METHOD, k_calls[i].name, k_calls[i].fn);
    n += sh_script_api_add(SH_SCRIPT_API_GLOBAL, "destructible", l_destructible);
    return n;
}

#ifndef SH_SCRIPT_PURE
/* ======================================================= the engine side */
#include <windows.h>
#include "../backend_log.h"
#include "../engine_build.h"
#include "../entity.h"
#include "../hook.h"
#include "../typeinfo.h"
#include "../cheatkeep.h"
#include "script_api_reflect.h"
#include "script_reflect.h"
#include "script_hooks.h"
#include "script_api_common.h"

/* idAI2's idTypeInfo (the address its GetType returns: VK vtable
 * 0x142035B30 slot 0 = 0x140391970, GL 0x14204F900 slot 0 = 0x140391240). */
#define AI_IDAI2_TYPEINFO_RVA     SH_RVA(0x5CF7A90u, 0x45FD110u)

typedef void *(*ai_vfn0)(void *self);
typedef void *(*ai_vfn_int)(void *self, int i);
typedef uint64_t (*ai_irs_fn)(void *ai);
typedef int (*ai_awareness_fn)(void *perception, void *entity);
typedef void *(*ai_ptr_get_fn)(void *entity_ptr);
typedef void *(*ai_path_key_fn)(void *ai, int index, const char *name);
typedef int (*ai_path_query_fn)(void *move_fsm, void *query, const float *goal, float nav_tolerance,
                                float *out_reachable, float *out_length, uint8_t flag);

static uint8_t          *g_base;
static int32_t rel32_at(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static ai_irs_fn         g_irs_target;
static ai_irs_fn         g_irs_orig;
static ai_awareness_fn   g_get_awareness;
static ai_ptr_get_fn     g_ptr_get;
static void *(*g_weapon_in_slot)(void *ai, int slot);         /* the weapon in an equipSlot_t, or NULL */
static void *(*g_inventory_item)(void *list, int index);      /* the inventory list's item, or NULL */
static ai_path_key_fn    g_path_key;
static ai_path_query_fn  g_path_query;
static const uint64_t   *g_path_global;
static const uint8_t    *g_ai2_type;
static int               g_detour_failed;

static void ai_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

/* A catalogue event through the shared event helper (script_api_common.h),
 * which finds it by name and checks its arguments against the game's. */
static int e_event(void *ent, int ev, const sh_ai_arg *args, int n, double *ret)
{
    sh_script_earg a[SH_SCRIPT_EVENT_ARGS], r;
    int i;
    if (ev < 0 || ev >= SH_AI_EV_COUNT || n > SH_SCRIPT_EVENT_ARGS || !ent) return 0;
    for (i = 0; i < n; i++) {
        const sh_ai_arg *s = &args[i];
        switch (s->type) {
        case 'i': case 'b': a[i] = sh_script_earg_int(s->i); break;   /* the engine copies a bool as 'i' */
        case 'f': a[i] = sh_script_earg_float(s->f); break;
        case 'v': a[i] = sh_script_earg_vec(s->v); break;
        case 's': case '1': a[i] = sh_script_earg_text(s->type, s->s); break;
        case '4': a[i] = sh_script_earg_no_anim(); break;
        case 'e': a[i] = sh_script_earg_entity(s->ent); break;
        default: return 0;
        }
    }
    if (!sh_script_api_event(ent, SH_AI_EVENTS[ev].name, a, n, ret ? &r : NULL)) return 0;
    if (ret) {
        if (r.type == 'f') *ret = r.f;
        else if (r.type == 'b') *ret = (r.i & 0xff) != 0;
        else *ret = r.i;
    }
    return 1;
}

static void *e_entity(sh_script_handle h) { return sh_script_handle_entity(h); }

static uint32_t spawn_id_of(void *entity)
{
    uint8_t *gm = (uint8_t *)sh_entity_gamemgr();
    uint32_t id = SH_SCRIPT_SPAWN_ID_NONE;
    if (!gm || !entity) return id;
    __try {
        int num = *(const int *)((const uint8_t *)entity + SH_SCRIPT_ENT_NUMBER);
        if (num >= 0 && num < SH_SCRIPT_ENT_NUMBER_WORLD)
            id = *(const uint32_t *)(gm + SH_SCRIPT_GAME_SPAWN_IDS + (size_t)num * 4);
    } __except (av_only(GetExceptionCode())) { id = SH_SCRIPT_SPAWN_ID_NONE; }
    return id;
}

static sh_script_handle e_handle_of(void *entity)
{
    uint32_t id = spawn_id_of(entity);
    sh_script_handle h;
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return 0;
    h = sh_script_handles_find(id, NULL);
    return h ? h : sh_script_handles_ref(id);
}

static int e_is_ai(void *entity)
{
    ai_vfn0 get_type = (ai_vfn0)vfn(entity, 0);
    int ok = 0;
    if (!get_type || !g_ai2_type) return 0;
    __try {
        const uint8_t *t = (const uint8_t *)get_type(entity);
        int num = *(const int *)(t + SH_AI_TYPE_NUM);
        int lo = *(const int *)(g_ai2_type + SH_AI_TYPE_NUM), hi = *(const int *)(g_ai2_type + SH_AI_TYPE_NUM + 4);
        ok = lo <= hi && num >= lo && num <= hi;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

const char *sh_script_ai_behavior(void *entity)
{
    if (!e_is_ai(entity)) return NULL;
    __try {
        const uint8_t *decl = *(const uint8_t *const *)((const uint8_t *)entity + SH_AI_OFF_BEHAVIOR_DECL);
        return decl ? *(const char *const *)(decl + 8) : NULL;
    } __except (av_only(GetExceptionCode())) { return NULL; }
}

static int e_origin(void *entity, float out[3])
{
    int ok = 0;
    __try {
        void *phys = *(void **)((uint8_t *)entity + SH_AI_OFF_PHYSICS);
        ai_vfn_int f = (ai_vfn_int)vfn(phys, SH_AI_VSLOT_ORIGIN);
        const float *o = f ? (const float *)f(phys, 0) : NULL;
        if (o) { out[0] = o[0]; out[1] = o[1]; out[2] = o[2]; ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_yaw(void *entity, float *deg)
{
    int ok = 0;
    __try {
        void *phys = *(void **)((uint8_t *)entity + SH_AI_OFF_PHYSICS);
        ai_vfn_int f = (ai_vfn_int)vfn(phys, SH_AI_VSLOT_AXIS);
        const float *m = f ? (const float *)f(phys, 0) : NULL;   /* idMat3: row 0 is forward */
        if (m) { *deg = (float)(atan2(m[1], m[0]) * 180.0 / AI_PI); ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

/* The demon's enemy as DOOM's own target queries see it: a player with noTarget on is no target
 * (cheatkeep.h sh_player_flags_no_target), though the enemy field still names it. */
static void *e_enemy(void *ai)
{
    void *e = NULL;
    uint8_t flags = 0;
    if (!g_ptr_get) return NULL;
    __try { e = g_ptr_get((uint8_t *)ai + SH_AI_OFF_ENEMY); }
    __except (av_only(GetExceptionCode())) { e = NULL; }
    if (e && sh_script_api_kind(e) == SH_SCRIPT_KIND_PLAYER) {
        __try { flags = *((const uint8_t *)e + SH_PLAYER_CHEAT_FLAGS_OFF); }
        __except (av_only(GetExceptionCode())) { flags = 0; }
        if (sh_player_flags_no_target(flags)) return NULL;
    }
    return e;
}

static int e_awareness(void *ai, void *target)
{
    int v = -1;
    if (!g_get_awareness || !target) return -1;
    __try { v = g_get_awareness((uint8_t *)ai + SH_AI_OFF_PERCEPTION, target); }
    __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The senses' relation table (script_api_ai.h, SH_AI_REL_*), rows whose entity is gone left out. */
static int e_relations(void *ai, sh_ai_relation *out, int cap)
{
    const uint8_t *senses = (const uint8_t *)ai + SH_AI_OFF_PERCEPTION, *rows = NULL;
    int count = 0, i, n = 0;
    if (!g_ptr_get || !out || cap <= 0) return -1;
    __try {
        rows = *(const uint8_t * const *)(senses + SH_AI_REL_ROWS);
        count = *(const int *)(senses + SH_AI_REL_COUNT);
    } __except (av_only(GetExceptionCode())) { return -1; }
    if (count < 0 || count > 4096 || (count && !rows)) return -1;
    for (i = 0; i < count && n < cap; i++) {
        const uint8_t *row = rows + (size_t)i * SH_AI_REL_SIZE;
        sh_ai_relation r;
        uint32_t word = 0;
        memset(&r, 0, sizeof r);
        __try {
            word = *(const uint32_t *)(row + SH_AI_REL_WORD);
            r.respect = *(const int *)(row + SH_AI_REL_RESPECT);
            r.emotion = *(const int *)(row + SH_AI_REL_EMOTION);
            r.entity = g_ptr_get((void *)(row + SH_AI_REL_ENTITY));
        } __except (av_only(GetExceptionCode())) { return -1; }
        if (!r.entity) continue;
        r.unlimited = word == SH_AI_REL_NO_ENTITY;
        out[n++] = r;
    }
    return n;
}

/* The actor's off-limits flag through the getter in its vtable (script_api_ai.h, SH_AI_VSLOT_OFF_LIMITS). */
static int e_off_limits(void *actor)
{
    const uint8_t *fn = NULL;
    uint8_t code[8], v = 0;
    int kind = sh_script_api_kind(actor);
    if (kind != SH_SCRIPT_KIND_AI && kind != SH_SCRIPT_KIND_PLAYER) return -1;
    __try {
        fn = (*(const uint8_t * const * const *)actor)[SH_AI_VSLOT_OFF_LIMITS / 8];
        memcpy(code, fn, sizeof code);
    } __except (av_only(GetExceptionCode())) { return -1; }
    if (!sh_ai_off_limits_getter(code)) return -1;
    __try { v = *((const uint8_t *)actor + SH_AI_OFF_OFF_LIMITS); }
    __except (av_only(GetExceptionCode())) { return -1; }
    return v != 0;
}

static int e_move_status(void *ai)
{
    int v = -1;
    __try {
        const uint8_t *fsm = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        if (fsm) v = *(const int *)(fsm + SH_AI_OFF_MOVE_STATUS);
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The hand the game fires the demon's weapon from: vtable +0xB20, its
 * aibehavior decl's weaponSlot. */
static int e_weapon_hand(void *ai)
{
    typedef int (*ai_hand_fn)(void *self);
    ai_hand_fn f = (ai_hand_fn)vfn(ai, SH_AI_VSLOT_WEAPON_HAND);
    int v = -1;
    if (!f) return -1;
    __try { v = f(ai); } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The weapon in `hand` (an equipSlot_t), as the engine's own per-hand reads
 * take it; NULL when that hand holds none. */
static uint8_t *weapon_in(void *ai, int hand)
{
    uint8_t *w = NULL;
    if (!g_weapon_in_slot) return NULL;
    __try { w = (uint8_t *)g_weapon_in_slot(ai, hand); } __except (av_only(GetExceptionCode())) { w = NULL; }
    return w;
}

static int e_item(void *ai, int index, char *decl, size_t cap, int *hand)
{
    typedef void *(*ai_list_fn)(void *self);
    ai_list_fn f = (ai_list_fn)vfn(ai, SH_AI_VSLOT_INVENTORY);
    int v = -1;
    if (!f || !g_inventory_item || !decl || !cap) return -1;
    decl[0] = 0;
    __try {
        void *list = f(ai);
        const uint8_t *item, *d;
        const char *name;
        if (!list) return -1;
        if (index >= *(const int *)((const uint8_t *)list + 8)) return 0;
        item = (const uint8_t *)g_inventory_item(list, index);
        d = item ? *(const uint8_t * const *)(item + SH_AI_ITEM_DECL) : NULL;
        name = d ? *(const char * const *)(d + 8) : NULL;
        if (!name) return -1;
        strncpy_s(decl, cap, name, _TRUNCATE);
        if (hand) *hand = *(const int *)(d + SH_AI_DECL_EQUIP_SLOT);
        v = 1;
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_held(void *ai, int hand, char *decl, size_t cap)
{
    uint8_t *w = weapon_in(ai, hand);
    int v = 0;
    if (!decl || !cap) return -1;
    decl[0] = 0;
    if (!w) return 0;
    __try {
        const uint8_t *d = *(const uint8_t * const *)(w + SH_AI_ITEM_DECL);
        const char *name = d ? *(const char * const *)(d + 8) : NULL;
        if (name) { strncpy_s(decl, cap, name, _TRUNCATE); v = 1; }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The demon's temp memory read whole (its 256 key pointers, each key's text copied while it is readable), then
 * sh_ai_order_find. The keys are interned GAME_STRINGs the engine never frees. */
static int e_snapmap_order(void *ai, sh_ai_order *out)
{
    static char text[SH_AI_TEMP_SLOTS][32];
    const char *keys[SH_AI_TEMP_SLOTS];
    uint32_t values[SH_AI_TEMP_SLOTS];
    int i, ok = 1;
    __try {
        const uint8_t *mem = (const uint8_t *)ai + SH_AI_OFF_TEMP_MEMORY;
        memcpy(values, mem, sizeof values);
        for (i = 0; i < SH_AI_TEMP_SLOTS; ++i) {
            const char *k = *(const char * const *)(mem + SH_AI_TEMP_KEYS + (size_t)i * 8);
            keys[i] = NULL;
            /* only keys that begin like an order are copied (the rest are other states' entries) */
            if (k && k[0] == 's' && !strncmp(k, "snapmap_force", 13)) {
                strncpy_s(text[i], sizeof text[i], k, _TRUNCATE);
                keys[i] = text[i];
            }
        }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) return -1;
    return sh_ai_order_find(keys, values, SH_AI_TEMP_SLOTS, out);
}

static int e_trigger_state(void *ai, int hand)
{
    int v = -1;
    uint8_t *w = weapon_in(ai, hand);
    __try {
        if (w) {
            int slot = *(const int *)(w + SH_AI_OFF_WEAPON_SLOT);
            if (slot >= 0 && slot < 4) v = *(const int *)(w + SH_AI_OFF_WEAPON_TRIGGER + 4 * slot) == 0;
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The engine's fire control (at ai +0x22278), the aim focus and the muzzle test's inputs (script_api_ai.h paths). */
static int e_fire_modes(void *ai, int *state, int *anim)
{
    int v = -1;
    __try {
        const uint8_t *fc = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_WEAPON_HOLDER);
        if (fc) {
            *state = *(const int *)(fc + SH_AI_FC_MODE_STATE);
            *anim = *(const int *)(fc + SH_AI_FC_MODE_ANIM);
            v = 1;
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_focus_state(void *ai, sh_ai_focus_state *out)
{
    int v = -1;
    __try {
        const uint8_t *a = (const uint8_t *)ai;
        const uint8_t *fc = *(const uint8_t * const *)(a + SH_AI_OFF_WEAPON_HOLDER);
        out->valid = a[SH_AI_OFF_FOCUS_VALID] != 0;
        memcpy(out->point, a + SH_AI_OFF_FOCUS_FINAL, 12);
        out->radius = *(const float *)(a + SH_AI_OFF_FOCUS_RADIUS);
        out->precision = *(const int *)(a + SH_AI_OFF_FOCUS_PRECISION);
        out->tracking = a[SH_AI_OFF_FOCUS_TRACKING] != 0;
        out->body_rotation = a[SH_AI_OFF_FOCUS_BODY_ROT] != 0;
        out->require_muzzle = fc ? fc[SH_AI_FC_REQUIRE_MUZZLE] != 0 : -1;
        v = 1;
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The engine's muzzle test (ai, weights, weapon) -> its bool in al. */
static uint64_t (*g_muzzle)(void *ai, int mode, void *weapon);
static void (*g_clear_move)(void *anim_fsm, int result_code);
static void (*g_reset_motion_correction)(void *anim_fsm);

static int e_on_ground(void *ai)
{
    __try {
        const uint8_t *physics = *(const uint8_t *const *)((const uint8_t *)ai + SH_AI_OFF_PHYSICS);
        if (physics == (const uint8_t *)ai + SH_AI_PHYSICS_OBJECT)
            return physics[SH_AI_PHYSICS_ON_GROUND] != 0;
    } __except (av_only(GetExceptionCode())) {}
    return -1;
}

static int e_reset_motion_correction(void *ai)
{
    int ok = 0;
    __try {
        void *fsm = *(void *const *)((const uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        if (fsm && *(void *const *)fsm == ai) {
            g_reset_motion_correction(fsm);
            ok = 1;
        }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_muzzle_test(void *ai, int hand, int mode)
{
    int v = -1;
    uint8_t *w = weapon_in(ai, hand);
    if (!g_muzzle || !w) return -1;
    __try { v = (g_muzzle(ai, mode, w) & 0xFF) != 0; } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_clear_move(void *ai)
{
    int v = -1;
    if (!g_clear_move) return -1;
    __try {
        void *fsm = *(void * const *)((const uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        if (fsm) { g_clear_move(fsm, 0); v = 1; }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The animWeb command context of a demon (its vtable's accessor). */
static void (*g_cmd_set_wait)(void *ctx, void *handle);
static uint8_t *cmd_ctx(void *ai)
{
    typedef void *(*ai_ctx_fn)(void *self);
    ai_ctx_fn f = (ai_ctx_fn)vfn(ai, SH_AI_VSLOT_CMD_CTX);
    return f ? (uint8_t *)f(ai) : NULL;
}

static int e_step_state(void *ai, sh_ai_step_state *out)
{
    int v = -1;
    __try {
        const uint8_t *fsm = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        const uint8_t *ctx = cmd_ctx(ai);
        if (fsm && ctx) {
            const uint8_t *wait = *(const uint8_t * const *)(ctx + SH_AI_CMDCTX_ACTIVE);
            int status = wait ? *(const int *)(wait + SH_AI_WAIT_STATUS) : -1;
            out->moving = *(const int *)(fsm + SH_AI_FSM_STATE) == SH_AI_FSM_STATE_MOVE;
            out->stage = *(const int *)(fsm + SH_AI_FSM_STAGE);
            out->waiting = status == SH_AI_AWCMD_WAITING || status == SH_AI_AWCMD_READY;
            out->idle_turn = (*(const int *)(fsm + SH_AI_FSM_MOVE_FLAGS) & SH_AI_FSM_IDLE_TURN_FLAG)
                                 ? *(const int *)(fsm + SH_AI_FSM_IDLE_TURN) : -1;
            v = 1;
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The context's set-active-wait-handle with no handle: the held command gets its done event and the status
 * INTERRUPTED, as when another command takes its place. */
static int e_step_interrupt(void *ai)
{
    int v = -1;
    if (!g_cmd_set_wait) return -1;
    __try {
        uint8_t *ctx = cmd_ctx(ai);
        if (ctx) {
            v = *(void * const *)(ctx + SH_AI_CMDCTX_ACTIVE) != NULL;
            if (v) g_cmd_set_wait(ctx, NULL);
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* The game's spawn inits the gore component unless the behaviour decl's monster type is generic; a mod's demon
 * is generic, so this makes the same call with the same values. */
typedef void (*ai_gore_init_fn)(void *gore, void *ai, void *damage, void *a, void *graph, void *b, void *c);
static ai_gore_init_fn g_gore_init;
static const int32_t *g_gore_cvar;
static uint32_t g_gore_comp, g_gore_damage;

static int e_gore_init(void *ai)
{
    int v = 0;
    if (!g_gore_init) return -1;
    __try {
        uint8_t *a = (uint8_t *)ai;
        const uint8_t *beh = *(const uint8_t * const *)(a + SH_AI_OFF_BEHAVIOR_DECL);
        uint8_t *gore = a + g_gore_comp;
        void *graph = *(void * const *)(gore + SH_AI_GORE_GRAPH);
        if (beh && *(const int32_t *)(beh + SH_AI_BEHAVIOR_MONSTER) == SH_AI_MONSTER_GENERIC && graph &&
            !*(void * const *)(gore + SH_AI_GORE_OWNER) && *g_gore_cvar != 0) {
            static int logged;
            g_gore_init(gore, ai, a + g_gore_damage, NULL, graph, NULL, NULL);
            v = 1;
            if (!logged++)
                backend_log("SCRIPTS: a demon of the generic monster type was given its gore component "
                            "(the game's spawn leaves it out)");
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_idle_turn_entry(void *ai, int turn)
{
    int v = -1;
    __try {
        const uint8_t *decl = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_BEHAVIOR_DECL);
        if (decl) {
            const uint8_t *list = *(const uint8_t * const *)(decl + SH_AI_BEHAVIOR_TURNPARMS);
            int count = *(const int *)(decl + SH_AI_BEHAVIOR_TURNCOUNT);
            v = (list && turn >= 0 && turn < count && *(const void * const *)(list + (size_t)turn * 8) != NULL) ? 1 : 0;
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_path(void *ai, const float goal[3], int key, float *len)
{
    struct { uint64_t global; uint64_t name; int key; int pad; } q;
    float reach[3] = { 0, 0, 0 }, length = 0;
    int st = -1;
    if (!g_path_query || !g_path_key || !g_path_global) return -1;
    __try {
        void *fsm = *(void **)((uint8_t *)ai + SH_AI_OFF_MOVE_FSM);
        const char *name = *(const char * const *)((uint8_t *)ai + SH_AI_OFF_PATH_NAME);
        const uint64_t *slot = (const uint64_t *)g_path_key(ai, 0, name);
        if (fsm && slot) {
            q.global = *g_path_global;
            q.name = *slot;
            q.key = key;
            q.pad = 0;
            st = g_path_query(fsm, &q, goal, 0.0f, reach, &length, 0);
        }
    } __except (av_only(GetExceptionCode())) { st = -1; }
    if (st == 0 || st == 1) { *len = length; return 0; }
    if (st == 2) return 1;
    return st < 0 ? -1 : 2;
}

static uint32_t e_frame(void) { return sh_script_hooks_frame(); }

/* ---------------------------------------------------------------- park */

/* One slot per parked demon. The engine's FSM threads read it without a lock, so it never moves: it has one
 * slot per entity the game can hold, is read only below the high-water mark, and reuses a gone demon's slot. */
#define AI_PARKED_MAX SH_SCRIPT_ENGINE_ENTITIES
static sh_ai_park_slot g_parked[AI_PARKED_MAX];   /* sh_ai_park_drives, _release, _take */
static volatile LONG g_parked_used;   /* high-water mark of used slots */

/* The slot of `ai` while it is the same entity (its spawn id matches), -1
 * none. FSM worker threads and the game thread; reads only. */
static int parked_slot(void *ai)
{
    int i, used = (int)g_parked_used;
    if (!ai) return -1;
    for (i = 0; i < used; i++) {
        if (g_parked[i].ai == ai) {
            uint32_t id = spawn_id_of(ai);
            if (id != SH_SCRIPT_SPAWN_ID_NONE && id == (uint32_t)g_parked[i].spawn_id) return i;
        }
    }
    return -1;
}

/* A script drives the demon: parked and not released. */
static int script_drives(void *ai)
{
    int slot = parked_slot(ai);
    return slot >= 0 && sh_ai_park_drives(&g_parked[slot], spawn_id_of(ai));
}

/* What the engine's FSM is told about a parked demon this tick (sh_ai_park_holds), from the demon's own fields;
 * unreadable fields leave it parked. */
#define AI_HEALTH_CUR        (0x2B290u + 0x1Cu)  /* idAI2: idAIHealth +0x2B290, current +0x1C (script_api_common.c) */
#define AI_PAIN_JOINT_GROUP  0x8510u             /* idAI2: damageInfo.painJointGroupIndex, -1 when none (VK 0x140403DD0) */
static int parked_now(void *ai, LONG pain_until)
{
    float health = 1.0f;
    int group = -1, reaction = SH_COMBAT_PAIN_NONE;
    __try {
        health = *(const float *)((const uint8_t *)ai + AI_HEALTH_CUR);
        group = *(const int *)((const uint8_t *)ai + AI_PAIN_JOINT_GROUP);
        reaction = *(const int *)((const uint8_t *)ai + SH_COMBAT_AI_PAIN);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return sh_ai_park_holds(!(health <= 0.0f), group >= 0 || pain_until != 0, reaction);
}

/* IsRunningScript(ai): for a parked, unreleased demon whose spawn id still matches, what the park holds; else
 * the engine's answer. FSM worker threads; reads only. */
static uint64_t irs_detour(void *ai)
{
    int slot = parked_slot(ai);
    if (slot >= 0 && sh_ai_park_drives(&g_parked[slot], spawn_id_of(ai)))
        return (uint64_t)parked_now(ai, g_parked[slot].pain_until);
    return g_irs_orig(ai);
}

/* The move system's transition query (job threads). The engine's overshoot rule rejects a turn clip whose
 * root travels, so the request's own bit that lifts the rule is set for the turn of a demon a script drives;
 * every other move is left to the engine. */
typedef int (*ai_move_fn)(void *move, void *request, void *out, int kind);
static ai_move_fn g_move_target, g_move_orig;

static int move_detour(void *move, void *request, void *out, int kind)
{
    if (kind == SH_AI_MOVE_KIND_TURN && move && request) {
        __try {
            void *ai = *(void *const *)move;
            if (script_drives(ai)) *((uint8_t *)request + SH_AI_MOVE_REQ_OVERSHOOT) |= 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return g_move_orig(move, request, out, kind);
}

/* Committed with the park hook. Without it a script's face() still turns a
 * demon whose turn clips stay in place. */
static void commit_move_detour(void)
{
    void *tramp;
    if (g_move_orig || !g_move_target) return;
    if (memcmp((const void *)g_move_target, k_ai_move_prologue, SH_AI_MOVE_STOLEN) != 0) { g_move_target = NULL; return; }
    tramp = hook_prepare((void *)g_move_target, (void *)move_detour, SH_AI_MOVE_STOLEN);
    if (!tramp) { g_move_target = NULL; return; }
    g_move_orig = (ai_move_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_move_orig = NULL;
        g_move_target = NULL;
        ai_log("Mod scripts: the turn hook could not be installed; a demon whose turn clips step is not turned by "
               "face() in this session.");
        return;
    }
    ai_log("SCRIPTS: turn hook installed at %p", (void *)g_move_target);
}

static int commit_detour(void)
{
    void *tramp;
    if (g_irs_orig && hook_is_installed((void *)g_irs_orig)) return 1;
    if (g_detour_failed || !g_irs_target) return 0;
    if (memcmp((const void *)g_irs_target, k_ai_irs_prologue, SH_AI_IRS_STOLEN) != 0) {
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the game's script-state function is not the "
               "expected one (another tool may have changed it).");
        return 0;
    }
    tramp = hook_prepare((void *)g_irs_target, (void *)irs_detour, SH_AI_IRS_STOLEN);
    if (!tramp) {
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the hook could not be prepared (%s).",
               hook_last_failure());
        return 0;
    }
    g_irs_orig = (ai_irs_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_irs_orig = NULL;
        g_detour_failed = 1;
        ai_log("Mod scripts cannot drive demons in this session: the hook could not be installed.");
        return 0;
    }
    ai_log("SCRIPTS: demon park hook installed at %p", (void *)g_irs_target);
    commit_move_detour();
    return 1;
}

/* ---- the engine's pain and death on a parked demon: requests to the alert cycle FSM, which refuses them while
 * a parked demon's cycle sits in ExecuteDoomscript. The request is detoured: on such a refusal the park opens
 * (IsRunningScript answers false), the cycle is asked to leave and the code is asked again inside the engine's
 * call. The park holds again once the reaction has played or after SH_AI_PAIN_TAKE_MS. */
typedef int      (*ai_pain_code_fn)(void *ai);
typedef uint8_t  (*ai_fsm_request_fn)(void *fsm, int code);
typedef void    *(*ai_fsm_get_fn)(void *owner);
static ai_fsm_request_fn g_fsm_request_target, g_fsm_request_orig;
static ai_pain_code_fn   g_pain_code;
static int               g_fsm_request_failed;
static int gm_clock(int *ms);
static int commit_hook(void *target, void *detour, const uint8_t *want, size_t stolen, void **orig, int *failed,
                       const char *what);

static void *alert_fsm_of(void *ai)
{
    void *owner = (uint8_t *)ai + SH_AI_FSM_OWNER, *fsm = NULL;
    __try {
        ai_fsm_get_fn get = (ai_fsm_get_fn)vfn(owner, SH_AI_FSM_GET_SLOT);
        if (get) fsm = get(owner);
    } __except (av_only(GetExceptionCode())) { fsm = NULL; }
    return fsm;
}

static int parked_slot_of_fsm(void *fsm)
{
    int i, used = (int)g_parked_used;
    for (i = 0; i < used; i++) {
        void *ai = g_parked[i].ai;
        if (!ai || g_parked[i].released || spawn_id_of(ai) != (uint32_t)g_parked[i].spawn_id) continue;
        if (alert_fsm_of(ai) == fsm) return i;
    }
    return -1;
}

/* The alert cycle of a demon (-1 unreadable). */
static int alert_cycle_of(void *ai)
{
    int v = -1;
    __try { v = *(const int *)((const uint8_t *)ai + SH_AI_OFF_ALERT_CYCLE); }
    __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

/* An FSM's name and the state it is in now, "name/state", by the game's reflection: idFiniteStateMachine.name and
 * .curState -> idState.stateData -> idStateData.nameFromDecl (idAtomicStrings: the text's pointer). "?" for a part
 * that is unreadable. The offsets are looked up once. */
static volatile LONG g_fsm_rf;   /* 0 not looked up, 1 found, -1 missing */
static uint32_t g_fsm_o_name, g_fsm_o_cur, g_fsm_o_data, g_fsm_o_state;

static int fsm_rf_ready(void)
{
    const sh_rf_host *h;
    sh_rf_field f;
    if (g_fsm_rf) return g_fsm_rf > 0;
    h = sh_script_reflect_host();
    if (!h) return 0;
    if (!sh_rf_find_field(h, "idFiniteStateMachine", "name", &f) || f.size != 8) goto missing;
    g_fsm_o_name = f.offset;
    if (!sh_rf_find_field(h, "idFiniteStateMachine", "curState", &f)) goto missing;
    g_fsm_o_cur = f.offset;
    if (!sh_rf_find_field(h, "idState", "stateData", &f)) goto missing;
    g_fsm_o_data = f.offset;
    if (!sh_rf_find_field(h, "idStateData", "nameFromDecl", &f) || f.size != 8) goto missing;
    g_fsm_o_state = f.offset;
    InterlockedExchange(&g_fsm_rf, 1);
    return 1;
missing:
    InterlockedExchange(&g_fsm_rf, -1);
    return 0;
}

static size_t copy_text(const char *s, char *out, size_t at, size_t cap)
{
    if (!s) s = "?";
    while (at + 1 < cap && *s) out[at++] = *s++;
    out[at] = 0;
    return at;
}

static void fsm_state_name(void *fsm, char *out, size_t cap)
{
    size_t at = 0;
    if (!cap) return;
    strcpy_s(out, cap, "?");
    if (!fsm || !fsm_rf_ready()) return;
    __try {
        const uint8_t *f = (const uint8_t *)fsm;
        const uint8_t *st = *(const uint8_t *const *)(f + g_fsm_o_cur);
        const uint8_t *data = st ? *(const uint8_t *const *)(st + g_fsm_o_data) : NULL;
        at = copy_text(*(const char *const *)(f + g_fsm_o_name), out, 0, cap);
        at = copy_text("/", out, at, cap);
        copy_text(data ? *(const char *const *)(data + g_fsm_o_state) : NULL, out, at, cap);
    } __except (av_only(GetExceptionCode())) { strcpy_s(out, cap, "?"); }
}

/* The FSM that controls the demon now (aiVolatile.fsms.fsmManager.controllingFSM), NULL when unreadable. */
static void *controlling_fsm_of(void *ai)
{
    const sh_rf_host *h = sh_script_reflect_host();
    sh_rf_loc loc;
    char err[160];
    void *fsm = NULL;
    if (!h || !ai) return NULL;
    memset(&loc, 0, sizeof loc);
    if (!sh_rf_resolve(h, ai, "idAI2", "aiVolatile.fsms.fsmManager.controllingFSM", &loc, err, sizeof err)) return NULL;
    if (loc.kind == SH_RF_POINTER && loc.addr) {
        __try { fsm = *(void *const *)loc.addr; } __except (av_only(GetExceptionCode())) { fsm = NULL; }
    }
    sh_rf_loc_free(&loc);
    return fsm;
}

/* Logs a dying request on a parked demon (released or not): what became of it, where the alert cycle FSM stood
 * and what the park told the engine. */
static void dying_note(void *fsm, const char *how, uint8_t taken)
{
    int i, used = (int)g_parked_used;
    for (i = 0; i < used; i++) {
        void *ai = g_parked[i].ai;
        char state[96], ctl[96];
        int drives;
        if (!ai || spawn_id_of(ai) != (uint32_t)g_parked[i].spawn_id || alert_fsm_of(ai) != fsm) continue;
        fsm_state_name(fsm, state, sizeof state);
        fsm_state_name(controlling_fsm_of(ai), ctl, sizeof ctl);
        drives = sh_ai_park_drives(&g_parked[i], spawn_id_of(ai));
        ai_log("SCRIPTS: dying request on a parked demon (slot %d, released %d): %s, taken %d, alert cycle %d, "
               "state %s, controlling %s, running script %d", i, (int)g_parked[i].released, how, (int)taken,
               alert_cycle_of(ai), state, ctl,
               drives ? (int)parked_now(ai, g_parked[i].pain_until) : (g_irs_orig ? (int)g_irs_orig(ai) : -1));
        return;
    }
}

/* Leave ExecuteDoomscript inside the caller's request. The way out to ac_default is a STATE link the FSM takes
 * only at its next update (neither the plain request nor the state-link request, VK 0x14064C900, takes it in a
 * call), by which time a dead demon's think reports "dead but not ALERTCYCLE_DYING". So the cycle is moved into
 * ac_default as the engine's entrance move does (SH_AI_SIG_FSM_MOVE): a move record with idTransitionCode and
 * code 5 through the FSM's vtable +0xA8 with the game time. Only a cycle in ExecuteDoomscript is moved. */
typedef void (*ai_game_time_fn)(void *game, int *out);
typedef void (*ai_fsm_take_fn)(void *fsm, void *move, int *time, uint8_t now, uint8_t forced);
static ai_game_time_fn g_game_time;
static const void     *g_fsm_code_vt;   /* idTransitionCode's vtable, from the entrance move's lea */

static int has_native_path(const uint8_t *alert)
{
    const sh_rf_host *host = sh_script_reflect_host();
    sh_rf_field manager, selector, linked;
    sh_ai_path_fields fields;
    if (!host ||
        !sh_rf_find_field(host, "idFiniteStateMachine", "manager", &manager) || manager.size != sizeof(void *) ||
        !sh_rf_find_field(host, "idStateData", "childFSMName", &selector) || selector.size != sizeof(void *) ||
        !sh_rf_find_field(host, "idState", "childFSM", &linked) || linked.size != sizeof(void *)) return 0;
    fields.data = g_fsm_o_data;
    fields.name = g_fsm_o_state;
    fields.manager = manager.offset;
    fields.selector = selector.offset;
    fields.linked = linked.offset;
    return sh_ai_fsm_has_native_path(alert, &fields);
}

/* The stock path state returns to ac_default or yields to a native interaction, pain or death. */
static int e_native_body_busy(void *ai, int check_path)
{
    const uint8_t *fsm = (const uint8_t *)alert_fsm_of(ai);
    int busy = -1;
    if (!fsm || !fsm_rf_ready()) return -1;
    __try {
        const char *state = state_name_of(*(const uint8_t *const *)(fsm + g_fsm_o_cur), g_fsm_o_data, g_fsm_o_state);
        if (state && (!check_path || has_native_path(fsm)))
            busy = strcmp(state, "ac_default") && strcmp(state, "behaviors") && strcmp(state, "ExecuteDoomscript");
    } __except (av_only(GetExceptionCode())) { busy = -1; }
    return busy;
}

/* 1 the alert cycle was moved into ac_default; 0 it was not in ExecuteDoomscript (nothing done); -1 a call, the
 * reflection or ac_default is missing; -2 a fault. *time_ms: the game time the move was given. */
static int fsm_enter_default(void *fsm, int *time_ms)
{
    void *gm = sh_entity_gamemgr();
    int t[4] = { 0, 0, 0, 0 }, n = 0, i, at;
    const char *names[FSM_STATES_MAX];
    const uint8_t *const *states = NULL;
    unsigned char move[SH_AI_FSM_MOVE_SIZE];
    ai_fsm_take_fn take;
    *time_ms = -1;
    if (!fsm || !gm || !g_game_time || !g_fsm_code_vt || !fsm_rf_ready()) return -1;
    __try {
        const uint8_t *f = (const uint8_t *)fsm;
        const char *cur = state_name_of(*(const uint8_t *const *)(f + g_fsm_o_cur), g_fsm_o_data, g_fsm_o_state);
        if (!cur || strcmp(cur, "ExecuteDoomscript")) return 0;
        states = *(const uint8_t *const *const *)(f + SH_AI_FSM_STATES);
        n = *(const int *)(f + SH_AI_FSM_STATE_COUNT);
        if (!states || n <= 0) return -1;
        if (n > FSM_STATES_MAX) n = FSM_STATES_MAX;
        for (i = 0; i < n; i++) names[i] = state_name_of(states[i], g_fsm_o_data, g_fsm_o_state);
        at = sh_ai_fsm_state_find(names, n, "ac_default");
        if (at < 0) return -1;
        take = (ai_fsm_take_fn)vfn(fsm, SH_AI_FSM_TAKE_SLOT);
        if (!take) return -1;
        sh_ai_fsm_move_init(move, g_fsm_code_vt, states[at]);
        g_game_time(gm, t);
        *time_ms = t[0];
        take(fsm, move, t, 1, 1);
    } __except (av_only(GetExceptionCode())) { return -2; }
    return 1;
}

static void dying_leave_note(void *fsm, int moved, int time_ms)
{
    char state[96];
    fsm_state_name(fsm, state, sizeof state);
    ai_log("SCRIPTS: dying request: the move into ac_default answered %d at game time %d (entrance move %s, game time "
           "%s); state now %s", moved, time_ms, g_fsm_code_vt ? "found" : "missing", g_game_time ? "found" : "missing",
           state);
}

static uint8_t fsm_request_detour(void *fsm, int code)
{
    uint8_t taken = g_fsm_request_orig(fsm, code);
    int slot, now = 0;
    if (code == SH_AI_CODE_DYING && fsm && !taken) dying_note(fsm, "first ask", taken);
    if (taken || !fsm || !sh_ai_fsm_code_yields(code)) return taken;
    slot = parked_slot_of_fsm(fsm);
    if (slot < 0 || InterlockedCompareExchange(&g_parked[slot].fsm_busy, 1, 0) != 0) {
        if (code == SH_AI_CODE_DYING) dying_note(fsm, slot < 0 ? "not parked now" : "busy", taken);
        return taken;
    }
    if (gm_clock(&now)) {
        int moved = -1, t = -1;
        InterlockedExchange(&g_parked[slot].pain_until, (LONG)(now + SH_AI_PAIN_TAKE_MS));
        __try {
            moved = fsm_enter_default(fsm, &t);
            if (code == SH_AI_CODE_DYING) dying_leave_note(fsm, moved, t);
            taken = g_fsm_request_orig(fsm, code);
        } __except (av_only(GetExceptionCode())) { taken = 0; }
        if (code == SH_AI_CODE_DYING) dying_note(fsm, "after the move into ac_default", taken);
    } else if (code == SH_AI_CODE_DYING) {
        dying_note(fsm, "no game clock, so not asked again", taken);
    }
    InterlockedExchange(&g_parked[slot].fsm_busy, 0);
    return taken;
}

/* Every frame (the game thread): the park opened for a refused request holds again when the reaction plays, the
 * demon is dead or the time is up; until then a still-refused pain is asked for again. */
static void pain_take_step(void)
{
    int i, used = (int)g_parked_used, now = 0;
    if (!g_fsm_request_orig || !gm_clock(&now)) return;
    for (i = 0; i < used; i++) {
        void *ai = g_parked[i].ai;
        LONG until = g_parked[i].pain_until;
        float health = 0.0f;
        int playing = SH_COMBAT_PAIN_NONE, done = 1;
        if (!until) continue;
        if (ai && spawn_id_of(ai) == (uint32_t)g_parked[i].spawn_id) {
            __try {
                health = *(const float *)((const uint8_t *)ai + AI_HEALTH_CUR);
                playing = *(const int *)((const uint8_t *)ai + SH_COMBAT_AI_PAIN);
                if (sh_ai_pain_take_next(!(health <= 0.0f), playing, now, (int)until)) {
                    void *fsm = alert_fsm_of(ai);
                    int code = g_pain_code ? g_pain_code(ai) : 0;
                    done = fsm && code && g_fsm_request_target(fsm, code);
                }
            } __except (av_only(GetExceptionCode())) { done = 1; }
        }
        if (done) InterlockedExchange(&g_parked[i].pain_until, 0);
    }
}

static void pain_take_commit(void)
{
    if (!g_fsm_request_target) return;
    (void)commit_hook((void *)g_fsm_request_target, (void *)fsm_request_detour, k_ai_fsm_request_prologue,
                      SH_AI_FSM_REQUEST_STOLEN, (void **)&g_fsm_request_orig, &g_fsm_request_failed,
                      "a scripted demon's pain and death");
}

/* ---- a pain a script asks for (ai:request_pain): repeats the engine's pain start (SH_AI_SIG_PAIN_START) with
 * its own calls (script_api_ai.h, SH_AI_PS_*): the gates, CheckPainAnim's clip choice, the request at ai +0x8678
 * and the alert cycle's transition (232 pain, 233 light, 237/238 stagger). The enterPainState event (1450; VK
 * 0x1414A1FAA) only writes the pain field with no state to end it, so it is not used. */
typedef int      (*ps_intensity_fn)(void *ai, void *damage_info, float damage);
typedef int      (*ps_dir_fn)(void *dirs, const float *dir, int n);
typedef void    *(*ps_group_fn)(void *ai, int *out, uint16_t joint);
typedef uint64_t (*ps_check_fn)(void *ai, void *damage_info, uint16_t *joint, int intensity, const float *dir,
                                void *row, int *dir_index, int *reaction, const char **str_a, const char **str_b,
                                int *index, uint8_t *flag_a, uint8_t *flag_b, uint8_t *flag_c);
typedef void    *(*ps_str_fn)(void *str, const char *text);
typedef void     (*ps_handle_fn)(void *handle, void *entity);
typedef void    *(*ps_copy_fn)(void *dst, const void *src);
typedef void     (*ps_dtor_fn)(void *damage_info);
typedef void    *(*ps_busy_fn)(void *p);
typedef void     (*ps_bb_set_fn)(void *bb, const void *key, int value);
typedef void     (*ps_bb_clear_fn)(void *bb, const void *key);
static struct {
    ps_intensity_fn intensity;
    ps_dir_fn       dir_index;
    ps_group_fn     group;
    ps_check_fn     check;
    ps_str_fn       str_assign;
    ps_handle_fn    handle_set;
    ps_copy_fn      damage_copy;
    ps_dtor_fn      damage_dtor;
    ps_copy_fn      assign;
    ps_busy_fn      busy;
    ps_bb_set_fn    bb_set;
    ps_bb_clear_fn  bb_clear;
    const void     *key;
    const char     *empty;
} g_ps;

static void ps_resolve(const uint8_t *s)
{
#define PS_CALL(off) ((const void *)(s + (off) + 5 + rel32_at(s + (off) + 1)))
#define PS_LEA(off)  ((const void *)(s + (off) + 7 + rel32_at(s + (off) + 3)))
    static const uint32_t calls[] = {
        SH_AI_PS_INTENSITY_CALL, SH_AI_PS_DIR_CALL, SH_AI_PS_GROUP_CALL, SH_AI_PS_CHECK_CALL, SH_AI_PS_STR_CALL,
        SH_AI_PS_HANDLE_CALL, SH_AI_PS_DAMAGE_COPY_CALL, SH_AI_PS_ASSIGN_CALL, SH_AI_PS_BUSY_CALL,
        SH_AI_PS_BB_SET_CALL, SH_AI_PS_BB_CLEAR_CALL, SH_AI_PS_DAMAGE_DTOR_CALL
    };
    size_t i;
    memset(&g_ps, 0, sizeof g_ps);
    for (i = 0; i < sizeof calls / sizeof calls[0]; i++) if (s[calls[i]] != 0xE8) return;
    if (memcmp(s + SH_AI_PS_KEY_LEA, "\x48\x8D\x15", 3) || memcmp(s + SH_AI_PS_EMPTY_LEA, "\x48\x8D\x15", 3)) return;
    g_ps.intensity = (ps_intensity_fn)PS_CALL(SH_AI_PS_INTENSITY_CALL);
    g_ps.dir_index = (ps_dir_fn)PS_CALL(SH_AI_PS_DIR_CALL);
    g_ps.group = (ps_group_fn)PS_CALL(SH_AI_PS_GROUP_CALL);
    g_ps.check = (ps_check_fn)PS_CALL(SH_AI_PS_CHECK_CALL);
    g_ps.str_assign = (ps_str_fn)PS_CALL(SH_AI_PS_STR_CALL);
    g_ps.handle_set = (ps_handle_fn)PS_CALL(SH_AI_PS_HANDLE_CALL);
    g_ps.damage_copy = (ps_copy_fn)PS_CALL(SH_AI_PS_DAMAGE_COPY_CALL);
    g_ps.assign = (ps_copy_fn)PS_CALL(SH_AI_PS_ASSIGN_CALL);
    g_ps.busy = (ps_busy_fn)PS_CALL(SH_AI_PS_BUSY_CALL);
    g_ps.bb_set = (ps_bb_set_fn)PS_CALL(SH_AI_PS_BB_SET_CALL);
    g_ps.bb_clear = (ps_bb_clear_fn)PS_CALL(SH_AI_PS_BB_CLEAR_CALL);
    g_ps.damage_dtor = (ps_dtor_fn)PS_CALL(SH_AI_PS_DAMAGE_DTOR_CALL);
    g_ps.key = PS_LEA(SH_AI_PS_KEY_LEA);
    g_ps.empty = (const char *)PS_LEA(SH_AI_PS_EMPTY_LEA);
#undef PS_CALL
#undef PS_LEA
}

static int ps_why(char *why, size_t cap, const char *text)
{
    if (why && cap) _snprintf_s(why, cap, _TRUNCATE, "%s", text);
    return 0;
}

/* The pain start's gates that need no hit, in its order. NULL passes. */
static const char *ps_gate(void *ai, int reaction)
{
    const uint8_t *a = (const uint8_t *)ai;
    const uint8_t *bdef, *fsm_anim, *table;
    int injured, now = 0;
    const char *why = NULL;
    uint8_t (*dead)(void *);
    if (a[SH_AI_PS_OFF_REMOTE]) return "this machine does not start the demon's pain";
    if (a[SH_AI_PS_OFF_FORCED_A] && a[SH_AI_PS_OFF_FORCED_B]) return "the demon's pain is forced elsewhere";
    if (*(const int *)(a + SH_AI_PS_OFF_DISABLED)) return "the demon's pain is disabled";
    if (!sh_ai_request_pain_admits(reaction, *(const int *)(a + SH_COMBAT_AI_PAIN), &why)) return why;
    if (gm_clock(&now) && sh_ai_blocked(ai, spawn_id_of(ai), reaction, now)) return "the reaction is blocked (block_pain)";
    bdef = *(const uint8_t *const *)(a + SH_AI_PS_OFF_BEHAVIOR);
    if (!bdef || !(bdef[0xD4] & 1)) return "the demon cannot take damage";
    if (bdef[0xD6] & 4) return "the demon's injured state disables pain";
    dead = (uint8_t (*)(void *))vfn(ai, SH_AI_PS_VSLOT_DEAD);
    if (!dead || dead(ai)) return "the demon is dead";
    fsm_anim = *(const uint8_t *const *)(a + SH_AI_PS_OFF_ANIMFSM);
    if (!fsm_anim || fsm_anim[0x199C]) return "the demon plays a traversal";
    injured = *(const int *)(a + SH_AI_PS_OFF_INJURED);
    table = *(const uint8_t *const *)(a + SH_AI_PS_OFF_INJURED_TABLE);
    if (injured >= 1 && (!table || !(table[0x44 + (size_t)injured * 0x48] & 2))) return "the demon's injured state takes no pain";
    if (!a[SH_AI_PS_OFF_CAN_PAIN]) return "the demon takes no pain";
    return NULL;
}

static int e_request_pain(void *ai, int reaction, float damage, char *why, size_t cap)
{
    __declspec(align(16)) uint8_t req[0x6A0];
    __declspec(align(16)) uint8_t row[0x40];
    uint8_t *a = (uint8_t *)ai, *bb;
    const char *refused = NULL, *str_a, *str_b;
    void *fsm, *enemy, *damage_info;
    float dir[3] = { 0.0f, 0.0f, 0.0f }, me[3], them[3], len;
    uint16_t joint = 0xFFFF;
    int intensity = 1, dir_index = 0, group = 0, index = 0, asked = reaction, code = 0, flags;
    uint8_t flag_a = 1, flag_b = 0, flag_c = 0, taken = 0;
    if (!g_ps.check || !g_pain_code || !g_fsm_request_target) return -1;
    if (reaction <= SH_COMBAT_PAIN_NONE || reaction >= SH_AI_PAIN_REACTIONS) return ps_why(why, cap, "it is not a pain reaction");
    __try {
        refused = ps_gate(ai, reaction);
        if (refused) return ps_why(why, cap, refused);
        fsm = alert_fsm_of(ai);
        if (!fsm) return ps_why(why, cap, "the demon has no alert cycle");
        /* the hit: from the enemy toward the demon, or from the front (the engine's own scripted pushback) */
        enemy = e_enemy(ai);
        if (enemy && e_origin(ai, me) && e_origin(enemy, them)) {
            dir[0] = me[0] - them[0]; dir[1] = me[1] - them[1]; dir[2] = me[2] - them[2];
        }
        len = (float)sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len > 1e-3f) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
        else {
            float yaw = 0.0f;
            if (!e_yaw(ai, &yaw)) return ps_why(why, cap, "the demon's facing is unreadable");
            dir[0] = -(float)cos(yaw * AI_PI / 180.0); dir[1] = -(float)sin(yaw * AI_PI / 180.0); dir[2] = 0.0f;
        }
        damage_info = a + SH_AI_PS_OFF_REQUEST + SH_AI_PS_REQ_DAMAGE;
        intensity = g_ps.intensity(ai, damage_info, damage);
        dir_index = g_ps.dir_index(a + SH_AI_PS_OFF_DIRS, dir, 4);
        g_ps.group(ai, &group, joint);
        memset(row, 0, sizeof row);
        str_a = str_b = g_ps.empty;
        if (!(uint8_t)g_ps.check(ai, damage_info, &joint, intensity, dir, row, &dir_index, &asked, &str_a, &str_b,
                                 &index, &flag_a, &flag_b, &flag_c))
            return ps_why(why, cap, "the pain graph has no clip for it");
        {
            const char **left = (const char **)(a + SH_AI_PS_OFF_PAIN_STRINGS);
            if (left[0] && left[0][0]) { str_b = left[0]; g_ps.str_assign(&left[0], g_ps.empty); }
            if (left[1] && left[1][0]) { str_a = left[1]; g_ps.str_assign(&left[1], g_ps.empty); }
        }
        memset(req, 0, sizeof req);
        memcpy(req + 0x00, &asked, 4);
        memcpy(req + 0x08, &str_a, 8);
        memcpy(req + 0x10, &str_b, 8);
        memcpy(req + 0x18, &index, 4);
        flags = (flag_a ? 1 : 0) | (flag_b ? 2 : 0) | (flag_c ? 4 : 0);
        memcpy(req + 0x1C, &flags, 4);
        memcpy(req + 0x20, &joint, 2);
        {
            static const int32_t none_id = 0x1FFFFFE;
            static const int64_t none_spawn = -1;
            memcpy(req + 0x28, &none_id, 4); memcpy(req + 0x2C, &none_spawn, 8);
            memcpy(req + 0x40, &none_id, 4); memcpy(req + 0x44, &none_spawn, 8);
            g_ps.handle_set(req + 0x28, enemy);
            g_ps.handle_set(req + 0x40, enemy);
        }
        memcpy(req + 0x58, &group, 4);
        memcpy(req + 0x5C, &dir_index, 4);
        memcpy(req + 0x60, &intensity, 4);
        g_ps.damage_copy(req + SH_AI_PS_REQ_DAMAGE, damage_info);
        memcpy(req + SH_AI_PS_REQ_DIR, dir, 12);
        g_ps.assign(a + SH_AI_PS_OFF_REQUEST, req);
        if (g_ps.busy(*(void **)((uint8_t *)fsm + 0x60))) refused = "the alert cycle is moving";
        else {
            bb = a + SH_AI_PS_OFF_BLACKBOARD;
            if (!(*(const uint8_t *const *)(a + SH_AI_PS_OFF_BB_FLAG))[0xD8]) g_ps.bb_clear(bb, g_ps.key);
            else g_ps.bb_set(bb, g_ps.key, 1);
            code = g_pain_code(ai);
            taken = code ? g_fsm_request_target(fsm, code) : 0;
            g_ps.bb_clear(bb, g_ps.key);
            if (!taken) refused = code ? "the alert cycle refused it" : "the alert cycle takes no pain now";
        }
        g_ps.damage_dtor(req + SH_AI_PS_REQ_DAMAGE);
    } __except (av_only(GetExceptionCode())) {
        return ps_why(why, cap, "the demon's memory was unreadable");
    }
    if (refused) return ps_why(why, cap, refused);
    return 1;
}

static int e_park(void *ai)
{
    uint32_t id = spawn_id_of(ai);
    int i, free_slot = -1, used = (int)g_parked_used;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !commit_detour()) return 0;
    for (i = 0; i < used; i++) {
        void *p = g_parked[i].ai;
        if (p == ai && (uint32_t)g_parked[i].spawn_id == id) {
            sh_ai_park_take(&g_parked[i]);   /* a command after ai:release() takes the demon back */
            return 1;
        }
        if (free_slot < 0 && (!p || spawn_id_of(p) != (uint32_t)g_parked[i].spawn_id)) free_slot = i;
    }
    if (free_slot < 0 && used < AI_PARKED_MAX) free_slot = used;
    if (free_slot < 0) return 0;   /* every entity the game can hold is a parked demon */
    g_parked[free_slot].ai = NULL;
    InterlockedExchange(&g_parked[free_slot].pain_until, 0);
    InterlockedExchange(&g_parked[free_slot].released, 0);
    InterlockedExchange(&g_parked[free_slot].spawn_id, (LONG)id);
    InterlockedExchangePointer((PVOID volatile *)&g_parked[free_slot].ai, ai);
    if (free_slot >= used) InterlockedExchange(&g_parked_used, free_slot + 1);
    pain_take_commit();
    return 1;
}

/* ai:release(): the slot stays the demon's (the next park finds it) and is
 * marked released; a pain the park had opened for is the engine's now. */
static int e_release(void *ai)
{
    int slot = parked_slot(ai);
    if (slot >= 0) sh_ai_park_release(&g_parked[slot]);
    return 1;
}

/* The anim web's set-variable-by-name (SH_AI_SIG_WEB_SCALAR_SET). */
typedef uint8_t (*ai_web_scalar_set_fn)(void *animator, const char *const *atom, float value);
static ai_web_scalar_set_fn g_web_scalar_set;
static const char *atom_of(const char *text);

static int e_anim_variable(void *ai, const char *name, float value)
{
    const char *atom = atom_of(name);
    uint8_t found = 0;
    if (!g_web_scalar_set || !atom) return -1;
    __try {
        found = g_web_scalar_set((uint8_t *)ai + SH_AI_OFF_WEB_ANIMATOR, &atom, value);
    } __except (av_only(GetExceptionCode())) { return -1; }
    return found ? 1 : 0;
}

/* ------------------------------------------ the brain's calls: engine side */

typedef void *(*ai_anim_of_prop_fn)(void *prop, void *model, int variant);
typedef void  (*ai_anim_delta_fn)(void *anim, float *delta, float *axis);
typedef void *(*ai_fire_decl_fn)(void *weapon, int mode);
typedef void  (*ai_focus_fn)(void *ai, int focus, const float *pos, uint8_t keep, int ms, int reason);
typedef void  (*ai_intern_fn)(const char **slot, const char *text);
typedef uint64_t (*ai_threat_row_fn)(void *threat, void *actor, const uint8_t *row, int64_t state, uint64_t a5,
                                     uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11);
typedef float (*ai_path_length_fn)(void *move_fsm, const uint8_t *path);
typedef int  *(*ai_clock_fn)(void *gm, int *out, int group);

static ai_anim_of_prop_fn g_anim_of_prop;
static ai_anim_delta_fn   g_anim_delta;
static ai_fire_decl_fn    g_fire_decl;
static const int32_t     *g_slot_table;
static ai_focus_fn        g_focus;
static ai_intern_fn       g_intern;
static uint8_t           *g_recoil_site;      /* checked again before each write */
static uint8_t           *g_pain_site;        /* the 14 bytes the stub replaces */
static ai_threat_row_fn   g_threat_target, g_threat_orig;
static ai_path_length_fn  g_plen_target, g_plen_orig;
static void              *g_pain_tramp;
static uint8_t           *g_pain_stub;
static int                g_threat_failed, g_plen_failed, g_pain_failed;

#define AI_NODE_LIST        0x58u    /* animWeb decl: node pointers, count +0x60 */
#define AI_NODE_COUNT       0x60u
#define AI_NODE_SUBWEB      0x02u    /* node: short subweb index, short state-name index */
#define AI_NODE_STATE       0x04u
#define AI_NODE_MODELS      0x38u    /* node: model rows (stride 0x40, short key +0), count +0x40 */
#define AI_NODE_MODEL_COUNT 0x40u
#define AI_MODEL_ROW        0x40u
#define AI_MODEL_PROPS      0x18u    /* model row: animation props (stride 0x18), count +0x20 */
#define AI_MODEL_PROP_COUNT 0x20u
#define AI_PROP_STRIDE      0x18u
#define AI_WEB_MODELS       0xA0u    /* animWeb decl: model list (+8 of its first row), count +0xA8 */
#define AI_WEB_MODEL_COUNT  0xA8u
#define AI_WEB_SUBWEBS      0x70u
#define AI_WEB_SUBWEB_COUNT 0x78u
#define AI_WEB_STATES       0x88u
#define AI_WEB_STATE_COUNT  0x90u
#define AI_ANIM_FRAMES      0x58u    /* idMD6Anim: the frame data the delta reads (NULL: none) */
#define AI_HOLDER_SLOT_BASE 4        /* holder + (slot + 4) * 0x10: the next-pull time */

/* The game clock the weapon holder and the threat table read: idGameLocal vtable +0x810 (game, out, group 1)
 * gives int milliseconds. Any thread; 1 when read. */
#define AI_GAME_CLOCK_SLOT 0x810u
static int gm_clock(int *ms)
{
    void *gm = sh_entity_gamemgr();
    ai_clock_fn f = (ai_clock_fn)vfn(gm, AI_GAME_CLOCK_SLOT);
    int buf[4] = { 0, 0, 0, 0 }, ok = 0;
    if (!gm || !f) return 0;
    __try {
        const int *t = f(gm, buf, 1);
        if (t) { *ms = *t; ok = 1; }
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

static int e_eye(void *ai, float out[3])
{
    sh_script_earg r;
    float o[3];
    if (!e_origin(ai, o) || !sh_script_api_event(ai, "getMaxs", NULL, 0, &r) || r.type != 'v' || r.v[2] <= 0) return 0;
    out[0] = o[0]; out[1] = o[1]; out[2] = o[2] + r.v[2] * 0.9f;   /* nine tenths of the demon's height */
    return 1;
}

static int e_sight(void *ai, const float from[3], const float to[3], void *target)
{
    const sh_script_engine *c = sh_script_api_engine();
    sh_script_trace t;
    if (!c || !c->trace || !c->trace(from, to, spawn_id_of(ai), &t)) return -1;
    if (!(t.fraction < 1.0f)) return 1;
    return target && t.hit_spawn_id != SH_SCRIPT_SPAWN_ID_NONE && t.hit_spawn_id == spawn_id_of(target);
}

static int e_ground(const float pos[3], float out[3])
{
    const sh_script_engine *c = sh_script_api_engine();
    return c && c->ground && c->ground(pos, out);
}

/* The engine's world trace from `from` to `to`, passing through up to AI_WORLD_HOPS players and demons
 * (the first trace passes `ai`): 1 with the world's hit in *t, 0 nothing hit, -1 unreadable. */
#define AI_WORLD_HOPS 6
static int world_trace(void *ai, const float from[3], const float to[3], sh_script_trace *t)
{
    const sh_script_engine *c = sh_script_api_engine();
    float at[3], dir[3], len;
    uint32_t ignore = spawn_id_of(ai);
    int hop, i;
    if (!c || !c->trace) return -1;
    memcpy(at, from, sizeof at);
    for (i = 0; i < 3; i++) dir[i] = to[i] - from[i];
    len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-3f)) return 0;
    for (i = 0; i < 3; i++) dir[i] /= len;
    for (hop = 0; hop < AI_WORLD_HOPS; hop++) {
        void *hit;
        int kind;
        if (!c->trace(at, to, ignore, t)) return -1;
        if (!(t->fraction < 1.0f)) return 0;
        if (t->hit_spawn_id == SH_SCRIPT_SPAWN_ID_NONE) return 1;
        hit = c->resolve ? c->resolve(t->hit_spawn_id) : NULL;
        kind = hit && c->kind ? c->kind(hit) : SH_SCRIPT_KIND_OTHER;
        if (kind != SH_SCRIPT_KIND_AI && kind != SH_SCRIPT_KIND_PLAYER) return 1;
        ignore = t->hit_spawn_id;                                 /* go on past it */
        for (i = 0; i < 3; i++) at[i] = t->end[i] + dir[i] * 2.0f;
    }
    return 1;
}

static int e_world_clear(void *ai, const float from[3], const float to[3])
{
    sh_script_trace t;
    int st = world_trace(ai, from, to, &t);
    return st < 0 ? -1 : st == 0;
}

static int e_world_ground(void *ai, const float pos[3], float out[3])
{
    sh_script_trace t;
    float from[3], to[3];
    from[0] = to[0] = pos[0];
    from[1] = to[1] = pos[1];
    from[2] = pos[2] + SH_SCRIPT_GROUND_ABOVE;
    to[2] = pos[2] - SH_SCRIPT_GROUND_DEPTH;
    if (world_trace(ai, from, to, &t) != 1 || t.normal[2] < SH_SCRIPT_GROUND_FLAT) return 0;
    memcpy(out, t.end, 3 * sizeof(float));
    return 1;
}

static int read_name(const char *src, char *out, size_t cap)
{
    size_t i = 0;
    out[0] = 0;
    if (!src) return 0;
    for (; i + 1 < cap && src[i]; i++) out[i] = src[i];
    out[i] = 0;
    return 1;
}

/* The node of the demon's animWeb named "subweb/state" (or "state"). */
static const uint8_t *web_node(const uint8_t *web, const char *name)
{
    const uint8_t *const *nodes = *(const uint8_t *const *const *)(web + AI_NODE_LIST);
    int n = *(const int *)(web + AI_NODE_COUNT), i;
    const uint8_t *subwebs = *(const uint8_t *const *)(web + AI_WEB_SUBWEBS);
    const char *const *states = *(const char *const *const *)(web + AI_WEB_STATES);
    int nsub = *(const int *)(web + AI_WEB_SUBWEB_COUNT), nstate = *(const int *)(web + AI_WEB_STATE_COUNT);
    const char *slash = strrchr(name, '/');
    if (!nodes || n <= 0 || n > 100000) return NULL;
    for (i = 0; i < n; i++) {
        const uint8_t *node = nodes[i];
        char sub[96], state[96];
        short si, ti;
        if (!node) continue;
        si = *(const short *)(node + AI_NODE_SUBWEB);
        ti = *(const short *)(node + AI_NODE_STATE);
        if (ti < 0 || ti >= nstate || !read_name(states[ti], state, sizeof state)) continue;
        if (_stricmp(state, slash ? slash + 1 : name)) continue;
        if (!slash) return node;
        sub[0] = 0;
        if (si >= 0 && si < nsub)
            read_name(*(const char *const *)(subwebs + (size_t)si * 0x38u + 0x18u), sub, sizeof sub);
        if (strlen(sub) == (size_t)(slash - name) && !_strnicmp(sub, name, (size_t)(slash - name))) return node;
    }
    return NULL;
}

static int e_anim_delta(void *ai, const char *name, int index, float out[3], int *count)
{
    int st = -1;
    if (count) *count = 0;
    if (!g_anim_of_prop || !g_anim_delta) return -1;
    __try {
        const uint8_t *web = *(const uint8_t *const *)((const uint8_t *)ai + SH_AI_OFF_ANIMWEB);
        const uint8_t *node = web ? web_node(web, name) : NULL, *row = NULL;
        void *model = NULL;
        int k, nrows;
        if (!node) return -1;
        st = 0;
        if (*(const int *)(web + AI_WEB_MODEL_COUNT) > 0)
            model = *(void *const *)(*(const uint8_t *const *)(web + AI_WEB_MODELS) + 8);
        nrows = *(const int *)(node + AI_NODE_MODEL_COUNT);
        for (k = 0; k < nrows && k < 64; k++) {
            const uint8_t *r = *(const uint8_t *const *)(node + AI_NODE_MODELS) + (size_t)k * AI_MODEL_ROW;
            if (*(const short *)r == 0) { row = r; break; }
        }
        if (row) {
            int nprops = *(const int *)(row + AI_MODEL_PROP_COUNT);
            if (count) *count = nprops;
            if (index >= 0 && index < nprops) {
                uint8_t *prop = *(uint8_t *const *)(row + AI_MODEL_PROPS) + (size_t)index * AI_PROP_STRIDE;
                uint8_t *anim = (uint8_t *)g_anim_of_prop(prop, model, 0);
                if (anim && *(void *const *)(anim + AI_ANIM_FRAMES)) {
                    g_anim_delta(anim, out, NULL);
                    st = out[0] == out[0] && out[1] == out[1] && out[2] == out[2];
                }
            }
        }
    } __except (av_only(GetExceptionCode())) { st = -1; }
    return st;
}

static int e_weapon_use(void *ai, const char *name)
{
    int st = 0;
    __try {
        const uint8_t *decl = *(const uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_BEHAVIOR_DECL);
        const uint8_t *list = decl ? *(const uint8_t * const *)(decl + SH_AI_BEHAVIOR_WEAPON_USES) : NULL;
        int n = decl ? *(const int32_t *)(decl + SH_AI_BEHAVIOR_WEAPON_USES + 8) : 0, i;
        if (!list || n <= 0 || n > SH_AI_WEAPON_USE_MAX) return decl ? 0 : -1;
        for (i = 0; i < n; i++) {
            const char *entry = *(const char * const *)(list + (size_t)i * SH_AI_WEAPON_USE_ENTRY);
            if (entry && !strcmp(entry, name)) {
                *(int32_t *)((uint8_t *)ai + SH_AI_OFF_WEAPON_USE_INDEX) = i;
                st = 1;
                break;
            }
        }
    } __except (av_only(GetExceptionCode())) { st = -1; }
    return st;
}

static int e_set_recoil(void *ai, float alpha)
{
    int ok = 0;
    if (!g_recoil_site || memcmp(g_recoil_site + SH_AI_RECOIL_SITE_MUL, k_ai_recoil_mul, sizeof k_ai_recoil_mul)) return 0;
    __try { *(float *)((uint8_t *)ai + SH_AI_OFF_RECOIL) = alpha; ok = 1; }
    __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

/* 1 when the game clock has reached the weapon's next-pull time for its
 * active trigger, 0 while it waits, -1 when the demon holds no weapon. */
static int e_weapon_ready(void *ai, int hand)
{
    int v = -1, now;
    uint8_t *w;
    if (!g_fire_decl || !g_slot_table || !gm_clock(&now)) return -1;
    w = weapon_in(ai, hand);
    __try {
        uint8_t *holder = *(uint8_t **)((uint8_t *)ai + SH_AI_OFF_WEAPON_HOLDER);
        if (holder && w) {
            const uint8_t *decl = (const uint8_t *)g_fire_decl(w, *(const int *)(w + SH_AI_OFF_ACTIVE_FIRE));
            if (decl) {
                int type = *(const int *)(decl + SH_AI_DECL_FIRE_TRIGGER), slot;
                if (type >= 0 && type < 12) {
                    slot = g_slot_table[type];
                    if (slot >= -1 && slot < 8)   /* -1 reads the record at +0x30, as the engine does */
                        v = now >= *(const int *)(holder + (size_t)(slot + AI_HOLDER_SLOT_BASE) * 0x10u);
                }
            }
        }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_aim_point(void *ai, const float pos[3])
{
    int ok = 0, focus;
    if (!g_focus) return 0;
    __try {
        static const int foci[3] = { 0, 2, 3 };   /* as ae_selectAimFocusPoint sets them */
        for (focus = 0; focus < 3; focus++) g_focus(ai, foci[focus], pos, 1, -1, 14);
        ok = 1;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    return ok;
}

/* ---- path points: the computed path, read by the path length the query calls */

typedef struct path_capture {
    volatile DWORD thread;
    float (*out)[3];             /* malloc'd here: every point of the engine's path */
    int n;
} path_capture;
static path_capture g_capture;

static float plen_detour(void *move_fsm, const uint8_t *path)
{
    if (g_capture.thread && g_capture.thread == GetCurrentThreadId() && path) {
        __try {
            int n = *(const int *)(path + SH_AI_PATH_SEGMENT_COUNT), i, k = 0;
            const uint8_t *seg = *(const uint8_t *const *)(path + SH_AI_PATH_SEGMENTS);
            free(g_capture.out);
            g_capture.out = NULL;
            if (seg && n > 0 && (g_capture.out = (float (*)[3])malloc((size_t)(n + 1) * sizeof *g_capture.out)) != NULL) {
                memcpy(g_capture.out[k++], seg + 4, 12);
                for (i = 0; i < n; i++) memcpy(g_capture.out[k++], seg + (size_t)i * 0x1C + 0x10, 12);
            }
            g_capture.n = k;
        } __except (av_only(GetExceptionCode())) { g_capture.n = 0; }
    }
    return g_plen_orig(move_fsm, path);
}

/* k_threat_prologue and k_plen_prologue: script_api_ai.h, where tests/script_ai_sigs_test.c compares them with the images. */

static int commit_hook(void *target, void *detour, const uint8_t *want, size_t stolen, void **orig, int *failed,
                       const char *what)
{
    void *tramp;
    if (*orig && hook_is_installed(*orig)) return 1;
    if (*failed || !target) return 0;
    if (memcmp(target, want, stolen)) {
        *failed = 1;
        ai_log("Mod scripts: %s is unavailable in this session: the game function it needs is not the expected one "
               "(another tool may have changed it).", what);
        return 0;
    }
    tramp = hook_prepare(target, detour, stolen);
    if (!tramp) {
        *failed = 1;
        ai_log("Mod scripts: %s is unavailable in this session; the hook could not be prepared (%s).", what,
               hook_last_failure());
        return 0;
    }
    *orig = tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) *orig = NULL;
        *failed = 1;
        ai_log("Mod scripts: %s is unavailable in this session; the hook could not be installed.", what);
        return 0;
    }
    ai_log("SCRIPTS: %s hook installed at %p", what, target);
    return 1;
}

static int e_path_points(void *ai, const float goal[3], int key, float (**out)[3], int *n, float *length)
{
    int st;
    *n = 0;
    *out = NULL;
    if (!commit_hook((void *)g_plen_target, (void *)plen_detour, k_plen_prologue, SH_AI_PATH_LENGTH_STOLEN, (void **)&g_plen_orig,
                     &g_plen_failed, "path_points"))
        return -1;
    g_capture.out = NULL;
    g_capture.n = 0;
    g_capture.thread = GetCurrentThreadId();
    st = e_path(ai, goal, key, length);
    g_capture.thread = 0;
    if (st == 0) { *out = g_capture.out; *n = g_capture.n; }
    else free(g_capture.out);
    g_capture.out = NULL;
    return st;
}

/* ---- the threat table: a blocked reaction's rows are refused */

static uint64_t threat_row_detour(void *threat, void *actor, const uint8_t *row, int64_t state, uint64_t a5,
                                  uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11)
{
    if (actor && row) {
        int reaction = -1, now = 0;
        __try { reaction = *(const int *)(row + SH_AI_THREAT_ROW_REACTION); } __except (av_only(GetExceptionCode())) { reaction = -1; }
        if (reaction > 0 && gm_clock(&now) && sh_ai_blocked(actor, spawn_id_of(actor), reaction, now)) return 0;
    }
    return g_threat_orig(threat, actor, row, state, a5, a6, a7, a8, a9, a10, a11);
}

static int e_block_pain(void *ai, int reaction, int ms)
{
    int now;
    if (!commit_hook((void *)g_threat_target, (void *)threat_row_detour, k_threat_prologue, SH_AI_THREAT_STOLEN,
                     (void **)&g_threat_orig,
                     &g_threat_failed, "block_pain"))
        return 0;
    if (!gm_clock(&now)) return 0;
    return sh_ai_block_set(ai, spawn_id_of(ai), reaction, now + ms);
}

/* ---- the pain string: interned by the engine, put in the request by the stub */

/* The engine's interned atom of each pain string asked for (the engine keeps
 * them for the session); the list grows. */
static struct ai_interned { char *text; const char *atom; } *g_interned;
static int g_ninterned, g_cinterned;

/* Called by the stub on the pain request's thread: the demon's string or NULL. */
static const char *pain_fetch(void *ai)
{
    return sh_ai_pain_string_get(ai, spawn_id_of(ai));
}

static int commit_pain_stub(void)
{
    DWORD old;
    if (g_pain_tramp && hook_is_installed(g_pain_tramp)) return 1;
    if (g_pain_failed || !g_pain_site || !g_intern) return 0;
    if (memcmp(g_pain_site, k_ai_pain_stolen, SH_AI_PAIN_STUB_STOLEN)) {
        g_pain_failed = 1;
        ai_log("Mod scripts: pain_string is unavailable in this session: the game's pain request is not the expected "
               "one (another tool may have changed it).");
        return 0;
    }
    g_pain_stub = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pain_stub) { g_pain_failed = 1; return 0; }
    g_pain_tramp = hook_prepare(g_pain_site, g_pain_stub, SH_AI_PAIN_STUB_STOLEN);
    if (!g_pain_tramp ||
        !sh_ai_pain_stub_build(g_pain_stub, 4096, (const void *)pain_fetch, g_pain_tramp) ||
        !VirtualProtect(g_pain_stub, 4096, PAGE_EXECUTE_READ, &old)) {
        g_pain_failed = 1;
        ai_log("Mod scripts: pain_string is unavailable in this session; the hook could not be prepared (%s).",
               hook_last_failure());
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), g_pain_stub, 4096);
    if (hook_commit(g_pain_tramp) != B2_PATCH_OK) {
        if (hook_unpatch(g_pain_tramp)) g_pain_tramp = NULL;
        g_pain_failed = 1;
        ai_log("Mod scripts: pain_string is unavailable in this session; the hook could not be installed.");
        return 0;
    }
    ai_log("SCRIPTS: pain_string hook installed at %p", (void *)g_pain_site);
    return 1;
}

/* The engine's atom of `text` (its interned string: the engine compares names
 * by this pointer), or NULL. */
static const char *atom_of(const char *text)
{
    const char *atom = NULL;
    int i;
    if (!g_intern || !text) return NULL;
    for (i = 0; i < g_ninterned; i++)
        if (!strcmp(g_interned[i].text, text)) { atom = g_interned[i].atom; break; }
    if (!atom) {
        char *copy = _strdup(text);
        __try { g_intern(&atom, text); } __except (av_only(GetExceptionCode())) { atom = NULL; }
        if (!atom) { free(copy); return NULL; }
        if (copy && g_ninterned == g_cinterned) {
            int cap = g_cinterned ? g_cinterned * 2 : 16;
            void *grown = realloc(g_interned, (size_t)cap * sizeof *g_interned);
            if (grown) { g_interned = grown; g_cinterned = cap; }
        }
        if (copy && g_ninterned < g_cinterned) {
            g_interned[g_ninterned].text = copy;
            g_interned[g_ninterned].atom = atom;
            g_ninterned++;
        } else {
            free(copy);   /* not kept: the next call interns it again, as the engine allows */
        }
    }
    return atom;
}

static int e_pain_string(void *ai, const char *text)
{
    const char *atom;
    if (!text) return sh_ai_pain_string_set(ai, spawn_id_of(ai), NULL);
    if (!commit_pain_stub()) return 0;
    atom = atom_of(text);
    if (!atom) return 0;
    return sh_ai_pain_string_set(ai, spawn_id_of(ai), atom);
}

/* ---- the laser targeter's lasers */

void *sh_script_combat_decl(const char *type, const char *name);   /* script_api_combat.c: a loaded decl by type and name */

typedef void (*ai_laser_fn)(void *component, const char **name);
static ai_laser_fn g_laser_add, g_laser_remove;

static int laser_call(ai_laser_fn fn, uint8_t *comp, const char *atom, void **slot, void *ribbon)
{
    void *own = slot ? *slot : NULL;
    int v = -1;
    if (ribbon && slot) *slot = ribbon;       /* the add reads the ribbon from the decl */
    __try { fn(comp, &atom); v = 1; } __except (av_only(GetExceptionCode())) { v = -1; }
    if (ribbon && slot) *slot = own;
    return v;
}

static int e_laser_add(void *ai, const char *tag, const char *ribbon)
{
    const char *atom;
    void *decl = NULL;
    uint8_t *comp = NULL, *cdecl_ = NULL;
    if (!g_laser_add) return -1;
    if (ribbon && !(decl = sh_script_combat_decl("ribbon", ribbon))) return -2;
    if (!(atom = atom_of(tag))) return -1;
    __try {
        comp = *(uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_LASER_COMPONENT);
        if (comp) cdecl_ = *(uint8_t * const *)(comp + SH_AI_LASER_COMP_DECL);
    } __except (av_only(GetExceptionCode())) { return -1; }
    if (!comp) return 0;
    if (!cdecl_) return -1;
    /* an entry of this name keeps its ribbon through the add, so it is made anew */
    if (g_laser_remove) laser_call(g_laser_remove, comp, atom, NULL, NULL);
    return laser_call(g_laser_add, comp, atom, (void **)(cdecl_ + SH_AI_LASER_DECL_RIBBON), decl);
}

static int e_laser_remove(void *ai, const char *tag)
{
    const char *atom;
    uint8_t *comp = NULL;
    if (!g_laser_remove || !(atom = atom_of(tag))) return -1;
    __try { comp = *(uint8_t * const *)((const uint8_t *)ai + SH_AI_OFF_LASER_COMPONENT); }
    __except (av_only(GetExceptionCode())) { return -1; }
    if (!comp) return 0;
    return laser_call(g_laser_remove, comp, atom, NULL, NULL);
}

/* ---- rear footsteps: the table of a rear foot */

typedef void *(*ai_foot_table_fn)(void *actor, int foot);
static ai_foot_table_fn g_foot_target, g_foot_orig;
static int g_foot_failed;

/* One record per demon that named rear tables (pairs of decl pointers, which outlive the map). The detour
 * reads it on the game's threads without a lock, so it never moves and is sized like the parked list. */
#define AI_FOOT_PAIRS 8      /* the gait tables an actor has: walk, slow walk, crouch, sprint and their friendly forms */
typedef struct {
    void *volatile ai;
    volatile LONG spawn_id;
    void *volatile front[AI_FOOT_PAIRS];
    void *volatile rear[AI_FOOT_PAIRS];
} ai_foot_rec;
static ai_foot_rec g_foot[AI_PARKED_MAX];
static volatile LONG g_foot_used;

static void *foot_table_detour(void *actor, int foot)
{
    void *table = g_foot_orig(actor, foot);
    if (table && (foot == 2 || foot == 3)) {
        int i, k, used = (int)g_foot_used;
        for (i = 0; i < used; i++) {
            if (g_foot[i].ai != actor) continue;
            if (spawn_id_of(actor) != (uint32_t)g_foot[i].spawn_id) break;
            for (k = 0; k < AI_FOOT_PAIRS; k++)
                if (g_foot[i].front[k] == table && g_foot[i].rear[k]) return g_foot[i].rear[k];
            break;
        }
    }
    return table;
}

/* A blend may skip a front foot's event (ae_leftFoot and ae_rightFoot carry EVENTFLAG_CANSKIP) but not a rear
 * foot's, so a blended walk would play the rear table several times a step. The rear events take the front
 * feet's skip bit, once. */
#define AI_EVENTFLAG_CANSKIP 1
static void rear_feet_skip_as_front(void)
{
    static volatile LONG done;
    const sh_script_engine *e = sh_script_api_engine();
    int front;
    if (done || !e || !e->event_flags) return;
    front = e->event_flags("ae_leftFoot", 0);
    if (front < 0) return;
    if (front & AI_EVENTFLAG_CANSKIP) {
        e->event_flags("ae_leftRearFoot", AI_EVENTFLAG_CANSKIP);
        e->event_flags("ae_rightRearFoot", AI_EVENTFLAG_CANSKIP);
    }
    InterlockedExchange(&done, 1);
}

static int e_rear_footsteps(void *ai, const char *front, const char *rear)
{
    void *f = sh_script_combat_decl("projectileImpactEffect", front);
    void *r = sh_script_combat_decl("projectileImpactEffect", rear);
    uint32_t id = spawn_id_of(ai);
    int i, k, slot = -1, used = (int)g_foot_used;
    if (!f || !r) return -2;
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return -1;
    rear_feet_skip_as_front();
    if (!commit_hook((void *)g_foot_target, (void *)foot_table_detour, k_ai_foot_prologue, SH_AI_FOOT_STOLEN,
                     (void **)&g_foot_orig, &g_foot_failed, "rear_footsteps"))
        return -1;
    for (i = 0; i < used; i++) {
        void *p = g_foot[i].ai;
        if (p == ai && (uint32_t)g_foot[i].spawn_id == id) { slot = i; break; }
        if (slot < 0 && (!p || spawn_id_of(p) != (uint32_t)g_foot[i].spawn_id)) slot = i;
    }
    if (i == used && slot < 0 && used < AI_PARKED_MAX) slot = used;
    if (slot < 0) return -1;
    if (g_foot[slot].ai != ai || (uint32_t)g_foot[slot].spawn_id != id) {
        g_foot[slot].ai = NULL;
        for (k = 0; k < AI_FOOT_PAIRS; k++) { g_foot[slot].front[k] = NULL; g_foot[slot].rear[k] = NULL; }
        InterlockedExchange(&g_foot[slot].spawn_id, (LONG)id);
    }
    for (k = 0; k < AI_FOOT_PAIRS; k++) if (g_foot[slot].front[k] == f || !g_foot[slot].front[k]) break;
    if (k == AI_FOOT_PAIRS) return -1;
    InterlockedExchangePointer((PVOID volatile *)&g_foot[slot].rear[k], r);
    InterlockedExchangePointer((PVOID volatile *)&g_foot[slot].front[k], f);
    InterlockedExchangePointer((PVOID volatile *)&g_foot[slot].ai, ai);
    if (slot >= used) InterlockedExchange(&g_foot_used, slot + 1);
    return 1;
}

/* ---- wound hides: the meshes that go when a gore wound is shown */

typedef void (*ai_show_wound_fn)(void *gore, void **row);
typedef int (*ai_mesh_index_fn)(void *model, const char *name);
typedef void (*ai_mesh_hide_fn)(void *model, int index);
static ai_show_wound_fn g_wound_target, g_wound_orig;
static ai_mesh_index_fn g_mesh_index;
static ai_mesh_hide_fn g_mesh_hide;
static int g_wound_failed;

/* A game thread retains a complete row while displaying the wound. */
typedef struct ai_wound_row {
    struct ai_wound_row *next;
    void *ai, *wound;
    uint32_t spawn_id;
    LONG refs;
    int n;
    int index[];
} ai_wound_row;

static SRWLOCK g_wound_lock = SRWLOCK_INIT;
static ai_wound_row *g_wounds;

static void wound_meshes_release(ai_wound_row *row)
{
    if (row && InterlockedDecrement(&row->refs) == 0) free(row);
}

static void wound_meshes_remove(uint32_t spawn_id)
{
    ai_wound_row **link, *row;
    AcquireSRWLockExclusive(&g_wound_lock);
    link = &g_wounds;
    while ((row = *link) != NULL) {
        if (spawn_id == SH_SCRIPT_SPAWN_ID_NONE || row->spawn_id == spawn_id) {
            *link = row->next;
            wound_meshes_release(row);
        } else {
            link = &row->next;
        }
    }
    ReleaseSRWLockExclusive(&g_wound_lock);
}

static ai_wound_row *wound_meshes_find(void *owner, uint32_t id, void *wound)
{
    ai_wound_row *row;
    AcquireSRWLockShared(&g_wound_lock);
    for (row = g_wounds; row; row = row->next) {
        if (row->ai == owner && row->spawn_id == id && row->wound == wound) {
            InterlockedIncrement(&row->refs);
            break;
        }
    }
    ReleaseSRWLockShared(&g_wound_lock);
    return row;
}

/* The owner of the gore component whose ShowWound runs on this thread: the
 * breakables the wound spawns are thrown in its frame (destructible). */
static __declspec(thread) void *t_wound_owner;

static void show_wound_detour(void *gore, void **row)
{
    void *wound = NULL, *owner = NULL, *was = t_wound_owner;
    ai_wound_row *meshes = NULL;
    __try { t_wound_owner = gore ? *(void * const *)((const uint8_t *)gore + SH_AI_GORE_OWNER) : NULL; }
    __except (av_only(GetExceptionCode())) { t_wound_owner = NULL; }
    g_wound_orig(gore, row);
    t_wound_owner = was;
    __try {
        if (gore && row && *row) {
            /* the row's first word is its gore node (woundData_t.woundNode), not the wound: the wound is the
             * node's areaWoundDecl (the engine's lookup VK 0x1406E8AB0 returns it unless a weaponSpecific row
             * names the damage decl; a ported demon's nodes have none) */
            wound = *(void * const *)((const uint8_t *)*row + SH_AI_GORE_NODE_WOUND);
            owner = *(void * const *)((const uint8_t *)gore + SH_AI_GORE_OWNER);
        }
        if (wound && owner) {
            int i;
            void *model = *(void * const *)((const uint8_t *)owner + SH_AI_OFF_MODEL);
            if (model) meshes = wound_meshes_find(owner, spawn_id_of(owner), wound);
            if (meshes) for (i = 0; i < meshes->n; i++) g_mesh_hide(model, meshes->index[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    wound_meshes_release(meshes);
}

static int e_wound_hides(void *ai, const char *wound, const char *const *meshes, int n)
{
    void *decl = sh_script_combat_decl("gorewounds", wound);
    uint32_t id = spawn_id_of(ai);
    ai_wound_row *row, *old, **link;
    void *model = NULL;
    int i;
    if (!decl) return -2;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !g_mesh_index || !g_mesh_hide || n < 0 ||
        (size_t)n > (SIZE_MAX - sizeof *row) / sizeof row->index[0]) return -1;
    __try { model = *(void * const *)((const uint8_t *)ai + SH_AI_OFF_MODEL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { model = NULL; }
    if (!model) return -1;
    row = (ai_wound_row *)malloc(sizeof *row + (size_t)n * sizeof row->index[0]);
    if (!row) return -1;
    row->ai = ai;
    row->spawn_id = id;
    row->wound = decl;
    row->refs = 1;
    row->n = n;
    for (i = 0; i < n; i++) {
        __try { row->index[i] = g_mesh_index(model, meshes[i]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { row->index[i] = -1; }
        if (row->index[i] < 0) { free(row); return -3; }
    }
    if (!commit_hook((void *)g_wound_target, (void *)show_wound_detour, k_ai_wound_prologue, SH_AI_WOUND_STOLEN,
                     (void **)&g_wound_orig, &g_wound_failed, "wound_hides")) {
        free(row);
        return -1;
    }
    AcquireSRWLockExclusive(&g_wound_lock);
    for (link = &g_wounds; (old = *link) != NULL; link = &old->next)
        if (old->ai == ai && old->spawn_id == id && old->wound == decl) break;
    row->next = old ? old->next : NULL;
    *link = row;
    ReleaseSRWLockExclusive(&g_wound_lock);
    wound_meshes_release(old);
    return 1;
}

/* Both renderers call the visual handler here after ShowWound has accepted the actual wound declaration. */
typedef uintptr_t (*ai_wound_visuals_fn)(void *, void *, unsigned char);
static ai_wound_visuals_fn g_visuals_orig;
static const void *g_wound_visual_return;
static int g_visuals_failed;

static uintptr_t wound_visuals_detour(void *gore, void *wound, unsigned char show)
{
    void *owner;
    int effect;
    const void *site = _ReturnAddress();
    uintptr_t result = g_visuals_orig(gore, wound, show);
    if (!gore || !wound || !show || site != g_wound_visual_return) return result;
    __try {
        owner = *(void * const *)((const uint8_t *)gore + SH_AI_GORE_OWNER);
        if (owner) {
            effect = sh_ai_wound_effect_at(owner, spawn_id_of(owner), wound, show, site, g_wound_visual_return);
            if (effect >= 0) sh_script_combat_start_owner_fx(owner, effect);
        }
    } __except (av_only(GetExceptionCode())) { }
    return result;
}

static int install_wound_effect(void)
{
    static const unsigned char before[] = {0x41,0xb0,0x01,0x48,0x8b,0xcb,0x48,0x8b,0xd5,0xe8};
    static const unsigned char after[] = {0x48,0x8b,0x94,0x24,0xb0,0,0,0};
    static const unsigned char prologue[] = {0x40,0x53,0x55,0x56,0x48,0x81,0xec,0xb0,0,0,0,0x48,0x8b,0x05};
    const uint8_t *site, *target;
    void *trampoline;
    sh_patch_status status;
    if (g_visuals_orig && hook_is_installed((void *)g_visuals_orig)) return 1;
    if (!g_wound_target || g_visuals_failed) return 0;
    site = (const uint8_t *)g_wound_target + 0x32e;
    if (memcmp(site - 9, before, sizeof before) || memcmp(site + 5, after, sizeof after)) return 0;
    target = site + 5 + rel32_at(site + 1);
    if (memcmp(target, prologue, sizeof prologue)) return 0;
    trampoline = hook_prepare_rip_load((void *)target, wound_visuals_detour, 18, 11);
    if (!trampoline) {
        ai_log("Mod scripts: wound_effect registration failed: %s.", hook_last_failure());
        return 0;
    }
    g_wound_visual_return = site + 5;
    g_visuals_orig = (ai_wound_visuals_fn)trampoline;
    status = hook_commit(trampoline);
    if (status == B2_PATCH_OK) return 1;
    if (hook_unpatch(trampoline)) g_visuals_orig = NULL;
    else g_visuals_failed = 1;
    ai_log("Mod scripts: wound_effect registration failed: %s.", sh_patch_status_str(status));
    return 0;
}

static int e_wound_effect(void *ent, const char *name, const char *condition)
{
    void *wound = sh_script_combat_decl("gorewounds", name);
    int effect = sh_script_combat_fx_condition(condition);
    uint32_t id = spawn_id_of(ent);
    if (!wound) return -2;
    if (effect < 0) return -3;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !install_wound_effect()) return -1;
    return sh_ai_set_wound_effect(ent, id, wound, effect) ? 1 : -1;
}

/* ---- posed legs: the rig's targets and poles from the clips' own legs */

typedef void (*ai_rig_entry_fn)(void *runtime, void *ctx, char align);
typedef uint64_t (*ai_rig_solve_fn)(const float *hip, const float *parent_q, const void *leg, const float *target,
                                    float *thigh_q, float *shin_q);
static ai_rig_entry_fn g_rig_target, g_rig_orig;
static ai_rig_solve_fn g_rig_solve;
static const void *g_rig_eval;
static uint32_t g_rig_model_off, g_rig_anim_off;
static int g_rig_failed;

/* One record per demon that asked, found by its rig runtime on the pose
 * jobs' threads without a lock, so it never moves; the runtime is set last
 * and cleared first. A frame step drops the record of a demon that is gone. */
typedef struct {
    void *volatile ai;
    volatile LONG spawn_id;
    void *volatile runtime;
} ai_legs_rec;
static ai_legs_rec g_legs[AI_PARKED_MAX];
static volatile LONG g_legs_used;

static void *rig_runtime_of(void *ai)
{
    void *rt = NULL;
    __try {
        const uint8_t *model = *(const uint8_t * const *)((const uint8_t *)ai + g_rig_model_off);
        const uint8_t *anim = model ? *(const uint8_t * const *)(model + g_rig_anim_off) : NULL;
        if (anim) rt = *(void * const *)(anim + SH_AI_RIG_ANIM_RUNTIME);
    } __except (EXCEPTION_EXECUTE_HANDLER) { rt = NULL; }
    return rt;
}

static int rig_solve_plain(void *leg, const float hip[3], const float parent_q[4], const float target[3],
                           const float target_q[4], float thigh_q[4])
{
    __declspec(align(16)) float at[4], pq[4], want[12], thigh[4], shin[4];
    memcpy(at, hip, 3 * sizeof(float)); at[3] = 0;
    memcpy(pq, parent_q, sizeof pq);
    memcpy(want, target, 3 * sizeof(float));
    memcpy(want + 3, target_q, 4 * sizeof(float));
    want[7] = 0; want[8] = 0; want[9] = 0; want[10] = 1; want[11] = 0;   /* no swivel */
    g_rig_solve(at, pq, leg, want, thigh, shin);
    memcpy(thigh_q, thigh, sizeof thigh);
    return 1;
}

/* Every leg of the runtime's two-bone components: posed (pose given) or only
 * counted (pose NULL). Caller guards the reads. */
static int rig_legs_walk(void *runtime, const sh_ai_leg_pose *pose)
{
    const uint8_t *engine = *(const uint8_t * const *)((const uint8_t *)runtime + SH_AI_RIG_RUNTIME_ENGINE);
    const uint8_t *blocks;
    int nblocks, b, legs = 0;
    if (!engine) return 0;
    blocks = *(const uint8_t * const *)(engine + SH_AI_RIG_ENGINE_BLOCKS);
    nblocks = *(const int32_t *)(engine + SH_AI_RIG_ENGINE_COUNT);
    if (!blocks || nblocks <= 0 || nblocks > 64) return 0;
    for (b = 0; b < nblocks; b++) {
        const uint8_t *block = blocks + (size_t)b * SH_AI_RIG_BLOCK_SIZE;
        const uint8_t *parts = *(const uint8_t * const *)(block + SH_AI_RIG_BLOCK_PARTS);
        int nparts = *(const int32_t *)(block + SH_AI_RIG_BLOCK_COUNT), k;
        if ((block[SH_AI_RIG_BLOCK_SKIP] & 1) || !parts || nparts <= 0 || nparts > 256) continue;
        for (k = 0; k < nparts; k++) {
            const uint8_t *part = parts + (size_t)k * SH_AI_RIG_PART_SIZE;
            void **object = *(void ** const *)part;
            uint8_t *leg = *(uint8_t * const *)(part + 8);
            int n = *(const int32_t *)(part + 0x10), i;
            if (!object || !leg || n <= 0 || n > 64) continue;
            if ((*(void ** const *)object)[SH_AI_RIG_PART_EVAL_SLOT] != g_rig_eval) continue;
            for (i = 0; i < n; i++, leg += SH_AI_RIG_LEG_SIZE) {
                legs++;
                if (pose) {
                    sh_ai_leg_joints j;
                    j.parent = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_PARENT) & 0xFFFFFFu);
                    j.target = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_TARGET) & 0xFFFFFFu);
                    j.pole = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_POLE) & 0xFFFFFFu);
                    j.hip = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_HIP) & 0xFFFFFFu);
                    j.thigh = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_THIGH) & 0xFFFFFFu);
                    j.foot = (int)(*(const uint32_t *)(leg + SH_AI_RIG_LEG_FOOT) & 0xFFFFFFu);
                    (void)sh_ai_posed_leg(pose, &j, rig_solve_plain, leg);
                }
            }
        }
    }
    return legs;
}

static void rig_entry_detour(void *runtime, void *ctx, char align)
{
    __try {
        int i, used = (int)g_legs_used;
        for (i = 0; i < used; i++) {
            void *ai;
            const uint8_t *pose;
            sh_ai_leg_pose p;
            if (g_legs[i].runtime != runtime) continue;
            ai = g_legs[i].ai;
            if (!ai || !ctx || rig_runtime_of(ai) != runtime) break;
            pose = *(const uint8_t * const *)((const uint8_t *)ctx + SH_AI_RIG_CTX_POSE);
            if (!pose) break;
            p.parents = *(const int16_t * const *)(pose + SH_AI_RIG_POSE_PARENTS);
            p.joints = *(const int32_t *)(pose + SH_AI_RIG_POSE_JOINTS);
            p.lt = *(uint8_t * const *)(pose + SH_AI_RIG_POSE_LT);
            p.lq = *(uint8_t * const *)(pose + SH_AI_RIG_POSE_LQ);
            p.lt_stride = *(const int32_t *)(pose + SH_AI_RIG_POSE_LT_STRIDE);
            p.lq_stride = *(const int32_t *)(pose + SH_AI_RIG_POSE_LQ_STRIDE);
            (void)rig_legs_walk(runtime, &p);
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_rig_orig(runtime, ctx, align);
}

/* Each frame, on the game's thread: a record whose demon is gone stops
 * answering before another animation can be given its rig runtime. */
static void posed_legs_step(void)
{
    int i, used = (int)g_legs_used;
    for (i = 0; i < used; i++) {
        void *ai = g_legs[i].ai;
        if (!ai) continue;
        if (spawn_id_of(ai) != (uint32_t)g_legs[i].spawn_id || rig_runtime_of(ai) != g_legs[i].runtime) {
            InterlockedExchangePointer((PVOID volatile *)&g_legs[i].runtime, NULL);
            InterlockedExchangePointer((PVOID volatile *)&g_legs[i].ai, NULL);
        }
    }
}

static int e_posed_legs(void *ai)
{
    uint32_t id = spawn_id_of(ai);
    void *rt = rig_runtime_of(ai);
    int i, slot = -1, used = (int)g_legs_used, legs = 0;
    if (id == SH_SCRIPT_SPAWN_ID_NONE || !g_rig_solve || !g_rig_eval) return -1;
    if (!rt) return 0;
    __try { legs = rig_legs_walk(rt, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { legs = 0; }
    if (legs <= 0) return 0;
    if (!commit_hook((void *)g_rig_target, (void *)rig_entry_detour, k_ai_rig_prologue, SH_AI_RIG_STOLEN,
                     (void **)&g_rig_orig, &g_rig_failed, "posed_legs"))
        return -1;
    for (i = 0; i < used; i++) {
        void *p = g_legs[i].ai;
        if (p == ai && (uint32_t)g_legs[i].spawn_id == id) { slot = i; break; }
        if (slot < 0 && (!p || spawn_id_of(p) != (uint32_t)g_legs[i].spawn_id)) slot = i;
    }
    if (i == used && slot < 0 && used < AI_PARKED_MAX) slot = used;
    if (slot < 0) return -1;
    InterlockedExchangePointer((PVOID volatile *)&g_legs[slot].runtime, NULL);
    InterlockedExchange(&g_legs[slot].spawn_id, (LONG)id);
    InterlockedExchangePointer((PVOID volatile *)&g_legs[slot].ai, ai);
    InterlockedExchangePointer((PVOID volatile *)&g_legs[slot].runtime, rt);
    if (slot >= used) InterlockedExchange(&g_legs_used, slot + 1);
    sh_script_api_on_frame(posed_legs_step);
    return 1;
}

/* ---- fk reference pose (script_api_ai.h, "The fk reference pose") */

typedef void (*ai_fkref_fn)(void *ai, const void *address, intptr_t effector, void *out);
typedef void *(*ai_web_leaf_fn)(void *web);
static ai_fkref_fn g_fkref_target, g_fkref_orig;
static ai_web_leaf_fn g_leaf_target, g_leaf_orig;
static int g_fkref_failed, g_leaf_failed;
typedef struct {
    void *volatile ai;
    volatile LONG spawn_id;
} ai_fkref_rec;
static ai_fkref_rec g_fkref[AI_PARKED_MAX];
static volatile LONG g_fkref_used;
/* Set by the reference pose of a named demon's effector that names an
 * animation, for its own first call of the web's current leaf (made before
 * any other call of that function); cleared by that call or on return. */
static __declspec(thread) int t_fkref_no_leaf;

static void *web_leaf_detour(void *web)
{
    if (t_fkref_no_leaf) {
        t_fkref_no_leaf = 0;
        return NULL;
    }
    return g_leaf_orig(web);
}

static void fkref_detour(void *ai, const void *address, intptr_t effector, void *out)
{
    int i, used = (int)g_fkref_used, named = 0;
    for (i = 0; ai && i < used; i++) {
        if (g_fkref[i].ai != ai) continue;
        if (spawn_id_of(ai) == (uint32_t)g_fkref[i].spawn_id) {
            __try { named = sh_ai_fk_reference_named(address, (int)effector); }
            __except (EXCEPTION_EXECUTE_HANDLER) { named = 0; }
        }
        break;
    }
    t_fkref_no_leaf = named;
    g_fkref_orig(ai, address, effector, out);
    t_fkref_no_leaf = 0;
}

/* The web's interruptBlend, written after the order's event returned (the
 * script events run their animator call at once: the web's update, later in
 * the frame, reads it). Only a value of the enum is replaced. */
static int e_interrupt_blend(void *ai, int mode)
{
    int v = -1;
    if (!ai || mode < SH_AI_INTR_BLEND_NO || mode > SH_AI_INTR_BLEND_MULTI) return -1;
    __try {
        int32_t *p = (int32_t *)((uint8_t *)ai + SH_AI_OFF_WEB_ANIMATOR + SH_AI_AW_INTERRUPT_BLEND);
        if (*p >= SH_AI_INTR_BLEND_NO && *p <= SH_AI_INTR_BLEND_MULTI) { *p = mode; v = 1; }
    } __except (av_only(GetExceptionCode())) { v = -1; }
    return v;
}

static int e_fk_reference_pose(void *ai)
{
    uint32_t id = spawn_id_of(ai);
    int i, slot = -1, used = (int)g_fkref_used;
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return -1;
    if (!commit_hook((void *)g_leaf_target, (void *)web_leaf_detour, k_ai_leaf_prologue, SH_AI_LEAF_STOLEN,
                     (void **)&g_leaf_orig, &g_leaf_failed, "fk_reference_pose (web leaf)"))
        return -1;
    if (!commit_hook((void *)g_fkref_target, (void *)fkref_detour, k_ai_fkref_prologue, SH_AI_FKREF_STOLEN,
                     (void **)&g_fkref_orig, &g_fkref_failed, "fk_reference_pose"))
        return -1;
    for (i = 0; i < used; i++) {
        void *p = g_fkref[i].ai;
        if (p == ai && (uint32_t)g_fkref[i].spawn_id == id) return 1;
        if (slot < 0 && (!p || spawn_id_of(p) != (uint32_t)g_fkref[i].spawn_id)) slot = i;
    }
    if (slot < 0 && used < AI_PARKED_MAX) slot = used;
    if (slot < 0) return -1;
    InterlockedExchangePointer((PVOID volatile *)&g_fkref[slot].ai, NULL);
    InterlockedExchange(&g_fkref[slot].spawn_id, (LONG)id);
    InterlockedExchangePointer((PVOID volatile *)&g_fkref[slot].ai, ai);
    if (slot >= used) InterlockedExchange(&g_fkref_used, slot + 1);
    return 1;
}

/* ---- destructibles: a gore breakable of a def named with destructible() */

int sh_companions_def_chain(void *entity, const char **names, int cap);   /* companion_engine.c */

typedef char (*ai_brk_gore_fn)(void *mgr, void *brk, const float *tag, const float *axis, const float *pos,
                               const float *dir, const float *falloff, void *gore_behavior);
typedef void (*ai_brk_break_fn)(void *brk, uint64_t zero);
typedef void (*ai_fxphys_place_fn)(void *broadphase, void *entry, const float *pos, const float *axis, void *zero);
typedef const float *(*ai_phys_vec_fn)(void *physics, int id);
static ai_brk_gore_fn g_brkg_target, g_brkg_orig;
static ai_brk_break_fn g_brk_break;
static ai_fxphys_place_fn g_fxphys_place;
static int g_brkg_failed;
static volatile LONG g_dd_seed = 0x2545F491;

static float dd_rand(void *state)
{
    (void)state;   /* an LCG of the game's kind; the draws need not be the game's own */
    return (float)(((uint32_t)InterlockedExchange(&g_dd_seed, (LONG)((uint32_t)g_dd_seed * 1664525u + 1013904223u))
                    >> 8) & 0xFFFFFF) / 16777216.0f;
}

static int dd_phys_vec(void *ent, size_t slot, float *out, int n)
{
    void *phys = *(void **)((uint8_t *)ent + SH_AI_OFF_PHYSICS);
    ai_phys_vec_fn get = (ai_phys_vec_fn)vfn(phys, slot);
    const float *v = get ? get(phys, 0) : NULL;
    if (!v) return 0;
    memcpy(out, v, (size_t)n * sizeof *out);
    return 1;
}

/* Breaks the breakable as the game does, puts every piece back where the model places it (the break moves each
 * up to 20 units an axis and turns it), then gives each Eternal's velocity and spin as momentum (mass x velocity,
 * world inertia x spin). `add`: on top of the game's own hit (a decl that takes the external force). */
static void destruct_activate(void *brk, const sh_ai_destruct_spec *spec, void *owner, const float *hit, int add)
{
    uint8_t *phys = (uint8_t *)brk + SH_AI_BRK_PHYSICS, **bodies;
    float (*lpos)[3] = NULL, (*lax)[9] = NULL, org[3], ax[9], oorg[3], ext[3] = { 0, 0, 1 };
    int i, n, fresh;
    bodies = *(uint8_t ***)(phys + SH_AI_FXB_BODIES);
    n = *(const int *)(phys + SH_AI_FXB_BODIES + 8);
    /* the explosion fx the break starts draws its render parm items on these pieces, as Eternal's on its
     * destructible's render entity */
    (void)sh_ai_destruct_fx_model(phys);
    if (!bodies || n <= 0) { g_brk_break(brk, 0); return; }
    fresh = *(const int32_t *)((uint8_t *)brk + SH_AI_BRK_BROKEN) == -1;
    /* every piece the breakable has, however many (where each sat before the break) */
    if (fresh) {
        lpos = (float (*)[3])malloc(sizeof *lpos * (size_t)n);
        lax = (float (*)[9])malloc(sizeof *lax * (size_t)n);
        if (!lpos || !lax) fresh = 0;   /* without memory the pieces stay where the break put them */
    }
    for (i = 0; i < n && fresh; i++) {
        if (!bodies[i]) continue;
        memcpy(lpos[i], bodies[i] + SH_AI_RB_POS, sizeof lpos[i]);
        memcpy(lax[i], bodies[i] + SH_AI_RB_AXIS, sizeof lax[i]);
    }
    g_brk_break(brk, 0);
    if (!dd_phys_vec(brk, SH_AI_VSLOT_ORIGIN, org, 3) || !dd_phys_vec(brk, SH_AI_VSLOT_AXIS, ax, 9)) {
        free(lpos);
        free(lax);
        return;
    }
    if (!owner || !dd_phys_vec(owner, SH_AI_VSLOT_ORIGIN, oorg, 3)) memcpy(oorg, org, sizeof oorg);
    if (hit && !add) hit = NULL;
    if (hit) { memcpy(ext, hit, sizeof ext); dd_unit(ext); if (dd_len(ext) <= 0.0f) { ext[0] = 0; ext[1] = 0; ext[2] = 1; } }
    for (i = 0; i < n; i++) {
        uint8_t *b = bodies[i], *props;
        float lin[3], ang[3], *p, *o, w[3], lb[3], j[9], inv[9], det, im;
        int r, c;
        if (!b) continue;
        p = (float *)(b + SH_AI_RB_POS);
        o = (float *)(b + SH_AI_RB_AXIS);
        props = *(uint8_t **)(b + SH_AI_RB_PROPS);
        if (fresh) {                       /* where the model places the piece: local x axis + origin, local axis x axis */
            float q[9];
            for (c = 0; c < 3; c++) p[c] = org[c] + lpos[i][0] * ax[c] + lpos[i][1] * ax[3 + c] + lpos[i][2] * ax[6 + c];
            for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
                q[3 * r + c] = lax[i][3 * r] * ax[c] + lax[i][3 * r + 1] * ax[3 + c] + lax[i][3 * r + 2] * ax[6 + c];
            memcpy(o, q, sizeof q);
            if (props && g_fxphys_place)
                g_fxphys_place(*(void **)(props + SH_AI_PROPS_BROADPHASE), b + SH_AI_RB_SPOBJECT, p, o, NULL);
        }
        if (i >= spec->n || !props) continue;
        sh_ai_destruct_velocity(&spec->piece[i], oorg, ax, ext, p, spec->max_speed, spec->max_spin, dd_rand, NULL,
                                lin, ang);
        im = *(const float *)(props + SH_AI_PROPS_INV_MASS);
        memcpy(j, props + SH_AI_PROPS_INV_INERTIA, sizeof j);
        det = j[0] * (j[4] * j[8] - j[5] * j[7]) - j[1] * (j[3] * j[8] - j[5] * j[6]) + j[2] * (j[3] * j[7] - j[4] * j[6]);
        if (im <= 0.0f || det == 0.0f || det != det) continue;
        inv[0] = (j[4] * j[8] - j[5] * j[7]) / det; inv[1] = (j[2] * j[7] - j[1] * j[8]) / det;
        inv[2] = (j[1] * j[5] - j[2] * j[4]) / det; inv[3] = (j[5] * j[6] - j[3] * j[8]) / det;
        inv[4] = (j[0] * j[8] - j[2] * j[6]) / det; inv[5] = (j[2] * j[3] - j[0] * j[5]) / det;
        inv[6] = (j[3] * j[7] - j[4] * j[6]) / det; inv[7] = (j[1] * j[6] - j[0] * j[7]) / det;
        inv[8] = (j[0] * j[4] - j[1] * j[3]) / det;
        for (r = 0; r < 3; r++) w[r] = o[3 * r] * ang[0] + o[3 * r + 1] * ang[1] + o[3 * r + 2] * ang[2];   /* O w */
        for (r = 0; r < 3; r++) lb[r] = inv[3 * r] * w[0] + inv[3 * r + 1] * w[1] + inv[3 * r + 2] * w[2];  /* I (O w) */
        {
            float *lm = (float *)(b + SH_AI_RB_LINEAR), *am = (float *)(b + SH_AI_RB_ANGULAR);
            for (c = 0; c < 3; c++) {
                float lw = lb[0] * o[c] + lb[1] * o[3 + c] + lb[2] * o[6 + c];                               /* O^T */
                lm[c] = (add ? lm[c] : 0.0f) + lin[c] / im;
                am[c] = (add ? am[c] : 0.0f) + lw;
            }
        }
        b[SH_AI_RB_SETTLED] = 0;
        b[SH_AI_RB_ACTIVE] = 1;
    }
    free(lpos);
    free(lax);
}

static char brk_gore_detour(void *mgr, void *brk, const float *tag, const float *axis, const float *pos,
                            const float *dir, const float *falloff, void *gore_behavior)
{
    sh_ai_destruct_spec spec;
    const char *names[10];
    int n = 0, have = 0;
    char r = 0;
    __try {
        n = sh_companions_def_chain(brk, names, 10);
        have = n > 0 && sh_ai_destructible_find(names, n, &spec);
    } __except (av_only(GetExceptionCode())) { have = 0; }
    if (!have) return g_brkg_orig(mgr, brk, tag, axis, pos, dir, falloff, gore_behavior);
    /* a decl that ignores the external force takes nothing of the hit: neither the damage's weapon-specific gore
     * nor the hit's explosion record the spawn queues when this answers true */
    if (!spec.ignore_external) r = g_brkg_orig(mgr, brk, tag, axis, pos, dir, falloff, gore_behavior);
    __try { destruct_activate(brk, &spec, t_wound_owner, dir, !spec.ignore_external); }
    __except (av_only(GetExceptionCode())) { }
    return r;
}

static int e_destructible(void)
{
    if (!g_brk_break || !g_fxphys_place) return -1;
    /* the owner the pieces are thrown in the frame of comes from ShowWound; without it their own origin serves */
    if (g_wound_target)
        (void)commit_hook((void *)g_wound_target, (void *)show_wound_detour, k_ai_wound_prologue, SH_AI_WOUND_STOLEN,
                          (void **)&g_wound_orig, &g_wound_failed, "destructible");
    return commit_hook((void *)g_brkg_target, (void *)brk_gore_detour, k_ai_brk_gore_prologue, SH_AI_BRK_GORE_STOLEN,
                       (void **)&g_brkg_orig, &g_brkg_failed, "destructible") ? 1 : -1;
}

/* ---- wound damage counts (script_api_ai.h, "Wound damage counts") */

typedef uint8_t (*ai_daw_fn)(void *gore, void *wound, void *record, void *group_decl, int group, int lost,
                             float max_health);
static ai_daw_fn g_daw_target, g_daw_orig;
static int g_daw_failed;
static volatile LONG g_daw_any;   /* a demon asked: until then the detour only calls through */

static uint8_t daw_detour(void *gore, void *wound, void *record, void *group_decl, int group, int lost,
                          float max_health)
{
    void *owner = NULL;
    uint32_t id, mark = 0;
    float total = 0.0f, relative = 0.0f, damage = 0.0f;
    int st, ok = 0;
    uint8_t pass;
    if (!g_daw_any || !gore || !wound || !record)
        return g_daw_orig(gore, wound, record, group_decl, group, lost, max_health);
    __try { owner = *(void * const *)((const uint8_t *)gore + SH_AI_GORE_OWNER); }
    __except (av_only(GetExceptionCode())) { owner = NULL; }
    id = owner ? spawn_id_of(owner) : SH_SCRIPT_SPAWN_ID_NONE;
    if (!owner || !sh_wound_count_on(owner, id))
        return g_daw_orig(gore, wound, record, group_decl, group, lost, max_health);
    __try {
        total = *(const float *)((const uint8_t *)wound + SH_AI_WOUND_TOTAL_REQUIRED);
        relative = *(const float *)((const uint8_t *)wound + SH_AI_WOUND_RELATIVE_REQUIRED);
        damage = *(const float *)((const uint8_t *)record + SH_AI_GFD_DAMAGE);
        mark = sh_wound_count_mark(record);
        ok = 1;
    } __except (av_only(GetExceptionCode())) { ok = 0; }
    if (!ok) return g_daw_orig(gore, wound, record, group_decl, group, lost, max_health);
    /* every gate but the group's loss, as the game runs them (the loss is read once, only for that gate) */
    pass = g_daw_orig(gore, wound, record, group_decl, group, SH_AI_DAW_LOST_RAISED, max_health);
    st = sh_wound_count_hit(owner, id, wound, record, mark, sh_wound_count_threshold(total, relative, max_health),
                            damage, pass != 0);
    if (st < 0) return g_daw_orig(gore, wound, record, group_decl, group, lost, max_health);
    return (uint8_t)st;
}

static int e_wound_damage_counts(void *ai, int on)
{
    uint32_t id = spawn_id_of(ai);
    if (id == SH_SCRIPT_SPAWN_ID_NONE) return -1;
    if (!on) {
        (void)sh_wound_count_set(ai, id, 0);
        return 1;
    }
    if (!commit_hook((void *)g_daw_target, (void *)daw_detour, k_ai_daw_prologue, SH_AI_DAW_STOLEN,
                     (void **)&g_daw_orig, &g_daw_failed, "wound_damage_counts"))
        return -1;
    if (!sh_wound_count_set(ai, id, 1)) return 0;
    if (!InterlockedExchange(&g_daw_any, 1))
        ai_log("Mod scripts: a demon's gore wounds now open on the damage inside each wound's own spheres "
               "(wound_damage_counts).");
    return 1;
}

/* ---- the compositions */

static const sh_script_ai_engine k_engine;

/* Path-cache keys of the compositions: 64 per demon (animation_fits +0, firing_position +32). */
static int ai_key(void *ai)
{
    return 0x40000000 | (int)((((uintptr_t)ai >> 4) & 0x3FF) << 15);   /* 2 * SH_AI_KEY_SPAN keys a demon */
}

static int e_anim_valid(void *ai, const char *node, unsigned tests, int side, unsigned seed, int *index)
{
    return sh_ai_anim_valid_run(&k_engine, ai, node, tests, side, seed, ai_key(ai), index);
}

static int e_firing_position(void *ai, const sh_ai_position_query *q, float out[3])
{
    return sh_ai_firing_run(&k_engine, ai, q, ai_key(ai) + SH_AI_KEY_SPAN, out);
}

static int e_spline_clear(void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap, int *n)
{
    return sh_ai_spline_run(&k_engine, ai, q, out, cap, n);
}

static const sh_script_ai_engine k_engine = {
    e_entity, e_handle_of, e_is_ai, e_origin, e_yaw, e_event, e_park, e_enemy, e_awareness,
    e_move_status, e_trigger_state, e_path, e_frame,
    e_eye, e_sight, e_ground, e_anim_delta,
    e_set_recoil, e_weapon_ready, e_aim_point, e_anim_valid, e_path_points, e_firing_position, e_spline_clear,
    e_pain_string, e_block_pain, e_weapon_hand, e_item,
    e_fire_modes, e_focus_state, e_muzzle_test, e_clear_move, e_idle_turn_entry,
    e_laser_add, e_laser_remove, e_rear_footsteps,
    e_step_state, e_step_interrupt, e_gore_init, e_wound_hides, e_held, e_posed_legs, e_fk_reference_pose,
    e_interrupt_blend, e_destructible, e_weapon_use, e_wound_damage_counts, e_snapmap_order,
    e_release, e_anim_variable, e_relations, e_off_limits, e_world_clear, e_world_ground, e_request_pain, e_wound_effect,
    e_native_body_busy, e_reset_motion_correction, e_on_ground
};
/* The table the calls use: k_engine less the calls whose game parts this executable lacks (they then refuse). */
static sh_script_ai_engine g_live;


int sh_script_ai_install(const uint8_t *module_base)
{
    sig_result r[SH_AI_SIG_COUNT];
    const uint8_t *site;
    int i;
    sh_script_ai_register();
    if (!module_base) return 0;
    g_base = (uint8_t *)module_base;
    for (i = 0; i <= SH_AI_SIG_PATH_SITE; i++) {
        if (sig_resolve_one(module_base, &SH_AI_SIGNATURES[i], &r[i]) != SIG_OK) {
            ai_log("Mod scripts cannot drive demons in this session: the game function they need (%s) was not "
                   "found in this executable.", SH_AI_SIGNATURES[i].name);
            return 0;
        }
    }
    g_irs_target = (ai_irs_fn)r[SH_AI_SIG_IS_RUNNING_SCRIPT].addr;
    g_get_awareness = (ai_awareness_fn)r[SH_AI_SIG_GET_AWARENESS].addr;
    g_ptr_get = (ai_ptr_get_fn)r[SH_AI_SIG_ENTITY_PTR_GET].addr;
    site = (const uint8_t *)r[SH_AI_SIG_PATH_SITE].addr;
    g_path_key = (ai_path_key_fn)(site + SH_AI_PATH_SITE_KEY_CALL + 5 + rel32_at(site + SH_AI_PATH_SITE_KEY_CALL + 1));
    g_path_global = (const uint64_t *)(site + SH_AI_PATH_SITE_GLOBAL_LOAD + 7 +
                                       rel32_at(site + SH_AI_PATH_SITE_GLOBAL_LOAD + 3));
    g_path_query = (ai_path_query_fn)(site + SH_AI_PATH_SITE_QUERY_CALL + 5 +
                                      rel32_at(site + SH_AI_PATH_SITE_QUERY_CALL + 1));
    g_ai2_type = AI_IDAI2_TYPEINFO_RVA ? module_base + AI_IDAI2_TYPEINFO_RVA : NULL;
    if (!g_ai2_type || !sh_script_api_engine()) {
        ai_log("Mod scripts cannot drive demons in this session: this is not the game build Snapmap+ supports.");
        return 0;
    }
    if (memcmp((const void *)g_irs_target, k_ai_irs_prologue, SH_AI_IRS_STOLEN) != 0) {
        ai_log("Mod scripts cannot drive demons in this session: the game's script-state function is not the "
               "expected one (another tool may have changed it).");
        return 0;
    }
    /* the remaining game functions: a missing one takes only its own call away */
    g_live = k_engine;
    for (i = SH_AI_SIG_PATH_SITE + 1; i < SH_AI_SIG_COUNT; i++) {
        if (sig_resolve_one(module_base, &SH_AI_SIGNATURES[i], &r[i]) != SIG_OK) {
            r[i].addr = 0;
            ai_log("Mod scripts: the game function %s was not found in this executable; the script call that needs "
                   "it is unavailable in this session.", SH_AI_SIGNATURES[i].name);
        }
    }
    g_move_target = (ai_move_fn)r[SH_AI_SIG_MOVE_TRANSITION].addr;
    g_reset_motion_correction = (void (*)(void *))r[SH_AI_SIG_RESET_CORRECTION].addr;
    if (!g_reset_motion_correction) g_live.reset_motion_correction = NULL;
    if (r[SH_AI_SIG_RECOIL_PLAY].addr) g_recoil_site = (uint8_t *)r[SH_AI_SIG_RECOIL_PLAY].addr;
    else g_live.set_recoil = NULL;
    if (!r[SH_AI_SIG_WEAPON_USE].addr) g_live.weapon_use = NULL;   /* its offsets are the signature's own bytes */
    if ((site = (const uint8_t *)r[SH_AI_SIG_NEXT_PULL].addr) != NULL) {
        g_fire_decl = (ai_fire_decl_fn)(site + SH_AI_NEXT_PULL_DECL_CALL + 5 + rel32_at(site + SH_AI_NEXT_PULL_DECL_CALL + 1));
        g_slot_table = (const int32_t *)(site + SH_AI_NEXT_PULL_SLOT_TABLE + 7 +
                                         rel32_at(site + SH_AI_NEXT_PULL_SLOT_TABLE + 3));
    } else g_live.weapon_ready = NULL;
    if ((site = (const uint8_t *)r[SH_AI_SIG_AIM_FOCUS_SITE].addr) != NULL)
        g_focus = (ai_focus_fn)(site + SH_AI_AIM_FOCUS_CALL + 5 + rel32_at(site + SH_AI_AIM_FOCUS_CALL + 1));
    else g_live.aim_point = NULL;
    if ((site = (const uint8_t *)r[SH_AI_SIG_PAIN_STRING_SITE].addr) != NULL) {
        g_intern = (ai_intern_fn)(site + SH_AI_PAIN_INTERN_CALL + 5 + rel32_at(site + SH_AI_PAIN_INTERN_CALL + 1));
        g_pain_site = (uint8_t *)site + SH_AI_PAIN_STUB_AT;
    } else g_live.pain_string = NULL;
    if (r[SH_AI_SIG_THREAT_ROW].addr) g_threat_target = (ai_threat_row_fn)r[SH_AI_SIG_THREAT_ROW].addr;
    else g_live.block_pain = NULL;
    if (r[SH_AI_SIG_PATH_LENGTH].addr) g_plen_target = (ai_path_length_fn)r[SH_AI_SIG_PATH_LENGTH].addr;
    else g_live.path_points = NULL;
    if (r[SH_AI_SIG_ANIM_OF_PROP].addr && r[SH_AI_SIG_ANIM_DELTA].addr) {
        g_anim_of_prop = (ai_anim_of_prop_fn)r[SH_AI_SIG_ANIM_OF_PROP].addr;
        g_anim_delta = (ai_anim_delta_fn)r[SH_AI_SIG_ANIM_DELTA].addr;
    } else {
        g_live.anim_delta = NULL;
        g_live.anim_valid = NULL;
    }
    /* Without the hand read the weapon calls have no weapon to read. */
    if (r[SH_AI_SIG_WEAPON_IN_SLOT].addr) g_weapon_in_slot = (void *(*)(void *, int))r[SH_AI_SIG_WEAPON_IN_SLOT].addr;
    else { g_live.trigger_state = NULL; g_live.weapon_ready = NULL; }
    if (r[SH_AI_SIG_INVENTORY_ITEM].addr) g_inventory_item = (void *(*)(void *, int))r[SH_AI_SIG_INVENTORY_ITEM].addr;
    else g_live.item = NULL;
    /* the fire calls read engine memory at offsets their signatures contain */
    if (!r[SH_AI_SIG_FIRE_THINK].addr) g_live.fire_modes = NULL;
    if (!r[SH_AI_SIG_MUZZLE_TEST].addr || !r[SH_AI_SIG_CHECK_PULL].addr) g_live.focus_state = NULL;
    if (r[SH_AI_SIG_MUZZLE_TEST].addr) g_muzzle = (uint64_t (*)(void *, int, void *))r[SH_AI_SIG_MUZZLE_TEST].addr;
    else g_live.muzzle_test = NULL;
    if (r[SH_AI_SIG_CLEAR_MOVE].addr) g_clear_move = (void (*)(void *, int))r[SH_AI_SIG_CLEAR_MOVE].addr;
    else g_live.clear_move = NULL;
    if (!r[SH_AI_SIG_IDLE_ACTION].addr) g_live.idle_turn_entry = NULL;
    /* the lasers: add and remove, the component's place in an idAI2 (in the ae_setLaserState handler), two reads
     * of the add that prove where the ribbon decl sits, and the engine's atom call (the pain site) */
    site = (const uint8_t *)r[SH_AI_SIG_LASER_ADD].addr;
    if (site && r[SH_AI_SIG_LASER_REMOVE].addr && r[SH_AI_SIG_LASER_STATE].addr && r[SH_AI_SIG_PAIN_STRING_SITE].addr &&
        !memcmp(site + SH_AI_LASER_ADD_DECL_AT, k_ai_laser_decl_read, sizeof k_ai_laser_decl_read) &&
        !memcmp(site + SH_AI_LASER_ADD_RIBBON_AT, k_ai_laser_ribbon_read, sizeof k_ai_laser_ribbon_read)) {
        g_laser_add = (ai_laser_fn)site;
        g_laser_remove = (ai_laser_fn)r[SH_AI_SIG_LASER_REMOVE].addr;
    } else {
        g_live.laser_add = NULL;
        g_live.laser_remove = NULL;
    }
    if (r[SH_AI_SIG_FOOT_TABLE].addr) g_foot_target = (ai_foot_table_fn)r[SH_AI_SIG_FOOT_TABLE].addr;
    else g_live.rear_footsteps = NULL;
    /* The pain start holds the alert cycle's request and the pain code calls; without it a parked demon takes no
     * pain and dies with no death clip. */
    if ((site = (const uint8_t *)r[SH_AI_SIG_PAIN_START].addr) != NULL &&
        site[SH_AI_PAIN_START_FSM_CALL] == 0xE8 && site[SH_AI_PAIN_START_CODE_CALL] == 0xE8) {
        g_fsm_request_target = (ai_fsm_request_fn)(site + SH_AI_PAIN_START_FSM_CALL + 5 +
                                                   rel32_at(site + SH_AI_PAIN_START_FSM_CALL + 1));
        g_pain_code = (ai_pain_code_fn)(site + SH_AI_PAIN_START_CODE_CALL + 5 +
                                        rel32_at(site + SH_AI_PAIN_START_CODE_CALL + 1));
        sh_script_api_on_frame(pain_take_step);
        ps_resolve(site);
    } else ai_log("Mod scripts: a demon a script drives will play no pain reaction or death clip in this session.");
    if (!g_ps.check) g_live.request_pain = NULL;
    /* the step calls read the stage where the move update does and end a command with the context's call;
     * without either, an order is given as it comes */
    if (r[SH_AI_SIG_CMD_SET_WAIT].addr && r[SH_AI_SIG_MOVE_STAGE].addr && r[SH_AI_SIG_IDLE_ACTION].addr)
        g_cmd_set_wait = (void (*)(void *, void *))r[SH_AI_SIG_CMD_SET_WAIT].addr;
    else {
        g_live.step_state = NULL;
        g_live.step_interrupt = NULL;
    }
    if ((site = (const uint8_t *)r[SH_AI_SIG_GORE_INIT_SITE].addr) != NULL) {
        g_gore_cvar = (const int32_t *)(site + SH_AI_GORE_SITE_CVAR + 7 + rel32_at(site + SH_AI_GORE_SITE_CVAR + 2));
        g_gore_damage = (uint32_t)rel32_at(site + SH_AI_GORE_SITE_DAMAGE + 3);
        g_gore_comp = (uint32_t)rel32_at(site + SH_AI_GORE_SITE_COMP + 3);
        g_gore_init = (ai_gore_init_fn)(site + SH_AI_GORE_SITE_CALL + 5 + rel32_at(site + SH_AI_GORE_SITE_CALL + 1));
    } else g_live.gore_init = NULL;
    g_wound_target = (ai_show_wound_fn)r[SH_AI_SIG_SHOW_WOUND].addr;
    if (!g_wound_target) g_live.wound_effect = NULL;
    if (g_wound_target && (site = (const uint8_t *)r[SH_AI_SIG_MESH_HIDE_SITE].addr) != NULL) {
        g_mesh_index = (ai_mesh_index_fn)(site + SH_AI_MESH_SITE_INDEX + 5 + rel32_at(site + SH_AI_MESH_SITE_INDEX + 1));
        g_mesh_hide = (ai_mesh_hide_fn)(site + SH_AI_MESH_SITE_HIDE + 5 + rel32_at(site + SH_AI_MESH_SITE_HIDE + 1));
    } else g_live.wound_hides = NULL;
    /* posed legs: the rig's entry, the two-bone component's evaluate (its address names the component, its call
     * the solve) and the demon's way to its rig runtime (the setup site's two displacements) */
    site = (const uint8_t *)r[SH_AI_SIG_RIG_SITE].addr;
    if (r[SH_AI_SIG_RIG_ENTRY].addr && r[SH_AI_SIG_RIG_RUN].addr && r[SH_AI_SIG_RIG_BLOCK].addr &&
        r[SH_AI_SIG_RIG_EVAL].addr && r[SH_AI_SIG_RIG_SOLVE].addr && site &&
        ((const uint8_t *)r[SH_AI_SIG_RIG_EVAL].addr)[SH_AI_RIG_EVAL_SOLVE_CALL] == 0xE8 &&
        (const uint8_t *)r[SH_AI_SIG_RIG_EVAL].addr + SH_AI_RIG_EVAL_SOLVE_CALL + 5 +
            rel32_at((const uint8_t *)r[SH_AI_SIG_RIG_EVAL].addr + SH_AI_RIG_EVAL_SOLVE_CALL + 1) ==
            (const uint8_t *)r[SH_AI_SIG_RIG_SOLVE].addr) {
        g_rig_target = (ai_rig_entry_fn)r[SH_AI_SIG_RIG_ENTRY].addr;
        g_rig_eval = (const void *)r[SH_AI_SIG_RIG_EVAL].addr;
        g_rig_solve = (ai_rig_solve_fn)r[SH_AI_SIG_RIG_SOLVE].addr;
        g_rig_model_off = (uint32_t)rel32_at(site + SH_AI_RIG_SITE_MODEL);
        g_rig_anim_off = (uint32_t)rel32_at(site + SH_AI_RIG_SITE_ANIM);
    } else g_live.posed_legs = NULL;
    /* the fk reference pose and the web's current leaf it calls first (the call at +0xA7 must reach that leaf) */
    site = (const uint8_t *)r[SH_AI_SIG_FK_REFERENCE].addr;
    if (site && r[SH_AI_SIG_WEB_LEAF].addr && site[SH_AI_FKREF_LEAF_CALL] == 0xE8 &&
        site + SH_AI_FKREF_LEAF_CALL + 5 + rel32_at(site + SH_AI_FKREF_LEAF_CALL + 1) ==
            (const uint8_t *)r[SH_AI_SIG_WEB_LEAF].addr) {
        g_fkref_target = (ai_fkref_fn)site;
        g_leaf_target = (ai_web_leaf_fn)r[SH_AI_SIG_WEB_LEAF].addr;
    } else g_live.fk_reference_pose = NULL;
    /* The interrupt mode: ForceState writes interruptBlend at web +0x570 and the update reads it there (both
     * found, the offsets the sigs test checks); the web is ai +0x222B8, the fk reference pose's check above. */
    if (!r[SH_AI_SIG_WEB_FORCE_STATE].addr || !r[SH_AI_SIG_WEB_UPDATE].addr || !g_live.fk_reference_pose)
        g_live.interrupt_blend = NULL;
    /* Destructibles: the weapon-specific gore of a gore breakable (detoured), the break and the broad phase move
     * (called; the sigs test checks the offsets each proves). */
    if (r[SH_AI_SIG_BREAKABLE_GORE].addr && r[SH_AI_SIG_BREAKABLE_BREAK].addr && r[SH_AI_SIG_FXPHYS_PLACE].addr) {
        g_brkg_target = (ai_brk_gore_fn)r[SH_AI_SIG_BREAKABLE_GORE].addr;
        g_brk_break = (ai_brk_break_fn)r[SH_AI_SIG_BREAKABLE_BREAK].addr;
        g_fxphys_place = (ai_fxphys_place_fn)r[SH_AI_SIG_FXPHYS_PLACE].addr;
    } else g_live.destructible = NULL;
    /* Wound damage counts: DamageActivatesWound (detoured at the first call; its window and the offsets the
     * detour reads are the sigs test's) */
    if (r[SH_AI_SIG_DAMAGE_WOUND].addr) g_daw_target = (ai_daw_fn)r[SH_AI_SIG_DAMAGE_WOUND].addr;
    else g_live.wound_damage_counts = NULL;
    /* the anim web's set-by-name (its offsets are the signature's own bytes) and the engine's atom call (the pain
     * site), which makes the name the web's rows compare */
    if (r[SH_AI_SIG_WEB_SCALAR_SET].addr && g_intern)
        g_web_scalar_set = (ai_web_scalar_set_fn)r[SH_AI_SIG_WEB_SCALAR_SET].addr;
    else g_live.anim_variable = NULL;
    /* The relations: the senses' table, whose layout the two functions show (the sigs test checks them). */
    if (!r[SH_AI_SIG_UPDATE_RELATIONS].addr || !r[SH_AI_SIG_BEST_RELATION].addr) g_live.relations = NULL;
    if (r[SH_AI_SIG_FSM_MOVE].addr) {
        /* idTransitionCode's vtable: lea rcx,[rip+disp] in the entrance move */
        const uint8_t *m = (const uint8_t *)r[SH_AI_SIG_FSM_MOVE].addr;
        __try {
            if (!memcmp(m + SH_AI_FSM_MOVE_VT_LEA, "\x48\x8D\x0D", 3) &&
                !memcmp(m + SH_AI_FSM_MOVE_CODE_STORE, "\xC7\x44\x24\x78\x05\x00\x00\x00", 8) &&
                !memcmp(m + SH_AI_FSM_MOVE_TAKE_CALL, "\xFF\x90\xA8\x00\x00\x00", 6))
                g_fsm_code_vt = m + SH_AI_FSM_MOVE_VT_LEA + 7 + rel32_at(m + SH_AI_FSM_MOVE_VT_LEA + 3);
        } __except (av_only(GetExceptionCode())) { g_fsm_code_vt = NULL; }
    }
    g_game_time = (ai_game_time_fn)r[SH_AI_SIG_GAME_TIME].addr;
    sh_script_ai_set_engine(&g_live);
    backend_log("SCRIPTS: AI calls resolved; the demon park hook is installed at the first command");
    return 1;
}

#endif /* SH_SCRIPT_PURE */
