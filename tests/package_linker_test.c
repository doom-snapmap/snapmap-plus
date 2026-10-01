#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_linker.h"
#include "package_link_inputs.h"
#include "package_native_names.h"
#include "swf_binary.h"
#include "package_scope.h"
#include "map_provenance.h"
#include "decl_rebind.h"
#include "package_fixture.h"

static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *out)
{
    (void)context; memset(out, 0, sizeof(*out));
    out->kind = !strcmp(type.name, "Entity") ? SH_DECL_VALUE_OBJECT :
        !strcmp(type.name, "Material") ? SH_DECL_VALUE_REFERENCE : SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type type, const char *name, sh_decl_value_type *out)
{
    (void)context; (void)type;
    *out = !strcmp(name, "material") ? (sh_decl_value_type){"Material", "*"} : (sh_decl_value_type){"int", ""};
    return 1;
}
static const sh_decl_dependency_schema schema = {NULL, describe, field};

/* Many bindings of one file, in bind order: the slots and targets binding
 * reports, and the same as caller-supplied sites (target unit indices). */
typedef struct plan_ref { const char *slot, *target; } plan_ref;
typedef struct slot_plan {
    const sh_compiled_resource *file;
    const plan_ref *refs;
    const sh_package_link_edge *sites;
    size_t count;
} slot_plan;
typedef struct fixture {
    sh_package_link_unit units[32];
    size_t count, assigned, bound;
    int unresolved, collision, changed_source, omit_replay, bad_inspection, abort_bind, shared_name;
    int original_paths, alias_rewrite;
    int member_fault, repeat_slot, native_originals;
    int original_bindings[32];
    unsigned char *mutate_witness;
    sh_package_link_edge *mutate_implicit;
    const slot_plan *plans;
    size_t plan_count;
} fixture;
typedef struct bridge { sh_package_link_reference reference; void *context; } bridge;
static int decl_reference(void *context, const char *slot, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    bridge *b = context;
    char *copy = malloc(length + 1); int result;
    assert(copy && !strcmp(type.name, "Material"));
    memcpy(copy, name, length); copy[length] = 0;
    result = b->reference(b->context, slot, "material", copy, replacement);
    free(copy); return result;
}
static int resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{
    fixture *f = context; (void)error; (void)capacity;
    if (f->unresolved) return 0;
    for (size_t i = 0; i < f->count; i++) if (
        f->units[unit].compilation == f->units[i].compilation &&
        !strcmp(f->units[i].family, family) && !strcmp(f->units[i].name, name)) {
        *target = i; return 1;
    }
    return 0;
}
static unsigned char *bind_file(void *context, size_t unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference,
    void *reference_context, size_t *output_length, char *error, size_t capacity)
{
    fixture *f = context;
    unsigned char *result;
    (void)file; (void)error; (void)capacity; f->bound++;
    if (f->abort_bind) return NULL;
    for (size_t p = 0; p < f->plan_count; p++) if (f->plans[p].file == file) {
        for (size_t k = 0; k < f->plans[p].count; k++) {
            const char *replacement = NULL;
            if (reference(reference_context, f->plans[p].refs[k].slot, "material", f->plans[p].refs[k].target,
                &replacement) < 0) return NULL;
        }
        result = malloc(length + 1); assert(result);
        memcpy(result, body, length); result[length] = 0; *output_length = length; return result;
    }
    if (f->repeat_slot && !strcmp(f->units[unit].family, "entitydef")) {
        /* Two assignments of one field in one file, as the native reader sees them. */
        for (int k = 0; k < 2; k++) {
            const char *replacement = NULL;
            if (reference(reference_context, "edit.material", "material", "materials/stone", &replacement) < 0) return NULL;
        }
        result = malloc(length + 1); assert(result);
        memcpy(result, body, length); result[length] = 0; *output_length = length; return result;
    }
    if (!strcmp(f->units[unit].family, "entitydef") && !(f->assigned && f->omit_replay)) {
        bridge b = {reference, reference_context};
        sh_decl_rebind_result details;
        result = (unsigned char *)sh_decl_rebind_entity((sh_decl_source){(char *)body, length},
            (sh_decl_value_type){"Entity", ""}, &schema, decl_reference, &b, output_length, &details);
    } else {
        result = malloc(length + 1); assert(result);
        memcpy(result, body, length); result[length] = 0; *output_length = length;
    }
    if (result && f->bad_inspection && length) result[0] ^= 1;
    if (result && f->alias_rewrite && f->assigned && unit && length) result[0] ^= 1;
    return result;
}
static int assign(void *context, size_t unit, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    fixture *f = context;
    char hex[65];
    (void)error; (void)capacity; f->assigned++;
    if (f->mutate_witness && f->assigned == 1) *f->mutate_witness ^= 1;
    if (f->mutate_implicit && f->assigned == 1) f->mutate_implicit->target ^= 1;
    f->original_bindings[unit] = original_binding;
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity[i]);
    if (!count) { *name = _strdup(f->units[unit].name); return *name != NULL; }
    *name = malloc(strlen(f->units[unit].name) + 70); assert(*name);
    int native = f->original_paths || f->units[unit].fixed_name || (f->native_originals && original_binding);
    if (f->shared_name || native) sprintf(*name, "%s", f->units[unit].name);
    else sprintf(*name, "smp/%s/%s", f->collision ? "same" : hex, f->units[unit].name);
    *paths = calloc(count, sizeof(**paths)); assert(*paths);
    for (size_t i = 0; i < count; i++) {
        (*paths)[i] = malloc(strlen(files[i]->engine_path) + strlen(*name) + 80); assert((*paths)[i]);
        if (native) sprintf((*paths)[i], "%s", files[i]->engine_path);
        else if (files[i]->type) sprintf((*paths)[i], "generated/decls/%s/%s.decl", files[i]->type, *name);
        else sprintf((*paths)[i], "linked/%s/%s", hex, files[i]->engine_path);
    }
    if (f->changed_source) {
        /* Mutating an in-memory prepared input simulates a bad caller snapshot. */
        sh_compiled_resource *resource = (sh_compiled_resource *)files[0];
        if (resource->body && resource->body_length) resource->body[0] ^= 1;
    }
    return 1;
}
/* Every fixture unit binds through the fixed resolve() over immutable bytes. */
static int stable_units(void *context, size_t unit) { (void)context; (void)unit; return 1; }
static int available(void *context, const char *path, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    assert(!strstr(path, "/smp/") && !strstr(path, "linked/"));
    return *(int *)context;
}

static int assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    fixture *f = context; (void)error; (void)capacity;
    *names = calloc(count, sizeof(**names)); assert(*names);
    for (size_t i = 0; i < count; i++) {
        size_t j;
        for (j = 0; j < f->units[unit].file_count; j++) if (files[i] == f->units[unit].files[j]) break;
        assert(j < f->units[unit].file_count);
        const sh_package_link_file_identity *member = f->units[unit].file_identities + j;
        int primary = !member->family || (!strcmp(member->family, f->units[unit].family) &&
            !strcmp(member->name, f->units[unit].name));
        const char *assigned = primary ? name : f->original_paths ? member->name : paths[i];
        if (f->member_fault == 1 && !primary) continue;
        if (f->member_fault == 2 && primary) assigned = "incorrect-primary";
        if (f->member_fault == 3 && !primary) assigned = "shared-companion";
        (*names)[i] = _strdup(assigned); assert((*names)[i]);
        if (f->member_fault == 4) return 0; /* Partly allocated failure. */
    }
    return 1;
}

typedef struct native_fixture {
    const char *colliding[2];
    uint32_t occupied[2];
    size_t checks, collisions;
    int unavailable, invalid, constant, all_occupied, replace_shared;
    const sh_package_link_output *resident;
    const char *shared_original;
} native_fixture;

/* An injected ID space deliberately unrelated to the engine's algorithm. */
static uint32_t test_native_id(const char *name)
{
    uint32_t id = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) id = (id ^ *p) * 16777619u;
    return id ? id : 1;
}

static int native_check(void *context, const char *family, const char *name,
    const unsigned char binding[32], int original, uint32_t *id, char *error, size_t capacity)
{
    native_fixture *native = context;
    (void)family; (void)binding; (void)original; native->checks++;
    if (native->unavailable) { snprintf(error, capacity, "native registry snapshot unavailable"); return -1; }
    *id = native->invalid ? 0 : test_native_id(name);
    if (native->constant || (native->colliding[0] && !strcmp(name, native->colliding[0])) ||
        (native->colliding[1] && !strcmp(name, native->colliding[1]))) *id = 17;
    if (native->all_occupied || *id == native->occupied[0] || *id == native->occupied[1]) {
        native->collisions++; return 0;
    }
    if (native->shared_original && !strcmp(name, native->shared_original)) return SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED;
    if (native->resident) for (size_t i = 0; i < native->resident->classes.count; i++)
        if (!strcmp(name, native->resident->names[i])) return SH_PACKAGE_LINK_REGISTRY_OCCUPIED;
    return native->replace_shared ? SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED : SH_PACKAGE_LINK_REGISTRY_AVAILABLE;
}

/* The developer report of a refused native identity: every line it notes. */
static char collision_notes[16384];
static void collision_note(void *context, const char *line)
{
    size_t used = strlen(collision_notes);
    (void)context;
    if (used + strlen(line) + 2 < sizeof(collision_notes)) { strcat(collision_notes, line); strcat(collision_notes, "\n"); }
}
static void collision_describe(void *context, size_t unit, char *out, size_t capacity)
{ (void)context; snprintf(out, capacity, "fixture unit %zu", unit); }

