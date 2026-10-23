#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "package_native_assets.h"
#include "package_sources.h"
#include "resource_catalog.h"
#include "package_fixture.h"

typedef struct concurrent_reader {
    sh_package_native_assets *assets;
    HANDLE ready;
    unsigned char content[32];
} concurrent_reader;
static DWORD WINAPI read_concurrently(void *context)
{
    concurrent_reader *reader = context;
    sh_package_library_native adapter = sh_package_native_assets_adapter(reader->assets);
    sh_package_compilation compiled = {0};
    sh_package_virtual_atlas atlas = {0};
    assert(WaitForSingleObject(reader->ready, 10000) == WAIT_OBJECT_0);
    for (size_t i = 0; i < 64; i++) {
        char path[96], error[1024]; unsigned char *body = NULL; size_t length = 0;
        sh_package_native_assets *held = sh_package_native_assets_retain(reader->assets);
        snprintf(path, sizeof(path), "virtualtextures/concurrent%zu.vmtr", i);
        assert(sh_package_native_assets_read(held, path, &body, &length) == 1);
        assert(length == 5 && !memcmp(body, "26\n0\n", 5)); free(body);
        assert(adapter.atlas(adapter.source.context, i, &compiled, "sample", "", &atlas, error, sizeof(error)) == 1);
        if (i) assert(!memcmp(reader->content, atlas.content, 32));
        memcpy(reader->content, atlas.content, 32);
        sh_package_native_assets_close(held);
    }
    sh_package_native_assets_close(reader->assets); return 0;
}
static void concurrent_capture(void)
{
    HANDLE workers[4], ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    concurrent_reader readers[4] = {0};
    sh_package_native_assets *assets = sh_package_native_assets_open(root);
    assert(ready && assets);
    for (size_t i = 0; i < 64; i++) {
        char path[96]; snprintf(path, sizeof(path), "virtualtextures/concurrent%zu.vmtr", i);
        create(path, "26\n0\n");
    }
    for (size_t i = 0; i < 4; i++) {
        readers[i].assets = sh_package_native_assets_retain(assets); readers[i].ready = ready;
        workers[i] = CreateThread(NULL, 0, read_concurrently, readers + i, 0, NULL); assert(workers[i]);
    }
    /* Drop the owner before workers start; their references must suffice. */
    sh_package_native_assets_close(assets);
    assert(SetEvent(ready));
    assert(WaitForMultipleObjects(4, workers, TRUE, 30000) == WAIT_OBJECT_0);
    for (size_t i = 0; i < 4; i++) {
        DWORD status; assert(GetExitCodeThread(workers[i], &status) && status == 0);
        assert(!memcmp(readers[0].content, readers[i].content, 32)); CloseHandle(workers[i]);
    }
    CloseHandle(ready);
}

static int probe(const char *directory)
{
    char error[1024]; sh_package_compilation compiled = {0};
    sh_package_native_assets *assets = sh_package_native_assets_open(directory);
    if (!assets) return 1;
    sh_package_library_native adapter = sh_package_native_assets_adapter(assets);
    sh_package_virtual_atlas first = {0}, again = {0}; ULONGLONG started = GetTickCount64();
    int ok = adapter.atlas(adapter.source.context, 0, &compiled, "probe", "", &first, error, sizeof(error));
    printf("native page capture: %llu ms\n", (unsigned long long)(GetTickCount64() - started));
    if (ok != 1) fprintf(stderr, "%s\n", error);
    started = GetTickCount64();
    for (size_t i = 0; ok == 1 && i < 32; i++) {
        ok = adapter.atlas(adapter.source.context, i, &compiled, "probe", "_vmtr", &again, error, sizeof(error));
        if (ok == 1 && memcmp(first.content, again.content, 32)) ok = -1;
    }
    printf("32 retained page bindings: %llu ms\n", (unsigned long long)(GetTickCount64() - started));
    sh_package_native_assets_close(assets); return ok == 1 ? 0 : 1;
}

