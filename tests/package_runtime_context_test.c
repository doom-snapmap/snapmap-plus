/* Real runtime provider transactions with independent library and map views.
 * Native activation is modeled here; renderer acceptance is separate. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#define PACKAGE_FIXTURE_ENTRIES 1024u
#include "package_fixture.h"
#include "hook.h"
#include "map_provenance.h"
#include "map_embed.h"

static void *(*atlas_lookup)(void *, void *);
static void *atlas_native_lookup(void *owner, void *material)
{ (void)owner; return material ? *(void **)((unsigned char *)material + 0x170) : NULL; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    CHECK(target == (void *)0x1234 && stolen == 16);
    atlas_lookup = (void *(*)(void *, void *))detour; return atlas_native_lookup;
}
sh_patch_status hook_commit(void *trampoline)
{ CHECK(trampoline == atlas_native_lookup); return B2_PATCH_OK; }

static const char *health_path = "generated/decls/entitydef/ai/cyberdemon.decl";
static const char *descriptor = "{\"id\":\"installed-bosses\",\"name\":\"Installed Bosses\",\"strings\":{\"en\":{\"label\":\"local\"}}}";
static const char *original = "{ edit = { health = 25000; } }";
void backend_log(const char *message) { (void)message; }

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, "stock.bin") || !strcmp(path, "campaign.bin")) {
        *length = 8; *body = (unsigned char *)_strdup("original");
        return !*body ? -1 : !strcmp(path, "stock.bin") ? 1 : 2;
    }
    if (strcmp(path, health_path)) return 0;
    *length = strlen(original); *body = (unsigned char *)_strdup(original);
    return *body ? 2 : -1;
}

/* Native metadata for a small typed declaration family. The admission walk
 * reads these registrations, never a locally loaded declaration's state. */
typedef struct admission_region { void *address; size_t length; } admission_region;
typedef struct admission_fixture {
    admission_region regions[128]; size_t count;
    void *registry, *reflection;
} admission_fixture;
static void *admission_alloc(admission_fixture *f, size_t length)
{
    void *p = calloc(1, length); CHECK(p && f->count < 128);
    if (p && f->count < 128) f->regions[f->count++] = (admission_region){p, length};
    return p;
}
static void admission_pointer(void *p, size_t at, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)p + at, &address, 8); }
static void admission_number(void *p, size_t at, int value)
{ memcpy((char *)p + at, &value, 4); }
static char *admission_string(admission_fixture *f, const char *text)
{ char *copy = admission_alloc(f, strlen(text) + 1); if (copy) strcpy(copy, text); return copy; }
static int admission_read(void *context, uintptr_t address, void *out, size_t length)
{
    admission_fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static void admission_setup(admission_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "OpaqueStringReader", "idDeclTypeInfo", "int"};
    const char *types[] = {"idDeclEntityDef", "OpaqueStringReader", "int"};
    const char *ops[] = {"*", "", ""}, *names[] = {"target", "custom", "health"};
    void *manager, *managers, *container, *records, *objects, *pointers, *fields;
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18); managers = admission_alloc(f, 8);
    manager = admission_alloc(f, 0x90); admission_pointer(managers, 0, manager);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 1);
    admission_pointer(manager, 8, admission_string(f, "entityDef"));
    admission_pointer(manager, 0x10, admission_string(f, classes[0]));
    f->reflection = admission_alloc(f, 0xb0); container = admission_alloc(f, 0x30);
    records = admission_alloc(f, 6 * 0x38); objects = admission_alloc(f, 6 * 16);
    pointers = admission_alloc(f, 6 * 16); fields = admission_alloc(f, 4 * 0x48);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 6);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 6);
    for (size_t i = 0; i < 5; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, ""));
    }
    admission_pointer(pointers, 8, (void *)0x10);
    admission_pointer(objects, 2 * 16 + 8, (void *)0x20);
    admission_pointer(records, 0x38 + 0x20, fields);
    for (size_t i = 0; i < 3; i++) {
        admission_pointer(fields, i * 0x48, admission_string(f, types[i]));
        admission_pointer(fields, i * 0x48 + 8, admission_string(f, ops[i]));
        admission_pointer(fields, i * 0x48 + 0x10, admission_string(f, names[i]));
        admission_number(fields, i * 0x48 + 0x1c, 8);
    }
}

typedef struct admission_activation {
    const char *next, *previous;
    int calls, recoveries, fail, next_map, previous_map;
} admission_activation;
static int admission_activate(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    admission_activation *state = context;
    unsigned char *bytes = NULL; size_t length = 0;
    const char *wanted = restoring ? state->previous : state->next;
    int read = sh_package_runtime_read("generated/decls/entitydef/admission/root.decl", &bytes, &length);
    CHECK(changes && sh_package_runtime_has_map_provider() == (restoring ? state->previous_map : state->next_map));
    CHECK(wanted ? read == 1 && bytes && length == strlen(wanted) && !memcmp(bytes, wanted, length) : read == 0);
    free(bytes);
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "fixture map transition failure"); return 0; }
    return 1;
}

static void test_map_usage_admission(void)
{
    const char *map_root_bytes = "{ class = \"Entity\"; edit = { target = \"admission/child\"; } }";
    const char *local_root_bytes = "{ class = \"Entity\"; edit = { target = \"admission/local-only\"; } }";
    const char *leaf = "{ class = \"Entity\"; edit = {} }";
    char data[MAX_PATH], map[MAX_PATH], error[2048], json[512];
    unsigned char fingerprint[32], *body; size_t length, resource_count, source_count;
    admission_fixture f;
    sh_package_missing missing = {0}; sh_package_references required = {0};
    sh_package_source_graph_report report = {0};
    sh_package_map_plan *plan;
    admission_activation activation = {0};
    const sh_package_compilation *compiled;
    const sh_compiled_resource *authored;
    admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/admission", root);
    snprintf(map, sizeof(map), "%s/admission-map", root);
    create("admission/overrides/a/package.json", "{\"id\":\"local-a\",\"name\":\"Local A\"}");
    create("admission/overrides/b/package.json", "{\"id\":\"local-b\",\"name\":\"Local B\"}");
    create("admission/overrides/a/assets/generated/decls/entitydef/admission/root.decl", local_root_bytes);
    create("admission/overrides/a/assets/generated/decls/entitydef/admission/local-only.decl", leaf);
    create("admission/overrides/a/assets/generated/image/admission/local_only.bimage", "local image bytes");
    create("admission/overrides/b/assets/generated/decls/entitydef/admission/child.decl",
        "{ class = \"Entity\"; edit = { custom = \"local-only reader\"; } }");
    create("admission-map/overrides/map/package.json", "{\"id\":\"intact-map\",\"name\":\"Intact Map\"}");
    create("admission-map/overrides/map/assets/generated/decls/entitydef/admission/root.decl", map_root_bytes);
    create("admission-map/overrides/map/assets/generated/decls/entitydef/admission/child.decl", leaf);
    create("admission-map/overrides/map/assets/generated/decls/entitydef/admission/unused.decl", leaf);
    create("admission-map/overrides/map/assets/generated/decls/entitydef/admission/opaque.decl",
        "{ class = \"Entity\"; edit = { custom = \"requires an adapter\"; } }");
    create("admission-map/overrides/map/assets/generated/decls/snapeditorentitydef/admission/preview.decl", "{ preview = 1; }");
    create("admission-map/overrides/map/assets/unused.bimage", "unused image bytes");
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error)); CHECK(plan);
    compiled = sh_package_map_plan_compilation(plan); CHECK(compiled && compiled->resource_count >= 6);
    if (!compiled) goto done;
    resource_count = compiled->resource_count; source_count = compiled->sources->file_count;
    CHECK(sh_package_compilation_find(compiled, "generated/decls/entitydef/admission/unused.decl"));
    CHECK(sh_package_compilation_find(compiled, "generated/decls/entitydef/admission/opaque.decl"));
    CHECK(sh_package_compilation_find(compiled, "generated/decls/snapeditorentitydef/admission/preview.decl"));
    CHECK(sh_package_compilation_find(compiled, "unused.bimage"));
    memcpy(fingerprint, compiled->sources->fingerprints[0], sizeof(fingerprint));
    authored = sh_package_compilation_find(compiled, "generated/decls/entitydef/admission/root.decl"); CHECK(authored);
    /* Full bundle delivery contains unavailable extras, while this map's typed
     * root and child are installed under different owners and different values. */
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 4);
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/root\"}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)));
    CHECK(missing.checked == 2 && !missing.count && !report.gaps);
    CHECK(sh_package_map_plan_source_resources(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &required, &report, error, sizeof(error)) && required.count == 2);
    for (size_t i = 0; i < required.count; i++) CHECK(!strstr(required.items[i].name, "local-only"));
    /* A used missing member selects its whole supplying owner, never a subset. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/unused\"}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)));
    CHECK(missing.checked == 1 && missing.count == 1 && sh_package_owners_count(&missing.packages) == 1);
    CHECK(missing.count && strstr(missing.paths[0], "admission/unused.decl"));
    CHECK(compiled->resource_count == resource_count && compiled->sources->file_count == source_count);
    CHECK(!memcmp(fingerprint, compiled->sources->fingerprints[0], sizeof(fingerprint)));
    body = sh_package_compilation_read(compiled, authored, 4096, &length, error, sizeof(error));
    CHECK(body && length == strlen(map_root_bytes) && !memcmp(body, map_root_bytes, length)); free(body);
    /* This source exists only in the local library: it must not repair the map. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/local-only\"}");
    CHECK(!sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)));
    CHECK(report.gaps && strstr(error, "coverage is incomplete") && !missing.count && !missing.checked);
    strcpy(json, "{\"targetType\":\"idImage\",\"value\":\"admission/local_only\"}");
    CHECK(!sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)));
    CHECK(report.gaps && strstr(error, "resource sources are absent") && !missing.count);
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/opaque\"}");
    CHECK(!sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)));
    CHECK(report.gaps && strstr(error, "coverage is incomplete") && !missing.count);
    /* Inline typed state is inspected too. Ordinary native byte strings and
     * editor-only roots neither add dependencies nor change the input bytes. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"className\":\"Entity\",\"state\":{\"edit\":{\"target\":\"admission/child\"}}}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && missing.checked == 1 && !missing.count);
    strcpy(json, "{\"message\":\"native\x97" "bytes\",\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/opaque\"}}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && !missing.checked && !report.gaps);
    CHECK((unsigned char)json[18] == 0x97);
    CHECK(!sh_package_map_plan_usage_missing(plan, registry, 0,
        json, strlen(json), &missing, &report, error, sizeof(error)) && !missing.count);
    CHECK(!sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        "{", 1, &missing, &report, error, sizeof(error)) && !missing.count);
    /* Keep the checked use set with the map, then rescan against physical
     * inventory. Unused incoming members remain absent throughout. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/root\"}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && !missing.count);
    activation.next = map_root_bytes; activation.previous = local_root_bytes;
    activation.next_map = 1;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    activation.previous = map_root_bytes; activation.previous_map = 1;
    sh_package_references observed = {0};
    CHECK(sh_package_references_add(&observed, "", "generated/decls/entitydef/admission/root.decl"));
    CHECK(sh_package_runtime_select_map(json, strlen(json), &observed, error, sizeof(error)));
    const sh_package_compilation *prior_library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    create("admission/overrides/a/package.json", "malformed committed source");
    sh_package_runtime_note_sources_changed();
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    sh_package_runtime_error(error, sizeof(error));
    CHECK(strstr(error, "required map resource") && activation.calls == 1 && !activation.recoveries);
    CHECK(sh_package_runtime_has_map_provider());
    create("admission/overrides/a/package.json", "{\"id\":\"local-a\",\"name\":\"Local A\"}");
    /* Conflicting authoring candidates still provide healthy installed files.
     * They need not become the selected local values for map availability. */
    create("admission/overrides/c/package.json", "{\"id\":\"local-c\",\"name\":\"Local C\"}");
    create("admission/overrides/c/assets/generated/decls/entitydef/admission/root.decl",
        "{ class = \"Entity\"; edit = { target = \"admission/other-local\"; } }");
    activation.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    CHECK(activation.calls == 2 && activation.recoveries == 1);
    const sh_package_compilation *after = sh_package_runtime_library_acquire();
    CHECK(after == prior_library); sh_package_runtime_release();
    activation.fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    CHECK(activation.calls == 3 && activation.recoveries == 1);
    after = sh_package_runtime_library_acquire();
    CHECK(after != prior_library && !sh_package_compilation_find(after, "generated/decls/entitydef/admission/root.decl"));
    sh_package_runtime_release();
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 4);
    /* A changed selection invalidates the old proof. Fresh inspection follows
     * the new used member, which really is unavailable in the local inventory. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/unused\"}");
    CHECK(sh_package_runtime_select_map(json, strlen(json), NULL, error, sizeof(error)));
    sh_package_runtime_note_sources_changed();
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    sh_package_runtime_error(error, sizeof(error));
    CHECK(strstr(error, "coverage is incomplete") && activation.calls == 3);
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && missing.count == 1);
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    sh_package_runtime_error(error, sizeof(error));
    CHECK(strstr(error, "admission/unused.decl") && activation.calls == 3);
    /* Inspecting a hypothetical earlier use cannot certify the still-selected
     * edited map until that earlier selection actually becomes current. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/root\"}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && !missing.count);
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    sh_package_runtime_error(error, sizeof(error)); CHECK(strstr(error, "coverage is incomplete"));
    /* An extra observed dependency invalidates even identical map bytes. */
    strcpy(json, "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"admission/root\"}");
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)f.reflection,
        json, strlen(json), &missing, &report, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_references_add(&observed, "", "generated/decls/entitydef/admission/unused.decl"));
    CHECK(sh_package_runtime_select_map(json, strlen(json), &observed, error, sizeof(error)));
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, admission_activate, &activation));
    sh_package_runtime_error(error, sizeof(error)); CHECK(strstr(error, "coverage is incomplete"));
    sh_package_references_free(&observed);
    /* Leaving for vanilla is not new admission of the old edited map. Retain
     * its exact bytes through failed activation/recovery, then release it. */
    activation.next = NULL; activation.next_map = 0; activation.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, admission_activate, &activation));
    CHECK(activation.calls == 4 && activation.recoveries == 2 && sh_package_runtime_has_map_provider());
    activation.fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, admission_activate, &activation));
    CHECK(activation.calls == 5 && !sh_package_runtime_has_map_provider());
done:
    sh_package_references_free(&required); sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    while (f.count) free(f.regions[--f.count].address);
}

static void expect_current(const char *path, const char *wanted)
{
    unsigned char *bytes = NULL; size_t length = 0;
    int result = sh_package_runtime_read(path, &bytes, &length);
    if (!(wanted ? result == 1 && bytes && strstr((const char *)bytes, wanted) : result == 0))
        fprintf(stderr, "resource %s: wanted %s; got %d: %.160s\n", path, wanted ? wanted : "absent", result,
            bytes ? (const char *)bytes : "(no bytes)");
    CHECK(wanted ? result == 1 && bytes && strstr((const char *)bytes, wanted) : result == 0);
    free(bytes);
}

static const sh_package_compilation *expect_library(const char *health)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *resource = sh_package_compilation_find(library, health_path);
    char error[512];
    CHECK(library && !library->map_overlay);
    CHECK(resource && resource->body && strstr((const char *)resource->body, health));
    CHECK(sh_package_compilation_probe(library, "map-only.bimage", error, sizeof(error)) == 0);
    sh_package_runtime_release(); return library;
}

typedef struct activation {
    const char *next_health, *previous_health, *library_health;
    const sh_package_compilation *previous, *library;
    int fail, calls, recoveries, map_active, previous_map_active, library_changed;
    const sh_package_change *expected;
    size_t expected_count;
} activation;

static int activate(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    activation *state = context;
    if (state->expected) {
        CHECK(changes->count == state->expected_count);
        for (size_t i = 0; i < changes->count && i < state->expected_count; i++) {
            sh_package_change_kind wanted = state->expected[i].kind;
            if (restoring && wanted == SH_PACKAGE_RESOURCE_ADDED) wanted = SH_PACKAGE_RESOURCE_REMOVED;
            else if (restoring && wanted == SH_PACKAGE_RESOURCE_REMOVED) wanted = SH_PACKAGE_RESOURCE_ADDED;
            CHECK(!strcmp(changes->items[i].path, state->expected[i].path));
            CHECK(changes->items[i].kind == wanted);
        }
    }
    const sh_package_compilation *current = sh_package_runtime_acquire();
    CHECK(restoring ? current == state->previous : current != state->previous);
    sh_package_runtime_release();
    CHECK(!sh_package_runtime_ready());
    CHECK(!sh_package_runtime_admission_ready());
    CHECK(sh_package_runtime_has_map_provider() == (restoring ? state->previous_map_active : state->map_active));
    expect_current(health_path, restoring ? state->previous_health : state->next_health);
    /* A committed installation rescans the library in the same transaction, so
     * the activation pass sees a new library; recovery restores the old one. */
    if (state->library_changed && !restoring) CHECK(expect_library(state->library_health) != state->library);
    else CHECK(expect_library(state->library_health) == state->library);
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail == 2) RaiseException(0xe04d504e, 0, 0, NULL);
    if (state->fail) { snprintf(error, capacity, "fixture map activation failure"); return 0; }
    return 1;
}

static int switch_map(const char *map, activation *state)
{
    state->previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    state->library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state->previous_map_active = sh_package_runtime_has_map_provider();
    return sh_package_runtime_activate_map(root, map, (sh_package_activation_guard){0}, activate, state);
}

static int activate_plan(sh_package_map_plan *plan, activation *state)
{
    state->previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    state->library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state->previous_map_active = sh_package_runtime_has_map_provider();
    return sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, activate, state);
}

