#include "map_session_json.h"
#include "config_json.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct session_json {
    const char *slots;
    size_t length;
    int version, count, values[4][3];
} session_json;

static const sh_json_field_span *session_field(const sh_json_field_span *fields, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++) if (!strcmp(fields[i].key, key)) return fields + i;
    return NULL;
}
static int session_type(const sh_json_field_span *field, const char *expected)
{
    char decoded[64];
    return field && field->kind == SH_JSON_STRING &&
        sh_native_json_decode_string(field->value, field->value_length, decoded, sizeof(decoded), NULL) &&
        !strcmp(decoded, expected);
}
static int session_int(const sh_json_field_span *field, int *out)
{
    char text[64], *end;
    double value;
    if (!field || field->kind != SH_JSON_NUMBER || field->value_length >= sizeof(text)) return 0;
    memcpy(text, field->value, field->value_length); text[field->value_length] = 0;
    value = strtod(text, &end);
    if (*end || !(value >= INT_MIN && value <= INT_MAX) || value != (double)(int)value) return 0;
    *out = (int)value; return 1;
}
static int session_filter(void *context, const char *key, size_t length, unsigned depth)
{
    (void)context; (void)key; (void)length; (void)depth;
    return 0; /* Validate nested values without allocating descendant visitors. */
}
static int session_root(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    session_json *session = context;
    const sh_json_field_span *slots = session_field(fields, count, "snapSlotSettings");
    if (depth || !session_type(session_field(fields, count, "~type"), "idSnapMap") ||
        !session_int(session_field(fields, count, "~version"), &session->version) ||
        session->version < 72 || !slots || slots->kind != SH_JSON_ARRAY) return 0;
    session->slots = slots->value; session->length = slots->value_length; return 1;
}
static int session_slot(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    session_json *session = context;
    int *values;
    if (depth != 1 || session->count >= 4 ||
        !session_type(session_field(fields, count, "~type"), "snapLobbySlotSetting_t")) return 0;
    values = session->values[session->count++];
    return session_int(session_field(fields, count, "race"), values) &&
        session_int(session_field(fields, count, "state"), values + 1) &&
        session_int(session_field(fields, count, "team"), values + 2);
}
static int session_four_elements(const char *json, size_t length)
{
    unsigned depth = 0, separators = 0;
    int quoted = 0;
    /* Input already passed the shared native JSON validator. Count only the
     * outer array separators; the visitor separately requires four objects. */
    for (size_t i = 0; i < length; i++) {
        char c = json[i];
        if (quoted) { if (c == '\\') i++; else if (c == '"') quoted = 0; }
        else if (c == '"') quoted = 1;
        else if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') depth--;
        else if (c == ',' && depth == 1) separators++;
    }
    return separators == 3;
}
char *sh_map_session_json(const char *json, size_t length, const char *native_defaults,
    size_t defaults_length, char *error, size_t capacity)
{
    session_json session = {0};
    sh_json_object defaults = {0};
    char slots[768], *out = NULL;
    size_t used = 1;
    if (error && capacity) *error = 0;
    if (!json || !sh_native_json_visit_objects_filtered(json, length, 128, session_root, session_filter, &session) ||
        !session.slots || !session_four_elements(session.slots, session.length) ||
        !sh_native_json_visit_objects_filtered(session.slots, session.length, 8,
            session_slot, session_filter, &session) || session.count != 4) {
        if (error && capacity) snprintf(error, capacity, "The map's native format or four lobby slots are invalid.");
        return NULL;
    }
    /* Use the engine's empty snapshot, including its current native format.
     * The generic native reader requires all reflected fields to be present. */
    slots[0] = '[';
    for (int i = 0; i < 4; i++) used += (size_t)snprintf(slots + used, sizeof(slots) - used,
        "%s{\"~type\":\"snapLobbySlotSetting_t\",\"race\":%d,\"state\":%d,\"team\":%d}",
        i ? "," : "", session.values[i][0], session.values[i][1], session.values[i][2]);
    memcpy(slots + used, "]", 2);
    if (native_defaults && sh_native_json_parse_object(native_defaults, defaults_length, 128, &defaults) &&
        sh_json_object_set(&defaults, "snapSlotSettings", slots, 128))
        out = sh_json_serialize_object(&defaults, 0, NULL);
    sh_json_object_free(&defaults);
    if (!out && error && capacity) snprintf(error, capacity, "The native empty map settings could not be prepared.");
    return out;
}
