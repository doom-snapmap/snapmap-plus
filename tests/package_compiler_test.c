/* Compile complete packages against verified originals, retaining all owners. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "decl_tree.h"
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

static void test_native_identity_changes(void)
{
    sh_compiled_resource a[2] = {0}, b[2] = {0};
    sh_package_compilation before = {0}, after = {0};
    sh_package_changes changes = {0};
    char error[1024];
    for (size_t i = 0; i < 2; i++) {
        a[i].engine_path = i ? "compiled/vertex.bin" : "compiled/fragment.bin";
        a[i].body = (unsigned char *)"same"; a[i].body_length = 4;
        a[i].native_type = "renderprog"; a[i].native_name = "smp/shared";
        b[i] = a[i];
    }
    before.resources = a; before.resource_count = 2;
    after.resources = b; after.resource_count = 1;
    /* One physical permutation disappears, but its native object still has a
     * provider. Neither direction should retire/reintroduce that identity. */
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && changes.identities_complete && !changes.removed_count && !changes.added_count);
    CHECK(changes.items[0].type && !strcmp(changes.items[0].type, "renderprog"));
    CHECK(changes.items[0].name && !strcmp(changes.items[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_ADDED && !changes.removed_count && !changes.added_count);
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_REMOVED);
    /* Same path and bytes with a new native binding still needs activation.
     * Both old and new identity sets must survive the rollback reversal. */
    b[0].native_name = "smp/variant";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && changes.removed_count == 1 && changes.added_count == 1);
    CHECK(!strcmp(changes.items[0].path, "compiled/vertex.bin") &&
        changes.items[0].kind == SH_PACKAGE_RESOURCE_REMOVED);
    CHECK(!strcmp(changes.removed[0].name, "smp/shared") && !strcmp(changes.added[0].name, "smp/variant"));
    sh_package_changes_reverse(&changes);
    CHECK(changes.items[0].kind == SH_PACKAGE_RESOURCE_ADDED &&
        !strcmp(changes.removed[0].name, "smp/variant") && !strcmp(changes.added[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    /* Changing bytes as well produces a physical replacement with its own
     * previous identity, independently of the complete logical identity sets. */
    b[0].body = (unsigned char *)"edit";
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 2 && changes.removed_count == 1 && changes.added_count == 1);
    CHECK(!strcmp(changes.removed[0].name, "smp/shared") && !strcmp(changes.added[0].name, "smp/variant"));
    CHECK(!strcmp(changes.items[0].name, "smp/variant") && !strcmp(changes.items[0].previous_name, "smp/shared"));
    CHECK(changes.items[0].name != b[0].native_name && changes.items[0].previous_name != a[0].native_name);
    sh_package_changes_reverse(&changes);
    CHECK(!strcmp(changes.items[0].name, "smp/shared") && !strcmp(changes.items[0].previous_name, "smp/variant"));
    CHECK(!strcmp(changes.removed[0].name, "smp/variant") && !strcmp(changes.added[0].name, "smp/shared"));
    sh_package_changes_reverse(&changes);
    CHECK(!strcmp(changes.items[0].name, "smp/variant") && !strcmp(changes.removed[0].name, "smp/shared"));
    /* Complete removal deduplicates all physical members to one retirement. */
    CHECK(sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)));
    CHECK(changes.count == 2 && changes.removed_count == 1 && !changes.added_count);
    before.resource_count = 1; a[0].native_type = a[0].native_name = NULL;
    CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(changes.count == 1 && !changes.items[0].previous_type && !changes.removed_count && changes.added_count == 1);
    sh_package_changes_reverse(&changes);
    CHECK(!changes.items[0].type && !strcmp(changes.items[0].previous_name, "smp/variant") && changes.removed_count == 1);
    /* Invalid half-bindings never publish partial file or native diffs. */
    b[0].native_type = NULL;
    CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    CHECK(!changes.items && !changes.added && !changes.removed && !changes.identities_complete);
    b[0].native_type = "renderprog"; b[0].type = "material"; b[0].name = "smp/variant";
    CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)));
    sh_package_changes_free(&changes); sh_package_changes_reverse(NULL);
}

