#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "decl_compose.h"
#include "decl_server_path.h"
#include "decl_text.h"
#include "decl_entity_class.h"
#include "decl_tree.h"

typedef struct pc_input {
    const sh_package_source_file *file;
    size_t index;
    char *path;
} pc_input;

static const sh_package_builtin *pc_builtin(const sh_package_compile_environment *environment,
    const char *path)
{
    size_t i;
    for (i = 0; i < environment->builtin_count; i++)
        if (!strcmp(environment->builtins[i].engine_path, path)) return &environment->builtins[i];
    return NULL;
}

static int pc_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const sh_package_compile_environment *environment = (const sh_package_compile_environment *)context;
    const sh_package_builtin *builtin;
    int scope;
    *body = NULL; *length = 0;
    scope = environment->baseline ? environment->baseline(environment->baseline_context, path, body, length) : 0;
    if (scope) return scope;
    free(*body); *body = NULL; *length = 0;
    builtin = pc_builtin(environment, path);
    if (!builtin) return 0;
    *body = (unsigned char *)malloc(builtin->length + 1);
    if (!*body) return -1;
    memcpy(*body, builtin->body, builtin->length); (*body)[builtin->length] = 0;
    *length = builtin->length; return 3;
}

typedef struct pc_class_name { struct pc_class_name *next; char *name; } pc_class_name;
typedef struct pc_class_context {
    const sh_package_sources *sources;
    const sh_compiled_resource *resource;
    const sh_package_compile_environment *environment;
    sh_decl_composition_role role;
    size_t index;
    pc_class_name *names;
    char parent_error[512];
    size_t input_count;
    const pc_input *inputs;
    size_t source_input_count;
    /* An owner-alone check sees one package's contributions, the original and
     * product defaults. It records reads that only another package could
     * satisfy and reads that failed rather than finding authored content. */
    int alone, peer_source, operational;
    size_t alone_owner, alone_count;
} pc_class_context;

static char *pc_key(const sh_package_compile_environment *environment, const char *path);

/* A contribution reads its own package; a result reads every package, or only
 * the package being checked alone. */
static int pc_view_includes(const pc_class_context *view, sh_decl_composition_role role,
    size_t owner, const sh_package_source_file *file)
{
    if (role != SH_DECL_COMPOSITION_RESULT) return file->owner == owner;
    return !view->alone || file->owner == view->alone_owner;
}

void sh_package_resource_inputs_free(sh_package_resource_inputs *inputs)
{
    if (!inputs) return;
    free((char *)inputs->original.text);
    for (size_t i = 0; i < inputs->count; i++) free((char *)inputs->contributions[i].text);
    free(inputs->contributions); free(inputs->owners); memset(inputs, 0, sizeof(*inputs));
}

static int pc_source_inputs(void *context, sh_decl_composition_role role, size_t index,
    const char *engine_path, sh_package_resource_inputs *out, char *error, size_t capacity)
{
    pc_class_context *view = context;
    const sh_package_compile_environment *environment = view->environment;
    const sh_package_builtin *builtin = NULL;
    char *path = NULL;
    unsigned char *body = NULL;
    size_t length = 0, owner = SIZE_MAX, count = 0, first = 0, end;
    int result = -1;
    if (error && capacity) *error = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (role < SH_DECL_COMPOSITION_ORIGINAL || role > SH_DECL_COMPOSITION_RESULT ||
        (role == SH_DECL_COMPOSITION_CONTRIBUTION && index >= view->input_count) ||
        !engine_path || !(path = pc_key(environment, engine_path))) goto done;
    out->original_scope = pc_original((void *)environment, path, &body, &length);
    if (out->original_scope < 0 || (out->original_scope && !body)) goto done;
    out->original = (sh_decl_source){(char *)body, length}; body = NULL;
    if (role == SH_DECL_COMPOSITION_ORIGINAL) { result = out->original_scope ? 1 : 0; goto done; }
    if (role == SH_DECL_COMPOSITION_CONTRIBUTION && view->alone)
        owner = index < view->alone_count ? view->alone_owner : SIZE_MAX;
    else if (role == SH_DECL_COMPOSITION_CONTRIBUTION && index < view->resource->source_count)
        owner = view->sources->files[view->resource->sources[index]].owner;
    if (role == SH_DECL_COMPOSITION_RESULT || owner == SIZE_MAX) builtin = pc_builtin(environment, path);
    /* Reuse the compiler's sorted canonical index. A schema read touches only
     * the matching resource, even in a large collection of installed bundles. */
    end = view->source_input_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (strcmp(view->inputs[middle].path, path) < 0) first = middle + 1;
        else end = middle;
    }
    count = builtin != NULL;
    while (end < view->source_input_count && !strcmp(view->inputs[end].path, path)) {
        if (pc_view_includes(view, role, owner, view->inputs[end].file)) count++;
        end++;
    }
    if (view->alone && end > first && !count && !out->original_scope) view->peer_source = 1;
    if (count > SIZE_MAX / sizeof(*out->contributions) || count > SIZE_MAX / sizeof(*out->owners)) goto done;
    if (count) {
        out->contributions = calloc(count, sizeof(*out->contributions));
        out->owners = calloc(count, sizeof(*out->owners));
        if (!out->contributions || !out->owners) goto done;
    }
    for (size_t i = first; i < end; i++) {
        const sh_package_source_file *file = view->inputs[i].file;
        if (!pc_view_includes(view, role, owner, file)) continue;
        body = sh_package_source_read(file, (size_t)PTRDIFF_MAX, &length, error, capacity);
        if (!body) goto done;
        out->contributions[out->count] = (sh_decl_source){(char *)body, length};
        out->owners[out->count++] = file->owner; body = NULL;
    }
    if (builtin) {
        body = malloc(builtin->length + 1);
        if (!body) goto done;
        memcpy(body, builtin->body, builtin->length); body[builtin->length] = 0;
        out->contributions[out->count] = (sh_decl_source){(char *)body, builtin->length};
        out->owners[out->count++] = SIZE_MAX; body = NULL;
    }
    result = out->count || out->original_scope ? 1 : 0;
done:
    free(path); free(body);
    if (result < 0) {
        view->operational = 1;
        sh_package_resource_inputs_free(out);
        if (error && capacity && !*error) snprintf(error, capacity, "resource source view is unreadable or invalid: %s",
            engine_path ? engine_path : "(missing path)");
    }
    return result;
}

static int pc_parent_read(void *context, const char *name, sh_decl_source *out)
{
    pc_class_context *view = (pc_class_context *)context;
    sh_package_resource_inputs inputs = {0};
    size_t name_length = strlen(name);
    char *path = NULL;
    int result = -1;
    *out = (sh_decl_source){0};
    if (name_length > SIZE_MAX - 34 || !(path = (char *)malloc(name_length + 34))) goto done;
    snprintf(path, name_length + 34, "generated/decls/entitydef/%s.decl", name);
    result = pc_source_inputs(view, view->role, view->index, path, &inputs, view->parent_error, sizeof(view->parent_error));
    if (result < 0) goto done;
    if (!inputs.count) {
        *out = inputs.original; inputs.original = (sh_decl_source){0}; goto done;
    }
    result = -1;
    out->text = sh_decl_compose_root_metadata(inputs.original_scope ? inputs.original :
        (sh_decl_source){"{}", 2}, inputs.contributions, inputs.count, &out->length,
        view->parent_error, sizeof(view->parent_error), NULL);
    if (out->text) result = 1;
done:
    if (result < 0 && !view->parent_error[0]) snprintf(view->parent_error, sizeof(view->parent_error),
        "parent source is unreadable or ambiguous: %s", name);
    sh_package_resource_inputs_free(&inputs); free(path); return result;
}

static int pc_class_derives(void *context, const char *child, const char *parent)
{
    pc_class_context *view = (pc_class_context *)context;
    return view->environment->class_derives(view->environment->type_context, child, parent);
}

static int pc_entity_class(void *context, const sh_decl_node *definition,
    sh_decl_composition_role role, size_t index, sh_decl_value_type *type,
    char *error, size_t capacity)
{
    pc_class_context *view = (pc_class_context *)context;
    sh_decl_entity_class_source source = {view, pc_parent_read, pc_class_derives};
    pc_class_name *owned = (pc_class_name *)calloc(1, sizeof(*owned));
    view->role = role; view->index = index; view->parent_error[0] = 0;
    if (!owned) { view->operational = 1; return -1; }
    owned->name = sh_decl_entity_tree_class(definition, &source, error, capacity);
    if (!owned->name) {
        if (view->parent_error[0] && error && capacity) snprintf(error, capacity, "%s", view->parent_error);
        free(owned); return -1;
    }
    owned->next = view->names; view->names = owned;
    *type = (sh_decl_value_type){owned->name, ""}; return 1;
}

static void pc_classes_free(pc_class_context *context)
{
    while (context->names) {
        pc_class_name *next = context->names->next;
        free(context->names->name); free(context->names); context->names = next;
    }
}

/* *allocation distinguishes memory failure from invalid or conflicting authored
 * policy, so a failed allocation never isolates the package being checked. */
