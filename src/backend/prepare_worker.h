/* prepare_worker.h -- a map open's preparation on a worker thread (R-398, perf16).
 *
 * The engine thread starts one preparation at a time and returns to its frame loop; the worker runs it. Work that
 * only the engine thread may do (a native call, a guarded native read) is handed back through a one-slot channel
 * the engine thread serves at its poll, a few milliseconds per frame, so the frame keeps running. Every engine
 * memory read the worker makes itself is journaled with its bytes; when the worker is done, the engine thread
 * reads every journaled range again: equal bytes everywhere mean the preparation read exactly what the engine
 * holds at the continuation, so its result stands; anything else redoes the preparation on the engine thread.
 *
 * Header-only (one shared state across translation units through selectany), so modules and tests that touch it
 * need no new source. All functions are cheap no-ops off a preparation. */
#ifndef BACKEND_PREPARE_WORKER_H
#define BACKEND_PREPARE_WORKER_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef intptr_t (*sh_prepare_engine_fn)(void *context);
typedef void (*sh_prepare_worker_fn)(void *context);

typedef struct sh_prepare_read { uintptr_t address; uint32_t length; uint32_t ok; size_t offset; } sh_prepare_read;
typedef struct sh_prepare_stats {
    unsigned long long worker_ms, service_ms, verify_ms;
    size_t service_calls, reads, distinct, read_bytes, unstable, mismatches;
    int verified; /* 1 every journaled range equal now, 0 a difference, -1 not checked */
    char refused[256];
} sh_prepare_stats;
typedef struct sh_prepare_worker_state {
    volatile LONG running, finished;
    volatile LONG serving;     /* the engine thread is running one of the worker's calls */
    volatile LONG locks_held;  /* exclusive runtime locks the worker holds now (package_runtime.c) */
    DWORD worker, engine;
    HANDLE thread, request, done;
    ULONGLONG started;
    sh_prepare_worker_fn fn; void *context;
    /* the one outstanding engine-thread call */
    sh_prepare_engine_fn call; void *call_context; intptr_t call_result;
    /* the memory read journal: entries, an open-addressed index over (address, length), their bytes */
    sh_prepare_read *reads; size_t read_count, read_capacity;
    size_t *index; size_t index_capacity;
    unsigned char *bytes; size_t byte_count, byte_capacity;
    size_t total_reads;
    sh_prepare_stats stats;
} sh_prepare_worker_state;
__declspec(selectany) sh_prepare_worker_state g_sh_prepare = {0};
/* Journal limit: a preparation reading more than this gives up and is redone on the engine thread. */
#define SH_PREPARE_JOURNAL_LIMIT ((size_t)768u << 20)

static __inline int sh_prepare_on_worker(void)
{ return g_sh_prepare.running && g_sh_prepare.worker == GetCurrentThreadId(); }
static __inline int sh_prepare_running(void) { return g_sh_prepare.running != 0; }
/* The engine thread is running a call on the worker's behalf: thread-keyed state the worker holds is its own. */
static __inline int sh_prepare_serving(void)
{ return g_sh_prepare.serving && g_sh_prepare.running && g_sh_prepare.engine == GetCurrentThreadId(); }
static __inline int sh_prepare_finished(void) { return g_sh_prepare.finished != 0; }

/* Mark that the worker reached a path only the engine thread may take: its result will not be used. */
static __inline void sh_prepare_refuse(const char *why)
{
    if (sh_prepare_on_worker() && !g_sh_prepare.stats.refused[0])
        _snprintf_s(g_sh_prepare.stats.refused, sizeof(g_sh_prepare.stats.refused), _TRUNCATE, "%s", why ? why : "refused");
}

