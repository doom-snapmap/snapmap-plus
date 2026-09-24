#include "package_library.h"
#include "package_native_names.h"
#include "package_native_registry.h"
#include "package_hud.h"
#include "package_sound.h"
#include "decl_native_rebind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pl_original {
    char *path;
    unsigned char *body;
    size_t length;
    int status;
} pl_original;
typedef struct pl_originals {
    sh_package_baseline_reader read;
    void *context;
    pl_original *files;
    size_t count, capacity;
} pl_originals;
typedef struct pl_contributions pl_contributions;
static void pl_contributions_close(pl_contributions *c);

/* One original snapshot covers both library passes and native material
 * inspection. Retain absence and failures too: retries inside this compilation
 * must not observe a different original/schema halfway through deduplication. */
static int pl_read_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    pl_originals *s = context;
    size_t low = 0, high = s->count;
    *body = NULL; *length = 0;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->files[mid].path, path) < 0) low = mid + 1; else high = mid;
    }
    if (low == s->count || strcmp(s->files[low].path, path)) {
        pl_original file = {0};
        if (s->count == s->capacity) {
            size_t next = s->capacity ? s->capacity * 2 : 64;
            pl_original *grown = next > s->capacity && next <= SIZE_MAX / sizeof(*grown) ?
                realloc(s->files, next * sizeof(*grown)) : NULL;
            if (!grown) return -1;
            s->files = grown; s->capacity = next;
        }
        file.path = _strdup(path); if (!file.path) return -1;
        file.status = s->read ? s->read(s->context, path, &file.body, &file.length) : 0;
        if (file.status < 0 || file.status > 3 || (file.status && !file.body) || file.length == SIZE_MAX)
            file.status = -1;
        if (file.status <= 0) { free(file.body); file.body = NULL; file.length = 0; }
        memmove(s->files + low + 1, s->files + low, (s->count - low) * sizeof(*s->files));
        s->files[low] = file; s->count++;
    }
    const pl_original *file = s->files + low;
    if (file->status <= 0) return file->status;
    *body = malloc(file->length + 1); if (!*body) return -1;
    memcpy(*body, file->body, file->length); (*body)[file->length] = 0;
    *length = file->length; return file->status;
}

typedef struct pl_build {
    sh_package_scope *views;
    sh_package_compilation **scopes;
    size_t count, authored_count;
    sh_package_source_link *reader;
    sh_package_scope shared_view;
    sh_package_compilation *shared;
    sh_package_link_output linked;
    const sh_package_library_native *native;
    sh_package_source_reader **material_readers;
    sh_package_virtual_materials *materials;
    sh_package_audio_program *audio;
    pl_originals originals[2];
    const sh_package_provenance *contexts;
    sh_package_native_registry *identities;
    pl_contributions *contributions;
} pl_build;

static void pl_native_close(pl_build *b)
{
    sh_package_audio_program_close(b->audio); b->audio = NULL;
    sh_package_virtual_material_close(b->materials); b->materials = NULL;
    if (b->material_readers) for (size_t i = 0; i < b->count; i++)
        sh_package_source_reader_close(b->material_readers[i]);
    free(b->material_readers); b->material_readers = NULL;
}

static int pl_native_open(pl_build *b, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, char *error, size_t capacity)
{
    if (b->native && b->native->audio) {
        b->audio = sh_package_audio_program_open((const sh_package_compilation *const *)b->scopes,
            b->count, b->native->audio, error, capacity);
        if (!b->audio) return 0;
    }
    if (!b->native || !b->native->atlas) return 1;
    b->materials = sh_package_virtual_material_open((const sh_package_compilation *const *)b->scopes,
        b->count, original, original_context);
    b->material_readers = calloc(b->count, sizeof(*b->material_readers));
    if (!b->materials || !b->material_readers) return 0;
    for (size_t i = 0; i < b->count; i++) {
        b->material_readers[i] = sh_package_source_reader_open(b->scopes[i], catalog,
            original, original_context, registry, reflection, error, capacity);
        if (!b->material_readers[i]) return 0;
    }
    return 1;
}

static int pl_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    pl_build *b = context;
    return b->native && b->native->source.admit ?
        b->native->source.admit(b->native->source.context, scope, family, name, semantics, error, capacity) : 0;
}

static int pl_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    pl_build *b = context;
    if (!strcmp(family, "sound") && b->audio) {
        sh_audio_graph_root root;
        unsigned char combined[64] = {0}; sh_package_file_identity identity;
        int original = b->native->source.semantics ?
            b->native->source.semantics(b->native->source.context, scope, family, name,
                combined, error, capacity) : 0;
        if (original < 0 || original > 1 || !sh_package_audio_program_prepare(b->audio, error, capacity) ||
            !sh_package_audio_program_root(b->audio, scope, name, &root, error, capacity)) return -1;
        memcpy(combined + 32, root.identity, 32);
        if (!sh_package_bytes_identity(combined, sizeof(combined), &identity)) return -1;
        memcpy(semantics, identity.digest, 32); return original && root.original;
    }
    if (!strcmp(family, "material") && b->materials) {
        sh_package_virtual_atlas atlas = {0}; int original = 0;
        if (sh_package_virtual_material_cached(b->materials, scope, name, semantics, &original)) return original;
        char *page_file = sh_package_source_material_path(b->material_readers[scope], name,
            "landPageFile", error, capacity);
        if (!page_file) return -1;
        int status = b->native->atlas(b->native->source.context, scope, b->scopes[scope], name,
            page_file, &atlas, error, capacity);
        if (status != 1) {
            if (!*error) snprintf(error, capacity, "material page provider is unavailable: %s", name);
            free(page_file);
            sh_virtual_page_source_release(atlas.pages);
            return -1;
        }
        status = sh_package_virtual_material_bind(b->materials, scope, name, &atlas,
            semantics, &original, error, capacity);
        sh_virtual_page_source_release(atlas.pages);
        free(page_file);
        if (status < 0) return -1;
        return original;
    }
    return b->native && b->native->source.semantics ?
        b->native->source.semantics(b->native->source.context, scope, family, name, semantics, error, capacity) : 0;
}

static int pl_material_companions(void *context, size_t scope, const char *material,
    int (*visit)(void *, const char *, const sh_compiled_resource *), void *visit_context,
    char *error, size_t capacity)
{
    pl_build *b = context; unsigned char semantics[32];
    return !b->materials || (pl_semantics(context, scope, "material", material, semantics, error, capacity) >= 0 &&
        sh_package_virtual_material_witnesses(b->materials, scope, material, visit, visit_context, error, capacity));
}
static int pl_material_seeds(void *context, size_t scope,
    int (*visit)(void *, const char *), void *visit_context, char *error, size_t capacity)
{
    pl_build *b = context;
    return !b->materials || sh_package_virtual_material_seeds(b->materials, scope,
        visit, visit_context, error, capacity);
}

static int pl_shared_declaration(const char *type)
{
    /* Entity definitions qualify only after the dependency walk proves no
     * gameplay contribution. This retains product defaults and editorVars-only
     * extensions without aliasing their vanilla gameplay identity. */
    return type && (!strcmp(type, "entitydef") || !strncmp(type, "snapeditor", 10) ||
        !strncmp(type, "snappropertyinspector", 21));
}
static int pl_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    pl_build *b = context;
    if (b->audio && !observing && unit->file_count && !strcmp(unit->family, "sound") &&
        !sh_package_audio_program_collect(b->audio, scope, unit->name, error, capacity)) return 0;
    return !b->native || !b->native->source.implicit ||
        b->native->source.implicit(b->native->source.context, scope, observing, unit,
            visit, visit_context, error, capacity) == 1;
}