static int pc_policy_merge(sh_json_object *target, const sh_json_object *source,
                            const char *path, unsigned depth, char *error, size_t capacity, int *allocation)
{
    size_t i;
    if (depth > 12u) { snprintf(error, capacity, "package policy is nested too deeply at %s", path); return 0; }
    for (i = 0; i < source->count; i++) {
        const sh_json_member *member = &source->members[i];
        const char *previous = NULL;
        size_t j;
        if (!member->key_length || member->key_length != strlen(member->key)) {
            snprintf(error, capacity, "invalid package policy key at %s", path); return 0;
        }
        for (j = 0; j < target->count; j++) if (!_stricmp(target->members[j].key, member->key)) {
            previous = target->members[j].value_json; break;
        }
        if (previous && strcmp(previous, member->value_json)) {
            sh_json_object left = {0}, right = {0};
            size_t path_length = strlen(path);
            char *field;
            if (path_length > SIZE_MAX - 2u || member->key_length > SIZE_MAX - path_length - 2u ||
                !(field = (char *)malloc(path_length + member->key_length + 2u))) {
                *allocation = 1; snprintf(error, capacity, "could not allocate package policy path"); return 0;
            }
            memcpy(field, path, path_length); field[path_length] = '.';
            memcpy(field + path_length + 1u, member->key, member->key_length + 1u);
            int parsed = sh_json_parse_object(previous, strlen(previous), 16u, &left) &&
                         sh_json_parse_object(member->value_json, strlen(member->value_json), 16u, &right);
            if (parsed) {
                char *merged; size_t length;
                int ok = pc_policy_merge(&left, &right, field, depth + 1u, error, capacity, allocation);
                free(field);
                sh_json_object_free(&right);
                merged = ok ? sh_json_serialize_object(&left, 0, &length) : NULL;
                sh_json_object_free(&left);
                if (ok && (!merged || !sh_json_object_set(target, target->members[j].key, merged, 16u))) {
                    *allocation = 1; ok = 0;
                }
                free(merged);
                if (!ok) return 0;
                continue;
            }
            sh_json_object_free(&left); sh_json_object_free(&right);
            /* Equivalent JSON string escaping is not a policy conflict. */
            {
                size_t an = strlen(previous) + 1u, bn = strlen(member->value_json) + 1u, al = 0, bl = 0;
                char *a = (char *)malloc(an), *b = (char *)malloc(bn);
                int same = a && b && sh_json_decode_string(previous, an - 1u, a, an, &al) &&
                    sh_json_decode_string(member->value_json, bn - 1u, b, bn, &bl) && al == bl && !memcmp(a, b, al);
                if (!a || !b) { free(a); free(b); free(field); *allocation = 1; return 0; }
                free(a); free(b);
                if (same) { free(field); continue; }
            }
            snprintf(error, capacity, "conflicting package policy: %s", field); free(field); return 0;
        }
        if (!previous && !sh_json_object_set(target, member->key, member->value_json, 16u)) {
            *allocation = 1; return 0;
        }
    }
    return 1;
}

static int pc_variant_shadowed(const sh_package_sources *sources, size_t candidate, size_t primary);

static int pc_policies(const sh_package_sources *sources, const sh_package_owners *owners,
                        sh_package_policy *out, char *error, size_t capacity, int *allocation)
{
    const char *names[] = {"requirements", "strings", "hud"};
    sh_json_object *targets[] = {&out->requirements, &out->strings, &out->hud};
    size_t i, section, other_package;
    int ignored = 0;
    if (!allocation) allocation = &ignored;
    for (i = 0; i < sources->component_count; i++) {
        const sh_package_component *component = &sources->components[i];
        int shadowed = 0;
        if (owners && !sh_package_owners_contains(owners, component->owner)) continue;
        /* A delivered variant's policy follows its resources: the authored
         * variant of the same package identity owns it. */
        for (other_package = 0; !shadowed && other_package < sources->package_count; other_package++)
            if (!owners || sh_package_owners_contains(owners, other_package))
                shadowed = pc_variant_shadowed(sources, component->owner, other_package);
        if (shadowed) continue;
        for (section = 0; section < 3; section++) {
            const char *raw = sh_package_descriptor_section(&component->descriptor, names[section]);
            sh_json_object object = {0};
            int ok;
            if (!raw) continue;
            if (!sh_json_parse_object(raw, strlen(raw), 16, &object)) {
                /* The descriptor already validated this fragment at depth 24. */
                sh_json_kind kind;
                if (sh_json_validate(raw, strlen(raw), 16, &kind)) *allocation = 1;
                else snprintf(error, capacity, "%s policy of package %s is nested too deeply",
                    names[section], sources->packages[component->owner].name);
                return 0;
            }
            ok = pc_policy_merge(targets[section], &object, names[section], 0, error, capacity, allocation);
            if (!ok && !*allocation) {
                size_t previous;
                /* Find an actual incompatible pair for the diagnostic, rather
                 * than assigning precedence to an arbitrary package order. */
                for (previous = 0; previous < i; previous++) {
                    const sh_package_component *other = &sources->components[previous];
                    if (owners && !sh_package_owners_contains(owners, other->owner)) continue;
                    const char *other_raw = sh_package_descriptor_section(&other->descriptor, names[section]);
                    sh_json_object probe = {0};
                    char detail[1024] = "";
                    int probe_allocation = 0;
                    int conflict = other_raw && sh_json_parse_object(other_raw, strlen(other_raw), 16, &probe) &&
                        !pc_policy_merge(&probe, &object, names[section], 0, detail, sizeof(detail), &probe_allocation) &&
                        !probe_allocation;
                    sh_json_object_free(&probe);
                    if (conflict) {
                        snprintf(error, capacity, "%s; packages %s (%s) and %s (%s)", detail,
                            sources->packages[other->owner].name, other->descriptor.id,
                            sources->packages[component->owner].name, component->descriptor.id);
                        break;
                    }
                }
            }
            sh_json_object_free(&object);
            if (!ok) return 0;
        }
    }
    /* Cvars are data from a narrow supported policy, never command text. */
    if (out->requirements.count) {
        const char *raw = sh_json_object_get(&out->requirements, "cvars");
        sh_json_object cvars = {0};
        int ok = out->requirements.count == 1 && raw && sh_json_parse_object(raw, strlen(raw), 2, &cvars);
        for (i = 0; ok && i < cvars.count; i++) {
            const sh_json_member *m = &cvars.members[i];
            ok = (!strcmp(m->key, "g_useResourceBlackList") || !strcmp(m->key, "g_useImageBlackList")) && !strcmp(m->value_json, "0");
        }
        sh_json_object_free(&cvars);
        if (!ok) { snprintf(error, capacity, "requirements contain an unsupported setting"); return 0; }
    }
    return 1;
}

void sh_package_policy_free(sh_package_policy *policy)
{
    if (!policy) return;
    sh_json_object_free(&policy->requirements);
    sh_json_object_free(&policy->strings);
    sh_json_object_free(&policy->hud);
    memset(policy, 0, sizeof(*policy));
}

/* Peer policies have already been validated separately. Only this map-to-local
 * boundary has precedence; ordinary package composition remains symmetric. */
static int pc_policy_overlay(sh_json_object *local, const sh_json_object *map, unsigned depth)
{
    size_t i, j;
    if (depth > 12u) return 0;
    for (i = 0; i < map->count; i++) {
        const sh_json_member *member = &map->members[i];
        const char *key = member->key, *previous = NULL;
        sh_json_object left = {0}, right = {0};
        char *combined = NULL; size_t length;
        int ok;
        for (j = 0; j < local->count; j++) if (!_stricmp(local->members[j].key, key)) {
            key = local->members[j].key; previous = local->members[j].value_json; break;
        }
        if (previous && sh_json_parse_object(previous, strlen(previous), 16, &left) &&
            sh_json_parse_object(member->value_json, strlen(member->value_json), 16, &right)) {
            ok = pc_policy_overlay(&left, &right, depth + 1u);
            if (ok) combined = sh_json_serialize_object(&left, 0, &length);
            ok = combined && sh_json_object_set(local, key, combined, 16);
        } else ok = sh_json_object_set(local, key, member->value_json, 16);
        free(combined); sh_json_object_free(&left); sh_json_object_free(&right);
        if (!ok) return 0;
    }
    return 1;
}

static int pc_view_policies(const sh_package_compilation *compiled, const sh_package_owners *owners,
    sh_package_policy *out, char *error, size_t capacity)
{
    sh_package_owners local = {0}, map = {0};
    sh_package_policy upper = {0};
    size_t i; int ok = 0;
    if (!compiled->map_overlay) return pc_policies(compiled->sources, owners, out, error, capacity, NULL);
    if (compiled->map_owner_begin > compiled->sources->package_count) goto done;
    for (i = 0; i < compiled->sources->package_count; i++) if (!owners || sh_package_owners_contains(owners, i))
        if (!sh_package_owners_add(i < compiled->map_owner_begin ? &local : &map, i)) goto done;
    if (!pc_policies(compiled->sources, &local, out, error, capacity, NULL) ||
        !pc_policies(compiled->sources, &map, &upper, error, capacity, NULL)) goto done;
    ok = pc_policy_overlay(&out->requirements, &upper.requirements, 0) &&
        pc_policy_overlay(&out->strings, &upper.strings, 0) &&
        pc_policy_overlay(&out->hud, &upper.hud, 0);
done:
    sh_package_owners_free(&local); sh_package_owners_free(&map); sh_package_policy_free(&upper);
    if (!ok && !error[0]) snprintf(error, capacity, "cannot prepare map package policy overlay");
    return ok;
}

int sh_package_compilation_policy(const sh_package_compilation *compilation,
                                    const sh_package_owners *owners, sh_package_policy *out,
                                    char *error, size_t error_capacity)
{
    const sh_package_sources *sources = compilation ? compilation->sources : NULL;
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!sources || !sh_package_owners_within(owners, sources->package_count)) goto invalid;
    for (i = 0; i < sources->component_count; i++)
        if (sources->components[i].owner >= sources->package_count) goto invalid;
    if (!pc_view_policies(compilation, owners, out, error, error_capacity)) {
        sh_package_policy_free(out);
        return 0;
    }
    return 1;
invalid:
    if (error && error_capacity) snprintf(error, error_capacity, "package policy ownership is invalid");
    return 0;
}

static int pc_compare(const void *a, const void *b)
{
    const pc_input *left = (const pc_input *)a, *right = (const pc_input *)b;
    int c = strcmp(left->path, right->path);
    if (c) return c;
    if (left->file->owner != right->file->owner) return left->file->owner < right->file->owner ? -1 : 1;
    return strcmp(left->file->relative, right->file->relative);
}

static int pc_identical(const sh_package_source_file *a, const sh_package_source_file *b)
{
    return a->length == b->length && !memcmp(a->digest, b->digest, 32);
}

static int pc_equal(const unsigned char *a, size_t an, const unsigned char *b, size_t bn)
{
    return an == bn && (!an || !memcmp(a, b, an));
}

