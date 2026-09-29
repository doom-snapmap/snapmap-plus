/* Real runtime provider transactions with independent library and map views.
 * Native activation is modeled here; renderer acceptance is separate. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "resource_graph.h"
/* Every created directory level counts until the final cleanup; overflowing
 * the table silently leaves later fixtures' files uncreated. */
#define PACKAGE_FIXTURE_ENTRIES 2048u
#include "package_fixture.h"
#include "hook.h"
#include "map_provenance.h"
#include "map_embed.h"
#include "package_spawn_registry.h"

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
int hook_unpatch(void *trampoline) { (void)trampoline; CHECK(0); return 0; }

static const char *health_path = "generated/decls/entitydef/ai/cyberdemon.decl";
static const char *descriptor = "{\"id\":\"installed-bosses\",\"name\":\"Installed Bosses\",\"strings\":{\"en\":{\"label\":\"local\"}}}";
static const char *original = "{ edit = { health = 25000; } }";
/* Library compiles that left an unchanged judged package out without
 * compiling it again (test_left_out_packages). */
static unsigned left_out_reuses;
/* Whole installed-library compiles, and refreshes that folded a published
 * installation in as an increment (test_published_install_refresh). */
static unsigned library_compiles, install_refreshes;
/* Source captures that replayed recordings of earlier compiles (the memo). */
static unsigned memo_replays;
/* Packages a library compile left out as remembered by an earlier process. */
static unsigned left_out_packages;
/* "Skipped local package" reports (rejected on their own content). */
static unsigned skipped_packages;
void backend_log(const char *message)
{
    const char *at;
    size_t replayed = 0;
    unsigned left = 0;
    if (!message) return;
    if ((at = strstr(message, "package library: ")) && strstr(message, "remembered by an earlier process") &&
        sscanf(at, "package library: %u unchanged package(s) left out", &left) == 1) left_out_packages += left;
    if (strstr(message, "replayed from earlier compiles") && (at = strstr(message, " ms; ")) &&
        sscanf(at, " ms; %zu replayed", &replayed) == 1 && replayed) memo_replays++;
    if (strstr(message, "left out as before")) left_out_reuses++;
    if (strstr(message, "Skipped local package")) skipped_packages++;
    if (strstr(message, "package library compile (installed library)")) library_compiles++;
    if (strstr(message, "refresh for a published installation: added packages")) install_refreshes++;
}

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

/* A map saved without recorded provenance gets a historical context from its
 * old composition. A shared include it supplies binds the SnapMap programs
 * catalogued beneath it, never a program only the campaign archive lists:
 * such a binding could never be captured and would refuse the whole map. */
