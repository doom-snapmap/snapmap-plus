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
                    sh_decl_value_type type, const char *path, unsigned depth,
                    const char *owner, const char *member);

typedef struct dj_object {
    dj_walk *walk;
    sh_decl_value_type type;
    const char *path;
    unsigned depth;
    const char *owner, *member;
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
        else ok = dj_value(walk, member->value, member->value_length, field, child_path, object->depth + 1,
            object->type.name, member->key);
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
    size_t i, at = count, members = 0;
    /* The engine writes a script object's members in its own order (a saved
     * map lists them alphabetically, so "object" follows "limitTurningOnly");
     * the class selector types every other member wherever it appears. */
    for (i = 0; i < count; i++) {
        if (!strcmp(fields[i].key, "~type") || !strcmp(fields[i].key, "~version")) continue;
        if (!strcmp(fields[i].key, "object")) at = i; else members++;
    }
    if (at == count)
        return members ? dj_gap(object->walk, object->path, object->type, "script class selector is missing") : 1;
    if (!dj_script_type(object, fields[at].value, fields[at].value_length, 1, &selected.type)) return 0;
    if (!selected.type.name) return 1;
    return dj_fields(&selected, fields, at, depth) && dj_fields(&selected, fields + at + 1, count - at - 1, depth);
}

/* The complete input was validated before any callbacks. This only splits
 * already valid array elements, respecting nested values and quoted commas. */
static int dj_array(dj_walk *walk, const char *json, size_t length,
                    const sh_decl_value_shape *shape, sh_decl_value_type type,
                    const char *path, unsigned depth, const char *owner, const char *member)
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
            else ok = dj_value(walk, json + start, end - start, shape->element, child_path, depth + 1, owner, member);
            free(child_path);
            if (!ok) return 0;
            index++; start = i + 1;
        }
    }
    return 1;
}

/* A fixed native array serializes as an object holding only the elements that
 * differ from their defaults, named as in declaration text: the item spelling
 * or the field's own name, then the index. spawnOrientation, for example, is
 * {"mat":{"mat[0]":{"x":0,"y":1},"mat[1]":{"x":-1,"y":0}}}. */
typedef struct dj_fixed {
    dj_walk *walk;
    const sh_decl_value_shape *shape;
    sh_decl_value_type type;
    const char *path, *prefix;
    size_t prefix_length;
    unsigned depth;
    const char *owner, *member;
} dj_fixed;

static int dj_count_key(const dj_fixed *array, const sh_json_field_span *field)
{
    const char *key = array->shape->count_key;
    return key && field->key_length == strlen(key) && !memcmp(field->key, key, field->key_length);
}

static int dj_fixed_members(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_fixed *array = context;
    size_t i, extent = SIZE_MAX;
    (void)depth;
    /* A counted list may state its count; its slots may not exceed it. */
    for (i = 0; i < count; i++) if (dj_count_key(array, fields + i)) {
        size_t at, value = 0;
        int valid = fields[i].value_length > 0;
        for (at = 0; valid && at < fields[i].value_length; at++) {
            unsigned digit = (unsigned char)fields[i].value[at] - (unsigned)'0';
            if (digit > 9 || value > (SIZE_MAX - 1 - digit) / 10) valid = 0;
            else value = value * 10 + digit;
        }
        if (!valid) return dj_gap(array->walk, array->path, array->type, "collection count is not a native count");
        extent = value;
    }
    for (i = 0; i < count; i++) {
        const char *key = fields[i].key, *reason = NULL;
        size_t length = fields[i].key_length, at, index = 0;
        char *child_path;
        int ok;
        if (dj_count_key(array, fields + i)) continue;
        if (length < array->prefix_length + 3 || memcmp(key, array->prefix, array->prefix_length) ||
            key[array->prefix_length] != '[' || key[length - 1] != ']') reason = "unsupported collection member";
        for (at = array->prefix_length + 1; !reason && at < length - 1; at++) {
            unsigned digit = (unsigned char)key[at] - (unsigned)'0';
            if (digit > 9 || index > (SIZE_MAX - digit) / 10) reason = "unsupported collection member";
            else index = index * 10 + digit;
        }
        if (!reason && array->shape->count_known && index >= array->shape->count)
            reason = "collection index exceeds native count";
        if (!reason && index >= extent) reason = "collection index exceeds its stated count";
        child_path = dj_path(array->path, key);
        if (!child_path) return 0;
        ok = reason ? dj_gap(array->walk, child_path, array->type, reason) :
            dj_value(array->walk, fields[i].value, fields[i].value_length, array->shape->element,
                child_path, array->depth + 1, array->owner, array->member);
        free(child_path);
        if (!ok) return 0;
    }
    return 1;
}

