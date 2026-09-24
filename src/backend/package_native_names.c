#include "package_native_names.h"
#include "decl_server_path.h"
#include "swf_binary.h"
#include "font_binary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nn_fail(char *error, size_t capacity, const char *reason,
    const sh_package_link_unit *unit)
{
    if (error && capacity) snprintf(error, capacity, "%s%s%s%s%s", reason,
        unit ? ": " : "", unit ? unit->family : "", unit ? "/" : "", unit ? unit->name : "");
    return 0;
}
static int nn_canonical(const char *path)
{
    char *canonical = sh_package_engine_path(path);
    int ok = canonical && !strcmp(path, canonical);
    free(canonical); return ok;
}
static int nn_decl(const char *path, const char *family, const char *identity)
{
    char type[SH_DECL_SERVER_TYPE_CAP], name[SH_DECL_SERVER_NAME_CAP], source[SH_DECL_SERVER_SOURCE_CAP];
    const char *reason;
    return path && !strncmp(path, "generated/decls/", 16) &&
        sh_decl_server_identity_from_relative(path + 16, type, sizeof(type), name, sizeof(name),
            source, sizeof(source), &reason) && !strcmp(type, family) && !strcmp(name, identity) && !strcmp(source, path);
}

static int nn_swf_pair(const char *movie, const char *atlas)
{
    if (!movie || !atlas || !nn_canonical(movie) || !nn_canonical(atlas) ||
        strncmp(atlas, "generated/swf/", 14) || strlen(movie) >= 256 || strlen(atlas) >= 256) return 0;
    size_t length = strlen(atlas);
    const char *marker = strstr(atlas, ".bimage");
    if (length < 7 || strcmp(atlas + length - 7, ".bimage") || !marker) return 0;
    size_t prefix = (size_t)(marker - atlas);
    return strlen(movie) == prefix + 5 && !strncmp(movie, atlas, prefix) && !strcmp(movie + prefix, ".bswf");
}

static int nn_swf_members(const sh_package_link_unit *unit,
    const sh_compiled_resource *const *files, size_t count, size_t *movie, size_t *atlas)
{
    *movie = *atlas = SIZE_MAX;
    if (strcmp(unit->family, "file") || count != 2 || unit->file_count != count ||
        !unit->file_identities || !unit->files || !files) return 0;
    for (size_t i = 0; i < count; i++) {
        size_t j;
        for (j = 0; j < count; j++) if (files[i] == unit->files[j]) break;
        if (j == count || !files[i] || !files[i]->engine_path || !nn_canonical(files[i]->engine_path)) return 0;
        const sh_package_link_file_identity *member = unit->file_identities + j;
        const char *family = member->family ? member->family : unit->family;
        const char *name = member->name ? member->name : unit->name;
        if ((member->family != NULL) != (member->name != NULL) || !*family || !*name) return 0;
        if (!strcmp(family, unit->family) && !strcmp(name, unit->name)) {
            if (*movie != SIZE_MAX || strcmp(name, files[i]->engine_path)) return 0;
            *movie = i;
        } else if (!strcmp(family, "image")) {
            if (*atlas != SIZE_MAX || strcmp(name, files[i]->engine_path)) return 0;
            *atlas = i;
        } else return 0;
    }
    return *movie != SIZE_MAX && *atlas != SIZE_MAX && nn_swf_pair(unit->name, files[*atlas]->engine_path);
}
static int nn_font_members(const sh_package_link_unit *unit,
    const sh_compiled_resource *const *files, size_t count, unsigned variants[3])
{
    if (!count || count > 3) return 0;
    for (size_t i = 0; i < count; i++) variants[i] = 3;
    for (unsigned variant = 0; variant < 3; variant++) {
        sh_font_binary_paths expected;
        if (!sh_font_binary_paths_for(unit->name, (sh_font_binary_variant)variant, &expected)) return -1;
        size_t matches = 0;
        for (size_t i = 0; i < count; i++) if (!strcmp(files[i]->engine_path, expected.file)) {
            variants[i] = variant; matches++;
        }
        sh_font_binary_paths_clear(&expected);
        if (matches > 1) return 0;
    }
    for (size_t i = 0; i < count; i++) if (variants[i] == 3) return 0;
    return 1;
}