static const sh_decl_collection_rule *pc_rules(const char *type, size_t *count)
{
    static const sh_decl_collection_rule blocking = {"edit.renderModelInfoList", "renderModelMaterial"};
    static const sh_decl_collection_rule encounters = {"edit.validEncounters", NULL};
    static const sh_decl_collection_rule editor_properties[] = {
        {"edit.propertySheets", ""},
        {"edit.propertySheets.item[*].properties", "path"}
    };
    static const sh_decl_collection_rule conductor[] = {
        {"edit.aiTypeList", "aiType"},
        {"edit.globalAIMods", "aiType"}
    };
    *count = 0;
    if (!type) return NULL;
    if (!_stricmp(type, "snappropertyinspector_blockingvolumerendermodel")) { *count = 1; return &blocking; }
    if (!_stricmp(type, "snappropertyinspector_whitelistencounterdecl") ||
        !_stricmp(type, "snappropertyinspector_whitelistencounterdecllist")) { *count = 1; return &encounters; }
    if (!_stricmp(type, "snapeditorentitydef")) {
        *count = sizeof(editor_properties) / sizeof(editor_properties[0]); return editor_properties;
    }
    if (!_stricmp(type, "entitydef")) {
        *count = sizeof(conductor) / sizeof(conductor[0]); return conductor;
    }
    return NULL;
}

/* *invalid reports an authored path that cannot be a declaration identity. */
static int pc_identity(sh_compiled_resource *resource, int *invalid, char *error, size_t capacity)
{
    char type[SH_DECL_SERVER_TYPE_CAP], name[SH_DECL_SERVER_NAME_CAP], source[SH_DECL_SERVER_SOURCE_CAP];
    const char *reason = NULL;
    if (strncmp(resource->engine_path, "generated/decls/", 16)) return 1;
    if (!sh_decl_server_identity_from_relative(resource->engine_path + 16,
        type, sizeof(type), name, sizeof(name), source, sizeof(source), &reason)) {
        *invalid = 1;
        snprintf(error, capacity, "%s: %s", resource->engine_path, reason ? reason : "invalid declaration identity"); return 0;
    }
    resource->type = _strdup(type); resource->name = _strdup(name);
    return resource->type && resource->name;
}

static int pc_unparseable(sh_decl_source text)
{
    char reason[256] = "";
    sh_decl_node *tree;
    errno = 0;
    tree = sh_decl_tree_parse(text, reason, sizeof(reason));
    if (tree) { sh_decl_tree_free(tree); return 0; }
    return errno != ENOMEM && !strstr(reason, "allocation");
}

static void pc_report_conflict(const sh_package_compilation *compilation,
    const sh_compiled_resource *resource, sh_decl_conflict cause, int built_in,
    const char *detail, char *error, size_t capacity)
{
    size_t i, used, total = resource->source_count + (built_in ? 1u : 0u);
    int selected = cause.first < total || cause.second < total, names = 0, n;
    if (!error || !capacity) return;
    n = snprintf(error, capacity, "%s [", resource->engine_path);
    if (n < 0 || (size_t)n >= capacity) return;
    used = (size_t)n;
    for (i = 0; i < total; i++) {
        const char *name;
        if (selected && i != cause.first && i != cause.second) continue;
        if (i == resource->source_count) name = resource->generated ? "generated resource" : "built-in default";
        else {
            size_t owner = compilation->sources->files[resource->sources[i]].owner;
            size_t prior;
            /* Diagnostics must still identify every owner after index63,
             * including when reporting an allocation failure. */
            for (prior = 0; prior < i; prior++) {
                if (selected && prior != cause.first && prior != cause.second) continue;
                if (compilation->sources->files[resource->sources[prior]].owner == owner) break;
            }
            if (prior < i) continue;
            name = compilation->sources->packages[owner].name;
        }
        n = snprintf(error + used, capacity - used, "%s%s", names ? "; " : "", name);
        if (n < 0 || (size_t)n >= capacity - used) return;
        used += (size_t)n; names++;
    }
    snprintf(error + used, capacity - used, "]: %s", detail);
}

static int pc_file_equal(const sh_package_file_identity *a, const sh_package_file_identity *b)
{
    return a->length == b->length && !memcmp(a->digest, b->digest, sizeof(a->digest));
}

/* Opaque payloads are indivisible replacements. Compare verified identities,
 * keeping the bytes on disk even when an installed original is very large.
 * Declarations continue through their byte/schema composition below. */
static int pc_compile_opaque(sh_package_compilation *out, sh_compiled_resource *resource,
    const sh_package_compile_environment *environment, const sh_package_builtin *builtin,
    char *error, size_t capacity)
{
    sh_package_original_identity original = {0};
    sh_package_file_identity selected = {0};
    sh_decl_conflict cause = {SIZE_MAX, SIZE_MAX};
    size_t effective = SIZE_MAX;
    unsigned char *base = NULL;
    size_t length = 0;
    int handled = 0;
    if (environment->baseline_identity) {
        handled = environment->baseline_identity(environment->identity_context,
            resource->engine_path, &original, error, capacity);
        if (handled < 0 || handled > 1 || (handled && (original.scope < 0 || original.scope > 3))) {
            if (!error[0]) snprintf(error, capacity, "%s: original file identity is unavailable or invalid", resource->engine_path);
            return 0;
        }
    }
    /* An absent game original does not suppress a product default or producer.
     * The compiler view's ordinary reader supplies their original evaluation. */
    if (!handled || (!original.scope && (builtin || environment->produce))) {
        original.scope = pc_original((void *)environment, resource->engine_path, &base, &length);
        if (original.scope < 0 || original.scope > 3 || (original.scope && !base)) {
            free(base);
            if (!error[0]) snprintf(error, capacity, "%s: installed original is unreadable or ambiguous", resource->engine_path);
            return 0;
        }
        if (original.scope && !sh_package_bytes_identity(base, length, &original.file)) {
            free(base); snprintf(error, capacity, "%s: cannot hash installed original", resource->engine_path); return 0;
        }
        free(base);
    }
    resource->baseline_known = original.scope;
    for (size_t i = 0; i < resource->source_count + (builtin != NULL); i++) {
        sh_package_file_identity current;
        const sh_package_source_file *file = i < resource->source_count ?
            &out->sources->files[resource->sources[i]] : NULL;
        int unchanged;
        if (file) {
            /* Verify every physical authored member, including duplicates. A
             * stale member cannot borrow another package's matching digest. */
            if (!sh_package_source_verify(file)) {
                cause.first = i;
                pc_report_conflict(out, resource, cause, builtin != NULL,
                    "source changed or is unreadable", error, capacity); return 0;
            }
            current.length = file->length;
            memcpy(current.digest, file->digest, sizeof(current.digest));
        } else if (!sh_package_bytes_identity(builtin->body, builtin->length, &current)) {
            snprintf(error, capacity, "%s: cannot hash product contribution", resource->engine_path); return 0;
        }
        unchanged = original.scope && pc_file_equal(&original.file, &current);
        if (file && (!unchanged || original.scope == 2)) {
            if (!sh_package_owners_add(&resource->owners, file->owner) ||
                !sh_package_owners_add(&resource->gameplay_owners, file->owner)) return 0;
        }
        if (unchanged) continue;
        if (effective == SIZE_MAX) { selected = current; effective = i; cause.first = i; }
        else if (!pc_file_equal(&selected, &current)) {
            cause.second = i;
            pc_report_conflict(out, resource, cause, builtin != NULL,
                !environment->baseline && !environment->baseline_identity && !builtin ?
                    "different contributions have no verified original for composition" :
                    "different opaque replacements cannot be combined", error, capacity); return 0;
        }
    }
    /* With no edits, any authored original is the same effective payload.
     * With only a product source, retain its owned bytes after evaluation. */
    if (effective == SIZE_MAX) effective = 0;
    if (effective < resource->source_count) resource->source = resource->sources[effective];
    else if (builtin) {
        resource->source = SIZE_MAX;
        resource->body = malloc(builtin->length + 1);
        if (!resource->body) return 0;
        memcpy(resource->body, builtin->body, builtin->length);
        resource->body[builtin->length] = 0; resource->body_length = builtin->length;
    } else return 0;
    return 1;
}

/* Two packages that share a descriptor identity are variants of one package, not
 * peers. An authored variant owns every resource it supplies; a delivered one
 * fills only what the author does not. This keeps a map's delivery installable
 * beside the author's own copy without overwriting it, without inventing a
 * priority setting, and without reporting a contradiction the transient map
 * layer already resolves. Peer packages with different identities are unchanged:
 * their overlapping edits still compose or conflict. */
static int pc_variant_shadowed(const sh_package_sources *sources, size_t candidate, size_t primary)
{
    const char *a, *b;
    if (candidate == primary) return 0;
    if (!sh_package_source_delivered(sources, candidate)) return 0;
    if (sh_package_source_delivered(sources, primary)) return 0;
    a = sh_package_source_identity(sources, candidate);
    b = sh_package_source_identity(sources, primary);
    return a && b && !strcmp(a, b);
}

/* Drop a delivered variant's contributions to a resource an authored variant of
 * the same package also supplies. Contributions from other packages, and the
 * delivered variant's own new resources, are untouched. */
static void pc_resolve_variants(const sh_package_sources *sources, sh_compiled_resource *resource)
{
    size_t i, j, kept = 0;
    if (resource->source_count < 2) return;
    for (i = 0; i < resource->source_count; i++) {
        size_t owner = sources->files[resource->sources[i]].owner;
        int shadowed = 0;
        for (j = 0; !shadowed && j < resource->source_count; j++)
            shadowed = pc_variant_shadowed(sources, owner, sources->files[resource->sources[j]].owner);
        if (!shadowed) resource->sources[kept++] = resource->sources[i];
    }
    resource->source_count = kept;
}

/* A resource that fails to compose belongs to one package only when that
 * package's contributions fail the same composition alone, against the
 * original and product defaults. A check that needs a source only another
 * package supplies, fails to read or allocate, or cannot name one of the
 * package's own contributions attributes nothing: peer conflicts, inheritance
 * from another package and original failures keep the whole transaction.
 * Returns the owner, with its own diagnostic in error, or SIZE_MAX. */
