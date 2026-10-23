/* The production map-document chain over historical-writer carriers (a beta13
 * legacy package and a beta14/15 descriptor-keyed modern package): the real
 * private map context (map_package.c), document bind (package_document.c:
 * prepare, rebind, native representation), capture, the save route's packing
 * and embedding (map_embed.c), provenance and reopen, against the runtime's
 * typed-entity fixture. Private owner folders are context-local ordinals,
 * independent of carrier spelling, so preparation follows the context's record. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "package_fixture.h"
#include "hook.h"
#include "map_package.h"
#include "map_embed.h"
#include "map_provenance.h"
#include "package_document.h"
#include "package_occurrence.h"
#include "map_package_historical_fixtures.h"
#include "package_archive.h"
#include "patch.h"
#include "engine_dialog.h"
#include "decl_server.h"
#include "typeinfo.h"
#include "resource_graph_prepared.h"
#include "prepare_worker.h"

void backend_log(const char *message)
{ if (message && getenv("SH_DOCUMENT_TEST_LOG")) fprintf(stderr, "log: %s\n", message); }
int sh_user_overrides_enabled_for_launch(void) { return 1; }
/* No native code is patched here. */
void *hook_prepare(void *target, void *detour, size_t stolen) { (void)target; (void)detour; (void)stolen; return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_REFUSED_BADARG; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 0; }
int hook_is_installed(void *trampoline) { return trampoline != NULL; }
sh_patch_status code_unpatch(sh_patch_handle *handle) { (void)handle; CHECK(0); return B2_PATCH_FAIL_NOTLIVE; }
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
    size_t len, sh_patch_handle *out_handle)
{ (void)r; (void)expect; (void)new_bytes; (void)len; (void)out_handle; CHECK(0); return B2_PATCH_REFUSED_BADARG; }
/* Calls this chain never reaches: engine dialogs, registration, the live
 * registry and reflection, and the save-time prepared map graph. */
int sh_engine_dialog_can_ask(void) { return 0; }
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{ (void)gdm_id; (void)button_set; (void)text; CHECK(0); return 0; }
int sh_engine_dialog_poll(int ticket) { (void)ticket; CHECK(0); return 0; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }
int sh_decl_server_registration_succeeded(void) { return 1; }
void sh_decl_server_request_rearm(void) {}
int sh_decl_server_registry_source(sh_decl_registry_source *source) { (void)source; CHECK(0); return 0; }
void *sh_typeinfo_get_reflect(void) { CHECK(0); return NULL; }
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source, uintptr_t reflection)
{ (void)source; (void)reflection; CHECK(0); return NULL; }
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *preparation, const char *name,
    sh_decl_dependency_result *result)
{ (void)preparation; (void)name; (void)result; CHECK(0); return 0; }
int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *preparation, const char *class_name,
    const char *inherit, const char *json, size_t length, int (*visitor)(void *context, const char *type, const char *name),
    void *context, sh_decl_dependency_result *result)
{
    (void)preparation; (void)class_name; (void)inherit; (void)json; (void)length; (void)visitor; (void)context; (void)result;
    CHECK(0); return 0;
}
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *preparation) { (void)preparation; }

#include "document_fixture.h"

/* The deepest data directory under root whose private map inventory directory
 * can be created (the same ANSI directory calls, a placeholder map name). */
static size_t inventory_cap(void)
{
    size_t base = strlen(root), cap = 0;
    for (size_t depth = base + 2; depth < MAX_PATH - 20; depth++) {
        static const char *const levels[] = {"", "\\package-cache", "\\package-cache\\maps",
            "\\package-cache\\maps\\map-00000000000000000000000000000000",
            "\\package-cache\\maps\\map-00000000000000000000000000000000\\overrides"};
        char path[MAX_PATH], padding[MAX_PATH];
        int made = 1;
        memset(padding, 'c', depth - base - 1); padding[depth - base - 1] = 0;
        for (size_t l = 0; made && l < sizeof(levels) / sizeof(*levels); l++)
            made = snprintf(path, sizeof(path), "%s\\%s%s", root, padding, levels[l]) < (int)sizeof(path) &&
                (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
        for (size_t l = sizeof(levels) / sizeof(*levels); l-- > 0;)
            if (snprintf(path, sizeof(path), "%s\\%s%s", root, padding, levels[l]) < (int)sizeof(path)) RemoveDirectoryA(path);
        if (!made) break;
        cap = depth;
    }
    return cap;
}

/* One open, bind and capture of a map document; returns the plan. */
typedef struct document_pass {
    sh_mpkg_context *context;
    sh_package_map_plan *plan;
    char *bound; size_t bound_length;
    sh_package_provenance selected;
    char *captured; size_t captured_length;
    sh_package_occurrence_snapshot *ownership;
    sh_mpkg_used *used; size_t used_count;
} document_pass;
static void pass_free(document_pass *p)
{
    sh_package_map_plan_free(p->plan); sh_package_provenance_free(&p->selected);
    if (p->bound) HeapFree(GetProcessHeap(), 0, p->bound);
    free(p->captured); sh_package_occurrence_release(p->ownership); sh_mpkg_used_free(p->used);
    CHECK(sh_mpkg_context_close(&p->context));
    memset(p, 0, sizeof(*p));
}
static int pass_run(document_pass *p, const char *data, const char *map, size_t length, const char *carrier)
{
    char error[2048] = "", *native = NULL;
    size_t native_length = 0, count = 0;
    int present = 0;
    memset(p, 0, sizeof(*p));
    p->context = sh_mpkg_context_open(data, map, length, error, sizeof(error));
    if (!p->context) fprintf(stderr, "document context: %s\n", error);
    CHECK(p->context && sh_mpkg_context_count(p->context) == 1);
    if (!p->context) return 0;
    /* The private owner folder is an ordinal, never the carrier's spelling. */
    CHECK(!strcmp(sh_mpkg_context_bundles(p->context, &count)[0].source_name, "0") &&
        !strcmp(sh_mpkg_context_carrier(p->context, 0), carrier) && strcmp(carrier, "0"));
    p->bound = heap_copy(map, length); p->bound_length = length;
    CHECK(p->bound != NULL);
    if (!p->bound || !sh_package_document_bind(data, p->context, &p->bound, &p->bound_length, &p->plan, error, sizeof(error))) {
        fprintf(stderr, "document bind: %s\n", error); CHECK(0); return 0;
    }
    CHECK(p->plan != NULL);
    CHECK(sh_map_provenance_read(p->bound, p->bound_length, &p->selected, &present, error, sizeof(error)) && present);
    /* The engine serializes the document without delivery variables. */
    native = sh_mpkg_strip(p->bound, p->bound_length, &native_length);
    if (!native) { native = heap_copy(p->bound, p->bound_length); native_length = p->bound_length; }
    p->captured = native ? sh_package_runtime_capture_document(p->plan, &p->selected, NULL, 0, native, native_length,
        &p->captured_length, &p->ownership, &p->used, &p->used_count, error, sizeof(error)) : NULL;
    if (!p->captured) fprintf(stderr, "document capture: %s\n", error);
    CHECK(p->captured && p->ownership && p->used_count == 1);
    if (native) HeapFree(GetProcessHeap(), 0, native);
    return p->captured && p->used_count == 1;
}

/* A historical writer's map: its carrier id, its exact archive, the migration
 * seed a legacy archive keeps (NULL for an already-modern one) and its note as
 * the bound document must keep it. */
typedef struct document_fixture {
    const char *map; size_t length;
    const char *carrier;
    const unsigned char *payload; size_t payload_length;
    const char *seed;
    const char *note;
} document_fixture;

/* The historical carrier through open, bind, capture, the save route and
 * reopen under data; the delivery stays its exact original archive. */
static void test_document_chain(const char *relative, const document_fixture *fixture)
{
    char data[MAX_PATH], error[2048] = "", key[SH_PACKAGE_BUNDLE_KEY_CAP], overrides[MAX_PATH];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    document_pass first = {0}, second = {0}, renamed = {0};
    char *saved = NULL, *provenanced = NULL;
    size_t saved_length = 0, provenanced_length = 0;
    unsigned char tree[32];
    snprintf(overrides, sizeof(overrides), "%s/overrides", relative); create(overrides, NULL);
    snprintf(data, sizeof(data), "%s/%s", root, relative);
    CHECK(sh_package_archive_tree_identity(fixture->payload, fixture->payload_length, tree, error, sizeof(error)));
    {
        /* The complete-tree key a re-save uses; a legacy archive's also names its seed. */
        size_t at = fixture->seed ? 16 : 9;
        memcpy(key, fixture->seed ? "delivery.legacy." : "delivery.", at);
        for (size_t i = 0; i < 32; i++, at += 2) snprintf(key + at, 3, "%02x", tree[i]);
        if (fixture->seed) {
            key[at++] = '.';
            for (size_t i = 0; fixture->seed[i]; i++, at += 2) snprintf(key + at, 3, "%02x", (unsigned char)fixture->seed[i]);
        }
        key[at] = 0;
    }
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);

    /* First open of the historical writer's carrier: the map's own historical
     * composition, private names, the note's native bytes kept. */
    if (pass_run(&first, data, fixture->map, fixture->length, fixture->carrier)) {
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(first.plan);
        const sh_compiled_resource *own = authored_resource(compiled, "health = 10");
        const sh_compiled_resource *added = authored_resource(compiled, "health = 55");
        size_t bytes = 0;
        unsigned char *payload;
        CHECK(own && own->native_name && !strncmp(own->native_name, "smp/", 4) && !authored_resource(compiled, "health = 100"));
        CHECK(added && added->native_name && own && strstr((const char *)own->body, added->native_name));
        CHECK(own && strstr(first.bound, own->native_name) && strstr(first.bound, fixture->note));
        CHECK(!strcmp(first.used[0].id, key));
        /* The save route: the retained original under its complete-tree key, then provenance. */
        payload = sh_mpkg_pack_used(&first.used[0], &bytes, error, sizeof(error));
        CHECK(payload && bytes == fixture->payload_length && !memcmp(payload, fixture->payload, bytes));
        saved = payload ? sh_mpkg_embed(first.captured, first.captured_length, first.used[0].id, payload, bytes,
            &saved_length, error, sizeof(error)) : NULL;
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
        provenanced = saved ? sh_map_provenance_write(saved, saved_length, sh_package_occurrence_provenance(first.ownership),
            &provenanced_length, error, sizeof(error)) : NULL;
        if (!provenanced) fprintf(stderr, "document save: %s\n", error);
        CHECK(provenanced != NULL);
    }
    pass_free(&first);

    /* Reopen with its recorded provenance: the same composition and delivery. */
    if (provenanced && pass_run(&second, data, provenanced, provenanced_length, key)) {
        const sh_package_compilation *compiled = sh_package_map_plan_compilation(second.plan);
        const sh_compiled_resource *own = authored_resource(compiled, "health = 10");
        const sh_compiled_resource *added = authored_resource(compiled, "health = 55");
        size_t bytes = 0;
        unsigned char *payload = sh_mpkg_pack_used(&second.used[0], &bytes, error, sizeof(error));
        CHECK(own && own->native_name && !strncmp(own->native_name, "smp/", 4) && strstr(second.bound, own->native_name));
        CHECK(added && added->native_name && own && strstr((const char *)own->body, added->native_name));
        CHECK(!strcmp(second.used[0].id, key));
        CHECK(payload && bytes == fixture->payload_length && !memcmp(payload, fixture->payload, bytes));
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
    }
    pass_free(&second);

    /* Discriminator: preparation follows the context's recorded owner folder;
     * a renamed folder is not silently matched by content or id. */
    if (provenanced) {
        renamed.context = sh_mpkg_context_open(data, provenanced, provenanced_length, error, sizeof(error));
        CHECK(renamed.context != NULL);
        if (renamed.context) {
            char from[MAX_PATH], to[MAX_PATH];
            wchar_t *wide_from, *wide_to;
            snprintf(from, sizeof(from), "%s\\overrides\\0", sh_mpkg_context_root(renamed.context));
            snprintf(to, sizeof(to), "%s\\overrides\\renamed", sh_mpkg_context_root(renamed.context));
            wide_from = sh_package_source_wide_path(from); wide_to = sh_package_source_wide_path(to);
            CHECK(wide_from && wide_to && MoveFileW(wide_from, wide_to));
            free(wide_from); free(wide_to);
            renamed.bound = heap_copy(provenanced, provenanced_length); renamed.bound_length = provenanced_length;
            CHECK(renamed.bound && !sh_package_document_bind(data, renamed.context, &renamed.bound, &renamed.bound_length,
                &renamed.plan, error, sizeof(error)));
            CHECK(strstr(error, "exact normalized source owner") != NULL);
        }
    }
    pass_free(&renamed);

    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (provenanced) HeapFree(GetProcessHeap(), 0, provenanced);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        /* Closed contexts leave only their now-empty private maps directory. */
        char maps[MAX_PATH];
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data);
        sh_mpkg_shared_trees_release();
        CHECK(RemoveDirectoryA(maps));
    }
    cleanup_resources(data);
}

