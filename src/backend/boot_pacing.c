/* Boot pacing (see boot_pacing.h).
 *
 * Build 13954591 evidence (snaphak-re boot-time-13954591 R-001, R-002):
 *   Load-screen video draw VK FUN_141901c80 / GL FUN_1418fab70, render thread:
 *     LEA RDI,[bink lock] (VK 0x146fd5228 / GL 0x1458dff48, a global holding a
 *     CRITICAL_SECTION pointer; enter = VK FUN_140324320 TryEnter-then-Enter,
 *     leave = FUN_140324370 LeaveCriticalSection); if the first handle
 *     (VK 0x146fd54b0) is open, upload and draw both handles, then
 *     CALL Sleep(10) at VK 0x141901e9b / GL 0x1418fad79 still holding the lock;
 *     after it, re-test the second handle (VK 0x146fd55a0) and read its frame
 *     number, then leave. Every other user of the lock (open VK FUN_141900d00,
 *     close FUN_141901600 which nulls both handles, decode FUN_141902780,
 *     BinkService FUN_1419015a0 on the stream thread, the done query
 *     FUN_141902b60) takes the same lock around its whole access. So the only
 *     state the draw uses after its sleep is re-read after the lock is
 *     re-taken, and releasing the lock for the sleep lets another thread run
 *     exactly what it could already run once the draw returns.
 *   UncachedBackgroundRead VK FUN_141938d70 / GL FUN_14192d6e0: queues one read
 *     with a stack completion byte ([rsp+0x70]), signals the stream thread,
 *     then `while (!done && !this->abort) Sleep(1)` with the call at
 *     VK 0x141938ebe / GL 0x14192d82e; RSI holds `this` throughout.
 *   Both Sleep calls go through the engine's Sleep thunk (REX JMP [IAT]), VK
 *     0x140324620 / GL 0x1403241b0. WinMain calls timeBeginPeriod(1), so each
 *     Sleep(1) costs one 1 ms timer tick or more.
 * Only the two calls' rel32 operands are changed; each points at a product
 * stub on a page within reach of the engine. */
#include "boot_pacing.h"
#include <string.h>
#include <windows.h>

int sh_boot_rel32(uintptr_t next, uintptr_t dest, int32_t *disp)
{
    intptr_t distance = (intptr_t)dest - (intptr_t)next;
    if (!disp || distance < INT32_MIN || distance > INT32_MAX) return 0;
    *disp = (int32_t)distance;
    return 1;
}

static uintptr_t rel_target(const unsigned char *site, size_t disp_at, size_t next_at)
{
    int32_t disp;
    memcpy(&disp, site + disp_at, 4);
    return (uintptr_t)site + next_at + (intptr_t)disp;
}

int sh_boot_video_lock_site(const unsigned char *site, uintptr_t *lock, uintptr_t *handle)
{
    static const unsigned char lea[3] = {0x48, 0x8d, 0x3d},
        mid[15] = {0x48, 0x89, 0x7c, 0x24, 0x58, 0xb2, 0x01, 0x48, 0x8b, 0xcf, 0xe8},
        set[8] = {0xc6, 0x44, 0x24, 0x60, 0x01, 0x48, 0x83, 0x3d};
    if (!site || !lock || !handle) return 0;
    if (memcmp(site, lea, 3) || memcmp(site + 7, mid, 11) || memcmp(site + 22, set, 8) ||
        site[34] != 0x00 || site[35] != 0x0f || site[36] != 0x84) return 0;
    *lock = rel_target(site, 3, 7);
    *handle = rel_target(site, 30, 35);
    return 1;
}

int sh_boot_video_sleep_site(const unsigned char *site, uintptr_t *first, uintptr_t *second,
                             uintptr_t *sleep)
{
    static const unsigned char args[6] = {0x44, 0x8b, 0xce, 0x44, 0x8b, 0xc5},
        call_ten[6] = {0xb9, 0x0a, 0x00, 0x00, 0x00, 0xe8};
    uintptr_t tested;
    if (!site || !first || !second || !sleep) return 0;
    if (site[0] != 0x80 || site[1] != 0x3d || site[6] != 0x00 || site[7] != 0x74 ||
        memcmp(site + 9, args, 6) || site[15] != 0x48 || site[16] != 0x8d || site[17] != 0x15 ||
        memcmp(site + 0x1e, args, 6) || site[0x24] != 0x48 || site[0x25] != 0x8d || site[0x26] != 0x15 ||
        memcmp(site + 0x33, call_ten, 6) || site[0x3d] != 0x48 || site[0x3e] != 0x83 ||
        site[0x3f] != 0x3d || site[0x44] != 0x00) return 0;
    *first = rel_target(site, 0x12, 0x16);
    *second = rel_target(site, 0x27, 0x2b);
    *sleep = rel_target(site, SH_BOOT_SLEEP_CALL + 1, SH_BOOT_SLEEP_CALL + 5);
    /* The CMP right after the sleep re-tests the second handle. */
    tested = rel_target(site, 0x40, 0x45);
    return tested == *second;
}

