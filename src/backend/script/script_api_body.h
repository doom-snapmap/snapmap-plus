/* script_api_body.h -- script calls on an entity's body: e:sync_partner(), e:wounds_open() and
 * e:break_constraint(name, [impulse]), each answering nil or false with a plain reason when the entity lacks the part.
 * The reads go through the game's type information; the one engine call is idPhysics_AF::DeleteConstraint. */
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
