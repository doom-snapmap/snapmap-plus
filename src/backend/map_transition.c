#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include "map_transition.h"
#include "engine_globals.h"
#include "hook.h"
#include "backend_log.h"

enum { TRANS_ALLOC, TRANS_UNLOAD, TRANS_PURGE, TRANS_FINALIZE, TRANS_HOOKS };
enum { TRANS_IDLE, TRANS_UNLOADING, TRANS_ACTIVATING, TRANS_LOADING, TRANS_FINISHING };
#define TRANS_CANCEL_EXCEPTION ((DWORD)0xe0534d50)
typedef unsigned char (*transition_alloc_fn)(void *, void *, const void *, void *);
typedef void (*transition_unload_fn)(void *, unsigned char, unsigned char);
typedef void (*transition_purge_fn)(unsigned);
typedef int (*transition_compare_fn)(const char *, const char *);
typedef void (*transition_finalize_fn)(void *, const void *, void *);
typedef void (*transition_cancel_fn)(void *, unsigned char);
static void *g_trans_sites[TRANS_HOOKS], *g_trans_original[TRANS_HOOKS];
static void *g_trans_common;
static void **g_trans_pending_resource;
static int *g_trans_has_resource, *g_trans_force_full;
static transition_compare_fn g_trans_compare;
static transition_cancel_fn g_trans_cancel;
static DWORD *g_trans_thread;
static sh_map_transition_callbacks g_trans_callbacks;
static volatile LONG g_trans_ready;
static __declspec(thread) struct {
    int phase, purged, skip_unload, admitted, allocated, cancel;
    const void *parameters;
    void *extra;
} g_trans;

