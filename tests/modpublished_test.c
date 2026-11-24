/* modpublished_test -- the published-map and offline-play gates: return-address
 * classification, pass-through for served builds, the hold, and what it does
 * with the rows a map names. A missing row that names units (pinned) is
 * fetched at the next tick with no question and a progress line ("Getting this
 * map's mod content: ..."), the content step composes it on a worker, and the
 * load activates and resumes with a fresh parse of the retained text; a failed
 * fetch (stopped, or out of memory), a fetched row that will not serve, and a
 * map whose served content carries scripts the player turned off drop the load
 * with one notice. A missing row that names no unit (a format 1 row nothing
 * serves, a format 2 local row) is refused at the next tick with one sentence
 * and nothing is fetched, for a published play, a My Maps play, a joiner and a
 * rawmap load. Also covered: a second load abandoning
 * the hold (also mid-fetch), the same map launched again mid-fetch, the
 * joiner's host re-check and the co-op lobby download, the warm-cache joiner
 * branch and My Maps play; a parse whose classification another detour broke,
 * the SnapMap feed's play and the launch boundary; the scripts gate (a map with a
 * pinned row whose served compositions carry scripts, never the player's own
 * local rows, never a joiner); the rawmap gate (rows counted under the asset
 * rule, pinned rows reaching the content step); the editor's open of a map and
 * its content; and the notice queue.
 *
 * The real map manifest (modmanifest.c) reads the rows; the engine, the content
 * cache and the store worker are doubles behind sh_modpub_ops. The worker
 * double runs the job on a real thread so the main-thread tick is exercised as
 * in the game. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modpublished.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modlobby.h"
#include "../src/backend/modcache.h"   /* SH_CACHE_ABSENT_*: the absence causes */

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
/* A local row (format 2): a mod this computer's folder serves, with no unit to fetch. */
#define ROW(salt, name) \
    "{\"mod\":\"" salt "\",\"name\":\"" name "\",\"size\":42,\"local\":true,\"names\":[]}"
#define MEMBER(rows) ",\"snapmap-plus\":{\"format\":2,\"mods\":[" rows "]}"