static void native_collisions(const sh_package_sources *sources, fixture *f,
    sh_package_link_adapter *adapter, const size_t entities[3], const size_t materials[3])
{
    sh_package_link_output out = {0};
    native_fixture native = {0};
    sh_package_link_registry registry = {&native, native_check};
    sh_package_link_unit saved[7];
    char *initial[6], *stable[6], error[512];
    memcpy(saved, f->units, sizeof(saved));
    assert(sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    for (size_t i = 0; i < f->count; i++) { initial[i] = _strdup(out.names[out.classes.nodes[i]]); assert(initial[i]); }
    assert(strcmp(initial[materials[0]], initial[materials[1]]));
    native.colliding[0] = initial[materials[0]]; native.colliding[1] = initial[materials[1]];
    adapter->registry = &registry;
    assert(sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    /* Different alias strings that occupy one native ID must still relocate. */
    assert((strcmp(initial[materials[0]], out.names[out.classes.nodes[materials[0]]]) != 0) +
        (strcmp(initial[materials[1]], out.names[out.classes.nodes[materials[1]]]) != 0) == 1);
    assert(native.checks == out.classes.count + 1);
    for (size_t i = 0; i < f->count; i++) { stable[i] = _strdup(out.names[out.classes.nodes[i]]); assert(stable[i]); }
    for (size_t i = 0; i < f->count; i++) f->units[i] = saved[f->count - i - 1];
    assert(sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    for (size_t i = 0; i < f->count; i++) assert(!strcmp(stable[f->count - i - 1], out.names[out.classes.nodes[i]]));
    memcpy(f->units, saved, sizeof(saved));
    for (size_t i = 0; i < f->count; i++) f->units[i].original_binding = 1;
    f->units[materials[1]].original_binding = 0;
    assert(sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!strcmp(initial[materials[0]], out.names[out.classes.nodes[materials[0]]]));
    assert(strcmp(initial[materials[1]], out.names[out.classes.nodes[materials[1]]]));
    memcpy(f->units, saved, sizeof(saved));
    memset(&native, 0, sizeof(native));
    /* Baseline and still-live registries each reserve an ID, even when their
     * stored resource names differ from the candidate's alias spelling. */
    native.occupied[0] = test_native_id(initial[entities[0]]);
    native.occupied[1] = test_native_id(initial[entities[1]]);
    assert(sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(native.collisions == 2);
    assert(strcmp(initial[entities[0]], out.names[out.classes.nodes[entities[0]]]));
    assert(strcmp(initial[entities[1]], out.names[out.classes.nodes[entities[1]]]));
    memset(&native, 0, sizeof(native)); native.unavailable = 1;
    assert(!sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "snapshot unavailable") && !out.compiled && !out.names);
    memset(&native, 0, sizeof(native)); native.invalid = 1;
    assert(!sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "verification") && !out.compiled);
    memset(&native, 0, sizeof(native)); native.constant = 1;
    assert(!sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled && !out.names);
    memset(&native, 0, sizeof(native)); native.all_occupied = 1;
    for (size_t i = 0; i < f->count; i++) f->units[i].original_binding = 1;
    assert(!sh_package_link(sources, f->units, f->count, adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "original resource") && native.checks == 1 && !out.compiled);
    memcpy(f->units, saved, sizeof(saved)); adapter->registry = NULL;
    for (size_t i = 0; i < f->count; i++) { free(initial[i]); free(stable[i]); }
    sh_package_link_output_free(&out);
}

static void repeated_native_bindings(const sh_package_sources *sources, const fixture *source,
    const size_t entities[3], const size_t materials[3])
{
    fixture f = *source; native_fixture native = {0}; char error[1024], directory[MAX_PATH + 40];
    sh_package_link_registry registry = {&native, native_check};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign, &registry};
    sh_package_link_output first = {0}, next = {0};
    snprintf(directory, sizeof(directory), "%s/package-cache", root);
    assert(CreateDirectoryA(directory, NULL));
    snprintf(directory, sizeof(directory), "%s/package-cache/resources", root);
    assert(CreateDirectoryA(directory, NULL));
    sh_package_link_history *history = sh_package_link_history_open(directory); assert(history);
    adapter.history = history;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &first, error, sizeof(error)));
    assert(sh_package_link_history_count(history) == 4);
    native.resident = &first;
    /* An occupied name without exact retained evidence still branches safely. */
    adapter.history = NULL;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    for (size_t i = 0; i < f.count; i++) assert(strcmp(first.names[first.classes.nodes[i]], next.names[next.classes.nodes[i]]));
    adapter.history = history;
    for (int pass = 0; pass < 12; pass++) {
        assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
        assert(sh_package_link_history_count(history) == 4 && next.compiled->resource_count == 4);
        for (size_t i = 0; i < f.count; i++) assert(!strcmp(first.names[first.classes.nodes[i]], next.names[next.classes.nodes[i]]));
    }
    /* A changed dependency must branch both itself and its consumer. Failed
     * replay leaves neither its proof nor its private identity reserved. */
    f.units[materials[0]].semantics[0] ^= 1; f.omit_replay = 1; f.assigned = 0;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(sh_package_link_history_count(history) == 4);
    f.omit_replay = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(sh_package_link_history_count(history) == 6);
    assert(strcmp(first.names[first.classes.nodes[materials[0]]], next.names[next.classes.nodes[materials[0]]]));
    assert(strcmp(first.names[first.classes.nodes[entities[0]]], next.names[next.classes.nodes[entities[0]]]));
    assert(!strcmp(first.names[first.classes.nodes[entities[2]]], next.names[next.classes.nodes[entities[2]]]));
    f.units[materials[0]].semantics[0] ^= 1;
    native.unavailable = 1;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(sh_package_link_history_count(history) == 6);
    native.unavailable = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    for (size_t i = 0; i < f.count; i++) assert(!strcmp(first.names[first.classes.nodes[i]], next.names[next.classes.nodes[i]]));
    /* Native numeric collisions cannot be waived by a matching proof. */
    native.all_occupied = 1;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(sh_package_link_history_count(history) == 6);
    sh_package_link_output_free(&first); sh_package_link_output_free(&next);
    sh_package_link_history_close(history);

    /* Reservations cover unmaterialized identities too, across compilations. */
    history = sh_package_link_history_open(directory); assert(history); adapter.history = history;
    memset(&native, 0, sizeof(native)); f = *source; f.count = 1;
    f.units[0] = source->units[materials[0]];
    adapter.history = NULL;
    assert(sh_package_link(sources, f.units, 1, &adapter, NULL, NULL, &first, error, sizeof(error)));
    char *one = _strdup(first.names[0]); assert(one);
    f.units[0] = source->units[materials[1]];
    assert(sh_package_link(sources, f.units, 1, &adapter, NULL, NULL, &next, error, sizeof(error)));
    char *two = _strdup(next.names[0]); assert(two);
    native.colliding[0] = one; native.colliding[1] = two; adapter.history = history;
    f.units[0] = source->units[materials[0]];
    assert(sh_package_link(sources, f.units, 1, &adapter, NULL, NULL, &first, error, sizeof(error)));
    assert(!strcmp(first.names[0], one));
    f.units[0] = source->units[materials[1]];
    assert(sh_package_link(sources, f.units, 1, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(strcmp(next.names[0], two) && sh_package_link_history_count(history) == 2);
    char *salted = _strdup(next.names[0]); assert(salted);
    assert(sh_package_link(sources, f.units, 1, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(!strcmp(next.names[0], salted) && sh_package_link_history_count(history) == 2);
    free(one); free(two); free(salted);
    sh_package_link_output_free(&first); sh_package_link_output_free(&next); sh_package_link_history_close(history);

    /* Cyclic source graphs retain the same complete binding on a second load. */
    history = sh_package_link_history_open(directory); assert(history); adapter.history = history;
    memset(&native, 0, sizeof(native)); f = *source; f.count = 2;
    f.units[0] = source->units[entities[0]]; f.units[1] = source->units[materials[0]];
    sh_package_link_edge back = {"native-owner", 0};
    f.units[1].implicit = &back; f.units[1].implicit_count = 1;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &first, error, sizeof(error)));
    native.resident = &first;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &next, error, sizeof(error)));
    for (size_t i = 0; i < 2; i++) assert(!strcmp(first.names[first.classes.nodes[i]], next.names[next.classes.nodes[i]]));
    assert(sh_package_link_history_count(history) == 2);
    sh_package_link_output_free(&first); sh_package_link_output_free(&next); sh_package_link_history_close(history);

    /* Equal bytes aimed at a shared editor slot and at a map-private copy
     * have different binding contracts, including for their consumers. */
    history = sh_package_link_history_open(directory); assert(history); adapter.history = history;
    memset(&native, 0, sizeof(native)); f = *source; f.count = 2;
    f.units[0] = source->units[entities[0]]; f.units[1] = source->units[materials[0]];
    f.units[1].fixed_name = 1; native.shared_original = f.units[1].name;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &first, error, sizeof(error)));
    assert(!strcmp(first.names[first.classes.nodes[1]], f.units[1].name));
    native.resident = &first; f.units[1].fixed_name = 0;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &next, error, sizeof(error)));
    assert(strcmp(first.names[first.classes.nodes[0]], next.names[next.classes.nodes[0]]));
    assert(strcmp(first.names[first.classes.nodes[1]], next.names[next.classes.nodes[1]]));
    assert(sh_package_link_history_count(history) == 4);
    sh_package_link_output_free(&first); sh_package_link_output_free(&next); sh_package_link_history_close(history);
}

static void shared_replacement(const sh_package_sources *sources, const fixture *source,
    const size_t materials[3])
{
    fixture f = {0};
    native_fixture native = {0}; native.replace_shared = 1;
    sh_package_link_registry registry = {&native, native_check};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign, &registry};
    sh_package_link_output out = {0};
    char error[512];
    f.units[0] = source->units[materials[0]]; f.count = 1; f.shared_name = 1;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!strcmp(out.names[0], f.units[0].name));
    f.shared_name = 0;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "same-name binding") && !out.compiled);
    f.shared_name = 1; f.units[0].original_binding = 1;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "same-name binding") && !out.compiled);
    f.units[0].original_binding = 0; f.units[1] = source->units[materials[1]]; f.count = 2;
    /* Replacement authorization for one shared slot cannot equate two distinct
     * new bindings at that slot or waive the candidate registry's checks. */
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled && !out.names);
    f.units[1].name = "materials/another"; native.constant = 1;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled && !out.names);
    sh_package_link_output_free(&out);
}

static void streamed_suppliers(const sh_package_sources *sources)
{
    sh_compiled_resource resources[2] = {0};
    const sh_compiled_resource *files[2] = {resources, resources + 1};
    sh_package_compilation compilation = {0}; compilation.sources = sources;
    fixture f = {0};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0};
    char error[512];
    size_t found = 0, length;
    unsigned char *body;
    for (size_t i = 0; i < sources->file_count && found < 2; i++)
        if (!strcmp(sources->files[i].relative, "equal.bin")) {
            resources[found].engine_path = "shader/shared.bin"; resources[found].source = i; found++;
        }
    assert(found == 2);
    resources[1].cache_file = sh_package_file_capture(sources->files[resources[1].source].absolute, error, sizeof(error));
    assert(resources[1].cache_file);
    for (size_t i = 0; i < 2; i++) f.units[i] = (sh_package_link_unit){"renderprog", "shared", &compilation, files + i, 1};
    f.count = 2;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && out.compiled->resource_count == 1);
    body = sh_package_compilation_read(&compilation, resources, SIZE_MAX, &length, error, sizeof(error));
    assert(body && length > 32768);
    resources[0].body = body; resources[0].body_length = length;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1);
    sh_package_link_output_free(&out); free(body); sh_package_file_release(resources[1].cache_file);
}

/* One captured member shared by two units whose bound dependencies differ
 * still makes two logical variants: both rocks hold the same captured object
 * and reach exact verification, but one binds a red stone and the other a
 * green one. With equal stones they are one variant. */
static void shared_member_variants(const sh_package_sources *sources)
{
    static const char rock_text[] = "{ edit = { material = \"materials/stone\"; health = 10; } }";
    static const char *colors[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }"};
    sh_compiled_resource rock = {0}, stones[2] = {{0}};
    rock.engine_path = "generated/decls/entitydef/props/rock.decl"; rock.type = "entitydef";
    rock.name = "props/rock"; rock.source = SIZE_MAX;
    rock.body = (unsigned char *)rock_text; rock.body_length = strlen(rock_text);
    sh_package_compilation scopes[2] = {{0}};
    for (size_t s = 0; s < 2; s++) {
        stones[s].engine_path = "generated/decls/material/materials/stone.decl"; stones[s].type = "material";
        stones[s].name = "materials/stone"; stones[s].source = SIZE_MAX;
        stones[s].body = (unsigned char *)colors[s]; stones[s].body_length = strlen(colors[s]);
        scopes[s].sources = sources;
    }
    const sh_compiled_resource *files[] = {&rock, stones, stones + 1};
    fixture f = {0}; f.count = 4;
    for (size_t s = 0; s < 2; s++) {
        f.units[s * 2] = (sh_package_link_unit){"entitydef", "props/rock", scopes + s, files, 1};
        f.units[s * 2 + 1] = (sh_package_link_unit){"material", "materials/stone", scopes + s, files + 1 + s, 1};
    }
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0};
    char error[1024], counters[1024];
    for (int equal = 0; equal < 2; equal++) {
        unsigned long long pairs = 0, shared = 0;
        if (equal) { stones[1].body = (unsigned char *)colors[0]; stones[1].body_length = strlen(colors[0]); }
        adapter.reuse = sh_package_link_reuse_open(f.count); assert(adapter.reuse);
        int ok = sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "shared member variants: %s\n", error);
        const char *at = strstr(sh_package_link_reuse_counters(adapter.reuse, counters, sizeof(counters)),
            "exact verification: ");
        assert(ok && at && sscanf(at, "exact verification: %llu pairs, %llu shared members", &pairs, &shared) == 2);
        /* The rocks always reach exact verification through the shared
         * object; equal stones are compared as two objects. */
        assert(shared == 1 && pairs == (equal ? 2u : 1u));
        if (equal) assert(out.classes.nodes[0] == out.classes.nodes[2] && out.classes.nodes[1] == out.classes.nodes[3]);
        else assert(out.classes.nodes[0] != out.classes.nodes[2] && out.classes.nodes[1] != out.classes.nodes[3] &&
            strcmp(out.names[out.classes.nodes[0]], out.names[out.classes.nodes[2]]));
        sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
        sh_package_link_output_free(&out);
    }
}

/* A descriptor without its own bytes is read through each unit's compilation,
 * so in two compilations it is two suppliers: under a forced fingerprint
 * collision exact verification still compares their bytes. Within one
 * compilation it is one supplier and certifies without a second read. */