static size_t pc_alone_invalid(const sh_package_compilation *out, const sh_compiled_resource *resource,
    const sh_package_compile_environment *environment, const sh_package_builtin *builtin,
    pc_class_context *classes, const sh_decl_composition_schema *schema, int typed, int custom_reader,
    sh_decl_source base, int have_base, const sh_decl_source *texts, char *error, size_t capacity)
{
    const sh_package_source_file *files = out->sources->files;
    const sh_decl_collection_rule *rules;
    size_t count = resource->source_count, saved_inputs = classes->input_count, rule_count = 0;
    size_t found = SIZE_MAX, i, j, *map = NULL;
    sh_decl_source *alone = NULL;
    rules = pc_rules(resource->type, &rule_count);
    if (count >= SIZE_MAX / sizeof(*alone) ||
        !(map = (size_t *)malloc((count + 1u) * sizeof(*map))) ||
        !(alone = (sh_decl_source *)malloc((count + 1u) * sizeof(*alone)))) goto done;
    for (i = 0; i < count; i++) {
        size_t owner = files[resource->sources[i]].owner, own = 0, total, distinct = 0;
        sh_decl_conflict cause = {SIZE_MAX, SIZE_MAX}, changes = {SIZE_MAX, SIZE_MAX};
        char detail[512] = "", *body = NULL;
        size_t length = 0;
        int result = 0, failed = 0;
        for (j = 0; j < i; j++) if (files[resource->sources[j]].owner == owner) break;
        if (j < i) continue;
        for (j = i; j < count; j++) if (files[resource->sources[j]].owner == owner) {
            alone[own] = texts[j]; map[own++] = j;
        }
        total = own;
        if (builtin) { alone[total] = texts[count]; map[total++] = count; }
        for (j = 0; j < total; j++) {
            size_t previous;
            if (have_base && pc_equal((const unsigned char *)base.text, base.length,
                (const unsigned char *)alone[j].text, alone[j].length)) continue;
            for (previous = 0; previous < j; previous++)
                if (pc_equal((const unsigned char *)alone[previous].text, alone[previous].length,
                    (const unsigned char *)alone[j].text, alone[j].length)) break;
            if (previous < j) continue;
            if (!distinct) changes.first = j;
            else if (distinct == 1) changes.second = j;
            distinct++;
        }
        /* Without an original reader, composition is unavailable to everyone. */
        if (((!typed || !schema->resolve) && distinct <= 1u) ||
            (!have_base && !environment->baseline && !builtin)) continue;
        classes->alone = 1; classes->alone_owner = owner; classes->alone_count = own;
        classes->input_count = total; classes->peer_source = classes->operational = 0;
        errno = 0;
        if (environment->compose_custom) {
            sh_package_source_view view = {classes, pc_source_inputs};
            result = environment->compose_custom(environment->type_context, resource->type,
                have_base ? base : (sh_decl_source){"{}", 2}, alone, total, &body, &length,
                detail, sizeof(detail), &cause, &view);
            failed = result < 0 || (result > 0 && !body);
            free(body); body = NULL;
        }
        if (!result && custom_reader) {
            snprintf(detail, sizeof(detail), "native declaration family requires a verified composition adapter");
            cause = changes; failed = 1;
        } else if (!result) {
            body = sh_decl_compose_resource(resource->type, have_base ? base : (sh_decl_source){"{}", 2},
                alone, total, rules, rule_count, typed ? schema : NULL, &length,
                detail, sizeof(detail), &cause);
            failed = !body; free(body); body = NULL;
        }
        classes->alone = 0; classes->input_count = saved_inputs;
        if (!failed) continue;
        if (classes->operational || errno == ENOMEM || strstr(detail, "allocation")) break;
        if (classes->peer_source || !(cause.first < own || cause.second < own)) continue;
        cause.first = cause.first < total ? map[cause.first] : SIZE_MAX;
        cause.second = cause.second < total ? map[cause.second] : SIZE_MAX;
        pc_report_conflict(out, resource, cause, builtin != NULL, detail, error, capacity);
        found = owner; break;
    }
done:
    classes->alone = 0; classes->input_count = saved_inputs;
    free(map); free(alone); return found;
}

static int pc_compile_resource(sh_package_compilation *out, sh_compiled_resource *resource,
                                const sh_package_compile_environment *environment,
                                const sh_package_builtin *generated,
                                const pc_input *inputs, size_t source_input_count,
                                char *error, size_t error_capacity)
{
    const sh_package_source_file *files = out->sources->files;
    const sh_package_builtin *builtin = generated ? generated : pc_builtin(environment, resource->engine_path);
    unsigned char *base = NULL, **bodies = NULL;
    size_t base_length = 0, i, effective = 0, distinct = 0, rule_count = 0;
    size_t input_count = resource->source_count + (builtin != NULL);
    size_t *lengths = NULL;
    sh_decl_source *texts = NULL;
    sh_decl_conflict cause = {SIZE_MAX, SIZE_MAX};
    const sh_decl_collection_rule *rules;
    pc_class_context classes = {0};
    sh_package_source_view source_view = {&classes, pc_source_inputs};
    sh_decl_composition_schema schema = {0};
    int typed = 0, custom_reader = 0, composing = 0;
    int have_base = 0, all_equal = 1, ok = 0;
    char detail[512] = "";
    classes.sources = out->sources; classes.resource = resource; classes.environment = environment;
    classes.input_count = input_count;
    classes.inputs = inputs; classes.source_input_count = source_input_count;
    int invalid_identity = 0;
    if (!pc_identity(resource, &invalid_identity, error, error_capacity)) {
        if (invalid_identity && environment->invalid_package && resource->source_count)
            *environment->invalid_package = files[resource->sources[0]].owner;
        goto done;
    }
    if (!resource->type) return pc_compile_opaque(out, resource, environment, builtin, error, error_capacity);
    if (environment->baseline || builtin) {
        have_base = pc_original((void *)environment, resource->engine_path, &base, &base_length);
        if (have_base < 0 || (have_base > 0 && !base)) {
            if (!error[0]) snprintf(error, error_capacity, "%s: installed original is unreadable or ambiguous", resource->engine_path);
            goto done;
        }
    }
    resource->baseline_known = have_base;
    typed = environment->types && resource->type && !strcmp(resource->type, "entitydef");
    if (typed) {
        schema.types = environment->types; schema.context = &classes; schema.resolve = pc_entity_class;
    } else if (environment->types && resource->type && environment->declaration_state) {
        int resolved = environment->declaration_state(environment->type_context,
            resource->type, &schema.state_type);
        if (resolved < 0 || (resolved && (!schema.state_type.name || !*schema.state_type.name))) {
            snprintf(detail, sizeof(detail), "native declaration state metadata is unavailable"); goto conflict;
        }
        typed = resolved > 0; custom_reader = !typed; schema.types = environment->types;
    }
    for (i = 1; i < resource->source_count; i++)
        if (!pc_identical(&files[resource->sources[0]], &files[resource->sources[i]])) all_equal = 0;
    if (!input_count || input_count < resource->source_count || input_count > SIZE_MAX / sizeof(*texts)) goto done;
    bodies = (unsigned char **)calloc(input_count, sizeof(*bodies));
    lengths = (size_t *)calloc(input_count, sizeof(*lengths));
    texts = (sh_decl_source *)calloc(input_count, sizeof(*texts));
    if (!bodies || !lengths || !texts) goto done;
    for (i = 0; i < resource->source_count; i++) {
        size_t previous;
        const sh_package_source_file *file = &files[resource->sources[i]];
        /* Share exact duplicate source reads within this compilation only. */
        for (previous = 0; previous < i; previous++) if (pc_identical(file, &files[resource->sources[previous]])) break;
        if (previous < i) { bodies[i] = bodies[previous]; lengths[i] = lengths[previous]; }
        else {
            bodies[i] = sh_package_source_read(file, (size_t)PTRDIFF_MAX,
                                                &lengths[i], detail, sizeof(detail));
            if (!bodies[i]) { cause.first = i; cause.second = SIZE_MAX; goto conflict; }
        }
        if (resource->type && !sh_decl_text_well_formed(bodies[i], lengths[i])) {
            cause.first = i; cause.second = SIZE_MAX;
            if (environment->invalid_package) *environment->invalid_package = file->owner;
            snprintf(detail, sizeof(detail), "malformed declaration in %s", file->relative); goto conflict;
        }
        texts[i].text = (const char *)bodies[i]; texts[i].length = lengths[i];
        if (have_base && pc_equal(base, base_length, bodies[i], lengths[i])) {
            if (have_base == 2) {
                if (!sh_package_owners_add(&resource->owners, file->owner) ||
                    !sh_package_owners_add(&resource->gameplay_owners, file->owner)) goto done;
            }
            continue;
        }
        if (!sh_package_owners_add(&resource->owners, file->owner)) goto done;
        if (!((have_base == 1 || have_base == 3) && resource->type && !strcmp(resource->type, "entitydef") &&
              sh_decl_entity_gameplay_equal((sh_decl_source){(const char *)base, base_length}, texts[i])))
            if (!sh_package_owners_add(&resource->gameplay_owners, file->owner)) goto done;
        if (previous == i) {
            if (!distinct) cause.first = i;
            else if (distinct == 1) cause.second = i;
            effective = i; distinct++;
        }
    }
    if (builtin) {
        size_t previous;
        texts[resource->source_count] = (sh_decl_source){(const char *)builtin->body, builtin->length};
        if (resource->type && !sh_decl_text_well_formed(builtin->body, builtin->length)) {
            cause.first = resource->source_count; cause.second = SIZE_MAX;
            snprintf(detail, sizeof(detail), "malformed built-in declaration"); goto conflict;
        }
        for (previous = 0; previous < resource->source_count; previous++)
            if (pc_equal((const unsigned char *)texts[previous].text, texts[previous].length,
                         builtin->body, builtin->length)) break;
        if (previous == resource->source_count &&
            (!have_base || !pc_equal(base, base_length, builtin->body, builtin->length))) {
            if (!distinct) cause.first = resource->source_count;
            else if (distinct == 1) cause.second = resource->source_count;
            effective = resource->source_count; distinct++;
        }
        if (resource->source_count && !pc_equal((const unsigned char *)texts[0].text,
            texts[0].length, builtin->body, builtin->length)) all_equal = 0;
    }
    composing = 1;
    /* Entity headers require normalization even without overlapping changes.
     * Other native families can also accept custom source dialects (particles
     * are one example); preserve their bytes when no composition is needed. */
    if ((!typed || !schema.resolve) && (distinct <= 1u || all_equal)) {
        resource->source = effective < resource->source_count ? resource->sources[effective] : SIZE_MAX;
        if (resource->type || resource->source == SIZE_MAX) {
            resource->body = (unsigned char *)malloc(texts[effective].length + 1u);
            if (!resource->body) goto done;
            memcpy(resource->body, texts[effective].text, texts[effective].length);
            resource->body[texts[effective].length] = 0;
            resource->body_length = texts[effective].length;
        }
        ok = 1; goto done;
    }
    if (!resource->type || (!have_base && !environment->baseline && !builtin)) {
        snprintf(detail, sizeof(detail), "%s", !have_base && !environment->baseline && !builtin ?
            "different contributions have no verified original for composition" :
            "different opaque replacements cannot be combined"); goto conflict;
    }
    if (environment->compose_custom) {
        char *body = NULL;
        size_t length = 0;
        int result = environment->compose_custom(environment->type_context, resource->type,
            have_base ? (sh_decl_source){(const char *)base, base_length} : (sh_decl_source){"{}", 2},
            texts, input_count, &body, &length, detail, sizeof(detail), &cause, &source_view);
        if (result < 0 || (result > 0 && !body)) { free(body); goto conflict; }
        if (result > 0) {
            resource->body = (unsigned char *)body; resource->body_length = length;
            resource->composed = distinct > 1 && !all_equal;
            resource->source = resource->source_count ? resource->sources[0] : SIZE_MAX; ok = 1; goto done;
        }
        free(body);
    }
    if (custom_reader) {
        snprintf(detail, sizeof(detail), "native declaration family requires a verified composition adapter"); goto conflict;
    }
    rules = pc_rules(resource->type, &rule_count);
    /* A reader-proven absent identity is a new declaration. Its original is
     * empty state, so compatible additions compose without an author-supplied
     * baseline file. A missing reader never establishes absence. */
    resource->body = (unsigned char *)sh_decl_compose_resource(resource->type, have_base ?
        (sh_decl_source){(const char *)base, base_length} : (sh_decl_source){"{}", 2},
        texts, input_count, rules, rule_count, typed ? &schema : NULL,
        &resource->body_length, detail, sizeof(detail), &cause);
    if (!resource->body) {
        /* A contribution that cannot be parsed on its own is that package's
         * defect. Conflicts, missing parents and adapters stay unattributed. */
        if (environment->invalid_package && cause.first < resource->source_count &&
            cause.second == SIZE_MAX && pc_unparseable(texts[cause.first]))
            *environment->invalid_package = files[resource->sources[cause.first]].owner;
        goto conflict;
    }
    resource->composed = distinct > 1 && !all_equal;
    resource->source = resource->source_count ? resource->sources[0] : SIZE_MAX; ok = 1; goto done;
conflict:
    if (composing && environment->invalid_package && *environment->invalid_package == SIZE_MAX) {
        size_t owner = pc_alone_invalid(out, resource, environment, builtin, &classes, &schema, typed,
            custom_reader, (sh_decl_source){(const char *)base, base_length}, have_base, texts,
            error, error_capacity);
        if (owner != SIZE_MAX) { *environment->invalid_package = owner; goto done; }
    }
    pc_report_conflict(out, resource, cause, builtin != NULL, detail, error, error_capacity);
done:
    if (resource->type && (!strncmp(resource->type, "snapeditor", 10) ||
                          !strncmp(resource->type, "snappropertyinspector", 21)))
        sh_package_owners_free(&resource->gameplay_owners);
    if (bodies) for (i = resource->source_count; i > 0; i--) {
        size_t j;
        for (j = 0; j + 1u < i; j++) if (bodies[j] == bodies[i - 1u]) break;
        if (j + 1u == i) free(bodies[i - 1u]);
    }
    free(bodies); free(lengths); free(texts); free(base); pc_classes_free(&classes); return ok;
}

