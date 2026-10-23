#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_strings.h"

typedef struct fixture {
    sh_package_sources sources;
    sh_package_component components[8];
} fixture;
typedef struct reservation_log {
    char *keys[64];
    size_t count, collisions;
    int fail, all_collide;
} reservation_log;

static void add(fixture *f, size_t owner, char *relative, const char *strings)
{
    const char *format = "{\"id\":\"same.id\",\"name\":\"Same ID\",\"strings\":%s}";
    char error[256], *json = malloc(strlen(strings) + strlen(format) + 1u);
    sh_package_component *component;
    assert(f->sources.component_count < 8 && json);
    f->sources.components = f->components;
    if (f->sources.package_count <= owner) f->sources.package_count = owner + 1u;
    component = f->components + f->sources.component_count++;
    component->owner = owner; component->relative = relative;
    sprintf(json, format, strings);
    assert(sh_package_descriptor_parse(json, strlen(json), &component->descriptor, error, sizeof(error)));
    free(json);
}
static void dispose(fixture *f)
{
    for (size_t i = 0; i < f->sources.component_count; i++)
        sh_package_descriptor_free(&f->components[i].descriptor);
    memset(f, 0, sizeof(*f));
}
static void clear_log(reservation_log *log)
{
    for (size_t i = 0; i < log->count; i++) free(log->keys[i]);
    memset(log, 0, sizeof(*log));
}
static int reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping_json)
{
    reservation_log *log = context;
    unsigned char zero[32] = {0};
    assert(log->count < 64 && !strncmp(key, "#str_smp_loc_", 13) && memcmp(identity, zero, 32));
    assert(mapping_json && strstr(mapping_json, "\"native\"") && strstr(mapping_json, "\"locales\""));
    log->keys[log->count++] = _strdup(key); assert(log->keys[log->count - 1u]);
    if (log->fail) return log->fail;
    /* This is the caller's native-ID check double, deliberately independent
     * of textual key/digest uniqueness. A different key can still collide. */
    if (log->all_collide || log->count <= log->collisions) return 0;
    for (size_t i = log->collisions; i + 1u < log->count; i++)
        assert(strcmp(log->keys[i], key));
    return 1;
}
static sh_package_strings *compile(fixture *f, const sh_package_string_request *requests,
    size_t count, reservation_log *log)
{
    sh_package_string_reservations reservations = {reserve, log, 8};
    char error[256];
    sh_package_strings *out = sh_package_strings_compile(&f->sources, requests, count,
        &reservations, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out); return out;
}
static void expect(const sh_package_strings *strings, size_t request,
    const char *const *chain, size_t count, const char *text, const char *native)
{
    sh_package_string_selection selection;
    char error[256];
    assert(sh_package_strings_select(strings, request, chain, count, &selection, error, sizeof(error)));
    assert(selection.key == sh_package_strings_key(strings, request));
    if (text) {
        assert(selection.kind == SH_PACKAGE_STRING_AUTHORED && !selection.native_key && selection.locale);
        assert(selection.length == strlen(text) && !memcmp(selection.text, text, strlen(text) + 1u));
    } else {
        assert(selection.kind == SH_PACKAGE_STRING_NATIVE && !selection.text && !selection.locale && !selection.length);
        assert(native && !strcmp(selection.native_key, native));
    }
}
static void policy_expect(const sh_json_object *policy, const char *locale,
    const char *key, const char *text)
{
    sh_json_object pairs = {0};
    const char *raw = sh_json_object_get(policy, locale), *value;
    char decoded[256]; size_t length;
    assert(raw && sh_json_parse_object(raw, strlen(raw), 1, &pairs));
    assert(!strncmp(key, "#str_", 5));
    value = sh_json_object_get(&pairs, key + 5);
    if (text) {
        assert(value && sh_json_decode_string(value, strlen(value), decoded, sizeof(decoded), &length));
        assert(length == strlen(text) && !strcmp(decoded, text));
    } else assert(!value);
    sh_json_object_free(&pairs);
}

