#include "decl_native_reference.h"
#include "resource_types.h"
#include <stdlib.h>
#include <string.h>

void sh_decl_native_reference_clear(sh_decl_native_reference *reference)
{
    if (!reference) return;
    sh_decl_reference_clear(&reference->resource);
    sh_model_reference_clear(&reference->model);
    memset(reference, 0, sizeof(*reference));
}

int sh_decl_native_reference_read(sh_decl_native_schema *schema,
    sh_decl_native_registry *registry, sh_decl_value_type type,
    const char *name, size_t length, sh_decl_native_reference *out)
{
    sh_decl_reference_reader reader;
    char *copy = NULL;
    int result = -1;
    if (!out) return -1;
    sh_decl_native_reference_clear(out);
    if (!schema || !registry || !name || length == SIZE_MAX || memchr(name, 0, length)) return -1;
    if (!length) return 0;
    copy = malloc(length + 1);
    if (!copy) return -1;
    memcpy(copy, name, length); copy[length] = 0;
    reader = sh_decl_native_schema_reader(schema, type);
    if (reader == SH_DECL_READER_DECL) {
        result = sh_decl_native_registry_resolve_source(registry, type.name, copy, &out->resource);
        /* The native pointer reader asks the class's own manager with
         * makeDefault (Vulkan 0x1409ca890, OpenGL 0x1409ca050); a class without
         * one searches every manager without it. A name with no source in this
         * scope therefore yields null or that manager's defaulted object, whose
         * body is the manager's constant default definition, never text chosen
         * by name or by a package: a name-only native identity (2) with no
         * source dependency. Sound derives its native event from the
         * declaration name, so an absent sound stays unavailable (-1). */
        if (!result) {
            const char *family = NULL;
            int typed = sh_decl_native_registry_class_type(registry, type.name, &family);
            result = typed < 0 || (typed && family && !_stricmp(family, "sound")) ? -1 : 2;
        }
    } else if (reader == SH_DECL_READER_MODEL) {
        result = sh_model_reference_read(copy, &out->model);
        if (result == 1) {
            out->resource.type = out->model.family;
            out->resource.name = _strdup(out->model.name);
            if (!out->resource.name) result = -1;
        }
    } else if (reader == SH_DECL_READER_IMAGE || reader == SH_DECL_READER_STATIC_MODEL ||
        reader == SH_DECL_READER_STATIC_PARTICLE) {
        const char *family = reader == SH_DECL_READER_STATIC_MODEL ? "model" :
            reader == SH_DECL_READER_STATIC_PARTICLE ? "staticparticlemodel" : NULL;
        /* The declaration registry excludes image, model and other resource
         * managers. These class/family bindings describe the engine's separate
         * resource registry and are verified in both renderer executables. */
        if (reader == SH_DECL_READER_IMAGE) for (size_t i = 0; i < sizeof(SH_RESOURCE_TYPES) / sizeof(SH_RESOURCE_TYPES[0]); i++)
            if (!_stricmp(type.name, SH_RESOURCE_TYPES[i].class_name)) {
                if (family) { family = NULL; break; }
                family = SH_RESOURCE_TYPES[i].type;
            }
        result = family ? 1 : -1;
        if (result == 1) {
            /* The generic reader selects its exact registered manager; the
             * static-model and particle readers select fixed managers. Neither
             * uses the render-model field's extension-based dispatcher. */
            for (size_t i = 0; i < length; i++) {
                unsigned char c = (unsigned char)copy[i];
                if (c >= 128 || c < 32) { result = -1; break; }
                copy[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
            }
            if (result == 1) {
                if (*copy == '/' && copy[1] != '/') memmove(copy, copy + 1, strlen(copy));
                if (!*copy) result = -1;
                else { out->resource.type = family; out->resource.name = copy; copy = NULL; }
            }
        } else result = -1;
    }
    free(copy);
    if (result != 1) sh_decl_native_reference_clear(out);
    return result;
}

char *sh_decl_native_reference_bind(const sh_decl_native_reference *reference,
    const char *manager_name)
{
    if (!reference || !reference->resource.type || !reference->resource.name || !manager_name || !*manager_name)
        return NULL;
    return reference->model.family ? sh_model_reference_bind(&reference->model, manager_name) : _strdup(manager_name);
}
