/* modpublished.c -- see modpublished.h.
 *
 * One hold at a time. A hold owns: the manifest rows of its parse, the hashes
 * still missing, a copy of the map text, a copy of the published metadata
 * (completions), the object the completion was called on and, for local play,
 * the saved-map model. Its life:
 *
 *   decide()       the completion is skipped; hold_ui does what the completion
 *                  itself would have done to the screen first (OnSnapPlay
 *                  clears its wait dialog and hides the progress widget)
 *   PROBING        the denial gate (modpublished.h): a worker job asks the
 *                  store about every missing bundle; sh_modpub_gate_decide
 *                  decides each, and any denial drops the hold with its
 *                  cause's sentence before the player is asked anything; a
 *                  row of the player's own content it substitutes leaves the
 *                  missing list (served by its key's installed build), and
 *                  when none is left the hold resumes at once
 *   RAISE          next ticks: wait until the dialog surface is idle, then ask
 *   ASKING         poll the answer
 *   FETCHING       Yes: one worker job fetches every missing bundle in turn
 *   (resume)       the job is done, on a main tick: rescan the index, check
 *                  every row is served (the asset rule), re-check the owner and (joiner)
 *                  the lobby host's item, activate, parse the retained text into
 *                  a fresh map and call the original completion with it
 *
 * No, a failed fetch, or a bundle that will not install drops the hold with a
 * notice naming the mod; the screen is left as the completion's own first
 * steps leave it, with nothing launched. Another map load, or a newer hold,
 * abandons the hold silently (its fetch is cancelled and its job retired when
 * the worker returns).
 *
 * A rawmap load (sh_modpub_rawmap_hold) has no completion to skip: the rawmap
 * layer never handed the map to the engine. Its hold goes through the same
 * PROBING and its resume opens the rawmap again.
 *
 * A prefab placement (sh_modpub_prefab_hold) is held the same way: PROBING,
 * then the question, the fetch and the install; its resume is the ready
 * callback (prefab_mods.c), which brings the mods into the open map and lets
 * the placement go on. Its texts say "prefab" and "placed" where a map's say
 * "map" and "loaded".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modpublished.h"
#include "plain_reason.h"
#include "modmanifest.h"

/* The fetch and the worker come from modsync.c and modnet.c: the fetch
 * downloads the bundle manifest and every blob it names, verifies them by
 * hash and installs the bundle into <data>\mods, stopping early when the
 * cancel flag becomes nonzero; the job helper runs one job at a time on a
 * worker thread. Both are adapted below, inside the engine section. */
#ifndef SH_MODPUBLISHED_PURE
#include "modnet.h"
#include "modsync.h"
#endif

#define PUB_GDM_ID          0x29u   /* GDM_CONFIRM_VIDEO_CHANGES: the shape map_package.c used */
#define PUB_BUTTONS_YES_NO  6u
#define PUB_BUTTONS_ACCEPT  0u
#define PUB_RAISE_TICKS     600     /* about ten seconds at 60 Hz */
#define PUB_PROGRESS_MS     2000ull
#define PUB_SEEN_MAX        16
#define PUB_ID_CAP          96
#define PUB_TITLE_CAP       160     /* a map's title, as its downloads folder is named after it */
#define PUB_TEXT_CAP        256     /* the native dialog body */
#define PUB_CAUSE_CAP       400     /* the asset rule's sentence for one row */

enum { PUB_DIALOG_PENDING = 0, PUB_DIALOG_ACCEPTED = 1 };
enum { HOLD_IDLE = 0, HOLD_RAISE, HOLD_ASKING, HOLD_FETCHING, HOLD_PROBING };

static sh_modpub_ops g_ops;
static int g_ops_set;

/* ------------------------------------------------------------ logging */

static void pub_log(const char *line)
{
    if (g_ops.log) g_ops.log(g_ops.context, line);
}

static void pub_logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    pub_log(line);
}

static void pub_print(const char *line);
static void pub_deny(const int *verdicts, const sh_modpub_probe *probes, const char *detail, const char *why);
static void pub_notice(const char *text);

static int pub_on_main(void)
{
    return g_ops_set && g_ops.on_main && g_ops.on_main(g_ops.context);
}

/* ------------------------------------------------------------ classification */

static const void *g_read_offline_return, *g_read_online_return;

int sh_modpublished_classify(const void *return_address)
{
    if (!return_address) return SH_MODPUB_PARSE_OTHER;
    if (g_read_offline_return && return_address == g_read_offline_return) return SH_MODPUB_PARSE_PUBLISHED_OFFLINE;
    if (g_read_online_return && return_address == g_read_online_return) return SH_MODPUB_PARSE_PUBLISHED_ONLINE;
    return SH_MODPUB_PARSE_OTHER;
}

/* ------------------------------------------------------------ the parse record */

typedef struct pub_record {
    int valid;
    int kind, parsed;
    void *map;
    sh_modmanifest_list rows;
    size_t missing;
    char *text;               /* only when missing > 0 */
    size_t length;
    unsigned long sequence;
    unsigned long frame;      /* g_frame when it was made: a gate uses only a record of its own frame */
    int repeat;               /* the held map parsed again: this launch is held with the running hold */
} pub_record;

static pub_record g_record;
/* Advanced by the main-thread tick after every native frame. A read's parse
 * and the completion (and launch) it feeds run in one call chain, inside one
 * frame; a record from an earlier frame belongs to some other load. */
static unsigned long g_frame;
/* The parse whose cause sentences the manifest holds (ADD-V5 B5): a gate that
 * takes it drops them; a later frame's poll logs them, since no gate did. */
static int g_notes_held;
static unsigned long g_notes_frame;

static void pub_notes_gated(void)
{
    if (!g_notes_held) return;
    g_notes_held = 0;
    (void)sh_modmanifest_load_notes_flush(0);
}
static volatile LONG g_parse_sequence;
static volatile LONG g_resuming;       /* our own parse and original call are running */

/* The asset rule (modserve.h sh_modserve_row_served): a row is served when an
 * installed folder at its bundle hash (or an alias of it) provides every
 * name it lists; a folder of the key at another hash is another build and
 * does not serve it (modserve.h, "installed at another build"). `subject` is
 * the word the cause uses ("map", "prefab"). */
static int pub_row_state(const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{
    if (cause && cap) cause[0] = 0;
    return g_ops.row_state ? g_ops.row_state(g_ops.context, row, subject, cause, cap) : SH_MODPUB_ROW_MISSING;
}

static int pub_row_served(const sh_modmanifest_row *row)
{
    return pub_row_state(row, NULL, NULL, 0) == SH_MODPUB_ROW_SERVED;
}

static size_t pub_count_missing(const sh_modmanifest_list *rows)
{
    size_t missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served(&rows->rows[i])) missing++;
    return missing;
}

int sh_modpub_row_substitute(const sh_modmanifest_row *row, char *hash, size_t hash_cap);
static int pub_is_own(int hook);

/* Served, or (the player's own content) served by the build the gate
 * substituted for it this session. */
static int pub_row_served_for(int hook, const sh_modmanifest_row *row)
{
    return pub_row_served(row) || (pub_is_own(hook) && sh_modpub_row_substitute(row, NULL, 0));
}

