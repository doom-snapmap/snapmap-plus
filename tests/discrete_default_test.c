/* Discrete animation defaults (R-321): pinned verification, the atomic redirect of the builder's
 * finish call, and the flag word inside scopes that belong to an execution context, run through
 * synthetic machine code laid out at both executables' pinned addresses and through real fibers,
 * including each context's retirement when its fiber is deleted, its thread exits or the index is
 * freed. No game image or process is opened. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "discrete_default.h"
#include "execution_context.h"
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#endif

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static char g_log[4096];
void backend_log(const char *message)
{
    size_t used = strlen(g_log), length = message ? strlen(message) : 0;
    if (!length || used + length + 2 >= sizeof(g_log)) return;
    memcpy(g_log + used, message, length); g_log[used + length] = 10; g_log[used + length + 1] = 0;
}
static int pinned = -1;
int sh_host_is_pinned_renderer_build(int vulkan) { return (vulkan == 0 || vulkan == 1) && vulkan == pinned; }
void sh_patch_test_faults(uint64_t protect_calls, uint64_t write_calls, size_t write_prefix);

/* By renderer: 0 OpenGL, 1 Vulkan (m130/ghidra/bytes-*.json). */
static const uint32_t builder_rva[2] = {0x1449762, 0x14559f2}, finish_rva[2] = {0x14bc5f0, 0x14c90d0};
static const uint32_t helper_rva[2][2] = {{0xdf5680, 0xdf8270}, {0xe011a0, 0xe03d90}};
static const unsigned char detach_head[2][16] = {
    {0x40,0x53,0x48,0x83,0xec,0x20,0x80,0xb9,0xd8,0,0,0,0,0x48,0x8b,0xd9},
    {0x40,0x53,0x48,0x83,0xec,0x20,0x80,0xb9,0x18,1,0,0,0,0x48,0x8b,0xd9}
};
static const unsigned char sort_head[16] = {0x48,0x8b,0xc4,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x40,0xc8};
static const unsigned char builder_bytes[] = {0xc7, 0x85, 0xf0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x8d, 0x90, 0x00, 0x00, 0x00};
static const unsigned char finish_head[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9,
    0xe8, 0xb2, 0x06, 0x00, 0x00, 0xf6, 0x43, 0x60, 0x02, 0x74, 0x0a, 0xb2, 0x01, 0x48, 0x8b, 0xcb,
    0xe8, 0xd2, 0x09, 0x00, 0x00, 0xf6, 0x43, 0x60, 0x01, 0x75, 0x0d};
#define IMAGE_SIZE 0x1500000u
#define FRAME_SETUP 8   /* push rbp; mov rbp,rcx; sub rsp,20h */
#define BOX 0x90        /* the box within the builder's frame */
#define UPLOADED 0x7c   /* box byte the synthetic static upload sets (clear of the surface list at +70h..+7Bh) */
#define FREED 0x7d      /* box byte the synthetic CPU-data free sets */
#define PREPARED 0x7e   /* box byte the finish's common preparation sets */
#define SURFACE_SIZE 0x78

static void protect(void *at, size_t size, DWORD protection)
{
    DWORD old;
    CHECK(VirtualProtect(at, size, protection, &old));
    FlushInstructionCache(GetCurrentProcess(), at, size);
}
/* The builder's call site and the finish at their pinned addresses. The builder runs inside a
 * frame setup and returns; the finish's first helper returns, its static upload and its CPU-data
 * free mark the box, and its tail is completed as the executables complete it. */