typedef struct pindex_bytes { unsigned char data[512]; size_t length; } pindex_bytes;
static void pindex_raw(pindex_bytes *b, const void *data, size_t n)
{ memcpy(b->data + b->length, data, n); b->length += n; }
static void pindex_be(pindex_bytes *b, uint32_t n)
{ unsigned char p[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n}; pindex_raw(b, p, 4); }
static void pindex_str(pindex_bytes *b, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);
    unsigned char p[4] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)};
    pindex_raw(b, p, 4); pindex_raw(b, text, n);
}
static void pindex_write(const char *relative, const char *program, const char *placeholder)
{
    pindex_bytes b = {{0}, 0}; unsigned char head[0x20] = {5, 'S', 'E', 'R'}, tail[21] = {0};
    char path[MAX_PATH + 64]; FILE *file;
    pindex_raw(&b, head, sizeof(head)); pindex_be(&b, placeholder ? 2 : 1);
    pindex_be(&b, 0); pindex_str(&b, "renderProg"); pindex_str(&b, program);
    pindex_str(&b, "decls/renderprogs/common.inc"); pindex_raw(&b, tail, sizeof(tail));
    /* A zero-byte placeholder row has no filename, as campaign collision
     * models for md6 names do. */
    if (placeholder) {
        pindex_be(&b, 1); pindex_str(&b, "cm"); pindex_str(&b, placeholder); pindex_str(&b, "");
        pindex_raw(&b, tail, sizeof(tail));
    }
    uint32_t size = (uint32_t)b.length - 0x20;
    b.data[4] = (unsigned char)(size >> 24); b.data[5] = (unsigned char)(size >> 16);
    b.data[6] = (unsigned char)(size >> 8); b.data[7] = (unsigned char)size;
    create(relative, ""); /* Tracked for cleanup; the binary index replaces it. */
    snprintf(path, sizeof(path), "%s\\%s", root, relative);
    file = fopen(path, "wb"); CHECK(file);
    if (file) { CHECK(fwrite(b.data, 1, b.length, file) == b.length); fclose(file); }
}
static void test_legacy_include_bindings(void)
{
    char base[MAX_PATH], path[MAX_PATH], error[1024];
    create("legacy-catalog/snap_gameresources.resources", ""); create("legacy-catalog/snap_gameresources.patch", "");
    create("legacy-catalog/gameresources.resources", ""); create("legacy-catalog/gameresources.patch", "");
    pindex_write("legacy-catalog/snap_gameresources.pindex", "fixture", NULL);
    pindex_write("legacy-catalog/gameresources.pindex", "campaignonly", "zion/boss.md6");
    snprintf(base, sizeof(base), "%s\\legacy-catalog", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    create("legacy-include/overrides/shader/package.json", "{\"id\":\"legacy-shader\",\"name\":\"Legacy shader\"}");
    create("legacy-include/overrides/shader/assets/decls/renderprogs/common.inc", "// shared include\n");
    /* Legacy import supplies that placeholder under its resource name. */
    create("legacy-include/overrides/shader/assets/zion/boss.md6", "");
    snprintf(path, sizeof(path), "%s\\legacy-include", root);
    sh_package_sources *sources = sh_package_sources_scan(path, error, sizeof(error));
    sh_package_compilation *compiled = sources ? sh_package_compile(sources, NULL, NULL, error, sizeof(error)) : NULL;
    sh_package_provenance historical = {0};
    CHECK(catalog && sources && compiled);
    CHECK(compiled && catalog && sh_package_runtime_test_legacy_bindings(compiled, catalog, &historical));
    size_t snapmap = 0, campaign = 0, placeholder = 0;
    for (size_t i = 0; i < historical.binding_count; i++) {
        const sh_package_provenance_binding *binding = historical.bindings + i;
        placeholder += !strcmp(binding->family, "cm") && !strcmp(binding->original, "zion/boss.md6");
        if (strcmp(binding->family, "renderprog")) continue;
        snapmap += !strcmp(binding->original, "fixture");
        campaign += !strcmp(binding->original, "campaignonly");
    }
    if (snapmap != 1 || campaign) fprintf(stderr, "legacy include bindings: %zu SnapMap, %zu campaign\n", snapmap, campaign);
    CHECK(snapmap == 1 && !campaign && placeholder == 1);
    sh_package_provenance_free(&historical); sh_package_compilation_free(compiled);
    sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
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
        /* The spawn-key detours read the restored generation's groups. */
        CHECK(sh_package_runtime_test_key_groups_current() && !sh_package_runtime_has_map_provider());
        CHECK(sh_package_map_plan_compilation(plan) == candidate);
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "local");
    }
    state.fail = 0; CHECK(activate_plan(plan, &state)); CHECK(sh_package_runtime_ready());
    CHECK(sh_package_runtime_test_key_groups_current() && sh_package_runtime_has_map_provider());
    {
        /* Editor inspectors read the serving (map) generation's key codes, a
         * code only the library defines there, and extend the map's table. */
        sh_package_key_groups library = {0}, map = {0};
        sh_package_key_set pair = {0}, trio = {0}, fresh = {0}, single = {0}, keys;
        uint32_t a = sh_package_key_private(40), b = sh_package_key_private(41), c = sh_package_key_private(42);
        uint32_t library_group = sh_package_key_private(12), map_group = sh_package_key_private(13);
        uint32_t code = 0, again = 0, historical = 0;
        sh_package_key_set_add(&pair, a); sh_package_key_set_add(&pair, b);
        trio = pair; sh_package_key_set_add(&trio, c);
        sh_package_key_set_add(&fresh, a); sh_package_key_set_add(&fresh, c);
        sh_package_key_set_add(&single, a);
        CHECK(sh_package_key_groups_define(&library, library_group, &pair));
        CHECK(sh_package_key_groups_define(&map, map_group, &trio));
        map.historical = 1u << 27;
        sh_package_runtime_test_key_tables(&library, &map);
        CHECK(sh_package_runtime_test_key_groups_current());
        sh_package_runtime_key_decode(library_group | 4u, &keys, &historical);
        CHECK(sh_package_key_set_equal(&keys, &pair) && historical == 1u << 27);
        sh_package_runtime_key_decode(map_group, &keys, NULL);
        CHECK(sh_package_key_set_equal(&keys, &trio));
        CHECK(sh_package_runtime_key_code(&pair, &code) == 1 && code == library_group);
        CHECK(sh_package_runtime_key_code(&single, &code) == 1 && code == a);
        CHECK(sh_package_runtime_key_code(&fresh, &code) == 1 && code == sh_package_key_private(9));
        sh_package_runtime_key_decode(code, &keys, NULL);
        CHECK(sh_package_key_set_equal(&keys, &fresh) && sh_package_runtime_test_key_groups_current());
        CHECK(sh_package_runtime_key_code(&fresh, &again) == 1 && again == code);
        CHECK(sh_package_runtime_key_code(NULL, &again) == -1 && sh_package_runtime_key_code(&fresh, NULL) == -1);
    }
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
    CHECK(sh_package_runtime_test_key_groups_current());
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "prepared");
    state.fail = 0; CHECK(activate_plan(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    CHECK(sh_package_runtime_test_key_groups_current());
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
    /* The committed library replaced the installed one under the map. */
    CHECK(sh_package_runtime_test_key_groups_current());
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
/* The MPKG commit: committed files become permanent library sources, which
 * schedules a rescan unless the plan already carries their library
 * (sh_mpkg_activation_commit). */
static int commit_published(char *error, size_t capacity)
{
    if (!commit_inventory(error, capacity)) return 0;
    sh_package_runtime_note_sources_changed(); return 1;
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
    /* The committed library's group codes are the ones the detours read. */
    CHECK(sh_package_runtime_test_key_groups_current());
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
            "{ class = \"Entity\"; edit = { health = 1; } }" :
        !strcmp(path, "generated/decls/entitydef/history/holder.decl") ?
            "{ class = \"Entity\"; edit = { health = 3; target = \"history/child\"; } }" : NULL;
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
/* Discovering another stock consumer of unchanged shared bytes does not
 * change what that consumer reads. Real edits and private identities still do. */
static void test_native_membership_changes(void)
{
    char base[MAX_PATH], error[1024];
    create("membership-catalog/snap_gameresources.resources", "");
    create("membership-catalog/snap_gameresources.patch", "");
    create("membership-catalog/gameresources.resources", "");
    create("membership-catalog/gameresources.patch", "");
    pindex_write("membership-catalog/snap_gameresources.pindex", "fixture", NULL);
    pindex_write("membership-catalog/gameresources.pindex", "campaignonly", NULL);
    snprintf(base, sizeof(base), "%s/membership-catalog", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    CHECK(catalog); if (!catalog) return;
    sh_compiled_resource a[2] = {0}, b[3] = {0};
    a[0].engine_path = "decls/renderprogs/common.inc";
    a[0].body = (unsigned char *)"same"; a[0].body_length = 4;
    a[0].native_type = "renderprog"; a[0].native_name = "campaignonly";
    b[0] = a[0];
    sh_package_native_identity alias = {"renderprog", "fixture"};
    b[0].native_aliases = &alias; b[0].native_alias_count = 1;
    b[1].engine_path = "private/added.bin";
    b[1].body = (unsigned char *)"new"; b[1].body_length = 3;
    b[1].native_type = "model"; b[1].native_name = "private/added";
    sh_package_compilation before = {0}, after = {0};
    before.resources = a; before.resource_count = 1;
    after.resources = b; after.resource_count = 2;
    sh_package_changes changes = {0};
#define MEMBERSHIP(old, next, cat, ca, cb) \
    CHECK(sh_package_runtime_test_native_changes(old, next, cat, ca, cb, &changes, error, sizeof(error)))
    MEMBERSHIP(&before, &after, catalog, NULL, NULL);
    CHECK(changes.count == 1 && changes.added_count == 1 && !changes.removed_count && !changes.rebound_count);
    CHECK(changes.added_count && !strcmp(changes.added[0].name, "private/added"));
    sh_package_changes_reverse(&changes);
    CHECK(changes.count == 1 && !changes.added_count && changes.removed_count == 1);
    sh_package_changes_free(&changes);
    MEMBERSHIP(&after, &before, catalog, NULL, NULL);
    CHECK(changes.count == 1 && !changes.added_count && changes.removed_count == 1);
    MEMBERSHIP(&before, &after, NULL, NULL, NULL);
    CHECK(changes.added_count == 2); /* Unknown native availability is not a proof. */
    b[0].body = (unsigned char *)"edit";
    MEMBERSHIP(&before, &after, catalog, NULL, NULL);
    CHECK(changes.count == 2 && changes.added_count == 2);
    CHECK(!strcmp(changes.items[0].path, a[0].engine_path) && changes.items[0].kind == SH_PACKAGE_RESOURCE_REPLACED);
    b[0].body = a[0].body;
    /* One unverified carrier prevents a partial proof from suppressing work. */
    a[1] = b[2] = a[0]; a[1].engine_path = b[2].engine_path = "unknown/shared.bin";
    b[2].native_aliases = &alias; b[2].native_alias_count = 1;
    before.resource_count = 2; after.resource_count = 3;
    MEMBERSHIP(&before, &after, catalog, NULL, NULL);
    CHECK(changes.count == 1 && changes.added_count == 2);
    before.resource_count = 1; after.resource_count = 2;
    /* Campaign-only names never acquire stock availability through an include. */
    a[0].native_name = b[0].native_name = "fixture"; alias.name = "campaignonly";
    MEMBERSHIP(&before, &after, catalog, NULL, NULL);
    CHECK(changes.added_count == 2);
    a[0].native_name = b[0].native_name = "campaignonly"; alias.name = "fixture";
    /* A changed native constructor remains a change, even when every physical
     * file is identical and the stock identity merely gained metadata. */
    sh_package_consumer_target old_target = {"input", "image", "old"}, next_target = {"input", "image", "next"};
    sh_package_consumer old_row = {"renderprog", "fixture", &old_target, 1};
    sh_package_consumer next_row = {"renderprog", "fixture", &next_target, 1};
    sh_package_consumers *old = sh_package_consumers_create(&old_row, 1, error, sizeof(error));
    sh_package_consumers *next = sh_package_consumers_create(&next_row, 1, error, sizeof(error));
    CHECK(old && next);
    MEMBERSHIP(&before, &after, catalog, old, next);
    CHECK(changes.added_count == 1 && changes.rebound_count == 1);
    if (changes.rebound_count) CHECK(!strcmp(changes.rebound[0].name, "fixture"));
    sh_package_changes_reverse(&changes); CHECK(changes.rebound_count == 1);
    sh_package_changes_free(&changes);
    MEMBERSHIP(&after, &before, catalog, next, old);
    CHECK(changes.removed_count == 1 && changes.rebound_count == 1);
    sh_package_consumers_free(old); sh_package_consumers_free(next);
    sh_package_changes_free(&changes); sh_resource_catalog_close(catalog);
#undef MEMBERSHIP
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
    /* An unchanged SnapMap original stays native: it is not published again,
     * so it claims no name, path or gameplay ownership of its own. */
    CHECK(!historical_resource(sh_package_map_plan_compilation(equal), historical_root));
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

/* A legacy map's package may change only a dependency: history/child here,
 * as a texture, material or bank changes the stock resources that reach it.
 * Under the file replacement the map was authored with, the stock
 * history/holder its entity names meant the changed child. Its own typed
 * values name that derived identity; an unaffected stock entity and the local
 * library's different child are not used, and a vanilla map gains nothing. */
static int derived_resource_health(const sh_package_compilation *compiled, const char *native, const char *health)
{
    if (compiled) for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->native_name && !strcmp(r->native_name, native) && r->body && strstr((const char *)r->body, health)) return 1;
    }
    return 0;
}
static void test_historical_derived_identities(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"entities\":["
        "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/holder\"},"
        "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/stockbase\"}]}";
    char data[MAX_PATH], map[MAX_PATH], vanilla[MAX_PATH], error[2048];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *before = NULL, *plan = NULL, *stock = NULL;
    sh_package_sources *sources = NULL;
    sh_package_bundle_binding row = {0};
    sh_package_provenance selected = {0};
    char *old = NULL, *bound = NULL, *plain = NULL; size_t old_length = 0, length = 0, plain_length = 0;
    create("derived-data/overrides/local/package.json", "{\"id\":\"local-child\",\"name\":\"Local child\"}");
    create("derived-data/overrides/local/assets/generated/decls/entitydef/history/child.decl",
        "{ class = \"Entity\"; edit = { health = 50; } }");
    create("derived-map/overrides/pkg/package.json", "{\"id\":\"legacy-child\",\"name\":\"Legacy child\"}");
    create("derived-map/overrides/pkg/assets/generated/decls/entitydef/history/child.decl",
        "{ class = \"Entity\"; edit = { health = 9; } }");
    create("derived-vanilla/overrides", NULL);
    snprintf(data, sizeof(data), "%s/derived-data", root);
    snprintf(map, sizeof(map), "%s/derived-map", root);
    snprintf(vanilla, sizeof(vanilla), "%s/derived-vanilla", root);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sources = sh_package_sources_scan(map, error, sizeof(error));
    CHECK(sources && sources->package_count == 1); if (!sources || sources->package_count != 1) goto done;
    strcpy_s(row.source_name, sizeof(row.source_name), sources->packages[0].name);
    memcpy(row.compiler_tree, sources->fingerprints[0], 32);
    row.original = sh_package_bundle_capture(sources, 0, data, error, sizeof(error)); CHECK(row.original);
    if (!row.original) goto done;
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    /* Authored files alone leave the derived holder stock (the old recipe). */
    before = sh_package_runtime_prepare_map_deliveries(data, map, NULL, &row, 1, error, sizeof(error));
    if (!before) fprintf(stderr, "derived before: %s\n", error);
    CHECK(before);
    if (before) {
        old = sh_package_map_plan_rebind(before, NULL, json, strlen(json), &old_length, error, sizeof(error));
        CHECK(old && strstr(old, "\"inherit\":\"history/holder\""));
    }
    plan = sh_package_runtime_prepare_map_document(data, map, NULL, &row, 1, json, strlen(json), error, sizeof(error));
    if (!plan) fprintf(stderr, "derived document: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    bound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &length, error, sizeof(error));
    if (!bound) fprintf(stderr, "derived rebind: %s\n", error);
    CHECK(bound && !strstr(bound, "\"inherit\":\"history/holder\"") && strstr(bound, "\"inherit\":\"history/stockbase\""));
    if (bound) {
        int present = 0;
        CHECK(sh_map_provenance_read(bound, length, &selected, &present, error, sizeof(error)) && present);
        /* One actual site selected the derived holder in the map's own context. */
        CHECK(selected.use_count == 1 && selected.binding_count == 1 &&
            !strcmp(selected.bindings[0].original, "history/holder") &&
            !strncmp(selected.bindings[0].name, "smp/", 4) &&
            selected.context_count == 1 && selected.contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED);
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(plan);
        /* The holder variant reaches the map's child, never the local library's. */
        const sh_compiled_resource *child = historical_resource(compiled, "generated/decls/entitydef/history/child.decl");
        CHECK(child && child->native_name && strstr((const char *)child->body, "health = 9"));
        if (child && child->native_name && selected.binding_count)
            CHECK(derived_resource_health(compiled, selected.bindings[0].name, child->native_name));
    }
    stock = sh_package_runtime_prepare_map_document(data, vanilla, NULL, NULL, 0, json, strlen(json), error, sizeof(error));
    CHECK(stock);
    if (stock) {
        plain = sh_package_map_plan_rebind(stock, NULL, json, strlen(json), &plain_length, error, sizeof(error));
        CHECK(plain && plain_length == strlen(json) && !memcmp(plain, json, plain_length));
    }
done:
    if (plain) HeapFree(GetProcessHeap(), 0, plain);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    if (old) HeapFree(GetProcessHeap(), 0, old);
    sh_package_provenance_free(&selected);
    sh_package_map_plan_free(stock); sh_package_map_plan_free(plan); sh_package_map_plan_free(before);
    sh_package_bundle_release(row.original); sh_package_sources_free(sources);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* The recipe keeps only changed derived identities the document names, in the
 * map's own context, and never replaces an authored row. */
static void test_historical_derived_recipe(void)
{
    sh_package_provenance historical = {0}, linked = {0};
    sh_package_references roots = {0};
    unsigned char own[1][32] = {{1}}, other[1][32] = {{2}};
    size_t h = sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED, own, 1, NULL);
    size_t mine = sh_package_provenance_context_add(&linked, SH_PACKAGE_CONTEXT_COMPOSED, own, 1, NULL);
    size_t theirs = sh_package_provenance_context_add(&linked, SH_PACKAGE_CONTEXT_COMPOSED, other, 1, NULL);
    historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    CHECK(h != SIZE_MAX && mine != SIZE_MAX && theirs != SIZE_MAX);
    CHECK(sh_package_provenance_bind(&historical, "material", "authored/mat", "authored/mat", &h, 1));
    CHECK(sh_package_provenance_bind(&linked, "material", "authored/mat", "smp/mat", &mine, 1));
    CHECK(sh_package_provenance_bind(&linked, "sound", "play_changed", "smp/sound", &mine, 1));
    CHECK(sh_package_provenance_bind(&linked, "entitydef", "props/reaches", "smp/entity", &mine, 1));
    CHECK(sh_package_provenance_bind(&linked, "entitydef", "props/unchanged", "props/unchanged", &mine, 1));
    CHECK(sh_package_provenance_bind(&linked, "entitydef", "props/elsewhere", "smp/elsewhere", &theirs, 1));
    CHECK(sh_package_provenance_bind(&linked, "sound", "play_unused", "smp/unused", &mine, 1));
    const char *named[][2] = {{"material", "authored/mat"}, {"sound", "play_changed"}, {"entitydef", "props/reaches"},
        {"entitydef", "props/unchanged"}, {"entitydef", "props/elsewhere"}};
    for (size_t i = 0; i < sizeof(named) / sizeof(*named); i++) CHECK(sh_package_references_add(&roots, named[i][0], named[i][1]));
    CHECK(sh_package_runtime_test_historical_derived(&historical, &linked, &roots));
    int material = 0, sound = 0, reaches = 0, unchanged = 0, elsewhere = 0, unused = 0;
    for (size_t i = 0; i < historical.binding_count; i++) {
        const sh_package_provenance_binding *b = historical.bindings + i;
        CHECK(!strcmp(b->name, b->original) && b->context_count == 1 && b->contexts[0] == h);
        material += !strcmp(b->original, "authored/mat"); sound += !strcmp(b->original, "play_changed");
        reaches += !strcmp(b->original, "props/reaches"); unchanged += !strcmp(b->original, "props/unchanged");
        elsewhere += !strcmp(b->original, "props/elsewhere"); unused += !strcmp(b->original, "play_unused");
    }
    CHECK(material == 1 && sound == 1 && reaches == 1 && !unchanged && !elsewhere && !unused);
    sh_package_references_free(&roots); sh_package_provenance_free(&historical); sh_package_provenance_free(&linked);
}
/* A legacy package may author an exact copy of a stock resource beside its
 * real edits (as the lever audio package authors play_console_uac_beeps while
 * its bank edit never runs). Once exact dedupe resolves that row to the native
 * identity itself, a site using it is vanilla: SnapMap serves the identity, the
 * provider publishes nothing for it, and neither the load nor a later native
 * capture (Play) may record, lease or verify it as a package use. */
static int native_identity_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{ (void)context; (void)restoring; (void)changes; (void)error; (void)capacity; return 1; }
/* R137 save/load path. A map use of a package entity definition is saved as
 * native inline state when exact; another use whose value SnapMap's carrier
 * cannot prove (a decimal for an int field) keeps its package, which then stays
 * delivered whole (mixed map). With only convertible uses the map needs no
 * package. Unused bundle members are no requirement: a stock-named edit is
 * compiled under a private identity, so stock consumers never reach it and only
 * bound map uses do. A shared setting keeps an otherwise unused delivery. The
 * native base must be a SnapMap palette definition (a shipped editor entry
 * resolves to it); package editor entries are covered by
 * package_editor_rows_test. */
#define INLINE_ENTITY(name) "{\"entityDef\":{\"className\":\"Entity\",\"inherit\":\"" name "\",\"name\":\"\"," \
    "\"state\":{\"edit\":{}},\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26}}"
#define INLINE_VARIABLES "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"entities\":["
static const char *const inline_editor_rows[] = {"inlinestock/row_base", "inlinestock/base_row",
    "inlinestock/child_row", "inlinestock/holder_row"};
static int inline_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/inlinestock/base.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; target = \"\"; } }" :
        !strcmp(path, "generated/decls/entitydef/inlinestock/holder.decl") ?
            "{ class = \"Entity\"; edit = { health = 3; target = \"inlinestock/child\"; } }" :
        !strcmp(path, "generated/decls/entitydef/inlinestock/child.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; } }" :
        !strcmp(path, "generated/decls/entitydef/inlinestock/hidden.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; } }" :
        /* Shipped editor entries: an abstract parent, and rows placing three of
         * the stock definitions; inlinestock/hidden has none. */
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/row_base.decl") ?
            "{ edit = { previewImage = \"icons/stock\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/base_row.decl") ?
            "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/child_row.decl") ?
            "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/child\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/holder_row.decl") ?
            "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/holder\"; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void inline_graph(const char *name, const char *child)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, "entitydef", name);
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin_state(&frame, "entitydef", name, 1);
    if (child) sh_resource_graph_reference("entitydef", child);
    sh_resource_graph_end(&frame, 1);
}
static int inline_save(const char *data, const char *map, const char *json, char **derived, size_t *count,
    sh_package_occurrence_snapshot **ownership, char **report)
{
    char error[2048]; size_t bound_length = 0, derived_length = 0;
    sh_mpkg_used *used = NULL; int result = -2;
    sh_package_map_plan *plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    if (!plan) { fprintf(stderr, "inline prepare: %s\n", error); return -2; }
    char *bound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &bound_length, error, sizeof(error));
    if (!bound) fprintf(stderr, "inline rebind: %s\n", error);
    if (bound) {
        result = sh_package_runtime_native_document(plan, NULL, NULL, 0, bound, bound_length, derived,
            &derived_length, ownership, &used, count, report, error, sizeof(error));
        if (result < 0) fprintf(stderr, "inline save: %s\n", error);
        HeapFree(GetProcessHeap(), 0, bound);
    }
    sh_mpkg_used_free(used); sh_package_map_plan_free(plan);
    return result;
}
static void inline_reset(char **derived, char **report, sh_package_occurrence_snapshot **ownership, size_t *count)
{
    free(*derived); *derived = NULL; free(*report); *report = NULL;
    sh_package_occurrence_release(*ownership); *ownership = NULL; *count = 99;
}
static void test_native_inline_save(void)
{
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], stock[MAX_PATH], keep[MAX_PATH], setting[MAX_PATH], rows[MAX_PATH], copy[MAX_PATH];
    char *derived = NULL, *report = NULL;
    size_t count = 99; sh_package_occurrence_snapshot *ownership = NULL;
    const sh_package_provenance *saved;
    create("inline-data/overrides", NULL);
    create("inline-map/overrides/tough/package.json", "{\"id\":\"tough\",\"name\":\"Tough\"}");
    create("inline-map/overrides/tough/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("inline-map/overrides/tough/assets/generated/decls/entitydef/inline/fraction.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 2.5; } }");
    create("inline-stock/overrides/withstock/package.json", "{\"id\":\"withstock\",\"name\":\"With stock\"}");
    create("inline-stock/overrides/withstock/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("inline-stock/overrides/withstock/assets/generated/decls/entitydef/inlinestock/child.decl",
        "{ class = \"Entity\"; edit = { health = 9; } }");
    create("inline-copy/overrides/copystock/package.json", "{\"id\":\"copystock\",\"name\":\"Copy stock\"}");
    create("inline-copy/overrides/copystock/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("inline-copy/overrides/copystock/assets/generated/decls/entitydef/inlinestock/base.decl",
        "{ class = \"Entity\"; editorVars { placeable = true; } edit = { health = 1; target = \"\"; } }");
    create("inline-rows/overrides/rows/package.json", "{\"id\":\"rows\",\"name\":\"Rows\"}");
    create("inline-rows/overrides/rows/assets/generated/decls/entitydef/inline/hidden.decl",
        "{ inherit = \"inlinestock/hidden\"; edit = { health = 40; } }");
    create("inline-keep/overrides/keepstock/package.json", "{\"id\":\"keepstock\",\"name\":\"Keep stock\"}");
    create("inline-keep/overrides/keepstock/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("inline-keep/overrides/keepstock/assets/generated/decls/entitydef/inlinestock/child.decl",
        "{ class = \"Entity\"; edit = { health = 9.5; } }");
    create("inline-setting/overrides/withsetting/package.json", "{\"id\":\"withsetting\",\"name\":\"With setting\","
        "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
    create("inline-setting/overrides/withsetting/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    snprintf(data, sizeof(data), "%s/inline-data", root);
    snprintf(map, sizeof(map), "%s/inline-map", root);
    snprintf(stock, sizeof(stock), "%s/inline-stock", root);
    snprintf(keep, sizeof(keep), "%s/inline-keep", root);
    snprintf(setting, sizeof(setting), "%s/inline-setting", root);
    snprintf(rows, sizeof(rows), "%s/inline-rows", root);
    snprintf(copy, sizeof(copy), "%s/inline-copy", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inline_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_editor_rows(inline_editor_rows, sizeof(inline_editor_rows) / sizeof(*inline_editor_rows));

    const char *mixed = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "," INLINE_ENTITY("inline/fraction") "]}";
    int result = inline_save(data, map, mixed, &derived, &count, &ownership, &report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") &&
        strstr(derived, "\"state\":{\"edit\":{\"health\":40}}") && count == 1);
    CHECK(report && strstr(report, "-> native inlinestock/base") && strstr(report, "kept (product gap)"));
    saved = ownership ? sh_package_occurrence_provenance(ownership) : NULL;
    CHECK(saved && saved->use_count == 1 && saved->uses[0].owner && !strcmp(saved->uses[0].owner, "entity/1"));
    inline_reset(&derived, &report, &ownership, &count);

    const char *native = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "]}";
    result = inline_save(data, map, native, &derived, &count, &ownership, &report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") && count == 0);
    saved = ownership ? sh_package_occurrence_provenance(ownership) : NULL;
    CHECK(saved && !saved->use_count);
    inline_reset(&derived, &report, &ownership, &count);

    /* The same delivery also edits a stock definition nothing in the map uses.
     * That edit is private to bound uses, so it is demonstrably unreachable
     * and no requirement, even before any dependency coverage is recorded. */
    result = inline_save(data, stock, native, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") && count == 0);
    CHECK(report && !strstr(report, "kept"));
    inline_reset(&derived, &report, &ownership, &count);

    /* A stock entity whose dependencies name that stock definition reaches the
     * stock original with or without the package: still fully native. */
    inline_graph("inlinestock/base", NULL);
    inline_graph("inlinestock/child", NULL);
    inline_graph("inlinestock/holder", "inlinestock/child");
    const char *holder = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "," INLINE_ENTITY("inlinestock/holder") "]}";
    result = inline_save(data, stock, holder, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/holder\"") && count == 0);
    inline_reset(&derived, &report, &ownership, &count);

    /* A map use of the package's replacement of that stock definition becomes
     * the stock identity plus one exact inline layer. */
    const char *replaced = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "," INLINE_ENTITY("inlinestock/child") "]}";
    result = inline_save(data, stock, replaced, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    CHECK(result == 1 && derived && count == 0 &&
        strstr(derived, "\"inherit\":\"inlinestock/child\",\"name\":\"\",\"state\":{\"edit\":{\"health\":9}}"));
    inline_reset(&derived, &report, &ownership, &count);

    /* When another actual map use needs that edit and cannot convert, the
     * delivery stays whole for it while the independent use still converts. */
    result = inline_save(data, keep, replaced, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") && count == 1);
    CHECK(report && strstr(report, "-> native inlinestock/base") && strstr(report, "kept (product gap)"));
    saved = ownership ? sh_package_occurrence_provenance(ownership) : NULL;
    CHECK(saved && saved->use_count == 1 && saved->uses[0].owner && !strcmp(saved->uses[0].owner, "entity/1"));
    inline_reset(&derived, &report, &ownership, &count);

    /* A shared setting keeps an otherwise unused delivery; whether the native
     * map needs it is not modelled, so that is a product gap. */
    result = inline_save(data, setting, native, &derived, &count, &ownership, &report);
    if (report && result != 0) fprintf(stderr, "%s", report);
    CHECK(result == 0 && !derived && count == 99 && !ownership);
    CHECK(report && strstr(report, "kept (product gap): its package lifts SnapMap's resource blacklist"));
    inline_reset(&derived, &report, &ownership, &count);

    /* A base with no shipped editor entry would fail SnapMap's map validation:
     * a native limit, and the use keeps its package. */
    const char *hidden = INLINE_VARIABLES INLINE_ENTITY("inline/hidden") "]}";
    result = inline_save(data, rows, hidden, &derived, &count, &ownership, &report);
    if (report && result != 0) fprintf(stderr, "%s", report);
    CHECK(result == 0 && !derived && report && strstr(report, "kept (native limit): 'inlinestock/hidden' has no SnapMap editor entry"));
    inline_reset(&derived, &report, &ownership, &count);
    /* A package copy of the stock base that adds only editor metadata leaves
     * its content the native original's: the use still becomes native data. */
    result = inline_save(data, copy, native, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    CHECK(result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") && count == 0);
    inline_reset(&derived, &report, &ownership, &count);
    /* Without the shipped roster, placement is unproven: nothing converts. */
    sh_package_runtime_test_editor_rows(NULL, 0);
    result = inline_save(data, map, native, &derived, &count, &ownership, &report);
    CHECK(result == 0 && !derived && report && strstr(report, "kept (product gap): SnapMap's shipped editor entries could not be read"));
    inline_reset(&derived, &report, &ownership, &count);

    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(map); cleanup_resources(stock);
    cleanup_resources(keep); cleanup_resources(setting); cleanup_resources(rows); cleanup_resources(copy);
}

/* R137 load path: the same conversion runs on the map-owned prepared document
 * before availability decides on an installation. The local library lacks the
 * map's package. A fully native equivalent then needs nothing installed; a
 * mixed map still selects its complete package. Both a historical document
 * (no recorded provenance) and a modern saved one take this route, and the
 * map's retained delivery stays byte-identical. */
static int inline_load_missing(sh_package_map_plan *plan, sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_missing *missing)
{
    sh_package_source_graph_report report = {0};
    char error[1024];
    sh_package_missing_free(missing); memset(missing, 0, sizeof(*missing));
    int ok = sh_package_map_plan_usage_missing(plan, registry, reflection, json, length, missing,
        &report, error, sizeof(error));
    if (!ok) fprintf(stderr, "inline load availability: %s\n", error);
    return ok;
}
static void test_native_inline_load(void)
{
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], error[2048];
    unsigned char before[32] = {0};
    sh_package_sources *sources = NULL;
    sh_package_bundle_binding row = {0};
    sh_package_missing missing = {0};
    create("inline-load-data/overrides", NULL);
    create("inline-load-map/overrides/tough/package.json", "{\"id\":\"tough-load\",\"name\":\"Tough load\"}");
    create("inline-load-map/overrides/tough/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("inline-load-map/overrides/tough/assets/generated/decls/entitydef/inline/fraction.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 2.5; } }");
    snprintf(data, sizeof(data), "%s/inline-load-data", root);
    snprintf(map, sizeof(map), "%s/inline-load-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inline_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sources = sh_package_sources_scan(map, error, sizeof(error));
    CHECK(sources && sources->package_count == 1); if (!sources || sources->package_count != 1) goto done;
    strcpy_s(row.source_name, sizeof(row.source_name), sources->packages[0].name);
    memcpy(row.compiler_tree, sources->fingerprints[0], 32);
    row.original = sh_package_bundle_capture(sources, 0, data, error, sizeof(error)); CHECK(row.original);
    if (!row.original || !sh_package_bundle_fingerprint(row.original)) goto done;
    memcpy(before, sh_package_bundle_fingerprint(row.original), 32);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_editor_rows(inline_editor_rows, sizeof(inline_editor_rows) / sizeof(*inline_editor_rows));
    for (int modern = 0; modern < 2; modern++) for (int mixed = 0; mixed < 2; mixed++) {
        const char *json = mixed ?
            INLINE_VARIABLES INLINE_ENTITY("inline/tough") "," INLINE_ENTITY("inline/fraction") "]}" :
            INLINE_VARIABLES INLINE_ENTITY("inline/tough") "]}";
        sh_package_provenance saved = {0};
        sh_package_map_plan *plan = NULL;
        char *bound = NULL, *document = NULL, *derived = NULL, *report = NULL;
        size_t bound_length = 0, document_length = strlen(json), derived_length = 0, count = 99;
        sh_package_occurrence_snapshot *ownership = NULL; sh_mpkg_used *used = NULL;
        int present = 0;
        if (modern) {
            /* A modern saved map carries its bound names and recorded provenance. */
            sh_package_map_plan *first = sh_package_runtime_prepare_map_document(data, map, NULL, &row, 1,
                json, strlen(json), error, sizeof(error));
            char *saved_map = first ? sh_package_map_plan_rebind(first, NULL, json, strlen(json),
                &document_length, error, sizeof(error)) : NULL;
            sh_package_map_plan_free(first);
            CHECK(saved_map && sh_map_provenance_read(saved_map, document_length, &saved, &present, error, sizeof(error)) &&
                present && saved.use_count == (mixed ? 2u : 1u));
            if (!saved_map || !present) {
                if (saved_map) HeapFree(GetProcessHeap(), 0, saved_map);
                sh_package_provenance_free(&saved); continue;
            }
            document = malloc(document_length + 1);
            if (document) { memcpy(document, saved_map, document_length); document[document_length] = 0; }
            HeapFree(GetProcessHeap(), 0, saved_map);
        } else document = _strdup(json);
        CHECK(document); if (!document) { sh_package_provenance_free(&saved); continue; }
        plan = sh_package_runtime_prepare_map_document(data, map, present ? &saved : NULL, &row, 1,
            document, document_length, error, sizeof(error));
        if (!plan) fprintf(stderr, "inline load prepare: %s\n", error);
        CHECK(plan);
        bound = plan ? sh_package_map_plan_rebind(plan, present ? &saved : NULL, document, document_length,
            &bound_length, error, sizeof(error)) : NULL;
        CHECK(bound);
        if (bound) {
            /* Without the conversion the used package is missing locally: the
             * map would ask to install it. */
            CHECK(inline_load_missing(plan, registry, (uintptr_t)f.reflection, bound, bound_length, &missing) &&
                missing.count && sh_package_owners_count(&missing.packages) == 1);
            int result = sh_package_runtime_native_document(plan, present ? &saved : NULL, NULL, 0, bound, bound_length,
                &derived, &derived_length, &ownership, &used, &count, &report, error, sizeof(error));
            if (result != 1) fprintf(stderr, "inline load route %d/%d: %d %s %s\n", modern, mixed, result, error,
                report ? report : "");
            int converted = result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") &&
                strstr(derived, "\"state\":{\"edit\":{\"health\":40}}") && count == (mixed ? 1u : 0u);
            if (!converted) fprintf(stderr, "inline load route %d/%d: %zu deliveries\n%s\n%s\n", modern, mixed, count,
                report ? report : "", derived ? derived : "");
            CHECK(converted);
            if (derived) {
                CHECK(inline_load_missing(plan, registry, (uintptr_t)f.reflection, derived, derived_length, &missing));
                /* Only the unconverted use's member is missing, and it selects
                 * its whole package; a fully native map needs nothing. */
                int expected = mixed ? missing.count == 1 && strstr(missing.paths[0], "inline/fraction.decl") &&
                    sh_package_owners_count(&missing.packages) == 1 :
                    !missing.count && !sh_package_owners_count(&missing.packages);
                for (size_t m = 0; !expected && m < missing.count; m++)
                    fprintf(stderr, "inline load route %d/%d missing %s\n", modern, mixed, missing.paths[m]);
                CHECK(expected);
            }
            HeapFree(GetProcessHeap(), 0, bound);
        }
        CHECK(!memcmp(before, sh_package_bundle_fingerprint(row.original), 32));
        free(derived); free(report); free(document);
        sh_package_occurrence_release(ownership); sh_mpkg_used_free(used);
        sh_package_map_plan_free(plan); sh_package_provenance_free(&saved);
    }
done:
    sh_package_missing_free(&missing);
    sh_package_bundle_release(row.original); sh_package_sources_free(sources);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(map);
}
/* R137 save through the installed library, in its own process. The library
 * links a package's stock parents under their stock names and serves them
 * composed from the originals (the live 2d_speaker case: a canonical one-line
 * entityDef with editorVars re-sorted, and the stock editor entry the package's
 * own entry inherits). None of those copies is a package change, so map data
 * inheriting the stock definition stays exact and a plain package editor entry
 * still copies the shipped one. A package that does change that stock editor
 * entry keeps its uses on the package route. */
static void lease_collection_setup(admission_fixture *f);
static const char *const library_editor_rows[] = {"inlinestock/row_base", "inlinestock/base_row"};
static int library_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    const char *text = !strcmp(path, "generated/decls/entitydef/inlinestock/base.decl") ?
            "{\n\tclass = \"Entity\";\n\teditorVars {\n\t\tplaceable = true;\n\t\tcolor = ( 1, 1, 1, 1 );\n\t}\n"
            "\tedit = {\n\t\ttarget = \"\";\n\t\thealth = 1;\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/row_base.decl") ?
            "{\n\tedit = {\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/base_row.decl") ?
            "{\n\tinherit = \"inlinestock/row_base\";\n\tedit = {\n\t\tentityDef = \"inlinestock/base\";\n\t}\n}\n" : NULL;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static int library_convert(const char *label, char **report)
{
    char error[2048], *derived = NULL;
    size_t bound_length = 0, derived_length = 0, count = 99;
    sh_package_occurrence_snapshot *ownership = NULL; sh_mpkg_used *used = NULL;
    const char *json = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "]}";
    char *bound = sh_package_runtime_rebind_local_document(json, strlen(json), &bound_length, error, sizeof(error));
    if (!bound) fprintf(stderr, "inline library %s rebind: %s\n", label, error);
    CHECK(bound != NULL);
    int result = bound ? sh_package_runtime_native_document(NULL, NULL, NULL, 0, bound, bound_length, &derived,
        &derived_length, &ownership, &used, &count, report, error, sizeof(error)) : -2;
    fprintf(stderr, "inline library %s: result %d, %zu deliveries: %s\n%s", label, result, count, error,
        *report ? *report : "");
    int converted = result == 1 && derived && strstr(derived, "\"inherit\":\"inlinestock/base\"") && count == 0;
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    free(derived); sh_package_occurrence_release(ownership); sh_mpkg_used_free(used);
    return result < 0 ? -1 : converted;
}
static int library_served(const char *path)
{
    unsigned char *served = NULL; size_t length = 0;
    int shared = sh_package_runtime_read(path, &served, &length);
    fprintf(stderr, "inline library: %s served under its name: %d (%zu bytes)%s%.*s\n", path, shared, length,
        served ? ": " : "", served ? (int)length : 0, served ? (const char *)served : "");
    free(served);
    return shared;
}
static void test_native_inline_library(void)
{
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], *report = NULL;
    snprintf(data, sizeof(data), "%s/inline-library", root);
    create("inline-library/overrides/tough/package.json", "{\"id\":\"library-tough\",\"name\":\"Library tough\"}");
    create("inline-library/overrides/tough/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    /* The package's own editor entry: a plain copy of the stock entry. */
    create("inline-library/overrides/tough/assets/generated/decls/snapeditorentitydef/inline/tough_row.decl",
        "{ inherit = \"inlinestock/base_row\"; edit = { entityDef = \"inline/tough\"; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(library_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(library_editor_rows, sizeof(library_editor_rows) / sizeof(*library_editor_rows));
    CHECK(sh_package_runtime_refresh(data));
    CHECK(library_served("generated/decls/entitydef/inlinestock/base.decl") == 1);
    /* The library serves stock editor entries under their own names through its
     * baseline-palette reverse roots, which need the game catalog this fixture
     * lacks; test_native_inline_placement_provenance covers that compiled shape. */
    library_served("generated/decls/snapeditorentitydef/inlinestock/base_row.decl");
    /* The unowned canonical entityDef copy obstructs nothing. */
    CHECK(library_convert("unowned stock copies", &report) == 1);
    free(report); report = NULL;
    /* Another installed package changes the stock editor entry itself: the
     * plain package entry no longer copies what SnapMap ships, so the use keeps
     * its package (a product gap, never a silent native substitute). */
    create("inline-library/overrides/rows/package.json", "{\"id\":\"library-rows\",\"name\":\"Library rows\"}");
    create("inline-library/overrides/rows/assets/generated/decls/snapeditorentitydef/inlinestock/base_row.decl",
        "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; inputs = { num = 0; } } }");
    CHECK(sh_package_runtime_refresh(data));
    library_served("generated/decls/snapeditorentitydef/inlinestock/base_row.decl");
    CHECK(library_convert("owned stock editor change", &report) == 0);
    CHECK(report && strstr(report, "kept (product gap): its package's editor entry") && strstr(report, "not a plain copy"));
    free(report); report = NULL;
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* The compiled shape that failed live (r137c): after an install the library
 * serves the stock editor entry a package entry inherits under its own name,
 * as a composed copy (baseline known, not byte-original, no owner). That copy
 * stands for the shipped entry, so the plain package entry still copies it.
 * A package-owned change to that stock entry must keep the package route. */
static sh_compiled_resource provenance_resource(const char *path, const char *body, const char *original,
    int baseline, size_t owner)
{
    sh_compiled_resource out = {0};
    out.engine_path = path; out.original_path = (char *)original; out.baseline_known = baseline;
    out.body = (unsigned char *)body; out.body_length = strlen(body); out.source = SIZE_MAX;
    if (owner != SIZE_MAX) { CHECK(sh_package_owners_add(&out.owners, owner)); CHECK(sh_package_owners_add(&out.gameplay_owners, owner)); }
    return out;
}
static void test_native_inline_placement_provenance(void)
{
    sh_package_sources sources = {0};
    sh_compiled_resource resources[3];
    sh_package_compilation compiled = {0};
    char reason[512]; int engine = 0;
    /* Sorted by engine path, as compilations are. */
    resources[0] = provenance_resource("generated/decls/entitydef/smp/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }", "generated/decls/entitydef/inline/tough.decl", 0, 0);
    resources[1] = provenance_resource("generated/decls/snapeditorentitydef/inlinestock/base_row.decl",
        "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; } }", NULL, 1, SIZE_MAX);
    resources[2] = provenance_resource("generated/decls/snapeditorentitydef/smp/tough_row.decl",
        "{ inherit = \"inlinestock/base_row\"; edit = { entityDef = \"smp/tough\"; } }",
        "generated/decls/snapeditorentitydef/inline/tough_row.decl", 0, 0);
    compiled.sources = &sources; compiled.resources = resources; compiled.resource_count = 3;
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inline_baseline, NULL);
    sh_package_runtime_test_editor_rows(inline_editor_rows, sizeof(inline_editor_rows) / sizeof(*inline_editor_rows));
    /* Unowned composed stock copy: the plain package entry places natively. */
    int placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    if (placed != 1) fprintf(stderr, "placement provenance (unowned copy): %d %s\n", placed, reason);
    CHECK(placed == 1);
    /* The same stock entry changed by a package (it has an owner): refused. */
    CHECK(sh_package_owners_add(&resources[1].owners, 1));
    placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    if (placed != 0) fprintf(stderr, "placement provenance (owned change): %d %s\n", placed, reason);
    CHECK(placed == 0 && !engine && strstr(reason, "smp/tough_row") && strstr(reason, "not a plain copy"));
    for (size_t i = 0; i < 3; i++) { sh_package_owners_free(&resources[i].owners); sh_package_owners_free(&resources[i].gameplay_owners); }
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
}
static void run_native_inline_library(void)
{
    char executable[MAX_PATH], command[MAX_PATH + 64];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    DWORD result = 1;
    CHECK(GetModuleFileNameA(NULL, executable, sizeof(executable)));
    snprintf(command, sizeof(command), "\"%s\" --native-inline-library", executable);
    /* Share this process's output so the isolated case's diagnostics reach the log. */
    fflush(stdout); fflush(stderr);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    CHECK(CreateProcessA(executable, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process));
    if (!process.hProcess) return;
    CHECK(WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &result) && !result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
static void test_historical_native_identity_use(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"entities\":["
        "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/root\"}]}";
    char data[MAX_PATH], map[MAX_PATH], error[2048];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    sh_package_occurrence_snapshot *document = NULL, *played = NULL, *world = NULL;
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    uintptr_t object = 0x1000;
    sh_package_provenance selected = {0};
    char *bound = NULL, *captured = NULL, *replayed = NULL;
    size_t bound_length = 0, captured_length = 0, replayed_length = 0;
    create("identity-data/overrides", NULL);
    create("identity-map/overrides/equal/package.json", "{\"id\":\"equal-copy\",\"name\":\"Equal copy\"}");
    create("identity-map/overrides/equal/assets/generated/decls/entitydef/history/root.decl", historical_original);
    create("identity-map/overrides/equal/assets/generated/decls/entitydef/history/child.decl",
        "{ class = \"Entity\"; edit = { health = 5; } }");
    snprintf(data, sizeof(data), "%s/identity-data", root);
    snprintf(map, sizeof(map), "%s/identity-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    if (!plan) fprintf(stderr, "identity prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    bound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &bound_length, error, sizeof(error));
    if (!bound) fprintf(stderr, "identity rebind: %s\n", error);
    CHECK(bound && strstr(bound, "\"inherit\":\"history/root\""));
    if (!bound) goto done;
    int present = 0;
    CHECK(sh_map_provenance_read(bound, bound_length, &selected, &present, error, sizeof(error)));
    /* The native identity is no package use and needs no delivery. */
    CHECK(!selected.use_count);
    captured = sh_package_runtime_capture_document(plan, &selected, NULL, 0, bound, bound_length,
        &captured_length, &document, NULL, NULL, error, sizeof(error));
    if (!captured) fprintf(stderr, "identity capture: %s\n", error);
    CHECK(captured && document); if (!document) goto done;
    /* The loaded editor object holds its occurrence (and any leases) live. */
    CHECK(sh_package_occurrence_attach_snapshot(tracker, 1, &object, 1, document, &world, error, sizeof(error)));
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, native_identity_activate, NULL));
    /* Play captures the live document against the active provider's retained
     * native uses; a vanilla site must not be refused there. */
    replayed = sh_package_runtime_capture_document(NULL, sh_package_occurrence_provenance(document), NULL, 0,
        captured, captured_length, &replayed_length, &played, NULL, NULL, error, sizeof(error));
    if (!replayed) fprintf(stderr, "identity native capture: %s\n", error);
    CHECK(replayed && played && replayed_length == captured_length && !memcmp(replayed, captured, captured_length));
done:
    free(replayed); free(captured); if (bound) HeapFree(GetProcessHeap(), 0, bound);
    sh_package_occurrence_release(played); sh_package_occurrence_release(world); sh_package_occurrence_release(document);
    sh_package_occurrence_tracker_free(tracker);
    sh_package_provenance_free(&selected); sh_package_map_plan_free(plan);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
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
    sh_package_key_set retained_keys = {0}, decoded_keys = {0};
    uint32_t retained_group = sh_package_key_private(12);
    CHECK(sh_package_key_set_add(&retained_keys, sh_package_key_private(40)));
    CHECK(sh_package_key_set_add(&retained_keys, sh_package_key_private(41)));
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
        if (!i) {
            sh_package_key_groups groups = {0};
            CHECK(sh_package_key_groups_define(&groups, retained_group, &retained_keys));
            sh_package_runtime_test_plan_key_table(plans[i], &groups);
        }
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
    sh_package_runtime_key_decode(retained_group, &decoded_keys, NULL);
    CHECK(sh_package_key_set_equal(&retained_keys, &decoded_keys));
    CHECK(sh_package_runtime_test_key_groups_current());
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
    sh_package_runtime_key_decode(retained_group, &decoded_keys, NULL);
    CHECK(!sh_package_key_set_equal(&retained_keys, &decoded_keys));
    CHECK(sh_package_key_set_has(&decoded_keys, retained_group));
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
/* A numeric spawn request resolves through a conductor registration, rather
 * than naming the AI declaration directly. Retaining its file alone cannot
 * prove that a copied occurrence still selects the same AI. */
static void lease_registration_setup(admission_fixture *f)
{
    admission_setup(f);
    void *container = *(void **)f->reflection;
    void *records = admission_alloc(f, 9 * 0x38), *objects = admission_alloc(f, 9 * 16);
    void *pointers = admission_alloc(f, 9 * 16), *element = admission_alloc(f, 0x48);
    memcpy(records, *(void **)((char *)container + 0x20), 5 * 0x38);
    memcpy(objects, *(void **)((char *)f->reflection + 0x88), 5 * 16);
    memcpy(pointers, *(void **)((char *)f->reflection + 0xa0), 5 * 16);
    admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 9);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 9);
    admission_pointer(records, 5 * 0x38, admission_string(f, "SpawnInfo"));
    admission_pointer(records, 5 * 0x38 + 8, admission_string(f, ""));
    admission_pointer(records, 6 * 0x38, admission_string(f, "idList < SpawnInfo >"));
    admission_pointer(records, 6 * 0x38 + 8, admission_string(f, ""));
    admission_pointer(records, 7 * 0x38, admission_string(f, "float"));
    admission_pointer(records, 7 * 0x38 + 8, admission_string(f, ""));
    admission_pointer(records, 6 * 0x38 + 0x18, element);
    admission_pointer(objects, 6 * 16 + 8, (void *)0x80);
    admission_pointer(element, 0, admission_string(f, "SpawnInfo"));
    admission_pointer(element, 8, admission_string(f, ""));
    admission_pointer(element, 0x10, admission_string(f, "element")); admission_number(element, 0x1c, 8);
    void *enums = admission_alloc(f, 2 * 0x18), *readers = admission_alloc(f, 2 * 16);
    void *members = admission_alloc(f, 22 * 0x10);
    admission_pointer(enums, 0, admission_string(f, "spawnAIType_t")); admission_pointer(enums, 0x10, members);
    for (int i = 0; i < 21; i++) {
        char name[16]; snprintf(name, sizeof(name), "T%d", i);
        admission_pointer(members, i * 0x10, admission_string(f, i == 20 ? "ALL" : name));
        admission_number(members, i * 0x10 + 8, i < 19 ? 1 << i : i == 19 ? 1 << 30 : -1);
    }
    admission_pointer(container, 0x10, enums);
    admission_pointer(f->reflection, 0x58, readers); admission_number(f->reflection, 0x60, 2);
    const char *types[] = {"int", "spawnAIType_t", "idList < SpawnInfo >", "idList < SpawnInfo >",
        "spawnAIType_t", "idDeclEntityDef", "float"};
    const char *names[] = {"health", "spawnType", "aiTypeList", "globalAIMods", "aiType", "entityDef", "healthMod"};
    void *entity = admission_alloc(f, 5 * 0x48), *row = admission_alloc(f, 4 * 0x48);
    admission_pointer(records, 0x38 + 0x20, entity); admission_pointer(records, 5 * 0x38 + 0x20, row);
    for (size_t i = 0; i < 7; i++) {
        char *field = i < 4 ? (char *)entity + i * 0x48 : (char *)row + (i - 4) * 0x48;
        admission_pointer(field, 0, admission_string(f, types[i]));
        admission_pointer(field, 8, admission_string(f, i == 5 ? "*" : ""));
        admission_pointer(field, 0x10, admission_string(f, names[i])); admission_number(field, 0x1c, 8);
    }
}
static int lease_registration_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/entitydef/conductor.decl")) return 0;
    const char *text = "{ class = \"Entity\"; edit = { aiTypeList = { num = 0; } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 2; } } } }";
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static uint32_t lease_registration_request(const sh_compiled_resource *r)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)r->body, r->body_length}, NULL, 0);
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const char *value = NULL; size_t length = 0; uint32_t key = 0;
    CHECK(sh_decl_tree_literal(sh_decl_tree_member(edit, "spawnType"), &value, &length));
    CHECK(sh_package_key_bits(NULL, value, length, &key));
    sh_decl_tree_free(tree); return key;
}
typedef struct registration_activation {
    uint32_t classification;
    int calls, recoveries, fail;
} registration_activation;
static int registration_admit(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    registration_activation *state = context;
    CHECK(sh_package_runtime_test_key_groups_current());
    CHECK(sh_package_runtime_test_unmodified_type(state->classification) == !restoring);
    for (size_t i = 0; i < changes->count; i++)
        CHECK(changes->items[i].kind == (restoring ? SH_PACKAGE_RESOURCE_REMOVED : SH_PACKAGE_RESOURCE_ADDED));
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "registration admission fixture failure"); return 0; }
    return 1;
}
static void test_retained_registration_meaning(int additive)
{
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], error[2048], request[1024] = "", ai[1024] = "";
    sh_package_map_plan *plan = NULL;
    sh_package_occurrence_snapshot *document = NULL;
    uint32_t local_key = 0, map_key = 0;
    char local_ai[1024] = "";
    for (size_t i = 0; i < 2; i++) {
        const char *directory = i ? "registration-map" : "registration-data";
        char path[512];
        snprintf(path, sizeof(path), "%s/overrides/variant/package.json", directory);
        create(path, "{\"id\":\"variant\",\"name\":\"Variant\"}");
        snprintf(path, sizeof(path), "%s/overrides/variant/assets/generated/decls/entitydef/unit.decl", directory);
        create(path, i ? "{ class = \"Entity\"; edit = { health = 10; } }" :
            "{ class = \"Entity\"; edit = { health = 12000; } }");
        snprintf(path, sizeof(path), "%s/overrides/variant/assets/generated/decls/entitydef/request.decl", directory);
        create(path, "{ class = \"Entity\"; edit = { spawnType = 524288; } }");
        snprintf(path, sizeof(path), "%s/overrides/variant/assets/generated/decls/entitydef/conductor.decl", directory);
        create(path, "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524292; entityDef = \"unit\"; } } } }");
    }
    snprintf(data, sizeof(data), "%s/registration-data", root); snprintf(map, sizeof(map), "%s/registration-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_registration_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_spawn_keys(1);
    int refreshed = sh_package_runtime_refresh(data);
    if (!refreshed) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "registration refresh: %s\n", error); }
    CHECK(refreshed);
    const sh_package_compilation *local = sh_package_runtime_acquire();
    for (size_t i = 0; local && i < local->resource_count; i++) {
        const sh_compiled_resource *r = local->resources + i;
        if (r->body && strstr((char *)r->body, "spawnType =")) local_key = lease_registration_request(r);
        if (r->body && strstr((char *)r->body, "health = 12000")) strcpy_s(local_ai, sizeof(local_ai), r->native_name);
    }
    sh_package_runtime_release(); CHECK(local_key && *local_ai);
    plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    if (!plan) fprintf(stderr, "registration map prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    const sh_package_compilation *compiled = sh_package_map_plan_compilation(plan);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->body && strstr((char *)r->body, "spawnType =")) {
            strcpy_s(request, sizeof(request), r->native_name); map_key = lease_registration_request(r);
        }
        if (r->body && strstr((char *)r->body, "health = 10")) strcpy_s(ai, sizeof(ai), r->native_name);
    }
    CHECK(*request && *ai); if (!*request || !*ai) goto done;
    char json[2048]; size_t length = 0;
    snprintf(json, sizeof(json), "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", request);
    char *bound = sh_package_runtime_capture_document(plan, NULL, NULL, 0, json, strlen(json),
        &length, &document, NULL, NULL, error, sizeof(error));
    if (!bound) fprintf(stderr, "registration capture: %s\n", error);
    CHECK(bound && document); free(bound); if (!document) goto done;
    if (additive) {
        uint32_t classification = 0;
        sh_package_key_space numeric = {0};
        for (size_t i = 0; i < compiled->resource_count; i++) {
            const sh_compiled_resource *r = compiled->resources + i;
            if (!r->body || !strstr((char *)r->body, "aiTypeList")) continue;
            sh_package_spawn_registry table = {0};
            int read = sh_package_spawn_registry_read((sh_decl_source){(char *)r->body, r->body_length},
                &numeric, &table, error, sizeof(error));
            CHECK(read >= 0);
            for (size_t row = 0; row < table.count; row++)
                if (!strcmp(table.rows[row].entity, ai)) { classification = table.rows[row].mask; break; }
            sh_package_spawn_registry_free(&table);
            if (classification) break;
        }
        CHECK(classification);
        registration_activation state = {classification, 0, 0, 1};
        CHECK(sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
            registration_admit, NULL, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
        CHECK(state.calls == 1 && state.recoveries == 1 && sh_package_runtime_ready());
        CHECK(sh_package_runtime_test_key_groups_current() &&
            !sh_package_runtime_test_unmodified_type(classification));
        state.fail = 0;
        int result = sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
            registration_admit, NULL, &state, error, sizeof(error));
        if (result != SH_PACKAGE_ADMISSION_READY) fprintf(stderr, "registration admission: %s\n", error);
        CHECK(result == SH_PACKAGE_ADMISSION_READY && state.calls == 2 && state.recoveries == 1);
        CHECK(sh_package_runtime_test_key_groups_current());
        CHECK(sh_package_runtime_test_unmodified_type(classification));
    } else CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, NULL, NULL));
    sh_package_map_plan_free(plan); plan = NULL;
    if (additive) {
        /* The next native load publishes retained registrations without a map
         * reopen, library rescan or replacement of the installed packages. */
        int fail = 0;
        CHECK(sh_package_runtime_occurrences_pending());
        CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
        CHECK(!sh_package_runtime_occurrences_pending());
    } else CHECK(sh_package_runtime_activate_map(data, NULL, (sh_package_activation_guard){0}, NULL, NULL));
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *conductor = sh_package_compilation_find(current, "generated/decls/entitydef/conductor.decl");
    CHECK(conductor && conductor->body && strstr((char *)conductor->body, ai));
    sh_package_spawn_registry registrations = {0}; sh_package_key_space key_space = {0};
    CHECK(conductor && sh_package_spawn_registry_read((sh_decl_source){(char *)conductor->body, conductor->body_length},
        &key_space, &registrations, error, sizeof(error)) == 1);
    sh_package_runtime_release();
    sh_package_key_groups groups;
    sh_package_runtime_test_serving_key_table(&groups);
    size_t local_matches = 0, map_matches = 0;
    CHECK(local_key != map_key && strcmp(local_ai, ai));
    for (size_t i = 0; i < registrations.count; i++) {
        const sh_package_spawn_row *row = registrations.rows + i;
        if (sh_package_key_request_matches(&groups, map_key, row->mask)) {
            map_matches++; CHECK(!strcmp(row->entity, ai));
            CHECK(sh_package_runtime_test_unmodified_type(row->mask));
        }
        if (sh_package_key_request_matches(&groups, local_key, row->mask)) {
            local_matches++; CHECK(!strcmp(row->entity, local_ai));
            CHECK(!sh_package_runtime_test_unmodified_type(row->mask));
        }
    }
    CHECK(map_matches == 1 && local_matches == 1);
    sh_package_spawn_registry_free(&registrations);
