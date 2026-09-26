#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_hud.h"
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

/* These are capability requests, not commands or desired global snapshots.
 * Zero opens an audited blacklist gate; one contributes no request. The native
 * requirements owner restores its captured value when the last zero goes away.
 * Keep this reduction separate from policy values with different semantics. */
static int pc_requirements_merge(sh_json_object *target, const sh_json_object *source,
    char *error, size_t capacity, int *allocation)
{
    static const char *const names[] = {"g_useImageBlackList", "g_useResourceBlackList"};
    static const char *const values[] = {"{}", "{\"g_useImageBlackList\":0}",
        "{\"g_useResourceBlackList\":0}",
        "{\"g_useImageBlackList\":0,\"g_useResourceBlackList\":0}"};
    sh_json_object cvars = {0}, previous = {0};
    const char *raw, *old;
    unsigned requested = 0;
    int ok = 0;
    if (!source->count) return 1;
    raw = sh_json_object_get(source, "cvars");
    if (source->count != 1 || !raw) goto invalid;
    if (!sh_json_parse_object(raw, strlen(raw), 2, &cvars)) {
        sh_json_kind kind;
        if (sh_json_validate(raw, strlen(raw), 2, &kind) && kind == SH_JSON_OBJECT) goto memory;
        goto invalid;
    }
    for (size_t i = 0; i < cvars.count; i++) {
        const sh_json_member *member = cvars.members + i;
        size_t gate;
        if (member->key_length != strlen(member->key)) goto invalid;
        for (gate = 0; gate < 2 && strcmp(member->key, names[gate]); gate++) {}
        if (gate == 2) goto invalid;
        if (!strcmp(member->value_json, "0") || !strcmp(member->value_json, "false"))
            requested |= 1u << gate;
        else if (strcmp(member->value_json, "1") && strcmp(member->value_json, "true")) goto invalid;
    }
    old = sh_json_object_get(target, "cvars");
    if (old) {
        if (!sh_json_parse_object(old, strlen(old), 2, &previous)) goto memory;
        for (size_t gate = 0; gate < 2; gate++)
            if (sh_json_object_get(&previous, names[gate])) requested |= 1u << gate;
    }
    /* Fixed spelling and ordering make the emitted request independent of
     * descriptor member order, component discovery, and boolean spelling. */
    if (!sh_json_object_set(target, "cvars", values[requested], 2)) goto memory;
    ok = 1;
    goto done;
invalid:
    snprintf(error, capacity, "requirements contain an unsupported setting");
    goto done;
memory:
    *allocation = 1;
    snprintf(error, capacity, "could not allocate package requirements");
done:
    sh_json_object_free(&cvars); sh_json_object_free(&previous);
    return ok;
}

