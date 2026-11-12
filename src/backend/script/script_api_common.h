/* script_api_common.h -- what every module of the mod-script API shares:
 * the engine operations the calls go through, entity arguments with their
 * ownership check, vectors in and out, plain argument checks, engine events,
 * the game clock, and the per-frame animation observer.
 *
 * Every function here runs on the game thread inside the script tick (the
 * only place Lua runs). Argument checks raise a plain error at the calling
 * script line BEFORE any engine call; nothing here raises after one, so a
 * longjmp never crosses an engine frame.
 *
 * The engine is reached only through sh_script_engine (below). The backend
 * fills it at boot (sh_script_api_install); the pure tests (SH_SCRIPT_PURE)
 * install a fake one with sh_script_api_set_engine. So an API module never
 * reads engine memory itself.
 *
 * Build 13954591 engine facts (shared Ghidra, both renderers; evidence in
 * snaphak-re logs/shared-ghidra/SC4-*):
 *   game clock      idGameLocal vtable +0x220 (game, 1) -> int milliseconds (eax, stored as an int by
 *                   idAIHealth::Heal; the slot forwards to the time object at game +0x49D210, vtable +0xC8)
 *   events          ProcessEventArgs VK 0x140B47350 / GL 0x140B46940 (obj, ret[0x18], def, n, a0..a7);
 *                   PostEventArgs VK 0x140B45700 / GL 0x140B44CF0 (obj, def, ms, n, a0...), queued for
 *                   ServiceEvents; event defs by name through the decl manager +0x90, +0x10 FindByName
 *   idAI2           vtable VK 0x142035B30 / GL 0x14204F900; health idAIHealth at +0x2B290 (vtable VK
 *                   0x142035820 / GL 0x14204F5F0): component 0 max float +0x14, current float +0x1C,
 *                   vtable +0x58 Heal(this, component, amount, overflow, allowOverMax)
 *   animWeb (idAI2) web decl at +0x225C8, current node index int16 at +0x2271C (-1 none); web +0x58 node
 *                   array, node +2 subweb index, +4 state-name index; subweb names char* at web +0x70
 *                   + 0x18 + i * 0x38 (count +0x78), state names char* at web +0x88 + i * 8 (count +0x90)
 *   idPlayer        vtable VK 0x1422A1298 / GL 0x1422BAFC8
 *   trace           the trace query VK 0x141637CE0 / GL 0x141638B60 on game +0xE0878 runs at once when
 *                   given a result (trace_t, 0x80 bytes: fraction +0, end +4, normal +0x44, the spawn id
 *                   hit +0x6C), the SnapMap laser hazard's own call shape and contents mask 0x103085 */
#ifndef SH_SCRIPT_API_COMMON_H
#define SH_SCRIPT_API_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include "../signatures.h"
#include "script_runtime.h"
#include "../../third_party/lua/lauxlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the engine */

/* What an entity is, for the calls whose engine path depends on it. */
#define SH_SCRIPT_KIND_GONE   0
#define SH_SCRIPT_KIND_OTHER  1
#define SH_SCRIPT_KIND_AI     2     /* idAI2 */
#define SH_SCRIPT_KIND_PLAYER 3     /* idPlayer */

/* One engine event argument or return value. type is the engine's
 * idEventArg type character:
 *   'i' int (a bool too: the engine's argument copy carries a bool as 'i'),
 *   'f' float, 'v' vector (v), 'a' angles (v: pitch, yaw, roll in degrees),
 *   's' text (s, copied by the engine), 'e' entity (p, an engine entity),
 *   'd' decl (p), '1' animWeb path text (s), '2' joint name text (s),
 *   'E' entity or none (p, NULL for none; sent as 'e'),
 *   '4' no animation (always NULL), 'c' a color (v and w: red, green, blue,
 *   alpha), 'r' any game object (p).
 * A return of type 0 means the receiver did not handle the event. */
typedef struct sh_script_earg {
    char        type;
    int         i;
    float       f;
    float       v[3];
    float       w;
    const char *s;
    void       *p;
} sh_script_earg;

#define SH_SCRIPT_EVENT_ARGS 8

/* A world trace's result. */
typedef struct sh_script_trace {
    float    fraction;   /* 0..1 of the way from start to end; 1 = nothing hit */
    float    end[3];
    float    normal[3];
    uint32_t hit_spawn_id;  /* SH_SCRIPT_SPAWN_ID_NONE when the world (or nothing) was hit */
} sh_script_trace;

/* The engine operations. Each returns 0 (or NULL) when the engine could not
 * answer, and never raises. Entities are engine pointers resolved this
 * frame; none is kept. */
