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
 *                  store about every missing build (HEAD of its archive);
 *                  sh_modpub_gate_decide decides each, and any denial drops
 *                  the hold with its cause's sentence before the player is
 *                  asked anything
 *   RAISE          next ticks: wait until the dialog surface is idle, then ask
 *   ASKING         poll the answer
 *   FETCHING       Yes: one worker job fetches every missing build in turn
 *                  into the map's place
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
#include "modui.h"
#include "plain_reason.h"
#include "modmanifest.h"
#include "map_carrier.h"
#include "../common/text.h"

/* The fetch and the worker come from modsync.c and modnet.c: the fetch
 * downloads the build's archive, verifies it against the row and unpacks it
 * into the map's place under <data>\mods\maps\, stopping early when the
 * cancel flag becomes nonzero; the job helper runs one job at a time on a
 * worker thread. Both are adapted below, inside the engine section. */
#ifndef SH_MODPUBLISHED_PURE
#include "modnet.h"
#include "modsync.h"
#endif

#define PUB_GDM_ID          0x29u   /* GDM_CONFIRM_VIDEO_CHANGES: a Yes/No body of our own text */
#define PUB_BUTTONS_YES_NO  6u
#define PUB_BUTTONS_ACCEPT  0u
#define PUB_RAISE_TICKS     600     /* about ten seconds at 60 Hz */
#define PUB_PROGRESS_MS     2000ull
#define PUB_SEEN_MAX        16
/* The engine's dialog: its descriptor carries 255 bytes of text
 * (engine_dialog.c). Every other text here is a heap string of its own
 * length; a notice longer than the dialog holds is said whole on the console
 * and the dialog says where it is (pub_poll_notice). */
#define PUB_TEXT_CAP        256
#define PUB_DIALOG_OVER     "This message is longer than the 255 bytes the game's dialog holds (the game's limit); " \
                            "it is on the console."

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
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    pub_log(line ? line : "MODS: (out of memory)");
    free(line);
}

/* The text, or "" for none. */
static const char *pub_s(const char *text) { return text ? text : ""; }

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
    char *place;              /* the place the text records (malloc'd; NULL or "" for none) */
} pub_record;

static pub_record g_record;
/* Advanced by the main-thread tick after every native frame. A read's parse
 * and the completion (and launch) it feeds run in one call chain, inside one
 * frame; a record from an earlier frame belongs to some other load. */
static unsigned long g_frame;
/* The parse whose cause sentences the manifest holds: a gate that
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

/* The asset rule (modserve.h sh_modserve_row_served_in): a row is served
 * when the build the one place resolver names for it in `place` provides
 * every name it lists. `subject` is the word the cause uses ("map",
 * "prefab"). */
static int pub_row_state(const sh_modmanifest_row *row, const char *place, const char *subject, char **cause)
{
    if (cause) *cause = NULL;
    return g_ops.row_state ? g_ops.row_state(g_ops.context, row, place ? place : "", subject, cause)
                           : SH_MODPUB_ROW_MISSING;
}

static int pub_row_served(const sh_modmanifest_row *row, const char *place)
{
    return pub_row_state(row, place, NULL, NULL) == SH_MODPUB_ROW_SERVED;
}

static size_t pub_count_missing(const sh_modmanifest_list *rows, const char *place)
{
    size_t missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served(&rows->rows[i], place)) missing++;
    return missing;
}

/* The place `text` records (map_carrier.h), as a place ("maps\<segment>"),
 * malloc'd: "" when it records none or one that is not a place; NULL only
 * when memory runs out. */
static char *pub_text_place(const char *text, size_t length)
{
    char *segment = NULL, *out = NULL;
    if (text && length && sh_carrier_place(text, length, &segment) == 1 && g_ops.place_norm &&
        !g_ops.place_norm(g_ops.context, segment, &out)) {
        free(out);
        out = NULL;
    }
    free(segment);
    return out ? out : sh_text("");
}

/* The place the next launch serves from (sh_modpub_launch_place), set by the
 * gate or hold that let its map through, and made the loaded map's now. */
static char *g_launch_place;
static int g_launch_set;

static void pub_place_decided(const char *place)
{
    free(g_launch_place);
    g_launch_place = sh_text(pub_s(place));
    g_launch_set = 1;
    if (g_ops.place_set) g_ops.place_set(g_ops.context, pub_s(g_launch_place));
}

int sh_modpub_launch_place(char **out)
{
    int set = g_launch_set;
    if (out) *out = sh_text(set ? pub_s(g_launch_place) : "");
    g_launch_set = 0;
    return set;
}

static void pub_record_clear(pub_record *r)
{
    sh_modmanifest_list_free(&r->rows);
    free(r->text);
    free(r->place);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------ maps seen by (item, revision)
 * The warm-cache joiner branch never parses: CacheMapForLobby answers from the
 * in-memory cache. Its manifest is the one this session saw when that item and
 * revision last reached a gated completion. */

typedef struct pub_seen {
    char *id, *revision;                 /* malloc'd */
    char *place;                         /* the place its gate decided (malloc'd) */
    sh_modmanifest_list rows;
    unsigned long stamp;
} pub_seen;

static void pub_seen_clear(pub_seen *slot)
{
    sh_modmanifest_list_free(&slot->rows);
    free(slot->id);
    free(slot->revision);
    free(slot->place);
    memset(slot, 0, sizeof(*slot));
}

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

static void pub_seen_note(const char *id, const char *revision, const char *place, const sh_modmanifest_list *rows)
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
    pub_seen_clear(slot);
    slot->id = sh_text(id);
    slot->revision = sh_text(revision);
    slot->place = sh_text(pub_s(place));
    if (!slot->id || !slot->revision || !slot->place || (rows && !pub_list_copy(&slot->rows, rows))) {
        pub_seen_clear(slot);
        return;
    }
    slot->stamp = ++g_seen_stamp;
}

/* ------------------------------------------------------------ the fetch job */

typedef struct pub_job {
    struct pub_job *next;
    size_t count;
    char *place;                       /* where the fetch installs: the map's place, "" the root (malloc'd) */
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    char (*salts)[SH_MODMANIFEST_SALT_CAP];
    char **names;                        /* the row's name, else its salt (malloc'd, count of them) */
    volatile int cancel;
    volatile LONG done;
    volatile LONG current;               /* index being fetched */
    volatile LONG64 progress_done, progress_total;
    int failed_index;                    /* -1 when every fetch returned 0 */
    int failed_code;
    int probe;                           /* ask the store (store_probe) instead of fetching */
    sh_modpub_probe *probes;             /* probe: the store's answer per build */
} pub_job;

static pub_job *volatile g_running_job;  /* the job the worker is in (progress target) */
static pub_job *g_orphans;               /* abandoned jobs the worker still owns */

