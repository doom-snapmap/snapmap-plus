#include "package_source_graph.h"
#include "package_keys.h"
#include "package_link_inputs.h"
#include "decl_entity_class.h"
#include "decl_graph_dependencies.h"
#include "decl_material.h"
#include "decl_md6_compose.h"
#include "md6_binary.h"
#include "model_binary.h"
#include "decl_native_reference.h"
#include "decl_native_rebind.h"
#include "decl_rebind.h"
#include "package_audio.h"
#include "package_sound.h"
#include "map_binding_sites.h"
#include "package_movie.h"
#include "package_font.h"
#include "decl_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sg_shader_abi {
    struct sg_shader_abi *next;
    char *name;
    unsigned char *source;
    size_t length;
    int kind, supplied;
    uint32_t index;
    const char *family;
    uint64_t mask;
} sg_shader_abi;
typedef struct sh_package_source_reader {
    const sh_package_compilation *compiled;
    const sh_resource_catalog *catalog;
    sh_package_baseline_reader original;
    void *original_context;
    sh_decl_registry_source native;
    sh_decl_native_registry *registry;
    sh_decl_native_schema *schema;
    sh_package_audio *audio;
    sh_package_source_audio audio_source;
    sh_package_audio_scope *audio_scope;
    sh_package_references queue, paths;
    sh_package_source_graph_report report;
    const char *current_type, *current_name;
    char *error;
    size_t capacity;
    int failed;
    sh_package_binding_resolver saved_binding;
    void *saved_context;
    /* Registration-key groups for map-authored values (package_keys.h). */
    sh_package_key_groups *key_groups;
    const sh_package_key_groups *other_key_groups;
    const sh_package_key_set *key_taken;
    sh_package_saved_keys_fn saved_keys;
    sh_package_bound_exists bound_exists;
    int bound_absent; /* The last lookup was positively absent from bound outputs. */
    void *bound_context;
    sh_package_source_shader shader;
    int shader_configured;
    int preparing; /* The last read retained a native preparation request (sh_package_source_reader_preparing). */
    char preparation[512]; /* The first such request's reason in the current shader open. */
    sg_shader_abi *shader_parameters;
    sh_package_source_admission admission;
    int native_origin; /* Inspecting an unchanged SnapMap original declaration. */
    /* Hashed membership for the queue and paths (the stock closure is large),
     * which queued identities a candidate declaration or the map itself names,
     * and gaps of identities only unchanged SnapMap originals named so far. */
    struct sg_set { size_t *slots; size_t capacity, count; } queue_set, paths_set;
    unsigned char *candidate;
    size_t candidate_capacity, current;
    int native_leaf;
    struct sg_deferred { size_t index; char *reason; } *deferred;
    size_t deferred_count, deferred_capacity;
    /* A path is required when collected for the map or a candidate; one only
     * unchanged SnapMap originals need is the game's own business. */
    unsigned char *required;
    size_t required_capacity;
    struct sg_deferred_path { size_t path, identity; } *deferred_paths;
    size_t deferred_path_count, deferred_path_capacity;
    sh_package_source_lookup lookup; /* sh_package_source_reader_lookups */
    void *lookup_context;
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
    char text[sizeof(walk->report.first_gap)];
    if (walk->native_origin) { walk->report.native_gaps++; return; }
    snprintf(text, sizeof(text), "%s/%s %s (%s): %s", walk->current_type ? walk->current_type : "map",
        walk->current_name ? walk->current_name : "inline", field, type ? type : "?", reason);
    if (walk->native_leaf) {
        /* Decided when the walk ends: a later candidate reference makes it real. */
        if (walk->deferred_count == walk->deferred_capacity) {
            size_t next = walk->deferred_capacity ? walk->deferred_capacity * 2 : 8;
            struct sg_deferred *grown = next <= SIZE_MAX / sizeof(*grown) ?
                realloc(walk->deferred, next * sizeof(*grown)) : NULL;
            if (!grown) { walk->failed = 1; return; }
            walk->deferred = grown; walk->deferred_capacity = next;
        }
        if (!(walk->deferred[walk->deferred_count].reason = _strdup(text))) { walk->failed = 1; return; }
        walk->deferred[walk->deferred_count++].index = walk->current; return;
    }
    walk->report.gaps++;
    if (!walk->report.first_gap[0]) snprintf(walk->report.first_gap, sizeof(walk->report.first_gap), "%s", text);
}

static uint64_t sg_hash(const char *type, const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char *c = type; *c; c++) { hash ^= (unsigned char)*c; hash *= UINT64_C(1099511628211); }
    hash ^= 0x1f; hash *= UINT64_C(1099511628211);
    for (const char *c = name; *c; c++) { hash ^= (unsigned char)*c; hash *= UINT64_C(1099511628211); }
    return hash;
}
/* Index of (type, name) in list, or SIZE_MAX. Slots hold index + 1. */
static size_t sg_set_find(const struct sg_set *set, const sh_package_references *list, const char *type, const char *name)
{
    if (!set->capacity) return SIZE_MAX;
    for (size_t at = (size_t)sg_hash(type, name) & (set->capacity - 1); set->slots[at]; at = (at + 1) & (set->capacity - 1)) {
        const sh_package_reference *item = list->items + set->slots[at] - 1;
        if (!strcmp(item->type, type) && !strcmp(item->name, name)) return set->slots[at] - 1;
    }
    return SIZE_MAX;
}
static int sg_set_insert(struct sg_set *set, const sh_package_references *list, size_t index)
{
    if ((set->count + 1) * 10 > set->capacity * 7) {
        size_t capacity = set->capacity ? set->capacity * 2 : 256;
        size_t *slots = capacity <= SIZE_MAX / sizeof(*slots) ? calloc(capacity, sizeof(*slots)) : NULL;
        if (!slots) return 0;
        for (size_t i = 0; i < set->capacity; i++) if (set->slots[i]) {
            const sh_package_reference *item = list->items + set->slots[i] - 1;
            size_t at = (size_t)sg_hash(item->type, item->name) & (capacity - 1);
            while (slots[at]) at = (at + 1) & (capacity - 1);
            slots[at] = set->slots[i];
        }
        free(set->slots); set->slots = slots; set->capacity = capacity;
    }
    const sh_package_reference *item = list->items + index;
    size_t at = (size_t)sg_hash(item->type, item->name) & (set->capacity - 1);
    while (set->slots[at]) at = (at + 1) & (set->capacity - 1);
    set->slots[at] = index + 1; set->count++; return 1;
}
/* Add without the list's linear duplicate scan; returns the item's index. */
static size_t sg_list_add(sg_walk *walk, sh_package_references *list, struct sg_set *set,
    const char *type, const char *name)
{
    size_t index = sg_set_find(set, list, type, name);
    if (index != SIZE_MAX) return index;
    sh_package_reference *items = list->count < SIZE_MAX / sizeof(*items) - 1 ?
        realloc(list->items, (list->count + 1) * sizeof(*items)) : NULL;
    char *type_copy = _strdup(type), *name_copy = _strdup(name);
    if (items) list->items = items;
    if (!items || !type_copy || !name_copy) { free(type_copy); free(name_copy); walk->failed = 1; return SIZE_MAX; }
    list->items[list->count] = (sh_package_reference){type_copy, name_copy};
    if (!sg_set_insert(set, list, list->count)) { walk->failed = 1; return SIZE_MAX; }
    return list->count++;
}

static int sg_add(void *context, const char *type, const char *name)
{
    sg_walk *walk = context;
    char *normalized_type = sg_name(type), *normalized_name = sg_name(name);
    int ok = normalized_type && normalized_name;
    if (ok && *normalized_name && !sg_editor(normalized_type)) {
        size_t index = sg_list_add(walk, &walk->queue, &walk->queue_set, normalized_type, normalized_name);
        ok = index != SIZE_MAX;
        if (ok && index >= walk->candidate_capacity) {
            size_t next = walk->candidate_capacity ? walk->candidate_capacity * 2 : 256;
            while (next <= index) next *= 2;
            unsigned char *grown = realloc(walk->candidate, next);
            if (!grown) ok = 0;
            else {
                memset(grown + walk->candidate_capacity, 0, next - walk->candidate_capacity);
                walk->candidate = grown; walk->candidate_capacity = next;
            }
        }
        /* Named by the map or a candidate declaration, not only by stock. */
        if (ok && !walk->native_origin && !walk->native_leaf) walk->candidate[index] = 1;
    }
    free(normalized_type); free(normalized_name);
    if (!ok) walk->failed = 1;
    return ok;
}

static void sg_lists_free(sg_walk *walk)
{
    free(walk->required); walk->required = NULL; walk->required_capacity = 0;
    free(walk->deferred_paths); walk->deferred_paths = NULL;
    walk->deferred_path_count = walk->deferred_path_capacity = 0;
    sh_package_references_free(&walk->queue); sh_package_references_free(&walk->paths);
    free(walk->queue_set.slots); free(walk->paths_set.slots);
    memset(&walk->queue_set, 0, sizeof(walk->queue_set)); memset(&walk->paths_set, 0, sizeof(walk->paths_set));
    free(walk->candidate); walk->candidate = NULL; walk->candidate_capacity = 0;
    for (size_t i = 0; i < walk->deferred_count; i++) free(walk->deferred[i].reason);
    free(walk->deferred); walk->deferred = NULL; walk->deferred_count = walk->deferred_capacity = 0;
}

static int sg_file(sg_walk *walk, const char *path)
{
    char *canonical = walk->compiled->canonical_path ?
        walk->compiled->canonical_path(path) : sh_package_engine_path(path);
    size_t index = canonical ? sg_list_add(walk, &walk->paths, &walk->paths_set, "", canonical) : SIZE_MAX;
    int ok = index != SIZE_MAX;
    free(canonical);
    if (ok && index >= walk->required_capacity) {
        size_t next = walk->required_capacity ? walk->required_capacity * 2 : 256;
        while (next <= index) next *= 2;
        unsigned char *grown = realloc(walk->required, next);
        if (!grown) ok = 0;
        else {
            memset(grown + walk->required_capacity, 0, next - walk->required_capacity);
            walk->required = grown; walk->required_capacity = next;
        }
    }
    if (ok && !walk->native_origin && !walk->native_leaf) walk->required[index] = 1;
    else if (ok && walk->native_leaf) {
        if (walk->deferred_path_count == walk->deferred_path_capacity) {
            size_t next = walk->deferred_path_capacity ? walk->deferred_path_capacity * 2 : 64;
            struct sg_deferred_path *grown = next <= SIZE_MAX / sizeof(*grown) ?
                realloc(walk->deferred_paths, next * sizeof(*grown)) : NULL;
            if (!grown) ok = 0;
            else { walk->deferred_paths = grown; walk->deferred_path_capacity = next; }
        }
        if (ok) walk->deferred_paths[walk->deferred_path_count++] = (struct sg_deferred_path){index, walk->current};
    }
    if (!ok) walk->failed = 1; return ok;
}

static int sg_read_path(sg_walk *walk, const char *path, unsigned char **body, size_t *length)
{
    const sh_compiled_resource *resource = sh_package_compilation_find(walk->compiled, path);
    *body = NULL; *length = 0;
    if (walk->lookup) walk->lookup(walk->lookup_context, path, resource != NULL);
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

static char *sg_lookup_path(sg_walk *walk, const char *type, const char *name)
{
    const char *replacement = NULL;
    if (walk->saved_binding) {
        char *family = sg_name(type);
        int status = family ? walk->saved_binding(walk->saved_context, "source", family, name, &replacement) : -1;
        free(family);
        if (status < 0 || (status && (!replacement || !*replacement))) { walk->failed = 1; return NULL; }
        if (status) name = replacement;
    }
    return sg_path(type, name);
}

static int sg_read_parent(void *context, const char *name, sh_decl_source *source)
{
    sg_walk *walk = context;
    unsigned char *body = NULL;
    char *path = sg_lookup_path(walk, "entitydef", name);
    int status = path && sg_file(walk, path) ? sg_read_path(walk, path, &body, &source->length) : -1;
    free(path); source->text = (const char *)body; return status > 0 ? 1 : status;
}

/* Parent reads for another reflected family follow the same bound names. An
 * open-bodied family is normalized to ordinary assignment text first. */
typedef struct sg_family_parent { sg_walk *walk; const char *type; } sg_family_parent;
static int sg_read_family_parent(void *context, const char *name, sh_decl_source *source)
{
    sg_family_parent *parent = context;
    unsigned char *body = NULL;
    size_t length = 0;
    char *path = sg_lookup_path(parent->walk, parent->type, name);
    int status = path && sg_file(parent->walk, path) ? sg_read_path(parent->walk, path, &body, &length) : -1;
    free(path);
    if (status > 0 && body && sh_decl_text_open_body_family(parent->type)) {
        sh_decl_node *tree = sh_decl_tree_parse_reflected_body((sh_decl_source){(const char *)body, length}, 0, NULL, 0);
        char *text = tree ? sh_decl_tree_write(tree, &length) : NULL;
        sh_decl_tree_free(tree); free(body); body = (unsigned char *)text;
        if (!body) status = -1;
    }
    if (status <= 0) { free(body); body = NULL; }
    source->text = (const char *)body; source->length = length; return status > 0 ? 1 : status;
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
    char *path;
    int status;
    (void)manager;
    if (walk->bound_exists) {
        char *family = sg_name(type);
        int exists = -1;
        status = family ? walk->bound_exists(walk->bound_context, family, name, &exists) : -1;
        free(family);
        if (status < 0 || (status && exists != 0 && exists != 1)) { walk->failed = 1; return -1; }
        if (status) { walk->bound_absent = !exists; return exists; }
    }
    path = sg_lookup_path(walk, type, name);
    if (!path) return -1;
    status = sg_read_path(walk, path, &body, &length);
    free(body); free(path); return status > 0 ? 1 : status;
}

/* Declaration text may keep a stock-style reference to a declaration that has
 * no source anywhere in its scope: the engine defaults it by name, as it does
 * for the stock original. A map's own reference must still resolve to an
 * actual source. A composed body keeps such references copied from stock
 * text; its composition tokens must always resolve (sg_composition_token). */
static int sg_native_default(const sg_walk *walk)
{ return walk->current_type && strcmp(walk->current_type, "map"); }
static int sg_composition_token(const char *name, size_t length)
{ return name && length >= 17 && !memcmp(name, "smp-contribution/", 17); }
/* An event decl argument (SH_DECL_EVENT_DECL_USE_OPS) whose declaration has no
 * source is the engine's default for that use, in a map as in a package: the
 * argument reader finds the name in its type's manager with makeDefault. A
 * reflected field of a map keeps the map's own strictness. */
static int sg_event_decl(sh_decl_value_type type)
{ return type.ops && !strcmp(type.ops, SH_DECL_EVENT_DECL_USE_OPS); }

/* A use whose loader defaults an absent resource (reference.make_default: an
 * event md6Anim argument finds its clip in the animation list, then loads it
 * with makeDefault and only warns). Like an alias clip
 * (sg_native_default_target), a clip that neither this scope nor SnapMap
 * ships, including one only the campaign archive carries, is an engine object
 * named by the use: declaration text keeps it with no edge. A supplied or
 * shipped clip binds to its supplier. A composition token binds strictly; a
 * captured identity binds, and one whose source is gone is refused, as for a
 * declaration (bound_absent). A map's own use defaults the same way in the
 * dependency walk (discovery), as the native reader does when the map loads; the
 * map binding walk leaves it to its per-site resolver, which holds captured
 * supplier authority. 1 defaults, 0 binds, -1 refused (walk->failed when the
 * lookup itself failed). */
static int sg_default_use(sg_walk *walk, const char *family, const char *name, int discovery)
{
    unsigned char *body = NULL; size_t length = 0;
    char *canonical, *path = NULL; int status = 0, exists = -1;
    if ((!discovery && !sg_native_default(walk)) || sg_composition_token(name, strlen(name))) return 0;
    if (!(canonical = sg_name(family))) { walk->failed = 1; return -1; }
    if (walk->bound_exists) status = walk->bound_exists(walk->bound_context, canonical, name, &exists);
    if (!status) path = sh_package_cooked_path(canonical, name);
    free(canonical);
    if (status < 0 || (status && exists != 0 && exists != 1)) { walk->failed = 1; return -1; }
    if (status) return exists ? 0 : -1;
    if (!path) return 0;
    status = sg_read_path(walk, path, &body, &length);
    free(body); free(path);
    if (status < 0) { walk->failed = 1; return -1; }
    return status == 0 || status == 2;
}

static int sg_reference(void *context, const char *field, sh_decl_value_type type,
    const char *name, size_t length)
{
    sg_walk *walk = context;
    sh_decl_native_reference reference = {0};
    int status, ok = 1;
    walk->report.references++;
    status = sh_decl_native_reference_read(walk->schema, walk->registry, type, name, length, &reference);
    if (status == 2) status = sg_native_default(walk) ||
        (sg_event_decl(type) && !sg_composition_token(name, length)) ? 0 : -1;
    if (status == 1 && reference.make_default) {
        int defaulted = sg_default_use(walk, reference.resource.type, reference.resource.name, 1);
        if (defaulted) status = defaulted > 0 ? 0 : -1;
    }
    if (status == 1) ok = sg_add(walk, reference.resource.type, reference.resource.name);
    else if (status < 0) sg_gap(walk, field, type.name, "reference identity is unavailable");
    sh_decl_native_reference_clear(&reference); return ok;
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
        /* A sampler value is inline renderer state, not a resource identity.
         * idParmBlock::Load resolves render programs and render parameters
         * through their declaration managers, but binds a sampler by name from
         * the renderer's own table, which creates its samplers from numeric
         * filter/repeat state. There is nothing for a package to deliver, so
         * this is a resolved reference rather than a coverage gap. */
        if (!strcmp(ref->family, "sampler")) continue;
        if (ref->name.length == SIZE_MAX || !(name = malloc(ref->name.length + 1))) { walk->failed = 1; break; }
        memcpy(name, ref->name.text, ref->name.length); name[ref->name.length] = 0;
        if (!sg_add(walk, ref->family, name)) { free(name); break; }
        free(name);
    }
    return !walk->failed;
}

