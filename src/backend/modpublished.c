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
 * layer never handed the map to the engine. Its hold starts one step earlier,
 * PROBING: a worker job asks the store for every missing bundle, and only
 * when the store has them all does it go on to RAISE. Its resume opens the
 * rawmap again.
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
} pub_record;

static pub_record g_record;
static volatile LONG g_parse_sequence;
static volatile LONG g_resuming;       /* our own parse and original call are running */

/* The asset rule (modserve.h sh_modserve_row_served): a row is served when an
 * installed folder declares its id and provides every name it lists,
 * whatever its bundle hash. `subject` is the word the cause uses ("map",
 * "prefab"). */
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
    sh_modmanifest_list_free(&slot->rows);
    memset(slot, 0, sizeof(*slot));
    strcpy_s(slot->id, sizeof slot->id, id);
    strcpy_s(slot->revision, sizeof slot->revision, revision);
    if (rows && !pub_list_copy(&slot->rows, rows)) { memset(slot, 0, sizeof(*slot)); return; }
    slot->stamp = ++g_seen_stamp;
}

/* ------------------------------------------------------------ the fetch job */

typedef struct pub_job {
    struct pub_job *next;
    size_t count;
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    char (*ids)[SH_MODMANIFEST_ID_CAP];
    volatile int cancel;
    volatile LONG done;
    volatile LONG current;               /* index being fetched */
    volatile LONG64 progress_done, progress_total;
    int failed_index;                    /* -1 when every fetch returned 0 */
    int failed_code;
    int probe;                           /* ask the store (store_has) instead of fetching */
    int *found;                          /* probe: store_has per bundle */
} pub_job;

static pub_job *volatile g_running_job;  /* the job the worker is in (progress target) */
static pub_job *g_orphans;               /* abandoned jobs the worker still owns */

static void pub_job_free(pub_job *job)
{
    if (!job) return;
    free(job->hashes);
    free(job->ids);
    free(job->found);
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
            job->found[i] = g_ops.store_has ? g_ops.store_has(g_ops.context, job->hashes[i], &job->cancel) : -1;
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
        rc = g_ops.fetch ? g_ops.fetch(g_ops.context, job->hashes[i], pub_progress, &job->cancel) : -1;
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
    char *text;                          /* owned, completions and local play */
    size_t length;
    sh_modmanifest_list rows;
    size_t missing_count;
    size_t *missing;                     /* indexes into rows */
    unsigned long long missing_bytes;
    int ticket;
    int waited;
    pub_job *job;
    ULONGLONG progress_at;
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

void sh_modpublished_compose_prefab_question(char *out, size_t cap, const char *const *names, size_t count,
                                             unsigned long long bytes)
{
    pub_compose_question(out, cap, "prefab", names, count, bytes);
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
    if (g_hold.state == HOLD_IDLE) return;
    pub_logf("MODS: the held %s of '%s' was dropped: %s", pub_hook_name(g_hold.hook),
             g_hold.id[0] ? g_hold.id : "local map", why);
    if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, g_hold.hook, g_hold.owner);
    pub_hold_free();
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
    if (InterlockedCompareExchange(&g_resuming, 0, 0)) return;
    if (!pub_on_main()) { g_record.valid = 0; return; }
    pub_record_clear(&g_record);
    if (g_hold.state != HOLD_IDLE) pub_abandon("another map load started");
    memset(&rows, 0, sizeof rows);
    g_record.kind = kind;
    g_record.parsed = parsed;
    g_record.map = out_map;
    g_record.sequence = sequence;
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
    return hook == SH_MODPUB_ON_SNAP_PLAY || hook == SH_MODPUB_BROWSER_PLAY || hook == SH_MODPUB_CACHE_READY;
}

