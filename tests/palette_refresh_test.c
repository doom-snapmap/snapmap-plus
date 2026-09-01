/* palette_refresh_test.c -- native one-shot palette service and exact gating. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "palette_refresh.h"

#define TEST_EDITOR_SINGLETON_RVA 0x3056748u
#define TEST_EDITOR_PALETTE_OFF   0x20660u
#define TEST_EDITOR_INIT_OFF      0x08u
#define TEST_PALETTE_VTABLE_RVA   0x20499A0u
#define TEST_BUILDER_RVA          0x54AEE0u
#define TEST_MODULE_BYTES         (TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF + 0x2000u)

static const uint8_t *g_test_editor;
static int g_builder_calls;
static void *g_last_palette;
static void *g_last_progress;
static int g_builder_raises;

void backend_log(const char *message)
{
    (void)message;
}

const uint8_t *sh_iface_engine_editor_base(void)
{
    return g_test_editor;
}

static void fake_palette_builder(void *palette, void *progress)
{
    g_builder_calls++;
    g_last_palette = palette;
    g_last_progress = progress;
    if (g_builder_raises)
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
}

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
    }                                                                           \
} while (0)

static void *make_module(void)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TEST_MODULE_BYTES);
}

static void setup_editor(uint8_t *module, unsigned char init_byte, int valid_vtable)
{
    uint8_t *editor = module + TEST_EDITOR_SINGLETON_RVA;
    void **palette = (void **)(editor + TEST_EDITOR_PALETTE_OFF);
    g_test_editor = editor;
    *palette = valid_vtable ? (void *)(module + TEST_PALETTE_VTABLE_RVA) : (void *)0x1234;
    *(volatile unsigned char *)(editor + TEST_EDITOR_INIT_OFF) = init_byte;
}

static void bind_fake(uint8_t *module)
{
    sh_palette_refresh_test_reset();
    sh_palette_refresh_test_bind(module, (void *)fake_palette_builder);
    g_builder_calls = 0;
    g_last_palette = NULL;
    g_last_progress = (void *)(uintptr_t)1;
    g_builder_raises = 0;
}

static void test_install_exact_gate(uint8_t *module, int *failed_out)
{
    int failed = *failed_out;
    sig_result result;

    memset(&result, 0, sizeof(result));
    result.name = "SnapPaletteBuild";
    result.status = SIG_OK;
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA;
    result.rva = TEST_BUILDER_RVA;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(&result, 1, module) == 1);

    sh_palette_refresh_test_reset();
    result.status = SIG_OK_HOOKED;
    CHECK(sh_palette_refresh_install(&result, 1, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    result.status = SIG_OK;
    result.rva = TEST_BUILDER_RVA + 1;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(&result, 1, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    result.rva = TEST_BUILDER_RVA;
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA + 1;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(&result, 1, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    *failed_out = failed;
}

int main(void)
{
    int failed = 0;
    uint8_t *module = (uint8_t *)make_module();
    SYSTEM_INFO system_info;
    DWORD old_protect = 0;
    void *page;

    if (!module) {
        fprintf(stderr, "module allocation failed\n");
        return 2;
    }

    test_install_exact_gate(module, &failed);

    bind_fake(module);
    setup_editor(module, 1, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(g_last_palette == (void *)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF));
    CHECK(g_last_progress == NULL);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);

    bind_fake(module);
    setup_editor(module, 0, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(g_last_palette == (void *)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF));
    CHECK(g_last_progress == NULL);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);

    bind_fake(module);
    setup_editor(module, 0, 0);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    bind_fake(module);
    setup_editor(module, 1, 1);
    g_test_editor = module + TEST_EDITOR_SINGLETON_RVA + 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    bind_fake(module);
    setup_editor(module, 1, 1);
    GetSystemInfo(&system_info);
    page = (void *)((uintptr_t)(g_test_editor + TEST_EDITOR_PALETTE_OFF) &
                    ~((uintptr_t)system_info.dwPageSize - 1u));
    CHECK(VirtualProtect(page, system_info.dwPageSize, PAGE_NOACCESS, &old_protect) != 0);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(VirtualProtect(page, system_info.dwPageSize, old_protect, &old_protect) != 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    bind_fake(module);
    setup_editor(module, 1, 1);
    g_builder_raises = 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(g_builder_calls == 1);

    HeapFree(GetProcessHeap(), 0, module);
    if (failed) {
        fprintf(stderr, "%d palette-refresh test(s) failed\n", failed);
        return 1;
    }
    puts("palette refresh tests passed");
    return 0;
}
