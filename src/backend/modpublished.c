/* modpublished.c -- see modpublished.h.
 *
 * One hold at a time. A hold owns: the manifest rows of its parse, the rows still missing, a copy of the map text, a
 * copy of the published metadata (completions), the object the completion was called on and, for local play, the
 * saved-map model. Its life:
 *
 *   decide()       the completion is skipped; hold_ui does what the completion itself would have done to the screen
 *                  first (OnSnapPlay clears its wait dialog and hides the progress widget)
 *   BEGUN          next tick: rows that name units (SPEC-distribution 5.7) are fetched at once, with no question, into
 *                  the content cache, four units at a time, under a progress line; whatever cannot be had stands in
 *                  (modcache.h). A row that names no unit (a local row whose folder this computer does not have) cannot
 *                  be downloaded: the load is refused with one sentence
 *   FETCHING       the job runs on the worker
 *   (resume)       the job is done, on a main tick: rescan the index, check every row is served (the asset rule),
 *                  re-check the owner and (joiner) the lobby host's item, activate, parse the retained text into a
 *                  fresh map and call the original completion with it
 *
 * A refusal drops the hold with a notice naming the mod; the screen is left as the completion's own first steps leave
 * it, with nothing launched. Another map load, or a newer hold, abandons the hold silently (its fetch is cancelled and
 * its job retired when the worker returns).
 *
 * A rawmap load (sh_modpub_rawmap_hold) has no completion to skip: the rawmap layer never handed the map to the engine.
 * Its resume opens the rawmap again. A prefab is never held: it carries units (prefab_mods.h, D35).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modpublished.h"
#include "modcache.h"
#include "modui.h"
#include "plain_reason.h"
#include "modmanifest.h"
#include "modserve.h"
#include "modlobby.h"   /* the state codes a lobby joiner sends its host */
#include "../common/text.h"

/* The fetch is the content cache's (modcache.h sh_cache_prepare: a row's units into the cache, composed) and the
 * worker modnet.c's, one job at a time; both are adapted below, inside the engine section. */
#ifndef SH_MODPUBLISHED_PURE
#include "modnet.h"
#endif

#define PUB_GDM_ID          0x29u   /* GDM_CONFIRM_VIDEO_CHANGES: a Yes/No body of our own text */
#define PUB_BUTTONS_YES_NO  6u
#define PUB_BUTTONS_ACCEPT  0u
#define PUB_NOTICE_JOIN_CAP 250     /* bytes: what one notice may hold when two are joined */
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
enum { HOLD_IDLE = 0, HOLD_BEGUN, HOLD_FETCHING };

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
/* The parse whose cause sentences the manifest holds: a gate that
 * takes it drops them; a later frame's poll logs them, since no gate did. */
static int g_notes_held;
static unsigned long g_notes_frame;
/* A rawmap hold asked the store for its map's content and resumed: the reopen's editor parse has been asked for
 * already, so it does not ask again (R-LV6: one load sent two fetches to an unreachable store); its notice is said. */
static int g_reopen_asked;

/* The content notice the parse's stand-ins made (modcache.h), taken by the gate: said when the gate lets the map
 * through with nothing to fetch (a local row whose folder is not here stands in, D6a, manager Q8), dropped when a hold
 * fetches (the hold's resume says what the map plays without). malloc'd, or NULL. */
static char *g_pass_notice;

static void pub_notes_gated(void)
{
    free(g_pass_notice);
    g_pass_notice = NULL;
    if (!g_notes_held) return;
    g_notes_held = 0;
    (void)sh_modmanifest_load_notes_flush(0);
    g_pass_notice = g_ops.content_notice ? g_ops.content_notice(g_ops.context) : NULL;
    if (g_ops.content_drop) g_ops.content_drop(g_ops.context);
}

/* The gate let the map through: its stand-ins' one sentence (S2's form), when the parse made one. */
static void pub_pass_said(void)
{
    char *said = g_pass_notice;
    g_pass_notice = NULL;
    if (!said) return;
    pub_print(said);
    pub_notice(said);
    free(said);
}
static volatile LONG g_parse_sequence;
static volatile LONG g_resuming;       /* our own parse and original call are running */

/* The asset rule (modserve.h sh_modserve_row_served_for): a row is served
 * when the build the row resolver names for it provides every name it lists.
 * `subject` is the word the cause uses ("map", "prefab"). */
static int pub_row_state(const sh_modmanifest_row *row, const char *subject, char **cause)
{
    if (cause) *cause = NULL;
    return g_ops.row_state ? g_ops.row_state(g_ops.context, row, subject, cause) : SH_MODPUB_ROW_MISSING;
}

static int pub_row_served(const sh_modmanifest_row *row)
{
    /* A pinned row whose composition stands in for objects is fetched again at a play (modcache.h). */
    if (row && row->pinned && g_ops.content_complete && !g_ops.content_complete(g_ops.context, row)) return 0;
    return pub_row_state(row, NULL, NULL) == SH_MODPUB_ROW_SERVED;
}

static size_t pub_count_missing(const sh_modmanifest_list *rows)
{
    size_t missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served(&rows->rows[i])) missing++;
    return missing;
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
    char *id, *revision;                 /* malloc'd */
    sh_modmanifest_list rows;
    unsigned long stamp;
} pub_seen;

static void pub_seen_clear(pub_seen *slot)
{
    sh_modmanifest_list_free(&slot->rows);
    free(slot->id);
    free(slot->revision);
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

static void pub_seen_note(const char *id, const char *revision, const sh_modmanifest_list *rows)
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
    if (!slot->id || !slot->revision || (rows && !pub_list_copy(&slot->rows, rows))) {
        pub_seen_clear(slot);
        return;
    }
    slot->stamp = ++g_seen_stamp;
}

/* ------------------------------------------------------------ the fetch job */

typedef struct pub_job {
    struct pub_job *next;
    size_t count;
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    char (*salts)[SH_MODMANIFEST_SALT_CAP];
    char **names;                        /* the row's name, else its salt (malloc'd, count of them) */
    volatile int cancel;
    volatile LONG done;
    volatile LONG current;               /* index being fetched */
    volatile LONG64 progress_done, progress_total;
    int failed_index;                    /* -1 when every fetch returned 0 */
    int failed_code;
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
        rc = g_ops.fetch_for ? g_ops.fetch_for(g_ops.context, &row, pub_progress, &job->cancel) : -1;
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
    char *text;                          /* owned, completions and local play */
    size_t length;
    sh_modmanifest_list rows;
    size_t missing_count;
    size_t *missing;                     /* indexes into rows */
    unsigned long long missing_bytes;    /* the rows' sizes */
    int waited;
    pub_job *job;
    ULONGLONG fetch_at, progress_at;     /* the fetch's start; the progress line's last update */
    int progress_shown;                  /* the progress line is up */
    int left_lobby;                      /* a joiner's hold: the joiner left the lobby (pub_leave_lobby) */
    int units;                           /* every missing row names units (a local row names none) */
    int in_lobby;                        /* a joiner's units download while it stays in the lobby (SPEC 5.11) */
    int start_held;                      /* the lobby asked it to load before its content was here: it leaves */
    int reported;                        /* the last state code sent to the host (modlobby.h), 0 none */
} pub_hold;

static pub_hold g_hold;

/* The word the messages a hold shows use for what is held. */
static const char *pub_thing(int hook) { (void)hook; return "map"; }
static int pub_is_joiner(int hook);
/* The sentence that says nothing happened. A joiner's hold that is dropped leaves the lobby (pub_drop). */
static const char *pub_not_done(int hook)
{
    return hook == SH_MODPUB_EDITOR_OPEN ? "The map stays open without it."
         : pub_is_joiner(hook) ? "You left the lobby so the match is not held up; join it again later."
                               : "The map was not loaded.";
}
/* A hold the rawmap layer or an open editor map started: no engine completion behind it. */
static int pub_is_local(int hook)
{
    return hook == SH_MODPUB_RAWMAP_LOAD || hook == SH_MODPUB_EDITOR_OPEN;
}
/* A lobby joiner's read of the host's map (cold or warm). A joiner runs no mod
 * script (only the host does, script_hooks.c), so its own scripts setting
 * never refuses the map. */
