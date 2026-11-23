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
 * ai:release() hands it back before that: its park slot is marked released,
 * IsRunningScript gives the engine's own answer for it again, the alert
 * cycle leaves ExecuteDoomscript by NotExecutingScript and the demon runs the
 * game's own behaviours (its search when it has lost its enemy, its combat
 * when it finds one), with nothing of the script's order left standing; the
 * next command parks it again (the mark is cleared and the script proxy
 * forces the alert cycle back into ExecuteDoomscript).
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
    SH_AI_EV_SET_FIRE_MODE,          /* setFireMode                i     */
    SH_AI_EV_AE_PULL_TRIGGER,        /* ae_aIPullTrigger           4sb   */
    SH_AI_EV_ENABLE_BODY_ROTATION,   /* enableBodyRotation         b     */
    SH_AI_EV_ENABLE_HEAD_TRACKING,   /* enableHeadTracking         b     */
    SH_AI_EV_TEMP_MEMORY_REMOVE,     /* tempMemory_Remove          s     */
    SH_AI_EV_COUNT
};

/* A SnapMap order (R-SM1 finding 3, read on both renderers: work/SM2/evidence/vk-dc-0x140439680.json and its
 * siblings). SnapMap's Follow action (idSnapMapAction_FollowPath) writes it into the demon's temp memory for the
 * stock FSM, whose snapmap state takes it and removes the entry: a path entity, a goal entity, or a position. A
 * demon its own script drives reads it here and removes it as the stock state does. */
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
/* What a forced via needs this frame, from what the web was seen doing
 * (SH_SCRIPT_ANIM_* flags of the request), whether its destination was asked
 * already, and the frames since it was issued. The destination is asked as
 * soon as the forced node is seen playing: a forced clip holds its last frame
 * until the web is asked to go somewhere. Pure. */
enum { SH_AI_FORCE_VIA_WAIT, SH_AI_FORCE_VIA_ASK_DEST, SH_AI_FORCE_VIA_DONE };
int sh_ai_force_via_next(unsigned flags, int asked, uint32_t frames_since_issue, uint32_t give_up_frames);

/* 1 while a parked demon stays the script's: the engine's FSM is told a
 * script is running, so it starts no behaviour of its own. The engine keeps
 * two things: death (a dead demon goes to its dying cycle, ragdoll and
 * removal as any demon does) and pain (the frame's damage asked for a pain
 * reaction, or one is playing: the engine's own pain cycle picks and plays
 * the clip, and the script hears of it through on.pain). The demon is the
 * script's again when the reaction is over. Pure. */
int sh_ai_park_holds(int alive, int pain_requested, int pain_reaction);
/* One parked demon (the engine side's table; the FSM threads read it without
 * a lock). drives: the slot is this entity's (its spawn id now, id_now,
 * matches) and not released, so IsRunningScript answers what the park holds;
 * otherwise the engine's own answer. release marks it released (and ends a
 * pain the park had opened for); take, at every park, clears the mark. */
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

void sh_ai_move_issue(sh_ai_move *m, uint32_t frame);
/* Feed the status read at `frame`; returns the move's state. */
int  sh_ai_move_step(sh_ai_move *m, int status, uint32_t frame);

/* The awareness word's name: "unaware", "lost", "suspected", "confirmed";
 * NULL for any other value. */
const char *sh_ai_awareness_name(int level);