static void pub_job_free(pub_job *job)
{
    if (!job) return;
    free(job->hashes);
    free(job->salts);
    if (job->names) for (size_t i = 0; i < job->count; i++) free(job->names[i]);
    free(job->names);
    free(job->probes);
    free(job->place);
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
            job->probes[i].found = g_ops.store_probe ?
                g_ops.store_probe(g_ops.context, job->hashes[i], &job->cancel, &job->probes[i]) : -1;
            if (job->probes[i].found > 0) job->probes[i].found = SH_MODPUB_STORE_HAS;
            else if (job->probes[i].found < 0) job->probes[i].found = SH_MODPUB_STORE_UNREACHABLE;
        }
        MemoryBarrier();
        InterlockedExchange(&job->done, 1);
        return;
    }
    g_running_job = job;
    for (size_t i = 0; i < job->count; i++) {
        sh_modmanifest_row row;
        int rc;
        if (job->cancel) { job->failed_index = (int)i; job->failed_code = -2; break; }
        InterlockedExchange(&job->current, (LONG)i);
        InterlockedExchange64(&job->progress_done, 0);
        InterlockedExchange64(&job->progress_total, 0);
        memset(&row, 0, sizeof row);
        strcpy_s(row.salt, sizeof row.salt, job->salts[i]);
        strcpy_s(row.build, sizeof row.build, job->hashes[i]);
        row.name = job->names[i];   /* borrowed: the row is not freed */
        rc = g_ops.fetch_for ? g_ops.fetch_for(g_ops.context, &row, job->place, pub_progress, &job->cancel) : -1;
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
    char *id, *revision;                 /* malloc'd ("" for none) while a hold runs */
    char *place;                         /* the map's place ("" the root): rows resolve and fetch there (malloc'd) */
    char *text;                          /* owned, completions and local play */
    size_t length;
    sh_modmanifest_list rows;
    size_t missing_count;
    size_t *missing;                     /* indexes into rows */
    sh_modpub_probe *probes;             /* per missing row: the store's answer (after PROBING) */
    unsigned long long missing_bytes;    /* the rows' sizes, then (after PROBING) the archives' */
    unsigned missing_scripts;            /* after PROBING: script files the missing builds carry */
    int ticket;
    int waited;
    pub_job *job;
    ULONGLONG fetch_at, progress_at;     /* the fetch's start; the progress line's last update */
    int progress_shown;                  /* the progress line is up */
    int cancel_asked;                    /* the one "Keep downloading?" question was raised */
    int cancelled;                       /* the player answered No to it */
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

/* A refused launch leaves nothing declared. */
static void pub_launch_refused(int hook)
{
    if (!pub_is_local(hook) && g_ops.launch_refused) g_ops.launch_refused(g_ops.context, hook);
}

/* One pending notice (malloc'd, whole), raised with a single Accept button
 * when the surface is idle and no question is up. A later notice replaces an
 * unraised one. */
static char *g_notice;
static int g_notice_ticket, g_notice_waited;

static void pub_notice(const char *text)
{
    char *copy;
    if (!text || !text[0] || !(copy = sh_text(text))) return;
    free(g_notice);
    g_notice = copy;
    g_notice_waited = 0;
    pub_logf("MODS: notice: %s", text);
}

static void pub_notice_clear(void)
{
    free(g_notice);
    g_notice = NULL;
}

void sh_modpublished_notice(const char *text)
{
    pub_notice(text);
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
                                 unsigned long long bytes, const char *scripts)
{
    static const char ask[] = "Download and install now?";
    char head[64], size[32], list[PUB_TEXT_CAP], tail[PUB_TEXT_CAP];
    size_t room;
    if (!out || !cap) return;
    if (cap > PUB_TEXT_CAP) cap = PUB_TEXT_CAP;
    _snprintf_s(head, sizeof head, _TRUNCATE, "This %s needs %u mod%s: ", thing, (unsigned)count, count == 1 ? "" : "s");
    pub_size_text(size, sizeof size, bytes);
    /* The scripts sentence is said whole; the names give way to it. */
    if (scripts && scripts[0]) _snprintf_s(tail, sizeof tail, _TRUNCATE, "). %s %s", scripts, ask);
    else _snprintf_s(tail, sizeof tail, _TRUNCATE, "). %s", ask);
    room = cap - 1;
    room = room > strlen(head) + strlen(size) + strlen(tail) + 2 ? room - strlen(head) - strlen(size) - strlen(tail) - 2 : 1;
    if (room > sizeof list) room = sizeof list;
    pub_names_line(list, room, names, count);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s (%s%s", head, list, size, tail);
}

void sh_modpublished_compose_question(char *out, size_t cap, const char *const *names, size_t count,
                                      unsigned long long bytes, const char *scripts)
{
    pub_compose_question(out, cap, "map", names, count, bytes, scripts);
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
    /* A placeholder system (DESIGN-additive 7, phase 6) answers here, with a
     * verdict of its own, before any denial below. */
    if (f->store == SH_MODPUB_STORE_HAS) return f->game_lacks ? SH_MODPUB_GATE_GAME_REVISION : SH_MODPUB_GATE_OBTAIN;
    if (f->store == SH_MODPUB_STORE_ABSENT) return f->published ? SH_MODPUB_GATE_GONE : SH_MODPUB_GATE_NEVER_PUBLISHED;
    return SH_MODPUB_GATE_UNREACHABLE;
}

char *sh_modpub_gate_wording(int verdict, const char *subject, const char *title, const char *source,
                             int store_code)
{
    char *reason, *out;
    const int prefab = subject && !strcmp(subject, "prefab");
    const char *what = subject && subject[0] ? subject : "map";
    const char *t = title && title[0] ? title : "a mod";
    /* A prefab's denial ends as every other prefab refusal does, saying the
     * placement did not happen (architecture.md "Prefabs that name mods"). */
    const char *end = prefab ? " The prefab was not placed." : "";
    switch (verdict) {
    case SH_MODPUB_GATE_GONE:
        return sh_textf("This %s needs %s, which is no longer available: its author unpublished it, or it was removed. "
                        "This is permanent on this machine.%s", what, t, end);
    case SH_MODPUB_GATE_UNREACHABLE:
        reason = sh_reason(SH_REASON_STORE, store_code, NULL, 0);
        out = sh_textf("This %s needs %s, which could not be downloaded: %s.%s", what, t, pub_s(reason), end);
        free(reason);
        return out;
    case SH_MODPUB_GATE_NEVER_PUBLISHED:
        return sh_textf("This %s needs %s, which is not installed and was never published. Restore its folder from the "
                        "Recycle Bin or load the %s.%s", what, t, prefab ? "prefab's ZIP export" : "map's rawmap ZIP", end);
    case SH_MODPUB_GATE_GAME_REVISION:
        return sh_textf("This %s needs %s, which needs a file this game does not hold (%s): a different game revision. "
                        "This is permanent on this machine.%s", what, t, source && source[0] ? source : "a game file", end);
    default:
        return NULL;
    }
}

static const char *pub_title(const sh_modmanifest_row *row);

int sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                char **why)
{
    if (hash_out && hash_cap) hash_out[0] = 0;
    if (why) *why = NULL;
    if (!row || !g_ops_set) return 0;
    /* The editor opens the author's own map: its rows follow the root
     * folders (sh_modserve_row_build), so a row nothing serves names a salt
     * no root folder holds. Nothing substitutes for it. */
    if (why) {
        const char *key = row->salt, *title = pub_title(row);
        /* The store was not asked, so the note says only what this computer
         * knows; the gate's words ("no longer available", "never published")
         * are said where the store is asked, at Play. A map whose placed
         * entities use the mod is refused by the engine's validator (its
         * damaged-save dialog, which the save guard dismisses), so the
         * sentence says the editor cannot open it and is also the notice;
         * it fits the dialog body, else the salt alone names the mod. */
        if (placed) {
            int k;
            for (k = 0; k < 2; k++) {
                free(*why);
                *why = sh_textf("This map needs %s, which is not installed on this computer, so the editor cannot open "
                                "the map. Restore the mod's folder from the Recycle Bin, or play the map from My Maps "
                                "to get it from the mod store.", k ? key : title);
                if (!*why || strlen(*why) < PUB_TEXT_CAP) break;
            }
        } else
            *why = sh_textf("This map needs %s, which is not installed on this computer, so the editor opens the map "
                            "without it. Playing the map asks the mod store for it.", title);
    }
    return 0;
}

/* The mod as the player knows it: the row's name (the author's, as the map
 * recorded it), else its salt. Borrowed from the row. */
static const char *pub_title(const sh_modmanifest_row *row)
{
    const char *name = sh_modmanifest_row_name(row);
    return name[0] ? name : row->salt[0] ? row->salt : "a mod";
}

/* Was the map (or this build) published? Every map-service hook is; a My
 * Maps map, a rawmap or a prefab only when a claim this machine holds names
 * the build (it was published from here and not unpublished). */
static int pub_published(int hook, const sh_modmanifest_row *row)
{
    switch (hook) {
    case SH_MODPUB_ON_SNAP_PLAY: case SH_MODPUB_BROWSER_PLAY: case SH_MODPUB_CACHE_READY:
    case SH_MODPUB_CACHE_LOBBY: case SH_MODPUB_FEED_PLAY: case SH_MODPUB_LOBBY_LAUNCH:
    case SH_MODPUB_OFFLINE_LAUNCH:
        return 1;
    default:
        return g_ops.claimed && row->build[0] && g_ops.claimed(g_ops.context, row->build);
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

/* The missing builds' display names (the rows' names, else their salts),
 * malloc'd array of borrowed pointers, or NULL. */
static const char **pub_missing_names(const pub_hold *h)
{
    const char **names = calloc(h->missing_count ? h->missing_count : 1, sizeof(*names));
    if (!names) return NULL;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        names[i] = sh_modmanifest_row_name(row)[0] ? row->name : row->salt;
    }
    return names;
}

/* The question: the one download approval per map, "This map needs N
 * mod(s): <names> (<size>). Download and install now?", the size the
 * archives' total the store reported. The download lands in the map's own
 * place, so the map gets exactly the builds it pins. */
static void pub_hold_question(const pub_hold *h, char *out, size_t cap)
{
    const char **names = pub_missing_names(h);
    char scripts[PUB_TEXT_CAP] = "";
    if (h->missing_scripts && g_ops.scripts_approval)
        g_ops.scripts_approval(g_ops.context, h->missing_scripts, scripts, sizeof scripts);
    pub_compose_question(out, cap, pub_thing(h->hook), names, names ? h->missing_count : 0, h->missing_bytes, scripts);
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
    free(h->probes);
    free(h->id);
    free(h->revision);
    free(h->place);
    sh_modmanifest_list_free(&h->rows);
    memset(h, 0, sizeof(*h));
}

/* The progress line goes when the download ends, is cancelled or is dropped.
 * An abandoned hold leaves it: another load owns the screen and its line. */
static void pub_progress_hide(pub_hold *h)
{
    if (!h->progress_shown) return;
    h->progress_shown = 0;
    if (g_ops.progress) g_ops.progress(g_ops.context, h->hook, h->owner, NULL);
}

/* Silent: another load owns the screen now. */
static void pub_abandon(const char *why)
{
    if (g_hold.state == HOLD_IDLE) return;
    pub_logf("MODS: the held %s of '%s' was abandoned: %s", pub_hook_name(g_hold.hook),
             g_hold.id && g_hold.id[0] ? g_hold.id : "local map", why);
    pub_hold_free();
}

/* Visible: the player's load is refused and told why. */
static void pub_drop(const char *notice, const char *why)
{
    int hook;
    if (g_hold.state == HOLD_IDLE) return;
    pub_logf("MODS: the held %s of '%s' was dropped: %s", pub_hook_name(g_hold.hook),
             g_hold.id && g_hold.id[0] ? g_hold.id : "local map", why);
    pub_progress_hide(&g_hold);
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
                 g_hold.id && g_hold.id[0] ? g_hold.id : "the held map", g_hold.state == HOLD_FETCHING ? "download" : "are offered");
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
    g_record.place = text ? pub_text_place(text, strlen(text)) : NULL;
    g_record.missing = pub_count_missing(&g_record.rows, pub_s(g_record.place));
    /* The text is kept for every map with rows: the gate that takes the
     * parse decides the map's place, in which a row the parse found served
     * may be missing. */
    if (text) {
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

/* Build the hold from rows (copied) and, when given, the record's text
 * (moved). `place` is the map's: its rows resolve there and its fetches
 * install there. */
static int pub_hold_begin(int hook, void *owner, const void *metadata, void *extra, const char *id,
                          const char *revision, const char *place, const sh_modmanifest_list *rows, pub_record *record)
{
    pub_hold *h = &g_hold;
    if (h->state != HOLD_IDLE) pub_abandon("a newer map launch replaced it");
    memset(h, 0, sizeof(*h));
    if (!pub_list_copy(&h->rows, rows)) goto nomem;
    h->missing = calloc(rows->count ? rows->count : 1, sizeof(*h->missing));
    if (!h->missing) goto nomem;
    if (!(h->place = sh_text(pub_s(place)))) goto nomem;
    for (size_t i = 0; i < h->rows.count; i++) {
        if (pub_row_served(&h->rows.rows[i], h->place)) continue;
        h->missing[h->missing_count++] = i;
        h->missing_bytes += h->rows.rows[i].size;
    }
    if (!h->missing_count) { pub_hold_free(); return 0; }
    h->hook = hook; h->owner = owner; h->extra = extra;
    if (!(h->id = sh_text(pub_s(id))) || !(h->revision = sh_text(pub_s(revision)))) goto nomem;
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
        char *cause = NULL;
        (void)pub_row_state(&h->rows.rows[h->missing[i]], h->place, pub_thing(hook), &cause);
        if (cause && cause[0]) { pub_print(cause); pub_logf("MODS: %s", cause); }
        free(cause);
    }
    return 1;
nomem:
    pub_hold_free();
    return -1;
}

/* The player turned off "Play maps whose mods include scripts": a map that
 * arrived with its mods (it has a place; the player's own maps have none)
 * whose builds carry scripts is not loaded, however its mods got here. The
 * launch is refused with the setting's sentence. 1 when refused. */
static int pub_scripts_off(const char *place, const sh_modmanifest_list *rows)
{
    return place && place[0] && rows && rows->count && g_ops.scripted_maps_allowed &&
           !g_ops.scripted_maps_allowed(g_ops.context) && g_ops.served_scripts &&
           g_ops.served_scripts(g_ops.context, rows, place);
}

static int pub_refuse_scripts(int hook, void *owner, const char *place, const sh_modmanifest_list *rows)
{
    if (!pub_scripts_off(place, rows)) return 0;
    pub_logf("MODS: %s: the map's mods include scripts and the player turned scripts off; it is not loaded",
             pub_hook_name(hook));
    if (owner && g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
    if (owner && g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
    pub_launch_refused(hook);
    pub_print(SH_MODUI_SCRIPTS_OFF);
    pub_notice(SH_MODUI_SCRIPTS_OFF);
    return 1;
}

int sh_modpub_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                     const char *id, const char *revision)
{
    char *item = NULL, *rev = NULL, *title = NULL, *place = NULL;
    pub_record *record = &g_record;
    int begun, passed;
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
        if (pub_refuse_scripts(hook, owner, seen->place, &seen->rows)) return 0;
        if (!seen->rows.count || !pub_count_missing(&seen->rows, seen->place)) {
            pub_place_decided(seen->place);
            return 1;
        }
        begun = pub_hold_begin(hook, owner, NULL, NULL, id, revision, seen->place, &seen->rows, NULL);
        if (begun <= 0) {
            if (begun < 0) {
                pub_log("MODS: the cached lobby map needs mods and could not be held; it is refused");
                pub_launch_refused(hook);
                pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
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
    if (pub_is_completion(hook) && g_ops.metadata_ids &&
        !g_ops.metadata_ids(g_ops.context, metadata, &item, &rev)) {
        free(item); free(rev);
        item = rev = NULL;
    }
    if (pub_is_completion(hook) && g_ops.metadata_title &&
        !g_ops.metadata_title(g_ops.context, metadata, &title)) {
        free(title);
        title = NULL;
    }
    /* The map's place before anything counts what is missing (DECISIONS
     * 11): a stranger's published map is served from its item's place; a
     * map this machine published finds its mods at the root, as the
     * player's own maps do; a My Maps play uses the place the map records. */
    if (pub_is_completion(hook)) {
        int own = item && item[0] && g_ops.own_item && g_ops.own_item(g_ops.context, item);
        if (record->rows.count && !own &&
            (!g_ops.item_place || !g_ops.item_place(g_ops.context, pub_s(title), pub_s(item), &place))) {
            free(place);
            place = NULL;
        }
    } else {
        place = sh_text(pub_s(record->place));
    }
    record->missing = pub_count_missing(&record->rows, pub_s(place));
    if (pub_is_completion(hook)) pub_seen_note(pub_s(item), pub_s(rev), pub_s(place), &record->rows);
    passed = -1;
    if (pub_refuse_scripts(hook, owner, pub_s(place), &record->rows)) { pub_record_clear(record); passed = 0; goto out; }
    if (!record->rows.count || !record->missing) {
        pub_record_clear(record);   /* consumed */
        pub_place_decided(pub_s(place));
        passed = 1;
        goto out;
    }
    /* A joiner's read of a map the lobby no longer hosts: CacheReady itself
     * ignores it (it sets nothing unless the host's item matches), so there is
     * nothing to ask about. */
    if (hook == SH_MODPUB_CACHE_READY &&
        (!g_ops.host_matches || !g_ops.host_matches(g_ops.context, pub_s(item), pub_s(rev)))) {
        pub_record_clear(record);
        passed = 1;
        goto out;
    }
    begun = pub_hold_begin(hook, owner, metadata, extra, pub_s(item), pub_s(rev), pub_s(place), &record->rows, record);
    pub_record_clear(record);
    if (begun < 0) {
        /* Never hand the engine a map whose mods are absent: it ends in the
         * palette miss that is fatal at START. Refuse instead. */
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
        pub_launch_refused(hook);
        pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
        passed = 0;
        goto out;
    }
    if (!begun) { pub_place_decided(pub_s(place)); passed = 1; goto out; }
    pub_logf("MODS: %s of '%s' needs %u missing mod(s) (%llu bytes); held for the player's answer",
             pub_hook_name(hook), item && item[0] ? item : "local map", (unsigned)g_hold.missing_count,
             g_hold.missing_bytes);
    if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
    passed = 0;
out:
    free(item);
    free(rev);
    free(title);
    free(place);
    return passed;
}

int sh_modpub_launch_gate(int hook, void *manager, void *map, void *extra)
{
    pub_record *record = &g_record;
    const char *place;
    char *owned;
    size_t missing;
    int begun = -1, passed;
    if (!g_ops_set || !pub_is_launch(hook)) return 1;
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!pub_on_main()) return 1;
    /* Only a parse of this frame that no gate decided: every gate consumes the
     * record it decides on, and a launch without a parse (the lobby's own
     * relaunch from the map cache) has none. */
    if (!record->valid || record->repeat || record->frame != g_frame || !record->rows.count) return 1;
    if (map && record->map != map) return 1;
    pub_notes_gated();
    /* The parse no gate saw is decided in the place its map records (taken
     * from the record, which is cleared below). */
    owned = record->place;
    record->place = NULL;
    place = pub_s(owned);
    if (pub_refuse_scripts(hook, NULL, place, &record->rows)) { pub_record_clear(record); passed = 0; goto out; }
    missing = pub_count_missing(&record->rows, place);
    if (!missing) { pub_record_clear(record); pub_place_decided(place); passed = 1; goto out; }
    pub_logf("MODS: %s of a map no gate saw (parse %lu, map %p) names %u missing mod(s)", pub_hook_name(hook),
             record->sequence, record->map, (unsigned)missing);
    if (hook == SH_MODPUB_LOBBY_LAUNCH)
        begun = pub_hold_begin(hook, manager, NULL, extra, "", "", place, &record->rows, record);
    pub_record_clear(record);
    if (begun > 0) {
        pub_logf("MODS: %s needs %u missing mod(s) (%llu bytes); held for the player's answer", pub_hook_name(hook),
                 (unsigned)g_hold.missing_count, g_hold.missing_bytes);
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, manager);
        passed = 0;
        goto out;
    }
    if (begun == 0) { pub_place_decided(place); passed = 1; goto out; }
    /* Never hand the engine a published map whose mods are absent. An offline
     * launch has no map object left to resume with, so it is refused. */
    pub_logf("MODS: the %s was refused: its mods are not installed and it cannot be held", pub_hook_name(hook));
    if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, manager);
    pub_launch_refused(hook);
    pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
    passed = 0;
out:
    free(owned);
    return passed;
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
    if (!g_notice || g_hold.state == HOLD_RAISE || g_hold.state == HOLD_ASKING) return;
    if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
        if (++g_notice_waited > PUB_RAISE_TICKS) {
            pub_log("MODS: the notice could not be shown; the dialog surface stayed busy");
            pub_notice_clear();
        }
        return;
    }
    /* The game's dialog holds 255 bytes: a longer notice is said whole on the
     * console, and the dialog says so. */
    if (strlen(g_notice) >= PUB_TEXT_CAP) pub_print(g_notice);
    g_notice_ticket = g_ops.ask ? g_ops.ask(g_ops.context, PUB_GDM_ID, PUB_BUTTONS_ACCEPT,
                                            strlen(g_notice) < PUB_TEXT_CAP ? g_notice : PUB_DIALOG_OVER) : 0;
    if (!g_notice_ticket) pub_log("MODS: the notice dialog would not raise");
    pub_notice_clear();
}

/* The job's per-build arrays from the hold's missing rows. 1, or 0 out of
 * memory. */
static int pub_job_rows(pub_job *job, const pub_hold *h)
{
    job->count = h->missing_count;
    job->hashes = calloc(job->count ? job->count : 1, sizeof(*job->hashes));
    job->salts = calloc(job->count ? job->count : 1, sizeof(*job->salts));
    job->names = calloc(job->count ? job->count : 1, sizeof(*job->names));
    if (!job->hashes || !job->salts || !job->names) return 0;
    for (size_t i = 0; i < job->count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        strcpy_s(job->hashes[i], sizeof job->hashes[i], row->build);
        strcpy_s(job->salts[i], sizeof job->salts[i], row->salt);
        if (!(job->names[i] = sh_text(sh_modmanifest_row_name(row)[0] ? row->name : row->salt))) return 0;
    }
    return 1;
}

/* The download's progress, as the player sees it, in words: "Downloading
 * <mod> (i of n): x of y MB, p%." */
static char *pub_progress_text(const char *name, long current, size_t count, long long done, long long total)
{
    if (total > 0)
        return sh_textf("Downloading %s (%ld of %u): %.1f of %.1f MB, %d%%.", name, current + 1, (unsigned)count,
                        done / 1048576.0, total / 1048576.0, (int)(done * 100 / total));
    return sh_textf("Downloading %s (%ld of %u): %.1f MB.", name, current + 1, (unsigned)count, done / 1048576.0);
}

/* While a download runs the player sees where it is in the game's own
 * progress line, the one the game shows while it downloads a published map:
 * its text is replaced in place every PUB_PROGRESS_MS, never taken down and
 * raised again, and the same words go to the console. The game's dialog
 * shows the text it was raised with and cannot change it, so it is raised
 * once, for a download still running after PUB_PROGRESS_MS: Yes keeps
 * downloading and is not asked again, No cancels. */
static void pub_progress_step(pub_hold *h, pub_job *job)
{
    char text[PUB_TEXT_CAP], *line;
    ULONGLONG now = GetTickCount64();
    if (h->ticket) {
        int answer = g_ops.poll ? g_ops.poll(g_ops.context, h->ticket) : PUB_DIALOG_PENDING;
        if (answer != PUB_DIALOG_PENDING) {
            if (g_ops.release) g_ops.release(g_ops.context, h->ticket);
            h->ticket = 0;
            if (answer != PUB_DIALOG_ACCEPTED) {
                /* No: the download stops at its next chunk; what it fetched
                 * is removed and nothing is installed half way. */
                job->cancel = 1;
                h->cancelled = 1;
                pub_progress_hide(h);
                pub_logf("MODS: the player cancelled the download of the %s's mods", pub_thing(h->hook));
                return;
            }
        }
    }
    if (!h->progress_shown || now - h->progress_at >= PUB_PROGRESS_MS) {
        LONG current = InterlockedCompareExchange(&job->current, 0, 0);
        long long done = InterlockedCompareExchange64(&job->progress_done, 0, 0);
        long long total = InterlockedCompareExchange64(&job->progress_total, 0, 0);
        h->progress_at = now;
        line = pub_progress_text(job->names[current], current, job->count, done, total);
        if (line) {
            if (h->progress_shown) pub_print(line);   /* the first line says only "0.0 MB" */
            if (g_ops.progress) g_ops.progress(g_ops.context, h->hook, h->owner, line);
            h->progress_shown = 1;
        }
        free(line);
    }
    if (!h->cancel_asked && now - h->fetch_at >= PUB_PROGRESS_MS && g_ops.can_ask && g_ops.can_ask(g_ops.context) &&
        g_ops.ask) {
        _snprintf_s(text, sizeof text, _TRUNCATE, "Downloading the %s's mods; the progress shows on screen and on the "
                    "console. Keep downloading? No cancels it and installs nothing more.", pub_thing(h->hook));
        h->ticket = g_ops.ask(g_ops.context, PUB_GDM_ID, PUB_BUTTONS_YES_NO, text);
        h->cancel_asked = 1;
    }
}

static void pub_start_fetch(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = calloc(1, sizeof(*job));
    if (!job || !pub_job_rows(job, h) || !(job->place = sh_text(pub_s(h->place)))) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s's mods could not be downloaded: Snapmap+ ran out of memory. %s Restart DOOM and try again.",
                    pub_thing(h->hook), pub_not_done(h->hook));
        pub_job_free(job);
        pub_drop(line, "no memory for the fetch job");
        return;
    }
    h->job = job;
    h->state = HOLD_FETCHING;
    h->fetch_at = GetTickCount64();
    h->progress_shown = 0;   /* the progress line follows the question at once */
    h->cancel_asked = 0;
    if (!g_ops.worker_run || !g_ops.worker_run(g_ops.context, pub_job_main, job)) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "Snapmap+ is busy with another upload or download. %s Try again in a moment.", pub_not_done(h->hook));
        h->job = NULL;
        pub_job_free(job);
        pub_drop(line, "the worker refused the job");
        return;
    }
    pub_logf("MODS: downloading %u mod(s) for '%s'", (unsigned)job->count, h->id && h->id[0] ? h->id : "local map");
}


