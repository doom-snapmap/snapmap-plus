/* overrides_internal_test.c -- immutable per-decl table and read-only stream tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "overrides.h"
#include "package_shader_receipt.h"
#include "resource_graph_native.h"
#include "package_runtime.h"
#include "resource_instances.h"
#include "resource_graph.h"
static int source_active, source_observed, source_baseline, source_captured;
static int source_failure;
static void *source_consumer, *source_input;
static uint64_t source_consumer_token, source_input_token, source_recovered_token;
int sh_resource_graph_native_source_fault_target(void **object,uint64_t *token)
{ *object=source_consumer; *token=source_consumer_token; return *object && *token; }
int sh_resource_graph_native_source_fault_input(void **object,uint64_t *token)
{ *object=source_input; *token=source_input_token; return *object && *token; }
int sh_resource_graph_native_source_fault_recovered(void *object,uint64_t token)
{ return object==source_consumer && token<=source_recovered_token; }
int sh_resource_graph_native_source_target(char family[128], char name[4096])
{
    if (!source_active) return 0;
    strcpy_s(family,128,"renderparm"); strcpy_s(name,4096,"test"); return 1;
}
int sh_resource_graph_native_source_observed(const char *family,const char *name,
    sh_package_resource_capture *capture,int baseline)
{
    if (source_active && !strcmp(family,"renderparm") && !strcmp(name,"test")) {
        source_observed++; source_baseline+=baseline; source_captured+=!!capture;
    }
    sh_package_runtime_resource_capture_release(capture); return source_active && source_failure ? -1 : source_active;
}
int sh_resource_graph_native_cook_input(const char *path,unsigned char **body,size_t *length)
{ (void)path;*body=NULL;*length=0;return 0; }

static int g_failed;
static int g_user_enabled;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

/* The last MERGED line: a merge reports what it added, changed and kept. */
static char g_last_merged[320];
void backend_log(const char *message)
{
    if (message && strstr(message, "internal decl table MERGED")) snprintf(g_last_merged, sizeof(g_last_merged), "%s", message);
}

int sh_user_overrides_enabled_for_launch(void)
{
    return g_user_enabled;
}




/* Baked navigation is a different subsystem with its own tests; the file shadow
 * only has to ask it first. */
#include "../src/backend/nav_bake.h"

/* Navigation baked from a map's marked volumes. No map is loaded in this test,
 * so the hook must fall through to the shadow exactly as it does in game. */
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    (void)name; (void)read_shipped;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}

int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}