static int pub_is_joiner(int hook) { return hook == SH_MODPUB_CACHE_READY || hook == SH_MODPUB_CACHE_LOBBY; }

/* A joiner whose map cannot be confirmed now (declined, refused, failed, or
 * downloading) leaves the lobby the engine's way, once: the host never
 * starts it on a map it does not hold. */
static void pub_leave_lobby(pub_hold *h, int hook, void *owner)
{
    if (!pub_is_joiner(hook) || (h && h->left_lobby)) return;
    if (h) h->left_lobby = 1;
    pub_log("MODS: the joiner leaves the lobby: its map's mods are not installed");
    if (g_ops.leave_lobby) g_ops.leave_lobby(g_ops.context, hook, owner);
}

/* The joiner's content state for the host (modlobby.h), each change once. */
static void pub_report(pub_hold *h, unsigned code)
{
    if (h && h->reported == (int)code) return;
    if (h) h->reported = (int)code;
    if (g_ops.lobby_report) g_ops.lobby_report(g_ops.context, code);
}

/* A refused launch leaves nothing declared. */
static void pub_launch_refused(int hook)
{
    if (!pub_is_local(hook) && g_ops.launch_refused) g_ops.launch_refused(g_ops.context, hook);
}

/* One pending notice (malloc'd, whole), raised with a single Accept button
 * when the surface is idle and no question is up. A second notice before the
 * first is raised joins it (first, a space, second) when the two fit the
 * game's 250 bytes; otherwise the later one replaces it. A notice waits as
 * long as the surface stays busy. */
static char *g_notice;
static int g_notice_ticket, g_notice_waited;
static long long g_notice_screen;   /* the screen the raised notice belongs to (ops.screen), 0 unknown */

static void pub_notice(const char *text)
{
    char *copy;
    if (!text || !text[0]) return;
    if (g_notice && strlen(g_notice) + 1 + strlen(text) <= PUB_NOTICE_JOIN_CAP) copy = sh_textf("%s %s", g_notice, text);
    else copy = sh_text(text);
    if (!copy) return;
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
    case SH_MODPUB_FEED_PLAY: return "SnapMap feed play";
    case SH_MODPUB_LOBBY_LAUNCH: return "lobby launch";
    case SH_MODPUB_OFFLINE_LAUNCH: return "offline launch";
    case SH_MODPUB_EDITOR_OPEN: return "open map's content";
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

static const char *pub_title(const sh_modmanifest_row *row);

int sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                char **why)
{
    if (hash_out && hash_cap) hash_out[0] = 0;
    if (why) *why = NULL;
    if (!row || !g_ops_set) return 0;
    if (row->pinned) {
        /* Units nothing here holds and no stand-in could take: the map loads without them (modcache.h). */
        if (why) *why = sh_textf("This map is playing without %s, which is not on this computer. Connect to the internet and open the map again to get it.", pub_title(row));
        return 0;
    }
    /* The editor opens the author's own map: its rows follow the root
     * folders (sh_modserve_row_build), so a row nothing serves names a salt
     * no root folder holds. Nothing substitutes for it. */
    if (why) {
        const char *title = pub_title(row);
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
                                "to get it from the mod store.", k ? "one of its mods" : title);
                if (!*why || strlen(*why) < PUB_TEXT_CAP) break;
            }
        } else
            *why = sh_textf("This map needs %s, which is not installed on this computer, so the editor opens the map "
                            "without it. Playing the map asks the mod store for it.", title);
    }
    return 0;
}

/* The mod as the player knows it: the row's name (the author's, as the map
 * recorded it), else "a mod". Borrowed from the row. */
static const char *pub_title(const sh_modmanifest_row *row)
{
    const char *name = sh_modmanifest_row_name(row);
    return name[0] ? name : "a mod";   /* never the salt (decision 18) */
}

