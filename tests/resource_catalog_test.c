/* Installed resource catalog and exact raw-deflate decoding tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_deflate.h"
#include "resource_catalog.h"

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static int make_dir(const char *path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_bytes(const char *path, const void *data, size_t length)
{
    FILE *file = NULL;
    size_t written;
    if (fopen_s(&file, path, "wb") != 0 || !file) return 0;
    written = fwrite(data, 1, length, file);
    fclose(file);
    return written == length;
}

static void put_be32(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void put_be64(unsigned char *p, unsigned long long value)
{
    put_be32(p, (unsigned int)(value >> 32));
    put_be32(p + 4, (unsigned int)value);
}

static void put_le32(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static size_t put_field(unsigned char *buffer, size_t position, const char *value)
{
    size_t length = strlen(value);
    put_le32(buffer + position, (unsigned int)length);
    memcpy(buffer + position + 4, value, length);
    return position + 4 + length;
}

static size_t append_pindex_row(unsigned char *buffer, size_t position,
                                unsigned int id, const char *type,
                                const char *name, const char *path,
                                unsigned long long offset,
                                unsigned int decoded, unsigned int stored,
                                unsigned char selector)
{
    put_be32(buffer + position, id);
    position += 4;
    position = put_field(buffer, position, type);
    position = put_field(buffer, position, name);
    position = put_field(buffer, position, path);
    put_be64(buffer + position, offset);
    put_be32(buffer + position + 8, decoded);
    put_be32(buffer + position + 12, stored);
    memset(buffer + position + 16, 0, 5);
    buffer[position + 20] = selector;
    return position + 21;
}

static size_t finish_pindex(unsigned char *buffer, size_t cap,
                            unsigned int rows, size_t position)
{
    if (!buffer || cap < position || position < 0x24) return 0;
    buffer[0] = 0x05;
    memcpy(buffer + 1, "SER", 3);
    put_be32(buffer + 0x20, rows);
    put_be32(buffer + 4, (unsigned int)(position - 0x20));
    return position;
}

static size_t build_pindex(unsigned char *buffer, size_t cap,
                           unsigned int decoded, unsigned int stored)
{
    static const char type[] = "entityDef";
    static const char name[] = "ai/demon/cyberdemon";
    static const char path[] = "generated/decls/entitydef/ai/demon/cyberdemon.decl";
    size_t position = 0x24;
    if (cap < 512) return 0;
    memset(buffer, 0, cap);
    position = append_pindex_row(buffer, position, 0x1234, type, name, path,
                                 0, decoded, stored, 0);
    return finish_pindex(buffer, cap, 1, position);
}

static void expected_body(unsigned char *body, size_t length)
{
    static const char row[] = "{\n value = \"cyberdemon\"\n}\n";
    size_t row_length = sizeof(row) - 1;
    size_t position;
    for (position = 0; position < length; position += row_length)
        memcpy(body + position, row, row_length);
}

static void test_huffman_code_spaces(void)
{
    static const unsigned char oversubscribed[] = { 1, 1, 1 };
    static const unsigned char incomplete[] = { 2, 2 };
    static const unsigned char single[] = { 1 };
    static const unsigned char single_long[] = { 2 };
    static const unsigned char empty[] = { 0 };
    static const unsigned char complete[] = { 1, 2, 2 };
    static const unsigned char fixed_distance[30] = {
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
    };

    CHECK(sh_inflate_test_huff_build(oversubscribed, sizeof(oversubscribed),
                                     SH_INFLATE_HUFF_CODE_LENGTH) == 0);
    CHECK(sh_inflate_test_huff_build(incomplete, sizeof(incomplete),
                                     SH_INFLATE_HUFF_CODE_LENGTH) == 0);
    CHECK(sh_inflate_test_huff_build(complete, sizeof(complete),
                                     SH_INFLATE_HUFF_CODE_LENGTH) == 1);
    CHECK(sh_inflate_test_huff_build(single, sizeof(single),
                                     SH_INFLATE_HUFF_CODE_LENGTH) == 0);

    CHECK(sh_inflate_test_huff_build(oversubscribed, sizeof(oversubscribed),
                                     SH_INFLATE_HUFF_LITERAL_LENGTH) == 0);
    CHECK(sh_inflate_test_huff_build(incomplete, sizeof(incomplete),
                                     SH_INFLATE_HUFF_LITERAL_LENGTH) == 0);
    /* zlib's canonical validator permits the one-symbol, one-bit EOB-only
     * literal/length tree; a two-bit incomplete tree remains malformed. */
    CHECK(sh_inflate_test_huff_build(single, sizeof(single),
                                     SH_INFLATE_HUFF_LITERAL_LENGTH) == 1);
    CHECK(sh_inflate_test_huff_build(single_long, sizeof(single_long),
                                     SH_INFLATE_HUFF_LITERAL_LENGTH) == 0);
    CHECK(sh_inflate_test_huff_build(complete, sizeof(complete),
                                     SH_INFLATE_HUFF_LITERAL_LENGTH) == 1);

    CHECK(sh_inflate_test_huff_build(oversubscribed, sizeof(oversubscribed),
                                     SH_INFLATE_HUFF_DISTANCE) == 0);
    CHECK(sh_inflate_test_huff_build(single_long, sizeof(single_long),
                                     SH_INFLATE_HUFF_DISTANCE) == 0);
    CHECK(sh_inflate_test_huff_build(single, sizeof(single),
                                     SH_INFLATE_HUFF_DISTANCE) == 1);
    CHECK(sh_inflate_test_huff_build(empty, sizeof(empty),
                                     SH_INFLATE_HUFF_DISTANCE) == 1);
    CHECK(sh_inflate_test_huff_build(fixed_distance, sizeof(fixed_distance),
                                     SH_INFLATE_HUFF_FIXED_DISTANCE) == 1);
}