done:
    sh_package_occurrence_release(document); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_spawn_keys(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); cleanup_resources(data);
    while (f.count) free(f.regions[--f.count].address);
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
    CHECK(sh_package_runtime_test_key_groups_current());
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
    state->selector = sh_package_runtime_selectors_retain("snapeditorentitydef", error, capacity); CHECK(state->selector);
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
/* An installation's additive increment serves what a full rescan of the same
 * installed inventory serves, with one designed difference: a newly installed
 * package's shared editor declaration keeps a private alias until the next
 * rescan, because an increment never rewrites or adds to the open editor's
 * shared objects (pl_document_shared); the rescan then composes it under its
 * authored name. Snapshot of one library as rows sorted by engine path. */
typedef struct library_row { char *path, *original; unsigned char *body; size_t length; } library_row;
static int library_row_order(const void *a, const void *b)
{ return strcmp(((const library_row *)a)->path, ((const library_row *)b)->path); }
static library_row *library_snapshot(size_t *count)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    library_row *rows = library ? calloc(library->resource_count ? library->resource_count : 1, sizeof(*rows)) : NULL;
    *count = 0;
    for (size_t i = 0; rows && i < library->resource_count; i++) {
        char error[256];
        rows[i].path = _strdup(library->resources[i].engine_path);
        rows[i].original = library->resources[i].original_path ? _strdup(library->resources[i].original_path) : NULL;
        rows[i].body = sh_package_compilation_read(library, library->resources + i, SIZE_MAX,
            &rows[i].length, error, sizeof(error));
        CHECK(rows[i].path && rows[i].body);
        (*count)++;
    }
    sh_package_runtime_release();
    if (rows && *count > 1) qsort(rows, *count, sizeof(*rows), library_row_order);
    return rows;
}
static void library_rows_free(library_row *rows, size_t count)
{
    for (size_t i = 0; rows && i < count; i++) { free(rows[i].path); free(rows[i].original); free(rows[i].body); }
    free(rows);
}
/* 1 when the increment rows equal the rescan rows except for shared editor
 * declarations the rescan serves under their authored path and the increment
 * under a private alias of exactly that path; *aliases counts those. */