static void test_prepared_map(const char *map_root)
{
    static const char *paths[] = {
        "generated/decls/material/prepared.decl", "generated/decls/fx/prepared.decl",
        "generated/decls/sound/prepared.decl", "generated/decls/aimodule/prepared.decl",
        "generated/rendermodels/prepared.bmodel", "generated/md6/prepared.md6mesh",
        "generated/md6/prepared.md6anim", "generated/images/prepared.bimage",
        "generated/spirv/prepared.vspv", "generated/glprogs/prepared.bprog",
        "sound/soundbanks/prepared.bnk", "sound/streams/prepared.wem",
        "future/engine-family/prepared.bytes"
    };
    const char *map_descriptor = "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\"}";
    const char *local_bytes = "{ value = \"local\"; }", *map_bytes = "{ value = \"prepared\"; }";
    char path[4096], error[2048], previous_error[2048];
    sh_package_map_plan *plan, *cancelled;
    const sh_package_compilation *current, *candidate;
    activation state = {0};
    create("map-a/overrides/map/package.json", map_descriptor);
    create("map-a/overrides/map/assets/campaign.bin", "map campaign bytes");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        snprintf(path, sizeof(path), "overrides/local/assets/%s", paths[i]); create(path, local_bytes);
        snprintf(path, sizeof(path), "map-a/overrides/map/assets/%s", paths[i]); create(path, map_bytes);
    }
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    current = sh_package_runtime_acquire(); sh_package_runtime_release();
    sh_package_runtime_error(previous_error, sizeof(previous_error));
    CHECK(!sh_package_runtime_prepare_map(root, "", error, sizeof(error)));
    CHECK(!sh_package_runtime_prepare_map(NULL, map_root, NULL, 0));
    create("map-a/overrides/map/package.json", "malformed source");
    CHECK(!sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)));
    CHECK(error[0] && sh_package_runtime_ready());
    sh_package_runtime_error(error, sizeof(error)); CHECK(!strcmp(error, previous_error));
    const sh_package_compilation *after = sh_package_runtime_acquire(); CHECK(after == current); sh_package_runtime_release();
    create("map-a/overrides/map/package.json", map_descriptor);
    cancelled = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(cancelled);
    CHECK(!sh_package_runtime_has_map_provider());
    sh_package_map_plan_free(cancelled); sh_package_map_plan_free(NULL);
    CHECK(sh_package_runtime_ready());
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    candidate = sh_package_map_plan_compilation(plan); CHECK(candidate && !candidate->map_overlay);
    {
        const char *requirements[] = {health_path, "stock.bin", "campaign.bin", "map-only.bimage"};
        sh_package_missing missing = {0};
        CHECK(sh_package_map_plan_missing(plan, requirements, 4, &missing, error, sizeof(error)));
        CHECK(missing.checked == 4 && missing.count == 2 && sh_package_owners_count(&missing.packages) == 1);
        CHECK(!strcmp(missing.paths[0], "campaign.bin") && !strcmp(missing.paths[1], "map-only.bimage"));
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        CHECK(missing.count == 2 && !strcmp(missing.paths[0], "campaign.bin") &&
            !strcmp(missing.paths[1], "map-only.bimage"));
        CHECK(!sh_package_map_plan_payload_missing(NULL, &missing, error, sizeof(error)));
        CHECK(!missing.count && !missing.paths && !missing.checked);
        CHECK(!sh_package_map_plan_missing(NULL, requirements, 4, &missing, error, sizeof(error)));
        CHECK(!missing.count && !missing.paths); sh_package_missing_free(&missing);
    }
    CHECK(!sh_package_map_plan_compilation(NULL));
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        const sh_compiled_resource *resource = sh_package_compilation_find(candidate, paths[i]);
        unsigned char *bytes; size_t length = 0;
        CHECK(resource); bytes = sh_package_compilation_read(candidate, resource, SIZE_MAX, &length, error, sizeof(error));
        CHECK(bytes && length == strlen(map_bytes) && !memcmp(bytes, map_bytes, length)); free(bytes);
        expect_current(paths[i], "local");
        /* The prepared candidate owns declarations and cached opaque bytes.
         * Changing sources cannot alter the already inspected candidate. */
        snprintf(path, sizeof(path), "map-a/overrides/map/assets/%s", paths[i]); create(path, "changed after preparation");
    }
    create("map-a/overrides/map/package.json", "changed after preparation");
    /* Pending consent can outlive a local refresh. Activation must retain the
     * latest library, not restore whichever library existed at preparation. */
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 16000; } }");
    CHECK(sh_package_runtime_refresh(root));
    state.next_health = "health = 10"; state.previous_health = "health = 16000";
    state.library_health = "health = 16000"; state.map_active = 1;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure; CHECK(!activate_plan(plan, &state));
        CHECK(state.calls == failure && state.recoveries == failure);
        CHECK(sh_package_map_plan_compilation(plan) == candidate);
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "local");
    }
    state.fail = 0; CHECK(activate_plan(plan, &state)); CHECK(sh_package_runtime_ready());
    {
        CHECK(sh_package_runtime_acquire() != NULL);
        const sh_package_provenance *bindings = sh_package_runtime_bindings();
        CHECK(bindings && bindings->context_count == 1 && bindings->binding_count);
        CHECK(bindings->contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED &&
            !memcmp(bindings->contexts[0].trees[0], candidate->sources->fingerprints[0], 32));
        sh_package_runtime_release();
    }
    {
        const char *required[] = {"map-only.bimage"};
        sh_package_missing missing = {0};
        /* Active map bytes cannot certify their own local availability. */
        expect_current("map-only.bimage", "map-only bytes");
        CHECK(sh_package_map_plan_missing(plan, required, 1, &missing, error, sizeof(error)));
        CHECK(missing.count == 1 && sh_package_owners_count(&missing.packages) == 1);
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        CHECK(missing.count == 2); /* Neither active map source counts as installed. */
        sh_package_missing_free(&missing);
    }
    sh_package_map_plan_free(plan); /* Runtime now owns the active map provider. */
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "prepared");
    state.next_health = "health = 16000"; state.previous_health = "health = 10"; state.map_active = 0;
    state.fail = 1; CHECK(!activate_plan(NULL, &state)); CHECK(sh_package_runtime_has_map_provider());
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "prepared");
    state.fail = 0; CHECK(activate_plan(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "local");
}

/* A whole-package installation commits its files into the shared package tree
 * while the temporary map provider is still the only thing supplying them.
 * Retiring that overlay against the pre-install library would report every
 * committed resource as removed and retire content the consumers are using. */