static const char VANILLA[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    VAR("door_label", "Open") "]}}";
static const char ONE_MOD[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    VAR("door_label", "Open") "]}" MEMBER(ROW(SALT_A, "alex.pink-imp")) "}";
static const char TWO_MODS[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    MEMBER(ROW(SALT_A, "alex.pink-imp") "," ROW(SALT_B, "bob.room")) "}";

/* ---- engine and worker doubles -------------------------------------------------------------- */

typedef struct fake {
    int installed_a, installed_b, install_on_fetch, refuse_install;   /* installed: a build serves the row */
    int fetch_fails;                /* nonzero: the fetch of the first build returns this */
    int fetch_calls, fetch_cancelled;
    HANDLE fetch_gate;               /* when set, the fetch waits on it */
    int can_ask_after;               /* ticks before the surface is idle */
    long long screen;                /* ops.screen: the shell's screen token, -1 between screens, 0 unknown */
    int editor_open;                 /* ops.editor_open: 1 (the default after reset) while the editor shows a map */
    int busy;                        /* ops.busy: 1 while a map loads or Play runs */
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
    int scripts_off;                 /* the player turned off "Play maps whose mods include scripts" */
    unsigned served_scripts;         /* script files of the compositions that serve the pinned rows */
    unsigned scripts_after_fetch;    /* script files the served compositions carry once a fetch has landed */
    int served_scripts_calls;
    int prog_set;                    /* the fetch reports prog_done of prog_total bytes (else 1 of 2) */
    unsigned long long prog_done, prog_total;
    int progress_sent;               /* the worker reported its first progress */
    int progress_shows, progress_hides;   /* the progress line: text set, taken down */
    int progress_up;
    char last_progress[256];
    int incomplete_a;               /* the build serving alexpink lacks a name the row lists */
    int prints;
    char last_print[1024];
    char prints_all[8192];
    int launch_refused, refused_hook; /* launch_refused: how often, and the last hook */
    char fetch_salt[16];             /* the salt of the row the fetch ran for */
} fake;

static fake F;
static int g_metadata_cookie;

/* The row resolver (modserve.h) over the double's store: a row is served when its mod is installed. */
static int f_served(const sh_modmanifest_row *row)
{
    if (!strcmp(row->salt, SALT_A)) return F.installed_a;
    if (!strcmp(row->salt, SALT_B)) return F.installed_b;
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
int sh_modserve_spawn_pins(void (*fn)(void *context, const char *salt, const char *slot), void *context)
{ (void)fn; (void)context; return 0; }

int modserve_activate_bundles(const char *const *bundle_hashes, int count)
{
    (void)bundle_hashes;
    return count;
}
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref)
{
    if (!ref) return;
    free(ref->name);
    free(ref->folder);
    free(ref->place);
    ref->name = ref->folder = ref->place = NULL;
}

/* A local row's folder that is here but cannot be used (modserve.h): none here. */
int sh_modserve_salt_installed(const char *salt) { (void)salt; return 0; }
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!row || !f_served(row)) return 0;
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
/* The content cache's double (modcache.h): a pinned row is complete or stands in for objects; a composition raises
 * the one sentence of what the map plays without (pending), which content_notice takes once. */
static struct {
    int armed;                 /* a fetch's composition and a resumed load's content step raise the sentence */
    int complete;              /* content_complete's answer */
    int served;                /* a pinned row serves (stand-ins included) */
    int pending, said, said_at_resumes, drops, refreshes, refresh_answer, absent_after_fetch;
    int unserve_after_fetch;   /* the composed row does not serve after the fetch */
    unsigned causes;           /* content_causes' mask (1 << SH_CACHE_ABSENT_*); 0: the op answers no composition */
} g_content;
static int f_resumes_now(void);
static int f_content_complete(void *c, const sh_modmanifest_row *row) { (void)c; (void)row; return g_content.complete; }
static int f_content_causes(void *c, const sh_modmanifest_row *row, unsigned *mask)
{
    (void)c; (void)row;
    *mask = g_content.complete ? 0 : g_content.causes;
    return g_content.causes != 0 || g_content.complete;
}
static char *f_content_notice(void *c)
{
    (void)c;
    if (!g_content.pending) return NULL;
    g_content.pending = 0;
    g_content.said++;
    g_content.said_at_resumes = f_resumes_now();
    return _strdup("This map is playing without the Tyrant from Eternal Demons: the mod store could not be reached. "
                   "Open the map again later to get it.");
}
static int f_content_pending(void *c) { (void)c; return g_content.pending; }
static void f_content_drop(void *c) { (void)c; g_content.pending = 0; g_content.drops++; }
static int f_content_refresh(void *c) { (void)c; g_content.refreshes++; return g_content.refresh_answer; }

static int f_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char **cause)
{
    const char *what = subject ? subject : "map";
    (void)c;
    if (cause) *cause = NULL;
    if (row->pinned) return g_content.served ? SH_MODPUB_ROW_SERVED : SH_MODPUB_ROW_MISSING;
    if (!strcmp(row->salt, SALT_A)) {
        if (f_served(row) && !F.incomplete_a) return SH_MODPUB_ROW_SERVED;
        if (f_served(row)) {
            if (cause) ho_setf(cause, "alex.pink-imp is installed but does not provide "
                                   "mods/alexpink/imp, which this %s uses.", what);
            return SH_MODPUB_ROW_INCOMPLETE;
        }
        if (cause) {
            if (F.refuse_install && F.fetch_calls)
                ho_setf(cause, "This %s uses alex.pink-imp, which is installed but cannot be used: "
                            "a file size differs.", what);
            else ho_setf(cause, "This %s uses alex.pink-imp, which is not installed.", what);
        }
        return SH_MODPUB_ROW_MISSING;
    }
    if (!strcmp(row->salt, SALT_B)) {
        if (f_served(row)) return SH_MODPUB_ROW_SERVED;
        if (cause) ho_setf(cause, "This %s uses bob.room, which is not installed.", what);
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
static long long f_screen(void *c) { (void)c; return F.screen; }
static int f_editor_open(void *c) { (void)c; return F.editor_open; }
static int f_busy(void *c) { (void)c; return F.busy; }

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
static int f_fetch_for(void *c, const sh_modmanifest_row *row, sh_modpub_progress_fn progress, volatile int *cancel)
{
    (void)c;
    F.fetch_calls++;
    strcpy_s(F.fetch_salt, sizeof F.fetch_salt, row->salt);
    if (progress) {
        if (F.prog_set) progress(NULL, F.prog_done, F.prog_total);
        else progress(NULL, 1, 2);
    }
    F.progress_sent = 1;
    if (F.fetch_gate) {
        while (WaitForSingleObject(F.fetch_gate, 5) == WAIT_TIMEOUT)
            if (*cancel) { F.fetch_cancelled++; return -2; }
    }
    if (F.fetch_fails && F.fetch_calls == 1) return F.fetch_fails;
    /* The fetch composed the row: what stayed absent waits for the notice. */
    if (g_content.armed) {
        g_content.pending = g_content.absent_after_fetch;
        g_content.complete = !g_content.absent_after_fetch;
        if (g_content.unserve_after_fetch) g_content.served = 0;   /* a row the fetch could not make servable */
    }
    if (F.install_on_fetch && !F.refuse_install) {
        if (!strcmp(row->salt, SALT_A)) F.installed_a = 1;
        if (!strcmp(row->salt, SALT_B)) F.installed_b = 1;
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
static int f_metadata_ids(void *c, const void *metadata, char **id, char **rev)
{
    (void)c;
    ho_set(id, (const char *)metadata);
    ho_set(rev, "rev1");
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
    /* The resumed load's content step says what it plays without, from what the fetch recorded. */
    if (g_content.armed && g_content.absent_after_fetch) g_content.pending = 1;
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
static char g_last_logs[8192];   /* the module's recent lines */
static void f_log(void *c, const char *line)
{
    (void)c;
    if (getenv("MODPUB_TEST_LOG")) printf("    %s\n", line);
    if (line && strlen(g_last_logs) + strlen(line) + 2 >= sizeof g_last_logs) g_last_logs[0] = 0;
    if (line) { strcat_s(g_last_logs, sizeof g_last_logs, line); strcat_s(g_last_logs, sizeof g_last_logs, "\n"); }
}
static int said_log(const char *text) { return strstr(g_last_logs, text) != NULL; }
static void f_launch_refused(void *c, int hook) { (void)c; F.launch_refused++; F.refused_hook = hook; }
static void f_print(void *c, const char *line)
{
    (void)c;
    F.prints++;
    strcpy_s(F.last_print, sizeof F.last_print, line);
    strncat_s(F.prints_all, sizeof F.prints_all, line, _TRUNCATE);
    strncat_s(F.prints_all, sizeof F.prints_all, "\n", _TRUNCATE);
}

static int f_scripted_maps_allowed(void *c) { (void)c; return !F.scripts_off; }
static unsigned f_served_scripts(void *c, const sh_modmanifest_list *rows)
{
    int pinned = 0;
    (void)c;
    F.served_scripts_calls++;
    for (size_t i = 0; rows && i < rows->count; i++) if (rows->rows[i].pinned) pinned = 1;
    return pinned ? F.served_scripts + (F.fetch_calls ? F.scripts_after_fetch : 0) : 0;
}
static void f_progress(void *c, int hook, void *owner, const char *text)
{
    (void)c; (void)hook; (void)owner;
    if (text) { F.progress_shows++; F.progress_up = 1; strcpy_s(F.last_progress, sizeof F.last_progress, text); }
    else { F.progress_hides++; F.progress_up = 0; }
}

static int g_refused_loads;
static void f_refused_load(void *c) { (void)c; ++g_refused_loads; }

/* A joiner taken out of the lobby, a cold read started, the Play scope set. */
static int g_leaves, g_cold_reads, g_cold_ok = 1, g_scope_plays;
static void f_leave_lobby(void *c, int hook, void *owner) { (void)c; (void)hook; (void)owner; g_leaves++; }
static int f_cold_read(void *c, void *owner, const char *id, const char *revision)
{
    (void)c; (void)owner; (void)id; (void)revision;
    g_cold_reads++;
    return g_cold_ok;
}
static void f_scope_play(void *c, const char *why) { (void)c; (void)why; g_scope_plays++; }

static const sh_modpub_ops OPS = {
    .on_main = f_on_main, .row_state = f_row_state, .rescan = f_rescan, .activate = f_activate,
    .can_ask = f_can_ask, .ask = f_ask, .poll = f_poll, .release = f_release, .screen = f_screen, .editor_open = f_editor_open, .busy = f_busy, .worker_run = f_worker_run,
    .fetch_for = f_fetch_for, .metadata_copy = f_metadata_copy, .metadata_free = f_metadata_free,
    .metadata_ids = f_metadata_ids, .hold_token = f_hold_token, .owner_current = f_owner_current,
    .host_matches = f_host_matches, .hold_ui = f_hold_ui, .drop_ui = f_drop_ui, .resume = f_resume, .log = f_log,
    .print = f_print, .launch_refused = f_launch_refused, .scripted_maps_allowed = f_scripted_maps_allowed,
    .served_scripts = f_served_scripts, .progress = f_progress, .refused_load = f_refused_load,
    .leave_lobby = f_leave_lobby, .cold_read = f_cold_read, .scope_play = f_scope_play
};

static int f_resumes_now(void) { return F.resumes; }

/* The last notice: still waiting to be raised, or raised (a notice is raised with the one Accept button). */
static int said_notice(const char *text, size_t n)
{
    return !strncmp(sh_modpub_test_notice(), text, n) || (F.ask_buttons == 0 && !strncmp(F.last_ask, text, n));
}

/* The ops with the content cache's double bound. */
static sh_modpub_ops g_ops_content;
static void use_content(void)
{
    g_ops_content = OPS;
    g_ops_content.content_complete = f_content_complete;
    g_ops_content.content_causes = f_content_causes;
    g_ops_content.content_notice = f_content_notice;
    g_ops_content.content_pending = f_content_pending;
    g_ops_content.content_drop = f_content_drop;
    g_ops_content.content_refresh = f_content_refresh;
    sh_modpub_test_set_ops(&g_ops_content);
}

static void reset(void)
{
    memset(&g_content, 0, sizeof g_content);
    if (F.worker) { WaitForSingleObject(F.worker, 5000); CloseHandle(F.worker); }
    sh_modpub_test_reset();
    memset(&F, 0, sizeof F);
    F.editor_open = 1;
    F.install_on_fetch = 1;
    F.host_ok = 1;
    F.owner_ok = 1;
    sh_modpub_test_set_ops(&OPS);
    sh_modmanifest_reset();
    g_leaves = g_cold_reads = g_scope_plays = 0;
    g_cold_ok = 1;
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

/* One main-thread tick. */
static void tick(int n)
{
    for (int i = 0; i < n; i++) {
        F.ticks++;
        sh_modpublished_poll();
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

/* Pinned maps (format 2): rows that name units. Their content is fetched at once, with no question. */
static const char PINNED_MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idPlaceableSnapAIEncounter\",\"inherit\":\"mods/" SALT_A "/tyrant\","
    "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},\"uniqueId\":1,\"~type\":\"idSnapEntity\"}],"
    "\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"" SALT_A "\",\"name\":\"Eternal Demons\",\"size\":42,"
    "\"objects\":{\"entities/tyrant\":\"" HASH_A "\"}}]}}";
static const char PINNED_TWO[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"" SALT_A "\",\"name\":\"Eternal Demons\",\"size\":42,"
    "\"objects\":{\"entities/tyrant\":\"" HASH_A "\"}},{\"mod\":\"" SALT_B "\",\"name\":\"Bob Room\",\"size\":42,"
    "\"objects\":{\"entities/crate\":\"" HASH_B "\"}}]}}";
/* One pinned row and one local row (format 2 "local": a folder this computer does not have, no unit to fetch). */
static const char PINNED_AND_LOCAL[] =
    "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
    ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"" SALT_A "\",\"name\":\"Eternal Demons\",\"size\":42,"
    "\"objects\":{\"entities/tyrant\":\"" HASH_A "\"}},{\"mod\":\"" SALT_B "\",\"name\":\"Bob Room\",\"size\":42,"
    "\"local\":true}]}}";

/* The sentence a missing row that names no unit is refused with (pub_refuse_local). */
#define LOCAL_ONE \
    "This map needs alex.pink-imp, which is not on this computer and cannot be downloaded: the map was saved before " \
    "its content was sent to the mod store. Ask its author to publish it again. The map was not loaded."
#define SCRIPTS_OFF_TEXT \
    "This map's mods include scripts, which you turned off. To play it, turn on " \
    "\"Play Maps Whose Mods Include Scripts\" in the Settings menu of the Snapmap+ window."
#define SAID(lit) said_notice((lit), sizeof(lit) - 1)

/* Pinned rows serve once composed: the content cache's double armed, incomplete until the fetch. */
static void use_units(void)
{
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
}

/* The worker has reported its first progress. */
static void wait_progress_sent(void)
{
    for (int i = 0; i < 400 && !*(volatile int *)&F.progress_sent; i++) Sleep(5);
}

/* ---- co-op lobbies (SPEC 5.11, owner 2026-11-19; modlobby.h) ------------------------------------------------- */

/* What the joiner told its host (modlobby.h codes), and where its lobby stands. */
static struct { unsigned codes[64]; int count; int place; } g_lobby;
static void f_lobby_report(void *c, unsigned code)
{
    (void)c;
    if (g_lobby.count < 64) g_lobby.codes[g_lobby.count] = code;
    g_lobby.count++;
}
static int f_lobby_state(void *c, const char *id, const char *revision) { (void)c; (void)id; (void)revision; return g_lobby.place; }
static unsigned last_code(void) { return g_lobby.count ? g_lobby.codes[(g_lobby.count < 64 ? g_lobby.count : 64) - 1] : 0; }
static int sent_code(unsigned code)
{
    for (int i = 0; i < g_lobby.count && i < 64; i++) if (g_lobby.codes[i] == code) return 1;
    return 0;
}

static void use_lobby(void)
{
    use_content();
    g_ops_content.lobby_report = f_lobby_report;
    g_ops_content.lobby_state = f_lobby_state;
    sh_modpub_test_set_ops(&g_ops_content);
    memset(&g_lobby, 0, sizeof g_lobby);
    g_lobby.place = SH_MODPUB_LOBBY_SAME;
    g_content.armed = 1;
    g_content.served = 1;       /* a composed row serves; incomplete until its fetch (content_complete) */
}

/* A joiner reads the host's pinned map (its CacheReady is held). */
static void join_pinned(void)
{
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
}

/* The worker is inside the fetch. */
static void wait_fetching(void)
{
    for (int i = 0; i < 400 && !F.fetch_calls; i++) Sleep(5);
}

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


/* Manager Q8, D6a: a map whose local row's folder is not here loads with its objects stood in; the gate lets it
 * through with nothing to fetch, and says the parse's one sentence. A map that passes with nothing stood in says
 * nothing; a stale sentence never reaches the next map. */
static void a_passing_map_says_its_stand_ins(void)
{
    printf("-- a map that plays with stand-ins and nothing to fetch says its one notice at the gate (Q8)\n");
    reset();
    use_content();
    F.installed_a = 1;          /* the row serves (through its stand-in composition, in the product) */
    g_content.pending = 1;      /* the parse's stand-ins made a notice */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0);
    CHECK(g_content.said == 1 && said_notice("This map is playing without the Tyrant", 38));
    CHECK(strstr(F.last_print, "This map is playing without the Tyrant") != NULL);

    reset();
    use_content();
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    tick(3);
    CHECK(g_content.said == 0 && F.asks == 0);
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

    /* A map whose row no build serves is held; its row names no unit, so the next tick refuses it (nothing is
     * asked of the store). */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.activations == 0);   /* a held map changes nothing yet */
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.asks == 1 && F.ask_buttons == 0);
    CHECK(!strcmp(F.last_ask, LOCAL_ONE));

    /* The build that serves it lacks a listed name: the asset rule's sentence is said at the hold, and the refusal says
     * the same, not "not on this computer". */
    reset();
    F.installed_a = 1;
    F.incomplete_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding());
    CHECK(strstr(F.last_print, "is installed but does not provide mods/alexpink/imp, which this map uses.") != NULL);
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.drop_ui == 1);
    CHECK(!strcmp(F.last_ask, "alex.pink-imp is installed but does not provide mods/alexpink/imp, which this map uses. "
                              "The map was not loaded."));

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
    use_units();
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.hold_ui == 1 && F.metadata_copies == 1);
    tick_until_idle();
    CHECK(F.asks == 0 && F.fetch_calls == 1);
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_BROWSER_PLAY && !strcmp(F.resume_text, PINNED_MAP));

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
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_FEED_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.metadata_copies == 1);
    tick_until_idle();
    CHECK(F.asks == 0);
    CHECK(F.activations == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_FEED_PLAY);
    CHECK(!strcmp(F.resume_id, METADATA) && !strcmp(F.resume_text, PINNED_MAP));
    /* Its launch, after the resume, passes the boundary: the record was consumed. */
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 1);
}

static void launch_boundary(void)
{
    printf("-- the launch boundary holds or refuses a map that no gate saw\n");
    /* A completion this module does not know: the parse is followed by the
     * lobby launch in the same frame. Held, its content fetched, resumed with the flag. */
    reset();
    use_units();
    F.token = 7;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_TWO, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, (void *)1) == 0);
    CHECK(sh_modpublished_holding() && F.metadata_copies == 0);
    tick(1);
    CHECK(F.asks == 0 && sh_modpub_test_state() == 2);
    tick_until_idle();
    CHECK(F.asks == 0 && F.fetch_calls == 2 && F.rescans == 1 && F.activations == 1 && F.activated_count == 2);
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_LOBBY_LAUNCH && F.resume_extra == (void *)1);
    CHECK(!strcmp(F.resume_text, PINNED_TWO));
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);

    /* The screen it was held on is gone by the time the content is in: abandoned. */
    reset();
    use_units();
    F.token = 7;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 0);
    F.token = 8;
    tick_until_idle();
    CHECK(F.resumes == 0 && F.activations == 0 && !sh_modpublished_holding());

    /* A row that names no unit: nothing launches, nothing is fetched; the notice names the mod. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 0);
    tick(1);
    CHECK(F.resumes == 0 && F.fetch_calls == 0 && F.drop_ui == 1 && !sh_modpublished_holding());
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_LOBBY_LAUNCH && !strcmp(F.last_ask, LOCAL_ONE));

    /* An offline launch has no object to resume with, so nothing is fetched: a row that names no unit is refused. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, MAP, NULL) == 0);
    CHECK(!sh_modpublished_holding() && F.drop_ui == 1 && F.asks == 0 && F.fetch_calls == 0);
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_OFFLINE_LAUNCH);
    CHECK(!strcmp(sh_modpub_test_notice(), "This map was not loaded: it needs mods that are not on this computer and "
                                           "cannot be downloaded. Ask its author to publish it again."));
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 0);

    /* A pinned row whose composition is here (stand-ins for what is not): the offline launch plays. */
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, MAP, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.drop_ui == 0 && F.launch_refused == 0 && F.fetch_calls == 0 &&
          !sh_modpub_test_notice()[0]);
    tick(2);
    CHECK(F.asks == 0);

    /* A pinned row with no composition at all: refused, told to connect and open it again. */
    reset();
    use_units();
    g_content.served = 0;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, MAP, NULL) == 0);
    CHECK(!sh_modpublished_holding() && F.drop_ui == 1 && F.launch_refused == 1 && F.refused_hook == SH_MODPUB_OFFLINE_LAUNCH);
    CHECK(F.fetch_calls == 0);
    CHECK(!strcmp(sh_modpub_test_notice(), "This map was not loaded: its mod content is not on this computer. Connect to "
                                           "the internet and open it again."));

    /* A row that names no unit beside a pinned one: the local sentence wins. */
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_AND_LOCAL, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, MANAGER, MAP, NULL) == 0);
    CHECK(F.drop_ui == 1 && F.launch_refused == 1);
    CHECK(!strcmp(sh_modpub_test_notice(), "This map was not loaded: it needs mods that are not on this computer and "
                                           "cannot be downloaded. Ask its author to publish it again."));

    /* A parse no gate saw, whose row no build serves, is held. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_launch_gate(SH_MODPUB_LOBBY_LAUNCH, MANAGER, NULL, NULL) == 0 && sh_modpublished_holding());

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

static void missing_pinned_rows_fetch_at_once(void)
{
    printf("-- missing pinned rows: hold, the fetch starts at the next tick with no question, activates and resumes\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    {
        sh_modpublished_parse_info info;
        sh_modpublished_last_parse(&info);
        CHECK(info.missing == 1);
    }
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.hold_ui == 1 && F.metadata_copies == 1);
    CHECK(F.asks == 0 && F.fetch_calls == 0 && sh_modpub_test_state() == 1);   /* nothing inside the completion */
    tick(1);
    CHECK(F.asks == 0 && sh_modpub_test_state() == 2);   /* the next tick starts the download */
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.asks == 0);
    CHECK(F.rescans == 1 && F.activations == 1 && F.activated_count == 1);
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_ON_SNAP_PLAY);
    CHECK(!strcmp(F.resume_text, PINNED_MAP));     /* the full text, rows included */
    CHECK(!strcmp(F.resume_id, METADATA));
    CHECK(F.metadata_frees == 1 && F.releases == 0 && F.drop_ui == 0);
    CHECK(!sh_modpublished_holding() && !sh_modpub_test_notice()[0]);

    /* The next completion of the same map passes: nothing is missing now. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
}

static void downloads_are_for_a_map(void)
{
    sh_modmanifest_list rows;
    printf("-- a held map's fetch runs for its pinned row and the map resumes with the pin kept\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.asks == 0 && F.fetch_calls == 1 && F.resumes == 1);
    CHECK(!strcmp(F.fetch_salt, SALT_A));
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_current(&rows) == 1 && rows.rows[0].pinned && !strcmp(rows.rows[0].salt, SALT_A));   /* its pin is kept */
    sh_modmanifest_list_free(&rows);
}

