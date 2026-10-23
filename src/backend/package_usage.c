#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_usage.h"
#include "resource_types.h"
#include "resource_graph.h"
#include "native_filename.h"

typedef struct pu_visit {
    sh_package_reference_visitor visitor;
    void *context;
    sh_package_reference_site_visitor site;
} pu_visit;
typedef struct pu_states { sh_package_state_visitor visitor; void *context; int borrowed; } pu_states;

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
        sh_native_json_decode_string(field->value, field->value_length, out, capacity, &length) && strlen(out) == length;
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
        if (!pu_text(field, name, sizeof(name))) return 0;
        if (name[0] && !(visit->site ? visit->site(visit->context, class_name, type, name,
                field->value, field->value_length) : visit->visitor(visit->context, type, name))) return 0;
    }
    return 1;
}

static int pu_gameplay_field(void *context, const char *key, size_t length, unsigned depth)
{
    (void)context; (void)depth;
    return length != sizeof("editorVars") - 1 || memcmp(key, "editorVars", length) != 0;
}

static int pu_edit_span(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    if (!depth) {
        const sh_json_field_span *edit = pu_field(fields, count, "edit");
        if (edit) *(sh_json_field_span *)context = *edit;
    }
    return 1;
}
static int pu_state(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    pu_states *visit = context;
    char target_name[256], class_name[256] = "", inherit[4096] = "";
    const sh_json_field_span *target = pu_field(fields, count, "targetType");
    const sh_json_field_span *state = pu_field(fields, count, "state"), *field;
    sh_json_field_span edit = {0};
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
    if (state->kind != SH_JSON_OBJECT || !sh_native_json_visit_objects_filtered(state->value,
        state->value_length, 128, pu_edit_span, NULL, &edit)) return 0;
    if (!edit.value || edit.kind == SH_JSON_NULL) return 1;
    if (visit->borrowed) return visit->visitor(visit->context, class_name, inherit, edit.value, edit.value_length);
    if (edit.value_length == SIZE_MAX) return 0;
    char *copy = malloc(edit.value_length + 1); if (!copy) return 0;
    memcpy(copy, edit.value, edit.value_length); copy[edit.value_length] = 0;
    int ok = visit->visitor(visit->context, class_name, inherit, copy, edit.value_length);
    free(copy); return ok;
}

int sh_package_map_states(const char *json, size_t length,
    sh_package_state_visitor visitor, void *context)
{
    pu_states visit = {visitor, context};
    return visitor && sh_native_json_visit_objects_filtered(json, length, 128, pu_state, pu_gameplay_field, &visit);
}

int sh_package_map_references(const char *json, size_t length,
                              sh_package_reference_visitor visitor, void *context)
{
    pu_visit visit = {visitor, context};
    return visitor && sh_native_json_visit_objects_filtered(json, length, 128, pu_object, pu_gameplay_field, &visit);
}

int sh_package_map_state_sites(const char *json, size_t length,
    sh_package_state_visitor visitor, void *context)
{
    pu_states visit = {visitor, context, 1};
    return visitor && sh_native_json_visit_objects_filtered(json, length, 128, pu_state, pu_gameplay_field, &visit);
}

int sh_package_map_reference_sites(const char *json, size_t length,
    sh_package_reference_site_visitor visitor, void *context)
{
    pu_visit visit = {NULL, context, visitor};
    return visitor && sh_native_json_visit_objects_filtered(json, length, 128, pu_object, pu_gameplay_field, &visit);
}

/* A set of exact (type, name) pairs, for the repeats one traversal skips. */
typedef struct pu_seen { char **keys; size_t count, capacity, *slots, slot_count; } pu_seen;
/* The compiled resources by native identity (type and name folded as
 * _stricmp folds ASCII; non-ASCII bytes all one), each list in resource
 * order; built on the first identity lookup of a traversal. */
