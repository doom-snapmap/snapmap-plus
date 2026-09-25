#include "package_library.h"
#include "package_keys.h"
#include "package_native_names.h"
#include "package_native_registry.h"
#include "package_hud.h"
#include "package_sound.h"
#include "decl_native_rebind.h"
#include "decl_block_compose.h"
#include "package_movie.h"
#include "swf_binary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    size_t contribution_owner;   /* package left unserved by a composition gap */
    size_t contribution_invalid; /* package whose own editor source is invalid */
    /* Each package's registration keys: the bit it authored and the bit it
     * holds in this library (see pl_key_phase). */
    struct pl_key_binding { size_t scope; unsigned original, current; } *keys;
    size_t key_count;
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
        if (original >= 0 && original <= 1 && !sh_package_audio_program_contains(b->audio, scope, name)) {
            memcpy(semantics, combined, 32); return original; /* Native event binding. */
        }
        if (original < 0 || original > 1 || !sh_package_audio_program_prepare(b->audio, error, capacity) ||
            !sh_package_audio_program_root(b->audio, scope, name, &root, error, capacity)) return -1;
        /* An exact native audio graph keeps the plain native certificate, the
         * same one an uncollected SnapMap sound carries in another scope. */
        if (original && root.original) { memcpy(semantics, combined, 32); return 1; }
        memcpy(combined + 32, root.identity, 32);
        if (!sh_package_bytes_identity(combined, sizeof(combined), &identity)) return -1;
        memcpy(semantics, identity.digest, 32); return 0;
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
    if (b->audio && !observing && unit->file_count && !strcmp(unit->family, "sound")) {
        /* Only a declaration a scope changed, or a SnapMap sound its banks
         * affect, needs an audio program root. Other SnapMap sounds keep their
         * native event binding, including events no bank implements (the
         * stock "default" sound posts play_default, which nothing plays). */
        int member = sh_package_audio_program_affected(b->audio, scope, unit->name);
        for (size_t f = 0; !member && f < unit->file_count; f++)
            member = unit->files[f]->source_count && !unit->files[f]->native_original;
        if (member && !sh_package_audio_program_collect(b->audio, scope, unit->name, error, capacity)) return 0;
    }
    return !b->native || !b->native->source.implicit ||
        b->native->source.implicit(b->native->source.context, scope, observing, unit,
            visit, visit_context, error, capacity) == 1;
}

static void pl_close(pl_build *b)
{
    free(b->keys); b->keys = NULL; b->key_count = 0;
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
    if (!sh_package_audio_program_contains(b->audio, scope, name)) return 1; /* Native binding. */
    if (sh_package_audio_program_owners(b->audio, scope, name, owners, gameplay)) return 1;
    snprintf(error, capacity, "sound has no captured dependency ownership: %s", name); return 0;
}