/* An MD6 definition names its inherited definition, its bound mesh and one
 * animation per alias. The mesh and the animations are cooked binaries, so they
 * enter the queue as catalog identities and are resolved like any other
 * non-declaration resource. */
static int sg_md6_reference(void *context, const char *type, const char *name)
{
    sg_walk *walk = context;
    walk->report.references++;
    return sg_add(walk, type, name);
}

static int sg_native_site(void *context, const char *type, const char *name, size_t offset, size_t length)
{
    (void)offset; (void)length;
    return sg_md6_reference(context, type, name);
}

static int sg_native_grammar(sg_walk *walk, sh_decl_source source,
    sh_decl_native_binding_format format, const char *family)
{
    char detail[1024];
    if (!sh_decl_native_reference_sites(format, source, NULL,
        sg_native_site, walk, detail, sizeof(detail)) && !walk->failed)
        sg_gap(walk, "", family, detail[0] ? detail : "native declaration source is unsupported");
    return !walk->failed;
}

static int sg_md6(sg_walk *walk, sh_decl_source source)
{
    char detail[1024];
    if (!sh_decl_md6_references(source, sg_md6_reference, walk, detail, sizeof(detail)) && !walk->failed)
        sg_gap(walk, "", "md6def", detail[0] ? detail : "md6 definition source is unsupported");
    return !walk->failed;
}

/* A cooked mesh names the skeleton the engine loads with it and one material per
 * mesh and per material record. Read the bytes this candidate would supply, not
 * whatever is installed. */
static int sg_basemodel(sg_walk *walk, const char *path)
{
    unsigned char *body = NULL;
    size_t length = 0;
    char detail[1024];
    int status = sg_read_path(walk, path, &body, &length);
    if (status <= 0 || !body) {
        free(body);
        sg_gap(walk, "", "basemodel", status < 0 ? "cooked mesh is unreadable" : "cooked mesh is absent");
        return !walk->failed;
    }
    if (!sh_md6_mesh_references(body, length, sg_md6_reference, walk, detail, sizeof(detail)) && !walk->failed)
        sg_gap(walk, "", "basemodel", detail[0] ? detail : "cooked mesh envelope is unsupported");
    free(body); return !walk->failed;
}

static int sg_shader_error(sg_walk *walk, const char *reason, const char *name)
{
    if (walk->error && walk->capacity && !*walk->error)
        snprintf(walk->error, walk->capacity, "%s: %s", reason, name ? name : "renderprog");
    return 0;
}
int sh_package_source_reader_shader(sh_package_source_reader *reader,
    const sh_package_source_shader *shader)
{
    if (!reader || !shader || reader->shader_configured ||
        (shader->renderer != SH_PACKAGE_SHADER_OPENGL && shader->renderer != SH_PACKAGE_SHADER_VULKAN)) return 0;
    reader->shader = *shader; reader->shader_configured = 1; return 1;
}
static void sg_shader_clear(sg_walk *walk)
{
    while (walk->shader_parameters) {
        sg_shader_abi *item = walk->shader_parameters; walk->shader_parameters = item->next;
        free(item->name); free(item->source); free(item);
    }
}
/* Keep the first pending need's own reason: later checks reuse the buffer. */
static void sg_shader_pending(sg_walk *walk)
{
    walk->preparing = 1;
    if (!*walk->preparation && walk->error && *walk->error)
        snprintf(walk->preparation, sizeof(walk->preparation), "%s", walk->error);
}
/* 1 ready, 2 pending native preparation with the supplied declaration's kind
 * (sh_package_shader_schema), -1 otherwise. */
static int sg_shader_parameter(void *context, const char *name, int *kind)
{
    sg_walk *walk = context;
    unsigned char *body = NULL; size_t length = 0;
    char *path = sg_path("renderparm", name), *canonical = sg_name(name);
    int supplied, source_kind = -1, actual_kind = -1, ok = 0;
    uint32_t index = UINT32_MAX;
    if (!path || !canonical || !walk->shader_configured || !walk->shader.parameter) goto done;
    supplied = sg_read_path(walk, path, &body, &length);
    if (supplied < 0 || (supplied && !body)) goto done;
    if (supplied && !sh_decl_material_parameter_kind((sh_decl_source){(const char *)body, length},
        &source_kind, walk->error, walk->capacity)) goto done;
    int answer = walk->shader.parameter(walk->shader.context, canonical,
        (sh_decl_source){(const char *)body, length}, &actual_kind, &index, walk->error, walk->capacity);
    if (answer == 2) {
        /* Nothing is cached: the retry after preparation repeats every check. */
        sg_shader_pending(walk);
        if (supplied && source_kind >= 0 && source_kind <= 18) { *kind = source_kind; ok = 2; }
        goto done;
    }
    if (answer != 1 || actual_kind < 0 || actual_kind > 18 || index > 32767 ||
        (supplied && source_kind != actual_kind)) goto done;
    for (sg_shader_abi *item = walk->shader_parameters; item; item = item->next) {
        if (strcmp(item->family, "renderparm") || strcmp(item->name, canonical)) continue;
        if (item->kind != actual_kind || item->index != index || item->supplied != (supplied != 0) ||
            item->length != length || (length && memcmp(item->source, body, length))) {
            sg_shader_error(walk, "shader parameter ABI changed during source binding", name); goto done;
        }
        *kind = actual_kind; ok = 1; goto done;
    }
    sg_shader_abi *item = calloc(1, sizeof(*item));
    if (!item) goto done;
    item->name = canonical; canonical = NULL; item->source = body; body = NULL;
    item->length = length; item->kind = actual_kind; item->index = index; item->supplied = supplied != 0;
    item->family = "renderparm";
    item->next = walk->shader_parameters; walk->shader_parameters = item;
    *kind = actual_kind; ok = 1;
done:
    free(path); free(canonical); free(body);
    if (!ok) sg_shader_error(walk, "shader parameter needs a compatible fixed native ABI", name);
    return ok == 2 ? 2 : ok ? 1 : -1;
}
static int sg_shader_flag(sg_walk *walk, const char *name)
{
    unsigned char *body = NULL; size_t length = 0; uint64_t mask = 0;
    char *path = sg_path("renderprogflag", name), *canonical = sg_name(name);
    int supplied = -1, ok = 0;
    if (!path || !canonical || !walk->shader_configured || !walk->shader.flag) goto done;
    supplied = sg_read_path(walk, path, &body, &length);
    if (supplied < 0 || (supplied && !body)) goto done;
    int answer = walk->shader.flag(walk->shader.context, canonical,
        (sh_decl_source){(const char *)body, length}, &mask, walk->error, walk->capacity);
    if (answer == 2) sg_shader_pending(walk);
    if (answer != 1) goto done;
    for (sg_shader_abi *item = walk->shader_parameters; item; item = item->next) {
        if (strcmp(item->family, "renderprogflag") || strcmp(item->name, canonical)) continue;
        if (item->mask != mask || item->supplied != (supplied != 0) || item->length != length ||
            (length && memcmp(item->source, body, length))) {
            sg_shader_error(walk, "shader flag ABI changed during source binding", name); goto done;
        }
        ok = 1; goto done;
    }
    sg_shader_abi *item = calloc(1, sizeof(*item));
    if (!item) goto done;
    item->family = "renderprogflag"; item->name = canonical; canonical = NULL;
    item->source = body; body = NULL; item->length = length; item->mask = mask;
    item->supplied = supplied != 0; item->next = walk->shader_parameters; walk->shader_parameters = item; ok = 1;
done:
    free(body); free(path); free(canonical);
    if (!ok) sg_shader_error(walk, "shader flag needs a compatible fixed native mask", name);
    return ok;
}
typedef struct sg_shader_paths { char declaration[256], binary[256]; } sg_shader_paths;
static int sg_shader_first_paths(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sg_shader_paths *paths = context; (void)linked;
    if (kind == SH_PACKAGE_SHADER_DECLARATION) snprintf(paths->declaration, 256, "%s", original);
    if (kind == SH_PACKAGE_SHADER_BINARY) snprintf(paths->binary, 256, "%s", original);
    return 1;
}
sh_package_source_shader_status sh_package_source_shader_open(sh_package_source_reader *reader, const char *name,
    unsigned char **declaration, size_t *declaration_length,
    unsigned char **binary, size_t *binary_length, sh_package_shader_program *program,
    char *error, size_t capacity)
{
    sg_shader_paths paths = {0}; sh_package_shader_program empty = {0};
    sh_package_shader_schema schema = {reader, sg_shader_parameter};
    char *old_error; size_t old_capacity; sh_package_source_shader_status ok = SH_PACKAGE_SOURCE_SHADER_ERROR;
    if (error && capacity) *error = 0;
    if (declaration) *declaration = NULL;
    if (declaration_length) *declaration_length = 0;
    if (binary) *binary = NULL;
    if (binary_length) *binary_length = 0;
    if (program) memset(program, 0, sizeof(*program));
    if (!reader || !name || !declaration || !declaration_length || !binary || !binary_length || !program) return 0;
    old_error = reader->error; old_capacity = reader->capacity;
    reader->error = error; reader->capacity = capacity; reader->preparing = 0; *reader->preparation = 0;
    if (!reader->shader_configured) { sg_shader_error(reader, "shader renderer contract is unavailable", name); goto done; }
    if (sh_package_shader_paths(reader->shader.renderer, name, name, &empty,
        sg_shader_first_paths, &paths, error, capacity) != SH_PACKAGE_SHADER_OK) goto done;
    int declaration_status = sg_read_path(reader, paths.declaration, declaration, declaration_length);
    int binary_status = declaration_status > 0 ? sg_read_path(reader, paths.binary, binary, binary_length) : 0;
    if (declaration_status > 0 && !binary_status && reader->shader.program &&
        reader->shader.program(reader->shader.context, name,
            (sh_decl_source){(const char *)*declaration, *declaration_length}, error, capacity) == 1) {
        ok = SH_PACKAGE_SOURCE_SHADER_PREPARING; goto done;
    }
    if (declaration_status <= 0 || binary_status <= 0) {
        sg_shader_error(reader, "shader declaration or renderer binary is absent", name); goto done;
    }
    int decoded = sh_package_shader_decode(*binary, *binary_length, &schema, program, error, capacity);
    if (decoded != SH_PACKAGE_SHADER_OK && decoded != SH_PACKAGE_SHADER_PENDING) goto done;
    if (program->declaration_checksum != sh_package_shader_declaration_checksum(*declaration, *declaration_length)) {
        sg_shader_error(reader, "shader binary does not match its scoped declaration", name); goto done;
    }
    /* Every pending parameter or flag joins one preparation round instead of
     * costing a complete library compile each; any other failure stops. */
    for (size_t i = 0; i < program->reference_count; i++) {
        const sh_package_shader_reference *ref = program->references + i; int kind = -1, was = reader->preparing;
        reader->preparing = 0;
        int ready = ref->kind == SH_PACKAGE_SHADER_PARAMETER ? sg_shader_parameter(reader, ref->name, &kind) == 1 :
            ref->kind == SH_PACKAGE_SHADER_FLAG ? sg_shader_flag(reader, ref->name) : 1;
        int pending = reader->preparing; reader->preparing |= was;
        if (!ready && !pending) goto done;
    }
    if (decoded == SH_PACKAGE_SHADER_OK && !reader->preparing) ok = SH_PACKAGE_SOURCE_SHADER_READY;
done:
    /* A parameter or flag awaiting native preparation: retry after it. The
     * first need's own reason names it; a decode framed by declared kinds is
     * discarded below with everything else. */
    if (ok == SH_PACKAGE_SOURCE_SHADER_ERROR && reader->preparing) {
        ok = SH_PACKAGE_SOURCE_SHADER_PREPARING;
        if (*reader->preparation && error && capacity) snprintf(error, capacity, "%s", reader->preparation);
    }
    if (ok != SH_PACKAGE_SOURCE_SHADER_READY) {
        free(*declaration); free(*binary); *declaration = *binary = NULL; *declaration_length = *binary_length = 0;
        sh_package_shader_program_clear(program);
    }
    reader->error = old_error; reader->capacity = old_capacity; return ok;
}
typedef struct sg_shader_walk {
    sg_walk *reader;
    const unsigned char *binary;
    const sh_package_shader_program *program;
    const char *member;
    int matched;
} sg_shader_walk;
static int sg_shader_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sg_shader_walk *shader = context; sg_walk *walk = shader->reader;
    unsigned char *body = NULL; size_t length = 0; int ok = 0;
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) return sg_add(walk, "renderprog", original);
    if (shader->member && !strcmp(shader->member, original)) shader->matched = 1;
    if (!sg_file(walk, original) || sg_read_path(walk, original, &body, &length) <= 0) {
        sg_shader_error(walk, "shader companion is absent or unreadable", original); goto done;
    }
    if (kind == SH_PACKAGE_SHADER_SPIRV) {
        const char *extension = strrchr(original, '.');
        int stage = extension && !strcmp(extension, ".vspv") ? 0 :
            extension && !strcmp(extension, ".fspv") ? 1 : extension && !strcmp(extension, ".cspv") ? 2 : -1;
        if (stage < 0) goto done;
        const sh_package_shader_stage *source = shader->program->stages + stage;
        if (sh_package_shader_spirv(body, length, shader->binary + source->offset, source->length,
            walk->error, walk->capacity) != SH_PACKAGE_SHADER_OK) goto done;
    }
    ok = 1;
done:
    free(body); return ok;
}
static int sg_renderprog(sg_walk *walk, const char *name)
{
    unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
    sh_package_shader_program program = {0}; char detail[1024] = "";
    if (sh_package_source_shader_open(walk, name, &declaration, &declaration_length,
        &binary, &binary_length, &program, detail, sizeof(detail)) != SH_PACKAGE_SOURCE_SHADER_READY) {
        sg_gap(walk, "", "renderprog", detail[0] ? detail : "shader unit is unavailable"); return !walk->failed;
    }
    walk->report.declarations++;
    for (size_t i = 0; i < program.reference_count; i++) {
        const sh_package_shader_reference *ref = program.references + i;
        if (ref->kind == SH_PACKAGE_SHADER_INCLUDE) continue;
        walk->report.references++;
        if (!sg_add(walk, ref->family, ref->name)) break;
    }
    sg_shader_walk shader = {walk, binary, &program};
    if (!walk->failed && sh_package_shader_paths(walk->shader.renderer, name, name, &program,
        sg_shader_path, &shader, detail, sizeof(detail)) != SH_PACKAGE_SHADER_OK)
        sg_gap(walk, "", "renderprog", detail[0] ? detail : "shader companion validation failed");
    free(declaration); free(binary); sh_package_shader_program_clear(&program); return !walk->failed;
}

/* Families whose cooked payload names no other engine resource. Measured over
 * the installed corpus with a scan for resource-path-shaped text: image 7436
 * files / 2.78 GB, cm 20117 / 4.25 GB, aas 863 / 194 MB, skeleton 945 / 5.8 MB,
 * binaryfile 48 / 56 MB and md6rig 2 -- none contained one. Transparency atlas
 * entries (249 files / 88 MB, cooked images) and fga vector-field grids (7
 * files / 4 MB of comma-separated numbers) scanned the same way contain none.
 * A leaf still needs a verified payload path to establish availability. */
