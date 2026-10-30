/* modpublished_test -- the published-map and offline-play gates: return-address
 * classification, pass-through for installed bundles, the hold, the question,
 * Yes (fetch on a worker, activation, resume with a fresh parse of the retained
 * text), No, a failed fetch, a bundle that will not install, a second load
 * abandoning the hold, the joiner's host re-check, the warm-cache joiner branch
 * and My Maps play.
 *
 * The real map manifest (modmanifest.c) reads the rows; the engine and the
 * modsync worker are doubles behind sh_modpub_ops. The worker double runs the
 * job on a real thread so the main-thread tick is exercised as in the game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modpublished.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ---- serving-layer doubles the real manifest calls ------------------------------------------ */

void backend_log(const char *line) { (void)line; }
int sh_modserve_active(void) { return 1; }
int modserve_activate_bundles(const char *const *bundle_hashes, int count)
{
    (void)bundle_hashes;
    return count;
}
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

/* ---- fixtures ------------------------------------------------------------------------------- */

#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HASH_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"
#define ROW(id, hash, size) \
    "{\\\"id\\\":\\\"" id "\\\",\\\"version\\\":\\\"1\\\",\\\"bundle\\\":\\\"" hash "\\\",\\\"size\\\":" size \
    ",\\\"names\\\":[]}"

