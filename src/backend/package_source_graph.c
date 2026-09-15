#include "package_source_graph.h"
#include "decl_entity_class.h"
#include "decl_graph_dependencies.h"
#include "decl_material.h"
#include "package_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sg_walk {
    const sh_package_compilation *compiled;
    const sh_resource_catalog *catalog;
    sh_package_baseline_reader original;
    void *original_context;
    sh_decl_registry_source native;
    sh_decl_native_registry *registry;
    sh_decl_native_schema *schema;
    sh_package_audio *audio;
    sh_package_references queue, paths;
    sh_package_source_graph_report report;
    const char *current_type, *current_name;
    char *error;
    size_t capacity;
    int failed;
} sg_walk;

static int sg_editor(const char *type)
{
    return !strncmp(type, "snapeditor", 10) || !strncmp(type, "snappropertyinspector", 21);
}

static char *sg_name(const char *name)
{
    char *copy;
    size_t i;
    if (!name || !(copy = _strdup(name))) return NULL;
    for (i = 0; copy[i]; i++) {
        unsigned char c = (unsigned char)copy[i];
        if (c >= 128) { free(copy); return NULL; }
        if (c == '\\') copy[i] = '/';
        else if (c >= 'A' && c <= 'Z') copy[i] += 'a' - 'A';
    }
    if (copy[0] == '/' && copy[1] != '/') memmove(copy, copy + 1, strlen(copy));
    return copy;
}

static char *sg_path(const char *type, const char *name)
{
    size_t a = strlen(type), b = strlen(name), capacity;
    char *raw, *path;
    if (a > SIZE_MAX - 23 || b > SIZE_MAX - a - 23) return NULL;
    capacity = a + b + 23;
    raw = malloc(capacity);
    if (!raw) return NULL;
    snprintf(raw, capacity, "generated/decls/%s/%s.decl", type, name);
    path = sh_package_engine_path(raw); free(raw); return path;
}

static void sg_gap(sg_walk *walk, const char *field, const char *type, const char *reason)
{
    walk->report.gaps++;
    if (!walk->report.first_gap[0]) snprintf(walk->report.first_gap, sizeof(walk->report.first_gap),
        "%s/%s %s (%s): %s", walk->current_type ? walk->current_type : "map",
        walk->current_name ? walk->current_name : "inline", field, type ? type : "?", reason);
}

static int sg_add(void *context, const char *type, const char *name)
{
    sg_walk *walk = context;
    char *normalized_type = sg_name(type), *normalized_name = sg_name(name);
    int ok = normalized_type && normalized_name;
    if (ok && *normalized_name && !sg_editor(normalized_type))
        ok = sh_package_references_add(&walk->queue, normalized_type, normalized_name);
    free(normalized_type); free(normalized_name);
    if (!ok) walk->failed = 1;
    return ok;
}

static int sg_file(sg_walk *walk, const char *path)
{
    char *canonical = walk->compiled->canonical_path ?
        walk->compiled->canonical_path(path) : sh_package_engine_path(path);
    int ok = canonical && sh_package_references_add(&walk->paths, "", canonical);
    free(canonical); if (!ok) walk->failed = 1; return ok;
}

static int sg_read_path(sg_walk *walk, const char *path, unsigned char **body, size_t *length)
{
    const sh_compiled_resource *resource = sh_package_compilation_find(walk->compiled, path);
    *body = NULL; *length = 0;
    if (resource) {
        *body = sh_package_compilation_read(walk->compiled, resource, (size_t)PTRDIFF_MAX,
            length, walk->error, walk->capacity);
        if (!*body) walk->failed = 1;
        return *body ? 1 : -1;
    }
    {
        int status = walk->original ? walk->original(walk->original_context, path, body, length) : 0;
        if (status < 0) walk->failed = 1;
        return status;
    }
}

static int sg_read_parent(void *context, const char *name, sh_decl_source *source)
{
    sg_walk *walk = context;
    unsigned char *body = NULL;
    char *path = sg_path("entitydef", name);
    int status = path && sg_file(walk, path) ? sg_read_path(walk, path, &body, &source->length) : -1;
    free(path); source->text = (const char *)body; return status > 0 ? 1 : status;
}

static int sg_derives(void *context, const char *child, const char *parent)
{ return sh_decl_native_schema_class_derives(((sg_walk *)context)->schema, child, parent); }

static int sg_native_read(void *context, uintptr_t address, void *out, size_t length)
{
    sg_walk *walk = context;
    return walk->native.read(walk->native.context, address, out, length);
}

static int sg_exists(void *context, uintptr_t manager, const char *type, const char *name)
{
    sg_walk *walk = context;
    unsigned char *body = NULL;
    size_t length = 0;
    char *path = sg_path(type, name);
    int status;
    (void)manager;
    if (!path) return -1;
    status = sg_read_path(walk, path, &body, &length);
    free(body); free(path); return status > 0 ? 1 : status;
}

