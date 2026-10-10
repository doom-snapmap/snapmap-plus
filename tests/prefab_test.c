/* Complete original deliveries and per-site recipes survive portable prefabs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prefab.h"
#include "map_package.h"
#include "map_provenance.h"
#include "package_runtime.h"
#include "engine_dialog.h"
#include "package_fixture.h"

/* Pure carrier operations must neither ask for installation nor select policy. */
const sh_package_compilation *sh_package_runtime_acquire(void) { CHECK(0); return NULL; }
const sh_package_compilation *sh_package_runtime_library_acquire(void) { CHECK(0); return NULL; }
void sh_package_runtime_release(void) { CHECK(0); }
int sh_package_runtime_ready(void) { CHECK(0); return 0; }
int sh_package_runtime_admission_ready(void) { CHECK(0); return 0; }
int sh_package_runtime_serving_disabled(void) { return 0; }
void sh_package_runtime_note_sources_changed(void) { CHECK(0); }
void sh_package_runtime_note_install_published(void) { CHECK(0); }
int sh_decl_server_registration_succeeded(void) { CHECK(0); return 0; }
void sh_decl_server_request_rearm(void) { CHECK(0); }
int sh_engine_dialog_ready(void) { CHECK(0); return 0; }
int sh_engine_dialog_can_ask(void) { CHECK(0); return 0; }
int sh_engine_dialog_ask(unsigned id, unsigned buttons, const char *text)
{ (void)id; (void)buttons; (void)text; CHECK(0); return 0; }
int sh_engine_dialog_poll(int ticket) { (void)ticket; CHECK(0); return SH_ENGINE_DIALOG_PENDING; }
void sh_engine_dialog_release(int ticket) { (void)ticket; CHECK(0); }
void backend_log(const char *text) { (void)text; }

static const char native[] =
    " {\"cameraYaw\":125.50,\"entities\":["
    "{\"uniqueId\":7,\"entityDef\":{\"inherit\":\"private/same\"}},"
    "{\"uniqueId\":7,\"entityDef\":{\"inherit\":\"private/same\"}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[{"
    "\"info\":{\"name\":\"author\",\"~type\":\"snapVarInfo_t\"},"
    "\"initialValue\":\"literal \\u03bb and byte \x97; smpkg.fake is prose\",\"~type\":\"snapVarString_t\"}]},"
    "\"~type\":\"idSnapEntityPrefab\"}\r\n";