static void pub_record_clear(pub_record *r)
{
    sh_modmanifest_list_free(&r->rows);
    free(r->text);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------ maps seen by (item, revision)
 * The warm-cache joiner branch never parses: CacheMapForLobby answers from the
 * in-memory cache. Its manifest is the one this session saw when that item and
 * revision last reached a gated completion. */

typedef struct pub_seen {
    char id[PUB_ID_CAP], revision[PUB_ID_CAP];
    char title[PUB_TITLE_CAP];
    sh_modmanifest_list rows;
    unsigned long stamp;
} pub_seen;

static pub_seen g_seen[PUB_SEEN_MAX];
static unsigned long g_seen_stamp;

static int pub_list_copy(sh_modmanifest_list *dst, const sh_modmanifest_list *src)
{
    memset(dst, 0, sizeof(*dst));
    for (size_t i = 0; i < src->count; i++)
        if (!sh_modmanifest_list_add(dst, &src->rows[i])) { sh_modmanifest_list_free(dst); return 0; }
    return 1;
}

static pub_seen *pub_seen_find(const char *id, const char *revision)
{
    if (!id || !id[0] || !revision) return NULL;
    for (size_t i = 0; i < PUB_SEEN_MAX; i++)
        if (g_seen[i].stamp && !strcmp(g_seen[i].id, id) && !strcmp(g_seen[i].revision, revision))
            return &g_seen[i];
    return NULL;
}

static void pub_seen_note(const char *id, const char *revision, const char *title, const sh_modmanifest_list *rows)
{
    pub_seen *slot;
    if (!id || !id[0] || !revision) return;
    slot = pub_seen_find(id, revision);
    if (!slot) {
        slot = &g_seen[0];
        for (size_t i = 0; i < PUB_SEEN_MAX; i++) {
            if (!g_seen[i].stamp) { slot = &g_seen[i]; break; }
            if (g_seen[i].stamp < slot->stamp) slot = &g_seen[i];
        }
    }
    sh_modmanifest_list_free(&slot->rows);
    memset(slot, 0, sizeof(*slot));
    strcpy_s(slot->id, sizeof slot->id, id);
    strcpy_s(slot->revision, sizeof slot->revision, revision);
    if (title) strncpy_s(slot->title, sizeof slot->title, title, _TRUNCATE);
    if (rows && !pub_list_copy(&slot->rows, rows)) { memset(slot, 0, sizeof(*slot)); return; }
    slot->stamp = ++g_seen_stamp;
}

/* ------------------------------------------------------------ the fetch job */

typedef struct pub_job {
    struct pub_job *next;
    size_t count;
    char title[PUB_TITLE_CAP], map_id[PUB_ID_CAP];   /* the map the fetch is for (its downloads folder) */
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    char (*ids)[SH_MODMANIFEST_ID_CAP];
    volatile int cancel;
    volatile LONG done;
    volatile LONG current;               /* index being fetched */
    volatile LONG64 progress_done, progress_total;
    int failed_index;                    /* -1 when every fetch returned 0 */
    int failed_code;
    int probe;                           /* ask the store (store_probe, else store_has) instead of fetching */
    sh_modpub_probe *probes;             /* probe: the store's answer per bundle */
} pub_job;

static pub_job *volatile g_running_job;  /* the job the worker is in (progress target) */
static pub_job *g_orphans;               /* abandoned jobs the worker still owns */

static void pub_job_free(pub_job *job)
{
    if (!job) return;
    free(job->hashes);
    free(job->ids);
    free(job->probes);
    free(job);
}

static void pub_progress(void *ctx, unsigned long long done, unsigned long long total)
{
    pub_job *job = g_running_job;
    (void)ctx;
    if (!job) return;
    InterlockedExchange64(&job->progress_done, (LONG64)done);
    InterlockedExchange64(&job->progress_total, (LONG64)total);
}

/* Worker thread. Touches only the job. */
static void pub_job_main(void *context)
{
    pub_job *job = context;
    job->failed_index = -1;
    if (job->probe) {
        /* Every bundle is asked about, so the refusal can name all of them. */
        for (size_t i = 0; i < job->count; i++) {
            if (job->cancel) { job->failed_index = (int)i; job->failed_code = -2; break; }
            InterlockedExchange(&job->current, (LONG)i);
            memset(&job->probes[i], 0, sizeof job->probes[i]);
            if (g_ops.store_probe) {
                job->probes[i].found = g_ops.store_probe(g_ops.context, job->hashes[i], &job->cancel, &job->probes[i]);
            } else {
                job->probes[i].found = g_ops.store_has ? g_ops.store_has(g_ops.context, job->hashes[i], &job->cancel) : -1;
            }
            if (job->probes[i].found > 0) job->probes[i].found = SH_MODPUB_STORE_HAS;
            else if (job->probes[i].found < 0) job->probes[i].found = SH_MODPUB_STORE_UNREACHABLE;
        }
        MemoryBarrier();
        InterlockedExchange(&job->done, 1);
        return;
    }
    g_running_job = job;
    for (size_t i = 0; i < job->count; i++) {
        int rc;
        if (job->cancel) { job->failed_index = (int)i; job->failed_code = -2; break; }
        InterlockedExchange(&job->current, (LONG)i);
        InterlockedExchange64(&job->progress_done, 0);
        InterlockedExchange64(&job->progress_total, 0);
        rc = g_ops.fetch_for ? g_ops.fetch_for(g_ops.context, job->hashes[i], job->title, job->map_id, pub_progress,
                                               &job->cancel)
             : g_ops.fetch ? g_ops.fetch(g_ops.context, job->hashes[i], pub_progress, &job->cancel) : -1;
        if (rc != 0) { job->failed_index = (int)i; job->failed_code = rc; break; }
    }
    g_running_job = NULL;
    MemoryBarrier();
    InterlockedExchange(&job->done, 1);
}

static void pub_orphans_retire(void)
{
    pub_job **link = &g_orphans;
    while (*link) {
        pub_job *job = *link;
        if (!InterlockedCompareExchange(&job->done, 0, 0)) { link = &job->next; continue; }
        *link = job->next;
        pub_job_free(job);
    }
}

/* ------------------------------------------------------------ the hold */

typedef struct pub_hold {
    int state;
    int hook;
    void *owner, *extra;
    unsigned long long token;
    void *metadata;                      /* owned copy, completions only */
    char id[PUB_ID_CAP], revision[PUB_ID_CAP];
    char title[PUB_TITLE_CAP];           /* the map's title: its downloads go to mods\downloads\<title>\ */
    char *text;                          /* owned, completions and local play */
    size_t length;
    sh_modmanifest_list rows;
    size_t missing_count;
    size_t *missing;                     /* indexes into rows */
    char (*other)[SH_MODMANIFEST_HASH_CAP];   /* per missing row: the key's installed other build, "" */
    char (*other_name)[128];             /* per missing row: that build's display name, "" */
    sh_modpub_probe *probes;             /* per missing row: the store's answer (after PROBING) */
    unsigned long long missing_bytes;
    int ticket;
    int waited;
    pub_job *job;
    ULONGLONG progress_at;
    size_t substituted;                  /* rows the gate served by the installed build of their key */
} pub_hold;

static pub_hold g_hold;
static sh_modpub_prefab_ready_fn g_prefab_ready;

static int pub_is_prefab(int hook) { return hook == SH_MODPUB_PREFAB_PLACE; }
/* "map" or "prefab", for the messages a hold shows. */
static const char *pub_thing(int hook) { return pub_is_prefab(hook) ? "prefab" : "map"; }
/* The sentence that says nothing happened. */
static const char *pub_not_done(int hook) { return pub_is_prefab(hook) ? "The prefab was not placed." : "The map was not loaded."; }
/* A hold the rawmap layer or a prefab started: no engine completion behind it. */
static int pub_is_local(int hook) { return hook == SH_MODPUB_RAWMAP_LOAD || hook == SH_MODPUB_PREFAB_PLACE; }
/* The player's own content: the gate substitutes the installed build of a key
 * whose declared build cannot be obtained (modpublished.h, SUBSTITUTE). */
static int pub_is_own(int hook) { return hook == SH_MODPUB_LOCAL_OFFLINE || pub_is_local(hook); }

/* The substitutions the gate made this session: declared build -> installed
 * build of the same key. Main thread. */
#define PUB_SUBST_MAX 32
typedef struct pub_subst { char from[SH_MODMANIFEST_HASH_CAP], to[SH_MODMANIFEST_HASH_CAP]; } pub_subst;
static pub_subst g_pub_subst[PUB_SUBST_MAX];
static unsigned g_pub_subst_next;

static void pub_subst_add(const char *from, const char *to)
{
    size_t i;
    if (!from || !from[0] || !to || !to[0]) return;
    for (i = 0; i < PUB_SUBST_MAX; i++)
        if (!strcmp(g_pub_subst[i].from, from)) { strcpy_s(g_pub_subst[i].to, sizeof g_pub_subst[i].to, to); return; }
    i = g_pub_subst_next++ % PUB_SUBST_MAX;
    strcpy_s(g_pub_subst[i].from, sizeof g_pub_subst[i].from, from);
    strcpy_s(g_pub_subst[i].to, sizeof g_pub_subst[i].to, to);
}

int sh_modpub_row_substitute(const sh_modmanifest_row *row, char *hash, size_t hash_cap)
{
    char other[SH_MODMANIFEST_HASH_CAP] = "", name[128];
    size_t i;
    if (hash && hash_cap) hash[0] = 0;
    if (!row || !row->bundle[0] || !g_ops_set) return 0;
    for (i = 0; i < PUB_SUBST_MAX; i++) {
        if (strcmp(g_pub_subst[i].from, row->bundle)) continue;
        /* Still the key's installed build: an uninstall or a newer install
         * makes the gate decide again. */
        if (!g_ops.other_build || !g_ops.other_build(g_ops.context, row, other, sizeof other, name, sizeof name) ||
            strcmp(other, g_pub_subst[i].to))
            return 0;
        if (hash && hash_cap) strcpy_s(hash, hash_cap, g_pub_subst[i].to);
        return 1;
    }
    return 0;
}

/* A refused launch leaves nothing declared (ADD-V4a H3). */
static void pub_launch_refused(int hook)
{
    if (!pub_is_local(hook) && g_ops.launch_refused) g_ops.launch_refused(g_ops.context, hook);
}

/* One pending notice, raised with a single Accept button when the surface is
 * idle and no question is up. A later notice replaces an unraised one. */
static char g_notice[PUB_TEXT_CAP];
static int g_notice_ticket, g_notice_waited;

static void pub_notice(const char *text)
{
    if (!text || !text[0]) return;
    strcpy_s(g_notice, sizeof g_notice, text);
    g_notice_waited = 0;
    pub_logf("MODS: published map notice: %s", text);
}

static const char *pub_hook_name(int hook)
{
    switch (hook) {
    case SH_MODPUB_ON_SNAP_PLAY: return "published play";
    case SH_MODPUB_BROWSER_PLAY: return "browser published play";
    case SH_MODPUB_CACHE_READY: return "lobby join";
    case SH_MODPUB_CACHE_LOBBY: return "lobby join (cached map)";
    case SH_MODPUB_LOCAL_OFFLINE: return "My Maps play";
    case SH_MODPUB_RAWMAP_LOAD: return "rawmap load";
    case SH_MODPUB_PREFAB_PLACE: return "prefab placement";
    case SH_MODPUB_FEED_PLAY: return "SnapMap feed play";
    case SH_MODPUB_LOBBY_LAUNCH: return "lobby launch";
    case SH_MODPUB_OFFLINE_LAUNCH: return "offline launch";
    default: return "?";
    }
}

/* Names of the missing bundles, "a, b and K more", fitting `cap`. */
static void pub_names_line(char *out, size_t cap, const char *const *names, size_t count)
{
    size_t listed = 0;
    out[0] = 0;
    for (size_t i = 0; i < count; i++) {
        char more[32] = "";
        size_t used = strlen(out), separator = listed ? 2 : 0;
        if (i + 1 < count) _snprintf_s(more, sizeof more, _TRUNCATE, " and %u more", (unsigned)(count - listed - 1));
        if (used + separator + strlen(names[i]) + strlen(more) >= cap) break;
        if (listed) strcat_s(out, cap, ", ");
        strcat_s(out, cap, names[i]);
        listed++;
    }
    if (listed < count) {
        char more[32];
        _snprintf_s(more, sizeof more, _TRUNCATE, listed ? " and %u more" : "%u mods", (unsigned)(count - listed));
        if (strlen(out) + strlen(more) < cap) strcat_s(out, cap, more);
    }
}

static void pub_size_text(char *out, size_t cap, unsigned long long bytes)
{
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (!bytes) strcpy_s(out, cap, "size unknown");
    else if (mb < 0.1) strcpy_s(out, cap, "under 0.1 MB");
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", mb);
}

static void pub_compose_question(char *out, size_t cap, const char *thing, const char *const *names, size_t count,
                                 unsigned long long bytes)
{
    static const char tail[] = "). Download and install now?";
    char head[64], size[32], list[PUB_TEXT_CAP];
    size_t room;
    if (!out || !cap) return;
    if (cap > PUB_TEXT_CAP) cap = PUB_TEXT_CAP;
    _snprintf_s(head, sizeof head, _TRUNCATE, "This %s needs %u mod%s: ", thing, (unsigned)count, count == 1 ? "" : "s");
    pub_size_text(size, sizeof size, bytes);
    room = cap - 1;
    room = room > strlen(head) + strlen(size) + strlen(tail) + 2 ? room - strlen(head) - strlen(size) - strlen(tail) - 2 : 1;
    if (room > sizeof list) room = sizeof list;
    pub_names_line(list, room, names, count);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s (%s%s", head, list, size, tail);
}

void sh_modpublished_compose_question(char *out, size_t cap, const char *const *names, size_t count,
                                      unsigned long long bytes)
{
    pub_compose_question(out, cap, "map", names, count, bytes);
}

/* ------------------------------------------------------------ the denial gate */

/* MODNET_ codes the fetch returns (modnet.h); the engine section checks they agree. */
#define PUB_NET_CONFIG      (-3)
#define PUB_NET_NET         (-4)
#define PUB_NET_TIMEOUT     (-5)
#define PUB_NET_CANCELLED   (-6)
#define PUB_NET_HTTP        (-7)
#define PUB_NET_PROTOCOL    (-8)
#define PUB_NET_NOT_FOUND   (-11)
#define PUB_NET_RATE        (-15)
#define PUB_NET_BLOCKED     (-16)
#define PUB_NET_UNAVAILABLE (-17)
#define PUB_NET_GAME        (-18)

int sh_modpub_gate_decide(const sh_modpub_gate_facts *f)
{
    if (!f) return SH_MODPUB_GATE_UNREACHABLE;
    /* The author's own map in the editor: the installed build of the key
     * serves (DESIGN-additive 2.5), whatever the store would say. */
    if (f->editor && f->other_build) return SH_MODPUB_GATE_SUBSTITUTE;
    /* The player's own content (a My Maps play, a rawmap, a prefab) whose
     * declared build cannot be obtained: the installed build serves, as the
     * editor serves it. A build the store has is fetched instead. */
    if (f->own && f->other_build && !(f->store == SH_MODPUB_STORE_HAS && !f->game_lacks))
        return SH_MODPUB_GATE_SUBSTITUTE;
    /* A placeholder system (DESIGN-additive 7, phase 6) answers here, with a
     * verdict of its own, before any denial below. */
    if (f->store == SH_MODPUB_STORE_HAS) return f->game_lacks ? SH_MODPUB_GATE_GAME_REVISION : SH_MODPUB_GATE_OBTAIN;
    if (f->store == SH_MODPUB_STORE_ABSENT) return f->published ? SH_MODPUB_GATE_GONE : SH_MODPUB_GATE_NEVER_PUBLISHED;
    return SH_MODPUB_GATE_UNREACHABLE;
}

void sh_modpub_gate_wording(int verdict, const char *subject, const char *title, const char *source,
                            int store_code, char *out, size_t cap)
{
    char reason[200];
    const int prefab = subject && !strcmp(subject, "prefab");
    const char *what = subject && subject[0] ? subject : "map";
    const char *t = title && title[0] ? title : "a mod";
    if (!out || !cap) return;
    switch (verdict) {
    case SH_MODPUB_GATE_GONE:
        _snprintf_s(out, cap, _TRUNCATE, "This %s needs %s, which is no longer available: its author "
                    "unpublished it, or it was removed. This is permanent on this machine.", what, t);
        break;
    case SH_MODPUB_GATE_UNREACHABLE:
        (void)sh_reason(SH_REASON_STORE, store_code, NULL, 0, reason, sizeof reason);
        _snprintf_s(out, cap, _TRUNCATE, "This %s needs %s, which could not be downloaded: %s.", what, t,
                    reason);
        break;
    case SH_MODPUB_GATE_NEVER_PUBLISHED:
        _snprintf_s(out, cap, _TRUNCATE, "This %s needs %s, which is not installed and was never "
                    "published. Restore its folder from the Recycle Bin or load the %s.", what, t,
                    prefab ? "prefab's ZIP export" : "map's rawmap ZIP");
        break;
    case SH_MODPUB_GATE_GAME_REVISION:
        _snprintf_s(out, cap, _TRUNCATE, "This %s needs %s, which needs a file this game does not hold "
                    "(%s): a different game revision. This is permanent on this machine.", what, t,
                    source && source[0] ? source : "a game file");
        break;
    default:
        out[0] = 0;
        return;
    }
    /* A prefab's denial ends as every other prefab refusal does, saying the
     * placement did not happen (architecture.md "Prefabs that name mods"). */
    if (prefab && strlen(out) + 28 < cap) strcat_s(out, cap, " The prefab was not placed.");
}

int sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                char *why, size_t why_cap)
{
    sh_modpub_gate_facts facts;
    char other[SH_MODMANIFEST_HASH_CAP] = "", name[128] = "", title[300];
    int verdict;
    if (hash_out && hash_cap) hash_out[0] = 0;
    if (why && why_cap) why[0] = 0;
    if (!row || !g_ops_set) return 0;
    memset(&facts, 0, sizeof facts);
    facts.editor = 1;
    facts.store = SH_MODPUB_STORE_ABSENT;   /* not asked: an editor open holds nothing */
    facts.other_build = g_ops.other_build && g_ops.other_build(g_ops.context, row, other, sizeof other, name, sizeof name) &&
                        other[0];
    verdict = sh_modpub_gate_decide(&facts);
    if (verdict == SH_MODPUB_GATE_SUBSTITUTE) {
        if (hash_out && hash_cap) strcpy_s(hash_out, hash_cap, other);
        return 1;
    }
    if (why && why_cap) {
        const char *key = row->id[0] ? row->id : row->bundle;
        if (name[0] && _stricmp(name, key)) _snprintf_s(title, sizeof title, _TRUNCATE, "%s (%s)", name, key);
        else _snprintf_s(title, sizeof title, _TRUNCATE, "%s", key);
        /* The store was not asked, so the note says only what this computer
         * knows; the gate's words ("no longer available", "never published")
         * are said where the store is asked, at Play. A map whose placed
         * entities use the mod is refused by the engine's validator (its
         * damaged-save dialog, which the save guard dismisses), so the
         * sentence says the editor cannot open it and is also the notice;
         * it fits the dialog body, else the key alone names the mod. */
        if (placed) {
            int k;
            for (k = 0; k < 2; k++) {
                _snprintf_s(why, why_cap, _TRUNCATE, "This map needs %s, which is not installed on this computer, so "
                            "the editor cannot open the map. Restore the mod's folder from the Recycle Bin, or play "
                            "the map from My Maps to get it from the mod store.", k ? key : title);
                if (strlen(why) < PUB_TEXT_CAP) break;
            }
        } else
            _snprintf_s(why, why_cap, _TRUNCATE, "This map needs %s, which is not installed on this computer, so "
                        "the editor opens the map without it. Playing the map asks the mod store for it.", title);
    }
    return 0;
}

/* "<name> (<key>)", or the key when no name is known or it is the key. */
static void pub_title(char *out, size_t cap, const sh_modmanifest_row *row, const char *known)
{
    char name[128] = "";
    const char *key = row->id[0] ? row->id : row->bundle;
    if (known && known[0]) strncpy_s(name, sizeof name, known, _TRUNCATE);
    else if (g_ops.mod_name && !g_ops.mod_name(g_ops.context, row, name, sizeof name)) name[0] = 0;
    if (name[0] && _stricmp(name, key)) _snprintf_s(out, cap, _TRUNCATE, "%s (%s)", name, key);
    else _snprintf_s(out, cap, _TRUNCATE, "%s", key);
}

/* Was the map (or this bundle) published? Every map-service hook is; a My
 * Maps map, a rawmap or a prefab only when a claim this machine holds names
 * the bundle (it was published from here and not unpublished). */
static int pub_published(int hook, const sh_modmanifest_row *row)
{
    switch (hook) {
    case SH_MODPUB_ON_SNAP_PLAY: case SH_MODPUB_BROWSER_PLAY: case SH_MODPUB_CACHE_READY:
    case SH_MODPUB_CACHE_LOBBY: case SH_MODPUB_FEED_PLAY: case SH_MODPUB_LOBBY_LAUNCH:
    case SH_MODPUB_OFFLINE_LAUNCH:
        return 1;
    default:
        return g_ops.claimed && row->bundle[0] && g_ops.claimed(g_ops.context, row->bundle);
    }
}

/* The store answer a failed fetch implies. */
static int pub_fetch_answer(int code)
{
    switch (code) {
    case PUB_NET_NOT_FOUND: case PUB_NET_BLOCKED: return SH_MODPUB_STORE_ABSENT;
    case PUB_NET_CONFIG: case PUB_NET_NET: case PUB_NET_TIMEOUT: case PUB_NET_HTTP: case PUB_NET_PROTOCOL:
    case PUB_NET_RATE: case PUB_NET_UNAVAILABLE:
        return SH_MODPUB_STORE_UNREACHABLE;
    default: return SH_MODPUB_STORE_HAS;
    }
}

/* The missing bundles' display names (ids), malloc'd array of borrowed
 * pointers, or NULL. */
static const char **pub_missing_names(const pub_hold *h)
{
    const char **names = calloc(h->missing_count ? h->missing_count : 1, sizeof(*names));
    if (!names) return NULL;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        names[i] = row->id[0] ? row->id : row->bundle;
    }
    return names;
}

/* The question: the ordinary download approval, "This map needs N mod(s):
 * <names> (<size>). Download and install now?". Another installed build of a
 * key is never mentioned: the download lands beside it by hash and the map
 * gets exactly what it pins (owner 2026-11-09). */
static void pub_hold_question(const pub_hold *h, char *out, size_t cap)
{
    const char **names = pub_missing_names(h);
    pub_compose_question(out, cap, pub_thing(h->hook), names, names ? h->missing_count : 0, h->missing_bytes);
    free((void *)names);
}

static void pub_hold_free(void)
{
    pub_hold *h = &g_hold;
    if (h->ticket && g_ops.release) g_ops.release(g_ops.context, h->ticket);
    if (h->job) {
        h->job->cancel = 1;
        if (InterlockedCompareExchange(&h->job->done, 0, 0)) pub_job_free(h->job);
        else { h->job->next = g_orphans; g_orphans = h->job; }
    }
    if (h->metadata && g_ops.metadata_free) g_ops.metadata_free(g_ops.context, h->metadata);
    free(h->text);
    free(h->missing);
    free(h->other);
    free(h->other_name);
    free(h->probes);
    sh_modmanifest_list_free(&h->rows);
    memset(h, 0, sizeof(*h));
}

/* Silent: another load owns the screen now. */
static void pub_abandon(const char *why)
{
    if (g_hold.state == HOLD_IDLE) return;
    pub_logf("MODS: the held %s of '%s' was abandoned: %s", pub_hook_name(g_hold.hook),
             g_hold.id[0] ? g_hold.id : "local map", why);
    pub_hold_free();
}