static int sg_reference(void *context, const char *field, sh_decl_value_type type,
    const char *name, size_t length)
{
    sg_walk *walk = context;
    sh_decl_reference reference = {0};
    char *copy;
    int status, ok = 1;
    walk->report.references++;
    if (sh_decl_native_schema_reader(walk->schema, type) != SH_DECL_READER_DECL) {
        sg_gap(walk, field, type.name, "resource reader needs a source identity adapter"); return 1;
    }
    if (length == SIZE_MAX || !(copy = malloc(length + 1))) { walk->failed = 1; return 0; }
    memcpy(copy, name, length); copy[length] = 0;
    status = sh_decl_native_registry_resolve_source(walk->registry, type.name, copy, &reference);
    if (status > 0) ok = sg_add(walk, reference.type, reference.name);
    else sg_gap(walk, field, type.name, status < 0 ? "reference identity is unavailable" : "reference has no source identity");
    sh_decl_reference_clear(&reference); free(copy); return ok;
}

static void sg_field_gap(void *context, const char *field, sh_decl_value_type type, const char *reason)
{ sg_gap(context, field, type.name, reason); }

static int sg_material_schema(void *context, const char *family, sh_decl_source name, int *kind)
{
    sg_walk *walk = context;
    char *copy, *path; unsigned char *body = NULL;
    size_t length = 0; int status;
    /* Typed resource names are queued after parsing. No existence claim is
     * made here for opaque paths, samplers or shader permutations. */
    if (!strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler")) return 1;
    if (name.length == SIZE_MAX || !(copy = malloc(name.length + 1))) { walk->failed = 1; return -1; }
    memcpy(copy, name.text, name.length); copy[name.length] = 0;
    path = sg_path(family, copy); free(copy);
    if (!path) { walk->failed = 1; return -1; }
    status = sg_read_path(walk, path, &body, &length);
    if (status > 0 && !sg_file(walk, path)) status = -1;
    if (status > 0 && !strcmp(family, "renderparm") &&
        !sh_decl_material_parameter_kind((sh_decl_source){(const char *)body, length}, kind, NULL, 0)) status = -1;
    free(path); free(body); return status > 0 ? 1 : status;
}

static int sg_material_references(sg_walk *walk,
    const sh_decl_material_reference *references, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const sh_decl_material_reference *ref = &references[i];
        char *name;
        walk->report.references++;
        if (!strcmp(ref->family, "sampler")) {
            sg_gap(walk, "", "sampler", "sampler identity needs its native registry adapter"); continue;
        }
        if (ref->name.length == SIZE_MAX || !(name = malloc(ref->name.length + 1))) { walk->failed = 1; break; }
        memcpy(name, ref->name.text, ref->name.length); name[ref->name.length] = 0;
        if (!sg_add(walk, ref->family, name)) { free(name); break; }
        free(name);
    }
    return !walk->failed;
}

static int sg_material(sg_walk *walk, sh_decl_source source)
{
    sh_decl_material material = {0}; char detail[1024];
    sh_decl_material_schema schema = {walk, sg_material_schema};
    if (!sh_decl_material_read(source, &schema, &material, detail, sizeof(detail))) {
        sg_gap(walk, "", "material", detail); return !walk->failed;
    }
    (void)sg_material_references(walk, material.references, material.reference_count);
    sh_decl_material_free(&material); return !walk->failed;
}

static int sg_renderparm(sg_walk *walk, sh_decl_source source)
{
    sh_decl_renderparm parameter = {0}; char detail[1024];
    sh_decl_material_schema schema = {walk, sg_material_schema};
    if (!sh_decl_renderparm_read(source, &schema, &parameter, detail, sizeof(detail))) {
        sg_gap(walk, "", "renderparm", detail); return !walk->failed;
    }
    (void)sg_material_references(walk, parameter.references, parameter.reference_count);
    sh_decl_renderparm_free(&parameter); return !walk->failed;
}