static const sh_package_compilation *claimed_supplier;
static int claimed_identity(void *context, size_t unit, const sh_compiled_resource *file,
    sh_package_file_identity *out, char *error, size_t capacity)
{
    (void)context; (void)unit;
    return sh_package_compilation_identity(claimed_supplier, file, out, error, capacity);
}
static int no_sites(void *context, size_t unit, const sh_compiled_resource *file,
    const sh_package_link_edge **sites, size_t *count)
{
    (void)context; (void)unit; (void)file; *sites = NULL; *count = 0; return 1;
}
static void foreign_suppliers(void)
{
    const char *bodies[] = {"AAAA", "BBBB"};
    char base[MAX_PATH + 32], relative[128], error[1024], counters[1024];
    sh_package_sources *sources[2];
    sh_package_compilation compilations[2] = {{0}};
    size_t index[2] = {SIZE_MAX, SIZE_MAX};
    for (size_t s = 0; s < 2; s++) {
        snprintf(relative, sizeof(relative), "suppliers/%c/overrides/p/package.json", 'a' + (int)s);
        create(relative, "{\"id\":\"p\",\"name\":\"P\"}");
        snprintf(relative, sizeof(relative), "suppliers/%c/overrides/p/x.bin", 'a' + (int)s);
        create(relative, bodies[s]);
        snprintf(base, sizeof(base), "%s/suppliers/%c", root, 'a' + (int)s);
        sources[s] = sh_package_sources_scan(base, error, sizeof(error)); assert(sources[s]);
        for (size_t i = 0; i < sources[s]->file_count; i++)
            if (!strcmp(sources[s]->files[i].relative, "x.bin")) index[s] = i;
        compilations[s].sources = sources[s];
    }
    assert(index[0] != SIZE_MAX && index[0] == index[1]);
    sh_compiled_resource resource = {0};
    resource.engine_path = "shader/x.bin"; resource.source = index[0];
    const sh_compiled_resource *files[] = {&resource};
    fixture f = {0}; f.count = 2;
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    adapter.sites = no_sites; adapter.identity = claimed_identity;
    sh_package_link_output out = {0};
    claimed_supplier = compilations;
    for (int same = 0; same < 2; same++) {
        unsigned long long pairs = 0, shared = 0;
        for (size_t i = 0; i < 2; i++)
            f.units[i] = (sh_package_link_unit){"renderprog", "shared", compilations + (same ? 0 : i), files, 1};
        adapter.reuse = sh_package_link_reuse_open(2); assert(adapter.reuse);
        int ok = sh_package_link(sources[0], f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error));
        const char *at = strstr(sh_package_link_reuse_counters(adapter.reuse, counters, sizeof(counters)),
            "exact verification: ");
        assert(at && sscanf(at, "exact verification: %llu pairs, %llu shared members", &pairs, &shared) == 2);
        if (same) assert(ok && out.classes.count == 1 && pairs >= 1 && shared >= 1);
        else assert(!ok && strstr(error, "exact supplier bytes differ") && pairs == 1 && !shared);
        sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
        sh_package_link_output_free(&out);
    }
    for (size_t s = 0; s < 2; s++) sh_package_sources_free(sources[s]);
}

/* Files with more bindings than the scan threshold count repeated fields in a
 * table. Each file numbers its own repeats exactly as the scan does, also for
 * fields spelled with their own '#', whether bound, supplied as sites or
 * replayed; a unit's second file and a second unit start their own counts. */
static void expected_slots(const char *path, const plan_ref *refs, size_t count, char (*out)[192])
{
    for (size_t k = 0; k < count; k++) {
        char key[192];
        size_t seen = 0, n;
        snprintf(key, sizeof(key), "%zu:%s%s", strlen(path), path, refs[k].slot);
        n = strlen(key);
        for (size_t i = 0; i < k; i++) if (!strncmp(out[i], key, n) && (!out[i][n] || out[i][n] == '#')) seen++;
        if (seen) snprintf(out[k], 192, "%s#%zu", key, seen + 1);
        else strcpy_s(out[k], 192, key);
    }
}
static int plan_sites(void *context, size_t unit, const sh_compiled_resource *file,
    const sh_package_link_edge **sites, size_t *count)
{
    const fixture *f = context;
    (void)unit;
    for (size_t p = 0; p < f->plan_count; p++) if (f->plans[p].file == file) {
        *sites = f->plans[p].sites; *count = f->plans[p].count; return 1;
    }
    return 0;
}
static void many_repeated_slots(const sh_package_sources *sources)
{
    enum { A = 40, B = 36 };
    static const char *const fields[] = {"edit.a", "edit.c", "edit.c#2", "edit.b", "edit.c", "edit.a#x", "edit.b#"};
    static const char *const stones[] = {"materials/m0", "materials/m1", "materials/m2"};
    static const char *const colors[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }", "{ color {0,0,1,1} }"};
    static const char *const stone_paths[] = {"generated/decls/material/materials/m0.decl",
        "generated/decls/material/materials/m1.decl", "generated/decls/material/materials/m2.decl"};
    static const char *const paths[] = {"generated/decls/entitydef/props/many_a.decl",
        "generated/decls/entitydef/props/many_b.decl"};
    static plan_ref refs[2][2][A];
    static sh_package_link_edge sites[2][2][A];
    static char expected[A + B][192];
    sh_compiled_resource entity[2][2] = {{{0}}}, stone[2][3] = {{{0}}};
    const sh_compiled_resource *entity_files[2][2], *stone_files[2][3];
    sh_package_compilation scopes[2] = {{0}};
    slot_plan plans[4];
    fixture f = {0};
    char error[1024], *bound_names[8] = {0};
    const char *got[A + B + 8];
    for (size_t s = 0; s < 2; s++) {
        scopes[s].sources = sources;
        for (size_t j = 0; j < 3; j++) {
            stone[s][j].engine_path = stone_paths[j]; stone[s][j].type = "material"; stone[s][j].name = stones[j];
            stone[s][j].source = SIZE_MAX;
            stone[s][j].body = (unsigned char *)colors[j]; stone[s][j].body_length = strlen(colors[j]);
            stone_files[s][j] = &stone[s][j];
            f.units[s * 4 + 1 + j] = (sh_package_link_unit){"material", stones[j], scopes + s, stone_files[s] + j, 1};
        }
        for (size_t file = 0; file < 2; file++) {
            entity[s][file].engine_path = paths[file]; entity[s][file].source = SIZE_MAX;
            entity[s][file].body = (unsigned char *)"{ }"; entity[s][file].body_length = 3;
            entity_files[s][file] = &entity[s][file];
            for (size_t k = 0; k < (file ? B : A); k++) {
                refs[s][file][k] = (plan_ref){fields[(k + file * 3) % 7], stones[(k + file) % 3]};
                sites[s][file][k] = (sh_package_link_edge){fields[(k + file * 3) % 7], s * 4 + 1 + (k + file) % 3};
            }
            plans[s * 2 + file] = (slot_plan){&entity[s][file], refs[s][file], sites[s][file], file ? B : A};
        }
        f.units[s * 4] = (sh_package_link_unit){"entitydef", "props/many", scopes + s, entity_files[s], 2};
    }
    f.count = 8; f.plans = plans; f.plan_count = 4;
    expected_slots(paths[0], refs[0][0], A, expected);
    expected_slots(paths[1], refs[0][1], B, expected + A);
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0};
    /* Bound, then supplied as sites; both replayed by the final link. */
    for (int sited = 0; sited < 3; sited++) {
        if (sited == 2) {
            /* A late repeat in the second unit's first file binds another stone. */
            refs[1][0][A - 3].target = stones[(A - 3 + 1) % 3];
            sites[1][0][A - 3].target = 4 + 1 + (A - 3 + 1) % 3;
        }
        adapter.sites = sited == 1 ? plan_sites : NULL;
        adapter.reuse = sh_package_link_reuse_open(f.count); assert(adapter.reuse);
        int ok = sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "many repeated slots: %s\n", error);
        assert(ok);
        for (size_t s = 0; s < 2; s++) {
            size_t n = sh_package_link_reuse_test_slots(adapter.reuse, s * 4, got, sizeof(got) / sizeof(*got));
            assert(n == A + B);
            for (size_t i = 0; i < n; i++) {
                if (strcmp(got[i], expected[i])) fprintf(stderr, "slot %zu of unit %zu: %s, expected %s\n", i, s * 4, got[i], expected[i]);
                assert(!strcmp(got[i], expected[i]));
            }
        }
        if (sited < 2) assert(out.classes.nodes[0] == out.classes.nodes[4]);
        else assert(out.classes.nodes[0] != out.classes.nodes[4] &&
            strcmp(out.names[out.classes.nodes[0]], out.names[out.classes.nodes[4]]));
        for (size_t u = 0; u < f.count; u++) {
            if (!sited) { bound_names[u] = _strdup(out.names[out.classes.nodes[u]]); assert(bound_names[u]); }
            else if (sited == 1) assert(!strcmp(bound_names[u], out.names[out.classes.nodes[u]]));
        }
        sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
        sh_package_link_output_free(&out);
    }
    for (size_t u = 0; u < 8; u++) free(bound_names[u]);
}

/* One file of exactly 33 bindings, in the only unit with that many: capture
 * leaves the repeat table at its 33rd binding, the unit then reads the reuse
 * cache's shared slot texts, and the final link's replay reaches that binding
 * again with no other table use in between. Slots, names and a different last
 * target hold without a reuse cache, with one, and with caller-supplied sites. */
static void occurrence_table_replay(const sh_package_sources *sources)
{
    enum { R = 33 };
    static const char *const fields[] = {"edit.a", "edit.c", "edit.c#2", "edit.b", "edit.c", "edit.a#x", "edit.b#"};
    static const char *const stones[] = {"materials/m0", "materials/m1", "materials/m2"};
    static const char *const colors[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }", "{ color {0,0,1,1} }"};
    static const char *const stone_paths[] = {"generated/decls/material/materials/m0.decl",
        "generated/decls/material/materials/m1.decl", "generated/decls/material/materials/m2.decl"};
    static const char path[] = "generated/decls/entitydef/props/single.decl";
    static plan_ref refs[R];
    static sh_package_link_edge sites[R];
    static char expected[R][192];
    sh_compiled_resource entity = {0}, stone[3] = {{0}};
    const sh_compiled_resource *entity_files[1] = {&entity}, *stone_files[3];
    sh_package_compilation scope = {0};
    slot_plan plan;
    fixture f = {0};
    char error[1024], *names[3] = {0};
    const char *got[R + 8];
    scope.sources = sources;
    for (size_t j = 0; j < 3; j++) {
        stone[j].engine_path = stone_paths[j]; stone[j].type = "material"; stone[j].name = stones[j];
        stone[j].source = SIZE_MAX; stone[j].body = (unsigned char *)colors[j]; stone[j].body_length = strlen(colors[j]);
        stone_files[j] = stone + j;
        f.units[1 + j] = (sh_package_link_unit){"material", stones[j], &scope, stone_files + j, 1};
    }
    entity.engine_path = path; entity.source = SIZE_MAX; entity.body = (unsigned char *)"{ }"; entity.body_length = 3;
    f.units[0] = (sh_package_link_unit){"entitydef", "props/single", &scope, entity_files, 1};
    for (size_t k = 0; k < R; k++) {
        refs[k] = (plan_ref){fields[k % 7], stones[k % 3]};
        sites[k] = (sh_package_link_edge){fields[k % 7], 1 + k % 3};
    }
    plan = (slot_plan){&entity, refs, sites, R};
    f.count = 4; f.plans = &plan; f.plan_count = 1;
    expected_slots(path, refs, R, expected);
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0};
    /* Without a cache; bound into one; supplied as sites into one; then the
     * last binding names another stone. */
    for (int run = 0; run < 4; run++) {
        if (run == 3) { refs[R - 1].target = stones[R % 3]; sites[R - 1].target = 1 + R % 3; }
        adapter.sites = run == 2 ? plan_sites : NULL;
        adapter.reuse = run ? sh_package_link_reuse_open(f.count) : NULL;
        assert(!run || adapter.reuse);
        int ok = sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "occurrence table replay: %s\n", error);
        assert(ok);
        if (adapter.reuse) {
            size_t n = sh_package_link_reuse_test_slots(adapter.reuse, 0, got, sizeof(got) / sizeof(*got));
            assert(n == R);
            for (size_t i = 0; i < n; i++) assert(!strcmp(got[i], expected[i]));
        }
        const char *name = out.names[out.classes.nodes[0]];
        if (run < 3) {
            names[run] = _strdup(name); assert(names[run]);
            assert(!run || !strcmp(names[run], names[0]));
        } else assert(strcmp(name, names[0]));
        sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
        sh_package_link_output_free(&out);
    }
    for (size_t i = 0; i < 3; i++) free(names[i]);
}

