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
    sh_json_object object = {0};
    size_t i;
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
            if (ok) walk->result->references++;
        }
        free(name); return ok;
    }
    if (shape.kind == SH_DECL_VALUE_COLLECTION)
        return dj_array(walk, json, length, &shape, type, path, depth);
    if (shape.kind != SH_DECL_VALUE_OBJECT)
        return dj_gap(walk, path, type, "type requires a verified native reader adapter");
    if (!sh_native_json_parse_object(json, length, 128, &object))
        return dj_gap(walk, path, type, "native object state is not a JSON object");
    for (i = 0; ok && i < object.count; i++) {
        const sh_json_member *member = &object.members[i];
        sh_decl_value_type field = {0};
        char *child_path;
        int found;
        if (!strcmp(member->key, "~type") || !strcmp(member->key, "~version") ||
            !strcmp(member->key, "editorVars")) continue;
        found = walk->schema->field(walk->schema->context, type, member->key, &field);
        if (!found) continue;
        child_path = dj_path(path, member->key);
        if (!child_path) { ok = 0; break; }
        if (found < 0 || !field.name)
            ok = dj_gap(walk, child_path, type, "field semantics are unavailable");
        else ok = dj_value(walk, member->value_json, strlen(member->value_json), field, child_path, depth + 1);
        free(child_path);
    }
    sh_json_object_free(&object);
    return ok;
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