static void pub_hold_free(void)
{
    pub_hold *h = &g_hold;
    if (h->job) {
        h->job->cancel = 1;
        if (InterlockedCompareExchange(&h->job->done, 0, 0)) pub_job_free(h->job);
        else { h->job->next = g_orphans; g_orphans = h->job; }
    }
    if (h->metadata && g_ops.metadata_free) g_ops.metadata_free(g_ops.context, h->metadata);
    free(h->text);
    free(h->missing);
    free(h->id);
    free(h->revision);
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
    pub_leave_lobby(&g_hold, g_hold.hook, g_hold.owner);
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
    g_notes_held = sh_modmanifest_load_notes_pending() > 0 || (g_ops.content_pending && g_ops.content_pending(g_ops.context));
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
    g_record.missing = pub_count_missing(&g_record.rows);
    /* The text is kept for every map with rows: the gate that takes the
     * parse counts again, and a hold resumes with it. */
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
 * (moved). */
static int pub_hold_begin(int hook, void *owner, const void *metadata, void *extra, const char *id,
                          const char *revision, const sh_modmanifest_list *rows, pub_record *record)
{
    pub_hold *h = &g_hold;
    if (h->state != HOLD_IDLE) pub_abandon("a newer map launch replaced it");
    memset(h, 0, sizeof(*h));
    if (!pub_list_copy(&h->rows, rows)) goto nomem;
    h->missing = calloc(rows->count ? rows->count : 1, sizeof(*h->missing));
    if (!h->missing) goto nomem;
    for (size_t i = 0; i < h->rows.count; i++) {
        if (pub_row_served(&h->rows.rows[i])) continue;
        h->missing[h->missing_count++] = i;
        h->missing_bytes += h->rows.rows[i].size;
    }
    if (!h->missing_count) { pub_hold_free(); return 0; }
    /* Rows that name units (SPEC-distribution 5.7): their content is fetched at once, with no question, and whatever
     * cannot be had stands in (modcache.h). A row that names none is refused at the next tick. */
    h->units = 1;
    for (size_t i = 0; i < h->missing_count; i++) if (!h->rows.rows[h->missing[i]].pinned) h->units = 0;
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
    h->state = HOLD_BEGUN;
    /* Each row the hold asks for, as the asset rule names it. */
    for (size_t i = 0; i < h->missing_count; i++) {
        char *cause = NULL;
        (void)pub_row_state(&h->rows.rows[h->missing[i]], pub_thing(hook), &cause);
        if (cause && cause[0]) { pub_print(cause); pub_logf("MODS: %s", cause); }
        free(cause);
    }
    return 1;
nomem:
    pub_hold_free();
    return -1;
}

/* 1 when a row names units (a pinned row): content that came from elsewhere, a publish, an export, a prefab. */
static int pub_rows_pinned(const sh_modmanifest_list *rows)
{
    for (size_t i = 0; rows && i < rows->count; i++) if (rows->rows[i].pinned) return 1;
    return 0;
}

/* The player turned off "Play maps whose mods include scripts": a map whose
 * units (its pinned rows: content that came from elsewhere; the player's own
 * local rows never count) carry scripts is not loaded, however its content
 * got here. The launch is refused with the setting's sentence. 1 when
 * refused. */
static int pub_scripts_off(int hook, const sh_modmanifest_list *rows)
{
    return !pub_is_joiner(hook) && pub_rows_pinned(rows) && g_ops.scripted_maps_allowed &&
           !g_ops.scripted_maps_allowed(g_ops.context) && g_ops.served_scripts &&
           g_ops.served_scripts(g_ops.context, rows);
}

static int pub_refuse_scripts(int hook, void *owner, const sh_modmanifest_list *rows)
{
    if (!pub_scripts_off(hook, rows)) return 0;
    pub_logf("MODS: %s: the map's mods include scripts and the player turned scripts off; it is not loaded",
             pub_hook_name(hook));
    if (owner && g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
    if (owner && g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
    pub_launch_refused(hook);
    pub_print(SH_MODUI_SCRIPTS_OFF);
    pub_notice(SH_MODUI_SCRIPTS_OFF);
    return 1;
}

/* 1 when the current manifest's rows are `rows` (same salts and builds, in order). */
static int pub_current_is(const sh_modmanifest_list *rows)
{
    sh_modmanifest_list now;
    int same;
    memset(&now, 0, sizeof now);
    if (!sh_modmanifest_current(&now)) { sh_modmanifest_list_free(&now); return 0; }
    same = now.count == rows->count;
    for (size_t i = 0; same && i < rows->count; i++)
        same = !strcmp(now.rows[i].salt, rows->rows[i].salt) && !_stricmp(now.rows[i].build, rows->rows[i].build);
    sh_modmanifest_list_free(&now);
    return same;
}

/* A joiner's map passed its gate for the lobby it is in: the joiner serves
 * in the Play scope from here, as the host's launch does. */
static void pub_joiner_ready(int hook, const char *item, const char *revision, int modded)
{
    if (!pub_is_joiner(hook)) return;
    if (hook == SH_MODPUB_CACHE_READY &&
        (!g_ops.host_matches || !g_ops.host_matches(g_ops.context, pub_s(item), pub_s(revision))))
        return;
    /* A modded map's host shows this member as holding its content (modlobby.h); its answer to the map check
     * follows. A vanilla map's lobby is told nothing. */
    if (modded) pub_report(NULL, SH_MODLOBBY_CODE_HOLDS);
    if (g_ops.scope_play) g_ops.scope_play(g_ops.context, "lobby join");
}

int sh_modpub_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                     const char *id, const char *revision)
{
    char *item = NULL, *rev = NULL;
    pub_record *record = &g_record;
    int begun, passed;
    if (!g_ops_set || hook < 0 || hook >= SH_MODPUB_HOOKS || pub_is_local(hook) || pub_is_launch(hook)) return 1;
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!pub_on_main()) return 1;

    if (hook == SH_MODPUB_CACHE_LOBBY) {
        pub_seen *seen = pub_seen_find(id, revision);
        /* A warm answer no gate of this process saw (the map was cached in an
         * earlier session, or by a path no gate took), or a seen map whose
         * rows are not the ones last listed and activated: the cold read
         * runs instead, and its CacheReady gate checks the mods and
         * activates them, as for a first join. */
        if ((!seen || (seen->rows.count && !pub_current_is(&seen->rows))) && g_ops.cold_read &&
            g_ops.cold_read(g_ops.context, owner, pub_s(id), pub_s(revision))) {
            pub_logf("MODS: lobby map '%s' revision '%s' is cached; %s, so it is read again and gated", pub_s(id),
                     pub_s(revision), seen ? "the mods last activated are another map's" : "this session never saw its manifest");
            return 0;
        }
        if (!seen) {
            pub_logf("MODS: lobby map '%s' revision '%s' is cached; this session never saw its manifest, "
                     "so it is not gated", id ? id : "", revision ? revision : "");
            return 1;
        }
        if (pub_refuse_scripts(hook, owner, &seen->rows)) return 0;
        if (!seen->rows.count || !pub_count_missing(&seen->rows)) {
            pub_joiner_ready(hook, id, revision, seen->rows.count > 0);
            return 1;
        }
        begun = pub_hold_begin(hook, owner, NULL, NULL, id, revision, &seen->rows, NULL);
        if (begun <= 0) {
            if (begun < 0) {
                pub_log("MODS: the cached lobby map needs mods and could not be held; it is refused");
                pub_launch_refused(hook);
                pub_leave_lobby(NULL, hook, owner);
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
    /* A published item serves the units it was published with (its pinned rows), for every player and the author
     * too (the owner, 2026-11-17): the author's edits reach it only by publishing again. The parse resolved the rows
     * already; this counts them again in the gate's frame. */
    record->missing = pub_count_missing(&record->rows);
    if (pub_is_completion(hook)) pub_seen_note(pub_s(item), pub_s(rev), &record->rows);
    passed = -1;
    if (pub_refuse_scripts(hook, owner, &record->rows)) { pub_record_clear(record); passed = 0; goto out; }
    if (!record->rows.count || !record->missing) {
        int rows = record->rows.count > 0;
        pub_record_clear(record);   /* consumed */
        pub_joiner_ready(hook, item, rev, rows);
        pub_pass_said();
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
    begun = pub_hold_begin(hook, owner, metadata, extra, pub_s(item), pub_s(rev), &record->rows, record);
    pub_record_clear(record);
    if (begun < 0) {
        /* Never hand the engine a map whose mods are absent: it ends in the
         * palette miss that is fatal at START. Refuse instead. */
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
        pub_launch_refused(hook);
        pub_leave_lobby(NULL, hook, owner);
        pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
        passed = 0;
        goto out;
    }
    if (!begun) { pub_joiner_ready(hook, item, rev, 1); pub_pass_said(); passed = 1; goto out; }
    pub_logf("MODS: %s of '%s' needs %u missing mod(s) (%llu bytes); held for the player's answer",
             pub_hook_name(hook), item && item[0] ? item : "local map", (unsigned)g_hold.missing_count,
             g_hold.missing_bytes);
    if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
    passed = 0;
out:
    free(item);
    free(rev);
    return passed;
}

int sh_modpub_launch_gate(int hook, void *manager, void *map, void *extra)
{
    pub_record *record = &g_record;
    size_t missing;
    int begun = -1, passed;
    sh_modmanifest_list rows_copy;
    memset(&rows_copy, 0, sizeof rows_copy);
    if (!g_ops_set || !pub_is_launch(hook)) return 1;
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!pub_on_main()) return 1;
    /* Only a parse of this frame that no gate decided: every gate consumes the
     * record it decides on, and a launch without a parse (the lobby's own
     * relaunch from the map cache) has none. */
    if (!record->valid || record->repeat || record->frame != g_frame || !record->rows.count) return 1;
    if (map && record->map != map) return 1;
    pub_notes_gated();
    /* A published read serves its published units (the co-op rule, owner 2026-11-17): its rows are pinned, and a
     * pinned row resolves to its composition only, never the author's root at another build. */
    if (pub_refuse_scripts(hook, NULL, &record->rows)) { pub_record_clear(record); passed = 0; goto out; }
    missing = pub_count_missing(&record->rows);
    if (!missing) {
        pub_record_clear(record);
        pub_pass_said();
        passed = 1;
        goto out;
    }
    pub_logf("MODS: %s of a map no gate saw (parse %lu, map %p) names %u missing mod(s)", pub_hook_name(hook),
             record->sequence, record->map, (unsigned)missing);
    if (!pub_list_copy(&rows_copy, &record->rows)) begun = -2;
    else if (hook == SH_MODPUB_LOBBY_LAUNCH)
        begun = pub_hold_begin(hook, manager, NULL, extra, "", "", &record->rows, record);
    pub_record_clear(record);
    if (begun > 0) {
        pub_logf("MODS: %s needs %u missing mod(s) (%llu bytes); held for the player's answer", pub_hook_name(hook),
                 (unsigned)g_hold.missing_count, g_hold.missing_bytes);
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, manager);
        passed = 0;
        goto out;
    }
    if (begun == 0) { pub_pass_said(); passed = 1; goto out; }
    {
        /* An offline launch has no map object left to resume with, so nothing is fetched now. A row whose content is
         * composed with stand-ins plays (5.8: the objects that are not here stand in, and the load's one notice says
         * S2); a row with no composition at all, or one that names no unit, cannot, and the launch is refused with
         * what is missing. The engine is never handed a map whose namespaces nothing serves. */
        int local = 0, absent = 0;
        for (size_t i = 0; i < rows_copy.count; i++) {
            const sh_modmanifest_row *row = &rows_copy.rows[i];
            if (pub_row_served(row)) continue;
            if (row->pinned && g_ops.row_state && g_ops.row_state(g_ops.context, row, "map", NULL) == SH_MODPUB_ROW_SERVED)
                continue;   /* composed, standing in for what is not here */
            if (row->pinned) absent++;
            else local++;
        }
        if (!local && !absent && begun < 0 && hook == SH_MODPUB_OFFLINE_LAUNCH) {
            pub_logf("MODS: the %s plays with stand-ins for the content that is not on this computer", pub_hook_name(hook));
            passed = 1;
            goto out;
        }
        pub_logf("MODS: the %s was refused: %d row(s) name no unit and %d have no content here", pub_hook_name(hook), local,
                 absent);
        if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, manager);
        pub_launch_refused(hook);
        pub_notice(local ? "This map was not loaded: it needs mods that are not on this computer and cannot be downloaded. "
                           "Ask its author to publish it again."
                         : begun < -1 || (begun < 0 && hook != SH_MODPUB_OFFLINE_LAUNCH)
                           ? "This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again."
                           : "This map was not loaded: its mod content is not on this computer. Connect to the internet and "
                             "open it again.");
        passed = 0;
    }
out:
    sh_modmanifest_list_free(&rows_copy);
    return passed;
}

/* ------------------------------------------------------------ the tick */

static void pub_poll_notice(void)
{
    int answer;
    long long screen = g_ops.screen ? g_ops.screen(g_ops.context) : 0;
    if (g_notice_ticket) {
        answer = g_ops.poll ? g_ops.poll(g_ops.context, g_notice_ticket) : -1;
        if (answer == PUB_DIALOG_PENDING) {
            /* The game changed screen under it (a published play's end showing RECOMMEND MAP): the notice is withdrawn,
             * so nothing of it draws through the game's own screens. */
            if (!g_notice_screen || !screen || screen == g_notice_screen) return;
            pub_log("MODS: the notice was withdrawn: the game changed screen under it");
        }
        if (g_ops.release) g_ops.release(g_ops.context, g_notice_ticket);
        g_notice_ticket = 0;
        g_notice_screen = 0;
        return;
    }
    if (!g_notice) return;
    if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context) || screen < 0) {   /* busy, or between screens */
        if (++g_notice_waited == PUB_RAISE_TICKS)
            pub_log("MODS: the notice is waiting: the dialog surface stayed busy; it is raised when the game can ask");
        if (g_notice_waited > PUB_RAISE_TICKS) g_notice_waited = PUB_RAISE_TICKS + 1;
        return;
    }
    /* The game's dialog holds 255 bytes: a longer notice is said whole on the
     * console, and the dialog says so. */
    if (strlen(g_notice) >= PUB_TEXT_CAP) pub_print(g_notice);
    g_notice_ticket = g_ops.ask ? g_ops.ask(g_ops.context, PUB_GDM_ID, PUB_BUTTONS_ACCEPT,
                                            strlen(g_notice) < PUB_TEXT_CAP ? g_notice : PUB_DIALOG_OVER) : 0;
    if (!g_notice_ticket) pub_log("MODS: the notice dialog would not raise");
    g_notice_screen = g_notice_ticket ? screen : 0;
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
        if (!(job->names[i] = sh_text(pub_title(row)))) return 0;
    }
    return 1;
}