/* Visible: the player's load is refused and told why. */
static void pub_drop(const char *notice, const char *why)
{
    int hook;
    if (g_hold.state == HOLD_IDLE) return;
    pub_logf("MODS: the held %s of '%s' was dropped: %s", pub_hook_name(g_hold.hook),
             g_hold.id[0] ? g_hold.id : "local map", why);
    if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, g_hold.hook, g_hold.owner);
    hook = g_hold.hook;
    pub_hold_free();
    pub_launch_refused(hook);
    pub_notice(notice);
}

int sh_modpublished_holding(void)
{
    return g_hold.state != HOLD_IDLE;
}

void sh_modpublished_parse_seen(int kind, const char *text, void *out_map, int parsed)
{
    sh_modmanifest_list rows;
    unsigned long sequence = (unsigned long)InterlockedIncrement(&g_parse_sequence);
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) { (void)sh_modmanifest_load_notes_flush(0); return; }
    if (!pub_on_main()) { g_record.valid = 0; return; }
    pub_record_clear(&g_record);
    g_notes_held = sh_modmanifest_load_notes_pending() > 0;
    g_notes_frame = g_frame;
    /* The same map launched again while its hold asks or downloads (the
     * player pressed Play once more, or the browser re-issued the request):
     * the hold goes on, and this launch is held with it rather than starting
     * the download over. Another map abandons the hold. */
    if (g_hold.state != HOLD_IDLE && text && g_hold.text && !pub_is_local(g_hold.hook) &&
        g_hold.length == strlen(text) && !memcmp(g_hold.text, text, g_hold.length)) {
        g_record.repeat = 1;
        g_record.kind = kind;
        g_record.parsed = parsed;
        g_record.map = out_map;
        g_record.sequence = sequence;
        g_record.frame = g_frame;
        g_record.valid = 1;
        pub_logf("MODS: '%s' was launched again while its mods %s; that launch is held with the first",
                 g_hold.id[0] ? g_hold.id : "the held map", g_hold.state == HOLD_FETCHING ? "download" : "are offered");
        return;
    }
    if (g_hold.state != HOLD_IDLE) pub_abandon("another map load started");
    memset(&rows, 0, sizeof rows);
    g_record.kind = kind;
    g_record.parsed = parsed;
    g_record.map = out_map;
    g_record.sequence = sequence;
    g_record.frame = g_frame;
    g_record.valid = 1;
    if (!parsed || !sh_modmanifest_current(&rows)) { sh_modmanifest_list_free(&rows); return; }
    g_record.rows = rows;
    g_record.missing = pub_count_missing(&g_record.rows);
    if (g_record.missing && text) {
        size_t length = strlen(text);
        g_record.text = malloc(length + 1);
        if (g_record.text) { memcpy(g_record.text, text, length + 1); g_record.length = length; }
        else pub_log("MODS: no memory to retain a map that needs mods; it cannot be resumed");
    }
}

int sh_modpublished_last_parse(sh_modpublished_parse_info *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g_record.valid) return -1;
    if (out) {
        out->kind = g_record.kind; out->parsed = g_record.parsed; out->map = g_record.map;
        out->rows = g_record.rows.count; out->missing = g_record.missing; out->sequence = g_record.sequence;
    }
    return g_record.kind;
}

int sh_modpublished_last_parse_published(void)
{
    return g_record.valid && (g_record.kind == SH_MODPUB_PARSE_PUBLISHED_OFFLINE ||
                              g_record.kind == SH_MODPUB_PARSE_PUBLISHED_ONLINE);
}

static int pub_is_completion(int hook)
{
    return hook == SH_MODPUB_ON_SNAP_PLAY || hook == SH_MODPUB_BROWSER_PLAY || hook == SH_MODPUB_CACHE_READY ||
           hook == SH_MODPUB_FEED_PLAY;
}

/* The launch boundary's hooks: they are asked through sh_modpub_launch_gate. */
static int pub_is_launch(int hook)
{
    return hook == SH_MODPUB_LOBBY_LAUNCH || hook == SH_MODPUB_OFFLINE_LAUNCH;
}

/* A rawmap's or prefab's name as its downloads folder is titled: the file
 * name without its folder and its .json/.zip extension. Static storage,
 * main thread. */
static const char *pub_leaf_stem(const char *name)
{
    static char stem[PUB_TITLE_CAP];
    const char *leaf = name, *c;
    size_t n;
    if (!name) return NULL;
    for (c = name; *c; c++) if (*c == '\\' || *c == '/') leaf = c + 1;
    strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
    for (;;) {
        n = strlen(stem);
        if (n > 5 && !_stricmp(stem + n - 5, ".json")) stem[n - 5] = 0;
        else if (n > 4 && !_stricmp(stem + n - 4, ".zip")) stem[n - 4] = 0;
        else break;
    }
    return stem;
}

/* Build the hold from rows (copied) and, when given, the record's text (moved). */
static int pub_hold_begin(int hook, void *owner, const void *metadata, void *extra, const char *id,
                          const char *revision, const char *title, const sh_modmanifest_list *rows, pub_record *record)
{
    pub_hold *h = &g_hold;
    if (h->state != HOLD_IDLE) pub_abandon("a newer map launch replaced it");
    memset(h, 0, sizeof(*h));
    if (!pub_list_copy(&h->rows, rows)) goto nomem;
    h->missing = calloc(rows->count ? rows->count : 1, sizeof(*h->missing));
    if (!h->missing) goto nomem;
    for (size_t i = 0; i < h->rows.count; i++) {
        if (pub_row_served_for(hook, &h->rows.rows[i])) continue;
        h->missing[h->missing_count++] = i;
        h->missing_bytes += h->rows.rows[i].size;
    }
    if (!h->missing_count) { pub_hold_free(); return 0; }
    h->other = calloc(h->missing_count, sizeof(*h->other));
    h->other_name = calloc(h->missing_count, sizeof(*h->other_name));
    if (!h->other || !h->other_name) goto nomem;
    for (size_t i = 0; i < h->missing_count && g_ops.other_build; i++)
        if (!g_ops.other_build(g_ops.context, &h->rows.rows[h->missing[i]], h->other[i], sizeof h->other[i],
                               h->other_name[i], sizeof h->other_name[i])) {
            h->other[i][0] = 0;
            h->other_name[i][0] = 0;
        }
    h->hook = hook; h->owner = owner; h->extra = extra;
    if (id) strcpy_s(h->id, sizeof h->id, id);
    if (revision) strcpy_s(h->revision, sizeof h->revision, revision);
    if (title) strncpy_s(h->title, sizeof h->title, title, _TRUNCATE);
    h->token = g_ops.hold_token ? g_ops.hold_token(g_ops.context, hook, owner, extra) : 0;
    if (record) {
        if (!record->text) goto nomem;
        h->text = record->text; h->length = record->length;
        record->text = NULL;
    }
    if (pub_is_completion(hook)) {
        h->metadata = g_ops.metadata_copy ? g_ops.metadata_copy(g_ops.context, metadata) : NULL;
        if (!h->metadata) {
            pub_log("MODS: the published map's metadata could not be retained");
            goto nomem;
        }
    }
    /* The gate (sh_modpub_gate_decide) runs first: the store is asked about
     * every missing bundle on the worker before the player is asked anything. */
    h->state = HOLD_PROBING;
    /* Each row the hold asks for, as the asset rule names it. */
    for (size_t i = 0; i < h->missing_count; i++) {
        char cause[PUB_CAUSE_CAP];
        (void)pub_row_state(&h->rows.rows[h->missing[i]], pub_thing(hook), cause, sizeof cause);
        if (cause[0]) { pub_print(cause); pub_logf("MODS: %s", cause); }
    }
    return 1;
nomem:
    pub_hold_free();
    return -1;
}

int sh_modpub_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                     const char *id, const char *revision)
{
    char item[PUB_ID_CAP] = "", rev[PUB_ID_CAP] = "", title[PUB_TITLE_CAP] = "";
    pub_record *record = &g_record;
    int begun;
    if (!g_ops_set || hook < 0 || hook >= SH_MODPUB_HOOKS || pub_is_local(hook) || pub_is_launch(hook)) return 1;
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!pub_on_main()) return 1;

    if (hook == SH_MODPUB_CACHE_LOBBY) {
        pub_seen *seen = pub_seen_find(id, revision);
        if (!seen) {
            pub_logf("MODS: lobby map '%s' revision '%s' is cached; this session never saw its manifest, "
                     "so it is not gated", id ? id : "", revision ? revision : "");
            return 1;
        }
        if (!seen->rows.count || !pub_count_missing(&seen->rows)) return 1;
        begun = pub_hold_begin(hook, owner, NULL, NULL, id, revision, seen->title, &seen->rows, NULL);
        if (begun <= 0) {
            if (begun < 0) {
                pub_log("MODS: the cached lobby map needs mods and could not be held; it is refused");
                pub_launch_refused(hook);
                pub_notice("This map needs mods that could not be prepared. The map was not loaded.");
            }
            return begun == 0;
        }
        pub_logf("MODS: lobby map '%s' (cached) needs %u missing mod(s); held for the player's answer",
                 id, (unsigned)g_hold.missing_count);
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        return 0;
    }

    /* The record must be this call's parse: valid, on the main thread, the
     * same map object and the same frame. Anything else is a launch this gate
     * did not see parse. The return-address classification is not required:
     * another detour on the parser can change it (see modpublished.h). */
    if (!record->valid || record->map != map || record->frame != g_frame) {
        if (record->valid && record->rows.count && record->map != map)
            pub_logf("MODS: %s received map %p, not the last parsed %p; not gated", pub_hook_name(hook),
                     map, record->map);
        else if (record->valid && record->rows.count)
            pub_logf("MODS: %s received map %p, last parsed in an earlier frame; not gated", pub_hook_name(hook), map);
        return 1;
    }
    /* This gate decides on the parse: the cause sentences the editor's
     * substitution held for it are not said (the gate says its own). */
    pub_notes_gated();
    if (pub_is_completion(hook) && record->kind == SH_MODPUB_PARSE_OTHER && record->rows.count)
        pub_logf("MODS: %s: the parse that filled its map was not recognized as a published read (another detour "
                 "on the parser changed its return address); the gate identifies it by its map object",
                 pub_hook_name(hook));
    if (record->repeat) {
        /* Held with the running hold of the same map (parse_seen above). */
        int held = g_hold.state != HOLD_IDLE;
        pub_record_clear(record);
        return held ? 0 : 1;
    }
    if (pub_is_completion(hook) && g_ops.metadata_ids)
        (void)g_ops.metadata_ids(g_ops.context, metadata, item, sizeof item, rev, sizeof rev);
    if (pub_is_completion(hook) && g_ops.metadata_title &&
        !g_ops.metadata_title(g_ops.context, metadata, title, sizeof title))
        title[0] = 0;
    /* The author's own published map follows their own folders, as a My
     * Maps load does (DESIGN-additive A.4, owner 2026-11-10): its rows are
     * rewritten to the builds this machine's own folders serve before
     * anything counts what is missing, so nothing is fetched for them. A
     * stranger's map keeps exactly what it pins. */
    if (pub_is_completion(hook) && item[0] && record->rows.count && g_ops.own_item && g_ops.follow_own &&
        g_ops.own_item(g_ops.context, item) && g_ops.follow_own(g_ops.context, item) > 0) {
        sh_modmanifest_list_free(&record->rows);
        (void)sh_modmanifest_current(&record->rows);
        record->missing = pub_count_missing(&record->rows);
    }
    if (pub_is_completion(hook)) pub_seen_note(item, rev, title, &record->rows);
    if (!record->rows.count || !record->missing) {
        pub_record_clear(record);   /* consumed */
        return 1;
    }
    /* A joiner's read of a map the lobby no longer hosts: CacheReady itself
     * ignores it (it sets nothing unless the host's item matches), so there is
     * nothing to ask about. */
    if (hook == SH_MODPUB_CACHE_READY &&
        (!g_ops.host_matches || !g_ops.host_matches(g_ops.context, item, rev))) {
        pub_record_clear(record);
        return 1;
    }
    begun = pub_hold_begin(hook, owner, metadata, extra, item, rev, title, &record->rows, record);
    pub_record_clear(record);
    if (begun < 0) {
        /* Never hand the engine a map whose mods are absent: it ends in the
         * palette miss that is fatal at START. Refuse instead. */
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
        pub_launch_refused(hook);
        pub_notice("This map needs mods that could not be prepared. The map was not loaded.");
        return 0;
    }
    if (!begun) return 1;
    pub_logf("MODS: %s of '%s' needs %u missing mod(s) (%llu bytes); held for the player's answer",
             pub_hook_name(hook), item[0] ? item : "local map", (unsigned)g_hold.missing_count,
             g_hold.missing_bytes);
    if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
    return 0;
}

int sh_modpub_launch_gate(int hook, void *manager, void *map, void *extra)
{
    pub_record *record = &g_record;
    size_t missing;
    int begun = -1;
    if (!g_ops_set || !pub_is_launch(hook)) return 1;
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!pub_on_main()) return 1;
    /* Only a parse of this frame that no gate decided: every gate consumes the
     * record it decides on, and a launch without a parse (the lobby's own
     * relaunch from the map cache) has none. */
    if (!record->valid || record->repeat || record->frame != g_frame || !record->rows.count) return 1;
    if (map && record->map != map) return 1;
    pub_notes_gated();
    missing = pub_count_missing(&record->rows);
    if (!missing) { pub_record_clear(record); return 1; }
    pub_logf("MODS: %s of a map no gate saw (parse %lu, map %p) names %u missing mod(s)", pub_hook_name(hook),
             record->sequence, record->map, (unsigned)missing);
    if (hook == SH_MODPUB_LOBBY_LAUNCH)
        begun = pub_hold_begin(hook, manager, NULL, extra, "", "", NULL, &record->rows, record);
    pub_record_clear(record);
    if (begun > 0) {
        pub_logf("MODS: %s needs %u missing mod(s) (%llu bytes); held for the player's answer", pub_hook_name(hook),
                 (unsigned)g_hold.missing_count, g_hold.missing_bytes);
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, manager);
        return 0;
    }
    if (begun == 0) return 1;
    /* Never hand the engine a published map whose mods are absent. An offline
     * launch has no map object left to resume with, so it is refused. */
    pub_logf("MODS: the %s was refused: its mods are not installed and it cannot be held", pub_hook_name(hook));
    if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, manager);
    pub_launch_refused(hook);
    pub_notice("This map needs mods that could not be prepared. The map was not loaded.");
    return 0;
}

/* ------------------------------------------------------------ the tick */

static void pub_poll_notice(void)
{
    int answer;
    if (g_notice_ticket) {
        answer = g_ops.poll ? g_ops.poll(g_ops.context, g_notice_ticket) : -1;
        if (answer == PUB_DIALOG_PENDING) return;
        if (g_ops.release) g_ops.release(g_ops.context, g_notice_ticket);
        g_notice_ticket = 0;
        return;
    }
    if (!g_notice[0] || g_hold.state == HOLD_RAISE || g_hold.state == HOLD_ASKING) return;
    if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
        if (++g_notice_waited > PUB_RAISE_TICKS) {
            pub_log("MODS: the notice could not be shown; the dialog surface stayed busy");
            g_notice[0] = 0;
        }
        return;
    }
    g_notice_ticket = g_ops.ask ? g_ops.ask(g_ops.context, PUB_GDM_ID, PUB_BUTTONS_ACCEPT, g_notice) : 0;
    if (!g_notice_ticket) pub_log("MODS: the notice dialog would not raise");
    g_notice[0] = 0;
}