static unsigned long long pl_now_ms(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000u + (unsigned long long)now.tv_nsec / 1000000u : 0;
}
/* Charge the time since *mark to phase and restart the mark. */
static void pl_phase(sh_package_library *out, int phase, unsigned long long *mark)
{
    unsigned long long now = pl_now_ms();
    out->phase_ms[phase] += now >= *mark ? now - *mark : 0; *mark = now;
}
const char *sh_package_library_phases(const sh_package_library *library, char *out, size_t capacity)
{
    static const char *const names[SH_PACKAGE_LIBRARY_PHASES] = {
        "scopes", "native", "source link", "contributions", "identities", "link", "outputs", "audio"
    };
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (int i = 0; library && i < SH_PACKAGE_LIBRARY_PHASES && used < capacity; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s %llu ms", i ? ", " : "", names[i], library->phase_ms[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return out;
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
    size_t atlas = 0;
    char *text = NULL;
    *out = NULL;
    for (size_t i = 0; i < count; i++) if (gaps[i].scope == SIZE_MAX) { stock++; if (!first) first = gaps + i; }
    for (size_t scope = 0; scope < b->authored_count; scope++)
        for (size_t f = 0; b->scopes[scope] && f < b->scopes[scope]->resource_count; f++)
            atlas += sh_package_source_link_ancillary(b->scopes[scope]->resources[f].engine_path);
    /* Measure, then write the identical text. */
    for (size_t pass = 0; (count || atlas) && pass < 2; pass++) {
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
        /* The renderer packs transparency atlas entries once at startup and
         * materials carry precomputed atlas coordinates; a package entry is
         * delivered intact but does not reach that atlas yet. */
        for (size_t scope = 0; scope < b->authored_count; scope++) {
            const sh_package_compilation *compiled = b->scopes[scope];
            for (size_t f = 0; compiled && f < compiled->resource_count; f++) {
                const sh_compiled_resource *file = compiled->resources + f;
                if (!file->source_count || !sh_package_source_link_ancillary(file->engine_path)) continue;
                const char *label = "map source", *id = "";
                if (!b->contexts && scope < sources->component_count) {
                    const sh_package_component *component = sources->components + scope;
                    label = component->descriptor.name[0] ? component->descriptor.name :
                        sources->packages[component->owner].name;
                    id = component->descriptor.id;
                }
                int n = snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                    "%s%s%s%s: transparency atlas entry %s is kept in the package but the renderer's atlas "
                    "does not use package entries yet.\n", label, *id ? " [" : "", id, *id ? "]" : "",
                    file->engine_path + 25);
                if (n < 0) return 0;
                used += (size_t)n;
            }
        }
        if (!pass) {
            if (!used) break;
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
            int several = 0;
            if (file->type && !strcmp(file->type, "animweb"))
                for (size_t other = 0; other < b->count && !several; other++)
                    several = other != scope && sh_package_compilation_find(b->scopes[other], file->engine_path) != NULL;
            if (!pl_shared_declaration(file->type) && !several) continue;
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
    const unsigned char *global; /* player hands webs (pl_hands_phase) */
} pl_assignment;

static int pl_assign(void *context, size_t index, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    pl_assignment *a = context;
    const sh_package_link_unit *unit = a->inputs->units + index;
    if ((a->shared[index] && pl_shared_declaration(unit->family)) || (a->global && a->global[index])) {
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
    unsigned char *global; /* members of a composed player hands web */
    pl_assignment *assignment;
    int dirty;
    /* Identity captures of untouched units, shared by every pass of this
     * build: the contribution phases and the final link bind them alike. */
    sh_package_link_reuse *reuse;
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
    sh_package_link_reuse_close(c->reuse);
    free(c->units); free(c->inputs.units); free(c->shared); free(c->global); free(c);
}
/* A composed or localized unit binds through the current tokens, so only
 * untouched units may reuse an earlier identity capture. */
static void pl_reuse_refresh(pl_contributions *c)
{
    for (size_t i = 0; c->reuse && i < c->inputs.count; i++)
        if (c->units[i].owned) sh_package_link_reuse_invalidate(c->reuse, i);
}

/* Borrowed-string set for exact collision checks over many names. */
typedef struct pl_names { const char **slots; size_t capacity; } pl_names;
static uint64_t pl_names_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return hash;
}
static int pl_names_open(pl_names *set, size_t count)
{
    size_t capacity = 64;
    while (capacity < count * 2 + 1) { if (capacity > SIZE_MAX / 4) return 0; capacity *= 2; }
    set->slots = calloc(capacity, sizeof(*set->slots)); set->capacity = capacity;
    return set->slots != NULL;
}
/* Return 1 when text is present; otherwise add it when add is set. */
static int pl_names_seen(pl_names *set, const char *text, int add)
{
    size_t at = (size_t)pl_names_hash(text) & (set->capacity - 1);
    while (set->slots[at]) {
        if (!strcmp(set->slots[at], text)) return 1;
        at = (at + 1) & (set->capacity - 1);
    }
    if (add) set->slots[at] = text;
    return 0;
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
    pl_names captured = {0}, issued = {0};
    int ok = 0;
    if (!c->dirty) return 1;
    pl_tokens_free(c);
    pl_reuse_refresh(c);
    sh_package_link_adapter adapter = {c, pl_bound_resolve, pl_bound_bind};
    adapter.reuse = c->reuse;
    if (!sh_package_link_identify_units(c->inputs.units, c->inputs.count, &adapter, &c->classes, error, capacity)) return 0;
    c->tokens = calloc(c->classes.count ? c->classes.count : 1, sizeof(*c->tokens));
    if (!c->tokens || !pl_names_open(&captured, c->inputs.count) || !pl_names_open(&issued, c->classes.count)) goto done;
    for (size_t j = 0; j < c->inputs.count; j++) (void)pl_names_seen(&captured, c->inputs.units[j].name, 1);
    c->token_count = c->classes.count;
    for (size_t i = 0; i < c->token_count; i++) {
        /* A token must read back through its field's reader: a movie field
         * names a cooked pair by its generated/swf path, and model fields
         * dispatch on the extension (an md6 definition by .md6 or .decl, a
         * breakable declaration by .decl). Keep the class's own extension. */
        const sh_package_link_unit *represented = c->inputs.units + c->classes.representatives[i];
        char token[256] = "smp-contribution/", alias[128];
        size_t prefix = strlen(token);
        for (size_t k = 0; k < 32; k++) snprintf(token + prefix + k * 2, 3, "%02x", c->classes.identities[i][k]);
        const char *slash = strrchr(represented->name, '/'), *dot = strrchr(represented->name, '.');
        if (sh_package_movie_owner(represented->family, represented->name)) {
            /* The generated movie path partitions a long stem like the native
             * constructor does; derive it rather than spell it. */
            sh_swf_binary_paths linked;
            snprintf(alias, sizeof(alias), "swf/%s.swf", token);
            if (!sh_swf_binary_paths_for(alias, &linked) || strlen(linked.movie) >= sizeof(token)) {
                snprintf(error, capacity, "editor binding token has no movie spelling"); goto done;
            }
            strcpy_s(token, sizeof(token), linked.movie);
        } else if (dot && dot > (slash ? slash : represented->name) && dot[1] && strlen(dot) < 16)
            strcat_s(token, sizeof(token), dot);
        /* These tokens exist only while composing. Verify their disjointness
         * from captured native names and from every unequal exact class. */
        if (pl_names_seen(&captured, token, 0)) {
            snprintf(error, capacity, "editor binding token collides with a captured resource"); goto done;
        }
        if (!(c->tokens[i] = _strdup(token))) goto done;
        if (pl_names_seen(&issued, c->tokens[i], 1)) {
            snprintf(error, capacity, "unequal editor bindings have colliding identities"); goto done;
        }
    }
    c->dirty = 0; ok = 1;
done:
    free((void *)captured.slots); free((void *)issued.slots);
    return ok;
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
/* Contribution headers stay lexical; their captured scope is unchanged. The
 * entity reader resolves its inherited native class before walking fields.
 * An inspector or editor definition's sparse inherited list indexes its parent
 * declaration's list in whichever bound variant a scope holds, so composition
 * compares parents by name; a parent differing only in other fields (a
 * placement flag) is not a different layout. Restore re-resolves each scope. */
static int pl_lexical_inherit(const pl_contributions *c, size_t unit, const char *slot)
{ return !strcmp(slot, "inherit") && sh_package_editor_contribution_phase(c->inputs.units[unit].family); }
static int pl_token_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    size_t target;
    if (!pl_bound_target(walk->graph, walk->unit, slot, family, name, &target, walk->error, walk->capacity)) return -1;
    if (pl_lexical_inherit(walk->graph, walk->unit, slot)) return 0;
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
        /* Only a deliberately un-tokenized inherited header remains lexical. */
        if (!pl_lexical_inherit(c, walk->unit, slot) ||
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

/* The package supplying a member's own authored bytes; SIZE_MAX for stock,
 * product content and byte-identical copies of an installed original. */
static size_t pl_member_owner(const pl_build *b, const sh_package_sources *sources, size_t unit)
{
    const sh_package_link_unit *value = sh_package_source_link_inputs(b->contributions->source)->units + unit;
    size_t scope = b->contributions->inputs.scopes[unit];
    int authored = 0;
    for (size_t f = 0; f < value->file_count; f++)
        if (value->files[f]->source_count && !value->files[f]->native_original) authored = 1;
    if (!authored || scope >= b->authored_count || scope >= sources->component_count) return SIZE_MAX;
    return sources->components[scope].owner;
}

/* A group the product cannot compose names its declaration. A package's own
 * invalid source rejects that package. A gap in one member's own content
 * leaves its package unserved for this candidate; a gap only in combining
 * them leaves one contributing package unserved, the last in package order,
 * so healthy peers still compile. Stock or product content alone blames
 * nobody, and environment failures never judge content. */
static void pl_contribution_failure(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, const sh_package_link_unit *input,
    int kind, size_t culprit, char *error, size_t capacity)
{
    char reason[768];
    size_t owner = SIZE_MAX;
    snprintf(reason, sizeof(reason), "%s", error);
    snprintf(error, capacity, "%s %s: %s", input->family, input->name, reason);
    if (kind == SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT) return;
    if (culprit != SIZE_MAX && (owner = pl_member_owner(b, sources, culprit)) != SIZE_MAX) {
        if (kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE) b->contribution_invalid = owner;
        else b->contribution_owner = owner;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        size_t candidate = pl_member_owner(b, sources, members[i]);
        if (candidate != SIZE_MAX && (owner == SIZE_MAX || candidate > owner)) owner = candidate;
    }
    b->contribution_owner = owner;
}

/* 1 with the inherit literal of a contribution text (NULL when absent). */
static int pl_inherit_name(sh_decl_source text, char **out)
{
    sh_decl_node *tree = sh_decl_tree_parse(text, NULL, 0);
    const char *value; size_t length;
    *out = NULL;
    if (!tree) return 0;
    if (sh_decl_tree_literal(sh_decl_tree_member(tree, "inherit"), &value, &length)) {
        if (!(*out = malloc(length + 1))) { sh_decl_tree_free(tree); return 0; }
        memcpy(*out, value, length); (*out)[length] = 0;
    }
    sh_decl_tree_free(tree); return 1;
}
static char *pl_token_text(pl_contributions *c, size_t unit, size_t *length, char *error, size_t capacity)
{
    const sh_package_link_unit *input = c->inputs.units + unit;
    size_t size = 0;
    unsigned char *body = sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX, &size, error, capacity);
    if (!body) return NULL;
    pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
    unsigned char *text = sh_package_source_link_bind_derived(c->source, unit, body, size,
        pl_token_reference, &walk, pl_derived_exists, &walk, length, error, capacity);
    free(body); return (char *)text;
}
/* The unit's original text as its author wrote it, bound in its own scope. */
static int pl_original_token_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    size_t target;
    if (!sh_package_source_link_resolve_unit(c->source, walk->unit, family, name, &target, walk->error, walk->capacity)) return -1;
    if (pl_lexical_inherit(c, walk->unit, slot)) return 0;
    *replacement = c->tokens[c->classes.nodes[target]]; return 1;
}
static char *pl_original_text(pl_contributions *c, size_t unit, size_t *length, char *error, size_t capacity)
{
    const sh_package_link_unit *input = sh_package_source_link_inputs(c->source)->units + unit;
    size_t size = 0;
    unsigned char *body = sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX, &size, error, capacity);
    if (!body) return NULL;
    pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
    unsigned char *text = sh_package_source_link_bind_file(c->source, unit, input->files[0], body, size,
        pl_original_token_reference, &walk, length, error, capacity);
    free(body); return (char *)text;
}
typedef struct pl_chain { sh_decl_source *texts; size_t count; } pl_chain;
static void pl_chain_free(pl_chain *chain)
{
    for (size_t i = 0; chain->texts && i < chain->count; i++) free((char *)chain->texts[i].text);
    free(chain->texts); memset(chain, 0, sizeof(*chain));
}
/* Parent texts, nearest first: published follows the bound (possibly composed)
 * parents; otherwise the original parents the unit's author saw. */
static int pl_parent_chain(pl_contributions *c, size_t unit, sh_decl_source own, int published,
    pl_chain *out, int *kind, char *error, size_t capacity)
{
    const char *family = c->inputs.units[unit].family;
    sh_decl_source text = own;
    size_t current = unit;
    memset(out, 0, sizeof(*out));
    *kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT;
    for (size_t depth = 0;; depth++) {
        char *parent = NULL; size_t target, length = 0;
        if (!pl_inherit_name(text, &parent)) { snprintf(error, capacity, "editor definition text cannot be read"); goto failed; }
        if (!parent) return 1;
        if (depth >= 32) {
            *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            snprintf(error, capacity, "inherit '%s': editor inheritance is circular or deeper than the native limit", parent);
            free(parent); goto failed;
        }
        int resolved = published ? pl_bound_target(c, current, "inherit", family, parent, &target, error, capacity) :
            sh_package_source_link_resolve_unit(c->source, current, family, parent, &target, error, capacity);
        free(parent);
        if (!resolved) { *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE; goto failed; }
        char *next = published ? pl_token_text(c, target, &length, error, capacity) :
            pl_original_text(c, target, &length, error, capacity);
        if (!next) goto failed;
        sh_decl_source *grown = out->count < SIZE_MAX / sizeof(*grown) - 1 ?
            realloc(out->texts, (out->count + 1) * sizeof(*grown)) : NULL;
        if (!grown) { free(next); goto failed; }
        out->texts = grown; out->texts[out->count++] = (sh_decl_source){next, length};
        text = out->texts[out->count - 1]; current = target;
    }
failed:
    pl_chain_free(out); return 0;
}
static int pl_stock_authored(const pl_contributions *c, size_t unit)
{
    const sh_package_link_unit *value = sh_package_source_link_inputs(c->source)->units + unit;
    for (size_t f = 0; f < value->file_count; f++)
        if (value->files[f]->source_count && !value->files[f]->native_original) return 0;
    return 1;
}
static int pl_same_parent(const char *a, const char *b)
{ return (!a && !b) || (a && b && !strcmp(a, b)); }

/* Restore captured references in a composed or rebased text and stage it as
 * this unit's owned file, unless it equals the current one. */
static int pl_contribution_stage(pl_build *b, const sh_package_sources *sources, size_t unit,
    const char *body, size_t length, const sh_compiled_resource *const *peers, size_t peer_count,
    pl_contribution_unit *pending, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + unit;
    const sh_compiled_resource **owners = calloc(peer_count + 1, sizeof(*owners));
    size_t native_length = 0;
    pl_contribution_walk walk = {c, unit, NULL, NULL, pending, error, capacity};
    unsigned char *native;
    if (!owners) { snprintf(error, capacity, "editor contribution allocation failed"); return 0; }
    native = sh_package_source_link_bind_derived(c->source, unit, (const unsigned char *)body,
        length, pl_restore_reference, &walk, pl_derived_exists, &walk, &native_length, error, capacity);
    if (!native) { free((void *)owners); return 0; }
    if (input->files[0]->body && native_length == input->files[0]->body_length &&
        !memcmp(native, input->files[0]->body, native_length) && pl_edges_same(c, unit, pending)) {
        free(native); free((void *)owners); pl_contribution_unit_free(pending); return 1;
    }
    owners[0] = input->files[0];
    for (size_t j = 0; j < peer_count; j++) owners[j + 1] = peers[j];
    sh_package_resource_projection projection = {input->files[0]->engine_path, owners, peer_count + 1,
        native, native_length, NULL, NULL, 1};
    pending->owned = sh_package_compilation_project(sources, &projection, 1, NULL,
        b->shared->canonical_path, error, capacity);
    free(native); free((void *)owners);
    if (!pending->owned) return 0;
    pending->file = pending->owned->resources; return 1;
}

#ifdef SH_PACKAGE_LIBRARY_TESTING
static long g_contribution_read_failures;
static const char *g_contribution_read_name;
/* Make the next editor contribution reads fail like an unreadable source. */
void sh_package_library_test_contribution_read_failures(const char *name, long count)
{ g_contribution_read_name = name; g_contribution_read_failures = count; }
#endif

/* One declaration's editor contributions. Members naming the reference
 * member's parent compose into one shared surface; a member whose author gave
 * it another parent is that author's own variant and is only rebased. Every
 * member's inherited sparse lists are first rebased from the parent layout
 * its author saw onto the published parent's, parents composing first. */
static int pl_contribution_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    int phase = sh_package_editor_contribution_phase(input->family);
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    char **parents = calloc(count, sizeof(*parents));
    unsigned char *included = calloc(count, 1), *rebased = calloc(count, 1);
    size_t *part_member = calloc(count + 1, sizeof(*part_member));
    sh_decl_source base = {"{}", 2};
    size_t source_count = 0, base_member = SIZE_MAX, reference, culprit = SIZE_MAX;
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT, any_rebased = 0;
    pl_chain published = {0}, authored_base = {0};
    if (!texts || !parts || !peers || !pending || !parents || !included || !rebased || !part_member) {
        snprintf(error, capacity, "editor contribution allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        const sh_compiled_resource *file = c->inputs.units[unit].files[0];
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(c->inputs.units[unit].compilation, file,
            SIZE_MAX, &length, error, capacity);
#ifdef SH_PACKAGE_LIBRARY_TESTING
        if (body && g_contribution_read_failures > 0 && (!g_contribution_read_name ||
                !strcmp(c->inputs.units[unit].name, g_contribution_read_name)) && g_contribution_read_failures--) {
            free(body); body = NULL; snprintf(error, capacity, "editor contribution source is unreadable");
        }
#endif
        if (!body) goto failed;
        pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
        texts[i].text = (char *)sh_package_source_link_bind_derived(c->source, unit, body, length,
            pl_token_reference, &walk, pl_derived_exists, &walk, &texts[i].length, error, capacity);
        free(body); if (!texts[i].text) goto failed;
        if (c->inputs.scopes[unit] == b->count - 1) base_member = i;
        if (phase != 2 && !pl_inherit_name(texts[i], parents + i)) {
            snprintf(error, capacity, "editor definition text cannot be read"); goto failed;
        }
    }
    reference = base_member != SIZE_MAX ? base_member : 0;
    for (size_t i = 0; i < count; i++) included[i] = phase == 2 || pl_same_parent(parents[i], parents[reference]);
    /* Rebase inherited collections: first only where the published parent's
     * layout differs, then, if any member changed, every member of the shared
     * surface, so all of them state complete lists. */
    for (int pass = 0; phase != 2 && pass < 2 && (pass == 0 || any_rebased); pass++)
        for (size_t i = 0; i < count; i++) {
            pl_chain own_published = {0}, own_authored = {0};
            const pl_chain *authored = &own_authored, *target = included[i] ? &published : &own_published;
            char *out = NULL; size_t out_length = 0;
            sh_package_editor_failure failure;
            int result;
            if (!parents[i] || (pass && (!included[i] || rebased[i]))) continue;
            if (included[i] && !published.texts && !published.count &&
                !pl_parent_chain(c, members[reference], texts[reference], 1, &published, &kind, error, capacity)) {
                culprit = members[reference]; goto failed;
            }
            if (!included[i] && !pl_parent_chain(c, members[i], texts[i], 1, &own_published, &kind, error, capacity)) {
                culprit = members[i]; goto failed;
            }
            if (pl_stock_authored(c, members[i]) && base_member != SIZE_MAX && pl_same_parent(parents[i], parents[base_member])) {
                if (!authored_base.count &&
                    !pl_parent_chain(c, members[base_member], texts[base_member], 0, &authored_base, &kind, error, capacity)) {
                    pl_chain_free(&own_published); culprit = members[base_member]; goto failed;
                }
                authored = &authored_base;
            } else if (!pl_parent_chain(c, members[i], texts[i], 0, &own_authored, &kind, error, capacity)) {
                pl_chain_free(&own_published); culprit = members[i]; goto failed;
            }
            result = sh_package_editor_rebase(input->family, texts[i], authored->texts, authored->count,
                target->texts, target->count, pass, &out, &out_length, &failure, error, capacity);
            pl_chain_free(&own_published); pl_chain_free(&own_authored);
            if (result < 0) { kind = failure.kind; culprit = members[i]; goto failed; }
            if (result) {
                free((char *)texts[i].text); texts[i] = (sh_decl_source){out, out_length};
                rebased[i] = 1; if (included[i]) any_rebased = 1;
            }
        }
    for (size_t i = 0; i < count; i++) if (included[i]) {
        if (i == base_member) base = texts[i];
        else { part_member[source_count] = i; parts[source_count++] = texts[i]; }
    }
    /* A stock child of a relaid parent republishes its rebased lists. */
    if (!source_count && base_member != SIZE_MAX && rebased[base_member]) {
        part_member[source_count] = base_member; parts[source_count++] = base;
    }
    for (size_t i = 0; i < count && source_count; i++) {
        size_t unit = members[i], length = 0, peer_count = 0;
        sh_package_editor_failure failure;
        if (!included[i]) continue;
        char *body = sh_package_editor_contributions_report(input->family, base, parts, source_count,
            texts[i], !sh_package_source_link_shared(c->source)[unit], &length, &failure, error, capacity);
        if (!body) {
            kind = failure.kind;
            if (kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && failure.source < source_count)
                culprit = members[part_member[failure.source]];
            goto failed;
        }
        for (size_t j = 0; j < count; j++) if (j != i && included[j] && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        int staged = pl_contribution_stage(b, sources, unit, body, length, peers, peer_count, pending + i, error, capacity);
        free(body); if (!staged) goto failed;
    }
    /* A member with another parent is its author's own variant: only rebased. */
    for (size_t i = 0; i < count; i++) if (!included[i] && rebased[i] &&
        !pl_contribution_stage(b, sources, members[i], texts[i].text, texts[i].length, NULL, 0, pending + i, error, capacity))
        goto failed;
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
    goto done;
failed:
    pl_contribution_failure(b, sources, members, count, input, kind, culprit, error, capacity);
done:
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    if (parents) for (size_t i = 0; i < count; i++) free(parents[i]);
    pl_chain_free(&published); pl_chain_free(&authored_base);
    free(texts); free(parts); free((void *)peers); free(pending); free(parents); free(included); free(rebased); free(part_member);
    return ok;
}

/* ---- Registration keys ------------------------------------------------------
 * A conductor row registers its AI under the spawn-type bits of its aiType
 * (package_keys.h). A bit no constant names is a key a package chose for its
 * own AI, and independent packages can choose the same one. In stable source
 * order the first keeps it; each later distinct registration moves to the
 * lowest unnamed bit no package registers, and every spawn-type site of that
 * package's declarations follows in derived output (authored files are never
 * rewritten). Equal registrations share their key. */
typedef struct pl_key_row { size_t scope; unsigned bit; char *identity; } pl_key_row;
typedef struct pl_keys { pl_key_row *rows; size_t count, capacity; } pl_keys;
static void pl_keys_free(pl_keys *keys)
{
    for (size_t i = 0; i < keys->count; i++) free(keys->rows[i].identity);
    free(keys->rows); memset(keys, 0, sizeof(*keys));
}
static int pl_keys_add(pl_keys *keys, size_t scope, unsigned bit, const char *identity, size_t length)
{
    char *copy;
    if (keys->count == keys->capacity) {
        size_t next = keys->capacity ? keys->capacity * 2 : 16;
        pl_key_row *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(keys->rows, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        keys->rows = grown; keys->capacity = next;
    }
    if (length == SIZE_MAX || !(copy = malloc(length + 1))) return 0;
    memcpy(copy, identity, length); copy[length] = 0;
    keys->rows[keys->count++] = (pl_key_row){scope, bit, copy}; return 1;
}
typedef struct pl_key_order { const pl_build *build; const sh_package_sources *sources; } pl_key_order;
/* Stable source order: delivery tree bytes, then component path. */
static int pl_scope_order(void *context, const void *left, const void *right)
{
    const pl_key_order *order = context;
    const pl_build *b = order->build;
    size_t x = *(const size_t *)left, y = *(const size_t *)right;
    const unsigned char *a = NULL, *z = NULL;
    const char *ra = "", *rz = "";
    int result;
    if (b->contexts) {
        const sh_package_provenance_context *p = b->contexts->contexts + x, *q = b->contexts->contexts + y;
        if (p->tree_count) a = p->trees[0];
        if (q->tree_count) z = q->trees[0];
        if (p->component) ra = p->component;
        if (q->component) rz = q->component;
    } else {
        const sh_package_component *p = order->sources->components + b->views[x].component;
        const sh_package_component *q = order->sources->components + b->views[y].component;
        a = sh_package_source_tree(order->sources, p->owner); z = sh_package_source_tree(order->sources, q->owner);
        if (p->relative) ra = p->relative;
        if (q->relative) rz = q->relative;
    }
    result = a && z ? memcmp(a, z, 32) : a ? -1 : z ? 1 : 0;
    if (!result) result = strcmp(ra, rz);
    return result ? result : (x < y ? -1 : x > y);
}
/* Custom-bit rows of every package conductor, each with the bound AI token. */
static int pl_key_rows(pl_build *b, const sh_package_key_space *space, pl_keys *keys, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        size_t scope = c->inputs.scopes[i], length = 0;
        int stated = 0;
        if (scope >= b->authored_count || !pl_modern(b, scope) || unit->file_count != 1 ||
            strcmp(unit->family, "entitydef") || !unit->files[0]->source_count) continue;
        unsigned char *body = sh_package_compilation_read(unit->compilation, unit->files[0], SIZE_MAX, &length, error, capacity);
        if (!body) return 0;
        for (size_t at = 0; !stated && at + 10 <= length; at++) stated = !memcmp(body + at, "aiTypeList", 10);
        free(body);
        if (!stated) continue;
        char *text = pl_token_text(c, i, &length, error, capacity);
        if (!text) return 0;
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){text, length}, NULL, 0);
        const sh_decl_node *edit = tree ? sh_decl_tree_member(tree, "edit") : NULL;
        const sh_decl_node *list = edit ? sh_decl_tree_member(edit, "aiTypeList") : NULL;
        for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
            const char *value = NULL, *entity = "";
            size_t value_length = 0, entity_length = 0;
            uint32_t bits = 0;
            if (!item->compound || !sh_decl_tree_literal(sh_decl_tree_member(item, "aiType"), &value, &value_length) ||
                !sh_package_key_custom_bits(space, value, value_length, &bits) || !bits) continue;
            if (!sh_decl_tree_literal(sh_decl_tree_member(item, "entityDef"), &entity, &entity_length)) entity_length = 0;
            for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
                if ((bits >> bit & 1u) && !pl_keys_add(keys, scope, bit, entity, entity_length)) {
                    sh_decl_tree_free(tree); free(text);
                    snprintf(error, capacity, "registration key allocation failed"); return 0;
                }
        }
        sh_decl_tree_free(tree); free(text);
    }
    return 1;
}
static int pl_key_row_order(const void *left, const void *right)
{
    const pl_key_row *x = left, *y = right;
    if (x->bit != y->bit) return x->bit < y->bit ? -1 : 1;
    if (x->scope != y->scope) return x->scope < y->scope ? -1 : 1;
    return strcmp(x->identity, y->identity);
}
/* One scope's registration of one key: its sorted distinct AI tokens. */
static int pl_key_same(const pl_keys *keys, size_t first, size_t first_end, size_t second, size_t second_end)
{
    size_t i = first, j = second;
    while (i < first_end && j < second_end) {
        if (strcmp(keys->rows[i].identity, keys->rows[j].identity)) return 0;
        const char *a = keys->rows[i].identity, *z = keys->rows[j].identity;
        while (i < first_end && !strcmp(keys->rows[i].identity, a)) i++;
        while (j < second_end && !strcmp(keys->rows[j].identity, z)) j++;
    }
    return i == first_end && j == second_end;
}
static size_t pl_scope_owner(const pl_build *b, const sh_package_sources *sources, size_t scope)
{
    if (b->contexts || scope >= b->authored_count || b->views[scope].component >= sources->component_count) return SIZE_MAX;
    return sources->components[b->views[scope].component].owner;
}
typedef struct pl_key_edit { const unsigned char *to; char *owned; } pl_key_edit;
static int pl_key_rewrite(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{
    pl_key_edit *edit = context;
    size_t out_length = 0; int failed = 0;
    char *moved = sh_package_key_relocate(value, length, edit->to, &out_length, &failed);
    (void)slot;
    if (failed) return -1;
    if (!moved) return 0;
    free(edit->owned); edit->owned = moved; *replacement = moved; return 1;
}
/* Rewrite every spawn-type site of one package scope through its moves. */
static int pl_key_relocate_scope(pl_build *b, const sh_package_sources *sources, size_t scope,
    const unsigned char to[SH_PACKAGE_KEY_BITS], char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        const sh_compiled_resource *file;
        pl_contribution_unit pending = {0};
        pl_key_edit edit = {to, NULL};
        unsigned char *out = NULL;
        size_t length = 0, out_length = 0;
        if (c->inputs.scopes[i] != scope || unit->file_count != 1) continue;
        file = unit->files[0];
        /* Registered custom grammars and binary leaves have no reflected
         * spawn-type field; only the package's own declarations can hold its key. */
        if (!file->type || !file->source_count || !strcmp(unit->family, "renderprog") ||
            sh_decl_native_binding_family(unit->family, NULL)) continue;
        char *text = pl_token_text(c, i, &length, error, capacity), *inspected = NULL;
        if (!text) return 0;
        int status = sh_package_source_link_bind_keys(b->reader, i, (const unsigned char *)text, length,
            pl_key_rewrite, &edit, &out, &out_length, error, capacity);
        free(edit.owned);
        if (status < 0) {
            /* A key site the readers cannot walk in this package's own
             * declaration leaves that package unserved; peers still compile. */
            if (!strstr(error, "allocation")) b->contribution_owner = pl_scope_owner(b, sources, scope);
            free(text); return 0;
        }
        const char *current = status == 1 ? (const char *)out : text;
        size_t current_length = status == 1 ? out_length : length;
        if (!strcmp(unit->family, "snappropertyinspector_bitflag")) {
            size_t inspected_length = 0; int failed = 0;
            inspected = sh_package_key_relocate_inspector(current, current_length, to, &inspected_length, &failed);
            if (failed) {
                snprintf(error, capacity, "registration key inspector allocation failed");
                free(text); free(out); return 0;
            }
            if (inspected) { current = inspected; current_length = inspected_length; }
        }
        int changed = current_length != length || memcmp(current, text, length);
        free(text);
        if (!changed) { free(out); free(inspected); continue; }
        int staged = pl_contribution_stage(b, sources, i, current, current_length, NULL, 0, &pending, error, capacity);
        free(out); free(inspected);
        if (!staged) { pl_contribution_unit_free(&pending); return 0; }
        if (!pending.owned) continue;
        c->dirty = 1;
        pl_contribution_unit_free(c->units + i); c->units[i] = pending;
        c->inputs.units[i].compilation = c->units[i].owned;
        c->inputs.units[i].files = &c->units[i].file;
        c->inputs.units[i].original_binding = 0;
    }
    return 1;
}
static int pl_key_phase(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    sh_package_key_space space;
    pl_keys keys = {0};
    unsigned char (*to)[SH_PACKAGE_KEY_BITS] = NULL;
    size_t *scopes = NULL, *starts = NULL;
    uint32_t used = 0, allocated = 0;
    int status = sh_package_source_link_key_space(b->reader, &space), ok = 0;
    if (status < 0) { snprintf(error, capacity, "spawn-type reflection is unreadable"); return 0; }
    if (!status || !b->authored_count) return 1;
    if (!pl_key_rows(b, &space, &keys, error, capacity)) goto done;
    if (!keys.count) { ok = 1; goto done; }
    qsort(keys.rows, keys.count, sizeof(*keys.rows), pl_key_row_order);
    for (size_t i = 0; i < keys.count; i++) used |= 1u << keys.rows[i].bit;
    to = malloc(b->authored_count * sizeof(*to));
    scopes = calloc(keys.count, sizeof(*scopes)); starts = calloc(keys.count + 1, sizeof(*starts));
    if (!to || !scopes || !starts) { snprintf(error, capacity, "registration key allocation failed"); goto done; }
    memset(to, 0xff, b->authored_count * sizeof(*to));
    for (size_t i = 0; i < keys.count;) {
        unsigned bit = keys.rows[i].bit;
        size_t end = i, count = 0;
        while (end < keys.count && keys.rows[end].bit == bit) end++;
        /* Scopes registering this key, each with its run of rows. */
        for (size_t k = i; k < end; k++)
            if (k == i || keys.rows[k].scope != keys.rows[k - 1].scope) { scopes[count] = keys.rows[k].scope; starts[count++] = k; }
        starts[count] = end;
        size_t *order = calloc(count, sizeof(*order));
        if (!order) { snprintf(error, capacity, "registration key allocation failed"); goto done; }
        for (size_t k = 0; k < count; k++) order[k] = k;
        pl_key_order context = {b, sources};
        /* Order runs by their scopes' stable source identity. */
        for (size_t k = 1; k < count; k++)
            for (size_t m = k; m > 0 && pl_scope_order(&context, scopes + order[m - 1], scopes + order[m]) > 0; m--) {
                size_t swap = order[m]; order[m] = order[m - 1]; order[m - 1] = swap;
            }
        for (size_t k = 0; k < count; k++) {
            size_t run = order[k], target = bit, prior;
            for (prior = 0; prior < k; prior++)
                if (pl_key_same(&keys, starts[order[prior]], starts[order[prior] + 1], starts[run], starts[run + 1])) break;
            if (prior < k) target = to[scopes[order[prior]]][bit] < SH_PACKAGE_KEY_BITS ? to[scopes[order[prior]]][bit] : bit;
            else if (k) {
                uint32_t free_bits = space.pool & ~used & ~allocated;
                if (!free_bits) {
                    snprintf(error, capacity, "registration key %u has no free spawn-type bit for another package's AI",
                        1u << bit);
                    b->contribution_owner = pl_scope_owner(b, sources, scopes[run]); free(order); goto done;
                }
                for (target = 0; !(free_bits >> target & 1u); target++) {}
                allocated |= 1u << target;
            }
            if (target != bit) to[scopes[run]][bit] = (unsigned char)target;
        }
        free(order);
        i = end;
    }
    /* Retain every package key, moved or kept, for map provenance. */
    free(b->keys); b->key_count = 0;
    if (!(b->keys = calloc(keys.count, sizeof(*b->keys)))) { snprintf(error, capacity, "registration key allocation failed"); goto done; }
    for (size_t i = 0; i < keys.count; i++) {
        const pl_key_row *row = keys.rows + i;
        if (i && row->scope == keys.rows[i - 1].scope && row->bit == keys.rows[i - 1].bit) continue;
        unsigned current = to[row->scope][row->bit] < SH_PACKAGE_KEY_BITS ? to[row->scope][row->bit] : row->bit;
        b->keys[b->key_count++] = (struct pl_key_binding){row->scope, row->bit, current};
    }
    for (size_t scope = 0; scope < b->authored_count; scope++) {
        int moves = 0;
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) if (to[scope][bit] < SH_PACKAGE_KEY_BITS) moves = 1;
        if (moves && !pl_key_relocate_scope(b, sources, scope, to[scope], error, capacity)) goto done;
    }
    ok = 1;