/* A demon's social relations, as DOOM 2016's senses keep them (both
 * renderers, 13954591). The senses (ai +SH_AI_OFF_PERCEPTION) rebuild a table
 * every AI think, whatever drives the demon (UpdateRelations VK 0x1404AA490 /
 * GL 0x1404A8C10, called by the senses' update from the AI think VK
 * 0x1403E0D40): one row per entity the demon perceives, players and bots
 * (the idPlayer classes) and demons, with the respect and emotion its faction
 * graph gives the two factions (a SnapMap team makes them "team_<n>_demon" and
 * "team_<n>_player", the co-op mode's VK 0x14088FAF0) or what it perceived of
 * that entity (being hurt raises the emotion), whichever is farther from
 * neutral; sorted by emotion, most hostile first. Rows at +0x28, count at
 * +0x30, 0x28 bytes a row: a word at +0 (0x1FFFFFE when the row has no entity
 * of its own: the 10,000-unit range of GetRelations, VK 0x1404AAA30, does not
 * limit it), respect +4, emotion +8, the entity's idEntityPtr +0x10. The
 * engine's pick reads the same rows (VK 0x1404AAD10 / GL 0x1404A9490). */
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
/* The words ai:relations answers: "min", "peon", "minion", "peer", "leader",
 * "worship", "max" and "min", "terrified", "fear", "wary",
 * "destroy_at_all_costs", "destroy", "hate", "anger", "dislike", "annoyed",
 * "neutral", "friendly", "squadmate", "partner", "max"; NULL outside. */
const char *sh_ai_respect_name(int respect);
/* An actor's off-limits flag (both renderers, 13954591): idActor volatile offLimits, a byte the actor's virtual
 * +SH_AI_VSLOT_OFF_LIMITS answers (idAI2's and idPlayer's VK 0x140427550 / GL 0x140426600: movzx eax, byte
 * [rcx+0x9408]; ret) and its +0x588 sets; a sync interaction that marks its instigator off-limits
 * (idDeclSyncInteraction markInstigatorOffLimits) sets it. Shared_SocialRelationAvailable's filterOutOffLimits drops
 * an actor that has it (VK 0x1405F4930). The call reads the byte through the getter it finds in the actor's vtable,
 * after checking the getter is that one instruction with this displacement. */
#define SH_AI_VSLOT_OFF_LIMITS    0x580u
#define SH_AI_OFF_OFF_LIMITS      0x9408u
/* 1 when `code` is the off-limits getter: movzx eax, byte [rcx + SH_AI_OFF_OFF_LIMITS]; ret. */
int sh_ai_off_limits_getter(const uint8_t code[8]);
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
/* The attack graph's VALIDATE_ANIM_DELTA: DOOM Eternal's anim delta check
 * (FUN_1409007D0, read) is a navigation trace from the AI to where the clip
 * ends (FUN_1408E2420), which no player or demon blocks and which a clip with
 * no root motion passes. The port's composition: the end stands on the world's
 * floor and the straight line to it at knee and chest height is clear of the
 * world, players and demons passed through (world_ground, world_clear). */
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

/* Wound damage counts. The game opens a living demon's gore wound by its
 * damage GROUP: idGoreComponent::DamageActivatesWound (SH_AI_SIG_DAMAGE_WOUND)
 * is asked per queued hit record, per damage group of the record and per
 * wound the group's gore nodes offer, with the health the whole group has
 * lost, and it passes the wound when that loss has reached the wound's
 * threshold and the hit lands inside the wound's clipping spheres (with its
 * frame damage, damage-per-second, gore type, distance and direction gates).
 * Wounds that share a group (two halves of a face on one joint) open
 * together, on a hit inside either. DOOM Eternal keeps a counter per wound:
 * the damage of each hit that landed inside that wound's own spheres, and the
 * wound opens when that sum reaches its threshold. ai:wound_damage_counts(true)
 * gives one demon that rule: the product calls the game's function with the
 * lost health raised past any threshold (SH_AI_DAW_LOST_RAISED), so every
 * other gate, the sphere test among them, is the game's own; when the game
 * passes, the record's damage is added to the wound's counter and the answer
 * is whether the counter has reached the wound's threshold.
 *
 * The threshold as the game computes it (VK 0x1406E8A80, GL 0x1406E7EE0):
 * the wound's relativeDamageRequiredToGore times the owner's max health when
 * that is not negative, else totalDamageRequiredToGore. The game's gate
 * refuses only a threshold above 0 that the loss is below. Pure. */
