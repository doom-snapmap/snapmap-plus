#include "package_link_inputs.h"
#include "native_filename.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct li_rule { const char *family, *prefix, *source, *cooked; } li_rule;
static const li_rule li_rules[] = {
    {"model", "cooked/model/", ".lwo", ".bmodel"},
    {"basemodel", "generated/basemodel/", ".md6mesh", ".bmd6model"},
    {"skeleton", "generated/skeleton/", ".md6skl", ".bmd6skl"},
    {"anim", "cooked/anim/", ".md6anim", ".bmd6anim"}
};
typedef struct li_row {
    size_t scope;
    char *family, *name;
    const sh_compiled_resource *file;
} li_row;

/* idResource keeps option-bearing source extensions, strips an ordinary source
 * extension, and accepts its explicit cooked extension unchanged. A leading
 * underscore names a native object, not a generated file. Options must already
 * be the manager spelling; this does not infer material keyword defaults. */
static char *li_generated_path(char *name, const char *prefix_text, const char *suffix_text)
{
    const char *options = strchr(name, '$'), *extension = NULL;
    const char *end = options ? options : name + strlen(name);
    size_t length = strlen(name), prefix = strlen(prefix_text), suffix = strlen(suffix_text);
    char *out;
    if (*name == '_') return NULL;
    if (length > suffix && !strcmp(name + length - suffix, suffix_text)) return _strdup(name);
    for (const char *p = end; p > name && p[-1] != '/';) {
        if (*--p == '.') { extension = p; break; }
    }
    if (extension && (size_t)(end - extension) == suffix &&
        !memcmp(extension, suffix_text, suffix)) return _strdup(name);
    if (!options && extension) length = (size_t)(extension - name);
    if (!length || name[length - 1] == '/' || length > SIZE_MAX - prefix - suffix - 1) return NULL;
    out = malloc(prefix + length + suffix + 1);
    if (!out) return NULL;
    memcpy(out, prefix_text, prefix); memcpy(out + prefix, name, length);
    memcpy(out + prefix + length, suffix_text, suffix + 1);
    /* The native generated-path sanitizer replaces punctuation, including '-'.
     * Safe package paths are already lowercase and use forward slashes. */
    for (size_t i = prefix; i < prefix + length; i++) {
        unsigned char c = (unsigned char)out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '$' || c == '.' || c == '/')) out[i] = '_';
    }
    char *canonical = sh_package_engine_path(out);
    free(out); return canonical;
}

/* A navigation file keeps its manager kind: the name "<x>.aas_<kind>" cooks
 * to "generated/<x>.baas_<kind>" by the native generated-filename contract
 * (every named installed aas row, long-name splits included). Returns the
 * marker in name's basename, or NULL. */
