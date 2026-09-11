/* Reflection, inheritance, and material queries shared by console and frontend.
 * Addresses resolve through signatures/global anchors; object layouts still
 * require validation when porting engine builds. Returned engine strings are borrowed. */
#ifndef BACKEND_B2_TYPEINFO_H
#define BACKEND_B2_TYPEINFO_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Cache signature/global dependencies once. Return 1 after the initial pass,
 * including partial binding, or 0 if already installed or module_base is NULL. */
int sh_typeinfo_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* Call the resolved decl-manager accessor. Return NULL if unavailable or faulting.
 * Shared by reflection, entity apply, and event-manager code after installation. */
void *sh_typeinfo_get_declmgr(void);
/* Borrow the engine reflection context through the established decl-manager
 * accessor. Different manager interfaces use different vtable layouts. */
void *sh_typeinfo_get_reflect(void);

/* Enumerate decl instances as NUL-separated names with a double-NUL terminator.
 * Use silent reflection lookup for types such as sound/projectile. Return 1 with
 * out_count when at least one name was copied, otherwise 0. */
int sh_typeinfo_enum_decls_of_type(const char *declType, char *out_buf, int cap, int *out_count);

/* Check class ancestry: 1 matches, 0 mismatches or unknown type, -1 means the
 * type system is unavailable. Callers reject only definite 0 to prevent fatal
 * decl-reparse errors without blocking edits when reflection cannot be reached. */
int sh_typeinfo_class_derives(const char *className, const char *baseName);

/* Copy an inherit declaration's base class into buf through read-only lookup.
 * Return buf or NULL when unavailable; compatibility callers then allow the edit. */
const char *sh_typeinfo_inherit_base(const char *inheritName, char *buf, size_t cap);

/* Copy inherited renderModelInfo.model from resolved entityDef text. Return 1
 * on a hit. Uses read-only lookup, with no load, creation, rendering, or GPU call. */
int sh_typeinfo_inherit_model(const char *inheritName, char *buf, size_t cap);

/* Collect up to cap live class names, borrowed from engine-owned static strings.
 * Return the count, including partial reads, or -1 if the registry is unavailable. */
int sh_typeinfo_collect_classnames(const char **out_names, int cap);

/* Shared candidate-buffer capacity. */
#define SH_REGISTRY_MAX  16384

/* Borrowed class and superclass names for frontend ancestry checks. */
typedef struct sh_ti_record { const char *name; const char *super; } sh_ti_record;

/* Collect names and superclass names. Uses the reflection context or a signed
 * container fallback; return the count or -1 if neither is available. */
int sh_typeinfo_collect_records(sh_ti_record *out, int cap);

/* Collect borrowed names from the loaded entityDef registry without creating
 * declarations. Return up to cap entries, or -1 when the manager is unavailable. */
int sh_typeinfo_collect_inherits(const char **out_names, int cap);

/* Look up a material without loading/creating it; shipped names can resolve
 * before first rendering. Return 1 on a hit and format found/dimension details
 * into buf. Optional dimension/probe failures do not invalidate the hit. */
int sh_typeinfo_find_material(const char *name, char *buf, size_t cap);

#endif /* BACKEND_B2_TYPEINFO_H */
