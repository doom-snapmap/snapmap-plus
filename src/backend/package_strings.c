#include "package_strings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct ps_translation {
    char *locale, *text, *json;
    size_t length;
} ps_translation;
typedef struct ps_mapping {
    char *native_key, *canonical, *compiled_key;
    ps_translation *translations;
    size_t count;
    unsigned char identity[32];
} ps_mapping;
typedef struct ps_binding {
    char *native_key;
    ps_mapping *mapping;
} ps_binding;
struct sh_package_strings {
    ps_mapping **mappings;
    size_t count;
    ps_binding *bindings;
    size_t binding_count;
};
typedef struct ps_ancestor { size_t component, depth; } ps_ancestor;

static int ps_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return 0;
}
static unsigned char ps_fold(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int ps_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (ps_fold((unsigned char)*a) != ps_fold((unsigned char)*b)) return 0;
    return *a == *b;
}
static char *ps_copy(const char *text, int fold)
{
    size_t length = strlen(text);
    char *out = malloc(length + 1u);
    if (out) {
        memcpy(out, text, length + 1u);
        if (fold) for (size_t i = 0; i < length; i++) out[i] = (char)ps_fold((unsigned char)out[i]);
    }
    return out;
}
static const sh_json_member *ps_member(const sh_json_object *object, const char *key)
{
    for (size_t i = 0; i < object->count; i++)
        if (ps_equal(object->members[i].key, key)) return object->members + i;
    return NULL;
}
static char *ps_quote(const char *text, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t size = 3u, at = 0;
    char *out;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        size_t extra = c < 32 ? 6u : c == '"' || c == '\\' ? 2u : 1u;
        if (size > SIZE_MAX - extra) return NULL;
        size += extra;
    }
    out = malloc(size);
    if (!out) return NULL;
    out[at++] = '"';
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32) {
            memcpy(out + at, "\\u00", 4); at += 4;
            out[at++] = hex[c >> 4]; out[at++] = hex[c & 15];
        } else {
            if (c == '"' || c == '\\') out[at++] = '\\';
            out[at++] = (char)c;
        }
    }
    out[at++] = '"'; out[at] = 0;
    return out;
}
static void ps_mapping_free(ps_mapping *mapping)
{
    if (!mapping) return;
    for (size_t i = 0; i < mapping->count; i++) {
        free(mapping->translations[i].locale);
        free(mapping->translations[i].text);
        free(mapping->translations[i].json);
    }
    free(mapping->translations); free(mapping->native_key);
    free(mapping->canonical); free(mapping->compiled_key); free(mapping);
}
void sh_package_strings_free(sh_package_strings *strings)
{
    if (!strings) return;
    for (size_t i = 0; i < strings->count; i++) ps_mapping_free(strings->mappings[i]);
    for (size_t i = 0; i < strings->binding_count; i++) free(strings->bindings[i].native_key);
    free(strings->mappings); free(strings->bindings); free(strings);
}
static int ps_ancestor_order(const void *left, const void *right)
{
    const ps_ancestor *a = left, *b = right;
    return a->depth > b->depth ? -1 : a->depth < b->depth;
}
static int ps_translation_order(const void *left, const void *right)
{ return strcmp(((const ps_translation *)left)->locale, ((const ps_translation *)right)->locale); }
static int ps_mapping_order(const void *left, const void *right)
{ return strcmp((*(const ps_mapping *const *)left)->canonical, (*(const ps_mapping *const *)right)->canonical); }

static int ps_translation_add(ps_mapping *mapping, const char *locale, const char *raw)
{
    ps_translation value = {0}, *grown;
    size_t raw_length = strlen(raw);
    for (size_t i = 0; i < mapping->count; i++)
        if (ps_equal(mapping->translations[i].locale, locale)) return 1;
    if (mapping->count >= SIZE_MAX / sizeof(*grown)) return 0;
    value.locale = ps_copy(locale, 1);
    value.text = malloc(raw_length + 1u);
    if (!value.locale || !value.text ||
        !sh_json_decode_string(raw, raw_length, value.text, raw_length + 1u, &value.length) ||
        strlen(value.text) != value.length || !(value.json = ps_quote(value.text, value.length))) goto failed;
    grown = realloc(mapping->translations, (mapping->count + 1u) * sizeof(*grown));
    if (!grown) goto failed;
    mapping->translations = grown; mapping->translations[mapping->count++] = value;
    return 1;
failed:
    free(value.locale); free(value.text); free(value.json); return 0;
}

