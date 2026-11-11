/* cmd_lists_test.c - the engine's command lists get their room off the calling
 * thread and the backfill never grows a list (src/backend/cmd_lists.h).
 *
 * The fake command system has the engine's layout: two lists of command
 * pointers at +0x08 and +0x20 (array, count +8, capacity +0xC). The fake growth
 * helper doubles a list and records the thread it ran on: growth on the
 * caller's thread is the defect this guards against, because in the game that
 * thread allocates from a heap the next level load destroys. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/cmd_lists.h"

static int g_failures, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct cmd { const char *name; void *handler; void *comp; const char *help; uint32_t flags; } cmd;

static DWORD g_grow_threads[64];
static int g_grows;
static int g_grow_refuses;

static void fake_grow(void *list)
{
    uint8_t *l = (uint8_t *)list;
    void **old = *(void ***)l;
    uint32_t n = *(uint32_t *)(l + 8), cap = *(uint32_t *)(l + 12), ncap = cap ? cap * 2 : 1, i;
    void **fresh;
    if (g_grows < 64) g_grow_threads[g_grows] = GetCurrentThreadId();
    g_grows++;
    if (g_grow_refuses) return;
    fresh = (void **)calloc(ncap, sizeof *fresh);
    for (i = 0; i < n; i++) fresh[i] = old[i];
    free(old);
    *(void ***)l = fresh;
    *(uint32_t *)(l + 12) = ncap;
}

static void list_init(uint8_t *list, uint32_t cap)
{
    *(void ***)list = (void **)calloc(cap ? cap : 1, sizeof(void *));
    *(uint32_t *)(list + 8) = 0;
    *(uint32_t *)(list + 12) = cap;
}

static void list_push(uint8_t *list, void *v)
{
    uint32_t n = *(uint32_t *)(list + 8);
    (*(void ***)list)[n] = v;
    *(uint32_t *)(list + 8) = n + 1;
}

static uint32_t count_of(uint8_t *list) { return *(uint32_t *)(list + 8); }
static uint32_t cap_of(uint8_t *list) { return *(uint32_t *)(list + 12); }

static int grew_on_this_thread(void)
{
    int i;
    for (i = 0; i < g_grows && i < 64; i++)
        if (g_grow_threads[i] == GetCurrentThreadId()) return 1;
    return 0;
}

int main(void)
{
    uint8_t sys[0x40];
    cmd cmds[327];
    uint32_t left = 99, walked, i;
    void **dev;

    /* the game's shape at the first frame: 285 commands in a full list of 512, 64 of them in a developer list of 64 */
    memset(sys, 0, sizeof sys);
    memset(cmds, 0, sizeof cmds);
    list_init(sys + SH_CMD_FULL_OFF, 512);
    list_init(sys + SH_CMD_DEV_OFF, 64);
    for (i = 0; i < 285; i++) { cmds[i].name = "c"; list_push(sys + SH_CMD_FULL_OFF, &cmds[i]); }
    for (i = 0; i < 64; i++) list_push(sys + SH_CMD_DEV_OFF, &cmds[i]);

    CHECK(!sh_cmd_lists_room(sys, 42));                    /* the developer list is full */
    CHECK(sh_cmd_lists_reserve(sys, fake_grow, 42) == 1);
    CHECK(g_grows > 0 && !grew_on_this_thread());          /* every growth ran on the helper thread */
    CHECK(cap_of(sys + SH_CMD_FULL_OFF) == 512);           /* the full list had room: untouched */
    CHECK(cap_of(sys + SH_CMD_DEV_OFF) >= 512);            /* as much room as the full list */
    CHECK(count_of(sys + SH_CMD_DEV_OFF) == 64);
    CHECK(sh_cmd_lists_room(sys, 42));

    /* the product's own registrations: the engine appends each to both lists, with room */
    for (i = 285; i < 327; i++) {
        cmds[i].name = "sh"; cmds[i].flags = 6;
        list_push(sys + SH_CMD_FULL_OFF, &cmds[i]);
        list_push(sys + SH_CMD_DEV_OFF, &cmds[i]);
    }
    g_grows = 0;
    walked = sh_cmd_lists_unlock(sys, &left);
    CHECK(walked == 327 && left == 0);
    CHECK(g_grows == 0);                                   /* the backfill never grows */
    CHECK(count_of(sys + SH_CMD_DEV_OFF) == 327);
    dev = *(void ***)(sys + SH_CMD_DEV_OFF);
    for (i = 0; i < 327; i++) {
        uint32_t k, seen = 0;
        for (k = 0; k < 327; k++) if (dev[k] == &cmds[i]) seen++;
        if (seen != 1 || (cmds[i].flags & 6) != 6) break;
    }
    CHECK(i == 327);                                       /* every command once, with both flags */

    /* a second pass changes nothing */
    walked = sh_cmd_lists_unlock(sys, &left);
    CHECK(walked == 327 && left == 0 && count_of(sys + SH_CMD_DEV_OFF) == 327);

    /* room already there: no thread, no growth */
    g_grows = 0;
    CHECK(sh_cmd_lists_reserve(sys, fake_grow, 42) == 1 && g_grows == 0);

    /* growth unavailable: the answer is whether the room is there, and the backfill leaves commands out
     * rather than grow */
    {
        uint8_t small[0x40];
        cmd c2[8];
        memset(small, 0, sizeof small);
        memset(c2, 0, sizeof c2);
        list_init(small + SH_CMD_FULL_OFF, 8);
        list_init(small + SH_CMD_DEV_OFF, 4);
        for (i = 0; i < 6; i++) list_push(small + SH_CMD_FULL_OFF, &c2[i]);
        for (i = 0; i < 4; i++) list_push(small + SH_CMD_DEV_OFF, &c2[i]);
        CHECK(sh_cmd_lists_reserve(small, NULL, 1) == 0);
        g_grow_refuses = 1; g_grows = 0;
        CHECK(sh_cmd_lists_reserve(small, fake_grow, 1) == 0);
        CHECK(!grew_on_this_thread());
        g_grow_refuses = 0;
        walked = sh_cmd_lists_unlock(small, &left);
        CHECK(walked == 6 && left == 2);
        CHECK(count_of(small + SH_CMD_DEV_OFF) == 4 && cap_of(small + SH_CMD_DEV_OFF) == 4);
        CHECK((c2[5].flags & 6) == 6);                     /* still exposed outside developer mode */
        /* a full list without room for the product's commands grows off-thread too */
        g_grows = 0;
        CHECK(sh_cmd_lists_reserve(small, fake_grow, 42) == 1);
        CHECK(cap_of(small + SH_CMD_FULL_OFF) >= 48 && cap_of(small + SH_CMD_DEV_OFF) >= cap_of(small + SH_CMD_FULL_OFF));
        CHECK(!grew_on_this_thread());
    }

    /* unreadable or absent system */
    CHECK(sh_cmd_lists_reserve(NULL, fake_grow, 1) == 0);
    CHECK(sh_cmd_lists_unlock(NULL, &left) == 0 && left == 0);
    CHECK(!sh_cmd_lists_room(NULL, 0));

    printf("cmd_lists_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
