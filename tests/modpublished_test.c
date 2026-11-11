/* modpublished_test -- the published-map and offline-play gates: return-address
 * classification, pass-through for served builds, the hold, the question
 * (names and total size), Yes (fetch into the map's place on a worker,
 * activation, resume with a fresh parse of the retained text), No, a failed
 * fetch, a build that will not install, a second load abandoning the hold, the
 * joiner's host re-check, the warm-cache joiner branch and My Maps play; a
 * parse whose classification another detour broke, the SnapMap feed's play
 * and the launch boundary; the place every gate decides (a stranger's map its
 * item's place, the player's own maps the root, a map the place it records),
 * against a store double that honours places; and the rawmap
 * gate: the rows a rawmap names counted under the asset rule, the store
 * check, the refusal that names each mod and the download that reopens the
 * rawmap.
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

/* ---- fixtures ------------------------------------------------------------------------------- */

#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HASH_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SALT_A "alexpink"
#define SALT_B "bobroom1"

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"
#define ROW(salt, name, hash, size) \
    "{\"mod\":\"" salt "@" hash "\",\"name\":\"" name "\",\"version\":\"1\",\"size\":" size ",\"names\":[]}"
#define MEMBER(rows) ",\"snapmap-plus\":{\"format\":1,\"mods\":[" rows "]}"
#define MEMBER_AT(rows, place) ",\"snapmap-plus\":{\"format\":1,\"mods\":[" rows "],\"place\":\"" place "\"}"

