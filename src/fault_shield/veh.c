/* Classify engine faults and attempt bounded recovery. Visibility-leaf faults
 * return false for the bad node. Repaired CSR/render-node faults can unwind to
 * editor Think (Class A); remaining eligible faults redirect to Error(6) and request
 * editor exit (Class B). Known off-main threads are never given that redirect.
 * Engine C++ throws continue to their handlers after recovery-gate adjustments.
 * Fault logging is independent of recovery and may also record non-engine faults. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_layout.h"
#include "../backend/host_image.h"     /* report the build a crash came from + gate every pinned RVA */
#include "../backend/engine_globals.h" /* locate DOOM's data globals by signing the code that computes them */
#include "fault_record.h"
#include "crash_report.h"
#include "recovery.h"
#include "shield_sigs.h"
#include "veh.h"
#pragma comment(lib, "user32.lib")   /* ClipCursor/ShowCursor -- free the captured mouse on a Class-B fault */

extern uint8_t *g_doom_base;
extern size_t   g_doom_size;

/* Is the host the exact build every pinned RVA in this file was extracted from? Answered once at
 * install, so neither the classifier nor the fault path does string work. */
static int g_pinned_build = 0;

/* Derive [lo,hi) from resolved entries and measured spans. Use a literal start
 * only on the pinned build; an unresolved range cannot classify a fault. */
static int editor_frame_range(uintptr_t *lo, uintptr_t *hi)
{
    uintptr_t l = g_eng.editor_pump_rva;
    if (!l && g_pinned_build) l = RVA_EDITOR_FRAME_LO;
    if (!l) return 0;
    *lo = l; *hi = l + EDITOR_FRAME_SPAN;
    return 1;
}
static int resolver_range(uintptr_t *lo, uintptr_t *hi)
{
    uintptr_t l = g_eng.resolver_rva;
    if (!l && g_pinned_build) l = RVA_RESOLVER_LO;
    if (!l) return 0;
    *lo = l; *hi = l + RESOLVER_SPAN;
    return 1;
}

/* Resolve handler globals during installation to avoid image scans while
 * handling faults. Zero means unavailable; callers skip dependent operations. */
static uintptr_t g_errstate_at   = 0;   /* errState        -- Frame's catch requires its low byte == 0 */
static uintptr_t g_load_state_at = 0;   /* load_state      -- only the LOADING value 1 blocks recovery */
static uintptr_t g_suppr_a_at    = 0;   /* throw-gate suppressor A */
static uintptr_t g_main_tid_at   = 0;   /* the engine's own main-thread id */
static uintptr_t g_editor_at     = 0;   /* the inline idSnapEditorLocal object (RN-FAULT diagnostic only) */
static uintptr_t g_ti_fatal_rva  = 0;   /* idFatalException ThrowInfo, as an RVA */
static int       g_ti_fatal_ok   = 0;   /* ...and whether it resolved (0 is a legal RVA, so flag it) */

/* The visibility redirect requires the resolved leaf and its measured false tail. */
static uintptr_t g_visleaf_lo_rva    = 0;
static uintptr_t g_visleaf_hi_rva    = 0;
static uintptr_t g_visleaf_false_rva = 0;
static int       g_visleaf_ok        = 0;
/* Offsets within the leaf remain tied to the measured body layout. */
#define VIS_LEAF_FALSE_OFF  (RVA_VIS_LEAF_FALSE - RVA_VIS_LEAF_LO)   /* == 0x24, the XOR AL,AL; RET tail */

/* An unresolved leaf never matches a fault. */
static int in_vis_leaf(uintptr_t rva)
{
    return g_visleaf_ok && rva >= g_visleaf_lo_rva && rva < g_visleaf_hi_rva;
}

/* Locate everything the handler reads. Called from veh_install, before the handler can run. */
static void veh_resolve_globals(void)
{
    const uint8_t *base = g_doom_base;
    uintptr_t v;

    g_pinned_build = sh_host_is_pinned_rva_build();
    if (base == NULL) return;

    g_errstate_at   = glb_resolve(base, "error_state", NULL);
    g_load_state_at = glb_resolve(base, "load_state", NULL);
    g_suppr_a_at    = glb_resolve(base, "throw_suppressor_a", NULL);
    g_main_tid_at   = glb_resolve(base, "main_thread_id", NULL);
    g_editor_at     = glb_resolve(base, "editor_singleton", NULL);

    v = glb_resolve(base, "throwinfo_fatal", NULL);
    if (v) { g_ti_fatal_rva = v - (uintptr_t)base; g_ti_fatal_ok = 1; }

    v = glb_resolve(base, "vis_leaf_lo", NULL);
    if (v) {
        g_visleaf_lo_rva    = v - (uintptr_t)base;
        g_visleaf_hi_rva    = g_visleaf_lo_rva + VIS_LEAF_SPAN;   /* body end, same span as before */
        g_visleaf_false_rva = g_visleaf_lo_rva + VIS_LEAF_FALSE_OFF;  /* its own XOR AL,AL; RET tail */
        g_visleaf_ok        = 1;
    } else {
        shield_fault f = { "sig", -1,
            "visibility leaf UNRESOLVED -- the vis-leaf micro-recovery is disabled on this build "
            "(the shield will not set RIP from an address it cannot locate)", 0, 0 };
        shield_emit(&f);
    }
}

