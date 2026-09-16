/* Compile complete packages against verified originals, retaining all owners. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "resource_catalog.h"
#include "package_fixture.h"

static const char *inspector = "generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl";
static const char *original = "{ edit = { validEncounters = { num = 1; item[0] = \"stock\"; } } }";
static const char *conductor = "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl";
static const char *conductor_base = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } } } }";
static const char *editor = "generated/decls/snapeditorentitydef/volume/blocking.decl";
static const char *editor_base = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
    "num = 1; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } } } } } }";

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *value = NULL;
    (void)context;
    *body = NULL; *length = 0;
    if (!strcmp(path, inspector)) value = original;
    if (!strcmp(path, conductor)) value = conductor_base;
    if (!strcmp(path, editor)) value = editor_base;
    if (!strcmp(path, "cooked/model/replaced.bmodel")) value = "original model";
    if (!value) return 0;
    *length = strlen(value); *body = (unsigned char *)_strdup(value); return *body ? 1 : -1;
}

static int probe_skipped(void *context, const sh_package *package, const char *reason)
{
    (void)context;
    printf("skipped: %s: %s\n", package->name, reason);
    return 1;
}

/* Offline library probe with the runtime's local isolation: invalid packages
 * are reported and removed; any other failure fails the probe. It has no
 * product built-ins, native reflection or live engine state. */
static void probe(const char *data_root, const char *base)
{
    char error[2048];
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    sh_package_sources *sources;
    sh_package_compilation *compiled = NULL;
    sh_package_compile_environment environment = {0};
    size_t i, bytes = 0, decls = 0, invalid = SIZE_MAX;
    if (!catalog) { fprintf(stderr, "%s\n", error); CHECK(0); return; }
    printf("catalog: %zu rows\n", sh_resource_catalog_count(catalog));
    sources = sh_package_sources_scan_local(data_root, probe_skipped, NULL, error, sizeof(error));
    if (!sources) { fprintf(stderr, "%s\n", error); CHECK(0); sh_resource_catalog_close(catalog); return; }
    environment.baseline = sh_resource_catalog_read_path; environment.baseline_context = catalog;
    environment.invalid_package = &invalid;
    for (;;) {
        compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        if (compiled || invalid >= sources->package_count) break;
        printf("skipped: %s: %s\n", sources->packages[invalid].name, error);
        if (!sh_package_sources_remove(sources, invalid)) break;
    }
    if (!compiled) { fprintf(stderr, "%s\n", error); CHECK(0); goto done; }
    for (i = 0; i < compiled->resource_count; i++) {
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(compiled, &compiled->resources[i], 256u * 1024u * 1024u, &length, error, sizeof(error));
        if (!body) { fprintf(stderr, "%s: %s\n", compiled->resources[i].engine_path, error); CHECK(0); }
        bytes += length; decls += compiled->resources[i].type != NULL; free(body);
    }
    printf("compiled: %zu packages, %zu resources, %zu declarations, %zu bytes; %zu identical duplicates, %zu composed\n",
        sources->package_count, compiled->resource_count, decls, bytes, compiled->duplicate_count, compiled->composed_count);
done:
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
}

static void expect_change(const sh_package_changes *changes, size_t i,
    const char *path, sh_package_change_kind kind)
{
    CHECK(i < changes->count);
    if (i < changes->count) CHECK(!strcmp(changes->items[i].path, path) && changes->items[i].kind == kind);
}

