/* script_hooks.h -- where mod scripts meet the engine: the tick, the map lifecycle, spawned entities, and the
 * queues every other hook writes to. */
/* Threads (build 13954591): the tick is ScriptTick (script_sigs.c), called only from idGameLocal::RunFrame
 * (VK 0x1403AE730, GL 0x1403ADE50) on the main thread, clients included, so it refuses to run on a client. RunFrame
 * is reached from idCommonLocal::Frame (VK 0x14159D930, GL 0x14159E9A0) through the host game-frame loop (VK
 * 0x1415A1BF0, GL 0x1415A2C60) and the client loops (VK 0x1415A1490, 0x1415A0FF0). Spawns, CancelMapChange and
 * the map gates run on the main thread; damage, death and animation notify hooks may run on job threads and only
 * enqueue (script_queue.h). */
/* Lifecycle: the map-load and Play gates end the previous map's scripts, and the Play gate prepares the served
 * mods that carry scripts (sh_script_map_open) and records their spawns. Each host game frame of that Play the
 * tick calls sh_script_tick once, never re-entrantly, during a loading screen or on a client, after
 * sh_script_hooks_walk_live records entities the spawn chain missed. The map ends at the next gate, an editor
 * return (end handlers run), a loading screen after start (none run), or CancelMapChange (run unless a load
 * was in progress). */
/* In Play is read from the editor's active flag (idSnapEditorLocal +9, clear during every SnapMap Play), because
 * a published or My Maps play leaves the deactivate reason at 0 and sh_modserve_engine_in_play cannot gate it.
 * The tick detour is committed only when a Play opens a mod, with the prologue verified, and is never unhooked. */
#ifndef SH_SCRIPT_HOOKS_H
#define SH_SCRIPT_HOOKS_H
#include <stddef.h>
#include <stdint.h>
#include "../signatures.h"
#include "script_handles.h"
#include "script_queue.h"
#include "script_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the mods of this map (called by the runtime inside map_open) ---- */

#define SH_SCRIPT_SALT_MAX 16

/* Register a mod with scripts for this map: its slot (the `mod` of the queue and handle APIs), the existing
 * slot for a registered salt, or -1 on a bad salt or out of memory (a line is written). */
int  sh_script_hooks_mod_add(const char *salt, const char *display_name);
int  sh_script_hooks_mod_count(void);
/* The slot of a registered salt (ASCII case-insensitive), or -1. */
int  sh_script_hooks_mod_find(const char *salt);
/* Empty the mod table and close every queue (the runtime, preparing a map). */
void sh_script_hooks_mods_reset(void);
const char *sh_script_hooks_mod_salt(int mod);   /* NULL when the slot is free */
const char *sh_script_hooks_mod_name(int mod);

/* The slot of the first name under "mods/<salt>/" (ASCII case-insensitive,
 * either separator) whose salt is registered, scanning names in order: an
 * entityDef's own name, then its inherits. -1 when none. Pure. */
int  sh_script_hooks_owner_of(const char *const *names, int count);

/* Record an entity of a scripted mod in the handle table: its owner (sh_script_hooks_owner_of), spawn id and
 * entityDef chain (lower case names, newline-separated). Returns the handle (existing if recorded), or 0 when no
 * scripted mod owns the names or the table is full (*full set). Pure. */
sh_script_handle sh_script_hooks_record(uint32_t spawn_id, const char *const *names, int count, int *full);

/* ---- the tick's guards (pure) ------------------------------------------ */

typedef struct sh_script_tick_state {
    int open;        /* mods this Play prepared (> 0: scripts exist) */
    int in_tick;     /* the tick is already running on this thread */
    int on_main;     /* 1 DOOM's main thread, 0 another, -1 unknown */
    int is_client;   /* 1 a network client, 0 the host or single player, -1 unknown */
    int loading;     /* 1 a map load is drawing its loading screen, 0 not, -1 unknown */
    int in_play;     /* 1 the editor is inactive (SnapMap Play of any kind), 0 the editor is active, -1 unknown */
    int started;     /* the runtime has had a tick this map */
} sh_script_tick_state;

#define SH_SCRIPT_TICK_RUN        1   /* call sh_script_tick */
#define SH_SCRIPT_TICK_END        2   /* the editor is back after scripts started: close with end handlers */
#define SH_SCRIPT_TICK_CLIENT     3   /* a client: close the states once, without handlers */
#define SH_SCRIPT_TICK_LEFT       4   /* a loading screen drew after start: close without end handlers */
#define SH_SCRIPT_TICK_IDLE       0   /* no scripts this map */
#define SH_SCRIPT_TICK_REENTERED (-1)
#define SH_SCRIPT_TICK_OFF_MAIN  (-2)
#define SH_SCRIPT_TICK_UNKNOWN   (-3) /* host, load or Play state unreadable: run nothing */
#define SH_SCRIPT_TICK_LOADING   (-4) /* the Play's own level load: scripts wait for it */
#define SH_SCRIPT_TICK_NOT_PLAY  (-5) /* the editor: scripts wait for Play */
int  sh_script_tick_decide(const sh_script_tick_state *s);
const char *sh_script_tick_name(int decision);

/* The map gates. MAP_LOAD (a SnapMap map was parsed) ends the previous map's scripts; PLAY (a Play's level is
 * being built) ends them and prepares this Play's mods. */
#define SH_SCRIPT_GATE_MAP_LOAD 1
#define SH_SCRIPT_GATE_PLAY     2
/* 1 when the gate opens the map's mods, 0 when it only ends the last ones. Pure. */
int  sh_script_gate_opens(int gate);