static void intrinsic_images(void)
{
    /* Thirteen intrinsics, then images renderer and VT initialization create
     * by name and DeclFind's reserved empty name. */
    static const char *const names[] = {"_default", "_default3d", "_white", "_borderedpointwhite",
        "_black", "_defaultcube", "_nofalloff", "_dither2", "_dither4", "_dither16", "_dither256",
        "_fresnel", "_blackcube", "_vmtrpagetable", "_physicalvmtrpages0", "_physicalvmtrpages3",
        "_physicalvmtrmappings", "_transsortimageatlas", "_lightimageatlas", "_augment", "_emptyname"};
    enum { NAMES = sizeof(names) / sizeof(names[0]) };
    sh_package_library_native adapter = sh_package_native_assets_adapter(NULL);
    unsigned char identities[NAMES][32], again[32], zero[32] = {0}; char error[128];
    assert(adapter.source.admit);
    for (size_t i = 0; i < NAMES; i++) {
        assert(adapter.source.admit(NULL, 0, "image", names[i], identities[i], error, sizeof(error)) == 1);
        assert(adapter.source.admit(NULL, 999, "image", names[i], again, error, sizeof(error)) == 1);
        assert(!memcmp(identities[i], again, 32) && memcmp(again, zero, 32));
        for (size_t j = 0; j < i; j++) assert(memcmp(identities[i], identities[j], 32));
    }
    /* Everything outside the verified set stays unadmitted: menu images the
     * online services feed at run time, an unproven video index, pages the
     * Vmtr pool never creates, pools stock materials never name, and
     * non-canonical spellings. */
    static const char *const absent[] = {"_white.tga", "_white$borderclamp", "_flat", "_defaulted_",
        "_missing", "textures/_white", "_WHITE", "_video5", "_imagelivetile01", "_imagesnapmotd1",
        "_imagecompmpmotd1", "_savedgamethumb", "_physicalvmtrpages4", "_physicaluniquemappings",
        "_VMTRPAGETABLE", "_emptyName"};
    for (size_t i = 0; i < sizeof(absent) / sizeof(*absent); i++) {
        memset(again, 9, 32);
        assert(adapter.source.admit(NULL, 0, "image", absent[i], again, error, sizeof(error)) == 0);
        assert(!memcmp(again, zero, 32));
    }
    assert(adapter.source.admit(NULL, 0, "material", "_white", again, error, sizeof(error)) == 0);
    assert(adapter.source.admit(NULL, 0, "anim", "_defaulted_", again, error, sizeof(error)) == 0);
}

static void ordinary_original_semantics(void)
{
    static const char *const families[] = {"entitydef", "snapeditorentitydef", "model", "image", "material"};
    sh_package_library_native adapter = sh_package_native_assets_adapter(NULL);
    unsigned char semantics[32], zero[32] = {0}; char error[128];
    for (size_t i = 0; i < sizeof(families)/sizeof(*families); i++) {
        memset(semantics, 7, sizeof(semantics));
        assert(adapter.source.semantics(NULL, 0, families[i], "ordinary/source", semantics,
            error, sizeof(error)) == 1 && !memcmp(semantics, zero, sizeof(semantics)));
        /* Same-name file equality eligibility is never source-free admission. */
        assert(adapter.source.admit(NULL, 0, families[i], "ordinary/source", semantics,
            error, sizeof(error)) == 0);
    }
    assert(adapter.source.semantics(NULL, 0, NULL, "ordinary/source", semantics, error, sizeof(error)) == 0);
    assert(adapter.source.semantics(NULL, 0, "image", "", semantics, error, sizeof(error)) == 0);
}

static void font_semantics(void)
{
    /* A font's family meaning is the native hash of its name (R-309), so forms
     * dedup only within one hash: ASCII case is the same identity, while a name
     * loading the same directory with another spelling is not. */
    sh_package_library_native adapter = sh_package_native_assets_adapter(NULL);
    unsigned char a[32], b[32], c[32], zero[32] = {0}; char error[128];
    assert(adapter.source.semantics(NULL, 0, "font", "square721 ex tl", a, error, sizeof(error)) == 1);
    assert(adapter.source.semantics(NULL, 3, "font", "Square721 Ex TL", b, error, sizeof(error)) == 1);
    assert(adapter.source.semantics(NULL, 0, "font", "square721_ex_tl", c, error, sizeof(error)) == 1);
    assert(!memcmp(a, b, 32) && memcmp(a, c, 32) && memcmp(a, zero, 32) && memcmp(c, zero, 32));
}