float sh_wound_count_threshold(float total, float relative, float max_health);
/* One hit on one wound: gates_passed is the game's answer with the lost
 * health raised. A refused hit never counts (*sum unchanged, 0); a passed one
 * adds damage (a negative or non-finite damage adds nothing) and answers 1
 * when the sum has reached the threshold, or always for a threshold that is
 * not above 0 (the game has no threshold then). Pure. */
int   sh_wound_count_step(float threshold, float *sum, float damage, int gates_passed);
/* The per-demon counters: one record per demon that asked, keyed by the demon
 * and its spawn id, one counter per wound decl (decls outlive the map), up to
 * SH_WOUND_COUNT_WOUNDS wounds a demon. A record is dropped when the demon's
 * handle is released (sh_script_handles_on_gone) or the call is turned off,
 * and the whole table at a map's start. Lock-protected: the gore think and
 * the script call may be on different threads. */
#define SH_WOUND_COUNT_WOUNDS 64
void  sh_wound_count_reset(void);
int   sh_wound_count_set(void *ai, uint32_t spawn_id, int on);    /* 1 done, 0 the table is full */
int   sh_wound_count_on(void *ai, uint32_t spawn_id);             /* 1 the demon asked */
/* The rule for one call of the game's function on a demon that asked. record
 * and mark (the record's damage and position bits, sh_wound_count_mark) name
 * the hit: the same hit offered twice for one wound counts once. 1 pass, 0
 * refuse, -1 not counted (the demon did not ask, or its wounds are full): the
 * caller answers the game's own rule. */
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

