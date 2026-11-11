/* Exercise real access faults and injected protection/rollback failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"
#include "hook.h"

void backend_log(const char *line) { (void)line; }
static int failures;
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

/* Quiesced writes: other threads are suspended while bytes change, a thread
 * inside the window (or returning into it) blocks the write, and a thread at
 * the window's first byte does not. */
static volatile LONG g_spin_counter;
static HANDLE g_worker;
static int g_worker_suspended_during_write;

static DWORD WINAPI busy_worker(LPVOID unused)
{
    (void)unused;
    for (;;) InterlockedIncrement(&g_spin_counter);
}

static void observe_worker(void *target)
{
    DWORD previous;
    (void)target;
    previous = SuspendThread(g_worker);
    if (previous != (DWORD)-1) {
        ResumeThread(g_worker);
        if (previous >= 1) g_worker_suspended_during_write = 1;
    }
}

static HANDLE start_at(unsigned char *code)
{
    HANDLE thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)code, NULL, 0, NULL);
    Sleep(50);   /* let it reach its spin */
    return thread;
}

typedef long long (*add3_fn)(long long);
static long long add_thousand(long long x) { return x + 1000; }
static volatile LONG g_stress_stop, g_stress_bad;
static add3_fn g_stress_target;

static DWORD WINAPI stress_caller(LPVOID unused)
{
    (void)unused;
    for (long long i = 0; !g_stress_stop; i++) {
        long long r = g_stress_target(i);
        if (r != i + 3 && r != i + 1000) InterlockedIncrement(&g_stress_bad);
    }
    return 0;
}

static void quiesced_writes(void)
{
    unsigned char *page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    sh_patch_handle handle;
    HANDLE spinner;
    int32_t displacement;
    if (!page) { failures++; return; }

    /* Another thread is suspended while the bytes are written. */
    memcpy(page, "\xb8\x11\x00\x00\x00\xc3", 6);
    g_worker = CreateThread(NULL, 0, busy_worker, NULL, 0, NULL);
    Sleep(20);
    sh_patch_test_observe_write(observe_worker);
    CHECK(code_patch(page + 1, (const uint8_t *)"\x11\x00\x00\x00", (const uint8_t *)"\x22\x00\x00\x00",
                     4, &handle) == B2_PATCH_OK);
    sh_patch_test_observe_write(NULL);
    CHECK(g_worker_suspended_during_write);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK);
    TerminateThread(g_worker, 0);
    CloseHandle(g_worker);

    /* A thread spinning inside the window blocks the write; nothing changes. */
    memcpy(page + 256, "\x90\x90\xeb\xfe\x90\x90\x90\x90", 8);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    spinner = start_at(page + 256);
    CHECK(code_patch(page + 256, NULL, (const uint8_t *)"\xc3\xc3\xc3\xc3\xc3\xc3\xc3\xc3", 8,
                     &handle) == B2_PATCH_FAIL_BUSY);
    CHECK(!handle.live && !memcmp(page + 256, "\x90\x90\xeb\xfe", 4));
    TerminateThread(spinner, 0);
    CloseHandle(spinner);

    /* A thread in a callee whose return address lies inside the window blocks it. */
    displacement = (int32_t)((page + 640) - (page + 512 + 5));
    page[512] = 0xe8; memcpy(page + 513, &displacement, 4);
    memcpy(page + 517, "\x90\x90\x90\xc3", 4);
    memcpy(page + 640, "\xeb\xfe", 2);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    spinner = start_at(page + 512);
    CHECK(code_patch(page + 512, NULL, (const uint8_t *)"\xc3\xc3\xc3\xc3\xc3\xc3\xc3\xc3", 8,
                     &handle) == B2_PATCH_FAIL_BUSY);
    CHECK(!handle.live && page[512] == 0xe8);
    TerminateThread(spinner, 0);
    CloseHandle(spinner);

    /* A thread at the window's first byte runs the new bytes: here a return that ends it. */
    memcpy(page + 768, "\xeb\xfe", 2);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    spinner = start_at(page + 768);
    CHECK(code_patch(page + 768, (const uint8_t *)"\xeb\xfe", (const uint8_t *)"\xc3\x90", 2,
                     &handle) == B2_PATCH_OK);
    CHECK(WaitForSingleObject(spinner, 5000) == WAIT_OBJECT_0);
    CloseHandle(spinner);

    /* Detours committed and removed over a function another thread keeps calling:
     * the caller only ever sees the original or the detour, never torn code. */
    memcpy(page + 1024, "\x48\x89\xc8\x48\x83\xc0\x01\x48\x83\xc0\x01\x48\x83\xc0\x01\xc3", 16);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    g_stress_target = (add3_fn)(void *)(page + 1024);
    CHECK(g_stress_target(4) == 7);
    spinner = CreateThread(NULL, 0, stress_caller, NULL, 0, NULL);
    for (int i = 0; i < 300; i++) {
        void *tramp = hook_prepare(page + 1024, (void *)add_thousand, 15);
        CHECK(tramp && hook_commit(tramp) == B2_PATCH_OK);
        CHECK(hook_unpatch(tramp));
    }
    g_stress_stop = 1;
    CHECK(WaitForSingleObject(spinner, 5000) == WAIT_OBJECT_0);
    CloseHandle(spinner);
    CHECK(g_stress_bad == 0);
    VirtualFree(page, 0, MEM_RELEASE);
}