static int sg_sound(sg_walk *walk, const char *name)
{
    const sh_package_audio_reference *references;
    uint32_t event;
    size_t count;
    if (!sh_audio_event_identity(name, &event)) {
        sg_gap(walk, "", "sound", "sound name cannot be represented by native event conversion"); return 1;
    }
    if (!walk->audio) {
        sh_package_audio_report report;
        walk->audio = sh_package_audio_open(walk->compiled, &report, walk->error, walk->capacity);
        if (!walk->audio) { walk->failed = 1; return 0; }
        if (report.invalid) sg_gap(walk, "", "sound bank", report.first_gap);
    }
    count = sh_package_audio_find(walk->audio, event, &references);
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bank *bank = references[i].metadata;
        walk->report.references++;
        if (!sg_file(walk, references[i].path)) return 0;
        /* A bank streams media from separate files. Carry the ones this
         * provider supplies; the rest stay installed originals. */
        for (size_t j = 0; bank && j < bank->media_count; j++) {
            const sh_package_audio_media *media;
            size_t supplied;
            if (!bank->media[j].stream) continue;
            supplied = sh_package_audio_find_media(walk->audio, bank->media[j].id, &media);
            for (size_t k = 0; k < supplied; k++) {
                walk->report.references++;
                if (!sg_file(walk, media[k].path)) return 0;
            }
        }
    }
    return 1;
}

static int sg_inspect(sg_walk *walk, const char *type, const char *name)
{
    sh_decl_source source = {0};
    sh_decl_node *tree = NULL;
    sh_decl_graph *graph = NULL;
    sh_decl_value_type state = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_entity_class_source classes = {walk, sg_read_parent, sg_derives};
    char *path = sg_path(type, name), *class_name = NULL;
    const char *native_class = NULL;
    const sh_compiled_resource *compiled_resource;
    unsigned char *body = NULL;
    int status, ok = 1;
    walk->current_type = type; walk->current_name = name;
    if (!path) { walk->failed = 1; return 0; }
    /* Sound event identity is assigned from the resource name after text
     * parsing. It can select a bank-only replacement even when the sound
     * declaration itself comes from the game or native default creation. */
    if (!strcmp(type, "sound") && !sg_sound(walk, name)) { ok = 0; goto done; }
    compiled_resource = sh_package_compilation_find(walk->compiled, path);
    /* Material is a verified custom grammar, independent of reflected edit
     * classes. Read the prospective provider before considering loaded state. */
    if (!strcmp(type, "material") || !strcmp(type, "renderparm")) {
        if (!sg_file(walk, path)) { ok = 0; goto done; }
        status = sg_read_path(walk, path, &body, &source.length);
        if (status <= 0 || !body) {
            sg_gap(walk, "", type, status < 0 ? "candidate source is unreadable" : "candidate source is absent"); goto done;
        }
        walk->report.declarations++; source.text = (const char *)body;
        ok = !strcmp(type, "material") ? sg_material(walk, source) : sg_renderparm(walk, source); goto done;
    }
    if (sh_decl_native_registry_type_class(walk->registry, type, &native_class) != 1 ||
        ((!compiled_resource || !compiled_resource->type) &&
         sh_decl_native_schema_reader(walk->schema, (sh_decl_value_type){native_class, "*"}) != SH_DECL_READER_DECL)) {
        const sh_resource_catalog_entry *const *entries;
        size_t count = sh_resource_catalog_find(walk->catalog, type, name, &entries);
        for (size_t i = 0; i < count; i++) if (*entries[i]->path && !sg_file(walk, entries[i]->path)) { ok = 0; break; }
        sg_gap(walk, "", type, "non-declaration resource requires its format reader"); goto done;
    }
    if (!sg_file(walk, path)) { ok = 0; goto done; }
    status = sg_read_path(walk, path, &body, &source.length);
    if (status <= 0 || !body) {
        sg_gap(walk, "", type, status < 0 ? "candidate source is unreadable" : "candidate source is absent"); goto done;
    }
    walk->report.declarations++; source.text = (const char *)body;
    if (strcmp(type, "entitydef")) {
        status = sh_decl_native_schema_decl_type(walk->schema, type, &state);
        if (status != 1) { sg_gap(walk, "", type, "declaration requires a custom source reader"); goto done; }
        status = sh_decl_native_schema_graph_type(walk->schema, state);
        if (status < 0) { sg_gap(walk, "", type, "graph reader metadata is unavailable"); goto done; }
        if (status) {
            graph = sh_decl_graph_open(source, NULL, 0);
            if (!graph) { sg_gap(walk, "", type, "graph source grammar is unsupported"); goto done; }
            (void)sh_decl_graph_dependencies(graph, type, walk->schema, &result);
            if (sh_decl_tree_member(sh_decl_graph_syntax(graph), "inherit"))
                sg_gap(walk, "inherit", type, "effective inherited graph state is not expanded");
            goto inspected;
        }
    }
    tree = sh_decl_tree_parse(source, NULL, 0);
    if (!tree) { sg_gap(walk, "", type, "declaration source grammar is unsupported"); goto done; }
    if (!strcmp(type, "entitydef")) {
        char class_error[1024] = "";
        class_name = sh_decl_entity_tree_class(tree, &classes, class_error, sizeof(class_error));
        if (!class_name) { sg_gap(walk, "class", type, class_error); goto done; }
        if (sh_decl_tree_member(tree, "inherit")) {
            sh_decl_node *effective = sh_decl_entity_expanded_state(tree, &classes, class_error, sizeof(class_error));
            if (effective) { sh_decl_tree_free(tree); tree = effective; }
            else sg_gap(walk, "inherit", type, class_error);
        }
        state = (sh_decl_value_type){class_name, ""};
        (void)sh_decl_entity_dependencies(tree, state, sh_decl_native_schema_view(walk->schema), &result);
    } else {
        const sh_decl_node *inherit = sh_decl_tree_member(tree, "inherit");
        const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
        if (inherit) {
            const char *parent; size_t length;
            if (sh_decl_tree_literal(inherit, &parent, &length)) {
                sh_decl_value_type pointer = {native_class, "*"};
                if (length && !sg_reference(walk, "inherit", pointer, parent, length)) { ok = 0; goto done; }
            } else sg_gap(walk, "inherit", type, "inheritance syntax is unsupported");
        }
        if (edit) (void)sh_decl_state_dependencies(edit, state, sh_decl_native_schema_view(walk->schema), &result);
    }
    if (strcmp(type, "entitydef") && sh_decl_tree_member(tree, "inherit"))
        sg_gap(walk, "inherit", type, "effective inherited field replacement is not expanded");
inspected:
    if (result.aborted) { sg_gap(walk, "", type, "source dependency inspection aborted"); if (walk->failed) ok = 0; }
done:
    free(path); free(body); free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph); return ok && !walk->failed;
}