static int dj_fixed_array(dj_walk *walk, const char *json, size_t length,
                          const sh_decl_value_shape *shape, sh_decl_value_type type,
                          const char *path, unsigned depth, const char *owner, const char *member)
{
    const char *field = strrchr(path, '.');
    dj_fixed array = {walk, shape, type, path, shape->item_key, 0, depth, owner, member};
    if (!array.prefix) array.prefix = field ? field + 1 : path;
    array.prefix_length = strcspn(array.prefix, "[");
    /* Native entity state writes counted lists keyed like declaration text as
     * well: sparse slots and an optional count, for example a player inventory's
     * {"item[0]":"weapon/zion/player/sp/pistol","item[1]":"NULL"}. */
    if (!shape->element.name || json[length - 1] != '}')
        return dj_gap(walk, path, type, "collection JSON requires a verified array reader");
    return sh_native_json_visit_objects_filtered(json, length, 128, dj_fixed_members, dj_shallow, &array);
}

/* idEventCallInfo, as declaration text spells it (decl_dependencies.c):
 * {"eventDef":"name","args":{"num":1,"item[0]":{"decl":{"sound":"x"}}}},
 * where the writer may annotate eventDef and num with their types. */
typedef struct dj_event {
    dj_walk *walk;
    sh_decl_value_type type;
    const char *path;
    size_t extent;
    size_t index;                   /* the argument being read */
    char event[128];                /* the decoded literal eventDef, for event_value */
    int named;                      /* event holds that name */
    const char *signature;          /* borrowed through each reference callback */
} dj_event;

static void dj_trim(const char **json, size_t *length)
{
    while (*length && strchr(" \r\n\t", **json)) { (*json)++; (*length)--; }
    while (*length && strchr(" \r\n\t", (*json)[*length - 1])) (*length)--;
}

static int dj_null(const char *json, size_t length)
{ return length == 4 && !memcmp(json, "null", 4); }

static int dj_event_decl_member(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_event *event = context;
    dj_walk *walk = event->walk;
    sh_decl_value_shape shape = {0};
    const char *json;
    size_t length, decoded = 0;
    char *name;
    int ok = 1;
    (void)depth;
    if (count != 1)
        return dj_gap(walk, event->path, event->type, "event decl argument must name exactly one declaration type");
    json = fields[0].value; length = fields[0].value_length; dj_trim(&json, &length);
    if (length < 2 || json[0] != '"')
        return dj_gap(walk, event->path, event->type, "event decl argument name is not a literal name");
    if (!(name = (char *)malloc(length + 1))) return 0;
    if (!sh_native_json_decode_string(json, length, name, length + 1, &decoded) || strlen(name) != decoded)
        ok = dj_gap(walk, event->path, event->type, "event decl argument name is not a literal name");
    else if (!decoded || (decoded == 4 && !memcmp(name, "NULL", 4))) ok = 1;
    else if (!walk->schema->describe(walk->schema->context,
            (sh_decl_value_type){fields[0].key, SH_DECL_EVENT_DECL_OPS}, &shape) ||
        (shape.kind != SH_DECL_VALUE_IGNORE && (shape.kind != SH_DECL_VALUE_REFERENCE || !shape.element.name)))
        ok = dj_gap(walk, event->path, event->type, "event decl argument type has no verified declaration manager");
    else if (shape.kind == SH_DECL_VALUE_REFERENCE) {
        ok = walk->schema->reference(walk->schema->context, event->path, shape.element, name, decoded);
        if (ok && walk->schema->reference_site) {
            sh_decl_reference_site site = {NULL, json, length};
            site.event_name = event->named ? event->event : NULL;
            site.event_name_length = event->named ? strlen(event->event) : 0;
            site.event_argument_index = event->index; site.event_argument_count = event->extent;
            site.event_argument_signature = event->signature;
            ok = walk->schema->reference_site(walk->schema->context, event->path, shape.element, name, decoded, &site);
        }
        if (ok) walk->result->references++;
    }
    free(name); return ok;
}

