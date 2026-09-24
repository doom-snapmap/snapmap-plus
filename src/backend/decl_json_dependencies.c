#include "decl_dependencies.h"
#include "config_json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dj_walk {
    const sh_decl_dependency_schema *schema;
    sh_decl_dependency_result *result;
} dj_walk;

static int dj_gap(dj_walk *walk, const char *path, sh_decl_value_type type, const char *reason)
{
    walk->result->gaps++;
    if (walk->schema->gap) walk->schema->gap(walk->schema->context, path, type, reason);
    return 1;
}

static char *dj_path(const char *parent, const char *key)
{
    size_t a = strlen(parent), b = strlen(key);
    char *out;
    if (a > SIZE_MAX - 2 || b > SIZE_MAX - a - 2 ||
        !(out = (char *)malloc(a + b + 2))) return NULL;
    memcpy(out, parent, a); out[a] = '.'; memcpy(out + a + 1, key, b + 1);
    return out;
}

static int dj_value(dj_walk *walk, const char *json, size_t length,
                    sh_decl_value_type type, const char *path, unsigned depth);

typedef struct dj_object {
    dj_walk *walk;
    sh_decl_value_type type;
    const char *path;
    unsigned depth;
} dj_object;

/* Visit only the current object here; typed recursion selects each child.
 * Keeping source spans avoids copied member strings losing their locations. */
static int dj_shallow(void *context, const char *key, size_t length, unsigned depth)
{
    (void)context; (void)key; (void)length; (void)depth;
    return 0;
}

static int dj_fields(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_object *object = context;
    dj_walk *walk = object->walk;
    size_t i;
    (void)depth;
    for (i = 0; i < count; i++) {
        const sh_json_field_span *member = &fields[i];
        sh_decl_value_type field = {0};
        char *child_path;
        int found, ok;
        if (!strcmp(member->key, "~type") || !strcmp(member->key, "~version") ||
            !strcmp(member->key, "editorVars")) continue;
        found = walk->schema->field(walk->schema->context, object->type, member->key, &field);
        if (!found) continue;
        child_path = dj_path(object->path, member->key);
        if (!child_path) return 0;
        if (found < 0 || !field.name)
            ok = dj_gap(walk, child_path, object->type, "field semantics are unavailable");
        else ok = dj_value(walk, member->value, member->value_length, field, child_path, object->depth + 1);
        free(child_path);
        if (!ok) return 0;
    }
    return 1;
}

static int dj_script_type(dj_object *object, const char *json, size_t length,
    int block_selector, sh_decl_value_type *selected)
{
    char *name;
    size_t decoded = 0;
    int ok;
    *selected = (sh_decl_value_type){0};
    if (length == 4 && !memcmp(json, "null", 4)) return 1;
    if (length == SIZE_MAX || !(name = malloc(length + 1))) return 0;
    ok = sh_native_json_decode_string(json, length, name, length + 1, &decoded) && strlen(name) == decoded;
    if (ok && block_selector && !strcmp(name, "NULL")) { free(name); return 1; }
    ok = ok && decoded && object->walk->schema->dynamic_type &&
        object->walk->schema->dynamic_type(object->walk->schema->context, object->type,
            name, decoded, selected) == 1 && selected->name;
    free(name);
    if (!ok) {
        *selected = (sh_decl_value_type){0};
        dj_gap(object->walk, object->path, object->type, "script class metadata is unavailable");
    }
    return 1;
}
static int dj_script_fields(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_object *object = context, selected = *object;
    size_t i = 0;
    while (i < count && (!strcmp(fields[i].key, "~type") || !strcmp(fields[i].key, "~version"))) i++;
    if (i == count) return 1;
    if (strcmp(fields[i].key, "object"))
        return dj_gap(object->walk, object->path, object->type, "script class selector must precede its fields");
    if (!dj_script_type(object, fields[i].value, fields[i].value_length, 1, &selected.type)) return 0;
    if (!selected.type.name) return 1;
    return dj_fields(&selected, fields + i + 1, count - i - 1, depth);
}

/* The complete input was validated before any callbacks. This only splits
 * already valid array elements, respecting nested values and quoted commas. */
