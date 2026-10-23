#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "discrete_default.h"
#include "execution_context.h"
#include "host_image.h"
#include "patch.h"
#include "signatures.h"
#include "backend_log.h"

/* Pinned per executable (R-321; m130/ghidra/bytes-*.json). The builder stores the box's flag word
 * (the box is at rbp+90h, its flag word at +60h) and calls the finish:
 *   mov dword [rbp+0F0h],2; lea rcx,[rbp+90h]; call finish
 * The finish uploads static buffers when bit 1 is set and keeps CPU data when bit 0 is set:
 *   push rbx; sub rsp,20h; mov rbx,rcx; call; test byte [rbx+60h],2; jz; mov dl,1; mov rcx,rbx; call;
 *   test byte [rbx+60h],1; jnz
 * Both executables carry these bytes; only the builder's call displacement differs. */
static const unsigned char g_builder[] = {0xc7, 0x85, 0xf0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x8d, 0x90, 0x00, 0x00, 0x00, 0xe8};
static const unsigned char g_finish_head[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9,
    0xe8, 0xb2, 0x06, 0x00, 0x00, 0xf6, 0x43, 0x60, 0x02, 0x74, 0x0a, 0xb2, 0x01, 0x48, 0x8b, 0xcb,
    0xe8, 0xd2, 0x09, 0x00, 0x00, 0xf6, 0x43, 0x60, 0x01, 0x75, 0x0d};
/* RVAs of the builder bytes and of the finish, by renderer (0 OpenGL, 1 Vulkan). */
static const uint32_t g_pins[2][2] = {{0x1449762, 0x14bc5f0}, {0x14559f2, 0x14c90d0}};
#define DD_CALL (sizeof(g_builder) - 1)
#define DD_FLAGS 0x60
/* The finish's upload routine (VK 0x1414c9ac0, GL 0x1414bcfe0; m130/ghidra/dec-box-upload-*.json) walks the
 * box's surfaces (pointer +70h, count +78h, 78h bytes each: material +0, geometry +18h) and does more than the
 * static upload. A surface whose material has bit 3 of byte +68h (the transparency sort, which idMaterial::Parse
 * sets only from a transSortMap renderParm with an atlas allocation) gets the sort preprocessing instead, and
 * geometry that borrows its vertex and index arrays (byte +118h Vulkan, +D8h OpenGL) gets its own copies first.
 * Flag word 1 skips the whole upload routine. The wrapper preserves its CPU preparation separately through
 * the native helpers, after the finish's common preparation and in the same surface order. */
#define DD_SURFACES 0x70
#define DD_SURFACE_COUNT 0x78
#define DD_SURFACE_SIZE 0x78
#define DD_SURFACE_GEOMETRY 0x18
#define DD_MATERIAL_FLAGS 0x68
#define DD_MATERIAL_SORTED 8
static const uint32_t g_borrowed[2] = {0xd8, 0x118};
/* The engine's CPU ownership and transparency-sort helpers, verified independently on both images.
 * Keeping their work separate from the static upload preserves CPU geometry for the default generator. */
static const uint32_t g_cpu_helpers[2][2] = {{0xdf5680, 0xdf8270}, {0xe011a0, 0xe03d90}};
static const unsigned char g_detach_head[2][16] = {
    {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9},
    {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x18, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9}
};
static const unsigned char g_sort_head[16] = {
    0x48, 0x8b, 0xc4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x40, 0xc8
};

typedef void (*dd_finish_fn)(void *box);
static dd_finish_fn g_finish;
typedef void (*dd_geometry_fn)(void *geometry);
static dd_geometry_fn g_detach, g_sort;
static unsigned char *g_relay;
static sh_patch_handle g_patch;
static volatile LONG g_installed, g_adjusted, g_declined;
/* The pinned builder and finish bytes verified in this process, so the read evidence applies. */
static volatile LONG g_verified;
static int g_renderer = -1;

/* Open scopes, per execution context (execution_context.h): the serials of a context's open scopes, innermost
 * last, in this module's memory. A fiber keeps its scopes while another fiber runs on the thread, or after it
 * resumes on another; they go when its context retires, and no scope is ever read from a stack. */
typedef struct dd_context { uint64_t id; uint64_t *open; size_t count, capacity; } dd_context;
static SRWLOCK g_context_lock = SRWLOCK_INIT;
static dd_context *g_contexts;
static size_t g_context_count, g_context_capacity;
static volatile LONG64 g_serial;
static void dd_retire(uint64_t id);
static sh_execution_owner g_owner = {dd_retire};