/* md6Anim: a JSON string naming one animation resource, kept exactly. */
static int dj_event_anim(dj_event *event, const char *json, size_t length)
{
    dj_walk *walk = event->walk;
    sh_decl_value_shape shape = {0};
    size_t decoded = 0;
    char *name;
    int ok;
    if (length < 2 || json[0] != '"')
        return dj_gap(walk, event->path, event->type, "md6Anim event argument is not a literal animation name");
    if (!(name = (char *)malloc(length + 1))) return 0;
    if (!sh_native_json_decode_string(json, length, name, length + 1, &decoded) || strlen(name) != decoded)
        ok = dj_gap(walk, event->path, event->type, "md6Anim event argument is not a literal animation name");
    else if (!decoded) ok = 1;
    else if (!walk->schema->describe(walk->schema->context,
            (sh_decl_value_type){SH_DECL_EVENT_ANIM_TYPE, SH_DECL_EVENT_ANIM_OPS}, &shape) ||
        shape.kind != SH_DECL_VALUE_REFERENCE || !shape.element.name)
        ok = dj_gap(walk, event->path, event->type, "md6Anim event argument has no verified animation resource reader");
    else {
        ok = walk->schema->reference(walk->schema->context, event->path, shape.element, name, decoded);
        if (ok && walk->schema->reference_site) {
            sh_decl_reference_site site = {NULL, json, length};
            ok = walk->schema->reference_site(walk->schema->context, event->path, shape.element, name, decoded, &site);
        }
        if (ok) walk->result->references++;
    }
    free(name); return ok;
}

/* One argument: its single key selects the native reader. */
static int dj_event_argument(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_event *event = context, decl = *event;
    const sh_json_field_span *reader = NULL;
    const char *json;
    size_t i, length;
    char *path;
    int ok, kind = 0;
    (void)depth;
    for (i = 0; i < count; i++) {
        int selected = sh_decl_event_argument_kind(fields[i].key);
        if (selected) { kind = selected; reader = fields + i; }
    }
    if (!reader) {
        dj_walk *walk = event->walk;
        if (count != 1 || !walk->schema->event_value) return 1;
        json = fields[0].value; length = fields[0].value_length; dj_trim(&json, &length);
        if (!(path = dj_path(event->path, fields[0].key))) return 0;
        sh_decl_reference_site site = {NULL, json, length};
        ok = walk->schema->event_value(walk->schema->context, path, event->named ? event->event : NULL,
            event->named ? strlen(event->event) : 0, event->index, sh_decl_event_member(fields[0].key), &site);
        free(path); return ok;
    }
    if (count > 1) return dj_gap(event->walk, event->path, event->type, "event argument has more than one reader key");
    json = reader->value; length = reader->value_length; dj_trim(&json, &length);
    if (!(path = dj_path(event->path, reader->key))) return 0;
    decl.path = path;
    if (kind == 2) ok = dj_event_anim(&decl, json, length);
    else if (dj_null(json, length) || (length == 6 && !memcmp(json, "\"NULL\"", 6))) ok = 1;
    else if (length >= 2 && json[0] == '{' && json[length - 1] == '}')
        ok = sh_native_json_visit_objects_filtered(json, length, 128, dj_event_decl_member, dj_shallow, &decl);
    else ok = dj_gap(event->walk, path, event->type, "event decl argument is not a native declaration block");
    free(path); return ok;
}