int sh_package_source_leaf_family(const char *type)
{
    static const char *const leaves[] = {"image", "skeleton", "cm", "aas", "binaryfile", "md6rig", "transsortatlas", "fga"};
    for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++)
        if (!strcmp(type, leaves[i])) return 1;
    return 0;
}

static int sg_open_body(const char *type)
{ return sh_decl_text_open_body_family(type); }

/* Leaves have no dependency grammar to read, but their source must still
 * belong to the candidate or an original baseline. Local availability cannot
 * supply an omitted authored image (or another same-named binary resource). */
static int sg_leaf_file(sg_walk *walk, const char *path)
{
    unsigned char *body = NULL;
    size_t length = 0;
    int status;
    if (!sg_file(walk, path)) return 0;
    if (sh_package_compilation_find(walk->compiled, path)) {
        if (walk->lookup) walk->lookup(walk->lookup_context, path, 1);
        return 1;
    }
    status = sg_read_path(walk, path, &body, &length);
    free(body);
    if (status <= 0) sg_gap(walk, "", walk->current_type,
        status < 0 ? "original resource source is unreadable" : "candidate and original resource sources are absent");
    return !walk->failed;
}

/* A cooked animation plays on exactly one skeleton, named in its header. The
 * per-skeleton default clips carry a placeholder there and name the skeleton in
 * their own identity instead; both forms are covered over the whole corpus. */
static int sg_anim(sg_walk *walk, const char *path, const char *name)
{
    unsigned char *body = NULL;
    size_t length = 0, before;
    char detail[1024];
    int status = sg_read_path(walk, path, &body, &length), emitted = 0;
    if (status <= 0 || !body) {
        free(body);
        sg_gap(walk, "", "anim", status < 0 ? "cooked animation is unreadable" : "cooked animation is absent");
        return !walk->failed;
    }
    before = walk->report.references;
    if (!sh_md6_anim_references(body, length, sg_md6_reference, walk, detail, sizeof(detail))) {
        if (!walk->failed)
            sg_gap(walk, "", "anim", detail[0] ? detail : "cooked animation envelope is unsupported");
        free(body); return !walk->failed;
    }
    emitted = walk->report.references != before;
    free(body);
    if (!emitted) {
        /* "<skeleton>.md6skl/<clip>.md6anim": the identity carries it. */
        const char *marker = strstr(name, ".md6skl/");
        char *skeleton;
        size_t used;
        if (!marker) { sg_gap(walk, "", "anim", "cooked animation names no skeleton"); return !walk->failed; }
        used = (size_t)(marker - name) + 7;
        skeleton = malloc(used + 1);
        if (!skeleton) { walk->failed = 1; return 0; }
        memcpy(skeleton, name, used); skeleton[used] = 0;
        walk->report.references++;
        if (!sg_add(walk, "skeleton", skeleton)) { free(skeleton); return 0; }
        free(skeleton);
    }
    return !walk->failed;
}

/* Static/discrete models bind material records. Particle meshes instead carry
 * only counted vertices, but still need a verified envelope before admission. */
static int sg_model(sg_walk *walk, const char *path, const char *family)
{
    unsigned char *body = NULL;
    size_t length = 0;
    char detail[1024];
    int status = sg_read_path(walk, path, &body, &length);
    if (status <= 0 || !body) {
        free(body);
        sg_gap(walk, "", family, status < 0 ? "cooked model is unreadable" : "cooked model is absent");
        return !walk->failed;
    }
    int ok = !strcmp(family, "staticparticlemodel") ? sh_particle_model_read(body, length, detail, sizeof(detail)) :
        !strcmp(family, "discreteanimation") ? sh_discrete_model_references(body, length, sg_md6_reference, walk, detail, sizeof(detail)) :
        sh_model_references(body, length, sg_md6_reference, walk, detail, sizeof(detail));
    if (!ok && !walk->failed) sg_gap(walk, "", family, detail[0] ? detail : "cooked model envelope is unsupported");
    free(body); return !walk->failed;
}

static int sg_material_request(void *context, const char *slot, const char *family, const char *name)
{ (void)slot; sg_walk *walk = context; walk->report.references++; return sg_add(walk, family, name); }
static int sg_material(sg_walk *walk, sh_decl_source source)
{
    sh_decl_material material = {0}; char detail[1024];
    sh_decl_material_schema schema = {walk, sg_material_schema};
    if (!sh_decl_material_read(source, &schema, &material, detail, sizeof(detail))) {
        sg_gap(walk, "", "material", detail); return !walk->failed;
    }
    (void)sg_material_references(walk, material.references, material.reference_count);
    if (!walk->failed && !sh_package_source_material_requests(walk, source, 0,
        sg_material_request, walk, detail, sizeof(detail))) sg_gap(walk, "", "material", detail);
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

int sh_package_source_reader_audio(sh_package_source_reader *reader,
    const sh_package_source_audio *audio)
{
    if (!reader || !audio || !audio->originals || reader->audio_scope) return 0;
    reader->audio_source = *audio; return 1;
}
static int sg_audio_path(void *context, const char *path, int original)
{
    sg_walk *walk = context; (void)original;
    walk->report.references++; return sg_file(walk, path);
}
static int sg_sound(sg_walk *walk, const char *name)
{
    const sh_package_audio_reference *references;
    uint32_t event;
    size_t count;
    int authored = sh_package_sound_event(walk->compiled, name, &event);
    if (!authored && !sh_audio_event_identity(name, &event)) {
        sg_gap(walk, "", "sound", "sound name cannot be represented by native event conversion"); return 1;
    }
    if (!walk->audio) {
        sh_package_audio_report report;
        walk->audio = sh_package_audio_open(walk->compiled, &report, walk->error, walk->capacity);
        if (!walk->audio) { walk->failed = 1; return 0; }
        if (report.invalid) sg_gap(walk, "", "sound bank", report.first_gap);
    }
    if (walk->audio_source.originals) {
        if (!walk->audio_scope) walk->audio_scope = sh_package_audio_scope_open(walk->compiled,
            walk->audio_source.originals, walk->error, walk->capacity);
        if (!walk->audio_scope) { walk->failed = 1; return 0; }
        /* As in the library, a SnapMap sound whose event no bank implements
         * keeps its native binding: the engine plays nothing for it (stock
         * demon player forms name such a sound), so nothing is required. An
         * event a package declaration authors still needs its bank. */
        if (!authored && !sh_package_audio_scope_implements(walk->audio_scope, event)) return 1;
        if (!sh_package_audio_scope_event(walk->audio_scope, event,
            sg_audio_path, walk, walk->error, walk->capacity)) {
            walk->failed = 1; return 0;
        }
        return 1;
    }
    count = sh_package_audio_find(walk->audio, event, &references);
    if (!count) sg_gap(walk, "", "sound", "event bank membership needs an original audio provider index");
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bank *bank = references[i].metadata;
        walk->report.references++;
        if (!sg_file(walk, references[i].path)) return 0;
        /* A bank streams media from separate files. An absent candidate entry
         * needs an original provider index, not an assumption of installation. */
        for (size_t j = 0; bank && j < bank->media_count; j++) {
            const sh_package_audio_media *media;
            size_t supplied;
            if (!bank->media[j].stream) continue;
            supplied = sh_package_audio_find_media(walk->audio, bank->media[j].id, &media);
            if (!supplied) sg_gap(walk, "", "sound media", "streamed media needs an original audio provider index");
            for (size_t k = 0; k < supplied; k++) {
                walk->report.references++;
                if (!sg_file(walk, media[k].path)) return 0;
            }
        }
    }
    if (count) sg_gap(walk, "", "sound", "bank membership requires complete audio event dependency coverage");
    return 1;
}

/* Present (1), positively absent (0) or unreadable (-1), without adding the
 * path to the closure. Candidate files win over the original baseline. */
static int sg_available(sg_walk *walk, const char *path)
{
    unsigned char *body = NULL; size_t length = 0;
    if (sh_package_compilation_find(walk->compiled, path)) {
        if (walk->lookup) walk->lookup(walk->lookup_context, path, 1);
        return 1;
    }
    int status = sg_read_path(walk, path, &body, &length);
    free(body); return status > 0 ? 1 : status;
}
/* Glyph forms carry no resource strings; the loader derives its material
 * names from the font identity and the form it selects. */
typedef struct sg_font_requests { sg_walk *walk; } sg_font_requests;
static int sg_font_material(void *context, const char *slot, const char *material, int optional)
{
    sg_walk *walk = ((sg_font_requests *)context)->walk;
    char *path = sg_path("material", material);
    int status = path ? sg_available(walk, path) : -1;
    (void)slot; free(path);
    if (status < 0) { walk->failed = 1; return -1; }
    if (!status) return optional ? 0 : 1; /* A missing required material is reported by its own inspection. */
    return sg_add(walk, "material", material) ? 1 : -1;
}
static int sg_font(sg_walk *walk, const char *name)
{
    sh_package_font_member members[3] = {0}; size_t count = 0; int ok = 1;
    for (unsigned i = 0; i < 3 && ok; i++) {
        sh_font_binary_paths paths;
        if (!sh_font_binary_paths_for(name, (sh_font_binary_variant)i, &paths)) { walk->failed = 1; return 0; }
        int status = sg_available(walk, paths.file);
        if (status < 0) ok = 0;
        else if (status && sg_file(walk, paths.file)) {
            members[count].path = _strdup(paths.file);
            ok = members[count].path && sg_read_path(walk, paths.file,
                (unsigned char **)&members[count].body, &members[count].length) > 0;
            count++;
        } else if (status) ok = 0;
        sh_font_binary_paths_clear(&paths);
    }
    if (ok && !count) sg_gap(walk, "", "font", "candidate and original glyph sources are absent");
    else if (ok) {
        sg_font_requests requests = {walk};
        char detail[256] = "";
        if (!sh_package_font_capture(name, members, count, sg_font_material, &requests, detail, sizeof(detail)) && !walk->failed)
            sg_gap(walk, "", "font", detail[0] ? detail : "glyph envelope is unsupported");
    }
    for (size_t i = 0; i < count; i++) { free((void *)members[i].path); free((void *)members[i].body); }
    return ok && !walk->failed;
}
/* A cooked UI movie is a constructor pair: the dictionary names materials and
 * fonts, and the constructor itself requests fixed GUI materials and the
 * language fallback fonts. Local text and ActionScript bytes stay opaque. */
typedef struct sg_movie_requests { sg_walk *walk; } sg_movie_requests;
static int sg_movie_request(void *context, const char *slot, const char *family, const char *name)
{
    sg_walk *walk = ((sg_movie_requests *)context)->walk;
    int status;
    (void)slot;
    if (!strcmp(family, "font")) {
        sh_font_binary_paths paths; status = 0;
        for (unsigned i = 0; i < 3 && status == 0; i++) {
            if (!sh_font_binary_paths_for(name, (sh_font_binary_variant)i, &paths)) return -1;
            status = sg_available(walk, paths.file);
            sh_font_binary_paths_clear(&paths);
        }
    } else {
        char *path = sg_path(family, name);
        status = path ? sg_available(walk, path) : -1;
        free(path);
    }
    if (status < 0) { walk->failed = 1; return -1; }
    return status && !sg_add(walk, family, name) ? -1 : status;
}
static int sg_movie_material(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    (void)offset; (void)length;
    return sg_add(context, family, name);
}
static int sg_movie(sg_walk *walk, const char *name)
{
    sh_package_movie_names names;
    unsigned char *body = NULL; size_t length = 0; int status;
    if (sh_package_movie_names_for_path(name, &names) != 1) {
        sg_gap(walk, "", "file", "cooked movie pair cannot be derived from its native name"); return 1;
    }
    if (!sg_file(walk, names.movie) || !sg_leaf_file(walk, names.atlas)) return 0;
    status = sg_read_path(walk, names.movie, &body, &length);
    if (status <= 0) {
        sg_gap(walk, "", "file", status < 0 ? "candidate movie is unreadable" : "candidate and original movie sources are absent");
        free(body); return !walk->failed;
    }
    walk->report.declarations++;
    sg_movie_requests requests = {walk}; char detail[256] = "";
    int ok = sh_package_movie_material_sites(NULL, body, length, sg_movie_material, walk, detail, sizeof(detail)) &&
        sh_package_movie_capture(body, length, sg_movie_request, &requests, detail, sizeof(detail));
    if (!ok && !walk->failed) sg_gap(walk, "", "file", detail[0] ? detail : "cooked movie envelope is unsupported");
    free(body); return !walk->failed;
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
    sh_decl_native_binding_format native_format;
    unsigned char *body = NULL;
    int status, ok = 1;
    walk->current_type = type; walk->current_name = name;
    if (!path) { walk->failed = 1; return 0; }
    /* Sound event identity is assigned from the resource name after text
     * parsing. It can select a bank-only replacement even when the sound
     * declaration itself comes from the game or native default creation. */
    if (!strcmp(type, "sound") && !sg_sound(walk, name)) { ok = 0; goto done; }
    compiled_resource = sh_package_compilation_find(walk->compiled, path);
    /* Only unchanged SnapMap originals have named this identity so far and the
     * candidate does not supply it: its gaps wait for the walk's end. */
    walk->native_leaf = walk->current < walk->queue.count && walk->candidate &&
        walk->current < walk->candidate_capacity && !walk->candidate[walk->current] && !compiled_resource;
    for (size_t i = 0; walk->native_leaf && i < walk->compiled->resource_count; i++)
        if (sh_package_resource_native_matches(walk->compiled->resources + i, type, name)) walk->native_leaf = 0;
    if (!strcmp(type, "renderprog")) { ok = sg_renderprog(walk, name); goto done; }
    if (sh_package_movie_owner(type, name)) { ok = sg_movie(walk, name); goto done; }
    if (!strcmp(type, "font")) { ok = sg_font(walk, name); goto done; }
    if (!strcmp(type, "renderprogflag")) {
        if (!sg_file(walk, path) || !sg_shader_flag(walk, name))
            sg_gap(walk, "", type, "shader flag needs its fixed native mask contract");
        goto done;
    }
    /* Material is a verified custom grammar, independent of reflected edit
     * classes. Read the prospective provider before considering loaded state. */
    if (sh_decl_native_binding_family(type, &native_format)) {
        if (!sg_file(walk, path)) { ok = 0; goto done; }
        status = sg_read_path(walk, path, &body, &source.length);
        if (status <= 0 || !body) {
            sg_gap(walk, "", type, status < 0 ? "candidate source is unreadable" : "candidate source is absent"); goto done;
        }
        walk->native_origin = !compiled_resource && status == 1;
        walk->report.declarations++; source.text = (const char *)body;
        ok = !strcmp(type, "material") ? sg_material(walk, source) :
            !strcmp(type, "md6def") ? sg_md6(walk, source) :
            !strcmp(type, "renderparm") ? sg_renderparm(walk, source) :
            sg_native_grammar(walk, source, native_format, type); goto done;
    }
    if (sh_decl_native_registry_type_class(walk->registry, type, &native_class) != 1 ||
        ((!compiled_resource || !compiled_resource->type) &&
         sh_decl_native_schema_reader(walk->schema, (sh_decl_value_type){native_class, "*"}) != SH_DECL_READER_DECL)) {
        const sh_resource_catalog_entry *const *entries;
        size_t count = sh_resource_catalog_find(walk->catalog, type, name, &entries), known = 0;
        int discrete = !strcmp(type, "discreteanimation");
        int cooked = !strcmp(type, "basemodel"), model = !strcmp(type, "model") || discrete || !strcmp(type, "staticparticlemodel");
        int anim = !strcmp(type, "anim");
        int leaf = sh_package_source_leaf_family(type), inspected = 0;
        for (size_t i = 0; i < count; i++) {
            if (!*entries[i]->path) continue;
            known++;
            if (!(leaf ? sg_leaf_file(walk, entries[i]->path) : sg_file(walk, entries[i]->path))) { ok = 0; break; }
            if (leaf || (!cooked && !model && !anim)) continue;
            if (!(cooked ? sg_basemodel(walk, entries[i]->path) :
                  model ? sg_model(walk, entries[i]->path, type) : sg_anim(walk, entries[i]->path, name))) { ok = 0; break; }
            inspected = 1;
        }
        /* Linked binary aliases have compiler-assigned native identities even
         * when they do not occur in the game's catalog. Keep every companion
         * belonging to that identity, and inspect its candidate bytes. */
        for (size_t i = 0; ok && i < walk->compiled->resource_count; i++) {
            const sh_compiled_resource *file = walk->compiled->resources + i;
            int catalogued = 0;
            if (!sh_package_resource_native_matches(file, type, name)) continue;
            for (size_t j = 0; j < count; j++)
                if (!strcmp(entries[j]->path, file->engine_path)) { catalogued = 1; break; }
            if (catalogued) continue;
            known++;
            if (!sg_file(walk, file->engine_path)) { ok = 0; break; }
            if (leaf || (!cooked && !model && !anim)) continue;
            inspected = cooked ? sg_basemodel(walk, file->engine_path) :
                model ? sg_model(walk, file->engine_path, type) : sg_anim(walk, file->engine_path, name);
            if (!inspected) ok = 0;
        }
        if (leaf) inspected = known != 0;
        /* With no catalogued or supplied file, the library link still admits
         * a SnapMap row registered without data and a verified engine-created
         * identity such as the intrinsic _black image. Nothing is delivered. */
        if (ok && !known) {
            unsigned char semantics[32];
            for (size_t i = 0; !inspected && i < count; i++)
                inspected = !*entries[i]->path && entries[i]->archive < 2;
            if (!inspected && walk->admission.admit) {
                int admitted = walk->admission.admit(walk->admission.context, 0, type, name,
                    semantics, walk->error, walk->capacity);
                if (admitted < 0) { walk->failed = 1; ok = 0; }
                else inspected = admitted;
            }
        }
        /* Image generated-path spelling is a verified native rule. Other leaf
         * families require catalog or compiler identities; no guessed paths. */
        if (ok && !known && !inspected && (!strcmp(type, "image") || !strcmp(type, "transsortatlas"))) {
            char *image_path = sh_package_cooked_path(type, name);
            if (image_path) { inspected = sg_leaf_file(walk, image_path); free(image_path); }
            ok = !walk->failed;
        }
        /* A mesh this candidate adds has no catalog row; its cooked path follows
         * the same naming rule, so the provider's own bytes can still be read. */
        if (ok && (cooked || model || anim) && !inspected) {
            char *cooked_path = sh_package_cooked_path(type, name);
            if (!cooked_path) { walk->failed = 1; ok = 0; }
            else {
                if (sg_file(walk, cooked_path))
                    inspected = cooked ? sg_basemodel(walk, cooked_path) :
                        model ? sg_model(walk, cooked_path, type) : sg_anim(walk, cooked_path, name);
                ok = !walk->failed;
                free(cooked_path);
            }
        }
        if (ok && !inspected) sg_gap(walk, "", type, leaf ?
            "resource payload path is unavailable" : "non-declaration resource requires its format reader");
        goto done;
    }
    if (!sg_file(walk, path)) { ok = 0; goto done; }
    status = sg_read_path(walk, path, &body, &source.length);
    if (status <= 0 || !body) {
        sg_gap(walk, "", type, status < 0 ? "candidate source is unreadable" : "candidate source is absent"); goto done;
    }
    walk->native_origin = !compiled_resource && status == 1;
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
    tree = sg_open_body(type) ? sh_decl_tree_parse_reflected_body(source, 0, NULL, 0) :
        sh_decl_tree_parse(source, NULL, 0);
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
            if (sh_decl_reference_literal(inherit, &parent, &length)) {
                sh_decl_value_type pointer = {native_class, "*"};
                if (length && !sg_reference(walk, "inherit", pointer, parent, length)) { ok = 0; goto done; }
            } else sg_gap(walk, "inherit", type, "inheritance syntax is unsupported");
            /* The parent declaration and its whole closure stay dependencies
             * above. Walk the child's fields in the effective state its
             * inherited values establish, overlaid like entity state, so a
             * field typed by the parent is read with that type. */
            if (!walk->failed) {
                char detail[512] = "";
                sg_family_parent reader = {walk, type};
                sh_decl_entity_class_source parents = {&reader, sg_read_family_parent, NULL};
                sh_decl_node *effective = sh_decl_entity_expanded_state(tree, &parents, detail, sizeof(detail));
                if (walk->failed) { sh_decl_tree_free(effective); ok = 0; goto done; }
                if (effective) { sh_decl_tree_free(tree); tree = effective; edit = sh_decl_tree_member(tree, "edit"); }
                else sg_gap(walk, "inherit", type, detail[0] ? detail : "effective inherited state is unsupported");
            }
        }
        if (edit) (void)sh_decl_state_dependencies(edit, state, sh_decl_native_schema_view(walk->schema), &result);
    }