int sh_boot_read_poll_site(const unsigned char *site, uintptr_t *sleep)
{
    static const unsigned char head[22] = {0x0f, 0xb6, 0x44, 0x24, SH_BOOT_POLL_DONE_OFF, 0x84, 0xc0, 0x75, 0x1b,
        0x0f, 0xb6, 0x46, SH_BOOT_POLL_ABORT_OFF, 0x84, 0xc0, 0x75, 0x13, 0xb9, 0x01, 0x00, 0x00, 0x00},
        tail[9] = {0x0f, 0xb6, 0x44, 0x24, SH_BOOT_POLL_DONE_OFF, 0x84, 0xc0, 0x74, 0xe5};
    if (!site || !sleep) return 0;
    if (memcmp(site, head, sizeof head) || site[SH_BOOT_POLL_CALL] != 0xe8 ||
        memcmp(site + SH_BOOT_POLL_CALL + 5, tail, sizeof tail)) return 0;
    *sleep = rel_target(site, SH_BOOT_POLL_CALL + 1, SH_BOOT_POLL_CALL + 5);
    return 1;
}

int sh_boot_sleep_thunk(const unsigned char *code)
{
    return code && code[0] == 0x48 && code[1] == 0xff && code[2] == 0x25;
}

size_t sh_boot_poll_stub(unsigned char *out, size_t cap, uintptr_t wait_fn)
{
    /* Entered by the engine's CALL, so the caller's [rsp+X] is [rsp+X+8]:
     *   lea rdx,[rsp+0x78]; lea r8,[rsi+0x10]; mov rax,wait_fn; jmp rax
     * ECX (the sleep length) passes through as the first argument. */
    unsigned char code[21] = {0x48, 0x8d, 0x54, 0x24, SH_BOOT_POLL_DONE_OFF + 8,
        0x4c, 0x8d, 0x46, SH_BOOT_POLL_ABORT_OFF, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    uint64_t fn = wait_fn;
    if (!out || cap < sizeof code) return 0;
    memcpy(code + 11, &fn, 8);
    memcpy(out, code, sizeof code);
    return sizeof code;
}

size_t sh_boot_jump_stub(unsigned char *out, size_t cap, uintptr_t fn)
{
    unsigned char code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    uint64_t target = fn;
    if (!out || cap < sizeof code) return 0;
    memcpy(code + 2, &target, 8);
    memcpy(out, code, sizeof code);
    return sizeof code;
}

/* The global that holds the Bink CRITICAL_SECTION pointer, set at install. */
static void *volatile *g_video_lock;
static volatile LONG g_reads, g_spun, g_yielded, g_slept, g_video_sleeps, g_video_released;
static LARGE_INTEGER g_qpf;

static void stats_line(void);

void sh_boot_read_wait(unsigned long ms, volatile const char *done, volatile const char *abort_flag)
{
    LARGE_INTEGER t0, now;
    LONGLONG spin_ticks, yield_ticks;
    LONG n;
    if (ms != 1 || !done || !abort_flag) { Sleep(ms); return; }
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    n = InterlockedIncrement(&g_reads);
    if (n >= 1024 && (n & (n - 1)) == 0) stats_line();   /* 1024, 2048, ... */
    spin_ticks = g_qpf.QuadPart * SH_BOOT_POLL_SPIN_US / 1000000;
    yield_ticks = g_qpf.QuadPart * SH_BOOT_POLL_YIELD_US / 1000000;
    QueryPerformanceCounter(&t0);
    /* The stream thread usually completes a read within microseconds once it
     * runs; a short spin catches that without giving up the core. */
    for (;;) {
        if (*done || *abort_flag) { InterlockedIncrement(&g_spun); return; }
        YieldProcessor();
        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart >= spin_ticks) break;
    }
    for (;;) {
        if (*done || *abort_flag) { InterlockedIncrement(&g_yielded); return; }
        if (!SwitchToThread()) Sleep(0);
        QueryPerformanceCounter(&now);
        if (now.QuadPart - t0.QuadPart >= yield_ticks) break;
    }
    /* A long read: back to the engine's own pacing. The caller re-tests. */
    InterlockedIncrement(&g_slept);
    Sleep(1);
}

