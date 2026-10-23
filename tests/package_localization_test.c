/* Typed package localization through binding, native reservation and refresh. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "package_library.h"
#include "package_runtime.h"
#include "strids.h"
#include "hook.h"
#include "package_fixture.h"
#include "../src/backend/virtual_material_runtime.c"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
int hook_is_installed(void *trampoline) { return trampoline != NULL; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
void backend_log(const char *message) { (void)message; }
int sh_user_overrides_enabled_for_launch(void) { return 1; }
static const sh_package_compilation *published;
const sh_package_compilation *sh_package_runtime_acquire(void) { return published; }
void sh_package_runtime_release(void) {}

typedef struct region { void *address; size_t length; } region;
typedef struct reflection_fixture {
    region regions[128]; size_t count;
    void *registry, *reflection;
} reflection_fixture;
static void *allocate(reflection_fixture *f, size_t length)
{
    void *p = calloc(1, length); assert(p && f->count < 128);
    f->regions[f->count++] = (region){p, length}; return p;
}
static void pointer(void *p, size_t offset, const void *value) { memcpy((char *)p + offset, &value, 8); }
static void number(void *p, size_t offset, int value) { memcpy((char *)p + offset, &value, 4); }
static char *text(reflection_fixture *f, const char *value)
{ char *copy = allocate(f, strlen(value) + 1); strcpy(copy, value); return copy; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    reflection_fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (const void *)address, length); return 1;
        }
    }
    return 0;
}
static void setup_reflection(reflection_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "idStrId", "idStr"};
    f->registry = allocate(f, 0x18);
    void *managers = allocate(f, 8), *manager = allocate(f, 0x90);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 1); pointer(managers, 0, manager);
    pointer(manager, 8, text(f, "entityDef")); pointer(manager, 0x10, text(f, "idDeclEntityDef"));
    f->reflection = allocate(f, 0xb0);
    void *container = allocate(f, 0x30), *records = allocate(f, 5 * 0x38);
    void *objects = allocate(f, 5 * 16), *pointers = allocate(f, 5 * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 5);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 5);
    for (size_t i = 0; i < 4; i++) {
        pointer(records, i * 0x38, text(f, classes[i])); pointer(records, i * 0x38 + 8, text(f, ""));
    }
    pointer(pointers, 8, (void *)0x10);
    pointer(objects, 2 * 16 + 8, (void *)0x20); pointer(objects, 3 * 16 + 8, (void *)0x30);
    void *fields = allocate(f, 5 * 0x48); pointer(records, 0x38 + 0x20, fields);
    const char *names[] = {"displayNameTag", "displayDescriptionTag", "prose", "unknown"};
    for (size_t i = 0; i < 4; i++) {
        pointer(fields, i * 0x48, text(f, i == 2 ? "idStr" : "idStrId"));
        pointer(fields, i * 0x48 + 8, text(f, ""));
        pointer(fields, i * 0x48 + 0x10, text(f, names[i])); number(fields, i * 0x48 + 0x1c, 8);
    }
}
static int absent(void *context, const char *path, unsigned char **body, size_t *length)
{ (void)context; (void)path; *body = NULL; *length = 0; return 0; }

#define ROWS 8192
static unsigned char dictionary_rows[ROWS][32];
static struct { void *rows; unsigned count, capacity; } dictionary = {dictionary_rows, 0, ROWS};
static char *pool[ROWS * 4]; static size_t pool_count;
static uint32_t native_hash(const char *key)
{
    /* Deliberately force distinct proposed names onto an occupied NATIVE ID. */
    if (!strcmp(key, "#str_stock") || (!strncmp(key, "#str_smp_loc_", 13) && strlen(key) == 77)) return 0x13579bdf;
    if (!strcmp(key, "#str_user_pending") || !strcmp(key, "#str_probe_user")) return 0x24681234;
    if (!strcmp(key, "#str_smp_render_distance") || !strcmp(key, "#str_probe_baked")) return 0x35792345;
    if (!strcmp(key, "#foreign_native") || !strcmp(key, "#str_probe_foreign")) return 0x468a3456;
    if (!strcmp(key, "#str_pending_global") || !strcmp(key, "#str_probe_pending")) return 0x579b4567;
    uint32_t hash = 2166136261u;
    for (; *key; key++) {
        unsigned char c = (unsigned char)*key;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}
