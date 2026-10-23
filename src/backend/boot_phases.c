/* Boot milestones and the first registration's placement (R-400). See boot_phases.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "boot_phases.h"

static volatile LONG64 g_at[SH_BOOT_MARK_COUNT];   /* ms after process creation; 0: not reached */
static volatile LONG g_placement = SH_BOOT_PLACE_PROMOTION;
static volatile LONG g_menu_reported, g_boundary_reported;
static SRWLOCK g_text_lock = SRWLOCK_INIT;
static char g_library[160], g_worker[512];
#ifdef SH_BOOT_PHASES_TESTING
static char g_last_line[2048];
#endif

sh_boot_placement sh_boot_placement_decide(int packages_enabled, int register_after_menu)
{
    if (!packages_enabled) return SH_BOOT_PLACE_NONE;
    return register_after_menu ? SH_BOOT_PLACE_FIRST_BOUNDARY : SH_BOOT_PLACE_PROMOTION;
}

const char *sh_boot_placement_name(sh_boot_placement placement)
{
    switch (placement) {
    case SH_BOOT_PLACE_PROMOTION: return "inside the engine's boot promotion";
    case SH_BOOT_PLACE_FIRST_BOUNDARY: return "after the menu, at the first SnapMap browser";
    case SH_BOOT_PLACE_NONE: return "none (user packages are off)";
    }
    return "unknown";
}

/* Milliseconds since this process was created, from the system clock both
 * times are read from. */
static uint64_t bp_now_ms(void)
{
    FILETIME created, exited, kernel, user, now;
    ULARGE_INTEGER c, n;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return GetTickCount64();
    GetSystemTimeAsFileTime(&now);
    c.LowPart = created.dwLowDateTime; c.HighPart = created.dwHighDateTime;
    n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
    if (n.QuadPart <= c.QuadPart) return 1;
    uint64_t ms = (n.QuadPart - c.QuadPart) / 10000u;
    return ms ? ms : 1;
}

void sh_boot_phases_mark(sh_boot_mark mark)
{
    if ((unsigned)mark >= SH_BOOT_MARK_COUNT) return;
    InterlockedCompareExchange64(&g_at[mark], (LONG64)bp_now_ms(), 0);
}

void sh_boot_phases_set_placement(sh_boot_placement placement) { InterlockedExchange(&g_placement, (LONG)placement); }
sh_boot_placement sh_boot_phases_placement(void) { return (sh_boot_placement)InterlockedCompareExchange(&g_placement, 0, 0); }

static void bp_set_text(char *slot, size_t capacity, const char *text)
{
    AcquireSRWLockExclusive(&g_text_lock);
    snprintf(slot, capacity, "%s", text ? text : "");
    ReleaseSRWLockExclusive(&g_text_lock);
}
void sh_boot_phases_set_library(const char *route) { bp_set_text(g_library, sizeof(g_library), route); }
void sh_boot_phases_set_worker(const char *summary) { bp_set_text(g_worker, sizeof(g_worker), summary); }

/* "<label> <seconds>" for a reached mark, "<label> -" otherwise. */
static size_t bp_put(char *out, size_t capacity, size_t used, const char *text)
{
    if (used >= capacity) return used;
    int n = snprintf(out + used, capacity - used, "%s", text);
    if (n < 0) return used;
    return used + (size_t)n < capacity ? used + (size_t)n : capacity - 1;
}
static void bp_seconds(char *out, size_t capacity, uint64_t ms)
{
    snprintf(out, capacity, "%llu.%01llu s", (unsigned long long)(ms / 1000u), (unsigned long long)(ms % 1000u / 100u));
}
static void bp_span(char *out, size_t capacity, uint64_t from, uint64_t to)
{
    if (!from || !to || to < from) snprintf(out, capacity, "-");
    else bp_seconds(out, capacity, to - from);
}