static void pub_resume(size_t installed);

static void pub_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    char *line, *cause = NULL;

    if (job->failed_index >= 0) {
        /* The gate decides a failed fetch too: the bundle left the store, the
         * store stopped answering, or its borrows name game bytes this
         * machine lacks. Anything else (a hash mismatch, a full disk) is the
         * download's own failure. */
        size_t at = (size_t)job->failed_index;
        const sh_modmanifest_row *row = at < h->missing_count ? &h->rows.rows[h->missing[at]] : NULL;
        char why[96];
        _snprintf_s(why, sizeof why, _TRUNCATE, "the fetch of %s returned %d", job->hashes[job->failed_index],
                    job->failed_code);
        if (row && job->failed_code != PUB_NET_CANCELLED && job->failed_code != -2) {
            sh_modpub_gate_facts facts;
            int *verdicts = calloc(h->missing_count, sizeof(*verdicts));
            char *detail = NULL;
            memset(&facts, 0, sizeof facts);
            facts.store = pub_fetch_answer(job->failed_code);
            facts.game_lacks = job->failed_code == PUB_NET_GAME;
            facts.published = pub_published(h->hook, row);
            if (facts.game_lacks && g_ops.fetch_detail &&
                !g_ops.fetch_detail(g_ops.context, job->hashes[job->failed_index], &detail)) {
                free(detail);
                detail = NULL;
            }
            if (verdicts) verdicts[at] = sh_modpub_gate_decide(&facts);
            if (verdicts && verdicts[at] != SH_MODPUB_GATE_OBTAIN) {
                if (h->probes) h->probes[at].code = job->failed_code;
                pub_deny(verdicts, h->probes, detail, why);
                free(verdicts);
                free(detail);
                return;
            }
            free(verdicts);
            free(detail);
        }
        {
            /* What stopped it, in its own words when the fetch has them (the
             * store's sentence, or why the downloaded mod did not install),
             * else the answer's plain reason. */
            char *said = NULL;
            if (g_ops.fetch_cause && g_ops.fetch_cause(g_ops.context, job->hashes[job->failed_index], &said) && said &&
                said[0]) {
                size_t n = strlen(said);
                while (n && (said[n - 1] == '.' || said[n - 1] == ' ')) said[--n] = 0;
                line = sh_textf("%s could not be downloaded and installed: %s. %s", job->names[job->failed_index], said,
                                pub_not_done(h->hook));
            } else {
                char *reason = sh_reason(SH_REASON_STORE, job->failed_code, NULL, 0);
                line = sh_textf("%s could not be downloaded: %s. %s", job->names[job->failed_index],
                                reason && reason[0] ? reason : "try again", pub_not_done(h->hook));
                free(reason);
            }
            free(said);
        }
        pub_drop(line ? line : "A mod could not be downloaded.", why);
        free(line);
        return;
    }
    if (g_ops.rescan) (void)g_ops.rescan(g_ops.context);
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        free(cause);
        cause = NULL;
        if (pub_row_state(row, h->place, pub_thing(h->hook), &cause) == SH_MODPUB_ROW_SERVED) continue;
        line = sh_textf("%s was downloaded but cannot be used.%s%s %s",
                        sh_modmanifest_row_name(row)[0] ? row->name : "The mod", cause && cause[0] ? " " : "",
                        pub_s(cause), pub_not_done(h->hook));
        pub_drop(line ? line : "A mod was downloaded but cannot be used.", "a fetched row is not served");
        free(line);
        free(cause);
        return;
    }
    free(cause);
    pub_resume(h->missing_count);
}