static void test_full_stream_contract(void)
{
    static const unsigned char fixed[] = { 0x73, 0x04, 0x00 }; /* final fixed: literal 'A', EOB */
    static const unsigned char nonfinal_then_empty_final[] = {
        0x72,0x04,0x0c,0x00 /* non-final fixed 'A', final empty fixed */
    };
    static const unsigned char nonfinal_sync_flush[] = {
        0x72,0x04,0x00,0x00,0x00,0xff,0xff
    };
    static const unsigned char sync_flush_bad_nlen[] = {
        0x72,0x04,0x00,0x00,0x00,0x00,0x00
    };
    static const unsigned char sync_flush_bad_padding[] = {
        0x72,0x04,0x20,0x00,0x00,0xff,0xff
    };
    static const unsigned char sync_flush_trailing[] = {
        0x72,0x04,0x00,0x00,0x00,0xff,0xff,0x00
    };
    static const unsigned char dynamic[] = {
        0xab,0xe6,0x52,0x28,0x4b,0xcc,0x29,0x4d,0x55,0xb0,0x55,0x50,0x4a,
        0xae,0x4c,0x4a,0x2d,0x4a,0x49,0xcd,0xcd,0xcf,0x53,0xe2,0xaa,0xe5,
        0xaa,0x1e,0x95,0x19,0x95,0x19,0x95,0x21,0x20,0x03,0x00
    };
    static const unsigned char dynamic_missing_eob[] = {
        0x05,0xc0,0x01,0x04,0x00,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00
    };
    static const unsigned char missing_eob[] = { 0x73, 0x04 };
    static const unsigned char exact_then_truncated[] = {
        0x00, 0x01, 0x00, 0xfe, 0xff, 'A' /* non-final stored block, no next header */
    };
    static const unsigned char nonfinal_eof[] = { 0x72, 0x04, 0x00 };
    static const unsigned char reserved_block[] = { 0x07 };
    static const unsigned char trailing_byte[] = { 0x73, 0x04, 0x00, 0x00 };
    unsigned char out[832];

    /* Both a small fixed stream and the installed-resource dynamic fixture must
     * consume a final EOB and end exactly at their bounded slice. */
    memset(out, 0, sizeof(out));
    CHECK(sh_inflate_raw(fixed, sizeof(fixed), out, 1) == 1);
    CHECK(out[0] == 'A');
    CHECK(sh_inflate_raw(nonfinal_then_empty_final,
                         sizeof(nonfinal_then_empty_final), out, 1) == 1);
    /* Doom's archive compressor terminates each selected compressed slice with
     * a non-final empty stored Z_SYNC_FLUSH block instead of BFINAL. */
    CHECK(sh_inflate_raw(nonfinal_sync_flush, sizeof(nonfinal_sync_flush), out, 1) == 1);
    CHECK(out[0] == 'A');
    CHECK(sh_inflate_raw(nonfinal_sync_flush, sizeof(nonfinal_sync_flush), out, 2) == 0);
    CHECK(sh_inflate_raw(sync_flush_bad_nlen, sizeof(sync_flush_bad_nlen), out, 1) == 0);
    CHECK(sh_inflate_raw(sync_flush_bad_padding, sizeof(sync_flush_bad_padding), out, 1) == 0);
    CHECK(sh_inflate_raw(sync_flush_trailing, sizeof(sync_flush_trailing), out, 1) == 0);
    memset(out, 0, sizeof(out));
    CHECK(sh_inflate_raw(dynamic, sizeof(dynamic), out, sizeof(out)) == sizeof(out));
    CHECK(sh_inflate_raw(dynamic_missing_eob, sizeof(dynamic_missing_eob), out, 1) == 0);

    /* Output reaching the requested size is not success by itself: a final
     * EOB/final block is still mandatory. */
    CHECK(sh_inflate_raw(missing_eob, sizeof(missing_eob), out, 1) == 0);
    CHECK(sh_inflate_raw(exact_then_truncated, sizeof(exact_then_truncated), out, 1) == 0);
    CHECK(sh_inflate_raw(nonfinal_eof, sizeof(nonfinal_eof), out, 1) == 0);
    CHECK(sh_inflate_raw(reserved_block, sizeof(reserved_block), out, 1) == 0);
    CHECK(sh_inflate_raw(fixed, sizeof(fixed), out, 2) == 0);

    /* A pindex zsize is an exact slice. Alignment bits in the final byte are
     * allowed, but a trailing byte is not silently ignored. */
    CHECK(sh_inflate_raw(trailing_byte, sizeof(trailing_byte), out, 1) == 0);
}

