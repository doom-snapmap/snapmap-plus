/* Source preservation, nested ownership, native paths and stale-read refusal. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_sources.h"

#define PACKAGE_FIXTURE_ENTRIES 16384u
#include "package_fixture.h"

static const sh_package_source_file *find(const sh_package_sources *sources, const char *package, const char *relative)
{
    size_t i;
    for (i = 0; sources && i < sources->file_count; i++) {
        const sh_package_source_file *file = &sources->files[i];
        if (!strcmp(sources->packages[file->owner].name, package) && !strcmp(file->relative, relative)) return file;
    }
    CHECK(0); return NULL;
}

static void paths(void)
{
    const char *bad[] = {"", "/root", "../x", "a/../b", "a//b", "a/./b", "a:stream", "a/NUL.decl", "x/COM1", "x/", "x./b", "x /b", "x?b"};
    size_t i;
    char *normal = sh_package_engine_path("Generated\\BaseModel\\DEMON.bmd6model");
    CHECK(normal && !strcmp(normal, "generated/basemodel/demon.bmd6model")); free(normal);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) { normal = sh_package_engine_path(bad[i]); CHECK(!normal); free(normal); }
}

static void scale_inventory(void)
{
    char path[2048] = "overrides/deep", error[512], descriptor[128];
    sh_package_sources *sources;
    const sh_package_source_file *file;
    size_t i, nested;
    create("overrides/deep/package.json", "{\"id\":\"deep\",\"name\":\"Deep package\"}");
    for (i = 0; i < 128; i++) strcat_s(path, sizeof(path), "/d");
    nested = strlen(path);
    strcat_s(path, sizeof(path), "/package.json");
    create(path, "{\"id\":\"deep.nested\",\"name\":\"Deep component\"}");
    path[nested] = 0; strcat_s(path, sizeof(path), "/assets/package.json");
    /* A file in assets must not become a component, even at this depth. */
    create(path, "ordinary engine data");
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "deep inventory: %s\n", error);
    CHECK(sources);
    if (sources) {
        CHECK(sources->package_count == 1 && sources->component_count == 2);
        CHECK(sources->file_count == 132);
        file = find(sources, "deep", path + strlen("overrides/deep/"));
        CHECK(file && file->engine_path && !strcmp(file->engine_path, "package.json"));
        CHECK(file && !strcmp(sources->components[file->component].descriptor.id, "deep.nested"));
    }
    sh_package_sources_free(sources);
    for (i = 0; i < 2050; i++) {
        snprintf(path, sizeof(path), "overrides/deep/components/c%04zu/package.json", i);
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"component.%zu\",\"name\":\"Component\"}", i);
        create(path, descriptor);
        snprintf(path, sizeof(path), "overrides/deep/components/c%04zu/assets/data.bin", i);
        create(path, "same shared bytes");
    }
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "wide inventory: %s\n", error);
    CHECK(sources);
    if (sources) {
        CHECK(sources->component_count == 2052);
        CHECK(sources->file_count == 8333);
        for (i = 0; i < 2050; i++) {
            snprintf(path, sizeof(path), "components/c%04zu/assets/data.bin", i);
            file = find(sources, "deep", path);
            snprintf(descriptor, sizeof(descriptor), "component.%zu", i);
            CHECK(file && file->owner == 0 && !strcmp(file->engine_path, "data.bin"));
            CHECK(file && !strcmp(sources->components[file->component].descriptor.id, descriptor));
        }
    }
    sh_package_sources_free(sources);
    create("overrides/deep/components/c2049/package.json", "invalid descriptor");
    sources = sh_package_sources_scan(root, error, sizeof(error));
    CHECK(!sources && error[0]); sh_package_sources_free(sources);
}

/* Optional disk-intensive acceptance probe. Every generated file lives in a
 * newly created fixture folder and is removed individually by its exact name. */
