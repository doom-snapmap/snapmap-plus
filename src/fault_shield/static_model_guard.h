/* Static render models never build from a surface table that is not a static
 * model's. A static render model copies its backing static model's surfaces;
 * when that table holds freed memory the copy faults, and the copied slots then
 * fault again when the model is freed, so the shield's recovery cannot finish
 * and the load screen stays up. The guard checks the table first and, when it
 * is not a static model's, draws that render model empty and logs the model. */
#ifndef SHIELD_STATIC_MODEL_GUARD_H
#define SHIELD_STATIC_MODEL_GUARD_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Install on "StaticModelUpdate" (and "ResourceMaterialize" for the engine's
 * own empty model). Returns 1 when armed. */
int sh_static_model_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* The two installs' own byte checks, for the build-image test. sh_static_model_update_check: 0 when
 * StaticModelUpdate's head, counter compare and list LEA are as expected and the counter is the list's reload
 * counter (*list, *counter set), 1 when the head or an instruction differs, 2 when the counter is not the
 * list's. sh_static_model_incref_head_ok: 1 when StaticModelIncRef's head is as expected. */
int sh_static_model_update_check(const uint8_t *target, const uint8_t **list, const uint8_t **counter);
int sh_static_model_incref_head_ok(const uint8_t *target);

/* Times the guard drew a render model empty this session. */
long sh_static_model_guard_fires(void);

/* A session static model reloads its surfaces in process heap 0: a level-4
 * refcounted static model's IncReferenceCount ("StaticModelIncRef") that is
 * about to reload its surfaces with a map heap selected runs inside process
 * heap 0 (the MemLocal heap-stack signatures). Returns 1 when armed. */
int sh_static_model_heap_rule_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Reloads moved to process heap 0 this session. */
long sh_static_model_heap_moves(void);

/* A backing static model reloaded in place keeps its pointer, so the engine's
 * update would keep each render model's copy of the old table. Begin a reload
 * pass (forgets the last one's models), then name each backing reloaded: every
 * render model of it is given the never-built marker once, at its next update
 * through the guard, and copies the new table. sh_static_model_reloaded returns
 * 1 when the guard is armed to do so. */
void sh_static_model_reload_begin(void);
int  sh_static_model_reloaded(const void *backing);
/* Render models rebuilt that way this session. */
long sh_static_model_rebuilds(void);

#endif /* SHIELD_STATIC_MODEL_GUARD_H */
