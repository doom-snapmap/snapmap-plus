/* mapload_guards.c -- two targeted guards for defects in the GAME's own code on the map-load /
 * entity-spawn path. Clean-room from our own reverse-engineering of the pinned DOOM build; every RVA and
 * struct offset used here is declared, with its re-derivation recipe, in engine_layout.h.
 *
 * Both guards follow the render-node guard's shape (palette_guard.c): detour the engine function, repair
 * the specific broken precondition BEFORE the engine reads it, leave a healthy path bit-for-bit alone,
 * never free anything, and stay SEH-guarded end to end so the guard itself can never fault.
 *
 * WHY THE PRE-CHECK NEVER WRAPS THE CALL TO THE ORIGINAL. Each detour calls its precheck helper (which
 * is internally SEH-guarded and returns before anything else happens) and only THEN calls the engine
 * original, outside every __try scope. That is deliberate: __except(EXCEPTION_EXECUTE_HANDLER) also
 * catches C++ exceptions (0xE06D7363), so wrapping the original would swallow the engine's own
 * idException and break the Frame catch the shield's Class-B recovery depends on.
 *
 * THREADING / COST. Both sites run on DOOM's main thread during map load and entity spawn. The happy
 * path costs one SEH-guarded header read (free -- x64 SEH is table-based, no runtime cost until an
 * exception actually occurs) plus, only when there is a non-empty buffer to vet, one VirtualQuery walk.
 * Neither guard adds per-frame work.
 *
 * A guard that fires writes a rate-limited line to shield_faults.log (class "load").
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "mapload_guards.h"
#include "engine_layout.h"   /* RVA_EVLINK / RVA_INTERACTABLE_SPAWN + the offsets, with their recipes */
#include "../backend/host_image.h"
#include "../backend/signatures.h" /* EventLink / InteractableSpawn are signature-resolved */
#include "fault_record.h"    /* shield_emit -> shield_faults.log */
#include "hook.h"            /* install_inline_hook */

/* ---- shared safe-probe helpers ---------------------------------------------------------------------
 * VirtualQuery-based, matching the render-node guard: a freed page reports its true state, and a buffer
 * that straddles a still-mapped page into an unmapped one is caught (a single-byte SEH probe would miss
 * exactly that). Used ONLY for spans we cannot simply read field-by-field. */
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

/* ---- verify BEFORE write -----------------------------------------------------------------------------
 * Both RVAs here are build-locked literals, and each STOLEN count is only correct for the exact prologue
 * it was derived from. On a different DOOM build the same RVA decodes to something else, so blindly
 * writing a 14-byte jump there would land mid-instruction and corrupt the game. So: compare the target's
 * first STOLEN bytes against the prologue recorded from the pinned build and REFUSE to install on any
 * mismatch. This mirrors the patch layer's refuse-on-mismatch rule and the signature layer's policy of
 * declining an already-hooked prologue -- if another tool got there first the bytes differ, and we leave
 * it alone rather than stacking a second detour on top.
 *
 * RE-DERIVE per build together with the matching STOLEN count in engine_layout.h: read the first STOLEN
 * bytes at the function entry and confirm they decode to whole, position-independent instructions. */
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

/* Rate-limited diagnostic -> shield_faults.log. `counter` is the guard's own fire count (already
 * incremented). The first GUARD_LOG_MAX fires log in full; the next one logs a single suppression
 * notice and the rest are silent, so a guard firing repeatedly cannot flood the log. */
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

/* ==== GUARD 1 -- the event/trigger linker's unvalidated list walk (map-load use-after-free) ==========
 *
 * See engine_layout.h for the decompile this is built on. In short: the linker walks list->data for
 * list->num elements having validated neither. When the list is stale the walk runs off into freed
 * memory and faults at RVA 0x9C24B0.
 *
 * WHAT THIS GUARD CHECKS, per list, before the engine touches it:
 *   - the owner's list slot is readable;
 *   - a NULL slot is left alone (the engine constructs a fresh empty list itself -- already correct);
 *   - the {data,num,cap} header is readable (read field-by-field under SEH, so no straddle blind spot);
 *   - num >= 0, cap >= 0, num <= cap, cap <= EVLIST_COUNT_MAX;
 *   - if data != NULL, the WHOLE span the engine may touch -- [data, data + cap*8) -- is readable. cap,
 *     not num, because the append writes at data[num] and num <= cap.
 *
 * WHAT IT DOES WHEN A CHECK FAILS. It makes the list EMPTY, which is exactly what the brief calls for:
 * the walk becomes a no-op instead of running off into freed memory. Two tiers:
 *   TIER A -- the list HANDLE itself is unreadable: NULL the owner's slot. The engine's own next act is
 *     `if (list == 0) { list = new(0x18); data = 0; num = 0; cap = 0; gran = 0x50000; }`, so the engine
 *     builds itself a fresh, fully-valid list. This is a full repair, not a mask.
 *   TIER B -- the handle is readable but its contents are not trustworthy: reset the header in place to
 *     {data = NULL, num = 0, cap = 0}. That is BYTE-IDENTICAL to the state the engine's own constructor
 *     leaves a fresh list in (it stores 0 to +0x00 and an 8-byte 0 across +0x08/+0x0C), so the append
 *     that follows takes the ordinary grow-an-empty-list path. Only the header is touched; the
 *     granularity word at +0x10 is left as the engine set it.
 * The old buffer is NEVER freed -- it is already gone, or it is not ours.
 *
 * COST OF LOSING THE LIST. Some event/trigger wiring for that one entity pair is dropped on a corrupt
 * load. That is enormously better than the crash, and the alternative -- letting the walk continue -- is
 * a guaranteed access violation.
 *
 * RESIDUAL LIMIT (stated, not hidden): a buffer that has been freed but whose pages are still committed
 * to the process heap reads without faulting and therefore passes. This guard addresses the OBSERVED
 * failure -- the reported faulting address IS the data pointer, i.e. the read itself faulted, so the
 * page was not committed. A committed-but-recycled buffer is a different (silent) defect and needs the
 * free site fixed at the source. */

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

    /* Read the header field-by-field under SEH. Every field the engine uses is read here, so an
     * unreadable byte anywhere in the header faults now rather than slipping past a one-byte probe. */
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

    /* The buffer the engine will read (and may append to) must be fully backed. Exactly cap*8 bytes: the
     * walk reads [0,num) and the append writes [num] only when num < cap, so both stay inside [0,cap).
     * Deliberately NOT one element more -- demanding a byte past the allocation would reject a healthy
     * list that happens to end at a region boundary. */
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