static const char VANILLA[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    VAR("door_label", "Open") "]}}";
static const char ONE_MOD[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    VAR("door_label", "Open") "]}" MEMBER(ROW(SALT_A, "alex.pink-imp", HASH_A, "3145728")) "}";
/* A map that arrived with its mods: it records its place. */
static const char ONE_MOD_PLACED[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    MEMBER_AT(ROW(SALT_A, "alex.pink-imp", HASH_A, "3145728"), "Pink Arena") "}";
static const char TWO_MODS[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    MEMBER(ROW(SALT_A, "alex.pink-imp", HASH_A, "3145728") "," ROW(SALT_B, "bob.room", HASH_B, "1048576")) "}";

/* ---- engine and worker doubles -------------------------------------------------------------- */

typedef struct fake {
    int installed_a, installed_b, install_on_fetch, refuse_install;   /* installed: the root at the row's build */
    int root_a;                      /* a root folder holds alexpink at another build (own maps follow it) */
    char place_a[64], place_b[64];   /* the place that holds the row's build ("maps\<segment>"), "" none */
    int fetch_fails;                 /* nonzero: the fetch of the first build returns this */
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
    int store_a, store_b;            /* the probe: 1 held, 0 absent, -1 unreachable */
    unsigned long long size_a;       /* the archive size the store reports for alexpink */
    int editor_only_a;               /* the fetch finds alexpink editor-only: recorded, not installed */
    int recorded_a;                  /* the record's editorOnly section names alexpink's build */
    int store_calls;
    int incomplete_a;                /* the build serving alexpink lacks a name the row lists */
    int prints;
    char last_print[1024];
    char prints_all[8192];
    int claimed_a;                   /* a claim this machine holds names alexpink's build */
    int fetch_detail_calls;
    int launch_refused, refused_hook; /* launch_refused: how often, and the last hook */
    /* where a fetch installs, and the author's own published map */
    char fetch_place[64], fetch_salt[16];
    int titled;                      /* metadata_title answers "Pink Arena" */
    int own_item;                    /* this machine published item 1234abcd */
    /* the loaded map's place, as the gates set it (place_set) */
    int place_sets;
    char loaded_place[64];
} fake;

static fake F;
static int g_metadata_cookie;

#define METADATA_ID "1234abcd"   /* METADATA below: the double reads the item id from it */

/* The one place resolver (modserve.h) over the double's store: the root at
 * the row's build; then, with a place, that place at the row's build, and
 * without one (the player's own map) the root at any build. */
static int f_served(const sh_modmanifest_row *row, const char *place)
{
    if (!strcmp(row->build, HASH_A) || !strcmp(row->salt, SALT_A))
        return F.installed_a || (place[0] ? !_stricmp(place, F.place_a) : F.root_a);
    if (!strcmp(row->build, HASH_B)) return F.installed_b || (place[0] && !_stricmp(place, F.place_b));
    return 0;
}

/* ---- serving-layer doubles the real manifest calls ------------------------------------------ */

/* The manifest's "MODS: map: <salt>: <sentence>" lines (ADD-V5 B5). */
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
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!row || !f_served(row, F.loaded_place)) return 0;
    if (out) {
        strcpy_s(out->id, sizeof out->id, row->salt);
        strcpy_s(out->hash, sizeof out->hash, row->build);
    }
    return 1;
}
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
{
    (void)names; (void)count; (void)visit; (void)context;
    return 0;
}

static int f_on_main(void *c) { (void)c; return 1; }
/* The asset rule as modserve.c answers it, over the fixture's two mods. */
static int f_row_state(void *c, const sh_modmanifest_row *row, const char *place, const char *subject, char *cause,
                       size_t cap)
{
    const char *what = subject ? subject : "map";
    (void)c;
    if (cause && cap) cause[0] = 0;
    if (!strcmp(row->build, HASH_A)) {
        if (F.recorded_a) return SH_MODPUB_ROW_SERVED;
        if (f_served(row, place) && !F.incomplete_a) return SH_MODPUB_ROW_SERVED;
        if (f_served(row, place)) {
            if (cause) _snprintf_s(cause, cap, _TRUNCATE, "alex.pink-imp is installed but does not provide "
                                   "mods/alexpink/imp, which this %s uses.", what);
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
    if (!strcmp(row->build, HASH_B)) {
        if (f_served(row, place)) return SH_MODPUB_ROW_SERVED;
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
    for (size_t i = 0; i < rows->count && i < 4; i++) strcpy_s(F.activated[i], sizeof F.activated[i], rows->rows[i].build);
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
static int f_fetch_for(void *c, const sh_modmanifest_row *row, const char *place, sh_modpub_progress_fn progress,
                       volatile int *cancel)
{
    (void)c;
    F.fetch_calls++;
    strcpy_s(F.fetch_place, sizeof F.fetch_place, place ? place : "");
    strcpy_s(F.fetch_salt, sizeof F.fetch_salt, row->salt);
    if (progress) progress(NULL, 1, 2);
    if (F.fetch_gate) {
        while (WaitForSingleObject(F.fetch_gate, 5) == WAIT_TIMEOUT)
            if (*cancel) { F.fetch_cancelled++; return -2; }
    }
    if (F.fetch_fails && F.fetch_calls == 1) return F.fetch_fails;
    if (F.editor_only_a && !strcmp(row->build, HASH_A)) { F.recorded_a = 1; return 0; }   /* recorded, nothing installed */
    if (F.install_on_fetch && !F.refuse_install) {
        /* Into the place, or the root. */
        if (!strcmp(row->build, HASH_A)) { if (place[0]) strcpy_s(F.place_a, sizeof F.place_a, place); else F.installed_a = 1; }
        if (!strcmp(row->build, HASH_B)) { if (place[0]) strcpy_s(F.place_b, sizeof F.place_b, place); else F.installed_b = 1; }
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
static void f_print(void *c, const char *line)
{
    (void)c;
    F.prints++;
    strcpy_s(F.last_print, sizeof F.last_print, line);
    strncat_s(F.prints_all, sizeof F.prints_all, line, _TRUNCATE);
    strncat_s(F.prints_all, sizeof F.prints_all, "\n", _TRUNCATE);
}

#define ROCK    "generated/image/stock/rock.bimage"
static int f_store_probe(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out)
{
    (void)c; (void)cancel;
    memset(out, 0, sizeof *out);
    F.store_calls++;
    out->found = !strcmp(hash, HASH_A) ? F.store_a : !strcmp(hash, HASH_B) ? F.store_b : 0;
    if (out->found > 0 && !strcmp(hash, HASH_A)) out->size = F.size_a;
    return out->found;
}
static int f_claimed(void *c, const char *hash) { (void)c; return F.claimed_a && !strcmp(hash, HASH_A); }
static int f_fetch_detail(void *c, const char *hash, char *out, size_t cap)
{
    (void)c; (void)hash;
    F.fetch_detail_calls++;
    strcpy_s(out, cap, ROCK);
    return 1;
}
static int f_metadata_title(void *c, const void *metadata, char *out, size_t cap)
{
    (void)c; (void)metadata;
    if (!F.titled) return 0;
    strcpy_s(out, cap, "Pink Arena");
    return 1;
}
static int f_own_item(void *c, const char *item) { (void)c; return F.own_item && !strcmp(item, METADATA_ID); }
/* The item's place: mods\maps\<title>\. */
static int f_item_place(void *c, const char *title, const char *item, char *out, size_t cap)
{
    (void)c; (void)item;
    _snprintf_s(out, cap, _TRUNCATE, "maps\\%s", title && title[0] ? title : "map");
    return 1;
}
static void f_place_set(void *c, const char *place)
{
    (void)c;
    F.place_sets++;
    strcpy_s(F.loaded_place, sizeof F.loaded_place, place ? place : "");
}
static int f_place_norm(void *c, const char *place, char *out, size_t cap)
{
    (void)c;
    if (!place || strchr(place, '\\') || strchr(place, '/') || strstr(place, "..")) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "maps\\%s", place);
    return 1;
}

static const sh_modpub_ops OPS = {
    NULL, f_on_main, f_row_state, f_rescan, f_activate, f_can_ask, f_ask, f_poll, f_release,
    f_worker_run, f_fetch_for, f_metadata_copy, f_metadata_free, f_metadata_ids, f_hold_token,
    f_owner_current, f_host_matches, f_hold_ui, f_drop_ui, f_resume, f_log,
    f_print, f_store_probe, f_claimed, f_fetch_detail, f_launch_refused, f_metadata_title, f_own_item,
    f_item_place, f_place_set, f_place_norm
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

    /* A stranger's map is served only at the row's build: a root folder of
     * the salt at another build does not serve it, so it is asked for. */
    reset();
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.place_sets == 0);   /* a held map changes nothing yet */
    tick(1);
    CHECK(F.asks == 1 && !strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));

    /* The build that serves it lacks a listed name: the store offer, then No. */
    reset();
    F.installed_a = 1;
    F.incomplete_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding());
    CHECK(strstr(F.last_print, "is installed but does not provide mods/alexpink/imp, which this map uses.") != NULL);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "Download and install now?") != NULL);
    F.answer = 2;   /* No: the launch is dropped with the asset rule's sentence */
    tick(3);
    CHECK(!sh_modpublished_holding());
    CHECK(!strcmp(F.last_ask, "alex.pink-imp is installed but does not provide mods/alexpink/imp, which "
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
    CHECK(!strcmp(sh_modpub_test_notice(), "This map was not loaded: Snapmap+ ran out of memory preparing its mods. "
                                           "Restart DOOM and open it again."));
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
    printf("-- a stranger's map is served from its own place; a fetch lands there, the pin kept\n");
    /* A stranger's map: its place is its item's, mods\maps\<title>\, where
     * the fetch installs; a root folder of the salt at another build does
     * not serve it. The resumed map is served from that place. */
    reset();
    F.titled = 1;
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(F.place_sets == 0);
    tick(1);
    CHECK(F.asks == 1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && !strcmp(F.fetch_place, "maps\\Pink Arena") && !F.installed_a);
    CHECK(F.resumes == 1 && !strcmp(F.loaded_place, "maps\\Pink Arena"));
    {
        char launch[64];
        CHECK(sh_modpub_launch_place(launch, sizeof launch) && !strcmp(launch, "maps\\Pink Arena"));
        CHECK(!sh_modpub_launch_place(launch, sizeof launch));   /* consumed */
    }
    CHECK(!strcmp(F.fetch_salt, SALT_A));
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_current(&rows) == 1 && !strcmp(rows.rows[0].build, HASH_A));   /* its pin is kept */
    sh_modmanifest_list_free(&rows);

    /* The size the question names is the archive's, as the store reported it. */
    reset();
    F.size_a = 5u << 20;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(!strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (5.0 MB). Download and install now?"));
}

static void own_published_map_follows_own_folder(void)
{
    printf("-- the author's own published map follows the root folders; nothing is downloaded\n");
    /* The author's own published map: its context is own, so a root folder
     * of the salt serves the row whatever its build; nothing is asked and
     * nothing downloaded, and the row is left as it is (the next save writes
     * the root build). */
    reset();
    F.titled = 1;
    F.own_item = 1;
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(F.place_sets == 1 && !F.loaded_place[0]);
    CHECK(!sh_modpublished_holding() && !F.hold_ui);
    tick(5);
    CHECK(F.asks == 0 && F.fetch_calls == 0);

    /* Own, but no root folder holds the salt: it is fetched as any map is. */
    reset();
    F.own_item = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    F.answer = 1;
    tick(1);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && !F.fetch_place[0] && F.installed_a);   /* into the root */

    /* A My Maps play is the player's own: the root folder serves it. */
    reset();
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(F.place_sets == 1 && !F.loaded_place[0] && !sh_modpublished_holding());
}

static void a_map_carries_its_place(void)
{
    char launch[64];
    printf("-- a map finds its mods where it records them, at every load\n");
    /* A map imported from a ZIP or downloaded, saved to My Maps: its place
     * comes back with it, so its mods are found there and nothing is asked. */
    reset();
    strcpy_s(F.place_a, sizeof F.place_a, "maps\\Pink Arena");
    strcpy_s(F.loaded_place, sizeof F.loaded_place, "maps\\Elsewhere");   /* the last map's */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD_PLACED, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.asks == 0 && !strcmp(F.loaded_place, "maps\\Pink Arena"));
    CHECK(sh_modpub_launch_place(launch, sizeof launch) && !strcmp(launch, "maps\\Pink Arena"));

    /* Copy to My Maps of a stranger's map with a mod (the map records no
     * place): the player's own map, so the fetch installs at the root and
     * the row is served there when the hold checks it again. */
    reset();
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    F.answer = 1;
    tick(1);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && !F.fetch_place[0] && F.installed_a && F.resumes == 1);
    CHECK(!strstr(F.last_ask, "could not be installed") && !sh_modpub_test_notice()[0]);
    /* The next play of it passes at once: the root serves it. */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);

    /* The launch boundary decides a parse no gate saw in that map's place,
     * never in the place an earlier map left behind. */
    reset();
    strcpy_s(F.place_a, sizeof F.place_a, "maps\\Pink Arena");
    strcpy_s(F.loaded_place, sizeof F.loaded_place, "maps\\Elsewhere");
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD_PLACED, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, (void *)1) == 1);
    CHECK(!strcmp(F.loaded_place, "maps\\Pink Arena"));
    reset();
    strcpy_s(F.place_a, sizeof F.place_a, "maps\\Elsewhere");      /* only another map's place holds it */
    strcpy_s(F.loaded_place, sizeof F.loaded_place, "maps\\Elsewhere");
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD_PLACED, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, (void *)1) == 0);   /* held */
    CHECK(sh_modpublished_holding());

    /* A place that is not one (a hostile map naming a folder outside
     * mods\maps\) is no place: the map finds its mods at the root. */
    reset();
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_OTHER,
          "{\"entities\":[],\"snapmap-plus\":{\"format\":1,\"mods\":["
          ROW(SALT_A, "alex.pink-imp", HASH_A, "3145728") "],\"place\":\"..\"}}", MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!F.loaded_place[0]);
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
    CHECK(strstr(F.last_ask, "alex.pink-imp could not be downloaded: ") == F.last_ask &&
          strstr(F.last_ask, "The map was not loaded.") != NULL);

    reset();
    F.refuse_install = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    F.answer = 1;
    tick_until_idle();
    CHECK(F.rescans == 1 && F.resumes == 0 && F.activations == 0);
    tick(2);
    CHECK(strstr(F.last_ask, "alex.pink-imp was downloaded but cannot be used. This map uses alex.pink-imp, which is "
                             "installed but cannot be used: a file size differs.") != NULL);
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
    CHECK(F.fetch_cancelled == 0 && F.fetch_calls == 1 && F.resumes == 1 && !strcmp(F.place_a, "maps\\map"));
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
    CHECK(strstr(F.last_ask, "the host changed maps while they downloaded. Join the lobby again.") != NULL);

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
    char place[SH_MODPUB_PLACE_CAP];
    printf("-- a rawmap's rows are counted under the asset rule, in the place it records\n");
    reset();
    CHECK(sh_modpub_rawmap_missing(VANILLA, strlen(VANILLA), &rows, place, sizeof place) == 0 && rows.count == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, place, sizeof place) == 2 && rows.count == 2);
    sh_modmanifest_list_free(&rows);
    F.installed_b = 1;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, place, sizeof place) == 1 && !place[0]);
    sh_modmanifest_list_free(&rows);
    /* No place: the author's own, a root folder of the salt at any build
     * serves it. A place: only the root at the row's build, or that place. */
    F.root_a = 1;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, place, sizeof place) == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_modpub_rawmap_missing(ONE_MOD_PLACED, strlen(ONE_MOD_PLACED), &rows, place, sizeof place) == 1);
    CHECK(!strcmp(place, "maps\\Pink Arena"));
    sh_modmanifest_list_free(&rows);
    strcpy_s(F.place_a, sizeof F.place_a, "maps\\Pink Arena");
    CHECK(sh_modpub_rawmap_missing(ONE_MOD_PLACED, strlen(ONE_MOD_PLACED), &rows, place, sizeof place) == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(F.place_sets == 0);   /* counting changes nothing */
    F.root_a = 0;
    /* Not a published-map parse: the rawmap check never touches the parse record. */
    CHECK(sh_modpublished_last_parse(NULL) == -1);
    /* Without bound gates nothing is missing: the rawmap loads as it always did. */
    sh_modpub_test_set_ops(NULL);
    F.installed_a = F.installed_b = 0;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, place, sizeof place) == 0);
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
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, NULL, 0) == 2);
    CHECK(sh_modpub_rawmap_hold(&rows, "shared.json", "") == 1);
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
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "one.zip", "") == 1);
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
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows, NULL, 0) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json", "") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_probed();
    CHECK(F.store_calls == 1 && sh_modpub_test_state() == 1);
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 6);
    CHECK(!strcmp(F.last_ask, "This map needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.rescans == 1);
    CHECK(!F.fetch_place[0] && F.installed_a);   /* a rawmap without a place: into the root */
    CHECK(F.activations == 0);           /* the reopened rawmap's own parse activates */
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_RAWMAP_LOAD && !strcmp(F.resume_id, "rawmap.json"));
    CHECK(!F.resume_text[0] && F.metadata_copies == 0 && F.hold_ui == 0);

    printf("-- No leaves the rawmap closed and names the mod\n");
    reset();
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json", "") == 1);
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
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 0);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json", "") == 0 && !sh_modpublished_holding());
    sh_modmanifest_list_free(&rows);
}