/* The live M133 case: a record-less map composed of two carried packages is
 * saved with its larger, used tree over the text budget, so that tree travels
 * as a reference and the smaller one as a delivery, and the record's composed
 * context names both. Reopened with the referenced tree installed in the
 * library (same digest, provided only there), the context lays it out from
 * the library and the record binds with every use resolved and the entity set
 * unchanged. Reopened without it, the load degrades the record and binds the
 * carried delivery alone instead of refusing. */
static size_t count_entities(const char *json)
{
    size_t n = 0;
    for (const char *at = json; at && (at = strstr(at, "\"entityDef\":{")) != NULL; at += 13) n++;
    return n;
}
/* Remove a tree the archive unpacker laid out (create() did not track it),
 * reporting the first entry that would not go. */
static int remove_unpacked_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int ok = 1;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= remove_unpacked_tree(child);
            else if (!DeleteFileA(child)) { fprintf(stderr, "cannot delete %s (%lu)\n", child, GetLastError()); ok = 0; }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    if (!RemoveDirectoryA(path) && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        fprintf(stderr, "cannot remove %s (%lu)\n", path, GetLastError()); ok = 0;
    }
    return ok;
}

static void test_referenced_tree_from_library(void)
{
    char data[MAX_PATH], absent[MAX_PATH], error[2048] = "", err[SH_MPKG_ERR_CAP] = "", folder[MAX_PATH];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_mpkg_context *context = NULL;
    sh_package_map_plan *plan = NULL;
    char *two = NULL, *bound = NULL, *native = NULL, *captured = NULL, *saved = NULL, *text = NULL, *reopened = NULL;
    size_t two_length = 0, bound_length = 0, native_length = 0, captured_length = 0, saved_length = 0, text_length = 0;
    size_t reopened_length = 0, used_count = 0, count = 0, entities;
    sh_package_provenance selected = {0}, record = {0}, after = {0};
    sh_package_occurrence_snapshot *ownership = NULL;
    sh_mpkg_used *used = NULL;
    sh_mpkg_delivery items[2] = {0};
    unsigned char history_tree[32];
    int referenced[2] = {0}, present = 0, history_index = -1;
    create("ref-data/overrides", NULL); create("ref-absent/overrides", NULL);
    snprintf(data, sizeof(data), "%s/ref-data", root);
    snprintf(absent, sizeof(absent), "%s/ref-absent", root);
    CHECK(sh_package_archive_tree_identity(hist_w3_payload, hist_w3_payload_len, history_tree, error, sizeof(error)));
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    /* The record-less carrier: the W3 history map plus a second, smaller
     * package the map does not use (as sp-pistol beside boss-demons). */
    {
        static const char descriptor[] = "{\"id\":\"local.second\",\"name\":\"Second\"}";
        static const char body[] = "{ class = \"Entity\"; edit = { health = 7; } }";
        sh_package_archive_file members[] = {
            {"package.json", (unsigned char *)descriptor, sizeof(descriptor) - 1, 0},
            {"assets/generated/decls/entitydef/history/second.decl", (unsigned char *)body, sizeof(body) - 1, 0}};
        sh_package_archive_files files = {members, 2};
        size_t second_length = 0;
        unsigned char *second = sh_package_archive_write(&files, &second_length, error, sizeof(error));
        CHECK(second);
        two = second ? sh_mpkg_embed(hist_map_w3, hist_map_w3_len, "local.second", second, second_length, &two_length, err, sizeof(err)) : NULL;
        free(second);
    }
    CHECK(two); if (!two) goto done;
    entities = count_entities(two);
    context = sh_mpkg_context_open(data, two, two_length, error, sizeof(error));
    CHECK(context && sh_mpkg_context_count(context) == 2); if (!context) goto done;
    bound = heap_copy(two, two_length); bound_length = two_length;
    if (!sh_package_document_bind(data, context, &bound, &bound_length, &plan, error, sizeof(error))) { fprintf(stderr, "two bind: %s\n", error); CHECK(0); goto done; }
    CHECK(sh_map_provenance_read(bound, bound_length, &selected, &present, error, sizeof(error)) && present);
    native = sh_mpkg_strip(bound, bound_length, &native_length);
    if (!native) { native = heap_copy(bound, bound_length); native_length = bound_length; }
    captured = sh_package_runtime_capture_document(plan, &selected, NULL, 0, native, native_length, &captured_length,
        &ownership, &used, &used_count, error, sizeof(error));
    if (!captured) fprintf(stderr, "two capture: %s\n", error);
    CHECK(captured && ownership && used_count == 2); if (!captured || used_count != 2) goto done;
    /* The save: both deliveries packed; the text budget admits only the smaller. */
    for (size_t i = 0; i < 2; i++) {
        size_t n = 0;
        unsigned char *payload = sh_mpkg_pack_used(&used[i], &n, error, sizeof(error));
        CHECK(payload); items[i].id = used[i].id; items[i].archive = payload; items[i].length = n;
        if (n == hist_w3_payload_len && !memcmp(payload, hist_w3_payload, n)) history_index = (int)i;
    }
    CHECK(history_index >= 0 && items[0].archive && items[1].archive);
    if (history_index < 0 || !items[0].archive || !items[1].archive) goto done;
    saved = sh_mpkg_embed_within(captured, captured_length, items, 2,
        captured_length + (items[1 - history_index].length * 4 / 3) + 600, referenced, &saved_length, &count, err, sizeof(err));
    if (!saved) fprintf(stderr, "two save: %s\n", err);
    CHECK(saved && referenced[history_index] && !referenced[1 - history_index]);
    if (!saved) goto done;
    text = sh_map_provenance_write(saved, saved_length, sh_package_occurrence_provenance(ownership), &text_length, error, sizeof(error));
    CHECK(text && strstr(text, "smpbind.") && strstr(text, "smpref."));
    if (!text) goto done;
    CHECK(sh_map_provenance_read(text, text_length, &record, &present, error, sizeof(error)) && present && record.use_count);
    sh_package_map_plan_free(plan); plan = NULL; CHECK(sh_mpkg_context_close(&context));
    /* Positive: the referenced tree installed in the library, same digest. */
    snprintf(folder, sizeof(folder), "%s/overrides/history-pack", data);
    CHECK(sh_package_archive_unpack(hist_w3_payload, hist_w3_payload_len, folder, NULL, error, sizeof(error)));
    sh_mpkg_boot_capture(data);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_mpkg_reference_missing(text, text_length, NULL, 0) == 0);
    /* The library captured the installed tree as a bundle at its compile; a context that references the tree
     * retains that bundle instead of reading and packing the installed folder again. An unknown tree has none. */
    sh_package_bundle *held = sh_package_runtime_installed_bundle(history_tree);
    {
        unsigned char other[32] = {1};
        CHECK(held && !memcmp(sh_package_bundle_fingerprint(held), history_tree, 32) && !sh_package_runtime_installed_bundle(other));
    }
    context = sh_mpkg_context_open(data, text, text_length, error, sizeof(error));
    if (!context) fprintf(stderr, "reopen context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 2);
    if (context && sh_mpkg_context_count(context) == 2) {
        size_t n = 0;
        const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(context, &n);
        CHECK(n == 2 && !memcmp(sh_package_bundle_fingerprint(bundles[1].original), history_tree, 32));
        CHECK(bundles[1].original == held);
        reopened = heap_copy(text, text_length); reopened_length = text_length;
        if (!sh_package_document_bind(data, context, &reopened, &reopened_length, &plan, error, sizeof(error))) {
            fprintf(stderr, "reopen bind: %s\n", error); CHECK(0);
        } else {
            CHECK(plan && count_entities(reopened) == entities);
            CHECK(sh_map_provenance_read(reopened, reopened_length, &after, &present, error, sizeof(error)) && present);
            CHECK(after.use_count == record.use_count);
            for (size_t u = 0; u < after.use_count; u++)
                CHECK(after.uses[u].binding < after.binding_count && after.bindings[after.uses[u].binding].name[0]);
            sh_package_provenance_free(&after);
        }
        sh_package_map_plan_free(plan); plan = NULL;
    }
    if (context) CHECK(sh_mpkg_context_close(&context));
    sh_package_bundle_release(held);
    if (reopened) { HeapFree(GetProcessHeap(), 0, reopened); reopened = NULL; }
    /* Negative: nobody has the referenced tree (the installed copy is removed,
     * so the boot snapshot's entry no longer holds on disk). The record as
     * saved cannot compile its composed context; degraded for a player who
     * lacks the package, the carried delivery binds alone. */
    sh_package_runtime_test_dispose();   /* the library's seals on the installed copy go first */
    for (char *c = folder; *c; c++) if (*c == '/') *c = '\\';
    CHECK(remove_unpacked_tree(folder));
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(absent));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    context = sh_mpkg_context_open(absent, text, text_length, error, sizeof(error));
    CHECK(context && sh_mpkg_context_count(context) == 1);
    CHECK(sh_mpkg_reference_missing(text, text_length, NULL, 0) == 1);
    if (context) {
        reopened = heap_copy(text, text_length); reopened_length = text_length;
        CHECK(!sh_package_document_bind(absent, context, &reopened, &reopened_length, &plan, error, sizeof(error)));
        CHECK(strstr(error, "composed map context") != NULL);
        sh_package_map_plan_free(plan); plan = NULL;
        HeapFree(GetProcessHeap(), 0, reopened);
        reopened = heap_copy(text, text_length); reopened_length = text_length;
        CHECK(sh_mpkg_degrade_absent_references(&reopened, &reopened_length, sh_package_document_stock, NULL));
        error[0] = 0;
        if (!sh_package_document_bind(absent, context, &reopened, &reopened_length, &plan, error, sizeof(error)))
            fprintf(stderr, "degraded bind: %s\n", error);
        CHECK(plan && !strstr(error, "composed map context") && count_entities(reopened) == entities);
        sh_package_map_plan_free(plan); plan = NULL;
        CHECK(sh_mpkg_context_close(&context));
    }
