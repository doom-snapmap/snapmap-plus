/* palette_refresh_test.c -- per-registration palette rebuild service and exact gating. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_image.h"
#include "engine_globals.h"
#include "palette_refresh.h"
#include "hook.h"

#define TEST_EDITOR_SINGLETON_RVA 0x3056748u
#define TEST_EDITOR_PALETTE_OFF   0x20660u
#define TEST_EDITOR_INIT_OFF      0x08u
/* Use representative RVAs inside read-only and executable fixture sections;
 * the gate checks section properties rather than pinned addresses. */
#define TEST_PALETTE_VTABLE_RVA   0x20499A0u
#define TEST_BUILDER_RVA          0x54AEE0u
#define TEST_TEXT_RVA             0x0001000u
#define TEST_RDATA_RVA            0x1001000u
#define TEST_MODULE_BYTES         (TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF + 0x2000u)

static const uint8_t *g_test_editor;
static int g_builder_calls;
static void *g_last_palette;
static void *g_last_progress;
static int g_builder_raises;
static int g_builder_reenter;
static int g_nested_result, g_nested_before, g_nested_after;
static int g_pinned_build = 1;
static int g_globals_resolvable = 1;
static unsigned char g_template_entity[0x158], g_template_decl[0x68];
static volatile LONG g_decoder_calls;
static int g_decode_in_builder, g_decode_other_thread;
static void *g_last_decode_entity, *g_last_decode_data;
static sh_patch_status g_commit_result = B2_PATCH_OK;
static int g_unpatch_result = 1, g_prepare_fails, g_hook_contract_failed;
static void (*g_detour)(void *, void *);

static void fake_decoder(void *entity, void *data)
{
    InterlockedIncrement(&g_decoder_calls);
    g_last_decode_entity = entity; g_last_decode_data = data;
}

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    if (!target || !detour || stolen != 18) g_hook_contract_failed++;
    g_detour = (void (*)(void *, void *))detour;
    return g_prepare_fails ? NULL : (void *)fake_decoder;
}
sh_patch_status hook_commit(void *trampoline)
{
    if (trampoline != (void *)fake_decoder) g_hook_contract_failed++;
    /* Even a failed commit may have exposed the detour to another caller. */
    g_detour(g_template_entity, (void *)0x1234);
    return g_commit_result;
}
int hook_unpatch(void *trampoline) { return g_unpatch_result; }

static DWORD WINAPI decode_other_thread(void *unused)
{
    (void)unused;
    sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
    return 0;
}

void backend_log(const char *message)
{
    (void)message;
}

const uint8_t *sh_iface_engine_editor_base(void)
{
    return g_test_editor;
}

int sh_host_is_pinned_rva_build(void)
{
    return g_pinned_build;
}

/* Resolve the fake singleton unless g_globals_resolvable disables it. */
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{
    if (!g_globals_resolvable || strcmp(name, "editor_singleton") != 0) {
        if (out_status) *out_status = GLB_ANCHOR_NOT_FOUND;
        return 0;
    }
    if (out_status) *out_status = GLB_OK;
    return (uintptr_t)(module_base + TEST_EDITOR_SINGLETON_RVA);
}

/* Provide executable builder and read-only vtable sections for the PE checks. */
static void write_pe_headers(uint8_t *module)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;

    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    nt = (IMAGE_NT_HEADERS64 *)(module + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = (WORD)sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = TEST_MODULE_BYTES;

    sec = IMAGE_FIRST_SECTION(nt);
    memcpy(sec[0].Name, ".text", 6);
    sec[0].VirtualAddress   = TEST_TEXT_RVA;
    sec[0].Misc.VirtualSize = TEST_RDATA_RVA - TEST_TEXT_RVA;
    sec[0].Characteristics  = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    memcpy(sec[1].Name, ".rdata", 7);
    sec[1].VirtualAddress   = TEST_RDATA_RVA;
    sec[1].Misc.VirtualSize = TEST_MODULE_BYTES - TEST_RDATA_RVA;
    sec[1].Characteristics  = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
}

static void fake_palette_builder(void *palette, void *progress)
{
    g_builder_calls++;
    g_last_palette = palette;
    g_last_progress = progress;
    if (g_decode_in_builder) sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
    if (g_decode_other_thread) {
        HANDLE thread = CreateThread(NULL, 0, decode_other_thread, NULL, 0, NULL);
        if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) g_hook_contract_failed++;
        if (thread) CloseHandle(thread);
    }
    if (g_builder_reenter) {
        g_builder_reenter = 0;
        g_nested_before = sh_palette_refresh_test_state();
        g_nested_result = sh_palette_refresh_after_decl_registration();
        g_nested_after = sh_palette_refresh_test_state();
    }
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
    sh_palette_refresh_test_decoder((void *)fake_decoder);
    g_decode_in_builder = g_decode_other_thread = 0;
    InterlockedExchange(&g_decoder_calls, 0);
    g_builder_calls = 0;
    g_last_palette = NULL;
    g_last_progress = (void *)(uintptr_t)1;
    g_builder_raises = 0;
    g_builder_reenter = 0;
    g_nested_result = g_nested_before = g_nested_after = -1;
}

/* Accept a clean unique signature result at either supported image's
 * address; reject hooked status and inconsistent address/RVA pairs. */