done:
    pl_keys_free(&keys); free(to); free(scopes); free(starts); return ok;
}

/* Shared gameplay registration lists (a conductor's aiTypeList): every source
 * of the declaration contributes its rows, each bound in its own scope, and
 * every member receives the union keyed by the native aiType. The stock-named
 * declaration therefore registers each package's AI variants for spawning;
 * colliding package keys were relocated first (pl_key_phase), so equal keys
 * that still bind differently are a composition gap reported like any other
 * contribution failure. */
static int pl_registration_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    size_t *part_member = calloc(count + 1, sizeof(*part_member));
    sh_decl_source base = {"{}", 2};
    size_t source_count = 0, culprit = SIZE_MAX;
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT, stated = 0;
    if (!texts || !parts || !peers || !pending || !part_member) {
        snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t length = 0;
        if (!(texts[i].text = pl_token_text(c, members[i], &texts[i].length, error, capacity))) goto failed;
        (void)length;
        if (sh_package_registration_lists(texts[i])) stated = 1;
        if (c->inputs.scopes[members[i]] == b->count - 1) base = texts[i];
        else { part_member[source_count] = i; parts[source_count++] = texts[i]; }
    }
    if (!stated || !source_count) { ok = 1; goto done; }
    for (size_t i = 0; i < count; i++) {
        size_t length = 0, peer_count = 0;
        sh_package_editor_failure failure;
        char *body = sh_package_registration_contributions_report(base, parts, source_count, texts[i],
            &length, &failure, error, capacity);
        if (!body) {
            kind = failure.kind;
            if (kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && failure.source < source_count)
                culprit = members[part_member[failure.source]];
            goto failed;
        }
        for (size_t j = 0; j < count; j++) if (j != i && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        int staged = pl_contribution_stage(b, sources, members[i], body, length, peers, peer_count, pending + i, error, capacity);
        free(body); if (!staged) goto failed;
    }
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
    goto done;
failed:
    pl_contribution_failure(b, sources, members, count, input, kind, culprit, error, capacity);
done:
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    free(texts); free(parts); free((void *)peers); free(pending); free(part_member);
    return ok;
}
/* Entity groups whose sources state registration lists; a cheap byte check
 * precedes parsing because most entity declarations have none. */