/* Resolve suppressor B through the dispatcher's CMP anchor. Its displacement
 * is followed by an immediate byte; engine_globals accounts for that tail.
 * Callers guard the write with SEH. */
static void write_suppressor_b(void)
{
    uintptr_t b;
    if (g_doom_base == NULL) return;
    b = glb_resolve(g_doom_base, "throw_suppressor_b", NULL);
    if (!b) return;                     /* unlocatable on this build -- skip, never guess */
    *(volatile int32_t *)b = 0;
}

#define SHIELD_MAX_REDIRECTS 8
static volatile LONG g_redirects = 0;    /* Class-B/fallback Error(6) redirects (runaway-guarded) */
static volatile LONG g_classa_seen = 0;  /* Class-A in-editor recoveries (log rate-limit only; NOT capped) */
static volatile LONG g_visleaf_seen = 0; /* vis-leaf micro-recoveries (log rate-limit only; NOT capped) */
static volatile LONG g_diag_seen = 0;    /* DIAGNOSTIC: count of AVs the VEH has logged */
static volatile LONG g_rn_seen = 0;      /* DIAGNOSTIC: render-node (vis-leaf) fault detail count */
static char g_why[200];   /* persists: Error(6) reads it as the fmt (rcx) after we return */
static char g_diag[260];
static char g_rndiag[220];

/* Cap detailed Class-B stack and crash-record capture independently of recovery.
 * The UI treats these records as nonterminal notices, not proof of survival. */
#define SHIELD_MAX_POPUP 3
static volatile LONG g_popup_seen = 0;
static char g_crashstk[512];

/* Log first-chance crash statuses from any module without changing disposition.
 * Recovery below only acts on its supported engine fault paths. */
#define SHIELD_MAX_FIRSTCHANCE 64
static volatile LONG g_firstchance_seen = 0;
static char g_fcdiag[320];

/* crash-class = a severity-ERROR status code (0xC.../0xE...), minus the benign first-chance noise + the DOOM
 * C++ throw (0xE06D7363) which LAYER 2 below handles + logs on its own. */
static int is_crash_class(DWORD c)
{
    if (c == 0x80000003u /*BREAKPOINT*/ || c == 0x80000004u /*SINGLE_STEP*/ ||
        c == 0x40010005u /*DBG_CONTROL_C*/ || c == 0x40010006u /*DBG_PRINTEXCEPTION_C*/ ||
        c == 0x4001000Au /*DBG_PRINTEXCEPTION_WIDE_C*/ || c == 0x406D1388u /*MS_VC_SET_THREAD_NAME*/ ||
        c == 0xE06D7363u /*C++ throw -- handled by LAYER 2*/)
        return 0;
    return (c >> 30) == 3;   /* STATUS_SEVERITY_ERROR */
}

static const char *exc_name(DWORD c)
{
    switch (c) {
        case 0xC0000005u: return "ACCESS_VIOLATION";
        case 0xC0000006u: return "IN_PAGE_ERROR";
        case 0xC000001Du: return "ILLEGAL_INSTRUCTION";
        case 0xC0000025u: return "NONCONTINUABLE";
        case 0xC0000094u: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000096u: return "PRIVILEGED_INSTRUCTION";
        case 0xC00000FDu: return "STACK_OVERFLOW";
        case 0xC0000374u: return "HEAP_CORRUPTION";
        case 0xC0000409u: return "STACK_BUFFER_OVERRUN/FASTFAIL";
        case 0xC000041Du: return "FATAL_USER_CALLBACK";
        default:          return "status";
    }
}

/* Resolve an address to "<module-basename>+0x<offset>". Best-effort; a failed lookup yields "?". Uses the
 * FROM_ADDRESS/UNCHANGED_REFCOUNT flags (no LoadLibrary side effect); called only on the rate-limited log path. */
