/* palette_refresh_test.c -- per-registration palette rebuild service and exact gating. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
#define TEST_MODULE_BYTES         (TEST_EDITOR_SINGLETON_RVA + TEST_EDITOR_PALETTE_OFF + 0x4000u)

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
static void *(*g_find_detour)(void *, const char *);
static const char *g_native_name;
static void *g_native_row[2];
static int g_entry_ctor_calls, g_entry_dtor_calls, g_entry_ctor_raises, g_entry_dtor_raises;
static void *g_invalid_entry;

static void *fake_find(void *palette, const char *name)
{
    if (g_native_name && !strcmp(g_native_name, name)) return g_native_row;
    return *(void **)((unsigned char *)palette + 0x28);
}
static unsigned char fake_entry_valid(void *decl) { return decl != g_invalid_entry; }
static void *fake_entry_ctor(void *body, void *decl)
{
    ++g_entry_ctor_calls;
    *(void **)body = decl;
    *(void **)((unsigned char *)body + 0x150) = *(void **)((unsigned char *)decl + 0x1c8);
    sh_palette_refresh_test_decode(g_template_entity, (void *)0x1234);
    g_nested_result = sh_palette_refresh_after_decl_registration();
    if (g_entry_ctor_raises) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return body;
}
static void fake_entry_dtor(void *body)
{
    (void)body; ++g_entry_dtor_calls;
    if (g_entry_dtor_raises) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
}

static void fake_decoder(void *entity, void *data)
{
    InterlockedIncrement(&g_decoder_calls);
    g_last_decode_entity = entity; g_last_decode_data = data;
}

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    if (stolen == 16) {
        if (!target || !detour) g_hook_contract_failed++;
        g_find_detour = (void *(*)(void *, const char *))detour;
        return g_prepare_fails ? NULL : (void *)fake_find;
    }
    if (!target || !detour || stolen != 18) g_hook_contract_failed++;
    g_detour = (void (*)(void *, void *))detour;
    return g_prepare_fails ? NULL : (void *)fake_decoder;
}
sh_patch_status hook_commit(void *trampoline)
{
    if (trampoline == (void *)fake_find) {
        void *palette = (void *)(g_test_editor + TEST_EDITOR_PALETTE_OFF);
        if (g_find_detour(palette, "absent") != *(void **)((unsigned char *)palette + 0x28))
            g_hook_contract_failed++;
        return g_commit_result;
    }
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

static int fake_admitted(void *context) { return *(int *)context; }
static void setup_entry(unsigned char *decl, unsigned char *entity, const char *name)
{
    memset(decl, 0, 0x500); memset(entity, 0, 0x70);
    *(void **)(decl + 0x1c8) = entity;
    /* Native entityDef resource name; the constructed defsub uses +0x58. */
    *(const char **)(entity + 8) = name;
}
static void bind_entries(uint8_t *module)
{
    bind_fake(module); setup_editor(module, 1, 1);
    sh_palette_entries_test_bind(fake_find, fake_entry_ctor, fake_entry_valid, fake_entry_dtor);
    *(void **)(g_test_editor + TEST_EDITOR_PALETTE_OFF + 0x10) = (void *)0x1110;
    *(void **)(g_test_editor + TEST_EDITOR_PALETTE_OFF + 0x28) = (void *)0x2220;
    *(void **)(g_template_entity + 0x150) = g_template_decl;
    *(const char **)(g_template_decl + 8) = "";
    *(const char **)(g_template_decl + 0x60) = "idInfo_UniversalTraversal";
    g_native_name = NULL; g_invalid_entry = NULL;
    g_entry_ctor_calls = g_entry_dtor_calls = g_entry_ctor_raises = g_entry_dtor_raises = 0;
}