/* Build the hold from rows (copied) and, when given, the record's text (moved). */
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
    h->hook = hook; h->owner = owner; h->extra = extra;
    if (id) strcpy_s(h->id, sizeof h->id, id);
    if (revision) strcpy_s(h->revision, sizeof h->revision, revision);
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
    h->state = HOLD_RAISE;
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
    char item[PUB_ID_CAP] = "", rev[PUB_ID_CAP] = "";
    pub_record *record = &g_record;
    int begun;
    if (!g_ops_set || hook < 0 || hook >= SH_MODPUB_HOOKS || pub_is_local(hook)) return 1;
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
        begun = pub_hold_begin(hook, owner, NULL, NULL, id, revision, &seen->rows, NULL);
        if (begun <= 0) {
            if (begun < 0) {
                pub_log("MODS: the cached lobby map needs mods and could not be held; it is refused");
                pub_notice("This map needs mods that could not be prepared. The map was not loaded.");
            }
            return begun == 0;
        }
        pub_logf("MODS: lobby map '%s' (cached) needs %u missing mod(s); held for the player's answer",
                 id, (unsigned)g_hold.missing_count);
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        return 0;
    }

    /* The record must be this call's parse: valid, on the main thread, and the
     * same map object. Anything else is a launch this gate did not see parse. */
    if (!record->valid || record->map != map ||
        (pub_is_completion(hook) && record->kind == SH_MODPUB_PARSE_OTHER)) {
        if (record->valid && record->rows.count && record->map != map)
            pub_logf("MODS: %s received map %p, not the last parsed %p; not gated", pub_hook_name(hook),
                     map, record->map);
        return 1;
    }
    if (pub_is_completion(hook) && g_ops.metadata_ids)
        (void)g_ops.metadata_ids(g_ops.context, metadata, item, sizeof item, rev, sizeof rev);
    if (pub_is_completion(hook)) pub_seen_note(item, rev, &record->rows);
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
    begun = pub_hold_begin(hook, owner, metadata, extra, item, rev, &record->rows, record);
    pub_record_clear(record);
    if (begun < 0) {
        /* Never hand the engine a map whose mods are absent: it ends in the
         * palette miss that is fatal at START. Refuse instead. */
        if (g_ops.hold_ui) g_ops.hold_ui(g_ops.context, hook, owner);
        if (g_ops.drop_ui) g_ops.drop_ui(g_ops.context, hook, owner);
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

static void pub_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    char line[PUB_TEXT_CAP], cause[PUB_CAUSE_CAP];
    sh_modpub_resume r;
    int ok;

    if (job->failed_index >= 0) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "Could not download the mod %s. %s",
                    job->ids[job->failed_index], pub_not_done(h->hook));
        _snprintf_s(cause, sizeof cause, _TRUNCATE, "the fetch of %s returned %d", job->hashes[job->failed_index],
                    job->failed_code);
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
                 (unsigned)h->missing_count);
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
    pub_logf("MODS: the held %s of '%s' resumed with %u mod(s) installed", pub_hook_name(h->hook),
             h->id[0] ? h->id : "local map", (unsigned)h->missing_count);
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
    missing = pub_count_missing(rows);
    /* An uninstalled mod still in the recycle bin comes back first, exactly as
     * the load's own activation would bring it back. */
    if (missing && g_ops.restore && pub_on_main()) {
        const char **hashes = calloc(missing, sizeof(*hashes));
        int n = 0;
        if (hashes) {
            for (size_t i = 0; i < rows->count && (size_t)n < missing; i++)
                if (!pub_row_served(&rows->rows[i])) hashes[n++] = rows->rows[i].bundle;
            if (g_ops.restore(g_ops.context, hashes, n) > 0) missing = pub_count_missing(rows);
            free((void *)hashes);
        }
    }
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
    begun = pub_hold_begin(SH_MODPUB_RAWMAP_LOAD, NULL, NULL, NULL, name, "", rows, NULL);
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
    begun = pub_hold_begin(SH_MODPUB_PREFAB_PLACE, NULL, NULL, NULL, nm, "", rows, NULL);
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
        job->found = calloc(job->count, sizeof(*job->found));
    }
    if (!job || !job->hashes || !job->ids || !job->found) {
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

static void pub_probe_finish(void)
{
    pub_hold *h = &g_hold;
    pub_job *job = h->job;
    const sh_modmanifest_row **absent = calloc(h->missing_count ? h->missing_count : 1, sizeof(*absent));
    size_t lacking = 0, unknown = 0;
    if (!absent) {
        char line[PUB_TEXT_CAP];
        _snprintf_s(line, sizeof line, _TRUNCATE, "This %s needs mods that could not be prepared. %s", pub_thing(h->hook),
                    pub_not_done(h->hook));
        pub_drop(line, "no memory");
        return;
    }
    for (size_t i = 0; i < h->missing_count; i++) {
        int found = (job->failed_index >= 0 && (size_t)job->failed_index <= i) ? -1 : job->found[i];
        if (found == 0) absent[lacking++] = &h->rows.rows[h->missing[i]];
        else if (found < 0) unknown++;
    }
    if (lacking) {
        pub_rawmap_refuse(h->hook, h->id, absent, lacking, 0, "the mod store does not hold every missing mod");
    } else if (unknown) {
        for (size_t i = 0; i < h->missing_count; i++) absent[i] = &h->rows.rows[h->missing[i]];
        pub_rawmap_refuse(h->hook, h->id, absent, h->missing_count, 1, "the mod store could not be asked");
    } else {
        pub_logf("MODS: the mod store holds all %u missing mod(s) of %s '%s'", (unsigned)h->missing_count,
                 pub_is_prefab(h->hook) ? "prefab" : "rawmap", h->id);
        pub_job_free(job);
        h->job = NULL;
        h->waited = 0;
        h->state = HOLD_RAISE;
    }
    free((void *)absent);
}

void sh_modpublished_poll(void)
{
    pub_hold *h = &g_hold;
    if (!g_ops_set) return;
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
        if (!(names = pub_missing_names(h))) { pub_drop(NULL, "no memory for the question"); break; }
        pub_compose_question(text, sizeof text, pub_thing(h->hook), names, h->missing_count, h->missing_bytes);
        free((void *)names);
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
    if (ops) { g_ops = *ops; g_ops_set = 1; }
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

enum { ENG_ON_SNAP, ENG_BROWSER, ENG_READY, ENG_LOBBY, ENG_LOCAL, ENG_HOOKS };
static const char *const ENG_HOOK_NAMES[ENG_HOOKS] = {
    "PublishedOnSnapPlayDone", "PublishedBrowserPlayDone", "PublishedMapCacheReady",
    "CacheMapForLobby", "LaunchLocalSnapMapForOfflinePlay" };

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

static void *eng_manager_of(int hook, void *owner)
{
    if (!owner) return NULL;
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
static int eng_fetch(void *c, const char *hash, sh_modpub_progress_fn progress, volatile int *cancel)
{
    modnet_io io;
    modsync_stats st;
    (void)c;
    memset(&io, 0, sizeof io);
    memset(&st, 0, sizeof st);
    io.cancel = (volatile long *)cancel;
    io.progress = (modnet_progress_fn)progress;
    if (modsync_fetch_bundle(hash, &io, &st)) return 0;
    pub_logf("fetch %.16s failed: %s", hash, st.cause[0] ? st.cause : modnet_code_text(st.code));
    return st.code ? st.code : -1;
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
    if (!r || !sh_process_heap_enter(&g_eng.heap, &scope)) return 0;
    __try {
        if (r->hook == SH_MODPUB_CACHE_LOBBY) {
            ran = eng_cold_read(r->owner, r->id, r->revision);
        } else if (r->text && (map = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ENG_MAP_SIZE)) != NULL) {
            g_eng.map_ctor(map); constructed = 1;
            if (g_eng.parse(r->text, map)) {
                if (r->hook == SH_MODPUB_LOCAL_OFFLINE)
                    ((eng_local_fn)g_eng.original[ENG_LOCAL])(r->owner, r->extra, map);
                else
                    ((eng_completion_fn)g_eng.original[r->hook == SH_MODPUB_ON_SNAP_PLAY ? ENG_ON_SNAP :
                        r->hook == SH_MODPUB_BROWSER_PLAY ? ENG_BROWSER : ENG_READY])(
                        r->owner, NULL, r->metadata, map);
                ran = 1;
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

static int eng_restore(void *c, const char *const *hashes, int n)
{
    (void)c;
    return sh_modserve_restore(hashes, n, NULL, 0, SH_MODSERVE_RESTORE_LOAD);
}

static void eng_print(void *c, const char *line) { (void)c; if (line) sh_printf("%s\n", line); }

static const sh_modpub_ops ENG_OPS = {
    NULL, eng_on_main, eng_row_state, eng_rescan, eng_activate, eng_can_ask, eng_ask, eng_poll,
    eng_release, eng_worker_run, eng_fetch, eng_metadata_copy, eng_metadata_free, eng_metadata_ids,
    eng_hold_token, eng_owner_current, eng_host_matches, eng_hold_ui, eng_drop_ui, eng_resume, eng_log,
    eng_store_has, eng_restore, eng_print
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
    if (eng_decide(SH_MODPUB_LOCAL_OFFLINE, manager, NULL, map, model, NULL, NULL))
        ((eng_local_fn)g_eng.original[ENG_LOCAL])(manager, model, map);
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
                                 (void *)eng_lobby_detour, (void *)eng_local_detour };
    if (InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    if (!results || !module_base || !eng_bind(results, count, module_base)) {
        g_read_offline_return = g_read_online_return = NULL;
        backend_log("MODS: published-map gates are not installed on this build; published maps load natively");
        return 0;
    }
    g_ops = ENG_OPS; g_ops_set = 1;
    /* OnSnapPlay: 11 bytes of pushes and sub, then the 7-byte security-cookie
     * load, rewritten. CacheReady: the stack-probe call at +7. The others are
     * whole, position-independent prologues. */
    g_eng.original[ENG_ON_SNAP] = hook_prepare_rip_load((void *)g_eng.site[ENG_ON_SNAP], detours[ENG_ON_SNAP], 18, 11);
    g_eng.original[ENG_BROWSER] = hook_prepare((void *)g_eng.site[ENG_BROWSER], detours[ENG_BROWSER], 20);
    g_eng.original[ENG_READY] = hook_prepare_relative_call((void *)g_eng.site[ENG_READY], detours[ENG_READY], 15, 7);
    g_eng.original[ENG_LOBBY] = hook_prepare((void *)g_eng.site[ENG_LOBBY], detours[ENG_LOBBY], 20);
    g_eng.original[ENG_LOCAL] = hook_prepare((void *)g_eng.site[ENG_LOCAL], detours[ENG_LOCAL], 22);
    for (size_t i = 0; i < ENG_HOOKS; i++) if (!g_eng.original[i]) goto failed;
    for (size_t i = 0; i < ENG_HOOKS; i++) if (hook_commit(g_eng.original[i]) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_eng.ready, 1);
    backend_log("MODS: published-map gates installed (published play, browser play, lobby join, cached lobby "
                "join, My Maps play); maps whose mods are installed load natively");
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