static int library_rows_designed(const library_row *a, size_t an, const library_row *b, size_t bn, size_t *aliases)
{
    size_t i = 0, j = 0; int equal = 1;
    *aliases = 0;
    while (i < an || j < bn) {
        int order = i == an ? 1 : j == bn ? -1 : strcmp(a[i].path, b[j].path);
        if (order < 0) {
            int alias = 0;
            for (size_t k = 0; a[i].original && k < bn; k++)
                if (!strcmp(b[k].path, a[i].original) && !b[k].original &&
                    strstr(b[k].path, "generated/decls/snapeditor") == b[k].path) alias = 1;
            if (!alias) { fprintf(stderr, "increment only: %s\n", a[i].path); equal = 0; }
            i++; continue;
        }
        if (order > 0) {
            int alias = 0;
            for (size_t k = 0; k < an; k++) if (a[k].original && !strcmp(a[k].original, b[j].path)) alias = 1;
            if (!alias || b[j].original || strstr(b[j].path, "generated/decls/snapeditor") != b[j].path) {
                fprintf(stderr, "rescan only: %s\n", b[j].path); equal = 0;
            } else (*aliases)++;
            j++; continue;
        }
        if (a[i].length != b[j].length || memcmp(a[i].body, b[j].body, a[i].length)) {
            fprintf(stderr, "different bytes: %s\n", a[i].path); equal = 0;
        }
        i++; j++;
    }
    return equal;
}
static void test_increment_designed_difference(void)
{
    const char *installed_descriptor = "{\"id\":\"already-installed\",\"name\":\"Already installed\"}";
    const char *descriptor = "{\"id\":\"increment-map\",\"name\":\"Increment map\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    library_row *incremented = NULL, *rescanned = NULL; size_t incremented_count = 0, rescanned_count = 0;
    create("increment-data/overrides/installed-first/package.json", installed_descriptor);
    create("increment-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    authoring_bundle("increment-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/increment-data", root);
    snprintf(source, sizeof(source), "%s/increment-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("increment-data/overrides/installed", descriptor);
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
        &(int){0}));
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library != state.library); sh_package_runtime_release();
    incremented = library_snapshot(&incremented_count);
    CHECK(sh_package_runtime_refresh(data));
    rescanned = library_snapshot(&rescanned_count);
    size_t aliases = 0;
    CHECK(incremented_count && library_rows_designed(incremented, incremented_count, rescanned, rescanned_count,
        &aliases) && aliases == 1); /* cross/controls */