static sh_package_bundle *bundle(const char *label, unsigned char **expected, size_t *length)
{
    char descriptor[200], error[512];
    snprintf(descriptor, sizeof(descriptor), " {\"id\":\"same\",\"name\":\"%s\"} \r\n", label);
    sh_package_archive_file files[] = {
        {"package.json", (unsigned char *)descriptor, strlen(descriptor), 0},
        {"notes/exact.txt", (unsigned char *)label, strlen(label), 0},
        {"assets/unused.bin", (unsigned char *)"unused\xff\0payload", 15, 0},
        {"empty", NULL, 0, 1}
    };
    sh_package_archive_files archive = {files, 4};
    *expected = sh_package_archive_write(&archive, length, error, sizeof(error)); CHECK(*expected);
    return *expected ? sh_package_bundle_from_archive(*expected, *length, root, error, sizeof(error)) : NULL;
}
static void add_use(sh_package_provenance *uses, const sh_package_bundle_binding *row,
    const char *owner, unsigned mode, const unsigned char (*trees)[32], size_t tree_count)
{
    size_t context = sh_package_provenance_context_add(uses, mode,
        trees ? trees : (const unsigned char (*)[32])sh_package_bundle_fingerprint(row->original),
        trees ? tree_count : 1, mode == SH_PACKAGE_CONTEXT_COMPOSED ? NULL : "");
    CHECK(context != SIZE_MAX);
    CHECK(sh_package_provenance_bind(uses, "entitydef", "authored/root", "private/same", &context, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, owner, "entityDef/inherit"};
    CHECK(sh_package_provenance_use_set(uses, &site, 0, &context, 1));
}
static const char exact_row[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static void exact_carriers(void)
{
    char error[512], key[SH_PACKAGE_BUNDLE_KEY_CAP];
    sh_package_bundle_binding rows[3] = {0};
    sh_package_provenance uses = {0};
    sh_package_occurrence_snapshot *document, *empty;
    sh_prefab_carrier read = {0};
    unsigned char *expected[3] = {0}; size_t lengths[3] = {0}, length = 0, again_length = 0;
    char *encoded = NULL, *again = NULL, *plain = NULL;
    const char *labels[] = {"chosen A", "chosen B", "unrelated same-ID"};
    for (size_t i = 0; i < 3; i++) {
        rows[i].original = bundle(labels[i], expected+i, lengths+i); CHECK(rows[i].original);
        strcpy_s(rows[i].source_name, sizeof(rows[i].source_name), labels[i]);
        memset(rows[i].compiler_tree, (int)i+1, 32);
    }
    uses.semantics = SH_PACKAGE_PROVENANCE_USES;
    add_use(&uses, rows, "entity/0", SH_PACKAGE_CONTEXT_ISOLATED, NULL, 0);
    add_use(&uses, rows+1, "entity/1", SH_PACKAGE_CONTEXT_ISOLATED, NULL, 0);
    document = sh_package_occurrence_prepare_document(&uses, rows, 3, error, sizeof(error)); CHECK(document);
    empty = sh_package_occurrence_prepare_document(NULL, NULL, 0, error, sizeof(error)); CHECK(empty);
    if (!document || !empty) goto done;
    /* An exact editor row reaches the prepared document the way a save carries
     * it, then rides the carrier on its own owner only. */
    CHECK(sh_package_provenance_row_set(&uses, "entity/0", exact_row) == 1);
    CHECK(sh_package_occurrence_carry_rows(document, &uses));
    /* No editable source or caller archive handle survives this point. */
    for (size_t i = 0; i < 3; i++) { sh_package_bundle_release(rows[i].original); rows[i].original = NULL; }
    encoded = sh_prefab_carrier_write(native, sizeof(native)-1, document, &length, error, sizeof(error)); CHECK(encoded);
    if (!encoded) goto done;
    sh_mpkg_decl packages[3];
    CHECK(sh_mpkg_scan(encoded, length, packages, 3) == 2);
    CHECK(sh_prefab_carrier_read(encoded, length, &read, error, sizeof(error)));
    CHECK(read.bindings_present && read.deliveries_present && read.bindings.use_count == 2);
    CHECK(read.native_length == sizeof(native)-1 && !memcmp(read.native_json, native, sizeof(native)));
    for (size_t i = 0; i < 2; i++) {
        unsigned char tree[32]; size_t n = 0;
        CHECK(sh_package_archive_tree_identity(expected[i], lengths[i], tree, error, sizeof(error)));
        snprintf(key, sizeof(key), "delivery.");
        for (size_t j = 0; j < 32; j++) snprintf(key + 9 + j*2, sizeof(key)-9-j*2, "%02x", tree[j]);
        unsigned char *payload = sh_mpkg_extract(encoded, length, key, &n, error, sizeof(error));
        CHECK(payload && n == lengths[i] && !memcmp(payload, expected[i], n));
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
    }
    CHECK(read.bindings.context_count == 2 && read.bindings.binding_count == 1);
    CHECK(read.bindings.uses[0].contexts[0] != read.bindings.uses[1].contexts[0]);
    const char *carried_row = NULL;
    CHECK(sh_package_provenance_row_get(&read.bindings, "entity/0", &carried_row) == 1 &&
        !strcmp(carried_row, exact_row));
    CHECK(sh_package_provenance_row_get(&read.bindings, "entity/1", &carried_row) == 0);
    again = sh_prefab_carrier_write(encoded, length, document, &again_length, error, sizeof(error));
    CHECK(again && again_length == length && !memcmp(again, encoded, length));
    plain = sh_prefab_carrier_write(encoded, length, empty, &again_length, error, sizeof(error));
    CHECK(plain && again_length == sizeof(native)-1 && !memcmp(plain, native, sizeof(native)));
    /* Existing exact document is unchanged after malformed transport. */
    char *before = read.native_json;
    char *corrupt = _strdup(encoded); CHECK(corrupt);
    if (corrupt) {
        char *header = strstr(corrupt, "smpbind."); CHECK(header); if (header) header[8] = 'x';
        CHECK(!sh_prefab_carrier_read(corrupt, length, &read, error, sizeof(error)));
        CHECK(read.native_json == before && read.bindings.use_count == 2); free(corrupt);
        CHECK(sh_package_provenance_row_get(&read.bindings, "entity/0", &carried_row) == 1 &&
            !strcmp(carried_row, exact_row));
    }
done:
    sh_package_occurrence_release(document); sh_package_occurrence_release(empty);
    sh_package_provenance_free(&uses); sh_prefab_carrier_free(&read);
    if (encoded) HeapFree(GetProcessHeap(), 0, encoded);
    if (again) HeapFree(GetProcessHeap(), 0, again);
    if (plain) HeapFree(GetProcessHeap(), 0, plain);
    for (size_t i = 0; i < 3; i++) { sh_package_bundle_release(rows[i].original); free(expected[i]); }
}

static void historical_and_vanilla(void)
{
    char error[512]; size_t length = 0, n = 0;
    sh_prefab_carrier read = {0};
    sh_package_occurrence_snapshot *empty = sh_package_occurrence_prepare_document(NULL, NULL, 0, error, sizeof(error));
    char *copy = sh_prefab_carrier_write(native, sizeof(native)-1, empty, &length, error, sizeof(error));
    CHECK(copy && length == sizeof(native)-1 && !memcmp(copy, native, sizeof(native)));
    CHECK(sh_prefab_carrier_read(native, sizeof(native)-1, &read, error, sizeof(error)));
    CHECK(!read.bindings_present && !read.deliveries_present && !memcmp(read.native_json, native, sizeof(native)));
    CHECK(!sh_prefab_carrier_read("{\"x\":1,\"x\":2}", 13, &read, error, sizeof(error)));
    CHECK(!sh_prefab_carrier_write(native, sizeof(native)-1, NULL, &n, error, sizeof(error)) && !n);
    if (copy) HeapFree(GetProcessHeap(), 0, copy);
    sh_prefab_carrier_free(&read); sh_package_occurrence_release(empty);

    sh_package_bundle_binding rows[2] = {0}; unsigned char *bytes[2] = {0}, trees[2][32]; size_t sizes[2] = {0};
    sh_package_provenance historical = {0}, modern = {0};
    for (size_t i = 0; i < 2; i++) {
        rows[i].original = bundle(i ? "historical B" : "historical A", bytes+i, sizes+i); CHECK(rows[i].original);
        memcpy(trees[i], sh_package_bundle_fingerprint(rows[i].original), 32);
    }
    size_t context = sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED, trees, 2, NULL);
    CHECK(context != SIZE_MAX);
    CHECK(sh_package_provenance_bind(&historical, "entitydef", "authored/root", "private/same", &context, 1));
    historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    copy = sh_map_provenance_write(native, sizeof(native)-1, &historical, &length, error, sizeof(error)); CHECK(copy);
    CHECK(sh_prefab_carrier_read(copy, length, &read, error, sizeof(error)));
    CHECK(read.bindings_present && read.bindings.semantics == SH_PACKAGE_PROVENANCE_HISTORICAL);
    sh_prefab_carrier_free(&read); if (copy) HeapFree(GetProcessHeap(), 0, copy);
    modern.semantics = SH_PACKAGE_PROVENANCE_USES;
    add_use(&modern, rows, "entity/0", SH_PACKAGE_CONTEXT_COMPOSED, trees, 2);
    sh_package_occurrence_snapshot *document = sh_package_occurrence_prepare_document(&modern, rows, 2, error, sizeof(error)); CHECK(document);
    copy = sh_prefab_carrier_write(native, sizeof(native)-1, document, &length, error, sizeof(error)); CHECK(copy);
    CHECK(sh_prefab_carrier_read(copy, length, &read, error, sizeof(error)));
    CHECK(read.bindings.use_count == 1 && read.bindings.contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED &&
        read.bindings.contexts[0].tree_count == 2);
    sh_mpkg_decl packages[2]; CHECK(sh_mpkg_scan(copy, length, packages, 2) == 2);
    sh_package_occurrence_release(document); sh_prefab_carrier_free(&read);
    sh_package_provenance_free(&historical); sh_package_provenance_free(&modern);
    if (copy) HeapFree(GetProcessHeap(), 0, copy);
    for (size_t i = 0; i < 2; i++) { sh_package_bundle_release(rows[i].original); free(bytes[i]); }
}
int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pfc", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    exact_carriers(); historical_and_vanilla(); cleanup();
    printf("prefab_test: %d failures\n", failures); return failures ? 1 : 0;
}
