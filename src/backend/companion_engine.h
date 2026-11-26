/* companion_engine.h -- spawns, places, binds and targets a tile's companions through an inline detour on
 * idClass::CallSpawn: after the original returns, an idEntity whose entityDef is or inherits an owner in the
 * companions table (companion.h) gets its companions, each spawned as idTarget_Spawn::SpawnSingleEntity does (game
 * vtable +0x448 creates it, +0x458 finishes it so an idProp_Coop places itself), then bound with idEntity::Bind
 * (oriented) and appended with idEntity::AddTarget. Nothing is spawned on a network client. */

/* Build 13954591 addresses, VK / GL (signatures resolve by pattern on either image): CallSpawn 0x141583A00 /
 * 0x141584A90 (detoured, 14 bytes); idEntity::Bind 0x140818D60 / 0x140817EB0; AddTarget 0x140818210 / 0x140817340;
 * SpawnEntityDef wrapper 0x1403BCFF0 / 0x1403BC950 (game vtable +0x450; its second call is the idEntity cast, VK
 * 0x1413B34E0 GL 0x1413B3370); IsClient 0x140398370 / 0x140397C80; create 0x140384DF0 / 0x140384690 and finish
 * 0x1403BCF10 / 0x1403BC870 (modscene_sigs.c). Entity and vtable offsets are the named constants in the .c file. */

/* The detour is committed when an admitted bundle declares companions, mod scripts need spawns or a directly
 * placed actor needs its identity for For Each AI, at boot or the next main-thread gate (sh_companions_ensure),
 * and then stays for the session. */
#ifndef SH_COMPANION_ENGINE_H
#define SH_COMPANION_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The capability name a mod requests the facility by (engine_caps). */
#define SH_COMPANIONS_CAPABILITY "snapmap.owner_companions"

/* Resolves the private signatures and the decl registry from the shared results, reads every admitted bundle's
 * companions, and commits the detour when at least one owner is declared. Idempotent. Returns 1 when the detour
 * is live. */
int sh_companions_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Returns 1 when CallSpawn is resolved at install and its prologue is the expected one, so the chain can be armed
 * for mod scripts at a Play. */
int sh_companions_spawn_ready(void);

/* Arms the detour after boot at a main-thread gate, before the spawns it precedes; returns 1 when it is live (at once
 * when it already is). It arms only on DOOM's main thread (elsewhere a spawn could be inside the 14-byte window), with
 * the serving index changed, an owner in the rebuilt table and CallSpawn's prologue as expected
 * (sh_companion_arm_decide), and logs `COMPANIONS: installed on CallSpawn at <p> for <n> owner(s) (<phase>)`. */
int sh_companions_ensure(const char *phase);

/* Marks the tables stale on a new serving index. The live detour re-reads them at the next outermost spawn of a mods
 * entity (never inside a companion's own spawn), an unarmed one at the next gate. There is one table per build that
 * declares companions. */
void sh_companions_refresh(void);

/* After companions spawn, the subscriber gets each idEntity and its mod entityDef chain, empty for a
 * map or game entity. A gate arms the detour when wanted, even without companions. */
typedef void (*sh_spawn_subscriber_fn)(void *entity, const char *const *def_names, int count);
typedef int  (*sh_spawn_wanted_fn)(void);
void sh_companions_set_spawn_subscriber(sh_spawn_subscriber_fn fn, sh_spawn_wanted_fn wanted);

/* Writes the same names for a live idEntity, up to cap. Zero for a non-mod entityDef, -1 if unreadable. */
int sh_companions_def_chain(void *entity, const char **names, int cap);

/* The signature table, NULL-terminated. */
extern const sig_entry SH_COMPANION_SIGNATURES[];

#ifdef __cplusplus
}
#endif
#endif