static const char VANILLA[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    VAR("door_label", "Open") "]}}";
static const char ONE_MOD[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
    VAR("door_label", "Open") "," VAR("smpmod1.0.1", ROW("alex.pink-imp", HASH_A, "3145728")) "]}}";
static const char TWO_MODS[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
    VAR("smpmod1.0.2", ROW("alex.pink-imp", HASH_A, "3145728")) ","
    VAR("smpmod1.1.2", ROW("bob.room", HASH_B, "1048576")) "]}}";

/* ---- engine and worker doubles -------------------------------------------------------------- */

typedef struct fake {
    int installed_a, installed_b, install_on_fetch, refuse_install;
    int fetch_fails;                 /* nonzero: the fetch of the first bundle returns this */
    int fetch_calls, fetch_cancelled;
    HANDLE fetch_gate;               /* when set, the fetch waits on it */
    int can_ask_after;               /* ticks before the surface is idle */
    int ticks;
    int asks, ask_gdm, ask_buttons, releases;
    char last_ask[256];
    int answer;                      /* what poll returns once */
    int answered;
    int rescans, activations, activated_count;
    char activated[4][65];
    int metadata_copies, metadata_frees;
    int hold_ui, drop_ui;
    int resumes, resume_hook;
    char resume_text[4096];
    void *resume_extra;
    char resume_id[96];
    int host_ok;
    int owner_ok;
    unsigned long long token;
    HANDLE worker;
} fake;

static fake F;
static int g_metadata_cookie;

static int f_on_main(void *c) { (void)c; return 1; }
static int f_bundle_state(void *c, const char *hash, char *cause, size_t cap)
{
    (void)c;
    if (cause && cap) cause[0] = 0;
    if (!strcmp(hash, HASH_A)) {
        if (!F.installed_a && F.refuse_install && cause) strcpy_s(cause, cap, "a file size differs");
        return F.installed_a ? 1 : (F.refuse_install && F.fetch_calls ? -1 : 0);
    }
    if (!strcmp(hash, HASH_B)) return F.installed_b ? 1 : 0;
    return 0;
}
static int f_rescan(void *c) { (void)c; F.rescans++; return 1; }
static int f_activate(void *c, const char *const *hashes, int n)
{
    (void)c;
    F.activations++;
    F.activated_count = n;
    for (int i = 0; i < n && i < 4; i++) strcpy_s(F.activated[i], sizeof F.activated[i], hashes[i]);
    return n;
}
static int f_can_ask(void *c) { (void)c; return F.ticks >= F.can_ask_after; }
static int f_ask(void *c, unsigned gdm, unsigned set, const char *text)
{
    (void)c;
    F.asks++; F.ask_gdm = (int)gdm; F.ask_buttons = (int)set;
    strcpy_s(F.last_ask, sizeof F.last_ask, text);
    F.answered = 0;
    return 100 + F.asks;
}
static int f_poll(void *c, int ticket)
{
    (void)c; (void)ticket;
    if (F.answered) return 0;
    if (!F.answer) return 0;
    F.answered = 1;
    return F.answer;
}
static void f_release(void *c, int ticket) { (void)c; (void)ticket; F.releases++; }

typedef struct job_thread { void (*job)(void *); void *ctx; } job_thread;
static DWORD WINAPI f_thread(void *p)
{
    job_thread *t = p;
    t->job(t->ctx);
    free(t);
    return 0;
}
static int f_worker_run(void *c, void (*job)(void *), void *ctx)
{
    job_thread *t = malloc(sizeof *t);
    (void)c;
    if (!t) return 0;
    t->job = job; t->ctx = ctx;
    if (F.worker) CloseHandle(F.worker);
    F.worker = CreateThread(NULL, 0, f_thread, t, 0, NULL);
    return F.worker != NULL;
}
static int f_fetch(void *c, const char *hash, sh_modpub_progress_fn progress, volatile int *cancel)
{
    (void)c;
    F.fetch_calls++;
    if (progress) progress(NULL, 1, 2);
    if (F.fetch_gate) {
        while (WaitForSingleObject(F.fetch_gate, 5) == WAIT_TIMEOUT)
            if (*cancel) { F.fetch_cancelled++; return -2; }
    }
    if (F.fetch_fails && F.fetch_calls == 1) return F.fetch_fails;
    if (F.install_on_fetch && !F.refuse_install) {
        if (!strcmp(hash, HASH_A)) F.installed_a = 1;
        if (!strcmp(hash, HASH_B)) F.installed_b = 1;
    }
    return 0;
}
static void *f_metadata_copy(void *c, const void *metadata)
{
    (void)c;
    if (!metadata) return NULL;
    F.metadata_copies++;
    return &g_metadata_cookie;
}
static void f_metadata_free(void *c, void *copy) { (void)c; if (copy == &g_metadata_cookie) F.metadata_frees++; }
static int f_metadata_ids(void *c, const void *metadata, char *id, size_t id_cap, char *rev, size_t rev_cap)
{
    (void)c;
    strcpy_s(id, id_cap, (const char *)metadata);
    strcpy_s(rev, rev_cap, "rev1");
    return 1;
}
static unsigned long long f_hold_token(void *c, int hook, void *owner, const void *extra)
{
    (void)c; (void)hook; (void)owner; (void)extra;
    return F.token;
}
static int f_owner_current(void *c, int hook, void *owner, const void *extra, unsigned long long token)
{
    (void)c; (void)hook; (void)owner; (void)extra;
    return F.owner_ok && token == F.token;
}
static int f_host_matches(void *c, const char *id, const char *revision)
{
    (void)c; (void)id; (void)revision;
    return F.host_ok;
}
static void f_hold_ui(void *c, int hook, void *owner) { (void)c; (void)hook; (void)owner; F.hold_ui++; }
static void f_drop_ui(void *c, int hook, void *owner) { (void)c; (void)hook; (void)owner; F.drop_ui++; }
static int f_resume(void *c, const sh_modpub_resume *r)
{
    (void)c;
    F.resumes++;
    F.resume_hook = r->hook;
    F.resume_extra = r->extra;
    strcpy_s(F.resume_text, sizeof F.resume_text, r->text ? r->text : "");
    strcpy_s(F.resume_id, sizeof F.resume_id, r->id ? r->id : "");
    /* The resume parses the retained text through the detour; that parse must
     * not abandon the hold it belongs to. */
    if (r->text) {
        size_t n = 0;
        char *stripped = sh_modmanifest_prepare_load(r->text, strlen(r->text), &n);
        sh_modpublished_parse_seen(SH_MODPUB_PARSE_OTHER, r->text, (void *)0x5150, 1);
        if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    }
    return 1;
}
static void f_log(void *c, const char *line) { (void)c; if (getenv("MODPUB_TEST_LOG")) printf("    %s\n", line); }

static const sh_modpub_ops OPS = {
    NULL, f_on_main, f_bundle_state, f_rescan, f_activate, f_can_ask, f_ask, f_poll, f_release,
    f_worker_run, f_fetch, f_metadata_copy, f_metadata_free, f_metadata_ids, f_hold_token,
    f_owner_current, f_host_matches, f_hold_ui, f_drop_ui, f_resume, f_log
};

static void reset(void)
{
    if (F.worker) { WaitForSingleObject(F.worker, 5000); CloseHandle(F.worker); }
    sh_modpub_test_reset();
    memset(&F, 0, sizeof F);
    F.install_on_fetch = 1;
    F.host_ok = 1;
    F.owner_ok = 1;
    sh_modpub_test_set_ops(&OPS);
    sh_modmanifest_reset();
}

/* What the DeserializeFromJson detour does: the manifest step, the parse, the
 * report. */
static void parse(int kind, const char *text, void *map)
{
    size_t n = 0;
    char *stripped = sh_modmanifest_prepare_load(text, strlen(text), &n);
    sh_modpublished_parse_seen(kind, text, map, 1);
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
}

static void tick(int n)
{
    for (int i = 0; i < n; i++) { F.ticks++; sh_modpublished_poll(); }
}

/* Tick until the worker finished and the tick observed it. */
static void tick_until_idle(void)
{
    for (int i = 0; i < 2000 && sh_modpublished_holding(); i++) { tick(1); Sleep(1); }
}

#define MANAGER ((void *)0x1000)
#define MAP     ((void *)0x2000)
#define MODEL   ((void *)0x3000)
static const char METADATA[] = "1234abcd";   /* the double reads the item id from it */

/* ---- tests ---------------------------------------------------------------------------------- */

static void classification(void)
{
    static const unsigned char code[64];
    printf("-- the parse is classified by its return address\n");
    sh_modpub_test_set_read_sites(NULL, NULL);
    CHECK(sh_modpublished_classify(code + 0x12A) == SH_MODPUB_PARSE_OTHER);
    sh_modpub_test_set_read_sites(code + 10, code + 20);
    CHECK(sh_modpublished_classify(code + 10) == SH_MODPUB_PARSE_PUBLISHED_OFFLINE);
    CHECK(sh_modpublished_classify(code + 20) == SH_MODPUB_PARSE_PUBLISHED_ONLINE);
    CHECK(sh_modpublished_classify(code + 11) == SH_MODPUB_PARSE_OTHER);
    CHECK(sh_modpublished_classify(NULL) == SH_MODPUB_PARSE_OTHER);

    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    {
        sh_modpublished_parse_info info;
        CHECK(sh_modpublished_last_parse(&info) == SH_MODPUB_PARSE_PUBLISHED_ONLINE);
        CHECK(info.map == MAP && info.rows == 1 && info.missing == 1 && info.parsed == 1);
        CHECK(sh_modpublished_last_parse_published());
    }
    parse(SH_MODPUB_PARSE_OTHER, VANILLA, MAP);
    CHECK(!sh_modpublished_last_parse_published());
}

static void vanilla_and_installed_pass_through(void)
{
    printf("-- a vanilla map, and a map whose mods are installed, pass through untouched\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, VANILLA, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !F.hold_ui && !F.metadata_copies);
    tick(5);
    CHECK(F.asks == 0);

    reset();
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !F.hold_ui && !F.metadata_copies);
    tick(5);
    CHECK(F.asks == 0 && F.activations == 0 && F.resumes == 0);

    /* A completion whose map is not the last parse's object is not gated. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, (void *)0x9999, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding());

    /* A non-published parse never gates a published completion. */
    reset();
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding());
}

static void yes_resumes_with_a_fresh_map(void)
{
    printf("-- missing: hold, ask on a later tick, Yes fetches, activates and resumes\n");
    reset();
    F.can_ask_after = 3;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);
    F.installed_b = 1;   /* only alex.pink-imp is missing */
    {
        sh_modpublished_parse_info info;
        sh_modpublished_last_parse(&info);
        CHECK(info.missing == 2);   /* B became installed after the parse; the hold recounts */
    }
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.hold_ui == 1 && F.metadata_copies == 1);
    CHECK(F.asks == 0);                    /* never asked inside the completion */
    tick(1);
    CHECK(F.asks == 0 && sh_modpub_test_state() == 1);   /* the surface is still busy */
    tick(3);
    CHECK(F.asks == 1 && F.ask_gdm == 0x29 && F.ask_buttons == 6);
    CHECK(!strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    tick(3);
    CHECK(sh_modpub_test_state() == 2 && F.fetch_calls == 0);
    F.answer = 1;   /* Yes */
    tick_until_idle();
    CHECK(F.fetch_calls == 1);
    CHECK(F.rescans == 1 && F.activations == 1 && F.activated_count == 2);
    CHECK(!strcmp(F.activated[0], HASH_A) && !strcmp(F.activated[1], HASH_B));
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_ON_SNAP_PLAY);
    CHECK(!strcmp(F.resume_text, TWO_MODS));     /* the full text, rows included */
    CHECK(!strcmp(F.resume_id, METADATA));
    CHECK(F.metadata_frees == 1 && F.releases == 1 && F.drop_ui == 0);
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);

    /* The next completion of the same map passes: nothing is missing now. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
}

static void no_drops(void)
{
    printf("-- No drops the hold and names the mod\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1);
    F.answer = 2;   /* No */
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.resumes == 0 && F.activations == 0);
    CHECK(F.drop_ui == 1 && F.metadata_frees == 1);
    /* The notice is raised with a single Accept on the same tick or the next. */
    tick(2);
    CHECK(F.asks == 2 && F.ask_buttons == 0 && strstr(F.last_ask, "alex.pink-imp") &&
          strstr(F.last_ask, "not loaded"));
}

