/* Backend implementation of the shared UI interface's editor operations.
 * Globals and functions resolve through signed code references. Editor field
 * offsets remain build-dependent; SEH contains access faults, not semantic
 * errors or every possible failure from an incompatible layout. */
#ifndef B2_IFACE_ENGINE_H
#define B2_IFACE_ENGINE_H

#include <stdint.h>
#include "signatures.h"

/* Bind editor callbacks after interface creation and signature resolution.
 * Return the number of bound slots. Each callback checks its dependencies;
 * installation is one-shot and can leave features partially available. */
int sh_iface_engine_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* Internal backend consumers may inspect the already-resolved inline editor
 * singleton from the main-thread tick. NULL means the engine bridge has not
 * been installed yet. The returned pointer is borrowed for the process life. */
const uint8_t *sh_iface_engine_editor_base(void);

/* Native paste requires an explicitly enabled, readable engine copy/paste cvar. */
int sh_iface_engine_copy_paste_enabled(void);

/* Check whether the class derives from the inherit decl's base type. Null
 * arguments retain live values. Return 0 for a known incompatible pair, 1
 * for compatible or unknown; this fail-open check is not full validation. */
int sh_iface_class_inherit_ok(int id, const char *newClass, const char *newInherit);

/* Format a module-qualified entity reference. Global entities produce a
 * no-module marker that target-reference callers reject. */
const char *ie_resolve_id_string(int id, char *buf, int cap);

#endif /* B2_IFACE_ENGINE_H */