static int pc_resource_compare(const void *a, const void *b)
{
    return strcmp(((const sh_compiled_resource *)a)->engine_path,
                  ((const sh_compiled_resource *)b)->engine_path);
}

/* Stable nodes allow producers to request resources which sort after their
 * output. Only complete candidates/generated results enter the published array.
 * Ordinary installed inputs stay private to this prospective build. */
typedef struct pc_node {
    sh_compiled_resource resource;
    int candidate, required_generated, state, available;
} pc_node;
typedef struct pc_original_frame {
    const char *path;
    struct pc_original_frame *parent;
} pc_original_frame;
typedef struct pc_build {
    sh_package_compilation *out;
    const sh_package_compile_environment *environment;
    sh_package_compile_environment view;
    const pc_input *inputs;
    size_t input_count;
    pc_node **nodes;
    size_t count, capacity;
    pc_original_frame *original_stack;
    char *error;
    size_t error_capacity;
} pc_build;
typedef struct pc_read_view {
    pc_build *build;
    pc_node *target;
    int original, failed;
} pc_read_view;

static void pc_resource_free(sh_compiled_resource *resource);
static int pc_evaluate(pc_build *build, pc_node *node);

static char *pc_key(const sh_package_compile_environment *environment, const char *path)
{
    return environment->canonical_path ? environment->canonical_path(path) : sh_package_engine_path(path);
}

static pc_node *pc_node_get(pc_build *build, const char *path)
{
    char *key = pc_key(build->environment, path);
    size_t low = 0, high = build->count;
    pc_node *node;
    if (!key) {
        snprintf(build->error, build->error_capacity, "invalid resource identity: %s", path ? path : "(null)");
        return NULL;
    }
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(key, build->nodes[middle]->resource.engine_path);
        if (!order) { free(key); return build->nodes[middle]; }
        if (order < 0) high = middle; else low = middle + 1;
    }
    if (build->count == build->capacity) {
        size_t capacity = build->capacity ? build->capacity * 2 : 32;
        pc_node **grown;
        if (capacity < build->capacity || capacity > SIZE_MAX / sizeof(*grown) ||
            !(grown = (pc_node **)realloc(build->nodes, capacity * sizeof(*grown)))) { free(key); return NULL; }
        build->nodes = grown; build->capacity = capacity;
    }
    node = (pc_node *)calloc(1, sizeof(*node));
    if (!node) { free(key); return NULL; }
    node->resource.engine_path = key; node->resource.owns_engine_path = 1;
    node->resource.source = SIZE_MAX;
    memmove(build->nodes + low + 1, build->nodes + low, (build->count - low) * sizeof(*build->nodes));
    build->nodes[low] = node; build->count++; return node;
}

static int pc_producer_read(void *context, const char *path, unsigned char **body, size_t *length);

static int pc_build_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    pc_build *build = (pc_build *)context;
    pc_original_frame frame, *parent;
    pc_read_view view = {build, NULL, 1, 0};
    char *key = pc_key(build->environment, path);
    int scope = -1;
    *body = NULL; *length = 0;
    if (!key) goto done;
    for (parent = build->original_stack; parent; parent = parent->parent)
        if (!strcmp(parent->path, key)) {
            snprintf(build->error, build->error_capacity, "%s: cyclic original producer dependency", key); goto done;
        }
    frame.path = key; frame.parent = build->original_stack; build->original_stack = &frame;
    scope = pc_original((void *)build->environment, key, body, length);
    if (!scope && build->environment->produce) {
        free(*body); *body = NULL; *length = 0;
        scope = build->environment->produce(build->environment->producer_context, key,
            pc_producer_read, &view, body, length);
        if (scope > 0) scope = 3;
    }
    if (view.failed || (scope > 0 && (!*body || *length >= (size_t)PTRDIFF_MAX))) scope = -1;
    build->original_stack = frame.parent;
done:
    free(key);
    if (scope <= 0) { free(*body); *body = NULL; *length = 0; }
    return scope;
}

static int pc_dependency(sh_compiled_resource *target, const sh_compiled_resource *input)
{
    char **grown, *path;
    size_t i;
    for (i = 0; i < target->generated_input_count; i++)
        if (!strcmp(target->generated_inputs[i], input->engine_path)) return 1;
    if (i >= SIZE_MAX / sizeof(*grown) - 1 || !(path = _strdup(input->engine_path))) return 0;
    grown = (char **)realloc(target->generated_inputs, (i + 1) * sizeof(*grown));
    if (!grown) { free(path); return 0; }
    target->generated_inputs = grown; grown[i] = path; target->generated_input_count++;
    return sh_package_owners_union(&target->owners, &input->owners) &&
        sh_package_owners_union(&target->gameplay_owners, &input->gameplay_owners);
}

static int pc_producer_read(void *context, const char *path, unsigned char **body, size_t *length)
{
    pc_read_view *view = (pc_read_view *)context;
    pc_build *build = view->build;
    int result;
    *body = NULL; *length = 0;
    if (view->original) result = pc_build_original(build, path, body, length);
    else {
        pc_node *input = pc_node_get(build, path);
        result = input ? pc_evaluate(build, input) : -1;
        if (result > 0) {
            if (!pc_dependency(&view->target->resource, &input->resource)) result = -1;
            else {
                *body = sh_package_compilation_read(build->out, &input->resource, (size_t)PTRDIFF_MAX,
                    length, build->error, build->error_capacity);
                if (!*body) result = -1;
            }
        }
    }
    if (result < 0) view->failed = 1;
    return result > 0 ? 1 : result;
}

static void pc_generated_error(pc_build *build, pc_node *node)
{
    size_t i, used;
    int first = 1;
    if (!build->error[0]) snprintf(build->error, build->error_capacity,
        "%s: resource producer failed", node->resource.engine_path);
    if (!node->resource.generated_input_count) return;
    for (i = 0; i < build->out->sources->package_count; i++) {
        size_t j;
        int found = 0;
        for (j = 0; j < node->resource.generated_input_count && !found; j++) {
            pc_node *input = pc_node_get(build, node->resource.generated_inputs[j]);
            found = input && sh_package_owners_contains(&input->resource.owners, i);
        }
        if (!found) continue;
        used = strlen(build->error);
        if (used >= build->error_capacity) break;
        snprintf(build->error + used, build->error_capacity - used, "%s%s", first ?
            "; generated inputs from: " : ", ", build->out->sources->packages[i].name);
        first = 0;
    }
}

