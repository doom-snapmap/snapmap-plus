/* modpublished_test -- the published-map and offline-play gates: return-address
 * classification, pass-through for installed bundles, the hold, the question,
 * Yes (fetch on a worker, activation, resume with a fresh parse of the retained
 * text), No, a failed fetch, a bundle that will not install, a second load
 * abandoning the hold, the joiner's host re-check, the warm-cache joiner branch
 * and My Maps play; a parse whose classification another detour broke, the
 * SnapMap feed's play and the launch boundary; and the rawmap gate: the rows a rawmap names counted
 * under the asset rule (an installed folder of the row's id serving its
 * names, whatever its hash), the store check, the refusal that names each
 * mod and the download that reopens the rawmap.
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
#include "../src/backend/modnet.h"
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

/* The manifest's "MODS: map: <key>: <sentence>" lines (ADD-V5 B5). */
static int g_map_notes;
void backend_log(const char *line)
{
    if (line && !strncmp(line, "MODS: map: ", 11) && strstr(line, "is not installed on this computer")) g_map_notes++;
}
int sh_modserve_active(void) { return 1; }
/* The registry choices' input (modmanifest.c gives them at every load). */
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{ (void)slots; (void)keys; (void)n; (void)recompute; return 0; }

int modserve_activate_bundles(const char *const *bundle_hashes, int count)
{
    (void)bundle_hashes;
    return count;
}
int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out)
{
    (void)bundle_hash; (void)id;
    if (out) memset(out, 0, sizeof *out);
    return 0;
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
    int store_a, store_b;            /* store_has: 1 held, 0 absent, -1 unreachable */
    int editor_only_a;               /* the fetch finds alex.pink-imp editor-only: recorded, not installed */
    int recorded_a;                  /* the record's editorOnly section names alex.pink-imp's bundle */
    int store_calls;
    int other_a;                     /* another build of alex.pink-imp: 1 serves its names, 2 lacks one */
    int prints;
    char last_print[1024];
    char prints_all[8192];
    /* the gate */
    int probe_ops;                   /* 1: store_probe is bound (else store_has answers alone) */
    int game_lacks_a;                /* the store has alex.pink-imp, but its borrow names a file this game lacks */
    int claimed_a;                   /* a claim this machine holds names alex.pink-imp */
    int other_build_a;               /* alex.pink-imp is installed at another build (OTHER_A) */
    int fetch_detail_calls;
    int launch_refused, refused_hook; /* launch_refused: how often, and the last hook */
    /* the map a fetch is for */
    char fetch_title[160], fetch_map[96];
    int titled;                      /* metadata_title answers "Pink Arena" */
} fake;

static fake F;
static int g_metadata_cookie;