static void large_inventory(void)
{
    char path[4096], error[512];
    sh_package_sources *sources;
    size_t i, count = 0;
    create("overrides/large/package.json", "{\"id\":\"large\",\"name\":\"Large package\"}");
    create("overrides/large/assets", NULL);
    for (i = 0; i < 131073; i++) {
        wchar_t *wide;
        HANDLE file;
        DWORD written;
        snprintf(path, sizeof(path), "%s/overrides/large/assets/f%06zu.bin", root, i);
        wide = sh_package_source_wide_path(path); CHECK(wide);
        if (!wide) break;
        file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wide); CHECK(file != INVALID_HANDLE_VALUE);
        if (file == INVALID_HANDLE_VALUE) break;
        count++;
        CHECK(WriteFile(file, &i, sizeof(i), &written, NULL) && written == sizeof(i));
        CloseHandle(file);
    }
    CHECK(count == 131073);
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "large inventory: %s\n", error);
    CHECK(sources);
    if (sources) {
        CHECK(sources->file_count == count + 2u);
        for (i = 0; i < sources->file_count; i++) {
            const sh_package_source_file *file = &sources->files[i];
            unsigned char *body;
            size_t length = 0, value = SIZE_MAX;
            if (!file->engine_path) continue;
            body = sh_package_source_read(file, sizeof(value), &length, error, sizeof(error));
            if (body && length == sizeof(value)) memcpy(&value, body, sizeof(value));
            snprintf(path, sizeof(path), "f%06zu.bin", value);
            CHECK(body && length == sizeof(value) && value < count && !strcmp(file->engine_path, path));
            free(body);
        }
        printf("large inventory: %zu authored files verified\n", count);
    }
    sh_package_sources_free(sources);
    for (i = 0; i < count; i++) {
        wchar_t *wide;
        snprintf(path, sizeof(path), "%s/overrides/large/assets/f%06zu.bin", root, i);
        wide = sh_package_source_wide_path(path); CHECK(wide);
        if (wide) { CHECK(DeleteFileW(wide)); free(wide); }
    }
}

static void large_verified_stream(void)
{
    const uint64_t end = UINT64_C(0x100000010);
    const char *digest = "affb9d82acfa9ec337dbce3043f7499cf08082025f41f91c951054a5955d60e1";
    char path[4096], error[512], bytes[4];
    sh_package_source_file source = {0};
    HANDLE handle;
    DWORD transferred = 0;
    LARGE_INTEGER offset;
    FILE *stream;
    size_t i;
    int sparse;
    create("large-stream.bin", "head");
    snprintf(path, sizeof(path), "%s/large-stream.bin", root);
    handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle == INVALID_HANDLE_VALUE) return;
    sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &transferred, NULL) != 0;
    CHECK(sparse);
    if (!sparse) { CloseHandle(handle); return; }
    offset.QuadPart = (LONGLONG)end;
    CHECK(SetFilePointerEx(handle, offset, NULL, FILE_BEGIN));
    CHECK(WriteFile(handle, "tail", 4, &transferred, NULL) && transferred == 4);
    CloseHandle(handle);
    source.absolute = path; source.relative = "large-stream.bin"; source.length = end + 4;
    for (i = 0; i < 32; i++) {
        unsigned int value = 0;
        CHECK(sscanf_s(digest + 2 * i, "%2x", &value) == 1);
        source.digest[i] = (unsigned char)value;
    }
    /* Independently calculated SHA256 for head + sparse zeros + tail. Verify
     * and keep one handle; no allocation proportional to this 4 GiB file. */
    stream = sh_package_source_open(&source, error, sizeof(error));
    if (!stream) fprintf(stderr, "large verified stream: %s\n", error);
    CHECK(stream);
    if (!stream) return;
    CHECK(fread(bytes, 1, 4, stream) == 4 && !memcmp(bytes, "head", 4));
    CHECK(!_fseeki64(stream, 0x80000000LL, SEEK_SET));
    CHECK(fread(bytes, 1, 4, stream) == 4 && !memcmp(bytes, "\0\0\0\0", 4));
    CHECK(!_fseeki64(stream, (long long)end, SEEK_SET));
    CHECK(fread(bytes, 1, 4, stream) == 4 && !memcmp(bytes, "tail", 4));
    CHECK(_ftelli64(stream) == (long long)source.length && fgetc(stream) == EOF);
    fclose(stream);
    printf("verified file stream: %llu bytes, 64-bit seeks and exact digest\n", (unsigned long long)source.length);
    {
        sh_package_file *sealed=sh_package_file_seal(&source,error,sizeof(error));
        sh_package_file_identity identity;
        uint64_t size; ULONGLONG started;
        CHECK(sealed);if(!sealed)return;
        CHECK(sh_package_file_get_identity(sealed,&identity));
        CHECK(identity.length==source.length && !memcmp(identity.digest,source.digest,32));
        started=GetTickCount64();
        for(unsigned n=0;n<64;++n) {
            stream=sh_package_file_open(sealed,&size,error,sizeof(error));CHECK(stream && size==source.length);
            if(stream) {
                CHECK(!_fseeki64(stream,(long long)end,SEEK_SET));
                CHECK(fread(bytes,1,4,stream)==4 && !memcmp(bytes,"tail",4));fclose(stream);
            }
        }
        printf("sealed file stream: 64 independent opens of %llu bytes in %llu ms after verification\n",
            (unsigned long long)source.length,(unsigned long long)(GetTickCount64()-started));
        sh_package_file_release(sealed);
    }
}