static const char *li_aas_marker(const char *name, const char *marker)
{
    const char *slash = strrchr(name, '/'), *base = slash ? slash + 1 : name, *at = NULL;
    for (const char *p = strstr(base, marker); p; p = strstr(p + 1, marker)) at = p;
    if (!at || at == base || !at[strlen(marker)]) return NULL;
    for (const char *p = at + strlen(marker); *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return NULL;
    return at;
}
static char *li_aas_path(const char *canonical)
{
    const char *at = li_aas_marker(canonical, ".aas_");
    char suffix[64], path[1024];
    if (!at || strlen(at) >= sizeof(suffix) - 1) return NULL;
    snprintf(suffix, sizeof(suffix), ".baas_%s", at + 5);
    return sh_native_generated_filename("generated/", canonical, suffix, path, sizeof(path)) ?
        sh_package_engine_path(path) : NULL;
}
/* A module's collision is named by its own path (every named installed cm
 * row outside generated/). A model's collision cooks under generated/cm/
 * through the generated-path sanitizer, which is not invertible, so that
 * form has no adapter. */
static int li_module_collision(const char *canonical)
{
    size_t length = strlen(canonical);
    return length > 4 && !strcmp(canonical + length - 4, ".bcm") && canonical[length - 5] != '/' &&
        strncmp(canonical, "generated/", 10);
}

char *sh_package_cooked_path(const char *family, const char *name)
{
    char *canonical = sh_package_engine_path(name), *raw = NULL, *out = NULL;
    if (!family || !canonical) goto done;
    if (!strcmp(family, "aas")) { out = li_aas_path(canonical); goto done; }
    if (!strcmp(family, "cm")) { out = li_module_collision(canonical) ? _strdup(canonical) : NULL; goto done; }
    if (!strcmp(family, "image")) { out = li_generated_path(canonical, "generated/image/", ".bimage"); goto done; }
    /* idMD6Skel Load uses the common idResource generated-path builder, with
     * its skeleton manager and bmd6skl extension (VK 1414ab4b0 / GL 14149eab0).
     * Keep its option, sanitizer and explicit cooked-name rules. */
    if (!strcmp(family, "skeleton")) { out = li_generated_path(canonical, "generated/skeleton/", ".bmd6skl"); goto done; }
    if (!strcmp(family, "transsortatlas")) { out = li_generated_path(canonical, "generated/transsortatlas/", ".bimage"); goto done; }
    if (!strcmp(family, "discreteanimation")) {
        out = li_generated_path(canonical, "generated/discreteanimation/", ".dmodel"); goto done;
    }
    if (!strcmp(family, "staticparticlemodel")) {
        char path[256];
        if (sh_native_generated_filename("generated/", canonical, ".pmodel", path, sizeof(path)))
            out = sh_package_engine_path(path);
        goto done;
    }
    for (size_t i = 0; i < sizeof(li_rules) / sizeof(li_rules[0]); i++) {
        const li_rule *rule = li_rules + i;
        size_t length = strlen(canonical), suffix = strlen(rule->source), size;
        if (strcmp(family, rule->family)) continue;
        if (length > suffix && !strcmp(canonical + length - suffix, rule->source)) length -= suffix;
        if (!length || canonical[length - 1] == '/' ||
            length > SIZE_MAX - strlen(rule->prefix) - strlen(rule->cooked) - 1) break;
        size = length + strlen(rule->prefix) + strlen(rule->cooked) + 1;
        raw = malloc(size);
        if (raw) {
            size_t prefix = strlen(rule->prefix);
            memcpy(raw, rule->prefix, prefix); memcpy(raw + prefix, canonical, length);
            memcpy(raw + prefix + length, rule->cooked, strlen(rule->cooked) + 1);
            out = sh_package_engine_path(raw);
        }
        break;
    }
done:
    free(canonical); free(raw); return out;
}

int sh_package_cooked_identity(const char *path, const char **family, char **name)
{
    char *canonical = sh_package_engine_path(path);
    int result = 0;
    if (family) *family = NULL;
    if (name) *name = NULL;
    if (!family || !name || !canonical) goto done;
    /* These managers accept explicit cooked names verbatim. Retain that
     * identity instead of inventing a source extension: several source
     * spellings can cook to this same physical file. */
    size_t length = strlen(canonical);
    if (length > 7 && !strcmp(canonical + length - 7, ".bimage") &&
        canonical[length - 8] != '/') {
        *family = !strncmp(canonical, "generated/transsortatlas/", 25) ? "transsortatlas" : "image";
        *name = canonical; canonical = NULL;
        result = 1; goto done;
    }
    if (length > 7 && !strcmp(canonical + length - 7, ".dmodel") && canonical[length - 8] != '/') {
        *family = "discreteanimation"; *name = canonical; canonical = NULL;
        result = 1; goto done;
    }
    if (li_module_collision(canonical)) {
        *family = "cm"; *name = canonical; canonical = NULL;
        result = 1; goto done;
    }
    if (length > 10 && !strncmp(canonical, "generated/", 10) && li_aas_marker(canonical + 10, ".baas_")) {
        /* The generated root removed and the manager's kind marker restored.
         * A path a long-name split could also produce names no single
         * resource, so it keeps no identity. */
        const char *at = li_aas_marker(canonical + 10, ".baas_");
        size_t stem = (size_t)(at - canonical) - 10;
        char *candidate = malloc(length - 10), *forward = NULL, *joined = NULL;
        if (!candidate) { result = -1; goto done; }
        memcpy(candidate, canonical + 10, stem);
        snprintf(candidate + stem, length - 10 - stem, ".aas_%s", at + 6);
        forward = li_aas_path(candidate);
        if (forward && !strcmp(forward, canonical)) {
            const char *slash = strrchr(candidate, '/'), *previous = slash;
            while (previous && previous > candidate && previous[-1] != '/') previous--;
            if (slash && previous && slash - previous == 30 && (joined = _strdup(candidate)) != NULL) {
                memmove(joined + (slash - candidate), joined + (slash - candidate) + 1, strlen(slash));
                free(forward); forward = li_aas_path(joined);
                if (forward && !strcmp(forward, canonical)) { free(candidate); candidate = NULL; }
            } else if (slash && previous && slash - previous == 30) { result = -1; free(candidate); candidate = NULL; }
            if (candidate) { *family = "aas"; *name = candidate; candidate = NULL; result = 1; }
        }
        free(candidate); free(forward); free(joined); goto done;
    }
    if (length >= 17 && !strncmp(canonical, "generated/", 10) &&
        !strcmp(canonical + length - 7, ".pmodel")) {
        /* No source extension is invented. Retain the cooked suffix and any
         * long-name pieces under the generated root; replacing that suffix
         * reproduces this path, including dots elsewhere in the basename. */
        char *candidate = _strdup(canonical + 10), *forward;
        if (!candidate) { result = -1; goto done; }
        forward = sh_package_cooked_path("staticparticlemodel", candidate);
        if (forward && !strcmp(forward, canonical)) {
            *family = "staticparticlemodel"; *name = candidate; candidate = NULL; result = 1;
        }
        free(forward); free(candidate); goto done;
    }
    for (size_t i = 0; i < sizeof(li_rules) / sizeof(li_rules[0]); i++) {
        const li_rule *rule = li_rules + i;
        size_t length = strlen(canonical), prefix = strlen(rule->prefix), suffix = strlen(rule->cooked), body;
        if (length <= prefix + suffix || strncmp(canonical, rule->prefix, prefix) ||
            strcmp(canonical + length - suffix, rule->cooked)) continue;
        body = length - prefix - suffix;
        if (canonical[prefix + body - 1] == '/' || body > SIZE_MAX - strlen(rule->source) - 1) break;
        *name = malloc(body + strlen(rule->source) + 1);
        if (!*name) { result = -1; break; }
        memcpy(*name, canonical + prefix, body);
        memcpy(*name + body, rule->source, strlen(rule->source) + 1);
        if (!strcmp(rule->family, "skeleton")) {
            char *forward = sh_package_cooked_path(rule->family, *name);
            int matches = forward && !strcmp(forward, canonical);
            free(forward);
            if (!matches) { free(*name); *name = NULL; break; }
        }
        *family = rule->family; result = 1; break;
    }
    /* Option-bearing spellings cannot always be inverted to a source name.
     * The native skeleton manager accepts an explicit cooked name verbatim. */
    if (!result && length > 8 && !strcmp(canonical + length - 8, ".bmd6skl") &&
        canonical[length - 9] != '/') {
        *family = "skeleton"; *name = canonical; canonical = NULL; result = 1;
    }
done:
    free(canonical); return result;
}

/* Every catalogued file and snapmodule row is named by its own path. */
const char *sh_package_generated_family(const sh_compiled_resource *file, const sh_resource_catalog *catalog)
{
    const char *family = NULL;
    if (!file || !file->generated || !file->generated_input_count || !file->generated_inputs) return NULL;
    for (size_t i = 0; i < file->generated_input_count; i++) {
        const sh_resource_catalog_entry *const *entries = NULL;
        size_t count = file->generated_inputs[i] ?
            sh_resource_catalog_find_provider(catalog, file->generated_inputs[i], &entries) : 0;
        if (!count) return NULL;
        for (size_t e = 0; e < count; e++) {
            if (!*entries[e]->type || strcmp(entries[e]->name, entries[e]->path) ||
                (family && strcmp(family, entries[e]->type))) return NULL;
            family = entries[e]->type;
        }
    }
    return family;
}

static int li_order(const void *a, const void *b)
{
    const li_row *x = a, *y = b;
    int order;
    if (x->scope != y->scope) return x->scope < y->scope ? -1 : 1;
    order = strcmp(x->family, y->family);
    if (!order) order = strcmp(x->name, y->name);
    if (!order) order = strcmp(x->file->engine_path, y->file->engine_path);
    return order;
}
static int li_same(const li_row *a, const li_row *b)
{ return a->scope == b->scope && !strcmp(a->family, b->family) && !strcmp(a->name, b->name); }

static int li_add(li_row **rows, size_t *count, size_t *capacity, size_t scope,
    const sh_compiled_resource *file, const char *family, const char *name)
{
    li_row row = {scope, NULL, NULL, file};
    if (!family || !*family || !name || !*name) return 0;
    row.family = _strdup(family); row.name = _strdup(name);
    if (!row.family || !row.name) goto bad;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 64;
        li_row *grown;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown) ||
            !(grown = realloc(*rows, next * sizeof(*grown)))) goto bad;
        *rows = grown; *capacity = next;
    }
    (*rows)[(*count)++] = row; return 1;