static void sound_semantics(void)
{
    sh_package_library_native adapter = sh_package_native_assets_adapter(NULL);
    unsigned char a[32], b[32], c[32], zero[32] = {0}; char error[256];
    assert(adapter.source.semantics(NULL, 0, "sound", "play_sample", a, error, sizeof(error)) == 1);
    assert(adapter.source.semantics(NULL, 99, "sound", "effects/sample.wav", b, error, sizeof(error)) == 1);
    assert(!memcmp(a, b, 32) && memcmp(a, zero, 32));
    assert(adapter.source.semantics(NULL, 0, "sound", "stop_sample", c, error, sizeof(error)) == 1);
    assert(memcmp(a, c, 32));
    assert(adapter.source.semantics(NULL, 0, "sound", "play_\xff", c, error, sizeof(error)) == -1);
    assert(strstr(error, "event conversion"));
    assert(adapter.source.admit(NULL, 0, "sound", "play_sample", c, error, sizeof(error)) == 0);
}

static int consumer_available;
static int consumer_ready(const char *family) { assert(!strcmp(family, "font")); return consumer_available; }
static int consumer_other(const char *family) { (void)family; assert(0); return 0; }
static void constructor_admission(void)
{
    char error[256], directory[MAX_PATH];
    assert(GetCurrentDirectoryA(MAX_PATH, directory));
    sh_package_native_assets *assets = sh_package_native_assets_open(directory);
    assert(assets);
    sh_package_library_native adapter = sh_package_native_assets_adapter(assets);
    sh_package_link_unit font = {"font", "sample"};
    sh_package_source_implicit check = adapter.source.implicit;
    assert(check);
    assert(check(assets, 0, 1, &font, NULL, NULL, error, sizeof(error)) == 1);
    assert(check(assets, 0, 0, &font, NULL, NULL, error, sizeof(error)) == 0);
    assert(!sh_package_native_assets_configure_consumers(NULL, consumer_ready));
    assert(!sh_package_native_assets_configure_consumers(assets, NULL));
    assert(sh_package_native_assets_configure_consumers(assets, consumer_ready));
    assert(sh_package_native_assets_configure_consumers(assets, consumer_ready));
    assert(!sh_package_native_assets_configure_consumers(assets, consumer_other));
    assert(check(assets, 0, 0, &font, NULL, NULL, error, sizeof(error)) == 0);
    consumer_available = 1;
    assert(check(assets, 0, 0, &font, NULL, NULL, error, sizeof(error)) == 1);
    sh_package_native_assets *held = sh_package_native_assets_retain(assets);
    sh_package_native_assets_close(assets);
    adapter = sh_package_native_assets_adapter(held);
    assert(adapter.source.implicit(held, 0, 0, &font, NULL, NULL, error, sizeof(error)) == 1);
    consumer_available = 0;
    assert(adapter.source.implicit(held, 0, 0, &font, NULL, NULL, error, sizeof(error)) == 0);
    sh_package_native_assets_close(held);
}
/* Minimal valid mega2 member: one level-0 page of 17 payload bytes. */
enum { PAGE_LENGTH = 0x199 };
static void put32(unsigned char *p, uint32_t x)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(x >> (i * 8)); }
static void put64(unsigned char *p, uint64_t x)
{ for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(x >> (i * 8)); }
static void page(unsigned char bytes[PAGE_LENGTH], unsigned char payload)
{
    memset(bytes, 0, PAGE_LENGTH); put32(bytes, 0xa63fbb21u); put32(bytes + 4, 2);
    put32(bytes + 0xc, 12); put64(bytes + 0x38, 0x170); put64(bytes + 0x40, 0x180);
    put32(bytes + 0x48, 1); put32(bytes + 0x4c, 2);
    put32(bytes + 0x58, 2); put32(bytes + 0x5c, 1); put32(bytes + 0x64, 2);
    put64(bytes + 0x170, 0x188); put64(bytes + 0x178, 17);
    put32(bytes + 0x184, UINT32_MAX); memset(bytes + 0x188, payload, 17);
}
static void create_page(const char *relative, unsigned char payload)
{
    unsigned char bytes[PAGE_LENGTH]; char path[4096];
    page(bytes, payload); create(relative, "");
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, PAGE_LENGTH, file) == PAGE_LENGTH && !fclose(file));
}
/* R-347: native assets configured with a compile's catalog take their own reference to it. The catalog, and so the
 * archive handles that keep every writer out, outlive the opener's close while the assets hold it and go with the
 * assets; a refused configuration takes no reference, and a configured handle refuses another. */