static void one_notice_queue(void)
{
    printf("-- the mod system's one notice queue: a notice from elsewhere is raised with one Accept button\n");
    reset();
    sh_modpublished_notice("This map is 10.1 MB, over the map service's 10.0 MB limit by 0.1 MB, so it cannot be "
                           "copied to My Maps.");
    tick(1);
    CHECK(F.asks == 1 && F.ask_buttons == 0 && strstr(F.last_ask, "cannot be copied to My Maps") != NULL);
    sh_modpublished_notice("first");
    sh_modpublished_notice("second");   /* a second notice joins one not raised yet (P2-10) */
    F.answer = 1;
    tick(3);
    CHECK(F.asks == 2 && !strcmp(F.last_ask, "first second"));
    {
        /* Two that do not fit 250 bytes together: the later replaces the earlier. */
        char big[200];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = 0;
        char big2[200];
        memset(big2, 'b', sizeof big2 - 1);
        big2[sizeof big2 - 1] = 0;
        sh_modpublished_notice(big);
        sh_modpublished_notice(big2);
        F.answer = 1;
        tick(3);
        CHECK(F.asks == 3 && !strcmp(F.last_ask, big2));
    }
    /* P2-9: a notice kept past the busy-tick limit is raised when the game can ask. */
    reset();
    F.can_ask_after = 1000;
    sh_modpublished_notice("kept until the surface is free");
    tick(900);
    CHECK(F.asks == 0 && !strcmp(sh_modpub_test_notice(), "kept until the surface is free"));
    tick(200);
    CHECK(F.asks == 1 && !strcmp(F.last_ask, "kept until the surface is free"));

    /* R-LB1: a notice is raised on a settled screen, and withdrawn when the game changes screen under it, so nothing
     * of it draws through the game's own screens (the RECOMMEND MAP popup after a published play) */
    reset();
    F.screen = -1;   /* between screens */
    sh_modpublished_notice("on the next screen");
    tick(5);
    CHECK(F.asks == 0);
    F.screen = 0x100000041ll;
    tick(1);
    CHECK(F.asks == 1);
    tick(3);
    CHECK(F.releases == 0);   /* still up on its screen */
    F.screen = 0x200000042ll;   /* the game moved on */
    tick(1);
    CHECK(F.releases == 1 && said_log("the notice was withdrawn: the game changed screen under it"));
    tick(3);
    CHECK(F.asks == 1);   /* not raised again */
    F.screen = 0;
}