typedef struct sh_script_engine {
    void    *(*resolve)(uint32_t spawn_id);         /* entity a spawn id names now, or NULL */
    uint32_t (*spawn_id)(void *ent);                /* SH_SCRIPT_SPAWN_ID_NONE when unreadable */
    int      (*slots)(void);                        /* entity numbers to scan (players first) */
    uint32_t (*slot_spawn_id)(int number);          /* spawn id of an entity number, NONE when free */
    int      (*kind)(void *ent);                    /* SH_SCRIPT_KIND_* */
    int      (*origin)(void *ent, float out[3]);
    int      (*axis)(void *ent, float out[9]);      /* rows: forward, left, up */
    int      (*place)(void *ent, const float *origin, const float *axis);   /* either may be NULL */
    int      (*def_name)(void *ent, char *out, size_t cap);
    /* run (post 0) or queue for this frame's event service (post 1) */
    int      (*event)(void *ent, const char *name, const sh_script_earg *args, int n, sh_script_earg *ret, int post);
    int      (*ai_health)(void *ai, float *cur, float *max);
    int      (*ai_heal)(void *ai, float amount);
    int      (*ai_set_health)(void *ai, float value);   /* lowers only; value >= 1 */
    char    *(*anim_node)(void *ai);   /* "subweb/state" now playing, whole (malloc'd; free); NULL when none */
    void    *(*spawn)(const char *def, const float origin[3], float yaw, const char **why);
    int      (*trace)(const float from[3], const float to[3], uint32_t ignore_spawn_id, sh_script_trace *out);
    int      (*game_time_ms)(int *ms);
    /* The floor under pos (1 found, with the point in out). */
    int      (*ground)(const float pos[3], float out[3]);
    /* The game difficulty, 0 easy .. 4 nightmare (1 when read). */
    int      (*difficulty)(int *level);
    /* The md6Def tag `name` of a demon (a named point on a parent joint with
     * its own offset and rotation) in world space: 1 with its position and
     * axis (rows forward, left, up), 0 when the demon's model has no such
     * tag (or the entity is not a demon), -1 unreadable. */
    int      (*tag)(void *ent, const char *name, float pos[3], float axis[9]);
    /* The argument format of the engine event `name` ("" when it takes
     * none): 1, or 0 when the game has no such event. */
    int      (*event_format)(const char *name, char *format, size_t cap);
} sh_script_engine;

/* The floor under pos made of the engine's world trace: straight down from
 * SH_SCRIPT_GROUND_ABOVE inches above pos for SH_SCRIPT_GROUND_DEPTH inches;
 * the first surface hit counts when it is flat enough to stand on (its normal
 * at least SH_SCRIPT_GROUND_FLAT up). 1 with the point; 0 none. Pure (the
 * engine side answers ground with it; the tests run it over a fake trace). */
#define SH_SCRIPT_GROUND_ABOVE 16.0f
#define SH_SCRIPT_GROUND_DEPTH 4096.0f
#define SH_SCRIPT_GROUND_FLAT  0.7f
int sh_script_api_ground_trace(int (*trace)(const float from[3], const float to[3], uint32_t ignore_spawn_id,
                                            sh_script_trace *out),
                               const float pos[3], float out[3]);

void sh_script_api_set_engine(const sh_script_engine *engine);
const sh_script_engine *sh_script_api_engine(void);

/* Boot (the backend): resolve the private signatures and the decl registry
 * from the shared results, fill the engine table and register every call of
 * the entity, animation, world and time groups. Commits nothing. Returns 1
 * when the engine side is usable; 0 with one log line when not (the calls
 * then answer nil). */
int sh_script_api_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The private signatures the engine side resolves (NULL-terminated), for the
 * two-image test. */
extern const sig_entry SH_SCRIPT_API_SIGNATURES[];

/* Add the calls of script_api_entity.c and script_api_world.c to every state
 * opened from now on (sh_script_api_add). Idempotent. */
void sh_script_api_register(void);
void sh_script_api_entity_register(void);
void sh_script_api_world_register(void);

/* ---------------------------------------------------------- the frame */

/* Once per host game frame of a Play, before sh_script_tick: observe the
 * animation of every entity whose script asked for one (anim_event,
 * anim_finished). */
void sh_script_api_frame(void);
/* Run fn at the end of every sh_script_api_frame (the AI calls finish a
 * forced animation through a via node there). Up to 4; idempotent. */
void sh_script_api_on_frame(void (*fn)(void));

/* The engine's own entity table: the game holds at most
 * SH_SCRIPT_ENGINE_ENTITIES entities at once (MAX_GENTITIES less the world
 * and none), and one more is a fatal engine error ("No free managed
 * instances"). A script's spawn or fire therefore needs a free entity slot
 * beyond SH_SCRIPT_ENGINE_RESERVE kept for the game's own spawns (players,
 * projectiles, effects, demons). The free count is read once a frame
 * (sh_script_api_frame) and each spawn or launch a script asks for takes one.
 * sh_script_api_check_room raises "<call> cannot add an entity now: the game
 * holds at most 16382 entities at once and nearly all are in use; remove some
 * first" when there is no room; it never refuses for any other reason. */
