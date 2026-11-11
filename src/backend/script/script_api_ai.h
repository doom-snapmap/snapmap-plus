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
    SH_AI_EV_GET_MONSTER_TYPE,       /* getMonsterType             -> i  */
    SH_AI_EV_PULL_TRIGGER_LEFT,      /* pullTriggerLeft                  */
    SH_AI_EV_RELEASE_TRIGGER_LEFT,   /* releaseTriggerLeft               */
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

/* --------------------------------------- the calls the Tyrant's brain needs */

/* anim_valid's test words (the TF_* words of an AnimationValid condition,
 * and the attack graph's VALIDATE_ANIM_DELTA), as bits. */
#define SH_AI_TF_NAV_DIRECT        0x001u   /* the demon has a navigation path to where the clip ends */
#define SH_AI_TF_NAV_ENDPOINT      0x002u   /* the clip ends on the ground */
#define SH_AI_TF_CLAMP_TO_GROUND   0x004u   /* the end point is taken down onto the ground */
#define SH_AI_TF_BOUNDS            0x008u   /* nothing in the world is in the way of the motion */
#define SH_AI_TF_EYE_VISIBILITY    0x010u   /* from the end point the demon's eyes see its enemy */
#define SH_AI_TF_MUZZLE_VISIBILITY 0x020u   /* the same sight from weapon height */
#define SH_AI_TF_TO_ENEMY_FORCE    0x040u   /* the motion does not carry the demon toward its enemy */
#define SH_AI_TF_AVOID_ENEMY_FOV   0x080u   /* the end point is out of the centre of the enemy's view */
#define SH_AI_TF_ANIM_DELTA        (SH_AI_TF_NAV_ENDPOINT | SH_AI_TF_BOUNDS)   /* VALIDATE_ANIM_DELTA */
#define SH_AI_FOV_CENTRE_DEG       20.0f    /* TF_AVOID_ENEMY_FOV: the cone left out around the enemy's aim */
/* The bits of a space-separated word list; 0 with the first word it does
 * not know copied into bad[cap]. Pure. */
unsigned sh_ai_tests_parse(const char *words, char *bad, size_t cap);

/* aiMonsterType_t of build 13954591: one bit per type. The name of a value
 * ("AI_MONSTER_BARON"), NULL for a value that is not exactly one type.
 * tests/script_ai_sigs_test.c compares the table with both executables. */
#define SH_AI_MONSTER_TYPES 24
extern const char *const SH_AI_MONSTER_NAMES[SH_AI_MONSTER_TYPES];   /* [0] NONE (0), [k] value 1 << (k - 1) */
const char *sh_ai_monster_name(uint32_t value);

/* The point at fraction t of the line from a to b, moved `offset` inches
 * across it in the plane the roll gives (roll 90: to the left of the line,
 * -90: to its right, 0: up). Pure. */
void sh_ai_line_offset(const float a[3], const float b[3], float t, float offset, float roll_deg, float out[3]);

/* A control table: n knots t (0..1, rising) with values, linear between
 * knots and 0 outside them; any number of knots (t and v point at the
 * caller's arrays). Pure. */
typedef struct sh_ai_table {
    int    n;
    float *t;
    float *v;
} sh_ai_table;
float sh_ai_table_at(const sh_ai_table *table, float t);

/* spline_clear's curve from `from` to `to`: the two ends and one point per
 * knot inside, each moved value x horizontal length across the line in the
 * direction the roll gives. Returns the points written (2..cap; a cap of
 * table->n + 2 holds every point). Pure. */
int sh_ai_spline_points(const float from[3], const float to[3], const sh_ai_table *table, float roll_deg,
                        float (*out)[3], int cap);

/* firing_position's kinds. */
#define SH_AI_POS_ANY        0   /* the best place, the current one included */
#define SH_AI_POS_NEW        1   /* the best place at least SH_AI_POS_MOVE_MIN from where the demon stands */
#define SH_AI_POS_SUPERIOR   2   /* a new place scoring better than the current one by SH_AI_POS_MARGIN */
#define SH_AI_POS_REPOSITION 3   /* a new place on the far half of the ring from the demon */
#define SH_AI_POS_MOVE_MIN   128.0f
#define SH_AI_POS_MARGIN     96.0f
int sh_ai_position_kind(const char *word);   /* -1 for a word it does not know */

typedef struct sh_ai_position_query {
    int   kind;
    void *target;                 /* the target entity (NULL: `at` alone) */
    float at[3];                  /* the target's position */
    float min, max, ideal;        /* inches from the target */
    int   visible;                /* the place must see the target */
    int   count;                  /* candidates on the ring (2 or more, any number) */
    float path_weight;
} sh_ai_position_query;

typedef struct sh_ai_spline_query {
    void       *target;
    float       to[3];            /* the target's position `seconds` ahead */
    sh_ai_table table;
    float       roll;
    int         nav_ground;       /* TF_NAV_GROUND: every point stands on the ground */
} sh_ai_spline_query;