static void test_internal_decl_table(void)
{
    static unsigned char source[] = "0123456789abcdef";
    static unsigned char source_two[] = "second-stream";
    static const sh_overrides_internal_decl_entry entries[] = {
        { "ActorModifier", "custom/first", source, sizeof(source) - 1 },
        { "entityDef", "custom/second", source_two, sizeof(source_two) - 1 }
    };
    static const sh_overrides_internal_decl_entry duplicate_entries[] = {
        { "entityDef", "custom/dup", source, sizeof(source) - 1 },
        { "ENTITYDEF", "CUSTOM/dup", source_two, sizeof(source_two) - 1 }
    };
    unsigned char readback[sizeof(source)];
    void *stream;

    sh_overrides_test_stream_helpers_reset();
    /* Require SIG_OK and an in-image address, accepting shifted RVAs.
     * SIG_OK_HOOKED and out-of-image pointers must refuse the provider. */
    {
        const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
        CHECK(base != NULL);
        /* Four arbitrary, mutually different in-image offsets: no RVA is privileged any more. */
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1000u, 1,
                  base + 0x1100u, 1,
                  base + 0x1200u, 1,
                  base + 0x1300u, 1) == 1);
        /* The same four shifted by an arbitrary amount -- exactly what the second shipped build
         * looks like -- are just as acceptable. */
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1001u, 1,
                  base + 0x1101u, 1,
                  base + 0x1201u, 1,
                  base + 0x1301u, 1) == 1);
        /* A hook-tolerant known_rva resolve of any one of them still refuses: the ctor prologue is
         * decoded and the helpers are called through an engine-owned table. */
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1000u, 1,
                  base + 0x1100u, 0,
                  base + 0x1200u, 1,
                  base + 0x1300u, 1) == 0);
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1000u, 0,
                  base + 0x1100u, 1,
                  base + 0x1200u, 1,
                  base + 0x1300u, 1) == 0);
        /* An unresolved location, or one that is not in the host image at all, refuses. */
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1000u, 1,
                  NULL, 1,
                  base + 0x1200u, 1,
                  base + 0x1300u, 1) == 0);
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1000u, 1,
                  base + 0x1100u, 1,
                  base + 0x1200u, 1,
                  base - 0x1000u, 1) == 0);
        CHECK(sh_overrides_test_supported_build_abi(
                  NULL,
                  base + 0x1000u, 1,
                  base + 0x1100u, 1,
                  base + 0x1200u, 1,
                  base + 0x1300u, 1) == 0);

        /* Accept a provider vtable in a read-only section; reject executable
         * and out-of-image pointers using real process addresses. */
        CHECK(sh_overrides_test_address_in_readonly_section(base, "rdata-probe") == 1);
        CHECK(sh_overrides_test_address_in_readonly_section(
                  base, (const void *)(uintptr_t)&test_internal_decl_table) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(base, &readback) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(base, NULL) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(NULL, base) == 0);
    }
    CHECK(sh_overrides_test_stream_vtable_slots() == 31);
    CHECK(sh_overrides_test_stream_vtable_slot(0) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(24) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(25) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(26) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(27) != NULL);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(24))(NULL) == 0);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(25))(NULL) == 1);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(26))(NULL) == 0);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(27))(NULL) == 0);
    CHECK(sh_overrides_test_stream_vtable_slot(28) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(29) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(30) == NULL);

    /* A dirty/missing helper refuses as one terminal publication: no tail slot can be observed
     * partially configured. */
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
                                                      (void *)(uintptr_t)0x2222, 0,
                                                      (void *)(uintptr_t)0x3333, 1) == 0);
    CHECK(sh_overrides_test_stream_helpers_ready() == 0);
    CHECK(sh_overrides_test_stream_vtable_slot(28) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(29) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(30) == NULL);

    sh_overrides_test_stream_helpers_reset();
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
                                                      (void *)(uintptr_t)0x2222, 1,
                                                      (void *)(uintptr_t)0x3333, 1) == 1);
    CHECK(sh_overrides_test_stream_helpers_ready() == 1);
    CHECK(sh_overrides_test_stream_vtable_slot(28) == (void *)(uintptr_t)0x1111);
    CHECK(sh_overrides_test_stream_vtable_slot(29) == (void *)(uintptr_t)0x2222);
    CHECK(sh_overrides_test_stream_vtable_slot(30) == (void *)(uintptr_t)0x3333);
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x4444, 1,
                                                      (void *)(uintptr_t)0x5555, 1,
                                                      (void *)(uintptr_t)0x6666, 1) == 0);
    sh_overrides_test_stream_helpers_reset();

    sh_overrides_test_internal_decl_table_reset();
    g_user_enabled = 0;
    CHECK(sh_overrides_test_internal_decl_table_install(entries, 2) == 0);

    g_user_enabled = 1;
    CHECK(sh_overrides_test_internal_decl_table_install(entries, 2) == 1);
    CHECK(sh_overrides_test_internal_decl_table_install(entries, 2) == 0);
    stream = sh_overrides_test_memory_stream(source, sizeof(source) - 1);
    source[0] = 'X'; /* An opened memory stream owns its bytes. */
    source_two[0] = 'X';
    CHECK(sh_overrides_test_internal_decl_open("decltree/ActorModifier/custom/first.decl") == NULL);
    CHECK(sh_overrides_test_internal_decl_open("generated/decls/actormodifier/custom/first.decl") == NULL);
    CHECK(sh_overrides_test_internal_decl_open("decltree/actormodifier/custom/first.decl.extra") == NULL);

    /* Registry history alone cannot serve declarations without a compiler.
     * Raw stream I/O is tested independently of declaration admission. */
    CHECK(sh_overrides_test_internal_decl_open("decltree/actormodifier/custom/first.decl") == NULL);
    CHECK(!sh_overrides_internal_decl_published("decltree/actormodifier/custom/first.decl"));
    CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_length(stream) == (long long)(sizeof(source) - 1));
        CHECK(sh_overrides_test_stream_true_flag(stream) == 1);
        CHECK(sh_overrides_test_stream_set_length(stream, 3) == 0);
        CHECK(sh_overrides_test_stream_length(stream) == (long long)(sizeof(source) - 1));
        CHECK(sh_overrides_test_stream_read(stream, readback, 2) == 2);
        CHECK(memcmp(readback, "01", 2) == 0);

        /* idFile seek origins are CUR=0, END=1, ABS=2. */
        CHECK(sh_overrides_test_stream_seek(stream, 2, 0) == 0);
        CHECK(sh_overrides_test_stream_read(stream, readback, 2) == 2);
        CHECK(memcmp(readback, "45", 2) == 0);
        CHECK(sh_overrides_test_stream_seek(stream, -3, 1) == 0);
        CHECK(sh_overrides_test_stream_read(stream, readback, 3) == 3);
        CHECK(memcmp(readback, "def", 3) == 0);
        CHECK(sh_overrides_test_stream_seek(stream, 5, 2) == 0);
        CHECK(sh_overrides_test_stream_read(stream, readback, 2) == 2);
        CHECK(memcmp(readback, "56", 2) == 0);

        /* Signed cursor arithmetic refuses overflow and preserves position. */
        CHECK(sh_overrides_test_stream_seek(stream, 5, 2) == 0);
        CHECK(sh_overrides_test_stream_seek(stream, LLONG_MAX, 0) == -1);
        CHECK(sh_overrides_test_stream_read(stream, readback, 2) == 2);
        CHECK(memcmp(readback, "56", 2) == 0);
        CHECK(sh_overrides_test_stream_seek(stream, LLONG_MAX, 1) == -1);

        /* An invalid origin refuses without changing the cursor. */
        CHECK(sh_overrides_test_stream_seek(stream, 1, 99) == -1);
        CHECK(sh_overrides_test_stream_read(stream, readback, 2) == 2);
        CHECK(memcmp(readback, "78", 2) == 0);

        /* The native read-at helper is absolute and leaves the cursor after its read. */
        CHECK(sh_overrides_test_stream_read_at(stream, 8, readback, 3) == 3);
        CHECK(memcmp(readback, "89a", 3) == 0);

        /* Memory-backed streams are read-only, including the native write-at helper. */
        CHECK(sh_overrides_test_stream_write_at(stream, 4, "Z", 1) == 0);
        CHECK(sh_overrides_test_stream_read_at(stream, 4, readback, 2) == 2);
        CHECK(memcmp(readback, "45", 2) == 0);
        CHECK(sh_overrides_test_stream_write(stream, "x", 1) == 0);
        sh_overrides_test_stream_close(stream);
    }

    CHECK(sh_overrides_test_internal_decl_open("decltree/entitydef/custom/second.decl") == NULL);
    stream = sh_overrides_test_memory_stream((const unsigned char *)"second-stream", sizeof(source_two) - 1);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_read(stream, readback, 6) == 6);
        CHECK(memcmp(readback, "second", 6) == 0);
        sh_overrides_test_stream_close(stream);
    }

    sh_overrides_test_internal_decl_table_reset();
    CHECK(sh_overrides_test_internal_decl_table_install(duplicate_entries, 2) == 0);

    g_user_enabled = 0;
    CHECK(sh_overrides_test_internal_decl_open("decltree/actormodifier/custom/first.decl") == NULL);
    sh_overrides_test_internal_decl_table_reset();
}

