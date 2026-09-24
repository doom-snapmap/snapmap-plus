#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    create("virtualtextures/_vmtr_sq1.mega2", "first synthetic immutable pages");
    create("virtualtextures/_vmtr_sq16.mega2", "last high quality chunk");
    create("virtualtextures/_vmtr_q4.mega2", "last low quality chunk");
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
    override.body = (unsigned char *)"different synthetic immutable pages";
    override.body_length = strlen((char *)override.body);
    compiled.resources = &override; compiled.resource_count = 1;
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 0);
    assert(strstr(error, "private provider"));
    /* An exact authored copy belongs to the same native provider. Distinct
     * same-size bytes still need isolation; metadata equality is not dedupe. */
    override.body = (unsigned char *)"first synthetic immutable pages";
    override.body_length = strlen((char *)override.body);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(again.original && !memcmp(first.content, again.content, 32));
    override.body = (unsigned char *)"other synthetic immutable pages";
    assert(strlen((char *)override.body) == override.body_length);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 0);
    compiled.resource_count = 0;
    sh_package_native_assets_close(assets);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!memcmp(first.content, again.content, 32));
    sh_package_native_assets_close(retained);

    /* The original provider is one engine-owned object. Its nominal key is
     * stable across captures, but authored-copy comparisons use fresh bytes. */
    create("virtualtextures/_vmtr_sq1.mega2", "second synthetic immutable pages");
    assets = sh_package_native_assets_open(root); assert(assets);
    adapter = sh_package_native_assets_adapter(assets);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
    assert(!memcmp(first.content, again.content, 32));
    compiled.resource_count = 1;
    override.body = (unsigned char *)"first synthetic immutable pages";
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 0);
    override.body = (unsigned char *)"second synthetic immutable pages";
    override.body_length = strlen((char *)override.body);
    assert(adapter.atlas(adapter.source.context, 0, &compiled, "sample", "", &again, error, sizeof(error)) == 1);
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