static __inline size_t sh_prepare_hash(uintptr_t address, uint32_t length)
{ uint64_t h = (uint64_t)address * 0x9e3779b97f4a7c15ull ^ (uint64_t)length * 0xc2b2ae3d27d4eb4full; return (size_t)(h ^ (h >> 29)); }
static __inline int sh_prepare_grow_index(void)
{
    size_t capacity = g_sh_prepare.index_capacity ? g_sh_prepare.index_capacity * 2 : 1u << 16;
    size_t *index = (size_t *)malloc(capacity * sizeof(*index));
    if (!index) return 0;
    for (size_t i = 0; i < capacity; i++) index[i] = SIZE_MAX;
    for (size_t r = 0; r < g_sh_prepare.read_count; r++) {
        size_t at = sh_prepare_hash(g_sh_prepare.reads[r].address, g_sh_prepare.reads[r].length) & (capacity - 1);
        while (index[at] != SIZE_MAX) at = (at + 1) & (capacity - 1);
        index[at] = r;
    }
    free(g_sh_prepare.index); g_sh_prepare.index = index; g_sh_prepare.index_capacity = capacity;
    return 1;
}
/* One read the worker made of engine memory: its bytes (ok) or its fault. Readers call it after reading. */
static __inline void sh_prepare_read_note(uintptr_t address, const void *bytes, size_t length, int ok)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    if (!sh_prepare_on_worker() || g->stats.refused[0]) return;
    g->total_reads++;
    if (length > 0xffffffffu) { sh_prepare_refuse("a journaled read exceeds 4 GiB"); return; }
    if ((g->read_count + 1) * 2 > g->index_capacity && !sh_prepare_grow_index()) { sh_prepare_refuse("the read journal cannot grow"); return; }
    size_t at = sh_prepare_hash(address, (uint32_t)length) & (g->index_capacity - 1);
    while (g->index[at] != SIZE_MAX) {
        const sh_prepare_read *r = g->reads + g->index[at];
        if (r->address == address && r->length == (uint32_t)length) {
            /* The same range read again during the preparation must read the same: the engine changed it meanwhile. */
            if ((int)r->ok != (ok != 0) || (ok && memcmp(g->bytes + r->offset, bytes, length))) g->stats.unstable++;
            return;
        }
        at = (at + 1) & (g->index_capacity - 1);
    }
    if (g->read_count == g->read_capacity) {
        size_t capacity = g->read_capacity ? g->read_capacity * 2 : 4096;
        sh_prepare_read *reads = (sh_prepare_read *)realloc(g->reads, capacity * sizeof(*reads));
        if (!reads) { sh_prepare_refuse("the read journal cannot grow"); return; }
        g->reads = reads; g->read_capacity = capacity;
    }
    size_t need = ok ? length : 0;
    if (g->byte_count + need > SH_PREPARE_JOURNAL_LIMIT) { sh_prepare_refuse("the read journal exceeds its limit"); return; }
    if (g->byte_count + need > g->byte_capacity) {
        size_t capacity = g->byte_capacity ? g->byte_capacity : 1u << 20;
        while (capacity < g->byte_count + need) capacity *= 2;
        unsigned char *grown = (unsigned char *)realloc(g->bytes, capacity);
        if (!grown) { sh_prepare_refuse("the read journal cannot grow"); return; }
        g->bytes = grown; g->byte_capacity = capacity;
    }
    sh_prepare_read *r = g->reads + g->read_count;
    r->address = address; r->length = (uint32_t)length; r->ok = ok != 0; r->offset = g->byte_count;
    if (need) memcpy(g->bytes + g->byte_count, bytes, need);
    g->byte_count += need;
    g->index[at] = g->read_count++;
}

/* Run fn on the engine thread: directly there, through the channel from the worker (which waits). */
static __inline intptr_t sh_prepare_engine(sh_prepare_engine_fn fn, void *context)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    if (!sh_prepare_on_worker()) return fn(context);
    /* The engine thread would wait on a lock the worker holds exclusively while it runs the call: never hand it. */
    if (g->locks_held) { sh_prepare_refuse("an engine-thread call while the runtime lock is held"); return 0; }
    g->call = fn; g->call_context = context;
    SignalObjectAndWait(g->request, g->done, INFINITE, FALSE);
    return g->call_result;
}