static void test_file_stream_is_read_only(void)
{
    char temp_dir[MAX_PATH];
    char path[MAX_PATH];
    FILE *fp;
    void *stream;

    CHECK(GetTempPathA(sizeof temp_dir, temp_dir) != 0);
    CHECK(GetTempFileNameA(temp_dir, "smp", 0, path) != 0);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (!fp) {
        DeleteFileA(path);
        return;
    }
    CHECK(fwrite("file-backed", 1, 11, fp) == 11);
    fclose(fp);

    stream = sh_overrides_test_stream_open_file(path);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_length(stream) == 11);
        CHECK(sh_overrides_test_stream_set_length(stream, 2) == 0);
        CHECK(sh_overrides_test_stream_length(stream) == 11);
        sh_overrides_test_stream_close(stream);
    }
    DeleteFileA(path);
}


static void test_owned_file_stream(void)
{
    char temp_dir[MAX_PATH], path[MAX_PATH], bytes[16] = {0};
    CHECK(GetTempPathA(sizeof(temp_dir), temp_dir) != 0);
    CHECK(GetTempFileNameA(temp_dir, "smp", 0, path) != 0);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL); if (!file) { DeleteFileA(path); return; }
    CHECK(fwrite("owned-pages", 1, 11, file) == 11); fclose(file);
    sh_overrides_test_stream_helpers_reset();
    file = fopen(path, "rb"); CHECK(file != NULL);
    CHECK(!sh_overrides_stream_take_file(file, 11, "virtualtextures/private.pages"));
    /* Exclusive reopen proves the failed constructor released the owned file. */
    HANDLE exclusive = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(exclusive != INVALID_HANDLE_VALUE);
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
        (void *)(uintptr_t)0x2222, 1, (void *)(uintptr_t)0x3333, 1));
    file = fopen(path, "rb"); CHECK(file != NULL);
    if (file) CHECK(_fseeki64(file, 8, SEEK_SET) == 0);
    char name[] = "virtualtextures/private.pages";
    void *stream = sh_overrides_stream_take_file(file, 11, name);
    name[0] = 'X';
    CHECK(stream != NULL);
    if (stream) {
        void **table = *(void ***)stream;
        const char *(*get_name)(void *) = (void *)table[3];
        CHECK(!strcmp(get_name(stream), "virtualtextures/private.pages"));
        CHECK(sh_overrides_test_stream_read(stream, bytes, sizeof(bytes)) == 11);
        CHECK(!strcmp(bytes, "owned-pages"));
        CHECK(sh_overrides_test_stream_length(stream) == 11);
        CHECK(sh_overrides_test_stream_write(stream, "x", 1) == 0);
        sh_overrides_test_stream_close(stream);
    }
    file = fopen(path, "rb"); CHECK(file != NULL);
    CHECK(!sh_overrides_stream_take_file(file, UINT64_MAX, "virtualtextures/private.pages"));
    exclusive = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(exclusive != INVALID_HANDLE_VALUE);
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    CHECK(!sh_overrides_stream_take_file(NULL, 0, "virtualtextures/private.pages"));
    sh_overrides_test_stream_helpers_reset(); CHECK(DeleteFileA(path));
}