static int pl_registration_phase(pl_build *b, const sh_package_sources *sources, const size_t *order,
    size_t order_count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < order_count;) {
        const sh_package_link_unit *unit = c->inputs.units + order[i];
        size_t end = i + 1;
        int candidate = 0;
        while (end < order_count && !strcmp(c->inputs.units[order[end]].family, unit->family) &&
            !strcmp(c->inputs.units[order[end]].name, unit->name)) end++;
        if (!strcmp(unit->family, "entitydef") && end - i > 1)
            for (size_t k = i; k < end && !candidate; k++) {
                const sh_compiled_resource *file = c->inputs.units[order[k]].files[0];
                size_t length = 0;
                unsigned char *body = sh_package_compilation_read(c->inputs.units[order[k]].compilation, file,
                    SIZE_MAX, &length, error, capacity);
                if (!body) return 0;
                for (size_t at = 0; !candidate && at + 12 <= length; at++)
                    candidate = !memcmp(body + at, "aiTypeList", 10) || (at + 12 <= length && !memcmp(body + at, "globalAIMods", 12));
                free(body);
            }
        if (candidate && !pl_registration_group(b, sources, order + i, end - i, error, capacity)) return 0;
        i = end;
    }
    return 1;
}

/* ---- The player hands web -------------------------------------------------
 * The web idPlayer::handsAnimWeb names is one global registry: the hands
 * animator looks every weapon's subweb up in it by name. A package changing it
 * (adding its weapon's subweb and switch edges) changes that global web, so
 * package changes compose by native records (the animation web block grammar)
 * into the stock-named declaration every player uses; one changing package
 * supplies it whole. Records two packages change differently cannot compose,
 * which leaves the later package unserved as a coverage gap. */