static void test_committed_install_joins_library(void)
{
    static const char *committed_mesh = "delivered.md6mesh";
    static const char *committed_decl = "generated/decls/entitydef/ai/delivered_demon.decl";
    static const char *identity = "{\"id\":\"delivered-demons\",\"name\":\"Delivered Demons\"}";
    static const char *decl_bytes = "{ class = \"Entity\"; edit = { health = 7; } }";
    const sh_package_change installed[] = {
        {"delivered.md6mesh", SH_PACKAGE_RESOURCE_ADDED},
        {"generated/decls/entitydef/ai/delivered_demon.decl", SH_PACKAGE_RESOURCE_ADDED}
    };
    char map_root[MAX_PATH], path[4096], error[2048];
    const sh_package_compilation *library;
    sh_package_map_plan *plan;
    activation state = {0};
    admission_fixture metadata;
    sh_package_missing missing = {0};
    sh_package_source_graph_report report;
    const char *map_json = "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"ai/delivered_demon\"}";
    int calls;
    snprintf(map_root, sizeof(map_root), "%s/map-install", root);
    create("map-install/overrides/map/package.json", identity);
    snprintf(path, sizeof(path), "map-install/overrides/map/assets/%s", committed_mesh);
    create(path, "delivered mesh");
    snprintf(path, sizeof(path), "map-install/overrides/map/assets/%s", committed_decl);
    create(path, decl_bytes);
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    expect_current(committed_mesh, NULL);
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    admission_setup(&metadata);
    sh_decl_registry_source registry = {&metadata, admission_read, (uintptr_t)metadata.registry, NULL};
    CHECK(sh_package_map_plan_usage_missing(plan, registry, (uintptr_t)metadata.reflection,
        map_json, strlen(map_json), &missing, &report, error, sizeof(error)) && missing.count == 1);
    sh_package_missing_free(&missing);
    state.next_health = state.previous_health = state.library_health = "health = 16000";
    state.map_active = 1; state.expected = installed; state.expected_count = 2;
    CHECK(activate_plan(plan, &state)); CHECK(state.calls == 1);
    expect_current(committed_mesh, "delivered mesh");
    CHECK(sh_package_runtime_select_map(map_json, strlen(map_json), NULL, error, sizeof(error)));
    create("overrides/delivered-demons/package.json", identity);
    snprintf(path, sizeof(path), "overrides/delivered-demons/assets/%s", committed_mesh);
    create(path, "delivered mesh");
    snprintf(path, sizeof(path), "overrides/delivered-demons/assets/%s", committed_decl);
    create(path, decl_bytes);
    sh_package_runtime_note_sources_changed();
    /* A rejected committed source cannot readmit a map that still uses it. */
    create("overrides/delivered-demons/package.json", "malformed after commit");
    state.map_active = 1; calls = state.calls;
    CHECK(!activate_plan(plan, &state));
    CHECK(state.calls == calls && !state.recoveries);
    CHECK(sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    create("overrides/delivered-demons/package.json", identity);
    /* Now the committed sources compile: continuing the map makes no resource
     * changes, and a following exit retires the map provider normally. */
    state.library_changed = 1; state.expected_count = 0;
    CHECK(activate_plan(plan, &state)); CHECK(state.calls == calls + 1);
    state.library_changed = 0; state.map_active = 0;
    CHECK(activate_plan(NULL, &state)); CHECK(state.calls == calls + 2);
    sh_package_map_plan_free(plan);
    while (metadata.count) free(metadata.regions[--metadata.count].address);
    CHECK(!sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    expect_current(committed_decl, "health = 7");
    library = sh_package_runtime_library_acquire();
    CHECK(library && sh_package_compilation_find(library, committed_mesh));
    CHECK(sh_package_compilation_find(library, committed_decl));
    sh_package_runtime_release();
    /* The rescan is consumed. A later map transition keeps the same library. */
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    state.library_changed = 0; state.map_active = 1; state.expected_count = 0;
    CHECK(activate_plan(plan, &state));
    state.map_active = 0; CHECK(activate_plan(NULL, &state));
    CHECK(!sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    sh_package_map_plan_free(plan);
}

/* One package identity, two variants: the author's own tree and a map's
 * delivery. The delivery installs beside the author's copy instead of being
 * refused or overwriting it. The author owns every resource they supply, the
 * delivery fills what they do not, and the map's own values govern only while
 * that map is loaded. */
static void test_variant_ownership(void)
{
    static const char *identity = "{\"id\":\"shared-boss\",\"name\":\"Shared boss\"}";
    static const char *scalar = "generated/decls/entitydef/ai/variant_boss.decl";
    static const char *extra = "delivered-only.bimage";
    char map_root[MAX_PATH], error[2048], path[MAX_PATH];
    const sh_package_compilation *library;
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    activation state = {0};
    snprintf(map_root, sizeof(map_root), "%s/map-variant", root);

    /* The author's own package. */
    create("overrides/authored-boss/package.json", identity);
    create("overrides/authored-boss/assets/generated/decls/entitydef/ai/variant_boss.decl",
           "{ edit = { health = 12000; } }");
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    expect_current(scalar, "health = 12000");
    expect_current(extra, NULL);

    /* The same identity delivered by a map: a different scalar and one resource
     * the author does not have, installed where deliveries go. */
    create("overrides/map-00112233445566aa/shared-boss/package.json", identity);
    create("overrides/map-00112233445566aa/shared-boss/assets/generated/decls/entitydef/ai/variant_boss.decl",
           "{ edit = { health = 10; } }");
    create("overrides/map-00112233445566aa/shared-boss/assets/delivered-only.bimage", "delivered bytes");
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    /* No contradiction is reported, the author's value stands, and the new
     * resource is available for later authoring. */
    expect_current(scalar, "health = 12000");
    expect_current(extra, "delivered bytes");
    library = sh_package_runtime_library_acquire();
    CHECK(library && sh_package_compilation_find(library, extra));
    sh_package_runtime_release();

    /* Its own map view still governs transiently. */
    create("map-variant/overrides/shared-boss/package.json", identity);
    create("map-variant/overrides/shared-boss/assets/generated/decls/entitydef/ai/variant_boss.decl",
           "{ edit = { health = 10; } }");
    create("map-variant/overrides/shared-boss/assets/delivered-only.bimage", "delivered bytes");
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    /* Everything this map needs is already available: no installation. */
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    sh_package_missing_free(&missing);
    state.next_health = state.previous_health = state.library_health = "health = 16000";
    state.map_active = 1;
    CHECK(activate_plan(plan, &state));
    expect_current(scalar, "health = 10");
    expect_current(extra, "delivered bytes");
    /* Leaving restores the local policy; the delivered resource stays. */
    state.map_active = 0;
    CHECK(activate_plan(NULL, &state));
    expect_current(scalar, "health = 12000");
    expect_current(extra, "delivered bytes");
    sh_package_map_plan_free(plan);

    /* The author's bytes were never touched. */
    {
        unsigned char body[64] = {0}; size_t length = 0;
        FILE *stream = NULL;
        snprintf(path, sizeof(path), "%s/overrides/authored-boss/assets/%s", root, scalar);
        CHECK(!fopen_s(&stream, path, "rb") && stream);
        if (stream) { length = fread(body, 1, sizeof(body) - 1, stream); fclose(stream); }
        body[length] = 0;
        CHECK(strstr((const char *)body, "12000") != NULL);
    }
}

static int inventory_commit_calls, inventory_commit_failure;
static int commit_inventory(char *error, size_t capacity)
{
    inventory_commit_calls++;
    if (inventory_commit_failure == 2) RaiseException(0xe0422083, 0, 0, NULL);
    if (inventory_commit_failure) { snprintf(error, capacity, "install commit refused"); return 0; }
    return 1;
}

static void test_installed_inventory_is_not_active_composition(void)
{
    char map_root[MAX_PATH], error[2048];
    sh_package_missing missing = {0};
    sh_package_map_plan *plan;
    activation state = {0};
    snprintf(map_root, sizeof(map_root), "%s/map-inventory", root);
    create("map-inventory/overrides/map/package.json", "{\"id\":\"installed-bosses\",\"name\":\"Authored variant\"}");
    create("map-inventory/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("map-inventory/overrides/map/assets/boss.md6mesh", "authored mesh");
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    /* A whole map-installed variant overlaps local gameplay values and shares
     * its id. Storing it must not force those contradictory values to compose. */
    create("overrides/delivered/bosses/package.json", "{\"id\":\"installed-bosses\",\"name\":\"Authored variant\"}");
    create("overrides/delivered/bosses/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("overrides/delivered/bosses/assets/boss.md6mesh", "authored mesh");
    CHECK(sh_package_map_plan_prepare_inventory(plan, root, error, sizeof(error)));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    CHECK(!sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && !inventory_commit_calls);
    state.next_health = "health = 10"; state.previous_health = state.library_health = "health = 16000";
    state.map_active = 1;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure; CHECK(!activate_plan(plan, &state));
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
        expect_current(health_path, "health = 16000");
    }
    state.fail = 0; CHECK(activate_plan(plan, &state));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    for (inventory_commit_failure = 1; inventory_commit_failure <= 2; inventory_commit_failure++) {
        CHECK(!sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && error[0]);
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
        CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
        expect_current(health_path, "health = 10");
    }
    inventory_commit_failure = 0;
    CHECK(!sh_package_runtime_commit_map(NULL, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 2);
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 3);
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 10"); expect_library("health = 16000");
    CHECK(sh_package_runtime_refresh(root));
    CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    /* This physical delivered package intentionally differs from the local
     * authoring package. Its files stay available for map admission. Reconcile
     * the authoring collision before the remaining map-exit assertions. */
    create("overrides/delivered/bosses/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 16000; } }");
    CHECK(sh_package_runtime_refresh(root));
    /* An unrelated rejected local composition does not invalidate the same
     * successfully activated map or require another installation. */
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 10"); expect_library("health = 16000");
    state.next_health = "health = 16000"; state.previous_health = "health = 10"; state.map_active = 0;
    CHECK(activate_plan(NULL, &state));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 16000"); expect_current("boss.md6mesh", "authored mesh");
    /* No installed source changed, and cached active-map data alone never
     * certifies a source file that was subsequently removed or changed. */
    create("overrides/delivered/bosses/assets/boss.md6mesh", "changed externally");
    CHECK(!sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
    CHECK(!missing.count && strstr(error, "installed source changed"));
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
}

static int startup_activation(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    int fail = *(int *)context;
    (void)error; (void)capacity;
    CHECK(!sh_package_runtime_admission_ready());
    if (restoring) return 1;
    return !fail;
}

static void test_conflicting_startup(void)
{
    char map_root[MAX_PATH], error[2048];
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    const sh_package_compilation *library;
    char *summary;
    int fail = 1;
    create("overrides/a/package.json", "{\"id\":\"bosses\",\"name\":\"Original Bosses\"}");
    create("overrides/a/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("overrides/b/package.json", "{\"id\":\"bosses\",\"name\":\"Map Bosses\"}");
    create("overrides/b/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("private/overrides/map/package.json", "{\"id\":\"bosses\",\"name\":\"Map Bosses\"}");
    create("private/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    snprintf(map_root, sizeof(map_root), "%s/private", root);
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(!sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!sh_package_runtime_admission_ready());
    library = sh_package_runtime_library_acquire(); CHECK(!library); sh_package_runtime_release();
    fail = 0;
    CHECK(sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    library = sh_package_runtime_library_acquire();
    CHECK(library && !library->sources->package_count && !sh_package_compilation_find(library, health_path));
    sh_package_runtime_release();
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 2 packages") && strstr(summary, "Skipped local package")); free(summary);
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    expect_current(health_path, "health = 10"); CHECK(sh_package_runtime_admission_ready());
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    expect_current(health_path, NULL); CHECK(sh_package_runtime_admission_ready());
    /* Reconciling the actual source conflict restores the normal complete local
     * composition, without a process reset or a hidden package priority. */
    create("overrides/b/assets/generated/decls/entitydef/ai/cyberdemon.decl", original);
    CHECK(sh_package_runtime_refresh(root)); expect_current(health_path, "health = 12000");
    summary = sh_package_runtime_summary(); CHECK(summary && !strstr(summary, "composition unavailable")); free(summary);
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
}

static void run_conflicting_startup(void)
{
    char executable[MAX_PATH], command[MAX_PATH + 64];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    DWORD result = 1;
    CHECK(GetModuleFileNameA(NULL, executable, sizeof(executable)));
    snprintf(command, sizeof(command), "\"%s\" --conflicting-startup", executable);
    CHECK(CreateProcessA(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
    if (!process.hProcess) return;
    CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &result) && !result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}

static void test_duplicate_editor_controls(void)
{
    const char *path = "generated/decls/snapeditorentitydef/props/dedupe.decl";
    const char *relative = "dedupe/overrides/editor/assets/generated/decls/snapeditorentitydef/props/dedupe.decl";
    const char *text = "{ edit = { propertySheets = { num = 2; "
        "item[0] = { properties = { num = 1; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"custom\"; } } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"stock\"; descriptionTag = \"help\"; } "
        "item[1] = { path = \"color\"; inspector = \"vec3\"; } } } } } }";
    const char *invalid = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"size\"; inspector = \"vec3\"; } "
        "item[1] = { path = \"size\"; inspector = \"scalar\"; } } } } } }";
    char data[MAX_PATH], map[MAX_PATH], error[2048], *summary;
    const sh_package_compilation *current;
    const sh_compiled_resource *resource;
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    snprintf(data, sizeof(data), "%s/dedupe", root);
    snprintf(map, sizeof(map), "%s/dedupe-map", root);
    create("dedupe/overrides/editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\"}");
    create("dedupe/overrides/duplicate/package.json", "{\"id\":\"another.id\",\"name\":\"Identical contents\"}");
    create("dedupe/overrides/healthy/package.json", "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
    create("dedupe/overrides/healthy/assets/healthy.bin", "independent resource");
    create(relative, text);
    create("dedupe/overrides/duplicate/assets/generated/decls/snapeditorentitydef/props/dedupe.decl", text);
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data) && sh_package_runtime_ready());
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 3 packages") && !strstr(summary, "Skipped local package"));
    free(summary);
    current = sh_package_runtime_acquire();
    resource = sh_package_compilation_find(current, path);
    CHECK(resource && resource->body && resource->source_count == 2 &&
        sh_package_owners_count(&resource->owners) == 2 && !sh_package_owners_count(&resource->gameplay_owners));
    if (resource && resource->body) {
        const char *body = (const char *)resource->body;
        const char *size = strstr(body, "path = \"size\"");
        CHECK(size && !strstr(size + 1, "path = \"size\""));
        CHECK(strstr(body, "path = \"color\"") && strstr(body, "\"custom\"") &&
            strstr(body, "\"help\"") && !strstr(body, "\"stock\""));
        /* Compilation and serving must retain both complete authored copies. */
        for (size_t i = 0; i < resource->source_count; i++) {
            size_t length = 0;
            unsigned char *authored = sh_package_source_read(current->sources->files + resource->sources[i],
                SIZE_MAX, &length, error, sizeof(error));
            CHECK(authored && length == strlen(text) && !memcmp(authored, text, length)); free(authored);
        }
    }
    sh_package_runtime_release();
    expect_current("healthy.bin", "independent resource");
    create("dedupe-map/overrides/editor/package.json", "{\"id\":\"map.editor\",\"name\":\"Map controls\"}");
    create("dedupe-map/overrides/editor/assets/generated/decls/snapeditorentitydef/props/dedupe.decl", text);
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    CHECK(plan && sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
    CHECK(missing.checked == 1 && !missing.count && !sh_package_owners_count(&missing.packages));
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    CHECK(!sh_package_runtime_has_map_provider());
    /* A real behavioral contradiction is not a duplicate. Reject only that
     * authored package; its healthy identical peer still provides the controls. */
    create(relative, invalid);
    CHECK(sh_package_runtime_refresh(data) && sh_package_runtime_ready());
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 2 packages") &&
        strstr(summary, "Skipped local package") && strstr(summary, "collection identity")); free(summary);
    expect_current(path, "\"custom\""); expect_current("healthy.bin", "independent resource");
    create(relative, text);
    CHECK(sh_package_runtime_refresh(data) && sh_package_runtime_ready());
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 3 packages") && !strstr(summary, "Skipped local package"));
    free(summary);
    sh_package_runtime_test_dispose();
    cleanup_resources(data);
}

static int unavailable_local_original(void *context, const char *path,
    unsigned char **body, size_t *length)
{
    if (!strcmp(path, "unavailable-original.bin")) {
        *body = NULL; *length = 0; return -1;
    }
    return baseline(context, path, body, length);
}

static void test_failed_local_compilation_admission(void)
{
    char data[MAX_PATH], map[MAX_PATH], error[2048], *summary;
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    int fail = 0;
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(unavailable_local_original, NULL);
    snprintf(data, sizeof(data), "%s/unavailable-library", root);
    snprintf(map, sizeof(map), "%s/independent-map", root);
    create("unavailable-library/overrides/local/package.json", "{\"id\":\"local\",\"name\":\"Local\"}");
    create("unavailable-library/overrides/local/assets/unavailable-original.bin", "cannot inspect original");
    create("unavailable-library/overrides/healthy/package.json", "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
    create("unavailable-library/overrides/healthy/assets/independent.bin", "required bytes");
    create("independent-map/overrides/map/package.json", "{\"id\":\"map\",\"name\":\"Map\"}");
    create("independent-map/overrides/map/assets/independent.bin", "required bytes");
    /* An unavailable original is an operational compile failure, not proof
     * that either authored package is malformed. First boot still activates
     * the product baseline and retains the complete installed inventory. */
    CHECK(!sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 2 packages") && strstr(summary, "composition unavailable"));
    free(summary);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_admission_ready());
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error)); CHECK(plan);
    CHECK(plan && sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    expect_current("independent.bin", "required bytes");
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_admission_ready() && !sh_package_runtime_has_map_provider());
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose();
    cleanup_resources(data);
}

static void write_bank(const char *relative, const char *name, uint32_t event)
{
    unsigned char bank[49] = {0}; uint32_t id = 0, value; char path[4096]; FILE *file = NULL;
    CHECK(sh_audio_bank_identity(name, &id));
    memcpy(bank, "BKHD", 4); value = 16; memcpy(bank + 4, &value, 4); value = 113; memcpy(bank + 8, &value, 4);
    memcpy(bank + 12, &id, 4); memcpy(bank + 24, "HIRC", 4); value = 17; memcpy(bank + 28, &value, 4);
    value = 1; memcpy(bank + 32, &value, 4); bank[36] = 4; value = 8; memcpy(bank + 37, &value, 4); memcpy(bank + 41, &event, 4);
    create(relative, "");
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    CHECK(!fopen_s(&file, path, "wb") && file);
    if (file) { CHECK(fwrite(bank, 1, sizeof(bank), file) == sizeof(bank)); fclose(file); }
}
/* A map's audio compiles into one retained program; the published view lists
 * it, a copied occurrence keeps it listed after the map departs, and the
 * native layer's owner reference keeps it usable until released. */
static void test_audio_programs(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    char data[MAX_PATH], map[MAX_PATH], base[MAX_PATH], error[2048];
    sh_package_runtime_audio *rows = NULL; size_t count = 7; int fail = 0;
    sh_package_occurrence_snapshot *document = NULL; sh_package_map_plan *plan = NULL;
    void *owner = NULL; sh_package_audio_program *program = NULL;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/audio-library", root);
    snprintf(map, sizeof(map), "%s/audio-map", root);
    snprintf(base, sizeof(base), "%s/audio-base", root);
    create("audio-base/sound", NULL);
    create("audio-library/overrides", NULL);
    create("audio-map/overrides/sound/package.json", "{\"id\":\"sound\",\"name\":\"Sound\"}");
    create("audio-map/overrides/sound/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("audio-map/overrides/sound/assets/sound/soundbanks/pc/mapbank.bnk", "mapbank.bnk", 77);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_package_runtime_audio_programs(&rows, &count, error, sizeof(error)) && !count);
    free(rows); rows = NULL;
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    if (!plan) fprintf(stderr, "audio program map: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_audio_programs(&rows, &count, error, sizeof(error)) && count == 1);
    if (count) { owner = rows[0].owner; program = rows[0].program; }
    size_t banks = 0, scopes = program ? sh_package_audio_program_scopes(program) : 0;
    for (size_t i = 0; i < scopes; i++) banks += sh_package_audio_program_banks(program, i);
    if (banks != 1) fprintf(stderr, "audio program scopes %zu banks %zu\n", scopes, banks);
    CHECK(owner && program && scopes >= 1 && banks == 1);
    free(rows); rows = NULL;
    /* A copied occurrence retains the map provider and so its program. */
    size_t rebound_length = 0, captured_length = 0;
    char *rebound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &rebound_length, error, sizeof(error));
    if (!rebound) fprintf(stderr, "audio rebind: %s\n", error);
    CHECK(rebound);
    if (rebound) {
        char *captured = sh_package_runtime_capture_document(plan, NULL, NULL, 0, rebound, rebound_length,
            &captured_length, &document, NULL, NULL, error, sizeof(error));
        if (!captured) fprintf(stderr, "audio capture: %s\n", error);
        CHECK(captured && document); free(captured); HeapFree(GetProcessHeap(), 0, rebound);
    }
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_audio_programs(&rows, &count, error, sizeof(error)) && count == 1 && rows[0].program == program);
    for (size_t i = 0; i < count; i++) sh_package_runtime_audio_release(rows[i].owner);
    free(rows); rows = NULL;
    sh_package_occurrence_release(document); document = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(sh_package_runtime_audio_programs(&rows, &count, error, sizeof(error)) && !count);
    free(rows); rows = NULL;
    /* The departed program stays usable while the native layer holds it. */
    size_t retained_banks = 0;
    for (size_t i = 0; program && i < sh_package_audio_program_scopes(program); i++) retained_banks += sh_package_audio_program_banks(program, i);
    CHECK(retained_banks == 1);
done:
    if (owner) sh_package_runtime_audio_release(owner);
    sh_package_occurrence_release(document); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
static void test_local_isolation(void)
{
    char data[MAX_PATH], map[MAX_PATH], error[2048], *summary;
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    snprintf(data, sizeof(data), "%s/isolated", root);
    snprintf(map, sizeof(map), "%s/strict", root);
    create("isolated/overrides/boss/package.json", "{\"id\":\"boss\",\"name\":\"Boss\"}");
    create("isolated/overrides/boss/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("isolated/overrides/bad/package.json", "{\"schema\":\"old\"}");
    create("isolated/overrides/text/package.json", "{\"id\":\"text\",\"name\":\"Text\"}");
    create("isolated/overrides/text/assets/generated/decls/snapeditorentitydef/bad.decl", "{} }");
    create("isolated/overrides/text/assets/generated/decls/snapeditorentitydef/rejected.decl", "{ value = 1; }");
    create("isolated/overrides/policy/package.json", "{\"id\":\"policy\",\"name\":\"Policy\",\"requirements\":{\"cvars\":{\"unsupported\":0}}}");
    create("isolated/overrides/outer/package.json", "{\"id\":\"outer\",\"name\":\"Outer\"}");
    create("isolated/overrides/outer/child/package.json", "invalid");
    create("isolated/overrides/outer/assets/rejected-nested.bin", "also unavailable");
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    expect_current(health_path, "health = 12000"); expect_current("generated/decls/snapeditorentitydef/rejected.decl", NULL);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 1 packages") && strstr(summary, "Skipped local package") &&
        strstr(summary, "malformed declaration") && strstr(summary, "unsupported setting")); free(summary);
    create("strict/overrides/map/package.json", "{\"id\":\"map\",\"name\":\"Map\"}");
    create("strict/overrides/map/assets/generated/decls/snapeditorentitydef/rejected.decl", "{ value = 2; }");
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    CHECK(plan && sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
    CHECK(missing.count == 1); sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    create("strict/overrides/broken/package.json", "invalid");
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    CHECK(!plan && error[0]); sh_package_map_plan_free(plan);
    expect_current(health_path, "health = 12000");
    create("isolated/overrides/text/assets/generated/decls/snapeditorentitydef/bad.decl", "{}");
    CHECK(sh_package_runtime_refresh(data));
    expect_current("generated/decls/snapeditorentitydef/rejected.decl", "value = 1;");

    /* A file that cannot be a declaration identity belongs to its own package. */
    create("isolated/overrides/identity/package.json", "{\"id\":\"identity\",\"name\":\"Identity\"}");
    create("isolated/overrides/identity/assets/generated/decls/notype.decl", "{}");
    create("isolated/overrides/identity/assets/generated/decls/entitydef/identity-only.decl", "{ edit = { value = 1; } }");
    /* When two packages must be composed, a contribution the compiler cannot
     * parse on its own isolates that package; the valid edit still loads. */
    create("isolated/overrides/garbled/package.json", "{\"id\":\"garbled\",\"name\":\"Garbled\"}");
    create("isolated/overrides/garbled/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = !12000; } }");
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    expect_current(health_path, "health = 12000");
    expect_current("generated/decls/entitydef/identity-only.decl", NULL);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 2 packages") && strstr(summary, "missing decl type directory") &&
        strstr(summary, "garbled") && strstr(summary, "identity")); free(summary);

    /* Every edit to the disputed field is excluded, including a nonidentical
     * snapshot repeating one value. An independent edit to the same resource
     * remains usable; the resource path itself is not a quarantine boundary. */
    create("isolated/overrides/independent/package.json", "{\"id\":\"independent\",\"name\":\"Independent\"}");
    create("isolated/overrides/independent/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 25000; armor = 1; } }");
    create("isolated/overrides/repeated/package.json", "{\"id\":\"repeated\",\"name\":\"Repeated\"}");
    create("isolated/overrides/repeated/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 11; speed = 7; } }");
    create("isolated/overrides/garbled/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 11; } }");
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    expect_current("generated/decls/snapeditorentitydef/rejected.decl", "value = 1;");
    expect_current(health_path, "health = 25000"); expect_current(health_path, "armor = 1");
    summary = sh_package_runtime_summary();
    /* Valid excluded peers remain physically installed for authored maps. */
    const sh_package_compilation *isolated_library = sh_package_runtime_library_acquire();
    CHECK(isolated_library && isolated_library->sources->package_count == 2);
    sh_package_runtime_release();
    CHECK(summary && strstr(summary, "Installed library: 5 packages") &&
        strstr(summary, "garbled") && strstr(summary, "boss") && strstr(summary, "repeated")); free(summary);
    create("isolated/overrides/garbled/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("isolated/overrides/repeated/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; speed = 7; } }");
    CHECK(sh_package_runtime_refresh(data)); expect_current(health_path, "health = 12000");
    expect_current(health_path, "armor = 1"); expect_current(health_path, "speed = 7");
    /* Binary collisions and policy collisions use the same whole-owner
     * isolation boundary. Duplicate copies on either side cannot win by order. */
    create("isolated/overrides/opaque-a/package.json", "{\"id\":\"opaque-a\",\"name\":\"Opaque A\"}");
    create("isolated/overrides/opaque-b/package.json", "{\"id\":\"opaque-b\",\"name\":\"Opaque B\"}");
    create("isolated/overrides/opaque-c/package.json", "{\"id\":\"opaque-c\",\"name\":\"Opaque C\"}");
    create("isolated/overrides/opaque-a/assets/shared.bimage", "a");
    create("isolated/overrides/opaque-b/assets/shared.bimage", "b");
    create("isolated/overrides/opaque-c/assets/shared.bimage", "b");
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    expect_current("shared.bimage", NULL); expect_current(health_path, "health = 12000");
    expect_current("generated/decls/snapeditorentitydef/rejected.decl", "value = 1;");
    create("isolated/overrides/opaque-a/assets/shared.bimage", "b");
    CHECK(sh_package_runtime_refresh(data)); expect_current("shared.bimage", "b");
    create("isolated/overrides/policy-a/package.json", "{\"id\":\"policy-a\",\"name\":\"Policy A\",\"strings\":{\"en\":{\"collision\":\"a\"}}}");
    create("isolated/overrides/policy-b/package.json", "{\"id\":\"policy-b\",\"name\":\"Policy B\",\"strings\":{\"en\":{\"collision\":\"b\"}}}");
    create("isolated/overrides/policy-c/package.json", "{\"id\":\"policy-c\",\"name\":\"Policy C\",\"strings\":{\"en\":{\"collision\":\"b\"}}}");
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    expect_current("shared.bimage", "b"); expect_current(health_path, "health = 12000");
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "policy-a") && strstr(summary, "policy-b") && strstr(summary, "policy-c")); free(summary);
    /* Unsupported index semantics are isolated, never guessed or allowed to
     * disable unrelated resources. Authored map compilation stays strict. */
    create("isolated/overrides/index-a/package.json", "{\"id\":\"index-a\",\"name\":\"Index A\"}");
    create("isolated/overrides/index-b/package.json", "{\"id\":\"index-b\",\"name\":\"Index B\"}");
    create("isolated/overrides/index-a/assets/generated/decls/entitydef/unadapted.decl", "{ edit = { entries = { num = 1; item[0] = 1; } } }");
    create("isolated/overrides/index-b/assets/generated/decls/entitydef/unadapted.decl", "{ edit = { entries = { num = 1; item[0] = 2; } } }");
    CHECK(sh_package_runtime_refresh(data)); expect_current(health_path, "health = 12000");
    expect_current("generated/decls/entitydef/unadapted.decl", NULL);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "index-a") && strstr(summary, "index-b") && strstr(summary, "composition adapter")); free(summary);
    sh_package_runtime_test_dispose();
    cleanup_resources(data);
}

typedef struct atlas_activation {
    unsigned char owner[0x178], provider[0x458], materials[2][0x1b8];
    int next, previous, fail, bad_atlas, calls, recoveries;
    int expect_binding_change, no_binding;
    void *rows[2];
} atlas_activation;
static void atlas_pointer(void *data, size_t at, const void *value)
{ memcpy((unsigned char *)data + at, &value, sizeof(value)); }
static void atlas_destroy(atlas_activation *s, int index)
{
    if (s->rows[index]) atlas_pointer(s->rows[index], 0x30, NULL);
    atlas_pointer(s->materials[index], 0x170, NULL); s->rows[index] = NULL;
}
static int activate_atlas(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    atlas_activation *s = context;
    int index = restoring ? s->previous : s->next;
    const char *names[] = {"variants/red", "variants/green"};
    char path[256];
    CHECK(changes && !sh_package_runtime_admission_ready());
    if (restoring) {
        s->recoveries++; if (s->next >= 0) atlas_destroy(s, s->next);
        atlas_pointer(s->provider, 0x1a0, "_vmtr");
    } else s->calls++;
    if (index < 0) {
        atlas_destroy(s, 0); atlas_destroy(s, 1);
        sh_virtual_material_runtime_collect(); return 1;
    }
    snprintf(path, sizeof(path), "generated/decls/material/%s.decl", names[index]);
    expect_current(path, index ? "green" : "red");
    if (s->expect_binding_change) {
        CHECK(changes->count == 1);
        CHECK(changes->count && !strcmp(changes->items[0].path, path));
        CHECK(changes->count && changes->items[0].kind == SH_PACKAGE_RESOURCE_REPLACED);
        CHECK(changes->count && !strcmp(changes->items[0].type, "material"));
        atlas_destroy(s, index);
    }
    atlas_pointer(s->materials[index], 8, names[index]);
    if (!restoring && s->bad_atlas) atlas_pointer(s->provider, 0x1a0, "unrelated");
    void *row = atlas_lookup(s->owner, s->materials[index]);
    if ((!s->bad_atlas || restoring) && !s->no_binding) CHECK(row); else CHECK(!row);
    if (row) {
        if (s->rows[index]) CHECK(row == s->rows[index]);
        s->rows[index] = row; atlas_pointer(s->materials[index], 0x170, row);
    }
    if (!restoring && s->fail) {
        snprintf(error, capacity, "synthetic activation cancellation"); return 0;
    }
    return 1; /* Success cannot hide a failed atlas lookup. */
}
static void test_native_material_provider_transaction(void)
{
    char path[4096], error[1024];
    sh_package_map_plan *a, *b, *unbound;
    atlas_activation state = {0};
    sh_virtual_material_record record = {"source/texture", 0, 0, 16, 16, 0, 1, 2};
    sh_virtual_material_runtime_binding input = {"variants/red", "_vmtr", &record, 0};
    sh_package_missing before = {0}, after = {0};
    sig_result signature = {"VirtualMaterialLookup", SIG_OK, 0x1234, 0};
    create("native-a/overrides/p/package.json", "{\"id\":\"pack\",\"name\":\"A\"}");
    create("native-a/overrides/p/assets/generated/decls/material/variants/red.decl", "{ label = red; }");
    create("native-b/overrides/p/package.json", "{\"id\":\"pack\",\"name\":\"B\"}");
    create("native-b/overrides/p/assets/generated/decls/material/variants/green.decl", "{ label = green; }");
    snprintf(path, sizeof(path), "%s/native-a", root);
    a = sh_package_runtime_prepare_map(root, path, error, sizeof(error)); CHECK(a);
    snprintf(path, sizeof(path), "%s/native-b", root);
    b = sh_package_runtime_prepare_map(root, path, error, sizeof(error)); CHECK(b);
    CHECK(sh_package_map_plan_payload_missing(a, &before, error, sizeof(error)));
    CHECK(sh_package_map_plan_bind_virtual_materials(a, &input, 1, error, sizeof(error)));
    CHECK(sh_package_map_plan_payload_missing(a, &after, error, sizeof(error)));
    CHECK(before.count == after.count && before.checked == after.checked);
    for (size_t i = 0; i < before.count && i < after.count; i++) CHECK(!strcmp(before.paths[i], after.paths[i]));
    sh_package_missing_free(&before); sh_package_missing_free(&after);
    input.name = "missing/material";
    CHECK(!sh_package_map_plan_bind_virtual_materials(a, &input, 1, error, sizeof(error)));
    input.name = "variants/green";
    CHECK(sh_package_map_plan_bind_virtual_materials(b, &input, 1, error, sizeof(error)));
    state.next = 0; state.previous = -1;
    atlas_pointer(state.owner, 0x130, state.provider); atlas_pointer(state.provider, 0x1a0, "_vmtr");
    { int extent = 256; memcpy(state.provider + 0x2fc, &extent, sizeof(extent)); }
    CHECK(!sh_package_runtime_activate_prepared_map(a, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(!state.calls && !state.recoveries && !sh_package_runtime_has_map_provider());
    CHECK(sh_virtual_material_runtime_install(&signature, 1));
    CHECK(sh_package_runtime_activate_prepared_map(a, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(state.calls == 1 && state.rows[0]);
    CHECK(!sh_package_map_plan_bind_virtual_materials(a, NULL, 0, error, sizeof(error)));
    /* Metadata-only transitions reconstruct the same material even when every
     * delivered source byte is identical. No synthetic resource path is added. */
    snprintf(path, sizeof(path), "%s/native-a", root);
    unbound = sh_package_runtime_prepare_map(root, path, error, sizeof(error)); CHECK(unbound);
    state.expect_binding_change = state.no_binding = 1;
    CHECK(sh_package_runtime_activate_prepared_map(unbound, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(!state.rows[0]); state.no_binding = 0;
    CHECK(sh_package_runtime_activate_prepared_map(a, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(state.rows[0]); state.expect_binding_change = 0;
    sh_package_map_plan_free(unbound);
    state.next = 1; state.previous = 0; state.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(b, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(state.recoveries == 1 && state.rows[0] && !state.rows[1]);
    CHECK(sh_package_runtime_admission_ready());
    state.fail = 0; state.bad_atlas = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(b, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(state.recoveries == 2 && sh_package_runtime_admission_ready());
    state.bad_atlas = 0;
    CHECK(sh_package_runtime_activate_prepared_map(b, (sh_package_activation_guard){0}, activate_atlas, &state));
    sh_package_map_plan_free(a); a = NULL;
    CHECK(state.rows[0] && !strcmp(*(const char **)state.rows[0], "variants/red"));
    sh_virtual_material_runtime_collect();
    CHECK(!strcmp(*(const char **)state.rows[0], "variants/red"));
    atlas_pointer(state.provider, 0x1a0, "unrelated");
    CHECK(!atlas_lookup(state.owner, state.materials[1]));
    CHECK(!sh_package_runtime_admission_ready());
    sh_package_runtime_error(error, sizeof(error)); CHECK(strstr(error, "native page provider"));
    atlas_pointer(state.provider, 0x1a0, "_vmtr");
    state.next = state.previous = 1; state.expect_binding_change = 1;
    CHECK(sh_package_runtime_activate_prepared_map(b, (sh_package_activation_guard){0}, activate_atlas, &state));
    CHECK(sh_package_runtime_admission_ready()); state.expect_binding_change = 0;
    state.next = -1; state.previous = 1;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, activate_atlas, &state));
    sh_package_map_plan_free(b); sh_virtual_material_runtime_collect();
    CHECK(!state.rows[0] && !state.rows[1] && !sh_package_runtime_has_map_provider());
}
static void test_complete_source_snapshots(void)
{
    char data[MAX_PATH], map[MAX_PATH], error[2048];
    const sh_package_compilation *candidate, *current;
    sh_package_map_plan *plan;
    sh_package_bundle_binding binding = {0}, invalid;
    sh_package_bundle *held = NULL;
    unsigned char *expected = NULL, *read, wrong[32];
    size_t expected_length = 0, length;
    create("snapshot-data/overrides/local/package.json", "{\"id\":\"same-id\",\"name\":\"Local\"}");
    create("snapshot-data/overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("snapshot-map/overrides/map/package.json", " { \"id\": \"same-id\", \"name\": \"Saved\" } \r\n");
    create("snapshot-map/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("snapshot-map/overrides/map/author-extra.txt", "exact retained unused author bytes");
    create("snapshot-map/overrides/map/empty", NULL);
    snprintf(data, sizeof(data), "%s/snapshot-data", root);
    snprintf(map, sizeof(map), "%s/snapshot-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    candidate = sh_package_map_plan_compilation(plan);
    CHECK(candidate && candidate->sources->package_count == 1);
    if (!candidate || candidate->sources->package_count != 1) goto done;
    expected = sh_package_archive_pack(candidate->sources, 0, &expected_length, error, sizeof(error)); CHECK(expected);
    binding.original = sh_package_bundle_capture(candidate->sources, 0, data, error, sizeof(error)); CHECK(binding.original);
    strcpy_s(binding.source_name, sizeof(binding.source_name), candidate->sources->packages[0].name);
    memcpy(binding.compiler_tree, candidate->sources->fingerprints[0], 32);
    CHECK(sh_package_map_plan_attach_bundles(plan, &binding, 1, error, sizeof(error)));
    CHECK(!sh_package_map_plan_attach_bundles(plan, &binding, 0, error, sizeof(error)));
    invalid = binding; invalid.compiler_tree[0] ^= 1;
    CHECK(!sh_package_map_plan_attach_bundles(plan, &invalid, 1, error, sizeof(error)));
    invalid = binding; strcpy_s(invalid.source_name, sizeof(invalid.source_name), "same-id");
    CHECK(!sh_package_map_plan_attach_bundles(plan, &invalid, 1, error, sizeof(error)));
    invalid = binding; invalid.original = NULL;
    CHECK(!sh_package_map_plan_attach_bundles(plan, &invalid, 1, error, sizeof(error)));
    sh_package_bundle_release(binding.original); binding.original = NULL;
    create("snapshot-map/overrides/map/package.json", "external edit after plan capture");
    create("snapshot-map/overrides/map/author-extra.txt", "changed unused extra");
    create("snapshot-map/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 999; } }");
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, NULL, NULL));
    current = sh_package_runtime_acquire();
    CHECK(current && current->map_overlay && current->map_owner_begin == 1);
    if (current && current->map_overlay) {
        held = sh_package_runtime_source_bundle(current->map_owner_begin, binding.compiler_tree); CHECK(held);
        memcpy(wrong, binding.compiler_tree, 32); wrong[0] ^= 1;
        CHECK(!sh_package_runtime_source_bundle(current->map_owner_begin, wrong));
        CHECK(!sh_package_runtime_source_bundle(current->sources->package_count, binding.compiler_tree));
    }
    sh_package_runtime_release();
    CHECK(!sh_package_map_plan_attach_bundles(plan, NULL, 0, error, sizeof(error)));
    sh_package_map_plan_free(plan); plan = NULL;
    expect_current(health_path, "health = 10");
    CHECK(sh_package_runtime_activate_map(data, NULL, (sh_package_activation_guard){0}, NULL, NULL));
    CHECK(!sh_package_runtime_has_map_provider()); expect_current(health_path, "health = 12000");
    sh_package_runtime_test_dispose();
    read = sh_package_bundle_read(held, &length, error, sizeof(error));
    CHECK(expected && read && length == expected_length && !memcmp(read, expected, length)); free(read);
done:
    sh_package_map_plan_free(plan); sh_package_bundle_release(binding.original);
    sh_package_bundle_release(held); free(expected); sh_package_runtime_test_dispose();
    cleanup_resources(data);
}

static void test_original_deliveries_before_compilation(void)
{
    char data[MAX_PATH], map[MAX_PATH], authored[MAX_PATH], error[2048];
    sh_package_sources *sources = NULL, *originals = NULL;
    sh_package_bundle_binding rows[2] = {{0}}, invalid[2];
    sh_package_map_plan *plan = NULL, *failed;
    const sh_package_compilation *candidate, *current;
    create("origin-data/overrides", NULL);
    for (size_t i = 0; i < 2; i++) {
        char path[256];
        snprintf(path, sizeof(path), "origin-authored/overrides/%c/package.json", (int)('a' + i));
        create(path, i ? "{ \"id\": \"same\", \"name\": \"Same\" }" : "{\"id\":\"same\",\"name\":\"Same\"}");
        snprintf(path, sizeof(path), "origin-authored/overrides/%c/author.txt", (int)('a' + i));
        create(path, i ? "original B" : "original A");
        snprintf(path, sizeof(path), "origin-map/overrides/%c/package.json", (int)('a' + i));
        create(path, "{\"id\":\"same\",\"name\":\"Same\"}");
        snprintf(path, sizeof(path), "origin-map/overrides/%c/assets/generated/decls/entitydef/ai/cyberdemon.decl", (int)('a' + i));
        create(path, "{ edit = { health = 10; } }");
    }
    snprintf(data, sizeof(data), "%s/origin-data", root);
    snprintf(map, sizeof(map), "%s/origin-map", root);
    snprintf(authored, sizeof(authored), "%s/origin-authored", root);
    sources = sh_package_sources_scan(map, error, sizeof(error)); CHECK(sources && sources->package_count == 2);
    originals = sh_package_sources_scan(authored, error, sizeof(error)); CHECK(originals && originals->package_count == 2);
    if (!sources || !originals || sources->package_count != 2 || originals->package_count != 2) goto done;
    CHECK(!memcmp(sources->fingerprints[0], sources->fingerprints[1], 32));
    CHECK(memcmp(originals->fingerprints[0], originals->fingerprints[1], 32));
    for (size_t i = 0; i < 2; i++) {
        strcpy_s(rows[i].source_name, sizeof(rows[i].source_name), sources->packages[i].name);
        memcpy(rows[i].compiler_tree, sources->fingerprints[i], 32);
        rows[i].original = sh_package_bundle_capture(originals, i, data, error, sizeof(error)); CHECK(rows[i].original);
    }
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map_deliveries(data, map, NULL, rows, 2, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    candidate = sh_package_map_plan_compilation(plan); CHECK(candidate && candidate->sources->original_fingerprints);
    if (!candidate || !candidate->sources->original_fingerprints) goto done;
    for (size_t i = 0; i < 2; i++) {
        CHECK(!memcmp(candidate->sources->fingerprints[i], rows[i].compiler_tree, 32));
        CHECK(!memcmp(sh_package_source_tree(candidate->sources, i), sh_package_bundle_fingerprint(rows[i].original), 32));
    }
    memcpy(invalid, rows, sizeof(rows)); invalid[1] = rows[0];
    failed = sh_package_runtime_prepare_map_deliveries(data, map, NULL, invalid, 2, error, sizeof(error));
    CHECK(!failed); sh_package_map_plan_free(failed);
    memcpy(invalid, rows, sizeof(rows)); invalid[0].compiler_tree[0] ^= 1;
    failed = sh_package_runtime_prepare_map_deliveries(data, map, NULL, invalid, 2, error, sizeof(error));
    CHECK(!failed); sh_package_map_plan_free(failed);
    memcpy(invalid, rows, sizeof(rows)); invalid[0].original = rows[1].original;
    CHECK(!sh_package_map_plan_attach_bundles(plan, invalid, 2, error, sizeof(error)));
    CHECK(sh_package_map_plan_attach_bundles(plan, rows, 2, error, sizeof(error)));
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, NULL, NULL));
    current = sh_package_runtime_acquire(); CHECK(current && current->map_overlay);
    if (current) {
        const sh_package_provenance *bindings = sh_package_runtime_bindings();
        CHECK(bindings && bindings->context_count == 1 && bindings->contexts[0].tree_count == 2);
        for (size_t i = 0; i < 2; i++) {
            sh_package_bundle *held = sh_package_runtime_source_bundle(current->map_owner_begin + i, rows[i].compiler_tree);
            CHECK(held && !memcmp(sh_package_bundle_fingerprint(held), sh_package_bundle_fingerprint(rows[i].original), 32));
            sh_package_bundle_release(held);
            int found = 0;
            if (bindings && bindings->context_count) for (size_t t = 0; t < bindings->contexts[0].tree_count; t++)
                if (!memcmp(bindings->contexts[0].trees[t], sh_package_bundle_fingerprint(rows[i].original), 32)) found = 1;
            CHECK(found);
        }
    }
    sh_package_runtime_release();
done:
    sh_package_map_plan_free(plan); sh_package_runtime_test_dispose();
    sh_package_sources_free(sources); sh_package_sources_free(originals);
    for (size_t i = 0; i < 2; i++) sh_package_bundle_release(rows[i].original);
    cleanup_resources(data);
}

static const char *historical_root = "generated/decls/entitydef/history/root.decl";
static const char *historical_original = "{ class = \"Entity\"; edit = { health = 100; target = \"\"; } }";
static int historical_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, historical_root) ? historical_original :
        !strcmp(path, "generated/decls/entitydef/history/stockbase.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; target = \"\"; } }" :
        !strcmp(path, "generated/decls/entitydef/history/child.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static const sh_compiled_resource *historical_resource(const sh_package_compilation *compiled, const char *path)
{
    if (compiled) for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->source_count && !strcmp(r->original_path ? r->original_path : r->engine_path, path)) return r;
    }
    return NULL;
}
static int consumer_requests(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    (void)context; (void)scope; (void)observing; (void)error; (void)capacity;
    if (!strcmp(unit->family, "entitydef") && !strcmp(unit->name, "consumer/root"))
        return visit(visitor, "constructor/target", "entitydef", "consumer/leaf");
    return 1;
}
static void test_constructor_binding_changes(void)
{
    char error[512];
    sh_package_consumer_target old_target = {"material", "material", "old"}, next_target = {"material", "material", "next"};
    sh_package_consumer old_rows[] = {{"font", "primary", &old_target, 1}, {"font", "secondary", &old_target, 1},
        {"font", "retired", &old_target, 1}, {"native", "no-file", &old_target, 1}};
    sh_package_consumer next_rows[] = {{"font", "primary", &next_target, 1}, {"font", "secondary", &next_target, 1},
        {"font", "added", &next_target, 1}, {"native", "no-file", NULL, 0}};
    sh_package_consumers *before = sh_package_consumers_create(old_rows, 4, error, sizeof(error));
    sh_package_consumers *after = sh_package_consumers_create(next_rows, 4, error, sizeof(error));
    CHECK(before && after);
    sh_package_native_identity added = {"font", "added"}, removed = {"font", "retired"};
    sh_package_changes changes = {0}; changes.identities_complete = 1;
    changes.added = &added; changes.added_count = 1; changes.removed = &removed; changes.removed_count = 1;
    CHECK(sh_package_runtime_test_consumer_changes(before, after, &changes));
    CHECK(!changes.count && changes.rebound_count == 3);
    if (changes.rebound_count == 3) {
        CHECK(!strcmp(changes.rebound[0].name, "primary") && !strcmp(changes.rebound[1].name, "secondary"));
        CHECK(!strcmp(changes.rebound[2].type, "native") && !strcmp(changes.rebound[2].name, "no-file"));
    }
    sh_package_native_identity *retained = changes.rebound;
    sh_package_changes_reverse(&changes);
    CHECK(changes.added == &removed && changes.removed == &added && changes.rebound == retained && changes.rebound_count == 3);
    sh_package_changes_reverse(&changes);
    changes.added = changes.removed = NULL; changes.added_count = changes.removed_count = 0;
    sh_package_changes_free(&changes);
    CHECK(sh_package_runtime_test_consumer_changes(after, after, &changes) && !changes.rebound_count);
    sh_package_changes_free(&changes);
    /* Removing/adding a captured plan for a surviving native owner is also a
     * reconstruction, not permission to keep stale state or to retire it. */
    CHECK(sh_package_runtime_test_consumer_changes(NULL, after, &changes) && changes.rebound_count == 4);
    sh_package_changes_free(&changes);
    CHECK(sh_package_runtime_test_consumer_changes(after, NULL, &changes) && changes.rebound_count == 4);
    sh_package_consumers_free(before); sh_package_consumers_free(after);
    CHECK(changes.rebound_count == 4 && !strcmp(changes.rebound[0].name, "added"));
    sh_package_changes_free(&changes);
}
static void expect_consumer(const char *owner, const char *target)
{
    sh_package_consumer_hold hold = {0};
    CHECK(sh_package_runtime_consumer_acquire("entitydef", owner, &hold) == (target != NULL));
    if (target && hold.binding) {
        const sh_package_consumer_target *slot = sh_package_consumer_find(hold.binding, "constructor/target");
        CHECK(slot && !strcmp(slot->family, "entitydef") && !strcmp(slot->name, target));
        CHECK(hold.binding->count == 1 && !strcmp(hold.binding->name, owner));
        CHECK(sh_package_runtime_consumer_acquire("entitydef", owner, &hold) == -1 && hold.provider && hold.binding);
    }
    sh_package_runtime_consumer_release(&hold);
    CHECK(!hold.provider && !hold.binding); sh_package_runtime_consumer_release(&hold);
}
typedef struct consumer_activation {
    const char *owner[2], *target[2];
    int next, previous, calls, recoveries, fail, retained;
} consumer_activation;
static int consumer_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{
    consumer_activation *state = context; int current = restoring ? state->previous : state->next;
    CHECK(changes && !sh_package_runtime_admission_ready());
    for (int i = 0; i < 2; i++) expect_consumer(state->owner[i],
        i == current || (!i && state->retained) ? state->target[i] : NULL);
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "fixture constructor activation failure"); return 0; }
    return 1;
}
static void test_constructor_provider_lifetimes(void)
{
    const char *owner_path = "generated/decls/entitydef/consumer/root.decl";
    const char *leaf_path = "generated/decls/entitydef/consumer/leaf.decl";
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"consumer/root\"}]}";
    char data[MAX_PATH], map[2][MAX_PATH], error[2048], owners[2][1024], targets[2][1024];
    sh_package_map_plan *plans[2] = {0}; sh_package_consumer_hold held = {0};
    sh_package_occurrence_snapshot *document = NULL;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    consumer_activation state = {{owners[0], owners[1]}, {targets[0], targets[1]}, 0, -1};
    create("consumer-data/overrides", NULL); snprintf(data, sizeof(data), "%s/consumer-data", root);
    for (int i = 0; i < 2; i++) {
        char path[512]; snprintf(map[i], sizeof(map[i]), "%s/consumer-%c", root, 'a' + i);
        snprintf(path, sizeof(path), "consumer-%c/overrides/p/package.json", 'a' + i);
        create(path, "{\"id\":\"same\",\"name\":\"Constructor dependencies\"}");
        snprintf(path, sizeof(path), "consumer-%c/overrides/p/assets/%s", 'a' + i, owner_path);
        create(path, "{ class = \"Entity\"; edit = {} }");
        snprintf(path, sizeof(path), "consumer-%c/overrides/p/assets/%s", 'a' + i, leaf_path);
        create(path, i ? "{ class = \"Entity\"; edit = { health = 20; } }" :
            "{ class = \"Entity\"; edit = { health = 10; } }");
    }
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_constructor_requests(consumer_requests, NULL);
    for (int i = 0; i < 2; i++) {
        plans[i] = sh_package_runtime_prepare_map(data, map[i], error, sizeof(error));
        if (!plans[i]) fprintf(stderr, "consumer prepare: %s\n", error);
        CHECK(plans[i]); if (!plans[i]) goto done;
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(plans[i]);
        const sh_compiled_resource *owner = historical_resource(compiled, owner_path), *leaf = historical_resource(compiled, leaf_path);
        CHECK(owner && owner->native_name && leaf && leaf->native_name); if (!owner || !leaf) goto done;
        strcpy_s(owners[i], sizeof(owners[i]), owner->native_name); strcpy_s(targets[i], sizeof(targets[i]), leaf->native_name);
    }
    CHECK(strcmp(owners[0], owners[1]) && strcmp(targets[0], targets[1]));
    CHECK(sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, consumer_activate, &state));
    CHECK(sh_package_runtime_consumer_acquire("entitydef", owners[0], &held) == 1);
    state.next = 1; state.previous = 0; state.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, consumer_activate, &state));
    CHECK(state.recoveries == 1 && sh_package_runtime_admission_ready());
    expect_consumer(owners[0], targets[0]); expect_consumer(owners[1], NULL);
    state.fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, consumer_activate, &state));
    CHECK(held.binding && !strcmp(sh_package_consumer_find(held.binding, "constructor/target")->name, targets[0]));
    /* Capture a copied A occurrence, then release both plans. Its own lease must
     * retain A's constructor bindings and dependency files beside the B map. */
    size_t rebound_length = 0, captured_length = 0;
    char *rebound = sh_package_map_plan_rebind(plans[0], NULL, json, strlen(json), &rebound_length, error, sizeof(error));
    if (!rebound) fprintf(stderr, "consumer rebind: %s\n", error);
    CHECK(rebound);
    if (rebound) {
        char *captured = sh_package_runtime_capture_document(plans[0], NULL, NULL, 0, rebound, rebound_length,
            &captured_length, &document, NULL, NULL, error, sizeof(error));
        if (!captured) fprintf(stderr, "consumer capture: %s\n", error);
        CHECK(captured && document); free(captured); HeapFree(GetProcessHeap(), 0, rebound);
    }
    CHECK(document); if (!document) goto done;
    state.retained = 1; state.previous = 1;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    for (int i = 0; i < 2; i++) { sh_package_map_plan_free(plans[i]); plans[i] = NULL; }
    state.next = -1;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    expect_consumer(owners[0], targets[0]); expect_consumer(owners[1], NULL);
    const sh_package_compilation *current = sh_package_runtime_acquire(); int retained_leaf = 0;
    for (size_t i = 0; current && i < current->resource_count; i++)
        if (sh_package_resource_native_matches(current->resources + i, "entitydef", targets[0])) retained_leaf = 1;
    CHECK(retained_leaf); sh_package_runtime_release();
    sh_package_occurrence_release(document); document = NULL;
    state.retained = 0; state.previous = -1;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    expect_consumer(owners[0], NULL);
    CHECK(held.binding && !strcmp(sh_package_consumer_find(held.binding, "constructor/target")->name, targets[0]));
done:
    sh_package_occurrence_release(document);
    for (int i = 0; i < 2; i++) sh_package_map_plan_free(plans[i]);
    sh_package_runtime_test_dispose(); sh_package_runtime_consumer_release(&held);
    sh_package_runtime_test_constructor_requests(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
static void test_historical_private_preparation(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/root\"}],\"note\":\"history/root\x97\"}";
    const char *part = "{ class = \"Entity\"; edit = { health = 100; target = \"history/child\"; } }";
    char data[MAX_PATH], map_a[MAX_PATH], map_b[MAX_PATH], vanilla[MAX_PATH], authored[MAX_PATH], error[2048];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *a = NULL, *b = NULL, *recorded = NULL, *stock = NULL, *equal = NULL;
    sh_package_provenance recipe = {0}, selected = {0};
    sh_package_sources *originals = NULL, *normalized = NULL;
    sh_package_bundle_binding rows[2] = {{0}};
    unsigned char *archives[2] = {0}; size_t archive_lengths[2] = {0};
    sh_package_occurrence_snapshot *ownership = NULL;
    sh_mpkg_used *used = NULL; size_t used_count = 0;
    const sh_package_compilation *ca = NULL, *cb;
    const sh_compiled_resource *ra, *rb;
    char *bound = NULL, *captured = NULL, *plain = NULL; size_t length = 0, captured_length = 0;
    create("historical-data/overrides", NULL); create("historical-vanilla/overrides", NULL);
    for (size_t i = 0; i < 3; i++) {
        char prefix[64], relative[256];
        if (i == 2) strcpy(prefix, "historical-original/overrides");
        else snprintf(prefix, sizeof(prefix), "historical-%c/overrides", (int)('a' + i));
        snprintf(relative, sizeof(relative), "%s/a/package.json", prefix);
        create(relative, i == 2 ? "{ \"id\": \"shared-id\", \"name\": \"Health contributor\" }" :
            "{\"id\":\"shared-id\",\"name\":\"Health contributor\"}");
        snprintf(relative, sizeof(relative), "%s/a/author.txt", prefix); create(relative, "retain this unused extra");
        snprintf(relative, sizeof(relative), "%s/a/assets/%s", prefix, historical_root);
        create(relative, i == 1 ? "{ class = \"Entity\"; edit = { health = 20; target = \"\"; } }" :
            "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
        snprintf(relative, sizeof(relative), "%s/b/package.json", prefix);
        create(relative, i == 2 ? "{\"id\":\"shared-id\",\"name\":\"Target contributor\"}" :
            "{ \"id\": \"shared-id\", \"name\": \"Target contributor\" }");
        snprintf(relative, sizeof(relative), "%s/b/assets/%s", prefix, historical_root); create(relative, part);
        snprintf(relative, sizeof(relative), "%s/b/assets/generated/decls/entitydef/history/child.decl", prefix);
        create(relative, "{ class = \"Entity\"; edit = { health = 7; } }");
    }
    snprintf(data, sizeof(data), "%s/historical-data", root);
    snprintf(map_a, sizeof(map_a), "%s/historical-a", root);
    snprintf(map_b, sizeof(map_b), "%s/historical-b", root);
    snprintf(vanilla, sizeof(vanilla), "%s/historical-vanilla", root);
    snprintf(authored, sizeof(authored), "%s/historical-original", root);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    originals = sh_package_sources_scan(authored, error, sizeof(error));
    normalized = sh_package_sources_scan(map_a, error, sizeof(error));
    CHECK(originals && normalized && originals->package_count == 2 && normalized->package_count == 2);
    if (!originals || !normalized) goto done;
    for (size_t i = 0; i < 2; i++) {
        strcpy_s(rows[i].source_name, sizeof(rows[i].source_name), normalized->packages[i].name);
        memcpy(rows[i].compiler_tree, normalized->fingerprints[i], 32);
        rows[i].original = sh_package_bundle_capture(originals, i, data, error, sizeof(error)); CHECK(rows[i].original);
        if (!rows[i].original) goto done;
        CHECK(memcmp(sh_package_bundle_fingerprint(rows[i].original), rows[i].compiler_tree, 32));
        archives[i] = sh_package_bundle_read(rows[i].original, archive_lengths + i, error, sizeof(error)); CHECK(archives[i]);
    }
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    a = sh_package_runtime_prepare_map_deliveries(data, map_a, NULL, rows, 2, error, sizeof(error));
    if (!a) fprintf(stderr, "historical prepare A: %s\n", error);
    CHECK(a); if (!a) goto done;
    ca = sh_package_map_plan_compilation(a); ra = historical_resource(ca, historical_root);
    CHECK(ra && ra->native_name && !strncmp(ra->native_name, "smp/", 4) &&
        strstr((const char *)ra->body, "health = 10") && strstr((const char *)ra->body, "smp/"));
    if (!ra) goto done;
    b = sh_package_runtime_prepare_map(data, map_b, error, sizeof(error)); CHECK(b); if (!b) goto done;
    cb = sh_package_map_plan_compilation(b); rb = historical_resource(cb, historical_root);
    CHECK(rb && rb->native_name && strcmp(ra->native_name, rb->native_name) &&
        strstr((const char *)rb->body, "health = 20"));
    /* The no-envelope recipe is equivalent to a recorded historical context,
     * and is complete even though both authors chose the same descriptor ID. */
    size_t context = sh_package_provenance_context_add(&recipe, SH_PACKAGE_CONTEXT_COMPOSED,
        ca->sources->original_fingerprints, ca->sources->package_count, NULL);
    recipe.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    CHECK(context != SIZE_MAX && sh_package_provenance_bind(&recipe, "entitydef", "history/root", "history/root", &context, 1));
    recorded = sh_package_runtime_prepare_map_deliveries(data, map_a, &recipe, rows, 2, error, sizeof(error));
    CHECK(recorded);
    const sh_compiled_resource *again = historical_resource(sh_package_map_plan_compilation(recorded), historical_root);
    CHECK(again && !strcmp(again->native_name, ra->native_name) && again->body_length == ra->body_length &&
        !memcmp(again->body, ra->body, ra->body_length));
    bound = sh_package_map_plan_rebind(a, NULL, json, strlen(json), &length, error, sizeof(error));
    if (!bound) fprintf(stderr, "historical rebind: %s\n", error);
    CHECK(bound && strstr(bound, ra->native_name) && strstr(bound, "\"note\":\"history/root\x97\""));
    if (bound) {
        int present = 0;
        CHECK(sh_map_provenance_read(bound, length, &selected, &present, error, sizeof(error)) && present);
        CHECK(selected.semantics == SH_PACKAGE_PROVENANCE_USES && selected.use_count == 1 &&
            selected.context_count == 1 && selected.contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED &&
            selected.contexts[0].tree_count == 2);
        for (size_t i = 0; selected.context_count && i < 2; i++) {
            int found = 0;
            for (size_t t = 0; t < selected.contexts[0].tree_count; t++)
                if (!memcmp(selected.contexts[0].trees[t], sh_package_bundle_fingerprint(rows[i].original), 32)) found = 1;
            CHECK(found);
        }
        captured = sh_package_runtime_capture_document(a, &selected, NULL, 0, bound, length,
            &captured_length, &ownership, &used, &used_count, error, sizeof(error));
        if (!captured) fprintf(stderr, "historical capture: %s\n", error);
        CHECK(captured && captured_length == length && !memcmp(captured, bound, length) && used_count == 2);
        sh_package_missing missing = {0}; sh_package_source_graph_report inspection = {0};
        CHECK(sh_package_map_plan_usage_missing(a, registry, (uintptr_t)f.reflection,
            bound, length, &missing, &inspection, error, sizeof(error)) && !missing.count && !inspection.gaps);
        sh_package_missing_free(&missing);
    }
    for (size_t i = 0; i < ca->sources->file_count; i++) if (!ca->sources->files[i].directory)
        CHECK(sh_package_source_verify(ca->sources->files + i));
    stock = sh_package_runtime_prepare_map(data, vanilla, error, sizeof(error)); CHECK(stock);
    CHECK(sh_package_map_plan_compilation(stock) && !sh_package_map_plan_compilation(stock)->sources->package_count);
    size_t plain_length = 0;
    plain = sh_package_map_plan_rebind(stock, NULL, json, strlen(json), &plain_length, error, sizeof(error));
    CHECK(plain && plain_length == strlen(json) && !memcmp(plain, json, plain_length));
    create("historical-equal/overrides/a/package.json", "{\"id\":\"shared-id\",\"name\":\"Unchanged original\"}");
    create("historical-equal/overrides/a/assets/generated/decls/entitydef/history/root.decl", historical_original);
    snprintf(authored, sizeof(authored), "%s/historical-equal", root);
    equal = sh_package_runtime_prepare_map(data, authored, error, sizeof(error)); CHECK(equal);
    const sh_compiled_resource *unchanged = historical_resource(sh_package_map_plan_compilation(equal), historical_root);
    CHECK(unchanged && unchanged->native_name && !strcmp(unchanged->native_name, "history/root") &&
        !sh_package_owners_count(&unchanged->gameplay_owners));
    /* An unsupported registered custom reader must not fall back to publishing
     * a global old-name resource after private context linking fails. */
    create("historical-gap/overrides/a/package.json", "{\"id\":\"shared-id\",\"name\":\"Unknown custom reader\"}");
    create("historical-gap/overrides/a/assets/generated/decls/entitydef/history/gap.decl",
        "{ class = \"Entity\"; edit = { custom = \"unproven reader\"; } }");
    snprintf(authored, sizeof(authored), "%s/historical-gap", root);
    sh_package_map_plan *gap = sh_package_runtime_prepare_map(data, authored, error, sizeof(error));
    CHECK(!gap && *error); sh_package_map_plan_free(gap);
    /* A same-ID available directory cannot repair a missing saved source. */
    recipe.contexts[0].trees[0][0] ^= 128;
    sh_package_map_plan *missing = sh_package_runtime_prepare_map_deliveries(data, map_a, &recipe, rows, 2, error, sizeof(error));
    CHECK(!missing); sh_package_map_plan_free(missing);
done:
    if (plain) HeapFree(GetProcessHeap(), 0, plain);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    free(captured);
    sh_package_provenance_free(&recipe); sh_package_provenance_free(&selected);
    sh_package_map_plan_free(equal); sh_package_map_plan_free(stock); sh_package_map_plan_free(recorded);
    sh_package_map_plan_free(b); sh_package_map_plan_free(a);
    for (size_t i = 0; i < 2; i++) { sh_package_bundle_release(rows[i].original); rows[i].original = NULL; }
    sh_package_occurrence_release(ownership);
    sh_package_sources_free(originals); sh_package_sources_free(normalized);
    for (size_t i = 0; i < used_count; i++) {
        size_t n = 0; unsigned char *bytes = sh_package_bundle_read(used[i].bundle, &n, error, sizeof(error));
        CHECK(bytes && ((n == archive_lengths[0] && !memcmp(bytes, archives[0], n)) ||
            (n == archive_lengths[1] && !memcmp(bytes, archives[1], n)))); free(bytes);
    }
    sh_mpkg_used_free(used); for (size_t i = 0; i < 2; i++) free(archives[i]);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

typedef struct lease_activation {
    const char *path;
    int before, after, fail, calls, recovered, removed;
    const char *capture_json;
    const sh_package_occurrence_snapshot *capture_saved;
} lease_activation;
static int lease_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    lease_activation *state = context; unsigned char *body = NULL; size_t length = 0;
    int present = sh_package_runtime_read(state->path, &body, &length);
    CHECK(present == (restoring ? state->before : state->after)); free(body);
    if (!restoring && state->capture_json) {
        size_t count = 0, captured_length = 0;
        const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(state->capture_saved, &count);
        sh_package_occurrence_snapshot *captured = NULL;
        char *json = sh_package_runtime_capture_document(NULL, sh_package_occurrence_provenance(state->capture_saved),
            rows, count, state->capture_json, strlen(state->capture_json), &captured_length,
            &captured, NULL, NULL, error, capacity);
        CHECK(json && captured && captured_length == strlen(state->capture_json));
        free(json); sh_package_occurrence_release(captured); state->capture_json = NULL;
    }
    for (size_t i = 0; !restoring && i < changes->count; i++)
        if (!strcmp(changes->items[i].path, state->path) && changes->items[i].kind == SH_PACKAGE_RESOURCE_REMOVED) state->removed++;
    if (restoring) { state->recovered++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "lease retirement fixture failure"); return 0; }
    return 1;
}
static void test_retained_occurrence_resources(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/root\"}]}";
    char data[MAX_PATH], map[2][MAX_PATH], error[2048], alias[1024] = "";
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plans[2] = {0};
    sh_package_occurrence_snapshot *documents[2] = {0}, *world = NULL, *exported = NULL, *recaptured = NULL;
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_key live = {1, 0x1000}, clipboard = {2, 0x2000}, undo = {3, 0x3000};
    uintptr_t object = live.object;
    char *bound[2] = {0}; size_t lengths[2] = {0};
    lease_activation state = {alias, 0, 1};
    create("lease-data/overrides", NULL);
    for (size_t i = 0; i < 2; i++) {
        char path[512];
        snprintf(path, sizeof(path), "lease-%c/overrides/source/package.json", (int)('a' + i));
        create(path, i ? "{\"id\":\"same\",\"name\":\"Original B\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},\"strings\":{\"en\":{\"fromB\":\"active B\"}}}" :
            "{\"id\":\"same\",\"name\":\"Original A\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}},\"strings\":{\"en\":{\"fromA\":\"old A\"}},\"hud\":{\"weapons\":{\"retained-a\":{\"ammo_display\":\"weapon\"}}}}");
        snprintf(path, sizeof(path), "lease-%c/overrides/source/assets/%s", (int)('a' + i), historical_root);
        create(path, "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
        snprintf(path, sizeof(path), "lease-%c/overrides/source/author.txt", (int)('a' + i));
        create(path, i ? "B complete original" : "A complete original");
        snprintf(map[i], sizeof(map[i]), "%s/lease-%c", root, (int)('a' + i));
    }
    snprintf(data, sizeof(data), "%s/lease-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    for (size_t i = 0; i < 2; i++) {
        plans[i] = sh_package_runtime_prepare_map(data, map[i], error, sizeof(error)); CHECK(plans[i]);
        if (!plans[i]) goto done;
        const sh_compiled_resource *resource = historical_resource(sh_package_map_plan_compilation(plans[i]), historical_root);
        CHECK(resource && resource->original_path); if (!resource) goto done;
        if (!i) strcpy_s(alias, sizeof(alias), resource->engine_path); else CHECK(!strcmp(alias, resource->engine_path));
        size_t n = 0; char *rebound = sh_package_map_plan_rebind(plans[i], NULL, json, strlen(json), &n, error, sizeof(error)); CHECK(rebound);
        if (rebound) {
            bound[i] = sh_package_runtime_capture_document(plans[i], NULL, NULL, 0, rebound, n,
                lengths + i, documents + i, NULL, NULL, error, sizeof(error));
            HeapFree(GetProcessHeap(), 0, rebound);
        }
        if (!bound[i]) fprintf(stderr, "lease capture: %s\n", error);
        CHECK(bound[i] && documents[i]); if (!documents[i]) goto done;
    }
    CHECK(sh_package_occurrence_attach_snapshot(tracker, live.namespace_id, &object, 1, documents[0], &world, error, sizeof(error)));
    CHECK(sh_package_occurrence_copy(tracker, clipboard, live)); CHECK(sh_package_occurrence_copy(tracker, undo, live));
    sh_package_occurrence_erase_namespace(tracker, live.namespace_id);
    sh_package_occurrence_release(documents[0]); documents[0] = NULL;
    sh_package_occurrence_release(world); world = NULL;
    state.capture_json = bound[1]; state.capture_saved = documents[1];
    CHECK(sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, lease_activate, &state));
    size_t length; char *policy = sh_package_runtime_policy("strings", &length);
    CHECK(policy && strstr(policy, "active B") && !strstr(policy, "old A")); free(policy);
    policy = sh_package_runtime_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useImageBlackList") && !strstr(policy, "g_useResourceBlackList")); free(policy);
    sh_package_occurrence_release(documents[1]); documents[1] = NULL;
    sh_package_map_plan_free(plans[0]); plans[0] = NULL; sh_package_map_plan_free(plans[1]); plans[1] = NULL;
    /* Serving must not recompile changed source folders after their maps exit. */
    create("lease-a/overrides/source/package.json", "changed invalid author folder");
    create("lease-a/overrides/source/assets/generated/decls/entitydef/history/root.decl", "{ changed = 999; }");
    state.before = state.after = 1;
    CHECK(sh_package_runtime_activate_map(data, NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    expect_current(alias, "health = 10"); CHECK(!sh_package_runtime_has_map_provider());
    policy = sh_package_runtime_policy("requirements", &length); CHECK(policy && !strstr(policy, "g_useResourceBlackList")); free(policy);
    exported = sh_package_occurrence_export(tracker, &clipboard, 1, NULL, error, sizeof(error)); CHECK(exported);
    size_t rows_count = 0; const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(exported, &rows_count);
    CHECK(rows_count == 1);
    if (rows_count) {
        size_t archive_length = 0; unsigned char *archive = sh_package_bundle_read(rows[0].original, &archive_length, error, sizeof(error));
        sh_package_archive_files members = {0}; CHECK(archive && sh_package_archive_read(archive, archive_length, &members, error, sizeof(error)));
        int original = 0;
        for (size_t i = 0; i < members.count; i++) if (!strcmp(members.items[i].name, "author.txt"))
            original = members.items[i].length == 19 && !memcmp(members.items[i].body, "A complete original", 19);
        CHECK(original); sh_package_archive_files_free(&members); free(archive);
    }
    /* Selecting A's actual occurrence promotes A, even though B shared its alias. */
    CHECK(sh_package_runtime_select_map_occurrences(bound[0], lengths[0], NULL, exported, error, sizeof(error)));
    policy = sh_package_runtime_active_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    int calls = state.calls;
    char *captured = sh_package_runtime_capture_document(NULL, sh_package_occurrence_provenance(exported),
        rows, rows_count, bound[0], lengths[0], &length, &recaptured, NULL, NULL, error, sizeof(error));
    CHECK(captured && recaptured && state.calls == calls); free(captured);
    sh_package_occurrence_release(recaptured); recaptured = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    policy = sh_package_runtime_policy("requirements", &length); CHECK(policy && strstr(policy, "g_useResourceBlackList")); free(policy);
    sh_package_occurrence_erase(tracker, undo); CHECK(state.calls == calls + 1);
    sh_package_occurrence_erase(tracker, clipboard); sh_package_occurrence_release(exported); exported = NULL;
    CHECK(sh_package_runtime_occurrences_pending()); expect_current(alias, "health = 10");
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, error, sizeof(error)));
    state.after = 0; state.fail = 1;
    CHECK(!sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    CHECK(state.recovered == 1 && sh_package_runtime_occurrences_pending()); expect_current(alias, "health = 10");
    state.fail = 0;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    CHECK(!sh_package_runtime_occurrences_pending() && state.removed == 2);
    const sh_package_compilation *current = sh_package_runtime_acquire();
    CHECK(current && !current->sources->package_count && !sh_package_compilation_find(current, alias)); sh_package_runtime_release();
done:
    sh_package_occurrence_release(recaptured); sh_package_occurrence_release(exported); sh_package_occurrence_release(world);
    for (size_t i = 0; i < 2; i++) { sh_package_occurrence_release(documents[i]); sh_package_map_plan_free(plans[i]); free(bound[i]); }
    sh_package_occurrence_tracker_free(tracker);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

static void lease_collection_setup(admission_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo", "int",
        "idDeclSnapEditorEntity", "idList < EntityRefs >", "idDeclTypeInfoGraph"};
    const char *types[] = {"entityDef", "snapEditorEntityDef"};
    const size_t indices[] = {0, 4};
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18);
    void *managers = admission_alloc(f, 16);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 2);
    for (size_t i = 0; i < 2; i++) {
        void *manager = admission_alloc(f, 0x90); admission_pointer(managers, i * 8, manager);
        admission_pointer(manager, 8, admission_string(f, types[i]));
        admission_pointer(manager, 0x10, admission_string(f, classes[indices[i]]));
    }
    f->reflection = admission_alloc(f, 0xb0);
    void *container = admission_alloc(f, 0x30), *records = admission_alloc(f, 8 * 0x38);
    void *objects = admission_alloc(f, 8 * 16), *pointers = admission_alloc(f, 8 * 16);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 8);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 8);
    for (size_t i = 0; i < 7; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, i == 4 || i == 6 ? "idDeclTypeInfo" : ""));
    }
    admission_pointer(pointers, 8, (void *)0x10); admission_pointer(pointers, 4 * 16 + 8, (void *)0x10);
    admission_pointer(objects, 5 * 16 + 8, (void *)0x80);
    admission_pointer(objects, 6 * 16 + 8, (void *)0x30);
    void *entity = admission_alloc(f, 3 * 0x48), *editor = admission_alloc(f, 3 * 0x48);
    void *element = admission_alloc(f, 0x48);
    admission_pointer(records, 0x38 + 0x20, entity); admission_pointer(records, 4 * 0x38 + 0x20, editor);
    admission_pointer(records, 5 * 0x38 + 0x18, element);
    void *rows[] = {entity, (char *)entity + 0x48, editor, element, (char *)editor + 0x48};
    const char *field_types[] = {"idDeclEntityDef", "int", "idList < EntityRefs >", "idDeclEntityDef", "idDeclEntityDef"};
    const char *operators[] = {"*", "", "", "*", "*"}, *names[] = {"target", "health", "inputs", "element", "entityDef"};
    for (size_t i = 0; i < 5; i++) {
        admission_pointer(rows[i], 0, admission_string(f, field_types[i]));
        admission_pointer(rows[i], 8, admission_string(f, operators[i]));
        admission_pointer(rows[i], 0x10, admission_string(f, names[i])); admission_number(rows[i], 0x1c, 8);
    }
}
static int lease_collection_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; const char *text = NULL; *body = NULL; *length = 0;
    if (!strcmp(path, "generated/decls/entitydef/cross/root.decl") ||
        !strcmp(path, "generated/decls/entitydef/cross/action.decl"))
        text = "{ class = \"Entity\"; edit = { health = 0; target = \"\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/cross/controls.decl"))
        text = "{ edit = { entityDef = \"cross/root\"; inputs = { num = 0; } } }";
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void test_retained_bound_dependencies(void)
{
    char data[MAX_PATH], map[MAX_PATH], error[2048], dependency[1024] = "", json[2048];
    char action_name[1024] = "", absent_name[1024] = "", source_root_path[1024] = "";
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_sources *sources = NULL;
    sh_package_map_plan *plan = NULL;
    sh_package_provenance recipe = {0}, uses = {0};
    sh_package_bundle_binding row = {0};
    sh_package_occurrence_snapshot *document = NULL, *direct = NULL, *unserved = NULL;
    char *direct_json = NULL;
    lease_activation state = {dependency, 0, 1};
    create("lease-cross-data/overrides", NULL);
    for (size_t i = 0; i < 2; i++) {
        char path[512];
        snprintf(path, sizeof(path), "lease-cross/overrides/%c/package.json", (int)('a' + i));
        create(path, i ? "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}" :
            "{\"id\":\"a\",\"name\":\"A\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
        snprintf(path, sizeof(path), "lease-cross/overrides/%c/assets/generated/decls/entitydef/cross/root.decl", (int)('a' + i));
        create(path, i ? "{ class = \"Entity\"; edit = { health = 20; target = \"\"; } }" :
            "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
        snprintf(path, sizeof(path), "lease-cross/overrides/%c/assets/generated/decls/entitydef/cross/action.decl", (int)('a' + i));
        create(path, i ? "{ class = \"Entity\"; edit = { health = 22; target = \"\"; } }" :
            "{ class = \"Entity\"; edit = { health = 11; target = \"\"; } }");
        snprintf(path, sizeof(path), "lease-cross/overrides/%c/assets/generated/decls/snapeditorentitydef/cross/controls.decl", (int)('a' + i));
        create(path, "{ edit = { entityDef = \"cross/root\"; inputs = { num = 1; item[0] = \"cross/action\"; } } }");
        snprintf(path, sizeof(path), "lease-cross/overrides/%c/assets/generated/decls/snapeditorentitydef/cross/inherited.decl", (int)('a' + i));
        create(path, "{ inherit = \"cross/controls\"; edit = {} }");
    }
    snprintf(data, sizeof(data), "%s/lease-cross-data", root); snprintf(map, sizeof(map), "%s/lease-cross", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data)); sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sources = sh_package_sources_scan(map, error, sizeof(error)); CHECK(sources && sources->package_count == 2);
    if (!sources || sources->package_count != 2) goto done;
    for (size_t i = 0; i < 2; i++) CHECK(sh_package_provenance_context_add(&recipe,
        SH_PACKAGE_CONTEXT_ISOLATED, sources->fingerprints + i, 1, "") != SIZE_MAX);
    plan = sh_package_runtime_prepare_map_with_bindings(data, map, &recipe, error, sizeof(error));
    if (!plan) fprintf(stderr, "cross-context prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    const sh_package_compilation *compiled = sh_package_map_plan_compilation(plan);
    const sh_compiled_resource *entity = NULL, *action = NULL, *selector = NULL;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->body && strstr((char *)r->body, "health = 10")) entity = r;
        if (r->body && strstr((char *)r->body, "health = 22")) action = r;
        if (r->body && strstr((char *)r->body, "health = 20")) strcpy_s(absent_name, sizeof(absent_name), r->native_name);
    }
    for (size_t i = 0; entity && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->type && !strcmp(r->type, "snapeditorentitydef") && r->body &&
            strstr((char *)r->body, entity->native_name)) selector = r;
    }
    CHECK(entity && action && selector); if (!entity || !action || !selector) goto done;
    int edge = 0;
    for (size_t i = 0; i < selector->bound_dependency_count; i++)
        if (!strcmp(selector->bound_dependencies[i], action->engine_path)) edge = 1;
    CHECK(edge && sh_package_owners_count(&action->owners) == 1 && !sh_package_owners_contains(&action->owners, 0));
    int companion = 0, inherited = 0;
    for (size_t i = 0; i < entity->bound_dependency_count; i++) {
        const sh_compiled_resource *r = sh_package_compilation_find(compiled, entity->bound_dependencies[i]);
        if (r == selector) companion = 1;
        if (r && r->body && strstr((char *)r->body, "inherit =")) inherited = 1;
    }
    CHECK(companion && inherited);
    strcpy_s(dependency, sizeof(dependency), action->engine_path);
    strcpy_s(action_name, sizeof(action_name), action->native_name);
    strcpy_s(source_root_path, sizeof(source_root_path), entity->engine_path);
    size_t context = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED, sources->fingerprints, 1, "");
    CHECK(context != SIZE_MAX && sh_package_provenance_bind(&uses, "entitydef", "cross/root", entity->native_name, &context, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", "/k:inherit"};
    CHECK(sh_package_provenance_use_set(&uses, &site, 0, &context, 1));
    strcpy_s(row.source_name, sizeof(row.source_name), sources->packages[0].name);
    memcpy(row.compiler_tree, sources->fingerprints[0], 32);
    row.original = sh_package_bundle_capture(sources, 0, data, error, sizeof(error)); CHECK(row.original);
    document = sh_package_occurrence_prepare_document(&uses, &row, 1, error, sizeof(error)); CHECK(document);
    CHECK(sh_package_runtime_bind_occurrence_resources(plan, document, error, sizeof(error)));
    snprintf(json, sizeof(json), "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", entity->native_name);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, lease_activate, &state));
    sh_package_map_plan_free(plan); plan = NULL;
    create("lease-cross/overrides/b/package.json", "source folder no longer contains the captured delivery");
    create("lease-cross/overrides/b/assets/generated/decls/entitydef/cross/action.decl", "{ changed = 999; }");
    state.before = state.after = 1;
    CHECK(sh_package_runtime_activate_map(data, NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    expect_current(dependency, "health = 22");
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_package_provenance *bindings = sh_package_runtime_bindings();
    CHECK(current && current->sources->package_count == 2 && bindings && bindings->context_count == 2);
    int action_catalog = 0, absent_catalog = 0;
    if (bindings) for (size_t i = 0; i < bindings->binding_count; i++) {
        if (!strcmp(bindings->bindings[i].name, action_name)) action_catalog = 1;
        if (!strcmp(bindings->bindings[i].name, absent_name)) absent_catalog = 1;
    }
    CHECK(action_catalog && !absent_catalog);
    sh_package_runtime_release();
    CHECK(sh_package_runtime_select_map_occurrences(json, strlen(json), NULL, document, error, sizeof(error)));
    size_t length = 0; char *policy = sh_package_runtime_active_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    /* Complete B delivery ownership does not authorize an unserved B variant. */
    snprintf(json, sizeof(json), "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", absent_name);
    char *rejected = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, json, strlen(json),
        &length, &unserved, NULL, NULL, error, sizeof(error));
    CHECK(!rejected && !unserved); free(rejected);
    /* A retained control creates a new B root. Capture must use B's original
     * immutable context, independently of A and the externally changed folder. */
    snprintf(json, sizeof(json), "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", action_name);
    int capture_calls = state.calls;
    direct_json = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, json, strlen(json),
        &length, &direct, NULL, NULL, error, sizeof(error));
    if (!direct_json) fprintf(stderr, "retained direct capture: %s\n", error);
    CHECK(direct_json && direct && state.calls == capture_calls);
    if (!direct) goto done;
    const sh_package_provenance *direct_uses = sh_package_occurrence_provenance(direct);
    CHECK(direct_uses->use_count == 1 && direct_uses->context_count == 1 &&
        !memcmp(direct_uses->contexts[0].trees[0], sources->fingerprints[1], 32));
    size_t bundle_count = 0; const sh_package_bundle_binding *direct_rows = sh_package_occurrence_bundles(direct, &bundle_count);
    CHECK(bundle_count == 1 && !memcmp(sh_package_bundle_fingerprint(direct_rows[0].original), sources->fingerprints[1], 32));
    if (bundle_count != 1) goto done;
    size_t archive_length = 0; unsigned char *archive = sh_package_bundle_read(direct_rows[0].original,
        &archive_length, error, sizeof(error));
    sh_package_archive_files members = {0}; CHECK(archive && sh_package_archive_read(archive, archive_length, &members, error, sizeof(error)));
    int original_descriptor = 0;
    const char *expected_descriptor = "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}";
    for (size_t i = 0; i < members.count; i++) if (!strcmp(members.items[i].name, "package.json"))
        original_descriptor = members.items[i].length == strlen(expected_descriptor) &&
            !memcmp(members.items[i].body, expected_descriptor, members.items[i].length);
    CHECK(original_descriptor); sh_package_archive_files_free(&members); free(archive);
    CHECK(sh_package_runtime_select_map_occurrences(direct_json, length, NULL, direct, error, sizeof(error)));
    policy = sh_package_runtime_active_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useImageBlackList") && !strstr(policy, "g_useResourceBlackList")); free(policy);
    int calls = state.calls; sh_package_occurrence_release(document); document = NULL;
    CHECK(state.calls == calls && sh_package_runtime_occurrences_pending()); expect_current(dependency, "health = 22");
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    expect_current(dependency, "health = 22"); expect_current(source_root_path, NULL);
    sh_package_occurrence_release(direct); direct = NULL;
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, error, sizeof(error)));
    state.after = 0;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &state));
    CHECK(state.removed == 1 && !sh_package_runtime_occurrences_pending()); expect_current(dependency, NULL);