static void test_cached_changes(void)
{
    /* Generated native files have sealed payloads, not authored source rows.
     * Exercise all representation pairs and differences beyond one IO chunk. */
    const size_t length = 131073;
    unsigned char *body = malloc(length + 1);
    char path[4096], error[1024];
    sh_package_file *files[3] = {0};
    sh_package_changes changes = {0};
    sh_compiled_resource a = {0}, b = {0};
    sh_package_compilation before = {0}, after = {0};
    CHECK(body); if (!body) return;
    memset(body, 'a', length); body[length] = 0;
    for (size_t i = 0; i < 3; i++) {
        char relative[64]; snprintf(relative, sizeof(relative), "sealed-comparison-%zu", i);
        if (i == 2) body[length - 1] = 'b';
        create(relative, (char *)body);
        snprintf(path, sizeof(path), "%s/%s", root, relative);
        files[i] = sh_package_file_pin(path, error, sizeof(error)); CHECK(files[i]);
    }
    body[length - 1] = 'a';
    a.engine_path = b.engine_path = "sound/soundbanks/pc/100.bnk";
    a.source = b.source = SIZE_MAX;
    a.cache_file = files[0]; b.cache_file = files[1];
    before.resources = &a; after.resources = &b; before.resource_count = after.resource_count = 1;
    if (files[0] && files[1] && files[2]) {
        CHECK(sh_package_compilation_changes(NULL, &before, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_changes(&before, NULL, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == 1);
        a.body = body; a.body_length = length;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_changes(&after, &before, &changes, NULL, 0) && !changes.count);
        b.cache_file = files[2];
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        a.body = NULL;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        /* Authored source metadata cannot override the generated payload. */
        sh_package_source_file source = {0}; sh_package_sources sources = {0};
        sh_package_file_identity identity;
        CHECK(sh_package_bytes_identity(body, length, &identity));
        sources.files = &source; sources.file_count = 1;
        source.length = identity.length; memcpy(source.digest, identity.digest, sizeof(source.digest));
        before.sources = after.sources = &sources; a.source = b.source = 0;
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == 1);
        b.cache_file = files[1];
        CHECK(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        a.cache_file = NULL; before.sources = NULL;
        CHECK(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
        CHECK(sh_package_compilation_probe(&before, a.engine_path, error, sizeof(error)) == -1);
    }
    sh_package_changes_free(&changes);
    for (size_t i = 0; i < 3; i++) sh_package_file_release(files[i]);
    free(body);
}

static void test_changes(const sh_package_compilation *compiled)
{
    test_cached_changes();
    test_native_identity_changes();
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

/* Unchanged SnapMap originals stay native: the published provider drops them
 * and the links naming them. Campaign, restored, renamed, composed and rebound
 * resources remain. */
static void test_prune_native(void)
{
    static const char *const paths[] = {"generated/decls/entitydef/changed.decl",
        "generated/decls/material/composed.decl", "generated/decls/material/renamed.decl",
        "generated/decls/material/restored.decl", "generated/decls/material/stock.decl",
        "generated/decls/sound/bound.decl", "generated/image/campaign.bimage", "generated/image/stock.bimage"};
    sh_package_compilation *compiled = calloc(1, sizeof(*compiled));
    CHECK(compiled); if (!compiled) return;
    compiled->resources = calloc(8, sizeof(*compiled->resources)); compiled->resource_count = 8;
    CHECK(compiled->resources); if (!compiled->resources) { free(compiled); return; }
    for (size_t i = 0; i < 8; i++) {
        sh_compiled_resource *r = compiled->resources + i;
        r->engine_path = _strdup(paths[i]); r->owns_engine_path = 1; r->source = SIZE_MAX;
        r->native_original = 1; r->baseline_known = 1;
    }
    sh_compiled_resource *changed = compiled->resources;
    changed->native_original = 0;
    changed->bound_dependencies = calloc(3, sizeof(char *)); changed->bound_dependency_count = 3;
    changed->bound_dependencies[0] = _strdup("generated/decls/material/stock.decl");
    changed->bound_dependencies[1] = _strdup("generated/image/campaign.bimage");
    changed->bound_dependencies[2] = _strdup("generated/image/stock.bimage");
    compiled->resources[1].composed = 1;
    compiled->resources[2].original_path = _strdup("generated/decls/material/stock.decl");
    compiled->resources[3].restored_original = 1;
    compiled->resources[5].sound_authored_event = 7; compiled->resources[5].sound_event = 9;
    compiled->resources[6].native_original = 0; compiled->resources[6].baseline_known = 2;
    CHECK(sh_package_compilation_prune_native(compiled, NULL, NULL) == 2);
    CHECK(compiled->resource_count == 6);
    CHECK(!sh_package_compilation_find(compiled, "generated/decls/material/stock.decl"));
    CHECK(!sh_package_compilation_find(compiled, "generated/image/stock.bimage"));
    CHECK(sh_package_compilation_find(compiled, "generated/image/campaign.bimage"));
    CHECK(sh_package_compilation_find(compiled, "generated/decls/material/restored.decl"));
    CHECK(changed->bound_dependency_count == 1 && !strcmp(changed->bound_dependencies[0], "generated/image/campaign.bimage"));
    CHECK(sh_package_compilation_prune_native(compiled, NULL, NULL) == 0 && compiled->resource_count == 6);
    sh_package_compilation_free(compiled);
}

/* One shipped file can carry several native names. It stays native only when
 * SnapMap itself serves every name from that file: a kept stock image reloads
 * every consumer at activation, a pruned campaign-only name is unreachable. */
static int prune_served(void *context, const char *path, const char *type, const char *name)
{
    (void)context;
    if (strcmp(type, "image")) return 0;
    if (!strcmp(path, "generated/image/pair.bimage")) return !strcmp(name, "pair") || !strcmp(name, "pair.tga");
    return !strcmp(path, "generated/image/split.bimage") && !strcmp(name, "split");
}
static void test_prune_native_names(void)
{
    static const char *const paths[] = {"generated/image/pair.bimage", "generated/image/split.bimage"};
    static const char *const names[][2] = {{"pair", "pair.tga"}, {"split", "split.tga"}};
    for (int served = 0; served < 2; served++) {
        sh_package_compilation *compiled = calloc(1, sizeof(*compiled));
        CHECK(compiled); if (!compiled) return;
        compiled->resources = calloc(2, sizeof(*compiled->resources)); compiled->resource_count = 2;
        CHECK(compiled->resources); if (!compiled->resources) { free(compiled); return; }
        for (size_t i = 0; i < 2; i++) {
            sh_compiled_resource *r = compiled->resources + i;
            r->engine_path = _strdup(paths[i]); r->owns_engine_path = 1; r->source = SIZE_MAX;
            r->native_original = 1; r->baseline_known = 1;
            CHECK(sh_package_resource_native_add(r, "image", names[i][0]) &&
                  sh_package_resource_native_add(r, "image", names[i][1]));
        }
        CHECK(sh_package_compilation_prune_native(compiled, served ? prune_served : NULL, NULL) == (size_t)served);
        CHECK(!sh_package_compilation_find(compiled, paths[0]) == served);
        CHECK(sh_package_compilation_find(compiled, paths[1]) != NULL);
        sh_package_compilation_free(compiled);
    }
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

static int wiring_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (strcmp(path, "generated/decls/snapeditorentitydef/test/logic.decl")) return 0;
    *body = (unsigned char *)_strdup(context); *length = strlen(context);
    return *body ? 1 : -1;
}

static void test_editor_wiring_collections(void)
{
    const char *base = "{ edit = { inputs = { num = 2; item[0] = \"action/base\"; item[1] = \"action/remove\"; }"
        " outputs = { num = 1; item[0] = \"listener/base\"; } } }";
    const char *builtin = "{ edit = { inputs = { num = 4; item[0] = \"action/base\"; item[1] = \"action/remove\";"
        " item[2] = \"action/builtin\"; item[3] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/builtin\"; } } }";
    const char *path = "generated/decls/snapeditorentitydef/test/logic.decl";
    sh_package_builtin contribution = {path, (const unsigned char *)builtin, strlen(builtin)};
    sh_package_compile_environment environment = {wiring_baseline, (void *)base, &contribution, 1};
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    char directory[MAX_PATH], error[2048];
    snprintf(directory, sizeof(directory), "%s/wiring", root);
    create("wiring/overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("wiring/overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("wiring/overrides/a/assets/generated/decls/snapeditorentitydef/test/logic.decl",
        "{ edit = { inputs = { num = 3; item[0] = \"action/base\"; item[1] = \"action/a\"; item[2] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/a\"; } } }");
    create("wiring/overrides/b/assets/generated/decls/snapeditorentitydef/test/logic.decl",
        "{ edit = { inputs = { num = 4; item[0] = \"action/base\"; item[1] = \"action/remove\";"
        " item[2] = \"action/b\"; item[3] = \"action/shared\"; }"
        " outputs = { num = 2; item[0] = \"listener/base\"; item[1] = \"listener/builtin\"; } } }");
    sources = sh_package_sources_scan(directory, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
    CHECK(compiled); if (!compiled) fprintf(stderr, "%s\n", error);
    resource = sh_package_compilation_find(compiled, path);
    CHECK(resource && resource->composed && resource->owners.bits == 3 && !resource->gameplay_owners.bits);
    if (resource) {
        const char *body = (const char *)resource->body;
        CHECK(strstr(body, "num = 5;") && strstr(body, "num = 3;"));
        CHECK(strstr(body, "action/a") && strstr(body, "action/builtin") && strstr(body, "action/b\""));
        CHECK(strstr(body, "item[4] = \"action/shared\";") && !strstr(body, "action/remove"));
        CHECK(strstr(body, "listener/a") && strstr(body, "listener/builtin"));
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

static int import_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)path;
    *body = (unsigned char *)_strdup("{ stageProgram uncooked }");
    *length = strlen((char *)*body);
    return *(int *)context;
}

static void test_native_original_provenance(void)
{
    char error[512];
    int scope;
    cleanup(); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/native/package.json", "{\"id\":\"native\",\"name\":\"Native import\"}");
    create("overrides/native/assets/generated/decls/material/imported.decl", "{ stageProgram uncooked }");
    for (scope = 1; scope <= 3; scope++) {
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
        sh_package_compilation *compiled = sh_package_compile(sources, import_baseline, &scope, error, sizeof(error));
        CHECK(sources && compiled);
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/decls/material/imported.decl");
        CHECK(resource && resource->native_original == (scope != 3));
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    }
    create("overrides/native/assets/generated/decls/material/imported.decl", "{ stageProgram authored }");
    scope = 2;
    {
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
        sh_package_compilation *compiled = sh_package_compile(sources, import_baseline, &scope, error, sizeof(error));
        CHECK(sources && compiled);
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/decls/material/imported.decl");
        CHECK(resource && !resource->native_original);
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    }
}

static void test_duplicate_editor_controls(void)
{
    const char *path = "generated/decls/snapeditorentitydef/props/example.decl";
    const char *relative = "overrides/editor/assets/generated/decls/snapeditorentitydef/props/example.decl";
    const char *text = "{ edit = { propertySheets = { num = 2; "
        "item[0] = { properties = { num = 1; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"custom\"; descriptionTag = \"custom help\"; } } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"size\"; "
        "inspector = \"vec3\"; displayNameTag = \"stock\"; descriptionTag = \"stock help\"; } "
        "item[1] = { path = \"color\"; inspector = \"vec3\"; } } } } } }";
    char error[1024];
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    cleanup(); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\"}");
    create("overrides/healthy/package.json", "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
    create("overrides/healthy/assets/generated/decls/entitydef/example.decl", "{ edit = { health = 10; } }");
    create(relative, text);
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error)); CHECK(compiled);
    if (!compiled) fprintf(stderr, "editor control dedupe: %s\n", error);
    if (compiled) {
        CHECK(compiled->sources->package_count == 2 && compiled->resource_count == 2);
        resource = sh_package_compilation_find(compiled, path); CHECK(resource && resource->body);
        if (resource && resource->body) {
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)resource->body, resource->body_length}, error, sizeof(error));
            const sh_decl_node *sheets = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "propertySheets");
            const sh_decl_node *first = sh_decl_tree_member(sh_decl_tree_member(sheets, "item[0]"), "properties");
            const sh_decl_node *second = sh_decl_tree_member(sh_decl_tree_member(sheets, "item[1]"), "properties");
            CHECK(!strcmp(sh_decl_tree_member(first, "num")->value, "1"));
            CHECK(!strcmp(sh_decl_tree_member(second, "num")->value, "1"));
            CHECK(strstr((char *)resource->body, "\"custom help\"") && !strstr((char *)resource->body, "\"stock help\""));
            CHECK(!strcmp(sh_decl_tree_member(sh_decl_tree_member(second, "item[0]"), "path")->value, "\"color\""));
            CHECK(!sh_package_owners_count(&resource->gameplay_owners));
            sh_decl_tree_free(tree);
        }
        /* Normalization is compiled output only. Intact package delivery must
         * retain the authored duplicate rows, not silently rewrite the file. */
        int found = 0;
        for (size_t i = 0; i < sources->file_count; i++) if (sources->files[i].engine_path &&
            !strcmp(sources->files[i].engine_path, path)) {
            size_t length = 0;
            unsigned char *body = sh_package_source_read(sources->files + i, SIZE_MAX, &length, error, sizeof(error));
            CHECK(body && length == strlen(text) && !memcmp(body, text, length)); free(body); found++;
        }
        CHECK(found == 1);
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}