static void test_copied_byte_stream(void)
{
    unsigned char source[] = "private page bytes";
    char name[] = "virtualtextures/private.pages", bytes[32] = {0};
    sh_overrides_test_stream_helpers_reset();
    CHECK(!sh_overrides_stream_copy_bytes(source, sizeof(source) - 1, name));
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
        (void *)(uintptr_t)0x2222, 1, (void *)(uintptr_t)0x3333, 1));
    CHECK(!sh_overrides_stream_copy_bytes(NULL, 1, name));
    CHECK(!sh_overrides_stream_copy_bytes(source, sizeof(source), NULL));
    CHECK(!sh_overrides_stream_copy_bytes(source, SIZE_MAX, name));
    void *stream = sh_overrides_stream_copy_bytes(source, sizeof(source) - 1, name);
    CHECK(stream != NULL); source[0] = 'X'; name[0] = 'X';
    if (stream) {
        void **table = *(void ***)stream;
        CHECK(!strcmp(((const char *(*)(void *))table[3])(stream), "virtualtextures/private.pages"));
        CHECK(sh_overrides_test_stream_read(stream, bytes, sizeof(bytes)) == sizeof(source) - 1);
        CHECK(!strcmp(bytes, "private page bytes"));
        CHECK(sh_overrides_test_stream_write(stream, "x", 1) == 0);
        sh_overrides_test_stream_close(stream);
    }
    stream = sh_overrides_stream_copy_bytes(NULL, 0, "empty"); CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_length(stream) == 0);
        CHECK(sh_overrides_test_stream_read(stream, bytes, sizeof(bytes)) == 0);
        sh_overrides_test_stream_close(stream);
    }
    sh_overrides_test_stream_helpers_reset();
}

/* R-398: a registration pass hands the table every candidate (about 6,000
 * live); its lookups and merge are indexed, with the same outcomes as the
 * linear walk: exact lower-case keys, supplied entries first, a duplicate
 * within the supplied set refused, and unchanged entries carried forward. */
static void test_large_table(void)
{
    enum { N = 4000 };
    static char names[N + 5][32];
    static sh_overrides_internal_decl_entry entries[N + 5];
    static const unsigned char same[] = "same body", changed[] = "changed body";
    char key[96];
    LARGE_INTEGER began, ended, frequency;
    g_user_enabled = 1;
    sh_overrides_test_internal_decl_table_reset();
    for (int i = 0; i < N + 5; i++) {
        snprintf(names[i], sizeof(names[i]), "bulk/%04d", (N + 4 - i) * 7919 % 10007);
        entries[i] = (sh_overrides_internal_decl_entry){"Material", names[i], same, sizeof(same) - 1};
    }
    CHECK(sh_overrides_test_internal_decl_table_install(entries, N) == 1);
    CHECK(sh_overrides_internal_decl_published_count() == N);
    for (int i = 0; i < N + 5; i++) {
        snprintf(key, sizeof(key), "decltree/material/%s.decl", names[i]);
        CHECK(sh_overrides_test_internal_decl_registered(key) == (i < N));
    }
    CHECK(!sh_overrides_test_internal_decl_registered("decltree/Material/bulk/0000.decl"));
    for (int i = N - 10; i < N; i++) entries[i].body = changed, entries[i].body_length = sizeof(changed) - 1;
    g_last_merged[0] = 0;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&began);
    CHECK(sh_overrides_test_internal_decl_table_merge(entries, N + 5) == 1);
    QueryPerformanceCounter(&ended);
    printf("large table merge: %d supplied over %d in %.1f ms\n", N + 5, N,
        (double)(ended.QuadPart - began.QuadPart) * 1000.0 / (double)frequency.QuadPart);
    CHECK(strstr(g_last_merged, "4005 supplied (5 added, 10 changed, 3990 unchanged) + 0 carried forward = 4005 published") != NULL);
    for (int i = 0; i < N + 5; i++) {
        snprintf(key, sizeof(key), "decltree/material/%s.decl", names[i]);
        CHECK(sh_overrides_test_internal_decl_registered(key) == 1);
    }
    CHECK(sh_overrides_test_internal_decl_table_merge(entries + 2, 3) == 1);
    CHECK(strstr(g_last_merged, "3 supplied (0 added, 0 changed, 3 unchanged) + 4002 carried forward = 4005 published") != NULL);
    {
        sh_overrides_internal_decl_entry twice[2] = {
            {"material", "bulk/new", same, sizeof(same) - 1}, {"MATERIAL", "BULK/NEW", changed, sizeof(changed) - 1}};
        CHECK(sh_overrides_test_internal_decl_table_merge(twice, 2) == 0);
        CHECK(sh_overrides_internal_decl_published_count() == N + 5);
        CHECK(!sh_overrides_test_internal_decl_registered("decltree/material/bulk/new.decl"));
        CHECK(sh_overrides_test_internal_decl_table_merge(twice, 1) == 1);
        CHECK(sh_overrides_test_internal_decl_registered("decltree/material/bulk/new.decl") == 1);
        CHECK(sh_overrides_internal_decl_published_count() == N + 6);
    }
    sh_overrides_test_internal_decl_table_reset();
    {
        sh_overrides_internal_decl_entry twice[2] = {
            {"material", "bulk/dup", same, sizeof(same) - 1}, {"Material", "Bulk/Dup", same, sizeof(same) - 1}};
        CHECK(sh_overrides_test_internal_decl_table_install(twice, 2) == 0);
    }
    sh_overrides_test_internal_decl_table_reset();
}