static int f_on_main(void *c) { (void)c; return 1; }
#define METADATA_ID "1234abcd"   /* METADATA below: the double reads the item id from it */
/* The asset rule as modserve.c answers it, over the fixture's two mods. */
static int f_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{
    const char *what = subject ? subject : "map";
    (void)c;
    if (cause && cap) cause[0] = 0;
    if (!strcmp(row->bundle, HASH_A) || !strcmp(row->id, "alex.pink-imp")) {
        if (F.installed_a || F.other_a == 1 || F.recorded_a) return SH_MODPUB_ROW_SERVED;
        if (F.other_a == 2) {
            if (cause) _snprintf_s(cause, cap, _TRUNCATE, "Pink Imp (alex.pink-imp) is installed but does not provide "
                                   "mods/alex.pink-imp/imp, which this %s uses.", what);
            return SH_MODPUB_ROW_INCOMPLETE;
        }
        if (cause) {
            if (F.refuse_install && F.fetch_calls)
                _snprintf_s(cause, cap, _TRUNCATE, "This %s uses alex.pink-imp, which is installed but cannot be used: "
                            "a file size differs.", what);
            else _snprintf_s(cause, cap, _TRUNCATE, "This %s uses alex.pink-imp, which is not installed.", what);
        }
        return SH_MODPUB_ROW_MISSING;
    }
    if (!strcmp(row->bundle, HASH_B)) {
        if (F.installed_b) return SH_MODPUB_ROW_SERVED;
        if (cause) _snprintf_s(cause, cap, _TRUNCATE, "This %s uses bob.room, which is not installed.", what);
    }
    return SH_MODPUB_ROW_MISSING;
}
static int f_rescan(void *c) { (void)c; F.rescans++; return 1; }
static int f_activate(void *c, const sh_modmanifest_list *rows)
{
    (void)c;
    F.activations++;
    F.activated_count = (int)rows->count;
    for (size_t i = 0; i < rows->count && i < 4; i++) strcpy_s(F.activated[i], sizeof F.activated[i], rows->rows[i].bundle);
    return (int)rows->count;
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
    if (F.editor_only_a && !strcmp(hash, HASH_A)) { F.recorded_a = 1; return 0; }   /* modsync: recorded, nothing installed */
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
static void f_log(void *c, const char *line)
{
    (void)c;
    if (getenv("MODPUB_TEST_LOG")) printf("    %s\n", line);
}
static void f_launch_refused(void *c, int hook) { (void)c; F.launch_refused++; F.refused_hook = hook; }
static int f_store_has(void *c, const char *hash, volatile int *cancel)
{
    (void)c; (void)cancel;
    F.store_calls++;
    if (!strcmp(hash, HASH_A)) return F.store_a;
    if (!strcmp(hash, HASH_B)) return F.store_b;
    return 0;
}
static void f_print(void *c, const char *line)
{
    (void)c;
    F.prints++;
    strcpy_s(F.last_print, sizeof F.last_print, line);
    strncat_s(F.prints_all, sizeof F.prints_all, line, _TRUNCATE);
    strncat_s(F.prints_all, sizeof F.prints_all, "\n", _TRUNCATE);
}

#define OTHER_A "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define ROCK    "generated/image/stock/rock.bimage"
static int f_store_probe(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out)
{
    memset(out, 0, sizeof *out);
    out->found = f_store_has(c, hash, cancel);
    if (!strcmp(hash, HASH_A) && out->found > 0) {
        strcpy_s(out->name, sizeof out->name, "Pink Imp");
        if (F.game_lacks_a) { out->game_lacks = 1; strcpy_s(out->source, sizeof out->source, ROCK); }
    }
    return out->found;
}
static int f_claimed(void *c, const char *hash) { (void)c; return F.claimed_a && !strcmp(hash, HASH_A); }
static int f_other_build(void *c, const sh_modmanifest_row *row, char *hash, size_t hash_cap, char *name, size_t name_cap)
{
    (void)c;
    if (!F.other_build_a || strcmp(row->id, "alex.pink-imp")) return 0;
    strcpy_s(hash, hash_cap, OTHER_A);
    strcpy_s(name, name_cap, "Pink Imp");
    return 1;
}
static int f_mod_name(void *c, const sh_modmanifest_row *row, char *out, size_t cap)
{
    (void)c; (void)row; (void)out; (void)cap;
    return 0;   /* nothing installed names it */
}
static int f_fetch_detail(void *c, const char *hash, char *out, size_t cap)
{
    (void)c; (void)hash;
    F.fetch_detail_calls++;
    strcpy_s(out, cap, ROCK);
    return 1;
}

static int f_fetch_for(void *c, const char *hash, const char *title, const char *map_id, sh_modpub_progress_fn progress,
                       volatile int *cancel)
{
    strcpy_s(F.fetch_title, sizeof F.fetch_title, title ? title : "");
    strcpy_s(F.fetch_map, sizeof F.fetch_map, map_id ? map_id : "");
    return f_fetch(c, hash, progress, cancel);
}
static int f_metadata_title(void *c, const void *metadata, char *out, size_t cap)
{
    (void)c; (void)metadata;
    if (!F.titled) return 0;
    strcpy_s(out, cap, "Pink Arena");
    return 1;
}

static const sh_modpub_ops OPS = {
    NULL, f_on_main, f_row_state, f_rescan, f_activate, f_can_ask, f_ask, f_poll, f_release,
    f_worker_run, f_fetch, f_metadata_copy, f_metadata_free, f_metadata_ids, f_hold_token,
    f_owner_current, f_host_matches, f_hold_ui, f_drop_ui, f_resume, f_log,
    f_store_has, f_print, f_store_probe, f_claimed, f_other_build, f_mod_name,
    f_fetch_detail, f_launch_refused, f_fetch_for, f_metadata_title
};

static void reset(void)
{
    if (F.worker) { WaitForSingleObject(F.worker, 5000); CloseHandle(F.worker); }
    sh_modpub_test_reset();
    memset(&F, 0, sizeof F);
    F.install_on_fetch = 1;
    F.host_ok = 1;
    F.owner_ok = 1;
    F.store_a = F.store_b = 1;
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

/* One main-thread tick. Every hold asks the store first (the denial gate's
 * probe, on the worker): a tick that finds the hold probing waits for the
 * probe and then ticks once more, which is where the question used to be
 * raised, so "the next tick asks" still reads as one tick. */
static void tick(int n)
{
    for (int i = 0; i < n; i++) {
        int probed = 0;
        F.ticks++;
        sh_modpublished_poll();
        for (int k = 0; k < 4000 && sh_modpub_test_state() == 4; k++) { Sleep(1); sh_modpublished_poll(); probed = 1; }
        if (probed) sh_modpublished_poll();
    }
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

    /* The asset rule: another build of the row's id that provides its names
     * plays, whatever its hash; nothing is asked. */
    reset();
    F.other_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !F.hold_ui);
    tick(5);
    CHECK(F.asks == 0);

    /* A folder of the id that lacks a listed name goes to the store offer. */
    reset();
    F.other_a = 2;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding());
    CHECK(strstr(F.last_print, "is installed but does not provide mods/alex.pink-imp/imp, which this map uses.") != NULL);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "Download and install now?") != NULL);
    F.answer = 2;   /* No: the launch is dropped with the asset rule's sentence */
    tick(3);
    CHECK(!sh_modpublished_holding());
    CHECK(!strcmp(F.last_ask, "Pink Imp (alex.pink-imp) is installed but does not provide mods/alex.pink-imp/imp, which "
                              "this map uses. The map was not loaded."));

    /* A completion whose map is not the last parse's object is not gated. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, (void *)0x9999, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding());

    /* A record from an earlier frame is some other load's: not gated. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    tick(1);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding());
}

static void unrecognized_parse_is_still_gated(void)
{
    printf("-- a parse whose return address another detour changed is still gated by its map object\n");
    /* Observed live: with a second detour on DeserializeFromJson that rewrites
     * the return address, every published read classified as OTHER and the
     * browser completion passed a map whose six mods were missing. */
    reset();
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.hold_ui == 1 && F.metadata_copies == 1);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "This map needs 1 mod: alex.pink-imp") != NULL);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_BROWSER_PLAY && !strcmp(F.resume_text, ONE_MOD));

    /* Installed: it passes, unchanged. */
    reset();
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !F.hold_ui);
}