static void pl_close(pl_build *b)
{
    sh_package_native_registry_close(b->identities);
    pl_contributions_close(b->contributions);
    sh_package_source_link_close(b->reader);
    pl_native_close(b);
    for (size_t i = 0; i < b->count; i++) {
        sh_package_compilation_free(b->scopes ? b->scopes[i] : NULL);
        if (b->views) sh_package_scope_close(b->views + i);
    }
    free(b->scopes); free(b->views);
    sh_package_compilation_free(b->shared); sh_package_scope_close(&b->shared_view);
    sh_package_link_output_free(&b->linked);
    for (size_t k = 0; k < 2; k++) {
        pl_originals *s = b->originals + k;
        for (size_t i = 0; i < s->count; i++) { free(s->files[i].path); free(s->files[i].body); }
        free(s->files);
    }
}

static int pl_source_owners(void *context, size_t scope, const char *family, const char *name,
    const sh_package_owners **owners, const sh_package_owners **gameplay, char *error, size_t capacity)
{
    pl_build *b = context;
    *owners = *gameplay = NULL;
    if (!b->audio || strcmp(family, "sound"))
        return !b->native || !b->native->source.owners ||
            b->native->source.owners(b->native->source.context, scope, family, name,
                owners, gameplay, error, capacity) == 1;
    if (sh_package_audio_program_owners(b->audio, scope, name, owners, gameplay)) return 1;
    snprintf(error, capacity, "sound has no captured dependency ownership: %s", name); return 0;
}

void sh_package_library_free(sh_package_library *library)
{
    if (!library) return;
    sh_package_audio_program_close(library->audio);
    sh_package_compilation_free(library->compiled);
    sh_virtual_material_runtime_retire(&library->virtual_materials);
    sh_package_consumers_free(library->consumers);
    free(library->coverage);
    sh_package_provenance_free(&library->bindings); memset(library, 0, sizeof(*library));
}

/* Describe accounted reverse-discovery gaps with their supplying delivery. The
 * stock summary is one line; each affected scope names its package. */
static int pl_coverage(const pl_build *b, const sh_package_sources *sources, char **out)
{
    const sh_package_reverse_gap *gaps = NULL;
    size_t count = sh_package_source_link_reverse_gaps(b->reader, &gaps), stock = 0, capacity = 0;
    const sh_package_reverse_gap *first = NULL;
    char *text = NULL;
    *out = NULL;
    for (size_t i = 0; i < count; i++) if (gaps[i].scope == SIZE_MAX) { stock++; if (!first) first = gaps + i; }
    /* Measure, then write the identical text. */
    for (size_t pass = 0; count && pass < 2; pass++) {
        size_t used = 0;
        if (first) {
            int n = snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                "Toybox variant discovery skipped %zu stock SnapMap resource%s whose format is not yet supported; "
                "package edits reachable only through them add no variant (first: %s %s: %s).\n",
                stock, stock == 1 ? "" : "s", first->family, first->name, first->reason);
            if (n < 0) return 0;
            used += (size_t)n;
        }
        for (size_t i = 0; i < count; i++) if (gaps[i].scope != SIZE_MAX) {
            const char *label = "map source", *id = "";
            if (!b->contexts && gaps[i].scope < sources->component_count) {
                const sh_package_component *component = sources->components + gaps[i].scope;
                label = component->descriptor.name[0] ? component->descriptor.name :
                    sources->packages[component->owner].name;
                id = component->descriptor.id;
            }
            int n = snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                "%s%s%s%s: no toybox variant for stock %s %s because its format is not yet supported (%s).\n",
                label, *id ? " [" : "", id, *id ? "]" : "", gaps[i].family, gaps[i].name, gaps[i].reason);
            if (n < 0) return 0;
            used += (size_t)n;
        }
        if (!pass) {
            if (used == SIZE_MAX || !(text = malloc(used + 1))) return 0;
            capacity = used + 1; *text = 0;
        }
    }
    *out = text; return 1;
}

/* Policy and product defaults have their own view. Peer declaration bytes are
 * never passed through the unbound whole-file composer. */
static int pl_shared_sources(pl_build *b, const sh_package_sources *sources,
    char *error, size_t capacity)
{
    size_t *owners = NULL;
    if (sources->package_count > SIZE_MAX / sizeof(*owners) ||
        !(owners = calloc(sources->package_count ? sources->package_count : 1, sizeof(*owners)))) return 0;
    for (size_t i = 0; i < sources->package_count; i++) owners[i] = i;
    int ok = sh_package_scope_composed(sources, owners, sources->package_count,
        &b->shared_view, error, capacity);
    free(owners); if (!ok) return 0;
    for (size_t i = 0; i < sources->file_count; i++) b->shared_view.sources.files[i].engine_path = NULL;
    return 1;
}

static int pl_builtin_has_authored_editor(const sh_package_sources *scope, const char *path)
{
    char family[256];
    if (strncmp(path, "generated/decls/", 16)) return 0;
    const char *end = strchr(path + 16, '/');
    if (!end || end - path - 16 >= sizeof(family)) return 0;
    memcpy(family, path + 16, (size_t)(end - path - 16)); family[end - path - 16] = 0;
    if (!sh_package_editor_contribution_phase(family)) return 0;
    for (size_t i = 0; i < scope->file_count; i++)
        if (scope->files[i].engine_path && !strcmp(scope->files[i].engine_path, path)) return 1;
    return 0;
}

static int pl_product_scope(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    size_t count = b->shared->resource_count;
    if (!count) return 1;
    sh_package_resource_projection *projections = calloc(count, sizeof(*projections));
    const sh_compiled_resource **members = calloc(count, sizeof(*members));
    int ok = 0;
    if (!projections || !members) goto done;
    for (size_t i = 0; i < count; i++) {
        members[i] = b->shared->resources + i;
        projections[i] = (sh_package_resource_projection){members[i]->engine_path, members + i, 1};
    }
    b->scopes[b->count] = sh_package_compilation_project(sources, projections, count, NULL,
        b->shared->canonical_path, error, capacity);
    if (!b->scopes[b->count]) goto done;
    b->count++; ok = 1;
done:
    free(projections); free(members); return ok;
}

/* Baseline references must bind in an original-only context. Resolving a stock
 * inspector through a package which replaces it would misclassify additions as
 * unchanged. Product defaults have their own contribution scope. Seed only known
 * original editor identities; source_link
 * captures their closure from the verified original reader, never another pack. */
static int pl_baseline_scope(pl_build *b, const sh_package_sources *sources,
    sh_package_baseline_reader original, void *context, char *error, size_t capacity)
{
    sh_package_resource_projection *projections = NULL;
    sh_compiled_resource *seeds = NULL;
    const sh_compiled_resource **members = NULL;
    size_t maximum = b->shared->resource_count, count = 0;
    int ok = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (b->scopes[i]->resource_count > SIZE_MAX - maximum) return 0;
        maximum += b->scopes[i]->resource_count;
    }
    if (maximum > SIZE_MAX / sizeof(*seeds) || maximum > SIZE_MAX / sizeof(*projections)) return 0;
    projections = calloc(maximum ? maximum : 1, sizeof(*projections));
    members = calloc(maximum ? maximum : 1, sizeof(*members));
    seeds = calloc(maximum ? maximum : 1, sizeof(*seeds));
    if (!projections || !members || !seeds) goto done;
    for (size_t scope = 0; scope < b->count; scope++) {
        if (b->contexts && scope < b->authored_count &&
            b->contexts->contexts[scope].mode == SH_PACKAGE_CONTEXT_COMPOSED) continue;
        const sh_package_compilation *compiled = b->scopes[scope];
        for (size_t i = 0; i < compiled->resource_count; i++) {
            const sh_compiled_resource *file = compiled->resources + i;
            size_t prior;
            if (!pl_shared_declaration(file->type)) continue;
            for (prior = 0; prior < count; prior++) if (!strcmp(projections[prior].engine_path, file->engine_path)) break;
            if (prior < count) continue;
            sh_compiled_resource *seed = seeds + count;
            int status = original ? original(context, file->engine_path, &seed->body, &seed->body_length) : 0;
            if (status < 0 || (status && !seed->body)) goto done;
            if (status != 1 && status != 3) { free(seed->body); memset(seed, 0, sizeof(*seed)); continue; }
            seed->engine_path = file->engine_path; seed->source = SIZE_MAX;
            seed->baseline_known = status; seed->native_original = status == 1;
            members[count] = seed;
            projections[count] = (sh_package_resource_projection){file->engine_path, members + count, 1};
            count++;
        }
    }
    b->scopes[b->count] = sh_package_compilation_project(sources, projections, count, NULL,
        b->shared->canonical_path, error, capacity);
    if (!b->scopes[b->count]) goto done;
    b->count++; ok = 1;