done:
    if (reopened) HeapFree(GetProcessHeap(), 0, reopened);
    if (plan) sh_package_map_plan_free(plan);
    if (context) sh_mpkg_context_close(&context);
    for (size_t i = 0; i < 2; i++) if (items[i].archive) HeapFree(GetProcessHeap(), 0, (void *)items[i].archive);
    sh_mpkg_used_free(used); sh_package_occurrence_release(ownership);
    sh_package_provenance_free(&selected); sh_package_provenance_free(&record);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    free(captured);
    if (native) HeapFree(GetProcessHeap(), 0, native);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    if (two) HeapFree(GetProcessHeap(), 0, two);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        char maps[MAX_PATH];
        sh_mpkg_shared_trees_release();
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data); RemoveDirectoryA(maps);
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", absent); RemoveDirectoryA(maps);
    }
    snprintf(folder, sizeof(folder), "%s/overrides/history-pack", data);
    for (char *c = folder; *c; c++) if (*c == '/') *c = '\\';
    remove_unpacked_tree(folder);
    cleanup_resources(data); cleanup_resources(absent);
}

/* The live D7-spider layout (R-379): cyberdemon and sp-pistol installed and
 * carried, boss-demons -- an outer descriptor without assets over cyberdemon,
 * hell_guard and spider_mastermind components -- referenced, the map using the
 * spider component's added encounter and the stock conductor list that
 * component shadows. Without boss-demons in the library the reopen leaves the
 * encounter entity out with its wires and rows, keeps the conductor entity
 * under its stock name, binds, and a save from that session carries no
 * reference. With boss-demons installed the reopen is complete and silent. */
static const char *const spider_stock[] = {
    "generated/decls/entitydef/ai/demon/cyberdemon.decl", "generated/decls/entitydef/weapons/pistol.decl",
    "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", "generated/decls/entitydef/ai/demon/cyberdemon_boss.decl"};
static int spider_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/worldspawn.decl") ?
        "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    for (size_t i = 0; !text && i < sizeof(spider_stock) / sizeof(*spider_stock); i++)
        if (!strcmp(path, spider_stock[i])) text = "{ class = \"Entity\"; edit = { health = 100; target = \"\"; } }";
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
#define SPIDER_MEMBER(path, text) {path, (unsigned char *)text, sizeof(text) - 1, 0}
static unsigned char *spider_archive(sh_package_archive_file *members, size_t count, size_t *length)
{
    char error[512] = "";
    sh_package_archive_files files = {members, count};
    unsigned char *bytes = sh_package_archive_write(&files, length, error, sizeof error);
    if (!bytes) fprintf(stderr, "spider archive: %s\n", error);
    CHECK(bytes);
    return bytes;
}
static const char spider_map[] =
    "{\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"string\":[]},"
    "\"entities\":[{\"entityDef\":{\"inherit\":\"ai/demon/cyberdemon\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":1},"
    "{\"entityDef\":{\"inherit\":\"weapons/pistol\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":2},"
    "{\"entityDef\":{\"inherit\":\"snapmaps/placeablesnapaiencounter/spider_mastermind\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":3},"
    "{\"entityDef\":{\"inherit\":\"ai/conductor/coop/snapmap_default\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":4}],"
    "\"instanceEntities\":{\"keyValues\":[0,4],\"values\":[1,2,3,4],\"~type\":\"idIndexMultimap\"},"
    "\"references\":{\"entityEntRefs\":{\"keyValues\":[0,0,1,2,3,4],\"values\":[3,-1,-1,-1]},"
    "\"entityVarRefs\":{\"keyValues\":[0,0,1,2,3,4],\"values\":[16777215,16777215,16777215,16777215]}},"
    "\"targets\":{\"connections\":[-2,3,-4,4,-3,4],\"entitySources\":{\"keyValues\":[],\"values\":[]},"
    "\"entityTargets\":{\"keyValues\":[],\"values\":[]},\"~type\":\"idSnapTargets\"},\"name\":\"spider\"}";

