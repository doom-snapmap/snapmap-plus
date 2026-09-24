/* Real whole-bundle ownership with synthetic opaque native object keys. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_occurrence.h"
#include "package_fixture.h"

static sh_package_bundle_binding sources[4];
static unsigned char original_trees[4][32];

static void make_sources(void)
{
    for (size_t i = 0; i < 4; i++) {
        char marker[160], extra[64], error[512];
        size_t length;
        snprintf(marker, sizeof(marker), " { \"id\": \"same-author-id\", \"name\": \"Source %zu\" } ", i);
        snprintf(extra, sizeof(extra), "exact unused source %zu bytes", i);
        sh_package_archive_file members[] = {
            {"package.json", (unsigned char *)marker, strlen(marker), 0},
            {"author-note.txt", (unsigned char *)extra, strlen(extra), 0},
            {"empty", NULL, 0, 1}
        };
        sh_package_archive_files files = {members, 3};
        unsigned char *archive = sh_package_archive_write(&files, &length, error, sizeof(error)); CHECK(archive);
        sources[i].original = archive ? sh_package_bundle_from_archive(archive, length, root, error, sizeof(error)) : NULL;
        CHECK(sources[i].original);
        snprintf(sources[i].source_name, sizeof(sources[i].source_name), "private-owner-%zu", i);
        if (sources[i].original) {
            memcpy(original_trees[i], sh_package_bundle_fingerprint(sources[i].original), 32);
            memcpy(sources[i].compiler_tree, original_trees[i], 32);
        }
        free(archive);
    }
    /* The tracker must never replace an exact original by the first source
     * having an equivalent normalized compiler tree. */
    memset(sources[0].compiler_tree, 0xc1, 32);
    memcpy(sources[1].compiler_tree, sources[0].compiler_tree, 32);
}
static size_t binding(const sh_package_provenance *value, const char *name)
{
    for (size_t i = 0; i < value->binding_count; i++) if (!strcmp(value->bindings[i].name, name)) return i;
    return SIZE_MAX;
}
static void make_document(sh_package_provenance *document)
{
    size_t contexts[4];
    for (size_t i = 0; i < 4; i++) {
        contexts[i] = sh_package_provenance_context_add(document, SH_PACKAGE_CONTEXT_ISOLATED,
            original_trees + i, 1, i == 2 ? "unused-component" : ""); CHECK(contexts[i] != SIZE_MAX);
    }
    CHECK(sh_package_provenance_bind(document, "entitydef", "actors/boss", "smp/shared-boss", contexts, 3));
    CHECK(sh_package_provenance_bind(document, "material", "world/light", "smp/world-light", contexts + 3, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", "/m7:inherit"};
    CHECK(sh_package_provenance_use_set(document, &site, binding(document, "smp/shared-boss"), contexts, 1));
    site.reference = "/m10:properties/m6:target";
    CHECK(sh_package_provenance_use_set(document, &site, binding(document, "smp/shared-boss"), contexts, 1));
    site.owner = "entity/1"; site.reference = "/m7:inherit";
    CHECK(sh_package_provenance_use_set(document, &site, binding(document, "smp/shared-boss"), contexts + 1, 1));
    site = (sh_package_provenance_site){SH_PACKAGE_USE_WORLD, "world", "/m8:lighting"};
    CHECK(sh_package_provenance_use_set(document, &site, binding(document, "smp/world-light"), contexts + 3, 1));
}
static void expect(sh_package_occurrence_snapshot *snapshot, size_t uses, size_t contexts, size_t bundles)
{
    size_t count;
    const sh_package_provenance *value = sh_package_occurrence_provenance(snapshot);
    CHECK(value && value->semantics == SH_PACKAGE_PROVENANCE_USES);
    if (value) CHECK(value->use_count == uses && value->context_count == contexts);
    sh_package_occurrence_bundles(snapshot, &count); CHECK(count == bundles);
}
static void expect_tree(sh_package_occurrence_snapshot *snapshot, size_t use, size_t original)
{
    const sh_package_provenance *value = sh_package_occurrence_provenance(snapshot);
    CHECK(value && use < value->use_count);
    if (value && use < value->use_count) {
        const sh_package_provenance_use *row = value->uses + use;
        CHECK(row->context_count == 1 && row->contexts[0] < value->context_count);
        if (row->context_count == 1 && row->contexts[0] < value->context_count)
            CHECK(!memcmp(value->contexts[row->contexts[0]].trees[0], original_trees[original], 32));
    }
}
static void release_source_rows(void)
{
    for (size_t i = 0; i < 4; i++) { sh_package_bundle_release(sources[i].original); sources[i].original = NULL; }
}

typedef struct lease_counter { int references, releases; } lease_counter;
static void lease_retain(void *context) { ((lease_counter *)context)->references++; }
static void lease_release(void *context)
{ lease_counter *counter = context; counter->references--; counter->releases++; }
static void retained_resource_slices(void)
{
    sh_package_provenance value = {0}; char error[512];
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_snapshot *document, *world = NULL, *exported, *held;
    lease_counter counters[4] = {{0}};
    const uintptr_t objects[] = {0x1000, 0x2000};
    const sh_package_occurrence_key clipboard = {2, 0x1000}, undo = {3, 0x1000};
    make_document(&value);
    document = sh_package_occurrence_prepare_document(&value, sources, 4, error, sizeof(error)); CHECK(document);
    const sh_package_provenance *prepared = sh_package_occurrence_provenance(document);
    if (!document || !tracker) goto done;
    for (size_t u = 0; u < prepared->use_count; u++) {
        const sh_package_provenance_use *use = prepared->uses + u;
        size_t context = use->contexts[0], original = 0;
        while (original < 4 && memcmp(prepared->contexts[context].trees[0], original_trees[original], 32)) original++;
        CHECK(original < 4);
        if (original < 4) CHECK(sh_package_occurrence_bind_resource(document, use->binding, context,
            (sh_package_occurrence_resource){counters + original, lease_retain, lease_release}));
    }
    CHECK(counters[0].references == 1 && counters[1].references == 1 && counters[2].references == 0 && counters[3].references == 1);
    held = sh_package_occurrence_retain(document);
    CHECK(!sh_package_occurrence_bind_resource(document, 0, 0,
        (sh_package_occurrence_resource){counters, lease_retain, lease_release}));
    sh_package_occurrence_release(held);
    CHECK(sh_package_occurrence_attach_snapshot(tracker, 1, objects, 2, document, &world, error, sizeof(error)));
    sh_package_occurrence_release(document); document = NULL;
    CHECK(counters[0].references == 1 && counters[1].references == 1 && counters[3].references == 1);
    CHECK(sh_package_occurrence_copy(tracker, clipboard, (sh_package_occurrence_key){1, objects[0]}));
    CHECK(sh_package_occurrence_copy(tracker, undo, (sh_package_occurrence_key){1, objects[1]}));
    sh_package_occurrence_erase_namespace(tracker, 1);
    sh_package_occurrence_release(world); world = NULL;
    CHECK(counters[0].references == 1 && counters[1].references == 1 && !counters[3].references);
    /* A shares its native name with B, but exporting only A keeps only A's lease. */
    exported = sh_package_occurrence_export(tracker, &clipboard, 1, NULL, error, sizeof(error));
    expect(exported, 2, 1, 1);
    CHECK(counters[0].references == 2 && counters[1].references == 1);
    const uintptr_t duplicate[] = {0x3000, 0x3000};
    CHECK(!sh_package_occurrence_attach_snapshot(tracker, 9, duplicate, 2, exported, &world, error, sizeof(error)));
    CHECK(counters[0].references == 2 && counters[1].references == 1);
    sh_package_occurrence_erase(tracker, clipboard); sh_package_occurrence_erase(tracker, undo);
    CHECK(counters[0].references == 1 && !counters[1].references);
    sh_package_occurrence_release(exported); CHECK(!counters[0].references);
done:
    sh_package_occurrence_release(document); sh_package_occurrence_release(world);
    sh_package_occurrence_tracker_free(tracker); sh_package_provenance_free(&value);
    for (size_t i = 0; i < 4; i++) CHECK(!counters[i].references);
}

static void copied_and_undo(void)
{
    const uintptr_t pointers[] = {0x1000, 0x2000};
    const sh_package_occurrence_key flat_a = {11, 0x1000}, flat_b = {11, 0x2000};
    const sh_package_occurrence_key live_a = {12, 0x1000}, live_b = {12, 0x2000};
    const sh_package_occurrence_key clipboard = {21, 0x3000}, undo = {22, 0x1000}, redo = {23, 0x4000};
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_snapshot *world = NULL, *a = NULL, *b = NULL, *get = NULL, *exported = NULL;
    sh_package_provenance document = {0}, malformed = {0};
    char error[512];
    make_document(&document); CHECK(tracker);
    CHECK(sh_package_occurrence_attach_document(tracker, 11, pointers, 2, &document,
        sources, 4, &world, error, sizeof(error)) == SH_PACKAGE_OCCURRENCE_READY);
    a = sh_package_occurrence_get(tracker, flat_a); b = sh_package_occurrence_get(tracker, flat_b);
    expect(a, 2, 1, 1); expect(b, 1, 1, 1); expect(world, 1, 1, 1);
    expect_tree(a, 0, 0); expect_tree(b, 0, 1);
    {
        sh_package_occurrence_snapshot *unchanged = b;
        CHECK(!sh_package_occurrence_prepare_entity(&document, 1, sources, 1, &b,
            error, sizeof(error)) && b == unchanged);
    }
    {
        size_t count;
        const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(b, &count);
        CHECK(count == 1 && rows && !memcmp(sh_package_bundle_fingerprint(rows[0].original), original_trees[1], 32));
    }
    CHECK(sh_package_occurrence_copy(tracker, live_a, flat_a));
    CHECK(sh_package_occurrence_copy(tracker, clipboard, live_a));
    CHECK(sh_package_occurrence_copy(tracker, undo, live_a));
    CHECK(sh_package_occurrence_move(tracker, live_b, flat_b));
    get = sh_package_occurrence_get(tracker, flat_b); CHECK(!get); sh_package_occurrence_release(get);
    get = sh_package_occurrence_get(tracker, undo); CHECK(get == a); sh_package_occurrence_release(get);
    /* Changing only the live occurrence preserves its shared undo/clipboard. */
    CHECK(sh_package_occurrence_set(tracker, live_a, b));
    CHECK(sh_package_occurrence_copy(tracker, redo, live_a));
    get = sh_package_occurrence_get(tracker, undo); CHECK(get == a); sh_package_occurrence_release(get);
    get = sh_package_occurrence_get(tracker, live_a); CHECK(get == b); sh_package_occurrence_release(get);
    sh_package_occurrence_erase(tracker, live_a);
    CHECK(sh_package_occurrence_copy(tracker, live_a, undo));
    get = sh_package_occurrence_get(tracker, live_a); CHECK(get == a); sh_package_occurrence_release(get);
    CHECK(sh_package_occurrence_copy(tracker, live_a, redo));
    get = sh_package_occurrence_get(tracker, live_a); CHECK(get == b); sh_package_occurrence_release(get);
    CHECK(sh_package_occurrence_copy(tracker, live_a, live_a));
    /* Same numeric pointer in distinct namespaces has independent lifetime. */
    get = sh_package_occurrence_get(tracker, flat_a); CHECK(get == a); sh_package_occurrence_release(get);
    sh_package_occurrence_erase(tracker, live_a); CHECK(!sh_package_occurrence_get(tracker, live_a));
    CHECK(sh_package_occurrence_set(tracker, live_a, b));
    CHECK(sh_package_occurrence_copy(tracker, live_a, (sh_package_occurrence_key){12, 0xdead}));
    CHECK(!sh_package_occurrence_get(tracker, live_a));
    CHECK(sh_package_occurrence_set(tracker, live_a, b));
    /* Failed full preparation must not overwrite any old flat/world entry. */
    sh_package_occurrence_snapshot *old_world = world;
    CHECK(!sh_package_occurrence_attach_document(tracker, 11, pointers, 2, &document,
        sources, 1, &world, error, sizeof(error)) && world == old_world);
    get = sh_package_occurrence_get(tracker, flat_a); CHECK(get == a); sh_package_occurrence_release(get);
    CHECK(!sh_package_occurrence_get(tracker, flat_b));
    const uintptr_t duplicate[] = {0x1000, 0x1000};
    CHECK(!sh_package_occurrence_attach_document(tracker, 11, duplicate, 2, &document,
        sources, 4, &world, error, sizeof(error)) && world == old_world);
    CHECK(sh_package_provenance_select_uses(&document, NULL, NULL, &malformed));
    free(malformed.uses[0].owner); malformed.uses[0].owner = _strdup("entity/02");
    CHECK(!sh_package_occurrence_attach_document(tracker, 11, pointers, 2, &malformed,
        sources, 4, &world, error, sizeof(error)) && world == old_world);
    sh_package_provenance_free(&malformed);
    CHECK(!sh_package_occurrence_prepare_selected(&document, SH_PACKAGE_OCCURRENCE_ENTITY,
        SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES, sources, 4, error, sizeof(error)));
    /* Serialization order alone establishes entity/N; native IDs are absent. */
    sh_package_occurrence_key order[] = {live_b, undo};
    exported = sh_package_occurrence_export(tracker, order, 2, world, error, sizeof(error));
    expect(exported, 4, 3, 3); expect_tree(exported, 0, 1); expect_tree(exported, 1, 0);
    const sh_package_provenance *value = sh_package_occurrence_provenance(exported);
    if (value) {
        CHECK(!strcmp(value->uses[0].owner, "entity/0") && !strcmp(value->uses[1].owner, "entity/1"));
        CHECK(value->uses[3].kind == SH_PACKAGE_USE_WORLD && !strcmp(value->uses[3].owner, "world"));
    }
    CHECK(!sh_package_occurrence_set(tracker, live_a, exported));
    sh_package_occurrence_release(exported);
    /* Map exit drops live/flat ownership without destroying clipboard/undo. */
    sh_package_occurrence_erase_namespace(tracker, 11); sh_package_occurrence_erase_namespace(tracker, 12);
    sh_package_occurrence_release(a); a = NULL; sh_package_occurrence_release(b); b = NULL;
    sh_package_occurrence_release(world); world = NULL; sh_package_provenance_free(&document);
    release_source_rows();
    exported = sh_package_occurrence_export(tracker, &clipboard, 1, NULL, error, sizeof(error));
    expect(exported, 2, 1, 1); expect_tree(exported, 0, 0);
    sh_package_occurrence_tracker_free(tracker); tracker = NULL;
    /* The exported clipboard document is its own strong source owner. */
    size_t count, length;
    const sh_package_bundle_binding *rows = sh_package_occurrence_bundles(exported, &count);
    unsigned char *archive = count ? sh_package_bundle_read(rows[0].original, &length, error, sizeof(error)) : NULL;
    CHECK(archive && count == 1);
    if (archive) {
        unsigned char identity[32];
        CHECK(sh_package_archive_tree_identity(archive, length, identity, error, sizeof(error)) &&
            !memcmp(identity, original_trees[0], 32)); free(archive);
    }
    sh_package_occurrence_release(exported);
}

static void historical_and_empty(void)
{
    sh_package_provenance historical = {0}, selected = {0};
    sh_package_occurrence_snapshot *old = NULL, *automatic = NULL, *world = NULL, *exported = NULL;
    sh_package_occurrence_tracker *tracker = sh_package_occurrence_tracker_create();
    sh_package_occurrence_key key = {77, 0x1000}; uintptr_t object = key.object;
    char error[512];
    size_t index = sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])sources[0].compiler_tree, 1, NULL);
    CHECK(index != SIZE_MAX && sh_package_provenance_bind(&historical, "entitydef", "old/boss", "old/boss", &index, 1));
    historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    CHECK(!sh_package_occurrence_prepare_selected(&historical, SH_PACKAGE_OCCURRENCE_ENTITY,
        SH_PACKAGE_OCCURRENCE_ORIGINAL_TREES, sources, 4, error, sizeof(error)));
    old = sh_package_occurrence_prepare_selected(&historical, SH_PACKAGE_OCCURRENCE_ENTITY,
        SH_PACKAGE_OCCURRENCE_COMPILER_TREES, sources, 4, error, sizeof(error));
    /* Two explicitly selected originals share a normalized tree: retain both. */
    expect(old, 1, 1, 2);
    automatic = sh_package_occurrence_retain(old);
    CHECK(sh_package_occurrence_prepare_entity(&historical, 0, sources, 4, &automatic,
        error, sizeof(error)) == SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION && automatic == old);
    CHECK(sh_package_occurrence_set(tracker, key, old));
    CHECK(sh_package_occurrence_attach_document(tracker, key.namespace_id, &object, 1, &historical,
        sources, 4, &world, error, sizeof(error)) == SH_PACKAGE_OCCURRENCE_NEEDS_SELECTION && !world);
    exported = sh_package_occurrence_export(tracker, &key, 1, NULL, error, sizeof(error)); expect(exported, 1, 1, 2);
    const sh_package_provenance *value = sh_package_occurrence_provenance(exported);
    if (value) CHECK(value->uses[0].kind == SH_PACKAGE_USE_HISTORICAL &&
        !*value->uses[0].owner && !*value->uses[0].reference);
    sh_package_occurrence_release(exported);
    /* A narrowed caller-selected compatibility row is preserved faithfully. */
    CHECK(sh_package_provenance_select_uses(&historical, NULL, NULL, &selected));
    sh_package_occurrence_snapshot *one = sh_package_occurrence_prepare_selected(&selected,
        SH_PACKAGE_OCCURRENCE_ENTITY, SH_PACKAGE_OCCURRENCE_COMPILER_TREES, sources + 1, 1, error, sizeof(error));
    expect(one, 1, 1, 1); sh_package_occurrence_release(one);
    /* A mixed object's new typed use must still match its original identity,
     * while its explicitly selected old aggregate uses compiler compatibility. */
    size_t modern = sh_package_provenance_context_add(&selected, SH_PACKAGE_CONTEXT_ISOLATED,
        original_trees + 1, 1, "");
    CHECK(modern != SIZE_MAX && sh_package_provenance_bind(&selected, "model", "new/mesh", "smp/new-mesh", &modern, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/7", "/m5:model"};
    CHECK(sh_package_provenance_use_set(&selected, &site, binding(&selected, "smp/new-mesh"), &modern, 1));
    one = sh_package_occurrence_prepare_selected(&selected, SH_PACKAGE_OCCURRENCE_ENTITY,
        SH_PACKAGE_OCCURRENCE_COMPILER_TREES, sources, 4, error, sizeof(error));
    expect(one, 2, 2, 2); sh_package_occurrence_release(one);
    CHECK(!sh_package_occurrence_prepare_selected(&selected, SH_PACKAGE_OCCURRENCE_ENTITY,
        SH_PACKAGE_OCCURRENCE_COMPILER_TREES, sources, 1, error, sizeof(error)));
    CHECK(sh_package_occurrence_attach_document(tracker, key.namespace_id, &object, 1, NULL,
        NULL, 0, &world, error, sizeof(error)) == SH_PACKAGE_OCCURRENCE_READY);
    exported = sh_package_occurrence_export(tracker, &key, 1, world, error, sizeof(error)); expect(exported, 0, 0, 0);
    CHECK(!sh_package_occurrence_set(tracker, key, world));
    sh_package_occurrence_release(exported); sh_package_occurrence_release(world);
    sh_package_occurrence_release(automatic); sh_package_occurrence_release(old);
    sh_package_occurrence_tracker_free(tracker);
    sh_package_provenance_free(&historical); sh_package_provenance_free(&selected);
}

int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "poc", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    make_sources(); historical_and_empty(); retained_resource_slices(); copied_and_undo();
    release_source_rows(); cleanup();
    printf("package_occurrence_test: %d failures\n", failures); return failures ? 1 : 0;
}
