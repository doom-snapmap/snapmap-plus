#include "package_movie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The language resolver can request these for any movie that draws text. */
static const char *const pm_fallback_fonts[] = {
    "square721 cn tl", "v7gothic-medium-p", "dflihei-bd", "tt supermolot", "vd-logog-medium-p"
};
/* Every constructor looks these up with default creation enabled. */
static const char *const pm_fixed_materials[] = {
    "guisolid", "guisolid_degamma", "guistencilincr", "guistencildecr", "_white",
    "textures/common/video_gui.tga", "textures/common/video_gui",
    "textures/common/video_gui_nofog", "textures/gui/load_image"
};

static int pm_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity && !*error) snprintf(error, capacity, "UI movie constructor: %s", reason); return 0; }

/* Native manager lookup spelling: fold ASCII uppercase, backslashes become
 * slashes and one leading slash is removed. Other bytes are kept exactly. */
static int pm_normalize(const char *text, size_t length, char *out, size_t capacity)
{
    size_t at = 0;
    if (!text || !out || !capacity) return 0;
    if (length && (text[0] == '/' || text[0] == '\\') && !(length > 1 && (text[1] == '/' || text[1] == '\\'))) { text++; length--; }
    if (!length || length >= capacity) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!c) return 0;
        out[at++] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
    }
    out[at] = 0; return 1;
}

int sh_package_movie_native_name(const char *text, size_t length, char *out, size_t capacity)
{ return pm_normalize(text, length, out, capacity); }
int sh_package_movie_owner(const char *family, const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return family && !strcmp(family, "file") && n > 19 && !strncmp(name, "generated/swf/", 14) &&
        !strcmp(name + n - 5, ".bswf");
}

int sh_package_movie_names_for_path(const char *path, sh_package_movie_names *out)
{
    char movie[256];
    size_t n = path ? strlen(path) : 0;
    sh_swf_binary_paths paths;
    if (out) memset(out, 0, sizeof(*out));
    if (!out || n <= 19 || strncmp(path, "generated/swf/", 14)) return 0;
    if (!strcmp(path + n - 5, ".bswf")) {
        if (n >= sizeof(movie)) return -1;
        memcpy(movie, path, n + 1);
    } else if (n > 21 && !strcmp(path + n - 7, ".bimage")) {
        if (n - 2 >= sizeof(movie)) return -1;
        memcpy(movie, path, n - 7); memcpy(movie + n - 7, ".bswf", 6);
    } else return 0;
    if (!sh_swf_binary_consumer_for(movie, &paths, out->consumer)) {
        memset(out, 0, sizeof(*out)); return -1;
    }
    memcpy(out->movie, paths.movie, sizeof(out->movie)); memcpy(out->atlas, paths.atlas, sizeof(out->atlas));
    return 1;
}

int sh_package_movie_names_for_consumer(const char *consumer, size_t length, sh_package_movie_names *out)
{
    char copy[256];
    sh_swf_binary_paths paths;
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !consumer || length == SIZE_MAX) return -1;
    if (!length) return 0;
    if (length >= sizeof(copy) || memchr(consumer, 0, length)) return -1;
    memcpy(copy, consumer, length); copy[length] = 0;
    if (!sh_swf_binary_paths_for(copy, &paths)) return -1;
    /* One file unit carries one atlas. A consumer whose derived atlas is not
     * the movie's own stem (the first-.bimage rule inside a directory) could
     * share a movie file with a different atlas, so it is not one identity. */
    size_t movie = strlen(paths.movie);
    if (movie <= 5 || strncmp(paths.atlas, paths.movie, movie - 5) || strcmp(paths.atlas + movie - 5, ".bimage")) return -1;
    memcpy(out->movie, paths.movie, sizeof(out->movie)); memcpy(out->atlas, paths.atlas, sizeof(out->atlas));
    memcpy(out->consumer, copy, length + 1); return 1;
}