static void test_spider_layout_without_boss_demons(void)
{
    static const char outer[] = "{\"id\":\"local.boss-demons\",\"name\":\"Boss Demons\",\"author\":\"Boss Author\","
        "\"homepage\":\"https://example.test/boss-demons\",\"description\":\"Campaign bosses.\"}";
    static const char cyber_component[] = "{\"id\":\"local.cyberdemon-boss\",\"name\":\"Cyberdemon Boss\"}";
    static const char hell_component[] = "{\"id\":\"local.hell-guard\",\"name\":\"Hell Guards\"}";
    static const char spider_component[] = "{\"id\":\"local.spider-mastermind\",\"name\":\"Spider Mastermind\"}";
    static const char cyber_descriptor[] = "{\"id\":\"local.cyberdemon\",\"name\":\"Cyberdemon\"}";
    static const char pistol_descriptor[] = "{\"id\":\"local.sp-pistol\",\"name\":\"SP Pistol\"}";
    static const char cyber_edit[] = "{ class = \"Entity\"; edit = { health = 7000; } }";
    static const char pistol_edit[] = "{ class = \"Entity\"; edit = { health = 1; } }";
    static const char boss_edit[] = "{ class = \"Entity\"; edit = { health = 8000; } }";
    static const char hell_decl[] = "{ class = \"Entity\"; edit = { health = 2000; } }";
    static const char list_edit[] = "{ class = \"Entity\"; edit = { health = 3; } }";
    static const char encounter[] = "{ class = \"Entity\"; edit = { health = 9000; } }";
    char data[MAX_PATH], absent[MAX_PATH], folder[MAX_PATH], error[2048] = "", err[SH_MPKG_ERR_CAP] = "";
    sh_package_archive_file cyber_members[] = {SPIDER_MEMBER("package.json", cyber_descriptor),
        SPIDER_MEMBER("assets/generated/decls/entitydef/ai/demon/cyberdemon.decl", cyber_edit)};
    sh_package_archive_file pistol_members[] = {SPIDER_MEMBER("package.json", pistol_descriptor),
        SPIDER_MEMBER("assets/generated/decls/entitydef/weapons/pistol.decl", pistol_edit)};
    sh_package_archive_file boss_members[] = {SPIDER_MEMBER("package.json", outer),
        SPIDER_MEMBER("cyberdemon/package.json", cyber_component),
        SPIDER_MEMBER("cyberdemon/assets/generated/decls/entitydef/ai/demon/cyberdemon_boss.decl", boss_edit),
        SPIDER_MEMBER("hell_guard/package.json", hell_component),
        SPIDER_MEMBER("hell_guard/assets/generated/decls/entitydef/ai/demon/hell_guard.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/package.json", spider_component),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/snapmaps/placeablesnapaiencounter/spider_mastermind.decl", encounter),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", list_edit),
        /* The component's other definitions make its delivery the largest, so the budget references it. */
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_0.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_1.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_2.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_3.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_4.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_5.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_6.decl", hell_decl),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/ai/demon/spider_mastermind_leg_7.decl", hell_decl)};
    unsigned char *archives[3] = {0};
    size_t lengths[3] = {0};
    sh_mpkg_delivery carried[3] = {0}, items[3] = {0};
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    sh_mpkg_context *context = NULL;
    sh_package_map_plan *plan = NULL;
    char *three = NULL, *bound = NULL, *native = NULL, *captured = NULL, *saved = NULL, *text = NULL, *reopened = NULL;
    char *again = NULL, *resaved = NULL;
    size_t three_length = 0, bound_length = 0, native_length = 0, captured_length = 0, saved_length = 0, text_length = 0;
    size_t reopened_length = 0, again_length = 0, resaved_length = 0, used_count = 0, count = 0, failed = 0, nested = SIZE_MAX; /* the boss delivery */
    sh_package_provenance selected = {0}, record = {0}, after = {0};
    sh_package_occurrence_snapshot *ownership = NULL, *again_ownership = NULL;
    sh_mpkg_used *used = NULL, *again_used = NULL;
    int referenced[3] = {0}, present = 0;
    create("spider-data/overrides", NULL); create("spider-absent/overrides", NULL);
    snprintf(data, sizeof(data), "%s/spider-data", root);
    snprintf(absent, sizeof(absent), "%s/spider-absent", root);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(spider_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    archives[0] = spider_archive(cyber_members, 2, lengths);
    archives[1] = spider_archive(pistol_members, 2, lengths + 1);
    archives[2] = spider_archive(boss_members, sizeof(boss_members) / sizeof(*boss_members), lengths + 2);
    if (!archives[0] || !archives[1] || !archives[2]) goto done;
    carried[0].id = "local.cyberdemon"; carried[1].id = "local.sp-pistol"; carried[2].id = "local.boss-demons";
    for (size_t i = 0; i < 3; i++) { carried[i].archive = archives[i]; carried[i].length = lengths[i]; }
    three = sh_mpkg_embed_set(spider_map, sizeof(spider_map) - 1, carried, 3, &three_length, &failed, err, sizeof err);
    if (!three) fprintf(stderr, "spider embed: %s\n", err);
    CHECK(three); if (!three) goto done;
    /* The author's session: every delivery carried, the map bound and captured. */
    context = sh_mpkg_context_open(data, three, three_length, error, sizeof(error));
    if (!context) fprintf(stderr, "spider context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 3); if (!context) goto done;
    bound = heap_copy(three, three_length); bound_length = three_length;
    if (!sh_package_document_bind(data, context, &bound, &bound_length, &plan, error, sizeof(error))) { fprintf(stderr, "spider bind: %s\n", error); CHECK(0); goto done; }
    CHECK(sh_map_provenance_read(bound, bound_length, &selected, &present, error, sizeof(error)) && present);
    native = sh_mpkg_strip(bound, bound_length, &native_length);
    if (!native) { native = heap_copy(bound, bound_length); native_length = bound_length; }
    captured = sh_package_runtime_capture_document(plan, &selected, NULL, 0, native, native_length, &captured_length,
        &ownership, &used, &used_count, error, sizeof(error));
    if (!captured) fprintf(stderr, "spider capture: %s\n", error);
    CHECK(captured && ownership && used_count == 3); if (!captured || used_count != 3) goto done;
    /* Every entity binds to a private identity of this compile, so the uses
     * pin their contexts and boss-demons travels whole (R-379): the save puts
     * that delivery, the largest, over the text budget. */
    {
        size_t small = 0;
        for (size_t i = 0; i < 3; i++) {
            size_t n = 0;
            unsigned char *payload = sh_mpkg_pack_used(&used[i], &n, error, sizeof(error));
            CHECK(payload && !used[i].nested); items[i].id = used[i].id; items[i].archive = payload; items[i].length = n;
            if (nested == SIZE_MAX || n > items[nested].length) nested = i;
        }
        if (!items[0].archive || !items[1].archive || !items[2].archive) goto done;
        for (size_t i = 0; i < 3; i++) if (i != nested) small += items[i].length;
        CHECK(items[nested].length == lengths[2] && !memcmp(items[nested].archive, archives[2], lengths[2]));
        saved = sh_mpkg_embed_within(captured, captured_length, items, 3, captured_length + small * 4 / 3 + 2400,
            referenced, &saved_length, &count, err, sizeof err);
    }
    if (!saved) fprintf(stderr, "spider save: %s\n", err);
    CHECK(saved && referenced[nested] && !referenced[(nested + 1) % 3] && !referenced[(nested + 2) % 3]);
    if (!saved) goto done;
    /* The record travels as the save path writes it (a component delivery would have its contexts retargeted). */
    CHECK(sh_package_provenance_select_uses(sh_package_occurrence_provenance(ownership), NULL, NULL, &record));
    sh_mpkg_used_retarget_record(&record, used, used_count);
    text = sh_map_provenance_write(saved, saved_length, &record, &text_length, error, sizeof(error));
    CHECK(text && strstr(text, "smpbind.") && strstr(text, "smpref.") && count_entities(text) == 4);
    if (!text) goto done;
    sh_package_map_plan_free(plan); plan = NULL; CHECK(sh_mpkg_context_close(&context));
    /* A player without boss-demons: the notice decision (one reference the player lacks), then the degraded document. */
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(spider_baseline, NULL);
    CHECK(sh_package_runtime_refresh(absent));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_mpkg_reference_missing(text, text_length, NULL, 0) == 1);
    context = sh_mpkg_context_open(absent, text, text_length, error, sizeof(error));
    if (!context) fprintf(stderr, "absent context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 2); if (!context) goto done;
    reopened = heap_copy(text, text_length); reopened_length = text_length;
    CHECK(sh_mpkg_degrade_absent_references(&reopened, &reopened_length, sh_package_document_stock, NULL));
    /* Exactly the encounter entity left, with its wires and rows; the conductor entity stays as stock. */
    CHECK(count_entities(reopened) == 3);
    CHECK(!strstr(reopened, "spider_mastermind\",\"targetType\"") && strstr(reopened, "\"inherit\":\"ai/conductor/coop/snapmap_default\""));
    CHECK(strstr(reopened, "\"instanceEntities\":{\"keyValues\":[0,3],\"values\":[1,2,4],"));
    CHECK(strstr(reopened, "\"entityEntRefs\":{\"keyValues\":[0,0,1,2,2,3],\"values\":[-1,-1,-1]}"));
    CHECK(strstr(reopened, "\"entityVarRefs\":{\"keyValues\":[0,0,1,2,2,3],\"values\":[16777215,16777215,16777215]}"));
    CHECK(strstr(reopened, "\"connections\":[-3,4],"));
    error[0] = 0;
    if (!sh_package_document_bind(absent, context, &reopened, &reopened_length, &plan, error, sizeof(error))) { fprintf(stderr, "degraded bind: %s\n", error); CHECK(0); }
    else {
        /* Every remaining identity resolves: the two carried uses, nothing of boss-demons. */
        CHECK(plan && count_entities(reopened) == 3);
        CHECK(sh_map_provenance_read(reopened, reopened_length, &after, &present, error, sizeof(error)) && present);
        CHECK(after.use_count == 2);
        for (size_t u = 0; u < after.use_count; u++) {
            CHECK(after.uses[u].binding < after.binding_count && after.bindings[after.uses[u].binding].name[0]);
            CHECK(after.uses[u].owner && (!strcmp(after.uses[u].owner, "entity/0") || !strcmp(after.uses[u].owner, "entity/1")));
        }
        /* A save from this session drops the missing package cleanly: two deliveries, no reference. */
        native_length = 0; HeapFree(GetProcessHeap(), 0, native);
        native = sh_mpkg_strip(reopened, reopened_length, &native_length);
        if (!native) { native = heap_copy(reopened, reopened_length); native_length = reopened_length; }
        again = sh_package_runtime_capture_document(plan, &after, NULL, 0, native, native_length, &again_length,
            &again_ownership, &again_used, &count, error, sizeof(error));
        if (!again) fprintf(stderr, "degraded capture: %s\n", error);
        CHECK(again && count == 2);
        if (again && count == 2) {
            sh_mpkg_delivery two[2] = {0};
            int two_referenced[2] = {0};
            size_t n0 = 0, n1 = 0;
            unsigned char *p0 = sh_mpkg_pack_used(&again_used[0], &n0, error, sizeof(error));
            unsigned char *p1 = sh_mpkg_pack_used(&again_used[1], &n1, error, sizeof(error));
            CHECK(p0 && p1);
            two[0].id = again_used[0].id; two[0].archive = p0; two[0].length = n0;
            two[1].id = again_used[1].id; two[1].archive = p1; two[1].length = n1;
            resaved = p0 && p1 ? sh_mpkg_embed_within(again, again_length, two, 2, 0, two_referenced, &resaved_length, &count, err, sizeof err) : NULL;
            CHECK(resaved && !two_referenced[0] && !two_referenced[1] && !strstr(resaved, "smpref.") && count_entities(resaved) == 3);
            CHECK(resaved && sh_mpkg_reference_missing(resaved, resaved_length, NULL, 0) == 0);
            if (p0) HeapFree(GetProcessHeap(), 0, p0);
            if (p1) HeapFree(GetProcessHeap(), 0, p1);
        }
        sh_package_provenance_free(&after);
    }
    sh_package_map_plan_free(plan); plan = NULL;
    CHECK(sh_mpkg_context_close(&context));
    HeapFree(GetProcessHeap(), 0, reopened); reopened = NULL;
    /* With boss-demons installed the reopen is complete and silent. */
    snprintf(folder, sizeof(folder), "%s/overrides/boss-demons", data);
    CHECK(sh_package_archive_unpack(archives[2], lengths[2], folder, NULL, error, sizeof(error)));
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(spider_baseline, NULL);
    sh_mpkg_boot_capture(data);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_mpkg_reference_missing(text, text_length, NULL, 0) == 0);
    context = sh_mpkg_context_open(data, text, text_length, error, sizeof(error));
    if (!context) fprintf(stderr, "installed context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 3);
    if (context) {
        reopened = heap_copy(text, text_length); reopened_length = text_length;
        CHECK(sh_mpkg_degrade_absent_references(&reopened, &reopened_length, sh_package_document_stock, NULL) && count_entities(reopened) == 4);
        if (!sh_package_document_bind(data, context, &reopened, &reopened_length, &plan, error, sizeof(error))) { fprintf(stderr, "installed bind: %s\n", error); CHECK(0); }
        else {
            CHECK(plan && count_entities(reopened) == 4);
            CHECK(sh_map_provenance_read(reopened, reopened_length, &after, &present, error, sizeof(error)) && present && after.use_count == record.use_count);
            sh_package_provenance_free(&after);
        }
        sh_package_map_plan_free(plan); plan = NULL;
        CHECK(sh_mpkg_context_close(&context));
    }
done:
    if (reopened) HeapFree(GetProcessHeap(), 0, reopened);
    if (plan) sh_package_map_plan_free(plan);
    if (context) sh_mpkg_context_close(&context);
    for (size_t i = 0; i < 3; i++) if (items[i].archive) HeapFree(GetProcessHeap(), 0, (void *)items[i].archive);
    sh_mpkg_used_free(used); sh_package_occurrence_release(ownership);
    sh_mpkg_used_free(again_used); sh_package_occurrence_release(again_ownership);
    sh_package_provenance_free(&selected); sh_package_provenance_free(&record);
    if (resaved) HeapFree(GetProcessHeap(), 0, resaved);
    free(again);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    free(captured);
    if (native) HeapFree(GetProcessHeap(), 0, native);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    if (three) HeapFree(GetProcessHeap(), 0, three);
    for (size_t i = 0; i < 3; i++) free(archives[i]);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        char maps[MAX_PATH];
        sh_mpkg_shared_trees_release();
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data); RemoveDirectoryA(maps);
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", absent); RemoveDirectoryA(maps);
    }
    snprintf(folder, sizeof(folder), "%s/overrides/boss-demons", data);
    for (char *c = folder; *c; c++) if (*c == '/') *c = '\\';
    remove_unpacked_tree(folder);
    cleanup_resources(data); cleanup_resources(absent);
}