static int ps_resolve(const sh_package_sources *sources, size_t component,
    const char *key, ps_mapping *mapping)
{
    ps_ancestor *ancestors = NULL;
    size_t count = 0;
    const sh_package_component *child = sources->components + component;
    sh_json_object canonical = {0}, locales = {0};
    char *native_json = NULL, *locales_json = NULL;
    sh_package_file_identity identity;
    int ok = 0;
    if (sources->component_count > SIZE_MAX / sizeof(*ancestors)) return 0;
    ancestors = malloc(sources->component_count * sizeof(*ancestors));
    if (!ancestors) goto done;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *parent = sources->components + i;
        size_t depth = strlen(parent->relative);
        if (parent->owner == child->owner && (!depth ||
            (!strncmp(parent->relative, child->relative, depth) &&
                (!child->relative[depth] || child->relative[depth] == '/'))))
            ancestors[count++] = (ps_ancestor){i, depth};
    }
    qsort(ancestors, count, sizeof(*ancestors), ps_ancestor_order);
    for (size_t i = 0; i < count; i++) {
        sh_json_object source = {0};
        const char *raw = sh_package_descriptor_section(
            &sources->components[ancestors[i].component].descriptor, "strings");
        if (i && ancestors[i - 1u].depth == ancestors[i].depth) goto done;
        if (!raw) continue;
        if (!sh_json_parse_object(raw, strlen(raw), 24, &source)) goto done;
        for (size_t j = 0; j < source.count; j++) {
            const sh_json_member *locale = source.members + j, *text;
            sh_json_object pairs = {0};
            int valid = locale->key_length && strlen(locale->key) == locale->key_length &&
                sh_json_parse_object(locale->value_json, strlen(locale->value_json), 24, &pairs);
            text = valid ? ps_member(&pairs, key + 5u) : NULL;
            if (text && !ps_translation_add(mapping, locale->key, text->value_json)) valid = 0;
            sh_json_object_free(&pairs);
            if (!valid) { sh_json_object_free(&source); goto done; }
        }
        sh_json_object_free(&source);
    }
    if (!mapping->count) { ok = 1; goto done; }
    qsort(mapping->translations, mapping->count, sizeof(*mapping->translations), ps_translation_order);
    mapping->native_key = ps_copy(key, 1);
    if (!mapping->native_key) goto done;
    native_json = ps_quote(mapping->native_key, strlen(mapping->native_key));
    for (size_t i = 0; i < mapping->count; i++) {
        const ps_translation *value = mapping->translations + i;
        if (!sh_json_object_set(&locales, value->locale, value->json, 1)) goto done;
    }
    locales_json = sh_json_serialize_object(&locales, 0, NULL);
    if (!native_json || !locales_json || !sh_json_object_set(&canonical, "native", native_json, 1) ||
        !sh_json_object_set(&canonical, "locales", locales_json, 2) ||
        !(mapping->canonical = sh_json_serialize_object(&canonical, 0, NULL)) ||
        !sh_package_bytes_identity(mapping->canonical, strlen(mapping->canonical), &identity)) goto done;
    memcpy(mapping->identity, identity.digest, sizeof(mapping->identity)); ok = 1;
done:
    free(ancestors); free(native_json); free(locales_json);
    sh_json_object_free(&canonical); sh_json_object_free(&locales);
    return ok;
}