static void fetch_failure_drops(void)
{
    printf("-- a failed fetch, or a bundle that will not install, drops with a notice\n");
    reset();
    F.fetch_fails = 7;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.activations == 0 && F.drop_ui == 1);
    tick(2);
    CHECK(strstr(F.last_ask, "Could not download the mod alex.pink-imp") != NULL);

    reset();
    F.refuse_install = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.rescans == 1 && F.resumes == 0 && F.activations == 0);
    tick(2);
    CHECK(strstr(F.last_ask, "could not be installed: a file size differs") != NULL);
}

static void second_load_abandons(void)
{
    printf("-- another load abandons the hold, also mid-fetch\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1);
    parse(SH_MODPUB_PARSE_OTHER, VANILLA, (void *)0x7000);   /* the player opened something else */
    CHECK(!sh_modpublished_holding() && F.releases == 1 && F.metadata_frees == 1 && F.drop_ui == 0);
    tick(3);
    CHECK(F.asks == 1 && !sh_modpub_test_notice()[0]);

    reset();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick(1);
    CHECK(sh_modpub_test_state() == 3);
    for (int i = 0; i < 200 && !F.fetch_calls; i++) Sleep(1);
    /* A newer published launch replaces it: the old fetch is cancelled. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, (void *)0x7100);
    CHECK(!sh_modpublished_holding());
    WaitForSingleObject(F.worker, 5000);
    CHECK(F.fetch_cancelled == 1 && F.resumes == 0 && F.activations == 0);
    tick(2);   /* the retired job is freed once the worker returned */
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;
}

static void joiner_host_mismatch_drops(void)
{
    printf("-- a joiner re-checks the lobby host's map before resuming\n");
    /* A read of a map the lobby no longer hosts is not asked about. */
    reset();
    F.host_ok = 0;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.hold_ui == 0);

    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    F.host_ok = 0;   /* the host switched maps during the download */
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.activations == 0 && F.drop_ui == 1);
    tick(2);
    CHECK(strstr(F.last_ask, "lobby's map changed") != NULL);

    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_CACHE_READY);
}