/* The download's progress, as the player sees it (SPEC-distribution 5.7): "Getting this map's mod content: Eternal
 * Demons (2 of 3), 84 of 212 MB." */
static char *pub_progress_text(const char *thing, const char *name, long current, size_t count, long long done,
                               long long total)
{
    if (total > 0)
        return sh_textf("Getting this %s's mod content: %s (%ld of %u), %.0f of %.0f MB.", thing, name, current + 1,
                        (unsigned)count, done / 1048576.0, total / 1048576.0);
    return sh_textf("Getting this %s's mod content: %s (%ld of %u), %.0f MB.", thing, name, current + 1, (unsigned)count,
                    done / 1048576.0);
}

/* While a download runs the player sees where it is in the game's own progress line, the one the game shows while it
 * downloads a published map: its text is replaced in place every PUB_PROGRESS_MS, never taken down and raised again,
 * and the same words go to the console. Nothing is asked: a map's content downloads with a progress line only
 * (D-A36). */
static void pub_progress_step(pub_hold *h, pub_job *job)
{
    char *line;
    ULONGLONG now = GetTickCount64();
    if (!h->progress_shown || now - h->progress_at >= PUB_PROGRESS_MS) {
        LONG current = InterlockedCompareExchange(&job->current, 0, 0);
        long long done = InterlockedCompareExchange64(&job->progress_done, 0, 0);
        long long total = InterlockedCompareExchange64(&job->progress_total, 0, 0);
        h->progress_at = now;
        line = pub_progress_text(pub_thing(h->hook), job->names[current], current, job->count, done, total);
        if (line) {
            if (h->progress_shown) pub_print(line);   /* the first line says only "0 MB" */
            if (g_ops.progress && h->hook != SH_MODPUB_EDITOR_OPEN) g_ops.progress(g_ops.context, h->hook, h->owner, line);
            h->progress_shown = 1;
        }
        free(line);
        /* A joiner downloading in the lobby tells its host how far it is, over every row of the download. */
        if (h->in_lobby && job->count) {
            unsigned long long part = total > 0 ? (unsigned long long)(done * 1000 / total) : 0;
            pub_report(h, sh_modlobby_progress_code((unsigned long long)current * 1000 + (part > 1000 ? 1000 : part),
                                                    (unsigned long long)job->count * 1000));
        }
    }
}