done:
    library_rows_free(incremented, incremented_count); library_rows_free(rescanned, rescanned_count);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* A published installation that no map load continues (the MPKG gate) folds
 * its packages in as a map-driven installation does: the refresh compiles only
 * the added packages, serves the rest from the retained library and so serves
 * exactly what the installation increment serves (one designed alias apart
 * from a whole-library rescan). A new package failing on its own bytes is left
 * out there too. A refresh that finds an installed package removed or changed
 * compiles the whole library, and a refresh no installation asked for always
 * does. */
static void remove_created(const char *relative);
/* The installed library's resource authored at original, which installed
 * links may serve under a private name: its text contains wanted, or it is
 * absent when wanted is NULL. */
static void expect_library_text(const char *original, const char *wanted)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *resource = historical_resource(library, original);
    size_t length = 0; char error[256];
    unsigned char *body = resource ? sh_package_compilation_read(library, resource, SIZE_MAX, &length, error, sizeof(error)) : NULL;
    int ok = wanted ? body && strstr((const char *)body, wanted) != NULL : !resource;
    if (!ok) fprintf(stderr, "library resource %s: wanted %s; got %s\n", original, wanted ? wanted : "(absent)",
        body ? (const char *)body : resource ? "(unreadable)" : "(absent)");
    CHECK(ok); free(body); sh_package_runtime_release();
}
static void test_published_install_refresh(void)
{
    const char *installed_descriptor = "{\"id\":\"already-installed\",\"name\":\"Already installed\"}";
    const char *descriptor = "{\"id\":\"published-map\",\"name\":\"Published map\"}";
    char data[MAX_PATH]; char *summary = NULL;
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    library_row *published = NULL, *rescanned = NULL; size_t published_count = 0, rescanned_count = 0;
    create("published-data/overrides/installed-first/package.json", installed_descriptor);
    create("published-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    snprintf(data, sizeof(data), "%s/published-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    library_compiles = install_refreshes = 0;
    CHECK(sh_package_runtime_refresh(data) && library_compiles == 1);
    const sh_package_compilation *first = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    /* Published through the gate: added packages only. */
    authoring_bundle("published-data/overrides/installed", descriptor);
    create("published-data/overrides/broken/package.json", "{\"id\":\"broken-published\",\"name\":\"Broken published\"}");
    create("published-data/overrides/broken/assets/generated/decls/entitydef/broken.decl", "{ edit = { ");
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data) && install_refreshes == 1 && !library_compiles);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library != first && historical_resource(library, authoring_unused));
    CHECK(!historical_resource(library, "generated/decls/entitydef/broken.decl"));
    sh_package_runtime_release();
    expect_library_text("generated/decls/entitydef/first/thing.decl", "health = 9");
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "broken")); free(summary); summary = NULL;
    published = library_snapshot(&published_count);
    /* A refresh nobody asked to fold in compiles the whole library, replaying
     * unchanged discovery recorded by the earlier compiles. */
    library_compiles = install_refreshes = memo_replays = 0;
    CHECK(sh_package_runtime_refresh(data) && library_compiles >= 1 && !install_refreshes && memo_replays);
    rescanned = library_snapshot(&rescanned_count);
    size_t aliases = 0;
    CHECK(published_count && library_rows_designed(published, published_count, rescanned, rescanned_count,
        &aliases) && aliases == 1);
    /* A changed installed package needs the whole library even when asked. */
    create("published-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data) && library_compiles >= 1 && !install_refreshes);
    expect_library_text("generated/decls/entitydef/first/thing.decl", "health = 10");
    /* So does a removed one. */
    remove_created("published-data/overrides/installed-first");
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data) && library_compiles >= 1 && !install_refreshes);
    expect_library_text("generated/decls/entitydef/first/thing.decl", NULL);
    /* After an edit and a removal, the library the memo helped compile is the
     * one a fresh process compiles from the same files. */
    library_rows_free(published, published_count); published = library_snapshot(&published_count);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    memo_replays = 0;
    CHECK(sh_package_runtime_refresh(data) && !memo_replays);
    library_rows_free(rescanned, rescanned_count); rescanned = library_snapshot(&rescanned_count);
    aliases = 0;
    CHECK(published_count && library_rows_designed(published, published_count, rescanned, rescanned_count,
        &aliases) && !aliases);
    library_rows_free(published, published_count); library_rows_free(rescanned, rescanned_count);
    published = rescanned = NULL;
    free(summary);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
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
/* A newly installed package whose own source is malformed is left out of the
 * installation increment exactly as the installed library leaves it out: its
 * healthy peer is still served, map preparation succeeds with no message left
 * behind (the load continuation reads any message as a failure and cancels a
 * finished load), the commit folds the increment into the library, and no
 * whole-library rescan is scheduled for the unchanged healthy library. The
 * own-bytes decision is remembered: a later rescan leaves it out as before. */
