/* palette_refresh_test.c -- per-registration palette rebuild service and exact gating. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_image.h"
#include "engine_globals.h"
#include "palette_refresh.h"

#define TEST_EDITOR_SINGLETON_RVA 0x3056748u
#define TEST_EDITOR_PALETTE_OFF   0x20660u
#define TEST_EDITOR_INIT_OFF      0x08u
/* Just "an address in a read-only section" and "an address in an executable section" now -- the
 * service no longer compares either against a recorded RVA, only against the section it lands in.
 * They keep the pinned build's real values so the test image resembles the thing it stands in for. */
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
static int g_pinned_build = 1;
static int g_globals_resolvable = 1;

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

/* palette-refresh locates the editor singleton through the globals resolver. Here it returns the
 * fake module's singleton, or nothing when g_globals_resolvable is cleared -- which is how the
 * "build we cannot place the singleton on" case is exercised. */
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{
    if (!g_globals_resolvable || strcmp(name, "editor_singleton") != 0) {
        if (out_status) *out_status = GLB_ANCHOR_NOT_FOUND;
        return 0;
    }
    if (out_status) *out_status = GLB_OK;
    return (uintptr_t)(module_base + TEST_EDITOR_SINGLETON_RVA);
}

/* Give the fake module enough of a PE image for the section walk that validates the palette vtable:
 * an executable .text covering the builder and a read-only .rdata covering the palette vtable and
 * the editor singleton. Both shipped DOOM builds have exactly this shape. */
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

/* The builder is admitted on the strength of a CLEAN UNIQUE masked-signature match, which is
 * stronger evidence than RVA equality -- two builds can share an RVA by coincidence, a signature
 * cannot match the wrong function -- and, unlike RVA equality, it survives the second shipped DOOM
 * executable, where every function sits at a different address. */
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

    /* Still SIG_OK only: this call has a vtable/data-layout contract, so the hook-tolerant
     * known_rva fallback is not good enough even though it is callable. */
    sh_palette_refresh_test_reset();
    result.status = SIG_OK_HOOKED;
    CHECK(sh_palette_refresh_install(&result, 1, module) == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);

    /* The same clean match at a shifted address -- what the other shipped build looks like -- is
     * accepted, where the old pinned-RVA gate refused it and silently disabled the service. */
    result.status = SIG_OK;
    result.rva = TEST_BUILDER_RVA - 0xE460u;
    result.addr = (uintptr_t)module + result.rva;
    sh_palette_refresh_test_reset();
    CHECK(sh_palette_refresh_install(&result, 1, module) == 1);

    /* What is still refused is a resolve whose address and RVA disagree about the image base: the
     * cached builder pointer and the base the editor is derived from must describe one module. */
    result.addr = (uintptr_t)module + TEST_BUILDER_RVA;
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
    write_pe_headers(module);

    test_install_exact_gate(module, &failed);

    bind_fake(module);
    setup_editor(module, 1, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 1);
    CHECK(sh_palette_refresh_test_call_count() == 1);
    CHECK(g_last_palette == (void *)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF));
    CHECK(g_last_progress == NULL);
    /* A later registration pass rebuilds again. The palette is a catalog DERIVED
     * from the decl list, so a package installed mid-session extends that list
     * and the catalog has to be rebuilt from it or the new types stay invisible
     * to every consumer that searches the catalog by name. */
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_APPLIED);
    CHECK(g_builder_calls == 2);
    CHECK(sh_palette_refresh_test_call_count() == 2);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(g_builder_calls == 3);

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

    /* A refusal is an integrity verdict on the engine objects this service calls
     * into, so it stays terminal: re-arming must not give a refused process a
     * second chance to call the native builder. */
    bind_fake(module);
    setup_editor(module, 0, 0);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);
    setup_editor(module, 0, 1);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    /* A palette pointer inside the image but in an EXECUTABLE section is not a vtable. The check
     * is what the pointer plausibly IS, not which build's address it equals -- the pointer is read
     * out of the live editor object, so it is already correct for whichever build we are in. */
    bind_fake(module);
    setup_editor(module, 1, 1);
    *(void **)(module + TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF) =
        (void *)(module + TEST_BUILDER_RVA);
    CHECK(sh_palette_refresh_after_decl_registration() == 0);
    CHECK(sh_palette_refresh_test_state() == SH_PALETTE_REFRESH_TEST_REFUSED);
    CHECK(g_builder_calls == 0);

    /* The editor singleton is still located by a raw pinned-build DATA RVA with no signature behind
     * it, so when the resolver cannot place the singleton the service FAILS CLOSED rather than
     * dereference unrelated memory. A wrong pointer is worse than none: the caller cannot tell. */
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