static int pc_policies(const sh_package_sources *sources, const sh_package_owners *owners,
                        sh_package_policy *out, char *error, size_t capacity, int *allocation,
                        int independent)
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
        for (other_package = 0; !independent && !shadowed && other_package < sources->package_count; other_package++)
            if (!owners || sh_package_owners_contains(owners, other_package))
                shadowed = pc_variant_shadowed(sources, component->owner, other_package);
        if (shadowed) continue;
        for (section = 0; section < 3; section++) {
            /* Modern asset metadata is bound in its owning source component
             * before library union. A global raw-key merge loses that scope. */
            if (independent && section == 1) continue;
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
            if (!section) ok = pc_requirements_merge(targets[section], &object, error, capacity, allocation);
            else if (independent && section == 2)
                ok = sh_package_hud_merge_provisional(targets[section], &object, error, capacity, allocation);
            else ok = pc_policy_merge(targets[section], &object, names[section], 0, error, capacity, allocation);
            if (!ok && section && !(independent && section == 2) && !*allocation) {
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
    for (size_t i = 0; i < policy->localized_count; i++) {
        free(policy->localized[i].key); free(policy->localized[i].mapping);
        sh_package_owners_free(&policy->localized[i].owners);
    }
    free(policy->localized); sh_package_owners_free(&policy->legacy_string_owners);
    for (size_t i = 0; i < policy->hud_binding_count; i++) {
        free(policy->hud_bindings[i].name); sh_package_owners_free(&policy->hud_bindings[i].owners);
    }
    free(policy->hud_bindings);
    memset(policy, 0, sizeof(*policy));
}

int sh_package_policy_localization(sh_package_policy *policy, const char *key,
    const char *mapping, const sh_package_owners *owners, char *error, size_t capacity)
{
    sh_json_object root = {0}, locales = {0};
    const char *native, *translations;
    sh_package_localized_string value = {0};
    int ok = 0;
    if (!policy || !key || strncmp(key, "#str_", 5) || !key[5] || !mapping) goto done;
    for (size_t i = 0; i < policy->localized_count; i++) if (!_stricmp(policy->localized[i].key, key)) {
        if (strcmp(policy->localized[i].mapping, mapping)) goto done;
        return !owners || sh_package_owners_union(&policy->localized[i].owners, owners);
    }
    if (!sh_json_parse_object(mapping, strlen(mapping), 4, &root) || root.count != 2 ||
        !(native = sh_json_object_get(&root, "native")) ||
        !(translations = sh_json_object_get(&root, "locales")) ||
        !sh_json_parse_object(translations, strlen(translations), 2, &locales) || !locales.count) goto done;
    {
        size_t n = strlen(native), length = 0;
        char *decoded = malloc(n + 1);
        ok = decoded && sh_json_decode_string(native, n, decoded, n + 1, &length) &&
            strlen(decoded) == length && length > 5 && !_strnicmp(decoded, "#str_", 5);
        free(decoded); if (!ok) goto done; ok = 0;
    }
    for (size_t i = 0; i < locales.count; i++) {
        const sh_json_member *locale = locales.members + i;
        sh_json_object pairs = {0};
        const char *previous = sh_json_object_get(&policy->strings, locale->key);
        char *combined = NULL, *decoded = malloc(strlen(locale->value_json) + 1);
        size_t length = 0;
        int valid = locale->key_length && strlen(locale->key) == locale->key_length && decoded &&
            sh_json_decode_string(locale->value_json, strlen(locale->value_json), decoded,
                strlen(locale->value_json) + 1, &length) && strlen(decoded) == length &&
            (!previous || sh_json_parse_object(previous, strlen(previous), 2, &pairs));
        const char *old = valid ? sh_json_object_get(&pairs, key + 5) : NULL;
        valid = valid && (!old || !strcmp(old, locale->value_json)) &&
            sh_json_object_set(&pairs, key + 5, locale->value_json, 1) &&
            (combined = sh_json_serialize_object(&pairs, 0, NULL)) &&
            sh_json_object_set(&policy->strings, locale->key, combined, 2);
        free(decoded); free(combined); sh_json_object_free(&pairs);
        if (!valid) goto done;
    }
    value.key = _strdup(key); value.mapping = _strdup(mapping);
    if (!value.key || !value.mapping || (owners && !sh_package_owners_union(&value.owners, owners)) ||
        policy->localized_count >= SIZE_MAX / sizeof(*policy->localized)) goto done;
    sh_package_localized_string *grown = realloc(policy->localized,
        (policy->localized_count + 1) * sizeof(*grown));
    if (!grown) goto done;
    policy->localized = grown; policy->localized[policy->localized_count++] = value;
    memset(&value, 0, sizeof(value)); ok = 1;
done:
    sh_json_object_free(&root); sh_json_object_free(&locales);
    free(value.key); free(value.mapping); sh_package_owners_free(&value.owners);
    if (!ok && error && capacity) snprintf(error, capacity, "invalid or conflicting bound string mapping");
    return ok;
}

static int pc_localized_copy(sh_package_policy *out, const sh_package_policy *from,
    const sh_package_owners *selected, size_t count, size_t offset, char *error, size_t capacity)
{
    if (!sh_package_owners_within(&from->legacy_string_owners, count)) return 0;
    for (size_t owner = 0; owner < count; owner++)
        if ((!selected || sh_package_owners_contains(selected, owner)) &&
            sh_package_owners_contains(&from->legacy_string_owners, owner) &&
            (owner > SIZE_MAX - offset || !sh_package_owners_add(&out->legacy_string_owners, owner + offset))) return 0;
    for (size_t i = 0; i < from->localized_count; i++) {
        const sh_package_localized_string *value = from->localized + i;
        sh_package_owners owners = {0};
        if (!sh_package_owners_within(&value->owners, count)) return 0;
        for (size_t owner = 0; owner < count; owner++)
            if (sh_package_owners_contains(&value->owners, owner) &&
                (!selected || sh_package_owners_contains(selected, owner)) &&
                (owner > SIZE_MAX - offset || !sh_package_owners_add(&owners, owner + offset))) {
                sh_package_owners_free(&owners); return 0;
            }
        int keep = !selected || sh_package_owners_count(&owners);
        int ok = !keep || sh_package_policy_localization(out, value->key, value->mapping, &owners, error, capacity);
        sh_package_owners_free(&owners); if (!ok) return 0;
    }
    return 1;
}

static int pc_legacy_strings(const sh_package_compilation *compiled, const sh_package_owners *selected,
    sh_package_policy *out, char *error, size_t capacity)
{
    int allocation = 0, ok = 0;
    sh_package_owners owners = {0}; sh_package_policy legacy = {0};
    for (size_t i = 0; i < compiled->sources->package_count; i++)
        if (sh_package_owners_contains(&compiled->policy.legacy_string_owners, i) &&
            (!selected || sh_package_owners_contains(selected, i)) && !sh_package_owners_add(&owners, i)) goto done;
    ok = !sh_package_owners_count(&owners) ||
        (pc_policies(compiled->sources, &owners, &legacy, error, capacity, &allocation, 0) &&
            pc_policy_merge(&out->strings, &legacy.strings, "strings", 0, error, capacity, &allocation));
done:
    sh_package_policy_free(&legacy); sh_package_owners_free(&owners); return ok;
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
    if (!compiled->map_overlay) return pc_policies(compiled->sources, owners, out, error, capacity, NULL,
        compiled->independent_sources) && pc_legacy_strings(compiled, owners, out, error, capacity) &&
        pc_localized_copy(out, &compiled->policy, owners, compiled->sources->package_count, 0, error, capacity) &&
        sh_package_hud_copy(out, &compiled->policy, owners, compiled->sources->package_count, 0) &&
        sh_package_hud_publish(out, 0, 0);
    if (compiled->map_owner_begin > compiled->sources->package_count) goto done;
    for (i = 0; i < compiled->sources->package_count; i++) if (!owners || sh_package_owners_contains(owners, i))
        if (!sh_package_owners_add(i < compiled->map_owner_begin ? &local : &map, i)) goto done;
    if (!pc_policies(compiled->sources, &local, out, error, capacity, NULL, compiled->independent_sources) ||
        !pc_policies(compiled->sources, &map, &upper, error, capacity, NULL, compiled->map_independent_sources)) goto done;
    ok = pc_policy_overlay(&out->requirements, &upper.requirements, 0) &&
        pc_policy_overlay(&out->strings, &upper.strings, 0) &&
        pc_policy_overlay(&out->hud, &upper.hud, 0) &&
        pc_legacy_strings(compiled, owners, out, error, capacity) &&
        pc_localized_copy(out, &compiled->policy, owners, compiled->sources->package_count, 0, error, capacity) &&
        sh_package_hud_copy(out, &compiled->policy, owners, compiled->sources->package_count, 0) &&
        sh_package_hud_publish(out, compiled->map_owner_begin, 1);
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
    /* A bit-flag inspector's choices: one checkbox per value; the list position
     * is presentation (the property holds the checked values, not indices). */
    static const char *const choice_labels[] = {"label", "description", NULL};
    static const sh_decl_collection_rule choices = {"edit.values", "enumValue", NULL, NULL, choice_labels,
        SH_DECL_COLLECTION_PRESENTATION_ORDER};
    static const char *const property_labels[] = {"displayNameTag", "descriptionTag", NULL};
    static const sh_decl_collection_rule editor_collections[] = {
        {"edit.propertySheets", "", "properties", "path", property_labels},
        {"edit.propertySheets.item[*].properties", "path", NULL, NULL, property_labels},
        /* Input actions and output listeners are admitted by declaration
         * identity, not their position in the editor's presentation list. */
        {"edit.inputs", NULL},
        {"edit.outputs", NULL}
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
    if (!_stricmp(type, "snappropertyinspector_bitflag")) { *count = 1; return &choices; }
    if (!_stricmp(type, "snapeditorentitydef")) {
        *count = sizeof(editor_collections) / sizeof(editor_collections[0]); return editor_collections;
    }
    if (!_stricmp(type, "entitydef")) {
        *count = sizeof(conductor) / sizeof(conductor[0]); return conductor;
    }
    return NULL;
}

int sh_package_editor_contribution_phase(const char *type)
{
    if (!type) return 0;
    if (!_stricmp(type, "snappropertyinspector_blockingvolumerendermodel") ||
        !_stricmp(type, "snappropertyinspector_whitelistencounterdecl") ||
        !_stricmp(type, "snappropertyinspector_whitelistencounterdecllist") ||
        !_stricmp(type, "snappropertyinspector_bitflag")) return 1;
    if (!_stricmp(type, "entitydef")) return 2;
    return !_stricmp(type, "snapeditorentitydef") ? 3 : 0;
}

/* Registration lists are entity state fields: rows keyed by a native value
 * (aiType) that each link to a bound declaration. */
static int pc_registration_field(const char *key)
{ return !strcmp(key, "aiTypeList") || !strcmp(key, "globalAIMods"); }

/* The library's registration union (after package keys are assigned): the
 * spawn manager picks among every row sharing a bit with a request, so one
 * type may register several AI and a row is its type and bound AI. Modifiers
 * apply first-match, one row per mask. The whole-file composer keeps plain
 * keys, so an equal key bound differently there remains a conflict. */
static const sh_decl_collection_rule pc_registration_rules[] = {
    {"edit.aiTypeList", "aiType", NULL, NULL, NULL, SH_DECL_COLLECTION_DISTINCT_BEHAVIOR},
    {"edit.globalAIMods", "aiType"}
};

static int pc_editor_field(const char *type, const char *key)
{
    int phase = sh_package_editor_contribution_phase(type);
    if (phase == 2) return !strcmp(key, "editorVars");
    if (phase == 3) return !strcmp(key, "propertySheets") || !strcmp(key, "inputs") || !strcmp(key, "outputs");
    if (phase == 1) return !strcmp(key, !_stricmp(type, "snappropertyinspector_blockingvolumerendermodel") ?
        "renderModelInfoList" : !_stricmp(type, "snappropertyinspector_bitflag") ? "values" : "validEncounters");
    return 0;
}

/* The entity parser reads editorVars in its preamble, before the state reader
 * starts at edit; editorVars after edit becomes part of the edit state, and so
 * of every descendant's (dc_entity_headers). Composed entity editorVars take the
 * place of the originals, or go before edit. Other editor fields keep the tail. */
static sh_decl_node *pc_editor_anchor(sh_decl_node *parent, const char *type)
{
    int found = 0;
    if (sh_package_editor_contribution_phase(type) != 2) return NULL;
    for (sh_decl_node *node = parent->children; node; node = node->next) {
        if (!strcmp(node->key, "edit")) return node;
        if (pc_editor_field(type, node->key)) found = 1;
        else if (found) return node;
    }
    return NULL;
}

/* Split an owned tree without copying native fields or changing their order. */
static sh_decl_node *pc_editor_take(sh_decl_node *parent, const char *type, int registration)
{
    sh_decl_node *out = NULL, **tail = &out, **next = &parent->children;
    while (*next) {
        sh_decl_node *node = *next;
        if (registration ? !pc_registration_field(node->key) : !pc_editor_field(type, node->key)) {
            next = &node->next; continue;
        }
        *next = node->next; node->next = NULL; *tail = node; tail = &node->next;
    }
    return out;
}

static char *pc_editor_projection(const char *type, sh_decl_source text, size_t *length, int *present,
    int keep_inherit, int registration, char *error, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse(text, error, capacity), *parent, *selected = NULL;
    char *out = NULL;
    if (!tree) return NULL;
    parent = !registration && sh_package_editor_contribution_phase(type) == 2 ? tree :
        (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    if (parent) {
        if (!parent->compound) { snprintf(error, capacity, "editor contribution state must be an object"); goto done; }
        selected = pc_editor_take(parent, type, registration);
        if (selected) *present = 1;
    }
    if (parent && parent != tree) {
        sh_decl_node *inherit = keep_inherit ? (sh_decl_node *)sh_decl_tree_member(tree, "inherit") : NULL;
        sh_decl_tree_free(parent->children); parent->children = selected; selected = NULL;
        sh_decl_node **link = &tree->children;
        while (*link) {
            sh_decl_node *node = *link;
            if (node == parent || node == inherit) { *link = node->next; node->next = NULL; }
            else link = &node->next;
        }
        sh_decl_tree_free(tree->children); tree->children = parent;
        if (inherit) { inherit->next = parent; tree->children = inherit; }
    } else {
        sh_decl_tree_free(tree->children); tree->children = selected; selected = NULL;
    }
    out = sh_decl_tree_write(tree, length);
done:
    sh_decl_tree_free(selected); sh_decl_tree_free(tree); return out;
}

/* A missing projected field contributes nothing. An explicitly supplied list
 * still compares against the baseline, including explicit entry removals. This
 * matters for a product default which adds properties but supplies no wiring. */
static char *pc_editor_complete(const char *type, sh_decl_source baseline, sh_decl_source part,
    int registration, size_t *length, char *error, size_t capacity)
{
    sh_decl_node *base = sh_decl_tree_parse(baseline, error, capacity);
    sh_decl_node *tree = sh_decl_tree_parse(part, error, capacity);
    char *out = NULL;
    if (!base || !tree) goto done;
    int entity = !registration && sh_package_editor_contribution_phase(type) == 2;
    sh_decl_node *from = entity ? base : (sh_decl_node *)sh_decl_tree_member(base, "edit");
    sh_decl_node *to = entity ? tree : (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    if (!to && from) {
        tree->children = base->children; base->children = NULL;
    } else if (to && from) {
        sh_decl_node **next = &from->children, **tail = &to->children;
        while (*tail) tail = &(*tail)->next;
        while (*next) {
            sh_decl_node *node = *next;
            if (sh_decl_tree_member(to, node->key)) { next = &node->next; continue; }
            *next = node->next; node->next = NULL; *tail = node; tail = &node->next;
        }
    }
    out = sh_decl_tree_write(tree, length);
done:
    sh_decl_tree_free(base); sh_decl_tree_free(tree); return out;
}

static const char *const pc_editor_labels[] = {"displayNameTag", "descriptionTag", NULL};
static const sh_decl_collection_rule pc_editor_controls[] = {
    {"edit.propertySheets", "", "properties", "path", pc_editor_labels,
        SH_DECL_COLLECTION_PRESENTATION_ORDER | SH_DECL_COLLECTION_DISTINCT_BEHAVIOR},
    {"edit.propertySheets.item[*].properties", "path", NULL, NULL, pc_editor_labels,
        SH_DECL_COLLECTION_PRESENTATION_ORDER | SH_DECL_COLLECTION_DISTINCT_BEHAVIOR},
    {"edit.inputs", NULL, NULL, NULL, NULL, SH_DECL_COLLECTION_PRESENTATION_ORDER},
    {"edit.outputs", NULL, NULL, NULL, NULL, SH_DECL_COLLECTION_PRESENTATION_ORDER}
};
static const sh_decl_collection_rule *pc_editor_rules(const char *type, size_t *count)
{
    int phase = sh_package_editor_contribution_phase(type);
    *count = 0;
    if (phase == 3) { *count = sizeof(pc_editor_controls) / sizeof(*pc_editor_controls); return pc_editor_controls; }
    return phase == 1 ? pc_rules(type, count) : NULL;
}

/* ---- Inherited editor collections -----------------------------------------
 * The native inherited parse copies the parent's state and applies the child's
 * text over it: a list's num resizes it and item[k] merges field by field into
 * slot k. A child's sparse list therefore addresses its parent's controls by
 * position (stock children append this way by the hundred). When the published
 * parent's layout differs from the one the child's author saw, the child is
 * rebased by control identity: the author's parent layout, the child's
 * effective result there and the published parent form a three-way merge,
 * restated as complete lists that the identity composer then unions. */
typedef struct pe_context { int kind; char *error; size_t capacity; } pe_context;
typedef struct pe_list { sh_decl_node **items; size_t count, capacity; } pe_list;
enum { PE_INHERITED = 1, PE_MODIFIED, PE_RESTATED, PE_OWN };
typedef struct pe_origin { int kind; size_t authored; const sh_decl_node *patch; } pe_origin;
#define PE_DEPTH 64u

static int pe_fail(pe_context *p, int kind, const char *reason)
{
    if (!p->kind) {
        p->kind = kind;
        if (p->error && p->capacity) snprintf(p->error, p->capacity, "%s", reason);
    }
    return 0;
}
static int pe_oom(pe_context *p)
{ return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT, "inherited editor collection allocation failed"); }

static sh_decl_node *pe_node(pe_context *p, const char *key, const char *value, int compound)
{
    sh_decl_node *node = calloc(1, sizeof(*node));
    if (!node) { pe_oom(p); return NULL; }
    node->compound = compound; node->assignment = 1;
    if ((key && !(node->key = _strdup(key))) || (value && !(node->value = _strdup(value)))) {
        sh_decl_tree_free(node); pe_oom(p); return NULL;
    }
    return node;
}
static sh_decl_node *pe_clone(pe_context *p, const sh_decl_node *node, unsigned depth)
{
    sh_decl_node *copy, **tail;
    if (depth > PE_DEPTH) { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor state is nested too deeply"); return NULL; }
    if (!(copy = pe_node(p, node->key, node->value, node->compound))) return NULL;
    copy->reset = node->reset; copy->assignment = node->assignment;
    if (node->annotation && !(copy->annotation = _strdup(node->annotation))) { sh_decl_tree_free(copy); pe_oom(p); return NULL; }
    tail = &copy->children;
    for (const sh_decl_node *child = node->children; child; child = child->next) {
        if (!(*tail = pe_clone(p, child, depth + 1))) { sh_decl_tree_free(copy); return NULL; }
        tail = &(*tail)->next;
    }
    return copy;
}
static void pe_append(sh_decl_node *parent, sh_decl_node *child)
{
    sh_decl_node **tail = &parent->children;
    while (*tail) tail = &(*tail)->next;
    *tail = child;
}
static int pe_digits(const char *text, size_t length, size_t *value)
{
    size_t result = 0;
    if (!text || !length || length > 9) return 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        result = result * 10 + (size_t)(text[i] - '0');
    }
    *value = result; return 1;
}
static int pe_index(const char *key, size_t *index)
{
    size_t length = key ? strlen(key) : 0;
    return length > 6 && !strncmp(key, "item[", 5) && key[length - 1] == ']' && pe_digits(key + 5, length - 6, index);
}
static int pe_count(const sh_decl_node *node, size_t *count)
{
    const char *text; size_t length;
    return node && !node->compound && sh_decl_tree_literal(node, &text, &length) && pe_digits(text, length, count);
}
static void pe_list_free(pe_list *list)
{
    for (size_t i = 0; i < list->count; i++) sh_decl_tree_free(list->items[i]);
    free(list->items); memset(list, 0, sizeof(*list));
}
static int pe_insert(pe_context *p, pe_list *list, size_t at, sh_decl_node *node)
{
    if (!node) return 0;
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 8;
        sh_decl_node **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(list->items, next * sizeof(*grown)) : NULL;
        if (!grown) { sh_decl_tree_free(node); return pe_oom(p); }
        list->items = grown; list->capacity = next;
    }
    memmove(list->items + at + 1, list->items + at, (list->count - at) * sizeof(*list->items));
    list->items[at] = node; list->count++; return 1;
}
static int pe_push(pe_context *p, pe_list *list, sh_decl_node *node)
{ return pe_insert(p, list, list->count, node); }

/* Overlay src onto dst as the native inherited parse does: fields merge, a
 * num first removes the slots at or beyond it, values replace. */
static int pe_merge(pe_context *p, sh_decl_node *dst, const sh_decl_node *src, unsigned depth)
{
    if (depth > PE_DEPTH) return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor state is nested too deeply");
    if (dst->compound != src->compound)
        return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor field changes between a value and a block");
    if (!src->compound) {
        char *value = src->value ? _strdup(src->value) : NULL;
        if (src->value && !value) return pe_oom(p);
        free(dst->value); dst->value = value; return 1;
    }
    dst->reset = src->reset; dst->assignment = src->assignment;
    for (const sh_decl_node *child = src->children; child; child = child->next) {
        sh_decl_node *target;
        if (!strcmp(child->key, "num")) {
            size_t count, index;
            if (!pe_count(child, &count)) return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list count is not a number");
            for (sh_decl_node **slot = &dst->children; *slot;) {
                sh_decl_node *item = *slot;
                if (pe_index(item->key, &index) && index >= count) { *slot = item->next; item->next = NULL; sh_decl_tree_free(item); }
                else slot = &item->next;
            }
        }
        target = (sh_decl_node *)sh_decl_tree_member(dst, child->key);
        if (target) { if (!pe_merge(p, target, child, depth + 1)) return 0; continue; }
        if (!(target = pe_clone(p, child, depth + 1))) return 0;
        pe_append(dst, target);
    }
    return 1;
}

/* A native list's complete slots: num of them, or up to its highest index;
 * an absent slot is a default (empty) element. */
static int pe_slots(pe_context *p, const sh_decl_node *list, pe_list *out)
{
    size_t count = 0, index;
    const sh_decl_node *num = list ? sh_decl_tree_member(list, "num") : NULL;
    memset(out, 0, sizeof(*out));
    if (list && !list->compound) return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list is not a block");
    if (num && !pe_count(num, &count)) return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list count is not a number");
    for (const sh_decl_node *child = list ? list->children : NULL; child; child = child->next) {
        if (!strcmp(child->key, "num")) continue;
        if (!pe_index(child->key, &index)) return pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list has a non-slot entry");
        if (!num && index >= count) count = index + 1;
    }
    for (size_t i = 0; i < count; i++) {
        char key[32]; const sh_decl_node *item;
        snprintf(key, sizeof(key), "item[%zu]", i);
        item = list ? sh_decl_tree_member(list, key) : NULL;
        if (!pe_push(p, out, item ? pe_clone(p, item, 1) : pe_node(p, key, NULL, 1))) { pe_list_free(out); return 0; }
    }
    return 1;
}
/* Consume items into a complete list node named key. */
static sh_decl_node *pe_list_node(pe_context *p, const char *key, pe_list *items)
{
    char number[32];
    sh_decl_node *list = pe_node(p, key, NULL, 1), *num;
    snprintf(number, sizeof(number), "%zu", items->count);
    if (!list || !(num = pe_node(p, "num", number, 0))) { sh_decl_tree_free(list); pe_list_free(items); return NULL; }
    pe_append(list, num);
    for (size_t i = 0; i < items->count; i++) {
        char slot[32]; char *renamed;
        snprintf(slot, sizeof(slot), "item[%zu]", i);
        if (!(renamed = _strdup(slot))) { sh_decl_tree_free(list); pe_list_free(items); pe_oom(p); return NULL; }
        free(items->items[i]->key); items->items[i]->key = renamed; items->items[i]->next = NULL;
        pe_append(list, items->items[i]); items->items[i] = NULL;
    }
    items->count = 0; pe_list_free(items); return list;
}

static int pe_literal_equal(const sh_decl_node *a, const sh_decl_node *b)
{
    const char *x, *y; size_t n, m;
    if (!a || !b || !sh_decl_tree_literal(a, &x, &n) || !sh_decl_tree_literal(b, &y, &m)) return 0;
    return n == m && !memcmp(x, y, n);
}
/* A control's identity is its keyed field (a scalar entry's own value);
 * strict matching also compares its inspector binding. */
static int pe_same(const sh_decl_node *a, const sh_decl_node *b, const char *key, int strict)
{
    if (!key || !*key) return !a->compound && !b->compound && pe_literal_equal(a, b);
    if (!pe_literal_equal(sh_decl_tree_member(a, key), sh_decl_tree_member(b, key))) return 0;
    if (!strict) return 1;
    const sh_decl_node *x = sh_decl_tree_member(a, "inspector"), *y = sh_decl_tree_member(b, "inspector");
    return (!x && !y) || pe_literal_equal(x, y);
}
/* Greedy in-order matching pairs the n-th occurrence of an identity with the
 * n-th one, first by strict identity and then by keyed identity alone. */
static int pe_map(pe_context *p, const pe_list *from, const pe_list *to, const char *key, size_t *map)
{
    unsigned char *taken = calloc(to->count ? to->count : 1, 1);
    if (!taken) return pe_oom(p);
    for (size_t i = 0; i < from->count; i++) map[i] = SIZE_MAX;
    for (int strict = 1; strict >= 0; strict--)
        for (size_t i = 0; i < from->count; i++) if (map[i] == SIZE_MAX)
            for (size_t j = 0; j < to->count; j++) if (!taken[j] && pe_same(from->items[i], to->items[j], key, strict)) {
                map[i] = j; taken[j] = 1; break;
            }
    free(taken); return 1;
}
/* Fields of node that differ from base (*out NULL when equal). */
static int pe_diff(pe_context *p, const sh_decl_node *node, const sh_decl_node *base, unsigned depth, sh_decl_node **out)
{
    *out = NULL;
    if (!base || node->compound != base->compound) return (*out = pe_clone(p, node, depth)) != NULL;
    if (!node->compound) {
        if ((!node->value && !base->value) || (node->value && base->value && !strcmp(node->value, base->value))) return 1;
        return (*out = pe_clone(p, node, depth)) != NULL;
    }
    sh_decl_node *result = NULL;
    for (const sh_decl_node *child = node->children; child; child = child->next) {
        sh_decl_node *part;
        if (!pe_diff(p, child, sh_decl_tree_member(base, child->key), depth + 1, &part)) { sh_decl_tree_free(result); return 0; }
        if (!part) continue;
        if (!result && !(result = pe_node(p, node->key, NULL, 1))) { sh_decl_tree_free(part); return 0; }
        pe_append(result, part);
    }
    *out = result; return 1;
}

/* The child's effective list in its author's frame, and where each slot came
 * from: an inherited control unchanged or patched in place, a control the
 * child restated at another slot, or its own. */
static int pe_child(pe_context *p, const pe_list *authored, const sh_decl_node *patch, const char *key,
    pe_list *child, pe_origin **origins)
{
    size_t capacity = authored->count + 8, index;
    unsigned char *present = NULL;
    pe_origin *origin = calloc(capacity, sizeof(*origin));
    memset(child, 0, sizeof(*child)); *origins = NULL;
    if (!origin) return pe_oom(p);
    for (size_t i = 0; i < authored->count; i++) {
        if (!pe_push(p, child, pe_clone(p, authored->items[i], 1))) goto failed;
        origin[i] = (pe_origin){PE_INHERITED, i, NULL};
    }
    for (const sh_decl_node *entry = patch ? patch->children : NULL; entry; entry = entry->next) {
        size_t target;
        if (!strcmp(entry->key, "num")) {
            if (!pe_count(entry, &target)) { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list count is not a number"); goto failed; }
            while (child->count > target) sh_decl_tree_free(child->items[--child->count]);
        } else if (pe_index(entry->key, &index)) target = index + 1;
        else { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list has a non-slot entry"); goto failed; }
        while (child->count < target) {
            if (child->count >= capacity) {
                pe_origin *grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? realloc(origin, capacity * 2 * sizeof(*grown)) : NULL;
                if (!grown) { pe_oom(p); goto failed; }
                origin = grown; capacity *= 2;
            }
            origin[child->count] = (pe_origin){PE_OWN, SIZE_MAX, NULL};
            if (!pe_push(p, child, pe_node(p, "item", NULL, 1))) goto failed;
        }
        if (strcmp(entry->key, "num")) {
            if (entry->compound != child->items[index]->compound) {
                /* A scalar list slot is replaced; a control merges field-wise. */
                sh_decl_node *copy = pe_clone(p, entry, 1);
                if (!copy) goto failed;
                if (child->items[index]->children || child->items[index]->value || entry->compound) {
                    if (!pe_merge(p, child->items[index], entry, 1)) { sh_decl_tree_free(copy); goto failed; }
                    sh_decl_tree_free(copy);
                } else { sh_decl_tree_free(child->items[index]); child->items[index] = copy; }
            } else if (!pe_merge(p, child->items[index], entry, 1)) goto failed;
            if (origin[index].kind == PE_INHERITED) origin[index] = (pe_origin){PE_MODIFIED, origin[index].authored, entry};
        }
    }
    if (!(present = calloc(authored->count ? authored->count : 1, 1))) { pe_oom(p); goto failed; }
    for (size_t k = 0; k < child->count; k++) {
        if (origin[k].kind == PE_MODIFIED && !pe_same(child->items[k], authored->items[origin[k].authored], key, 0))
            origin[k] = (pe_origin){PE_OWN, SIZE_MAX, NULL};
        if (origin[k].kind == PE_INHERITED || origin[k].kind == PE_MODIFIED) present[origin[k].authored] = 1;
    }
    for (size_t k = 0; k < child->count; k++) if (origin[k].kind == PE_OWN)
        for (size_t i = 0; i < authored->count; i++)
            if (!present[i] && pe_same(child->items[k], authored->items[i], key, 0)) {
                origin[k] = (pe_origin){PE_RESTATED, i, NULL}; present[i] = 1; break;
            }
    free(present); *origins = origin; return 1;
failed:
    free(present); free(origin); pe_list_free(child); return 0;
}

/* One effective control for the result list (see pe_rebase_flat). */
static sh_decl_node *pe_result(pe_context *p, const pe_list *authored, const pe_list *published,
    const pe_list *child, const pe_origin *origin, size_t k, const size_t *map)
{
    size_t i = origin[k].authored, j = i < authored->count ? map[i] : SIZE_MAX;
    sh_decl_node *node, *changes = NULL;
    if (origin[k].kind == PE_OWN || j == SIZE_MAX) return pe_clone(p, child->items[k], 1);
    if (!(node = pe_clone(p, published->items[j], 1))) return NULL;
    if (origin[k].kind == PE_MODIFIED && !pe_merge(p, node, origin[k].patch, 1)) { sh_decl_tree_free(node); return NULL; }
    if (origin[k].kind == PE_RESTATED) {
        if (!pe_diff(p, child->items[k], authored->items[i], 1, &changes)) { sh_decl_tree_free(node); return NULL; }
        if (changes && !pe_merge(p, node, changes, 1)) { sh_decl_tree_free(changes); sh_decl_tree_free(node); return NULL; }
        sh_decl_tree_free(changes);
    }
    return node;
}

/* Rebased order keeps every published control at its published position: a
 * native list item merges into the parent's slot of the same index, so a
 * control placed where the parent holds a different one would take on that
 * control's fields. Published controls come first (each with the child's
 * changes, those the child dropped left out), then the child's own controls
 * and its edits of controls the parent's authors removed. Page, label and item
 * position are presentation; the controls and their behavior are preserved. */
typedef struct pe_rebased { sh_decl_node *node; size_t page; } pe_rebased;
static int pe_rebased_push(pe_context *p, pe_rebased **items, size_t *count, size_t *capacity,
    sh_decl_node *node, size_t page)
{
    if (!node) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;
        pe_rebased *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(*items, next * sizeof(*grown)) : NULL;
        if (!grown) { sh_decl_tree_free(node); return pe_oom(p); }
        *items = grown; *capacity = next;
    }
    (*items)[(*count)++] = (pe_rebased){node, page}; return 1;
}
/* Shared by flat lists and pages. child/origin are the child's effective
 * controls in its author's frame; map pairs authored controls with published
 * ones. Published pages of controls come from published_page (NULL: flat);
 * own_page gives the page for each child control appended after them. */
static int pe_rebase_controls(pe_context *p, const pe_list *authored, const pe_list *published,
    const pe_list *child, const pe_origin *origin, const size_t *map, const char *key,
    const size_t *published_page, const size_t *own_page, pe_rebased **out, size_t *out_count)
{
    size_t *slot = calloc(authored->count + 1, sizeof(*slot)), *owner = calloc(published->count + 1, sizeof(*owner));
    unsigned char *consumed = calloc(child->count + 1, 1);
    size_t capacity = 0;
    int ok = 0;
    *out = NULL; *out_count = 0;
    if (!slot || !owner || !consumed) { pe_oom(p); goto done; }
    for (size_t i = 0; i < authored->count; i++) slot[i] = SIZE_MAX;
    for (size_t j = 0; j < published->count; j++) owner[j] = SIZE_MAX;
    for (size_t k = 0; k < child->count; k++) if (origin[k].kind != PE_OWN) slot[origin[k].authored] = k;
    for (size_t i = 0; i < authored->count; i++) if (map[i] != SIZE_MAX) owner[map[i]] = i;
    for (size_t j = 0; j < published->count; j++) {
        size_t page = published_page ? published_page[j] : 0, i = owner[j];
        sh_decl_node *node;
        if (i == SIZE_MAX) {
            /* The parent's own addition; a child control with the same identity
             * is that control too, with the child's fields. */
            if (!(node = pe_clone(p, published->items[j], 1))) goto done;
            for (size_t k = 0; k < child->count; k++)
                if (origin[k].kind == PE_OWN && !consumed[k] && pe_same(child->items[k], published->items[j], key, 1)) {
                    consumed[k] = 1;
                    if (!pe_merge(p, node, child->items[k], 1)) { sh_decl_tree_free(node); goto done; }
                    break;
                }
        } else if (slot[i] == SIZE_MAX) continue; /* the child dropped it */
        else {
            consumed[slot[i]] = 1;
            node = pe_result(p, authored, published, child, origin, slot[i], map);
        }
        if (!pe_rebased_push(p, out, out_count, &capacity, node, page)) goto done;
    }
    for (size_t k = 0; k < child->count; k++) {
        size_t i = origin[k].authored;
        if (consumed[k]) continue;
        if (origin[k].kind == PE_INHERITED) continue; /* removed by the parent's authors */
        if (origin[k].kind != PE_OWN && i < authored->count && map[i] != SIZE_MAX) continue;
        if (!pe_rebased_push(p, out, out_count, &capacity, pe_clone(p, child->items[k], 1), own_page ? own_page[k] : 0)) goto done;
    }
    ok = 1;
done:
    if (!ok) { for (size_t d = 0; d < *out_count; d++) sh_decl_tree_free((*out)[d].node); free(*out); *out = NULL; *out_count = 0; }
    free(slot); free(owner); free(consumed); return ok;
}

/* Flat list (inputs, outputs, one inspector list). */
static int pe_rebase_flat(pe_context *p, const pe_list *authored, const sh_decl_node *patch,
    const pe_list *published, const char *key, pe_list *out)
{
    pe_list child = {0};
    pe_origin *origin = NULL;
    pe_rebased *rebased = NULL;
    size_t *map = NULL, count = 0;
    int ok = 0;
    memset(out, 0, sizeof(*out));
    if (!pe_child(p, authored, patch, key, &child, &origin)) return 0;
    if (!(map = calloc(authored->count + 1, sizeof(*map)))) { pe_oom(p); goto done; }
    if (!pe_map(p, authored, published, key, map) ||
        !pe_rebase_controls(p, authored, published, &child, origin, map, key, NULL, NULL, &rebased, &count)) goto done;
    for (size_t d = 0; d < count; d++) {
        if (!pe_push(p, out, rebased[d].node)) { rebased[d].node = NULL; goto done; }
        rebased[d].node = NULL;
    }
    ok = 1;
done:
    for (size_t d = 0; d < count; d++) sh_decl_tree_free(rebased[d].node);
    if (!ok) pe_list_free(out);
    pe_list_free(&child); free(origin); free(map); free(rebased); return ok;
}

/* Property sheets: pages are positions; a control keeps its identity across
 * pages. Flatten both parent layouts and rebase the controls as above, each on
 * its published page; the child's own controls follow on their own page, and
 * a page the child added follows the published pages. */
typedef struct pe_sheets {
    pe_list fields;      /* each page without its member list */
    pe_list controls;    /* every page's members, in order */
    size_t *page;        /* page index of each control */
    size_t *first;       /* first control of each page */
} pe_sheets;
static void pe_sheets_free(pe_sheets *s)
{ pe_list_free(&s->fields); pe_list_free(&s->controls); free(s->page); free(s->first); memset(s, 0, sizeof(*s)); }
static int pe_strip(pe_context *p, sh_decl_node *page, const char *members)
{
    (void)p;
    for (sh_decl_node **slot = &page->children; *slot;) {
        sh_decl_node *node = *slot;
        if (!strcmp(node->key, members)) { *slot = node->next; node->next = NULL; sh_decl_tree_free(node); }
        else slot = &node->next;
    }
    return 1;
}
static int pe_sheets_open(pe_context *p, const pe_list *pages, const char *members, pe_sheets *out)
{
    memset(out, 0, sizeof(*out));
    if (!(out->first = calloc(pages->count + 1, sizeof(*out->first)))) return pe_oom(p);
    for (size_t s = 0; s < pages->count; s++) {
        pe_list list; sh_decl_node *fields;
        out->first[s] = out->controls.count;
        if (!pe_slots(p, sh_decl_tree_member(pages->items[s], members), &list)) goto failed;
        if (!(fields = pe_clone(p, pages->items[s], 1))) { pe_list_free(&list); goto failed; }
        pe_strip(p, fields, members);
        if (!pe_push(p, &out->fields, fields)) { pe_list_free(&list); goto failed; }
        size_t *grown = realloc(out->page, (out->controls.count + list.count + 1) * sizeof(*grown));
        if (!grown) { pe_list_free(&list); pe_oom(p); goto failed; }
        out->page = grown;
        for (size_t k = 0; k < list.count; k++) {
            out->page[out->controls.count] = s;
            if (!pe_push(p, &out->controls, list.items[k])) { list.items[k] = NULL; pe_list_free(&list); goto failed; }
            list.items[k] = NULL;
        }
        free(list.items);
    }
    out->first[pages->count] = out->controls.count;
    return 1;
failed:
    pe_sheets_free(out); return 0;
}
static int pe_rebase_sheets(pe_context *p, const pe_list *authored_pages, const sh_decl_node *patch,
    const pe_list *published_pages, const char *members, const char *key, pe_list *out)
{
    pe_sheets a = {0}, b = {0};
    pe_list child_pages = {0}, child = {0};
    pe_origin *origin = NULL;
    pe_rebased *rebased = NULL;
    size_t *child_page = NULL, *own_page = NULL, *map = NULL, origin_capacity, rebased_count = 0, pages;
    size_t shared = authored_pages->count < published_pages->count ? authored_pages->count : published_pages->count;
    unsigned char *present = NULL;
    const sh_decl_node **page_patch = NULL;
    int ok = 0;
    memset(out, 0, sizeof(*out));
    if (!pe_sheets_open(p, authored_pages, members, &a) || !pe_sheets_open(p, published_pages, members, &b)) goto done;
    /* The child's pages natively, then each page's members with origins. */
    for (size_t s = 0; s < authored_pages->count; s++)
        if (!pe_push(p, &child_pages, pe_clone(p, authored_pages->items[s], 1))) goto done;
    origin_capacity = a.controls.count + 8;
    if (!(origin = calloc(origin_capacity, sizeof(*origin))) ||
        !(child_page = calloc(origin_capacity, sizeof(*child_page)))) { pe_oom(p); goto done; }
    {
        size_t index, target;
        for (const sh_decl_node *entry = patch ? patch->children : NULL; entry; entry = entry->next) {
            if (!strcmp(entry->key, "num")) {
                if (!pe_count(entry, &target)) { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list count is not a number"); goto done; }
                while (child_pages.count > target) sh_decl_tree_free(child_pages.items[--child_pages.count]);
                while (child_pages.count < target) if (!pe_push(p, &child_pages, pe_node(p, "item", NULL, 1))) goto done;
            } else if (pe_index(entry->key, &index)) {
                while (child_pages.count <= index) if (!pe_push(p, &child_pages, pe_node(p, "item", NULL, 1))) goto done;
            } else { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list has a non-slot entry"); goto done; }
        }
        if (!(page_patch = calloc(child_pages.count + 1, sizeof(*page_patch)))) { pe_oom(p); goto done; }
        for (const sh_decl_node *entry = patch ? patch->children : NULL; entry; entry = entry->next)
            if (pe_index(entry->key, &index) && index < child_pages.count) page_patch[index] = entry;
    }
    for (size_t s = 0; s < child_pages.count; s++) {
        pe_list inherited = {0}, own = {0};
        pe_origin *own_origin = NULL;
        const sh_decl_node *members_patch = page_patch[s] ? sh_decl_tree_member(page_patch[s], members) : NULL;
        size_t base = s < authored_pages->count ? a.first[s] : 0;
        if (page_patch[s] && !page_patch[s]->compound) {
            pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor page is not a block"); goto done;
        }
        if (s < authored_pages->count) for (size_t k = a.first[s]; k < a.first[s + 1]; k++)
            if (!pe_push(p, &inherited, pe_clone(p, a.controls.items[k], 1))) { pe_list_free(&inherited); goto done; }
        int ok_child = pe_child(p, &inherited, members_patch, key, &own, &own_origin);
        pe_list_free(&inherited);
        if (!ok_child) goto done;
        for (size_t k = 0; k < own.count; k++) {
            if (child.count >= origin_capacity) {
                size_t next = origin_capacity * 2;
                pe_origin *grown = realloc(origin, next * sizeof(*grown));
                size_t *grown_page = grown ? realloc(child_page, next * sizeof(*grown_page)) : NULL;
                if (grown) origin = grown;
                if (grown_page) child_page = grown_page;
                if (!grown || !grown_page) { pe_list_free(&own); free(own_origin); pe_oom(p); goto done; }
                origin_capacity = next;
            }
            origin[child.count] = own_origin[k];
            if (own_origin[k].authored != SIZE_MAX) origin[child.count].authored = base + own_origin[k].authored;
            child_page[child.count] = s;
            if (!pe_push(p, &child, own.items[k])) { own.items[k] = NULL; pe_list_free(&own); free(own_origin); goto done; }
            own.items[k] = NULL;
        }
        free(own.items); free(own_origin);
    }
    /* A control restated on another page is still the author's control. */
    if (!(present = calloc(a.controls.count + 1, 1))) { pe_oom(p); goto done; }
    for (size_t k = 0; k < child.count; k++) if (origin[k].kind != PE_OWN) present[origin[k].authored] = 1;
    for (size_t k = 0; k < child.count; k++) if (origin[k].kind == PE_OWN)
        for (size_t i = 0; i < a.controls.count; i++)
            if (!present[i] && pe_same(child.items[k], a.controls.items[i], key, 0)) {
                origin[k] = (pe_origin){PE_RESTATED, i, NULL}; present[i] = 1; break;
            }
    if (!(map = calloc(a.controls.count + 1, sizeof(*map))) || !(own_page = calloc(child.count + 1, sizeof(*own_page)))) {
        pe_oom(p); goto done;
    }
    for (size_t k = 0; k < child.count; k++)
        own_page[k] = child_page[k] < shared ? child_page[k] : published_pages->count + (child_page[k] - shared);
    if (!pe_map(p, &a.controls, &b.controls, key, map) ||
        !pe_rebase_controls(p, &a.controls, &b.controls, &child, origin, map, key, b.page, own_page,
            &rebased, &rebased_count)) goto done;
    pages = published_pages->count;
    for (size_t s = shared; s < child_pages.count; s++)
        if (published_pages->count + (s - shared) + 1 > pages) pages = published_pages->count + (s - shared) + 1;
    for (size_t d = 0; d < rebased_count; d++) if (rebased[d].page + 1 > pages) pages = rebased[d].page + 1;
    for (size_t t = 0; t < pages; t++) {
        pe_list members_list = {0};
        sh_decl_node *page = NULL, *list;
        if (t < published_pages->count) {
            if (!(page = pe_clone(p, b.fields.items[t], 1))) goto done;
            /* The child's page fields (a label) apply to the page it patched. */
            if (t < shared && page_patch[t]) {
                sh_decl_node *fields = pe_clone(p, page_patch[t], 1);
                if (!fields) { sh_decl_tree_free(page); goto done; }
                pe_strip(p, fields, members);
                int merged = pe_merge(p, page, fields, 1); sh_decl_tree_free(fields);
                if (!merged) { sh_decl_tree_free(page); goto done; }
            }
        } else {
            size_t s = t - published_pages->count + shared;
            page = s < child_pages.count ? pe_clone(p, child_pages.items[s], 1) : pe_node(p, "item", NULL, 1);
            if (!page) goto done;
            if (s < child_pages.count && page_patch[s]) {
                sh_decl_node *fields = pe_clone(p, page_patch[s], 1);
                if (!fields || !pe_merge(p, page, fields, 1)) { sh_decl_tree_free(fields); sh_decl_tree_free(page); goto done; }
                sh_decl_tree_free(fields);
            }
            pe_strip(p, page, members);
        }
        for (size_t d = 0; d < rebased_count; d++) if (rebased[d].page == t) {
            if (!pe_push(p, &members_list, rebased[d].node)) { rebased[d].node = NULL; pe_list_free(&members_list); sh_decl_tree_free(page); goto done; }
            rebased[d].node = NULL;
        }
        if (!(list = pe_list_node(p, members, &members_list))) { sh_decl_tree_free(page); goto done; }
        pe_append(page, list);
        if (!pe_push(p, out, page)) goto done;
    }
    ok = 1;
done:
    if (!ok) pe_list_free(out);
    for (size_t d = 0; d < rebased_count; d++) sh_decl_tree_free(rebased[d].node);
    pe_sheets_free(&a); pe_sheets_free(&b); pe_list_free(&child_pages); pe_list_free(&child);
    free(origin); free(child_page); free(own_page); free(map); free(present); free(rebased); free((void *)page_patch);
    return ok;
}

static sh_decl_node *pe_at(sh_decl_node *root, const char *path)
{
    char segment[128];
    sh_decl_node *node = root;
    while (node && *path) {
        const char *dot = strchr(path, '.');
        size_t length = dot ? (size_t)(dot - path) : strlen(path);
        if (length >= sizeof(segment)) return NULL;
        memcpy(segment, path, length); segment[length] = 0;
        node = (sh_decl_node *)sh_decl_tree_member(node, segment);
        path += length + (dot ? 1 : 0);
    }
    return node;
}
/* The effective list at path over a parent chain (nearest parent first). */
static int pe_effective(pe_context *p, sh_decl_node *const *chain, size_t count, const char *path, pe_list *out)
{
    sh_decl_node *list = pe_node(p, "list", NULL, 1);
    int ok = 0;
    if (!list) return 0;
    for (size_t i = count; i > 0; i--) {
        sh_decl_node *layer = pe_at(chain[i - 1], path);
        if (layer && !layer->compound) { pe_fail(p, SH_PACKAGE_EDITOR_FAILURE_SOURCE, "inherited editor list is not a block"); goto done; }
        if (layer && !pe_merge(p, list, layer, 1)) goto done;
    }
    ok = pe_slots(p, list, out);
done:
    sh_decl_tree_free(list); return ok;
}
static int pe_same_layout(const pe_list *a, const pe_list *b, const char *key, const char *members)
{
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        if (!members) { if (!pe_same(a->items[i], b->items[i], key, 1)) return 0; continue; }
        const sh_decl_node *x = sh_decl_tree_member(a->items[i], members), *y = sh_decl_tree_member(b->items[i], members);
        size_t n = 0, m = 0, k;
        for (const sh_decl_node *c = x ? x->children : NULL; c; c = c->next) if (pe_index(c->key, &k) && k + 1 > n) n = k + 1;
        for (const sh_decl_node *c = y ? y->children : NULL; c; c = c->next) if (pe_index(c->key, &k) && k + 1 > m) m = k + 1;
        if (n != m) return 0;
        for (k = 0; k < n; k++) {
            char slot[32]; snprintf(slot, sizeof(slot), "item[%zu]", k);
            const sh_decl_node *l = sh_decl_tree_member(x, slot), *r = sh_decl_tree_member(y, slot);
            if (!l || !r || !pe_same(l, r, key, 1)) return 0;
        }
    }
    return 1;
}

int sh_package_editor_rebase(const char *type, sh_decl_source child,
    const sh_decl_source *authored, size_t authored_count,
    const sh_decl_source *published, size_t published_count, int force,
    char **out, size_t *length, sh_package_editor_failure *failure, char *error, size_t capacity)
{
    pe_context p = {0, error, capacity};
    const sh_decl_collection_rule *rules;
    sh_decl_node *tree = NULL, **a = NULL, **b = NULL;
    size_t rule_count = 0;
    int changed = 0, result = -1;
    if (error && capacity) *error = 0;
    if (failure) { failure->kind = SH_PACKAGE_EDITOR_FAILURE_NONE; failure->source = SIZE_MAX; }
    if (out) *out = NULL;
    if (length) *length = 0;
    rules = pc_editor_rules(type, &rule_count);
    if (!out || !length || (authored_count && !authored) || (published_count && !published)) {
        pe_fail(&p, SH_PACKAGE_EDITOR_FAILURE_COVERAGE, "invalid inherited editor rebase request"); goto done;
    }
    if (!rules || !rule_count) { result = 0; goto done; }
    a = calloc(authored_count + 1, sizeof(*a)); b = calloc(published_count + 1, sizeof(*b));
    if (!a || !b) { pe_oom(&p); goto done; }
    if (!(tree = sh_decl_tree_parse(child, error, capacity))) {
        p.kind = errno == ENOMEM ? SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT : SH_PACKAGE_EDITOR_FAILURE_SOURCE; goto done;
    }
    /* Parents are stock, product or other packages' sources: they never
     * make this contribution malformed. */
    for (size_t i = 0; i < authored_count; i++) if (!(a[i] = sh_decl_tree_parse(authored[i], error, capacity))) {
        p.kind = errno == ENOMEM ? SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT : SH_PACKAGE_EDITOR_FAILURE_COVERAGE; goto done;
    }
    for (size_t i = 0; i < published_count; i++) if (!(b[i] = sh_decl_tree_parse(published[i], error, capacity))) {
        p.kind = errno == ENOMEM ? SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT : SH_PACKAGE_EDITOR_FAILURE_COVERAGE; goto done;
    }
    for (size_t r = 0; r < rule_count; r++) {
        const sh_decl_collection_rule *rule = rules + r;
        sh_decl_node *list = pe_at(tree, rule->path), *replacement;
        pe_list authored_list = {0}, published_list = {0}, rebased = {0};
        const char *key = rule->group_items ? rule->group_identity : rule->item_key;
        int rebase;
        if (strstr(rule->path, "[*]") || !list) continue;
        if (!pe_effective(&p, a, authored_count, rule->path, &authored_list) ||
            !pe_effective(&p, b, published_count, rule->path, &published_list)) {
            /* A parent's own shape is not this contribution's defect. */
            if (p.kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE) p.kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            pe_list_free(&authored_list); pe_list_free(&published_list); goto done;
        }
        if (rule->group_items) {
            pe_sheets x = {0}, y = {0};
            int same = pe_sheets_open(&p, &authored_list, rule->group_items, &x) &&
                pe_sheets_open(&p, &published_list, rule->group_items, &y);
            if (!same && p.kind) { pe_sheets_free(&x); pe_sheets_free(&y); pe_list_free(&authored_list); pe_list_free(&published_list); goto done; }
            same = same && pe_same_layout(&authored_list, &published_list, key, rule->group_items);
            pe_sheets_free(&x); pe_sheets_free(&y);
            rebase = force || !same;
            if (rebase && !pe_rebase_sheets(&p, &authored_list, list, &published_list, rule->group_items, key, &rebased)) {
                pe_list_free(&authored_list); pe_list_free(&published_list); goto done;
            }
        } else {
            rebase = force || !pe_same_layout(&authored_list, &published_list, key, NULL);
            if (rebase && !pe_rebase_flat(&p, &authored_list, list, &published_list, key, &rebased)) {
                pe_list_free(&authored_list); pe_list_free(&published_list); goto done;
            }
        }
        pe_list_free(&authored_list); pe_list_free(&published_list);
        if (!rebase) continue;
        if (!(replacement = pe_list_node(&p, list->key, &rebased))) goto done;
        replacement->assignment = list->assignment;
        /* Swap the restated list into the child's place, keeping field order. */
        replacement->next = list->next; list->next = NULL;
        {
            char parent_path[256]; const char *dot = strrchr(rule->path, '.');
            sh_decl_node *holder = tree;
            if (dot) {
                size_t n = (size_t)(dot - rule->path);
                if (n >= sizeof(parent_path)) { sh_decl_tree_free(replacement); pe_fail(&p, SH_PACKAGE_EDITOR_FAILURE_COVERAGE, "editor collection path is too long"); goto done; }
                memcpy(parent_path, rule->path, n); parent_path[n] = 0; holder = pe_at(tree, parent_path);
            }
            sh_decl_node **slot = &holder->children;
            while (*slot && *slot != list) slot = &(*slot)->next;
            *slot = replacement; sh_decl_tree_free(list);
        }
        changed = 1;
    }
    if (!changed) { result = 0; goto done; }
    if (!(*out = sh_decl_tree_write(tree, length))) { pe_oom(&p); goto done; }
    result = 1;
done:
    if (result < 0 && failure) failure->kind = p.kind ? p.kind : SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
    if (result < 0 && error && capacity && !*error) snprintf(error, capacity, "inherited editor collections could not be rebased");
    sh_decl_tree_free(tree);
    for (size_t i = 0; a && i < authored_count; i++) sh_decl_tree_free(a[i]);
    for (size_t i = 0; b && i < published_count; i++) sh_decl_tree_free(b[i]);
    free(a); free(b); return result;
}

/* Allocation and read failures are transaction failures, as in resource
 * composition; they never become a content decision. */
static int pc_editor_operational(const char *detail)
{ return errno == ENOMEM || (detail && strstr(detail, "allocation")); }

char *sh_package_editor_contributions(const char *type, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, sh_decl_source target, int private_binding,
    size_t *length, char *error, size_t capacity)
{
    return sh_package_editor_contributions_report(type, baseline, sources, count, target, private_binding,
        length, NULL, error, capacity);
}

static char *pc_contributions(const char *type, int registration, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, sh_decl_source target, int private_binding,
    size_t *length, sh_package_editor_failure *failure, char *error, size_t capacity);
char *sh_package_editor_contributions_report(const char *type, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, sh_decl_source target, int private_binding,
    size_t *length, sh_package_editor_failure *failure, char *error, size_t capacity)
{
    return pc_contributions(type, 0, baseline, sources, count, target, private_binding,
        length, failure, error, capacity);
}
char *sh_package_registration_contributions_report(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, sh_decl_source target,
    size_t *length, sh_package_editor_failure *failure, char *error, size_t capacity)
{
    return pc_contributions("entitydef", 1, baseline, sources, count, target, 0,
        length, failure, error, capacity);
}
int sh_package_registration_lists(sh_decl_source text)
{
    sh_decl_node *tree = sh_decl_tree_parse(text, NULL, 0);
    const sh_decl_node *edit = tree ? sh_decl_tree_member(tree, "edit") : NULL;
    int found = edit && (sh_decl_tree_member(edit, "aiTypeList") || sh_decl_tree_member(edit, "globalAIMods"));
    sh_decl_tree_free(tree); return found;
}

static char *pc_contributions(const char *type, int registration, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, sh_decl_source target, int private_binding,
    size_t *length, sh_package_editor_failure *failure, char *error, size_t capacity)
{
    sh_decl_source *parts = NULL;
    char *base = NULL, *merged = NULL, *out = NULL;
    sh_decl_node *tree = NULL, *addition = NULL, *parent, *added;
    const sh_decl_collection_rule *rules;
    size_t base_length = 0, merged_length = 0, rule_count = 0;
    char fallback[1024];
    size_t failed_source = SIZE_MAX;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0; if (length) *length = 0;
    if (failure) { failure->kind = SH_PACKAGE_EDITOR_FAILURE_NONE; failure->source = SIZE_MAX; }
    int phase = sh_package_editor_contribution_phase(type), present = 0;
    if (!phase || !length || (count && !sources) || count > SIZE_MAX / sizeof(*parts)) goto done;
    /* A sparse inherited list patches its parent's entries by index and means
     * nothing without that parent. Keep a parent the contributions share in
     * the projections so composition recognizes such lists; with differing
     * parents only complete lists compose. */
    int keep_inherit = phase != 2 && !registration;
    char *shared_parent = NULL;
    for (size_t i = 0; keep_inherit && i < count; i++) {
        sh_decl_node *tree = sh_decl_tree_parse(sources[i], error, capacity);
        const char *value = NULL; size_t value_length = 0;
        if (!tree) { free(shared_parent); failed_source = i; goto done; }
        if (sh_decl_tree_literal(sh_decl_tree_member(tree, "inherit"), &value, &value_length)) {
            if (!shared_parent) {
                shared_parent = malloc(value_length + 1);
                if (shared_parent) { memcpy(shared_parent, value, value_length); shared_parent[value_length] = 0; }
                else keep_inherit = 0;
            } else if (strlen(shared_parent) != value_length || memcmp(shared_parent, value, value_length)) keep_inherit = 0;
        }
        sh_decl_tree_free(tree);
    }
    free(shared_parent);
    parts = calloc(count ? count : 1, sizeof(*parts));
    base = pc_editor_projection(type, baseline, &base_length, &present, keep_inherit, registration, error, capacity);
    if (!parts || !base) goto done;
    for (size_t i = 0; i < count; i++) {
        parts[i].text = pc_editor_projection(type, sources[i], &parts[i].length, &present, keep_inherit,
            registration, error, capacity);
        if (!parts[i].text) { failed_source = i; goto done; }
        char *completed = pc_editor_complete(type, (sh_decl_source){base, base_length}, parts[i],
            registration, &parts[i].length, error, capacity);
        free((char *)parts[i].text); parts[i].text = completed;
        if (!completed) goto done;
    }
    if (!present) {
        if (target.length == SIZE_MAX || !(out = malloc(target.length + 1))) goto done;
        memcpy(out, target.text, target.length); out[target.length] = 0; *length = target.length; goto done;
    }
    if (registration) {
        rules = pc_registration_rules;
        rule_count = sizeof(pc_registration_rules) / sizeof(*pc_registration_rules);
    } else rules = pc_editor_rules(type, &rule_count);
    if (phase == 2 && !registration) {
        size_t target_length = 0;
        char *projected = pc_editor_projection(type, private_binding ? target : baseline,
            &target_length, &present, 0, 0, error, capacity);
        char *completed = projected ? pc_editor_complete(type, (sh_decl_source){base, base_length},
            (sh_decl_source){projected, target_length}, 0, &target_length, error, capacity) : NULL;
        free(projected);
        if (!completed) goto done;
        merged = sh_decl_compose_editor_variant((sh_decl_source){base, base_length}, parts, count,
            (sh_decl_source){completed, target_length}, &merged_length, error, capacity);
        free(completed);
    } else merged = sh_decl_compose_resource(type, (sh_decl_source){base, base_length},
        parts, count, rules, rule_count, NULL, &merged_length, error, capacity, NULL);
    if (!merged && failure && !pc_editor_operational(error)) {
        /* A contribution that cannot compose even alone with the baseline is
         * invalid in its own context; otherwise their combination is a
         * product composition gap, not an author's defect. */
        for (size_t i = 0; i < count && failed_source == SIZE_MAX; i++) {
            char detail[512] = ""; size_t alone_length = 0;
            sh_decl_conflict cause = {SIZE_MAX, SIZE_MAX, 0, 0};
            char *alone = phase == 2 && !registration ?
                sh_decl_compose_editor_variant((sh_decl_source){base, base_length}, parts + i, 1, parts[i],
                    &alone_length, detail, sizeof(detail)) :
                sh_decl_compose_resource_report(type, (sh_decl_source){base, base_length}, parts + i, 1,
                    rules, rule_count, NULL, &alone_length, detail, sizeof(detail), &cause, NULL);
            if (alone) { free(alone); continue; }
            if (pc_editor_operational(detail)) { snprintf(error, capacity, "%s", detail); break; }
            if ((phase == 2 && !registration) || cause.first == 0) { failed_source = i; snprintf(error, capacity, "%s", detail); }
        }
    }
    if (!merged || !(tree = sh_decl_tree_parse(target, error, capacity)) ||
        !(addition = sh_decl_tree_parse((sh_decl_source){merged, merged_length}, error, capacity))) goto done;
    parent = phase == 2 && !registration ? tree : (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    added = phase == 2 && !registration ? addition : (sh_decl_node *)sh_decl_tree_member(addition, "edit");
    if (!parent && added && added->children) {
        parent = calloc(1, sizeof(*parent));
        if (!parent) goto done;
        parent->key = _strdup("edit"); parent->compound = parent->assignment = 1;
        if (!parent->key) { free(parent); goto done; }
        sh_decl_node **tail = &tree->children;
        while (*tail) tail = &(*tail)->next;
        *tail = parent;
    }
    if (parent) {
        sh_decl_node *anchor = registration ? NULL : pc_editor_anchor(parent, type), **link = &parent->children;
        sh_decl_tree_free(pc_editor_take(parent, type, registration));
        while (*link && *link != anchor) link = &(*link)->next;
        if (added && added->children) {
            sh_decl_node *last = added->children;
            while (last->next) last = last->next;
            last->next = *link; *link = added->children; added->children = NULL;
        }
    }
    out = sh_decl_tree_write(tree, length);
done:
    if (parts) for (size_t i = 0; i < count; i++) free((char *)parts[i].text);
    free(parts); free(base); free(merged); sh_decl_tree_free(tree); sh_decl_tree_free(addition);
    if (!out && !*error) snprintf(error, capacity, "bound editor contribution could not be composed");
    if (!out && failure) {
        failure->kind = pc_editor_operational(error) ? SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT :
            failed_source != SIZE_MAX ? SH_PACKAGE_EDITOR_FAILURE_SOURCE : SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
        failure->source = failure->kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE ? failed_source : SIZE_MAX;
    }
    return out;
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
    int selected = !cause.all_sources && (cause.first < total || cause.second < total), names = 0, n;
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

/* Only used after every owner passed its own policy check. Find all conflicting
 * pairs, including duplicate copies on either side, without blaming packages
 * that merely carry unrelated strings or HUD settings. */
static void pc_conflicted_policies(const sh_package_sources *sources, sh_package_owners *rejected, int independent)
{
    size_t i, j, k;
    if (!rejected) return;
    for (i = 0; i < sources->package_count; i++) {
        if (!independent) {
            for (k = 0; k < sources->package_count; k++) if (pc_variant_shadowed(sources, i, k)) break;
            if (k != sources->package_count) continue;
        }
        for (j = i + 1; j < sources->package_count; j++) {
            sh_package_owners pair = {0};
            sh_package_policy policy = {0};
            char detail[1024] = "";
            int allocation = 0, valid;
            if (!independent) {
                for (k = 0; k < sources->package_count; k++) if (pc_variant_shadowed(sources, j, k)) break;
                if (k != sources->package_count) continue;
            }
            if (!sh_package_owners_add(&pair, i) || !sh_package_owners_add(&pair, j)) {
                sh_package_owners_free(&pair); goto failed;
            }
            errno = 0;
            valid = pc_policies(sources, &pair, &policy, detail, sizeof(detail), &allocation, independent);
            sh_package_policy_free(&policy);
            if (!valid && (allocation || errno == ENOMEM || !detail[0])) {
                sh_package_owners_free(&pair); goto failed;
            }
            if (!valid && !sh_package_owners_union(rejected, &pair)) {
                sh_package_owners_free(&pair); goto failed;
            }
            sh_package_owners_free(&pair);
        }
    }
    return;
failed:
    sh_package_owners_free(rejected);
}

static void pc_conflicted_resource(const sh_package_compile_environment *environment,
    const sh_package_sources *sources, const sh_compiled_resource *resource,
    sh_decl_conflict cause, const unsigned char *participants)
{
    if (!environment->conflicted_packages) return;
    if (cause.precise_sources && participants) {
        for (size_t i = 0; i < resource->source_count; i++) if (participants[i])
            if (!sh_package_owners_add(environment->conflicted_packages, sources->files[resource->sources[i]].owner))
                goto failed;
    } else if (!sh_package_owners_union(environment->conflicted_packages, &resource->owners)) goto failed;
    return;
failed:
    sh_package_owners_free(environment->conflicted_packages);
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
            /* Include later contributors too. Otherwise a third copy of one
             * conflicting payload would become a winner by scan order. */
            if (environment->conflicted_packages) {
                int valid = 1;
                for (size_t j = 0; j < resource->source_count; j++) {
                    const sh_package_source_file *peer = &out->sources->files[resource->sources[j]];
                    sh_package_file_identity identity = {peer->length, {0}};
                    memcpy(identity.digest, peer->digest, sizeof(identity.digest));
                    if (!sh_package_source_verify(peer)) { valid = 0; break; }
                    if ((!original.scope || !pc_file_equal(&original.file, &identity)) &&
                        !sh_package_owners_add(environment->conflicted_packages, peer->owner)) { valid = 0; break; }
                }
                if (!valid) sh_package_owners_free(environment->conflicted_packages);
            }
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
        if ((!rule_count && (!typed || !schema->resolve) && distinct <= 1u) ||
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
        if (classes->peer_source || !(cause.all_sources || cause.first < own || cause.second < own)) continue;
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
    unsigned char *participants = NULL;
    size_t base_length = 0, i, effective = 0, distinct = 0, rule_count = 0;
    size_t input_count = resource->source_count + (builtin != NULL);
    size_t *lengths = NULL;
    sh_decl_source *texts = NULL;
    sh_decl_conflict cause = {SIZE_MAX, SIZE_MAX};
    const sh_decl_collection_rule *rules;
    pc_class_context classes = {0};
    sh_package_source_view source_view = {&classes, pc_source_inputs};
    sh_decl_composition_schema schema = {0};
    int typed = 0, custom_reader = 0, composing = 0, operational = 0;
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
    if (environment->conflicted_packages && !(participants = calloc(input_count, 1))) goto done;
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
    errno = 0;
    rules = pc_rules(resource->type, &rule_count);
    /* Entity headers require normalization even without overlapping changes.
     * Other native families can also accept custom source dialects (particles
     * are one example); preserve their bytes when no composition is needed. */
    int deferred_editor = environment->independent_sources &&
        sh_package_editor_contribution_phase(resource->type) &&
        sh_package_editor_contribution_phase(resource->type) != 2;
    if ((!typed || !schema.resolve || deferred_editor) && (distinct <= 1u || all_equal)) {
        resource->source = effective < resource->source_count ? resource->sources[effective] : SIZE_MAX;
        if (rule_count && !deferred_editor) {
            if (!sh_decl_normalize_collections(texts[effective], rules, rule_count,
                (char **)&resource->body, &resource->body_length, detail, sizeof(detail))) {
                cause.first = effective; cause.second = SIZE_MAX; goto conflict;
            }
            if (resource->body) { ok = 1; goto done; }
        }
        if (resource->type || resource->source == SIZE_MAX) {
            resource->body = (unsigned char *)malloc(texts[effective].length + 1u);
            if (!resource->body) goto done;
            memcpy(resource->body, texts[effective].text, texts[effective].length);
            resource->body[texts[effective].length] = 0;
            resource->body_length = texts[effective].length;
        }
        ok = 1; goto done;
    }
    if (!resource->type || (distinct > 1u && !all_equal && !have_base && !environment->baseline && !builtin)) {
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
    resource->body = (unsigned char *)sh_decl_compose_resource_report(resource->type, have_base ?
        (sh_decl_source){(const char *)base, base_length} : (sh_decl_source){"{}", 2},
        texts, input_count, rules, rule_count, typed ? &schema : NULL,
        &resource->body_length, detail, sizeof(detail), &cause, participants);
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
    /* Source-view read failures and allocation failures are transaction
     * failures. Never turn them into a content-rejection decision. */
    operational = classes.operational || errno == ENOMEM || strstr(detail, "allocation") != NULL;
    if (composing && environment->invalid_package && *environment->invalid_package == SIZE_MAX) {
        size_t owner = pc_alone_invalid(out, resource, environment, builtin, &classes, &schema, typed,
            custom_reader, (sh_decl_source){(const char *)base, base_length}, have_base, texts,
            error, error_capacity);
        if (owner != SIZE_MAX) { *environment->invalid_package = owner; goto done; }
    }
    if (composing && !operational && !classes.operational &&
        (cause.all_sources || cause.first < input_count || cause.second < input_count))
        pc_conflicted_resource(environment, out->sources, resource, cause, participants);
    pc_report_conflict(out, resource, cause, builtin != NULL, detail, error, error_capacity);
done:
    if (ok && (have_base == 1 || have_base == 2) && resource->body)
        resource->native_original = pc_equal(base, base_length, resource->body, resource->body_length);
    if (resource->type && (!strncmp(resource->type, "snapeditor", 10) ||
                          !strncmp(resource->type, "snappropertyinspector", 21)))
        sh_package_owners_free(&resource->gameplay_owners);
    if (bodies) for (i = resource->source_count; i > 0; i--) {
        size_t j;
        for (j = 0; j + 1u < i; j++) if (bodies[j] == bodies[i - 1u]) break;
        if (j + 1u == i) free(bodies[i - 1u]);
    }
    free(bodies); free(lengths); free(texts); free(base); free(participants); pc_classes_free(&classes); return ok;
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
    if (environment && environment->unserved_package) *environment->unserved_package = SIZE_MAX;
    if (environment && environment->conflicted_packages) sh_package_owners_free(environment->conflicted_packages);
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
    out->independent_sources = environment->independent_sources;
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
        int valid = pc_policies(sources, &owner, &policy, error, error_capacity, &allocation,
            environment->independent_sources);
        if (!valid && errno == ENOMEM) allocation = 1;
        sh_package_owners_free(&owner); sh_package_policy_free(&policy);
        if (!valid) {
            if (error[0] && !allocation) *environment->invalid_package = i;
            goto bad;
        }
    }
    {
        int allocation = 0;
        errno = 0;
        if (!pc_policies(sources, NULL, &out->policy, error, error_capacity, &allocation,
            environment->independent_sources)) {
            if (!allocation && errno != ENOMEM && environment->invalid_package)
                pc_conflicted_policies(sources, environment->conflicted_packages, environment->independent_sources);
            goto bad;
        }
    }
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
        if (!environment->independent_sources) pc_resolve_variants(sources, resource);
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

size_t sh_package_resource_native_count(const sh_compiled_resource *resource)
{
    if (!resource) return 0;
    return resource->native_alias_count + (resource->native_type || resource->type ? 1u : 0u);
}
sh_package_native_identity sh_package_resource_native_at(const sh_compiled_resource *resource, size_t index)
{
    if (!resource) return (sh_package_native_identity){0};
    if (resource->native_type || resource->type) {
        if (!index) return resource->native_type ?
            (sh_package_native_identity){resource->native_type, resource->native_name} :
            (sh_package_native_identity){resource->type, resource->name};
        index--;
    }
    return index < resource->native_alias_count && resource->native_aliases ?
        resource->native_aliases[index] : (sh_package_native_identity){0};
}
int sh_package_resource_native_matches(const sh_compiled_resource *resource, const char *type, const char *name)
{
    if (!type || !name) return 0;
    size_t count = sh_package_resource_native_count(resource);
    for (size_t i = 0; i < count; i++) {
        sh_package_native_identity identity = sh_package_resource_native_at(resource, i);
        if (identity.type && identity.name && !_stricmp(identity.type, type) && !_stricmp(identity.name, name)) return 1;
    }
    return 0;
}
int sh_package_resource_native_add(sh_compiled_resource *resource, const char *type, const char *name)
{
    if (!resource || !type || !*type || !name || !*name ||
        (resource->native_alias_count && !resource->native_aliases) ||
        (resource->type && (!resource->name || _stricmp(resource->type, type) || _stricmp(resource->name, name)))) return 0;
    if (sh_package_resource_native_matches(resource, type, name)) return 1;
    char *family = _strdup(type), *key = _strdup(name);
    if (!family || !key) { free(family); free(key); return 0; }
    if (!resource->native_type && !resource->type) {
        resource->native_type = family; resource->native_name = key; return 1;
    }
    if (resource->native_alias_count >= SIZE_MAX / sizeof(*resource->native_aliases)) {
        free(family); free(key); return 0;
    }
    sh_package_native_identity *aliases = realloc(resource->native_aliases,
        (resource->native_alias_count + 1) * sizeof(*aliases));
    if (!aliases) { free(family); free(key); return 0; }
    resource->native_aliases = aliases;
    aliases[resource->native_alias_count++] = (sh_package_native_identity){family, key}; return 1;
}
static int pc_native_valid(const sh_compiled_resource *resource)
{
    if ((resource->native_type == NULL) != (resource->native_name == NULL) ||
        (resource->native_type && (!*resource->native_type || !*resource->native_name)) ||
        (resource->native_type && resource->type && (!resource->name || strcmp(resource->type, resource->native_type) ||
            strcmp(resource->name, resource->native_name))) ||
        resource->native_alias_count > SIZE_MAX / sizeof(*resource->native_aliases) ||
        (resource->native_alias_count && !resource->native_aliases)) return 0;
    size_t count = sh_package_resource_native_count(resource);
    for (size_t i = 0; i < count; i++) {
        sh_package_native_identity a = sh_package_resource_native_at(resource, i);
        if (!a.type || !*a.type || !a.name || !*a.name ||
            (resource->type && (strcmp(resource->type, a.type) || strcmp(resource->name, a.name)))) return 0;
        for (size_t j = 0; j < i; j++) {
            sh_package_native_identity b = sh_package_resource_native_at(resource, j);
            if (!_stricmp(a.type, b.type) && !_stricmp(a.name, b.name)) return 0;
        }
    }
    return 1;
}
static void pc_native_free(sh_compiled_resource *resource)
{
    free(resource->native_type); free(resource->native_name);
    resource->native_type = resource->native_name = NULL;
    for (size_t i = 0; i < resource->native_alias_count; i++) {
        free(resource->native_aliases[i].type); free(resource->native_aliases[i].name);
    }
    free(resource->native_aliases); resource->native_aliases = NULL; resource->native_alias_count = 0;
}
static void pc_resource_free(sh_compiled_resource *resource)
{
    size_t i;
    if (resource->restored_original || resource->owns_engine_path) free((void *)resource->engine_path);
    sh_package_owners_free(&resource->owners); sh_package_owners_free(&resource->gameplay_owners);
    free(resource->body); free(resource->sources); free(resource->type);
    free(resource->name); free(resource->cache_path); free(resource->original_path);
    pc_native_free(resource);
    sh_package_file_release(resource->cache_file);
    for (i = 0; i < resource->generated_input_count; i++) free(resource->generated_inputs[i]);
    free(resource->generated_inputs);
    for (i = 0; i < resource->bound_dependency_count; i++) free(resource->bound_dependencies[i]);
    free(resource->bound_dependencies);
}

static int pc_bound_dependency(sh_compiled_resource *to, const char *path)
{
    if (!path || !*path) return 0;
    for (size_t i = 0; i < to->bound_dependency_count; i++)
        if (!strcmp(to->bound_dependencies[i], path)) return 1;
    if (to->bound_dependency_count == SIZE_MAX / sizeof(*to->bound_dependencies)) return 0;
    char *copy = _strdup(path);
    char **grown = copy ? realloc(to->bound_dependencies,
        (to->bound_dependency_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return 0; }
    to->bound_dependencies = grown; grown[to->bound_dependency_count++] = copy;
    return 1;
}

static int pc_sound_binding_valid(const sh_compiled_resource *resource)
{
    const char *type = resource->native_type ? resource->native_type : resource->type;
    return (!resource->sound_authored_event && !resource->sound_event) ||
        (resource->sound_authored_event && resource->sound_event && type && !strcmp(type, "sound"));
}

static int pc_copy_resource(sh_compiled_resource *to, const sh_compiled_resource *from,
    const sh_package_sources *sources, size_t owner_offset, size_t file_offset)
{
    size_t i;
    if (!from->engine_path || from->body_length == SIZE_MAX || !pc_sound_binding_valid(from) || !pc_native_valid(from) ||
        from->source_count > SIZE_MAX / sizeof(*to->sources) ||
        from->generated_input_count > SIZE_MAX / sizeof(*to->generated_inputs) ||
        from->bound_dependency_count > SIZE_MAX / sizeof(*to->bound_dependencies) ||
        (from->source_count && !from->sources) ||
        (from->generated_input_count && !from->generated_inputs) ||
        (from->bound_dependency_count && !from->bound_dependencies) ||
        !sh_package_owners_within(&from->owners, sources->package_count) ||
        !sh_package_owners_within(&from->gameplay_owners, sources->package_count)) return 0;
    to->engine_path = _strdup(from->engine_path); to->owns_engine_path = 1;
    to->type = from->type ? _strdup(from->type) : NULL;
    to->name = from->name ? _strdup(from->name) : NULL;
    to->native_type = from->native_type ? _strdup(from->native_type) : NULL;
    to->native_name = from->native_name ? _strdup(from->native_name) : NULL;
    to->cache_path = from->cache_path ? _strdup(from->cache_path) : NULL;
    to->original_path = from->original_path ? _strdup(from->original_path) : NULL;
    to->cache_file = sh_package_file_retain(from->cache_file);
    if (!to->engine_path || (from->type && !to->type) || (from->name && !to->name) ||
        (from->cache_path && !to->cache_path) || (from->original_path && !to->original_path) ||
        (from->native_type && (!to->native_type || !to->native_name))) return 0;
    for (i = 0; i < from->native_alias_count; i++)
        if (!sh_package_resource_native_add(to, from->native_aliases[i].type, from->native_aliases[i].name)) return 0;
    to->baseline_known = from->baseline_known; to->composed = from->composed;
    to->native_original = from->native_original;
    to->restored_original = from->restored_original; to->generated = from->generated;
    to->sound_authored_event = from->sound_authored_event; to->sound_event = from->sound_event;
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
    for (i = 0; i < from->bound_dependency_count; i++)
        if (!pc_bound_dependency(to, from->bound_dependencies[i])) return 0;
    return 1;
}

static int pc_source_index_compare(const void *left, const void *right)
{
    size_t a = *(const size_t *)left, b = *(const size_t *)right;
    return a < b ? -1 : a > b;
}

sh_package_compilation *sh_package_compilation_project(const sh_package_sources *sources,
    const sh_package_resource_projection *projections, size_t count,
    const sh_package_policy *policy, char *(*canonical_path)(const char *),
    char *error, size_t capacity)
{
    sh_package_compilation *out = NULL;
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!sources || (count && !projections) || count > SIZE_MAX / sizeof(*out->resources)) goto failed;
    out = calloc(1, sizeof(*out));
    if (!out) goto failed;
    out->sources = sources; out->canonical_path = canonical_path;
    out->resources = calloc(count ? count : 1, sizeof(*out->resources));
    if (!out->resources) goto failed;
    for (size_t i = 0; i < count; i++) {
        const sh_package_resource_projection *projection = &projections[i];
        sh_compiled_resource *resource = &out->resources[out->resource_count++];
        const sh_compiled_resource *first;
        const char *original;
        size_t members = 0, used = 0;
        char *path;
        int invalid = 0;
        if (!projection->input_count || !projection->inputs || !projection->inputs[0] ||
            !projection->engine_path || (projection->length && !projection->body) || projection->length == SIZE_MAX) goto failed;
        first = projection->inputs[0]; original = first->original_path ? first->original_path : first->engine_path;
        if (!original || !pc_copy_resource(resource, first, sources, 0, 0)) goto failed;
        path = canonical_path ? canonical_path(projection->engine_path) : sh_package_engine_path(projection->engine_path);
        if (!path || strcmp(path, projection->engine_path)) { free(path); goto failed; }
        free((void *)resource->engine_path); resource->engine_path = path;
        free(resource->type); free(resource->name); resource->type = resource->name = NULL;
        if (!pc_identity(resource, &invalid, error, capacity)) goto failed;
        pc_native_free(resource);
        if ((projection->native_type == NULL) != (projection->native_name == NULL)) goto failed;
        if (projection->native_type) {
            if (!*projection->native_type || !*projection->native_name ||
                (resource->type && (strcmp(resource->type, projection->native_type) ||
                    strcmp(resource->name, projection->native_name)))) {
                snprintf(error, capacity, "projected native identity disagrees with declaration path: %s", path);
                goto failed;
            }
            resource->native_type = _strdup(projection->native_type);
            resource->native_name = _strdup(projection->native_name);
            if (!resource->native_type || !resource->native_name) goto failed;
        }
        if (strcmp(path, original)) {
            free(resource->original_path); resource->original_path = _strdup(original);
            if (!resource->original_path) goto failed;
            resource->native_original = resource->restored_original = 0;
        }
        for (size_t j = 0; j < projection->input_count; j++) {
            const sh_compiled_resource *input = projection->inputs[j];
            if (!input || !input->engine_path || (input->source_count && !input->sources) ||
                (input->bound_dependency_count && !input->bound_dependencies) ||
                strcmp(input->original_path ? input->original_path : input->engine_path, original) ||
                !pc_sound_binding_valid(input) || !pc_native_valid(input) ||
                input->sound_authored_event != resource->sound_authored_event || input->sound_event != resource->sound_event ||
                !sh_package_owners_within(&input->owners, sources->package_count) ||
                !sh_package_owners_within(&input->gameplay_owners, sources->package_count) ||
                input->source_count > SIZE_MAX / sizeof(*resource->sources) - members) goto failed;
            members += input->source_count;
            if (!sh_package_owners_union(&resource->owners, &input->owners) ||
                (!projection->editor_contributions &&
                    !sh_package_owners_union(&resource->gameplay_owners, &input->gameplay_owners))) goto failed;
            for (size_t k = 0; k < input->bound_dependency_count; k++)
                if (!pc_bound_dependency(resource, input->bound_dependencies[k])) goto failed;
            /* A regenerated body at the same served path still serves every
             * positively captured alias. A renamed projection starts a new
             * binding and must not inherit the old path's logical identities. */
            if (!strcmp(input->engine_path, resource->engine_path))
                for (size_t k = 0; k < sh_package_resource_native_count(input); k++) {
                    sh_package_native_identity identity = sh_package_resource_native_at(input, k);
                    if (!sh_package_resource_native_add(resource, identity.type, identity.name)) goto failed;
                }
        }
        if (!pc_sound_binding_valid(resource)) goto failed;
        free(resource->sources); resource->sources = NULL; resource->source_count = 0;
        if (members && !(resource->sources = malloc(members * sizeof(*resource->sources)))) goto failed;
        for (size_t j = 0; j < projection->input_count; j++) {
            const sh_compiled_resource *input = projection->inputs[j];
            for (size_t k = 0; k < input->source_count; k++) {
                if (input->sources[k] >= sources->file_count) goto failed;
                resource->sources[used++] = input->sources[k];
            }
        }
        if (used > 1) qsort(resource->sources, used, sizeof(*resource->sources), pc_source_index_compare);
        for (size_t j = 0; j < used; j++)
            if (!resource->source_count || resource->sources[resource->source_count - 1] != resource->sources[j])
                resource->sources[resource->source_count++] = resource->sources[j];
        if (projection->body) {
            resource->native_original = 0;
            free(resource->body); resource->body = malloc(projection->length + 1);
            resource->body_length = 0;
            if (!resource->body) goto failed;
            memcpy(resource->body, projection->body, projection->length); resource->body[projection->length] = 0;
            resource->body_length = projection->length;
            sh_package_file_release(resource->cache_file); resource->cache_file = NULL;
            free(resource->cache_path); resource->cache_path = NULL;
        }
        out->composed_count += resource->composed != 0;
        if (projection->input_count - 1 > SIZE_MAX - out->duplicate_count) goto failed;
        out->duplicate_count += projection->input_count - 1;
    }
    if (count > 1) qsort(out->resources, count, sizeof(*out->resources), pc_resource_compare);
    for (size_t i = 1; i < count; i++) if (!strcmp(out->resources[i - 1].engine_path, out->resources[i].engine_path)) {
        snprintf(error, capacity, "linker projected two resources to one engine identity: %s", out->resources[i].engine_path); goto failed;
    }
    if (policy && (!pc_policy_overlay(&out->policy.requirements, &policy->requirements, 0) ||
        !pc_policy_overlay(&out->policy.strings, &policy->strings, 0) ||
        !pc_policy_overlay(&out->policy.hud, &policy->hud, 0) ||
        !pc_localized_copy(&out->policy, policy, NULL, sources->package_count, 0, error, capacity) ||
        !sh_package_hud_copy(&out->policy, policy, NULL, sources->package_count, 0))) goto failed;
    return out;
failed:
    if (!*error) snprintf(error, capacity, "resource binding projection is invalid or could not be allocated");
    sh_package_compilation_free(out); return NULL;
}

typedef struct pc_alias_input { const unsigned char *body; FILE *stream; uint64_t length, offset; } pc_alias_input;
static int pc_alias_input_open(const sh_package_compilation *compiled, const sh_compiled_resource *resource,
    pc_alias_input *out, char *error, size_t capacity)
{
    memset(out, 0, sizeof(*out));
    if (resource->body) { out->body = resource->body; out->length = resource->body_length; return 1; }
    if (resource->cache_file) out->stream = sh_package_file_open(resource->cache_file, &out->length, error, capacity);
    else if (compiled->sources && resource->source < compiled->sources->file_count) {
        sh_package_source_file source = compiled->sources->files[resource->source];
        if (resource->cache_path) source.absolute = resource->cache_path;
        out->length = source.length; out->stream = sh_package_source_open(&source, error, capacity);
    }
    if (out->stream) return 1;
    if (!*error) snprintf(error, capacity, "physical alias source is unavailable: %s", resource->engine_path);
    return 0;
}
static int pc_alias_input_read(pc_alias_input *input, unsigned char *out, size_t length)
{
    if (input->body) memcpy(out, input->body + (size_t)input->offset, length);
    else if (fread(out, 1, length, input->stream) != length) return 0;
    input->offset += length; return 1;
}
static int pc_alias_equal(const sh_package_compilation *a, const sh_compiled_resource *left,
    const sh_package_compilation *b, const sh_compiled_resource *right, char *error, size_t capacity)
{
    pc_alias_input x = {0}, y = {0}; unsigned char first[16384], second[16384]; int result = -1;
    if (!pc_alias_input_open(a, left, &x, error, capacity) || !pc_alias_input_open(b, right, &y, error, capacity)) goto done;
    if (x.length != y.length) { result = 0; goto done; }
    while (x.offset < x.length) {
        uint64_t remaining = x.length - x.offset;
        size_t length = remaining < sizeof(first) ? (size_t)remaining : sizeof(first);
        if (!pc_alias_input_read(&x, first, length) || !pc_alias_input_read(&y, second, length)) goto done;
        if (memcmp(first, second, length)) { result = 0; goto done; }
    }
    if ((x.stream && (fgetc(x.stream) != EOF || ferror(x.stream))) ||
        (y.stream && (fgetc(y.stream) != EOF || ferror(y.stream)))) goto done;
    result = 1;
done:
    if (x.stream) fclose(x.stream);
    if (y.stream) fclose(y.stream);
    if (result < 0 && !*error) snprintf(error, capacity, "physical alias equality could not be verified: %s", left->engine_path);
    return result;
}
static int pc_overlay_aliases(sh_compiled_resource *out, const sh_package_compilation *selected,
    const sh_compiled_resource *chosen, const sh_package_compilation *other,
    const sh_compiled_resource *source, char *error, size_t capacity)
{
    if (!pc_native_valid(source)) return 0;
    size_t count = sh_package_resource_native_count(source); int missing = 0;
    for (size_t i = 0; i < count; i++) {
        sh_package_native_identity identity = sh_package_resource_native_at(source, i);
        if (!sh_package_resource_native_matches(out, identity.type, identity.name)) missing = 1;
    }
    if (!missing) return 1;
    int equal = pc_alias_equal(selected, chosen, other, source, error, capacity);
    if (equal < 0) return 0;
    if (!equal) return 1; /* Different map bytes retain their selected identity set. */
    for (size_t i = 0; i < count; i++) {
        sh_package_native_identity identity = sh_package_resource_native_at(source, i);
        if (!sh_package_resource_native_add(out, identity.type, identity.name)) return 0;
    }
    for (size_t i = 0; i < source->bound_dependency_count; i++)
        if (!pc_bound_dependency(out, source->bound_dependencies[i])) return 0;
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
    out->independent_sources = local->independent_sources;
    out->map_independent_sources = map->independent_sources;
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
        const sh_package_compilation *alias_source = NULL;
        const sh_compiled_resource *alias_resource = NULL;
        if (!order) {
            const sh_compiled_resource *candidate = &map->resources[upper];
            int authored = candidate->source_count || sh_package_owners_count(&candidate->owners) ||
                sh_package_owners_count(&candidate->gameplay_owners);
            from = authored ? map : local;
            resource = authored ? candidate : &local->resources[lower];
            alias_source = authored ? local : map;
            alias_resource = authored ? &local->resources[lower] : candidate;
            lower++; upper++;
        } else if (order < 0) { from = local; resource = &local->resources[lower++]; }
        else { from = map; resource = &map->resources[upper++]; }
        sh_compiled_resource *copy = &out->resources[out->resource_count++];
        if (!pc_copy_resource(copy, resource, from->sources,
            from == map ? local->sources->package_count : 0,
            from == map ? local->sources->file_count : 0)) goto bad;
        if (alias_source && !pc_overlay_aliases(copy, from, resource, alias_source, alias_resource, error, capacity)) goto bad;
        out->composed_count += copy->composed != 0;
        for (size_t i = 1; i < copy->source_count; i++) {
            size_t j;
            for (j = 0; j < i; j++) if (pc_identical(&out->sources->files[copy->sources[i]],
                &out->sources->files[copy->sources[j]])) break;
            out->duplicate_count += j < i;
        }
    }
    if (!pc_localized_copy(&out->policy, &local->policy, NULL, local->sources->package_count, 0, error, capacity) ||
        !pc_localized_copy(&out->policy, &map->policy, NULL, map->sources->package_count,
            local->sources->package_count, error, capacity) ||
        !sh_package_hud_copy(&out->policy, &local->policy, NULL, local->sources->package_count, 0) ||
        !sh_package_hud_copy(&out->policy, &map->policy, NULL, map->sources->package_count, local->sources->package_count) ||
        !sh_package_hud_publish(&out->policy, out->map_owner_begin, 1) ||
        !pc_legacy_strings(out, NULL, &out->policy, error, capacity)) goto bad;
    return out;
bad:
    if (!error[0]) snprintf(error, capacity, "cannot retain map and local resource providers");
    sh_package_compilation_free(out); return NULL;
}

static int pc_native_unchanged(const sh_compiled_resource *resource,
    sh_package_native_served served, void *context)
{
    size_t names = sh_package_resource_native_count(resource);
    if (!resource->native_original || resource->baseline_known != 1 || resource->original_path ||
        resource->restored_original || resource->composed ||
        resource->sound_event != resource->sound_authored_event) return 0;
    if (names <= 1) return 1;
    /* One shipped file can carry several names; a name only the campaign lists
     * must stay published or it becomes unreachable. Publishing an unchanged
     * stock file rebuilds every recorded consumer at activation. */
    if (!served) return 0;
    for (size_t i = 0; i < names; i++) {
        sh_package_native_identity identity = sh_package_resource_native_at(resource, i);
        if (!served(context, resource->engine_path, identity.type, identity.name)) return 0;
    }
    return 1;
}

size_t sh_package_compilation_prune_native(sh_package_compilation *compiled,
    sh_package_native_served served, void *context)
{
    size_t kept = 0, dropped = 0;
    if (!compiled || (compiled->resource_count && !compiled->resources)) return 0;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        if (pc_native_unchanged(compiled->resources + i, served, context)) {
            pc_resource_free(compiled->resources + i); dropped++; continue;
        }
        if (kept != i) compiled->resources[kept] = compiled->resources[i];
        kept++;
    }
    compiled->resource_count = kept;
    if (!dropped) return 0;
    /* A link to a native original resolves natively; nothing here serves it.
     * Bound dependencies hold canonical engine paths of this ordered table. */
    for (size_t i = 0; i < compiled->resource_count; i++) {
        sh_compiled_resource *resource = compiled->resources + i;
        size_t used = 0;
        for (size_t d = 0; d < resource->bound_dependency_count; d++) {
            char *path = resource->bound_dependencies[d];
            size_t low = 0, high = compiled->resource_count;
            int found = 0;
            while (low < high && !found) {
                size_t middle = low + (high - low) / 2;
                int order = strcmp(path, compiled->resources[middle].engine_path);
                if (!order) found = 1; else if (order < 0) high = middle; else low = middle + 1;
            }
            if (found) resource->bound_dependencies[used++] = path;
            else free(path);
        }
        resource->bound_dependency_count = used;
    }
    return dropped;
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
        if (old->original_path || !old->type || (old->baseline_known != 1 && old->baseline_known != 3)) continue;
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

int sh_package_compilation_identity(const sh_package_compilation *compilation,
    const sh_compiled_resource *resource, sh_package_file_identity *out,
    char *error, size_t capacity)
{
    if (out) memset(out, 0, sizeof(*out));
    if (compilation && resource && out) {
        if (resource->body) {
            if (sh_package_bytes_identity(resource->body, resource->body_length, out)) return 1;
        } else if (resource->cache_file) {
            if (sh_package_file_get_identity(resource->cache_file, out)) return 1;
        } else if (compilation->sources && compilation->sources->files &&
                   resource->source < compilation->sources->file_count) {
            const sh_package_source_file *file = compilation->sources->files + resource->source;
            if (!file->directory) {
                out->length = file->length; memcpy(out->digest, file->digest, 32); return 1;
            }
        }
    }
    if (error && capacity) snprintf(error, capacity, "compiled resource has no captured payload identity: %s",
        resource && resource->engine_path ? resource->engine_path : "(null)");
    return 0;
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
        free(changes->items[i].previous_type); free(changes->items[i].previous_name);
    }
    for (size_t i = 0; i < changes->added_count; i++) { free(changes->added[i].type); free(changes->added[i].name); }
    for (size_t i = 0; i < changes->removed_count; i++) { free(changes->removed[i].type); free(changes->removed[i].name); }
    for (size_t i = 0; i < changes->rebound_count; i++) { free(changes->rebound[i].type); free(changes->rebound[i].name); }
    free(changes->rebound);
    free(changes->added); free(changes->removed);
    free(changes->items); memset(changes, 0, sizeof(*changes));
}

void sh_package_changes_reverse(sh_package_changes *changes)
{
    if (!changes) return;
    for (size_t i = 0; i < changes->count; i++) {
        sh_package_change *change = changes->items + i;
        if (change->kind == SH_PACKAGE_RESOURCE_ADDED) change->kind = SH_PACKAGE_RESOURCE_REMOVED;
        else if (change->kind == SH_PACKAGE_RESOURCE_REMOVED) change->kind = SH_PACKAGE_RESOURCE_ADDED;
        else if (changes->identities_complete) {
            char *type = change->type, *name = change->name;
            change->type = change->previous_type; change->name = change->previous_name;
            change->previous_type = type; change->previous_name = name;
        }
    }
    sh_package_native_identity *set = changes->added; size_t count = changes->added_count;
    changes->added = changes->removed; changes->added_count = changes->removed_count;
    changes->removed = set; changes->removed_count = count;
}

static sh_package_native_identity pc_native_identity(const sh_compiled_resource *resource)
{
    return resource->native_type ? (sh_package_native_identity){resource->native_type, resource->native_name} :
        (sh_package_native_identity){resource->type, resource->name};
}
static int pc_native_compare(const void *a, const void *b)
{
    const sh_package_native_identity *x = a, *y = b;
    int order = strcmp(x->type, y->type);
    return order ? order : strcmp(x->name, y->name);
}
static int pc_native_copy(sh_package_native_identity from, char **type, char **name)
{
    *type = from.type ? _strdup(from.type) : NULL;
    *name = from.name ? _strdup(from.name) : NULL;
    return !from.type || (*type && *name);
}
static sh_package_native_identity *pc_native_set(const sh_package_compilation *compiled, size_t *count)
{
    size_t capacity = 0;
    sh_package_native_identity *set;
    *count = 0;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        size_t n = sh_package_resource_native_count(compiled->resources + i);
        if (n > SIZE_MAX - capacity) return NULL;
        capacity += n;
    }
    if (capacity > SIZE_MAX / sizeof(*set) || !(set = calloc(capacity ? capacity : 1, sizeof(*set)))) return NULL;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++)
        for (size_t j = 0; j < sh_package_resource_native_count(compiled->resources + i); j++)
            set[(*count)++] = sh_package_resource_native_at(compiled->resources + i, j);
    if (*count > 1) qsort(set, *count, sizeof(*set), pc_native_compare);
    size_t used = 0;
    for (size_t i = 0; i < *count; i++)
        if (!used || pc_native_compare(set + used - 1, set + i)) set[used++] = set[i];
    *count = used; return set;
}
static int pc_native_difference(const sh_package_native_identity *from, size_t count,
    const sh_package_native_identity *to, size_t to_count,
    sh_package_native_identity **out, size_t *out_count)
{
    size_t selected = 0;
    for (size_t i = 0; i < count; i++) if (!bsearch(from + i, to, to_count, sizeof(*to), pc_native_compare)) selected++;
    if (selected > SIZE_MAX / sizeof(**out) || !(*out = calloc(selected ? selected : 1, sizeof(**out)))) return 0;
    for (size_t i = 0; i < count; i++) if (!bsearch(from + i, to, to_count, sizeof(*to), pc_native_compare)) {
        sh_package_native_identity *identity = *out + (*out_count)++;
        if (!pc_native_copy(from[i], &identity->type, &identity->name)) return 0;
    }
    return 1;
}
static int pc_native_changes(const sh_package_compilation *before, const sh_package_compilation *after,
    sh_package_changes *out)
{
    size_t a_count = 0, b_count = 0;
    sh_package_native_identity *a = pc_native_set(before, &a_count), *b = pc_native_set(after, &b_count);
    int ok = a && b && pc_native_difference(a, a_count, b, b_count, &out->removed, &out->removed_count) &&
        pc_native_difference(b, b_count, a, a_count, &out->added, &out->added_count);
    free(a); free(b); out->identities_complete = ok; return ok;
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
    /* Logical identity additions/removals are recorded separately. Choosing a
     * different primary alias cannot turn unchanged physical bytes into a
     * replacement that reconstructs existing native consumers. */
    if (a->sound_authored_event != b->sound_authored_event || a->sound_event != b->sound_event) return 0;
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
    if ((compiled->resource_count && !compiled->resources) ||
        compiled->resource_count > SIZE_MAX / sizeof(*compiled->resources)) return 0;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = &compiled->resources[i];
        if (!r->engine_path || !*r->engine_path || !pc_sound_binding_valid(r) || !pc_native_valid(r) ||
            (r->type == NULL) != (r->name == NULL) || (r->type && (!*r->type || !*r->name)) ||
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
    if (!pc_native_changes(before, after, out)) goto allocation_failed;
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
        if (!pc_native_copy(pc_native_identity(identity), &change->type, &change->name)) goto allocation_failed;
        if (!order && !pc_native_copy(pc_native_identity(a), &change->previous_type, &change->previous_name))
            goto allocation_failed;
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
        const char *original;
        int present;
        if (i && !strcmp(paths[i - 1], paths[i])) continue;
        resource = sh_package_compilation_find(map, paths[i]); result.checked++;
        if (resource && resource->generated) continue;
        original = resource && resource->original_path ? resource->original_path : paths[i];
        if (resource && resource->original_path) {
            char *canonical = map->canonical_path ? map->canonical_path(original) : sh_package_engine_path(original);
            int valid = canonical && !strcmp(canonical, original);
            free(canonical); if (!valid) goto done;
        }
        present = available(context, original, error, capacity);
        if (present == 1) continue;
        if (present != 0) goto done;
        if (!resource || !resource->source_count) {
            snprintf(error, capacity, "%s: required resource is unavailable and the map carries no supplying package", original);
            goto done;
        }
        for (size_t j = 0; j < resource->source_count; j++) {
            size_t file = resource->sources[j];
            if (file >= map->sources->file_count || map->sources->files[file].owner >= map->sources->package_count ||
                !sh_package_owners_add(&result.packages, map->sources->files[file].owner)) goto done;
        }
        result.paths[result.count] = _strdup(original);
        if (!result.paths[result.count]) goto done;
        result.count++;
    }
    if (result.count > 1) {
        size_t kept = 0;
        qsort(result.paths, result.count, sizeof(*result.paths), pc_required_compare);
        for (size_t i = 0; i < result.count; i++) {
            if (kept && !strcmp(result.paths[kept - 1], result.paths[i])) free(result.paths[i]);
            else result.paths[kept++] = result.paths[i];
        }
        result.count = kept;
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
