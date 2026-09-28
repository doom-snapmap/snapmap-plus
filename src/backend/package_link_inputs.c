#include "package_link_inputs.h"
#include "native_filename.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct li_rule { const char *family, *prefix, *source, *cooked; } li_rule;
static const li_rule li_rules[] = {
    {"model", "cooked/model/", ".lwo", ".bmodel"},
    {"basemodel", "generated/basemodel/", ".md6mesh", ".bmd6model"},
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

char *sh_package_cooked_path(const char *family, const char *name)
{
    char *canonical = sh_package_engine_path(name), *raw = NULL, *out = NULL;
    if (!family || !canonical) goto done;
    if (!strcmp(family, "image")) { out = li_generated_path(canonical, "generated/image/", ".bimage"); goto done; }
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
        *family = rule->family; result = 1; break;
    }
done:
    free(canonical); return result;
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
                const char *family = NULL;
                char *name = NULL;
                int status = sh_package_cooked_identity(path, &family, &name);
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
