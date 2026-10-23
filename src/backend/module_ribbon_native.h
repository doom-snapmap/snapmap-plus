/* module_ribbon_native.h -- package module variants in the editor's module picker. */
#ifndef BACKEND_MODULE_RIBBON_NATIVE_H
#define BACKEND_MODULE_RIBBON_NATIVE_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Resolve the blueprint add mode's tick and its sort-cycle refresh sequence and
 * hook the tick. Requires the private module lookup (sh_grid_native_install).
 * Install happens once per process on a quiescent boot thread. */
int sh_module_ribbon_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Take the served library's module variants as the picker's published set now,
 * on the engine main thread. The add mode's tick does this by itself whenever a
 * new compilation is served (sh_package_runtime_served_serial). Rows change at
 * the next refresh of the open picker (the add mode's next tick, through the
 * same sequence its sort cycle runs), or when the picker next opens. Returns 0
 * only on allocation failure. */
int sh_module_ribbon_publish(void);

/* One line per published variant and its picker state, for sh_packages. Caller
 * frees. NULL when nothing is published. */
char *sh_module_ribbon_status(void);

#endif /* BACKEND_MODULE_RIBBON_NATIVE_H */