void sh_boot_video_sleep(unsigned long ms)
{
    CRITICAL_SECTION *cs = NULL;
    InterlockedIncrement(&g_video_sleeps);
    if (g_video_lock) cs = (CRITICAL_SECTION *)*g_video_lock;
    /* Release only a lock this thread holds exactly once, so the draw's own
     * leave stays balanced and an outer holder keeps its lock. */
    if (cs && (DWORD)(ULONG_PTR)cs->OwningThread == GetCurrentThreadId() && cs->RecursionCount == 1) {
        InterlockedIncrement(&g_video_released);
        LeaveCriticalSection(cs);
        Sleep(ms);
        EnterCriticalSection(cs);
        return;
    }
    Sleep(ms);
}

void sh_boot_pacing_get_stats(sh_boot_pacing_stats *out)
{
    if (!out) return;
    out->reads = g_reads; out->spun = g_spun; out->yielded = g_yielded; out->slept = g_slept;
    out->video_sleeps = g_video_sleeps; out->video_released = g_video_released;
}

#ifdef SH_BOOT_PACING_PURE
static void stats_line(void) {}
#else
#include "patch.h"
#include <stdio.h>

void backend_log(const char *message);

#define PART_COUNT 2
static unsigned char *g_page;
static sh_patch_handle g_patches[PART_COUNT];
static unsigned g_live;

static void stats_line(void)
{
    sh_boot_pacing_stats s;
    char line[256];
    sh_boot_pacing_get_stats(&s);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "BOOT: %ld read waits (%ld in the spin, %ld while yielding, %ld fell back to Sleep(1)); "
        "%ld video sleeps, %ld with the Bink lock released; process age %lu ms",
        s.reads, s.spun, s.yielded, s.slept, s.video_sleeps, s.video_released,
        sh_boot_pacing_process_age_ms());
    backend_log(line);
}

unsigned long sh_boot_pacing_process_age_ms(void)
{
    FILETIME created, exited, kernel, user, now;
    ULARGE_INTEGER a, b;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (unsigned long)((b.QuadPart - a.QuadPart) / 10000u) : 0;
}

static const char *const k_names[SH_BOOT_PACING_SIGS] = {"BootVideoLock", "BootVideoSleep", "BootReadPoll"};

size_t sh_boot_pacing_match(const uint8_t *base, sig_result *results, size_t cap)
{
    size_t i, j, ok = 0;
    for (i = 0; i < SH_BOOT_PACING_SIGS && i < cap; i++) {
        results[i].name = k_names[i]; results[i].status = SIG_NOT_FOUND;
        results[i].addr = 0; results[i].rva = 0;
        for (j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
            if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, k_names[i])) {
                if (sig_match_recorded(base, &BACKEND_ENGINE_SIGNATURES[j], &results[i]) == SIG_OK) ok++;
                break;
            }
    }
    return ok;
}

static const sig_result *binding(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return r + i;
    return NULL;
}