done:
    if (seeds) for (size_t i = 0; i < maximum; i++) free(seeds[i].body);
    free(seeds); free(projections); free(members); return ok;
}

typedef struct pl_assignment {
    const sh_package_link_inputs *inputs;
    const unsigned char *shared;
    int materials_prepared;
} pl_assignment;

static int pl_assign(void *context, size_t index, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    pl_assignment *a = context;
    const sh_package_link_unit *unit = a->inputs->units + index;
    if (a->shared[index] && pl_shared_declaration(unit->family)) {
        /* This is the one composed editor object, not a claim that its bytes
         * equal vanilla. Its private dependencies would have disqualified it. */
        *name = _strdup(unit->name); *paths = calloc(count ? count : 1, sizeof(**paths));
        if (!*name || !*paths) return 0;
        for (size_t i = 0; i < count; i++) if (!((*paths)[i] = _strdup(files[i]->engine_path))) return 0;
        return 1;
    }
    /* A non-generic material's renamed identity needs an inspected native atlas
     * binding, including a proven no-row result for a nonvirtual material. */
    if (!a->materials_prepared && !original_binding && !strcmp(unit->family, "material") && strncmp(unit->name, "generic", 7)) {
        snprintf(error, capacity, "linked material requires its prepared native atlas binding: %s", unit->name);
        return 0;
    }
    return sh_package_native_names_assign((void *)a->inputs, index, identity, original_binding,
        files, count, name, paths, error, capacity);
}

typedef struct pl_contribution_edge { char *slot; size_t target; } pl_contribution_edge;
typedef struct pl_contribution_unit {
    sh_package_compilation *owned;
    const sh_compiled_resource *file;
    pl_contribution_edge *edges;
    size_t count, capacity;
} pl_contribution_unit;
struct pl_contributions {
    sh_package_source_link *source;
    sh_package_link_inputs inputs;
    pl_contribution_unit *units;
    sh_package_link_classes classes;
    char **tokens;
    size_t token_count, pending_unit, pending_target;
    unsigned char *shared;
    pl_assignment *assignment;
    int dirty;
};

static void pl_contribution_unit_free(pl_contribution_unit *unit)
{
    for (size_t i = 0; i < unit->count; i++) free(unit->edges[i].slot);
    free(unit->edges); sh_package_compilation_free(unit->owned); memset(unit, 0, sizeof(*unit));
}
static void pl_tokens_free(pl_contributions *c)
{
    for (size_t i = 0; i < c->token_count; i++) free(c->tokens[i]);
    free(c->tokens); c->tokens = NULL; c->token_count = 0;
}
static void pl_contributions_close(pl_contributions *c)
{
    if (!c) return;
    if (c->units) for (size_t i = 0; i < c->inputs.count; i++) pl_contribution_unit_free(c->units + i);
    pl_tokens_free(c); sh_package_link_classes_free(&c->classes);
    free(c->units); free(c->inputs.units); free(c->shared); free(c);
}

/* A merged field is resolved by its retained slot binding, not by looking up
 * its restored spelling in the receiving unit. Unmodified fields retain their
 * original scoped reader, including native entity inheritance/class lookup. */
static int pl_bound_target(pl_contributions *c, size_t unit, const char *slot,
    const char *family, const char *name, size_t *target, char *error, size_t capacity)
{
    pl_contribution_unit *bound = c->units + unit;
    if (bound->owned) {
        for (size_t i = 0; i < bound->count; i++) if (!strcmp(bound->edges[i].slot, slot)) {
            *target = bound->edges[i].target;
            const sh_package_link_unit *value = c->inputs.units + *target;
            if (!strcmp(value->family, family) && !strcmp(value->name, name)) return 1;
            break;
        }
        snprintf(error, capacity, "composed editor binding changed at %s/%s: %s", family, name, slot);
        return 0;
    }
    return sh_package_source_link_resolve_unit(c->source, unit, family, name, target, error, capacity);
}

typedef struct pl_contribution_walk {
    pl_contributions *graph;
    size_t unit;
    sh_package_link_reference reference;
    void *context;
    pl_contribution_unit *capture;
    char *error;
    size_t capacity;
} pl_contribution_walk;

static int pl_derived_exists(void *context, const char *family, const char *name, int *exists)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    for (size_t i = 0; i < c->token_count; i++) if (!strcmp(c->tokens[i], name)) {
        const sh_package_link_unit *target = c->inputs.units + c->classes.representatives[i];
        *exists = !strcmp(target->family, family); return 1;
    }
    pl_contribution_unit *bound = c->units + walk->unit;
    int captured = 0;
    for (size_t i = 0; i < bound->count; i++) {
        const sh_package_link_unit *target = c->inputs.units + bound->edges[i].target;
        if (strcmp(target->name, name)) continue;
        captured = 1;
        if (!strcmp(target->family, family)) { *exists = 1; return 1; }
    }
    if (captured) { *exists = 0; return 1; }
    return 0;
}

static int pl_bound_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    size_t target;
    if (!pl_bound_target(c, walk->unit, slot, family, name, &target, walk->error, walk->capacity)) return -1;
    c->pending_unit = walk->unit; c->pending_target = target;
    int result = walk->reference(walk->context, slot, family, c->inputs.units[target].name, replacement);
    c->pending_unit = c->pending_target = SIZE_MAX;
    return result;
}
static int pl_bound_resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{
    pl_contributions *c = context;
    if (c->pending_unit == unit && c->pending_target < c->inputs.count) {
        const sh_package_link_unit *value = c->inputs.units + c->pending_target;
        if (!strcmp(value->family, family) && !strcmp(value->name, name)) { *target = c->pending_target; return 1; }
    }
    snprintf(error, capacity, "editor binding replay has no captured target: %s/%s", family, name); return 0;
}
static unsigned char *pl_bound_bind(void *context, size_t unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference, void *reference_context,
    size_t *output_length, char *error, size_t capacity)
{
    pl_contributions *c = context;
    pl_contribution_walk walk = {c, unit, reference, reference_context, NULL, error, capacity};
    if (!c->units[unit].owned)
        return sh_package_source_link_bind_file(c->source, unit, file, body, length,
            pl_bound_reference, &walk, output_length, error, capacity);
    return sh_package_source_link_bind_derived(c->source, unit, body, length,
        pl_bound_reference, &walk, pl_derived_exists, &walk, output_length, error, capacity);
}
static int pl_bound_assign(void *context, size_t unit, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    pl_contributions *c = context;
    return pl_assign(c->assignment, unit, identity, original_binding, files, count, name, paths, error, capacity);
}
static int pl_bound_assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    pl_contributions *c = context;
    return sh_package_native_names_assign_members((void *)c->assignment->inputs, unit, name,
        files, paths, count, names, error, capacity);
}