static void feed_play_is_gated(void)
{
    printf("-- SnapMap feed play is held and resumed like the other completions\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_FEED_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.metadata_copies == 1);
    tick(1);
    CHECK(F.asks == 1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.activations == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_FEED_PLAY);
    CHECK(!strcmp(F.resume_id, METADATA) && !strcmp(F.resume_text, ONE_MOD));
    /* Its launch, after the resume, passes the boundary: the record was consumed. */
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
}

static void launch_boundary(void)
{
    printf("-- the launch boundary holds or refuses a map that no gate saw\n");
    /* A completion this module does not know: the parse is followed by the
     * lobby launch in the same frame. Held, asked, resumed with the flag. */
    reset();
    F.token = 7;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, (void *)1) == 0);
    CHECK(sh_modpublished_holding() && F.metadata_copies == 0);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "This map needs 2 mods: alex.pink-imp, bob.room") != NULL);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 2 && F.rescans == 1 && F.activations == 1 && F.activated_count == 2);
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_LOBBY_LAUNCH && F.resume_extra == (void *)1);
    CHECK(!strcmp(F.resume_text, TWO_MODS));
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);

    /* The screen it was held on is gone by the time the mods are in: abandoned. */
    reset();
    F.token = 7;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    F.token = 8;
    tick_until_idle();
    CHECK(F.resumes == 0 && F.activations == 0 && !sh_modpublished_holding());

    /* No: nothing launches; the notice names the mod. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 0);
    tick(1); F.answer = 2; tick(1);
    CHECK(F.resumes == 0 && F.drop_ui == 1 && !sh_modpublished_holding());

    /* An offline launch has no object to resume with: refused with the notice. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, MAP, NULL) == 0);
    CHECK(!sh_modpublished_holding() && F.drop_ui == 1 && F.asks == 0);
    CHECK(!strcmp(sh_modpub_test_notice(), "This map needs mods that could not be prepared. The map was not loaded."));
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 0);

    /* Installed mods, a parse a gate already decided, a parse of an earlier
     * frame (the lobby's relaunch from the cache) and another map object all
     * launch unchanged. */
    reset();
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
    CHECK(F.asks == 0 && sh_modpublished_holding());   /* only the gate's hold */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    tick(1);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, (void *)0x9999, NULL) == 1);
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, VANILLA, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
    /* decide never answers for the boundary's hooks. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, MAP, NULL, NULL, NULL) == 1);
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

static void downloads_are_for_a_map(void)
{
    sh_modmanifest_list rows;
    printf("-- a fetch is told the map it is for: its title and item\n");
    /* A stranger's map: fetched for its title and item, so the build lands
     * in mods\\downloads\\<title>\\ (modsync_fetch_bundle_for). */
    reset();
    F.titled = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && !strcmp(F.fetch_title, "Pink Arena") && !strcmp(F.fetch_map, METADATA_ID));
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_current(&rows) == 1 && !strcmp(rows.rows[0].bundle, HASH_A));   /* its pin is kept */
    sh_modmanifest_list_free(&rows);
}

static void editor_only_row_plays_without_a_folder(void)
{
    printf("-- a row whose bundle changes only the editor: fetched, recorded, not installed; the map resumes\n");
    reset();
    F.editor_only_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding());
    tick(3);
    CHECK(F.asks == 1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.installed_a == 0 && F.recorded_a == 1);
    CHECK(F.rescans == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_BROWSER_PLAY);
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);

    /* Every later launch of a map naming it passes: the record serves the row. */
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding());
    tick(3);
    CHECK(F.asks == 1 && F.fetch_calls == 1);
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
    CHECK(strstr(F.last_ask, "could not be installed. This map uses alex.pink-imp, which is installed but cannot be used: "
                             "a file size differs.") != NULL);
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

    printf("-- the same map launched again mid-fetch is held with the first; the download goes on\n");
    reset();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick(1);
    CHECK(sh_modpub_test_state() == 3);
    for (int i = 0; i < 200 && !F.fetch_calls; i++) Sleep(1);
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, (void *)0x7200);   /* Play pressed again */
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 3);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, (void *)0x7200, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.asks == 1 && F.hold_ui == 1);
    SetEvent(F.fetch_gate);
    tick_until_idle();
    CHECK(F.fetch_cancelled == 0 && F.fetch_calls == 1 && F.resumes == 1 && F.installed_a);
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

/* Tick until the store check has finished and the hold moved on. */
static void tick_until_probed(void)
{
    for (int i = 0; i < 4000 && sh_modpub_test_state() == 4; i++) { F.ticks++; sh_modpublished_poll(); Sleep(1); }
}

