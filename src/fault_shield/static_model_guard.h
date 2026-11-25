/* Static render models never build from a surface table that is not a static model's: a table holding freed
 * memory faults on copy and again on free, so recovery cannot finish. The guard checks the table first and
 * draws the render model empty when it is bad. */
#ifndef SHIELD_STATIC_MODEL_GUARD_H
#define SHIELD_STATIC_MODEL_GUARD_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Install on "StaticModelUpdate" (and "ResourceMaterialize" for the engine's
 * own empty model). Returns 1 when armed. */
int sh_static_model_guard_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* The installs' byte checks. sh_static_model_update_check returns 0 when StaticModelUpdate's head, counter
 * compare and list LEA are as expected (*list, *counter set), 1 when one differs, 2 when the counter is not the
 * list's reload counter; sh_static_model_incref_head_ok returns 1 when StaticModelIncRef's head is as expected. */
int sh_static_model_update_check(const uint8_t *target, const uint8_t **list, const uint8_t **counter);
int sh_static_model_incref_head_ok(const uint8_t *target);

/* Times the guard drew a render model empty this session. */
long sh_static_model_guard_fires(void);

/* A level-4 refcounted static model's IncReferenceCount ("StaticModelIncRef") that reloads its surfaces with
 * a map heap selected runs inside process heap 0. Returns 1 when armed. */
int sh_static_model_heap_rule_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Reloads moved to process heap 0 this session. */
long sh_static_model_heap_moves(void);

/* A backing reloaded in place keeps its pointer, so render models would keep the old table. Begin a reload
 * pass (forgetting the last one's models), then name each reloaded backing: every render model of it gets
 * the never-built marker once, at its next update. sh_static_model_reloaded returns 1 when the guard is armed. */
void sh_static_model_reload_begin(void);
int  sh_static_model_reloaded(const void *backing);
/* Render models rebuilt that way this session. */
long sh_static_model_rebuilds(void);

#endif /* SHIELD_STATIC_MODEL_GUARD_H */