static int pl_tokens(pl_contributions *c, char *error, size_t capacity)
{
    if (!c->dirty) return 1;
    pl_tokens_free(c);
    sh_package_link_adapter adapter = {c, pl_bound_resolve, pl_bound_bind};
    if (!sh_package_link_identify_units(c->inputs.units, c->inputs.count, &adapter, &c->classes, error, capacity)) return 0;
    c->tokens = calloc(c->classes.count ? c->classes.count : 1, sizeof(*c->tokens));
    if (!c->tokens) return 0;
    c->token_count = c->classes.count;
    for (size_t i = 0; i < c->token_count; i++) {
        char token[96] = "smp-contribution/";
        size_t prefix = strlen(token);
        for (size_t k = 0; k < 32; k++) snprintf(token + prefix + k * 2, 3, "%02x", c->classes.identities[i][k]);
        /* These tokens exist only while composing. Verify their disjointness
         * from captured native names and from every unequal exact class. */
        for (size_t j = 0; j < c->inputs.count; j++) if (!strcmp(token, c->inputs.units[j].name)) {
            snprintf(error, capacity, "editor binding token collides with a captured resource"); return 0;
        }
        for (size_t j = 0; j < i; j++) if (!strcmp(token, c->tokens[j])) {
            snprintf(error, capacity, "unequal editor bindings have colliding identities"); return 0;
        }
        if (!(c->tokens[i] = _strdup(token))) return 0;
    }
    c->dirty = 0;
    return 1;
}

typedef struct pl_original_walk {
    pl_contributions *contributions;
    sh_package_link_edge *edges;
    size_t count, capacity;
} pl_original_walk;

static int pl_original_edge(pl_original_walk *walk, size_t target)
{
    if (target >= walk->contributions->inputs.count) return 0;
    if (walk->count == walk->capacity) {
        size_t next = walk->capacity ? walk->capacity * 2 : 16;
        sh_package_link_edge *grown = next > walk->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(walk->edges, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        walk->edges = grown; walk->capacity = next;
    }
    /* Only dependency reachability is used by the originals walk. */
    walk->edges[walk->count++] = (sh_package_link_edge){NULL, target};
    return 1;
}
static int pl_original_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_original_walk *walk = context;
    (void)slot; (void)family; (void)name; (void)replacement;
    return pl_original_edge(walk, walk->contributions->pending_target) ? 0 : -1;
}

/* An imported document may reuse an unchanged native editor slot, but must not
 * replace an existing shared selector/inspector with its authored extensions.
 * This is same-name, exact-source reuse, not a name-independent reader claim.
 * A changed dependency also makes its unchanged parent private. */
static int pl_document_shared(pl_build *b, sh_package_baseline_reader original,
    void *original_context, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    size_t count = c->inputs.count;
    sh_package_link_node *nodes = calloc(count ? count : 1, sizeof(*nodes));
    unsigned char *local = calloc(count ? count : 1, 1), *retained = calloc(count ? count : 1, 1);
    int ok = 0;
    if (!nodes || !local || !retained) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        int shared = c->shared[i] && pl_shared_declaration(unit->family);
        local[i] = unit->original_binding || shared;
        if (!local[i]) continue;
        pl_original_walk walk = {c};
        for (size_t f = 0; f < unit->file_count; f++) {
            const sh_compiled_resource *file = unit->files[f];
            size_t length = 0, bound_length = 0;
            unsigned char *body = sh_package_compilation_read(unit->compilation, file,
                SIZE_MAX, &length, error, capacity);
            if (!body) { free(walk.edges); goto done; }
            if (shared) {
                unsigned char *base = NULL; size_t base_length = 0;
                int status = original ? original(original_context, file->engine_path, &base, &base_length) : 0;
                if (status < 0) { free(base); free(body); free(walk.edges); goto done; }
                if ((status != 1 && status != 2) || !base || length != base_length || memcmp(body, base, length))
                    local[i] = 0;
                free(base);
                if (!local[i]) { free(body); break; }
            }
            unsigned char *bound = pl_bound_bind(c, i, file, body, length,
                pl_original_reference, &walk, &bound_length, error, capacity);
            free(body);
            if (!bound) { free(walk.edges); goto done; }
            free(bound);
        }
        for (size_t j = 0; j < unit->implicit_count; j++)
            if (!pl_original_edge(&walk, unit->implicit[j].target)) { free(walk.edges); goto done; }
        nodes[i].edges = walk.edges; nodes[i].edge_count = walk.count;
    }
    if (!sh_package_link_originals(nodes, count, local, retained, error, capacity)) goto done;
    for (size_t i = 0; i < count; i++) c->shared[i] &= retained[i];
    ok = 1;
done:
    if (nodes) for (size_t i = 0; i < count; i++) free((void *)nodes[i].edges);
    free(nodes); free(local); free(retained); return ok;
}
static int pl_token_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    size_t target;
    if (!pl_bound_target(walk->graph, walk->unit, slot, family, name, &target, walk->error, walk->capacity)) return -1;
    /* The entity reader resolves its inherited native class before walking
     * fields. Leave this header lexical; its captured scope is unchanged. */
    if (!strcmp(slot, "inherit") && !strcmp(walk->graph->inputs.units[walk->unit].family, "entitydef")) return 0;
    *replacement = walk->graph->tokens[walk->graph->classes.nodes[target]]; return 1;
}
static int pl_capture_edge(pl_contribution_unit *capture, const char *slot, size_t target)
{
    for (size_t i = 0; i < capture->count; i++) if (!strcmp(capture->edges[i].slot, slot)) return 0;
    if (capture->count == capture->capacity) {
        size_t next = capture->capacity ? capture->capacity * 2 : 16;
        pl_contribution_edge *grown = next > capture->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(capture->edges, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        capture->edges = grown; capture->capacity = next;
    }
    char *key = _strdup(slot); if (!key) return 0;
    capture->edges[capture->count++] = (pl_contribution_edge){key, target}; return 1;
}
static int pl_restore_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    pl_contribution_unit *capture = walk->capture;
    size_t target = SIZE_MAX;
    for (size_t i = 0; i < c->token_count; i++) if (!strcmp(c->tokens[i], name)) {
        target = c->classes.representatives[i]; break;
    }
    if (target == SIZE_MAX) {
        /* Only a deliberately un-tokenized inherited class remains lexical. */
        if (strcmp(slot, "inherit") || strcmp(c->inputs.units[walk->unit].family, "entitydef") ||
            !pl_bound_target(c, walk->unit, slot, family, name, &target, walk->error, walk->capacity)) {
            snprintf(walk->error, walk->capacity, "editor contribution contains an uncaptured reference: %s", slot); return -1;
        }
    }
    const sh_package_link_unit *value = c->inputs.units + target;
    if (strcmp(value->family, family)) return -1;
    if (!pl_capture_edge(capture, slot, target)) return -1;
    if (!strcmp(name, value->name)) return 0;
    *replacement = value->name; return 1;
}

static int pl_modern(const pl_build *b, size_t scope)
{
    return scope >= b->authored_count || !b->contexts ||
        b->contexts->contexts[scope].mode != SH_PACKAGE_CONTEXT_COMPOSED;
}

typedef struct pl_string_site {
    char *slot;
    sh_package_string_request request;
    size_t unit;
} pl_string_site;
typedef struct pl_localization {
    pl_build *build;
    pl_string_site *sites;
    size_t count, capacity, unit, component;
    sh_package_strings *strings;
    char *error;
    size_t error_capacity;
    int globals_reserved;
} pl_localization;

static int pl_string_bind(void *context, const char *slot, const char *key,
    size_t length, const char **replacement)
{
    pl_localization *p = context;
    if (memchr(key, 0, length)) return 0; /* No authored JSON key can match it. */
    if (p->strings) {
        for (size_t i = 0; i < p->count; i++) if (p->sites[i].unit == p->unit &&
            !strcmp(p->sites[i].slot, slot) && strlen(p->sites[i].request.key) == length &&
            !memcmp(p->sites[i].request.key, key, length)) {
            const char *bound = sh_package_strings_key(p->strings, i);
            if (!bound) return -1;
            if (strlen(bound) == length && !memcmp(bound, key, length)) return 0;
            *replacement = bound; return 1;
        }
        return 0; /* An explicit product-only field has no authored source site. */
    }
    if (length == SIZE_MAX) return -1;
    for (size_t i = 0; i < p->count; i++)
        if (p->sites[i].unit == p->unit && !strcmp(p->sites[i].slot, slot)) return -1;
    if (p->count == p->capacity) {
        size_t next = p->capacity ? p->capacity * 2 : 32;
        pl_string_site *grown = next > p->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(p->sites, next * sizeof(*grown)) : NULL;
        if (!grown) return -1;
        p->sites = grown; p->capacity = next;
    }
    char *saved = malloc(length + 1), *path = _strdup(slot);
    if (!saved || !path) { free(saved); free(path); return -1; }
    memcpy(saved, key, length); saved[length] = 0;
    p->sites[p->count++] = (pl_string_site){path, {p->component, saved}, p->unit}; return 0;
}

