/* script_hooks.h -- where mod scripts meet the engine: the tick, the map
 * lifecycle, spawned entities, and the queues every other hook writes to.
 *
 * Threads (build 13954591, both renderers, decompiled in shared Ghidra; the
 * evidence is snaphak-re logs/shared-ghidra/sc3-*):
 *   - The tick is the SuperScript per-frame wrapper (ScriptTick, script_sigs.c).
 *     Its only caller is idGameLocal::RunFrame (VK 0x1403AE730, GL 0x1403ADE50,
 *     idGameLocal vtable slot +0x08), after the entity think loop. RunFrame is
 *     reached from idCommonLocal::Frame (VK 0x14159D930, GL 0x14159E9A0)
 *     through the host's game-frame loop (VK 0x1415A1BF0, GL 0x1415A2C60, the
 *     "RunGameFrame" profile scope; one or more game frames per Frame) and
 *     through the two client frame loops (VK 0x1415A1490 and 0x1415A0FF0),
 *     which call the same slot. Frame is DOOM's main thread, so the tick runs
 *     on the main thread, on clients as well as on the host: the tick refuses
 *     to run scripts on a client (idGameLocal::IsClient).
 *   - idClass::CallSpawn runs on the main thread (companion_engine.h): map
 *     entities from SpawnEntitiesForLayers during the load, later spawns from
 *     game code inside RunFrame.
 *   - idCommonLocal::CancelMapChange (common vtable +0x28) runs on the main
 *     thread: from Frame when a session leaves its map, and from Frame's catch
 *     after a failed load (load_screen_stop.h).
 *   - The map gates are called directly by the serving layer on the main
 *     thread, just before it arms the companions detour: the map load
 *     (modserve_activate_bundles, every SnapMap map parse: the editor's load,
 *     a My Maps or published launch at the hub) and the Play build
 *     (sh_modserve_play_begin from the SnapBuild detour, inside the level
 *     load of every Play: the editor's Play, a My Maps play, a published map
 *     from the browser or the feed, a lobby's host), before that level's
 *     spawns.
 *   - Damage, death and animation notify hooks may run on AI prethink and
 *     animWeb job threads; they only enqueue (script_queue.h).
 *
 * Lifecycle. At the map-load gate the previous map's scripts end. At the
 * Play gate they end too and the runtime prepares the served mods that carry
 * scripts (sh_script_map_open; no script code runs there). Spawns of those
 * mods' entities are recorded in the handle table from then on. Each host
 * game frame of that Play the tick calls the runtime once (sh_script_tick),
 * never re-entrantly, never while a map load draws its loading screen, never
 * on a client. Before the runtime's first frame the tick walks the live
 * entities once and records every entity of a scripted mod the spawn chain
 * did not (sh_script_hooks_walk_live); the runtime then runs the mods' files
 * and map_start handlers, then the handle table's spawns and the queues. The
 * map ends at the next gate, when the editor is active again (a return from
 * the editor's Play: end handlers run), when a loading screen draws after the
 * scripts started (the session left the map: no end handlers), or at
 * CancelMapChange (end handlers run unless a load was in progress).
 *
 * In Play is read from the editor's active flag (idSnapEditorLocal +9, clear
 * during every SnapMap Play, set while editing): a published or My Maps play
 * leaves the editor's deactivate reason at 0, so the serving layer's
 * sh_modserve_engine_in_play (reason 3, the editor's own Play) is 0 there and
 * cannot gate scripts.
 *
 * The tick detour is committed only when a Play opens at least one mod, at
 * the gate on the main thread with the prologue verified, and is never
 * unhooked. */
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

/* The runtime's side (script_runtime.h): sh_script_map_open at the Play gate,
 * sh_script_tick(now, dt) once per host game frame of that Play,
 * sh_script_map_close(run_end_handlers) when it ends. */

/* ---- the mods of this map (called by the runtime inside map_open) ---- */

#define SH_SCRIPT_SALT_MAX 16

/* Register a mod with scripts for this map. Returns its slot (from 0; the
 * table grows with the map's mods), the existing slot for a salt already
 * registered, or -1 (bad salt, or out of memory: its line is written). The
 * slot is the `mod` of the queue and handle APIs. */
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

/* Record an entity of a scripted mod: the owner of names[0..count-1]
 * (sh_script_hooks_owner_of), its spawn id and its entityDef chain (the
 * names in order, lower case, a newline between them) go into the handle
 * table. Returns the handle (the existing one when already recorded), or 0
 * when no scripted mod owns the names or the table is full (*full set).
 * Pure; the spawn subscriber and the live walk call it. */
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
#define SH_SCRIPT_TICK_END        2   /* Play ended after scripts started (the editor is back): close
                                         with end handlers */
