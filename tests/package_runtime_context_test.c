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
#define PACKAGE_FIXTURE_ENTRIES 4096u
#include "package_fixture.h"
#include "hook.h"
#include "fsm_web.h"
#include "package_fsm.h"
#include "decl_graph.h"
#include "map_provenance.h"
#include "map_embed.h"
#include "package_spawn_registry.h"
#include "strids.h"
#include "package_movie.h"

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
static int baseline(void *context,const char *path,unsigned char **body,size_t *length);
typedef struct finish_token { int old, restoring; } finish_token;
static struct {
    int enabled, live, serial, accepts, rejects, calls, preflights, finalizations;
    int refuse_once, preflight_refuse, cleanup_fault, activation_fault;
    finish_token *active, *damaged;
} finish_probe;
static int probe_finish(void *context,int accepted,int *committed,char *error,size_t capacity)
{
    finish_token *token=context; CHECK(token==finish_probe.active); *committed=0;
    if (accepted==2) {
        finish_probe.preflights++;
        if (finish_probe.preflight_refuse) { finish_probe.preflight_refuse=0; return 0; }
        return 1;
    }
    finish_probe.calls++;
    if (finish_probe.refuse_once) {
        finish_probe.refuse_once=0; snprintf(error,capacity,"fixture native admission owed"); return 0;
    }
    if (accepted) { *committed=1; finish_probe.accepts++; }
    else { finish_probe.live=token->old; finish_probe.rejects++; }
    finish_probe.active=NULL;
    if (accepted && finish_probe.cleanup_fault) {
        finish_probe.damaged=token;
        if (finish_probe.cleanup_fault==2) RaiseException(0xe0420001,0,0,NULL);
        snprintf(error,capacity,"fixture accepted native cleanup damaged"); return -1;
    }
    free(token); return 1;
}
static void probe_begin(int restoring)
{
    if (!finish_probe.enabled) return;
    CHECK(!finish_probe.active); /* Reject must finish before consumer recovery. */
    finish_token *token=calloc(1,sizeof(*token)); CHECK(token); if (!token) return;
    token->old=finish_probe.live; token->restoring=restoring;
    finish_probe.active=token; finish_probe.live=++finish_probe.serial;
    CHECK(sh_package_runtime_defer_activation_finish(probe_finish,token));
    CHECK(!sh_package_runtime_defer_activation_finish(probe_finish,token)); /* Duplicate. */
}
static int probe_activate(void *context,int restoring,const sh_package_changes *changes,char *error,size_t capacity)
{
    (void)context; (void)changes; probe_begin(restoring);
    if (!restoring && finish_probe.activation_fault==2) RaiseException(0xe0420002,0,0,NULL);
    if (!restoring && finish_probe.activation_fault) { snprintf(error,capacity,"fixture native parse failed"); return 0; }
    return 1;
}
static int probe_fsm_installed(void) { return 1; }
static int probe_fsm_issue(const char *const *names,size_t count) { (void)names; (void)count; return 1; }
static int probe_fsm_fault;
static int probe_fsm_publish(const struct sh_fsm_web_table *table)
{
    (void)table; CHECK(finish_probe.active);
    if (probe_fsm_fault) {
        int fault=probe_fsm_fault; probe_fsm_fault=0;
        if (fault==2) RaiseException(0xe0420003,0,0,NULL);
        return 0;
    }
    return 1;
}
static void test_deferred_activation_finish(void)
{
    char map[MAX_PATH],error[2048]; snprintf(map,sizeof(map),"%s\\deferred-map",root);
    create("deferred-map/overrides/p/package.json","{\"id\":\"deferred\",\"name\":\"Deferred\"}");
    create("deferred-map/overrides/p/assets/generated/decls/entitydef/ai/cyberdemon.decl","{ edit = { health = 77; } }");
    for (int fault=0;fault<2;fault++) {
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline,NULL);
        CHECK(sh_package_runtime_refresh(root)); memset(&finish_probe,0,sizeof(finish_probe)); finish_probe.enabled=1;
        CHECK(!sh_package_runtime_defer_activation_finish(probe_finish,&finish_probe)); /* Stale registration. */
        sh_package_runtime_test_fsm_hook(probe_fsm_installed,probe_fsm_issue,probe_fsm_publish);
        for (int fail=1;fail<=2;fail++) {
            finish_probe.activation_fault=fail;
            CHECK(!sh_package_runtime_activate_map(root,map,(sh_package_activation_guard){0},probe_activate,NULL));
            CHECK(!finish_probe.active && finish_probe.rejects==fail && finish_probe.accepts==fail);
        }
        finish_probe.activation_fault=0;
        for (int fail=1;fail<=2;fail++) {
            probe_fsm_fault=fail; int rejects=finish_probe.rejects;
            CHECK(!sh_package_runtime_activate_map(root,map,(sh_package_activation_guard){0},probe_activate,NULL));
            CHECK(!finish_probe.active && finish_probe.rejects==rejects+1);
        }
        CHECK(sh_package_runtime_activate_map(root,map,(sh_package_activation_guard){0},probe_activate,NULL));
        CHECK(!finish_probe.active && finish_probe.preflights && sh_package_runtime_admission_ready());
        finish_probe.preflight_refuse=1;
        CHECK(!sh_package_runtime_activate_map(root,map,(sh_package_activation_guard){0},probe_activate,NULL));
        CHECK(!finish_probe.active && sh_package_runtime_admission_ready());
        finish_probe.cleanup_fault=fault+1;
        const sh_package_compilation *before=sh_package_runtime_acquire(); sh_package_runtime_release();
        CHECK(!sh_package_runtime_activate_map(root,map,(sh_package_activation_guard){0},probe_activate,NULL));
        const sh_package_compilation *latched=sh_package_runtime_acquire(); CHECK(latched && latched!=before); sh_package_runtime_release();
        CHECK(!finish_probe.active && finish_probe.damaged && !sh_package_runtime_admission_ready());
        int calls=finish_probe.calls;
        CHECK(!sh_package_runtime_activate_map(root,NULL,(sh_package_activation_guard){0},probe_activate,NULL));
        const sh_package_compilation *after=sh_package_runtime_acquire(); CHECK(after==latched); sh_package_runtime_release();
        CHECK(finish_probe.calls==calls); sh_package_runtime_error(error,sizeof(error)); CHECK(strstr(error,"damaged"));
        sh_package_runtime_test_fsm_hook(NULL,NULL,NULL); sh_package_runtime_test_dispose();
        free(finish_probe.damaged); memset(&finish_probe,0,sizeof(finish_probe));
    }
}
static const char *descriptor = "{\"id\":\"installed-bosses\",\"name\":\"Installed Bosses\",\"strings\":{\"en\":{\"label\":\"local\"}}}";
static const char *original = "{ edit = { health = 25000; } }";
/* Library compiles that left an unchanged judged package out without
 * compiling it again (test_left_out_packages). */
static unsigned left_out_reuses;
/* Whole installed-library compiles, and refreshes that folded a published
 * installation in as an increment (test_published_install_refresh). */
static unsigned library_compiles, install_refreshes;
/* Watched refreshes served as an exact increment, and source units every
 * compile discovered, replayed or not (test_watched_increment). */
static unsigned watched_increments;
static size_t discovered_units;
/* Source captures that replayed recordings of earlier compiles (the memo). */
static unsigned memo_replays;
/* Packages a library compile left out as remembered by an earlier process. */
static unsigned left_out_packages;
/* "Skipped local package" reports (rejected on their own content). */
static unsigned skipped_packages;
/* Failed captures that stopped at the source admission boundary; reads of a
 * stock dependency that only discovering a healthy package's stock root
 * requests (test_malformed_owners_together), in total and when the last
 * failed installed-library compile was reported; the last published library
 * digest. */
static unsigned admission_stops, closure_reads, closure_reads_failed;
static char published_digest[65];
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
    if (strstr(message, "package library compile (installed library): failed")) closure_reads_failed = closure_reads;
    if (strstr(message, "admission stopped after")) admission_stops++;
    if ((at = strstr(message, "published digest ")) && strlen(at + 17) < sizeof(published_digest))
        strcpy_s(published_digest, sizeof(published_digest), at + 17);
    if (strstr(message, "package library compile (installed library)")) library_compiles++;
    if (strstr(message, "refresh for a published installation: added packages")) install_refreshes++;
    if (strstr(message, "refresh for watched packages: added packages")) watched_increments++;
    {
        size_t units = 0;
        if ((at = strstr(message, "repeated work: source link ")) &&
            sscanf(at, "repeated work: source link %zu units discovered", &units) == 1) discovered_units += units;
    }
    if (getenv("SH_CONTEXT_TEST_LOG")) fprintf(stderr, "log: %s\n", message);
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
/* Capture one occurrence document from the published view, as the editor's
 * native inspection does (package_editor.c pe_native_inspect passes no plan).
 * With a plan the saved text is first rebound to its native names; keep, when
 * asked, those native names for a later copy of the same entity. */
static sh_package_occurrence_snapshot *chain_capture(sh_package_map_plan *plan, const char *json, char **native)
{
    char error[2048];
    size_t rebound_length = 0, captured_length = 0;
    sh_package_occurrence_snapshot *document = NULL;
    char *rebound = NULL;
    if (plan) {
        rebound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &rebound_length, error, sizeof(error));
        if (!rebound) { fprintf(stderr, "audio chain rebind: %s\n", error); return NULL; }
    }
    const char *text = rebound ? rebound : json;
    size_t length = rebound ? rebound_length : strlen(json);
    char *captured = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, text, length,
        &captured_length, &document, NULL, NULL, error, sizeof(error));
    if (!captured) fprintf(stderr, "audio chain capture: %s\n", error);
    if (native && rebound && (*native = malloc(rebound_length + 1))) {
        memcpy(*native, rebound, rebound_length); (*native)[rebound_length] = 0;
    }
    free(captured); if (rebound) HeapFree(GetProcessHeap(), 0, rebound);
    return document;
}
/* The published programs; each listed owner is released, the programs kept. */
static size_t chain_programs(sh_package_audio_program **out, size_t capacity)
{
    sh_package_runtime_audio *rows = NULL; size_t count = 0; char error[2048];
    if (!sh_package_runtime_audio_programs(&rows, &count, error, sizeof(error))) { fprintf(stderr, "audio chain: %s\n", error); return SIZE_MAX; }
    for (size_t i = 0; i < count; i++) {
        if (i < capacity) out[i] = rows[i].program;
        sh_package_runtime_audio_release(rows[i].owner);
    }
    free(rows); return count;
}
/* A retained occurrence keeps only its own map's root and program. Map A's
 * copied occurrence outlives A while map B is opened, copied from and left;
 * once A's occurrence is released, A's program leaves the published set and A
 * itself is released, although B's retained view was built while A was still
 * held. A second copy of A's entity taken while B is published (A-B-A reuse)
 * keeps A until it too is released, and B's copy keeps only B. */
static void test_audio_program_chain(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    char data[MAX_PATH], map_a[MAX_PATH], map_b[MAX_PATH], base[MAX_PATH], error[2048], *native_a = NULL;
    sh_package_audio_program *listed[8] = {0}, *program_a = NULL, *program_b = NULL;
    sh_package_occurrence_snapshot *document_a = NULL, *document_b = NULL, *document_a2 = NULL;
    sh_package_map_plan *plan = NULL; void *owner_b = NULL; int fail = 0; size_t count, roots;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/chain-library", root);
    snprintf(map_a, sizeof(map_a), "%s/chain-map-a", root);
    snprintf(map_b, sizeof(map_b), "%s/chain-map-b", root);
    snprintf(base, sizeof(base), "%s/chain-base", root);
    create("chain-base/sound", NULL);
    create("chain-library/overrides", NULL);
    create("chain-map-a/overrides/sound-a/package.json", "{\"id\":\"sound-a\",\"name\":\"Sound A\"}");
    create("chain-map-a/overrides/sound-a/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("chain-map-a/overrides/sound-a/assets/sound/soundbanks/pc/chainbank_a.bnk", "chainbank_a.bnk", 77);
    create("chain-map-b/overrides/sound-b/package.json", "{\"id\":\"sound-b\",\"name\":\"Sound B\"}");
    create("chain-map-b/overrides/sound-b/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 9; } }");
    write_bank("chain-map-b/overrides/sound-b/assets/sound/soundbanks/pc/chainbank_b.bnk", "chainbank_b.bnk", 78);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(chain_programs(listed, 8) == 0);
    roots = sh_package_runtime_test_audio_roots();
    /* Map A, a copy taken from it, then A is left. */
    plan = sh_package_runtime_prepare_map(data, map_a, error, sizeof(error));
    if (!plan) fprintf(stderr, "audio chain map A: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1); program_a = listed[0];
    document_a = chain_capture(plan, json, &native_a); CHECK(document_a && native_a);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Map B while A's copy is still held, a copy from B, then B is left. */
    plan = sh_package_runtime_prepare_map(data, map_b, error, sizeof(error));
    if (!plan) fprintf(stderr, "audio chain map B: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    CHECK(count == 2);
    for (size_t i = 0; i < count && i < 8; i++) if (listed[i] != program_a) program_b = listed[i];
    CHECK(program_a && program_b && program_b != program_a);
    {
        sh_package_runtime_audio *rows = NULL; size_t n = 0;
        if (sh_package_runtime_audio_programs(&rows, &n, error, sizeof(error)))
            for (size_t i = 0; i < n; i++) {
                if (rows[i].program == program_b && !owner_b) owner_b = rows[i].owner;
                else sh_package_runtime_audio_release(rows[i].owner);
            }
        free(rows);
    }
    document_b = chain_capture(plan, json, NULL); CHECK(document_b);
    /* A second copy of A's entity, taken from the published view while B is open. */
    if (native_a) { document_a2 = chain_capture(NULL, native_a, NULL); CHECK(document_a2); }
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Releasing A's first copy keeps A's program for its second copy. */
    sh_package_occurrence_release(document_a); document_a = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    if (count != 2) fprintf(stderr, "audio chain: %zu programs while A's second copy is held\n", count);
    CHECK(count == 2 && ((listed[0] == program_a && listed[1] == program_b) || (listed[0] == program_b && listed[1] == program_a)));
    /* Releasing A's last copy retires A's program; B's copy keeps only B's. */
    sh_package_occurrence_release(document_a2); document_a2 = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    if (count != 1) fprintf(stderr, "audio chain: %zu programs after releasing map A's copies\n", count);
    CHECK(count == 1 && listed[0] == program_b);
    count = sh_package_runtime_test_audio_roots() - roots;
    if (count != 1) fprintf(stderr, "audio chain: %zu audio roots allocated after releasing map A's copies\n", count);
    CHECK(count == 1);
    sh_package_occurrence_release(document_b); document_b = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 0);
    if (owner_b) sh_package_runtime_audio_release(owner_b);
    owner_b = NULL;
    CHECK(sh_package_runtime_test_audio_roots() == roots);
done:
    if (owner_b) sh_package_runtime_audio_release(owner_b);
    sh_package_occurrence_release(document_a); sh_package_occurrence_release(document_b);
    sh_package_occurrence_release(document_a2); free(native_a);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* Every installed file and folder has one disposition from the served
 * library's records (sh_packages inputs). Exact copies are shared, never
 * double counted; a row equal to its SnapMap original stays native and its
 * input is still accounted after pruning; a nested component's descriptor is
 * named; author data and empty folders stay with their package; a malformed
 * unit keeps its scanned files accountable; and an authored file the library
 * neither serves nor records is unaccounted, not dismissed as ancillary. */
static int input_line(const char *text, const char *first, const char *second)
{
    for (const char *line = text; line && *line; ) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        char copy[2048];
        if (length < sizeof(copy)) {
            memcpy(copy, line, length); copy[length] = 0;
            if (strstr(copy, first) && (!second || strstr(copy, second))) return 1;
        }
        line = end ? end + 1 : NULL;
    }
    return 0;
}
static void lease_registration_setup(admission_fixture *f);
static int lease_registration_baseline(void *context, const char *path, unsigned char **body, size_t *length);
/* lease_registration_baseline plus two SnapMap originals: an opaque image and
 * a declaration an exact authored copy leaves native (pruned). */
static const char g_inputs_stock[] = "{ class = \"Entity\"; edit = { health = 9; } }";
static int inputs_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "stock.bimage") ? "original" :
        !strcmp(path, "generated/decls/entitydef/stockthing.decl") ? g_inputs_stock : NULL;
    if (!text) return lease_registration_baseline(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text);
    return *body ? 1 : -1;
}
static void inputs_row_provenance(const char *path, size_t *sources, size_t *owners)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *row = library ? sh_package_compilation_find(library, path) : NULL;
    *sources = row ? row->source_count : SIZE_MAX; *owners = row ? sh_package_owners_count(&row->owners) : SIZE_MAX;
    sh_package_runtime_release();
}
/* A generated row names input paths, not files, and its owners span all its
 * inputs. A file of a package the row does not name, or one another named
 * package contradicts with different bytes, is never attributed; the rest is
 * reported as unproved attribution, not as proof. */
static void test_input_generated_attribution(void)
{
    sh_package served = {0};
    sh_package_source_file files[3] = {0};
    sh_package packages[3];
    sh_package_sources sources = {0};
    sh_compiled_resource row = {0};
    char *inputs[1] = {"generated/decls/entitydef/input.decl"};
    for (size_t i = 0; i < 3; i++) {
        packages[i] = served;
        files[i].owner = i; files[i].relative = "assets/generated/decls/entitydef/input.decl";
        files[i].engine_path = "generated/decls/entitydef/input.decl"; files[i].length = 4;
        files[i].digest[0] = i == 2 ? 2 : 1;
    }
    sources.packages = packages; sources.package_count = 3; sources.files = files; sources.file_count = 3;
    row.engine_path = "generated/decls/entitydef/output.decl"; row.generated = 1;
    row.generated_inputs = inputs; row.generated_input_count = 1;
    CHECK(sh_package_owners_add(&row.owners, 0));
    CHECK(sh_package_runtime_test_generated_attributable(&sources, &row, 0));
    CHECK(!sh_package_runtime_test_generated_attributable(&sources, &row, 2)); /* same path, unnamed owner */
    CHECK(sh_package_owners_add(&row.owners, 1));
    CHECK(sh_package_runtime_test_generated_attributable(&sources, &row, 1)); /* equal bytes: either supplied them */
    CHECK(sh_package_owners_add(&row.owners, 2));
    CHECK(!sh_package_runtime_test_generated_attributable(&sources, &row, 0)); /* different bytes: unprovable */
    CHECK(!sh_package_runtime_test_generated_attributable(&sources, &row, 2));
    sh_package_owners_free(&row.owners);
}
static void test_input_accounting(void)
{
    static const char conductor[] = "{ class = \"Entity\"; edit = { aiTypeList = { num = 0; } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 2; } } } }";
    char data[MAX_PATH], *text;
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/inputs", root);
    create("inputs/overrides/alpha/package.json", "{\"id\":\"alpha\",\"name\":\"Alpha\"}");
    create("inputs/overrides/alpha/assets/generated/decls/entitydef/shared.decl", "{ class = \"Entity\"; edit = { health = 40; } }");
    create("inputs/overrides/alpha/assets/generated/decls/entitydef/alpha.decl", "{ class = \"Entity\"; edit = { health = 41; } }");
    create("inputs/overrides/alpha/assets/generated/decls/entitydef/conductor.decl", conductor);
    create("inputs/overrides/alpha/assets/stock.bimage", "original");
    create("inputs/overrides/alpha/assets/generated/decls/entitydef/stockthing.decl", g_inputs_stock);
    create("inputs/overrides/alpha/notes.txt", "author notes");
    create("inputs/overrides/alpha/empty", NULL);
    create("inputs/overrides/alpha/inner/package.json", "{\"id\":\"alpha.inner\",\"name\":\"Inner\"}");
    create("inputs/overrides/alpha/inner/assets/generated/decls/entitydef/inner.decl", "{ class = \"Entity\"; edit = { health = 42; } }");
    create("inputs/overrides/beta/package.json", "{\"id\":\"beta\",\"name\":\"Beta\"}");
    create("inputs/overrides/beta/assets/generated/decls/entitydef/shared.decl", "{ class = \"Entity\"; edit = { health = 40; } }");
    create("inputs/overrides/broken/package.json", "{\"id\":\"broken\",\"name\":\"Broken\"}");
    create("inputs/overrides/broken/assets/generated/decls/snapeditorentitydef/bad.decl", "{} }");
    create("inputs/overrides/broken/assets/kept.bimage", "kept");
    create("inputs/overrides/later/package.json", "{\"id\":\"later\",\"name\":\"Later\"}");
    create("inputs/overrides/later/assets/stock.bin", "original");
    create("inputs/overrides/old/package.json", "{\"name\":\"Old\"}");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inputs_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    text = sh_package_runtime_inputs(NULL, 1);
    fprintf(stderr, "inputs overview:\n%s\n", text ? text : "(null)");
    CHECK(text && input_line(text, "Alpha [alpha] (alpha): served library", "2 components"));
    CHECK(text && input_line(text, "Broken [broken] (broken): malformed unit", "3 files"));
    CHECK(text && input_line(text, "Unit rejected before its files were inventoried", "missing its required id"));
    CHECK(text && input_line(text, "Later [later] (later): not served yet", NULL) &&
        input_line(text, "is not served yet (product coverage)", "stock.bin"));
    CHECK(text && !strstr(text, "unaccounted"));
    free(text);
    text = sh_package_runtime_inputs("alpha", 1);
    fprintf(stderr, "inputs alpha:\n%s\n", text ? text : "(null)");
    CHECK(text && input_line(text, "entitydef/shared.decl", "served:"));
    CHECK(text && input_line(text, "entitydef/alpha.decl", "served:"));
    CHECK(text && input_line(text, "entitydef/conductor.decl", "served:"));
    CHECK(text && input_line(text, "assets/stock.bimage", "served:"));
    CHECK(text && input_line(text, "entitydef/stockthing.decl", "unchanged original: equals the installed SnapMap original"));
    CHECK(text && input_line(text, "notes.txt", "author data: outside assets/"));
    CHECK(text && input_line(text, "empty/", "empty authored folder"));
    CHECK(text && input_line(text, "inner/package.json", "descriptor: nested component Inner [alpha.inner]"));
    CHECK(text && input_line(text, "entitydef/inner.decl", "served:"));
    CHECK(text && input_line(text, "Rows 1-", NULL));
    free(text);
    text = sh_package_runtime_inputs("beta", 1);
    CHECK(text && input_line(text, "entitydef/shared.decl", "shared copy: same bytes as Alpha [alpha] assets/generated/decls/entitydef/shared.decl"));
    free(text);
    text = sh_package_runtime_inputs("broken", 1);
    CHECK(text && input_line(text, "assets/kept.bimage", "malformed unit: with its whole unit"));
    CHECK(text && input_line(text, "Skipped local package", "malformed declaration"));
    free(text);
    /* A path query finds the copy in every package, each with its own role. */
    text = sh_package_runtime_inputs("assets/generated/decls/entitydef/shared.decl", 1);
    CHECK(text && input_line(text, "alpha/assets/generated/decls/entitydef/shared.decl", "served:") &&
        input_line(text, "beta/assets/generated/decls/entitydef/shared.decl", "shared copy:") && input_line(text, "Rows 1-2 of 2", NULL));
    free(text);
    text = sh_package_runtime_summary();
    CHECK(text && strstr(text, "Inputs: ") && strstr(text, "migrate-overrides") && strstr(text, "names what to fix"));
    free(text);
    /* Paging is deterministic and bounded. */
    text = sh_package_runtime_inputs("alpha", 3);
    CHECK(text && input_line(text, "  3. ", NULL) && !input_line(text, "  2. ", NULL));
    free(text);
    /* An installation serves its added package through the retained view. The
     * view keeps both sides' unchanged-original records; every added input
     * must still resolve, as a whole compile of the same inventory does. */
    create("inputs/overrides/gamma/package.json", "{\"id\":\"gamma\",\"name\":\"Gamma\"}");
    create("inputs/overrides/gamma/assets/generated/decls/entitydef/gamma.decl", "{ class = \"Entity\"; edit = { health = 43; } }");
    create("inputs/overrides/gamma/assets/generated/decls/entitydef/shared.decl", "{ class = \"Entity\"; edit = { health = 40; } }");
    create("inputs/overrides/gamma/assets/generated/decls/entitydef/conductor.decl", conductor);
    create("inputs/overrides/gamma/assets/stock.bimage", "original");
    create("inputs/overrides/gamma/assets/generated/decls/entitydef/stockthing.decl", g_inputs_stock);
    /* A second added exact copy joins the same served row. */
    create("inputs/overrides/delta/package.json", "{\"id\":\"delta\",\"name\":\"Delta\"}");
    create("inputs/overrides/delta/assets/generated/decls/entitydef/conductor.decl", conductor);
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data));
    fprintf(stderr, "inputs view: %u install refresh(es), %u library compile(s)\n", install_refreshes, library_compiles);
    text = sh_package_runtime_inputs("gamma", 1);
    CHECK(install_refreshes == 1 && !library_compiles);
    fprintf(stderr, "inputs gamma (view):\n%s\n", text ? text : "(null)");
    CHECK(text && input_line(text, "entitydef/gamma.decl", "served:"));
    CHECK(text && input_line(text, "entitydef/shared.decl", "shared copy:"));
    CHECK(text && input_line(text, "entitydef/stockthing.decl", "unchanged original:"));
    /* Added exact copies equal to their originals: the view joins their files
     * to the rows it already serves, as the whole compile lists them. */
    CHECK(text && input_line(text, "entitydef/conductor.decl", "shared copy:"));
    CHECK(text && input_line(text, "assets/stock.bimage", "shared copy:"));
    CHECK(text && !strstr(text, "unaccounted"));
    free(text);
    size_t view_sources[2], view_owners[2], whole_sources[2], whole_owners[2];
    inputs_row_provenance("generated/decls/entitydef/conductor.decl", &view_sources[0], &view_owners[0]);
    inputs_row_provenance("stock.bimage", &view_sources[1], &view_owners[1]);
    CHECK(view_sources[0] == 3 && view_sources[1] == 2);
    /* Every exposure of one entry is listed, one line per row and identity. */
    text = sh_package_runtime_input_exposures("gamma", 5, 1);
    CHECK(text && input_line(text, "entitydef/conductor.decl", "shared copy:") &&
        input_line(text, "row generated/decls/entitydef/conductor.decl", NULL) &&
        input_line(text, "identity entitydef conductor", NULL) && input_line(text, "Exposure lines 1-2 of 2", NULL));
    free(text);
    text = sh_package_runtime_input_exposures("gamma", 99, 1);
    CHECK(text && strstr(text, "No entry 99 matches gamma"));
    free(text);
    library_compiles = install_refreshes = 0;
    CHECK(sh_package_runtime_refresh(data));
    text = sh_package_runtime_inputs("gamma", 1);
    fprintf(stderr, "inputs gamma (whole, %u compile(s)):\n%s\n", library_compiles, text ? text : "(null)");
    CHECK(text && input_line(text, "entitydef/gamma.decl", "served:"));
    CHECK(text && input_line(text, "entitydef/shared.decl", "shared copy:"));
    CHECK(text && input_line(text, "entitydef/conductor.decl", "shared copy:"));
    CHECK(text && input_line(text, "assets/stock.bimage", "shared copy:"));
    CHECK(text && input_line(text, "entitydef/stockthing.decl", "unchanged original:"));
    free(text);
    inputs_row_provenance("generated/decls/entitydef/conductor.decl", &whole_sources[0], &whole_owners[0]);
    inputs_row_provenance("stock.bimage", &whole_sources[1], &whole_owners[1]);
    CHECK(whole_sources[0] == view_sources[0] && whole_owners[0] == view_owners[0]);
    CHECK(whole_sources[1] == view_sources[1] && whole_owners[1] == view_owners[1]);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    cleanup_resources(data);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    sh_package_runtime_test_baseline(baseline, NULL);
}
/* A renderer binary: the native header magic and the checksum of its declaration. */
static void shader_binary(const char *relative, const char *declaration)
{
    char path[4096];
    unsigned char bytes[12] = {0xb6, 0x88, 0x00, 0xad, 0, 0, 0, 0, 'b', 'o', 'd', 'y'};
    uint32_t checksum = sh_package_shader_declaration_checksum((const unsigned char *)declaration, strlen(declaration));
    FILE *file = NULL;
    create(relative, "placeholder");
    bytes[4] = (unsigned char)(checksum >> 24); bytes[5] = (unsigned char)(checksum >> 16);
    bytes[6] = (unsigned char)(checksum >> 8); bytes[7] = (unsigned char)checksum;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    CHECK(!fopen_s(&file, path, "wb") && file);
    if (file) { CHECK(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes)); fclose(file); }
}
/* The source link's own reason accounts each shader input it kept without
 * publishing, on either renderer contract; only that package's exact file is
 * explained. The same declaration without its verified binary is never
 * explained by its path, records survive an installation's view, and records
 * that could not be kept leave the library served and the inputs unaccounted. */
static void test_input_shader_accounting(sh_package_shader_renderer renderer)
{
    static const char declaration[] = "renderProg fixture_1 { }\n";
    int vulkan = renderer == SH_PACKAGE_SHADER_VULKAN;
    const char *set = vulkan ? "shaders-vk" : "shaders-gl";
    const char *binary = vulkan ? "_pc.bin" : "_pc_vulkan.bin";
    char data[MAX_PATH], path[512], file[256], *text;
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
#define SHADER_FILE(package, name) (snprintf(path, sizeof(path), "%s/overrides/%s/%s", set, package, name), path)
    create(SHADER_FILE("paired", "package.json"), "{\"id\":\"paired\",\"name\":\"Paired\"}");
    create(SHADER_FILE("paired", "assets/generated/decls/entitydef/paired.decl"), "{ class = \"Entity\"; edit = { health = 44; } }");
    create(SHADER_FILE("paired", "assets/generated/renderprogs/permutations/fixture_1.decl"), declaration);
    snprintf(file, sizeof(file), "assets/generated/renderprogs/permutations/fixture_1%s", binary);
    shader_binary(SHADER_FILE("paired", file), declaration);
    if (!vulkan) create(SHADER_FILE("paired", "assets/generated/spirv/fixture.vspv"), "spirv");
    /* Equal declaration bytes at the same path, but no binary to pair with. */
    create(SHADER_FILE("unpaired", "package.json"), "{\"id\":\"unpaired\",\"name\":\"Unpaired\"}");
    create(SHADER_FILE("unpaired", "assets/generated/renderprogs/permutations/fixture_1.decl"), declaration);
    snprintf(data, sizeof(data), "%s/%s", root, set);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inputs_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1); sh_package_runtime_test_shader_renderer((int)renderer);
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    text = sh_package_runtime_inputs(NULL, 1);
    fprintf(stderr, "shader inputs overview (%s):\n%s\n", set, text ? text : "(null)");
    CHECK(text && input_line(text, "Paired [paired] (paired): served library", vulkan ? "inactive shader 2" : "inactive shader 3"));
    CHECK(text && input_line(text, "Unpaired [unpaired]", NULL) && !input_line(text, "Unpaired [unpaired]", "served library"));
    CHECK(text && !strstr(text, "unaccounted") && !strstr(text, "not retained"));
    free(text);
    text = sh_package_runtime_inputs("paired", 1);
    fprintf(stderr, "shader inputs paired (%s):\n%s\n", set, text ? text : "(null)");
    CHECK(text && input_line(text, "entitydef/paired.decl", "served:"));
    CHECK(text && input_line(text, "permutations/fixture_1.decl", vulkan ?
        "inactive shader: dormant OpenGL permutation declaration: checksum" :
        "inactive shader: dormant Vulkan permutation declaration: checksum"));
    snprintf(file, sizeof(file), "matches assets/generated/renderprogs/permutations/fixture_1%s | 12 bytes", binary);
    CHECK(text && input_line(text, "permutations/fixture_1.decl", file));
    CHECK(text && input_line(text, vulkan ? "fixture_1_pc.bin" : "fixture_1_pc_vulkan.bin", vulkan ?
        "inactive shader: OpenGL renderer artifact; the Vulkan library publishes only" :
        "inactive shader: Vulkan renderer artifact; the OpenGL library publishes only"));
    if (!vulkan) CHECK(text && input_line(text, "generated/spirv/fixture.vspv", "inactive shader: Vulkan renderer artifact"));
    free(text);
    text = sh_package_runtime_inputs("unpaired", 1);
    fprintf(stderr, "shader inputs unpaired (%s):\n%s\n", set, text ? text : "(null)");
    CHECK(text && input_line(text, "permutations/fixture_1.decl", NULL) && !strstr(text, "inactive shader"));
    free(text);
    /* An installation's view keeps base's records and adds its own. */
    create(SHADER_FILE("added", "package.json"), "{\"id\":\"added\",\"name\":\"Added\"}");
    create(SHADER_FILE("added", "assets/generated/decls/entitydef/added.decl"), "{ class = \"Entity\"; edit = { health = 45; } }");
    snprintf(file, sizeof(file), "assets/generated/renderprogs/permutations/added_1%s", binary);
    create(SHADER_FILE("added", file), "other renderer bytes");
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data));
    fprintf(stderr, "shader view: %u install refresh(es), %u library compile(s)\n", install_refreshes, library_compiles);
    CHECK(install_refreshes == 1 && !library_compiles);
    text = sh_package_runtime_inputs("added", 1);
    fprintf(stderr, "shader inputs added (view):\n%s\n", text ? text : "(null)");
    CHECK(text && input_line(text, "entitydef/added.decl", "served:"));
    CHECK(text && input_line(text, "permutations/added_1", "inactive shader:") && !strstr(text, "unaccounted"));
    free(text);
    text = sh_package_runtime_inputs("paired", 1);
    CHECK(text && input_line(text, "permutations/fixture_1.decl", "inactive shader: dormant") && !strstr(text, "unaccounted"));
    free(text);
    /* Records that cannot be kept never refuse the library. */
    create(SHADER_FILE("late", "package.json"), "{\"id\":\"late\",\"name\":\"Late\"}");
    snprintf(file, sizeof(file), "assets/generated/renderprogs/permutations/late_1%s", binary);
    create(SHADER_FILE("late", file), "other renderer bytes");
    sh_package_runtime_test_shader_input_failures(1);
    CHECK(sh_package_runtime_refresh(data)); CHECK(sh_package_runtime_ready());
    sh_package_runtime_test_shader_input_failures(0);
    text = sh_package_runtime_inputs("late", 1);
    fprintf(stderr, "shader inputs late (lost):\n%s\n", text ? text : "(null)");
    CHECK(text && input_line(text, "permutations/late_1", "unaccounted:") &&
        input_line(text, "permutations/late_1", "shader accounting records were not retained"));
    free(text);
    text = sh_package_runtime_inputs(NULL, 1);
    CHECK(text && input_line(text, "Late [late] (late): served library", NULL) &&
        strstr(text, "Shader accounting records were not retained"));
    free(text);
#undef SHADER_FILE
    sh_package_runtime_test_shader_renderer(-1); sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    cleanup_resources(data);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    sh_package_runtime_test_baseline(baseline, NULL);
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
    probe_begin(restoring);
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
    sh_package_consumer_hold *lifetime;
} consumer_activation;
static int consumer_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{
    consumer_activation *state = context; int current = restoring ? state->previous : state->next;
    CHECK(changes && !sh_package_runtime_admission_ready());
    for (int i = 0; i < 2; i++) expect_consumer(state->owner[i],
        i == current || (!i && state->retained) ? state->target[i] : NULL);
    if (state->lifetime && current == 0 && !state->lifetime->provider) {
        CHECK(sh_package_runtime_consumer_retain("entitydef", state->owner[0], state->lifetime, error, capacity) == 1);
        CHECK(state->lifetime->binding && state->lifetime->provider && state->lifetime->lifetime);
    }
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "fixture constructor activation failure"); return 0; }
    return 1;
}
static void test_constructor_provider_lifetimes(int persistent)
{
    const char *owner_path = "generated/decls/entitydef/consumer/root.decl";
    const char *leaf_path = "generated/decls/entitydef/consumer/leaf.decl";
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"consumer/root\"}]}";
    char data[MAX_PATH], map[2][MAX_PATH], error[2048], owners[2][1024], targets[2][1024];
    sh_package_map_plan *plans[2] = {0}; sh_package_consumer_hold held = {0}, lifetime = {0};
    sh_package_occurrence_snapshot *document = NULL;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    consumer_activation state = {{owners[0], owners[1]}, {targets[0], targets[1]}, 0, -1};
    state.retained = persistent; state.lifetime = persistent ? &lifetime : NULL;
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
        snprintf(path, sizeof(path), "consumer-%c/overrides/p/assets/generated/decls/entitydef/consumer/sibling.decl", 'a' + i);
        create(path, "{ class = \"Entity\"; edit = { health = 77; } }");
        snprintf(path, sizeof(path), "consumer-%c/overrides/unrelated/package.json", 'a' + i);
        create(path, "{\"id\":\"unrelated\",\"name\":\"Unrelated source\"}");
        snprintf(path, sizeof(path), "consumer-%c/overrides/unrelated/assets/generated/decls/entitydef/consumer/unrelated.decl", 'a' + i);
        create(path, "{ class = \"Entity\"; edit = { health = 88; } }");
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
    if (persistent) {
        const sh_package_consumer *prior = lifetime.binding;
        CHECK(sh_package_runtime_consumer_retain("entitydef", owners[0], &lifetime, error, sizeof(error)) == -1);
        CHECK(lifetime.binding == prior && lifetime.lifetime);
        sh_package_consumer_hold absent = {0};
        CHECK(sh_package_runtime_consumer_retain("entitydef", "consumer/not-in-catalog", &absent, error, sizeof(error)) == 0);
        CHECK(!absent.provider && !absent.binding && !absent.lifetime);
        for (unsigned allocation = 1; allocation <= 3; allocation++) {
            sh_package_runtime_test_consumer_allocation_fault(allocation);
            CHECK(sh_package_runtime_consumer_retain("entitydef", owners[0], &absent, error, sizeof(error)) == -1);
            CHECK(error[0] && !absent.provider && !absent.binding && !absent.lifetime);
            CHECK(lifetime.binding == prior && lifetime.lifetime);
            expect_consumer(owners[0], targets[0]);
        }
        sh_package_runtime_test_consumer_allocation_fault(0);
    }
    state.next = 1; state.previous = 0; state.fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, consumer_activate, &state));
    CHECK(state.recoveries == 1 && sh_package_runtime_admission_ready());
    expect_consumer(owners[0], targets[0]); expect_consumer(owners[1], NULL);
    state.fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, consumer_activate, &state));
    CHECK(held.binding && !strcmp(sh_package_consumer_find(held.binding, "constructor/target")->name, targets[0]));
    /* Capture a copied A occurrence, then release both plans. Its own lease must
     * retain A's constructor bindings and dependency files beside the B map. */
    if (!persistent) {
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
    }
    state.retained = 1; state.previous = 1;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    for (int i = 0; i < 2; i++) { sh_package_map_plan_free(plans[i]); plans[i] = NULL; }
    state.next = -1;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    expect_consumer(owners[0], targets[0]); expect_consumer(owners[1], NULL);
    const sh_package_compilation *current = sh_package_runtime_acquire(); int retained_leaf = 0;
    for (size_t i = 0; current && i < current->resource_count; i++)
        if (sh_package_resource_native_matches(current->resources + i, "entitydef", targets[0])) retained_leaf = 1;
    CHECK(retained_leaf);
    CHECK(!historical_resource(current, "generated/decls/entitydef/consumer/sibling.decl"));
    CHECK(!historical_resource(current, "generated/decls/entitydef/consumer/unrelated.decl"));
    sh_package_runtime_release();
    sh_package_occurrence_release(document); document = NULL;
    sh_package_runtime_consumer_release(&lifetime); state.lifetime = NULL;
    CHECK(!lifetime.provider && !lifetime.binding && !lifetime.lifetime);
    state.retained = 0; state.previous = -1;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, consumer_activate, &state));
    expect_consumer(owners[0], NULL);
    CHECK(held.binding && !strcmp(sh_package_consumer_find(held.binding, "constructor/target")->name, targets[0]));
done:
    sh_package_occurrence_release(document);
    sh_package_runtime_consumer_release(&lifetime);
    for (int i = 0; i < 2; i++) sh_package_map_plan_free(plans[i]);
    sh_package_runtime_test_dispose(); sh_package_runtime_consumer_release(&held);
    sh_package_runtime_test_constructor_requests(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* Same cooked dictionary and registered readers as test_movie_late_namespace.
 * There are no ActionScript literals: the late namespace, not token scanning,
 * must retain computed sound/material names and private localized keys. */
typedef struct movie_lease_bytes { unsigned char data[512]; size_t used; } movie_lease_bytes;
static void movie_lease_put(movie_lease_bytes *m, const void *p, size_t n)
{ CHECK(n <= sizeof(m->data) - m->used); memcpy(m->data + m->used, p, n); m->used += n; }
static void movie_lease_zero(movie_lease_bytes *m, size_t n)
{ CHECK(n <= sizeof(m->data) - m->used); memset(m->data + m->used, 0, n); m->used += n; }
static void movie_lease_word(movie_lease_bytes *m, unsigned n)
{ unsigned char p[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n}; movie_lease_put(m, p, 4); }
static void movie_lease_text(movie_lease_bytes *m, const char *s)
{ unsigned n = (unsigned)strlen(s); unsigned char p[4] = {(unsigned char)n, (unsigned char)(n >> 8), 0, 0};
  movie_lease_put(m, p, 4); movie_lease_put(m, s, n); }
static void movie_lease_build(movie_lease_bytes *m)
{
    m->used = 0; movie_lease_word(m, 0x42535717); movie_lease_zero(m, 22);
    movie_lease_zero(m, 2);
    for (unsigned i = 0; i < 4; i++) movie_lease_word(m, 0);
    movie_lease_word(m, 3);
    movie_lease_word(m, 1); movie_lease_text(m, "GUI/Paint"); movie_lease_zero(m, 32);
    movie_lease_word(m, 1); movie_lease_text(m, ".atlas"); movie_lease_zero(m, 32);
    movie_lease_word(m, 5); movie_lease_text(m, "Absent"); movie_lease_zero(m, 6); movie_lease_word(m, 0);
}
static int movie_lease_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    movie_lease_bytes *m = context; const char *text = NULL; const unsigned char *bytes = NULL; size_t n = 0;
    static const unsigned char glyphs[14] = {'i','d','f','+',0,12,0,0,0,12,0,12,0,0};
    static const unsigned char atlas[] = "stock atlas pixels";
    *body = NULL; *length = 0;
    if (!strcmp(path, "generated/decls/renderparm/color.decl")) text = "{ Vec 0 }";
    else if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) text = "{ String }";
    else if (!strcmp(path, "generated/swf/stock/panel.bswf")) { bytes = m->data; n = m->used; }
    else if (!strcmp(path, "generated/swf/stock/panel.bimage")) { bytes = atlas; n = sizeof(atlas) - 1; }
    else if (!strcmp(path, "fonts/title/64_df.dat")) { bytes = glyphs; n = sizeof(glyphs); }
    else if (!strcmp(path, "generated/decls/material/fonts/title/64_df.tga.decl")) text = "{ color {1,0,0} }";
    else if (!strcmp(path, "generated/decls/material/gui/paint.decl")) text = "{ color {0,0,1} }";
    else if (!strcmp(path, "generated/decls/material/guisolid.decl")) text = "{ color {1,1,1} }";
    else return 0;
    if (text) { bytes = (const unsigned char *)text; n = strlen(text); }
    *body = malloc(n + 1); CHECK(*body); if (!*body) return -1;
    memcpy(*body, bytes, n); (*body)[n] = 0; *length = n; return 1;
}
static void movie_lease_reflection(admission_fixture *f)
{
    admission_setup(f);
    void *managers = admission_alloc(f, 2 * 8), *manager = admission_alloc(f, 0x90);
    memcpy(managers, *(void **)((char *)f->registry + 8), 8);
    admission_pointer(managers, 8, manager);
    admission_pointer(manager, 8, admission_string(f, "sound"));
    admission_pointer(manager, 0x10, admission_string(f, "idSoundShader"));
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 2);
    void *container = *(void **)f->reflection;
    void *records = admission_alloc(f, 9 * 0x38), *objects = admission_alloc(f, 9 * 16),
        *pointers = admission_alloc(f, 9 * 16), *fields = admission_alloc(f, 5 * 0x48);
    memcpy(records, *(void **)((char *)container + 0x20), 6 * 0x38);
    memcpy(objects, *(void **)((char *)f->reflection + 0x88), 6 * 16);
    memcpy(pointers, *(void **)((char *)f->reflection + 0xa0), 6 * 16);
    /* Replace the old terminator, append both readers, and retain a new one. */
    admission_pointer(records, 5 * 0x38, admission_string(f, "idSoundShader"));
    admission_pointer(records, 5 * 0x38 + 8, admission_string(f, "idDeclTypeInfo"));
    admission_pointer(pointers, 5 * 16 + 8, (void *)0x10);
    admission_pointer(records, 6 * 0x38, admission_string(f, "idSWF"));
    admission_pointer(records, 6 * 0x38 + 8, admission_string(f, ""));
    admission_pointer(pointers, 6 * 16 + 8, (void *)0xa0);
    /* Sound's registered declaration reader must also distinguish non-graph
     * declarations from the native graph base, as setup_sound does. */
    admission_pointer(records, 7 * 0x38, admission_string(f, "idDeclTypeInfoGraph"));
    admission_pointer(records, 7 * 0x38 + 8, admission_string(f, "idDeclTypeInfo"));
    admission_pointer(objects, 7 * 16 + 8, (void *)0x30);
    admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 9);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 9);
    memcpy(fields, *(void **)((char *)records + 0x38 + 0x20), 3 * 0x48);
    admission_pointer(fields, 3 * 0x48, admission_string(f, "idSWF"));
    admission_pointer(fields, 3 * 0x48 + 8, admission_string(f, "*"));
    admission_pointer(fields, 3 * 0x48 + 0x10, admission_string(f, "gui"));
    admission_number(fields, 3 * 0x48 + 0x1c, 8);
    admission_pointer(records, 0x38 + 0x20, fields);
}
static int movie_lease_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{ (void)context; (void)identity; CHECK(key && mapping && !strncmp(key, "#str_smp_loc_", 13)); return 1; }
static int movie_lease_global(void *context, const char *key)
{ (void)context; (void)key; return 1; }
static const sh_package_localized_string *movie_lease_mapping(const sh_package_compilation *compiled, const char *key)
{
    for (size_t i = 0; compiled && key && i < compiled->policy.localized_count; i++)
        if (!strcmp(compiled->policy.localized[i].key, key)) return compiled->policy.localized + i;
    return NULL;
}
static const sh_compiled_resource *movie_lease_named(const sh_package_compilation *compiled, const char *family, const char *name)
{
    for (size_t i = 0; compiled && name && i < compiled->resource_count; i++)
        if (sh_package_resource_native_matches(compiled->resources + i, family, name)) return compiled->resources + i;
    return NULL;
}
typedef struct movie_lease_expected {
    char owner[1024], sound[1024], material[1024], key[1024], empty[1024];
    char sibling_key[1024], sibling_owner[1024], sibling_path[1024], unrelated_path[1024];
} movie_lease_expected;
static int movie_lease_capture(sh_package_map_plan *plan, movie_lease_expected *out, int variant,
    int sibling_published)
{
    const sh_package_compilation *compiled = sh_package_map_plan_compilation(plan);
    sh_package_consumer_hold held = {0}; const sh_package_consumer *movie = NULL;
    /* Current catalog is public; acquire the exact FILE entry after activation. */
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (sh_package_movie_owner("file", r->engine_path) &&
            sh_package_runtime_consumer_acquire("file", r->engine_path, &held) == 1) {
            const char *candidate_key = NULL;
            sh_package_movie_string_route(held.binding, "#str_notice", &candidate_key);
            const sh_package_localized_string *candidate = movie_lease_mapping(compiled, candidate_key);
            if (candidate && strstr(candidate->mapping, variant ? "\"B\"" : "\"A\"")) {
                movie = held.binding; break;
            }
            sh_package_runtime_consumer_release(&held);
        }
    }
    if (!movie) {
        fprintf(stderr, "movie capture: %zu resources, %zu localized mappings\n",
            compiled ? compiled->resource_count : 0, compiled ? compiled->policy.localized_count : 0);
        for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
            const sh_compiled_resource *r = compiled->resources + i;
            fprintf(stderr, "movie resource: %s native %s/%s aliases %zu\n", r->engine_path,
                r->native_type ? r->native_type : "", r->native_name ? r->native_name : "", r->native_alias_count);
        }
        for (size_t i = 0; compiled && i < compiled->policy.localized_count; i++)
            fprintf(stderr, "movie mapping: %s -> %s\n", compiled->policy.localized[i].key,
                compiled->policy.localized[i].mapping);
    }
    CHECK(movie); if (!movie) return 0;
    const char *sound = sh_package_consumer_request(movie, "sound", "play_computed");
    const char *material = sh_package_consumer_request(movie, "material", "LATE\\FX"), *key = NULL, *empty = NULL;
    CHECK(sh_package_movie_string_route(movie, "#str_notice", &key) == 1);
    CHECK(sh_package_movie_string_route(movie, "#str_empty", &empty) == 1);
    CHECK(sound && material && key && empty);
    if (!sound || !material || !key || !empty) { sh_package_runtime_consumer_release(&held); return 0; }
    strcpy_s(out->owner, sizeof(out->owner), movie->name); strcpy_s(out->sound, sizeof(out->sound), sound);
    strcpy_s(out->material, sizeof(out->material), material); strcpy_s(out->key, sizeof(out->key), key);
    strcpy_s(out->empty, sizeof(out->empty), empty);
    CHECK(!sh_package_consumer_request(movie, "material", "sibling/only"));
    CHECK(sh_package_movie_string_route(movie, "#str_sibling", &key) == 0 && !key);
    const sh_package_localized_string *mapping = movie_lease_mapping(compiled, out->key);
    CHECK(mapping && strstr(mapping->mapping, variant ? "\"B\"" : "\"A\"") && strstr(mapping->mapping, "ancestor"));
    const sh_compiled_resource *fx = movie_lease_named(compiled, "material", out->material);
    CHECK(fx && strstr((const char *)fx->body, variant ? "0,1,0" : "1,0,0"));
    const sh_compiled_resource *sibling = historical_resource(compiled, "generated/decls/material/sibling/only.decl");
    const sh_compiled_resource *unrelated = historical_resource(compiled, "generated/decls/entitydef/movie/unrelated.decl");
    CHECK(sibling && unrelated);
    if (sibling) strcpy_s(out->sibling_path, sizeof(out->sibling_path), sibling->engine_path);
    if (unrelated) strcpy_s(out->unrelated_path, sizeof(out->unrelated_path), unrelated->engine_path);
    for (size_t i = 0; compiled && i < compiled->policy.localized_count; i++)
        if (strstr(compiled->policy.localized[i].mapping, "#str_sibling"))
            strcpy_s(out->sibling_key, sizeof(out->sibling_key), compiled->policy.localized[i].key);
    /* The complete prepared plan contains the distinct sibling namespace. */
    CHECK(out->sibling_key[0]);
    sh_package_runtime_consumer_release(&held);
    if (!sibling_published) {
        /* Additive admission publishes only the selected occurrence. Resolve
         * the fixture's one rewritten GUI field in the complete prepared plan,
         * then prove that unrelated sibling is absent from the serving view. */
        const sh_compiled_resource *entity = historical_resource(compiled,
            "generated/decls/entitydef/movie/sibling.decl");
        const char *start = entity && entity->body ? strstr((const char *)entity->body, "gui = \"") : NULL;
        if (start) start += strlen("gui = \"");
        const char *end = start ? strchr(start, '"') : NULL;
        sh_package_movie_names names = {0};
        int named = end && sh_package_movie_names_for_consumer(start, (size_t)(end - start), &names) == 1;
        CHECK(named && sh_package_compilation_find(compiled, names.movie));
        if (!named) return 0;
        strcpy_s(out->sibling_owner, sizeof(out->sibling_owner), names.movie);
        CHECK(sh_package_runtime_consumer_acquire("file", out->sibling_owner, &held) == 0);
        sh_package_runtime_consumer_release(&held);
        CHECK(out->sibling_owner[0] && strcmp(out->sibling_owner, out->owner)); return 1;
    }
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (sh_package_movie_owner("file", r->engine_path) &&
            sh_package_runtime_consumer_acquire("file", r->engine_path, &held) == 1) {
            const char *sibling_key = NULL;
            if (sh_package_movie_string_route(held.binding, "#str_sibling", &sibling_key) == 1 &&
                sibling_key && !strcmp(sibling_key, out->sibling_key))
                strcpy_s(out->sibling_owner, sizeof(out->sibling_owner), held.binding->name);
            sh_package_runtime_consumer_release(&held);
        }
    }
    CHECK(out->sibling_owner[0] && strcmp(out->sibling_owner, out->owner)); return 1;
}
static void movie_lease_expect(const movie_lease_expected *a, const sh_package_consumer *held,
    const movie_lease_bytes *bytes, int retained, int vanilla)
{
    CHECK(held && !strcmp(held->name, a->owner));
    const char *sound = sh_package_consumer_request(held, "sound", "play_computed");
    const char *material = sh_package_consumer_request(held, "material", "LATE\\FX");
    CHECK(sound && !strcmp(sound, a->sound)); CHECK(material && !strcmp(material, a->material));
    const char *key = NULL;
    CHECK(sh_package_movie_string_route(held, "#str_notice", &key) == 1 && key && !strcmp(key, a->key));
    sh_package_consumer_hold published = {0};
    CHECK(sh_package_runtime_consumer_acquire("file", a->owner, &published) == 1);
    key = NULL;
    CHECK(sh_package_movie_string_route(published.binding, "#str_notice", &key) == 1 && key && !strcmp(key, a->key));
    sh_package_runtime_consumer_release(&published);
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *movie = movie_lease_named(current, "file", a->owner);
    CHECK(!!movie == retained);
    if (retained) {
        CHECK(movie && movie->body_length == bytes->used && !memcmp(movie->body, bytes->data, bytes->used));
        const sh_compiled_resource *sound = movie_lease_named(current, "sound", a->sound);
        CHECK(sound);
        const sh_compiled_resource *fx = movie_lease_named(current, "material", a->material);
        CHECK(fx && strstr((const char *)fx->body, "1,0,0"));
        const sh_package_localized_string *mapping = movie_lease_mapping(current, a->key);
        CHECK(mapping && strstr(mapping->mapping, "\"A\"") && strstr(mapping->mapping, "ancestor"));
        CHECK(movie_lease_mapping(current, a->empty));
        /* Physical movie dependencies and late request targets are distinct
         * edges. Verify/read both exact closures outside the acquired view. */
        size_t count = movie ? movie->bound_dependency_count : 0;
        char **paths = calloc(count + 2, sizeof(*paths));
        CHECK(paths);
        unsigned closure = 0;
        for (size_t i = 0; paths && i < count; i++) {
            const sh_compiled_resource *dependency = sh_package_compilation_find(current, movie->bound_dependencies[i]);
            CHECK(dependency);
            if (dependency && sh_package_resource_native_matches(dependency, "sound", a->sound)) closure |= 1;
            if (dependency && sh_package_resource_native_matches(dependency, "material", a->material)) closure |= 2;
            paths[i] = _strdup(movie->bound_dependencies[i]); CHECK(paths[i]);
        }
        if (paths && sound) { paths[count] = _strdup(sound->engine_path); CHECK(paths[count]); closure |= 1; }
        if (paths && fx) { paths[count + 1] = _strdup(fx->engine_path); CHECK(paths[count + 1]); closure |= 2; }
        count += 2;
        CHECK(closure == 3);
        CHECK(!movie_lease_named(current, "file", a->sibling_owner));
        CHECK(!sh_package_compilation_find(current, a->sibling_path));
        CHECK(!sh_package_compilation_find(current, a->unrelated_path));
        CHECK(!movie_lease_mapping(current, a->sibling_key));
        if (vanilla) {
            /* The native dictionary projection must contain the retained
             * private labels; no authored public key or global HUD is allowed. */
            CHECK(!current->policy.hud.count && !current->policy.hud_binding_count);
            CHECK(current->policy.strings.count == 2);
            CHECK(!strncmp(a->key, "#str_", 5) && !strncmp(a->empty, "#str_", 5));
            unsigned labels = 0;
            for (size_t i = 0; i < current->policy.strings.count; i++) {
                const sh_json_member *locale = current->policy.strings.members + i;
                int english = !strcmp(locale->key, "english"), french = !strcmp(locale->key, "french");
                sh_json_object values = {0};
                CHECK(english || french);
                int parsed = sh_json_parse_object(locale->value_json, strlen(locale->value_json), 16, &values);
                CHECK(parsed);
                for (size_t j = 0; parsed && j < values.count; j++) {
                    const sh_json_member *value = values.members + j;
                    if (!strcmp(value->key, a->key + 5)) {
                        CHECK(!strcmp(value->value_json, english ? "\"A\"" : "\"ancestor\""));
                        labels |= english ? 1 : 2;
                    } else if (!strcmp(value->key, a->empty + 5)) {
                        CHECK(english && !strcmp(value->value_json, "\"\"")); labels |= 4;
                    } else CHECK(0); /* Any public or sibling label is a leak. */
                }
                sh_json_object_free(&values);
            }
            CHECK(labels == 7);
            CHECK(!sh_package_owners_count(&current->policy.legacy_string_owners));
        }
        sh_package_runtime_release();
        unsigned char *body = NULL; size_t length = 0;
        CHECK(sh_package_runtime_read(a->owner, &body, &length) == 1 && length == bytes->used &&
            body && !memcmp(body, bytes->data, bytes->used)); free(body);
        for (size_t i = 0; paths && i < count; i++) {
            body = NULL; length = 0;
            CHECK(paths[i] && sh_package_runtime_read(paths[i], &body, &length) == 1 && body && length);
            free(body); free(paths[i]);
        }
        free(paths); return;
    }
    CHECK(!movie_lease_mapping(current, a->key) && !movie_lease_mapping(current, a->empty));
    sh_package_runtime_release();
}
static int movie_lease_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{ (void)context; (void)restoring; (void)changes; (void)error; (void)capacity; return 1; }

typedef struct movie_rollback_probe {
    sh_package_map_plan *plan;
    movie_lease_expected expected;
    sh_package_movie_hold hold;
    unsigned forward, recovered;
    int release_constructor_frame;
} movie_rollback_probe;
static void movie_rollback_served(const movie_rollback_probe *probe, const movie_lease_bytes *bytes)
{
    const movie_lease_expected *b = &probe->expected;
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *movie = movie_lease_named(current, "file", b->owner);
    const sh_compiled_resource *sound = movie_lease_named(current, "sound", b->sound);
    const sh_compiled_resource *fx = movie_lease_named(current, "material", b->material);
    const sh_package_localized_string *mapping = movie_lease_mapping(current, b->key);
    CHECK(movie && movie->body_length == bytes->used && !memcmp(movie->body, bytes->data, bytes->used));
    CHECK(sound && fx && strstr((const char *)fx->body, "0,1,0"));
    CHECK(mapping && strstr(mapping->mapping, "\"B\"") && strstr(mapping->mapping, "ancestor"));
    CHECK(movie_lease_mapping(current, b->empty));
    for (size_t i = 0; movie && i < movie->bound_dependency_count; i++)
        CHECK(sh_package_compilation_find(current, movie->bound_dependencies[i]));
    CHECK(!sh_package_compilation_find(current, b->unrelated_path));
    const sh_compiled_resource *authored = historical_resource(sh_package_map_plan_compilation(probe->plan),
        "generated/decls/entitydef/movie/screen.decl");
    CHECK(authored && !sh_package_compilation_find(current, authored->engine_path));
    CHECK(!sh_package_owners_count(&current->policy.legacy_string_owners));
    sh_package_runtime_release();
    unsigned char *body = NULL; size_t length = 0;
    CHECK(sh_package_runtime_read(b->owner, &body, &length) == 1);
    CHECK(body && length == bytes->used && !memcmp(body, bytes->data, bytes->used)); free(body);
}
static int movie_rollback_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    movie_rollback_probe *probe = context;
    if (!restoring) {
        probe->forward++;
        CHECK(movie_lease_capture(probe->plan, &probe->expected, 1, !probe->release_constructor_frame));
        CHECK(sh_package_runtime_movie_begin(probe->expected.owner, &probe->hold) == 1);
        if (probe->release_constructor_frame) sh_package_runtime_movie_release(&probe->hold);
        snprintf(error, capacity, "injected failure after candidate movie begin"); return 0;
    }
    probe->recovered++;
    /* Recovery must not retire a B file/consumer which the fallback still
     * serves. The instance may already have returned from its constructor. */
    for (size_t i = 0; i < changes->count; i++)
        CHECK(strcmp(changes->items[i].path, probe->expected.owner) ||
            changes->items[i].kind != SH_PACKAGE_RESOURCE_REMOVED);
    for (size_t i = 0; i < changes->removed_count; i++)
        CHECK(strcmp(changes->removed[i].name, probe->expected.owner));
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *fx = movie_lease_named(current, "material", probe->expected.material);
    int reconstructed = 0;
    for (size_t i = 0; fx && i < changes->count; i++)
        if (!strcmp(changes->items[i].path, fx->engine_path) &&
            changes->items[i].kind == SH_PACKAGE_RESOURCE_REPLACED) reconstructed = 1;
    CHECK(fx && reconstructed); /* Covers a material fault after diff preparation. */
    sh_package_runtime_release();
    return 1;
}
static int movie_lease_atlas(void *context, size_t scope, const sh_package_compilation *compiled,
    const char *material, const char *page_file, sh_package_virtual_atlas *out, char *error, size_t capacity)
{
    unsigned *seen = context;
    (void)scope; (void)error; (void)capacity;
    CHECK(seen && compiled && material && page_file && out);
    if (!seen || !compiled || !material || !page_file || !out) return -1;
    CHECK(!*page_file); if (*page_file) return -1;
    if (!strcmp(material, "late/fx")) *seen |= 1;
    if (!strcmp(material, "sibling/only")) *seen |= 2;
    /* These materials have no VT row. Certify their original empty provider
     * while retaining the real scoped material-body/dependency validation. */
    memset(out, 0, sizeof(*out));
    out->name = "_vmtr"; out->source = "virtualtextures/_vmtr"; out->original = 1;
    return 1;
}
static struct {
    const char *name;
    sh_package_movie_hold *hold;
    unsigned begun, aborted, committed;
} movie_lease_race;
static void movie_lease_race_begin(void)
{
    movie_lease_race.begun++;
    if (!movie_lease_race.hold->token)
        CHECK(sh_package_runtime_movie_begin(movie_lease_race.name, movie_lease_race.hold) == 1);
}
static void movie_lease_race_end(int committed)
{
    if (committed) movie_lease_race.committed++;
    else movie_lease_race.aborted++;
}
static sh_package_map_plan *movie_lease_prepare(const char *data, const char *map, char *error, size_t capacity)
{
    sh_package_sources *sources = sh_package_sources_scan(map, error, capacity);
    sh_package_provenance recipe = {0}; sh_package_map_plan *plan = NULL;
    CHECK(sources); if (!sources) return NULL;
    /* This fixture is authored library content with one namespace per source
     * component. Historical COMPOSED maps intentionally use another policy. */
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *component = sources->components + i;
        const unsigned char *tree = sh_package_source_tree(sources, component->owner);
        size_t context = tree ? sh_package_provenance_context_add(&recipe, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])tree, 1, component->relative) : SIZE_MAX;
        CHECK(context != SIZE_MAX); if (context == SIZE_MAX) goto done;
    }
    CHECK(recipe.context_count == sources->component_count && recipe.context_count);
    if (recipe.context_count) plan = sh_package_runtime_prepare_map_with_bindings(data, map, &recipe, error, capacity);
done:
    sh_package_provenance_free(&recipe); sh_package_sources_free(sources); return plan;
}
static void test_movie_file_provider_lifetime_mode(int pooled)
{
    char data[MAX_PATH], maps[2][MAX_PATH], error[2048];
    sh_package_map_plan *plans[2] = {0}; movie_lease_expected expected[2] = {0};
    sh_package_consumer_hold lifetime = {0}; movie_lease_bytes movie = {0};
    sh_package_movie_hold first = {0}, second = {0}, delayed = {0}, callback = {0}, stock = {0};
    movie_rollback_probe rollback = {0};
    sh_package_occurrence_snapshot *rollback_document = NULL;
    char *rollback_bound = NULL; size_t rollback_length = 0;
    const sh_package_consumer *retained = NULL;
    int first_ended = 0, second_ended = 0;
    unsigned atlas_seen = 0;
    admission_fixture f; movie_lease_reflection(&f); movie_lease_build(&movie);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {movie_lease_reserve, NULL, 64, movie_lease_global};
    create("movie-lease-data/overrides", NULL); snprintf(data, sizeof(data), "%s/movie-lease-data", root);
    for (unsigned i = 0; i < 2; i++) {
        char path[512], descriptor[512]; snprintf(maps[i], sizeof(maps[i]), "%s/movie-lease-%c", root, 'a' + i);
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/package.json", 'a' + i);
        create(path, "{\"id\":\"movies\",\"name\":\"Movies\",\"strings\":{\"french\":{\"notice\":\"ancestor\"}},"
            "\"hud\":{\"weapons\":{\"retained-a\":{\"ammo_display\":\"weapon\"}}}}");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/assets/generated/decls/material/late/fx.decl", 'a' + i);
        create(path, i ? "{ color {0,1,0} }" : "{ color {1,0,0} }");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/assets/generated/decls/sound/play_computed.decl", 'a' + i);
        create(path, "{ edit = {} }");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/selected/package.json", 'a' + i);
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"same\",\"name\":\"Selected movie\",\"strings\":{\"english\":{\"notice\":\"%c\",\"empty\":\"\"}}}", 'A' + i);
        create(path, descriptor);
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/selected/assets/generated/decls/entitydef/movie/screen.decl", 'a' + i);
        create(path, "{ class = \"Entity\"; edit = { gui = \"swf/stock/panel.swf\"; } }");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/sibling/package.json", 'a' + i);
        create(path, i ? "{\"id\":\"sibling\",\"name\":\"Sibling movie\",\"strings\":{\"english\":{\"sibling\":\"foreign B\"}}}" :
            "{\"id\":\"sibling\",\"name\":\"Sibling movie\",\"strings\":{\"english\":{\"sibling\":\"foreign A\"}}}");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/sibling/assets/generated/decls/material/sibling/only.decl", 'a' + i);
        create(path, i ? "{ color {1,1,0} }" : "{ color {0,0,1} }");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/outer/sibling/assets/generated/decls/entitydef/movie/sibling.decl", 'a' + i);
        create(path, "{ class = \"Entity\"; edit = { gui = \"swf/stock/panel.swf\"; } }");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/unrelated/package.json", 'a' + i);
        create(path, "{\"id\":\"unrelated\",\"name\":\"Unrelated\"}");
        snprintf(path, sizeof(path), "movie-lease-%c/overrides/unrelated/assets/generated/decls/entitydef/movie/unrelated.decl", 'a' + i);
        create(path, i ? "{ class = \"Entity\"; edit = { health = 89; } }" :
            "{ class = \"Entity\"; edit = { health = 88; } }");
    }
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(movie_lease_baseline, &movie);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    /* Existing constructor capture-only seam; source_link still constructs the
     * real movie namespace and graph. No catalog or closure is injected. */
    sh_package_runtime_test_constructor_requests(consumer_requests, NULL);
    sh_package_runtime_test_strings(&strings);
    sh_package_runtime_test_atlas(movie_lease_atlas, &atlas_seen);
    for (unsigned i = 0; i < 2; i++) {
        atlas_seen = 0;
        plans[i] = movie_lease_prepare(data, maps[i], error, sizeof(error));
        if (!plans[i]) fprintf(stderr, "movie lease prepare: %s\n", error);
        CHECK(plans[i]); if (!plans[i]) goto done;
        CHECK((atlas_seen & 3) == 3); /* Target and nonselected sibling were both inspected. */
        if (pooled >= 2) {
            /* The ordinary closure fixture certifies an empty VMTR. Recovery
             * reconstruction needs an actual mapped binding, so add the same
             * valid test-owned row used by the native atlas transaction case. */
            const sh_compiled_resource *fx = historical_resource(sh_package_map_plan_compilation(plans[i]),
                "generated/decls/material/late/fx.decl");
            sh_virtual_material_record row = {"source/texture", 0, 0, 16, 16, 0, 1, 2};
            sh_virtual_material_runtime_binding binding = {fx ? fx->name : NULL, "_vmtr", &row, 0};
            CHECK(fx && fx->name); if (!fx || !fx->name) goto done;
            int bound = sh_package_map_plan_bind_virtual_materials(plans[i], &binding, 1, error, sizeof(error));
            if (!bound) fprintf(stderr, "movie rollback atlas binding mode %d: %s\n", pooled, error);
            CHECK(bound); if (!bound) goto done;
        }
    }
    if (pooled >= 2) {
        CHECK(sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, NULL, NULL));
        CHECK(movie_lease_capture(plans[0], expected, 0, 1));
        rollback.plan = plans[1]; rollback.release_constructor_frame = pooled == 3;
        if (pooled >= 3) {
            /* A fresh isolated use names the prepared native catalog identity.
             * No historical occurrence exists to select an authored spelling. */
            const sh_compiled_resource *screen = historical_resource(sh_package_map_plan_compilation(plans[1]),
                "generated/decls/entitydef/movie/screen.decl");
            char json[2048];
            CHECK(screen && screen->name); if (!screen || !screen->name) goto done;
            snprintf(json, sizeof(json), "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
                "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", screen->name);
            size_t length = 0;
            char *rebound = sh_package_map_plan_rebind(plans[1], NULL, json, strlen(json), &length, error, sizeof(error));
            if (!rebound) fprintf(stderr, "movie rollback rebind mode %d: %s\n", pooled, error);
            CHECK(rebound);
            if (rebound) {
                rollback_bound = sh_package_runtime_capture_document(plans[1], NULL, NULL, 0, rebound, length,
                    &rollback_length, &rollback_document, NULL, NULL, error, sizeof(error));
                if (!rollback_bound || !rollback_document)
                    fprintf(stderr, "movie rollback capture mode %d: %s\n", pooled, error);
                HeapFree(GetProcessHeap(), 0, rebound);
            }
            CHECK(rollback_bound && rollback_document); if (!rollback_document) goto done;
        }
        if (pooled == 4) {
            /* Clone already covers A, but a post-clone birth still must abort
             * and retry before the additive publication/activation begins. */
            memset(&movie_lease_race, 0, sizeof(movie_lease_race));
            movie_lease_race.name = expected[0].owner; movie_lease_race.hold = &first;
            CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], rollback_document,
                (sh_package_activation_guard){movie_lease_race_begin, movie_lease_race_end},
                movie_lease_activate, NULL, error, sizeof(error)) == SH_PACKAGE_ADMISSION_READY);
            CHECK(movie_lease_race.begun == 2 && movie_lease_race.aborted == 1 && movie_lease_race.committed == 1);
            CHECK(first.binding && first.token && sh_package_runtime_has_map_provider());
            sh_package_runtime_movie_end_instance(&first); first_ended = 1;
            sh_package_runtime_movie_release(&first);
        } else {
            /* First reserve candidate records, then fail rollback reservation:
             * there must be no native activation or serving-pointer mutation. */
            sh_package_runtime_test_consumer_allocation_fault(2);
            CHECK(!sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0},
                movie_rollback_activate, &rollback));
            CHECK(!rollback.forward && !rollback.recovered && !rollback.hold.token);
            CHECK(!sh_package_runtime_test_consumer_allocation_remaining());
            CHECK(sh_package_runtime_has_map_provider());
            const sh_package_compilation *before = sh_package_runtime_acquire();
            CHECK(movie_lease_named(before, "file", expected[0].owner)); sh_package_runtime_release();
            sh_package_runtime_test_consumer_allocation_fault(0);
            if (pooled == 2)
                CHECK(!sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0},
                    movie_rollback_activate, &rollback));
            else
                CHECK(sh_package_runtime_activate_prepared_occurrences(plans[1], rollback_document,
                    (sh_package_activation_guard){0}, movie_rollback_activate, &rollback,
                    error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
            CHECK(rollback.forward == 1 && rollback.recovered == 1);
            CHECK(sh_package_runtime_has_map_provider() && sh_package_runtime_admission_ready());
            CHECK(sh_package_runtime_movie_borrow(rollback.expected.owner, &callback) == 1);
            CHECK(callback.binding && !strcmp(callback.binding->name, rollback.expected.owner));
            movie_rollback_served(&rollback, &movie);
            /* Cancel the rejected incoming document. Its separately captured
             * occurrence lease may legitimately retain the B entitydef on a
             * later refresh; this regression isolates the movie obligation. */
            sh_package_occurrence_release(rollback_document); rollback_document = NULL;
            free(rollback_bound); rollback_bound = NULL;
            /* The same previous map continues. A supported refresh narrows the
             * preallocated superset while B remains a valid live movie. */
            CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0},
                movie_lease_activate, NULL));
            movie_rollback_served(&rollback, &movie);
            const sh_package_compilation *narrowed = sh_package_runtime_acquire();
            CHECK(!movie_lease_named(narrowed, "file", rollback.expected.sibling_owner));
            CHECK(!movie_lease_mapping(narrowed, rollback.expected.sibling_key));
            sh_package_runtime_release();
            sh_package_runtime_movie_end_instance(&callback);
            sh_package_runtime_movie_release(&callback); sh_package_runtime_movie_release(&rollback.hold);
        }
        sh_package_occurrence_release(rollback_document); rollback_document = NULL;
        free(rollback_bound); rollback_bound = NULL;
        CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, movie_lease_activate, NULL));
        const sh_package_compilation *retired = sh_package_runtime_acquire();
        if (pooled != 4) CHECK(!movie_lease_named(retired, "file", rollback.expected.owner));
        CHECK(!movie_lease_named(retired, "file", expected[0].owner)); sh_package_runtime_release();
        size_t providers = 99, live = 99;
        CHECK(!sh_package_runtime_test_movie_records(&providers, &live) && !providers && !live);
        goto done;
    }
    if (pooled) {
        size_t before = sh_package_runtime_test_movie_records(NULL, NULL);
        sh_package_movie_hold untouched = {(const sh_package_consumer *)1, (void *)2};
        CHECK(sh_package_runtime_movie_begin("generated/swf/stock/panel.bswf", &untouched) == -1);
        CHECK(untouched.binding == (const sh_package_consumer *)1 && untouched.token == (void *)2);
        sh_package_runtime_test_consumer_allocation_fault(1);
        CHECK(!sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, NULL, NULL));
        CHECK(!sh_package_runtime_has_map_provider());
        CHECK(sh_package_runtime_test_movie_records(NULL, NULL) == before);
        CHECK(!sh_package_runtime_test_consumer_allocation_remaining());
        CHECK(sh_package_runtime_movie_begin("generated/swf/stock/panel.bswf", &stock) == 0);
        CHECK(!stock.binding && !stock.token);
        sh_package_runtime_test_consumer_allocation_fault(0);
    }
    CHECK(sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, NULL, NULL));
    CHECK(movie_lease_capture(plans[0], expected, 0, 1)); if (!expected[0].owner[0]) goto done;
    if (pooled) {
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &callback) == -1);
        CHECK(!callback.binding && !callback.token);
        sh_package_runtime_test_consumer_allocation_fault(1);
        CHECK(sh_package_runtime_movie_begin(expected[0].owner, &first) == 1);
        CHECK(first.binding && first.token); if (!first.binding || !first.token) goto done;
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &callback) == 1);
        CHECK(callback.binding == first.binding && callback.token == first.token);
        CHECK(sh_package_runtime_movie_begin("generated/swf/stock/panel.bswf", &stock) == 0);
        CHECK(sh_package_runtime_movie_borrow("generated/swf/stock/panel.bswf", &stock) == 0);
        CHECK(sh_package_runtime_movie_borrow("generated/swf/missing/not-owned.bswf", &stock) == 0);
        CHECK(!stock.binding && !stock.token); /* A nested stock request cannot inherit A. */
        CHECK(sh_package_runtime_test_consumer_allocation_remaining() == 1); /* begin/borrow allocate nothing. */
        sh_package_runtime_test_consumer_allocation_fault(0);
        sh_package_runtime_movie_release(&callback);
        /* The first view selects no live A. Starting A at the prepublication
         * guard must rebuild that view internally, before native retirement. */
        sh_package_runtime_movie_end_instance(&first);
        sh_package_runtime_movie_release(&first);
        memset(&movie_lease_race, 0, sizeof(movie_lease_race));
        movie_lease_race.name = expected[0].owner; movie_lease_race.hold = &first;
        CHECK(sh_package_runtime_activate_prepared_map(NULL,
            (sh_package_activation_guard){movie_lease_race_begin, movie_lease_race_end}, movie_lease_activate, NULL));
        CHECK(movie_lease_race.begun == 2 && movie_lease_race.aborted == 1 && movie_lease_race.committed == 1);
        CHECK(first.binding && first.token && !sh_package_runtime_has_map_provider());
        unsigned char *race_bytes = NULL; size_t race_length = 0;
        CHECK(sh_package_runtime_read(expected[0].owner, &race_bytes, &race_length) == 1);
        CHECK(race_bytes && race_length == movie.used && !memcmp(race_bytes, movie.data, movie.used));
        free(race_bytes);
        /* A second provider with the same complete plan uses the live record,
         * rather than redirecting existing instances to its idle reservation. */
        CHECK(sh_package_runtime_activate_prepared_map(plans[0], (sh_package_activation_guard){0}, NULL, NULL));
        CHECK(sh_package_runtime_movie_begin(expected[0].owner, &second) == 1);
        CHECK(second.binding == first.binding && second.token == first.token);
        retained = first.binding;
    } else {
        CHECK(sh_package_runtime_consumer_retain("file", expected[0].owner, &lifetime, error, sizeof(error)) == 1);
        CHECK(lifetime.binding && lifetime.lifetime); if (!lifetime.binding || !lifetime.lifetime) goto done;
        retained = lifetime.binding;
    }
    CHECK(sh_package_runtime_activate_prepared_map(plans[1], (sh_package_activation_guard){0}, NULL, NULL));
    CHECK(movie_lease_capture(plans[1], expected + 1, 1, 1)); if (!expected[1].owner[0]) goto done;
    CHECK(strcmp(expected[0].owner, expected[1].owner));
    CHECK(strcmp(expected[0].material, expected[1].material) && strcmp(expected[0].key, expected[1].key));
    CHECK(strcmp(expected[0].sibling_key, expected[1].sibling_key));
    CHECK(strcmp(expected[0].sibling_path, expected[1].sibling_path) &&
        strcmp(expected[0].unrelated_path, expected[1].unrelated_path));
    const sh_package_compilation *active_b = sh_package_runtime_acquire();
    const sh_package_localized_string *b_mapping = movie_lease_mapping(active_b, expected[1].key);
    const sh_compiled_resource *b_fx = movie_lease_named(active_b, "material", expected[1].material);
    CHECK(b_mapping && strstr(b_mapping->mapping, "\"B\""));
    CHECK(b_fx && strstr((const char *)b_fx->body, "0,1,0"));
    sh_package_runtime_release();
    for (unsigned i = 0; i < 2; i++) { sh_package_map_plan_free(plans[i]); plans[i] = NULL; }
    if (pooled) {
        size_t providers = 0, live = 0;
        CHECK(sh_package_runtime_test_movie_records(&providers, &live) && providers && live == 1);
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &delayed) == 1);
        CHECK(delayed.binding == retained && delayed.token == first.token);
        sh_package_runtime_movie_end_instance(&first); first_ended = 1;
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &callback) == 1); /* Second instance still live. */
        sh_package_runtime_movie_release(&callback);
        sh_package_runtime_movie_end_instance(&second); second_ended = 1;
        sh_package_runtime_movie_release(&first); sh_package_runtime_movie_release(&second);
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &callback) == -1);
        CHECK(!callback.binding && !callback.token);
        CHECK(sh_package_runtime_test_movie_records(NULL, &live) && live == 1); /* Last entered frame owns A. */
    }
    movie_lease_expect(expected, retained, &movie, 1, 0);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, NULL, NULL));
    CHECK(!sh_package_runtime_has_map_provider());
    movie_lease_expect(expected, retained, &movie, 1, 1);
    if (pooled) {
        const sh_package_compilation *vanilla = sh_package_runtime_acquire();
        CHECK(!movie_lease_named(vanilla, "file", expected[1].owner)); /* B was never begun. */
        sh_package_runtime_release();
        /* Offline teardown clears the published provider without destroying
         * the last entered frame. Its exact record remains usable on its own. */
        sh_package_runtime_test_dispose();
        CHECK(sh_package_runtime_movie_begin(expected[0].owner, &callback) == 1);
        CHECK(callback.binding == delayed.binding && callback.token == delayed.token);
        CHECK(sh_package_runtime_movie_borrow(expected[0].owner, &stock) == 1);
        CHECK(stock.binding == delayed.binding && stock.token == delayed.token);
        sh_package_runtime_movie_end_instance(&callback);
        sh_package_runtime_movie_release(&callback); sh_package_runtime_movie_release(&stock);
        CHECK(sh_package_runtime_refresh(data));
        sh_package_runtime_movie_release(&delayed);
        CHECK(!delayed.token && !delayed.binding);
        CHECK(sh_package_runtime_occurrences_pending());
    } else sh_package_runtime_consumer_release(&lifetime);
    CHECK(!lifetime.binding && !lifetime.provider && !lifetime.lifetime);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, movie_lease_activate, NULL));
    if (pooled) {
        size_t providers = 99, live = 99;
        CHECK(!sh_package_runtime_test_movie_records(&providers, &live) && !providers && !live);
    }
    const sh_package_compilation *current = sh_package_runtime_acquire();
    CHECK(!movie_lease_named(current, "file", expected[0].owner));
    CHECK(!movie_lease_named(current, "file", expected[1].owner));
    CHECK(!movie_lease_named(current, "sound", expected[0].sound));
    CHECK(!movie_lease_named(current, "material", expected[0].material));
    CHECK(!movie_lease_mapping(current, expected[0].key) && !movie_lease_mapping(current, expected[0].empty));
    CHECK(!current->policy.localized_count);
    /* Native originals restored after an override retires may remain served;
     * no private or package-owned resource may survive this final boundary. */
    for (size_t i = 0; i < current->resource_count; i++) {
        const sh_compiled_resource *r = current->resources + i;
        CHECK((r->native_original || r->restored_original) && !r->original_path);
        CHECK(!sh_package_owners_count(&r->owners) && !sh_package_owners_count(&r->gameplay_owners));
        CHECK(r->source == SIZE_MAX && !r->source_count);
    }
    CHECK(!current->policy.strings.count && !current->policy.hud.count && !current->policy.hud_binding_count);
    sh_package_runtime_release();
    unsigned char *served = NULL; size_t length = 0;
    CHECK(sh_package_runtime_read(expected[0].owner, &served, &length) == 0); free(served);
done:
    sh_package_runtime_test_consumer_allocation_fault(0);
    if (rollback.expected.owner[0] && sh_package_runtime_movie_borrow(rollback.expected.owner, &stock) == 1) {
        sh_package_runtime_movie_end_instance(&stock); sh_package_runtime_movie_release(&stock);
    }
    sh_package_runtime_movie_release(&rollback.hold);
    sh_package_occurrence_release(rollback_document); free(rollback_bound);
    if (!first_ended) sh_package_runtime_movie_end_instance(&first);
    if (!second_ended) sh_package_runtime_movie_end_instance(&second);
    sh_package_runtime_movie_release(&callback); sh_package_runtime_movie_release(&delayed);
    sh_package_runtime_movie_release(&first); sh_package_runtime_movie_release(&second);
    sh_package_runtime_consumer_release(&lifetime);
    for (unsigned i = 0; i < 2; i++) sh_package_map_plan_free(plans[i]);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_constructor_requests(NULL, NULL);
    sh_package_runtime_test_strings(NULL); sh_package_runtime_test_atlas(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    while (f.count) free(f.regions[--f.count].address);
    cleanup_resources(data);
}
static void test_movie_file_provider_lifetime(void) { test_movie_file_provider_lifetime_mode(0); }
static void test_movie_record_provider_lifetime(void) { test_movie_file_provider_lifetime_mode(1); }
static void test_movie_rollback_provider_lifetime(void)
{
    test_movie_file_provider_lifetime_mode(2); /* Full activation, entered frame remains. */
    test_movie_file_provider_lifetime_mode(3); /* Additive activation, instance only. */
    test_movie_file_provider_lifetime_mode(4); /* Additive zero-to-live publication race. */
}
/* Whether the published view serves this native unit. */
static int chain_serves(const char *family, const char *name)
{
    const sh_package_compilation *current = sh_package_runtime_acquire();
    int found = 0;
    for (size_t i = 0; current && !found && i < current->resource_count; i++)
        found = sh_package_resource_native_matches(current->resources + i, family, name);
    sh_package_runtime_release(); return found;
}
/* A family selector keeps only the roots of what it borrows. It is acquired
 * from map B's view while map A's copy is held, so that view also serves A.
 * Once A's copy is released and B is left, the selector still borrows B's
 * materials and keeps B's program, but A's program retires, although A ships
 * one of those materials' packages too: B's view served it from B. Releasing
 * the selector retires B's and releases both roots. */
static void test_audio_selector_chain(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    const char *palette = "generated/decls/material/palette/tool_b.decl";
    char data[MAX_PATH], map_a[MAX_PATH], map_b[MAX_PATH], base[MAX_PATH], error[2048], tool[1024] = "", tint[1024] = "";
    sh_package_audio_program *listed[8] = {0}, *program_a = NULL, *program_b = NULL;
    sh_package_occurrence_snapshot *document_a = NULL; sh_package_selector_hold *selector = NULL;
    sh_package_map_plan *plan = NULL; int fail = 0; size_t count, roots;
    movie_lease_bytes renderparms = {0}; unsigned atlas_seen = 0;
    admission_fixture f; admission_setup(&f); movie_lease_build(&renderparms);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/selector-chain-library", root);
    snprintf(map_a, sizeof(map_a), "%s/selector-chain-a", root);
    snprintf(map_b, sizeof(map_b), "%s/selector-chain-b", root);
    snprintf(base, sizeof(base), "%s/selector-chain-base", root);
    create("selector-chain-base/sound", NULL);
    create("selector-chain-library/overrides", NULL);
    create("selector-chain-a/overrides/sound-a/package.json", "{\"id\":\"sound-a\",\"name\":\"Sound A\"}");
    create("selector-chain-a/overrides/sound-a/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("selector-chain-a/overrides/sound-a/assets/sound/soundbanks/pc/selectorbank_a.bnk", "selectorbank_a.bnk", 77);
    create("selector-chain-a/overrides/shared/package.json", "{\"id\":\"shared\",\"name\":\"Shared\"}");
    create("selector-chain-a/overrides/shared/assets/generated/decls/material/shared/tint.decl", "{ color {1,1,0} }");
    create("selector-chain-b/overrides/shared/package.json", "{\"id\":\"shared\",\"name\":\"Shared\"}");
    create("selector-chain-b/overrides/shared/assets/generated/decls/material/shared/tint.decl", "{ color {1,1,0} }");
    create("selector-chain-b/overrides/sound-b/package.json", "{\"id\":\"sound-b\",\"name\":\"Sound B\"}");
    create("selector-chain-b/overrides/sound-b/assets/generated/decls/material/palette/tool_b.decl", "{ color {0,1,0} }");
    write_bank("selector-chain-b/overrides/sound-b/assets/sound/soundbanks/pc/selectorbank_b.bnk", "selectorbank_b.bnk", 78);
    /* The movie lease baseline supplies the material render parameters. */
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(movie_lease_baseline, &renderparms);
    sh_package_runtime_test_atlas(movie_lease_atlas, &atlas_seen);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    roots = sh_package_runtime_test_audio_roots();
    /* Map A and a copy taken from it, then A is left with its copy held. */
    plan = sh_package_runtime_prepare_map(data, map_a, error, sizeof(error));
    if (!plan) fprintf(stderr, "selector chain map A: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1); program_a = listed[0];
    document_a = chain_capture(plan, json, NULL); CHECK(document_a);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Map B, and a selector acquired from the view that also serves A. */
    plan = sh_package_runtime_prepare_map(data, map_b, error, sizeof(error));
    if (!plan) fprintf(stderr, "selector chain map B: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    const sh_compiled_resource *entry = historical_resource(sh_package_map_plan_compilation(plan), palette);
    CHECK(entry && entry->native_name); if (!entry || !entry->native_name) goto done;
    strcpy_s(tool, sizeof(tool), entry->native_name);
    entry = historical_resource(sh_package_map_plan_compilation(plan), "generated/decls/material/shared/tint.decl");
    CHECK(entry && entry->native_name); if (!entry || !entry->native_name) goto done;
    strcpy_s(tint, sizeof(tint), entry->native_name);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    CHECK(count == 2);
    for (size_t i = 0; i < count && i < 8; i++) if (listed[i] != program_a) program_b = listed[i];
    CHECK(program_a && program_b && program_b != program_a);
    selector = sh_package_runtime_selectors_retain("material", error, sizeof(error));
    if (!selector) fprintf(stderr, "selector chain material: %s\n", error);
    CHECK(selector); if (!selector) goto done;
    /* A's copy is released and B is left while the selector still borrows B. */
    sh_package_occurrence_release(document_a); document_a = NULL;
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_serves("material", tool) && chain_serves("material", tint));
    count = chain_programs(listed, 8);
    if (count != 1) fprintf(stderr, "selector chain: %zu programs while only B's material is held\n", count);
    CHECK(count == 1 && listed[0] == program_b);
    sh_package_runtime_selectors_release(selector); selector = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!chain_serves("material", tool));
    CHECK(chain_programs(listed, 8) == 0 && sh_package_runtime_test_audio_roots() == roots);
done:
    sh_package_runtime_selectors_release(selector);
    sh_package_occurrence_release(document_a); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_atlas(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* A live movie keeps only the roots of what it borrows. Map X's copy is held
 * while movie map B opens, so B's view also serves X, and B's movie begins from
 * that view. Once X's copy is released and B is left, the live movie keeps its
 * closure and B's program, but X's program retires. Ending the movie retires
 * B's and releases both roots. */
static void test_audio_movie_chain(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    char data[MAX_PATH], map_x[MAX_PATH], map_b[MAX_PATH], base[MAX_PATH], error[2048];
    sh_package_audio_program *listed[8] = {0}, *program_x = NULL, *program_b = NULL;
    sh_package_occurrence_snapshot *document_x = NULL; sh_package_map_plan *plan = NULL;
    sh_package_movie_hold movie_hold = {0}; movie_lease_expected expected = {0}; movie_lease_bytes movie = {0};
    int fail = 0, ended = 0; unsigned atlas_seen = 0; size_t count, roots, providers = 99, live = 99;
    admission_fixture f; movie_lease_reflection(&f); movie_lease_build(&movie);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {movie_lease_reserve, NULL, 64, movie_lease_global};
    snprintf(data, sizeof(data), "%s/movie-chain-data", root);
    snprintf(map_x, sizeof(map_x), "%s/movie-chain-x", root);
    snprintf(map_b, sizeof(map_b), "%s/movie-chain-b", root);
    snprintf(base, sizeof(base), "%s/movie-chain-base", root);
    create("movie-chain-base/sound", NULL);
    create("movie-chain-data/overrides", NULL);
    create("movie-chain-x/overrides/sound-x/package.json", "{\"id\":\"sound-x\",\"name\":\"Sound X\"}");
    create("movie-chain-x/overrides/sound-x/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("movie-chain-x/overrides/sound-x/assets/sound/soundbanks/pc/moviebank_x.bnk", "moviebank_x.bnk", 77);
    /* Map B is the movie lease fixture's first map; a bank serves its sound. */
    create("movie-chain-b/overrides/outer/package.json", "{\"id\":\"movies\",\"name\":\"Movies\",\"strings\":{\"french\":{\"notice\":\"ancestor\"}},"
        "\"hud\":{\"weapons\":{\"retained-a\":{\"ammo_display\":\"weapon\"}}}}");
    create("movie-chain-b/overrides/outer/assets/generated/decls/material/late/fx.decl", "{ color {1,0,0} }");
    create("movie-chain-b/overrides/outer/assets/generated/decls/sound/play_computed.decl", "{ edit = {} }");
    create("movie-chain-b/overrides/outer/selected/package.json",
        "{\"id\":\"same\",\"name\":\"Selected movie\",\"strings\":{\"english\":{\"notice\":\"A\",\"empty\":\"\"}}}");
    create("movie-chain-b/overrides/outer/selected/assets/generated/decls/entitydef/movie/screen.decl",
        "{ class = \"Entity\"; edit = { gui = \"swf/stock/panel.swf\"; } }");
    create("movie-chain-b/overrides/outer/sibling/package.json",
        "{\"id\":\"sibling\",\"name\":\"Sibling movie\",\"strings\":{\"english\":{\"sibling\":\"foreign A\"}}}");
    create("movie-chain-b/overrides/outer/sibling/assets/generated/decls/material/sibling/only.decl", "{ color {0,0,1} }");
    create("movie-chain-b/overrides/outer/sibling/assets/generated/decls/entitydef/movie/sibling.decl",
        "{ class = \"Entity\"; edit = { gui = \"swf/stock/panel.swf\"; } }");
    create("movie-chain-b/overrides/unrelated/package.json", "{\"id\":\"unrelated\",\"name\":\"Unrelated\"}");
    create("movie-chain-b/overrides/unrelated/assets/generated/decls/entitydef/movie/unrelated.decl",
        "{ class = \"Entity\"; edit = { health = 88; } }");
    write_bank("movie-chain-b/overrides/outer/assets/sound/soundbanks/pc/moviebank_b.bnk", "moviebank_b.bnk", 1984956495u);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(movie_lease_baseline, &movie);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_constructor_requests(consumer_requests, NULL);
    sh_package_runtime_test_strings(&strings);
    sh_package_runtime_test_atlas(movie_lease_atlas, &atlas_seen);
    roots = sh_package_runtime_test_audio_roots();
    /* Map X and a copy taken from it, then X is left with its copy held. */
    plan = sh_package_runtime_prepare_map(data, map_x, error, sizeof(error));
    if (!plan) fprintf(stderr, "movie chain map X: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1); program_x = listed[0];
    document_x = chain_capture(plan, json, NULL); CHECK(document_x);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Map B, and its movie begun from the view that also serves X. */
    plan = movie_lease_prepare(data, map_b, error, sizeof(error));
    if (!plan) fprintf(stderr, "movie chain map B: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    CHECK(count == 2);
    for (size_t i = 0; i < count && i < 8; i++) if (listed[i] != program_x) program_b = listed[i];
    CHECK(program_x && program_b && program_b != program_x);
    CHECK(movie_lease_capture(plan, &expected, 0, 1)); if (!expected.owner[0]) goto done;
    CHECK(sh_package_runtime_movie_begin(expected.owner, &movie_hold) == 1);
    /* X's copy is released and B is left while B's movie still plays. */
    sh_package_occurrence_release(document_x); document_x = NULL;
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_serves("file", expected.owner) && chain_serves("material", expected.material));
    count = chain_programs(listed, 8);
    if (count != 1) fprintf(stderr, "movie chain: %zu programs while only B's movie is live\n", count);
    CHECK(count == 1 && listed[0] == program_b);
    sh_package_runtime_movie_end_instance(&movie_hold); ended = 1;
    sh_package_runtime_movie_release(&movie_hold);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!chain_serves("file", expected.owner));
    CHECK(!sh_package_runtime_test_movie_records(&providers, &live) && !providers && !live);
    CHECK(chain_programs(listed, 8) == 0 && sh_package_runtime_test_audio_roots() == roots);
done:
    if (!ended) sh_package_runtime_movie_end_instance(&movie_hold);
    sh_package_runtime_movie_release(&movie_hold);
    sh_package_occurrence_release(document_x); sh_package_map_plan_free(plan);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_constructor_requests(NULL, NULL);
    sh_package_runtime_test_strings(NULL); sh_package_runtime_test_atlas(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    while (f.count) free(f.regions[--f.count].address);
    cleanup_resources(data);
}
/* A copy whose own root has no audio program stays on the view it was taken
 * from, which holds its complete closure, but later views take from it only the
 * roots that serve what they keep. Map X's copy is held while map A, compiled
 * without audio, opens, so A's view also serves X. Once X's copy is released,
 * X's program retires although A's copy still holds A's view, A's entity is
 * still served after A is left, and releasing A's copy releases X. */
static void test_audio_fallback_chain(void)
{
    const char *json_x = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    const char *json_a = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"plain/root\"}]}";
    char data[MAX_PATH], map_x[MAX_PATH], map_a[MAX_PATH], base[MAX_PATH], error[2048], entity[1024] = "";
    sh_package_audio_program *listed[8] = {0};
    sh_package_occurrence_snapshot *document_x = NULL, *document_a = NULL;
    sh_package_map_plan *plan = NULL, *plan_a = NULL; int fail = 0; size_t count, roots;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/fallback-chain-library", root);
    snprintf(map_x, sizeof(map_x), "%s/fallback-chain-x", root);
    snprintf(map_a, sizeof(map_a), "%s/fallback-chain-a", root);
    snprintf(base, sizeof(base), "%s/fallback-chain-base", root);
    create("fallback-chain-base/sound", NULL);
    create("fallback-chain-library/overrides", NULL);
    create("fallback-chain-x/overrides/sound-x/package.json", "{\"id\":\"sound-x\",\"name\":\"Sound X\"}");
    create("fallback-chain-x/overrides/sound-x/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("fallback-chain-x/overrides/sound-x/assets/sound/soundbanks/pc/fallbackbank_x.bnk", "fallbackbank_x.bnk", 77);
    create("fallback-chain-a/overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("fallback-chain-a/overrides/plain/assets/generated/decls/entitydef/plain/root.decl", "{ class = \"Entity\"; edit = { health = 5; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    roots = sh_package_runtime_test_audio_roots();
    /* Map A compiles without native audio originals, so its root has no program. */
    plan_a = sh_package_runtime_prepare_map(data, map_a, error, sizeof(error));
    if (!plan_a) fprintf(stderr, "fallback chain map A: %s\n", error);
    CHECK(plan_a); if (!plan_a) goto done;
    const sh_compiled_resource *plain = historical_resource(sh_package_map_plan_compilation(plan_a),
        "generated/decls/entitydef/plain/root.decl");
    CHECK(plain && plain->native_name); if (!plain || !plain->native_name) goto done;
    strcpy_s(entity, sizeof(entity), plain->native_name);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    /* Map X and a copy taken from it, then X is left with its copy held. */
    plan = sh_package_runtime_prepare_map(data, map_x, error, sizeof(error));
    if (!plan) fprintf(stderr, "fallback chain map X: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1);
    document_x = chain_capture(plan, json_x, NULL); CHECK(document_x);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Map A, and a copy of its entity from the view that also serves X. */
    plan = plan_a; plan_a = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1);
    document_a = chain_capture(plan, json_a, NULL); CHECK(document_a);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* X's copy is released while A's copy holds the view that served X. */
    sh_package_occurrence_release(document_x); document_x = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    if (count) fprintf(stderr, "fallback chain: %zu programs after releasing X's copy\n", count);
    CHECK(count == 0);
    /* A is left; its copy still has its entity and closure. */
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_serves("entitydef", entity) && chain_programs(listed, 8) == 0);
    sh_package_occurrence_release(document_a); document_a = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!chain_serves("entitydef", entity));
    CHECK(chain_programs(listed, 8) == 0 && sh_package_runtime_test_audio_roots() == roots);
done:
    sh_package_occurrence_release(document_x); sh_package_occurrence_release(document_a);
    sh_package_map_plan_free(plan); sh_package_map_plan_free(plan_a);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* A copy left on its view keeps the programs that serve its whole closure. Map
 * A, compiled without audio, has an entity that inherits from a package map X
 * ships too; X's copy is held while A opens, so X's program is the one in A's
 * view that serves that package. A copy of A's entity stays on A's view, and
 * after X's own copy is released X's program stays for it until it too is
 * released. */
static void test_audio_fallback_closure(void)
{
    const char *json_x = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"audio/root\"}]}";
    const char *json_a = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"plain/child\"}]}";
    char data[MAX_PATH], map_x[MAX_PATH], map_a[MAX_PATH], base[MAX_PATH], error[2048], entity[1024] = "";
    sh_package_audio_program *listed[8] = {0}, *program_x = NULL;
    sh_package_occurrence_snapshot *document_x = NULL, *document_a = NULL;
    sh_package_map_plan *plan = NULL, *plan_a = NULL; int fail = 0; size_t count, roots;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    snprintf(data, sizeof(data), "%s/closure-chain-library", root);
    snprintf(map_x, sizeof(map_x), "%s/closure-chain-x", root);
    snprintf(map_a, sizeof(map_a), "%s/closure-chain-a", root);
    snprintf(base, sizeof(base), "%s/closure-chain-base", root);
    create("closure-chain-base/sound", NULL);
    create("closure-chain-library/overrides", NULL);
    create("closure-chain-x/overrides/sound-x/package.json", "{\"id\":\"sound-x\",\"name\":\"Sound X\"}");
    create("closure-chain-x/overrides/sound-x/assets/generated/decls/entitydef/audio/root.decl", "{ class = \"Entity\"; edit = { health = 7; } }");
    write_bank("closure-chain-x/overrides/sound-x/assets/sound/soundbanks/pc/closurebank_x.bnk", "closurebank_x.bnk", 77);
    for (int i = 0; i < 2; i++) {
        char path[512];
        snprintf(path, sizeof(path), "closure-chain-%c/overrides/shared/package.json", i ? 'a' : 'x');
        create(path, "{\"id\":\"shared\",\"name\":\"Shared\"}");
        snprintf(path, sizeof(path), "closure-chain-%c/overrides/shared/assets/generated/decls/entitydef/shared/base.decl", i ? 'a' : 'x');
        create(path, "{ class = \"Entity\"; edit = { health = 3; } }");
    }
    create("closure-chain-a/overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("closure-chain-a/overrides/plain/assets/generated/decls/entitydef/plain/child.decl",
        "{ inherit = \"shared/base\"; edit = { health = 5; } }");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    roots = sh_package_runtime_test_audio_roots();
    /* Map A compiles without native audio originals, so its root has no program. */
    plan_a = sh_package_runtime_prepare_map(data, map_a, error, sizeof(error));
    if (!plan_a) fprintf(stderr, "closure chain map A: %s\n", error);
    CHECK(plan_a); if (!plan_a) goto done;
    const sh_compiled_resource *child = historical_resource(sh_package_map_plan_compilation(plan_a),
        "generated/decls/entitydef/plain/child.decl");
    CHECK(child && child->native_name && child->bound_dependency_count);
    if (!child || !child->native_name || !child->bound_dependency_count) goto done;
    strcpy_s(entity, sizeof(entity), child->native_name);
    sh_package_runtime_test_audio_originals(base, L"English(US)");
    /* Map X and a copy taken from it, then X is left with its copy held. */
    plan = sh_package_runtime_prepare_map(data, map_x, error, sizeof(error));
    if (!plan) fprintf(stderr, "closure chain map X: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_programs(listed, 8) == 1); program_x = listed[0];
    document_x = chain_capture(plan, json_x, NULL); CHECK(document_x);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Map A, and a copy of its inheriting entity from the view that also serves X. */
    plan = plan_a; plan_a = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    document_a = chain_capture(plan, json_a, NULL); CHECK(document_a);
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* X's own copy is released; A's copy still keeps the package X serves. */
    sh_package_occurrence_release(document_x); document_x = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    count = chain_programs(listed, 8);
    if (count != 1) fprintf(stderr, "closure chain: %zu programs while A's copy keeps X's package\n", count);
    CHECK(count == 1 && listed[0] == program_x);
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(chain_serves("entitydef", entity));
    count = chain_programs(listed, 8);
    CHECK(count == 1 && listed[0] == program_x);
    sh_package_occurrence_release(document_a); document_a = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!chain_serves("entitydef", entity));
    CHECK(chain_programs(listed, 8) == 0 && sh_package_runtime_test_audio_roots() == roots);
done:
    sh_package_occurrence_release(document_x); sh_package_occurrence_release(document_a);
    sh_package_map_plan_free(plan); sh_package_map_plan_free(plan_a);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_audio_originals(NULL, NULL);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* Library pruning keeps an unchanged SnapMap movie native only when that is
 * its whole behavior: every captured request names its own unchanged identity.
 * It then needs no binding or lifetime record, so inventory activation succeeds
 * and the movie begins as stock. A scope that changes a material the movie
 * draws, adds a late request or localizes a string keeps a served movie with
 * its binding, the changed target and a lifetime record. */
static void native_movie_build(movie_lease_bytes *m, const char *font)
{
    m->used = 0; movie_lease_word(m, 0x42535717); movie_lease_zero(m, 22);
    movie_lease_zero(m, 2);
    for (unsigned i = 0; i < 4; i++) movie_lease_word(m, 0);
    movie_lease_word(m, 3);
    movie_lease_word(m, 1); movie_lease_text(m, "GUI/Paint"); movie_lease_zero(m, 32);
    movie_lease_word(m, 1); movie_lease_text(m, ".atlas"); movie_lease_zero(m, 32);
    movie_lease_word(m, 5); movie_lease_text(m, font); movie_lease_zero(m, 6); movie_lease_word(m, 0);
}
typedef struct native_movie_seen { char names[8][256]; size_t count; } native_movie_seen;
static void native_movie_served(native_movie_seen *out)
{
    const sh_package_compilation *current = sh_package_runtime_acquire();
    out->count = 0;
    for (size_t i = 0; current && i < current->resource_count; i++)
        for (size_t n = 0; n < sh_package_resource_native_count(current->resources + i); n++) {
            sh_package_native_identity id = sh_package_resource_native_at(current->resources + i, n);
            if (!id.type || !id.name || !sh_package_movie_owner(id.type, id.name)) continue;
            CHECK(out->count < 8);
            if (out->count < 8) strcpy_s(out->names[out->count++], sizeof(out->names[0]), id.name);
        }
    sh_package_runtime_release();
}
/* Each served movie must begin with its lifetime record. Returns which changes
 * the bound plans route: 1 late material, 2 late sound, 4 string, 8 drawn
 * material, each to its served private target or localized mapping. */
static unsigned native_movie_records(const char *label)
{
    native_movie_seen seen; unsigned routed = 0;
    native_movie_served(&seen);
    for (size_t i = 0; i < seen.count; i++) {
        sh_package_movie_hold hold = {0};
        int begun = sh_package_runtime_movie_begin(seen.names[i], &hold);
        fprintf(stderr, "native movie %s: %s begins %d with %zu row(s)\n", label, seen.names[i], begun,
            hold.binding ? hold.binding->count : 0);
        CHECK(begun == 1 && hold.binding && hold.token);
        const sh_package_compilation *current = sh_package_runtime_acquire();
        for (size_t t = 0; begun == 1 && hold.binding && t < hold.binding->count; t++) {
            const sh_package_consumer_target *row = hold.binding->targets + t;
            const sh_compiled_resource *target = movie_lease_named(current, row->family, row->name);
            if (!strcmp(row->slot, "request/material/gui/paint") && target && target->body &&
                strstr((const char *)target->body, "0,1,0")) routed |= 1;
            if (!strcmp(row->slot, "movie/material/guisolid") && target && target->body &&
                strstr((const char *)target->body, "0,0,0")) routed |= 8;
            if (!strcmp(row->slot, "request/sound/play_computed") && target) routed |= 2;
            if (!strcmp(row->family, "string") && movie_lease_mapping(current, row->name)) routed |= 4;
        }
        sh_package_runtime_release();
        if (begun == 1) sh_package_runtime_movie_end_instance(&hold);
        sh_package_runtime_movie_release(&hold);
    }
    return routed;
}
static void native_movie_stock(const char *stock)
{
    sh_package_movie_hold hold = {0}; sh_package_consumer_hold binding = {0};
    unsigned char *served = NULL; size_t length = 0;
    CHECK(sh_package_runtime_read(stock, &served, &length) == 0); free(served);
    CHECK(sh_package_runtime_consumer_acquire("file", stock, &binding) == 0);
    sh_package_runtime_consumer_release(&binding);
    CHECK(sh_package_runtime_movie_begin(stock, &hold) == 0 && !hold.binding && !hold.token);
    sh_package_runtime_movie_release(&hold);
}
/* Captured movies are decided after every other resource. Only an unchanged
 * movie whose valid plan requests its own identities, none served, stays
 * native and loses its binding. A changed target, localized or redirected
 * row keeps the movie published; a changed movie, or one the compilation never
 * held, keeps its binding too. Other consumers are untouched. */
static void native_movie_resource(sh_compiled_resource *r, const char *path, const char *type,
    const char *name, int original)
{
    r->engine_path = _strdup(path); r->owns_engine_path = 1; r->source = SIZE_MAX;
    r->type = _strdup(type); r->name = _strdup(name);
    r->native_original = original; r->baseline_known = 1;
    CHECK(r->engine_path && r->type && r->name);
}
static void test_native_movie_decisions(void)
{
    static const char *const movies[] = {"generated/swf/a/native.bswf", "generated/swf/b/target.bswf",
        "generated/swf/c/localized.bswf", "generated/swf/d/redirect.bswf", "generated/swf/e/changed.bswf",
        "generated/swf/f/absent.bswf"};
    char error[512]; size_t counts[2] = {9, 9};
    sh_package_consumer_target native_rows[] = {{"movie/font/title", "font", "title"},
        {"movie/material/a/stock", "material", "a/stock"}};
    sh_package_consumer_target target_rows[] = {{"movie/material/b/paint", "material", "b/paint"}};
    sh_package_consumer_target localized_rows[] = {{"movie/string/#str_notice", "string", "#str_notice"}};
    sh_package_consumer_target redirect_rows[] = {{"request/material/a/stock", "material", "private/stock"}};
    sh_package_consumer_target font_rows[] = {{"request/material/fonts/title/64_df.tga", "material",
        "fonts/title/64_df.tga"}};
    sh_package_consumer rows[] = {{"file", movies[0], native_rows, 2}, {"file", movies[1], target_rows, 1},
        {"file", movies[2], localized_rows, 1}, {"file", movies[3], redirect_rows, 1},
        {"file", movies[4], native_rows, 2}, {"file", movies[5], native_rows, 2}, {"font", "title", font_rows, 1}};
    sh_package_consumers *consumers = sh_package_consumers_create(rows, 7, error, sizeof(error));
    sh_package_compilation *compiled = calloc(1, sizeof(*compiled));
    if (compiled) compiled->resources = calloc(8, sizeof(*compiled->resources));
    CHECK(consumers && compiled && compiled->resources);
    if (!consumers || !compiled || !compiled->resources) {
        sh_package_consumers_free(consumers); if (compiled) free(compiled->resources); free(compiled); return;
    }
    compiled->resource_count = 8;
    /* Ordered by engine path; the paint material changed, so it stays served. */
    native_movie_resource(compiled->resources, "fonts/title/64_df.dat", "font", "title", 1);
    native_movie_resource(compiled->resources + 1, "generated/decls/material/a/stock.decl", "material", "a/stock", 1);
    native_movie_resource(compiled->resources + 2, "generated/decls/material/b/paint.decl", "material", "b/paint", 0);
    for (size_t i = 0; i < 5; i++) native_movie_resource(compiled->resources + 3 + i, movies[i], "file", movies[i], i < 4);
    CHECK(sh_package_runtime_test_prune_native(compiled, &consumers, counts, error, sizeof(error)) == 3);
    CHECK(counts[0] == 1 && counts[1] == 3 && compiled->resource_count == 5);
    CHECK(!sh_package_compilation_find(compiled, movies[0]));
    for (size_t i = 1; i < 5; i++) CHECK(sh_package_compilation_find(compiled, movies[i]) != NULL);
    CHECK(sh_package_compilation_find(compiled, "generated/decls/material/b/paint.decl") != NULL);
    CHECK(sh_package_consumers_count(consumers) == 6 && !sh_package_consumers_find(consumers, "file", movies[0]));
    for (size_t i = 1; i < 6; i++) CHECK(sh_package_consumers_find(consumers, "file", movies[i]) != NULL);
    CHECK(sh_package_consumers_find(consumers, "font", "title") != NULL);
    sh_package_consumers_free(consumers); sh_package_compilation_free(compiled);
}
static void test_native_movie_pruning(void)
{
    static const char stock[] = "generated/swf/stock/panel.bswf";
    static const char screen[] = "{ class = \"Entity\"; edit = { gui = \"swf/stock/panel.swf\"; } }";
    char data[MAX_PATH], error[1024];
    movie_lease_bytes movie = {0}; unsigned atlas_seen = 0;
    admission_fixture f; movie_lease_reflection(&f); native_movie_build(&movie, "Title");
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {movie_lease_reserve, NULL, 64, movie_lease_global};
    snprintf(data, sizeof(data), "%s/native-movie", root);
    create("native-movie/overrides/native/package.json", "{\"id\":\"native-movie\",\"name\":\"Native movie\"}");
    create("native-movie/overrides/native/assets/generated/decls/entitydef/movie/native_screen.decl", screen);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(movie_lease_baseline, &movie);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_constructor_requests(consumer_requests, NULL);
    sh_package_runtime_test_strings(&strings); sh_package_runtime_test_atlas(movie_lease_atlas, &atlas_seen);
    sh_package_runtime_test_installed_links(1);
    int ready = sh_package_runtime_refresh(data);
    if (!ready) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "native movie: %s\n", error); }
    CHECK(ready);
    native_movie_stock(stock);
    CHECK(native_movie_records("unchanged") == 0);
    /* The same stock movie drawing a material its scope changes. */
    create("native-movie/overrides/paint/package.json", "{\"id\":\"paint-movie\",\"name\":\"Paint movie\"}");
    create("native-movie/overrides/paint/assets/generated/decls/material/gui/paint.decl", "{ color {0,1,0} }");
    create("native-movie/overrides/paint/assets/generated/decls/entitydef/movie/paint_screen.decl", screen);
    ready = sh_package_runtime_refresh(data);
    if (!ready) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "native movie paint: %s\n", error); }
    CHECK(ready && native_movie_records("paint") == 1);
    /* A late request in its scope, then a localized string. */
    create("native-movie/overrides/late/package.json", "{\"id\":\"late-movie\",\"name\":\"Late movie\"}");
    create("native-movie/overrides/late/assets/generated/decls/sound/play_computed.decl", "{ edit = { } }");
    create("native-movie/overrides/late/assets/generated/decls/entitydef/movie/late_screen.decl", screen);
    ready = sh_package_runtime_refresh(data);
    if (!ready) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "native movie late: %s\n", error); }
    CHECK(ready && native_movie_records("late") == 3);
    create("native-movie/overrides/notice/package.json",
        "{\"id\":\"notice-movie\",\"name\":\"Notice movie\",\"strings\":{\"english\":{\"notice\":\"N\"}}}");
    create("native-movie/overrides/notice/assets/generated/decls/entitydef/movie/notice_screen.decl", screen);
    ready = sh_package_runtime_refresh(data);
    if (!ready) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "native movie notice: %s\n", error); }
    CHECK(ready && native_movie_records("notice") == 7);
    /* A material the movie itself draws. */
    create("native-movie/overrides/solid/package.json", "{\"id\":\"solid-movie\",\"name\":\"Solid movie\"}");
    create("native-movie/overrides/solid/assets/generated/decls/material/guisolid.decl", "{ color {0,0,0} }");
    create("native-movie/overrides/solid/assets/generated/decls/entitydef/movie/solid_screen.decl", screen);
    ready = sh_package_runtime_refresh(data);
    if (!ready) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "native movie solid: %s\n", error); }
    CHECK(ready && native_movie_records("solid") == 15);
    /* The package that only uses the movie still leaves it native. */
    sh_package_movie_hold hold = {0};
    CHECK(sh_package_runtime_movie_begin(stock, &hold) == 0 && !hold.token);
    sh_package_runtime_movie_release(&hold);
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_constructor_requests(NULL, NULL);
    sh_package_runtime_test_strings(NULL); sh_package_runtime_test_atlas(NULL, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    while (f.count) free(f.regions[--f.count].address);
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
/* The native-compare stand-in's controls and route record (defined below). */
static int g_native_compares;
static sh_native_eval_status g_native_status;
static size_t g_native_layers;
static char g_native_layer[256], g_native_layer_text[512];
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
    char data[MAX_PATH], map[MAX_PATH], stock[MAX_PATH], keep[MAX_PATH], setting[MAX_PATH], rows[MAX_PATH], copy[MAX_PATH], served[MAX_PATH];
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
    create("inline-served/overrides/changes/package.json", "{\"id\":\"changes\",\"name\":\"Changes base\"}");
    create("inline-served/overrides/changes/assets/generated/decls/entitydef/inlinestock/base.decl",
        "{ class = \"Entity\"; editorVars { placeable = true; } edit = { health = 7; target = \"\"; } }");
    snprintf(served, sizeof(served), "%s/inline-served", root);
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
    /* Per-definition source authority. The engine serves nothing of this
     * document (an empty library), so the target chain's package definition is
     * prepared privately with its authored source, never looked up by name. */
    g_native_compares = 0;
    result = inline_save(data, map, native, &derived, &count, &ownership, &report);
    CHECK(result == 1 && g_native_compares == 1 && g_native_layers == 1 &&
        strstr(g_native_layer_text, "health = 40") && strstr(g_native_layer_text, "inlinestock/base"));
    inline_reset(&derived, &report, &ownership, &count);
    /* Every non-EQUAL verdict keeps the complete package as a product gap with
     * its own outcome; none is a native limit. */
    {
        const sh_native_eval_status refused[] = {SH_NATIVE_EVAL_UNEQUAL, SH_NATIVE_EVAL_UNSUPPORTED,
            SH_NATIVE_EVAL_FAILED, SH_NATIVE_EVAL_UNAVAILABLE};
        const char *outcome[] = {"native evaluation found a different state", "native evaluation does not cover a value",
            "native evaluation failed", "native evaluation is unavailable"};
        for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); i++) {
            g_native_status = refused[i];
            result = inline_save(data, map, native, &derived, &count, &ownership, &report);
            CHECK(result == 0 && !derived && report && strstr(report, "kept (product gap)") && strstr(report, outcome[i]) &&
                !strstr(report, "native limit"));
            inline_reset(&derived, &report, &ownership, &count);
        }
        g_native_status = SH_NATIVE_EVAL_EQUAL;
    }
    /* Installing the same package in the library does not make the engine
     * serve this map's own definition: the serving library has no compiled
     * resource at the document's private path, so that definition is still
     * prepared privately rather than trusted by its authored name. */
    CHECK(sh_package_runtime_refresh(map));
    g_native_layers = 99;
    result = inline_save(map, map, native, &derived, &count, &ownership, &report);
    if (report && result != 1) fprintf(stderr, "%s", report);
    if (result != 1 || g_native_layers != 1)
        fprintf(stderr, "inline served library: result %d, %zu layers, %s\n", result, g_native_layers, g_native_layer);
    CHECK(result == 1 && g_native_layers == 1 && strncmp(g_native_layer, "smp/", 4) == 0);
    inline_reset(&derived, &report, &ownership, &count);
    /* An engine that answers the stock base with a changed copy would resolve
     * the candidate's inherit to that copy: no evaluation, package kept. */
    CHECK(sh_package_runtime_refresh(served));
    g_native_compares = 0;
    result = inline_save(data, map, native, &derived, &count, &ownership, &report);
    if (report && result != 0) fprintf(stderr, "%s", report);
    CHECK(result == 0 && !derived && g_native_compares == 0 && report && strstr(report, "kept (product gap)") &&
        strstr(report, "the engine serves a changed copy of stock 'inlinestock/base'"));
    inline_reset(&derived, &report, &ownership, &count);
    CHECK(sh_package_runtime_refresh(data));
    /* Without the shipped roster, placement is unproven: nothing converts. */
    sh_package_runtime_test_editor_rows(NULL, 0);
    result = inline_save(data, map, native, &derived, &count, &ownership, &report);
    CHECK(result == 0 && !derived && report && strstr(report, "kept (product gap): SnapMap's shipped editor entries could not be read"));
    inline_reset(&derived, &report, &ownership, &count);

    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(map); cleanup_resources(stock);
    cleanup_resources(keep); cleanup_resources(setting); cleanup_resources(rows); cleanup_resources(copy); cleanup_resources(served);
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
/* Exact editor rows through the production rebind, native conversion,
 * carrier write and load capture: one row per object, never ownership. */
static const char inline_row_tough[] = "1111111111111111111111111111111111111111111111111111111111111111";
static const char inline_row_fraction[] = "2222222222222222222222222222222222222222222222222222222222222222";
static int inline_row_is(const sh_package_provenance *value, const char *owner, const char *expected)
{
    const char *row = NULL; int state = sh_package_provenance_row_get(value, owner, &row);
    return expected ? state == 1 && !strcmp(row, expected) : state == 0;
}
static int inline_owners(const sh_package_provenance *value, const char *only)
{
    for (size_t i = 0; i < value->use_count; i++)
        if (value->uses[i].kind == SH_PACKAGE_USE_OBJECT && (!only || strcmp(value->uses[i].owner, only))) return 0;
    return 1;
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
            CHECK(sh_package_provenance_row_set(&saved, "entity/0", inline_row_tough) == 1);
            CHECK(!mixed || sh_package_provenance_row_set(&saved, "entity/1", inline_row_fraction) == 1);
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
            if (modern && derived && ownership) {
                /* The rebind kept each owner's row; the conversion's own ownership
                 * is uniquely held, so the save-time carry succeeds; the converted
                 * object leaves carrier-free and the packaged one keeps entity/1.
                 * Written, reread and captured as a load (under the map's plan, as
                 * an activated load is), the row returns to the same owner token. */
                sh_package_provenance rebound = {0}, reread = {0};
                sh_package_occurrence_snapshot *loaded = NULL;
                char *carrier = NULL, *native = NULL, *captured = NULL;
                size_t carrier_length = 0, native_length = 0, captured_length = 0; int carried = 0;
                const sh_package_provenance *owned = sh_package_occurrence_provenance(ownership);
                CHECK(sh_map_provenance_read(bound, bound_length, &rebound, &carried, error, sizeof(error)) && carried);
                CHECK(inline_row_is(&rebound, "entity/0", inline_row_tough));
                CHECK(inline_row_is(&rebound, "entity/1", mixed ? inline_row_fraction : NULL));
                CHECK(owned && sh_package_occurrence_carry_rows(ownership, &saved));
                CHECK(owned && inline_owners(owned, mixed ? "entity/1" : "") && inline_row_is(owned, "entity/0", NULL));
                CHECK(owned && inline_row_is(owned, "entity/1", mixed ? inline_row_fraction : NULL));
                carrier = owned ? sh_map_provenance_write(derived, derived_length, owned, &carrier_length,
                    error, sizeof(error)) : NULL;
                CHECK(carrier || !mixed);
                if (carrier) {
                    carried = 0;
                    CHECK(sh_map_provenance_read(carrier, carrier_length, &reread, &carried, error, sizeof(error)));
                    CHECK(carried == mixed && inline_row_is(&reread, "entity/0", NULL));
                    CHECK(inline_row_is(&reread, "entity/1", mixed ? inline_row_fraction : NULL));
                    native = sh_map_provenance_strip(carrier, carrier_length, &native_length, error, sizeof(error));
                    CHECK(native);
                    captured = native ? sh_package_runtime_capture_document(plan, carried ? &reread : NULL, NULL, 0,
                        native, native_length, &captured_length, &loaded, NULL, NULL, error, sizeof(error)) : NULL;
                    if (!captured) fprintf(stderr, "inline rows load capture %d: %s\n", mixed, error);
                    CHECK(captured && loaded);
                    CHECK(loaded && (!carried || sh_package_occurrence_carry_rows(loaded, &reread)));
                    CHECK(loaded && inline_owners(sh_package_occurrence_provenance(loaded), mixed ? "entity/1" : ""));
                    CHECK(loaded && inline_row_is(sh_package_occurrence_provenance(loaded), "entity/1",
                        mixed ? inline_row_fraction : NULL));
                    free(captured); if (native) HeapFree(GetProcessHeap(), 0, native);
                    HeapFree(GetProcessHeap(), 0, carrier);
                }
                sh_package_occurrence_release(loaded);
                sh_package_provenance_free(&rebound); sh_package_provenance_free(&reread);
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
    /* An unchanged stock definition stays native: composition only re-sorts
     * its editorVars, which is the same block, so no canonical copy is served. */
    CHECK(library_served("generated/decls/entitydef/inlinestock/base.decl") == 0);
    /* The library serves stock editor entries under their own names through its
     * baseline-palette reverse roots, which need the game catalog this fixture
     * lacks; test_native_inline_placement_provenance covers that compiled shape. */
    library_served("generated/decls/snapeditorentitydef/inlinestock/base_row.decl");
    /* The package use converts to native map data over the stock definition. */
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
    /* The live shape (D8, 2026-09-29): the stock entry linked privately for the
     * package's own context, because a shared editor declaration it references
     * differs there. The unowned copy still stands for the shipped entry; the
     * same copy owned by a package is a package change and is refused. */
    sh_compiled_resource context[3];
    context[0] = provenance_resource("generated/decls/entitydef/smp/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }", "generated/decls/entitydef/inline/tough.decl", 0, 0);
    context[1] = provenance_resource("generated/decls/snapeditorentitydef/smp/stock_context.decl",
        "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; } }",
        "generated/decls/snapeditorentitydef/inlinestock/base_row.decl", 1, SIZE_MAX);
    context[2] = provenance_resource("generated/decls/snapeditorentitydef/smp/tough_row.decl",
        "{ inherit = \"smp/stock_context\"; edit = { entityDef = \"smp/tough\"; } }",
        "generated/decls/snapeditorentitydef/inline/tough_row.decl", 0, 0);
    compiled.resources = context; compiled.resource_count = 3;
    placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    if (placed != 1) fprintf(stderr, "placement provenance (unowned context copy): %d %s\n", placed, reason);
    CHECK(placed == 1);
    CHECK(sh_package_owners_add(&context[1].owners, 1));
    placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    if (placed != 0) fprintf(stderr, "placement provenance (owned context copy): %d %s\n", placed, reason);
    CHECK(placed == 0 && !engine && strstr(reason, "not a plain copy"));
    for (size_t i = 0; i < 3; i++) { sh_package_owners_free(&context[i].owners); sh_package_owners_free(&context[i].gameplay_owners); }
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
}
/* A toybox label alias is presentation only (package_library.c): the served
 * text of a plain package entry gains a generated displayNameTag, but native
 * conversion reads the authored body, so the entity still places natively and
 * saves carrier-free. The control puts the same generated member into the body
 * itself, the path this separation prevents: the entry is no longer a plain
 * copy and conversion is refused. */
static void test_native_inline_placement_label_alias(void)
{
    sh_package_sources sources = {0};
    sh_compiled_resource resources[3];
    sh_package_compilation compiled = {0};
    char reason[512]; int engine = 0;
    const char *plain = "{ inherit = \"inlinestock/base_row\"; edit = { entityDef = \"smp/tough\"; } }";
    const char *decorated = "{ inherit = \"inlinestock/base_row\"; edit = { entityDef = \"smp/tough\"; "
        "displayNameTag = \"#str_smp_loc_0123\"; } }";
    resources[0] = provenance_resource("generated/decls/entitydef/smp/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }", "generated/decls/entitydef/inline/tough.decl", 0, 0);
    resources[1] = provenance_resource("generated/decls/snapeditorentitydef/inlinestock/base_row.decl",
        "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; } }", NULL, 1, SIZE_MAX);
    resources[2] = provenance_resource("generated/decls/snapeditorentitydef/smp/tough_row.decl", plain,
        "generated/decls/snapeditorentitydef/inline/tough_row.decl", 0, 0);
    resources[2].presented = (unsigned char *)decorated; resources[2].presented_length = strlen(decorated);
    compiled.sources = &sources; compiled.resources = resources; compiled.resource_count = 3;
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(inline_baseline, NULL);
    sh_package_runtime_test_editor_rows(inline_editor_rows, sizeof(inline_editor_rows) / sizeof(*inline_editor_rows));
    int placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    if (placed != 1) fprintf(stderr, "placement with a label alias: %d %s\n", placed, reason);
    CHECK(placed == 1);
    /* Serving reads the presentation; every other reader the authored body. */
    size_t length = 0; char error[256] = "";
    unsigned char *served = sh_package_compilation_read_served(&compiled, resources + 2, SIZE_MAX, &length, error, sizeof(error));
    CHECK(served && length == strlen(decorated) && !memcmp(served, decorated, length)); free(served);
    unsigned char *authored = sh_package_compilation_read(&compiled, resources + 2, SIZE_MAX, &length, error, sizeof(error));
    CHECK(authored && length == strlen(plain) && !memcmp(authored, plain, length)); free(authored);
    /* Control: the generated member in the body itself is refused. */
    resources[2].presented = NULL; resources[2].presented_length = 0;
    resources[2].body = (unsigned char *)decorated; resources[2].body_length = strlen(decorated);
    placed = sh_package_runtime_test_placement(&compiled, "smp/tough", "inlinestock/base", &engine, reason, sizeof(reason));
    CHECK(placed == 0 && !engine && strstr(reason, "not a plain copy"));
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
    int modified;
    uint32_t local_classification;
} registration_activation;
static int registration_admit(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    registration_activation *state = context;
    CHECK(sh_package_runtime_test_key_groups_current());
    CHECK(sh_package_runtime_test_unmodified_type(state->classification) == (!restoring && !state->modified));
    CHECK(!sh_package_runtime_test_exact_type(state->classification));
    CHECK(sh_package_runtime_test_exact_type(state->local_classification));
    for (size_t i = 0; i < changes->count; i++)
        CHECK(changes->items[i].kind == (restoring ? SH_PACKAGE_RESOURCE_REMOVED : SH_PACKAGE_RESOURCE_ADDED));
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail) { snprintf(error, capacity, "registration admission fixture failure"); return 0; }
    return 1;
}
static void test_retained_registration_meaning(int additive, int modified)
{
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], error[2048], request[1024] = "", ai[1024] = "";
    sh_package_map_plan *plan = NULL;
    sh_package_occurrence_snapshot *document = NULL;
    uint32_t local_key = 0, map_key = 0, local_classification = 0;
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
        create(path, i && modified ? "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524292; entityDef = \"unit\"; } } globalAIMods = { num = 1; "
            "item[0] = { aiType = 524288; healthMod = 3; damageMod = 1; } } } }" :
            "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
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
    const sh_compiled_resource *local_conductor = sh_package_compilation_find(local, "generated/decls/entitydef/conductor.decl");
    sh_package_spawn_registry local_rows = {0}; sh_package_key_space numeric_keys = {0};
    CHECK(local_conductor && sh_package_spawn_registry_read((sh_decl_source){(char *)local_conductor->body,
        local_conductor->body_length}, &numeric_keys, &local_rows, error, sizeof(error)) == 1);
    for (size_t i = 0; i < local_rows.count; i++)
        if (!strcmp(local_rows.rows[i].entity, local_ai)) { local_classification = local_rows.rows[i].mask; break; }
    sh_package_spawn_registry_free(&local_rows);
    CHECK(local_classification && sh_package_runtime_test_exact_type(local_classification));
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
        CHECK(!sh_package_runtime_test_exact_type(classification));
        registration_activation state = {classification, 0, 0, 1, modified, local_classification};
        CHECK(sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
            registration_admit, NULL, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
        CHECK(state.calls == 1 && state.recoveries == 1 && sh_package_runtime_ready());
        CHECK(sh_package_runtime_test_key_groups_current() &&
            !sh_package_runtime_test_unmodified_type(classification));
        CHECK(!sh_package_runtime_test_exact_type(classification) &&
            sh_package_runtime_test_exact_type(local_classification));
        state.fail = 0;
        int result = sh_package_runtime_admit_occurrences(plan, document, (sh_package_activation_guard){0},
            registration_admit, NULL, &state, error, sizeof(error));
        if (result != SH_PACKAGE_ADMISSION_READY) fprintf(stderr, "registration admission: %s\n", error);
        CHECK(result == SH_PACKAGE_ADMISSION_READY && state.calls == 2 && state.recoveries == 1);
        CHECK(sh_package_runtime_test_key_groups_current());
        CHECK(sh_package_runtime_test_unmodified_type(classification) == !modified);
        CHECK(!sh_package_runtime_test_exact_type(classification));
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
            CHECK(sh_package_runtime_test_unmodified_type(row->mask) == !modified);
            CHECK(sh_package_runtime_test_exact_type(row->mask) == modified);
            if (modified) {
                size_t first;
                for (first = 0; first < registrations.modifier_count; first++)
                    if (registrations.modifiers[first].mask == row->mask) break;
                CHECK(first < registrations.modifier_count &&
                    strstr(registrations.modifiers[first].body, "healthMod = 3"));
            }
        }
        if (sh_package_key_request_matches(&groups, local_key, row->mask)) {
            local_matches++; CHECK(!strcmp(row->entity, local_ai));
            CHECK(!sh_package_runtime_test_unmodified_type(row->mask));
            CHECK(sh_package_runtime_test_exact_type(row->mask));
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
/* An accepted map's unpublished historical key dictionary already owns its
 * codes while the installed authoring increment is prepared. The active
 * library beside it owns independent keys throughout this admission. */
static void test_prepared_map_install_key_reservations(void)
{
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], map[MAX_PATH], error[2048] = "";
    sh_package_map_plan *plan = NULL;
    sh_package_provenance live = {0};
    sh_package_occurrence_snapshot *document = NULL, *world = NULL;
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_key object_key = {1, 0x1000}, clipboard_key = {2, 0x2000};
    uintptr_t object = object_key.object;
    char request_name[1024] = "", map_ai[1024] = "", installed_ai[1024] = "", local_ai[1024] = "";
    uint32_t map_key = 0, installed_key = 0, local_key = 0, reserved_group = 0;
    sh_package_key_groups pending_groups = {0}; sh_package_key_set members = {0};
    for (size_t i = 0; i < 3; i++) {
        const char *directory = i == 0 ? "keyinstall-data/overrides/local" :
            i == 1 ? "keyinstall-map/overrides/incoming" : "keyinstall-data/overrides/incoming";
        char path[512];
        snprintf(path, sizeof(path), "%s/package.json", directory);
        create(path, i ? "{\"id\":\"incoming\",\"name\":\"Incoming\"}" :
            "{\"id\":\"local\",\"name\":\"Local\"}");
        snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/%s.decl", directory, i ? "incoming" : "local");
        create(path, i ? "{ class = \"Entity\"; edit = { health = 30; } }" :
            "{ class = \"Entity\"; edit = { health = 12000; } }");
        snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/request.decl", directory);
        create(path, "{ class = \"Entity\"; edit = { spawnType = 524288; } }");
        snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/conductor.decl", directory);
        create(path, i ? "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524288; entityDef = \"incoming\"; } } } }" :
            "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524288; entityDef = \"local\"; } } } }");
        if (i == 0) {
            snprintf(data, sizeof(data), "%s/keyinstall-data", root);
            snprintf(map, sizeof(map), "%s/keyinstall-map", root);
            sh_package_runtime_test_dispose();
            sh_package_runtime_test_baseline(lease_registration_baseline, NULL);
            sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
            sh_package_runtime_test_installed_links(1); sh_package_runtime_test_spawn_keys(1);
            CHECK(sh_package_runtime_refresh(data));
            const sh_package_compilation *local = sh_package_runtime_acquire();
            for (size_t j = 0; local && j < local->resource_count; j++) {
                const sh_compiled_resource *r = local->resources + j;
                if (r->body && strstr((char *)r->body, "health = 12000")) strcpy_s(local_ai, sizeof(local_ai), r->native_name);
                if (r->body && strstr((char *)r->body, "spawnType =")) local_key = lease_registration_request(r);
            }
            CHECK(sh_package_provenance_overlay(sh_package_runtime_bindings(), NULL, &live));
            sh_package_runtime_release(); CHECK(*local_ai && local_key);
        } else if (i == 1) {
            plan = sh_package_runtime_prepare_map(data, map, error, sizeof(error));
            if (!plan) fprintf(stderr, "key install map prepare: %s\n", error);
            CHECK(plan); if (!plan) goto done;
        }
    }
    const sh_package_compilation *map_compiled = sh_package_map_plan_compilation(plan);
    for (size_t i = 0; map_compiled && i < map_compiled->resource_count; i++) {
        const sh_compiled_resource *r = map_compiled->resources + i;
        if (r->body && strstr((char *)r->body, "spawnType =")) {
            strcpy_s(request_name, sizeof(request_name), r->native_name); map_key = lease_registration_request(r);
        }
        if (r->body && strstr((char *)r->body, "health = 30")) strcpy_s(map_ai, sizeof(map_ai), r->native_name);
    }
    CHECK(*request_name && *map_ai && map_key);
    /* A pending group's code is held even without a single-key binding for
     * it. Choose the next available marked code so the increment must skip it. */
    const sh_package_provenance *before = sh_package_runtime_test_plan_bindings(plan, 0);
    for (size_t i = 0; before && i < before->binding_count; i++) {
        const sh_package_provenance_binding *b = before->bindings + i;
        if (!strcmp(b->family, SH_PACKAGE_KEY_FAMILY) && sh_package_key_set_count(&members) < 2)
            CHECK(sh_package_key_set_add(&members, (uint32_t)strtoul(b->name, NULL, 10)));
    }
    for (size_t i = 0; i < SH_PACKAGE_KEY_CODES && !reserved_group; i++) {
        uint32_t code = sh_package_key_private(i); int held = 0;
        if (!(code & SH_PACKAGE_KEY_MARKER)) continue;
        for (size_t j = 0; before && j < before->binding_count; j++)
            if (!strcmp(before->bindings[j].family, SH_PACKAGE_KEY_FAMILY) &&
                (uint32_t)strtoul(before->bindings[j].name, NULL, 10) == code) held = 1;
        for (size_t j = 0; j < live.binding_count; j++)
            if (!strcmp(live.bindings[j].family, SH_PACKAGE_KEY_FAMILY) &&
                (uint32_t)strtoul(live.bindings[j].name, NULL, 10) == code) held = 1;
        if (!held) reserved_group = code;
    }
    CHECK(reserved_group && sh_package_key_groups_define(&pending_groups, reserved_group, &members));
    sh_package_runtime_test_plan_key_table(plan, &pending_groups);
    CHECK(sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error)));
    const sh_package_provenance *pending = sh_package_runtime_test_plan_bindings(plan, 0);
    const sh_package_provenance *installed = sh_package_runtime_test_plan_bindings(plan, 1);
    CHECK(pending && installed);
    size_t historical_keys = 0, installed_keys = 0, collisions = 0;
    for (size_t i = 0; pending && i < pending->binding_count; i++) {
        const sh_package_provenance_binding *a = pending->bindings + i;
        if (strcmp(a->family, SH_PACKAGE_KEY_FAMILY)) continue;
        historical_keys++;
        for (size_t j = 0; installed && j < installed->binding_count; j++) {
            const sh_package_provenance_binding *b = installed->bindings + j;
            if (!strcmp(b->family, SH_PACKAGE_KEY_FAMILY) && !strcmp(a->name, b->name)) {
                collisions++;
                fprintf(stderr, "key install collision: historical %s / installed %s = %s\n", a->original, b->original, a->name);
            }
        }
    }
    for (size_t i = 0; installed && i < installed->binding_count; i++) {
        const sh_package_provenance_binding *a = installed->bindings + i;
        if (strcmp(a->family, SH_PACKAGE_KEY_FAMILY)) continue;
        installed_keys++;
        installed_key = (uint32_t)strtoul(a->name, NULL, 10);
        CHECK(installed_key != reserved_group);
        for (size_t j = 0; j < live.binding_count; j++) {
            const sh_package_provenance_binding *b = live.bindings + j;
            CHECK(strcmp(b->family, SH_PACKAGE_KEY_FAMILY) || strcmp(a->name, b->name));
        }
    }
    CHECK(historical_keys == 32 && installed_keys == 1 && !collisions);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &(int){0}));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    int committed = sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error));
    if (!committed) fprintf(stderr, "key install commit: %s\n", error);
    CHECK(committed && inventory_commit_calls == 1);
    char json[2048]; size_t length = 0;
    snprintf(json, sizeof(json), "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", request_name);
    char *captured = sh_package_runtime_capture_document(plan, NULL, NULL, 0, json, strlen(json),
        &length, &document, NULL, NULL, error, sizeof(error));
    if (!captured) fprintf(stderr, "key install capture: %s\n", error);
    CHECK(captured && document); free(captured); if (!document) goto done;
    CHECK(sh_package_occurrence_attach_snapshot(tracker, object_key.namespace_id, &object, 1, document, &world,
        error, sizeof(error)));
    CHECK(sh_package_occurrence_copy(tracker, clipboard_key, object_key));
    sh_package_occurrence_erase_namespace(tracker, object_key.namespace_id);
    sh_package_occurrence_release(document); document = NULL; sh_package_occurrence_release(world); world = NULL;
    sh_package_map_plan_free(plan); plan = NULL;
    int left = sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &(int){0});
    if (!left) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "key install retirement: %s\n", error); }
    CHECK(left && !sh_package_runtime_has_map_provider());
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    for (size_t i = 0; library && i < library->resource_count; i++) {
        const sh_compiled_resource *r = library->resources + i;
        if (r->body && strstr((char *)r->body, "health = 30")) strcpy_s(installed_ai, sizeof(installed_ai), r->native_name);
    }
    sh_package_runtime_release(); CHECK(*installed_ai);
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *conductor = sh_package_compilation_find(current, "generated/decls/entitydef/conductor.decl");
    sh_package_spawn_registry registrations = {0}; sh_package_key_space numeric = {0};
    CHECK(conductor && sh_package_spawn_registry_read((sh_decl_source){(char *)conductor->body, conductor->body_length},
        &numeric, &registrations, error, sizeof(error)) == 1);
    sh_package_runtime_release();
    sh_package_key_groups groups; sh_package_runtime_test_serving_key_table(&groups);
    size_t map_matches = 0, installed_matches = 0, local_matches = 0;
    for (size_t i = 0; i < registrations.count; i++) {
        const sh_package_spawn_row *row = registrations.rows + i;
        if (sh_package_key_request_matches(&groups, map_key, row->mask)) {
            map_matches++; CHECK(!strcmp(row->entity, map_ai));
        }
        if (sh_package_key_request_matches(&groups, installed_key, row->mask)) {
            installed_matches++; CHECK(!strcmp(row->entity, installed_ai));
        }
        if (sh_package_key_request_matches(&groups, local_key, row->mask)) {
            local_matches++; CHECK(!strcmp(row->entity, local_ai));
        }
    }
    CHECK(map_matches == 1 && installed_matches == 1 && local_matches == 1);
    sh_package_key_set decoded; sh_package_key_decode(&groups, reserved_group, &decoded);
    CHECK(sh_package_key_set_equal(&members, &decoded));
    sh_package_spawn_registry_free(&registrations); sh_package_occurrence_erase(tracker, clipboard_key);
done:
    sh_package_provenance_free(&live); sh_package_map_plan_free(plan);
    sh_package_occurrence_release(world); sh_package_occurrence_release(document); sh_package_occurrence_tracker_free(tracker);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_installed_links(0); sh_package_runtime_test_spawn_keys(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL); cleanup_resources(data); cleanup_resources(map);
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
    probe_begin(restoring);
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
    if (finish_probe.enabled) { CHECK(finish_probe.active); finish_probe.finalizations++; }
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
    if (finish_probe.enabled) {
        int finalizations=state.finalizations,accepts=finish_probe.accepts,rejects=finish_probe.rejects;
        finish_probe.refuse_once=1;
        CHECK(sh_package_runtime_admit_occurrences(plans[1],documents[1],(sh_package_activation_guard){0},
            additive_activate,additive_finalize,&state,error,sizeof(error))==SH_PACKAGE_ADMISSION_READY);
        CHECK(finish_probe.active && finish_probe.accepts==accepts && finish_probe.rejects==rejects &&
            state.finalizations==finalizations+1 && !sh_package_runtime_admission_ready());
        /* NEXT admission consumes the previously accepted cleanup, before
         * refusing new work on an unusable provider. It cannot reject the
         * committed decision or repeat the disk finalizer. */
        CHECK(sh_package_runtime_admit_occurrences(plans[1],documents[1],(sh_package_activation_guard){0},
            additive_activate,additive_finalize,&state,error,sizeof(error))==SH_PACKAGE_ADMISSION_FAILED);
        CHECK(!finish_probe.active && finish_probe.accepts==accepts+1 && finish_probe.rejects==rejects &&
            state.finalizations==finalizations+1);
    }
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

/* ---- AI state-machine webs through admission ------------------------------
 * The real hook API (fsm_web.c) over a modeled engine list: activation issues
 * a candidate's product state machines, registers them, publishes its table;
 * a failed or raised finalization and every recovery bind the previous table
 * again, the candidate's names staying excluded. Install never runs here. */
sh_patch_status code_unpatch(sh_patch_handle *handle) { (void)handle; CHECK(0); return B2_PATCH_FAIL_NOTLIVE; }
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
    size_t len, sh_patch_handle *out_handle)
{ (void)r; (void)expect; (void)new_bytes; (void)len; (void)out_handle; CHECK(0); return B2_PATCH_REFUSED_BADARG; }

#define FSM_ENGINE_DECLS 24
typedef struct fsm_engine_config { const char *name; uint32_t mask, pad; } fsm_engine_config;
typedef struct fsm_engine_decl {
    unsigned char bytes[0xd0];
    char name[200];
    unsigned char graphs[8][0x40];
    void *graph_pointers[8];
    char config_names[8][64];
    fsm_engine_config configs[8];
} fsm_engine_decl;
static fsm_engine_decl g_fsm_engine[FSM_ENGINE_DECLS];
static int g_fsm_engine_count;
static unsigned char g_fsm_engine_list[0x40];
static uintptr_t g_fsm_engine_generation = 0x7000;
static const char *g_fsm_view[FSM_ENGINE_DECLS];
static int g_fsm_view_count;
static const char *g_fsm_view_config;
static int fsm_engine_installed(void) { return 1; }
static void *fsm_engine_get(void *list, int index)
{ return list == g_fsm_engine_list && index >= 0 && index < g_fsm_engine_count ? &g_fsm_engine[index] : NULL; }
static void fsm_engine_generation(void)
{
    int count = g_fsm_engine_count, capacity = FSM_ENGINE_DECLS;
    g_fsm_engine_generation += 0x100;
    memcpy(g_fsm_engine_list + 0x20, &g_fsm_engine_generation, 8);
    memcpy(g_fsm_engine_list + 0x28, &count, 4); memcpy(g_fsm_engine_list + 0x2c, &capacity, 4);
}
/* Register (or reload) one declaration as the engine parses it: its graph
 * masks and the configuration names its table interns. Lists never shrink. */
static void fsm_engine_register(const char *name, const char *text, size_t length)
{
    fsm_engine_decl *d = NULL;
    char error[256];
    for (int i = 0; i < g_fsm_engine_count && !d; i++) if (!strcmp(g_fsm_engine[i].name, name)) d = g_fsm_engine + i;
    if (!d) { CHECK(g_fsm_engine_count < FSM_ENGINE_DECLS); if (g_fsm_engine_count >= FSM_ENGINE_DECLS) return; d = g_fsm_engine + g_fsm_engine_count++; }
    memset(d, 0, sizeof(*d)); strcpy_s(d->name, sizeof(d->name), name);
    const char *name_pointer = d->name; void **graphs = d->graph_pointers; fsm_engine_config *configs = d->configs;
    int graph_count = 0, config_count = 0;
    sh_decl_graph *graph = sh_decl_graph_open((sh_decl_source){text, length}, error, sizeof(error)); CHECK(graph);
    for (size_t i = 0; graph && i < sh_decl_graph_count(graph) && graph_count < 8; i++) {
        const sh_decl_graph_record *record = sh_decl_graph_at(graph, i);
        if (record->kind != SH_DECL_GRAPH_SUBGRAPH) continue;
        memcpy(d->graphs[graph_count] + 0x38, &record->layers, 4);
        d->graph_pointers[graph_count] = d->graphs[graph_count]; graph_count++;
    }
    sh_decl_graph_close(graph);
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, length}, error, sizeof(error));
    const sh_decl_node *edit = tree ? sh_decl_tree_member(tree, "edit") : NULL;
    const sh_decl_node *table = edit ? sh_decl_tree_member(edit, "layersConfigs") : NULL;
    for (const sh_decl_node *item = table ? table->children : NULL; item && config_count < 8; item = item->next) {
        strcpy_s(d->config_names[config_count], sizeof(d->config_names[0]), item->key);
        d->configs[config_count].name = d->config_names[config_count]; config_count++;
    }
    sh_decl_tree_free(tree);
    memcpy(d->bytes + 0x08, &name_pointer, 8);
    memcpy(d->bytes + 0x68, &graphs, 8); memcpy(d->bytes + 0x70, &graph_count, 4);
    memcpy(d->bytes + 0xb8, &configs, 8); memcpy(d->bytes + 0xc0, &config_count, 4);
}
/* Native activation registers every state machine the published provider serves. */
static void fsm_engine_register_served(void)
{
    const sh_package_compilation *current = sh_package_runtime_acquire();
    char error[512];
    for (size_t i = 0; current && i < current->resource_count; i++) {
        const sh_compiled_resource *r = current->resources + i;
        if (!r->native_type || strcmp(r->native_type, "aifsmmanager") || !r->native_name) continue;
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(current, r, SIZE_MAX, &length, error, sizeof(error)); CHECK(body);
        if (body) fsm_engine_register(r->native_name, (const char *)body, length);
        free(body);
    }
    sh_package_runtime_release();
    fsm_engine_generation();
}
static uintptr_t fsm_engine_builder(void *manager, void *config)
{
    (void)manager;
    g_fsm_view_config = config ? *(const char *const *)config : NULL;
    g_fsm_view_count = 0;
    for (int i = 0; i < g_fsm_engine_count; i++) {
        unsigned char *decl = sh_fsm_web_test_decl_at(g_fsm_engine_list, i);
        const char *name = NULL;
        if (decl) memcpy(&name, decl + 0x08, 8);
        g_fsm_view[g_fsm_view_count++] = name;
    }
    return 1;
}
/* One AI spawn of a behavior (its name at +8, its configuration at +0x558). */
static void fsm_spawn(const char *behavior, const char *config)
{
    static unsigned char object[0x600];
    memset(object, 0, sizeof(object));
    memcpy(object + 0x08, &behavior, 8); memcpy(object + 0x558, &config, 8);
    CHECK(sh_fsm_web_test_build(NULL, object + 0x558) == 1);
}
static int fsm_view_has(const char *name)
{
    for (int i = 0; i < g_fsm_view_count; i++) if (g_fsm_view[i] && !strcmp(g_fsm_view[i], name)) return 1;
    return 0;
}
typedef struct fsm_admission {
    char behavior[200], machine[200];
    int calls, recoveries, finalizations, finalize_fail, recovery_fail, bound_in_finalize;
} fsm_admission;
static void fsm_served_names(fsm_admission *state)
{
    const sh_package_compilation *current = sh_package_runtime_acquire();
    for (size_t i = 0; current && i < current->resource_count; i++) {
        const sh_compiled_resource *r = current->resources + i;
        if (!r->native_type || !r->native_name || strncmp(r->native_name, "smp/", 4)) continue;
        if (!strcmp(r->native_type, "aibehavior")) strcpy_s(state->behavior, sizeof(state->behavior), r->native_name);
        if (!strcmp(r->native_type, "aifsmmanager") && strncmp(r->native_name, SH_PACKAGE_FSM_FIRST_PREFIX,
            strlen(SH_PACKAGE_FSM_FIRST_PREFIX)) && strcmp(r->native_name, SH_PACKAGE_FSM_PLACEHOLDER))
            strcpy_s(state->machine, sizeof(state->machine), r->native_name);
    }
    sh_package_runtime_release();
}
/* The candidate's web: its generated first declaration (with the private
 * configuration) and its private state machine. */
static int fsm_bound_view(void)
{
    return g_fsm_view_count && g_fsm_view[0] && !strncmp(g_fsm_view[0], SH_PACKAGE_FSM_FIRST_PREFIX,
        strlen(SH_PACKAGE_FSM_FIRST_PREFIX)) && g_fsm_view_config && !strncmp(g_fsm_view_config, "smp_", 4);
}
/* The native web: stock first declaration, the behavior's own configuration,
 * every issued product state machine read as the placeholder. */
static int fsm_native_view(const char *machine)
{
    return g_fsm_view_count && g_fsm_view[0] && !strcmp(g_fsm_view[0], "states/alertcycle") &&
        g_fsm_view_config && !strcmp(g_fsm_view_config, "cyberdemon_hell") && (!*machine || !fsm_view_has(machine)) &&
        fsm_view_has(SH_PACKAGE_FSM_PLACEHOLDER);
}
static int fsm_admission_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    fsm_admission *state = context;
    (void)changes; (void)error; (void)capacity;
    if (restoring) {
        state->recoveries++;
        if (state->recovery_fail) { snprintf(error, capacity, "fixture state-machine recovery"); return 0; }
        return 1;
    }
    state->calls++;
    fsm_engine_register_served();
    fsm_served_names(state);
    return 1;
}
static int fsm_admission_finalize(void *context, char *error, size_t capacity)
{
    fsm_admission *state = context;
    state->finalizations++;
    /* The candidate's table is live: the private behavior builds its web. */
    fsm_spawn(state->behavior, "cyberdemon_hell");
    state->bound_in_finalize += fsm_bound_view() && fsm_view_has(state->machine);
    if (state->finalize_fail == 2) RaiseException(0xe0350001, 0, 0, NULL);
    if (state->finalize_fail) { snprintf(error, capacity, "fixture state-machine install failure"); return 0; }
    return 1;
}
/* Registry and reflection with AI behaviors, state-machine graphs and the
 * entity field naming a behavior, beside the admission fixture's entity. */
static void fsm_admission_setup(admission_fixture *f)
{
    enum { FA_ENTITY, FA_ENTITY_CLASS, FA_OPAQUE, FA_TYPEINFO, FA_INT, FA_BEHAVIOR, FA_STR, FA_GRAPH, FA_MACHINE, FA_GRAPH_FSM, FA_SUBGRAPH, FA_COUNT };
    const char *classes[FA_COUNT] = {"idDeclEntityDef", "Entity", "OpaqueStringReader", "idDeclTypeInfo", "int",
        "idDeclAIBehavior", "idStr", "idDeclTypeInfoGraph", "idDeclAIFSMManager", "idAIFSMGraphFSM", "idTypeInfoSubGraph"};
    const char *parents[FA_COUNT] = {"", "", "", "", "", "idDeclTypeInfo", "", "idDeclTypeInfo", "idDeclTypeInfoGraph",
        "idTypeInfoSubGraph", ""};
    const char *managers_types[3] = {"entityDef", "aiBehavior", "aiFSMManager"};
    const int managers_classes[3] = {FA_ENTITY, FA_BEHAVIOR, FA_MACHINE};
    void *managers, *container, *records, *objects, *pointers;
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18); managers = admission_alloc(f, 3 * 8);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 3);
    for (int i = 0; i < 3; i++) {
        void *manager = admission_alloc(f, 0x90); admission_pointer(managers, i * 8, manager);
        admission_pointer(manager, 8, admission_string(f, managers_types[i]));
        admission_pointer(manager, 0x10, admission_string(f, classes[managers_classes[i]]));
    }
    f->reflection = admission_alloc(f, 0xb0); container = admission_alloc(f, 0x30);
    records = admission_alloc(f, (FA_COUNT + 1) * 0x38); objects = admission_alloc(f, (FA_COUNT + 1) * 16);
    pointers = admission_alloc(f, (FA_COUNT + 1) * 16);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, FA_COUNT + 1);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, FA_COUNT + 1);
    for (int i = 0; i < FA_COUNT; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, parents[i]));
    }
    admission_pointer(pointers, FA_ENTITY * 16 + 8, (void *)0x10);
    admission_pointer(pointers, FA_BEHAVIOR * 16 + 8, (void *)0x10); admission_pointer(pointers, FA_MACHINE * 16 + 8, (void *)0x10);
    admission_pointer(objects, FA_OPAQUE * 16 + 8, (void *)0x20); admission_pointer(objects, FA_STR * 16 + 8, (void *)0x70);
    admission_pointer(objects, FA_GRAPH * 16 + 8, (void *)0x30); admission_pointer(objects, FA_MACHINE * 16 + 8, (void *)0x30);
    struct { int record; const char *type, *ops, *name; } rows[] = {
        {FA_ENTITY_CLASS, "idDeclEntityDef", "*", "target"}, {FA_ENTITY_CLASS, "OpaqueStringReader", "", "custom"},
        {FA_ENTITY_CLASS, "int", "", "health"}, {FA_ENTITY_CLASS, "idDeclAIBehavior", "*", "behavior"},
        {FA_BEHAVIOR, "idStr", "", "aiFSMManagerDeclLayersConfig"}, {FA_GRAPH_FSM, "idStr", "", "name"}};
    for (int record = 0; record < FA_COUNT; record++) {
        size_t count = 0;
        for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) count += rows[r].record == record;
        if (!count) continue;
        void *fields = admission_alloc(f, (count + 1) * 0x48); size_t at = 0;
        admission_pointer(records, record * 0x38 + 0x20, fields);
        for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) if (rows[r].record == record) {
            admission_pointer(fields, at * 0x48, admission_string(f, rows[r].type));
            admission_pointer(fields, at * 0x48 + 8, admission_string(f, rows[r].ops));
            admission_pointer(fields, at * 0x48 + 0x10, admission_string(f, rows[r].name));
            admission_number(fields, at * 0x48 + 0x1c, 8); at++;
        }
    }
}
#define FSM_LAYERS "layers = { layer = \"common\"; layer = \"imp_combat\"; layer = \"cyberdemon_hell\"; }"
#define FSM_GRAPH(name, layer) "subGraphs = { subGraph = { object = { className = \"idAIFSMGraphFSM\"; " \
    "object = { name = \"" name "\"; } } layers = { " layer " } nodes = { } links = { } } }"
static const char fsm_alertcycle[] = "{ edit = { " FSM_LAYERS " layersConfigs = { common = { layer = \"common\"; } "
    "cyberdemon_hell = { layer = \"common\"; layer = \"cyberdemon_hell\"; } } " FSM_GRAPH("FSM_alertCycle", "layer = \"common\";") " } }";
static const char fsm_generic[] = "{ edit = { " FSM_LAYERS " " FSM_GRAPH("FSM_generic", "layer = \"common\";") " } }";
static const char fsm_hell[] = "{ edit = { " FSM_LAYERS " " FSM_GRAPH("FSM_hell_fixed", "layer = \"cyberdemon_hell\";") " } }";
static int fsm_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/aifsmmanager/states/alertcycle.decl") ? fsm_alertcycle :
        !strcmp(path, "generated/decls/aifsmmanager/states/behavior_generic.decl") ? fsm_generic :
        !strcmp(path, "generated/decls/aibehavior/fsm/boss.decl") ?
            "{ edit = { aiFSMManagerDeclLayersConfig = \"cyberdemon_hell\"; } }" : NULL;
    if (!text) return historical_baseline(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void test_fsm_admission(void)
{
    const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"fsmtest/root\"}]}";
    char data[MAX_PATH], paths[2][MAX_PATH], error[2048];
    admission_fixture f; fsm_admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plans[2] = {0}; sh_package_occurrence_snapshot *documents[2] = {0};
    char *bound[2] = {0}; size_t lengths[2] = {0};
    fsm_admission state = {0};
    create("fsm-data/overrides", NULL);
    create("fsm-map/overrides/a/package.json", "{\"id\":\"fsm-map\",\"name\":\"FSM Map\"}");
    create("fsm-map/overrides/a/assets/generated/decls/entitydef/fsmtest/root.decl",
        "{ class = \"Entity\"; edit = { health = 10; target = \"\"; } }");
    create("fsm-prefab/overrides/b/package.json", "{\"id\":\"fsm-prefab\",\"name\":\"FSM Prefab\"}");
    create("fsm-prefab/overrides/b/assets/generated/decls/entitydef/fsmtest/root.decl",
        "{ class = \"Entity\"; edit = { health = 20; target = \"\"; behavior = \"fsm/boss\"; } }");
    create("fsm-prefab/overrides/b/assets/generated/decls/aifsmmanager/states/cyberdemon_hell.decl", fsm_hell);
    snprintf(data, sizeof(data), "%s/fsm-data", root);
    snprintf(paths[0], sizeof(paths[0]), "%s/fsm-map", root);
    snprintf(paths[1], sizeof(paths[1]), "%s/fsm-prefab", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(fsm_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    for (size_t i = 0; i < 2; i++) {
        plans[i] = sh_package_runtime_prepare_map(data, paths[i], error, sizeof(error));
        if (!plans[i]) fprintf(stderr, "fsm admission plan %zu: %s\n", i, error);
        CHECK(plans[i]); if (!plans[i]) goto done;
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
    /* The modeled engine: stock state machines registered, the hook bridged. */
    sh_fsm_web_test_reset(); g_fsm_engine_count = 0;
    sh_fsm_web_test_bind(fsm_engine_get, fsm_engine_builder, g_fsm_engine_list);
    fsm_engine_register("states/alertcycle", fsm_alertcycle, strlen(fsm_alertcycle)); fsm_engine_generation();
    sh_package_runtime_test_fsm_hook(fsm_engine_installed, sh_fsm_web_issue,
        (int (*)(const struct sh_fsm_web_table *))sh_fsm_web_publish);
    /* Publication succeeds, then finalization fails or raises: the previous
     * table (none) binds again, behaviors select native webs, and the
     * candidate's state machines stay excluded from every web. */
    for (int failure = 1; failure <= 2; failure++) {
        state.finalize_fail = failure;
        CHECK(sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
            fsm_admission_activate, fsm_admission_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
        CHECK(state.calls == failure && state.recoveries == failure && state.finalizations == failure);
        CHECK(state.bound_in_finalize == failure && *state.behavior && *state.machine);
        CHECK(sh_package_runtime_admission_ready() && sh_package_runtime_ready());
        fsm_spawn(state.behavior, "cyberdemon_hell");
        CHECK(fsm_native_view(state.machine));
        fsm_spawn("fsm/boss", "cyberdemon_hell");
        CHECK(fsm_native_view(state.machine));
    }
    /* A recovery that cannot complete natively is not usable; completing it
     * binds the previous table first and refuses while that cannot resolve. */
    state.finalize_fail = state.recovery_fail = 1;
    CHECK(sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        fsm_admission_activate, fsm_admission_finalize, &state, error, sizeof(error)) == SH_PACKAGE_ADMISSION_FAILED);
    CHECK(!sh_package_runtime_ready() && !sh_package_runtime_admission_ready());
    {
        fsm_engine_decl *placeholder = NULL;
        for (int i = 0; i < g_fsm_engine_count; i++) if (!strcmp(g_fsm_engine[i].name, SH_PACKAGE_FSM_PLACEHOLDER)) placeholder = g_fsm_engine + i;
        CHECK(placeholder != NULL);
        if (placeholder) {
            const char *renamed = "smp/not_a_placeholder", *restored = placeholder->name;
            memcpy(placeholder->bytes + 0x08, &renamed, 8); fsm_engine_generation();
            CHECK(!sh_package_runtime_complete_occurrence_recovery(&state));
            CHECK(!sh_package_runtime_ready() && !sh_package_runtime_admission_ready());
            memcpy(placeholder->bytes + 0x08, &restored, 8); fsm_engine_generation();
        }
    }
    state.recovery_fail = 0;
    CHECK(sh_package_runtime_complete_occurrence_recovery(&state));
    CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    fsm_spawn(state.behavior, "cyberdemon_hell"); CHECK(fsm_native_view(state.machine));
    /* The retry succeeds; later spawns select the candidate's web for its
     * behavior and the native web for every other one. */
    state.finalize_fail = 0;
    int admitted = sh_package_runtime_admit_occurrences(plans[1], documents[1], (sh_package_activation_guard){0},
        fsm_admission_activate, fsm_admission_finalize, &state, error, sizeof(error));
    if (admitted != SH_PACKAGE_ADMISSION_READY) fprintf(stderr, "fsm admission: %s\n", error);
    CHECK(admitted == SH_PACKAGE_ADMISSION_READY && sh_package_runtime_ready());
    fsm_spawn(state.behavior, "cyberdemon_hell");
    CHECK(fsm_bound_view() && fsm_view_has(state.machine));
    fsm_spawn("fsm/boss", "cyberdemon_hell");
    CHECK(fsm_native_view(state.machine));
done:
    sh_package_runtime_test_fsm_hook(NULL, NULL, NULL); sh_fsm_web_test_reset();
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
/* Each served row's native identities, as rows sorted by engine path whose
 * body lists them ("type name" lines in served order). */
static library_row *library_identities(size_t *count)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    library_row *rows = library ? calloc(library->resource_count ? library->resource_count : 1, sizeof(*rows)) : NULL;
    *count = 0;
    for (size_t i = 0; rows && i < library->resource_count; i++) {
        const sh_compiled_resource *r = library->resources + i;
        size_t names = sh_package_resource_native_count(r), length = 1;
        for (size_t n = 0; n < names; n++) {
            sh_package_native_identity id = sh_package_resource_native_at(r, n);
            length += strlen(id.type) + strlen(id.name) + 2;
        }
        rows[i].path = _strdup(r->engine_path);
        rows[i].body = calloc(length, 1);
        CHECK(rows[i].path && rows[i].body);
        if (!rows[i].path || !rows[i].body) { free(rows[i].path); free(rows[i].body); break; }
        for (size_t n = 0; n < names; n++) {
            sh_package_native_identity id = sh_package_resource_native_at(r, n);
            size_t at = strlen((char *)rows[i].body);
            snprintf((char *)rows[i].body + at, length - at, "%s %s\n", id.type, id.name);
        }
        rows[i].length = strlen((char *)rows[i].body);
        (*count)++;
    }
    sh_package_runtime_release();
    if (rows && *count > 1) qsort(rows, *count, sizeof(*rows), library_row_order);
    return rows;
}
/* 1 when every path both snapshots serve has the same native identities. */
static int library_identities_agree(const library_row *a, size_t an, const library_row *b, size_t bn)
{
    int equal = 1;
    for (size_t i = 0, j = 0; i < an && j < bn; ) {
        int order = strcmp(a[i].path, b[j].path);
        if (order < 0) { i++; continue; }
        if (order > 0) { j++; continue; }
        if (a[i].length != b[j].length || memcmp(a[i].body, b[j].body, a[i].length)) {
            fprintf(stderr, "different native identities: %s\n", a[i].path); equal = 0;
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
/* D8 counterexample (OpenGL, 2026-09-30): a package whose editor row inherits
 * a stock row and whose entity inherits a stock definition, installed while
 * the library already composes an editor-only extension of that definition
 * (a local editor package). The stock definition is a shared declaration the
 * engine serves once, under its native name, with whatever payload the shared
 * publication selects; a consumer binds that named contract in every context.
 * So a full scan, the installation increment over the published library and
 * the identical package in the map's own context bind the consumer alike, and
 * a different shared composition does not change it. A real gameplay change
 * to the parent is a private variant and still distinguishes its consumers,
 * even with the same authored bytes. Shipped files are pretty-printed, so
 * their bytes differ from any compiler re-serialization. */
static const char *d8c_stock_def = "generated/decls/entitydef/d8stock/speaker.decl";
static const char *d8c_stock_row = "generated/decls/snapeditorentitydef/d8stock/speaker_row.decl";
static const char *d8c_probe_def = "generated/decls/entitydef/d8probe/speaker.decl";
static const char *d8c_probe_row = "generated/decls/snapeditorentitydef/d8probe/speaker.decl";
static int d8c_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    /* The shipped definition states its editorVars out of key order, as the
     * game's files do; editor composition writes that block sorted. */
    const char *text = !strcmp(path, d8c_stock_def) ?
            "{\n\tclass = \"Entity\";\n\teditorVars {\n\t\trotatable = false;\n\t\tplaceable = false;\n"
            "\t\tcolor = ( 1, 1, 1, 1 );\n\t}\n\tedit = {\n\t\thealth = 1;\n\t\ttarget = \"\";\n\t}\n}\n" :
        /* The shipped entry states its collection before its other fields;
         * editor composition moves collection fields to the tail of edit. */
        !strcmp(path, d8c_stock_row) ?
            "{\n\tedit = {\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t\tentityDef = \"d8stock/speaker\";\n\t}\n}\n" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void d8c_file(const char *prefix, const char *relative, const char *body)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", prefix, relative); create(path, body);
}
static void d8c_probe(const char *prefix)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-probe\",\"name\":\"D8 probe\"}");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8probe/speaker.decl",
        "{ inherit = \"d8stock/speaker\"; edit = { health = 7; } }");
    d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8probe/speaker.decl",
        "{ inherit = \"d8stock/speaker_row\"; edit = { entityDef = \"d8probe/speaker\"; } }");
}
/* An editor-only extension of the stock definition (variant 1 or 2). */
static void d8c_local_editor(const char *prefix, int variant)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-local-editor\",\"name\":\"D8 local editor\"}");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8stock/speaker.decl", variant == 1 ?
        "{ class = \"Entity\"; editorVars = { localEditorExtension = true; } edit = { health = 1; target = \"\"; } }" :
        "{ class = \"Entity\"; editorVars = { otherEditorExtension = 3; } edit = { health = 1; target = \"\"; } }");
}
/* A real gameplay change to the stock parent, with a consumer whose authored
 * path and bytes equal the probe's definition. */
static void d8c_gameplay(const char *prefix)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-gameplay\",\"name\":\"D8 gameplay\"}");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8stock/speaker.decl",
        "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8probe/speaker.decl",
        "{ inherit = \"d8stock/speaker\"; edit = { health = 7; } }");
}
static const library_row *d8c_original(const library_row *rows, size_t count, const char *original)
{
    for (size_t i = 0; i < count; i++) if (rows[i].original && !strcmp(rows[i].original, original)) return rows + i;
    return NULL;
}
static size_t d8c_originals(const library_row *rows, size_t count, const char *original)
{
    size_t found = 0;
    for (size_t i = 0; i < count; i++) if (rows[i].original && !strcmp(rows[i].original, original)) found++;
    return found;
}
/* Exposure: editor rows served as private copies of a stock row no package
 * authors (visible palette duplicates). Internal aliases of definitions a
 * package owns are not counted. */
static size_t d8c_stock_row_copies(const char *label, const library_row *rows, size_t count)
{
    size_t copies = 0;
    for (size_t i = 0; i < count; i++)
        if (rows[i].original && !strcmp(rows[i].original, d8c_stock_row) && strcmp(rows[i].original, rows[i].path)) {
            fprintf(stderr, "%s: private copy %s of stock row %s\n", label, rows[i].path, rows[i].original); copies++;
        }
    return copies;
}
static void d8c_dump(const char *label, const library_row *rows, size_t count)
{
    for (size_t i = 0; i < count; i++)
        fprintf(stderr, "%s: %s (%s): %.*s\n", label, rows[i].path, rows[i].original ? rows[i].original : "-",
            (int)(rows[i].length < 200 ? rows[i].length : 200), (const char *)rows[i].body);
}
static library_row *d8c_compilation_rows(const sh_package_compilation *compiled, size_t *count)
{
    char error[512];
    library_row *rows = calloc(compiled && compiled->resource_count ? compiled->resource_count : 1, sizeof(*rows));
    *count = 0;
    for (size_t i = 0; rows && compiled && i < compiled->resource_count; i++) {
        rows[i].path = _strdup(compiled->resources[i].engine_path);
        rows[i].original = compiled->resources[i].original_path ? _strdup(compiled->resources[i].original_path) : NULL;
        rows[i].body = sh_package_compilation_read(compiled, compiled->resources + i, SIZE_MAX,
            &rows[i].length, error, sizeof(error));
        (*count)++;
    }
    return rows;
}
static int d8c_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{
    (void)changes; (void)error; (void)capacity;
    if (!restoring) (*(int *)context)++;
    return 1;
}
/* One full scan of a fixture library: the probe plus the selected peers. */
static library_row *d8c_scan(const char *name, int local_variant, int gameplay, admission_fixture *f, size_t *count)
{
    char data[MAX_PATH], prefix[MAX_PATH];
    sh_decl_registry_source registry = {f, admission_read, (uintptr_t)f->registry, NULL};
    snprintf(prefix, sizeof(prefix), "%s/overrides/probe", name); d8c_probe(prefix);
    if (local_variant) { snprintf(prefix, sizeof(prefix), "%s/overrides/local-editor", name); d8c_local_editor(prefix, local_variant); }
    if (gameplay) { snprintf(prefix, sizeof(prefix), "%s/overrides/gameplay", name); d8c_gameplay(prefix); }
    snprintf(data, sizeof(data), "%s/%s", root, name);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8c_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f->reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    library_row *rows = library_snapshot(count);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    return rows;
}
static void test_increment_editor_composition(void)
{
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    library_row *full = NULL, *other = NULL, *plain = NULL, *variant = NULL;
    library_row *incremented = NULL, *rescanned = NULL, *mapped = NULL;
    size_t full_count = 0, other_count = 0, plain_count = 0, variant_count = 0;
    size_t incremented_count = 0, rescanned_count = 0, mapped_count = 0;
    int activations = 0;
    snprintf(data, sizeof(data), "%s/d8c-data", root);
    /* Full scans: the local extension, a different one, none, and none plus a
     * real gameplay variant of the parent. */
    full = d8c_scan("d8c-full", 1, 0, &f, &full_count);
    other = d8c_scan("d8c-other", 2, 0, &f, &other_count);
    plain = d8c_scan("d8c-plain", 0, 0, &f, &plain_count);
    variant = d8c_scan("d8c-variant", 0, 1, &f, &variant_count);
    const library_row *row = d8c_original(full, full_count, d8c_probe_row);
    const library_row *def = d8c_original(full, full_count, d8c_probe_def);
    CHECK(row && def && !d8c_stock_row_copies("full scan", full, full_count));
    if (!row || !def) goto done;
    CHECK(strstr((const char *)row->body, "\"d8stock/speaker_row\"") != NULL);
    CHECK(strstr((const char *)def->body, "\"d8stock/speaker\"") != NULL);
    /* The shared composition selects the parent's payload, not the consumer. */
    const library_row *peers[2] = {d8c_original(other, other_count, d8c_probe_def),
        d8c_original(plain, plain_count, d8c_probe_def)};
    CHECK(peers[0] && peers[1] && !strcmp(peers[0]->path, def->path) && !strcmp(peers[1]->path, def->path));
    /* Same authored path and bytes over a real gameplay parent variant: two
     * bindings, and the probe's own consumer is unaffected. */
    CHECK(d8c_originals(variant, variant_count, d8c_probe_def) == 2);
    int kept = 0, different = 0;
    for (size_t i = 0; i < variant_count; i++) {
        if (!variant[i].original || strcmp(variant[i].original, d8c_probe_def)) continue;
        if (!strcmp(variant[i].path, def->path)) kept++; else different++;
    }
    CHECK(kept == 1 && different == 1);
    /* The same pair reached by installing the probe from a map over a library
     * that already publishes the local extension. */
    d8c_local_editor("d8c-data/overrides/local-editor", 1);
    d8c_probe("d8c-map/overrides/probe");
    snprintf(data, sizeof(data), "%s/d8c-data", root);
    snprintf(source, sizeof(source), "%s/d8c-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8c_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    expect_current(d8c_stock_def, "localEditorExtension");
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) { fprintf(stderr, "d8 map: %s\n", error); goto done; }
    mapped = d8c_compilation_rows(sh_package_map_plan_compilation(plan), &mapped_count);
    d8c_probe("d8c-data/overrides/probe");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    if (!prepared || error[0]) fprintf(stderr, "d8 inventory: %s\n", error);
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, d8c_activate, &activations));
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
        &(int){0}));
    incremented = library_snapshot(&incremented_count);
    CHECK(sh_package_runtime_refresh(data));
    rescanned = library_snapshot(&rescanned_count);
    size_t aliases = 0;
    int designed = library_rows_designed(incremented, incremented_count, rescanned, rescanned_count, &aliases);
    size_t copies = d8c_stock_row_copies("increment", incremented, incremented_count);
    const library_row *installed[2] = {d8c_original(incremented, incremented_count, d8c_probe_row),
        d8c_original(incremented, incremented_count, d8c_probe_def)};
    const library_row *authored[2] = {d8c_original(mapped, mapped_count, d8c_probe_row),
        d8c_original(mapped, mapped_count, d8c_probe_def)};
    int bound = installed[0] && installed[1] && !strcmp(installed[0]->path, row->path) &&
        !strcmp(installed[1]->path, def->path);
    int identical = authored[0] && authored[1] && !strcmp(authored[0]->path, row->path) &&
        !strcmp(authored[1]->path, def->path);
    if (!designed || aliases || copies || !bound || !identical) {
        d8c_dump("increment", incremented, incremented_count); d8c_dump("rescan", rescanned, rescanned_count);
        d8c_dump("full", full, full_count); d8c_dump("map", mapped, mapped_count);
    }
    /* The increment serves what the rescan serves; the installed probe and the
     * identical probe in the map's own context are one binding. */
    CHECK(designed && !aliases && !copies && bound && identical);
done:
    library_rows_free(full, full_count); library_rows_free(other, other_count);
    library_rows_free(plain, plain_count); library_rows_free(variant, variant_count);
    library_rows_free(incremented, incremented_count);
    library_rows_free(rescanned, rescanned_count); library_rows_free(mapped, mapped_count);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* D8 stock rows (OpenGL, 2026-09-30, candidate d8-contract-8): the live shapes
 * of the stock editor rows a probe reaches. A speaker row states a complete
 * property list and names an action row among its inputs; the action row
 * inherits a base row and states only its own property as a sparse list. A
 * local editor package overrides the base row and privately overrides its
 * name inspector, so the library serves the base row bound to that inspector;
 * the children's controls stay at their slots. Installing the probe compiles
 * it alone, where no package contributes to those stock rows at all. Shipped
 * files state property fields in their own order (path first) and a list
 * count before its items; composing over nothing would respell them. */
static const char *d8r_inspector = "generated/decls/snappropertyinspector_text/d8rows/name_inspector.decl";
static const char *d8r_base = "generated/decls/snapeditorentitydef/d8rows/input_base.decl";
static const char *d8r_play = "generated/decls/snapeditorentitydef/d8rows/play.decl";
static const char *d8r_row = "generated/decls/snapeditorentitydef/d8rows/speaker_row.decl";
static const char *d8r_probe_row = "generated/decls/snapeditorentitydef/d8probe/speaker.decl";
static const char *d8r_probe_def = "generated/decls/entitydef/d8probe/speaker.decl";
static const char *const d8r_base_text =
    "{\n\tedit = {\n\t\tpropertySheets = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n"
    "\t\t\t\t\tnum = 2;\n\t\t\t\t\titem[0] = {\n\t\t\t\t\t\tpath = \"displayName\";\n"
    "\t\t\t\t\t\tdisplayNameTag = \"#str_name\";\n\t\t\t\t\t\tdescriptionTag = \"#str_name_desc\";\n"
    "\t\t\t\t\t\tinspector = \"d8rows/name_inspector\";\n\t\t\t\t\t}\n\t\t\t\t\titem[1] = {\n"
    "\t\t\t\t\t\tpath = \"actionDelay\";\n\t\t\t\t\t\tdisplayNameTag = \"#str_delay\";\n"
    "\t\t\t\t\t\tdescriptionTag = \"#str_delay_desc\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
static int d8r_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text =
        !strcmp(path, "generated/decls/entitydef/d8rows/speaker.decl") ?
            "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 1;\n\t\ttarget = \"\";\n\t}\n}\n" :
        !strcmp(path, "generated/decls/entitydef/d8rows/play_action.decl") ?
            "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 0;\n\t\ttarget = \"\";\n\t}\n}\n" :
        !strcmp(path, d8r_inspector) ?
            "{\n\tedit = {\n\t\tplaceholderText = \"#str_enter\";\n\t\tmaxCharLength = 20;\n\t}\n}\n" :
        !strcmp(path, d8r_base) ? d8r_base_text :
        !strcmp(path, d8r_play) ?
            "{\n\tinherit = \"d8rows/input_base\";\n\tedit = {\n\t\tentityDef = \"d8rows/play_action\";\n"
            "\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 3;\n"
            "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tpath = \"volume\";\n\t\t\t\t\t\tdisplayNameTag = \"#str_volume\";\n"
            "\t\t\t\t\t\tdescriptionTag = \"#str_volume_desc\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/play_loud.decl") ?
            "{\n\tinherit = \"d8rows/play\";\n\tedit = {\n\t\tpropertySheets = {\n\t\t\titem[0] = {\n"
            "\t\t\t\tproperties = {\n\t\t\t\t\tnum = 4;\n\t\t\t\t\titem[3] = {\n"
            "\t\t\t\t\t\tpath = \"loudness\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/extra_base.decl") ?
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"hint\"; } } } } } }" :
        /* A shipped child stating its whole list over the base row. */
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/full_child.decl") ?
            "{ inherit = \"d8rows/input_base\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
            "item[0] = { path = \"displayName\"; } item[1] = { path = \"actionDelay\"; } item[2] = { path = \"volume\"; } "
            "} } } } }" :
        /* A shipped control without a path, as the native reader places it. */
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/anon_base.decl") ?
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"displayName\"; displayNameTag = \"#str_name\"; } "
            "item[1] = { displayNameTag = \"#str_anon\"; descriptionTag = \"#str_anon_desc\"; } } } } } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/envparent.decl") ?
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"envbase\"; } } } } } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/envchild.decl") ?
            "{ inherit = \"d8rows/envparent\"; edit = { propertySheets = { item[0] = { properties = { num = 2; "
            "item[1] = { path = \"envown\"; } } } } } }" :
        !strcmp(path, "generated/decls/snappropertyinspector_text/d8rows/x_picker.decl") ?
            "{\n\tedit = {\n\t\tplaceholderText = \"#str_pick\";\n\t\tallowedTypes = {\n\t\t\tnum = 1;\n"
            "\t\t\titem[0] = \"d8rows/z_dest\";\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snappropertyinspector_text/d8rows/y_picker.decl") ?
            "{\n\tedit = {\n\t\tplaceholderText = \"#str_pick\";\n\t\tallowedTypes = {\n\t\t\tnum = 1;\n"
            "\t\t\titem[0] = \"d8rows/a_dest\";\n\t\t}\n\t}\n}\n" :
        /* Destination rows a picker allows, each showing the name inspector. */
        (!strcmp(path, "generated/decls/snapeditorentitydef/d8rows/z_dest.decl") ||
         !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/a_dest.decl")) ?
            "{\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n\t\tpropertySheets = {\n\t\t\tnum = 1;\n"
            "\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = {\n"
            "\t\t\t\t\t\tpath = \"displayName\";\n\t\t\t\t\t\tinspector = \"d8rows/name_inspector\";\n"
            "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" :
        /* Stock children of the base row patching one sparse slot with a
         * picker inspector, as the shipped action rows do. */
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/z_caller.decl") ?
            "{\n\tinherit = \"d8rows/input_base\";\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n"
            "\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 3;\n"
            "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tpath = \"target\";\n\t\t\t\t\t\tinspector = \"d8rows/y_picker\";\n"
            "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/a_caller.decl") ?
            "{\n\tinherit = \"d8rows/input_base\";\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n"
            "\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 3;\n"
            "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tpath = \"target\";\n\t\t\t\t\t\tinspector = \"d8rows/x_picker\";\n"
            "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/caller_host.decl") ?
            "{\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n\t\tinputs = {\n\t\t\tnum = 2;\n"
            "\t\t\titem[0] = \"d8rows/a_caller\";\n\t\t\titem[1] = \"d8rows/z_caller\";\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/tinted.decl") ?
            "{\n\tedit = {\n\t\tdisplayNameColor = {\n\t\t\tr = 1;\n\t\t}\n\t\tentityDef = \"d8rows/speaker\";\n\t}\n}\n" :
        !strcmp(path, d8r_row) ?
            "{\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n\t\tpropertySheets = {\n\t\t\tnum = 1;\n"
            "\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 1;\n\t\t\t\t\titem[0] = {\n"
            "\t\t\t\t\t\tpath = \"displayName\";\n\t\t\t\t\t\tdisplayNameTag = \"#str_name\";\n"
            "\t\t\t\t\t\tdescriptionTag = \"#str_name_desc\";\n\t\t\t\t\t\tinspector = \"d8rows/name_inspector\";\n"
            "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t\tinputs = {\n\t\t\tnum = 1;\n\t\t\titem[0] = \"d8rows/play\";\n"
            "\t\t}\n\t}\n}\n" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* Native metadata for the rows above: an editor entry's inputs name editor
 * entries, its property sheets hold properties whose inspector is a text
 * inspector declaration. */
static void d8r_setup(admission_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo", "int", "idDeclSnapEditorEntity",
        "idList < EditorRefs >", "idDeclTypeInfoGraph", "idStr", "idList < EditorSheet >", "EditorSheet",
        "idList < EditorProperty >", "EditorProperty", "idDeclSnapPropertyInspector_Text", "idColor", "float"};
    const char *types[] = {"entityDef", "snapEditorEntityDef", "snapPropertyInspector_Text"};
    const size_t indices[] = {0, 4, 12}, count = sizeof(classes) / sizeof(*classes);
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18);
    void *managers = admission_alloc(f, 3 * 8);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 3);
    for (size_t i = 0; i < 3; i++) {
        void *manager = admission_alloc(f, 0x90); admission_pointer(managers, i * 8, manager);
        admission_pointer(manager, 8, admission_string(f, types[i]));
        admission_pointer(manager, 0x10, admission_string(f, classes[indices[i]]));
    }
    f->reflection = admission_alloc(f, 0xb0);
    void *container = admission_alloc(f, 0x30), *records = admission_alloc(f, (count + 1) * 0x38);
    void *objects = admission_alloc(f, (count + 1) * 16), *pointers = admission_alloc(f, (count + 1) * 16);
    char *parent = admission_string(f, "idDeclTypeInfo"), *none = admission_string(f, "");
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, (int)count + 1);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, (int)count + 1);
    for (size_t i = 0; i < count; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, i == 4 || i == 6 || i == 12 ? parent : none);
    }
    /* Declaration references, the graph reader, strings and lists. */
    admission_pointer(pointers, 8, (void *)0x10); admission_pointer(pointers, 4 * 16 + 8, (void *)0x10);
    admission_pointer(pointers, 12 * 16 + 8, (void *)0x10);
    admission_pointer(objects, 6 * 16 + 8, (void *)0x30); admission_pointer(objects, 7 * 16 + 8, (void *)0x70);
    const size_t lists[] = {5, 8, 10}, elements[] = {4, 9, 11};
    for (size_t i = 0; i < 3; i++) {
        void *element = admission_alloc(f, 0x48);
        admission_pointer(objects, lists[i] * 16 + 8, (void *)0x80);
        admission_pointer(records, lists[i] * 0x38 + 0x18, element);
        admission_pointer(element, 0, admission_string(f, classes[elements[i]]));
        admission_pointer(element, 8, i ? none : admission_string(f, "*"));
        admission_pointer(element, 0x10, admission_string(f, "element")); admission_number(element, 0x1c, 8);
    }
    static const struct { size_t record, count; const char *types[6], *ops[6], *names[6]; } rows[] = {
        {1, 2, {"idDeclEntityDef", "int"}, {"*", ""}, {"target", "health"}},
        {4, 5, {"idList < EditorRefs >", "idDeclEntityDef", "idList < EditorSheet >", "idColor", "idStr"},
            {"", "*", "", "", ""}, {"inputs", "entityDef", "propertySheets", "displayNameColor", "placementFlags"}},
        {13, 4, {"float", "float", "float", "float"}, {"", "", "", ""}, {"r", "g", "b", "a"}},
        {9, 1, {"idList < EditorProperty >"}, {""}, {"properties"}},
        {11, 4, {"idStr", "idStr", "idStr", "idDeclSnapPropertyInspector_Text"}, {"", "", "", "*"},
            {"path", "displayNameTag", "descriptionTag", "inspector"}},
        {12, 3, {"idStr", "int", "idList < EditorRefs >"}, {"", "", ""}, {"placeholderText", "maxCharLength", "allowedTypes"}},
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(*rows); r++) {
        void *table = admission_alloc(f, (rows[r].count + 1) * 0x48);
        admission_pointer(records, rows[r].record * 0x38 + 0x20, table);
        for (size_t i = 0; i < rows[r].count; i++) {
            admission_pointer(table, i * 0x48, admission_string(f, rows[r].types[i]));
            admission_pointer(table, i * 0x48 + 8, *rows[r].ops[i] ? admission_string(f, rows[r].ops[i]) : none);
            admission_pointer(table, i * 0x48 + 0x10, admission_string(f, rows[r].names[i]));
            admission_number(table, i * 0x48 + 0x1c, 8);
        }
    }
}
static void d8r_probe(const char *prefix)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-probe\",\"name\":\"D8 probe\"}");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8probe/speaker.decl",
        "{ inherit = \"d8rows/speaker\"; edit = { health = 7; } }");
    d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8probe/speaker.decl",
        "{ inherit = \"d8rows/speaker_row\"; edit = { entityDef = \"d8probe/speaker\"; } }");
}
/* The installed local editor package: a whole-file override of the name
 * inspector and its own copy of the base row (kind 1, as installed), a base
 * row with a control inserted first (kind 2), the base row reparented onto
 * another stock row (kind 3: its author's own variant, which the shared base
 * row does not take, so its children keep inheriting the shipped one), or the
 * base row with only its display colour changed (kind 4, presentation: the
 * shared row carries it) or with its placement changed (kind 5, behaviour: its
 * author's own variant). Kinds 4 and 5 also author a row whose inputs reach
 * the action row, so that row is bound in the package's own scope. */
static void d8r_local_editor(const char *prefix, int kind)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-local-editor\",\"name\":\"D8 local editor\"}");
    if (kind == 10) {
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/a_caller.decl",
            "{\n\tinherit = \"d8rows/input_base\";\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n"
            "\t\tpropertySheets = {\n\t\t\titem[0] = {\n\t\t\t\tproperties = {\n\t\t\t\t\tnum = 3;\n"
            "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tpath = \"target\";\n\t\t\t\t\t\tinspector = \"d8rows/y_picker\";\n"
            "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n");
        return;
    }
    if (kind == 7 || kind == 8 || kind == 9) {
        if (kind == 9) {
            unsigned char *copy = NULL; size_t copy_length = 0;
            if (d8r_baseline(NULL, "generated/decls/snapeditorentitydef/d8rows/a_caller.decl", &copy, &copy_length) == 1)
                d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/a_caller.decl", (const char *)copy);
            free(copy);
        }
        if (kind == 8)
            d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/a_caller.decl",
                "{ inherit = \"d8rows/input_base\"; edit = { placementFlags = \"SNAP_ENTITY_PLACE_FLOOR\"; "
                "entityDef = \"d8rows/speaker\"; propertySheets = { item[0] = { properties = { num = 3; "
                "item[2] = { path = \"target\"; inspector = \"d8rows/x_picker\"; } } } } } }");
        d8c_file(prefix, "assets/generated/decls/snappropertyinspector_text/d8rows/name_inspector.decl",
            "{ edit = { placeholderText = \"#str_enter\"; maxCharLength = 666; } }");
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/caller_host.decl",
            "{\n\tedit = {\n\t\tplacementFlags = \"SNAP_ENTITY_PLACE_FLOOR\";\n\t\tentityDef = \"d8rows/speaker\";\n"
            "\t\tinputs = {\n\t\t\tnum = 2;\n\t\t\titem[0] = \"d8rows/a_caller\";\n\t\t\titem[1] = \"d8rows/z_caller\";\n"
            "\t\t}\n\t}\n}\n");
        return;
    }
    if (kind == 4 || kind == 5) {
        char text[2048];
        const char *edit = strstr(d8r_base_text, "edit = {");
        snprintf(text, sizeof(text), "{\n\tedit = {\n\t\t%s\n%s", kind == 4 ?
            "displayNameColor = {\n\t\t\tr = 0;\n\t\t\tg = 1;\n\t\t\tb = 0;\n\t\t}" :
            "placementFlags = \"SNAP_ENTITY_PLACE_FLOOR\";", edit ? edit + strlen("edit = {") + 1 : "");
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl", text);
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8editor/actions.decl",
            "{ edit = { entityDef = \"d8rows/speaker\"; inputs = { num = 1; item[0] = \"d8rows/play\"; } } }");
        return;
    }
    if (kind == 3) {
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl",
            "{ inherit = \"d8rows/extra_base\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[1] = { path = \"displayName\"; displayNameTag = \"#str_name\"; descriptionTag = \"#str_name_desc\"; "
            "inspector = \"d8rows/name_inspector\"; } "
            "item[2] = { path = \"actionDelay\"; displayNameTag = \"#str_delay\"; descriptionTag = \"#str_delay_desc\"; } "
            "} } } } }");
        return;
    }
    if (kind == 2) {
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
            "item[0] = { path = \"comment\"; displayNameTag = \"#str_comment\"; } "
            "item[1] = { path = \"displayName\"; displayNameTag = \"#str_name\"; descriptionTag = \"#str_name_desc\"; "
            "inspector = \"d8rows/name_inspector\"; } "
            "item[2] = { path = \"actionDelay\"; displayNameTag = \"#str_delay\"; descriptionTag = \"#str_delay_desc\"; } "
            "} } } } }");
        return;
    }
    d8c_file(prefix, "assets/generated/decls/snappropertyinspector_text/d8rows/name_inspector.decl",
        "{ edit = { placeholderText = \"#str_enter\"; maxCharLength = 666; } }");
    d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl", d8r_base_text);
}
/* A real gameplay change to the speaker definition: the stock speaker row
 * reaches it unchanged, so that package's row is a downstream-only variant. */
static void d8r_gameplay(const char *prefix)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-gameplay\",\"name\":\"D8 gameplay\"}");
    d8c_file(prefix, "assets/generated/decls/entitydef/d8rows/speaker.decl",
        "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
    d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8gameplay/speaker.decl",
        "{ inherit = \"d8rows/speaker_row\"; edit = { } }");
}
/* Served private copies of one stock declaration (its original path). */
static size_t d8r_copies(const char *label, const library_row *rows, size_t count, const char *stock)
{
    size_t copies = 0;
    for (size_t i = 0; i < count; i++)
        if (rows[i].original && !strcmp(rows[i].original, stock) && strcmp(rows[i].path, stock)) {
            fprintf(stderr, "%s: private copy %s of %s: %.*s\n", label, rows[i].path, stock,
                (int)(rows[i].length < 400 ? rows[i].length : 400), (const char *)rows[i].body);
            copies++;
        }
    return copies;
}
static const library_row *d8r_path(const library_row *rows, size_t count, const char *path)
{
    for (size_t i = 0; i < count; i++) if (!strcmp(rows[i].path, path)) return rows + i;
    return NULL;
}
/* The effective first property page of an editor entry as the engine's
 * inherited parse builds it from what a library serves (the shipped text where
 * it serves none): the parent's list, then each num resizing it and each
 * item[k] merging field by field into slot k. Written as path[:inspector]|... */
typedef struct d8r_control { char path[64], inspector[96]; } d8r_control;
static int d8r_controls(const library_row *rows, size_t count, const char *name, d8r_control *list, size_t *n,
    unsigned depth)
{
    char path[256];
    unsigned char *body = NULL; size_t length = 0;
    snprintf(path, sizeof(path), "generated/decls/snapeditorentitydef/%s.decl", name);
    const library_row *served = d8r_path(rows, count, path);
    if (served) { body = (unsigned char *)_strdup((const char *)served->body); length = served->length; }
    else if (d8r_baseline(NULL, path, &body, &length) != 1) return 0;
    sh_decl_node *tree = body ? sh_decl_tree_parse((sh_decl_source){(const char *)body, length}, NULL, 0) : NULL;
    free(body);
    if (!tree || depth > 8) { sh_decl_tree_free(tree); return 0; }
    const char *text; size_t text_length; char parent[128];
    int ok = 1;
    *n = 0;
    if (sh_decl_tree_literal(sh_decl_tree_member(tree, "inherit"), &text, &text_length) && text_length < sizeof(parent)) {
        memcpy(parent, text, text_length); parent[text_length] = 0;
        ok = d8r_controls(rows, count, parent, list, n, depth + 1);
    }
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *sheets = edit ? sh_decl_tree_member(edit, "propertySheets") : NULL;
    const sh_decl_node *page = sheets ? sh_decl_tree_member(sheets, "item[0]") : NULL;
    const sh_decl_node *properties = page ? sh_decl_tree_member(page, "properties") : NULL;
    for (const sh_decl_node *entry = properties ? properties->children : NULL; ok && entry; entry = entry->next) {
        size_t target = 0, index = 0;
        if (!strcmp(entry->key, "num")) {
            ok = sh_decl_tree_literal(entry, &text, &text_length) && sscanf(text, "%zu", &target) == 1 && target < 32;
            while (ok && *n < target) memset(list + (*n)++, 0, sizeof(*list));
            if (ok) *n = target;
            continue;
        }
        ok = sscanf(entry->key, "item[%zu]", &index) == 1 && index < 32;
        while (ok && *n <= index) memset(list + (*n)++, 0, sizeof(*list));
        for (const sh_decl_node *field = ok ? entry->children : NULL; field; field = field->next) {
            char *slot = !strcmp(field->key, "path") ? list[index].path :
                !strcmp(field->key, "inspector") ? list[index].inspector : NULL;
            size_t capacity = !strcmp(field->key, "path") ? sizeof(list[index].path) : sizeof(list[index].inspector);
            if (slot && sh_decl_tree_literal(field, &text, &text_length) && text_length < capacity) {
                /* The native declaration-pointer reader clears unquoted NULL;
                 * it is a constructor default, not an inspector's name. */
                if (!strcmp(field->key, "inspector") && field->value[0] != '"' &&
                    text_length == 4 && !memcmp(text, "NULL", 4)) text_length = 0;
                memcpy(slot, text, text_length); slot[text_length] = 0;
            }
        }
    }
    sh_decl_tree_free(tree); return ok;
}
static void d8r_effective(const library_row *rows, size_t count, const char *name, char *out, size_t capacity)
{
    d8r_control list[32]; size_t n = 0;
    *out = 0;
    if (!d8r_controls(rows, count, name, list, &n, 0)) { snprintf(out, capacity, "(unreadable)"); return; }
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, capacity - used, "%s%s%s%s", i ? "|" : "", list[i].path,
            *list[i].inspector ? ":" : "", list[i].inspector);
    }
}
static library_row *d8r_scan(const char *name, int probe, int local_editor, int gameplay, admission_fixture *f,
    size_t *count)
{
    char data[MAX_PATH], prefix[MAX_PATH];
    sh_decl_registry_source registry = {f, admission_read, (uintptr_t)f->registry, NULL};
    if (probe) { snprintf(prefix, sizeof(prefix), "%s/overrides/probe", name); d8r_probe(prefix); }
    if (local_editor) {
        snprintf(prefix, sizeof(prefix), "%s/overrides/local-editor", name); d8r_local_editor(prefix, local_editor);
    }
    if (gameplay) { snprintf(prefix, sizeof(prefix), "%s/overrides/gameplay", name); d8r_gameplay(prefix); }
    snprintf(data, sizeof(data), "%s/%s", root, name);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f->reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    library_row *rows = library_snapshot(count);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    return rows;
}
static void test_increment_stock_rows(void)
{
    const char *stock[] = {d8r_base, d8r_play, d8r_row};
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    static const char *const shipped[] = {"d8rows/extra_base", "d8rows/input_base", "d8rows/play",
        "d8rows/play_loud", "d8rows/speaker_row"};
    admission_fixture f; d8r_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    sh_package_runtime_test_editor_rows(shipped, sizeof(shipped) / sizeof(*shipped));
    library_row *full = NULL, *moved = NULL, *variant = NULL, *incremented = NULL, *rescanned = NULL, *mapped = NULL;
    size_t full_count = 0, moved_count = 0, variant_count = 0, incremented_count = 0, rescanned_count = 0;
    size_t mapped_count = 0;
    int activations = 0;
    /* The library: the local editor package and the probe, scanned whole. The
     * package's byte-faithful copy of the base row is not an edit to it: that
     * scope binds the base row to its private inspector as its own variant,
     * while the shipped base row and its children keep their native text,
     * whose controls stay at the slots their authors saw. */
    full = d8r_scan("d8r-full", 1, 1, 0, &f, &full_count);
    size_t full_copies = 0, base_copies = d8r_copies("full scan", full, full_count, d8r_base);
    for (size_t s = 1; s < 3; s++) full_copies += d8r_copies("full scan", full, full_count, stock[s]);
    const library_row *own_base = NULL;
    for (size_t i = 0; i < full_count; i++)
        if (full[i].original && !strcmp(full[i].original, d8r_base) && strcmp(full[i].path, d8r_base)) own_base = full + i;
    const library_row *row = d8c_original(full, full_count, d8r_probe_row);
    const library_row *def = d8c_original(full, full_count, d8r_probe_def);
    CHECK(!full_copies && base_copies == 1 && own_base && row && def && !d8r_path(full, full_count, d8r_base));
    if (!own_base || !row || !def) { d8c_dump("full", full, full_count); goto done; }
    CHECK(strstr((const char *)own_base->body, "inspector = \"smp/") != NULL);
    CHECK(!d8r_path(full, full_count, d8r_play) && !d8r_path(full, full_count, d8r_row));
    CHECK(strstr((const char *)row->body, "\"d8rows/speaker_row\"") != NULL);
    /* A real inherited change: with a control inserted before them, the child's
     * patch no longer lands on its intended control natively, so the child is
     * served restated under its own name, its control after the parent's. */
    moved = d8r_scan("d8r-moved", 1, 2, 0, &f, &moved_count);
    const library_row *restated = d8r_path(moved, moved_count, d8r_play);
    const char *comment = restated ? strstr((const char *)restated->body, "\"comment\"") : NULL;
    CHECK(restated && comment && strstr(comment, "\"actionDelay\"") && strstr(strstr(comment, "\"actionDelay\""), "\"volume\""));
    char layout[512];
    d8r_effective(moved, moved_count, "d8rows/play_loud", layout, sizeof(layout));
    if (strcmp(layout, "comment|displayName:d8rows/name_inspector|actionDelay|volume|loudness"))
        fprintf(stderr, "d8r moved: play_loud %s\n", layout);
    CHECK(!strcmp(layout, "comment|displayName:d8rows/name_inspector|actionDelay|volume|loudness"));
    CHECK(!d8r_copies("moved", moved, moved_count, d8r_play) && !d8r_copies("moved", moved, moved_count, d8r_row));
    /* Downstream-only: a gameplay variant of the speaker definition gives the
     * unchanged stock row a second binding, which stays distinct. */
    variant = d8r_scan("d8r-variant", 1, 0, 1, &f, &variant_count);
    CHECK(d8r_copies("gameplay variant", variant, variant_count, d8r_row) == 1);
    CHECK(d8r_copies("gameplay variant", variant, variant_count, d8r_play) == 0);
    /* The probe installed from a map over a library holding only the local
     * editor, so the action row is first reached by the incoming package: with
     * the base row bound to the private inspector (kind 1) its controls stay
     * inherited; with a control inserted before them (kind 2) it needs a real
     * layout rebase. Either way its effective controls keep their order and
     * values, the base row's binding survives, and the increment binds what
     * the rescan and the map's own context bind. */
    for (int kind = 1; kind <= 5; kind++) {
        char label[32], effective[3][512], inspector[128] = "d8rows/name_inspector", expected[512], loud[2][512];
        snprintf(label, sizeof(label), "d8r-data-%d", kind); snprintf(data, sizeof(data), "%s/%s", root, label);
        snprintf(source, sizeof(source), "%s/overrides/local-editor", label); d8r_local_editor(source, kind);
        snprintf(source, sizeof(source), "d8r-map-%d/overrides/probe", kind); d8r_probe(source);
        snprintf(source, sizeof(source), "%s/d8r-map-%d", root, kind);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        CHECK(sh_package_runtime_refresh(data));
        /* Before the probe: no package reaches the action row, yet its native
         * patches already land where their author meant them. */
        library_row *initial = NULL; size_t initial_count = 0;
        initial = library_snapshot(&initial_count);
        CHECK(!d8c_original(initial, initial_count, d8r_probe_row));
        d8r_effective(initial, initial_count, "d8rows/play", effective[0], sizeof(effective[0]));
        int initially_served = d8r_path(initial, initial_count, d8r_play) != NULL;
        if (initially_served != (kind == 2)) d8c_dump("initial", initial, initial_count);
        library_rows_free(initial, initial_count);
        plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
        if (!plan) { fprintf(stderr, "d8r map %d: %s\n", kind, error); goto done; }
        mapped = d8c_compilation_rows(sh_package_map_plan_compilation(plan), &mapped_count);
        snprintf(source, sizeof(source), "%s/overrides/probe", label); d8r_probe(source);
        int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
        if (!prepared || error[0]) fprintf(stderr, "d8r inventory %d: %s\n", kind, error);
        CHECK(prepared && !error[0]); if (!prepared) goto done;
        CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, d8c_activate, &activations));
        CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
        sh_package_map_plan_free(plan); plan = NULL;
        CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
            &(int){0}));
        incremented = library_snapshot(&incremented_count);
        CHECK(sh_package_runtime_refresh(data));
        rescanned = library_snapshot(&rescanned_count);
        size_t aliases = 0, copies = 0;
        int designed = library_rows_designed(incremented, incremented_count, rescanned, rescanned_count, &aliases);
        /* No copy of a stock row no package authors; the base row reparented by
         * the local editor (kind 3) is its author's own variant. */
        size_t played = d8r_copies("increment", incremented, incremented_count, d8r_play);
        copies += d8r_copies("increment", incremented, incremented_count, d8r_row);
        copies += d8r_copies("increment", incremented, incremented_count,
            "generated/decls/snapeditorentitydef/d8rows/play_loud.decl");
        size_t variants = d8r_copies("increment", incremented, incremented_count, d8r_base);
        /* A behavioural base row is its author's own variant, and the action
         * row that package's scope reaches follows it; presentation alone
         * composes into the shared row, which then carries it. */
        const library_row *shared_base = d8r_path(rescanned, rescanned_count, d8r_base);
        int presented = kind != 4 || (shared_base && strstr((const char *)shared_base->body, "displayNameColor"));
        int behaviour = played == (kind == 5) && variants == (kind == 1 || kind == 3 || kind == 5);
        const library_row *installed = d8c_original(incremented, incremented_count, d8r_probe_row);
        const library_row *again = d8c_original(rescanned, rescanned_count, d8r_probe_row);
        const library_row *authored = d8c_original(mapped, mapped_count, d8r_probe_row);
        int bound = installed && again && authored && !strcmp(installed->path, again->path) &&
            !strcmp(authored->path, again->path) && d8c_originals(incremented, incremented_count, d8r_probe_row) == 1;
        /* The local editor's faithful copy of the base row (kind 1) binds its
         * private inspector as that scope's own variant; the shipped base row
         * the probe's children inherit keeps the shipped inspector. */
        if (kind == 1) {
            const library_row *own = NULL;
            for (size_t i = 0; i < rescanned_count; i++)
                if (rescanned[i].original && !strcmp(rescanned[i].original, d8r_base) && strcmp(rescanned[i].path, d8r_base))
                    own = rescanned + i;
            CHECK(own && strstr((const char *)own->body, "inspector = \"smp/") && !d8r_path(rescanned, rescanned_count, d8r_base));
        }
        snprintf(expected, sizeof(expected), "%sdisplayName:%s|actionDelay|volume", kind == 2 ? "comment|" : "", inspector);
        if (strcmp(effective[0], expected) || initially_served != (kind == 2))
            fprintf(stderr, "d8r kind %d: initial play %s (served %d), expected %s\n", kind, effective[0],
                initially_served, expected);
        CHECK(!strcmp(effective[0], expected) && initially_served == (kind == 2));
        d8r_effective(incremented, incremented_count, "d8rows/play_loud", loud[0], sizeof(loud[0]));
        d8r_effective(rescanned, rescanned_count, "d8rows/play_loud", loud[1], sizeof(loud[1]));
        if (strncmp(loud[0], expected, strlen(expected)) || strcmp(loud[0] + strlen(expected), "|loudness") ||
            strcmp(loud[0], loud[1]))
            fprintf(stderr, "d8r kind %d: play_loud increment %s, rescan %s\n", kind, loud[0], loud[1]);
        CHECK(!strncmp(loud[0], expected, strlen(expected)) && !strcmp(loud[0] + strlen(expected), "|loudness") &&
            !strcmp(loud[0], loud[1]));
        d8r_effective(incremented, incremented_count, "d8rows/play", effective[0], sizeof(effective[0]));
        d8r_effective(rescanned, rescanned_count, "d8rows/play", effective[1], sizeof(effective[1]));
        d8r_effective(rescanned, rescanned_count, "d8rows/input_base", effective[2], sizeof(effective[2]));
        /* Same layout: inherited; a real layout change: served rebased. */
        int served = d8r_path(incremented, incremented_count, d8r_play) != NULL;
        int rebased = d8r_path(rescanned, rescanned_count, d8r_play) != NULL;
        int right = !strcmp(effective[0], expected) && !strcmp(effective[1], expected) &&
            served == (kind == 2) && rebased == (kind == 2);
        if (!designed || aliases || copies || !behaviour || !presented || !bound || !right) {
            fprintf(stderr, "d8r kind %d: play increment %s, rescan %s, expected %s; base %s; served %d/%d\n", kind,
                effective[0], effective[1], expected, effective[2], served, rebased);
            d8c_dump("increment", incremented, incremented_count); d8c_dump("rescan", rescanned, rescanned_count);
            d8c_dump("map", mapped, mapped_count);
        }
        CHECK(designed && !aliases && !copies && behaviour && presented && bound && right);
        library_rows_free(incremented, incremented_count); incremented = NULL; incremented_count = 0;
        library_rows_free(rescanned, rescanned_count); rescanned = NULL; rescanned_count = 0;
        library_rows_free(mapped, mapped_count); mapped = NULL; mapped_count = 0;
        sh_package_runtime_test_dispose(); cleanup_resources(data);
        snprintf(data, sizeof(data), "%s/d8r-map-%d", root, kind); cleanup_resources(data);
    }
done:
    library_rows_free(full, full_count); library_rows_free(moved, moved_count);
    library_rows_free(variant, variant_count);
    library_rows_free(incremented, incremented_count); library_rows_free(rescanned, rescanned_count);
    library_rows_free(mapped, mapped_count);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* Row-level presentation composes into the shared editor entry only where
 * every package difference is a presentation field (displayNameColor). Opposing
 * colours resolve to the first authored position, whichever package discovery
 * meets first; a member that also changes behaviour stays its author's own
 * variant with its own colour, which the shared entry does not take; a member
 * that omits a shipped colour cannot be represented as shared presentation and
 * stays a variant, the shared entry keeping the shipped colour. */
static const char *d8p_edit_text(const char *extra)
{
    static char text[2048];
    const char *edit = strstr(d8r_base_text, "edit = {");
    snprintf(text, sizeof(text), "{\n\tedit = {\n\t\t%s\n%s", extra, edit + strlen("edit = {") + 1);
    return text;
}
static void d8p_package(const char *root_name, const char *folder, const char *path, const char *text)
{
    char prefix[MAX_PATH], descriptor[256];
    snprintf(prefix, sizeof(prefix), "%s/overrides/%s", root_name, folder);
    snprintf(descriptor, sizeof(descriptor), "{\"id\":\"d8p-%s\",\"name\":\"D8 presentation %s\"}", folder, folder);
    d8c_file(prefix, "package.json", descriptor);
    d8c_file(prefix, path, text);
}
static library_row *d8p_scan(const char *name, admission_fixture *f, size_t *count)
{
    char data[MAX_PATH];
    sh_decl_registry_source registry = {f, admission_read, (uintptr_t)f->registry, NULL};
    snprintf(data, sizeof(data), "%s/%s", root, name);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f->reflection);
    sh_package_runtime_test_installed_links(1);
    if (!sh_package_runtime_refresh(data)) {
        char error[1024];
        sh_package_runtime_error(error, sizeof(error));
        fprintf(stderr, "%s: refresh failed: %s\n", name, error);
        CHECK(0);
    }
    library_row *rows = library_snapshot(count);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    return rows;
}
/* The live set_bounce_pad_destination shape: a stock child of the base row
 * patches one sparse slot with a picker inspector whose allowed entry shows
 * an inspector a package overrides (bounce_pad_dest_picker names
 * bounce_pad_destination, which shows display_name_inspector); the package's
 * own stock row reaches the child, so the baseline scope reaches it too. No
 * package authors the child, the picker or the entry, so nothing composes
 * into them: the package's scope binds its picker (and the entry and child)
 * as its own variants, and every scope binding what the shipped rows bind
 * shares them. Before, the package scope's copy of the child contributed its
 * binding to the shipped one, which then was staged alone and collided with
 * the package scope's certified copy (the refused boot). A second package
 * changing the speaker the rows place is a gameplay-bound scope: it binds
 * its own variants beside the first (two valid variants, not incompatible
 * edits). Checked with the caller composing before (a_caller) and after
 * (z_caller) the entry its picker allows, in both package discovery orders,
 * and across an installation increment, a full rescan and a map's own
 * context; the package's placement changes stay its own variants. */
static const char *const d8t_callers[] = {"generated/decls/snapeditorentitydef/d8rows/a_caller.decl",
    "generated/decls/snapeditorentitydef/d8rows/z_caller.decl"};
static const char *d8t_host_row = "generated/decls/snapeditorentitydef/d8host/host.decl";
static const char *d8t_caller_host = "generated/decls/snapeditorentitydef/d8rows/caller_host.decl";
/* A second package whose own row reaches both callers; with gameplay it also
 * changes the speaker definition the rows place, in its own scope. */
static const char *const d8t_bundle[] = {"snapeditorentitydef/d8rows/a_caller", "snapeditorentitydef/d8rows/z_caller",
    "snappropertyinspector_text/d8rows/x_picker", "snappropertyinspector_text/d8rows/y_picker",
    "snapeditorentitydef/d8rows/a_dest", "snapeditorentitydef/d8rows/z_dest"};
static void d8t_host(const char *prefix, int gameplay);
/* The host package, optionally bundling byte-faithful copies of the shipped
 * callers, pickers and destinations instead of reaching them as stock. */
static void d8t_host_bundle(const char *prefix, int gameplay, int bundle)
{
    if (bundle)
        for (size_t i = 0; i < sizeof(d8t_bundle) / sizeof(*d8t_bundle); i++) {
            char path[256], relative[300];
            unsigned char *copy = NULL; size_t copy_length = 0;
            snprintf(path, sizeof(path), "generated/decls/%s.decl", d8t_bundle[i]);
            snprintf(relative, sizeof(relative), "assets/%s", path);
            if (d8r_baseline(NULL, path, &copy, &copy_length) == 1) d8c_file(prefix, relative, (const char *)copy);
            free(copy);
        }
    d8t_host(prefix, gameplay);
}
static void d8t_host(const char *prefix, int gameplay)
{
    d8c_file(prefix, "package.json", "{\"id\":\"d8-host\",\"name\":\"D8 host\"}");
    d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8host/host.decl",
        "{ edit = { entityDef = \"d8rows/speaker\"; inputs = { num = 2; item[0] = \"d8rows/a_caller\"; "
        "item[1] = \"d8rows/z_caller\"; } } }");
    if (gameplay)
        d8c_file(prefix, "assets/generated/decls/entitydef/d8rows/speaker.decl",
            "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
}
static library_row *d8t_scan(const char *name, const char *editor, int kind, const char *host, int gameplay,
    int bundle, admission_fixture *f, size_t *count)
{
    char prefix[MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s/overrides/%s", name, editor); d8r_local_editor(prefix, kind);
    if (host) { snprintf(prefix, sizeof(prefix), "%s/overrides/%s", name, host); d8t_host_bundle(prefix, gameplay, bundle); }
    return d8p_scan(name, f, count);
}
/* The private copies of one stock row in a library, joined by '|' in order. */
static void d8t_copy_paths(const library_row *rows, size_t count, const char *stock, char *out, size_t capacity)
{
    *out = 0;
    for (size_t i = 0; i < count; i++)
        if (rows[i].original && !strcmp(rows[i].original, stock) && strcmp(rows[i].path, stock)) {
            size_t used = strlen(out);
            snprintf(out + used, capacity - used, "%s%s", used ? "|" : "", rows[i].path);
        }
}
static void test_picker_topology(void)
{
    static const struct { const char *name, *editor, *host; } layouts[] = {
        {"d8t-alone", "local-editor", NULL}, {"d8t-order-0", "a-editor", "z-host"},
        {"d8t-order-1", "z-editor", "a-host"}};
    char data[MAX_PATH], source[MAX_PATH], error[2048], paths[2][2][1024];
    admission_fixture f; d8r_setup(&f);
    library_row *rows = NULL, *incremented = NULL, *rescanned = NULL, *mapped = NULL;
    size_t count = 0, incremented_count = 0, rescanned_count = 0, mapped_count = 0;
    sh_package_map_plan *plan = NULL;
    int activations = 0;
    /* The shipped rows stay shipped (nothing composes into them); the package
     * scope's binding makes one variant of each caller, the same one in every
     * layout and discovery order; the host scope shares the shipped callers;
     * the package's placement change to its host row stays its own variant. */
    for (size_t l = 0; l < sizeof(layouts) / sizeof(*layouts); l++) {
        rows = d8t_scan(layouts[l].name, layouts[l].editor, 7, layouts[l].host, 0, 0, &f, &count);
        int right = d8r_copies(layouts[l].name, rows, count, d8t_caller_host) == 1;
        for (size_t k = 0; k < 2; k++) {
            char mine[1024];
            d8t_copy_paths(rows, count, d8t_callers[k], mine, sizeof(mine));
            right = right && !d8r_path(rows, count, d8t_callers[k]) &&
                d8r_copies(layouts[l].name, rows, count, d8t_callers[k]) == 1;
            if (!l) strcpy_s(paths[0][k], sizeof(paths[0][k]), mine);
            else right = right && !strcmp(mine, paths[0][k]);
        }
        if (!right) d8c_dump(layouts[l].name, rows, count);
        CHECK(right);
        library_rows_free(rows, count); rows = NULL;
    }
    /* A caller the package supplies with its own placement stays its own
     * variant; like any supplied row, its scope's bindings (its picker)
     * compose into the shipped entry, which does not take the placement. */
    rows = d8t_scan("d8t-placed", "local-editor", 8, NULL, 0, 0, &f, &count);
    {
        const library_row *shared = d8r_path(rows, count, d8t_callers[0]);
        int right = d8r_copies("d8t-placed", rows, count, d8t_callers[0]) == 1 &&
            d8r_copies("d8t-placed", rows, count, d8t_callers[1]) == 1 && shared &&
            !strstr((const char *)shared->body, "placementFlags");
        if (!right) d8c_dump("d8t-placed", rows, count);
        CHECK(right);
    }
    library_rows_free(rows, count); rows = NULL;
    /* A byte-faithful copy of a caller the package bundles is the shipped
     * caller as its scope reaches it: the same bound variant, under the same
     * private name, as when that scope reaches the stock file (kind 7). */
    rows = d8t_scan("d8t-copied", "local-editor", 9, NULL, 0, 0, &f, &count);
    {
        char mine[1024];
        d8t_copy_paths(rows, count, d8t_callers[0], mine, sizeof(mine));
        int right = !strcmp(mine, paths[0][0]) && !d8r_path(rows, count, d8t_callers[0]);
        if (!right) { fprintf(stderr, "d8t-copied: %s, reached %s\n", mine, paths[0][0]); d8c_dump("d8t-copied", rows, count); }
        CHECK(right);
    }
    library_rows_free(rows, count); rows = NULL;
    /* A caller the package supplies with its own picker choice is an edit to
     * the shipped caller: the shipped entry takes it, and the supplied copy,
     * whose own composition already states it, stages the same output, so
     * the declaration stays one entry (no copy of it). */
    rows = d8t_scan("d8t-rebound", "local-editor", 10, NULL, 0, 0, &f, &count);
    {
        const library_row *shared = d8r_path(rows, count, d8t_callers[0]);
        int right = !d8r_copies("d8t-rebound", rows, count, d8t_callers[0]) && shared &&
            strstr((const char *)shared->body, "d8rows/y_picker") != NULL;
        if (!right) d8c_dump("d8t-rebound", rows, count);
        CHECK(right);
    }
    library_rows_free(rows, count); rows = NULL;
    /* A scope binding a gameplay variant of what the rows place binds its own
     * callers beside the package's: two valid variants, and the shipped rows
     * stay shipped (the refused compile this pins said "incompatible edits"). */
    for (int bundle = 0; bundle < 2; bundle++) {
        const char *name = bundle ? "d8t-gameplay-bundled" : "d8t-gameplay";
        rows = d8t_scan(name, "local-editor", 7, "host", 1, bundle, &f, &count);
        int right = count > 0;
        for (size_t k = 0; k < 2; k++) {
            char mine[1024];
            d8t_copy_paths(rows, count, d8t_callers[k], mine, sizeof(mine));
            right = right && d8r_copies(name, rows, count, d8t_callers[k]) == 2 && !d8r_path(rows, count, d8t_callers[k]);
            if (!bundle) strcpy_s(paths[1][k], sizeof(paths[1][k]), mine);
            else right = right && !strcmp(mine, paths[1][k]);
        }
        if (!right) d8c_dump(name, rows, count);
        CHECK(right);
        library_rows_free(rows, count); rows = NULL;
    }
    /* The host package installed from a map over a library holding only the
     * local editor binds what a full rescan and the map's own context bind. */
    {
        sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
        snprintf(source, sizeof(source), "d8t-data/overrides/local-editor"); d8r_local_editor(source, 7);
        snprintf(source, sizeof(source), "d8t-map/overrides/host"); d8t_host(source, 0);
        snprintf(data, sizeof(data), "%s/d8t-data", root); snprintf(source, sizeof(source), "%s/d8t-map", root);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        CHECK(sh_package_runtime_refresh(data));
        plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
        if (!plan) { fprintf(stderr, "d8t map: %s\n", error); goto done; }
        mapped = d8c_compilation_rows(sh_package_map_plan_compilation(plan), &mapped_count);
        d8t_host("d8t-data/overrides/host", 0);
        int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
        if (!prepared || error[0]) fprintf(stderr, "d8t inventory: %s\n", error);
        CHECK(prepared && !error[0]); if (!prepared) goto done;
        CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, d8c_activate, &activations));
        CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
        sh_package_map_plan_free(plan); plan = NULL;
        CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
            &(int){0}));
        incremented = library_snapshot(&incremented_count);
        CHECK(sh_package_runtime_refresh(data));
        rescanned = library_snapshot(&rescanned_count);
        size_t aliases = 0;
        int designed = library_rows_designed(incremented, incremented_count, rescanned, rescanned_count, &aliases);
        const library_row *installed = d8c_original(incremented, incremented_count, d8t_host_row);
        const library_row *again = d8r_path(rescanned, rescanned_count, d8t_host_row);
        const library_row *authored = d8c_original(mapped, mapped_count, d8t_host_row);
        /* The host's own editor row is a new shared declaration: the
         * increment and the map context alias it alike until the full scan
         * names it (the one designed alias). */
        int bound = installed && again && authored && !strcmp(installed->path, authored->path) && aliases == 1;
        int callers = 1;
        for (size_t k = 0; k < 2; k++) {
            d8t_copy_paths(incremented, incremented_count, d8t_callers[k], paths[0][k], sizeof(paths[0][k]));
            d8t_copy_paths(rescanned, rescanned_count, d8t_callers[k], paths[1][k], sizeof(paths[1][k]));
            callers = callers && !strcmp(paths[0][k], paths[1][k]) && !strchr(paths[0][k], '|') && *paths[0][k] &&
                !d8c_original(mapped, mapped_count, d8t_callers[k]);
        }
        if (!designed || !bound || !callers) {
            fprintf(stderr, "d8t increment: designed %d, aliases %zu, bound %d, callers %d\n", designed, aliases,
                bound, callers);
            d8c_dump("increment", incremented, incremented_count); d8c_dump("rescan", rescanned, rescanned_count);
            d8c_dump("map", mapped, mapped_count);
        }
        CHECK(designed && bound && callers);
    }
done:
    sh_package_map_plan_free(plan);
    library_rows_free(incremented, incremented_count); library_rows_free(rescanned, rescanned_count);
    library_rows_free(mapped, mapped_count);
    sh_package_runtime_test_dispose();
    snprintf(data, sizeof(data), "%s/d8t-data", root); cleanup_resources(data);
    snprintf(data, sizeof(data), "%s/d8t-map", root); cleanup_resources(data);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
static void test_editor_presentation(void)
{
    const char *base_path = "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl";
    const char *green = "displayNameColor = {\n\t\t\tr = 0;\n\t\t\tg = 1;\n\t\t}";
    const char *red = "displayNameColor = {\n\t\t\tr = 1;\n\t\t\tg = 0;\n\t\t}";
    admission_fixture f; d8r_setup(&f);
    library_row *rows; size_t count;
    /* Opposing colours, in both discovery orders: the earlier one wins. */
    for (int reversed = 0; reversed < 2; reversed++) {
        char name[32]; snprintf(name, sizeof(name), "d8p-opposing-%d", reversed);
        d8p_package(name, reversed ? "z-green" : "a-green", base_path, d8p_edit_text(green));
        d8p_package(name, reversed ? "a-red" : "z-red", base_path, d8p_edit_text(red));
        rows = d8p_scan(name, &f, &count);
        const library_row *shared = d8r_path(rows, count, d8r_base);
        const char *wanted = reversed ? "r = 1;" : "g = 1;";
        int right = shared && strstr((const char *)shared->body, wanted) && !d8r_copies(name, rows, count, d8r_base);
        if (!right) d8c_dump(name, rows, count);
        CHECK(right);
        library_rows_free(rows, count);
    }
    /* A presentation-only member beside one that also changes placement. */
    {
        const char *name = "d8p-mixed";
        char mixed[256];
        snprintf(mixed, sizeof(mixed), "%s\n\t\tplacementFlags = \"SNAP_ENTITY_PLACE_FLOOR\";", red);
        d8p_package(name, "a-green", base_path, d8p_edit_text(green));
        d8p_package(name, "m-mixed", base_path, d8p_edit_text(mixed));
        rows = d8p_scan(name, &f, &count);
        const library_row *shared = d8r_path(rows, count, d8r_base), *variant = NULL;
        for (size_t i = 0; i < count; i++)
            if (rows[i].original && !strcmp(rows[i].original, d8r_base) && strcmp(rows[i].path, d8r_base)) variant = rows + i;
        int right = shared && strstr((const char *)shared->body, "g = 1;") && !strstr((const char *)shared->body, "placementFlags") &&
            variant && strstr((const char *)variant->body, "r = 1;") && strstr((const char *)variant->body, "placementFlags");
        if (!right) d8c_dump(name, rows, count);
        CHECK(right);
        library_rows_free(rows, count);
    }
    /* A member omitting the shipped colour stays its own variant, without a
     * colour, alone or beside a peer whose colour the shared entry takes. */
    for (int peer = 0; peer < 2; peer++) {
        char name[32]; snprintf(name, sizeof(name), "d8p-absent-%d", peer);
        const char *tinted_path = "assets/generated/decls/snapeditorentitydef/d8rows/tinted.decl";
        const char *tinted = "generated/decls/snapeditorentitydef/d8rows/tinted.decl";
        d8p_package(name, "a-plain", tinted_path, "{\n\tedit = {\n\t\tentityDef = \"d8rows/speaker\";\n\t}\n}\n");
        if (peer)
            d8p_package(name, "z-green", tinted_path, "{\n\tedit = {\n\t\tdisplayNameColor = {\n\t\t\tr = 0;\n"
                "\t\t\tg = 1;\n\t\t}\n\t\tentityDef = \"d8rows/speaker\";\n\t}\n}\n");
        rows = d8p_scan(name, &f, &count);
        const library_row *shared = d8r_path(rows, count, tinted), *own = NULL;
        for (size_t i = 0; i < count; i++)
            if (rows[i].original && !strcmp(rows[i].original, tinted) && strcmp(rows[i].path, tinted)) own = rows + i;
        int colour = peer ? shared && strstr((const char *)shared->body, "g = 1;") :
            !shared || strstr((const char *)shared->body, "r = 1;");
        int right = colour && d8r_copies(name, rows, count, tinted) == 1 && own &&
            !strstr((const char *)own->body, "displayNameColor");
        if (!right) d8c_dump(name, rows, count);
        CHECK(right);
        library_rows_free(rows, count);
    }
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* The installed B3 shape (zz-acceptance b3-anon-one, b3-anon-two, b3-label)
 * over the base row: two packages restate it with one more control without a
 * path, one label-only and one description-only, and a third changes a stock
 * control's description. An unpaired control without a path takes its
 * identity from its content, so every discovery order serves the same parent,
 * both additions once each after the stock controls, and the same restated
 * shipped descendants, whose own appended controls follow the additions. */
static void pathless_text(char *out, size_t capacity, int which)
{
    snprintf(out, capacity, "{\n\tedit = {\n\t\tpropertySheets = {\n\t\t\tnum = 1;\n\t\t\titem[0] = {\n"
        "\t\t\t\tproperties = {\n\t\t\t\t\tnum = %d;\n\t\t\t\t\titem[0] = {\n\t\t\t\t\t\tpath = \"displayName\";\n"
        "\t\t\t\t\t\tdisplayNameTag = \"#str_name\";\n\t\t\t\t\t\tdescriptionTag = \"#str_name_desc\";\n"
        "\t\t\t\t\t\tinspector = \"d8rows/name_inspector\";\n\t\t\t\t\t}\n\t\t\t\t\titem[1] = {\n"
        "\t\t\t\t\t\tpath = \"actionDelay\";\n\t\t\t\t\t\tdisplayNameTag = \"#str_delay\";\n"
        "\t\t\t\t\t\tdescriptionTag = \"%s\";\n\t\t\t\t\t}\n%s\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n",
        which == 2 ? 2 : 3, which == 2 ? "#str_name_desc" : "#str_delay_desc",
        which == 0 ? "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tdisplayNameTag = \"#str_delay\";\n\t\t\t\t\t}\n" :
        which == 1 ? "\t\t\t\t\titem[2] = {\n\t\t\t\t\t\tdescriptionTag = \"#str_delay_desc\";\n\t\t\t\t\t}\n" : "");
}
/* Slot k of the served base's first page as fields in authored order. */
static void pathless_slots(const library_row *row, char *out, size_t capacity)
{
    sh_decl_node *tree = row ? sh_decl_tree_parse((sh_decl_source){(const char *)row->body, row->length}, NULL, 0) : NULL;
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *sheets = edit ? sh_decl_tree_member(edit, "propertySheets") : NULL;
    const sh_decl_node *page = sheets ? sh_decl_tree_member(sheets, "item[0]") : NULL;
    const sh_decl_node *list = page ? sh_decl_tree_member(page, "properties") : NULL;
    *out = 0;
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        size_t used = strlen(out);
        if (!strcmp(item->key, "num")) { snprintf(out + used, capacity - used, "num=%s", item->value); continue; }
        snprintf(out + used, capacity - used, "|");
        for (const sh_decl_node *field = item->children; field; field = field->next) {
            used = strlen(out);
            snprintf(out + used, capacity - used, "%s%s=%s", field == item->children ? "" : ",", field->key, field->value);
        }
    }
    if (!tree) snprintf(out, capacity, "(unreadable)");
    sh_decl_tree_free(tree);
}
/* The effective first page of an editor entry as the engine's inherited parse
 * builds it from what a library serves (the shipped text where it serves
 * none), every field of each slot: path,label,description,inspector|... An
 * absent field and its stated default (an empty string, unquoted NULL) read
 * alike. */
typedef struct pathless_control { char field[4][96]; } pathless_control;
static int pathless_controls(const library_row *rows, size_t count, const char *name, pathless_control *list,
    size_t *n, unsigned depth)
{
    static const char *const fields[4] = {"path", "displayNameTag", "descriptionTag", "inspector"};
    char path[256];
    unsigned char *body = NULL; size_t length = 0;
    snprintf(path, sizeof(path), "generated/decls/snapeditorentitydef/%s.decl", name);
    const library_row *served = d8r_path(rows, count, path);
    if (served) { body = (unsigned char *)_strdup((const char *)served->body); length = served->length; }
    else if (d8r_baseline(NULL, path, &body, &length) != 1) return 0;
    sh_decl_node *tree = body ? sh_decl_tree_parse((sh_decl_source){(const char *)body, length}, NULL, 0) : NULL;
    free(body);
    if (!tree || depth > 8) { sh_decl_tree_free(tree); return 0; }
    const char *text; size_t text_length; char parent[128];
    int ok = 1;
    *n = 0;
    if (sh_decl_tree_literal(sh_decl_tree_member(tree, "inherit"), &text, &text_length) && text_length < sizeof(parent)) {
        memcpy(parent, text, text_length); parent[text_length] = 0;
        ok = pathless_controls(rows, count, parent, list, n, depth + 1);
    }
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *sheets = edit ? sh_decl_tree_member(edit, "propertySheets") : NULL;
    const sh_decl_node *page = sheets ? sh_decl_tree_member(sheets, "item[0]") : NULL;
    const sh_decl_node *properties = page ? sh_decl_tree_member(page, "properties") : NULL;
    for (const sh_decl_node *entry = properties ? properties->children : NULL; ok && entry; entry = entry->next) {
        size_t target = 0, index = 0;
        if (!strcmp(entry->key, "num")) {
            ok = sh_decl_tree_literal(entry, &text, &text_length) && sscanf(text, "%zu", &target) == 1 && target < 32;
            while (ok && *n < target) memset(list + (*n)++, 0, sizeof(*list));
            if (ok) *n = target;
            continue;
        }
        ok = sscanf(entry->key, "item[%zu]", &index) == 1 && index < 32;
        while (ok && *n <= index) memset(list + (*n)++, 0, sizeof(*list));
        for (const sh_decl_node *field = ok ? entry->children : NULL; field; field = field->next)
            for (size_t k = 0; k < 4; k++) if (!strcmp(field->key, fields[k]) && sh_decl_tree_literal(field, &text, &text_length) &&
                    text_length < sizeof(list[index].field[k])) {
                if (k == 3 && field->value[0] != '"' && text_length == 4 && !memcmp(text, "NULL", 4)) text_length = 0;
                memcpy(list[index].field[k], text, text_length); list[index].field[k][text_length] = 0;
            }
    }
    sh_decl_tree_free(tree); return ok;
}
static void pathless_effective(const library_row *rows, size_t count, const char *name, char *out, size_t capacity)
{
    pathless_control list[32]; size_t n = 0;
    *out = 0;
    if (!pathless_controls(rows, count, name, list, &n, 0)) { snprintf(out, capacity, "(unreadable)"); return; }
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, capacity - used, "%s%s,%s,%s,%s", i ? "|" : "", list[i].field[0], list[i].field[1],
            list[i].field[2], list[i].field[3]);
    }
}
static void test_editor_pathless_discovery(void)
{
    static const char *const names[] = {"one", "two", "label"};
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    const char *base_path = "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl";
    const char *served[] = {d8r_base, d8r_play, "generated/decls/snapeditorentitydef/d8rows/play_loud.decl"};
    char *first[3] = {0}, first_slots[1024] = "", first_play[512] = "", first_loud[512] = "";
    size_t first_length[3] = {0};
    static const char *const shipped[] = {"d8rows/extra_base", "d8rows/input_base", "d8rows/play",
        "d8rows/play_loud", "d8rows/speaker_row"};
    admission_fixture f; d8r_setup(&f);
    sh_package_runtime_test_editor_rows(shipped, sizeof(shipped) / sizeof(*shipped));
    for (size_t o = 0; o < 6; o++) {
        char name[32], folder[16], text[2048], slots[1024], play[1024], loud[1024], parent[1024], expected[3][1024];
        size_t count = 0;
        snprintf(name, sizeof(name), "b3d-%zu", o);
        for (size_t k = 0; k < 3; k++) {
            snprintf(folder, sizeof(folder), "%c-%s", 'a' + (int)k, names[orders[o][k]]);
            pathless_text(text, sizeof(text), orders[o][k]);
            d8p_package(name, folder, base_path, text);
        }
        library_row *rows = d8p_scan(name, &f, &count);
        const library_row *base = d8r_path(rows, count, d8r_base);
        pathless_slots(base, slots, sizeof(slots));
        pathless_effective(rows, count, "d8rows/input_base", parent, sizeof(parent));
        pathless_effective(rows, count, "d8rows/play", play, sizeof(play));
        pathless_effective(rows, count, "d8rows/play_loud", loud, sizeof(loud));
        int copies = 0;
        for (size_t s = 0; s < 3; s++) copies += (int)d8r_copies(name, rows, count, served[s]);
        /* Every field of every effective control: the parent, the child and
         * the grandchild each hold both additions once, the label-only and the
         * description-only one, after the stock controls (in their content
         * order), the description change, and the descendants' own controls
         * after them. No slot identity leaks into a served text. */
        int leaked = 0;
        for (size_t s = 0; s < 3; s++) {
            const library_row *row = d8r_path(rows, count, served[s]);
            leaked |= row && strstr((const char *)row->body, "#snapmap-plus-slot") != NULL;
        }
        const char *stock = "displayName,#str_name,#str_name_desc,d8rows/name_inspector|actionDelay,#str_delay,#str_name_desc,";
        const char *label = ",#str_delay,,", *description = ",,#str_delay_desc,";
        const char *own[3] = {"", "|volume,#str_volume,#str_volume_desc,", "|volume,#str_volume,#str_volume_desc,|loudness,,,"};
        int found[3] = {0}, order[3] = {-1, -1, -1};
        for (size_t e = 0; e < 3; e++) for (int swap = 0; swap < 2 && !found[e]; swap++) {
            snprintf(expected[e], sizeof(expected[e]), "%s|%s|%s%s", stock, swap ? label : description,
                swap ? description : label, own[e]);
            if ((found[e] = !strcmp(e == 0 ? parent : e == 1 ? play : loud, expected[e]))) order[e] = swap;
        }
        int shape = base && !copies && !leaked && !strncmp(slots, "num=4|", 6) && found[0] && found[1] && found[2] &&
            order[1] == order[0] && order[2] == order[0];
        if (!shape) {
            fprintf(stderr, "pathless discovery %zu: copies %d leaked %d\n  served base %s\n  parent %s\n  child %s\n"
                "  grandchild %s\n", o, copies, leaked, slots, parent, play, loud);
            for (size_t s = 0; s < 3; s++) {
                const library_row *row = d8r_path(rows, count, served[s]);
                fprintf(stderr, "  %s: %.*s\n", served[s], row ? (int)row->length : 0, row ? (const char *)row->body : "");
            }
        }
        CHECK(shape);
        /* Every order serves the same bytes at the same paths. */
        for (size_t s = 0; s < 3; s++) {
            const library_row *row = d8r_path(rows, count, served[s]);
            if (!o) {
                if (row && (first[s] = malloc(row->length + 1))) {
                    memcpy(first[s], row->body, row->length); first[s][row->length] = 0; first_length[s] = row->length;
                }
                continue;
            }
            int same = row ? first[s] && first_length[s] == row->length && !memcmp(first[s], row->body, row->length) : !first[s];
            if (!same) fprintf(stderr, "pathless discovery %zu: %s differs from order 0\n", o, served[s]);
            CHECK(same);
        }
        if (!o) {
            snprintf(first_slots, sizeof(first_slots), "%s", slots);
            snprintf(first_play, sizeof(first_play), "%s", play); snprintf(first_loud, sizeof(first_loud), "%s", loud);
        } else CHECK(!strcmp(slots, first_slots) && !strcmp(play, first_play) && !strcmp(loud, first_loud));
        library_rows_free(rows, count);
    }
    for (size_t s = 0; s < 3; s++) free(first[s]);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* Two packages restate a shipped control without a path, one leaving the path
 * out and one stating its default (path = ""), each adding a control of its
 * own: both name the shipped control, so it stays one control in either
 * discovery order, with its own payload or the one package's edit to it.
 * Over an inherited path the two differ: the stated default is a reset. */
static void test_editor_pathless_default_identity(void)
{
    const char *path = "assets/generated/decls/snapeditorentitydef/d8rows/anon_base.decl";
    const char *served = "generated/decls/snapeditorentitydef/d8rows/anon_base.decl";
    admission_fixture f; d8r_setup(&f);
    for (int edited = 0; edited < 2; edited++) {
        char *first = NULL; size_t first_length = 0;
        for (int reversed = 0; reversed < 2; reversed++) {
            char name[48], omits[1024], states[1024], expected[512], effective[1024];
            size_t count = 0;
            snprintf(omits, sizeof(omits), "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
                "item[0] = { path = \"displayName\"; displayNameTag = \"#str_name\"; } "
                "item[1] = { displayNameTag = \"#str_anon\"; descriptionTag = \"%s\"; } "
                "item[2] = { path = \"omitted\"; displayNameTag = \"#str_omitted\"; } } } } } }",
                edited ? "#str_anon_edit" : "#str_anon_desc");
            snprintf(states, sizeof(states), "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
                "item[0] = { path = \"displayName\"; displayNameTag = \"#str_name\"; } "
                "item[1] = { path = \"\"; displayNameTag = \"#str_anon\"; descriptionTag = \"#str_anon_desc\"; } "
                "item[2] = { path = \"stated\"; displayNameTag = \"#str_stated\"; } } } } } }");
            snprintf(name, sizeof(name), "b3i-%d-%d", edited, reversed);
            d8p_package(name, reversed ? "z-omits" : "a-omits", path, omits);
            d8p_package(name, reversed ? "a-states" : "z-states", path, states);
            library_row *rows = d8p_scan(name, &f, &count);
            const library_row *row = d8r_path(rows, count, served);
            pathless_effective(rows, count, "d8rows/anon_base", effective, sizeof(effective));
            snprintf(expected, sizeof(expected), "displayName,#str_name,,|,#str_anon,%s,|", edited ? "#str_anon_edit" : "#str_anon_desc");
            size_t anonymous = 0;
            for (const char *at = effective; (at = strstr(at, "#str_anon,")); at++) anonymous++;
            int one = !strncmp(effective, expected, strlen(expected)) && strstr(effective, "|omitted,#str_omitted,,") &&
                strstr(effective, "|stated,#str_stated,,") && anonymous == 1 &&
                row && !strstr((const char *)row->body, "#snapmap-plus-slot") && !d8r_copies(name, rows, count, served);
            int same = row && (!first || (first_length == row->length && !memcmp(first, row->body, row->length)));
            if (!one || !same)
                fprintf(stderr, "default identity (edited %d, reversed %d): %s\n  %.*s\n", edited, reversed, effective,
                    row ? (int)row->length : 0, row ? (const char *)row->body : "");
            CHECK(one && same);
            if (row && !first && (first = malloc(row->length + 1))) {
                memcpy(first, row->body, row->length); first[row->length] = 0; first_length = row->length;
            }
            library_rows_free(rows, count);
        }
        free(first);
    }
    /* Over an inherited control with a path, a statement leaving the path out
     * keeps the inherited one and one stating its default clears it; every
     * other field left out is still inherited. As a sparse patch of a shipped
     * child (form 0), a whole restatement of one stating its whole list (1),
     * and a whole restatement of one stating a sparse patch (2). */
    for (int form = 0; form < 3; form++) for (int clears = 0; clears < 2; clears++) {
        char name[48], text[1024], expected[512], effective[1024];
        int whole = form != 0;
        const char *row_name = form == 1 ? "d8rows/full_child" : "d8rows/play";
        const char *reset = clears ? "path = \"\"; " : "", *label = clears ? "#str_cleared" : "#str_kept";
        char file[160], served_path[160];
        size_t count = 0;
        if (form == 1)
            snprintf(text, sizeof(text), "{ inherit = \"d8rows/input_base\"; edit = { propertySheets = { num = 1; item[0] = { "
                "properties = { num = 3; item[0] = { path = \"displayName\"; } item[1] = { %sdisplayNameTag = \"%s\"; } "
                "item[2] = { path = \"volume\"; } } } } } }", reset, label);
        else if (form == 2)
            snprintf(text, sizeof(text), "{ inherit = \"d8rows/input_base\"; edit = { entityDef = \"d8rows/play_action\"; "
                "propertySheets = { num = 1; item[0] = { properties = { num = 3; item[0] = { path = \"displayName\"; } "
                "item[1] = { %sdisplayNameTag = \"%s\"; } item[2] = { path = \"volume\"; displayNameTag = \"#str_volume\"; "
                "descriptionTag = \"#str_volume_desc\"; } } } } } }", reset, label);
        else
            snprintf(text, sizeof(text), "{ inherit = \"d8rows/input_base\"; edit = { entityDef = \"d8rows/play_action\"; "
                "propertySheets = { item[0] = { properties = { num = 3; item[1] = { %sdisplayNameTag = \"%s\"; } "
                "item[2] = { path = \"volume\"; displayNameTag = \"#str_volume\"; descriptionTag = \"#str_volume_desc\"; } "
                "} } } } }", reset, label);
        snprintf(file, sizeof(file), "assets/generated/decls/snapeditorentitydef/%s.decl", row_name);
        snprintf(served_path, sizeof(served_path), "generated/decls/snapeditorentitydef/%s.decl", row_name);
        snprintf(name, sizeof(name), "b3i-inherited-%d-%d", form, clears);
        d8p_package(name, "a-row", file, text);
        library_row *rows = d8p_scan(name, &f, &count);
        const library_row *row = d8r_path(rows, count, served_path);
        pathless_effective(rows, count, row_name, effective, sizeof(effective));
        snprintf(expected, sizeof(expected), "displayName,#str_name,#str_name_desc,d8rows/name_inspector|%s,%s,#str_delay_desc,|%s",
            clears ? "" : "actionDelay", label, form == 1 ? "volume,,," : "volume,#str_volume,#str_volume_desc,");
        int right = row && !strcmp(effective, expected) && !strstr((const char *)row->body, "#snapmap-plus-slot");
        if (!right) fprintf(stderr, "inherited path (form %d, clears %d): %s\n  %.*s\n", form, clears, effective,
            row ? (int)row->length : 0, row ? (const char *)row->body : "");
        CHECK(right);
        library_rows_free(rows, count);
    }
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* The layout prepass keeps the owner contract (F1/A1): a package whose relaid
 * editor collection cannot be read is left out by the phase that attributes it
 * (not served as a coverage gap, or skipped on its own malformed content) and
 * reported as itself, while its healthy peers' relays are found and served; a
 * stock parent the prepass cannot read is an environment failure that leaves
 * out and remembers nobody. */
static int d8r_unreadable_parent(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (!strcmp(path, "generated/decls/snapeditorentitydef/d8rows/envparent.decl")) { *body = NULL; *length = 0; return -1; }
    return d8r_baseline(context, path, body, length);
}
static void test_relay_owners(void)
{
    static const char *const shipped[] = {"d8rows/envchild", "d8rows/envparent", "d8rows/input_base", "d8rows/play",
        "d8rows/play_loud", "d8rows/speaker_row"};
    /* Collections the prepass cannot read: shapes the readers do not support
     * yet (the package is not served, as a coverage gap) and one they reject
     * as malformed (the package is skipped on its own content). */
    static const struct { const char *text; int malformed; } defects[] = {
        {"{ edit = { propertySheets = { num = 1; item[0] = { properties = { bogus = 1; } } } } }", 0},
        {"{ edit = { propertySheets = { num = 1; item[0] = { properties = 5; } } } }", 0},
        {"{ edit = { propertySheets = 7; } }", 0},
        {"{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; item[0] = 5; } } } } }", 0},
        {"{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; item[0] = { path = { x = 1; } } } } } } }", 1},
    };
    char data[MAX_PATH], effective[512], *summary = NULL;
    admission_fixture f; d8r_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_runtime_test_editor_rows(shipped, sizeof(shipped) / sizeof(*shipped));
    for (size_t d = 0; d < sizeof(defects) / sizeof(*defects); d++) {
    char folder[64];
    snprintf(folder, sizeof(folder), "d8o-data-%zu", d);
    snprintf(data, sizeof(data), "%s/overrides/a-bad/package.json", folder);
    create(data, "{\"id\":\"d8-bad-editor\",\"name\":\"D8 bad editor\"}");
    snprintf(data, sizeof(data), "%s/overrides/a-bad/assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl", folder);
    create(data, defects[d].text);
    snprintf(data, sizeof(data), "%s/overrides/m-local-editor", folder); d8r_local_editor(data, 2);
    snprintf(data, sizeof(data), "%s/overrides/z-probe", folder); d8r_probe(data);
    snprintf(data, sizeof(data), "%s/%s", root, folder);
    for (int process = 0; process < 2; process++) {
        library_row *rows = NULL; size_t count = 0;
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        skipped_packages = left_out_packages = 0;
        int refreshed = sh_package_runtime_refresh(data);
        rows = refreshed ? library_snapshot(&count) : NULL;
        d8r_effective(rows, count, "d8rows/play", effective, sizeof(effective));
        summary = sh_package_runtime_summary();
        /* Every problem line names the bad package only. */
        int reported = 0, blamed_peer = 0;
        for (const char *line = summary; line && *line;) {
            const char *end = strchr(line, '\n');
            size_t length = end ? (size_t)(end - line) : strlen(line);
            char text[1024];
            snprintf(text, sizeof(text), "%.*s", (int)(length < sizeof(text) - 1 ? length : sizeof(text) - 1), line);
            if (strstr(text, defects[d].malformed ? "Skipped local package" : "is not served yet")) {
                reported |= strstr(text, "a-bad") != NULL;
                blamed_peer |= strstr(text, "m-local-editor") || strstr(text, "z-probe");
            }
            line = end ? end + 1 : NULL;
        }
        int remembered = defects[d].malformed ? skipped_packages + left_out_packages == 1 :
            !skipped_packages && !left_out_packages;
        reported = reported && !blamed_peer;
        if (!refreshed || !reported || !remembered || strcmp(effective, "comment|displayName:d8rows/name_inspector|actionDelay|volume")) {
            fprintf(stderr, "d8r owners %d: refreshed %d, play %s, left out %u, summary %s\n", process, refreshed,
                effective, left_out_packages, summary ? summary : "(none)");
            d8c_dump("owners", rows, count);
        }
        CHECK(refreshed && reported && remembered &&
            !strcmp(effective, "comment|displayName:d8rows/name_inspector|actionDelay|volume") &&
            d8r_path(rows, count, d8r_play) && d8c_original(rows, count, d8r_probe_row));
        free(summary); summary = NULL; library_rows_free(rows, count);
    }
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    }
    /* The same relay whose stock parent cannot be read: nobody is blamed. */
    create("d8o-env/overrides/m-env/package.json", "{\"id\":\"d8-env-editor\",\"name\":\"D8 env editor\"}");
    create("d8o-env/overrides/m-env/assets/generated/decls/snapeditorentitydef/d8rows/envchild.decl",
        "{ inherit = \"d8rows/envparent\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
        "item[1] = { path = \"inserted\"; } item[2] = { path = \"envown\"; } } } } } }");
    snprintf(data, sizeof(data), "%s/d8o-env", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_unreadable_parent, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    skipped_packages = left_out_packages = 0;
    int refreshed = sh_package_runtime_refresh(data);
    if (refreshed || skipped_packages) fprintf(stderr, "d8r owners env: refreshed %d, %u skipped\n", refreshed, skipped_packages);
    CHECK(!refreshed && !skipped_packages);
    /* Readable again, the package is served: nothing was remembered against it. */
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    skipped_packages = left_out_packages = 0;
    refreshed = sh_package_runtime_refresh(data);
    CHECK(refreshed && !skipped_packages && !left_out_packages);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* The shipped roster a positional relay needs is complete or the compile
 * fails as an environment failure: an entry the archive holds but cannot read
 * serves no partial descendant closure and blames or remembers no package, and
 * the next compile after recovery serves the whole closure. An absent or
 * campaign-only name is not an entry; a missing roster only matters when a
 * package moves a stock entry's inherited controls. */
static int d8r_roster_reader(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (context && !strcmp(path, "generated/decls/snapeditorentitydef/d8rows/play_loud.decl")) {
        *body = NULL; *length = 0; return -1;
    }
    if (!strcmp(path, "generated/decls/snapeditorentitydef/d8rows/campaign_row.decl")) {
        static const char text[] = "{ inherit = \"d8rows/play\"; edit = { } }";
        *length = sizeof(text) - 1; *body = (unsigned char *)_strdup(text); return *body ? 2 : -1;
    }
    return d8r_baseline(context, path, body, length);
}
static int d8r_roster_refresh(const char *data, admission_fixture *f, sh_package_baseline_reader reader, void *context,
    library_row **rows, size_t *count)
{
    sh_decl_registry_source registry = {f, admission_read, (uintptr_t)f->registry, NULL};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(reader, context);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f->reflection);
    sh_package_runtime_test_installed_links(1);
    skipped_packages = left_out_packages = 0;
    int refreshed = sh_package_runtime_refresh(data);
    *rows = NULL; *count = 0;
    if (refreshed) *rows = library_snapshot(count);
    return refreshed;
}
static void test_relay_roster(void)
{
    static const char *const shipped[] = {"d8rows/absent_row", "d8rows/campaign_row", "d8rows/input_base",
        "d8rows/play", "d8rows/play_loud", "d8rows/speaker_row"};
    const char *loud = "comment|displayName:d8rows/name_inspector|actionDelay|volume|loudness";
    char data[MAX_PATH], effective[512];
    library_row *rows = NULL; size_t count = 0;
    admission_fixture f; d8r_setup(&f);
    d8r_local_editor("d8q-moved/overrides/m-local-editor", 2); d8r_probe("d8q-moved/overrides/z-probe");
    d8r_local_editor("d8q-bound/overrides/m-local-editor", 1); d8r_probe("d8q-bound/overrides/z-probe");
    snprintf(data, sizeof(data), "%s/d8q-moved", root);
    sh_package_runtime_test_editor_rows(shipped, sizeof(shipped) / sizeof(*shipped));
    /* A shipped child the archive holds but cannot read: nothing is served. */
    int refreshed = d8r_roster_refresh(data, &f, d8r_roster_reader, (void *)1, &rows, &count);
    if (refreshed || skipped_packages || left_out_packages)
        fprintf(stderr, "d8r roster unreadable: refreshed %d, %u skipped, %u left out\n", refreshed, skipped_packages,
            left_out_packages);
    CHECK(!refreshed && !skipped_packages && !left_out_packages);
    library_rows_free(rows, count);
    /* Recovered: the whole closure, the absent and campaign names aside. */
    refreshed = d8r_roster_refresh(data, &f, d8r_roster_reader, NULL, &rows, &count);
    d8r_effective(rows, count, "d8rows/play_loud", effective, sizeof(effective));
    if (!refreshed || skipped_packages || left_out_packages || strcmp(effective, loud))
        fprintf(stderr, "d8r roster recovered: refreshed %d, %u skipped, %u left out, play_loud %s\n", refreshed,
            skipped_packages, left_out_packages, effective);
    CHECK(refreshed && !skipped_packages && !left_out_packages && !strcmp(effective, loud) &&
        d8r_path(rows, count, "generated/decls/snapeditorentitydef/d8rows/play_loud.decl") &&
        !d8r_path(rows, count, "generated/decls/snapeditorentitydef/d8rows/campaign_row.decl"));
    library_rows_free(rows, count);
    /* No roster at all: a positional relay cannot be served without it... */
    sh_package_runtime_test_editor_rows_unavailable(1);
    refreshed = d8r_roster_refresh(data, &f, d8r_baseline, NULL, &rows, &count);
    CHECK(!refreshed && !skipped_packages && !left_out_packages);
    library_rows_free(rows, count);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    /* ...while a relay that keeps every control at its slot never reads it. */
    snprintf(data, sizeof(data), "%s/d8q-bound", root);
    refreshed = d8r_roster_refresh(data, &f, d8r_baseline, NULL, &rows, &count);
    CHECK(refreshed && !skipped_packages);
    library_rows_free(rows, count);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_editor_rows_unavailable(0); sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* The production toybox roster (sh_package_runtime_palette) over real library
 * compiles: every served editor entry, then the shipped ones not served, each
 * resolving its inherited fields through the served entries first. A package
 * changes the shipped parent's category. Served in place (an unlinked
 * compile), the untouched shipped child shows the served category too; linked
 * as installed packages are, the change is its author's own private variant,
 * a tile of its own, and the shipped parent and child keep theirs. Either way
 * the new public entry the package adds is a peer under its own name and an
 * entry without a category is no tile. Without a shipped roster the aliases
 * keep their base labels. */
static const char *const roster_editor_rows[] = {"rost/child", "rost/cleared", "rost/hidden", "rost/parent"};
static int roster_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/snapeditorentitydef/rost/parent.decl") ?
            "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_parent\"; "
            "categoryNameTag = \"#str_cat\"; metaCategoryNameTag = \"#str_meta\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/rost/child.decl") ?
            "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/play_action\"; } }" :
        /* A shipped child stating an empty label: its own value, not the parent's. */
        !strcmp(path, "generated/decls/snapeditorentitydef/rost/cleared.decl") ?
            "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/rost/hidden.decl") ?
            "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_hidden\"; } }" : NULL;
    if (!text) return d8r_baseline(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* d8r_setup with the editor entry's toybox label fields. */
static void roster_setup(admission_fixture *f)
{
    static const char *const types[] = {"idList < EditorRefs >", "idDeclEntityDef", "idList < EditorSheet >", "idColor",
        "idStr", "idStr", "idStr", "idStr"};
    static const char *const names[] = {"inputs", "entityDef", "propertySheets", "displayNameColor", "placementFlags",
        "displayNameTag", "categoryNameTag", "metaCategoryNameTag"};
    d8r_setup(f);
    void *container = *(void **)f->reflection, *records = *(void **)((char *)container + 0x20);
    char *none = admission_string(f, "");
    void *table = admission_alloc(f, 9 * 0x48);
    admission_pointer(records, 4 * 0x38 + 0x20, table);
    for (size_t i = 0; i < 8; i++) {
        admission_pointer(table, i * 0x48, admission_string(f, types[i]));
        admission_pointer(table, i * 0x48 + 8, i == 1 ? admission_string(f, "*") : none);
        admission_pointer(table, i * 0x48 + 0x10, admission_string(f, names[i]));
        admission_number(table, i * 0x48 + 0x1c, 8);
    }
}
static const sh_strids_palette_entry *roster_find(const sh_strids_palette_entry *entries, size_t count, const char *row)
{
    const sh_strids_palette_entry *found = NULL;
    for (size_t i = 0; i < count; i++) if (!strcmp(entries[i].row, row)) { CHECK(!found); found = entries + i; }
    return found;
}
static void test_palette_roster(void)
{
    char data[MAX_PATH];
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    create("roster-data/overrides/changer/package.json", "{\"id\":\"local.changer\",\"name\":\"Changer\"}");
    create("roster-data/overrides/changer/assets/generated/decls/snapeditorentitydef/rost/parent.decl",
        "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_parent\"; "
        "categoryNameTag = \"#str_cat_new\"; metaCategoryNameTag = \"#str_meta\"; } }");
    create("roster-data/overrides/changer/assets/generated/decls/snapeditorentitydef/rost/newrow.decl",
        "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_new\"; "
        "categoryNameTag = \"#str_cat\"; metaCategoryNameTag = \"#str_meta\"; } }");
    /* An Add Logic selector states a category but no meta category: it is listed, with an empty meta. */
    create("roster-data/overrides/changer/assets/generated/decls/snapeditorentitydef/rost/act.decl",
        "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_act\"; categoryNameTag = \"#str_acts\"; } }");
    snprintf(data, sizeof(data), "%s/roster-data", root);
    for (int linked = 0; linked < 2; linked++) {
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(linked);
        sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
        skipped_packages = left_out_packages = 0;
        CHECK(sh_package_runtime_refresh(data) && !skipped_packages && !left_out_packages);
        const sh_strids_palette_entry *entries = NULL; size_t count = 0;
        CHECK(sh_package_runtime_palette(NULL, &entries, &count) == 1);
        const sh_strids_palette_entry *parent = roster_find(entries, count, "rost/parent");
        const sh_strids_palette_entry *child = roster_find(entries, count, "rost/child");
        const sh_strids_palette_entry *added = roster_find(entries, count, "rost/newrow"), *variant = NULL;
        const sh_strids_palette_entry *cleared = roster_find(entries, count, "rost/cleared");
        const sh_strids_palette_entry *act = roster_find(entries, count, "rost/act");
        size_t variants = 0;
        for (size_t i = 0; i < count; i++)
            if (!strncmp(entries[i].row, "smp/", 4)) { variants++; variant = entries + i; }
        const char *category = linked ? "#str_cat" : "#str_cat_new";
        int right = parent && !strcmp(parent->category, category) && !strcmp(parent->label, "#str_parent") &&
            child && !strcmp(child->category, category) && !strcmp(child->label, "#str_parent") &&
            !strcmp(child->meta, "#str_meta") &&
            cleared && !*cleared->label && !strcmp(cleared->category, category) &&
            added && !strcmp(added->label, "#str_new") && !strcmp(added->category, "#str_cat") &&
            act && !strcmp(act->label, "#str_act") && !strcmp(act->category, "#str_acts") && !*act->meta &&
            !roster_find(entries, count, "rost/hidden") &&
            variants == (size_t)linked && (!linked || (!strcmp(variant->category, "#str_cat_new") &&
            !strcmp(variant->label, "#str_parent") && !strcmp(variant->meta, "#str_meta")));
        if (!right)
            for (size_t i = 0; i < count; i++)
                fprintf(stderr, "roster %d: %s label %s category %s meta %s\n", linked, entries[i].row, entries[i].label,
                    entries[i].category, entries[i].meta);
        CHECK(right);
        sh_package_runtime_test_editor_rows(NULL, 0);
        CHECK(sh_package_runtime_palette(NULL, &entries, &count) == 0 && !count);
    }
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* Toybox label aliases through the real linked library compile and native
 * conversion. The same installed package (a plain editor entry for its own
 * definition, copying a shipped tile) compiles without and with a native
 * string reservation: with it, the entry's served text carries its label
 * alias and its authored body does not change, and a map use of the
 * definition converts to the same native data, carrier-free, with the same
 * report. The regression this separation fixed is reproduced by serving the
 * alias from the body itself: the entry stops being a plain copy and the use
 * keeps its package. */
static int label_conversion_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{ (void)identity; (void)mapping; if (strncmp(key, "#str_smp_loc_", 13)) return 0; (*(size_t *)context)++; return 1; }
static int label_conversion_global(void *context, const char *key) { (void)context; (void)key; return 1; }
static int label_conversion_convert(const char *label, char **derived, char **report)
{
    char error[2048];
    size_t bound_length = 0, derived_length = 0, count = 99;
    sh_package_occurrence_snapshot *ownership = NULL; sh_mpkg_used *used = NULL;
    const char *json = INLINE_VARIABLES INLINE_ENTITY("conv/tough") "]}";
    *derived = NULL; *report = NULL;
    char *bound = sh_package_runtime_rebind_local_document(json, strlen(json), &bound_length, error, sizeof(error));
    if (!bound) fprintf(stderr, "label conversion %s rebind: %s\n", label, error);
    CHECK(bound != NULL);
    int result = bound ? sh_package_runtime_native_document(NULL, NULL, NULL, 0, bound, bound_length, derived,
        &derived_length, &ownership, &used, &count, report, error, sizeof(error)) : -2;
    int converted = result == 1 && *derived && strstr(*derived, "\"inherit\":\"d8rows/speaker\"") &&
        strstr(*derived, "\"health\":40") && count == 0;
    if (!converted) fprintf(stderr, "label conversion %s: result %d, %zu deliveries: %s\n%s", label, result, count, error,
        *report ? *report : "");
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    sh_package_occurrence_release(ownership); sh_mpkg_used_free(used);
    return result < 0 ? -1 : converted;
}
/* The library's compiled row for the package entry, found by its authored path. */
static sh_compiled_resource *label_conversion_row(const sh_package_compilation *library)
{
    const char *authored = "generated/decls/snapeditorentitydef/conv/tough_row.decl";
    for (size_t i = 0; library && i < library->resource_count; i++) {
        const sh_compiled_resource *resource = library->resources + i;
        const char *path = resource->original_path ? resource->original_path : resource->engine_path;
        if (!strcmp(path, authored)) return (sh_compiled_resource *)resource;
    }
    return NULL;
}
static void test_label_alias_conversion(void)
{
    char data[MAX_PATH];
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    size_t reserved = 0;
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    create("conversion-data/overrides/tough/package.json", "{\"id\":\"local.tough\",\"name\":\"Tough\"}");
    create("conversion-data/overrides/tough/assets/generated/decls/entitydef/conv/tough.decl",
        "{ inherit = \"d8rows/speaker\"; edit = { health = 40; } }");
    create("conversion-data/overrides/tough/assets/generated/decls/snapeditorentitydef/conv/tough_row.decl",
        "{ inherit = \"rost/parent\"; edit = { entityDef = \"conv/tough\"; } }");
    snprintf(data, sizeof(data), "%s/conversion-data", root);
    char *derived[2] = {NULL, NULL}, *report[2] = {NULL, NULL}, *body = NULL;
    size_t body_length = 0;
    for (int labelled = 0; labelled < 2; labelled++) {
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
        sh_package_runtime_test_strings(labelled ? &strings : NULL);
        CHECK(sh_package_runtime_refresh(data));
        const sh_package_compilation *library = sh_package_runtime_library_acquire();
        const sh_compiled_resource *row = label_conversion_row(library);
        CHECK(row && row->body);
        if (row && !labelled) {
            CHECK(!row->presented);
            body = malloc(row->body_length); body_length = row->body_length;
            if (body) memcpy(body, row->body, body_length);
        }
        if (row && labelled) {
            CHECK(body && row->body_length == body_length && !memcmp(row->body, body, body_length));
            CHECK(row->presented && strstr((const char *)row->presented, "displayNameTag = \"#str_smp_loc_"));
        }
        sh_package_runtime_release();
        CHECK(label_conversion_convert(labelled ? "labelled" : "plain", derived + labelled, report + labelled) == 1);
    }
    CHECK(reserved >= 1);
    CHECK(derived[0] && derived[1] && !strcmp(derived[0], derived[1]));
    CHECK(report[0] && report[1] && !strcmp(report[0], report[1]));
    /* The pre-fix shape: the alias served from the body itself, in the
     * compilation the conversion reads. */
    {
        sh_compiled_resource *row = label_conversion_row(sh_package_runtime_acquire());
        unsigned char *authored = row ? row->body : NULL; size_t authored_length = row ? row->body_length : 0;
        if (row && row->presented) { row->body = row->presented; row->body_length = row->presented_length; }
        sh_package_runtime_release();
        char *refused = NULL, *refusal = NULL;
        int kept = label_conversion_convert("decorated body", &refused, &refusal);
        if (kept != 0 || !refusal || !strstr(refusal, "not a plain copy"))
            fprintf(stderr, "decorated body: row %s (%s), result %d\n%s\n%s\n", row ? row->engine_path : "(none)",
                row && row->presented ? "presented" : "no presentation", kept, refused ? refused : "", refusal ? refusal : "");
        CHECK(kept == 0);
        CHECK(refusal && strstr(refusal, "not a plain copy"));
        free(refused); free(refusal);
        row = label_conversion_row(sh_package_runtime_acquire());
        if (row && authored) { row->body = authored; row->body_length = authored_length; }
        sh_package_runtime_release();
    }
    for (int i = 0; i < 2; i++) { free(derived[i]); free(report[i]); }
    free(body);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* Add Logic selector rows (inputs and outputs state a category but no meta category)
 * are aliased and listed like tiles. Two packages supplying one authored spelling with
 * different entity definitions publish two private rows; each gains a label alias naming
 * its package, with the empty meta category, and the served roster lists both with that
 * empty meta and the alias as their label. */
static void test_label_alias_selector(void)
{
    char data[MAX_PATH];
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    size_t reserved = 0, aliases = 0, packages = 0, listed = 0;
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    create("selector-data/overrides/sel-one/package.json", "{\"id\":\"local.sel-one\",\"name\":\"Selector one\"}");
    create("selector-data/overrides/sel-one/assets/generated/decls/snapeditorentitydef/sel/pulse.decl",
        "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_pulse\"; categoryNameTag = \"#str_acts\"; } }");
    create("selector-data/overrides/sel-two/package.json", "{\"id\":\"local.sel-two\",\"name\":\"Selector two\"}");
    create("selector-data/overrides/sel-two/assets/generated/decls/snapeditorentitydef/sel/pulse.decl",
        "{ edit = { entityDef = \"d8rows/play_action\"; displayNameTag = \"#str_pulse\"; categoryNameTag = \"#str_acts\"; } }");
    snprintf(data, sizeof(data), "%s/selector-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(data));
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    for (size_t i = 0; library && i < library->policy.localized_count; i++) {
        sh_json_object mapping = {0}, label = {0};
        const char *raw, *value = library->policy.localized[i].mapping;
        if (sh_json_parse_object(value, strlen(value), 4, &mapping) &&
            (raw = sh_json_object_get(&mapping, "label")) != NULL && sh_json_parse_object(raw, strlen(raw), 3, &label)) {
            const char *meta = sh_json_object_get(&label, "meta"), *category = sh_json_object_get(&label, "category"),
                *sources = sh_json_object_get(&label, "sources");
            if (sources && strstr(sources, "sel/pulse")) {
                aliases++;
                CHECK(meta && !strcmp(meta, "\"\"") && category && !strcmp(category, "\"#str_acts\""));
                packages += (strstr(sources, "Selector one") != NULL) + (strstr(sources, "Selector two") != NULL);
            }
        }
        sh_json_object_free(&label); sh_json_object_free(&mapping);
    }
    sh_package_runtime_release();
    if (aliases != 2 || packages != 2 || reserved < 2)
        fprintf(stderr, "selector aliases: %zu mappings, %zu package names, %zu reservations\n", aliases, packages, reserved);
    CHECK(aliases == 2 && packages == 2 && reserved >= 2);
    const sh_strids_palette_entry *entries = NULL; size_t count = 0;
    CHECK(sh_package_runtime_palette(NULL, &entries, &count) == 1);
    for (size_t i = 0; i < count; i++)
        if (!strncmp(entries[i].row, "smp/", 4) && !strncmp(entries[i].label, "#str_smp_loc_", 13) && !*entries[i].meta &&
            !strcmp(entries[i].category, "#str_acts")) listed++;
    CHECK(listed == 2);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* Label mappings naming one toybox row. An installed package adding an exact
 * copy of another installed package's entry recompiles the library (an
 * increment does not fold such a copy in), and the one published row's alias
 * names both sources; the served roster lists the row once, naming that alias.
 * With two compilations serving one row (a map overlay over the installed
 * library), the overlay serves the map's resource and keeps both mappings: the
 * roster names the map's alias, and the library's stays only for lookup
 * (strids_labels_test decides the texts of that shape). */
#define PR_TEST_EDITOR_PREFIX "generated/decls/snapeditorentitydef/"
static size_t label_mappings_for(const sh_package_compilation *compiled, const char *row, const char *selected,
    char *sources, size_t capacity)
{
    size_t found = 0;
    if (sources && capacity) *sources = 0;
    for (size_t i = 0; compiled && i < compiled->policy.localized_count; i++) {
        const sh_package_localized_string *value = compiled->policy.localized + i;
        sh_json_object mapping = {0}, label = {0};
        const char *raw;
        char name[256]; size_t length = 0;
        if (sh_json_parse_object(value->mapping, strlen(value->mapping), 4, &mapping) &&
            (raw = sh_json_object_get(&mapping, "label")) != NULL &&
            sh_json_parse_object(raw, strlen(raw), 3, &label) && (raw = sh_json_object_get(&label, "row")) != NULL &&
            sh_json_decode_string(raw, strlen(raw), name, sizeof(name), &length) && !strcmp(name, row)) {
            found++;
            raw = sh_json_object_get(&label, "sources");
            if (selected && sources && raw && !_stricmp(value->key, selected)) snprintf(sources, capacity, "%s", raw);
        }
        sh_json_object_free(&label); sh_json_object_free(&mapping);
    }
    return found;
}
/* The served row authored at conv/tough_row: its published name and the alias
 * its presented text selects. */
static const sh_compiled_resource *label_served_row(const sh_package_compilation *compiled, char *name, size_t name_capacity,
    char *selected, size_t selected_capacity, size_t *rows)
{
    const sh_compiled_resource *served = label_conversion_row(compiled);
    *name = 0; *selected = 0; *rows = 0;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        const char *path = resource->original_path ? resource->original_path : resource->engine_path;
        if (!strcmp(path, "generated/decls/snapeditorentitydef/conv/tough_row.decl")) (*rows)++;
    }
    if (served && strlen(served->engine_path) > sizeof(PR_TEST_EDITOR_PREFIX) + 4) {
        size_t n = strlen(served->engine_path) - (sizeof(PR_TEST_EDITOR_PREFIX) - 1) - 5;
        if (n < name_capacity) { memcpy(name, served->engine_path + sizeof(PR_TEST_EDITOR_PREFIX) - 1, n); name[n] = 0; }
    }
    const char *tag = served && served->presented ? strstr((const char *)served->presented, "#str_smp_loc_") : NULL;
    if (tag) {
        size_t n = strcspn(tag, "\"");
        if (n < selected_capacity) { memcpy(selected, tag, n); selected[n] = 0; }
    }
    return served;
}
static void test_label_alias_exact_install(void)
{
    char data[MAX_PATH], name[256], selected[128], first[128], sources[1024];
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    size_t reserved = 0, rows = 0;
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    const char *row = "{ inherit = \"rost/parent\"; edit = { entityDef = \"conv/tough\"; } }";
    const char *def = "{ inherit = \"d8rows/speaker\"; edit = { health = 40; } }";
    create("exact-data/overrides/first/package.json", "{\"id\":\"local.first\",\"name\":\"First\"}");
    create("exact-data/overrides/first/assets/generated/decls/entitydef/conv/tough.decl", def);
    create("exact-data/overrides/first/assets/generated/decls/snapeditorentitydef/conv/tough_row.decl", row);
    snprintf(data, sizeof(data), "%s/exact-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(data));
    label_served_row(sh_package_runtime_acquire(), name, sizeof(name), first, sizeof(first), &rows);
    sh_package_runtime_release();
    CHECK(rows == 1 && *first);
    create("exact-data/overrides/second/package.json", "{\"id\":\"local.second\",\"name\":\"Second\"}");
    create("exact-data/overrides/second/assets/generated/decls/entitydef/conv/tough.decl", def);
    create("exact-data/overrides/second/assets/generated/decls/snapeditorentitydef/conv/tough_row.decl", row);
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data));
    CHECK(library_compiles >= 1 && !install_refreshes);
    const sh_package_compilation *current = sh_package_runtime_acquire();
    const sh_compiled_resource *served = label_served_row(current, name, sizeof(name), selected, sizeof(selected), &rows);
    size_t mappings = label_mappings_for(current, name, selected, sources, sizeof(sources));
    sh_package_runtime_release();
    const sh_strids_palette_entry *entries = NULL; size_t count = 0;
    CHECK(sh_package_runtime_palette(NULL, &entries, &count) == 1);
    const sh_strids_palette_entry *listed = *name ? roster_find(entries, count, name) : NULL;
    int right = rows == 1 && served && *selected && strcmp(selected, first) && mappings == 1 &&
        strstr(sources, "local.first") && strstr(sources, "local.second") && listed && !_stricmp(listed->label, selected);
    if (!right) fprintf(stderr, "exact install: %zu row(s) %s, %zu mapping(s), selected %s (was %s), sources %s, roster %s\n",
        rows, name, mappings, selected, first, sources, listed ? listed->label : "(absent)");
    CHECK(right);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(data);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
static void label_overlay_row(sh_compiled_resource *row, const char *path, const char *body, const char *presented)
{
    memset(row, 0, sizeof(*row));
    row->engine_path = path; row->type = "snapeditorentitydef"; row->name = "smp/dup"; row->source = SIZE_MAX;
    row->body = (unsigned char *)body; row->body_length = strlen(body);
    row->presented = (unsigned char *)presented; row->presented_length = strlen(presented);
}
static void label_overlay_mapping(sh_package_policy *policy, const char *key, const char *name, const char *id)
{
    char mapping[1024], error[256] = "";
    snprintf(mapping, sizeof(mapping), "{\"native\":\"#str_parent\",\"locales\":{},\"label\":{\"row\":\"smp/dup\","
        "\"category\":\"#str_cat\",\"meta\":\"#str_meta\",\"sources\":{\"0\":{\"name\":\"%s\",\"id\":\"%s\","
        "\"component\":\"\",\"row\":\"chime/row\"}}}}", name, id);
    int ok = sh_package_policy_localization(policy, key, mapping, NULL, error, sizeof(error));
    if (!ok) fprintf(stderr, "overlay mapping %s: %s\n", key, error);
    CHECK(ok);
}
static void test_label_alias_overlay(void)
{
    char error[512] = "", map_root[MAX_PATH];
    const char *path = "generated/decls/snapeditorentitydef/smp/dup.decl";
    const char *body = "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; } }";
    const char *local_text = "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; "
        "displayNameTag = \"#str_smp_loc_local\"; } }";
    const char *map_text = "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; "
        "displayNameTag = \"#str_smp_loc_map\"; } }";
    char local_root[MAX_PATH];
    create("overlay-local/overrides", NULL);
    create("overlay-map/overrides/mapped/package.json", "{\"id\":\"map.chimes\",\"name\":\"Map chimes\"}");
    create("overlay-map/overrides/mapped/assets/generated/decls/entitydef/chime/def.decl", "{ class = \"Entity\"; }");
    snprintf(local_root, sizeof(local_root), "%s/overlay-local", root);
    snprintf(map_root, sizeof(map_root), "%s/overlay-map", root);
    sh_package_sources *local_sources = sh_package_sources_scan(local_root, error, sizeof(error));
    sh_package_sources *map_sources = sh_package_sources_scan(map_root, error, sizeof(error));
    CHECK(local_sources && !local_sources->package_count && map_sources && map_sources->package_count == 1);
    sh_compiled_resource local_row, map_row;
    sh_package_compilation local = {0}, map = {0};
    label_overlay_row(&local_row, path, body, local_text);
    label_overlay_row(&map_row, path, body, map_text);
    CHECK(sh_package_owners_add(&map_row.owners, 0));
    local.sources = local_sources; local.resources = &local_row; local.resource_count = 1;
    map.sources = map_sources; map.resources = &map_row; map.resource_count = 1;
    label_overlay_mapping(&local.policy, "#str_smp_loc_local", "Local chimes", "local.chimes");
    label_overlay_mapping(&map.policy, "#str_smp_loc_map", "Map chimes", "map.chimes");
    sh_package_compilation *overlay = local_sources && map_sources ?
        sh_package_compilation_overlay(&local, &map, error, sizeof(error)) : NULL;
    if (!overlay) fprintf(stderr, "label overlay: %s\n", error);
    CHECK(overlay != NULL);
    if (overlay) {
        const sh_compiled_resource *served = sh_package_compilation_find(overlay, path);
        size_t length = 0;
        unsigned char *text = served ? sh_package_compilation_read_served(overlay, served, SIZE_MAX, &length, error, sizeof(error)) : NULL;
        CHECK(overlay->resource_count == 1 && text && strstr((const char *)text, "#str_smp_loc_map"));
        free(text);
        CHECK(label_mappings_for(overlay, "smp/dup", NULL, NULL, 0) == 2);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
        sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
        const sh_strids_palette_entry *entries = NULL; size_t count = 0;
        CHECK(sh_package_runtime_test_palette_of(overlay, &entries, &count) == 1);
        const sh_strids_palette_entry *listed = roster_find(entries, count, "smp/dup");
        CHECK(listed && !strcmp(listed->label, "#str_smp_loc_map") && !strcmp(listed->category, "#str_cat"));
        sh_package_runtime_test_editor_rows(NULL, 0);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
        sh_package_compilation_free(overlay);
    }
    sh_package_owners_free(&map_row.owners);
    sh_package_policy_free(&local.policy); sh_package_policy_free(&map.policy);
    sh_package_sources_free(local_sources); sh_package_sources_free(map_sources);
    cleanup_resources(local_root); cleanup_resources(map_root);
}
/* A map's own package installed through its prompt (the MPKG route) whose
 * placeable editor row gains a label alias. The map context composes the map's
 * packages in one historical context, where its row is aliased from the package
 * files its own source list names (test_label_alias_map_context); the
 * installation compiles the same package alone, where its row is aliased from
 * its owner. Both publish the row under one name with the same authored body,
 * and while the map stays active its own resource is served: its alias may
 * differ from the installed output's, which is display only. The installation
 * is admitted and commits, and the library serves its alias once the map is
 * left. */
static int label_install_activations, label_install_recoveries;
static int label_install_activate(void *context, int restoring, const sh_package_changes *changes,
    char *error, size_t capacity)
{
    (void)context; (void)changes; (void)error; (void)capacity;
    if (restoring) label_install_recoveries++; else label_install_activations++;
    return 1;
}
/* One commit attempt over a tampered served view: refused before the
 * installation's own transaction, naming what is not served as installed. */
static void label_install_refused(sh_package_map_plan *plan, const char *label, const char *path, const char *reason)
{
    char error[2048] = "";
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    int committed = sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error));
    int right = !committed && !inventory_commit_calls && strstr(error, path) && strstr(error, reason);
    if (!right) fprintf(stderr, "label map install, %s: committed %d: %s\n", label, committed, error);
    CHECK(right);
}
static void label_install_package(const char *prefix)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/package.json", prefix);
    create(path, "{\"id\":\"acceptance.tough\",\"name\":\"Tough\"}");
    snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/conv/tough.decl", prefix);
    create(path, "{ inherit = \"d8rows/speaker\"; edit = { health = 40; } }");
    snprintf(path, sizeof(path), "%s/assets/generated/decls/snapeditorentitydef/conv/tough_row.decl", prefix);
    create(path, "{ inherit = \"rost/parent\"; edit = { entityDef = \"conv/tough\"; } }");
}
static void test_label_alias_map_install(void)
{
    char data[MAX_PATH], map_root[MAX_PATH], error[2048] = "";
    char map_name[256] = "", name[256], selected[128], alias[128] = "";
    unsigned char *map_body = NULL; size_t map_length = 0, rows = 0, reserved = 0;
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    sh_package_map_plan *plan = NULL;
    const sh_compiled_resource *served;
    sh_package_occurrence_snapshot *document = NULL, *world = NULL;
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_key live = {1, 0x1000}, clipboard = {2, 0x2000};
    uintptr_t object = live.object;
    create("labelmap-data/overrides", NULL);
    label_install_package("labelmap-map/overrides/tough");
    snprintf(data, sizeof(data), "%s/labelmap-data", root);
    snprintf(map_root, sizeof(map_root), "%s/labelmap-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map(data, map_root, error, sizeof(error));
    if (!plan) fprintf(stderr, "label map install: prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    /* The map's own row: published privately and aliased from the package its source list names. */
    served = label_served_row(sh_package_map_plan_compilation(plan), map_name, sizeof(map_name), selected,
        sizeof(selected), &rows);
    CHECK(served && served->body && served->presented && *selected && rows == 1 && *map_name);
    {
        char supplied[512];
        CHECK(label_mappings_for(sh_package_map_plan_compilation(plan), map_name, selected, supplied, sizeof(supplied)) == 1 &&
            strstr(supplied, "acceptance.tough"));
    }
    if (!served || !served->body) goto done;
    map_body = malloc(served->body_length); map_length = served->body_length;
    if (map_body) memcpy(map_body, served->body, map_length);
    label_install_package("labelmap-data/overrides/map-label/tough");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    if (!prepared || error[0]) fprintf(stderr, "label map install: inventory: %s\n", error);
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    int activated = sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0},
        label_install_activate, NULL);
    if (!activated) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "label map install: activation: %s\n", error); }
    CHECK(activated && label_install_activations == 1 && !label_install_recoveries);
    if (!activated) goto done;
    /* While the map is active its own row is served under the same name. */
    served = label_served_row(sh_package_runtime_acquire(), name, sizeof(name), selected, sizeof(selected), &rows);
    CHECK(served && served->presented && *selected && rows == 1 && !strcmp(name, map_name));
    sh_package_runtime_release();
    /* Only that presentation may differ. A changed body on the served row, a
     * changed definition the row places, or a row the view does not serve is
     * not the installed output, and the commit refuses it before its own
     * transaction. Each is tampered in place, as the conversion case does. */
    {
        const sh_package_compilation *current = sh_package_runtime_acquire();
        sh_compiled_resource *row = (sh_compiled_resource *)label_served_row(current, name, sizeof(name), selected,
            sizeof(selected), &rows);
        sh_compiled_resource *def = (sh_compiled_resource *)historical_resource(current,
            "generated/decls/entitydef/conv/tough.decl");
        sh_package_runtime_release();
        CHECK(row && row->body && def && def->body);
        if (row && row->body && def && def->body) {
            static const char changed_row[] = "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; } }";
            unsigned char *body = row->body; size_t length = row->body_length;
            row->body = (unsigned char *)changed_row; row->body_length = sizeof(changed_row) - 1;
            label_install_refused(plan, "changed row body", row->engine_path, "served with different content");
            row->body = body; row->body_length = length;
            body = def->body; length = def->body_length;
            char *changed = malloc(length + 1), *health = NULL;
            if (changed) { memcpy(changed, body, length); changed[length] = 0; health = strstr(changed, "health = 40"); }
            CHECK(health);
            if (health) {
                health[10] = '1'; def->body = (unsigned char *)changed;
                label_install_refused(plan, "changed definition", def->engine_path, "served with different content");
                def->body = body;
            }
            free(changed);
            const char *path = row->engine_path;
            char *elsewhere = _strdup(path);
            CHECK(elsewhere);
            if (elsewhere) {
                elsewhere[strlen(elsewhere) - 1] = 'x'; row->engine_path = elsewhere;
                label_install_refused(plan, "row not served", path, "(not served)");
                row->engine_path = path;
            }
            free(elsewhere);
        }
    }
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    int committed = sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error));
    if (!committed) fprintf(stderr, "label map install: commit: %s\n", error);
    CHECK(committed && inventory_commit_calls == 1);
    /* The installed library row: the same name and body, with its alias. */
    served = label_served_row(sh_package_runtime_library_acquire(), name, sizeof(name), alias, sizeof(alias), &rows);
    CHECK(served && served->body && rows == 1 && !strcmp(name, map_name) && *alias && map_body &&
        served->body_length == map_length && !memcmp(served->body, map_body, map_length));
    sh_package_runtime_release();
    /* The editor's copy of the map's use stays on the clipboard after its live
     * occurrence is gone. Its lease retains the map's historical context, the
     * unaliased row included, when the map is left. */
    {
        const char *json = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
            "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"conv/tough\"}]}";
        size_t n = 0, captured_length = 0;
        char *rebound = sh_package_map_plan_rebind(plan, NULL, json, strlen(json), &n, error, sizeof(error));
        char *captured = rebound ? sh_package_runtime_capture_document(plan, NULL, NULL, 0, rebound, n,
            &captured_length, &document, NULL, NULL, error, sizeof(error)) : NULL;
        if (!captured) fprintf(stderr, "label map install: capture: %s\n", error);
        CHECK(captured && document);
        if (rebound) HeapFree(GetProcessHeap(), 0, rebound);
        free(captured);
        if (!document) goto done;
    }
    CHECK(sh_package_occurrence_attach_snapshot(tracker, live.namespace_id, &object, 1, document, &world,
        error, sizeof(error)));
    CHECK(sh_package_occurrence_copy(tracker, clipboard, live));
    sh_package_occurrence_erase_namespace(tracker, live.namespace_id);
    sh_package_occurrence_release(document); document = NULL;
    sh_package_occurrence_release(world); world = NULL;
    /* A retained row whose meaning differs, not only its label alias, still
     * cannot be served under the installed one. */
    {
        static const char changed_row[] = "{ inherit = \"rost/parent\"; edit = { entityDef = \"d8rows/speaker\"; } }";
        sh_compiled_resource *row = (sh_compiled_resource *)label_served_row(sh_package_map_plan_compilation(plan),
            name, sizeof(name), selected, sizeof(selected), &rows);
        CHECK(row && row->body);
        if (row && row->body) {
            unsigned char *body = row->body; size_t length = row->body_length;
            row->body = (unsigned char *)changed_row; row->body_length = sizeof(changed_row) - 1;
            int refused = !sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0},
                startup_activation, &(int){0});
            sh_package_runtime_error(error, sizeof(error));
            int right = refused && strstr(error, row->engine_path) && strstr(error, "different meaning");
            if (!right) fprintf(stderr, "label map install: changed retained row: refused %d: %s\n", refused, error);
            CHECK(right);
            row->body = body; row->body_length = length;
        }
    }
    sh_package_map_plan_free(plan); plan = NULL;
    /* Leaving the map serves the installed row, alias and all, the retained
     * use included. */
    int left = sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
        &(int){0});
    if (!left) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "label map install: leave: %s\n", error); }
    CHECK(left && !sh_package_runtime_has_map_provider());
    served = label_served_row(sh_package_runtime_acquire(), name, sizeof(name), selected, sizeof(selected), &rows);
    CHECK(served && served->presented && rows == 1 && !strcmp(name, map_name) && !strcmp(selected, alias));
    sh_package_runtime_release();
    sh_package_occurrence_erase(tracker, clipboard);
done:
    free(map_body); sh_package_map_plan_free(plan);
    sh_package_occurrence_release(world); sh_package_occurrence_release(document);
    sh_package_occurrence_tracker_free(tracker);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(data); cleanup_resources(map_root);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* Rows of a map's own context. The map's packages compile as one composed,
 * historical context whose rows name no single owning package, and its rows were
 * left unaliased, so two packages' tiles that read alike, and two Add Logic
 * selectors (which state no meta category), could not be told apart. Each such
 * row is aliased from the package files its own source list names, display only:
 * the authored bodies carry no alias, and each alias names its own package and
 * the other's not. A tile and a selector of each package are covered. */
static const sh_compiled_resource *map_context_row(const sh_package_compilation *compiled, const char *authored)
{
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        const char *path = resource->original_path ? resource->original_path : resource->engine_path;
        if (!strcmp(path, authored)) return resource;
    }
    return NULL;
}
/* The label mappings naming package and authored row with the meta text stated (raw JSON); both
 * counts the mappings naming both packages when given. */
static size_t map_context_mappings(const sh_package_compilation *compiled, const char *package, const char *authored,
    const char *meta, size_t *both)
{
    size_t found = 0;
    for (size_t i = 0; compiled && i < compiled->policy.localized_count; i++) {
        const char *value = compiled->policy.localized[i].mapping, *raw;
        sh_json_object mapping = {0}, label = {0};
        if (sh_json_parse_object(value, strlen(value), 4, &mapping) &&
            (raw = sh_json_object_get(&mapping, "label")) != NULL && sh_json_parse_object(raw, strlen(raw), 3, &label)) {
            const char *state = sh_json_object_get(&label, "meta"), *sources = sh_json_object_get(&label, "sources");
            if (both && sources && strstr(sources, "Context one") && strstr(sources, "Context two")) (*both)++;
            if (sources && state && !strcmp(state, meta) && strstr(sources, package) && strstr(sources, authored)) found++;
        }
        sh_json_object_free(&label); sh_json_object_free(&mapping);
    }
    return found;
}
/* A shipped parent that states a category and no meta category: what an Add Logic selector inherits. */
static int map_context_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    static const char selector[] = "{ edit = { entityDef = \"d8rows/speaker\"; displayNameTag = \"#str_pulse\"; "
        "categoryNameTag = \"#str_acts\"; } }";
    if (strcmp(path, "generated/decls/snapeditorentitydef/rost/selparent.decl"))
        return roster_baseline(context, path, body, length);
    *length = sizeof(selector) - 1; *body = (unsigned char *)_strdup(selector); return *body ? 1 : -1;
}
static const char *const map_context_editor_rows[] = {"rost/child", "rost/cleared", "rost/hidden", "rost/parent",
    "rost/selparent"};
/* One package of the map: its own definition, a tile row copying the shipped parent, and a selector row
 * copying the selector parent, both placing that definition (so each package's rows differ). */
static void map_context_package(const char *prefix, const char *id, const char *name, const char *tag, const char *health)
{
    char path[1024], text[512];
    snprintf(path, sizeof(path), "%s/package.json", prefix);
    snprintf(text, sizeof(text), "{\"id\":\"%s\",\"name\":\"%s\"}", id, name);
    create(path, text);
    snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/ctx/%s.decl", prefix, tag);
    snprintf(text, sizeof(text), "{ inherit = \"d8rows/speaker\"; edit = { health = %s; } }", health);
    create(path, text);
    snprintf(path, sizeof(path), "%s/assets/generated/decls/snapeditorentitydef/ctx/tile_%s.decl", prefix, tag);
    snprintf(text, sizeof(text), "{ inherit = \"rost/parent\"; edit = { entityDef = \"ctx/%s\"; } }", tag);
    create(path, text);
    snprintf(path, sizeof(path), "%s/assets/generated/decls/snapeditorentitydef/ctx/pulse_%s.decl", prefix, tag);
    snprintf(text, sizeof(text), "{ inherit = \"rost/selparent\"; edit = { entityDef = \"ctx/%s\"; } }", tag);
    create(path, text);
}
static void test_label_alias_map_context(void)
{
    static const struct { const char *package, *authored, *meta; } rows[] = {
        {"Context one", "ctx/tile_one", "\"#str_meta\""}, {"Context two", "ctx/tile_two", "\"#str_meta\""},
        {"Context one", "ctx/pulse_one", "\"\""}, {"Context two", "ctx/pulse_two", "\"\""},
    };
    char data[MAX_PATH], map_root[MAX_PATH], error[2048] = "";
    size_t reserved = 0, stray = 0;
    admission_fixture f; roster_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    sh_package_map_plan *plan = NULL;
    create("ctxmap-data/overrides", NULL);
    map_context_package("ctxmap-map/overrides/ctx-one", "local.ctx-one", "Context one", "one", "40");
    map_context_package("ctxmap-map/overrides/ctx-two", "local.ctx-two", "Context two", "two", "41");
    snprintf(data, sizeof(data), "%s/ctxmap-data", root);
    snprintf(map_root, sizeof(map_root), "%s/ctxmap-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(map_context_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(map_context_editor_rows,
        sizeof(map_context_editor_rows) / sizeof(*map_context_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(data));
    plan = sh_package_runtime_prepare_map(data, map_root, error, sizeof(error));
    if (!plan) fprintf(stderr, "map context aliases: prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    {
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(plan);
        for (size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++) {
            char path[256];
            snprintf(path, sizeof(path), "generated/decls/snapeditorentitydef/%s.decl", rows[i].authored);
            const sh_compiled_resource *row = map_context_row(compiled, path);
            size_t named = map_context_mappings(compiled, rows[i].package, rows[i].authored, rows[i].meta,
                i ? NULL : &stray);
            int right = row && row->body && !strstr((const char *)row->body, "#str_smp_loc_") && row->presented &&
                strstr((const char *)row->presented, "displayNameTag = \"#str_smp_loc_") && named == 1;
            if (!right) fprintf(stderr, "map context aliases: %s: row %s, %s, %zu mapping(s)\n", rows[i].authored,
                row ? "served" : "absent", row && row->presented ? "aliased" : "unaliased", named);
            CHECK(right);
        }
    }
    CHECK(!stray && reserved >= 4);
done:
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(data); cleanup_resources(map_root);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}
/* The label text a toybox or selector tile shows, through the production serving path.
 * test_label_alias_map_context decides the alias mappings and their owners; this decides
 * the text. One package supplies two placeable rows, each placing its own definition: the
 * first names a label key only this package's strings supply, the second a key the game
 * ships. Each compile's aliases are served by the real string injector (strids.c) under the
 * production roster (sh_package_runtime_palette) and the production native language adapter
 * over a doubled native dictionary, and the text the dictionary holds under the label key
 * the roster lists for each row is read back, as the toybox shows it. The installed library
 * binds the package-only key to the package's authored text, so its alias reads that text. A
 * map's composed (historical) context binds no key, so its alias keeps the key itself as its
 * base and the native lookup serves it: success with empty text for a key the game never
 * shipped. The shipped key keeps its stock text in both. The roster fixtures type the toybox
 * keys idStr; the game's idDeclSnapEditorEntity types them idStrId (reference/idlib_schema.json),
 * the type the library binds, so this fixture retypes them. */
int hook_is_installed(void *trampoline) { return trampoline != NULL; }
int sh_user_overrides_enabled_for_launch(void) { return 1; }
#define LBS_ROWS 4096
static unsigned char lbs_rows[LBS_ROWS][32];
static struct { void *rows; unsigned count, capacity; } lbs_dictionary = {lbs_rows, 0, LBS_ROWS};
static char *lbs_pool[LBS_ROWS * 4];
static size_t lbs_pool_count;
static uint32_t lbs_hash(const char *key)
{
    uint32_t hash = 2166136261u;
    for (; *key; key++) {
        unsigned char c = (unsigned char)*key;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}
static void lbs_intern(void *out, const char *value)
{
    CHECK(lbs_pool_count < LBS_ROWS * 4);
    lbs_pool[lbs_pool_count] = _strdup(value); CHECK(lbs_pool[lbs_pool_count]);
    memcpy(out, lbs_pool + lbs_pool_count++, 8);
}
static int lbs_insert(void *table, void *row)
{
    CHECK(table == &lbs_dictionary && lbs_dictionary.count < LBS_ROWS);
    memcpy(lbs_rows[lbs_dictionary.count++], row, 32); return (int)lbs_dictionary.count - 1;
}
static int lbs_row_order(const void *a, const void *b)
{
    uint32_t left, right; memcpy(&left, a, 4); memcpy(&right, b, 4);
    return left < right ? -1 : left > right;
}
static void lbs_sort(void *context, void *rows, unsigned count, unsigned radix)
{ CHECK(context == &lbs_dictionary && radix == 0x20); qsort(rows, count, 32, lbs_row_order); }
static const char *lbs_value_of(const char *key)
{
    const char *result = NULL;
    for (unsigned i = 0; i < lbs_dictionary.count; i++) {
        const char *name = *(const char **)(lbs_rows[i] + 8);
        if (name && !strcmp(name, key)) { CHECK(!result); result = *(const char **)(lbs_rows[i] + 16); }
    }
    return result;
}
/* A key the game ships, with its text, before the injector binds the table. */
static void lbs_stock(const char *key, const char *text)
{
    unsigned at = lbs_dictionary.count++;
    uint32_t hash = lbs_hash(key);
    CHECK(at < LBS_ROWS);
    memcpy(lbs_rows[at], &hash, 4);
    lbs_intern(lbs_rows[at] + 8, key); lbs_intern(lbs_rows[at] + 16, text);
}
/* roster_setup's reflection with the game's own field types: the toybox keys of the editor
 * entry (record 4, fields 5 to 7) are idStrId, a class registered after the others. */
static void lbs_setup(admission_fixture *f)
{
    roster_setup(f);
    void *container = *(void **)f->reflection;
    const unsigned char *old_records = *(unsigned char **)((char *)container + 0x20),
        *old_objects = *(unsigned char **)((char *)f->reflection + 0x88),
        *old_pointers = *(unsigned char **)((char *)f->reflection + 0xa0);
    int terminated = *(int *)((char *)f->reflection + 0x90); /* the classes and the terminating record */
    CHECK(terminated == 16 && f->count + 3 <= 128);
    size_t classes = (size_t)terminated - 1, records_size = (classes + 2) * 0x38, objects_size = (classes + 2) * 16;
    unsigned char *block = admission_alloc(f, records_size + 2 * objects_size);
    char *name = admission_string(f, "idStrId"), *none = admission_string(f, "");
    if (!block || !name || !none) return;
    unsigned char *records = block, *objects = block + records_size, *pointers = objects + objects_size;
    memcpy(records, old_records, classes * 0x38);
    memcpy(objects, old_objects, classes * 16); memcpy(pointers, old_pointers, classes * 16);
    admission_pointer(records, classes * 0x38, name); admission_pointer(records, classes * 0x38 + 8, none);
    admission_pointer(objects, classes * 16 + 8, (void *)0x20);
    admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, (int)classes + 2);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, (int)classes + 2);
    unsigned char *fields = *(unsigned char **)(records + 4 * 0x38 + 0x20);
    for (size_t i = 5; i < 8; i++) admission_pointer(fields, i * 0x48, name);
}
static const char lbs_boss[] = "generated/decls/snapeditorentitydef/lbl/boss_tile.decl";
static const char lbs_plain[] = "generated/decls/snapeditorentitydef/lbl/plain_tile.decl";
/* One package, two placeable rows that each place their own definition (so each is published
 * privately): boss_tile names a label key only the package's strings supply, plain_tile one
 * the game ships. The same files serve the installed library and the map's context. */
static void lbs_package(const char *prefix)
{
    static const struct { const char *tag, *health, *label; } rows[] = {
        {"boss", "40", "#str_boss_label"}, {"plain", "41", "#str_stock_label"}};
    char path[1024], text[512];
    snprintf(path, sizeof(path), "%s/package.json", prefix);
    create(path, "{\"id\":\"local.boss-pack\",\"name\":\"Boss pack\","
        "\"strings\":{\"en\":{\"boss_label\":\"Boss thing\"}}}");
    for (size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++) {
        snprintf(path, sizeof(path), "%s/assets/generated/decls/entitydef/lbl/%s.decl", prefix, rows[i].tag);
        snprintf(text, sizeof(text), "{ inherit = \"d8rows/speaker\"; edit = { health = %s; } }", rows[i].health);
        create(path, text);
        snprintf(path, sizeof(path), "%s/assets/generated/decls/snapeditorentitydef/lbl/%s_tile.decl", prefix, rows[i].tag);
        snprintf(text, sizeof(text), "{ inherit = \"rost/parent\"; edit = { entityDef = \"lbl/%s\"; "
            "displayNameTag = \"%s\"; } }", rows[i].tag, rows[i].label);
        create(path, text);
    }
}
typedef struct lbs_probe {
    char name[256], key[160], mapping[1024];
    const char *text;
    int aliased, listed;
} lbs_probe;
/* The row authored at the path in one compile: its published name, the alias its presented
 * text selects, and that alias's mapping. */
static void lbs_row(const sh_package_compilation *compiled, const char *authored, lbs_probe *probe)
{
    const sh_compiled_resource *row = map_context_row(compiled, authored);
    memset(probe, 0, sizeof(*probe));
    if (!row) return;
    if (strlen(row->engine_path) > sizeof(PR_TEST_EDITOR_PREFIX) + 4) {
        size_t n = strlen(row->engine_path) - (sizeof(PR_TEST_EDITOR_PREFIX) - 1) - 5;
        if (n < sizeof(probe->name)) {
            memcpy(probe->name, row->engine_path + sizeof(PR_TEST_EDITOR_PREFIX) - 1, n); probe->name[n] = 0;
        }
    }
    const char *tag = row->presented ? strstr((const char *)row->presented, "#str_smp_loc_") : NULL;
    if (tag) {
        size_t n = strcspn(tag, "\"");
        if (n < sizeof(probe->key)) { memcpy(probe->key, tag, n); probe->key[n] = 0; }
    }
    probe->aliased = *probe->key != 0;
    for (size_t i = 0; probe->aliased && i < compiled->policy.localized_count; i++)
        if (!_stricmp(compiled->policy.localized[i].key, probe->key))
            snprintf(probe->mapping, sizeof(probe->mapping), "%s", compiled->policy.localized[i].mapping);
}
/* What the toybox shows for the row after the string refresh: the production roster must list
 * the row naming that alias, and the native dictionary serves the text under it. */
static void lbs_serve(lbs_probe *probe)
{
    const sh_strids_palette_entry *entries = NULL; size_t count = 0;
    CHECK(sh_package_runtime_palette(NULL, &entries, &count) == 1);
    const sh_strids_palette_entry *listed = *probe->name ? roster_find(entries, count, probe->name) : NULL;
    probe->listed = listed && !_stricmp(listed->label, probe->key);
    probe->text = *probe->key ? lbs_value_of(probe->key) : NULL;
}
static void lbs_check(const char *phase, const char *what, const lbs_probe *probe, const char *expected)
{
    int nonempty = probe->text && *probe->text, equal = nonempty && !strcmp(probe->text, expected);
    int empty_base = probe->text && (!*probe->text || !strncmp(probe->text, " (", 2));
    if (!probe->aliased || !probe->listed || !equal)
        fprintf(stderr, "label base, %s, %s: row %s %s, the roster %s it; label key %s serves '%s'%s, expected '%s'; "
            "mapping %s\n", phase, what, *probe->name ? probe->name : "(absent)",
            probe->aliased ? "is aliased" : "is not aliased", probe->listed ? "lists" : "does not list",
            *probe->key ? probe->key : "(none)", probe->text ? probe->text : "(unserved)",
            empty_base ? " (an empty base)" : "", expected, *probe->mapping ? probe->mapping : "(none)");
    CHECK(probe->aliased && probe->listed);
    CHECK(nonempty);
    CHECK(equal);
}
static int lbs_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{ (void)context; (void)restoring; (void)changes; (void)error; (void)capacity; return 1; }
static void test_label_base_text(void)
{
    char lib[MAX_PATH], ctx[MAX_PATH], map_root[MAX_PATH], missing[MAX_PATH], error[2048] = "";
    size_t reserved = 0;
    int activated = 0;
    lbs_probe lib_boss, lib_plain, map_boss, map_plain;
    admission_fixture f; lbs_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_string_reservations strings = {label_conversion_reserve, &reserved, 4, label_conversion_global};
    sh_package_map_plan *plan = NULL;
    const sh_package_compilation *compiled;
    unsigned char cvar[0x50] = {0}, system[0x18] = {0};
    void *cvars[] = {cvar}, *system_slot = system;
    sh_strids_palette palette = {NULL, sh_package_runtime_palette};
    memset(&lib_boss, 0, sizeof(lib_boss)); memset(&lib_plain, 0, sizeof(lib_plain));
    memset(&map_boss, 0, sizeof(map_boss)); memset(&map_plain, 0, sizeof(map_plain));
    create("lbs-lib/overrides", NULL); lbs_package("lbs-lib/overrides/boss-pack");
    create("lbs-ctx/overrides", NULL); lbs_package("lbs-map/overrides/boss-pack");
    snprintf(lib, sizeof(lib), "%s/lbs-lib", root); snprintf(ctx, sizeof(ctx), "%s/lbs-ctx", root);
    snprintf(map_root, sizeof(map_root), "%s/lbs-map", root);
    snprintf(missing, sizeof(missing), "%s/lbs-missing-user.json", root);
    /* The doubled native language: the registered sys_lang cvar, and a table that ships the
     * stock keys. The injector's own adapter then answers a lookup of a key the table lacks
     * with success and empty text, as the engine does. */
    admission_pointer(cvar, 0x40, "sys_lang"); admission_pointer(cvar, 0x10, "english");
    admission_pointer(system, 8, cvars); admission_number(system, 0x10, 1);
    CHECK(sh_strids_set_source(missing));
    lbs_stock("#str_parent", "Parent"); lbs_stock("#str_cat", "Category"); lbs_stock("#str_meta", "Meta");
    lbs_stock("#str_stock_label", "Stock label");
    sh_package_runtime_test_dispose();
    CHECK(sh_strids_test_inject(&lbs_dictionary, lbs_insert, lbs_hash, lbs_intern) > 0);
    sh_strids_test_set_sort(lbs_sort);
    CHECK(sh_strids_bind_native_localization(&system_slot));
    sh_strids_set_palette(&palette);
    /* The installed library. */
    sh_package_runtime_test_baseline(roster_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(lib));
    CHECK(sh_strids_rearm());
    compiled = sh_package_runtime_library_acquire();
    lbs_row(compiled, lbs_boss, &lib_boss); lbs_row(compiled, lbs_plain, &lib_plain);
    sh_package_runtime_release();
    lbs_serve(&lib_boss); lbs_serve(&lib_plain);
    /* The premise: the library bound the package-only key to the package's authored text. */
    CHECK(strstr(lib_boss.mapping, "Boss thing") != NULL);
    lbs_check("installed library", "package-only key", &lib_boss, "Boss thing");
    lbs_check("installed library", "stock key", &lib_plain, "Stock label");
    /* The same package files as a map's own context, over an empty library. */
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(roster_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(roster_editor_rows, sizeof(roster_editor_rows) / sizeof(*roster_editor_rows));
    sh_package_runtime_test_strings(&strings);
    CHECK(sh_package_runtime_refresh(ctx));
    plan = sh_package_runtime_prepare_map(ctx, map_root, error, sizeof(error));
    if (!plan) fprintf(stderr, "label base: prepare: %s\n", error);
    CHECK(plan); if (!plan) goto done;
    compiled = sh_package_map_plan_compilation(plan);
    lbs_row(compiled, lbs_boss, &map_boss); lbs_row(compiled, lbs_plain, &map_plain);
    activated = sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, lbs_activate, NULL);
    if (!activated) { sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "label base: activation: %s\n", error); }
    CHECK(activated && sh_package_runtime_has_map_provider()); if (!activated) goto done;
    CHECK(sh_strids_rearm());
    lbs_serve(&map_boss); lbs_serve(&map_plain);
    lbs_check("map context", "package-only key", &map_boss, "Boss thing");
    lbs_check("map context", "stock key", &map_plain, "Stock label");
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, lbs_activate, NULL));
    {
        /* Split locales: a second package of the same composed context states only French for the
         * key. The historical policy joins both locales, and the alias base carries both. */
        char split_root[MAX_PATH], rival_root[MAX_PATH];
        lbs_probe split_boss;
        sh_package_map_plan *split = NULL, *rival = NULL;
        memset(&split_boss, 0, sizeof(split_boss));
        lbs_package("lbs-split/overrides/boss-pack");
        create("lbs-split/overrides/fr-pack/package.json", "{\"id\":\"local.fr-pack\",\"name\":\"French pack\","
            "\"strings\":{\"fr\":{\"boss_label\":\"Chose boss\"}}}");
        create("lbs-split/overrides/fr-pack/assets/generated/decls/entitydef/lbl/fr_marker.decl",
            "{ inherit = \"d8rows/speaker\"; edit = { health = 42; } }");
        snprintf(split_root, sizeof(split_root), "%s/lbs-split", root);
        split = sh_package_runtime_prepare_map(ctx, split_root, error, sizeof(error));
        if (!split) fprintf(stderr, "label base: split prepare: %s\n", error);
        CHECK(split);
        if (split) {
            lbs_row(sh_package_map_plan_compilation(split), lbs_boss, &split_boss);
            if (!strstr(split_boss.mapping, "Boss thing") || !strstr(split_boss.mapping, "Chose boss"))
                fprintf(stderr, "label base, split locales: mapping %s\n", *split_boss.mapping ? split_boss.mapping : "(none)");
            CHECK(strstr(split_boss.mapping, "Boss thing") != NULL);
            CHECK(strstr(split_boss.mapping, "Chose boss") != NULL);
            CHECK(sh_package_runtime_activate_prepared_map(split, (sh_package_activation_guard){0}, lbs_activate, NULL));
            CHECK(sh_strids_rearm());
            lbs_serve(&split_boss);
            lbs_check("split locales", "package-only key", &split_boss, "Boss thing");
            CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, lbs_activate, NULL));
        }
        /* Conflicting valid suppliers, a tracked compatibility gap (B2/D7), not desired behavior:
         * a second package of the same composed context states other English text for the key.
         * Independently valid bundles should stay valid with source-bound keys (campaign README
         * 493-508), but the current single merged legacy string table refuses the composition.
         * This records the existing rollback: the transaction fails naming both packages, no
         * alias base is chosen, and no map provider replaces the last good state. */
        lbs_package("lbs-rival/overrides/boss-pack");
        create("lbs-rival/overrides/rival-pack/package.json", "{\"id\":\"local.rival-pack\",\"name\":\"Rival pack\","
            "\"strings\":{\"en\":{\"boss_label\":\"Other thing\"}}}");
        create("lbs-rival/overrides/rival-pack/assets/generated/decls/entitydef/lbl/rival_marker.decl",
            "{ inherit = \"d8rows/speaker\"; edit = { health = 43; } }");
        snprintf(rival_root, sizeof(rival_root), "%s/lbs-rival", root);
        error[0] = 0;
        rival = sh_package_runtime_prepare_map(ctx, rival_root, error, sizeof(error));
        if (rival || !strstr(error, "local.boss-pack") || !strstr(error, "local.rival-pack"))
            fprintf(stderr, "label base, conflicting suppliers: plan %s, error '%s'\n", rival ? "prepared" : "refused", error);
        CHECK(!rival);
        CHECK(strstr(error, "local.boss-pack") && strstr(error, "local.rival-pack"));
        CHECK(!sh_package_runtime_has_map_provider());
        sh_package_map_plan_free(split); sh_package_map_plan_free(rival);
        cleanup_resources(split_root); cleanup_resources(rival_root);
    }
done:
    sh_package_map_plan_free(plan);
    sh_strids_set_palette(NULL); sh_strids_bind_native_localization(NULL);
    sh_package_runtime_test_strings(NULL);
    sh_package_runtime_test_editor_rows(NULL, 0);
    sh_package_runtime_test_dispose(); cleanup_resources(lib); cleanup_resources(ctx); cleanup_resources(map_root);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    /* Release the injector's owned copies before freeing the synthetic native pool. */
    sh_strids_test_inject(&lbs_dictionary, lbs_insert, lbs_hash, lbs_intern);
    while (lbs_pool_count) free(lbs_pool[--lbs_pool_count]);
    lbs_dictionary.count = 0;
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
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
/* A refresh the installed-package watcher asked for reuses the library already
 * served while a fresh scan of the installed packages is the one it was
 * prepared from. An installation's own folder is no change: committed through
 * its map's prompt (the committed library already holds it) or rolled back (a
 * prepared folder that came and went). A changed package, or one the scan
 * cannot accept, compiles the whole library, and so does every refresh the
 * watcher did not ask for. */
static void watched_refresh(const char *data)
{
    sh_package_runtime_note_watched_change(1);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_note_watched_change(0);
}
static void test_watched_refresh(void)
{
    const char *descriptor = "{\"id\":\"watched-map\",\"name\":\"Watched map\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    const sh_package_compilation *library = NULL, *served = NULL;
    create("watched-data/overrides/installed-first/package.json", "{\"id\":\"installed-first\",\"name\":\"Installed first\"}");
    create("watched-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    authoring_bundle("watched-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/watched-data", root);
    snprintf(source, sizeof(source), "%s/watched-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    /* Nothing changed. */
    library_compiles = 0;
    watched_refresh(data);
    served = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(!library_compiles && served == library);
    /* Committed through its map's prompt. */
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("watched-data/overrides/installed", descriptor);
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    state.library = library;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &(int){0}));
    library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(library != state.library && historical_resource(library, authoring_unused));
    library_compiles = 0;
    watched_refresh(data);
    served = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(!library_compiles && served == library);
    expect_library_text(authoring_unused, "health = 40");
    /* Rolled back: the prepared folder came and went. */
    create("watched-data/overrides/rolled/package.json", "{\"id\":\"rolled\",\"name\":\"Rolled\"}");
    create("watched-data/overrides/rolled/assets/generated/decls/entitydef/rolled/thing.decl",
        "{ class = \"Entity\"; edit = { health = 3; target = \"\"; } }");
    remove_created("watched-data/overrides/rolled");
    library_compiles = 0;
    watched_refresh(data);
    served = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(!library_compiles && served == library);
    /* A changed package compiles the whole library. */
    create("watched-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 11; target = \"\"; } }");
    library_compiles = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1);
    expect_library_text("generated/decls/entitydef/first/thing.decl", "health = 11");
    /* So does a package the scan cannot accept: its diagnosis is new. */
    create("watched-data/overrides/broken/package.json", "{");
    library_compiles = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1);
    remove_created("watched-data/overrides/broken");
    /* The watcher did not ask: compiled, unchanged or not. */
    library_compiles = 0;
    CHECK(sh_package_runtime_refresh(data));
    CHECK(library_compiles >= 1);
done:
    inventory_commit_failure = 0;
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(source);
}
/* An installation that serves its base for every package it does not compile
 * describes its result only when it kept every base package: one removed
 * while the installation was prepared is still served by the committed
 * library, so the watcher's next refresh compiles and drops it. */
static void test_watched_refresh_after_removal(void)
{
    const char *descriptor = "{\"id\":\"watched-map\",\"name\":\"Watched map\"}";
    const char *first = "generated/decls/entitydef/first/thing.decl";
    char data[MAX_PATH], source[MAX_PATH], error[2048];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    create("removal-data/overrides/installed-first/package.json", "{\"id\":\"installed-first\",\"name\":\"Installed first\"}");
    create("removal-data/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    authoring_bundle("removal-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/removal-data", root);
    snprintf(source, sizeof(source), "%s/removal-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    expect_library_text(first, "health = 9");
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("removal-data/overrides/installed", descriptor);
    remove_created("removal-data/overrides/installed-first");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &(int){0}));
    expect_library_text(first, "health = 9");
    library_compiles = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1);
    expect_library_text(first, NULL);
done:
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(source);
}
/* With user packages off for this launch (sh_user_overrides 0) a native map
 * document owns no package content: its capture is the document itself, byte
 * for byte (ordinary strings included), with empty ownership, and the map's
 * policy selects nothing. A document naming package uses, or carrying
 * deliveries, keeps its provenance and is refused rather than run without its
 * packages. While serving is expected, a missing provider still refuses. */
static void test_serving_disabled_documents(void)
{
    static const unsigned char tree[1][32] = {{7}};
    const char *vanilla = "{\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[{\"info\":"
        "{\"name\":\"Ordinary \\u00e9 text\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"keep \\\"this\\\" exactly\"}]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"snapmaps/audio/2d_speaker\"}]}";
    size_t length = strlen(vanilla), captured_length = 0, delivery_count = 99, rows = 99, policy_length = 0;
    char error[1024] = "";
    sh_package_occurrence_snapshot *ownership = NULL;
    sh_mpkg_used *deliveries = NULL;
    sh_package_provenance uses = {0};
    sh_package_bundle_binding row = {0};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(baseline, NULL);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_set_serving_disabled(0);
    char *captured = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, vanilla, length, &captured_length,
        &ownership, NULL, NULL, error, sizeof(error));
    CHECK(!captured && strstr(error, "unavailable") && !ownership);
    sh_package_runtime_set_serving_disabled(1);
    captured = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, vanilla, length, &captured_length,
        &ownership, &deliveries, &delivery_count, error, sizeof(error));
    if (!captured) fprintf(stderr, "serving disabled capture: %s\n", error);
    CHECK(captured && captured_length == length && !memcmp(captured, vanilla, length) && ownership &&
        !deliveries && !delivery_count);
    const sh_package_provenance *owned = sh_package_occurrence_provenance(ownership);
    CHECK(!sh_package_occurrence_bundles(ownership, &rows) && !rows && (!owned || !owned->use_count));
    int selected = captured && sh_package_runtime_select_map_occurrences(captured, captured_length, NULL, ownership,
        error, sizeof(error));
    if (!selected) fprintf(stderr, "serving disabled policy: %s\n", error);
    CHECK(selected);
    char *policy = sh_package_runtime_active_policy("requirements", &policy_length);
    CHECK(!policy || !strstr(policy, "g_use")); free(policy);
    free(captured);
    size_t context = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED, tree, 1, "");
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", "/k:inherit"};
    CHECK(context != SIZE_MAX && sh_package_provenance_bind(&uses, "entitydef", "cross/root", "smp/root", &context, 1) &&
        sh_package_provenance_use_set(&uses, &site, 0, &context, 1));
    error[0] = 0;
    captured = sh_package_runtime_capture_document(NULL, &uses, NULL, 0, vanilla, length, &captured_length,
        &ownership, NULL, NULL, error, sizeof(error));
    CHECK(!captured && strstr(error, "disabled") && uses.use_count == 1 && uses.binding_count == 1);
    strcpy_s(row.source_name, sizeof(row.source_name), "carried");
    error[0] = 0;
    captured = sh_package_runtime_capture_document(NULL, NULL, &row, 1, vanilla, length, &captured_length,
        &ownership, NULL, NULL, error, sizeof(error));
    CHECK(!captured && strstr(error, "disabled"));
    sh_package_runtime_set_serving_disabled(0);
    sh_package_occurrence_release(ownership); sh_package_provenance_free(&uses);
    sh_package_runtime_test_dispose();
}
/* Every canonical field a whole compile serves for one row: identity, native
 * identity and aliases (the order pr_library_digest hashes), owners, sources
 * and bytes. An additive view must serve each unchanged or composed row so. */
static int canon_add(char **text, size_t *used, const char *value)
{
    size_t n = strlen(value);
    char *grown = realloc(*text, *used + n + 2);
    if (!grown) return 0;
    memcpy(grown + *used, value, n); grown[*used + n] = '|'; grown[*used + n + 1] = 0;
    *text = grown; *used += n + 1; return 1;
}
/* Header: every identity string in full, then the owning packages and source
 * files by name (attribution, not counts); then the exact bytes with their
 * length, compared length-safely. */
static char *library_row_canon(const sh_package_compilation *library, const sh_compiled_resource *r)
{
    char error[256], *text = NULL, number[32]; size_t length = 0, used = 0; int ok = 1;
    unsigned char *body = sh_package_compilation_read(library, r, SIZE_MAX, &length, error, sizeof(error));
    const char *fields[] = {r->engine_path, r->type ? r->type : "", r->name ? r->name : "",
        r->native_type ? r->native_type : "", r->native_name ? r->native_name : "", r->original_path ? r->original_path : ""};
    for (size_t i = 0; ok && i < sizeof(fields) / sizeof(*fields); i++) ok = canon_add(&text, &used, fields[i]);
    /* Each section is framed by its name and count so fields cannot merge. */
    snprintf(number, sizeof(number), "aliases=%zu", r->native_alias_count); ok = ok && canon_add(&text, &used, number);
    for (size_t a = 0; ok && a < r->native_alias_count; a++)
        ok = canon_add(&text, &used, r->native_aliases[a].type) && canon_add(&text, &used, r->native_aliases[a].name);
    snprintf(number, sizeof(number), "owners=%zu", sh_package_owners_count(&r->owners)); ok = ok && canon_add(&text, &used, number);
    for (size_t o = 0; ok && o < library->sources->package_count; o++)
        if (sh_package_owners_contains(&r->owners, o)) ok = canon_add(&text, &used, library->sources->packages[o].name);
    snprintf(number, sizeof(number), "gameplay=%zu", sh_package_owners_count(&r->gameplay_owners)); ok = ok && canon_add(&text, &used, number);
    for (size_t o = 0; ok && o < library->sources->package_count; o++)
        if (sh_package_owners_contains(&r->gameplay_owners, o)) ok = canon_add(&text, &used, library->sources->packages[o].name);
    snprintf(number, sizeof(number), "sources=%zu", r->source_count + (r->source != SIZE_MAX)); ok = ok && canon_add(&text, &used, number);
    for (size_t f = 0; ok && f < r->source_count + (r->source != SIZE_MAX); f++) {
        size_t index = f < r->source_count ? r->sources[f] : r->source;
        ok = index < library->sources->file_count &&
            canon_add(&text, &used, library->sources->packages[library->sources->files[index].owner].name) &&
            canon_add(&text, &used, library->sources->files[index].relative);
    }
    /* Bytes as their exact length and SHA256: the text stays NUL-free, so a
     * string comparison covers every initialized byte and nothing else. */
    sh_package_file_identity identity; char digest[80];
    ok = ok && body && sh_package_bytes_identity(body, length, &identity);
    if (ok) {
        int at = snprintf(digest, sizeof(digest), "bytes=%zu:", length);
        for (int b = 0; b < 32; b++) at += snprintf(digest + at, sizeof(digest) - (size_t)at, "%02x", identity.digest[b]);
        ok = canon_add(&text, &used, digest);
    }
    free(body);
    if (!ok) { free(text); return NULL; }
    return text;
}
static int library_canon_order(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char **library_canon_snapshot(size_t *count)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    char **rows = library ? calloc(library->resource_count ? library->resource_count : 1, sizeof(*rows)) : NULL;
    *count = 0;
    for (size_t i = 0; rows && i < library->resource_count; i++) {
        rows[i] = library_row_canon(library, library->resources + i); CHECK(rows[i] != NULL); (*count)++;
    }
    sh_package_runtime_release();
    if (rows && *count > 1) qsort(rows, *count, sizeof(*rows), library_canon_order);
    return rows;
}
static int library_canon_equal(char **a, size_t an, char **b, size_t bn)
{
    int equal = an == bn;
    for (size_t i = 0; equal && i < an; i++) if (strcmp(a[i], b[i])) {
        fprintf(stderr, "additive view row differs from the whole compile:\n  view:  %.300s\n  whole: %.300s\n", a[i], b[i]);
        equal = 0;
    }
    if (an != bn) fprintf(stderr, "additive view serves %zu rows, the whole compile %zu\n", an, bn);
    for (size_t i = 0; i < an; i++) free(a[i]);
    for (size_t i = 0; i < bn; i++) free(b[i]);
    free(a); free(b); return equal;
}
/* A composed multi-owner native row (the conductor every installed package's
 * spawn registrations compose into) is unchanged by an addition that does not
 * register. An installation increment serves it exactly as the whole compile
 * does: same bytes, native identity, owners and sources. */
static void test_increment_composed_rows(void)
{
    admission_fixture f; lease_registration_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    char data[MAX_PATH], path[512]; char **view = NULL, **whole = NULL; size_t view_count = 0, whole_count = 0;
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "second" : "first";
        snprintf(path, sizeof(path), "composed-data/overrides/%s/package.json", name);
        create(path, i ? "{\"id\":\"second\",\"name\":\"Second\"}" : "{\"id\":\"first\",\"name\":\"First\"}");
        snprintf(path, sizeof(path), "composed-data/overrides/%s/assets/generated/decls/entitydef/%s.decl", name, name);
        create(path, i ? "{ class = \"Entity\"; edit = { health = 30; } }" : "{ class = \"Entity\"; edit = { health = 40; } }");
        snprintf(path, sizeof(path), "composed-data/overrides/%s/assets/generated/decls/entitydef/conductor.decl", name);
        create(path, i ? "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524288; entityDef = \"second\"; } } } }" :
            "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
            "item[0] = { aiType = 524289; entityDef = \"first\"; } } } }");
    }
    snprintf(data, sizeof(data), "%s/composed-data", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_registration_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1); sh_package_runtime_test_spawn_keys(1);
    CHECK(sh_package_runtime_refresh(data));
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *conductor = library ?
        sh_package_compilation_find(library, "generated/decls/entitydef/conductor.decl") : NULL;
    CHECK(conductor && conductor->native_type && sh_package_owners_count(&conductor->owners) == 2);
    sh_package_runtime_release();
    create("composed-data/overrides/added/package.json", "{\"id\":\"added\",\"name\":\"Added\"}");
    create("composed-data/overrides/added/assets/generated/decls/entitydef/added/thing.decl",
        "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
    library_compiles = install_refreshes = 0;
    sh_package_runtime_note_install_published();
    CHECK(sh_package_runtime_refresh(data) && install_refreshes == 1 && !library_compiles);
    view = library_canon_snapshot(&view_count);
    library_compiles = install_refreshes = 0;
    CHECK(sh_package_runtime_refresh(data) && library_compiles >= 1 && !install_refreshes);
    whole = library_canon_snapshot(&whole_count);
    CHECK(view_count && library_canon_equal(view, view_count, whole, whole_count));
    sh_package_runtime_test_dispose(); sh_package_runtime_test_installed_links(0); sh_package_runtime_test_spawn_keys(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
/* A unit the added package alone binds to an original path (sl_path) is, in
 * the whole link, one more physical alias of the row another package serves at
 * that path with equal bytes (pl_project's same-path union, primary first in
 * projection order). The increment prunes its original-only row; the view must
 * compose its identity exactly. Different bytes refuse the additive view. */
static void test_increment_native_witness(void)
{
    char error[512] = "";
    sh_package_sources empty = {0};
    sh_compiled_resource supplied = {0}, original = {0}, other = {0};
    supplied.engine_path = original.engine_path = other.engine_path = "decls/renderprogs/shared.inc";
    supplied.body = original.body = (unsigned char *)"// same"; supplied.body_length = original.body_length = 7;
    supplied.source = original.source = other.source = SIZE_MAX;
    supplied.native_type = "renderprog"; supplied.native_name = "beta";
    original.native_type = "renderprog"; original.native_name = "aaa";
    original.native_original = 1; original.baseline_known = 1;
    other = original; other.body = (unsigned char *)"// changed"; other.body_length = 10;
    const sh_compiled_resource *base_inputs[1] = {&supplied}, *whole_inputs[2] = {&supplied, &original};
    sh_package_resource_projection base_row = {supplied.engine_path, base_inputs, 1, NULL, 0, "renderprog", "beta"};
    sh_package_resource_projection whole_row = {supplied.engine_path, whole_inputs, 2, NULL, 0, "renderprog", "aaa"};
    sh_package_compilation *view = sh_package_compilation_project(&empty, &base_row, 1, NULL, NULL, error, sizeof(error));
    sh_package_compilation *whole = sh_package_compilation_project(&empty, &whole_row, 1, NULL, NULL, error, sizeof(error));
    sh_package_compilation *refused = sh_package_compilation_project(&empty, &base_row, 1, NULL, NULL, error, sizeof(error));
    CHECK(view && whole && refused); if (!view || !whole || !refused) goto done;
    CHECK(sh_package_resource_native_count(whole->resources) == 2);
    /* The base row alone (the old view) lacks the added identity. */
    CHECK(sh_package_resource_native_count(view->resources) == 1);
    CHECK(sh_package_runtime_test_witness_compose(view, &original, error, sizeof(error)));
    CHECK(sh_package_resource_native_count(view->resources) == sh_package_resource_native_count(whole->resources));
    for (size_t i = 0; i < sh_package_resource_native_count(whole->resources); i++) {
        sh_package_native_identity a = sh_package_resource_native_at(view->resources, i),
            b = sh_package_resource_native_at(whole->resources, i);
        CHECK(!strcmp(a.type, b.type) && !strcmp(a.name, b.name));
    }
    CHECK(!strcmp(view->resources->native_name, "aaa") && view->resources->body_length == 7);
    CHECK(!sh_package_runtime_test_witness_compose(refused, &other, error, sizeof(error)) &&
        strstr(error, "serves differently"));
    CHECK(sh_package_resource_native_count(refused->resources) == 1);
done:
    sh_package_compilation_free(view); sh_package_compilation_free(whole); sh_package_compilation_free(refused);
}
/* A first installation whose added package binds an original that the
 * installed library serves with other bytes is no additive union the view can
 * prove (pr_native_witnesses_compose). The installation compiles the whole
 * library inside its transaction instead: the valid map prepares without an
 * error, activates and commits. Its committed library serves every row of
 * the whole compile of the same inventory with the same bytes and native
 * identities, and keeps the private alias the installation's authoring rows
 * serve to the open map (pr_installation_whole); an ordinary whole compile
 * alone does not serve it and the activation refuses. The witness targets a
 * row the installed library actually serves. */
static void test_install_witness_fallback(void)
{
    const char *installed_descriptor = "{\"id\":\"already-installed\",\"name\":\"Already installed\"}";
    const char *descriptor = "{\"id\":\"fallback-map\",\"name\":\"Fallback map\"}";
    char data[MAX_PATH], source[MAX_PATH], error[2048] = "", prepared_digest[65] = "", whole_digest[65] = "";
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    installed_authoring_activation state = {0};
    library_row *served = NULL, *prepared_rows = NULL, *whole_rows = NULL;
    size_t served_count = 0, prepared_count = 0, whole_count = 0, prepared_id_count = 0, whole_id_count = 0;
    library_row *prepared_ids = NULL, *whole_ids = NULL;
    sh_compiled_resource original = {0};
    create("install-fallback/overrides/installed-first/package.json", installed_descriptor);
    create("install-fallback/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    authoring_bundle("install-fallback-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/install-fallback", root);
    snprintf(source, sizeof(source), "%s/install-fallback-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    served = library_snapshot(&served_count);
    CHECK(served_count); if (!served_count) goto done;
    original.engine_path = served[0].path;
    original.body = (unsigned char *)"{ other bytes }"; original.body_length = strlen("{ other bytes }");
    original.source = SIZE_MAX; original.native_type = "entitydef"; original.native_name = "m35/witness";
    original.native_original = 1; original.baseline_known = 1;
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) goto done;
    authoring_bundle("install-fallback/overrides/installed", descriptor);
    sh_package_runtime_test_install_witness(&original);
    strcpy_s(error, sizeof(error), "stale");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    sh_package_runtime_test_install_witness(NULL);
    if (!prepared || error[0]) fprintf(stderr, "install witness fallback: %d %s\n", prepared, error);
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    state.library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, initial_authoring_activate, &state));
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 1);
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation,
        &(int){0}));
    CHECK(sh_package_runtime_test_library_digest(prepared_digest));
    prepared_rows = library_snapshot(&prepared_count); prepared_ids = library_identities(&prepared_id_count);
    CHECK(sh_package_runtime_refresh(data));
    CHECK(sh_package_runtime_test_library_digest(whole_digest));
    whole_rows = library_snapshot(&whole_count); whole_ids = library_identities(&whole_id_count);
    {
        size_t aliases = 0;
        int designed = library_rows_designed(prepared_rows, prepared_count, whole_rows, whole_count, &aliases);
        int agree = library_identities_agree(prepared_ids, prepared_id_count, whole_ids, whole_id_count);
        /* Whole rows and identities, plus the designed alias the open map serves. */
        int faithful = prepared_count && designed && !aliases && agree && prepared_count == whole_count + 1;
        if (!faithful) fprintf(stderr, "install witness fallback: designed %d aliases %zu identities %d rows %zu/%zu "
            "prepared %s whole %s\n", designed, aliases, agree, prepared_count, whole_count, prepared_digest, whole_digest);
        CHECK(faithful);
    }
done:
    sh_package_runtime_test_install_witness(NULL);
    library_rows_free(served, served_count);
    library_rows_free(prepared_rows, prepared_count); library_rows_free(whole_rows, whole_count);
    library_rows_free(prepared_ids, prepared_id_count); library_rows_free(whole_ids, whole_id_count);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(source);
}
/* The player's hands web (handsAnimWeb) is one global registry that every
 * package changing it composes into (pl_hands_group). The served library
 * already composes an installed pistol's subweb; a map then installs two
 * variants that redefine that subweb differently, as the B6 fixture does. The
 * installation's increment links the added packages alone, so its hands web
 * would replace the served owner's subweb: the additive view cannot judge that
 * composition, and the installation links the whole library instead
 * (pr_increment_view). The prepared library is the whole compile of the same
 * inventory; every weapon names the subweb carrying its own idle, and the
 * stock subweb stays. A failed native activation keeps the served web and
 * meanings; the successful one commits, and leaving the map keeps them. */
static void hands_fields(admission_fixture *f, void *record, size_t count,
    const char *const *types, const char *const *ops, const char *const *names)
{
    void *rows = admission_alloc(f, (count + 1) * 0x48); admission_pointer(record, 0x20, rows);
    for (size_t i = 0; i < count; i++) {
        admission_pointer(rows, i * 0x48, admission_string(f, types[i]));
        admission_pointer(rows, i * 0x48 + 8, admission_string(f, ops[i]));
        admission_pointer(rows, i * 0x48 + 0x10, admission_string(f, names[i])); admission_number(rows, i * 0x48 + 0x1c, 8);
    }
}
/* The reverse-roots schema (package_reverse_roots_test setup): the player's
 * entityDef names its hands web, and a weapon names hands subwebs by string. */
static void hands_registry_setup(admission_fixture *f)
{
    static const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo",
        "idDeclSnapEditorEntity", "idImage", "idRenderModel", "idStaticModel", "idStr", "idDeclTypeInfoGraph",
        "idDeclRenderProg", "idDeclAnimWeb", "idDeclWeapon"};
    static const char *types[] = {"entityDef", "snapEditorEntityDef", "image", "renderProg", "animWeb", "weapon"};
    static const size_t indices[] = {0, 3, 4, 9, 10, 11};
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18);
    void *managers = admission_alloc(f, 6 * 8);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 6);
    for (size_t i = 0; i < 6; i++) {
        void *manager = admission_alloc(f, 0x90); admission_pointer(managers, i * 8, manager);
        admission_pointer(manager, 8, admission_string(f, types[i]));
        admission_pointer(manager, 0x10, admission_string(f, classes[indices[i]]));
    }
    f->reflection = admission_alloc(f, 0xb0);
    void *container = admission_alloc(f, 0x30), *records = admission_alloc(f, 13 * 0x38);
    void *objects = admission_alloc(f, 13 * 16), *pointers = admission_alloc(f, 13 * 16);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 13);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 13);
    admission_pointer(pointers, 10 * 16 + 8, (void *)0x10); admission_pointer(pointers, 11 * 16 + 8, (void *)0x10);
    for (size_t i = 0; i < 12; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, i == 3 || i == 8 || i == 11 ? "idDeclTypeInfo" : ""));
    }
    admission_pointer(pointers, 8, (void *)0x10); admission_pointer(pointers, 3 * 16 + 8, (void *)0x10);
    admission_pointer(pointers, 4 * 16 + 8, (void *)0x40); admission_pointer(pointers, 5 * 16 + 8, (void *)0x50);
    admission_pointer(pointers, 6 * 16 + 8, (void *)0x60); admission_pointer(objects, 7 * 16 + 8, (void *)0x70);
    admission_pointer(objects, 8 * 16 + 8, (void *)0x30); admission_pointer(pointers, 9 * 16 + 8, (void *)0x10);
    const char *et[] = {"idRenderModel", "idImage", "idDeclEntityDef", "idSWF", "idDeclRenderProg", "idDeclAnimWeb"};
    const char *eo[] = {"*", "*", "*", "*", "*", "*"};
    const char *en[] = {"model", "image", "target", "gui", "program", "handsAnimWeb"};
    hands_fields(f, (char *)records + 0x38, 6, et, eo, en);
    const char *st[] = {"idDeclEntityDef", "idStr", "idStr"};
    const char *so[] = {"*", "", ""}, *sn[] = {"entityDef", "category", "displayName"};
    hands_fields(f, (char *)records + 3 * 0x38, 3, st, so, sn);
    const char *wt[] = {"idStr", "idStr"}, *wo[] = {"", ""}, *wn[] = {"subweb_normal", "handsHitReactionSubweb"};
    hands_fields(f, (char *)records + 11 * 0x38, 2, wt, wo, wn);
}
static int hands_install_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; const char *text = NULL; *body = NULL; *length = 0;
    if (!strcmp(path, "generated/decls/entitydef/player/mp.decl"))
        text = "{ class = \"Entity\"; edit = { handsAnimWeb = \"hands\"; } }";
    else if (!strcmp(path, "generated/decls/animweb/hands.decl"))
        text = "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { } } }";
    else if (!strcmp(path, "generated/decls/md6def/hands.md6.decl"))
        text = "{ init { } jointGroups { } events { } aliases { } props { } }";
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void hands_package(const char *folder, const char *id, const char *idle, const char *weapon)
{
    char path[MAX_PATH], body[640];
    snprintf(path, sizeof(path), "%s/package.json", folder);
    snprintf(body, sizeof(body), "{\"id\":\"%s\",\"name\":\"%s\"}", id, id); create(path, body);
    snprintf(path, sizeof(path), "%s/assets/generated/decls/animweb/hands.decl", folder);
    snprintf(body, sizeof(body), "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { } "
        "subWeb \"sp_pistol\" { node \"idle\" { props { } blendTrees { tree { modelIndex 0 blendEq \"none\" anims { "
        "alias { name \"%s\" } } } } edges { } } } } }", idle);
    create(path, body);
    snprintf(path, sizeof(path), "%s/assets/generated/decls/weapon/%s.decl", folder, weapon);
    create(path, "{ edit = { subweb_normal = \"sp_pistol\"; handsHitReactionSubweb = \"pistol\"; } }");
}
/* The serving weapon a package owns names a hands subweb whose own block holds
 * idle, and the stock subweb stays: 1; 0 otherwise; -1 when no serving weapon
 * is that package's. */
static char *hands_text(const sh_package_compilation *serving, const sh_compiled_resource *row)
{
    char error[256]; size_t length = 0;
    unsigned char *body = sh_package_compilation_read(serving, row, SIZE_MAX, &length, error, sizeof(error));
    char *text = body ? realloc(body, length + 1) : NULL;
    if (!text) { free(body); return NULL; }
    text[length] = 0; return text;
}
static int hands_binding(const char *package, const char *idle)
{
    const sh_package_compilation *serving = sh_package_runtime_acquire();
    const sh_compiled_resource *web_row = serving ?
        sh_package_compilation_find(serving, "generated/decls/animweb/hands.decl") : NULL, *weapon = NULL;
    char name[96] = "", block[128], *named = NULL, *web = NULL;
    const char *at = NULL, *mine = NULL, *end = NULL, *found = NULL;
    for (size_t i = 0; serving && !weapon && i < serving->resource_count; i++) {
        const sh_compiled_resource *row = serving->resources + i;
        if (!row->native_type || strcmp(row->native_type, "weapon")) continue;
        for (size_t p = 0; p < serving->sources->package_count; p++)
            if (sh_package_owners_contains(&row->owners, p) &&
                !strcmp(sh_package_source_identity(serving->sources, p), package)) { weapon = row; break; }
    }
    int bound = -1;
    if (weapon) {
        bound = 0;
        if (web_row && (named = hands_text(serving, weapon)) && (web = hands_text(serving, web_row)) &&
            (at = strstr(named, "subweb_normal = \"")) && sscanf(at + 17, "%95[^\"]", name) == 1) {
            snprintf(block, sizeof(block), "subWeb \"%s\" {", name);
            if ((mine = strstr(web, block))) { end = strstr(mine + 1, "subWeb \""); found = strstr(mine, idle); }
            bound = found && (!end || found < end) && strstr(web, "subWeb \"pistol\" {") != NULL;
        }
        if (!bound) fprintf(stderr, "hands binding %s -> %s (%s): %.600s\n", package, name, idle, web ? web : "(none)");
    }
    sh_package_runtime_release();
    free(named); free(web); return bound;
}
static void hands_install_case(const char *const weapons[3], const char *omit_path, const char *omit_package)
{
    char data[MAX_PATH], source[MAX_PATH], error[2048] = "", prepared_digest[65] = "", whole_digest[65] = "";
    admission_fixture f; hands_registry_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL;
    library_row *prepared_rows = NULL, *whole_rows = NULL;
    size_t prepared_count = 0, whole_count = 0;
    int fail = 0;
    /* Each case installs into its own data root; created files outlive it. */
    const char *tag = omit_path ? "-omit" : "";
    char folder[MAX_PATH];
#define HANDS_FOLDER(format) (snprintf(folder, sizeof(folder), format, tag), folder)
    hands_package(HANDS_FOLDER("hands-install%s/overrides/sp-pistol"), "local.sp-pistol", "legacy_idle", weapons[0]);
    hands_package(HANDS_FOLDER("hands-install-map%s/overrides/sp-pistol"), "local.sp-pistol", "legacy_idle", weapons[0]);
    hands_package(HANDS_FOLDER("hands-install-map%s/overrides/a"), "local.b6-pistol-a", "a_idle", weapons[1]);
    hands_package(HANDS_FOLDER("hands-install-map%s/overrides/b"), "local.b6-pistol-b", "b_idle", weapons[2]);
    snprintf(data, sizeof(data), "%s/hands-install%s", root, tag);
    snprintf(source, sizeof(source), "%s/hands-install-map%s", root, tag);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(hands_install_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1); sh_package_runtime_test_block_compose(1);
    CHECK(sh_package_runtime_refresh(data));
    CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1);
    expect_current("generated/decls/animweb/hands.decl", "legacy_idle");
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan);
    if (!plan) { fprintf(stderr, "hands install map: %s\n", error); goto done; }
    /* Consent copies the missing packages where deliveries go. */
    hands_package(HANDS_FOLDER("hands-install%s/overrides/map-00112233445566bb/a"), "local.b6-pistol-a", "a_idle", weapons[1]);
    hands_package(HANDS_FOLDER("hands-install%s/overrides/map-00112233445566bb/b"), "local.b6-pistol-b", "b_idle", weapons[2]);
    if (omit_path) {
        /* The whole compile lacks B's declaration; rows of equal input bytes
         * stay served under other paths or owners. None of them admits it:
         * the installation refuses and the served library keeps its meanings. */
        sh_package_runtime_test_whole_omit(omit_path, omit_package);
        error[0] = 0;
        int refused = !sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
        sh_package_runtime_test_whole_omit(NULL, NULL);
        if (!refused || !strstr(error, "composes no row")) fprintf(stderr, "hands omit %s: %d %s\n", omit_path, refused, error);
        CHECK(refused && strstr(error, "composes no row"));
        CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1);
        expect_current("generated/decls/animweb/hands.decl", "legacy_idle");
        CHECK(hands_binding("local.b6-pistol-a", "a_idle") == -1 && hands_binding("local.b6-pistol-b", "b_idle") == -1);
        goto done;
    }
    strcpy_s(error, sizeof(error), "stale");
    int prepared = sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error));
    if (!prepared || error[0]) fprintf(stderr, "hands install: %d %s\n", prepared, error);
    CHECK(prepared && !error[0]); if (!prepared) goto done;
    /* A failed native activation restores the served web and its meanings. */
    fail = 1;
    CHECK(!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1);
    expect_current("generated/decls/animweb/hands.decl", "legacy_idle");
    CHECK(hands_binding("local.b6-pistol-a", "a_idle") == -1 && hands_binding("local.b6-pistol-b", "b_idle") == -1);
    {
        unsigned char *web = NULL; size_t length = 0;
        CHECK(sh_package_runtime_read("generated/decls/animweb/hands.decl", &web, &length) == 1 && web &&
            !strstr((const char *)web, "a_idle") && !strstr((const char *)web, "b_idle"));
        free(web);
    }
    fail = 0;
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1 && hands_binding("local.b6-pistol-a", "a_idle") == 1 && hands_binding("local.b6-pistol-b", "b_idle") == 1);
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 1);
    sh_package_map_plan_free(plan); plan = NULL;
    /* Leaving the map serves the committed library with every meaning. */
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1 && hands_binding("local.b6-pistol-a", "a_idle") == 1 && hands_binding("local.b6-pistol-b", "b_idle") == 1);
    CHECK(sh_package_runtime_test_library_digest(prepared_digest));
    prepared_rows = library_snapshot(&prepared_count);
    /* A fresh process compiles the same installed inventory whole. */
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(hands_install_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_package_runtime_refresh(data));
    CHECK(sh_package_runtime_test_library_digest(whole_digest));
    whole_rows = library_snapshot(&whole_count);
    {
        size_t aliases = 0;
        int designed = library_rows_designed(prepared_rows, prepared_count, whole_rows, whole_count, &aliases);
        int same = prepared_count == whole_count && !strcmp(prepared_digest, whole_digest);
        if (!same || !designed) fprintf(stderr, "hands install: rows %zu/%zu designed %d prepared %s whole %s\n",
            prepared_count, whole_count, designed, prepared_digest, whole_digest);
        CHECK(same && designed);
    }
    CHECK(hands_binding("local.sp-pistol", "legacy_idle") == 1 && hands_binding("local.b6-pistol-a", "a_idle") == 1 && hands_binding("local.b6-pistol-b", "b_idle") == 1);
done:
    library_rows_free(prepared_rows, prepared_count); library_rows_free(whole_rows, whole_count);
    sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_block_compose(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data); cleanup_resources(source);
#undef HANDS_FOLDER
}
static void test_hands_install_retains_served_owner(void)
{
    /* Distinct weapon paths with equal authored bytes, as the B6 fixture has.
     * (Owners declaring one weapon path compose a single row, and their
     * differing subweb bindings conflict at map preparation instead.) */
    static const char *const weapons[] = {"legacy", "b6a", "b6b"};
    hands_install_case(weapons, NULL, NULL);
    hands_install_case(weapons, "generated/decls/weapon/b6b.decl", "local.b6-pistol-b");
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
/* A watched refresh that only adds packages compiles them alone and serves
 * the rest from the retained library, exactly what compiling the whole library
 * serves for the same files. An addition the increment would serve
 * differently, a removal or an edit compiles the whole library. */
static void test_watched_increment(void)
{
    const char *descriptor = "{\"id\":\"watched-editor\",\"name\":\"Watched editor\"}";
    const char *first = "generated/decls/entitydef/first/thing.decl", *added = "generated/decls/entitydef/added/thing.decl";
    char data[MAX_PATH];
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    library_row *served = NULL, *whole = NULL; size_t served_count = 0, whole_count = 0, aliases = 0, units = 0;
    char digest[sizeof(published_digest)];
    create("watched-increment/overrides/installed-first/package.json", "{\"id\":\"installed-first\",\"name\":\"Installed first\"}");
    create("watched-increment/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    snprintf(data, sizeof(data), "%s/watched-increment", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh(data));
    /* Added alone: only its closure is discovered, and the result is the
     * whole library's, row for row. */
    create("watched-increment/overrides/added/package.json", "{\"id\":\"added\",\"name\":\"Added\"}");
    create("watched-increment/overrides/added/assets/generated/decls/entitydef/added/thing.decl",
        "{ class = \"Entity\"; edit = { health = 5; target = \"\"; } }");
    library_compiles = watched_increments = 0; discovered_units = 0;
    watched_refresh(data);
    units = discovered_units; strcpy_s(digest, sizeof(digest), published_digest);
    CHECK(watched_increments == 1 && !library_compiles);
    expect_library_text(added, "health = 5"); expect_library_text(first, "health = 9");
    served = library_snapshot(&served_count);
    library_compiles = 0; discovered_units = 0;
    CHECK(sh_package_runtime_refresh(data) && library_compiles >= 1);
    CHECK(units && units < discovered_units && digest[0] && !strcmp(digest, published_digest));
    whole = library_snapshot(&whole_count);
    CHECK(served_count && library_rows_designed(served, served_count, whole, whole_count, &aliases) && !aliases);
    library_rows_free(served, served_count); library_rows_free(whole, whole_count); served = whole = NULL;
    /* A same-path variant of an installed package's file serves what the whole
     * library serves for both packages. */
    create("watched-increment/overrides/variant/package.json", "{\"id\":\"variant\",\"name\":\"Variant\"}");
    create("watched-increment/overrides/variant/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 7; target = \"\"; } }");
    watched_refresh(data);
    served = library_snapshot(&served_count);
    CHECK(sh_package_runtime_refresh(data));
    whole = library_snapshot(&whole_count);
    CHECK(served_count && library_rows_designed(served, served_count, whole, whole_count, &aliases) && !aliases);
    library_rows_free(served, served_count); library_rows_free(whole, whole_count); served = whole = NULL;
    remove_created("watched-increment/overrides/variant");
    CHECK(sh_package_runtime_refresh(data));
    /* A shared editor declaration the increment would alias compiles the
     * whole library instead, and serves its result. */
    authoring_bundle("watched-increment/overrides/editor", descriptor);
    library_compiles = watched_increments = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1 && !watched_increments);
    served = library_snapshot(&served_count);
    CHECK(sh_package_runtime_refresh(data));
    whole = library_snapshot(&whole_count);
    CHECK(served_count && library_rows_designed(served, served_count, whole, whole_count, &aliases) && !aliases);
    library_rows_free(served, served_count); served = NULL;
    /* Control: served as any increment, the same addition keeps the alias. */
    remove_created("watched-increment/overrides/editor");
    CHECK(sh_package_runtime_refresh(data));
    authoring_bundle("watched-increment/overrides/editor", descriptor);
    sh_package_runtime_test_exact_increment(0);
    library_compiles = watched_increments = 0;
    watched_refresh(data);
    sh_package_runtime_test_exact_increment(1);
    CHECK(watched_increments == 1 && !library_compiles);
    strcpy_s(digest, sizeof(digest), published_digest);
    served = library_snapshot(&served_count);
    CHECK(served_count && library_rows_designed(served, served_count, whole, whole_count, &aliases) && aliases == 1);
    CHECK(sh_package_runtime_refresh(data) && strcmp(digest, published_digest));
    library_rows_free(served, served_count); library_rows_free(whole, whole_count); served = whole = NULL;
    /* A removal compiles the whole library and leaves the package out. */
    remove_created("watched-increment/overrides/added");
    library_compiles = watched_increments = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1 && !watched_increments);
    expect_library_text(added, NULL);
    /* So does an edit that arrives with an addition: both are served. */
    create("watched-increment/overrides/installed-first/assets/generated/decls/entitydef/first/thing.decl",
        "{ class = \"Entity\"; edit = { health = 12; target = \"\"; } }");
    create("watched-increment/overrides/added/package.json", "{\"id\":\"added\",\"name\":\"Added\"}");
    create("watched-increment/overrides/added/assets/generated/decls/entitydef/added/thing.decl",
        "{ class = \"Entity\"; edit = { health = 6; target = \"\"; } }");
    library_compiles = watched_increments = 0;
    watched_refresh(data);
    CHECK(library_compiles >= 1 && !watched_increments);
    expect_library_text(first, "health = 12"); expect_library_text(added, "health = 6");
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
/* One map load as the editor makes it: prepare the map and the installed
 * inventory, activate, commit the inventory, then leave the map. */
static int covered_load(const char *data, const char *source)
{
    char error[2048] = "";
    int fail = 0;
    sh_package_map_plan *plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error));
    int ok = plan && sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error)) && !error[0] &&
        sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail);
    inventory_commit_calls = 0; inventory_commit_failure = 0;
    ok = ok && sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 1;
    if (!ok) fprintf(stderr, "covered load: %s\n", error);
    sh_package_map_plan_free(plan);
    return ok && sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail);
}
/* An installed package is removed or changed while the library still serves it
 * (or removed and then installed again with the same bytes), and map loads
 * prepare and commit the installed inventory before the installed-package
 * watcher's refresh. Those loads must not make the old package
 * current for good: once the watcher's refresh runs, the library serves the
 * remaining or changed package only, while a held copy keeps the old entity until
 * it is released. A cancelled preparation changes nothing. After the release
 * and its transition, the providers and the bytes their compilations hold are
 * those of a fresh library over the same installed packages. */
static void changed_inventory_case(int changed) /* 0 removed, 1 changed, 2 removed then reinstalled */
{
    const char *descriptor = "{\"id\":\"changed-map\",\"name\":\"Changed map\"}";
    const char *alpha = "generated/decls/entitydef/changed/alpha.decl";
    const char *beta = "generated/decls/entitydef/changed/beta.decl";
    char data[MAX_PATH], source[MAX_PATH], error[2048], json[1024], name[1024] = "";
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL; sh_package_occurrence_snapshot *copy = NULL;
    const sh_package_compilation *before = NULL, *after = NULL;
    size_t providers[3] = {0}, bytes[2] = {0}; int fail = 0;
    create("changed-data/overrides/alpha/package.json", "{\"id\":\"alpha\",\"name\":\"Alpha\"}");
    create("changed-data/overrides/alpha/assets/generated/decls/entitydef/changed/alpha.decl",
        "{ class = \"Entity\"; edit = { health = 11; target = \"\"; } }");
    create("changed-data/overrides/beta/package.json", "{\"id\":\"beta\",\"name\":\"Beta\"}");
    create("changed-data/overrides/beta/assets/generated/decls/entitydef/changed/beta.decl",
        "{ class = \"Entity\"; edit = { health = 12; target = \"\"; } }");
    authoring_bundle("changed-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/changed-data", root);
    snprintf(source, sizeof(source), "%s/changed-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* A held copy of alpha's entity. */
    before = sh_package_runtime_library_acquire();
    const sh_compiled_resource *entity = historical_resource(before, alpha);
    if (entity) strcpy_s(name, sizeof(name), entity->native_name ? entity->native_name : entity->name);
    sh_package_runtime_release();
    CHECK(name[0]); if (!name[0]) goto done;
    snprintf(json, sizeof(json), "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", name);
    copy = chain_capture(NULL, json, NULL); CHECK(copy); if (!copy) goto done;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Alpha changes on disk; no refresh has run yet. */
    if (changed == 1) create("changed-data/overrides/alpha/assets/generated/decls/entitydef/changed/alpha.decl",
        "{ class = \"Entity\"; edit = { health = 21; target = \"\"; } }");
    else remove_created("changed-data/overrides/alpha");
    before = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error)) && !error[0]);
    sh_package_map_plan_free(plan); plan = NULL;
    after = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(after == before);
    /* Two map loads prepare and commit the changed inventory, then the
     * installed-package watcher's refresh runs. */
    size_t depth[2] = {0};
    for (int load = 0; load < 2; load++) {
        CHECK(covered_load(data, source));
        depth[load] = sh_package_runtime_test_library_depth();
        if (changed == 2 && !load) {
            create("changed-data/overrides/alpha/package.json", "{\"id\":\"alpha\",\"name\":\"Alpha\"}");
            create("changed-data/overrides/alpha/assets/generated/decls/entitydef/changed/alpha.decl",
                "{ class = \"Entity\"; edit = { health = 11; target = \"\"; } }");
        }
    }
    if (depth[1] != depth[0]) fprintf(stderr, "changed inventory (case %d): library ancestry %zu then %zu deep\n",
        changed, depth[0], depth[1]);
    CHECK(depth[1] == depth[0]);
    watched_refresh(data);
    after = sh_package_runtime_library_acquire();
    const sh_compiled_resource *served = historical_resource(after, alpha);
    int selected = historical_resource(after, beta) && (changed ? served && served->body &&
        strstr((const char *)served->body, changed == 1 ? "health = 21" : "health = 11") : !served);
    if (!selected) fprintf(stderr, "changed inventory (case %d): alpha %s after the watched refresh\n",
        changed, served ? (served->body ? (const char *)served->body : "(no body)") : "absent");
    CHECK(selected);
    sh_package_runtime_release();
    CHECK(chain_serves("entitydef", name));
    /* Releasing the copy retires the old alpha. */
    sh_package_occurrence_release(copy); copy = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(changed || !chain_serves("entitydef", name));
    providers[0] = sh_package_runtime_test_providers(); bytes[0] = sh_package_runtime_test_owned_bytes();
    CHECK(covered_load(data, source));
    providers[1] = sh_package_runtime_test_providers();
    /* A fresh library over the same installed packages. */
    sh_package_runtime_test_dispose();
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    providers[2] = sh_package_runtime_test_providers(); bytes[1] = sh_package_runtime_test_owned_bytes();
    if (providers[0] != providers[2] || providers[1] != providers[2] || bytes[0] != bytes[1])
        fprintf(stderr, "changed inventory (case %d): %zu providers and %zu owned bytes after release, %zu after "
            "another load; a fresh library has %zu providers and %zu bytes\n", changed,
            providers[0], bytes[0], providers[1], providers[2], bytes[1]);
    CHECK(providers[0] == providers[2] && providers[1] == providers[2] && bytes[0] == bytes[1]);
done:
    sh_package_occurrence_release(copy); sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(baseline, NULL);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup_resources(data);
}
static void test_changed_inventory_retention(void)
{
    changed_inventory_case(0); /* removed */
    changed_inventory_case(1); /* changed */
    changed_inventory_case(2); /* removed, then reinstalled with the same bytes */
}
/* A map whose packages are all installed still prepares and commits the
 * installed inventory at every load. While that scan is the one the library
 * was prepared from, repeated loads must leave the same providers allocated: a
 * fresh library copy per load keeps the previous library alive as its parent,
 * one whole library more each time. The inventory still commits, an own-bytes
 * exclusion stays reported, a cancelled preparation changes nothing, a held
 * copy of a library entity keeps its owner, and a real installation still
 * publishes a new library, which later covered loads keep. */
static void test_covered_inventory_providers(void)
{
    const char *descriptor = "{\"id\":\"covered-map\",\"name\":\"Covered map\"}";
    const char *thing = "generated/decls/entitydef/covered/thing.decl";
    const char *added = "generated/decls/entitydef/covered/added.decl";
    char data[MAX_PATH], source[MAX_PATH], error[2048], json[1024], name[1024] = "", *summary = NULL;
    admission_fixture f; lease_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_package_map_plan *plan = NULL; sh_package_occurrence_snapshot *copy = NULL;
    const sh_package_compilation *before = NULL, *after = NULL;
    size_t live[6] = {0}; int fail = 0;
    create("covered-data/overrides/installed/package.json", "{\"id\":\"installed\",\"name\":\"Installed\"}");
    create("covered-data/overrides/installed/assets/generated/decls/entitydef/covered/thing.decl",
        "{ class = \"Entity\"; edit = { health = 9; target = \"\"; } }");
    create("covered-data/overrides/broken/package.json", "{\"id\":\"broken-local\",\"name\":\"Broken local\"}");
    create("covered-data/overrides/broken/assets/generated/decls/entitydef/broken.decl", "{ edit = { ");
    authoring_bundle("covered-map/overrides/b", descriptor);
    snprintf(data, sizeof(data), "%s/covered-data", root);
    snprintf(source, sizeof(source), "%s/covered-map", root);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(lease_collection_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    CHECK(sh_package_runtime_refresh_activated(data, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* A copy of a library entity, held throughout. */
    before = sh_package_runtime_library_acquire();
    const sh_compiled_resource *entity = historical_resource(before, thing);
    if (entity) strcpy_s(name, sizeof(name), entity->native_name ? entity->native_name : entity->name);
    sh_package_runtime_release();
    CHECK(name[0]); if (!name[0]) goto done;
    snprintf(json, sizeof(json), "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"%s\"}]}", name);
    copy = chain_capture(NULL, json, NULL); CHECK(copy); if (!copy) goto done;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    /* Repeated covered loads over the unchanged installed scan. */
    for (int load = 0; load < 4; load++) {
        CHECK(covered_load(data, source)); live[load] = sh_package_runtime_test_providers();
    }
    if (live[1] != live[2] || live[2] != live[3])
        fprintf(stderr, "covered inventory: %zu, %zu, %zu, %zu providers after each load\n",
            live[0], live[1], live[2], live[3]);
    CHECK(live[1] == live[2] && live[2] == live[3]);
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "malformed declaration"));
    free(summary); summary = NULL;
    CHECK(chain_serves("entitydef", name));
    /* A cancelled preparation changes nothing. */
    before = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    plan = sh_package_runtime_prepare_map(data, source, error, sizeof(error)); CHECK(plan); if (!plan) goto done;
    CHECK(sh_package_map_plan_prepare_inventory(plan, data, error, sizeof(error)) && !error[0]);
    sh_package_map_plan_free(plan); plan = NULL;
    after = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    CHECK(after == before && sh_package_runtime_test_providers() == live[3]);
    /* A newly skipped package adds no package but changes the installed scan:
     * the library carries its report. */
    create("covered-data/overrides/unreadable/package.json", "{");
    CHECK(covered_load(data, source));
    summary = sh_package_runtime_summary();
    if (!summary || !strstr(summary, "unreadable")) fprintf(stderr, "covered summary: %s\n", summary ? summary : "(none)");
    CHECK(summary && strstr(summary, "unreadable") && strstr(summary, "malformed declaration"));
    free(summary); summary = NULL;
    /* A real installation publishes a new library; later covered loads keep it. */
    create("covered-data/overrides/added/package.json", "{\"id\":\"added\",\"name\":\"Added\"}");
    create("covered-data/overrides/added/assets/generated/decls/entitydef/covered/added.decl",
        "{ class = \"Entity\"; edit = { health = 4; target = \"\"; } }");
    CHECK(covered_load(data, source));
    after = sh_package_runtime_library_acquire();
    CHECK(after != before && historical_resource(after, added) && historical_resource(after, thing));
    sh_package_runtime_release();
    for (int load = 4; load < 6; load++) {
        CHECK(covered_load(data, source)); live[load] = sh_package_runtime_test_providers();
    }
    if (live[4] != live[5]) fprintf(stderr, "covered inventory after installation: %zu, %zu providers\n", live[4], live[5]);
    CHECK(live[4] == live[5]);
    before = sh_package_runtime_library_acquire();
    CHECK(before == after || historical_resource(before, added)); sh_package_runtime_release();
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Skipped local package")); free(summary); summary = NULL;
    CHECK(chain_serves("entitydef", name));
    sh_package_occurrence_release(copy); copy = NULL;
    CHECK(sh_package_runtime_activate_occurrences(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
done:
    free(summary);
    sh_package_occurrence_release(copy); sh_package_map_plan_free(plan);
    sh_package_runtime_test_installed_links(0);
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
static int together_closure_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = NULL;
    *body = NULL; *length = 0;
    /* The supplied referrer names root; only root's own discovery requests deep. */
    if (!strcmp(path, "generated/decls/entitydef/together_stock/root.decl"))
        text = "{ inherit = \"together_stock/deep\"; edit = { health = 5; } }";
    else if (!strcmp(path, "generated/decls/entitydef/together_stock/deep.decl")) {
        closure_reads++;
        text = "{ class = \"Entity\"; edit = { health = 0; target = \"\"; } }";
    }
    if (!text) return lease_collection_baseline(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
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
    /* Admission: the failed capture judges every supplied unit and stops
     * before discovering the stock closure a healthy package requests. Its
     * supplied referrer names together_stock/root, which only the root's own
     * discovery follows to together_stock/deep: the failed capture must not
     * read deep (reading root to judge the referrer is allowed), while the
     * link does. Both malformed owners are left out from that one bounded
     * pass, and the library publishes exactly what the healthy subset alone
     * publishes. */
    for (int subset = 0; subset < 2; subset++) {
        char data[MAX_PATH], path[MAX_PATH];
        const char *folders[4] = {"a-one", "m-good", "m-closure", "z-two"};
        const char *names[4] = {"one", "good", "closure", "two"};
        const char *bodies[4] = {bad[1], "{ class = \"Entity\"; edit = { health = 2; target = \"\"; } }",
            "{ edit = { entityDef = \"together_stock/root\"; inputs = { num = 1; } } }", bad[1]};
        static char batch_digest[65];
        for (int p = 0; p < 4; p++) {
            if (subset && (p == 0 || p == 3)) continue;
            char descriptor_text[128];
            snprintf(path, sizeof(path), "together-admission-%d/overrides/%s/package.json", subset, folders[p]);
            snprintf(descriptor_text, sizeof(descriptor_text), "{\"id\":\"together-%s\",\"name\":\"Together %s\"}",
                names[p], names[p]);
            create(path, descriptor_text);
            snprintf(path, sizeof(path), "together-admission-%d/overrides/%s/assets/generated/decls/%s/together/%s.decl",
                subset, folders[p], p == 2 ? "snapeditorentitydef" : "entitydef", names[p]);
            create(path, bodies[p]);
        }
        snprintf(data, sizeof(data), "%s/together-admission-%d", root, subset);
        sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(together_closure_baseline, NULL);
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        sh_package_runtime_test_installed_links(1);
        library_compiles = skipped_packages = admission_stops = closure_reads = 0;
        closure_reads_failed = (unsigned)-1; published_digest[0] = 0;
        CHECK(sh_package_runtime_refresh(data));
        int bounded = subset ? library_compiles == 1 && !admission_stops && closure_reads :
            library_compiles == 2 && admission_stops == 1 && skipped_packages == 2 &&
            closure_reads_failed == 0 && closure_reads;
        if (!bounded || !published_digest[0])
            fprintf(stderr, "admission subset %d: %u compiles, %u stops, %u skipped, closure reads %u (%u at failure)\n",
                subset, library_compiles, admission_stops, skipped_packages, closure_reads, closure_reads_failed);
        CHECK(bounded && published_digest[0]);
        expect_library_text("generated/decls/entitydef/together/good.decl", "health = 2");
        expect_library_text("generated/decls/entitydef/together/one.decl", NULL);
        expect_library_text("generated/decls/entitydef/together/two.decl", NULL);
        if (!subset) strcpy_s(batch_digest, sizeof(batch_digest), published_digest);
        else {
            if (strcmp(batch_digest, published_digest))
                fprintf(stderr, "admission digest %s, healthy subset %s\n", batch_digest, published_digest);
            CHECK(!strcmp(batch_digest, published_digest));
        }
        sh_package_runtime_test_dispose();
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
/* Two packages restating the shipped base row whole, each with one more
 * control that has no path and only a label (the shapes of the historical F1
 * live owners f1-bad-one/two; native valid: the reader places it by slot), beside
 * a peer changing the same row, are admitted together in either discovery
 * order and in a later process: one installed-library compile each time,
 * nothing left out, and the served row carries both added controls (different
 * content, so two) and the peer's change. Unserved packages being judged
 * afresh in a later process is test_unserved_rejudged. */
static void test_contribution_owners_runtime(void)
{
    admission_fixture f; d8r_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    static const char *const labels[2] = {"displayNameTag = \"#str_one\";", "descriptionTag = \"#str_two\";"};
    for (int order = 0; order < 2; order++) {
        char data[MAX_PATH], prefix[MAX_PATH], text[1024];
        const char *folders[2] = {order ? "z-one" : "a-one", order ? "a-two" : "z-two"};
        for (int p = 0; p < 2; p++) {
            snprintf(prefix, sizeof(prefix), "owners-runtime-%d/overrides/%s", order, folders[p]);
            snprintf(text, sizeof(text), "{\"id\":\"owners-%s\",\"name\":\"Owners %s\"}", folders[p] + 2, folders[p] + 2);
            d8c_file(prefix, "package.json", text);
            snprintf(text, sizeof(text), "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 3; "
                "item[0] = { path = \"displayName\"; displayNameTag = \"#str_name\"; descriptionTag = \"#str_name_desc\"; "
                "inspector = \"d8rows/name_inspector\"; } item[1] = { path = \"actionDelay\"; displayNameTag = \"#str_delay\"; "
                "descriptionTag = \"#str_delay_desc\"; } item[2] = { %s } } } } } }", labels[p]);
            d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl", text);
        }
        snprintf(prefix, sizeof(prefix), "owners-runtime-%d/overrides/m-good", order);
        d8c_file(prefix, "package.json", "{\"id\":\"owners-good\",\"name\":\"Owners good\"}");
        d8c_file(prefix, "assets/generated/decls/snapeditorentitydef/d8rows/input_base.decl",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
            "item[0] = { path = \"displayName\"; displayNameTag = \"#str_name\"; descriptionTag = \"#str_name_desc\"; "
            "inspector = \"d8rows/name_inspector\"; } item[1] = { path = \"actionDelay\"; displayNameTag = \"#str_delay\"; "
            "descriptionTag = \"#str_good\"; } } } } } }");
        snprintf(data, sizeof(data), "%s/owners-runtime-%d", root, order);
        for (int process = 0; process < 2; process++) {
            char *summary = NULL;
            sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(d8r_baseline, NULL);
            sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
            sh_package_runtime_test_installed_links(1);
            library_compiles = memo_replays = skipped_packages = left_out_packages = 0;
            CHECK(sh_package_runtime_refresh(data));
            summary = sh_package_runtime_summary();
            int together = library_compiles == 1 && !skipped_packages && !left_out_packages &&
                summary && strstr(summary, folders[0]) && strstr(summary, folders[1]) && strstr(summary, "m-good") &&
                !strstr(summary, "Skipped");
            if (!together)
                fprintf(stderr, "contribution owners order %d process %d: %u compiles, %u replays, %u skipped, %u left out; %s\n",
                    order, process, library_compiles, memo_replays, skipped_packages, left_out_packages,
                    summary ? summary : "(none)");
            CHECK(together);
            free(summary);
            expect_library_text(d8r_base, "#str_good");
            expect_library_text(d8r_base, "#str_one");
            expect_library_text(d8r_base, "#str_two");
        }
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

/* Offline stand-in for the engine's written-state comparison (plumbing
 * only). It answers g_native_status (EQUAL by default) for every definition
 * pair the runtime submits and records the route: these runtime assertions
 * cover binding, per-definition source authority, the private preparation of
 * document-only chains and ownership recomputation after each verdict; native
 * equality itself is proved only by paired controls. */
static int g_native_compares;
static sh_native_eval_status g_native_status = SH_NATIVE_EVAL_EQUAL;
static size_t g_native_layers;
static char g_native_layer[256], g_native_layer_text[512];
static sh_native_eval_status test_native_compare(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, char *detail, size_t capacity)
{
    g_native_compares++;
    g_native_layers = layer_count;
    snprintf(g_native_layer, sizeof(g_native_layer), "%s", layer_count && layers[0].name ? layers[0].name : "");
    snprintf(g_native_layer_text, sizeof(g_native_layer_text), "%s", layer_count && layers[0].text ? layers[0].text : "");
    if (!target || !candidate || !strstr(target, "inherit") || !strstr(candidate, "inherit")) return SH_NATIVE_EVAL_FAILED;
    if (g_native_status != SH_NATIVE_EVAL_EQUAL) snprintf(detail, capacity, "stand-in verdict %d", (int)g_native_status);
    return g_native_status;
}

int main(int argc, char **argv)
{
    sh_package_runtime_test_native_compare(test_native_compare);
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
    if (argc == 2 && !strcmp(argv[1], "--increment-composition")) {
        test_increment_editor_composition(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--hands-install")) {
        test_hands_install_retains_served_owner(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--increment-stock-rows")) {
        test_increment_stock_rows(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--relay-owners")) {
        test_relay_owners(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--picker-topology")) {
        test_picker_topology(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--editor-presentation")) {
        test_editor_presentation(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--editor-pathless-discovery")) {
        test_editor_pathless_discovery(); test_editor_pathless_default_identity(); cleanup(); return failures ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--relay-roster")) {
        test_relay_roster(); cleanup(); return failures ? 1 : 0;
    }
    run_conflicting_startup();
    test_duplicate_editor_controls();
    test_audio_programs();
    test_audio_program_chain();
    test_audio_selector_chain();
    test_audio_movie_chain();
    test_audio_fallback_chain();
    test_audio_fallback_closure();
    test_input_generated_attribution();
    test_input_accounting();
    test_input_shader_accounting(SH_PACKAGE_SHADER_OPENGL);
    test_input_shader_accounting(SH_PACKAGE_SHADER_VULKAN);
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
    finish_probe.enabled=1; test_native_material_provider_transaction(); CHECK(!finish_probe.active);
    finish_probe.enabled=0;
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
    test_native_inline_placement_label_alias();
    test_constructor_provider_lifetimes(0);
    test_constructor_provider_lifetimes(1);
    test_movie_file_provider_lifetime();
    test_movie_record_provider_lifetime();
    test_movie_rollback_provider_lifetime();
    test_native_movie_pruning();
    test_native_movie_decisions();
    test_constructor_binding_changes();
    test_native_membership_changes();
    test_retained_occurrence_resources();
    test_retained_bound_dependencies();
    test_retained_registration_meaning(0, 0);
    test_retained_registration_meaning(1, 0);
    test_retained_registration_meaning(0, 1);
    test_retained_registration_meaning(1, 1);
    test_prepared_map_install_key_reservations();
    finish_probe.enabled=1; test_additive_occurrence_admission(); CHECK(!finish_probe.active);
    finish_probe.enabled=0;
    test_fsm_admission();
    test_deferred_activation_finish();
    test_installed_authoring_admission();
    test_initial_installed_authoring();
    test_local_authoring_defect();
    test_published_install_refresh();
    test_watched_increment();
    test_increment_composed_rows();
    test_increment_native_witness();
    test_left_out_packages();
    test_covered_inventory_providers();
    test_changed_inventory_retention();
    test_left_out_across_processes();
    test_malformed_owners_together();
    test_contribution_owners_runtime();
    test_increment_designed_difference();
    test_install_witness_fallback();
    test_hands_install_retains_served_owner();
    test_increment_editor_composition();
    test_increment_stock_rows();
    test_relay_owners();
    test_relay_roster();
    test_palette_roster();
    test_label_alias_conversion();
    test_label_alias_selector();
    test_label_alias_exact_install();
    test_label_alias_overlay();
    test_label_alias_map_install();
    test_label_alias_map_context();
    test_label_base_text();
    test_watched_refresh();
    test_watched_refresh_after_removal();
    test_serving_disabled_documents();
    test_editor_presentation();
    test_editor_pathless_discovery();
    test_editor_pathless_default_identity();
    test_picker_topology();
    test_selector_shared_retirement();
    test_selector_unrelated_gameplay();
    test_failed_local_compilation_admission();
    cleanup(); if (failures) return 1;
    puts("package runtime map context tests passed"); return 0;
}
