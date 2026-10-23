/* Immutable native-source retries through the real runtime lock boundary. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "package_native_assets.h"
#include "package_shader_receipt.h"
#include "native_resource_id.h"
#include "package_fixture.h"

typedef struct native_fixture {
    unsigned char head[8], manager[0x30], items[8], object[0x90];
    char type[16], name[16];
    sh_package_native_assets *assets;
    int ready, mode, calls, recoveries;
    const sh_package_compilation *published;
} native_fixture;
static native_fixture fixture;
static const char parameter[] = "{ Tex2D textures/fixture }";
static const char descriptor[] = "{\"id\":\"fixture\",\"name\":\"Fixture\"}";
void backend_log(const char *message) { (void)message; }
static void pointer(void *object, size_t at, const void *value)
{ memcpy((char *)object + at, &value, sizeof(value)); }
static void number(void *object, size_t at, uint32_t value)
{ memcpy((char *)object + at, &value, sizeof(value)); }
static int native_read(void *context, uintptr_t address, void *out, size_t length)
{
    native_fixture *f = context;
    uintptr_t start = (uintptr_t)f;
    if (address < start || address - start > sizeof(*f) || length > sizeof(*f) - (size_t)(address - start)) return 0;
    memcpy(out, (const void *)address, length); return 1;
}
static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/renderparm/tex.decl")) return 0;
    *body = (unsigned char *)_strdup(parameter); *length = strlen(parameter); return *body ? 1 : -1;
}
static void initialize(void)
{
    char error[512] = "";
    uint32_t id = 0;
    memset(&fixture, 0, sizeof(fixture)); fixture.ready = 1;
    strcpy(fixture.type, "renderParm"); strcpy(fixture.name, "tex");
    pointer(fixture.head, 0, fixture.manager);
    pointer(fixture.manager, 8, fixture.type); pointer(fixture.manager, 0x20, fixture.items);
    number(fixture.manager, 0x28, 1); pointer(fixture.items, 0, fixture.object);
    pointer(fixture.object, 8, fixture.name); pointer(fixture.object, 0x10, fixture.manager);
    CHECK(sh_native_resource_id(fixture.type, fixture.name, &id)); number(fixture.object, 0x20, id);
    number(fixture.object, 0x68, 0x410); number(fixture.object, 0x6c, 1);
    fixture.assets = sh_package_native_assets_open(root); CHECK(fixture.assets);
    CHECK(sh_package_native_assets_configure_shader(fixture.assets, SH_PACKAGE_SHADER_OPENGL,
        (sh_native_resource_registry_source){&fixture, native_read, (uintptr_t)fixture.head},
        original, &fixture, error, sizeof(error)));
}
static void enqueue(void)
{
    char error[512] = ""; int kind = 0; uint32_t index = 0;
    sh_package_shader_receipt_invalidate((uintptr_t)fixture.object);
    sh_package_library_native native = sh_package_native_assets_adapter(fixture.assets);
    const sh_package_source_shader *shader = native.source.shader;
    /* 2: a preparation request was retained; the caller retries after it. */
    CHECK(shader && shader->parameter(shader->context, "tex", (sh_decl_source){parameter, strlen(parameter)},
        &kind, &index, error, sizeof(error)) == 2);
    CHECK(strstr(error, "environment needs") && sh_package_native_assets_shader_pending(fixture.assets) == 1);
}
static int prepare_one(void *context, const sh_package_shader_request *request, char *error, size_t capacity)
{
    native_fixture *f = context; (void)error; (void)capacity;
    CHECK(request->resource == (uintptr_t)f->object && request->task_id);
    CHECK(request->source_length == strlen(parameter) && !memcmp(request->source, parameter, strlen(parameter)));
    if (f->mode == 1) return 1; /* Claiming success cannot manufacture a receipt. */
    sh_package_shader_receipt_frame receipt;
    sh_package_shader_receipt_begin(&receipt, request->resource, request->family, request->name);
    sh_package_shader_receipt_retained(request->source, request->source_length);
    if (f->mode >= 2) f->object[0x58] = 19;
    sh_package_shader_receipt_end(&receipt, f->mode == 0, native_read, f);
    return f->mode == 0;
}
static int recover(void *context, const sh_package_shader_preparation *batch, char *error, size_t capacity)
{
    native_fixture *f = context; (void)error; (void)capacity; f->recoveries++;
    CHECK(sh_package_shader_preparation_assets(batch) == f->assets);
    if (f->mode == 4) return 0;
    f->object[0x58] = 0; sh_package_shader_receipt_invalidate((uintptr_t)f->object); return 1;
}
int sh_package_runtime_test_native_ready(void) { return fixture.ready; }
int sh_package_runtime_test_native_run(sh_package_shader_preparation *batch, char *error, size_t capacity)
{
    fixture.calls++;
    CHECK(!sh_package_runtime_admission_ready());
    const sh_package_compilation *published = sh_package_runtime_acquire();
    CHECK(published == fixture.published); sh_package_runtime_release();
    /* A private candidate's native read must never inherit the published
     * view's identity, including a name absent from that older compilation. */
    sh_package_resource_capture *capture = NULL;
    char capture_error[256] = "";
    CHECK(sh_package_runtime_resource_capture("renderparm", "tex", &capture,
        capture_error, sizeof(capture_error)) == -1);
    CHECK(!capture && capture_error[0]);
    /* Both locks are released. A nested refresh/selection is refused without
     * changing the current provider or corrupting its readiness. */
    CHECK(!sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    CHECK(!sh_package_runtime_select_map(NULL, 0, NULL, error, capacity));
    *error = 0;
    create("overrides/fixture/package.json", "external edit after immutable capture");
    if (fixture.mode == 3) RaiseException(0xe0505250, 0, 0, NULL);
    return sh_package_shader_preparation_run(batch, prepare_one, recover, &fixture, error, capacity);
}
static void run_case(int mode)
{
    char error[512] = ""; uint64_t *completed = NULL; size_t count = 0;
    create("overrides/fixture/package.json", descriptor);
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_admission_ready());
    initialize(); fixture.mode = mode;
    fixture.published = sh_package_runtime_acquire(); sh_package_runtime_release();
    enqueue();
    if (mode == 5) fixture.ready = 0;
    int ok = sh_package_runtime_test_prepare_native_sources(fixture.assets, &completed, &count, error, sizeof(error));
    CHECK(ok == (mode == 0)); CHECK(count == (mode == 0 ? 1u : 0u));
    CHECK(fixture.calls == (mode == 5 ? 0 : 1));
    CHECK(sh_package_runtime_admission_ready() == (mode != 3 && mode != 4));
    CHECK(sh_package_runtime_acquire() == fixture.published); sh_package_runtime_release();
    if (mode == 0) {
        /* Lost evidence cannot trigger an unbounded repeat of a previously
         * completed native task in the same immutable compilation. */
        enqueue(); error[0] = 0;
        CHECK(!sh_package_runtime_test_prepare_native_sources(fixture.assets, &completed, &count, error, sizeof(error)));
        CHECK(strstr(error, "same compilation") && fixture.calls == 1 && count == 1);
        CHECK(sh_package_runtime_admission_ready());
    } else CHECK(*error);
    CHECK(fixture.recoveries == ((mode == 1 || mode == 2 || mode == 4) ? 1 : 0));
    if (mode != 4) CHECK(fixture.object[0x58] == 0);
    free(completed); sh_package_native_assets_close(fixture.assets);
    sh_package_shader_receipt_invalidate((uintptr_t)fixture.object);
    /* Failure or exception released the exclusive retry guard. */
    create("overrides/fixture/package.json", descriptor);
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_admission_ready());
}
int main(void)
{
    char temp[MAX_PATH]; DWORD n = GetTempPathA(sizeof(temp), temp);
    CHECK(n && n < sizeof(temp));
    snprintf(root, sizeof(root), "%ssmp-native-prepare-%lu-%llu", temp,
        GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    CHECK(CreateDirectoryA(root, NULL));
    sh_package_runtime_test_empty_catalog();
    for (int mode = 0; mode < 6; mode++) run_case(mode);
    cleanup();
    if (failures) return 1;
    puts("package_native_preparation_runtime_test: PASS"); return 0;
}
