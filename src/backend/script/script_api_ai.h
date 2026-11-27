/* script_api_ai.h -- the AI calls of mod scripts: a script moves, turns, animates, arms and queries its own demons, and
 * any entity that is not an idAI2 of the calling mod is refused before the engine is reached. Lua calls run on the game
 * thread; SH_SCRIPT_PURE builds the Lua side over the engine seam below, with a fake engine in the tests. */
#ifndef SH_SCRIPT_API_AI_H
#define SH_SCRIPT_API_AI_H

#include <stddef.h>
#include <stdint.h>
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- catalogue */

/* The engine events the AI calls post, by catalogue index, with the argument format of the 13954591 event registry
 * ('v' idVec3&, 'i' int or enum, 'f' float, 'b' bool, 'e' entity, 's' char*, '1' animWeb path, '4' idMD6Anim*, always
 * NULL). A call whose event's format differs at boot is refused for the session, never sent. */
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
    SH_AI_EV_SET_FIRE_MODE,          /* setFireMode                i     */
    SH_AI_EV_AE_PULL_TRIGGER,        /* ae_aIPullTrigger           4sb   */
    SH_AI_EV_ENABLE_BODY_ROTATION,   /* enableBodyRotation         b     */
    SH_AI_EV_ENABLE_HEAD_TRACKING,   /* enableHeadTracking         b     */
    SH_AI_EV_TEMP_MEMORY_REMOVE,     /* tempMemory_Remove          s     */
    SH_AI_EV_COUNT
};

/* A SnapMap order: SnapMap's Follow action (idSnapMapAction_FollowPath) writes it into the demon's temp memory for the
 * stock FSM, which takes it and removes the entry; a demon its own script drives reads and removes it here. */
#define SH_AI_ORDER_NONE      0
#define SH_AI_ORDER_PATH      1   /* snapmap_forceFollowPath: the first info/path node of a path */
#define SH_AI_ORDER_ENTITY    2   /* snapmap_forceFollowEntity: an entity to follow */
#define SH_AI_ORDER_POSITION  3   /* snapmap_forceMoveToPosition: a point, three slots */
extern const char *const SH_AI_ORDER_KEYS[4];   /* the keys, by kind ([0] NULL) */
typedef struct sh_ai_order {
    int      kind;
    uint32_t spawn_id;           /* PATH, ENTITY: the target's spawn id */
    float    pos[3];             /* POSITION */
} sh_ai_order;
/* The temp memory's layout (idAI2 +0x25B30, both renderers; VK 0x1403D4120 finds a key's slots): 256 32-bit
 * values, then at +0x400 256 keys (each a pointer to its interned text). */
#define SH_AI_OFF_TEMP_MEMORY  0x25B30u
#define SH_AI_TEMP_SLOTS       256
#define SH_AI_TEMP_KEYS        0x400u

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
#define SH_AI_AIMPOINT_BEST        10   /* the game's AIMPOINT_BEST (DOOM Eternal's is 11: it adds MEAT_HOOK at 10) */
/* The game's aimPoint_t names without their prefix, by value (0 ORIGIN .. 12 LOOKAHEAD). */
extern const char *const SH_AI_AIMPOINT_NAMES[13];
#define SH_AI_PLAYER_CLOSEST        0   /* aiPlayer_t */
#define SH_AI_FOCI_AIM              0   /* aiFoci_t */
#define SH_AI_AWARE_UNAWARE         0   /* AIAWARE_ */
#define SH_AI_AWARE_LOST            1
#define SH_AI_AWARE_SUSPECTED       2
#define SH_AI_AWARE_CONFIRMED       3

/* aiFireMode_t (the fire control's modes) and aiIdleTurn_t (the turn in place
 * an idle order asks for), 13954591; tests/script_ai_sigs_test.c compares both
 * name tables with the executables' own enum tables. */
#define SH_AI_FIREMODE_HOLDFIRE              0
#define SH_AI_FIREMODE_WEAPON_EXPLICIT_RELEASE 1
#define SH_AI_FIREMODE_WEAPON                2
#define SH_AI_FIREMODE_FIREWHENREADY         3
#define SH_AI_FIREMODE_SPRAYWHENREADY        4
#define SH_AI_FIREMODE_MAX                   5   /* the end of the list, not a mode */
#define SH_AI_IDLETURN_RELAXED               0
#define SH_AI_IDLETURN_INCOVER               2
#define SH_AI_IDLETURN_MAX                   3
extern const char *const SH_AI_FIREMODE_NAMES[SH_AI_FIREMODE_MAX + 1];
extern const char *const SH_AI_IDLETURN_NAMES[SH_AI_IDLETURN_MAX + 1];
/* The value of a mode or turn named by its whole enum name or without its
 * prefix ("AIFIREMODE_FIREWHENREADY" or "FIREWHENREADY", any case); -1 for a
 * word that is none (SH_AI_FIREMODE_MAX is not a mode: -1 too). Pure. */
int sh_ai_fire_mode_parse(const char *word);
int sh_ai_idle_turn_parse(const char *word);
/* The enum name of a value, NULL past the table. Pure. */
const char *sh_ai_fire_mode_name(int mode);
const char *sh_ai_idle_turn_name(int turn);
/* The muzzle test's three weights (CheckPullTrigger asks 0, the burst's
 * keep-firing check 1): "pull", "firing" or "plain"; -1 for another word. */
int sh_ai_muzzle_mode_parse(const char *word);

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

/* Move completion from the move FSM's status word (moveFSM +0xD20: 0 idle or arrived, 1..4 under way, 5 and above
 * failed). The status is still 0 on the frame a command is given, so a move is done when 0 returns after a nonzero
 * status, failed on a failure status, or done after SH_AI_MOVE_SETTLE frames of 0 (already there, or dropped). */
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
/* What a forced via needs this frame, from the web's SH_SCRIPT_ANIM_* flags, whether its destination was asked, and the
 * frames since issue. The destination is asked once the forced node plays, since a forced clip holds its last frame
 * until then. Pure. */
enum { SH_AI_FORCE_VIA_WAIT, SH_AI_FORCE_VIA_ASK_DEST, SH_AI_FORCE_VIA_DONE };
int sh_ai_force_via_next(unsigned flags, int asked, uint32_t frames_since_issue, uint32_t give_up_frames);

/* The park: a commanded demon waits in the engine's ExecuteDoomscript alert-cycle state, because the IsRunningScript(ai)
 * detour answers true for it. Each command also posts setScriptAbort 0 0 and setScriptAbortFlags 0 0, or a demon in
 * combat leaves its point; ai:release() hands it back and the next command parks it again.
 * 1 while a parked demon stays the script's: death and a requested or playing pain reaction are the engine's. Pure. */
int sh_ai_park_holds(int alive, int pain_requested, int pain_reaction);
/* One parked demon (the engine side's table; FSM threads read it without a lock). drives: the slot is this entity's
 * (spawn id matches id_now) and not released, so IsRunningScript answers what the park holds; else the engine's answer.
 * release marks it released and ends a pain the park had opened for; take clears the mark at every park. */
typedef struct sh_ai_park_slot {
    void *volatile ai;
    volatile long  spawn_id;
    volatile long  pain_until;   /* game ms until which the park is open for a pain or death request; 0 none */
    volatile long  fsm_busy;     /* the request detour is inside this demon's request */
    volatile long  released;     /* ai:release(): the engine's own behaviour until the next park */
} sh_ai_park_slot;
int  sh_ai_park_drives(const sh_ai_park_slot *s, uint32_t id_now);
void sh_ai_park_release(sh_ai_park_slot *s);
void sh_ai_park_take(sh_ai_park_slot *s);
/* The alert cycle transition codes a parked demon's park opens for: TRANSCODE_AC_IN_PAIN (232), _IN_PAIN_LIGHT
 * (233), _IN_STAGGER (237), _IN_STAGGER_ONESHOT (238) and _IS_DYING (242). */
int      sh_ai_fsm_code_yields(int code);
/* A refused pain is asked again while the demon lives, no reaction plays and the time is not up (1), else dropped (0). */
int      sh_ai_pain_take_next(int alive, int playing, int now_ms, int until_ms);
/* The index of the state named `want` among an FSM's n state names (a NULL name is skipped), -1 when none. */
int      sh_ai_fsm_state_find(const char *const *names, int n, const char *want);
/* Reflected offsets used to inspect the alert branch and the exact child its manager selects. */
typedef struct { uint32_t data, name, manager, selector, linked; } sh_ai_path_fields;
#define SH_AI_FSM_FIND_CHILD_SLOT 0x68u
int sh_ai_fsm_has_native_path(const uint8_t *alert, const sh_ai_path_fields *fields);
/* The move record an FSM's take-transition virtual is given to enter `target` at once, as the engine's own entrance
 * move builds it (SH_AI_SIG_FSM_MOVE): zeroed, the target state, idTransitionCode's vtable and code 5. */
#define SH_AI_FSM_MOVE_SIZE       0x48u
#define SH_AI_FSM_MOVE_TARGET     0x10u
#define SH_AI_FSM_MOVE_CODE_VT    0x20u
#define SH_AI_FSM_MOVE_CODE       0x28u
#define SH_AI_FSM_MOVE_IMMEDIATE  5
void     sh_ai_fsm_move_init(unsigned char rec[SH_AI_FSM_MOVE_SIZE], const void *code_vtable, const void *target);

void sh_ai_move_issue(sh_ai_move *m, uint32_t frame);
/* Feed the status read at `frame`; returns the move's state. */
int  sh_ai_move_step(sh_ai_move *m, int status, uint32_t frame);