done:
    sh_package_occurrence_release(document); sh_package_occurrence_release(direct); sh_package_occurrence_release(unserved);
    free(direct_json); sh_package_map_plan_free(plan); sh_package_bundle_release(row.original);
    sh_package_provenance_free(&recipe); sh_package_provenance_free(&uses); sh_package_sources_free(sources);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

typedef struct additive_activation {
    const char *old_path, *new_path, *dependency, *unused;
    int calls, recoveries, fail, finalizations, finalize_fail, recovery_fail;
} additive_activation;
static int additive_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    additive_activation *state = context;
    unsigned char *body = NULL; size_t length = 0;
    CHECK(sh_package_runtime_has_map_provider());
    expect_current(state->old_path, "health = 10");
    CHECK(sh_package_runtime_read(state->new_path, &body, &length) == !restoring); free(body); body = NULL;
    CHECK(sh_package_runtime_read(state->dependency, &body, &length) == !restoring); free(body); body = NULL;
    CHECK(sh_package_runtime_read(state->unused, &body, &length) == 0); free(body);
    for (size_t i = 0; i < changes->count; i++)
        CHECK(changes->items[i].kind == (restoring ? SH_PACKAGE_RESOURCE_REMOVED : SH_PACKAGE_RESOURCE_ADDED));
    char *policy = sh_package_runtime_active_policy("hud", &length);
    CHECK(policy && strstr(policy, "original-map") && !strstr(policy, "imported-prefab")); free(policy);
    policy = sh_package_runtime_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") &&
        (!!strstr(policy, "g_useImageBlackList") == !restoring)); free(policy);
    if (restoring) {
        state->recoveries++;
        if (state->recovery_fail) { snprintf(error, capacity, "fixture retained recovery"); return 0; }
        return 1;
    }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "fixture additive failure"); return 0; }
    return 1;
}
static int additive_finalize(void *context, char *error, size_t capacity)
{
    additive_activation *state = context;
    state->finalizations++;
    CHECK(sh_package_runtime_has_map_provider());
    expect_current(state->old_path, "health = 10");
    expect_current(state->new_path, "health = 20");
    if (state->finalize_fail == 2) RaiseException(0xe0340001, 0, 0, NULL);
    if (state->finalize_fail) { snprintf(error, capacity, "fixture install commit failure"); return 0; }
    return 1;
}
static void test_additive_occurrence_admission(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/root\"}]}";
    char data[MAX_PATH], paths[2][MAX_PATH], error[2048], old_path[1024] = "", new_path[1024] = "";
    char dependency[1024] = "", unused[1024] = "";
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plans[2] = {0}; sh_package_occurrence_snapshot *documents[2] = {0};
    char *bound[2] = {0}; size_t lengths[2] = {0};
    additive_activation state = {old_path, new_path, dependency, unused};
    create("additive-data/overrides", NULL);
    create("additive-data/overrides/tools/package.json", "{\"id\":\"local-editor-tools\",\"name\":\"Local Editor Tools\"}");
    create("additive-data/overrides/tools/assets/generated/decls/entitydef/history/stockbase.decl",
        "{ class = \"Entity\"; editorVars = { localEditorExtension = true; } edit = { health = 1; target = \"\"; } }");
    create("additive-map/overrides/a/package.json", "{\"id\":\"same\",\"name\":\"Original Map\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}},\"hud\":{\"weapons\":{\"original-map\":{\"ammo_display\":\"weapon\"}}}}");
    create("additive-map/overrides/a/assets/generated/decls/entitydef/history/root.decl", "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
    create("additive-prefab/overrides/b/package.json", "{\"id\":\"same\",\"name\":\"Imported Prefab\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},\"hud\":{\"weapons\":{\"imported-prefab\":{\"ammo_display\":\"weapon\"}}}}");
    create("additive-prefab/overrides/b/assets/generated/decls/entitydef/history/root.decl", "{ class = \"Entity\"; edit = { health = 20; target = \"history/child\"; } }");
    create("additive-prefab/overrides/b/assets/generated/decls/entitydef/history/child.decl", "{ class = \"Entity\"; edit = { health = 30; target = \"history/stockbase\"; } }");
    create("additive-prefab/overrides/b/assets/generated/decls/entitydef/history/stockbase.decl",
        "{ class = \"Entity\"; edit = { health = 1; target = \"\"; } }");
    create("additive-prefab/overrides/b/assets/generated/decls/entitydef/history/unused.decl", "{ class = \"Entity\"; edit = { health = 40; target = \"\"; } }");
    create("additive-prefab/overrides/b/author.txt", "intact unused delivery bytes");
    snprintf(data, sizeof(data), "%s/additive-data", root);
    snprintf(paths[0], sizeof(paths[0]), "%s/additive-map", root);
    snprintf(paths[1], sizeof(paths[1]), "%s/additive-prefab", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    expect_current("generated/decls/entitydef/history/stockbase.decl", "localEditorExtension");
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    for (size_t i = 0; i < 2; i++) {
        plans[i] = sh_package_runtime_prepare_map(data, paths[i], error, sizeof(error)); CHECK(plans[i]);
        if (!plans[i]) goto done;
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(plans[i]);
        const sh_compiled_resource *resource = historical_resource(compiled, historical_root); CHECK(resource);
        if (!resource) goto done;
        strcpy_s(i ? new_path : old_path, 1024, resource->engine_path);
        if (i) {
            resource = historical_resource(compiled, "generated/decls/entitydef/history/child.decl"); CHECK(resource);
            if (resource) strcpy_s(dependency, sizeof(dependency), resource->engine_path);
            resource = historical_resource(compiled, "generated/decls/entitydef/history/unused.decl"); CHECK(resource);
            if (resource) strcpy_s(unused, sizeof(unused), resource->engine_path);
        }
        size_t length = 0; char *rebound = sh_package_map_plan_rebind(plans[i], NULL, json, strlen(json), &length, error, sizeof(error)); CHECK(rebound);
        if (rebound) {
            bound[i] = sh_package_runtime_capture_document(plans[i], NULL, NULL, 0, rebound, length,
                lengths + i, documents + i, NULL, NULL, error, sizeof(error)); HeapFree(GetProcessHeap(), 0, rebound);
        }
        CHECK(bound[i] && documents[i]); if (!documents[i]) goto done;
        if (!i) {
            CHECK(sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, NULL, NULL));
            CHECK(sh_package_runtime_select_map_occurrences(bound[0], lengths[0], NULL, documents[0], error, sizeof(error)));
        }
    }
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(!sh_package_runtime_commit_occurrences(plans[1], documents[1], commit_inventory, error, sizeof(error)) && !inventory_commit_calls);
    CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], NULL, (sh_package_activation_guard){0},
        additive_activate, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], documents[0], (sh_package_activation_guard){0},
        additive_activate, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(!state.calls && !state.recoveries && sh_package_runtime_admission_ready());
    state.fail = 1;
    CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        additive_activate, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(state.calls == 1 && state.recoveries == 1 && sh_package_runtime_admission_ready() && sh_package_runtime_ready());
    CHECK(!sh_package_runtime_commit_occurrences(plans[1], documents[1], commit_inventory, error, sizeof(error)) && !inventory_commit_calls);
    state.fail = 0;
    for (int failure = 1; failure <= 2; failure++) {
        state.finalize_fail = failure;
        CHECK(sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
            additive_activate, additive_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
        CHECK(state.calls == 1 + failure && state.recoveries == 1 + failure && state.finalizations == failure);
        CHECK(sh_package_runtime_admission_ready() && sh_package_runtime_ready());
        CHECK(strstr(error, failure == 1 ? "fixture install commit failure" : "exception 0xe0340001"));
        CHECK(!sh_package_runtime_commit_occurrences(plans[1], documents[1], commit_inventory, error, sizeof(error)) && !inventory_commit_calls);
    }
    state.finalize_fail = state.recovery_fail = 1;
    CHECK(sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        additive_activate, additive_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(!sh_package_runtime_ready() && !sh_package_runtime_admission_ready());
    CHECK(!sh_package_runtime_complete_occurrence_recovery(NULL));
    expect_current(old_path, "health = 10");
    state.recovery_fail = 0;
    CHECK(sh_package_runtime_complete_occurrence_recovery(&state));
    CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    CHECK(!sh_package_runtime_complete_occurrence_recovery(&state));
    state.finalize_fail = 0;
    int admitted = sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        additive_activate, additive_finalize, &state, error, sizeof(error));
    if (admitted != SH_PACKAGE_ADMISSION_READY) fprintf(stderr, "additive admission: %s\n", error);
    CHECK(admitted == SH_PACKAGE_ADMISSION_READY && state.calls == 5 && state.recoveries == 4 && state.finalizations == 4);
    const sh_package_compilation *current = sh_package_runtime_acquire();
    size_t resource_count = current ? current->resource_count : 0, owner_count = current ? current->sources->package_count : 0;
    sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        additive_activate, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_READY);
    current = sh_package_runtime_acquire();
    CHECK(current && current->resource_count == resource_count && current->sources->package_count == owner_count);
    sh_package_runtime_release(); CHECK(state.calls == 6 && state.recoveries == 4);
    lease_activation held = {new_path, 1, 1};
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &held));
    unsigned char *unwanted = NULL; size_t unwanted_length = 0;
    CHECK(sh_package_runtime_read(unused, &unwanted, &unwanted_length) == 0); free(unwanted);
    expect_current(dependency, "health = 30"); expect_current(old_path, "health = 10");
    expect_current("generated/decls/entitydef/history/stockbase.decl", "localEditorExtension");
    inventory_commit_failure = 1;
    CHECK(!sh_package_runtime_commit_occurrences(plans[1], documents[1], commit_inventory, error, sizeof(error)) && inventory_commit_calls == 1);
    inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_occurrences(plans[1], documents[1], commit_inventory, error, sizeof(error)) && inventory_commit_calls == 2);
    CHECK(sh_package_runtime_commit_map(plans[0], commit_inventory, error, sizeof(error)) && inventory_commit_calls == 3);
    CHECK(!sh_package_runtime_commit_map(plans[1], commit_inventory, error, sizeof(error)) && inventory_commit_calls == 3);
    expect_current(old_path, "health = 10"); expect_current(new_path, "health = 20"); expect_current(dependency, "health = 30");
    create("additive-prefab/overrides/b/package.json", "external mutation after immutable admission");
    size_t bundle_count = 0; const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(documents[1], &bundle_count);
    CHECK(bundle_count == 1 && rows && rows[0].original);
    sh_package_map_plan_free(plans[1]); plans[1] = NULL;
    sh_package_occurrence_release(documents[1]); documents[1] = NULL;
    CHECK(sh_package_runtime_occurrences_pending()); expect_current(new_path, "health = 20");
    lease_activation retired = {new_path, 1, 0};
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate, &retired));
    CHECK(retired.removed == 1 && sh_package_runtime_has_map_provider()); expect_current(old_path, "health = 10");
