/* script_hooks_test.c -- the mod-script hooks' pure parts
 * (src/backend/script/script_queue.c, script_handles.c and the decisions of
 * script_hooks.c, built with SH_SCRIPT_PURE):
 *   - the queues: many threads enqueue, one thread drains, each producer's
 *     records arrive in order with increasing sequence numbers, a queue grows
 *     with what arrives and drops nothing, only the drain thread may drain, a
 *     closed queue refuses;
 *   - the handle table: add, find, duplicates, the reserved spawn ids, the
 *     sweep's spawn, removal and gone reports, a record released with its
 *     entity and its slot reused under a new generation, staleness after a
 *     map change, growth far past any old bound;
 *   - the mod table (it grows) and the owner match of entityDef names;
 *   - the tick's guards and the gates that open a map. */
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "script/script_hooks.h"

/* The runtime is not linked: the hooks' test-only reload (a console test's seam) is not used here. */
int sh_script_map_open(void) { return 0; }
void sh_script_map_close(int run_end_handlers) { (void)run_end_handlers; }

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
        } while (r == 0);   /* a record is dropped only without memory: retry it, so every record arrives in order */
        p->queued++;
    }
    return 0;
}

static void test_queue_threads(void)
{
    static sh_script_event buf[1024];
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
        int n = sh_script_queue_drain(0, buf, 1024), k;
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
    CHECK(drops == 0);
    printf("  queue: %d producers x %d records drained in order, none dropped\n", PRODUCERS, PER_PRODUCER);
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
    char long_text[5000];
    HANDLE th;
    sh_script_queue_set_drain_thread(GetCurrentThreadId());
    CHECK(sh_script_queue_push(1, NULL) == -1);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);   /* not open */
    CHECK(sh_script_queue_post(-1, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);
    CHECK(sh_script_queue_post(100000, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == -1);   /* never opened */
    CHECK(sh_script_queue_open(1) == 1);
    /* One frame's records: far past the old 1,024, nothing is dropped. */
    for (i = 0; i < 50000; i++)
        CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, (sh_script_handle)(i + 1), 0, 0, 0, "step", NULL) == 1);
    CHECK(sh_script_queue_pending(1) == 50000);
    CHECK(sh_script_queue_take_dropped(1, &first) == 0 && first == 0);
    /* Oldest first. */
    CHECK(sh_script_queue_drain(1, out, 4) == 4);
    CHECK(out[0].self == 1 && out[3].self == 4 && out[0].seq == 1 && out[3].seq == 4);
    /* Another thread may not drain. */
    th = (HANDLE)_beginthreadex(NULL, 0, try_drain, &other, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CHECK(other == -1);
    CHECK(sh_script_queue_pending(1) == 50000 - 4);
    {
        static sh_script_event rest[1024];
        int got, seen = 4, ordered = 1;
        while ((got = sh_script_queue_drain(1, rest, 1024)) > 0) {
            int k;
            for (k = 0; k < got; k++) if (rest[k].self != (sh_script_handle)(++seen)) ordered = 0;
        }
        CHECK(seen == 50000 && ordered);
    }
    /* A queue far above the first ones: the queues grow with the mods. */
    CHECK(sh_script_queue_open(300) == 1);
    CHECK(sh_script_queue_post(300, SH_SCRIPT_EV_ANIM, 1, 0, 0, 0, "x", NULL) == 1);
    CHECK(sh_script_queue_post(1, SH_SCRIPT_EV_ANIM, 2, 0, 0, 0, "y", NULL) == 1);   /* the earlier one still works */
    CHECK(sh_script_queue_drain(300, out, 4) == 1 && out[0].self == 1);
    CHECK(sh_script_queue_drain(1, out, 4) == 1 && out[0].self == 2);
    /* Strings are copied whole, of any length (the old record cut them at 127 bytes). */
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
    CHECK(strlen(ev.text) == sizeof long_text - 1 && strlen(ev.text2) == sizeof long_text - 1);
    sh_script_event_release(&ev);
    CHECK(ev.text == NULL && ev.text2 == NULL);
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

/* Records come and go with their entities: projectiles launched and gone,
 * a million over a map, never more than a few hundred alive. The table
 * stays small, every handle of a gone one stays dead, and the gone callback
 * hears each exactly once. */
static long g_gone_calls;
static void count_gone(sh_script_handle h, uint32_t id) { (void)h; (void)id; g_gone_calls++; }

static void test_handle_reuse(void)
{
    static sh_script_handle held[300];
    sh_script_handle_change ch[512];
    sh_script_handle first_gone = 0;
    int round, k, stale_alive = 0;
    sh_script_handles_on_gone(count_gone);
    g_gone_calls = 0;
    memset(g_present, 0, sizeof g_present);
    sh_script_handles_reset();
    for (round = 0; round < 4000; round++) {
        for (k = 0; k < 300; k++) {
            uint32_t id = (uint32_t)(1 + ((round & 1) * 300) + k);   /* alternate ids, as the engine's slots */
            g_present[id] = 1;
            held[k] = sh_script_handles_ref(id);
            if (!held[k]) stale_alive = -1;
        }
        if (round == 0) first_gone = held[0];
        for (k = 0; k < 300; k++) g_present[1 + ((round & 1) * 300) + k] = 0;   /* all gone */
        while (sh_script_handles_sweep(ch, 512) > 0) {}
        if (sh_script_handle_known(first_gone) || sh_script_handle_entity(first_gone)) stale_alive = 1;
    }
    CHECK(stale_alive == 0);
    CHECK(g_gone_calls == 4000L * 300);
    CHECK(sh_script_handles_count() == 0);
    printf("  handles: 1,200,000 references made and released, none alive after, the first stays dead\n");
    sh_script_handles_reset();
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
    CHECK(n == 4);
    CHECK(ch[0].handle == a && ch[0].kind == SH_SCRIPT_HANDLE_SPAWNED && ch[0].mod == 0);
    CHECK(ch[1].handle == b && ch[1].kind == SH_SCRIPT_HANDLE_SPAWNED && ch[1].mod == 1);
    CHECK(ch[2].handle == c && ch[2].kind == SH_SCRIPT_HANDLE_SPAWNED);
    CHECK(n == 4 && ch[3].handle == d && ch[3].kind == SH_SCRIPT_HANDLE_GONE);   /* gone before its first sweep */
    CHECK(!sh_script_handle_alive(d) && sh_script_handle_entity(d) == NULL);   /* never seen by a script */
    CHECK(sh_script_handle_entity(a) == (void *)(uintptr_t)0x1000A);
    CHECK(sh_script_handles_live(-1) == 3 && sh_script_handles_live(0) == 2 && sh_script_handles_live(1) == 1);
    CHECK(sh_script_handles_sweep(ch, 16) == 0);   /* nothing changed */
    /* Removal: the next call resolves to nothing at once; the sweep reports it once. */
    g_present[11] = 0;
    CHECK(sh_script_handle_entity(b) == NULL);
    n = sh_script_handles_sweep(ch, 16);
    CHECK(n == 1 && ch[0].handle == b && ch[0].kind == SH_SCRIPT_HANDLE_REMOVED && ch[0].mod == 1);
    CHECK(!sh_script_handle_known(b) && sh_script_handles_find(11, NULL) == 0);   /* released with its entity */
    g_present[11] = 1;                       /* the engine's slot is reused by another entity with the same id in the fake */
    CHECK(sh_script_handles_sweep(ch, 16) == 0);
    CHECK(!sh_script_handle_alive(b) && sh_script_handle_entity(b) == NULL);   /* a released handle stays dead */
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
    /* No count of its own: 100,000 records at once, every one found. */
    sh_script_handles_reset();
    full = 0;
    for (i = 0; i < 100000; i++) if (!sh_script_handles_add(0, (uint32_t)(20000 + i), &full)) break;
    CHECK(i == 100000 && full == 0);
    CHECK(sh_script_handles_count() == 100000);
    CHECK(sh_script_handles_find((uint32_t)(20000 + 99999), &mod) != 0 && mod == 0);
    sh_script_handles_reset();
    test_handle_reuse();
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
    CHECK(!strcmp(sh_script_hooks_mod_salt(a), "abcd1234") && sh_script_hooks_mod_salt(1000) == NULL);
    CHECK(sh_script_hooks_owner_of(imp, 3) == a);    /* a placed entity's inherit names the mod */
    CHECK(sh_script_hooks_owner_of(other, 1) == -1);
    CHECK(sh_script_hooks_owner_of(own, 1) == a);    /* either case, either separator */
    CHECK(sh_script_hooks_owner_of(bare, 4) == -1);
    CHECK(sh_script_hooks_owner_of(two, 2) == b);    /* the first mods name decides */
    CHECK(sh_script_hooks_owner_of(NULL, 2) == -1 && sh_script_hooks_owner_of(imp, 0) == -1);
    /* The table grows: 500 mods with scripts in one map, each with its queue. */
    {
        char salt[16];
        int i, last = 0;
        for (i = 2; i < 500; i++) {
            _snprintf_s(salt, sizeof salt, _TRUNCATE, "s%07d", i);
            last = sh_script_hooks_mod_add(salt, salt);
            CHECK(last >= 0);
        }
        CHECK(sh_script_hooks_mod_count() == 500);
        CHECK(sh_script_hooks_mod_find("s0000499") == last && !strcmp(sh_script_hooks_mod_name(last), "s0000499"));
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
    /* a loading screen after the scripts started: the session left the map */
    s.started = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_LEFT); s.started = 0;
    s.loading = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN); s.loading = 0;
    s.in_play = 0; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_NOT_PLAY);   /* the editor */
    s.started = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_END);        /* Play ended */
    s.in_play = -1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_UNKNOWN);
    s.in_play = 1; CHECK(sh_script_tick_decide(&s) == SH_SCRIPT_TICK_RUN);
    CHECK(!strcmp(sh_script_tick_name(SH_SCRIPT_TICK_LOADING), "loading"));
    /* only the Play build opens a map's mods; a map load only ends the last ones */
    CHECK(sh_script_gate_opens(SH_SCRIPT_GATE_PLAY));
    CHECK(!sh_script_gate_opens(SH_SCRIPT_GATE_MAP_LOAD) && !sh_script_gate_opens(0));
}