static void intern(void *out, const char *value)
{
    assert(pool_count < ROWS * 4); pool[pool_count] = _strdup(value); assert(pool[pool_count]);
    memcpy(out, pool + pool_count++, 8);
}
static int insert(void *table, void *row)
{
    assert(table == &dictionary && dictionary.count < ROWS);
    memcpy(dictionary_rows[dictionary.count++], row, 32); return (int)dictionary.count - 1;
}
static int row_order(const void *a, const void *b)
{
    uint32_t left, right; memcpy(&left, a, 4); memcpy(&right, b, 4);
    return left < right ? -1 : left > right;
}
static void sort_rows(void *context, void *rows, unsigned count, unsigned radix)
{ assert(context == &dictionary && radix == 0x20); qsort(rows, count, 32, row_order); }
static const char *value_of(const char *key)
{
    const char *result = NULL;
    for (unsigned i = 0; i < dictionary.count; i++) {
        const char *name = *(const char **)(dictionary_rows[i] + 8);
        if (name && !strcmp(name, key)) { assert(!result); result = *(const char **)(dictionary_rows[i] + 16); }
    }
    return result;
}
static const char *locale_order[] = {"fr", "en"};
static size_t locale_count = 1;
static int fail_fallback;
static int locales(void *context, const char *const **order, size_t *count, char *error, size_t capacity)
{ (void)context; (void)error; (void)capacity; *order = locale_order; *count = locale_count; return 1; }
static int native_original(void *context, const char *key, unsigned char **body, size_t *length,
    char *error, size_t capacity)
{
    (void)context; (void)error; (void)capacity;
    assert(!strcmp(key, "#str_title") || !strcmp(key, "#str_onlyfrench"));
    if (fail_fallback) return 0;
    const char *text = !strcmp(key, "#str_title") ? "Original\x97" : "Native empty fallback";
    *length = strlen(text); *body = malloc(*length); assert(*body); memcpy(*body, text, *length); return 1;
}
static size_t owner_named(const sh_package_sources *sources, const char *name)
{
    for (size_t i = 0; i < sources->package_count; i++) if (!strcmp(sources->packages[i].name, name)) return i;
    assert(0); return SIZE_MAX;
}
static void assert_locale(const sh_package_compilation *compiled, const char *locale)
{
    for (size_t i = 0; i < compiled->policy.localized_count; i++) {
        const sh_package_localized_string *entry = compiled->policy.localized + i;
        sh_json_object mapping = {0}, translations = {0}; char expected[128]; size_t length;
        assert(sh_json_parse_object(entry->mapping, strlen(entry->mapping), 4, &mapping));
        const char *raw = sh_json_object_get(&mapping, "locales");
        assert(sh_json_parse_object(raw, strlen(raw), 2, &translations));
        raw = sh_json_object_get(&translations, locale);
        if (raw) assert(sh_json_decode_string(raw, strlen(raw), expected, sizeof(expected), &length));
        else {
            raw = sh_json_object_get(&mapping, "native");
            assert(sh_json_decode_string(raw, strlen(raw), expected, sizeof(expected), &length));
            strcpy(expected, !strcmp(expected, "#str_title") ? "Original\x97" : "Native empty fallback");
        }
        assert(value_of(entry->key) && !strcmp(value_of(entry->key), expected));
        sh_json_object_free(&mapping); sh_json_object_free(&translations);
    }
}

