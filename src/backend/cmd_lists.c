/* The engine's command lists: room reserved off the main thread, and the
 * backfill that never grows a list (cmd_lists.h says why). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "cmd_lists.h"

#define COUNT_SANITY 100000u
#define GROW_ROUNDS  40          /* doubling from 1 passes any sane count long before this */

static int list_read(const uint8_t *list, void ***array, uint32_t *count, uint32_t *cap)
{
    __try {
        *array = *(void ***)list;
        *count = *(const uint32_t *)(list + 8);
        *cap   = *(const uint32_t *)(list + 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return *count <= COUNT_SANITY && *cap <= COUNT_SANITY * 4u && *count <= *cap;
}

int sh_cmd_lists_room(const uint8_t *cmdsys, uint32_t extra)
{
    void **fa, **da;
    uint32_t fn, fc, dn, dc;
    if (!cmdsys) return 0;
    if (!list_read(cmdsys + SH_CMD_FULL_OFF, &fa, &fn, &fc)) return 0;
    if (!list_read(cmdsys + SH_CMD_DEV_OFF, &da, &dn, &dc)) return 0;
    /* the developer list ends up holding every command of the full list */
    return fn + extra <= fc && fn + extra <= dc;
}

typedef struct reserve_job {
    uint8_t *cmdsys;
    sh_idlist_grow_fn grow;
    uint32_t extra;
} reserve_job;

static void grow_until(uint8_t *list, sh_idlist_grow_fn grow, uint32_t want)
{
    int round;
    for (round = 0; round < GROW_ROUNDS; round++) {
        void **a;
        uint32_t n, cap, before;
        if (!list_read(list, &a, &n, &cap) || cap >= want) return;
        before = cap;
        __try { grow(list); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!list_read(list, &a, &n, &cap) || cap <= before) return;
    }
}

static DWORD WINAPI reserve_thread(LPVOID arg)
{
    reserve_job *job = (reserve_job *)arg;
    void **a;
    uint32_t n, cap;
    if (!list_read(job->cmdsys + SH_CMD_FULL_OFF, &a, &n, &cap)) return 0;
    grow_until(job->cmdsys + SH_CMD_FULL_OFF, job->grow, n + job->extra);
    if (!list_read(job->cmdsys + SH_CMD_FULL_OFF, &a, &n, &cap)) return 0;
    /* as much room as the full list has: the two then fill together, so the
     * developer list never needs to grow before the full list does */
    grow_until(job->cmdsys + SH_CMD_DEV_OFF, job->grow, cap > n + job->extra ? cap : n + job->extra);
    return 0;
}

int sh_cmd_lists_reserve(uint8_t *cmdsys, sh_idlist_grow_fn grow, uint32_t extra)
{
    reserve_job job;
    HANDLE thread;
    if (!cmdsys) return 0;
    if (sh_cmd_lists_room(cmdsys, extra)) {
        void **fa, **da;
        uint32_t fn, fc, dn, dc;
        if (list_read(cmdsys + SH_CMD_FULL_OFF, &fa, &fn, &fc) &&
            list_read(cmdsys + SH_CMD_DEV_OFF, &da, &dn, &dc) && dc >= fc) return 1;
    }
    if (!grow) return sh_cmd_lists_room(cmdsys, extra);
    job.cmdsys = cmdsys;
    job.grow = grow;
    job.extra = extra;
    thread = CreateThread(NULL, 0, reserve_thread, &job, 0, NULL);
    if (!thread) return sh_cmd_lists_room(cmdsys, extra);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return sh_cmd_lists_room(cmdsys, extra);
}

static int in_list(void **array, uint32_t count, const void *cmd)
{
    uint32_t i;
    __try {
        for (i = 0; i < count; i++)
            if (array[i] == cmd) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }   /* unreadable: never append a duplicate */
    return 0;
}

uint32_t sh_cmd_lists_unlock(uint8_t *cmdsys, uint32_t *left_out)
{
    void **full, **dev;
    uint32_t n, fcap, dn, dcap, i, left = 0;
    if (left_out) *left_out = 0;
    if (!cmdsys || !list_read(cmdsys + SH_CMD_FULL_OFF, &full, &n, &fcap) || !full || !n) return 0;
    for (i = 0; i < n; i++) {
        void *cmd = NULL;
        __try { cmd = full[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!cmd) continue;
        __try { *(uint32_t *)((uint8_t *)cmd + SH_CMD_FLAGS_OFF) |= SH_CMD_BOTH_FLAGS; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!list_read(cmdsys + SH_CMD_DEV_OFF, &dev, &dn, &dcap) || !dev) { left++; continue; }
        if (in_list(dev, dn, cmd)) continue;
        if (dn >= dcap) { left++; continue; }
        __try {
            dev[dn] = cmd;
            *(uint32_t *)(cmdsys + SH_CMD_DEV_OFF + 8) = dn + 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { left++; }
    }
    if (left_out) *left_out = left;
    return n;
}