done:
    for (size_t i = 0; i < 2; i++) { sh_package_occurrence_release(documents[i]); sh_package_map_plan_free(plans[i]); free(bound[i]); }
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

static const char *authoring_unused = "generated/decls/entitydef/cross/unused.decl";
static const char *authoring_selector = "generated/decls/snapeditorentitydef/cross/sibling.decl";
static void authoring_bundle(const char *prefix, const char *descriptor)
{
    const char *paths[] = {"package.json", "assets/generated/decls/entitydef/cross/root.decl",
        "assets/generated/decls/entitydef/cross/action.decl", "assets/generated/decls/entitydef/cross/unused.decl",
        "assets/generated/decls/snapeditorentitydef/cross/controls.decl",
        "assets/generated/decls/snapeditorentitydef/cross/sibling.decl", "author.txt"};
    const char *bodies[] = {descriptor,
        "{ class = \"Entity\"; edit = { health = 20; target = \"cross/action\"; } }",
        "{ class = \"Entity\"; edit = { health = 30; target = \"\"; } }",
        "{ class = \"Entity\"; edit = { health = 40; target = \"\"; } }",
        "{ edit = { entityDef = \"cross/root\"; inputs = { num = 1; item[0] = \"cross/action\"; } } }",
        "{ edit = { entityDef = \"cross/unused\"; inputs = { num = 0; } } }",
        "complete unused author document"};
    char path[1024];
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", prefix, paths[i]); create(path, bodies[i]);
    }
}
typedef struct installed_authoring_activation {
    const sh_package_compilation *library;
    sh_package_selector_hold *selector;
    const char *map_path, *imported_path;
    int calls, recoveries, finalizations, fail, finalize_fail;
} installed_authoring_activation;
static void authoring_visible(int present)
{
    const sh_package_compilation *compiled = sh_package_runtime_acquire();
    const sh_compiled_resource *unused = historical_resource(compiled, authoring_unused);
    const sh_compiled_resource *selector = historical_resource(compiled, authoring_selector);
    CHECK((unused != NULL) == present && (selector != NULL) == present);
    if (unused && selector) {
        CHECK(unused->body && strstr((char *)unused->body, "health = 40"));
        CHECK(selector->body && strstr((char *)selector->body, unused->native_name));
        CHECK(unused->original_path && selector->original_path);
    }
    sh_package_runtime_release();
}
static int installed_authoring_activate(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    installed_authoring_activation *state = context;
    CHECK(sh_package_runtime_has_map_provider());
    expect_current(state->map_path, "health = 10");
    expect_current("generated/decls/snapeditorentitydef/cross/controls.decl", "cross/localaction");
    authoring_visible(!restoring);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library == state->library && !historical_resource(library, authoring_unused));
    sh_package_runtime_release();
    for (size_t i = 0; i < changes->count; i++) CHECK(changes->items[i].kind ==
        (restoring ? SH_PACKAGE_RESOURCE_REMOVED : SH_PACKAGE_RESOURCE_ADDED));
    size_t length = 0; char *policy = sh_package_runtime_active_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    if (restoring) {
        sh_package_runtime_selectors_release(state->selector); state->selector = NULL;
        state->recoveries++; return 1;
    }
    state->calls++;
    CHECK(!state->selector);
    state->selector = sh_package_runtime_selectors_retain(error, capacity); CHECK(state->selector);
    if (!state->selector) return 0;
    expect_current(state->imported_path, "health = 20");
    if (state->fail) { snprintf(error, capacity, "fixture authoring native failure"); return 0; }
    return 1;
}
static int installed_authoring_finalize(void *context, char *error, size_t capacity)
{
    installed_authoring_activation *state = context;
    state->finalizations++; authoring_visible(1);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library == state->library); sh_package_runtime_release();
    if (state->finalize_fail == 2) RaiseException(0xe0380001, 0, 0, NULL);
    if (state->finalize_fail) { snprintf(error, capacity, "fixture authoring disk refusal"); return 0; }
    sh_package_runtime_note_sources_changed();
    return 1;
}
static int initial_authoring_activate(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    installed_authoring_activation *state = context;
    (void)changes;
    authoring_visible(!restoring);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library == state->library && !historical_resource(library, authoring_unused));
    sh_package_runtime_release();
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "fixture first-map preparation failure"); return 0; }
    return 1;
}
static void test_initial_installed_authoring(void)
{
    const char *descriptor = "{\"id\":\"first-installed\",\"name\":\"First installed\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048], unused_path[1024] = "";
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    create("authoring-first-data/overrides", NULL);
    authoring_bundle("authoring-first-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/authoring-first-data", root);
    snprintf(source, sizeof(source), "%s/authoring-first-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("authoring-first-data/overrides/installed", descriptor);
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    CHECK(prepared); if (!prepared) goto done;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    CHECK(state.calls == 1 && state.recoveries == 1 && sh_package_runtime_admission_ready());
    state.fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    CHECK(state.calls == 2 && state.recoveries == 1);
    inventory_commit_calls = 0; inventory_commit_failure = 1;
    CHECK(!sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library == state.library); sh_package_runtime_release();
    inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 2);
    library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *unused = historical_resource(library, authoring_unused);
    CHECK(library != state.library && unused);
    if (unused) strcpy_s(unused_path, sizeof(unused_path), unused->engine_path);
    sh_package_runtime_release();
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, lease_activate,
        &(lease_activation){unused_path, 1, 1}));
    CHECK(!sh_package_runtime_has_map_provider()); authoring_visible(1);