/* The awareness word's name: "unaware", "lost", "suspected", "confirmed";
 * NULL for any other value. */
const char *sh_ai_awareness_name(int level);

/* A demon's social relations (both renderers): the senses (ai +SH_AI_OFF_PERCEPTION) rebuild one row per perceived
 * entity every AI think (UpdateRelations VK 0x1404AA490 / GL 0x1404A8C10), sorted by emotion, most hostile first.
 * Rows at +0x28, count at +0x30, 0x28 bytes a row: word +0 (0x1FFFFFE when the row has no entity of its own and
 * GetRelations' 10,000-unit range does not limit it, VK 0x1404AAA30), respect +4, emotion +8, idEntityPtr +0x10.
 * The engine's pick reads the same rows (VK 0x1404AAD10 / GL 0x1404A9490). */
#define SH_AI_REL_ROWS            0x28u
#define SH_AI_REL_COUNT           0x30u
#define SH_AI_REL_SIZE            0x28u
#define SH_AI_REL_WORD            0x00u
#define SH_AI_REL_RESPECT         0x04u
#define SH_AI_REL_EMOTION         0x08u
#define SH_AI_REL_ENTITY          0x10u
#define SH_AI_REL_NO_ENTITY       0x1FFFFFEu
/* where the two functions show them (tests/script_ai_sigs_test.c) */
#define SH_AI_UPDREL_ROWS_DISP    0x27u     /* UpdateRelations: lea rbx,[rcx+0x28] */
#define SH_AI_UPDREL_COUNT_DISP   0x2Au     /* mov dword [rbx+8],0: the count, rows + 8 */
#define SH_AI_UPDREL_FACTION_IMM  0x3Cu     /* add rdx,0x4EC0: the entity's faction */
#define SH_AI_BEST_EMOTION_DISP   0x66u     /* the pick: mov eax,[rbx+rcx+8] */
#define SH_AI_BEST_ENTITY_IMM     0x6Fu     /* add rcx,0x10 */
#define SH_AI_BEST_STRIDE_IMM     0xDCu     /* add rbx,0x28 */
#define SH_AI_BEST_START_DISP     0x44u     /* lea r15d,[rbp+0xE]: EMOTION_MAX */
#define SH_AI_REL_MAX             256       /* rows read at most (the engine's table holds 64 before it says so) */
typedef struct sh_ai_relation {
    void *entity;
    int   respect;      /* socialRespect_t: RESPECT_MIN 0 .. RESPECT_MAX 6 */
    int   emotion;      /* socialEmotion_t: EMOTION_MIN 0 .. EMOTION_MAX 14 */
    int   unlimited;    /* 1: the row has no entity of its own (no range limit) */
} sh_ai_relation;
/* The respect words ai:relations answers ("min", "peon", "minion", "peer", "leader", "worship", "max"); NULL outside. */
const char *sh_ai_respect_name(int respect);
/* An actor's off-limits flag (both renderers): idActor volatile offLimits, a byte its virtual +SH_AI_VSLOT_OFF_LIMITS
 * answers (idAI2's and idPlayer's VK 0x140427550 / GL 0x140426600: movzx eax, byte [rcx+0x9408]; ret) and +0x588 sets.
 * Shared_SocialRelationAvailable's filterOutOffLimits drops an actor that has it (VK 0x1405F4930). The call reads the
 * byte through that getter after checking it is that one instruction with this displacement. */
#define SH_AI_VSLOT_OFF_LIMITS    0x580u
#define SH_AI_OFF_OFF_LIMITS      0x9408u
/* 1 when `code` is the off-limits getter: movzx eax, byte [rcx + SH_AI_OFF_OFF_LIMITS]; ret. */
int sh_ai_off_limits_getter(const uint8_t code[8]);
/* The emotion words ("min", "terrified" .. "neutral" .. "partner", "max"); NULL outside. */
const char *sh_ai_emotion_name(int emotion);

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

/* animation_fits's test words (the TF_* words of an AnimationValid condition,
 * and the attack graph's VALIDATE_ANIM_DELTA), as bits. */
#define SH_AI_TF_NAV_DIRECT        0x001u   /* the demon has a navigation path to where the clip ends */
#define SH_AI_TF_NAV_ENDPOINT      0x002u   /* the clip ends on the ground */
#define SH_AI_TF_CLAMP_TO_GROUND   0x004u   /* the end point is taken down onto the ground */
#define SH_AI_TF_BOUNDS            0x008u   /* nothing in the world is in the way of the motion */
#define SH_AI_TF_EYE_VISIBILITY    0x010u   /* from the end point the demon's eyes see its enemy */
#define SH_AI_TF_MUZZLE_VISIBILITY 0x020u   /* the same sight from weapon height */
#define SH_AI_TF_TO_ENEMY_FORCE    0x040u   /* the motion does not carry the demon toward its enemy */
#define SH_AI_TF_AVOID_ENEMY_FOV   0x080u   /* the end point is out of the centre of the enemy's view */
/* The attack graph's VALIDATE_ANIM_DELTA: DOOM Eternal's anim delta check (FUN_1409007D0) is a navigation trace from the
 * AI to where the clip ends, which no player or demon blocks and which a clip with no root motion passes. The port
 * composes it as: the end stands on the world's floor and the line to it at knee and chest height is clear of the world. */
#define SH_AI_TF_NAV_LINE          0x100u
#define SH_AI_TF_ANIM_DELTA        SH_AI_TF_NAV_LINE                          /* VALIDATE_ANIM_DELTA */
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

/* spline_clear's curve from `from` to `to`: the two ends and one point per inner knot, each moved value x horizontal
 * length across the line by the roll. Returns the points written (2..cap; table->n + 2 holds all). Pure. */
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

/* Wound damage counts. The game opens a gore wound by damage GROUP: idGoreComponent::DamageActivatesWound
 * (SH_AI_SIG_DAMAGE_WOUND) is asked per queued hit record and wound, with the health the whole group has lost, and
 * passes it when that loss reaches the wound's threshold and the hit lands in its clipping spheres. DOOM Eternal counts
 * the damage of each hit inside a wound's own spheres instead; ai:wound_damage_counts(true) gives one demon that rule
 * by calling the game's function with the lost health raised (SH_AI_DAW_LOST_RAISED), so every other gate is the game's,
 * and adding a passed record's damage to the wound's counter. */

/* The threshold as the game computes it (VK 0x1406E8A80, GL 0x1406E7EE0): relativeDamageRequiredToGore times the
 * owner's max health when that is not negative, else totalDamageRequiredToGore. Pure. */
float sh_wound_count_threshold(float total, float relative, float max_health);
/* One hit on one wound: gates_passed is the game's answer with the lost health raised. A refused hit never counts; a
 * passed one adds its damage (negative or non-finite adds nothing) and answers 1 once the sum reaches the threshold, or
 * always for a threshold not above 0. Pure. */
int   sh_wound_count_step(float threshold, float *sum, float damage, int gates_passed);
/* The per-demon counters: one record per demon that asked (keyed by demon and spawn id), one counter per wound decl, up
 * to SH_WOUND_COUNT_WOUNDS. A record is dropped when the handle is released or the call is turned off, the table at a
 * map's start; lock-protected because the gore think and the script call may be on different threads. */
#define SH_WOUND_COUNT_WOUNDS 64
void  sh_wound_count_reset(void);
int   sh_wound_count_set(void *ai, uint32_t spawn_id, int on);    /* 1 done, 0 the table is full */
int   sh_wound_count_on(void *ai, uint32_t spawn_id);             /* 1 the demon asked */
/* The rule for one call of the game's function on a demon that asked; record and mark (damage and position bits) name
 * the hit, so one hit offered twice for a wound counts once. 1 pass, 0 refuse, -1 not counted (the caller answers the
 * game's rule). */
int   sh_wound_count_hit(void *ai, uint32_t spawn_id, const void *wound, const void *record, uint32_t mark,
                         float threshold, float damage, int gates_passed);
float sh_wound_count_sum(void *ai, uint32_t spawn_id, const void *wound);   /* the counter, 0 when none */
void  sh_wound_count_gone(uint32_t spawn_id);
uint32_t sh_wound_count_mark(const void *record);   /* FNV-1a of the record's first 16 bytes */

/* ------------------------------------------------------------ engine seam */

/* The muzzle test's weights (the ray against a sphere of the focus radius:
 * the radius is halved at the pull, 1.5 times while the burst runs). */
#define SH_AI_MUZZLE_PULL   0
#define SH_AI_MUZZLE_FIRING 1
#define SH_AI_MUZZLE_PLAIN  2

/* What the engine holds about the aim focus (idAI2.aiVolatile.focus): the
 * muzzle test reads valid, point and radius and nothing else. */
typedef struct sh_ai_focus_state {
    int   valid;            /* foci[AIM].finalFocusPointIsValid */
    float point[3];         /* foci[AIM].finalFocusPoint: the sphere's centre */
    float radius;           /* focus.aimFocusTargetRadius */
    int   precision;        /* focus.muzzleAimPrecision: 0 NORMAL, 1 PRECISE, 2 MAX */
    int   require_muzzle;   /* fireControl.requireMuzzleOnTarget, -1 unreadable */
    int   tracking;         /* focus.enableHeadTracking */
    int   body_rotation;    /* focus.enableBodyRotation */
} sh_ai_focus_state;

