/* Repair invalid event-link lists and missing interactable subsystems before
 * map-load/spawn code uses them. Healthy paths remain unchanged; repairs never
 * free engine memory. A repair can discard wiring or an interactable's tags.
 * Keep the original calls outside __try: catching their C++ exceptions would
 * swallow idException and prevent the engine's Frame catch from recovering.
 * Work runs at load/spawn time, with bounded logging and guarded memory access. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "mapload_guards.h"
#include "engine_layout.h"   /* RVA_EVLINK / RVA_INTERACTABLE_SPAWN + the offsets, with their recipes */
#include "../backend/host_image.h"
#include "../backend/signatures.h" /* EventLink / InteractableSpawn are signature-resolved */
#include "fault_record.h"    /* shield_emit -> shield_faults.log */
#include "hook.h"            /* install_inline_hook */

/* Probe every page in a span; a one-byte read would miss an unmapped tail. */
static int mem_range_readable(const void *addr, size_t nbytes)
{
    const uint8_t *p   = (const uint8_t *)addr;
    const uint8_t *end = p + (nbytes ? nbytes : 1);
    if (end < p) return 0;                                              /* length overflow -> reject */
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
        if (mbi.State != MEM_COMMIT) return 0;                          /* freed = MEM_FREE/MEM_RESERVE */
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        const uint8_t *region_end = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return 0;                                  /* no forward progress -> bail */
        p = region_end;
    }
    return 1;
}

static int mem_range_writable(const void *addr, size_t nbytes)
{
    const DWORD wr = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const uint8_t *p   = (const uint8_t *)addr;
    const uint8_t *end = p + (nbytes ? nbytes : 1);
    if (end < p) return 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & PAGE_GUARD) return 0;
        if ((mbi.Protect & wr) == 0) return 0;
        const uint8_t *region_end = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return 0;
        p = region_end;
    }
    return 1;
}

/* Verify the complete stolen prologue before detouring a resolved target.
 * Mismatches, including existing hooks, must refuse installation. Recheck whole
 * instruction boundaries and position independence when updating these bytes. */
static const uint8_t k_evlink_prologue[EVLINK_STOLEN] = {
    0x40, 0x57,                                      /* PUSH RDI                        */
    0x48, 0x83, 0xEC, 0x30,                          /* SUB  RSP,0x30                   */
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF  /* MOV qword [RSP+0x20],-2    */
};
static const uint8_t k_ia_spawn_prologue[INTERACTABLE_STOLEN] = {
    0x48, 0x8B, 0xC4,                                /* MOV  RAX,RSP                    */
    0x55,                                            /* PUSH RBP                        */
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  /* PUSH R12/R13/R14/R15            */
    0x48, 0x8D, 0xA8, 0x98, 0xFE, 0xFF, 0xFF         /* LEA  RBP,[RAX-0x168]            */
};

