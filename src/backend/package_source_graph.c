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
#include "native_class_registry.h"
#include "decl_native_carrier.h"
#include "package_native_inline.h"
#include "package_variants.h"
#include "native_resource_id.h"
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
    const sh_package_map_local_names *map_local; /* sh_package_source_reader_map_local */
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
    /* Per queued identity (SG_* below): who names it, how its uses reach
     * construction, and what the idClass registry says about its class.
     * verdict holds the registry's reason for a nonconstructible or
     * unverifiable class (sh_native_construction_text). */
    unsigned short *candidate;
    char **verdict;
    int inherit_read; /* the reference being queued is an inherit read */
    int pool_listing; /* queuing an AI spawn pool's entityDef lists (accounted by sg_pool_lists) */
    /* AI behavior use (sg_ai_routes): ai_state while an idAI2-derived state's
     * dependencies are walked (1 a definition's, 2 a constructed placed
     * copy's), ai_field while that state's behavior field is queued, and
     * ai_override once a placed copy sets the field itself. ai_uses pairs a
     * definition (SIZE_MAX: a constructed placed copy) with the behavior its
     * state selects; ai_entries a behavior with each pooledProjectileEnts
     * entry. world_gap: the world's selection is already a recorded gap. */
    int ai_state, ai_field, ai_override, world_gap, behavior_state;
    /* Spawner use (sg_spawner_routes): spawner_state while the state of a
     * class deriving idSnapMapGameEntity_Spawner is walked (1 a
     * definition's, 2 a constructed placed copy's), spawner_field while one
     * of its spawnerEntityPair definitions is queued (1 entityStatic, 2
     * entityPinata). spawner_uses pairs a spawner definition, or (definition
     * SIZE_MAX) the constructed placed copy spawner_copies[copy], with each
     * definition it spawns by name. spawner_pairs records whether each walked
     * spawner definition's effective state sets both pointers;
     * spawner_copies whether each constructed placed copy's merged state does
     * and which fields it leaves to its spawner-class parent (SIZE_MAX: none
     * read through the parent's walk). spawner_copy: the copy being walked. */
    int spawner_state, spawner_field;
    struct sg_spawner_use { size_t definition, entry, copy; int field; } *spawner_uses;
    size_t spawner_use_count, spawner_use_capacity;
    struct sg_pair *spawner_pairs;
    size_t spawner_pair_count, spawner_pair_capacity;
    struct sg_spawner_copy { size_t parent; int unset, set; } *spawner_copies;
    size_t spawner_copy_count, spawner_copy_capacity, spawner_copy;
    int inherited_member; /* a served parent's state is walked under a placed copy's class (sg_inline_inherited) */
    int project_mode; /* R-249 map projection (sh_package_source_reader_project_mode) */
    sh_package_map_projection_resolver project; void *project_context;
    /* R-255 reader-bound parent variants a bound document names (sh_package_source_reader_variant_sink). */
    sh_package_variant_sink variant_sink; void *variant_context;
    /* Every counted gap's text, in order (sh_package_source_reader_gap_sink). */
    sh_package_gap_sink gap_sink; void *gap_context;
    /* Members a captured copy inherits from its variant (fold sites), while
     * its parent form is bound (sg_bind_map). */
    char **variant_sites; size_t variant_site_count; int variant_folding;
    struct sg_pair { size_t first, second; } *ai_uses, *ai_entries;
    size_t ai_use_count, ai_use_capacity, ai_entry_count, ai_entry_capacity;
    /* The AI spawn pool the world selects (sg_world_pool): 0 not read yet,
     * 1 read (world_pool_name NULL: none), -1 not modelled (reason kept). */
    int world_pool;
    char *world_pool_name, world_pool_reason[200];
    /* The effective pool settings (sh_native_pool_settings_read), read once
     * when a construction is first judged: 0 not read, 1 read, -1 unavailable. */
    int pool_settings_state;
    sh_native_pool_settings pool_settings;
    const char *map_text; /* the map JSON whose root spans are being queued */
    size_t map_length;
    /* map_state: a placed entity's own state is being walked (sg_inline).
     * local_names: the map's placed play-copy names (sg_walk_local), read once:
     * local_state 0 not read, 1 read, -1 unavailable. */
    int map_state, local_state;
    sh_native_inline_generated_set local_names;
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
    sh_package_source_original_status original_status; /* sh_package_source_reader_original_status */
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
    if (walk->gap_sink) walk->gap_sink(walk->gap_context, text);
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

/* SG_CANDIDATE named by the map or a candidate declaration; SG_NAMED reached
 * other than through an inherit read or an AI spawn pool list, by a reference
 * whose native consumer is not traced; SG_POOLED its own positive poolCount;
 * SG_WORLD_SIZED listed by an enabled AI spawn pool with a positive poolSize
 * (pool creation without an own poolCount); SG_WORLD_LISTED listed there
 * with no size (projectileEnts, poolSize 0: a pool only for an own
 * poolCount); SG_INSPECTED its class was judged; SG_SERVED the candidate
 * serves it; SG_UNBUILDABLE CreateManagedClassFromDecl returns NULL for its
 * class; SG_CLASS_UNAVAILABLE the registry could not be read for it;
 * SG_POOL_GAP a construction gap is recorded (SG_PROVEN_GAP: one for a proven
 * route); SG_AI_PREPOOL listed by the pooledProjectileEnts of a behavior
 * an AI whose construction is proven spawns with (SG_AI_USE on that
 * behavior), SG_AI_UNTRACED listed by, or for a behavior used by, an AI
 * whose construction is not traced; SG_AI_SPAWNED a definition a
 * constructed placed idAI2 copy spawns without setting its own behavior
 * (sg_ai_routes); SG_SPAWNER_SPAWNED a constructed spawner spawns it by name
 * (sg_spawner_routes). */
enum {
    SG_CANDIDATE = 1, SG_NAMED = 2, SG_POOLED = 4, SG_POOL_GAP = 8, SG_INSPECTED = 16, SG_WORLD_SIZED = 32,
    SG_WORLD_LISTED = 64, SG_UNBUILDABLE = 128, SG_CLASS_UNAVAILABLE = 256, SG_SERVED = 512, SG_AI_PREPOOL = 1024,
    SG_PROVEN_GAP = 2048, SG_AI_USE = 4096, SG_AI_UNTRACED = 8192, SG_AI_SPAWNED = 16384, SG_SPAWNER_SPAWNED = 32768
};
/* The SnapMap spawner class whose spawnerEntityPair its spawn reads. */
#define SG_SPAWNER_CLASS "idSnapMapGameEntity_Spawner"
static void sg_construction_check(sg_walk *walk, size_t index);
static void sg_judge(sg_walk *walk, size_t index, const char *class_name, int pooled, int served);
static void sg_pool_lists(sg_walk *walk, const sh_decl_node *edit, const char *type);
static void sg_behavior_lists(sg_walk *walk, const sh_decl_node *edit);
static int sg_inherited_readers(sg_walk *walk, const sh_decl_entity_class_source *classes, const char *consumer,
    const char *inherit, const sh_decl_node *const *own, size_t own_count, const char *skip, char *reason, size_t capacity,
    int projectable);
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
            unsigned short *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(walk->candidate, next * sizeof(*grown)) : NULL;
            char **reasons = grown ? realloc(walk->verdict, next * sizeof(*reasons)) : NULL;
            if (grown) walk->candidate = grown;
            if (reasons) walk->verdict = reasons;
            if (!grown || !reasons) ok = 0;
            else {
                memset(grown + walk->candidate_capacity, 0, (next - walk->candidate_capacity) * sizeof(*grown));
                memset(reasons + walk->candidate_capacity, 0, (next - walk->candidate_capacity) * sizeof(*reasons));
                walk->candidate_capacity = next;
            }
        }
        /* Named by the map or a candidate declaration, not only by stock. */
        if (ok && !walk->native_origin && !walk->native_leaf) walk->candidate[index] |= SG_CANDIDATE;
        /* Parse reads a parent through inherit; the engine never spawns it by
         * that name. AI spawn pool lists are accounted by their own consumer
         * (sg_pool_lists). A pooled definition first named later still gets
         * its gap. */
        if (ok && !walk->inherit_read && !walk->pool_listing && !walk->ai_field && !walk->spawner_field) {
            walk->candidate[index] |= SG_NAMED;
            sg_construction_check(walk, index);
        }
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
    for (size_t i = 0; walk->verdict && i < walk->candidate_capacity; i++) free(walk->verdict[i]);
    free(walk->verdict); walk->verdict = NULL;
    free(walk->world_pool_name); walk->world_pool_name = NULL; walk->world_pool = 0;
    free(walk->ai_uses); free(walk->ai_entries); walk->ai_uses = walk->ai_entries = NULL;
    free(walk->spawner_uses); free(walk->spawner_pairs); free(walk->spawner_copies);
    walk->spawner_uses = NULL; walk->spawner_pairs = NULL; walk->spawner_copies = NULL;
    walk->spawner_use_count = walk->spawner_use_capacity = walk->spawner_pair_count = walk->spawner_pair_capacity = 0;
    walk->spawner_copy_count = walk->spawner_copy_capacity = 0; walk->spawner_copy = SIZE_MAX;
    walk->ai_use_count = walk->ai_use_capacity = walk->ai_entry_count = walk->ai_entry_capacity = 0;
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
/* sg_read_path's answer where only the answer is used: the same lookup observation and status, without copying
 * an original's bytes when the reader can tell its status alone (sh_package_source_reader_original_status). A
 * scope's own file is still read, as sg_read_path checks it. */
static int sg_path_status(sg_walk *walk, const char *path)
{
    unsigned char *body = NULL; size_t length = 0;
    if (!walk->original_status || !walk->original || sh_package_compilation_find(walk->compiled, path)) {
        int status = sg_read_path(walk, path, &body, &length);
        free(body); return status;
    }
    if (walk->lookup) walk->lookup(walk->lookup_context, path, 0);
    int status = walk->original_status(walk->original_context, path);
    if (status < 0) walk->failed = 1;
    return status;
}

/* Every own statement of the entity's state name, in order, without a limit.
 * 1 with *states (the caller frees; NULL when there are none), -1 on failure. */
static int sg_own_states(const sh_decl_entity_headers *headers, const sh_decl_node ***states, size_t *count)
{
    size_t n = 0;
    *states = NULL; *count = 0;
    if (!headers->state || !headers->state->key) return 1;
    for (const sh_decl_node *child = headers->state; child; child = child->next)
        if (child->key && !strcmp(child->key, headers->state->key)) n++;
    if (!(*states = malloc(n * sizeof(**states)))) return -1;
    for (const sh_decl_node *child = headers->state; child; child = child->next)
        if (child->key && !strcmp(child->key, headers->state->key)) (*states)[(*count)++] = child;
    return 1;
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
static int sg_known(void *context, const char *name)
{ return sh_decl_native_schema_class_known(((sg_walk *)context)->schema, name); }

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
/* A by-name reference (SH_DECL_BY_NAME_DECL_OPS) whose name has no source in
 * scope stays as authored with no edge and no gap, in a map as in a package:
 * the value is text its consumers resolve later, and nothing in scope is
 * supplied under the name. A source in scope binds like a typed pointer. */
static int sg_by_name(sh_decl_value_type type, const char *name, size_t length)
{ return type.ops && !strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS) && !sg_composition_token(name, length); }
/* Whether a declaration has a source only in the campaign archive: no file in
 * this scope and a campaign-only original (sg_read_path 2). A by-name reference
 * to one keeps its literal name with no source edge, as for an absent name: an
 * idEntityDefRef holds text until a consumer resolves it, the served text keeps
 * that text, and this scope supplies nothing under the name, so whatever the
 * engine's resolution yields is what the authored or stock text gets (no claim
 * that it yields NULL). The stock SnapMap player is one such holder: player/mp's
 * syncMelee.syncMeleeEntityDefs (idSyncAttack +0x98, idList<idEntityDefRef>)
 * names zion/syncattack/revenant_player_ranged_pvp, which only the campaign
 * archive carries. Its reader, idSyncAttack VK 0x140c45b20 / GL 0x140c45450 at
 * the player's initialization, resolves each name through the entityDef
 * manager's find-or-load with makeDefault 0 (VK 0x1417b5250 / GL 0x1417a7d10)
 * and spawns a definition the game local's list did not already hold.
 * 1 campaign-only, 0 not, -1 failed (walk->failed). */
static int sg_campaign_only(sg_walk *walk, const char *family, const char *name)
{
    unsigned char *body = NULL; size_t length = 0;
    char *canonical = sg_name(family), *path = canonical && name ? sg_path(canonical, name) : NULL;
    free(canonical);
    if (!path) { walk->failed = 1; return -1; }
    int status = sg_read_path(walk, path, &body, &length);
    free(body); free(path);
    return status < 0 ? -1 : status == 2;
}

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

/* The queue index of (type, name), or SIZE_MAX. */
static size_t sg_queued(sg_walk *walk, const char *type, const char *name)
{
    char *normalized_type = sg_name(type), *normalized_name = sg_name(name);
    size_t index = normalized_type && normalized_name ?
        sg_set_find(&walk->queue_set, &walk->queue, normalized_type, normalized_name) : SIZE_MAX;
    free(normalized_type); free(normalized_name);
    return index < walk->candidate_capacity ? index : SIZE_MAX;
}
static int sg_pair_add(sg_walk *walk, struct sg_pair **items, size_t *count, size_t *capacity, size_t first, size_t second)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;
        struct sg_pair *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(*items, next * sizeof(*grown)) : NULL;
        if (!grown) { walk->failed = 1; return 0; }
        *items = grown; *capacity = next;
    }
    (*items)[*count].first = first; (*items)[(*count)++].second = second; return 1;
}
static int sg_spawner_use_add(sg_walk *walk, size_t definition, size_t entry, size_t copy, int field)
{
    if (walk->spawner_use_count == walk->spawner_use_capacity) {
        size_t next = walk->spawner_use_capacity ? walk->spawner_use_capacity * 2 : 16;
        struct sg_spawner_use *grown = next <= SIZE_MAX / sizeof(*grown) ?
            realloc(walk->spawner_uses, next * sizeof(*grown)) : NULL;
        if (!grown) { walk->failed = 1; return 0; }
        walk->spawner_uses = grown; walk->spawner_use_capacity = next;
    }
    walk->spawner_uses[walk->spawner_use_count++] = (struct sg_spawner_use){definition, entry, copy, field};
    return 1;
}
static size_t sg_spawner_copy_add(sg_walk *walk)
{
    if (walk->spawner_copy_count == walk->spawner_copy_capacity) {
        size_t next = walk->spawner_copy_capacity ? walk->spawner_copy_capacity * 2 : 16;
        struct sg_spawner_copy *grown = next <= SIZE_MAX / sizeof(*grown) ?
            realloc(walk->spawner_copies, next * sizeof(*grown)) : NULL;
        if (!grown) { walk->failed = 1; return SIZE_MAX; }
        walk->spawner_copies = grown; walk->spawner_copy_capacity = next;
    }
    walk->spawner_copies[walk->spawner_copy_count] = (struct sg_spawner_copy){SIZE_MAX, 0, 3};
    return walk->spawner_copy_count++;
}

/* The pointer each spawnerEntityPair field of a state stores, as the native
 * read applies it over the spawner constructor's NULL pair (VK 0x1414f9bd0 /
 * GL 0x1414ed0c0 store 0 at +0xC90 and +0xC98): bit 1 entityStatic, bit 2
 * entityPinata set to a declaration, bit 4/8 the field present at all. The
 * generic pointer dispatch (VK 0x141a21260) stores NULL for the unquoted NULL
 * token, the form a JSON null loads as. Any other value is a name its pointer
 * reader looks up with makeDefault (VK 0x1409ca890 / GL 0x1409ca050), a quoted
 * "" (renamed _emptyName) or "NULL" included, so it stores a declaration. A
 * pair or field shape that reader does not model is taken as set, never as a
 * reason to drop the spawn. The last occurrence of a key is the one kept. */
static int sg_pair_state(const sh_decl_node *state)
{
    static const char *const fields[] = {"entityStatic", "entityPinata"};
    const sh_decl_node *pair = NULL;
    int bits = 0;
    if (!state) return 0;
    if (!state->compound) return 15;
    for (const sh_decl_node *child = state->children; child; child = child->next)
        if (child->key && !strcmp(child->key, "spawnerEntityPair")) pair = child;
    if (!pair) return 0;
    if (!pair->compound) return 15;
    for (int i = 0; i < 2; i++) {
        const sh_decl_node *value = NULL;
        for (const sh_decl_node *child = pair->children; child; child = child->next)
            if (child->key && !strcmp(child->key, fields[i])) value = child;
        if (!value) continue;
        bits |= 4 << i;
        if (value->compound || !value->value || strcmp(value->value, "NULL")) bits |= 1 << i;
    }
    return bits;
}

/* Whether a served resource was authored as this declaration: an inherited
 * name a package supplies under a private identity, which an unbound spelling
 * would miss, unlike a name no package supplies (the engine's default). */
static int sg_authored(sg_walk *walk, sh_decl_value_type type, const char *name, size_t length)
{
    const char *family = NULL;
    char *copy = NULL, *path = NULL;
    int found = 0;
    if (!name || !length || sh_decl_native_registry_class_type(walk->registry, type.name, &family) <= 0 || !family)
        return 0;
    if (!(copy = malloc(length + 1))) { walk->failed = 1; return 0; }
    memcpy(copy, name, length); copy[length] = 0;
    if (!(path = sg_path(family, copy))) walk->failed = 1;
    for (size_t i = 0; path && !found && i < walk->compiled->resource_count; i++) {
        const sh_compiled_resource *resource = walk->compiled->resources + i;
        found = resource->original_path && !_stricmp(resource->original_path, path);
    }
    free(copy); free(path); return found;
}

/* An empty quoted literal reaches the walk as its lookup of _emptyName
 * (empty_lookup), whose absence the lookup itself defaults. */
static int sg_empty(const char *name, size_t length)
{ return name && length == 10 && !memcmp(name, "_emptyName", 10); }

/* Declaration text and a map's JSON state both report empty literals as
 * lookups: the map reader loads a JSON "" as the quoted empty token. */
static int sg_empty_names(void *context)
{ (void)context; return 1; }

static int sg_local_equal(const char *a, size_t a_length, const char *b, size_t b_length);
/* A placed entity's own state may name another placed entity's generated play
 * copy (sg_map_local, the binder's route): SnapMap play registers every copy
 * before any entity spawns, and the state's typed lookup finds it there. It is
 * neither a resource nor missing. 1 exactly one placed entity's copy carries
 * the name, 0 none (or not an entityDef), -1 more than one. */
static int sg_walk_local(sg_walk *walk, sh_decl_value_type type, const char *name, size_t length)
{
    const char *family = NULL;
    size_t found = 0;
    if (!walk->map_text || sh_decl_native_registry_class_type(walk->registry, type.name, &family) <= 0 || !family ||
        _stricmp(family, "entityDef")) return 0;
    if (!walk->local_state)
        walk->local_state = sh_native_inline_generated_read(walk->map_text, walk->map_length, &walk->local_names) > 0 ? 1 : -1;
    for (size_t i = 0; walk->local_state > 0 && i < walk->local_names.count; i++) {
        const sh_native_inline_generated *row = walk->local_names.rows + i;
        char *generated = row->inherit && row->unique ? sh_native_inline_generated_name(row, row->inherit) : NULL;
        if (generated && sg_local_equal(generated, strlen(generated), name, length)) found++;
        free(generated);
    }
    return found == 1 ? 1 : found ? -1 : 0;
}

