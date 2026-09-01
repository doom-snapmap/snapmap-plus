/* resource_bridge_test.c -- sparse installed-resource snapshot and decode tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_deflate.h"
#include "decl_text.h"
#include "resource_bridge.h"

static int g_failed;
static char g_last_log[1024];

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

void backend_log(const char *message)
{
    strcpy_s(g_last_log, sizeof(g_last_log), message ? message : "");
}

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

static void cleanup_tree(const char *root, const char *base, const char *manifest)
{
    char path[MAX_PATH];
    DeleteFileA(manifest);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    DeleteFileA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    DeleteFileA(path);
    RemoveDirectoryA(base);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\resources", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\package.json", root);
    DeleteFileA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides", root);
    RemoveDirectoryA(path);
    RemoveDirectoryA(root);
}

static void test_sparse_snapshot(void)
{
    static const unsigned char compressed[] = {
        0xab,0xe6,0x52,0x28,0x4b,0xcc,0x29,0x4d,0x55,0xb0,0x55,0x50,0x4a,
        0xae,0x4c,0x4a,0x2d,0x4a,0x49,0xcd,0xcd,0xcf,0x53,0xe2,0xaa,0xe5,
        0xaa,0x1e,0x95,0x19,0x95,0x19,0x95,0x21,0x20,0x03,0x00
    };
    static const char manifest_text[] =
        "entityDef\tai/demon/cyberdemon\t"
        "generated/decls/entitydef/ai/demon/cyberdemon.decl\n";
    unsigned char pindex[2048], expected[832], truncated[832];
    unsigned char doubled[sizeof(compressed) * 2];
    unsigned char *opened = NULL;
    char *decl = NULL;
    size_t pindex_length, opened_length = 0, decl_length = 0;
    const char *source = NULL, *type = NULL, *name = NULL, *reason = NULL;
    char temp[MAX_PATH], root[MAX_PATH], base[MAX_PATH], path[MAX_PATH], manifest[MAX_PATH];
    DWORD pid = GetCurrentProcessId();
    GetTempPathA(sizeof(temp), temp);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssnapmap-plus-resource-bridge-%lu",
                temp, (unsigned long)pid);
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\base", root);
    CHECK(make_dir(root));
    CHECK(make_dir(base));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides", root); CHECK(make_dir(path));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides", root); CHECK(make_dir(path));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\package.json", root);
    CHECK(write_bytes(path, "{}", 2));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\resources", root); CHECK(make_dir(path));
    _snprintf_s(manifest, sizeof(manifest), _TRUNCATE,
                "%s\\overrides\\my-overrides\\resources\\cyberdemon.manifest", root);
    CHECK(write_bytes(manifest, manifest_text, sizeof(manifest_text) - 1));

    pindex_length = build_pindex(pindex, sizeof(pindex), sizeof(expected), sizeof(compressed));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    CHECK(write_bytes(path, pindex, pindex_length));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    CHECK(write_bytes(path, compressed, sizeof(compressed)));
    expected_body(expected, sizeof(expected));

    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 1);
    CHECK(sh_resource_bridge_has_manifests() == 1);
    CHECK(sh_resource_bridge_entry_count() == 1);
    CHECK(sh_resource_bridge_decl_count() == 1);
    CHECK(sh_resource_bridge_gate_ok() == 0);
    sh_resource_bridge_set_provider_ready(1);
    CHECK(sh_resource_bridge_gate_ok() == 1);
    CHECK(sh_resource_bridge_open("generated\\decls\\entitydef\\ai\\demon\\cyberdemon.decl",
                                  &opened, &opened_length, &source) == SH_RESOURCE_BRIDGE_OPENED);
    CHECK(opened_length == sizeof(expected));
    CHECK(opened && memcmp(opened, expected, sizeof(expected)) == 0);
    CHECK(source && strstr(source, "cyberdemon.manifest:1") != NULL);
    if (opened) HeapFree(GetProcessHeap(), 0, opened);
    CHECK(sh_resource_bridge_open("not/admitted", &opened, &opened_length, &source) ==
          SH_RESOURCE_BRIDGE_MISS);
    CHECK(sh_resource_bridge_decl_metadata(0, &type, &name, &source) == 1);
    CHECK(type && strcmp(type, "entityDef") == 0);
    CHECK(name && strcmp(name, "ai/demon/cyberdemon") == 0);
    CHECK(source && strcmp(source,
          "generated/decls/entitydef/ai/demon/cyberdemon.decl") == 0);
    CHECK(sh_resource_bridge_read_decl(0, &decl, &decl_length, &reason) == 1);
    CHECK(decl_length == sizeof(expected));
    CHECK(decl && memcmp(decl, expected, sizeof(expected)) == 0 && decl[decl_length] == '\0');
    if (decl) HeapFree(GetProcessHeap(), 0, decl);

    memset(truncated, 0, sizeof(truncated));
    CHECK(sh_inflate_raw(compressed, sizeof(compressed) / 2,
                         truncated, sizeof(truncated)) != sizeof(truncated));
    sh_resource_bridge_test_reset();

    memcpy(doubled, compressed, sizeof(compressed));
    memcpy(doubled + sizeof(compressed), compressed, sizeof(compressed));
    CHECK(write_bytes(manifest,
          "entityDef\tai/demon/cyberdemon\tgenerated/decls/entitydef/ai/demon/cyberdemon.decl\n"
          "entityDef\tai/demon/cyberdemon\tgenerated/renderprogs/cyberdemon.bin\n",
          sizeof("entityDef\tai/demon/cyberdemon\tgenerated/decls/entitydef/ai/demon/cyberdemon.decl\n"
                 "entityDef\tai/demon/cyberdemon\tgenerated/renderprogs/cyberdemon.bin\n") - 1));
    memset(pindex, 0, sizeof(pindex));
    pindex_length = 0x24;
    pindex_length = append_pindex_row(
        pindex, pindex_length, 0x1234, "entityDef", "ai/demon/cyberdemon",
        "generated/decls/entitydef/ai/demon/cyberdemon.decl", 0,
        sizeof(expected), sizeof(compressed), 0);
    pindex_length = append_pindex_row(
        pindex, pindex_length, 0x1234, "entityDef", "ai/demon/cyberdemon",
        "generated/renderprogs/cyberdemon.bin", sizeof(compressed),
        sizeof(expected), sizeof(compressed), 0);
    pindex_length = finish_pindex(pindex, sizeof(pindex), 2, pindex_length);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    CHECK(write_bytes(path, pindex, pindex_length));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    CHECK(write_bytes(path, doubled, sizeof(doubled)));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 1);
    CHECK(sh_resource_bridge_entry_count() == 2);
    CHECK(sh_resource_bridge_decl_count() == 1);
    sh_resource_bridge_set_provider_ready(1);
    CHECK(sh_resource_bridge_open("generated/renderprogs/cyberdemon.bin",
                                  &opened, &opened_length, &source) ==
          SH_RESOURCE_BRIDGE_OPENED);
    CHECK(opened_length == sizeof(expected));
    CHECK(opened && memcmp(opened, expected, sizeof(expected)) == 0);
    if (opened) HeapFree(GetProcessHeap(), 0, opened);
    sh_resource_bridge_test_reset();

    CHECK(write_bytes(manifest, manifest_text, sizeof(manifest_text) - 1));
    memset(pindex, 0, sizeof(pindex));
    pindex_length = 0x24;
    pindex_length = append_pindex_row(
        pindex, pindex_length, 0x1234, "entityDef", "ai/demon/cyberdemon",
        "generated/decls/entitydef/ai/demon/cyberdemon.decl", 0,
        sizeof(expected), sizeof(compressed), 0);
    pindex_length = append_pindex_row(
        pindex, pindex_length, 0x1234, "entityDef", "ai/demon/cyberdemon",
        "generated/decls/entitydef/ai/demon/cyberdemon.decl", sizeof(compressed),
        sizeof(expected), sizeof(compressed), 0);
    pindex_length = finish_pindex(pindex, sizeof(pindex), 2, pindex_length);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    CHECK(write_bytes(path, pindex, pindex_length));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    CHECK(write_bytes(path, doubled, sizeof(doubled)));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 1);
    CHECK(sh_resource_bridge_entry_count() == 1);
    CHECK(strstr(g_last_log, "1 byte-identical duplicate pindex row(s) collapsed") != NULL);
    sh_resource_bridge_test_reset();

    doubled[sizeof(compressed)] ^= 1u;
    CHECK(write_bytes(path, doubled, sizeof(doubled)));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 0);
    CHECK(sh_resource_bridge_entry_count() == 0);
    CHECK(strstr(g_last_log, "zero entries admitted") != NULL);
    sh_resource_bridge_test_reset();

    CHECK(write_bytes(manifest,
          "entityDef\tai/demon/cyberdemon\tsame.decl\n"
          "fx\tfx/creatures/cyberdemon\tsame.decl\n",
          sizeof("entityDef\tai/demon/cyberdemon\tsame.decl\n"
                 "fx\tfx/creatures/cyberdemon\tsame.decl\n") - 1));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 0);
    CHECK(sh_resource_bridge_gate_ok() == 0);
    CHECK(sh_resource_bridge_entry_count() == 0);
    CHECK(strstr(g_last_log, "zero entries admitted") != NULL);
    sh_resource_bridge_test_reset();

    /* A compressed payload with no decoded bytes is not a legal selected
     * archive slice for this bridge. It must not be treated as an empty
     * successful stream merely because the decoder returns zero. */
    CHECK(write_bytes(manifest, manifest_text, sizeof(manifest_text) - 1));
    memset(pindex, 0, sizeof(pindex));
    pindex_length = build_pindex(pindex, sizeof(pindex), 0, sizeof(compressed));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    CHECK(write_bytes(path, pindex, pindex_length));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    CHECK(write_bytes(path, compressed, sizeof(compressed)));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 0);
    CHECK(sh_resource_bridge_entry_count() == 0);
    CHECK(strstr(g_last_log, "zero entries admitted") != NULL);
    sh_resource_bridge_test_reset();
    cleanup_tree(root, base, manifest);
}