static int dj_array(dj_walk *walk, const char *json, size_t length,
                    const sh_decl_value_shape *shape, sh_decl_value_type type,
                    const char *path, unsigned depth)
{
    size_t i = 1, start = 1, index = 0;
    unsigned nested = 0;
    int quoted = 0, escaped = 0;
    if (length < 2 || json[0] != '[' || json[length - 1] != ']' || !shape->element.name)
        return dj_gap(walk, path, type, "collection JSON requires a verified array reader");
    for (; i < length; i++) {
        char c = json[i];
        if (quoted) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
        } else if (c == '"') quoted = 1;
        else if (c == '{' || c == '[') nested++;
        else if (nested && (c == '}' || c == ']')) nested--;
        else if (!nested && (c == ',' || c == ']')) {
            size_t end = i;
            char key[48], *child_path;
            int ok;
            while (start < end && strchr(" \r\n\t", json[start])) start++;
            while (end > start && strchr(" \r\n\t", json[end - 1])) end--;
            if (start == end) break; /* Empty array; validation ruled out holes. */
            snprintf(key, sizeof(key), "[%zu]", index);
            child_path = dj_path(path, key);
            if (!child_path) return 0;
            if (shape->count_known && index >= shape->count)
                ok = dj_gap(walk, child_path, type, "array exceeds the native collection capacity");
            else ok = dj_value(walk, json + start, end - start, shape->element, child_path, depth + 1);
            free(child_path);
            if (!ok) return 0;
            index++; start = i + 1;
        }
    }
    return 1;
}

static int dj_value(dj_walk *walk, const char *json, size_t length,
                    sh_decl_value_type type, const char *path, unsigned depth)
{
    sh_decl_value_shape shape = {0};
    dj_object object = {walk, type, path, depth};
    int ok = 1;
    if (depth > 128) return 0;
    while (length && strchr(" \r\n\t", *json)) { json++; length--; }
    while (length && strchr(" \r\n\t", json[length - 1])) length--;
    walk->result->visited++;
    if (!type.name || !walk->schema->describe(walk->schema->context, type, &shape))
        shape.kind = SH_DECL_VALUE_UNKNOWN;
    if (shape.kind == SH_DECL_VALUE_IGNORE) return 1;
    if (shape.kind == SH_DECL_VALUE_REFERENCE) {
        char *name;
        size_t decoded = 0;
        if (length == 4 && !memcmp(json, "null", 4)) return 1;
        if (length == SIZE_MAX || !(name = (char *)malloc(length + 1))) return 0;
        if (!sh_native_json_decode_string(json, length, name, length + 1, &decoded) || strlen(name) != decoded)
            ok = dj_gap(walk, path, type, "resource JSON is not a literal name");
        else if (decoded) {
            ok = walk->schema->reference(walk->schema->context, path, type, name, decoded);
            if (ok && walk->schema->reference_site) {
                sh_decl_reference_site site = {NULL, json, length};
                ok = walk->schema->reference_site(walk->schema->context, path, type, name, decoded, &site);
            }
            if (ok) walk->result->references++;
        }
        free(name); return ok;
    }
    if (shape.kind == SH_DECL_VALUE_COLLECTION)
        return dj_array(walk, json, length, &shape, type, path, depth);
    if (shape.kind == SH_DECL_VALUE_SCRIPT) {
        if (length && (json[0] == '"' || json[0] == 'n')) {
            sh_decl_value_type selected;
            return dj_script_type(&object, json, length, 0, &selected);
        }
        if (length < 2 || json[0] != '{' || json[length - 1] != '}')
            return dj_gap(walk, path, type, "script object state is not a JSON object or class name");
        return sh_native_json_visit_objects_filtered(json, length, 128, dj_script_fields, dj_shallow, &object);
    }
    if (shape.kind != SH_DECL_VALUE_OBJECT)
        return dj_gap(walk, path, type, "type requires a verified native reader adapter");
    if (length < 2 || json[0] != '{' || json[length - 1] != '}')
        return dj_gap(walk, path, type, "native object state is not a JSON object");
    return sh_native_json_visit_objects_filtered(json, length, 128, dj_fields, dj_shallow, &object);
}

int sh_decl_json_state_dependencies(const char *json, size_t length, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    dj_walk walk = {schema, result};
    sh_json_kind kind;
    int ok;
    if (result) memset(result, 0, sizeof(*result));
    if (!result) return 0;
    if (!schema || !schema->describe || !schema->field || !schema->reference ||
        !json || !sh_native_json_validate(json, length, 128, &kind, NULL) || kind != SH_JSON_OBJECT) {
        result->aborted = 1; return 0;
    }
    ok = dj_value(&walk, json, length, type, "edit", 0);
    if (!ok) result->aborted = 1;
    return ok && !result->gaps;
}