/* Where the demon's animFSM stands with a step animation of its own (a turn
 * in place, a step or an arrival of a move). The engine ends such a stage
 * when the animWeb command it gave ends; a script's own animation order goes
 * round that command, so the stage would wait for good (the engine then
 * restarts the animFSM after a second in its idle, and until then starts no
 * turn and no move). */
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
    /* Run catalogue event `ev` on `entity` now. Returns 1 when it ran, with
     * its return value (an int, bool or float the event returns) in *ret
     * when ret is not NULL; 0 when it is unavailable or faulted. */
    int      (*event)(void *entity, int ev, const sh_ai_arg *args, int n, double *ret);
    int      (*park)(void *ai);                       /* 1 parked (now or before) */
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
    /* The root motion of animation `index` of animWeb node "subweb/state" of
     * the demon, in its own frame (x forward, y left, z up). 1 read, 0 not
     * (index past the last); *count gets the node's animations; -1 no such
     * node. */
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
    int      (*path_points)(void *ai, const float goal[3], int key, float (**out)[3], int *n);
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
    /* A laser of the demon's laser targeter component on model tag `tag`,
     * drawn with ribbon decl `ribbon` (its whole name; NULL: the component
     * decl's laserPinpoint_ribbonDecl), off until it is switched on. 1 added,
     * 0 the demon has no laser targeter, -2 no such ribbon decl is loaded,
     * -1 unavailable. */
    int      (*laser_add)(void *ai, const char *tag, const char *ribbon);
    /* Take it away again. 1 done, 0 no laser targeter, -1 unavailable. */
    int      (*laser_remove)(void *ai, const char *tag);
    /* For the demon's rear feet (the game's foot indices 2 and 3: the
     * ae_leftRearFoot and ae_rightRearFoot events) play footstep table `rear`
     * wherever the game picks `front` (whole projectileImpactEffect decl
     * names). 1 set, -2 one of the decls is not loaded, -1 unavailable. */
    int      (*rear_footsteps)(void *ai, const char *front, const char *rear);
    /* The animFSM's stage and the command it waits on (sh_ai_step_state).
     * 1 read, -1 unreadable. */
    int      (*step_state)(void *ai, sh_ai_step_state *out);
    /* End the animWeb command the demon's command context waits on, as the
     * engine does itself when another command takes its place; the stage of
     * the animFSM that waits on it then ends at its next update. 1 done, 0
     * none was waiting, -1 unavailable. */
    int      (*step_interrupt)(void *ai);
    /* Give a demon of the generic monster type the gore component the game's
     * spawn gives every other type (the game's own init with the gore graph
     * its entityDef names). 1 done, 0 nothing to do (another type, no gore
     * graph, already done, gore off), -1 unavailable. */
    int      (*gore_init)(void *ai);
    /* From now on, when the game shows the demon's gore wound `wound` (a
     * whole gorewounds decl name), hide the n meshes of its model named in
     * `meshes`, as the game's armour pop-off hides a damage group's
     * hideMeshes. 1 set, -2 the wound decl is not loaded, -3 the model has
     * no mesh of one of the names (nothing set), -1 unavailable. */
    int      (*wound_hides)(void *ai, const char *wound, const char *const *meshes, int n);
    /* The decl name of the inventory item in `hand` now (the engine's weapon in that equip slot): 1 and the
     * name, 0 when the hand is empty, -1 when it cannot be read. */
    int      (*held)(void *ai, int hand, char *decl, size_t cap);
    /* From now on the game's leg rig takes each leg's target and pole from
     * the legs as the demon's clips pose them, every frame, before it follows
     * the ground. 1 set, 0 the demon's model has no two-bone leg rig, -1
     * unavailable. */
    int      (*posed_legs)(void *ai);
    /* From now on the demon's fk effectors take their reference pose from
     * the poseReferenceAnim their fkGraph names, as DOOM Eternal's do, not
     * from the animation of the anim web's current leaf (sh_ai_fk_reference
     * below). 1 set, -1 unavailable. */
    int      (*fk_reference_pose)(void *ai);
    /* Set the interrupt blend mode of the demon's anim web (SH_AI_INTR_BLEND_)
     * for the order just given: the mode the web's next transition takes when
     * it meets a running blend (sh_ai_order_interrupt below). 1 set, -1
     * unavailable. */
    int      (*interrupt_blend)(void *ai, int mode);
    /* Make sure the game's handling of a gore breakable's hit runs through
     * the product (sh_ai_destructible below). 1 ready, -1 unavailable. */
    int      (*destructible)(void);
    /* Select the entry named `name` of the demon's behaviour decl's
     * weaponUseBehaviorList as its weapon-use behaviour (the recoil clip and
     * weights, the trigger-hold times): the game selects "default" at spawn
     * and never again. 1 selected, 0 the list has no entry of that name
     * (nothing changed), -1 unavailable. */
    int      (*weapon_use)(void *ai, const char *name);
    /* on 1: from now on a gore wound of the demon opens on the damage that
     * landed inside its own hit spheres (sh_wound_count_step); on 0: by the
     * game's own rule again. 1 set, -1 unavailable. */
    int      (*wound_damage_counts)(void *ai, int on);
    /* The SnapMap order waiting in the demon's temp memory (sh_ai_order_find
     * on its slots, AI +0x25B30 on both renderers): 1 with it, 0 none, -1
     * unreadable. It stays there until the caller removes it. */
    int      (*snapmap_order)(void *ai, sh_ai_order *out);
    /* Hand the parked demon back to its own behaviour: IsRunningScript gives
     * the engine's own answer for it again, so its alert cycle leaves
     * ExecuteDoomscript by the NotExecutingScript link and runs the stock
     * behaviours (its search when it has lost its enemy), until the next park
     * (the next command) takes it back. 1 done (also for a demon that was not
     * parked), -1 unavailable. */
    int      (*release)(void *ai);
    /* Set the variable `name` of the demon's body anim web to `value`, by the
     * engine's own set-by-name (sh_ai_web_scalar below): the member it is
     * bound to, or the web's own scalar memory. 1 set, 0 the web has no
     * variable of that name, -1 unavailable. */
    int      (*anim_variable)(void *ai, const char *name, float value);
    /* The demon's social relations (sh_ai_relation above), in the engine's
     * order, rows whose entity is gone left out: their count (at most cap),
     * -1 unreadable. */
    int      (*relations)(void *ai, sh_ai_relation *out, int cap);
    /* The actor's off-limits flag (sh_ai_off_limits_getter above): 1 set, 0 not, -1 not an actor or unreadable. */
    int      (*off_limits)(void *actor);
    /* The world alone between two points, as a navigation trace sees it: a
     * trace from `from` to `to` passing the demon and every player and demon
     * it meets (none of them is part of the ground a demon walks). 1 clear,
     * 0 blocked by the world, -1 unreadable. */
    int      (*world_clear)(void *ai, const float from[3], const float to[3]);
    /* The floor under pos, the same way: players and demons (the asking
     * demon's own body included) are passed through. 1 found, with the
     * point in out. */
    int      (*world_ground)(void *ai, const float pos[3], float out[3]);
} sh_script_ai_engine;

