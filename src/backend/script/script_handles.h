/* script_handles.h -- the entity handles mod scripts hold (a slot of this table plus its generation, so a stale
 * handle resolves to nothing); a script never holds an engine pointer. A spawn id is the 32-bit idEntityPtr value;
 * idGameLocal vtable +0x7F0 (VK 0x140393220, GL 0x140392AF0) resolves it, and +0x7F8 (VK 0x1403912A0, GL
 * 0x140390B70) is that lookup plus the idEntity cast.
 * add, sweep and entity run on the main thread; find and mod are safe on any thread. Pure C; the resolver is
 * injected. */
#ifndef SH_SCRIPT_HANDLES_H
#define SH_SCRIPT_HANDLES_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sh_script_handle;   /* 0 = no entity */

#define SH_SCRIPT_HANDLES_MAX   262143u      /* the handle format's slot range (records alive at once) */
#define SH_SCRIPT_SPAWN_ID_NONE  0x1FFFFFEu  /* the engine's unset spawn id */
#define SH_SCRIPT_SPAWN_ID_WORLD 0x1FFFFFDu  /* the world entity's spawn id */

/* The engine's lookup: the entity a spawn id names, or NULL. */
typedef void *(*sh_script_resolve_fn)(uint32_t spawn_id);

typedef enum sh_script_handle_change_kind {
    SH_SCRIPT_HANDLE_SPAWNED = 1,
    SH_SCRIPT_HANDLE_REMOVED = 2,   /* a mod's own live entity was removed */
    SH_SCRIPT_HANDLE_GONE    = 3    /* any other record's entity is gone (a reference, or one never delivered) */
} sh_script_handle_change_kind;

typedef struct sh_script_handle_change {
    int mod;
    int kind;              /* sh_script_handle_change_kind */
    sh_script_handle handle;
    uint32_t spawn_id;
} sh_script_handle_change;

/* Called once for every record the sweep releases (removed or gone), after the table's lock is released, so the
 * other tables of the script layer drop what they keep for that entity. Up to four; idempotent. Main thread. */
typedef void (*sh_script_gone_fn)(sh_script_handle h, uint32_t spawn_id);
void sh_script_handles_on_gone(sh_script_gone_fn fn);

void sh_script_handles_set_resolver(sh_script_resolve_fn fn);

/* Empty the table for a new map; every handle of the previous map dies. */
void sh_script_handles_reset(void);

/* Record a spawned entity of `mod`: its handle (the existing one when already recorded for that mod), or 0 for
 * the none or world id, another mod's entity, or no room (then *full is set to 1 unless NULL). */
sh_script_handle sh_script_handles_add(int mod, uint32_t spawn_id, int *full);

/* The same, keeping the entity's entityDef chain with the record: `names`
 * is its entityDef's name then the names it inherits, a newline between them, as
 * the runtime dispatches on it (copied; NULL keeps none). */
sh_script_handle sh_script_handles_add_named(int mod, uint32_t spawn_id, const char *names, int *full);

/* A handle for an entity no scripted mod owns (a player, an attacker): the existing record, else a new one
 * the sweep never reports as spawned and releases as "gone" with its entity. 0 for none, world or no room. */
#define SH_SCRIPT_HANDLE_FOREIGN (-2)
sh_script_handle sh_script_handles_ref(uint32_t spawn_id);

/* The entityDef chain recorded with `h` into out ("" when none). Returns its
 * length, or -1 when h is not a handle of this map. Any thread. */
int sh_script_handle_names(sh_script_handle h, char *out, size_t cap);

/* 1 while h is recorded and not yet reported gone (waiting for its first
 * sweep, or live), else 0. Any thread. */
int sh_script_handle_known(sh_script_handle h);

/* The handle recorded for `spawn_id`, or 0; *mod (may be NULL) gets its mod.
 * Any thread. A record the sweep released is not found. */
sh_script_handle sh_script_handles_find(uint32_t spawn_id, int *mod);

/* The mod that owns `h`, SH_SCRIPT_HANDLE_FOREIGN for a handle no mod owns,
 * or -1 when h is not a handle of this map. Any thread. */
int sh_script_handle_mod(sh_script_handle h);

/* The spawn id behind `h`, or SH_SCRIPT_SPAWN_ID_NONE. */
uint32_t sh_script_handle_spawn_id(sh_script_handle h);

/* The entity `h` names, resolved through the engine: NULL when h is stale, released, not yet reported as
 * spawned, or gone. Main thread. */
void *sh_script_handle_entity(sh_script_handle h);

/* 1 when h is live (reported as spawned and not yet removed), else 0. */
int sh_script_handle_alive(sh_script_handle h);

/* The tick's check: resolve every pending and live entry once, in spawn order, and write each change to out[]
 * (up to cap; the rest wait for the next sweep). Returns the number written; removed or gone records are
 * released, and a pending entry already gone is reported as gone. */
int sh_script_handles_sweep(sh_script_handle_change *out, int cap);

/* Live entries of `mod` (-1: all mods). */
int sh_script_handles_live(int mod);

/* Records held (pending and live, references included). */
int sh_script_handles_count(void);

#ifdef __cplusplus
}
#endif
#endif