static void rawmap_missing_counts_rows(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap's rows are counted under the asset rule\n");
    reset();
    CHECK(sh_modpub_rawmap_missing(VANILLA, strlen(VANILLA), &rows) == 0 && rows.count == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 2 && rows.count == 2);
    sh_modmanifest_list_free(&rows);
    F.installed_b = 1;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 1);
    sh_modmanifest_list_free(&rows);
    F.other_a = 2;                       /* a folder of the id that lacks a listed name: still missing */
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 1);
    sh_modmanifest_list_free(&rows);
    F.other_a = 1;                       /* another build of the id that serves the names: nothing to ask */
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 0);
    sh_modmanifest_list_free(&rows);
    F.other_a = 0;
    /* Not a published-map parse: the rawmap check never touches the parse record. */
    CHECK(sh_modpublished_last_parse(NULL) == -1);
    /* Without bound gates nothing is missing: the rawmap loads as it always did. */
    sh_modpub_test_set_ops(NULL);
    F.installed_a = F.installed_b = 0;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 0);
    sh_modmanifest_list_free(&rows);
    sh_modpub_test_set_ops(&OPS);
    /* Never a hook a detour can ask about. */
    CHECK(sh_modpub_decide(SH_MODPUB_RAWMAP_LOAD, MANAGER, NULL, MAP, NULL, NULL, NULL) == 1);
}

static void rawmap_store_lacks_refuses(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap whose mods the store lacks is denied: cause 3, never published\n");
    reset();
    F.store_b = 0;                       /* bob.room was never published */
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 2);
    CHECK(sh_modpub_rawmap_hold(&rows, "shared.json") == 1);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 4 && F.asks == 0);
    /* One line per row the asset rule names, then the store check. */
    CHECK(F.prints == 3 && strstr(F.last_print, "Rawmap shared.json needs 2 mods you do not have") != NULL);
    tick_until_probed();
    CHECK(F.store_calls == 2);           /* both asked, so the denial can name every one */
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.resumes == 0 && F.activations == 0);
    CHECK(!strcmp(F.last_print, "This map needs bob.room, which is not installed and was never published. "
                                "Restore its folder from the Recycle Bin or load the map's rawmap ZIP."));
    tick(2);
    CHECK(F.asks == 1 && F.ask_buttons == 0);
    CHECK(!strcmp(F.last_ask, "This map needs bob.room, which is not installed and was never published. "
                              "Restore its folder from the Recycle Bin or load the map's rawmap ZIP."));

    printf("-- a store that cannot be reached: cause 2, nothing downloaded\n");
    reset();
    F.store_a = -1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "one.zip") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_probed();
    tick(2);
    CHECK(F.fetch_calls == 0 && F.resumes == 0 && !sh_modpublished_holding());
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which could not be downloaded: the mod store did "
                              "not answer; try again in a few minutes."));
    CHECK(!strcmp(F.last_print, F.last_ask));
}

static void rawmap_store_has_downloads_and_reopens(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap whose mods the store has asks as My Maps does, then downloads and reopens\n");
    reset();
    F.installed_b = 1;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_probed();
    CHECK(F.store_calls == 1 && sh_modpub_test_state() == 1);
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 6);
    CHECK(!strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.rescans == 1);
    CHECK(!strcmp(F.fetch_title, "rawmap") && !F.fetch_map[0]);   /* mods\downloads\rawmap\ */
    CHECK(F.activations == 0);           /* the reopened rawmap's own parse activates */
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_RAWMAP_LOAD && !strcmp(F.resume_id, "rawmap.json"));
    CHECK(!F.resume_text[0] && F.metadata_copies == 0 && F.hold_ui == 0);

    printf("-- No leaves the rawmap closed and names the mod\n");
    reset();
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_probed();
    tick(1);
    F.answer = 2;
    tick(3);
    CHECK(F.fetch_calls == 0 && F.resumes == 0);
    CHECK(!strcmp(F.last_ask, "This map uses alex.pink-imp, which is not installed. The map was not loaded."));

    printf("-- nothing missing after all: no hold\n");
    reset();
    F.installed_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 0);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 0 && !sh_modpublished_holding());
    sh_modmanifest_list_free(&rows);
}

/* ---- the denial gate (DESIGN-additive 2.8) ------------------------------------------------- */

/* The author's map in the editor: a declared build that is gone while the
 * key is installed at another build substitutes (2.5); with no build of the
 * key it is the same cause as everywhere; Play and the hub keep the gate. */
static void gate_editor_substitutes(void)
{
    sh_modpub_gate_facts f;
    sh_modmanifest_row row;
    char hash[80], why[600];
    printf("-- the gate: the editor opens the author's map with the installed build of a key\n");
    memset(&f, 0, sizeof f);
    f.editor = 1;
    f.store = SH_MODPUB_STORE_ABSENT;
    f.other_build = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);
    f.store = SH_MODPUB_STORE_UNREACHABLE;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);
    f.other_build = 0;
    f.store = SH_MODPUB_STORE_ABSENT;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_NEVER_PUBLISHED);   /* the key is not installed: denied */
    f.editor = 0;
    f.other_build = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_NEVER_PUBLISHED);   /* Play, the hub: denied as before */
    /* The manifest's op: the other build's hash, or the cause. */
    reset();
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "alex.pink-imp");
    strcpy_s(row.bundle, sizeof row.bundle, HASH_A);
    F.other_build_a = 1;
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, why, sizeof why) == 1 && !strcmp(hash, OTHER_A) && !why[0]);
    F.other_build_a = 0;
    /* The store is not asked at an editor open: the note says what this
     * computer knows, never "permanent" or "never published". */
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, why, sizeof why) == 0 && !hash[0] &&
          !strcmp(why, "This map needs alex.pink-imp, which is not installed on this computer, so the editor "
                       "opens the map without it. Playing the map asks the mod store for it."));
    F.claimed_a = 1;
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, why, sizeof why) == 0 &&
          strstr(why, "is not installed on this computer") && !strstr(why, "permanent") && !strstr(why, "Recycle Bin"));
    /* The map places something of the mod: the engine's validator refuses it, so the
     * sentence says the editor cannot open the map and what to do, and fits the notice. */
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 1, why, sizeof why) == 0 && !hash[0] &&
          !strcmp(why, "This map needs alex.pink-imp, which is not installed on this computer, so the editor cannot "
                       "open the map. Restore the mod's folder from the Recycle Bin, or play the map from My Maps to "
                       "get it from the mod store.") && strlen(why) < 256);
    /* A placed mod with an installed build of its key still substitutes. */
    F.other_build_a = 1;
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 1, why, sizeof why) == 1 && !strcmp(hash, OTHER_A));
    F.other_build_a = 0;
    reset();
}