/* Paths of one page (or scalar values of a flat list) joined by commas; the
 * page's first control label, when asked, follows a semicolon. */
static void rebased_layout(const char *text, const char *list, int page, char *out, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    const sh_decl_node *items = tree ? sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), list) : NULL;
    char slot[32];
    *out = 0;
    if (items && page >= 0) {
        snprintf(slot, sizeof(slot), "item[%d]", page);
        items = sh_decl_tree_member(sh_decl_tree_member(items, slot), "properties");
    }
    for (int k = 0; items; k++) {
        const sh_decl_node *item, *value; const char *spelling; size_t length;
        snprintf(slot, sizeof(slot), "item[%d]", k);
        if (!(item = sh_decl_tree_member(items, slot))) break;
        value = page >= 0 ? sh_decl_tree_member(item, "path") : item;
        if (*out) strcat_s(out, capacity, ",");
        if (!sh_decl_tree_literal(value, &spelling, &length)) strcat_s(out, capacity, "?");
        else strncat_s(out, capacity, spelling, length);
        const sh_decl_node *label = page >= 0 ? sh_decl_tree_member(item, "descriptionTag") : NULL;
        if (label && sh_decl_tree_literal(label, &spelling, &length)) {
            strcat_s(out, capacity, "=");
            strncat_s(out, capacity, spelling, length);
        }
    }
    sh_decl_tree_free(tree);
}
#define SHEET(paths) "{ edit = { propertySheets = { num = 1; item[0] = { properties = { " paths " } } } } }"
static char *rebase_with(const char *child, const char *authored, const char *published, int force,
    sh_package_editor_slots *slots, int *result, int *kind)
{
    sh_decl_source a = {authored, strlen(authored)}, b = {published, strlen(published)};
    sh_package_editor_failure failure; char error[512], *out = NULL; size_t length = 0;
    *result = sh_package_editor_rebase("snapeditorentitydef", (sh_decl_source){child, strlen(child)},
        &a, 1, &b, 1, force, slots, &out, &length, &failure, error, sizeof(error));
    *kind = failure.kind;
    if (*result == -1) fprintf(stderr, "rebase: %s\n", error);
    return out;
}
static char *rebase_one(const char *child, const char *authored, const char *published, int force, int *result, int *kind)
{ return rebase_with(child, authored, published, force, NULL, result, kind); }
/* The identity a rebase gave the n-th control (from 0) of the first page, or
 * "" when it has none; stripped is the layout after dropping them under space. */