static int pl_string_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{
    pl_localization *p = context;
    const sh_package_string_reservations *native = &p->build->native->strings;
    if (!p->globals_reserved) {
        const sh_json_object *strings = &p->build->shared->policy.strings;
        if (strings->count && !native->reserve_global) return -1;
        for (size_t i = 0; i < strings->count; i++) {
            const char *raw = strings->members[i].value_json; sh_json_object pairs = {0};
            if (!sh_json_parse_object(raw, strlen(raw), 2, &pairs)) return -1;
            int ok = 1;
            for (size_t j = 0; ok && j < pairs.count; j++) {
                const char *id = pairs.members[j].key; size_t length = strlen(id);
                char *global = length <= SIZE_MAX - 6 ? malloc(length + 6) : NULL;
                if (global) {
                    memcpy(global, "#str_", 5); memcpy(global + 5, id, length + 1);
                    ok = native->reserve_global(native->context, global) == 1;
                } else ok = 0;
                free(global);
            }
            sh_json_object_free(&pairs); if (!ok) return -1;
        }
        p->globals_reserved = 1;
    }
    int result = native->reserve(native->context, key, identity, mapping);
    if (result == 1 && !sh_package_policy_localization(&p->build->shared->policy, key, mapping,
        NULL, p->error, p->error_capacity)) return -1;
    return result;
}

static int pl_string_capture_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context; size_t target;
    (void)replacement;
    return pl_bound_target(walk->graph, walk->unit, slot, family, name, &target,
        walk->error, walk->capacity) && pl_capture_edge(walk->capture, slot, target) ? 0 : -1;
}

static int pl_component_strings(const sh_package_sources *sources, size_t component)
{
    const sh_package_component *child = sources->components + component;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *parent = sources->components + i;
        size_t length = strlen(parent->relative);
        if (parent->owner == child->owner && (!length ||
            (!strncmp(parent->relative, child->relative, length) &&
             (!child->relative[length] || child->relative[length] == '/'))) &&
            sh_package_descriptor_section(&parent->descriptor, "strings")) return 1;
    }
    return 0;
}

/* Bind explicit authored typed sites before field union. Walk the authored
 * source for attribution, then replay only its exact slots/values in the
 * compiled body; inherited and product-only fields keep their own contexts. */
static int pl_localize(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_localization p = {0}; sh_package_string_request *requests = NULL;
    pl_contributions *c = b->contributions;
    p.build = b; p.error = error; p.error_capacity = capacity;
    int ok = 0;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (!pl_modern(b, c->inputs.scopes[i]) || unit->file_count != 1) continue;
        const sh_compiled_resource *file = unit->files[0];
        if (!file->type || !file->source_count) continue;
        /* These registered custom grammars contain resource/path operands,
         * not reflected idStrId leaves. Binary/catalog leaves likewise have
         * no declaration type. Other reflected families use the typed pass. */
        if (!strcmp(unit->family, "renderprog") || sh_decl_native_binding_family(unit->family, NULL)) continue;
        if (file->sources[0] >= sources->file_count) goto done;
        const sh_package_source_file *authored = sources->files + file->sources[0];
        if (authored->component >= sources->component_count) goto done;
        if (!pl_component_strings(sources, authored->component)) continue;
        /* Isolated scopes select exactly one nearest authored physical file. */
        if (file->source_count != 1) {
            snprintf(error, capacity, "localized declaration has multiple uncaptured source contributions: %s", file->engine_path);
            goto done;
        }
        size_t length = 0, bound_length = 0;
        unsigned char *raw = sh_package_source_read(authored, SIZE_MAX, &length, error, capacity);
        if (!raw) goto done;
        p.unit = i; p.component = authored->component;
        unsigned char *observed = sh_package_source_link_bind_strings(b->reader, i, raw, length,
            pl_string_bind, &p, &bound_length, error, capacity);
        free(raw); if (!observed) goto done; free(observed);
    }
    if (!p.count) { ok = 1; goto done; }
    requests = calloc(p.count, sizeof(*requests)); if (!requests) goto done;
    for (size_t i = 0; i < p.count; i++) requests[i] = p.sites[i].request;
    sh_package_string_reservations reservations = {0};
    if (b->native && b->native->strings.reserve) {
        reservations = b->native->strings; reservations.reserve = pl_string_reserve; reservations.context = &p;
    }
    p.strings = sh_package_strings_compile(sources, requests, p.count, &reservations, error, capacity);
    if (!p.strings) goto done;
    for (size_t i = 0; i < p.count; i++) if (sh_package_strings_identity(p.strings, i)) {
        const char *key = sh_package_strings_key(p.strings, i); size_t j;
        for (j = 0; j < b->shared->policy.localized_count; j++) {
            sh_package_localized_string *value = b->shared->policy.localized + j;
            if (!strcmp(value->key, key)) {
                if (!sh_package_owners_add(&value->owners, sources->components[p.sites[i].request.component].owner)) goto done;
                break;
            }
        }
        if (j == b->shared->policy.localized_count) goto done;
    }
    for (size_t i = 0; i < c->inputs.count; i++) {
        size_t site;
        for (site = 0; site < p.count; site++) if (p.sites[site].unit == i) break;
        if (site == p.count) continue;
        sh_package_link_unit *unit = c->inputs.units + i;
        const sh_compiled_resource *file = unit->files[0];
        size_t length = 0, bound_length = 0, inspected = 0;
        unsigned char *raw = sh_package_compilation_read(unit->compilation, file, SIZE_MAX, &length, error, capacity);
        if (!raw) goto done;
        p.unit = i;
        unsigned char *bound = sh_package_source_link_bind_strings(b->reader, i, raw, length,
            pl_string_bind, &p, &bound_length, error, capacity);
        if (!bound) { free(raw); goto done; }
        if (length == bound_length && !memcmp(raw, bound, length)) { free(raw); free(bound); continue; }
        free(raw);
        pl_contribution_unit pending = {0};
        pl_contribution_walk walk = {c, i, NULL, NULL, &pending, error, capacity};
        unsigned char *checked = sh_package_source_link_bind_unit(b->reader, i, bound, bound_length,
            pl_string_capture_reference, &walk, &inspected, error, capacity);
        if (!checked) { free(bound); pl_contribution_unit_free(&pending); goto done; }
        free(checked);
        sh_package_resource_projection projection = {file->engine_path, &file, 1, bound, bound_length};
        pending.owned = sh_package_compilation_project(sources, &projection, 1, NULL,
            b->shared->canonical_path, error, capacity);
        free(bound);
        if (!pending.owned) { pl_contribution_unit_free(&pending); goto done; }
        pending.file = pending.owned->resources; c->units[i] = pending;
        unit->compilation = pending.owned; unit->files = &c->units[i].file; unit->original_binding = 0;
    }
    ok = 1;
done:
    for (size_t i = 0; i < p.count; i++) { free(p.sites[i].slot); free((char *)p.sites[i].request.key); }
    free(p.sites); free(requests); sh_package_strings_free(p.strings);
    if (!ok && !*error) snprintf(error, capacity, "cannot bind source-scoped localization");
    return ok;
}