/* The editor opens a My Maps map whose mod is gone (V13's "V13 crate", live 7491a99): with
 * an entity of the mod placed, the engine refused the map with its damaged-save dialog and
 * the player was not told why. The parse no gate takes now raises one plain notice; a map
 * that only declares the mod opens without it and raises none. */
static void editor_open_missing_placed_mod(void)
{
    static const char PLACED[] =
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"mods/alex.pink-imp/imp\"}}],\"instances\":[],"
        "\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
        VAR("door_label", "Open") "," VAR("smpmod1.0.1", ROW("alex.pink-imp", HASH_A, "3145728")) "]}}";
    printf("-- the editor cannot open a map that places a mod not installed: one plain notice\n");
    reset();
    g_map_notes = 0;
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, PLACED, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(!sh_modpub_test_notice()[0]);
    tick(1);
    CHECK(g_map_notes == 1);
    tick(2);
    CHECK(F.asks == 1 && F.ask_buttons == 0 && !strcmp(F.last_ask, "This map needs alex.pink-imp, which is not installed on this computer, so "
                                             "the editor cannot open the map. Restore the mod's folder from the Recycle "
                                             "Bin, or play the map from My Maps to get it from the mod store."));
    reset();
    /* Declared only (Use in this map): the editor opens it without the mod; no notice. */
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    sh_modmanifest_set_editor_load(0);
    tick(3);
    CHECK(F.asks == 0 && !sh_modpub_test_notice()[0]);
    reset();
    /* A gate that takes the parse (a published play) says its own cause: no editor notice. */
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, PLACED, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(!sh_modmanifest_load_refusal(NULL, 0));
    reset();
}

/* The cause sentence an editor-flagged parse finds for a row nothing serves
 * is said only after the gate: a published play the parse detour did not
 * recognise (the editor flag set) is gated, so it is never said; a parse no
 * gate takes (an editor open) says it once, at the next tick. */
static void gate_notes_after_the_decision(void)
{
    printf("-- the gate: a row's cause is logged after the gate's decision, once\n");
    reset();
    g_map_notes = 0;
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(g_map_notes == 0 && sh_modmanifest_load_notes_pending() == 1);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modmanifest_load_notes_pending() == 0);
    tick(3);
    CHECK(g_map_notes == 0);
    reset();
    /* An editor open: no gate takes the parse. */
    g_map_notes = 0;
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(g_map_notes == 0);
    tick(1);
    CHECK(g_map_notes == 1 && sh_modmanifest_load_notes_pending() == 0);
    tick(2);
    CHECK(g_map_notes == 1);
    reset();
}