static int shared_registry_read(void *context, uintptr_t address, void *destination, size_t length)
{ (void)context; (void)address; (void)destination; (void)length; return 0; }
static void shared_shader_catalog(void)
{
    static const char *const files[] = {"snap_gameresources.pindex", "gameresources.pindex",
        "snap_gameresources.resources", "snap_gameresources.patch", "gameresources.resources", "gameresources.patch"};
    unsigned char index[0x24] = {5, 'S', 'E', 'R', 0, 0, 0, 4}; /* no rows */
    char base[4096], path[4096], archive[4096], error[1024];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "shared-catalog/%s", files[i]); create(path, "");
        if (i >= 2) continue;
        snprintf(path, sizeof(path), "%s/shared-catalog/%s", root, files[i]);
        FILE *file = fopen(path, "wb"); assert(file);
        assert(fwrite(index, 1, sizeof(index), file) == sizeof(index) && !fclose(file));
    }
    snprintf(base, sizeof(base), "%s/shared-catalog", root);
    snprintf(archive, sizeof(archive), "%s/shared-catalog/gameresources.resources", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error)); assert(catalog);
    sh_native_resource_registry_source registry = {NULL, shared_registry_read, 1};
    sh_package_native_assets *assets = sh_package_native_assets_open(root), *refused = sh_package_native_assets_open(root);
    assert(assets && refused);
    assert(!sh_package_native_assets_configure_shader_catalog_shared(NULL, SH_PACKAGE_SHADER_VULKAN, registry, catalog,
        error, sizeof(error)));
    assert(!sh_package_native_assets_configure_shader_catalog_shared(assets, SH_PACKAGE_SHADER_VULKAN, registry, NULL,
        error, sizeof(error)));
    assert(!sh_package_native_assets_configure_shader_catalog_shared(refused, (sh_package_shader_renderer)7, registry,
        catalog, error, sizeof(error)) && strstr(error, "invalid shader native configuration"));
    sh_package_native_assets_close(refused);
    assert(sh_package_native_assets_configure_shader_catalog_shared(assets, SH_PACKAGE_SHADER_VULKAN, registry, catalog,
        error, sizeof(error)));
    assert(!sh_package_native_assets_configure_shader_catalog_shared(assets, SH_PACKAGE_SHADER_VULKAN, registry, catalog,
        error, sizeof(error)) && strstr(error, "fresh asset handle"));
    sh_resource_catalog_close(catalog);
    HANDLE writer = CreateFileA(archive, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    sh_package_native_assets_close(assets);
    writer = CreateFileA(archive, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(writer != INVALID_HANDLE_VALUE);
    CloseHandle(writer);
    fprintf(stderr, "shared shader catalog: held by the assets after its opener closed, released with them\n");
}
/* Row completeness of a private page family (na_page_row): a 4x4-page level-0
 * square of 120-texel pages. A member that omits a page the installed member
 * holds inside a bound material's row refuses the whole family, naming the
 * material and row; that family then binds to the engine's provider. */
enum { ROW_CELLS = 16, ROW_PAGES = 2, ROW_LENGTH = 0x170 + ROW_CELLS * 4 + ROW_PAGES * 16 + ROW_PAGES * 8 };
static void row_member(unsigned char bytes[ROW_LENGTH], const uint32_t cells[ROW_CELLS], unsigned char payload)
{
    memset(bytes, 0, ROW_LENGTH); put32(bytes, 0xa63fbb21u); put32(bytes + 4, 2); put32(bytes + 0xc, 12);
    put32(bytes + 0x10, 480); put32(bytes + 0x14, 480); put32(bytes + 0x18, 4); put32(bytes + 0x1c, 4);
    uint64_t index = 0x170, table = index + ROW_CELLS * 4, payloads = table + ROW_PAGES * 16;
    put64(bytes + 0x38, table); put64(bytes + 0x40, index); put32(bytes + 0x48, ROW_PAGES); put32(bytes + 0x4c, ROW_CELLS);
    put32(bytes + 0x58, 4); put32(bytes + 0x5c, 4); put32(bytes + 0x64, ROW_CELLS);
    for (unsigned i = 0; i < ROW_CELLS; i++) put32(bytes + index + i * 4, cells[i]);
    for (unsigned p = 0; p < ROW_PAGES; p++) {
        put64(bytes + table + p * 16, payloads + p * 8); put64(bytes + table + p * 16 + 8, 8);
        memset(bytes + payloads + p * 8, payload + p, 8);
    }
}
static void row_completeness(void)
{
    char rows_root[4096], path[4096], error[1024];
    uint32_t full[ROW_CELLS], swapped[ROW_CELLS], holed[ROW_CELLS];
    unsigned char installed[ROW_LENGTH], complete[ROW_LENGTH], missing[ROW_LENGTH];
    for (unsigned i = 0; i < ROW_CELLS; i++) { full[i] = i & 1; swapped[i] = !(i & 1); holed[i] = full[i]; }
    /* The row at texels (240, 0, 240, 240) covers cells x 2..3, y 0..1. */
    holed[3] = UINT32_MAX; holed[6] = UINT32_MAX;
    row_member(installed, full, 0x10); row_member(complete, swapped, 0x10); row_member(missing, holed, 0x10);
    snprintf(rows_root, sizeof(rows_root), "%s/rows", root); assert(CreateDirectoryA(rows_root, NULL));
    snprintf(path, sizeof(path), "%s/virtualtextures", rows_root); assert(CreateDirectoryA(path, NULL));
    snprintf(path, sizeof(path), "%s/virtualtextures/_vmtr_sq1.mega2", rows_root);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(installed, 1, ROW_LENGTH, file) == ROW_LENGTH && !fclose(file));
    sh_package_native_assets *assets = sh_package_native_assets_open(rows_root); assert(assets);
    sh_package_library_native adapter = sh_package_native_assets_adapter(assets);
    sh_package compiled_package = {0}; snprintf(compiled_package.name, sizeof(compiled_package.name), "zz-row-test");
    sh_package_sources sources = {0}; sources.packages = &compiled_package; sources.package_count = 1;
    sh_compiled_resource member = {0}; member.engine_path = "virtualtextures/_vmtr_sq1.mega2";
    assert(sh_package_owners_add(&member.owners, 0));
    sh_package_compilation compiled = {0}; compiled.sources = &sources;
    sh_virtual_material_record row = {"models/row", 240, 0, 240, 240, 0, 0, 0}, other = {"models/other", 0, 0, 240, 240, 0, 0, 0};
    sh_package_virtual_atlas atlas = {0};
    /* Control: a scope without a member binds the engine's provider; nothing to certify. */
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "models/row", "", &atlas, error, sizeof(error)) == 1);
    assert(atlas.original && !atlas.pages && !atlas.row_complete);
    /* A complete re-pointed member: every installed page of the row is served. */
    compiled.resources = &member; compiled.resource_count = 1;
    member.body = complete; member.body_length = ROW_LENGTH;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "models/row", "", &atlas, error, sizeof(error)) == 1);
    assert(!atlas.original && atlas.pages && atlas.row_complete);
    assert(atlas.row_complete(atlas.row_context, &compiled, atlas.pages, &row, "models/row", error, sizeof(error)) == 1);
    sh_virtual_page_source_release(atlas.pages); memset(&atlas, 0, sizeof(atlas));
    /* A member missing two of the row's pages: a row it does not touch still
     * passes; the reached row refuses the family and names material and row. */
    uint64_t before = sh_package_native_assets_page_refusals(NULL, 0);
    member.body = missing;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "models/row", "", &atlas, error, sizeof(error)) == 1);
    assert(atlas.pages && atlas.row_complete);
    assert(atlas.row_complete(atlas.row_context, &compiled, atlas.pages, &other, "models/other", error, sizeof(error)) == 1);
    assert(sh_package_native_assets_page_refusals(NULL, 0) == before);
    error[0] = 0;
    assert(atlas.row_complete(atlas.row_context, &compiled, atlas.pages, &row, "models/row", error, sizeof(error)) == 0);
    assert(strstr(error, "virtual texture member refused: package zz-row-test supplies virtualtextures/_vmtr_sq1.mega2 "
        "without row 240 0 240 240 that material models/row reads (2 of 4 level-0 pages missing)"));
    char reason[1024];
    assert(sh_package_native_assets_page_refusals(reason, sizeof(reason)) == before + 1 && !strcmp(reason, error));
    /* Input accounting finds the refusal by supplying package and member path only. */
    assert(sh_package_native_assets_page_refusal("zz-row-test", "virtualtextures/_vmtr_sq1.mega2", reason, sizeof(reason)) &&
        !strcmp(reason, error));
    assert(!sh_package_native_assets_page_refusal("zz-other", "virtualtextures/_vmtr_sq1.mega2", NULL, 0) &&
        !sh_package_native_assets_page_refusal("zz-row-test", "virtualtextures/_vmtr_sq2.mega2", NULL, 0));
    sh_virtual_page_source_release(atlas.pages); memset(&atlas, 0, sizeof(atlas));
    /* The refused family serves nothing: its materials keep the engine's provider. */
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "models/row", "", &atlas, error, sizeof(error)) == 1);
    assert(atlas.original && !atlas.pages && !atlas.row_complete);
    /* Other bytes are judged afresh. */
    member.body = complete;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "models/row", "", &atlas, error, sizeof(error)) == 1);
    assert(atlas.pages);
    sh_virtual_page_source_release(atlas.pages);
    sh_package_owners_free(&member.owners);
    sh_package_native_assets_close(assets);
    assert(DeleteFileA(path));
    snprintf(path, sizeof(path), "%s/virtualtextures", rows_root);
    assert(RemoveDirectoryA(path) && RemoveDirectoryA(rows_root));
    puts("row completeness: missing reached pages refuse the family and name the material and row");
}
int main(int argc, char **argv)
{
    if (argc == 2) return probe(argv[1]);
    constructor_admission();
    intrinsic_images();
    sound_semantics();
    font_semantics();
    ordinary_original_semantics();
    char temporary[MAX_PATH], absolute[4096], error[1024];
    sh_package_compilation compiled = {0}; sh_package_virtual_atlas first = {0}, again = {0};
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-native-assets-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    create("virtualtextures/_vmtr.vmtr", "26\n0\n");
    create_page("virtualtextures/_vmtr_sq1.mega2", 0x11);
    create_page("virtualtextures/_vmtr_sq16.mega2", 0x22);
    create_page("virtualtextures/_vmtr_q4.mega2", 0x33);
    shared_shader_catalog();
    unsigned char first_bytes[PAGE_LENGTH], changed_bytes[PAGE_LENGTH], other_bytes[PAGE_LENGTH], second_bytes[PAGE_LENGTH];
    page(first_bytes, 0x11); page(changed_bytes, 0x44); page(other_bytes, 0x55); page(second_bytes, 0x66);
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_native_assets *retained = sh_package_native_assets_retain(assets); assert(retained == assets);
    sh_package_library_native adapter = sh_package_native_assets_adapter(assets);
    unsigned char *body = NULL; size_t length = 0;
    assert(sh_package_native_assets_read(assets, "generated/decls/material/unrelated.decl", &body, &length) == 0);
    assert(sh_package_native_assets_read(assets, "virtualtextures/../_vmtr.vmtr", &body, &length) == -1);
    assert(sh_package_native_assets_read(assets, "virtualtextures/_vmtr.vmtr", &body, &length) == 1);
    assert(length == 5 && !memcmp(body, "26\n0\n", 5)); free(body);
    assert(sh_package_native_assets_read(assets, "virtualtextures/_vmtr_dlc5.vmtr", &body, &length) == 0);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &first, error, sizeof(error)) == 1);
    assert(first.original && !strcmp(first.name, "_vmtr") && !strcmp(first.source, "virtualtextures/_vmtr"));
    assert(adapter.atlas(adapter.source.context, 99, &compiled, "sample", "_VMTR", &again, error, sizeof(error)) == 1);
    assert(!memcmp(first.content, again.content, 32));
    snprintf(absolute, sizeof(absolute), "%s/virtualtextures/_vmtr_sq1.mega2", root);
    HANDLE writer = CreateFileA(absolute, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    assert(!DeleteFileA(absolute) && GetLastError() == ERROR_SHARING_VIOLATION);
    /* Another page source names the engine's own provider (a palette's megatexture, na_atlas): its nominal identity
     * certifies it while no scope supplies a file of that family; a supplied member of the family still needs a
     * private provider of its own. */
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "custom", &again, error, sizeof(error)) == 1);
    assert(again.original && !strcmp(again.name, "custom") && !strcmp(again.source, "virtualtextures/custom"));
    {
        static const char nominal_name[] = "doom2016:engine-owned-page-provider:custom";
        sh_package_file_identity nominal;
        assert(sh_package_bytes_identity(nominal_name, strlen(nominal_name), &nominal) &&
            !memcmp(again.content, nominal.digest, 32) && memcmp(again.content, first.content, 32));
        sh_compiled_resource custom = {0}; custom.engine_path = "virtualtextures/custom_sq1.mega2";
        compiled.resources = &custom; compiled.resource_count = 1;
        assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "custom", &again, error, sizeof(error)) == 0);
        assert(strstr(error, "private native page provider requires preparation: custom"));
        compiled.resources = NULL; compiled.resource_count = 0;
    }
    sh_compiled_resource override = {0}; override.engine_path = "virtualtextures/_vmtr_sq1.mega2";
    override.body = changed_bytes; override.body_length = PAGE_LENGTH;
    compiled.resources = &override; compiled.resource_count = 1;
    /* Changed page data captures the scope's complete family for a private
     * kind-2 provider in the unique-VMTR mode: the authored member, and every
     * other member from the installed originals. */
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!again.original && again.pages && again.page_kind == 2 && again.page_mode == 2);
    assert(!strcmp(again.name, "_vmtr") && !strcmp(again.source, "virtualtextures/_vmtr"));
    assert(!memcmp(again.content, sh_virtual_page_source_identity(again.pages), 32) && memcmp(again.content, first.content, 32));
    assert(sh_virtual_page_source_member(again.pages, 0)->body && sh_virtual_page_source_member(again.pages, 15) &&
        sh_virtual_page_source_member(again.pages, 19) && !sh_virtual_page_source_member(again.pages, 1) &&
        !sh_virtual_page_source_member(again.pages, 20));
    {
        /* Every material of the scope shares one capture. */
        sh_package_virtual_atlas shared = {0};
        assert(adapter.atlas(adapter.source.context, 0, &compiled, "another", "_vmtr", &shared, error, sizeof(error)) == 1);
        assert(shared.pages == again.pages && !memcmp(shared.content, again.content, 32));
        sh_virtual_page_source_release(shared.pages);
    }
    sh_virtual_page_source_release(again.pages); memset(&again, 0, sizeof(again));
    /* An exact authored copy belongs to the same native provider. Distinct
     * same-size bytes still need isolation; metadata equality is not dedupe. */
    override.body = first_bytes;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(again.original && !again.pages && !memcmp(first.content, again.content, 32));
    override.body = other_bytes;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!again.original && again.pages);
    {
        unsigned char other[32]; memcpy(other, again.content, 32);
        sh_virtual_page_source_release(again.pages); memset(&again, 0, sizeof(again));
        override.body = changed_bytes;
        assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
        assert(again.pages && memcmp(other, again.content, 32));
        sh_virtual_page_source_release(again.pages); memset(&again, 0, sizeof(again));
        /* A member that is not valid mega2 cannot become a private family. */
        override.body = (unsigned char *)"not a mega2 member"; override.body_length = 18;
        assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == -1 && !again.pages);
        override.body_length = PAGE_LENGTH;
    }
    compiled.resource_count = 0;
    sh_package_native_assets_close(assets);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!memcmp(first.content, again.content, 32));
    sh_package_native_assets_close(retained);

    /* The original provider is one engine-owned object. Its nominal key is
     * stable across captures, but authored-copy comparisons use fresh bytes. */
    create_page("virtualtextures/_vmtr_sq1.mega2", 0x66);
    assets = sh_package_native_assets_open(root); assert(assets);
    adapter = sh_package_native_assets_adapter(assets);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!memcmp(first.content, again.content, 32));
    compiled.resource_count = 1;
    override.body = first_bytes;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!again.original && again.pages);
    sh_virtual_page_source_release(again.pages); memset(&again, 0, sizeof(again));
    override.body = second_bytes;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(again.original && !again.pages);
    compiled.resource_count = 0;
    sh_package_native_assets_close(assets);

    /* Operational sharing failures can be retried without a game restart. */
    writer = CreateFileA(absolute, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); assert(writer != INVALID_HANDLE_VALUE);
    assets = sh_package_native_assets_open(root); assert(assets);
    adapter = sh_package_native_assets_adapter(assets);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == -1);
    CloseHandle(writer);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    sh_package_native_assets_close(assets);
    concurrent_capture();
    row_completeness();
    cleanup(); assert(!failures); puts("package_native_assets_test: PASS"); return 0;
}