typedef struct pu_identity_index { size_t *heads, *next, slot_count; int built, failed; } pu_identity_index;
typedef struct pu_owners {
    const sh_package_compilation *compiled;
    const sh_resource_catalog *catalog;
    sh_package_owners owners;
    int dependencies_complete;
    sh_package_references *resources;
    /* Within one traversal a root or a collected identity adds the same
     * owners and resources however often it is met, so each is handled once. */
    pu_seen referenced, collected;
    pu_identity_index identities;
} pu_owners;
static sh_package_usage_stats g_pu_stats;
void sh_package_usage_stats_take(sh_package_usage_stats *out, int reset)
{
    if (out) *out = g_pu_stats;
    if (reset) memset(&g_pu_stats, 0, sizeof(g_pu_stats));
}
static size_t pu_hash(const char *type, const char *name, int fold)
{
    size_t h = (size_t)14695981039346656037ull;
    for (const unsigned char *at = (const unsigned char *)type; *at; at++)
        h = (h ^ (fold ? (*at >= 0x80 ? 0x80u : *at >= 'A' && *at <= 'Z' ? *at + 32u : *at) : *at)) * 1099511628211ull;
    h = (h ^ 0x1ffu) * 1099511628211ull;
    for (const unsigned char *at = (const unsigned char *)name; *at; at++)
        h = (h ^ (fold ? (*at >= 0x80 ? 0x80u : *at >= 'A' && *at <= 'Z' ? *at + 32u : *at) : *at)) * 1099511628211ull;
    return h;
}
/* 1 when (type, name) was already in the set, 0 after adding it, -1 on
 * allocation failure (the caller then handles it as new). */
static int pu_seen_add(pu_seen *set, const char *type, const char *name)
{
    size_t h = pu_hash(type, name, 0), tl = strlen(type), nl = strlen(name);
    if (set->slot_count) {
        for (size_t at = h & (set->slot_count - 1); set->slots[at]; at = (at + 1) & (set->slot_count - 1)) {
            const char *key = set->keys[set->slots[at] - 1];
            if (!strcmp(key, type) && !strcmp(key + tl + 1, name)) return 1;
        }
    }
    if (set->count + 1 > set->slot_count / 2) {
        size_t slots = set->slot_count ? set->slot_count * 2 : 1024;
        size_t *table = (size_t *)calloc(slots, sizeof(*table));
        char **keys = set->count + 1 > set->capacity ? (char **)realloc(set->keys, slots / 2 * sizeof(*keys)) : set->keys;
        if (!table || !keys) { free(table); if (keys) set->keys = keys; return -1; }
        set->keys = keys; if (set->capacity < slots / 2) set->capacity = slots / 2;
        for (size_t i = 0; i < set->count; i++) {
            const char *key = set->keys[i];
            size_t at = pu_hash(key, key + strlen(key) + 1, 0) & (slots - 1);
            while (table[at]) at = (at + 1) & (slots - 1);
            table[at] = i + 1;
        }
        free(set->slots); set->slots = table; set->slot_count = slots;
    }
    char *key = (char *)malloc(tl + nl + 2);
    if (!key) return -1;
    memcpy(key, type, tl + 1); memcpy(key + tl + 1, name, nl + 1);
    size_t at = h & (set->slot_count - 1);
    while (set->slots[at]) at = (at + 1) & (set->slot_count - 1);
    set->keys[set->count] = key; set->slots[at] = ++set->count;
    return 0;
}
static void pu_seen_free(pu_seen *set)
{
    for (size_t i = 0; i < set->count; i++) free(set->keys[i]);
    free(set->keys); free(set->slots); memset(set, 0, sizeof(*set));
}
static void pu_identities_free(pu_identity_index *index)
{ free(index->heads); free(index->next); memset(index, 0, sizeof(*index)); }
static void pu_owners_release(pu_owners *used)
{ pu_seen_free(&used->referenced); pu_seen_free(&used->collected); pu_identities_free(&used->identities); }
/* Each resource's native identities chained by folded key; a resource is
 * chained once per identity, in resource order (heads are pushed from the
 * last resource down). A failed build leaves the full scan. */
