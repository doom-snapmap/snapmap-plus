/* script_hooks_test.c -- the mod-script hooks' pure parts
 * (src/backend/script/script_queue.c, script_handles.c and the decisions of
 * script_hooks.c, built with SH_SCRIPT_PURE):
 *   - the queues: many threads enqueue, one thread drains, each producer's
 *     records arrive in order with increasing sequence numbers, a full queue
 *     drops and counts, the drop is reported once per map, only the drain
 *     thread may drain, a closed queue refuses;
 *   - the handle table: add, find, duplicates, the reserved spawn ids, the
 *     sweep's spawn and removal reports, staleness after a map change, a full
 *     table;
 *   - the mod table and the owner match of entityDef names;
 *   - the tick's guards and the gates that open a map. */
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "script/script_hooks.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ---- queues ---------------------------------------------------------------- */

#define PRODUCERS 8
#define PER_PRODUCER 100000

typedef struct producer { int id; volatile LONG *go; long queued, dropped; } producer;

static unsigned __stdcall produce(void *arg)
{
    producer *p = (producer *)arg;
    int i;
    while (!*p->go) YieldProcessor();
    for (i = 0; i < PER_PRODUCER; i++) {
        int r;
        do {
            r = sh_script_queue_post(0, SH_SCRIPT_EV_DAMAGE, (sh_script_handle)p->id, (sh_script_handle)i, 0,
                                     (float)i, "damage/test", NULL);
            if (r == 0) { p->dropped++; YieldProcessor(); }
        } while (r == 0);   /* retry a full queue, so every record gets through in order */
        p->queued++;
    }
    return 0;
}

static void test_queue_threads(void)
{
    static sh_script_event buf[SH_SCRIPT_QUEUE_CAP];
    producer p[PRODUCERS];
    HANDLE h[PRODUCERS];
    volatile LONG go = 0;
    int next[PRODUCERS] = { 0 }, i, done = 0;
    uint32_t last_seq = 0;
    long total = 0, drops = 0;
    sh_script_queue_set_drain_thread(GetCurrentThreadId());
    CHECK(sh_script_queue_open(0) == 1);
    for (i = 0; i < PRODUCERS; i++) {
        p[i].id = i;
        p[i].go = &go;
        p[i].queued = p[i].dropped = 0;
        h[i] = (HANDLE)_beginthreadex(NULL, 0, produce, &p[i], 0, NULL);
        CHECK(h[i] != NULL);
    }
    InterlockedExchange(&go, 1);
    while (!done) {
        int n = sh_script_queue_drain(0, buf, SH_SCRIPT_QUEUE_CAP), k;
        CHECK(n >= 0);
        for (k = 0; k < n; k++) {
            int who = (int)buf[k].self;
            CHECK(buf[k].seq == last_seq + 1);
            last_seq = buf[k].seq;
            CHECK(buf[k].kind == SH_SCRIPT_EV_DAMAGE);
            CHECK(!strcmp(buf[k].text, "damage/test") && !buf[k].text2[0]);
            if (who < 0 || who >= PRODUCERS) { CHECK(0); continue; }
            if ((int)buf[k].other != next[who]) { CHECK((int)buf[k].other == next[who]); }
            next[who] = (int)buf[k].other + 1;
            total++;
        }
        if (total == (long)PRODUCERS * PER_PRODUCER) done = 1;
        else if (!n) Sleep(0);
    }
    WaitForMultipleObjects(PRODUCERS, h, TRUE, INFINITE);
    for (i = 0; i < PRODUCERS; i++) {
        CloseHandle(h[i]);
        CHECK(p[i].queued == PER_PRODUCER);
        CHECK(next[i] == PER_PRODUCER);
        drops += p[i].dropped;
    }
    CHECK(sh_script_queue_pending(0) == 0);
    {
        int first = 0;
        long counted = sh_script_queue_take_dropped(0, &first);
        CHECK(counted == drops);
        CHECK(first == (drops > 0));
    }
    printf("  queue: %d producers x %d records drained in order (%ld full-queue retries)\n", PRODUCERS,
           PER_PRODUCER, drops);
    sh_script_queue_close(0);
}

static unsigned __stdcall try_drain(void *arg)
{
    sh_script_event ev;
    *(int *)arg = sh_script_queue_drain(1, &ev, 1);
    return 0;
}