static void sealed_streams(const sh_package_source_file *source)
{
    char error[256]; uint64_t size=0; size_t length=0;
    sh_package_file *sealed=sh_package_file_seal(source,error,sizeof(error)), *retained;
    FILE *first=NULL,*second=NULL; wchar_t *wide=sh_package_source_wide_path(source->absolute);
    HANDLE writer; unsigned char *body;
    sh_package_file_identity identity, memory;
    CHECK(sealed && wide);if(!sealed || !wide){sh_package_file_release(sealed);free(wide);return;}
    CHECK(sh_package_file_get_identity(sealed,&identity));
    CHECK(identity.length==source->length && !memcmp(identity.digest,source->digest,32));
    retained=sh_package_file_retain(sealed);
    first=sh_package_file_open(sealed,&size,error,sizeof(error));CHECK(first && size==source->length);
    second=sh_package_file_open(retained,&size,error,sizeof(error));CHECK(second && size==source->length);
    if(first && second) {
        CHECK(!setvbuf(first,NULL,_IONBF,0));CHECK(!setvbuf(second,NULL,_IONBF,0));
        CHECK(fgetc(first)=='{');CHECK(fgetc(first)==' ');CHECK(fgetc(second)=='{');
        CHECK(!_fseeki64(second,-3,SEEK_END));CHECK(fgetc(first)=='x');
    }
    body=sh_package_file_read(sealed,(size_t)source->length,&length,error,sizeof(error));
    CHECK(body && length==source->length && body[length]==0);
    CHECK(sh_package_bytes_identity(body,length,&memory));
    CHECK(memory.length==identity.length && !memcmp(memory.digest,identity.digest,32));free(body);
    body=sh_package_file_read(sealed,1,&length,error,sizeof(error));CHECK(!body && !length);
    writer=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    CHECK(!DeleteFileW(wide));
    sh_package_file_release(sealed);sh_package_file_release(retained);
    /* Independent readers keep the verified bytes protected after compilation
     * ownership ends, without borrowing a seal or source-inventory pointer. */
    CHECK(!DeleteFileW(wide));
    if(first)fclose(first);
    CHECK(!DeleteFileW(wide));
    if(second) { CHECK(fgetc(second)=='}');fclose(second); }
    writer=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer!=INVALID_HANDLE_VALUE);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    {
        sh_package_source_file changed=*source;changed.digest[0]^=1;
        sealed=sh_package_file_seal(&changed,error,sizeof(error));CHECK(!sealed);sh_package_file_release(sealed);
    }
    sealed=sh_package_file_capture(source->absolute,error,sizeof(error));CHECK(sealed);
    CHECK(sh_package_file_get_identity(sealed,&identity));
    CHECK(identity.length==source->length && !memcmp(identity.digest,source->digest,32));
    writer=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    sh_package_file_release(sealed);
    sealed=sh_package_file_capture(NULL,error,sizeof(error));CHECK(!sealed && error[0]);
    sh_package_file_release(sealed);
    CHECK(!sh_package_file_get_identity(NULL,&identity) && !identity.length);
    for(size_t i=0;i<sizeof(identity.digest);i++)CHECK(!identity.digest[i]);
    free(wide);
}