static void pub_start_fetch(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = calloc(1, sizeof(*job));
    if (!job || !pub_job_rows(job, h)) {
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
    h->progress_shown = 0;
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
static char *pub_content_said(void);

/* 1 when every row of `rows` is still a row of the open map (same salt and build). */
static int pub_current_has(const sh_modmanifest_list *rows)
{
    sh_modmanifest_list now;
    int all = 1;
    memset(&now, 0, sizeof now);
    (void)sh_modmanifest_current(&now);
    for (size_t i = 0; all && i < rows->count; i++) {
        int found = 0;
        for (size_t k = 0; k < now.count && !found; k++)
            found = !strcmp(now.rows[k].salt, rows->rows[i].salt) && !_stricmp(now.rows[k].build, rows->rows[i].build);
        all = found;
    }
    sh_modmanifest_list_free(&now);
    return all;
}

/* The open map's content arrived (SH_MODPUB_EDITOR_OPEN): the open map serves it, and the open's one notice is what
 * is still absent, or, when the editor shows the new objects only at the next open, that they are here. */
static void pub_editor_fetched(size_t rows)
{
    pub_hold *h = &g_hold;
    char *said;
    int shown;
    if (!pub_current_has(&h->rows)) {
        pub_log("MODS: the open map's content arrived after the editor left the map; it stays in the content cache");
        pub_hold_free();
        return;
    }
    shown = g_ops.content_refresh ? g_ops.content_refresh(g_ops.context) : -1;
    pub_logf("MODS: the open map's content: %u row(s) asked of the mod store; the open map %s", (unsigned)rows,
             shown == 1 ? "serves what arrived" : shown == 2 ? "shows what arrived when it is opened again"
                        : shown == 0 ? "already served it" : "could not be refreshed");
    pub_hold_free();
    said = pub_content_said();
    if (!said && (shown == 2 || shown < 0))
        said = sh_text("This map's mod content is here now. It shows when you open the map again.");
    if (said) pub_notice(said);
    free(said);
}

/* An editor open whose pinned rows stood in for objects (modcache.h) asks the store for them now, with no question:
 * the map is open already, so nothing waits on the answer. The open's one notice waits for it (pub_editor_fetched).
 * 1 when the fetch started. */
static int pub_editor_fetch(void)
{
    sh_modmanifest_list now, want;
    int begun = 0;
    if (g_hold.state != HOLD_IDLE || !g_ops.content_complete || !g_ops.worker_run) return 0;
    memset(&now, 0, sizeof now);
    memset(&want, 0, sizeof want);
    (void)sh_modmanifest_current(&now);
    for (size_t i = 0; i < now.count; i++) {
        unsigned mask = 0;
        if (!now.rows[i].pinned || g_ops.content_complete(g_ops.context, &now.rows[i])) continue;
        /* what moderation removed is not asked for again: the store will not have it (S3) */
        if (g_ops.content_causes && g_ops.content_causes(g_ops.context, &now.rows[i], &mask) &&
            mask == (1u << SH_CACHE_ABSENT_REMOVED))
            continue;
        if (!sh_modmanifest_list_add(&want, &now.rows[i])) break;
    }
    if (want.count) begun = pub_hold_begin(SH_MODPUB_EDITOR_OPEN, NULL, NULL, NULL, "", "", &want, NULL) == 1;
    if (begun) pub_logf("MODS: the open map stands in for objects of %u row(s); asking the mod store for them",
                        (unsigned)g_hold.missing_count);
    sh_modmanifest_list_free(&now);
    sh_modmanifest_list_free(&want);
    return begun;
}

static char *pub_content_said(void);

/* A joiner downloading in the lobby enters the match only with every unit its map names: stand-ins would not be the
 * host's served bytes (its snapshot templates would differ, a fatal mismatch). 1 when the content is whole and the host
 * is told so; else the joiner tells the host it failed and leaves the lobby with one sentence (the fallback, D27). */
/* 1 when a pinned row's composition stands in only for content no player can get (S3, S5, S4b): every player composes
 * the same stand-ins, the host included (sh_modpub_lobby_host_content), so the joiner plays them (R-LB1 F5). */
static int pub_row_standin_shared(const sh_modmanifest_row *row)
{
    unsigned mask = 0;
    int c;
    if (!g_ops.content_causes || !g_ops.content_causes(g_ops.context, row, &mask) || !mask) return 0;
    for (c = 0; c < 32; c++) if ((mask & (1u << c)) && !sh_modpub_standin_shared(c)) return 0;
    return 1;
}

static int pub_joiner_content_whole(pub_hold *h)
{
    char *said;
    int stands_in = 0;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        if (!row->pinned || !g_ops.content_complete || g_ops.content_complete(g_ops.context, row)) continue;
        if (pub_row_standin_shared(row)) { stands_in = 1; continue; }
        pub_report(h, SH_MODLOBBY_CODE_FAILED);
        /* The composition's own sentence says what is absent and why (S2-S6): "This map is playing without the Tyrant
         * from Eternal Demons, which was removed from the mod store." A joiner does not play it: it leaves. */
        said = pub_content_said();
        {
            static const char lead[] = "This map is playing without ";
            char *line = said && !strncmp(said, lead, sizeof lead - 1)
                       ? sh_textf("You left the lobby: this map cannot be played without %s", said + sizeof lead - 1) : NULL;
            if (!line || strlen(line) >= PUB_TEXT_CAP) {
                free(line);
                line = sh_text("Some of this map's mod content could not be downloaded, and the match cannot be played "
                               "without it. You left the lobby so the match is not held up; join it again later.");
            }
            if (said) pub_logf("MODS: lobby join: %s", said);
            pub_drop(line ? line : "This map's mod content could not be downloaded. You left the lobby.",
                     "a joiner's units are not all here");
            free(line);
        }
        free(said);
        return 0;
    }
    if (stands_in) {
        /* What no player can get stands in for every player alike: the joiner plays the map, and says so once (S3). */
        said = pub_content_said();
        pub_logf("MODS: lobby join: the map plays with stand-ins every player has: %s",
                 said ? said : "some of its mod content no player can get");
        if (said) pub_notice(said);
        free(said);
        pub_report(h, SH_MODLOBBY_CODE_STANDS_IN);
        return 1;
    }
    pub_report(h, SH_MODLOBBY_CODE_HOLDS);
    return 1;
}

static void pub_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    char *line, *cause = NULL;

    /* A joiner whose download failed tells its host before it leaves the lobby (pub_drop). */
    if (job->failed_index >= 0 && h->in_lobby) pub_report(h, SH_MODLOBBY_CODE_FAILED);
    if (job->failed_index >= 0) {
        /* What cannot be had stands in, so a fetch fails only when it was stopped or memory ran out. */
        char why[96];
        char *reason = sh_reason(SH_REASON_STORE, job->failed_code, NULL, 0);
        _snprintf_s(why, sizeof why, _TRUNCATE, "the fetch of %s returned %d", job->hashes[job->failed_index],
                    job->failed_code);
        line = sh_textf("The mod content of %s could not be prepared: %s. %s", job->names[job->failed_index],
                        reason && reason[0] ? reason : "try again", pub_not_done(h->hook));
        free(reason);
        pub_drop(line ? line : "The map's mod content could not be prepared.", why);
        free(line);
        return;
    }
    if (g_ops.rescan) (void)g_ops.rescan(g_ops.context);
    if (h->in_lobby && !pub_joiner_content_whole(h)) return;
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        free(cause);
        cause = NULL;
        if (pub_row_state(row, pub_thing(h->hook), &cause) == SH_MODPUB_ROW_SERVED) continue;
        line = sh_textf("%s was downloaded but cannot be used.%s%s %s",
                        sh_modmanifest_row_name(row)[0] ? row->name : "The mod", cause && cause[0] ? " " : "",
                        pub_s(cause), pub_not_done(h->hook));
        pub_drop(line ? line : "A mod was downloaded but cannot be used.", "a fetched row is not served");
        free(line);
        free(cause);
        return;
    }
    free(cause);
    /* The player turned off "Play maps whose mods include scripts": content that arrived with scripts is not played. */
    if (pub_scripts_off(h->hook, &h->rows)) {
        pub_print(SH_MODUI_SCRIPTS_OFF);
        pub_drop(SH_MODUI_SCRIPTS_OFF, "the map's mods include scripts and the player turned scripts off");
        return;
    }
    pub_resume(h->missing_count);
}

/* What the resumed load plays without, in one sentence (S2-S6), said once the load has run: the fetch recorded the
 * store's answer beside the composition, and the resumed parse's content step names it, so the hold and the load
 * never say it twice (R-LV4). NULL when nothing is absent; malloc'd. */
static char *pub_content_said(void)
{
    char *said = g_ops.content_notice ? g_ops.content_notice(g_ops.context) : NULL;
    if (said) pub_print(said);
    return said;
}

/* The hold's content is here (fetched into the content cache): the load goes
 * on. `installed` is the number of mods the fetch installed, for the log
 * line. */
static void pub_resume(size_t installed)
{
    pub_hold *h = &g_hold;
    sh_modpub_resume r;
    int ok;
    /* A joiner left the lobby to download (pub_leave_lobby): it joins again,
     * and that join finds the content in the cache; its one notice says
     * so, and what the map will play without. */
    if (h->left_lobby) {
        pub_logf("MODS: the held %s of '%s' installed %u mod(s); the joiner left the lobby to download them",
                 pub_hook_name(h->hook), pub_s(h->id), (unsigned)installed);
        {
            int units = h->units;
            char *said = pub_content_said(), *line;
            pub_hold_free();
            line = said ? sh_textf("%s Join the lobby again to play it.", said) : NULL;
            pub_notice(line ? line : units ? "The map's mod content is here. Join the lobby again to play it."
                                           : "The map's mods are installed. Join the lobby again to play it.");
            free(line);
            free(said);
        }
        return;
    }
    if (h->hook == SH_MODPUB_EDITOR_OPEN) {
        pub_editor_fetched(installed);
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
        pub_drop("The mods are installed, but the map could not be opened. Open it again.",
                 "the resume did not reach the original");
        return;
    }
    pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed", pub_hook_name(h->hook),
             h->id && h->id[0] ? h->id : "local map", (unsigned)installed);
    {
        /* A rawmap opens again on a later editor frame, whose load says its notice (sh_modpublished_poll). */
        char *said = h->hook != SH_MODPUB_RAWMAP_LOAD ? pub_content_said() : NULL;
        if (h->hook == SH_MODPUB_RAWMAP_LOAD) g_reopen_asked = 1;
        pub_hold_free();
        if (said) pub_notice(said);
        free(said);
    }
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
    return pub_title(row);
}

/* mode 0: the store lacks them; 1: the store could not be reached. */
static void pub_compose_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode)
{
    static const char gone[] = ": ";
    static const char lacks[] = " and the mod store does not hold: ";
    const char *head = "This map was not loaded. It needs mods you do not have";
    const char *mid = mode == 0 ? lacks : gone;
    const char *tail = mode == 1 ? ". The mod store could not be reached. Try again, or ask the map's author for its .zip export."
                                 : ". Ask the map's author for its .zip export.";
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
    pub_compose_refusal(out, cap, mods, count, unreachable ? 1 : 0);
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

size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows)
{
    unsigned rejected = 0;
    size_t missing;
    if (!rows) return 0;
    memset(rows, 0, sizeof(*rows));
    if (!g_ops_set || !text || !length) return 0;
    sh_modmanifest_read(text, length, rows, &rejected);
    /* A pinned row's units are fetched into the content cache only when the cache knows the row: a rawmap's rows
     * are noted here, before its hold, as a load notes them before they resolve (R-LV2: the hold took the retired
     * build route and the store's 410 ended the load). */
    sh_modmanifest_content_for(text, length, rows);
    missing = 0;
    for (size_t i = 0; i < rows->count; i++)
        if (!pub_row_served(&rows->rows[i])) missing++;
    return missing;
}