static void test_queue_bounds(void)
{
    sh_script_event ev, out[4];
    int i, first = -1, other = 99;
    char long_text[200];
    HANDLE th;
    sh_script_queue_set_drain_thread(GetCurrentThreadId());
    CHECK(sh_script_queue_push(1, NULL) == -1);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);   /* not open */
    CHECK(sh_script_queue_post(-1, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);
    CHECK(sh_script_queue_post(SH_SCRIPT_MODS_MAX, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);
    CHECK(sh_script_queue_open(1) == 1);
    for (i = 0; i < SH_SCRIPT_QUEUE_CAP; i++)
        CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, (sh_script_handle)(i + 1), 0, 0, 0, "step", NULL) == 1);
    CHECK(sh_script_queue_pending(1) == SH_SCRIPT_QUEUE_CAP);
    for (i = 0; i < 5; i++) CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, 9999, 0, 0, 0, "over", NULL) == 0);
    CHECK(sh_script_queue_take_dropped(1, &first) == 5 && first == 1);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, 9999, 0, 0, 0, "over", NULL) == 0);
    CHECK(sh_script_queue_take_dropped(1, &first) == 1 && first == 0);   /* the line is written once per map */
    CHECK(sh_script_queue_take_dropped(1, &first) == 0 && first == 0);
    /* Oldest first; the dropped records are the new ones. */
    CHECK(sh_script_queue_drain(1, out, 4) == 4);
    CHECK(out[0].self == 1 && out[3].self == 4 && out[0].seq == 1 && out[3].seq == 4);
    /* Another thread may not drain. */
    th = (HANDLE)_beginthreadex(NULL, 0, try_drain, &other, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CHECK(other == -1);
    CHECK(sh_script_queue_pending(1) == SH_SCRIPT_QUEUE_CAP - 4);
    /* Strings are copied and cut to fit. */
    memset(long_text, 'a', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = 0;
    sh_script_queue_close(1);
    CHECK(sh_script_queue_pending(1) == 0);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_INPUT, 1, 2, 3, 1.5f, "n", NULL) == -1);   /* closed */
    CHECK(sh_script_queue_open(1) == 1);   /* a new map: empty, counters reset */
    CHECK(sh_script_queue_take_dropped(1, &first) == 0);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_INPUT, 1, 2, 3, 1.5f, long_text, long_text) == 1);
    CHECK(sh_script_queue_drain(1, &ev, 1) == 1);
    CHECK(ev.seq == 1 && ev.kind == SH_SCRIPT_EV_INPUT && ev.self == 1 && ev.other == 2 && ev.third == 3 && ev.amount == 1.5f);
    CHECK(strlen(ev.text) == SH_SCRIPT_EVENT_TEXT - 1 && strlen(ev.text2) == SH_SCRIPT_EVENT_TEXT - 1);
    /* A record pushed between drains waits for the next one. */
    CHECK(sh_script_queue_drain(1, out, 4) == 0);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_DEATH, 7, 0, 0, 0, NULL, NULL) == 1);
    CHECK(sh_script_queue_drain(1, out, 4) == 1 && out[0].kind == SH_SCRIPT_EV_DEATH && out[0].seq == 2);
    sh_script_queue_close_all();
}

/* ---- handles ---------------------------------------------------------------- */

#define FAKE_MAX 16384
static int g_present[FAKE_MAX];   /* spawn id (small in the fake) -> present */

static void *fake_resolve(uint32_t spawn_id)
{
    return spawn_id < FAKE_MAX && g_present[spawn_id] ? (void *)(uintptr_t)(0x10000 + spawn_id) : NULL;
}

