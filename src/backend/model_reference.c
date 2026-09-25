#include "model_reference.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void sh_model_reference_clear(sh_model_reference *reference)
{ if (reference) { free(reference->name); memset(reference, 0, sizeof(*reference)); } }

int sh_model_reference_read(const char *request, sh_model_reference *out)
{
    static const char *const runtime[] = {"_scratch", "_stamp", "_world", "_decal"};
    char *name, *dot, *slash;
    size_t length;
    const char *family = "model", *suffix = "";
    size_t resource_suffix = 0;
    if (!out) return -1;
    sh_model_reference_clear(out);
    if (!request || !*request) return 0;
    length = strlen(request);
    if (length > SIZE_MAX - 7 || !(name = malloc(length + 7))) return -1;
    for (size_t i = 0; i <= length; i++) {
        unsigned char c = (unsigned char)request[i];
        /* Non-ASCII resource normalization needs a separate native contract;
         * ordinary declaration/map strings never pass through this function. */
        if (c >= 128 || (c && c < 32)) { free(name); return -1; }
        name[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
    }
    if (*name == '/' && name[1] != '/') memmove(name, name + 1, strlen(name));
    for (size_t i = 0; i < sizeof(runtime) / sizeof(runtime[0]); i++)
        if (!strcmp(name, runtime[i])) { free(name); return 0; }
    dot = strrchr(name, '.'); slash = strrchr(name, '/');
    if (dot && slash && dot < slash) dot = NULL;
    if (dot) {
        if (!strcmp(dot, ".prt")) { family = "particle"; suffix = ".prt"; *dot = 0; }
        else if (!strcmp(dot, ".cloth")) { family = "cloth"; suffix = ".cloth"; *dot = 0; }
        else if (!strcmp(dot, ".md6") || !strcmp(dot, ".decl")) family = "md6def";
        else if (!strcmp(dot, ".break")) { family = "breakable"; suffix = ".break"; *dot = 0; }
        else if (!strcmp(dot, ".breakabledecl")) {
            family = "breakable"; suffix = ".breakabledecl"; resource_suffix = 5; memcpy(dot, ".decl", 6);
        } else if (!strcmp(dot, ".hotspot")) { family = "hotspot"; suffix = ".hotspot"; *dot = 0; }
        else if (!strcmp(dot, ".watersurf") || !strcmp(dot, ".automap")) { free(name); return -1; }
    }
    if (!*name) { free(name); return -1; }
    *out = (sh_model_reference){family, name, suffix, resource_suffix}; return 1;
}

/* Editor preview models (Vulkan 0x1405df8a0 / OpenGL 0x1405df180): an empty
 * name is the default model; prt, cloth, md6/decl, breakabledecl, break,
 * watersurf, hotspot and automap take the renderer's branches (Vulkan
 * 0x140301520 / OpenGL 0x1403008f0); fx makes an _empty model named after the
 * effect; anything else goes to FindModel. Runtime names reach FindModel
 * there instead of being generated, but still name no source. */
int sh_model_preview_reference_read(const char *request, sh_model_reference *out)
{
    int result = sh_model_reference_read(request, out);
    const char *dot, *slash;
    if (result != 1) return result;
    /* The dispatcher's own fx branch asks FindModel for _empty, and the entity
     * preview (Vulkan 0x140d33770) tests for it before dispatching. */
    if (!strcmp(out->name, "_empty")) { sh_model_reference_clear(out); return 0; }
    dot = strrchr(out->name, '.'); slash = strrchr(out->name, '/');
    if (!strcmp(out->family, "model") && dot && (!slash || dot > slash) && !strcmp(dot, ".fx")) {
        sh_model_reference_clear(out); return -1;
    }
    return 1;
}

char *sh_model_reference_bind(const sh_model_reference *reference, const char *replacement)
{
    char *out;
    size_t length, suffix;
    if (!reference || !reference->family || !reference->name || !reference->request_suffix ||
        !replacement || !*replacement) return NULL;
    length = strlen(replacement); suffix = strlen(reference->request_suffix);
    if (reference->resource_suffix_length > length ||
        (reference->resource_suffix_length && (length < 5 || strcmp(replacement + length - 5, ".decl")))) return NULL;
    length -= reference->resource_suffix_length;
    if (suffix >= SIZE_MAX - length || !(out = malloc(length + suffix + 1))) return NULL;
    memcpy(out, replacement, length); memcpy(out + length, reference->request_suffix, suffix + 1);
    /* Ensure a relocation did not accidentally change the native dispatcher. */
    {
        sh_model_reference check = {0};
        int valid = sh_model_reference_read(out, &check) == 1 && !strcmp(check.family, reference->family) &&
            !strcmp(check.name, replacement);
        sh_model_reference_clear(&check);
        if (!valid) { free(out); return NULL; }
    }
    return out;
}
