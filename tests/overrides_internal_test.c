/* overrides_internal_test.c -- immutable per-decl table and read-only stream tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "overrides.h"
#include "resource_bridge.h"

static int g_failed;
static int g_user_enabled;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

void backend_log(const char *message)
{
    (void)message;
}

int sh_user_overrides_enabled_for_launch(void)
{
    return g_user_enabled;
}

int sh_resource_bridge_capture(const char *data_root)
{
    (void)data_root;
    return 1;
}

void sh_resource_bridge_set_provider_ready(int ready)
{
    (void)ready;
}

int sh_resource_bridge_open(const char *name, unsigned char **out,
                            size_t *out_length, const char **out_source)
{
    (void)name;
    if (out) *out = NULL;
    if (out_length) *out_length = 0;
    if (out_source) *out_source = NULL;
    return SH_RESOURCE_BRIDGE_MISS;
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
    {
        const uint8_t *base = (const uint8_t *)(uintptr_t)0x10000000u;
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1A51070u,
                  base + 0x0267390u,
                  base + 0x0267290u,
                  base + 0x0268470u) == 1);
        CHECK(sh_overrides_test_supported_build_abi(
                  base,
                  base + 0x1A51070u,
                  base + 0x0267391u,
                  base + 0x0267290u,
                  base + 0x0268470u) == 0);
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
    source[0] = 'X'; /* publication must own a process-lifetime copy */
    source_two[0] = 'X';
    CHECK(sh_overrides_test_internal_decl_open("decltree/ActorModifier/custom/first.decl") == NULL);
    CHECK(sh_overrides_test_internal_decl_open("generated/decls/actormodifier/custom/first.decl") == NULL);
    CHECK(sh_overrides_test_internal_decl_open("decltree/actormodifier/custom/first.decl.extra") == NULL);

    stream = sh_overrides_test_internal_decl_open("decltree/actormodifier/custom/first.decl");
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

    stream = sh_overrides_test_internal_decl_open("decltree/entitydef/custom/second.decl");
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

int main(void)
{
    test_internal_decl_table();
    test_file_stream_is_read_only();
    if (g_failed) {
        fprintf(stderr, "%d overrides internal-decl-table test(s) failed\n", g_failed);
        return 1;
    }
    puts("overrides internal-decl-table tests passed");
    return 0;
}