/* Where the demon's animFSM stands with a step animation of its own (a turn in place, a step or an arrival). The engine
 * ends such a stage when its animWeb command ends; a script's own animation order bypasses that command, so the stage
 * would wait until the engine restarts the animFSM after a second. */
typedef struct sh_ai_step_state {
    int moving;             /* the animFSM is in its move state */
    int stage;              /* its move stage (SH_AI_STAGE_*) */
    int waiting;            /* the command context's active command has not ended */
    int idle_turn;          /* the turn type of a standing idle order (ssaction_Idle), -1 with none */
} sh_ai_step_state;
#define SH_AI_STAGE_MOVE_STEP     2    /* STAGE_MOVE_STEP: a step or a turn in place */
#define SH_AI_STAGE_ARRIVAL       7    /* STAGE_ARRIVAL */
#define SH_AI_STAGE_ARRIVAL_SOFT  8    /* STAGE_ARRIVAL_SOFT */
/* 1 when the state says a step animation is in flight. */
int sh_ai_step_in_flight(const sh_ai_step_state *s);

typedef struct sh_script_ai_engine {
    void    *(*entity)(sh_script_handle h);          /* the live entity, NULL when gone */
    sh_script_handle (*handle_of)(void *entity);      /* a handle a script can hold, 0 for none */
    int      (*is_ai)(void *entity);                  /* 1 an idAI2 (or a subclass) */
    int      (*origin)(void *entity, float out[3]);   /* physics origin */
    int      (*yaw)(void *entity, float *deg);        /* facing, degrees */
    /* Run catalogue event `ev` on `entity`: 1 when it ran, with its int, bool or float return value in *ret when ret
     * is not NULL; 0 when unavailable or faulted. */
    int      (*event)(void *entity, int ev, const sh_ai_arg *args, int n, double *ret);
    int      (*park)(void *ai);                       /* 1 parked (this call or earlier) */
    void    *(*enemy)(void *ai);                      /* the current enemy entity, or NULL */
    int      (*awareness)(void *ai, void *target);    /* AIAWARE_ level, -1 unreadable */
    int      (*move_status)(void *ai);                /* move FSM status word, -1 unreadable */
    /* The weapon calls act on the weapon in one hand (SH_AI_HAND_*; 0 the
     * demon's weapon hand, below). */
    int      (*trigger_state)(void *ai, int hand);    /* 1 trigger held, 0 not, -1 no weapon there */
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
    /* The root motion of animation `index` of animWeb node "subweb/state" in the demon's frame (x forward, y left,
     * z up). 1 read, 0 index past the last (*count gets the node's animations), -1 no such node. */
    int      (*anim_delta)(void *ai, const char *node, int index, float out[3], int *count);
    /* Engine answers (the engine side composes the queries from the
     * primitives with sh_ai_*_run below; a test engine answers directly). */
    int      (*set_recoil)(void *ai, float alpha);
    int      (*weapon_ready)(void *ai, int hand);     /* 1 ready, 0 cooling down, -1 no weapon there */
    int      (*aim_point)(void *ai, const float pos[3]);
    /* 1 valid (*index: the animation), 0 not, 2 still being asked, -1 no such node; seed (side < 0): 0 tries
     * the animations in order, another value in an order shuffled by it */
    int      (*anim_valid)(void *ai, const char *node, unsigned tests, int side, unsigned seed, int *index);
    /* 0 found (*n points), 1 still searching, 2 no path, -1 unavailable */
    /* the computed path's points, whole: *out malloc'd (the caller frees it), *n its count */
    int      (*path_points)(void *ai, const float goal[3], int key, float (**out)[3], int *n, float *length);
    /* 0 found, 1 still searching, 2 none, -1 unavailable */
    int      (*firing_position)(void *ai, const sh_ai_position_query *q, float out[3]);
    /* 1 clear (*n points), 0 blocked, 2 still searching, -1 unavailable */
    int      (*spline_clear)(void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap, int *n);
    int      (*pain_string)(void *ai, const char *text);        /* NULL: the engine's own again */
    int      (*block_pain)(void *ai, int reaction, int ms);     /* refuse `reaction` for ms */
    /* The hand the game fires the demon's weapon from: its aibehavior decl's
     * weaponSlot (an equipSlot_t, SH_AI_HAND_* for the hands), -1 unreadable. */
    int      (*weapon_hand)(void *ai);
    /* Inventory item `index` (0 first): 1 with its decl's whole name in decl
     * and the equipSlot_t its decl puts it in; 0 past the last item; -1
     * unreadable. */
    int      (*item)(void *ai, int index, char *decl, size_t cap, int *hand);
    /* The engine's fire control: the fire mode the script events write
     * (aiVolatile.fireControl.fireMode_state) and the one the animation
     * events write (fireMode_anim). 1 read, -1 unreadable. */
    int      (*fire_modes)(void *ai, int *state, int *anim);
    /* The aim focus and the muzzle test's inputs (reflected paths in
     * sh_ai_focus_state). 1 read, -1 unreadable. */
    int      (*focus_state)(void *ai, sh_ai_focus_state *out);
    /* The engine's own muzzle test (the one CheckPullTrigger runs) for the
     * weapon in `hand`, with weights mode (SH_AI_MUZZLE_*): 1 on target, 0
     * not, -1 unavailable (no such weapon or the function is missing). */
    int      (*muzzle_test)(void *ai, int hand, int mode);
    /* The engine's ClearMove on the demon's animFSM (the call the scripted
     * actions start with): 1 done, -1 unavailable. */
    int      (*clear_move)(void *ai);
    /* Whether the demon's aibehavior decl has a turnParms entry for idle
     * turn type `turn` (the entry the engine's idle turn reads): 1 yes, 0 no,
     * -1 unreadable. */
    int      (*idle_turn_entry)(void *ai, int turn);
    /* A laser of the demon's laser targeter component on model tag `tag`, drawn with ribbon decl `ribbon` (whole name;
     * NULL: the component decl's laserPinpoint_ribbonDecl), off until switched on. 1 added, 0 no laser targeter, -2 no
     * such ribbon decl loaded, -1 unavailable. */
    int      (*laser_add)(void *ai, const char *tag, const char *ribbon);
    /* Take it away again. 1 done, 0 no laser targeter, -1 unavailable. */
    int      (*laser_remove)(void *ai, const char *tag);
    /* For the demon's rear feet (the game's foot indices 2 and 3) play footstep table `rear` wherever the game picks
     * `front` (whole projectileImpactEffect decl names). 1 set, -2 a decl is not loaded, -1 unavailable. */
    int      (*rear_footsteps)(void *ai, const char *front, const char *rear);
    /* The animFSM's stage and the command it waits on (sh_ai_step_state).
     * 1 read, -1 unreadable. */
    int      (*step_state)(void *ai, sh_ai_step_state *out);
    /* End the animWeb command the demon's command context waits on, as the engine does when another command takes its
     * place; the animFSM stage waiting on it then ends at its next update. 1 done, 0 none waiting, -1 unavailable. */
    int      (*step_interrupt)(void *ai);
    /* Give a generic-type demon the gore component the game's spawn gives every other type (the game's init with its
     * entityDef's gore graph). 1 done, 0 nothing to do (other type, no graph, done, gore off), -1 unavailable. */
    int      (*gore_init)(void *ai);
    /* Hide the n meshes named in `meshes` of the demon's model whenever the game shows its gore wound `wound` (whole
     * gorewounds decl name), as the armour pop-off hides a damage group's hideMeshes. 1 set, -2 wound decl not loaded,
     * -3 a mesh name not in the model (nothing set), -1 unavailable. */
    int      (*wound_hides)(void *ai, const char *wound, const char *const *meshes, int n);
    /* The decl name of the inventory item in `hand` (the engine's weapon in that equip slot): 1 and the name, 0 when
     * the hand is empty, -1 when it cannot be read. */
    int      (*held)(void *ai, int hand, char *decl, size_t cap);
    /* Make the game's leg rig take each leg's target and pole from the legs as the demon's clips pose them, every
     * frame, before it follows the ground. 1 set, 0 no two-bone leg rig on the model, -1 unavailable. */
    int      (*posed_legs)(void *ai);
    /* Make the demon's fk effectors take their reference pose from the poseReferenceAnim their fkGraph names, as DOOM
     * Eternal's do, not from the web's current leaf (sh_ai_fk_reference below). 1 set, -1 unavailable. */
    int      (*fk_reference_pose)(void *ai);
    /* Set the interrupt blend mode of the demon's anim web (SH_AI_INTR_BLEND_) for the order just given: the mode
     * the next transition takes when it meets a running blend (sh_ai_order_interrupt). 1 set, -1 unavailable. */
    int      (*interrupt_blend)(void *ai, int mode);
    /* Make sure the game's handling of a gore breakable's hit runs through
     * the product (sh_ai_destructible below). 1 ready, -1 unavailable. */
    int      (*destructible)(void);
    /* Select the entry `name` of the demon's behaviour decl's weaponUseBehaviorList as its weapon-use behaviour (recoil
     * clip and weights, trigger-hold times); the game selects "default" at spawn only. 1 selected, 0 no such entry
     * (nothing changed), -1 unavailable. */
    int      (*weapon_use)(void *ai, const char *name);
    /* on 1: a gore wound of the demon opens on the damage that landed inside its own hit spheres
     * (sh_wound_count_step); on 0: by the game's own rule. 1 set, -1 unavailable. */
    int      (*wound_damage_counts)(void *ai, int on);
    /* The SnapMap order waiting in the demon's temp memory (sh_ai_order_find
     * on its slots, AI +0x25B30 on both renderers): 1 with it, 0 none, -1
     * unreadable. It stays there until the caller removes it. */
    int      (*snapmap_order)(void *ai, sh_ai_order *out);
    /* Hand the parked demon back to its own behaviour: IsRunningScript gives the engine's answer again, so its alert
     * cycle leaves ExecuteDoomscript by the NotExecutingScript link until the next command parks it. 1 done (also for a
     * demon not parked), -1 unavailable. */
    int      (*release)(void *ai);
    /* Set the variable `name` of the demon's body anim web to `value` by the engine's set-by-name (the anim web's
     * variables, below): a bound member or the web's scalar memory. 1 set, 0 no such variable, -1 unavailable. */
    int      (*anim_variable)(void *ai, const char *name, float value);
    /* The demon's social relations (sh_ai_relation above), in the engine's
     * order, rows whose entity is gone left out: their count (at most cap),
     * -1 unreadable. */
    int      (*relations)(void *ai, sh_ai_relation *out, int cap);
    /* The actor's off-limits flag (sh_ai_off_limits_getter above): 1 set, 0 not, -1 not an actor or unreadable. */
    int      (*off_limits)(void *actor);
    /* The world alone between two points, as a navigation trace sees it: a trace from `from` to `to` passing the
     * demon and every player and demon it meets. 1 clear, 0 blocked by the world, -1 unreadable. */
    int      (*world_clear)(void *ai, const float from[3], const float to[3]);
    /* The floor under pos, the same way (players and demons, the asking demon included, are passed through). 1 found,
     * with the point in out. */
    int      (*world_ground)(void *ai, const float pos[3], float out[3]);
    /* Ask the engine for pain reaction `reaction` (painType_t) as its pain start does for a frame's damage: its gates,
     * the pain graph's clip choice, the pending request and the alert cycle's FSM request, with `damage` as the
     * intensity. 1 the alert cycle took it, 0 refused (why says by what), -1 unavailable. */
    int      (*request_pain)(void *ai, int reaction, float damage, char *why, size_t cap);
    /* Register one owner FX condition to start when the native wound actually opens. */
    int (*wound_effect)(void *ent, const char *wound, const char *condition);
    /* Verify the path state before handoff. Then 1 while the native body is busy, 0 when free, -1 unavailable. */
    int (*native_body_busy)(void *ai, int check_path);
    int (*reset_motion_correction)(void *ai);
    int (*on_ground)(void *ai);  /* 1 grounded, 0 airborne, -1 when a different physics body is active */
} sh_script_ai_engine;