static int pl_edges_same(pl_contributions *c, size_t unit, const pl_contribution_unit *capture)
{
    if (c->units[unit].owned && c->units[unit].count != capture->count) return 0;
    for (size_t i = 0; i < capture->count; i++) {
        const pl_contribution_edge *edge = capture->edges + i;
        const sh_package_link_unit *value = c->inputs.units + edge->target;
        size_t prior; char ignored[256];
        if (!pl_bound_target(c, unit, edge->slot, value->family, value->name, &prior, ignored, sizeof(ignored)) ||
            c->classes.nodes[prior] != c->classes.nodes[edge->target]) return 0;
    }
    return 1;
}

static int pl_contribution_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count, sizeof(*parts));
    const sh_compiled_resource **owners = calloc(count + 1, sizeof(*owners));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    sh_decl_source base = {"{}", 2};
    size_t source_count = 0;
    int ok = 0;
    if (!texts || !parts || !owners || !pending) goto done;
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        const sh_compiled_resource *file = c->inputs.units[unit].files[0];
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(c->inputs.units[unit].compilation, file,
            SIZE_MAX, &length, error, capacity);
        if (!body) goto done;
        pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
        texts[i].text = (char *)sh_package_source_link_bind_derived(c->source, unit, body, length,
            pl_token_reference, &walk, pl_derived_exists, &walk, &texts[i].length, error, capacity);
        free(body); if (!texts[i].text) goto done;
        if (c->inputs.scopes[unit] == b->count - 1) base = texts[i];
        else parts[source_count++] = texts[i];
    }
    if (!source_count) { ok = 1; goto done; }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i], length = 0, native_length = 0, owner_count = 1;
        const sh_package_link_unit *input = c->inputs.units + unit;
        char *body = sh_package_editor_contributions(input->family, base, parts, source_count,
            texts[i], !sh_package_source_link_shared(c->source)[unit], &length, error, capacity);
        if (!body) goto done;
        pl_contribution_walk walk = {c, unit, NULL, NULL, pending + i, error, capacity};
        unsigned char *native = sh_package_source_link_bind_derived(c->source, unit, (unsigned char *)body,
            length, pl_restore_reference, &walk, pl_derived_exists, &walk, &native_length, error, capacity);
        free(body); if (!native) goto done;
        if (input->files[0]->body && native_length == input->files[0]->body_length &&
            !memcmp(native, input->files[0]->body, native_length) && pl_edges_same(c, unit, pending + i)) {
            free(native); pl_contribution_unit_free(pending + i); continue;
        }
        owners[0] = input->files[0];
        for (size_t j = 0; j < count; j++) if (j != i && c->inputs.scopes[members[j]] != b->count - 1)
            owners[owner_count++] = c->inputs.units[members[j]].files[0];
        sh_package_resource_projection projection = {input->files[0]->engine_path, owners, owner_count,
            native, native_length, NULL, NULL, 1};
        pending[i].owned = sh_package_compilation_project(sources, &projection, 1, NULL,
            b->shared->canonical_path, error, capacity);
        free(native); if (!pending[i].owned) goto done;
        pending[i].file = pending[i].owned->resources;
    }
    /* Commit the phase together: no later supplier observes a partly composed
     * peer, and failure leaves the previous captured graph intact. */
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        if (!pending[i].owned) continue;
        c->dirty = 1;
        pl_contribution_unit_free(c->units + unit); c->units[unit] = pending[i]; memset(pending + i, 0, sizeof(*pending));
        c->inputs.units[unit].compilation = c->units[unit].owned;
        c->inputs.units[unit].files = &c->units[unit].file;
        const sh_package_link_unit *original = sh_package_source_link_inputs(c->source)->units + unit;
        c->inputs.units[unit].original_binding = original->original_binding &&
            original->files[0]->body && original->files[0]->body_length == c->units[unit].file->body_length &&
            !memcmp(original->files[0]->body, c->units[unit].file->body, c->units[unit].file->body_length);
    }
    ok = 1;
done:
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    free(texts); free(parts); free(owners); free(pending); return ok;
}

static int pl_contributions_open(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = calloc(1, sizeof(*c));
    const sh_package_link_inputs *original = sh_package_source_link_inputs(b->reader);
    size_t *members = NULL;
    unsigned char *done = NULL;
    if (!c) return 0;
    b->contributions = c; c->source = b->reader; c->inputs.count = original->count; c->inputs.scopes = original->scopes;
    c->dirty = 1;
    c->pending_unit = c->pending_target = SIZE_MAX;
    c->inputs.units = calloc(original->count ? original->count : 1, sizeof(*c->inputs.units));
    c->units = calloc(original->count ? original->count : 1, sizeof(*c->units));
    c->shared = calloc(original->count ? original->count : 1, 1);
    members = calloc(original->count ? original->count : 1, sizeof(*members));
    done = calloc(original->count ? original->count : 1, 1);
    if (!c->inputs.units || !c->units || !c->shared || !members || !done) goto bad;
    memcpy(c->inputs.units, original->units, original->count * sizeof(*c->inputs.units));
    if (!pl_localize(b, sources, error, capacity)) goto bad;
    for (int phase = 1; phase <= 3; phase++) {
        if (!pl_tokens(c, error, capacity)) goto bad;
        memset(done, 0, original->count);
        for (size_t i = 0; i < original->count; i++) {
            const sh_package_link_unit *unit = c->inputs.units + i;
            size_t count = 0;
            if (done[i] || unit->file_count != 1 || !pl_modern(b, original->scopes[i]) ||
                sh_package_editor_contribution_phase(unit->family) != phase) continue;
            for (size_t j = i; j < original->count; j++) {
                const sh_package_link_unit *peer = c->inputs.units + j;
                if (peer->file_count == 1 && pl_modern(b, original->scopes[j]) &&
                    !strcmp(unit->family, peer->family) && !strcmp(unit->name, peer->name)) {
                    done[j] = 1; members[count++] = j;
                }
            }
            if (!pl_contribution_group(b, sources, members, count, error, capacity)) goto bad;
        }
    }
    if (!pl_tokens(c, error, capacity)) goto bad;
    /* Keep a verified baseline selector/bank slot. Other bound variants get
     * private native identities; historical contexts are never overwritten by
     * the modern field union. With no baseline, only one equal shared class can
     * occupy the authored name. Registry allocation still verifies native IDs. */
    for (size_t i = 0; i < original->count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (!pl_shared_declaration(unit->family)) continue;
        size_t selected = SIZE_MAX, first = SIZE_MAX; int equal = 1, eligible = 0;
        for (size_t j = 0; j < original->count; j++) {
            const sh_package_link_unit *peer = c->inputs.units + j;
            if (strcmp(unit->family, peer->family) || strcmp(unit->name, peer->name)) continue;
            size_t binding = c->classes.nodes[j];
            if (first == SIZE_MAX) first = binding; else if (first != binding) equal = 0;
            if (original->scopes[j] == b->count - 1) selected = binding;
            if (sh_package_source_link_shared(b->reader)[j]) eligible = 1;
        }
        if (selected == SIZE_MAX && equal && eligible) selected = first;
        c->shared[i] = c->classes.nodes[i] == selected;
    }
    pl_tokens_free(c); free(members); free(done); return 1;
bad:
    free(members); free(done); return 0;
}