static void test_handles(void)
{
    sh_script_handle_change ch[16];
    sh_script_handle a, b, c, d, old;
    int mod, full, n, i;
    memset(g_present, 0, sizeof g_present);
    sh_script_handles_set_resolver(fake_resolve);
    sh_script_handles_reset();
    CHECK(sh_script_handles_add(0, SH_SCRIPT_SPAWN_ID_NONE, NULL) == 0);
    CHECK(sh_script_handles_add(0, SH_SCRIPT_SPAWN_ID_WORLD, NULL) == 0);
    CHECK(sh_script_handles_add(-1, 5, NULL) == 0);
    a = sh_script_handles_add(0, 10, &full);
    b = sh_script_handles_add(1, 11, NULL);
    c = sh_script_handles_add(0, 12, NULL);
    d = sh_script_handles_add(0, 13, NULL);
    CHECK(a && b && c && d && !full);
    CHECK(a != b && b != c && c != d);
    CHECK(sh_script_handles_add(0, 10, NULL) == a);     /* the same spawn again */
    CHECK(sh_script_handles_add(1, 10, NULL) == 0);     /* another mod cannot claim it */
    CHECK(sh_script_handles_find(11, &mod) == b && mod == 1);
    CHECK(sh_script_handles_find(99, &mod) == 0 && mod == -1);
    CHECK(sh_script_handle_mod(a) == 0 && sh_script_handle_mod(b) == 1 && sh_script_handle_mod(0) == -1);
    CHECK(sh_script_handle_spawn_id(c) == 12);
    /* Not yet reported: nothing resolves, nothing is alive. */
    g_present[10] = g_present[11] = g_present[12] = 1;   /* 13 died before the first tick */
    CHECK(!sh_script_handle_alive(a) && sh_script_handle_entity(a) == NULL);
    n = sh_script_handles_sweep(ch, 16);
    CHECK(n == 3);
    CHECK(ch[0].handle == a && ch[0].kind == SH_SCRIPT_HANDLE_SPAWNED && ch[0].mod == 0);
    CHECK(ch[1].handle == b && ch[1].kind == SH_SCRIPT_HANDLE_SPAWNED && ch[1].mod == 1);
    CHECK(ch[2].handle == c && ch[2].kind == SH_SCRIPT_HANDLE_SPAWNED);
    CHECK(!sh_script_handle_alive(d) && sh_script_handle_entity(d) == NULL);   /* never seen by a script */
    CHECK(sh_script_handle_entity(a) == (void *)(uintptr_t)0x1000A);
    CHECK(sh_script_handles_live(-1) == 3 && sh_script_handles_live(0) == 2 && sh_script_handles_live(1) == 1);
    CHECK(sh_script_handles_sweep(ch, 16) == 0);   /* nothing changed */
    /* Removal: the next call resolves to nothing at once; the sweep reports it once. */
    g_present[11] = 0;
    CHECK(sh_script_handle_entity(b) == NULL);
    n = sh_script_handles_sweep(ch, 16);
    CHECK(n == 1 && ch[0].handle == b && ch[0].kind == SH_SCRIPT_HANDLE_REMOVED && ch[0].mod == 1);
    g_present[11] = 1;                       /* the slot is reused by another entity with the same id in the fake */
    CHECK(sh_script_handles_sweep(ch, 16) == 0);
    CHECK(!sh_script_handle_alive(b) && sh_script_handle_entity(b) == NULL);   /* a dead handle stays dead */
    /* A sweep with less room continues next time. */
    for (i = 100; i < 105; i++) { g_present[i] = 1; CHECK(sh_script_handles_add(2, (uint32_t)i, NULL)); }
    CHECK(sh_script_handles_sweep(ch, 2) == 2);
    CHECK(sh_script_handles_sweep(ch, 16) == 3);
    /* A map change: old handles name nothing. */
    old = a;
    sh_script_handles_reset();
    CHECK(sh_script_handle_mod(old) == -1 && sh_script_handle_entity(old) == NULL && !sh_script_handle_alive(old));
    a = sh_script_handles_add(0, 10, NULL);
    CHECK(a && a != old);
    CHECK(sh_script_handles_sweep(ch, 16) == 1 && sh_script_handle_entity(a) != NULL);
    /* A full table refuses and says so. */
    sh_script_handles_reset();
    for (i = 0; i < SH_SCRIPT_HANDLES_MAX; i++) CHECK(sh_script_handles_add(0, (uint32_t)(20000 + i), NULL) != 0);
    CHECK(sh_script_handles_add(0, 999999, &full) == 0 && full == 1);
    CHECK(sh_script_handles_find((uint32_t)(20000 + SH_SCRIPT_HANDLES_MAX - 1), &mod) != 0 && mod == 0);
    sh_script_handles_reset();
}

/* ---- mods and owners ------------------------------------------------------------ */