/* ---- engine side --------------------------------------------------------- */

enum sh_script_sig {
    SH_SCRIPT_SIG_TICK, SH_SCRIPT_SIG_SPAWN_ID_LOOKUP, SH_SCRIPT_SIG_IS_CLIENT,
    SH_SCRIPT_SIG_ON_DAMAGE, SH_SCRIPT_SIG_KILLED, SH_SCRIPT_SIG_RELAY_DOACTION,
    SH_SCRIPT_SIG_COUNT
};

/* The private signature table (NULL-terminated, enum order). */
extern const sig_entry SH_SCRIPT_SIGNATURES[];

/* A signature whose bytes repeat in a sibling function differing only in the callback name it loads at
 * lea_offset (`lea rdx,[rip+disp32]`): resolved when exactly one match loads `callback`. */
typedef struct sh_script_named_sig {
    sig_entry sig;
    size_t lea_offset;
    const char *callback;
} sh_script_named_sig;
extern const sh_script_named_sig SH_SCRIPT_ANIM_NOTIFY_SIGNATURE;

/* Resolve a named signature over the image's executable sections (a mapped module or an RVA-laid image):
 * SIG_OK, SIG_NOT_FOUND, SIG_AMBIGUOUS, SIG_BAD_PATTERN or SIG_BAD_MODULE. */
sig_status sh_script_resolve_named(const uint8_t *module_base, const sh_script_named_sig *s, sig_result *out);

/* Detour windows (whole, position-independent instructions). */
#define SH_SCRIPT_TICK_STOLEN          15
#define SH_SCRIPT_ON_DAMAGE_STOLEN     15
#define SH_SCRIPT_ON_ANIM_STOLEN       15
#define SH_SCRIPT_KILLED_STOLEN        21
#define SH_SCRIPT_RELAY_STOLEN         16
static const uint8_t k_script_tick_prologue[SH_SCRIPT_TICK_STOLEN] = {
    0x40, 0x57,                                      /* push rdi                      */
    0x48, 0x83, 0xEC, 0x40,                          /* sub  rsp,0x40                 */
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF /* mov qword [rsp+0x20],-2   */
};

/* Engine offsets the handle scheme reads (both renderers). */
#define SH_SCRIPT_ENT_NUMBER        0x08u     /* idEntity: entity number */
#define SH_SCRIPT_GAME_SPAWN_IDS    0x653F8u  /* idGameLocal: uint32 spawn id per entity number */
#define SH_SCRIPT_ENT_NUMBER_WORLD  0x3FFE
#define SH_SCRIPT_GAME_LOOKUP_SLOT  0x7F0u    /* idGameLocal vtable: spawn id -> object; +8: then the idEntity cast */

/* Boot: resolve the signatures and globals, verify the tick prologue, and subscribe to the spawn chain and
 * CancelMapChange; commits nothing. Returns 1 when usable on this executable, else 0 with one log line. */
int  sh_script_hooks_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The map gates (main thread), called by modserve_engine.c before it arms the companions detour, so a Play
 * that opens a scripted mod arms the spawn chain in the same pass. */
void sh_script_hooks_gate(int gate);

/* Before the first frame of a Play: record every live entity of a scripted mod that the spawn chain did not.
 * Returns how many it newly recorded. Main thread. */
int  sh_script_hooks_walk_live(void);

/* Reload this map's scripts (console sh_scripts reload, main thread, outside the frame): end them with their
 * end handlers, prepare them again from the files and record the live entities. Returns the mods prepared, 0
 * when no Play is running scripts. */
int  sh_script_hooks_reload(void);

/* 1 while a Play has mods with scripts open (prepared or running). */
int  sh_script_hooks_open(void);

/* The CallSpawn subscriber (main thread): `entity` is the idEntity, names its
 * entityDef's name then its inherits. */
void sh_script_hooks_after_spawn(void *entity, const char *const *names, int count);

/* 1 while this map has states, so the spawn chain must be watched. */
int  sh_script_hooks_spawn_wanted(void);

/* The CancelMapChange subscriber (main thread); during_load: the loading screen was still drawing. */
void sh_script_hooks_map_cancel(int during_load);

/* For hooks on any thread: the mod and handle a spawned entity was recorded under, or 0 when scripts do not
 * run here. Reads the entity's spawn id; never resolves. */
sh_script_handle sh_script_hooks_handle_of(void *entity, int *mod);
/* 1 on DOOM's main thread, the only thread scripts run on. */
int sh_script_hooks_on_game_thread(void);
/* 1 during any SnapMap Play (the editor's active flag is clear), 0 while the editor is active, -1 unknown.
 * Any thread; reads one byte. */
int sh_script_hooks_in_play(void);

#ifdef SH_SCRIPT_PURE
/* Tests only: empty the mod table. */
void sh_script_hooks_test_reset(void);
/* Tests only: pretend a Play runs the mods (1) or not (0), for sh_script_hooks_open and _reload. */
void sh_script_hooks_test_play(int on);
#endif

/* Game frames the tick has run scripts in this map. */
uint32_t sh_script_hooks_frame(void);

/* Send one captured damage notification to the victim's and credited attacker's respective owners. */
void sh_script_hooks_queue_damage(int victim_mod, int source_mod, const sh_script_event *damage, const char *decl);

/* A nested hit on the same player accounts for its own health and armor loss. */
typedef struct sh_script_player_hit {
    void *player;
    float before, nested;
    struct sh_script_player_hit *outer;
} sh_script_player_hit;
float sh_script_player_hit_loss(const sh_script_player_hit *hit, float after);

#ifdef __cplusplus
}
#endif
#endif