static void scoped_resolution(void)
{
    fixture f = {0}; reservation_log log = {0};
    const char *en[] = {"en"}, *fr[] = {"fr"}, *de[] = {"de"};
    const char *missing[] = {"en-US"}, *fallback[] = {"en-US", "EN", "fr"};
    const char *unicode = "Caf\xc3\xa9 \xf0\x9f\x9a\x80\n\"quoted\"\\";
    sh_package_string_request requests[] = {
        {1, "#str_label"}, {2, "#str_label"}, {3, "#str_label"}, {4, "#str_label"},
        {1, "#str_empty"}, {1, "#str_unicode"}, {1, "#str_native"},
        {1, ""}, {1, "#custom_native"}, {1, "#str_\xff"}, {1, "#str_sibling_only"}
    };
    char *before[5], error[256];
    sh_package_strings *strings;
    sh_json_object policy = {0};
    add(&f, 0, "", "{\"en\":{\"label\":\"Root\",\"empty\":\"Not empty\"},\"de\":{\"label\":\"Eltern\"}}");
    add(&f, 0, "red", "{\"en\":{\"label\":\"Shared\",\"empty\":\"\",\"unicode\":\"Caf\\u00e9 \\ud83d\\ude80\\n\\\"quoted\\\"\\\\\"},\"fr\":{\"label\":\"Rouge\"}}");
    add(&f, 0, "red/deep", "{\"fr\":{\"label\":\"Profond\"}}");
    add(&f, 0, "blue", "{\"en\":{\"label\":\"Shared\",\"sibling_only\":\"Hidden\"},\"fr\":{\"label\":\"Bleu\"}}");
    add(&f, 1, "", "{\"DE\":{\"LABEL\":\"Eltern\"},\"fr\":{\"label\":\"Rouge\"},\"en\":{\"label\":\"Shared\"}}");
    for (size_t i = 0; i < 5; i++) {
        before[i] = sh_json_serialize_object(&f.components[i].descriptor.fields, 0, NULL); assert(before[i]);
    }
    strings = compile(&f, requests, sizeof(requests) / sizeof(*requests), &log);
    assert(sh_package_strings_mapping_count(strings) == 5 && log.count == 5);
    assert(!strcmp(sh_package_strings_key(strings, 0), sh_package_strings_key(strings, 3)));
    assert(!memcmp(sh_package_strings_identity(strings, 0), sh_package_strings_identity(strings, 3), 32));
    assert(strcmp(sh_package_strings_key(strings, 0), sh_package_strings_key(strings, 1)));
    assert(strcmp(sh_package_strings_key(strings, 0), sh_package_strings_key(strings, 2)));
    expect(strings, 0, en, 1, "Shared", NULL); expect(strings, 0, de, 1, "Eltern", NULL);
    expect(strings, 0, fr, 1, "Rouge", NULL); expect(strings, 1, fr, 1, "Profond", NULL);
    expect(strings, 1, en, 1, "Shared", NULL); expect(strings, 2, fr, 1, "Bleu", NULL);
    expect(strings, 4, en, 1, "", NULL); expect(strings, 5, en, 1, unicode, NULL);
    expect(strings, 0, missing, 1, NULL, "#str_label");
    expect(strings, 0, fallback, 3, "Shared", NULL);
    expect(strings, 0, NULL, 0, NULL, "#str_label");
    for (size_t i = 6; i < sizeof(requests) / sizeof(*requests); i++) {
        assert(!strcmp(sh_package_strings_key(strings, i), requests[i].key));
        assert(!sh_package_strings_identity(strings, i));
        expect(strings, i, en, 1, NULL, requests[i].key);
    }
    assert(sh_package_strings_policy(strings, &policy, error, sizeof(error)));
    assert(policy.count == 3);
    policy_expect(&policy, "en", sh_package_strings_key(strings, 4), "");
    policy_expect(&policy, "en", sh_package_strings_key(strings, 5), unicode);
    policy_expect(&policy, "fr", sh_package_strings_key(strings, 0), "Rouge");
    policy_expect(&policy, "fr", sh_package_strings_key(strings, 5), NULL);
    for (size_t i = 0; i < 5; i++) {
        char *after = sh_json_serialize_object(&f.components[i].descriptor.fields, 0, NULL);
        assert(after && !strcmp(before[i], after)); free(before[i]); free(after);
    }
    /* The compiled result owns its text and fallback keys, not source views. */
    dispose(&f); expect(strings, 0, fr, 1, "Rouge", NULL); expect(strings, 5, en, 1, unicode, NULL);
    sh_json_object_free(&policy); sh_package_strings_free(strings); clear_log(&log);
}

static void identity_and_order(void)
{
    fixture f = {0}, reversed = {0}; reservation_log a = {0}, b = {0};
    sh_package_string_request forward[] = {{0, "#str_key"}, {1, "#STR_KEY"}, {2, "#str_key"}, {0, "#str_alias"}};
    sh_package_string_request backward[] = {{2, "#str_alias"}, {0, "#str_key"}, {1, "#STR_KEY"}, {2, "#str_key"}};
    const char *docs[] = {
        "{\"en\":{\"key\":\"Caf\\u00e9\",\"alias\":\"Caf\\u00e9\"},\"fr\":{\"key\":\"X\",\"alias\":\"X\"}}",
        "{\"FR\":{\"KEY\":\"X\"},\"EN\":{\"KEY\":\"Caf\xc3\xa9\"}}",
        "{\"en\":{\"key\":\"Caf\\u00e9\"},\"fr\":{\"key\":\"Y\"}}"
    };
    sh_json_object pa = {0}, pb = {0}; char error[256], *ja, *jb;
    sh_package_strings *first, *second;
    for (size_t i = 0; i < 3; i++) { add(&f, i, "", docs[i]); add(&reversed, 2u - i, "", docs[2u - i]); }
    first = compile(&f, forward, 4, &a); second = compile(&reversed, backward, 4, &b);
    assert(a.count == 3 && b.count == 3 && sh_package_strings_mapping_count(first) == 3);
    assert(!strcmp(sh_package_strings_key(first, 0), sh_package_strings_key(first, 1)));
    assert(strcmp(sh_package_strings_key(first, 0), sh_package_strings_key(first, 2)));
    /* Equal authored texts under different original keys have different native
     * fallback identities, so they are not complete semantic duplicates. */
    assert(strcmp(sh_package_strings_key(first, 0), sh_package_strings_key(first, 3)));
    for (size_t i = 0; i < 4; i++)
        assert(!strcmp(sh_package_strings_key(first, i), sh_package_strings_key(second, 3u - i)));
    for (size_t i = 0; i < a.count; i++) assert(!strcmp(a.keys[i], b.keys[i]));
    assert(sh_package_strings_policy(first, &pa, error, sizeof(error)));
    assert(sh_package_strings_policy(second, &pb, error, sizeof(error)));
    ja = sh_json_serialize_object(&pa, 0, NULL); jb = sh_json_serialize_object(&pb, 0, NULL);
    assert(ja && jb && !strcmp(ja, jb)); free(ja); free(jb);
    sh_json_object_free(&pa); sh_json_object_free(&pb);
    sh_package_strings_free(first); sh_package_strings_free(second);
    clear_log(&a); clear_log(&b); dispose(&f); dispose(&reversed);
}