static const uint8_t *transition_site(const sig_result *results, size_t count,
    const uint8_t *base, const char *name)
{
    const uint8_t *site = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (site || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        site = (const uint8_t *)results[i].addr;
    }
    return site;
}
static void *transition_relative(const uint8_t *site, const char *opcode, size_t length)
{
    int32_t displacement;
    if (memcmp(site, opcode, length)) return NULL;
    memcpy(&displacement, site + length, 4);
    return (void *)(site + length + 4 + displacement);
}
int sh_map_transition_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *allocation, *unload, *purge, *finalize, *cancel;
    void *common, *pending, *has_resource, *force_full, *compare;
    uintptr_t thread;
    if (!results || !base || g_trans_ready) return 0;
    allocation = transition_site(results, count, base, "AllocateGameResources");
    unload = transition_site(results, count, base, "UnloadGameResources");
    finalize = transition_site(results, count, base, "FinalizeMapChange");
    cancel = transition_site(results, count, base, "CancelMapChange");
    if (!allocation || !unload || !finalize || !cancel) return 0;
    __try {
        /* This exact prefix only clears one resource pointer and computes the
         * unload arguments. No loading local has been constructed at +0xae. */
        pending = transition_relative(allocation + 0x5b, "\x4c\x89\x3d", 3);
        has_resource = transition_relative(allocation + 0x62, "\x44\x39\x3d", 3);
        force_full = transition_relative(allocation + 0x84, "\x44\x39\x3d", 3);
        compare = transition_relative(allocation + 0x7b, "\xe8", 1);
        common = transition_relative(allocation + 0xa2, "\x48\x8d\x0d", 3);
        if (!pending || !has_resource || !force_full || !compare || !common ||
            memcmp(allocation + 0x70, "\x48\x8b\x91\x70\x2c\x00\x00\x49\x8b\x48\x30", 11) ||
            memcmp(allocation + 0x95, "\x0f\xb6\x96\xf8\x70\x01\x00\xc0\xea\x05\x80\xe2\x01", 13) ||
            transition_relative(allocation + 0xa9, "\xe8", 1) != unload) return 0;
        purge = transition_relative(unload + 0x1a4, "\xe8", 1);
        if (!purge || transition_relative(unload + 0x1b3, "\xe8", 1) != purge ||
            memcmp(purge, "\x40\x57\x48\x83\xec\x40\x48\xc7\x44\x24\x20\xfe\xff\xff\xff", 15)) return 0;
        /* Verified on both images: native bailout uses common +0x28; the
         * resource allocation is the +0x1e7 virtual call with common+0x1298. */
        if (memcmp(finalize, "\x40\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\xb8\x70\x74\x00\x00", 17) ||
            memcmp(finalize + 0x1b9, "\x48\x8b\x06\x33\xd2\x48\x8b\xce\xff\x50\x28", 11) ||
            memcmp(finalize + 0x1c9, "\xc6\x86\x76\xf5\x00\x00\x01", 7) ||
            memcmp(finalize + 0x1e0, "\x48\x8d\x96\x98\x12\x00\x00\xff\x50\x58\x84\xc0", 12)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    thread = glb_resolve(base, "main_thread_id", NULL);
    if (!thread) return 0;
    g_trans_sites[TRANS_ALLOC] = (void *)allocation;
    g_trans_sites[TRANS_UNLOAD] = (void *)unload;
    g_trans_sites[TRANS_PURGE] = (void *)purge;
    g_trans_sites[TRANS_FINALIZE] = (void *)finalize;
    g_trans_cancel = (transition_cancel_fn)cancel;
    g_trans_common = common; g_trans_pending_resource = pending;
    g_trans_has_resource = has_resource; g_trans_force_full = force_full;
    g_trans_compare = (transition_compare_fn)compare; g_trans_thread = (DWORD *)thread;
    return 1;
}
int sh_map_transition_at_boundary(void)
{
    return g_trans_ready && g_trans_thread && *g_trans_thread == GetCurrentThreadId() &&
        g_trans.phase == TRANS_ACTIVATING && g_trans.purged;
}
int sh_map_transition_ready(void)
{
    return g_trans_ready && g_trans_thread && *g_trans_thread == GetCurrentThreadId();
}
static void transition_purge_from(unsigned mask, const void *caller)
{
    ((transition_purge_fn)g_trans_original[TRANS_PURGE])(mask);
    if (g_trans.phase == TRANS_UNLOADING && mask == 1 &&
        caller == (const uint8_t *)g_trans_sites[TRANS_UNLOAD] + 0x1a9) g_trans.purged = 1;
}
static void transition_purge(unsigned mask)
{
    transition_purge_from(mask, _ReturnAddress());
}
static void transition_unload_from(void *common, unsigned char force, unsigned char full, const void *caller)
{
    /* Only consume the exact call in the allocation whose unload we already
     * performed. Other callbacks must never inherit this one-shot bypass. */
    if (g_trans.phase == TRANS_LOADING && g_trans.skip_unload && common == g_trans_common &&
        caller == (const uint8_t *)g_trans_sites[TRANS_ALLOC] + 0xae) {
        g_trans.skip_unload = 0; return;
    }
    ((transition_unload_fn)g_trans_original[TRANS_UNLOAD])(common, force, full);
}
static void transition_unload(void *common, unsigned char force, unsigned char full)
{
    transition_unload_from(common, force, full, _ReturnAddress());
}
static __declspec(noreturn) void transition_abort(void)
{
    ULONG_PTR token = (ULONG_PTR)&g_trans;
    g_trans.cancel = 1;
    RaiseException(TRANS_CANCEL_EXCEPTION, EXCEPTION_NONCONTINUABLE, 1, &token);
    __assume(0);
}
static int transition_cancel_filter(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    return g_trans.cancel && record->ExceptionCode == TRANS_CANCEL_EXCEPTION &&
        record->NumberParameters == 1 && record->ExceptionInformation[0] == (ULONG_PTR)&g_trans
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
static unsigned char transition_allocate_from(void *manager, void *out,
    const void *parameters, void *extra, const void *caller)
{
    int pending, loaded = 0;
    unsigned char force, full;
    /* Only the exact allocation inside our native finalization scope can
     * consume a retained request. Shell and worker allocations stay native. */
    if (!g_trans_ready || !g_trans_thread || *g_trans_thread != GetCurrentThreadId() ||
        !g_trans.parameters || g_trans.phase == TRANS_FINISHING)
        return ((transition_alloc_fn)g_trans_original[TRANS_ALLOC])(manager, out, parameters, extra);
    if (g_trans.phase != TRANS_IDLE) transition_abort();
    if (parameters != g_trans.parameters || extra != g_trans.extra ||
        out != (uint8_t *)g_trans_common + 0x1298 ||
        caller != (const uint8_t *)g_trans_sites[TRANS_FINALIZE] + 0x1ea)
        return ((transition_alloc_fn)g_trans_original[TRANS_ALLOC])(manager, out, parameters, extra);
    pending = g_trans_callbacks.pending(manager, parameters);
    if (!pending) return ((transition_alloc_fn)g_trans_original[TRANS_ALLOC])(manager, out, parameters, extra);
    g_trans.admitted = 1;
    if (pending < 0) transition_abort();
    /* No allocation-frame local has been constructed at this point. Perform
     * the already requested unload once, then publish before new consumers. */
    *g_trans_pending_resource = NULL;
    full = *g_trans_has_resource &&
        (g_trans_compare(*(const char *const *)((const uint8_t *)parameters + 0x30),
            *(const char *const *)((const uint8_t *)manager + 0x2c70)) || *g_trans_force_full);
    force = (*(const uint8_t *)((const uint8_t *)parameters + 0x170f8) >> 5) & 1;
    g_trans.phase = TRANS_UNLOADING; g_trans.purged = 0;
    ((transition_unload_fn)g_trans_original[TRANS_UNLOAD])(g_trans_common, force, full);
    if (g_trans.purged) {
        int activated = 0;
        g_trans.phase = TRANS_ACTIVATING;
        __try { activated = g_trans_callbacks.activate(); }
        __except (GetExceptionCode() == TRANS_CANCEL_EXCEPTION
            ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
            backend_log("MPKG: map handoff activation raised an exception");
        }
        if (activated) {
            g_trans.phase = TRANS_LOADING; g_trans.skip_unload = 1;
            loaded = ((transition_alloc_fn)g_trans_original[TRANS_ALLOC])(manager, out, parameters, extra) != 0;
            if (g_trans.skip_unload) {
                backend_log("MPKG: native map allocation did not consume its verified unload call"); loaded = 0;
            }
        }
    } else backend_log("MPKG: native map unload did not complete its resource purge; allocation refused");
    /* Never return false to FinalizeMapChange: it turns that into a fatal
     * error. Unwind only our token to the outer owner and use native bailout. */
    if (!loaded) transition_abort();
    g_trans.allocated = 1;
    return 1;
}
static unsigned char transition_allocate(void *manager, void *out, const void *parameters, void *extra)
{
    return transition_allocate_from(manager, out, parameters, extra, _ReturnAddress());
}
static void transition_finalize(void *common, const void *parameters, void *extra)
{
    int completed = 0;
    if (!g_trans_ready || !g_trans_thread || *g_trans_thread != GetCurrentThreadId() ||
        common != g_trans_common || g_trans.phase == TRANS_FINISHING) {
        ((transition_finalize_fn)g_trans_original[TRANS_FINALIZE])(common, parameters, extra); return;
    }
    if (g_trans.parameters) transition_abort();
    g_trans.parameters = parameters; g_trans.extra = extra;
    __try {
        __try {
            ((transition_finalize_fn)g_trans_original[TRANS_FINALIZE])(common, parameters, extra);
            if (g_trans.allocated) {
                int committed = 0;
                __try { committed = g_trans_callbacks.commit(); }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    backend_log("MPKG: completing the map installation raised an exception");
                }
                if (!committed) transition_abort();
            }
            completed = g_trans.allocated;
        } __except (transition_cancel_filter(GetExceptionInformation())) {
            g_trans.phase = TRANS_FINISHING; g_trans.skip_unload = 0;
            /* The same bailout called by FinalizeMapChange for a rejected
             * map. Its session/menu cleanup runs after all loading frames
             * unwind. It is never an activation step on the successful path. */
            g_trans_cancel(common, 0);
            *(uint16_t *)((uint8_t *)common + 0xf576) = 0;
        }
    } __finally {
        g_trans.phase = TRANS_FINISHING; g_trans.skip_unload = 0;
        if (g_trans.admitted) {
            __try { g_trans_callbacks.finished(completed); }
            __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MPKG: map handoff ownership cleanup raised an exception"); }
        }
        memset(&g_trans, 0, sizeof(g_trans));
    }
}
int sh_map_transition_install(const sig_result *results, size_t count, const uint8_t *base,
    const sh_map_transition_callbacks *callbacks)
{
    static const size_t stolen[TRANS_HOOKS] = {21, 20, 15, 17};
    void *detours[TRANS_HOOKS] = {(void *)transition_allocate, (void *)transition_unload,
        (void *)transition_purge, (void *)transition_finalize};
    if (g_trans_ready) return 1;
    if (!callbacks || !callbacks->pending || !callbacks->activate || !callbacks->commit || !callbacks->finished) return 0;
    for (size_t i = 0; i < TRANS_HOOKS; i++) if (g_trans_original[i]) {
        if (!hook_unpatch(g_trans_original[i])) return 0;
        g_trans_original[i] = NULL;
    }
    if (!sh_map_transition_bind(results, count, base)) return 0;
    __try {
        if ((transition_cancel_fn)(*(void ***)g_trans_common)[5] != g_trans_cancel) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_trans_callbacks = *callbacks;
    for (size_t i = 0; i < TRANS_HOOKS; i++) {
        g_trans_original[i] = hook_prepare(g_trans_sites[i], detours[i], stolen[i]);
        if (!g_trans_original[i]) goto failed;
    }
    for (size_t i = 0; i < TRANS_HOOKS; i++) if (hook_commit(g_trans_original[i]) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_trans_ready, 1);
    return 1;
failed:
    for (size_t i = TRANS_HOOKS; i > 0; i--) if (g_trans_original[i - 1] && hook_unpatch(g_trans_original[i - 1]))
        g_trans_original[i - 1] = NULL;
    backend_log("MPKG: native map handoff hooks unavailable; surviving gates remain pass-through");
    return 0;
}
