/* overrides_internal_test.c -- provider ABI checks, read-only streams and the
 * resource ordering of the file shadow (navigation, built-in defaults, engine). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "overrides.h"

static int g_failed;

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

int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{
    (void)name;
    if (out) *out = NULL;
    if (length) *length = 0;
    return 0;
}

int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}

static void test_stream_abi_and_memory_streams(void)
{
    static unsigned char source[] = "0123456789abcdef";
    static unsigned char source_two[] = "second-stream";
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
                  base, (const void *)(uintptr_t)&test_stream_abi_and_memory_streams) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(base, &readback) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(base, NULL) == 0);
        CHECK(sh_overrides_test_address_in_readonly_section(NULL, base) == 0);
    }
    CHECK(sh_overrides_test_stream_vtable_slots() == 34);
    CHECK(sh_overrides_test_stream_vtable_slot(0) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(26) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(27) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(28) != NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(29) != NULL);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(26))(NULL) == 0);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(27))(NULL) == 1);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(28))(NULL) == 0);
    CHECK(((long long(*)(void *))sh_overrides_test_stream_vtable_slot(29))(NULL) == 0);
    CHECK(sh_overrides_test_stream_vtable_slot(31) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(32) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(33) == NULL);

    /* A dirty/missing helper refuses as one terminal publication: no tail slot can be observed
     * partially configured. */
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
                                                      (void *)(uintptr_t)0x2222, 0,
                                                      (void *)(uintptr_t)0x3333, 1) == 0);
    CHECK(sh_overrides_test_stream_helpers_ready() == 0);
    CHECK(sh_overrides_test_stream_vtable_slot(31) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(32) == NULL);
    CHECK(sh_overrides_test_stream_vtable_slot(33) == NULL);

    sh_overrides_test_stream_helpers_reset();
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x1111, 1,
                                                      (void *)(uintptr_t)0x2222, 1,
                                                      (void *)(uintptr_t)0x3333, 1) == 1);
    CHECK(sh_overrides_test_stream_helpers_ready() == 1);
    CHECK(sh_overrides_test_stream_vtable_slot(31) == (void *)(uintptr_t)0x1111);
    CHECK(sh_overrides_test_stream_vtable_slot(32) == (void *)(uintptr_t)0x2222);
    CHECK(sh_overrides_test_stream_vtable_slot(33) == (void *)(uintptr_t)0x3333);
    CHECK(sh_overrides_test_stream_helpers_configure((void *)(uintptr_t)0x4444, 1,
                                                      (void *)(uintptr_t)0x5555, 1,
                                                      (void *)(uintptr_t)0x6666, 1) == 0);
    sh_overrides_test_stream_helpers_reset();

    stream = sh_overrides_test_memory_stream(source, sizeof(source) - 1);
    source[0] = 'X'; /* An opened memory stream owns its bytes. */
    source_two[0] = 'X';
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

    stream = sh_overrides_test_memory_stream((const unsigned char *)"second-stream", sizeof(source_two) - 1);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_read(stream, readback, 6) == 6);
        CHECK(memcmp(readback, "second", 6) == 0);
        sh_overrides_test_stream_close(stream);
    }
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


/* R-021: the shadow serves map navigation, Grid Room variants, built-in
 * defaults and otherwise the engine. With no engine open installed (this test
 * process) an unclaimed name returns nothing; no user or package file is read,
 * whatever exists under the user-data folder. */
static void test_builtin_defaults_without_user_files(void)
{
    char bytes[8] = {0};
    void *stream = sh_overrides_test_open("generated/decls/snapeditorentitydef/func/lift.decl", 0);
    CHECK(stream != NULL);
    if (stream) {
        CHECK(sh_overrides_test_stream_length(stream) > 0);
        CHECK(sh_overrides_test_stream_read(stream, bytes, 1) == 1 && bytes[0] == '{');
        sh_overrides_test_stream_close(stream);
    }
    /* Case and separators are tolerated, as the engine asks with either. */
    stream = sh_overrides_test_open("GENERATED\\decls\\snapeditorentitydef\\func\\lift.decl", 0);
    CHECK(stream != NULL);
    if (stream) sh_overrides_test_stream_close(stream);
    /* Mode 2 bypasses the shadow entirely. */
    CHECK(sh_overrides_test_open("generated/decls/snapeditorentitydef/func/lift.decl", 2) == NULL);
    /* A name that only a user package could have supplied is not served. */
    CHECK(sh_overrides_test_open("generated/decls/entitydef/custom/user_package_only.decl", 0) == NULL);
    CHECK(sh_overrides_test_open("strings/policy.json", 0) == NULL);
}

/* Native-stream slots: the backend reads installed resources through the
 * 34-slot idFile layout of build 13954591 (Read +0x30, GetLength +0x68). The
 * fake native stream answers only at those slots; the 2018 slots (+0x28 Read,
 * +0x58 GetLength) return what the 2024 engine returns there: a field getter
 * and 0. */
