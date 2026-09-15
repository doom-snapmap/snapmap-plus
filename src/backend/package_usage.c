#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_usage.h"
#include "resource_types.h"
#include "resource_graph.h"

typedef struct pu_visit { sh_package_reference_visitor visitor; void *context; } pu_visit;
typedef struct pu_states { sh_package_state_visitor visitor; void *context; } pu_states;

int sh_package_references_add(void *context, const char *type, const char *name)
{
    sh_package_references *references = context;
    sh_package_reference *items;
    char *type_copy, *name_copy;
    size_t i;
    if (!references || !type || !name || !*name) return 0;
    for (i = 0; i < references->count; i++)
        if (!strcmp(references->items[i].type, type) && !strcmp(references->items[i].name, name)) return 1;
    if (references->count >= SIZE_MAX / sizeof(*items)) return 0;
    type_copy = _strdup(type); name_copy = _strdup(name);
    if (!type_copy || !name_copy) { free(type_copy); free(name_copy); return 0; }
    items = realloc(references->items, (references->count + 1) * sizeof(*items));
    if (!items) { free(type_copy); free(name_copy); return 0; }
    references->items = items;
    items[references->count++] = (sh_package_reference){type_copy, name_copy};
    return 1;
}

void sh_package_references_free(sh_package_references *references)
{
    size_t i;
    if (!references) return;
    for (i = 0; i < references->count; i++) {
        free(references->items[i].type); free(references->items[i].name);
    }
    free(references->items); memset(references, 0, sizeof(*references));
}

static int pu_editor_type(const char *type)
{
    return !strncmp(type, "snapeditor", 10) || !strncmp(type, "snappropertyinspector", 21);
}

static const sh_json_field_span *pu_field(const sh_json_field_span *fields, size_t count, const char *key)
{
    size_t i, n = strlen(key);
    for (i = 0; i < count; i++) if (fields[i].key_length == n && !memcmp(fields[i].key, key, n)) return &fields[i];
    return NULL;
}
static int pu_text(const sh_json_field_span *field, char *out, size_t capacity)
{
    size_t length;
    return field && field->kind == SH_JSON_STRING &&
        sh_json_decode_string(field->value, field->value_length, out, capacity, &length) && strlen(out) == length;
}
static int pu_object(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    pu_visit *visit = (pu_visit *)context;
    char class_name[256], name[4096];
    const char *type = NULL;
    const sh_json_field_span *target = pu_field(fields, count, "targetType");
    size_t i;
    (void)depth;
    if (!target) target = pu_field(fields, count, "~type");
    if (!target) return 1;
    if (!pu_text(target, class_name, sizeof(class_name))) return 0;
    for (i = 0; i < sizeof(SH_RESOURCE_TYPES) / sizeof(SH_RESOURCE_TYPES[0]); i++)
        if (!strcmp(class_name, SH_RESOURCE_TYPES[i].class_name)) {
            if (type) return 0; /* class alone cannot distinguish two native managers */
            type = SH_RESOURCE_TYPES[i].type;
        }
    if (!type || pu_editor_type(type)) return 1;
    for (i = 0; i < 2; i++) {
        const sh_json_field_span *field = pu_field(fields, count, i ? "value" : "inherit");
        if (!field || field->kind == SH_JSON_NULL) continue;
        if (!pu_text(field, name, sizeof(name)) || (name[0] && !visit->visitor(visit->context, type, name))) return 0;
    }
    return 1;
}

static int pu_gameplay_field(void *context, const char *key, size_t length, unsigned depth)
{
    (void)context; (void)depth;
    return length != sizeof("editorVars") - 1 || memcmp(key, "editorVars", length) != 0;
}

static int pu_state(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    pu_states *visit = context;
    char target_name[256], class_name[256] = "", inherit[4096] = "";
    const sh_json_field_span *target = pu_field(fields, count, "targetType");
    const sh_json_field_span *state = pu_field(fields, count, "state"), *field;
    const char *edit;
    sh_json_object object = {0};
    int ok = 1;
    (void)depth;
    if (!state || state->kind == SH_JSON_NULL) return 1;
    if (!target) target = pu_field(fields, count, "~type");
    if (!target) return 1;
    if (!pu_text(target, target_name, sizeof(target_name))) return 0;
    if (strcmp(target_name, "idDeclEntityDef")) return 1;
    field = pu_field(fields, count, "className");
    if (field && field->kind != SH_JSON_NULL && !pu_text(field, class_name, sizeof(class_name))) return 0;
    field = pu_field(fields, count, "inherit");
    if (field && field->kind != SH_JSON_NULL && !pu_text(field, inherit, sizeof(inherit))) return 0;
    if (!sh_json_parse_object(state->value, state->value_length, 128, &object)) return 0;
    edit = sh_json_object_get(&object, "edit");
    if (edit && strcmp(edit, "null"))
        ok = visit->visitor(visit->context, class_name, inherit, edit, strlen(edit));
    sh_json_object_free(&object); return ok;
}

int sh_package_map_states(const char *json, size_t length,
    sh_package_state_visitor visitor, void *context)
{
    pu_states visit = {visitor, context};
    return visitor && sh_json_visit_objects_filtered(json, length, 128, pu_state, pu_gameplay_field, &visit);
}

int sh_package_map_references(const char *json, size_t length,
                              sh_package_reference_visitor visitor, void *context)
{
    pu_visit visit = {visitor, context};
    return visitor && sh_json_visit_objects_filtered(json, length, 128, pu_object, pu_gameplay_field, &visit);
}