static int pl_provenance(const pl_build *b, const sh_package_sources *sources,
    sh_package_provenance *out)
{
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(b->reader);
    for (size_t i = 0; i < inputs->count; i++) {
        const sh_package_link_unit *unit = inputs->units + i;
        size_t scope = inputs->scopes[i], selected;
        int authored = 0;
        if (!b->contexts && sh_package_source_link_shared(b->reader)[i]) continue;
        for (size_t f = 0; f < unit->file_count; f++) if (unit->files[f]->source_count) authored = 1;
        /* A reverse-discovered selector/entity can keep original source text
         * while reaching a changed scoped dependency. Its recipe belongs to
         * that authored context even though its own file has no source owner. */
        if (!authored && (scope >= b->authored_count || sh_package_source_link_shared(b->reader)[i])) continue;
        if (scope >= b->count) return 0;
        if (b->contexts) {
            const sh_package_provenance_context *recorded = b->contexts->contexts + scope;
            selected = sh_package_provenance_context_add(out, recorded->mode,
                recorded->trees, recorded->tree_count, recorded->component);
        } else {
            const sh_package_component *component = sources->components + b->views[scope].component;
            if (!sources->fingerprints || component->owner >= sources->package_count) return 0;
            selected = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])sh_package_source_tree(sources, component->owner),
                1, component->relative);
        }
        if (selected == SIZE_MAX || !sh_package_provenance_bind(out, unit->family, unit->name,
            b->linked.names[b->linked.classes.nodes[i]], &selected, 1)) return 0;
    }
    return 1;
}

/* Descriptor weapon names resolve in the same lexical component as assets.
 * An absent file keeps its typed original weapon name; policy alone never
 * manufactures a resource, dependency or gameplay owner. */
static int pl_hud(pl_build *b, char *error, size_t capacity)
{
    sh_package_policy *policy = &b->linked.compiled->policy;
    const sh_package_link_inputs *inputs = &b->contributions->inputs;
    policy->hud_bound = 1;
    for (size_t scope = 0; scope < b->authored_count; scope++) {
        const sh_package_sources *sources = &b->views[scope].sources;
        for (size_t component = 0; component < sources->component_count; component++) {
            const sh_package_component *owner = sources->components + component;
            const char *raw = sh_package_descriptor_section(&owner->descriptor, "hud");
            sh_json_object hud = {0}, rows = {0}; int ok = 0;
            if (!raw) continue;
            if (!sh_json_parse_object(raw, strlen(raw), 5, &hud)) goto row_done;
            if (!hud.count) { ok = 1; goto row_done; }
            raw = sh_json_object_get(&hud, "weapons");
            if (!raw || !sh_json_parse_object(raw, strlen(raw), 3, &rows)) goto row_done;
            for (size_t r = 0; r < rows.count; r++) {
                const sh_json_member *row = rows.members + r;
                int mode = 0, shadowed = 0;
                if (!sh_package_hud_mode(row->value_json, &mode)) goto row_done;
                /* A nested component's explicit row overrides its inherited
                 * parent policy in that scope, not its sibling's weapon. */
                if (b->views[scope].component != SIZE_MAX)
                    for (size_t j = 0; !shadowed && j < sources->component_count; j++) {
                        const sh_package_component *nearer = sources->components + j;
                        if (nearer->owner != owner->owner || strlen(nearer->relative) <= strlen(owner->relative)) continue;
                        const char *next_raw = sh_package_descriptor_section(&nearer->descriptor, "hud");
                        sh_json_object next = {0}, weapons = {0};
                        if (next_raw && sh_json_parse_object(next_raw, strlen(next_raw), 5, &next)) {
                            next_raw = sh_json_object_get(&next, "weapons");
                            if (next_raw && sh_json_parse_object(next_raw, strlen(next_raw), 3, &weapons))
                                shadowed = sh_json_object_get(&weapons, row->key) != NULL;
                        }
                        sh_json_object_free(&next); sh_json_object_free(&weapons);
                    }
                if (shadowed) continue;
                const char *name = row->key;
                size_t unit = sh_package_link_inputs_find(inputs, scope, "weapon", name);
                if (unit != SIZE_MAX) name = b->linked.names[b->linked.classes.nodes[unit]];
                sh_package_owners supplier = {0};
                int bound = sh_package_owners_add(&supplier, owner->owner) && sh_package_hud_bind(policy, name, mode, &supplier);
                sh_package_owners_free(&supplier); if (!bound) goto row_done;
            }
            ok = 1;
row_done:
            sh_json_object_free(&hud); sh_json_object_free(&rows);
            if (!ok) { if (!*error) snprintf(error, capacity, "cannot bind scoped weapon HUD policy"); return 0; }
        }
    }
    return sh_package_hud_publish(policy, 0, 0);
}

static int pl_consumers(const sh_package_link_inputs *inputs, const sh_package_link_output *linked,
    sh_package_consumers **out, char *error, size_t capacity)
{
    if (!linked->implicit_count) return 1;
    sh_package_consumer *owners = NULL;
    sh_package_consumer_target *targets = NULL;
    size_t count = 0;
    if (linked->implicit_count > SIZE_MAX / sizeof(*owners) || linked->implicit_count > SIZE_MAX / sizeof(*targets)) goto done;
    owners = calloc(linked->implicit_count, sizeof(*owners)); targets = calloc(linked->implicit_count, sizeof(*targets));
    if (!owners || !targets) goto done;
    for (size_t i = 0; i < linked->implicit_count; i++) {
        const sh_package_link_implicit_binding *binding = linked->implicit + i;
        const sh_package_link_unit *owner = inputs->units + linked->classes.representatives[binding->owner];
        const sh_package_link_unit *target = inputs->units + linked->classes.representatives[binding->target];
        if (!i || linked->implicit[i - 1].owner != binding->owner)
            owners[count++] = (sh_package_consumer){owner->family, linked->names[binding->owner], targets + i, 0};
        targets[i] = (sh_package_consumer_target){binding->slot, target->family, linked->names[binding->target]};
        owners[count - 1].count++;
    }
    *out = sh_package_consumers_create(owners, count, error, capacity);
done:
    free(owners); free(targets);
    if (!*out && error && capacity && !*error) snprintf(error, capacity, "cannot retain compiled native consumer bindings");
    return *out != NULL;
}