#define SH_SCRIPT_ENGINE_ENTITIES 16382
#define SH_SCRIPT_ENGINE_RESERVE  256
void sh_script_api_check_room(lua_State *L, const char *call);

/* The game clock in milliseconds (idGameLocal vtable +0x220). 1 when read. */
int sh_script_api_now_ms(int *ms);

/* ----------------------------------------------------------- arguments */

/* The engine entity behind the entity object at `arg`: NULL when it was
 * removed or is gone (the caller returns nil). Raises a plain error naming
 * `call` when the value is not an entity. *h (may be NULL) gets the handle.
 * An entity this frame's spawn just made (not yet delivered) resolves too. */
void *sh_script_api_entity(lua_State *L, int arg, const char *call, sh_script_handle *h);

/* The same, and the entity must be this mod's own (placed from its
 * entityDefs or spawned by its scripts): another entity raises
 * "<call> can only <verb> this mod's own entities; this one is not". */
void *sh_script_api_owned(lua_State *L, int arg, const char *call, const char *verb, sh_script_handle *h);

/* 1 when `h` is this mod's own entity. */
int sh_script_api_owns(lua_State *L, sh_script_handle h);

/* A position: a table {x=, y=, z=} (or {x, y, z}) of numbers. Raises
 * "<call> needs <what> (a position with x, y and z), got <type>". */
void sh_script_api_check_vec(lua_State *L, int arg, const char *call, const char *what, float out[3]);
/* The same, or 0 when the argument is absent or nil. */
int  sh_script_api_opt_vec(lua_State *L, int arg, const char *call, const char *what, float out[3]);
/* A position or an entity (its position). 0 when the entity was removed. */
int  sh_script_api_check_point(lua_State *L, int arg, const char *call, const char *what, float out[3]);

/* A finite number. Raises "<call> needs <what> as a number, got <type>". */
double sh_script_api_check_number(lua_State *L, int arg, const char *call, const char *what);
/* The same, or 0 when absent or nil (then *out is untouched). */
int  sh_script_api_opt_number(lua_State *L, int arg, const char *call, const char *what, double *out);
/* Text of at most max_len bytes. */
const char *sh_script_api_check_text(lua_State *L, int arg, const char *call, const char *what, size_t max_len);

/* A new table {x=, y=, z=}. */
void sh_script_api_push_vec(lua_State *L, const float v[3]);

/* Push the script object for an engine entity: its handle (a mod's entity
 * keeps its own; any other gets a reference no mod owns), or nil when the
 * entity is NULL or the handle table is full. */
void sh_script_api_push_engine_entity(lua_State *L, void *ent);
/* The handle for an engine entity (recording a reference when needed), or 0. */
sh_script_handle sh_script_api_handle_of(void *ent);

/* ---------------------------------------------------------- engine use */

/* The entity's kind (SH_SCRIPT_KIND_*). */
int sh_script_api_kind(void *ent);

/* Run an event on an entity now, with the result in *ret (may be NULL).
 * 1 when the engine ran it (ret->type 0: the receiver did not answer). */
int sh_script_api_event(void *ent, const char *name, const sh_script_earg *args, int n, sh_script_earg *ret);
/* Queue an event for this frame's event service. 1 when queued. */
int sh_script_api_post(void *ent, const char *name, const sh_script_earg *args, int n);

/* Argument builders. */
sh_script_earg sh_script_earg_int(int v);
sh_script_earg sh_script_earg_float(float v);
sh_script_earg sh_script_earg_vec(const float v[3]);
sh_script_earg sh_script_earg_text(char type, const char *s);   /* 's' or '1' */
sh_script_earg sh_script_earg_entity(void *ent);
sh_script_earg sh_script_earg_no_anim(void);

/* --------------------------------------------- animation observation */

/* The request a script made of an entity's animWeb (anim_state, anim_force),
 * observed every frame from the node now playing. */
#define SH_SCRIPT_ANIM_VIA_IN    0x01u   /* the via node became the node playing */
#define SH_SCRIPT_ANIM_VIA_OUT   0x02u   /* after the via node, another node played */
#define SH_SCRIPT_ANIM_DEST      0x04u   /* the destination node became the node playing */
#define SH_SCRIPT_ANIM_FINISHED  0x08u   /* the destination played and the web moved on */

void sh_script_api_anim_request(sh_script_handle h, const char *dest, const char *via);
/* The event bits of h's last request; *requested 0 when there was none. */
unsigned sh_script_api_anim_flags(sh_script_handle h, void *ent, int *requested);
/* The same destination, in place (valid until the next request or frame), or NULL. */
char *sh_script_api_anim_dest_of(sh_script_handle h);
/* Observe one entity now (also done every frame). */
void sh_script_api_anim_observe(sh_script_handle h, void *ent);
/* Forget every request (a new map). */
void sh_script_api_anim_reset(void);

#ifdef __cplusplus
}
#endif
#endif