static void a_copied_map_fetches_then_passes(void)
{
    printf("-- a stranger's map copied to My Maps with a mod: fetched and resumed, and its next play passes\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.asks == 0);
    CHECK(!sh_modpub_test_notice()[0]);
    /* The next play of it passes at once: its content is here. */
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
}

static void fetch_failure_drops(void)
{
    printf("-- a failed fetch, or content that will not serve, drops with a notice\n");
    /* A fetch fails only when it was stopped or memory ran out (what cannot be had stands in). */
    reset();
    use_units();
    F.fetch_fails = MODNET_E_CANCELLED;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.activations == 0 && F.drop_ui == 1);
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_ON_SNAP_PLAY);
    CHECK(F.asks == 1 && F.ask_buttons == 0);
    CHECK(strstr(F.last_ask, "The mod content of Eternal Demons could not be prepared: ") == F.last_ask &&
          strstr(F.last_ask, "The map was not loaded.") != NULL && !strstr(F.last_ask, HASH_A));
    if (getenv("MODPUB_TEST_WORDS")) printf("    fetch failed: %s\n", F.last_ask);

    reset();
    use_units();
    F.fetch_fails = MODNET_E_NOMEM;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.resumes == 0 && strstr(F.last_ask, "The mod content of Eternal Demons could not be prepared: ") == F.last_ask);
    if (getenv("MODPUB_TEST_WORDS")) printf("    fetch failed: %s\n", F.last_ask);

    /* The second of two rows fails to prepare: the first's name is not blamed. */
    reset();
    use_units();
    F.fetch_fails = MODNET_E_CANCELLED;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_TWO, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0);   /* the first failure ends the download */

    /* The fetch returned but a row is still not served: it was downloaded but cannot be used. */
    reset();
    use_units();
    g_content.unserve_after_fetch = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.rescans == 1 && F.resumes == 0 && F.activations == 0 && F.drop_ui == 1);
    CHECK(!strcmp(F.last_ask, "Eternal Demons was downloaded but cannot be used. The map was not loaded."));
}

static void second_load_abandons(void)
{
    printf("-- another load abandons the hold, also mid-fetch\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    parse(SH_MODPUB_PARSE_OTHER, VANILLA, (void *)0x7000);   /* the player opened something else */
    CHECK(!sh_modpublished_holding() && F.metadata_frees == 1 && F.drop_ui == 0);
    tick(3);
    CHECK(F.asks == 0 && F.fetch_calls == 0 && !sh_modpub_test_notice()[0]);

    reset();
    use_units();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(sh_modpub_test_state() == 2);
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
    use_units();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(sh_modpub_test_state() == 2);
    for (int i = 0; i < 200 && !F.fetch_calls; i++) Sleep(1);
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, PINNED_MAP, (void *)0x7200);   /* Play pressed again */
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 2);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, (void *)0x7200, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.asks == 0 && F.hold_ui == 1);
    SetEvent(F.fetch_gate);
    tick_until_idle();
    CHECK(F.fetch_cancelled == 0 && F.fetch_calls == 1 && F.resumes == 1);
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;
}

static void joiner_host_mismatch_drops(void)
{
    printf("-- a joiner re-checks the lobby host's map before resuming\n");
    /* A read of a map the lobby no longer hosts is not held. */
    reset();
    F.host_ok = 0;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.hold_ui == 0);

    /* The host chose another map while the content downloaded in the lobby: no resume; the joiner leaves
     * with the sentence that tells it to join again. */
    reset();
    use_lobby();
    join_pinned();
    F.host_ok = 0;
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.drop_ui == 1 && g_leaves == 1 && g_scope_plays == 0);
    CHECK(SAID("The mods are installed, but the host changed maps while they downloaded. Join the lobby again."));

    /* A row that is still not served after the download leaves the lobby too. */
    reset();
    use_lobby();
    g_content.unserve_after_fetch = 1;
    join_pinned();
    tick_until_idle();
    tick(2);
    CHECK(!sh_modpublished_holding() && g_leaves == 1 && F.resumes == 0);

    /* A joiner whose row names no unit leaves the lobby at once, with the sentence that names the mod. */
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(!sh_modpublished_holding() && g_leaves == 1 && F.fetch_calls == 0 && F.resumes == 0);
    CHECK(strstr(F.last_ask, "This map needs alex.pink-imp, which is not on this computer and cannot be downloaded") == F.last_ask);
    CHECK(strstr(F.last_ask, "You left the lobby so the match is not held up; join it again later.") != NULL);

    /* A joiner runs no mod script, so its scripts setting never refuses the
     * host's map: with scripts off and scripted builds in place, it passes. */
    reset();
    F.scripts_off = 1;
    F.served_scripts = 3;
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(g_leaves == 0 && !sh_modpublished_holding());
    /* Its map passed for the lobby it is in: it serves in the Play scope, as the host's launch does. */
    CHECK(g_scope_plays == 1);
}

static void warm_cache_joiner(void)
{
    printf("-- the warm-cache joiner branch: a map no gate of this process saw is read again and gated\n");
    reset();
    use_lobby();
    /* Never seen (cached in an earlier session): read cold, so CacheReady's gate checks it. */
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 0);
    CHECK(g_cold_reads == 1 && !sh_modpublished_holding());
    /* With no way to read it again, the old pass (and its log line) stands. */
    g_cold_ok = 0;
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 1);
    g_cold_ok = 1;
    /* A browser read of the item: held, its content fetched, resumed. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_BROWSER_PLAY && !sh_modpublished_holding());
    /* Joining that item's lobby with the map cached, seen with its content gone from the cache, and its rows the
     * ones last read: held; the joiner downloads in the lobby (it does not leave) and answers when it is here. */
    g_content.complete = 0;
    g_cold_reads = 0;
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 0);
    CHECK(sh_modpublished_holding() && g_cold_reads == 0);
    tick_until_idle();
    CHECK(g_leaves == 0 && F.asks == 0 && F.fetch_calls == 2);
    CHECK(F.resumes == 2 && F.resume_hook == SH_MODPUB_CACHE_LOBBY);
    /* Seen, nothing missing now, but another map was loaded since: read again. */
    parse(SH_MODPUB_PARSE_OTHER, VANILLA, MAP);
    g_cold_reads = 0;
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 0 && g_cold_reads == 1);
    /* Seen, nothing missing, its rows the ones last listed: passes, in the Play scope. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    g_cold_reads = g_scope_plays = 0;
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev1") == 1);
    CHECK(g_cold_reads == 0 && g_scope_plays == 1);
    /* Another revision of the item is a different map, never seen: read cold. */
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_LOBBY, MANAGER, NULL, NULL, NULL, METADATA, "rev2") == 0 && g_cold_reads == 1);
}

static void my_maps_play(void)
{
    printf("-- My Maps play: fetch, resume with the model; a changed model abandons; a row with no unit is refused\n");
    reset();
    use_units();
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    CHECK(F.metadata_copies == 0);
    tick_until_idle();
    CHECK(F.asks == 0 && F.resumes == 1 && F.resume_hook == SH_MODPUB_LOCAL_OFFLINE && F.resume_extra == MODEL);
    CHECK(!strcmp(F.resume_text, PINNED_MAP));

    reset();
    use_units();
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    F.token = 43;   /* the browser moved on to another saved map */
    tick_until_idle();
    CHECK(F.resumes == 0 && F.activations == 0 && !sh_modpublished_holding());

    /* A map saved before its content went to the store: refused, the launch's declared rows released. */
    reset();
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    CHECK(F.launch_refused == 0);   /* held: not refused yet */
    tick(1);
    CHECK(F.resumes == 0 && !sh_modpublished_holding() && F.fetch_calls == 0);
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_LOCAL_OFFLINE && F.drop_ui == 1);
    CHECK(!strcmp(F.last_ask, LOCAL_ONE));
}

static void a_newer_launch_replaces(void)
{
    printf("-- a newer held launch replaces an unfinished one\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    /* Its own parse already abandons; the decide supersedes even without one. */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_TWO, (void *)0x2100);
    CHECK(!sh_modpublished_holding() && F.metadata_frees == 1);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, (void *)0x2100, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.asks == 0 && F.fetch_calls == 2 && F.resumes == 1 && F.activated_count == 2 &&
          !strcmp(F.resume_text, PINNED_TWO));
}