/* The SnapMap order of a temp memory block (pure): `keys[i]` the slot's key
 * text (an interned GAME_STRING; NULL for an empty or unreadable slot) and
 * `values[i]` its 32-bit value, n slots. The first order key found wins: an
 * entity order's value is the target's spawn id; a position order spans
 * three slots of one key (x, y, z as floats). 1 with *out, 0 none. */
int sh_ai_order_find(const char *const *keys, const uint32_t *values, int n, sh_ai_order *out);

/* Posed legs. The game poses a leg by a rig: it solves thigh and shin from
 * the hip to a target joint and swivels the knee by a pole joint, and its own
 * clips animate those two joints and carry no thigh, shin or foot tracks. A
 * demon whose clips rotate the leg joints themselves has its pose in those
 * joints, with every additive layer blended in; this turns that pose into the
 * rig's inputs. Rotations are x y z w; a joint's model transform is its
 * parent's times its own. */
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
/* Write the target joint's local translation and rotation (the end joint's
 * model transform under the target's parent) and the pole joint's local
 * rotation (about x by the angle that turns the solved thigh to the posed one
 * about the line from hip to target) into the pose. 1 written, 0 not (a joint
 * out of range, a leg of no length, the solve refused). */
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
/* DamageActivatesWound (SH_AI_SIG_DAMAGE_WOUND; VK 0x1408FC8E0, GL
 * 0x1408FB910, the same code on both): (component, wound decl, record,
 * damage group decl, group index, lost health, max health) -> a bool in al.
 * The arguments: rcx the gore component (its owner handle at +0x608, the
 * owner at +0x610, SH_AI_GORE_OWNER); rdx the wound's gorewounds decl (the
 * gore node's areaWoundDecl; the frame gate reads its +0x218); r8 the hit's
 * goreFrameDamage_t; r9 the damage group decl (only tested for NULL); the
 * stack's 5th the group index (an int, at most 31), 6th the group's lost
 * health (an int: movd and cvtdq2ps at +0x1E0, the only read of it), 7th the
 * owner's max health (a float: movss xmm1 at +0x1CE, passed to the threshold
 * helper the call at +0x1D6 reaches). The helper reads the wound's +0x214
 * (relative) and +0x210 (total) as sh_wound_count_threshold does. The record
 * is one slot (0x58 bytes) of the component's 8-slot ring; the frame damage
 * queue (VK 0x1408FA7B0, GL 0x1408F97E0) stores at its +0 the hit's damage
 * times the gore behaviour's multiplier (+0x9C), the gore damage cvar and the
 * damage's own scale, at +4 its position. tests/script_ai_sigs_test.c checks
 * every byte named here on both executables. The window: push rbp, rbx, rsi,
 * rdi, r12, r14, r15; lea rbp,[rsp-0xF] (whole, position-independent). */
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
#define SH_AI_WOUND_MESHES        24      /* meshes one wound can hide */
#define SH_AI_WOUND_ROWS          32      /* wounds of one demon that hide meshes */
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

/* Add the AI calls to every script state opened after this. Returns the
 * number of calls added. distance, animation_state and force_animation are the entity
 * calls' (script_api_entity.c), which work on any entity; on a demon
 * animation_state and force_animation come here. */
int sh_script_ai_register(void);