done:
    inventory_commit_failure = 0;
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
typedef struct selector_slot_activation {
    const char *served;
    int calls;
} selector_slot_activation;
static int selector_slot_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    selector_slot_activation *state = context;
    (void)changes; (void)error; (void)capacity;
    CHECK(!restoring); state->calls++;
    expect_current(health_path, state->served); expect_library("health = 20");
    return 1;
}
static void test_selector_shared_retirement(void)
{
    char data[MAX_PATH], error[2048];
    sh_package_selector_hold *first = NULL, *second = NULL;
    selector_slot_activation state = {"health = 10", 0};
    create("selector-slot-data/overrides/current/package.json", "{\"id\":\"selector-slot\",\"name\":\"Old slot\","
        "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},\"strings\":{\"en\":{\"slot\":\"old\"}},"
        "\"hud\":{\"weapons\":{\"weapons/pistol\":{\"ammo_display\":\"weapon\"}}}}");
    create("selector-slot-data/overrides/current/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 10; } }");
    snprintf(data, sizeof(data), "%s/selector-slot-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    first = sh_package_runtime_selectors_retain(error, sizeof(error));
    second = sh_package_runtime_selectors_retain(error, sizeof(error));
    CHECK(first && second); if (!first || !second) goto done;
    create("selector-slot-data/overrides/current/package.json", "{\"id\":\"selector-slot\",\"name\":\"New slot\","
        "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}},\"strings\":{\"en\":{\"slot\":\"new\"}},"
        "\"hud\":{\"weapons\":{\"weapons/pistol\":{\"ammo_display\":\"engine\"}}}}");
    create("selector-slot-data/overrides/current/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 20; } }");
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, selector_slot_activate, &state));
    CHECK(state.calls == 1);
    size_t length = 0;
    char *policy = sh_package_runtime_policy("requirements", &length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    policy = sh_package_runtime_policy("strings", &length);
    CHECK(policy && strstr(policy, "new") && !strstr(policy, "old")); free(policy);
    policy = sh_package_runtime_policy("hud", &length);
    CHECK(policy && strstr(policy, "engine") && !strstr(policy, "\"weapon\"")); free(policy);
    sh_package_runtime_selectors_release(first); first = NULL;
    CHECK(sh_package_runtime_occurrences_pending());
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, selector_slot_activate, &state));
    CHECK(state.calls == 2); expect_current(health_path, "health = 10");
    /* Modeled positive retirement of the final native palette generation. */
    sh_package_runtime_selectors_release(second); second = NULL;
    CHECK(sh_package_runtime_occurrences_pending()); state.served = "health = 20";
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, selector_slot_activate, &state));
    CHECK(state.calls == 3); expect_current(health_path, "health = 20");