size_t sh_boot_phases_format(char *out, size_t capacity, const uint64_t at[SH_BOOT_MARK_COUNT],
    sh_boot_placement placement, const char *library, const char *worker)
{
    char a[32], b[32], c[32], d[32], e[32], f[32], g[32], h[32], piece[1600];
    size_t used = 0;
    if (!out || !capacity) return 0;
    out[0] = 0;
    bp_seconds(a, sizeof(a), at[SH_BOOT_ATTACH]);
    bp_span(b, sizeof(b), at[SH_BOOT_ATTACH], at[SH_BOOT_SIGNATURES]);
    bp_span(c, sizeof(c), at[SH_BOOT_SIGNATURES], at[SH_BOOT_HOOKS]);
    bp_span(d, sizeof(d), at[SH_BOOT_HOOKS], at[SH_BOOT_PROMOTION]);
    if (at[SH_BOOT_MENU]) bp_seconds(h, sizeof(h), at[SH_BOOT_MENU]); else snprintf(h, sizeof(h), "-");
    snprintf(piece, sizeof(piece), "MPKG: boot phases: attached %s after process start; engine code ready %s later, "
        "hooks %s, engine init to the boot promotion %s; ", a, b, c, d);
    used = bp_put(out, capacity, used, piece);
    if (placement == SH_BOOT_PLACE_PROMOTION) {
        bp_span(e, sizeof(e), at[SH_BOOT_PROMOTION], at[SH_BOOT_LIBRARY]);
        bp_span(f, sizeof(f), at[SH_BOOT_LIBRARY], at[SH_BOOT_REGISTERED]);
        bp_span(g, sizeof(g), at[SH_BOOT_PROMOTION], at[SH_BOOT_PROMOTION_RETURNED]);
        snprintf(piece, sizeof(piece), "registration %s: library %s (%s), registration %s, %s held in the promotion "
            "(the engine's boot counter waits at its last value meanwhile); ",
            sh_boot_placement_name(placement), e, library && *library ? library : "not prepared", f, g);
    } else {
        bp_span(g, sizeof(g), at[SH_BOOT_PROMOTION], at[SH_BOOT_PROMOTION_RETURNED]);
        snprintf(piece, sizeof(piece), "registration %s: %s held in the promotion; ", sh_boot_placement_name(placement), g);
    }
    used = bp_put(out, capacity, used, piece);
    bp_span(e, sizeof(e), at[SH_BOOT_PROMOTION_RETURNED], at[SH_BOOT_MENU]);
    snprintf(piece, sizeof(piece), "promotion return to menu %s; menu %s after process start", e, h);
    used = bp_put(out, capacity, used, piece);
    if (worker && *worker) {
        snprintf(piece, sizeof(piece), "; boot worker: %s", worker);
        used = bp_put(out, capacity, used, piece);
    }
    return used;
}

static void bp_snapshot(uint64_t at[SH_BOOT_MARK_COUNT])
{
    for (int i = 0; i < SH_BOOT_MARK_COUNT; i++) at[i] = (uint64_t)InterlockedCompareExchange64(&g_at[i], 0, 0);
}

static void bp_log(const char *line)
{
#ifdef SH_BOOT_PHASES_TESTING
    snprintf(g_last_line, sizeof(g_last_line), "%s", line);
#endif
    backend_log(line);
}

void sh_boot_phases_report_menu(void)
{
    uint64_t at[SH_BOOT_MARK_COUNT];
    char line[2048], library[160], worker[512];
    if (InterlockedCompareExchange(&g_menu_reported, 1, 0)) return;
    sh_boot_phases_mark(SH_BOOT_MENU);
    bp_snapshot(at);
    AcquireSRWLockShared(&g_text_lock);
    snprintf(library, sizeof(library), "%s", g_library); snprintf(worker, sizeof(worker), "%s", g_worker);
    ReleaseSRWLockShared(&g_text_lock);
    sh_boot_phases_format(line, sizeof(line), at, (sh_boot_placement)InterlockedCompareExchange(&g_placement, 0, 0),
        library, worker);
    bp_log(line);
}

void sh_boot_phases_report_first_boundary(unsigned long long registration_ms)
{
    uint64_t at[SH_BOOT_MARK_COUNT];
    char line[512], after[32], library[160];
    if (InterlockedCompareExchange(&g_placement, 0, 0) != SH_BOOT_PLACE_FIRST_BOUNDARY ||
        InterlockedCompareExchange(&g_boundary_reported, 1, 0)) return;
    sh_boot_phases_mark(SH_BOOT_FIRST_BOUNDARY);
    bp_snapshot(at);
    bp_span(after, sizeof(after), at[SH_BOOT_MENU], at[SH_BOOT_FIRST_BOUNDARY]);
    AcquireSRWLockShared(&g_text_lock);
    snprintf(library, sizeof(library), "%s", g_library);
    ReleaseSRWLockShared(&g_text_lock);
    snprintf(line, sizeof(line), "MPKG: boot phases: the deferred first registration completed at the first SnapMap "
        "browser %s after the menu (library %s); the pass took %llu ms", after, library[0] ? library : "prepared",
        registration_ms);
    bp_log(line);
}

#ifdef SH_BOOT_PHASES_TESTING
void sh_boot_phases_test_reset(void)
{
    for (int i = 0; i < SH_BOOT_MARK_COUNT; i++) InterlockedExchange64(&g_at[i], 0);
    InterlockedExchange(&g_placement, SH_BOOT_PLACE_PROMOTION);
    InterlockedExchange(&g_menu_reported, 0); InterlockedExchange(&g_boundary_reported, 0);
    g_library[0] = g_worker[0] = g_last_line[0] = 0;
}
void sh_boot_phases_test_set(sh_boot_mark mark, uint64_t at_ms)
{
    if ((unsigned)mark < SH_BOOT_MARK_COUNT) InterlockedExchange64(&g_at[mark], (LONG64)at_ms);
}
const char *sh_boot_phases_test_last_line(void) { return g_last_line; }
#endif