static void test_install_exact_gate(uint8_t *module, int *failed_out)
{
    int failed = *failed_out;
    sig_result results[2];
#define result results[0]

    memset(&result, 0, sizeof(result));
    result.name = "SnapPaletteBuild";
    result.status = SIG_OK;
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA;
    result.rva = TEST_BUILDER_RVA;
    memset(&results[1], 0, sizeof(results[1]));
    results[1].name = "SnapEntityTraversalDecode";
    results[1].status = SIG_OK;
    results[1].rva = 0x543660;
    results[1].addr = (uintptr_t)module + results[1].rva;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 1);

    /* Still SIG_OK only: this call has a vtable/data-layout contract, so the hook-tolerant
     * known_rva fallback is not good enough even though it is callable. */
    sh_palette_refresh_test_reset();
    result.status = SIG_OK_HOOKED;
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    /* The same clean match at a shifted address -- what the other shipped build looks like -- is
     * accepted, where the old pinned-RVA gate refused it and silently disabled the service. */
    result.status = SIG_OK;
    result.rva = TEST_BUILDER_RVA - 0xE460u;
    result.addr = (uintptr_t)module + result.rva;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 1);

    /* What is still refused is a resolve whose address and RVA disagree about the image base: the
     * cached builder pointer and the base the editor is derived from must describe one module. */
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA;
    result.rva = TEST_BUILDER_RVA + 1;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    result.rva = TEST_BUILDER_RVA;
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA + 1;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    result.addr = (uintptr_t)module + result.rva;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 1, module) == 0);
    results[1].status = SIG_OK_HOOKED;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    results[1].status = SIG_OK;
    results[1].addr++;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    results[1].addr--;
    g_prepare_fails = 1;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(results, 2, module) == 0);
    g_prepare_fails = 0;
    for (g_unpatch_result = 0; g_unpatch_result <= 1; g_unpatch_result++) {
        LONG before = g_decoder_calls;
        g_commit_result = B2_PATCH_FAIL_ROLLBACK;
        sh_palette_refresh_test_reset();
        CHECK(sh_palette_refresh_install(results, 2, module) == 0);
        CHECK(g_decoder_calls == before + 1); /* Original callback precedes commit. */
        if (!g_unpatch_result) {
            sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
            CHECK(g_decoder_calls == before + 2); /* Failed rollback retains callback. */
        }
    }
    g_commit_result = B2_PATCH_OK; g_unpatch_result = 1;
    CHECK(g_hook_contract_failed == 0);
    *failed_out = failed;
#undef result
}

static void test_template_scope(uint8_t *module, int *failed_out)
{
    int failed = *failed_out;
    bind_fake(module); setup_editor(module, 1, 1);
    *(void **)(g_template_entity + 0x150) = g_template_decl;
    *(const char **)(g_template_decl + 8) = "";
    *(const char **)(g_template_decl + 0x60) = "idInfo_UniversalTraversal";
    sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
    CHECK(g_decoder_calls == 1); /* Actual decode outside catalog construction. */
    g_decode_in_builder = 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_decoder_calls == 1);
    g_decode_other_thread = 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_decoder_calls == 2);
    g_decode_other_thread = 0;
    *(const char **)(g_template_decl + 8) = "authored/traversal";
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_decoder_calls == 3);
    *(const char **)(g_template_decl + 8) = "";
    *(const char **)(g_template_decl + 0x60) = "idInfoTraversal";
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_decoder_calls == 4);
    CHECK(g_last_decode_entity == g_template_entity && g_last_decode_data == (void *)0x1234);
    *(void **)(g_template_entity + 0x150) = (void *)1;
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_decoder_calls == 5); /* Invalid identity is forwarded, never swallowed. */
    *(void **)(g_template_entity + 0x150) = g_template_decl;
    *(const char **)(g_template_decl + 0x60) = "idInfo_UniversalTraversal";
    g_builder_raises = 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(g_decoder_calls == 5);
    sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
    CHECK(g_decoder_calls == 6); /* Scope unwinds even when the builder throws. */
    CHECK(g_hook_contract_failed == 0);
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
    write_pe_headers(module);

    test_install_exact_gate(module, &failed);
    test_template_scope(module, &failed);

    bind_fake(module);
    setup_editor(module, 1, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(g_last_palette == (void *)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF));
    CHECK(g_last_progress == NULL);
    /* Each registration pass must rebuild the palette from the updated decl list. */
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 2);
    CHECK(sh_palette_refresh_test_call_count() == 2);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_builder_calls == 3);

    /* A nested attempt cannot replace the live palette during its outer build.
     * Refusing that attempt must not poison the completed outer pass. */
    bind_fake(module);
    setup_editor(module, 1, 1);
    g_builder_reenter = 1;
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_nested_before == SH_PALETTE_REFRESH_TEST_PENDING);
    CHECK(g_nested_result == 0);
    CHECK(g_nested_after == SH_PALETTE_REFRESH_TEST_PENDING);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_builder_calls == 2);

    bind_fake(module);
    setup_editor(module, 0, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(g_last_palette == (void *)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF));
    CHECK(g_last_progress == NULL);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_builder_calls == 2);
    CHECK(sh_palette_refresh_test_call_count() == 2);

    /* A refused service stays terminal even if its objects later look valid. */
    bind_fake(module);
    setup_editor(module, 0, 0);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);
    setup_editor(module, 0, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    /* Reject an executable-section pointer where a read-only vtable is required. */
    bind_fake(module);
    setup_editor(module, 1, 1);
    *(void **)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF) =
        (void *)(module + TEST_BUILDER_RVA);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    /* If the globals resolver cannot locate the editor singleton, refuse
     * without dereferencing a pinned-address fallback. */
    bind_fake(module);
    setup_editor(module, 1, 1);
    g_globals_resolvable = 0;
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);
    g_globals_resolvable = 1;

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