static void pu_identities_build(pu_owners *used)
{
    pu_identity_index *index = &used->identities;
    const sh_package_compilation *compiled = used->compiled;
    size_t total = 0;
    index->built = 1;
    for (size_t i = 0; i < compiled->resource_count; i++) total += sh_package_resource_native_count(compiled->resources + i);
    size_t slots = 1;
    while (slots < total * 2 + 2) slots <<= 1;
    index->heads = (size_t *)calloc(slots, sizeof(*index->heads));
    index->next = (size_t *)malloc((total + 1) * 2 * sizeof(*index->next));   /* pairs: resource, next */
    if (!index->heads || !index->next) { pu_identities_free(index); index->built = 1; index->failed = 1; return; }
    index->slot_count = slots;
    size_t used_entries = 0;
    for (size_t i = compiled->resource_count; i-- > 0;) {
        const sh_compiled_resource *resource = compiled->resources + i;
        size_t count = sh_package_resource_native_count(resource);
        for (size_t k = 0; k < count; k++) {
            sh_package_native_identity identity = sh_package_resource_native_at(resource, k);
            if (!identity.type || !identity.name) continue;
            size_t b = pu_hash(identity.type, identity.name, 1) & (slots - 1);
            index->next[used_entries * 2] = i; index->next[used_entries * 2 + 1] = index->heads[b];
            index->heads[b] = ++used_entries;
        }
    }
}
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
        name = sh_native_virtual_path(name);
        resource = sh_package_compilation_find(used->compiled, name);
        return pu_resource(used, resource, resource ? resource->engine_path : name);
    }
    /* Compiled binary aliases have no stock catalog rows. Every permutation
     * sharing that manager identity contributes its original delivery owners,
     * in resource order (the index lists candidates in that order; each is
     * matched exactly, and a resource is visited once). */
    if (!used->identities.built) pu_identities_build(used);
    if (used->identities.failed) {
        for (i = 0; i < used->compiled->resource_count; i++) {
            resource = used->compiled->resources + i;
            if (sh_package_resource_native_matches(resource, type, name) &&
                !pu_resource(used, resource, resource->engine_path)) return 0;
        }
    } else {
        size_t last = SIZE_MAX;
        for (size_t at = used->identities.heads[pu_hash(type, name, 1) & (used->identities.slot_count - 1)]; at;
             at = used->identities.next[(at - 1) * 2 + 1]) {
            size_t index = used->identities.next[(at - 1) * 2];
            if (index == last) continue;   /* a resource with two identities in one bucket */
            resource = used->compiled->resources + index;
            if (!sh_package_resource_native_matches(resource, type, name)) continue;
            last = index;
            if (!pu_resource(used, resource, resource->engine_path)) return 0;
        }
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
static int pu_collect_once(pu_owners *used, const char *type, const char *name)
{
    g_pu_stats.collects++;
    if (pu_seen_add(&used->collected, type, name) == 1) { g_pu_stats.collects_repeated++; return 1; }
    return pu_collect(used, type, name);
}
static int pu_dependency(void *context, const char *parent_type, const char *parent_name,
                          const char *type, const char *name)
{
    (void)parent_type; (void)parent_name;
    if (pu_editor_type(type)) return 0;
    return pu_collect_once((pu_owners *)context, type, name) ? 1 : -1;
}
static int pu_reference(void *context, const char *type, const char *name)
{
    pu_owners *used = (pu_owners *)context;
    sh_resource_graph_status status;
    g_pu_stats.roots++;
    /* A root already walked in this traversal adds nothing again: its collects
     * are idempotent and its walk's status was already accounted. */
    if (type && name && pu_seen_add(&used->referenced, type, name) == 1) { g_pu_stats.roots_repeated++; return 1; }
    /* A root can be supplied by a package before its first native load. */
    if (!pu_collect_once(used, type, name)) return 0;
    g_pu_stats.walks++;
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
    pu_owners_release(&used);
    *owners = used.owners; return 1;
bad:
    pu_owners_release(&used);
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
    pu_owners_release(&used);
    *out = resources; sh_package_owners_free(&used.owners); return 1;
bad:
    pu_owners_release(&used);
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