int sh_ai_set_wound_effect(void *owner, uint32_t id, void *wound, int condition);
int sh_ai_wound_effect_at(void *owner, uint32_t id, void *wound, int show,
                          const void *call_return, const void *accepted_return);
void sh_ai_remove_wound_effects(uint32_t id);

/* The SnapMap order of a temp memory block (pure): keys[i] is the slot's key text (an interned GAME_STRING; NULL for an
 * empty slot), values[i] its 32-bit value. The first order key found wins: an entity order's value is the target's
 * spawn id; a position order spans three slots of one key (x, y, z floats). 1 with *out, 0 none. */
int sh_ai_order_find(const char *const *keys, const uint32_t *values, int n, sh_ai_order *out);

/* Posed legs. The game poses a leg by a rig that solves thigh and shin from the hip to a target joint and swivels the
 * knee by a pole joint; a demon whose clips rotate the leg joints themselves has its pose in those joints, and this
 * turns that pose into the rig's inputs. Rotations are x y z w; a joint's model transform is its parent's times its own. */
typedef struct {
    const int16_t *parents;   /* parent joint per joint, negative for a root */
    int            joints;
    uint8_t       *lt;        /* local translations (3 floats), lt_stride bytes apart */
    int            lt_stride;
    uint8_t       *lq;        /* local rotations (4 floats), lq_stride bytes apart */
    int            lq_stride;
} sh_ai_leg_pose;
typedef struct {
    int parent;               /* the thigh's parent: the rig reads its model rotation */
    int target, pole;         /* the two joints the rig reads */
    int hip, thigh, foot;     /* where the thigh starts, the thigh, and the leg's end joint */
} sh_ai_leg_joints;
/* The rig's own solve with no swivel: the thigh's model rotation for a hip
 * place, the parent's model rotation and a target. 1 when it answered. */
typedef int (*sh_ai_leg_solve_fn)(void *leg, const float hip[3], const float parent_q[4], const float target[3],
                                  const float target_q[4], float thigh_q[4]);
/* Write the target joint's local translation and rotation and the pole joint's local rotation (about x by the angle that
 * turns the solved thigh to the posed one) into the pose. 1 written, 0 not (joint out of range, leg of no length, the
 * solve refused). */
int sh_ai_posed_leg(const sh_ai_leg_pose *pose, const sh_ai_leg_joints *j, sh_ai_leg_solve_fn solve, void *leg);

/* The gore init site (SH_AI_SIG_GORE_INIT_SITE): the cvar test, the two
 * component offsets and the call. */
#define SH_AI_GORE_SITE_CVAR      9u      /* cmp dword [rip+d32], 0 (7 bytes): g_EnableGore's integer */
#define SH_AI_GORE_SITE_DAMAGE    37u     /* lea r8, [ai+d32]: the entity damage component */
#define SH_AI_GORE_SITE_COMP      44u     /* lea rcx, [ai+d32]: the gore component */
#define SH_AI_GORE_SITE_CALL      72u     /* call init */
#define SH_AI_GORE_GRAPH          0x630u  /* component: the gore graph decl */
#define SH_AI_GORE_OWNER          0x610u  /* component: its owner, NULL until the init */
#define SH_AI_GORE_NODE_WOUND     0x28u   /* idDeclGoreNode: areaWoundDecl, the wound a shown row opens */
/* ShowWound's window: the first argument's home, push rbx, push rdi, sub
 * rsp (whole, position-independent). */
#define SH_AI_WOUND_STOLEN 14
static const unsigned char k_ai_wound_prologue[SH_AI_WOUND_STOLEN] = {
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00
};
/* DamageActivatesWound (SH_AI_SIG_DAMAGE_WOUND; VK 0x1408FC8E0, GL 0x1408FB910, the same code on both): (component,
 * wound decl, record, damage group decl, group index, lost health, max health) -> bool in al. rcx the gore component
 * (owner at +0x610, SH_AI_GORE_OWNER); rdx the wound decl (its +0x218 is the frame gate); r8 the goreFrameDamage_t (one
 * 0x58-byte slot of the component's 8-slot ring: damage at +0, position at +4); r9 the group decl (NULL-tested only);
 * the stack's 5th the group index (at most 31), 6th the group's lost health (int at +0x1E0), 7th the owner's max health
 * (float at +0x1CE). tests/script_ai_sigs_test.c checks every byte named here on both executables. */
/* The window: push rbp, rbx, rsi, rdi, r12, r14, r15; lea rbp,[rsp-0xF] (whole, position-independent). */
#define SH_AI_DAW_STOLEN 16
static const unsigned char k_ai_daw_prologue[SH_AI_DAW_STOLEN] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF1
};
#define SH_AI_DAW_MAXH_READ       0x1CEu   /* movss xmm1,[rbp+0x7F]; mov rcx,rsi: the max health and the wound */
#define SH_AI_DAW_THRESHOLD_CALL  0x1D6u   /* call threshold(wound, max health) */
#define SH_AI_DAW_LOST_READ       0x1E0u   /* movd xmm1,[rbp+0x77]; cvtdq2ps; comiss; ja refuse */
#define SH_AI_DAW_LOST_RAISED     0x7FFFFFFF
#define SH_AI_WOUND_TOTAL_REQUIRED    0x210u  /* gorewounds decl: damageRequirements.totalDamageRequiredToGore */
#define SH_AI_WOUND_RELATIVE_REQUIRED 0x214u  /*                  damageRequirements.relativeDamageRequiredToGore */
#define SH_AI_GFD_DAMAGE          0x00u    /* goreFrameDamage_t: totalDamage */
#define SH_AI_GFD_SIZE            0x58u
/* The mesh hide site (SH_AI_SIG_MESH_HIDE_SITE): the two calls, and where
 * an entity keeps its model. */
#define SH_AI_MESH_SITE_INDEX     11u     /* call index-by-name (model, name) */
#define SH_AI_MESH_SITE_HIDE      107u    /* call hide (model, index) */
#define SH_AI_OFF_MODEL           0x3DB0u /* entity: its md6 model */
#define SH_AI_BEHAVIOR_MONSTER    0x120u  /* behaviour decl: aiMonsterType_t */
#define SH_AI_MONSTER_GENERIC     1

/* A mod's entity has spawned (the runtime, once per entity, before the
 * mod's spawn handler): a demon gets what the game's spawn left out. */
void sh_script_ai_spawned(void *ent);

/* equipSlot_t: the two hands a demon holds a weapon in. */
#define SH_AI_HAND_LEFT  1
#define SH_AI_HAND_RIGHT 2

/* The compositions the engine side answers queries with, over an engine's
 * primitives (pure: the tests run them over a fake). key_base names the
 * path requests in the engine's path cache. */
int sh_ai_firing_run(const sh_script_ai_engine *e, void *ai, const sh_ai_position_query *q, int key_base,
                     float out[3]);
int sh_ai_spline_run(const sh_script_ai_engine *e, void *ai, const sh_ai_spline_query *q, float (*out)[3], int cap,
                     int *n);
