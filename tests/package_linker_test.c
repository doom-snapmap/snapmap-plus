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

typedef struct fixture {
    sh_package_link_unit units[12];
    size_t count, assigned, bound;
    int unresolved, collision, changed_source, omit_replay, bad_inspection, abort_bind, shared_name;
    int original_paths, alias_rewrite;
    int member_fault, repeat_slot, native_originals;
    int original_bindings[12];
    unsigned char *mutate_witness;
    sh_package_link_edge *mutate_implicit;
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
    int native = f->original_paths || (f->native_originals && original_binding);
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
    return native->replace_shared ? SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED : SH_PACKAGE_LINK_REGISTRY_AVAILABLE;
}

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
    shared_replacement(sources, &f, materials);
    streamed_suppliers(sources);
    repeated_field_slots();
    campaign_originals(sources);
    assert(sh_package_link(sources, f.units, f.count, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 4 && out.compiled->resource_count == 4 && out.compiled->duplicate_count == 2);
    assert(out.classes.nodes[entities[0]] == out.classes.nodes[entities[2]]);
    assert(out.classes.nodes[entities[0]] != out.classes.nodes[entities[1]]);
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
    sh_package_link_inputs_free(&discovered);
    for (size_t i = 0; i < 3; i++) { sh_package_compilation_free(scopes[i]); sh_package_scope_close(&views[i]); }
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) assert(sh_package_source_verify(&sources->files[i]));
    sh_package_sources_free(sources); cleanup();
    puts("package linker tests passed"); return failures != 0;
}
