/* prepare_worker_test.c -- the open's preparation worker (R-398 perf16): the engine-thread channel, the read
 * journal and its verification, refusals, and the guard against handing a call while a runtime lock is held. */
#include <stdio.h>
#include "../src/backend/prepare_worker.h"

static int g_failed, g_checks;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); } } while (0)

static DWORD g_engine;
static volatile LONG g_cell = 1, g_other = 5;
static int g_calls_seen;
static intptr_t on_engine(void *context)
{
    g_calls_seen += GetCurrentThreadId() == g_engine;
    return (intptr_t)context + 1;
}
/* Reads g_cell (journaled), asks the engine thread twice, reads g_cell again. */
static intptr_t g_answer;
static HANDLE g_gate;
static void work_reads(void *unused)
{
    LONG a = g_cell;
    (void)unused;
    sh_prepare_read_note((uintptr_t)&g_cell, &a, sizeof(a), 1);
    g_answer = sh_prepare_engine(on_engine, (void *)41) + sh_prepare_engine(on_engine, (void *)0);
    if (g_gate) WaitForSingleObject(g_gate, 10000);
    LONG b = g_cell;
    sh_prepare_read_note((uintptr_t)&g_cell, &b, sizeof(b), 1);
    LONG c = g_other;
    sh_prepare_read_note((uintptr_t)&g_other, &c, sizeof(c), 1);
}
static void work_refuses(void *unused) { (void)unused; sh_prepare_refuse("only the engine thread"); sh_prepare_refuse("second"); }
static void work_locked(void *unused)
{
    (void)unused;
    InterlockedIncrement(&g_sh_prepare.locks_held);
    g_answer = sh_prepare_engine(on_engine, (void *)1);
    InterlockedDecrement(&g_sh_prepare.locks_held);
}
static void work_raises(void *unused) { (void)unused; RaiseException(0xe0000001u, 0, 0, NULL); }

int main(void)
{
    sh_prepare_stats stats;
    g_engine = GetCurrentThreadId();
    /* Off a preparation everything runs where it is called. */
    CHECK(!sh_prepare_on_worker() && !sh_prepare_running());
    CHECK(sh_prepare_engine(on_engine, (void *)2) == 3 && g_calls_seen == 1);
    sh_prepare_read_note(1, "x", 1, 1); /* ignored off the worker */
    /* Equal reads, two engine-thread calls served at the poll. */
    g_calls_seen = 0; g_answer = 0;
    CHECK(sh_prepare_start(work_reads, NULL) && sh_prepare_running());
    CHECK(!sh_prepare_start(work_reads, NULL)); /* one at a time */
    for (int i = 0; i < 1000 && !sh_prepare_finished(); i++) sh_prepare_service(5);
    CHECK(sh_prepare_finished());
    CHECK(sh_prepare_join(&stats) == 1 && !sh_prepare_running());
    CHECK(g_answer == 43 && g_calls_seen == 2 && stats.service_calls == 2);
    CHECK(stats.reads == 3 && stats.distinct == 2 && stats.read_bytes == 2 * sizeof(LONG) && !stats.unstable);
    CHECK(stats.verified == 1 && !stats.mismatches && !stats.refused[0]);
    /* A range the engine changed during the preparation (read twice, unequal): unstable. */
    g_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(sh_prepare_start(work_reads, NULL));
    for (int i = 0; i < 200 && g_sh_prepare.stats.service_calls < 2; i++) sh_prepare_service(5);
    g_cell = 2; SetEvent(g_gate);
    CHECK(sh_prepare_join(&stats) == 0 && stats.unstable == 1 && stats.verified == 0);
    /* A range changed after the worker's last read: found by the continuation's verification. */
    ResetEvent(g_gate);
    CHECK(sh_prepare_start(work_reads, NULL));
    SetEvent(g_gate);
    for (int i = 0; i < 1000 && !sh_prepare_finished(); i++) sh_prepare_service(5);
    g_other = 6;
    CHECK(sh_prepare_join(&stats) == 0 && !stats.unstable && stats.mismatches == 1);
    CloseHandle(g_gate); g_gate = NULL;
    /* Refusal: the first reason stands. */
    CHECK(sh_prepare_start(work_refuses, NULL));
    sh_prepare_join(&stats);
    CHECK(!strcmp(stats.refused, "only the engine thread"));
    /* No engine-thread call is handed while the worker holds a runtime lock exclusively. */
    g_calls_seen = 0; g_answer = -1;
    CHECK(sh_prepare_start(work_locked, NULL));
    sh_prepare_join(&stats);
    CHECK(g_answer == 0 && !g_calls_seen && strstr(stats.refused, "runtime lock") != NULL);
    /* An exception on the worker is a refusal, and the join still completes. */
    CHECK(sh_prepare_start(work_raises, NULL));
    sh_prepare_join(&stats);
    CHECK(strstr(stats.refused, "raised exception e0000001") != NULL && !sh_prepare_running());
    printf("prepare worker: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
