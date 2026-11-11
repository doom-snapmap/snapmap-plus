/* script_queue.c -- see script_queue.h. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "script_queue.h"

#define FIRST_RING 1024

typedef struct queue {
    volatile LONG lock;       /* 0 free, 1 held */
    volatile LONG open;
    sh_script_event *ring;    /* cap records; grows, kept for the session */
    uint32_t cap;
    uint32_t head, count;     /* oldest record, records held */
    uint32_t seq;
    volatile LONG dropped;
    int reported;             /* a nonzero drop count was already taken this map */
} queue;

/* One queue per mod slot, each allocated once and kept, so a pointer read
 * under g_dir stays valid; the directory grows with the mods. */
static queue  **g_q;
static int      g_nq, g_cq;
static SRWLOCK  g_dir = SRWLOCK_INIT;
static volatile LONG g_drain_thread;

static void q_lock(queue *q)
{
    while (InterlockedCompareExchange(&q->lock, 1, 0) != 0) YieldProcessor();
}

static void q_unlock(queue *q)
{
    InterlockedExchange(&q->lock, 0);
}

static queue *q_at(int mod)
{
    queue *q = NULL;
    AcquireSRWLockShared(&g_dir);
    if (mod >= 0 && mod < g_nq) q = g_q[mod];
    ReleaseSRWLockShared(&g_dir);
    return q;
}

/* The queue of slot `mod`, made (with the slots below it) when missing. */
static queue *q_make(int mod)
{
    queue *q = NULL;
    if (mod < 0) return NULL;
    AcquireSRWLockExclusive(&g_dir);
    if (mod >= g_cq) {
        int cap = g_cq ? g_cq : 16;
        queue **grown;
        while (cap <= mod) cap *= 2;
        grown = (queue **)realloc(g_q, (size_t)cap * sizeof *grown);
        if (grown) { g_q = grown; g_cq = cap; }
    }
    while (g_nq <= mod && g_nq < g_cq) {
        queue *made = (queue *)calloc(1, sizeof *made);
        if (!made) break;
        g_q[g_nq++] = made;
    }
    if (mod < g_nq) q = g_q[mod];
    ReleaseSRWLockExclusive(&g_dir);
    return q;
}

int sh_script_queue_open(int mod)
{
    queue *q = q_make(mod);
    sh_script_event *ring;
    if (!q) return 0;
    ring = q->ring;
    if (!ring) {
        ring = (sh_script_event *)calloc(FIRST_RING, sizeof *ring);
        if (!ring) return 0;
    }
    q_lock(q);
    if (!q->ring) q->cap = FIRST_RING;
    q->ring = ring;
    q->head = q->count = 0;
    q->seq = 0;
    q->dropped = 0;
    q->reported = 0;
    q->open = 1;
    q_unlock(q);
    return 1;
}

void sh_script_queue_close(int mod)
{
    queue *q = q_at(mod);
    if (!q) return;
    q_lock(q);
    q->open = 0;
    q->head = q->count = 0;
    q_unlock(q);
}

void sh_script_queue_close_all(void)
{
    int i, n;
    AcquireSRWLockShared(&g_dir);
    n = g_nq;
    ReleaseSRWLockShared(&g_dir);
    for (i = 0; i < n; i++) sh_script_queue_close(i);
}

/* Twice the room, the held records moved to the front in order. Lock held.
 * 0 when memory has none. */
static int grow_ring(queue *q)
{
    uint32_t cap = q->cap * 2, i;
    sh_script_event *ring;
    if (cap <= q->cap) return 0;
    ring = (sh_script_event *)malloc((size_t)cap * sizeof *ring);
    if (!ring) return 0;
    for (i = 0; i < q->count; i++) ring[i] = q->ring[(q->head + i) % q->cap];
    free(q->ring);
    q->ring = ring;
    q->cap = cap;
    q->head = 0;
    return 1;
}

int sh_script_queue_push(int mod, const sh_script_event *ev)
{
    queue *q = q_at(mod);
    int r;
    if (!q || !ev) return -1;
    q_lock(q);
    if (!q->open || !q->ring) {
        r = -1;
    } else if (q->count >= q->cap && !grow_ring(q)) {
        InterlockedIncrement(&q->dropped);
        r = 0;
    } else {
        sh_script_event *slot = &q->ring[(q->head + q->count) % q->cap];
        *slot = *ev;
        slot->text[SH_SCRIPT_EVENT_TEXT - 1] = 0;
        slot->text2[SH_SCRIPT_EVENT_TEXT - 1] = 0;
        slot->seq = ++q->seq;
        q->count++;
        r = 1;
    }
    q_unlock(q);
    return r;
}

static void copy_text(char *dst, const char *src)
{
    size_t n;
    if (!src) { dst[0] = 0; return; }
    n = strlen(src);
    if (n > SH_SCRIPT_EVENT_TEXT - 1) n = SH_SCRIPT_EVENT_TEXT - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int sh_script_queue_post(int mod, sh_script_event_kind kind, sh_script_handle self, sh_script_handle other,
                         sh_script_handle third, float amount, const char *text, const char *text2)
{
    sh_script_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = (uint16_t)kind;
    ev.self = self;
    ev.other = other;
    ev.third = third;
    ev.amount = amount;
    copy_text(ev.text, text);
    copy_text(ev.text2, text2);
    return sh_script_queue_push(mod, &ev);
}

int sh_script_queue_drain(int mod, sh_script_event *out, int cap)
{
    queue *q = q_at(mod);
    int n = 0;
    if (!q || !out || cap <= 0) return 0;
    if ((DWORD)g_drain_thread != GetCurrentThreadId()) return -1;
    q_lock(q);
    while (n < cap && q->count) {
        out[n++] = q->ring[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q_unlock(q);
    return n;
}

int sh_script_queue_pending(int mod)
{
    queue *q = q_at(mod);
    int n;
    if (!q) return 0;
    q_lock(q);
    n = (int)q->count;
    q_unlock(q);
    return n;
}

long sh_script_queue_take_dropped(int mod, int *first_time)
{
    queue *q = q_at(mod);
    long n;
    if (first_time) *first_time = 0;
    if (!q) return 0;
    n = InterlockedExchange(&q->dropped, 0);
    if (n && !q->reported) {
        q->reported = 1;
        if (first_time) *first_time = 1;
    }
    return n;
}

void sh_script_queue_set_drain_thread(uint32_t thread_id)
{
    InterlockedExchange(&g_drain_thread, (LONG)thread_id);
}