static void test_local_authoring_defect(void)
{
    const char *descriptor = "{\"id\":\"defect-map\",\"name\":\"Defect map\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    char *summary = NULL;
    create("authoring-defect-data/overrides", NULL);
    authoring_bundle("authoring-defect-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/authoring-defect-data", root);
    snprintf(source, sizeof(source), "%s/authoring-defect-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    create("authoring-defect-data/overrides/broken/package.json", "{\"id\":\"broken-local\",\"name\":\"Broken local\"}");
    create("authoring-defect-data/overrides/broken/assets/generated/decls/entitydef/broken.decl", "{ edit = { ");
    authoring_bundle("authoring-defect-data/overrides/installed", descriptor);
    strcpy_s(error, sizeof(error), "stale");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    if (!prepared || error[0]) fprintf(stderr, "local authoring defect: %d %s\n", prepared, error);
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    summary = sh_package_runtime_summary();
    CHECK(summary && !strstr(summary, "Local authoring composition unavailable")); free(summary); summary = NULL;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_published, error, sizeof(error)) && inventory_commit_calls == 1);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library != state.library && historical_resource(library, authoring_unused));
    CHECK(!historical_resource(library, "generated/decls/entitydef/broken.decl"));
    sh_package_runtime_release();
    summary = sh_package_runtime_summary();
    int reported = summary && strstr(summary, "Skipped local package") && strstr(summary, "broken") &&
        !strstr(summary, "Local authoring composition unavailable");
    if (!reported) fprintf(stderr, "authoring defect summary: %s\n", summary ? summary : "(none)");
    CHECK(reported); free(summary); summary = NULL;
    sh_package_map_plan_free(plan); plan = NULL;
    /* Leaving the map rescans nothing: the committed library is kept. */
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &(int){0}));
    const sh_package_compilation *after = sh_package_runtime_library_acquire();
    CHECK(after == library); sh_package_runtime_release();
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data) && left_out_reuses == 1);
    expect_current("generated/decls/entitydef/broken.decl", NULL);