bad:
    free(row.family); free(row.name); return 0;
}

void sh_package_link_inputs_free(sh_package_link_inputs *inputs)
{
    if (!inputs) return;
    for (size_t i = 0; i < inputs->count; i++) {
        free((char *)inputs->units[i].family); free((char *)inputs->units[i].name);
        free((void *)inputs->units[i].files);
    }
    free(inputs->units); free(inputs->scopes); memset(inputs, 0, sizeof(*inputs));
}

int sh_package_link_inputs_open(const sh_package_compilation *const *scopes,
    size_t count, const sh_resource_catalog *catalog, sh_package_link_inputs *out,
    char *error, size_t capacity)
{
    li_row *rows = NULL;
    size_t used = 0, allocated = 0, groups = 0;
    const char *path = NULL;
    const char *reason = "cannot allocate native source identities";
    char fallback[1024];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!out) { snprintf(error, capacity, "native source output is required"); return 0; }
    sh_package_link_inputs_free(out);
    if ((!scopes && count) || count > SIZE_MAX / sizeof(*scopes)) {
        reason = "invalid native source scopes"; goto done;
    }
    for (size_t s = 0; s < count; s++) {
        const sh_package_compilation *scope = scopes[s];
        if (!scope || (!scope->resources && scope->resource_count) ||
            scope->resource_count > SIZE_MAX / sizeof(*scope->resources)) {
            reason = "invalid native source compilation"; goto done;
        }
        for (size_t r = 0; r < scope->resource_count; r++) {
            const sh_compiled_resource *file = scope->resources + r;
            const sh_resource_catalog_entry *const *entries = NULL;
            size_t entries_count;
            path = file->engine_path;
            if (!path || !*path) { reason = "native source has no provider path"; goto done; }
            if (file->type || file->name) {
                if (!file->type || !*file->type || !file->name || !*file->name) {
                    reason = "native source has an incomplete declaration identity"; goto done;
                }
                if (!li_add(&rows, &used, &allocated, s, file, file->type, file->name)) goto done;
                continue;
            }
            entries_count = sh_resource_catalog_find_provider(catalog, path, &entries);
            if (entries_count) {
                for (size_t e = 0; e < entries_count; e++) {
                    if (!*entries[e]->type || !*entries[e]->name) {
                        reason = "catalog resource needs its unnamed-reader adapter"; goto done;
                    }
                    if (!li_add(&rows, &used, &allocated, s, file, entries[e]->type, entries[e]->name)) goto done;
                }
            } else {
                const char *family = sh_package_generated_family(file, catalog);
                char *name = NULL;
                int status = family ? ((name = _strdup(path)) != NULL ? 1 : -1) :
                    sh_package_cooked_identity(path, &family, &name);
                if (status != 1) {
                    reason = status < 0 ? "cannot allocate cooked resource identity" :
                        "provider path needs its native resource identity adapter";
                    free(name); goto done;
                }
                int added = li_add(&rows, &used, &allocated, s, file, family, name);
                free(name); if (!added) goto done;
            }
        }
    }
    path = NULL;
    if (used > 1) qsort(rows, used, sizeof(*rows), li_order);
    for (size_t i = 0; i < used; i++) if (!i || !li_same(rows + i - 1, rows + i)) groups++;
    if (groups > SIZE_MAX / sizeof(*out->units) || groups > SIZE_MAX / sizeof(*out->scopes)) goto done;
    out->units = calloc(groups ? groups : 1, sizeof(*out->units));
    out->scopes = calloc(groups ? groups : 1, sizeof(*out->scopes));
    if (!out->units || !out->scopes) goto done;
    for (size_t i = 0; i < used;) {
        size_t end = i + 1, files = 0;
        const sh_compiled_resource **members;
        while (end < used && li_same(rows + i, rows + end)) end++;
        for (size_t j = i; j < end; j++) if (j == i || strcmp(rows[j - 1].file->engine_path, rows[j].file->engine_path)) files++;
        if (files > SIZE_MAX / sizeof(*members) || !(members = calloc(files, sizeof(*members)))) goto done;
        sh_package_link_unit *unit = out->units + out->count;
        out->scopes[out->count++] = rows[i].scope;
        unit->files = members; unit->compilation = scopes[rows[i].scope];
        unit->family = _strdup(rows[i].family); unit->name = _strdup(rows[i].name);
        if (!unit->family || !unit->name) goto done;
        for (size_t j = i; j < end; j++) {
            if (j > i && !strcmp(rows[j - 1].file->engine_path, rows[j].file->engine_path)) {
                if (rows[j - 1].file != rows[j].file) {
                    reason = "compiled scope repeats a provider path"; goto done;
                }
                continue;
            }
            members[unit->file_count++] = rows[j].file;
        }
        i = end;
    }
    ok = 1;
