/* script_api_ai.h -- the AI calls of mod scripts (API version 1): a script
 * that drives one of its own demons moves it, turns it, plays its animWeb
 * nodes, picks its enemy, fires its weapon and asks what it perceives.
 *
 * Every call is a method of an entity handle and works only on a demon (an
 * idAI2) of the calling mod; on any other entity it is refused with one
 * plain sentence, before anything reaches the engine.
 *
 * The park. A demon a script commands leaves its own behaviour and waits in
 * the engine's own ExecuteDoomscript alert-cycle state: the first command a
 * script gives it marks it parked, and from then on the engine's
 * IsRunningScript(ai) answers true for it (a detour on that one function),
 * so the alert cycle enters ExecuteDoomscript from its default state and its
 * NotExecutingScript link never fires. That state does nothing on its own
 * (its class overrides no state method), the designer commands the script
 * posts (ssaction_*) take effect there, and the engine keeps pain, damage,
 * death and the animation system. The command also posts
 * setScriptAbort 0 0 and setScriptAbortFlags 0 0, without which a demon in
 * combat leaves its commanded point. The engine's script proxy forces the
 * alert cycle into ExecuteDoomscript when a command arrives while the demon
 * is elsewhere (for example in its combat behaviour), so a parked demon
 * comes back on the next command. Nothing is edited in any decl and the
 * author names nothing: any idAI2 entityDef of a mod can be driven.
 * A demon stays parked for the rest of the map, also after its mod's
 * scripts stop; it then idles where it stands and still takes pain and dies.
 *
 * Threads: every Lua call runs on the game thread inside the script tick.
 * The IsRunningScript detour runs on FSM worker threads and only reads the
 * parked table (lock-free).
 *
 * SH_SCRIPT_PURE builds the Lua side over the engine seam below without the
 * engine (the tests install a fake engine). */
#ifndef SH_SCRIPT_API_AI_H
#define SH_SCRIPT_API_AI_H

#include <stddef.h>
#include <stdint.h>
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- catalogue */

/* The engine events the AI calls post, by catalogue index. Each one's
 * argument format is the live 13954591 event registry's (one character per
 * argument: 'v' idVec3&, 'i' int or enum, 'f' float, 'b' bool, 'e' entity,
 * 's' char*, '1' animWeb path, '4' idMD6Anim* which is always NULL here). At
 * boot each name is looked up and its format compared; a call whose event
 * differs is refused for the session with a plain line, never sent with the
 * wrong arguments. */
enum sh_ai_ev {
    SH_AI_EV_MOVE_TO_POINT,          /* ssaction_MoveToPoint       vifbv */
    SH_AI_EV_IDLE,                   /* ssaction_Idle              ii    */
    SH_AI_EV_SET_WALK_STATE,         /* ssaction_SetWalkState      i     */
    SH_AI_EV_SET_PLAYER_FOCUS,       /* ssaction_SetPlayerFocus    ibb   */
    SH_AI_EV_CHANGE_ANIM_STATE,      /* ssaction_ChangeAnimState   i1i   */
    SH_AI_EV_CHANGE_ANIM_STATE_VIA,  /* ssaction_ChangeAnimStateVia i11i */
    SH_AI_EV_FORCE_ANIM_STATE,       /* ssaction_ForceAnimState    i1ii  */
    SH_AI_EV_SET_FOCUS,              /* setFocus                   ebbi  */
    SH_AI_EV_CLEAR_AIM_FOCUS,        /* clearAimFocus                    */
    SH_AI_EV_CLEAR_LOOK_FOCUS,       /* clearLookFocus                   */
    SH_AI_EV_SET_ENEMY,              /* setEnemy                   eb    */
    SH_AI_EV_SET_PLAYER_ENEMY,       /* setPlayerEnemy             i     */
    SH_AI_EV_IS_ENTITY_VISIBLE,      /* isEntityVisible            e     */
    SH_AI_EV_HAS_AWARENESS_OF_ENEMY, /* hasAwarenessOfEnemy              */
    SH_AI_EV_PULL_TRIGGER,           /* pullTriggerRight                 */
    SH_AI_EV_RELEASE_TRIGGER,        /* releaseTriggerRight              */
    SH_AI_EV_EQUIP_ITEM,             /* equipInventoryItemByIndex  i     */
    SH_AI_EV_SET_LASER_STATE,        /* setLaserState              bs    */
    SH_AI_EV_START_LASER_SWEEP,      /* ae_startLaserSweep         4s    */
    SH_AI_EV_SET_FOCUS_TAG,          /* ae_setOverrideFocusTagName 4is   */
    SH_AI_EV_CLEAR_FOCUS_TAG,        /* ae_clearOverrideFocusTag   4i    */
    SH_AI_EV_SET_SCRIPT_ABORT,       /* setScriptAbort             bb    */
    SH_AI_EV_SET_SCRIPT_ABORT_FLAGS, /* setScriptAbortFlags        bb    */
    SH_AI_EV_AE_SET_LASER_STATE,     /* ae_setLaserState           4bs   */
    SH_AI_EV_COUNT
};