/* Under g_context_lock. */
static dd_context *dd_context_find(uint64_t id, int create)
{
    dd_context *context;
    for (size_t i = 0; i < g_context_count; i++) if (g_contexts[i].id == id) return g_contexts + i;
    if (!create) return NULL;
    if (g_context_count == g_context_capacity) {
        size_t capacity = g_context_capacity ? g_context_capacity * 2 : 16;
        dd_context *grown = (dd_context *)realloc(g_contexts, capacity * sizeof(*grown));
        if (!grown) return NULL;
        g_contexts = grown; g_context_capacity = capacity;
    }
    context = g_contexts + g_context_count++;
    memset(context, 0, sizeof(*context));
    context->id = id;
    return context;
}
static void dd_context_release(dd_context *context)
{
    if (context->count) return;
    free(context->open);
    *context = g_contexts[--g_context_count];
}
/* The context retired (its fiber was deleted, its thread exited or the index was freed): its scopes go with it. */
static void dd_retire(uint64_t id)
{
    AcquireSRWLockExclusive(&g_context_lock);
    dd_context *context = dd_context_find(id, 0);
    if (context) { context->count = 0; dd_context_release(context); }
    ReleaseSRWLockExclusive(&g_context_lock);
}
/* Whether the calling execution context has a scope open. */
static int dd_in_scope(void)
{
    uint64_t id = sh_execution_context(NULL, 0);
    int open = 0;
    if (!id) return 0;
    AcquireSRWLockShared(&g_context_lock);
    const dd_context *context = dd_context_find(id, 0);
    open = context && context->count;
    ReleaseSRWLockShared(&g_context_lock);
    return open;
}
int sh_discrete_default_enter(sh_discrete_scope *scope)
{
    /* The identifier first: no FLS call is made under this module's lock. */
    uint64_t id = sh_execution_context(&g_owner, 1);
    int linked = 0;
    memset(scope, 0, sizeof(*scope));
    if (!id) return 0;
    AcquireSRWLockExclusive(&g_context_lock);
    dd_context *context = dd_context_find(id, 1);
    if (context && context->count == context->capacity) {
        size_t capacity = context->capacity ? context->capacity * 2 : 4;
        uint64_t *grown = (uint64_t *)realloc(context->open, capacity * sizeof(*grown));
        if (grown) { context->open = grown; context->capacity = capacity; }
    }
    if (context && context->count < context->capacity && (scope->serial = sh_execution_next(&g_serial)) != 0) {
        scope->context = id;
        context->open[context->count++] = scope->serial;
        linked = 1;
    } else if (context) dd_context_release(context);
    ReleaseSRWLockExclusive(&g_context_lock);
    return linked;
}
int sh_discrete_default_leave(sh_discrete_scope *scope)
{
    uint64_t id = scope->serial ? sh_execution_context(NULL, 0) : 0;
    int in_order = 0;
    /* Only the context that entered a scope closes it; another context's call changes nothing. */
    if (!id || id != scope->context) return 0;
    AcquireSRWLockExclusive(&g_context_lock);
    dd_context *context = dd_context_find(id, 0);
    size_t i = context ? context->count : 0;
    while (i && context->open[i - 1] != scope->serial) i--;
    if (i) {
        in_order = i == context->count;
        /* Scopes above it belong to frames that already returned. */
        context->count = i - 1;
        dd_context_release(context);
    }
    scope->context = scope->serial = 0;
    ReleaseSRWLockExclusive(&g_context_lock);
    return in_order;
}

/* Validate the native construction's surface references before changing its finish policy. Sorted materials
 * and borrowed arrays are supported; they still receive their native CPU preprocessing below. */