done:
    for (size_t i = 0; i < used; i++) { free(rows[i].family); free(rows[i].name); }
    free(rows);
    if (!ok) {
        snprintf(error, capacity, "%s%s%s", reason, path ? ": " : "", path ? path : "");
        sh_package_link_inputs_free(out);
    }
    return ok;
}

size_t sh_package_link_inputs_find(const sh_package_link_inputs *inputs,
    size_t scope, const char *family, const char *name)
{
    size_t low = 0, high = inputs ? inputs->count : 0;
    if (!inputs || !family || !name || (!inputs->units && high) || (!inputs->scopes && high)) return SIZE_MAX;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const sh_package_link_unit *unit = inputs->units + mid;
        int order = inputs->scopes[mid] == scope ? 0 : inputs->scopes[mid] < scope ? -1 : 1;
        if (!order) order = strcmp(unit->family, family);
        if (!order) order = strcmp(unit->name, name);
        if (order < 0) low = mid + 1; else high = mid;
    }
    return low < inputs->count && inputs->scopes[low] == scope &&
        !strcmp(inputs->units[low].family, family) && !strcmp(inputs->units[low].name, name) ? low : SIZE_MAX;
}

static int mf_starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }
static int mf_ends(const char *s, size_t n, const char *suffix)
{ size_t k = strlen(suffix); return n >= k && !strcmp(s + n - k, suffix); }
/* Split "<R>/<file>": R is everything before the last separator, B its last
 * component. */
