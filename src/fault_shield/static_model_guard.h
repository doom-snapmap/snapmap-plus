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

/* Times the guard drew a render model empty this session. */
long sh_static_model_guard_fires(void);

/* A session static model reloads its surfaces in process heap 0: a level-4
 * refcounted static model's IncReferenceCount ("StaticModelIncRef") that is
 * about to reload its surfaces with a map heap selected runs inside process
 * heap 0 (the MemLocal heap-stack signatures). Returns 1 when armed. */
int sh_static_model_heap_rule_install(const uint8_t *module_base, const sig_result *results, size_t count);

/* Reloads moved to process heap 0 this session. */
long sh_static_model_heap_moves(void);

#endif /* SHIELD_STATIC_MODEL_GUARD_H */
