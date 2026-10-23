#include "decl_native_carrier.h"
#include "config_json.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dc_refuse(sh_decl_carrier_refusal *refusal, sh_decl_carrier_limit limit, const char *reason)
{
    if (refusal) { refusal->limit = limit; refusal->reason = reason; }
    return 0;
}

typedef struct dc_text { char *data; size_t length, capacity; } dc_text;
static int dc_append(dc_text *t, const char *bytes, size_t length)
{
    if (length > SIZE_MAX - t->length - 1) return 0;
    if (t->length + length + 1 > t->capacity) {
        size_t next = t->capacity ? t->capacity : 64;
        while (next < t->length + length + 1) {
            if (next > SIZE_MAX / 2) return 0;
            next *= 2;
        }
        char *grown = realloc(t->data, next);
        if (!grown) return 0;
        t->data = grown; t->capacity = next;
    }
    memcpy(t->data + t->length, bytes, length);
    t->length += length; t->data[t->length] = 0;
    return 1;
}
static int dc_append_text(dc_text *t, const char *text) { return dc_append(t, text, strlen(text)); }

/* JSON string. Bytes needing JSON escapes other than controls never reach here
 * (the carrier refuses them); controls use \u00XX, which JsonCpp decodes to the
 * same byte. Other bytes, including non-ASCII, are copied verbatim. */
static int dc_json_string(dc_text *t, const char *bytes, size_t length)
{
    if (!dc_append(t, "\"", 1)) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 0x20) {
            char escape[8]; snprintf(escape, sizeof(escape), "\\u%04x", c);
            if (!dc_append_text(t, escape)) return 0;
        } else if (c == '"' || c == '\\') {
            char escape[2] = {'\\', (char)c};
            if (!dc_append(t, escape, 2)) return 0;
        } else if (!dc_append(t, (const char *)&c, 1)) return 0;
    }
    return dc_append(t, "\"", 1);
}

/* FS_WriteFloatString "%f": flush below FLT_MIN, "%1.10f", then
 * StripTrailing('0') and StripTrailing('.'). */
static void dc_float_text(float value, char *out, size_t capacity)
{
    double shown = fabsf(value) < FLT_MIN ? 0.0 : (double)value;
    size_t length;
    snprintf(out, capacity, "%1.10f", shown);
    length = strlen(out);
    while (length && out[length - 1] == '0') out[--length] = 0;
    while (length && out[length - 1] == '.') out[--length] = 0;
}

/* A decimal spelling for a float field. Declaration parsing reads it as
 * (float)atof(text) (idToken::NumberValue); SnapMap loads the saved real as
 * (float)asDouble and writes it back with dc_float_text, and a later native save
 * stores atof(that) with "%#.16g". Accept only values that survive all of it. */
static int dc_decimal_float(const char *value, char *carrier, size_t capacity,
    sh_decl_carrier_refusal *refusal)
{
    char *end = NULL, saved[64];
    double parsed = strtod(value, &end);
    float authored = (float)parsed, loaded, resaved;
    /* The native lexer includes an empty lowercase exponent. Its CRT reader
     * rewinds that exponent and keeps the mantissa (paired source readers
     * 141ea72f0/141ec0760). strtod leaves end at the same exponent. */
    int empty_exponent = end && end[0] == 'e' && (!end[1] ||
        ((end[1] == '+' || end[1] == '-') && !end[2]));
    if (!end || (*end && !empty_exponent) || !isfinite(authored))
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "float spelling has no proven native value");
    if (authored != 0.0f && fabsf(authored) < FLT_MIN)
        return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE, "SnapMap stores this denormal float as zero");
    /* Beyond 2^24 the carrier text is an integer token, whose float value the
     * lexer accumulates digit by digit; that path is not modelled here. */
    if (fabsf(authored) >= 16777216.0f)
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "large float spelling needs the native integer-token float path");
    dc_float_text(authored, carrier, capacity);
    loaded = (float)strtod(carrier, NULL);
    if (loaded != authored || signbit(loaded) != signbit(authored))
        return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE,
            "SnapMap writes this float with ten decimals and loads back a different value");
    snprintf(saved, sizeof(saved), "%#.16g", strtod(carrier, NULL));
    resaved = (float)strtod(saved, NULL);
    if (resaved != authored || signbit(resaved) != signbit(authored))
        return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE, "this float changes across a native map save");
    return 1;
}