/* Names after a repaired order: with a reuse cache, renaming one of many units
 * between links moves it from the front to the back of the remembered shape
 * order, which is repaired rather than sorted again; every unit keeps the
 * class and name a fresh link gives. */
static void repaired_order_names(const sh_package_sources *sources)
{
    enum { S = 3, M = 8 };
    static char names[S * M][32], paths[S * M][80], bodies[S * M][40];
    sh_compiled_resource parts[S * M] = {{0}};
    const sh_compiled_resource *files[S * M];
    sh_package_compilation scopes[S] = {{0}};
    fixture f = {0};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output reused = {0}, fresh = {0};
    char error[1024];
    for (size_t s = 0; s < S; s++) {
        scopes[s].sources = sources;
        for (size_t i = 0; i < M; i++) {
            size_t u = s * M + i;
            snprintf(names[u], sizeof(names[u]), "materials/r%zu", i);
            snprintf(paths[u], sizeof(paths[u]), "generated/decls/material/%s.decl", names[u]);
            snprintf(bodies[u], sizeof(bodies[u]), "{ color {%zu,0,0,1} }", i);
            parts[u].engine_path = paths[u]; parts[u].type = "material"; parts[u].name = names[u];
            parts[u].source = SIZE_MAX; parts[u].body = (unsigned char *)bodies[u]; parts[u].body_length = strlen(bodies[u]);
            files[u] = parts + u;
            f.units[u] = (sh_package_link_unit){"material", names[u], scopes + s, files + u, 1};
        }
    }
    f.count = S * M;
    adapter.reuse = sh_package_link_reuse_open(f.count); assert(adapter.reuse);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &reused, error, sizeof(error)));
    /* Scope 2's r0 sorts first among the materials; renamed, it sorts last. */
    size_t moved = 2 * M;
    snprintf(names[moved], sizeof(names[moved]), "materials/zz");
    snprintf(paths[moved], sizeof(paths[moved]), "generated/decls/material/materials/zz.decl");
    sh_package_link_reuse_invalidate(adapter.reuse, moved);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &reused, error, sizeof(error)));
    sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &fresh, error, sizeof(error)));
    assert(reused.classes.count == fresh.classes.count);
    for (size_t u = 0; u < f.count; u++) {
        for (size_t v = 0; v < f.count; v++)
            assert((reused.classes.nodes[u] == reused.classes.nodes[v]) == (fresh.classes.nodes[u] == fresh.classes.nodes[v]));
        assert(!strcmp(reused.names[reused.classes.nodes[u]], fresh.names[fresh.classes.nodes[u]]));
    }
    assert(reused.classes.nodes[0] == reused.classes.nodes[M] && reused.classes.nodes[M] != reused.classes.nodes[moved]);
    sh_package_link_output_free(&reused); sh_package_link_output_free(&fresh);
}

static void original_aliases(const sh_package_sources *sources)
{
    sh_compiled_resource resources[2] = {0};
    const sh_compiled_resource *files[] = {resources, resources + 1};
    sh_package_compilation compilation = {0}; compilation.sources = sources;
    fixture f = {0}; f.original_paths = 1; f.count = 2;
    native_fixture native = {0}; sh_package_link_registry registry = {&native, native_check};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign, &registry};
    sh_package_link_output out = {0}; char error[512];
    for (size_t i = 0; i < 2; i++) {
        resources[i].engine_path = "generated/image/shared.bimage";
        resources[i].body = (unsigned char *)"same"; resources[i].body_length = 4; resources[i].source = SIZE_MAX;
        f.units[i] = (sh_package_link_unit){"image", i ? "two" : "one", &compilation, files + i, 1, {0}, 1};
    }
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 2 && out.compiled->resource_count == 1 && native.checks == 2);
    assert(sh_package_resource_native_matches(out.compiled->resources, "image", "one"));
    assert(sh_package_resource_native_matches(out.compiled->resources, "image", "two"));
    assert(sh_package_resource_native_count(out.compiled->resources) == 2);
    resources[1].body = (unsigned char *)"diff";
    assert(!sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "original resource") && !out.compiled);
    resources[1].body = (unsigned char *)"same"; f.assigned = 0; f.alias_rewrite = 1;
    assert(!sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "emit different bytes") && !out.compiled);
    f.alias_rewrite = 0; native.constant = 1;
    assert(!sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "original resource") && !out.compiled);
    sh_package_link_output_free(&out);
}

static void implicit_ownership(const sh_package_sources *sources)
{
    sh_compiled_resource resource = {0}; const sh_compiled_resource *file = &resource;
    sh_package_compilation scopes[2] = {{0}, {0}};
    resource.engine_path = "generated/decls/material/owned.decl";
    resource.type = "material"; resource.name = "owned"; resource.source = SIZE_MAX;
    resource.body = (unsigned char *)"{}"; resource.body_length = 2;
    sh_package_owners a = {0}, b = {0}, invalid = {0};
    assert(sources->package_count >= 2 && sh_package_owners_add(&a, 0) && sh_package_owners_add(&b, 1));
    fixture f = {0}; f.count = 2;
    for (size_t i = 0; i < 2; i++) {
        scopes[i].sources = sources;
        f.units[i] = (sh_package_link_unit){"material", "owned", scopes + i, &file, 1};
    }
    f.units[0].implicit_owners = &a;
    f.units[1].implicit_owners = f.units[1].implicit_gameplay_owners = &b;
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0}; char error[1024];
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && out.compiled->resource_count == 1);
    const sh_compiled_resource *linked = out.compiled->resources;
    assert(sh_package_owners_count(&linked->owners) == 2 && sh_package_owners_contains(&linked->gameplay_owners, 1));
    assert(sh_package_owners_count(&linked->gameplay_owners) == 1 && !linked->source_count);
    char *name = _strdup(out.names[0]); assert(name);
    /* Borrowed input sets can disappear without invalidating emitted ownership. */
    sh_package_owners_free(&a); sh_package_owners_free(&b);
    assert(sh_package_owners_count(&linked->owners) == 2);
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && !strcmp(name, out.names[0]) && !sh_package_owners_count(&out.compiled->resources[0].owners));
    free(name);
    assert(sh_package_owners_add(&invalid, sources->package_count)); f.units[0].implicit_owners = &invalid;
    assert(!sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)) &&
        strstr(error, "outside the captured inventory") && !out.compiled);
    sh_package_owners_free(&invalid); sh_package_link_output_free(&out);
}

static void exact_witnesses(const sh_package_sources *sources)
{
    fixture f = {0}; sh_package_link_output out = {0}; char error[512];
    sh_package_compilation scope = {0}; scope.sources = sources;
    unsigned char a[32771] = {0}, b[32771] = {0};
    sh_compiled_resource physical = {0}, pages[4] = {0};
    physical.engine_path = "generated/model/main.bmodel";
    physical.body = (unsigned char *)"main"; physical.body_length = 4; physical.source = SIZE_MAX;
    for (size_t i = 0; i < 4; i++) {
        pages[i].engine_path = i % 2 ? "virtualtextures/a_sq2.mega2" : "virtualtextures/a_sq1.mega2";
        pages[i].body = i < 2 ? a : b; pages[i].body_length = sizeof(a); pages[i].source = SIZE_MAX;
    }
    sh_package_link_witness witnesses[] = {{"sq1", pages}, {"sq2", pages + 1},
        {"sq2", pages + 3}, {"sq1", pages + 2}};
    const sh_compiled_resource *file = &physical;
    f.units[0] = (sh_package_link_unit){"model", "main", &scope, &file, 1};
    f.units[1] = f.units[0]; f.count = 2;
    f.units[0].witnesses = witnesses; f.units[0].witness_count = 2;
    f.units[1].witnesses = witnesses + 2; f.units[1].witness_count = 2;
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && out.compiled->resource_count == 1 && f.bound == 4);
    assert(!strstr(out.compiled->resources[0].engine_path, "virtualtextures"));
    unsigned char key[32]; memcpy(key, out.classes.identities[0], 32);
    sh_package_link_witness swap = witnesses[0]; witnesses[0] = witnesses[1]; witnesses[1] = swap;
    f.assigned = f.bound = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && !memcmp(key, out.classes.identities[0], 32));
    b[sizeof(b) - 1] = 1; f.assigned = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 2 && out.compiled->resource_count == 2);
    b[sizeof(b) - 1] = 0; f.assigned = 0; f.mutate_witness = a;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "witness bytes changed") && !out.compiled);
    *a = 0; f.mutate_witness = NULL; witnesses[1].role = witnesses[0].role;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "repeats a native role")); sh_package_link_output_free(&out);
}
static int assign_native_pair(void *context, size_t unit, const unsigned char token[32], int original,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths, char *error, size_t capacity)
{
    fixture *f = context;
    sh_package_link_inputs inputs = {f->units, NULL, f->count};
    return sh_package_native_names_assign(&inputs, unit, token, original, files, count, name, paths, error, capacity);
}
static int assign_native_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    fixture *f = context;
    sh_package_link_inputs inputs = {f->units, NULL, f->count};
    return sh_package_native_names_assign_members(&inputs, unit, name, files, paths, count, names, error, capacity);
}