static int sg_reference(void *context, const char *field, sh_decl_value_type type,
    const char *name, size_t length)
{
    sg_walk *walk = context;
    sh_decl_native_reference reference = {0};
    int status, ok = 1;
    walk->report.references++;
    status = sh_decl_native_reference_read(walk->schema, walk->registry, type, name, length, &reference);
    /* A by-name reference to a declaration only the campaign archive carries
     * keeps its literal name with no source in scope (sg_campaign_only), below.
     * A typed pointer keeps its source dependency here: its reader resolves
     * the name at parse and stores the object; the source link decides whether
     * that dependency is authored (sl_authored_mark). */
    if (status == 1 && !walk->inherited_member && sg_by_name(type, name, length)) {
        int campaign = sg_campaign_only(walk, reference.resource.type, reference.resource.name);
        if (campaign < 0) { sh_decl_native_reference_clear(&reference); return 0; }
        if (campaign) status = 2;
    }
    /* A package name the defining layer left unbound reaches the reading
     * class's global lookup: absent, or another source's same spelling
     * (bound names are private identities, never an authored spelling). */
    if ((status == 2 || status == 1) && walk->inherited_member && sg_authored(walk, type, name, length)) status = -1;
    else if (status == 2 && walk->map_state && !walk->inherited_member && sg_walk_local(walk, type, name, length) > 0) status = 0;
    else if (status == 2) status = sg_native_default(walk) || sg_empty(name, length) ||
        (sg_event_decl(type) && !sg_composition_token(name, length)) || sg_by_name(type, name, length) ? 0 : -1;
    if (status == 1 && reference.make_default) {
        int defaulted = sg_default_use(walk, reference.resource.type, reference.resource.name, 1);
        if (defaulted) status = defaulted > 0 ? 0 : -1;
    }
    walk->inherit_read = field && !strcmp(field, "inherit");
    /* AI +0xc120, which the AI spawn reads for its behavior, is the
     * reflected idAI2::aiEditable (+0xbe38) .behaviors (+0x2e0) .decl (+0x8)
     * on both images. Only that field of an idAI2-derived state selects the
     * behavior; another reference names it for an untraced consumer. */
    walk->ai_field = status == 1 && walk->ai_state && field && !strcmp(field, "edit.aiEditable.behaviors.decl") &&
        reference.resource.type && !_stricmp(reference.resource.type, "aibehavior");
    /* The spawner's spawn (VK 0x141284120 / GL 0x1412760b0) spawns its
     * reflected spawnerEntityPair (+0xC88) .entityStatic (+0xC90) or
     * .entityPinata (+0xC98) by name through the game spawn (vtable +0x450):
     * routed by the spawner's construction (sg_spawner_routes), not an
     * untraced consumer. */
    walk->spawner_field = status == 1 && walk->spawner_state && field && reference.resource.type &&
        !_stricmp(reference.resource.type, "entitydef") ? !strcmp(field, "edit.spawnerEntityPair.entityStatic") ? 1 :
        !strcmp(field, "edit.spawnerEntityPair.entityPinata") ? 2 : 0 : 0;
    /* A behavior's pooledProjectileEnts are its AI's pre-pool and projectile
     * list: routed by the behavior's use (sg_ai_routes), not a use by name. */
    int listing = walk->pool_listing;
    if (walk->behavior_state && field && !strncmp(field, "edit.pooledProjectileEnts", 25) && strchr(".[", field[25]))
        walk->pool_listing = 1;
    if (status == 1) ok = sg_add(walk, reference.resource.type, reference.resource.name);
    else if (status < 0) sg_gap(walk, field, type.name, walk->inherited_member ? "reference identity is unavailable "
        "(product coverage): the consuming class reads this inherited member, which its defining class does not "
        "reflect and the classes deriving from it do not type alike, so the package compiled the name unbound" :
        "reference identity is unavailable");
    walk->pool_listing = listing;
    if (ok && walk->ai_field) {
        size_t behavior = sg_queued(walk, reference.resource.type, reference.resource.name);
        if (behavior != SIZE_MAX)
            ok = sg_pair_add(walk, &walk->ai_uses, &walk->ai_use_count, &walk->ai_use_capacity,
                walk->ai_state == 2 ? SIZE_MAX : walk->current, behavior);
        if (walk->ai_state == 2) walk->ai_override = 1;
    }
    if (ok && walk->spawner_field) {
        size_t entry = sg_queued(walk, reference.resource.type, reference.resource.name);
        if (entry != SIZE_MAX)
            ok = sg_spawner_use_add(walk, walk->spawner_state == 2 ? SIZE_MAX : walk->current, entry,
                walk->spawner_state == 2 ? walk->spawner_copy : SIZE_MAX, walk->spawner_field);
    }
    walk->inherit_read = walk->ai_field = walk->spawner_field = 0;
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

/* A map's light probe is a file-family resource at a path its consumer
 * derives from the map and probe entity names (VK 0x1405b2bf0 / GL
 * 0x1405b22e0 set explicitLightProbePath). The environment probe loader (VK
 * 0x140dc6b80 / GL 0x140dbb6d0) reads it through
 * idBinaryImage::LoadFromGeneratedFile (VK 0x140d6c700 / GL 0x140d6bad0),
 * which reads a timestamp, the BIM magic, numeric header fields and, per face
 * and level, five integers and the pixels: no name, ID or reference. All 3086
 * installed file-family .bimage rows there are that one 132018-byte format.
 * The family's other .bimage rows, under generated/swf/, are movie atlases
 * (package_movie.c), which this rule does not cover.
 *
 * A module's sound BSP and ambient lighting are file-family resources at
 * <module>/<base>.sbsp and .ambientsh, named from the module's path
 * (BuildSoundWorld VK 0x1404f0670 / GL 0x1404f0040; the placed-module walk
 * VK 0x1404ec690 / GL 0x1404ec060). idSoundBSPFile::Read (VK 0x14184adc0 /
 * GL 0x14183db60) reads planes, vertices, indices and topology; the ambient
 * reader (VK 0x140d697f0 / GL 0x140d69070) reads nodes and samples of
 * coefficients and positions. Neither reads a name, and none of the 232
 * installed files of either kind contains resource-path text.
 *
 * Its occlusion tome is <module>/_combo/_world.tome, which InitFromMap (VK
 * 0x14174fe70 / GL 0x141742b10) hands to Umbra (SetTome VK 0x140e0a630 / GL
 * 0x140dfeb90). The 283 installed tomes (586 MB) share one header magic and
 * none contains a resource root or extension token. */
static int sg_module_member_leaf(const char *name, size_t length)
{
    static const char *const extensions[] = {".sbsp", ".ambientsh"};
    const char *role = NULL;
    if (sh_package_module_member(name, NULL, 0, &role) && !strcmp(role, "occlusion")) return 1;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        size_t ext = strlen(extensions[i]), stem;
        const char *base, *parent;
        if (length <= 13 + ext || strcmp(name + length - ext, extensions[i])) continue;
        base = strrchr(name, '/') + 1;
        stem = (size_t)(name + length - ext - base);
        if (!stem || base - 1 <= name + 13) return 0;
        for (parent = base - 1; parent > name && parent[-1] != '/'; parent--) {}
        return (size_t)(base - 1 - parent) == stem && !strncmp(parent, base, stem);
    }
    return 0;
}