/* ---- One saved use across renderers (R-392) ----
 * The two game images serve one package source under different private names
 * (a compiled identity includes renderer-specific output; live: Vulkan saved
 * the spider encounter as smp/aec97eac.. and reopened it as smp/433a94f3..,
 * OpenGL rebinds the same record to smp/cbc44127..). A saved name is only the
 * saving image's spelling: the binding record carries the source itself (the
 * family, the authored original and the use's exact contexts), and a reopen
 * resolves it through the opening image's compile. The other image's save is
 * modelled exactly by its spelling: the same text and record with each saved
 * name of the record replaced, which is all a second image changes. */
static int xr_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/worldspawn.decl") ?
        "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }" :
        !strcmp(path, "generated/decls/entitydef/snapmaps/placeablesnapaiencounter/spider_mastermind.decl") ?
            "{ class = \"Entity\"; edit = { health = 100; target = \"\"; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* The installed library's spelling of the resource whose body states value. */
static const char *xr_library_name(const char *value)
{
    static char name[256];
    const sh_package_compilation *compiled = sh_package_runtime_library_acquire();
    name[0] = 0;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->body && strstr((const char *)r->body, value) && r->native_name)
            snprintf(name, sizeof(name), "%s", r->native_name);
    }
    sh_package_runtime_release();
    return name;
}
/* Another spelling of a private name of the same length. */
static void xr_other_spelling(const char *name, char other[69])
{
    snprintf(other, 69, "%s", name);
    memcpy(other + 60, other[60] == '0' ? "11111111" : "00000000", 8);
}
/* The text and record another image saved: the quoted value at the
 * occurrence-th quoted from (every one when occurrence < 0) and the record's
 * use of that site spell to. With every value, each binding named from is
 * renamed; with one, that site's use moves to a second binding of the same
 * source under to (a mixed record). Heap text in, heap text out. */