inspected:
    if (result.aborted) { sg_gap(walk, "", type, "source dependency inspection aborted"); if (walk->failed) ok = 0; }
done:
    walk->native_origin = walk->native_leaf = 0;
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

sh_package_source_reader *sh_package_source_reader_open(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    char *error, size_t capacity)
{
    sg_walk *walk = calloc(1, sizeof(*walk));
    if (!walk || !compiled || !registry.read || !reflection) goto failed;
    walk->compiled = compiled; walk->catalog = catalog; walk->original = original;
    walk->original_context = original_context; walk->native = registry;
    walk->error = error; walk->capacity = capacity;
    walk->registry = sh_decl_native_registry_open(
        (sh_decl_registry_source){walk, sg_native_read, registry.registry, sg_exists}, error, capacity);
    walk->schema = sh_decl_native_schema_open(
        (sh_decl_native_source){walk, sg_native_read, reflection, registry.scripts},
        (sh_decl_dependency_observer){walk, sg_reference, sg_field_gap}, error, capacity);
    if (walk->registry && walk->schema) {
        sh_decl_native_schema_attach_registry(walk->schema, walk->registry);
        return walk;
    }
failed:
    sh_package_source_reader_close(walk); return NULL;
}

void sh_package_source_reader_lookups(sh_package_source_reader *reader,
    sh_package_source_lookup lookup, void *context)
{
    if (reader) { reader->lookup = lookup; reader->lookup_context = lookup ? context : NULL; }
}
int sh_package_source_reader_preparing(const sh_package_source_reader *reader)
{ return reader && reader->preparing; }
typedef struct sg_digest { unsigned char *bytes; size_t length, capacity; int failed; } sg_digest;
static int sg_digest_emit(void *context, const void *bytes, size_t length)
{
    sg_digest *d = context;
    if (d->failed || length > SIZE_MAX - d->length) { d->failed = 1; return 0; }
    if (d->length + length > d->capacity) {
        size_t next = d->capacity ? d->capacity : 65536;
        while (next < d->length + length) {
            if (next > SIZE_MAX / 2) { d->failed = 1; return 0; }
            next *= 2;
        }
        unsigned char *grown = realloc(d->bytes, next);
        if (!grown) { d->failed = 1; return 0; }
        d->bytes = grown; d->capacity = next;
    }
    memcpy(d->bytes + d->length, bytes, length); d->length += length; return 1;
}
int sh_package_source_reader_native_digest(sh_package_source_reader *reader, unsigned char out[32])
{
    sg_digest d = {0}; sh_package_file_identity identity; int ok = 0;
    if (!reader || !out) return 0;
    if (sh_decl_native_registry_digest(reader->registry, sg_digest_emit, &d) &&
        sh_decl_native_schema_digest(reader->schema, sg_digest_emit, &d) && !d.failed &&
        sh_package_bytes_identity(d.bytes, d.length, &identity)) { memcpy(out, identity.digest, 32); ok = 1; }
    free(d.bytes); return ok;
}

void sh_package_source_reader_close(sh_package_source_reader *reader)
{
    if (!reader) return;
    sg_lists_free(reader);
    sh_package_audio_close(reader->audio); sh_package_audio_scope_close(reader->audio_scope);
    sg_shader_clear(reader);
    sh_decl_native_registry_close(reader->registry); sh_decl_native_schema_close(reader->schema);
    free(reader);
}

char *sh_package_source_material_path(sh_package_source_reader *reader,
    const char *name, const char *parameter, char *error, size_t capacity)
{
    unsigned char *material_bytes = NULL, *parameter_bytes = NULL;
    size_t material_length = 0, parameter_length = 0;
    char *path = NULL, *out = NULL;
    sh_decl_material material = {0}; sh_decl_renderparm defaults = {0};
    sh_decl_tokens tokens = {0}; sh_decl_source selected = {0};
    sh_decl_material_schema schema = {reader, sg_material_schema};
    if (error && capacity) *error = 0;
    if (!reader || !name || !parameter || !*parameter) goto done;
    reader->error = error; reader->capacity = capacity; reader->failed = 0;
    path = sg_path("renderparm", parameter);
    if (!path || sg_read_path(reader, path, &parameter_bytes, &parameter_length) <= 0 ||
        !sh_decl_renderparm_read((sh_decl_source){(char *)parameter_bytes, parameter_length},
            &schema, &defaults, error, capacity) || defaults.kind != 12) goto done;
    free(path); path = sg_path("material", name);
    if (!path || sg_read_path(reader, path, &material_bytes, &material_length) <= 0 ||
        !sh_decl_material_read((sh_decl_source){(char *)material_bytes, material_length},
            &schema, &material, error, capacity)) goto done;
    selected = defaults.value;
    for (size_t i = 0; i < material.count; i++) {
        const sh_decl_material_write *write = material.writes + i;
        if (write->name.length == strlen(parameter) &&
            !_strnicmp(write->name.text, parameter, write->name.length)) {
            if (write->kind != 12) goto done;
            selected = write->value;
        }
    }
    if (!selected.text) selected = (sh_decl_source){"", 0};
    if (reader->failed || !sh_decl_native_lex(selected, &tokens, error, capacity) || tokens.count > 1) goto done;
    size_t begin = 0, length = 0;
    if (tokens.count) {
        const sh_decl_token *token = tokens.items;
        if (token->kind != SH_DECL_TOKEN_NAME && token->kind != SH_DECL_TOKEN_STRING) goto done;
        begin = token->begin; length = token->end - begin;
        if (token->kind == SH_DECL_TOKEN_STRING) { begin++; length -= 2; }
    }
    if (length == SIZE_MAX || !(out = malloc(length + 1))) goto done;
    memcpy(out, selected.text + begin, length); out[length] = 0;
done:
    sh_decl_tokens_free(&tokens); sh_decl_material_free(&material); sh_decl_renderparm_free(&defaults);
    free(path); free(material_bytes); free(parameter_bytes);
    if (reader) { reader->error = NULL; reader->capacity = 0; }
    if (!out && error && capacity && !*error)
        snprintf(error, capacity, "material path parameter is unavailable or invalid: %s/%s",
            name ? name : "", parameter ? parameter : "");
    return out;
}

/* ParseRestOfLine joins token spellings with one space, without quotes; the
 * material constructor then strips the final extension before either lookup.
 * Ordinary String values remain opaque. Only these native ABI selectors have
 * this resource meaning (paired idMaterial::Parse implementations). */
static char *sg_material_request_name(sh_decl_source value, char *error, size_t capacity)
{
    sh_decl_tokens tokens = {0}; char *out = NULL, *canonical = NULL; size_t used = 0;
    if (value.length == SIZE_MAX || !sh_decl_native_lex(value, &tokens, error, capacity)) goto done;
    if (value.length > SIZE_MAX - tokens.count - 1 || !(out = malloc(value.length + tokens.count + 1))) goto done;
    for (size_t i = 0; i < tokens.count; i++) {
        const sh_decl_token *token = tokens.items + i;
        size_t begin = token->begin, end = token->end;
        if (token->kind == SH_DECL_TOKEN_STRING || token->kind == SH_DECL_TOKEN_LITERAL) { begin++; end--; }
        if (used) out[used++] = ' ';
        memcpy(out + used, value.text + begin, end - begin); used += end - begin;
    }
    out[used] = 0;
    for (size_t i = used; i; ) {
        char c = out[--i];
        if (c == '/' || c == '\\') break;
        if (c == '.') { out[i] = 0; break; }
    }
    canonical = sg_name(out);
done:
    free(out); sh_decl_tokens_free(&tokens); return canonical;
}
int sh_package_source_material_requests(sh_package_source_reader *reader,
    sh_decl_source source, int observing,
    int (*visit)(void *, const char *, const char *, const char *), void *context,
    char *error, size_t capacity)
{
    sh_decl_material material = {0}; sh_decl_material_schema schema = {reader, sg_material_schema};
    static const char *const selectors[] = {"transsortmap", "transatlasmap"};
    static const char *const families[] = {"transsortatlas", "image"};
    int ok = 0; char *old_error; size_t old_capacity;
    if (!reader || !visit) return 0;
    old_error = reader->error; old_capacity = reader->capacity;
    reader->error = error; reader->capacity = capacity;
    if (!sh_decl_material_read(source, &schema, &material, error, capacity)) goto done;
    for (size_t p = 0; p < sizeof(selectors) / sizeof(*selectors); p++) {
        const sh_decl_material_write *selected = NULL;
        for (size_t i = 0; i < material.count; i++) {
            const sh_decl_material_write *write = material.writes + i;
            if (write->kind == 12 && write->name.length == strlen(selectors[p]) &&
                !_strnicmp(write->name.text, selectors[p], write->name.length)) selected = write;
        }
        if (!selected) continue;
        char *name = sg_material_request_name(selected->value, error, capacity);
        if (!name) goto done;
        if (!*name) { free(name); continue; } /* Native empty-name default. */
        char *atlas_file = NULL;
        for (size_t f = 0; f < sizeof(families) / sizeof(*families); f++) {
            const char *family = families[f]; char *path = sh_package_cooked_path(family, name);
            unsigned char *body = NULL; size_t length = 0;
            int present = path ? sg_read_path(reader, path, &body, &length) : 0;
            if (!f && present == 1) { atlas_file = path; path = NULL; }
            free(path); free(body);
            if (present < 0) { free(atlas_file); free(name); goto done; }
            /* The atlas carrier is itself a native cooked image. If its owner
             * supplies no separate fallback, the image manager can read that
             * exact carrier by explicit .bimage name. A full atlas then still
             * displays the authored texture through the native fallback. */
            const char *target = !observing && f && present != 1 && atlas_file ? atlas_file : name;
            if (!observing && present != 1 && target == name) continue;
            size_t size = strlen(family) + strlen(name) + 10;
            char *slot = malloc(size);
            if (!slot) { free(atlas_file); free(name); goto done; }
            snprintf(slot, size, "request/%s/%s", family, name);
            int accepted = visit(context, slot, family, target);
            free(slot);
            if (!accepted) { free(atlas_file); free(name); goto done; }
        }
        free(atlas_file); free(name);
    }
    ok = 1;
done:
    reader->error = old_error; reader->capacity = old_capacity;
    sh_decl_material_free(&material);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "material constructor requests could not be captured");
    return ok;
}

typedef struct sg_binding {
    sg_walk *reader;
    sh_package_binding_resolver resolver;
    void *context;
    const char *family, *native_class;
    const sh_decl_node *tree;
    const sh_decl_graph *graph;
    sh_decl_value_type state;
    size_t references, record;
    char *replacement;
    int observe;
} sg_binding;

typedef struct sg_candidate { sg_binding *binding; const char *slot; } sg_candidate;
static int sg_bind_candidate(void *context, const char *family, const char *name)
{
    sg_candidate *candidate = context; sg_binding *binding = candidate->binding;
    const char *replacement = NULL; char *canonical = sg_name(family);
    int ok = canonical && binding->resolver(binding->context, candidate->slot,
        canonical, name, &replacement) == 0;
    free(canonical); return ok;
}

/* A map's event decl argument with no source in scope (sg_event_decl) goes to
 * the map's per-site resolver under its declaration family, as an event clip
 * does (sg_default_use): a new or unchanged use keeps its authored text as the
 * engine's default, a captured supplier binds, and a captured use whose
 * supplier is gone is refused. 1 replaced, 0 unchanged, -1 refused. */
static int sg_bind_event_decl(sg_binding *binding, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    const char *family = NULL, *target = NULL;
    char *canonical = NULL, *copy = NULL;
    int result = -1;
    if (sh_decl_native_registry_class_type(binding->reader->registry, type.name, &family) > 0 && family &&
        (canonical = sg_name(family)) && (copy = malloc(length + 1))) {
        memcpy(copy, name, length); copy[length] = 0;
        binding->references++;
        result = binding->resolver(binding->context, slot, canonical, copy, &target);
        if (result == 1) {
            free(binding->replacement);
            binding->replacement = target && *target ? _strdup(target) : NULL;
            if (!binding->replacement) result = -1;
            else *replacement = binding->replacement;
        }
    }
    free(copy); free(canonical);
    return result;
}