static void memory_identities(void)
{
    static const unsigned char abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,
        0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    static const unsigned char empty[32]={0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,
        0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    sh_package_file_identity identity;
    CHECK(sh_package_bytes_identity("abc",3,&identity));
    CHECK(identity.length==3 && !memcmp(identity.digest,abc,32));
    CHECK(sh_package_bytes_identity(NULL,0,&identity));
    CHECK(!identity.length && !memcmp(identity.digest,empty,32));
    CHECK(!sh_package_bytes_identity(NULL,1,&identity) && !identity.length);
    for(size_t i=0;i<sizeof(identity.digest);i++)CHECK(!identity.digest[i]);
    CHECK(!sh_package_bytes_identity("abc",3,NULL));
}

int main(int argc, char **argv)
{
    char temp[MAX_PATH], error[512], long_path[1024];
    sh_package_sources *sources, *again;
    const sh_package_source_file *cyber, *duplicate, *hell, *shader;
    unsigned char *body;
    size_t length, i;
    const char *descriptor = "{\"id\":\"test.cyberdemon\",\"name\":\"Cyberdemon\",\"strings\":{\"en\":{\"npc\":\"Cyberdemon\"}}}\n";
    if (argc == 2 && strcmp(argv[1], "--large")) {
        sources = sh_package_sources_scan(argv[1], error, sizeof(error));
        if (!sources) { fprintf(stderr, "%s\n", error); return 1; }
        printf("%zu packages, %zu components, %zu exact source entries\n", sources->package_count, sources->component_count, sources->file_count);
        for (i = 0; i < sources->file_count; i++) if (sources->files[i].engine_path) printf("%s\t%s\n", sources->packages[sources->files[i].owner].name, sources->files[i].engine_path);
        sh_package_sources_free(sources); return 0;
    }
    paths(); memory_identities(); CHECK(GetTempPathA(sizeof(temp), temp));
    CHECK(GetTempFileNameA(temp, "ps2", 0, root)); CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    if (argc == 2) { large_inventory(); cleanup(); return failures ? 1 : 0; }
    large_verified_stream();
    create("overrides/cyberdemon/package.json", descriptor);
    create("overrides/cyberdemon/assets/generated/decls/entityDef/demon/cyber.decl", "{ x = 1; }\r\n");
    create("overrides/boss-demons/package.json", "{\"id\":\"test.bosses\",\"name\":\"Boss demons\"}");
    create("overrides/boss-demons/cyberdemon/package.json", descriptor);
    create("overrides/boss-demons/cyberdemon/assets/generated/decls/entityDef/demon/cyber.decl", "{ x = 1; }\r\n");
    create("overrides/boss-demons/hell_guard/package.json", "{\"id\":\"test.hellguard\",\"name\":\"Hell Guard\"}");
    create("overrides/boss-demons/hell_guard/assets/cooked/model/hell_guard.bmodel", "native model bytes");
    create("overrides/boss-demons/README.md", "authored documentation\r\n");
    create("overrides/boss-demons/empty", NULL);
    create("overrides/editor/package.json", "{\"id\":\"test.editor\",\"name\":\"Editor\"}");
    create("overrides/editor/assets/generated/decls/entitydef/example.decl", "{ x = 1; }");
    create("overrides/editor/assets/generated/spirv/a/b/c.vspv", "shader");
    snprintf(long_path, sizeof(long_path), "overrides/boss-demons/hell_guard/assets/generated/spirv/%s/%s/%s/demon.fspv",
        "01234567890123456789012345678901234567890123456789012345678901234567890123456789",
        "01234567890123456789012345678901234567890123456789012345678901234567890123456789",
        "01234567890123456789012345678901234567890123456789012345678901234567890123456789");
    create(long_path, "long shader");
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "%s\n", error);
    CHECK(sources);
    if (!sources) goto done;
    CHECK(sources->package_count == 3); CHECK(sources->component_count == 5);
    cyber = find(sources, "cyberdemon", "assets/generated/decls/entityDef/demon/cyber.decl");
    duplicate = find(sources, "boss-demons", "cyberdemon/assets/generated/decls/entityDef/demon/cyber.decl");
    hell = find(sources, "boss-demons", "hell_guard/assets/cooked/model/hell_guard.bmodel");
    shader = find(sources, "editor", "assets/generated/spirv/a/b/c.vspv");
    CHECK(cyber && duplicate && hell && shader);
    if(cyber)sealed_streams(cyber);
    if (cyber && duplicate && hell && shader) {
        CHECK(cyber->owner != duplicate->owner);
        CHECK(!strcmp(cyber->engine_path, duplicate->engine_path));
        CHECK(!strcmp(cyber->engine_path, "generated/decls/entitydef/demon/cyber.decl"));
        CHECK(!strcmp(hell->engine_path, "cooked/model/hell_guard.bmodel"));
        CHECK(!strcmp(shader->engine_path, "generated/spirv/a/b/c.vspv"));
        CHECK(!memcmp(cyber->digest, duplicate->digest, 32));
        body = sh_package_source_read(cyber, 1024u, &length, error, sizeof(error));
        CHECK(body && length == strlen("{ x = 1; }\r\n") && !memcmp(body, "{ x = 1; }\r\n", length)); free(body);
        {
            char bytes[32] = {0};
            FILE *stream = sh_package_source_open(cyber, error, sizeof(error));
            HANDLE writer;
            wchar_t *wide = sh_package_source_wide_path(cyber->absolute);
            CHECK(stream && wide);
            if (stream) {
                CHECK(fread(bytes, 1, sizeof(bytes), stream) == cyber->length);
                CHECK(!strcmp(bytes, "{ x = 1; }\r\n"));
                CHECK(!_fseeki64(stream, 6, SEEK_SET) && _ftelli64(stream) == 6);
                CHECK(fgetc(stream) == '1');
                if (wide) {
                    writer = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    CHECK(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
                    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
                    CHECK(!DeleteFileW(wide) && GetLastError() == ERROR_SHARING_VIOLATION);
                }
                fclose(stream);
            }
            free(wide);
        }
        again = sh_package_sources_scan(root, error, sizeof(error)); CHECK(again);
        if (again) CHECK(!memcmp(sources->fingerprints, again->fingerprints, sources->package_count * 32));
        sh_package_sources_free(again);
        create("overrides/cyberdemon/assets/generated/decls/entityDef/demon/cyber.decl", "{ x = 2; }\r\n");
        body = sh_package_source_read(cyber, 1024u, &length, error, sizeof(error)); CHECK(!body); CHECK(strstr(error, "changed")); free(body);
        {
            FILE *stream = sh_package_source_open(cyber, error, sizeof(error));
            CHECK(!stream && strstr(error, "changed"));
            if (stream) fclose(stream);
        }
        body = sh_package_source_read(duplicate, 1024u, &length, error, sizeof(error)); CHECK(body); free(body);
    }
    CHECK(!find(sources, "boss-demons", "README.md")->engine_path);
    CHECK(find(sources, "boss-demons", "empty")->directory);
    sh_package_sources_free(sources); sources = NULL;
    create("overrides/boss-demons/hell_guard/package.json", "{\"schema\":\"snapmap-plus.package.v2\"}");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(!sources); CHECK(error[0]);
done:
    sh_package_sources_free(sources);
    cleanup();
    CHECK(GetTempFileNameA(temp, "ps2", 0, root)); CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    scale_inventory(); cleanup();
    if (failures) return 1;
    puts("package source inventory checks passed"); return 0;
}