static char *xr_foreign(const char *text, size_t length, const char *from, const char *to,
    int occurrence, const char *site_owner, size_t *out_length)
{
    char error[512] = "", quoted[80], *copy, *out = NULL;
    sh_package_provenance record = {0};
    int present = 0, seen = 0, ok = 1;
    *out_length = 0;
    if (strlen(from) != strlen(to) || snprintf(quoted, sizeof(quoted), "\"%s\"", from) >= (int)sizeof(quoted)) return NULL;
    if (!(copy = heap_copy(text, length))) return NULL;
    for (char *at = copy; (at = strstr(at, quoted)) != NULL; at += strlen(quoted), seen++)
        if (occurrence < 0 || seen == occurrence) memcpy(at + 1, to, strlen(to));
    if (sh_map_provenance_read(copy, length, &record, &present, error, sizeof(error)) && present) {
        if (occurrence < 0) {
            for (size_t b = 0; b < record.binding_count; b++)
                if (!strcmp(record.bindings[b].name, from)) memcpy(record.bindings[b].name, to, strlen(to));
        } else {
            ok = 0;
            for (size_t u = 0; u < record.use_count && !ok; u++) {
                sh_package_provenance_use *use = record.uses + u;
                if (strcmp(use->owner, site_owner) || strcmp(record.bindings[use->binding].name, from)) continue;
                const sh_package_provenance_binding *source = record.bindings + use->binding;
                sh_package_provenance_site site = {use->kind, use->owner, use->reference};
                char owner[64], reference[256];
                size_t *contexts = malloc(use->context_count * sizeof(*contexts)), count = use->context_count;
                if (!contexts) break;
                memcpy(contexts, use->contexts, count * sizeof(*contexts));
                snprintf(owner, sizeof(owner), "%s", use->owner); snprintf(reference, sizeof(reference), "%s", use->reference);
                site.owner = owner; site.reference = reference;
                char family[64], original[256];
                snprintf(family, sizeof(family), "%s", source->family); snprintf(original, sizeof(original), "%s", source->original);
                size_t binding;
                ok = sh_package_provenance_bind(&record, family, original, to, contexts, count);
                for (binding = 0; ok && binding < record.binding_count; binding++)
                    if (!strcmp(record.bindings[binding].name, to)) break;
                ok = ok && binding < record.binding_count &&
                    sh_package_provenance_use_set(&record, &site, binding, contexts, count);
                free(contexts);
            }
        }
        if (ok) out = sh_map_provenance_write(copy, length, &record, out_length, error, sizeof(error));
    }
    if (!out) fprintf(stderr, "foreign record: %s\n", error[0] ? error : "the site's use was not found");
    sh_package_provenance_free(&record); HeapFree(GetProcessHeap(), 0, copy);
    return out;
}
static int xr_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{ (void)context; (void)restoring; (void)changes; (void)error; (void)capacity; return 1; }

/* The live slot's route (L12, AA1ECBAE): boss-demons installed, its spider
 * component's encounter placed (from the library, or bound by a carried
 * delivery), the map saved with boss-demons referenced (L2), then reopened
 * where the same installed tree serves the encounter under another spelling:
 * the reference lays that tree out in the map's context, the record rebinds
 * by source, the plan activates and the parsed document's ownership attaches
 * (rawmap.c attach_loaded_occurrences). mixed keeps one of two placed copies
 * under the opening image's own spelling, as a copy pasted after a reopen. */
static void test_cross_image_reference(int from_library, int mixed)
{
    static const char outer[] = "{\"id\":\"local.boss-demons\",\"name\":\"Boss Demons\",\"author\":\"Boss Author\"}";
    static const char spider_component[] = "{\"id\":\"local.spider-mastermind\",\"name\":\"Spider Mastermind\"}";
    static const char encounter[] = "{ class = \"Entity\"; edit = { health = 9000; target = \"\"; } }";
    sh_package_archive_file members[] = {SPIDER_MEMBER("package.json", outer),
        SPIDER_MEMBER("spider_mastermind/package.json", spider_component),
        SPIDER_MEMBER("spider_mastermind/assets/generated/decls/entitydef/snapmaps/placeablesnapaiencounter/spider_mastermind.decl", encounter)};
    char data[MAX_PATH], folder[MAX_PATH], error[2048] = "", err[SH_MPKG_ERR_CAP] = "", map[1024];
    char saved_name[69] = "", other[69] = "";
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    unsigned char *archive = NULL; size_t archive_length = 0;
    sh_package_occurrence_snapshot *ownership = NULL, *attached = NULL;
    sh_mpkg_used *used = NULL;
    size_t used_count = 0, captured_length = 0, saved_length = 0, text_length = 0, count = 0, reopened_length = 0;
    size_t native_length = 0, attach_length = 0, carrier_length = 0, bound_length = 0, foreign_length = 0;
    char *captured = NULL, *saved = NULL, *text = NULL, *reopened = NULL, *native = NULL, *attach = NULL;
    char *carrier = NULL, *bound = NULL, *foreign = NULL;
    sh_package_provenance record = {0}, after = {0}, selected = {0};
    sh_mpkg_context *context = NULL;
    sh_package_map_plan *plan = NULL;
    sh_mpkg_delivery item = {0};
    int referenced = 0, present = 0;
    const char *entities = mixed ?
        "{\"entityDef\":{\"inherit\":\"%s\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":1},"
        "{\"entityDef\":{\"inherit\":\"%s\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":2}" :
        "{\"entityDef\":{\"inherit\":\"%s\",\"targetType\":\"idDeclEntityDef\"},\"uniqueId\":1}";
    create("xr-data/overrides", NULL);
    snprintf(data, sizeof(data), "%s/xr-data", root);
    archive = spider_archive(members, sizeof(members) / sizeof(*members), &archive_length);
    if (!archive) goto done;
    snprintf(folder, sizeof(folder), "%s/overrides/boss-demons", data);
    CHECK(sh_package_archive_unpack(archive, archive_length, folder, NULL, error, sizeof(error)));
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(xr_baseline, NULL);
    sh_package_runtime_test_installed_links(from_library);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_mpkg_boot_capture(data);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    {
        char body[640], value[96];
        snprintf(value, sizeof(value), "%s", from_library ? xr_library_name("health = 9000") :
            "snapmaps/placeablesnapaiencounter/spider_mastermind");
        if (from_library) CHECK(!strncmp(value, "smp/", 4));
        snprintf(body, sizeof(body), entities, value, value);
        snprintf(map, sizeof(map), "{\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"string\":[]},"
            "\"entities\":[%s],\"instanceEntities\":{\"keyValues\":[0,%d],\"values\":[%s],\"~type\":\"idIndexMultimap\"},"
            "\"name\":\"xr\"}", body, mixed ? 2 : 1, mixed ? "1,2" : "1");
    }
    if (from_library) {
        /* The author places the encounter from the installed library and
         * saves; the capture records the library's own source context. */
        captured = sh_package_runtime_capture_document(NULL, NULL, NULL, 0, map, strlen(map), &captured_length,
            &ownership, &used, &used_count, error, sizeof(error));
    } else {
        /* The author's session binds the encounter through a carried delivery. */
        carrier = sh_mpkg_embed(map, strlen(map), "local.boss-demons", archive, archive_length, &carrier_length, err, sizeof err);
        CHECK(carrier); if (!carrier) goto done;
        context = sh_mpkg_context_open(data, carrier, carrier_length, error, sizeof(error));
        CHECK(context && sh_mpkg_context_count(context) == 1); if (!context) goto done;
        bound = heap_copy(carrier, carrier_length); bound_length = carrier_length;
        if (!sh_package_document_bind(data, context, &bound, &bound_length, &plan, error, sizeof(error))) {
            fprintf(stderr, "cross-image author bind: %s\n", error); CHECK(0); goto done;
        }
        CHECK(sh_map_provenance_read(bound, bound_length, &selected, &present, error, sizeof(error)) && present);
        native = sh_mpkg_strip(bound, bound_length, &native_length);
        if (!native) { native = heap_copy(bound, bound_length); native_length = bound_length; }
        captured = sh_package_runtime_capture_document(plan, &selected, NULL, 0, native, native_length, &captured_length,
            &ownership, &used, &used_count, error, sizeof(error));
        HeapFree(GetProcessHeap(), 0, native); native = NULL;
        sh_package_map_plan_free(plan); plan = NULL; CHECK(sh_mpkg_context_close(&context));
    }
    if (!captured) fprintf(stderr, "cross-image capture: %s\n", error);
    CHECK(captured && used_count == 1); if (!captured || used_count != 1) goto done;
    /* The save: boss-demons over the budget travels as a reference. */
    item.id = used[0].id;
    item.archive = sh_mpkg_pack_used(&used[0], &item.length, error, sizeof(error));
    CHECK(item.archive); if (!item.archive) goto done;
    saved = sh_mpkg_embed_within(captured, captured_length, &item, 1, captured_length + 16, &referenced, &saved_length, &count, err, sizeof err);
    CHECK(saved && referenced); if (!saved) goto done;
    CHECK(sh_package_provenance_select_uses(sh_package_occurrence_provenance(ownership), NULL, NULL, &record));
    sh_mpkg_used_retarget_record(&record, used, used_count);
    CHECK(record.binding_count == 1 && record.use_count == (mixed ? 2u : 1u) && record.context_count == 1);
    if (record.binding_count != 1 || !record.context_count) goto done;
    /* The library authors in an isolated component context; a record-less
     * carrier composes its delivery. Either is the source the reopen uses. */
    CHECK(from_library ? record.contexts[0].mode == SH_PACKAGE_CONTEXT_ISOLATED &&
        record.contexts[0].component && !strcmp(record.contexts[0].component, "spider_mastermind") :
        record.contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED);
    text = sh_map_provenance_write(saved, saved_length, &record, &text_length, error, sizeof(error));
    CHECK(text && strstr(text, "smpbind.") && strstr(text, "smpref.")); if (!text) goto done;
    sh_package_occurrence_release(ownership); ownership = NULL;
    /* The same save made by the other image: its own spelling of every saved
     * use, or of one copy only. */
    snprintf(saved_name, sizeof(saved_name), "%s", record.bindings[0].name);
    CHECK(!strncmp(saved_name, "smp/", 4) && strlen(saved_name) == 68);
    xr_other_spelling(saved_name, other);
    foreign = xr_foreign(text, text_length, saved_name, other, mixed ? 1 : -1, "entity/1", &foreign_length);
    CHECK(foreign); if (!foreign) goto done;
    CHECK(strstr(foreign, other) && (mixed ? strstr(foreign, saved_name) != NULL : !strstr(foreign, saved_name)));
    sh_package_provenance_free(&record);
    CHECK(sh_map_provenance_read(foreign, foreign_length, &record, &present, error, sizeof(error)) && present);
    CHECK(record.binding_count == (mixed ? 2u : 1u));
    /* The opening image: the same installed package, a fresh runtime. */
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(xr_baseline, NULL);
    sh_package_runtime_test_installed_links(from_library);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_mpkg_boot_capture(data);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(sh_mpkg_reference_missing(foreign, foreign_length, NULL, 0) == 0);
    context = sh_mpkg_context_open(data, foreign, foreign_length, error, sizeof(error));
    if (!context) fprintf(stderr, "cross-image context: %s\n", error);
    CHECK(context && sh_mpkg_context_count(context) == 1); if (!context) goto done;
    reopened = heap_copy(foreign, foreign_length); reopened_length = foreign_length;
    if (!sh_package_document_bind(data, context, &reopened, &reopened_length, &plan, error, sizeof(error))) {
        fprintf(stderr, "cross-image bind: %s\n", error); CHECK(0); goto done;
    }
    /* Every use now names the opening image's own spelling of its source. */
    CHECK(sh_map_provenance_read(reopened, reopened_length, &after, &present, error, sizeof(error)) && present);
    CHECK(after.binding_count == 1 && after.use_count == (mixed ? 2u : 1u));
    CHECK(after.binding_count && !strcmp(after.bindings[0].original, "snapmaps/placeablesnapaiencounter/spider_mastermind") &&
        !strcmp(after.bindings[0].name, saved_name) && !strstr(reopened, other));
    /* The native load: the plan activates, then the parsed document's ownership attaches. */
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, xr_activate, NULL));
    native = sh_mpkg_strip(reopened, reopened_length, &native_length);
    if (!native) { native = heap_copy(reopened, reopened_length); native_length = reopened_length; }
    attach = sh_package_runtime_capture_document(NULL, &after, NULL, 0, native, native_length, &attach_length,
        &attached, NULL, NULL, error, sizeof(error));
    if (!attach) fprintf(stderr, "cross-image attach: %s\n", error);
    CHECK(attach && attached);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, xr_activate, NULL));