static void test_refresh_keeps_open_streams(void)
{
    static const unsigned char old_body[] = "old body";
    static const unsigned char new_body[] = "new body";
    sh_overrides_internal_decl_entry entry = {"material", "test/refresh", old_body, 8};
    char bytes[16] = {0};
    void *old_stream, *new_stream;
    g_user_enabled = 1;
    sh_overrides_test_internal_decl_table_reset();
    CHECK(sh_overrides_test_internal_decl_table_install(&entry, 1) == 1);
    old_stream = sh_overrides_test_memory_stream(old_body, 8);
    CHECK(old_stream != NULL);
    sh_overrides_internal_decl_table_reopen();
    CHECK(sh_overrides_internal_decl_published("decltree/material/test/refresh.decl") == 0);
    entry.body = new_body;
    g_last_merged[0] = 0;
    CHECK(sh_overrides_test_internal_decl_table_merge(&entry, 1) == 1);
    CHECK(strstr(g_last_merged, "1 supplied (0 added, 1 changed, 0 unchanged) + 0 carried forward = 1 published") != NULL);
    for (int i = 0; i < 79; i++)
        CHECK(sh_overrides_test_internal_decl_table_merge(&entry, 1) == 1);
    /* R-381: a pass that supplies the same bodies again reports them unchanged,
     * not new, and the table does not grow. */
    CHECK(strstr(g_last_merged, "1 supplied (0 added, 0 changed, 1 unchanged) + 0 carried forward = 1 published") != NULL);
    CHECK(sh_overrides_internal_decl_published_count() == 1);
    {
        sh_overrides_internal_decl_entry added = {"material", "test/added", new_body, 8};
        CHECK(sh_overrides_test_internal_decl_table_merge(&added, 1) == 1);
        CHECK(strstr(g_last_merged, "1 supplied (1 added, 0 changed, 0 unchanged) + 1 carried forward = 2 published") != NULL);
        CHECK(sh_overrides_internal_decl_published_count() == 2);
        CHECK(sh_overrides_test_internal_decl_table_merge(&entry, 1) == 1);
        CHECK(sh_overrides_internal_decl_published_count() == 2);
    }
    CHECK(sh_overrides_test_internal_decl_open("decltree/material/test/refresh.decl") == NULL);
    new_stream = sh_overrides_test_memory_stream(new_body, 8);
    CHECK(new_stream != NULL);
    if (old_stream) {
        CHECK(sh_overrides_test_stream_read(old_stream, bytes, 8) == 8);
        CHECK(memcmp(bytes, old_body, 8) == 0);
        sh_overrides_test_stream_close(old_stream);
    }
    if (new_stream) {
        CHECK(sh_overrides_test_stream_read(new_stream, bytes, 8) == 8);
        CHECK(memcmp(bytes, new_body, 8) == 0);
        sh_overrides_test_stream_close(new_stream);
    }
    {
        sh_overrides_internal_decl_entry entries[1025];
        char names[1025][32];
        for (int i = 0; i < 1025; i++) {
            _snprintf_s(names[i], sizeof(names[i]), _TRUNCATE, "test/entry-%03d", i);
            entries[i].type = "material";
            entries[i].name = names[i];
            entries[i].body = old_body;
            entries[i].body_length = 8;
        }
        sh_overrides_test_internal_decl_table_reset();
        CHECK(sh_overrides_test_internal_decl_table_install(entries, SIZE_MAX) == 0);
        CHECK(sh_overrides_test_internal_decl_table_install(entries, 1025) == 1);
        for (int i = 0; i < 1025; i++) entries[i].body = new_body;
        CHECK(sh_overrides_test_internal_decl_table_merge(entries, 1025) == 1);
        CHECK(sh_overrides_internal_decl_published_count() == 1025);
        /* Carry forward a table larger than the old limit while adding a key. */
        CHECK(sh_overrides_test_internal_decl_table_merge(&entry, 1) == 1);
        CHECK(sh_overrides_internal_decl_published_count() == 1026);
        for (int i = 0; i < 1025; i++) {
            char key[96];
            void *current;
            _snprintf_s(key, sizeof(key), _TRUNCATE,
                        "decltree/material/test/entry-%03d.decl", i);
            current = sh_overrides_test_internal_decl_open(key);
            CHECK(current == NULL);
            CHECK(!sh_overrides_internal_decl_published(key));
        }
        /* Refuse impossible allocation arithmetic without damaging publication. */
        CHECK(sh_overrides_test_internal_decl_table_merge(&entry, SIZE_MAX) == 0);
        CHECK(sh_overrides_test_internal_decl_table_merge(&entry, SIZE_MAX / 8) == 0);
        CHECK(sh_overrides_internal_decl_published_count() == 1026);
    }
    sh_overrides_test_internal_decl_table_reset();
}