int main(void)
{
    char temporary[MAX_PATH], path[1024], error[1024];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-localization-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    const char *descriptor = "{\"id\":\"same\",\"name\":\"A\",\"strings\":{\"en\":{\"title\":\"Same\"},"
        "\"fr\":{\"title\":\"Rouge\",\"onlyFrench\":\"\"}}}";
    create("overrides/a/package.json", descriptor); create("overrides/c/package.json", descriptor);
    create("overrides/b/package.json", "{\"id\":\"same\",\"name\":\"B\",\"strings\":{\"en\":{\"title\":\"Same\"},"
        "\"fr\":{\"title\":\"Vert\",\"onlyFrench\":\"\"}}}");
    create("overrides/a/child/package.json", "{\"id\":\"child\",\"name\":\"Child\",\"strings\":{\"fr\":{\"title\":\"Enfant\"}}}");
    create("overrides/a/sibling/package.json", "{\"id\":\"sibling\",\"name\":\"Sibling\",\"strings\":{\"en\":{\"title\":\"Wrong\"}}}");
    const char *body = "{ class = \"Entity\"; edit = { displayNameTag = \"#str_title\"; "
        "displayDescriptionTag = \"#str_onlyFrench\"; prose = \"#str_title\x97\"; unknown = \"#str_unknown\"; } }";
    const char *folders[] = {"a", "b", "c"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/root.decl", folders[i]); create(path, body);
    }
    create("overrides/a/child/assets/generated/decls/entitydef/child.decl",
        "{ inherit = \"root\"; edit = { displayDescriptionTag = \"#str_title\"; } }");
    snprintf(path, sizeof(path), "%s/missing-user.json", root); assert(sh_strids_set_source(path));
    number(dictionary_rows[0], 0, (int)native_hash("#str_stock"));
    intern(dictionary_rows[0] + 8, "#str_stock"); intern(dictionary_rows[0] + 16, "Stock"); dictionary.count = 1;
    number(dictionary_rows[1], 0, (int)native_hash("#foreign_native"));
    intern(dictionary_rows[1] + 8, "#foreign_native"); intern(dictionary_rows[1] + 16, "Foreign"); dictionary.count++;
    assert(sh_strids_test_inject(&dictionary, insert, native_hash, intern) > 0);
    sh_strids_test_set_sort(sort_rows);
    reflection_fixture reflection = {0}; setup_reflection(&reflection);
    sh_decl_registry_source registry = {&reflection, read_memory, (uintptr_t)reflection.registry, NULL};
    sh_package_compile_environment environment = {0}; environment.baseline = absent;
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library library = {0}, repeated = {0}, legacy = {0};
    sh_package_library_native native = {0};
    assert(!sh_package_library_compile(sources, &environment, NULL, absent, NULL, registry,
        (uintptr_t)reflection.reflection, &native, &library, error, sizeof(error)));
    if (!strstr(error, "native ID reservation")) fprintf(stderr, "reservation precondition: %s\n", error);
    assert(!library.compiled && strstr(error, "native ID reservation"));
    sh_strids_reservation *reservation = sh_strids_reservation_open(error, sizeof(error)); assert(reservation);
    create("pending-user.json", "{\"user_pending\":\"Not yet published\"}");
    snprintf(path, sizeof(path), "%s/pending-user.json", root); assert(sh_strids_set_source(path));
    sh_strids_reservation_free(reservation);
    reservation = sh_strids_reservation_open(error, sizeof(error)); assert(reservation);
    native.strings = sh_strids_reservation_adapter(reservation, 8);
    const char *probe = "{\"locales\":{\"fr\":\"Probe\"},\"native\":\"#str_probe\"}";
    assert(!native.strings.reserve(native.strings.context, "#str_probe_user", NULL, probe));
    assert(!native.strings.reserve(native.strings.context, "#str_probe_baked", NULL, probe));
    assert(!native.strings.reserve(native.strings.context, "#str_probe_foreign", NULL, probe));
    sh_package_policy pending = {0};
    assert(sh_json_parse_object("{\"en\":{\"pending_global\":\"Pending\"}}", 35, 4, &pending.strings));
    assert(sh_strids_reservation_policy(reservation, &pending, error, sizeof(error)));
    assert(!native.strings.reserve(native.strings.context, "#str_probe_pending", NULL, probe));
    sh_package_policy_free(&pending);
    int ok = sh_package_library_compile(sources, &environment, NULL, absent, NULL, registry,
        (uintptr_t)reflection.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "localized library: %s\n", error);
    assert(ok && library.compiled->policy.localized_count == 4);
    assert(sh_strids_reservation_policy(reservation, &library.compiled->policy, error, sizeof(error)));
    assert(library.compiled->resource_count == 3); /* Two root mappings and the nested child. */
    size_t roots = 0, children = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *resource = library.compiled->resources + i;
        assert(strstr((const char *)resource->body, "#str_smp_loc_"));
        if (strstr(resource->original_path, "/root.decl")) {
            roots++; assert(strstr((const char *)resource->body, "#str_title\x97"));
            assert(strstr((const char *)resource->body, "#str_unknown"));
        } else { children++; assert(strstr(resource->original_path, "/child.decl")); }
    }
    assert(roots == 2 && children == 1);
    for (size_t i = 0; i < library.compiled->policy.localized_count; i++) {
        const char *key = library.compiled->policy.localized[i].key;
        assert(strlen(key) == 79 && !strcmp(key + 77, "_1") && native_hash(key) != native_hash("#str_stock"));
    }
    assert(sh_strids_reservation_validate(reservation, error, sizeof(error)));
    assert(!strcmp(value_of("#str_stock"), "Stock"));
    published = library.compiled;
    sh_strids_localization language = {NULL, locales, native_original}; sh_strids_set_localization(&language);
    assert(sh_strids_rearm()); assert_locale(published, "fr");
    unsigned count = dictionary.count;
    assert(sh_strids_rearm() && dictionary.count == count);
    locale_order[0] = "untranslated"; assert(sh_strids_rearm()); assert_locale(published, "untranslated");
    locale_count = 2; assert(sh_strids_rearm()); assert_locale(published, "en");
    fail_fallback = 1; assert(!sh_strids_rearm() && dictionary.count == count); assert_locale(published, "en");
    fail_fallback = 0; locale_order[0] = "fr"; locale_count = 1; assert(sh_strids_rearm());
    assert(sh_strids_reservation_validate(reservation, error, sizeof(error)));
    sh_strids_reservation_free(reservation);
    reservation = sh_strids_reservation_open(error, sizeof(error)); assert(reservation);
    native.strings = sh_strids_reservation_adapter(reservation, 8);
    assert(sh_package_library_compile(sources, &environment, NULL, absent, NULL, registry,
        (uintptr_t)reflection.reflection, &native, &repeated, error, sizeof(error)));
    assert(repeated.compiled->policy.localized_count == 4);
    for (size_t i = 0; i < 4; i++) assert(!strcmp(library.compiled->policy.localized[i].key, repeated.compiled->policy.localized[i].key));
    /* A new native occupant after capture invalidates the pending transaction. */
    const char *first_key = repeated.compiled->policy.localized[0].key;
    number(dictionary_rows[dictionary.count], 0, (int)native_hash(first_key));
    intern(dictionary_rows[dictionary.count] + 8, "#str_late_native"); dictionary.count++;
    assert(!sh_strids_reservation_validate(reservation, error, sizeof(error))); dictionary.count--;
    assert(sh_strids_reservation_validate(reservation, error, sizeof(error)));
    sh_package_owners selected = {0}; sh_package_policy policy = {0};
    size_t owner_b = owner_named(sources, "b"); assert(sh_package_owners_add(&selected, owner_b));
    assert(sh_package_compilation_policy(library.compiled, &selected, &policy, error, sizeof(error)));
    assert(policy.localized_count == 2); sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    sh_package_provenance historical = {0};
    assert(sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, owner_b), 1, NULL) == 0);
    ok = sh_package_library_compile_contexts(sources, &historical, &environment, NULL, absent, NULL,
        registry, (uintptr_t)reflection.reflection, &native, &legacy, error, sizeof(error));
    if (!ok) fprintf(stderr, "historical localization: %s\n", error);
    assert(ok);
    assert(!legacy.compiled->policy.localized_count && sh_json_object_get(&legacy.compiled->policy.strings, "en"));
    assert(strstr((const char *)legacy.compiled->resources[0].body, "#str_title"));
    sh_package_compilation *overlay = sh_package_compilation_overlay(library.compiled, legacy.compiled, error, sizeof(error));
    assert(overlay && overlay->policy.localized_count == 4);
    assert(sh_package_owners_contains(&overlay->policy.legacy_string_owners, sources->package_count + owner_b));
    sh_package_compilation_free(overlay); sh_package_provenance_free(&historical);
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) assert(sh_package_source_verify(sources->files + i));
    /* Production adapter reads the registered native language and preserves the
     * original table value underneath an explicit user override. */
    unsigned char cvar[0x50] = {0}, system[0x18] = {0};
    void *cvars[] = {cvar}, *system_pointer = system;
    pointer(cvar, 0x40, "sys_lang"); pointer(cvar, 0x10, "french");
    pointer(system, 8, cvars); number(system, 0x10, 1);
    assert(sh_strids_bind_native_localization(&system_pointer));
    number(dictionary_rows[dictionary.count], 0, (int)native_hash("#str_title"));
    intern(dictionary_rows[dictionary.count] + 8, "#str_title");
    intern(dictionary_rows[dictionary.count++] + 16, "Native before reload");
    create("native-user.json", "{\"title\":\"User title\"}");
    snprintf(path, sizeof(path), "%s/native-user.json", root); assert(sh_strids_set_source(path));
    assert(sh_strids_rearm()); assert_locale(published, "fr");
    assert(!strcmp(value_of("#str_title"), "User title"));
    pointer(cvar, 0x10, "untranslated");
    assert(sh_strids_rearm()); assert_locale(published, "fr"); /* Merely changing the cvar did not reload. */
    memset(dictionary_rows, 0, sizeof(dictionary_rows)); dictionary.count = 2;
    number(dictionary_rows[0], 0, (int)native_hash("#str_stock"));
    intern(dictionary_rows[0] + 8, "#str_stock"); intern(dictionary_rows[0] + 16, "Stock");
    number(dictionary_rows[1], 0, (int)native_hash("#str_title"));
    intern(dictionary_rows[1] + 8, "#str_title"); intern(dictionary_rows[1] + 16, "New native\x95");
    assert(sh_strids_test_native_sort()); assert(!strcmp(value_of("#str_title"), "User title"));
    for (size_t i = 0; i < library.compiled->policy.localized_count; i++) {
        const sh_package_localized_string *entry = library.compiled->policy.localized + i;
        sh_json_object mapping = {0}; char native_key[80]; size_t length;
        assert(sh_json_parse_object(entry->mapping, strlen(entry->mapping), 4, &mapping));
        const char *raw = sh_json_object_get(&mapping, "native");
        assert(sh_json_decode_string(raw, strlen(raw), native_key, sizeof(native_key), &length));
        const char *expected = !strcmp(native_key, "#str_title") ? "New native\x95" : "";
        if (!value_of(entry->key) || strcmp(value_of(entry->key), expected))
            fprintf(stderr, "native fallback for %s: '%s' expected '%s'\n", native_key,
                value_of(entry->key) ? value_of(entry->key) : "<missing>", expected);
        assert(value_of(entry->key) && !strcmp(value_of(entry->key), expected));
        sh_json_object_free(&mapping);
    }
    assert(sh_strids_rearm()); /* Repeating refresh does not recapture the overridden native row. */
    sh_strids_bind_native_localization(NULL);
    published = NULL; assert(sh_strids_rearm());
    for (size_t i = 0; i < library.compiled->policy.localized_count; i++) assert(!value_of(library.compiled->policy.localized[i].key));
    assert(!strcmp(value_of("#str_stock"), "Stock"));
    sh_strids_reservation_free(reservation); sh_package_library_free(&legacy);
    sh_package_library_free(&repeated); sh_package_library_free(&library); sh_package_sources_free(sources);
    /* Release injector-owned copies before freeing the synthetic native pool. */
    sh_strids_test_inject(&dictionary, insert, native_hash, intern);
    while (pool_count) free(pool[--pool_count]);
    while (reflection.count) free(reflection.regions[--reflection.count].address);
    cleanup(); assert(!failures);
    puts("scoped localization binding, native ID reservation, language and fallback checks passed"); return 0;
}