static int pl_hands_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    size_t *part_member = calloc(count + 1, sizeof(*part_member));
    size_t base_member = SIZE_MAX, part_count = 0, culprit = SIZE_MAX, composed_length = 0;
    char *composed = NULL;
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT;
    if (!texts || !parts || !peers || !pending || !part_member) {
        snprintf(error, capacity, "player hands web allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        if (!(texts[i].text = pl_token_text(c, unit, &texts[i].length, error, capacity))) goto failed;
        if (c->inputs.scopes[unit] == b->count - 1) base_member = i;
        else if (!pl_stock_authored(c, unit)) { part_member[part_count] = i; parts[part_count++] = texts[i]; }
    }
    if (!part_count) { ok = 1; goto done; }
    if (part_count == 1) {
        if (!(composed = malloc(parts[0].length + 1))) { snprintf(error, capacity, "player hands web allocation failed"); goto failed; }
        memcpy(composed, parts[0].text, parts[0].length); composed[parts[0].length] = 0; composed_length = parts[0].length;
    } else {
        sh_decl_conflict conflict = {SIZE_MAX, SIZE_MAX, 0, 0};
        if (base_member == SIZE_MAX) {
            kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE; culprit = members[part_member[part_count - 1]];
            snprintf(error, capacity, "several packages change the player hands web without its captured stock original");
            goto failed;
        }
        composed = sh_decl_block_compose("animweb", texts[base_member], parts, part_count, &composed_length,
            error, capacity, &conflict);
        if (!composed) {
            if (!strstr(error, "allocation")) {
                kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
                size_t blamed = conflict.second < part_count ? conflict.second : conflict.first;
                culprit = members[part_member[blamed < part_count ? blamed : part_count - 1]];
            }
            goto failed;
        }
    }
    for (size_t i = 0; i < count; i++) {
        size_t peer_count = 0;
        for (size_t j = 0; j < count; j++) if (j != i && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        if (!pl_contribution_stage(b, sources, members[i], composed, composed_length, peers, peer_count,
            pending + i, error, capacity)) goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        c->global[unit] = 1;
        if (!pending[i].owned) continue;
        c->dirty = 1;
        pl_contribution_unit_free(c->units + unit); c->units[unit] = pending[i]; memset(pending + i, 0, sizeof(*pending));
        c->inputs.units[unit].compilation = c->units[unit].owned;
        c->inputs.units[unit].files = &c->units[unit].file;
        c->inputs.units[unit].original_binding = 0;
    }
    ok = 1;
    goto done;
failed:
    pl_contribution_failure(b, sources, members, count, input, kind, culprit, error, capacity);
done:
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    free(texts); free(parts); free((void *)peers); free(pending); free(part_member); free(composed);
    return ok;
}
static int pl_hands_phase(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    unsigned char *seen = calloc(c->inputs.count ? c->inputs.count : 1, 1);
    size_t *members = calloc(c->inputs.count ? c->inputs.count : 1, sizeof(*members));
    int ok = 0;
    if (!seen || !members) { snprintf(error, capacity, "player hands web allocation failed"); goto done; }
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        size_t count = 0;
        if (seen[i] || unit->file_count != 1 || strcmp(unit->family, "animweb") ||
            !sh_package_source_link_player_hands(b->reader, unit->name)) continue;
        for (size_t j = i; j < c->inputs.count; j++) {
            const sh_package_link_unit *peer = c->inputs.units + j;
            if (seen[j] || peer->file_count != 1 || strcmp(peer->family, unit->family) || strcmp(peer->name, unit->name)) continue;
            seen[j] = 1; members[count++] = j;
        }
        if (!pl_hands_group(b, sources, members, count, error, capacity)) goto done;
    }
    ok = 1;
done:
    free(seen); free(members); return ok;
}