static void pub_start_fetch(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->count = h->missing_count;
        job->hashes = calloc(job->count, sizeof(*job->hashes));
        job->ids = calloc(job->count, sizeof(*job->ids));
    }
    if (!job || !job->hashes || !job->ids) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s's mods could not be downloaded (out of memory). %s",
                    pub_thing(h->hook), pub_not_done(h->hook));
        pub_job_free(job);
        pub_drop(line, "no memory for the fetch job");
        return;
    }
    strncpy_s(job->title, sizeof job->title, h->title, _TRUNCATE);
    if (!pub_is_local(h->hook)) strncpy_s(job->map_id, sizeof job->map_id, h->id, _TRUNCATE);
    for (size_t i = 0; i < job->count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        strcpy_s(job->hashes[i], sizeof job->hashes[i], row->bundle);
        strcpy_s(job->ids[i], sizeof job->ids[i], row->id[0] ? row->id : row->bundle);
    }
    h->job = job;
    h->state = HOLD_FETCHING;
    h->progress_at = GetTickCount64();
    if (!g_ops.worker_run || !g_ops.worker_run(g_ops.context, pub_job_main, job)) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "The mod download could not start. %s", pub_not_done(h->hook));
        h->job = NULL;
        pub_job_free(job);
        pub_drop(line, "the worker refused the job");
        return;
    }
    pub_logf("MODS: downloading %u mod(s) for '%s'", (unsigned)job->count, h->id[0] ? h->id : "local map");
}

/* Drop the missing rows `drop` marks from the hold's per-row arrays (the
 * rows themselves stay: a resume activates and hands on all of them). */
static void pub_hold_keep(const unsigned char *drop)
{
    pub_hold *h = &g_hold;
    size_t i, n = 0;
    h->missing_bytes = 0;
    for (i = 0; i < h->missing_count; i++) {
        if (drop[i]) continue;
        if (n != i) {
            h->missing[n] = h->missing[i];
            if (h->other) memcpy(h->other[n], h->other[i], sizeof h->other[n]);
            if (h->other_name) memcpy(h->other_name[n], h->other_name[i], sizeof h->other_name[n]);
            if (h->probes) h->probes[n] = h->probes[i];
        }
        h->missing_bytes += h->rows.rows[h->missing[n]].size;
        n++;
    }
    h->missing_count = n;
}

/* The rows the gate decided SUBSTITUTE: remembered for the session, said
 * once each on the console, and no longer missing. Returns how many. */
static size_t pub_substitute(const int *verdicts)
{
    pub_hold *h = &g_hold;
    unsigned char *drop = calloc(h->missing_count ? h->missing_count : 1, 1);
    size_t i, n = 0;
    if (!drop) return 0;
    for (i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        const char *key = row->id[0] ? row->id : row->bundle;
        char line[600];
        if (verdicts[i] != SH_MODPUB_GATE_SUBSTITUTE || !h->other || !h->other[i][0]) continue;
        pub_subst_add(row->bundle, h->other[i]);
        if (pub_is_prefab(h->hook))
            _snprintf_s(line, sizeof line, _TRUNCATE, "%s '%s': %s: uses the installed mod; the placement adds it to the "
                        "map", pub_hook_name(h->hook), h->id, key);
        else
            _snprintf_s(line, sizeof line, _TRUNCATE, "%s: %s: uses the installed mod; the next save of the map records it",
                        pub_hook_name(h->hook), key);
        pub_print(line);
        pub_logf("MODS: %s (declared %.16s, not installed; serving %.16s)", line, row->bundle, h->other[i]);
        drop[i] = 1;
        n++;
    }
    if (n) pub_hold_keep(drop);
    h->substituted += n;
    free(drop);
    return n;
}

/* After a rescan: the missing rows that are served now (fetched) or by a
 * substitution leave the hold's list. */
static void pub_hold_settle(void)
{
    pub_hold *h = &g_hold;
    unsigned char *drop = calloc(h->missing_count ? h->missing_count : 1, 1);
    size_t i;
    if (!drop) return;
    for (i = 0; i < h->missing_count; i++) drop[i] = (unsigned char)pub_row_served_for(h->hook, &h->rows.rows[h->missing[i]]);
    pub_hold_keep(drop);
    free(drop);
}

static void pub_resume(size_t installed);

static void pub_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    char line[PUB_TEXT_CAP], cause[PUB_CAUSE_CAP];

    if (job->failed_index >= 0) {
        /* The gate decides a failed fetch too: the bundle left the store, the
         * store stopped answering, or its borrows name game bytes this
         * machine lacks. Anything else (a hash mismatch, a full disk) is the
         * download's own failure. */
        size_t at = (size_t)job->failed_index;
        const sh_modmanifest_row *row = at < h->missing_count ? &h->rows.rows[h->missing[at]] : NULL;
        _snprintf_s(cause, sizeof cause, _TRUNCATE, "the fetch of %s returned %d", job->hashes[job->failed_index],
                    job->failed_code);
        if (row && job->failed_code != PUB_NET_CANCELLED && job->failed_code != -2) {
            sh_modpub_gate_facts facts;
            int *verdicts = calloc(h->missing_count, sizeof(*verdicts));
            char detail[260] = "";
            memset(&facts, 0, sizeof facts);
            facts.store = pub_fetch_answer(job->failed_code);
            facts.game_lacks = job->failed_code == PUB_NET_GAME;
            facts.published = pub_published(h->hook, row);
            facts.other_build = h->other && h->other[at][0];
            facts.own = pub_is_own(h->hook);
            if (facts.game_lacks && g_ops.fetch_detail &&
                !g_ops.fetch_detail(g_ops.context, job->hashes[job->failed_index], detail, sizeof detail))
                detail[0] = 0;
            if (verdicts && (verdicts[at] = sh_modpub_gate_decide(&facts)) == SH_MODPUB_GATE_SUBSTITUTE) {
                /* The build the store listed could not be fetched: the
                 * player's own content goes on with the installed build. The
                 * rows fetched before it install now; any after it are
                 * fetched again on their own. */
                for (size_t i = 0; i < h->missing_count; i++) if (i != at) verdicts[i] = SH_MODPUB_GATE_OBTAIN;
                (void)pub_substitute(verdicts);
                free(verdicts);
                pub_job_free(job);
                h->job = NULL;
                if (g_ops.rescan) (void)g_ops.rescan(g_ops.context);
                pub_hold_settle();
                if (!h->missing_count) pub_resume(at);
                else pub_start_fetch();
                return;
            }
            if (verdicts && verdicts[at] != SH_MODPUB_GATE_OBTAIN) {
                if (h->probes) h->probes[at].code = job->failed_code;
                pub_deny(verdicts, h->probes, detail, cause);
                free(verdicts);
                return;
            }
            free(verdicts);
        }
        _snprintf_s(line, sizeof line, _TRUNCATE, "Could not download the mod %s. %s",
                    job->ids[job->failed_index], pub_not_done(h->hook));
        pub_drop(line, cause);
        return;
    }
    if (g_ops.rescan) (void)g_ops.rescan(g_ops.context);
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        if (pub_row_state(row, pub_thing(h->hook), cause, sizeof cause) == SH_MODPUB_ROW_SERVED) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE, "The mod %s was downloaded but could not be installed.%s%s %s",
                    row->id[0] ? row->id : row->bundle, cause[0] ? " " : "", cause, pub_not_done(h->hook));
        pub_drop(line, "a fetched row is not served");
        return;
    }
    pub_resume(h->missing_count);
}

/* The hold's mods are in place (fetched and installed, or served by the
 * installed build the gate substituted): the load goes on. `installed` is
 * the number of mods the fetch installed, for the log line. */
static void pub_resume(size_t installed)
{
    pub_hold *h = &g_hold;
    sh_modpub_resume r;
    int ok;
    if (pub_is_prefab(h->hook)) {
        /* The placement goes on: the ready callback brings the mods into the
         * open map (activation, the map-level rows) and the Prefabs tab places
         * the prefab again. */
        if (!g_prefab_ready) {
            pub_drop("The mods were installed, but the prefab could not be placed. Place it again.",
                     "no prefab callback is bound");
            return;
        }
        g_prefab_ready(h->id, &h->rows);
        pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed and %u served by the installed build",
                 pub_hook_name(h->hook), h->id, (unsigned)installed, (unsigned)h->substituted);
        pub_hold_free();
        return;
    }
    /* A rawmap hold has no screen of its own; the reopen asks the editor itself. */
    if (h->hook != SH_MODPUB_RAWMAP_LOAD && g_ops.owner_current &&
        !g_ops.owner_current(g_ops.context, h->hook, h->owner, h->extra, h->token)) {
        pub_abandon("the screen that asked for it is gone");
        return;
    }
    if ((h->hook == SH_MODPUB_CACHE_READY || h->hook == SH_MODPUB_CACHE_LOBBY) &&
        (!g_ops.host_matches || !g_ops.host_matches(g_ops.context, h->id, h->revision))) {
        pub_drop("The mods were installed, but the lobby's map changed meanwhile.",
                 "the lobby host's item differs from the held map");
        return;
    }
    /* A rawmap is activated by its own parse when it opens again; activating
     * now would change what the map that is open serves. */
    if (h->hook != SH_MODPUB_RAWMAP_LOAD) {
        /* Each row through the build that serves it now (the asset rule). */
        if (g_ops.activate) (void)g_ops.activate(g_ops.context, &h->rows);
    }

    memset(&r, 0, sizeof r);
    r.hook = h->hook; r.owner = h->owner; r.extra = h->extra; r.metadata = h->metadata;
    r.text = h->text; r.length = h->length; r.id = h->id; r.revision = h->revision;
    InterlockedExchange(&g_resuming, 1);
    ok = g_ops.resume ? g_ops.resume(g_ops.context, &r) : 0;
    InterlockedExchange(&g_resuming, 0);
    if (!ok) {
        pub_drop("The mods were installed, but the map could not be resumed. Try opening it again.",
                 "the resume did not reach the original");
        return;
    }
    pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed and %u served by the installed build",
             pub_hook_name(h->hook), h->id[0] ? h->id : "local map", (unsigned)installed, (unsigned)h->substituted);
    pub_hold_free();
}

/* ------------------------------------------------------------ rawmap loads */

static void pub_print(const char *line)
{
    if (g_ops.print) g_ops.print(g_ops.context, line);
}

/* "id version" for a row, the way the refusal names a mod. */
static void pub_row_label(char *out, size_t cap, const sh_modmanifest_row *row)
{
    if (row->id[0] && row->version[0]) _snprintf_s(out, cap, _TRUNCATE, "%s %s", row->id, row->version);
    else if (row->id[0]) _snprintf_s(out, cap, _TRUNCATE, "%s", row->id);
    else _snprintf_s(out, cap, _TRUNCATE, "bundle %.16s", row->bundle);
}

/* mode 0: the store lacks them; 1: the store could not be reached; 2 (a
 * prefab with no gates bound): the store was not asked. */
static void pub_compose_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode, int prefab)
{
    static const char gone[] = ": ";
    static const char lacks[] = " and the mod store does not hold: ";
    const char *head = prefab ? "This prefab was not placed. It needs mods you do not have"
                              : "This map was not loaded. It needs mods you do not have";
    const char *mid = mode == 0 ? lacks : gone;
    const char *tail = mode == 1
        ? (prefab ? ". The mod store could not be reached. Try again, or ask the prefab's author for its .zip export."
                  : ". The mod store could not be reached. Try again, or ask the map's author for its .zip export.")
        : (prefab ? ". Ask the prefab's author for its .zip export." : ". Ask the map's author for its .zip export.");
    char list[PUB_TEXT_CAP];
    size_t room, fixed = strlen(head) + strlen(mid) + strlen(tail);
    if (!out || !cap) return;
    if (cap > PUB_TEXT_CAP) cap = PUB_TEXT_CAP;
    room = cap - 1 > fixed + 1 ? cap - 1 - fixed : 1;
    if (room > sizeof list) room = sizeof list;
    pub_names_line(list, room, mods, count);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s%s", head, mid, list, tail);
}

void sh_modpublished_compose_refusal(char *out, size_t cap, const char *const *mods, size_t count, int unreachable)
{
    pub_compose_refusal(out, cap, mods, count, unreachable ? 1 : 0, 0);
}

void sh_modpublished_compose_prefab_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode)
{
    pub_compose_refusal(out, cap, mods, count, mode < 0 || mode > 2 ? 0 : mode, 1);
}

void sh_modpublished_prefab_refusal_line(char *out, size_t cap, const char *name, const char *const *mods, size_t count,
                                         int mode)
{
    size_t i;
    if (!out || !cap) return;
    _snprintf_s(out, cap, _TRUNCATE, "Prefab %s was not placed: it needs ", name && name[0] ? name : "prefab");
    for (i = 0; i < count; i++) {
        if (i) strncat_s(out, cap, ", ", _TRUNCATE);
        strncat_s(out, cap, mods[i], _TRUNCATE);
    }
    strncat_s(out, cap, mode == 1
        ? ", which you do not have, and the mod store could not be reached. Try again, or ask the prefab's author "
          "for its .zip export, which carries the mods."
        : mode == 2 ? ", which you do not have. Ask the prefab's author for its .zip export, which carries the mods."
        : ", which you do not have and the mod store does not hold. Ask the prefab's author for its .zip export, "
          "which carries the mods.", _TRUNCATE);
}

size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows)
{
    unsigned rejected = 0;
    size_t missing;
    if (!rows) return 0;
    memset(rows, 0, sizeof(*rows));
    if (!g_ops_set || !text || !length) return 0;
    sh_modmanifest_read(text, length, rows, &rejected);
    if (!rows->count) return 0;
    /* A row the gate substituted this session is served (a rawmap's reopen). */
    missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served_for(SH_MODPUB_RAWMAP_LOAD, &rows->rows[i])) missing++;
    return missing;
}

#define PUB_LABEL_CAP (SH_MODMANIFEST_ID_CAP + SH_MODMANIFEST_VERSION_CAP + 16)

/* One row's refusal notice: the asset rule's sentence, what did not happen,
 * and where the mod can come from. Kept only when it fits the dialog body. */
static void pub_single_notice(char *notice, size_t cap, const char *cause, int hook, int unreachable)
{
    char text[PUB_TEXT_CAP * 2];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s %s %s", cause, pub_not_done(hook),
                unreachable ? "The mod store could not be reached; try again, or ask its author for the .zip export."
                            : "Ask its author for the .zip export.");
    if (strlen(text) < cap && strlen(text) < PUB_TEXT_CAP) strcpy_s(notice, cap, text);
}