int main(void)
{
    unsigned char original[8] = {1,2,3,4,5,6,7,8}, replacement[8] = {8,7,6,5,4,3,2,1};
    unsigned char *target = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    unsigned char *source = VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sh_patch_handle handle;
    DWORD ignored;
    MEMORY_BASIC_INFORMATION memory;
    if (!target || !source) return 1;
    memcpy(target, original, sizeof original);
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READ, &ignored));
    memcpy(source + 4092, replacement, 4);
    CHECK(VirtualProtect(source + 4096, 4096, PAGE_NOACCESS, &ignored));
    CHECK(code_patch(target, original, source + 4092, 8, &handle) == B2_PATCH_FAIL_SEH);
    CHECK(!handle.live && !memcmp(target, original, 8));
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);

    sh_patch_test_faults(1, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_PROTECT);
    CHECK(!handle.live && !memcmp(target, original, 8));
    sh_patch_test_faults(2, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_PROTECT);
    CHECK(!handle.live && !memcmp(target, original, 8));
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live && handle.target == target && handle.len == 8);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !handle.live);
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);

    sh_patch_test_faults(0, 1 | 2, 4);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !memcmp(target, original, 8));
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_OK);
    sh_patch_test_faults(1, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_FAIL_PROTECT && handle.live);
    CHECK(!memcmp(target, replacement, 8));
    sh_patch_test_faults(0, 1, 3);
    CHECK(code_unpatch(&handle) == B2_PATCH_FAIL_SEH && handle.live);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !memcmp(target, original, 8));

    /* A call redirect is an ordinary quiesced patch with the same rollback. */
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READWRITE, &ignored));
    memcpy(target + 3, "\xe8\x01\x02\x03\x04", 5);
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READ, &ignored));
    sig_result site = {"call", SIG_OK, (uintptr_t)(target + 3), 3};
    const uint8_t before[5] = {0xe8,1,2,3,4}, after[5] = {0xe8,5,6,7,8};
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(code_patch_call_sig(&site, before, after, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live && !memcmp(target + 3, before, 5));
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK);
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);
    site.status = SIG_NOT_FOUND;
    CHECK(code_patch_call_sig(&site, before, after, &handle) == B2_PATCH_REFUSED_SIG && !handle.live);

    quiesced_writes();
    CHECK(sh_patch_selftest());
    VirtualFree(source, 0, MEM_RELEASE);
    VirtualFree(target, 0, MEM_RELEASE);
    printf("patch_test: %d failures\n", failures);
    return failures != 0;
}
