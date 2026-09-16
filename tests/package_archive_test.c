#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_archive.h"
#include "raw_deflate_encode.h"
#include "raw_deflate.h"
#include "map_package.h"
#include "package_runtime.h"
#include "engine_dialog.h"
#include "package_fixture.h"
#include "package_zip64_fixtures.h"

/* Offline delivery checks deliberately do not claim native UI coverage. */
const sh_package_compilation *sh_package_runtime_acquire(void) { return NULL; }
const sh_package_compilation *sh_package_runtime_library_acquire(void) { return NULL; }
void sh_package_runtime_release(void) {}
int sh_package_runtime_ready(void) { return 0; }
int sh_package_runtime_admission_ready(void) { return 0; }
void sh_package_runtime_note_sources_changed(void) {}
int sh_decl_server_registration_succeeded(void) { return 0; }
void sh_decl_server_request_rearm(void) {}
int sh_engine_dialog_ready(void) { return 0; }
int sh_engine_dialog_can_ask(void) { return 0; }
int sh_engine_dialog_ask(unsigned id, unsigned buttons, const char *text) { (void)id; (void)buttons; (void)text; return 0; }
int sh_engine_dialog_poll(int ticket) { (void)ticket; return SH_ENGINE_DIALOG_PENDING; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }
void backend_log(const char *text) { (void)text; }

static void compression(void)
{
    unsigned char input[70000], output[70000];
    unsigned state = 17, mode;
    size_t i, n;
    for (mode = 0; mode < 4; mode++) {
        for (i = 0; i < sizeof(input); i++) {
            state = state * 1664525u + 1013904223u;
            input[i] = mode == 0 ? 0 : mode == 1 ? (unsigned char)i :
                       mode == 2 ? (unsigned char)(state >> 24) : (unsigned char)((i / 7) % 39);
        }
        for (n = 0; n <= sizeof(input); n = n < 300 ? n + 1 : n < 32768 ? 32768 : n < sizeof(input) ? sizeof(input) : sizeof(input) + 1) {
            size_t compressed_length;
            unsigned char *compressed = sh_deflate_raw(input, n, &compressed_length);
            CHECK(compressed);
            if (!compressed) continue;
            CHECK(sh_inflate_raw_exact(compressed, compressed_length, output, n));
            CHECK(!memcmp(input, output, n));
            CHECK(!sh_inflate_raw_exact(compressed, compressed_length - 1, output, n));
            free(compressed);
        }
    }
}

static void zip64_headers(void)
{
    unsigned char copy[sizeof(zip64_fixture)], fingerprint[32];
    char error[512], id[SH_PACKAGE_ID_CAP];
    unsigned files;
    size_t i;
    /* Locations in the independent two-member fixture, including a ZIP64
     * central offset and both local and central size extensions. */
    const size_t corrupt[] = {18, 179 + 24, 237, 239, 338 + 7,
        346, 350, 360, 362, 366, 370, 385, 386, 394,
        402, 406, 410 + 7, 418, 430};
    CHECK(sh_package_archive_inspect(zip64_fixture, sizeof(zip64_fixture), id, &files, error, sizeof(error)));
    CHECK(files == 2 && !strcmp(id, "tests.zip64"));
    CHECK(sh_package_archive_identity(zip64_fixture, sizeof(zip64_fixture), id, fingerprint, error, sizeof(error)));
    for (i = 0; i < sizeof(zip64_fixture); i++)
        CHECK(!sh_package_archive_inspect(zip64_fixture, i, NULL, NULL, error, sizeof(error)));
    for (i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
        memcpy(copy, zip64_fixture, sizeof(copy)); copy[corrupt[i]] ^= 1;
        CHECK(!sh_package_archive_inspect(copy, sizeof(copy), NULL, NULL, error, sizeof(error)));
    }
}