static void warm_cache_joiner(void)
{
    printf("-- the warm-cache joiner branch uses the manifest seen for that item\n");
    reset();
    /* Never seen: not gated. */
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 1);
    /* A browser read of the item, declined. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1); F.answer = 2; tick(1); tick(2); F.answer = 0;
    CHECK(!sh_modpublished_holding());
    /* Joining that item's lobby with the map cached: held, asked, resumed by a read. */
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 0);
    CHECK(sh_modpublished_holding());
    tick(2);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_CACHE_LOBBY && !strcmp(F.resume_id, METADATA));
    /* Another revision of the item is a different map. */
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev2") == 1);
}

static void my_maps_play(void)
{
    printf("-- My Maps play: ask, fetch, resume with the model; a changed model abandons\n");
    reset();
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    CHECK(F.metadata_copies == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_LOCAL_OFFLINE && F.resume_extra == MODEL);
    CHECK(!strcmp(F.resume_text, ONE_MOD));

    reset();
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    F.token = 43;   /* the browser moved on to another saved map */
    tick_until_idle();
    CHECK(F.resumes == 0 && F.activations == 0 && !sh_modpublished_holding());

    /* No: return without the original, nothing else. */
    reset();
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1); F.answer = 2; tick(1);
    CHECK(F.resumes == 0 && !sh_modpublished_holding());
}