int sh_ai_anim_valid_run(const sh_script_ai_engine *e, void *ai, const char *node, unsigned tests, int side,
                         unsigned seed, int key_base, int *index);
/* The path-cache key of animation `index` of `node` in an animation_fits query: each node takes its own block of
 * 64 keys inside the demon's span, so two nodes asked in turn do not restart each other's path queries. */
int sh_ai_anim_key(int key_base, const char *node, int index);
/* The engine's path cache is keyed by an int: each demon's record gets
 * SH_AI_KEY_SPAN keys for its queries (a query with more candidates than that
 * shares keys in turn; the engine then answers "searching" a little longer). */
#define SH_AI_KEY_SPAN 0x4000

/* The engine the calls use (the engine side installs the real one; tests a
 * fake). NULL makes every call refuse with "not available". */
void sh_script_ai_set_engine(const sh_script_ai_engine *engine);

/* Add the AI calls to every script state opened after this; returns the number added. distance, animation_state and
 * force_animation are the entity calls' (script_api_entity.c); on a demon the last two come here. */
int sh_script_ai_register(void);

/* Park the mod's own demon `ai` (handle h) and ask its body animWeb for `dest`: ssaction_ChangeAnimState, ..Via through
 * `via`, or with force ssaction_ForceAnimState blending blend_ms. 1 when the engine ran it; 0 when the demon cannot be
 * parked or the AI calls are unavailable. Game thread, inside the script tick. */
int sh_script_ai_anim(void *ai, sh_script_handle h, const char *dest, const char *via, int force, int blend_ms);

/* The kinds of animation order (sh_script_ai_anim's `force`): CHANGE is ssaction_ChangeAnimState(Via), FORCE is
 * ssaction_ForceAnimState, FORCE_VIA forces ai:force_animation_via's via node, VIA_DEST is the change to its destination
 * asked when the via plays. */
enum { SH_AI_ORDER_CHANGE = 0, SH_AI_ORDER_FORCE = 1, SH_AI_ORDER_FORCE_VIA = 2, SH_AI_ORDER_VIA_DEST = 3 };

/* Interrupts. The anim web's update (VK 0x14170B890, DOOM Eternal 0x140495800) meets a new transition during a
 * running blend: a node with AW_CF_USE_MULTIPLE_INTERRUPT (customFlags bit 25) nests it; otherwise interruptBlend
 * decides (NO waits, YES ends the running blend at once, MULTI nests) and each transition resets it to NO. The game's
 * script events give a change NO (VK 0x1405DC680) and a forced node YES (VK 0x1405DCAC0); DOOM Eternal's AI command
 * (constructor 0x1408F2D40) gives a change or a forced via MULTI. sh_ai_order_interrupt returns that mode of an order
 * kind, or -1 to leave the game's (a forced node: YES in both; the via's destination). */
#define SH_AI_INTR_BLEND_NO        0   /* interruptBlend_t */
#define SH_AI_INTR_BLEND_YES       1
#define SH_AI_INTR_BLEND_MULTI     2
#define SH_AI_OFF_WEB_ANIMATOR     0x222B8u /* idAI2: the body anim web's idAnimator_AnimWeb */
#define SH_AI_AW_INTERRUPT_BLEND   0x570u   /* idAnimator_AnimWeb: interruptBlend (interruptPath at +0x56C) */
#define SH_AI_FORCE_STATE_SET_YES  0x412u   /* ForceState: mov dword [r13+0x570], 1 */
#define SH_AI_UPDATE_FLAG_TEST     0x564u   /* update: mov eax,[rax+0x10]; shr eax,0x19; test al,1 */
#define SH_AI_UPDATE_READ_MODE     0x575u   /* update: mov eax,[r12+0x570] */
#define SH_AI_UPDATE_RESET_MODE    0xAB6u   /* update: mov [r12+0x570], r13d (zero) at a transition */
int sh_ai_order_interrupt(int kind);

/* The anim web's variables (both renderers; SH_AI_SIG_WEB_SCALAR_SET, VK 0x14170A950 / GL 0x14170B3C0). An
 * idAnimator_AnimWeb holds one row per variable (rows at +0x580, count +0x588, 0x18 bytes: name atom +0, short +10,
 * pointer +0x10); a variable bound to an owner member has the short below 0 and the member at +0x10, any other an index
 * into the web's float memory (pointer at +0x598). The engine's set-by-name finds the row by atom and stores the float
 * (VK 0x1417574D0 / GL 0x141758620), as idAISnippet_Shared_SetScalarToProjectileIndex does (VK 0x1404B02A0). 1 when a
 * row of that name was found. */
#define SH_AI_WEB_ROWS             0x580u
#define SH_AI_WEB_ROW_COUNT        0x588u
#define SH_AI_WEB_SCALAR_MEMORY    0x598u
#define SH_AI_WEB_ROW_SIZE         0x18u
#define SH_AI_WEB_SCALAR_STORE     0x52u    /* the set: call store(row, &memory, value) */

/* Park the mod's own demon `ai` (handle h) without a command: 1 when it is
 * parked. ai:fire uses it, so a demon a script fires from stops its own
 * behaviour like one it moves. Game thread, inside the script tick. */
int sh_script_ai_park(void *ai, sh_script_handle h);

/* Destructible impulses use model-space or hit-space directions and signed speeds in units/s and rad/s. */
enum { SH_AI_PRESET_CUSTOM = 0, SH_AI_PRESET_RANDOM = 1, SH_AI_PRESET_CONE = 2, SH_AI_PRESET_SPHERICAL = 3,
       SH_AI_PRESET_ANGEL_WINGS = 4, SH_AI_PRESET_THRU_SQUARE = 5 };
typedef struct {
    int   preset;              /* SH_AI_PRESET_ (DOOM Eternal's impulsePreset_t order) */
    float lin_min, lin_max;    /* the preset's [minForce, maxForce] as a speed, units/s */
    float spin_min, spin_max;  /* the same pair as a spin, rad/s */
    float pitch, yaw;          /* angel wings: limitAngles (degrees) */
    float bottom;              /* spherical: restrictBottomAngle (degrees) */
    int   random_dir;          /* angel wings: changeDirRandomly */
    int   owner_frame;         /* angel wings: inheritParentTransform */
    float dir[3];              /* the force's linearDirection, in the owner's axes */
    float speed;               /* the force's linearSpeed, units/s */
    float spin;                /* the force's angularSpeed, rad/s */
} sh_ai_destruct_piece;
typedef struct {
    int   ignore_external;     /* DESTRUCT_FLAG_IGNORE_EXTERNAL_FORCE */
    float max_speed;           /* units/s; 0 none */
    float max_spin;            /* rad/s; 0 none */
    int   n;                   /* pieces, in the breakable's piece order (any number) */
    sh_ai_destruct_piece *piece;   /* n of them; the registry keeps its own copy, a spec it gives back points at it
                                    * until the def is named again or the map ends (sh_ai_destructible_reset) */
} sh_ai_destruct_spec;
typedef float (*sh_ai_rand_fn)(void *state);   /* uniform in [0, 1) */
/* One piece's velocity (units/s) and spin (rad/s) by DOOM Eternal's law (pure). org and axis: the owner's origin and
 * axis (rows forward, left, up); ext: the hit's direction ((0, 0, 1) when the decl ignores it), the frame of the cone and
 * the angel wings without inheritParentTransform; piece: where the piece is. The preset's draws come from rnd. */
void sh_ai_destruct_velocity(const sh_ai_destruct_piece *p, const float org[3], const float axis[9], const float ext[3],
                             const float piece[3], float max_speed, float max_spin, sh_ai_rand_fn rnd, void *rs,
                             float lin[3], float ang[3]);
/* Give a breakable's explosion fx managers (physics at `phys`) the breakable's pieces model as the model their render
 * parm items write, where they have none: DOOM Eternal plays the fx on the destructible's render entity, so an instance
 * tint or fade reaches its pieces; DOOM 2016's managers have no model. -> managers given the model. */
int sh_ai_destruct_fx_model(unsigned char *phys);
/* 1 recorded (a second call for the same def replaces it), 0 the registry
 * is full or the spec is out of range. */
int sh_ai_destructible_add(const char *def, const sh_ai_destruct_spec *spec);
/* 1 and a copy of the spec when one of the n names (an entity's def chain)
 * was recorded, else 0. */
int sh_ai_destructible_find(const char *const *names, int n, sh_ai_destruct_spec *out);
void sh_ai_destructible_reset(void);

/* Forget every per-demon record (a new map). The parked table is the
 * engine side's and is checked by spawn id, so a stale entry never parks
 * another map's demon. */
void sh_script_ai_reset(void);

#ifndef SH_SCRIPT_PURE
#include "../signatures.h"
/* Boot, after the entity calls' engine side (sh_script_api_install, whose event helper checks arguments against the
 * game's event by name): resolve the signatures (script_ai_sigs.c), prepare the IsRunningScript detour (committed at
 * the first park, on the game thread) and register the calls with the engine seam. 1 when usable; 0 with one plain log
 * line, and the AI calls then refuse with "not available". */