static int pc_evaluate(pc_build *build, pc_node *node)
{
    sh_compiled_resource *resource = &node->resource;
    unsigned char *generated = NULL;
    size_t length = 0;
    pc_read_view view = {build, node, 0, 0};
    sh_package_builtin contribution;
    int claimed = 0, ok;
    if (node->state == 2) return node->available;
    if (node->state == 1) {
        snprintf(build->error, build->error_capacity, "%s: cyclic effective producer dependency", resource->engine_path);
        return -1;
    }
    node->state = 1;
    if (build->environment->produce)
        claimed = build->environment->produce(build->environment->producer_context,
            resource->engine_path, pc_producer_read, &view, &generated, &length);
    if (claimed < 0 || view.failed || (claimed && (!generated || length >= (size_t)PTRDIFF_MAX))) goto failed;
    if (claimed && pc_builtin(build->environment, resource->engine_path)) {
        snprintf(build->error, build->error_capacity, "%s: multiple product producers claim one identity", resource->engine_path);
        goto failed;
    }
    if (!claimed && node->required_generated) {
        snprintf(build->error, build->error_capacity, "%s: previous generated identity lost its producer", resource->engine_path);
        goto failed;
    }
    resource->generated = claimed > 0;
    if (!claimed && !node->candidate) {
        node->available = pc_build_original(build, resource->engine_path, &resource->body, &resource->body_length);
        if (node->available < 0) goto failed;
        resource->baseline_known = node->available;
        node->available = node->available > 0; node->state = 2; free(generated); return node->available;
    }
    contribution = (sh_package_builtin){resource->engine_path, generated, length};
    ok = pc_compile_resource(build->out, resource, &build->view, claimed ? &contribution : NULL,
        build->inputs, build->input_count,
        build->error, build->error_capacity);
    if (!ok) goto failed;
    node->state = 2; node->available = 1; free(generated); return 1;
failed:
    pc_generated_error(build, node); free(generated); return -1;
}

sh_package_compilation *sh_package_compile_with(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, char *error, size_t error_capacity)
{
    sh_package_compilation *out = NULL;
    pc_input *inputs = NULL;
    size_t count = 0, i, position;
    pc_build build = {0};
    char fallback[1024];
    if (!error || !error_capacity) { error = fallback; error_capacity = sizeof(fallback); }
    error[0] = 0;
    build.error = error; build.error_capacity = error_capacity;
    if (environment && environment->invalid_package) *environment->invalid_package = SIZE_MAX;
    if (!sources || !environment ||
        (environment->builtin_count && !environment->builtins) ||
        (environment->types && !environment->class_derives)) goto bad;
    if ((sources->package_count && !sources->packages) || (sources->file_count && !sources->files) ||
        (sources->component_count && !sources->components)) goto bad;
    for (i = 0; i < sources->component_count; i++)
        if (sources->components[i].owner >= sources->package_count) goto bad;
    for (i = 0; i < sources->file_count; i++)
        if (sources->files[i].owner >= sources->package_count) goto bad;
    for (i = 0; i < environment->builtin_count; i++) {
        const sh_package_builtin *builtin = &environment->builtins[i];
        char *canonical = pc_key(environment, builtin->engine_path);
        size_t j;
        int valid = canonical && builtin->body && builtin->length < (size_t)PTRDIFF_MAX &&
            !strcmp(canonical, builtin->engine_path);
        free(canonical);
        for (j = 0; valid && j < i; j++) if (!strcmp(environment->builtins[j].engine_path, builtin->engine_path)) valid = 0;
        if (!valid) { snprintf(error, error_capacity, "built-in resource identity or payload is invalid"); goto bad; }
    }
    out = (sh_package_compilation *)calloc(1, sizeof(*out));
    inputs = (pc_input *)calloc(sources->file_count ? sources->file_count : 1u, sizeof(*inputs));
    if (!out || !inputs) goto bad;
    out->sources = sources;
    out->canonical_path = environment->canonical_path;
    build.out = out; build.environment = environment; build.view = *environment;
    if (environment->baseline || environment->produce) {
        build.view.baseline = pc_build_original; build.view.baseline_context = &build;
    }
    if (environment->invalid_package) for (i = 0; i < sources->package_count; i++) {
        sh_package_owners owner = {0};
        sh_package_policy policy = {0};
        int allocation = 0;
        if (!sh_package_owners_add(&owner, i)) goto bad;
        /* Policy JSON reports a failed allocation like invalid text; the C
         * runtime allocators record it, and it never isolates a package. */
        errno = 0;
        int valid = pc_policies(sources, &owner, &policy, error, error_capacity, &allocation);
        if (!valid && errno == ENOMEM) allocation = 1;
        sh_package_owners_free(&owner); sh_package_policy_free(&policy);
        if (!valid) {
            if (error[0] && !allocation) *environment->invalid_package = i;
            goto bad;
        }
    }
    if (!pc_policies(sources, NULL, &out->policy, error, error_capacity, NULL)) goto bad;
    for (i = 0; i < sources->file_count; i++) if (sources->files[i].engine_path) {
        inputs[count].file = &sources->files[i]; inputs[count].index = i;
        inputs[count].path = pc_key(environment, sources->files[i].engine_path);
        if (!inputs[count++].path) { snprintf(error, error_capacity, "invalid authored resource identity"); goto bad; }
    }
    qsort(inputs, count, sizeof(*inputs), pc_compare);
    build.inputs = inputs; build.input_count = count;
    for (position = 0; position < count;) {
        size_t end = position + 1u;
        pc_node *node = pc_node_get(&build, inputs[position].path);
        sh_compiled_resource *resource;
        if (!node) goto bad;
        node->candidate = 1; resource = &node->resource;
        while (end < count && !strcmp(inputs[position].path, inputs[end].path)) end++;
        resource->source_count = end - position;
        resource->sources = (size_t *)malloc(resource->source_count * sizeof(*resource->sources));
        if (!resource->sources) goto bad;
        for (i = position; i < end; i++) resource->sources[i - position] = inputs[i].index;
        pc_resolve_variants(sources, resource);
        for (i = 1; i < resource->source_count; i++) {
            size_t j;
            for (j = 0; j < i; j++)
                if (pc_identical(&sources->files[resource->sources[i]], &sources->files[resource->sources[j]])) break;
            out->duplicate_count += j < i;
        }
        position = end;
    }
    for (i = 0; i < environment->builtin_count; i++) {
        const sh_package_builtin *builtin = &environment->builtins[i];
        pc_node *node = pc_node_get(&build, builtin->engine_path);
        if (!node) goto bad;
        node->candidate = 1;
        if (!node->resource.source_count) node->resource.restored_original = 1;
    }
    if (environment->previous) for (i = 0; i < environment->previous->resource_count; i++) {
        const sh_compiled_resource *old = &environment->previous->resources[i];
        pc_node *node;
        if (!old->generated) continue;
        node = pc_node_get(&build, old->engine_path);
        if (!node) goto bad;
        node->candidate = node->required_generated = 1;
    }
    /* Insertions during dependency reads move pointer slots. Terminal states
     * prevent duplicate work as we revisit slots after recursive evaluation. */
    for (i = 0; i < build.count; i++)
        if (build.nodes[i]->candidate && pc_evaluate(&build, build.nodes[i]) < 0) goto bad;
    for (i = 0; i < build.count; i++)
        if (build.nodes[i]->candidate || build.nodes[i]->resource.generated) out->resource_count++;
    out->resources = (sh_compiled_resource *)calloc(out->resource_count ? out->resource_count : 1, sizeof(*out->resources));
    if (!out->resources) { out->resource_count = 0; goto bad; }
    for (i = 0, position = 0; i < build.count; i++) {
        pc_node *node = build.nodes[i];
        if (node->candidate || node->resource.generated) {
            out->resources[position++] = node->resource;
            out->composed_count += node->resource.composed != 0;
            memset(&node->resource, 0, sizeof(node->resource));
        }
        pc_resource_free(&node->resource); free(node);
    }
    free(build.nodes);
    for (i = 0; i < count; i++) free(inputs[i].path);
    free(inputs); return out;
bad:
    if (!error[0]) snprintf(error, error_capacity, "package compiler allocation or input limit failed");
    for (i = 0; i < build.count; i++) { pc_resource_free(&build.nodes[i]->resource); free(build.nodes[i]); }
    free(build.nodes);
    for (i = 0; i < count; i++) free(inputs[i].path);
    free(inputs); sh_package_compilation_free(out); return NULL;
}

sh_package_compilation *sh_package_compile(const sh_package_sources *sources,
    sh_package_baseline_reader baseline, void *context, char *error, size_t error_capacity)
{
    sh_package_compile_environment environment = {0};
    environment.baseline = baseline; environment.baseline_context = context;
    return sh_package_compile_with(sources, &environment, error, error_capacity);
}

static void pc_resource_free(sh_compiled_resource *resource)
{
    size_t i;
    if (resource->restored_original || resource->owns_engine_path) free((void *)resource->engine_path);
    sh_package_owners_free(&resource->owners); sh_package_owners_free(&resource->gameplay_owners);
    free(resource->body); free(resource->sources); free(resource->type);
    free(resource->name); free(resource->cache_path);
    sh_package_file_release(resource->cache_file);
    for (i = 0; i < resource->generated_input_count; i++) free(resource->generated_inputs[i]);
    free(resource->generated_inputs);
}

static int pc_copy_resource(sh_compiled_resource *to, const sh_compiled_resource *from,
    const sh_package_sources *sources, size_t owner_offset, size_t file_offset)
{
    size_t i;
    if (!from->engine_path || from->body_length == SIZE_MAX ||
        from->source_count > SIZE_MAX / sizeof(*to->sources) ||
        from->generated_input_count > SIZE_MAX / sizeof(*to->generated_inputs) ||
        (from->source_count && !from->sources) ||
        (from->generated_input_count && !from->generated_inputs) ||
        !sh_package_owners_within(&from->owners, sources->package_count) ||
        !sh_package_owners_within(&from->gameplay_owners, sources->package_count)) return 0;
    to->engine_path = _strdup(from->engine_path); to->owns_engine_path = 1;
    to->type = from->type ? _strdup(from->type) : NULL;
    to->name = from->name ? _strdup(from->name) : NULL;
    to->cache_path = from->cache_path ? _strdup(from->cache_path) : NULL;
    to->cache_file = sh_package_file_retain(from->cache_file);
    if (!to->engine_path || (from->type && !to->type) || (from->name && !to->name) ||
        (from->cache_path && !to->cache_path)) return 0;
    to->baseline_known = from->baseline_known; to->composed = from->composed;
    to->restored_original = from->restored_original; to->generated = from->generated;
    to->source = SIZE_MAX;
    if (from->source != SIZE_MAX) {
        if (from->source >= sources->file_count) return 0;
        to->source = from->source + file_offset;
    }
    if (from->body) {
        to->body = malloc(from->body_length + 1u);
        if (!to->body) return 0;
        memcpy(to->body, from->body, from->body_length); to->body[from->body_length] = 0;
        to->body_length = from->body_length;
    } else if (from->body_length) return 0;
    if (from->source_count) {
        to->sources = malloc(from->source_count * sizeof(*to->sources));
        if (!to->sources) return 0;
        for (i = 0; i < from->source_count; i++) {
            if (from->sources[i] >= sources->file_count) return 0;
            to->sources[to->source_count++] = from->sources[i] + file_offset;
        }
    }
    for (i = 0; i < sources->package_count; i++) {
        if (sh_package_owners_contains(&from->owners, i) && !sh_package_owners_add(&to->owners, i + owner_offset)) return 0;
        if (sh_package_owners_contains(&from->gameplay_owners, i) &&
            !sh_package_owners_add(&to->gameplay_owners, i + owner_offset)) return 0;
    }
    if (from->generated_input_count) {
        to->generated_inputs = calloc(from->generated_input_count, sizeof(*to->generated_inputs));
        if (!to->generated_inputs) return 0;
        for (i = 0; i < from->generated_input_count; i++) {
            if (!from->generated_inputs[i]) return 0;
            to->generated_inputs[i] = _strdup(from->generated_inputs[i]);
            if (!to->generated_inputs[i]) return 0;
            to->generated_input_count++;
        }
    }
    return 1;
}