typedef struct pm_fonts {
    char **names; size_t count, capacity;
    int failed;
} pm_fonts;
static int pm_font(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    pm_fonts *fonts = context; char normalized[256];
    (void)offset; (void)length;
    if (strcmp(family, "font")) return 1;
    /* An empty or overlong native font name has no manager identity to bind. */
    if (!pm_normalize(name, strlen(name), normalized, sizeof(normalized))) { fonts->failed = 1; return 0; }
    for (size_t i = 0; i < fonts->count; i++) if (!strcmp(fonts->names[i], normalized)) return 1;
    if (fonts->count == fonts->capacity) {
        size_t next = fonts->capacity ? fonts->capacity * 2 : 8;
        char **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(fonts->names, next * sizeof(*grown)) : NULL;
        if (!grown) { fonts->failed = 1; return 0; }
        fonts->names = grown; fonts->capacity = next;
    }
    if (!(fonts->names[fonts->count] = _strdup(normalized))) { fonts->failed = 1; return 0; }
    fonts->count++; return 1;
}
static int pm_slot(sh_package_movie_request visit, void *context, const char *kind,
    const char *family, const char *name, char *error, size_t capacity)
{
    char slot[300];
    int n = snprintf(slot, sizeof(slot), "movie/%s/%s", kind, name);
    if (n < 0 || (size_t)n >= sizeof(slot)) return pm_fail(error, capacity, "request name exceeds its slot capacity");
    int status = visit(context, slot, family, name);
    return status == 0 || status == 1 ? 1 : pm_fail(error, capacity, "request availability could not be established");
}
int sh_package_movie_capture(const unsigned char *body, size_t length,
    sh_package_movie_request visit, void *context, char *error, size_t capacity)
{
    pm_fonts fonts = {0};
    sh_swf_binary_observer observer = {&fonts, pm_font, NULL};
    int ok = 0;
    if (error && capacity) *error = 0;
    if (!body || !visit) return pm_fail(error, capacity, "requires a captured movie body");
    if (!sh_swf_binary_read(body, length, &observer, error, capacity) || fonts.failed) {
        pm_fail(error, capacity, "dictionary font identities are invalid"); goto done;
    }
    for (size_t i = 0; i < fonts.count; i++)
        if (!pm_slot(visit, context, "font", "font", fonts.names[i], error, capacity)) goto done;
    if (fonts.count) for (size_t i = 0; i < sizeof(pm_fallback_fonts) / sizeof(*pm_fallback_fonts); i++) {
        int repeated = 0;
        for (size_t j = 0; j < fonts.count && !repeated; j++) repeated = !strcmp(fonts.names[j], pm_fallback_fonts[i]);
        if (!repeated && !pm_slot(visit, context, "font", "font", pm_fallback_fonts[i], error, capacity)) goto done;
    }
    for (size_t i = 0; i < sizeof(pm_fixed_materials) / sizeof(*pm_fixed_materials); i++)
        if (!pm_slot(visit, context, "material", "material", pm_fixed_materials[i], error, capacity)) goto done;
    ok = 1;
done:
    for (size_t i = 0; i < fonts.count; i++) free(fonts.names[i]);
    free(fonts.names); return ok;
}

typedef struct pm_sites { sh_resource_binding_site visitor; void *context; } pm_sites;
static int pm_material(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    pm_sites *sites = context;
    return strcmp(family, "material") ? 1 : sites->visitor(sites->context, family, name, offset, length);
}
int sh_package_movie_material_sites(void *reader_context,
    const unsigned char *body, size_t length, sh_resource_binding_site visitor,
    void *context, char *error, size_t capacity)
{
    pm_sites sites = {visitor, context};
    sh_swf_binary_observer observer = {&sites, pm_material, NULL};
    (void)reader_context;
    return visitor && sh_swf_binary_read(body, length, &observer, error, capacity);
}

int sh_package_movie_route(const sh_package_consumer *owner, const char *family,
    const char *request, const char **replacement)
{
    char normalized[256], slot[300]; const char *kind;
    if (replacement) *replacement = NULL;
    if (!replacement || !family || !request) return -1;
    if (!owner || !sh_package_movie_owner(owner->family, owner->name)) return 0;
    if (!_stricmp(family, "font")) kind = "font";
    else if (!_stricmp(family, "material")) kind = "material";
    else return 0;
    if (!pm_normalize(request, strlen(request), normalized, sizeof(normalized))) return 0;
    int n = snprintf(slot, sizeof(slot), "movie/%s/%s", kind, normalized);
    if (n < 0 || (size_t)n >= sizeof(slot)) return 0;
    const sh_package_consumer_target *target = sh_package_consumer_find(owner, slot);
    if (!target) return 0;
    if (!target->family || _stricmp(target->family, kind) || !target->name || !*target->name) return -1;
    if (!strcmp(target->name, normalized)) return 0;
    *replacement = target->name; return 1;
}