int sh_modpub_rawmap_scripts_off(const sh_modmanifest_list *rows)
{
    if (!g_ops_set || !pub_scripts_off(SH_MODPUB_RAWMAP_LOAD, rows)) return 0;
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
static void pub_rawmap_refuse(int hook, const char *source, const sh_modmanifest_row *const *rows,
                              size_t count, int unreachable, const char *why)
{
    char notice[PUB_TEXT_CAP], *line, *head;
    const char **labels = calloc(count ? count : 1, sizeof(*labels));
    const char *bare = "This map was not loaded: it needs mods you do not have, and Snapmap+ ran out of memory naming them. "
                       "Restart DOOM and open it again.";
    char *single = NULL;
    /* The asset rule's sentence for each row first; one row's sentence is
     * also the notice. */
    for (size_t i = 0; i < count; i++) {
        char *cause = NULL;
        (void)pub_row_state(rows[i], pub_thing(hook), &cause);
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
    {
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

int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source)
{
    const char *name = pub_s(source);
    int begun;
    if (!rows || !rows->count) return 0;
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
    begun = pub_hold_begin(SH_MODPUB_RAWMAP_LOAD, NULL, NULL, NULL, name, "", rows, NULL);
    if (begun == 0) return 0;
    if (begun < 0) {
        pub_notice("This map was not loaded: Snapmap+ ran out of memory preparing its mods. Restart DOOM and open it again.");
        return -1;
    }
    pub_logf("MODS: rawmap '%s' needs %u missing mod(s) (%llu bytes)", name, (unsigned)g_hold.missing_count,
             g_hold.missing_bytes);
    return 1;
}

/* ------------------------------------------------------------ local rows */

/* Rows that name no unit and that nothing here serves: a local row (D6a) follows its author's folder, so no store has
 * its content. A local row whose folder is not here plays with stand-ins (the content cache's composition of no
 * units, manager Q8), so this is reached only when none could be made (memory, the cache's folder) or when a folder
 * is here and does not carry every name the row lists. The load is refused with one sentence naming the mods. */
static void pub_refuse_local(void)
{
    pub_hold *h = &g_hold;
    const char **names = calloc(h->missing_count ? h->missing_count : 1, sizeof(*names));
    char list[160] = "mods that are not on this computer";
    size_t n = 0;
    char *line;
    /* A folder that is here but does not serve every name the row lists says so in its own words (the asset rule). */
    for (size_t i = 0; i < h->missing_count; i++) {
        const sh_modmanifest_row *row = &h->rows.rows[h->missing[i]];
        char *cause = NULL;
        if (row->pinned || pub_row_state(row, pub_thing(h->hook), &cause) != SH_MODPUB_ROW_INCOMPLETE ||
            !cause || !cause[0]) {
            free(cause);
            continue;
        }
        line = sh_textf("%s %s", cause, pub_not_done(h->hook));
        free(cause);
        free((void *)names);
        pub_drop(line && strlen(line) < PUB_TEXT_CAP ? line : "This map needs a mod whose folder here is incomplete.",
                 "a row's folder is here but incomplete");
        free(line);
        return;
    }
    for (size_t i = 0; names && i < h->missing_count; i++)
        if (!h->rows.rows[h->missing[i]].pinned) names[n++] = pub_title(&h->rows.rows[h->missing[i]]);
    if (names && n) pub_names_line(list, sizeof list, names, n);
    free((void *)names);
    line = sh_textf("This %s needs %s, which %s not on this computer and cannot be downloaded: the %s was saved before "
                    "%s content was sent to the mod store. Ask its author to publish it again. %s", pub_thing(h->hook),
                    list, n > 1 ? "are" : "is", pub_thing(h->hook), n > 1 ? "their" : "its", pub_not_done(h->hook));
    if (line) pub_print(line);
    pub_drop(line && strlen(line) < PUB_TEXT_CAP ? line : "This map needs mods that are not on this computer and cannot "
                                                         "be downloaded. Ask its author to publish it again.",
             "a row names no unit and nothing here serves it");
    free(line);
}

/* A joiner downloading in the lobby follows its lobby (SPEC 5.11). The lobby asked it to load before the content was
 * here (sh_modpub_joiner_start_held): it leaves, keeps downloading and is told to join again. It is in no lobby now
 * (it left or was kicked, the host cancelled or left): the download stops, and what it fetched stays in the cache's
 * partial files and resumes from there the next time. The lobby moved to another map: this one is not wanted any more;
 * the new map's request brings its own hold. 1 when the hold ended or changed. */
static int pub_lobby_check(pub_hold *h)
{
    int where;
    if (h->start_held) {
        char *said = sh_textf("The host started the match before this map's mod content was here, so you left the lobby. "
                              "It keeps downloading (%.0f MB); join the lobby again when it is done.",
                              (double)h->missing_bytes / (1024.0 * 1024.0));
        h->in_lobby = 0;
        h->start_held = 0;
        pub_leave_lobby(h, h->hook, h->owner);
        if (said) pub_notice(said);
        free(said);
        return 1;
    }
    where = g_ops.lobby_state ? g_ops.lobby_state(g_ops.context, pub_s(h->id), pub_s(h->revision)) : SH_MODPUB_LOBBY_SAME;
    if (where == SH_MODPUB_LOBBY_SAME) return 0;
    h->job->cancel = 1;
    pub_progress_hide(h);
    if (where == SH_MODPUB_LOBBY_GONE) {
        pub_logf("MODS: the joiner is no longer in the lobby of '%s'; its download stops and resumes the next time",
                 pub_s(h->id));
        pub_hold_free();
        pub_notice("You are no longer in the lobby, so this map's mod content stopped downloading. It continues from "
                   "where it stopped the next time you join.");
    } else {
        pub_logf("MODS: the lobby moved from '%s' to another map; its download stops", pub_s(h->id));
        pub_hold_free();
    }
    return 1;
}

int sh_modpub_joiner_start_held(void)
{
    pub_hold *h = &g_hold;
    if (!g_ops_set || h->state == HOLD_IDLE || !h->in_lobby) return 0;
    if (!h->start_held) pub_log("MODS: the lobby asked this joiner to load before its map's content was here; it leaves");
    h->start_held = 1;
    return 1;
}

int sh_modpub_lobby_map_modded(const char *item, const char *revision)
{
    pub_seen *seen;
    if (!item || !item[0] || !revision) return 0;
    seen = pub_seen_find(item, revision);
    return seen && seen->rows.count > 0;
}

int sh_modpub_standin_shared(int cause)
{
    return cause == SH_CACHE_ABSENT_REMOVED || cause == SH_CACHE_ABSENT_NOT_IN_STORE || cause == SH_CACHE_ABSENT_GAME;
}

/* The causes a start waits for, most actionable first, and what the host is told for each (%s: the mods). A cause
 * not listed (none recorded, a local row's) has the last sentence. */
static const struct { int cause; const char *text; } PUB_HOST_HOLDS[] = {
    { SH_CACHE_ABSENT_UNREACHABLE, "This map cannot be hosted yet: the mod store could not be reached for %s. Try again "
                                   "when it can be reached." },
    { SH_CACHE_ABSENT_OFFLINE,     "This map cannot be hosted yet: %s is not on this computer. Connect to the internet "
                                   "and open the map once to get it, then host it." },
    { SH_CACHE_ABSENT_DISK,        "This map cannot be hosted yet: there is not enough free disk space for %s. Free some "
                                   "space, then host it." },
    { SH_CACHE_ABSENT_NEWER,       "This map cannot be hosted: %s needs a newer Snapmap+. Run the Snapmap+ installer to "
                                   "update, then host it." },
    { 0,                           "This map cannot be hosted yet: some of its mod content (%s) is not on this computer. "
                                   "Open the map once online to get it, then host it." },
};

int sh_modpub_lobby_host_content(const char *item, const char *revision, char *out, size_t cap)
{
    pub_seen *seen;
    char names[160] = "";
    size_t i, n = 0, worst = sizeof PUB_HOST_HOLDS / sizeof PUB_HOST_HOLDS[0];
    int shared = 0;
    if (out && cap) out[0] = 0;
    if (!item || !item[0] || !revision || !(seen = pub_seen_find(item, revision))) return SH_MODPUB_HOST_WHOLE;
    for (i = 0; i < seen->rows.count; i++) {
        const sh_modmanifest_row *row = &seen->rows.rows[i];
        unsigned mask = 0;
        size_t k;
        int c, held = 0;
        if (!row->pinned || !g_ops.content_complete || g_ops.content_complete(g_ops.context, row)) continue;
        /* A row standing in: which causes. One that no player can get is shared; any other holds the start. */
        if (!g_ops.content_causes || !g_ops.content_causes(g_ops.context, row, &mask) || !mask) mask = 1u;   /* not known */
        for (c = 0; c < 32; c++) {
            if (!(mask & (1u << c)) || sh_modpub_standin_shared(c)) continue;
            held = 1;
            for (k = 0; k + 1 < sizeof PUB_HOST_HOLDS / sizeof PUB_HOST_HOLDS[0] && PUB_HOST_HOLDS[k].cause != c; k++) {}
            if (k < worst) worst = k;
        }
        if (!held) { shared = 1; continue; }
        if (n++ < 3) {
            const char *label = pub_row_label(row);
            strncat_s(names, sizeof names, n > 1 ? (n == 3 ? " and " : ", ") : "", _TRUNCATE);
            strncat_s(names, sizeof names, label && label[0] ? label : "a mod", _TRUNCATE);
        }
    }
    if (!n) return shared ? SH_MODPUB_HOST_SHARED : SH_MODPUB_HOST_WHOLE;
    if (n > 3) strncat_s(names, sizeof names, " and more", _TRUNCATE);
    if (out && cap) _snprintf_s(out, cap, _TRUNCATE, PUB_HOST_HOLDS[worst].text, names);
    return SH_MODPUB_HOST_REFUSED;
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
        /* The editor cannot open the map: the player is told why, in words,
         * and the engine's own "damaged" notice for the same failed load is
         * dismissed: the map is sound. */
        if (refused && refusal) {
            if (g_ops.refused_load) g_ops.refused_load(g_ops.context);
            pub_notice(refusal);
        } else if (g_ops.editor_open && g_ops.editor_open(g_ops.context) == 0) {
            /* not the editor's: the hub reads a map after its play (the lobby, RECOMMEND MAP); it says nothing there */
            if (g_ops.content_drop) g_ops.content_drop(g_ops.context);
            pub_log("MODS: a map read outside the editor (the hub after a play) says nothing and asks nothing");
        } else if (!g_reopen_asked && pub_editor_fetch()) {
            /* The open's notice is the store's answer (pub_editor_fetched). */
        } else if (g_ops.content_notice) {
            /* An editor open composed stand-ins (modcache.h) and nothing could be asked: its one notice (S2). */
            char *said = g_ops.content_notice(g_ops.context);
            if (said) pub_notice(said);
            free(said);
        }
        free(refusal);
        g_reopen_asked = 0;
    }
    if (h->state == HOLD_IDLE && !g_notice && !g_notice_ticket && !g_orphans) return;
    if (!pub_on_main()) return;
    pub_orphans_retire();

    switch (h->state) {
    case HOLD_BEGUN:
        if (!h->units) { pub_refuse_local(); break; }
        if (!h->job) {
            /* SPEC 5.11 (owner, 2026-11-19): a joiner downloads in the lobby. It does not answer the host's map check
             * until the content is here, so the host's start waits for it (modlobby.h); the host sees its progress. */
            if (pub_is_joiner(h->hook)) {
                h->in_lobby = 1;
                pub_report(h, SH_MODLOBBY_CODE_PROGRESS);
            }
            pub_start_fetch();
        }
        break;
    case HOLD_FETCHING: {
        pub_job *job = h->job;
        if (h->in_lobby && pub_lobby_check(h)) break;
        if (!InterlockedCompareExchange(&job->done, 0, 0)) {
            pub_progress_step(h, job);
            break;
        }
        pub_progress_hide(h);
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
    for (size_t i = 0; i < PUB_SEEN_MAX; i++) pub_seen_clear(&g_seen[i]);
    pub_notice_clear(); g_notice_ticket = 0; g_notice_waited = 0;
    free(g_pass_notice); g_pass_notice = NULL;
    InterlockedExchange(&g_resuming, 0);
    g_reopen_asked = 0;
}
int sh_modpub_test_state(void) { return g_hold.state; }
const char *sh_modpub_test_notice(void) { return g_notice ? g_notice : ""; }
#endif

/* ================================================================== engine
 * Everything below binds the seam to DOOM build 13954591 (both renderers; the
 * offsets are equal on both, the addresses are not). Every engine read and
 * call is SEH-guarded; a failed bind installs nothing. The byte checks are in
 * modpub_sites.c (sh_modpub_check_sites). */
#ifndef SH_MODPUBLISHED_PURE
#include "signatures.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "engine_dialog.h"
#include "../fault_shield/recovery.h"   /* shield_note_refused_load: a map the editor cannot open is sound */
#include "modserve.h"
#include "modstore.h"
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
    const uint8_t *editor;   /* the editor object: in a session while +0x8 is set and its edit +0x204C8 is there */
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
    size_t slot_leave;                   /* the session's leave-lobby slot, as fail_fn calls it; 0 when unproven */
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

static int eng_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char **cause)
{ (void)c; return sh_modserve_row_served_for(row, subject, cause); }
static int eng_rescan(void *c) { (void)c; return sh_modserve_index_rescan(); }
static int eng_activate(void *c, const sh_modmanifest_list *rows) { (void)c; return sh_modmanifest_activate(rows); }
static int eng_can_ask(void *c) { (void)c; return sh_engine_dialog_can_ask(); }

/* 1 while the editor has a map in a session (as modpublish.c's editor token reads it), 0 not, -1 unknown. */
static int eng_editor_open(void *c)
{
    (void)c;
    if (!g_eng.editor) return -1;
    __try { return g_eng.editor[0x8] != 0 && *(void *const *)(g_eng.editor + 0x204C8) != NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

/* idMenuManager_Shell: the current screen +0x8, the next +0xC, the shell state +0x910 (SHELL_SCREEN, SHELL_STATE;
 * the same on both renderers). One token for a settled screen, -1 while it moves, 0 unknown. */
static long long eng_screen(void *c)
{
    void *manager = eng_shell_manager();
    (void)c;
    if (!manager) return 0;
    __try {
        int current = *(const volatile int *)((const uint8_t *)manager + 0x08);
        int next = *(const volatile int *)((const uint8_t *)manager + 0x0C);
        int state = *(const volatile int *)((const uint8_t *)manager + 0x910);
        if (next != current) return -1;
        return ((long long)(unsigned)(state + 1) << 32) | (unsigned)(current + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
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

/* A row's units into the content cache, composed (modcache.h): what cannot be had stands in. Worker thread. */
static int eng_fetch_for(void *c, const sh_modmanifest_row *row, sh_modpub_progress_fn progress, volatile int *cancel)
{
    modnet_io io;
    int rc;
    (void)c;
    memset(&io, 0, sizeof io);
    io.cancel = (volatile long *)cancel;
    io.progress = (modnet_progress_fn)progress;
    if (!sh_cache_row_known(row->build)) {
        pub_logf("MODS: the content of %s is not known to the content cache", pub_title(row));
        return 0;   /* nothing to fetch: the load stands in for it */
    }
    rc = sh_cache_prepare(row->build, &io);
    if (rc) pub_logf("MODS: the content of %s could not be prepared (%d)", pub_title(row), rc);
    return rc == MODNET_E_CANCELLED || rc == MODNET_E_NOMEM ? rc : 0;
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
 * takes it down). A rawmap hold has no owner: the shell's manager.
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

static int eng_scripted_maps_allowed(void *c) { (void)c; return sh_modui_scripted_maps_allowed(); }

/* The script files of the compositions serving the pinned rows (the content that came from elsewhere), from each
 * composition's mod.json. Asked only while scripts are turned off. */
static unsigned eng_served_scripts(void *c, const sh_modmanifest_list *rows)
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
        if (!rows->rows[i].pinned) continue;   /* the player's own local mod */
        if (!sh_modserve_row_build(&rows->rows[i], &ref) || !ref.folder || !ref.folder[0] ||
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

static void eng_refused_load(void *c) { (void)c; shield_note_refused_load(); }

/* The joiner leaves the lobby through the session's own leave call, the one
 * the read-error handler makes after its dialog (eng_bind checks it). */
static void eng_leave_lobby(void *c, int hook, void *owner)
{
    (void)c; (void)hook; (void)owner;
    if (!g_eng.slot_leave || !g_eng.session_slot) return;
    __try {
        void *session = *g_eng.session_slot;
        if (session) ((void (*)(void *))(*(void ***)session)[g_eng.slot_leave])(session);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: leaving the lobby faulted");
    }
}

static int eng_cold_read(void *manager, const char *id, const char *revision);
static void eng_scope_play(const char *why);

static int eng_cold_read_op(void *c, void *owner, const char *id, const char *revision)
{
    sh_process_heap_scope scope;
    int ran = 0;
    (void)c;
    if (!owner || !sh_process_heap_enter(&g_eng.heap, &scope)) return 0;
    __try { ran = eng_cold_read(owner, id, revision); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: the lobby map's fresh read faulted"); ran = 0; }
    (void)sh_process_heap_leave(&scope);
    return ran;
}

static void eng_scope_play_op(void *c, const char *why) { (void)c; eng_scope_play(why); }

static int eng_content_complete(void *c, const sh_modmanifest_row *row)
{
    int complete = 0;
    (void)c;
    return row && sh_cache_composed(row->build, &complete) && complete;
}
static int eng_content_causes(void *c, const sh_modmanifest_row *row, unsigned *mask)
{
    (void)c;
    return row && sh_cache_absent_causes(row->build, mask);
}
static char *eng_content_notice(void *c) { (void)c; return sh_cache_take_notice(); }
static int eng_content_pending(void *c) { (void)c; return sh_cache_notice_pending(); }
static void eng_content_drop(void *c) { (void)c; sh_cache_notice_drop(); }
/* The open map serves the units that arrived: the decls they change are parsed again in place (modserve.h). */
static int eng_content_refresh(void *c)
{
    sh_modserve_refresh rf;
    int state;
    (void)c;
    memset(&rf, 0, sizeof rf);
    state = sh_modserve_refresh_after_rescan(&rf);
    if (state < 0) return -1;
    if (!state) return 0;
    return rf.needs_map_reload > 0 || rf.needs_restart > 0 || rf.pending > 0 ? 2 : 1;
}

static void eng_lobby_report(void *c, unsigned code) { (void)c; sh_modlobby_send(code); }

#define ENG_LOBBY_TYPE 0x1E80   /* idLobby: its type, 0 party, 1 game, 2 dedicated (the host's map check reads it) */

/* Where the joiner's lobby stands: the session's active lobby (+0x348) must be a game lobby whose launch parameters
 * still name the held map (eng_host_matches reads the same). */
static int eng_lobby_state(void *c, const char *id, const char *revision)
{
    int type = -1;
    (void)c;
    __try {
        void *session = g_eng.session_slot ? *g_eng.session_slot : NULL, *lobby;
        lobby = session ? ((void *(*)(void *))(*(void ***)session)[g_eng.slot_lobby])(session) : NULL;
        if (lobby) type = *(const int *)((const uint8_t *)lobby + ENG_LOBBY_TYPE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { type = -1; }
    /* 1 the game lobby, 2 a dedicated one; the party lobby (0) or none: this machine left the match's lobby. */
    if (type != 1 && type != 2) return SH_MODPUB_LOBBY_GONE;
    return eng_host_matches(c, id, revision) ? SH_MODPUB_LOBBY_SAME : SH_MODPUB_LOBBY_OTHER_MAP;
}

void sh_modpublished_lobby_line(const char *text)
{
    if (!InterlockedCompareExchange(&g_eng.ready, 0, 0)) return;
    eng_progress(NULL, 0, NULL, text);
}

static const sh_modpub_ops ENG_OPS = {
    .on_main = eng_on_main, .row_state = eng_row_state, .rescan = eng_rescan, .activate = eng_activate,
    .can_ask = eng_can_ask, .ask = eng_ask, .poll = eng_poll, .release = eng_release, .screen = eng_screen, .editor_open = eng_editor_open,
    .worker_run = eng_worker_run,
    .fetch_for = eng_fetch_for, .metadata_copy = eng_metadata_copy, .metadata_free = eng_metadata_free,
    .metadata_ids = eng_metadata_ids, .hold_token = eng_hold_token, .owner_current = eng_owner_current,
    .host_matches = eng_host_matches, .hold_ui = eng_hold_ui, .drop_ui = eng_drop_ui, .resume = eng_resume,
    .log = eng_log, .print = eng_print, .launch_refused = eng_launch_refused,
    .scripted_maps_allowed = eng_scripted_maps_allowed, .served_scripts = eng_served_scripts, .progress = eng_progress,
    .refused_load = eng_refused_load, .leave_lobby = eng_leave_lobby,
    .cold_read = eng_cold_read_op, .scope_play = eng_scope_play_op, .content_complete = eng_content_complete, .content_causes = eng_content_causes,
    .content_notice = eng_content_notice, .content_pending = eng_content_pending, .content_drop = eng_content_drop,
    .content_refresh = eng_content_refresh, .lobby_report = eng_lobby_report, .lobby_state = eng_lobby_state
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
    const uint8_t *ready;
    sh_modpub_sites sites;
    sh_modpub_bound bound;
    const char *failed;
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
    shell = glb_resolve(base, "shell_ptr_slot", NULL);
    thread = glb_resolve(base, "main_thread_id", NULL);
    if (!shell || !thread) { backend_log("MODS: the shell or main-thread global did not resolve"); return 0; }
    /* the editor object (idSnapEditorLocal), for whether a parse no gate took was the editor's */
    g_eng.editor = (const uint8_t *)glb_resolve(base, "editor_singleton", NULL);
    if (!g_eng.editor) backend_log("MODS: the editor object did not resolve; every parse no gate takes counts as the editor's");
    ready = g_eng.site[ENG_READY];
    sites.on_snap = g_eng.site[ENG_ON_SNAP]; sites.browser = g_eng.site[ENG_BROWSER];
    sites.ready = ready; sites.lobby = g_eng.site[ENG_LOBBY]; sites.feed = g_eng.site[ENG_FEED];
    sites.lobby_launch = g_eng.site[ENG_LOBBY_LAUNCH]; sites.offline_launch = g_eng.site[ENG_OFFLINE_LAUNCH];
    sites.read = read; sites.limit = limit; sites.selection = selection; sites.parse = parse;
    sites.str_ctor = str_ctor; sites.str_dtor = str_dtor; sites.shell_slot = (const void *)shell;
    __try { failed = sh_modpub_check_sites(&sites, &bound); }
    __except (EXCEPTION_EXECUTE_HANDLER) { failed = "a site's bytes could not be read"; }
    if (failed) {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: the published-map gate check '%s' did not hold", failed);
        backend_log(line);
        return 0;
    }
    g_eng.metadata_copy = (eng_copy_fn)bound.metadata_copy;
    g_eng.snap_params = (const uint8_t *)bound.snap_params;
    g_eng.snap_source = (const char *)bound.snap_source;
    g_eng.browser_flag = (eng_object_fn)bound.browser_flag;
    g_eng.hide_progress = (eng_object_fn)bound.hide_progress;
    g_eng.session_slot = (void **)bound.session_slot;
    g_eng.slot_lobby = bound.slot_lobby;
    g_eng.slot_parameters = bound.slot_parameters;
    g_eng.system = (eng_get_fn)bound.system;
    g_eng.wait_text = (const char *)bound.wait_text;
    g_eng.show_wait = (eng_show_fn)bound.show_wait;
    g_eng.op_new = (eng_new_fn)bound.op_new;
    g_eng.vft_ok = (void *)bound.vft_ok;
    g_eng.vft_fail = (void *)bound.vft_fail;
    g_eng.fail_fn = (void *)bound.fail_fn;
    /* The read-error handler's leave is used alone, and only when every byte of its sequence holds; else a joiner
     * cannot be taken out. */
    g_eng.slot_leave = bound.slot_leave;
    if (!g_eng.slot_leave) backend_log("MODS: the lobby's leave call did not verify; a joiner without its mods stays in the lobby");
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