static int pl_unit_name_order(void *context, const void *left, const void *right)
{
    const sh_package_link_unit *units = context;
    const sh_package_link_unit *a = units + *(const size_t *)left, *b = units + *(const size_t *)right;
    int order = strcmp(a->family, b->family);
    if (!order) order = strcmp(a->name, b->name);
    return order ? order : (*(const size_t *)left < *(const size_t *)right ? -1 : 1);
}
typedef struct pl_group { size_t first, count, parent; } pl_group;
/* Compose one phase's groups (runs of equal family and name in order), each
 * after the group of the parent it names, so a child rebases onto its
 * published parent. A cycle cannot order; the chain reader refuses it. */
static int pl_contribution_phase(pl_build *b, const sh_package_sources *sources, const size_t *order,
    size_t order_count, int phase, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    pl_group *groups = NULL;
    size_t group_count = 0, *stack = NULL, depth = 0;
    unsigned char *state = NULL;
    int ok = 0;
    for (size_t i = 0; i < order_count;) {
        const sh_package_link_unit *unit = c->inputs.units + order[i];
        size_t end = i + 1;
        while (end < order_count && !strcmp(c->inputs.units[order[end]].family, unit->family) &&
            !strcmp(c->inputs.units[order[end]].name, unit->name)) end++;
        if (sh_package_editor_contribution_phase(unit->family) == phase) {
            pl_group *grown = realloc(groups, (group_count + 1) * sizeof(*grown));
            if (!grown) { snprintf(error, capacity, "editor contribution allocation failed"); goto done; }
            groups = grown; groups[group_count++] = (pl_group){i, end - i, SIZE_MAX};
        }
        i = end;
    }
    if (!(state = calloc(group_count + 1, 1)) || !(stack = calloc(group_count + 1, sizeof(*stack)))) {
        snprintf(error, capacity, "editor contribution allocation failed"); goto done;
    }
    if (phase != 2) for (size_t g = 0; g < group_count; g++) {
        /* The parent the reference member (the stock one when present) names. */
        size_t reference = order[groups[g].first], length = 0;
        for (size_t k = 0; k < groups[g].count; k++)
            if (c->inputs.scopes[order[groups[g].first + k]] == b->count - 1) reference = order[groups[g].first + k];
        const sh_package_link_unit *unit = c->inputs.units + reference;
        unsigned char *body = sh_package_compilation_read(unit->compilation, unit->files[0], SIZE_MAX, &length, error, capacity);
        char *parent = NULL;
        if (!body) goto done;
        int read = pl_inherit_name((sh_decl_source){(const char *)body, length}, &parent);
        free(body);
        if (!read) continue; /* composition reports the unreadable text */
        for (size_t low = 0, high = group_count; parent && low < high;) {
            size_t middle = low + (high - low) / 2;
            const sh_package_link_unit *probe = c->inputs.units + order[groups[middle].first];
            int cmp = strcmp(probe->family, unit->family);
            if (!cmp) cmp = strcmp(probe->name, parent);
            if (!cmp) { groups[g].parent = middle; break; }
            if (cmp < 0) low = middle + 1; else high = middle;
        }
        free(parent);
    }
    for (size_t g = 0; g < group_count; g++) {
        if (state[g]) continue;
        stack[depth++] = g;
        while (depth) {
            size_t top = stack[depth - 1], parent = groups[top].parent;
            if (!state[top]) {
                state[top] = 1;
                if (parent != SIZE_MAX && !state[parent]) { stack[depth++] = parent; continue; }
            }
            depth--;
            if (state[top] == 2) continue;
            state[top] = 2;
            if (!pl_contribution_group(b, sources, order + groups[top].first, groups[top].count, error, capacity)) goto done;
        }
    }
    ok = 1;
done:
    free(groups); free(stack); free(state); return ok;
}