/* The refusal: the notice, and one console line naming every mod. */
static void pub_rawmap_refuse(int hook, const char *source, const sh_modmanifest_row *const *rows, size_t count,
                              int unreachable, const char *why)
{
    char notice[PUB_TEXT_CAP], line[1024], piece[PUB_LABEL_CAP + 4];
    const char **labels = calloc(count ? count : 1, sizeof(*labels));
    char (*storage)[PUB_LABEL_CAP] = calloc(count ? count : 1, sizeof(*storage));
    const char *bare = pub_is_prefab(hook) ? "This prefab was not placed. It needs mods you do not have."
                                           : "This map was not loaded. It needs mods you do not have.";
    char single[PUB_CAUSE_CAP] = "";
    /* The asset rule's sentence for each row first; one row's sentence is
     * also the notice. */
    for (size_t i = 0; i < count; i++) {
        char cause[PUB_CAUSE_CAP];
        (void)pub_row_state(rows[i], pub_thing(hook), cause, sizeof cause);
        if (!cause[0]) continue;
        pub_print(cause);
        if (count == 1) strcpy_s(single, sizeof single, cause);
    }
    if (!labels || !storage) {
        free((void *)labels); free(storage);
        if (g_hold.state != HOLD_IDLE) pub_drop(bare, why);
        else pub_notice(bare);
        return;
    }
    if (pub_is_prefab(hook)) {
        for (size_t i = 0; i < count; i++) {
            pub_row_label(storage[i], sizeof storage[i], rows[i]);
            labels[i] = storage[i];
        }
        sh_modpublished_prefab_refusal_line(line, sizeof line, source, labels, count, unreachable ? 1 : 0);
        sh_modpublished_compose_prefab_refusal(notice, sizeof notice, labels, count, unreachable ? 1 : 0);
        if (single[0]) pub_single_notice(notice, sizeof notice, single, hook, unreachable);
        free((void *)labels); free(storage);
        pub_print(line);
        pub_logf("MODS: %s", line);
        if (g_hold.state != HOLD_IDLE) pub_drop(notice, why);
        else pub_notice(notice);
        return;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "Rawmap %s was not loaded: it needs ", source && source[0] ? source : "map");
    for (size_t i = 0; i < count; i++) {
        pub_row_label(storage[i], sizeof storage[i], rows[i]);
        labels[i] = storage[i];
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s%s", i ? ", " : "", storage[i]);
        strncat_s(line, sizeof line, piece, _TRUNCATE);
    }
    strncat_s(line, sizeof line, unreachable
        ? ", which you do not have, and the mod store could not be reached. Try again, or ask the map's author "
          "for its .zip export, which carries the mods."
        : ", which you do not have and the mod store does not hold. Ask the map's author for its .zip export, "
          "which carries the mods.", _TRUNCATE);
    sh_modpublished_compose_refusal(notice, sizeof notice, labels, count, unreachable);
    if (single[0]) pub_single_notice(notice, sizeof notice, single, hook, unreachable);
    free((void *)labels); free(storage);
    pub_print(line);
    pub_logf("MODS: %s", line);
    if (g_hold.state != HOLD_IDLE) pub_drop(notice, why);
    else pub_notice(notice);
}

int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source)
{
    char name[PUB_ID_CAP] = "";
    int begun;
    if (!rows || !rows->count) return 0;
    if (source) strncpy_s(name, sizeof name, source, _TRUNCATE);
    if (!g_ops_set || !pub_on_main()) {
        const sh_modmanifest_row **missing = calloc(rows->count, sizeof(*missing));
        size_t n = 0;
        if (missing) {
            for (size_t i = 0; i < rows->count; i++)
                if (!pub_row_served(&rows->rows[i])) missing[n++] = &rows->rows[i];
            if (n) pub_rawmap_refuse(SH_MODPUB_RAWMAP_LOAD, name, missing, n, 0, "the rawmap gate is unavailable here");
            free((void *)missing);
        }
        return -1;
    }
    begun = pub_hold_begin(SH_MODPUB_RAWMAP_LOAD, NULL, NULL, NULL, name, "", pub_leaf_stem(name), rows, NULL);
    if (begun == 0) return 0;
    if (begun < 0) {
        pub_notice("This map needs mods that could not be prepared. The map was not loaded.");
        return -1;
    }
    g_hold.state = HOLD_PROBING;
    pub_logf("MODS: rawmap '%s' needs %u missing mod(s) (%llu bytes); asking the mod store for them", name,
             (unsigned)g_hold.missing_count, g_hold.missing_bytes);
    {
        char line[PUB_TEXT_CAP + PUB_ID_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "Rawmap %s needs %u mod%s you do not have; checking the mod store.",
                    name[0] ? name : "map", (unsigned)g_hold.missing_count, g_hold.missing_count == 1 ? "" : "s");
        pub_print(line);
    }
    return 1;
}

/* ------------------------------------------------------------ prefab placements */

void sh_modpub_set_prefab_ready(sh_modpub_prefab_ready_fn fn) { g_prefab_ready = fn; }

int sh_modpub_prefab_holding(void)
{
    return g_hold.state != HOLD_IDLE && pub_is_prefab(g_hold.hook);
}

int sh_modpub_prefab_hold(const sh_modmanifest_list *rows, const char *name)
{
    char nm[PUB_ID_CAP] = "", line[PUB_TEXT_CAP + PUB_ID_CAP];
    int begun;
    if (!rows || !rows->count) return 0;
    if (!g_ops_set) return -2;
    if (!pub_on_main()) return -1;
    if (name) strncpy_s(nm, sizeof nm, name, _TRUNCATE);
    begun = pub_hold_begin(SH_MODPUB_PREFAB_PLACE, NULL, NULL, NULL, nm, "", pub_leaf_stem(nm), rows, NULL);
    if (begun <= 0) return begun;
    g_hold.state = HOLD_PROBING;
    pub_logf("MODS: prefab '%s' needs %u missing mod(s) (%llu bytes); asking the mod store for them", nm,
             (unsigned)g_hold.missing_count, g_hold.missing_bytes);
    _snprintf_s(line, sizeof line, _TRUNCATE, "Prefab %s needs %u mod%s you do not have; checking the mod store.",
                nm[0] ? nm : "prefab", (unsigned)g_hold.missing_count, g_hold.missing_count == 1 ? "" : "s");
    pub_print(line);
    return 1;
}

static void pub_start_probe(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->count = h->missing_count;
        job->probe = 1;
        job->hashes = calloc(job->count, sizeof(*job->hashes));
        job->ids = calloc(job->count, sizeof(*job->ids));
        job->probes = calloc(job->count, sizeof(*job->probes));
    }
    if (!job || !job->hashes || !job->ids || !job->probes) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s needs mods that could not be prepared. %s", pub_thing(h->hook),
                    pub_not_done(h->hook));
        pub_job_free(job);
        pub_drop(line, "no memory for the store check");
        return;
    }
    for (size_t i = 0; i < job->count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        strcpy_s(job->hashes[i], sizeof job->hashes[i], row->bundle);
        strcpy_s(job->ids[i], sizeof job->ids[i], row->id[0] ? row->id : row->bundle);
    }
    if (!g_ops.worker_run || !g_ops.worker_run(g_ops.context, pub_job_main, job)) {
        pub_job_free(job);
        /* Another network job (an upload, a download) is running: try again
         * on later ticks, for as long as a question may wait for the surface. */
        if (++h->waited > PUB_RAISE_TICKS) {
            char line[PUB_TEXT_CAP];
            _snprintf_s(line, sizeof line, _TRUNCATE, "The mod store is busy, so this %s's mods could not be checked. %s "
                        "Try again in a moment.", pub_thing(h->hook), pub_not_done(h->hook));
            pub_drop(line, "the worker stayed busy");
        }
        return;
    }
    h->job = job;
    h->waited = 0;
}

/* The denial: the first denied bundle's sentence in the dialog (with a note
 * when others are denied too), one console line per denied bundle. `verdicts`
 * and `probes` are per missing row; `detail` (optional) is a source for a
 * verdict 4 the probe did not see. */
static void pub_deny(const int *verdicts, const sh_modpub_probe *probes, const char *detail, const char *why)
{
    pub_hold *h = &g_hold;
    char notice[PUB_TEXT_CAP] = "", line[1024];
    size_t denied = 0;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        char title[300];
        const char *source;
        if (verdicts[i] == SH_MODPUB_GATE_OBTAIN || verdicts[i] == SH_MODPUB_GATE_SUBSTITUTE) continue;
        pub_title(title, sizeof title, row, probes && probes[i].name[0] ? probes[i].name : h->other_name[i]);
        source = probes && probes[i].source[0] ? probes[i].source : detail;
        sh_modpub_gate_wording(verdicts[i], pub_thing(h->hook), title, source, probes ? probes[i].code : 0, line,
                               sizeof line);
        pub_print(line);
        pub_logf("MODS: denied (cause %d, bundle %.16s): %s", verdicts[i], row->bundle, line);
        if (!denied++) strncpy_s(notice, sizeof notice, line, _TRUNCATE);
    }
    if (denied > 1) {
        char more[96];
        _snprintf_s(more, sizeof more, _TRUNCATE, " %u more mod%s cannot be obtained; see the console.",
                    (unsigned)(denied - 1), denied == 2 ? "" : "s");
        if (strlen(notice) + strlen(more) < sizeof notice) strcat_s(notice, sizeof notice, more);
    }
    pub_drop(notice, why);
}

static void pub_probe_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    int *verdicts = calloc(h->missing_count ? h->missing_count : 1, sizeof(*verdicts));
    size_t denied = 0;
    if (!verdicts) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s needs mods that could not be prepared. %s", pub_thing(h->hook),
                    pub_not_done(h->hook));
        pub_drop(line, "no memory");
        return;
    }
    /* The probe's answers stay with the hold: the question and a later
     * denial name the bundles by what the store said. */
    free(h->probes);
    h->probes = job->probes;
    job->probes = NULL;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        sh_modpub_gate_facts facts;
        if (job->failed_index >= 0 && (size_t)job->failed_index <= i) h->probes[i].found = SH_MODPUB_STORE_UNREACHABLE;
        memset(&facts, 0, sizeof facts);
        facts.store = h->probes[i].found;
        facts.game_lacks = h->probes[i].game_lacks;
        facts.published = pub_published(h->hook, row);
        facts.other_build = h->other && h->other[i][0];
        facts.own = pub_is_own(h->hook);
        verdicts[i] = sh_modpub_gate_decide(&facts);
        if (verdicts[i] != SH_MODPUB_GATE_OBTAIN && verdicts[i] != SH_MODPUB_GATE_SUBSTITUTE) denied++;
    }
    if (denied) {
        pub_deny(verdicts, h->probes, NULL, "the gate denied the launch");
    } else if (pub_substitute(verdicts) && !h->missing_count) {
        /* Every missing row is served by its key's installed build: nothing
         * to ask or fetch. */
        pub_job_free(job);
        h->job = NULL;
        pub_resume(0);
    } else {
        pub_logf("MODS: the mod store holds all %u missing mod(s) of the %s of '%s'", (unsigned)h->missing_count,
                 pub_hook_name(h->hook), h->id[0] ? h->id : "local map");
        pub_job_free(job);
        h->job = NULL;
        h->waited = 0;
        h->state = HOLD_RAISE;
    }
    free(verdicts);
}

void sh_modpublished_poll(void)
{
    pub_hold *h = &g_hold;
    g_frame++;
    if (!g_ops_set) return;
    /* A parse no gate took in its frame was the editor's: its held cause
     * sentences are said now, once. */
    if (g_notes_held && g_notes_frame != g_frame && pub_on_main()) {
        char refusal[700];
        int refused = sh_modmanifest_load_refusal(refusal, sizeof refusal);
        g_notes_held = 0;
        (void)sh_modmanifest_load_notes_flush(1);
        /* The editor cannot open the map: the player is told why, in words. */
        if (refused && strlen(refusal) < PUB_TEXT_CAP) pub_notice(refusal);
    }
    if (h->state == HOLD_IDLE && !g_notice[0] && !g_notice_ticket && !g_orphans) return;
    if (!pub_on_main()) return;
    pub_orphans_retire();

    switch (h->state) {
    case HOLD_PROBING:
        if (!h->job) { pub_start_probe(); break; }
        if (!InterlockedCompareExchange(&h->job->done, 0, 0)) break;
        pub_probe_finish();
        break;
    case HOLD_RAISE: {
        char text[PUB_TEXT_CAP];
        const char **names;
        if (g_notice_ticket) break;           /* let a notice finish first */
        if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
            if (++h->waited > PUB_RAISE_TICKS)
                pub_drop(NULL, "the dialog surface never became idle, so the player was never asked");
            break;
        }
        (void)names;
        pub_hold_question(h, text, sizeof text);
        h->ticket = g_ops.ask ? g_ops.ask(g_ops.context, PUB_GDM_ID, PUB_BUTTONS_YES_NO, text) : 0;
        if (!h->ticket) { pub_drop(NULL, "the dialog would not raise; nothing is downloaded without an answer"); break; }
        h->state = HOLD_ASKING;
        break;
    }
    case HOLD_ASKING: {
        int answer = g_ops.poll ? g_ops.poll(g_ops.context, h->ticket) : -1;
        const char **names;
        char list[160] = "mods that are not installed", line[PUB_TEXT_CAP];
        if (answer == PUB_DIALOG_PENDING) break;
        if (g_ops.release) g_ops.release(g_ops.context, h->ticket);
        h->ticket = 0;
        if (answer == PUB_DIALOG_ACCEPTED) { pub_start_fetch(); break; }
        if ((names = pub_missing_names(h)) != NULL) {
            pub_names_line(list, sizeof list, names, h->missing_count);
            free((void *)names);
        }
        _snprintf_s(line, sizeof line, _TRUNCATE, pub_is_prefab(h->hook) ? "The prefab was not placed: it needs %s."
                                                                        : "The map was not loaded: it needs %s.", list);
        if (h->missing_count == 1) {
            char cause[PUB_CAUSE_CAP], text[PUB_TEXT_CAP * 2];
            (void)pub_row_state(&h->rows.rows[h->missing[0]], pub_thing(h->hook), cause, sizeof cause);
            _snprintf_s(text, sizeof text, _TRUNCATE, "%s %s", cause, pub_not_done(h->hook));
            if (cause[0] && strlen(text) < sizeof line) strcpy_s(line, sizeof line, text);
        }
        pub_drop(line, "the player declined");
        break;
    }
    case HOLD_FETCHING: {
        pub_job *job = h->job;
        if (!InterlockedCompareExchange(&job->done, 0, 0)) {
            ULONGLONG now = GetTickCount64();
            if (now - h->progress_at >= PUB_PROGRESS_MS) {
                LONG current = InterlockedCompareExchange(&job->current, 0, 0);
                long long done = InterlockedCompareExchange64(&job->progress_done, 0, 0);
                long long total = InterlockedCompareExchange64(&job->progress_total, 0, 0);
                h->progress_at = now;
                pub_logf("MODS: downloading %s (%ld of %u): %.1f of %.1f MB", job->ids[current],
                         current + 1, (unsigned)job->count, done / 1048576.0, total / 1048576.0);
            }
            break;
        }
        pub_finish();
        break;
    }
    default:
        break;
    }
    pub_poll_notice();
}

/* ------------------------------------------------------------ tests */
#ifdef SH_MODPUBLISHED_TESTING
void sh_modpub_test_set_ops(const sh_modpub_ops *ops)
{
    if (ops) { g_ops = *ops; g_ops_set = 1; sh_modmanifest_set_substitute(sh_modpub_editor_substitute); }
    else { memset(&g_ops, 0, sizeof g_ops); g_ops_set = 0; }
}
void sh_modpub_test_set_read_sites(const void *offline_return, const void *online_return)
{
    g_read_offline_return = offline_return;
    g_read_online_return = online_return;
}
void sh_modpub_test_reset(void)
{
    pub_hold_free();
    while (g_orphans) {
        pub_job *job = g_orphans;
        while (!InterlockedCompareExchange(&job->done, 0, 0)) Sleep(1);
        g_orphans = job->next;
        pub_job_free(job);
    }
    pub_record_clear(&g_record);
    memset(g_pub_subst, 0, sizeof g_pub_subst);
    g_pub_subst_next = 0;
    g_prefab_ready = NULL;
    for (size_t i = 0; i < PUB_SEEN_MAX; i++) sh_modmanifest_list_free(&g_seen[i].rows);
    memset(g_seen, 0, sizeof g_seen);
    g_notice[0] = 0; g_notice_ticket = 0; g_notice_waited = 0;
    InterlockedExchange(&g_resuming, 0);
}
int sh_modpub_test_state(void) { return g_hold.state; }
const char *sh_modpub_test_notice(void) { return g_notice; }
#endif