int sh_script_ai_install(const uint8_t *module_base);
const char *sh_script_ai_behavior(void *entity);
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
    SH_AI_SIG_WEAPON_IN_SLOT,      /* the weapon in an equip slot (the demon, equipSlot_t) */
    SH_AI_SIG_INVENTORY_ITEM,      /* inventory item by index (the inventory list, index) */
    SH_AI_SIG_MOVE_TRANSITION,     /* the move system's transition query (detoured) */
    SH_AI_SIG_MUZZLE_TEST,         /* the muzzle test (ai, mode, weapon): ray against the focus sphere */
    SH_AI_SIG_CLEAR_MOVE,          /* ClearMove(animFSM, result code) */
    SH_AI_SIG_FIRE_THINK,          /* the fire control's per-frame think (reads the two fire modes) */
    SH_AI_SIG_IDLE_ACTION,         /* the ssaction_Idle handler (writes idleTurnType, reads turnParms) */
    SH_AI_SIG_CHECK_PULL,          /* CheckPullTrigger (the gate; reads requireMuzzleOnTarget) */
    SH_AI_SIG_LASER_ADD,           /* the laser targeter's add-laser-by-tag (component, atom*) */
    SH_AI_SIG_LASER_REMOVE,        /* its remove (component, atom*) */
    SH_AI_SIG_LASER_STATE,         /* the ae_setLaserState handler (reads the component at ai +0x2CD90) */
    SH_AI_SIG_FOOT_TABLE,          /* the footstep table selector (actor, foot) (detoured) */
    SH_AI_SIG_CMD_SET_WAIT,        /* the command context's set-active-wait-handle (context, handle) */
    SH_AI_SIG_MOVE_STAGE,          /* the animFSM's move update (reads the stage) */
    SH_AI_SIG_GORE_INIT_SITE,      /* the demon spawn's gore component init (skipped for the generic type) */
    SH_AI_SIG_SHOW_WOUND,          /* idGoreComponent::ShowWound (component, wound row) (detoured) */
    SH_AI_SIG_MESH_HIDE_SITE,      /* the armour pop-off's mesh index by name and hide by index */
    SH_AI_SIG_PAIN_START,          /* the engine's start of a pain reaction (its FSM request and pain code calls) */
    SH_AI_SIG_RIG_ENTRY,           /* the rig's entry after the blend (runtime, context, align) (detoured) */
    SH_AI_SIG_RIG_RUN,             /* the rig engine's run (reads its blocks) */
    SH_AI_SIG_RIG_BLOCK,           /* a rig block's run (reads its components and calls each one's evaluate) */
    SH_AI_SIG_RIG_EVAL,            /* the two-bone leg component's evaluate (reads a leg's joints, calls the solve) */
    SH_AI_SIG_RIG_SOLVE,           /* the two-bone solve (hip, parent rotation, leg, target, thigh out, shin out) */
    SH_AI_SIG_RIG_SITE,            /* the demon's setup: its model's animation object, whose rig runtime it is */
    SH_AI_SIG_FK_REFERENCE,        /* an fk effector's reference pose (ai, chain address, effector, out) (detoured) */
    SH_AI_SIG_WEB_LEAF,            /* the anim web's current leaf (web) (detoured) */
    SH_AI_SIG_WEB_FORCE_STATE,     /* the anim web's ForceState (sets interruptPath and interruptBlend YES) */
    SH_AI_SIG_WEB_UPDATE,          /* the anim web's update (tests interruptBlend, resets it at a transition) */
    SH_AI_SIG_BREAKABLE_GORE,      /* a gore breakable's weapon-specific gore (manager, breakable, ...) (detoured) */
    SH_AI_SIG_BREAKABLE_BREAK,     /* idBreakable's break (breakable, 0): every piece placed and activated */
    SH_AI_SIG_FXPHYS_PLACE,        /* an effect rigid body's broad phase entry moved (broad phase, entry, pos, axis) */
    SH_AI_SIG_WEAPON_USE,          /* the demon's weapon-use behaviour: the list entry its index names */
    SH_AI_SIG_DAMAGE_WOUND,        /* idGoreComponent::DamageActivatesWound (component, wound, record, ...) (detoured) */
    SH_AI_SIG_WEB_SCALAR_SET,      /* the anim web's set-variable-by-name (animator, atom*, float) */
    SH_AI_SIG_UPDATE_RELATIONS,    /* the senses' UpdateRelations (writes the relation table) */
    SH_AI_SIG_BEST_RELATION,       /* the engine's pick from relations (reads the rows) */
    SH_AI_SIG_FSM_MOVE,            /* the entrance move of an AI FSM into a named state (the pain and death park) */
    SH_AI_SIG_GAME_TIME,           /* the game's time record an FSM request is given */
    SH_AI_SIG_RESET_CORRECTION,    /* reset the active animFSM's root-motion correction */
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
/* The recoil play's multiply by the field e:set_recoil writes (mulss xmm6, [rsi+SH_AI_OFF_RECOIL]). */
static const unsigned char k_ai_recoil_mul[8] = { 0xF3, 0x0F, 0x59, 0xB6, 0xB0, 0x4E, 0x02, 0x00 };
/* The weapon-use accessor is its whole signature: movsxd rax,[rcx+0x250C8] (the index), mov rdx,[rcx+0xC120] (the
 * behaviour decl), mov rax,[rdx+0x398] (the list's elements; its count follows), 0x50 an entry: name pointer +0, data +8. */
#define SH_AI_OFF_WEAPON_USE_INDEX  0x250C8u
#define SH_AI_OFF_BEHAVIOR_DECL     0xC120u
#define SH_AI_BEHAVIOR_WEAPON_USES  0x398u
#define SH_AI_WEAPON_USE_ENTRY      0x50u
#define SH_AI_WEAPON_USE_MAX        64
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
/* Build the pain-string stub at `code` (SH_AI_PAIN_STUB_SIZE bytes): it saves the volatile registers, calls fetch(rbx =
 * the demon) and stores a returned string in [rbp+0x88], then restores all and jumps to `resume` (the trampoline running
 * the stolen bytes). Pure. */
#define SH_AI_PAIN_STUB_SIZE 192
size_t sh_ai_pain_stub_build(uint8_t *code, size_t cap, const void *fetch, const void *resume);
/* Detour windows, whole and position-independent. */
#define SH_AI_THREAT_STOLEN 15
static const unsigned char k_threat_prologue[SH_AI_THREAT_STOLEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x40
};
/* The pain start (SH_AI_SIG_PAIN_START, a site): its call of the alert cycle's FSM request, its call of the pain
 * transition code, and where it finds that FSM (the object at idAI2 +0x123B8, vtable +0x78); the signature holds
 * the last two. The FSM request is detoured: its window and the transition codes the detour acts on. */
#define SH_AI_PAIN_START_FSM_CALL   0x7Eu
#define SH_AI_PAIN_START_CODE_CALL  0x62Eu
#define SH_AI_FSM_OWNER             0x123B8u
#define SH_AI_FSM_GET_SLOT          0x78u
#define SH_AI_FSM_REQUEST_STOLEN    18
static const unsigned char k_ai_fsm_request_prologue[SH_AI_FSM_REQUEST_STOLEN] = {
    0x40, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF
};
#define SH_AI_TRANSCODE_NONE        0
/* A pain a script asks for (request_pain) runs the pain start's own steps with the calls the pain start makes,
 * each found at its call in the pain start (rel32 targets; the same offsets on both renderers):
 *   intensity   (ai, damage info, damage) -> 1 light, 2 medium, 3 heavy
 *   dir index   (ai +0x5008, idVec3 *dir, 4) -> the damage direction's index
 *   pain group  (ai, int *out, joint) -> out
 *   check       CheckPainAnim (VK 0x1403F5440 / GL 0x1403F4920): the pain graph's choice of the clip
 *   str assign  (atomic string *, text): empties the two strings the choice leaves at ai +0x24E38 / +0x24E40
 *   handle set  (idEntityPtr *, entity)
 *   damage copy (dst, src) and damage dtor (damage info): the request's damage info at +0x68
 *   assign      (ai +0x8678, request): the pending request the alert cycle's pain state reads
 *   busy        (fsm +0x60) -> non-NULL while the FSM moves
 *   bb set / bb clear (ai +0x25B30, key[, 1]): the blackboard key the pain state checks, around the request
 * key is the pain start's lea of that key, empty its lea of the engine's empty string. */
#define SH_AI_PS_INTENSITY_CALL     0x119u
#define SH_AI_PS_DIR_CALL           0x154u
#define SH_AI_PS_GROUP_CALL         0x38Du
#define SH_AI_PS_CHECK_CALL         0x462u
#define SH_AI_PS_STR_CALL           0x4A9u
#define SH_AI_PS_HANDLE_CALL        0x533u
#define SH_AI_PS_DAMAGE_COPY_CALL   0x572u
#define SH_AI_PS_ASSIGN_CALL        0x5DDu
#define SH_AI_PS_BUSY_CALL          0x5EAu
#define SH_AI_PS_BB_SET_CALL        0x61Fu
#define SH_AI_PS_BB_CLEAR_CALL      0x626u
#define SH_AI_PS_DAMAGE_DTOR_CALL   0x757u
#define SH_AI_PS_KEY_LEA            0x606u
#define SH_AI_PS_EMPTY_LEA          0x4A2u
/* The pain start's gates (each displacement is the one its instruction at that offset reads; the test checks
 * them) and the request's layout (the assign copies 0x694 bytes; the damage info is 0x620 at +0x68). */