static void coupled_native_pairs(const sh_package_sources *sources, fixture *f)
{
    native_fixture native = {0};
    sh_package_link_registry registry = {&native, native_check};
    sh_package_link_adapter adapter = {f, resolve, bind_file, assign_native_pair, &registry, assign_native_members};
    sh_package_link_output out = {0};
    char error[512], *first = NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        assert(sh_package_link(sources, f->units, f->count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        assert(out.classes.count == 2 && out.compiled->resource_count == 4);
        assert(out.classes.nodes[0] == out.classes.nodes[2]);
        if (attempt) assert(strcmp(first, out.names[out.classes.nodes[0]]) && native.collisions == 1);
        else { first = _strdup(out.names[out.classes.nodes[0]]); assert(first); }
        for (size_t i = 0; i < out.compiled->resource_count; i++) {
            const sh_compiled_resource *movie = out.compiled->resources + i;
            if (strcmp(movie->native_type, "file")) continue;
            assert(movie->bound_dependency_count == 1 && !strcmp(movie->native_name, movie->engine_path));
            const sh_compiled_resource *atlas = sh_package_compilation_find(out.compiled, movie->bound_dependencies[0]);
            assert(atlas && !strcmp(atlas->native_type, "image") && !strcmp(atlas->native_name, atlas->engine_path));
            char source[256]; sh_swf_binary_paths pair;
            assert(strlen(movie->native_name) < sizeof(source) && !strncmp(movie->native_name, "generated/swf/smp/", 18));
            snprintf(source, sizeof(source), "%s", movie->native_name + 10);
            memcpy(source + strlen(source) - 5, ".swf", 5);
            assert(sh_swf_binary_paths_for(source, &pair) && !strcmp(pair.movie, movie->engine_path) &&
                !strcmp(pair.atlas, atlas->engine_path));
            if (!attempt && !strcmp(movie->native_name, first)) native.occupied[0] = test_native_id(atlas->native_name);
        }
    }
    free(first); sh_package_link_output_free(&out);
}

static void coupled_members(const sh_package_sources *sources)
{
    sh_compiled_resource resources[7] = {0};
    const sh_compiled_resource *files[3][3] = {{0}};
    sh_package_link_file_identity identities[3][3] = {{{0}}};
    sh_package_compilation compilation = {0}; compilation.sources = sources;
    fixture f = {0}; f.count = 3;
    native_fixture native = {0};
    sh_package_link_registry registry = {&native, native_check};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign, &registry, assign_members};
    sh_package_link_output out = {0};
    char error[512];
    for (size_t i = 0; i < 6; i++) {
        resources[i].engine_path = i % 2 ? "generated/swf/panel.bimage" : "generated/swf/panel.bswf";
        resources[i].body = (unsigned char *)(i % 2 ? i == 3 ? "green" : "red" : "movie");
        resources[i].body_length = strlen((char *)resources[i].body); resources[i].source = SIZE_MAX;
        size_t group = i / 2, file = i % 2;
        if (group == 2) file = 1 - file; /* Enumeration order has no meaning. */
        files[group][file] = resources + i;
        identities[group][file] = i % 2 ? (sh_package_link_file_identity){"image", "generated/swf/panel.bimage"} :
            (sh_package_link_file_identity){0};
    }
    for (size_t i = 0; i < 3; i++) {
        f.units[i] = (sh_package_link_unit){"file", "generated/swf/panel.bswf", &compilation, files[i], 2};
        f.units[i].file_identities = identities[i];
    }
    coupled_native_pairs(sources, &f);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 2 && out.compiled->resource_count == 4 && out.compiled->duplicate_count == 2);
    assert(out.classes.nodes[0] == out.classes.nodes[2] && out.classes.nodes[0] != out.classes.nodes[1]);
    char *primary = _strdup(out.names[out.classes.nodes[0]]), *atlas = NULL;
    assert(primary);
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *resource = out.compiled->resources + i;
        assert(resource->bound_dependency_count == 1 && sh_package_resource_native_count(resource) == 1);
        const sh_compiled_resource *paired = sh_package_compilation_find(out.compiled, resource->bound_dependencies[0]);
        assert(paired && !strcmp(paired->bound_dependencies[0], resource->engine_path));
        assert(strcmp(paired->native_type, resource->native_type));
        if (!strcmp(resource->native_type, "file")) {
            assert(strstr(resource->engine_path, ".bswf"));
            if (!strcmp(resource->native_name, primary)) atlas = _strdup(paired->native_name);
        } else assert(!strcmp(resource->native_type, "image") && strstr(resource->engine_path, ".bimage"));
    }
    assert(atlas && native.checks == 4);
    /* A baseline/still-live collision on the companion must move both names,
     * without reserving the failed primary name or leaking a partial group. */
    native.occupied[0] = test_native_id(atlas); native.checks = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(native.collisions == 1 && native.checks == 6 && strcmp(primary, out.names[out.classes.nodes[0]]));
    for (size_t i = 0; i < out.compiled->resource_count; i++) assert(strcmp(atlas, out.compiled->resources[i].native_name));
    char *moved = _strdup(out.names[out.classes.nodes[0]]); assert(moved);
    sh_package_link_unit swap = f.units[0]; f.units[0] = f.units[2]; f.units[2] = swap;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!strcmp(moved, out.names[out.classes.nodes[0]]));
    swap = f.units[0]; f.units[0] = f.units[2]; f.units[2] = swap;
    free(moved);
    /* Candidate registry collisions also include companions, not just primaries. */
    memset(&native, 0, sizeof(native)); f.member_fault = 3;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled && !out.names);
    f.member_fault = 0; memset(&native, 0, sizeof(native)); native.colliding[0] = atlas;
    /* Capture the other variant's atlas, then force the two distinct names to
     * have the same injected engine ID. One complete pair must relocate. */
    adapter.registry = NULL;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    char *other_atlas = NULL;
    for (size_t i = 0; i < out.compiled->resource_count; i++) if (!strcmp(out.compiled->resources[i].native_type, "image") &&
        strcmp(atlas, out.compiled->resources[i].native_name)) other_atlas = _strdup(out.compiled->resources[i].native_name);
    assert(other_atlas); native.colliding[1] = other_atlas; adapter.registry = &registry;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(native.checks == 6);
    size_t retained = 0;
    for (size_t i = 0; i < out.compiled->resource_count; i++) if (!strcmp(out.compiled->resources[i].native_type, "image"))
        retained += !strcmp(atlas, out.compiled->resources[i].native_name) || !strcmp(other_atlas, out.compiled->resources[i].native_name);
    assert(retained == 1);
    free(other_atlas); free(atlas); free(primary); memset(&native, 0, sizeof(native));
    for (int fault = 1; fault <= 4; fault++) {
        f.member_fault = fault;
        assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        assert(*error && !out.compiled && !out.names && !out.classes.count);
    }
    f.member_fault = 0; adapter.assign_members = NULL;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "require native name assignment")); adapter.assign_members = assign_members;
    /* A changed member identity alone changes equivalence, even when all
     * physical paths and bytes match. Explicit primary metadata is a no-op. */
    sh_package_link_classes classes = {0}; f.units[1] = f.units[2]; f.count = 2;
    assert(sh_package_link_identify_units(f.units, f.count, &adapter, &classes, error, sizeof(error)) && classes.count == 1);
    identities[2][1] = (sh_package_link_file_identity){"file", "generated/swf/panel.bswf"};
    assert(sh_package_link_identify_units(f.units, f.count, &adapter, &classes, error, sizeof(error)) && classes.count == 1);
    identities[2][0].name = "generated/swf/other.bimage";
    assert(sh_package_link_identify_units(f.units, f.count, &adapter, &classes, error, sizeof(error)) && classes.count == 2);
    identities[2][0].name = NULL;
    assert(!sh_package_link_identify_units(f.units, f.count, &adapter, &classes, error, sizeof(error)));
    assert(strstr(error, "incomplete native identity"));
    identities[2][0].name = "generated/swf/panel.bimage";
    identities[2][1] = identities[2][0];
    assert(!sh_package_link_identify_units(f.units, f.count, &adapter, &classes, error, sizeof(error)));
    assert(strstr(error, "no primary native member"));
    /* Native IDs live within a family; equal numeric IDs across the movie
     * and image managers are valid. Collisions within one group are not. */
    f.count = 1; native.constant = 1;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    resources[6] = resources[1]; resources[6].engine_path = "generated/swf/other.bimage";
    files[0][2] = resources + 6; identities[0][2] = (sh_package_link_file_identity){"image", "generated/swf/other.bimage"};
    f.units[0].file_count = 3;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled);
    f.units[0].file_count = 2; memset(&native, 0, sizeof(native));
    /* Baseline movie groups and a direct baseline image root can share the
     * same original atlas, after exact bytes and graph certification. */
    f.original_paths = 1; f.units[0].original_binding = 1;
    f.units[1] = (sh_package_link_unit){"image", "generated/swf/panel.bimage", &compilation, files[0] + 1, 1};
    f.units[1].original_binding = 1; f.count = 2;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.compiled->resource_count == 2 && out.classes.count == 2);
    f.units[1].original_binding = 0;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "exhausted") && !out.compiled);
    sh_package_link_classes_free(&classes); sh_package_link_output_free(&out);
}

/* Conditional bindings: a behavior's layer configuration selects state-machine
 * declarations (fsm_web.h). Two scopes, each with a behavior b, state machines
 * f and g it selects, and a dependency d that f reaches. */