static unsigned char shader_header[0x90];
static int shader_read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (address != (uintptr_t)shader_header || length > sizeof(shader_header)) return 0;
    memcpy(out, shader_header, length); return 1;
}
static void test_actual_stream_receipt(void)
{
    const char source[] = "{ Tex2D source }\r\n";
    const char *path = "generated/decls/renderparm/test.decl";
    sh_package_shader_receipt_frame frame;
    void *stream = sh_overrides_test_memory_stream(source, strlen(source));
    CHECK(stream != NULL); if (!stream) return;
    void **original = *(void ***)stream; char body[64] = {0};
    sh_package_shader_receipt_begin(&frame, (uintptr_t)shader_header, "renderparm", "test");
    sh_overrides_test_receipt_attach(stream, path);
    void **table = *(void ***)stream;
    CHECK(table != original && table[5] != original[5] && table[0] != original[0]);
    for (size_t i = 1; i < 31; i++) if (i != 5) CHECK(table[i] == original[i]);
    long long (*read)(void *, void *, uint64_t) = (void *)table[5];
    CHECK(read(stream, body, 4) == 4);
    CHECK(read(stream, body + 4, sizeof(body) - 4) == (long long)strlen(source) - 4);
    CHECK(!memcmp(body, source, strlen(source)));
    CHECK(((long long (*)(void *))table[13])(stream) == (long long)strlen(source));
    /* The native destructor receives its original table, and the private
     * observer dies with this stream. No bytes were reread by path. */
    ((void *(*)(void *, unsigned))table[0])(stream, 1);
    sh_package_shader_receipt_end(&frame, 1, shader_read_memory, NULL);
    CHECK(sh_package_shader_receipt_matches((uintptr_t)shader_header, "renderparm", "test",
        source, strlen(source), shader_header, sizeof(shader_header)));
    sh_package_shader_receipt_invalidate((uintptr_t)shader_header);
}
static void test_actual_source_receipt(void)
{
    const char *path="generated/decls/renderparm/test.decl";
    const unsigned char body[]="exact original source";
    source_active=1; source_observed=source_baseline=source_captured=0;
    void *stream=sh_overrides_test_memory_stream(body,sizeof(body));
    CHECK(stream!=NULL); if(!stream) return;
    void **original=*(void ***)stream;
    sh_overrides_test_source_attach(stream,path,NULL,1);
    void **table=*(void ***)stream;
    CHECK(table!=original && !source_observed);
    unsigned char got[sizeof(body)]={0};
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,4)==4);
    CHECK(source_observed==1 && source_baseline==1 && !source_captured);
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got+4,sizeof(body)-4)==sizeof(body)-4);
    CHECK(source_observed==2 && source_baseline==2 && !memcmp(got,body,sizeof(body)));
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,0)==0 && source_observed==2);
    ((void *(*)(void *,unsigned))table[0])(stream,1);
    stream=sh_overrides_test_memory_stream(body,sizeof(body)); original=*(void ***)stream;
    sh_overrides_test_source_attach(stream,"generated/decls/renderparm/unrelated.decl",NULL,1);
    CHECK(*(void ***)stream==original); sh_overrides_test_stream_close(stream);
    stream=sh_overrides_test_memory_stream(body,sizeof(body));
    sh_overrides_test_source_attach(stream,path,NULL,0); table=*(void ***)stream;
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,1)==1);
    CHECK(source_observed==3 && source_baseline==2); /* Unknown is not baseline. */
    source_active=0;
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,1)==1 && source_observed==3);
    ((void *(*)(void *,unsigned))table[0])(stream,1);
}