/* Per-demon records the engine reads from other threads: the pain string and
 * the blocked pain reactions. Lock-free, keyed by the demon and its spawn id
 * (a new entity in the same memory never inherits a record). Pure. */
#define SH_AI_PAIN_REACTIONS 14
void        sh_ai_pain_records_reset(void);
int         sh_ai_pain_string_set(void *ai, uint32_t spawn_id, const char *interned);   /* NULL clears */
const char *sh_ai_pain_string_get(void *ai, uint32_t spawn_id);
int         sh_ai_block_set(void *ai, uint32_t spawn_id, int reaction, int until_ms);
int         sh_ai_blocked(void *ai, uint32_t spawn_id, int reaction, int now_ms);

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
    /* Primitives the calls below are made of. */
    int      (*eye)(void *ai, float out[3]);          /* where the demon looks from */
    /* A world trace from `from` to `to` passing the demon: 1 clear (or the
     * first thing hit is `target`, when given), 0 blocked, -1 unreadable. */
    int      (*sight)(void *ai, const float from[3], const float to[3], void *target);
    int      (*ground)(const float pos[3], float out[3]);   /* the floor under pos: 1 found */
    /* The root motion of animation `index` of animWeb node "subweb/state" of
     * the demon, in its own frame (x forward, y left, z up). 1 read, 0 not
     * (index past the last); *count gets the node's animations; -1 no such
     * node. */
    int      (*anim_delta)(void *ai, const char *node, int index, float out[3], int *count);
    /* Engine answers (the engine side composes the queries from the
     * primitives with sh_ai_*_run below; a test engine answers directly). */
    int      (*set_recoil)(void *ai, float alpha);
    int      (*weapon_ready)(void *ai);               /* 1 ready, 0 cooling down, -1 no weapon */
    int      (*aim_point)(void *ai, const float pos[3]);
    /* 1 valid (*index: the animation), 0 not, 2 still being asked, -1 no such node */
    int      (*anim_valid)(void *ai, const char *node, unsigned tests, int side, int *index);
    /* 0 found (*n points), 1 still searching, 2 no path, -1 unavailable */
    /* the computed path's points, whole: *out malloc'd (the caller frees it), *n its count */
    int      (*path_points)(void *ai, const float goal[3], int key, float (**out)[3], int *n);
    /* 0 found, 1 still searching, 2 none, -1 unavailable */
    int      (*firing_position)(void *ai, const sh_ai_position_query *q, float out[3]);
    /* 1 clear (*n points), 0 blocked, 2 still searching, -1 unavailable */
    int      (*spline_clear)(void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap, int *n);
    int      (*pain_string)(void *ai, const char *text);        /* NULL: the engine's own again */
    int      (*block_pain)(void *ai, int reaction, int ms);     /* refuse `reaction` for ms */
} sh_script_ai_engine;

/* The compositions the engine side answers queries with, over an engine's
 * primitives (pure: the tests run them over a fake). key_base names the
 * path requests in the engine's path cache. */
int sh_ai_firing_run(const sh_script_ai_engine *e, void *ai, const sh_ai_position_query *q, int key_base,
                     float out[3]);
int sh_ai_spline_run(const sh_script_ai_engine *e, void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap,
                     int *n);
int sh_ai_anim_valid_run(const sh_script_ai_engine *e, void *ai, const char *node, unsigned tests, int side,
                         int key_base, int *index);
/* The engine's path cache is keyed by an int: each demon's record gets
 * SH_AI_KEY_SPAN keys for its queries (a query with more candidates than that
 * shares keys in turn; the engine then answers "searching" a little longer). */
#define SH_AI_KEY_SPAN 0x4000

/* The engine the calls use (the engine side installs the real one; tests a
 * fake). NULL makes every call refuse with "not available". */
void sh_script_ai_set_engine(const sh_script_ai_engine *engine);

/* Add the AI calls to every script state opened after this. Returns the
 * number of calls added. distance, anim_state and anim_force are the entity
 * calls' (script_api_entity.c), which work on any entity; on a demon
 * anim_state and anim_force come here. */
int sh_script_ai_register(void);

/* Park the mod's own demon `ai` (handle h) and ask its body animWeb for
 * `dest` now: ssaction_ChangeAnimState, ..Via through `via`, or with force
 * ssaction_ForceAnimState blending blend_ms milliseconds. 1 when the engine
 * ran it; 0 when the demon cannot be parked or the AI calls are unavailable
 * this session. Game thread, inside the script tick. */
int sh_script_ai_anim(void *ai, sh_script_handle h, const char *dest, const char *via, int force, int blend_ms);

/* Park the mod's own demon `ai` (handle h) without a command: 1 when it is
 * parked. ai:fire uses it, so a demon a script fires from stops its own
 * behaviour like one it moves. Game thread, inside the script tick. */
int sh_script_ai_park(void *ai, sh_script_handle h);

/* Forget every per-demon record (a new map). The parked table is the
 * engine side's and is checked by spawn id, so a stale entry never parks
 * another map's demon. */