/* ---- recording entities: owner, chain, foreign references ------------------------ */

static void test_record(void)
{
    const char *chain[] = { "mods/abcd1234/AI/Brute", "MODS/abcd1234/ai/base" };
    const char *stock[] = { "ai/fodder/imp" };
    char names[256];
    sh_script_handle h, f, g;
    int a, full = 0, mod = 0;
    sh_script_hooks_test_reset();
    sh_script_handles_reset();
    a = sh_script_hooks_mod_add("abcd1234", "Brutes");
    CHECK(sh_script_hooks_mod_find("ABCD1234") == a && sh_script_hooks_mod_find("nope") == -1);
    h = sh_script_hooks_record(500, chain, 2, &full);
    CHECK(h != 0 && !full && sh_script_handle_mod(h) == a);
    CHECK(sh_script_handle_names(h, names, sizeof names) > 0);
    CHECK(!strcmp(names, "mods/abcd1234/ai/brute\nmods/abcd1234/ai/base"));
    CHECK(sh_script_hooks_record(500, chain, 2, NULL) == h);           /* recorded once */
    CHECK(sh_script_hooks_record(501, stock, 1, NULL) == 0);           /* no scripted mod owns it */
    CHECK(sh_script_handle_known(h) && !sh_script_handle_alive(h));    /* waiting for its first sweep */
    /* a foreign reference (a player, an attacker): live at once, owned by nobody, never swept */
    f = sh_script_handles_ref(77);
    CHECK(f != 0 && sh_script_handles_ref(77) == f);
    CHECK(sh_script_handle_mod(f) == SH_SCRIPT_HANDLE_FOREIGN && sh_script_handle_alive(f));
    CHECK(sh_script_handles_find(77, &mod) == f && mod == SH_SCRIPT_HANDLE_FOREIGN);
    CHECK(sh_script_handle_names(f, names, sizeof names) == 0 && names[0] == 0);
    g = sh_script_handles_ref(500);
    CHECK(g == h);                                                     /* an owned entity keeps its record */
    CHECK(sh_script_handles_ref(SH_SCRIPT_SPAWN_ID_NONE) == 0 && sh_script_handles_ref(SH_SCRIPT_SPAWN_ID_WORLD) == 0);
    CHECK(sh_script_handles_live(-1) == 0);                            /* foreign references are not counted */
    sh_script_handles_reset();
    CHECK(sh_script_handle_names(h, names, sizeof names) == -1 && !sh_script_handle_known(h));
    sh_script_hooks_test_reset();
}

int main(void)
{
    test_queue_bounds();
    test_queue_threads();
    test_handles();
    test_mods();
    test_tick();
    test_record();
    printf(g_fail ? "script_hooks_test: FAIL\n" : "script_hooks_test: PASS\n");
    return g_fail;
}