static int graph_payloads(void *context,const char *parent_type,const char *parent_name,const char *type,const char *name)
{
    (void)parent_name;
    if(parent_type && !*type && !strcmp(name,"test/metadata-only.bin"))(*(int *)context)++;
    return 1;
}
static void test_file_metadata_is_not_payload(void)
{
    const char *path="test/metadata-only.bin";unsigned char byte;
    for(int mode=0;mode<4;mode++) {
        sh_resource_graph_frame frame;int found=0;
        sh_resource_graph_begin(&frame,"material","metadata-boundary");
        void *stream=sh_overrides_test_memory_stream((const unsigned char *)"x",mode==3?0:1);
        CHECK(stream);if(!stream){sh_resource_graph_end(&frame,0);continue;}
        void **original=*(void ***)stream;
        sh_overrides_test_receipt_attach(stream,path);
        void **table=*(void ***)stream;CHECK(table!=original);
        sh_overrides_test_receipt_attach(stream,path);CHECK(*(void ***)stream==table);
        CHECK(((long long (*)(void *))table[11])(stream)==(mode==3?0:1));
        ((unsigned (*)(void *))table[19])(stream); /* Timestamp */
        if(mode==1)CHECK(sh_overrides_test_stream_read(stream,&byte,0)==0);
        if(mode>=2)CHECK(sh_overrides_test_stream_read(stream,&byte,1)==(mode==3?0:1));
        sh_overrides_test_stream_close(stream);sh_resource_graph_end(&frame,1);
        sh_resource_graph_walk("material","metadata-boundary",graph_payloads,&found);
        CHECK(found==(mode>=2));
    }
    source_active=1;source_observed=source_baseline=0;
    void *stream=sh_overrides_test_memory_stream((const unsigned char *)"",0);CHECK(stream);
    if(stream) {
        sh_overrides_test_source_attach(stream,"generated/decls/renderparm/test.decl",NULL,1);
        CHECK(sh_overrides_test_stream_read(stream,&byte,1)==0 && source_observed==1 && source_baseline==1);
        sh_overrides_test_stream_close(stream);
    }
    source_active=0;
}

static void test_failed_source_consumers(void)
{
    const char body[]="retained opening bytes"; unsigned char out[8]; char error[128];
    int input, unrelated, consumer;
    int admission_before=sh_package_runtime_admission_ready();
    void *affected[]={&input}, *other[]={&unrelated};
    source_active=source_failure=1;
    source_consumer=&consumer; source_input=&input;
    source_consumer_token=10; source_input_token=20; source_recovered_token=0;
    void *stream=sh_overrides_test_memory_stream(body,sizeof(body)); CHECK(stream);
    if (!stream) return;
    sh_overrides_test_source_attach(stream,"generated/decls/renderparm/test.decl",NULL,1);
    void **table=*(void ***)stream;
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,out,1)==1);
    CHECK(sh_overrides_test_pending_source_count()==1);
    CHECK(!sh_overrides_source_receipts_admit(affected,1,error,sizeof(error)));
    CHECK(sh_overrides_source_receipts_admit(other,1,error,sizeof(error)));
    CHECK(sh_package_runtime_admission_ready()==admission_before); /* No new global fence. */
    source_recovered_token=10;
    CHECK(sh_overrides_source_receipts_admit(affected,1,error,sizeof(error)));
    sh_overrides_collect_source_receipts();
    CHECK(sh_overrides_test_pending_source_count()==1); /* The stream is still open. */
    source_consumer_token=11; /* Same address, a later lifetime cannot borrow token10 recovery. */
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,out,1)==1);
    CHECK(!sh_overrides_source_receipts_admit(affected,1,error,sizeof(error)));
    ((void *(*)(void*,unsigned))table[0])(stream,1);
    CHECK(sh_overrides_test_pending_source_count()==1);
    source_recovered_token=11;
    sh_overrides_collect_source_receipts();
    CHECK(sh_overrides_test_pending_source_count()==0);
    CHECK(sh_overrides_source_receipts_admit(affected,1,error,sizeof(error)));
    CHECK(!sh_overrides_test_failed_source_count() && sh_package_runtime_admission_ready()==admission_before);
    source_active=source_failure=0; source_consumer=source_input=NULL;
}