typedef struct pu_owners {
    const sh_package_compilation *compiled;
    const sh_resource_catalog *catalog;
    sh_package_owners owners;
    int dependencies_complete;
    sh_package_references *resources;
} pu_owners;
static int pu_resource(pu_owners *used, const sh_compiled_resource *resource, const char *path)
{
    char *canonical;
    int ok;
    if (resource && !sh_package_owners_union(&used->owners, &resource->gameplay_owners)) return 0;
    if (!used->resources) return 1;
    canonical = used->compiled->canonical_path ? used->compiled->canonical_path(path) : sh_package_engine_path(path);
    if (!canonical) return 0;
    ok = sh_package_references_add(used->resources, "", canonical); free(canonical); return ok;
}
static int pu_collect(pu_owners *used, const char *type, const char *name)
{
    const sh_compiled_resource *resource;
    const sh_resource_catalog_entry *const *entries;
    char *path;
    size_t count, i, a, b, capacity;
    if (!*type) {
        resource = sh_package_compilation_find(used->compiled, name);
        return pu_resource(used, resource, resource ? resource->engine_path : name);
    }
    a = strlen(type); b = strlen(name);
    if (a > SIZE_MAX - 23 || b > SIZE_MAX - a - 23) return 0;
    capacity = a + b + 23;
    path = (char *)malloc(capacity);
    if (!path) return 0;
    snprintf(path, capacity, "generated/decls/%s/%s.decl", type, name);
    resource = sh_package_compilation_find(used->compiled, path);
    free(path);
    if (resource && !pu_resource(used, resource, resource->engine_path)) return 0;
    count = sh_resource_catalog_find(used->catalog, type, name, &entries);
    for (i = 0; i < count; i++) if (entries[i]->path[0]) {
        resource = sh_package_compilation_find(used->compiled, entries[i]->path);
        if (!pu_resource(used, resource, resource ? resource->engine_path : entries[i]->path)) return 0;
    }
    return 1;
}
static int pu_dependency(void *context, const char *parent_type, const char *parent_name,
                          const char *type, const char *name)
{
    (void)parent_type; (void)parent_name;
    if (pu_editor_type(type)) return 0;
    return pu_collect((pu_owners *)context, type, name) ? 1 : -1;
}
static int pu_reference(void *context, const char *type, const char *name)
{
    pu_owners *used = (pu_owners *)context;
    sh_resource_graph_status status;
    /* A root can be supplied by a package before its first native load. */
    if (!pu_collect(used, type, name)) return 0;
    status = sh_resource_graph_walk_status(type, name, pu_dependency, used);
    if (status == SH_RESOURCE_GRAPH_ERROR) return 0;
    if (status == SH_RESOURCE_GRAPH_INCOMPLETE) used->dependencies_complete = 0;
    return 1;
}
int sh_package_map_owners(const sh_package_compilation *compiled, const sh_resource_catalog *catalog,
                           const char *json, size_t length, const sh_package_references *references,
                           sh_package_owners *owners, int *dependencies_complete)
{
    pu_owners used = {compiled, catalog, {0}, 1};
    size_t i;
    sh_package_owners_free(owners);
    if (dependencies_complete) *dependencies_complete = 0;
    if (!compiled || !owners || !sh_package_map_references(json, length, pu_reference, &used)) goto bad;
    if (references) {
        if (references->incomplete) used.dependencies_complete = 0;
        for (i = 0; i < references->count; i++)
            if (!pu_reference(&used, references->items[i].type, references->items[i].name)) goto bad;
    }
    if (dependencies_complete) *dependencies_complete = used.dependencies_complete;
    *owners = used.owners; return 1;
bad:
    sh_package_owners_free(&used.owners); return 0;
}

int sh_package_map_resources(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const char *json, size_t length,
    const sh_package_references *references, sh_package_references *out)
{
    sh_package_references resources = {0};
    pu_owners used = {compiled, catalog, {0}, 1, &resources};
    if (!out || out == references) return 0;
    sh_package_references_free(out);
    if (!compiled || !sh_package_map_references(json, length, pu_reference, &used)) goto bad;
    if (references) {
        if (references->incomplete) used.dependencies_complete = 0;
        for (size_t i = 0; i < references->count; i++)
            if (!pu_reference(&used, references->items[i].type, references->items[i].name)) goto bad;
    }
    resources.incomplete = !used.dependencies_complete;
    *out = resources; sh_package_owners_free(&used.owners); return 1;
bad:
    sh_package_references_free(&resources); sh_package_owners_free(&used.owners); return 0;
}

int sh_package_map_policy(const sh_package_compilation *compiled,
                            const sh_resource_catalog *catalog,
                            const char *json, size_t length, const sh_package_references *references,
                            sh_package_policy *out, sh_package_owners *owners,
                            int *dependencies_complete,
                            char *error, size_t error_capacity)
{
    sh_package_owners selected = {0};
    int complete = 0;
    sh_package_owners_free(owners);
    if (dependencies_complete) *dependencies_complete = 0;
    if (error && error_capacity) error[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!sh_package_map_owners(compiled, catalog, json, length, references, &selected, &complete)) {
        if (error && error_capacity) snprintf(error, error_capacity, "map package policy could not resolve gameplay ownership");
        return 0;
    }
    if (!sh_package_compilation_policy(compiled, &selected, out, error, error_capacity)) {
        sh_package_owners_free(&selected); return 0;
    }
    if (owners) *owners = selected;
    else sh_package_owners_free(&selected);
    if (dependencies_complete) *dependencies_complete = complete;
    return 1;
}