static void reservations_and_failures(void)
{
    fixture f = {0}; reservation_log log = {0}; char error[256];
    sh_package_string_request request = {0, "#str_key"}, unknown = {0, "#str_native"};
    sh_package_string_reservations reservations = {reserve, &log, 3};
    sh_package_strings *strings;
    sh_package_string_selection selection;
    sh_json_object policy = {0};
    const char *bad_locale[] = {NULL}, *empty_locale[] = {""};
    add(&f, 0, "", "{\"en\":{\"key\":\"Text\"}}");
    log.collisions = 2;
    strings = compile(&f, &request, 1, &log);
    assert(log.count == 3 && strstr(sh_package_strings_key(strings, 0), "_2"));
    assert(strcmp(log.keys[0], log.keys[1]) && strcmp(log.keys[1], log.keys[2]));
    assert(!sh_package_strings_select(strings, 0, bad_locale, 1, &selection, error, sizeof(error)) && !selection.key);
    assert(!sh_package_strings_select(strings, 0, empty_locale, 1, &selection, error, sizeof(error)) && !selection.key);
    assert(!sh_package_strings_select(strings, 1, NULL, 0, &selection, error, sizeof(error)) && !selection.key);
    assert(!sh_package_strings_key(strings, 1) && !sh_package_strings_identity(strings, 1));
    assert(sh_json_object_set(&policy, "sentinel", "true", 1));
    assert(!sh_package_strings_policy(NULL, &policy, error, sizeof(error)));
    assert(!strcmp(sh_json_object_get(&policy, "sentinel"), "true"));
    assert(sh_package_strings_policy(strings, &policy, error, sizeof(error)));
    assert(!sh_json_object_get(&policy, "sentinel")); sh_json_object_free(&policy);
    sh_package_strings_free(strings); clear_log(&log);
    log.all_collide = 1;
    assert(!sh_package_strings_compile(&f.sources, &request, 1, &reservations, error, sizeof(error)));
    assert(log.count == 3 && strstr(error, "exhausted")); clear_log(&log);
    log.fail = -1;
    assert(!sh_package_strings_compile(&f.sources, &request, 1, &reservations, error, sizeof(error)));
    assert(log.count == 1 && strstr(error, "failed")); clear_log(&log);
    log.fail = 2;
    assert(!sh_package_strings_compile(&f.sources, &request, 1, &reservations, error, sizeof(error)));
    assert(strstr(error, "invalid")); clear_log(&log);
    assert(!sh_package_strings_compile(&f.sources, &request, 1, NULL, error, sizeof(error)));
    assert(strstr(error, "reservation"));
    strings = sh_package_strings_compile(&f.sources, &unknown, 1, NULL, error, sizeof(error));
    assert(strings && !sh_package_strings_mapping_count(strings)); sh_package_strings_free(strings);
    strings = sh_package_strings_compile(&f.sources, NULL, 0, NULL, error, sizeof(error));
    assert(strings && !sh_package_strings_mapping_count(strings)); sh_package_strings_free(strings);
    request.component = 1;
    assert(!sh_package_strings_compile(&f.sources, &request, 1, &reservations, error, sizeof(error)));
    request.component = 0;
    /* Even a violated validated-descriptor precondition cannot truncate a
     * NUL-containing value and call it a successful translation. */
    assert(sh_json_object_set(&f.components[0].descriptor.fields, "strings", "{\"en\":{\"key\":\"A\\u0000B\"}}", 3));
    assert(!sh_package_strings_compile(&f.sources, &request, 1, &reservations, error, sizeof(error)) && !log.count);
    dispose(&f);
}

int main(void)
{
    scoped_resolution(); identity_and_order(); reservations_and_failures();
    puts("package string scope, locale, identity, fallback and reservation checks passed");
    return 0;
}