sh_package_compilation *sh_package_compilation_overlay(
    const sh_package_compilation *local, const sh_package_compilation *map,
    char *error, size_t capacity)
{
    sh_package_compilation *out = NULL;
    char fallback[1024];
    size_t lower = 0, upper = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!local || !map || !local->sources || !map->sources || local->map_overlay || map->map_overlay ||
        local->canonical_path != map->canonical_path ||
        local->resource_count > SIZE_MAX / sizeof(*out->resources) ||
        map->resource_count > SIZE_MAX / sizeof(*out->resources) - local->resource_count) goto bad;
    out = calloc(1, sizeof(*out));
    if (!out) goto bad;
    out->owns_sources = out->map_overlay = 1;
    out->map_owner_begin = local->sources->package_count;
    out->canonical_path = local->canonical_path;
    out->sources = sh_package_sources_join(local->sources, map->sources, error, capacity);
    if (!out->sources) goto bad;
    size_t count = local->resource_count + map->resource_count;
    out->resources = calloc(count ? count : 1, sizeof(*out->resources));
    if (!out->resources || !pc_view_policies(out, NULL, &out->policy, error, capacity)) goto bad;
    while (lower < local->resource_count || upper < map->resource_count) {
        int order = lower == local->resource_count ? 1 : upper == map->resource_count ? -1 :
            strcmp(local->resources[lower].engine_path, map->resources[upper].engine_path);
        const sh_package_compilation *from;
        const sh_compiled_resource *resource;
        if (!order) {
            const sh_compiled_resource *candidate = &map->resources[upper];
            int authored = candidate->source_count || sh_package_owners_count(&candidate->owners) ||
                sh_package_owners_count(&candidate->gameplay_owners);
            from = authored ? map : local;
            resource = authored ? candidate : &local->resources[lower]; lower++; upper++;
        } else if (order < 0) { from = local; resource = &local->resources[lower++]; }
        else { from = map; resource = &map->resources[upper++]; }
        sh_compiled_resource *copy = &out->resources[out->resource_count++];
        if (!pc_copy_resource(copy, resource, from->sources,
            from == map ? local->sources->package_count : 0,
            from == map ? local->sources->file_count : 0)) goto bad;
        out->composed_count += copy->composed != 0;
        for (size_t i = 1; i < copy->source_count; i++) {
            size_t j;
            for (j = 0; j < i; j++) if (pc_identical(&out->sources->files[copy->sources[i]],
                &out->sources->files[copy->sources[j]])) break;
            out->duplicate_count += j < i;
        }
    }
    return out;
bad:
    if (!error[0]) snprintf(error, capacity, "cannot retain map and local resource providers");
    sh_package_compilation_free(out); return NULL;
}

int sh_package_compilation_restore_missing(sh_package_compilation *current,
    const sh_package_compilation *previous, sh_package_baseline_reader baseline,
    void *context, char *error, size_t error_capacity)
{
    sh_compiled_resource *restored = NULL, *combined;
    size_t count = 0, i, position = 0;
    char fallback[1024];
    int ok = 0;
    if (!error || !error_capacity) { error = fallback; error_capacity = sizeof(fallback); }
    error[0] = 0;
    if (!current) goto done;
    if (!previous || previous == current || !previous->resource_count) return 1;
    restored = (sh_compiled_resource *)calloc(previous->resource_count, sizeof(*restored));
    if (!restored) goto done;
    for (i = 0; i < previous->resource_count; i++) {
        const sh_compiled_resource *old = &previous->resources[i];
        sh_compiled_resource *resource;
        unsigned char *body = NULL;
        size_t length = 0;
        int scope;
        if (!old->type || (old->baseline_known != 1 && old->baseline_known != 3)) continue;
        while (position < current->resource_count &&
               strcmp(current->resources[position].engine_path, old->engine_path) < 0) position++;
        if (position < current->resource_count &&
            !strcmp(current->resources[position].engine_path, old->engine_path)) continue;
        scope = baseline ? baseline(context, old->engine_path, &body, &length) : 0;
        if (scope != old->baseline_known || !body || length == SIZE_MAX || !sh_decl_text_well_formed(body, length)) {
            free(body);
            snprintf(error, error_capacity, "%s: cannot restore verified SnapMap original", old->engine_path);
            goto done;
        }
        resource = &restored[count++];
        resource->restored_original = 1;
        resource->baseline_known = scope;
        resource->source = SIZE_MAX;
        resource->engine_path = _strdup(old->engine_path);
        resource->type = _strdup(old->type); resource->name = _strdup(old->name);
        resource->body = (unsigned char *)malloc(length + 1u);
        if (resource->body) {
            memcpy(resource->body, body, length); resource->body[length] = 0;
            resource->body_length = length;
        }
        free(body);
        if (!resource->engine_path || !resource->type || !resource->name || !resource->body) goto done;
    }
    if (count) {
        if (current->resource_count > SIZE_MAX / sizeof(*combined) - count) goto done;
        combined = (sh_compiled_resource *)realloc(current->resources,
            (current->resource_count + count) * sizeof(*combined));
        if (!combined) goto done;
        current->resources = combined;
        memcpy(combined + current->resource_count, restored, count * sizeof(*restored));
        current->resource_count += count;
        qsort(combined, current->resource_count, sizeof(*combined), pc_resource_compare);
        count = 0; /* Ownership transferred only after every original was read. */
    }
    ok = 1;
done:
    for (i = 0; i < count; i++) pc_resource_free(&restored[i]);
    free(restored);
    if (!ok && !error[0]) snprintf(error, error_capacity, "cannot prepare removed declaration originals");
    return ok;
}

void sh_package_compilation_free(sh_package_compilation *compilation)
{
    size_t i;
    if (!compilation) return;
    for (i = 0; i < compilation->resource_count; i++) {
        pc_resource_free(&compilation->resources[i]);
    }
    sh_package_policy_free(&compilation->policy);
    if (compilation->owns_sources) sh_package_sources_free((sh_package_sources *)compilation->sources);
    free(compilation->resources); free(compilation);
}

const sh_compiled_resource *sh_package_compilation_find(const sh_package_compilation *compilation,
                                                        const char *engine_path)
{
    size_t low = 0, high;
    const sh_compiled_resource *found = NULL;
    char *key = compilation && compilation->canonical_path ? compilation->canonical_path(engine_path) :
        sh_package_engine_path(engine_path);
    if (!compilation || !key) { free(key); return NULL; }
    high = compilation->resource_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        int compare = strcmp(key, compilation->resources[mid].engine_path);
        if (!compare) { found = &compilation->resources[mid]; break; }
        if (compare < 0) high = mid; else low = mid + 1u;
    }
    free(key); return found;
}

unsigned char *sh_package_compilation_read(const sh_package_compilation *compilation,
                                            const sh_compiled_resource *resource,
                                            size_t limit, size_t *length,
                                            char *error, size_t error_capacity)
{
    unsigned char *out;
    if (length) *length = 0;
    if (!compilation || !resource || !length) return NULL;
    if (!resource->body) {
        if (resource->cache_file)
            return sh_package_file_read(resource->cache_file, limit, length, error, error_capacity);
        sh_package_source_file file = compilation->sources->files[resource->source];
        if (resource->cache_path) file.absolute = resource->cache_path;
        return sh_package_source_read(&file, limit, length, error, error_capacity);
    }
    if (resource->body_length > limit) return NULL;
    out = (unsigned char *)malloc(resource->body_length + 1u);
    if (!out) return NULL;
    memcpy(out, resource->body, resource->body_length); out[resource->body_length] = 0;
    *length = resource->body_length; return out;
}

int sh_package_compilation_probe(const sh_package_compilation *compilation,
    const char *engine_path, char *error, size_t capacity)
{
    const sh_compiled_resource *resource;
    sh_package_source_file file;
    FILE *stream;
    char *key, fallback[512];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    key = compilation && compilation->canonical_path ? compilation->canonical_path(engine_path) :
        sh_package_engine_path(engine_path);
    if (!key) { snprintf(error, capacity, "invalid required resource path"); return -1; }
    resource = sh_package_compilation_find(compilation, key); free(key);
    if (!resource) return 0;
    if (resource->body) return 1;
    if (!compilation->sources || resource->source >= compilation->sources->file_count) {
        snprintf(error, capacity, "%s: compiled resource has no readable source", engine_path); return -1;
    }
    file = compilation->sources->files[resource->source];
    if (resource->cache_path) file.absolute = resource->cache_path;
    if (resource->cache_file) {
        uint64_t size;
        stream = sh_package_file_open(resource->cache_file, &size, error, capacity);
    } else stream = sh_package_source_open(&file, error, capacity);
    if (!stream) return -1;
    fclose(stream); return 1;
}

void sh_package_changes_free(sh_package_changes *changes)
{
    if (!changes) return;
    for (size_t i = 0; i < changes->count; i++) {
        free(changes->items[i].path);
        free(changes->items[i].type); free(changes->items[i].name);
    }
    free(changes->items); memset(changes, 0, sizeof(*changes));
}