void sh_script_ai_reset(void);

#ifndef SH_SCRIPT_PURE
#include "../signatures.h"
/* Boot, after the entity calls' engine side (sh_script_api_install, whose
 * shared event helper runs every command and checks its arguments against
 * the game's event by name): resolve the signatures (script_ai_sigs.c),
 * prepare the IsRunningScript detour (it is committed at the first park, on
 * the game thread), and register the calls with the engine seam. Returns 1
 * when the AI calls are usable; 0 with one plain log line otherwise (AI
 * calls then refuse with "not available"). */
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
    SH_AI_SIG_RECOIL_PLAY,         /* the recoil play: the animator weight times ai +0x24EB0 */
    SH_AI_SIG_NEXT_PULL,           /* the weapon holder's next-pull writer (fire decl, slot table) */
    SH_AI_SIG_AIM_FOCUS_SITE,      /* ae_selectAimFocusPoint's set-focus-to-position call */
    SH_AI_SIG_PAIN_STRING_SITE,    /* the pain request: its painString accessor branch */
    SH_AI_SIG_THREAT_ROW,          /* the threat table's per-reaction check (detoured) */
    SH_AI_SIG_PATH_LENGTH,         /* the path length of a computed path (detoured) */
    SH_AI_SIG_ANIM_OF_PROP,        /* an animWeb node's animation for a model */
    SH_AI_SIG_ANIM_DELTA,          /* an animation's total root motion */
    SH_AI_SIG_COUNT
};
extern const sig_entry SH_AI_SIGNATURES[];   /* enum order, NULL-terminated */

/* Inside the path site: the path-key call (E8 at +5), the mov rcx,[rip+]
 * of the query's first word (48 8B 0D at +10) and the path query call (E8
 * at +0x6B). */
#define SH_AI_PATH_SITE_KEY_CALL    0x05u
#define SH_AI_PATH_SITE_GLOBAL_LOAD 0x0Au
#define SH_AI_PATH_SITE_QUERY_CALL  0x6Bu

/* Inside the recoil play: mulss xmm6,[rsi+0x24EB0] at +0x29 reads the
 * recoil strength. */
#define SH_AI_RECOIL_SITE_MUL       0x29u
/* Inside the next-pull writer: the fire-decl call (E8 at +0x52) and the
 * trigger-slot table (lea rbp,[rip+] at +0x57). */
#define SH_AI_NEXT_PULL_DECL_CALL   0x52u
#define SH_AI_NEXT_PULL_SLOT_TABLE  0x57u
/* Inside the aim focus site: the set-focus-to-position call (E8 at +0x3D). */
#define SH_AI_AIM_FOCUS_CALL        0x3Du
/* Inside the pain string site: the atomic-string set call (E8 at +0x24) and
 * the 14 bytes at +0x36 (mov rax,[rbx+0xC120]; mov rcx,[rax+0x90]) where the
 * pain request's string ([rbp+0x88]) is final; the stub replaces it there. */
#define SH_AI_PAIN_INTERN_CALL      0x24u
#define SH_AI_PAIN_STUB_AT          0x36u
#define SH_AI_PAIN_STUB_STOLEN      14
#define SH_AI_PAIN_STRING_SLOT      0x88u
static const uint8_t k_ai_pain_stolen[SH_AI_PAIN_STUB_STOLEN] = {
    0x48, 0x8B, 0x83, 0x20, 0xC1, 0x00, 0x00, 0x48, 0x8B, 0x88, 0x90, 0x00, 0x00, 0x00
};
/* Build the pain-string stub at `code` (SH_AI_PAIN_STUB_SIZE bytes): it saves
 * the volatile registers, calls fetch(rbx = the demon) and, when that returns
 * a string, stores it in [rbp+0x88]; then it restores everything and jumps
 * to `resume` (the trampoline that runs the stolen bytes). Pure. */
#define SH_AI_PAIN_STUB_SIZE 192
size_t sh_ai_pain_stub_build(uint8_t *code, size_t cap, const void *fetch, const void *resume);
/* Detour windows, whole and position-independent. */
#define SH_AI_THREAT_STOLEN 15
#define SH_AI_PATH_LENGTH_STOLEN 14
/* idAI2 offsets of the new calls (both renderers). */
#define SH_AI_OFF_RECOIL          0x24EB0u  /* float recoil strength (FSM states write recoilStrength here) */
#define SH_AI_OFF_ANIMWEB         0x225C8u  /* the animWeb decl */
#define SH_AI_DECL_FIRE_TRIGGER   0x1B4u    /* fire decl: trigger type, index into the slot table */
#define SH_AI_OFF_ACTIVE_FIRE     0x8D4u    /* weapon: active fire mode */
#define SH_AI_THREAT_ROW_REACTION 0x1Cu     /* SDPSReaction_t: reaction painType_t */
#define SH_AI_PATH_SEGMENTS       0x90u     /* computed path: segments (stride 0x1C, from +4, to +0x10) */
#define SH_AI_PATH_SEGMENT_COUNT  0x70u

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