typedef struct conditional_case {
    fixture f; sh_package_compilation scopes[2];
    sh_compiled_resource parts[8]; const sh_compiled_resource *files[8];
    sh_package_link_edge selects[2][2], uses[2][1];
} conditional_case;
static void conditional_setup(conditional_case *c, const sh_package_sources *sources)
{
    static const char *const families[] = {"aibehavior", "aifsmmanager", "aifsmmanager", "dep"};
    static const char *const names[] = {"b", "f", "g", "d"};
    static const char *const bodies[] = {"behavior", "fsm f", "fsm g", "dependency"};
    memset(c, 0, sizeof(*c));
    for (size_t s = 0; s < 2; s++) {
        c->scopes[s].sources = sources;
        for (size_t k = 0; k < 4; k++) {
            size_t i = s * 4 + k;
            c->parts[i].engine_path = k == 0 ? "generated/decls/aibehavior/b.decl" : k == 1 ?
                "generated/decls/aifsmmanager/f.decl" : k == 2 ? "generated/decls/aifsmmanager/g.decl" : "dep/d.bin";
            c->parts[i].body = (unsigned char *)bodies[k]; c->parts[i].body_length = strlen(bodies[k]);
            c->parts[i].source = SIZE_MAX; c->files[i] = c->parts + i;
            c->f.units[i] = (sh_package_link_unit){families[k], names[k], c->scopes + s, c->files + i, 1};
            c->f.units[i].original_binding = 1;
        }
        c->selects[s][0] = (sh_package_link_edge){"fsm/f", s * 4 + 1};
        c->selects[s][1] = (sh_package_link_edge){"fsm/g", s * 4 + 2};
        c->uses[s][0] = (sh_package_link_edge){"uses", s * 4 + 3};
        c->f.units[s * 4].conditional = c->selects[s]; c->f.units[s * 4].conditional_count = 2;
        c->f.units[s * 4 + 1].implicit = c->uses[s]; c->f.units[s * 4 + 1].implicit_count = 1;
    }
    c->f.count = 8; c->f.native_originals = 1;
}
static void conditional_change(conditional_case *c, size_t unit, const char *body)
{
    c->parts[unit].body = (unsigned char *)body; c->parts[unit].body_length = strlen(body);
    c->f.units[unit].original_binding = 0;
}
static size_t conditional_class(const sh_package_link_output *out, size_t unit) { return out->classes.nodes[unit]; }
static const char *conditional_name(const sh_package_link_output *out, size_t unit)
{ return out->names[out->classes.nodes[unit]]; }
static int conditional_binds(const sh_package_link_output *out, size_t owner, size_t target)
{
    int found = 0;
    for (size_t i = 0; i < out->implicit_count; i++)
        found += out->implicit[i].owner == out->classes.nodes[owner] && out->implicit[i].target == out->classes.nodes[target];
    return found;
}
static int conditional_depends(const sh_package_link_output *out, const char *owner, const char *target)
{
    for (size_t i = 0; i < out->compiled->resource_count; i++) {
        const sh_compiled_resource *r = out->compiled->resources + i;
        if (!r->native_name || strcmp(r->native_name, owner)) continue;
        for (size_t j = 0; j < r->bound_dependency_count; j++) {
            const sh_compiled_resource *t = sh_package_compilation_find(out->compiled, r->bound_dependencies[j]);
            if (t && t->native_name && !strcmp(t->native_name, target)) return 1;
        }
    }
    return 0;
}
static void conditional_bindings(const sh_package_sources *sources)
{
    conditional_case c;
    sh_package_link_output out = {0}, plain = {0};
    sh_package_link_adapter adapter;
    unsigned char unchanged[32];
    char error[512];
    char *name_a = NULL;
    /* Unchanged targets: the edges vanish. Identities and names are exactly
     * those of the same graph without them, and both scopes dedupe natively. */
    conditional_setup(&c, sources);
    adapter = (sh_package_link_adapter){&c.f, resolve, bind_file, assign};
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) == conditional_class(&out, 4) && !strcmp(conditional_name(&out, 0), "b"));
    assert(c.f.original_bindings[out.classes.representatives[conditional_class(&out, 0)]] == 1);
    assert(!conditional_binds(&out, 0, 1) && !conditional_binds(&out, 0, 2) && out.implicit_count == 1);
    memcpy(unchanged, out.classes.identities[conditional_class(&out, 0)], 32);
    c.f.units[0].conditional_count = c.f.units[4].conditional_count = 0;
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &plain, error, sizeof(error)));
    assert(!memcmp(unchanged, plain.classes.identities[conditional_class(&plain, 0)], 32));
    sh_package_link_output_free(&plain);
    /* A direct edit in scope 1 splits only its behavior and binds only the
     * changed declaration; scope 0 keeps the native identity and name. */
    conditional_setup(&c, sources);
    adapter.context = &c.f;
    conditional_change(&c, 5, "fsm f, changed");
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) != conditional_class(&out, 4));
    assert(!strcmp(conditional_name(&out, 0), "b") && strcmp(conditional_name(&out, 4), "b"));
    assert(!memcmp(unchanged, out.classes.identities[conditional_class(&out, 0)], 32));
    assert(!c.f.original_bindings[out.classes.representatives[conditional_class(&out, 4)]]);
    assert(conditional_binds(&out, 4, 5) == 1 && !conditional_binds(&out, 4, 6) && !conditional_binds(&out, 0, 1));
    assert(conditional_depends(&out, conditional_name(&out, 4), conditional_name(&out, 5)));
    /* Downstream only: f keeps its native bytes but reaches a changed d. */
    conditional_setup(&c, sources);
    adapter.context = &c.f;
    conditional_change(&c, 7, "dependency, changed");
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) != conditional_class(&out, 4) && strcmp(conditional_name(&out, 4), "b"));
    assert(conditional_binds(&out, 4, 5) == 1 && !conditional_binds(&out, 4, 6));
    assert(!memcmp(unchanged, out.classes.identities[conditional_class(&out, 0)], 32));
    /* Exact duplicates in both scopes dedupe to one private behavior. */
    conditional_setup(&c, sources);
    adapter.context = &c.f;
    conditional_change(&c, 1, "fsm f, changed"); conditional_change(&c, 5, "fsm f, changed");
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) == conditional_class(&out, 4) && strcmp(conditional_name(&out, 0), "b"));
    name_a = _strdup(conditional_name(&out, 0)); assert(name_a);
    /* Differing graphs stay apart, whichever scope comes first. */
    conditional_change(&c, 5, "fsm f, other");
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) != conditional_class(&out, 4));
    assert(!strcmp(conditional_name(&out, 0), name_a) && strcmp(conditional_name(&out, 4), name_a));
    assert(strcmp(conditional_name(&out, 4), "b"));
    free(name_a);
    /* The reuse cache keeps complete captures: certification is decided again
     * in each pass, so a later change still binds its dependents. */
    conditional_setup(&c, sources);
    adapter.context = &c.f;
    adapter.reuse = sh_package_link_reuse_open(c.f.count); assert(adapter.reuse);
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) == conditional_class(&out, 4) && !conditional_binds(&out, 4, 5));
    c.f.units[6].original_binding = 0;   /* g is scope 1's campaign-only or refused original now */
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) != conditional_class(&out, 4) && conditional_binds(&out, 4, 6) == 1);
    assert(!conditional_binds(&out, 4, 5));
    c.f.units[6].original_binding = 1;
    assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(conditional_class(&out, 0) == conditional_class(&out, 4) && !strcmp(conditional_name(&out, 4), "b"));
    sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
    /* Scopes holding their members as the same captured objects certify them
     * without reading those bytes twice, with the classes and names a byte
     * comparison of separate objects gives. */
    {
        size_t compared[8]; char *compared_names[8], counters[1024];
        unsigned long long pairs = 0, shared = 0;
        conditional_setup(&c, sources);
        adapter.context = &c.f;
        assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        for (size_t i = 0; i < 8; i++) {
            compared[i] = conditional_class(&out, i);
            compared_names[i] = _strdup(conditional_name(&out, i)); assert(compared_names[i]);
        }
        for (size_t i = 4; i < 8; i++) c.files[i] = c.parts + i - 4;
        adapter.reuse = sh_package_link_reuse_open(c.f.count); assert(adapter.reuse);
        assert(sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        for (size_t i = 0; i < 8; i++)
            assert(conditional_class(&out, i) == compared[i] && !strcmp(conditional_name(&out, i), compared_names[i]));
        const char *at = strstr(sh_package_link_reuse_counters(adapter.reuse, counters, sizeof(counters)),
            "exact verification: ");
        assert(at && sscanf(at, "exact verification: %llu pairs, %llu shared members", &pairs, &shared) == 2);
        assert(pairs && shared);
        sh_package_link_reuse_close(adapter.reuse); adapter.reuse = NULL;
        for (size_t i = 0; i < 8; i++) free(compared_names[i]);
    }
    /* A slot is conditional or implicit, never both. */
    conditional_setup(&c, sources);
    adapter.context = &c.f;
    sh_package_link_edge twice[] = {{"fsm/f", 1}};
    c.f.units[0].implicit = twice; c.f.units[0].implicit_count = 1;
    assert(!sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "both conditional and implicit"));
    c.f.units[0].implicit = NULL; c.f.units[0].implicit_count = 0;
    c.f.units[0].conditional = NULL;
    assert(!sh_package_link(sources, c.f.units, c.f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    sh_package_link_output_free(&out);
}

static void implicit_bindings(const sh_package_sources *sources)
{
    fixture f = {0}; sh_package_compilation scope = {0}; scope.sources = sources;
    sh_compiled_resource parts[6] = {0};
    const sh_compiled_resource *files[6];
    sh_package_link_edge edges[3][3];
    sh_package_link_output out = {0}; sh_package_link_classes identified = {0};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    char error[512], main_slot[] = "main";
    for (size_t i = 0; i < 6; i++) {
        parts[i].engine_path = i % 2 ? "image/shared.bin" : "consumer/shared.bin";
        parts[i].body = (unsigned char *)(i % 2 ? (i == 3 ? "green" : "red") : "same consumer");
        parts[i].body_length = strlen((char *)parts[i].body); parts[i].source = SIZE_MAX;
        files[i] = parts + i;
        f.units[i] = (sh_package_link_unit){i % 2 ? "image" : "consumer", "shared", &scope, files + i, 1};
    }
    f.count = 6;
    for (size_t i = 0; i < 3; i++) {
        edges[i][0] = (sh_package_link_edge){main_slot, i * 2 + 1};
        edges[i][1] = (sh_package_link_edge){"alternate", i * 2 + 1};
        edges[i][2] = edges[i][0];
        f.units[i * 2].implicit = edges[i]; f.units[i * 2].implicit_count = 3;
    }
    assert(sh_package_link_identify_units(f.units, f.count, &adapter, &identified, error, sizeof(error)));
    assert(identified.count == 4 && identified.nodes[0] == identified.nodes[4] && identified.nodes[0] != identified.nodes[2]);
    sh_package_link_classes_free(&identified);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 4 && out.compiled->resource_count == 4 && out.implicit_count == 4);
    char *stable = _strdup(out.names[out.classes.nodes[0]]); assert(stable);
    for (size_t i = 0; i < out.implicit_count; i++) {
        const sh_package_link_implicit_binding *binding = out.implicit + i;
        size_t owner = out.classes.representatives[binding->owner];
        assert(!(owner % 2));
        assert(binding->target == out.classes.nodes[owner + 1]);
        assert(!strcmp(binding->slot, "main") || !strcmp(binding->slot, "alternate"));
        assert(binding->slot != main_slot);
    }
    size_t owners = 0;
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *r = out.compiled->resources + i;
        if (strcmp(r->native_type, "consumer")) continue;
        owners++; assert(r->bound_dependency_count == 1);
        const sh_compiled_resource *target = sh_package_compilation_find(out.compiled, r->bound_dependencies[0]);
        assert(target && !strcmp(target->native_type, "image"));
        size_t length; unsigned char *body = sh_package_compilation_read(out.compiled, r, SIZE_MAX, &length, error, sizeof(error));
        assert(body && length == 13 && !memcmp(body, "same consumer", length)); free(body);
    }
    assert(owners == 2);
    sh_package_link_edge swap = edges[0][0]; edges[0][0] = edges[0][1]; edges[0][1] = swap;
    f.units[0].implicit_count = 2; /* Redundant repeated slots do not split a binding. */
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!strcmp(stable, out.names[out.classes.nodes[0]]) && out.implicit_count == 4); free(stable);
    /* Keep the mutated snapshot structurally valid so replay, rather than
     * contradictory duplicate-slot validation, detects the changed target. */
    f.units[4].implicit_count = 2;
    f.assigned = 0; f.mutate_implicit = edges[2];
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "changed during linking") && !out.compiled && !out.implicit && !out.implicit_count);
    edges[2][0].target ^= 1; f.mutate_implicit = NULL;
    f.units[4].implicit_count = 3;
    /* Slot identity is separate from its target identity. */
    edges[2][0].slot = "different"; edges[2][2].slot = "different";
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 5 && out.classes.nodes[0] != out.classes.nodes[4]);
    edges[2][0].slot = edges[2][2].slot = main_slot;
    for (size_t i = 0; i < f.count; i++) f.units[i].original_binding = 1;
    f.units[3].original_binding = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!f.original_bindings[out.classes.representatives[out.classes.nodes[2]]]);
    assert(f.original_bindings[out.classes.representatives[out.classes.nodes[0]]]);
    /* Contradictory captures and missing/out-of-range metadata do not escape. */
    sh_package_link_edge malformed[2] = {{"main", 1}, {"main", 3}};
    f.units[0].implicit = malformed; f.units[0].implicit_count = 2;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "contradictory"));
    malformed[0].slot = NULL; f.units[0].implicit_count = 1;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    malformed[0].slot = "";
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    malformed[0] = (sh_package_link_edge){"main", f.count};
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    f.units[0].implicit = NULL;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    f.units[0].implicit = malformed; f.units[0].implicit_count = SIZE_MAX;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    /* Retain supplied data through admitted native nodes, even across cycles. */
    sh_package_link_edge root_edge[] = {{"native", 2}};
    sh_package_link_edge a_edge[] = {{"next", 3}};
    sh_package_link_edge b_edge[] = {{"back", 2}, {"data", 1}};
    f.units[0].implicit = root_edge; f.units[0].implicit_count = 1;
    f.units[2] = (sh_package_link_unit){"native", "a", NULL, NULL, 0};
    f.units[3] = (sh_package_link_unit){"native", "b", NULL, NULL, 0};
    f.units[2].implicit = a_edge; f.units[2].implicit_count = 1; f.units[2].original_binding = 1;
    f.units[3].implicit = b_edge; f.units[3].implicit_count = 2; f.units[3].original_binding = 1;
    f.count = 4;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.compiled->resource_count == 2 && out.implicit_count == 4);
    const sh_compiled_resource *owner = NULL;
    for (size_t i = 0; i < out.compiled->resource_count; i++)
        if (!strcmp(out.compiled->resources[i].native_type, "consumer")) owner = out.compiled->resources + i;
    assert(owner && owner->bound_dependency_count == 1);
    const sh_compiled_resource *target = sh_package_compilation_find(out.compiled, owner->bound_dependencies[0]);
    assert(target && !strcmp(target->native_type, "image"));
    assert(sh_package_link(sources, NULL, 0, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(!out.implicit && !out.implicit_count);
    sh_package_link_output_free(&out);
}

/* Campaign-only bytes a scope supplies keep their native name (original binding
 * 2), but never make a stock dependent original: that scope's consumer goes
 * private while every other scope keeps the stock name. */
static void campaign_originals(const sh_package_sources *sources)
{
    fixture f = {0}; sh_package_compilation first = {0}, second = {0};
    first.sources = second.sources = sources;
    sh_compiled_resource parts[4] = {0};
    const sh_compiled_resource *files[4];
    const char *paths[] = {"consumer/stock.bin", "consumer/stock.bin", "program/campaign.bin", "skeleton/campaign.bin"};
    const char *bodies[] = {"stock consumer", "stock consumer", "campaign program", "campaign skeleton"};
    sh_package_link_output out = {0};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_edge program[] = {{"program", 2}}, skeleton[] = {{"skeleton", 3}};
    char error[512];
    for (size_t i = 0; i < 4; i++) {
        parts[i].engine_path = paths[i]; parts[i].body = (unsigned char *)bodies[i];
        parts[i].body_length = strlen(bodies[i]); parts[i].source = SIZE_MAX; files[i] = parts + i;
    }
    f.units[0] = (sh_package_link_unit){"consumer", "stock", &first, files, 1};
    f.units[1] = (sh_package_link_unit){"consumer", "stock", &second, files + 1, 1};
    f.units[2] = (sh_package_link_unit){"renderprog", "campaign", &second, files + 2, 1};
    f.units[3] = (sh_package_link_unit){"skeleton", "campaign", &second, files + 3, 1};
    f.units[1].implicit = program; f.units[1].implicit_count = 1;
    f.units[2].implicit = skeleton; f.units[2].implicit_count = 1;
    f.units[0].original_binding = f.units[1].original_binding = 1;
    f.units[2].original_binding = f.units[3].original_binding = 2;
    f.count = 4; f.native_originals = 1;
    if (!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)))
        fprintf(stderr, "campaign originals: %s\n", error);
    assert(out.compiled && out.classes.nodes[0] != out.classes.nodes[1]);
    assert(f.original_bindings[out.classes.representatives[out.classes.nodes[0]]]);
    assert(!f.original_bindings[out.classes.representatives[out.classes.nodes[1]]]);
    assert(!strcmp(out.names[out.classes.nodes[0]], "stock") && strcmp(out.names[out.classes.nodes[1]], "stock"));
    /* Campaign data resting on campaign data keeps every name. */
    assert(!strcmp(out.names[out.classes.nodes[2]], "campaign") && !strcmp(out.names[out.classes.nodes[3]], "campaign"));
    /* A changed dependency still makes campaign data private. */
    f.units[3].original_binding = 0; f.assigned = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strcmp(out.names[out.classes.nodes[2]], "campaign") && !strcmp(out.names[out.classes.nodes[0]], "stock"));
    /* Certified like SnapMap bytes, both stock consumers would claim one name. */
    f.units[2].original_binding = f.units[3].original_binding = 1; f.assigned = 0;
    assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(strstr(error, "native identity collision") && strstr(error, "consumer stock"));
    sh_package_link_output_free(&out);
}