static void test_mods(void)
{
    const char *imp[] = { "snapmaps/placed/imp_12", "mods/AbCd1234/ai/imp", "ai/fodder/imp" };
    const char *other[] = { "mods/zzzz9999/ai/imp" };
    const char *own[] = { "MODS\\abcd1234\\ai\\brute" };
    const char *bare[] = { "mods/abcd1234", "mods/abcd1234/", "mods//x", "modsx/abcd1234/a" };
    const char *two[] = { "mods/efgh5678/script", "mods/abcd1234/ai/imp" };
    int a, b;
    sh_script_hooks_test_reset();
    CHECK(sh_script_hooks_owner_of(imp, 3) == -1);   /* no mods registered */
    CHECK(sh_script_hooks_mod_add(NULL, "x") == -1);
    CHECK(sh_script_hooks_mod_add("bad/salt", "x") == -1);
    CHECK(sh_script_hooks_mod_add("waytoolongsaltvalue", "x") == -1);
    a = sh_script_hooks_mod_add("abcd1234", "Imp Pack");
    b = sh_script_hooks_mod_add("efgh5678", "");
    CHECK(a >= 0 && b >= 0 && a != b);
    CHECK(sh_script_hooks_mod_add("ABCD1234", "again") == a);
    CHECK(sh_script_hooks_mod_count() == 2);
    CHECK(!strcmp(sh_script_hooks_mod_name(a), "Imp Pack") && !strcmp(sh_script_hooks_mod_name(b), "efgh5678"));
    CHECK(!strcmp(sh_script_hooks_mod_salt(a), "abcd1234") && sh_script_hooks_mod_salt(SH_SCRIPT_MODS_MAX - 1) == NULL);
    CHECK(sh_script_hooks_owner_of(imp, 3) == a);    /* a placed entity's inherit names the mod */
    CHECK(sh_script_hooks_owner_of(other, 1) == -1);
    CHECK(sh_script_hooks_owner_of(own, 1) == a);    /* either case, either separator */
    CHECK(sh_script_hooks_owner_of(bare, 4) == -1);
    CHECK(sh_script_hooks_owner_of(two, 2) == b);    /* the first mods name decides */
    CHECK(sh_script_hooks_owner_of(NULL, 2) == -1 && sh_script_hooks_owner_of(imp, 0) == -1);
    /* The table holds SH_SCRIPT_MODS_MAX mods. */
    {
        char salt[16];
        int i, last = 0;
        for (i = 2; i < SH_SCRIPT_MODS_MAX; i++) {
            _snprintf_s(salt, sizeof salt, _TRUNCATE, "s%07d", i);
            last = sh_script_hooks_mod_add(salt, salt);
            CHECK(last >= 0);
        }
        CHECK(sh_script_hooks_mod_add("onetoomany", "x") == -1);
        CHECK(sh_script_queue_post(last, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == 1);   /* its queue is open */
    }
    sh_script_hooks_test_reset();
    CHECK(sh_script_hooks_mod_count() == 0 && sh_script_hooks_owner_of(imp, 3) == -1);
    CHECK(sh_script_queue_post(a, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);   /* closed with the map */
}

/* ---- the tick's guards and the gates -------------------------------------------- */

static void test_tick(void)
{
    sh_script_tick_state s;
    memset(&s, 0, sizeof s);
    s.open = 1; s.on_main = 1; s.is_client = 0; s.loading = 0; s.in_play = 1;
    CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_RUN);
    s.open = 0; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_IDLE); s.open = 1;
    CHECK(sh_script_tick_decide(NULL) == SH_SCRIPT_TICK_IDLE);
    s.in_tick = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_REENTERED); s.in_tick = 0;
    s.on_main = 0; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_OFF_MAIN);
    s.on_main = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN); s.on_main = 1;
    s.is_client = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_CLIENT);
    s.in_play = 0; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_CLIENT); s.in_play = 1;   /* client first */
    s.is_client = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN); s.is_client = 0;
    s.loading = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_LOADING);
    s.started = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_LOADING); s.started = 0;
    s.loading = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN); s.loading = 0;
    s.in_play = 0; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_NOT_PLAY);   /* the editor */
    s.started = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_END);        /* Play ended */
    s.in_play = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN);
    s.in_play = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_RUN);
    CHECK(!strcmp(sh_script_tick_name(SH_SCRIPT_TICK_LOADING), "loading"));
    CHECK(sh_script_gate_opens("map") && sh_script_gate_opens("play"));
    CHECK(!sh_script_gate_opens("editor") && !sh_script_gate_opens("rescan") && !sh_script_gate_opens("launch") &&
          !sh_script_gate_opens("return to the editor") && !sh_script_gate_opens(NULL));
}

int main(void)
{
    test_queue_bounds();
    test_queue_threads();
    test_handles();
    test_mods();
    test_tick();
    printf(g_fail ? "script_hooks_test: FAIL\n" : "script_hooks_test: PASS\n");
    return g_fail;
}