int sh_decl_carrier_scalar(const char *value, const char *field_type, char **json,
    sh_decl_carrier_refusal *refusal)
{
    size_t length = value ? strlen(value) : 0, i, at, digits = 0, dots = 0;
    int exponent = 0;
    dc_text out = {0};
    if (refusal) { refusal->limit = SH_DECL_CARRIER_EXACT; refusal->reason = NULL; }
    if (!json) return -1;
    *json = NULL;
    if (!length) return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE, "an empty value loads back as NULL");
    if (value[0] == '"') {
        if (length < 2 || value[length - 1] != '"')
            return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "value is not one carrier token");
        for (i = 1; i + 1 < length; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c == '"') return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "value is not one carrier token");
            /* The carrier's writer escapes these bytes; declaration parsing
             * keeps backslashes literally, so the loaded string would differ. */
            if ((c >= 0x07 && c <= 0x0d) || c == '\'' || c == '?' || c == '\\')
                return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE,
                    "SnapMap escapes this string byte on load, changing the value");
        }
        if (!dc_json_string(&out, value + 1, length - 2)) { free(out.data); return -1; }
        *json = out.data; return 1;
    }
    if (!strcmp(value, "true") || !strcmp(value, "false") || !strcmp(value, "NULL")) {
        *json = _strdup(!strcmp(value, "NULL") ? "null" : value);
        return *json ? 1 : -1;
    }
    /* ReadNumber (VK 141a0c920 / GL 1419feb50) classifies a decimal
     * mantissa followed by lowercase 'e' and an optional sign as float.
     * Leading zero without an immediate dot instead selects its octal path. */
    at = value[0] == '-';
    if (value[at] == '0' && length - at > 1 && value[at + 1] != '.')
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "octal spelling needs its native integer conversion");
    for (i = at; i < length; i++) {
        if (value[i] == '.') dots++;
        else if (value[i] >= '0' && value[i] <= '9') digits++;
        else if (value[i] == 'e') { exponent = 1; break; }
        else return dc_refuse(refusal, SH_DECL_CARRIER_GAP,
            "spelling outside the carrier's number grammar needs a proven equivalent for its reader");
    }
    if (!digits || dots > 1)
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "number spelling has no proven native value");
    if (exponent) {
        i++;
        if (value[i] == '+' || value[i] == '-') i++;
        for (; i < length && value[i] >= '0' && value[i] <= '9'; i++) {}
        if (i != length)
            return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "float exponent is not one complete number");
    }
    if (!dots && !exponent) {
        int64_t magnitude = 0;
        if (at && length == 2 && value[1] == '0')
            return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "negative zero loads back as zero");
        for (i = at; i < length; i++) {
            if (magnitude > INT64_C(214748364700)) break;
            magnitude = magnitude * 10 + (value[i] - '0');
        }
        /* JsonCpp asInt logs and truncates outside this range. */
        if (i < length || (at ? magnitude > INT64_C(2147483647) : magnitude > INT64_C(2147483646)))
            return dc_refuse(refusal, SH_DECL_CARRIER_ENGINE, "integer is outside SnapMap's 32-bit carrier range");
        *json = _strdup(value);
        return *json ? 1 : -1;
    }
    if (!field_type)
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "float spelling of a field whose native reader is unknown");
    if (strcmp(field_type, "float"))
        return dc_refuse(refusal, SH_DECL_CARRIER_GAP, "float spelling of a non-float field needs its native reader");
    char carrier[80];
    if (!dc_decimal_float(value, carrier, sizeof(carrier), refusal)) return 0;
    /* Keep the JSON a real (JsonCpp reads a spelling without '.' as an
     * integer, which would drop the sign of -0 and change the load path). */
    if (!dc_append_text(&out, carrier) || (!strchr(carrier, '.') && !dc_append_text(&out, ".0"))) {
        free(out.data); return -1;
    }
    *json = out.data; return 1;
}

typedef struct dc_frame { const sh_decl_node *node, *child; size_t path_length; } dc_frame;

static int dc_key(const char *key)
{
    unsigned char c = (unsigned char)key[0];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    for (; *key; key++) if (*key == '"' || *key == '\\' || (unsigned char)*key < 0x20) return 0;
    return 1;
}