static void scripts_off_at_launch(void)
{
    printf("-- scripts off: a map whose pinned content arrived earlier does not launch either; local rows never count\n");
    /* Its content is here already: nothing is missing, nothing asked. On: it plays. */
    reset();
    use_units();
    g_content.complete = 1;
    F.served_scripts = 4;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    reset();
    use_units();
    g_content.complete = 1;
    F.served_scripts = 4;
    F.scripts_off = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(F.served_scripts_calls == 1 && F.fetch_calls == 0 && F.resumes == 0 && F.drop_ui == 1);
    CHECK(!strcmp(sh_modpub_test_notice(), SCRIPTS_OFF_TEXT));
    CHECK(strstr(F.prints_all, "which you turned off. To play it") != NULL);
    /* A map without scripts in its content still launches with the setting off. */
    reset();
    use_units();
    g_content.complete = 1;
    F.scripts_off = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    /* A map whose rows are local only (the player's own) is never refused, played from My Maps or read as a
     * published map: the scripts are not asked about. */
    reset();
    F.installed_a = 1;
    F.served_scripts = 4;
    F.scripts_off = 1;
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, (void *)0x5000, NULL, NULL) == 1);
    CHECK(F.served_scripts_calls == 0);
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(F.served_scripts_calls == 0 && F.drop_ui == 0);
}

/* Hold a published play of `text` whose fetch waits on F.fetch_gate, and bring the progress line up. */
static void progress_line_for(const char *text)
{
    reset();
    use_units();
    F.fetch_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, text, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
}

static void finish_gated_fetch(void)
{
    SetEvent(F.fetch_gate);
    tick_until_idle();
    CloseHandle(F.fetch_gate);
    F.fetch_gate = NULL;
}

static void download_progress(void)
{
    ULONGLONG t0;
    int shows;
    printf("-- the download's progress line is updated in place, with no question\n");
    progress_line_for(PINNED_MAP);
    F.prog_set = 1;
    F.prog_done = 84ull << 20;
    F.prog_total = 212ull << 20;
    tick(1);
    wait_progress_sent();
    tick(1);
    /* The progress line comes up at once; no dialog, now or later. */
    CHECK(sh_modpub_test_state() == 2 && F.asks == 0 && F.progress_up && F.progress_hides == 0);
    CHECK(!strcmp(F.last_progress, "Getting this map's mod content: Eternal Demons (1 of 1), 84 of 212 MB."));
    /* A download still running after two seconds: the line is updated in place, never taken down, and the console
     * says it too. */
    shows = F.progress_shows;
    t0 = GetTickCount64();
    for (int i = 0; i < 5000 && F.progress_shows < shows + 1; i++) { tick(1); Sleep(1); }
    CHECK(F.progress_shows >= shows + 1 && GetTickCount64() - t0 >= 1900);
    CHECK(F.asks == 0 && F.progress_hides == 0 && F.progress_up && F.fetch_cancelled == 0 && sh_modpub_test_state() == 2);
    CHECK(strstr(F.prints_all, "Getting this map's mod content: Eternal Demons (1 of 1), 84 of 212 MB.") != NULL);
    finish_gated_fetch();
    CHECK(F.resumes == 1 && F.progress_hides == 1 && !F.progress_up && F.asks == 0 && F.releases == 0);

    printf("-- the progress line without a total says the megabytes alone; with two rows it counts them\n");
    progress_line_for(PINNED_MAP);
    F.prog_set = 1;
    F.prog_done = 5ull << 20;
    F.prog_total = 0;
    tick(1);
    wait_progress_sent();
    tick(1);
    CHECK(!strcmp(F.last_progress, "Getting this map's mod content: Eternal Demons (1 of 1), 5 MB."));
    finish_gated_fetch();
    CHECK(F.resumes == 1);

    progress_line_for(PINNED_TWO);
    tick(1);
    wait_progress_sent();
    tick(1);
    CHECK(!strncmp(F.last_progress, "Getting this map's mod content: Eternal Demons (1 of 2), ", 57));
    finish_gated_fetch();
    CHECK(F.fetch_calls == 2 && F.resumes == 1);

    printf("-- a short download asks nothing and leaves no line up\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.asks == 0 && F.releases == 0 && F.resumes == 1 && !F.progress_up);
}

static void scripts_off_after_the_fetch(void)
{
    printf("-- scripts off: content that arrives with scripts is not played; without scripts, or with them on, it plays\n");
    /* The scripted pinned row's unit is absent at the gate: the row counts as missing and the gate finds no scripts
     * to refuse (nothing is composed yet). The hold's fetch brings the unit; at the hold's recheck the composition
     * carries scripts, and the resume is refused with the setting's sentence. */
    reset();
    use_units();
    g_content.complete = 0;        /* the unit is not in the content cache */
    F.scripts_off = 1;
    F.scripts_after_fetch = 2;     /* the composition carries scripts once the fetch has landed */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    {
        sh_modpublished_parse_info info;
        sh_modpublished_last_parse(&info);
        CHECK(info.missing == 1);
    }
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && F.drop_ui == 0 && F.served_scripts_calls == 1);   /* nothing served yet to refuse */
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.activations == 0 && F.drop_ui == 1);
    CHECK(F.launch_refused == 1 && F.refused_hook == SH_MODPUB_ON_SNAP_PLAY);
    CHECK(F.asks == 1 && F.ask_buttons == 0 && !strcmp(F.last_ask, SCRIPTS_OFF_TEXT));
    CHECK(strstr(F.prints_all, "which you turned off. To play it") != NULL);

    reset();
    use_units();
    F.scripts_off = 1;             /* off, but the content carries no scripts */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.drop_ui == 0);

    reset();
    use_units();
    F.scripts_after_fetch = 2;     /* scripts on: the content plays */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.drop_ui == 0);

    /* The same for a My Maps play of a map whose pinned row is absent: the hold's recheck refuses the resume. */
    reset();
    use_units();
    F.scripts_off = 1;
    F.scripts_after_fetch = 2;
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && F.drop_ui == 1 && F.launch_refused == 1);
    CHECK(!strcmp(F.last_ask, SCRIPTS_OFF_TEXT));

    /* A joiner runs no script: its content is never refused for them. */
    reset();
    use_lobby();
    F.scripts_off = 1;
    F.scripts_after_fetch = 2;
    join_pinned();
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_CACHE_READY && g_leaves == 0);
}

static void rawmap_scripts_follow_the_setting(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap that came with scripted mods does not open while scripts are off; the player's own does\n");
    reset();
    use_units();
    g_content.complete = 1;
    F.served_scripts = 3;
    CHECK(sh_modpub_rawmap_missing(PINNED_MAP, strlen(PINNED_MAP), &rows) == 0);
    CHECK(!sh_modpub_rawmap_scripts_off(&rows));   /* on: it opens */
    F.scripts_off = 1;
    CHECK(sh_modpub_rawmap_scripts_off(&rows) == 1);
    F.served_scripts = 0;
    CHECK(!sh_modpub_rawmap_scripts_off(&rows));   /* its mods carry no scripts */
    sh_modmanifest_list_free(&rows);
    /* The player's own rawmap (local rows only) is never refused. */
    F.installed_a = 1;
    F.served_scripts = 3;
    F.served_scripts_calls = 0;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 0);
    CHECK(!sh_modpub_rawmap_scripts_off(&rows) && F.served_scripts_calls == 0);
    sh_modmanifest_list_free(&rows);
    /* A rawmap without mods, scripts off: nothing to check, nothing asked. */
    F.served_scripts_calls = 0;
    CHECK(sh_modpub_rawmap_missing(VANILLA, strlen(VANILLA), &rows) == 0);
    CHECK(rows.count == 0);
    CHECK(!sh_modpub_rawmap_scripts_off(&rows));
    CHECK(!sh_modpub_rawmap_scripts_off(NULL) && F.served_scripts_calls == 0);
    sh_modmanifest_list_free(&rows);
}

/* A rawmap's pinned rows reach the content step before its hold (R-LV2): the hold fetches a row's units into the
 * content cache only when the cache knows the row, so a rawmap whose rows were not noted took the retired build
 * route. The step gets the pinned row and the names the map places. */
static struct { int calls; size_t rows, pinned, placed; char first[96]; } g_content_seen;
static void content_spy(const sh_modmanifest_list *rows, char *const *placed, size_t count)
{
    size_t i;
    g_content_seen.calls++;
    g_content_seen.rows = rows ? rows->count : 0;
    for (i = 0; rows && i < rows->count; i++) g_content_seen.pinned += rows->rows[i].pinned ? 1 : 0;
    g_content_seen.placed = count;
    if (count && placed[0]) strcpy_s(g_content_seen.first, sizeof g_content_seen.first, placed[0]);
}

static void rawmap_pinned_rows_reach_the_content_step(void)
{
    static const char PINNED[] =
        "{\"entities\":[{\"entityDef\":{\"className\":\"idProp_Coop\",\"inherit\":\"mods/" SALT_A "/crate\","
        "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},\"uniqueId\":1,\"~type\":\"idSnapEntity\"}],"
        "\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
        ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"" SALT_A "\",\"name\":\"Pink\",\"size\":42,"
        "\"objects\":{\"entities/crate\":\"" HASH_A "\"}}]}}";
    sh_modmanifest_list rows;
    printf("-- a rawmap's pinned rows reach the content step before its hold\n");
    reset();
    memset(&g_content_seen, 0, sizeof g_content_seen);
    sh_modmanifest_set_content(content_spy);
    (void)sh_modpub_rawmap_missing(PINNED, strlen(PINNED), &rows);
    CHECK(g_content_seen.calls == 1 && g_content_seen.rows == 1 && g_content_seen.pinned == 1);
    CHECK(g_content_seen.placed == 1 && !strcmp(g_content_seen.first, "mods/" SALT_A "/crate"));
    sh_modmanifest_list_free(&rows);
    /* Local rows whose folders are not here reach the step as rows of no units (D6a, manager Q8): their objects
     * stand in. */
    memset(&g_content_seen, 0, sizeof g_content_seen);
    (void)sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows);
    CHECK(g_content_seen.calls == 1 && g_content_seen.rows == 2 && g_content_seen.pinned == 2);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_set_content(NULL);
}