static int ps_assign(sh_package_strings *strings,
    const sh_package_string_reservations *reservations, char *error, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    if (!strings->count) return 1;
    if (!reservations || !reservations->reserve || !reservations->max_attempts)
        return ps_error(error, capacity, "private string keys require native ID reservation");
    qsort(strings->mappings, strings->count, sizeof(*strings->mappings), ps_mapping_order);
    for (size_t i = 0; i < strings->count; i++) {
        ps_mapping *mapping = strings->mappings[i];
        char token[65], candidate[128];
        for (size_t j = 0; j < 32; j++) {
            token[j * 2u] = hex[mapping->identity[j] >> 4];
            token[j * 2u + 1u] = hex[mapping->identity[j] & 15];
        }
        token[64] = 0;
        for (size_t attempt = 0; attempt < reservations->max_attempts; attempt++) {
            int reserved, duplicate = 0;
            if (!attempt) snprintf(candidate, sizeof(candidate), "#str_smp_loc_%s", token);
            else snprintf(candidate, sizeof(candidate), "#str_smp_loc_%s_%zu", token, attempt);
            for (size_t j = 0; j < i; j++)
                if (!strcmp(strings->mappings[j]->compiled_key, candidate)) { duplicate = 1; break; }
            if (duplicate) continue; /* Exact canonical bytes, not digests, decide equality. */
            reserved = reservations->reserve(reservations->context, candidate, mapping->identity, mapping->canonical);
            if (reserved == -1) return ps_error(error, capacity, "native string ID reservation failed");
            if (!reserved) continue;
            if (reserved != 1) return ps_error(error, capacity, "invalid native string ID reservation result");
            mapping->compiled_key = ps_copy(candidate, 0);
            if (!mapping->compiled_key) return ps_error(error, capacity, "cannot retain compiled string key");
            break;
        }
        if (!mapping->compiled_key)
            return ps_error(error, capacity, "native string ID collision attempts exhausted");
    }
    return 1;
}

sh_package_strings *sh_package_strings_compile(const sh_package_sources *sources,
    const sh_package_string_request *requests, size_t count,
    const sh_package_string_reservations *reservations, char *error, size_t capacity)
{
    sh_package_strings *strings = NULL;
    const char *reason = "invalid source-scoped string requests";
    if (error && capacity) error[0] = 0;
    if (!sources || (count && !requests) ||
        (sources->component_count && !sources->components) ||
        count > SIZE_MAX / sizeof(ps_binding) || count > SIZE_MAX / sizeof(ps_mapping *)) goto failed;
    for (size_t i = 0; i < sources->component_count; i++)
        if (sources->components[i].owner >= sources->package_count || !sources->components[i].relative) goto failed;
    for (size_t i = 0; i < count; i++)
        if (requests[i].component >= sources->component_count || !requests[i].key) goto failed;
    reason = "cannot retain source-scoped strings";
    strings = calloc(1, sizeof(*strings));
    if (!strings) goto failed;
    strings->bindings = calloc(count ? count : 1u, sizeof(*strings->bindings));
    strings->mappings = calloc(count ? count : 1u, sizeof(*strings->mappings));
    if (!strings->bindings || !strings->mappings) goto failed;
    strings->binding_count = count;
    for (size_t i = 0; i < count; i++) {
        const char *key = requests[i].key;
        ps_mapping *mapping;
        size_t previous;
        strings->bindings[i].native_key = ps_copy(key, 0);
        if (!strings->bindings[i].native_key) goto failed;
        if (strlen(key) <= 5u || key[0] != '#' || ps_fold((unsigned char)key[1]) != 's' ||
            ps_fold((unsigned char)key[2]) != 't' || ps_fold((unsigned char)key[3]) != 'r' || key[4] != '_') continue;
        for (previous = 0; previous < i; previous++)
            if (requests[previous].component == requests[i].component && ps_equal(requests[previous].key, key)) break;
        if (previous < i) {
            strings->bindings[i].mapping = strings->bindings[previous].mapping; continue;
        }
        mapping = calloc(1, sizeof(*mapping));
        if (!mapping) goto failed;
        if (!ps_resolve(sources, requests[i].component, key, mapping)) {
            ps_mapping_free(mapping); reason = "cannot resolve source-scoped string mapping"; goto failed;
        }
        if (!mapping->count) { ps_mapping_free(mapping); continue; }
        size_t match;
        for (match = 0; match < strings->count; match++)
            if (!strcmp(strings->mappings[match]->canonical, mapping->canonical)) {
                ps_mapping_free(mapping); mapping = strings->mappings[match]; break;
            }
        strings->bindings[i].mapping = mapping;
        if (match == strings->count) strings->mappings[strings->count++] = mapping;
    }
    if (!ps_assign(strings, reservations, error, capacity)) { sh_package_strings_free(strings); return NULL; }
    return strings;
failed:
    sh_package_strings_free(strings); ps_error(error, capacity, reason); return NULL;
}