/* Park the mod's own demon `ai` (handle h) and ask its body animWeb for
 * `dest` now: ssaction_ChangeAnimState, ..Via through `via`, or with force
 * ssaction_ForceAnimState blending blend_ms milliseconds. 1 when the engine
 * ran it; 0 when the demon cannot be parked or the AI calls are unavailable
 * this session. Game thread, inside the script tick. */
int sh_script_ai_anim(void *ai, sh_script_handle h, const char *dest, const char *via, int force, int blend_ms);

/* The kinds of animation order (sh_script_ai_anim's `force`). FORCE is
 * ssaction_ForceAnimState; FORCE_VIA forces the via node of ai:force_animation_via
 * the same way; VIA_DEST is the change to its destination asked when the via
 * plays; CHANGE is ssaction_ChangeAnimState(Via). */
enum { SH_AI_ORDER_CHANGE = 0, SH_AI_ORDER_FORCE = 1, SH_AI_ORDER_FORCE_VIA = 2, SH_AI_ORDER_VIA_DEST = 3 };

/* Interrupts. Both games' anim web update (VK 0x14170B890, DOOM Eternal
 * 0x140495800) meets a new transition while the model's blend still runs in
 * one of two ways. When the current node carries AW_CF_USE_MULTIPLE_INTERRUPT
 * (bit 25 of its customFlags in both games) the new blend is nested over the
 * running one, the oldest clip dropped when two already run. Otherwise the
 * web's interruptBlend decides: NO waits for the blend to end, YES ends the
 * running blend at once (the pose jumps to its destination) and then blends,
 * MULTI nests. Each transition resets the mode to NO. The orders set it:
 * ChangeState and ChangeStateVia take it from their caller, ForceState sets
 * YES and ForceAndChangeState (a forced via with a destination) sets MULTI,
 * the same functions in both games. DOOM Eternal's AI command (constructor
 * 0x1408F2D40: intrPath YES, intrBlend MULTI; its processor 0x1408F0EE0)
 * gives a change MULTI, a forced via MULTI and a forced node without a via
 * YES. The game's script events give ssaction_ChangeAnimState(Via) NO (VK
 * 0x1405DC680) and ssaction_ForceAnimState YES (VK 0x1405DCAC0, ForceState),
 * so a ported demon's changes waited and its forced vias cut where DOOM
 * Eternal's nest. sh_ai_order_interrupt: the mode DOOM Eternal's command of
 * that kind carries, or -1 to leave the game's (a forced node: YES in both;
 * the via's destination: in DOOM Eternal the second leg of the same command,
 * whose transition finds the mode reset). */
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

/* The anim web's variables (both renderers; SH_AI_SIG_WEB_SCALAR_SET, VK
 * 0x14170A950 / GL 0x14170B3C0). An idAnimator_AnimWeb holds one row per
 * variable its web names (rows at +0x580, count +0x588, 0x18 bytes a row): the
 * name's atom at +0, a short at +10 and a pointer at +0x10. A variable the
 * owner binds to one of its own members (an idAI2's are idAnimWebAI::
 * blendVariables_t: hangoutIndex, painIndex, deathAnimIndex ...) has the short
 * below 0 and the member at +0x10; any other has its index into the web's own
 * float memory (the pointer at +0x598). The engine's set-by-name finds the row
 * whose atom is the name's and stores the float there (the store VK
 * 0x1417574D0 / GL 0x141758620); the game's own snippets set their variables
 * this way (idAISnippet_Shared_SetScalarToProjectileIndex, VK 0x1404B02A0: the
 * animator at ai +0x222B8). 1 when a row of that name was found. */
#define SH_AI_WEB_ROWS             0x580u
#define SH_AI_WEB_ROW_COUNT        0x588u
#define SH_AI_WEB_SCALAR_MEMORY    0x598u
#define SH_AI_WEB_ROW_SIZE         0x18u
#define SH_AI_WEB_SCALAR_STORE     0x52u    /* the set: call store(row, &memory, value) */