static int dd_readable(const unsigned char *box)
{
    if (g_renderer < 0) return 0;
    __try {
        const unsigned char *surfaces = *(const unsigned char *const *)(box + DD_SURFACES);
        int count = *(const int *)(box + DD_SURFACE_COUNT);
        if (count < 0 || (count && !surfaces)) return 0;
        for (int i = 0; i < count; i++) {
            const unsigned char *surface = surfaces + (size_t)i * DD_SURFACE_SIZE;
            const unsigned char *material = *(const unsigned char *const *)surface;
            const unsigned char *geometry = *(const unsigned char *const *)(surface + DD_SURFACE_GEOMETRY);
            if (!material || !geometry) return 0;
            /* Read both fields while the guarded probe is active, including on a plain surface. */
            volatile unsigned char flags = material[DD_MATERIAL_FLAGS];
            volatile unsigned char borrowed = geometry[g_borrowed[g_renderer]];
            (void)flags; (void)borrowed;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* The finish's common preparation has already run. Preserve surface order and invoke the engine's own
 * transformations, including repeated references to one geometry. Detach borrowed arrays before a sort:
 * the native sort replaces/frees its input arrays and must own them. No upload or CPU-data free follows. */
static void dd_preprocess(unsigned char *box)
{
    unsigned char *surfaces = *(unsigned char **)(box + DD_SURFACES);
    int count = *(int *)(box + DD_SURFACE_COUNT);
    for (int i = 0; i < count; i++) {
        unsigned char *surface = surfaces + (size_t)i * DD_SURFACE_SIZE;
        const unsigned char *material = *(const unsigned char *const *)surface;
        unsigned char *geometry = *(unsigned char **)(surface + DD_SURFACE_GEOMETRY);
        if (geometry[g_borrowed[g_renderer]]) g_detach(geometry);
        if (material[DD_MATERIAL_FLAGS] & DD_MATERIAL_SORTED) g_sort(geometry);
    }
}
/* The redirected call: only the builder reaches it, with its box. */
static void dd_finish(void *box)
{
    volatile uint32_t *flags = (volatile uint32_t *)((unsigned char *)box + DD_FLAGS);
    if (*flags == 2 && dd_in_scope()) {
        if (dd_readable((const unsigned char *)box)) {
            *flags = 1;   /* keep CPU data (bit 0); no static upload (bit 1 clear) */
            g_finish(box);
            dd_preprocess((unsigned char *)box);
            InterlockedIncrement(&g_adjusted);
            return;
        } else if (InterlockedIncrement(&g_declined) == 1) {
            backend_log("discrete animation defaults: unreadable construction data; that box takes the engine's route");
        }
    }
    g_finish(box);
}
/* The pinned builder call of this executable and the finish it calls. */
static int dd_verify(const uint8_t *base, int vulkan, uintptr_t *call, uintptr_t *finish)
{
    const uint8_t *site, *target;
    int32_t displacement;
    if (!base || vulkan < 0 || vulkan > 1) return 0;
    site = base + g_pins[vulkan][0]; target = base + g_pins[vulkan][1];
    __try {
        if (memcmp(site, g_builder, sizeof(g_builder)) || memcmp(target, g_finish_head, sizeof(g_finish_head)))
            return 0;
        if (memcmp(base + g_cpu_helpers[vulkan][0], g_detach_head[vulkan], sizeof(g_detach_head[vulkan])) ||
            memcmp(base + g_cpu_helpers[vulkan][1], g_sort_head, sizeof(g_sort_head))) return 0;
        memcpy(&displacement, site + DD_CALL + 1, sizeof(displacement));
        if (site + DD_CALL + 5 + displacement != target) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    *call = (uintptr_t)(site + DD_CALL); *finish = (uintptr_t)target;
    return 1;
}
/* Our own relay within reach of the call: mov rax,dd_finish; jmp rax. RW while written, RX
 * before publication; a leaf tail jump with no stack changes. */
static unsigned char *dd_relay(uintptr_t call)
{
    SYSTEM_INFO info;
    void *handler = (void *)dd_finish;
    GetSystemInfo(&info);
    uintptr_t step = info.dwAllocationGranularity, center = call & ~(step - 1);
    for (uintptr_t delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[2] = {center >= delta ? center - delta : 0, center + delta};
        for (int i = 0; i < 2; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            unsigned char *relay;
            DWORD old;
            if (candidates[i] < (uintptr_t)info.lpMinimumApplicationAddress ||
                candidates[i] > (uintptr_t)info.lpMaximumApplicationAddress ||
                !VirtualQuery((void *)candidates[i], &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            relay = (unsigned char *)VirtualAlloc((void *)candidates[i], 4096, MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);
            if (!relay) continue;
            relay[0] = 0x48; relay[1] = 0xb8; memcpy(relay + 2, &handler, sizeof handler);
            relay[10] = 0xff; relay[11] = 0xe0;
            if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) {
                VirtualFree(relay, 0, MEM_RELEASE); return NULL;
            }
            FlushInstructionCache(GetCurrentProcess(), relay, 12);
            return relay;
        }
    }
    return NULL;
}
int sh_discrete_default_install(const uint8_t *base)
{
    uintptr_t call = 0, finish = 0;
    unsigned char expected[5], replacement[5] = {0xe8};
    sig_result site = {"DiscreteDefaultBoxFinish", SIG_OK, 0, 0};
    intptr_t distance = 0;
    int32_t displacement;
    const char *refused = NULL;
    char line[256];
    if (InterlockedCompareExchange(&g_installed, 0, 0)) return 1;
    g_renderer = sh_host_is_pinned_renderer_build(1) ? 1 : sh_host_is_pinned_renderer_build(0) ? 0 : -1;
    if (g_renderer < 0) refused = "the executable is not a pinned build";
    else if (!dd_verify(base, g_renderer, &call, &finish)) refused = "its builder call or finish did not verify";
    else if (!(g_relay = dd_relay(call))) refused = "no relay is within reach of the call";
    else {
        distance = (intptr_t)g_relay - (intptr_t)(call + 5);
        if (distance < INT32_MIN || distance > INT32_MAX) refused = "the relay is out of reach of the call";
    }
    InterlockedExchange(&g_verified, call != 0);   /* dd_verify sets call only when the pins verify */
    if (!refused) {
        memcpy(expected, (const void *)call, sizeof(expected));
        displacement = (int32_t)distance;
        memcpy(replacement + 1, &displacement, sizeof(displacement));
        site.addr = call; site.rva = (uint32_t)(call - (uintptr_t)base);
        g_finish = (dd_finish_fn)finish;   /* published before the call can reach the relay */
        g_detach = (dd_geometry_fn)(base + g_cpu_helpers[g_renderer][0]);
        g_sort = (dd_geometry_fn)(base + g_cpu_helpers[g_renderer][1]);
        if (code_patch_call_sig(&site, expected, replacement, &g_patch) != B2_PATCH_OK)
            refused = g_patch.live ? "the redirect did not complete; its relay stays for the outstanding restore" :
                "the redirect was refused";
    }
    if (refused) {
        if (g_relay && !g_patch.live) {
            VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = NULL; g_finish = NULL; g_detach = g_sort = NULL;
        }
        snprintf(line, sizeof(line), "discrete animation defaults: not installed (%s); a retirement that needs "
            "one is refused on Vulkan", refused);
        backend_log(line);
        return 0;
    }
    InterlockedExchange(&g_installed, 1);
    snprintf(line, sizeof(line), "discrete animation defaults: the builder's temporary box stays on the CPU "
        "inside resident scopes (%s)", g_renderer ? "vulkan" : "opengl");
    backend_log(line);
    return 1;
}
int sh_discrete_default_ready(const char *family)
{
    if (!family || _stricmp(family, "discreteanimation")) return 1;
    /* OpenGL maps a static buffer for reading, so its own default completes without the redirect. */
    return InterlockedCompareExchange(&g_installed, 0, 0) || g_renderer == 0;
}
int sh_discrete_default_observed(const char *family)
{
    return family && !_stricmp(family, "discreteanimation") && InterlockedCompareExchange(&g_verified, 0, 0);
}
long sh_discrete_default_adjusted(void) { return InterlockedCompareExchange(&g_adjusted, 0, 0); }
long sh_discrete_default_declined(void) { return InterlockedCompareExchange(&g_declined, 0, 0); }

#ifdef SH_DISCRETE_DEFAULT_TESTING
int sh_discrete_default_test_reset(void)
{
    if (g_patch.live && code_unpatch(&g_patch) != B2_PATCH_OK) return 0;
    if (g_relay) VirtualFree(g_relay, 0, MEM_RELEASE);
    g_relay = NULL; g_finish = NULL; g_detach = g_sort = NULL;
    g_installed = g_adjusted = g_declined = g_verified = 0; g_renderer = -1;
    AcquireSRWLockExclusive(&g_context_lock);
    for (size_t i = 0; i < g_context_count; i++) free(g_contexts[i].open);
    free(g_contexts); g_contexts = NULL; g_context_count = g_context_capacity = 0;
    ReleaseSRWLockExclusive(&g_context_lock);
    return 1;
}
size_t sh_discrete_default_test_contexts(void)
{
    size_t count;
    AcquireSRWLockShared(&g_context_lock);
    count = g_context_count;
    ReleaseSRWLockShared(&g_context_lock);
    return count;
}
void sh_discrete_default_test_helpers(void (*detach)(void *), void (*sort)(void *))
{
    g_detach = detach; g_sort = sort;
}
#endif