done:
    sh_package_runtime_test_installed_links(0);
    free(attach); sh_package_occurrence_release(attached);
    if (native) HeapFree(GetProcessHeap(), 0, native);
    if (reopened) HeapFree(GetProcessHeap(), 0, reopened);
    if (plan) sh_package_map_plan_free(plan);
    if (context) sh_mpkg_context_close(&context);
    if (item.archive) HeapFree(GetProcessHeap(), 0, (void *)item.archive);
    sh_mpkg_used_free(used); sh_package_occurrence_release(ownership);
    sh_package_provenance_free(&record); sh_package_provenance_free(&after); sh_package_provenance_free(&selected);
    if (foreign) HeapFree(GetProcessHeap(), 0, foreign);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    if (carrier) HeapFree(GetProcessHeap(), 0, carrier);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    free(captured); free(archive);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        char maps[MAX_PATH];
        sh_mpkg_shared_trees_release();
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data); RemoveDirectoryA(maps);
    }
    for (char *c = folder; *c; c++) if (*c == '/') *c = '\\';
    remove_unpacked_tree(folder);
    cleanup_resources(data);
}

/* A map's compiled context outlives its plan: the plan's release keeps the provider for the same map to open
 * again beside the same library, so that reopen compiles nothing and takes the kept provider. Another text
 * (one byte of the note) is another map and compiles; a library change (a package installed) releases the
 * kept compile at the refresh. */
static void test_departed_plan_reuse(const document_fixture *fixture)
{
    char data[MAX_PATH], *edited = NULL, *at;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    document_pass pass = {0};
    long compiles, reuses;
    create("reuse-data/overrides", NULL);
    snprintf(data, sizeof(data), "%s/reuse-data", root);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    reuses = sh_package_runtime_test_map_reuses();
    CHECK(!sh_package_runtime_test_departed_map());
    /* The first open compiles the map's context; its departure keeps the compile. */
    CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
    compiles = sh_package_runtime_test_compiles();
    CHECK(!sh_package_runtime_test_departed_map());
    pass_free(&pass);
    CHECK(sh_package_runtime_test_departed_map());
    /* The same text again: the kept compile serves the plan while it is open and is kept again after. The
     * context's folder is hard links to the process's one copy of the carried tree, not a second unpack. */
    CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
    CHECK(sh_package_runtime_test_compiles() == compiles && sh_package_runtime_test_map_reuses() == reuses + 1);
    CHECK(!sh_package_runtime_test_departed_map());
    {
        char linked[MAX_PATH];
        BY_HANDLE_FILE_INFORMATION info = {0};
        HANDLE file;
        snprintf(linked, sizeof(linked), "%s\\overrides\\0\\package.json", sh_mpkg_context_root(pass.context));
        file = CreateFileA(linked, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &info) && info.nNumberOfLinks >= 2);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    pass_free(&pass);
    CHECK(sh_package_runtime_test_departed_map());
    /* Another text: a compile, which replaces the kept one. */
    edited = malloc(fixture->length + 1); CHECK(edited);
    if (!edited) goto done;
    memcpy(edited, fixture->map, fixture->length); edited[fixture->length] = 0;
    at = strstr(edited, "\"note\":\"history/root"); CHECK(at);
    if (at) at[strlen("\"note\":\"histo")] = 'R';
    CHECK(pass_run(&pass, data, edited, fixture->length, fixture->carrier));
    CHECK(sh_package_runtime_test_compiles() == compiles + 1 && sh_package_runtime_test_map_reuses() == reuses + 1);
    pass_free(&pass);
    CHECK(sh_package_runtime_test_departed_map());
    /* The library changes: the kept compile goes with the refresh, and the next open compiles. */
    create("reuse-data/overrides/extra/package.json", "{\"id\":\"reuse-extra\",\"name\":\"Extra\"}");
    create("reuse-data/overrides/extra/assets/generated/decls/entitydef/reuse/extra.decl", "{ class = \"Entity\"; edit = { health = 3; } }");
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    CHECK(!sh_package_runtime_test_departed_map());
    compiles = sh_package_runtime_test_compiles();
    CHECK(pass_run(&pass, data, edited, fixture->length, fixture->carrier));
    CHECK(sh_package_runtime_test_compiles() == compiles + 1 && sh_package_runtime_test_map_reuses() == reuses + 1);
    pass_free(&pass);
    CHECK(sh_package_runtime_test_departed_map());
done:
    free(edited);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    CHECK(!sh_package_runtime_test_departed_map());
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        /* Closed contexts leave only their now-empty private maps directory. */
        char maps[MAX_PATH];
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data);
        sh_mpkg_shared_trees_release();
        CHECK(RemoveDirectoryA(maps));
    }
    cleanup_resources(data);
}