/* Park the mod's own demon `ai` (handle h) without a command: 1 when it is
 * parked. ai:fire uses it, so a demon a script fires from stops its own
 * behaviour like one it moves. Game thread, inside the script tick. */
int sh_script_ai_park(void *ai, sh_script_handle h);

/* Destructibles. DOOM Eternal throws a destructible's pieces by SETTING each
 * one's velocity when it is activated (idDestructibleMechanic_Generic::
 * OnActivation 0x140AF8020, its sum 0x140AEEB50, the presets 0x140AEF540):
 * a preset direction (spherical, angel wings, random, cone) at a speed drawn
 * from the preset's [min, max], plus the decl's force (linearDirection in the
 * owner's axes at linearSpeed); the spin is the same preset direction at a
 * second draw from [min, max] plus the force direction at angularSpeed (the
 * preset's own angular axis is computed and never read). The frame is the
 * owner's origin and axis (the gore wound's spawn 0x140C4C170 passes them).
 * A decl flagged DESTRUCT_FLAG_IGNORE_EXTERNAL_FORCE takes nothing from the
 * hit (0x140AF3F10 zeroes it). Velocities are capped at
 * Destructible_MaxLinearVelocity (30 m/s) and Destructible_MaxAngularVelocity
 * (31.42 rad/s). DOOM 2016 throws a breakable's pieces by explosion records
 * (impulse / ((d + 1) x falloff), radial from the record, on every piece's
 * momentum): its decl's, and one more a gore wound queues from the hit at
 * the breakable's origin (VK 0x14079E3B0) unless the damage's weapon-specific
 * gore threw the pieces itself (VK 0x1407A06E0, which then answers false);
 * the break (VK 0x14079E6D0) also moves every piece up to 20 units on each
 * axis from where its model places it and turns it at random (VK
 * 0x141758780). destructible(def, spec) names a breakable entityDef of the
 * mod with what its DOOM Eternal decl says: when the game's gore spawns a
 * breakable of that def, the product breaks it with its pieces where the
 * model places them, sets each piece's velocity and spin by DOOM Eternal's
 * law, and, for a decl that ignores the external force, keeps the hit away
 * from the pieces. Speeds are units a second, spins radians a second. The
 * registry is per map; names are whole and compared without case. The cone
 * and thru-square presets are refused (not carried). */
#define SH_AI_DESTRUCT_DEFS        64
#define SH_AI_DESTRUCT_PIECES      32
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
    int   n;                   /* pieces, in the breakable's piece order */
    sh_ai_destruct_piece piece[SH_AI_DESTRUCT_PIECES];
} sh_ai_destruct_spec;
typedef float (*sh_ai_rand_fn)(void *state);   /* uniform in [0, 1) */
/* One piece's velocity (units/s) and spin (rad/s) by DOOM Eternal's law
 * (pure). org and axis: the owner's origin and axis (rows: forward, left,
 * up); ext: the hit's direction ((0, 0, 1) when the decl ignores it), whose
 * frame the cone and the angel wings without inheritParentTransform use;
 * piece: where the piece is. The preset's draws come from rnd. */
void sh_ai_destruct_velocity(const sh_ai_destruct_piece *p, const float org[3], const float axis[9], const float ext[3],
                             const float piece[3], float max_speed, float max_spin, sh_ai_rand_fn rnd, void *rs,
                             float lin[3], float ang[3]);
/* Give a breakable's explosion fx managers (the physics at `phys`) the
 * breakable's own pieces model as the model their render parm items write,
 * where they have none: DOOM Eternal plays a destructible's fx on the
 * destructible's render entity, so an instance tint or fade reaches its
 * pieces; DOOM 2016 starts a breakable's explosion fx on managers without a
 * model, where those items write nothing. -> managers given the model. */
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
    SH_AI_SIG_FSM_STATE_ASK,       /* an AI FSM's request through its state links (the pain and death park) */
    SH_AI_SIG_GAME_TIME,           /* the game's time record an FSM request is given */
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
/* The weapon-use accessor is its whole signature: movsxd rax,[rcx+0x250C8]
 * (the index), mov rdx,[rcx+0xC120] (the behaviour decl), mov rax,[rdx+0x398]
 * (the list's elements; its count is the int after the pointer), 0x50 an
 * entry: the name (a pointer to its text) at +0, the data at +8. */
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
/* Build the pain-string stub at `code` (SH_AI_PAIN_STUB_SIZE bytes): it saves
 * the volatile registers, calls fetch(rbx = the demon) and, when that returns
 * a string, stores it in [rbp+0x88]; then it restores everything and jumps
 * to `resume` (the trampoline that runs the stolen bytes). Pure. */