static const char g_native_body[] = "entityDef stock_module { }";
static int g_native_closed;
typedef struct fake_native_stream { void **vtable; long long position; } fake_native_stream;
static long long fake_stale_getter(fake_native_stream *s) { (void)s; return 0x1234; }
static long long fake_stale_zero(fake_native_stream *s) { (void)s; return 0; }
static long long fake_read(fake_native_stream *s, void *buffer, unsigned long long n)
{
    long long left = (long long)sizeof g_native_body - 1 - s->position;
    long long take = (long long)n < left ? (long long)n : left;
    memcpy(buffer, g_native_body + s->position, (size_t)take);
    s->position += take;
    return take;
}
static long long fake_length(fake_native_stream *s) { (void)s; return (long long)sizeof g_native_body - 1; }
static void fake_close(fake_native_stream *s) { g_native_closed++; HeapFree(GetProcessHeap(), 0, s); }
static void *g_fake_native_vtable[SH_OVERRIDES_STREAM_SLOTS];
static void *g_open_provider;
static unsigned int g_open_mode;
static void *fake_native_open(void *self, const char *name, unsigned char b1, unsigned char b2,
                              unsigned int mode)
{
    fake_native_stream *s;
    (void)b1; (void)b2;
    g_open_provider = self; g_open_mode = mode;
    if (strcmp(name, "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl")) return NULL;
    s = (fake_native_stream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (s) s->vtable = g_fake_native_vtable;
    return s;
}
static const unsigned char g_own_body[] = "own stream body";
static void *own_stream_open(void *self, const char *name, unsigned char b1, unsigned char b2,
                             unsigned int mode)
{
    (void)self; (void)name; (void)b1; (void)b2; (void)mode;
    return sh_overrides_test_memory_stream(g_own_body, sizeof g_own_body - 1);
}

static void test_native_stream_slots(void)
{
    int provider;
    size_t len = 99;
    unsigned char *data;
    for (size_t i = 0; i < SH_OVERRIDES_STREAM_SLOTS; i++) g_fake_native_vtable[i] = (void *)fake_stale_zero;
    g_fake_native_vtable[SH_IDFILE_SLOT_CLOSE] = (void *)fake_close;
    g_fake_native_vtable[5] = (void *)fake_stale_getter;    /* 2018 Read slot (+0x28) */
    g_fake_native_vtable[11] = (void *)fake_stale_zero;     /* 2018 GetLength slot (+0x58) */
    g_fake_native_vtable[SH_IDFILE_SLOT_READ] = (void *)fake_read;
    g_fake_native_vtable[SH_IDFILE_SLOT_LENGTH] = (void *)fake_length;

    CHECK(SH_IDFILE_SLOT_READ == 6 && SH_IDFILE_SLOT_LENGTH == 13 && SH_IDFILE_SLOT_CLOSE == 0);
    CHECK(SH_IDFILE_SLOT_READ * 8 == 0x30 && SH_IDFILE_SLOT_LENGTH * 8 == 0x68);

    g_native_closed = 0;
    data = sh_overrides_test_read_installed((void *)fake_native_open, &provider,
                                            "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl", &len);
    CHECK(data != NULL);
    CHECK(len == sizeof g_native_body - 1);
    CHECK(data && !memcmp(data, g_native_body, sizeof g_native_body - 1));
    CHECK(g_native_closed == 1);
    CHECK(g_open_provider == &provider && g_open_mode == 2);
    if (data) HeapFree(GetProcessHeap(), 0, data);

    len = 99;
    CHECK(sh_overrides_test_read_installed((void *)fake_native_open, &provider, "missing.decl", &len) == NULL);
    CHECK(len == 0);

    /* The module's own stream implements the same layout, so it reads back too. */
    len = 0;
    data = sh_overrides_test_read_installed((void *)own_stream_open, &provider, "any", &len);
    CHECK(data != NULL && len == sizeof g_own_body - 1);
    CHECK(data && !memcmp(data, g_own_body, sizeof g_own_body - 1));
    if (data) HeapFree(GetProcessHeap(), 0, data);
}

int main(void)
{
    test_stream_abi_and_memory_streams();
    test_file_stream_is_read_only();
    test_builtin_defaults_without_user_files();
    test_native_stream_slots();
    if (g_failed) {
        fprintf(stderr, "%d overrides provider test(s) failed\n", g_failed);
        return 1;
    }
    puts("overrides provider tests passed");
    return 0;
}

/* AAS validation is exercised by grid_room_nav_test, not these lookup tests. */
int sh_navmesh_validate_aas(const unsigned char *p,size_t n,char *e,size_t cap)
{(void)p;(void)n;if(e&&cap)e[0]=0;return 0;}