/* ================================================================== engine
 * Everything below binds the seam to DOOM build 13954591 (both renderers; the
 * offsets are equal on both, the addresses are not). Every engine read and
 * call is SEH-guarded; a failed bind installs nothing. */
#ifndef SH_MODPUBLISHED_PURE
#include "signatures.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "engine_dialog.h"
#include "modserve.h"
#include "modstore.h"
#include "modpublish.h" /* sh_modpublish_item_linked: an item this machine published */
#include "modui.h"      /* sh_modui_scope_launch: a hub launch's listing */
#include "process_heap_scope.h"
#include "rawmap.h"     /* sh_rawmap_reopen_after_mods: a rawmap hold's resume */
#include "commands.h"   /* sh_printf: the rawmap refusal's console line */

#define ENG_METADATA_SIZE  0x588   /* idPublishedMapModel; the selection record allocates 0x590 with its prefix */
#define ENG_MAP_SIZE       0x800   /* idSnapMap: 0x770 on every native caller's stack */
#define ENG_STRING_SIZE    0x40    /* idStr is 0x30; native frames reserve 0x30 */
#define ENG_DIALOG_PARAMS  0xB0    /* OnSnapPlay's ClearDialog block, rsp+0x20 .. rsp+0xD0 */
#define ENG_MGR_PROGRESS   0xA38   /* manager: the progress widget */
#define ENG_LOBBY_ITEM     0x550   /* lobby launch parameters: item id idStr */
#define ENG_LOBBY_REVISION 0x600   /* lobby launch parameters: revision idStr */
#define ENG_SHELL_MANAGER  0x18    /* shell: the menu manager */
#define ENG_MODEL_KEY      0x10    /* localSavedModel: the field LaunchLocal's caller reads the map by */

typedef void (*eng_completion_fn)(void *, void *, const void *, void *);
typedef unsigned char (*eng_cache_lobby_fn)(void *, const void *, const void *);
typedef void (*eng_local_fn)(void *, void *, void *);
typedef void *(*eng_read_fn)(void *, const char *, const char *, void *, void *);
typedef void *(*eng_copy_fn)(void *, const void *);
typedef void (*eng_destroy_fn)(void *);
typedef void *(*eng_ctor_fn)(void *);
typedef int (*eng_parse_fn)(const char *, void *);
typedef void *(*eng_get_fn)(void);
typedef void *(*eng_new_fn)(size_t);
typedef void *(*eng_idstr_ctor_fn)(void *, const char *);
typedef void (*eng_object_fn)(void *);
typedef void (*eng_show_fn)(void *, void *);
typedef void (*eng_clear_fn)(void *, void *);
typedef unsigned long long (*eng_lobby_launch_fn)(void *, unsigned char);
typedef void (*eng_offline_launch_fn)(void *, const void *, void *);

#define ENG_FEED_PLAY_ACTION 3     /* the feed completion's action int: 1 details, 2 author, 3 play */

enum { ENG_ON_SNAP, ENG_BROWSER, ENG_READY, ENG_LOBBY, ENG_LOCAL, ENG_FEED, ENG_LOBBY_LAUNCH, ENG_OFFLINE_LAUNCH,
       ENG_HOOKS };
static const char *const ENG_HOOK_NAMES[ENG_HOOKS] = {
    "PublishedOnSnapPlayDone", "PublishedBrowserPlayDone", "PublishedMapCacheReady",
    "CacheMapForLobby", "LaunchLocalSnapMapForOfflinePlay", "PublishedFeedPlayDone",
    "PublishedMapLobbyLaunch", "PublishedMapOfflineLaunch" };

static struct {
    const uint8_t *site[ENG_HOOKS];
    void *original[ENG_HOOKS];
    sh_process_heap_api heap;
    eng_copy_fn metadata_copy;
    eng_destroy_fn metadata_destroy;
    eng_ctor_fn map_ctor;
    eng_destroy_fn map_dtor;
    eng_parse_fn parse;                  /* the DeserializeFromJson entry: our detour runs */
    DWORD *main_thread;
    void **shell_slot, **session_slot;
    size_t slot_lobby, slot_parameters;
    const uint8_t *snap_params;          /* the 16-byte head of OnSnapPlay's dialog block */
    const char *snap_source;
    eng_clear_fn clear_dialog;
    eng_object_fn browser_flag, hide_progress;
    eng_get_fn system;
    eng_idstr_ctor_fn idstr_ctor;
    eng_object_fn idstr_dtor;
    eng_show_fn show_wait;
    const char *wait_text;
    eng_new_fn op_new;
    void *vft_ok, *vft_fail, *ready_fn, *fail_fn;
    eng_read_fn read;
    volatile LONG ready;
} g_eng;

static const uint8_t *eng_site(const sig_result *results, size_t count, const uint8_t *base, const char *name)
{
    const uint8_t *site = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (site || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        site = (const uint8_t *)results[i].addr;
    }
    return site;
}

static void *eng_call(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8) return NULL;
    memcpy(&displacement, p + 1, 4);
    return (void *)(p + 5 + displacement);
}

/* The target of a RIP-relative operand whose disp32 follows `opcode`. */
static void *eng_rip(const uint8_t *p, const char *opcode, size_t opcode_length)
{
    int32_t displacement;
    if (memcmp(p, opcode, opcode_length)) return NULL;
    memcpy(&displacement, p + opcode_length, 4);
    return (void *)(p + opcode_length + 4 + displacement);
}

/* The slot index of `call qword ptr [reg+disp]` (optional REX.B, FF /2). */
static size_t eng_vcall_slot(const uint8_t *p)
{
    int32_t displacement;
    uint8_t modrm;
    if (p[0] == 0x41) p++;
    if (p[0] != 0xff) return 0;
    modrm = p[1];
    if (((modrm >> 3) & 7) != 2 || (modrm & 7) == 4) return 0;
    if ((modrm >> 6) == 1) displacement = (int8_t)p[2];
    else if ((modrm >> 6) == 2) memcpy(&displacement, p + 2, 4);
    else return 0;
    if (displacement <= 0 || displacement >= 0x800 || displacement % 8) return 0;
    return (size_t)displacement / sizeof(void *);
}

static void *eng_vslot(void *object, size_t offset)
{
    return (*(void ***)object)[offset / sizeof(void *)];
}

/* A native idStr's text: length at +8, data at +0x10. */
static int eng_string(const void *s, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!s || !out || !cap) return 0;
    __try {
        int length = *(const int *)((const uint8_t *)s + 8);
        const char *data = *(const char *const *)((const uint8_t *)s + 0x10);
        if (length < 0 || (size_t)length >= cap || (length && !data)) return 0;
        if (length) memcpy(out, data, (size_t)length);
        out[length] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
}