int sh_package_native_names_assign_bounded(void *context, size_t index,
    const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count,
    size_t name_capacity, size_t path_capacity,
    char **name, char ***paths, char *error, size_t capacity)
{
    const sh_package_link_inputs *inputs = context;
    const sh_package_link_unit *unit = NULL;
    char hex[65], alias[128], relative[SH_DECL_SERVER_SOURCE_CAP];
    char *original_path = NULL;
    const char *prefix = "smp/", *suffix = "";
    int declaration = 0, movie_pair = 0, font = 0;
    unsigned font_variants[3];
    size_t movie = SIZE_MAX, atlas = SIZE_MAX;
    if (error && capacity) *error = 0;
    if (name) *name = NULL;
    if (paths) *paths = NULL;
    if (!inputs || !inputs->units || index >= inputs->count || !identity || !name || !paths ||
        (count && !files) || count > SIZE_MAX / sizeof(**paths) || !name_capacity || !path_capacity)
        return nn_fail(error, capacity, "invalid native name assignment", NULL);
    unit = inputs->units + index;
    if (!unit->family || !unit->name || !nn_canonical(unit->family) || !nn_canonical(unit->name) ||
        count != unit->file_count) return nn_fail(error, capacity, "invalid native resource identity", NULL);
    for (size_t i = 0; i < count; i++) if (!files[i] || !nn_canonical(files[i]->engine_path))
        return nn_fail(error, capacity, "invalid original physical resource path", unit);
    if (!strcmp(unit->family, "file") && unit->file_identities) {
        if (!nn_swf_members(unit, files, count, &movie, &atlas))
            return nn_fail(error, capacity, "movie native identities do not describe its paired files", unit);
        movie_pair = 1;
    }
    if (!strcmp(unit->family, "font") && count) {
        int status = nn_font_members(unit, files, count, font_variants);
        if (status < 0) goto allocation;
        if (!status) return nn_fail(error, capacity, "font files do not match their native filename forms", unit);
        font = 1;
    }
    if (original_binding) {
        if (strlen(unit->name) >= name_capacity)
            return nn_fail(error, capacity, "original native name exceeds consumer capacity", unit);
        if (movie_pair && strlen(files[atlas]->engine_path) >= name_capacity)
            return nn_fail(error, capacity, "original native member name exceeds consumer capacity", unit);
        for (size_t i = 0; i < count; i++) if (strlen(files[i]->engine_path) >= path_capacity)
            return nn_fail(error, capacity, "original native path exceeds consumer capacity", unit);
        *name = _strdup(unit->name);
        *paths = calloc(count ? count : 1, sizeof(**paths));
        if (!*name || !*paths) goto allocation;
        for (size_t i = 0; i < count; i++) if (!((*paths)[i] = _strdup(files[i]->engine_path))) goto allocation;
        return 1;
    }
    /* Source-free native objects and verified fixed ABI families retain their
     * names. An underscore is also legal in authored file-backed names and is
     * not evidence of a native-created object. */
    if (!count || !strcmp(unit->family, "renderparm") ||
        !strcmp(unit->family, "renderprog") || !strcmp(unit->family, "sampler"))
        return nn_fail(error, capacity, "native ABI identity requires its original binding", unit);
    if (movie_pair) {
        for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity[i]);
        for (size_t digits = 64; digits >= 16; digits -= 2) {
            sh_swf_binary_paths linked;
            snprintf(alias, sizeof(alias), "swf/smp/%.*s.swf", (int)digits, hex);
            if (!sh_swf_binary_paths_for(alias, &linked) || !nn_swf_pair(linked.movie, linked.atlas))
                return nn_fail(error, capacity, "movie native filename generation failed", unit);
            if (strlen(linked.movie) >= name_capacity || strlen(linked.atlas) >= name_capacity ||
                strlen(linked.movie) >= path_capacity || strlen(linked.atlas) >= path_capacity) continue;
            *name = _strdup(linked.movie); *paths = calloc(count, sizeof(**paths));
            if (!*name || !*paths) goto allocation;
            (*paths)[movie] = _strdup(linked.movie); (*paths)[atlas] = _strdup(linked.atlas);
            if (!(*paths)[movie] || !(*paths)[atlas]) goto allocation;
            return 1;
        }
        return nn_fail(error, capacity, "native consumer capacity cannot hold paired movie identities", unit);
    }
    if (font) {
        for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity[i]);
        for (size_t digits = 64; digits >= 16; digits -= 2) {
            sh_font_binary_paths linked[3] = {0};
            int fits = 1, allocated = 1;
            snprintf(alias, sizeof(alias), "smp/%.*s", (int)digits, hex);
            if (strlen(alias) >= name_capacity) continue;
            for (size_t i = 0; i < count; i++) {
                if (!sh_font_binary_paths_for(alias, (sh_font_binary_variant)font_variants[i], linked + i)) {
                    allocated = 0; break;
                }
                if (strlen(linked[i].file) >= path_capacity) fits = 0;
            }
            if (fits && allocated) {
                *name = _strdup(alias); *paths = calloc(count, sizeof(**paths));
                if (!*name || !*paths) allocated = 0;
                else for (size_t i = 0; i < count; i++) {
                    (*paths)[i] = linked[i].file; linked[i].file = NULL;
                }
            }
            for (size_t i = 0; i < count; i++) sh_font_binary_paths_clear(linked + i);
            if (!allocated) goto allocation;
            if (fits) return 1;
        }
        return nn_fail(error, capacity, "native consumer capacity cannot hold linked font filenames", unit);
    }
    if (count != 1) return nn_fail(error, capacity, "resource permutations require a native relocation rule", unit);
    declaration = nn_decl(files[0]->engine_path, unit->family, unit->name);
    if (!declaration) {
        original_path = sh_package_cooked_path(unit->family, unit->name);
        int matches = original_path && !strcmp(original_path, files[0]->engine_path);
        free(original_path);
        if (!matches) return nn_fail(error, capacity, "resource path has no verified native relocation rule", unit);
    }
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity[i]);
    if (!declaration && !strcmp(unit->family, "image")) {
        /* Explicit bimage names bypass NameWithOpts: the digest already
         * distinguishes the bound source image/options. Do not append material
         * options a second time or infer a source extension from cooked bytes. */
        prefix = "generated/image/smp/"; suffix = ".bimage";
    } else {
        if (!strcmp(unit->family, "material")) {
            /* Parse distinguishes a leading generic prefix and baked/ anywhere
             * in the name. Preserve both flags without retaining long paths. */
            prefix = !strncmp(unit->name, "generic", 7) ?
                (strstr(unit->name, "baked/") ? "generic/baked/smp/" : "generic/smp/") :
                (strstr(unit->name, "baked/") ? "baked/smp/" : "smp/");
        }
        if (!declaration) suffix = (!strcmp(unit->family, "model") || !strcmp(unit->family, "discreteanimation")) ? ".lwo" :
            !strcmp(unit->family, "basemodel") ? ".md6mesh" : !strcmp(unit->family, "anim") ? ".md6anim" : "";
    }
    for (size_t digits = 64; digits >= 16; digits -= 2) {
        char *physical;
        int n = snprintf(alias, sizeof(alias), "%s%.*s%s", prefix, (int)digits, hex, suffix);
        if (n < 0 || (size_t)n >= sizeof(alias) || (size_t)n >= name_capacity) continue;
        if (declaration) {
            n = snprintf(relative, sizeof(relative), "generated/decls/%s/%s.decl", unit->family, alias);
            if (n < 0 || (size_t)n >= sizeof(relative) || (size_t)n >= path_capacity) continue;
            if (!nn_decl(relative, unit->family, alias))
                return nn_fail(error, capacity, "linked declaration identity is invalid", unit);
            physical = _strdup(relative);
        } else physical = sh_package_cooked_path(unit->family, alias);
        if (!physical) goto allocation;
        if (strlen(physical) >= path_capacity) { free(physical); continue; }
        *name = _strdup(alias); *paths = calloc(1, sizeof(**paths));
        if (!*name || !*paths) { free(physical); goto allocation; }
        (*paths)[0] = physical; return 1;
    }
    return nn_fail(error, capacity, "native consumer capacity cannot hold a linked identity", unit);
