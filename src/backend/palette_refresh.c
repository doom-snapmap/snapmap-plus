/* Rebuild the native entity palette after each successful decl registration
 * pass. Registration alone does not update this catalog, which by-name
 * lookups and map validation consume. Stale entries can report "not found in palette".
 * The builder replaces its previous array.
 *
 * The decl server calls synchronously on the main thread. Missing signatures,
 * invalid objects, vtable checks or native exceptions enter terminal REFUSED;
 * later rearm attempts do not retry this service.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "engine_globals.h"   /* the editor singleton, located from the code site that computes it */
#include "host_image.h"
#include "iface_engine.h"
#include "palette_refresh.h"

/* PR_EDITOR_PALETTE_OFF is the supported structure offset. RVAs below are
 * pinned Vulkan audit references, not lookup gates. Resolve the builder by
 * clean signature and editor_singleton through engine_globals. The OpenGL
 * singleton RVA is 0x309B588.
 */
#define PR_EDITOR_SINGLETON_RVA 0x3056748u
#define PR_EDITOR_PALETTE_OFF   0x20660u
#define PR_PALETTE_VTABLE_RVA   0x20499A0u
#define PR_BUILDER_RVA          0x54AEE0u

enum {
    PR_STATE_IDLE = 0,
    PR_STATE_PENDING,
    PR_STATE_APPLIED,
    PR_STATE_REFUSED
};

typedef void (*palette_build_fn)(void *palette, void *progress);

static volatile LONG g_state = PR_STATE_IDLE;
static const uint8_t *g_module_base;
static palette_build_fn g_builder;

#ifdef SH_PALETTE_REFRESH_TESTING
static volatile LONG g_test_call_count;
#endif

static const sig_result *pr_result(const sig_result *results, size_t count,
                                   const char *name)
{
    size_t i;
    if (!results || !name) return NULL;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0)
            return &results[i];
    return NULL;
}

/* 1 = non-null, 0 = null, -1 = the object was not readable. */
static int pr_read_ptr(const void *address, void **out)
{
    if (!address || !out) return -1;
    __try {
        *out = *(void *const *)address;
        return *out ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

/* Check that the palette vtable lies in a readable, non-writable, non-
 * executable host section. This is a plausibility check; unreadable PE
 * headers refuse.
 */
static int pr_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        uintptr_t rva;
        unsigned int i;
        if (!module_base || !address || (uintptr_t)address < (uintptr_t)module_base) return 0;
        rva = (uintptr_t)address - (uintptr_t)module_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        sec = (const IMAGE_SECTION_HEADER *)IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            uintptr_t start = (uintptr_t)sec->VirtualAddress;
            uintptr_t span  = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (rva < start || rva >= start + span) continue;
            return (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0 &&
                   (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void pr_refuse(const char *reason)
{
    InterlockedExchange(&g_state, PR_STATE_REFUSED);
    backend_log(reason ? reason : "palette-refresh REFUSED");
}

int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base)
{
    const sig_result *builder;

    if (InterlockedCompareExchange(&g_state, PR_STATE_IDLE, PR_STATE_IDLE) != PR_STATE_IDLE ||
        g_builder != NULL)
        return 0;
    builder = pr_result(results, count, "SnapPaletteBuild");
    /* This call has a vtable/data-layout contract, so a hook-tolerant resolve
     * is not acceptable even though it is callable for ordinary leaf calls. */
    if (!builder || builder->status != SIG_OK || !builder->addr || !module_base) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild requires a clean SIG_OK resolve");
        return 0;
    }

    /* Require the resolved address and RVA to describe the same host module. */
    if (builder->addr != (uintptr_t)module_base + builder->rva) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild resolve is not self-consistent with the host image base");
        return 0;
    }

    g_module_base = module_base;
    g_builder = (palette_build_fn)builder->addr;
    backend_log("palette-refresh installed: waiting for complete native decl registration");
    return 1;
}

/* Claim the service for one rebuild. A pass may start from IDLE (the first
 * registration of the process) or from APPLIED (every later one). REFUSED and
 * PENDING are not claimable: the first is terminal, the second means a rebuild
 * is already in flight. */
static int pr_claim(void)
{
    if (InterlockedCompareExchange(&g_state, PR_STATE_PENDING, PR_STATE_IDLE) == PR_STATE_IDLE)
        return 1;
    return InterlockedCompareExchange(&g_state, PR_STATE_PENDING, PR_STATE_APPLIED) ==
           PR_STATE_APPLIED;
}

int sh_palette_refresh_after_decl_registration(void)
{
    const uint8_t *editor;
    void *palette_vtable = NULL;
    int vtable_status;

    if (!pr_claim())
        return 0;
    backend_log("palette-refresh ARMED: complete native decl registration succeeded");

    if (!g_module_base || !g_builder) {
        pr_refuse("palette-refresh REFUSED: clean builder/module dependency unavailable");
        return 0;
    }
    /* Use the runtime-resolved editor global; refuse if it is unavailable. */
    {
        uintptr_t expect = glb_resolve(g_module_base, "editor_singleton", NULL);
        if (!expect) {
            pr_refuse("palette-refresh REFUSED: the editor singleton could not be located on this build");
            return 0;
        }
        editor = sh_iface_engine_editor_base();
        if (!editor || (uintptr_t)editor != expect) {
            pr_refuse("palette-refresh REFUSED: editor singleton identity validation failed");
            return 0;
        }
    }

    /* Before calling the builder, require a readable palette object and a
     * non-null vtable in a read-only host section.
     */
    vtable_status = pr_read_ptr(editor + PR_EDITOR_PALETTE_OFF, &palette_vtable);
    if (vtable_status < 0 || vtable_status == 0 ||
        !pr_address_in_readonly_section(g_module_base, palette_vtable)) {
        pr_refuse("palette-refresh REFUSED: editor palette object/vtable validation failed");
        return 0;
    }

    /* The actual call consumes the state before invoking the engine. */
    if (InterlockedCompareExchange(&g_state, PR_STATE_APPLIED, PR_STATE_PENDING) != PR_STATE_PENDING)
        return 0;
    __try {
#ifdef SH_PALETTE_REFRESH_TESTING
        InterlockedIncrement(&g_test_call_count);
#endif
        g_builder((void *)(editor + PR_EDITOR_PALETTE_OFF), NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild raised an exception");
        return 0;
    }
    backend_log("palette-refresh FIRED: native SnapPaletteBuild rebuilt the entity palette");
    return 1;
}

#ifdef SH_PALETTE_REFRESH_TESTING
void sh_palette_refresh_test_reset(void)
{
    InterlockedExchange(&g_state, PR_STATE_IDLE);
    InterlockedExchange(&g_test_call_count, 0);
    g_module_base = NULL;
    g_builder = NULL;
}

void sh_palette_refresh_test_bind(const uint8_t *module_base, void *builder)
{
    g_module_base = module_base;
    g_builder = (palette_build_fn)builder;
}

int sh_palette_refresh_test_state(void)
{
    return (int)InterlockedCompareExchange(&g_state, 0, 0);
}

int sh_palette_refresh_test_call_count(void)
{
    return (int)InterlockedCompareExchange(&g_test_call_count, 0, 0);
}
#endif
