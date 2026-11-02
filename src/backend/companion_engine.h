/* companion_engine.h -- spawn, place, bind and target a tile's companions.
 *
 * The one engine patch is an inline detour on idClass::CallSpawn (the typeinfo
 * spawn chain every spawned object runs once its edit state is read): after
 * the original returns, an idEntity whose entityDef is, or inherits, an owner
 * in the companions table (companion.h) gets its companions. Map entities
 * reach CallSpawn from idGameLocal::SpawnEntitiesForLayers' "Spawning %d
 * entities" pass, after every map entity has been registered, so companions
 * take entity numbers after the map's own. Each companion is spawned through
 * idGameLocal::SpawnEntityDef (game vtable +0x450, the call the engine's own
 * idAnimated starting attachments make), placed with the entity SetOrigin and
 * SetAxis slots, bound with idEntity::Bind (oriented) and appended to the
 * owner's targets with idEntity::AddTarget, in list order. Nothing is spawned
 * on a network client, which receives server-spawned entities itself.
 *
 * Build 13954591 addresses, both executables (signatures are private to this
 * file and resolve by pattern on either image):
 *   CallSpawn                 VK 0x141583A00  GL 0x141584A90  (detoured, 14 bytes)
 *   idEntity::Bind            VK 0x140818D60  GL 0x140817EB0
 *   idEntity::AddTarget       VK 0x140818210  GL 0x140817340
 *   SpawnEntityDef wrapper    VK 0x1403BCFF0  GL 0x1403BC950  (game vtable +0x450;
 *                             its second call is the idEntity cast, VK 0x1413B34E0 GL 0x1413B3370)
 *   idGameLocal::IsClient     VK 0x140398370  GL 0x140397C80
 * Offsets (both): entity +0x6D0 entityDef, +0x808 physics, +0x3F0/+0x3F8
 * targets; entityDef +0x08 name, +0x58 inherit; entity vtable +0x338 SetOrigin,
 * +0x358 SetAxis; physics vtable +0x80 GetOrigin(id), +0x88 GetAxis(id).
 *
 * The detour is committed only when an admitted bundle declares companions.
 * With no such bundle the spawn chain is untouched. */
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

/* Re-read the bundles' companions at the next spawn (after a rescan). */
void sh_companions_refresh(void);

/* Restore CallSpawn. Call only while no spawn can run (no map loading).
 * Returns 1 when the detour is gone (or was never committed). */
int sh_companions_restore(void);

/* One status line: live or not, owners, spawned and failed counts. */
void sh_companions_status(char *out, size_t cap);

/* The private signature table, for the two-image test. NULL-terminated. */
extern const sig_entry SH_COMPANION_SIGNATURES[];

#ifdef __cplusplus
}
#endif
#endif
