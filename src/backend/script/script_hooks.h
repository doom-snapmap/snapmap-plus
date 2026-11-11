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
 *   - The map gates (sh_script_hooks_gate) are the serving layer's main-thread
 *     gates that already arm the companions detour: the map load ("map") and
 *     the Play start ("play"), both before that map's spawns.
 *   - Damage, death and animation notify hooks may run on AI prethink and
 *     animWeb job threads; they only enqueue (script_queue.h).
 *
 * Lifecycle. At a map gate the previous map's scripts end and the runtime
 * opens one state per served mod that carries scripts (sh_script_map_open;
 * no script code runs there). Spawns of those mods' entities are recorded in
 * the handle table from then on. Each host game frame during SnapMap Play the
 * tick calls the runtime once (sh_script_tick), never re-entrantly, never while
 * a map load draws its loading screen, never on a client. The runtime's first
 * tick of a map runs the mods' files and map_start handlers, then the handle
 * table's spawns and the queues. The map ends at the next gate, when Play ends
 * (a tick sees Play over: end handlers run), or at CancelMapChange (end
 * handlers run unless a load was in progress).
 *
 * The tick detour is committed only when a map opens at least one state, at a
 * gate on the main thread with the prologue verified, and is never unhooked. */
#ifndef SH_SCRIPT_HOOKS_H
#define SH_SCRIPT_HOOKS_H
#include <stddef.h>
#include <stdint.h>
#include "../signatures.h"
#include "script_handles.h"
#include "script_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented by the runtime (script_runtime.c) -------------------- */

/* Main thread, at a map gate, before the map's spawns. Create one state per
 * served mod that carries scripts and register each with
 * sh_script_hooks_mod_add; run no script code. Returns the states opened (0:
 * this map has no scripts), or -1 when the runtime could not open them (it has
 * written the reason). */
int  sh_script_map_open(void);

/* Main thread, once per host game frame during Play, never re-entrantly,
 * never during a load. Delivers, per mod: on the first call of a map, the
 * files and map_start; then each sh_script_handles_sweep change (spawn,
 * remove), the queue (sh_script_queue_drain), think and due timers. */
void sh_script_tick(void);

/* Main thread. The map ended: run map_end handlers (within their budget)
 * when run_end_handlers is 1, then close every state. The hooks layer has
 * stopped routing records to them before this call. */
void sh_script_map_close(int run_end_handlers);

/* ---- the mods of this map (called by the runtime inside map_open) ---- */

#define SH_SCRIPT_SALT_MAX 16
#define SH_SCRIPT_NAME_MAX 96

/* Register a mod with scripts for this map. Returns its slot (0 ..
 * SH_SCRIPT_MODS_MAX-1), the existing slot for a salt already registered, or
 * -1 (no free slot, bad salt, or out of memory: its line is written). The slot
 * is the `mod` of the queue and handle APIs. */
int  sh_script_hooks_mod_add(const char *salt, const char *display_name);
int  sh_script_hooks_mod_count(void);
const char *sh_script_hooks_mod_salt(int mod);   /* NULL when the slot is free */
const char *sh_script_hooks_mod_name(int mod);

/* The slot of the first name under "mods/<salt>/" (ASCII case-insensitive,
 * either separator) whose salt is registered, scanning names in order: an
 * entityDef's own name, then its inherits. -1 when none. Pure. */
int  sh_script_hooks_owner_of(const char *const *names, int count);

/* ---- the tick's guards (pure) ------------------------------------------ */

typedef struct sh_script_tick_state {
    int open;        /* states this map opened (> 0: scripts exist) */
    int in_tick;     /* the tick is already running on this thread */
    int on_main;     /* 1 DOOM's main thread, 0 another, -1 unknown */
    int is_client;   /* 1 a network client, 0 the host or single player, -1 unknown */
    int loading;     /* 1 a map load is drawing its loading screen, 0 not, -1 unknown */
    int in_play;     /* 1 SnapMap Play, 0 the editor or a menu, -1 unknown */
    int started;     /* the runtime has had a tick this map */
} sh_script_tick_state;

#define SH_SCRIPT_TICK_RUN        1   /* call sh_script_tick */
#define SH_SCRIPT_TICK_END        2   /* Play ended after scripts started: close with end handlers */
#define SH_SCRIPT_TICK_CLIENT     3   /* a client: close the states once, without handlers */
#define SH_SCRIPT_TICK_IDLE       0   /* no scripts this map */
#define SH_SCRIPT_TICK_REENTERED (-1)
#define SH_SCRIPT_TICK_OFF_MAIN  (-2)
#define SH_SCRIPT_TICK_UNKNOWN   (-3) /* host, load or Play state unreadable: run nothing */
#define SH_SCRIPT_TICK_LOADING   (-4)
#define SH_SCRIPT_TICK_NOT_PLAY  (-5) /* the editor: scripts wait for Play */
int  sh_script_tick_decide(const sh_script_tick_state *s);
const char *sh_script_tick_name(int decision);

/* What a gate phase means for scripts: 1 a new map (end the old, open the
 * new), 0 nothing. "map" and "play" are new maps. Pure. */
int  sh_script_gate_opens(const char *phase);

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
 * tick prologue, and subscribe to the spawn chain, the map gates and
 * CancelMapChange. Commits nothing. Returns 1 when the scripts' engine side is
 * usable on this executable. */
int  sh_script_hooks_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* The map gates (main thread), called by the companions' gate fan-out. */
void sh_script_hooks_gate(const char *phase);

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
#endif

/* Game frames the tick has run scripts in this map. */
uint32_t sh_script_hooks_frame(void);

#ifdef __cplusplus
}
#endif
#endif