static void module_at(void *addr, char *out, size_t cap, uintptr_t *off)
{
    HMODULE h = NULL;
    out[0] = '\0';
    if (off) *off = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &h) && h) {
        char path[MAX_PATH];
        const char *base = "?";
        if (GetModuleFileNameA(h, path, MAX_PATH)) {
            char *s = strrchr(path, '\\');
            base = s ? s + 1 : path;
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s", base);
        if (off) *off = (uintptr_t)addr - (uintptr_t)h;
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "?");
    }
}

static int rip_in_doom(void *rip)
{
    return g_doom_base &&
           (uint8_t *)rip >= g_doom_base &&
           (uint8_t *)rip <  g_doom_base + g_doom_size;
}

/* Uncommitted or inaccessible data is wild. Readable recycled heap storage
 * cannot be distinguished from live storage by this page-state check. */
static int is_wild(void *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) == 0) return 1;   /* unqueryable -> wild */
    if (mbi.State != MEM_COMMIT) return 1;                     /* free / reserved -> a read faults */
    /* PAGE_NOACCESS can be committed. Mask modifiers so a stack guard page is
     * not mistaken for an inaccessible page and redirected during stack growth. */
    if ((mbi.Protect & 0xFF) == PAGE_NOACCESS) return 1;
    return 0;
}

/* Unwind a context copy to [lo,hi), restoring nonvolatile registers and RSP.
 * Return 1 at the target frame, or 0 after maxframes without a match. Frameless
 * leaves pop their return address. UNW_FLAG_NHANDLER skips cleanup handlers, so
 * this recovery may leak frame temporaries and requires an intact stack. */
static int unwind_to_rva_range(CONTEXT *ctx, uintptr_t lo_rva, uintptr_t hi_rva, int maxframes)
{
    int i;
    for (i = 0; i < maxframes; i++) {
        if (rip_in_doom((void *)ctx->Rip)) {
            uintptr_t r = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
            if (r >= lo_rva && r < hi_rva) return 1;
        }
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)ctx->Rip, &imageBase, NULL);
            if (fn == NULL) {
                /* frameless leaf: the return address sits at [RSP], no saved nonvolatiles. */
                uintptr_t sp = (uintptr_t)ctx->Rsp;
                if (sp == 0 || is_wild((void *)sp)) return 0;
                ctx->Rip = *(uintptr_t *)sp;
                ctx->Rsp = sp + 8;
            } else {
                PVOID  handlerData = NULL;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)ctx->Rip, fn,
                                 ctx, &handlerData, &establisherFrame, NULL);
            }
        }
        if (ctx->Rip == 0) return 0;
    }
    return 0;
}

/* ---- Capture the FAULTING call stack as a compact "DOOM+0xRVA <- ..." string (for the crash log + popup).
 * Walks a COPY of the fault context with the OS unwinder (like unwind_to_rva_range); non-DOOM frames show as
 * module+off. SEH-guarded per frame so a wild frame just ends the walk -- never re-faults the VEH. */
static void capture_fault_stack(const CONTEXT *ctx_in, char *out, size_t cap, int maxframes)
{
    CONTEXT ctx = *ctx_in;
    size_t used = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < maxframes && used + 12 < cap; i++) {
        __try {
            const char *sep = used ? "\n    " : "";
            if (rip_in_doom((void *)ctx.Rip)) {
                uintptr_t r = (uintptr_t)ctx.Rip - (uintptr_t)g_doom_base;
                int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "%sDOOM+0x%llx", sep,
                                    (unsigned long long)r);
                if (n <= 0) break; used += (size_t)n;
            } else {
                char mod[64]; uintptr_t moff = 0;
                module_at((void *)ctx.Rip, mod, sizeof mod, &moff);
                int n = _snprintf_s(out + used, cap - used, _TRUNCATE, "%s%s+0x%llx", sep, mod,
                                    (unsigned long long)moff);
                if (n <= 0) break; used += (size_t)n;
            }
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)ctx.Rip, &imageBase, NULL);
            if (fn == NULL) {
                uintptr_t sp = (uintptr_t)ctx.Rsp;
                if (sp == 0 || is_wild((void *)sp)) break;
                ctx.Rip = *(uintptr_t *)sp; ctx.Rsp = sp + 8;
            } else {
                PVOID hd = NULL; DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)ctx.Rip, fn, &ctx, &hd, &ef, NULL);
            }
            if (ctx.Rip == 0) break;
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
}