allocation:
    return nn_fail(error, capacity, "cannot allocate native resource names", unit);
}

int sh_package_native_names_assign(void *context, size_t index,
    const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count,
    char **name, char ***paths, char *error, size_t capacity)
{
    return sh_package_native_names_assign_bounded(context, index, identity, original_binding,
        files, count, SH_DECL_SERVER_NAME_CAP, SH_DECL_SERVER_SOURCE_CAP,
        name, paths, error, capacity);
}

int sh_package_native_names_assign_members(void *context, size_t index, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    const sh_package_link_inputs *inputs = context;
    size_t movie, atlas;
    if (error && capacity) *error = 0;
    if (names) *names = NULL;
    if (!inputs || index >= inputs->count || !inputs->units || !names || !name || !paths)
        return nn_fail(error, capacity, "invalid coupled native name assignment", NULL);
    const sh_package_link_unit *unit = inputs->units + index;
    if (!unit->family || !unit->name || !nn_swf_members(unit, files, count, &movie, &atlas) ||
        !paths[movie] || strcmp(name, paths[movie]) || !nn_swf_pair(paths[movie], paths[atlas]))
        return nn_fail(error, capacity, "coupled movie assignment lost its native file pair", NULL);
    *names = calloc(count, sizeof(**names));
    if (!*names) return nn_fail(error, capacity, "cannot allocate native member names", unit);
    for (size_t i = 0; i < count; i++) if (!((*names)[i] = _strdup(paths[i])))
        return nn_fail(error, capacity, "cannot allocate native member name", unit);
    return 1;
}