static int mf_split(const char *rest, size_t *r, const char **file, const char **b, size_t *b_length)
{
    const char *slash = strrchr(rest, '/'), *base;
    if (!slash || slash == rest || !slash[1]) return 0;
    *r = (size_t)(slash - rest); *file = slash + 1;
    for (base = slash; base > rest && base[-1] != '/'; base--) {}
    *b = base; *b_length = (size_t)(slash - base);
    return *b_length > 0;
}
static int mf_digits(const char *s, size_t n)
{
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
/* The role of a file named for its module (basename b), or NULL. */
static const char *mf_file_role(const char *file, const char *b, size_t b_length)
{
    size_t n = strlen(file);
    if (n <= b_length || strncmp(file, b, b_length)) return NULL;
    const char *tail = file + b_length;
    size_t t = n - b_length;
    if (!strcmp(tail, "_floor_collision.bcm")) return "collision";
    if (!strcmp(tail, ".sbsp")) return "sound";
    if (!strcmp(tail, ".ambientsh")) return "ambient";
    if (!strcmp(tail, "_single_surface_walls.bmodel") || !strcmp(tail, "_walls_floor.bmodel")) return "blueprint";
    if (t > 6 + 7 && !strncmp(tail, "_wall_", 6) && mf_ends(tail, t, ".bmodel") && mf_digits(tail + 6, t - 6 - 7))
        return "blueprint";
    /* Other files named for the module (the AAS preview models
     * <B>_aas_monster<N>.bmodel among them); files the declaration names, such
     * as its volumes' collision, are ordinary references instead. */
    /* A module's declaration in its category directory can share that
     * directory's name as a prefix; it is a module, not a member. */
    if ((tail[0] == '_' || tail[0] == '.') && tail[1] && !mf_ends(tail, t, ".decl")) return "named";
    return NULL;
}
/* Where path names a member of module maps/modules/<R>: R's offset and length
 * within path, and the member's role. */
static int mf_member(const char *path, size_t *at, size_t *r, const char **role)
{
    size_t n = path ? strlen(path) : 0, rl;
    const char *rest, *file, *b; size_t b_length;
    if (!n || strstr(path, "..") || strchr(path, 92) || strstr(path, "//")) return 0;
    if (mf_starts(path, "generated/decls/snapmoduleinfo/") && mf_ends(path, n, ".decl")) {
        *at = 31; *r = n - 31 - 5; *role = "card"; return *r > 0;
    }
    if (mf_starts(path, "maps/modules/palettes/") && mf_ends(path, n, ".bmodel")) {
        const char *slash = strchr(path + 22, '/');
        if (!slash || slash == path + 22) return 0;
        *at = (size_t)(slash + 1 - path);
        if (n <= *at + 7) return 0;
        *r = n - *at - 7; *role = "render"; return 1;
    }
    if (mf_starts(path, "generated/maps/modules/")) {
        rest = path + 23;
        if (!mf_split(rest, &rl, &file, &b, &b_length)) return 0;
        if (strlen(file) <= b_length + 6 || strncmp(file, b, b_length) || strncmp(file + b_length, ".baas_", 6)) return 0;
        *at = 23; *r = rl; *role = "navigation"; return 1;
    }
    if (!mf_starts(path, "maps/modules/") || mf_starts(path, "maps/modules/palettes/")) return 0;
    rest = path + 13;
    if (mf_ends(path, n, "/_combo/world.bcm")) {
        if (n <= 13 + 17) return 0;
        *at = 13; *r = n - 13 - 17; *role = "collision"; return 1;
    }
    if (mf_ends(path, n, "/_combo/_world.tome")) {
        if (n <= 13 + 19) return 0;
        *at = 13; *r = n - 13 - 19; *role = "occlusion"; return 1;
    }
    {
        const char *probes = strstr(rest, "/lightprobes/");
        if (probes && probes > rest && mf_ends(path, n, ".bimage") && !strchr(probes + 13, '/') &&
            probes[13] != '.') { *at = 13; *r = (size_t)(probes - rest); *role = "probe"; return 1; }
    }
    if (!mf_split(rest, &rl, &file, &b, &b_length) || !(*role = mf_file_role(file, b, b_length))) return 0;
    *at = 13; *r = rl; return 1;
}
int sh_package_module_member(const char *path, char *module, size_t capacity, const char **role)
{
    size_t at, r; const char *kind;
    if (!mf_member(path, &at, &r, &kind)) return 0;
    if (module) {
        if (r + 14 > capacity) return 0;
        memcpy(module, "maps/modules/", 13); memcpy(module + 13, path + at, r); module[13 + r] = 0;
    }
    if (role) *role = kind;
    return 1;
}
char *sh_package_module_member_rename(const char *path, const char *from, const char *to)
{
    size_t at, r; const char *role, *from_b, *to_b;
    if (!from || !to || !mf_starts(from, "maps/modules/") || !mf_starts(to, "maps/modules/") ||
        !mf_member(path, &at, &r, &role)) return NULL;
    from += 13; to += 13;
    if (!*to || strlen(from) != r || strncmp(path + at, from, r)) return NULL;
    from_b = strrchr(from, '/'); from_b = from_b ? from_b + 1 : from;
    to_b = strrchr(to, '/'); to_b = to_b ? to_b + 1 : to;
    size_t from_bl = strlen(from_b), to_bl = strlen(to_b);
    const char *tail = path + at + r;
    /* The basename recurs only in a file named for the module. */
    int named = tail[0] == '/' && !strncmp(tail + 1, from_b, from_bl) && strcmp(role, "card") &&
        strcmp(role, "render") && strcmp(role, "probe") && strncmp(tail, "/_combo/", 8);
    size_t length = at + strlen(to) + (named ? 1 + to_bl + strlen(tail + 1 + from_bl) : strlen(tail));
    char *out = malloc(length + 1);
    if (!out) return NULL;
    memcpy(out, path, at); out[at] = 0; strcat(out, to);
    if (named) { strcat(out, "/"); strcat(out, to_b); strcat(out, tail + 1 + from_bl); }
    else strcat(out, tail);
    return out;
}