static void slot_identity(const char *text, int n, char *out, size_t capacity)
{
    char layout[1024], *at = layout, *end;
    *out = 0;
    rebased_layout(text, "propertySheets", 0, layout, sizeof(layout));
    for (int k = 0; k < n && at; k++) if ((at = strchr(at, ','))) at++;
    if (!at) return;
    if ((end = strchr(at, ','))) *end = 0;
    if ((end = strchr(at, '='))) *end = 0;
    snprintf(out, capacity, "%s", at);
}
static int stripped_layout(const char *text, const char *space, char *layout, size_t capacity)
{
    char *plain = NULL; size_t length = 0;
    int result = sh_package_editor_strip_slots("snapeditorentitydef", space, (sh_decl_source){text, strlen(text)},
        &plain, &length);
    if (result == 1) rebased_layout(plain, "propertySheets", 0, layout, capacity);
    free(plain); return result;
}
/* A child's sparse list addresses its parent's controls by position. When the
 * published parent's layout differs from its author's, each patch still lands
 * on its intended control, the parent's additions survive and the child's own
 * removals, restatements and pages are kept. */
static void test_editor_rebase(void)
{
    char layout[512]; int result, kind; char *out;
    const char *stock = SHEET("num = 2; item[0] = { path = \"displayName\"; } item[1] = { path = \"actionDelay\"; }");
    const char *added = SHEET("num = 3; item[0] = { path = \"displayName\"; } item[1] = { path = \"actionDelay\"; } "
        "item[2] = { path = \"comment\"; descriptionTag = \"#str_comment\"; }");
    /* Appending by index (stock apply_buff) would overwrite the added control. */
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
        "item[2] = { path = \"modType\"; } item[3] = { path = \"modValue\"; } } } } } }", stock, added, 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "displayName,actionDelay,comment=#str_comment,modType,modValue"));
    CHECK(out && strstr(out, "inherit = \"input_base\""));
    free(out);
    /* Parents are read in order, as the native reader applies them: a shipped
     * row repeating another field (fx/ambient_embers states fxInfo twice) still
     * rebases, and a child stating no collection never reads its parents. A
     * parent stating a collection twice is read twice, in order (pe_apply). */
    {
        const char *fx = "{ edit = { fxInfo = { rate = 1; } fxInfo = { rate = 2; } ";
        char repeated_stock[512], repeated_added[512];
        snprintf(repeated_stock, sizeof(repeated_stock), "%s%s", fx, stock + strlen("{ edit = { "));
        snprintf(repeated_added, sizeof(repeated_added), "%s%s", fx, added + strlen("{ edit = { "));
        out = rebase_one("{ inherit = \"embers\"; edit = { propertySheets = { num = 1; item[0] = { properties = { num = 4; "
            "item[2] = { path = \"modType\"; } item[3] = { path = \"modValue\"; } } } } } }", repeated_stock, repeated_added,
            0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "displayName,actionDelay,comment=#str_comment,modType,modValue"));
        free(out);
        const char *twice = "{ edit = { propertySheets = { num = 1; } propertySheets = { num = 2; } } }";
        out = rebase_one("{ inherit = \"embers\"; edit = { entityDef = \"embers_def\"; } }", twice, twice, 1, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        /* The second statement extends the first: a, b, then c at slot 2. The
         * child's patch of b follows it past the published insertion of x. */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 2; item[0] = { path = \"a\"; } "
            "item[1] = { path = \"b\"; } } } } propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { path = \"c\"; } } } } } }",
            SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"x\"; } item[2] = { path = \"b\"; } "
                "item[3] = { path = \"c\"; }"), 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "a,x,b=#str_b,c"));
        free(out);
        /* A block met for the first time inside a parent's first statement is
         * read as the native reader reads it, not copied as written: its
         * repeated properties blocks both apply, the first num of each before
         * its items, an item past that count is skipped (a later resize leaves
         * its slot empty), and a repeated scalar keeps its last value. The
         * authored parent therefore lays out exactly as this published one, and
         * the child needs no rebase. */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; type = \"float\"; type = \"int\"; } item[2] = { path = \"ghost\"; } } "
            "properties = { num = 3; item[1] = { path = \"b\"; } } } } } }",
            SHEET("num = 3; item[0] = { path = \"a\"; type = \"int\"; } item[1] = { path = \"b\"; } item[2] = { }"),
            0, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        /* The repeated scalar's last value is what the child's author saw: a
         * child moving a with type "int" changes nothing of a, so a keeps the
         * published parent's own type (forced: the layouts agree). */
        out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; "
            "item[0] = { path = \"z\"; } item[1] = { path = \"a\"; type = \"int\"; } } } } } }",
            "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
            "item[0] = { path = \"a\"; type = \"float\"; type = \"int\"; } } } } } }",
            SHEET("num = 1; item[0] = { path = \"a\"; type = \"bool\"; }"), 1, &result, &kind);
        CHECK(result == 1 && out);
        {
            sh_decl_node *tree = out ? sh_decl_tree_parse((sh_decl_source){out, strlen(out)}, NULL, 0) : NULL;
            const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
                sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
            const sh_decl_node *a = NULL;
            const char *type = NULL; size_t type_length = 0;
            for (const sh_decl_node *item = properties ? properties->children : NULL; item; item = item->next) {
                const char *path; size_t length;
                if (sh_decl_tree_literal(sh_decl_tree_member(item, "path"), &path, &length) && length == 1 && *path == 'a') a = item;
            }
            CHECK(a && sh_decl_tree_literal(sh_decl_tree_member(a, "type"), &type, &type_length) &&
                type_length == 4 && !memcmp(type, "bool", 4));
            sh_decl_tree_free(tree);
        }
        free(out);
        /* Only a block's first num counts: x and y stay, as without the second. */
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 3; item[2] = \"z\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; num = 1; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"w\"; } } }", 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "inputs", -1, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,y,w,z"));
        free(out);
        /* An item past the count is skipped when read, so growing the list later
         * gives an empty slot, not that item: the authored parent differs from
         * the published one, which states q there. */
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { item[1] = \"Y\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } inputs = { item[2] = \"q\"; } "
            "inputs = { num = 3; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"q\"; } } }", 0, &result, &kind);
        CHECK(result == 1 && out); free(out);
        out = rebase_one("{ inherit = \"p\"; edit = { inputs = { item[1] = \"Y\"; } } }",
            "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } inputs = { num = 3; item[2] = \"q\"; } } }",
            "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"q\"; } } }", 0, &result, &kind);
        CHECK(result == 0 && !out); free(out);
    }
    /* A control without a path, which the native reader places by slot alone,
     * has no identity a composition can union, so a rebase for composition
     * (slots) gives it one by where it came from, under the caller's
     * namespace: a published control its published slot (equal ones paired by
     * content, then order, so an edit lands on the slot its author edited and
     * their number is kept); the shipped declaration's own such control its
     * native slot; any other own control its content, equal ones paired by
     * occurrence. Dropping them afterwards touches nothing else. */
    {
        sh_package_editor_slots slots = {"#t-0:", {NULL, 0}};
        const char *stock_sheet = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { type = \"float\"; }");
        const char *relaid = SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { path = \"a\"; } "
            "item[2] = { type = \"float\"; }");
        const char *child = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[1] = { descriptionTag = \"#str_n\"; } item[2] = { type = \"int\"; } } } } } }";
        char first[160], second[160], third[160];
        out = rebase_with(child, stock_sheet, relaid, 0, &slots, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strncmp(layout, "x,a,#t-0:p2=#str_n,#t-0:c", 25) && strlen(layout) > 3 &&
            !strcmp(layout + strlen(layout) - 2, "#0"));
        CHECK(out && stripped_layout(out, "#t-0:", layout, sizeof(layout)) == 1 && !strcmp(layout, "x,a,?=#str_n,?"));
        CHECK(out && stripped_layout(out, "#t-1:", layout, sizeof(layout)) == 0);
        free(out);
        /* The unchanged layout stays sparse; without slots nothing is given. */
        out = rebase_with(child, stock_sheet, stock_sheet, 0, &slots, &result, &kind);
        CHECK(result == 0 && !out); free(out);
        out = rebase_one(child, stock_sheet, relaid, 0, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,a,?=#str_n,?")); free(out);
        /* Repeated equal published controls keep their number, and the edit of
         * the second stays on the second. */
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[1] = { descriptionTag = \"#str_second\"; } } } } } }",
            SHEET("num = 2; item[0] = { type = \"float\"; } item[1] = { type = \"float\"; }"),
            SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { type = \"float\"; } item[2] = { type = \"float\"; }"),
            0, &slots, &result, &kind);
        CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
        CHECK(out && !strcmp(layout, "x,#t-0:p1,#t-0:p2=#str_second")); free(out);
        /* Own controls: different ones differ, an exact duplicate in another
         * child is the same, a child's repeated equal ones are counted. */
        out = rebase_with(child, stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, first, sizeof(first)); free(out);
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, second, sizeof(second)); free(out);
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { type = \"bool\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        if (out) slot_identity(out, 3, third, sizeof(third)); free(out);
        CHECK(!strncmp(first, "#t-0:c", 6) && !strcmp(first, second) && strcmp(first, third) && !strncmp(third, "#t-0:c", 6));
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
            "item[2] = { type = \"int\"; } item[3] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0, &slots,
            &result, &kind);
        if (out) { slot_identity(out, 3, second, sizeof(second)); slot_identity(out, 4, third, sizeof(third)); }
        CHECK(out && !strcmp(first, second) && !strncmp(third, first, strlen(first) - 1) &&
            !strcmp(third + strlen(third) - 2, "#1"));
        free(out);
        /* The shipped declaration's own equal controls are its native slots in
         * every copy, edited or not; a further own one has its content. */
        {
            const char *shipped = "{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
                "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; } } } } } }";
            sh_package_editor_slots shared = {"#t-0:", {shipped, strlen(shipped)}};
            const char *two = SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }");
            const char *relaid_two = SHEET("num = 3; item[0] = { path = \"x\"; } item[1] = { path = \"a\"; } "
                "item[2] = { path = \"b\"; }");
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 5; "
                "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; descriptionTag = \"#str_second\"; } "
                "item[4] = { type = \"float\"; } } } } } }", two, relaid_two, 0, &shared, &result, &kind);
            CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
            CHECK(out && !strncmp(layout, "x,a,b,#t-0:s0.2,#t-0:s0.3=#str_second,#t-0:c", 44) &&
                !strcmp(layout + strlen(layout) - 2, "#0"));
            free(out);
        }
        /* A rebase for composition reports when the child appends, in its
         * author's frame, an own control without a path; a sparse patch's item
         * addressing an inherited control is not one. */
        {
            sh_package_editor_slots probe = {"#t-0:", {NULL, 0}, 0};
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
                "item[1] = { descriptionTag = \"#str_n\"; } } } } } }", stock_sheet, stock_sheet, 0, &probe, &result, &kind);
            CHECK(result == 0 && !out && !probe.appends); free(out);
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
                "item[2] = { type = \"int\"; } } } } } }", stock_sheet, stock_sheet, 0, &probe, &result, &kind);
            CHECK(result == 0 && !out && probe.appends); free(out);
        }
        /* The namespace is the caller's: an input stating it is refused for that
         * namespace only (-2, choose another); an authored value that merely
         * looks like an identity is neither refused nor dropped. */
        out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
            "item[2] = { path = \"#t-0:p0\"; } } } } } }", stock_sheet, relaid, 0, &slots, &result, &kind);
        CHECK(result == -2 && !out); free(out);
        out = rebase_with(child, stock_sheet, SHEET("num = 3; item[0] = { path = \"#t-0:x\"; } item[1] = { path = \"a\"; } "
            "item[2] = { type = \"float\"; }"), 0, &slots, &result, &kind);
        CHECK(result == -2 && !out); free(out);
        {
            sh_package_editor_slots other = {"#t-1:", {NULL, 0}};
            out = rebase_with("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
                "item[2] = { path = \"#t-0:p0\"; } item[3] = { type = \"int\"; } } } } } }", stock_sheet, relaid, 0,
                &other, &result, &kind);
            CHECK(result == 1 && out);
            CHECK(out && stripped_layout(out, "#t-1:", layout, sizeof(layout)) == 1 && !strcmp(layout, "x,a,?,#t-0:p0,?"));
            free(out);
        }
    }
    /* Equal layouts are left to native inheritance unless forced. */
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_delay\"; } } } } } }", stock, stock, 0, &result, &kind);
    CHECK(result == 0 && !out);
    out = rebase_one("{ inherit = \"input_base\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_delay\"; } } } } } }", stock, stock, 1, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "displayName,actionDelay=#str_delay"));
    free(out);
    /* A control rebound to another inspector at its own slot still takes the
     * child's patch natively: nothing to restate. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 3; "
        "item[2] = { path = \"volume\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"name\"; inspector = \"stock\"; } item[1] = { path = \"delay\"; }"),
        SHEET("num = 2; item[0] = { path = \"name\"; inspector = \"private\"; } item[1] = { path = \"delay\"; }"),
        0, &result, &kind);
    CHECK(result == 0 && !out); free(out);
    /* Controls sharing a path are told apart by their binding. Swapped, the
     * child's patch would land on the other one natively, so it is rebased to
     * follow its control; one of them rebound in place keeps both slots. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[0] = { descriptionTag = \"#str_x\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"x\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"y\"; } item[1] = { path = \"m\"; inspector = \"x\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "m,m=#str_x"));
    free(out);
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[0] = { descriptionTag = \"#str_x\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"x\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        SHEET("num = 2; item[0] = { path = \"m\"; inspector = \"z\"; } item[1] = { path = \"m\"; inspector = \"y\"; }"),
        0, &result, &kind);
    CHECK(result == 0 && !out); free(out);
    /* A control inserted first: the patch follows speed, not its old slot. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_child\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"health\"; } item[1] = { path = \"speed\"; }"),
        SHEET("num = 3; item[0] = { path = \"armor\"; } item[1] = { path = \"health\"; } item[2] = { path = \"speed\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "armor,health,speed=#str_child"));
    free(out);
    /* Restating shifted slots (stock keycards): each restated control is the
     * published one at its published slot, the addition keeps its place and
     * the child's new control follows, so no slot inherits another's fields. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 4; "
        "item[1] = { path = \"m\"; } item[2] = { path = \"f\"; } item[3] = { path = \"c\"; } } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"f\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"f\"; descriptionTag = \"#str_f\"; } "
            "item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }"), 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,f=#str_f,c,z,m"));
    free(out);
    /* The child's truncation stays; a control the parent's authors removed is
     * kept only where the child edited it. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { num = 2; } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 4; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; } item[3] = { path = \"z\"; }"),
        0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,b,z"));
    free(out);
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_b\"; } } } } } }",
        SHEET("num = 3; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; } item[2] = { path = \"c\"; }"),
        SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"c\"; }"), 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a,c,b=#str_b"));
    free(out);
    /* Pages: the package moved b to its own page; the child's new page follows
     * the published pages. */
    out = rebase_one("{ inherit = \"p\"; edit = { propertySheets = { num = 2; item[0] = { properties = { "
        "item[1] = { descriptionTag = \"#str_b\"; } } } item[1] = { properties = { num = 1; item[0] = { path = \"q\"; } } } } } }",
        SHEET("num = 2; item[0] = { path = \"a\"; } item[1] = { path = \"b\"; }"),
        "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 1; item[0] = { path = \"a\"; } } } "
        "item[1] = { properties = { num = 1; item[0] = { path = \"b\"; } } } } } }", 0, &result, &kind);
    CHECK(result == 1 && out);
    if (out) rebased_layout(out, "propertySheets", 0, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "a"));
    if (out) rebased_layout(out, "propertySheets", 1, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "b=#str_b"));
    if (out) rebased_layout(out, "propertySheets", 2, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "q"));
    free(out);
    /* Flat lists rebase by value. */
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 3; item[2] = \"z\"; } } }",
        "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } } }",
        "{ edit = { inputs = { num = 3; item[0] = \"x\"; item[1] = \"y\"; item[2] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result == 1 && out); if (out) rebased_layout(out, "inputs", -1, layout, sizeof(layout));
    CHECK(out && !strcmp(layout, "x,y,w,z"));
    free(out);
    /* Only the child's own list shape can make it malformed. */
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { bogus = 1; } } }",
        "{ edit = { inputs = { num = 1; item[0] = \"x\"; } } }",
        "{ edit = { inputs = { num = 2; item[0] = \"x\"; item[1] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result < 0 && kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out);
    out = rebase_one("{ inherit = \"p\"; edit = { inputs = { num = 2; item[1] = \"z\"; } } }",
        "{ edit = { inputs = { num = broken; } } }",
        "{ edit = { inputs = { num = 1; item[0] = \"w\"; } } }", 0, &result, &kind);
    CHECK(result < 0 && kind != SH_PACKAGE_EDITOR_FAILURE_SOURCE && !out);
}