static int sg_bind_typed(void *context, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    sg_binding *binding = context;
    sh_decl_native_reference reference = {0};
    const char *target = NULL;
    if (binding->observe && sh_decl_native_schema_reader(binding->reader->schema, type) == SH_DECL_READER_DECL) {
        if (!name || length == SIZE_MAX || memchr(name, 0, length)) return -1;
        if (!length) return 0;
        char *copy = malloc(length + 1); if (!copy) return -1;
        memcpy(copy, name, length); copy[length] = 0;
        sg_candidate candidate = {binding, slot};
        int ok = sh_decl_native_registry_source_candidates(binding->reader->registry,
            type.name, copy, sg_bind_candidate, &candidate);
        free(copy); binding->references++;
        if (!ok) sg_gap(binding->reader, slot, type.name, "native candidate names could not be observed");
        return ok ? 0 : -1;
    }
    binding->reader->bound_absent = 0;
    int result = sh_decl_native_reference_read(binding->reader->schema, binding->reader->registry,
        type, name, length, &reference);
    /* Authored declaration text keeps an engine-defaulted absent declaration
     * unchanged, with no binding edge, like its stock counterpart. */
    int event_decl = result == 2 && !sg_native_default(binding->reader) && sg_event_decl(type) &&
        !binding->reader->bound_absent && !sg_composition_token(name, length);
    if (event_decl) result = sg_bind_event_decl(binding, slot, type, name, length, replacement);
    else if (result == 2) result = sg_native_default(binding->reader) && !binding->reader->bound_absent &&
        !sg_composition_token(name, length) ? 0 : -1;
    if (result == 1 && reference.make_default) {
        int defaulted = sg_default_use(binding->reader, reference.resource.type, reference.resource.name, 0);
        if (defaulted) result = defaulted > 0 ? 0 : -1;
    }
    if (result == 1 && !event_decl) {
        /* Native registry labels retain mixed case (entityDef, for example).
         * Source-unit identities use the same canonical family as the source
         * graph queue and catalog; do not create a second manager identity. */
        char *family = sg_name(reference.resource.type);
        binding->references++;
        result = family ? binding->resolver(binding->context, slot, family, reference.resource.name, &target) : -1;
        free(family);
        if (result == 1) {
            free(binding->replacement);
            binding->replacement = sh_decl_native_reference_bind(&reference, target);
            if (!binding->replacement) result = -1;
            else *replacement = binding->replacement;
        }
    }
    if (result < 0) {
        char reason[384];
        /* A name with no native identity is reported as authored, so a refused
         * map names what it lacks. */
        if (reference.resource.name)
            snprintf(reason, sizeof(reason), "native source binding could not be resolved: %s %s",
                reference.resource.type ? reference.resource.type : "", reference.resource.name);
        else if (name && length && length != SIZE_MAX && !memchr(name, 0, length))
            snprintf(reason, sizeof(reason), "native source binding could not be resolved for \"%.*s\"",
                (int)(length > 256 ? 256 : length), name);
        else snprintf(reason, sizeof(reason), "native source binding could not be resolved");
        sg_gap(binding->reader, slot, type.name, reason);
    }
    sh_decl_native_reference_clear(&reference); return result;
}

/* Consumers whose native loader asks the target's manager with makeDefault
 * set. A target that neither this scope nor SnapMap ships, including one only
 * the campaign archive carries, becomes an engine object named by the
 * reference: the consumer keeps the authored name with no binding edge or
 * install requirement, like its stock counterpart. 1 defaults.
 * - MD6 mesh surface materials: mesh loader Vulkan 0x14149b240 / OpenGL
 *   0x14148ea40 and the model-level list Vulkan 0x14149c950 call DeclFind.
 * - Animation clips named by md6def and animweb aliases (below).
 * - Images named by material parameters and renderparm defaults: Material
 *   Parse (Vulkan 0x140d76bb0 / OpenGL 0x140d76df0) reads its parameters with
 *   the parm block reader (0x140db3710 / 0x140da9930), and renderparm parsing
 *   uses the same value reader; texture kinds reach 0x140d7b540 / 0x140d7b780,
 *   which Materialize the image by name with makeDefault (0x141801380 /
 *   0x1417f3cc0). Engine-created targets such as _viewDepth are found there. */
static int sg_native_default_target(sg_binding *binding, const char *family, const char *name)
{
    unsigned char *body = NULL; size_t length = 0;
    char *path = NULL;
    if (!binding->family || sg_composition_token(name, strlen(name))) return 0;
    if (!strcmp(binding->family, "basemodel") && !strcmp(family, "material")) path = sg_path("material", name);
    else if (!strcmp(binding->family, "md6def") && strcmp(family, "md6def") && strcmp(family, "basemodel") &&
        strcmp(family, "anim") && strcmp(family, "skeleton") && strcmp(family, "image") && strcmp(family, "sound")) {
        /* Declaration-typed animation event arguments: ReadEventCallInfo
         * (Vulkan 0x1409caa70 / OpenGL 0x1409ca230) finds the argument's type
         * manager and calls DeclFind with makeDefault. A sound keeps its
         * name-derived event and stays strict, as in declaration text. */
        path = sg_path(family, name);
    }
    else if ((!strcmp(binding->family, "md6def") || !strcmp(binding->family, "animweb")) &&
        !strcmp(family, "anim")) {
        /* Alias clips load through the anim list with makeDefault and only warn
         * on failure (md6def aliases Vulkan 0x14147c4f0 / OpenGL 0x14146fe10,
         * lazy alias loads 0x14147c460 / 0x14146fd80); a registered row with
         * no data reports "No data" and defaults (Vulkan 0x1414865e0). Stock
         * animwebs name such clips and load in shipping SnapMap. */
        path = sh_package_cooked_path("anim", name);
        if (!path) return 0;
    } else if (!strcmp(binding->family, "anim") && !strcmp(family, "skeleton")) {
        /* MD6Anim load (Vulkan 0x1414865e0 / OpenGL 0x141479f10) checks its
         * skeleton through the skeleton registry; one that does not exist
         * fails the remap check and reports "Old skeleton" through the load
         * warning (0x141800450 / 0x1417f2da0), leaving the clip defaulted. */
        static const char prefix[] = "generated/skeleton/";
        size_t n = strlen(name);
        if (n <= 7 || strcmp(name + n - 7, ".md6skl")) return 0;
        /* Catalogued skeletons store name.md6skl as generated/skeleton/name.bmd6skl. */
        if (!(path = malloc(sizeof(prefix) + n + 2))) { binding->reader->failed = 1; return -1; }
        snprintf(path, sizeof(prefix) + n + 2, "%s%.*s.bmd6skl", prefix, (int)(n - 7), name);
    } else if ((!strcmp(binding->family, "material") || !strcmp(binding->family, "renderparm") ||
        !strcmp(binding->family, "env")) && !strcmp(family, "image")) {
        /* Engine-named images have no physical path. Materialize returns the
         * renderer's object of that name (render targets, intrinsics) or
         * creates a defaulted one, so the name alone is the identity. */
        path = sh_package_cooked_path("image", name);
        if (!path) return name[0] == '_';
    } else if ((!strcmp(binding->family, "material") || !strcmp(binding->family, "renderparm") ||
        !strcmp(binding->family, "env")) && !strcmp(family, "renderprog")) {
        /* A parm block finds a program without makeDefault (Vulkan 0x140db3710 /
         * OpenGL 0x140da9930): an unknown one reports "'%s' is an unknown
         * renderProg." and ends the block, and idMaterial::Parse (0x140d76bb0 /
         * 0x140d76df0) warns and keeps the ill-formed material. A renderparm's
         * own value asks with makeDefault (0x140d7bae0 / 0x140d7bd20). A program
         * that neither this scope nor SnapMap has gives that same result in every
         * scope, as in stock (models/characters/eyes/eyelip names eyeLip). */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = sh_resource_catalog_find(binding->reader->catalog, "renderprog", name, &rows);
        for (size_t i = 0; i < count; i++) if (rows[i]->archive < 2) return 0;
        path = sg_path("renderprog", name);
    } else return 0;
    int status = path ? sg_read_path(binding->reader, path, &body, &length) : -1;
    free(body); free(path);
    if (status < 0) { binding->reader->failed = 1; return -1; }
    return status == 0 || status == 2;
}

static int sg_bind_native(void *context, const char *family, const char *name, const char **replacement)
{
    sg_binding *binding = context;
    char slot[64];
    if (binding->record == SIZE_MAX) return -1;
    snprintf(slot, sizeof(slot), "native/%zu", binding->record++);
    binding->references++;
    /* Native custom grammars retain source spelling. Manager identities use
     * canonical names, just like reflected references and catalog inputs. Keep
     * the authored token intact unless the resolver actually replaces it. */
    char *normalized_family = sg_name(family), *normalized_name = sg_name(name);
    int result = -1;
    if (normalized_family && normalized_name) {
        int defaulted = sg_native_default_target(binding, normalized_family, normalized_name);
        if (defaulted > 0) { *replacement = NULL; result = 0; }
        else if (!defaulted) result = binding->resolver(binding->context, slot, normalized_family,
            normalized_name, replacement);
    }
    free(normalized_family); free(normalized_name); return result;
}

static int sg_bind_envelope(void *context, const sh_decl_dependency_schema *view,
    sh_decl_dependency_result *result)
{
    sg_binding *binding = context;
    memset(result, 0, sizeof(*result));
    if (binding->graph) return sh_decl_graph_dependencies_with(binding->graph, binding->family,
        binding->reader->schema, view, result);
    if (!strcmp(binding->family, "entitydef"))
        return sh_decl_entity_dependencies(binding->tree, binding->state, view, result);
    return sh_decl_envelope_dependencies(binding->tree, binding->state,
        (sh_decl_value_type){binding->native_class, "*"}, view, result);
}

typedef struct sg_shader_token { size_t begin, end; char quote; } sg_shader_token;
typedef struct sg_shader_cursor { sh_decl_source source; size_t at; } sg_shader_cursor;
/* This only locates outer commands and opaque balanced source blocks. It does
 * not expand shader preprocessing or interpret HLSL, state, flags or defines. */
static int sg_shader_token_next(sg_shader_cursor *cursor, sg_shader_token *token)
{
    const char *s = cursor->source.text; size_t n = cursor->source.length, p = cursor->at;
    for (;;) {
        while (p < n && (unsigned char)s[p] <= 32) p++;
        if (p + 1 < n && s[p] == '/' && s[p + 1] == '/') {
            p += 2; while (p < n && s[p] != '\n') p++; continue;
        }
        if (p + 1 < n && s[p] == '/' && s[p + 1] == '*') {
            p += 2; while (p + 1 < n && (s[p] != '*' || s[p + 1] != '/')) p++;
            if (p + 1 >= n) return -1;
            p += 2; continue;
        }
        break;
    }
    if (p == n) { cursor->at = p; return 0; }
    token->begin = p; token->quote = 0;
    if (s[p] == '"' || s[p] == '\'') {
        token->quote = s[p++];
        while (p < n && s[p] != token->quote) p++;
        if (p == n) return -1;
        p++;
    } else if (strchr("{};", s[p])) p++;
    else {
        do { p++; } while (p < n && (unsigned char)s[p] > 32 && !strchr("{};\"'", s[p]) &&
            !(s[p] == '/' && p + 1 < n && (s[p + 1] == '/' || s[p + 1] == '*')));
    }
    token->end = cursor->at = p; return 1;
}
static int sg_shader_token_is(sh_decl_source source, sg_shader_token token, const char *name)
{ return !token.quote && token.end - token.begin == strlen(name) && !memcmp(source.text + token.begin, name, strlen(name)); }
typedef struct sg_shader_sites {
    sh_resource_binding_site visit;
    void *context;
    size_t base;
} sg_shader_sites;
static int sg_shader_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    sg_shader_sites *sites = context;
    return offset != SIZE_MAX && sites->visit(sites->context, family, name, sites->base + offset, length);
}
static int sg_shader_source_sites(void *context, const unsigned char *body, size_t length,
    sh_resource_binding_site visit, void *visit_context, char *error, size_t capacity)
{
    sg_walk *reader = context; sh_decl_source source = {(const char *)body, length};
    sg_shader_cursor cursor = {source, 0}; sg_shader_token token;
    sh_decl_material_schema schema = {reader, sg_material_schema};
    int inherited = 0, commands = 0;
    if (memchr(body, 0, length) || sg_shader_token_next(&cursor, &token) != 1 ||
        !sg_shader_token_is(source, token, "{")) goto gap;
    while (sg_shader_token_next(&cursor, &token) == 1) {
        if (sg_shader_token_is(source, token, "}"))
            return sg_shader_token_next(&cursor, &token) == 0;
        if (sg_shader_token_is(source, token, "inherit")) {
            if (commands || inherited || sg_shader_token_next(&cursor, &token) != 1) goto gap;
            size_t begin = token.begin + (token.quote != 0), end = token.end - (token.quote != 0);
            if (end <= begin) goto gap;
            char *name = malloc(end - begin + 1); if (!name) goto gap;
            memcpy(name, body + begin, end - begin); name[end - begin] = 0;
            int ok = visit(visit_context, "renderprog", name, begin, end - begin); free(name);
            if (!ok) return 0;
            inherited = 1; continue;
        }
        static const char *const blocks[] = {"permutations", "parms", "state", "flags", "inout",
            "computelayout", "renderoutput", "buffer", "compileroptions", "wavelimits",
            "hlsl_header", "hlsl_vp_header", "hlsl_fp_header", "hlsl_cp_header", "hlsl_vp", "hlsl_fp", "hlsl_cp"};
        size_t kind = 0;
        while (kind < sizeof(blocks) / sizeof(*blocks) && !sg_shader_token_is(source, token, blocks[kind])) kind++;
        if (kind == sizeof(blocks) / sizeof(*blocks) || sg_shader_token_next(&cursor, &token) != 1 ||
            !sg_shader_token_is(source, token, "{")) goto gap;
        size_t begin = token.begin, depth = 1;
        while (depth) {
            if (sg_shader_token_next(&cursor, &token) != 1) goto gap;
            if (sg_shader_token_is(source, token, "{")) depth++;
            if (sg_shader_token_is(source, token, "}")) depth--;
        }
        if (kind == 1) {
            sg_shader_sites sites = {visit, visit_context, begin};
            if (!sh_decl_native_reference_sites(SH_BINDING_MATERIAL,
                (sh_decl_source){source.text + begin, token.end - begin}, &schema,
                sg_shader_site, &sites, error, capacity)) return 0;
        }
        commands++;
    }
gap:
    if (error && capacity && !*error) snprintf(error, capacity,
        "shader source relocation requires verified outer commands and unexpanded parameter blocks");
    return 0;
}
typedef struct sg_shader_binding {
    sg_walk *reader;
    const sh_package_shader_program *program;
    char **replacements;
    unsigned char *used;
    int parent_changed;
} sg_shader_binding;
static int sg_shader_source_reference(void *context, const char *family, const char *name, const char **replacement)
{
    sg_shader_binding *binding = context; char *canonical = sg_name(name); int result = 0;
    if (!canonical) return -1;
    for (size_t i = 0; i < binding->program->reference_count; i++) {
        const sh_package_shader_reference *ref = binding->program->references + i;
        if (!binding->replacements[i] || !ref->family || strcmp(ref->family, family) || _stricmp(ref->name, canonical)) continue;
        if (result && strcmp(*replacement, binding->replacements[i])) { result = -1; break; }
        *replacement = binding->replacements[i]; binding->used[i] = 1; result = 1;
        if (ref->offset == 12 && !strcmp(family, "renderprog")) binding->parent_changed = 1;
    }
    free(canonical); return result;
}
static int sg_shader_binary_reference(void *context, const sh_package_shader_reference *reference,
    const char **replacement)
{
    sg_shader_binding *binding = context;
    for (size_t i = 0; i < binding->program->reference_count; i++) {
        if (binding->program->references[i].offset != reference->offset) continue;
        *replacement = binding->replacements[i]; return *replacement != NULL;
    }
    return -1;
}
unsigned char *sh_package_source_shader_bind(sh_package_source_reader *reader,
    const char *name, const char *path, const unsigned char *body, size_t length,
    int (*resolver)(void *, const char *, const char *, const char *, const char **),
    void *context, size_t *output_length, char *error, size_t capacity)
{
    unsigned char *declaration = NULL, *binary = NULL, *linked_decl = NULL, *out = NULL;
    size_t declaration_length = 0, binary_length = 0, linked_length = 0, changed = 0;
    sh_package_shader_program program = {0}, empty = {0}; sg_shader_paths paths = {0};
    sg_shader_binding binding = {reader, &program}; sh_package_shader_schema schema = {reader, sg_shader_parameter};
    char fallback[1024]; char *old_error = NULL; size_t old_capacity = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0; if (output_length) *output_length = 0;
    if (!reader || !name || !path || !body || !resolver || !output_length || length == SIZE_MAX) goto done;
    old_error = reader->error; old_capacity = reader->capacity; reader->error = error; reader->capacity = capacity;
    if (sh_package_source_shader_open(reader, name, &declaration, &declaration_length,
        &binary, &binary_length, &program, error, capacity) != SH_PACKAGE_SOURCE_SHADER_READY ||
        sh_package_shader_paths(reader->shader.renderer, name, name, &empty,
        sg_shader_first_paths, &paths, error, capacity) != SH_PACKAGE_SHADER_OK) goto done;
    int is_declaration = !strcmp(path, paths.declaration), is_binary = !strcmp(path, paths.binary);
    if (!is_declaration && !is_binary) {
        sg_shader_walk shader = {reader, binary, &program, path, 0};
        if (sh_package_shader_paths(reader->shader.renderer, name, name, &program,
            sg_shader_path, &shader, error, capacity) != SH_PACKAGE_SHADER_OK || !shader.matched) goto done;
        unsigned char *original = NULL; size_t original_length = 0;
        int exact = sg_read_path(reader, path, &original, &original_length) > 0 &&
            length == original_length && !memcmp(body, original, length);
        free(original); if (!exact) goto done;
        out = malloc(length + 1);
        if (out) { memcpy(out, body, length); out[length] = 0; *output_length = length; }
        goto done;
    }
    const unsigned char *expected = is_declaration ? declaration : binary;
    size_t expected_length = is_declaration ? declaration_length : binary_length;
    if (length != expected_length || memcmp(body, expected, length)) {
        sg_shader_error(reader, "shader member differs from its captured source unit", path); goto done;
    }
    binding.replacements = calloc(program.reference_count ? program.reference_count : 1, sizeof(*binding.replacements));
    binding.used = calloc(program.reference_count ? program.reference_count : 1, 1);
    if (!binding.replacements || !binding.used) goto done;
    for (size_t i = 0; i < program.reference_count; i++) {
        const sh_package_shader_reference *ref = program.references + i; const char *replacement = NULL;
        if (ref->kind == SH_PACKAGE_SHADER_INCLUDE) continue;
        char slot[64], *canonical = sg_name(ref->name);
        snprintf(slot, sizeof(slot), "shader/record/%zu", ref->offset);
        int action = canonical ? resolver(context, slot, ref->family, canonical, &replacement) : -1;
        if (action < 0 || (action && (!replacement || !*replacement))) { free(canonical); goto done; }
        int different = action && strcmp(canonical, replacement);
        free(canonical);
        if (!different) continue;
        if (ref->kind == SH_PACKAGE_SHADER_PARAMETER || ref->kind == SH_PACKAGE_SHADER_FLAG) {
            sg_shader_error(reader, "shader reference cannot change its fixed native ABI name", ref->name); goto done;
        }
        binding.replacements[i] = _strdup(replacement);
        if (!binding.replacements[i]) goto done;
        changed++;
    }
    if (changed) {
        size_t source_changes = 0;
        linked_decl = sh_resource_rebind_with(sg_shader_source_sites, reader, SH_BINDING_NATIVE_NAME,
            declaration, declaration_length, sg_shader_source_reference, &binding,
            &linked_length, &source_changes, error, capacity);
        if (!linked_decl) goto done;
        for (size_t i = 0; i < program.reference_count; i++) if (binding.replacements[i] && !binding.used[i] && !binding.parent_changed) {
            sg_shader_error(reader, "changed cooked shader reference has no verified declaration origin", program.references[i].name); goto done;
        }
    } else {
        linked_decl = malloc(declaration_length + 1);
        if (!linked_decl) goto done;
        memcpy(linked_decl, declaration, declaration_length); linked_decl[declaration_length] = 0; linked_length = declaration_length;
    }
    if (is_declaration) { out = linked_decl; linked_decl = NULL; *output_length = linked_length; }
    else if (sh_package_shader_rebind(binary, binary_length, &schema,
        (sh_package_shader_source){declaration, declaration_length},
        (sh_package_shader_source){linked_decl, linked_length}, sg_shader_binary_reference, &binding,
        &out, output_length, &changed, error, capacity) != SH_PACKAGE_SHADER_OK) { free(out); out = NULL; }
done:
    if (!out && !*error) snprintf(error, capacity, "shader source binding could not be completed: %s", path ? path : "");
    if (binding.replacements) for (size_t i = 0; i < program.reference_count; i++) free(binding.replacements[i]);
    free(binding.replacements); free(binding.used); free(declaration); free(binary); free(linked_decl);
    sh_package_shader_program_clear(&program);
    if (reader) { reader->error = old_error; reader->capacity = old_capacity; }
    return out;
}