/* The hold's mods are in place (fetched into the map's place): the load goes
 * on. `installed` is the number of mods the fetch installed, for the log
 * line. */
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
        pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed", pub_hook_name(h->hook), h->id,
                 (unsigned)installed);
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
        pub_drop("The mods are installed, but the host changed maps while they downloaded. Join the lobby again.",
                 "the lobby host's item differs from the held map");
        return;
    }
    /* A rawmap is activated by its own parse when it opens again; activating
     * now would change what the map that is open serves. */
    if (h->hook != SH_MODPUB_RAWMAP_LOAD) {
        /* The map's place is the loaded map's from here, and its launch's.
         * Each row through the build that serves it there (the asset rule). */
        pub_place_decided(h->place);
        if (g_ops.activate) (void)g_ops.activate(g_ops.context, &h->rows);
    }

    memset(&r, 0, sizeof r);
    r.hook = h->hook; r.owner = h->owner; r.extra = h->extra; r.metadata = h->metadata;
    r.text = h->text; r.length = h->length; r.id = h->id; r.revision = h->revision;
    InterlockedExchange(&g_resuming, 1);
    ok = g_ops.resume ? g_ops.resume(g_ops.context, &r) : 0;
    InterlockedExchange(&g_resuming, 0);
    if (!ok) {
        pub_drop("The mods are installed, but the map could not be opened. Open it again.",
                 "the resume did not reach the original");
        return;
    }
    pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed", pub_hook_name(h->hook),
             h->id && h->id[0] ? h->id : "local map", (unsigned)installed);
    pub_hold_free();
}