/* A declaration may assign one field twice (stock projectile impact effects
 * repeat rumbleDecl); the native reader resolves both and keeps the last. Both
 * references bind, each under its own replayable slot. */
static void repeated_field_slots(void)
{
    char base[MAX_PATH + 32], error[1024];
    snprintf(base, sizeof(base), "%s/repeated", root);
    create("repeated/overrides/rock/package.json", "{\"id\":\"rock\",\"name\":\"Rock\"}");
    create("repeated/overrides/rock/assets/generated/decls/entitydef/props/rock.decl", "{ edit = { } }");
    sh_package_sources *sources = sh_package_sources_scan(base, error, sizeof(error)); assert(sources);
    /* A captured original is read as the native reader reads it, not through
     * the package compiler's authoring checks. */
    sh_compiled_resource resources[2] = {0};
    resources[0].engine_path = "generated/decls/entitydef/props/rock.decl"; resources[0].type = "entitydef";
    resources[0].name = "props/rock"; resources[0].source = SIZE_MAX;
    resources[0].body = (unsigned char *)"{ edit = { material = \"materials/stone\"; material = \"materials/stone\"; health = 10; } }";
    resources[0].body_length = strlen((const char *)resources[0].body);
    resources[1].engine_path = "generated/decls/material/materials/stone.decl"; resources[1].type = "material";
    resources[1].name = "materials/stone"; resources[1].source = SIZE_MAX;
    resources[1].body = (unsigned char *)"{ color {1,0,0,1} }"; resources[1].body_length = 19;
    sh_package_compilation compiled = {0}; compiled.resources = resources; compiled.resource_count = 2;
    compiled.sources = sources;
    const sh_package_compilation *scope = &compiled;
    sh_package_link_inputs discovered = {0};
    assert(sh_package_link_inputs_open(&scope, 1, NULL, &discovered, error, sizeof(error)) && discovered.count == 2);
    fixture f = {0}; f.count = 2; f.repeat_slot = 1;
    for (size_t n = 0; n < 2; n++) f.units[n] = discovered.units[n];
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    sh_package_link_output out = {0};
    int ok = sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "repeated field: %s\n", error);
    assert(ok && out.compiled && out.classes.count == 2);
    sh_package_link_output_free(&out); sh_package_link_inputs_free(&discovered); sh_package_sources_free(sources);
}

