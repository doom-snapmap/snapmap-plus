/* Native pointer-reader identities shared by source inspection and linking. */
#ifndef SH_DECL_NATIVE_REFERENCE_H
#define SH_DECL_NATIVE_REFERENCE_H
#include "decl_native_registry.h"
#include "decl_native_schema.h"
#include "model_reference.h"

typedef struct sh_decl_native_reference {
    sh_decl_reference resource;
    sh_model_reference model;
    int movie; /* resource is a cooked UI movie file; bind writes its constructor name. */
    /* The use's loader creates a defaulted object when the resource has no
     * data (an event md6Anim argument); whether it needs a source is the
     * caller's availability question. A reflected field leaves this 0. */
    int make_default;
} sh_decl_native_reference;

/* 1 resource, 0 native runtime/null identity, 2 declaration with no source in
 * this scope (the engine supplies null or its constant default by name; not a
 * source dependency), -1 unavailable. An event md6Anim use
 * ({SH_DECL_EVENT_ANIM_TYPE, SH_DECL_EVENT_ANIM_OPS}) returns its anim
 * resource as 1 with make_default set. Only the verified
 * pointer reader selects semantics: image names do not use declaration search,
 * and a model field can dispatch to several different resource managers.
 * The registry must describe the prospective source scope. Initialize output
 * empty; repeated calls replace it and failures leave it empty. */
int sh_decl_native_reference_read(sh_decl_native_schema *schema,
    sh_decl_native_registry *registry, sh_decl_value_type type,
    const char *name, size_t length, sh_decl_native_reference *out);
/* Return an owned field value for a new manager identity, retaining native
 * model dispatcher syntax. A movie identity is written as the constructor name
 * that derives exactly that cooked pair; it does not establish loading. */
char *sh_decl_native_reference_bind(const sh_decl_native_reference *reference,
    const char *manager_name);
void sh_decl_native_reference_clear(sh_decl_native_reference *reference);
#endif