static void test_requirement_reduction(void)
{
    const char *descriptors[] = {
        "{\"id\":\"a\",\"name\":\"A\",\"requirements\":{\"cvars\":{"
        "\"g_useResourceBlackList\":1,\"g_useImageBlackList\":false}}}",
        "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{"
        "\"g_useImageBlackList\":true,\"g_useResourceBlackList\":0}}}"
    };
    const char *invalid[] = {"{\"unknownGate\":0}", "{\"g_useImageBlackList\":2}",
        "{\"g_useImageBlackList\":\"0\"}", "{\"g_useImageBlackList\":null}",
        "{\"g_useImageBlackList\":{}}", "{\"g_useImageBlackList\\u0000suffix\":0}"};
    sh_package packages[2] = {0};
    sh_package_component components[2] = {0};
    sh_package_sources sources = {packages, 2, components, 2};
    sh_package_compile_environment environment = {0};
    sh_package_owners conflicted = {0}, selected = {0};
    size_t rejected = 0;
    char error[1024], descriptor[1024];
    char *expected = NULL;
    strcpy_s(packages[0].name, sizeof(packages[0].name), "a");
    strcpy_s(packages[1].name, sizeof(packages[1].name), "b");
    for (size_t i = 0; i < 2; i++) {
        components[i].owner = i; components[i].relative = "";
        CHECK(sh_package_descriptor_parse(descriptors[i], strlen(descriptors[i]),
            &components[i].descriptor, error, sizeof(error)));
    }
    environment.invalid_package = &rejected; environment.conflicted_packages = &conflicted;
    for (int independent = 0; independent < 2; independent++) {
        environment.independent_sources = independent;
        for (int reversed = 0; reversed < 2; reversed++) {
            sh_package_compilation *compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
            CHECK(compiled && rejected == SIZE_MAX && !sh_package_owners_count(&conflicted));
            if (compiled) {
                sh_package_policy selected_policy = {0};
                const char *cvars = sh_json_object_get(&compiled->policy.requirements, "cvars");
                char *serialized = sh_json_serialize_object(&compiled->policy.requirements, 0, NULL);
                CHECK(compiled->sources->package_count == 2 && cvars && !strcmp(cvars,
                    "{\"g_useImageBlackList\":0,\"g_useResourceBlackList\":0}"));
                if (!expected) expected = serialized;
                else { CHECK(serialized && !strcmp(serialized, expected)); free(serialized); }
                CHECK(sh_package_owners_add(&selected, 0));
                CHECK(sh_package_compilation_policy(compiled, &selected, &selected_policy, error, sizeof(error)));
                cvars = sh_json_object_get(&selected_policy.requirements, "cvars");
                CHECK(cvars && !strcmp(cvars, "{\"g_useImageBlackList\":0}"));
                sh_package_policy_free(&selected_policy); sh_package_owners_free(&selected);
            }
            sh_package_compilation_free(compiled);
            sh_package_component temporary = components[0]; components[0] = components[1]; components[1] = temporary;
        }
    }
    /* The derived union must not rewrite the descriptor retained for delivery. */
    CHECK(!strcmp(sh_package_descriptor_section(&components[0].descriptor, "requirements"),
        "{\"cvars\":{\"g_useResourceBlackList\":1,\"g_useImageBlackList\":false}}"));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        sh_package_compilation *compiled;
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"b\",\"name\":\"B\","
            "\"requirements\":{\"cvars\":%s}}", invalid[i]);
        sh_package_descriptor_free(&components[1].descriptor);
        CHECK(sh_package_descriptor_parse(descriptor, strlen(descriptor), &components[1].descriptor, error, sizeof(error)));
        compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
        CHECK(!compiled && rejected == 1 && strstr(error, "unsupported setting") &&
            !sh_package_owners_count(&conflicted));
        sh_package_compilation_free(compiled);
        sources.package_count = sources.component_count = 1;
        compiled = sh_package_compile_with(&sources, &environment, error, sizeof(error));
        CHECK(compiled && rejected == SIZE_MAX);
        sh_package_compilation_free(compiled);
        sources.package_count = sources.component_count = 2;
    }
    /* Map policy overlay still owns presentation and historical string values.
     * A map's restrictive permission request cannot close a local open gate. */
    sh_package_descriptor_free(&components[0].descriptor);
    sh_package_descriptor_free(&components[1].descriptor);
    descriptors[0] = "{\"id\":\"a\",\"name\":\"A\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},"
        "\"strings\":{\"en\":{\"#str_demo\":\"Local\"}},\"hud\":{\"weapons\":{\"demo\":{\"ammo_display\":\"weapon\"}}}}";
    descriptors[1] = "{\"id\":\"b\",\"name\":\"B\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":true}},"
        "\"strings\":{\"en\":{\"#str_demo\":\"Map\"}},\"hud\":{\"weapons\":{\"demo\":{\"ammo_display\":\"engine\"}}}}";
    for (size_t i = 0; i < 2; i++) CHECK(sh_package_descriptor_parse(descriptors[i], strlen(descriptors[i]),
        &components[i].descriptor, error, sizeof(error)));
    {
        sh_package_compilation view = {0}; sh_package_policy policy = {0}; sh_package_owners selected = {0};
        view.sources = &sources; view.map_overlay = 1; view.map_owner_begin = 1;
        CHECK(sh_package_owners_add(&selected, 0) && sh_package_owners_add(&selected, 1));
        CHECK(sh_package_compilation_policy(&view, &selected, &policy, error, sizeof(error)));
        const char *cvars = sh_json_object_get(&policy.requirements, "cvars");
        const char *strings = sh_json_object_get(&policy.strings, "en");
        const char *weapons = sh_json_object_get(&policy.hud, "weapons");
        CHECK(cvars && strstr(cvars, "g_useImageBlackList") && strstr(cvars, "0"));
        CHECK(strings && strstr(strings, "Map") && !strstr(strings, "Local"));
        CHECK(weapons && strstr(weapons, "engine") && !strstr(weapons, "weapon\""));
        sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    }
    for (size_t i = 0; i < 2; i++) sh_package_descriptor_free(&components[i].descriptor);
    sh_package_owners_free(&conflicted); free(expected);
}