static int dj_event_index(const char *key, size_t *index)
{
    size_t length = strlen(key), value = 0;
    if (length < 7 || memcmp(key, "item[", 5) || key[length - 1] != ']') return 0;
    for (size_t at = 5; at < length - 1; at++) {
        unsigned digit = (unsigned char)key[at] - (unsigned)'0';
        if (digit > 9 || value > (SIZE_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *index = value; return 1;
}
typedef struct dj_event_format {
    const sh_decl_dependency_schema *schema;
    char format;
} dj_event_format;
static int dj_event_reader_format(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_event_format *reader = context;
    (void)depth;
    if (count == 1) reader->format = sh_decl_event_argument_format(reader->schema, fields[0].key);
    return 1;
}
static char *dj_event_signature(const dj_event *event, const sh_json_field_span *fields, size_t count)
{
    size_t last = 0, supplied = 0, counts = 0;
    unsigned seen = 0;
    char *signature;
    for (size_t i = 0; i < count; i++) {
        const char *key = sh_decl_event_member(fields[i].key); size_t index = 0;
        if (!strcmp(key, "num")) { counts++; continue; }
        if (supplied++ >= event->extent || !dj_event_index(key, &index) || index >= SH_DECL_EVENT_MAX_ARGUMENTS ||
            (seen & (1u << index))) return NULL;
        seen |= 1u << index;
        if (index + 1 > last) last = index + 1;
    }
    if (counts != 1 || !(signature = malloc(last + 1))) return NULL;
    memset(signature, '?', last); signature[last] = 0;
    for (size_t i = 0; i < count; i++) {
        const char *key = sh_decl_event_member(fields[i].key), *json = fields[i].value;
        size_t index = 0, length = fields[i].value_length;
        dj_event_format reader = {event->walk->schema};
        if (!strcmp(key, "num")) continue;
        if (!dj_event_index(key, &index) || signature[index] != '?') { free(signature); return NULL; }
        dj_trim(&json, &length);
        if (dj_null(json, length)) reader.format = '0';
        else if (length >= 2 && json[0] == '{' && json[length - 1] == '}')
            sh_native_json_visit_objects_filtered(json, length, 128, dj_event_reader_format, dj_shallow, &reader);
        if (!reader.format) { free(signature); return NULL; }
        signature[index] = reader.format;
    }
    return signature;
}
static int dj_event_arguments(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_event *event = context;
    char *signature;
    size_t i, supplied = 0, counts = 0;
    unsigned seen = 0;
    (void)depth;
    for (i = 0; i < count; i++) if (!strcmp(sh_decl_event_member(fields[i].key), "num")) {
        if (counts++) { dj_gap(event->walk, event->path, event->type, "repeated event argument count"); continue; }
        if (!sh_decl_event_argument_count(fields[i].value, fields[i].value_length, &event->extent))
            return dj_gap(event->walk, event->path, event->type, "unsupported event argument count");
    }
    signature = dj_event_signature(event, fields, count);
    event->signature = signature;
    for (i = 0; i < count; i++) {
        const char *key = sh_decl_event_member(fields[i].key), *reason = NULL, *json = fields[i].value;
        size_t index = 0, value_length = fields[i].value_length;
        dj_event item = *event;
        char *path;
        int ok = 1;
        if (!strcmp(key, "num")) continue;
        if (supplied++ >= event->extent) reason = "event argument record exceeds its stated count";
        else if (!dj_event_index(key, &index)) reason = "unsupported event argument member";
        else if (index >= SH_DECL_EVENT_MAX_ARGUMENTS) reason = "event argument index exceeds native argument storage";
        else if (seen & (1u << index)) reason = "repeated event argument index";
        else seen |= 1u << index;
        dj_trim(&json, &value_length);
        if (!reason && !dj_null(json, value_length) &&
            (value_length < 2 || json[0] != '{' || json[value_length - 1] != '}'))
            reason = "event argument is not a native argument block";
        if (!(path = dj_path(event->path, fields[i].key))) { free(signature); return 0; }
        item.path = path; item.index = index;
        if (reason) dj_gap(event->walk, path, event->type, reason);
        else if (!dj_null(json, value_length))
            ok = sh_native_json_visit_objects_filtered(json, value_length, 128, dj_event_argument, dj_shallow, &item);
        free(path);
        if (!ok) { free(signature); return 0; }
    }
    free(signature); event->signature = NULL;
    return 1;
}

static int dj_event_members(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    dj_event *event = context;
    size_t i, args = 0;
    sh_decl_reference_site selected = {NULL, NULL, 0};
    (void)depth;
    for (i = 0; i < count; i++) {
        const char *json = fields[i].value;
        size_t length = fields[i].value_length, decoded = 0;
        if (strcmp(sh_decl_event_member(fields[i].key), "eventDef")) continue;
        dj_trim(&json, &length);
        selected.json = json; selected.json_length = length;
        event->named = length >= 2 && json[0] == '"' &&
            sh_native_json_decode_string(json, length, event->event, sizeof(event->event), &decoded) &&
            strlen(event->event) == decoded;
    }
    if (event->walk->schema->event_value) {
        if (!event->walk->schema->event_value(event->walk->schema->context, event->path,
                event->named ? event->event : NULL, event->named ? strlen(event->event) : 0, SIZE_MAX, NULL, &selected))
            return 0;
    }
    for (i = 0; i < count; i++) {
        const char *name = sh_decl_event_member(fields[i].key), *json = fields[i].value;
        size_t length = fields[i].value_length;
        dj_event block = *event;
        char *path;
        int ok = 1;
        if (!strcmp(fields[i].key, "~type") || !strcmp(fields[i].key, "~version") || !strcmp(name, "eventDef")) continue;
        if (!(path = dj_path(event->path, "args"))) return 0;
        dj_trim(&json, &length);
        block.path = path; block.extent = 0;
        if (strcmp(name, "args")) dj_gap(event->walk, event->path, event->type, "event call member is not read by the native reader");
        else if (args++) dj_gap(event->walk, path, event->type, "repeated event arguments");
        else if (length >= 2 && json[0] == '{' && json[length - 1] == '}')
            ok = sh_native_json_visit_objects_filtered(json, length, 128, dj_event_arguments, dj_shallow, &block);
        else if (!dj_null(json, length)) dj_gap(event->walk, path, event->type, "event arguments are not a native argument block");
        free(path);
        if (!ok) return 0;
    }
    return 1;
}

static int dj_value(dj_walk *walk, const char *json, size_t length,
                    sh_decl_value_type type, const char *path, unsigned depth,
                    const char *owner, const char *member)
{
    sh_decl_value_shape shape = {0};
    dj_object object = {walk, type, path, depth, owner, member};
    int ok = 1;
    if (depth > 128) return 0;
    while (length && strchr(" \r\n\t", *json)) { json++; length--; }
    while (length && strchr(" \r\n\t", json[length - 1])) length--;
    walk->result->visited++;
    if (!type.name || !walk->schema->describe(walk->schema->context, type, &shape))
        shape.kind = SH_DECL_VALUE_UNKNOWN;
    if (shape.kind == SH_DECL_VALUE_IGNORE) {
        sh_decl_reference_site site = {NULL, json, length, owner, member};
        return !walk->schema->value_site || walk->schema->value_site(walk->schema->context, path, type, &site);
    }
    if (shape.kind == SH_DECL_VALUE_REFERENCE) {
        char *name;
        size_t decoded = 0;
        /* A by-name reference names a declaration of its element's class. */
        if (shape.element.ops && !strcmp(shape.element.ops, SH_DECL_BY_NAME_DECL_OPS)) type = shape.element;
        if (length == 4 && !memcmp(json, "null", 4)) return 1;
        /* The engine's own writer spells an absent file resource as a number
         * (a saved map carries "renderModelInfo":{"model":0}). An unquoted
         * scalar cannot name a package identity, so the walk records nothing. */
        if (length && (json[0] == '-' || (json[0] >= '0' && json[0] <= '9') || json[0] == 't' || json[0] == 'f'))
            return 1;
        if (length == SIZE_MAX || !(name = (char *)malloc(length + 1))) return 0;
        if (!sh_native_json_decode_string(json, length, name, length + 1, &decoded) || strlen(name) != decoded)
            ok = dj_gap(walk, path, type, "resource JSON is not a literal name");
        /* The native writer spells an empty reference as declaration text does. */
        else if (decoded == 4 && !memcmp(name, "NULL", 4)) ok = 1;
        /* The map reader loads a JSON "" as the quoted empty token
         * (sh_decl_carrier_tree), which a declaration reader looks up as
         * _emptyName (empty_lookup), at the literal's own span. A JSON null
         * loads as the NULL token and names nothing. */
        else if (decoded || (walk->schema->empty_lookup &&
                walk->schema->empty_lookup(walk->schema->context, type))) {
            const char *target = decoded ? name : "_emptyName";
            size_t target_length = decoded ? decoded : 10;
            ok = walk->schema->reference(walk->schema->context, path, type, target, target_length);
            if (ok && walk->schema->reference_site) {
                sh_decl_reference_site site = {NULL, json, length, owner, member};
                ok = walk->schema->reference_site(walk->schema->context, path, type, target, target_length, &site);
            }
            if (ok) walk->result->references++;
        }
        free(name); return ok;
    }
    if (shape.kind == SH_DECL_VALUE_COLLECTION)
        return length && json[0] == '{' ? dj_fixed_array(walk, json, length, &shape, type, path, depth, owner, member) :
            dj_array(walk, json, length, &shape, type, path, depth, owner, member);
    if (shape.kind == SH_DECL_VALUE_SCRIPT) {
        if (length && (json[0] == '"' || json[0] == 'n')) {
            sh_decl_value_type selected;
            return dj_script_type(&object, json, length, 0, &selected);
        }
        if (length < 2 || json[0] != '{' || json[length - 1] != '}')
            return dj_gap(walk, path, type, "script object state is not a JSON object or class name");
        return sh_native_json_visit_objects_filtered(json, length, 128, dj_script_fields, dj_shallow, &object);
    }
    if (shape.kind == SH_DECL_VALUE_EVENT_CALL) {
        dj_event event = {walk, type, path, SIZE_MAX};
        if (length < 2 || json[0] != '{' || json[length - 1] != '}')
            return dj_gap(walk, path, type, "event call state is not a JSON object");
        return sh_native_json_visit_objects_filtered(json, length, 128, dj_event_members, dj_shallow, &event);
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
    ok = dj_value(&walk, json, length, type, "edit", 0, NULL, NULL);
    if (!ok) result->aborted = 1;
    return ok && !result->gaps;
}