/* One page within rel32 reach of every site in sites[]. */
static unsigned char *near_page(const uintptr_t *sites, size_t n)
{
    SYSTEM_INFO info;
    uintptr_t step, center, delta;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    center = sites[0] & ~(step - 1);
    for (delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[2] = {center >= delta ? center - delta : 0, center + delta};
        int c;
        for (c = 0; c < 2; c++) {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t a = candidates[c];
            int32_t disp;
            size_t i;
            int reach = 1;
            void *page;
            if (a < (uintptr_t)info.lpMinimumApplicationAddress || a > (uintptr_t)info.lpMaximumApplicationAddress) continue;
            for (i = 0; i < n; i++)
                if (!sh_boot_rel32(sites[i], a, &disp) || !sh_boot_rel32(sites[i], a + 0x1000, &disp)) reach = 0;
            if (!reach) continue;
            if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            page = VirtualAlloc((void *)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (page) return (unsigned char *)page;
        }
    }
    return NULL;
}

/* Repoint the 4-byte operand at `at` (the instruction ends at next) to dest. */
static int repoint(uintptr_t at, uintptr_t next, uintptr_t dest, sh_patch_handle *h)
{
    int32_t old_disp, new_disp;
    memcpy(&old_disp, (const void *)at, 4);
    if (!sh_boot_rel32(next, dest, &new_disp)) return 0;
    return code_patch((void *)at, (const uint8_t *)&old_disp, (const uint8_t *)&new_disp, 4, h) == B2_PATCH_OK;
}

/* Page layout: the poll stub, then the video stub. */
#define PAGE_POLL  0x00
#define PAGE_VIDEO 0x40
#define PAGE_END   0x80

unsigned sh_boot_pacing_install(const sig_result *results, size_t count, unsigned mask, unsigned long age_ms)
{
    const sig_result *lock_r = binding(results, count, "BootVideoLock"),
        *sleep_r = binding(results, count, "BootVideoSleep"),
        *poll_r = binding(results, count, "BootReadPoll");
    uintptr_t lock = 0, handle = 0, first = 0, second = 0, video_sleep = 0, poll_sleep = 0;
    uintptr_t sites[2];
    size_t nsites = 0;
    const char *video_why = NULL, *poll_why = NULL;
    unsigned want = 0;
    DWORD old;
    char line[512];

    if (g_live) return g_live;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);

    if (!(mask & SH_BOOT_PACING_VIDEO_LOCK)) video_why = "off";
    else if (!lock_r || !sleep_r) video_why = "the load-screen video draw was not found";
    else if (!sh_boot_video_lock_site((const unsigned char *)lock_r->addr, &lock, &handle) ||
             !sh_boot_video_sleep_site((const unsigned char *)sleep_r->addr, &first, &second, &video_sleep))
        video_why = "the load-screen video draw has unexpected bytes";
    else if (sleep_r->addr <= lock_r->addr || sleep_r->addr - lock_r->addr > 0x200 || handle != first)
        video_why = "the video lock and sleep sites are not one draw";
    else if (!sh_boot_sleep_thunk((const unsigned char *)video_sleep))
        video_why = "the draw's sleep is not the engine's Sleep";
    else { want |= SH_BOOT_PACING_VIDEO_LOCK; sites[nsites++] = sleep_r->addr; }

    if (!(mask & SH_BOOT_PACING_READ_POLL)) poll_why = "off";
    else if (!poll_r) poll_why = "the read completion poll was not found";
    else if (!sh_boot_read_poll_site((const unsigned char *)poll_r->addr, &poll_sleep))
        poll_why = "the read completion poll has unexpected bytes";
    else if (!sh_boot_sleep_thunk((const unsigned char *)poll_sleep))
        poll_why = "the poll's sleep is not the engine's Sleep";
    else { want |= SH_BOOT_PACING_READ_POLL; sites[nsites++] = poll_r->addr; }

    if (want && age_ms > SH_BOOT_PACING_LATE_MS) {
        video_why = (want & SH_BOOT_PACING_VIDEO_LOCK) ? "installed too late to patch safely" : video_why;
        poll_why = (want & SH_BOOT_PACING_READ_POLL) ? "installed too late to patch safely" : poll_why;
        want = 0;
    }
    if (want && !g_page && !(g_page = near_page(sites, nsites))) {
        video_why = (want & SH_BOOT_PACING_VIDEO_LOCK) ? "no page within reach of the engine" : video_why;
        poll_why = (want & SH_BOOT_PACING_READ_POLL) ? "no page within reach of the engine" : poll_why;
        want = 0;
    }
    if (want) {
        /* Fill the page completely, then make it read-only and executable
         * before any operand points at it. */
        sh_boot_poll_stub(g_page + PAGE_POLL, PAGE_VIDEO - PAGE_POLL, (uintptr_t)sh_boot_read_wait);
        sh_boot_jump_stub(g_page + PAGE_VIDEO, PAGE_END - PAGE_VIDEO, (uintptr_t)sh_boot_video_sleep);
        g_video_lock = (void *volatile *)lock;
        if (!VirtualProtect(g_page, 4096, PAGE_EXECUTE_READ, &old)) {
            video_why = poll_why = "the stub page could not be made executable";
            want = 0;
        } else FlushInstructionCache(GetCurrentProcess(), g_page, 4096);
    }
    if (want & SH_BOOT_PACING_VIDEO_LOCK) {
        uintptr_t call = sleep_r->addr + SH_BOOT_SLEEP_CALL;
        if (repoint(call + 1, call + 5, (uintptr_t)(g_page + PAGE_VIDEO), &g_patches[0])) g_live |= SH_BOOT_PACING_VIDEO_LOCK;
        else video_why = "the draw's sleep could not be patched";
    }
    if (want & SH_BOOT_PACING_READ_POLL) {
        uintptr_t call = poll_r->addr + SH_BOOT_POLL_CALL;
        if (repoint(call + 1, call + 5, (uintptr_t)(g_page + PAGE_POLL), &g_patches[1])) g_live |= SH_BOOT_PACING_READ_POLL;
        else poll_why = "the read poll could not be patched";
    }
    if (!g_live && g_page) { VirtualFree(g_page, 0, MEM_RELEASE); g_page = NULL; g_video_lock = NULL; }

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "BOOT: pacing at process age %lu ms: video lock %s%s%s; read poll %s%s%s",
        age_ms,
        (g_live & SH_BOOT_PACING_VIDEO_LOCK) ? "released around the draw's Sleep(10)" : "unchanged",
        video_why ? " -- " : "", video_why ? video_why : "",
        (g_live & SH_BOOT_PACING_READ_POLL) ? "spins and yields before Sleep(1)" : "unchanged",
        poll_why ? " -- " : "", poll_why ? poll_why : "");
    backend_log(line);
    return g_live;
}

unsigned sh_boot_pacing_live(void) { return g_live; }
#endif