/* ---- the denial gate ------------------------------------------------------------------------ */

/* The author's map in the editor: its rows follow the root folders, so a
 * row nothing serves names a salt no root folder holds. Nothing substitutes;
 * the note says what this computer knows. */
static void gate_editor_substitutes(void)
{
    sh_modmanifest_row row;
    char hash[80], why[600];
    printf("-- the gate: the editor opens the author's map; nothing substitutes, the note is plain\n");
    reset();
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, SALT_A);
    strcpy_s(row.name, sizeof row.name, "alex.pink-imp");
    strcpy_s(row.build, sizeof row.build, HASH_A);
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
    /* A row without a name says its salt. */
    row.name[0] = 0;
    CHECK(sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, why, sizeof why) == 0 &&
          !strncmp(why, "This map needs alexpink, which", 30));
    reset();
}

/* The editor opens a My Maps map whose mod is gone (V13's "V13 crate", live 7491a99): with
 * an entity of the mod placed, the engine refused the map with its damaged-save dialog and
 * the player was not told why. The parse no gate takes now raises one plain notice; a map
 * that only declares the mod opens without it and raises none. */
static void editor_open_missing_placed_mod(void)
{
    static const char PLACED[] =
        "{\"entities\":[{\"entityDef\":{\"inherit\":\"mods/" SALT_A "/imp\"}}],\"instances\":[],"
        "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}"
        MEMBER(ROW(SALT_A, "alex.pink-imp", HASH_A, "3145728")) "}";
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
    f.published = 1;
    CHECK(sh_modpub_gate_decide(&f) == SH_MODPUB_GATE_OBTAIN);
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
    printf("-- cause 1: a published map whose build left the store; never asked, permanent\n");
    reset();
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

    printf("-- cause 3: a My Maps map never published whose salt no folder holds\n");
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
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 1);
    CHECK(sh_modpub_prefab_hold(&rows, "Door Kit") == 1);
    sh_modmanifest_list_free(&rows);
    tick(3);
    CHECK(!strcmp(F.last_ask, "This prefab needs alex.pink-imp, which is not installed and was never "
                              "published. Restore its folder from the Recycle Bin or load the prefab's ZIP export. The "
                              "prefab was not placed."));
    CHECK(F.drop_ui == 1 && F.launch_refused == 0);   /* a placement is not a launch */

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
    /* Cause 4: the archive arrived, but a borrow names a file this game does
     * not hold; learned at the install. */
    CHECK(F.fetch_detail_calls == 1 &&
          !strcmp(F.last_ask, "This map needs alex.pink-imp, which needs a file this game does not hold "
                              "(generated/image/stock/rock.bimage): a different game revision. This is permanent on "
                              "this machine."));
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
    printf("-- a stranger's map with a root build of the salt at another build: the ordinary question\n");
    reset();
    F.root_a = 1;
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
    F.root_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, TWO_MODS, MAP);

    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(!strcmp(F.last_ask, "This map needs 2 mods: alex.pink-imp, bob.room (4.0 MB). Download and install now?"));
}

