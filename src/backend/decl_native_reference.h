/* Native pointer-reader identities shared by source inspection and linking. */
#ifndef SH_DECL_NATIVE_REFERENCE_H
#define SH_DECL_NATIVE_REFERENCE_H
#include "decl_native_registry.h"
#include "decl_native_schema.h"
#include "model_reference.h"

typedef struct sh_decl_native_reference {
    sh_decl_reference resource;
    sh_model_reference model;
} sh_decl_native_reference;

/* 1 resource, 0 native runtime/null identity, -1 unavailable. Only the verified
 * pointer reader selects semantics: image names do not use declaration search,
 * and a model field can dispatch to several different resource managers.
 * The registry must describe the prospective source scope. Initialize output
 * empty; repeated calls replace it and failures leave it empty. */
int sh_decl_native_reference_read(sh_decl_native_schema *schema,
    sh_decl_native_registry *registry, sh_decl_value_type type,
    const char *name, size_t length, sh_decl_native_reference *out);
/* Return an owned field value for a new manager identity, retaining native
 * model dispatcher syntax. It does not establish that the resource is loaded. */
char *sh_decl_native_reference_bind(const sh_decl_native_reference *reference,
    const char *manager_name);
void sh_decl_native_reference_clear(sh_decl_native_reference *reference);
#endif