static int probe_installed_snapshot(const char *root, const char *base)
{
    size_t count, index, total = 0, failures = 0;
    size_t compressed_count = 0, stored_count = 0, zero_count = 0;
    size_t first_index = 0, first_expected = 0, first_actual = 0;
    int first_status = SH_RESOURCE_BRIDGE_MISS;
    const char *first_alias = NULL, *first_source = NULL;
    char first_reason[1024] = "";
    sh_resource_bridge_test_set_doom_base(base);
    if (!sh_resource_bridge_capture(root)) {
        fprintf(stderr, "resource bridge probe capture failed: %s\n", g_last_log);
        sh_resource_bridge_test_reset();
        return 1;
    }
    sh_resource_bridge_set_provider_ready(1);
    count = sh_resource_bridge_entry_count();
    if (!count || !sh_resource_bridge_has_manifests()) {
        fprintf(stderr,
                "resource bridge installed probe: FAIL (no manifest-backed entries found; "
                "data-root must be the snapmap-plus directory above overrides)\n");
        sh_resource_bridge_test_reset();
        return 1;
    }
    for (index = 0; index < count; index++) {
        const char *alias = NULL, *source = NULL;
        size_t expected = 0, stored_size = 0, length = 0;
        unsigned char *bytes = NULL;
        int status = SH_RESOURCE_BRIDGE_MISS;
        int metadata_ok = sh_resource_bridge_test_entry_metadata(
            index, &alias, &expected, &stored_size, &source);
        if (metadata_ok) {
            if (!expected && !stored_size) zero_count++;
            else if (stored_size == expected) stored_count++;
            else compressed_count++;
        }
        if (metadata_ok)
            status = sh_resource_bridge_open(alias, &bytes, &length, &source);
        if (!metadata_ok || status != SH_RESOURCE_BRIDGE_OPENED || length != expected) {
            if (!failures) {
                first_index = index;
                first_alias = alias;
                first_source = source;
                first_status = status;
                first_expected = expected;
                first_actual = length;
                strcpy_s(first_reason, sizeof(first_reason), g_last_log);
            }
            failures++;
            if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
            continue;
        }
        if (_strnicmp(alias, "generated/decls/", 16) == 0 &&
            !sh_decl_text_well_formed(bytes, length)) {
            if (!failures) {
                first_index = index;
                first_alias = alias;
                first_source = source;
                first_status = SH_RESOURCE_BRIDGE_OPENED;
                first_expected = expected;
                first_actual = length;
                strcpy_s(first_reason, sizeof(first_reason),
                         "linked decl is structurally invalid");
            }
            failures++;
            if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
            continue;
        }
        total += length;
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    }
    if (failures) {
        fprintf(stderr,
                "resource bridge installed probe: FAIL (%zu entries checked, %zu failures, "
                "%zu compressed, %zu stored, %zu zero, %zu decoded bytes); first failure "
                "at %zu (%s): status=%d expected=%zu actual=%zu source=%s reason=%s\n",
                count, failures, compressed_count, stored_count, zero_count, total,
                first_index, first_alias ? first_alias : "?",
                first_status, first_expected, first_actual,
                first_source ? first_source : "?", first_reason);
        sh_resource_bridge_test_reset();
        return 1;
    }
    printf("resource bridge installed probe: PASS (%zu entries, %zu compressed, %zu stored, "
           "%zu zero, %zu decoded bytes)\n",
           count, compressed_count, stored_count, zero_count, total);
    sh_resource_bridge_test_reset();
    return 0;
}