static unsigned char *sg_bind_source(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity, int observe)
{
    sh_decl_source source = {(const char *)body, length};
    sh_decl_node *tree = NULL;
    sh_decl_graph *graph = NULL;
    sh_decl_rebind_result rebound = {0};
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives};
    sh_decl_native_binding_format native_format;
    sg_binding binding = {0};
    unsigned char *out = NULL;
    char *class_name = NULL, fallback[1024];
    size_t changed = 0, out_length = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    if (reader) reader->preparing = 0;
    error[0] = 0;
    if (output_length) *output_length = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (reader) {
        reader->failed = 0;
        memset(&reader->report, 0, sizeof(reader->report));
    }
    if (!reader || !family || !name || !body || !resolver || length == SIZE_MAX) goto done;
    reader->error = error; reader->capacity = capacity; reader->failed = 0;
    reader->current_type = family; reader->current_name = name;
    sg_lists_free(reader);
    binding.reader = reader; binding.resolver = resolver; binding.context = context; binding.family = family;
    binding.observe = observe;
    if (!strcmp(family, "renderprog")) {
        sg_shader_paths paths = {0}; sh_package_shader_program empty = {0};
        if (reader->shader_configured && sh_package_shader_paths(reader->shader.renderer, name, name,
            &empty, sg_shader_first_paths, &paths, error, capacity) == SH_PACKAGE_SHADER_OK)
            out = sh_package_source_shader_bind(reader, name, paths.declaration, body, length,
                resolver, context, &out_length, error, capacity);
    } else if (!strcmp(family, "renderprogflag")) {
        unsigned char *captured = NULL; size_t captured_length = 0;
        char *path = sg_path(family, name);
        if (path && sg_shader_flag(reader, name) && sg_read_path(reader, path, &captured, &captured_length) > 0 &&
            captured_length == length && !memcmp(captured, body, length)) {
            out = malloc(length + 1);
            if (out) { memcpy(out, body, length); out[length] = 0; out_length = length; }
        }
        free(path); free(captured);
    } else if (sh_decl_native_binding_family(family, &native_format)) {
        sh_decl_material_schema material_schema = {reader, sg_material_schema};
        out = (unsigned char *)sh_decl_native_rebind(native_format, source, &material_schema,
            sg_bind_native, &binding, &out_length, &changed, error, capacity);
    } else if (!strcmp(family, "model") || !strcmp(family, "basemodel") || !strcmp(family, "anim") ||
        !strcmp(family, "discreteanimation") || !strcmp(family, "staticparticlemodel")) {
        sh_resource_rebind_format format = !strcmp(family, "model") ? SH_REBIND_STATIC_MODEL :
            !strcmp(family, "basemodel") ? SH_REBIND_MD6_MESH :
            !strcmp(family, "discreteanimation") ? SH_REBIND_DISCRETE_MODEL :
            !strcmp(family, "staticparticlemodel") ? SH_REBIND_STATIC_PARTICLE : SH_REBIND_MD6_ANIMATION;
        out = sh_resource_rebind(format, body, length, sg_bind_native, &binding,
            &out_length, &changed, error, capacity);
    } else if (sh_package_source_leaf_family(family)) {
        out = malloc(length + 1);
        if (out) { memcpy(out, body, length); out[length] = 0; out_length = length; }
    } else {
        int is_graph = 0;
        if (!strcmp(family, "entitydef")) {
            tree = sh_decl_tree_parse_ordered(source, error, capacity);
            if (!tree) { if (result) result->malformed = 1; goto done; }
            if (!(class_name = sh_decl_entity_tree_class(tree, &classes, error, capacity))) goto done;
            binding.state = (sh_decl_value_type){class_name, ""};
        } else {
            if (sh_decl_native_schema_decl_type(reader->schema, family, &binding.state) != 1 ||
                sh_decl_native_registry_type_class(reader->registry, family, &binding.native_class) != 1 ||
                (is_graph = sh_decl_native_schema_graph_type(reader->schema, binding.state)) < 0) {
                snprintf(error, capacity, "%s source requires its registered reader adapter", family); goto done;
            }
            if (is_graph) {
                graph = sh_decl_graph_open(source, error, capacity);
                if (!graph) { if (result) result->malformed = 1; goto done; }
            } else {
                tree = sg_open_body(family) ? sh_decl_tree_parse_reflected_body(source, 1, error, capacity) :
                    sh_decl_tree_parse_ordered(source, error, capacity);
                if (!tree) { if (result) result->malformed = 1; goto done; }
            }
        }
        binding.tree = tree; binding.graph = graph;
        out = (unsigned char *)sh_decl_rebind_parsed(source, graph ? sh_decl_graph_syntax(graph) : tree,
            sh_decl_native_schema_view(reader->schema), sg_bind_envelope, &binding,
            sg_bind_typed, &binding, &out_length, &rebound);
        changed = rebound.changed;
    }
done:
    if (reader && reader->failed) { free(out); out = NULL; }
    if (!out && !error[0]) snprintf(error, capacity, "%s source bindings could not be completed%s%s",
        family ? family : "resource", reader && reader->report.first_gap[0] ? ": " : "",
        reader ? reader->report.first_gap : "");
    if (result) {
        result->references = binding.references; result->changed = out ? changed : 0;
        if (reader) result->inspection = reader->report;
    }
    if (output_length && out) *output_length = out_length;
    free(binding.replacement); free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
    if (reader) { reader->error = NULL; reader->capacity = 0; }
    return out;
}

unsigned char *sh_package_source_bind(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    return sg_bind_source(reader, family, name, body, length, resolver, context,
        output_length, result, error, capacity, 0);
}

typedef struct sg_observer { sh_package_binding_resolver visit; void *context; } sg_observer;
static int sg_observe_only(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    sg_observer *observer = context;
    int status = observer->visit(observer->context, slot, family, name, replacement);
    *replacement = NULL; return status == 0 ? 0 : -1;
}
unsigned char *sh_package_source_observe(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver observer, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    sg_observer walk = {observer, context};
    return sg_bind_source(reader, family, name, body, length, observer ? sg_observe_only : NULL,
        &walk, output_length, result, error, capacity, 1);
}

typedef struct sg_string_binding {
    sg_binding envelope;
    const sh_decl_dependency_schema *native;
    sh_package_string_site_resolver resolver;
    void *context;
} sg_string_binding;

static int sg_string_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    sg_string_binding *binding = context;
    if (!binding->native->describe(binding->native->context, type, shape)) return 0;
    /* This is a separate observation projection, not a new resource edge.
     * Require the registered reflected value type; pointers and collection
     * wrappers retain the native shape and expose only their typed elements. */
    if (type.name && !strcmp(type.name, "idStrId") && (!type.ops || !*type.ops)) {
        if (shape->kind != SH_DECL_VALUE_IGNORE ||
            sh_decl_native_schema_class_derives(binding->envelope.reader->schema,
                "idStrId", "idStrId") != 1) return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE;
    } else if (shape->kind == SH_DECL_VALUE_REFERENCE) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int sg_string_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    sg_string_binding *binding = context;
    return binding->native->field(binding->native->context, type, key, out);
}
static int sg_string_dynamic(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    sg_string_binding *binding = context;
    return binding->native->dynamic_type &&
        binding->native->dynamic_type(binding->native->context, base, name, length, selected);
}
static void sg_string_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    sg_string_binding *binding = context;
    if (binding->native->gap) binding->native->gap(binding->native->context, path, type, reason);
}
static int sg_string_site(void *context, const char *slot, sh_decl_value_type type,
    const char *key, size_t length, const char **replacement)
{
    sg_string_binding *binding = context;
    int status;
    /* Some envelope readers directly emit inheritance resource references. */
    if (!type.name || strcmp(type.name, "idStrId") || (type.ops && *type.ops)) return 0;
    if (binding->envelope.references == SIZE_MAX) return -1;
    binding->envelope.references++;
    status = binding->resolver(binding->context, slot, key, length, replacement);
    if (status < 0) sg_gap(binding->envelope.reader, slot, type.name, "localized string binding could not be resolved");
    return status;
}

unsigned char *sh_package_source_bind_strings(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_string_site_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    sh_decl_source source = {(const char *)body, length};
    sh_decl_node *tree = NULL;
    sh_decl_graph *graph = NULL;
    sh_decl_rebind_result rebound = {0};
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives};
    sg_string_binding binding = {0};
    sh_decl_dependency_schema localized = {&binding, sg_string_describe, sg_string_field,
        NULL, sg_string_gap, sg_string_dynamic, NULL};
    unsigned char *out = NULL;
    char *class_name = NULL, fallback[1024];
    size_t out_length = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (output_length) *output_length = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!reader || !family || !name || !body || !resolver || length == SIZE_MAX) goto done;
    reader->failed = 0; memset(&reader->report, 0, sizeof(reader->report));
    reader->error = error; reader->capacity = capacity;
    reader->current_type = family; reader->current_name = name;
    sg_lists_free(reader);
    binding.envelope.reader = reader; binding.envelope.family = family;
    binding.native = sh_decl_native_schema_view(reader->schema);
    binding.resolver = resolver; binding.context = context;
    if (!strcmp(family, "entitydef")) {
        tree = sh_decl_tree_parse_ordered(source, error, capacity);
        if (!tree || !(class_name = sh_decl_entity_tree_class(tree, &classes, error, capacity))) goto done;
        binding.envelope.state = (sh_decl_value_type){class_name, ""};
    } else {
        int is_graph;
        if (sh_decl_native_schema_decl_type(reader->schema, family, &binding.envelope.state) != 1 ||
            sh_decl_native_registry_type_class(reader->registry, family, &binding.envelope.native_class) != 1 ||
            (is_graph = sh_decl_native_schema_graph_type(reader->schema, binding.envelope.state)) < 0) {
            snprintf(error, capacity, "%s localization requires its registered reflected reader", family); goto done;
        }
        if (is_graph) {
            graph = sh_decl_graph_open(source, error, capacity);
            if (!graph) goto done;
        } else {
            tree = sg_open_body(family) ? sh_decl_tree_parse_reflected_body(source, 1, error, capacity) :
                sh_decl_tree_parse_ordered(source, error, capacity);
            if (!tree) goto done;
        }
    }
    binding.envelope.tree = tree; binding.envelope.graph = graph;
    out = (unsigned char *)sh_decl_rebind_parsed(source, graph ? sh_decl_graph_syntax(graph) : tree,
        &localized, sg_bind_envelope, &binding.envelope, sg_string_site, &binding, &out_length, &rebound);
done:
    if (reader && reader->failed) { free(out); out = NULL; }
    if (!out && !error[0]) snprintf(error, capacity, "%s localized string sites could not be completed%s%s",
        family ? family : "resource", reader && reader->report.first_gap[0] ? ": " : "",
        reader ? reader->report.first_gap : "");
    if (result) {
        result->references = binding.envelope.references; result->changed = out ? rebound.changed : 0;
        if (reader) result->inspection = reader->report;
    }
    if (out && output_length) *output_length = out_length;
    free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
    if (reader) { reader->error = NULL; reader->capacity = 0; }
    return out;
}

/* Registration-key sites: the same projection as localized strings, for the
 * reflected spawn-type enum instead of idStrId. */
typedef struct sg_key_binding {
    sg_binding envelope;
    const sh_decl_dependency_schema *native;
    sh_package_key_site_resolver resolver;
    void *context;
} sg_key_binding;
static int sg_key_type(sh_decl_value_type type)
{ return type.name && !strcmp(type.name, SH_PACKAGE_KEY_TYPE) && (!type.ops || !*type.ops); }
static int sg_key_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    sg_key_binding *binding = context;
    if (!binding->native->describe(binding->native->context, type, shape)) return 0;
    if (sg_key_type(type)) {
        if (shape->kind != SH_DECL_VALUE_IGNORE) return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE;
    } else if (shape->kind == SH_DECL_VALUE_REFERENCE) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int sg_key_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    sg_key_binding *binding = context;
    return binding->native->field(binding->native->context, type, key, out);
}
static int sg_key_dynamic(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    sg_key_binding *binding = context;
    return binding->native->dynamic_type &&
        binding->native->dynamic_type(binding->native->context, base, name, length, selected);
}
static void sg_key_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    sg_key_binding *binding = context;
    if (binding->native->gap) binding->native->gap(binding->native->context, path, type, reason);
}
static int sg_key_site(void *context, const char *slot, sh_decl_value_type type,
    const char *value, size_t length, const char **replacement)
{
    sg_key_binding *binding = context;
    int status;
    if (!sg_key_type(type)) return 0;
    if (binding->envelope.references == SIZE_MAX) return -1;
    binding->envelope.references++;
    status = binding->resolver(binding->context, slot, value, length, replacement);
    if (status < 0) sg_gap(binding->envelope.reader, slot, type.name, "registration key site could not be resolved");
    return status;
}

