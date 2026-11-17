/* script_api_body.h -- script calls on an entity's body: the sync it is a
 * participant of, the gore wounds open on it, and its articulated figure.
 *
 *   e:sync_partner()              -> target, instigator, sync entity, or nil and why
 *   e:wounds_open()               -> { node, ... } the gore nodes open or about to open, or nil and why
 *   e:break_constraint(name, [impulse]) -> true, or false and why
 *
 * Each works on any entity of the map that has the part it reads (a sync
 * participant, a gore component, an articulated figure): a demon, a mod's
 * idGoreEntity, anything else. A receiver without the part, a name the
 * figure does not have or a sync that already ended answers nil or false
 * with a plain reason; only a wrong argument type raises. Nothing is kept
 * across frames and nothing of the game is changed but the receiver's own
 * figure, by the game's own constraint removal.
 *
 * The reads go through the game's type information (script_reflect.h); the
 * one engine call is idPhysics_AF::DeleteConstraint (script_api_sigs.c,
 * ScriptAfDeleteConstraint). */
#ifndef SH_SCRIPT_API_BODY_H
#define SH_SCRIPT_API_BODY_H

#ifdef __cplusplus
extern "C" {
#endif

/* The figure's constraint removal: (physics, the constraint's own name
 * field). 1 when it ran. Set by the engine half, or a test. */
typedef int (*sh_body_delete_fn)(void *physics, void *name_field);
void sh_script_api_body_set_delete(sh_body_delete_fn fn);

/* Register the three calls (once). */
void sh_script_api_body_register(void);

/* 1 when the entity has a gore component the game's types describe. */
int sh_script_body_has_gore(void *ent);

#ifndef SH_SCRIPT_PURE
#include <stdint.h>
/* Resolve the engine call, register, and let wound_hides take a gore entity. */
int sh_script_api_body_install(const uint8_t *module_base);
#endif

#ifdef __cplusplus
}
#endif
#endif