/* R-398: a map context compiled in one process serves the next process's open of the same map: the record its
 * committed activation wrote is loaded instead of compiled, and the publication is the compiled one. Another text
 * compiles afresh and joins the list, and the first text still loads in a later process. */
static void test_map_context_records(const document_fixture *fixture)
{
    char data[MAX_PATH], last[700], *edited = NULL, *at;
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    document_pass pass = {0};
    long compiles, hits, misses, writes, h, m, w;
    create("records-data/overrides", NULL);
    snprintf(data, sizeof(data), "%s/records-data", root);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_warm(1); sh_package_runtime_test_map_records(1); sh_package_runtime_test_image_slot("vk");
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_map_record_counts(&hits, &misses, &writes, NULL, 0);
    /* The first process compiles the context and, once its activation committed, writes its record. */
    compiles = sh_package_runtime_test_compiles();
    CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
    CHECK(sh_package_runtime_activate_prepared_map(pass.plan, (sh_package_activation_guard){0}, NULL, NULL));
    sh_package_runtime_test_map_record_counts(&h, &m, &w, last, sizeof(last));
    if (w != writes + 1) fprintf(stderr, "map context record (first open): %s\n", last);
    CHECK(sh_package_runtime_test_compiles() > compiles && h == hits && m == misses + 1 && w == writes + 1);
    CHECK(strstr(last, "map context record written") != NULL);
    char *cold = sh_package_runtime_test_publication_digest();
    pass_free(&pass);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, NULL, NULL));
    /* A later process: the same open loads the record and compiles nothing. */
    /* A later process starts as the first did: no source readers until its library is published. */
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    compiles = sh_package_runtime_test_compiles();
    CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
    sh_package_runtime_test_map_record_counts(&hits, &misses, &writes, last, sizeof(last));
    if (hits != h + 1) fprintf(stderr, "map context record (second process): %s\n", last);
    CHECK(sh_package_runtime_test_compiles() == compiles && hits == h + 1 && misses == m);
    CHECK(strstr(last, "map context record used") != NULL);
    CHECK(sh_package_runtime_activate_prepared_map(pass.plan, (sh_package_activation_guard){0}, NULL, NULL));
    {
        char *warm = sh_package_runtime_test_publication_digest();
        CHECK(cold && warm && !strcmp(cold, warm));
        free(warm);
    }
    sh_package_runtime_test_map_record_counts(NULL, NULL, &w, NULL, 0);
    CHECK(w == writes); /* a loaded context writes nothing */
    pass_free(&pass);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, NULL, NULL));
    /* Another text is another context: it compiles and joins the list. */
    edited = malloc(fixture->length + 1); CHECK(edited);
    if (!edited) goto done;
    memcpy(edited, fixture->map, fixture->length); edited[fixture->length] = 0;
    at = strstr(edited, "\"note\":\"history/root"); CHECK(at);
    if (at) at[strlen("\"note\":\"histo")] = 'R';
    /* A later process starts as the first did: no source readers until its library is published. */
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    compiles = sh_package_runtime_test_compiles();
    CHECK(pass_run(&pass, data, edited, fixture->length, fixture->carrier));
    CHECK(sh_package_runtime_activate_prepared_map(pass.plan, (sh_package_activation_guard){0}, NULL, NULL));
    sh_package_runtime_test_map_record_counts(&h, &m, &w, last, sizeof(last));
    CHECK(sh_package_runtime_test_compiles() > compiles && m == misses + 1 && w == writes + 1);
    pass_free(&pass);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, NULL, NULL));
    /* Both are listed: the first text loads again in a later process. */
    /* A later process starts as the first did: no source readers until its library is published. */
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data));
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    compiles = sh_package_runtime_test_compiles();
    CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
    sh_package_runtime_test_map_record_counts(&hits, NULL, NULL, last, sizeof(last));
    CHECK(sh_package_runtime_test_compiles() == compiles && hits == h + 1);
    pass_free(&pass);
done:
    free(edited); free(cold);
    sh_package_runtime_test_map_records(-1); sh_package_runtime_test_warm(-1);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        char maps[MAX_PATH];
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data);
        sh_mpkg_shared_trees_release();
        RemoveDirectoryA(maps);
    }
    cleanup_resources(data);
}

/* R-398 perf16: the open's bind (the map context compile, the rebind and the native map data) run on the
 * preparation worker gives the plan the engine thread's bind gives: the same served publication. */
typedef struct worker_pass { document_pass *pass; const char *data; const document_fixture *fixture; int ok; } worker_pass;
static void worker_pass_run(void *context)
{
    worker_pass *w = context;
    w->ok = pass_run(w->pass, w->data, w->fixture->map, w->fixture->length, w->fixture->carrier);
}
static void test_bind_on_the_preparation_worker(const document_fixture *fixture)
{
    char data[MAX_PATH];
    admission_fixture f; admission_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    document_pass pass = {0};
    sh_prepare_stats stats;
    char *engine = NULL, *worker = NULL;
    create("worker-data/overrides", NULL);
    snprintf(data, sizeof(data), "%s/worker-data", root);
    for (int on_worker = 0; on_worker < 2; on_worker++) {
        sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
        sh_package_runtime_test_dispose();
        sh_package_runtime_test_baseline(historical_baseline, NULL);
        CHECK(sh_package_runtime_refresh(data));
        sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
        if (on_worker) {
            worker_pass w = {&pass, data, fixture, 0};
            CHECK(sh_prepare_start(worker_pass_run, &w));
            while (!sh_prepare_finished()) sh_prepare_service(5);
            CHECK(sh_prepare_join(&stats) == 1 && !stats.refused[0] && w.ok);
        } else CHECK(pass_run(&pass, data, fixture->map, fixture->length, fixture->carrier));
        CHECK(sh_package_runtime_activate_prepared_map(pass.plan, (sh_package_activation_guard){0}, NULL, NULL));
        if (on_worker) worker = sh_package_runtime_test_publication_digest();
        else engine = sh_package_runtime_test_publication_digest();
        pass_free(&pass);
        CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, NULL, NULL));
    }
    CHECK(engine && worker && !strcmp(engine, worker));
    free(engine); free(worker);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    {
        char maps[MAX_PATH];
        snprintf(maps, sizeof(maps), "%s/package-cache/maps", data);
        sh_mpkg_shared_trees_release();
        RemoveDirectoryA(maps);
    }
    cleanup_resources(data);
}

int main(void)
{
    char temporary[MAX_PATH], deep[MAX_PATH];
    size_t cap, base;
    const document_fixture history = {hist_map_history, hist_map_history_len, HIST_HISTORY_ID,
        hist_history_payload, hist_history_payload_len, HIST_HISTORY_ID, "\"note\":\"history/root\x97\""};
    /* W3: the beta14/15 writers' descriptor-keyed modern carrier over their
     * UTF-8 base, re-saved under its complete-tree key without migration. */
    const document_fixture w3 = {hist_map_w3, hist_map_w3_len, HIST_W3_ID, hist_w3_payload, hist_w3_payload_len, NULL,
        "\"note\":\"history/root\xe2\x80\x94\""};
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pdt", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    test_document_chain("document-data", &history);
    test_document_chain("document-w3", &w3);
    test_referenced_tree_from_library();
    test_spider_layout_without_boss_demons();
    test_departed_plan_reuse(&w3);
    test_map_context_records(&w3);
    test_bind_on_the_preparation_worker(&w3);
    test_cross_image_reference(0, 0);
    test_cross_image_reference(1, 0);
    test_cross_image_reference(1, 1);
    test_cross_image_reference(0, 1);
    /* The same chain under the deepest data directory where the private
     * inventory directory can be created. */
    cap = inventory_cap(); base = strlen(root);
    CHECK(cap > base + 2 && cap - base - 1 < sizeof(deep));
    if (cap > base + 2 && cap - base - 1 < sizeof(deep)) {
        memset(deep, 'd', cap - base - 1); deep[cap - base - 1] = 0;
        printf("document chain at data root depth %zu\n", cap);
        test_document_chain(deep, &history);
    }
    cleanup();
    if (failures) { fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    puts("package_document_test: all checks passed");
    return 0;
}