static void a_newer_launch_replaces(void)
{
    printf("-- a newer held launch replaces an unanswered one\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    /* Its own parse already abandons; the decide supersedes even without one. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, (void *)0x2100);
    CHECK(!sh_modpublished_holding() && F.metadata_frees == 1);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, (void *)0x2100, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "needs 2 mods: alex.pink-imp, bob.room (4.0 MB)"));
}

static void the_question(void)
{
    char text[256];
    const char *names[40];
    char storage[40][40];
    printf("-- the question fits the native body and summarizes what does not fit\n");
    sh_modpublished_compose_question(text, sizeof text, names, 0, 0);
    for (int i = 0; i < 40; i++) {
        _snprintf_s(storage[i], sizeof storage[i], _TRUNCATE, "author%02d.a-rather-long-mod-name", i);
        names[i] = storage[i];
    }
    sh_modpublished_compose_question(text, sizeof text, names, 1, 52428);
    CHECK(!strcmp(text, "This map needs 1 mod: author00.a-rather-long-mod-name (under 0.1 MB). "
                        "Download and install now?"));
    sh_modpublished_compose_question(text, sizeof text, names, 40, 40ull << 20);
    CHECK(strlen(text) < 256);
    CHECK(!strncmp(text, "This map needs 40 mods: author00.a-rather-long-mod-name, ", 57));
    CHECK(strstr(text, " more (40.0 MB). Download and install now?") != NULL);
    sh_modpublished_compose_question(text, 64, names, 40, 40ull << 20);
    CHECK(strlen(text) < 64);
}

int main(void)
{
    classification();
    vanilla_and_installed_pass_through();
    yes_resumes_with_a_fresh_map();
    no_drops();
    fetch_failure_drops();
    second_load_abandons();
    joiner_host_mismatch_drops();
    warm_cache_joiner();
    my_maps_play();
    a_newer_launch_replaces();
    the_question();
    reset();
    sh_modpub_test_set_ops(NULL);
    printf("modpublished_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