int sh_package_source_leaf(const char *family, const char *name)
{
    const char *base, *parent;
    size_t length = name ? strlen(name) : 0;
    if (!family || !name) return 0;
    if (sh_package_source_leaf_family(family)) return 1;
    if (strcmp(family, "file")) return 0;
    if (!strncmp(name, "maps/modules/", 13) && sg_module_member_leaf(name, length)) return 1;
    if (length <= 5 + 7 || strncmp(name, "maps/", 5) || strcmp(name + length - 7, ".bimage")) return 0;
    base = strrchr(name, '/') + 1;
    if (base == name + length - 7) return 0;
    for (parent = base - 1; parent > name && parent[-1] != '/'; parent--) {}
    return base - 1 - parent == 11 && !strncmp(parent, "lightprobes", 11);
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

/* An entityDef's own positional poolCount header; Parse does not inherit it.
 * With a positive value, generic spawn takes a pool route in pool mode 1 or 2
 * (VK 140344f20 / GL 140344e60). Per-definition pool creation (VK 1408293d0 /
 * GL 140828220) constructs one template while the cvar
 * g_entityPoolTypeInfoClone (default 0) is set, and mode 2's per-type pools
 * build one per definition (VK 1408291e0 / GL 140828030); both read its
 * prepared state (+0x140) into CreateManagedClassFromDecl's result without a
 * NULL check, while the individual-spawn branch skips NULL. 1 positive, 0
 * absent or not positive, -1 not a plain integer. */
static int sg_own_pool(const sh_decl_node *tree)
{
    sh_decl_entity_headers headers;
    const char *text = NULL; size_t length = 0, at = 0;
    long long value = 0;
    sh_decl_entity_split(tree, &headers);
    if (!headers.pool_count) return 0;
    if (headers.pool_count->compound || headers.pool_count->reset ||
        !sh_decl_tree_literal(headers.pool_count, &text, &length) || !length) return -1;
    if (text[0] == '-') at = 1;
    if (at == length) return -1;
    for (; at < length; at++) {
        if (text[at] < '0' || text[at] > '9') return -1;
        if (value < 1000000000) value = value * 10 + (text[at] - '0');
    }
    return text[0] != '-' && value > 0;
}

static int sg_inspect(sg_walk *walk, const char *type, const char *name)
{
    sh_decl_source source = {0};
    sh_decl_node *tree = NULL, *own_tree = NULL; /* own_tree: an entity declaration's own text once expanded */
    sh_decl_graph *graph = NULL;
    sh_decl_value_type state = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_entity_class_source classes = {walk, sg_read_parent, sg_derives, sg_known};
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
        walk->current < walk->candidate_capacity && !(walk->candidate[walk->current] & SG_CANDIDATE) && !compiled_resource;
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
        int leaf = sh_package_source_leaf(type, name), inspected = 0;
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
    /* An entity declaration's repeated member is native-valid: its
     * occurrences stay in order and the expanded state applies the merge. */
    int entity = !strcmp(type, "entitydef");
    tree = sg_open_body(type) ? sh_decl_tree_parse_reflected_body(source, entity, NULL, 0) :
        entity ? sh_decl_tree_parse_ordered(source, NULL, 0) : sh_decl_tree_parse(source, NULL, 0);
    if (!tree) { sg_gap(walk, "", type, "declaration source grammar is unsupported"); goto done; }
    if (!strcmp(type, "entitydef")) {
        char class_error[1024] = "";
        sh_decl_entity_merge_failure class_failure = SH_DECL_ENTITY_MERGE_DECIDED;
        char *unreflected = NULL;
        int pooled = sg_own_pool(tree);
        class_name = sh_decl_entity_tree_class_detailed(tree, &classes, &class_failure, &unreflected,
            class_error, sizeof(class_error));
        /* Validity follows Parse; construction follows the idClass registry
         * (sg_judge). Unused, only inherited, or only named passively, a
         * declaration stays valid and served whatever its class. */
        if (class_name || class_failure == SH_DECL_ENTITY_MERGE_COVERAGE)
            sg_judge(walk, walk->current, class_name ? class_name : unreflected, pooled, compiled_resource != NULL);
        free(unreflected);
        if (walk->failed) { ok = 0; goto done; }
        if (pooled < 0 && (class_failure == SH_DECL_ENTITY_MERGE_COVERAGE || (walk->current < walk->candidate_capacity &&
                (walk->candidate[walk->current] & (SG_UNBUILDABLE | SG_CLASS_UNAVAILABLE)))))
            sg_gap(walk, "poolCount", type, "its own poolCount is not a plain integer, so whether a spawn by this "
                "name reaches the unchecked pool template constructors is undecided");
        if (!class_name) {
            sg_gap(walk, "class", type, class_error);
            /* Its opaque state names nothing known, but Parse still reads the
             * parent its served inherit binds: that parent is in the closure. */
            if (class_failure != SH_DECL_ENTITY_MERGE_COVERAGE) goto done;
            (void)sh_decl_entity_header_dependencies(tree, sh_decl_native_schema_view(walk->schema), &result);
            goto inspected;
        }
        /* An inherited state outside the verified merge model (the binding
         * route's product coverage, served as authored) selects no decided
         * statement: every block is a possible dependency, and the closure is
         * never a complete actual use. */
        sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
        char *selected = NULL;
        int every = sh_decl_entity_merges_classified(tree, &classes, &selected, &failure,
            class_error, sizeof(class_error)) < 0 && failure == SH_DECL_ENTITY_MERGE_COVERAGE;
        free(selected);
        int expanded = 0;
        if (every) sg_gap(walk, "edit", type, class_error);
        /* Otherwise only the prepared text's first statement is state, whatever
         * its name, with or without a positional parent; later statements and
         * an unconsumed inherit are never read, so they are no dependencies. */
        else {
            sh_decl_node *effective = sh_decl_entity_expanded_state(tree, &classes, class_error, sizeof(class_error));
            if (effective) { own_tree = tree; tree = effective; expanded = 1; }
            else sg_gap(walk, "edit", type, class_error);
        }
        state = (sh_decl_value_type){class_name, ""};
        walk->ai_state = sg_derives(walk, class_name, "idAI2") == 1;
        walk->spawner_state = sg_derives(walk, class_name, SG_SPAWNER_CLASS) == 1;
        /* Whether its own spawn sets both pointers (sg_spawner_routes): the
         * expanded state is the whole merged pair; a state that is not
         * expanded is taken as set. */
        if (walk->spawner_state && !sg_pair_add(walk, &walk->spawner_pairs, &walk->spawner_pair_count,
                &walk->spawner_pair_capacity, walk->current, !expanded ? 3 : (size_t)(sg_pair_state(
                sh_decl_tree_member(tree, "edit")) & 3))) { walk->ai_state = walk->spawner_state = 0; ok = 0; goto done; }
        if (every) (void)sh_decl_entity_every_state_dependencies(tree, state, sh_decl_native_schema_view(walk->schema), &result);
        else (void)sh_decl_entity_dependencies(tree, state, sh_decl_native_schema_view(walk->schema), &result);
        walk->ai_state = walk->spawner_state = 0;
        /* Its class reads, under its own type, the members its parent's
         * class does not reflect (sg_inherited_readers), unless its own
         * layer sets them. */
        sh_decl_entity_headers headers = {0};
        if (own_tree) sh_decl_entity_split(own_tree, &headers);
        const sh_decl_node *parent_node = own_tree && sh_decl_entity_expands(own_tree) == 1 ? headers.inherit : NULL;
        const char *parent_name; size_t parent_length;
        if (parent_node && !walk->failed && !result.aborted &&
            sh_decl_reference_literal(parent_node, &parent_name, &parent_length) && parent_length) {
            char *inherit = malloc(parent_length + 1), reason[512] = "";
            if (!inherit) { walk->failed = 1; ok = 0; goto done; }
            memcpy(inherit, parent_name, parent_length); inherit[parent_length] = 0;
            const sh_decl_node **own_states = NULL;
            size_t own_count = 0;
            if (sg_own_states(&headers, &own_states, &own_count) < 0) { free(inherit); walk->failed = 1; ok = 0; goto done; }
            const char *current_type = walk->current_type, *current_name = walk->current_name;
            int origin = walk->native_origin;
            int status = memchr(inherit, '"', parent_length) ? 0 : sg_inherited_readers(walk, &classes, class_name, inherit,
                own_states, own_count, NULL, reason, sizeof(reason), 0);
            free((void *)own_states);
            walk->current_type = current_type; walk->current_name = current_name; walk->native_origin = origin;
            if (status < 0 && !walk->failed) {
                char text[640];
                snprintf(text, sizeof(text), "the members its class reads from its parent's state are undecided "
                    "(product coverage): %s", reason[0] ? reason : "its inherited state is not modelled");
                sg_gap(walk, "edit", type, text);
            }
            free(inherit);
            if (walk->failed) { ok = 0; goto done; }
        }
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
        int pools = !strcmp(type, "aipoolnumbers");
        walk->pool_listing = pools;
        walk->behavior_state = !strcmp(type, "aibehavior");
        if (edit) (void)sh_decl_state_dependencies(edit, state, sh_decl_native_schema_view(walk->schema), &result);
        walk->pool_listing = walk->behavior_state = 0;
        if (pools && !walk->failed) sg_pool_lists(walk, edit, type);
        if (!strcmp(type, "aibehavior") && !walk->failed) sg_behavior_lists(walk, edit);
    }
inspected:
    if (result.aborted) { sg_gap(walk, "", type, "source dependency inspection aborted"); if (walk->failed) ok = 0; }
done:
    walk->native_origin = walk->native_leaf = 0;
    free(path); free(body); free(class_name); sh_decl_tree_free(tree); sh_decl_tree_free(own_tree); sh_decl_graph_close(graph);
    return ok && !walk->failed;
}

/* Construction of an identity the candidate names or serves. Routes:
 * - world AI pool setup (VK 0x14069d930, while ai_pool_enable) requests a
 *   pool for each enabled spawnPoolEntries row with its poolSize and each
 *   projectileEnts entry with -1; the request (VK 0x140332b50 / GL
 *   0x1403328d0) creates one only for a positive size or the definition's
 *   own positive poolCount (+0x10c), so a positive poolSize is a
 *   construction without an own poolCount and a listing without one needs
 *   it (proven consumers);
 * - a spawn by name takes an own positive poolCount into a pool in pool mode
 *   1 or 2 (VK 0x140344f20); a constructed SnapMap spawner's
 *   spawnerEntityPair is such a spawn (sg_spawner_routes, proven); another
 *   non-inherit reference is an untraced consumer: undecided, never
 *   labelled a proven spawn;
 * - the AI spawn (VK 0x140654010 / GL 0x140653d40) pre-pools its
 *   behavior's pooledProjectileEnts with size -1 unless the load-state
 *   getter (VK 0x1419fc870 / GL 0x1419eeb00, the load_state global)
 *   returns 1, which no production transition writes (sg_behavior_lists).
 * Pool creation (VK 0x1408293d0 / GL 0x140828220) builds a template while
 * g_entityPoolTypeInfoClone is set, and mode 2 always builds per-type ones
 * (VK 0x1408291e0 / GL 0x140828030); both pass CreateManagedClassFromDecl's
 * result to SetEntityEditState without a NULL check. Otherwise pool creation
 * spawns each entity individually and skips a NULL result.
 *
 * The walk judges the settings of the process that loads the map
 * (sh_native_pool_settings_read, once per walk): a route whose effective
 * branch cannot reach an unchecked template is a valid native path, never
 * refused because another setting would take a different branch. Unread
 * settings leave every possible route undecided (environment). Converted
 * map data stays equivalent under every setting; this only decides whether
 * this load could fault. */
static const sh_native_pool_settings *sg_pool_settings(sg_walk *walk)
{
    if (!walk->pool_settings_state)
        walk->pool_settings_state = sh_native_pool_settings_read(walk->native.classes, &walk->pool_settings) ? 1 : -1;
    return walk->pool_settings_state > 0 ? &walk->pool_settings : NULL;
}
static void sg_construction_check(sg_walk *walk, size_t index)
{
    unsigned short bits = index < walk->candidate_capacity ? walk->candidate[index] : 0;
    const char *type = walk->current_type, *name = walk->current_name, *verdict, *field;
    const sh_native_pool_settings *settings;
    int origin = walk->native_origin, leaf = walk->native_leaf;
    char reason[1024], context[160] = "";
    if (!(bits & SG_INSPECTED) || !(bits & (SG_UNBUILDABLE | SG_CLASS_UNAVAILABLE)) ||
        !(bits & (SG_CANDIDATE | SG_SERVED)) || !(verdict = walk->verdict[index])) return;
    int world = (bits & SG_WORLD_SIZED) || ((bits & SG_POOLED) && (bits & SG_WORLD_LISTED));
    int prepool = (bits & SG_POOLED) && (bits & SG_AI_PREPOOL);
    int untraced = (bits & SG_POOLED) && (bits & SG_NAMED);
    int spawned = (bits & SG_POOLED) && (bits & SG_SPAWNER_SPAWNED);
    int untraced_prepool = (bits & SG_POOLED) && (bits & SG_AI_UNTRACED);
    if ((settings = sg_pool_settings(walk)) != NULL) {
        /* Only branches this process takes: world setup needs ai_pool_enable,
         * pool creation's template the clone setting, and a spawn by name a
         * per-type pool (mode 2) or a per-definition one that clones (mode 1). */
        if (!settings->ai_pool_enable || !settings->type_info_clone) world = 0;
        if (!settings->type_info_clone) prepool = untraced_prepool = 0;
        if (settings->pool_mode != 2 && (settings->pool_mode != 1 || !settings->type_info_clone)) untraced = spawned = 0;
        snprintf(context, sizeof(context), " (g_entityPoolTypeInfoClone %d, g_entityPoolMode %d, ai_pool_enable %d)",
            settings->type_info_clone, settings->pool_mode, settings->ai_pool_enable);
    }
    int sized = world && (bits & SG_WORLD_SIZED), listed = world && !sized;
    int prepooled = !world && prepool;
    spawned = !world && !prepooled && spawned;
    int proven = world || prepooled || spawned;
    int named = untraced && !untraced_prepool;
    untraced = untraced || untraced_prepool;
    /* A proven route discovered after an undecided one still records its own
     * gap, so a late world or behavior listing is never hidden. */
    if (proven ? (bits & SG_PROVEN_GAP) != 0 : (!untraced || (bits & SG_POOL_GAP))) return;
    field = sized ? "poolSize" : "poolCount";
    if ((bits & SG_CLASS_UNAVAILABLE) || !settings)
        snprintf(reason, sizeof(reason), "construction is undecided (environment): %s%s, and %s", verdict,
            settings ? "" : "; the effective entity pool settings (g_entityPoolTypeInfoClone, g_entityPoolMode, "
            "ai_pool_enable) are unavailable",
            sized ? "an enabled AI spawn pool lists it with a positive poolSize" : listed ?
            "an enabled AI spawn pool lists it and its own positive poolCount creates the pool" : prepooled ?
            "an AI behavior's pooledProjectileEnts lists it and its own positive poolCount creates the pool" : spawned ?
            "a SnapMap spawner the map constructs spawns it by name (spawnerEntityPair) and its own positive poolCount "
            "takes that spawn into a pool" : named ?
            "a reference names it and its own positive poolCount would take a spawn by this name into a pool" :
            "an AI behavior's pooledProjectileEnts lists it and its own positive poolCount would create the pool if "
            "an AI that spawns with that behavior is constructed");
    else if (prepooled)
        snprintf(reason, sizeof(reason), "impossible construction, not an inert use: %s, so CreateManagedClassFromDecl "
            "returns NULL; an AI behavior's pooledProjectileEnts lists it and its own positive poolCount creates its pool "
            "when that AI spawns (VK 0x140654010 / GL 0x140653d40 pre-pools each entry with size -1; the load-state "
            "gate never skips it in production), and pool creation's template branch passes that NULL to "
            "SetEntityEditState without a check%s", verdict, context);
    else if (spawned)
        snprintf(reason, sizeof(reason), "impossible construction, not an inert use: %s, so CreateManagedClassFromDecl "
            "returns NULL; a SnapMap spawner the map constructs spawns it by name when it spawns (spawnerEntityPair, "
            "VK 0x141284120 / GL 0x1412760b0, through the game spawn VK 0x140344f20 / GL 0x140344e60), its own "
            "positive poolCount takes that spawn into a pool, and pool creation's template branch passes that NULL to "
            "SetEntityEditState without a check%s", verdict, context);
    else if (sized || listed)
        snprintf(reason, sizeof(reason), "impossible construction, not an inert use: %s, so CreateManagedClassFromDecl "
            "returns NULL; %s (world pool setup), and pool creation's template branch passes that NULL to "
            "SetEntityEditState without a check%s", verdict,
            sized ? "an enabled AI spawn pool lists it with a positive poolSize, which creates its pool without an own "
            "poolCount" : "an enabled AI spawn pool lists it and its own positive poolCount creates its pool", context);
    else if (named)
        snprintf(reason, sizeof(reason), "construction is undecided (product coverage): %s; a reference whose native "
            "consumer is not traced names it, and if that consumer spawns it by name, its own positive poolCount takes "
            "the spawn into a pool whose template constructor uses the instance without a NULL check%s", verdict, context);
    else
        snprintf(reason, sizeof(reason), "construction is undecided (product coverage): %s; an AI behavior's "
            "pooledProjectileEnts lists it, and whether an AI that spawns with that behavior is constructed is not "
            "traced (the behavior or its AI is named only by an untraced consumer); if one is, its own positive "
            "poolCount creates a pool whose template constructor uses the instance without a NULL check%s", verdict, context);
    walk->candidate[index] |= proven ? SG_POOL_GAP | SG_PROVEN_GAP : SG_POOL_GAP;
    walk->current_type = walk->queue.items[index].type; walk->current_name = walk->queue.items[index].name;
    walk->native_origin = walk->native_leaf = 0;
    sg_gap(walk, field, walk->current_type, reason);
    walk->current_type = type; walk->current_name = name;
    walk->native_origin = origin; walk->native_leaf = leaf;
}

/* Judge an inspected entityDef's class with the idClass registry the native
 * constructor reads. Without an attached registry (offline inspection)
 * construction is not judged; an unreadable one is never a verdict. */
static void sg_judge(sg_walk *walk, size_t index, const char *class_name, int pooled, int served)
{
    const sh_native_class_source *classes = walk->native.classes;
    sh_native_construction construction;
    char text[320];
    if (index >= walk->candidate_capacity) return;
    if (pooled > 0) walk->candidate[index] |= SG_POOLED;
    if (served) walk->candidate[index] |= SG_SERVED;
    if (!classes || (walk->candidate[index] & SG_INSPECTED)) return;
    construction = sh_native_class_construction(classes, class_name, NULL);
    walk->candidate[index] |= SG_INSPECTED;
    if (construction == SH_NATIVE_CONSTRUCTION_ELIGIBLE) return;
    walk->candidate[index] |= construction == SH_NATIVE_CONSTRUCTION_UNAVAILABLE ? SG_CLASS_UNAVAILABLE : SG_UNBUILDABLE;
    snprintf(text, sizeof(text), "class '%.120s' %s", class_name ? class_name : "", sh_native_construction_text(construction));
    free(walk->verdict[index]);
    if (!(walk->verdict[index] = _strdup(text))) { walk->failed = 1; return; }
    sg_construction_check(walk, index);
}

/* The entityDef lists of an AI spawn pool declaration (aipoolnumbers), read
 * as world pool setup reads them (sg_construction_check). Its
 * suppressedEntityDefNames only exempt names from the pool base lookup
 * (VK 0x140691a00 / GL 0x140690980): identity, never construction. A
 * disabled pool lists nothing for construction. Members outside this model
 * stay gaps. */
static size_t sg_pool_entry(sg_walk *walk, const char *name)
{
    /* The list's consumer requests the pool by this definition: queue it as
     * an entityDef dependency (never a use by name). */
    char *normalized = sg_name(name);
    size_t index;
    int listing = walk->pool_listing;
    if (!normalized) { walk->failed = 1; return SIZE_MAX; }
    walk->pool_listing = 1;
    if (*normalized && !sg_add(walk, "entitydef", normalized)) { free(normalized); walk->pool_listing = listing; return SIZE_MAX; }
    walk->pool_listing = listing;
    index = sg_set_find(&walk->queue_set, &walk->queue, "entitydef", normalized);
    free(normalized);
    return index < walk->candidate_capacity ? index : SIZE_MAX;
}
static void sg_pool_mark(sg_walk *walk, const char *name, unsigned short bit)
{
    size_t index = sg_pool_entry(walk, name);
    if (index == SIZE_MAX) return;
    walk->candidate[index] |= bit;
    sg_construction_check(walk, index);
}
static int sg_plain_integer(const sh_decl_node *node, long long *value)
{
    const char *text; size_t length, at = 0;
    *value = 0;
    if (!node || node->compound || node->reset || !sh_decl_tree_literal(node, &text, &length) || !length) return 0;
    if (text[0] == '-') at = 1;
    if (at == length) return 0;
    for (; at < length; at++) {
        if (text[at] < '0' || text[at] > '9') return 0;
        if (*value < 1000000000) *value = *value * 10 + (text[at] - '0');
    }
    if (text[0] == '-') *value = -*value;
    return 1;
}
/* World pool setup reads the pool the world selects: SnapMap's play world is
 * the literal { inherit = "worldspawn"; edit = { } } (VK 0x1404ee8e0 / GL
 * 0x1404ee2b0), whose expanded chain's last aiSpawnPoolDecl names it (the
 * idWorldspawn constructor leaves it unset; package_native_inline.c
 * ni_pool_resolve). Another aipoolnumbers declaration lists nothing world
 * setup constructs. */
static int sg_world_pool(sg_walk *walk)
{
    static const char world[] = "{ inherit = \"worldspawn\"; edit = { } }";
    sh_decl_entity_class_source classes = {walk, sg_read_parent, sg_derives, sg_known};
    char error[256] = "";
    sh_decl_node *literal, *tree;
    if (walk->world_pool) return walk->world_pool;
    walk->world_pool = -1;
    literal = sh_decl_tree_parse_ordered((sh_decl_source){world, sizeof(world) - 1}, error, sizeof(error));
    tree = literal ? sh_decl_entity_expanded_state(literal, &classes, error, sizeof(error)) : NULL;
    sh_decl_tree_free(literal);
    if (walk->failed) { sh_decl_tree_free(tree); return -1; }
    if (!tree || (tree->children && (!tree->children->compound || tree->children->reset))) {
        snprintf(walk->world_pool_reason, sizeof(walk->world_pool_reason), "the world chain is not modelled: %.150s",
            tree ? "it replaces its inherited state" : error);
        sh_decl_tree_free(tree); return -1;
    }
    for (const sh_decl_node *decl = tree->children ? tree->children->children : NULL; decl; decl = decl->next) {
        const char *text; size_t length;
        if (strcmp(decl->key, "aiSpawnPoolDecl")) continue;
        free(walk->world_pool_name); walk->world_pool_name = NULL;
        if (decl->compound || decl->reset || !sh_decl_tree_literal(decl, &text, &length)) {
            snprintf(walk->world_pool_reason, sizeof(walk->world_pool_reason), "the world's aiSpawnPoolDecl is not a plain name");
            sh_decl_tree_free(tree); return -1;
        }
        if (length && !(walk->world_pool_name = malloc(length + 1))) { walk->failed = 1; sh_decl_tree_free(tree); return -1; }
        if (length) { memcpy(walk->world_pool_name, text, length); walk->world_pool_name[length] = 0; }
    }
    sh_decl_tree_free(tree);
    return walk->world_pool = 1;
}
/* The AI spawn's pre-pool list (AI +0xc120 -> +0x3b0/+0x3b8) is the
 * idDeclAIBehavior field pooledProjectileEnts: the reflected record on both
 * images places that idList<const idDeclEntityDef *> at +0x3b0 and
 * monsterType, which the same spawn compares at +0x120, at +0x120. Each
 * entry gets a size -1 pool request: a pool only for its own poolCount.
 * Entries are queued here and routed once every use of the behavior is
 * known (sg_ai_routes): inspecting a behavior is not an AI using it. */
static void sg_behavior_lists(sg_walk *walk, const sh_decl_node *edit)
{
    const sh_decl_node *list = edit ? sh_decl_tree_member(edit, "pooledProjectileEnts") : NULL;
    const char *text; size_t length;
    if (!list || !list->compound || list->reset) return;
    for (const sh_decl_node *item = list->children; item && !walk->failed; item = item->next) {
        char *name;
        if (!strcmp(item->key, "num") || item->compound || item->reset ||
            !sh_decl_tree_literal(item, &text, &length) || !length) continue;
        if (!(name = malloc(length + 1))) { walk->failed = 1; return; }
        memcpy(name, text, length); name[length] = 0;
        size_t entry = sg_pool_entry(walk, name);
        free(name);
        if (entry != SIZE_MAX && walk->current < walk->candidate_capacity)
            (void)sg_pair_add(walk, &walk->ai_entries, &walk->ai_entry_count, &walk->ai_entry_capacity, walk->current, entry);
    }
}
static void sg_pool_lists(sg_walk *walk, const sh_decl_node *edit, const char *type)
{
    const char *text; size_t length;
    int selected = sg_world_pool(walk);
    if (walk->failed) return;
    if (selected < 0) {
        if (walk->world_gap) return;
        sg_gap(walk, "aiSpawnPoolDecl", type, walk->world_pool_reason[0] ? walk->world_pool_reason :
            "the world's AI spawn pool selection is not modelled, so whether world pool setup reads this list is undecided");
        return;
    }
    if (!walk->world_pool_name || _stricmp(walk->world_pool_name, walk->current_name)) return;
    for (const sh_decl_node *member = edit ? edit->children : NULL; member; member = member->next) {
        if (strcmp(member->key, "enabled")) continue;
        if (member->compound || member->reset || !sh_decl_tree_literal(member, &text, &length) ||
            !((length == 1 && (*text == '0' || *text == '1')) || (length == 4 && !memcmp(text, "true", 4)) ||
              (length == 5 && !memcmp(text, "false", 5)))) {
            sg_gap(walk, "enabled", type, "AI spawn pool enabled is not a plain flag, so whether its lists construct is undecided");
            return;
        }
        if (*text == '0' || *text == 'f') return;
    }
    for (const sh_decl_node *member = edit ? edit->children : NULL; member && !walk->failed; member = member->next) {
        int rows = !strcmp(member->key, "spawnPoolEntries"), projectiles = !strcmp(member->key, "projectileEnts");
        if (!strcmp(member->key, "enabled") || !strcmp(member->key, "suppressedEntityDefNames")) continue;
        if ((!rows && !projectiles) || !member->compound || member->reset) {
            sg_gap(walk, member->key, type, "AI spawn pool member is not modelled, so whether it constructs an entityDef is undecided");
            continue;
        }
        for (const sh_decl_node *item = member->children; item && !walk->failed; item = item->next) {
            const sh_decl_node *def = item;
            long long size = 0;
            char *name;
            if (!strcmp(item->key, "num")) continue;
            if (rows) {
                if (!item->compound) { sg_gap(walk, member->key, type, "AI spawn pool row is not a record"); continue; }
                def = sh_decl_tree_member(item, "entityDef");
                if (!def) continue;
                if (sh_decl_tree_member(item, "poolSize") && !sg_plain_integer(sh_decl_tree_member(item, "poolSize"), &size)) {
                    sg_gap(walk, "poolSize", type, "AI spawn pool poolSize is not a plain integer, so whether it creates a pool is undecided");
                    continue;
                }
            }
            if (def->compound || def->reset || !sh_decl_tree_literal(def, &text, &length) || !length) continue;
            if (!(name = malloc(length + 1))) { walk->failed = 1; return; }
            memcpy(name, text, length); name[length] = 0;
            sg_pool_mark(walk, name, size > 0 ? SG_WORLD_SIZED : SG_WORLD_LISTED);
            free(name);
        }
    }
}

/* The play world is a root of every map: world AI pool setup reads the pool
 * its chain selects (sg_world_pool) whether or not the map names it. Only
 * that declaration is queued, as the world's selection rather than a use by
 * name, under the name the bound sources serve (a package variant included).
 * The world chain itself and worldspawn's own precache and base
 * dependencies stay passive, a stock pool stays the game's business, and
 * other pools are not reached. A walk without the idClass registry judges no
 * construction and adds nothing; an unmodelled chain is accounted where a
 * pool declaration is inspected (sg_pool_lists). */
static int sg_world_root(sg_walk *walk)
{
    int selected, ok, served = 0;
    if (!walk->native.classes) return 1;
    walk->current_type = "entitydef"; walk->current_name = "worldspawn";
    walk->native_origin = 1;
    selected = sg_world_pool(walk);
    walk->native_origin = 0;
    if (walk->failed) return 0;
    /* An unreadable chain already failed the walk (sg_read_path), which keeps
     * the prior generation. An unmodelled one selects no decided pool: while
     * the library serves the chain or any AI spawn pool declaration, the
     * selection may reach package content, so it is a gap rather than no
     * root. Otherwise the selection stays the game's own. */
    for (size_t i = 0; selected < 0 && i < walk->compiled->resource_count; i++) {
        const sh_compiled_resource *r = walk->compiled->resources + i;
        if (!r->restored_original && r->source_count && r->type && r->name && (!_stricmp(r->type, "aipoolnumbers") ||
            (!_stricmp(r->type, "entitydef") && !_stricmp(r->name, "worldspawn")))) served = 1;
    }
    if (served) {
        char reason[sizeof(walk->world_pool_reason) + 160];
        snprintf(reason, sizeof(reason), "%s, so which AI spawn pool world setup reads is undecided (product coverage)",
            walk->world_pool_reason[0] ? walk->world_pool_reason : "the world's AI spawn pool selection is not modelled");
        sg_gap(walk, "aiSpawnPoolDecl", "entitydef", reason);
        walk->world_gap = 1;
    }
    if (selected < 0 || !walk->world_pool_name || !*walk->world_pool_name) return 1;
    walk->native_origin = walk->pool_listing = 1;
    ok = sg_add(walk, "aipoolnumbers", walk->world_pool_name);
    walk->native_origin = walk->pool_listing = 0;
    return ok;
}

/* Route each behavior's pooledProjectileEnts by the AIs that use it, once
 * the whole closure is known (a late use still counts): a behavior that a
 * constructed placed idAI2 copy spawns with, itself or through the parent
 * whose state it keeps (SG_AI_SPAWNED), pre-pools its entries (proven);
 * one selected by an AI named only by an untraced consumer (by name, a pool
 * list or another pre-pool), or itself named by another reference such as
 * an event argument or an unrelated field, may (undecided); one only read
 * through inherit, or selected only by unused or unconstructible AIs, is
 * passive. An entry that is itself an AI passes its route on. */
static int sg_ai_routes(sg_walk *walk)
{
    int changed = 1, any = 0;
    for (size_t pass = 0; changed && pass <= walk->queue.count; pass++) {
        changed = 0;
        for (size_t i = 0; i < walk->ai_use_count; i++) {
            size_t ai = walk->ai_uses[i].first, behavior = walk->ai_uses[i].second;
            unsigned short spawn = ai == SIZE_MAX ? SG_AI_SPAWNED : walk->candidate[ai], add =
                (spawn & (SG_AI_SPAWNED | SG_SPAWNER_SPAWNED)) ? SG_AI_USE :
                (spawn & (SG_NAMED | SG_WORLD_SIZED | SG_WORLD_LISTED | SG_AI_PREPOOL | SG_AI_UNTRACED)) ? SG_AI_UNTRACED : 0;
            if (add && (walk->candidate[behavior] & add) != add) { walk->candidate[behavior] |= add; changed = 1; }
        }
        for (size_t i = 0; i < walk->ai_entry_count; i++) {
            unsigned short behavior = walk->candidate[walk->ai_entries[i].first], add =
                (behavior & SG_AI_USE) ? SG_AI_PREPOOL | SG_NAMED :
                (behavior & (SG_AI_UNTRACED | SG_NAMED)) ? SG_AI_UNTRACED | SG_NAMED : 0;
            size_t entry = walk->ai_entries[i].second;
            /* A used behavior's AI also fires these entries; that spawn by
             * name is an untraced consumer (SG_NAMED). */
            if (add && (walk->candidate[entry] & add) != add) { walk->candidate[entry] |= add; changed = 1; }
        }
        any |= changed;
    }
    for (size_t i = 0; i < walk->ai_entry_count && !walk->failed; i++) sg_construction_check(walk, walk->ai_entries[i].second);
    return any;
}

/* Route each definition a spawner's spawnerEntityPair names by the spawner's
 * construction, once the closure is known. The spawn (VK 0x141284120 / GL
 * 0x1412760b0, paired line for line) checks no class: it requires both
 * fields set, selects entityStatic (placed at the spawner through the
 * spawner's own physics) or entityPinata (placed at the activator and
 * launched through the spawned entity's physics) by the Spawn Object
 * action's flag, and spawns it through the game spawn (vtable +0x450, VK
 * 0x140344f20 / GL 0x140344e60), which takes an own positive poolCount into
 * a pool in pool mode 1 or 2 (sg_construction_check). A spawner whose
 * effective state leaves either pointer NULL spawns nothing (sg_pair_state):
 * both names stay dependencies the read looks up, but neither is constructed
 * by it, so an impossible construction there is dormant. A constructed placed
 * spawner copy that sets both spawns its own values and, for a field it does
 * not set, its parent's; a spawner definition that sets both and that a proven
 * spawner spawn constructs spawns its own (proven, SG_SPAWNER_SPAWNED). One
 * named by an untraced consumer, a pool list or a pre-pool may (undecided,
 * SG_NAMED); one only read through inherit, or a copy that is never
 * constructed, spawns nothing. Which field an action selects and whether a
 * trigger fires at play time stay authored behavior, never a reason to drop
 * either value. Returns whether a route changed. */
static int sg_spawner_routes(sg_walk *walk)
{
    int changed = 1, any = 0;
    for (size_t pass = 0; changed && pass <= walk->queue.count; pass++) {
        changed = 0;
        for (size_t i = 0; i < walk->spawner_use_count; i++) {
            const struct sg_spawner_use *use = walk->spawner_uses + i;
            unsigned short spawn = 0, add;
            if (use->definition == SIZE_MAX)
                spawn = use->copy < walk->spawner_copy_count && walk->spawner_copies[use->copy].set == 3 ? SG_SPAWNER_SPAWNED : 0;
            else for (size_t p = 0; p < walk->spawner_pair_count; p++)
                if (walk->spawner_pairs[p].first == use->definition && walk->spawner_pairs[p].second == 3)
                    spawn = walk->candidate[use->definition];
            for (size_t c = 0; use->definition != SIZE_MAX && c < walk->spawner_copy_count; c++)
                if (walk->spawner_copies[c].parent == use->definition && (walk->spawner_copies[c].unset & use->field) &&
                    walk->spawner_copies[c].set == 3)
                    spawn |= SG_SPAWNER_SPAWNED;
            add = (spawn & SG_SPAWNER_SPAWNED) ? SG_SPAWNER_SPAWNED :
                (spawn & (SG_NAMED | SG_WORLD_SIZED | SG_WORLD_LISTED | SG_AI_PREPOOL | SG_AI_UNTRACED)) ? SG_NAMED : 0;
            if (add && (walk->candidate[use->entry] & add) != add) { walk->candidate[use->entry] |= add; changed = 1; }
        }
        any |= changed;
    }
    for (size_t i = 0; i < walk->spawner_use_count && !walk->failed; i++) sg_construction_check(walk, walk->spawner_uses[i].entry);
    return any;
}

/* Map roots through their exact JSON spans: a placement's "inherit" is its
 * own declaration's parent, read through inherit (placed copies carry no
 * poolCount); "value" names the definition itself. An unrecognized key
 * spelling is a use by name. */
static int sg_map_site(void *context, const char *native_class, const char *type, const char *name,
    const char *json, size_t length)
{
    sg_walk *walk = context;
    const char *at = json;
    size_t back = walk->map_text && json >= walk->map_text ? (size_t)(json - walk->map_text) : 0;
    (void)native_class; (void)length;
    if (back && *json != '"' && at[-1] == '"') { at--; back--; }
    while (back && (at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\r' || at[-1] == '\n')) { at--; back--; }
    if (back && at[-1] == ':') { at--; back--; }
    while (back && (at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\r' || at[-1] == '\n')) { at--; back--; }
    walk->inherit_read = back >= 9 && !memcmp(at - 9, "\"inherit\"", 9);
    int ok = sg_add(walk, type, name);
    walk->inherit_read = 0;
    return ok;
}

/* The placed copy's own aiEditable.behaviors.decl, as the map reader loads its
 * JSON state into declaration text (sh_decl_carrier_tree; JSON objects are
 * merged by key, the last occurrence kept): 0 absent, so the inherited value
 * selects; 1 any own name sg_reference resolves (a quoted "NULL" included:
 * the pointer reader VK 0x1409ca890 looks names up with makeDefault), or a
 * shape the JSON walk already reports; 3 a quoted "", which that lookup
 * (VK 0x1417b36f0 / GL 0x1417a61b0) renames to _emptyName before its ordinary
 * search, so an existing aibehavior/_emptyName is selected and only its
 * absence makes a manager default (the JSON walk reports it as that lookup,
 * empty_lookup); 2 the unquoted NULL a JSON null loads as,
 * which the pointer dispatch stores as NULL; -1 the state is not a loadable
 * object. */
static int sg_inline_behavior(const char *json, size_t length)
{
    static const char *const path[] = {"aiEditable", "behaviors", "decl"};
    sh_decl_node *tree = sh_decl_carrier_tree(json, length, NULL, NULL, NULL, 0);
    const sh_decl_node *node = tree;
    const char *name; size_t name_length;
    int own = 0;
    if (!tree) return -1;
    for (size_t i = 0; node && i < sizeof(path) / sizeof(path[0]); i++) {
        const sh_decl_node *found = NULL;
        if (!node->compound) { own = 1; node = NULL; break; }
        for (const sh_decl_node *child = node->children; child; child = child->next)
            if (child->key && !strcmp(child->key, path[i])) found = child;
        node = found;
    }
    if (node) own = node->compound || !sh_decl_reference_literal(node, &name, &name_length) ? 1 :
        !strcmp(node->value, "NULL") ? 2 : !strcmp(node->value, "\"\"") ? 3 : 1;
    sh_decl_tree_free(tree);
    return own;
}

/* The state a constructed placed idAI2 copy inherits is applied to the copy's
 * class. When its parent's effective class does not derive idAI2, the
 * parent's own walk never read aiEditable (that class does not reflect it),
 * so the copy's merged state (its JSON over its inherit, the native
 * root-first merge) is walked once more under the copy's class: the behavior
 * it selects is the copy's (sg_ai_routes), and inherited references keep the
 * parent's origin. 1 walked, 0 the parent's class derives idAI2 (its own walk
 * recorded the selection), -1 not modelled (reason filled). */
static int sg_inline_inherited(sg_walk *walk, const sh_decl_entity_class_source *classes,
    sh_decl_value_type type, const char *inherit, const char *json, size_t length, char *reason, size_t capacity)
{
    sh_decl_source parent = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_node *own = NULL, *merged = NULL, definition = {0}, parent_node = {0}, edit = {0};
    const sh_decl_node *state = NULL;
    char *path = sg_lookup_path(walk, "entitydef", inherit), *parent_class = NULL, *quoted = NULL;
    int origin = walk->native_origin, status = -1, read, derives;
    walk->native_origin = path && !sh_package_compilation_find(walk->compiled, path);
    free(path);
    read = walk->failed ? -1 : sg_read_parent(walk, inherit, &parent);
    if (read == 1) parent_class = sh_decl_entity_class(parent, classes, reason, capacity);
    free((void *)parent.text);
    if (read != 1 || !parent_class) {
        if (read == 0) snprintf(reason, capacity, "its parent '%.160s' is absent", inherit);
        goto done;
    }
    derives = sg_derives(walk, parent_class, "idAI2");
    if (derives == 1) { status = 0; goto done; }
    if (derives < 0) { snprintf(reason, capacity, "the ancestry of its parent's class '%.160s' is unavailable", parent_class); goto done; }
    if (strchr(inherit, '"') || !(quoted = malloc(strlen(inherit) + 3))) {
        if (!quoted && !strchr(inherit, '"')) walk->failed = 1;
        snprintf(reason, capacity, "its parent name is not a plain declaration name"); goto done;
    }
    sprintf(quoted, "\"%s\"", inherit);
    if (!(own = sh_decl_carrier_tree(json, length, NULL, NULL, reason, capacity))) goto done;
    parent_node.key = "inherit"; parent_node.value = quoted; parent_node.assignment = 1; parent_node.next = &edit;
    edit.key = "edit"; edit.compound = edit.assignment = 1; edit.children = own->children;
    definition.compound = 1; definition.children = &parent_node;
    merged = sh_decl_entity_expanded_state(&definition, classes, reason, capacity);
    state = merged && !walk->failed ? sh_decl_tree_member(merged, "edit") : NULL;
    if (!state) { if (merged) snprintf(reason, capacity, "its merged state has no edit block"); goto done; }
    /* The parent's text is read in its own context, where a stock-style name
     * with no source is the engine's default by name (as in its own walk). A
     * served parent's member only the copy's class reflects was bound in the
     * parent's authored context when it was published (sg_bind_dormant); a
     * name its package supplies that is still unbound there is refused
     * (sg_reference, sg_authored). */
    const char *current_type = walk->current_type, *current_name = walk->current_name;
    walk->current_type = "entitydef"; walk->current_name = inherit;
    walk->inherited_member = !walk->native_origin;
    walk->ai_state = 2;
    (void)sh_decl_state_dependencies(state, type, sh_decl_native_schema_view(walk->schema), &result);
    walk->ai_state = walk->inherited_member = 0;
    walk->current_type = current_type; walk->current_name = current_name;
    status = result.aborted ? -1 : 1;
    if (result.aborted) snprintf(reason, capacity, "inspection of its merged state aborted");
done:
    walk->native_origin = origin;
    free(parent_class); free(quoted); sh_decl_tree_free(own); sh_decl_tree_free(merged);
    return walk->failed ? -1 : status;
}

/* A constructed placed spawner copy (walk->spawner_copy) spawns only when
 * both pointers of its merged state are set (sg_pair_state): its JSON over
 * its inherit, the native root-first merge, over the constructor's NULL pair.
 * The fields it does not set are its parent's effective values whatever the
 * parent's class: a parent whose class derives the spawner read them in its
 * own walk, so they are its uses attributed to this copy (parent, unset);
 * another parent's walk never read a pair its class does not reflect, so they
 * are walked here under the copy's class with the parent's origin, as
 * sg_inline_inherited walks an AI's inherited behavior. A merged state the
 * product cannot form takes the inherited fields as set. */
static void sg_inline_spawner(sg_walk *walk, const sh_decl_entity_class_source *classes,
    sh_decl_value_type type, const char *inherit, const char *json, size_t length)
{
    static const char *const names[] = {"entityStatic", "entityPinata"};
    struct sg_spawner_copy *copy = walk->spawner_copies + walk->spawner_copy;
    sh_decl_source parent = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_node *own = sh_decl_carrier_tree(json, length, NULL, NULL, NULL, 0), *merged = NULL;
    sh_decl_node definition = {0}, parent_node = {0}, edit = {0}, state = {0}, pair = {0}, fields[2];
    const sh_decl_node *merged_edit = NULL, *merged_pair = NULL;
    char *path, *parent_class = NULL, *quoted = NULL, reason[512] = "";
    int origin = walk->native_origin, mine = own ? sg_pair_state(own) : 0, derives = -1;
    size_t count = 0;
    copy->unset = own ? (~mine >> 2) & 3 : 3;
    copy->set = own ? mine & 3 : 3;
    if (!copy->unset || !inherit || !*inherit) goto done;
    copy->parent = sg_queued(walk, "entitydef", inherit);
    if (!own) goto done;
    if (sg_read_parent(walk, inherit, &parent) == 1) parent_class = sh_decl_entity_class(parent, classes, reason, sizeof(reason));
    free((void *)parent.text);
    if (parent_class) derives = sg_derives(walk, parent_class, SG_SPAWNER_CLASS);
    if (derives < 0 || walk->failed || strchr(inherit, '"') || !(quoted = malloc(strlen(inherit) + 3))) {
        if (derives >= 0 && !walk->failed && !strchr(inherit, '"')) walk->failed = 1;
        copy->set |= copy->unset; goto done;
    }
    sprintf(quoted, "\"%s\"", inherit);
    parent_node.key = "inherit"; parent_node.value = quoted; parent_node.assignment = 1; parent_node.next = &edit;
    edit.key = "edit"; edit.compound = edit.assignment = 1; edit.children = own->children;
    definition.compound = 1; definition.children = &parent_node;
    merged = sh_decl_entity_expanded_state(&definition, classes, reason, sizeof(reason));
    merged_edit = merged && !walk->failed ? sh_decl_tree_member(merged, "edit") : NULL;
    if (!merged_edit) { copy->set |= copy->unset; goto done; }
    copy->set = sg_pair_state(merged_edit) & 3;
    if (derives == 1) goto done;
    copy->parent = SIZE_MAX;
    if (merged_edit->compound)
        for (const sh_decl_node *child = merged_edit->children; child; child = child->next)
            if (child->key && !strcmp(child->key, "spawnerEntityPair")) merged_pair = child;
    if (!merged_pair || !merged_pair->compound) goto done;
    for (int i = 0; i < 2; i++) {
        const sh_decl_node *value = NULL;
        if (!(copy->unset & (1 << i))) continue;
        for (const sh_decl_node *child = merged_pair->children; child; child = child->next)
            if (child->key && !strcmp(child->key, names[i])) value = child;
        if (!value) continue;
        fields[count] = *value; fields[count].next = NULL;
        if (count) fields[count - 1].next = fields + count;
        count++;
    }
    if (!count) goto done;
    pair = *merged_pair; pair.next = NULL; pair.children = fields;
    state.compound = 1; state.children = &pair;
    path = sg_lookup_path(walk, "entitydef", inherit);
    walk->native_origin = path && !sh_package_compilation_find(walk->compiled, path);
    free(path);
    const char *current_type = walk->current_type, *current_name = walk->current_name;
    walk->current_type = "entitydef"; walk->current_name = inherit;
    walk->inherited_member = !walk->native_origin;
    walk->spawner_state = 2;
    (void)sh_decl_state_dependencies(&state, type, sh_decl_native_schema_view(walk->schema), &result);
    walk->spawner_state = walk->inherited_member = 0;
    walk->current_type = current_type; walk->current_name = current_name;
    if (result.aborted && !walk->failed)
        sg_gap(walk, "edit.spawnerEntityPair", type.name, "inspection of the placed spawner's inherited pair aborted");
done:
    walk->native_origin = origin;
    free(parent_class); free(quoted); sh_decl_tree_free(own); sh_decl_tree_free(merged);
}

/* The inherited members a consumer's own layer leaves in effect, as the
 * root-first merge applies them: an inherited member the own layer does not
 * set stays, a compound both set merges by key, and an own scalar or reset
 * block replaces it. Nodes are shallow copies kept in *made (freed by the
 * caller with sg_made_free); strings and leaves stay borrowed. */
typedef struct sg_made { sh_decl_node **items; size_t count, capacity; int failed; } sg_made;
static sh_decl_node *sg_made_copy(sg_made *made, const sh_decl_node *node)
{
    sh_decl_node *copy;
    if (made->failed) return NULL;
    if (made->count == made->capacity) {
        size_t next = made->capacity ? made->capacity * 2 : 16;
        sh_decl_node **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(made->items, next * sizeof(*grown)) : NULL;
        if (!grown) { made->failed = 1; return NULL; }
        made->items = grown; made->capacity = next;
    }
    if (!(copy = malloc(sizeof(*copy)))) { made->failed = 1; return NULL; }
    *copy = *node; copy->next = NULL;
    made->items[made->count++] = copy; return copy;
}
static void sg_made_free(sg_made *made)
{
    for (size_t i = 0; i < made->count; i++) free(made->items[i]);
    free(made->items); memset(made, 0, sizeof(*made));
}
/* Inherited children minus what the own blocks (count of them) set. */
static sh_decl_node *sg_unset(sg_made *made, const sh_decl_node *inherited, const sh_decl_node *const *own, size_t count)
{
    sh_decl_node *head = NULL, **tail = &head;
    for (const sh_decl_node *child = inherited; child && !made->failed; child = child->next) {
        const sh_decl_node *blocks[32];
        size_t found = 0;
        int replaced = 0;
        for (size_t i = 0; i < count && !replaced; i++)
            for (const sh_decl_node *mine = own[i]->children; mine && !replaced; mine = mine->next) {
                if (!mine->key || !child->key || strcmp(mine->key, child->key)) continue;
                if (!mine->compound || mine->reset || !child->compound || found == sizeof(blocks) / sizeof(blocks[0]))
                    replaced = 1;
                else blocks[found++] = mine;
            }
        if (replaced) continue;
        sh_decl_node *copy = sg_made_copy(made, child);
        if (!copy) break;
        if (found && child->compound) {
            copy->children = sg_unset(made, child->children, blocks, found);
            if (!copy->children) continue;
        }
        *tail = copy; tail = &copy->next;
    }
    return made->failed ? NULL : head;
}

/* A served definition's members its effective class does not reflect are
 * read by a consumer of another class: a placed copy of its own class, or a
 * definition deriving from it, constructs over the inherited text and reads
 * each member under the consumer's class. Publication typed such a member by
 * every deriving class (sg_bind_dormant); when they agreed it was bound in the
 * definition's context, otherwise it stays as authored. The members the
 * consumer's own layer leaves in effect (sg_unset) are walked once more under
 * the consumer's class with the definition's origin: a bound name is this
 * use's requirement, a package name compiled unbound is refused as product
 * coverage (sg_reference), and a member the consumer reads as text names
 * nothing. Members the definition's class reflects were judged by its own
 * walk. skip names a member another route already judged (NULL none).
 * 1 walked, 0 nothing to judge, -1 not modelled (reason filled). */
/* R-249: the package-supplied resource serving entitydef name in this
 * reader; NULL for a stock or absent layer. */
static const sh_compiled_resource *sg_layer_resource(sg_walk *walk, const char *name)
{
    char *path = sg_lookup_path(walk, "entitydef", name);
    const sh_compiled_resource *found = path ? sh_package_compilation_find(walk->compiled, path) : NULL;
    free(path);
    return found && found->source_count && !found->native_original && !found->restored_original ? found : NULL;
}

/* R-249: inherited members a reader of class consumer reads unbound. Each value
 * keeps the context of the layer that authored it: the nearest layer of the
 * chain from parent whose own state statements set the member. Publication
 * bound it when that layer's class reflects it or every class deriving from the
 * layer's class types it alike (sg_bind_dormant); otherwise its authored
 * spelling reaches every reader. A stock layer's value keeps its global
 * meaning. Kept: package-authored values left unbound that consumer's class
 * reflects and own does not set (sg_unset). *head lists them (shallow copies in
 * made borrowing *merged, which the caller frees); *layers (when given; the
 * caller frees it) receives each kept member's authoring layer in order. Returns the kept count, -1 on failure. */
static int sg_unbound_inherited(sg_walk *reader, const sh_decl_entity_class_source *classes, const char *consumer,
    const char *parent, const sh_decl_node *const *own, size_t own_count, sg_made *made, sh_decl_node **head,
    sh_decl_node **merged, const sh_compiled_resource ***layers)
{
    const sh_decl_dependency_schema *view = sh_decl_native_schema_view(reader->schema);
    sh_decl_node definition = {0}, inherit_node = {0}, edit = {0};
    struct { char *key, *layer_class; const sh_compiled_resource *layer; } *authored = NULL;
    char **visited = NULL;
    size_t authored_count = 0, authored_capacity = 0, visited_count = 0;
    char reason[512] = "", *quoted = NULL, *name = NULL;
    int kept = 0;
    *head = NULL; *merged = NULL;
    if (layers) *layers = NULL;
    if (!consumer || !*consumer || !parent || !*parent || strchr(parent, '"')) return 0;
    for (size_t i = 0; i < own_count; i++) if (!own[i]->compound) return 0;
    if (!(quoted = malloc(strlen(parent) + 3)) || !(name = _strdup(parent))) { reader->failed = 1; kept = -1; goto done; }
    sprintf(quoted, "\"%s\"", parent);
    /* The native expansion has no depth limit. A repeated layer is cyclic
     * inheritance, which the expansion below reports. */
    while (name && !reader->failed) {
        sh_decl_source text = {0};
        sh_decl_entity_headers headers = {0};
        sh_decl_node *tree = NULL;
        char *layer_class = NULL, *next = NULL;
        const char *literal = NULL; size_t n = 0;
        size_t seen = 0;
        while (seen < visited_count && strcmp(visited[seen], name)) seen++;
        if (seen < visited_count) break;
        {
            char **grown = realloc(visited, (visited_count + 1) * sizeof(*visited));
            if (!grown) { reader->failed = 1; break; }
            visited = grown; visited[visited_count++] = name;
        }
        if (sg_read_parent(reader, name, &text) != 1) { free((void *)text.text); name = NULL; break; }
        tree = sh_decl_tree_parse_ordered(text, reason, sizeof(reason));
        layer_class = tree ? sh_decl_entity_class(text, classes, reason, sizeof(reason)) : NULL;
        const sh_compiled_resource *layer = sg_layer_resource(reader, name);
        if (tree) {
            sh_decl_entity_split(tree, &headers);
            for (const sh_decl_node *s = headers.state; s && headers.state->key; s = s->next) {
                if (!s->key || strcmp(s->key, headers.state->key) || !s->compound) continue;
                for (const sh_decl_node *m = s->children; m; m = m->next) {
                    size_t j = 0;
                    if (!m->key) continue;
                    while (j < authored_count && strcmp(authored[j].key, m->key)) j++;
                    if (j < authored_count) continue;
                    if (authored_count == authored_capacity) {
                        size_t capacity = authored_capacity ? authored_capacity * 2 : 16;
                        void *grown = realloc(authored, capacity * sizeof(*authored));
                        if (!grown) { reader->failed = 1; break; }
                        authored = grown; authored_capacity = capacity;
                    }
                    authored[j].key = _strdup(m->key);
                    authored[j].layer_class = layer_class ? _strdup(layer_class) : NULL;
                    authored[j].layer = layer; authored_count++;
                    if (!authored[j].key) reader->failed = 1;
                }
            }
            if (sh_decl_entity_expands(tree) == 1 && headers.inherit &&
                sh_decl_reference_literal(headers.inherit, &literal, &n) && n && (next = malloc(n + 1)) != NULL) {
                memcpy(next, literal, n); next[n] = 0;
            }
        }
        sh_decl_tree_free(tree); free(layer_class); free((void *)text.text); name = next;
    }
    if (reader->failed) { kept = -1; goto done; }
    inherit_node.key = "inherit"; inherit_node.value = quoted; inherit_node.assignment = 1; inherit_node.next = &edit;
    edit.key = "edit"; edit.compound = edit.assignment = 1;
    definition.compound = 1; definition.children = &inherit_node;
    *merged = sh_decl_entity_expanded_state(&definition, classes, reason, sizeof(reason));
    const sh_decl_node *inherited = *merged && !reader->failed ? sh_decl_tree_member(*merged, "edit") : NULL;
    if (!inherited || !inherited->compound) { kept = reader->failed ? -1 : 0; goto done; }
    sh_decl_node *list = sg_unset(made, inherited->children, own, own_count), **tail = head;
    if (made->failed) { reader->failed = 1; kept = -1; goto done; }
    for (sh_decl_node *member = list, *next; member; member = next) {
        sh_decl_value_type field = {0}, descendant = {0};
        size_t j = 0;
        next = member->next; member->next = NULL;
        if (!member->key) continue;
        while (j < authored_count && strcmp(authored[j].key, member->key)) j++;
        if (j == authored_count || !authored[j].layer || !authored[j].layer_class ||
            view->field(view->context, (sh_decl_value_type){authored[j].layer_class, ""}, member->key, &field) != 0 ||
            sh_decl_native_schema_descendant_field(reader->schema, authored[j].layer_class, member->key, &descendant) == 1 ||
            view->field(view->context, (sh_decl_value_type){consumer, ""}, member->key, &field) != 1) continue;
        if (layers) {
            const sh_compiled_resource **grown = realloc((void *)*layers, ((size_t)kept + 1) * sizeof(*grown));
            if (!grown) { reader->failed = 1; kept = -1; goto done; }
            *layers = grown; grown[kept] = authored[j].layer;
        }
        *tail = member; tail = &member->next; kept++;
    }
done:
    for (size_t j = 0; j < authored_count; j++) { free(authored[j].key); free(authored[j].layer_class); }
    for (size_t j = 0; j < visited_count; j++) free(visited[j]);
    free(authored); free(visited); free(quoted); free(name);
    return kept;
}

static int sg_inherited_readers(sg_walk *walk, const sh_decl_entity_class_source *classes, const char *consumer,
    const char *inherit, const sh_decl_node *const *own, size_t own_count, const char *skip, char *reason, size_t capacity,
    int projectable)
{
    sh_decl_node *unbound_merged = NULL;
    sh_decl_source parent = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_node *merged = NULL, definition = {0}, parent_node = {0}, edit = {0}, state = {0};
    const sh_decl_node *inherited = NULL;
    const sh_decl_dependency_schema *view = sh_decl_native_schema_view(walk->schema);
    sg_made made = {0};
    char *path, *parent_class = NULL, *quoted = NULL;
    int origin = walk->native_origin, status = -1, read;
    if (!consumer || !*consumer || !inherit || !*inherit || walk->failed) return 0;
    read = sg_read_parent(walk, inherit, &parent);
    if (read == 1) parent_class = sh_decl_entity_class(parent, classes, reason, capacity);
    free((void *)parent.text);
    if (read != 1 || !parent_class) {
        if (read == 0) snprintf(reason, capacity, "its parent '%.160s' is absent", inherit);
        goto done;
    }
    if (!strcmp(parent_class, consumer)) { status = 0; goto done; }
    if (strchr(inherit, '"') || !(quoted = malloc(strlen(inherit) + 3))) {
        if (!quoted && !strchr(inherit, '"')) walk->failed = 1;
        snprintf(reason, capacity, "its parent name is not a plain declaration name"); goto done;
    }
    sprintf(quoted, "\"%s\"", inherit);
    parent_node.key = "inherit"; parent_node.value = quoted; parent_node.assignment = 1; parent_node.next = &edit;
    edit.key = "edit"; edit.compound = edit.assignment = 1;
    definition.compound = 1; definition.children = &parent_node;
    merged = sh_decl_entity_expanded_state(&definition, classes, reason, capacity);
    inherited = merged && !walk->failed ? sh_decl_tree_member(merged, "edit") : NULL;
    if (!inherited) { if (merged) snprintf(reason, capacity, "its inherited state has no edit block"); goto done; }
    if (!inherited->compound) { status = 0; goto done; }
    {
        int scalar = 0;
        for (size_t i = 0; i < own_count; i++) scalar |= !own[i]->compound;
        sh_decl_node *kept = scalar ? NULL : sg_unset(&made, inherited->children, own, own_count);
        sh_decl_node *head = NULL, **tail = &head;
        if (made.failed) { walk->failed = 1; goto done; }
        for (sh_decl_node *member = kept, *next; member; member = next) {
            sh_decl_value_type field = {0};
            next = member->next; member->next = NULL;
            if (!member->key || (skip && !strcmp(member->key, skip))) continue;
            int reflected = view->field(view->context, (sh_decl_value_type){parent_class, ""}, member->key, &field);
            if (reflected < 0) { snprintf(reason, capacity, "the reflected fields of its parent's class '%.160s' are "
                "unavailable", parent_class); goto done; }
            if (reflected) continue;
            int reads = view->field(view->context, (sh_decl_value_type){consumer, ""}, member->key, &field);
            if (reads < 0) { snprintf(reason, capacity, "the reflected fields of the consuming class '%.160s' are "
                "unavailable", consumer); goto done; }
            if (!reads) continue;
            *tail = member; tail = &member->next;
        }
        if (!head) { status = 0; goto done; }
        state.compound = 1; state.children = head;
    }
    path = sg_lookup_path(walk, "entitydef", inherit);
    walk->native_origin = path && !sh_package_compilation_find(walk->compiled, path);
    free(path);
    /* R-249: a value its package layer left unbound is read in that layer's
     * context. Map use projects it into a placed copy's own state, bound in the
     * copy's selected supplier context (sh_package_source_reader_projection),
     * where the copy's own walk requires exactly that declaration; a copy the
     * projection leaves without it reads the spelling's global meaning. Any
     * other unbound package read stays refused (inherited_member). */
    {
        sh_decl_node *unbound = NULL, *open = NULL, **open_tail = &open, *guarded = NULL, **guarded_tail = &guarded;
        int count = sg_unbound_inherited(walk, classes, consumer, inherit, own, own_count, &made, &unbound,
            &unbound_merged, NULL), aborted = 0;
        if (count < 0) goto done;
        for (sh_decl_node *member = state.children, *next; member; member = next) {
            const sh_decl_node *match = unbound;
            next = member->next; member->next = NULL;
            while (match && strcmp(match->key, member->key)) match = match->next;
            if (!match || (projectable && !member->compound)) { *open_tail = member; open_tail = &member->next; }
            else { *guarded_tail = member; guarded_tail = &member->next; }
        }
        const char *current_type = walk->current_type, *current_name = walk->current_name;
        walk->current_type = "entitydef"; walk->current_name = inherit;
        for (int pass = 0; pass < 2; pass++) {
            if (!(state.children = pass ? guarded : open)) continue;
            memset(&result, 0, sizeof(result));
            walk->inherited_member = pass && !walk->native_origin;
            (void)sh_decl_state_dependencies(&state, (sh_decl_value_type){consumer, ""}, view, &result);
            aborted |= result.aborted;
        }
        walk->inherited_member = 0;
        walk->current_type = current_type; walk->current_name = current_name;
        status = aborted ? -1 : 1;
        if (aborted) snprintf(reason, capacity, "inspection of its inherited state aborted");
    }
done:
    walk->native_origin = origin;
    sh_decl_tree_free(unbound_merged);
    sg_made_free(&made);
    free(parent_class); free(quoted); sh_decl_tree_free(merged);
    return walk->failed ? -1 : status;
}

static int sg_inline(void *context, const char *class_name, const char *inherit,
    const char *json, size_t length)
{
    sg_walk *walk = context;
    sh_decl_source parent = {0};
    sh_decl_dependency_result result = {0};
    sh_decl_entity_class_source classes = {walk, sg_read_parent, sg_derives, sg_known};
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
    /* Map spawn constructs the placed copy with this class through
     * CreateManagedClassFromDecl; a NULL result spawns nothing. A constructed
     * idAI2 copy spawns with the behavior its state selects (sg_reference). */
    int ai = 0, spawner = 0, constructed = 1, judged = 0;
    if (walk->native.classes) {
        sh_native_construction construction = sh_native_class_construction(walk->native.classes, type.name, NULL);
        ai = construction == SH_NATIVE_CONSTRUCTION_ELIGIBLE && sg_derives(walk, type.name, "idAI2") == 1;
        spawner = construction == SH_NATIVE_CONSTRUCTION_ELIGIBLE && sg_derives(walk, type.name, SG_SPAWNER_CLASS) == 1;
        constructed = construction == SH_NATIVE_CONSTRUCTION_ELIGIBLE || construction == SH_NATIVE_CONSTRUCTION_UNAVAILABLE;
        if (construction != SH_NATIVE_CONSTRUCTION_ELIGIBLE) {
            char reason[640];
            snprintf(reason, sizeof(reason), construction == SH_NATIVE_CONSTRUCTION_UNAVAILABLE ?
                "construction of the placed entity is undecided (environment): class '%.160s' %s" :
                "impossible construction: the map places an entity whose class '%.160s' %s, so CreateManagedClassFromDecl "
                "returns NULL and the map spawn creates nothing", type.name ? type.name : "",
                sh_native_construction_text(construction));
            sg_gap(walk, "class", type.name, reason);
        }
    }
    walk->ai_state = ai ? 2 : 0; walk->ai_override = 0;
    walk->spawner_copy = spawner ? sg_spawner_copy_add(walk) : SIZE_MAX;
    walk->spawner_state = walk->spawner_copy != SIZE_MAX ? 2 : 0;
    walk->map_state = 1;
    (void)sh_decl_json_state_dependencies(json, length, type,
        sh_decl_native_schema_view(walk->schema), &result);
    walk->ai_state = walk->spawner_state = walk->map_state = 0;
    if (result.aborted && !walk->failed) sg_gap(walk, "edit", type.name, "inline dependency inspection aborted");
    /* Whether the constructed spawner copy's merged state sets both pointers,
     * and the values it inherits (sg_inline_spawner, sg_spawner_routes). */
    if (walk->spawner_copy != SIZE_MAX && !walk->failed) sg_inline_spawner(walk, &classes, type, inherit, json, length);
    walk->spawner_copy = SIZE_MAX;
    /* A constructed idAI2 copy spawns with the behavior its own state sets
     * (sg_reference), or without one with the inherited value. Any own value
     * leaves the parent's unused. A NULL one is no native-valid AI: the AI
     * spawn (VK 0x140654010 / GL 0x140653d40) reads the behavior at +0xd4
     * before its NULL-checked pre-pool. */
    int own = ai && !walk->failed ? sg_inline_behavior(json, length) : 0;
    /* The empty name is an own name: the JSON walk above already resolved it
     * as the ordinary lookup of _emptyName (sg_reference). */
    if (own == 3) own = 1;
    if (own == 0 && walk->ai_override) own = 1;
    /* The AI spawn reads the behavior unchecked (VK 0x140654010 / GL
     * 0x140653d40). The only direct store to AI +0xc120 on either image is a
     * script event that rejects NULL (VK 0x1406393c0 / GL 0x1406396b0), but
     * stores through the aiEditable sub-object or whole-structure copies are
     * not excluded, so a NULL selection stays undecided rather than proven
     * impossible. */
    if (own == 2) sg_gap(walk, "edit.aiEditable.behaviors.decl", type.name, "the placed AI's spawn is undecided "
        "(native evidence): its behavior is NULL (a JSON null loads as the NULL token), the AI spawn reads it without "
        "a NULL check, and no traced initialization replaces it");
    else if (own < 0) sg_gap(walk, "edit", type.name, "the placed AI's behavior selection is undecided (product coverage): "
        "its state is not a loadable JSON object");
    else if (ai && !own && inherit && *inherit && !walk->failed) {
        /* An inherited selection: a parent whose class derives idAI2 recorded
         * what its state selects (sg_ai_routes); another parent class is read
         * under the copy's class (sg_inline_inherited). */
        char reason[512] = "";
        int inherited = class_name && *class_name ?
            sg_inline_inherited(walk, &classes, type, inherit, json, length, reason, sizeof(reason)) : 0;
        judged = inherited == 1;
        if (!inherited) {
            size_t index = sg_queued(walk, "entitydef", inherit);
            if (index != SIZE_MAX) walk->candidate[index] |= SG_AI_SPAWNED;
        } else if (inherited < 0 && !walk->failed) {
            char text[640];
            snprintf(text, sizeof(text), "the behavior the placed AI inherits is undecided (product coverage): %s",
                reason[0] ? reason : "its merged state is not modelled");
            sg_gap(walk, "edit.aiEditable.behaviors.decl", type.name, text);
        }
    }
    /* Any other constructed copy whose class differs from its parent's reads
     * the parent's unreflected members itself (sg_inherited_readers); the AI
     * route above already walked a whole merged state, and the spawner route
     * its pair. */
    if (constructed && !judged && class_name && *class_name && inherit && *inherit && !walk->failed) {
        char reason[512] = "";
        sh_decl_node *own = sh_decl_carrier_tree(json, length, NULL, NULL, reason, sizeof(reason));
        const sh_decl_node *blocks[1] = {own};
        int status = own || !json ? sg_inherited_readers(walk, &classes, type.name, inherit, blocks, own ? 1 : 0,
            spawner ? "spawnerEntityPair" : NULL, reason, sizeof(reason), json != NULL) : -1;
        sh_decl_tree_free(own);
        if (status < 0 && !walk->failed) {
            char text[640];
            snprintf(text, sizeof(text), "the members the placed copy reads from its parent's state are undecided "
                "(product coverage): %s", reason[0] ? reason : "its inherited state is not modelled");
            sg_gap(walk, "edit", type.name, text);
        }
    }
    free((void *)parent.text); free(inherited_class); return !walk->failed;
}

sh_package_source_reader *sh_package_source_reader_open(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    char *error, size_t capacity)
{
    return sh_package_source_reader_open_shared(NULL, compiled, catalog, original, original_context,
        registry, reflection, error, capacity);
}
/* The schema reads the image through the caller's registry read itself, not
 * through this walk, so a reader sharing its records never reads through a
 * closed walk. */
sh_package_source_reader *sh_package_source_reader_open_shared(const sh_package_source_reader *base,
    const sh_package_compilation *compiled, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context, sh_decl_registry_source registry,
    uintptr_t reflection, char *error, size_t capacity)
{
    sg_walk *walk = calloc(1, sizeof(*walk));
    if (!walk || !compiled || !registry.read || !reflection) goto failed;
    walk->compiled = compiled; walk->catalog = catalog; walk->original = original;
    walk->original_context = original_context; walk->native = registry;
    walk->error = error; walk->capacity = capacity;
    walk->registry = sh_decl_native_registry_open(
        (sh_decl_registry_source){walk, sg_native_read, registry.registry, sg_exists}, error, capacity);
    walk->schema = sh_decl_native_schema_open_shared(base ? base->schema : NULL,
        (sh_decl_native_source){registry.context, registry.read, reflection, registry.scripts},
        (sh_decl_dependency_observer){walk, sg_reference, sg_field_gap, NULL, sg_empty_names}, error, capacity);
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
void sh_package_source_reader_original_status(sh_package_source_reader *reader,
    sh_package_source_original_status status)
{
    if (reader) reader->original_status = status;
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
    free(path); path = NULL;
    {
        /* A material the catalog lists under its own file (a palette's
         * megatexture, maps/modules/palettes/<p>/mega.decl) is read there,
         * as its unit's source is; others use their declaration path. */
        const sh_resource_catalog_entry *const *rows = NULL;
        size_t count = reader->catalog ? sh_resource_catalog_find(reader->catalog, "material", name, &rows) : 0;
        for (size_t i = 0; !path && i < count; i++)
            if (*rows[i]->path && rows[i]->archive < 2) path = sh_package_engine_path(rows[i]->path);
        if (!path) path = sg_path("material", name);
    }
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
    const char *entity_state;
    int every_state; /* undecided state coverage: bind every statement */
    int header_only; /* no reflected class schema: bind the inherit header only */
    int environment; /* stopped on allocation (sh_package_source_binding) */
    size_t references, record;
    char *replacement;
    int observe;
    /* Reference binding of an entity definition also binds the members its
     * class does not reflect (sg_bind_dormant); dormant marks that pass. */
    int dormant_members, dormant;
    /* Map bindings only: an entityDef name no source supplies, at an entity's
     * own state, that names a placed entity's play declaration (sg_map_local):
     * 1 kept, 2 rewritten, 0 not such a name, -1 refused with its own gap. */
    int (*local)(void *context, const char *name, size_t length, const char **replacement);
    void *local_context;
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

/* A name in a member its declaration's class does not reflect (sg_bind_dormant)
 * binds only when the lookup the reading class performs finds an existing
 * declaration: one this authored scope supplies takes its bound identity, and
 * a native one keeps its spelling. An absent name, or one another reader
 * resolves, stays as authored with no edge and no gap: no class of this
 * declaration reads it, and a construction that does judges that use itself. */
static int sg_bind_dormant_typed(sg_binding *binding, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    sg_walk *reader = binding->reader;
    sh_decl_native_reference reference = {0};
    const char *target = NULL;
    int result = 0;
    if (sh_decl_native_schema_reader(reader->schema, type) != SH_DECL_READER_DECL) return 0;
    if (sh_decl_native_reference_read(reader->schema, reader->registry, type, name, length, &reference) != 1 ||
        reader->failed) { sh_decl_native_reference_clear(&reference); return reader->failed ? -1 : 0; }
    if (binding->observe) {
        char *copy = malloc(length + 1);
        if (!copy) { reader->failed = 1; sh_decl_native_reference_clear(&reference); return -1; }
        memcpy(copy, name, length); copy[length] = 0;
        sg_candidate candidate = {binding, slot};
        binding->references++;
        result = sh_decl_native_registry_source_candidates(reader->registry, type.name, copy,
            sg_bind_candidate, &candidate) ? 0 : -1;
        free(copy);
        if (result < 0) sg_gap(reader, slot, type.name, "native candidate names could not be observed");
    } else {
        char *family = sg_name(reference.resource.type);
        binding->references++;
        result = family ? binding->resolver(binding->context, slot, family, reference.resource.name, &target) : -1;
        free(family);
        if (result == 1) {
            free(binding->replacement);
            binding->replacement = target && *target ? sh_decl_native_reference_bind(&reference, target) : NULL;
            if (!binding->replacement) result = -1;
            else *replacement = binding->replacement;
        }
        if (result < 0) sg_gap(reader, slot, type.name, "native source binding of an existing declaration "
            "could not be resolved");
    }
    sh_decl_native_reference_clear(&reference); return result;
}

/* The product serves its own declarations only under these names
 * (package_native_names.c, package_library.c, package_virtual_material.c,
 * package_fsm.h): a path segment "smp" (smp/, generic/smp/, baked/smp/,
 * generated/image/smp/, smp/vt/, smp/fsmweb/) or one starting "smp-"
 * (smp-contribution/ composition tokens). */
static int sg_served_name(const char *canonical)
{
    for (const char *segment = canonical; segment && *segment; ) {
        size_t n = strcspn(segment, "/");
        if ((n == 3 && !memcmp(segment, "smp", 3)) || (n > 4 && !memcmp(segment, "smp-", 4))) return 1;
        segment += n; if (*segment) segment++;
    }
    return 0;
}
/* R-390: a map's name with no source in scope (status 2) that the engine has
 * already registered is the engine's own declaration: a sound bank event
 * (soundbanksinfo.events) or a default it made when stock text named an
 * absent declaration (zion/vo/hellknight/default). A vanilla map carrying it
 * opens in the game, so it binds nothing and is no gap. The product's served
 * names, and any name the map's record or the installed library binds
 * (lookup only), are not the engine's: they keep their refusal. 1 engine
 * default, 0 not one, -1 failure. */
static int sg_engine_default(sg_binding *binding, const char *slot, sh_decl_value_type type,
    const char *name, size_t length)
{
    sg_walk *reader = binding->reader;
    sh_decl_reference registered = {0};
    const char *target = NULL;
    char *copy, *canonical, *family;
    int found, bound;
    if (!name || !length || length == SIZE_MAX || memchr(name, 0, length) || !(copy = malloc(length + 1))) return -1;
    memcpy(copy, name, length); copy[length] = 0;
    canonical = sh_native_resource_name(copy);
    if (!canonical || sg_served_name(canonical)) { free(copy); free(canonical); return canonical ? 0 : -1; }
    found = sh_decl_native_registry_resolve(reader->registry, type.name, copy, &registered);
    free(copy);
    if (found != 1 || !registered.type) { sh_decl_reference_clear(&registered); free(canonical); return found < 0 ? -1 : 0; }
    family = sg_name(registered.type);
    bound = !family ? -1 : reader->saved_binding ?
        reader->saved_binding(reader->saved_context, slot, family, canonical, &target) : 0;
    free(family); free(canonical); sh_decl_reference_clear(&registered);
    return bound < 0 ? -1 : !bound;
}

static int sg_bind_typed(void *context, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    sg_binding *binding = context;
    sh_decl_native_reference reference = {0};
    const char *target = NULL;
    if (binding->dormant) return sg_bind_dormant_typed(binding, slot, type, name, length, replacement);
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
    /* As in the dependency walk (sg_reference): a by-name reference to a
     * declaration only the campaign archive carries binds as its literal name
     * with no source in scope (sg_campaign_only). */
    if (result == 1 && sg_by_name(type, name, length)) {
        int campaign = sg_campaign_only(binding->reader, reference.resource.type, reference.resource.name);
        if (campaign < 0) { sh_decl_native_reference_clear(&reference); return -1; }
        if (campaign) result = 2;
    }
    /* Authored declaration text keeps an engine-defaulted absent declaration
     * unchanged, with no binding edge, like its stock counterpart. */
    int event_decl = result == 2 && !sg_native_default(binding->reader) && sg_event_decl(type) &&
        !binding->reader->bound_absent && !sg_composition_token(name, length);
    if (event_decl) result = sg_bind_event_decl(binding, slot, type, name, length, replacement);
    else if (result == 2) {
        const char *family = NULL;
        int local = binding->local && sh_decl_native_registry_class_type(binding->reader->registry, type.name, &family) > 0 &&
            family && !_stricmp(family, "entityDef") ? binding->local(binding->local_context, name, length, replacement) : 0;
        if (local) { sh_decl_native_reference_clear(&reference); return local < 0 ? -1 : local - 1; }
        result = (sg_native_default(binding->reader) || sg_empty(name, length) || sg_by_name(type, name, length)) &&
            !binding->reader->bound_absent && !sg_composition_token(name, length) ? 0 : -1;
        if (result < 0 && !binding->reader->bound_absent && !sg_composition_token(name, length)) {
            int engine = sg_engine_default(binding, slot, type, name, length);
            if (engine) { sh_decl_native_reference_clear(&reference); return engine < 0 ? -1 : 0; }
        }
    }
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
    /* Only the target's status decides: its bytes are not read (sg_path_status). */
    int status = path ? sg_path_status(binding->reader, path) : -1;
    free(path);
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

/* The resolved state contract over the bound parents (sh_decl_entity_merges):
 * "" for the first stateful layer, the inherited first statement's name in
 * update mode. Nothing substitutes a state when it is undecided: an inherited
 * first statement outside the verified model is remaining coverage, recorded
 * as a gap while every block statement binds (no statement native may read
 * keeps an unbound reference); malformed input (-2), a missing parent source
 * or allocation (0) fail. 1 otherwise. */
#ifdef SH_PACKAGE_LIBRARY_TESTING
static long g_entity_state_allocation_failures;
void sh_package_source_test_entity_state_allocation(long count) { g_entity_state_allocation_failures = count; }
#endif
static int sg_entity_state(sg_binding *binding, const sh_decl_node *tree, const sh_decl_entity_class_source *classes)
{
    char *name = NULL, reason[512] = "";
    sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
    sg_walk *walk = binding->reader;
    int merges = sh_decl_entity_merges_classified(tree, classes, &name, &failure, reason, sizeof(reason));
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_entity_state_allocation_failures > 0 && g_entity_state_allocation_failures--) {
        free(name); name = NULL; merges = -1; failure = SH_DECL_ENTITY_MERGE_ALLOCATION;
        snprintf(reason, sizeof(reason), "allocation failed");
    }
#endif
    if (merges == 0) { free(name); name = _strdup(""); }
    binding->entity_state = name;
    if (merges >= 0) {
        if (name) return 1;
        binding->environment = 1;
        snprintf(walk->error, walk->capacity, "entity state allocation failed"); return 0;
    }
    /* Allocation or a parent source the reader could not supply says nothing
     * about the declaration; only a conclusively absent parent is missing. */
    if (failure == SH_DECL_ENTITY_MERGE_ALLOCATION || failure == SH_DECL_ENTITY_MERGE_UNAVAILABLE)
        binding->environment = 1;
    if (failure == SH_DECL_ENTITY_MERGE_COVERAGE) {
        char gap[640];
        snprintf(gap, sizeof(gap), "native state selection is product coverage: %s", reason);
        binding->every_state = 1; sg_gap(walk, "edit", binding->state.name, gap); return 1;
    }
    snprintf(walk->error, walk->capacity, "entity state %s: %s", failure == SH_DECL_ENTITY_MERGE_MALFORMED ?
        "is malformed" : failure == SH_DECL_ENTITY_MERGE_MISSING ? "parent source is missing" :
        failure == SH_DECL_ENTITY_MERGE_UNAVAILABLE ? "parent source is unavailable" : "failed", reason);
    return failure == SH_DECL_ENTITY_MERGE_MALFORMED ? -2 : 0;
}
/* Members an entity definition's class does not reflect stay in its text. The
 * engine reads them when an entity whose class derives from that class and
 * reflects them constructs over this text: a placed copy of another class, or
 * a child definition, inherits the merged state and reads it under its own
 * class (entityDef inheritance requires the child's class to derive from the
 * parent's). Such a member is typed as every deriving class that reflects it
 * reads it (sh_decl_native_schema_descendant_field) and bound in this
 * definition's authored context (sg_bind_dormant_typed), so publishing it under
 * a private identity never leaves the name to a later global lookup. A member
 * no deriving class reflects is never read; one they type differently stays
 * as authored, and a construction reading it judges that use
 * (sg_inline_inherited, sg_inline_spawner). The declaration's own class still
 * decides everything it reflects; the inherit header was visited already. */
static const char sg_dormant_type[] = "<members the class does not reflect>";
typedef struct sg_dormant {
    sh_decl_dependency_schema schema;
    const sh_decl_dependency_schema *view;
    sg_binding *binding;
} sg_dormant;
static int sg_dormant_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    sg_dormant *dormant = context;
    if (type.name != sg_dormant_type) return dormant->view->describe(dormant->view->context, type, shape);
    memset(shape, 0, sizeof(*shape)); shape->kind = SH_DECL_VALUE_OBJECT; return 1;
}
static int sg_dormant_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    sg_dormant *dormant = context;
    if (type.name != sg_dormant_type) return dormant->view->field(dormant->view->context, type, key, out);
    sh_decl_value_type found = {0};
    if (sh_decl_native_schema_descendant_field(dormant->binding->reader->schema,
            dormant->binding->state.name, key, &found) != 1) return 0;
    *out = found; return 1;
}
static int sg_dormant_reference(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length)
{
    sg_dormant *dormant = context;
    if (path && !strcmp(path, "inherit")) return 1;
    return dormant->view->reference(dormant->view->context, path, type, name, length);
}
static int sg_dormant_site(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const sh_decl_reference_site *site)
{
    sg_dormant *dormant = context;
    if (path && !strcmp(path, "inherit")) return 1;
    return dormant->view->reference_site(dormant->view->context, path, type, name, length, site);
}
static void sg_dormant_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{ (void)context; (void)path; (void)type; (void)reason; }
static int sg_dormant_dynamic(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    sg_dormant *dormant = context;
    return dormant->view->dynamic_type(dormant->view->context, base, name, length, selected);
}
static int sg_dormant_empty(void *context, sh_decl_value_type type)
{
    sg_dormant *dormant = context;
    return dormant->view->empty_lookup(dormant->view->context, type);
}
static int sg_bind_dormant(sg_binding *binding, const sh_decl_dependency_schema *view,
    sh_decl_dependency_result *result)
{
    sg_dormant dormant = {{0}, view, binding};
    sh_decl_dependency_result extra = {0};
    dormant.schema.context = &dormant;
    dormant.schema.describe = sg_dormant_describe; dormant.schema.field = sg_dormant_field;
    dormant.schema.reference = sg_dormant_reference; dormant.schema.gap = sg_dormant_gap;
    dormant.schema.dynamic_type = view->dynamic_type ? sg_dormant_dynamic : NULL;
    dormant.schema.reference_site = view->reference_site ? sg_dormant_site : NULL;
    dormant.schema.empty_lookup = view->empty_lookup ? sg_dormant_empty : NULL;
    binding->dormant = 1;
    int ok = sh_decl_entity_state_dependencies(binding->tree, (sh_decl_value_type){sg_dormant_type, ""},
        binding->entity_state, &dormant.schema, &extra);
    binding->dormant = 0;
    if (extra.references > SIZE_MAX - result->references) return 0;
    result->references += extra.references;
    result->aborted |= extra.aborted;
    return ok && !binding->reader->failed;
}

static int sg_bind_envelope(void *context, const sh_decl_dependency_schema *view,
    sh_decl_dependency_result *result);
/* A definition whose class differs from its positional parent's reads the
 * parent's unreflected members under its own class. Publication of the layer
 * that authored such a member bound it only when every deriving class types it
 * alike (sg_bind_dormant); otherwise its text stays as authored for text
 * readers. For this definition, the members its own state leaves in effect
 * (sg_unset) that its class reads and its parent's class does not reflect are
 * bound in the parent's authored context (this scope's resolver: package class
 * views take parents from the same package) and served as a later statement of
 * its own state, which native update mode applies after its own statements.
 * The parent's served text is unchanged, so other readers keep the authored
 * text, and the bound names make the served bytes, identity and edges differ
 * per source context. Returns the extended text, NULL when nothing changes
 * (reader->failed on failure). */
static unsigned char *sg_project_inherited(sg_binding *binding, const sh_decl_entity_class_source *classes,
    const char *class_name, const unsigned char *out, size_t out_length, size_t *projected_length)
{
    sg_walk *reader = binding->reader;
    const sh_decl_dependency_schema *view = sh_decl_native_schema_view(reader->schema);
    sh_decl_entity_headers headers = {0};
    const sh_decl_node **own = NULL;
    sh_decl_node *merged = NULL, *head = NULL, state = {0}, root = {0}, *mini_tree = NULL;
    sh_decl_rebind_result rebound = {0};
    sg_made made = {0};
    char *parent_name = NULL, *mini = NULL, *bound = NULL, reason[512] = "";
    unsigned char *result = NULL;
    size_t own_count = 0, mini_length = 0, bound_length = 0;
    const char *name; size_t length;
    *projected_length = 0;
    sh_decl_entity_split(binding->tree, &headers);
    if (sh_decl_entity_expands(binding->tree) != 1 || !headers.inherit ||
        !sh_decl_reference_literal(headers.inherit, &name, &length) || !length || memchr(name, '"', length)) return NULL;
    if (!(parent_name = malloc(length + 1))) { reader->failed = 1; goto done; }
    memcpy(parent_name, name, length); parent_name[length] = 0;
    if (sg_own_states(&headers, &own, &own_count) < 0) { reader->failed = 1; goto done; }
    for (size_t i = 0; i < own_count; i++) if (!own[i]->compound) goto done;
    if (sg_unbound_inherited(reader, classes, class_name, parent_name, own, own_count, &made, &head, &merged,
        NULL) <= 0) goto done;
    state.key = (char *)binding->entity_state; state.compound = state.assignment = 1; state.children = head;
    root.compound = 1; root.children = &state;
    if (!(mini = sh_decl_tree_write(&root, &mini_length)) ||
        !(mini_tree = sh_decl_tree_parse_ordered((sh_decl_source){mini, mini_length}, reason, sizeof(reason)))) {
        if (!mini) reader->failed = 1;
        goto done;
    }
    {
        sg_binding projection = *binding;
        projection.tree = mini_tree; projection.graph = NULL;
        projection.state = (sh_decl_value_type){class_name, ""};
        projection.every_state = projection.header_only = projection.dormant_members = projection.dormant = 0;
        projection.references = 0; projection.replacement = NULL; projection.local = NULL;
        bound = (char *)sh_decl_rebind_parsed((sh_decl_source){mini, mini_length}, mini_tree, view,
            sg_bind_envelope, &projection, sg_bind_typed, &projection, &bound_length, &rebound);
        binding->references += projection.references;
        free(projection.replacement);
    }
    if (!bound || !rebound.changed) goto done;
    {
        const char *open = memchr(bound, '{', bound_length), *close = NULL, *end = NULL;
        for (size_t i = bound_length; i; i--) if (bound[i - 1] == '}') { close = bound + i - 1; break; }
        for (size_t i = out_length; i; i--) if (out[i - 1] == '}') { end = (const char *)out + i - 1; break; }
        if (!open || !close || close <= open || !end) goto done;
        size_t head_length = (size_t)(end - (const char *)out), inner = (size_t)(close - open - 1);
        if (!(result = malloc(out_length + inner + 2))) { reader->failed = 1; goto done; }
        memcpy(result, out, head_length);
        memcpy(result + head_length, open + 1, inner);
        memcpy(result + head_length + inner, end, out_length - head_length);
        *projected_length = out_length + inner;
        result[*projected_length] = 0;
    }
done:
    sg_made_free(&made);
    free(parent_name); free(mini); free(bound); free((void *)own);
    sh_decl_tree_free(merged); sh_decl_tree_free(mini_tree);
    return reader->failed ? (free(result), NULL) : result;
}

static int sg_bind_envelope(void *context, const sh_decl_dependency_schema *view,
    sh_decl_dependency_result *result)
{
    sg_binding *binding = context;
    memset(result, 0, sizeof(*result));
    if (binding->graph) return sh_decl_graph_dependencies_with(binding->graph, binding->family,
        binding->reader->schema, view, result);
    if (!strcmp(binding->family, "entitydef") && binding->header_only)
        return sh_decl_entity_header_dependencies(binding->tree, view, result);
    if (!strcmp(binding->family, "entitydef")) {
        if (binding->every_state)
            return sh_decl_entity_every_state_dependencies(binding->tree, binding->state, view, result);
        int ok = sh_decl_entity_state_dependencies(binding->tree, binding->state, binding->entity_state, view, result);
        return ok && binding->dormant_members && binding->state.name ? sg_bind_dormant(binding, view, result) : ok;
    }
    return sh_decl_envelope_dependencies(binding->tree, binding->state,
        (sh_decl_value_type){binding->native_class, "*"}, view, result);
}

/* The render program declaration grammar (sh_decl_render_program_sites).
 * Relocation needs a writable token for every reference, so an assembled one
 * (offset SIZE_MAX) refuses here. */
typedef struct sg_shader_sites { sh_resource_binding_site visit; void *context; } sg_shader_sites;
static int sg_shader_site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    sg_shader_sites *sites = context;
    return offset != SIZE_MAX && sites->visit(sites->context, family, name, offset, length);
}
static int sg_shader_source_sites(void *context, const unsigned char *body, size_t length,
    sh_resource_binding_site visit, void *visit_context, char *error, size_t capacity)
{
    sg_walk *reader = context;
    sh_decl_material_schema schema = {reader, sg_material_schema};
    sg_shader_sites sites = {visit, visit_context};
    return sh_decl_render_program_sites((sh_decl_source){(const char *)body, length}, &schema, sg_shader_site, &sites,
        error, capacity);
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
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives, sg_known};
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
    } else if (sh_package_source_leaf(family, name)) {
        out = malloc(length + 1);
        if (out) { memcpy(out, body, length); out[length] = 0; out_length = length; }
    } else {
        int is_graph = 0;
        if (!strcmp(family, "entitydef")) {
            tree = sh_decl_tree_parse_ordered(source, error, capacity);
            if (!tree) { if (result) result->malformed = 1; goto done; }
            sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
            if (!(class_name = sh_decl_entity_tree_class_classified(tree, &classes, &failure, error, capacity))) {
                /* A malformed chain (a cycle, an unsupported header, a class
                 * Parse rejects) is the declaration's own defect; unreadable
                 * parent or class metadata, or allocation, is environment. An
                 * absent parent leaves the content unserved without judging it. */
                if (failure == SH_DECL_ENTITY_MERGE_MALFORMED && result) result->malformed = 1;
                if (failure == SH_DECL_ENTITY_MERGE_ALLOCATION || failure == SH_DECL_ENTITY_MERGE_UNAVAILABLE)
                    binding.environment = 1;
                /* A parentless class the reflected inventory lacks, or a layer
                 * inheriting one, is a valid declaration: Parse never checks
                 * it, and later consumers (the type cache, pools, derived
                 * declarations) still read it; a pooled use by name is
                 * accounted on the walk (sg_own_pool). With no reflected field
                 * schema its state stays opaque and is served as authored with
                 * the coverage mark (never an equivalence; its uses keep their
                 * closure gap), but its positional inherit is a known native
                 * reference: it binds to the parent this package context
                 * serves, exactly as a decided definition's does. */
                if (failure != SH_DECL_ENTITY_MERGE_COVERAGE) goto done;
                error[0] = 0;
                binding.tree = tree; binding.every_state = 1; binding.header_only = 1;
                out = (unsigned char *)sh_decl_rebind_parsed(source, tree, sh_decl_native_schema_view(reader->schema),
                    sg_bind_envelope, &binding, sg_bind_typed, &binding, &out_length, &rebound);
                changed = rebound.changed;
                if (!out && !reader->failed && !error[0]) binding.environment = 1;
                goto done;
            }
            binding.state = (sh_decl_value_type){class_name, ""};
            int stated = sg_entity_state(&binding, tree, &classes);
            if (stated <= 0) { if (stated == -2 && result) result->malformed = 1; goto done; }
            binding.dormant_members = !binding.every_state;
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
        if (out && class_name && binding.entity_state && *binding.entity_state && !reader->failed) {
            size_t projected_length = 0;
            unsigned char *projected = sg_project_inherited(&binding, &classes, class_name, out, out_length,
                &projected_length);
            if (projected) { free(out); out = projected; out_length = projected_length; changed++; }
        }
    }
done:
    if (reader && reader->failed) { free(out); out = NULL; }
    if (!out && !error[0]) snprintf(error, capacity, "%s source bindings could not be completed%s%s",
        family ? family : "resource", reader && reader->report.first_gap[0] ? ": " : "",
        reader ? reader->report.first_gap : "");
    if (result) {
        result->references = binding.references; result->changed = out ? changed : 0;
        result->environment = !out && binding.environment;
        result->coverage = out && binding.every_state;
        if (reader) result->inspection = reader->report;
    }
    if (output_length && out) *output_length = out_length;
    free(binding.replacement); free((char *)binding.entity_state); free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
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
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives, sg_known};
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
        int stated = sg_entity_state(&binding.envelope, tree, &classes);
        if (stated <= 0) { if (stated == -2 && result) result->malformed = 1; goto done; }
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
    free((char *)binding.envelope.entity_state); free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
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
    return sh_package_source_entity_fields_state(reader, name, body, length, fields, field_count,
        output, output_length, NULL, error, capacity);
}
int sh_package_source_entity_fields_state(sh_package_source_reader *reader, const char *name,
    const unsigned char *body, size_t length, const char *const *fields, size_t field_count,
    unsigned char **output, size_t *output_length, char **state, char *error, size_t capacity)
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
    /* Repeated members are native-valid; expansion applies the merge. */
    tree = sh_decl_tree_parse_ordered((sh_decl_source){(const char *)body, length}, error, capacity);
    if (!tree) goto done;
    /* Only a positional, nonempty inherit is a parent (sh_decl_entity_split). */
    sh_decl_entity_headers headers;
    const char *parent_name = NULL; size_t parent_length = 0;
    sh_decl_entity_split(tree, &headers);
    if (!headers.inherit || (sh_decl_tree_literal(headers.inherit, &parent_name, &parent_length) && !parent_length)) {
        status = 0; goto done;
    }
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
    if (!from) { status = 0; goto done; }
    /* The own statement the merge applies to the prepared first statement:
     * under a stateful ancestor the first one of that statement's name (first
     * match), otherwise this definition's first state statement. A new block
     * goes after every statement, never before the positional headers. */
    char *state_name = NULL;
    int merges = sh_decl_entity_merges(tree, &parents, &state_name, error, capacity);
    if (merges < 0) goto done;
    sh_decl_entity_split(tree, &headers);
    sh_decl_node *into = merges ? NULL : (sh_decl_node *)headers.state, **tail = &tree->children;
    for (const sh_decl_node *node = headers.state; merges && node && !into; node = node->next)
        if (!strcmp(node->key, state_name)) into = (sh_decl_node *)node;
    if (into && (!into->compound || !into->assignment)) {
        free(state_name); snprintf(error, capacity, "'%s' is not a state block", into->key); goto done;
    }
    /* Readers of the prepared text select by this state, never by name. */
    if (state && !(*state = _strdup(merges ? state_name : ""))) { free(state_name); goto done; }
    if (!into) {
        into = calloc(1, sizeof(*into));
        if (into) into->key = _strdup(merges ? state_name : "edit");
        if (!into || !into->key) { free(state_name); sh_decl_tree_free(into); goto done; }
        into->compound = into->assignment = 1;
        while (*tail) tail = &(*tail)->next;
        *tail = into;
    }
    free(state_name);
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
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives, sg_known};
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
        int stated = sg_entity_state(&binding.envelope, tree, &classes);
        if (stated <= 0) { if (stated == -2 && result) result->malformed = 1; goto done; }
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
    free((char *)binding.envelope.entity_state); free(class_name); sh_decl_tree_free(tree); sh_decl_graph_close(graph);
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
    /* Map-local play declarations (sg_map_local): local_state 0 unread, 1 read,
     * -1 none. roots holds each entity's inherit as the walk bound it; every
     * root binds before any state site (sg_map_walk). */
    sh_native_inline_generated_set local_names;
    int local_state;
    struct sg_root { char *owner, *name; } *roots;
    size_t root_count, root_capacity;
    char *local_replacement;
    /* R-249: "owner\nreference" of own members equal to their projection that
     * the saved map does not record; they are bound but never observed. */
    char **skip;
    size_t skip_count;
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
    int skipped = 0;
    for (size_t i = 0; map->reference_site.owner && map->reference_site.reference && i < map->skip_count; i++) {
        size_t n = strlen(map->reference_site.owner);
        skipped |= !strncmp(map->skip[i], map->reference_site.owner, n) && map->skip[i][n] == '\n' &&
            !strcmp(map->skip[i] + n + 1, map->reference_site.reference);
    }
    if (map->observed && !map->collecting && map->reference_site.owner && !skipped)
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
static int sg_map_key_empty(void *context, sh_decl_value_type type)
{
    sg_map_binding *map = context;
    return map->native->empty_lookup && map->native->empty_lookup(map->native->context, type);
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
void sh_package_source_reader_project_mode(sh_package_source_reader *reader, int mode)
{ if (reader) reader->project_mode = mode; }
void sh_package_source_reader_variant_sink(sh_package_source_reader *reader, sh_package_variant_sink sink, void *context)
{ if (reader) { reader->variant_sink = sink; reader->variant_context = context; } }
void sh_package_source_reader_gap_sink(sh_package_source_reader *reader, sh_package_gap_sink sink, void *context)
{ if (reader) { reader->gap_sink = sink; reader->gap_context = sink ? context : NULL; } }
void sh_package_source_reader_projection(sh_package_source_reader *reader,
    sh_package_map_projection_resolver resolver, void *context)
{ if (reader) { reader->project = resolver; reader->project_context = context; } }
void sh_package_source_reader_map_local(sh_package_source_reader *reader, const sh_package_map_local_names *names)
{
    if (reader) reader->map_local = names && names->read && names->name && names->release ? names : NULL;
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
/* Declaration names compare as the engine's lookup normalizes them: ASCII case
 * and separators fold, and one leading '/' is dropped (VK 0x141801380). */
static int sg_local_char(char c) { return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int sg_local_equal(const char *a, size_t a_length, const char *b, size_t b_length)
{
    if (a_length && *a == '/' && (a_length < 2 || a[1] != '/')) { a++; a_length--; }
    if (b_length && *b == '/' && (b_length < 2 || b[1] != '/')) { b++; b_length--; }
    if (a_length != b_length) return 0;
    for (size_t i = 0; i < a_length; i++) if (sg_local_char(a[i]) != sg_local_char(b[i])) return 0;
    return 1;
}
/* A name of the play declaration shape "<digits>_<module>/<inherit>_<uniqueId>"
 * for this inherit and the row's uniqueId, whatever its group and module. */
static int sg_local_shape(const sh_native_inline_generated *row, const char *spelling, const char *name, size_t length)
{
    if (!spelling) return 0;
    size_t inherit = strlen(spelling), unique = strlen(row->unique), digits = 0;
    while (digits < length && name[digits] >= '0' && name[digits] <= '9') digits++;
    if (!digits || digits >= length || name[digits] != '_' || length < digits + 3 + inherit + unique) return 0;
    const char *tail = name + length - unique - inherit - 2;
    return tail[0] == '/' && tail[inherit + 1] == '_' && sg_local_equal(tail + 1, inherit, spelling, inherit) &&
        !memcmp(tail + inherit + 2, row->unique, unique);
}
static const char *sg_map_root_name(const sg_map_binding *map, size_t entity)
{
    char owner[48];
    snprintf(owner, sizeof(owner), "entity/%zu", entity);
    for (size_t i = 0; i < map->root_count; i++) if (!strcmp(map->roots[i].owner, owner)) return map->roots[i].name;
    return NULL;
}
static int sg_map_root_record(sg_map_binding *map, const sh_package_provenance_site *location,
    const char *name, size_t length, const char *bound)
{
    if (!location->owner || strncmp(location->owner, "entity/", 7) || !location->reference ||
        strcmp(location->reference, "/k:entityDef/k:inherit")) return 1;
    if (map->root_count == map->root_capacity) {
        size_t next = map->root_capacity ? map->root_capacity * 2 : 32;
        struct sg_root *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(map->roots, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        map->roots = grown; map->root_capacity = next;
    }
    struct sg_root *root = map->roots + map->root_count;
    if (!(root->owner = _strdup(location->owner))) return 0;
    if (bound) root->name = _strdup(bound);
    else if ((root->name = malloc(length + 1)) != NULL) { memcpy(root->name, name, length); root->name[length] = 0; }
    if (!root->name) { free(root->owner); return 0; }
    map->root_count++; return 1;
}
/* An entity's typed state is read when that entity spawns (SetEntityEditState
 * VK 1409d1ef0 / GL 1409d16b0), and the declaration pointer reader then finds
 * the name with makeDefault (VK 1409ca890 / GL 1409ca050). The play map builder
 * has registered every placed entity's play declaration by then, so a state
 * name equal to one of those names that copy. The use binds to that exact
 * occurrence: its name is rewritten for the occurrence's bound inherit, needs
 * no source and records no resource use. A name the map's own entities cannot
 * produce is not map-local and stays unresolved. Inherit headers are read when
 * the copy is parsed during the build, so they never take this route. */
static int sg_map_local(void *context, const char *name, size_t length, const char **replacement)
{
    sg_map_binding *map = context;
    sg_walk *reader = map->binding.reader;
    const sh_package_map_local_names *names = reader->map_local;
    const sh_package_provenance_site *site = &map->reference_site;
    const sh_native_inline_generated *found = NULL, *partial = NULL;
    char reason[384], *generated = NULL;
    if (!names || !site->owner || strncmp(site->owner, "entity/", 7) || !site->reference ||
        strncmp(site->reference, "/k:entityDef/k:state/", 21) || !length || length == SIZE_MAX) return 0;
    if (!map->local_state) {
        int read = names->read(map->json, map->length, &map->local_names);
        if (read < 0) { sg_gap(reader, "map/local", "entityDef", "map-local play declarations could not be read"); return -1; }
        map->local_state = read ? 1 : -1;
    }
    if (map->local_state < 0) return 0;
    for (size_t i = 0; i < map->local_names.count; i++) {
        const sh_native_inline_generated *row = map->local_names.rows + i;
        if (!row->inherit || !row->unique) continue;
        if (!row->module_index || !row->module_name) {
            if (sg_local_shape(row, row->inherit, name, length) ||
                sg_local_shape(row, sg_map_root_name(map, row->entity), name, length)) partial = row;
            continue;
        }
        /* The copy is registered under the inherit the engine receives, the
         * bound one; the authored spelling names the same occurrence. */
        const char *bound = sg_map_root_name(map, row->entity);
        int equal = 0;
        for (int pass = 0; pass < 2 && !equal; pass++) {
            const char *as = pass ? bound : row->inherit;
            if (!as || (pass && !strcmp(as, row->inherit))) continue;
            if (!(generated = names->name(row, as))) { reader->failed = 1; return -1; }
            equal = sg_local_equal(generated, strlen(generated), name, length);
            free(generated); generated = NULL;
        }
        if (!equal) continue;
        if (found) {
            snprintf(reason, sizeof(reason), "map-local play declaration \"%.*s\" names more than one placed entity",
                (int)(length > 200 ? 200 : length), name);
            sg_gap(reader, "map/local", "entityDef", reason); return -1;
        }
        found = row;
    }
    if (!found && partial) {
        snprintf(reason, sizeof(reason), "map-local play declaration \"%.*s\" cannot be confirmed: entity %zu's module "
            "group or loaded module name is not modelled", (int)(length > 200 ? 200 : length), name, partial->entity);
        sg_gap(reader, "map/local", "entityDef", reason); return -1;
    }
    if (!found) return 0;
    const char *bound = sg_map_root_name(map, found->entity);
    if (!(generated = names->name(found, bound ? bound : found->inherit))) { reader->failed = 1; return -1; }
    int kept = sg_local_equal(generated, strlen(generated), name, length);
    if (map->observed && !map->legacy) {
        const char *ignored = NULL;
        if (map->observed(map->context, site, SH_PACKAGE_MAP_LOCAL_FAMILY, generated, &ignored) < 0) {
            free(generated); sg_gap(reader, "map/local", "entityDef", "a map-local play declaration use could not be recorded");
            return -1;
        }
    }
    if (kept) { free(generated); return 1; }
    free(map->local_replacement); map->local_replacement = generated; *replacement = generated;
    return 2;
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
    memset(&map->reference_site, 0, sizeof(map->reference_site));
    if (ok >= 0 && name && !sg_map_root_record(map, location, name, length, ok == 1 ? *replacement : NULL)) {
        map->binding.reader->failed = 1; return -1;
    }
    return ok;
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
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives, sg_known};
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

/* R-249: each placed copy's state site, in document order. */
typedef struct sg_map_row { char *owner, *reference, *klass, *inherit; size_t offset, length; } sg_map_row;
typedef struct sg_map_rows { sg_map_binding *map; const char *json; sg_map_row *rows; size_t count, capacity; int failed; } sg_map_rows;
static int sg_map_row_add(void *context, const char *class_name, const char *inherit, const char *json, size_t length)
{
    sg_map_rows *rows = context;
    const sh_package_provenance_site *location = rows->map ? sg_map_span_find(rows->map, json, length) : NULL;
    if (rows->count == rows->capacity) {
        size_t next = rows->capacity ? rows->capacity * 2 : 16;
        sg_map_row *grown = realloc(rows->rows, next * sizeof(*grown));
        if (!grown) { rows->failed = 1; return 0; }
        rows->rows = grown; rows->capacity = next;
    }
    sg_map_row *row = rows->rows + rows->count++;
    memset(row, 0, sizeof(*row));
    row->offset = (size_t)(json - rows->json); row->length = length;
    if (location && location->owner && location->reference &&
        (!(row->owner = _strdup(location->owner)) || !(row->reference = _strdup(location->reference)))) rows->failed = 1;
    if (class_name && *class_name && !(row->klass = _strdup(class_name))) rows->failed = 1;
    if (inherit && *inherit && !(row->inherit = _strdup(inherit))) rows->failed = 1;
    return !rows->failed;
}
static void sg_map_rows_free(sg_map_rows *rows)
{
    for (size_t i = 0; i < rows->count; i++) {
        free(rows->rows[i].owner); free(rows->rows[i].reference); free(rows->rows[i].klass); free(rows->rows[i].inherit);
    }
    free(rows->rows);
}
/* One JSON value from p; the end of a top-level scalar stops at its delimiter. */
static const char *sg_json_skip(const char *p, const char *end)
{
    int depth = 0, string = 0;
    for (; p < end; p++) {
        if (string) { if (*p == '\\') p++; else if (*p == '"') { string = 0; if (!depth) return p + 1; } continue; }
        if (*p == '"') string = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (!depth) return p; if (!--depth) return p + 1; }
        else if (!depth && *p == ',') return p;
    }
    return depth || string ? NULL : p;
}
#define SG_JSON_SPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n')
/* The top-level member key of object (from its '{'): 1 with its value and the
 * span removing it with one separating comma, 0 absent, -1 malformed. */
static int sg_json_find(const char *object, size_t length, const char *key, size_t *from, size_t *to,
    const char **value, size_t *value_length)
{
    const char *end = object + length, *p = object + 1, *previous = NULL;
    size_t key_length = strlen(key);
    for (;;) {
        while (p < end && SG_JSON_SPACE(*p)) p++;
        if (p >= end || *p == '}') return 0;
        if (*p != '"') return -1;
        const char *member = p, *key_end = sg_json_skip(p, end);
        if (!key_end) return -1;
        int match = (size_t)(key_end - p) == key_length + 2 && !memcmp(p + 1, key, key_length);
        for (p = key_end; p < end && SG_JSON_SPACE(*p); p++) {}
        if (p >= end || *p++ != ':') return -1;
        while (p < end && SG_JSON_SPACE(*p)) p++;
        const char *start = p, *stop = sg_json_skip(p, end);
        if (!stop) return -1;
        for (p = stop; p < end && SG_JSON_SPACE(*p); p++) {}
        const char *comma = p < end && *p == ',' ? p : NULL;
        if (match) {
            *value = start; *value_length = (size_t)(stop - start);
            if (comma) { *from = (size_t)(member - object); *to = (size_t)(comma + 1 - object); }
            else if (previous) { *from = (size_t)(previous - object); *to = (size_t)(stop - object); }
            else { *from = (size_t)(member - object); *to = (size_t)(stop - object); }
            return 1;
        }
        if (!comma) return 0;
        previous = comma; p = comma + 1;
    }
}
static int sg_plain_key(const char *key)
{
    if (!key || !*key) return 0;
    for (; *key; key++) if (!((*key >= 'a' && *key <= 'z') || (*key >= 'A' && *key <= 'Z') ||
        (*key >= '0' && *key <= '9') || *key == '_')) return 0;
    return 1;
}
typedef struct sg_map_projection { sg_walk *reader; const char *owner, *inherit, *member; int authored, called, record;
    const char *layer; } sg_map_projection;
static int sg_map_project_resolve(void *context, const char *slot, const char *family, const char *name,
    const char **replacement)
{
    sg_map_projection *projection = context;
    (void)slot;
    *replacement = NULL; projection->called = 1;
    int status = projection->reader->project(projection->reader->project_context, projection->record,
        projection->owner, projection->inherit, projection->layer, projection->member, family, name, replacement);
    if (status == 2) { projection->authored = 1; *replacement = NULL; return 0; }
    if (status < 0) projection->reader->failed = 1;
    return status;
}
/* One unbound inherited member a copy's class reads as a declaration, bound
 * for that copy by the projection resolver in its canonical family: 1 with
 * *bound when its name changes, 0 when it keeps its spelling, the saved map
 * authors that site, or the class reads it as anything but a declaration. */
static int sg_map_project_member(sg_walk *reader, const char *consumer, const sh_decl_node *member,
    sg_map_projection *projection, char **bound)
{
    const sh_decl_dependency_schema *view = sh_decl_native_schema_view(reader->schema);
    sh_decl_value_type field = {0};
    const char *family = NULL, *literal = NULL, *replacement = NULL;
    char *canonical = NULL, *spelling = NULL;
    size_t length = 0;
    int status;
    *bound = NULL;
    if (member->compound || !sh_decl_reference_literal(member, &literal, &length) || !length ||
        view->field(view->context, (sh_decl_value_type){consumer, ""}, member->key, &field) != 1 || !field.name ||
        sh_decl_native_schema_reader(reader->schema, field) != SH_DECL_READER_DECL ||
        sh_decl_native_registry_class_type(reader->registry, field.name, &family) <= 0 || !family) return 0;
    if (!(canonical = sg_name(family)) || !(spelling = malloc(length + 1))) { free(canonical); return -1; }
    memcpy(spelling, literal, length); spelling[length] = 0;
    status = sg_map_project_resolve(projection, NULL, canonical, spelling, &replacement);
    if (status == 1 && replacement && strcmp(replacement, spelling)) status = (*bound = _strdup(replacement)) ? 1 : -1;
    else if (status > 0) status = 0;
    free(canonical); free(spelling);
    return status;
}
/* R-255: a copy's full bound assignment over its parent: each kept member its
 * class reads as a declaration, whether or not its own state sets it, bound for
 * the copy (op 3). When any is bound and the parent's variant stating them is
 * served (or the sink accepts it), *target names that variant and each bound
 * member's projection is recorded (op 1; an authored site records none).
 * 1 named, 0 not, -1 failure. */
/* The canonical path of the declaration name a member key of consumer reads, or NULL. */
static char *sg_member_path(sg_walk *reader, const char *consumer, const char *key, const char *name)
{
    const sh_decl_dependency_schema *view = sh_decl_native_schema_view(reader->schema);
    sh_decl_value_type field = {0};
    const char *family = NULL;
    char *canonical, *path;
    if (view->field(view->context, (sh_decl_value_type){consumer, ""}, key, &field) != 1 || !field.name ||
        sh_decl_native_registry_class_type(reader->registry, field.name, &family) <= 0 || !family ||
        !(canonical = sg_name(family))) return NULL;
    path = sg_lookup_path(reader, canonical, name);
    free(canonical); return path;
}
static int sg_variant_site(const sg_walk *reader, size_t carrier, const char *key)
{
    char site[160];
    if (!reader->variant_folding || snprintf(site, sizeof(site), "%zu\n%s", carrier, key) >= (int)sizeof(site)) return 0;
    for (size_t i = 0; i < reader->variant_site_count; i++) if (!strcmp(reader->variant_sites[i], site)) return 1;
    return 0;
}
static int sg_map_variant(sg_map_binding *map, const sh_decl_entity_class_source *classes, const sg_map_row *row,
    char **target)
{
    sg_walk *reader = map->binding.reader;
    sh_decl_node *head = NULL, *merged = NULL;
    sg_made made = {0};
    const sh_compiled_resource **layers = NULL;
    sh_package_variant_member *members = NULL;
    char **dependencies = NULL;
    size_t count = 0, index = 0, text_length = 0;
    unsigned char *text = NULL;
    char member_site[384], name[69];
    int result = 0, served = 0;
    *target = NULL;
    int kept = sg_unbound_inherited(reader, classes, row->klass, row->inherit, NULL, 0, &made, &head, &merged, &layers);
    if (kept < 0 || (kept && (!(members = calloc((size_t)kept, sizeof(*members))) ||
        !(dependencies = calloc((size_t)kept + 1, sizeof(*dependencies)))))) { result = -1; goto done; }
    for (int pass = 0; pass < 2 && result >= 0; pass++) {
        index = 0;
        for (sh_decl_node *member = kept > 0 ? head : NULL; member; member = member->next, index++) {
            char *bound = NULL;
            if (!sg_plain_key(member->key) || member->compound || strlen(member->key) > 120) continue;
            snprintf(member_site, sizeof(member_site), "%s/k:%s", row->reference, member->key);
            sg_map_projection projection = {reader, row->owner, row->inherit, member_site, 0, 0, pass ? 1 : 3,
                layers && layers[index] ? layers[index]->name : NULL};
            int status = sg_map_project_member(reader, row->klass, member, &projection, &bound);
            if (status < 0) { result = -1; break; }
            if (status == 1 && !pass) {
                dependencies[count + 1] = sg_member_path(reader, row->klass, member->key, bound);
                members[count].key = member->key; members[count++].value = bound;
            } else free(bound);
        }
        if (pass || result < 0) break;
        /* A spelling no variant can state keeps the inserted projections. */
        if (!count || !(text = sh_package_variant_text(row->inherit, members, count, &text_length)) ||
            !sh_package_variant_name(0, text, text_length, name)) goto done;
        served = sh_package_variant_served(reader->compiled, "entitydef", name);
        if (!served && reader->variant_sink) {
            /* Its generated layer keeps exactly the declarations its text names. */
            int complete = (dependencies[0] = sg_lookup_path(reader, "entitydef", row->inherit)) != NULL;
            for (size_t i = 0; complete && i < count; i++) complete = dependencies[i + 1] != NULL;
            served = complete ? reader->variant_sink(reader->variant_context, name, text, text_length,
                (const char *const *)dependencies, count + 1) : 0;
        }
        if (served <= 0) { result = served < 0 ? -1 : 0; goto done; }
    }
    if (result >= 0) result = (*target = _strdup(name)) ? 1 : -1;
done:
    for (size_t i = 0; i < count; i++) free(members[i].value);
    for (size_t i = 0; dependencies && i <= count; i++) free(dependencies[i]);
    free(members); free(text); free(dependencies);
    sg_made_free(&made); sh_decl_tree_free(merged); free((void *)layers);
    return result;
}
/* R-249 projection of each placed copy (rows, document order). An own member
 * equal to the projection the saved map recorded for it (projection
 * provenance, op 2 of the resolver) is synthesized; any other own value is
 * authored. op 0 (capture) binds synthesized members unobserved (map->skip) and
 * records them again as projections; op 1 removes them before binding; op 2
 * inserts and records each projection the copy's own state does not set.
 * Returns the document (edited by op 1/2). */
static char *sg_map_project(sg_map_binding *map, const char *json, size_t length, sg_map_rows *rows, int op,
    size_t *out_length, int *changed)
{
    sg_walk *reader = map->binding.reader;
    sh_decl_entity_class_source classes = {reader, sg_read_parent, sg_derives, sg_known};
    static const char suffix[] = "/k:state/k:edit";
    char *doc = malloc(length + 1), **targets = op == 2 ? calloc(rows->count + 1, sizeof(*targets)) : NULL;
    size_t size = length, named = 0; ptrdiff_t shift = 0;
    *changed = 0;
    if (!doc || (op == 2 && !targets)) { free(doc); free(targets); reader->failed = 1; return NULL; }
    memcpy(doc, json, length); doc[length] = 0;
    for (size_t r = 0; r < rows->count && !reader->failed; r++) {
        const sg_map_row *row = rows->rows + r;
        size_t at = (size_t)((ptrdiff_t)row->offset + shift), span = row->length;
        size_t reference_length = row->reference ? strlen(row->reference) : 0, trim = sizeof(suffix) - 1;
        char inherit_site[256], member_site[384], reason[512] = "";
        if (!row->owner || !row->klass || !row->inherit || reference_length < trim || reference_length > 200 ||
            strcmp(row->reference + reference_length - trim, suffix) || doc[at] != '{') continue;
        snprintf(inherit_site, sizeof(inherit_site), "%.*s/k:inherit", (int)(reference_length - trim), row->reference);
        sh_decl_node *own = op == 2 ? sh_decl_carrier_tree(doc + at, span, NULL, NULL, reason, sizeof(reason)) : NULL;
        if (op == 2 && !own) continue;
        const sh_decl_node *blocks[1] = {own};
        sh_decl_node *head = NULL, *merged = NULL;
        sg_made made = {0};
        const sh_compiled_resource **layers = NULL;
        size_t index = 0;
        map->inherit_site = (sh_package_provenance_site){SH_PACKAGE_USE_OBJECT, row->owner, inherit_site};
        if (op == 2) {
            int variant = sg_map_variant(map, &classes, row, targets + r);
            if (variant < 0) reader->failed = 1;
            if (variant) { named++; map->inherit_site = (sh_package_provenance_site){0}; sh_decl_tree_free(own); continue; }
        }
        int count = sg_unbound_inherited(reader, &classes, row->klass, row->inherit, blocks, own ? 1 : 0,
            &made, &head, &merged, &layers);
        for (sh_decl_node *member = count > 0 ? head : NULL; member && !reader->failed; member = member->next, index++) {
            if (!sg_plain_key(member->key) || member->compound || strlen(member->key) > 120) continue;
            snprintf(member_site, sizeof(member_site), "%s/k:%s", row->reference, member->key);
            sg_map_projection projection = {reader, row->owner, row->inherit, member_site, 0, 0, op != 1,
                layers && layers[index] ? layers[index]->name : NULL};
            char *bound = NULL, *piece = NULL;
            const char *recorded = NULL;
            size_t from = 0, to = 0, value_length = 0;
            const char *value = NULL;
            int present = op == 2 ? 0 : sg_json_find(doc + at, span, member->key, &from, &to, &value, &value_length);
            int synthesized = 0;
            if (present > 0) {
                int saved = reader->project(reader->project_context, 2, row->owner, row->inherit, NULL, member_site,
                    NULL, NULL, &recorded);
                if (saved < 0) { reader->failed = 1; break; }
                synthesized = (saved == 1 && recorded && value_length == strlen(recorded) + 2 &&
                    !memcmp(value + 1, recorded, value_length - 2)) || (op == 0 && sg_variant_site(reader, r, member->key));
            }
            if (op != 2 && !synthesized) continue;
            int status = op == 1 ? 1 : sg_map_project_member(reader, row->klass, member, &projection, &bound);
            if (status < 0) { reader->failed = 1; break; }
            int equal = synthesized;
            if (op == 0 && equal && !projection.authored) {
                char *entry = malloc(strlen(row->owner) + strlen(member_site) + 2);
                char **grown = entry ? realloc(map->skip, (map->skip_count + 1) * sizeof(*grown)) : NULL;
                if (!grown) { free(entry); reader->failed = 1; }
                else { sprintf(entry, "%s\n%s", row->owner, member_site); map->skip = grown; map->skip[map->skip_count++] = entry; }
            } else if (op == 1 && equal) {
                memmove(doc + at + from, doc + at + to, size - (at + to) + 1);
                size -= to - from; span -= to - from; shift -= (ptrdiff_t)(to - from); *changed = 1;
            } else if (op == 2 && status == 1) {
                size_t first = 1, add;
                while (first < span && SG_JSON_SPACE(doc[at + first])) first++;
                int empty = first < span && doc[at + first] == '}';
                char *grown = NULL;
                add = strlen(member->key) + strlen(bound) + 5 + (empty ? 0 : 1);
                if (!(piece = malloc(add + 1)) || !(grown = realloc(doc, size + add + 1))) reader->failed = 1;
                else {
                    doc = grown;
                    snprintf(piece, add + 1, "\"%s\":\"%s\"%s", member->key, bound, empty ? "" : ",");
                    memmove(doc + at + 1 + add, doc + at + 1, size - at);
                    memcpy(doc + at + 1, piece, add);
                    size += add; span += add; shift += (ptrdiff_t)add; *changed = 1;
                }
            }
            free(piece); free(bound);
        }
        map->inherit_site = (sh_package_provenance_site){0};
        sg_made_free(&made); sh_decl_tree_free(merged); sh_decl_tree_free(own); free((void *)layers);
    }
    if (named && !reader->failed) {
        const char **expected = calloc(rows->count + 1, sizeof(*expected));
        size_t retargeted_length = 0;
        char *retargeted = NULL;
        for (size_t i = 0; expected && i < rows->count; i++) expected[i] = rows->rows[i].inherit;
        if (expected) retargeted = sh_package_variant_retarget(doc, size, expected, (const char *const *)targets,
            rows->count, &retargeted_length);
        free((void *)expected);
        if (!retargeted) reader->failed = 1;
        else { free(doc); doc = retargeted; size = retargeted_length; *changed = 1; }
    }
    for (size_t i = 0; targets && i < rows->count; i++) free(targets[i]);
    free(targets);
    if (reader->failed) { free(doc); return NULL; }
    *out_length = size; return doc;
}

static char *sg_bind_map(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_binding_resolver legacy,
    sh_package_map_binding_resolver occurrence, sh_package_map_binding_resolver observed, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity)
{
    sg_map_binding map = {0}; sh_decl_rebind_result rebound = {0};
    char *out = NULL, fallback[1024], *restart = NULL; size_t out_length = 0, restart_length = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!reader || !json || (!legacy && !occurrence)) return NULL;
    /* R-255: a captured live document naming served variants is bound in its
     * parent form; each member a copy inherits from its variant is stated and
     * bound as a projection (sg_map_project op 0). */
    if (reader->project && reader->project_mode == 1 && !reader->variant_folding) {
        size_t folded_length = 0, site_count = 0;
        int changed = 0;
        char **sites = NULL;
        char *folded = sh_package_variant_fold_sites(reader->compiled, json, length, 1, &folded_length, &changed,
            &sites, &site_count);
        if (!folded) { snprintf(error, capacity, "a document naming parent variants cannot be read in its parent form"); return NULL; }
        if (changed) {
            reader->variant_folding = 1; reader->variant_sites = sites; reader->variant_site_count = site_count;
            out = sg_bind_map(reader, folded, folded_length, legacy, occurrence, observed, context,
                output_length, result, error, capacity);
            reader->variant_folding = 0; reader->variant_sites = NULL; reader->variant_site_count = 0;
        }
        sh_package_variant_sites_free(sites, site_count); free(folded);
        if (changed) return out;
    }
    /* A document bound for a plan is read in its parent form too. One that
     * already names the plan's variants is that plan's own binding (a prefab
     * delivery after sh_package_document_bind); a variant states only members
     * its copies do not set, so naming the parent again restores the exact
     * document it was bound from, and each copy keeps its use. */
    if (reader->project && reader->project_mode == 2) {
        size_t folded_length = 0;
        int changed = 0;
        char *folded = sh_package_variant_fold(reader->compiled, json, length, 0, &folded_length, &changed);
        if (!folded) { snprintf(error, capacity, "a document naming parent variants cannot be read in its parent form"); return NULL; }
        if (changed) out = sg_bind_map(reader, folded, folded_length, legacy, occurrence, observed, context,
            output_length, result, error, capacity);
        free(folded);
        if (changed) return out;
    }
    reader->failed = 0; reader->error = error; reader->capacity = capacity;
    memset(&reader->report, 0, sizeof(reader->report));
    reader->current_type = "map"; reader->current_name = "bindings";
    reader->saved_binding = sg_map_resolve; reader->saved_context = &map;
    map.binding.reader = reader; map.binding.resolver = sg_map_observe; map.binding.context = &map;
    map.binding.local = sg_map_local; map.binding.local_context = &map;
    map.legacy = legacy; map.occurrence = occurrence; map.observed = observed; map.context = context;
    map.json = json; map.length = length;
    map.native = sh_decl_native_schema_view(reader->schema);
    map.keyed = (sh_decl_dependency_schema){&map, sg_map_key_describe, sg_map_key_field,
        map.native->reference, sg_map_key_gap, sg_map_key_dynamic, map.native->reference_site};
    /* A map's "" is the lookup of _emptyName at its site, bound like any name. */
    map.keyed.empty_lookup = sg_map_key_empty;
    if (!sh_package_map_reference_sites(json, length, sg_map_collect_root, &map) ||
        !sh_package_map_state_sites(json, length, sg_map_collect_state, &map) || !sg_map_locate(&map)) goto done;
    /* R-249: a document the engine loads drops synthesized members before
     * binding (and is bound again without them); a capture binds them unobserved. */
    if (reader->project && reader->project_mode) {
        sg_map_rows rows = {&map, json};
        int changed = 0;
        char *prepared = sh_package_map_state_sites(json, length, sg_map_row_add, &rows) && !rows.failed ?
            sg_map_project(&map, json, length, &rows, reader->project_mode == 2 ? 1 : 0, &restart_length, &changed) : NULL;
        sg_map_rows_free(&rows);
        if (!prepared) { reader->failed = 1; goto done; }
        if (changed) { restart = prepared; goto done; }
        free(prepared); restart_length = 0;
    }
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
    /* R-249: each copy's projections join its state after binding. Copies keep
     * document order, so the input's located owners pair with the output. */
    if (out && reader->project && reader->project_mode == 2 && !reader->failed) {
        sg_map_rows in = {&map, json}, bound = {NULL, out};
        size_t projected_length = 0;
        int changed = 0;
        char *projected = NULL;
        if (sh_package_map_state_sites(json, length, sg_map_row_add, &in) && !in.failed &&
            sh_package_map_state_sites(out, out_length, sg_map_row_add, &bound) && !bound.failed && in.count == bound.count) {
            for (size_t i = 0; i < in.count; i++) {
                bound.rows[i].owner = in.rows[i].owner; bound.rows[i].reference = in.rows[i].reference;
                in.rows[i].owner = in.rows[i].reference = NULL;
            }
            projected = sg_map_project(&map, out, out_length, &bound, 2, &projected_length, &changed);
        }
        sg_map_rows_free(&in); sg_map_rows_free(&bound);
        if (!projected) reader->failed = 1;
        else if (changed) { free(out); out = projected; out_length = projected_length; rebound.changed++; }
        else free(projected);
    }
done:
    if (reader->failed) { free(out); out = NULL; }
    if (!out && !*error) snprintf(error, capacity, "typed map bindings could not be completed%s%s",
        reader->report.first_gap[0] ? ": " : "", reader->report.first_gap);
    if (output_length && out) *output_length = out_length;
    if (result) {
        result->references = map.binding.references; result->changed = out ? rebound.changed : 0;
        result->inspection = reader->report;
    }
    free(map.binding.replacement); free(map.key_replacement); free(map.local_replacement);
    for (size_t i = 0; i < map.root_count; i++) { free(map.roots[i].owner); free(map.roots[i].name); }
    free(map.roots);
    if (map.local_state > 0 && reader->map_local) reader->map_local->release(&map.local_names);
    sh_map_binding_sites_clear(map.spans, map.span_count); free(map.spans);
    for (size_t i = 0; i < map.skip_count; i++) free(map.skip[i]);
    free(map.skip);
    reader->saved_binding = NULL; reader->saved_context = NULL;
    reader->error = NULL; reader->capacity = 0;
    if (restart) {
        free(out);
        out = sg_bind_map(reader, restart, restart_length, legacy, occurrence, observed, context,
            output_length, result, error, capacity);
        free(restart);
    }
    return out;
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
    observer = (sh_decl_dependency_observer){&walk, sg_reference, sg_field_gap, NULL, sg_empty_names};
    walk.registry = sh_decl_native_registry_open(candidate, error, capacity);
    walk.schema = sh_decl_native_schema_open(metadata, observer, error, capacity);
    if (!walk.registry || !walk.schema) goto done;
    sh_decl_native_schema_attach_registry(walk.schema, walk.registry);
    walk.map_text = json; walk.map_length = length;
    if (!sh_package_map_reference_sites(json, length, sg_map_site, &walk) ||
        !sh_package_map_states(json, length, sg_inline, &walk) || !sg_world_root(&walk)) goto done;
    for (size_t i = 0; i < walk.queue.count; i++) {
        /* Strings have independent ownership: enqueue may move queue.items. */
        const char *type = walk.queue.items[i].type, *name = walk.queue.items[i].name;
        walk.current = i;
        if (!sg_inspect(&walk, type, name)) goto done;
    }
    /* A spawner may spawn an AI and an AI pre-pool may list a spawner: route
     * both until neither changes. */
    for (size_t round = 0; round <= walk.queue.count && !walk.failed; round++) {
        int spawned = sg_spawner_routes(&walk);
        if (!sg_ai_routes(&walk) && !spawned) break;
    }
    if (walk.failed) goto done;
    /* A gap of an identity only unchanged SnapMap originals name is native;
     * one the map or a candidate also names counts. */
    for (size_t i = 0; i < walk.deferred_count; i++) {
        if (!(walk.candidate[walk.deferred[i].index] & SG_CANDIDATE)) { walk.report.native_gaps++; continue; }
        walk.report.gaps++;
        if (!walk.report.first_gap[0])
            snprintf(walk.report.first_gap, sizeof(walk.report.first_gap), "%s", walk.deferred[i].reason);
    }
    for (size_t i = 0; i < walk.deferred_path_count; i++)
        if (walk.candidate[walk.deferred_paths[i].identity] & SG_CANDIDATE) walk.required[walk.deferred_paths[i].path] = 1;
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
    walk.report.pool_settings_state = walk.pool_settings_state;
    if (walk.pool_settings_state > 0) walk.report.pool_settings = walk.pool_settings;
    if (report) *report = walk.report;
    if (walk.local_state > 0) sh_native_inline_generated_free(&walk.local_names);
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