/* Public wrapper (veh.h): the crash-record writer captures fatal-path stacks with the same walker. */
void shield_capture_stack(const CONTEXT *ctx, char *out, size_t cap, int maxframes)
{
    capture_fault_stack(ctx, out, cap, maxframes);
}

/* A committed, writable 4-byte slot? (guard before the shield pokes engine memory). */
static int writable_int(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD wr = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (VirtualQuery((void *)addr, &mbi, sizeof mbi) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((mbi.Protect & wr) == 0) return 0;
    return 1;
}

/* Repair the bad CSR column using the original resolver frame. RBP locates the
 * column array and loop index, RSI is the faulty value, and R12 is the source
 * entity index. Require a matching slot, known fault range, and writable storage.
 * Return 1 after replacing the column; otherwise leave recovery to the caller. */
static int try_revert_csr_entry(const CONTEXT *ctx)
{
    uintptr_t rva, rbp, holder, colArr, entry_addr;
    int64_t   loopidx;
    int       bad_value, valid_value;

    if (!rip_in_doom((void *)ctx->Rip)) return 0;
    rva = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
    /* resolver range rides the sig-resolved entry; the frameless vis-leaf rides glb_resolve. Either one
     * being unlocatable contributes an empty range rather than a guessed one. */
    {
        uintptr_t rlo = 0, rhi = 0;
        int in_resolver = resolver_range(&rlo, &rhi) && rva >= rlo && rva < rhi;
        if (!(in_resolver || in_vis_leaf(rva))) return 0;
    }

    rbp = (uintptr_t)ctx->Rbp;
    if (rbp == 0) return 0;
    if (is_wild((void *)(rbp - CSR_FRAME_COL_HOLDER))) return 0;
    if (is_wild((void *)(rbp - CSR_FRAME_LOOPIDX)))    return 0;

    holder = *(uintptr_t *)(rbp - CSR_FRAME_COL_HOLDER);     /* R15 = ET+0x5e0 */
    if (holder == 0 || is_wild((void *)holder)) return 0;
    colArr = *(uintptr_t *)holder;                           /* the column int array */
    if (colArr == 0 || is_wild((void *)colArr)) return 0;

    loopidx = *(int64_t *)(rbp - CSR_FRAME_LOOPIDX);
    if (loopidx < 0 || loopidx > 0x7FFFFFF) return 0;        /* sane column-index bound */
    entry_addr = colArr + (uintptr_t)loopidx * 4;
    if (!writable_int(entry_addr)) return 0;

    bad_value   = *(int *)entry_addr;
    valid_value = (int)ctx->R12;                             /* the source entity index (in range) */
    if (bad_value != (int)ctx->Rsi) return 0;                /* must match the faulting deref index */
    if (bad_value == valid_value)   return 0;                /* already safe */

    *(int *)entry_addr = valid_value;                        /* CLAMP -> next frame reads a valid index */
    return 1;
}

/* Clear a dangling render-node reference only at the known frameless leaf.
 * RCX holds the node, and RAX must match its +0x70 or +0x80 slot before clearing.
 * A null reference suppresses that wire on the next frame. Return 1 if changed. */
static int try_neutralize_rendernode(const CONTEXT *ctx)
{
    uintptr_t rva, node, rax;
    if (!rip_in_doom((void *)ctx->Rip)) return 0;
    rva = (uintptr_t)ctx->Rip - (uintptr_t)g_doom_base;
    if (!in_vis_leaf(rva)) return 0;
    node = (uintptr_t)ctx->Rcx;                       /* the render-node (RCX, preserved across the leaf) */
    rax  = (uintptr_t)ctx->Rax;                        /* the faulting connection-node ref value */
    if (node == 0 || is_wild((void *)node) || rax == 0) return 0;
    __try {
        if (*(uintptr_t *)(node + 0x70) == rax && writable_int(node + 0x70)) {
            *(uintptr_t *)(node + 0x70) = 0;
            return 1;
        }
        if (*(uintptr_t *)(node + 0x80) == rax && writable_int(node + 0x80)) {
            *(uintptr_t *)(node + 0x80) = 0;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

/* Supported non-AV hardware faults route to Class B without the wild-address
 * test. Stack overflow is excluded because recovery needs a usable stack.
 * C++ throws and debugger notifications use separate handling or pass through. */
static int is_other_hw_fault(DWORD code)
{
    switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return 1;
    default:
        return 0;
    }
}

/* Count of C++ throws the shield has gate-forced (log rate-limit only). */
static volatile LONG g_cxx_seen = 0;

/* Open Frame's recovery gate before the engine catch reads it: zero errState,
 * change load_state only when it equals the blocking value 1, and clear resolved
 * throw suppressors. Frame tests errState's low byte; this code clears the word.
 * This cannot recover an error caught by an inner engine handler first. */
static void force_recovery_gate(void)
{
    if (g_doom_base == NULL) return;
    __try {
        if (g_errstate_at)
            *(volatile int32_t *)g_errstate_at = 0;                  /* the (char) low-byte getter -> 0 */
        if (g_load_state_at && *(volatile int32_t *)g_load_state_at == 1)  /* only neutralize LOADING */
            *(volatile int32_t *)g_load_state_at = 0;
        if (g_suppr_a_at)
            *(volatile int32_t *)g_suppr_a_at = 0;
        write_suppressor_b();
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* Read the engine's thread ID: 1 = main, 0 = off-main, -1 = unknown/unreadable.
 * Bootstrap runs on another thread, so its ID cannot identify Frame's thread.
 * Only a definite off-main result blocks the Class-B redirect. */
static int shield_on_main_thread(void)
{
    uint32_t mt = 0;
    if (g_main_tid_at == 0) return -1;
    __try {
        mt = *(volatile uint32_t *)g_main_tid_at;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (mt == 0) return -1;
    return (mt == GetCurrentThreadId()) ? 1 : 0;
}

/* Count of off-main-thread faults the shield has declined to force through Error(6) (log rate-limit). */
#define SHIELD_MAX_OFFTHREAD 8
static volatile LONG g_offthread_seen = 0;
static char g_offthread[320];

static LONG CALLBACK shield_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    void *rip  = (void *)ep->ContextRecord->Rip;

    /* First-chance logging is independent of recovery and guarded against bad reads. */
    if (is_crash_class(code) && InterlockedIncrement(&g_firstchance_seen) <= SHIELD_MAX_FIRSTCHANCE) {
        __try {
            char mod[80]; uintptr_t moff = 0;
            module_at(rip, mod, sizeof mod, &moff);
            void *fa = (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                         ? (void *)er->ExceptionInformation[1] : NULL;
            uintptr_t drva = rip_in_doom(rip) ? ((uintptr_t)rip - (uintptr_t)g_doom_base) : 0;
            _snprintf_s(g_fcdiag, sizeof g_fcdiag, _TRUNCATE,
                "FIRST-CHANCE code=0x%08lx (%s) rip=%p mod=%s+0x%llx doom_rva=0x%llx fault=%p",
                (unsigned long)code, exc_name(code), rip, mod, (unsigned long long)moff,
                (unsigned long long)drva, fa);
            shield_fault fc = { "fc", (int)code, g_fcdiag, drva, (uintptr_t)fa };
            shield_emit(&fc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* Identify DOOM C++ throws by ExceptionInformation[3], the x64 throwing image
     * base; RIP points into RaiseException. Open the recovery gate before Frame
     * handles the throw, then continue exception search. */
    if (code == 0xE06D7363 && g_doom_base != NULL &&
        er->NumberParameters >= 4 && er->ExceptionInformation[3] == (ULONG_PTR)g_doom_base) {
        int es = -1, ls = -1;
        uintptr_t ti = 0;   /* ThrowInfo RVA: idException (recoverable) vs idFatalException (terminal) */
        __try {
            if (g_errstate_at)   es = *(volatile int32_t *)g_errstate_at;
            if (g_load_state_at) ls = *(volatile int32_t *)g_load_state_at;
            if (er->NumberParameters >= 3)
                ti = (uintptr_t)er->ExceptionInformation[2] - (uintptr_t)g_doom_base;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        force_recovery_gate();
        log_engine_error_text();     /* record the engine's verbatim error text -- e.g. a masked load-time FatalError */
        /* Record a fatal engine throw before its caught unwind exits the process.
         * That path does not reach an unhandled-exception filter. */
        /* Classify only a resolved ThrowInfo; zero must not act as an unknown match. */
        if (g_ti_fatal_ok && ti == g_ti_fatal_rva) {
            static volatile LONG s_fatal_throw_recorded = 0;
            if (InterlockedExchange(&s_fatal_throw_recorded, 1) == 0) {
                __try {
                    static char stk[1024], msg[HARVEST_MSG_MAX];
                    capture_fault_stack(ep->ContextRecord, stk, sizeof stk, 20);
                    msg[0] = '\0';
                    shield_last_engine_msg(msg, sizeof msg);
                    crash_report_file("engine_fatalerror", code, 0, 0, sh_host_image_name(), stk, msg, "");
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        if (InterlockedIncrement(&g_cxx_seen) <= 10) {
            _snprintf_s(g_diag, sizeof g_diag, _TRUNCATE,
                "DOOM C++ throw -> forced gate (errState=%d load_state=%d throwInfo_rva=0x%llx)",
                es, ls, (unsigned long long)ti);
            shield_fault df = { "diag", -1, g_diag, 0, 0 };
            shield_emit(&df);
            /* The throwing stack identifies the heap operation or caller reporting an
             * engine error. The walker guards each frame. */
            {
                char stk[512];
                capture_fault_stack(ep->ContextRecord, stk, sizeof stk, 20);
                if (stk[0]) { shield_fault sf = { "cxxstack", -1, stk, 0, 0 }; shield_emit(&sf); }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* Keep only supported hardware faults; AVs need further address classification. */
    if (code != EXCEPTION_ACCESS_VIOLATION && !is_other_hw_fault(code))
        return EXCEPTION_CONTINUE_SEARCH;

    int   is_av = (code == EXCEPTION_ACCESS_VIOLATION);
    /* fault-address + data-deref classification are AV-only (ExceptionInformation[1] is the AV address). */
    void *fault_addr = is_av ? (void *)er->ExceptionInformation[1] : NULL;
    int   data       = is_av && (fault_addr != rip);

    /* Recovery is limited to DOOM code. First-chance logging above may still
     * record other modules. */
    if (!rip_in_doom(rip))
        return EXCEPTION_CONTINUE_SEARCH;

    /* DIAGNOSTIC (first 40 faults): log every captured hardware fault + the redirect-decision inputs, so an
     * un-redirected crash tells us WHY (fault addr committed / wrong class / etc.). */
    if (InterlockedIncrement(&g_diag_seen) <= 40) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T q = fault_addr ? VirtualQuery(fault_addr, &mbi, sizeof mbi) : 0;
        uintptr_t drva = (uintptr_t)rip - (uintptr_t)g_doom_base;
        _snprintf_s(g_diag, sizeof g_diag, _TRUNCATE,
            "FAULT code=0x%lx rip=%p rip_rva=0x%llx fault=%p vq=%llu state=0x%lx prot=0x%lx",
            (unsigned long)code, rip, (unsigned long long)drva, fault_addr, (unsigned long long)q,
            (unsigned long)(q ? mbi.State : 0), (unsigned long)(q ? mbi.Protect : 0));
        shield_fault df = { "diag", (int)code, g_diag, drva, (uintptr_t)fault_addr };
        shield_emit(&df);
    }

    /* At the visibility leaf, record the node index and reference slots for diagnosis. */
    {
        uintptr_t rrva = (uintptr_t)rip - (uintptr_t)g_doom_base;
        if (is_av && in_vis_leaf(rrva) && InterlockedIncrement(&g_rn_seen) <= 12) {
            __try {
                uintptr_t node = (uintptr_t)ep->ContextRecord->Rcx;
                uintptr_t rax  = (uintptr_t)ep->ContextRecord->Rax;
                /* The entity index is a nicety in a log line; without the editor object we simply do
                 * not report one rather than dereferencing an address we could not locate. */
                uintptr_t base = g_editor_at ? *(uintptr_t *)(g_editor_at + 0x1d0) : 0;
                long idx = (base && !is_wild((void *)base)) ? (long)(((intptr_t)node - (intptr_t)base) / 0x180) : -1;
                uintptr_t p70 = 0, p80 = 0;
                if (!is_wild((void *)node)) { p70 = *(uintptr_t *)(node + 0x70); p80 = *(uintptr_t *)(node + 0x80); }
                _snprintf_s(g_rndiag, sizeof g_rndiag, _TRUNCATE,
                    "RN-FAULT node=%p idx=%ld base=%p +70=%p +80=%p rax=%p",
                    (void *)node, idx, (void *)base, (void *)p70, (void *)p80, (void *)rax);
                shield_fault rf = { "rn", (int)code, g_rndiag, rrva, rax };
                shield_emit(&rf);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    uintptr_t rva = (uintptr_t)rip - (uintptr_t)g_doom_base;

    /* AV-specific: a WILD data AV is the in-editor draw-fault class (Class-A). A committed-but-valid AV is
     * left to the engine's own SEH (redirecting one the engine would have handled is worse than its local
     * handling -- fault-shield-recovery.md scope). Non-AV hardware faults skip this gate -> Class-B. */
    if (is_av) {
        /* Visibility-leaf faults return false for this node by resuming at the leaf
         * tail. The leaf is frameless, so RSP still holds the caller return address.
         * This skips one predicate without unwinding active resolver/build frames.
         * Use only a resolved leaf with the measured false-return offset; otherwise
         * fall through to the ordinary recovery classification. */
        if (g_visleaf_ok && g_doom_base &&
            rva >= g_visleaf_lo_rva && rva < g_visleaf_false_rva) {
            /* Return through the leaf itself. Unwinding past active builder frames here
             * caused heap corruption on exit from Play; preserve those frames. */
            if (InterlockedIncrement(&g_visleaf_seen) <= 3) {
                _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                    "vis-leaf render-node fault @ 0x%llx (rip+0x%llx) -> predicate forced FALSE (skip node), resumed",
                    (unsigned long long)(uintptr_t)fault_addr, (unsigned long long)rva);
                shield_fault vf = { "visleaf", -1, g_why, rva, (uintptr_t)fault_addr };
                shield_emit(&vf);
            }
            ep->ContextRecord->Rip = (DWORD64)((uintptr_t)g_doom_base + g_visleaf_false_rva);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (!(data && is_wild(fault_addr)))
            return EXCEPTION_CONTINUE_SEARCH;

    /* Class A: after a known repair, unwind to editor Think and abort the draw.
     * An unrecognized fault retains its context for Class B. Recovered draws are not
     * charged against the redirect budget; only their logging is limited. */
    {
        CONTEXT unwound = *ep->ContextRecord;
        uintptr_t elo = 0, ehi = 0;
        if (editor_frame_range(&elo, &ehi) && unwind_to_rva_range(&unwound, elo, ehi, 32)) {
            /* Use the original resolver registers to repair the persistent bad connection. */
            int reverted   = try_revert_csr_entry(ep->ContextRecord);
            int rn_cleared = try_neutralize_rendernode(ep->ContextRecord);
            /* A frame on this stack does not establish a draw-only failure.
             * Collision/resource faults must reach the native error boundary;
             * skipping their callers left a second invalid return address. */
            if(reverted || rn_cleared) {
            if (InterlockedIncrement(&g_classa_seen) <= 5) {
                _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                    "in-editor draw fault @ 0x%llx (rip+0x%llx) -> aborted draw%s%s, resumed editor frame",
                    (unsigned long long)(uintptr_t)fault_addr, (unsigned long long)rva,
                    reverted ? " + reverted bad connection" : "",
                    rn_cleared ? " + cleared dangling render-node ref" : "");
                shield_fault fa = { "action", -1, g_why, rva, (uintptr_t)fault_addr };
                shield_emit(&fa);
            }
            /* Class-A recovery stays silent to avoid repeated error toasts during
             * transient render rebuilds. Diagnostics above are rate-limited. */

            *ep->ContextRecord = unwound;
            return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    }   /* end if (is_av) -- a wild AV the editor-unwind didn't claim falls through to Class-B */

    /* Error(6) relies on Frame's catch on the engine main thread. On a known worker
     * thread, record the fault and continue normal exception search instead. This
     * does not recover or prove containment; a surrounding handler may catch it.
     * Unknown thread identity preserves the existing Class-B fallback behavior. */
    if (shield_on_main_thread() == 0) {
        if (InterlockedIncrement(&g_offthread_seen) <= SHIELD_MAX_OFFTHREAD) {
            __try {
                char mod[80]; uintptr_t moff = 0;
                module_at(rip, mod, sizeof mod, &moff);
                _snprintf_s(g_offthread, sizeof g_offthread, _TRUNCATE,
                    "OFF-MAIN fault code=0x%08lx (%s) tid=%lu rip=%p rip_rva=0x%llx mod=%s+0x%llx fault=%p "
                    "-> NOT redirected to Error(6) (no idCommonLocal::Frame catch on this thread; forcing "
                    "the throw here would unwind off the thread unhandled) -> CONTINUE_SEARCH",
                    (unsigned long)code, exc_name(code), GetCurrentThreadId(), rip,
                    (unsigned long long)rva, mod, (unsigned long long)moff, fault_addr);
                shield_fault of = { "offthread", (int)code, g_offthread, rva, (uintptr_t)fault_addr };
                shield_emit(&of);
                /* the faulting call stack + a crash record, so an off-main death is fully triageable --
                 * the same evidence the Class-B path produces, minus the redirect. */
                capture_fault_stack(ep->ContextRecord, g_crashstk, sizeof g_crashstk, 14);
                if (g_crashstk[0]) {
                    shield_fault sk = { "stack", (int)code, g_crashstk, rva, (uintptr_t)fault_addr };
                    shield_emit(&sk);
                }
                crash_report_file("offthread", code, rva, (uintptr_t)fault_addr, sh_host_image_name(),
                                  g_crashstk, "", "");
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* ---- Class B (Layer 1): a wild AV the editor-unwind did NOT claim, OR a non-AV hardware fault in DOOM
     * (illegal/priv instruction, int/float divide, in-page, ...). Redirect into the engine's own recoverable
     * Error(6) + the proven editor-exit -> My-Maps browser. Runaway-guarded; CAN surface DOOM's recoverable-
     * error drop-to-menu -- survivable, vastly better than the hard crash these faults would otherwise be. */
    if (InterlockedIncrement(&g_redirects) > SHIELD_MAX_REDIRECTS) {
        /* runaway guard -> let the OS take it. This is the shield GIVING UP -- the process is very
         * likely about to die, so write a fatal crash record first (one-shot; the branch re-enters on
         * every subsequent fault). LOG-ONLY: the disposition below is unchanged. */
        static volatile LONG s_runaway_recorded = 0;
        if (InterlockedExchange(&s_runaway_recorded, 1) == 0) {
            __try {
                capture_fault_stack(ep->ContextRecord, g_crashstk, sizeof g_crashstk, 14);
                crash_report_file("fatal", code, rva, (uintptr_t)fault_addr, sh_host_image_name(),
                                  g_crashstk, "", "");
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
        "shield: caught fault 0x%lx at rip+0x%llx -> recovered via Error(6)",
        (unsigned long)code, (unsigned long long)rva);
    shield_fault f = { "load", -1, g_why, rva, (uintptr_t)fault_addr };
    shield_emit(&f);

    /* Capture stack and a nonterminal record, capped separately from redirects.
     * Raw hardware faults do not populate engine error text; reading that buffer
     * here would attach an unrelated earlier message. */
    if (InterlockedIncrement(&g_popup_seen) <= SHIELD_MAX_POPUP) {
        __try {
            capture_fault_stack(ep->ContextRecord, g_crashstk, sizeof g_crashstk, 14);
            shield_fault sk = { "stack", (int)code, g_crashstk, rva, (uintptr_t)fault_addr };
            shield_emit(&sk);   /* the full call stack -> shield_faults.log */
            crash_report_file("classB", code, rva, (uintptr_t)fault_addr, sh_host_image_name(),
                              g_crashstk, "", "");
            /* FREE THE MOUSE: DOOM clips the cursor to its window + hides it (captured input); un-clip +
             * force it visible (ShowCursor is a refcount -- bounded loop) so the user can actually reach
             * the Snapmap+ window where the crash dialog appears. */
            ClipCursor(NULL);
            { int cc = 0; while (ShowCursor(TRUE) < 0 && ++cc < 32) {} }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* Request normal editor teardown so an incomplete render world is not reused. */
    recovery_arm();

    /* Error(6) will populate engine error text after this redirect. Request a toast
     * using that text if recovery later has an editor screen to show it on. */
    notice_request_msg();

    /* Clear both resolved suppressors before Error(6). A failed write is logged;
     * the redirect still proceeds because the gate may already be open. */
    __try {
        if (g_suppr_a_at) *(volatile int32_t *)g_suppr_a_at = 0;
        write_suppressor_b();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shield_fault sf = { "sig", -1, "throw-gate suppressor write faulted", 0, 0 };
        shield_emit(&sf);
    }

    /* Simulate call Error(g_why): align RSP and push the fault RIP as the unwindable
     * return address. Require a resolved wrapper or the exact pinned-build fallback. */
    {
        uintptr_t error6 = g_eng.error6;
        if (!error6 && g_pinned_build)
            error6 = (uintptr_t)(g_doom_base + RVA_ERROR6);
        if (!error6) {
            shield_fault nf = { "sig", (int)code,
                "Class-B declined: idCommon::Error(6) is unresolved and this is not the pinned "
                "extraction build -- resuming into a pinned address would be a wild call", rva,
                (uintptr_t)fault_addr };
            shield_emit(&nf);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        uintptr_t sp = (ep->ContextRecord->Rsp & ~(uintptr_t)0xF) - 8;
        *(uintptr_t *)sp = (uintptr_t)rip;
        ep->ContextRecord->Rsp = sp;
        ep->ContextRecord->Rcx = (uintptr_t)g_why;                   /* Error(fmt) in rcx */
        ep->ContextRecord->Rip = error6;                             /* resume INTO Error(6) */
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

int veh_install(void)
{
    /* Resolve handler globals before the VEH can run. */
    veh_resolve_globals();
    return AddVectoredExceptionHandler(1 /* first-in-chain */, shield_veh) != NULL;
}