static void gate_decision_and_wording(void)
{
    sh_modpub_gate_facts f;
    char text[1024];
    printf("-- the gate: one decision, four causes\n");
    memset(&f, 0, sizeof f);
    f.store = SH_MODPUB_STORE_HAS;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_OBTAIN);
    f.other_build = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_OBTAIN);   /* another build installed: obtainable */
    f.game_lacks = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_GAME_REVISION);
    memset(&f, 0, sizeof f);
    f.store = SH_MODPUB_STORE_ABSENT;
    f.published = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_GONE);
    f.published = 0;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_NEVER_PUBLISHED);
    f.store = SH_MODPUB_STORE_UNREACHABLE;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_UNREACHABLE);
    f.published = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_UNREACHABLE);
    CHECK(sh_modpub_gate_decide(NULL) == SH_MODPUB_GATE_UNREACHABLE);

    sh_modpub_gate_wording(SH_MODPUB_GATE_GONE, "map", "Hell Guards (hell-guards-7f3k)", NULL, 0, text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which is no longer available: its "
                        "author unpublished it, or it was removed. This is permanent on this machine."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_UNREACHABLE, "map", "Hell Guards (hell-guards-7f3k)", NULL, 0, text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which could not be downloaded: the mod "
                        "store did not answer; try again in a few minutes."));
    /* Worded by the store's failure (plain_reason.h), not one sentence for all. */
    sh_modpub_gate_wording(SH_MODPUB_GATE_UNREACHABLE, "map", "Hell Guards (hell-guards-7f3k)", NULL, MODNET_E_UNAVAILABLE,
                           text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which could not be downloaded: the mod "
                        "store is busy; try again in a few minutes."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_UNREACHABLE, "map", "Hell Guards (hell-guards-7f3k)", NULL, MODNET_E_NET,
                           text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which could not be downloaded: the mod "
                        "store could not be reached; check the internet connection and try again."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_UNREACHABLE, "map", "x", NULL, MODNET_E_HTTP, text, sizeof text);
    CHECK(strstr(text, "the address in mods.store answered, but not as the mod store; check the mods.store setting") != NULL);
    sh_modpub_gate_wording(SH_MODPUB_GATE_NEVER_PUBLISHED, "map", "Hell Guards (hell-guards-7f3k)", NULL, 0, text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which is not installed and was never "
                        "published. Restore its folder from the Recycle Bin or load the map's rawmap ZIP."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_GAME_REVISION, "map", "Hell Guards (hell-guards-7f3k)", ROCK, 0, text, sizeof text);
    CHECK(!strcmp(text, "This map needs Hell Guards (hell-guards-7f3k), which needs a file this game does not "
                        "hold (generated/image/stock/rock.bimage): a different game revision. This is permanent on "
                        "this machine."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_NEVER_PUBLISHED, "prefab", "door-kit", NULL, 0, text, sizeof text);
    CHECK(!strcmp(text, "This prefab needs door-kit, which is not installed and was never published. Restore "
                        "its folder from the Recycle Bin or load the prefab's ZIP export. The prefab was not placed."));
    sh_modpub_gate_wording(SH_MODPUB_GATE_OBTAIN, "map", "x", NULL, 0, text, sizeof text);
    CHECK(!text[0]);
    if (getenv("MODPUB_TEST_WORDS")) {
        int v;
        for (v = 1; v <= 4; v++) {
            sh_modpub_gate_wording(v, "map", "Hell Guards (hell-guards-7f3k)", ROCK, 0, text, sizeof text);
            printf("    cause %d: %s\n", v, text);
        }
    }
}

/* Hold a published play of ONE_MOD, run the probe, and return the notice the
 * player is shown (ticked until the one-button notice is raised). */
static void published_denial(int hook)
{
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(hook, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 4 && F.asks == 0);
    tick(3);
}

static void gate_causes_through_the_hold(void)
{
    sh_modmanifest_list rows;
    printf("-- cause 1: a published map whose bundle left the store; never asked, permanent\n");
    reset();
    F.probe_ops = 1;
    F.store_a = 0;
    published_denial(SH_MODPUB_ON_SNAP_PLAY);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.drop_ui == 1 && F.metadata_frees == 1);
    CHECK(F.asks == 1 && F.ask_buttons == 0);
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which is no longer available: its author "
                              "unpublished it, or it was removed. This is permanent on this machine."));
    CHECK(!strcmp(F.last_print, F.last_ask));
    /* A joiner's check words it the same (the lobby path holds the host's map). */
    reset();
    F.store_a = 0;
    published_denial(SH_MODPUB_CACHE_READY);
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which is no longer available: its author "
                              "unpublished it, or it was removed. This is permanent on this machine."));
    /* A My Maps map published from this machine (a claim names the bundle). */
    reset();
    F.store_a = 0;
    F.claimed_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(3);
    CHECK(strstr(F.last_ask, "is no longer available: its author unpublished it, or it was removed.") != NULL);

    printf("-- cause 2: the store did not answer\n");
    reset();
    F.store_a = -1;
    published_denial(SH_MODPUB_BROWSER_PLAY);
    CHECK(F.fetch_calls == 0 && !sh_modpublished_holding());
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which could not be downloaded: the mod store did "
                              "not answer; try again in a few minutes."));
    reset();
    F.store_a = -1;
    published_denial(SH_MODPUB_CACHE_READY);   /* a joiner: the same sentence */
    CHECK(strstr(F.last_ask, "could not be downloaded: the mod store did not answer; try again in a few minutes.") != NULL);

    printf("-- cause 3: a My Maps map never published whose folder is gone\n");
    reset();
    F.store_a = 0;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    CHECK(F.launch_refused == 0);   /* held: not refused yet */
    tick(3);
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which is not installed and was never published. "
                              "Restore its folder from the Recycle Bin or load the map's rawmap ZIP."));
    /* The denial refused the launch: its parse's rows must not stay declared (ADD-V4a H3). */
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_LOCAL_OFFLINE && F.drop_ui == 1);
    /* A prefab's placement: its own wording. */
    reset();
    F.store_a = 0;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 1);
    CHECK(sh_modpub_prefab_hold(&rows, "Door Kit") == 1);
    sh_modmanifest_list_free(&rows);
    tick(3);
    CHECK(!strcmp(F.last_ask, "This prefab needs alex.pink-imp, which is not installed and was never "
                              "published. Restore its folder from the Recycle Bin or load the prefab's ZIP export. The "
                              "prefab was not placed."));
    CHECK(F.drop_ui == 1 && F.launch_refused == 0);   /* a placement is not a launch */

    printf("-- cause 4: the store has it, but a borrow names a file this game does not hold\n");
    reset();
    F.game_lacks_a = 1;
    published_denial(SH_MODPUB_ON_SNAP_PLAY);
    CHECK(F.fetch_calls == 0 && !sh_modpublished_holding());
    CHECK(!strcmp(F.last_ask, "This map needs Pink Imp (alex.pink-imp), which needs a file this game does not "
                              "hold (generated/image/stock/rock.bimage): a different game revision. This is permanent "
                              "on this machine."));
    CHECK(!strcmp(F.last_print, F.last_ask));

    printf("-- the fetch's own failure is decided by the same gate\n");
    reset();
    F.fetch_fails = -11;                 /* MODNET_E_NOT_FOUND: purged between the probe and the fetch */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1);
    F.answer = 1;
    tick_until_idle();
    tick(2);
    CHECK(F.resumes == 0 && strstr(F.last_ask, "is no longer available: its author unpublished it") != NULL);
    reset();
    F.fetch_fails = -18;                 /* MODNET_E_GAME */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_detail_calls == 1 && strstr(F.last_ask, "(generated/image/stock/rock.bimage): a different game revision.") != NULL);
    reset();
    F.fetch_fails = -4;                  /* MODNET_E_NET */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    tick(2);
    CHECK(strstr(F.last_ask, "the mod store could not be reached; check the internet connection and try again.") != NULL);

    printf("-- several denied: the first in the dialog, one console line each\n");
    reset();
    F.store_a = 0;
    F.store_b = -1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(3);
    CHECK(strstr(F.last_ask, "This map needs alex.pink-imp, which is no longer available") == F.last_ask);
    CHECK(strstr(F.last_ask, "1 more mod cannot be obtained; see the console.") != NULL);
    CHECK(strstr(F.prints_all, "This map needs bob.room, which could not be downloaded") != NULL);
}