static void test_changes(const sh_package_compilation *compiled)
{
    sh_package_changes changes = {0};
    char error[1024];
    CHECK(sh_package_compilation_changes(NULL, NULL, &changes, error, sizeof(error)) && !changes.count);
    CHECK(sh_package_compilation_changes(NULL, compiled, &changes, error, sizeof(error)));
    CHECK(changes.count == compiled->resource_count);
    for (size_t i = 0; i < changes.count; i++) {
        expect_change(&changes, i, compiled->resources[i].engine_path, SH_PACKAGE_RESOURCE_ADDED);
        if (compiled->resources[i].type) {
            CHECK(changes.items[i].type && changes.items[i].name);
            CHECK(changes.items[i].type != compiled->resources[i].type);
            CHECK(changes.items[i].name != compiled->resources[i].name);
            CHECK(!strcmp(changes.items[i].type, compiled->resources[i].type));
            CHECK(!strcmp(changes.items[i].name, compiled->resources[i].name));
        } else CHECK(!changes.items[i].type && !changes.items[i].name);
    }
    CHECK(sh_package_compilation_changes(compiled, NULL, &changes, error, sizeof(error)));
    for (size_t i = 0; i < changes.count; i++) {
        expect_change(&changes, i, compiled->resources[i].engine_path, SH_PACKAGE_RESOURCE_REMOVED);
        if (compiled->resources[i].type) {
            CHECK(changes.items[i].type && changes.items[i].name);
            CHECK(!strcmp(changes.items[i].type, compiled->resources[i].type));
            CHECK(!strcmp(changes.items[i].name, compiled->resources[i].name));
        }
    }

    /* Different bundle ownership and duplicate counts do not change the game
     * payload. No copying/removal of those authored sources is required. */
    sh_compiled_resource resources[5];
    sh_package_compilation other = *compiled;
    CHECK(compiled->resource_count == 5);
    memcpy(resources, compiled->resources, sizeof(resources)); other.resources = resources;
    other.duplicate_count += 100;
    for (size_t i = 0; i < other.resource_count; i++) {
        resources[i].owners = (sh_package_owners){0}; resources[i].source_count = 0;
    }
    CHECK(sh_package_compilation_changes(compiled, &other, &changes, error, sizeof(error)) && !changes.count);

    /* Same provider path, different effective bytes. Embedded NULs count. */
    unsigned char bytes_a[] = {'x', 0, 'a'}, bytes_b[] = {'x', 0, 'b'};
    sh_compiled_resource a[3] = {0}, b[3] = {0};
    sh_package_compilation before = {0}, after = {0};
    before.resources = a; after.resources = b; before.resource_count = after.resource_count = 3;
    a[0].engine_path = "a"; a[1].engine_path = "c"; a[2].engine_path = "d";
    b[0].engine_path = "b"; b[1].engine_path = "c"; b[2].engine_path = "d";
    for (int i = 0; i < 3; i++) {
        a[i].body = bytes_a; b[i].body = i == 1 ? bytes_b : bytes_a;
        a[i].body_length = b[i].body_length = sizeof(bytes_a);
    }
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 3);
    expect_change(&changes, 0, "a", SH_PACKAGE_RESOURCE_REMOVED);
    expect_change(&changes, 1, "b", SH_PACKAGE_RESOURCE_ADDED);
    expect_change(&changes, 2, "c", SH_PACKAGE_RESOURCE_REPLACED);
    CHECK(sh_package_compilation_changes(&after, &before, &changes, error, sizeof(error)) && changes.count == 3);
    expect_change(&changes, 0, "a", SH_PACKAGE_RESOURCE_ADDED);
    expect_change(&changes, 1, "b", SH_PACKAGE_RESOURCE_REMOVED);
    expect_change(&changes, 2, "c", SH_PACKAGE_RESOURCE_REPLACED);
    before.resource_count = after.resource_count = 1; b[0] = a[0];
    b[0].type = "material"; b[0].name = "a";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
    b[0] = a[0]; b[0].body_length--;
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);

    /* Compare a verified opaque source with the same bytes held in memory;
     * changing the representation must not cause a false reload. */
    const sh_compiled_resource *image = sh_package_compilation_find(compiled, "generated/image/shared.bimage");
    CHECK(image && !image->body);
    if (image) {
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(compiled, image, SIZE_MAX, &length, error, sizeof(error));
        CHECK(body && length);
        a[0] = b[0] = *image; before.sources = after.sources = compiled->sources;
        b[0].body = body; b[0].body_length = length;
        if (body && length) {
            CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
            CHECK(sh_package_compilation_changes(&after, &before, &changes, error, sizeof(error)) && !changes.count);
            body[length - 1] ^= 1;
            CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
            a[0].cache_path = "Z:/nonexistent-package-comparison-fixture";
            CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
            CHECK(error[0] && !changes.count && !changes.items);
        }
        free(body);
        /* Captured opaque identities survive source retirement and use full
         * 64-bit lengths without allocating or reading gigabytes. */
        sh_package_source_file af = compiled->sources->files[image->source], bf = af;
        sh_package_sources as = {0}, bs = {0};
        as.files = &af; bs.files = &bf; as.file_count = bs.file_count = 1;
        before.sources = &as; after.sources = &bs; a[0] = b[0] = *image;
        a[0].source = b[0].source = 0;
        af.absolute = bf.absolute = "Z:/nonexistent-package-comparison-fixture";
        af.length = bf.length = UINT64_C(1) << 33;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        bf.length++;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        bf.length = af.length; bf.digest[31] ^= 1;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        bs.files = NULL;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        bs.files = &bf; bf.directory = 1;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
    }
    before.sources = after.sources = NULL;
    a[0] = a[1] = (sh_compiled_resource){0}; a[0].body = a[1].body = bytes_a;
    a[0].engine_path = "b"; a[1].engine_path = "a"; before.resource_count = 2;
    CHECK(!sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)) && !changes.count);
    a[1].engine_path = "b";
    CHECK(!sh_package_compilation_changes(&before, NULL, &changes, NULL, 0) && !changes.items);
    CHECK(!sh_package_compilation_changes(NULL, NULL, NULL, NULL, 0));

    /* A map may add hundreds of resources. Result paths are owned, including
     * after the source snapshot's names and allocations have been retired. */
    sh_compiled_resource *many = calloc(257, sizeof(*many));
    CHECK(many);
    if (many) {
        before.resources = many; before.resource_count = 257;
        for (size_t i = 0; i < 257; i++) {
            char name[32]; snprintf(name, sizeof(name), "resource/%04zu", i);
            many[i].engine_path = _strdup(name); many[i].body = bytes_a; many[i].body_length = sizeof(bytes_a);
        }
        CHECK(sh_package_compilation_changes(NULL, &before, &changes, error, sizeof(error)) && changes.count == 257);
        for (size_t i = 0; i < 257; i++) free((void *)many[i].engine_path);
        free(many); expect_change(&changes, 256, "resource/0256", SH_PACKAGE_RESOURCE_ADDED);
    }
    sh_package_changes_free(&changes); sh_package_changes_free(&changes);
}