static int pl_contributions_open(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = calloc(1, sizeof(*c));
    const sh_package_link_inputs *original = sh_package_source_link_inputs(b->reader);
    size_t *members = NULL, eligible = 0;
    unsigned char *done = NULL;
    if (!c) return 0;
    b->contributions = c; c->source = b->reader; c->inputs.count = original->count; c->inputs.scopes = original->scopes;
    c->dirty = 1;
    c->pending_unit = c->pending_target = SIZE_MAX;
    c->reuse = sh_package_link_reuse_open(original->count);
    c->inputs.units = calloc(original->count ? original->count : 1, sizeof(*c->inputs.units));
    c->units = calloc(original->count ? original->count : 1, sizeof(*c->units));
    c->shared = calloc(original->count ? original->count : 1, 1);
    c->global = calloc(original->count ? original->count : 1, 1);
    members = calloc(original->count ? original->count : 1, sizeof(*members));
    done = calloc(original->count ? original->count : 1, 1);
    if (!c->reuse || !c->inputs.units || !c->units || !c->shared || !c->global || !members || !done) goto bad;
    memcpy(c->inputs.units, original->units, original->count * sizeof(*c->inputs.units));
    if (!pl_localize(b, sources, error, capacity)) goto bad;
    /* Group members once: eligible units sorted by family and name. */
    for (size_t i = 0; i < original->count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (unit->file_count == 1 && pl_modern(b, original->scopes[i]) && sh_package_editor_contribution_phase(unit->family))
            members[eligible++] = i;
    }
    qsort_s(members, eligible, sizeof(*members), pl_unit_name_order, c->inputs.units);
    (void)done;
    /* Package keys move before any shared collection composes, so inspector
     * choices and registration rows already carry each package's final key. */
    if (!pl_tokens(c, error, capacity) || !pl_key_phase(b, sources, error, capacity)) goto bad;
    for (int phase = 1; phase <= 3; phase++) {
        if (!pl_tokens(c, error, capacity)) goto bad;
        if (!pl_contribution_phase(b, sources, members, eligible, phase, error, capacity)) goto bad;
    }
    if (!pl_tokens(c, error, capacity) ||
        !pl_registration_phase(b, sources, members, eligible, error, capacity)) goto bad;
    if (!pl_tokens(c, error, capacity) || !pl_hands_phase(b, sources, error, capacity)) goto bad;
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

/* Registration keys are bound like typed identities: the bit a package
 * authored (original) and the bit it holds here (name), in its source context,
 * so a map-authored key keeps its package across differently keyed libraries. */
static int pl_key_provenance(const pl_build *b, const sh_package_sources *sources,
    sh_package_provenance *out)
{
    for (size_t i = 0; i < b->key_count; i++) {
        const struct pl_key_binding *key = b->keys + i;
        char original[16], current[16];
        size_t selected;
        if (key->scope >= b->count) return 0;
        if (b->contexts) {
            const sh_package_provenance_context *recorded = b->contexts->contexts + key->scope;
            selected = sh_package_provenance_context_add(out, recorded->mode,
                recorded->trees, recorded->tree_count, recorded->component);
        } else {
            const sh_package_component *component = sources->components + b->views[key->scope].component;
            if (!sources->fingerprints || component->owner >= sources->package_count) return 0;
            selected = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])sh_package_source_tree(sources, component->owner),
                1, component->relative);
        }
        snprintf(original, sizeof(original), "%u", 1u << key->original);
        snprintf(current, sizeof(current), "%u", 1u << key->current);
        if (selected == SIZE_MAX ||
            !sh_package_provenance_bind(out, SH_PACKAGE_KEY_FAMILY, original, current, &selected, 1)) return 0;
    }
    return 1;
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
    b.contribution_owner = b.contribution_invalid = SIZE_MAX;
    sh_package_source_link_native source_native = {&b, pl_admit, pl_semantics,
        native ? native->source.shader : NULL, pl_material_companions, pl_material_seeds, pl_implicit, pl_source_owners};
    sh_package_compile_environment env;
    char fallback[1024]; int ok = 0;
    unsigned long long mark = pl_now_ms();
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
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_SCOPES, &mark);
    if (!pl_product_scope(&b, sources, error, capacity)) goto done;
    if (!pl_baseline_scope(&b, sources, env.baseline ? env.baseline : original,
        env.baseline ? env.baseline_context : original_context, error, capacity)) goto done;
    if (!pl_native_open(&b, catalog, original, original_context, registry, reflection, error, capacity)) goto done;
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_NATIVE, &mark);
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
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_SOURCE_LINK, &mark);
    if (b.reader) sh_package_source_link_phases(b.reader, out->source_phases, sizeof(out->source_phases));
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
    if (!pl_contributions_open(&b, sources, error, capacity)) {
        if (!contexts && b.contribution_invalid != SIZE_MAX && environment->invalid_package)
            *environment->invalid_package = b.contribution_invalid;
        else if (!contexts && b.contribution_owner != SIZE_MAX && environment->unserved_package)
            *environment->unserved_package = b.contribution_owner;
        goto done;
    }
    if (contexts && !pl_document_shared(&b, original, original_context, error, capacity)) goto done;
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_CONTRIBUTIONS, &mark);
    /* Authored documents carry their own selector/inspector behavior. Changed
     * editor units receive ordinary bound aliases just like their gameplay
     * dependencies, so importing them never rewrites the open editor's shared
     * objects. Certified vanilla bindings still retain native names. Local
     * library compilation continues composing its one shared editor surface. */
    pl_assignment assignment = {&b.contributions->inputs, b.contributions->shared,
        b.materials != NULL, b.contributions->global};
    b.contributions->assignment = &assignment;
    sh_package_link_registry identity_adapter = {0};
    if (native && native->identities.head_slot) {
        b.identities = sh_package_native_registry_open(native->identities, catalog, error, capacity);
        if (!b.identities) goto done;
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (((assignment.shared[i] && pl_shared_declaration(unit->family)) || assignment.global[i]) &&
                !sh_package_native_registry_share(b.identities, unit->family, unit->name)) goto done;
        }
        identity_adapter = sh_package_native_registry_adapter(b.identities);
    }
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_IDENTITIES, &mark);
    sh_package_link_adapter adapter = {b.contributions, pl_bound_resolve, pl_bound_bind, pl_bound_assign,
        b.identities ? &identity_adapter : NULL, pl_bound_assign_members, b.contributions->reuse};
    pl_reuse_refresh(b.contributions);
    if (!sh_package_link(sources, assignment.inputs->units, assignment.inputs->count, &adapter, &b.shared->policy,
        environment->canonical_path, &b.linked, error, capacity) ||
        (b.identities && !sh_package_native_registry_validate(b.identities, error, capacity))) goto done;
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_LINK, &mark);
    if (!pl_provenance(&b, sources, &out->bindings) || !pl_key_provenance(&b, sources, &out->bindings) ||
        !pl_hud(&b, error, capacity) ||
        !sh_package_sound_prepare(b.linked.compiled, error, capacity) ||
        !pl_consumers(assignment.inputs, &b.linked, &out->consumers, error, capacity)) goto done;
    if (b.materials) {
        out->virtual_materials = sh_package_virtual_material_prepare(b.materials, assignment.inputs,
            &b.linked, error, capacity);
        if (!out->virtual_materials) goto done;
    }
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_OUTPUTS, &mark);
    if (b.audio) {
        if (!sh_package_audio_program_prepare(b.audio, error, capacity)) goto done;
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (unit->file_count && !strcmp(unit->family, "sound") &&
                sh_package_audio_program_contains(b.audio, assignment.inputs->scopes[i], unit->name) &&
                !sh_package_audio_program_output(b.audio, assignment.inputs->scopes[i], unit->name,
                    b.linked.names[b.linked.classes.nodes[i]], error, capacity)) goto done;
        }
        out->audio = b.audio; b.audio = NULL;
    }
    pl_phase(out, SH_PACKAGE_LIBRARY_PHASE_AUDIO, &mark);
    out->compiled = b.linked.compiled; b.linked.compiled = NULL;
    out->compiled->independent_sources = 1; ok = 1;
done:
    pl_close(&b);
    if (!ok) {
        /* Accounted coverage and phase timings survive a later failure as
         * diagnostics only. */
        char *coverage = out->coverage; out->coverage = NULL;
        unsigned long long phases[SH_PACKAGE_LIBRARY_PHASES]; char source_phases[sizeof(out->source_phases)];
        memcpy(phases, out->phase_ms, sizeof(phases)); memcpy(source_phases, out->source_phases, sizeof(source_phases));
        sh_package_library_free(out); out->coverage = coverage;
        memcpy(out->phase_ms, phases, sizeof(phases)); memcpy(out->source_phases, source_phases, sizeof(source_phases));
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