static unsigned char *image_build(int renderer)
{
    static const unsigned char setup[FRAME_SETUP] = {0x55, 0x48, 0x89, 0xcd, 0x48, 0x83, 0xec, 0x20};
    static const unsigned char leave[] = {0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3};
    static const unsigned char tail[] = {0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9};
    static const unsigned char epilogue[] = {0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
    static const unsigned char upload[] = {0xc6, 0x41, UPLOADED, 0x01, 0xc3};
    static const unsigned char release[] = {0xc6, 0x41, FREED, 0x01, 0xc3};
    static const unsigned char prepare[] = {0xc6, 0x41, PREPARED, 0x01, 0xc3};
    unsigned char *base = (unsigned char *)VirtualAlloc(NULL, IMAGE_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    unsigned char *site, *finish, *code, *page;
    int32_t rel;
    if (!base) return NULL;
    site = base + builder_rva[renderer]; finish = base + finish_rva[renderer];
    code = site - FRAME_SETUP;
    page = (unsigned char *)((uintptr_t)code & ~(uintptr_t)0xfff);
    if (!VirtualAlloc(page, 0x2000, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc((void *)((uintptr_t)finish & ~(uintptr_t)0xfff), 0x1000, MEM_COMMIT, PAGE_READWRITE))
        return NULL;
    memcpy(code, setup, sizeof setup);
    memcpy(site, builder_bytes, sizeof builder_bytes);
    site[sizeof builder_bytes] = 0xe8;
    rel = (int32_t)(finish - (site + sizeof builder_bytes + 5));
    memcpy(site + sizeof builder_bytes + 1, &rel, sizeof rel);
    memcpy(site + sizeof builder_bytes + 5, leave, sizeof leave);
    memcpy(finish, finish_head, sizeof finish_head);
    memcpy(finish + 0x24, tail, sizeof tail);
    rel = 0x200 - 0x31; memcpy(finish + 0x2d, &rel, sizeof rel);
    memcpy(finish + 0x31, epilogue, sizeof epilogue);
    memcpy(finish + 0x6c0, prepare, sizeof prepare);
    memcpy(finish + 0x9f0, upload, sizeof upload);
    memcpy(finish + 0x200, release, sizeof release);
    protect(page, 0x2000, PAGE_EXECUTE_READ);
    protect((void *)((uintptr_t)finish & ~(uintptr_t)0xfff), 0x1000, PAGE_EXECUTE_READ);
    for (int i = 0; i < 2; i++) {
        unsigned char *helper = base + helper_rva[renderer][i];
        void *helper_page = (void *)((uintptr_t)helper & ~(uintptr_t)0xfff);
        if (!VirtualAlloc(helper_page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) return NULL;
        memcpy(helper, i ? sort_head : detach_head[renderer], 16);
        protect(helper_page, 0x1000, PAGE_EXECUTE_READ);
    }
    return base;
}
static unsigned char poke(unsigned char *at, unsigned char value)
{
    unsigned char old = *at;
    protect(at, 1, PAGE_EXECUTE_READWRITE); *at = value; protect(at, 1, PAGE_EXECUTE_READ);
    return old;
}
static const unsigned char *call_target(const unsigned char *base, int renderer)
{
    const unsigned char *call = base + builder_rva[renderer] + sizeof builder_bytes;
    int32_t rel;
    memcpy(&rel, call + 1, sizeof rel);
    return call + 5 + rel;
}
typedef struct outcome { uint32_t flags; int uploaded, freed, prepared; } outcome;
static __declspec(thread) const unsigned char *active_prepared;
/* Build one box through the builder, as the engine's default does, with this surface list (pointer +70h,
 * count +78h, as the builder's surface step leaves it). */
static outcome build_box_with(const unsigned char *base, int renderer, const unsigned char *surfaces, int count)
{
    __declspec(align(16)) unsigned char frame[0x200] = {0};
    const unsigned char *previous_prepared = active_prepared;
    outcome out;
    memcpy(frame + BOX + 0x70, &surfaces, sizeof surfaces); memcpy(frame + BOX + 0x78, &count, sizeof count);
    active_prepared = frame + BOX + PREPARED;
    ((void (*)(unsigned char *))(uintptr_t)(base + builder_rva[renderer] - FRAME_SETUP))(frame);
    active_prepared = previous_prepared;
    memcpy(&out.flags, frame + BOX + 0x60, sizeof out.flags);
    out.uploaded = frame[BOX + UPLOADED]; out.freed = frame[BOX + FREED];
    out.prepared = frame[BOX + PREPARED];
    return out;
}
static outcome build_box(const unsigned char *base, int renderer) { return build_box_with(base, renderer, NULL, 0); }
static int kept(outcome o) { return o.flags == 1 && !o.uploaded && !o.freed && o.prepared; }
static int engine(outcome o) { return o.flags == 2 && o.uploaded && o.freed && o.prepared; }
/* Another thread builds boxes outside any scope of its own, through every redirect state. */
typedef struct runner {
    const unsigned char *base; int renderer;
    volatile LONG stop, iterations, wrong;
    HANDLE ready;
} runner;
static DWORD WINAPI run_builder(void *arg)
{
    runner *run = arg;
    SetEvent(run->ready);
    while (!run->stop) {
        if (!engine(build_box(run->base, run->renderer))) InterlockedIncrement(&run->wrong);
        InterlockedIncrement(&run->iterations);
    }
    return 0;
}
/* Each pinned byte is required: the stored flag word, the finish's bit tests and the call target. */
static void pinned_bytes(unsigned char *base, int renderer)
{
    unsigned char *site = base + builder_rva[renderer], *finish = base + finish_rva[renderer];
    unsigned char *bytes[6] = {site + 6, finish + 0x11, finish + 0x21, site + sizeof builder_bytes + 1,
        base + helper_rva[renderer][0], base + helper_rva[renderer][1]};
    for (int i = 0; i < 6; i++) {
        unsigned char old = poke(bytes[i], (unsigned char)(*bytes[i] + 1));
        g_log[0] = 0;
        CHECK(!sh_discrete_default_install(base) && strstr(g_log, "did not verify"));
        CHECK(call_target(base, renderer) == (i == 3 ? finish + 1 : finish));
        poke(bytes[i], old);
    }
}

typedef struct stack_range { const void *low, *high; } stack_range;
static stack_range stack_now(void)
{
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    stack_range range = {tib->StackLimit, tib->StackBase};
    return range;
}
/* A fiber deleted, or a thread ended, inside its frames leaves their stack poisoning for AddressSanitizer; a later
 * stack at the same addresses would report it. */
static void stack_forget(stack_range range)
{
#ifdef __SANITIZE_ADDRESS__
    if (range.high > range.low)
        __asan_unpoison_memory_region((void *)range.low, (size_t)((const char *)range.high - (const char *)range.low));
#else
    (void)range;
#endif
}
/* Execution context (root's control, m130/root-default-execution-context.txt): a scope belongs to its
 * execution stack. Fibers A and B interleave on one thread out of LIFO order, and A resumes on another
 * thread. A thread-keyed scope would hand A's scope to B, let B's scope reach A, end A's out of order and
 * lose A's scope on the other thread. */
static const unsigned char *g_vk;
static void *main_fiber, *fiber_a, *fiber_b, *thread_fiber;
static int fiber_mode, fiber_ok_a, fiber_ok_b;
static stack_range fiber_stacks[4];
static void CALLBACK fiber_a_body(void *unused)
{
    sh_discrete_scope scope;
    (void)unused;
    fiber_stacks[fiber_mode * 2] = stack_now();
    if (fiber_mode == 0) {
        fiber_ok_a = sh_discrete_default_enter(&scope);
        SwitchToFiber(fiber_b);              /* B builds outside its scope, then opens its own */
        fiber_ok_a = fiber_ok_a && kept(build_box(g_vk, 1));
        fiber_ok_a = sh_discrete_default_leave(&scope) && fiber_ok_a;
        fiber_ok_a = fiber_ok_a && engine(build_box(g_vk, 1));
        SwitchToFiber(fiber_b);              /* B still holds its own scope */
    } else {
        fiber_ok_a = sh_discrete_default_enter(&scope);
        SwitchToFiber(main_fiber);           /* resumed below on another thread */
        fiber_ok_a = fiber_ok_a && kept(build_box(g_vk, 1));
        fiber_ok_a = sh_discrete_default_leave(&scope) && fiber_ok_a;
        fiber_ok_a = fiber_ok_a && engine(build_box(g_vk, 1));
        SwitchToFiber(thread_fiber);
    }
    for (;;) SwitchToFiber(main_fiber);
}
static void CALLBACK fiber_b_body(void *unused)
{
    sh_discrete_scope scope;
    (void)unused;
    fiber_stacks[fiber_mode * 2 + 1] = stack_now();
    if (fiber_mode == 0) {
        fiber_ok_b = engine(build_box(g_vk, 1));   /* A's scope is suspended on another stack */
        fiber_ok_b = sh_discrete_default_enter(&scope) && fiber_ok_b;
        SwitchToFiber(fiber_a);
        fiber_ok_b = fiber_ok_b && kept(build_box(g_vk, 1));
        fiber_ok_b = sh_discrete_default_leave(&scope) && fiber_ok_b;
    }
    for (;;) SwitchToFiber(main_fiber);
}
static DWORD WINAPI resume_thread(void *unused)
{
    (void)unused;
    thread_fiber = ConvertThreadToFiber(NULL);
    if (!thread_fiber) return 1;
    SwitchToFiber(fiber_a);
    ConvertFiberToThread();
    return 0;
}
static void fiber_cases(const unsigned char *vk)
{
    void *fibers[4] = {0};
    size_t made = 0;
    g_vk = vk;
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber != NULL);
    if (!main_fiber) return;
    for (fiber_mode = 0; fiber_mode < 2; fiber_mode++) {
        fiber_ok_a = fiber_ok_b = 0;
        fibers[made++] = fiber_a = CreateFiber(0, fiber_a_body, NULL);
        fibers[made++] = fiber_b = CreateFiber(0, fiber_b_body, NULL);
        CHECK(fiber_a && fiber_b);
        if (!fiber_a || !fiber_b) break;
        SwitchToFiber(fiber_a);
        if (fiber_mode == 1) {
            /* A holds its scope; this stack does not. */
            CHECK(engine(build_box(vk, 1)) && sh_discrete_default_test_contexts() == 1);
            HANDLE thread = CreateThread(NULL, 0, resume_thread, NULL, 0, NULL);
            CHECK(thread != NULL);
            if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
            fiber_ok_b = 1;
        }
        CHECK(fiber_ok_a && fiber_ok_b);
        CHECK(sh_discrete_default_test_contexts() == 0);
    }
    /* Only now: a fiber deleted while suspended leaves its frames' stack poisoning behind for
     * AddressSanitizer, which is cleared since the retirement cases' fibers reuse those addresses. */
    for (size_t i = 0; i < made; i++) if (fibers[i]) DeleteFiber(fibers[i]);
    for (int i = 0; i < 4; i++) stack_forget(fiber_stacks[i]);
    fiber_a = fiber_b = NULL;
    ConvertFiberToThread();
    main_fiber = NULL;
}
/* One context's scopes: strict nesting, exceptions, out-of-order and repeated leaves, and handles that are no open
 * scope of this context. A handle's bytes decide nothing: one never entered, a copy of one already left, another
 * context's call or an overwritten handle closes nothing; only the context's own records open and close scopes. */
static sh_discrete_scope *foreign;
static int foreign_left = -1;
static DWORD WINAPI leave_foreign(void *unused)
{
    (void)unused;
    foreign_left = sh_discrete_default_leave(foreign);
    return 0;
}
static void chain_cases(const unsigned char *vk)
{
    static sh_discrete_scope off_stack;
    sh_discrete_scope outer, inner, copy, never = {0};
    HANDLE thread;
    CHECK(sh_discrete_default_enter(&outer) && sh_discrete_default_enter(&inner));
    CHECK(kept(build_box(vk, 1)));
    CHECK(sh_discrete_default_leave(&inner) && kept(build_box(vk, 1)));
    __try { RaiseException(0xe0800039, 0, 0, NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { CHECK(kept(build_box(vk, 1))); }
    CHECK(sh_discrete_default_leave(&outer) && engine(build_box(vk, 1)));
    CHECK(!sh_discrete_default_leave(&outer) && sh_discrete_default_test_contexts() == 0);
    /* Leaving the outer scope first drops the inner one; the inner one then reports. */
    CHECK(sh_discrete_default_enter(&outer) && sh_discrete_default_enter(&inner));
    CHECK(!sh_discrete_default_leave(&outer) && engine(build_box(vk, 1)));
    CHECK(!sh_discrete_default_leave(&inner) && sh_discrete_default_test_contexts() == 0);
    /* Off the stack, a handle is an ordinary handle: where it is stored decides nothing. */
    CHECK(sh_discrete_default_enter(&outer) && sh_discrete_default_enter(&off_stack) && kept(build_box(vk, 1)));
    CHECK(sh_discrete_default_leave(&off_stack) && sh_discrete_default_leave(&outer) && engine(build_box(vk, 1)));
    /* Never entered, or a copy of a handle already left: nothing closes. */
    CHECK(sh_discrete_default_enter(&outer));
    copy = outer;
    CHECK(!sh_discrete_default_leave(&never) && kept(build_box(vk, 1)));
    CHECK(sh_discrete_default_leave(&outer) && !sh_discrete_default_leave(&copy) && engine(build_box(vk, 1)));
    /* Another context's call changes nothing: the scope stays open until its own context leaves it. */
    CHECK(sh_discrete_default_enter(&outer));
    foreign = &outer;
    thread = CreateThread(NULL, 0, leave_foreign, NULL, 0, NULL);
    CHECK(thread != NULL);
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    CHECK(foreign_left == 0 && kept(build_box(vk, 1)) && sh_discrete_default_leave(&outer));
    /* An overwritten handle, as a frame that never left: its scope stays open until the outer leave drops it and
     * reports. */
    CHECK(sh_discrete_default_enter(&outer) && sh_discrete_default_enter(&inner));
    memset(&inner, 0, sizeof(inner));
    CHECK(!sh_discrete_default_leave(&inner) && kept(build_box(vk, 1)));
    CHECK(!sh_discrete_default_leave(&outer) && engine(build_box(vk, 1)) && sh_discrete_default_test_contexts() == 0);
    /* A fresh scope after all of it works. */
    CHECK(sh_discrete_default_enter(&outer) && kept(build_box(vk, 1)) && sh_discrete_default_leave(&outer));
}

/* Retirement (R-328). The registry this replaces keyed a context by its TIB StackBase and checked an entry by its
 * address and magic bytes: a fiber deleted inside a scope left its entry, and a later stack at the same addresses
 * holding the scope's old bytes reopened it. A context is now owned through fiber local storage, whose callback
 * queues it when the fiber is deleted, the thread exits or the index is freed, and the next lookup by a live context
 * retires it; no scope is read from a stack. These
 * controls delete a fiber suspended inside its scope, run later fibers with its old handle bytes (on its stack
 * addresses when the system reuses them), hand its identifier out again (a synthetic reuse: production identifiers
 * are never reused), end a thread inside a scope, free the index and exhaust the indexes. What they print as
 * observed is this system's behaviour, not a documented guarantee. */
static void test_retire(uint64_t context) { (void)context; }
static sh_execution_owner test_owner = {test_retire};
static sh_discrete_scope held;
static stack_range held_stack, probe_stack;
static int probe_mode, probe_kept, probe_left, probe_reopened;
static uint64_t probe_context, probe_old;
static void CALLBACK scope_fiber(void *unused)
{
    sh_discrete_scope scope;
    (void)unused;
    if (probe_mode == 0) {
        /* Inside its scope until deleted. */
        CHECK(sh_discrete_default_enter(&scope));
        held = scope; held_stack = stack_now();
        for (;;) SwitchToFiber(main_fiber);
    }
    probe_stack = stack_now();
    if (probe_mode == 2) probe_context = sh_execution_context(&test_owner, 1);
    if (probe_mode == 3) {
        /* Inside its scope while the index is freed. */
        CHECK(sh_discrete_default_enter(&scope));
        probe_old = scope.context;
        SwitchToFiber(main_fiber);
    } else {
        /* The deleted fiber's handle bytes back in place: at its very addresses when the stack was reused. */
        memcpy(&scope, &held, sizeof(scope));
    }
    probe_kept = kept(build_box(g_vk, 1));
    probe_left = sh_discrete_default_leave(&scope);
    probe_reopened = sh_discrete_default_enter(&scope) && scope.context != probe_old && kept(build_box(g_vk, 1)) &&
        sh_discrete_default_leave(&scope);
    for (;;) SwitchToFiber(main_fiber);
}
static void probe_run(int mode, uint64_t old)
{
    void *fiber;
    probe_mode = mode; probe_old = old; probe_context = 0;
    probe_kept = probe_left = probe_reopened = -1;
    fiber = CreateFiber(0, scope_fiber, NULL);
    CHECK(fiber != NULL);
    if (!fiber) return;
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    stack_forget(probe_stack);
}
static stack_range exit_stack;
static uint64_t exit_context;
static DWORD WINAPI exit_inside(void *unused)
{
    sh_discrete_scope scope;
    (void)unused;
    if (!sh_discrete_default_enter(&scope)) return 1;
    exit_context = scope.context; exit_stack = stack_now();
    ExitThread(0);   /* inside the scope: its frame never returns */
}
static int boundary_last = -1, boundary_none = -1;
static void CALLBACK boundary_fiber(void *none)
{
    sh_discrete_scope scope;
    probe_stack = stack_now();
    if (!none) boundary_last = sh_execution_context(&test_owner, 1) == UINT64_MAX && sh_discrete_default_enter(&scope) &&
        scope.context == UINT64_MAX && kept(build_box(g_vk, 1)) && sh_discrete_default_leave(&scope);
    else boundary_none = !sh_execution_context(&test_owner, 1) && !sh_discrete_default_enter(&scope) &&
        engine(build_box(g_vk, 1));
    for (;;) SwitchToFiber(main_fiber);
}
static void retirement_cases(const unsigned char *vk)
{
    void *fiber;
    unsigned long on = 0;
    long retired;
    int reused = 0;
    uint64_t resume, before;
    HANDLE thread;
    DWORD thread_id = 0, code = 1, index;
    sh_discrete_scope scope;
    g_vk = vk;
    before = sh_execution_context(NULL, 0);
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber != NULL);
    if (!main_fiber) return;
    printf("observed: ConvertThreadToFiber %s this thread's identifier\n",
        sh_execution_context(NULL, 0) == before ? "kept" : "did not keep");
    /* Deleted while suspended inside its scope: the callback runs on the deleting thread and only queues the
     * context; the next lookup by a live context, here the box's, retires it. */
    probe_mode = 0;
    fiber = CreateFiber(0, scope_fiber, NULL);
    CHECK(fiber != NULL);
    if (!fiber) return;
    SwitchToFiber(fiber);
    CHECK(held.context && sh_discrete_default_test_contexts() == 1 && engine(build_box(vk, 1)));
    retired = sh_execution_context_test_retirements(NULL);
    DeleteFiber(fiber);
    stack_forget(held_stack);
    CHECK(sh_execution_context_test_retirements(&on) == retired + 1 && on == GetCurrentThreadId());
    CHECK(sh_execution_context_test_queued() == 1 && sh_discrete_default_test_contexts() == 1);
    CHECK(engine(build_box(vk, 1)) && !sh_execution_context_test_queued() && sh_discrete_default_test_contexts() == 0);
    /* Later fibers find nothing open, even with its handle's bytes in place on its stack addresses, and open and
     * close their own scopes. */
    for (int attempt = 1; attempt <= 8 && !reused; attempt++) {
        probe_run(1, held.context);
        CHECK(probe_kept == 0 && probe_left == 0 && probe_reopened == 1 && sh_discrete_default_test_contexts() == 0);
        if (probe_stack.high == held_stack.high) reused = attempt;
    }
    printf("observed: the deleted fiber's stack base came back to later fiber %d of 8 (0: never)\n", reused);
    /* Its identifier handed out again: nothing is open under it, and the old handle closes nothing there. */
    resume = sh_execution_context_test_next(held.context);
    probe_run(2, 0);
    sh_execution_context_test_next(resume);
    CHECK(probe_context == held.context && probe_kept == 0 && probe_left == 0 && probe_reopened == 1);
    CHECK(sh_discrete_default_test_contexts() == 0);
    /* A thread that exits inside its scope: the callback runs on that thread and queues the context. */
    retired = sh_execution_context_test_retirements(NULL);
    thread = CreateThread(NULL, 0, exit_inside, NULL, 0, &thread_id);
    CHECK(thread != NULL);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CHECK(GetExitCodeThread(thread, &code) && code == 0);
        CloseHandle(thread);
    }
    stack_forget(exit_stack);
    CHECK(exit_context && sh_execution_context_test_retirements(&on) == retired + 1 && on == thread_id);
    CHECK(sh_execution_context_test_queued() == 1 && sh_discrete_default_test_contexts() == 1);
    CHECK(engine(build_box(vk, 1)) && sh_discrete_default_test_contexts() == 0);
    /* Freeing the index queues every context holding a value: a suspended fiber's scope and this context's go at
     * the next lookup, and each starts again under a new identifier. */
    probe_mode = 3; probe_old = 0; probe_kept = probe_left = probe_reopened = -1;
    fiber = CreateFiber(0, scope_fiber, NULL);
    CHECK(fiber != NULL);
    if (fiber) {
        SwitchToFiber(fiber);
        CHECK(sh_discrete_default_enter(&scope) && sh_discrete_default_test_contexts() == 2);
        before = scope.context;
        retired = sh_execution_context_test_retirements(NULL);
        sh_execution_context_test_shutdown();
        CHECK(sh_execution_context_test_retirements(NULL) >= retired + 2 && sh_execution_context_test_queued() >= 2);
        CHECK(engine(build_box(vk, 1)) && sh_discrete_default_test_contexts() == 0 && !sh_discrete_default_leave(&scope));
        SwitchToFiber(fiber);
        CHECK(probe_kept == 0 && probe_left == 0 && probe_reopened == 1);
        CHECK(sh_discrete_default_enter(&scope) && scope.context != before && kept(build_box(vk, 1)) &&
            sh_discrete_default_leave(&scope));
        DeleteFiber(fiber);
        stack_forget(probe_stack);
    }
    /* No FLS index left: no context can be recorded, so nothing opens and boxes take the engine's route. Once an
     * index is free again, a lookup leaves it free and the next creation takes it, with no reset in between. */
    {
        enum { HOG = 16384 };
        DWORD *hog = (DWORD *)malloc(HOG * sizeof(*hog)), spare;
        size_t hogged = 0;
        CHECK(hog != NULL);
        sh_execution_context_test_shutdown();
        while (hog && hogged < HOG && (hog[hogged] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) hogged++;
        if (hog && hogged && hogged < HOG) {
            CHECK(!sh_discrete_default_enter(&scope) && !scope.serial && engine(build_box(vk, 1)));
            CHECK(!sh_discrete_default_leave(&scope) && sh_discrete_default_test_contexts() == 0);
            CHECK(!sh_execution_context(&test_owner, 1));
            printf("observed: %zu more FLS indexes were available before exhaustion\n", hogged);
            FlsFree(hog[--hogged]);
            CHECK(!sh_execution_context(NULL, 0));
            spare = FlsAlloc(NULL);
            CHECK(spare != FLS_OUT_OF_INDEXES);
            if (spare != FLS_OUT_OF_INDEXES) FlsFree(spare);
            CHECK(sh_discrete_default_enter(&scope) && kept(build_box(vk, 1)) && sh_discrete_default_leave(&scope));
        } else printf("FLS indexes were not exhausted: the exhaustion control did not run\n");
        while (hogged) FlsFree(hog[--hogged]);
        free(hog);
        CHECK(sh_discrete_default_enter(&scope) && kept(build_box(vk, 1)) && sh_discrete_default_leave(&scope));
    }
    /* A caller's last-error value survives the helper; what FlsGetValue itself does to it is printed. */
    index = FlsAlloc(NULL);
    if (index != FLS_OUT_OF_INDEXES && FlsSetValue(index, (void *)1)) {
        SetLastError(42);
        (void)FlsGetValue(index);
        printf("observed: a successful FlsGetValue left the last error %lu (set to 42)\n", GetLastError());
    }
    if (index != FLS_OUT_OF_INDEXES) FlsFree(index);
    SetLastError(42);
    CHECK(sh_execution_context(NULL, 0) && GetLastError() == 42);
    SetLastError(42);
    CHECK(sh_execution_context(&test_owner, 1) && GetLastError() == 42);
    /* Identifiers and serials end in a terminal state instead of wrapping: a counter near its end hands out its
     * last values, then only 0. Through the context path, a new context receives the last identifier and the next
     * none, so it opens no scope. */
    {
        volatile long long counter = (long long)(UINT64_MAX - 2);
        CHECK(sh_execution_next(&counter) == UINT64_MAX - 1 && sh_execution_next(&counter) == UINT64_MAX);
        CHECK(!sh_execution_next(&counter) && !sh_execution_next(&counter) && (uint64_t)counter == UINT64_MAX);
    }
    resume = sh_execution_context_test_next(UINT64_MAX);
    for (int none = 0; none < 2; none++) {
        fiber = CreateFiber(0, boundary_fiber, none ? (void *)1 : NULL);
        CHECK(fiber != NULL);
        if (!fiber) break;
        SwitchToFiber(fiber);
        DeleteFiber(fiber);
        stack_forget(probe_stack);
    }
    sh_execution_context_test_next(resume);
    CHECK(boundary_last == 1 && boundary_none == 1 && sh_discrete_default_test_contexts() == 0);
    /* Converting back to a thread: whether the identifier survives is observed, not documented. */
    before = sh_execution_context(NULL, 0);
    retired = sh_execution_context_test_retirements(NULL);
    ConvertFiberToThread();
    main_fiber = NULL;
    printf("observed: ConvertFiberToThread %s this thread's identifier (%ld retirement(s))\n",
        sh_execution_context(NULL, 0) == before ? "kept" : "did not keep",
        sh_execution_context_test_retirements(NULL) - retired);
}
/* Result-producing stand-ins for the two pinned native helpers. Production calls the engine, not these
 * algorithms. The fixture checks that ownership, complete vertex records, indices and common-preparation
 * order survive the adapter; the paired decompilation establishes the engine helpers' actual algorithms. */
static int helper_renderer, detach_calls, sort_calls;
static int geometry_count(const unsigned char *geometry, size_t offset)
{
    int count; memcpy(&count, geometry + offset, sizeof count); return count;
}
static void *geometry_array(const unsigned char *geometry, size_t offset)
{
    void *array; memcpy(&array, geometry + offset, sizeof array); return array;
}
static void fixture_detach(void *object)
{
    unsigned char *geometry = object;
    const size_t borrowed = helper_renderer ? 0x118 : 0xd8;
    int vertices = geometry_count(geometry, 0x24), indices = geometry_count(geometry, 0x20);
    void *old_vertices = geometry_array(geometry, 0x70), *old_indices = geometry_array(geometry, 0x78);
    void *new_vertices = vertices ? malloc((size_t)vertices * 0x30) : NULL;
    void *new_indices = indices ? malloc((size_t)indices * sizeof(uint16_t)) : NULL;
    CHECK(active_prepared && *active_prepared && geometry[borrowed]);
    CHECK((!vertices || new_vertices) && (!indices || new_indices));
    if ((vertices && !new_vertices) || (indices && !new_indices)) abort();
    if (vertices) memcpy(new_vertices, old_vertices, (size_t)vertices * 0x30);
    if (indices) memcpy(new_indices, old_indices, (size_t)indices * sizeof(uint16_t));
    memcpy(geometry + 0x70, &new_vertices, sizeof new_vertices);
    memcpy(geometry + 0x78, &new_indices, sizeof new_indices);
    geometry[borrowed] = 0;
    detach_calls++;
}
static void fixture_sort(void *object)
{
    unsigned char *geometry = object;
    int vertices = geometry_count(geometry, 0x24), indices = geometry_count(geometry, 0x20);
    unsigned char *old_vertices = geometry_array(geometry, 0x70);
    uint16_t *old_indices = geometry_array(geometry, 0x78);
    unsigned char *new_vertices = vertices ? malloc((size_t)vertices * 0x30) : NULL;
    uint16_t *new_indices = indices ? malloc((size_t)indices * sizeof(uint16_t)) : NULL;
    CHECK(active_prepared && *active_prepared && !geometry[helper_renderer ? 0x118 : 0xd8]);
    CHECK((!vertices || new_vertices) && (!indices || new_indices));
    if ((vertices && !new_vertices) || (indices && !new_indices)) abort();
    /* Rotate full records and remap indices: a second call has a distinct, observable result. */
    for (int i = 0; i < vertices; i++)
        memcpy(new_vertices + (size_t)i * 0x30, old_vertices + (size_t)((i + 1) % vertices) * 0x30, 0x30);
    for (int i = 0; i < indices; i++) {
        CHECK(vertices > 0 && old_indices[i] < vertices);
        new_indices[i] = (uint16_t)((old_indices[i] + vertices - 1) % vertices);
    }
    free(old_vertices); free(old_indices);
    memcpy(geometry + 0x70, &new_vertices, sizeof new_vertices);
    memcpy(geometry + 0x78, &new_indices, sizeof new_indices);
    sort_calls++;
}
static void geometry_results(const unsigned char *base, int renderer)
{
    static const uint16_t source_indices[6] = {0,1,2,0,2,3};
    unsigned char source_vertices[4][0x30];
    helper_renderer = renderer;
    sh_discrete_default_test_helpers(fixture_detach, fixture_sort);
    for (int vertex = 0; vertex < 4; vertex++)
        for (int byte = 0; byte < 0x30; byte++) source_vertices[vertex][byte] = (unsigned char)(vertex * 53 + byte);
    /* Each construction owns a fresh result. Shared geometry appears once per surface, as in the builder. */
    for (int repetition = 0; repetition < 3; repetition++) for (int borrowed = 0; borrowed < 2; borrowed++)
    for (int sorted = 0; sorted < 3; sorted++) {
        unsigned char material[2][0x100] = {{0}}, geometry[0x200] = {0}, surfaces[2 * SURFACE_SIZE] = {0};
        unsigned char preserved_vertices[sizeof source_vertices];
        uint16_t preserved_indices[6];
        void *vertices = borrowed ? (void *)source_vertices : malloc(sizeof source_vertices);
        void *indices = borrowed ? (void *)source_indices : malloc(sizeof source_indices);
        int vertex_count = 4, index_count = 6;
        sh_discrete_scope scope;
        CHECK(vertices && indices);
        if (!vertices || !indices) abort();
        memcpy(preserved_vertices, source_vertices, sizeof source_vertices);
        memcpy(preserved_indices, source_indices, sizeof source_indices);
        if (!borrowed) { memcpy(vertices, source_vertices, sizeof source_vertices); memcpy(indices, source_indices, sizeof source_indices); }
        memcpy(geometry + 0x20, &index_count, sizeof index_count);
        memcpy(geometry + 0x24, &vertex_count, sizeof vertex_count);
        memcpy(geometry + 0x70, &vertices, sizeof vertices); memcpy(geometry + 0x78, &indices, sizeof indices);
        geometry[renderer ? 0x118 : 0xd8] = (unsigned char)borrowed;
        for (int i = 0; i < 2; i++) {
            void *mat = material[i], *mesh = geometry;
            material[i][0x68] = i < sorted ? 8 : 0;
            memcpy(surfaces + i * SURFACE_SIZE, &mat, sizeof mat);
            memcpy(surfaces + i * SURFACE_SIZE + 0x18, &mesh, sizeof mesh);
        }
        detach_calls = sort_calls = 0;
        CHECK(sh_discrete_default_enter(&scope));
        CHECK(kept(build_box_with(base, renderer, surfaces, 2)));
        CHECK(sh_discrete_default_leave(&scope));
        unsigned char *result_vertices = geometry_array(geometry, 0x70);
        uint16_t *result_indices = geometry_array(geometry, 0x78);
        CHECK(detach_calls == borrowed && sort_calls == sorted && !geometry[renderer ? 0x118 : 0xd8]);
        CHECK(geometry_count(geometry, 0x24) == 4 && geometry_count(geometry, 0x20) == 6);
        if (borrowed) CHECK(result_vertices != (void *)source_vertices && result_indices != (void *)source_indices);
        for (int i = 0; i < 4; i++) CHECK(!memcmp(result_vertices + (size_t)i * 0x30, source_vertices[(i + sorted) % 4], 0x30));
        for (int i = 0; i < 6; i++) CHECK(result_indices[i] == (source_indices[i] + 4 - sorted) % 4);
        CHECK(!memcmp(source_vertices, preserved_vertices, sizeof source_vertices));
        CHECK(!memcmp(source_indices, preserved_indices, sizeof source_indices));
        free(result_vertices); free(result_indices);
    }
}
/* The box's real material and geometry determine CPU preprocessing. Unreadable construction data still
 * leaves the original finish policy intact; sorted or borrowed geometry is no longer a refusal condition. */
static void surface_cases(const unsigned char *base, int renderer)
{
    static unsigned char materials[2][0x100], geometry[2][0x200];
    static __declspec(align(16)) unsigned char surfaces[2 * SURFACE_SIZE];
    const size_t borrowed = renderer ? 0x118 : 0xd8, other = renderer ? 0xd8 : 0x118;
    long adjusted = sh_discrete_default_adjusted(), declined = sh_discrete_default_declined();
    unsigned char *closed = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    void *saved;
    sh_discrete_scope scope;
    helper_renderer = renderer;
    sh_discrete_default_test_helpers(fixture_detach, fixture_sort);
    memset(materials, 0, sizeof materials); memset(geometry, 0, sizeof geometry); memset(surfaces, 0, sizeof surfaces);
    for (int i = 0; i < 2; i++) {
        unsigned char *material = materials[i], *mesh = geometry[i];
        memcpy(surfaces + i * SURFACE_SIZE, &material, sizeof material);
        memcpy(surfaces + i * SURFACE_SIZE + 0x18, &mesh, sizeof mesh);
    }
    CHECK(closed != NULL);
    /* Outside a scope nothing is read or changed. */
    geometry[1][borrowed] = 1;
    CHECK(engine(build_box_with(base, renderer, surfaces, 2)));
    geometry[1][borrowed] = 0;
    CHECK(sh_discrete_default_adjusted() == adjusted && sh_discrete_default_declined() == declined);
    CHECK(sh_discrete_default_enter(&scope));
    CHECK(kept(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_adjusted() == ++adjusted);
    /* Other material bits and the other renderer's borrowed byte do not matter. */
    materials[1][0x68] = 0xf7; geometry[0][other] = 1;
    CHECK(kept(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_adjusted() == ++adjusted);
    /* Sorted and borrowed surfaces keep their CPU preprocessing, without any static upload. */
    materials[1][0x68] = 0x08; g_log[0] = 0;
    CHECK(kept(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_adjusted() == ++adjusted);
    materials[1][0x68] = 0;
    geometry[0][borrowed] = 1;
    CHECK(kept(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_adjusted() == ++adjusted);
    geometry[0][borrowed] = 0;
    for (int field = 0; field < 2; field++) {
        unsigned char *slot = surfaces + SURFACE_SIZE + (field ? 0x18 : 0);
        memcpy(&saved, slot, sizeof saved); memset(slot, 0, sizeof saved);
        CHECK(engine(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_declined() == ++declined);
        memcpy(slot, &saved, sizeof saved);
    }
    CHECK(engine(build_box_with(base, renderer, surfaces, -1)) && sh_discrete_default_declined() == ++declined);
    CHECK(engine(build_box_with(base, renderer, NULL, 2)) && sh_discrete_default_declined() == ++declined);
    if (closed) {
        CHECK(engine(build_box_with(base, renderer, closed, 2)) && sh_discrete_default_declined() == ++declined);
        VirtualFree(closed, 0, MEM_RELEASE);
    }
    /* A box without surfaces needs nothing of the routine; restored surfaces are plain again. */
    CHECK(kept(build_box_with(base, renderer, NULL, 0)) && sh_discrete_default_adjusted() == ++adjusted);
    CHECK(kept(build_box_with(base, renderer, surfaces, 2)) && sh_discrete_default_adjusted() == ++adjusted);
    CHECK(sh_discrete_default_leave(&scope) && sh_discrete_default_declined() == declined);
    geometry_results(base, renderer);
}
static void vulkan_cases(unsigned char *vk)
{
    const unsigned char *finish = vk + finish_rva[1], *relay;
    runner run = {vk, 1};
    HANDLE thread;
    sh_discrete_scope scope, nested;
    outcome o;
    /* An executable that is not pinned: nothing installs, and only the discrete family is unbuildable. */
    pinned = -1; g_log[0] = 0;
    CHECK(!sh_discrete_default_install(vk) && strstr(g_log, "not a pinned build") && call_target(vk, 1) == finish);
    CHECK(!sh_discrete_default_observed("discreteanimation"));
    CHECK(!sh_discrete_default_ready("discreteAnimation") && sh_discrete_default_ready("model") &&
        sh_discrete_default_ready(NULL));
    /* Before any redirect the engine's route runs, in a scope or not: upload, then free CPU data. */
    CHECK(sh_discrete_default_enter(&scope));
    CHECK(engine(build_box(vk, 1)));
    CHECK(sh_discrete_default_leave(&scope));
    pinned = 1; pinned_bytes(vk, 1);
    CHECK(!sh_discrete_default_ready("discreteanimation"));
    /* The OpenGL pins do not describe this executable. */
    pinned = 0; g_log[0] = 0;
    CHECK(!sh_discrete_default_install(vk) && strstr(g_log, "did not verify") && call_target(vk, 1) == finish);
    CHECK(!sh_discrete_default_observed("discreteanimation"));
    /* A refused page protection leaves the call and frees the relay. */
    pinned = 1; sh_patch_test_faults(1, 0, 0); g_log[0] = 0;
    CHECK(!sh_discrete_default_install(vk) && strstr(g_log, "the redirect was refused") && call_target(vk, 1) == finish);
    /* The read evidence is about the verified code, not the redirect; without it the family is still unbuildable. */
    CHECK(sh_discrete_default_observed("discreteanimation") && !sh_discrete_default_ready("discreteanimation"));
    sh_patch_test_faults(0, 0, 0);
    /* The redirect is one aligned displacement exchange while another thread runs the builder. */
    run.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    thread = run.ready ? CreateThread(NULL, 0, run_builder, &run, 0, NULL) : NULL;
    CHECK(thread && WaitForSingleObject(run.ready, 2000) == WAIT_OBJECT_0);
    g_log[0] = 0;
    CHECK(sh_discrete_default_install(vk) && strstr(g_log, "inside resident scopes (vulkan)"));
    relay = call_target(vk, 1);
    CHECK(relay != finish && relay[0] == 0x48 && relay[1] == 0xb8 && relay[10] == 0xff && relay[11] == 0xe0);
    CHECK(sh_discrete_default_install(vk) && call_target(vk, 1) == relay);
    CHECK(sh_discrete_default_ready("discreteanimation"));
    CHECK(sh_discrete_default_observed("DiscreteAnimation") && !sh_discrete_default_observed("model") &&
        !sh_discrete_default_observed(NULL));
    /* Outside a scope nothing changes. */
    o = build_box(vk, 1);
    CHECK(engine(o) && sh_discrete_default_adjusted() == 0);
    /* Inside one the box stays on the CPU: no static upload, CPU data kept. The other thread holds
     * no scope of its own and keeps the engine's route. */
    CHECK(sh_discrete_default_enter(&scope));
    o = build_box(vk, 1);
    CHECK(kept(o) && sh_discrete_default_adjusted() == 1);
    Sleep(20);
    CHECK(sh_discrete_default_enter(&nested) && sh_discrete_default_leave(&nested));
    o = build_box(vk, 1);
    CHECK(kept(o) && sh_discrete_default_adjusted() == 2);
    {
        /* Only a flag word of exactly 2 changes. */
        __declspec(align(16)) unsigned char box[0x100] = {0};
        uint32_t flags = 3;
        memcpy(box + 0x60, &flags, sizeof flags);
        ((void (*)(void *))(uintptr_t)relay)(box);
        memcpy(&flags, box + 0x60, sizeof flags);
        CHECK(flags == 3 && box[UPLOADED] && !box[FREED] && sh_discrete_default_adjusted() == 2);
    }
    CHECK(sh_discrete_default_leave(&scope));
    surface_cases(vk, 1);
    o = build_box(vk, 1); CHECK(engine(o));
    run.stop = 1;
    if (thread) { CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0); CloseHandle(thread); }
    if (run.ready) CloseHandle(run.ready);
    CHECK(run.iterations > 0 && run.wrong == 0);
    chain_cases(vk);
    fiber_cases(vk);
    retirement_cases(vk);
    CHECK(sh_discrete_default_test_reset() && call_target(vk, 1) == finish);
    CHECK(!sh_discrete_default_ready("discreteanimation") && !sh_discrete_default_observed("discreteanimation"));
}
static void opengl_cases(unsigned char *gl)
{
    const unsigned char *finish = gl + finish_rva[0];
    sh_discrete_scope scope;
    pinned = 0; pinned_bytes(gl, 0);
    /* OpenGL maps a static buffer for reading: its default is buildable without the redirect. */
    CHECK(sh_discrete_default_ready("discreteanimation") && !sh_discrete_default_observed("discreteanimation"));
    g_log[0] = 0;
    CHECK(sh_discrete_default_install(gl) && strstr(g_log, "inside resident scopes (opengl)"));
    CHECK(call_target(gl, 0) != finish && sh_discrete_default_observed("discreteanimation"));
    CHECK(sh_discrete_default_enter(&scope));
    CHECK(kept(build_box(gl, 0)));
    CHECK(sh_discrete_default_leave(&scope));
    CHECK(engine(build_box(gl, 0)));
    surface_cases(gl, 0);
    CHECK(sh_discrete_default_test_reset() && call_target(gl, 0) == finish);
    CHECK(!sh_discrete_default_observed("discreteanimation"));
}
/* Paired-image check (tests/run-tests.ps1 -Doom / -DoomAlt): the pinned executable, mapped by sections. The other
 * renderer's pins do not verify there; this renderer's builder call targets its finish, and the redirect installs
 * there and is restored. Nothing in it is executed. */
static void image_cases(const char *path, int renderer)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *file = NULL, *base = NULL;
    if (!f) { CHECK(!"the image opens"); return; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    file = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(file); CHECK(!"the image reads"); return; }
    fclose(f);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(file + ((const IMAGE_DOS_HEADER *)file)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    base = (unsigned char *)VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(base != NULL);
    if (base) {
        const unsigned char *finish = base + finish_rva[renderer];
        memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            DWORD length = sec[i].SizeOfRawData;
            if (sec[i].Misc.VirtualSize && sec[i].Misc.VirtualSize < length) length = sec[i].Misc.VirtualSize;
            if (length && sec[i].PointerToRawData + (size_t)length <= (size_t)size &&
                sec[i].VirtualAddress + (size_t)length <= nt->OptionalHeader.SizeOfImage)
                memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData, length);
        }
        pinned = !renderer; g_log[0] = 0;
        CHECK(!sh_discrete_default_install(base) && strstr(g_log, "did not verify"));
        CHECK(!sh_discrete_default_observed("discreteanimation"));
        CHECK(sh_discrete_default_test_reset());
        pinned = renderer; g_log[0] = 0;
        CHECK(call_target(base, renderer) == finish);
        CHECK(sh_discrete_default_install(base) && call_target(base, renderer) != finish);
        CHECK(sh_discrete_default_observed("discreteanimation"));
        CHECK(sh_discrete_default_test_reset() && call_target(base, renderer) == finish);
        printf("%s: %s builder call and finish verified; redirect installed and restored\n", path,
            renderer ? "Vulkan" : "OpenGL");
        VirtualFree(base, 0, MEM_RELEASE);
    }
    free(file);
}
int main(int argc, char **argv)
{
    if (argc > 2) image_cases(argv[1], atoi(argv[2]) != 0);
    else {
        unsigned char *vk = image_build(1), *gl = image_build(0);
        CHECK(vk && gl);
        if (vk && gl) { vulkan_cases(vk); opengl_cases(gl); }
    }
    printf("discrete defaults: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