/* The shell's menu manager, the one every published-map screen belongs to. */
static void *eng_shell_manager(void)
{
    __try {
        void *shell = g_eng.shell_slot ? *g_eng.shell_slot : NULL;
        return shell ? *(void **)((uint8_t *)shell + ENG_SHELL_MANAGER) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* The manager's current screen id plus one (0: unknown). */
static unsigned long long eng_screen_token(void)
{
    void *manager = eng_shell_manager();
    if (!manager) return 0;
    __try { return (unsigned long long)(unsigned)*(const int *)((const uint8_t *)manager + 0x08) + 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *eng_manager_of(int hook, void *owner)
{
    if (!owner) return NULL;
    /* The feed widget reaches its manager through its screen; the shell's
     * manager is that manager. */
    if (hook == SH_MODPUB_FEED_PLAY) return eng_shell_manager();
    if (hook != SH_MODPUB_BROWSER_PLAY) return owner;
    __try { return *(void **)((uint8_t *)owner + 0x10); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* ------------------------------------------------------------ ops */

static void eng_log(void *c, const char *line) { (void)c; backend_log(line); }

static int eng_on_main(void *c)
{
    (void)c;
    __try { return g_eng.main_thread && *g_eng.main_thread && *g_eng.main_thread == GetCurrentThreadId(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int eng_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{ (void)c; return sh_modserve_row_served_for(row, subject, cause, cap); }
static int eng_rescan(void *c) { (void)c; return sh_modserve_index_rescan(); }
static int eng_activate(void *c, const sh_modmanifest_list *rows) { (void)c; return sh_modmanifest_activate(rows); }
static int eng_can_ask(void *c) { (void)c; return sh_engine_dialog_can_ask(); }
static int eng_ask(void *c, unsigned gdm, unsigned set, const char *text) { (void)c; return sh_engine_dialog_ask(gdm, set, text); }
static int eng_poll(void *c, int ticket) { (void)c; return sh_engine_dialog_poll(ticket); }
static void eng_release(void *c, int ticket) { (void)c; sh_engine_dialog_release(ticket); }
/* One job at a time: modnet_job_start refuses a second while one runs, which
 * is the contract the hold expects (a newer hold cancels the older job first). */
static struct { void (*job)(void *); void *ctx; } g_eng_shim;
static int eng_shim_run(void *context, const modnet_io *io)
{
    (void)context; (void)io;
    if (g_eng_shim.job) g_eng_shim.job(g_eng_shim.ctx);
    return 0;
}
static int eng_worker_run(void *c, void (*job)(void *), void *ctx)
{
    (void)c;
    if (modnet_job_busy(NULL, 0)) return 0;
    g_eng_shim.job = job; g_eng_shim.ctx = ctx;
    return modnet_job_start("published-map fetch", eng_shim_run, NULL, NULL, NULL, NULL) == 1;
}
/* The source of the borrow that refused the last fetch (MODNET_E_GAME), for
 * the gate's cause 4. Written on the worker, read on the main tick after the
 * job is done (the job's completion is the barrier). */
static char g_eng_fetch_hash[SH_MODMANIFEST_HASH_CAP], g_eng_fetch_source[260];

C_ASSERT(PUB_NET_CONFIG == MODNET_E_CONFIG && PUB_NET_NET == MODNET_E_NET && PUB_NET_TIMEOUT == MODNET_E_TIMEOUT);
C_ASSERT(PUB_NET_CANCELLED == MODNET_E_CANCELLED && PUB_NET_HTTP == MODNET_E_HTTP && PUB_NET_PROTOCOL == MODNET_E_PROTOCOL);
C_ASSERT(PUB_NET_NOT_FOUND == MODNET_E_NOT_FOUND && PUB_NET_RATE == MODNET_E_RATE && PUB_NET_BLOCKED == MODNET_E_BLOCKED);
C_ASSERT(PUB_NET_UNAVAILABLE == MODNET_E_UNAVAILABLE && PUB_NET_GAME == MODNET_E_GAME);

static int eng_fetch(void *c, const char *hash, sh_modpub_progress_fn progress, volatile int *cancel)
{
    modnet_io io;
    modsync_stats st;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&st, 0, sizeof st);
    io.cancel = (volatile long *)cancel;
    io.progress = (modnet_progress_fn)progress;
    g_eng_fetch_hash[0] = 0;
    g_eng_fetch_source[0] = 0;
    if (modsync_fetch_bundle(hash, &io, &st)) {
        if (st.editor_only)
            pub_logf("MODS: %.16s changes only the editor; the map plays without it, so it was not installed", hash);
        return 0;
    }
    pub_logf("fetch %.16s failed: %s", hash, st.cause[0] ? st.cause : modnet_code_text(st.code));
    if (st.game_lacks) {
        strncpy_s(g_eng_fetch_hash, sizeof g_eng_fetch_hash, hash, _TRUNCATE);
        strncpy_s(g_eng_fetch_source, sizeof g_eng_fetch_source, st.game_source, _TRUNCATE);
    }
    return st.code ? st.code : -1;
}

static int eng_fetch_for(void *c, const char *hash, const char *title, const char *map_id, sh_modpub_progress_fn progress,
                         volatile int *cancel)
{
    modnet_io io;
    modsync_stats st;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&st, 0, sizeof st);
    io.cancel = (volatile long *)cancel;
    io.progress = (modnet_progress_fn)progress;
    g_eng_fetch_hash[0] = 0;
    g_eng_fetch_source[0] = 0;
    if (modsync_fetch_bundle_for(hash, title && title[0] ? title : NULL, map_id && map_id[0] ? map_id : NULL, &io, &st)) {
        if (st.editor_only)
            pub_logf("MODS: %.16s changes only the editor; the map plays without it, so it was not installed", hash);
        else if (st.key[0])
            pub_logf("MODS: %.16s is served by mods\\%s%s", hash, st.key, st.already_installed ? " (already in the store)" : "");
        return 0;
    }
    pub_logf("fetch %.16s failed: %s", hash, st.cause[0] ? st.cause : modnet_code_text(st.code));
    if (st.game_lacks) {
        strncpy_s(g_eng_fetch_hash, sizeof g_eng_fetch_hash, hash, _TRUNCATE);
        strncpy_s(g_eng_fetch_source, sizeof g_eng_fetch_source, st.game_source, _TRUNCATE);
    }
    return st.code ? st.code : -1;
}

/* The published metadata's title (an idStr at +0x398; the item id is at +0
 * and the revision at +0x30, as eng_metadata_ids reads them). */
#define ENG_METADATA_TITLE 0x398
static int eng_metadata_title(void *c, const void *metadata, char *out, size_t cap)
{
    (void)c;
    return metadata && eng_string((const uint8_t *)metadata + ENG_METADATA_TITLE, out, cap) && out[0];
}

static int eng_own_item(void *c, const char *item)
{
    (void)c;
    return item && item[0] && (modsync_claim_lookup(item, NULL, NULL) || sh_modpublish_item_linked(item));
}

/* A row of an own published map: the build its key's own folder serves. */
static int eng_follow_own_row(const char *bundle_hash, const char *id, sh_modmanifest_row *out)
{
    sh_modserve_bundle_ref ref;
    if (!out || !sh_modserve_row_follow_own(id, bundle_hash, &ref)) return 0;
    if (strlen(ref.id) >= sizeof out->id || strlen(ref.version) >= sizeof out->version) return 0;
    strcpy_s(out->id, sizeof out->id, ref.id);
    strcpy_s(out->version, sizeof out->version, ref.version);
    strcpy_s(out->bundle, sizeof out->bundle, ref.hash);
    out->size = ref.size;
    return 1;
}

static int eng_follow_own(void *c, const char *item)
{
    (void)c;
    return sh_modmanifest_current_follow(eng_follow_own_row, item);
}

static int eng_fetch_detail(void *c, const char *hash, char *out, size_t cap)
{
    (void)c;
    if (!hash || _stricmp(hash, g_eng_fetch_hash) || !g_eng_fetch_source[0]) return 0;
    strncpy_s(out, cap, g_eng_fetch_source, _TRUNCATE);
    return 1;
}

/* A deep copy through the copy constructor the selection record uses, inside
 * process heap 0 so its strings and lists outlive the read that made it. */
static void *eng_metadata_copy(void *c, const void *metadata)
{
    sh_process_heap_scope scope;
    uint8_t *copy = NULL;
    int constructed = 0;
    (void)c;
    if (!metadata || !sh_process_heap_enter(&g_eng.heap, &scope)) return NULL;
    __try {
        copy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ENG_METADATA_SIZE);
        if (copy) { g_eng.metadata_copy(copy, metadata); constructed = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { constructed = 0; }
    if (!sh_process_heap_leave(&scope)) {
        /* Never run another native destructor on an unknown heap stack. */
        backend_log("MODS: the heap scope could not be restored after a metadata copy");
        return NULL;
    }
    if (!constructed) { if (copy) HeapFree(GetProcessHeap(), 0, copy); return NULL; }
    return copy;
}

static void eng_metadata_free(void *c, void *copy)
{
    sh_process_heap_scope scope;
    (void)c;
    if (!copy) return;
    if (!sh_process_heap_enter(&g_eng.heap, &scope)) {
        backend_log("MODS: a retained published-map metadata copy was leaked (heap scope unavailable)");
        return;
    }
    __try { g_eng.metadata_destroy(copy); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: a retained metadata destructor faulted"); }
    HeapFree(GetProcessHeap(), 0, copy);
    (void)sh_process_heap_leave(&scope);
}

static int eng_metadata_ids(void *c, const void *metadata, char *id, size_t id_cap, char *rev, size_t rev_cap)
{
    (void)c;
    return metadata && eng_string(metadata, id, id_cap) &&
           eng_string((const uint8_t *)metadata + 0x30, rev, rev_cap);
}

static unsigned long long eng_hold_token(void *c, int hook, void *owner, const void *extra)
{
    (void)c; (void)owner;
    /* A feed widget or a launch boundary resumes into the screen it was held
     * on: the widget lives in that screen, the lobby launch reads the
     * history record the completion pushed from it. */
    if (hook == SH_MODPUB_FEED_PLAY || hook == SH_MODPUB_LOBBY_LAUNCH) return eng_screen_token();
    if (hook != SH_MODPUB_LOCAL_OFFLINE || !extra) return 0;
    __try { return *(const unsigned long long *)((const uint8_t *)extra + ENG_MODEL_KEY); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int eng_owner_current(void *c, int hook, void *owner, const void *extra, unsigned long long token)
{
    void *manager = eng_manager_of(hook, owner);
    if (!eng_on_main(c) || !manager) return 0;
    __try {
        void *shell = g_eng.shell_slot ? *g_eng.shell_slot : NULL;
        if (!shell || *(void **)((uint8_t *)shell + ENG_SHELL_MANAGER) != manager) return 0;
        if (hook == SH_MODPUB_LOCAL_OFFLINE &&
            (!extra || *(const unsigned long long *)((const uint8_t *)extra + ENG_MODEL_KEY) != token)) return 0;
        if ((hook == SH_MODPUB_FEED_PLAY || hook == SH_MODPUB_LOBBY_LAUNCH) && token &&
            eng_screen_token() != token) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* The session lobby's launch parameters, as PublishedMapCacheReady reads
 * them: session -> lobby (+0x348) -> parameters (+0x2B8). */
static int eng_host_matches(void *c, const char *id, const char *revision)
{
    char item[PUB_ID_CAP], rev[PUB_ID_CAP];
    (void)c;
    if (!id || !id[0] || !revision) return 0;
    __try {
        void *session = g_eng.session_slot ? *g_eng.session_slot : NULL, *lobby;
        const uint8_t *parameters;
        if (!session) return 0;
        lobby = ((void *(*)(void *))(*(void ***)session)[g_eng.slot_lobby])(session);
        if (!lobby) return 0;
        parameters = ((const uint8_t *(*)(void *))(*(void ***)lobby)[g_eng.slot_parameters])(lobby);
        if (!parameters || !eng_string(parameters + ENG_LOBBY_ITEM, item, sizeof item) ||
            !eng_string(parameters + ENG_LOBBY_REVISION, rev, sizeof rev)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return !strcmp(item, id) && !strcmp(rev, revision);
}

/* OnSnapPlay's own first steps: clear the read's wait dialog (GDM 0x63)
 * through the shell's ClearDialog slot with the block it builds, then hide
 * the progress widget. Holding without this leaves the wait dialog queued,
 * and the question could never be raised over it. */
static void eng_snap_clear_wait(void)
{
    uint8_t params[ENG_DIALOG_PARAMS];
    void *shell;
    memset(params, 0, sizeof params);
    memcpy(params, g_eng.snap_params, 16);
    *(const char **)(params + 0x98) = g_eng.snap_source;
    *(int *)(params + 0xA0) = 0x1919;
    params[0xA4] = 0;
    *(int *)(params + 0xA8) = -1;
    shell = g_eng.shell_slot ? *g_eng.shell_slot : NULL;
    if (!shell || eng_vslot(shell, 0x220) != (void *)g_eng.clear_dialog) {
        backend_log("MODS: the shell's ClearDialog slot is not the bound wrapper; the wait dialog stays");
        return;
    }
    g_eng.clear_dialog(shell, params);
}

static void eng_hold_ui(void *c, int hook, void *owner)
{
    void *manager = eng_manager_of(hook, owner);
    (void)c;
    if (!manager) return;
    __try {
        switch (hook) {
        case SH_MODPUB_ON_SNAP_PLAY:
            eng_snap_clear_wait();
            g_eng.hide_progress(manager);
            break;
        case SH_MODPUB_CACHE_READY:
            g_eng.hide_progress(manager);
            break;
        case SH_MODPUB_BROWSER_PLAY: {
            /* The browser completion's own first steps: clear the manager's
             * pending flag (+0x348), the browser's state (+0x20), set its
             * widget to 0xF and hide the progress widget. */
            void *widget = *(void **)((uint8_t *)owner + 0x30);
            g_eng.browser_flag(manager);
            *(int *)((uint8_t *)owner + 0x20) = 0;
            if (widget) ((void (*)(void *, int))eng_vslot(widget, 8))(widget, 0xF);
            g_eng.hide_progress(manager);
            break;
        }
        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: the hold's screen update faulted"); }
}

static void eng_drop_ui(void *c, int hook, void *owner)
{
    void *manager = eng_manager_of(hook, owner);
    (void)c;
    if (!manager || hook == SH_MODPUB_LOCAL_OFFLINE || hook == SH_MODPUB_CACHE_LOBBY) return;
    if (!eng_owner_current(c, hook, owner, NULL, 0)) return;
    __try { g_eng.hide_progress(manager); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* The joiner's cold branch of CacheMapForLobby, replayed: the wait text, the
 * two bound callbacks (CacheReady and the read-error handler), and the read. */
static int eng_cold_read(void *manager, const char *id, const char *revision)
{
    uint8_t wait[ENG_STRING_SIZE];
    void **ok = NULL, **fail = NULL;
    g_eng.idstr_ctor(wait, g_eng.wait_text);
    g_eng.show_wait(manager, wait);
    g_eng.idstr_dtor(wait);
    ok = g_eng.op_new(0x18);
    if (ok) { ok[0] = g_eng.vft_ok; ok[1] = manager; ok[2] = g_eng.ready_fn; }
    fail = g_eng.op_new(0x18);
    if (fail) { fail[0] = g_eng.vft_fail; fail[1] = manager; fail[2] = g_eng.fail_fn; }
    g_eng.read(NULL, id, revision, ok, fail);
    return 1;
}

/* A hub launch (a published map, a My Maps play) runs in the serving layer's
 * Play mode from the launch on, as the editor's Play does, so a mod the map
 * does not declare serves nothing in it (modserve.h, sh_modserve_scope_mode),
 * and it declares exactly the launched map's rows, which its parse made the
 * current manifest (modui.h, sh_modui_scope_launch): never the last editor
 * map's. The tick (modui.c) clears that listing once the Play is over, and
 * sets the editor mode again when the editor next shows a map. Main thread. */
static void eng_scope_play(const char *why)
{
    __try {
        (void)sh_modui_scope_launch(why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the launch's serving mode change faulted; the launch continues");
    }
}

static int eng_resume(void *c, const sh_modpub_resume *r)
{
    sh_process_heap_scope scope;
    uint8_t *map = NULL;
    int constructed = 0, ran = 0;
    (void)c;
    if (r && r->hook == SH_MODPUB_RAWMAP_LOAD) {
        /* Nothing was launched, so there is nothing to call: the rawmap opens
         * again, now or at the next map open. */
        char note[PUB_TEXT_CAP] = "";
        ran = sh_rawmap_reopen_after_mods(note, sizeof note);
        if (note[0]) pub_notice(note);
        return ran;
    }
    if (r && r->hook == SH_MODPUB_LOBBY_LAUNCH && r->text) {
        /* The launch boundary held a map whose parse already ran: its map is
         * in the cache. Bring the manifest's scope and activation up as that
         * parse would have with the mods installed, then launch. */
        size_t n = 0;
        char *stripped = NULL;
        __try { stripped = sh_modmanifest_prepare_load(r->text, r->length, &n); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: the held launch's manifest step faulted"); }
        if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    }
    if (r && (r->hook == SH_MODPUB_LOBBY_LAUNCH || r->hook == SH_MODPUB_LOCAL_OFFLINE))
        eng_scope_play(r->hook == SH_MODPUB_LOBBY_LAUNCH ? "published launch" : "My Maps play");
    if (!r || !sh_process_heap_enter(&g_eng.heap, &scope)) return 0;
    __try {
        if (r->hook == SH_MODPUB_CACHE_LOBBY) {
            ran = eng_cold_read(r->owner, r->id, r->revision);
        } else if (r->hook == SH_MODPUB_LOBBY_LAUNCH) {
            ((eng_lobby_launch_fn)g_eng.original[ENG_LOBBY_LAUNCH])(r->owner, (unsigned char)(uintptr_t)r->extra);
            ran = 1;
        } else if (r->text && (map = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ENG_MAP_SIZE)) != NULL) {
            g_eng.map_ctor(map); constructed = 1;
            if (g_eng.parse(r->text, map)) {
                ran = 1;
                if (r->hook == SH_MODPUB_LOCAL_OFFLINE)
                    ((eng_local_fn)g_eng.original[ENG_LOCAL])(r->owner, r->extra, map);
                else if (r->hook == SH_MODPUB_FEED_PLAY) {
                    /* The feed completion reads only the action int at
                     * request+8 and frees it with the engine's sized delete,
                     * so it gets a new one from the engine's operator new. */
                    uint8_t request[0x10];
                    int *action = g_eng.op_new(sizeof(int));
                    memset(request, 0, sizeof request);
                    if (action) {
                        *action = ENG_FEED_PLAY_ACTION;
                        *(int **)(request + 8) = action;
                        ((eng_completion_fn)g_eng.original[ENG_FEED])(r->owner, request, r->metadata, map);
                    } else ran = 0;
                } else
                    ((eng_completion_fn)g_eng.original[r->hook == SH_MODPUB_ON_SNAP_PLAY ? ENG_ON_SNAP :
                        r->hook == SH_MODPUB_BROWSER_PLAY ? ENG_BROWSER : ENG_READY])(
                        r->owner, NULL, r->metadata, map);
            } else backend_log("MODS: the retained map text did not parse on resume");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: the resume raised exception %08lx", GetExceptionCode());
        backend_log(line);
    }
    if (constructed) {
        __try { g_eng.map_dtor(map); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: the resume's map destructor faulted"); }
    }
    if (map) HeapFree(GetProcessHeap(), 0, map);
    if (!sh_process_heap_leave(&scope)) backend_log("MODS: the heap scope could not be restored after a resume");
    return ran;
}

/* GET /mods/bundle/<hash>: the manifest is small (at most 1 MB) and is what a
 * fetch reads first, so a 404 here is the fetch's own first refusal. 451 (on
 * the takedown list) is as absent as 404 for the player. */
static int eng_store_has(void *c, const char *hash, volatile int *cancel)
{
    modnet_io io;
    modnet_result r;
    char *text = NULL;
    size_t len = 0;
    int code;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&r, 0, sizeof r);
    io.cancel = (volatile long *)cancel;
    code = modnet_get_bundle(hash, &text, &len, &io, &r);
    free(text);
    if (code == MODNET_OK) return 1;
    pub_logf("MODS: store check of %.16s: %s", hash, modnet_code_text(code));
    if (code == MODNET_E_NOT_FOUND || code == MODNET_E_BLOCKED) return 0;
    return -1;
}

static void eng_print(void *c, const char *line) { (void)c; if (line) sh_printf("%s\n", line); }

/* The gate's probe: the bundle manifest from the store and its borrows
 * checked against this player's game (modsync.h). */
static int eng_store_probe(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out)
{
    modnet_io io;
    modsync_probe p;
    (void)c;
    memset(&io, 0, sizeof io);
    io.cancel = (volatile long *)cancel;
    (void)modsync_probe_bundle(hash, &io, &p);
    memset(out, 0, sizeof *out);
    out->found = p.found > 0 ? SH_MODPUB_STORE_HAS : p.found == 0 ? SH_MODPUB_STORE_ABSENT : SH_MODPUB_STORE_UNREACHABLE;
    out->code = p.found < 0 ? p.code : 0;
    out->game_lacks = p.game_lacks;
    strncpy_s(out->source, sizeof out->source, p.source, _TRUNCATE);
    strncpy_s(out->name, sizeof out->name, p.name, _TRUNCATE);
    if (p.found <= 0) pub_logf("MODS: store check of %.16s: %s", hash, modnet_code_text(p.code));
    else if (p.game_lacks) pub_logf("MODS: store check of %.16s: held, but %s", hash, p.cause);
    return out->found;
}

static int eng_claimed(void *c, const char *hash) { (void)c; return modsync_claim_names(hash); }

/* A display name of the row's key from the installed folders (any build). */
static int eng_mod_name(void *c, const sh_modmanifest_row *row, char *out, size_t cap)
{
    sh_ms_list l;
    int found = 0;
    (void)c;
    if (!row || !row->id[0] || !out || !cap) return 0;
    modstore_lock();
    if (sh_ms_enumerate(&l)) {
        for (size_t i = 0; i < l.count && !found; i++) {
            const sh_ms_entry *e = &l.items[i];
            if (!e->loaded || _stricmp(e->bundle.id, row->id) || !e->bundle.name || !e->bundle.name[0]) continue;
            strncpy_s(out, cap, e->bundle.name, _TRUNCATE);
            found = 1;
        }
        sh_ms_list_free(&l);
    }
    modstore_unlock();
    return found;
}

static int eng_other_build(void *c, const sh_modmanifest_row *row, char *hash, size_t hash_cap, char *name,
                           size_t name_cap)
{
    sh_modserve_bundle_ref ref;
    if (!sh_modserve_row_other_build(row, &ref)) return 0;
    strncpy_s(hash, hash_cap, ref.hash, _TRUNCATE);
    if (!eng_mod_name(c, row, name, name_cap)) name[0] = 0;
    return 1;
}

/* The gate refused a launch: the map scope declares nothing of it. */
static void eng_launch_refused(void *c, int hook)
{
    (void)c;
    __try {
        (void)sh_modui_scope_launch_refused(pub_hook_name(hook));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the refused launch's scope clear faulted");
    }
}

static const sh_modpub_ops ENG_OPS = {
    NULL, eng_on_main, eng_row_state, eng_rescan, eng_activate, eng_can_ask, eng_ask, eng_poll,
    eng_release, eng_worker_run, eng_fetch, eng_metadata_copy, eng_metadata_free, eng_metadata_ids,
    eng_hold_token, eng_owner_current, eng_host_matches, eng_hold_ui, eng_drop_ui, eng_resume, eng_log,
    eng_store_has, eng_print, eng_store_probe, eng_claimed, eng_other_build, eng_mod_name,
    eng_fetch_detail, eng_launch_refused, eng_fetch_for, eng_metadata_title, eng_own_item, eng_follow_own
};

/* ------------------------------------------------------------ detours */

static int eng_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                      const char *id, const char *revision)
{
    if (!InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    __try { return sh_modpub_decide(hook, owner, metadata, map, extra, id, revision); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the published-map gate faulted; the launch continues natively");
        return 1;
    }
}

static void eng_on_snap_detour(void *manager, void *request, const void *metadata, void *map)
{
    if (eng_decide(SH_MODPUB_ON_SNAP_PLAY, manager, metadata, map, NULL, NULL, NULL))
        ((eng_completion_fn)g_eng.original[ENG_ON_SNAP])(manager, request, metadata, map);
}

static void eng_browser_detour(void *browser, void *request, const void *metadata, void *map)
{
    if (eng_decide(SH_MODPUB_BROWSER_PLAY, browser, metadata, map, NULL, NULL, NULL))
        ((eng_completion_fn)g_eng.original[ENG_BROWSER])(browser, request, metadata, map);
}

static void eng_ready_detour(void *manager, void *request, const void *metadata, void *map)
{
    if (eng_decide(SH_MODPUB_CACHE_READY, manager, metadata, map, NULL, NULL, NULL))
        ((eng_completion_fn)g_eng.original[ENG_READY])(manager, request, metadata, map);
}

/* 1 when the map cache already holds (id, revision): the branch that returns
 * without a read. -1 when it cannot be asked. */
static int eng_cache_warm(const void *id, const void *revision)
{
    __try {
        void *system = g_eng.system(), *cache;
        if (!system) return -1;
        cache = ((void *(*)(void *))eng_vslot(system, 0x38))(system);
        if (!cache) return -1;
        return ((unsigned char (*)(void *, const void *, const void *))eng_vslot(cache, 0x1B0))(cache, id, revision) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static unsigned char eng_lobby_detour(void *manager, const void *id, const void *revision)
{
    char item[PUB_ID_CAP], rev[PUB_ID_CAP];
    eng_cache_lobby_fn original = (eng_cache_lobby_fn)g_eng.original[ENG_LOBBY];
    if (!InterlockedCompareExchange(&g_eng.ready, 0, 0) || InterlockedCompareExchange(&g_resuming, 0, 0) ||
        eng_cache_warm(id, revision) != 1 ||
        !eng_string(id, item, sizeof item) || !eng_string(revision, rev, sizeof rev))
        return original(manager, id, revision);
    if (eng_decide(SH_MODPUB_CACHE_LOBBY, manager, NULL, NULL, NULL, item, rev))
        return original(manager, id, revision);
    return 0;
}

static void eng_local_detour(void *manager, void *model, void *map)
{
    if (eng_decide(SH_MODPUB_LOCAL_OFFLINE, manager, NULL, map, model, NULL, NULL)) {
        eng_scope_play("My Maps play");
        ((eng_local_fn)g_eng.original[ENG_LOCAL])(manager, model, map);
    }
}

/* The SnapMap feed's read completion. Only the play action launches; details
 * and author views pass. A held or refused play still runs the completion,
 * with its action set to one that launches nothing, so the feed clears its
 * wait state and the action's int is freed as the engine expects. */
static void eng_feed_detour(void *widget, void *request, const void *metadata, void *map)
{
    int *action = NULL, value = -1;
    __try {
        action = request ? *(int **)((uint8_t *)request + 8) : NULL;
        value = action ? *action : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { action = NULL; value = -1; }
    if (value == ENG_FEED_PLAY_ACTION && !eng_decide(SH_MODPUB_FEED_PLAY, widget, metadata, map, NULL, NULL, NULL)) {
        __try { *action = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    }
    ((eng_completion_fn)g_eng.original[ENG_FEED])(widget, request, metadata, map);
}

static int eng_launch_gate(int hook, void *manager, void *map, void *extra)
{
    if (!InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    __try { return sh_modpub_launch_gate(hook, manager, map, extra); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the published-map launch boundary faulted; the launch continues natively");
        return 1;
    }
}

/* The launch boundary. Its callers ignore the result; a held or refused
 * launch returns 0, as the function does when the lobby cannot be entered. */
static unsigned long long eng_lobby_launch_detour(void *manager, unsigned char flag)
{
    if (eng_launch_gate(SH_MODPUB_LOBBY_LAUNCH, manager, NULL, (void *)(uintptr_t)flag)) {
        eng_scope_play("published launch");
        return ((eng_lobby_launch_fn)g_eng.original[ENG_LOBBY_LAUNCH])(manager, flag);
    }
    return 0;
}

static void eng_offline_launch_detour(void *manager, const void *metadata, void *map)
{
    if (eng_launch_gate(SH_MODPUB_OFFLINE_LAUNCH, manager, map, NULL)) {
        eng_scope_play("published launch");
        ((eng_offline_launch_fn)g_eng.original[ENG_OFFLINE_LAUNCH])(manager, metadata, map);
    }
}

/* ------------------------------------------------------------ install */

static int eng_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *read, *limit, *selection, *destroy, *parse, *map_ctor, *map_dtor, *str_ctor, *str_dtor, *clear;
    const uint8_t *snap, *browser, *ready, *lobby;
    uintptr_t shell, thread;
    for (size_t i = 0; i < ENG_HOOKS; i++)
        if (!(g_eng.site[i] = eng_site(results, count, base, ENG_HOOK_NAMES[i]))) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: published-map gate site %s did not resolve cleanly",
                        ENG_HOOK_NAMES[i]);
            backend_log(line);
            return 0;
        }
    read = eng_site(results, count, base, "PublishedMapRead");
    limit = eng_site(results, count, base, "SnapMapReadSizeLimit");
    selection = eng_site(results, count, base, "PublishedMapSelectionConstruct");
    destroy = eng_site(results, count, base, "PublishedMapMetadataDestroy");
    parse = eng_site(results, count, base, "DeserializeFromJson");
    map_ctor = eng_site(results, count, base, "SnapMapCtor");
    map_dtor = eng_site(results, count, base, "SnapMapDtor");
    str_ctor = eng_site(results, count, base, "IdStrCtor");
    str_dtor = eng_site(results, count, base, "IdStrDtor");
    clear = eng_site(results, count, base, "ClearDialogWrapper");
    if (!read || !limit || !selection || !destroy || !parse || !map_ctor || !map_dtor || !str_ctor ||
        !str_dtor || !clear || !sh_process_heap_bind(&g_eng.heap, results, count, base)) {
        backend_log("MODS: a published-map helper did not resolve cleanly");
        return 0;
    }
    snap = g_eng.site[ENG_ON_SNAP]; browser = g_eng.site[ENG_BROWSER];
    ready = g_eng.site[ENG_READY]; lobby = g_eng.site[ENG_LOBBY];
    shell = glb_resolve(base, "shell_ptr_slot", NULL);
    thread = glb_resolve(base, "main_thread_id", NULL);
    if (!shell || !thread) { backend_log("MODS: the shell or main-thread global did not resolve"); return 0; }
    __try {
        /* The two read sites call the parser; their return addresses classify. */
        if (eng_call(read + 0x125) != (void *)parse || eng_call(limit + 0x58) != (void *)parse) return 0;
        /* The selection record allocates 0x590 and copy-constructs the metadata. */
        if (memcmp(selection + 0x45, "\xb9\x90\x05\x00\x00", 5) ||
            !(g_eng.metadata_copy = (eng_copy_fn)eng_call(selection + 0x69))) return 0;
        /* OnSnapPlay: the cookie load relocated at +0xB, the dialog block's
         * constant head, the shell slot, the source label, ClearDialog at
         * vtable +0x220 and the progress widget. */
        if (memcmp(snap + 0x0B, "\x48\x8b\x05", 3) ||
            !(g_eng.snap_params = eng_rip(snap + 0x1D, "\x66\x0f\x6f\x05", 4)) ||
            eng_rip(snap + 0x4E, "\x48\x8b\x0d", 3) != (void *)shell ||
            !(g_eng.snap_source = eng_rip(snap + 0x55, "\x48\x8d\x05", 3)) ||
            memcmp(snap + 0x69, "\xc7\x84\x24\xc0\x00\x00\x00\x19\x19\x00\x00", 11) ||
            memcmp(snap + 0x8A, "\xff\x90\x20\x02\x00\x00", 6) ||
            memcmp(snap + 0x90, "\x48\x8b\x8b\x38\x0a\x00\x00", 7) ||
            memcmp(snap + 0xA1, "\xff\x50\x30", 3)) return 0;
        /* Browser play: the manager flag, the browser state and widget, and the
         * manager's progress hide (also used for the other manager hooks). */
        if (!(g_eng.browser_flag = (eng_object_fn)eng_call(browser + 0x21)) ||
            memcmp(browser + 0x26, "\x48\x8b\x4b\x30\xc7\x43\x20\x00\x00\x00\x00\x48\x85\xc9\x74\x0b"
                                   "\x48\x8b\x01\xba\x0f\x00\x00\x00\xff\x50\x08\x48\x8b\x4b\x10", 31) ||
            !(g_eng.hide_progress = (eng_object_fn)eng_call(browser + 0x45))) return 0;
        /* The launch boundary is what browser play and feed play call: the
         * lobby launch at browser+0x9C and feed+0xE3, the offline launch at
         * browser+0xAD. */
        if (eng_call(browser + 0x9C) != (void *)g_eng.site[ENG_LOBBY_LAUNCH] ||
            eng_call(browser + 0xAD) != (void *)g_eng.site[ENG_OFFLINE_LAUNCH] ||
            eng_call(g_eng.site[ENG_FEED] + 0xE3) != (void *)g_eng.site[ENG_LOBBY_LAUNCH]) return 0;
        /* CacheReady: progress, then session -> lobby -> parameters. */
        if (memcmp(ready + 0x38, "\x48\x8b\x89\x38\x0a\x00\x00", 7) || memcmp(ready + 0x49, "\xff\x50\x30", 3) ||
            !(g_eng.session_slot = eng_rip(ready + 0x4C, "\x48\x8b\x0d", 3)) ||
            memcmp(ready + 0x53, "\x48\x8b\x01", 3) || !(g_eng.slot_lobby = eng_vcall_slot(ready + 0x56)) ||
            memcmp(ready + 0x5C, "\x48\x8b\x10\x48\x8b\xc8", 6) ||
            !(g_eng.slot_parameters = eng_vcall_slot(ready + 0x62))) return 0;
        /* CacheMapForLobby: the warm query, then the cold branch it replays. */
        if (!(g_eng.system = (eng_get_fn)eng_call(lobby + 0x34)) ||
            memcmp(lobby + 0x39, "\x48\x8b\x10\x48\x8b\xc8\xff\x52\x38", 9) ||
            memcmp(lobby + 0x42, "\x4c\x8b\x08\x4c\x8b\xc5\x48\x8b\xd6\x48\x8b\xc8\x41\xff\x91\xb0\x01\x00\x00", 19) ||
            !(g_eng.wait_text = eng_rip(lobby + 0x60, "\x48\x8d\x15", 3)) ||
            eng_call(lobby + 0x6C) != (void *)str_ctor ||
            !(g_eng.show_wait = (eng_show_fn)eng_call(lobby + 0x7A)) ||
            eng_call(lobby + 0x85) != (void *)str_dtor ||
            memcmp(lobby + 0x8A, "\xb9\x18\x00\x00\x00", 5) ||
            !(g_eng.op_new = (eng_new_fn)eng_call(lobby + 0x8F)) ||
            eng_call(lobby + 0xC3) != (void *)g_eng.op_new ||
            !(g_eng.vft_ok = eng_rip(lobby + 0xA1, "\x48\x8d\x05", 3)) ||
            eng_rip(lobby + 0xAF, "\x48\x8d\x05", 3) != (void *)ready ||
            !(g_eng.vft_fail = eng_rip(lobby + 0xD2, "\x48\x8d\x0d", 3)) ||
            !(g_eng.fail_fn = eng_rip(lobby + 0xE0, "\x48\x8d\x0d", 3)) ||
            eng_call(lobby + 0xFD) != (void *)read) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_eng.ready_fn = (void *)ready;
    g_eng.read = (eng_read_fn)read;
    g_eng.metadata_destroy = (eng_destroy_fn)destroy;
    g_eng.map_ctor = (eng_ctor_fn)map_ctor;
    g_eng.map_dtor = (eng_destroy_fn)map_dtor;
    g_eng.parse = (eng_parse_fn)parse;
    g_eng.idstr_ctor = (eng_idstr_ctor_fn)str_ctor;
    g_eng.idstr_dtor = (eng_object_fn)str_dtor;
    g_eng.clear_dialog = (eng_clear_fn)clear;
    g_eng.shell_slot = (void **)shell;
    g_eng.main_thread = (DWORD *)thread;
    g_read_offline_return = read + 0x12A;
    g_read_online_return = limit + 0x5D;
    return 1;
}

int sh_modpublished_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    void *detours[ENG_HOOKS] = { (void *)eng_on_snap_detour, (void *)eng_browser_detour, (void *)eng_ready_detour,
                                 (void *)eng_lobby_detour, (void *)eng_local_detour, (void *)eng_feed_detour,
                                 (void *)eng_lobby_launch_detour, (void *)eng_offline_launch_detour };
    if (InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    if (!results || !module_base || !eng_bind(results, count, module_base)) {
        g_read_offline_return = g_read_online_return = NULL;
        backend_log("MODS: published-map gates are not installed on this build; published maps load natively");
        return 0;
    }
    g_ops = ENG_OPS; g_ops_set = 1;
    sh_modmanifest_set_substitute(sh_modpub_editor_substitute);
    /* OnSnapPlay: 11 bytes of pushes and sub, then the 7-byte security-cookie
     * load, rewritten. CacheReady: the stack-probe call at +7. The others are
     * whole, position-independent prologues. */
    g_eng.original[ENG_ON_SNAP] = hook_prepare_rip_load((void *)g_eng.site[ENG_ON_SNAP], detours[ENG_ON_SNAP], 18, 11);
    g_eng.original[ENG_BROWSER] = hook_prepare((void *)g_eng.site[ENG_BROWSER], detours[ENG_BROWSER], 20);
    g_eng.original[ENG_READY] = hook_prepare_relative_call((void *)g_eng.site[ENG_READY], detours[ENG_READY], 15, 7);
    g_eng.original[ENG_LOBBY] = hook_prepare((void *)g_eng.site[ENG_LOBBY], detours[ENG_LOBBY], 20);
    g_eng.original[ENG_LOCAL] = hook_prepare((void *)g_eng.site[ENG_LOCAL], detours[ENG_LOCAL], 22);
    /* Feed: seven pushes and sub rsp. Lobby launch: five pushes and the frame
     * LEA (its stack-probe call follows at +23). Offline launch: three
     * pushes, sub rsp and the frame-cookie store. All position-independent. */
    g_eng.original[ENG_FEED] = hook_prepare((void *)g_eng.site[ENG_FEED], detours[ENG_FEED], 14);
    g_eng.original[ENG_LOBBY_LAUNCH] = hook_prepare((void *)g_eng.site[ENG_LOBBY_LAUNCH], detours[ENG_LOBBY_LAUNCH], 18);
    g_eng.original[ENG_OFFLINE_LAUNCH] = hook_prepare((void *)g_eng.site[ENG_OFFLINE_LAUNCH],
                                                      detours[ENG_OFFLINE_LAUNCH], 17);
    for (size_t i = 0; i < ENG_HOOKS; i++) if (!g_eng.original[i]) goto failed;
    for (size_t i = 0; i < ENG_HOOKS; i++) if (hook_commit(g_eng.original[i]) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_eng.ready, 1);
    backend_log("MODS: published-map gates installed (published play, browser play, SnapMap feed play, lobby join, "
                "cached lobby join, My Maps play) with the lobby and offline launch boundary; maps whose mods are "
                "installed load natively");
    return 1;
failed:
    for (size_t i = ENG_HOOKS; i > 0; i--)
        if (g_eng.original[i - 1] && hook_unpatch(g_eng.original[i - 1])) g_eng.original[i - 1] = NULL;
    g_read_offline_return = g_read_online_return = NULL;
    g_ops_set = 0;
    backend_log("MODS: published-map gate hooks could not be installed; published maps load natively");
    return 0;
}
#endif