#define SH_AI_PS_OFF_REMOTE         0x2AA1Du  /* byte: no pain is started here (+0x4A) */
#define SH_AI_PS_OFF_FORCED_A       0x2A9ECu  /* byte, with +0xC0E5: the start only asks the FSM for 232 (+0x57) */
#define SH_AI_PS_OFF_FORCED_B       0xC0E5u   /* (+0x60) */
#define SH_AI_PS_OFF_DISABLED       0x2A9E0u  /* int: pain disabled (+0x92) */
#define SH_AI_PS_OFF_BEHAVIOR       0xC120u   /* the aibehavior decl: +0xD4 bit 0 canTakeDamage, +0xD6 bit 2 the
                                                 injured state disables pain (+0x13C, +0x1EF, +0x21C) */
#define SH_AI_PS_VSLOT_DEAD         0x6C8u    /* idAI2 vtable: dead (+0x259; the start says "dead") */
#define SH_AI_PS_OFF_ANIMFSM        0x23198u  /* the animFSM: +0x199C playing a traversal (+0x287, +0x28E) */
#define SH_AI_PS_OFF_INJURED        0x8518u   /* int: the injured state's index, its table at +0x4E48, stride 0x48,
                                                 +0x44 bit 1 lets pain start (+0x2BB, +0x2CA) */
#define SH_AI_PS_OFF_INJURED_TABLE  0x4E48u
#define SH_AI_PS_OFF_CAN_PAIN       0xC2B0u   /* byte: 0 takes no pain at all (+0x357) */
#define SH_AI_PS_OFF_DIRS           0x5008u   /* (+0x143) */
#define SH_AI_PS_OFF_PAIN_STRINGS   0x24E38u  /* the two strings the choice leaves (+0x48F, +0x4AE) */
#define SH_AI_PS_OFF_REQUEST        0x8678u   /* the pending request (+0x5D2) */
#define SH_AI_PS_OFF_BB_FLAG        0x2A9D8u  /* object whose +0xD8 byte picks set or clear (+0x5F8, +0x610) */
#define SH_AI_PS_OFF_BLACKBOARD     0x25B30u  /* (+0x5FF) */
#define SH_AI_PS_REQUEST_SIZE       0x694u
#define SH_AI_PS_REQ_DAMAGE         0x68u
#define SH_AI_PS_REQ_DIR            0x688u
/* The request a script's pain answers to: whether it may go to the engine. `current` is the demon's pain (painType_t,
 * weakest to strongest). A reaction it already plays at least as strongly is not asked (Eternal raises a requested pain
 * only over a weaker one); PAIN_DEATH and PAIN_FULL_GIB are deaths the damage gives. 1 may be asked, 0 not, with *why. Pure. */
#define SH_AI_PAIN_DEATH 12
int sh_ai_request_pain_admits(int reaction, int current, const char **why);
/* The entrance move (SH_AI_SIG_FSM_MOVE, VK 0x14064D330 / GL 0x14064C5B0): an FSM's states are an array at +0x38
 * with their count at +0x40; the move loads idTransitionCode's vtable with a lea at +0xF0, stores code 5 at +0x10F
 * and takes the transition through the FSM's vtable +0xA8 at +0x138 (fsm, move, time, 1, 1). */
#define SH_AI_FSM_STATES            0x38u
#define SH_AI_FSM_STATE_COUNT       0x40u
#define SH_AI_FSM_TAKE_SLOT         0xA8u
#define SH_AI_FSM_MOVE_VT_LEA       0xF0u
#define SH_AI_FSM_MOVE_CODE_STORE   0x10Fu
#define SH_AI_FSM_MOVE_TAKE_CALL    0x138u
/* The alert cycle the AI is in (alertCycle_t; written only by VK 0x1403DD2C0): ALERTCYCLE_DYING 5, _DEAD 6. The
 * think (VK 0x1403E0FE0) prints "is dead but not ALERTCYCLE_DYING or ALERTCYCLE_DEAD" for a dead AI outside both. */
#define SH_AI_OFF_ALERT_CYCLE       0x21958u
#define SH_AI_ALERT_DYING           5
#define SH_AI_ALERT_DEAD            6
#define SH_AI_CODE_DYING            242
#define SH_AI_PAIN_TAKE_MS          250      /* how long the park stays open for a request the alert cycle refused */
#define SH_AI_PATH_LENGTH_STOLEN 14
static const unsigned char k_plen_prologue[SH_AI_PATH_LENGTH_STOLEN] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x63, 0x4A, 0x70, 0x0F, 0x57, 0xE4, 0x48, 0x85, 0xC9
};
/* The rig (SH_AI_SIG_RIG_*). The entry's window: a register save, push rdi, sub rsp and two register moves (whole,
 * position-independent). The runtime holds the rig engine at +0x8; the engine its blocks (0x70 bytes each) and count;
 * a block its components (0x18 bytes: object, legs, count), count and a skip bit; a two-bone leg is 0xCC bytes with its
 * joints from +0x80 (24 bits each). The context's pose (+0x50) holds the parents, joint count and local translations and
 * rotations with strides. The evaluate calls the solve at +0x1ED. */
/* The fk reference pose: both games aim a demon's fk chain from a reference pose of the effector and its chain. The game
 * samples it from the animation of the anim web's current leaf (the heavier of a blend's two) and only without one from
 * the effector's fkGraph poseReferenceAnim; DOOM Eternal does the reverse ("Bad reference anim (%s) in FK Graph node"), so
 * in the game the origin, and the aim and look goals built from it, jump when a blend's new clip gets heavier.
 * fk_reference_pose gives a demon DOOM Eternal's order: while an effector that names a poseReferenceAnim has its reference
 * pose made, the web's current leaf answers none, so the game takes the named animation. */
#define SH_AI_FKREF_STOLEN 15
static const unsigned char k_ai_fkref_prologue[SH_AI_FKREF_STOLEN] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
/* The web's current leaf: push rbx, sub rsp, mov rax,[rcx], mov rbx,rcx and
 * the call of the web's node accessor through rax (register-indirect). */
#define SH_AI_LEAF_STOLEN 18
static const unsigned char k_ai_leaf_prologue[SH_AI_LEAF_STOLEN] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00
};
#define SH_AI_FKREF_LEAF_CALL      0xA7u   /* the reference pose's call of the web's current leaf (E8 rel32) */
#define SH_AI_FK_SUBGRAPH_EFFECTORS 0x40u  /* idDeclFKSubGraph: idList<fkEffector_t> effectors (data, count +8) */
#define SH_AI_FK_EFFECTOR_SIZE     0x58u   /* fkEffector_t */
#define SH_AI_FK_EFFECTOR_REF_LEN  0x28u   /*              poseReferenceAnim (idStr at +0x20): its length */
/* 1 when the effector `effector` of the chain at `chain_address` (an
 * fkChainAddress_t: the subgraph first) names a poseReferenceAnim, 0 when it
 * names none or cannot be read. */
int sh_ai_fk_reference_named(const void *chain_address, int effector);

#define SH_AI_RIG_STOLEN 16
static const unsigned char k_ai_rig_prologue[SH_AI_RIG_STOLEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9
};
#define SH_AI_RIG_SITE_MODEL       4       /* the site: disp32 of mov rcx,[r12+model] */
#define SH_AI_RIG_SITE_ANIM        23      /* the site: disp32 of mov rcx,[rcx+animation object] */
#define SH_AI_RIG_ANIM_RUNTIME     0x8u    /* animation object: its rig runtime */
#define SH_AI_RIG_RUNTIME_ENGINE   0x8u
#define SH_AI_RIG_ENGINE_BLOCKS    0x0u
#define SH_AI_RIG_ENGINE_COUNT     0x8u
#define SH_AI_RIG_BLOCK_SIZE       0x70u
#define SH_AI_RIG_BLOCK_PARTS      0x18u
#define SH_AI_RIG_BLOCK_COUNT      0x20u
#define SH_AI_RIG_BLOCK_SKIP       0x60u   /* bit 0: the block is not run */
#define SH_AI_RIG_PART_SIZE        0x18u   /* object +0, legs +8, count +0x10 */
#define SH_AI_RIG_PART_EVAL_SLOT   11      /* the object's vtable slot the block calls (+0x58) */
#define SH_AI_RIG_LEG_SIZE         0xCCu
#define SH_AI_RIG_LEG_PARENT       0x80u
#define SH_AI_RIG_LEG_TARGET       0x84u
#define SH_AI_RIG_LEG_POLE         0x88u
#define SH_AI_RIG_LEG_HIP          0x8Cu
#define SH_AI_RIG_LEG_THIGH        0x90u
#define SH_AI_RIG_LEG_FOOT         0x98u
#define SH_AI_RIG_EVAL_SOLVE_CALL  0x1EDu
#define SH_AI_RIG_CTX_POSE         0x50u
#define SH_AI_RIG_POSE_PARENTS     0x0u
#define SH_AI_RIG_POSE_JOINTS      0x8u
#define SH_AI_RIG_POSE_LT          0x10u
#define SH_AI_RIG_POSE_LQ          0x18u
#define SH_AI_RIG_POSE_LT_STRIDE   0x28u
#define SH_AI_RIG_POSE_LQ_STRIDE   0x2Cu
/* The footstep table selector's window: two register saves and push rsi, r14, r15 (whole, position-independent). */
#define SH_AI_FOOT_STOLEN 15
static const unsigned char k_ai_foot_prologue[SH_AI_FOOT_STOLEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x56, 0x41, 0x57
};
/* The laser targeter: the component of an idAI2, its decl, the decl's
 * laserPinpoint_ribbonDecl; and where the add-laser function reads the last
 * two (mov rdx,[rsi+0x10] ... mov rdx,[rdx+0x108]). */