done:
    sh_package_runtime_selectors_release(first); sh_package_runtime_selectors_release(second);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
}
static void test_installed_authoring_admission(void)
{
    const char *descriptor = "{\"id\":\"same\",\"name\":\"Installed authoring\","
        "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":false}}}";
    const char *original_descriptor = "{\"id\":\"same\",\"name\":\"Installed authoring\",\"version\":\"historical\","
        "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":false}}}";
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"cross/root\"}]}";
    char data[MAX_PATH], map_root[MAX_PATH], source_root[MAX_PATH], original_root[MAX_PATH], error[2048];
    char map_path[1024] = "", imported_path[1024] = "", unused_path[1024] = "";
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_sources *sources = NULL, *originals = NULL;
    sh_package_map_plan *map = NULL, *plan = NULL, *stale = NULL;
    sh_package_occurrence_snapshot *map_document = NULL, *document = NULL;
    sh_package_bundle_binding delivery = {0};
    char *map_json = NULL, *bound = NULL; size_t map_length = 0, bound_length = 0;
    installed_authoring_activation state = {0};
    create("authoring-data/overrides/tools/package.json", "{\"id\":\"tools\",\"name\":\"Tools\"}");
    create("authoring-data/overrides/tools/assets/generated/decls/entitydef/cross/localaction.decl",
        "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
    create("authoring-data/overrides/tools/assets/generated/decls/snapeditorentitydef/cross/controls.decl",
        "{ edit = { entityDef = \"cross/root\"; inputs = { num = 1; item[0] = \"cross/localaction\"; } } }");
    create("authoring-map/overrides/map/package.json", "{\"id\":\"same\",\"name\":\"Existing map\","
        "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
    create("authoring-map/overrides/map/assets/generated/decls/entitydef/cross/root.decl",
        "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
    authoring_bundle("authoring-source/overrides/b", descriptor);
    authoring_bundle("authoring-original/overrides/b", original_descriptor);
    snprintf(data, sizeof(data), "%s/authoring-data", root);
    snprintf(map_root, sizeof(map_root), "%s/authoring-map", root);
    snprintf(source_root, sizeof(source_root), "%s/authoring-source", root);
    snprintf(original_root, sizeof(original_root), "%s/authoring-original", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    map = sh_package_runtime_prepare_map(data, map_root, error, sizeof(error)); CHECK(map);
    if (!map) goto done;
    const sh_compiled_resource *resource = historical_resource(sh_package_map_plan_compilation(map),
        "generated/decls/entitydef/cross/root.decl"); CHECK(resource);
    if (!resource) goto done;
    strcpy_s(map_path, sizeof(map_path), resource->engine_path);
    size_t rebound_length = 0;
    char *rebound = sh_package_map_plan_rebind(map, NULL, json, strlen(json), &rebound_length, error, sizeof(error));
    CHECK(rebound);
    if (rebound) {
        map_json = sh_package_runtime_capture_document(map, NULL, NULL, 0, rebound, rebound_length,
            &map_length, &map_document, NULL, NULL, error, sizeof(error));
        HeapFree(GetProcessHeap(), 0, rebound);
    }
    CHECK(map_json && map_document);
    if (!map_document) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(map, (sh_package_activation_guard){0}, NULL, NULL));
    CHECK(sh_package_runtime_select_map_occurrences(map_json, map_length, NULL, map_document, error, sizeof(error)));
    sources = sh_package_sources_scan(source_root, error, sizeof(error));
    originals = sh_package_sources_scan(original_root, error, sizeof(error));
    CHECK(sources && originals && sources->package_count == 1 && originals->package_count == 1);
    if (!sources || !originals) goto done;
    strcpy_s(delivery.source_name, sizeof(delivery.source_name), sources->packages[0].name);
    memcpy(delivery.compiler_tree, sources->fingerprints[0], 32);
    delivery.original = sh_package_bundle_capture(originals, 0, data, error, sizeof(error)); CHECK(delivery.original);
    if (!delivery.original) goto done;
    CHECK(memcmp(delivery.compiler_tree, sh_package_bundle_fingerprint(delivery.original), 32));
    plan = sh_package_runtime_prepare_map_deliveries(data, source_root, NULL, &delivery, 1, error, sizeof(error));
    stale = sh_package_runtime_prepare_map_deliveries(data, source_root, NULL, &delivery, 1, error, sizeof(error));
    CHECK(plan && stale); if (!plan || !stale) goto done;
    resource = historical_resource(sh_package_map_plan_compilation(plan), "generated/decls/entitydef/cross/root.decl");
    CHECK(resource); if (!resource) goto done;
    strcpy_s(imported_path, sizeof(imported_path), resource->engine_path);
    rebound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &rebound_length, error, sizeof(error));
    CHECK(rebound);
    if (rebound) {
        bound = sh_package_runtime_capture_document(plan, NULL, NULL, 0, rebound, rebound_length, &bound_length,
            &document, NULL, NULL, error, sizeof(error));
        HeapFree(GetProcessHeap(), 0, rebound);
    }
    CHECK(bound && document); if (!document) goto done;
    authoring_bundle("authoring-data/overrides/new", descriptor);
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    if (!prepared) fprintf(stderr, "installed authoring preparation: %s\n", error);
    CHECK(prepared); if (!prepared) goto done;
    prepared = sh_package_map_plan_prepare_inventory(stale, data, error, sizeof(error));
    CHECK(prepared); if (!prepared) goto done;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state.map_path = map_path; state.imported_path = imported_path;
    authoring_visible(0); state.fail = 1;
    CHECK(sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
        installed_authoring_activate, installed_authoring_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(state.calls == 1 && state.recoveries == 1 && !state.finalizations && sh_package_runtime_admission_ready());
    state.fail = 0;
    for (int fail = 1; fail <= 2; fail++) {
        state.finalize_fail = fail;
        CHECK(sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
            installed_authoring_activate, installed_authoring_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
        CHECK(state.calls == fail + 1 && state.recoveries == fail + 1 && state.finalizations == fail);
        CHECK(sh_package_runtime_admission_ready()); authoring_visible(0);
    }
    state.finalize_fail = 0;
    int result = sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
        installed_authoring_activate, installed_authoring_finalize, &state, error, sizeof(error));
    if (result != SH_PACKAGE_ADMISSION_READY) fprintf(stderr, "whole authoring admission: %s\n", error);
    CHECK(result == SH_PACKAGE_ADMISSION_READY && state.calls == 4 && state.recoveries == 3 && state.finalizations == 3);
    if (result != SH_PACKAGE_ADMISSION_READY) goto done;
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *unused = historical_resource(library, authoring_unused);
    CHECK(library != state.library && unused);
    if (unused) strcpy_s(unused_path, sizeof(unused_path), unused->engine_path);
    sh_package_runtime_release();
    authoring_visible(1);
    int calls = state.calls;
    CHECK(sh_package_runtime_admit_occurrences(stale, document, (sh_package_activation_guard){0},
        installed_authoring_activate, installed_authoring_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(state.calls == calls && strstr(error, "library changed"));
    size_t delivery_count = 0;
    const sh_package_bundle_binding *portable = sh_package_occurrence_bundles(document, &delivery_count);
    CHECK(delivery_count == 1 && portable && !memcmp(sh_package_bundle_fingerprint(portable[0].original),
        sh_package_bundle_fingerprint(delivery.original), 32));
    sh_package_bundle *installed = NULL;
    const sh_package_compilation *current = sh_package_runtime_acquire();
    for (size_t i = 0; current && i < current->sources->package_count; i++)
        if (!strcmp(current->sources->packages[i].name, "new")) {
            installed = sh_package_runtime_source_bundle(i, current->sources->fingerprints[i]); break;
        }
    sh_package_runtime_release(); CHECK(installed);
    CHECK(installed && !memcmp(sh_package_bundle_fingerprint(installed), delivery.compiler_tree, 32) &&
        memcmp(sh_package_bundle_fingerprint(installed), sh_package_bundle_fingerprint(delivery.original), 32));
    sh_package_bundle_release(installed);
    sh_package_occurrence_release(document); document = NULL;
    sh_package_map_plan_free(plan); plan = NULL; sh_package_map_plan_free(stale); stale = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate,
        &(lease_activation){map_path, 1, 1}));
    authoring_visible(1); expect_current(map_path, "health = 10");
    size_t policy_length = 0; char *policy = sh_package_runtime_active_policy("requirements", &policy_length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    /* The accepted palette still borrows the entire serving generation after
     * every import document/plan is gone. A new empty library cannot retire
     * its unused sibling until positive palette replacement releases the hold. */
    create("authoring-empty/overrides", NULL);
    char empty_data[MAX_PATH]; snprintf(empty_data, sizeof(empty_data), "%s/authoring-empty", root);
    CHECK(state.selector);
    CHECK(sh_package_runtime_refresh_activated(empty_data, (sh_package_activation_guard){0}, lease_activate,
        &(lease_activation){unused_path, 1, 1}));
    authoring_visible(1); expect_current(map_path, "health = 10");
    library = sh_package_runtime_library_acquire();
    CHECK(!historical_resource(library, authoring_unused)); sh_package_runtime_release();
    policy = sh_package_runtime_policy("requirements", &policy_length);
    CHECK(policy && strstr(policy, "g_useResourceBlackList") && !strstr(policy, "g_useImageBlackList")); free(policy);
    sh_package_runtime_selectors_release(state.selector); state.selector = NULL;
    CHECK(sh_package_runtime_occurrences_pending());
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, lease_activate,
        &(lease_activation){unused_path, 1, 0}));
    authoring_visible(0); expect_current(map_path, "health = 10");
done:
    sh_package_runtime_selectors_release(state.selector);
    sh_package_occurrence_release(document); sh_package_occurrence_release(map_document);
    sh_package_map_plan_free(plan); sh_package_map_plan_free(stale); sh_package_map_plan_free(map);
    sh_package_bundle_release(delivery.original); sh_package_sources_free(sources); sh_package_sources_free(originals);
    free(bound); free(map_json);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

/* A package whose own content reaches a dependency SnapMap does not ship stays
 * installed but is not served; its healthy peer still compiles, and neither is
 * reported as a malformed package to skip. */
static void test_unserved_coverage(void)
{
    char data[MAX_PATH], *summary;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/unserved", root);
    create("unserved/overrides/healthy/package.json", "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
    create("unserved/overrides/healthy/assets/generated/decls/entitydef/admission/healthy.decl",
        "{ class = \"Entity\"; edit = { health = 5; } }");
    create("unserved/overrides/gap/package.json", "{\"id\":\"gap\",\"name\":\"Gap\"}");
    create("unserved/overrides/gap/assets/generated/decls/entitydef/admission/gap.decl",
        "{ class = \"Entity\"; edit = { target = \"ai/cyberdemon\"; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    int healthy = 0, gap = 0;
    for (size_t i = 0; library && i < library->resource_count; i++) {
        char error[256]; size_t length = 0;
        unsigned char *body = sh_package_compilation_read(library, library->resources + i, SIZE_MAX,
            &length, error, sizeof(error));
        unsigned char *text = body ? realloc(body, length + 1) : NULL;
        if (!text) { free(body); continue; }
        text[length] = 0;
        healthy += strstr((const char *)text, "health = 5") != NULL;
        gap += strstr((const char *)text, "ai/cyberdemon") != NULL;
        free(text);
    }
    sh_package_runtime_release();
    CHECK(healthy == 1 && !gap);
    summary = sh_package_runtime_summary();
    int reported = summary && strstr(summary, "Installed library: 2 packages") &&
        strstr(summary, "is not served yet (product coverage)") && strstr(summary, "campaign dependency") &&
        strstr(summary, "(via entitydef ai/cyberdemon <- entitydef admission/gap)") &&
        strstr(summary, "need a Snapmap+ update") && !strstr(summary, "Skipped local package");
    if (!reported) fprintf(stderr, "unserved summary: %s\n", summary ? summary : "(none)");
    CHECK(reported); free(summary);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}

int main(int argc, char **argv)
{
    const sh_package_change map_a_changes[] = {
        {"generated/decls/entitydef/ai/cyberdemon.decl", SH_PACKAGE_RESOURCE_REPLACED},
        {"generated/spirv/fixture.vspv", SH_PACKAGE_RESOURCE_REPLACED},
        {"map-only.bimage", SH_PACKAGE_RESOURCE_ADDED}
    };
    const sh_package_change map_b_changes[] = {
        {"generated/decls/entitydef/ai/cyberdemon.decl", SH_PACKAGE_RESOURCE_REPLACED},
        {"generated/spirv/fixture.vspv", SH_PACKAGE_RESOURCE_REPLACED},
        {"map-only.bimage", SH_PACKAGE_RESOURCE_REMOVED}
    };
    char temporary[MAX_PATH], map_a[MAX_PATH], map_b[MAX_PATH], error[2048];
    activation state = {0};
    const sh_package_compilation *library;
    sh_package_references refs = {0};
    size_t length; char *policy;
    FILE *held_map_file = NULL; uint64_t held_length = 0;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pmr", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    if (argc == 2 && !strcmp(argv[1], "--conflicting-startup")) {
        test_conflicting_startup(); cleanup(); return failures ? 1 : 0;
    }
    run_conflicting_startup();
    test_duplicate_editor_controls();
    test_audio_programs();
    test_local_isolation();
    test_unserved_coverage();
    test_map_usage_admission();
    snprintf(map_a, sizeof(map_a), "%s\\map-a", root); snprintf(map_b, sizeof(map_b), "%s\\map-b", root);
    create("overrides/local/package.json", descriptor);
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("overrides/local/assets/generated/spirv/fixture.vspv", "local shader");
    create("map-a/overrides/map/package.json", "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\",\"strings\":{\"en\":{\"label\":\"map A\"}}}");
    create("map-a/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("map-a/overrides/map/assets/generated/spirv/fixture.vspv", "shader A");
    create("map-a/overrides/map/assets/map-only.bimage", "map-only bytes");
    create("map-b/overrides/map/package.json", "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\",\"strings\":{\"en\":{\"label\":\"map B\"}}}");
    create("map-b/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 20; } }");
    create("map-b/overrides/map/assets/generated/spirv/fixture.vspv", "shader B");
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(!sh_package_runtime_activate_map(root, map_a, (sh_package_activation_guard){0}, activate, &state));
    CHECK(!state.calls && !state.recoveries);
    CHECK(sh_package_runtime_refresh(root)); CHECK(!sh_package_runtime_has_map_provider());
    library = expect_library("health = 12000");
    {
        sh_package_missing missing = {0};
        sh_package_map_plan *plan = sh_package_runtime_prepare_map(root, map_b, error, sizeof(error));
        CHECK(plan && sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        /* Different package identity, health and shader bytes: every supplied
         * engine path is already installed, so no installation is needed. */
        CHECK(missing.checked == 2 && !missing.count && !sh_package_owners_count(&missing.packages));
        CHECK(!sh_package_runtime_has_map_provider() && sh_package_runtime_ready());
        expect_current(health_path, "health = 12000"); expect_current("generated/spirv/fixture.vspv", "local shader");
        sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    }
    CHECK(sh_package_references_add(&refs, "", health_path));
    CHECK(sh_package_runtime_select_map("{}", 2, &refs, error, sizeof(error))); sh_package_references_free(&refs);
    state.next_health = "health = 10"; state.previous_health = "health = 12000";
    state.library_health = "health = 12000"; state.map_active = 1;
    state.expected = map_a_changes; state.expected_count = 3;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure;
        CHECK(!switch_map(map_a, &state)); CHECK(state.calls == failure && state.recoveries == failure);
        CHECK(!sh_package_runtime_has_map_provider() && !sh_package_runtime_ready());
        expect_current(health_path, "health = 12000"); CHECK(expect_library("health = 12000") == library);
    }
    state.fail = 0; CHECK(switch_map(map_a, &state)); CHECK(sh_package_runtime_ready());
    expect_current("generated/spirv/fixture.vspv", "shader A"); expect_current("map-only.bimage", "map-only bytes");
    CHECK(sh_package_runtime_open_file("map-only.bimage", &held_map_file, &held_length) == 1);
    CHECK(held_map_file && held_length == strlen("map-only bytes"));
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map A")); free(policy);
    /* A map switch reads only the new map. A broken local source cannot make
     * the runtime discard the already captured local restore provider. */
    create("overrides/local/package.json", "malformed external edit");
    state.next_health = "health = 20"; state.previous_health = "health = 10";
    state.expected = map_b_changes;
    state.fail = 1; CHECK(!switch_map(map_b, &state));
    expect_current("map-only.bimage", "map-only bytes");
    state.fail = 0;
    CHECK(switch_map(map_b, &state)); CHECK(expect_library("health = 12000") == library);
    expect_current("generated/spirv/fixture.vspv", "shader B"); expect_current("map-only.bimage", NULL);
    if (held_map_file) {
        char held_bytes[32] = {0};
        CHECK(fread(held_bytes, 1, sizeof(held_bytes), held_map_file) == strlen("map-only bytes"));
        CHECK(!strcmp(held_bytes, "map-only bytes")); fclose(held_map_file); held_map_file = NULL;
    }
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map B")); free(policy);
    /* Re-entering an identical map still runs activation for policy and native
     * readiness, but supplies no false resource replacements. */
    state.previous_health = "health = 20"; state.expected_count = 0;
    CHECK(switch_map(map_b, &state));
    state.next_health = "health = 12000"; state.previous_health = "health = 20"; state.map_active = 0;
    state.expected_count = 2;
    state.fail = 1; CHECK(!switch_map(NULL, &state)); CHECK(sh_package_runtime_has_map_provider());
    expect_current("generated/spirv/fixture.vspv", "shader B");
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map B")); free(policy);
    state.fail = 0; CHECK(switch_map(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 12000"); expect_current("generated/spirv/fixture.vspv", "local shader");
    CHECK(expect_library("health = 12000") == library);
    {
        sh_json_object empty_policy = {0};
        policy = sh_package_runtime_active_policy("strings", &length);
        CHECK(policy && sh_json_parse_object(policy, length, 16, &empty_policy));
        CHECK(empty_policy.count == 0); sh_json_object_free(&empty_policy); free(policy);
    }
    create("overrides/local/package.json", descriptor);
    state.next_health = "health = 10"; state.previous_health = "health = 12000"; state.map_active = 1;
    state.expected = map_a_changes; state.expected_count = 3;
    CHECK(switch_map(map_a, &state));
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 15000; } }");
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 10"); library = expect_library("health = 15000");
    /* Peer conflicts isolate local owners while the map keeps its authored
     * policy. Fixing the local conflict restores its library before map exit. */
    create("overrides/conflict/package.json", "{\"id\":\"conflict\",\"name\":\"Conflicting edit\"}");
    create("overrides/conflict/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 17000; } }");
    CHECK(sh_package_runtime_refresh(root));
    expect_current(health_path, "health = 10");
    create("overrides/conflict/package.json", "invalid");
    CHECK(sh_package_runtime_refresh(root));
    expect_library("health = 15000"); expect_current(health_path, "health = 10");
    state.next_health = "health = 15000"; state.previous_health = "health = 10";
    state.library_health = "health = 15000"; state.map_active = 0;
    state.expected = map_b_changes;
    CHECK(switch_map(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 15000");
    create("overrides/conflict/package.json", "invalid");
    create("overrides/local/package.json", descriptor);
    test_prepared_map(map_a);
    test_committed_install_joins_library();
    test_variant_ownership();
    test_installed_inventory_is_not_active_composition();
    test_native_material_provider_transaction();
    test_complete_source_snapshots();
    test_original_deliveries_before_compilation();
    test_historical_private_preparation();
    test_constructor_provider_lifetimes();
    test_constructor_binding_changes();
    test_retained_occurrence_resources();
    test_retained_bound_dependencies();
    test_additive_occurrence_admission();
    test_installed_authoring_admission();
    test_initial_installed_authoring();
    test_selector_shared_retirement();
    test_failed_local_compilation_admission();
    cleanup(); if (failures) return 1;
    puts("package runtime map context tests passed"); return 0;
}