done:
    free(summary);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* Delete a fixture subtree made by create(), newest entry first, so the final
 * cleanup no longer expects it. */
static void remove_created(const char *relative)
{
    char prefix[4096];
    snprintf(prefix, sizeof(prefix), "%s/%s", root, relative);
    size_t n = strlen(prefix);
    for (size_t i = created_count; i > 0; i--) {
        if (strncmp(created[i - 1], prefix, n) || (created[i - 1][n] && created[i - 1][n] != '/')) continue;
        wchar_t *wide = sh_package_source_wide_path(created[i - 1]); CHECK(wide);
        if (wide) { CHECK(directories[i - 1] ? RemoveDirectoryW(wide) : DeleteFileW(wide)); free(wide); }
        memmove(created + i - 1, created + i, (created_count - i) * sizeof(created[0]));
        memmove(directories + i - 1, directories + i, (created_count - i) * sizeof(directories[0]));
        created_count--;
    }
}
/* A package the installed library rejected on its own bytes (a malformed
 * declaration) stays out, with its report, while its files are unchanged: a
 * rescan does not compile it again, and an installation's additive increment
 * still extends the library instead of failing on it (which left every
 * install to a full rebuild at the next map transition). A repaired, newly
 * broken or removed tree is judged afresh. */
static void test_left_out_packages(void)
{
    const char *descriptor = "{\"id\":\"left-out-map\",\"name\":\"Left out map\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048], *summary = NULL;
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    create("left-out-data/overrides/broken/package.json", "{\"id\":\"broken-local\",\"name\":\"Broken local\"}");
    create("left-out-data/overrides/broken/assets/generated/decls/entitydef/broken.decl", "{ edit = { ");
    authoring_bundle("left-out-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/left-out-data", root);
    snprintf(source, sizeof(source), "%s/left-out-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration"));
    free(summary);
    CHECK(sh_package_runtime_refresh(data) && left_out_reuses == 1);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration"));
    free(summary);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("left-out-data/overrides/installed", descriptor);
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    summary = sh_package_runtime_summary();
    CHECK(summary && !strstr(summary, "Local authoring composition unavailable")); free(summary);
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 1);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    CHECK(library != state.library && historical_resource(library, authoring_unused));
    CHECK(!historical_resource(library, "generated/decls/entitydef/broken.decl"));
    sh_package_runtime_release();
    summary = sh_package_runtime_summary();
    int reported = summary && strstr(summary, "Installed library: 1 packages") &&
        strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration") &&
        !strstr(summary, "Local authoring composition unavailable");
    if (!reported) fprintf(stderr, "left-out summary: %s\n", summary ? summary : "(none)");
    CHECK(reported); free(summary); summary = NULL;
    sh_package_map_plan_free(plan); plan = NULL;
    /* Its repaired tree is judged afresh and served. */
    create("left-out-data/overrides/broken/assets/generated/decls/entitydef/broken.decl",
        "{ class = \"Entity\"; edit = { health = 3; } }");
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
    expect_current("generated/decls/entitydef/broken.decl", "health = 3");
    summary = sh_package_runtime_summary();
    CHECK(summary && !strstr(summary, "Skipped local package")); free(summary); summary = NULL;
    /* A new defect is a new tree: judged afresh, then remembered as itself. */
    create("left-out-data/overrides/broken/assets/generated/decls/entitydef/broken.decl", "{ class = ");
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
    expect_current("generated/decls/entitydef/broken.decl", NULL);
    CHECK(sh_package_runtime_refresh(data) && left_out_reuses == 1);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration"));
    free(summary); summary = NULL;
    /* Removed from disk, it is neither left out nor reported. */
    remove_created("left-out-data/overrides/broken");
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
    summary = sh_package_runtime_summary();
    CHECK(summary && !strstr(summary, "Skipped local package") && strstr(summary, "Installed library: 1 packages"));
done:
    free(summary);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* An own-bytes decision outlives the process only for the same product and
 * game images and the same tree: a later process leaves the package out without
 * compiling it, with the same report, while a healthy peer is served. Another
 * image, a changed tree or a missing, truncated or corrupt memo is judged
 * afresh; nothing else is remembered. */
static void memo_edit(const char *data, int truncate)
{
    char path[MAX_PATH]; FILE *file = NULL; long size = 0;
    snprintf(path, sizeof(path), "%s/package-cache/own-bytes-exclusions-vk.txt", data);
    CHECK(!fopen_s(&file, path, "r+b") && file); if (!file) return;
    CHECK(!fseek(file, 0, SEEK_END)); size = ftell(file); CHECK(size > 8);
    if (truncate) {
        fclose(file);
        HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); CHECK(handle != INVALID_HANDLE_VALUE);
        LARGE_INTEGER at; at.QuadPart = size - 3; CHECK(SetFilePointerEx(handle, at, NULL, FILE_BEGIN) && SetEndOfFile(handle));
        CloseHandle(handle); return;
    }
    int c; CHECK(!fseek(file, size - 2, SEEK_SET)); c = fgetc(file);
    CHECK(!fseek(file, size - 2, SEEK_SET)); fputc(c == '0' ? '1' : '0', file); fclose(file);
}
static void memo_refresh(const char *data, unsigned expected_reuses, int broken_reported)
{
    char *summary;
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    left_out_reuses = 0;
    CHECK(sh_package_runtime_refresh(data));
    if (left_out_reuses != expected_reuses) fprintf(stderr, "session memo reuses %u, wanted %u\n", left_out_reuses, expected_reuses);
    CHECK(left_out_reuses == expected_reuses);
    expect_current("generated/decls/entitydef/memo_good.decl", "health = 2");
    summary = sh_package_runtime_summary();
    CHECK(summary && (broken_reported ? strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration") :
        !strstr(summary, "Skipped local package")));
    free(summary);
}
/* Independent packages that fail on their own content are left out together:
 * one failed installed-library compile finds every one of them, in whichever
 * order discovery meets them, and the next compile links with the healthy
 * peer; a later process leaves all of them out without compiling. Two kinds:
 * text the declaration parser rejects in its isolated scope, and a scalar the
 * native grammar rejects during source discovery. */