static void test_retained_entries(uint8_t *module, int *failed_out)
{
    int failed = *failed_out, admitted = 1;
    unsigned char decl_a[0x500], entity_a[0x70], decl_b[0x500], entity_b[0x70];
    sh_palette_entry_source rows[2];
    sh_palette_entries *a = NULL, *b = NULL, *extra = NULL;
    void *palette, *end, *first;
    char error[256];
    setup_entry(decl_a, entity_a, "private/a"); setup_entry(decl_b, entity_b, "private/b");
    rows[0] = (sh_palette_entry_source){"private/a", decl_a};
    rows[1] = (sh_palette_entry_source){"private/b", decl_b};
    bind_entries(module); palette = (void *)(g_test_editor + TEST_EDITOR_PALETTE_OFF);
    end = *(void **)((unsigned char *)palette + 0x28);
    CHECK(sh_palette_entries_ready());
    admitted = 0;
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 0);
    CHECK(!a && !g_entry_ctor_calls);
    admitted = 1;
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 1);
    CHECK(g_entry_ctor_calls == 1 && g_decoder_calls == 0); /* Existing traversal guard applies. */
    CHECK(g_nested_result == 0 && g_builder_calls == 0);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == end); /* Invisible until publish. */
    CHECK(sh_palette_entries_publish(a, fake_admitted, &admitted, error, sizeof(error)) == 1);
    first = sh_palette_entries_test_find(palette, "private/a");
    CHECK(first != end && *(void **)first == decl_a);
    CHECK(sh_palette_entries_test_find(palette, "PRIVATE/a") == end);
    rows[1] = rows[0];
    CHECK(sh_palette_entries_prepare(rows, 2, fake_admitted, &admitted, &b, error, sizeof(error)) == 1);
    CHECK(g_entry_ctor_calls == 1 && sh_palette_entries_test_count() == 1);
    CHECK(sh_palette_entries_publish(b, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == 1 && !a);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == first && !g_entry_dtor_calls);
    extra = b; CHECK(sh_palette_entries_retain(extra));
    CHECK(sh_palette_entries_end(&b, fake_admitted, &admitted, error, sizeof(error)) == 1 && !b);
    admitted = 0;
    CHECK(sh_palette_entries_end(&extra, fake_admitted, &admitted, error, sizeof(error)) == 0 && extra);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == first);
    admitted = 1;
    CHECK(sh_palette_entries_end(&extra, fake_admitted, &admitted, error, sizeof(error)) == 1 && !extra);
    CHECK(g_entry_dtor_calls == 1 && sh_palette_entries_test_count() == 0);
    CHECK(*(void **)((unsigned char *)palette + 0x10) == (void *)0x1110);
    CHECK(*(void **)((unsigned char *)palette + 0x28) == end);

    /* Native storage remains authoritative; a conflicting declaration cannot
     * acquire that alias, even before any extension row exists. */
    g_native_name = "private/a"; g_native_row[0] = decl_b;
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 0 && !a);
    CHECK(g_entry_ctor_calls == 1);
    g_native_row[0] = decl_a;
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(a, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == g_native_row);
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == 1);
    g_native_name = NULL;

    /* Failure later in a batch destroys its earlier private preparation and
     * leaves an already published owner intact. */
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(a, fake_admitted, &admitted, error, sizeof(error)) == 1);
    first = sh_palette_entries_test_find(palette, "private/a");
    rows[1] = (sh_palette_entry_source){"private/b", decl_b}; g_invalid_entry = decl_b;
    CHECK(sh_palette_entries_prepare(rows, 2, fake_admitted, &admitted, &b, error, sizeof(error)) == 0 && !b);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == first && sh_palette_entries_test_count() == 1);
    g_invalid_entry = NULL;
    *(const char **)(entity_b + 8) = "private/a";
    rows[1].entity_def_name = "private/a";
    CHECK(sh_palette_entries_prepare(rows + 1, 1, fake_admitted, &admitted, &b, error, sizeof(error)) == 0 && !b);
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == 1);

    /* Native construction/destruction faults retain explicit bounded ownership;
     * no retry destroys an incomplete object or silently drops source lifetime. */
    g_entry_ctor_raises = 1;
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == -1 && a);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == end && !sh_palette_entries_ready());
    int before = g_entry_dtor_calls;
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == -1 && a);
    CHECK(g_entry_dtor_calls == before);
    sh_palette_entries_test_discard_quarantine(&a);
    bind_entries(module);
    CHECK(sh_palette_entries_prepare(rows, 1, fake_admitted, &admitted, &a, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(a, fake_admitted, &admitted, error, sizeof(error)) == 1);
    g_entry_dtor_raises = 1;
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == -1 && a);
    CHECK(sh_palette_entries_test_find(palette, "private/a") == end && g_entry_dtor_calls == 1);
    CHECK(sh_palette_entries_end(&a, fake_admitted, &admitted, error, sizeof(error)) == -1 && g_entry_dtor_calls == 1);
    sh_palette_entries_test_discard_quarantine(&a);
    CHECK(sh_palette_entries_test_count() == 0);
    *failed_out = failed;
}