static void gate_other_build_prompt(void)
{
    printf("-- installed at another build: obtainable; the ordinary question, no build talk\n");
    reset();
    F.other_build_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 6);
    CHECK(!strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    CHECK(!strstr(F.last_ask, "build") && !strstr(F.last_ask, "beside"));
    if (getenv("MODPUB_TEST_WORDS")) printf("    other build: %s\n", F.last_ask);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1);
    /* With another missing mod, the list names the mods alone. */
    reset();
    F.other_build_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(!strcmp(F.last_ask, "This map needs 2 mods: alex.pink-imp, bob.room (4.0 MB). Download and install now?"));
}

/* The player's own content (DESIGN-additive 2.8 as amended by ADD-L13): a My
 * Maps play, a rawmap load or a prefab placement whose declared build cannot
 * be obtained, while its key is installed at another build, goes on with that
 * build, as the editor does; a community map from the browser still denies. */
static int g_ready_calls;
static char g_ready_name[96];
static void f_prefab_ready(const char *name, const sh_modmanifest_list *rows)
{
    (void)rows;
    g_ready_calls++;
    strcpy_s(g_ready_name, sizeof g_ready_name, name ? name : "");
}

#define SUBST_LINE_A "alex.pink-imp: uses the installed mod"

static void gate_own_substitutes(void)
{
    sh_modpub_gate_facts f;
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    char hash[80];
    printf("-- the gate: the player's own content substitutes the installed build of a key it cannot obtain\n");
    memset(&f, 0, sizeof f);
    f.own = 1;
    f.other_build = 1;
    f.store = SH_MODPUB_STORE_ABSENT;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);
    f.published = 1;                                                   /* published from here, then gone */
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);
    f.published = 0;
    f.store = SH_MODPUB_STORE_UNREACHABLE;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);
    f.store = SH_MODPUB_STORE_HAS;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_OBTAIN);         /* obtainable: fetched beside */
    f.game_lacks = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_SUBSTITUTE);     /* listed but not installable here */
    /* Nothing of the key installed: the four causes, as everywhere. */
    memset(&f, 0, sizeof f);
    f.own = 1;
    f.store = SH_MODPUB_STORE_ABSENT;
    f.published = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_GONE);
    f.published = 0;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_NEVER_PUBLISHED);
    f.store = SH_MODPUB_STORE_UNREACHABLE;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_UNREACHABLE);
    f.store = SH_MODPUB_STORE_HAS;
    f.game_lacks = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_GAME_REVISION);
    /* A community map (not own): the author's build or cause 1. */
    memset(&f, 0, sizeof f);
    f.other_build = 1;
    f.published = 1;
    f.store = SH_MODPUB_STORE_ABSENT;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_GONE);

    printf("-- the gate: a My Maps play substitutes, says so, and the Play serves the installed build\n");
    reset();
    F.store_a = 0;
    F.other_build_a = 1;
    sh_modmanifest_set_editor_load(1);                 /* the parse detour: a read that is not a published map's */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(3);
    CHECK(!sh_modpublished_holding() && F.resumes == 1 && F.resume_hook == SH_MODPUB_LOCAL_OFFLINE);
    CHECK(F.asks == 0 && F.fetch_calls == 0 && F.launch_refused == 0 && F.drop_ui == 0 && F.store_calls == 1);
    CHECK(strstr(F.prints_all, "My Maps play: " SUBST_LINE_A "; the next save of the map records it") != NULL);
    if (getenv("MODPUB_TEST_WORDS")) printf("    substitute: %s", F.prints_all);
    CHECK(F.activations == 1);
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "alex.pink-imp");
    strcpy_s(row.bundle, sizeof row.bundle, HASH_A);
    CHECK(sh_modpub_row_substitute(&row, hash, sizeof hash) == 1 && !strcmp(hash, OTHER_A));
    /* The launched map's listing declares the installed build (modui's
     * mu_listing_set reads the manifest's substitution). */
    CHECK(sh_modmanifest_substituted(HASH_A, hash, sizeof hash) == 1 && !strcmp(hash, OTHER_A));
    /* The same map again this session: no hold, no store check. */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.store_calls == 1);
    /* The installed build changed: the substitution no longer holds. */
    F.other_build_a = 0;
    CHECK(sh_modpub_row_substitute(&row, hash, sizeof hash) == 0 && !hash[0]);
    F.other_build_a = 1;
    /* A community map naming the same build, from the browser: cause 1. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(3);
    CHECK(F.resumes == 1 && F.launch_refused == 1 && F.refused_hook == SH_MODPUB_BROWSER_PLAY);
    CHECK(!strcmp(F.last_ask, "This map needs Pink Imp (alex.pink-imp), which is no longer available: its "
                              "author unpublished it, or it was removed. This is permanent on this machine."));
    sh_modmanifest_set_editor_load(0);

    /* The store did not answer: the player's own map still plays. */
    reset();
    F.store_a = -1;
    F.other_build_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(3);
    CHECK(F.resumes == 1 && F.asks == 0 && F.launch_refused == 0);
    /* The store has the declared build: it is offered with the ordinary question. */
    reset();
    F.other_build_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1 && !strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    /* ...and when that fetch then fails (the build left the store), it substitutes. */
    F.fetch_fails = -11;
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.launch_refused == 0 && strstr(F.prints_all, SUBST_LINE_A) != NULL);
    /* Two mods: one substituted, the other offered alone and fetched. */
    reset();
    F.other_build_a = 1;
    F.store_a = 0;
    parse(SH_MODPUB_PARSE_OTHER, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "bob.room") != NULL && !strstr(F.last_ask, "alex.pink-imp"));
    if (getenv("MODPUB_TEST_WORDS")) printf("    mixed: %s\n", F.last_ask);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.installed_b && F.activated_count == 2);
    /* A substitution with another mod nothing serves: denied for that one alone. */
    reset();
    F.other_build_a = 1;
    F.store_a = 0;
    F.store_b = 0;
    parse(SH_MODPUB_PARSE_OTHER, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(3);
    CHECK(F.resumes == 0 && F.launch_refused == 1 && strstr(F.last_ask, "This map needs bob.room, which is not "
                                                                        "installed and was never published.") == F.last_ask);
    CHECK(!strstr(F.last_ask, "more mod"));

    printf("-- the gate: a rawmap load and a prefab placement substitute the same way\n");
    reset();
    F.store_a = 0;
    F.other_build_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick(3);
    CHECK(!sh_modpublished_holding() && F.resumes == 1 && F.resume_hook == SH_MODPUB_RAWMAP_LOAD && F.asks == 0);
    CHECK(strstr(F.prints_all, "rawmap load: " SUBST_LINE_A) != NULL);
    /* Its reopen finds the row served. */
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 0);
    sh_modmanifest_list_free(&rows);
    reset();
    g_ready_calls = 0;
    sh_modpub_set_prefab_ready(f_prefab_ready);
    F.store_a = 0;
    F.other_build_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 1);
    CHECK(sh_modpub_prefab_hold(&rows, "Door Kit") == 1);
    sh_modmanifest_list_free(&rows);
    tick(3);
    CHECK(!sh_modpublished_holding() && g_ready_calls == 1 && !strcmp(g_ready_name, "Door Kit") && F.resumes == 0);
    CHECK(F.drop_ui == 0 && strstr(F.prints_all, "prefab placement 'Door Kit': " SUBST_LINE_A
                                                 "; the placement adds it to the map") != NULL);
    reset();
}

