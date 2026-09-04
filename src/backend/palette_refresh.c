/* palette_refresh.c -- native palette rebuild after each new decl registration.
 *
 * The engine's entity palette is a catalog DERIVED from the live decl list.
 * Registering a new decl after the catalog has been built does not put it in
 * the catalog by itself. The native SnapPaletteBuild routine (RVA 0x54AEE0)
 * rebuilds the catalog in place from the decl list; this service calls that
 * routine synchronously from the dynamic decl server's main-thread command
 * after a registration pass succeeds.
 *
 * ONCE PER REGISTRATION PASS, NOT ONCE PER PROCESS, and that distinction is
 * the whole point of this file. The catalog is what every by-name consumer
 * searches, including idSnapMap::RepairAndMigrate's entity validator
 * (RVA 0x5F27C0), which binary-searches it and reports
 *
 *     Invalid Entity %d:%s not found in palette
 *
 * then fails the whole map, which the shell reports to the player as a damaged
 * save. So while this call was latched to fire exactly once, a package
 * installed mid-session registered its decls correctly and still could not be
 * used: any map NAMING one of its types was refused until DOOM was restarted,
 * because the catalog searched at load time was the one built before the
 * package existed. Rebuilding per pass is what removes the restart.
 *
 * The native builder is written to be re-run: it tears down and frees the
 * previous array before repopulating it, which is dead code on a first call.
 *
 * This is deliberately not a rawmap hook, an editor injection, or a refresh
 * retry loop. A missing/unsupported build, invalid editor object, vtable
 * mismatch, or native exception is terminal for this process -- a refusal is
 * an integrity verdict on the engine objects being called into, so re-arming
 * never gives a refused process another attempt.
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

/* PR_EDITOR_PALETTE_OFF is a STRUCT OFFSET and carries across both shipped DOOM builds unchanged.
 *
 * The three RVAs are what these locations occupy on the PINNED VULKAN BUILD (DOOMx64vk.exe),
 * recorded for audit and for re-deriving a signature that stops matching. The two code/vtable ones
 * NO LONGER GATE: DOOM 2016 ships two executables built from one source tree, the game relaunches
 * itself into the other when r_renderAPI changes, and every function RVA shifts between them with
 * no uniform delta. The builder's identity is proven by a clean unique masked-signature match --
 * which is stronger evidence than RVA equality, since two builds can share an RVA by coincidence
 * but a signature cannot match the wrong function -- and the palette vtable is read out of the
 * live editor object, so it needs no pinned address at all, only a plausibility check.
 *
 * PR_EDITOR_SINGLETON_RVA is a raw DATA RVA. It is no longer used to locate anything: the
 * singleton is resolved as "editor_singleton" through engine_globals.h, which signs the code site
 * that computes the address. The constant is the pinned Vulkan value (0x309B588 on the OpenGL
 * build), kept for audit and re-derivation. */
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

/* Non-zero when `address` lies in a section of the host image the loader mapped READ-ONLY (READ
 * set, WRITE and EXECUTE clear) -- .rdata on both shipped builds, where the engine's vtables live.
 * This is the plausibility test for the palette vtable pointer read out of the live editor object:
 * that pointer is already correct for whichever build we are in, so there is nothing to compare it
 * against, only somewhere it has to land. SEH-guarded; a malformed or unreadable image refuses. */
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

    /* The clean SIG_OK resolve above IS the identity proof; all that is left to confirm is that the
     * resolver's own address and RVA agree about this image base, so the pointer we cache and the
     * base we later derive the editor from cannot be describing two different modules. */
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
    /* The editor singleton is located at runtime from the code site that computes its address, so
     * this validates against a value derived on THIS build rather than one baked for another. If
     * the resolver cannot place it, refuse -- a wrong pointer is worse than none, because the
     * caller cannot tell the difference. */
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

    /* Validate the palette object before invoking the engine-owned rebuild. The vtable pointer is
     * read out of the live editor object, so it is already right for this build; what is checked is
     * that it is present and plausibly a vtable -- a non-null read landing in a read-only section of
     * the host image -- rather than that it equals one build's recorded address. */
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