static int restoration_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    int mode = *(int *)context;
    int result = baseline(NULL, path, body, length);
    if (!strcmp(path, inspector)) {
        if (mode == -1) { free(*body); *body = NULL; *length = 0; return -1; }
        if (mode == 0 || mode == 2) return mode;
        if (mode == 3) { free(*body); *body = (unsigned char *)_strdup("{ broken"); *length = 8; }
    }
    return result;
}

static void test_restoration(const sh_package_compilation *previous)
{
    sh_package_sources empty_sources = {0};
    sh_package_compilation *current = NULL, *again = NULL, *readded = NULL;
    const sh_compiled_resource *resource;
    char error[2048];
    int mode;
    current = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
    CHECK(current);
    if (!current) return;
    /* A failure after an earlier original was staged publishes nothing. */
    for (mode = -1; mode <= 3; mode++) if (mode != 1) {
        CHECK(!sh_package_compilation_restore_missing(current, previous,
            restoration_baseline, &mode, error, sizeof(error)));
        CHECK(current->resource_count == 0);
        CHECK(strstr(error, "cannot restore verified SnapMap original"));
        CHECK(previous->resource_count == 5);
    }
    CHECK(!sh_package_compilation_restore_missing(current, previous, NULL, NULL, NULL, 0));
    CHECK(sh_package_compilation_restore_missing(current, previous, baseline, NULL, error, sizeof(error)));
    CHECK(current->resource_count == 3 && current->duplicate_count == 0 && current->composed_count == 0);
    resource = sh_package_compilation_find(current, editor);
    CHECK(resource && resource->restored_original && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners));
    CHECK(resource && !strcmp((char *)resource->body, editor_base));
    resource = sh_package_compilation_find(current, inspector);
    CHECK(resource && resource->restored_original && resource->baseline_known == 1);
    CHECK(resource && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners) && !resource->source_count);
    CHECK(resource && resource->source == SIZE_MAX && !strcmp((char *)resource->body, original));
    CHECK(!sh_package_compilation_find(current, "cooked/model/replaced.bmodel"));
    CHECK(!sh_package_compilation_find(current, "generated/image/shared.bimage"));
    CHECK(sh_package_compilation_restore_missing(current, previous, baseline, NULL, error, sizeof(error)));
    CHECK(current->resource_count == 3); /* Idempotent, no duplicate originals. */
    again = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
    CHECK(again && sh_package_compilation_restore_missing(again, current, baseline, NULL, error, sizeof(error)));
    sh_package_compilation_free(current); current = NULL;
    resource = sh_package_compilation_find(again, inspector);
    if (resource) {
        size_t length;
        unsigned char *body = sh_package_compilation_read(again, resource, 1024, &length, error, sizeof(error));
        CHECK(body && length == strlen(original) && !strcmp((char *)body, original)); free(body);
    } else CHECK(0);
    readded = sh_package_compile(previous->sources, baseline, NULL, error, sizeof(error));
    CHECK(readded && sh_package_compilation_restore_missing(readded, again, baseline, NULL, error, sizeof(error)));
    sh_package_compilation_free(again);
    resource = sh_package_compilation_find(readded, inspector);
    CHECK(resource && !resource->restored_original && resource->owners.bits == 3 && resource->composed);
    sh_package_compilation_free(readded);
    /* Neither campaign imports nor new identities can be restored as stock. */
    {
        sh_compiled_resource imported[2] = {0};
        sh_package_compilation old = {0};
        imported[0].engine_path = inspector; imported[0].type = "material";
        imported[0].baseline_known = 2;
        imported[1].engine_path = "generated/decls/material/new.decl"; imported[1].type = "material";
        old.resources = imported; old.resource_count = 2;
        current = sh_package_compile(&empty_sources, NULL, NULL, error, sizeof(error));
        CHECK(current && sh_package_compilation_restore_missing(current, &old, NULL, NULL, error, sizeof(error)));
        CHECK(current && current->resource_count == 0);
        sh_package_compilation_free(current);
    }
}