/* ------------------------------------------------------------ rawmap loads */

static void pub_print(const char *line)
{
    if (g_ops.print) g_ops.print(g_ops.context, line);
}

/* A row's name, the way the refusal names a mod (the salt when the row has
 * no name). Borrowed from the row. */
static const char *pub_row_label(const sh_modmanifest_row *row)
{
    return sh_modmanifest_row_name(row)[0] ? row->name : row->salt;
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

/* "<head><mods joined by ", "><tail>", every name whole (malloc'd). */
static char *pub_join(const char *head, const char *const *mods, size_t count, const char *tail)
{
    size_t i, n = strlen(head) + strlen(tail) + 1;
    char *out;
    for (i = 0; i < count; i++) n += strlen(mods[i]) + 2;
    if (!(out = (char *)malloc(n))) return NULL;
    strcpy_s(out, n, head);
    for (i = 0; i < count; i++) {
        if (i) strcat_s(out, n, ", ");
        strcat_s(out, n, mods[i]);
    }
    strcat_s(out, n, tail);
    return out;
}

char *sh_modpublished_prefab_refusal_line(const char *name, const char *const *mods, size_t count, int mode)
{
    char *head = sh_textf("Prefab %s was not placed: it needs ", name && name[0] ? name : "prefab"), *out;
    if (!head) return NULL;
    out = pub_join(head, mods, count, mode == 1
        ? ", which you do not have, and the mod store could not be reached. Try again, or ask the prefab's author "
          "for its .zip export, which carries the mods."
        : mode == 2 ? ", which you do not have. Ask the prefab's author for its .zip export, which carries the mods."
        : ", which you do not have and the mod store does not hold. Ask the prefab's author for its .zip export, "
          "which carries the mods.");
    free(head);
    return out;
}

size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows, char **place)
{
    unsigned rejected = 0;
    size_t missing;
    char *at;
    if (place) *place = NULL;
    if (!rows) return 0;
    memset(rows, 0, sizeof(*rows));
    if (!g_ops_set || !text || !length) return 0;
    at = pub_text_place(text, length);
    sh_modmanifest_read(text, length, rows, &rejected);
    missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served(&rows->rows[i], pub_s(at))) missing++;
    if (place) *place = at;
    else free(at);
    return missing;
}