int main(void)
{
    const char *folders[] = {"mars", "green", "duplicate"};
    const char *entity = "{ edit = { material = \"materials/stone\"; health = 10; } }";
    const char *colors[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }", "{ color {1,0,0,1} }"};
    sh_package_sources *sources;
    sh_package_scope views[3] = {{0}};
    sh_package_compilation *scopes[3] = {0};
    sh_package_link_inputs discovered = {0};
    const sh_compiled_resource *files[7][2] = {{0}};
    sh_package_link_output out = {0};
    fixture f = {0};
    sh_package_link_adapter adapter = {&f, resolve, bind_file, assign};
    char temporary[MAX_PATH], relative[256], error[1024];
    size_t entities[3], materials[3];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-linker-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"rocks\",\"name\":\"Rocks\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/props/rock.decl", folders[i]); create(relative, entity);
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/material/materials/stone.decl", folders[i]); create(relative, colors[i]);
        char stream[32780]; memset(stream, 'x', sizeof(stream) - 1); stream[sizeof(stream) - 1] = 0;
        snprintf(relative, sizeof(relative), "overrides/%s/equal.bin", folders[i]); create(relative, stream);
    }
    /* Different delivery content does not turn identical native assets into
     * another instance, but both supplying authored trees retain provenance. */
    create("overrides/duplicate/readme.txt", "Another author's intact delivery bundle.");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources && sources->component_count == 3);
    for (size_t s = 0; s < 3; s++) {
        assert(sh_package_scope_open(sources, s, &views[s], error, sizeof(error)));
        scopes[s] = sh_package_compile(&views[s].sources, NULL, NULL, error, sizeof(error));
        assert(scopes[s] && scopes[s]->resource_count == 2);
    }
    assert(sh_package_link_inputs_open((const sh_package_compilation *const *)scopes, 3,
        NULL, &discovered, error, sizeof(error)) && discovered.count == 6);
    for (size_t n = 0; n < discovered.count; n++) f.units[n] = discovered.units[n];
    for (size_t s = 0; s < 3; s++) {
        entities[s] = sh_package_link_inputs_find(&discovered, s, "entitydef", "props/rock");
        materials[s] = sh_package_link_inputs_find(&discovered, s, "material", "materials/stone");
        assert(entities[s] != SIZE_MAX && materials[s] != SIZE_MAX);
    }
    f.count = 6;
    native_collisions(sources, &f, &adapter, entities, materials);
    repeated_native_bindings(sources, &f, entities, materials);
    shared_replacement(sources, &f, materials);
    streamed_suppliers(sources);
    shared_member_variants(sources);
    foreign_suppliers();
    many_repeated_slots(sources);
    occurrence_table_replay(sources);
    repaired_order_names(sources);
    repeated_field_slots();
    campaign_originals(sources);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 4 && out.compiled->resource_count == 4 && out.compiled->duplicate_count == 2);
    assert(out.classes.nodes[entities[0]] == out.classes.nodes[entities[2]]);
    assert(out.classes.nodes[entities[0]] != out.classes.nodes[entities[1]]);
    /* Two unequal classes that both keep one native name refuse the link, and
     * the report names both claimants: their classes, certification, sources,
     * payloads and the bound dependencies that differ. */
    {
        sh_package_link_unit saved[12];
        memcpy(saved, f.units, sizeof(saved));
        f.units[entities[0]].fixed_name = 1; f.units[entities[1]].fixed_name = 1;
        adapter.note = collision_note; adapter.describe = collision_describe; collision_notes[0] = 0;
        assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        if (!strstr(error, "exhausted") || getenv("SH_LINKER_TEST_NOTES"))
            fprintf(stderr, "collision refusal: %s\nnotes:\n%s", error, collision_notes);
        assert(strstr(error, "exhausted") && strstr(error, "props/rock") && !out.compiled);
        assert(strstr(collision_notes, "collision claimant: unit ") && strstr(collision_notes, "collision holder: unit "));
        assert(strstr(collision_notes, "collision claimant source: fixture unit ") &&
            strstr(collision_notes, "collision holder source: fixture unit "));
        assert(strstr(collision_notes, "collision claimant differs (") && strstr(collision_notes, "materials/stone") &&
            strstr(collision_notes, "collision claimant file ") && strstr(collision_notes, "collision holder edges ("));
        adapter.note = NULL; adapter.describe = NULL;
        memcpy(f.units, saved, sizeof(saved));
        assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    }
    {
        sh_package_link_classes captured = {0};
        sh_package_link_adapter inspect = {&f, resolve, bind_file};
        int assigned = f.assigned;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &captured, error, sizeof(error)));
        assert(f.assigned == assigned && captured.count == out.classes.count);
        for (size_t i = 0; i < f.count; i++)
            assert(!memcmp(captured.identities[captured.nodes[i]],
                out.classes.identities[out.classes.nodes[i]], 32));
        assert(captured.nodes[entities[0]] == captured.nodes[entities[2]]);
        assert(captured.nodes[entities[0]] != captured.nodes[entities[1]]);
        f.unresolved = 1;
        assert(!sh_package_link_identify_units(f.units, f.count, &inspect, &captured, error, sizeof(error)));
        assert(!captured.count && !captured.nodes && *error && f.assigned == assigned);
        f.unresolved = 0;
        assert(sh_package_link_identify_units(NULL, 0, &inspect, &captured, error, sizeof(error)));
        assert(!captured.node_count && !captured.count);
        sh_package_link_classes_free(&captured);
    }
    {
        /* Appending resource units must preserve completed captures for the
         * unchanged prefix, while new units and invalidated units are read. */
        sh_package_link_classes prefix = {0}, grown = {0};
        sh_package_link_reuse *reuse = sh_package_link_reuse_open(2);
        sh_package_link_adapter inspect = {&f, resolve, bind_file};
        inspect.reuse = reuse;
        size_t bound = f.bound;
        assert(reuse && sh_package_link_identify_units(f.units, 2, &inspect, &prefix, error, sizeof(error)));
        assert(f.bound - bound == 2);
        assert(sh_package_link_reuse_reserve(reuse, f.count));
        bound = f.bound;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &grown, error, sizeof(error)));
        assert(f.bound - bound == f.count - 2 && grown.count == out.classes.count);
        for (size_t i = 0; i < f.count; i++)
            assert(!memcmp(grown.identities[grown.nodes[i]], out.classes.identities[out.classes.nodes[i]], 32));
        assert(sh_package_link_reuse_reserve(reuse, 1)); /* Reserving less never shrinks. */
        assert(!sh_package_link_reuse_reserve(reuse, SIZE_MAX));
        bound = f.bound;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &grown, error, sizeof(error)));
        assert(f.bound == bound); /* A failed reservation retains every capture. */
        sh_package_link_reuse_invalidate(reuse, materials[0]);
        assert(sh_package_link_reuse_reserve(reuse, f.count + 16));
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &grown, error, sizeof(error)));
        assert(f.bound - bound == 1);
        sh_package_link_reuse_close(reuse);
        sh_package_link_classes_free(&prefix); sh_package_link_classes_free(&grown);
    }
    {
        /* Later passes reuse each valid unit's capture and exact-byte proofs:
         * nothing is bound again and the classes are identical. An invalidated
         * unit is captured afresh from its current bytes. */
        sh_package_link_classes first = {0}, again = {0};
        sh_package_link_reuse *reuse = sh_package_link_reuse_open(f.count);
        sh_package_link_adapter inspect = {&f, resolve, bind_file};
        inspect.reuse = reuse;
        size_t bound = f.bound;
        assert(reuse && sh_package_link_identify_units(f.units, f.count, &inspect, &first, error, sizeof(error)));
        assert(f.bound - bound == f.count && first.count == 4);
        bound = f.bound;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &again, error, sizeof(error)));
        assert(f.bound == bound && again.count == first.count);
        for (size_t i = 0; i < f.count; i++)
            assert(again.nodes[i] == first.nodes[i] &&
                !memcmp(again.identities[again.nodes[i]], first.identities[first.nodes[i]], 32));
        /* The full link binds its replay only; its classes match the passes. */
        sh_package_link_output linked = {0};
        inspect.assign = assign; bound = f.bound;
        assert(sh_package_link(sources, f.units, f.count, &inspect, NULL, NULL, &linked, error, sizeof(error)));
        assert(linked.classes.count == first.count && f.bound - bound < 2 * f.count);
        sh_package_link_output_free(&linked); inspect.assign = NULL;
        /* Green's stone becomes red: stale captures would keep four classes. */
        sh_compiled_resource *stone = (sh_compiled_resource *)f.units[materials[1]].files[0];
        unsigned char kept[64]; size_t length = stone->body_length;
        assert(stone->body && length == strlen(colors[1]) && length < sizeof(kept));
        memcpy(kept, stone->body, length); memcpy(stone->body, colors[0], length);
        bound = f.bound;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &again, error, sizeof(error)));
        assert(f.bound == bound && again.count == 4);
        sh_package_link_reuse_invalidate(reuse, materials[1]);
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &again, error, sizeof(error)));
        assert(f.bound - bound == 1 && again.count == 2);
        assert(again.nodes[entities[0]] == again.nodes[entities[1]] && again.nodes[materials[0]] == again.nodes[materials[1]]);
        memcpy(stone->body, kept, length);
        bound = f.bound;
        assert(sh_package_link_identify_units(f.units, f.count, &inspect, &again, error, sizeof(error)));
        assert(f.bound - bound == 1 && again.count == 4);
        sh_package_link_reuse_close(reuse);
        sh_package_link_classes_free(&first); sh_package_link_classes_free(&again);
    }
    {
        /* A stable adapter lets projection skip replaying units held in
         * memory: a class whose targets all keep their names (the materials)
         * projects its own bytes, duplicates are not replayed, and classes
         * with renamed references (the entities) still replay. The linked
         * output is identical. */
        sh_package_link_output plain = {0}, skipped = {0};
        size_t bound = f.bound;
        assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &plain, error, sizeof(error)));
        size_t replayed = f.bound - bound;
        adapter.stable = stable_units; bound = f.bound;
        assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &skipped, error, sizeof(error)));
        adapter.stable = NULL;
        /* Identify binds all six units; projection replays the two entity
         * classes only, instead of four classes and two duplicates. */
        assert(replayed == 12 && f.bound - bound == 8);
        assert(plain.classes.count == skipped.classes.count &&
            plain.compiled->resource_count == skipped.compiled->resource_count);
        for (size_t i = 0; i < f.count; i++)
            assert(!strcmp(plain.names[plain.classes.nodes[i]], skipped.names[skipped.classes.nodes[i]]));
        for (size_t i = 0; i < plain.compiled->resource_count; i++) {
            const sh_compiled_resource *a = plain.compiled->resources + i, *b = skipped.compiled->resources + i;
            size_t alen = 0, blen = 0;
            unsigned char *x = sh_package_compilation_read(plain.compiled, a, SIZE_MAX, &alen, error, sizeof(error));
            unsigned char *y = sh_package_compilation_read(skipped.compiled, b, SIZE_MAX, &blen, error, sizeof(error));
            assert(!strcmp(a->engine_path, b->engine_path) && x && y && alen == blen && !memcmp(x, y, alen));
            assert(a->source_count == b->source_count && a->native_original == b->native_original);
            free(x); free(y);
        }
        sh_package_link_output_free(&plain); sh_package_link_output_free(&skipped);
    }
    {
        const char map[] = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        sh_package_provenance contexts = {0}, saved = {0}, restored = {0};
        size_t unit_contexts[7], roots[3] = {entities[0], entities[2], entities[0]}, length;
        int present; char *carrier;
        for (size_t s = 0; s < 3; s++) {
            const sh_package_component *component = sources->components + s;
            size_t context = sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_ISOLATED,
                sources->fingerprints + component->owner, 1, component->relative);
            assert(context != SIZE_MAX); unit_contexts[s * 2] = unit_contexts[s * 2 + 1] = context;
        }
        assert(sh_package_link_provenance(&out, f.units, f.count, &contexts, unit_contexts,
            roots, 3, &saved, error, sizeof(error)));
        /* Exact duplicate assets share one binding, retaining both distinct
         * authored trees. The unused green variant is not embedded. */
        assert(saved.binding_count == 1 && saved.context_count == 2 && saved.bindings[0].context_count == 2);
        assert(!strcmp(saved.bindings[0].name, out.names[out.classes.nodes[entities[0]]]));
        carrier = sh_map_provenance_write(map, sizeof(map) - 1, &saved, &length, error, sizeof(error));
        assert(carrier && sh_map_provenance_read(carrier, length, &restored, &present, error, sizeof(error)) && present);
        assert(restored.binding_count == 1 && !strcmp(restored.bindings[0].name, saved.bindings[0].name));
        HeapFree(GetProcessHeap(), 0, carrier);
        roots[1] = entities[1];
        assert(sh_package_link_provenance(&out, f.units, f.count, &contexts, unit_contexts,
            roots, 2, &saved, error, sizeof(error)) && saved.binding_count == 2 && saved.context_count == 3);
        assert(sh_package_link_provenance(&out, f.units, f.count, &contexts, unit_contexts,
            NULL, 0, &saved, error, sizeof(error)) && !saved.binding_count && !saved.context_count);
        roots[0] = f.count;
        assert(!sh_package_link_provenance(&out, f.units, f.count, &contexts, unit_contexts,
            roots, 1, &saved, error, sizeof(error)) && !saved.binding_count && !saved.context_count);
        unit_contexts[entities[0]] = SIZE_MAX; roots[0] = entities[0];
        assert(!sh_package_link_provenance(&out, f.units, f.count, &contexts, unit_contexts,
            roots, 1, &saved, error, sizeof(error)) && !saved.binding_count);
        sh_package_provenance_free(&contexts); sh_package_provenance_free(&saved); sh_package_provenance_free(&restored);
    }
    for (size_t s = 0; s < 3; s++) {
        char path[256]; size_t length;
        snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", out.names[out.classes.nodes[entities[s]]]);
        const sh_compiled_resource *resource = sh_package_compilation_find(out.compiled, path);
        unsigned char *body = sh_package_compilation_read(out.compiled, resource, SIZE_MAX, &length, error, sizeof(error));
        assert(body && strstr((char *)body, out.names[out.classes.nodes[materials[s]]]) && strstr((char *)body, "health = 10"));
        assert(resource->source_count == (s == 1 ? 1 : 2)); free(body);
    }
    {
        int present = 1; sh_package_missing missing = {0};
        assert(sh_package_compilation_payload_missing(out.compiled, available, &present, &missing, error, sizeof(error)) && !missing.count);
        present = 0;
        assert(sh_package_compilation_payload_missing(out.compiled, available, &present, &missing, error, sizeof(error)));
        assert(missing.count == 2 && sh_package_owners_count(&missing.packages) == 3);
        sh_package_missing_free(&missing);
    }
    /* A new independent native identity must not rename existing variants. */
    char *stable = _strdup(out.names[out.classes.nodes[entities[0]]]); assert(stable);
    f.units[6] = (sh_package_link_unit){"image", "native/white", NULL, NULL, 0}; f.count = 7;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 5 && out.compiled->resource_count == 4 &&
        !strcmp(stable, out.names[out.classes.nodes[entities[0]]])); free(stable);
    f.count = 6;
    /* Failures clear the previous candidate, never publishing a partial set. */
    for (size_t i = 0; i < f.count; i++) f.units[i].original_binding = 1;
    f.units[materials[1]].original_binding = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) {
        size_t entity = out.classes.representatives[out.classes.nodes[entities[i]]];
        size_t material = out.classes.representatives[out.classes.nodes[materials[i]]];
        assert(f.original_bindings[entity] == (i != 1));
        assert(f.original_bindings[material] == (i != 1));
    }
    /* An uncertified duplicate cannot inherit its representative's certificate.
     * Its parents must become variants too, regardless of input order. */
    f.units[materials[2]].original_binding = 0;
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    for (size_t i = 0; i < out.classes.count; i++) assert(!f.original_bindings[out.classes.representatives[i]]);
    for (size_t i = 0; i < f.count; i++) f.units[i].original_binding = 0;
    int *faults[] = {&f.unresolved, &f.collision, &f.omit_replay, &f.bad_inspection, &f.abort_bind, &f.changed_source};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        *faults[i] = 1; f.assigned = 0;
        assert(!sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
        assert(*error && !out.compiled && !out.names && !out.classes.count); *faults[i] = 0;
    }
    /* A single manager with several physical files compares the complete set,
     * independent of source enumeration order. No text references in this unit. */
    sh_compiled_resource parts[4] = {0};
    sh_package_compilation fake = {0}; fake.sources = sources;
    for (size_t i = 0; i < 4; i++) {
        parts[i].engine_path = i % 2 ? "shader/fragment.bin" : "shader/vertex.bin";
        parts[i].body = (unsigned char *)(i % 2 ? "fragment" : "vertex"); parts[i].body_length = strlen((char *)parts[i].body);
        parts[i].source = SIZE_MAX;
    }
    files[0][0] = &parts[0]; files[0][1] = &parts[1];
    files[1][0] = &parts[3]; files[1][1] = &parts[2];
    f.units[0] = (sh_package_link_unit){"renderprog", "shared", &fake, files[0], 2};
    f.units[1] = (sh_package_link_unit){"renderprog", "shared", &fake, files[1], 2}; f.count = 2;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 1 && out.compiled->resource_count == 2 && out.compiled->duplicate_count == 2);
    {
        sh_package_changes changes = {0};
        sh_package_sources empty_sources = {0};
        sh_package_compilation empty = {0}; empty.sources = &empty_sources;
        sh_package_compilation *copy = sh_package_compilation_overlay(out.compiled, &empty, error, sizeof(error));
        assert(copy && sh_package_compilation_changes(NULL, copy, &changes, error, sizeof(error)));
        assert(changes.count == 2 && changes.added_count == 1 && changes.identities_complete);
        for (size_t i = 0; i < copy->resource_count; i++) {
            const sh_compiled_resource *resource = copy->resources + i;
            assert(!resource->type && !resource->name); /* Never a declaration. */
            assert(resource->native_type && !strcmp(resource->native_type, "renderprog"));
            assert(resource->native_name && !strcmp(resource->native_name, out.names[0]));
            assert(resource->native_name != out.compiled->resources[i].native_name);
            assert(changes.items[i].type && !strcmp(changes.items[i].type, "renderprog"));
        }
        sh_package_compilation_free(copy); sh_package_changes_free(&changes);
    }
    parts[3].body = (unsigned char *)"different"; parts[3].body_length = 9;
    assert(sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 2 && out.compiled->resource_count == 4);
    files[1][1] = files[1][0];
    assert(!sh_package_link(sources, f.units, 2, &adapter, NULL, NULL, &out, error, sizeof(error)) && *error);
    assert(sh_package_link(sources, NULL, 0, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.compiled && !out.compiled->resource_count && !out.classes.count);
    sh_package_link_output_free(&out);
    {
        sh_compiled_resource declaration = {0};
        declaration.engine_path = "generated/decls/material/original.decl";
        declaration.type = "material"; declaration.name = "original"; declaration.source = SIZE_MAX;
        declaration.body = (unsigned char *)"{}"; declaration.body_length = 2;
        const sh_compiled_resource *input = &declaration;
        sh_package_resource_projection projection = {"generated/decls/material/linked.decl", &input, 1,
            NULL, 0, "model", "linked"};
        assert(!sh_package_compilation_project(sources, &projection, 1, NULL, NULL, error, sizeof(error)));
        assert(strstr(error, "disagrees with declaration path"));
        projection.native_type = "material"; projection.native_name = "linked";
        sh_package_compilation *projected = sh_package_compilation_project(sources, &projection, 1,
            NULL, NULL, error, sizeof(error));
        assert(projected && !strcmp(projected->resources[0].native_name, "linked"));
        sh_package_compilation_free(projected);
        projection.native_name = NULL;
        assert(!sh_package_compilation_project(sources, &projection, 1, NULL, NULL, error, sizeof(error)));
    }
    original_aliases(sources);
    exact_witnesses(sources);
    implicit_ownership(sources);
    coupled_members(sources);
    implicit_bindings(sources);
    conditional_bindings(sources);
    sh_package_link_inputs_free(&discovered);
    for (size_t i = 0; i < 3; i++) { sh_package_compilation_free(scopes[i]); sh_package_scope_close(&views[i]); }
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) assert(sh_package_source_verify(&sources->files[i]));
    sh_package_sources_free(sources); cleanup();
    puts("package linker tests passed"); return failures != 0;
}
