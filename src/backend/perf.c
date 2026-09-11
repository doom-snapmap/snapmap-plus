/* Wall-clock counters for the hot paths. See perf.h. */
#include "perf.h"

#include <stdio.h>

typedef struct {
    const char      *label;
    volatile LONG64  calls;
    volatile LONG64  ticks;
    volatile LONG64  worst;   /* the single slowest call */
} perf_counter;

static perf_counter g_perf[SH_PERF_SLOTS] = {
    { "resource open (all)" },
    { "override file checks" },
    { "engine's own open" },
    { "installed resources" },
    { "published decls" },
    { "map -> JSON (engine)" },
    { "navigation parse" },
    { "green preview build" },
    { "session log line" },
};

static LONGLONG g_freq;

LONGLONG sh_perf_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void sh_perf_add(sh_perf_slot slot, LONGLONG ticks)
{
    perf_counter *c;
    if ((unsigned)slot >= SH_PERF_SLOTS || ticks < 0) return;
    c = &g_perf[slot];
    InterlockedIncrement64(&c->calls);
    InterlockedAdd64(&c->ticks, ticks);
    /* Racing writers can lose a sample here; the worst call is a guide, not a
     * ledger, and a lock on this path would cost more than the reading is worth. */
    if (ticks > c->worst) c->worst = ticks;
}

void sh_perf_reset(void)
{
    int i;
    for (i = 0; i < SH_PERF_SLOTS; i++) {
        InterlockedExchange64(&g_perf[i].calls, 0);
        InterlockedExchange64(&g_perf[i].ticks, 0);
        InterlockedExchange64(&g_perf[i].worst, 0);
    }
}

void sh_perf_report(void (*out)(const char *fmt, ...))
{
    int i;
    if (!out) return;
    if (g_freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    if (g_freq == 0) { out("timing: this machine has no performance counter.\n"); return; }

    out("time spent inside snapmap-plus, since DOOM started or since 'sh_perf reset':\n");
    for (i = 0; i < SH_PERF_SLOTS; i++) {
        LONG64 calls = InterlockedCompareExchange64(&g_perf[i].calls, 0, 0);
        LONG64 ticks = InterlockedCompareExchange64(&g_perf[i].ticks, 0, 0);
        LONG64 worst = InterlockedCompareExchange64(&g_perf[i].worst, 0, 0);
        double total_ms = (double)ticks * 1000.0 / (double)g_freq;
        if (calls == 0) { out("  %-22s never ran\n", g_perf[i].label); continue; }
        out("  %-22s %lld call(s), %.1f ms total, %.3f ms each, worst %.1f ms\n",
            g_perf[i].label, calls, total_ms, total_ms / (double)calls,
            (double)worst * 1000.0 / (double)g_freq);
    }
}