static void test_encounter_membership_order(void)
{
    static const char *types[] = {"snappropertyinspector_whitelistencounterdecl",
        "snappropertyinspector_whitelistencounterdecllist"};
    const char *base_text = "{ edit = { validEncounters = { num = 0; } } }";
    const char *a = "{ edit = { validEncounters = { num = 2; item[0] = \"alpha\"; item[1] = \"beta\"; } } }";
    const char *b = "{ edit = { validEncounters = { num = 4; item[0] = \"beta\"; item[1] = \"alpha\"; "
        "item[2] = \"gamma\"; item[3] = \"beta\"; } } }";
    sh_decl_source base = {base_text, strlen(base_text)}, parts[] = {{a, strlen(a)}, {b, strlen(b)}};
    for (size_t type = 0; type < 2; type++) for (int reverse = 0; reverse < 2; reverse++) {
        sh_decl_source inputs[] = {parts[reverse], parts[!reverse]};
        char error[1024]; size_t length = 0;
        char *out = sh_package_editor_contributions(types[type], base, inputs, 2, inputs[0], 0,
            &length, error, sizeof(error));
        if (!out) fprintf(stderr, "encounter membership %s: %s\n", types[type], error);
        CHECK(out);
        if (out) {
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){out, length}, error, sizeof(error));
            const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters");
            const sh_decl_node *num = sh_decl_tree_member(list, "num");
            CHECK(num && !strcmp(num->value, "3"));
            CHECK(strstr(out, "\"alpha\"") && strstr(out, "\"beta\"") && strstr(out, "\"gamma\""));
            sh_decl_tree_free(tree); free(out);
        }
    }
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
    test_prune_native();
    test_prune_native_names();
    test_builtin_composition(compiled);
    test_editor_wiring_collections();
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
    test_new_declarations(); test_native_original_provenance(); test_duplicate_editor_controls();
    test_requirement_reduction(); test_editor_rebase(); test_encounter_membership_order(); cleanup();
    if (failures) return 1;
    puts("package compiler checks passed"); return 0;
}