static int vt_read_scope, vt_reads, vt_unknown;
static sh_resource_instance_origin *vt_seen;
static int vt_recording(void) { return vt_read_scope; }
static int vt_observed(sh_resource_instance_origin *origin)
{
    vt_reads++;
    if (!origin) { vt_unknown++; return 0; }
    if (vt_seen) CHECK(vt_seen==origin);
    else { CHECK(sh_resource_instances_origin_hold(origin)); vt_seen=origin; }
    return 1;
}
static int vt_promote(const void *capture, void **lease)
{
    char error[256]; return sh_package_runtime_resource_promote(capture,
        (sh_package_selector_hold **)lease,error,sizeof(error));
}
static void vt_release_capture(void *capture) { sh_package_runtime_resource_capture_release(capture); }
static void vt_release_lease(void *lease) { sh_package_runtime_selectors_release(lease); }
static void test_actual_vt_source_receipt(void)
{
    const unsigned char body[]="page header"; unsigned char got[32];
    CHECK(sh_resource_instances_bind((sh_resource_instance_api){vt_promote,vt_release_capture,vt_release_lease}));
    sh_overrides_test_vt_observer(vt_recording,vt_observed);
    vt_read_scope=1; vt_reads=vt_unknown=0; source_active=0;
    void *stream=sh_overrides_test_memory_stream(body,sizeof(body)); CHECK(stream);
    if (!stream) return;
    void **original=*(void ***)stream;
    sh_overrides_test_source_attach(stream,"virtualtextures/_vmtr.pages",NULL,1);
    void **table=*(void ***)stream;
    CHECK(table!=original && !vt_reads && !vt_seen);
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,0)==0 && !vt_reads);
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,3)==3);
    CHECK(vt_reads==1 && vt_seen && !vt_unknown);
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got+3,sizeof(body)-3)==sizeof(body)-3);
    CHECK(vt_reads==2 && !vt_unknown && !memcmp(got,body,sizeof(body)));
    /* EOF is an observed result of a payload request; a replacement file
     * could have supplied another byte. The same retained origin is reused. */
    CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,1)==0 && vt_reads==3 && !vt_unknown);
    ((void *(*)(void*,unsigned))table[0])(stream,1);
    /* The observer's live share survives destruction of the reading stream. */
    CHECK(sh_resource_instances_origin_hold(vt_seen));
    sh_resource_instances_origin_unhold(vt_seen);
    sh_resource_instances_origin_unhold(vt_seen); vt_seen=NULL;
    stream=sh_overrides_test_memory_stream(body,sizeof(body)); CHECK(stream);
    if (stream) {
        sh_overrides_test_source_attach(stream,"virtualtextures/private.pages",NULL,0);
        table=*(void ***)stream;
        CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,1)==1);
        CHECK(vt_reads==4 && vt_unknown==1 && !vt_seen);
        vt_read_scope=0;
        CHECK(((long long(*)(void*,void*,uint64_t))table[5])(stream,got,1)==1 && vt_reads==4);
        ((void *(*)(void*,unsigned))table[0])(stream,1);
    }
    CHECK(sh_overrides_test_failed_source_count()==0);
    sh_overrides_test_vt_observer(NULL,NULL);
}

static int failing_source_destructor_calls;
static void *failing_source_destructor(void *stream, unsigned flags)
{
    (void)stream; (void)flags; failing_source_destructor_calls++;
    RaiseException(0xe1230042, 0, 0, NULL);
    return NULL;
}
static void test_interrupted_source_teardown(void)
{
    const char body[] = "source"; void *stream = sh_overrides_test_memory_stream(body, sizeof(body));
    CHECK(stream != NULL); if (!stream) return;
    void **original = *(void ***)stream, *table[31];
    memcpy(table, original, sizeof(table)); table[0] = (void *)failing_source_destructor;
    *(void ***)stream = table; source_active = 1;
    sh_overrides_test_source_attach(stream, "generated/decls/renderparm/test.decl", NULL, 1);
    int raised = 0;
    __try { ((void *(*)(void *, unsigned))(*(void ***)stream)[0])(stream, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { raised = GetExceptionCode() == 0xe1230042; }
    CHECK(raised && failing_source_destructor_calls == 1);
    CHECK(*(void ***)stream == table && sh_overrides_test_failed_source_count() == 1);
    CHECK(!sh_package_runtime_admission_ready());
    CHECK(!sh_package_runtime_refresh("unused-after-interrupted-cleanup"));
    char error[256]; sh_package_runtime_error(error, sizeof(error));
    CHECK(strstr(error, "native source cleanup") != NULL);
    CHECK(failing_source_destructor_calls == 1);
    /* This fixture's failed callback never entered real cleanup. Free the
     * fixture directly; production never retries or probes a failed object. */
    *(void ***)stream = original;
    ((void *(*)(void *, unsigned))original[0])(stream, 1);
    source_active = 0;
}

int main(void)
{
    test_internal_decl_table();
    test_file_stream_is_read_only();
    test_owned_file_stream();
    test_copied_byte_stream();
    test_large_table();
    test_refresh_keeps_open_streams();
    test_actual_stream_receipt();
    test_actual_source_receipt();
    test_file_metadata_is_not_payload();
    test_failed_source_consumers();
    test_actual_vt_source_receipt();
    test_interrupted_source_teardown();
    if (g_failed) {
        fprintf(stderr, "%d overrides internal-decl-table test(s) failed\n", g_failed);
        return 1;
    }
    puts("overrides internal-decl-table tests passed");
    return 0;
}

/* AAS validation is exercised by grid_room_nav_test, not these lookup tests. */
int sh_navmesh_validate_aas(const unsigned char *p,size_t n,char *e,size_t cap)
{(void)p;(void)n;if(e&&cap)e[0]=0;return 0;}

/* This fixture models source/VT capture, not native DEFAULT consumers. */
int sh_resource_graph_native_observe_resource(void *resource)
{ (void)resource; sh_resource_graph_incomplete(); return 0; }
