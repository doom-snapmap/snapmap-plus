/* Sanitize editor wire-render node references before the native resolver
 * walks them. The legacy palette_guard name remains; this is not a palette
 * name-sort hook.
 *
 * The pinned Vulkan resolver at RVA 0x5e0ad0 receives a vector handle as
 * argument 2 (View+0x60, editor+0x1d0). Its records are 0x180 bytes;
 * output/input references at +0x70/+0x80 lead to the predicate status byte at
 * node+0x30. Reloads or uninitialized slots can leave unreadable references.
 * Clear those references without reordering or freeing records. Readable
 * pointers remain unchanged, including freed storage that is still mapped.
 *
 * This installer uses a build-specific RVA and a 20-byte prologue of four
 * argument home stores before the pushes. Re-derive the entry, instruction
 * boundary and layout before porting it.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "palette_guard.h"
#include "hook.h"            /* install_inline_hook */
#include "backend_log.h"

#define RVA_WIRE_RESOLVER      0x5e0ad0u   /* FUN_1405e0ad0(OP, arrHandle, out, i, flag): the wire-render walk */
#define WIRE_RESOLVER_STOLEN   20u         /* Four argument home stores form the 20-byte stolen prologue, before pushes. */

/* render-node vector handle (the resolver's 2nd arg = View+0x60 = editor+0x1d0). */
#define RN_BASE_OFF            0x00        /* base ptr (first 0x180 record) */
#define RN_SIZE_OFF            0x08        /* logical element count (int) */
#define RN_CAP_OFF             0x0c        /* Capacity: resolver high-water can exceed
                                            * size; the remaining slots may be
                                            * uninitialized.
                                            */
#define RN_STRIDE             0x180u       /* one per-entity render-node record */
#define RN_COUNT_MAX          0x4000       /* editor entity cap ~0x3ffe; a larger size = a bad read -> skip */

/* the two node references a render-node record holds (each a ptr to a vtable'd node object). */
#define RN_OUTNODE_OFF        0x70         /* output-node reference (the crash field) */
#define RN_INNODE_OFF         0x80         /* input-node reference */
#define NODE_STATUS_OFF       0x30         /* the byte the predicate FUN_140d32a30 reads: `cmp byte [node+0x30]` */
#define NODE_MIN_PTR          0x10000u     /* below this = a small integer, never a real heap object */

typedef void (*wire_resolver_fn)(void *op, void *arr_handle, void *out, int i, char flag);
static wire_resolver_fn g_orig_resolver = NULL;
static volatile LONG    g_reset_total   = 0;

/* Require the full byte range to be committed and readable. VirtualQuery
 * catches spans crossing inaccessible or guard pages.
 */
static int mem_range_readable(const void *addr, size_t nbytes)
{
    const uint8_t *p   = (const uint8_t *)addr;
    const uint8_t *end = p + (nbytes ? nbytes : 1);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
        if (mbi.State != MEM_COMMIT) return 0;                           /* freed = MEM_FREE/MEM_RESERVE */
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        const uint8_t *region_end = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= p) return 0;                                  /* no forward progress -> bail */
        p = region_end;
    }
    return 1;
}

/* NULL means no node. Reject small integers and unreadable ranges through
 * node+0x30. Readability does not prove object lifetime; freed but still
 * mapped storage passes.
 */
static int node_ref_invalid(const void *p)
{
    if (p == NULL) return 0;                                             /* no node -> fine */
    if ((uintptr_t)p < NODE_MIN_PTR) return 1;                          /* small integer in a pointer slot */
    return mem_range_readable(p, NODE_STATUS_OFF + 1) ? 0 : 1;          /* range not committed -> dangling */
}

/* Clear an invalid reference under SEH. Return 1 if the field changed. */
static int sanitize_node_ref(uint8_t *field)
{
    void *p;
    __try { p = *(void *const volatile *)field; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }                   /* can't even read the slot -> leave it */
    if (!node_ref_invalid(p)) return 0;
    __try { *(void *volatile *)field = NULL; }                          /* -> "no node"; the old ref is never freed */
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

/* Walk the render-node array and null every invalid output/input node reference before the resolver reads them. */
static void sh_rendernode_sanitize(void *arr_handle)
{
    void *base;
    int   size, cap;
    __try {
        base = *(void *const volatile *)((const uint8_t *)arr_handle + RN_BASE_OFF);
        size = *(const volatile int *)((const uint8_t *)arr_handle + RN_SIZE_OFF);
        cap  = *(const volatile int *)((const uint8_t *)arr_handle + RN_CAP_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                                                          /* can't read the handle -- defer to the engine */
    }
    /* Inspect the full bounded capacity because the resolver high-water can
     * exceed logical size. Invalid capacity falls back to size.
     */
    int n = (cap >= size && cap <= RN_COUNT_MAX) ? cap : size;
    if (base == NULL || n <= 0 || n > RN_COUNT_MAX) return;

    int reset = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *rec = (uint8_t *)base + (size_t)i * RN_STRIDE;
        reset += sanitize_node_ref(rec + RN_OUTNODE_OFF);
        reset += sanitize_node_ref(rec + RN_INNODE_OFF);
    }
    if (reset) {
        /* Log only repairs, with a bounded initial sample. */
        if (InterlockedAdd(&g_reset_total, reset) <= 64) {
            char m[128];
            _snprintf_s(m, sizeof m, _TRUNCATE,
                "rendernode-guard: walked %d records (size=%d cap=%d), nulled %d invalid node ref(s)",
                n, size, cap, reset);
            backend_log(m);
        }
    }
}

static void sh_wire_resolver_detour(void *op, void *arr_handle, void *out, int i, char flag)
{
    sh_rendernode_sanitize(arr_handle);                  /* clean the render-node array BEFORE the walk reads it */
    g_orig_resolver(op, arr_handle, out, i, flag);
}

int sh_palette_guard_install(const uint8_t *module_base)
{
    if (module_base == NULL) return 0;
    if (g_orig_resolver != NULL) return 1;               /* one-shot */
    void *target = (void *)(module_base + RVA_WIRE_RESOLVER);
    void *tramp  = install_inline_hook(target, (void *)sh_wire_resolver_detour, WIRE_RESOLVER_STOLEN);
    if (tramp == NULL) {
        backend_log("rendernode-guard: install FAIL (install_inline_hook returned NULL -- re-derive 0x5e0ad0)");
        return 0;
    }
    g_orig_resolver = (wire_resolver_fn)tramp;
    backend_log("rendernode-guard: armed -- wire-render stale-node guard on the connection resolver (0x5e0ad0)");
    return 1;
}