/* R-LV4: a held load said what it played without twice, once at the hold and once at the load. A load says it once,
 * after the load ran, from what the fetch recorded. */
static void a_held_load_says_its_notice_once(void)
{
    sh_modmanifest_list rows;
    printf("-- a held load says what it plays without once, after the load\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.absent_after_fetch = 1;
    F.token = 42;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick_until_idle();
    tick(2);
    CHECK(F.asks <= 1 && F.ask_buttons != 6 && F.fetch_calls == 1 && F.resumes == 1);   /* units: no question */
    CHECK(g_content.said == 1 && g_content.said_at_resumes == 1);  /* once, after the resumed load */
    CHECK(said_notice("This map is playing without the Tyrant", 38));

    printf("-- a rawmap's hold leaves its notice to the reopened rawmap's load\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.absent_after_fetch = 1;
    CHECK(sh_modpub_rawmap_missing(PINNED_MAP, strlen(PINNED_MAP), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_idle();
    CHECK(F.resumes == 1 && g_content.said == 0 && g_content.pending == 1);
    /* The reopened rawmap is the editor's parse: its one notice on the next frame. */
    g_content.complete = 1;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(2);
    CHECK(g_content.said == 1);

    printf("-- a rawmap whose hold could not get its content does not ask the store again when it reopens\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.absent_after_fetch = 1;
    CHECK(sh_modpub_rawmap_missing(PINNED_MAP, strlen(PINNED_MAP), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_idle();
    CHECK(F.resumes == 1 && F.fetch_calls == 1);
    g_content.pending = 1;                         /* the reopen still stands in: the store could not supply it */
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(2);
    tick_until_idle();
    CHECK(F.fetch_calls == 1 && g_content.said == 1);   /* R-LV6: it fetched twice and said its notice after the second */
    CHECK(said_notice("This map is playing without the Tyrant", 38));
}

static void coop_lobby_joiner_downloads_in_the_lobby(void)
{
    printf("-- co-op: a joiner downloads its map's content in the lobby, tells the host, and answers when it is here\n");
    reset();
    use_lobby();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    join_pinned();
    tick(1);
    /* D27 left the lobby here; now it stays, says nothing, and the host hears it is downloading. */
    CHECK(g_leaves == 0 && !sh_modpub_test_notice()[0]);
    CHECK(sent_code(SH_MODLOBBY_CODE_PROGRESS));
    CHECK(F.resumes == 0);                       /* no answer to the host's map check while it downloads */
    SetEvent(F.fetch_gate);
    tick_until_idle();
    tick(1);
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_CACHE_READY);   /* CacheReady: the answer */
    CHECK(last_code() == SH_MODLOBBY_CODE_HOLDS && g_leaves == 0);
    CHECK(F.asks == 0);                          /* no question, no "Keep downloading?" (D-A36) */
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;

    printf("-- co-op: a joiner whose map's content is here tells the host at once; a vanilla map's host hears nothing\n");
    reset();
    use_lobby();
    g_content.complete = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(g_lobby.count == 1 && last_code() == SH_MODLOBBY_CODE_HOLDS);
    reset();
    use_lobby();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, VANILLA, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(g_lobby.count == 0);
}

static void coop_lobby_joiner_cannot_get_it(void)
{
    printf("-- co-op: a joiner whose map's content is not all to be had tells the host and leaves, in one sentence\n");
    reset();
    use_lobby();
    g_content.absent_after_fetch = 1;            /* the store refused a unit: it stands in */
    join_pinned();
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 0 && g_leaves == 1);
    CHECK(last_code() == SH_MODLOBBY_CODE_FAILED);
    CHECK(said_notice("You left the lobby: this map cannot be played without the Tyrant from Eternal Demons: the mod "
                      "store could not be reached.", 104));
    CHECK(!strstr(F.last_ask, HASH_A) && !strstr(sh_modpub_test_notice(), HASH_A));   /* no hash in player text */

    printf("-- co-op: a joiner whose missing content nobody can get (S3) plays the same stand-ins and answers\n");
    reset();
    use_lobby();
    g_content.absent_after_fetch = 1;
    g_content.causes = 1u << SH_CACHE_ABSENT_REMOVED;
    join_pinned();
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && F.resumes == 1 && F.resume_hook == SH_MODPUB_CACHE_READY && g_leaves == 0);
    CHECK(last_code() == SH_MODLOBBY_CODE_STANDS_IN && !sent_code(SH_MODLOBBY_CODE_FAILED));
    g_content.causes = 1u << SH_CACHE_ABSENT_UNREACHABLE;   /* S4: another player could get it: it leaves, as before */
    reset();
    use_lobby();
    g_content.absent_after_fetch = 1;
    g_content.causes = 1u << SH_CACHE_ABSENT_UNREACHABLE;
    join_pinned();
    tick_until_idle();
    tick(2);
    CHECK(F.resumes == 0 && g_leaves == 1 && last_code() == SH_MODLOBBY_CODE_FAILED);

    printf("-- co-op: a failed download (network, disk, hash) tells the host and leaves the lobby\n");
    reset();
    use_lobby();
    F.fetch_fails = -4;                          /* MODNET_E_NET */
    join_pinned();
    tick_until_idle();
    tick(2);
    CHECK(F.resumes == 0 && g_leaves == 1 && sent_code(SH_MODLOBBY_CODE_FAILED));
    CHECK(!sh_modpublished_holding());
}

static void coop_lobby_joiner_follows_its_lobby(void)
{
    printf("-- co-op: the joiner is no longer in the lobby (left, kicked, the host left): the download stops\n");
    reset();
    use_lobby();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    join_pinned();
    tick(1);
    wait_fetching();
    CHECK(sh_modpublished_holding() && F.fetch_calls == 1);
    g_lobby.place = SH_MODPUB_LOBBY_GONE;
    tick(1);
    CHECK(!sh_modpublished_holding() && g_leaves == 0 && F.resumes == 0);
    for (int i = 0; i < 400 && !F.fetch_cancelled; i++) Sleep(5);
    CHECK(F.fetch_cancelled == 1);               /* the worker stopped; its partial files stay for the next time */
    CHECK(said_notice("You are no longer in the lobby, so this map's mod content stopped downloading.", 77));
    CHECK(!F.progress_up);
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;

    printf("-- co-op: the host chose another map: the old download stops without a word\n");
    reset();
    use_lobby();
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    join_pinned();
    tick(1);
    wait_fetching();
    g_lobby.place = SH_MODPUB_LOBBY_OTHER_MAP;
    tick(1);
    CHECK(!sh_modpublished_holding() && g_leaves == 0 && !sh_modpub_test_notice()[0] && !F.progress_up);
    for (int i = 0; i < 400 && !F.fetch_cancelled; i++) Sleep(5);
    CHECK(F.fetch_cancelled == 1);
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;

    printf("-- co-op: the lobby asks the joiner to load before its content is here: it leaves and keeps downloading\n");
    reset();
    use_lobby();
    CHECK(sh_modpub_joiner_start_held() == 0);  /* no hold: the load runs */
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    join_pinned();
    tick(1);
    CHECK(sh_modpub_joiner_start_held() == 1);
    tick(1);
    CHECK(g_leaves == 1 && sh_modpublished_holding());
    CHECK(said_notice("The host started the match before this map's mod content was here", 65));
    SetEvent(F.fetch_gate);
    tick_until_idle();
    tick(2);
    CHECK(F.resumes == 0 && g_leaves == 1);      /* it joins again; the join finds the content */
    CHECK(said_notice("The map's mod content is here. Join the lobby again to play it.", 63));
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;
}

static void coop_lobby_map_is_modded(void)
{
    printf("-- co-op: a lobby is a modded map's when a gate saw its map with content rows\n");
    reset();
    use_lobby();
    CHECK(!sh_modpub_lobby_map_modded(METADATA, "rev1"));      /* never seen */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, VANILLA, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(!sh_modpub_lobby_map_modded(METADATA, "rev1"));      /* vanilla */
    g_content.complete = 1;
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_MAP, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 1);
    CHECK(sh_modpub_lobby_map_modded(METADATA, "rev1"));
    CHECK(!sh_modpub_lobby_map_modded(METADATA, "rev2"));

    printf("-- co-op: a host whose copy stands in waits only while some player could get it (R-LB1 F5, SPEC 5.19)\n");
    {
        /* Every absence cause: a start waits (with the cause's own sentence) while some player could get the content,
         * and runs (solo too: every published play is a lobby the player hosts) when nobody can. */
        static const struct { int cause; int expect; const char *says; } CASES[] = {
            { SH_CACHE_ABSENT_REMOVED,      SH_MODPUB_HOST_SHARED,  NULL },   /* S3 */
            { SH_CACHE_ABSENT_NOT_IN_STORE, SH_MODPUB_HOST_SHARED,  NULL },   /* S5 */
            { SH_CACHE_ABSENT_GAME,         SH_MODPUB_HOST_SHARED,  NULL },   /* S4b */
            { SH_CACHE_ABSENT_UNREACHABLE,  SH_MODPUB_HOST_REFUSED, "This map cannot be hosted yet: the mod store could not "
                                                                    "be reached for " },   /* S4 */
            { SH_CACHE_ABSENT_OFFLINE,      SH_MODPUB_HOST_REFUSED, "This map cannot be hosted yet: " },   /* S2 */
            { SH_CACHE_ABSENT_DISK,         SH_MODPUB_HOST_REFUSED, "there is not enough free disk space for " },
            { SH_CACHE_ABSENT_NEWER,        SH_MODPUB_HOST_REFUSED, "needs a newer Snapmap+" },   /* S6 */
            { SH_CACHE_ABSENT_LOCAL,        SH_MODPUB_HOST_REFUSED, "Open the map once online to get it" },
            { SH_CACHE_ABSENT_BROKEN,       SH_MODPUB_HOST_REFUSED, "Open the map once online to get it" },
            { 0,                            SH_MODPUB_HOST_REFUSED, "Open the map once online to get it" },   /* not known */
        };
        char out[256];
        size_t i;
        CHECK(sh_modpub_lobby_host_content(METADATA, "rev1", out, sizeof out) == SH_MODPUB_HOST_WHOLE && !out[0]);
        CHECK(sh_modpub_lobby_host_content(METADATA, "rev2", out, sizeof out) == SH_MODPUB_HOST_WHOLE);   /* never seen */
        g_content.complete = 0;
        for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
            int got;
            g_content.causes = 1u << CASES[i].cause;
            got = sh_modpub_lobby_host_content(METADATA, "rev1", out, sizeof out);
            CHECK(got == CASES[i].expect);
            CHECK(CASES[i].says ? strstr(out, CASES[i].says) != NULL && strlen(out) < 256 : !out[0]);
            CHECK(!strstr(out, "Play the map once online"));   /* the old advice, wrong for a removed unit */
            if (got != CASES[i].expect || (CASES[i].says && !strstr(out, CASES[i].says)))
                printf("   cause %d: %d \"%s\"\n", CASES[i].cause, got, out);
        }
        /* a row standing in for a cause nobody can get beside one somebody could: the start waits */
        g_content.causes = (1u << SH_CACHE_ABSENT_REMOVED) | (1u << SH_CACHE_ABSENT_UNREACHABLE);
        CHECK(sh_modpub_lobby_host_content(METADATA, "rev1", out, sizeof out) == SH_MODPUB_HOST_REFUSED &&
              strstr(out, "could not be reached"));
        /* no composition on disk: the start waits */
        g_content.causes = 0;
        CHECK(sh_modpub_lobby_host_content(METADATA, "rev1", out, sizeof out) == SH_MODPUB_HOST_REFUSED);
        CHECK(sh_modpub_standin_shared(SH_CACHE_ABSENT_REMOVED) && !sh_modpub_standin_shared(SH_CACHE_ABSENT_UNREACHABLE));
        g_content.complete = 1;
    }
}

/* D-A36 (R-LV6): a rawmap's units downloaded with "Keep downloading?" raised after two seconds. A map's content
 * downloads with a progress line only. */
static void units_download_without_a_question(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap's content downloads with a progress line and no question\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(sh_modpub_rawmap_missing(PINNED_MAP, strlen(PINNED_MAP), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick(1);
    Sleep(2200);                                 /* past PUB_PROGRESS_MS */
    tick(3);
    CHECK(F.asks == 0 && F.progress_shows >= 1);
    SetEvent(F.fetch_gate);
    tick_until_idle();
    CHECK(F.resumes == 1);
    CloseHandle(F.fetch_gate); F.fetch_gate = NULL;
}

/* R-LV4: a map opened from My Maps whose Tyrant could not be had never asked the store again. An editor open that
 * stands in for objects asks the store at once, with no question, and its one notice is the answer. */
static void editor_open_fetches_its_content(void)
{
    printf("-- an editor open that stands in for objects asks the store, with no question\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;            /* the open's content step stood in for the Tyrant */
    g_content.refresh_answer = 1;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(1);
    CHECK(g_content.said == 0);       /* the open's notice waits for the store */
    tick_until_idle();
    tick(2);
    CHECK(F.asks == 0 && F.fetch_calls == 1 && g_content.refreshes == 1 && F.resumes == 0);
    CHECK(g_content.said == 0 && !sh_modpub_test_notice()[0]);   /* everything came: nothing to say */

    printf("-- what the store could not supply is the open's one notice\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.absent_after_fetch = 1;
    g_content.refresh_answer = 0;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(1);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && g_content.said == 1 && F.asks == 1 && F.ask_buttons == 0);   /* the notice alone */
    CHECK(said_notice("This map is playing without the Tyrant", 38));

    printf("-- a map read outside the editor (the hub after a published play) asks nothing and says nothing (R-LB1)\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.absent_after_fetch = 1;
    F.editor_open = 0;
    g_last_logs[0] = 0;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(1);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 0 && F.asks == 0 && !sh_modpub_test_notice()[0]);
    {
        /* idle at the hub, the editor never showing the map: the read is dropped, said nowhere, even if an editor
         * shows a map later (the editor's own open parses again) */
        int i;
        for (i = 0; i < 700 && !strstr(g_last_logs, "a map read outside the editor"); i++) { tick(1); Sleep(10); }
        CHECK(strstr(g_last_logs, "a map read outside the editor (the hub after a play) says nothing") != NULL);
        CHECK(g_content.pending == 0);   /* its stand-ins' notice is dropped */
        F.editor_open = 1;
        tick(3);
        CHECK(F.fetch_calls == 0 && F.asks == 0 && g_content.said == 0 && !sh_modpub_test_notice()[0]);
    }

    printf("-- an editor open whose screen comes up frames after its parse (a load between) says its notice then\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.absent_after_fetch = 1;
    g_content.refresh_answer = 0;
    F.editor_open = 0;
    F.busy = 1;                       /* the level load */
    g_last_logs[0] = 0;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    {
        int i;
        for (i = 0; i < 500; i++) { tick(1); if (i % 50 == 0) Sleep(50); }   /* a long load: polls and time pass */
        CHECK(F.fetch_calls == 0 && F.asks == 0 && !strstr(g_last_logs, "outside the editor"));   /* busy counts not */
        F.busy = 0;
        tick(20);                     /* idle frames before the editor screen is up */
        CHECK(F.fetch_calls == 0 && !strstr(g_last_logs, "outside the editor"));
    }
    F.editor_open = 1;                /* the editor shows the map */
    tick(1);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 1 && g_content.said == 1 && F.asks == 1 && F.ask_buttons == 0);
    CHECK(said_notice("This map is playing without the Tyrant", 38));

    printf("-- an editor open does not ask the store again for what moderation removed (S3)\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.causes = 1u << SH_CACHE_ABSENT_REMOVED;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(1);
    tick_until_idle();
    tick(2);
    CHECK(F.fetch_calls == 0 && F.asks == 1);   /* the open's one notice, from what is here */
    g_content.causes = 0;

    printf("-- the editor shows what arrived at the next open: it says so\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.refresh_answer = 2;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(1);
    tick_until_idle();
    tick(2);
    CHECK(said_notice("This map's mod content is here now. It shows when you open the map again.", 80));

    printf("-- the editor left the map before the content arrived: nothing is said\n");
    reset();
    use_content();
    g_content.armed = 1;
    g_content.served = 1;
    g_content.pending = 1;
    g_content.refresh_answer = 1;
    F.fetch_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(2);
    sh_modmanifest_current_close();
    SetEvent(F.fetch_gate);
    tick_until_idle();
    CloseHandle(F.fetch_gate);
    F.fetch_gate = NULL;
    tick(2);
    CHECK(F.fetch_calls == 1 && g_content.refreshes == 0 && !sh_modpub_test_notice()[0] && F.asks == 0);

    printf("-- a complete map asks nothing\n");
    reset();
    use_content();
    g_content.complete = 1;
    parse(SH_MODPUB_PARSE_OTHER, PINNED_MAP, MAP);
    tick(2);
    CHECK(F.fetch_calls == 0 && !sh_modpublished_holding());
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
    F.installed_a = 1;
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(F.activations == 0);   /* counting changes nothing */
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


static void rawmap_without_content_is_refused(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap whose missing rows name no unit is refused at the next tick, nothing fetched\n");
    reset();
    CHECK(sh_modpub_rawmap_missing(TWO_MODS, strlen(TWO_MODS), &rows) == 2);
    CHECK(sh_modpub_rawmap_hold(&rows, "shared.json") == 1);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 1 && F.asks == 0 && F.fetch_calls == 0);
    CHECK(F.prints == 2);               /* one line per row the asset rule names; no store is asked */
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.resumes == 0 && F.activations == 0);
    CHECK(F.drop_ui == 1 && F.launch_refused == 0);   /* a rawmap load is not a launch */
    CHECK(F.prints == 3 && !strcmp(F.last_print,
          "This map needs alex.pink-imp, bob.room, which are not on this computer and cannot be downloaded: the map was "
          "saved before their content was sent to the mod store. Ask its author to publish it again. The map was not loaded."));
    CHECK(F.asks == 1 && F.ask_buttons == 0 && !strcmp(F.last_ask, F.last_print));

    printf("-- a rawmap with pinned rows and one local row is refused for the local row alone\n");
    reset();
    use_units();
    CHECK(sh_modpub_rawmap_missing(PINNED_AND_LOCAL, strlen(PINNED_AND_LOCAL), &rows) == 2);
    CHECK(sh_modpub_rawmap_hold(&rows, "mixed.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.resumes == 0);
    CHECK(strstr(F.last_ask, "This map needs Bob Room, which is not on this computer and cannot be downloaded") == F.last_ask &&
          !strstr(F.last_ask, "Eternal Demons"));
}

static void rawmap_pinned_rows_download_and_reopen(void)
{
    sh_modmanifest_list rows;
    printf("-- a rawmap's pinned rows download with no question, then the rawmap reopens\n");
    reset();
    use_units();
    CHECK(sh_modpub_rawmap_missing(PINNED_MAP, strlen(PINNED_MAP), &rows) == 1);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 1);
    sh_modmanifest_list_free(&rows);
    tick_until_idle();
    CHECK(F.asks == 0 && F.fetch_calls == 1 && F.rescans == 1);
    CHECK(F.activations == 0);           /* the reopened rawmap's own parse activates */
    CHECK(F.resumes == 1 && F.resume_hook == SH_MODPUB_RAWMAP_LOAD && !strcmp(F.resume_id, "rawmap.json"));
    CHECK(!F.resume_text[0] && F.metadata_copies == 0 && F.hold_ui == 0);

    printf("-- nothing missing after all: no hold\n");
    reset();
    F.installed_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 0);
    CHECK(sh_modpub_rawmap_hold(&rows, "rawmap.json") == 0 && !sh_modpublished_holding());
    sh_modmanifest_list_free(&rows);
}

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
    sh_modmanifest_row_set_name(&row, "alex.pink-imp");
    strcpy_s(row.build, sizeof row.build, HASH_A);
    /* The note says what this computer knows, never "permanent" or "never published". */
    CHECK(HO(why, sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, HO_P(0))) == 0 && !hash[0] &&
          !strcmp(why, "This map needs alex.pink-imp, which is not installed on this computer, so the editor "
                       "opens the map without it. Playing the map asks the mod store for it."));
    CHECK(HO(why, sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, HO_P(0))) == 0 &&
          strstr(why, "is not installed on this computer") && !strstr(why, "permanent") && !strstr(why, "Recycle Bin"));
    /* The map places something of the mod: the engine's validator refuses it, so the
     * sentence says the editor cannot open the map and what to do, and fits the notice. */
    CHECK(HO(why, sh_modpub_editor_substitute(&row, hash, sizeof hash, 1, HO_P(0))) == 0 && !hash[0] &&
          !strcmp(why, "This map needs alex.pink-imp, which is not installed on this computer, so the editor cannot "
                       "open the map. Restore the mod's folder from the Recycle Bin, or play the map from My Maps to "
                       "get it from the mod store.") && strlen(why) < 256);
    /* A row without a name says its salt. */
    row.name[0] = 0;
    CHECK(HO(why, sh_modpub_editor_substitute(&row, hash, sizeof hash, 0, HO_P(0))) == 0 &&
          !strncmp(why, "This map needs a mod, which", 27));
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
        MEMBER(ROW(SALT_A, "alex.pink-imp")) "}";
    printf("-- the editor cannot open a map that places a mod not installed: one plain notice, not \"damaged\"\n");
    reset();
    g_map_notes = 0;
    g_refused_loads = 0;
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, PLACED, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(!sh_modpub_test_notice()[0]);
    tick(1);
    CHECK(g_map_notes == 1);
    /* The engine's "damaged" notice for the same failed load is dismissed (R-PG1, R-CR1 live). */
    CHECK(g_refused_loads == 1);
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
    CHECK(g_refused_loads == 1);
    reset();
    /* A gate that takes the parse (a published play) says its own cause: no editor notice. */
    sh_modmanifest_set_editor_load(1);
    parse(SH_MODPUB_PARSE_OTHER, PLACED, MAP);
    sh_modmanifest_set_editor_load(0);
    CHECK(sh_modpub_decide(SH_MODPUB_BROWSER_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(!sh_modmanifest_load_refusal(NULL));
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

/* A published play, a My Maps play and a joiner whose missing row names no unit (a local row for which no stand-in
 * could be made: the seam serves none): the hold is refused at the next tick with one sentence, and nothing is
 * fetched. */
static void a_row_that_names_no_unit_is_refused(void)
{
    sh_modmanifest_list rows;
    printf("-- a published play whose missing row names no unit is refused at the next tick; nothing is fetched\n");
    reset();
    use_units();    /* the content step is bound: the row is still not a unit's */
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    CHECK(sh_modpublished_holding() && sh_modpub_test_state() == 1 && F.hold_ui == 1);
    CHECK(F.drop_ui == 0 && F.launch_refused == 0 && F.asks == 0);   /* held: not refused until the tick */
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.worker == NULL && F.rescans == 0);
    CHECK(F.resumes == 0 && F.activations == 0 && F.metadata_frees == 1);
    CHECK(F.drop_ui == 1 && F.launch_refused == 1 && F.refused_hook == SH_MODPUB_ON_SNAP_PLAY);
    CHECK(F.asks == 1 && F.ask_buttons == 0 && !strcmp(F.last_ask, LOCAL_ONE) && !strcmp(F.last_print, LOCAL_ONE));
    tick(5);
    CHECK(F.asks == 1 && F.fetch_calls == 0);   /* said once */

    printf("-- the same for the other published hooks, and for a joiner (who leaves the lobby)\n");
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_OFFLINE, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_FEED_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.fetch_calls == 0 && F.launch_refused == 1 && F.refused_hook == SH_MODPUB_FEED_PLAY);
    CHECK(strstr(F.last_ask, "This map needs alex.pink-imp, bob.room, which are not on this computer") == F.last_ask);
    reset();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_CACHE_READY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.fetch_calls == 0 && g_leaves == 1 && F.drop_ui == 1 && F.resumes == 0);

    printf("-- a format 2 local row reads as not pinned; a pinned row beside it is not blamed\n");
    reset();
    use_units();
    parse(SH_MODPUB_PARSE_PUBLISHED_ONLINE, PINNED_AND_LOCAL, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_ON_SNAP_PLAY, MANAGER, METADATA, MAP, NULL, NULL, NULL) == 0);
    tick(1);
    CHECK(!sh_modpublished_holding() && F.fetch_calls == 0 && F.launch_refused == 1);
    CHECK(strstr(F.last_ask, "This map needs Bob Room, which is not on this computer and cannot be downloaded") == F.last_ask);
    CHECK(!strstr(F.last_ask, "Eternal Demons"));

}