const char *sh_package_strings_key(const sh_package_strings *strings, size_t request)
{
    const ps_binding *binding;
    if (!strings || request >= strings->binding_count) return NULL;
    binding = strings->bindings + request;
    return binding->mapping ? binding->mapping->compiled_key : binding->native_key;
}
const unsigned char *sh_package_strings_identity(const sh_package_strings *strings, size_t request)
{
    return strings && request < strings->binding_count && strings->bindings[request].mapping ?
        strings->bindings[request].mapping->identity : NULL;
}
size_t sh_package_strings_mapping_count(const sh_package_strings *strings)
{ return strings ? strings->count : 0; }

int sh_package_strings_select(const sh_package_strings *strings, size_t request,
    const char *const *locale_order, size_t locale_count,
    sh_package_string_selection *out, char *error, size_t capacity)
{
    const ps_binding *binding;
    if (error && capacity) error[0] = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !strings || request >= strings->binding_count || (locale_count && !locale_order))
        return ps_error(error, capacity, "invalid source-scoped string language selection");
    for (size_t i = 0; i < locale_count; i++)
        if (!locale_order[i] || !locale_order[i][0])
            return ps_error(error, capacity, "invalid source-scoped string locale");
    binding = strings->bindings + request;
    out->key = sh_package_strings_key(strings, request);
    out->kind = SH_PACKAGE_STRING_NATIVE;
    out->native_key = binding->native_key;
    if (!binding->mapping) return 1;
    for (size_t i = 0; i < locale_count; i++)
        for (size_t j = 0; j < binding->mapping->count; j++) {
            const ps_translation *value = binding->mapping->translations + j;
            if (!ps_equal(locale_order[i], value->locale)) continue;
            out->kind = SH_PACKAGE_STRING_AUTHORED; out->native_key = NULL;
            out->locale = value->locale; out->text = value->text; out->length = value->length;
            return 1;
        }
    return 1;
}

int sh_package_strings_policy(const sh_package_strings *strings,
    sh_json_object *out, char *error, size_t capacity)
{
    sh_json_object policy = {0};
    if (error && capacity) error[0] = 0;
    if (!strings || !out) return ps_error(error, capacity, "invalid compiled string policy output");
    for (size_t i = 0; i < strings->count; i++) {
        const ps_mapping *mapping = strings->mappings[i];
        for (size_t j = 0; j < mapping->count; j++) {
            const ps_translation *value = mapping->translations + j;
            sh_json_object pairs = {0};
            const char *previous = sh_json_object_get(&policy, value->locale);
            char *json = NULL;
            int ok = (!previous || sh_json_parse_object(previous, strlen(previous), 1, &pairs)) &&
                sh_json_object_set(&pairs, mapping->compiled_key + 5u, value->json, 1) &&
                (json = sh_json_serialize_object(&pairs, 0, NULL)) &&
                sh_json_object_set(&policy, value->locale, json, 2);
            free(json); sh_json_object_free(&pairs);
            if (!ok) {
                sh_json_object_free(&policy);
                return ps_error(error, capacity, "cannot assemble compiled string policy");
            }
        }
    }
    sh_json_object_free(out); *out = policy; return 1;
}
