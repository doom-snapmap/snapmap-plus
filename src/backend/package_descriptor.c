#include "package_descriptor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PD_MAX_DEPTH 24u

static int pd_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return 0;
}

static int pd_string(const char *raw, char *out, size_t capacity)
{
    size_t length = 0;
    return raw && sh_json_decode_string(raw, strlen(raw), out, capacity, &length) &&
           strlen(out) == length;
}

static int pd_text(const char *raw)
{
    size_t size, length = 0;
    char *decoded;
    int ok;
    if (!raw) return 0;
    size = strlen(raw) + 1u;
    decoded = (char *)malloc(size);
    if (!decoded) return 0;
    ok = sh_json_decode_string(raw, size - 1u, decoded, size, &length) &&
         strlen(decoded) == length;
    free(decoded);
    return ok;
}

int sh_package_id_valid(const char *id)
{
    size_t i, length;
    if (!id || !(length = strlen(id)) || length >= SH_PACKAGE_ID_CAP) return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)id[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (i == 0 || i + 1u == length || (c != '.' && c != '_' && c != '-')) return 0;
        if (c == '.' && id[i - 1u] == '.') return 0;
    }
    return 1;
}

static int pd_locale_valid(const char *locale, size_t length)
{
    size_t i;
    if (!length || length >= 48u || strlen(locale) != length) return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)locale[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) continue;
        if (c != '-' || i == 0 || i + 1u == length || locale[i - 1u] == '-') return 0;
    }
    return 1;
}

/* Native keys compare case-insensitively. JSON's distinct spelling must not
 * hide conflicting strings within one locale or locale aliases. */
static int pd_key_equal(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
        if (ac != bc) return 0;
    }
    return *a == *b;
}

static int pd_unique_keys(const sh_json_object *object)
{
    size_t i, j;
    for (i = 0; i < object->count; i++) {
        if (!object->members[i].key_length ||
            strlen(object->members[i].key) != object->members[i].key_length) return 0;
        for (j = 0; j < i; j++)
            if (pd_key_equal(object->members[i].key, object->members[j].key)) return 0;
    }
    return 1;
}

static int pd_strings_valid(const char *raw)
{
    sh_json_object locales = {0};
    size_t i;
    int ok = 0;
    if (!sh_json_parse_object(raw, strlen(raw), PD_MAX_DEPTH, &locales) ||
        !pd_unique_keys(&locales)) goto done;
    for (i = 0; i < locales.count; i++) {
        sh_json_object strings = {0};
        const sh_json_member *locale = &locales.members[i];
        size_t j;
        if (!pd_locale_valid(locale->key, locale->key_length)) goto done;
        if (!sh_json_parse_object(locale->value_json, strlen(locale->value_json),
                                  PD_MAX_DEPTH, &strings)) goto done;
        int valid = pd_unique_keys(&strings);
        for (j = 0; valid && j < strings.count; j++)
            valid = pd_text(strings.members[j].value_json);
        sh_json_object_free(&strings);
        if (!valid) goto done;
    }
    ok = 1;
done:
    sh_json_object_free(&locales);
    return ok;
}

static int pd_object(const char *raw)
{
    sh_json_kind kind;
    return raw && sh_json_validate(raw, strlen(raw), PD_MAX_DEPTH, &kind) &&
           kind == SH_JSON_OBJECT;
}

void sh_package_descriptor_free(sh_package_descriptor *descriptor)
{
    if (!descriptor) return;
    sh_json_object_free(&descriptor->fields);
    memset(descriptor, 0, sizeof(*descriptor));
}

const char *sh_package_descriptor_section(const sh_package_descriptor *descriptor,
                                          const char *section)
{
    return descriptor && section ? sh_json_object_get(&descriptor->fields, section) : NULL;
}

int sh_package_descriptor_parse(const char *json, size_t length,
                                 sh_package_descriptor *out,
                                 char *error, size_t error_capacity)
{
    sh_package_descriptor parsed = {0};
    const char *raw;
    const char *failure = "package.json must contain one valid JSON object";
    if (error && error_capacity) error[0] = '\0';
    if (!out) return pd_error(error, error_capacity, "missing package descriptor output");
    memset(out, 0, sizeof(*out));
    if (!json || !length || length > PTRDIFF_MAX ||
        !sh_json_parse_object(json, length, PD_MAX_DEPTH, &parsed.fields)) goto fail;

    failure = "package id must be a stable lowercase identifier";
    if (!pd_string(sh_json_object_get(&parsed.fields, "id"), parsed.id, sizeof(parsed.id)) ||
        !sh_package_id_valid(parsed.id)) goto fail;
    failure = "package name must be a nonempty string";
    if (!pd_string(sh_json_object_get(&parsed.fields, "name"), parsed.name, sizeof(parsed.name)) ||
        !parsed.name[0]) goto fail;

    raw = sh_json_object_get(&parsed.fields, "requirements");
    failure = "package requirements must be an object";
    if (raw && !pd_object(raw)) goto fail;
    raw = sh_json_object_get(&parsed.fields, "hud");
    failure = "package hud must be an object";
    if (raw && !pd_object(raw)) goto fail;
    raw = sh_json_object_get(&parsed.fields, "strings");
    failure = "package strings must map unique locales and native keys to text";
    if (raw && !pd_strings_valid(raw)) goto fail;
    *out = parsed;
    return 1;

fail:
    sh_package_descriptor_free(&parsed);
    return pd_error(error, error_capacity, failure);
}