typedef struct sh_ai_event_def {
    const char *name;
    const char *format;
} sh_ai_event_def;
extern const sh_ai_event_def SH_AI_EVENTS[SH_AI_EV_COUNT];

/* Engine enum values the calls use (13954591 enum tables). */
#define SH_AI_ARRIVAL_STOP          2   /* aiArrivalAction_t AIARRIVAL_STOP */
#define SH_AI_IDLETURN_COMBAT       1   /* aiIdleTurn_t */
#define SH_AI_ANIMWEB_BODY          0   /* aiAnimWeb_t */
#define SH_AI_ANIMWAIT_DONT_WAIT    0   /* aiAnimWait_t */
#define SH_AI_WALKSTATE_WALKING     1   /* walkState_t */
#define SH_AI_WALKSTATE_RUNNING     2
#define SH_AI_AIMPOINT_CENTER       3   /* aimPoint_t */
#define SH_AI_PLAYER_CLOSEST        0   /* aiPlayer_t */
#define SH_AI_FOCI_AIM              0   /* aiFoci_t */
#define SH_AI_AWARE_UNAWARE         0   /* AIAWARE_ */
#define SH_AI_AWARE_LOST            1
#define SH_AI_AWARE_SUSPECTED       2
#define SH_AI_AWARE_CONFIRMED       3

/* One event argument as the calls build it (the engine side turns it into
 * the engine's 0x18-byte argument record). */
typedef struct sh_ai_arg {
    char        type;     /* the format character it fills */
    int         i;        /* 'i', 'b' */
    float       f;        /* 'f' */
    float       v[3];     /* 'v' */
    const char *s;        /* 's', '1' */
    void       *ent;      /* 'e': the engine entity */
} sh_ai_arg;

/* 1 when args[0..n-1] fill `format` exactly ('4' takes a NULL anim). Pure. */
int sh_ai_args_fit(const char *format, const sh_ai_arg *args, int n);

/* 1 when the registry's format for event `ev` is the catalogue's. Pure. */
int sh_ai_event_format_ok(int ev, const char *registry_format);

/* -------------------------------------------------------- pure decisions */

/* Move completion from the move FSM's status word (moveFSM +0xD20: 0 idle
 * or arrived, 1..4 under way, 5 and above failed). A command takes effect
 * at the demon's next think, so the status is still 0 on the frame it is
 * given: the move counts as done when a status other than 0 was seen and 0
 * came back (arrived), when a failure status is seen (failed), or when the
 * status stayed 0 for SH_AI_MOVE_SETTLE frames after the command (the
 * demon was already there, or the engine dropped the command). */
#define SH_AI_MOVE_SETTLE 3
#define SH_AI_MOVE_NONE     0   /* no move commanded */
#define SH_AI_MOVE_RUNNING  1
#define SH_AI_MOVE_ARRIVED  2
#define SH_AI_MOVE_FAILED   3
typedef struct sh_ai_move {
    int      state;       /* SH_AI_MOVE_ */
    int      seen;        /* a status other than 0 was seen since the command */
    uint32_t issued;      /* frame of the command */
} sh_ai_move;
void sh_ai_move_issue(sh_ai_move *m, uint32_t frame);
/* Feed the status read at `frame`; returns the move's state. */
int  sh_ai_move_step(sh_ai_move *m, int status, uint32_t frame);

/* The awareness word's name: "unaware", "lost", "suspected", "confirmed";
 * NULL for any other value. */
const char *sh_ai_awareness_name(int level);

/* Degrees from a demon's facing (yaw, degrees) to `to` seen from `from`,
 * in the horizontal plane: -180..180, positive to the demon's left. */
float sh_ai_angle_to(const float from[3], float yaw, const float to[3]);

/* find_position's candidates: `count` points on the circle of radius `r`
 * around `center`, the first one on the side of `toward` (the demon), the
 * rest every 360/count degrees around, at center's height. */
void sh_ai_ring(const float center[3], const float toward[3], float r, int count, float (*out)[3]);

/* find_position's score of a candidate (lower is better): its distance
 * from the ideal distance to the target plus the path length times
 * path_weight. */
float sh_ai_position_score(float dist_to_target, float ideal, float path_len, float path_weight);

/* ------------------------------------------------------------ engine seam */