#define SH_SCRIPT_TICK_CLIENT     3   /* a client: close the states once, without handlers */
#define SH_SCRIPT_TICK_LEFT       4   /* a loading screen draws after the scripts started: the session
                                         left the map and its entities are going, so close without
                                         end handlers (a handle could name another map's entity) */
#define SH_SCRIPT_TICK_IDLE       0   /* no scripts this map */
#define SH_SCRIPT_TICK_REENTERED (-1)
#define SH_SCRIPT_TICK_OFF_MAIN  (-2)
#define SH_SCRIPT_TICK_UNKNOWN   (-3) /* host, load or Play state unreadable: run nothing */
#define SH_SCRIPT_TICK_LOADING   (-4) /* the Play's own level load: scripts wait for it */
#define SH_SCRIPT_TICK_NOT_PLAY  (-5) /* the editor: scripts wait for Play */
int  sh_script_tick_decide(const sh_script_tick_state *s);
const char *sh_script_tick_name(int decision);

/* The map gates. MAP_LOAD: a SnapMap map was parsed (the editor's load, a
 * launch at the hub): the previous map's scripts end, nothing opens. PLAY:
 * the level of a Play is being built: the previous scripts end and this
 * Play's mods are prepared. */
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

/* The private signature table (NULL-terminated, enum order), for the
 * two-image test. */
extern const sig_entry SH_SCRIPT_SIGNATURES[];

/* A signature whose bytes repeat in a sibling function that differs only in
 * the callback-name string it loads with `lea rdx,[rip+disp32]` at
 * lea_offset: resolved when exactly one match loads `callback`. */
typedef struct sh_script_named_sig {
    sig_entry sig;
    size_t lea_offset;
    const char *callback;
} sh_script_named_sig;
extern const sh_script_named_sig SH_SCRIPT_ANIM_NOTIFY_SIGNATURE;

/* Resolve a named signature over the image's executable sections (a mapped
 * module or an image laid out by RVA). Returns SIG_OK, SIG_NOT_FOUND,
 * SIG_AMBIGUOUS, SIG_BAD_PATTERN or SIG_BAD_MODULE. */
sig_status sh_script_resolve_named(const uint8_t *module_base, const sh_script_named_sig *s, sig_result *out);

/* Detour windows (whole, position-independent instructions; the test proves
 * them on both executables). */
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

/* Boot: resolve the signatures, the globals and the dependencies, verify the
 * tick prologue, and subscribe to the spawn chain and CancelMapChange.
 * Commits nothing. Returns 1 when the scripts' engine side is usable on this
 * executable; 0 with its one log line when not (mod scripts then never run). */
int  sh_script_hooks_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The map gates (main thread), called by the serving layer
 * (modserve_engine.c) before it arms the companions detour, so a Play that
 * opens a scripted mod arms the spawn chain in the same pass. */
void sh_script_hooks_gate(int gate);

/* Before the first frame of a Play: record every live entity of a scripted
 * mod (an entity number's spawn id resolved through the engine, its entityDef
 * chain read as the spawn subscriber reads it). Placed entities spawn during
 * the level load; the spawn chain records them, and this catches any it did
 * not see. Returns the entities it recorded that were not recorded yet.
 * Main thread. */
int  sh_script_hooks_walk_live(void);

/* Reload this map's scripts (the console's sh_scripts reload, main thread,
 * outside the frame): while a Play runs them, end them with their end
 * handlers, prepare them again from the files and record the live entities,
 * so they start again at the next frame. Returns the mods prepared, 0 when
 * no Play is running scripts. */
int  sh_script_hooks_reload(void);

/* 1 while a Play has mods with scripts open (prepared or running). */
int  sh_script_hooks_open(void);

/* The CallSpawn subscriber (main thread): `entity` is the idEntity, names its
 * entityDef's name then its inherits. */
void sh_script_hooks_after_spawn(void *entity, const char *const *names, int count);

/* 1 while this map has states, so the spawn chain must be watched. */
int  sh_script_hooks_spawn_wanted(void);

/* The CancelMapChange subscriber (main thread). during_load: the loading
 * screen was still drawing (a failed load). */
void sh_script_hooks_map_cancel(int during_load);

/* For hooks on any thread: the mod and handle a spawned entity was recorded
 * under, or 0 when scripts do not run here (no states, a client, or not one of
 * a scripted mod's entities). Reads the entity's spawn id; never resolves. */
sh_script_handle sh_script_hooks_handle_of(void *entity, int *mod);

#ifdef SH_SCRIPT_PURE
/* Tests only: empty the mod table. */
void sh_script_hooks_test_reset(void);
/* Tests only: pretend a Play runs the mods (1) or not (0): sh_script_hooks_open
 * answers it and sh_script_hooks_reload prepares the mods again from the
 * provider, as the engine side does. */
void sh_script_hooks_test_play(int on);
#endif

/* Game frames the tick has run scripts in this map. */
uint32_t sh_script_hooks_frame(void);

#ifdef __cplusplus
}
#endif
#endif