int sh_modpub_rawmap_scripts_off(const sh_modmanifest_list *rows, const char *place)
{
    if (!g_ops_set || !pub_scripts_off(place, rows)) return 0;
    pub_logf("MODS: rawmap load: the map's mods include scripts and the player turned scripts off; it is not opened");
    return 1;
}

/* One row's refusal notice: the asset rule's sentence, what did not happen,
 * and where the mod can come from. It replaces the composed notice only when
 * it fits the game's dialog (the composed one always does). */
static void pub_single_notice(char *notice, size_t cap, const char *cause, int hook, int unreachable)
{
    char *text = sh_textf("%s %s %s", cause, pub_not_done(hook),
                          unreachable ? "The mod store could not be reached; try again, or ask its author for the .zip export."
                                      : "Ask its author for the .zip export.");
    if (text && strlen(text) < cap && strlen(text) < PUB_TEXT_CAP) strcpy_s(notice, cap, text);
    free(text);
}

/* The refusal: the notice, and one console line naming every mod. */
static void pub_rawmap_refuse(int hook, const char *source, const char *place, const sh_modmanifest_row *const *rows,
                              size_t count, int unreachable, const char *why)
{
    char notice[PUB_TEXT_CAP], *line, *head;
    const char **labels = calloc(count ? count : 1, sizeof(*labels));
    const char *bare = pub_is_prefab(hook)
        ? "This prefab was not placed: it needs mods you do not have, and Snapmap+ ran out of memory naming them. Restart DOOM and place it again."
        : "This map was not loaded: it needs mods you do not have, and Snapmap+ ran out of memory naming them. Restart DOOM and open it again.";
    char *single = NULL;
    /* The asset rule's sentence for each row first; one row's sentence is
     * also the notice. */
    for (size_t i = 0; i < count; i++) {
        char *cause = NULL;
        (void)pub_row_state(rows[i], g_hold.state != HOLD_IDLE ? g_hold.place : place, pub_thing(hook), &cause);
        if (cause && cause[0]) {
            pub_print(cause);
            if (count == 1 && !single) { single = cause; cause = NULL; }
        }
        free(cause);
    }
    if (!labels) {
        free(single);
        if (g_hold.state != HOLD_IDLE) pub_drop(bare, why);
        else pub_notice(bare);
        return;
    }
    for (size_t i = 0; i < count; i++) labels[i] = pub_row_label(rows[i]);
    if (pub_is_prefab(hook)) {
        line = sh_modpublished_prefab_refusal_line(source, labels, count, unreachable ? 1 : 0);
        sh_modpublished_compose_prefab_refusal(notice, sizeof notice, labels, count, unreachable ? 1 : 0);
    } else {
        head = sh_textf("Rawmap %s was not loaded: it needs ", source && source[0] ? source : "map");
        line = head ? pub_join(head, labels, count, unreachable
            ? ", which you do not have, and the mod store could not be reached. Try again, or ask the map's author "
              "for its .zip export, which carries the mods."
            : ", which you do not have and the mod store does not hold. Ask the map's author for its .zip export, "
              "which carries the mods.") : NULL;
        free(head);
        sh_modpublished_compose_refusal(notice, sizeof notice, labels, count, unreachable);
    }
    if (single) pub_single_notice(notice, sizeof notice, single, hook, unreachable);
    free((void *)labels);
    free(single);
    if (line) {
        pub_print(line);
        pub_logf("MODS: %s", line);
        free(line);
    }
    if (g_hold.state != HOLD_IDLE) pub_drop(notice, why);
    else pub_notice(notice);
}

int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source, const char *place)
{
    const char *name = pub_s(source);
    int begun;
    if (!rows || !rows->count) return 0;
    if (!g_ops_set || !pub_on_main()) {
        const sh_modmanifest_row **missing = calloc(rows->count, sizeof(*missing));
        size_t n = 0;
        if (missing) {
            for (size_t i = 0; i < rows->count; i++)
                if (!pub_row_served(&rows->rows[i], place)) missing[n++] = &rows->rows[i];
            if (n) pub_rawmap_refuse(SH_MODPUB_RAWMAP_LOAD, name, place, missing, n, 0, "the rawmap gate is unavailable here");
            free((void *)missing);
        }
        return -1;
    }
    /* The rawmap's place is the one its text records (its import's, for a
     * ZIP): its rows resolve there and its fetches install there. */
    begun = pub_hold_begin(SH_MODPUB_RAWMAP_LOAD, NULL, NULL, NULL, name, "", place, rows, NULL);
    if (begun == 0) return 0;
    if (begun < 0) {
        pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
        return -1;
    }
    g_hold.state = HOLD_PROBING;
    pub_logf("MODS: rawmap '%s' needs %u missing mod(s) (%llu bytes); asking the mod store for them", name,
             (unsigned)g_hold.missing_count, g_hold.missing_bytes);
    {
        char *line = sh_textf("Rawmap %s needs %u mod%s you do not have; checking the mod store.",
                              name[0] ? name : "map", (unsigned)g_hold.missing_count, g_hold.missing_count == 1 ? "" : "s");
        if (line) pub_print(line);
        free(line);
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
    const char *nm = pub_s(name);
    char *line;
    int begun;
    if (!rows || !rows->count) return 0;
    if (!g_ops_set) return -2;
    if (!pub_on_main()) return -1;
    /* A prefab's mods are the player's own once placed: they install at the
     * root (DECISIONS 12). */
    begun = pub_hold_begin(SH_MODPUB_PREFAB_PLACE, NULL, NULL, NULL, nm, "", "", rows, NULL);
    if (begun <= 0) return begun;
    g_hold.state = HOLD_PROBING;
    pub_logf("MODS: prefab '%s' needs %u missing mod(s) (%llu bytes); asking the mod store for them", nm,
             (unsigned)g_hold.missing_count, g_hold.missing_bytes);
    line = sh_textf("Prefab %s needs %u mod%s you do not have; checking the mod store.",
                    nm[0] ? nm : "prefab", (unsigned)g_hold.missing_count, g_hold.missing_count == 1 ? "" : "s");
    if (line) pub_print(line);
    free(line);
    return 1;
}

static void pub_start_probe(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->probe = 1;
        job->probes = calloc(h->missing_count ? h->missing_count : 1, sizeof(*job->probes));
    }
    if (!job || !job->probes || !pub_job_rows(job, h)) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s's mods could not be checked: Snapmap+ ran out of memory. %s Restart DOOM and try again.", pub_thing(h->hook),
                    pub_not_done(h->hook));
        pub_job_free(job);
        pub_drop(line, "no memory for the store check");
        return;
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
    char *notice = NULL;
    size_t denied = 0;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        char *line;
        if (verdicts[i] == SH_MODPUB_GATE_OBTAIN) continue;
        line = sh_modpub_gate_wording(verdicts[i], pub_thing(h->hook), pub_title(row), detail, probes ? probes[i].code : 0);
        if (!line) continue;
        pub_print(line);
        pub_logf("MODS: denied (cause %d, %s@%.16s): %s", verdicts[i], row->salt, row->build, line);
        if (!denied++) notice = line;
        else free(line);
    }
    if (denied > 1 && notice)
        (void)sh_text_set(&notice, "%s %u more mod%s cannot be obtained; see the console.", notice,
                          (unsigned)(denied - 1), denied == 2 ? "" : "s");
    pub_drop(notice, why);
    free(notice);
}

