#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "package_native_assets.h"
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
int main(int argc, char **argv)
{
    if (argc == 2) return probe(argv[1]);
    constructor_admission();
    intrinsic_images();
    sound_semantics();
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
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "custom", &again, error, sizeof(error)) == 0);
    assert(strstr(error, "private native page provider"));
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
    cleanup(); assert(!failures); puts("package_native_assets_test: PASS"); return 0;
}
