/* Complete authored snapshots survive editable inventories and provider views. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_bundle.h"
#include "package_fixture.h"

typedef struct bundle_reader {
    sh_package_bundle *bundle;
    const unsigned char *expected;
    size_t length;
    int ok;
} bundle_reader;

static DWORD WINAPI read_many(void *context)
{
    bundle_reader *r = context;
    r->ok = 1;
    for (size_t i = 0; i < 50; i++) {
        size_t length; char error[256];
        unsigned char *bytes = sh_package_bundle_read(r->bundle, &length, error, sizeof(error));
        if (!bytes || length != r->length || memcmp(bytes, r->expected, length)) r->ok = 0;
        free(bytes);
    }
    return 0;
}

static void retained_tree(void)
{
    sh_package_sources *sources;
    sh_package_bundle *bundle, *retained;
    unsigned char *expected, *bytes, fingerprint[32];
    char path[4096], key[SH_PACKAGE_BUNDLE_KEY_CAP], seed[SH_PACKAGE_ID_CAP], error[512];
    size_t expected_length, length;
    create("overrides/complete/package.json", " { \"id\": \"same\", \"name\": \"Complete\", \"extra\": 7 } \r\n");
    create("overrides/complete/assets/generated/decls/entitydef/unused.decl", "{ edit = { health = 17; } }");
    create("overrides/complete/Notes/Preserve.txt", "author bytes\r\n");
    create("overrides/complete/empty", NULL);
    create("overrides/complete/editor/package.json", "{\"id\":\"nested\",\"name\":\"Editor\"}");
    create("overrides/complete/editor/assets/unused.bin", "not referenced by gameplay");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) return;
    expected = sh_package_archive_pack(sources, 0, &expected_length, error, sizeof(error)); CHECK(expected);
    bundle = sh_package_bundle_capture(sources, 0, root, error, sizeof(error)); CHECK(bundle);
    CHECK(!sh_package_bundle_capture(sources, 1, root, error, sizeof(error)));
    if (!bundle || !expected) goto done;
    CHECK(!memcmp(sh_package_bundle_fingerprint(bundle), sources->fingerprints[0], 32));
    sh_package_bundle_delivery_key(bundle, key);
    CHECK(sh_package_bundle_parse_key(key, fingerprint, seed) && !seed[0]);
    CHECK(!memcmp(fingerprint, sources->fingerprints[0], 32));
    /* Editable source files are not sealed or consulted by retained reads. */
    create("overrides/complete/package.json", "{\"id\":\"same\",\"name\":\"Changed\"}");
    create("overrides/complete/Notes/Preserve.txt", "different local author bytes");
    CHECK(!sh_package_bundle_capture(sources, 0, root, error, sizeof(error)) && error[0]);
    snprintf(path, sizeof(path), "%s/overrides/complete/editor/assets/unused.bin", root);
    CHECK(DeleteFileA(path));
    sh_package_sources_free(sources); sources = NULL;
    retained = sh_package_bundle_retain(bundle); sh_package_bundle_release(bundle); bundle = retained;
    bytes = sh_package_bundle_read(bundle, &length, error, sizeof(error));
    CHECK(bytes && length == expected_length && !memcmp(bytes, expected, length)); free(bytes);
    {
        bundle_reader readers[2] = {{bundle, expected, expected_length, 0}, {bundle, expected, expected_length, 0}};
        HANDLE threads[2];
        for (size_t i = 0; i < 2; i++) { threads[i] = CreateThread(NULL, 0, read_many, readers + i, 0, NULL); CHECK(threads[i]); }
        for (size_t i = 0; i < 2; i++) if (threads[i]) {
            CHECK(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0); CloseHandle(threads[i]); CHECK(readers[i].ok);
        }
    }
    create("overrides/complete/editor/assets/unused.bin", "replacement for fixture cleanup");
done:
    sh_package_bundle_release(bundle); free(expected); sh_package_sources_free(sources);
}

static void raw_legacy(void)
{
    const char marker[] = "{\"schema\":\"snapmap-plus.override-package.v1\",\"name\":\"Old\"}";
    const unsigned char raw[] = {0, 0xff, 0x80, 'x'};
    sh_package_archive_file members[] = {
        {"package.json", (unsigned char *)marker, sizeof(marker) - 1, 0},
        {"author-extra.bin", (unsigned char *)raw, sizeof(raw), 0},
        {"empty", NULL, 0, 1}
    };
    sh_package_archive_files files = {members, 3};
    unsigned char *archive, *read, tree[32], parsed[32];
    sh_package_bundle *bundle;
    char error[512], key[SH_PACKAGE_BUNDLE_KEY_CAP], seed[SH_PACKAGE_ID_CAP], long_seed[SH_PACKAGE_ID_CAP];
    size_t length, read_length;
    archive = sh_package_archive_write(&files, &length, error, sizeof(error)); CHECK(archive);
    if (!archive) return;
    CHECK(!sh_package_archive_identity(archive, length, NULL, NULL, error, sizeof(error)));
    CHECK(sh_package_archive_tree_identity(archive, length, tree, error, sizeof(error)));
    memset(long_seed, 'a', sizeof(long_seed) - 1); long_seed[sizeof(long_seed) - 1] = 0;
    bundle = sh_package_bundle_from_legacy_archive(archive, length, long_seed, root, error, sizeof(error)); CHECK(bundle);
    sh_package_bundle_delivery_key(bundle, key);
    CHECK(strlen(key) + 1 == SH_PACKAGE_BUNDLE_KEY_CAP);
    CHECK(sh_package_bundle_parse_key(key, parsed, seed) && !memcmp(tree, parsed, 32) && !strcmp(seed, long_seed));
    read = sh_package_bundle_read(bundle, &read_length, error, sizeof(error));
    CHECK(read && read_length == length && !memcmp(read, archive, length)); free(read);
    key[16] = 'g'; CHECK(!sh_package_bundle_parse_key(key, parsed, seed));
    sh_package_bundle_delivery_key(bundle, key); key[strlen(key) - 1] = 0;
    CHECK(!sh_package_bundle_parse_key(key, parsed, seed));
    CHECK(!sh_package_bundle_from_legacy_archive(archive, length, "BAD", root, error, sizeof(error)));
    sh_package_bundle_release(bundle);
    archive[0] ^= 1;
    CHECK(!sh_package_bundle_from_archive(archive, length, root, error, sizeof(error)));
    free(archive);
}

int main(void)
{
    char temp[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pb", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    retained_tree(); raw_legacy();
    snprintf(pattern, sizeof(pattern), "%s/package-cache/bundles/*.zip", root);
    search = FindFirstFileA(pattern, &found); CHECK(search == INVALID_HANDLE_VALUE);
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    cleanup();
    printf("package_bundle_test: %d failures\n", failures); return failures ? 1 : 0;
}
