/* script_handles.h -- the entity handles mod scripts hold.
 *
 * A script never holds an engine pointer. The spawn subscriber (script_hooks.c,
 * on idClass::CallSpawn) records each spawned entity of a mod with scripts by
 * its engine spawn id, the 32-bit value an idEntityPtr keeps: the entity's
 * number in the low bits and the spawn count of that entity slot in the high
 * bits, read from idGameLocal +0x653F8 + 4 * entity number, with the entity
 * number at idEntity +0x08 (both renderers, build 13954591). The engine
 * resolves a spawn id back to the object through idGameLocal vtable +0x7F0
 * (VK 0x140393220, GL 0x140392AF0), which returns NULL once the entity is
 * gone or its slot was reused, because the stored spawn count no longer
 * matches; slot +0x7F8 (VK 0x1403912A0, GL 0x140390B70) is that lookup
 * followed by the idEntity cast, and is what the engine side calls. A handle is that record's index in this table plus the map's epoch,
 * so a handle kept past the map end, or one that never existed, resolves to
 * nothing instead of to a reused slot.
 *
 * Every tick the table is swept (sh_script_handles_sweep): a new entry whose
 * spawn id resolves becomes live and is reported as a spawn; a live entry
 * whose spawn id no longer resolves is reported once as a removal and stays
 * dead. Every API call resolves the handle again (sh_script_handle_entity), so
 * a removed entity is noticed at once and nothing dangles. Entries are never
 * reused within a map, so handle order is spawn order.
 *
 * Threads: add, sweep and entity run on the main thread (the spawn chain and
 * the tick); find and mod are safe on any thread (hooks on job threads use
 * them to route a record). Pure C; the resolver is injected, so the tests use
 * a fake one. */
#ifndef SH_SCRIPT_HANDLES_H
#define SH_SCRIPT_HANDLES_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sh_script_handle;   /* 0 = no entity */

#define SH_SCRIPT_HANDLES_MAX   8192         /* recorded entities per map, all mods */
#define SH_SCRIPT_SPAWN_ID_NONE  0x1FFFFFEu  /* the engine's unset spawn id */
#define SH_SCRIPT_SPAWN_ID_WORLD 0x1FFFFFDu  /* the world entity's spawn id */

/* The engine's lookup: the entity a spawn id names now, or NULL. */
typedef void *(*sh_script_resolve_fn)(uint32_t spawn_id);

typedef enum sh_script_handle_change_kind {
    SH_SCRIPT_HANDLE_SPAWNED = 1,
    SH_SCRIPT_HANDLE_REMOVED = 2
} sh_script_handle_change_kind;

typedef struct sh_script_handle_change {
    int mod;
    int kind;              /* sh_script_handle_change_kind */
    sh_script_handle handle;
} sh_script_handle_change;

void sh_script_handles_set_resolver(sh_script_resolve_fn fn);

/* Empty the table for a new map and start a new epoch (old handles die). */
void sh_script_handles_reset(void);

/* Record a spawned entity of `mod`. Returns its handle; the existing handle
 * when the spawn id is already recorded (for the same mod); 0 when the spawn
 * id is none or the world, when another mod holds it, or when the table is
 * full (*full is then set to 1 when full is not NULL). */
sh_script_handle sh_script_handles_add(int mod, uint32_t spawn_id, int *full);

/* The same, keeping the entity's entityDef chain with the record: `names`
 * is its entityDef's name then the names it inherits, a newline between them, as
 * the runtime dispatches on it (copied; NULL keeps none). */
sh_script_handle sh_script_handles_add_named(int mod, uint32_t spawn_id, const char *names, int *full);

/* A handle for an entity no scripted mod owns (a player, an attacker), so a
 * script can be handed it: the existing record when the spawn id is already
 * recorded, else a new one that the sweep never reports and no mod owns
 * (sh_script_handle_mod answers SH_SCRIPT_HANDLE_FOREIGN). 0 for none, the
 * world or a full table. Any thread. */
#define SH_SCRIPT_HANDLE_FOREIGN (-2)
sh_script_handle sh_script_handles_ref(uint32_t spawn_id);

/* The entityDef chain recorded with `h` into out ("" when none). Returns its
 * length, or -1 when h is not a handle of this map. Any thread. */
int sh_script_handle_names(sh_script_handle h, char *out, size_t cap);

/* 1 while h is recorded and not yet reported gone (waiting for its first
 * sweep, or live), else 0. Any thread. */
int sh_script_handle_known(sh_script_handle h);

/* The handle recorded for `spawn_id`, or 0; *mod (may be NULL) gets its mod.
 * Any thread. A dead entry is still found (its handle resolves to nothing). */
sh_script_handle sh_script_handles_find(uint32_t spawn_id, int *mod);

/* The mod that owns `h`, SH_SCRIPT_HANDLE_FOREIGN for a handle no mod owns,
 * or -1 when h is not a handle of this map. Any thread. */
int sh_script_handle_mod(sh_script_handle h);

/* The spawn id behind `h`, or SH_SCRIPT_SPAWN_ID_NONE. */
uint32_t sh_script_handle_spawn_id(sh_script_handle h);

/* The entity `h` names right now, resolved through the engine: NULL when h
 * is stale, dead, not yet reported as spawned, or the entity is gone. Main
 * thread. */
void *sh_script_handle_entity(sh_script_handle h);

/* 1 when h is live (reported as spawned and not yet removed), else 0. */
int sh_script_handle_alive(sh_script_handle h);

/* The tick's check: resolve every pending and live entry once, in spawn
 * order, and write each change to out[] (up to cap). Entries past cap are
 * left as they were for the next sweep. Returns the number written. A pending
 * entry that is already gone is dropped silently: the script never saw it. */
int sh_script_handles_sweep(sh_script_handle_change *out, int cap);

/* Live entries of `mod` (-1: all mods). */
int sh_script_handles_live(int mod);

#ifdef __cplusplus
}
#endif
#endif