int sh_package_source_key_space(sh_package_source_reader *reader, sh_package_key_space *space)
{ return reader ? sh_package_key_space_read(reader->schema, space) : -1; }

int sh_package_source_entity_fields(sh_package_source_reader *reader, const char *name,
    const unsigned char *body, size_t length, const char *const *fields, size_t field_count,
    unsigned char **output, size_t *output_length, char *error, size_t capacity)
{
    sh_decl_node *tree = NULL, *effective = NULL;
    char *class_name = NULL, fallback[1024];
    int status = -1, candidate = 0, changed = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (!reader || !name || !body || !fields || !output || !output_length) goto done;
    reader->failed = 0; memset(&reader->report, 0, sizeof(reader->report));
    reader->error = error; reader->capacity = capacity;
    reader->current_type = "entitydef"; reader->current_name = name; sg_lists_free(reader);
    tree = sh_decl_tree_parse((sh_decl_source){(const char *)body, length}, error, capacity);
    if (!tree) goto done;
    if (!sh_decl_tree_member(tree, "inherit")) { status = 0; goto done; }
    sh_decl_entity_class_source parents = {reader, sg_read_parent, sg_derives};
    class_name = sh_decl_entity_tree_class(tree, &parents, error, capacity);
    if (!class_name) goto done;
    const sh_decl_dependency_schema *schema = sh_decl_native_schema_view(reader->schema);
    for (size_t i = 0; i < field_count; i++) {
        sh_decl_value_type type;
        int found = schema->field(schema->context, (sh_decl_value_type){class_name, ""}, fields[i], &type);
        if (found < 0) goto done;
        candidate |= found;
    }
    if (!candidate) { status = 0; goto done; }
    effective = sh_decl_entity_expanded_state(tree, &parents, error, capacity);
    if (!effective) goto done;
    sh_decl_node *from = (sh_decl_node *)sh_decl_tree_member(effective, "edit");
    sh_decl_node *into = (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    if (!from) { status = 0; goto done; }
    if (!into) {
        into = calloc(1, sizeof(*into));
        if (into) into->key = _strdup("edit");
        if (!into || !into->key) { sh_decl_tree_free(into); goto done; }
        into->compound = into->assignment = 1; into->next = tree->children; tree->children = into;
    }
    for (size_t i = 0; i < field_count; i++) {
        sh_decl_node **take = &from->children, **put = &into->children;
        while (*take && strcmp((*take)->key, fields[i])) take = &(*take)->next;
        if (!*take) continue;
        while (*put && strcmp((*put)->key, fields[i])) put = &(*put)->next;
        sh_decl_node *node = *take, *old = *put;
        *take = node->next; node->next = old ? old->next : NULL; *put = node;
        if (old) { old->next = NULL; sh_decl_tree_free(old); }
        changed = 1;
    }
    if (!changed) { status = 0; goto done; }
    *output = (unsigned char *)sh_decl_tree_write(tree, output_length);
    if (*output) status = 1;
done:
    if (reader && reader->failed) status = -1;
    if (status < 0) {
        if (output) { free(*output); *output = NULL; }
        if (output_length) *output_length = 0;
        if (*error && reader && !reader->failed && !strstr(error, "allocation")) status = -2;
        if (!*error) snprintf(error, capacity, "cannot prepare inherited entity fields");
    }
    free(class_name); sh_decl_tree_free(tree); sh_decl_tree_free(effective); return status;
}

int sh_package_source_bind_keys(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_key_site_resolver resolver, void *context, unsigned char **output,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    int status = -1;
    sh_decl_source source = {(const char *)body, length};
    sh_decl_node *tree = NULL;
    sh_decl_graph *graph = NULL;
    sh_decl_rebind_result rebound = {0};
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives};
    sg_key_binding binding = {0};
    sh_decl_dependency_schema keyed = {&binding, sg_key_describe, sg_key_field,
        NULL, sg_key_gap, sg_key_dynamic, NULL};
    unsigned char *out = NULL;
    char *class_name = NULL, fallback[1024];
    size_t out_length = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!reader || !family || !name || !body || !resolver || !output || length == SIZE_MAX) goto done;
    reader->failed = 0; memset(&reader->report, 0, sizeof(reader->report));
    reader->error = error; reader->capacity = capacity;
    reader->current_type = family; reader->current_name = name;
    sg_lists_free(reader);
    binding.envelope.reader = reader; binding.envelope.family = family;
    binding.native = sh_decl_native_schema_view(reader->schema);
    binding.resolver = resolver; binding.context = context;
    if (!strcmp(family, "entitydef")) {
        tree = sh_decl_tree_parse_ordered(source, error, capacity);
        if (!tree || !(class_name = sh_decl_entity_tree_class(tree, &classes, error, capacity))) goto done;
        binding.envelope.state = (sh_decl_value_type){class_name, ""};
    } else {
        int is_graph, reflected = sh_decl_native_schema_decl_type(reader->schema, family, &binding.envelope.state);
        /* A family with no reflected state reader has no typed key site. */
        if (!reflected) { status = 0; goto done; }
        if (reflected != 1 ||
            sh_decl_native_registry_type_class(reader->registry, family, &binding.envelope.native_class) != 1 ||
            (is_graph = sh_decl_native_schema_graph_type(reader->schema, binding.envelope.state)) < 0) {
            snprintf(error, capacity, "%s registration keys require its registered reflected reader", family); goto done;
        }
        if (is_graph) {
            graph = sh_decl_graph_open(source, error, capacity);
            if (!graph) goto done;
        } else {
            tree = sg_open_body(family) ? sh_decl_tree_parse_reflected_body(source, 1, error, capacity) :
                sh_decl_tree_parse_ordered(source, error, capacity);
            if (!tree) goto done;
        }
    }
    binding.envelope.tree = tree; binding.envelope.graph = graph;
    out = (unsigned char *)sh_decl_rebind_parsed(source, graph ? sh_decl_graph_syntax(graph) : tree,
        &keyed, sg_bind_envelope, &binding.envelope, sg_key_site, &binding, &out_length, &rebound);
done:
    if (reader && reader->failed) { free(out); out = NULL; }
    if (out) status = 1;
    else if (status && !error[0]) snprintf(error, capacity, "%s registration key sites could not be completed%s%s",
        family ? family : "resource", reader && reader->report.first_gap[0] ? ": " : "",
        reader ? reader->report.first_gap : "");
    if (result) {
        result->references = binding.envelope.references; result->changed = out ? rebound.changed : 0;
        if (reader) result->inspection = reader->report;
    }
    if (out) { *output = out; if (output_length) *output_length = out_length; }
    free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
    if (reader) { reader->error = NULL; reader->capacity = 0; }
    return status;
}

unsigned char *sh_package_source_bind_derived(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context,
    sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    if (!reader) return sh_package_source_bind(reader, family, name, body, length,
        resolver, context, output_length, result, error, capacity);
    sh_package_bound_exists previous = reader->bound_exists;
    void *previous_context = reader->bound_context;
    reader->bound_exists = exists; reader->bound_context = exists_context;
    unsigned char *out = sh_package_source_bind(reader, family, name, body, length,
        resolver, context, output_length, result, error, capacity);
    reader->bound_exists = previous; reader->bound_context = previous_context;
    return out;
}

typedef struct sg_map_binding {
    sg_binding binding;
    const char *json;
    size_t length;
    const sh_decl_dependency_schema *view;
    sh_decl_dependency_result *result;
    sh_package_binding_resolver legacy;
    sh_package_map_binding_resolver occurrence;
    sh_package_map_binding_resolver observed;
    void *context;
    sh_map_binding_span *spans;
    size_t span_count, span_capacity, located;
    sh_package_provenance_site inherit_site, reference_site;
    int collecting;
    /* Registration-key sites (package_keys.h) project through this view. */
    const sh_decl_dependency_schema *native;
    sh_decl_dependency_schema keyed;
    sh_package_key_space key_space;
    int key_state; /* 0 unread, 1 read, -1 no key enum */
    char *key_replacement;
} sg_map_binding;

static int sg_map_span_order(const void *left, const void *right)
{
    const sh_map_binding_span *a = left, *b = right;
    uintptr_t ap = (uintptr_t)a->json, bp = (uintptr_t)b->json;
    if (ap != bp) return ap < bp ? -1 : 1;
    return a->length < b->length ? -1 : a->length > b->length;
}
static int sg_map_span_add(sg_map_binding *map, const char *json, size_t length)
{
    if (map->span_count == map->span_capacity) {
        size_t capacity = map->span_capacity ? map->span_capacity * 2 : 32;
        if (capacity < map->span_capacity || capacity > SIZE_MAX / sizeof(*map->spans)) return 0;
        sh_map_binding_span *grown = realloc(map->spans, capacity * sizeof(*grown));
        if (!grown) return 0;
        map->spans = grown; map->span_capacity = capacity;
    }
    map->spans[map->span_count++] = (sh_map_binding_span){json, length}; return 1;
}
static const sh_package_provenance_site *sg_map_span_find(sg_map_binding *map, const char *json, size_t length)
{
    sh_map_binding_span needle = {json, length};
    sh_map_binding_span *span = bsearch(&needle, map->spans, map->located, sizeof(*map->spans), sg_map_span_order);
    return span ? &span->site : NULL;
}
static int sg_map_locate(sg_map_binding *map)
{
    size_t count = 0;
    qsort(map->spans, map->span_count, sizeof(*map->spans), sg_map_span_order);
    for (size_t i = 0; i < map->span_count; i++) {
        if (count && !sg_map_span_order(map->spans + count - 1, map->spans + i))
            sh_map_binding_sites_clear(map->spans + i, 1);
        else map->spans[count++] = map->spans[i];
    }
    map->span_count = count;
    if (!sh_map_binding_sites_locate(map->json, map->length, map->spans, count,
        map->binding.reader->error, map->binding.reader->capacity)) return 0;
    map->located = count; return 1;
}
static int sg_map_collect_root(void *context, const char *native_class, const char *family,
    const char *name, const char *json, size_t length)
{
    (void)native_class; (void)family; (void)name;
    return sg_map_span_add(context, json, length);
}
static int sg_map_collect_state(void *context, const char *native_class, const char *inherit,
    const char *json, size_t length)
{
    (void)native_class; (void)inherit;
    return sg_map_span_add(context, json, length);
}
static int sg_map_resolve(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    sg_map_binding *map = context;
    if (map->legacy) return map->legacy(map->context, slot, family, name, replacement);
    const sh_package_provenance_site *site = map->reference_site.owner ? &map->reference_site :
        map->inherit_site.owner ? &map->inherit_site : NULL;
    return map->occurrence(map->context, site, family, name, replacement);
}
static int sg_map_observe(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    sg_map_binding *map = context;
    /* Only sg_bind_typed's final source value calls this route. Metadata,
     * inherited class and parent reads keep the lookup-only resolver. */
    if (map->observed && !map->collecting && map->reference_site.owner)
        return map->observed(map->context, &map->reference_site, family, name, replacement);
    return sg_map_resolve(context, slot, family, name, replacement);
}
static int sg_map_key_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    sg_map_binding *map = context;
    if (!map->native->describe(map->native->context, type, shape)) return 0;
    if (sg_key_type(type) && shape->kind == SH_DECL_VALUE_IGNORE) shape->kind = SH_DECL_VALUE_REFERENCE;
    return 1;
}
static int sg_map_key_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    sg_map_binding *map = context;
    return map->native->field(map->native->context, type, key, out);
}
static int sg_map_key_dynamic(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    sg_map_binding *map = context;
    return map->native->dynamic_type && map->native->dynamic_type(map->native->context, base, name, length, selected);
}
static void sg_map_key_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    sg_map_binding *map = context;
    if (map->native->gap) map->native->gap(map->native->context, path, type, reason);
}
/* One map-authored key bound like a typed reference at "<site>#<key>": 1 with
 * the key its source context holds now, 0 when that context binds none. */
static int sg_map_key_bind(sg_map_binding *map, const char *slot, const sh_package_provenance_site *location,
    uint32_t key, uint32_t *target)
{
    char decimal[16], *reference, *end = NULL;
    const char *bound = NULL;
    size_t reference_length;
    int status;
    snprintf(decimal, sizeof(decimal), "%u", key);
    reference_length = strlen(location->reference) + strlen(decimal) + 2;
    if (!(reference = malloc(reference_length))) return -1;
    snprintf(reference, reference_length, "%s#%s", location->reference, decimal);
    sh_package_provenance_site site = {location->kind, location->owner, reference};
    if (map->legacy) status = map->legacy(map->context, slot, SH_PACKAGE_KEY_FAMILY, decimal, &bound);
    else if (map->observed) status = map->observed(map->context, &site, SH_PACKAGE_KEY_FAMILY, decimal, &bound);
    else status = map->occurrence(map->context, &site, SH_PACKAGE_KEY_FAMILY, decimal, &bound);
    free(reference);
    if (status < 0) return -1;
    if (status != 1 || !bound) return 0;
    unsigned long number = strtoul(bound, &end, 10);
    if (!end || *end || !number || number > 0xffffffffu) return -1;
    *target = (uint32_t)number; return 1;
}
void sh_package_source_reader_key_codes(sh_package_source_reader *reader, sh_package_key_groups *groups,
    const sh_package_key_groups *other, const sh_package_key_set *taken, sh_package_saved_keys_fn saved_keys)
{
    if (!reader) return;
    reader->key_groups = groups; reader->other_key_groups = other;
    reader->key_taken = taken; reader->saved_keys = saved_keys;
}
static void sg_map_key_move(sg_map_binding *map, sh_package_key_move *move, uint32_t target)
{
    if (!sh_package_key_space_private(&map->key_space)) {
        sh_package_key_move_to(move, 0, target); return;
    }
    sh_package_source_reader *reader = map->binding.reader;
    const sh_package_key_groups *groups = reader->key_groups;
    uint32_t pattern = (target & SH_PACKAGE_KEY_SINGLE) >> 19;
    if ((target & SH_PACKAGE_KEY_MARKER) && (!groups || !groups->defined[pattern])) groups = reader->other_key_groups;
    move->kind = SH_PACKAGE_KEY_REPLACE;
    move->bits = target & ~SH_PACKAGE_KEY_PRIVATE;
    sh_package_key_decode(groups, target, &move->keys);
}
static int sg_map_key_encode(void *context, const sh_package_key_set *members, uint32_t *code)
{
    sg_map_binding *map = context;
    sh_package_source_reader *reader = map->binding.reader;
    *code = sh_package_key_groups_code(reader->key_groups, reader->other_key_groups, reader->key_taken, members);
    if (!*code && reader->error && reader->capacity)
        snprintf(reader->error, reader->capacity, "map registration group could not be represented");
    return *code != 0;
}
/* A map-authored multi-bit code no saved or current key binding names on its
 * own is a group of keys: the site's saved member keys (a loaded map) or, for
 * a value set in this session, the serving generation's definition. Each
 * member binds at "<site>#<member>" like a key of its own, and the value then
 * carries the code the serving generation gives the resulting set. 1 with the
 * code, 0 when the code is no known group, -1 on failure. */
static int sg_map_key_group(sg_map_binding *map, const char *slot, const sh_package_provenance_site *location,
    uint32_t marked, uint32_t *code)
{
    sh_package_source_reader *reader = map->binding.reader;
    uint32_t members[SH_PACKAGE_KEY_CODES];
    uint32_t native_bits = 0;
    size_t count = 0;
    sh_package_key_set current;
    memset(&current, 0, sizeof(current));
    if (reader->saved_keys) {
        int found = reader->saved_keys(map->context, location, members, sizeof(members) / sizeof(*members));
        if (found < 0 || (size_t)found > sizeof(members) / sizeof(*members)) return -1;
        for (int i = 0; i < found; i++)
            if (sh_package_key_index(members[i]) != SIZE_MAX) members[count++] = members[i];
    }
    if (!count) {
        sh_package_key_set defined;
        sh_package_key_decode(reader->key_groups, marked, &defined);
        if (sh_package_key_set_count(&defined) < 2) sh_package_key_decode(reader->other_key_groups, marked, &defined);
        for (size_t i = 0; i < SH_PACKAGE_KEY_CODES && count < sizeof(members) / sizeof(*members); i++)
            if (sh_package_key_set_has(&defined, sh_package_key_code(i))) members[count++] = sh_package_key_code(i);
    }
    if (count < 2) return 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t target = members[i];
        int status = sg_map_key_bind(map, slot, location, members[i], &target);
        if (status < 0) return -1;
        sh_package_key_move moved = {0};
        sg_map_key_move(map, &moved, status ? target : members[i]);
        native_bits |= moved.bits;
        for (size_t w = 0; w < sizeof(current.words) / sizeof(current.words[0]); w++)
            current.words[w] |= moved.keys.words[w];
    }
    if (!(*code = sh_package_key_groups_code(reader->key_groups, reader->other_key_groups, reader->key_taken, &current))) {
        snprintf(reader->error, reader->capacity, "a map value names a set of package AI keys no free group code can hold");
        return -1;
    }
    *code |= native_bits;
    return 1;
}
/* A map-authored spawn-type value: each key it names on its own (a decimal
 * bit or single-bit constant, or a whole marked private key) is bound at
 * "<site>#<key>". An unnamed bit is a package key, and a stock type bit is
 * one where a package's row holds or extends it, so each keeps the package it
 * was chosen from; the value is rewritten through the keys held now. A key
 * no package in the site's context binds stays as authored. */
