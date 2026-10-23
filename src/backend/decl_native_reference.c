#include "decl_native_reference.h"
#include "resource_types.h"
#include "package_movie.h"
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
    copy = malloc(length + 5); /* room for the collision builder's ".bcm" */
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
            int sound = typed > 0 && family && !_stricmp(family, "sound");
            result = typed < 0 ? -1 : sound ? -1 : 2;
            /* The sound system registers one sound declaration for every
             * event of sound/soundbanks/pc/soundbanksinfo.events at startup
             * (VK 0x141850900 / GL 0x1418438a0, "reading %d sound events"),
             * under the canonical lowercase name with no declaration file: a
             * live registry held all 7649 bank events (R-390). Such a
             * registered sound is a name-only native identity (2) like any
             * engine-created declaration; its consumer decides (sg_bind_typed).
             * A sound neither sourced nor registered has no event. */
            if (sound) {
                sh_decl_reference registered = {0};
                int found = sh_decl_native_registry_resolve(registry, type.name, copy, &registered);
                sh_decl_reference_clear(&registered);
                if (found == 1) result = 2;
                else if (found < 0) result = -1;
            }
        }
    } else if (reader == SH_DECL_READER_MODEL || reader == SH_DECL_READER_PREVIEW_MODEL) {
        result = reader == SH_DECL_READER_MODEL ? sh_model_reference_read(copy, &out->model) :
            sh_model_preview_reference_read(copy, &out->model);
        if (result == 1) {
            out->resource.type = out->model.family;
            out->resource.name = _strdup(out->model.name);
            if (!out->resource.name) result = -1;
        }
    } else if (reader == SH_DECL_READER_MOVIE) {
        /* The constructor tests the authored swf/ prefix before normalizing,
         * so the identity is derived from these exact bytes, not a folded copy. */
        sh_package_movie_names names;
        result = sh_package_movie_names_for_consumer(copy, length, &names);
        if (result == 1) {
            out->resource.type = "file"; out->movie = 1;
            out->resource.name = _strdup(names.movie);
            if (!out->resource.name) result = -1;
        }
    } else if (reader == SH_DECL_READER_IMAGE || reader == SH_DECL_READER_STATIC_MODEL ||
        reader == SH_DECL_READER_STATIC_PARTICLE || reader == SH_DECL_READER_VOICE_TRACK ||
        reader == SH_DECL_READER_COLLISION_MODEL) {
        const char *family = reader == SH_DECL_READER_STATIC_MODEL ? "model" :
            reader == SH_DECL_READER_STATIC_PARTICLE ? "staticparticlemodel" :
            reader == SH_DECL_READER_VOICE_TRACK ? "voicetrack" : reader == SH_DECL_READER_COLLISION_MODEL ? "cm" : NULL;
        /* The collision-model name builder (VK 0x14179a590 / GL 0x14178d670)
         * rewrites a name starting maps/ (any case) unless it contains the
         * case-sensitive "/megabreakable": SetFileExtension("bcm") (VK
         * 0x141a021a0), ASCII lowercase (0x141a034f0; sign-extended bytes
         * >= 0x80 stay), then every ".entities" is removed (0x141a018d0). Its
         * idStrStatic<256> stays within capacity below 250 bytes. */
        if (reader == SH_DECL_READER_COLLISION_MODEL && length >= 5 && !_strnicmp(copy, "maps/", 5) &&
            !strstr(copy, "/megabreakable")) {
            char *end = copy + length, *fragment;
            if (length >= 250) family = NULL;
            else {
                while (end > copy && end[-1] != '/' && end[-1] != '\\') if (*--end == '.') { *end = 0; break; }
                memcpy(copy + strlen(copy), ".bcm", 5);
                for (char *p = copy; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
                while ((fragment = strstr(copy, ".entities")) != NULL) memmove(fragment, fragment + 9, strlen(fragment + 9) + 1);
                length = strlen(copy);
            }
        }
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
             * static-model, particle, voice-track and collision-model readers
             * select fixed managers through the same find (VK 0x141801380). None
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
            /* The event argument reader finds the clip in the animation list,
             * then loads it with makeDefault (Vulkan 0x1409caa70 / OpenGL
             * 0x1409ca230); a reflected idMD6Anim field does not. */
            if (result == 1 && type.ops && !strcmp(type.ops, SH_DECL_EVENT_ANIM_OPS)) out->make_default = 1;
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
    if (reference->movie) {
        /* An unchanged binding keeps its authored spelling at the caller. A
         * relocated pair is regular by construction and always invertible. */
        sh_swf_binary_paths paths; char consumer[256];
        return sh_swf_binary_consumer_for(manager_name, &paths, consumer) ? _strdup(consumer) : NULL;
    }
    return reference->model.family ? sh_model_reference_bind(&reference->model, manager_name) : _strdup(manager_name);
}