typedef struct sh_script_ai_engine {
    void    *(*entity)(sh_script_handle h);          /* the live entity, NULL when gone */
    sh_script_handle (*handle_of)(void *entity);      /* a handle a script can hold, 0 for none */
    int      (*is_ai)(void *entity);                  /* 1 an idAI2 (or a subclass) */
    int      (*origin)(void *entity, float out[3]);   /* physics origin */
    int      (*yaw)(void *entity, float *deg);        /* facing, degrees */
    /* Run catalogue event `ev` on `entity` now. Returns 1 when it ran, with
     * its return value (an int, bool or float the event returns) in *ret
     * when ret is not NULL; 0 when it is unavailable or faulted. */
    int      (*event)(void *entity, int ev, const sh_ai_arg *args, int n, double *ret);
    int      (*park)(void *ai);                       /* 1 parked (now or before) */
    void    *(*enemy)(void *ai);                      /* the current enemy entity, or NULL */
    int      (*awareness)(void *ai, void *target);    /* AIAWARE_ level, -1 unreadable */
    int      (*move_status)(void *ai);                /* move FSM status word, -1 unreadable */
    int      (*trigger_state)(void *ai);              /* 1 trigger held, 0 not, -1 no weapon */
    /* Path from the demon to `goal`: 0 found (*len inches), 1 still being
     * searched (ask again), 2 no path, -1 unavailable. `key` names the
     * request in the engine's path cache. */
    int      (*path)(void *ai, const float goal[3], int key, float *len);
    uint32_t (*frame)(void);                          /* game frames with scripts so far */
} sh_script_ai_engine;

/* The engine the calls use (the engine side installs the real one; tests a
 * fake). NULL makes every call refuse with "not available". */
void sh_script_ai_set_engine(const sh_script_ai_engine *engine);

/* Add the AI calls to every script state opened after this. Returns the
 * number of calls added. */
int sh_script_ai_register(void);

/* Forget every per-demon record (a new map). The parked table is the
 * engine side's and is checked by spawn id, so a stale entry never parks
 * another map's demon. */
void sh_script_ai_reset(void);

#ifndef SH_SCRIPT_PURE
#include "../signatures.h"
/* Boot: resolve the signatures (script_ai_sigs.c), look up each catalogue
 * event and compare its format, prepare the IsRunningScript detour (it is
 * committed at the first park, on the game thread), and register the calls
 * with the engine seam. Returns 1 when the AI calls are usable; 0 with one
 * plain log line otherwise (AI calls then refuse with "not available"). */
int sh_script_ai_install(const uint8_t *module_base);
#endif

/* ------------------------------------------------------------- signatures */

/* script_ai_sigs.c: the private signatures (two-image test:
 * tests/script_ai_sigs_test.c). */
#include "../signatures.h"
enum sh_ai_sig {
    SH_AI_SIG_IS_RUNNING_SCRIPT,   /* idAI2::IsRunningScript (detoured) */
    SH_AI_SIG_GET_AWARENESS,       /* perception GetAwareness(perception, entity) */
    SH_AI_SIG_ENTITY_PTR_GET,      /* idEntityPtr resolve (ptr) -> entity */
    SH_AI_SIG_PATH_SITE,           /* Shared_TargetDistanceFilter's path query call site */
    SH_AI_SIG_COUNT
};
extern const sig_entry SH_AI_SIGNATURES[];   /* enum order, NULL-terminated */

/* Inside the path site: the path-key call (E8 at +5), the mov rcx,[rip+]
 * of the query's first word (48 8B 0D at +10) and the path query call (E8
 * at +0x6B). */
#define SH_AI_PATH_SITE_KEY_CALL    0x05u
#define SH_AI_PATH_SITE_GLOBAL_LOAD 0x0Au
#define SH_AI_PATH_SITE_QUERY_CALL  0x6Bu

/* The IsRunningScript detour window: push rsi; sub rsp,0x20;
 * mov eax,[rcx+0x23BC0]; mov rsi,rcx (whole, position-independent). */
#define SH_AI_IRS_STOLEN 15
static const uint8_t k_ai_irs_prologue[SH_AI_IRS_STOLEN] = {
    0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81, 0xC0, 0x3B, 0x02, 0x00, 0x48, 0x8B, 0xF1
};

/* idAI2 offsets the calls read (both renderers, 13954591). */
#define SH_AI_OFF_MOVE_FSM        0x23198u  /* move FSM pointer; status int at +0xD20 */
#define SH_AI_OFF_MOVE_STATUS     0xD20u
#define SH_AI_OFF_WEAPON_HOLDER   0x22278u  /* +8: the equipped weapon (else vtable +0xB28) */
#define SH_AI_VSLOT_WEAPON        0xB28u
#define SH_AI_OFF_WEAPON_SLOT     0x8D4u    /* weapon: active trigger index */
#define SH_AI_OFF_WEAPON_TRIGGER  0x8F8u    /* weapon: trigger state per index, 0 = pulled */
#define SH_AI_OFF_ENEMY           0x25050u  /* idEntityPtr of the current enemy */
#define SH_AI_OFF_PERCEPTION      0x29A88u
#define SH_AI_OFF_PATH_NAME       0x388u    /* the name the path key is built from */
#define SH_AI_OFF_PHYSICS         0x808u    /* idEntity: physics; vtable +0x80 origin(0), +0x88 axis(0) */
#define SH_AI_VSLOT_ORIGIN        0x80u
#define SH_AI_VSLOT_AXIS          0x88u
#define SH_AI_TYPE_NUM            0x68u     /* idTypeInfo: type number, +0x6C last descendant */

#ifdef __cplusplus
}
#endif
#endif