/* The player's own content: a My Maps play and a rawmap load whose rows a build serves are not held, and a row
 * nothing serves (no stand-in could be made: the seam serves none) is refused for that mod alone. */

static void own_content_follows_the_installed_builds(void)
{
    sh_modmanifest_list rows;
    printf("-- the player's own content whose rows are served is not held: a My Maps play passes\n");
    reset();
    F.installed_a = 1;
    sh_modmanifest_set_editor_load(1);                 /* the parse detour: a read that is not a published map's */
    parse(SH_MODPUB_PARSE_OTHER, ONE_MOD, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 1);
    CHECK(!sh_modpublished_holding() && F.asks == 0);
    sh_modmanifest_set_editor_load(0);

    printf("-- a row nothing serves is refused for that mod alone; two mods, one served\n");
    reset();
    F.installed_a = 1;
    parse(SH_MODPUB_PARSE_OTHER, TWO_MODS, MAP);
    CHECK(sh_modpub_decide(SH_MODPUB_LOCAL_OFFLINE, MANAGER, NULL, MAP, MODEL, NULL, NULL) == 0);
    tick(1);
    CHECK(F.fetch_calls == 0 && F.resumes == 0 && F.launch_refused == 1);
    CHECK(strstr(F.last_ask, "This map needs bob.room, which is not on this computer and cannot be downloaded") == F.last_ask);
    CHECK(!strstr(F.last_ask, "alex.pink-imp"));

    printf("-- a rawmap load follows the installed builds the same way\n");
    reset();
    F.installed_a = 1;
    CHECK(sh_modpub_rawmap_missing(ONE_MOD, strlen(ONE_MOD), &rows) == 0);
    sh_modmanifest_list_free(&rows);
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
    missing_pinned_rows_fetch_at_once();
    downloads_are_for_a_map();
    fetch_failure_drops();
    second_load_abandons();
    joiner_host_mismatch_drops();
    warm_cache_joiner();
    my_maps_play();
    a_newer_launch_replaces();
    a_row_that_names_no_unit_is_refused();
    download_progress();
    scripts_off_at_launch();
    scripts_off_after_the_fetch();
    rawmap_scripts_follow_the_setting();
    rawmap_missing_counts_rows();
    rawmap_pinned_rows_reach_the_content_step();
    a_copied_map_fetches_then_passes();
    one_notice_queue();
    rawmap_without_content_is_refused();
    rawmap_pinned_rows_download_and_reopen();
    the_refusal();
    gate_editor_substitutes();
    own_content_follows_the_installed_builds();
    gate_notes_after_the_decision();
    editor_open_missing_placed_mod();
    a_held_load_says_its_notice_once();
    a_passing_map_says_its_stand_ins();
    editor_open_fetches_its_content();
    coop_lobby_joiner_downloads_in_the_lobby();
    coop_lobby_joiner_cannot_get_it();
    coop_lobby_joiner_follows_its_lobby();
    coop_lobby_map_is_modded();
    units_download_without_a_question();
    reset();
    sh_modpub_test_set_ops(NULL);
    printf("modpublished_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