static int together_environment_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (!strcmp(path, "generated/decls/entitydef/together/env_parent.decl")) { *body = NULL; *length = 0; return -1; }
    return lease_collection_baseline(context, path, body, length);
}
static void test_malformed_owners_together(void)
{
    static const char *const bad[2] = {"{ edit = { ",
        "{ class = \"Entity\"; edit = { health = 3\r\n target = \"\"; } }"};
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    for (int kind = 0; kind < 2; kind++) for (int order = 0; order < 2; order++) {
        char data[MAX_PATH], path[MAX_PATH], *summary = NULL;
        const char *one = order ? "z-one" : "a-one", *two = order ? "a-two" : "z-two";
        const char *folders[3] = {one, "m-good", two};
        const char *names[3] = {"one", "good", "two"};
        for (int p = 0; p < 3; p++) {
            snprintf(path, sizeof(path), "together-%d-%d/overrides/%s/package.json", kind, order, folders[p]);
            char descriptor_text[128];
            snprintf(descriptor_text, sizeof(descriptor_text), "{\"id\":\"together-%s\",\"name\":\"Together %s\"}",
                names[p], names[p]);
            create(path, descriptor_text);
            snprintf(path, sizeof(path), "together-%d-%d/overrides/%s/assets/generated/decls/entitydef/together/%s.decl",
                kind, order, folders[p], names[p]);
            create(path, p == 1 ? "{ class = \"Entity\"; edit = { health = 2; target = \"\"; } }" : bad[kind]);
        }
        snprintf(data, sizeof(data), "%s/together-%d-%d", root, kind, order);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        library_compiles = left_out_packages = 0;
        CHECK(sh_package_runtime_refresh(data));
        if (library_compiles != 2)
            fprintf(stderr, "malformed owners kind %d order %d: %u installed-library compiles\n", kind, order, library_compiles);
        CHECK(library_compiles == 2); /* One failed pass finds both, then the link. */
        expect_library_text("generated/decls/entitydef/together/good.decl", "health = 2");
        expect_library_text("generated/decls/entitydef/together/one.decl", NULL);
        expect_library_text("generated/decls/entitydef/together/two.decl", NULL);
        summary = sh_package_runtime_summary();
        int both = summary && strstr(summary, one) && strstr(summary, two) && !strstr(summary, "m-good:");
        if (!both) fprintf(stderr, "malformed owners summary: %s\n", summary ? summary : "(none)");
        CHECK(both);
        free(summary);
        /* A later process of the same build leaves both out without compiling them. */
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
        library_compiles = left_out_packages = 0;
        CHECK(sh_package_runtime_refresh(data));
        if (library_compiles != 1 || left_out_packages != 2)
            fprintf(stderr, "malformed owners kind %d order %d later: %u compiles, %u left out\n",
                kind, order, library_compiles, left_out_packages);
        CHECK(library_compiles == 1 && left_out_packages == 2);
        expect_library_text("generated/decls/entitydef/together/good.decl", "health = 2");
        sh_package_runtime_test_dispose(); /* Close retained bundle streams before removing them. */
        cleanup_resources(data);
    }
    /* A capture that withdraws a malformed package and then meets an
     * environmental failure (an unreadable original) blames nobody: no
     * package is skipped or remembered, and the refresh keeps no new library. */
    {
        char data[MAX_PATH];
        create("together-env/overrides/a-one/package.json", "{\"id\":\"together-one\",\"name\":\"Together one\"}");
        create("together-env/overrides/a-one/assets/generated/decls/entitydef/together/one.decl", bad[1]);
        create("together-env/overrides/m-env/package.json", "{\"id\":\"together-env\",\"name\":\"Together env\"}");
        create("together-env/overrides/m-env/assets/generated/decls/entitydef/together/env.decl",
            "{ inherit = \"together/env_parent\"; edit = { health = 4; } }");
        create("together-env/overrides/z-two/package.json", "{\"id\":\"together-two\",\"name\":\"Together two\"}");
        create("together-env/overrides/z-two/assets/generated/decls/entitydef/together/two.decl", bad[1]);
        snprintf(data, sizeof(data), "%s/together-env", root);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(together_environment_baseline, NULL);
        skipped_packages = 0;
        int refreshed = sh_package_runtime_refresh(data);
        if (refreshed || skipped_packages)
            fprintf(stderr, "environment after withdrawals: refreshed %d, %u skipped\n", refreshed, skipped_packages);
        CHECK(!refreshed && !skipped_packages);
        sh_package_runtime_test_dispose();
        cleanup_resources(data);
    }
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
static void test_left_out_across_processes(void)
{
    char data[MAX_PATH];
    admission_fixture f; lease_collection_setup(&f);
    create("memo-data/overrides/broken/package.json", "{\"id\":\"memo-broken\",\"name\":\"Memo broken\"}");
    create("memo-data/overrides/broken/assets/generated/decls/entitydef/memo_broken.decl", "{ edit = { ");
    create("memo-data/overrides/good/package.json", "{\"id\":\"memo-good\",\"name\":\"Memo good\"}");
    create("memo-data/overrides/good/assets/generated/decls/entitydef/memo_good.decl",
        "{ class = \"Entity\"; edit = { health = 2; } }");
    snprintf(data, sizeof(data), "%s/memo-data", root);
    sh_package_runtime_test_build_identity("game A\nproduct A\n");
    memo_refresh(data, 0, 1);  /* judged, reported, remembered */
    memo_refresh(data, 1, 1);  /* a later process: left out without compiling */
    memo_refresh(data, 1, 1);  /* and again */
    sh_package_runtime_test_build_identity("game A\nproduct B\n");
    memo_refresh(data, 0, 1);  /* another product image: judged afresh */
    sh_package_runtime_test_build_identity("game B\nproduct B\n");
    memo_refresh(data, 0, 1);  /* another game image: judged afresh */
    memo_refresh(data, 1, 1);
    /* The other renderer's executable keeps its own memo: judged afresh there
     * and remembered, and back on this one the package is still left out
     * without compiling it. */
    sh_package_runtime_test_image_slot("gl");
    sh_package_runtime_test_build_identity("game G\nproduct B\n");
    memo_refresh(data, 0, 1);
    memo_refresh(data, 1, 1);
    sh_package_runtime_test_image_slot("vk");
    sh_package_runtime_test_build_identity("game B\nproduct B\n");
    memo_refresh(data, 1, 1);
    /* The unslotted memo of an earlier build is removed. */
    {
        char legacy[MAX_PATH]; FILE *file = NULL;
        snprintf(legacy, sizeof(legacy), "%s/package-cache/own-bytes-exclusions.txt", data);
        CHECK(!fopen_s(&file, legacy, "wb") && file); if (file) fclose(file);
        memo_refresh(data, 1, 1);
        CHECK(GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES);
    }
    memo_edit(data, 0);
    memo_refresh(data, 0, 1);  /* corrupt memo: judged afresh, rewritten */
    memo_refresh(data, 1, 1);
    memo_edit(data, 1);
    memo_refresh(data, 0, 1);  /* truncated memo: judged afresh */
    /* A repaired tree is judged afresh and served; its old entry never applies. */
    create("memo-data/overrides/broken/assets/generated/decls/entitydef/memo_broken.decl",
        "{ class = \"Entity\"; edit = { health = 4; } }");
    memo_refresh(data, 0, 0);
    expect_current("generated/decls/entitydef/memo_broken.decl", "health = 4");
    memo_refresh(data, 0, 0);
    sh_package_runtime_test_build_identity("game test\nproduct test\n");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
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
    first = sh_package_runtime_selectors_retain("entitydef", error, sizeof(error));
    second = sh_package_runtime_selectors_retain("entitydef", error, sizeof(error));
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
static void test_selector_unrelated_gameplay(void)
{
    char data[MAX_PATH], map_root[MAX_PATH], error[2048];
    const char *palette = "generated/decls/snapeditorentitydef/palette/tool.decl";
    sh_package_selector_hold *hold = NULL;
    sh_package_map_plan *plan = NULL;
    create("selector-scope-data/overrides/current/package.json", "{\"id\":\"selector-scope\",\"name\":\"Palette\"}");
    create("selector-scope-data/overrides/current/assets/generated/decls/snapeditorentitydef/palette/tool.decl",
        "{ edit = { displayName = \"Tool\"; } }");
    create("selector-scope-data/overrides/current/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 10; } }");
    create("selector-scope-map/overrides/authored/package.json", "{\"id\":\"authored\",\"name\":\"Authored\"}");
    create("selector-scope-map/overrides/authored/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 30; } }");
    snprintf(data, sizeof(data), "%s/selector-scope-data", root);
    snprintf(map_root, sizeof(map_root), "%s/selector-scope-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    expect_current(health_path, "health = 10"); expect_current(palette, "Tool");
    hold = sh_package_runtime_selectors_retain("snapeditorentitydef", error, sizeof(error)); CHECK(hold);
    plan = sh_package_runtime_prepare_map(data, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) fprintf(stderr, "selector map preparation: %s\n", error);
    /* A palette borrows its templates and their dependency closure. A shared
     * gameplay resource elsewhere in that provider is not a palette borrow. */
    CHECK(plan && sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, NULL, NULL));
    expect_current(health_path, "health = 30"); expect_current(palette, "Tool");
    expect_library("health = 10");
    sh_package_runtime_selectors_release(hold); hold = NULL;
    sh_package_map_plan_free(plan); sh_package_runtime_test_dispose(); cleanup_resources(data);
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
static int local_document_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/entitydef/legacy/native.decl")) return 0;
    *body = (unsigned char *)_strdup("{ class = \"Entity\"; edit = { health = 10; } }");
    if (!*body) return -1;
    *length = strlen((char *)*body); return 1;
}
static void test_local_document_binding(void)
{
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], error[2048]; size_t length, next_length;
    snprintf(data, sizeof(data), "%s/local-document", root);
    snprintf(map, sizeof(map), "%s/local-document-map", root);
    create("local-document/overrides/local/package.json", "{\"id\":\"local\",\"name\":\"Local\"}");
    create("local-document/overrides/local/assets/generated/decls/entitydef/legacy/custom.decl",
        "{ class = \"Entity\"; edit = { health = 5; } }");
    create("local-document/overrides/local/assets/generated/decls/entitydef/legacy/native.decl",
        "{ class = \"Entity\"; edit = { health = 20; } }");
    create("local-document-map/overrides/map/package.json", "{\"id\":\"map\",\"name\":\"Map\"}");
    create("local-document-map/overrides/map/assets/generated/decls/entitydef/legacy/custom.decl",
        "{ class = \"Entity\"; edit = { health = 42; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(local_document_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    const char *json = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"legacy/custom\"}},{\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"legacy/native\"}}],\"note\":\"legacy/custom\x97\"}";
    char *out = sh_package_runtime_rebind_local_document(json, strlen(json), &length, error, sizeof(error));
    if (!out) fprintf(stderr, "local document: %s\n", error);
    CHECK(out && strstr(out, "\"inherit\":\"smp/") && strstr(out, "\"inherit\":\"legacy/native\"") &&
        strstr(out, "\"note\":\"legacy/custom\x97\"") && !strstr(out, "smpbind."));
    sh_package_map_plan *plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
    CHECK(plan); int fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    char *again = sh_package_runtime_rebind_local_document(json, strlen(json), &next_length, error, sizeof(error));
    CHECK(out && again && length == next_length && !memcmp(out, again, length + 1));
    HeapFree(GetProcessHeap(), 0, out); HeapFree(GetProcessHeap(), 0, again);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(map);
}
/* A package left out as not served yet is judged again on every compile: its
 * decision also reads stock coverage, which is not part of its own tree. When
 * that changes (here the campaign-only dependency becomes a SnapMap original),
 * the unchanged tree is served. Nothing about it is remembered. */
static int served_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (strcmp(path, health_path)) return baseline(context, path, body, length);
    *body = (unsigned char *)_strdup("{ class = \"Entity\"; edit = { health = 25000; } }");
    *length = *body ? strlen((const char *)*body) : 0;
    return *body ? 1 : -1;
}
static void test_unserved_rejudged(void)
{
    char data[MAX_PATH], *summary = NULL;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/unserved-again", root);
    create("unserved-again/overrides/gap/package.json", "{\"id\":\"gap\",\"name\":\"Gap\"}");
    create("unserved-again/overrides/gap/assets/generated/decls/entitydef/admission/gap.decl",
        "{ class = \"Entity\"; edit = { target = \"ai/cyberdemon\"; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    left_out_reuses = 0;
    for (int pass = 0; pass < 2; pass++) {
        CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
        summary = sh_package_runtime_summary();
        CHECK(summary && strstr(summary, "is not served yet (product coverage)")); free(summary);
    }
    sh_package_runtime_test_baseline(served_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data) && !left_out_reuses);
    summary = sh_package_runtime_summary();
    int served = summary && !strstr(summary, "is not served yet") && strstr(summary, "Installed library: 1 packages");
    if (!served) fprintf(stderr, "rejudged summary: %s\n", summary ? summary : "(none)");
    CHECK(served); free(summary);
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    int gap = 0;
    for (size_t i = 0; library && i < library->resource_count; i++) {
        char error[256]; size_t length = 0;
        unsigned char *body = sh_package_compilation_read(library, library->resources + i, SIZE_MAX,
            &length, error, sizeof(error));
        unsigned char *text = body ? realloc(body, length + 1) : NULL;
        if (!text) { free(body); continue; }
        text[length] = 0;
        gap += strstr((const char *)text, "target = \"ai/cyberdemon\"") != NULL;
        free(text);
    }
    sh_package_runtime_release();
    CHECK(gap == 1);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
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
        strstr(summary, "(via entitydef ai/cyberdemon <- entitydef admission/gap [scope ") &&
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
    if (argc == 2 && !strcmp(argv[1], "--native-inline-library")) {
        test_native_inline_library(); cleanup(); return failures ? 1 : 0;
    }
    run_conflicting_startup();
    test_duplicate_editor_controls();
    test_audio_programs();
    test_local_isolation();
    test_local_document_binding();
    test_unserved_coverage();
    test_unserved_rejudged();
    test_map_usage_admission();
    test_legacy_include_bindings();
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
    test_historical_derived_identities();
    test_historical_derived_recipe();
    test_historical_native_identity_use();
    test_native_inline_save();
    test_native_inline_load();
    run_native_inline_library();
    test_native_inline_placement_provenance();
    test_constructor_provider_lifetimes();
    test_constructor_binding_changes();
    test_native_membership_changes();
    test_retained_occurrence_resources();
    test_retained_bound_dependencies();
    test_retained_registration_meaning(0);
    test_retained_registration_meaning(1);
    test_additive_occurrence_admission();
    test_installed_authoring_admission();
    test_initial_installed_authoring();
    test_local_authoring_defect();
    test_published_install_refresh();
    test_left_out_packages();
    test_left_out_across_processes();
    test_malformed_owners_together();
    test_increment_designed_difference();
    test_selector_shared_retirement();
    test_selector_unrelated_gameplay();
    test_failed_local_compilation_admission();
    cleanup(); if (failures) return 1;
    puts("package runtime map context tests passed"); return 0;
}
