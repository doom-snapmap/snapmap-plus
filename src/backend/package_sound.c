#include "package_sound.h"
#include "audio_banks.h"
#include <stdio.h>
#include <string.h>

static const char *ps_name(const sh_compiled_resource *resource)
{
    const char *type = resource->native_type ? resource->native_type : resource->type;
    return type && !strcmp(type, "sound") ?
        (resource->native_name ? resource->native_name : resource->name) : NULL;
}

static int ps_authored(const sh_compiled_resource *resource, uint32_t *event)
{
    static const char prefix[] = "generated/decls/sound/";
    const char *name = ps_name(resource);
    char original[256];
    if (!name || !*name) return 0;
    if (resource->original_path) {
        const char *path = resource->original_path;
        size_t length = strlen(path), begin = sizeof(prefix)-1;
        if (length <= begin + 5 || strncmp(path, prefix, begin) ||
            strcmp(path + length - 5, ".decl") || length - begin - 5 >= sizeof(original)) return 0;
        length -= begin + 5;
        memcpy(original, path + begin, length); original[length] = 0;
        name = original;
    }
    return sh_audio_event_identity(name, event);
}

int sh_package_sound_prepare(sh_package_compilation *compiled, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!compiled || (compiled->resource_count && !compiled->resources)) return 0;
    /* Validate the complete result before filling absent bindings. A caller
     * may retry preparation after another family finishes its own admission. */
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        const char *name = ps_name(resource);
        uint32_t authored, native;
        if (!name && !resource->sound_authored_event && !resource->sound_event) continue;
        if (!name || !ps_authored(resource, &authored) || !sh_audio_event_identity(name, &native) ||
            ((!resource->sound_authored_event) != (!resource->sound_event)) ||
            (resource->sound_authored_event && resource->sound_authored_event != authored)) {
            if (error && capacity) snprintf(error, capacity, "%s: invalid compiled sound event binding",
                resource->engine_path ? resource->engine_path : "sound");
            return 0;
        }
    }
    for (size_t i = 0; i < compiled->resource_count; i++) {
        sh_compiled_resource *resource = compiled->resources + i;
        if (ps_name(resource) && !resource->sound_authored_event) {
            (void)ps_authored(resource, &resource->sound_authored_event);
            resource->sound_event = resource->sound_authored_event;
        }
    }
    return 1;
}

int sh_package_sound_event(const sh_package_compilation *compiled, const char *name, uint32_t *out)
{
    static const char prefix[] = "generated/decls/sound/";
    char path[sizeof(prefix) + 256 + 5];
    size_t length, low = 0, high;
    if (!out) return 0;
    *out = 0;
    if (!compiled || !name || !(length = strnlen_s(name, 256)) || length == 256) return 0;
    memcpy(path, prefix, sizeof(prefix)-1);
    memcpy(path + sizeof(prefix)-1, name, length);
    memcpy(path + sizeof(prefix)-1 + length, ".decl", 6);
    high = compiled->resource_count;
    while (low < high) {
        size_t mid = low + (high-low)/2;
        if (strcmp(compiled->resources[mid].engine_path, path) < 0) low = mid+1; else high = mid;
    }
    if (low < compiled->resource_count) {
        const sh_compiled_resource *resource = compiled->resources + low;
        const char *bound_name = ps_name(resource);
        if (!strcmp(resource->engine_path, path) && bound_name && !strcmp(bound_name, name) &&
            resource->sound_authored_event && resource->sound_event) {
            *out = resource->sound_event; return 1;
        }
    }
    return 0;
}

int sh_package_sound_requires_binding(const sh_package_compilation *compiled)
{
    if (!compiled) return 0;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        uint32_t native;
        if (resource->sound_event && (!sh_audio_event_identity(ps_name(resource), &native) ||
            native != resource->sound_event)) return 1;
    }
    return 0;
}

int sh_package_sound_rebind(sh_compiled_resource *resource, uint32_t authored, uint32_t event)
{
    uint32_t verified;
    if (!resource || !authored || !event || resource->sound_authored_event != authored ||
        !resource->sound_event || !ps_authored(resource, &verified) || verified != authored) return 0;
    resource->sound_event = event; return 1;
}
