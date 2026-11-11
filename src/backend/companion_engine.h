/* companion_engine.h -- spawn, place, bind and target a tile's companions.
 *
 * The one engine patch is an inline detour on idClass::CallSpawn (the typeinfo
 * spawn chain every spawned object runs once its edit state is read): after
 * the original returns, an idEntity whose entityDef is, or inherits, an owner
 * in the companions table (companion.h) gets its companions. Map entities
 * reach CallSpawn from idGameLocal::SpawnEntitiesForLayers' "Spawning %d
 * entities" pass, after every map entity has been registered, so companions
 * take entity numbers after the map's own. Each companion is spawned the way
 * idTarget_Spawn::SpawnSingleEntity spawns at a pose: game vtable +0x448
 * creates it, its spawnPosition (+0x3A8) and spawnOrientation (+0x3B4) get the
 * owner-relative world transform, and +0x458 finishes it (register, then
 * CallSpawn), so the class's own Spawn places it. Placing after the spawn is
 * not enough: SpawnEntityDef (+0x450) runs CallSpawn before it returns, the
 * entity SetOrigin/SetAxis slots write the physics only, and an idProp_Coop
 * stays where its Spawn presented it, the world origin. When the
 * create and finish calls do not resolve, the companion falls back to
 * SpawnEntityDef, SetOrigin, SetAxis and UpdateVisuals (+0x3C0), the pair the
 * engine's setOrigin event makes. It is then bound with idEntity::Bind
 * (oriented) and appended to the owner's targets with idEntity::AddTarget, in
 * list order. Nothing is spawned on a network client, which receives
 * server-spawned entities itself.
 *
 * Build 13954591 addresses, both executables (signatures are private to this
 * file and resolve by pattern on either image):
 *   CallSpawn                 VK 0x141583A00  GL 0x141584A90  (detoured, 14 bytes)
 *   idEntity::Bind            VK 0x140818D60  GL 0x140817EB0
 *   idEntity::AddTarget       VK 0x140818210  GL 0x140817340
 *   SpawnEntityDef wrapper    VK 0x1403BCFF0  GL 0x1403BC950  (game vtable +0x450;
 *                             its second call is the idEntity cast, VK 0x1413B34E0 GL 0x1413B3370)
 *   idGameLocal::IsClient     VK 0x140398370  GL 0x140397C80
 *   create (vtable +0x448)    VK 0x140384DF0  GL 0x140384690  (modscene_sigs.c)
 *   finish (vtable +0x458)    VK 0x1403BCF10  GL 0x1403BC870  (modscene_sigs.c)
 * Offsets (both): entity +0x6D0 entityDef, +0x808 physics, +0x3F0/+0x3F8
 * targets; entityDef +0x08 name, +0x58 inherit; entity vtable +0x338 SetOrigin,
 * +0x358 SetAxis, +0x3C0 UpdateVisuals; entity +0x3A8 spawnPosition, +0x3B4
 * spawnOrientation; physics vtable +0x80 GetOrigin(id), +0x88 GetAxis(id).

 *
 * The detour is committed only when an admitted bundle declares companions,
 * or when the map's mod scripts need the spawns (sh_companions_set_spawn_
 * subscriber). With neither the spawn chain is untouched. A bundle with companions
 * installed after boot arms it at the next main-thread gate
 * (sh_companions_ensure): the map load, the Play build, the editor palette
 * pass, a rescan. Once committed it stays for the session. */
#ifndef SH_COMPANION_ENGINE_H
#define SH_COMPANION_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The capability name a mod requests the facility by (engine_caps), once the
 * facility carries hook rows; until then the companions block is the request. */
#define SH_COMPANIONS_CAPABILITY "snapmap.owner_companions"

/* Resolve the private signatures and the decl registry from the shared results,
 * read every admitted bundle's companions, and commit the detour when at least
 * one owner is declared. Idempotent. Returns 1 when the detour is live. */
int sh_companions_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Arm the detour after boot, at a gate the product owns on DOOM's main
 * thread, before the spawns it precedes. Checks, in
 * order: boot resolution succeeded (it is cached, so no signature is resolved
 * here); the detour is not live yet (1 at once when it is: it is never
 * unhooked mid-session); the caller is DOOM's main thread (main_thread_id;
 * never armed from another thread, where a spawn could be inside the 14-byte
 * window); the serving index changed since the table was built and the
 * rebuilt table has an owner; CallSpawn's prologue bytes are still the
 * expected ones (companion.h, sh_companion_arm_decide). Logs
 * `COMPANIONS: installed on CallSpawn at <p> for <n> owner(s) (<phase>)` when
 * it commits. Returns 1 when the detour is live. */
int sh_companions_ensure(const char *phase);

/* Mark the tables stale: a new serving index (the index listener calls it).
 * The live detour re-reads them at the next outermost spawn of a mods
 * entity, never inside a companion's own spawn (its owner's loop is still
 * reading them); an unarmed one at the next gate. There is one table per
 * build that declares companions, and an owner's companions come from the
 * build that serves the owner's entityDef for the open map (a map's own copy
 * of a salt, not the root folder's). */
void sh_companions_refresh(void);

/* Another subscriber of the spawn chain (mod scripts, script/script_hooks.h).
 * It runs after the companions of every spawned idEntity whose entityDef or
 * first inherit is in the mods namespace, on the main thread, with the
 * entityDef's name and then its inherits while they stay in that namespace
 * (at most 8 inherits). `wanted` says whether it needs the detour: when it
 * returns 1 at a gate (sh_companions_ensure), the detour is armed even with no
 * companion owner. Set once at boot; NULL clears. */
typedef void (*sh_spawn_subscriber_fn)(void *entity, const char *const *def_names, int count);
typedef int  (*sh_spawn_wanted_fn)(void);
void sh_companions_set_spawn_subscriber(sh_spawn_subscriber_fn fn, sh_spawn_wanted_fn wanted);

/* The same names for a live idEntity (mod scripts' walk of the live
 * entities): up to cap written, 0 when its entityDef is not in the mods
 * namespace or the decl lookups did not resolve. Main thread. */
int sh_companions_def_chain(void *entity, const char **names, int cap);

/* The private signature table, for the two-image test. NULL-terminated. */
extern const sig_entry SH_COMPANION_SIGNATURES[];

#ifdef __cplusplus
}
#endif
#endif