#define SH_AI_PAIN_STUB_SIZE 192
size_t sh_ai_pain_stub_build(uint8_t *code, size_t cap, const void *fetch, const void *resume);
/* Detour windows, whole and position-independent. */
#define SH_AI_THREAT_STOLEN 15
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
/* The alert cycle the AI is in (alertCycle_t; written only by VK 0x1403DD2C0): ALERTCYCLE_DYING 5, _DEAD 6. The
 * think (VK 0x1403E0FE0) prints "is dead but not ALERTCYCLE_DYING or ALERTCYCLE_DEAD" for a dead AI outside both. */
#define SH_AI_OFF_ALERT_CYCLE       0x21958u
#define SH_AI_ALERT_DYING           5
#define SH_AI_ALERT_DEAD            6
#define SH_AI_CODE_DYING            242
#define SH_AI_PAIN_TAKE_MS          250      /* how long the park stays open for a request the alert cycle refused */
#define SH_AI_PATH_LENGTH_STOLEN 14
/* The footstep table selector's window: two register saves and push rsi,
 * r14, r15 (whole, position-independent). */
/* The rig (SH_AI_SIG_RIG_*). The entry's window: a register save, push rdi,
 * sub rsp and two register moves (whole, position-independent). The runtime
 * holds the rig engine at +0x8; the engine its blocks (0x70 bytes each) and
 * their count; a block its components (0x18 bytes each: object, legs, count)
 * and their count, and a skip bit; a leg of the two-bone component is 0xCC
 * bytes with its joints from +0x80 (24 bits each). The context's pose (+0x50)
 * holds the parents, the joint count, and the local translations and
 * rotations with their strides. The evaluate calls the solve at +0x1ED. */
/* The fk reference pose. Both games aim a demon's fk chain from a reference
 * pose of the effector and its chain: where the effector sits and how its
 * chain is turned when the clip has done its part. The game samples that pose
 * from the animation of the anim web's current leaf, the heavier of a
 * blend's two, and only when there is none from the poseReferenceAnim the
 * effector's fkGraph names (FkReference below). DOOM Eternal does the
 * reverse: the named poseReferenceAnim first, the current leaf only for an
 * effector that names none (its executable, the effector's reference anim
 * load, "Bad reference anim (%s) in FK Graph node"). So in the game the
 * effector's origin jumps when a blend's new clip becomes the heavier one,
 * and the aim and look focus goals, built from that origin, jump with it: a
 * ported demon's head and cannon pop in that tick where DOOM Eternal's turn.
 * fk_reference_pose gives a demon DOOM Eternal's order: while the reference
 * pose of one of its effectors that names a poseReferenceAnim is made, the
 * web's current leaf answers none, so the game takes the named animation. */
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

/* The move transition detour window: push rbp, rsi, rdi, r12-r15;
 * lea rbp,[rsp-0x1640] (whole, position-independent). The turn on the spot
 * is kind 3; byte 0x102 bit 0 of the request lets its clip query take a clip
 * whose root travels (the engine's own "allow transition overshoot"). */
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

/* The animFSM's stage and the animWeb command it waits on (both renderers,
 * 13954591; reflected paths in brackets; tests/script_ai_sigs_test.c checks
 * the code that uses each). The command context is the object the demon's
 * vtable +0x2C8 returns [aiVolatile.animation.awCmdCtx]. */
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