static void pub_probe_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    int *verdicts = calloc(h->missing_count ? h->missing_count : 1, sizeof(*verdicts));
    size_t denied = 0;
    if (!verdicts) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s's mods could not be checked: Snapmap+ ran out of memory. %s Restart DOOM and try again.", pub_thing(h->hook),
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
        facts.published = pub_published(h->hook, row);
        verdicts[i] = sh_modpub_gate_decide(&facts);
        if (verdicts[i] != SH_MODPUB_GATE_OBTAIN) denied++;
    }
    if (denied) {
        pub_deny(verdicts, h->probes, NULL, "the gate denied the launch");
    } else {
        /* The question names the size the store reported for each archive
         * (the row's own size when it reported none) and the scripts the
         * builds carry. */
        h->missing_bytes = 0;
        h->missing_scripts = 0;
        for (size_t i = 0; i < h->missing_count; i++) {
            h->missing_bytes += h->probes[i].size ? h->probes[i].size : h->rows.rows[h->missing[i]].size;
            h->missing_scripts += h->probes[i].scripts;
        }
        /* The player turned scripts off: such a map is neither downloaded
         * nor played, and the sentence says what to turn on and where. */
        if (h->missing_scripts && g_ops.scripted_maps_allowed && !g_ops.scripted_maps_allowed(g_ops.context)) {
            pub_print(SH_MODUI_SCRIPTS_OFF);
            pub_drop(SH_MODUI_SCRIPTS_OFF, "the map's mods include scripts and the player turned scripts off");
            free(verdicts);
            return;
        }
        pub_logf("MODS: the mod store holds all %u missing mod(s) of the %s of '%s'", (unsigned)h->missing_count,
                 pub_hook_name(h->hook), h->id && h->id[0] ? h->id : "local map");
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
        char *refusal = NULL;
        int refused = sh_modmanifest_load_refusal(&refusal);
        g_notes_held = 0;
        (void)sh_modmanifest_load_notes_flush(1);
        /* The editor cannot open the map: the player is told why, in words. */
        if (refused && refusal) pub_notice(refusal);
        free(refusal);
    }
    if (h->state == HOLD_IDLE && !g_notice && !g_notice_ticket && !g_orphans) return;
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
            /* The one row's sentence, when the dialog holds it whole. */
            char *cause = NULL, *text;
            (void)pub_row_state(&h->rows.rows[h->missing[0]], h->place, pub_thing(h->hook), &cause);
            text = cause && cause[0] ? sh_textf("%s %s", cause, pub_not_done(h->hook)) : NULL;
            if (text && strlen(text) < sizeof line) strcpy_s(line, sizeof line, text);
            free(text);
            free(cause);
        }
        pub_drop(line, "the player declined");
        break;
    }
    case HOLD_FETCHING: {
        pub_job *job = h->job;
        if (!InterlockedCompareExchange(&job->done, 0, 0)) {
            if (!h->cancelled) pub_progress_step(h, job);
            break;
        }
        if (h->ticket) {
            if (g_ops.release) g_ops.release(g_ops.context, h->ticket);
            h->ticket = 0;
        }
        pub_progress_hide(h);
        if (h->cancelled) {
            char line[PUB_TEXT_CAP];
            _snprintf_s(line, sizeof line, _TRUNCATE, "The download was cancelled. %s", pub_not_done(h->hook));
            pub_print(line);
            pub_drop(line, "the player cancelled the download");
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
    g_prefab_ready = NULL;
    for (size_t i = 0; i < PUB_SEEN_MAX; i++) pub_seen_clear(&g_seen[i]);
    pub_notice_clear(); g_notice_ticket = 0; g_notice_waited = 0;
    free(g_launch_place); g_launch_place = NULL; g_launch_set = 0;
    InterlockedExchange(&g_resuming, 0);
}
int sh_modpub_test_state(void) { return g_hold.state; }
const char *sh_modpub_test_notice(void) { return g_notice ? g_notice : ""; }
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
#include "modexport.h"  /* modexport_fetch_build: a fetch into the map's place or the root */

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

/* A native idStr's text, whole (malloc'd): length at +8, data at +0x10.
 * NULL when it does not read. */
static char *eng_string(const void *s)
{
    char *out = NULL;
    if (!s) return NULL;
    __try {
        int length = *(const int *)((const uint8_t *)s + 8);
        const char *data = *(const char *const *)((const uint8_t *)s + 0x10);
        if (length < 0 || (length && !data)) return NULL;
        if ((out = (char *)malloc((size_t)length + 1)) == NULL) return NULL;
        if (length) memcpy(out, data, (size_t)length);
        out[length] = 0;
        return out;
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(out); return NULL; }
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

static int eng_row_state(void *c, const sh_modmanifest_row *row, const char *place, const char *subject, char **cause)
{ (void)c; return sh_modserve_row_served_in(row, place, subject, cause); }
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
static char g_eng_fetch_hash[SH_MODMANIFEST_HASH_CAP], *g_eng_fetch_source;   /* the source: malloc'd, or NULL */
static char g_eng_fetch_cause_hash[SH_MODMANIFEST_HASH_CAP], *g_eng_fetch_cause;   /* the last failed fetch's own words */

C_ASSERT(PUB_NET_CONFIG == MODNET_E_CONFIG && PUB_NET_NET == MODNET_E_NET && PUB_NET_TIMEOUT == MODNET_E_TIMEOUT);
C_ASSERT(PUB_NET_CANCELLED == MODNET_E_CANCELLED && PUB_NET_HTTP == MODNET_E_HTTP && PUB_NET_PROTOCOL == MODNET_E_PROTOCOL);
C_ASSERT(PUB_NET_NOT_FOUND == MODNET_E_NOT_FOUND && PUB_NET_RATE == MODNET_E_RATE && PUB_NET_BLOCKED == MODNET_E_BLOCKED);
C_ASSERT(PUB_NET_UNAVAILABLE == MODNET_E_UNAVAILABLE && PUB_NET_GAME == MODNET_E_GAME);

/* Fetch the row's build where the map finds it (modexport.h
 * modexport_fetch_build): its place, or the root. */
static int eng_fetch_for(void *c, const sh_modmanifest_row *row, const char *place, sh_modpub_progress_fn progress,
                         volatile int *cancel)
{
    modnet_io io;
    modsync_stats st;
    char *where = NULL;
    int code;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&st, 0, sizeof st);
    io.cancel = (volatile long *)cancel;
    io.progress = (modnet_progress_fn)progress;
    g_eng_fetch_hash[0] = 0;
    free(g_eng_fetch_source);
    g_eng_fetch_source = NULL;
    if (modexport_fetch_build(row, place, &io, &st, &where)) {
        if (where && where[0])
            pub_logf("MODS: %s is served by mods\\%s%s", pub_title(row), where,
                     st.already_installed ? " (already there)" : "");
        free(where);
        modsync_stats_free(&st);
        return 0;
    }
    pub_logf("MODS: the download of %s failed: %s", pub_title(row), st.cause && st.cause[0] ? st.cause : modnet_code_text(st.code));
    free(g_eng_fetch_cause);
    g_eng_fetch_cause = st.cause && st.cause[0] ? sh_text(st.cause) : NULL;
    strcpy_s(g_eng_fetch_cause_hash, sizeof g_eng_fetch_cause_hash, row->build);
    if (st.game_lacks) {
        strcpy_s(g_eng_fetch_hash, sizeof g_eng_fetch_hash, row->build);
        g_eng_fetch_source = st.game_source ? sh_text(st.game_source) : NULL;
    }
    code = st.code ? st.code : -1;
    modsync_stats_free(&st);
    return code;
}

/* The published metadata's title (an idStr at +0x398; the item id is at +0
 * and the revision at +0x30, as eng_metadata_ids reads them). */
#define ENG_METADATA_TITLE 0x398
static int eng_metadata_title(void *c, const void *metadata, char **out)
{
    (void)c;
    *out = metadata ? eng_string((const uint8_t *)metadata + ENG_METADATA_TITLE) : NULL;
    return *out && (*out)[0];
}

static int eng_own_item(void *c, const char *item)
{
    (void)c;
    return item && item[0] && (modsync_claim_lookup(item, NULL, NULL) || sh_modpublish_item_linked(item));
}

/* The published item's place: the record's, else a free one its title names. */
static int eng_item_place(void *c, const char *title, const char *item, char **out)
{
    sh_ms_place place;
    (void)c;
    *out = NULL;
    if (item && item[0] && sh_ms_place_of(item, out, NULL)) return 1;
    place.title = title && title[0] ? title : "map";
    place.map_id = item && item[0] ? item : NULL;
    /* Noted now, so the item keeps this place whatever else takes the title. */
    return (*out = sh_ms_place_dir(&place)) != NULL && sh_ms_place_note(&place, *out);
}

static void eng_place_set(void *c, const char *place) { (void)c; (void)sh_modserve_place_set(place); }

static int eng_place_norm(void *c, const char *place, char **out)
{
    (void)c;
    return (*out = sh_modserve_place_norm(place)) != NULL;
}

static int eng_fetch_cause(void *c, const char *hash, char **out)
{
    (void)c;
    *out = NULL;
    if (!hash || _stricmp(hash, g_eng_fetch_cause_hash) || !g_eng_fetch_cause || !g_eng_fetch_cause[0]) return 0;
    return (*out = sh_text(g_eng_fetch_cause)) != NULL;
}

static int eng_fetch_detail(void *c, const char *hash, char **out)
{
    (void)c;
    *out = NULL;
    if (!hash || _stricmp(hash, g_eng_fetch_hash) || !g_eng_fetch_source || !g_eng_fetch_source[0]) return 0;
    return (*out = sh_text(g_eng_fetch_source)) != NULL;
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

static int eng_metadata_ids(void *c, const void *metadata, char **id, char **rev)
{
    (void)c;
    *id = metadata ? eng_string(metadata) : NULL;
    *rev = metadata ? eng_string((const uint8_t *)metadata + 0x30) : NULL;
    return *id && *rev;
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
    const uint8_t *parameters = NULL;
    char *item, *rev;
    int same;
    (void)c;
    if (!id || !id[0] || !revision) return 0;
    __try {
        void *session = g_eng.session_slot ? *g_eng.session_slot : NULL, *lobby;
        if (!session) return 0;
        lobby = ((void *(*)(void *))(*(void ***)session)[g_eng.slot_lobby])(session);
        if (!lobby) return 0;
        parameters = ((const uint8_t *(*)(void *))(*(void ***)lobby)[g_eng.slot_parameters])(lobby);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!parameters) return 0;
    item = eng_string(parameters + ENG_LOBBY_ITEM);
    rev = eng_string(parameters + ENG_LOBBY_REVISION);
    same = item && rev && !strcmp(item, id) && !strcmp(rev, revision);
    free(item);
    free(rev);
    return same;
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

/* The download's progress in the manager's progress line, the widget
 * CacheMapForLobby fills with its wait text while the game downloads a
 * published map (show_wait replaces the line's text in place; hide_progress
 * takes it down). A rawmap or prefab hold has no owner: the shell's manager.
 * The widget keeps its own copy of the text, made in process heap 0 as the
 * engine's own call makes it. */
static void eng_progress(void *c, int hook, void *owner, const char *text)
{
    void *manager = owner ? eng_manager_of(hook, owner) : eng_shell_manager();
    sh_process_heap_scope scope;
    (void)c;
    if (!manager || !g_eng.show_wait || !g_eng.hide_progress || !sh_process_heap_enter(&g_eng.heap, &scope)) return;
    __try {
        if (text) {
            uint8_t line[ENG_STRING_SIZE];
            g_eng.idstr_ctor(line, text);
            g_eng.show_wait(manager, line);
            g_eng.idstr_dtor(line);
        } else {
            g_eng.hide_progress(manager);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: the download's progress line faulted"); }
    (void)sh_process_heap_leave(&scope);
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
        /* The launched map's place, as its gate or hold decided it, before
         * its serving is computed; a launch no gate decided keeps the place
         * its parse set. */
        char *place = NULL;
        if (sh_modpub_launch_place(&place) && place) (void)sh_modserve_place_set(place);
        free(place);
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
        char *note = NULL;
        ran = sh_rawmap_reopen_after_mods(&note);
        if (note && note[0]) pub_notice(note);
        free(note);
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

static void eng_print(void *c, const char *line) { (void)c; if (line) sh_printf("%s\n", line); }

/* The gate's probe: HEAD of the build's archive (modsync.h). Whether its
 * borrows resolve against this game is learned when it is installed. */
static int eng_store_probe(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out)
{
    modnet_io io;
    modsync_probe p;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&p, 0, sizeof p);
    io.cancel = (volatile long *)cancel;
    (void)modsync_probe_build(hash, &io, &p);
    memset(out, 0, sizeof *out);
    out->found = p.found > 0 ? SH_MODPUB_STORE_HAS : p.found == 0 ? SH_MODPUB_STORE_ABSENT : SH_MODPUB_STORE_UNREACHABLE;
    out->code = p.found < 0 ? p.code : 0;
    out->size = p.found > 0 ? p.size : 0;
    out->scripts = p.found > 0 ? p.scripts : 0;
    if (p.found <= 0) pub_logf("MODS: store check of %.16s: %s", hash, modnet_code_text(p.code));
    return out->found;
}

static int eng_scripted_maps_allowed(void *c) { (void)c; return sh_modui_scripted_maps_allowed(); }

/* The script files of the builds serving the rows in `place`, from each
 * serving folder's mod.json. Asked only while scripts are turned off. */
static unsigned eng_served_scripts(void *c, const sh_modmanifest_list *rows, const char *place)
{
    const char *root = sh_ms_root();
    unsigned n = 0;
    (void)c;
    if (!root) return 0;
    for (size_t i = 0; i < rows->count; i++) {
        sh_modserve_bundle_ref ref;
        sh_mb_bundle b;
        char *folder;
        memset(&ref, 0, sizeof ref);
        if (!sh_modserve_row_build_in(&rows->rows[i], place, &ref) || !ref.folder || !ref.folder[0] ||
            !(folder = sh_textf("%s\\%s", root, ref.folder))) {
            sh_modserve_bundle_ref_free(&ref);
            continue;
        }
        sh_modserve_bundle_ref_free(&ref);
        memset(&b, 0, sizeof b);
        if (sh_mb_load_folder(folder, &b)) n += b.script_files;
        sh_mb_free(&b);
        free(folder);
    }
    return n;
}
static void eng_scripts_approval(void *c, unsigned files, char *out, size_t cap)
{
    (void)c;
    sh_modui_scripts_approval(files, out, cap);
}

static int eng_claimed(void *c, const char *hash) { (void)c; return modsync_claim_names(hash); }

/* The gate refused a launch: the map scope declares nothing of it. */
static void eng_launch_refused(void *c, int hook)
{
    (void)c;
    __try {
        /* The map the editor still shows finds its mods where it did. */
        (void)sh_modserve_place_restore();
        (void)sh_modui_scope_launch_refused(pub_hook_name(hook));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the refused launch's scope clear faulted");
    }
}

static const sh_modpub_ops ENG_OPS = {
    NULL, eng_on_main, eng_row_state, eng_rescan, eng_activate, eng_can_ask, eng_ask, eng_poll,
    eng_release, eng_worker_run, eng_fetch_for, eng_metadata_copy, eng_metadata_free, eng_metadata_ids,
    eng_hold_token, eng_owner_current, eng_host_matches, eng_hold_ui, eng_drop_ui, eng_resume, eng_log,
    eng_print, eng_store_probe, eng_claimed, eng_fetch_detail, eng_launch_refused, eng_metadata_title,
    eng_own_item, eng_item_place, eng_place_set, eng_place_norm, eng_scripted_maps_allowed, eng_scripts_approval,
    eng_served_scripts, eng_progress, eng_fetch_cause
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
    char *item = NULL, *rev = NULL;
    int pass;
    eng_cache_lobby_fn original = (eng_cache_lobby_fn)g_eng.original[ENG_LOBBY];
    if (!InterlockedCompareExchange(&g_eng.ready, 0, 0) || InterlockedCompareExchange(&g_resuming, 0, 0) ||
        eng_cache_warm(id, revision) != 1 || !(item = eng_string(id)) || !(rev = eng_string(revision))) {
        free(item);
        return original(manager, id, revision);
    }
    pass = eng_decide(SH_MODPUB_CACHE_LOBBY, manager, NULL, NULL, NULL, item, rev);
    free(item);
    free(rev);
    if (pass) return original(manager, id, revision);
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