/* Two packages that bridge the SAME resource is the normal case -- shared gore,
 * FX and animation assets belong to no single demon. The identical rows must
 * compose into one served entry, not refuse the whole snapshot, and a manifest
 * from an earlier package must survive a later package being present at all. */
static void test_packages_compose(void)
{
    static const unsigned char compressed[] = {
        0xab,0xe6,0x52,0x28,0x4b,0xcc,0x29,0x4d,0x55,0xb0,0x55,0x50,0x4a,
        0xae,0x4c,0x4a,0x2d,0x4a,0x49,0xcd,0xcd,0xcf,0x53,0xe2,0xaa,0xe5,
        0xaa,0x1e,0x95,0x19,0x95,0x19,0x95,0x21,0x20,0x03,0x00
    };
    static const char shared_row[] =
        "entityDef\tai/demon/cyberdemon\t"
        "generated/decls/entitydef/ai/demon/cyberdemon.decl\n";
    static const char conflicting_row[] =
        "entityDef\tai/demon/baron\t"
        "generated/decls/entitydef/ai/demon/cyberdemon.decl\n";
    unsigned char pindex[2048], expected[832];
    size_t pindex_length;
    const char *source = NULL;
    char temp[MAX_PATH], root[MAX_PATH], base[MAX_PATH], path[MAX_PATH];
    char first[MAX_PATH], second[MAX_PATH];
    DWORD pid = GetCurrentProcessId();

    GetTempPathA(sizeof(temp), temp);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssnapmap-plus-rb-compose-%lu",
                temp, (unsigned long)pid);
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\base", root);
    CHECK(make_dir(root));
    CHECK(make_dir(base));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides", root); CHECK(make_dir(path));

    /* Package one. */
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides", root);
    CHECK(make_dir(path));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\package.json", root);
    CHECK(write_bytes(path, "{}", 2));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\resources", root);
    CHECK(make_dir(path));
    _snprintf_s(first, sizeof(first), _TRUNCATE,
                "%s\\overrides\\my-overrides\\resources\\shared.manifest", root);
    CHECK(write_bytes(first, shared_row, sizeof(shared_row) - 1));

    /* Package two: sorts AFTER "my-overrides", so if collection restarted at index
     * zero per package this package would erase package one's manifest. */
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second", root);
    CHECK(make_dir(path));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second\\package.json", root);
    CHECK(write_bytes(path, "{}", 2));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second\\resources", root);
    CHECK(make_dir(path));
    _snprintf_s(second, sizeof(second), _TRUNCATE,
                "%s\\overrides\\zz-second\\resources\\shared.manifest", root);
    CHECK(write_bytes(second, shared_row, sizeof(shared_row) - 1));

    pindex_length = build_pindex(pindex, sizeof(pindex), sizeof(expected), sizeof(compressed));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    CHECK(write_bytes(path, pindex, pindex_length));
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    CHECK(write_bytes(path, compressed, sizeof(compressed)));
    expected_body(expected, sizeof(expected));

    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 1);
    /* One entry, not two, and not a refusal. */
    CHECK(sh_resource_bridge_entry_count() == 1);
    CHECK(sh_resource_bridge_decl_count() == 1);
    {   /* the survivor still carries which package's manifest row served it */
        const char *type = NULL, *name = NULL;
        CHECK(sh_resource_bridge_decl_metadata(0, &type, &name, &source) == 1);
        CHECK(name && strcmp(name, "ai/demon/cyberdemon") == 0);
    }
    sh_resource_bridge_test_reset();

    /* A real disagreement is different: one provider path cannot serve two
     * identities, so the snapshot is refused rather than silently picking one. */
    CHECK(write_bytes(second, conflicting_row, sizeof(conflicting_row) - 1));
    sh_resource_bridge_test_set_doom_base(base);
    CHECK(sh_resource_bridge_capture(root) == 0);
    sh_resource_bridge_test_reset();

    DeleteFileA(first);
    DeleteFileA(second);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second\\package.json", root);
    DeleteFileA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second\\resources", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\zz-second", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.pindex", base);
    DeleteFileA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\gameresources.resources", base);
    DeleteFileA(path);
    RemoveDirectoryA(base);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\resources", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides\\package.json", root);
    DeleteFileA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides\\my-overrides", root);
    RemoveDirectoryA(path);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overrides", root);
    RemoveDirectoryA(path);
    RemoveDirectoryA(root);
}

int main(int argc, char **argv)
{
    if (argc == 3) return probe_installed_snapshot(argv[1], argv[2]);
    if (argc != 1) {
        fprintf(stderr, "usage: resource_bridge_test [data-root doom-base]\n");
        return 2;
    }
    test_huffman_code_spaces();
    test_full_stream_contract();
    test_sparse_snapshot();
    test_packages_compose();
    if (g_failed) {
        fprintf(stderr, "resource_bridge_test: %d failure(s)\n", g_failed);
        return 1;
    }
    printf("resource_bridge_test: PASS\n");
    return 0;
}