/* Locate a guard target. Both functions are in the shipped signature database, so they are found by
 * their BYTES on whichever DOOM executable is running -- the pinned RVAs below are only true of the
 * Vulkan image, and using one on the OpenGL build made these guards refuse to install at all. The
 * prologue comparison at each call site stays: the signature says which function this is, the
 * prologue says the stolen-byte window is still the one we measured. */
static void *guard_target(const uint8_t *module_base, const char *sig_name, uint32_t pinned_rva)
{
    sig_result results[SIG_RESULTS_MAX];
    size_t db = sig_db_count();
    uintptr_t a;
    if (db > SIG_RESULTS_MAX) db = SIG_RESULTS_MAX;
    /* sig_resolve_all fills results BY DATABASE INDEX and returns how many RESOLVED. Searching only
     * the returned count truncates the tail of the array, so a single earlier signature failing (our
     * own detours are already installed by the time the guards arm) silently hides the last entries --
     * which is exactly how InteractableSpawn went missing while its neighbour EventLink resolved. */
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

/* ==== GUARD 2 -- idInteractable::Spawn's unchecked subsystem pointer ================================
 *
 * See engine_layout.h for the decompile. The tag-binding loop dereferences *(this+0x3DB0) twice without
 * ever null-checking it; when an interactable spawns before its subsystem is up that pointer is NULL and
 * `MOV RCX,[RAX+0xe90]` at 0x123293F faults at address 0xE90.
 *
 * !! CORRECTION TO THE OBVIOUS READING, and the reason this guard is shaped the way it is. It is
 * tempting to conclude from
 *       0x123293F  MOV RCX,[RAX+0xe90]
 *       0x1232946  TEST RCX,RCX
 *       0x1232949  JZ  0x1232954
 *       0x1232954  MOV RCX,RDX            ; RDX = 0 on the first iteration
 *       0x123295E  CALL 0x1414B7920
 * that the engine has a defined "the chain is absent" path and that a guard should simply reproduce it
 * (make the +0xE90 load yield 0). IT DOES NOT, and doing that would only move the crash. The JZ arm
 * passes 0 into FUN_1414B7920, whose own entry is
 *       0x14B7946  MOV R14,RCX
 *       0x14B794B  CMP dword ptr [RCX+0x8],EDI     <-- no null check: reads address 0x8 and faults
 * so the "absent" branch is a second, immediate access violation. (It is also only 0 on the first
 * iteration; RDX is volatile and holds callee garbage on later ones.) The result-null-check at 0x1232946
 * is unexercised defensive boilerplate, not a supported path: the subsystem pointer is REQUIRED, and the
 * same unchecked base deref appears again in FUN_1414BCEA0 at 0x14BCEAF. Verified DIRECT by disassembly
 * of all three sites.
 *
 * WHAT THE ENGINE ACTUALLY DOES WHEN THERE IS NOTHING TO BIND -- and this one IS real, exercised, and
 * reached on every tagless interactable -- is the tag-count gate that gets you into the loop at all:
 *       0x12328F1  CMP dword ptr [R14+0x4c98],EDX
 *       0x12328F8  JLE <past the entire loop>
 * That path never touches +0x3DB0. So THAT is the existing behaviour this guard steers into.
 *
 * WHAT THIS GUARD CHECKS, at Spawn entry:
 *   - tag count <= 0  -> return immediately. The engine never reaches the deref; zero intervention.
 *   - subsystem pointer non-NULL and [base, base+0xE98) readable -> return. Healthy; zero intervention.
 *   - otherwise the loop WILL fault -> set the tag count to 0.
 * Within Spawn the tag count is read in exactly three places and all three are this loop's machinery
 * (the list pre-size, the loop entry gate, and the loop's own continue test) -- DIRECT, decompile of the
 * whole 3032-byte function -- so zeroing it suppresses the loop and touches nothing else.
 *
 * WHY THE COUNT IS NOT RESTORED AFTERWARDS. With the count at 0 the engine sets the tag list's element
 * count to 0 as well, leaving a self-consistent empty list (count 0, and the list is not even allocated,
 * because the pre-size call is skipped). Restoring the original count would leave the object claiming N
 * tags over a list that holds none and whose buffer may still be NULL -- so any later code that iterates
 * by the count would read uninitialized entries or dereference NULL. That would be a NEW crash of our
 * own making. Leaving the count at 0 cannot: "this interactable has no tags" is a state the engine
 * builds for itself and handles everywhere.
 *
 * COST WHEN IT FIRES. That one interactable spawns without its tags -- it will not be interactive. Every
 * other part of its Spawn still runs, because we let the original execute in full. The subsystem was not
 * up, so those tags could not have been bound by any means; the alternative is the access violation. */

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
        /* fault_addr = 0xE90: the address the access violation WOULD have reported, so a shield_faults.log
         * reader can tie the averted fire to the crash signature it prevents. */
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