char *sh_decl_carrier_state(const sh_decl_node *members, sh_decl_carrier_leaf_info info,
    void *context, size_t *length, sh_decl_carrier_refusal *refusal)
{
    dc_text out = {0};
    dc_frame *stack = NULL;
    size_t count = 0, capacity = 0;
    char path[256] = "";
    int ok = 0;
    if (length) *length = 0;
    if (refusal) { refusal->limit = SH_DECL_CARRIER_EXACT; refusal->reason = NULL; refusal->path[0] = 0; }
    if (!members || !members->compound || !dc_append(&out, "{", 1)) goto done;
    stack = malloc(16 * sizeof(*stack)); capacity = 16;
    if (!stack) goto done;
    stack[count++] = (dc_frame){members, members->children, 0};
    while (count) {
        dc_frame *frame = &stack[count - 1];
        const sh_decl_node *node = frame->child;
        size_t base = frame->path_length, key_length;
        path[base] = 0;
        if (!node) {
            if (!dc_append(&out, "}", 1)) goto done;
            count--; continue;
        }
        frame->child = node->next;
        key_length = strlen(node->key);
        if (base + key_length + 2 < sizeof(path)) {
            if (base) path[base++] = '.';
            memcpy(path + base, node->key, key_length + 1);
        }
        if (refusal) snprintf(refusal->path, sizeof(refusal->path), "%s", path);
        if (node->annotation) { dc_refuse(refusal, SH_DECL_CARRIER_GAP, "type-annotated member has no carrier spelling"); goto done; }
        if (node->reset) { dc_refuse(refusal, SH_DECL_CARRIER_ENGINE, "SnapMap's carrier has no spelling for a reset block"); goto done; }
        if (!node->assignment) { dc_refuse(refusal, SH_DECL_CARRIER_GAP, "a block without '=' is written back with '='"); goto done; }
        if (!dc_key(node->key)) { dc_refuse(refusal, SH_DECL_CARRIER_GAP, "the carrier would quote this member name"); goto done; }
        if (out.data[out.length - 1] != '{' && !dc_append(&out, ",", 1)) goto done;
        if (!dc_json_string(&out, node->key, key_length) || !dc_append(&out, ":", 1)) goto done;
        if (node->compound) {
            if (count == capacity) {
                dc_frame *grown;
                if (capacity > SIZE_MAX / 2 / sizeof(*stack) ||
                    !(grown = realloc(stack, capacity * 2 * sizeof(*stack)))) goto done;
                stack = grown; capacity *= 2;
            }
            if (!dc_append(&out, "{", 1)) goto done;
            stack[count++] = (dc_frame){node, node->children, strlen(path)};
        } else {
            sh_decl_carrier_leaf leaf = {0};
            char *value = NULL;
            if (info) info(context, node, &leaf);
            if (leaf.json) {
                if (!dc_append_text(&out, leaf.json)) goto done;
                continue;
            }
            int scalar = sh_decl_carrier_scalar(node->value, leaf.type, &value, refusal);
            if (scalar <= 0) goto done;
            int appended = dc_append_text(&out, value);
            free(value);
            if (!appended) goto done;
        }
    }
    ok = 1;
done:
    free(stack);
    if (!ok) { free(out.data); return NULL; }
    if (refusal) refusal->path[0] = 0;
    if (length) *length = out.length;
    return out.data;
}

/* FUN_1419fe5a0 / FUN_1419f0800: quote and escape a loaded string. */
static int dc_native_quoted(dc_text *t, const char *bytes, size_t length)
{
    static const char codes[] = "abtnvfr";
    if (!dc_append(t, "\"", 1)) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        char escape[2] = {'\\', 0};
        if (c >= 0x07 && c <= 0x0d) escape[1] = codes[c - 0x07];
        else if (c == '"' || c == '\'' || c == '?' || c == '\\') escape[1] = (char)c;
        if (escape[1] ? !dc_append(t, escape, 2) : !dc_append(t, (const char *)&c, 1)) return 0;
    }
    return dc_append(t, "\"", 1);
}

/* JsonCpp decodeNumber (FUN_141a5c350 / FUN_141a4e830), then the carrier's
 * asInt/asUInt/asDouble writer. */
static int dc_json_number_text(const char *json, size_t length, char *out, size_t capacity)
{
    int real = 0, negative = length && json[0] == '-';
    uint64_t value = 0, maximum = negative ? UINT64_C(0x8000000000000000) : UINT64_MAX;
    for (size_t i = 0; i < length; i++) {
        char c = json[i];
        if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && i)) real = 1;
    }
    for (size_t i = negative; !real && i < length; i++) {
        if (json[i] < '0' || json[i] > '9') return 0;
        if (maximum / 10 <= value) { real = 1; break; }
        value = value * 10 + (uint64_t)(json[i] - '0');
    }
    if (real) {
        char buffer[64];
        if (length >= sizeof(buffer)) return 0;
        memcpy(buffer, json, length); buffer[length] = 0;
        dc_float_text((float)strtod(buffer, NULL), out, capacity);
        return 1;
    }
    if (negative || value < UINT64_C(0x8000000000000000))
        snprintf(out, capacity, "%d", (int)(uint32_t)(negative ? (uint64_t)0 - value : value));
    else snprintf(out, capacity, "%u", (unsigned)(uint32_t)value);
    return 1;
}