static int prologue_matches(const uint8_t *target, const uint8_t *expect, size_t n)
{
    size_t i;
    __try {
        for (i = 0; i < n; i++) {
            if (target[i] != expect[i]) return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                                    /* unreadable target -> definitely do not patch it */
    }
    return 1;
}

/* Log the first GUARD_LOG_MAX repairs, one suppression notice, then stay silent. */
#define GUARD_LOG_MAX 32

static void guard_log(LONG counter, const char *msg, uintptr_t rva, uintptr_t addr)
{
    shield_fault f;
    f.cls          = "load";
    f.severity     = -1;                      /* not an engine level -- a pre-emptive repair, not a fault */
    f.message      = msg;
    f.faulting_rva = rva;
    f.fault_addr   = addr;
    if (counter <= GUARD_LOG_MAX) {
        shield_emit(&f);
    } else if (counter == GUARD_LOG_MAX + 1) {
        f.message = "guard fired more than 32 times this session -- suppressing further lines";
        shield_emit(&f);
    }
}

/* Event-link guard: validate the list header and its entire capacity span.
 * Drop an unreadable handle so the engine reconstructs it, or reset a damaged
 * header to {NULL,0,0}. Preserve granularity and never free the old buffer.
 * Repairs lose that list's wiring. Page checks cannot detect freed storage that
 * remains committed or has been recycled; those defects require a lifetime fix. */

typedef void (*evlink_fn)(void *a, void *b);
static evlink_fn     g_orig_evlink   = NULL;
static volatile LONG g_evlink_fires  = 0;

/* Vet one list and repair it if needed. `owner_slot` = address of the pointer field holding the list.
 * Returns: 0 = healthy / untouched, 1 = TIER A (slot nulled), 2 = TIER B (header reset). */
static int evlink_vet_slot(void *owner_slot)
{
    uint8_t *list;
    void    *data;
    int      num, cap;

    __try {
        list = *(uint8_t *const volatile *)owner_slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                                    /* cannot even read the slot -> defer to the engine */
    }
    if (list == NULL) return 0;                      /* engine allocates a fresh one: already the empty case */

    /* Read every header field under SEH so an unreadable tail cannot pass a probe. */
    __try {
        data = *(void *const volatile *)(list + EVLIST_DATA_OFF);
        num  = *(const volatile int *)(list + EVLIST_NUM_OFF);
        cap  = *(const volatile int *)(list + EVLIST_CAP_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* TIER A: the handle is gone. Drop it so the engine constructs a fresh list. */
        if (!mem_range_writable(owner_slot, sizeof(void *))) return 0;
        __try {
            *(void *volatile *)owner_slot = NULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return 1;
    }

    /* Fast-out: the engine's own fresh-list state. Nothing to walk, nothing to probe. */
    if (data == NULL && num == 0 && cap == 0) return 0;

    /* Header consistency -- the checks the engine skips entirely. */
    if (num < 0 || cap < 0 || num > cap || cap > EVLIST_COUNT_MAX) goto reset;
    if (num > 0 && data == NULL) goto reset;

    /* Probe cap*8 bytes: reads use [0,num), and an append uses [num] only if num<cap.
     * Probing one element past capacity would reject allocations at page boundaries. */
    if (data != NULL && !mem_range_readable(data, (size_t)cap * 8u)) goto reset;

    return 0;                                        /* healthy -- the engine runs untouched */

reset:
    /* TIER B: reset in place to the engine's own empty-list state. */
    if (!mem_range_writable(list, EVLIST_HDR_SIZE)) return 0;
    __try {
        *(void *volatile *)(list + EVLIST_DATA_OFF) = NULL;
        *(volatile int *)(list + EVLIST_NUM_OFF)    = 0;
        *(volatile int *)(list + EVLIST_CAP_OFF)    = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 2;
}

static void evlink_precheck(void *a, void *b)
{
    int ra = 0, rb = 0;

    if (a != NULL) ra = evlink_vet_slot((uint8_t *)a + EVLINK_SLOT_A);
    if (b != NULL) rb = evlink_vet_slot((uint8_t *)b + EVLINK_SLOT_B);
    if (ra == 0 && rb == 0) return;

    {
        char m[224];
        LONG n = InterlockedIncrement(&g_evlink_fires);
        _snprintf_s(m, sizeof m, _TRUNCATE,
            "evwire-guard: stale event-link list neutralized before the walk (a=%p tier=%d, b=%p tier=%d; "
            "tier1=handle dropped, engine rebuilds it; tier2=header reset to empty). Some event wiring for "
            "this pair is dropped; nothing was freed.", a, ra, b, rb);
        guard_log(n, m, RVA_EVLINK, 0);
    }
}

static void sh_evlink_detour(void *a, void *b)
{
    evlink_precheck(a, b);      /* internally SEH-guarded; returns before the original runs */
    g_orig_evlink(a, b);        /* OUTSIDE every __try -- must not swallow the engine's C++ throws */
}

/* Resolve by signature, with pinned-build fallback. Callers also verify the
 * stolen prologue because an identified function may already be hooked. */
static void *guard_target(const uint8_t *module_base, const char *sig_name, uint32_t pinned_rva)
{
    sig_result results[SIG_RESULTS_MAX];
    size_t db = sig_db_count();
    uintptr_t a;
    if (db > SIG_RESULTS_MAX) db = SIG_RESULTS_MAX;
    /* Results use database indices, not the number of successful resolutions. */
    (void)sig_resolve_all(module_base, results, SIG_RESULTS_MAX);
    a = sig_addr_by_name(results, db, sig_name);
    if (a) return (void *)a;
    if (sh_host_is_pinned_rva_build()) return (void *)(module_base + pinned_rva);
    return NULL;   /* unknown build: refuse rather than detour a guessed address */
}
int sh_evwire_guard_install(const uint8_t *module_base)
{
    void *target, *tramp;

    if (module_base == NULL) return 0;
    if (g_orig_evlink != NULL) return 1;                     /* one-shot */

    target = guard_target(module_base, "EventLink", RVA_EVLINK);
    if (target == NULL) {
        shield_fault f = { "load", -1,
            "evwire-guard: EventLink is unresolved on this build -- NOT installed", RVA_EVLINK, 0 };
        shield_emit(&f);
        return 0;
    }
    if (!prologue_matches((const uint8_t *)target, k_evlink_prologue, EVLINK_STOLEN)) {
        shield_fault f = { "load", -1,
            "evwire-guard: prologue MISMATCH at the resolved EventLink -- NOT installed (the function is "
            "already hooked, or its stolen-byte window changed). Re-derive the STOLEN count.",
            RVA_EVLINK, 0 };
        shield_emit(&f);
        return 0;
    }
    tramp  = install_inline_hook(target, (void *)sh_evlink_detour, EVLINK_STOLEN);
    if (tramp == NULL) {
        shield_fault f = { "load", -1,
            "evwire-guard: install FAIL (install_inline_hook returned NULL -- re-derive 0x9C2370)",
            RVA_EVLINK, 0 };
        shield_emit(&f);
        return 0;
    }
    g_orig_evlink = (evlink_fn)tramp;
    {
        shield_fault f = { "load", -1,
            "evwire-guard: armed -- stale event-link list guard on the event/trigger linker (0x9C2370)",
            RVA_EVLINK, 0 };
        shield_emit(&f);
    }
    return 1;
}

/* Interactable guard: skip tag binding if its subsystem is missing or unreadable.
 * The loop's apparent null-chain branch still dereferences null in its callee;
 * only the tag-count gate avoids both unchecked subsystem reads.
 * Leave the count zero after Spawn: restoring it would advertise tags that the
 * engine never populated. The rest of Spawn runs, but the object loses its tags. */

typedef void (*ia_spawn_fn)(void *self);
static ia_spawn_fn   g_orig_ia_spawn = NULL;
static volatile LONG g_ia_fires      = 0;

static void ia_spawn_precheck(void *self)
{
    void *base;
    int   tagcount;

    if (self == NULL) return;

    __try {
        tagcount = *(const volatile int *)((uint8_t *)self + IA_TAGCOUNT_OFF);
        base     = *(void *const volatile *)((uint8_t *)self + IA_SUBSYS_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                                       /* cannot read `this` -> defer to the engine */
    }

    if (tagcount <= 0) return;                        /* engine's own gate skips the loop; nothing to do */
    if (base != NULL && mem_range_readable(base, IA_SUBSYS_PROBE)) return;  /* healthy -- engine untouched */

    if (!mem_range_writable((uint8_t *)self + IA_TAGCOUNT_OFF, sizeof(int))) return;
    __try {
        *(volatile int *)((uint8_t *)self + IA_TAGCOUNT_OFF) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    {
        char m[256];
        LONG n = InterlockedIncrement(&g_ia_fires);
        _snprintf_s(m, sizeof m, _TRUNCATE,
            "interactable-guard: idInteractable::Spawn entered with an absent tag subsystem "
            "(this=%p *(this+0x3db0)=%p tags=%d) -- tag count zeroed so the engine takes its own "
            "no-tags path; this interactable spawns without tags instead of faulting at 0x123293f.",
            self, base, tagcount);
        /* Use the prevented null+0xE90 address to match reports of this crash. */
        guard_log(n, m, RVA_INTERACTABLE_SPAWN, IA_SUBSYS_PROBE - 8u);
    }
}

static void sh_ia_spawn_detour(void *self)
{
    ia_spawn_precheck(self);    /* internally SEH-guarded; returns before the original runs */
    g_orig_ia_spawn(self);      /* OUTSIDE every __try -- must not swallow the engine's C++ throws */
}

int sh_interactable_guard_install(const uint8_t *module_base)
{
    void *target, *tramp;

    if (module_base == NULL) return 0;
    if (g_orig_ia_spawn != NULL) return 1;                   /* one-shot */

    target = guard_target(module_base, "InteractableSpawn", RVA_INTERACTABLE_SPAWN);
    if (target == NULL) {
        shield_fault f = { "load", -1,
            "interactable-guard: InteractableSpawn is unresolved on this build -- NOT installed",
            RVA_INTERACTABLE_SPAWN, 0 };
        shield_emit(&f);
        return 0;
    }
    if (!prologue_matches((const uint8_t *)target, k_ia_spawn_prologue, INTERACTABLE_STOLEN)) {
        shield_fault f = { "load", -1,
            "interactable-guard: prologue MISMATCH at 0x1232830 -- NOT installed (different DOOM build, or "
            "the function is already hooked). Re-derive the RVA + STOLEN count for this build.",
            RVA_INTERACTABLE_SPAWN, 0 };
        shield_emit(&f);
        return 0;
    }
    tramp  = install_inline_hook(target, (void *)sh_ia_spawn_detour, INTERACTABLE_STOLEN);
    if (tramp == NULL) {
        shield_fault f = { "load", -1,
            "interactable-guard: install FAIL (install_inline_hook returned NULL -- re-derive 0x1232830)",
            RVA_INTERACTABLE_SPAWN, 0 };
        shield_emit(&f);
        return 0;
    }
    g_orig_ia_spawn = (ia_spawn_fn)tramp;
    {
        shield_fault f = { "load", -1,
            "interactable-guard: armed -- absent-subsystem guard on idInteractable::Spawn (0x1232830)",
            RVA_INTERACTABLE_SPAWN, 0 };
        shield_emit(&f);
    }
    return 1;
}