/* This model owns independent allocations like the native typed list helpers.
 * The paired image gate proves the actual helper addresses and call paths. */
typedef struct test_native_list {
    void *data;
    int count, capacity;
    unsigned short granularity;
    unsigned char tag, kind;
    unsigned int padding;
} test_native_list;
typedef struct test_native_palette {
    void *vtable;
    int selected;
    unsigned int padding;
    test_native_list bodies;
    void *end;
    test_native_list groups, labels;
} test_native_palette;
static int g_auth_copies, g_auth_destroys, g_auth_resize_fail, g_auth_copy_raise;
static int g_auth_add, g_auth_filter, g_auth_widget, g_auth_destroy_raise, g_auth_refresh_raise;
static unsigned char g_auth_ui[0x9c8];
static void *g_auth_mode = (void *)0x34560, *g_auth_addvt = (void *)0x34570, *g_auth_filtervt = (void *)0x34580;
static test_native_palette *auth_palette(void)
{ return (test_native_palette *)(g_test_editor + TEST_EDITOR_PALETTE_OFF); }
static unsigned char auth_resize(void *value, int count)
{
    test_native_list *list = value;
    if (g_auth_resize_fail) return 0;
    list->data = calloc((size_t)count, 0x6f0); list->capacity = count;
    return list->data != NULL;
}
static void auth_string_init(unsigned char *value)
{
    memset(value, 0, 0x30); *(void **)(value + 0x10) = value + 0x1c;
    *(unsigned int *)(value + 0x18) = 0x80000014u;
}
static void auth_string_release(unsigned char *value)
{
    void *data = *(void **)(value + 0x10);
    if (data && data != value + 0x1c) free(data);
}
static void auth_string_assign(void *destination, const void *source)
{
    unsigned char *dst = destination;
    const unsigned char *src = source;
    int length = *(const int *)(src + 8);
    auth_string_release(dst); auth_string_init(dst);
    *(int *)(dst + 8) = length;
    if (length >= 20) *(void **)(dst + 0x10) = malloc((size_t)length + 1);
    memcpy(*(void **)(dst + 0x10), *(const void *const *)(src + 0x10), (size_t)length + 1);
}
static void auth_string_set(unsigned char *dst, const char *text)
{
    unsigned char source[0x30] = {0};
    *(int *)(source + 8) = (int)strlen(text); *(const char **)(source + 0x10) = text;
    auth_string_assign(dst, source);
}
static void auth_groups_grow(void *value)
{
    test_native_list *list = value;
    int capacity = list->capacity ? list->capacity * 2 : 1;
    void *data = calloc((size_t)capacity, sizeof(void *));
    if (list->count) memcpy(data, list->data, (size_t)list->count * sizeof(void *));
    free(list->data); list->data = data; list->capacity = capacity;
}
static void auth_labels_grow(void *value)
{
    test_native_list *list = value;
    int capacity = list->capacity ? list->capacity * 2 : 1;
    unsigned char *data = calloc((size_t)capacity, 0x30);
    for (int i = 0; i < capacity; i++) auth_string_init(data + (size_t)i * 0x30);
    for (int i = 0; i < list->count; i++)
        auth_string_assign(data + (size_t)i * 0x30, (unsigned char *)list->data + (size_t)i * 0x30);
    for (int i = 0; i < list->capacity; i++) auth_string_release((unsigned char *)list->data + (size_t)i * 0x30);
    free(list->data); list->data = data; list->capacity = capacity;
}
static void *auth_body_copy(void *destination, const void *source)
{
    ++g_auth_copies; memcpy(destination, source, 0x6f0);
    if (g_auth_copy_raise) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return destination;
}
static void auth_destroy(void *value)
{
    test_native_palette *palette = value;
    ++g_auth_destroys;
    if (g_auth_destroy_raise) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    for (int i = 0; i < palette->labels.capacity; i++)
        auth_string_release((unsigned char *)palette->labels.data + (size_t)i * 0x30);
    free(palette->labels.data); free(palette->groups.data); free(palette->bodies.data);
    memset(&palette->bodies, 0, sizeof(*palette) - offsetof(test_native_palette, bodies));
}
static void auth_categories(test_native_palette *palette, int visible, const char *category)
{
    palette->end = (unsigned char *)palette->bodies.data + (size_t)visible * 0x6f0;
    while (palette->groups.capacity < 2) auth_groups_grow(&palette->groups);
    while (palette->labels.capacity < 1) auth_labels_grow(&palette->labels);
    palette->groups.count = 2; palette->labels.count = 1;
    ((void **)palette->groups.data)[0] = palette->bodies.data;
    ((void **)palette->groups.data)[1] = palette->end;
    auth_string_set(palette->labels.data, category);
}
static unsigned char auth_prepare_add(void *state, void *editor)
{
    (void)state; (void)editor; ++g_auth_add;
    auth_categories(auth_palette(), auth_palette()->bodies.count, "Authored category - retained exact bytes");
    if (g_auth_refresh_raise) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return 1;
}
static void auth_prepare_filter(void *state, void *editor)
{
    (void)state; (void)editor; ++g_auth_filter;
    auth_categories(auth_palette(), auth_palette()->bodies.count, "Filtered category");
}
static void auth_refresh_widget(void *widget, void *palette)
{ (void)widget; (void)palette; ++g_auth_widget; }
static void auth_rebuild(void *palette, void *progress)
{ (void)progress; auth_destroy(palette); }
static void auth_rebuild_same_address(void *value, void *progress)
{
    test_native_palette *palette = value;
    (void)progress;
    /* Model an engine allocator releasing and immediately recycling this
     * backing block. All previous logical body contents are destroyed. */
    ++g_auth_destroys;
    memset(palette->bodies.data, 0x6b, (size_t)palette->bodies.capacity * 0x6f0);
    auth_categories(palette, palette->bodies.count, "Rebuilt category");
}
static void auth_bind(uint8_t *module, void *old_decl, int tool)
{
    sh_palette_authoring_test_api api = {auth_resize, auth_groups_grow, auth_labels_grow,
        auth_body_copy, auth_string_assign, auth_destroy, auth_prepare_add,
        auth_prepare_filter, auth_refresh_widget, g_auth_mode, g_auth_addvt, g_auth_filtervt};
    bind_entries(module); sh_palette_authoring_test_bind(&api);
    test_native_palette *palette = auth_palette();
    memset((unsigned char *)palette + 8, 0, sizeof(*palette) - 8);
    auth_resize(&palette->bodies, 2); palette->bodies.count = 2;
    *(void **)palette->bodies.data = old_decl;
    *(void **)((unsigned char *)palette->bodies.data + 0x6f0) = old_decl;
    ((unsigned char *)palette->bodies.data)[0x345] = 0x91;
    auth_categories(palette, 1, "Original category \xce\xa9");
    unsigned char *editor = (unsigned char *)g_test_editor, *mode = editor + 0x22330;
    *(void **)mode = g_auth_mode; *(void **)(mode + 0x258) = g_auth_addvt;
    *(void **)(mode + 0x3f0) = g_auth_filtervt;
    *(int *)(mode + 0x1a8) = *(int *)(mode + 0x1ac) = tool;
    *(int *)(editor + 0x23618) = 2;
    *(void **)(editor + 0x21088) = g_auth_ui; *(void **)(g_auth_ui + 0x9c0) = g_auth_ui;
    g_auth_copies = g_auth_destroys = g_auth_add = g_auth_filter = g_auth_widget = 0;
    g_auth_copy_raise = g_auth_destroy_raise = g_auth_refresh_raise = 0;
}
static void test_authoring_snapshots(uint8_t *module, int *failed_out)
{
    int failed = *failed_out, admitted = 1;
    unsigned char old_decl[0x500], old_entity[0x70], new_decl[0x500], new_entity[0x70];
    unsigned char saved_rows[2 * 0x6f0];
    sh_palette_entries *batch = NULL;
    sh_palette_entry_source source = {"private/new", new_decl};
    char error[256];
    setup_entry(old_decl, old_entity, "old/entry"); setup_entry(new_decl, new_entity, "private/new");
    for (int tool = 3; tool <= 10; tool += tool == 3 ? 6 : 1) {
        auth_bind(module, old_decl, tool);
        test_native_palette before = *auth_palette();
        memcpy(saved_rows, before.bodies.data, sizeof(saved_rows));
        CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
        CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
        CHECK(g_auth_copies == 3 && !memcmp(auth_palette(), &before, sizeof(before)));
        CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
        test_native_palette *current = auth_palette();
        CHECK(current->bodies.data != before.bodies.data && current->bodies.count == 3);
        CHECK(*(void **)((unsigned char *)current->bodies.data + 2 * 0x6f0) == new_decl);
        CHECK(!memcmp(saved_rows, before.bodies.data, sizeof(saved_rows)));
        CHECK(current->groups.data != before.groups.data && current->labels.data != before.labels.data);
        CHECK(g_auth_add == (tool == 3) && g_auth_filter == (tool == 9));
        if (tool == 10) {
            CHECK(current->end == (unsigned char *)current->bodies.data + 0x6f0);
            CHECK(!strcmp(*(char **)((unsigned char *)current->labels.data + 0x10), "Original category \xce\xa9"));
        } else {
            CHECK(current->end == (unsigned char *)current->bodies.data + 3 * 0x6f0);
            CHECK(!strcmp(*(char **)((unsigned char *)current->labels.data + 0x10),
                tool == 3 ? "Authored category - retained exact bytes" : "Filtered category"));
        }
        admitted = 0;
        CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && batch);
        admitted = 1;
        CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
        CHECK(!memcmp(auth_palette(), &before, sizeof(before)) && g_auth_destroys == 1);
        CHECK(g_auth_widget == (tool != 10));
        CHECK(!memcmp(saved_rows, before.bodies.data, sizeof(saved_rows)));
        auth_destroy(auth_palette());
    }
    /* Native picker transitions can expand the searchable body prefix before
     * rebuilding category boundaries. Preserve that valid uncategorized tail. */
    auth_bind(module, old_decl, 10);
    auth_palette()->end = (unsigned char *)auth_palette()->bodies.data + 2 * 0x6f0;
    test_native_palette with_tail = *auth_palette();
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(auth_palette()->end == (unsigned char *)auth_palette()->bodies.data + 2 * 0x6f0);
    CHECK(((void **)auth_palette()->groups.data)[1] == (unsigned char *)auth_palette()->bodies.data + 0x6f0);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    CHECK(!memcmp(auth_palette(), &with_tail, sizeof(with_tail)));
    auth_destroy(auth_palette());

    /* Accepted snapshots remain owned until a complete native replacement,
     * even when the caller has an otherwise valid retirement boundary. */
    auth_bind(module, old_decl, 3);
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    sh_palette_entries_accept(batch);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && batch && !g_auth_destroys);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && batch && !g_auth_destroys);
    sh_palette_refresh_test_bind(module, auth_rebuild);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    CHECK(g_auth_destroys == 2 && !sh_palette_entries_test_count());

    auth_bind(module, old_decl, 3);
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    sh_palette_entries_accept(batch);
    void *recycled = auth_palette()->bodies.data;
    sh_palette_refresh_test_bind(module, auth_rebuild_same_address);
    CHECK(sh_palette_refresh_after_decl_registration() == 1);
    CHECK(auth_palette()->bodies.data == recycled && ((unsigned char *)recycled)[0x345] == 0x6b);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    CHECK(auth_palette()->bodies.data == recycled && ((unsigned char *)recycled)[0x345] == 0x6b && g_auth_destroys == 2);
    auth_destroy(auth_palette());

    auth_bind(module, old_decl, 10);
    test_native_palette before = *auth_palette();
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    g_auth_resize_fail = 1;
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0);
    CHECK(!memcmp(auth_palette(), &before, sizeof(before)) && !g_auth_copies);
    g_auth_resize_fail = 0;
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    auth_palette()->bodies.granularity = 3; /* An intervening native change refuses the exchange. */
    CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 0);
    auth_palette()->bodies.granularity = before.bodies.granularity;
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    CHECK(!memcmp(auth_palette(), &before, sizeof(before)));
    auth_destroy(auth_palette());

    auth_bind(module, old_decl, 10); before = *auth_palette();
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    auth_palette()->bodies.kind = 1; /* Unsupported interior/static array stays untouched. */
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && !g_auth_copies);
    auth_palette()->bodies.kind = before.bodies.kind;
    auth_palette()->end = (unsigned char *)before.end + 1;
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && !g_auth_copies);
    auth_palette()->end = before.end;
    ((void **)auth_palette()->groups.data)[1] = (unsigned char *)before.end + 0x6f0;
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && !g_auth_copies);
    ((void **)auth_palette()->groups.data)[1] = (unsigned char *)before.end - 1;
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && !g_auth_copies);
    ((void **)auth_palette()->groups.data)[1] = before.end;
    *(const char **)(old_entity + 8) = "private/new";
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 0 && !g_auth_copies);
    *(const char **)(old_entity + 8) = "old/entry";
    CHECK(!memcmp(auth_palette(), &before, sizeof(before)));
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    auth_destroy(auth_palette());

    /* Already represented declarations share native rows, without cloning. */
    auth_bind(module, new_decl, 3); before = *auth_palette();
    CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
    CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !g_auth_copies);
    CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == 1);
    CHECK(!memcmp(auth_palette(), &before, sizeof(before)) && !g_auth_add);
    CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == 1 && !batch);
    auth_destroy(auth_palette());

    /* A raised copy/refresh/destructor keeps explicit source ownership and
     * never retries a potentially partial native destructor. */
    for (int fault = 0; fault < 3; fault++) {
        auth_bind(module, old_decl, 3); before = *auth_palette();
        CHECK(sh_palette_entries_prepare(&source, 1, fake_admitted, &admitted, &batch, error, sizeof(error)) == 1);
        g_auth_copy_raise = fault == 0;
        CHECK(sh_palette_entries_prepare_authoring(batch, fake_admitted, &admitted, error, sizeof(error)) == (fault == 0 ? -1 : 1));
        if (fault) {
            g_auth_refresh_raise = fault == 1;
            CHECK(sh_palette_entries_publish(batch, fake_admitted, &admitted, error, sizeof(error)) == (fault == 1 ? -1 : 1));
        }
        g_auth_destroy_raise = fault == 2;
        CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == -1 && batch);
        int destroys = g_auth_destroys;
        CHECK(sh_palette_entries_end(&batch, fake_admitted, &admitted, error, sizeof(error)) == -1 && batch);
        CHECK(g_auth_destroys == destroys);
        g_auth_copy_raise = g_auth_refresh_raise = g_auth_destroy_raise = 0;
        sh_palette_entries_test_discard_quarantine(&batch);
        CHECK(!batch && !memcmp(auth_palette(), &before, sizeof(before)));
        auth_destroy(auth_palette());
    }
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
    test_retained_entries(module, &failed);
    test_authoring_snapshots(module, &failed);

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