static const sh_package_source_file *pc_change_source(const sh_package_compilation *compiled,
    const sh_compiled_resource *resource)
{
    if (!compiled->sources || !compiled->sources->files ||
        resource->source >= compiled->sources->file_count) return NULL;
    const sh_package_source_file *file = &compiled->sources->files[resource->source];
    return file->directory ? NULL : file;
}

/* Exact in-memory comparison, captured strong content identities for two
 * opaque snapshots, and bounded streaming only when representations differ. */
static int pc_change_equal(const sh_package_compilation *before, const sh_compiled_resource *a,
    const sh_package_compilation *after, const sh_compiled_resource *b, char *error, size_t capacity)
{
    const sh_package_source_file *af = a->body ? NULL : pc_change_source(before, a);
    const sh_package_source_file *bf = b->body ? NULL : pc_change_source(after, b);
    if ((!a->body && !af) || (!b->body && !bf)) {
        if (error && capacity) snprintf(error, capacity, "%s: invalid compiled source for resource comparison", a->engine_path);
        return -1;
    }
    if ((a->type == NULL) != (b->type == NULL) || (a->name == NULL) != (b->name == NULL) ||
        (a->type && strcmp(a->type, b->type)) || (a->name && strcmp(a->name, b->name))) return 0;
    uint64_t alen = a->body ? a->body_length : af->length;
    uint64_t blen = b->body ? b->body_length : bf->length;
    if (alen != blen) return 0;
    if (a->body && b->body) return !memcmp(a->body, b->body, a->body_length);
    if (af && bf) return !memcmp(af->digest, bf->digest, sizeof(af->digest));
    const sh_compiled_resource *opaque = af ? a : b;
    const sh_compiled_resource *memory = af ? b : a;
    sh_package_source_file file = *(af ? af : bf);
    unsigned char chunk[65536];
    size_t offset = 0;
    int equal = 1;
    if (opaque->cache_path) file.absolute = opaque->cache_path;
    uint64_t opaque_length;
    FILE *stream = opaque->cache_file ? sh_package_file_open(opaque->cache_file, &opaque_length, error, capacity) :
        sh_package_source_open(&file, error, capacity);
    if (!stream) return -1;
    while (offset < memory->body_length) {
        size_t amount = memory->body_length - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (fread(chunk, 1, amount, stream) != amount) {
            if (error && capacity) snprintf(error, capacity, "%s: resource comparison read failed", opaque->engine_path);
            equal = -1; break;
        }
        if (memcmp(memory->body + offset, chunk, amount)) { equal = 0; break; }
        offset += amount;
    }
    fclose(stream); return equal;
}

static int pc_changes_valid(const sh_package_compilation *compiled)
{
    if (!compiled) return 1;
    if (compiled->resource_count && !compiled->resources) return 0;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = &compiled->resources[i];
        if (!r->engine_path || !*r->engine_path ||
            (i && strcmp(compiled->resources[i - 1].engine_path, r->engine_path) >= 0) ||
            (!r->body && !pc_change_source(compiled, r))) return 0;
    }
    return 1;
}

int sh_package_compilation_changes(const sh_package_compilation *before,
    const sh_package_compilation *after, sh_package_changes *out, char *error, size_t capacity)
{
    size_t ai = 0, bi = 0, allocated = 0;
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_changes_free(out);
    if (!pc_changes_valid(before) || !pc_changes_valid(after)) {
        if (error && capacity) snprintf(error, capacity, "invalid or unordered compilation in resource comparison");
        return 0;
    }
    while ((before && ai < before->resource_count) || (after && bi < after->resource_count)) {
        const sh_compiled_resource *a = before && ai < before->resource_count ? &before->resources[ai] : NULL;
        const sh_compiled_resource *b = after && bi < after->resource_count ? &after->resources[bi] : NULL;
        int order = !a ? 1 : !b ? -1 : strcmp(a->engine_path, b->engine_path);
        const char *path = order > 0 ? b->engine_path : a->engine_path;
        sh_package_change_kind kind = order < 0 ? SH_PACKAGE_RESOURCE_REMOVED :
            order > 0 ? SH_PACKAGE_RESOURCE_ADDED : SH_PACKAGE_RESOURCE_REPLACED;
        if (!order) {
            int equal = pc_change_equal(before, a, after, b, error, capacity);
            if (equal < 0) goto failed;
            ai++; bi++;
            if (equal) continue;
        } else if (order < 0) ai++;
        else bi++;
        if (out->count == allocated) {
            size_t next = allocated ? allocated * 2 : 32;
            sh_package_change *grown;
            if (next < allocated || next > SIZE_MAX / sizeof(*grown) ||
                !(grown = realloc(out->items, next * sizeof(*grown)))) goto allocation_failed;
            out->items = grown; allocated = next;
        }
        const sh_compiled_resource *identity = order < 0 ? a : b;
        sh_package_change *change = &out->items[out->count++];
        *change = (sh_package_change){_strdup(path), kind};
        if (identity->type && identity->name) {
            change->type = _strdup(identity->type); change->name = _strdup(identity->name);
            if (!change->type || !change->name) goto allocation_failed;
        }
        if (!change->path) goto allocation_failed;
    }
    return 1;
allocation_failed:
    if (error && capacity) snprintf(error, capacity, "resource comparison allocation failed");
failed:
    sh_package_changes_free(out); return 0;
}

static int pc_required_compare(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int pc_required_add(const sh_package_compilation *map, char ***paths,
    size_t *count, size_t *allocated, const char *path)
{
    char *canonical;
    if (!path) return 0;
    canonical = map->canonical_path ? map->canonical_path(path) : sh_package_engine_path(path);
    if (!canonical || !*canonical) { free(canonical); return 0; }
    if (*count == *allocated) {
        size_t size = *allocated ? *allocated * 2 : 32;
        char **grown;
        if (size < *allocated || size > SIZE_MAX / sizeof(*grown) ||
            !(grown = realloc(*paths, size * sizeof(*grown)))) { free(canonical); return 0; }
        *paths = grown; *allocated = size;
    }
    (*paths)[(*count)++] = canonical; return 1;
}

void sh_package_missing_free(sh_package_missing *missing)
{
    if (!missing) return;
    for (size_t i = 0; i < missing->count; i++) free(missing->paths[i]);
    free(missing->paths); sh_package_owners_free(&missing->packages);
    memset(missing, 0, sizeof(*missing));
}

int sh_package_compilation_payload_missing(const sh_package_compilation *map,
    sh_package_resource_available available, void *context,
    sh_package_missing *out, char *error, size_t capacity)
{
    const char **paths = NULL;
    size_t count = 0;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!map || map->map_overlay || !map->sources || !out || !available ||
        map->resource_count > SIZE_MAX / sizeof(*paths) ||
        (map->resource_count && !(paths = malloc(map->resource_count * sizeof(*paths))))) goto done;
    for (size_t i = 0; i < map->resource_count; i++) {
        const sh_compiled_resource *resource = &map->resources[i];
        if (resource->source_count || sh_package_owners_count(&resource->owners))
            paths[count++] = resource->engine_path;
    }
    ok = sh_package_compilation_missing(map, paths, count, available, context, out, error, capacity);
done:
    free(paths);
    if (!ok) {
        sh_package_missing_free(out);
        if (error && capacity && !error[0]) snprintf(error, capacity, "compiled payload availability could not be checked");
    }
    return ok;
}

int sh_package_compilation_missing(const sh_package_compilation *map,
    const char *const *required_paths, size_t required_count,
    sh_package_resource_available available, void *context,
    sh_package_missing *out, char *error, size_t capacity)
{
    sh_package_missing result = {0};
    sh_package_owners expanded = {0};
    char **paths = NULL, fallback[1024];
    size_t count = 0, allocated = 0;
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!out || !map || map->map_overlay || !map->sources || !available ||
        (required_count && !required_paths) || required_count > SIZE_MAX / sizeof(*paths)) goto done;
    for (size_t i = 0; i < required_count; i++)
        if (!pc_required_add(map, &paths, &count, &allocated, required_paths[i])) goto done;
    /* The compiler already checked producer cycles. Expand each generated
     * resource once; duplicate requests never multiply its input traversal. */
    for (size_t i = 0; i < count; i++) {
        const sh_compiled_resource *resource = sh_package_compilation_find(map, paths[i]);
        if (!resource || !resource->generated) continue;
        size_t index = (size_t)(resource - map->resources);
        if (sh_package_owners_contains(&expanded, index)) continue;
        if (!sh_package_owners_add(&expanded, index)) goto done;
        for (size_t j = 0; j < resource->generated_input_count; j++)
            if (!pc_required_add(map, &paths, &count, &allocated, resource->generated_inputs[j])) goto done;
    }
    if (count > 1) qsort(paths, count, sizeof(*paths), pc_required_compare);
    if (count && !(result.paths = calloc(count, sizeof(*result.paths)))) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_compiled_resource *resource;
        int present;
        if (i && !strcmp(paths[i - 1], paths[i])) continue;
        resource = sh_package_compilation_find(map, paths[i]); result.checked++;
        if (resource && resource->generated) continue;
        present = available(context, paths[i], error, capacity);
        if (present == 1) continue;
        if (present != 0) goto done;
        if (!resource || !resource->source_count) {
            snprintf(error, capacity, "%s: required resource is unavailable and the map carries no supplying package", paths[i]);
            goto done;
        }
        for (size_t j = 0; j < resource->source_count; j++) {
            size_t file = resource->sources[j];
            if (file >= map->sources->file_count || map->sources->files[file].owner >= map->sources->package_count ||
                !sh_package_owners_add(&result.packages, map->sources->files[file].owner)) goto done;
        }
        result.paths[result.count] = _strdup(paths[i]);
        if (!result.paths[result.count]) goto done;
        result.count++;
    }
    ok = 1;
done:
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths); sh_package_owners_free(&expanded);
    /* Inputs may refer to an earlier missing-path result. Retire that result
     * only after all reads, and publish the complete replacement at once. */
    sh_package_missing_free(out);
    if (ok) { *out = result; memset(&result, 0, sizeof(result)); }
    sh_package_missing_free(&result);
    if (!ok && !error[0]) snprintf(error, capacity, "required resource availability could not be checked");
    return ok;
}
