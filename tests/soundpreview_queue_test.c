/* soundpreview_queue_test.c -- exact rollback and ordering for the main-thread audio queue. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/backend/soundpreview.c"

static int failures;
static int kick_count;
static int overflow_logs;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg)
{
    if (msg && strstr(msg, "queue full")) overflow_logs++;
}

int sh_imgpreview_has(int kind, const char *name)
{
    (void)kind;
    return name && name[0];
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    (void)results; (void)n; (void)name;
    return 0;
}

static void buffer_ok(void *cmdsys, const char *text)
{
    (void)cmdsys;
    CHECK(strcmp(text, SP_CMD_NAME "\n") == 0);
    kick_count++;
}

static void buffer_fault(void *cmdsys, const char *text)
{
    (void)cmdsys; (void)text;
    kick_count++;
    RaiseException(0xE0000001u, 0, 0, NULL);
}

static const sp_item *queue_at(int logical_index)
{
    return &g_queue[(g_qhead + logical_index) % SP_QUEUE_MAX];
}

static void reset_queue(void)
{
    EnterCriticalSection(&g_qlock);
    memset(g_queue, 0, sizeof g_queue);
    g_qhead = 0;
    g_qcount = 0;
    LeaveCriticalSection(&g_qlock);
    kick_count = 0;
    overflow_logs = 0;
    g_cmdsys = (void *)1;
    g_cmd_registered = 1;
}

int main(void)
{
    InitializeCriticalSection(&g_qlock);
    g_qlock_init = 1;

    reset_queue();
    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_PLAY, "failed") == 0);
    CHECK(g_qcount == 0);

    g_buffer_cmd = buffer_ok;
    CHECK(sp_post(SP_OP_PLAY, "next") == 1);
    CHECK(g_qcount == 1);
    CHECK(strcmp(queue_at(0)->name, "next") == 0);

    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_STOP, NULL) == 0);
    CHECK(g_qcount == 1);
    CHECK(queue_at(0)->op == SP_OP_PLAY);
    CHECK(strcmp(queue_at(0)->name, "next") == 0);

    reset_queue();
    g_buffer_cmd = buffer_ok;
    for (int i = 0; i < SP_QUEUE_MAX; ++i) {
        char name[32];
        _snprintf_s(name, sizeof name, _TRUNCATE, "sound-%d", i);
        CHECK(sp_post(SP_OP_PLAY, name) == 1);
    }
    CHECK(g_qcount == SP_QUEUE_MAX);
    g_buffer_cmd = buffer_fault;
    CHECK(sp_post(SP_OP_PLAY, "refused-overflow") == 0);
    CHECK(g_qcount == SP_QUEUE_MAX);
    CHECK(strcmp(queue_at(0)->name, "sound-0") == 0);
    CHECK(strcmp(queue_at(SP_QUEUE_MAX - 1)->name, "sound-7") == 0);
    CHECK(overflow_logs == 0);

    g_buffer_cmd = buffer_ok;
    CHECK(sp_post(SP_OP_PLAY, "accepted-overflow") == 1);
    CHECK(g_qcount == SP_QUEUE_MAX);
    CHECK(strcmp(queue_at(0)->name, "sound-1") == 0);
    CHECK(strcmp(queue_at(SP_QUEUE_MAX - 1)->name, "accepted-overflow") == 0);
    CHECK(overflow_logs == 1);

    {
        char too_long[SP_NAME_CAP + 1];
        memset(too_long, 'x', sizeof too_long - 1);
        too_long[sizeof too_long - 1] = '\0';
        int before_count = g_qcount;
        int before_kicks = kick_count;
        CHECK(sp_post(SP_OP_PLAY, too_long) == 0);
        CHECK(g_qcount == before_count);
        CHECK(kick_count == before_kicks);
    }

    DeleteCriticalSection(&g_qlock);
    g_qlock_init = 0;

    if (failures) {
        fprintf(stderr, "%d sound queue test(s) failed\n", failures);
        return 1;
    }
    puts("sound preview queue tests passed");
    return 0;
}