static DWORD WINAPI sh_prepare_thread(void *unused)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    ULONGLONG began = GetTickCount64();
    (void)unused;
    __try { g->fn(g->context); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(g->stats.refused, sizeof(g->stats.refused), _TRUNCATE, "the preparation raised exception %08lx",
            (unsigned long)GetExceptionCode());
    }
    g->stats.worker_ms = GetTickCount64() - began;
    InterlockedExchange(&g->finished, 1);
    return 0;
}
/* Engine thread: start fn(context) on the worker. 0 when one runs already or no thread could start. */
static __inline int sh_prepare_start(sh_prepare_worker_fn fn, void *context)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    if (g->running) return 0;
    free(g->reads); free(g->index); free(g->bytes);
    memset(g, 0, sizeof(*g));
    g->stats.verified = -1;
    g->fn = fn; g->context = context; g->engine = GetCurrentThreadId(); g->started = GetTickCount64();
    g->request = CreateEventW(NULL, FALSE, FALSE, NULL); g->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g->request || !g->done) goto failed;
    g->running = 1;
    g->thread = CreateThread(NULL, (SIZE_T)64u << 20, sh_prepare_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, &g->worker);
    if (!g->thread) { g->running = 0; goto failed; }
    ResumeThread(g->thread);
    return 1;
failed:
    if (g->request) CloseHandle(g->request);
    if (g->done) CloseHandle(g->done);
    g->request = g->done = NULL;
    return 0;
}
/* Engine thread: serve the worker's calls for up to budget_ms, returning when it finished or the budget ran out. */
static __inline void sh_prepare_service(DWORD budget_ms)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    ULONGLONG began = GetTickCount64(), end = began + budget_ms;
    if (!g->running || GetCurrentThreadId() != g->engine) return;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        HANDLE handles[2] = {g->request, g->thread};
        DWORD wait = now >= end ? 0 : (DWORD)(end - now);
        DWORD woke = WaitForMultipleObjects(2, handles, FALSE, wait);
        if (woke == WAIT_OBJECT_0) {
            ULONGLONG call_began = GetTickCount64();
            InterlockedExchange(&g->serving, 1);
            __try { g->call_result = g->call(g->call_context); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g->call_result = 0;
                _snprintf_s(g->stats.refused, sizeof(g->stats.refused), _TRUNCATE,
                    "an engine-thread call raised exception %08lx", (unsigned long)GetExceptionCode());
            }
            InterlockedExchange(&g->serving, 0);
            g->stats.service_calls++; g->stats.service_ms += GetTickCount64() - call_began;
            SetEvent(g->done);
            continue;
        }
        break; /* finished, or the budget is spent */
    }
}
/* Engine thread: read every journaled range again. 1 all equal, 0 any difference (counted). */
static __inline int sh_prepare_verify(void)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    ULONGLONG began = GetTickCount64();
    unsigned char *buffer = NULL; size_t buffer_capacity = 0;
    size_t mismatches = 0;
    for (size_t i = 0; i < g->read_count; i++) {
        const sh_prepare_read *r = g->reads + i;
        int ok = 0;
        if (r->length > buffer_capacity) {
            unsigned char *grown = (unsigned char *)realloc(buffer, r->length);
            if (!grown) { mismatches++; break; }
            buffer = grown; buffer_capacity = r->length;
        }
        __try { if (r->length) memcpy(buffer, (const void *)r->address, r->length); ok = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (ok != (int)r->ok || (ok && r->length && memcmp(buffer, g->bytes + r->offset, r->length))) mismatches++;
    }
    free(buffer);
    g->stats.mismatches = mismatches; g->stats.verify_ms = GetTickCount64() - began;
    g->stats.verified = !mismatches && !g->stats.unstable;
    return g->stats.verified;
}
/* Engine thread: wait for the worker (serving it meanwhile), verify its reads, release the thread. stats may be
 * NULL. Returns the verification (1 equal, 0 changed, -1 no worker). */
static __inline int sh_prepare_join(sh_prepare_stats *stats)
{
    sh_prepare_worker_state *g = &g_sh_prepare;
    int verified = -1;
    if (!g->running) return -1;
    while (!g->finished) sh_prepare_service(50);
    WaitForSingleObject(g->thread, INFINITE);
    CloseHandle(g->thread); CloseHandle(g->request); CloseHandle(g->done);
    g->thread = g->request = g->done = NULL;
    g->stats.reads = g->total_reads; g->stats.distinct = g->read_count; g->stats.read_bytes = g->byte_count;
    verified = sh_prepare_verify();
    g->running = 0;
    if (stats) *stats = g->stats;
    free(g->reads); free(g->index); free(g->bytes);
    g->reads = NULL; g->index = NULL; g->bytes = NULL; g->read_count = g->read_capacity = g->index_capacity = 0;
    g->byte_count = g->byte_capacity = 0;
    return verified;
}
#endif