static void test_builtin_composition(const sh_package_compilation *previous)
{
    const char *built_in = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
        "num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"builtinControl\"; inspector = \"boolinspector\"; } } } } } }";
    const char *conflict = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { "
        "num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"differentInspector\"; } } } } } }";
    const char *ordered = "{ edit = { validEncounters = { num = 3; item[0] = \"stock\"; "
        "item[1] = \"hell_guard\"; item[2] = \"cyber\"; } } }";
    sh_package_builtin builtins[] = {{inspector, (const unsigned char *)ordered, strlen(ordered)},
        {editor, (const unsigned char *)built_in, strlen(built_in)}};
    sh_package_compile_environment environment = {baseline, NULL, builtins, 2};
    sh_package_sources empty = {0};
    sh_package_compilation *compiled, *restored, *again, *refused;
    const sh_compiled_resource *resource;
    unsigned char *before;
    size_t length, count;
    char error[2048];
    int failure_mode = -1;
    compiled = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(compiled);
    if (!compiled) return;
    /* The original contributions and built-ins establish ordering together.
     * No intermediate output can invent author constraints. */
    resource = sh_package_compilation_find(compiled, inspector);
    CHECK(resource && strstr((char *)resource->body, "item[1] = \"hell_guard\";"));
    CHECK(resource && strstr((char *)resource->body, "item[2] = \"cyber\";") && resource->owners.bits == 3);
    resource = sh_package_compilation_find(compiled, editor);
    CHECK(resource && strstr((char *)resource->body, "num = 4;"));
    CHECK(resource && strstr((char *)resource->body, "allowClimb"));
    CHECK(resource && strstr((char *)resource->body, "receiveDecals"));
    CHECK(resource && strstr((char *)resource->body, "builtinControl"));
    CHECK(resource && resource->owners.bits == 3 && !sh_package_owners_count(&resource->gameplay_owners) && resource->source_count == 2);
    before = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
    CHECK(before); count = compiled->composed_count;
    again = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(again && again->composed_count == count);
    if (again) {
        const sh_compiled_resource *same = sh_package_compilation_find(again, editor);
        CHECK(same && same->body_length == length && !memcmp(before, same->body, length));
    }
    sh_package_compilation_free(again);
    builtins[1].body = (const unsigned char *)conflict; builtins[1].length = strlen(conflict);
    refused = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(!refused && strstr(error, "built-in default") && strstr(error, "properties"));
    CHECK(resource->body_length == length && !memcmp(before, resource->body, length));
    sh_package_compilation_free(refused);
    builtins[1].body = (const unsigned char *)built_in; builtins[1].length = strlen(built_in);
    environment.baseline = restoration_baseline; environment.baseline_context = &failure_mode;
    refused = sh_package_compile_with(previous->sources, &environment, error, sizeof(error));
    CHECK(!refused && strstr(error, "original is unreadable"));
    sh_package_compilation_free(refused);
    environment.baseline = baseline; environment.baseline_context = NULL;
    restored = sh_package_compile_with(&empty, &environment, error, sizeof(error));
    CHECK(restored && sh_package_compilation_restore_missing(restored, compiled, baseline, NULL, error, sizeof(error)));
    resource = sh_package_compilation_find(restored, editor);
    CHECK(resource && resource->restored_original && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners) && !resource->source_count);
    CHECK(resource && strstr((char *)resource->body, "num = 2;") && strstr((char *)resource->body, "builtinControl"));
    CHECK(resource && !strstr((char *)resource->body, "allowClimb") && !strstr((char *)resource->body, "receiveDecals"));
    free(before); sh_package_compilation_free(restored); sh_package_compilation_free(compiled);
}
static void test_new_declarations(void)
{
    char directory[MAX_PATH], error[2048];
    const char *identity = "generated/decls/entitydef/new/shared.decl";
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    snprintf(directory, sizeof(directory), "%s/new-definitions", root);
    create("new-definitions/overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("new-definitions/overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("new-definitions/overrides/a/assets/generated/decls/entitydef/new/shared.decl",
        "{ inherit = \"ai/base\"; class = \"idAI2\"; edit = { stats = { health = 100; } } }");
    create("new-definitions/overrides/b/assets/generated/decls/entitydef/new/shared.decl",
        "{ inherit = \"ai/base\"; class = \"idAI2\"; edit = { stats = { speed = 5; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(compiled);
    resource = sh_package_compilation_find(compiled, identity);
    CHECK(resource && resource->composed && !resource->baseline_known &&
        resource->owners.bits == 3 && resource->gameplay_owners.bits == 3 && resource->source_count == 2);
    CHECK(resource && strstr((char *)resource->body, "health = 100;") &&
        strstr((char *)resource->body, "speed = 5;"));
    if (resource) {
        const char *body = (const char *)resource->body;
        CHECK(strstr(body, "inherit =") && strstr(body, "class =") && strstr(body, "edit ="));
        CHECK(strstr(body, "inherit =") < strstr(body, "class ="));
        CHECK(strstr(body, "class =") < strstr(body, "edit ="));
    }
    sh_package_compilation_free(compiled);
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error));
    CHECK(!compiled && strstr(error, "verified original"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    create("new-definitions/overrides/b/assets/generated/decls/entitydef/new/shared.decl",
        "{ class = \"idAI2\"; edit = { stats = { health = 200; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled && strstr(error, "edit.stats.health"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], error[2048];
    sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = NULL;
    const sh_compiled_resource *resource;
    if (argc == 3) { probe(argv[1], argv[2]); return failures ? 1 : 0; }
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pc2", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("overrides/a/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl",
        "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"cyber\"; } } }");
    create("overrides/b/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/demo.decl",
        "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"hell_guard\"; } } }");
    create("overrides/a/assets/generated/image/shared.bimage", "identical image");
    create("overrides/b/assets/generated/image/shared.bimage", "identical image");
    create("overrides/a/assets/generated/decls/snapeditorentitydef/volume/blocking.decl",
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"boolinspector\"; } } } } } }");
    create("overrides/b/assets/generated/decls/snapeditorentitydef/volume/blocking.decl",
        "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"receiveDecals\"; inspector = \"boolinspector\"; } } } } } }");
    create("overrides/a/assets/cooked/model/replaced.bmodel", "original model");
    create("overrides/b/assets/cooked/model/replaced.bmodel", "modified model");
    create("overrides/a/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"524288\"; entityDef = \"ai/cyber\"; } } } }");
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"1048576\"; entityDef = \"ai/spider\"; } } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) goto done;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (!compiled) goto done;
    CHECK(compiled->resource_count == 5); CHECK(compiled->duplicate_count == 1); CHECK(compiled->composed_count == 3);
    resource = sh_package_compilation_find(compiled, editor);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && resource->gameplay_owners.bits == 0);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    CHECK(resource && strstr((const char *)resource->body, "allowClimb"));
    CHECK(resource && strstr((const char *)resource->body, "receiveDecals"));
    resource = sh_package_compilation_find(compiled, inspector);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && resource->source_count == 2);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    resource = sh_package_compilation_find(compiled, conductor);
    CHECK(resource && resource->composed && resource->gameplay_owners.bits == 3);
    CHECK(resource && strstr((const char *)resource->body, "num = 3;"));
    CHECK(resource && strstr((const char *)resource->body, "ai/imp"));
    CHECK(resource && strstr((const char *)resource->body, "ai/cyber"));
    CHECK(resource && strstr((const char *)resource->body, "ai/spider"));
    resource = sh_package_compilation_find(compiled, "generated/image/shared.bimage");
    CHECK(resource && resource->owners.bits == 3 && resource->source_count == 2 && !resource->composed);
    resource = sh_package_compilation_find(compiled, "cooked/model/replaced.bmodel");
    CHECK(resource && resource->owners.bits == 2 && !resource->composed);
    if (resource) {
        unsigned char *body; size_t length;
        body = sh_package_compilation_read(compiled, resource, 1024, &length, error, sizeof(error));
        CHECK(body && !strcmp((const char *)body, "modified model")); free(body);
    }
    test_changes(compiled);
    test_restoration(compiled);
    test_builtin_composition(compiled);
    sh_package_compilation_free(compiled); compiled = NULL;
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error)); CHECK(!compiled); CHECK(strstr(error, "verified original"));
    sh_package_sources_free(sources); sources = NULL;
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"4\"; entityDef = \"ai/imp\"; } "
        "item[1] = { aiType = \"524288\"; entityDef = \"ai/spider\"; } } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled); CHECK(strstr(error, "aiTypeList")); CHECK(strstr(error, "[a; b]"));
    sh_package_compilation_free(compiled); compiled = NULL;
    sh_package_sources_free(sources); sources = NULL;
    create("overrides/b/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", conductor_base);
    create("overrides/a/assets/cooked/model/replaced.bmodel", "different model");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error));
    CHECK(!compiled); CHECK(strstr(error, "opaque replacements")); CHECK(strstr(error, "[a; b]"));
done:
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    test_new_declarations(); cleanup();
    if (failures) return 1;
    puts("package compiler checks passed"); return 0;
}