/* The player's own content (ONE-SHAPE rule 4): a My Maps play, a rawmap load
 * and a prefab placement follow the root folders, so a root folder of the
 * salt at any build serves the row and nothing is held; with no root folder
 * of the salt the gate asks the store as for any map. A community map from
 * the browser keeps exactly the build it pins. */
static int g_ready_calls;
static char g_ready_name[96];
static void f_prefab_ready(const char *name, const sh_modmanifest_list *rows)
{
    (void)rows;
    g_ready_calls++;
    strcpy_s(g_ready_name, sizeof g_ready_name, name ? name : "");
}

static void gate_own_substitutes(void)
{
    sh_modmanifest_list rows;
    printf("-- the player's own content follows the root folders: a My Maps play is not held\n");
    reset();
    F.store_a = 0;
    F.root_a = 1;
    sh_modmanifest_set_editor_load(1);                 /* the parse detour: a read that is not a published map's */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && !F.loaded_place[0] && F.store_calls == 0 && F.asks == 0);
    /* A community map naming the same build, from the browser: its own
     * context, and cause 1 when the store lacks it. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(3);
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_BROWSER_PLAY);
    CHECK(!strcmp(F.last_ask, "This map needs alex.pink-imp, which is no longer available: its "
                              "author unpublished it, or it was removed. This is permanent on this machine."));
    sh_modmanifest_set_editor_load(0);

    printf("-- own content with no root folder of the salt asks the store; two mods, one served\n");
    reset();
    F.root_a = 1;
    F.store_b = 1;
    parse(SH_MODPUB_PARSE_OTHER, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.asks == 1 && strstr(F.last_ask, "bob.room") != NULL && !strstr(F.last_ask, "alex.pink-imp"));
    F.answer = 1;
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.installed_b && F.activated_count == 2);
    CHECK(!F.fetch_place[0]);   /* a My Maps map without a place: into the root */
    /* The other one nothing serves and the store lacks: denied for it alone. */
    reset();
    F.root_a = 1;
    F.store_b = 0;
    parse(SH_MODPUB_PARSE_OTHER, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(3);
    CHECK(F.resumes == 0 && F.launch_refused == 1 && strstr(F.last_ask, "This map needs bob.room, which is not "
                                                                        "installed and was never published.") == F.last_ask);
    CHECK(!strstr(F.last_ask, "more mod"));

    printf("-- a rawmap load and a prefab placement follow the root folders the same way\n");
    reset();
    F.root_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 0);   /* no place: the root serves it */
    sh_modmanifest_list_free(&rows);
    F.root_a = 0;
    reset();
    g_ready_calls = 0;
    sh_modpub_set_prefab_ready(f_prefab_ready);
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows, NULL, 0) == 1);
    CHECK(sh_modpub_prefab_hold(&rows, "Door Kit") == 1);
    sh_modmanifest_list_free(&rows);
    tick(1);
    CHECK(F.asks == 1 && !strcmp(F.last_ask, "This prefab needs 1 mod: alex.pink-imp (3.0 MB). Download and install now?"));
    F.answer = 1;
    tick_until_idle();
    CHECK(!sh_modpublished_holding() && g_ready_calls == 1 && !strcmp(g_ready_name, "Door Kit") && F.resumes == 0);
    CHECK(F.fetch_calls == 1 && !F.fetch_place[0] && F.installed_a);   /* a prefab's mods: the root */
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
    own_published_map_follows_own_folder();
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
    a_map_carries_its_place();
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