typedef struct dc_parse_frame { sh_decl_node *owner; sh_json_object object; size_t next; } dc_parse_frame;

sh_decl_node *sh_decl_carrier_tree(const char *json, size_t length,
    int (*verbatim)(void *context, const sh_decl_node *leaf, const char *json, size_t length),
    void *context, char *error, size_t capacity)
{
    sh_decl_node *root = calloc(1, sizeof(*root));
    dc_parse_frame *stack = NULL;
    size_t count = 0, allocated = 0;
    int ok = 0;
    if (error && capacity) *error = 0;
    if (!root) goto done;
    root->compound = 1; root->assignment = 1;
    stack = calloc(8, sizeof(*stack)); allocated = 8;
    if (!stack || !sh_native_json_parse_object(json, length, 64, &stack[0].object)) {
        if (error && capacity) snprintf(error, capacity, "state is not a native JSON object");
        goto done;
    }
    stack[0].owner = root; count = 1;
    while (count) {
        dc_parse_frame *frame = &stack[count - 1];
        if (frame->next == frame->object.count) {
            sh_json_object_free(&frame->object); count--;
            continue;
        }
        const sh_json_member *member = frame->object.members + frame->next++;
        const char *value = member->value_json;
        size_t value_length = strlen(value);
        sh_json_kind kind;
        sh_json_error parse_error;
        sh_decl_node *node = calloc(1, sizeof(*node)), **tail = &frame->owner->children;
        if (!node) goto done;
        while (*tail) tail = &(*tail)->next;
        *tail = node;
        node->assignment = 1;
        if (!(node->key = malloc(member->key_length + 1))) goto done;
        memcpy(node->key, member->key, member->key_length); node->key[member->key_length] = 0;
        if (strlen(node->key) != member->key_length ||
            !sh_native_json_validate(value, value_length, 64, &kind, &parse_error)) {
            if (error && capacity) snprintf(error, capacity, "state member '%s' is not native JSON", node->key);
            goto done;
        }
        if (kind == SH_JSON_OBJECT) {
            node->compound = 1;
            if (count == allocated) {
                dc_parse_frame *grown = allocated <= SIZE_MAX / 2 / sizeof(*stack) ?
                    realloc(stack, allocated * 2 * sizeof(*stack)) : NULL;
                if (!grown) goto done;
                stack = grown; allocated *= 2;
                frame = &stack[count - 1];
            }
            memset(&stack[count], 0, sizeof(stack[count]));
            if (!sh_native_json_parse_object(value, value_length, 64, &stack[count].object)) goto done;
            stack[count++].owner = node;
            continue;
        }
        if (kind == SH_JSON_ARRAY) {
            if (error && capacity) snprintf(error, capacity, "state member '%s' is an array, which the native reader rejects", node->key);
            goto done;
        }
        dc_text text = {0};
        if (kind == SH_JSON_STRING) {
            char *bytes = malloc(value_length + 1); size_t decoded = 0;
            if (!bytes || !sh_native_json_decode_string(value, value_length, bytes, value_length + 1, &decoded) ||
                !dc_native_quoted(&text, bytes, decoded)) { free(bytes); free(text.data); goto done; }
            free(bytes);
        } else if (kind == SH_JSON_NUMBER) {
            char number[80];
            if (!dc_json_number_text(value, value_length, number, sizeof(number)) || !dc_append_text(&text, number)) {
                free(text.data); goto done;
            }
        } else if (!dc_append_text(&text, kind == SH_JSON_NULL ? "NULL" : !strcmp(value, "true") ? "true" : "false")) {
            free(text.data); goto done;
        }
        node->value = text.data;
        if (verbatim && !verbatim(context, node, value, value_length)) goto done;
    }
    ok = 1;
done:
    while (count) sh_json_object_free(&stack[--count].object);
    free(stack);
    if (!ok) {
        sh_decl_tree_free(root);
        if (error && capacity && !*error) snprintf(error, capacity, "cannot model the loaded declaration text");
        return NULL;
    }
    return root;
}