static void zip64_large(const char *output)
{
    char source[MAX_PATH], destination[MAX_PATH], path[MAX_PATH], error[512], id[SH_PACKAGE_ID_CAP];
    sh_package_sources *sources = NULL, *installed = NULL;
    unsigned char *zip = NULL;
    size_t count = 0, i, length = 0;
    unsigned files;
    create("source/package.json", "{\"id\":\"tests.large\",\"name\":\"Large archive\"}");
    create("source/assets", NULL);
    snprintf(source, sizeof(source), "%s/source", root);
    snprintf(destination, sizeof(destination), "%s/installed", root);
    for (i = 0; i < 65536; i++) {
        snprintf(path, sizeof(path), "%s/assets/d%05zu", source, i);
        if (!CreateDirectoryA(path, NULL)) { CHECK(0); goto done; }
        count++;
    }
    sources = sh_package_sources_scan_directory(source, error, sizeof(error)); CHECK(sources);
    if (!sources) goto done;
    CHECK(sources->file_count == 65538);
    zip = sh_package_archive_pack(sources, 0, &length, error, sizeof(error)); CHECK(zip);
    if (!zip) { fprintf(stderr, "%s\n", error); goto done; }
    CHECK(sh_package_archive_inspect(zip, length, id, &files, error, sizeof(error)));
    CHECK(files == 1 && !strcmp(id, "tests.large"));
    {
        unsigned char fingerprint[32];
        CHECK(sh_package_archive_identity(zip, length, id, fingerprint, error, sizeof(error)));
        CHECK(!memcmp(fingerprint, sources->fingerprints[0], sizeof(fingerprint)));
    }
    {
        const char *map = "{\"variables\":{\"string\":[],\"allocCount\":[0,0,0,0,0]}}";
        char *embedded;
        unsigned char *extracted;
        size_t map_length = 0, extracted_length = 0;
        embedded = sh_mpkg_embed(map, strlen(map), id, zip, length, &map_length, error, sizeof(error));
        CHECK(embedded);
        if (embedded) {
            extracted = sh_mpkg_extract(embedded, map_length, id, &extracted_length, error, sizeof(error));
            CHECK(extracted && extracted_length == length && !memcmp(extracted, zip, length));
            if (extracted) HeapFree(GetProcessHeap(), 0, extracted);
            HeapFree(GetProcessHeap(), 0, embedded);
        }
    }
    CHECK(sh_package_archive_unpack(zip, length, destination, &files, error, sizeof(error)));
    installed = sh_package_sources_scan_directory(destination, error, sizeof(error)); CHECK(installed);
    if (installed) {
        CHECK(installed->file_count == sources->file_count);
        CHECK(!memcmp(installed->fingerprints[0], sources->fingerprints[0], 32));
    }
    if (output) {
        FILE *file = NULL;
        CHECK(!fopen_s(&file, output, "wb"));
        if (file) { CHECK(fwrite(zip, 1, length, file) == length); CHECK(!fclose(file)); }
    }
    printf("ZIP64: %zu exact entries packed, embedded, extracted and inventoried (%zu archive bytes)\n", sources->file_count, length);
done:
    free(zip); sh_package_sources_free(sources); sh_package_sources_free(installed);
    /* Only fixed fixture names within the freshly created source/destination. */
    for (i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/assets/d%05zu", source, i); CHECK(RemoveDirectoryA(path));
        snprintf(path, sizeof(path), "%s/assets/d%05zu", destination, i);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) CHECK(RemoveDirectoryA(path));
    }
    if (GetFileAttributesA(destination) != INVALID_FILE_ATTRIBUTES) {
        snprintf(path, sizeof(path), "%s/package.json", destination); CHECK(DeleteFileA(path));
        snprintf(path, sizeof(path), "%s/assets", destination); CHECK(RemoveDirectoryA(path));
        CHECK(RemoveDirectoryA(destination));
    }
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], source[MAX_PATH], destination[MAX_PATH], error[512], id[SH_PACKAGE_ID_CAP], long_path[1000];
    sh_package_sources *s = NULL, *installed = NULL;
    unsigned char *zip = NULL, *again = NULL;
    size_t length, again_length, i;
    unsigned files;
    if (argc == 3 && strcmp(argv[1], "--large")) {
        FILE *out;
        s = sh_package_sources_scan_directory(argv[1], error, sizeof(error));
        if (!s) { fprintf(stderr, "%s\n", error); return 1; }
        zip = sh_package_archive_pack(s, 0, &length, error, sizeof(error));
        if (!zip || !sh_package_archive_inspect(zip, length, id, &files, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error); return 1;
        }
        if (fopen_s(&out, argv[2], "wb")) return 1;
        CHECK(fwrite(zip, 1, length, out) == length); CHECK(!fclose(out));
        printf("%s: %u files, %zu source entries, %zu compressed archive bytes\n", id, files, s->file_count, length);
        free(zip); sh_package_sources_free(s); return failures ? 1 : 0;
    }
    compression(); zip64_headers();
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pa", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    if (argc >= 2 && !strcmp(argv[1], "--large")) {
        zip64_large(argc == 3 ? argv[2] : NULL); cleanup(); return failures ? 1 : 0;
    }
    create("source/package.json", "{\"id\":\"tests.boss-demons\",\"name\":\"Boss demons\",\"strings\":{\"en\":{\"boss\":\"Demons\"}}}\n");
    create("source/cyberdemon/package.json", "{\"id\":\"tests.cyberdemon\",\"name\":\"Cyberdemon\"}");
    create("source/cyberdemon/assets/generated/decls/entitydef/cyber.decl", "{ edit = { value = 1; } }\r\n");
    create("source/README.md", "Exact authored bytes.\r\n");
    create("source/empty", NULL);
    create("source/empty-file", "");
    /* Installer-looking names are authored content too; delivery omits nothing. */
    create("source/smpkg.digest", "authored auxiliary file");
    /* A stored payload larger than the removed 32 MiB policy cap. */
    create("source/large-asset.bin", "");
    {
        char path[MAX_PATH]; unsigned char chunk[65536]; unsigned state = 9, block;
        HANDLE file; DWORD written;
        snprintf(path, sizeof(path), "%s/source/large-asset.bin", root);
        file = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            for (block = 0; block < 640; block++) {
                size_t k;
                for (k = 0; k < sizeof(chunk); k++) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; chunk[k] = (unsigned char)state; }
                CHECK(WriteFile(file, chunk, sizeof(chunk), &written, NULL) && written == sizeof(chunk));
            }
            CloseHandle(file);
        }
    }
    snprintf(long_path, sizeof(long_path), "source/cyberdemon/assets/generated/spirv/%s/%s/%s/model.vspv",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    create(long_path, "long shader bytes\n");
    snprintf(source, sizeof(source), "%s/source", root); snprintf(destination, sizeof(destination), "%s/installed", root);
    s = sh_package_sources_scan_directory(source, error, sizeof(error)); CHECK(s);
    if (!s) goto done;
    zip = sh_package_archive_pack(s, 0, &length, error, sizeof(error)); CHECK(zip);
    if (!zip) goto done;
    CHECK(sh_package_archive_inspect(zip, length, id, &files, error, sizeof(error)));
    {
        unsigned char fingerprint[32];
        CHECK(sh_package_archive_identity(zip, length, id, fingerprint, error, sizeof(error)));
        CHECK(!memcmp(fingerprint, s->fingerprints[0], 32));
    }
    CHECK(!strcmp(id, "tests.boss-demons") && files == 8);
    CHECK(length > 32u * 1024u * 1024u);
    {
        const char *map = "{\"variables\":{\"string\":[],\"allocCount\":[0,0,0,0,0]}}";
        char *embedded, *stripped;
        unsigned char *extracted;
        size_t map_length, extracted_length, stripped_length;
        sh_mpkg_decl declaration;
        embedded = sh_mpkg_embed(map, strlen(map), id, zip, length, &map_length, error, sizeof(error));
        CHECK(embedded);
        if (embedded) {
            CHECK(sh_mpkg_scan(embedded, map_length, &declaration, 1) == 1);
            CHECK(declaration.complete && !strcmp(declaration.id, id));
            CHECK(declaration.total > 2048); /* crosses the old reader's shard ceiling too */
            extracted = sh_mpkg_extract(embedded, map_length, id, &extracted_length, error, sizeof(error));
            CHECK(extracted && extracted_length == length && !memcmp(extracted, zip, length));
            if (extracted) HeapFree(GetProcessHeap(), 0, extracted);
            stripped = sh_mpkg_strip(embedded, map_length, &stripped_length);
            CHECK(stripped && stripped_length == strlen(map) && !memcmp(stripped, map, stripped_length));
            if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
            HeapFree(GetProcessHeap(), 0, embedded);
        }
    }
    again = sh_package_archive_pack(s, 0, &again_length, error, sizeof(error));
    CHECK(again && again_length == length && !memcmp(zip, again, length)); free(again); again = NULL;
    /* Corrupt a stored member's data: CRC must fail before any write. */
    {
        size_t offset = 0;
        while (offset + 30 < length) {
            unsigned method = zip[offset + 8] | ((unsigned)zip[offset + 9] << 8);
            unsigned packed = zip[offset + 18] | ((unsigned)zip[offset + 19] << 8) |
                              ((unsigned)zip[offset + 20] << 16) | ((unsigned)zip[offset + 21] << 24);
            unsigned n = zip[offset + 26] | ((unsigned)zip[offset + 27] << 8);
            size_t data = offset + 30 + n;
            if (!method && packed) {
                zip[data] ^= 0x40;
                CHECK(!sh_package_archive_unpack(zip, length, destination, &files, error, sizeof(error)));
                CHECK(strstr(error, "CRC") != NULL); CHECK(GetFileAttributesA(destination) == INVALID_FILE_ATTRIBUTES);
                zip[data] ^= 0x40; break;
            }
            offset = data + packed;
        }
        CHECK(offset + 30 < length);
    }
    if (!sh_package_archive_unpack(zip, length, destination, &files, error, sizeof(error))) { fprintf(stderr, "%s\n", error); CHECK(0); goto done; }
    strcpy_s(created[created_count], sizeof(created[0]), destination); directories[created_count++] = 1;
    for (i = 0; i < s->file_count; i++) {
        snprintf(created[created_count], sizeof(created[0]), "%s/%s", destination, s->files[i].relative);
        directories[created_count++] = s->files[i].directory;
    }
    installed = sh_package_sources_scan_directory(destination, error, sizeof(error)); CHECK(installed);
    if (installed) {
        CHECK(installed->file_count == s->file_count && installed->component_count == s->component_count);
        CHECK(!memcmp(installed->fingerprints[0], s->fingerprints[0], 32));
        again = sh_package_archive_pack(installed, 0, &again_length, error, sizeof(error));
        CHECK(again && length == again_length && !memcmp(zip, again, length));
    }
    CHECK(!sh_package_archive_unpack(zip, length, destination, &files, error, sizeof(error)));
    create("source/README.md", "Changed after inventory");
    free(again); again = sh_package_archive_pack(s, 0, &again_length, error, sizeof(error)); CHECK(!again);
done:
    free(zip); free(again); sh_package_sources_free(s); sh_package_sources_free(installed); cleanup();
    if (failures) return 1;
    puts("package archive and raw compression checks passed"); return 0;
}