static void the_refusal(void)
{
    char text[256];
    const char *names[40];
    char storage[40][48];
    printf("-- the refusal fits the native body\n");
    for (int i = 0; i < 40; i++) {
        _snprintf_s(storage[i], sizeof storage[i], _TRUNCATE, "author%02d.a-rather-long-mod-name 1.0.%d", i, i);
        names[i] = storage[i];
    }
    sh_modpublished_compose_refusal(text, sizeof text, names, 40, 0);
    CHECK(strlen(text) < 256 && strstr(text, " more. Ask the map's author for its .zip export.") != NULL);
    sh_modpublished_compose_refusal(text, sizeof text, names, 40, 1);
    CHECK(strlen(text) < 256 && strstr(text, "could not be reached") != NULL);
}

int main(void)
{
    classification();
    vanilla_and_installed_pass_through();
    unrecognized_parse_is_still_gated();
    feed_play_is_gated();
    launch_boundary();
    yes_resumes_with_a_fresh_map();
    downloads_are_for_a_map();
    editor_only_row_plays_without_a_folder();
    no_drops();
    fetch_failure_drops();
    second_load_abandons();
    joiner_host_mismatch_drops();
    warm_cache_joiner();
    my_maps_play();
    a_newer_launch_replaces();
    the_question();
    rawmap_missing_counts_rows();
    rawmap_store_lacks_refuses();
    rawmap_store_has_downloads_and_reopens();
    the_refusal();
    gate_decision_and_wording();
    gate_causes_through_the_hold();
    gate_other_build_prompt();
    gate_editor_substitutes();
    gate_own_substitutes();
    gate_notes_after_the_decision();
    editor_open_missing_placed_mod();
    reset();
    sh_modpub_test_set_ops(NULL);
    printf("modpublished_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