static const char *resource_path = "generated/decls/entitydef/ai/demon/cyberdemon.decl";
static const unsigned char compressed_body[] = {
    0xab,0xe6,0x52,0x28,0x4b,0xcc,0x29,0x4d,0x55,0xb0,0x55,0x50,0x4a,
    0xae,0x4c,0x4a,0x2d,0x4a,0x49,0xcd,0xcd,0xcf,0x53,0xe2,0xaa,0xe5,
    0xaa,0x1e,0x95,0x19,0x95,0x19,0x95,0x21,0x20,0x03,0x00
};
static char fixture[MAX_PATH];
static void put_file(const char *name, const void *bytes, size_t length)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", fixture, name);
    CHECK(write_bytes(path, bytes, length));
}
static void fixture_reset(void)
{
    unsigned char index[512] = {0};
    size_t length = finish_pindex(index, sizeof(index), 0, 0x24);
    put_file("snap_gameresources.pindex", index, length);
    length = build_pindex(index, sizeof(index), 832, sizeof(compressed_body));
    put_file("gameresources.pindex", index, length);
    put_file("snap_gameresources.resources", "", 0);
    put_file("snap_gameresources.patch", "", 0);
    put_file("gameresources.resources", compressed_body, sizeof(compressed_body));
    put_file("gameresources.patch", "", 0);
}
static sh_resource_catalog *open_catalog(void)
{
    char error[512];
    sh_resource_catalog *catalog = sh_resource_catalog_open(fixture, error, sizeof(error));
    if (!catalog) fprintf(stderr, "%s\n", error);
    CHECK(catalog != NULL);
    return catalog;
}
typedef struct reader_context {
    sh_resource_catalog *catalog;
    unsigned char expected[832];
    volatile LONG failures;
} reader_context;
static DWORD WINAPI read_concurrently(LPVOID parameter)
{
    reader_context *context = (reader_context *)parameter;
    int i;
    for (i = 0; i < 160; i++) {
        unsigned char *body = NULL;
        size_t length = 0;
        int result = sh_resource_catalog_read_path(context->catalog, resource_path, &body, &length);
        if (result != 2 || length != sizeof(context->expected) || !body || memcmp(body, context->expected, length))
            InterlockedIncrement(&context->failures);
        free(body);
    }
    return 0;
}
static void test_catalog_reads(void)
{
    sh_resource_catalog *catalog;
    const sh_resource_catalog_entry *const *found = NULL;
    unsigned char *body = NULL;
    size_t length = 0, i;
    reader_context context = {0};
    HANDLE threads[4];
    fixture_reset(); catalog = open_catalog(); if (!catalog) return;
    CHECK(sh_resource_catalog_count(catalog) == 1);
    CHECK(sh_resource_catalog_find(catalog, "ENTITYDEF", "AI\\DEMON\\CYBERDEMON", &found) == 1);
    CHECK(found && !strcmp(found[0]->path, resource_path));
    CHECK(sh_resource_catalog_find_path(catalog, resource_path, &found) == 1);
    CHECK(found && !strcmp(found[0]->type, "entitydef") && !strcmp(found[0]->name, "ai/demon/cyberdemon"));
    CHECK(sh_resource_catalog_find_path(catalog, "absent", &found) == 0 && !found);
    CHECK(sh_resource_catalog_find_path(catalog, NULL, &found) == 0 && !found);
    CHECK(sh_resource_catalog_find_path(catalog, resource_path, NULL) == 0);
    CHECK(sh_resource_catalog_read_path(catalog, "absent", &body, &length) == 0 && !body && !length);
    CHECK(sh_resource_catalog_read_entry(catalog, 0, &body, &length));
    expected_body(context.expected, sizeof(context.expected));
    CHECK(length == sizeof(context.expected) && body && !memcmp(body, context.expected, length)); free(body);
    context.catalog = catalog;
    for (i = 0; i < 4; i++) { threads[i] = CreateThread(NULL, 0, read_concurrently, &context, 0, NULL); CHECK(threads[i]); }
    CHECK(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; i++) CloseHandle(threads[i]);
    CHECK(!context.failures);
    sh_resource_catalog_close(catalog);
}
static void test_patch_duplicates_and_baselines(void)
{
    const sh_resource_catalog_entry *const *found;
    unsigned char index[2048] = {0};
    size_t length, position;
    sh_resource_catalog *catalog;
    unsigned char *body = NULL;
    size_t body_length = 0;
    fixture_reset();
    position = append_pindex_row(index, 0x24, 1, "entityDef", "ai/demon/cyberdemon", resource_path, 0, 1, 1, 1);
    length = finish_pindex(index, sizeof(index), 1, position);
    put_file("snap_gameresources.pindex", index, length); put_file("snap_gameresources.patch", "A", 1);
    catalog = open_catalog(); if (!catalog) return;
    CHECK(sh_resource_catalog_find_path(catalog, resource_path, &found) == 2);
    CHECK(found && found[0]->archive < 2 && found[1]->archive >= 2);
    CHECK(sh_resource_catalog_read_path(catalog, resource_path, &body, &body_length) == 1);
    CHECK(body_length == 1 && body && *body == 'A'); free(body); sh_resource_catalog_close(catalog);
    /* Equal rows in one catalog share the identity; divergent rows refuse. */
    position = append_pindex_row(index, position, 2, "entityDef", "ai/demon/cyberdemon", resource_path, 1, 1, 1, 1);
    length = finish_pindex(index, sizeof(index), 2, position);
    put_file("snap_gameresources.pindex", index, length); put_file("snap_gameresources.patch", "AA", 2);
    catalog = open_catalog(); if (!catalog) return;
    CHECK(sh_resource_catalog_read_path(catalog, resource_path, &body, &body_length) == 1); free(body);
    sh_resource_catalog_close(catalog);
    put_file("snap_gameresources.patch", "AB", 2);
    catalog = open_catalog(); if (!catalog) return;
    CHECK(sh_resource_catalog_read_path(catalog, resource_path, &body, &body_length) == -1 && !body && !body_length);
    sh_resource_catalog_close(catalog);
}
static void test_encoded_aliases(void)
{
    static const unsigned char a[] = {0x73, 0x04, 0x00};
    static const unsigned char padded[] = {0x73, 0x04, 0x80};
    static const unsigned char alternate[] = {0x72, 0x04, 0x0c, 0x00};
    static const unsigned char prefix[] = {0x02, 0xcc, 0x11, 0x00};
    static const unsigned char b[] = {0x73, 0x02, 0x00};
    static const unsigned char invalid[] = {0x73, 0x04};
    const struct {
        const unsigned char *first, *second;
        unsigned first_stored, second_stored, second_size;
        int alias, patch, expected;
    } cases[] = {
        {a, a, 3, 3, 1, 0, 0, 2},
        {a, a, 3, 3, 1, 1, 0, 2},
        {a, padded, 3, 3, 1, 0, 0, -1},
        {alternate, prefix, 4, 4, 1, 0, 0, 2},
        {a, alternate, 3, 4, 1, 0, 0, 2},
        {a, (const unsigned char *)"A", 3, 1, 1, 0, 0, 2},
        {a, b, 3, 3, 1, 0, 0, -1},
        {a, b, 3, 3, 1, 0, 1, -1},
        {a, a, 3, 3, 2, 1, 0, -1},
        {invalid, invalid, 2, 2, 1, 1, 0, -1},
        {a, invalid, 3, 2, 1, 0, 0, -1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned char index[2048] = {0}, storage[16] = {0};
        unsigned char *body = NULL;
        size_t position, length = 0;
        sh_resource_catalog *catalog;
        fixture_reset();
        memcpy(storage, cases[i].first, cases[i].first_stored);
        memcpy(storage + cases[i].first_stored, cases[i].second, cases[i].second_stored);
        position = append_pindex_row(index, 0x24, 1, "renderprog", "first", resource_path,
            0, 1, cases[i].first_stored, 0);
        position = append_pindex_row(index, position, 2, "renderprog", "second", resource_path,
            cases[i].alias || cases[i].patch ? 0 : cases[i].first_stored,
            cases[i].second_size, cases[i].second_stored, (unsigned char)cases[i].patch);
        position = finish_pindex(index, sizeof(index), 2, position);
        put_file("gameresources.pindex", index, position);
        put_file("gameresources.resources", storage, cases[i].first_stored + cases[i].second_stored);
        put_file("gameresources.patch", cases[i].second, cases[i].second_stored);
        catalog = open_catalog(); if (!catalog) continue;
        int result = sh_resource_catalog_read_path(catalog, resource_path, &body, &length);
        if (result != cases[i].expected) fprintf(stderr, "encoded alias case %zu: result=%d\n", i, result);
        CHECK(result == cases[i].expected);
        if (cases[i].expected > 0) CHECK(body && length == 1 && *body == 'A');
        else CHECK(!body && !length);
        free(body); sh_resource_catalog_close(catalog);
    }
}

static void test_malformed_catalog(void)
{
    unsigned char index[2048] = {0};
    char error[512];
    sh_resource_catalog *catalog;
    size_t length, position;
    unsigned test;
    for (test = 0; test < 9; test++) {
        unsigned size = 1, stored = 1;
        uint64_t offset = 0;
        unsigned char selector = 0;
        fixture_reset();
        if (test == 0) selector = 2;
        if (test == 1) offset = UINT64_MAX;
        if (test == 2) stored = 100;
        if (test == 3) size = 0;
        if (test == 4) stored = 0;
        position = append_pindex_row(index, 0x24, 1, "entityDef", "demo", resource_path, offset, size, stored, selector);
        length = finish_pindex(index, sizeof(index), 1, position);
        if (test == 5) index[0] = 0;
        if (test == 6) put_be32(index + 4, 1);
        if (test == 7) put_be32(index + 0x20, 1000001);
        if (test == 8) put_le32(index + 0x28, 4096);
        put_file("gameresources.pindex", index, length);
        catalog = sh_resource_catalog_open(fixture, error, sizeof(error)); CHECK(!catalog && error[0]);
        sh_resource_catalog_close(catalog);
    }
    fixture_reset();
    /* A valid row with an invalid exact compressed slice fails on read. */
    length = build_pindex(index, sizeof(index), 832, sizeof(compressed_body) - 1);
    put_file("gameresources.pindex", index, length);
    catalog = open_catalog();
    if (catalog) {
        unsigned char *body = NULL; size_t n = 0;
        CHECK(sh_resource_catalog_read_path(catalog, resource_path, &body, &n) == -1 && !body && !n);
        sh_resource_catalog_close(catalog);
    }
}
int main(void)
{
    static const char *files[] = {"snap_gameresources.pindex", "gameresources.pindex", "snap_gameresources.resources", "snap_gameresources.patch", "gameresources.resources", "gameresources.patch"};
    char temp[MAX_PATH], path[MAX_PATH];
    size_t i;
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "rct", 0, fixture));
    CHECK(DeleteFileA(fixture)); CHECK(make_dir(fixture));
    test_huffman_code_spaces(); test_full_stream_contract(); test_catalog_reads();
    test_patch_duplicates_and_baselines(); test_encoded_aliases(); test_malformed_catalog();
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", fixture, files[i]); CHECK(DeleteFileA(path));
    }
    CHECK(RemoveDirectoryA(fixture));
    if (g_failed) return 1;
    puts("resource catalog and decoder checks passed"); return 0;
}