static int sg_inline(void *context, const char *class_name, const char *inherit,
    const char *json, size_t length)
{
    sg_walk *walk = context;
    sh_decl_source parent = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_entity_class_source classes = {walk, sg_read_parent, sg_derives};
    sh_decl_value_type type = {class_name, ""};
    char *inherited_class = NULL, class_error[1024] = "";
    walk->current_type = "map"; walk->current_name = inherit;
    if (!class_name || !*class_name) {
        if (!inherit || !*inherit || sg_read_parent(walk, inherit, &parent) != 1 ||
            !(inherited_class = sh_decl_entity_class(parent, &classes, class_error, sizeof(class_error)))) {
            sg_gap(walk, "class", class_name, class_error[0] ? class_error : "inline class source is unavailable");
            free((void *)parent.text); return !walk->failed;
        }
        type.name = inherited_class;
    }
    (void)sh_decl_json_state_dependencies(json, length, type,
        sh_decl_native_schema_view(walk->schema), &result);
    if (result.aborted && !walk->failed) sg_gap(walk, "edit", type.name, "inline dependency inspection aborted");
    free((void *)parent.text); free(inherited_class); return !walk->failed;
}

int sh_package_source_graph(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity)
{
    sg_walk walk = {0};
    sh_decl_registry_source candidate;
    sh_decl_native_source metadata;
    sh_decl_dependency_observer observer;
    char fallback[1024];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (report) memset(report, 0, sizeof(*report));
    if (!out) return 0;
    sh_package_references_free(out);
    if (!compiled || !registry.read || !reflection || !json) goto done;
    walk.compiled = compiled; walk.catalog = catalog; walk.original = original;
    walk.original_context = original_context; walk.native = registry;
    walk.error = error; walk.capacity = capacity;
    candidate = (sh_decl_registry_source){&walk, sg_native_read, registry.registry, sg_exists};
    metadata = (sh_decl_native_source){&walk, sg_native_read, reflection};
    observer = (sh_decl_dependency_observer){&walk, sg_reference, sg_field_gap};
    walk.registry = sh_decl_native_registry_open(candidate, error, capacity);
    walk.schema = sh_decl_native_schema_open(metadata, observer, error, capacity);
    if (!walk.registry || !walk.schema) goto done;
    if (!sh_package_map_references(json, length, sg_add, &walk) ||
        !sh_package_map_states(json, length, sg_inline, &walk)) goto done;
    for (size_t i = 0; i < walk.queue.count; i++) {
        /* Strings have independent ownership: enqueue may move queue.items. */
        const char *type = walk.queue.items[i].type, *name = walk.queue.items[i].name;
        if (!sg_inspect(&walk, type, name)) goto done;
    }
    /* Source pointers do not cover late string consumers or opaque formats. */
    walk.paths.incomplete = 1; *out = walk.paths; memset(&walk.paths, 0, sizeof(walk.paths)); ok = 1;
done:
    if (!ok && !error[0]) snprintf(error, capacity, "candidate source dependency inspection failed");
    if (report) *report = walk.report;
    sh_package_references_free(&walk.paths); sh_package_references_free(&walk.queue);
    sh_package_audio_close(walk.audio);
    sh_decl_native_registry_close(walk.registry); sh_decl_native_schema_close(walk.schema); return ok;
}