static int sg_map_key_site(sg_map_binding *map, const char *slot, const sh_package_provenance_site *location,
    const char *value, size_t length, const char **replacement)
{
    sh_package_key_moves to;
    uint32_t bits = 0, target = 0, marked;
    int moved = 0, status;
    if (!map->key_state) map->key_state = sh_package_key_space_read(map->binding.reader->schema, &map->key_space) == 1 ? 1 : -1;
    if (map->key_state < 0 || !sh_package_key_movable_bits(&map->key_space, value, length, &bits) || !bits) return 0;
    memset(to, 0, sizeof(to));
    marked = (bits & SH_PACKAGE_KEY_MARKER) && sh_package_key_space_private(&map->key_space) ? bits & SH_PACKAGE_KEY_PRIVATE : 0;
    if (marked) {
        if ((status = sg_map_key_bind(map, slot, location, marked, &target)) < 0) return -1;
        if (!status && (status = sg_map_key_group(map, slot, location, marked, &target)) < 0) return -1;
        if (status && target != marked) {
            unsigned low = 0;
            while (!(marked >> low & 1u)) low++;
            for (unsigned bit = low + 1; bit < SH_PACKAGE_KEY_BITS; bit++)
                if (marked >> bit & 1u) to[bit].kind = SH_PACKAGE_KEY_DROP;
            sg_map_key_move(map, to + low, target); moved = 1;
        }
        bits &= ~marked;
    }
    for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
        if (!(bits >> bit & 1u)) continue;
        if ((status = sg_map_key_bind(map, slot, location, 1u << bit, &target)) < 0) return -1;
        if (status && target != 1u << bit) { sg_map_key_move(map, to + bit, target); moved = 1; }
    }
    if (!moved) return 0;
    size_t out_length = 0; int failed = 0;
    sh_package_key_encoder encoder = {sg_map_key_encode, map};
    char *out = sh_package_key_relocate(&map->key_space, value, length, to,
        sh_package_key_space_private(&map->key_space) ? &encoder : NULL, &out_length, &failed);
    if (failed) return -1;
    if (!out) return 0;
    free(map->key_replacement); map->key_replacement = out; *replacement = out; return 1;
}
static int sg_map_typed_site(void *context, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const sh_decl_reference_site *site, const char **replacement)
{
    sg_map_binding *map = context;
    if (map->collecting) return site && !site->node && site->json &&
        sg_map_span_add(map, site->json, site->json_length) ? 0 : -1;
    const sh_package_provenance_site *location = site ? sg_map_span_find(map, site->json, site->json_length) : NULL;
    if (!location) return -1;
    if (sg_key_type(type)) {
        int status = sg_map_key_site(map, slot, location, name, length, replacement);
        if (status < 0) sg_gap(map->binding.reader, slot, type.name, "map registration key could not be bound");
        return status;
    }
    map->reference_site = *location;
    int ok = sg_bind_typed(&map->binding, slot, type, name, length, replacement);
    memset(&map->reference_site, 0, sizeof(map->reference_site)); return ok;
}

static int sg_map_root(void *context, const char *native_class, const char *family,
    const char *name, const char *json, size_t length)
{
    sg_map_binding *map = context;
    sh_decl_value_type type = {native_class, "*"};
    sh_decl_reference_site site = {NULL, json, length};
    const sh_decl_dependency_schema *view = map->view;
    (void)family;
    const sh_package_provenance_site *location = sg_map_span_find(map, json, length);
    if (!location) return 0;
    map->inherit_site = *location;
    if (!view->reference(view->context, "map/root", type, name, strlen(name)) ||
        !view->reference_site(view->context, "map/root", type, name, strlen(name), &site)) return 0;
    if (map->result->references == SIZE_MAX) return 0;
    map->result->references++; return 1;
}

static int sg_map_inline(void *context, const char *class_name, const char *inherit,
    const char *json, size_t length)
{
    sg_map_binding *map = context;
    sg_walk *reader = map->binding.reader;
    sh_decl_source parent = {0};
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives};
    sh_decl_dependency_result result = {0};
    char *inherited = NULL, *inherit_path = NULL;
    int ok = 0;
    /* The usage reader supplies state.edit inside an idDeclEntityDef envelope.
     * Its inherit member belongs to that same envelope, irrespective of native
     * IDs or where the envelope is nested in the serialized document. */
    const sh_package_provenance_site *location = sg_map_span_find(map, json, length);
    static const char suffix[] = "/k:state/k:edit";
    if (!location) goto done;
    size_t path_length = strlen(location->reference), suffix_length = sizeof(suffix) - 1;
    if (path_length < suffix_length || strcmp(location->reference + path_length - suffix_length, suffix)) goto done;
    inherit_path = malloc(path_length + 1);
    if (!inherit_path) goto done;
    memcpy(inherit_path, location->reference, path_length - suffix_length);
    memcpy(inherit_path + path_length - suffix_length, "/k:inherit", sizeof("/k:inherit"));
    map->inherit_site = *location; map->inherit_site.reference = inherit_path;
    if (!class_name || !*class_name) {
        if (!inherit || !*inherit || sg_read_parent(reader, inherit, &parent) != 1 ||
            !(inherited = sh_decl_entity_class(parent, &classes, reader->error, reader->capacity))) goto done;
        class_name = inherited;
    }
    ok = sh_decl_json_state_dependencies(json, length, (sh_decl_value_type){class_name, ""}, map->view, &result);
    if (result.visited > SIZE_MAX - map->result->visited ||
        result.references > SIZE_MAX - map->result->references || result.gaps > SIZE_MAX - map->result->gaps) ok = 0;
    else {
        map->result->visited += result.visited; map->result->references += result.references;
        map->result->gaps += result.gaps; map->result->aborted |= result.aborted;
    }
done:
    memset(&map->inherit_site, 0, sizeof(map->inherit_site));
    free(inherit_path); free((void *)parent.text); free(inherited); return ok;
}

static int sg_map_walk(void *context, const sh_decl_dependency_schema *view, sh_decl_dependency_result *result)
{
    sg_map_binding *map = context;
    map->view = view; map->result = result;
    return sh_package_map_reference_sites(map->json, map->length, sg_map_root, map) &&
        sh_package_map_state_sites(map->json, map->length, sg_map_inline, map);
}

static char *sg_bind_map(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_binding_resolver legacy,
    sh_package_map_binding_resolver occurrence, sh_package_map_binding_resolver observed, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    sg_map_binding map = {0}; sh_decl_rebind_result rebound = {0};
    char *out = NULL, fallback[1024]; size_t out_length = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!reader || !json || (!legacy && !occurrence)) return NULL;
    reader->failed = 0; reader->error = error; reader->capacity = capacity;
    memset(&reader->report, 0, sizeof(reader->report));
    reader->current_type = "map"; reader->current_name = "bindings";
    reader->saved_binding = sg_map_resolve; reader->saved_context = &map;
    map.binding.reader = reader; map.binding.resolver = sg_map_observe; map.binding.context = &map;
    map.legacy = legacy; map.occurrence = occurrence; map.observed = observed; map.context = context;
    map.json = json; map.length = length;
    map.native = sh_decl_native_schema_view(reader->schema);
    map.keyed = (sh_decl_dependency_schema){&map, sg_map_key_describe, sg_map_key_field,
        map.native->reference, sg_map_key_gap, sg_map_key_dynamic, map.native->reference_site};
    if (!sh_package_map_reference_sites(json, length, sg_map_collect_root, &map) ||
        !sh_package_map_state_sites(json, length, sg_map_collect_state, &map) || !sg_map_locate(&map)) goto done;
    /* Reflection determines which value spans are resources. Locate that exact
     * set before binding, so equal names in two objects never share ownership.
     * The collect pass does not use existence checks to choose native objects. */
    map.collecting = 1;
    out = sh_decl_rebind_json_walk_sites((sh_decl_source){json, length}, &map.keyed,
        sg_map_walk, &map, sg_map_typed_site, &map, &out_length, &rebound);
    if (!out) goto done;
    free(out); out = NULL; map.collecting = 0;
    memset(&map.inherit_site, 0, sizeof(map.inherit_site));
    if (!sg_map_locate(&map)) goto done;
    out = sh_decl_rebind_json_walk_sites((sh_decl_source){json, length}, &map.keyed,
        sg_map_walk, &map, sg_map_typed_site, &map, &out_length, &rebound);
done:
    if (reader->failed) { free(out); out = NULL; }
    if (!out && !*error) snprintf(error, capacity, "typed map bindings could not be completed%s%s",
        reader->report.first_gap[0] ? ": " : "", reader->report.first_gap);
    if (output_length && out) *output_length = out_length;
    if (result) {
        result->references = map.binding.references; result->changed = out ? rebound.changed : 0;
        result->inspection = reader->report;
    }
    free(map.binding.replacement); free(map.key_replacement);
    sh_map_binding_sites_clear(map.spans, map.span_count); free(map.spans);
    reader->saved_binding = NULL; reader->saved_context = NULL;
    reader->error = NULL; reader->capacity = 0; return out;
}

char *sh_package_source_bind_map(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{ return sg_bind_map(reader, json, length, resolver, NULL, NULL, context, output_length, result, error, capacity); }

char *sh_package_source_bind_map_sites(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_map_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{ return sg_bind_map(reader, json, length, NULL, resolver, resolver, context, output_length, result, error, capacity); }

char *sh_package_source_bind_map_sites_observed(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_map_binding_resolver lookup,
    sh_package_map_binding_resolver observe, void *context, size_t *output_length,
    sh_package_source_binding *result, char *error, size_t capacity)
{ return sg_bind_map(reader, json, length, NULL, lookup, observe, context, output_length, result, error, capacity); }

/* Reflected text cannot name every native dependency. Registration dispatch,
 * cooked companions and custom readers already contribute exact bound edges
 * during compilation. Follow those edges from required roots as well; never
 * rediscover their targets through another source scope or the live registry. */
static int sg_bound_files(sg_walk *walk)
{
    const sh_package_compilation *compiled = walk->compiled;
    size_t *queue = NULL, count = 0;
    unsigned char *seen = NULL;
    int ok = 0;
    if (!compiled->resource_count) return 1;
    if (compiled->resource_count > SIZE_MAX / sizeof(*queue) ||
        !(queue = malloc(compiled->resource_count * sizeof(*queue))) ||
        !(seen = calloc(compiled->resource_count, 1))) goto done;
    for (size_t i = 0; i < walk->paths.count; i++) {
        const sh_compiled_resource *resource;
        if (i >= walk->required_capacity || !walk->required[i]) continue;
        resource = sh_package_compilation_find(compiled, walk->paths.items[i].name);
        if (!resource || (resource->type && sg_editor(resource->type)) ||
            (resource->native_type && sg_editor(resource->native_type))) continue;
        size_t index = (size_t)(resource - compiled->resources);
        if (!seen[index]) { seen[index] = 1; queue[count++] = index; }
    }
    walk->native_origin = walk->native_leaf = 0;
    for (size_t i = 0; i < count; i++) {
        const sh_compiled_resource *resource = compiled->resources + queue[i];
        if (resource->bound_dependency_count && !resource->bound_dependencies) goto done;
        for (size_t j = 0; j < resource->bound_dependency_count; j++) {
            const char *path = resource->bound_dependencies[j];
            const sh_compiled_resource *target = sh_package_compilation_find(compiled, path);
            /* Palette companionship retains native objects but is not a map
             * gameplay requirement. A file also reached from gameplay still
             * participates through that independent edge. */
            if (target && ((target->type && sg_editor(target->type)) ||
                (target->native_type && sg_editor(target->native_type)))) continue;
            if (!sg_file(walk, path)) goto done;
            if (target) {
                size_t index = (size_t)(target - compiled->resources);
                if (!seen[index]) { seen[index] = 1; queue[count++] = index; }
            }
        }
    }
    ok = 1;
done:
    free(queue); free(seen);
    if (!ok) walk->failed = 1;
    return ok;
}

int sh_package_source_graph_with_contracts(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_shader *shader, const sh_package_source_audio *audio,
    const sh_package_source_admission *admission,
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
    if (shader && !sh_package_source_reader_shader(&walk, shader)) goto done;
    if (audio && !sh_package_source_reader_audio(&walk, audio)) goto done;
    if (admission) walk.admission = *admission;
    candidate = (sh_decl_registry_source){&walk, sg_native_read, registry.registry, sg_exists};
    metadata = (sh_decl_native_source){&walk, sg_native_read, reflection, registry.scripts};
    observer = (sh_decl_dependency_observer){&walk, sg_reference, sg_field_gap};
    walk.registry = sh_decl_native_registry_open(candidate, error, capacity);
    walk.schema = sh_decl_native_schema_open(metadata, observer, error, capacity);
    if (!walk.registry || !walk.schema) goto done;
    sh_decl_native_schema_attach_registry(walk.schema, walk.registry);
    if (!sh_package_map_references(json, length, sg_add, &walk) ||
        !sh_package_map_states(json, length, sg_inline, &walk)) goto done;
    for (size_t i = 0; i < walk.queue.count; i++) {
        /* Strings have independent ownership: enqueue may move queue.items. */
        const char *type = walk.queue.items[i].type, *name = walk.queue.items[i].name;
        walk.current = i;
        if (!sg_inspect(&walk, type, name)) goto done;
    }
    /* A gap of an identity only unchanged SnapMap originals name is native;
     * one the map or a candidate also names counts. */
    for (size_t i = 0; i < walk.deferred_count; i++) {
        if (!walk.candidate[walk.deferred[i].index]) { walk.report.native_gaps++; continue; }
        walk.report.gaps++;
        if (!walk.report.first_gap[0])
            snprintf(walk.report.first_gap, sizeof(walk.report.first_gap), "%s", walk.deferred[i].reason);
    }
    for (size_t i = 0; i < walk.deferred_path_count; i++)
        if (walk.candidate[walk.deferred_paths[i].identity]) walk.required[walk.deferred_paths[i].path] = 1;
    if (!sg_bound_files(&walk)) goto done;
    {
        size_t kept = 0;
        for (size_t i = 0; i < walk.paths.count; i++) {
            if (i < walk.required_capacity && walk.required[i]) { walk.paths.items[kept++] = walk.paths.items[i]; continue; }
            free(walk.paths.items[i].type); free(walk.paths.items[i].name);
        }
        walk.paths.count = kept;
    }
    /* Closure is only claimed when nothing in this walk was left to an adapter:
     * every recorded gap (unsupported reader, unexpanded inheritance, late
     * string consumer) keeps the result explicitly incomplete. */
    walk.paths.incomplete = walk.report.gaps != 0;
    *out = walk.paths; memset(&walk.paths, 0, sizeof(walk.paths)); ok = 1;
done:
    if (!ok && !error[0]) snprintf(error, capacity, "candidate source dependency inspection failed");
    if (report) *report = walk.report;
    sg_lists_free(&walk);
    sh_package_audio_close(walk.audio); sh_package_audio_scope_close(walk.audio_scope);
    sg_shader_clear(&walk);
    sh_decl_native_registry_close(walk.registry); sh_decl_native_schema_close(walk.schema); return ok;
}

int sh_package_source_graph_with_shader(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_shader *shader, const char *json, size_t length,
    sh_package_references *out, sh_package_source_graph_report *report, char *error, size_t capacity)
{
    return sh_package_source_graph_with_contracts(compiled, catalog, original, original_context,
        registry, reflection, shader, NULL, NULL, json, length, out, report, error, capacity);
}

int sh_package_source_graph(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity)
{
    return sh_package_source_graph_with_shader(compiled, catalog, original, original_context,
        registry, reflection, NULL, json, length, out, report, error, capacity);
}