#define SH_AI_OFF_LASER_COMPONENT  0x2CD90u
#define SH_AI_LASER_COMP_DECL      0x10u
#define SH_AI_LASER_DECL_RIBBON    0x108u
#define SH_AI_LASER_ADD_DECL_AT    0x193u
#define SH_AI_LASER_ADD_RIBBON_AT  0x1B2u
/* The laser add's two reads that prove where the ribbon decl sits: mov rdx,[rsi+0x10]; mov rdx,[rdx+0x108]. */
static const unsigned char k_ai_laser_decl_read[4] = { 0x48, 0x8B, 0x56, 0x10 };
static const unsigned char k_ai_laser_ribbon_read[7] = { 0x48, 0x8B, 0x92, 0x08, 0x01, 0x00, 0x00 };
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

/* The move transition detour window: push rbp, rsi, rdi, r12-r15; lea rbp,[rsp-0x1640] (whole, position-independent).
 * The turn on the spot is kind 3; byte 0x102 bit 0 of the request lets its clip query take a clip whose root travels. */
#define SH_AI_MOVE_STOLEN 20
static const uint8_t k_ai_move_prologue[SH_AI_MOVE_STOLEN] = {
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xE9, 0xFF, 0xFF
};
#define SH_AI_MOVE_KIND_TURN       3
#define SH_AI_MOVE_REQ_OVERSHOOT   0x102u

/* idAI2 offsets the fire calls read (both renderers, 13954591; reflected
 * paths in brackets; tests/script_ai_sigs_test.c checks the code that uses
 * each). The fire control is the object at ai +0x22278. */
#define SH_AI_FC_MODE_ANIM        0x1Cu     /* fireControl: [fireMode_anim] set by ae_setFireMode */
#define SH_AI_FC_MODE_STATE       0x20u     /* fireControl: [fireMode_state] set by setFireMode */
#define SH_AI_FC_REQUIRE_MUZZLE   0x27u     /* fireControl: [requireMuzzleOnTarget], a byte */
#define SH_AI_OFF_FOCUS_VALID     0xEE9Bu   /* [aiVolatile.focus.foci[0].finalFocusPointIsValid] */
#define SH_AI_OFF_FOCUS_FINAL     0xE020u   /* [...foci[0].finalFocusPoint] three floats */
#define SH_AI_OFF_FOCUS_TRACKING  0x11AE0u  /* [aiVolatile.focus.enableHeadTracking] */
#define SH_AI_OFF_FOCUS_BODY_ROT  0x11AE1u  /* [aiVolatile.focus.enableBodyRotation] */
#define SH_AI_OFF_FOCUS_RADIUS    0x11AE4u  /* [aiVolatile.focus.aimFocusTargetRadius] */
#define SH_AI_OFF_FOCUS_PRECISION 0x11AE8u  /* [aiVolatile.focus.muzzleAimPrecision] */
#define SH_AI_OFF_BEHAVIOR_DECL   0xC120u   /* [aiEditable.behaviors.decl]: turnParms list +0x528, count +0x530 */
#define SH_AI_BEHAVIOR_TURNPARMS  0x528u
#define SH_AI_BEHAVIOR_TURNCOUNT  0x530u

/* The animFSM's stage and the animWeb command it waits on (both renderers; reflected paths in brackets;
 * tests/script_ai_sigs_test.c checks the code that uses each). The command context is the object the demon's vtable
 * +0x2C8 returns [aiVolatile.animation.awCmdCtx]. */
#define SH_AI_VSLOT_CMD_CTX       0x2C8u    /* the animWeb command context */
#define SH_AI_CMDCTX_ACTIVE       0x18u     /* context: [activeWaitHandle], the command it waits on (NULL: none) */
#define SH_AI_WAIT_STATUS         0x18u     /* wait handle: [status], an awCmdStatus_t */
#define SH_AI_AWCMD_WAITING       8         /* AWCMD_STATUS_WAITING */
#define SH_AI_AWCMD_READY         11        /* AWCMD_STATUS_READY_TO_COMPLETE */
#define SH_AI_FSM_STATE           0x2A98u   /* animFSM: [state], 1 while it moves */
#define SH_AI_FSM_STAGE           0x2A9Cu   /* animFSM: [subState], the move stage */
#define SH_AI_FSM_STATE_MOVE      1
#define SH_AI_FSM_MOVE_FLAGS      0xB6Cu    /* animFSM: [overrideMoveFlags]; ssaction_Idle sets bit 8 */
#define SH_AI_FSM_IDLE_TURN_FLAG  8
#define SH_AI_FSM_IDLE_TURN       0xBF0u    /* animFSM: [idleTurnType] */

/* idAI2 offsets the calls read (both renderers, 13954591). */
#define SH_AI_OFF_MOVE_FSM        0x23198u  /* move FSM pointer; status int at +0xD20 */
#define SH_AI_OFF_MOVE_STATUS     0xD20u
#define SH_AI_OFF_WEAPON_HOLDER   0x22278u  /* +8: the equipped weapon (else vtable +0xB28) */
#define SH_AI_VSLOT_WEAPON        0xB28u    /* the weapon in the weapon hand (below) */
#define SH_AI_VSLOT_WEAPON_HAND   0xB20u    /* int: the aibehavior decl's weaponSlot (decl +0x134) */
#define SH_AI_VSLOT_INVENTORY     0x660u    /* the inventory list (ai +0x9900): entries +0, count +8 */
#define SH_AI_INVENTORY_STRIDE    0x20u     /* one idEntityPtr per item */
#define SH_AI_ITEM_DECL           0x30u     /* inventory item: its decl */
#define SH_AI_DECL_EQUIP_SLOT     0x1B4u    /* inventory decl: equipSlot_t */
#define SH_AI_OFF_WEAPON_SLOT     0x8D4u    /* weapon: active trigger index */
#define SH_AI_OFF_WEAPON_TRIGGER  0x8F8u    /* weapon: trigger state per index, 0 = pulled */
#define SH_AI_OFF_ENEMY           0x25050u  /* idEntityPtr of the current enemy */
#define SH_AI_OFF_PERCEPTION      0x29A88u
#define SH_AI_OFF_PATH_NAME       0x388u    /* the name the path key is built from */
#define SH_AI_OFF_PHYSICS         0x808u    /* idEntity: physics; vtable +0x80 origin(0), +0x88 axis(0) */
#define SH_AI_PHYSICS_OBJECT      0xC2B8u   /* idAI2.aiVolatile.physics.object */
#define SH_AI_PHYSICS_ON_GROUND   0x1A72u   /* idPhysics_AI.current.onGround */
#define SH_AI_VSLOT_ORIGIN        0x80u
#define SH_AI_VSLOT_AXIS          0x88u
#define SH_AI_TYPE_NUM            0x68u     /* idTypeInfo: type number, +0x6C last descendant */

/* Breakables (both renderers, 13954591; reflected names in brackets; tests/script_ai_sigs_test.c checks the code
 * that uses each). The weapon-specific gore's window is 19 bytes: mov r11,rsp; push rbp, r12..r15; lea rbp,
 * [r11-0xB8] (position-independent). */
#define SH_AI_BRK_PHYSICS         0xCA8u    /* idBreakable: [breakablePhysics] (idEffectPhysicsBreakable) */
#define SH_AI_BRK_BROKEN          0xE6Cu    /* idBreakable: [brokenStartTime], -1 until the break */
#define SH_AI_FXB_BODIES          0x18u     /* idEffectPhysicsBreakable: [rigidBodies] list, count at +8 */
#define SH_AI_RB_PROPS            0x00u     /* idEffectPhysicsRigidBody: [properties] */
#define SH_AI_RB_POS              0x08u     /* [currentState.position] */
#define SH_AI_RB_AXIS             0x14u     /* [currentState.orientation] (rows) */
#define SH_AI_RB_LINEAR           0x38u     /* [currentState.linearMomentum] */
#define SH_AI_RB_ANGULAR          0x44u     /* [currentState.angularMomentum] */
#define SH_AI_RB_SPOBJECT         0x68u     /* [SPObject], the broad phase entry */
#define SH_AI_RB_ACTIVE           0x110u    /* [active] */
#define SH_AI_RB_SETTLED          0x111u    /* [settled] */
#define SH_AI_PROPS_BROADPHASE    0x00u     /* idEffectPhysicsProperties: [broadPhase] */
#define SH_AI_PROPS_INV_MASS      0x40u     /* [inverseMass] */
#define SH_AI_PROPS_INV_INERTIA   0x74u     /* [inverseInertiaTensor] (body axes) */
#define SH_AI_FXB_EXPLOSION_FX    0x68u     /* idEffectPhysicsBreakable: [explosionFX] list of idFXManager, count +8 */
#define SH_AI_FXB_MODEL           0x180u    /* idEffectPhysicsBreakable: [model], the pieces' render model */
#define SH_AI_FXM_SIZE            0x338u    /* sizeof idFXManager */
#define SH_AI_FXM_RM              0x30u     /* idFXManager: [rm], the render model its render parm items write */
#define SH_AI_BRK_GORE_STOLEN     19
static const unsigned char k_ai_brk_gore_prologue[SH_AI_BRK_GORE_STOLEN] = {
    0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x48, 0xFF, 0xFF, 0xFF
};

/* wound_hides takes a non-demon entity when fn answers 1 for it (an entity
 * with a gore component; script_api_body.c). */
void sh_script_ai_set_gore_owner_test(int (*fn)(void *ent));

#ifdef __cplusplus
}
#endif
#endif