static int pl_compile(const sh_package_sources *sources, const sh_package_provenance *contexts,
    const sh_package_compile_environment *environment, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_library_native *native, sh_package_library *out,
    char *error, size_t capacity)
{
    pl_build b = {0};
    sh_package_source_link_native source_native = {&b, pl_admit, pl_semantics,
        native ? native->source.shader : NULL, pl_material_companions, pl_material_seeds, pl_implicit, pl_source_owners};
    sh_package_compile_environment env;
    char fallback[1024]; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!out) return 0;
    sh_package_library_free(out);
    if (environment && environment->invalid_package) *environment->invalid_package = SIZE_MAX;
    if (environment && environment->unserved_package) *environment->unserved_package = SIZE_MAX;
    if (environment && environment->conflicted_packages) sh_package_owners_free(environment->conflicted_packages);
    if (!sources || !environment || (contexts && !contexts->context_count)) goto done;
    if (!contexts && !sources->component_count) {
        out->compiled = sh_package_compile_with(sources, environment, error, capacity);
        return out->compiled != NULL;
    }
    b.count = contexts ? contexts->context_count : sources->component_count; b.authored_count = b.count; b.native = native;
    b.contexts = contexts;
    env = *environment; env.previous = NULL; env.independent_sources = 1;
    b.originals[0].read = environment->baseline; b.originals[0].context = environment->baseline_context;
    b.originals[1].read = original; b.originals[1].context = original_context;
    if (environment->baseline) { env.baseline = pl_read_original; env.baseline_context = b.originals; }
    size_t original_slot = original == environment->baseline && original_context == environment->baseline_context ? 0 : 1;
    original = pl_read_original; original_context = b.originals + original_slot;
    if (b.count > SIZE_MAX / sizeof(*b.views) - 2 || b.count > SIZE_MAX / sizeof(*b.scopes) - 2) goto done;
    b.views = calloc(b.count + 2, sizeof(*b.views)); b.scopes = calloc(b.count + 2, sizeof(*b.scopes));
    if (!b.views || !b.scopes) goto done;
    for (size_t i = 0; i < b.count; i++) {
        if (!(contexts ? sh_package_provenance_scope(contexts, i, sources, b.views + i, error, capacity) :
            sh_package_scope_open(sources, i, b.views + i, error, capacity))) goto done;
        sh_package_compile_environment scoped = env;
        if (contexts && contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_COMPOSED) scoped.independent_sources = 0;
        sh_package_builtin *builtins = NULL;
        if (scoped.independent_sources && scoped.builtin_count) {
            builtins = calloc(scoped.builtin_count, sizeof(*builtins));
            if (!builtins) goto done;
            scoped.builtin_count = 0;
            for (size_t j = 0; j < env.builtin_count; j++)
                if (!pl_builtin_has_authored_editor(&b.views[i].sources, env.builtins[j].engine_path))
                    builtins[scoped.builtin_count++] = env.builtins[j];
            scoped.builtins = builtins;
        }
        b.scopes[i] = sh_package_compile_with(&b.views[i].sources, &scoped, error, capacity);
        free(builtins);
        if (!b.scopes[i]) goto done;
    }
    if (!pl_shared_sources(&b, sources, error, capacity)) goto done;
    b.shared = sh_package_compile_with(&b.shared_view.sources, &env, error, capacity);
    if (!b.shared) goto done;
    if (contexts) {
        sh_package_owners all = {0}; sh_package_policy policy = {0};
        for (size_t i = 0; i < sources->package_count; i++)
            if (!sh_package_owners_add(&all, i)) { sh_package_owners_free(&all); goto done; }
        for (size_t i = 0; i < b.authored_count; i++) if (!pl_modern(&b, i))
            for (size_t j = 0; j < b.views[i].sources.component_count; j++) {
                const sh_package_component *component = b.views[i].sources.components + j;
                if (component->descriptor.id[0] &&
                    !sh_package_owners_add(&b.shared->policy.legacy_string_owners, component->owner)) {
                    sh_package_owners_free(&all); goto done;
                }
            }
        int prepared = sh_package_compilation_policy(b.shared, &all, &policy, error, capacity);
        sh_package_owners_free(&all);
        if (!prepared) goto done;
        sh_package_policy_free(&b.shared->policy); b.shared->policy = policy;
    }
    if (!pl_product_scope(&b, sources, error, capacity)) goto done;
    if (!pl_baseline_scope(&b, sources, env.baseline ? env.baseline : original,
        env.baseline ? env.baseline_context : original_context, error, capacity)) goto done;
    if (!pl_native_open(&b, catalog, original, original_context, registry, reflection, error, capacity)) goto done;
    source_native.audio = b.audio;
    unsigned char *discover = calloc(b.count ? b.count : 1, 1);
    if (!discover) goto done;
    if (!contexts || !contexts->binding_count) for (size_t i = 0; i < b.authored_count; i++)
        discover[i] = !contexts || contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_ISOLATED;
    sh_package_source_root_plan root_plan = {discover, b.count - 1, contexts};
    sh_package_source_failure failure;
    b.reader = sh_package_source_link_open_attributed((const sh_package_compilation *const *)b.scopes,
        b.count, catalog, original, original_context, registry, reflection, &source_native,
        &root_plan, &failure, error, capacity);
    free(discover);
    if (!b.reader) {
        /* Only content a local component supplied makes it responsible: its own
         * unit refused by a reader is malformed source; content reaching a unit
         * the product cannot link leaves its complete delivery unserved for this
         * candidate. Healthy peers then compile without it. Product/stock-only
         * chains, environment, map contexts and product/baseline scopes keep
         * refusing the whole candidate and blame no package. */
        size_t used = strlen(error);
        if (failure.chain[0] && used < capacity)
            snprintf(error + used, capacity - used, " (via %s%s)", failure.chain,
                failure.kind == SH_PACKAGE_SOURCE_FAILURE_PRODUCT ? "; reached only from product and stock content" : "");
        if (!contexts && failure.scope < b.authored_count && failure.scope < sources->component_count) {
            size_t owner = sources->components[failure.scope].owner;
            if (failure.kind == SH_PACKAGE_SOURCE_FAILURE_SOURCE && environment->invalid_package)
                *environment->invalid_package = owner;
            else if (failure.kind == SH_PACKAGE_SOURCE_FAILURE_COVERAGE && environment->unserved_package)
                *environment->unserved_package = owner;
        }
        goto done;
    }
    if (!pl_coverage(&b, sources, &out->coverage)) {
        snprintf(error, capacity, "cannot account package coverage"); goto done;
    }
    if (!pl_contributions_open(&b, sources, error, capacity)) goto done;
    if (contexts && !pl_document_shared(&b, original, original_context, error, capacity)) goto done;
    /* Authored documents carry their own selector/inspector behavior. Changed
     * editor units receive ordinary bound aliases just like their gameplay
     * dependencies, so importing them never rewrites the open editor's shared
     * objects. Certified vanilla bindings still retain native names. Local
     * library compilation continues composing its one shared editor surface. */
    pl_assignment assignment = {&b.contributions->inputs, b.contributions->shared,
        b.materials != NULL};
    b.contributions->assignment = &assignment;
    sh_package_link_registry identity_adapter = {0};
    if (native && native->identities.head_slot) {
        b.identities = sh_package_native_registry_open(native->identities, catalog, error, capacity);
        if (!b.identities) goto done;
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (assignment.shared[i] && pl_shared_declaration(unit->family) &&
                !sh_package_native_registry_share(b.identities, unit->family, unit->name)) goto done;
        }
        identity_adapter = sh_package_native_registry_adapter(b.identities);
    }
    sh_package_link_adapter adapter = {b.contributions, pl_bound_resolve, pl_bound_bind, pl_bound_assign,
        b.identities ? &identity_adapter : NULL, pl_bound_assign_members};
    if (!sh_package_link(sources, assignment.inputs->units, assignment.inputs->count, &adapter, &b.shared->policy,
        environment->canonical_path, &b.linked, error, capacity) ||
        (b.identities && !sh_package_native_registry_validate(b.identities, error, capacity)) ||
        !pl_provenance(&b, sources, &out->bindings) || !pl_hud(&b, error, capacity) ||
        !sh_package_sound_prepare(b.linked.compiled, error, capacity) ||
        !pl_consumers(assignment.inputs, &b.linked, &out->consumers, error, capacity)) goto done;
    if (b.materials) {
        out->virtual_materials = sh_package_virtual_material_prepare(b.materials, assignment.inputs,
            &b.linked, error, capacity);
        if (!out->virtual_materials) goto done;
    }
    if (b.audio) {
        if (!sh_package_audio_program_prepare(b.audio, error, capacity)) goto done;
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (unit->file_count && !strcmp(unit->family, "sound") &&
                !sh_package_audio_program_output(b.audio, assignment.inputs->scopes[i], unit->name,
                    b.linked.names[b.linked.classes.nodes[i]], error, capacity)) goto done;
        }
        out->audio = b.audio; b.audio = NULL;
    }
    out->compiled = b.linked.compiled; b.linked.compiled = NULL;
    out->compiled->independent_sources = 1; ok = 1;
done:
    pl_close(&b);
    if (!ok) {
        /* Accounted coverage survives a later failure as a diagnostic only. */
        char *coverage = out->coverage; out->coverage = NULL;
        sh_package_library_free(out); out->coverage = coverage;
        if (!*error) snprintf(error, capacity, "scoped package library could not be prepared");
    }
    return ok;
}

int sh_package_library_compile(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native,
    sh_package_library *out, char *error, size_t capacity)
{
    return pl_compile(sources, NULL, environment, catalog, original, original_context,
        registry, reflection, native, out, error, capacity);
}

int sh_package_library_compile_contexts(const sh_package_sources *sources,
    const sh_package_provenance *contexts, const sh_package_compile_environment *environment,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native,
    sh_package_library *out, char *error, size_t capacity)
{
    if (!contexts) {
        if (out) sh_package_library_free(out);
        if (error && capacity) snprintf(error, capacity, "map package contexts are unavailable");
        return 0;
    }
    return pl_compile(sources, contexts, environment, catalog, original, original_context,
        registry, reflection, native, out, error, capacity);
}
