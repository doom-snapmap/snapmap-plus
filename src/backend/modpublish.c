/* modpublish.c -- see modpublish.h.
 *
 * One publish at a time. Its life (SPEC-distribution 5.3):
 *
 *   PublishStart (held)  the editor's Publish: the engine's PublishStart is not called yet. A job on the modnet worker
 *                 computes the units of the map's local rows (modpin.h: each placed object's unit from its folder's
 *                 build), asks the store which of the map's units it holds, and sizes what it lacks (modclaim.h). The
 *                 editor shows its pacifier with a line of progress meanwhile.
 *   ASKING        bytes to upload: "Publishing uploads 212 MB of mod content that the mod store does not have yet.
 *                 Publish?" (S7). No: the publish never starts.
 *   UPLOADING     the missing units go up, with "Uploading mod content: 84 of 212 MB." on the pacifier.
 *   (resume)      the engine's PublishStart runs; its save pins a copy of the map's rows (sh_modmanifest_set_pin),
 *                 which the cloud save's read of the slot gets (sh_modpublish_cloud_text), so the text the engine
 *                 publishes names units only. The open map and its slot keep their own rows.
 *   PublishAfterSave  the text bound (10 MB) and an unreadable mod list are refused here, as before.
 *   PublishFeedPost   (the item id known) the save's units are claimed for the item on the worker (a republish
 *                 replaces its set); a claim the store could not take is queued and retried at every start.
 *
 * A busy store is waited out (Retry-After) for two minutes; after that the publish goes on and what could not be sent
 * is queued (N27). A unit neither here nor in the store (S8) or removed by moderation is said in one notice after the
 * publish, which proceeds. An object whose folder is not built cannot be published: the publish does not start, and the
 * notice says what to fix.
 *
 * Background jobs (claims, releases, the launch step) run one at a time on the worker, in order, whenever it is free.
 *
 * Delete checks: the delete completion queues one check per deleted map with linked, claimed items; the next tick
 * starts the engine's published-items query, whose callbacks post an answer (any thread) that a later tick settles. An
 * unanswered check expires and keeps its claims.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modpublish.h"
#include "modmanifest.h"
#include "modnet.h"
#include "modclaim.h"
#include "modpin.h"
#include "modauthor.h"
#include "modcache.h"
#include "modstore.h"
#include "modserve.h"
#include "mp_json.h"
#include "map_carrier.h"
#include "config.h"
#include "plain_reason.h"
#include "../common/text.h"

#define MP_GDM_ID          0x29u   /* GDM_CONFIRM_VIDEO_CHANGES: the Yes/No shape modpublished.c uses */
#define MP_BUTTONS_YES_NO  6u
#define MP_RAISE_TICKS     600     /* about ten seconds at 60 Hz */
#define MP_SAVE_FRESH_MS   5000ull /* the Save state saves and calls back in one frame */
#define MP_PIN_ARM_MS      30000ull /* PublishStart to its save is about 60 frames */
#define MP_CONTEXT_MS      (15ull * 60ull * 1000ull)
#define MP_STORE_WAIT_MS   120000ull   /* a busy store is waited out this long (N27) */
#define MP_TEXT_CAP        256     /* the game's dialog body (engine_dialog.c: 255 bytes of text) */
/* A saved-map or item id the links file keeps: the engine's own ids, which
 * are short plain tokens; anything longer is not one (mp_link_id). */
#define MP_ID_CAP          160

static void mp_link_record(const char *item, const char *cloud, const char *local);
static void mp_link_forget(const char *item);

enum { MP_DIALOG_PENDING = 0, MP_DIALOG_ACCEPTED = 1 };
/* HOLD_RESUME: a publish held at FINISH is ready; the EditorFrame detour closes the screens and starts it. */
enum { HOLD_IDLE = 0, HOLD_PREPARING, HOLD_RAISE, HOLD_ASKING, HOLD_UPLOADING, HOLD_RESUME };
#define MP_RESUME_TICKS    120     /* no editor frame in two seconds: the tick resumes it itself */
#define MP_QUESTION_TITLE  "Mod content to upload"
/* CTX_KEEP: the map names mods but no unit of them can be claimed: the item's claim stays as it is (R-LB1 F9). */
enum { CTX_NONE = 0, CTX_ROWS, CTX_NO_ROWS, CTX_UNKNOWN, CTX_KEEP };
enum { JOB_PREPARE = 1, JOB_UPLOAD, JOB_CLAIM, JOB_RELEASE, JOB_LAUNCH, JOB_FETCH };

static sh_modpublish_ops g_ops;
static int g_ops_set;
static volatile LONG g_resuming;
/* The rows a save would write now (modmanifest.h); tests give their own. */
static int (*g_rows_for)(const char *const *names, size_t count, sh_modmanifest_list *out) = sh_modmanifest_rows_for;

/* ------------------------------------------------------------ logging */

static void mp_log(const char *line)
{
    if (g_ops_set && g_ops.log) g_ops.log(g_ops.context, line);
}

static void mp_logf(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    mp_log(line ? line : "MODS: publish: (out of memory)");
    free(line);
}

static int mp_on_main(void)
{
    return g_ops_set && g_ops.on_main && g_ops.on_main(g_ops.context);
}

/* ------------------------------------------------------------ text */

void sh_modpublish_size_text(char *out, size_t cap, unsigned long long bytes)
{
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (!out || !cap) return;
    if (bytes < 1024ull * 100ull) _snprintf_s(out, cap, _TRUNCATE, "%llu byte%s", bytes, bytes == 1 ? "" : "s");
    else _snprintf_s(out, cap, _TRUNCATE, "%.0f MB", mb < 1.0 ? 1.0 : mb);
}

void sh_modpublish_compose_question(char *out, size_t cap, unsigned long long bytes)
{
    char size[32];
    if (!out || !cap) return;
    if (cap > MP_TEXT_CAP) cap = MP_TEXT_CAP;
    sh_modpublish_size_text(size, sizeof size, bytes);
    _snprintf_s(out, cap, _TRUNCATE,
                "Publishing uploads %s of mod content that the mod store does not have yet. Publish?", size);
}

void sh_modpublish_progress_text(char *out, size_t cap, unsigned long long done, unsigned long long total)
{
    char a[32], b[32];
    if (!out || !cap) return;
    if (!total) { _snprintf_s(out, cap, _TRUNCATE, "Preparing this map's mod content."); return; }
    if (done > total) done = total;
    sh_modpublish_size_text(b, sizeof b, total);
    if (strlen(b) > 3 && !strcmp(b + strlen(b) - 3, " MB")) {
        /* "84 of 212 MB": the part sent in whole MB of the same unit, rounded down until it is all sent */
        unsigned long long mb = done / (1024ull * 1024ull);
        _snprintf_s(a, sizeof a, _TRUNCATE, "%llu", done == total ? (unsigned long long)atoll(b) : mb);
        _snprintf_s(out, cap, _TRUNCATE, "Uploading mod content: %s of %s.", a, b);
        return;
    }
    sh_modpublish_size_text(a, sizeof a, done);
    _snprintf_s(out, cap, _TRUNCATE, "Uploading mod content: %s of %s.", a, b);
}

/* ------------------------------------------------------------ the save record */

typedef struct mp_save {
    int valid;
    unsigned long sequence;
    size_t length;
    ULONGLONG at;
    size_t count;                              /* rows */
    size_t local;                              /* rows not pinned: a publish that did not pin them */
    unsigned uses;                             /* what the map uses of Snapmap+ itself (modmanifest.h) */
    char (*units)[MODNET_HASH_HEX + 1];        /* the units the pinned rows name, each once */
    size_t nunits;
    char (*salts)[SH_MODMANIFEST_SALT_CAP];    /* the pinned rows' namespaces, each once */
    size_t nsalts;
    int carrier;                               /* the text carries the snapmap-plus member */
    int unreadable;                            /* rows the reader refused, or the read failed: never published */
    char *unreadable_name;                     /* the first refused row's mod name (malloc'd), NULL when it has none */
} mp_save;

static mp_save g_save;
static volatile LONG g_save_sequence;

static void mp_save_clear(mp_save *s)
{
    free(s->units);
    free(s->salts);
    free(s->unreadable_name);
    memset(s, 0, sizeof(*s));
}

/* The name a refused row gives its mod, read from the member itself: each
 * row of the "mods" part is read alone, and the first the manifest reader
 * refuses names it, whole (malloc'd; NULL when it has no name either). */
static char *mp_refused_name(const char *text, size_t length)
{
    size_t from = 0, to = 0;
    const char *cursor, *elem;
    size_t elem_len;
    if (sh_carrier_part(text, length, "mods", &from, &to) != 1) return NULL;
    cursor = text + from;
    while (sh_carrier_array_next(&cursor, text + to, &elem, &elem_len) == 1) {
        static const char head[] = "{\"" SH_CARRIER_KEY "\":{\"format\":2,\"mods\":[";
        sh_modmanifest_list one;
        unsigned refused = 0;
        char *alone = (char *)malloc(sizeof head + elem_len + 4);
        const char *name = NULL;
        size_t name_len = 0;
        if (!alone) return NULL;
        memcpy(alone, head, sizeof head - 1);
        memcpy(alone + sizeof head - 1, elem, elem_len);
        memcpy(alone + sizeof head - 1 + elem_len, "]}}", 4);
        memset(&one, 0, sizeof one);
        (void)sh_modmanifest_read(alone, strlen(alone), &one, &refused);
        sh_modmanifest_list_free(&one);
        free(alone);
        if (!refused) continue;
        if (sh_carrier_object_string(elem, elem + elem_len, "name", &name, &name_len) == 1 && name_len &&
            !memchr(name, '\\', name_len))
            return sh_textn(name, name_len);
        return NULL;
    }
    return NULL;
}

/* Add a unit to a set, once. 0 when memory ran out. */
static int mp_units_add(char (**units)[MODNET_HASH_HEX + 1], size_t *n, size_t *cap, const char *hash)
{
    size_t i;
    for (i = 0; i < *n; i++) if (!strcmp((*units)[i], hash)) return 1;
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        char (*g)[MODNET_HASH_HEX + 1] = realloc(*units, c * sizeof *g);
        if (!g) return 0;
        *units = g;
        *cap = c;
    }
    strcpy_s((*units)[(*n)++], MODNET_HASH_HEX + 1, hash);
    return 1;
}

/* The published copy's rows (R-LB1 F6, the owner's ruling): a publish's save pins a copy of its rows and the slot
 * keeps the map's own, so the author's map follows their folders, published or not. The cloud save reads the slot back
 * to upload it (ReadLocalSavedMapText in the cloud-save request, both renderers) and gets the text with these rows
 * instead (sh_modpublish_cloud_text); the bound and the claim count them. Armed from the save to the feed post (or the
 * next publish, or the context's end), with no timer: a republish fetches the existing cloud save over the network
 * before it reads the slot. `sequence` is the save it was armed for; `served` says the cloud save's read got it. */
static struct {
    int armed, served;
    unsigned long sequence;
    ULONGLONG at;
    sh_modmanifest_list rows;
    char local[MP_ID_CAP];
} g_upload;
static SRWLOCK g_upload_lock = SRWLOCK_INIT;   /* the cloud save's read may come from a job thread */
/* The published copy's declChecksum (the engine's hash of the pinned text), from its text read to its metadata read in
 * the same cloud-save request: the uploaded metadata names the text it is uploaded with. A read that is not served
 * forgets it. Under g_upload_lock. */
static struct { int set; unsigned sum; char local[MP_ID_CAP]; } g_cloud_sum;

static void mp_upload_clear(void)
{
    AcquireSRWLockExclusive(&g_upload_lock);
    sh_modmanifest_list_free(&g_upload.rows);
    memset(&g_upload, 0, sizeof g_upload);
    memset(&g_cloud_sum, 0, sizeof g_cloud_sum);
    ReleaseSRWLockExclusive(&g_upload_lock);
}

/* Under the lock (either mode). */
static int mp_upload_live_locked(void)
{
    return g_upload.armed;
}

static int mp_upload_live(void)
{
    int live;
    AcquireSRWLockShared(&g_upload_lock);
    live = mp_upload_live_locked();
    ReleaseSRWLockShared(&g_upload_lock);
    return live;
}

/* `text` with the published copy's rows in its snapmap-plus member (HeapFree), or NULL. Under the lock. */
static char *mp_upload_text_locked(const char *text, size_t length, size_t *out_length)
{
    char err[192];
    char *out = sh_modmanifest_write(text, length, &g_upload.rows, out_length, err, sizeof err);
    if (!out) mp_logf("MODS: publish: the published copy's rows could not be written (%s)", err);
    return out;
}

char *sh_modpublish_cloud_text(const char *local_id, const char *text, size_t length, size_t *out_length)
{
    char *out = NULL;
    size_t n = 0;
    if (out_length) *out_length = 0;
    if (!g_ops_set || !text || !length) return NULL;
    AcquireSRWLockExclusive(&g_upload_lock);
    if (mp_upload_live_locked() && (!g_upload.local[0] || (local_id && !_stricmp(local_id, g_upload.local)))) {
        out = mp_upload_text_locked(text, length, out_length);
        n = g_upload.rows.count;
        if (out) g_upload.served = 1;
    }
    if (!out) g_cloud_sum.set = 0;   /* this request uploads the slot as it is, with its own checksum */
    ReleaseSRWLockExclusive(&g_upload_lock);
    if (out) mp_logf("MODS: publish: the published copy carries %zu pinned row(s); the map's slot keeps its own rows", n);
    return out;
}

void sh_modpublish_cloud_sum_note(const char *local_id, unsigned sum)
{
    AcquireSRWLockExclusive(&g_upload_lock);
    g_cloud_sum.set = local_id && strlen(local_id) < sizeof g_cloud_sum.local;
    g_cloud_sum.sum = sum;
    if (g_cloud_sum.set) strcpy_s(g_cloud_sum.local, sizeof g_cloud_sum.local, local_id);
    ReleaseSRWLockExclusive(&g_upload_lock);
}

int sh_modpublish_cloud_sum_take(const char *local_id, unsigned *sum)
{
    int got = 0;
    AcquireSRWLockExclusive(&g_upload_lock);
    if (g_cloud_sum.set && local_id && !_stricmp(local_id, g_cloud_sum.local)) {
        if (sum) *sum = g_cloud_sum.sum;
        got = 1;
        g_cloud_sum.set = 0;
    }
    ReleaseSRWLockExclusive(&g_upload_lock);
    return got;
}

static void mp_save_seen_text(const char *text, size_t length, unsigned long sequence);

void sh_modpublish_save_seen(const char *text, size_t length)
{
    unsigned long sequence = (unsigned long)InterlockedIncrement(&g_save_sequence);
    char *upload = NULL;
    size_t upload_length = 0;
    if (!g_ops_set || InterlockedCompareExchange(&g_resuming, 0, 0)) return;
    if (!mp_on_main()) { g_save.valid = 0; return; }
    /* a publish's save: what is published is the slot's text with the pinned rows */
    if (text && length) {
        /* only the save whose pin step armed it: a later plain save is the slot's own */
        AcquireSRWLockShared(&g_upload_lock);
        if (mp_upload_live_locked() && g_upload.sequence == sequence)
            upload = mp_upload_text_locked(text, length, &upload_length);
        ReleaseSRWLockShared(&g_upload_lock);
    }
    if (upload) { text = upload; length = upload_length; }
    mp_save_seen_text(text, length, sequence);
    if (upload) HeapFree(GetProcessHeap(), 0, upload);
}

static void mp_save_seen_text(const char *text, size_t length, unsigned long sequence)
{
    sh_modmanifest_list rows;
    unsigned rejected = 0;
    size_t cap = 0;
    mp_save_clear(&g_save);
    g_save.sequence = sequence;
    g_save.length = length;
    g_save.at = GetTickCount64();
    g_save.valid = 1;
    if (!text || !length) return;
    {
        sh_carrier_view view;
        __try {
            g_save.carrier = sh_carrier_view_read(text, length, &view) && view.state != SH_CARRIER_ABSENT;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_save.carrier = 0;
            g_save.unreadable = 1;
        }
    }
    if (!g_save.carrier) return;
    memset(&rows, 0, sizeof rows);
    __try {
        (void)sh_modmanifest_read(text, length, &rows, &rejected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(&rows, 0, sizeof rows);   /* left as it is: its state is unknown */
        g_save.unreadable = 1;
        mp_log("MODS: publish: the saved map's mod list could not be read; it will not be published");
        return;
    }
    for (size_t i = 0; i < rows.count; i++) {
        const sh_modmanifest_row *r = &rows.rows[i];
        size_t k2;
        int dup = 0;
        if (!r->pinned) { g_save.local++; continue; }
        for (k2 = 0; k2 < g_save.nsalts && !dup; k2++) dup = !strcmp(g_save.salts[k2], r->salt);
        if (!dup) {
            char (*g)[SH_MODMANIFEST_SALT_CAP] = realloc(g_save.salts, (g_save.nsalts + 1) * sizeof *g);
            if (g) { g_save.salts = g; strcpy_s(g_save.salts[g_save.nsalts++], SH_MODMANIFEST_SALT_CAP, r->salt); }
        }
        for (size_t k = 0; k < r->unit_count; k++)
            if (!mp_units_add(&g_save.units, &g_save.nunits, &cap, r->units[k])) g_save.unreadable = 1;
    }
    g_save.count = rows.count;
    g_save.uses = rows.uses;
    if (rejected) {
        /* A row the reader refused names a mod the publish cannot know:
         * publishing would leave players without it, so the map is not
         * published until it is fixed. */
        g_save.unreadable = 1;
        g_save.unreadable_name = mp_refused_name(text, length);
        mp_logf("MODS: publish: %u row(s) of the saved map's mod list did not read (first: %s); it will not be published",
                rejected, g_save.unreadable_name ? g_save.unreadable_name : "a mod with no name");
    }
    sh_modmanifest_list_free(&rows);
}

/* ------------------------------------------------------------ search tag */

/* The tag must be in the local save's metadata: the cloud save that
 * TryCloudSave starts re-reads it from the profile, so a tag added to the
 * editor's model after the save never reaches the cloud save or the item.
 * PublishStart arms; the save observer then sees the rows; the local save's
 * WriteLocalSavedMapText takes the tag into the model it writes. */
#define MP_TAG_ARM_MS 10000ull   /* PublishStart to the save is about 60 frames */

static struct {
    int armed;
    ULONGLONG at;
    char local[MP_ID_CAP];
    unsigned long tagged_sequence;   /* the save whose metadata got (or had) the tag */
} g_tag;

void sh_modpublish_publish_started(const char *local_id)
{
    if (!g_ops_set || !mp_on_main()) return;
    g_tag.armed = 1;
    g_tag.at = GetTickCount64();
    g_tag.local[0] = 0;
    if (local_id && strlen(local_id) < sizeof g_tag.local) strcpy_s(g_tag.local, sizeof g_tag.local, local_id);
}

int sh_modpublish_save_tag(const char *local_id, void *model)
{
    int tagged, armed = 0;
    if (!g_ops_set || !mp_on_main()) return 0;
    /* the published copy is the cloud save's read of this slot */
    AcquireSRWLockExclusive(&g_upload_lock);
    if (mp_upload_live_locked() && !g_upload.local[0] && local_id && strlen(local_id) < sizeof g_upload.local)
        strcpy_s(g_upload.local, sizeof g_upload.local, local_id);
    ReleaseSRWLockExclusive(&g_upload_lock);
    if (g_tag.armed) {
        if (GetTickCount64() - g_tag.at > MP_TAG_ARM_MS) g_tag.armed = 0;
        else if (!g_tag.local[0] || (local_id && !strcmp(local_id, g_tag.local))) { armed = 1; g_tag.armed = 0; }
    }
    /* The observer reported this save's text just before its write. Every
     * local save makes the tag match it: present with rows or with a feature of
     * Snapmap+ the map uses (a resized module, its own navigation), absent
     * without. */
    if (!g_save.valid || GetTickCount64() - g_save.at > MP_SAVE_FRESH_MS || !model) return 0;
    if (!g_save.count && !g_save.uses) {
        if (!g_ops.remove_tag) return 0;
        tagged = g_ops.remove_tag(g_ops.context, model, SH_MODPUBLISH_TAG);
        if (tagged > 0)
            mp_log("MODS: save: search tag '" SH_MODPUBLISH_TAG "' removed from the saved map's tags (the save carries "
                   "no mod rows)");
        else if (tagged < 0)
            mp_log("MODS: save: the search tag '" SH_MODPUBLISH_TAG "' could not be removed from the saved map's tags");
        return tagged > 0 ? 2 : tagged;
    }
    if (!g_ops.add_tag) return 0;
    tagged = g_ops.add_tag(g_ops.context, model, SH_MODPUBLISH_TAG);
    if (tagged >= 0) g_tag.tagged_sequence = g_save.sequence;
    if (tagged > 0)
        mp_logf("MODS: %s: search tag '" SH_MODPUBLISH_TAG "' written into the saved map's tags (the map needs "
                "mods, so players without Snapmap+ cannot play it)", armed ? "publish" : "save");
    else if (tagged < 0)
        mp_logf("MODS: %s: the search tag '" SH_MODPUBLISH_TAG "' could not be written%s", armed ? "publish" : "save",
                armed ? "; the map publishes without it" : "");
    return tagged;
}

/* ------------------------------------------------------------ jobs */

typedef struct mp_job {
    struct mp_job *next;
    int kind;
    char *ref;                                 /* claim, release: the published item */
    char (*units)[MODNET_HASH_HEX + 1];        /* claim: the save's units; prepare: filled in */
    size_t nunits;
    sh_modmanifest_list rows;                  /* prepare: the rows the save will write, pinned by the job */
    modclaim_plan plan;                        /* prepare: what the store holds and lacks; upload: what to send */
    volatile LONG done;
    int code;                                  /* MODNET_OK or the failure */
    char *why;                                 /* the failure, for the log (malloc'd) */
    char *unbuilt;                             /* prepare: the first row whose folder is not built (its name) */
    int made_key;                              /* the key was made by this job (the first publish) */
    char *after;                               /* prepare: S8 or the removed sentence, said after the publish */
    unsigned long long done_bytes, total_bytes;   /* upload progress, written by the worker */
    char (*salts)[SH_MODMANIFEST_SALT_CAP];    /* claim: the namespaces its units serve (the Mods tab's record) */
    size_t nsalts;
    char (*removed)[MODNET_HASH_HEX + 1];      /* claim: units moderation removed */
    size_t nremoved;
    modclaim_launch_stats launch;
} mp_job;

static mp_job *volatile g_running;     /* the job on the worker, or NULL */
static mp_job *g_queue;                /* background jobs not started yet, in order */
static mp_job *g_orphans;              /* abandoned holds' jobs the worker still owns */
static SRWLOCK g_queue_lock = SRWLOCK_INIT;

static void mp_job_free(mp_job *job)
{
    if (!job) return;
    free(job->ref);
    free(job->units);
    free(job->why);
    free(job->unbuilt);
    free(job->after);
    free(job->removed);
    free(job->salts);
    sh_modmanifest_list_free(&job->rows);
    modclaim_plan_free(&job->plan);
    free(job);
}

static mp_job *mp_job_new(int kind, const char *ref, const char (*units)[MODNET_HASH_HEX + 1], size_t n)
{
    mp_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->kind = kind;
    if (!(job->ref = sh_text(ref ? ref : ""))) { free(job); return NULL; }
    if (n) {
        if (!(job->units = calloc(n, sizeof *job->units))) { free(job->ref); free(job); return NULL; }
        memcpy(job->units, units, n * sizeof *job->units);
        job->nunits = n;
    }
    return job;
}

/* Upload progress on the worker: the bytes sent of the plan's total, across its units. */
static void mp_progress(void *context, unsigned long long done, unsigned long long total)
{
    mp_job *job = (mp_job *)context;
    (void)total;
    if (job) InterlockedExchange64((volatile LONG64 *)&job->done_bytes, (LONG64)done);
}

/* The asked paths of this computer's claims, as the unit walk seeds them (3.8). */
typedef struct mp_asked { sh_unit_asked *v; size_t n, cap; } mp_asked;
static void mp_asked_one(void *context, const char *unit, const char *root, const char *path)
{
    mp_asked *a = (mp_asked *)context;
    char *request = NULL, *cause = NULL;
    (void)unit; (void)root;
    /* an author path names the engine's request under any namespace: the walk reads mods/<x>/ as the build's own */
    if (!sh_ma_request("asked000", path, &request, &cause) || !request) { free(cause); return; }
    free(cause);
    if (a->n == a->cap) {
        size_t c = a->cap ? a->cap * 2 : 16;
        sh_unit_asked *g = (sh_unit_asked *)realloc(a->v, c * sizeof *g);
        if (!g) { free(request); return; }
        a->v = g;
        a->cap = c;
    }
    a->v[a->n].request = request;
    a->v[a->n].by = NULL;
    a->n++;
}

/* The objects of the plan players will not get, named as the load notices name them: "Players will see this map
 * without the Marauder from Eternal Demons: its content is not on this computer or in the mod store." (S8), or "...,
 * which was removed from the mod store." (moderation). Absent units first: they are the author's to fix. */
static char *mp_missing_sentence(const mp_job *job)
{
    sh_cache_absent *a;
    char **names;
    size_t n = 0, i, r, k;
    int removed_only = !job->plan.absent;
    char *subject, *out = NULL;
    if (!(a = (sh_cache_absent *)calloc(job->plan.n + 1, sizeof *a)) || !(names = (char **)calloc(job->plan.n + 1, sizeof *names))) {
        free(a);
        return NULL;
    }
    for (i = 0; i < job->plan.n; i++) {
        int want = removed_only ? job->plan.state[i] == MODCLAIM_REMOVED
                                : job->plan.state[i] == MODCLAIM_ABSENT || job->plan.state[i] == MODCLAIM_REMOVED;
        if (!want) continue;
        for (r = 0; r < job->rows.count && !a[n].object; r++)
            for (k = 0; k < job->rows.rows[r].unit_count; k++)
                if (!strcmp(job->rows.rows[r].units[k], job->plan.units[i])) {
                    names[n] = sh_cache_object_name(job->rows.rows[r].roots[k]);
                    a[n].object = names[n] ? names[n] : "object";
                    a[n].mod = sh_modmanifest_row_name(&job->rows.rows[r])[0] ? sh_modmanifest_row_name(&job->rows.rows[r]) : "a mod";
                    break;
                }
        if (a[n].object) n++;
    }
    subject = n ? sh_cache_objects_subject(a, n) : NULL;
    if (subject) {
        if (removed_only) out = sh_textf("Players will see this map without %s, which %s removed from the mod store.", subject,
                                         n > 1 ? "were" : "was");
        else out = sh_textf("Players will see this map without %s: %s content is not on this computer or in the mod store.",
                            subject, n > 1 ? "their" : "its");
    }
    free(subject);
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    free(a);
    return out;
}

/* Worker: the units of the save to come, and what the store lacks of them. */
static int mp_prepare(mp_job *job, const modnet_io *io)
{
    char (*units)[MODNET_HASH_HEX + 1] = NULL;
    size_t n = 0, cap = 0, i, k;
    int code;
    modclaim_set_budget(MP_STORE_WAIT_MS);
    if (!modclaim_signer(NULL, &job->made_key, &job->why)) return MODNET_E_KEY;
    {
        mp_asked asked;
        memset(&asked, 0, sizeof asked);
        if (modclaim_asked(mp_asked_one, &asked, io, NULL) == MODNET_OK && asked.n) sh_modpin_set_asked(asked.v, asked.n);
        for (i = 0; i < asked.n; i++) free((void *)asked.v[i].request);
        free(asked.v);
    }
    for (i = 0; i < job->rows.count; i++) {
        sh_modmanifest_row *row = &job->rows.rows[i];
        sh_modpin_row_result r;
        memset(&r, 0, sizeof r);
        if (!row->pinned && _stricmp(row->salt, SH_MODMANIFEST_PRODUCT)) {
            int state = sh_modpin_row(row, NULL, 0, NULL, &r);
            if (state == SH_MODPIN_ABSENT && !job->unbuilt)
                job->unbuilt = sh_text(sh_modmanifest_row_name(row)[0] ? sh_modmanifest_row_name(row) : "a mod");
            free(r.why);
        }
        for (k = 0; row->pinned && k < row->unit_count; k++)
            if (!mp_units_add(&units, &n, &cap, row->units[k])) { free(units); return MODNET_E_NOMEM; }
    }
    if (job->unbuilt) { free(units); return MODNET_OK; }
    {
        const char **list = (const char **)calloc(n ? n : 1, sizeof *list);
        if (!list) { free(units); return MODNET_E_NOMEM; }
        for (i = 0; i < n; i++) list[i] = units[i];
        code = modclaim_plan_units(list, n, &job->plan, io, &job->why);
        free((void *)list);
    }
    free(units);
    if (code == MODNET_OK && (job->plan.absent || job->plan.removed)) job->after = mp_missing_sentence(job);
    return code;
}

/* Worker thread. Touches only the job. */
static int mp_job_run(void *context, const modnet_io *io)
{
    mp_job *job = context;
    modnet_io mine;
    const char **list = NULL;
    size_t i;
    memset(&mine, 0, sizeof mine);
    if (io) mine = *io;
    switch (job->kind) {
    case JOB_PREPARE:
        job->code = mp_prepare(job, io);
        break;
    case JOB_UPLOAD:
        modclaim_set_budget(MP_STORE_WAIT_MS);
        mine.progress = mp_progress;
        mine.context = job;
        job->code = modclaim_upload(&job->plan, &mine, &job->why);
        break;
    case JOB_CLAIM:
        modclaim_set_budget(MP_STORE_WAIT_MS);
        list = (const char **)calloc(job->nunits ? job->nunits : 1, sizeof *list);
        if (!list) { job->code = MODNET_E_NOMEM; break; }
        for (i = 0; i < job->nunits; i++) list[i] = job->units[i];
        job->code = modclaim_claim(job->ref, list, job->nunits, &job->removed, &job->nremoved, io, &job->why);
        if (job->code != MODNET_OK && modnet_transient(job->code)) (void)modclaim_queue(job->ref, list, job->nunits);
        if (job->code == MODNET_OK || modnet_transient(job->code)) {
            const char **salts = (const char **)calloc(job->nsalts ? job->nsalts : 1, sizeof *salts);
            for (i = 0; salts && i < job->nsalts; i++) salts[i] = job->salts[i];
            if (salts) modclaim_set_mods(job->ref, salts, job->nsalts);
            free((void *)salts);
        }
        break;
    case JOB_RELEASE:
        modclaim_set_budget(MP_STORE_WAIT_MS);
        job->code = modclaim_release(job->ref, io, &job->why);
        if (job->code != MODNET_OK && modnet_transient(job->code)) (void)modclaim_queue(job->ref, NULL, 0);
        break;
    case JOB_LAUNCH:
        modclaim_set_budget(MP_STORE_WAIT_MS);
        job->code = modclaim_launch(io, &job->launch);
        break;
    case JOB_FETCH:
        /* D25: a map copied to My Maps before its content came: the content of each of its rows (`units` holds the
         * rows' composition hashes) into the cache, so the kept map opens offline. */
        for (i = 0; i < job->nunits && job->code == MODNET_OK; i++) {
            int rc = sh_cache_prepare(job->units[i], io);
            if (rc == MODNET_E_CANCELLED) job->code = rc;
        }
        break;
    default:
        break;
    }
    free((void *)list);
    return job->code;
}

static void mp_job_done(void *context, int result)
{
    mp_job *job = context;
    (void)result;
    g_running = NULL;
    MemoryBarrier();
    InterlockedExchange(&job->done, 1);
}

static const char *mp_job_name(int kind)
{
    switch (kind) {
    case JOB_PREPARE: return "publish preparation";
    case JOB_UPLOAD: return "publish upload";
    case JOB_CLAIM: return "publish claim";
    case JOB_RELEASE: return "publish release";
    case JOB_LAUNCH: return "publish launch retry";
    case JOB_FETCH: return "kept map content";
    default: return "publish";
    }
}

/* 1 started, 0 the worker is busy, -1 it could not start. */
static int mp_job_start(mp_job *job)
{
    int rc;
    if (g_running) return 0;
    g_running = job;
    rc = modnet_job_start(mp_job_name(job->kind), mp_job_run, mp_job_done, NULL, NULL, job);
    if (rc != 1) g_running = NULL;
    return rc;
}

static void mp_queue_push(mp_job *job)
{
    mp_job **link;
    AcquireSRWLockExclusive(&g_queue_lock);
    for (link = &g_queue; *link; link = &(*link)->next) {}
    job->next = NULL;
    *link = job;
    ReleaseSRWLockExclusive(&g_queue_lock);
}

static int mp_queue_release(const char *ref, const char *why)
{
    mp_job *job;
    if (!ref || !ref[0]) return 0;
    if (!(job = mp_job_new(JOB_RELEASE, ref, NULL, 0))) {
        mp_logf("MODS: publish: no memory to queue the release of '%s'", ref);
        return 0;
    }
    mp_queue_push(job);
    mp_logf("MODS: publish: release of the claim for '%s' queued (%s)", ref, why);
    return 1;
}

/* ------------------------------------------------------------ notices */

/* A menu notice goes to the mod system's one notice queue (the notice op:
 * modpublished.h sh_modpublished_notice), raised with a single Accept
 * button when the dialog surface is idle. */
static void mp_menu_notice(const char *text)
{
    if (!text || !text[0]) return;
    mp_logf("MODS: notice: %s", text);
    if (g_ops.notice) g_ops.notice(g_ops.context, text);
}

static void mp_editor_notice(void *editor, const char *text)
{
    mp_logf("MODS: publish notice: %s", text);
    if (g_ops.editor_notice) g_ops.editor_notice(g_ops.context, editor, text);
}

/* The store stayed busy or out of reach past the wait: the publish goes on; the claim at the feed post queues what
 * it could not send, and the next start sends it (N27). */
#define MP_QUEUED_SENTENCE "The map is published, but its mod content could not reach the mod store yet; Snapmap+ " \
                           "sends it the next time DOOM starts."

/* The finished background job: its result lines and, for a claim, what players will miss. */
static void mp_job_report(mp_job *job)
{
    const char *cause = job->why && job->why[0] ? job->why : modnet_code_text(job->code);
    switch (job->kind) {
    case JOB_CLAIM:
        if (job->code == MODNET_OK) mp_logf("MODS: publish: item '%s' claims its %zu unit(s)", job->ref, job->nunits);
        else {
            mp_logf("MODS: publish: the claim of item '%s' failed: %s%s", job->ref, cause,
                    modnet_transient(job->code) ? " (queued; retried at the next start)" : "");
            /* The map is published by now: the author reads what players will miss, once (a retired Snapmap+ reads the
             * newer-version notice below). */
            if (modnet_transient(job->code)) mp_menu_notice(MP_QUEUED_SENTENCE);
            else if (job->code != MODNET_E_RETIRED) {
                char *reason = sh_reason(SH_REASON_STORE, job->code, NULL, 0);
                char *line = sh_textf("The map is published, but its mod content could not reach the mod store: %s.",
                                      reason ? reason : "the mod store refused it");
                free(reason);
                mp_menu_notice(line);
                free(line);
            }
        }
        break;
    case JOB_RELEASE:
        if (job->code == MODNET_OK) mp_link_forget(job->ref);
        else mp_logf("MODS: publish: the release of '%s' failed: %s%s", job->ref, cause,
                     modnet_transient(job->code) ? " (queued; retried at the next start)" : "");
        break;
    case JOB_FETCH:
        mp_logf("MODS: copy: the content of %zu row(s) of the copied map %s", job->nunits,
                job->code == MODNET_OK ? "is here" : "did not all arrive; it comes when the map is opened");
        break;
    case JOB_LAUNCH:
        mp_logf("MODS: launch: the queue: %u retried, %u settled, %d left; %u published map(s) repaired, %u unit(s) sent",
                job->launch.retried, job->launch.settled, job->launch.left, job->launch.repaired, job->launch.uploaded);
        break;
    default:
        break;
    }
    if (modnet_newer_needed()) mp_menu_notice(MODNET_NEWER_SENTENCE);
}


/* ------------------------------------------------------------ the hold and the context */

typedef struct mp_hold {
    int state;
    void *editor;
    unsigned char overwrite;                   /* PublishStart's own argument, given back at the resume */
    unsigned long long token;
    int ticket;
    int waited;
    int started;                               /* the job is on the worker */
    int made_key;                              /* the preparation made this computer's publishing key */
    unsigned long long shown;                  /* the progress the pacifier shows */
    mp_job *job;                               /* the prepare job, then the upload job */
    modclaim_plan plan;                        /* what the store lacks, kept from the prepare */
    char *after;                               /* a notice for after the publish (S8, removed, queued), malloc'd */
    int finish;                                /* held at the publish screens' FINISH, which stay up (R-LB1 S7) */
} mp_hold;

typedef struct mp_context {
    int kind;
    void *editor;
    unsigned long long token;
    char (*units)[MODNET_HASH_HEX + 1];
    size_t nunits;
    char (*salts)[SH_MODMANIFEST_SALT_CAP];
    size_t nsalts;
    ULONGLONG at;
} mp_context;

static mp_hold g_hold;
static mp_context g_ctx;
/* The units the last prepared publish found neither here nor in the store, or removed from it: left out of its claim
 * (R-LB1 F7). Set at the resume, cleared when a publish begins. */
static struct { char (*v)[MODNET_HASH_HEX + 1]; size_t n; } g_left_out;
static int g_finish_on;                 /* S7 is asked at FINISH (sh_modpublish_finish_set); off: at PublishStart */
static int (*g_finish_patch)(int on);   /* the engine side's gate-call patch: 1 when it is as asked */
static int g_editor_ticking;            /* inside sh_modpublish_editor_tick */
static struct { int armed; ULONGLONG at; } g_pin;   /* the resumed PublishStart's save pins its rows */

static void mp_ctx_clear(void)
{
    free(g_ctx.units);
    free(g_ctx.salts);
    memset(&g_ctx, 0, sizeof g_ctx);
}

static void mp_busy(void *editor, const char *text)
{
    if (g_ops.busy) g_ops.busy(g_ops.context, editor, text);
}

static void mp_hold_free(void)
{
    mp_hold *h = &g_hold;
    if (h->ticket && g_ops.release) g_ops.release(g_ops.context, h->ticket);
    if (h->job) {
        if (!h->started || InterlockedCompareExchange(&h->job->done, 0, 0)) mp_job_free(h->job);
        else {
            if (g_running == h->job) (void)modnet_job_cancel();
            h->job->next = g_orphans;
            g_orphans = h->job;
        }
    }
    modclaim_plan_free(&h->plan);
    free(h->after);
    memset(h, 0, sizeof(*h));
}

/* The editor is gone: nothing to clear on its screen. */
static void mp_abandon(const char *why)
{
    if (g_hold.state == HOLD_IDLE) return;
    mp_logf("MODS: publish: the held publish was abandoned: %s", why);
    mp_hold_free();
}

/* The publish does not start: the pacifier goes, and one sentence says why. Nothing was sent to the map service. Held
 * at FINISH, the publish screens stay as they were and the sentence is the editor's own dialog over them, as the
 * game's own publish errors are (the toast when that dialog cannot show). */
static void mp_refuse(const char *notice, const char *why)
{
    void *editor = g_hold.editor;
    int finish = g_hold.finish;
    if (g_hold.state == HOLD_IDLE) return;
    mp_logf("MODS: publish: not started: %s", why);
    mp_hold_free();
    mp_busy(editor, NULL);
    if (finish && g_ops.finish_clear) g_ops.finish_clear(g_ops.context, editor);
    if (!notice) return;
    if (finish && g_ops.finish_ask && (!g_ops.finish_can_ask || g_ops.finish_can_ask(g_ops.context, editor)) &&
        g_ops.finish_ask(g_ops.context, editor, NULL, notice, 0)) {
        mp_logf("MODS: publish notice: %s", notice);
        return;
    }
    mp_editor_notice(editor, notice);
}

int sh_modpublish_holding(void)
{
    return g_hold.state != HOLD_IDLE;
}

static unsigned long long mp_token(void *editor)
{
    return g_ops.editor_token ? g_ops.editor_token(g_ops.context, editor) : 0;
}

/* Every pinned or pinnable unit is in the store (or queued): the engine's PublishStart runs, its save pins the rows. */
static void mp_resume(void)
{
    mp_hold *h = &g_hold;
    void *editor = h->editor;
    unsigned char overwrite = h->overwrite;
    int finish = h->finish;
    char *after;
    /* Held at FINISH: the screens close where the frame would have closed them, in the editor's frame (the pacifier
     * stays until then). */
    if (finish && !g_editor_ticking) {
        if (h->state != HOLD_RESUME) { h->state = HOLD_RESUME; h->waited = 0; }
        return;
    }
    after = h->after;
    h->after = NULL;
    {
        /* what no claim of this publish can name (F7): the plan's absent and removed units */
        size_t i;
        free(g_left_out.v);
        memset(&g_left_out, 0, sizeof g_left_out);
        if (h->plan.n && (g_left_out.v = calloc(h->plan.n, sizeof *g_left_out.v)) != NULL)
            for (i = 0; i < h->plan.n; i++)
                if (h->plan.state[i] == MODCLAIM_ABSENT || h->plan.state[i] == MODCLAIM_REMOVED)
                    strcpy_s(g_left_out.v[g_left_out.n++], MODNET_HASH_HEX + 1, h->plan.units[i]);
    }
    mp_hold_free();
    mp_busy(editor, NULL);
    g_pin.armed = 1;
    g_pin.at = GetTickCount64();
    mp_log(finish ? "MODS: publish: the map's mod content is ready; the publish screens close and the publish starts"
                  : "MODS: publish: the map's mod content is ready; the publish starts");
    if (finish && g_ops.finish_close) g_ops.finish_close(g_ops.context, editor);
    InterlockedExchange(&g_resuming, 1);
    if (g_ops.start) g_ops.start(g_ops.context, editor, overwrite);
    InterlockedExchange(&g_resuming, 0);
    /* the frame's own order: the screens' close, PublishStart, then its success event */
    if (finish && g_ops.finish_done) g_ops.finish_done(g_ops.context, editor);
    if (after) { mp_menu_notice(after); free(after); }
}

void sh_modpublish_editor_tick(void *editor)
{
    if (g_hold.state != HOLD_RESUME || !editor || editor != g_hold.editor || !mp_on_main()) return;
    if (mp_token(editor) != g_hold.token || !g_hold.token) return;   /* the tick abandons it */
    g_editor_ticking = 1;
    mp_resume();
    g_editor_ticking = 0;
}

static int mp_hold_begin(void *editor, unsigned char overwrite, int finish);

int sh_modpublish_publish_start(void *editor, unsigned char overwrite)
{
    if (!g_ops_set || InterlockedCompareExchange(&g_resuming, 0, 0) || !mp_on_main()) return 1;
    if (g_hold.state != HOLD_IDLE) return 0;   /* the held publish is still preparing: a second Publish waits for it */
    return mp_hold_begin(editor, overwrite, 0);
}

int sh_modpublish_finish_gate(void *editor, unsigned char overwrite)
{
    if (!g_ops_set || !g_finish_on || !g_ops.finish_ask || !g_ops.finish_poll || !g_ops.finish_close ||
        InterlockedCompareExchange(&g_resuming, 0, 0) || !mp_on_main())
        return 0;
    if (g_hold.state != HOLD_IDLE) return 1;   /* a held publish is on its way: FINISH waits for it */
    return mp_hold_begin(editor, overwrite, 1) ? 0 : 1;
}

void sh_modpublish_finish_set(int on)
{
    on = on ? 1 : 0;
    if (g_finish_patch && !g_finish_patch(on)) {
        mp_logf("MODS: publish: the FINISH step could not be %s; S7 stays where it was (%s)",
                on ? "hooked" : "unhooked", g_finish_on ? "at FINISH" : "after the publish screens close");
        return;
    }
    if (g_finish_on != on) mp_logf("MODS: publish: S7 is now asked %s", on ? "at FINISH, over the publish screens"
                                                                          : "after the publish screens close");
    g_finish_on = on;
}

int sh_modpublish_finish_on(void) { return g_finish_on; }

/* A held publish begins (1: nothing to hold, the publish goes on as the engine would). */
static int mp_hold_begin(void *editor, unsigned char overwrite, int finish)
{
    mp_hold *h = &g_hold;
    char **names = NULL;
    size_t count = 0;
    sh_modmanifest_list rows;
    int n;
    if (g_ops.account_name) {
        /* The name the key's requests present (D31): read from the signed-in game at each publish, never typed. */
        char name[256];
        if (g_ops.account_name(g_ops.context, name, sizeof name)) modclaim_set_account_name(name);
    }
    if (g_ctx.kind != CTX_NONE) mp_ctx_clear();
    mp_upload_clear();
    free(g_left_out.v);
    memset(&g_left_out, 0, sizeof g_left_out);
    memset(&rows, 0, sizeof rows);
    if (g_ops.placed_names && g_ops.placed_names(g_ops.context, &names, &count) < 0) count = 0;
    n = g_rows_for((const char *const *)names, count, &rows);
    sh_modmanifest_names_free(names, count);
    if (n <= 0) {
        sh_modmanifest_list_free(&rows);
        return 1;   /* a vanilla map: the engine publishes it as it always has */
    }
    if (g_ops.cloud_ready && !g_ops.cloud_ready(g_ops.context)) {
        /* the published copy could not carry the pinned rows: players would play stand-ins */
        sh_modmanifest_list_free(&rows);
        mp_log("MODS: publish: not started: the cloud save's read of the slot is not hooked on this build");
        mp_editor_notice(editor, "The map was not published: Snapmap+ cannot send its mod content with this game "
                                 "version. Use Send feedback in the Snapmap+ window.");
        return 0;
    }
    memset(h, 0, sizeof *h);
    h->editor = editor;
    h->overwrite = overwrite;
    h->token = mp_token(editor);
    if (!(h->job = mp_job_new(JOB_PREPARE, NULL, NULL, 0))) {
        sh_modmanifest_list_free(&rows);
        memset(h, 0, sizeof *h);
        mp_log("MODS: publish: no memory to prepare the map's mod content; published as the engine would");
        return 1;
    }
    h->job->rows = rows;
    h->state = HOLD_PREPARING;
    h->waited = 0;
    h->finish = finish;
    mp_busy(editor, "Preparing this map's mod content.");
    mp_logf("MODS: publish held%s: preparing %d row(s) of mod content", finish ? " at FINISH" : "", n);
    return 0;
}

/* The armed publish's save: its rows are pinned from the units the preparation computed (modpin.h; the same walk,
 * remembered, so this is a lookup). Bound with sh_modmanifest_set_pin. */
int sh_modpublish_pin_save(sh_modmanifest_list *rows, const char *json, size_t length)
{
    size_t i;
    int pinned = 0;
    if (!g_pin.armed) return 0;
    if (GetTickCount64() - g_pin.at > MP_PIN_ARM_MS) { g_pin.armed = 0; return 0; }
    g_pin.armed = 0;
    mp_upload_clear();
    for (i = 0; rows && i < rows->count; i++) {
        sh_modpin_row_result r;
        int state;
        if (rows->rows[i].pinned || !_stricmp(rows->rows[i].salt, SH_MODMANIFEST_PRODUCT)) continue;
        memset(&r, 0, sizeof r);
        state = sh_modpin_row(&rows->rows[i], json, length, NULL, &r);
        if (state == SH_MODPIN_PINNED || state == SH_MODPIN_WHOLE) pinned++;
        else mp_logf("MODS: publish: a row could not be pinned at the save: %s", r.why ? r.why : "unknown");
        free(r.why);
    }
    mp_logf("MODS: publish: the save pinned %d row(s) to their units for the published copy; the map's slot keeps its "
            "own rows", pinned);
    if (rows && rows->count) {
        size_t k;
        int ok;
        AcquireSRWLockExclusive(&g_upload_lock);
        for (k = 0; k < rows->count && sh_modmanifest_list_add(&g_upload.rows, &rows->rows[k]); k++) {}
        g_upload.rows.uses = rows->uses;
        ok = k == rows->count;
        if (ok) {
            g_upload.armed = 1;
            g_upload.at = GetTickCount64();
            /* the save observer reports this save next, with the next sequence */
            g_upload.sequence = (unsigned long)InterlockedCompareExchange(&g_save_sequence, 0, 0) + 1;
        }
        ReleaseSRWLockExclusive(&g_upload_lock);
        if (!ok) { mp_upload_clear(); mp_log("MODS: publish: no memory for the published copy's rows; it carries the slot's"); }
    }
    return pinned > 0;
}

int sh_modpublish_after_save(void *editor)
{
    unsigned long long token;
    char line[MP_TEXT_CAP], size[32], over[32];
    if (!g_ops_set || InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!mp_on_main()) return 1;
    token = mp_token(editor);
    if (g_ctx.kind != CTX_NONE) mp_ctx_clear();
    g_ctx.editor = editor;
    g_ctx.token = token;
    g_ctx.at = GetTickCount64();
    if (!g_save.valid || g_ctx.at - g_save.at > MP_SAVE_FRESH_MS) {
        g_ctx.kind = CTX_UNKNOWN;
        mp_log("MODS: publish: the saved text was not seen; the publish continues without the mod check");
        return 1;
    }
    /* The bound counts the whole saved text, the snapmap-plus member included: a map that carries the member or names
     * mods is refused before anything is sent. A map without either is the engine's own text and its own check. */
    if (g_save.length > SH_MODPUBLISH_TEXT_BOUND && (g_save.count || g_save.carrier)) {
        sh_modpublish_size_text(size, sizeof size, g_save.length);
        sh_modpublish_size_text(over, sizeof over, g_save.length - SH_MODPUBLISH_TEXT_BOUND);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "This map is %s, over the map service's 10.0 MB limit by %s. It was not published.", size, over);
        mp_logf("MODS: publish refused: the saved text is %zu bytes, over the %llu-byte bound", g_save.length,
                SH_MODPUBLISH_TEXT_BOUND);
        if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
        mp_editor_notice(editor, line);
        memset(&g_ctx, 0, sizeof g_ctx);
        return 0;
    }
    if (g_save.unreadable) {
        char *said = g_save.unreadable_name
            ? sh_textf("The map was not published: Snapmap+ cannot read the mod %s in its list. Remove that mod in "
                       "the Mods tab, save, and publish again.", g_save.unreadable_name)
            : NULL;
        const char *text = said ? said : "The map was not published: its list of mods cannot be read. Save the map "
                                         "again, then publish it.";
        mp_logf("MODS: publish refused: %s", text);
        if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
        mp_editor_notice(editor, text);
        free(said);
        memset(&g_ctx, 0, sizeof g_ctx);
        return 0;
    }
    if (g_save.local)
        mp_logf("MODS: publish: %zu row(s) of the save were not pinned to units; players get stand-ins for them",
                g_save.local);
    if (!g_save.nunits) {
        g_ctx.kind = CTX_NO_ROWS;
        return 1;
    }
    g_ctx.kind = CTX_ROWS;
    if ((g_ctx.units = calloc(g_save.nunits, sizeof *g_ctx.units)) != NULL) {
        /* R-LB1 F7: the claim names only what the store holds or this publish sent. A unit neither on this computer
         * nor in the store, or one moderation removed, is left out (S8 said so after the publish): no claim of it can
         * ever succeed, and no later start could send it. */
        size_t i, k, out = 0, left = 0;
        for (i = 0; i < g_save.nunits; i++) {
            int skip = 0;
            for (k = 0; k < g_left_out.n && !skip; k++) skip = !strcmp(g_left_out.v[k], g_save.units[i]);
            if (skip) { left++; continue; }
            memcpy(g_ctx.units[out++], g_save.units[i], sizeof *g_ctx.units);
        }
        g_ctx.nunits = out;
        if (left)
            mp_logf("MODS: publish: %zu unit(s) left out of the claim: neither on this computer nor in the mod store, or "
                    "removed from it", left);
        if (!out) {
            /* The map still names mods: an emptied claim is no release (F9); the item keeps what it claims, and S8 has
             * told the author what players will miss. */
            free(g_ctx.units);
            g_ctx.units = NULL;
            g_ctx.nunits = 0;
            g_ctx.kind = CTX_KEEP;
            mp_log("MODS: publish: no unit of the map's mods can be claimed; the published item keeps its claim");
            return 1;
        }
    }
    if (g_save.nsalts && (g_ctx.salts = calloc(g_save.nsalts, sizeof *g_ctx.salts)) != NULL) {
        memcpy(g_ctx.salts, g_save.salts, g_save.nsalts * sizeof *g_ctx.salts);
        g_ctx.nsalts = g_save.nsalts;
    }
    if (g_tag.tagged_sequence != g_save.sequence)
        mp_log("MODS: publish: the search tag '" SH_MODPUBLISH_TAG "' was not written with this save; the published "
               "map will not carry it");
    g_tag.armed = 0;
    return 1;
}

void sh_modpublish_feed_post(void *edit, const char *item_id, const char *revision)
{
    mp_job *job;
    if (!g_ops_set || !mp_on_main()) return;
    if (mp_upload_live() && !g_upload.served)
        mp_log("MODS: publish: the cloud save's read of the slot never reached Snapmap+; the published map carries the "
               "slot's local rows, so players get stand-ins for its mods' objects");
    mp_upload_clear();   /* the published copy is uploaded */
    if (!item_id || !modnet_valid_ref(item_id)) {
        mp_logf("MODS: publish: the published item id '%s' is not a usable claim ref; no claim change",
                item_id ? item_id : "");
        return;
    }
    if (g_ctx.kind == CTX_NONE || g_ctx.token != (unsigned long long)(uintptr_t)edit) {
        mp_logf("MODS: publish: item '%s' revision '%s' posted without a publish this session decided; "
                "no claim change", item_id, revision ? revision : "");
        return;
    }
    switch (g_ctx.kind) {
    case CTX_ROWS:
        job = mp_job_new(JOB_CLAIM, item_id, (const char (*)[MODNET_HASH_HEX + 1])g_ctx.units, g_ctx.nunits);
        if (!job) {
            mp_log("MODS: publish: no memory to queue the claim");
            (void)modclaim_queue(item_id, NULL, 0);
            mp_ctx_clear();
            return;
        }
        if (g_ctx.nsalts && (job->salts = calloc(g_ctx.nsalts, sizeof *job->salts)) != NULL) {
            memcpy(job->salts, g_ctx.salts, g_ctx.nsalts * sizeof *job->salts);
            job->nsalts = g_ctx.nsalts;
        }
        mp_queue_push(job);
        mp_logf("MODS: publish: item '%s' revision '%s' published with %zu unit(s); claim queued", item_id,
                revision ? revision : "", g_ctx.nunits);
        {
            /* The engine keeps no link from the saved map to this item; the delete check needs one. */
            char cloud[MP_ID_CAP] = "", local[MP_ID_CAP] = "";
            if (!g_ops.edit_ids || !g_ops.edit_ids(g_ops.context, edit, cloud, sizeof cloud, local, sizeof local))
                cloud[0] = local[0] = 0;
            mp_link_record(item_id, cloud, local);
        }
        break;
    case CTX_NO_ROWS:
        /* A republish without mods: the item's old set (if any) is no longer needed by its current revision. */
        if (modclaim_known(item_id)) (void)mp_queue_release(item_id, "the republished map uses no mods");
        break;
    case CTX_KEEP:
        /* R-LB1 F9: the map still names mods but none of its units can be claimed (S8 said so): never a release, the
         * item keeps what it claims */
        mp_logf("MODS: publish: item '%s' revision '%s' names mods none of whose units the store holds or this publish "
                "sent; its claim is kept as it was", item_id, revision ? revision : "");
        break;
    default:
        mp_logf("MODS: publish: item '%s' published without the mod check; no claim change", item_id);
        break;
    }
    mp_ctx_clear();
}

void sh_modpublish_unpublished(const char *item_id)
{
    if (!g_ops_set || !mp_on_main()) return;
    if (!item_id || !modnet_valid_ref(item_id)) {
        mp_logf("MODS: unpublish: the item id '%s' is not a usable claim ref", item_id ? item_id : "");
        return;
    }
    if (!modclaim_known(item_id)) {
        mp_logf("MODS: unpublish: item '%s' has no claim of this computer; nothing to release", item_id);
        return;
    }
    (void)mp_queue_release(item_id, "the map was unpublished");
}

/* ------------------------------------------------------------ links */

/* The "links" section of mods\snapmap-plus.json: {"<item>":{"cloud":"..","local":"..","at":..}},
 * edited here as the document {"format":1,"links":{...}}. Main thread only
 * (feed post, delete, job reports); the record layer writes through a
 * temporary and a rename. Ids are kept only when they are plain (letters,
 * digits, '-', '_', '.', ':'), so the section needs no escaping. */

static int mp_link_id(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= MP_ID_CAP) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' ||
              *s == '_' || *s == '.' || *s == ':')) return 0;
    return 1;
}

/* The links document, or a fresh empty one; NULL only without memory. */
static mpj *mp_links_load(void)
{
    const char *root = sh_ms_root();
    char *text = NULL;
    mpj *doc, *links = NULL;
    if (root && sh_mb_record_get(root, SH_MODPUBLISH_LINKS_SECTION, &text) == 1 && text)
        links = mpj_parse(text, strlen(text));
    free(text);
    if (links && links->type != MPJ_OBJ) { mpj_free(links); links = NULL; }
    doc = mpj_new(MPJ_OBJ);
    if (doc && (!mpj_set(doc, "format", mpj_new_int(1)) || !mpj_set(doc, "links", links ? links : mpj_new(MPJ_OBJ)))) {
        mpj_free(doc);
        doc = NULL;
    }
    return doc;
}

static int mp_links_save(const mpj *doc)
{
    const char *root = sh_ms_root();
    const mpj *links = doc ? mpj_get(doc, "links") : NULL;
    size_t len = 0;
    char *text = links ? mpj_dump(links, &len) : NULL;
    int ok = text && root && sh_mb_record_put(root, SH_MODPUBLISH_LINKS_SECTION, text);
    free(text);
    return ok;
}

int sh_modpublish_item_linked(const char *item)
{
    mpj *doc;
    int found;
    if (!mp_link_id(item)) return 0;
    doc = mp_links_load();
    found = doc && mpj_get(mpj_get(doc, "links"), item) != NULL;
    mpj_free(doc);
    return found;
}

/* Record that `item` was published from this saved map. */
static void mp_link_record(const char *item, const char *cloud, const char *local)
{
    mpj *doc, *link;
    if (!mp_link_id(item) || (!mp_link_id(cloud) && !mp_link_id(local))) {
        mp_logf("MODS: publish: item '%s' has no readable saved-map id; deleting the map will not release its claim",
                item ? item : "");
        return;
    }
    if (!(doc = mp_links_load())) return;
    link = mpj_new(MPJ_OBJ);
    if (link && mp_link_id(cloud)) (void)mpj_set(link, "cloud", mpj_new_str(cloud));
    if (link && mp_link_id(local)) (void)mpj_set(link, "local", mpj_new_str(local));
    if (link) (void)mpj_set(link, "at", mpj_new_int((long long)time(NULL)));
    if (!link || !mpj_set(mpj_get(doc, "links"), item, link) || !mp_links_save(doc))
        mp_logf("MODS: publish: the saved-map link of item '%s' could not be written", item);
    mpj_free(doc);
}

static void mp_link_forget(const char *item)
{
    mpj *doc, *links;
    int found = -1;
    if (!mp_link_id(item) || !(doc = mp_links_load())) return;
    links = mpj_get(doc, "links");
    for (int i = 0; i < links->n && found < 0; i++) if (links->keys[i] && !strcmp(links->keys[i], item)) found = i;
    if (found >= 0) {
        mpj_free(links->items[found]);
        free(links->keys[found]);
        memmove(&links->items[found], &links->items[found + 1], (size_t)(links->n - found - 1) * sizeof(*links->items));
        memmove(&links->keys[found], &links->keys[found + 1], (size_t)(links->n - found - 1) * sizeof(*links->keys));
        links->n--;
        (void)mp_links_save(doc);
    }
    mpj_free(doc);
}

/* ------------------------------------------------------------ delete */

#define MP_DELETE_ITEMS 16
#define MP_ANSWER_ITEMS 256
/* The search answers in pages of 30 (200 for the first query of a session);
 * an answer this long may be a first page, so it proves nothing missing. */
#define MP_ANSWER_PAGE  30

/* A link younger than this is never released at delete: a fresh item may not
 * be in the search yet. */
static long long g_link_min_age = 3600;

typedef struct mp_check {
    struct mp_check *next;
    unsigned ticket;
    int started;
    ULONGLONG at;
    char cloud[MP_ID_CAP];
    int count;
    char items[MP_DELETE_ITEMS][MP_ID_CAP];
} mp_check;

typedef struct mp_answer {
    struct mp_answer *next;
    unsigned ticket;
    int ok, count;
    char (*items)[MP_ID_CAP];
} mp_answer;

static mp_check *g_checks;                 /* main thread */
static mp_answer *volatile g_answers;      /* any thread, under g_answer_lock */
static SRWLOCK g_answer_lock = SRWLOCK_INIT;
static volatile LONG g_ticket;

void sh_modpublish_published_result(unsigned ticket, int ok, const char *const *item_ids, int count)
{
    mp_answer *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->ticket = ticket;
    a->ok = ok;
    if (ok && count > 0 && item_ids) {
        if (count > MP_ANSWER_ITEMS) count = MP_ANSWER_ITEMS;
        a->items = calloc((size_t)count, sizeof(*a->items));
        if (!a->items) { free(a); return; }
        /* An answer id that is not a plain id of the links' form names no
         * linked item: it is left out, never cut. */
        for (int i = 0; i < count; i++)
            if (item_ids[i] && mp_link_id(item_ids[i])) strcpy_s(a->items[a->count++], MP_ID_CAP, item_ids[i]);
    }
    AcquireSRWLockExclusive(&g_answer_lock);
    a->next = g_answers;
    g_answers = a;
    ReleaseSRWLockExclusive(&g_answer_lock);
}

void sh_modpublish_deleted(const char *local_id, const char *cloud_id)
{
    mp_check *check;
    mpj *doc, *links;
    int found = 0, young = 0;
    long long now = (long long)time(NULL);
    if (!g_ops_set) return;
    if (!mp_on_main()) {
        mp_log("MODS: delete: the delete completion ran off the main thread; no claim is checked");
        return;
    }
    if (!mp_link_id(cloud_id) && !mp_link_id(local_id)) return;
    {
        /* Nothing published from this installation: no links, nothing to do. */
        const char *root = sh_ms_root();
        char *text = NULL;
        int has = root && sh_mb_record_get(root, SH_MODPUBLISH_LINKS_SECTION, &text) == 1;
        free(text);
        if (!has) return;
    }
    if (!(check = calloc(1, sizeof(*check)))) return;
    if (mp_link_id(cloud_id)) strcpy_s(check->cloud, sizeof check->cloud, cloud_id);
    if (!(doc = mp_links_load())) { free(check); return; }
    links = mpj_get(doc, "links");
    for (int i = 0; i < links->n && check->count < MP_DELETE_ITEMS; i++) {
        const mpj *link = links->items[i];
        const mpj *cloud = link ? mpj_get(link, "cloud") : NULL, *local = link ? mpj_get(link, "local") : NULL;
        const char *item = links->keys[i];
        int same = (check->cloud[0] && mpj_str_is(cloud, check->cloud)) ||
                   (mp_link_id(local_id) && mpj_str_is(local, local_id));
        const mpj *at = link ? mpj_get(link, "at") : NULL;
        if (!same || !item || !modnet_valid_ref(item)) continue;
        found++;
        if (!modclaim_known(item)) continue;   /* released already */
        if (!at || !mpj_is_num(at) || now - mpj_int(at) < g_link_min_age) { young++; continue; }
        strcpy_s(check->items[check->count++], MP_ID_CAP, item);
    }
    mpj_free(doc);
    if (young)
        mp_logf("MODS: delete: %d claimed item(s) of the deleted map were published less than %lld minute(s) ago; "
                "their claims are kept", young, g_link_min_age / 60);
    if (!check->count) {
        if (found && !young)
            mp_logf("MODS: delete: the deleted map's %d published item(s) hold no claim; nothing to release", found);
        free(check);
        return;
    }
    if (!check->cloud[0]) {
        mp_logf("MODS: delete: the deleted map has no cloud-saved id, so whether its %d claimed item(s) are still "
                "published cannot be checked; the claims are kept", check->count);
        free(check);
        return;
    }
    /* The query starts on the next main tick, outside the engine's completion. */
    check->ticket = (unsigned)InterlockedIncrement(&g_ticket);
    check->at = GetTickCount64();
    check->next = g_checks;
    g_checks = check;
}

static void mp_start_checks(void)
{
    mp_check **link = &g_checks;
    while (*link) {
        mp_check *check = *link;
        if (check->started) { link = &check->next; continue; }
        if (!g_ops.published_query || !g_ops.published_query(g_ops.context, check->cloud, check->ticket)) {
            mp_logf("MODS: delete: the published-items check for '%s' could not start; %d claim(s) kept",
                    check->cloud, check->count);
            *link = check->next;
            free(check);
            continue;
        }
        check->started = 1;
        check->at = GetTickCount64();
        mp_logf("MODS: delete: checking whether the %d claimed item(s) published from '%s' are still published",
                check->count, check->cloud);
        link = &check->next;
    }
}

/* Main tick: settle the answered checks and drop the expired ones. */
static void mp_poll_checks(void)
{
    mp_answer *answers, *a;
    mp_check **link;
    mp_start_checks();
    AcquireSRWLockExclusive(&g_answer_lock);
    answers = g_answers;
    g_answers = NULL;
    ReleaseSRWLockExclusive(&g_answer_lock);
    while ((a = answers) != NULL) {
        mp_check *check = NULL;
        answers = a->next;
        for (link = &g_checks; *link; link = &(*link)->next)
            if ((*link)->ticket == a->ticket) { check = *link; *link = check->next; break; }
        if (check && !a->ok) {
            mp_logf("MODS: delete: the published-items check for '%s' failed; %d claim(s) kept", check->cloud,
                    check->count);
        } else if (check && a->count >= MP_ANSWER_PAGE) {
            mp_logf("MODS: delete: '%s' answered a full page of %d published item(s); %d claim(s) kept",
                    check->cloud, a->count, check->count);
        } else if (check) {
            int released = 0, kept = 0;
            for (int i = 0; i < check->count; i++) {
                int listed = 0;
                for (int k = 0; k < a->count && !listed; k++) listed = !strcmp(a->items[k], check->items[i]);
                if (listed) { kept++; continue; }
                if (mp_queue_release(check->items[i], "the deleted map's item is no longer published")) released++;
            }
            mp_logf("MODS: delete: '%s' has %d published item(s) now; %d claim(s) released, %d kept (still published)",
                    check->cloud, a->count, released, kept);
        }
        free(check);
        free(a->items);
        free(a);
    }
    for (link = &g_checks; *link;) {
        mp_check *check = *link;
        if (GetTickCount64() - check->at <= SH_MODPUBLISH_DELETE_CHECK_MS) { link = &check->next; continue; }
        *link = check->next;
        mp_logf("MODS: delete: no published-items answer for '%s' within %llu s; %d claim(s) kept", check->cloud,
                SH_MODPUBLISH_DELETE_CHECK_MS / 1000ull, check->count);
        free(check);
    }
}

/* ------------------------------------------------------------ copy gate */

static char *g_cache_root;   /* tests: the cache folder (malloc'd), or NULL for the game's */

/* The game's published-map cache folder (malloc'd), or NULL. */
static char *mp_cache_root(void)
{
    PWSTR known = NULL;
    char *local, *out;
    if (g_cache_root) return sh_text(g_cache_root);
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) || !known) return NULL;
    local = sh_mb_utf8(known);
    CoTaskMemFree(known);
    if (!local) return NULL;
    out = sh_textf("%s\\id Software\\DOOM\\base\\doomsnapmaps\\published-maps", local);
    free(local);
    return out;
}

static int mp_plain_id(const char *s)
{
    if (!s || !s[0] || strlen(s) > 128) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

char *sh_modpublish_cache_path(const char *item_id, const char *revision)
{
    char *root, *out;
    if (!mp_plain_id(item_id) || !mp_plain_id(revision) || !(root = mp_cache_root())) return NULL;
    out = sh_textf("%s\\%s_%s_map.decl", root, item_id, revision);
    free(root);
    return out;
}

/* D25 (5.9): a published map copied to My Maps keeps its rows, and its content must be here for the kept map to open
 * offline: the pinned rows whose composition is not complete on this computer are fetched in the background, with
 * nothing asked. The cached published text names them. */
static void mp_copy_fetch(const char *item_id, const char *revision)
{
    char *path = sh_modpublish_cache_path(item_id, revision), *text;
    size_t len = 0, i, n = 0;
    sh_modmanifest_list rows;
    char (*want)[MODNET_HASH_HEX + 1] = NULL;
    mp_job *job;
    if (!path) return;
    text = (char *)sh_mb_read_file(path, (size_t)SH_MODPUBLISH_TEXT_BOUND + 1, &len);
    free(path);
    if (!text) return;
    memset(&rows, 0, sizeof rows);
    (void)sh_modmanifest_read(text, len, &rows, NULL);
    if (rows.count) sh_modmanifest_content_for(text, len, &rows);   /* the cache learns the rows before it fetches them */
    free(text);
    if (rows.count && (want = calloc(rows.count, sizeof *want)) != NULL)
        for (i = 0; i < rows.count; i++) {
            int complete = 0;
            if (!rows.rows[i].pinned || !sh_cache_row_known(rows.rows[i].build)) continue;
            if (sh_cache_composed(rows.rows[i].build, &complete) && complete) continue;
            strcpy_s(want[n++], MODNET_HASH_HEX + 1, rows.rows[i].build);
        }
    sh_modmanifest_list_free(&rows);
    if (n && (job = mp_job_new(JOB_FETCH, item_id, (const char (*)[MODNET_HASH_HEX + 1])want, n)) != NULL) {
        mp_queue_push(job);
        mp_logf("MODS: copy: map '%s' is kept now; the content of %zu of its row(s) is fetched in the background", item_id, n);
    }
    free(want);
}

int sh_modpublish_copy_allowed(const char *item_id, const char *revision)
{
    char *path, size[32], over[32], line[MP_TEXT_CAP];
    wchar_t *wide;
    WIN32_FILE_ATTRIBUTE_DATA data;
    unsigned long long bytes;
    int cached;
    if (!g_ops_set || !mp_on_main()) return 1;
    if (!(path = sh_modpublish_cache_path(item_id, revision))) {
        mp_logf("MODS: copy: map '%s' revision '%s' has no cache path; copied natively", item_id ? item_id : "",
                revision ? revision : "");
        return 1;
    }
    wide = sh_mb_wpath(path);
    cached = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
    free(wide);
    free(path);
    if (!cached) {
        mp_logf("MODS: copy: the published text of '%s' is not cached; copied natively", item_id);
        return 1;
    }
    bytes = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (bytes <= SH_MODPUBLISH_TEXT_BOUND) {
        mp_copy_fetch(item_id, revision);
        return 1;
    }
    sh_modpublish_size_text(size, sizeof size, bytes);
    sh_modpublish_size_text(over, sizeof over, bytes - SH_MODPUBLISH_TEXT_BOUND);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "This map is %s, over the map service's 10.0 MB limit by %s, so it cannot be copied to My Maps.",
                size, over);
    mp_logf("MODS: copy refused: '%s' revision '%s' is %llu bytes, over the %llu-byte bound", item_id, revision,
            bytes, SH_MODPUBLISH_TEXT_BOUND);
    mp_menu_notice(line);
    return 0;
}


/* ------------------------------------------------------------ launch */

void sh_modpublish_launch(void)
{
    mp_job *job = mp_job_new(JOB_LAUNCH, NULL, NULL, 0);
    if (!job) { mp_log("MODS: launch: no memory to queue the retry"); return; }
    mp_queue_push(job);
}

/* ------------------------------------------------------------ the tick */

#define MP_WORKER_TICKS    (60 * 180)   /* the worker busy with a background job: about three minutes */
#define MP_PROGRESS_TICKS  30           /* the pacifier's progress line, twice a second at most */

static void mp_orphans_retire(void)
{
    mp_job **link = &g_orphans;
    while (*link) {
        mp_job *job = *link;
        if (!InterlockedCompareExchange(&job->done, 0, 0)) { link = &job->next; continue; }
        *link = job->next;
        mp_job_free(job);
    }
}

/* A store failure as the refusal says it: the store's own sentence alone, else the plain reason. malloc'd. */
static char *mp_store_sentence(const mp_job *job)
{
    if (job->code == MODNET_E_RETIRED) return sh_text(MODNET_RETIRED_SENTENCE);
    /* The store's own words, when it sent some (the client's stand-in for an empty answer is not the store's). */
    if (job->code == MODNET_E_REFUSED && job->why && job->why[0] && strcmp(job->why, modnet_code_text(job->code))) {
        size_t len = strlen(job->why);
        char *line = sh_textf("The map was not published. %s%s", job->why,
                              job->why[len - 1] == '.' || job->why[len - 1] == '!' || job->why[len - 1] == '?' ? "" : ".");
        char *c = line ? line + strlen("The map was not published. ") : NULL;
        if (c && *c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        return line;
    }
    {
        char *reason = sh_reason(SH_REASON_STORE, job->code, NULL, 0), *line;
        line = sh_textf("The map was not published: %s.", reason ? reason : "the mod store refused its mod content");
        free(reason);
        return line;
    }
}

static void mp_after_add(mp_hold *h, const char *line)
{
    char *joined;
    if (!line) return;
    if (!h->after) { h->after = sh_text(line); return; }
    if ((joined = sh_textf("%s %s", h->after, line)) != NULL) { free(h->after); h->after = joined; }
}

/* The preparation finished: refuse, ask, upload or go on. */
static void mp_prepared(void)
{
    mp_hold *h = &g_hold;
    mp_job *job = h->job;
    h->job = NULL;
    h->started = 0;
    h->waited = 0;
    if (job->unbuilt) {
        char *line = sh_textf("The map was not published: Snapmap+ cannot find the mod %s on this computer. Build or "
                              "install it in the Mods tab, or remove its objects, then publish again.", job->unbuilt);
        mp_job_free(job);
        mp_refuse(line ? line : "The map was not published: one of its mods is not on this computer.",
                  "a row's folder is not built here");
        free(line);
        return;
    }
    if (job->code != MODNET_OK) {
        if (modnet_transient(job->code)) {
            mp_logf("MODS: publish: the mod store did not answer (%s); the publish goes on and its claim is queued",
                    job->why ? job->why : modnet_code_text(job->code));
            mp_after_add(h, MP_QUEUED_SENTENCE);
            mp_job_free(job);
            mp_resume();
            return;
        }
        {
            char *line = mp_store_sentence(job);
            mp_logf("MODS: publish: the preparation failed: %s", job->why ? job->why : modnet_code_text(job->code));
            mp_job_free(job);
            mp_refuse(line ? line : "The map was not published.", "the preparation failed");
            free(line);
        }
        return;
    }
    h->made_key = job->made_key;
    if (h->made_key) {
        /* The first publish made the key: offer its backup once (SPEC 4.2), where the Snapmap+ window shows it. */
        modclaim_key_offer(1);
        if (g_ops.key_made) g_ops.key_made(g_ops.context);
        /* the offer shows in the Snapmap+ window; closed, the publish's notice says where it is */
        if (!g_ops.window_open || !g_ops.window_open(g_ops.context)) mp_after_add(h, SH_MODPUBLISH_KEY_MADE_TEXT);
        mp_log("MODS: publish: the publishing key was made; its backup is offered");
    }
    h->plan = job->plan;
    memset(&job->plan, 0, sizeof job->plan);
    if (job->after) { mp_after_add(h, job->after); }
    mp_logf("MODS: publish: %zu unit(s): %zu held by the store, %zu to upload (%llu bytes), %zu absent, %zu removed",
            h->plan.n, h->plan.n - h->plan.missing - h->plan.absent - h->plan.removed, h->plan.missing,
            h->plan.missing_bytes, h->plan.absent, h->plan.removed);
    mp_job_free(job);
    if (!h->plan.missing) { mp_resume(); return; }
    /* The question is the game's own dialog, over the editor. Held at FINISH, the pacifier stays until the tick that
     * raises the dialog, so the publish screens never take input in between (the game's profanity check clears and
     * shows in one frame too). */
    if (!h->finish) mp_busy(h->editor, NULL);
    h->state = HOLD_RAISE;
}

/* The upload finished: go on, or refuse with the store's sentence. */
static void mp_uploaded(void)
{
    mp_hold *h = &g_hold;
    mp_job *job = h->job;
    h->job = NULL;
    /* The plan went to the upload job; it comes back to the hold, so what is said next counts what this publish sent
     * (R-LB1: the summary read the emptied hold and said 0 units, 0 bytes after each unit's upload line). */
    modclaim_plan_free(&h->plan);
    h->plan = job->plan;
    memset(&job->plan, 0, sizeof job->plan);
    if (job->code == MODNET_OK) {
        mp_logf("MODS: publish: uploaded %zu unit(s), %.1f MB (%llu bytes)", h->plan.missing,
                (double)h->plan.missing_bytes / 1048576.0, h->plan.missing_bytes);
        mp_job_free(job);
        mp_resume();
        return;
    }
    if (modnet_transient(job->code)) {
        mp_logf("MODS: publish: the upload stopped (%s); the publish goes on and its claim is queued",
                job->why ? job->why : modnet_code_text(job->code));
        mp_after_add(h, MP_QUEUED_SENTENCE);
        mp_job_free(job);
        mp_resume();
        return;
    }
    {
        char *line = mp_store_sentence(job);
        mp_logf("MODS: publish: the upload failed: %s", job->why ? job->why : modnet_code_text(job->code));
        mp_job_free(job);
        mp_refuse(line ? line : "The map was not published.", "the upload failed");
        free(line);
    }
}

/* Start the hold's job when the worker is free: 1 running, 0 waiting, -1 refused. */
static int mp_hold_start(mp_hold *h)
{
    int rc;
    if (h->started) return 1;
    rc = mp_job_start(h->job);
    if (rc == 1) { h->started = 1; h->waited = 0; return 1; }
    if (rc < 0 || ++h->waited > MP_WORKER_TICKS) {
        mp_refuse("The map was not published: Snapmap+ is busy with the mod store. Publish again in a moment.",
                  rc < 0 ? "the worker could not start" : "the worker stayed busy");
        return -1;
    }
    return 0;
}

static void mp_poll_hold(void)
{
    mp_hold *h = &g_hold;
    char text[MP_TEXT_CAP];
    int answer;
    if (h->state == HOLD_IDLE) return;
    if (mp_token(h->editor) != h->token || !h->token) {
        mp_abandon("the editor left the map");
        return;
    }
    switch (h->state) {
    case HOLD_PREPARING:
        if (mp_hold_start(h) != 1) break;
        if (!InterlockedCompareExchange(&h->job->done, 0, 0)) break;
        mp_prepared();
        break;
    case HOLD_RAISE:
        if (h->finish) mp_busy(h->editor, NULL);   /* cleared in the tick that raises (or puts it back below) */
        if (h->finish ? g_ops.finish_can_ask && !g_ops.finish_can_ask(g_ops.context, h->editor)
                      : !g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
            if (h->finish) mp_busy(h->editor, "Preparing this map's mod content.");
            if (++h->waited > MP_RAISE_TICKS)
                mp_refuse("The map was not published: its question could not be shown. Publish it again.",
                          "the dialog surface never became idle");
            break;
        }
        sh_modpublish_compose_question(text, sizeof text, h->plan.missing_bytes);
        if (h->finish) {
            /* over the publish screens, in the editor's own dialog: the one the game's publish errors use */
            if (!g_ops.finish_ask(g_ops.context, h->editor, MP_QUESTION_TITLE, text, 1)) {
                mp_refuse("The map was not published: its question could not be shown. Publish it again.",
                          "the editor's dialog would not show; nothing is uploaded without an answer");
                break;
            }
        } else {
            h->ticket = g_ops.ask ? g_ops.ask(g_ops.context, MP_GDM_ID, MP_BUTTONS_YES_NO, text) : 0;
            if (h->ticket <= 0) {
                h->ticket = 0;
                mp_refuse("The map was not published: its question could not be shown. Publish it again.",
                          "the dialog would not raise; nothing is uploaded without an answer");
                break;
            }
        }
        h->state = HOLD_ASKING;
        h->waited = 0;
        break;
    case HOLD_ASKING:
        if (h->finish) answer = g_ops.finish_poll(g_ops.context, h->editor);
        else answer = g_ops.poll ? g_ops.poll(g_ops.context, h->ticket) : -1;
        if (answer == MP_DIALOG_PENDING) break;
        if (!h->finish && g_ops.release) g_ops.release(g_ops.context, h->ticket);
        h->ticket = 0;
        if (answer != MP_DIALOG_ACCEPTED) {
            mp_refuse(NULL, "the author chose not to upload");
            break;
        }
        if (!(h->job = mp_job_new(JOB_UPLOAD, NULL, NULL, 0))) {
            mp_refuse("The map was not published: Snapmap+ ran out of memory. Restart DOOM and publish again.",
                      "no memory for the upload job");
            break;
        }
        h->job->plan = h->plan;
        memset(&h->plan, 0, sizeof h->plan);
        h->job->total_bytes = h->job->plan.missing_bytes;
        h->state = HOLD_UPLOADING;
        h->started = 0;
        h->waited = 0;
        h->shown = (unsigned long long)-1;
        sh_modpublish_progress_text(text, sizeof text, 0, h->job->total_bytes);
        mp_busy(h->editor, text);
        /* fall through */
    case HOLD_UPLOADING:
        if (mp_hold_start(h) != 1) break;
        if (InterlockedCompareExchange(&h->job->done, 0, 0)) {
            /* the plan goes back to the hold: the save's units are the prepared ones */
            mp_uploaded();
            break;
        }
        if (++h->waited % MP_PROGRESS_TICKS == 0) {
            unsigned long long done = (unsigned long long)InterlockedCompareExchange64(
                (volatile LONG64 *)&h->job->done_bytes, 0, 0);
            unsigned long long step = h->job->total_bytes / 100 + 1;
            if (h->shown == (unsigned long long)-1 || done / step != h->shown / step) {
                h->shown = done;
                sh_modpublish_progress_text(text, sizeof text, done, h->job->total_bytes);
                mp_busy(h->editor, text);
            }
        }
        break;
    case HOLD_RESUME:
        /* the EditorFrame detour resumes it; without an editor frame (its hook not installed) the tick does */
        if (++h->waited > MP_RESUME_TICKS) {
            mp_log("MODS: publish: no editor frame came to close the publish screens; the tick closes them");
            g_editor_ticking = 1;
            mp_resume();
            g_editor_ticking = 0;
        }
        break;
    default:
        break;
    }
}

static void mp_poll_background(void)
{
    mp_job *job;
    /* Harvest the finished background job (the queue head while it ran). */
    AcquireSRWLockExclusive(&g_queue_lock);
    job = g_queue;
    if (job && InterlockedCompareExchange(&job->done, 0, 0)) g_queue = job->next;
    else job = NULL;
    ReleaseSRWLockExclusive(&g_queue_lock);
    if (job) { mp_job_report(job); mp_job_free(job); }
    /* Start the next one when the worker is free and the hold does not need it. */
    if (g_hold.state == HOLD_PREPARING || g_hold.state == HOLD_UPLOADING || g_running) return;
    AcquireSRWLockShared(&g_queue_lock);
    job = g_queue;
    ReleaseSRWLockShared(&g_queue_lock);
    if (!job || InterlockedCompareExchange(&job->done, 0, 0)) return;
    if (mp_job_start(job) < 0) {
        AcquireSRWLockExclusive(&g_queue_lock);
        g_queue = job->next;
        ReleaseSRWLockExclusive(&g_queue_lock);
        mp_logf("MODS: publish: the %s could not start on the worker; dropped", mp_job_name(job->kind));
        mp_job_free(job);
    }
}

void sh_modpublish_poll(void)
{
    if (!g_ops_set) return;
    if (g_hold.state == HOLD_IDLE && g_ctx.kind == CTX_NONE && !g_queue && !g_orphans && !g_checks && !g_answers &&
        !g_pin.armed)
        return;
    if (!mp_on_main()) return;
    mp_orphans_retire();
    mp_poll_checks();
    mp_poll_hold();
    if (g_pin.armed && GetTickCount64() - g_pin.at > MP_PIN_ARM_MS) {
        g_pin.armed = 0;
        mp_log("MODS: publish: the publish's save was not seen; its rows were not pinned");
    }
    if (g_ctx.kind != CTX_NONE && g_hold.state == HOLD_IDLE &&
        (mp_token(g_ctx.editor) != g_ctx.token || GetTickCount64() - g_ctx.at > MP_CONTEXT_MS)) {
        mp_ctx_clear();
        mp_upload_clear();   /* the publish this context was for is over */
    }
    mp_poll_background();
}

/* ------------------------------------------------------------ tests */
#ifdef SH_MODPUBLISH_TESTING
void sh_modpublish_test_set_ops(const sh_modpublish_ops *ops)
{
    if (ops) { g_ops = *ops; g_ops_set = 1; }
    else { memset(&g_ops, 0, sizeof g_ops); g_ops_set = 0; }
}
void sh_modpublish_test_reset(void)
{
    mp_hold_free();
    mp_ctx_clear();
    mp_upload_clear();
    (void)modnet_job_wait(30000);
    while (g_orphans) { mp_job *j = g_orphans; g_orphans = j->next; mp_job_free(j); }
    while (g_queue) { mp_job *j = g_queue; g_queue = j->next; mp_job_free(j); }
    g_running = NULL;
    mp_save_clear(&g_save);
    InterlockedExchange(&g_resuming, 0);
    memset(&g_tag, 0, sizeof g_tag);
    memset(&g_pin, 0, sizeof g_pin);
    while (g_checks) { mp_check *c = g_checks; g_checks = c->next; free(c); }
    AcquireSRWLockExclusive(&g_answer_lock);
    while (g_answers) { mp_answer *a = g_answers; g_answers = a->next; free(a->items); free(a); }
    ReleaseSRWLockExclusive(&g_answer_lock);
}
int sh_modpublish_test_delete_checks(void)
{
    int n = 0;
    for (const mp_check *c = g_checks; c; c = c->next) n++;
    return n;
}
void sh_modpublish_test_age_delete_checks(unsigned long long ms)
{
    for (mp_check *c = g_checks; c; c = c->next) c->at -= ms;
}
void sh_modpublish_test_set_link_min_age(long long seconds) { g_link_min_age = seconds; }
void sh_modpublish_test_age_tag_arm(unsigned long long ms) { g_tag.at -= ms; }
int sh_modpublish_test_state(void) { return g_hold.state; }
void sh_modpublish_test_set_rows_for(int (*fn)(const char *const *names, size_t count, struct sh_modmanifest_list *out))
{
    g_rows_for = fn ? fn : sh_modmanifest_rows_for;
}
int sh_modpublish_test_pin_armed(void) { return g_pin.armed; }
size_t sh_modpublish_test_context_units(void) { return g_ctx.kind == CTX_ROWS ? g_ctx.nunits : 0; }
int sh_modpublish_test_background_busy(void) { return g_queue != NULL || g_running != NULL; }
void sh_modpublish_test_set_cache_root(const char *root)
{
    free(g_cache_root);
    g_cache_root = root ? sh_text(root) : NULL;
}
#endif

/* ================================================================== engine
 * Everything below binds the seam to DOOM build 13954591 (both renderers; the
 * offsets are equal on both, the addresses are not). Every engine read and
 * call is SEH-guarded; a failed bind installs nothing. Evidence:
 * logs/shared-ghidra/mod-system-n4/ and lane N1a section 5. The byte checks
 * are in modpub_sites.c (sh_modpublish_check_sites). */
#ifndef SH_MODPUBLISH_PURE
#include "signatures.h"
#include "hook.h"
#include "patch.h"
#include "near_page.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "engine_dialog.h"
#include "modpublished.h"   /* sh_modpublished_notice: the one notice queue */
#include "rawmap.h"
#include "modui.h"
#include "editor_frame.h"   /* the slot read the cloud save makes */

#define ENG_EDITOR_IN_SESSION 0x8       /* byte: nonzero inside an editor session */
#define ENG_EDITOR_EDIT       0x204C8   /* idSnapMapEdit*: null outside a session */
#define ENG_EDITOR_SCREEN     0x21088   /* the editor's menu screen (the toast surface) */
#define ENG_SCREEN_RECORD     0x300     /* MapInfo screen: its history record index */
#define ENG_IDSTR_SIZE        0x40      /* idStr is 0x30; native frames reserve 0x30 */
#define ENG_FAIL_TITLE        "#str_snapmaps_publish_fail_title"

typedef void (*eng_editor_fn)(void *);
typedef void (*eng_feed_fn)(void *, const void *);
typedef void (*eng_unpub_fn)(void *, void *, const void *);
typedef void (*eng_copy_fn)(void *);
typedef void (*eng_browser_copy_fn)(void *, unsigned char);
typedef void *(*eng_get1_fn)(void *);
typedef void *(*eng_lookup_fn)(void *, int);
typedef void (*eng_idstr_ctor_fn)(void *, const char *);
typedef void (*eng_object_fn)(void *);
typedef void (*eng_toast_fn)(void *, void *, void *);
typedef void (*eng_state_fn)(void *, int);
typedef void (*eng_deleted_fn)(void *, void *, const void *, const void *);
typedef void (*eng_publish_start_fn)(void *, unsigned char);
typedef void (*eng_busy_fn)(void *, void *, void *);
typedef unsigned (*eng_write_local_fn)(const char *, void *, const char *);
typedef unsigned char (*eng_grow_fn)(void *);
typedef void (*eng_assign_fn)(void *, const void *);
typedef void *(*eng_query_fn)(void *, const char *, const char *, void *, void *);
typedef unsigned char (*eng_gate_fn)(void *, unsigned char, unsigned char);
typedef void (*eng_gdm_show_fn)(void *, const void *);

/* The editor screen's own dialog (sh_modpublish_check_finish): the one the game's publish errors use over the
 * publish screens. */
#define ENG_SCREEN_DIALOG     0x8E8     /* [editor+0x21088]+0x8E8: the dialog */
#define ENG_SCREEN_SCREENS    0xA18     /* [editor+0x21088]+0xA18: the publish screens */
#define ENG_SCREEN_FLAGS      0xA20     /* bit 0x400: the pacifier is up (EditorBusyShow) */
#define ENG_SCREEN_MODAL      0xA2C     /* bit 0: the dialog is up; EditorFrame clears it at the answer */
#define ENG_DIALOG_ANSWERED   0x1D8     /* byte: the show clears it, the input sets it */
#define ENG_DIALOG_ANSWER     0x1DC     /* 0 the first button, 1 the second or Back */
#define ENG_SCREENS_RESULT    0x8E0     /* the screens' pending result, which the MapProperties frame takes */
#define ENG_STATE_OVERWRITE   0x21      /* MapProperties state: the byte PublishStart gets */
#define ENG_DESC_SIZE         0xD0      /* title +0, text +0x30, labels +0x60 and +0x90, kind +0xC0 */
#define ENG_DESC_KIND         0xC0      /* 0 two buttons, 1 one */

/* The edit's tag list (idList<idStr>: data +0, num +8, size +0xC; idStr
 * elements of 0x30), its saved-map ids, and the published-items answer. */
#define ENG_EDIT_TAGS       0x998
#define ENG_EDIT_LOCAL_ID   0x7F8      /* idLocalSavedMapModel at edit+0x7F8: localId idStr first */
#define ENG_MODEL_TAGS      0x1A0      /* idLocalSavedMapModel tags (edit+0x998 is the edit's copy's) */
#define ENG_EDIT_CLOUD_ID   0x9B0      /* its cloudSavedItemId idStr (+0x1B8) */
#define ENG_TAG_STRIDE      0x30
#define ENG_TAGS_MAX        4096
#define ENG_PUBLISHED_STRIDE 0x588     /* idPublishedMapModel in the search answer; id idStr at +0 */

enum { ENG_AFTER_SAVE, ENG_FEED_POST, ENG_UNPUBLISHED, ENG_MAPINFO_COPY, ENG_BROWSER_COPY, ENG_DELETED,
       ENG_PUBLISH_START, ENG_WRITE_LOCAL, ENG_HOOKS };
static const char *const ENG_HOOK_NAMES[ENG_HOOKS] = {
    "PublishAfterSave", "PublishFeedPost", "MapInfoUnpublishDone", "MapInfoCopyToMyMaps", "BrowserCopyToMyMaps",
    "MapInfoDeleteDone", "PublishStart", "WriteLocalSavedMapText" };
static const size_t ENG_STEAL[ENG_HOOKS] = { 15, 15, 14, 19, 16, 14, 15, 20 };

static struct {
    const uint8_t *site[ENG_HOOKS];
    void *original[ENG_HOOKS];
    eng_editor_fn publish_fail;
    eng_toast_fn toast;
    eng_idstr_ctor_fn idstr_ctor;
    eng_object_fn idstr_dtor;
    eng_get1_fn screen_shell, shell_manager;     /* MapInfo: screen+0x20 -> shell, shell -> manager */
    eng_lookup_fn record;                        /* (manager, index) -> history record */
    eng_lookup_fn entry;                         /* browser: (list at +0x38, index) -> entry */
    eng_state_fn browser_state;                  /* browser: the state change its cancel path makes */
    eng_object_fn hide_progress;                 /* manager: hide the request-wait widget */
    eng_grow_fn tags_grow;                       /* idList<idStr> grow, as PublishStart calls it */
    eng_assign_fn idstr_assign;                  /* idStr = idStr */
    eng_query_fn published_query;                /* PublishedPrevQuery */
    eng_busy_fn busy_show;                       /* EditorBusyShow(editor, title, body): the pacifier */
    eng_editor_fn busy_clear;                    /* EditorBusyClear(editor) */
    /* The publish screens' FINISH step (sh_modpublish_check_finish): bound when every byte holds; the gate call is
     * repointed at the relay only while S7 is asked there. */
    int finish_bound;
    sig_result finish_site;                      /* PublishFinishGateCall, for code_patch_call_sig */
    eng_gate_fn gate;                            /* the gate the call reaches natively */
    eng_editor_fn screens_close;                 /* the screens' close the frame calls next */
    eng_gdm_show_fn gdm_show;                    /* the editor dialog's show */
    void *(*event_owner)(void);                  /* the structured log manager */
    void (*event_log)(void *, const char *, const char *);   /* its CreateInfoLog */
    const char *event_name, *event_desc;         /* the frame's "snapeditor.publish.success" and its description */
    const void *cloud_read_ret;                  /* the return address of the cloud-save request's slot read */
    unsigned (*decl_hash)(const char *, long long);   /* id's hash of a map text: a model's declChecksum */
    int (*meta_read)(const char *, void *, int, int); /* ReadLocalSaveMeta, which the request's metadata read calls */
    sh_patch_handle cloud_meta_patch;            /* that call, relayed to eng_cloud_meta_detour */
    uint8_t *finish_relay;                       /* near page: mov r9,rsi (the MapProperties state); jmp detour */
    sh_patch_handle finish_patch;
    DWORD *main_thread;
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

/* A native idStr's text, whole (malloc'd), or NULL when it does not read. */
static char *eng_string_dup(const void *s)
{
    char *out = NULL;
    if (!s) return NULL;
    __try {
        int length = *(const int *)((const uint8_t *)s + 8);
        const char *data = *(const char *const *)((const uint8_t *)s + 0x10);
        if (length < 0 || (length && !data) || !(out = (char *)malloc((size_t)length + 1))) return NULL;
        if (length) memcpy(out, data, (size_t)length);
        out[length] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(out); out = NULL; }
    return out;
}

/* ------------------------------------------------------------ ops */

static void eng_log(void *c, const char *line) { (void)c; backend_log(line); }

static int eng_on_main(void *c)
{
    (void)c;
    __try { return g_eng.main_thread && *g_eng.main_thread && *g_eng.main_thread == GetCurrentThreadId(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int eng_can_ask(void *c) { (void)c; return sh_engine_dialog_can_ask(); }
static int eng_ask(void *c, unsigned gdm, unsigned set, const char *text) { (void)c; return sh_engine_dialog_ask(gdm, set, text); }
static int eng_poll(void *c, int ticket) { (void)c; return sh_engine_dialog_poll(ticket); }
static void eng_release(void *c, int ticket) { (void)c; sh_engine_dialog_release(ticket); }

static unsigned long long eng_editor_token(void *c, void *editor)
{
    (void)c;
    if (!editor) return 0;
    __try {
        if (!((const uint8_t *)editor)[ENG_EDITOR_IN_SESSION]) return 0;
        return *(const unsigned long long *)((const uint8_t *)editor + ENG_EDITOR_EDIT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void eng_publish_fail(void *c, void *editor)
{
    (void)c;
    if (!editor) return;
    __try { g_eng.publish_fail(editor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the engine's PublishFail faulted"); }
}

static void eng_publish_start_detour(void *editor, unsigned char overwrite);

/* The held publish goes on: the engine's PublishStart, through the detour (which passes while resuming and arms the
 * search tag). */
static void eng_start(void *c, void *editor, unsigned char overwrite)
{
    (void)c;
    __try { eng_publish_start_detour(editor, overwrite); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish: the resumed PublishStart raised %08lx",
                    GetExceptionCode());
        backend_log(line);
    }
}

/* The editor's pacifier, as PublishStart raises it ("#str_snap_editor_pacifier_publish_title" over our line); NULL
 * clears it. */
#define ENG_PACIFIER_TITLE "#str_snap_editor_pacifier_publish_title"
static void eng_busy(void *c, void *editor, const char *text)
{
    __declspec(align(16)) uint8_t title[ENG_IDSTR_SIZE], body[ENG_IDSTR_SIZE];
    int built = 0;
    (void)c;
    if (!editor) return;
    if (!text) {
        if (!g_eng.busy_clear) return;
        __try { g_eng.busy_clear(editor); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the pacifier clear faulted"); }
        return;
    }
    if (!g_eng.busy_show) return;
    __try {
        g_eng.idstr_ctor(title, ENG_PACIFIER_TITLE);
        g_eng.idstr_ctor(body, text);
        built = 1;
        g_eng.busy_show(editor, title, body);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the pacifier faulted"); }
    if (built) {
        __try { g_eng.idstr_dtor(body); g_eng.idstr_dtor(title); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

static int eng_placed_names(void *c, char ***names, size_t *count)
{
    (void)c;
    return sh_modui_placed_names(names, count);
}

/* The editor toast PublishFail itself uses, with our text under the engine's
 * own "publish failed" title. */
static void eng_editor_notice(void *c, void *editor, const char *text)
{
    __declspec(align(16)) uint8_t title[ENG_IDSTR_SIZE], body[ENG_IDSTR_SIZE];
    void *screen;
    int built = 0;
    (void)c;
    if (!editor || !text) return;
    __try {
        screen = *(void **)((uint8_t *)editor + ENG_EDITOR_SCREEN);
        if (!screen) return;
        g_eng.idstr_ctor(title, ENG_FAIL_TITLE);
        g_eng.idstr_ctor(body, text);
        built = 1;
        g_eng.toast(screen, title, body);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the editor toast faulted"); }
    if (built) {
        __try { g_eng.idstr_dtor(body); g_eng.idstr_dtor(title); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

/* A file a unit left out that a player's game asked for: told to the store (POST /mods/unit/<hash>/asked, its author
 * path) for the publishers of maps that claim the unit, whose next publish carries it (3.8). On a pool thread: one
 * small request; the game never waits for it. */
typedef struct eng_asked { char hash[MODNET_HASH_HEX + 1]; char *path; } eng_asked;

static DWORD WINAPI eng_asked_send(LPVOID p)
{
    eng_asked *a = (eng_asked *)p;
    modnet_result r;
    int code;
    memset(&r, 0, sizeof r);
    code = modnet_unit_asked(a->hash, a->path, NULL, &r);
    if (code != MODNET_OK) mp_logf("MODS: the store was not told about %s: %s", a->path, r.message ? r.message : "");
    free(a->path);
    free(a);
    return 0;
}

static void eng_report_asked(const char *salt, const char *unit, const char *request, const char *by)
{
    eng_asked *a;
    (void)by;
    /* A stand-in's own name is the product's: never asked of the store (R-LB1). */
    if (!salt || !request || sh_modserve_standin_name(request) || !modnet_valid_hash(unit) ||
        !(a = (eng_asked *)calloc(1, sizeof *a))) return;
    strcpy_s(a->hash, sizeof a->hash, unit);
    a->path = sh_ma_path(salt, request);
    if (!a->path || !QueueUserWorkItem(eng_asked_send, a, WT_EXECUTEDEFAULT)) {
        free(a->path);
        free(a);
    }
}

/* The search tag, appended to a saved-map model's tag list the way
 * PublishStart appends map:dev to the edit's: grow the list when it is full,
 * assign into list[num], num++. Main thread, inside the local save write. */
static int eng_add_tag(void *c, void *model, const char *tag)
{
    __declspec(align(16)) uint8_t text[ENG_IDSTR_SIZE];
    int built = 0, result = -1;
    (void)c;
    if (!model || !tag) return -1;
    __try {
        uint8_t *list = (uint8_t *)model + ENG_MODEL_TAGS;
        int num, size;
        num = *(int *)(list + 8);
        size = *(int *)(list + 0xC);
        if (num < 0 || size < num || num > ENG_TAGS_MAX || (num && !*(uint8_t **)list)) return -1;
        for (int i = 0; i < num; i++) {
            char have[64];
            if (eng_string(*(uint8_t **)list + (size_t)i * ENG_TAG_STRIDE, have, sizeof have) && !_stricmp(have, tag))
                return 0;
        }
        g_eng.idstr_ctor(text, tag);
        built = 1;
        if (num == size && !g_eng.tags_grow(list)) result = -1;
        else if (*(int *)(list + 8) < *(int *)(list + 0xC) && *(uint8_t **)list) {
            num = *(int *)(list + 8);
            g_eng.idstr_assign(*(uint8_t **)list + (size_t)num * ENG_TAG_STRIDE, text);
            *(int *)(list + 8) = num + 1;
            result = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: publish: the tag list write faulted");
        result = -1;
    }
    if (built) {
        __try { g_eng.idstr_dtor(text); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return result;
}

/* Take `tag` out of a tag list (idList<idStr>): the entries after it move
 * down one through the engine's idStr assignment (the one PublishStart uses)
 * and num shrinks by one, leaving the last slot constructed for the list's
 * own destructor. remove 0 only asks. 1 removed (or present), 0 absent, -1
 * unreadable or failed. Main thread. */
static int eng_list_tag(uint8_t *list, const char *tag, int remove)
{
    char have[64];
    int result = -1;
    if (remove && (!InterlockedCompareExchange(&g_eng.ready, 0, 0) || !g_eng.idstr_assign)) return -1;
    __try {
        uint8_t *data = *(uint8_t **)list;
        int num = *(int *)(list + 8), size = *(int *)(list + 0xC), at = -1, i;
        if (num < 0 || size < num || num > ENG_TAGS_MAX || (num && !data)) return -1;
        for (i = 0; i < num && at < 0; i++)
            if (eng_string(data + (size_t)i * ENG_TAG_STRIDE, have, sizeof have) && !_stricmp(have, tag)) at = i;
        if (at < 0) result = 0;
        else if (!remove) result = 1;
        else {
            for (i = at; i < num - 1; i++)
                g_eng.idstr_assign(data + (size_t)i * ENG_TAG_STRIDE, data + (size_t)(i + 1) * ENG_TAG_STRIDE);
            *(int *)(list + 8) = num - 1;
            result = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: a tag list could not be read or written");
        result = -1;
    }
    return result;
}

/* The search tag out of the saved-map model a local save writes (tags at
 * +0x1A0), inside that write. */
static int eng_remove_tag(void *c, void *model, const char *tag)
{
    (void)c;
    if (!model || !tag) return -1;
    return eng_list_tag((uint8_t *)model + ENG_MODEL_TAGS, tag, 1);
}

/* The published edit's saved-map ids, read at the feed post (the cloud save
 * of this publish has completed, so the cloud-saved id is set). */
static int eng_edit_ids(void *c, void *edit, char *cloud, size_t cloud_cap, char *local, size_t local_cap)
{
    int a, b;
    (void)c;
    if (!edit) return 0;
    __try {
        a = eng_string((uint8_t *)edit + ENG_EDIT_CLOUD_ID, cloud, cloud_cap);
        b = eng_string((uint8_t *)edit + ENG_EDIT_LOCAL_ID, local, local_cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return a || b;
}

/* The published-items query's result callbacks: engine callback objects
 * (vtable slot 0 deleting destructor, +8 invoke, +0x10 clone) built here, so
 * the engine's own request context invokes and destroys them. They only post
 * the answer; the main tick acts on it. */
typedef struct eng_callback { void *const *vtable; unsigned ticket; } eng_callback;

static void *eng_cb_destroy(eng_callback *self, unsigned flags) { if (flags & 1) free(self); return self; }
static eng_callback *eng_cb_clone(const eng_callback *self)
{
    eng_callback *copy = malloc(sizeof(*copy));
    if (copy) *copy = *self;
    return copy;
}
static void eng_cb_ok(eng_callback *self, void *context, const void *list, const void *cursor)
{
    const char *ids[MP_ANSWER_ITEMS];
    char (*text)[MP_ID_CAP] = NULL;
    int n = 0, ok = 0;
    (void)context; (void)cursor;
    __try {
        const uint8_t *data = *(const uint8_t *const *)list;
        int num = *(const int *)((const uint8_t *)list + 8);
        if (num >= 0 && (!num || data)) {
            if (num > MP_ANSWER_ITEMS) num = MP_ANSWER_ITEMS;
            text = num ? calloc((size_t)num, sizeof(*text)) : NULL;
            ok = !num || text != NULL;
            for (int i = 0; ok && i < num; i++)
                if (eng_string(data + (size_t)i * ENG_PUBLISHED_STRIDE, text[n], MP_ID_CAP)) { ids[n] = text[n]; n++; }
                else ok = 0;   /* an unreadable entry: no answer rather than a short one */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    sh_modpublish_published_result(self->ticket, ok, ids, ok ? n : 0);
    free(text);
}
static void eng_cb_fail(eng_callback *self, void *context, int code)
{
    (void)context; (void)code;
    sh_modpublish_published_result(self->ticket, 0, NULL, 0);
}
static void *eng_cb_unused(void) { return NULL; }

static void *const ENG_CB_OK_VTABLE[4] = { (void *)eng_cb_destroy, (void *)eng_cb_ok, (void *)eng_cb_clone,
                                           (void *)eng_cb_unused };
static void *const ENG_CB_FAIL_VTABLE[4] = { (void *)eng_cb_destroy, (void *)eng_cb_fail, (void *)eng_cb_clone,
                                             (void *)eng_cb_unused };

static int eng_published_query(void *c, const char *cloud_id, unsigned ticket)
{
    eng_callback *ok = malloc(sizeof(*ok)), *fail = malloc(sizeof(*fail));
    (void)c;
    if (!ok || !fail || !cloud_id || !cloud_id[0]) { free(ok); free(fail); return 0; }
    ok->vtable = ENG_CB_OK_VTABLE;
    ok->ticket = ticket;
    fail->vtable = ENG_CB_FAIL_VTABLE;
    fail->ticket = ticket;
    __try {
        /* The engine owns both objects from here (its context destroys them),
         * also when it answers at once without a signed-in session. */
        (void)g_eng.published_query(NULL, cloud_id, NULL, ok, fail);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: delete: the published-items query faulted");
        return 0;
    }
    return 1;
}


/* The mod system's one notice queue (modpublished.c). */
static void eng_notice(void *c, const char *text) { (void)c; sh_modpublished_notice(text); }

/* GetProfileUsername(idStr *out) (signatures.c): the name the main menu's "Signed in as" shows. Main thread. */
typedef char (*eng_profile_name_fn)(void *idstr);
static eng_profile_name_fn g_profile_name;

static int eng_account_name(void *c, char *out, size_t cap)
{
    __declspec(align(16)) uint8_t text[ENG_IDSTR_SIZE];
    int ok = 0, built = 0;
    (void)c;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!g_profile_name || !g_eng.idstr_ctor || !g_eng.idstr_dtor || !mp_on_main()) return 0;
    __try {
        g_eng.idstr_ctor(text, "");
        built = 1;
        if (g_profile_name(text)) {
            const char *data = *(const char *const *)(text + 0x10);   /* idStr data: inline buffer or heap */
            if (data) strncpy_s(out, cap, data, _TRUNCATE);
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; out[0] = 0; }
    if (built) { __try { g_eng.idstr_dtor(text); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return ok;
}

static void eng_key_made(void *c) { (void)c; sh_modui_note_changed(); }

/* The Snapmap+ window (snapmap_plus_ui_webview.cpp, class SnapmapPlusStudioWebView) of this process, shown and not
 * minimized. */
static int eng_window_open(void *c)
{
    HWND w = NULL;
    (void)c;
    while ((w = FindWindowExW(NULL, w, L"SnapmapPlusStudioWebView", NULL)) != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) return IsWindowVisible(w) && !IsIconic(w);
    }
    return 0;
}

/* ------------------------------------------------------------ the FINISH step's dialog */

static uint8_t *eng_screen(void *editor)
{
    return editor ? *(uint8_t **)((uint8_t *)editor + ENG_EDITOR_SCREEN) : NULL;
}

static int eng_finish_can_ask(void *c, void *editor)
{
    (void)c;
    __try {
        uint8_t *screen = eng_screen(editor);
        if (!screen || !*(void **)(screen + ENG_SCREEN_DIALOG)) return 0;
        return !(*(volatile uint32_t *)(screen + ENG_SCREEN_MODAL) & 1u) &&
               !(*(volatile uint32_t *)(screen + ENG_SCREEN_FLAGS) & 0x400u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* The descriptor the game's own publish errors build (FUN_1411913F0 / FUN_141191470): four idStrs and the kind; the
 * show reads it and the frame's locals destroy it after. 1 when the dialog shows (the show clears the answered flag
 * only once its widget resolves) and +0xA2C bit 0 is set, as each of those errors sets it. */
static int eng_finish_ask(void *c, void *editor, const char *title, const char *text, int yes_no)
{
    __declspec(align(16)) uint8_t desc[ENG_DESC_SIZE];
    int built = 0, shown = 0;
    (void)c;
    if (!editor || !text || !g_eng.gdm_show) return 0;
    memset(desc, 0, sizeof desc);
    __try {
        uint8_t *screen = eng_screen(editor), *dialog;
        if (!screen || !(dialog = *(uint8_t **)(screen + ENG_SCREEN_DIALOG))) return 0;
        g_eng.idstr_ctor(desc, title ? title : ENG_FAIL_TITLE);
        g_eng.idstr_ctor(desc + 0x30, text);
        g_eng.idstr_ctor(desc + 0x60, yes_no ? "#str_online_yes" : "");
        g_eng.idstr_ctor(desc + 0x90, yes_no ? "#str_online_no" : "");
        built = 1;
        *(int *)(desc + ENG_DESC_KIND) = yes_no ? 0 : 1;
        dialog[ENG_DIALOG_ANSWERED] = 1;
        g_eng.gdm_show(dialog, desc);
        if (!dialog[ENG_DIALOG_ANSWERED]) {
            *(volatile uint32_t *)(screen + ENG_SCREEN_MODAL) |= 1u;
            shown = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the editor's dialog faulted"); shown = 0; }
    if (built) {
        __try { g_eng.idstr_dtor(desc + 0x90); g_eng.idstr_dtor(desc + 0x60); g_eng.idstr_dtor(desc + 0x30);
                g_eng.idstr_dtor(desc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (!shown) backend_log("MODS: publish: the editor's dialog did not show");
    return shown;
}

/* 0 while the dialog is up; then 1 for its first button (Yes), 2 for the second or Back, or when it went unanswered. */
static int eng_finish_poll(void *c, void *editor)
{
    (void)c;
    __try {
        uint8_t *screen = eng_screen(editor), *dialog;
        if (!screen || !(dialog = *(uint8_t **)(screen + ENG_SCREEN_DIALOG))) return 2;
        if (*(volatile uint32_t *)(screen + ENG_SCREEN_MODAL) & 1u) return MP_DIALOG_PENDING;
        if (!dialog[ENG_DIALOG_ANSWERED]) return 2;
        return *(volatile int *)(dialog + ENG_DIALOG_ANSWER) == 0 ? MP_DIALOG_ACCEPTED : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
}

static void eng_finish_close(void *c, void *editor)
{
    (void)c;
    if (!editor || !g_eng.screens_close) return;
    __try { g_eng.screens_close(editor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the publish screens' close faulted"); }
}

/* idStructuredLogManager::CreateInfoLog(manager, "snapeditor.publish.success", its description): the structured info
 * log ("mp-cloud-gobbler") the frame writes after PublishStart, with the game's own strings. */
static void eng_finish_done(void *c, void *editor)
{
    (void)c; (void)editor;
    if (!g_eng.event_owner || !g_eng.event_log) return;
    __try { g_eng.event_log(g_eng.event_owner(), g_eng.event_name, g_eng.event_desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the publish success event faulted"); }
}

static void eng_finish_clear(void *c, void *editor)
{
    (void)c;
    __try {
        uint8_t *screen = eng_screen(editor), *screens;
        if (screen && (screens = *(uint8_t **)(screen + ENG_SCREEN_SCREENS)) != NULL)
            *(volatile int *)(screens + ENG_SCREENS_RESULT) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* The cloud save's read of a publish's slot (editor_frame.c's ReadLocalSavedMapText detour, after the read): the text
 * becomes the published copy, with the pinned rows. VK 0x1411BE200 / GL 0x1411BD5D0 read the slot this way, then
 * pack and upload what they read. */
static int eng_live(void);

static void eng_cloud_read(const char *id, void *text, const void *caller)
{
    __declspec(align(16)) uint8_t tmp[ENG_IDSTR_SIZE];
    char *now, *out;
    size_t length = 0;
    /* only the cloud-save request's read: the editor opening the map reads its slot as it is */
    if (!eng_live() || !g_eng.idstr_assign || !g_eng.cloud_read_ret || caller != g_eng.cloud_read_ret) return;
    if (!(now = eng_string_dup(text))) return;
    out = sh_modpublish_cloud_text(id, now, strlen(now), &length);
    free(now);
    if (!out) return;
    /* the request's metadata read of this slot follows: its declChecksum becomes this text's, by the game's own hash */
    if (g_eng.decl_hash) {
        __try { sh_modpublish_cloud_sum_note(id, g_eng.decl_hash(out, (long long)length)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the published copy's checksum faulted"); }
    }
    __try {
        g_eng.idstr_ctor(tmp, out);
        g_eng.idstr_assign(text, tmp);
        g_eng.idstr_dtor(tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the published copy's text could not be set"); }
    HeapFree(GetProcessHeap(), 0, out);
}

static int eng_cloud_ready(void *c)
{
    (void)c;
    return g_eng.cloud_read_ret && g_eng.cloud_meta_patch.live && sh_editor_frame_read_filter_live();
}

/* The cloud-save request's ReadLocalSaveMeta of the slot (relayed at CloudSaveMetaReadCall): read as the game reads
 * it, then, for a published copy, its declChecksum (+0xB8) is the pinned text's, so the uploaded metadata matches the
 * text uploaded with it. */
#define ENG_MODEL_DECL_CHECKSUM 0xB8
static int eng_cloud_meta_detour(const char *id, void *model, int a3, int a4)
{
    int r = g_eng.meta_read(id, model, a3, a4);
    unsigned sum;
    if (r != 0 || !model || !eng_live()) return r;
    __try {
        if (sh_modpublish_cloud_sum_take(id, &sum)) {
            unsigned was = *(unsigned *)((uint8_t *)model + ENG_MODEL_DECL_CHECKSUM);
            *(unsigned *)((uint8_t *)model + ENG_MODEL_DECL_CHECKSUM) = sum;
            if (was != sum)
                backend_log("MODS: publish: the published copy's metadata names its own text's checksum, not the slot's");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the published copy's metadata faulted"); }
    return r;
}

static int eng_cloud_meta_install(const sig_result *site)
{
    uint8_t expect[5], patch[5] = { 0xe8 }, *relay;
    void *handler = (void *)eng_cloud_meta_detour;
    intptr_t distance;
    int32_t relative;
    DWORD old;
    if (!(relay = (uint8_t *)sh_near_page_alloc(site->addr + 5))) return 0;
    memcpy(relay, "\xff\x25\x00\x00\x00\x00", 6);
    memcpy(relay + 6, &handler, 8);
    if (!VirtualProtect(relay, SH_NEAR_PAGE_BYTES, PAGE_EXECUTE_READ, &old)) { VirtualFree(relay, 0, MEM_RELEASE); return 0; }
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    distance = (intptr_t)relay - (intptr_t)(site->addr + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) { VirtualFree(relay, 0, MEM_RELEASE); return 0; }
    relative = (int32_t)distance;
    memcpy(patch + 1, &relative, 4);
    memcpy(expect, (const void *)site->addr, 5);
    if (code_patch_call_sig(site, expect, patch, &g_eng.cloud_meta_patch) != B2_PATCH_OK) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static const sh_modpublish_ops ENG_OPS = {
    .on_main = eng_on_main, .can_ask = eng_can_ask, .ask = eng_ask, .poll = eng_poll, .release = eng_release,
    .editor_token = eng_editor_token, .publish_fail = eng_publish_fail, .start = eng_start, .busy = eng_busy,
    .placed_names = eng_placed_names, .editor_notice = eng_editor_notice, .log = eng_log, .add_tag = eng_add_tag,
    .edit_ids = eng_edit_ids, .published_query = eng_published_query, .remove_tag = eng_remove_tag,
    .notice = eng_notice, .account_name = eng_account_name, .key_made = eng_key_made,
    .window_open = eng_window_open,
    .finish_can_ask = eng_finish_can_ask, .finish_ask = eng_finish_ask, .finish_poll = eng_finish_poll,
    .finish_close = eng_finish_close, .finish_clear = eng_finish_clear, .finish_done = eng_finish_done,
    .cloud_ready = eng_cloud_ready
};

/* ------------------------------------------------------------ detours */

static int eng_live(void)
{
    return InterlockedCompareExchange(&g_eng.ready, 0, 0) != 0;
}

static void eng_after_save_detour(void *editor)
{
    int pass = 1;
    if (eng_live()) {
        __try { pass = sh_modpublish_after_save(editor); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            backend_log("MODS: publish: the publish gate faulted; the publish continues natively");
            pass = 1;
        }
    }
    if (pass) ((eng_editor_fn)g_eng.original[ENG_AFTER_SAVE])(editor);
}

static void eng_feed_post_detour(void *edit, const void *model)
{
    if (eng_live()) {
        char *id = eng_string_dup(model), *revision = eng_string_dup((const uint8_t *)model + 0x30);
        __try {
            if (id && revision) sh_modpublish_feed_post(edit, id, revision);
            else backend_log("MODS: publish: the published model's id did not read; no claim change");
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the feed-post claim faulted"); }
        free(id);
        free(revision);
    }
    ((eng_feed_fn)g_eng.original[ENG_FEED_POST])(edit, model);
}

/* The MapInfo screen's unpublish success: (screen, context, const idStr &id). */
static void eng_unpublished_detour(void *screen, void *context, const void *id_string)
{
    if (eng_live()) {
        char *id = eng_string_dup(id_string);
        __try {
            if (id) sh_modpublish_unpublished(id);
            else backend_log("MODS: unpublish: the item id did not read; no release");
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: unpublish: the release step faulted"); }
        free(id);
    }
    ((eng_unpub_fn)g_eng.original[ENG_UNPUBLISHED])(screen, context, id_string);
}

/* The MapInfo screen's My Maps delete success: (screen, context, const idStr
 * &localId, const idStr &cloudItemId); either id may be empty. */
static void eng_deleted_detour(void *screen, void *context, const void *local_string, const void *cloud_string)
{
    char local[MP_ID_CAP], cloud[MP_ID_CAP];
    if (eng_live()) {
        __try {
            if (!eng_string(local_string, local, sizeof local)) local[0] = 0;
            if (!eng_string(cloud_string, cloud, sizeof cloud)) cloud[0] = 0;
            sh_modpublish_deleted(local, cloud);
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: delete: the claim check faulted"); }
    }
    ((eng_deleted_fn)g_eng.original[ENG_DELETED])(screen, context, local_string, cloud_string);
    if (eng_live()) sh_cache_sweep_async();   /* a map deleted: content only it used goes (5.17) */
}

/* PublishStart(editorLocal, overwrite), the editor's Publish: a map with mods is held while its mod content is
 * prepared (sh_modpublish_publish_start), and the engine's PublishStart runs at the resume. Then the search tag is
 * armed for the edit's local save, which the Save state writes about 60 frames later. */
static void eng_publish_start_detour(void *editor, unsigned char overwrite)
{
    int pass = 1;
    if (eng_live()) {
        __try { pass = sh_modpublish_publish_start(editor, overwrite); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            backend_log("MODS: publish: the publish hold faulted; the publish continues natively");
            pass = 1;
        }
    }
    if (!pass) return;
    ((eng_publish_start_fn)g_eng.original[ENG_PUBLISH_START])(editor, overwrite);
    if (!eng_live()) return;
    __try {
        uint8_t *edit = editor ? *(uint8_t **)((uint8_t *)editor + ENG_EDITOR_EDIT) : NULL;
        char local[MP_ID_CAP];
        if (!edit || !eng_string(edit + ENG_EDIT_LOCAL_ID, local, sizeof local)) local[0] = 0;
        if (edit) sh_modpublish_publish_started(local);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the publish-start step faulted"); }
}

/* The MapProperties frame's gate call, while S7 is asked at FINISH: the game's own checks first (their dialogs and the
 * screens' close are theirs), then the publish is held with the screens up. `state` is the frame's MapProperties state
 * (rsi at the call), whose +0x21 byte PublishStart would get. */
static unsigned char eng_finish_gate_detour(void *editor, unsigned char show, unsigned char back, const uint8_t *state)
{
    unsigned char blocked = g_eng.gate(editor, show, back);
    if (blocked || !eng_live() || !state) return blocked;
    __try {
        if (sh_modpublish_finish_gate(editor, state[ENG_STATE_OVERWRITE])) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: publish: the FINISH hold faulted; the publish continues natively");
    }
    return 0;
}

static const void *eng_call_target(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8) return NULL;
    memcpy(&displacement, p + 1, 4);
    return (const void *)((uintptr_t)g_eng.finish_site.addr + 5 + (intptr_t)displacement);
}

/* The relay the gate call reaches (rel32 from the call): mov r9,rsi; jmp [rip+0] -> the detour. */
static uint8_t *eng_finish_relay(uintptr_t call)
{
    uint8_t *relay = (uint8_t *)sh_near_page_alloc(call + 5);
    void *handler = (void *)eng_finish_gate_detour;
    DWORD old;
    if (!relay) return NULL;
    memcpy(relay, "\x4c\x8b\xce\xff\x25\x00\x00\x00\x00", 9);
    memcpy(relay + 9, &handler, 8);
    if (!VirtualProtect(relay, SH_NEAR_PAGE_BYTES, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 17);
    return relay;
}

/* sh_modpublish_finish_set's patch: the gate call repointed at the relay (on) or given back (off). */
static int eng_finish_patch(int on)
{
    uint8_t expect[5], patch[5] = { 0xe8 };
    intptr_t distance;
    int32_t relative;
    if (!on) {
        if (!g_eng.finish_patch.live) return 1;
        return code_unpatch(&g_eng.finish_patch) == B2_PATCH_OK;
    }
    if (g_eng.finish_patch.live) return 1;
    if (!g_eng.finish_bound) return 0;
    if (!g_eng.finish_relay && !(g_eng.finish_relay = eng_finish_relay(g_eng.finish_site.addr))) return 0;
    distance = (intptr_t)g_eng.finish_relay - (intptr_t)(g_eng.finish_site.addr + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) return 0;
    relative = (int32_t)distance;
    memcpy(patch + 1, &relative, 4);
    __try { memcpy(expect, (const void *)g_eng.finish_site.addr, 5); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (eng_call_target(expect) != (const void *)g_eng.gate) return 0;
    return code_patch_call_sig(&g_eng.finish_site, expect, patch, &g_eng.finish_patch) == B2_PATCH_OK;
}

/* Bind the FINISH step (optional: without it S7 is asked after the screens close, as before). */
static void eng_bind_finish(const sig_result *results, size_t count, const uint8_t *base)
{
    sh_modpublish_finish_sites s;
    sh_modpublish_finish_bound b;
    const char *failed;
    size_t i;
    g_eng.finish_bound = 0;
    s.call = eng_site(results, count, base, "PublishFinishGateCall");
    s.start = g_eng.site[ENG_PUBLISH_START];
    s.gdm_show = eng_site(results, count, base, "EditorGdmShow");
    s.answer = eng_site(results, count, base, "EditorGdmAnswerCall");
    __try { failed = sh_modpublish_check_finish(&s, &b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { failed = "a site's bytes could not be read"; }
    if (failed) {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish: the FINISH step check '%s' did not hold; S7 is "
                    "asked after the publish screens close", failed);
        backend_log(line);
        return;
    }
    for (i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, "PublishFinishGateCall")) g_eng.finish_site = results[i];
    g_eng.gate = (eng_gate_fn)b.gate;
    g_eng.screens_close = (eng_editor_fn)b.close;
    g_eng.gdm_show = (eng_gdm_show_fn)s.gdm_show;
    g_eng.event_owner = (void *(*)(void))b.event_owner;
    g_eng.event_log = (void (*)(void *, const char *, const char *))b.event_log;
    g_eng.event_name = b.event_name;
    g_eng.event_desc = b.event_desc;
    g_eng.finish_bound = 1;
}

/* WriteLocalSavedMapText(localId, model, text): the local save writes the
 * model as the slot's metadata, which the cloud save reads back. */
static unsigned eng_write_local_detour(const char *local_id, void *model, const char *text)
{
    if (eng_live()) {
        __try { (void)sh_modpublish_save_tag(local_id, model); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the search tag step faulted"); }
    }
    return ((eng_write_local_fn)g_eng.original[ENG_WRITE_LOCAL])(local_id, model, text);
}

/* The published model a MapInfo screen shows: what its copy entry reads. */
static const void *eng_mapinfo_model(void *screen)
{
    __try {
        void *shell = *(void **)((uint8_t *)screen + 0x18), *manager;
        const uint8_t *record;
        if (!shell) shell = g_eng.screen_shell(*(void **)((uint8_t *)screen + 0x20));
        manager = g_eng.shell_manager(shell);
        if (!manager) return NULL;
        record = g_eng.record(manager, *(int *)((uint8_t *)screen + ENG_SCREEN_RECORD));
        if (!record || *(const int *)(record + 0xC) != 1 || !*(void *const *)(record + 0x18)) return NULL;
        return *(const uint8_t *const *)(record + 0x18) + 8;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* The published model a browser's copy entry reads. */
static const void *eng_browser_model(void *browser)
{
    __try {
        const uint8_t *record = g_eng.record(*(void **)((uint8_t *)browser + 0x10), *(int *)((uint8_t *)browser + 8));
        const int *entry;
        if (!record || *(const int *)(record + 0xC) != 5) return NULL;
        entry = g_eng.entry((uint8_t *)browser + 0x38, *(int *)browser);
        if (!entry || *entry != 0 || !*(void *const *)((const uint8_t *)entry + 0x28)) return NULL;
        return *(const uint8_t *const *)((const uint8_t *)entry + 0x28) + 8;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static int eng_copy_decide(const void *model)
{
    char *id, *revision;
    int allowed = 1;
    if (!eng_live() || !model) return 1;
    id = eng_string_dup(model);
    revision = eng_string_dup((const uint8_t *)model + 0x30);
    __try {
        if (id && revision) allowed = sh_modpublish_copy_allowed(id, revision);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: copy: the size gate faulted; the copy continues natively");
        allowed = 1;
    }
    free(id);
    free(revision);
    return allowed;
}

static void eng_mapinfo_copy_detour(void *screen)
{
    if (eng_copy_decide(eng_mapinfo_model(screen))) ((eng_copy_fn)g_eng.original[ENG_MAPINFO_COPY])(screen);
}

/* The browser's action handler showed its request-wait widget before the
 * copy entry: a refusal undoes it the way the entry's own cancelled-error
 * path does (OnBranchError for a cancel code). */
static void eng_browser_copy_detour(void *browser, unsigned char flag)
{
    if (eng_copy_decide(eng_browser_model(browser))) {
        ((eng_browser_copy_fn)g_eng.original[ENG_BROWSER_COPY])(browser, flag);
        return;
    }
    __try {
        void *widget = *(void **)((uint8_t *)browser + 0x30);
        *(int *)((uint8_t *)browser + 0x20) = 0;
        if (widget) ((void (*)(void *, int))(*(void ***)widget)[1])(widget, 0);
        g_eng.browser_state(browser, 0xF);
        g_eng.hide_progress(*(void **)((uint8_t *)browser + 0x10));
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: copy: the browser's screen reset faulted"); }
}

static void eng_save_observer(const char *text, size_t length)
{
    __try { sh_modpublish_save_seen(text, length); }
    __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the save record faulted"); }
}

/* ------------------------------------------------------------ install */

static int eng_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *fail, *ctor, *dtor, *start, *query;
    sh_modpublish_sites sites;
    sh_modpublish_bound bound;
    const char *failed;
    uintptr_t thread;
    for (size_t i = 0; i < ENG_HOOKS; i++)
        if (!(g_eng.site[i] = eng_site(results, count, base, ENG_HOOK_NAMES[i]))) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish hook site %s did not resolve cleanly",
                        ENG_HOOK_NAMES[i]);
            backend_log(line);
            return 0;
        }
    fail = eng_site(results, count, base, "PublishFail");
    ctor = eng_site(results, count, base, "IdStrCtor");
    dtor = eng_site(results, count, base, "IdStrDtor");
    start = g_eng.site[ENG_PUBLISH_START];
    query = eng_site(results, count, base, "PublishedPrevQuery");
    thread = glb_resolve(base, "main_thread_id", NULL);
    if (!fail || !ctor || !dtor || !start || !query || !thread) {
        backend_log("MODS: a publish helper (PublishFail, PublishStart, PublishedPrevQuery, IdStrCtor, IdStrDtor, "
                    "main thread) did not resolve");
        return 0;
    }
    sites.fail = fail; sites.copy = g_eng.site[ENG_MAPINFO_COPY]; sites.browser = g_eng.site[ENG_BROWSER_COPY];
    sites.start = start; sites.after_save = g_eng.site[ENG_AFTER_SAVE]; sites.str_ctor = ctor; sites.str_dtor = dtor;
    __try { failed = sh_modpublish_check_sites(&sites, &bound); }
    __except (EXCEPTION_EXECUTE_HANDLER) { failed = "a site's bytes could not be read"; }
    if (failed) {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: the publish hook check '%s' did not hold", failed);
        backend_log(line);
        return 0;
    }
    g_eng.toast = (eng_toast_fn)bound.toast;
    g_eng.screen_shell = (eng_get1_fn)bound.screen_shell;
    g_eng.shell_manager = (eng_get1_fn)bound.shell_manager;
    g_eng.record = (eng_lookup_fn)bound.record;
    g_eng.entry = (eng_lookup_fn)bound.entry;
    g_eng.browser_state = (eng_state_fn)bound.browser_state;
    g_eng.hide_progress = (eng_object_fn)bound.hide_progress;
    g_eng.tags_grow = (eng_grow_fn)bound.tags_grow;
    g_eng.idstr_assign = (eng_assign_fn)bound.idstr_assign;
    g_eng.published_query = (eng_query_fn)query;
    /* The pacifier PublishStart raises; without it the hold shows nothing while it prepares. */
    g_eng.busy_show = (eng_busy_fn)eng_site(results, count, base, "EditorBusyShow");
    g_eng.busy_clear = (eng_editor_fn)eng_site(results, count, base, "EditorBusyClear");
    if (!g_eng.busy_show || !g_eng.busy_clear) {
        g_eng.busy_show = NULL;
        g_eng.busy_clear = NULL;
        backend_log("MODS: publish: the editor's pacifier did not resolve; a publish with mods prepares without it");
    }
    g_eng.publish_fail = (eng_editor_fn)fail;
    /* the account name the publishing key presents (D31); without it the key's requests carry no name */
    g_profile_name = (eng_profile_name_fn)eng_site(results, count, base, "GetProfileUsername");
    if (!g_profile_name) backend_log("MODS: publish: GetProfileUsername did not resolve; the key presents no account name");
    g_eng.idstr_ctor = (eng_idstr_ctor_fn)ctor;
    g_eng.idstr_dtor = (eng_object_fn)dtor;
    g_eng.main_thread = (DWORD *)thread;
    return 1;
}

/* The edit's own tag list (edit+0x998, the model the next local save writes,
 * idList<idStr>). remove 0: 1 when `tag` is present, 0 absent, -1 unreadable.
 * remove 1: the entries after it move down one through the engine's idStr
 * assignment (the one PublishStart uses) and num shrinks by one, leaving the
 * last slot constructed for the list's own destructor: 1 removed, 0 absent,
 * -1 failed. Main thread. */
int sh_modpublish_edit_tag(void *edit, const char *tag, int remove)
{
    if (!edit || !tag) return -1;
    return eng_list_tag((uint8_t *)edit + ENG_EDIT_TAGS, tag, remove);
}

int sh_modpublish_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    void *detours[ENG_HOOKS] = { (void *)eng_after_save_detour, (void *)eng_feed_post_detour,
                                 (void *)eng_unpublished_detour, (void *)eng_mapinfo_copy_detour,
                                 (void *)eng_browser_copy_detour, (void *)eng_deleted_detour,
                                 (void *)eng_publish_start_detour, (void *)eng_write_local_detour };
    if (InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    if (!results || !module_base || !eng_bind(results, count, module_base)) {
        backend_log("MODS: publish hooks are not installed on this build; maps publish natively and their mod "
                    "content is not sent");
        return 0;
    }
    g_ops = ENG_OPS; g_ops_set = 1;
    for (size_t i = 0; i < ENG_HOOKS; i++)
        g_eng.original[i] = hook_prepare((void *)g_eng.site[i], detours[i], ENG_STEAL[i]);
    for (size_t i = 0; i < ENG_HOOKS; i++) if (!g_eng.original[i]) goto failed;
    for (size_t i = 0; i < ENG_HOOKS; i++) if (hook_commit(g_eng.original[i]) != B2_PATCH_OK) goto failed;
    sh_rawmap_set_save_observer(eng_save_observer);
    sh_modmanifest_set_pin(sh_modpublish_pin_save);
    sh_modmanifest_set_notice(sh_modpublished_notice);   /* the version rule's S9 (D23) */
    sh_modmanifest_set_carries(sh_modserve_root_carries);
    sh_cache_set_held(modclaim_held);   /* the sweep keeps what this computer's published maps claim */
    sh_modui_set_key(modclaim_key_action, modclaim_key_status_json);   /* Settings: the key's backup and restore */
   sh_modserve_set_miss_reporter(eng_report_asked);
    /* S7 at FINISH: bound now, the gate call patched only while it is switched on (sh_publish_question) */
    eng_bind_finish(results, count, module_base);
    g_finish_patch = eng_finish_patch;
    {
        /* the published copy carries the pinned rows: made at the cloud-save request's read of the slot */
        const uint8_t *call = eng_site(results, count, module_base, "CloudSaveSlotReadCall");
        const uint8_t *read = eng_site(results, count, module_base, "ReadLocalSavedMapText");
        const char *why;
        __try { why = sh_modpublish_check_cloud_read(call, read); }
        __except (EXCEPTION_EXECUTE_HANDLER) { why = "its bytes could not be read"; }
        if (why) {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish: the cloud-save read check '%s' did not hold; maps "
                        "with mods are not published on this build", why);
            backend_log(line);
        } else g_eng.cloud_read_ret = call + 5;
        if (!why) {
            /* and its metadata read: the uploaded declChecksum is the published copy's own */
            const uint8_t *meta = eng_site(results, count, module_base, "CloudSaveMetaReadCall");
            const uint8_t *create = eng_site(results, count, module_base, "NewSavedMapDeclChecksumCall");
            const uint8_t *local = eng_site(results, count, module_base, "LocalSaveDeclChecksumCall");
            const void *hash = NULL;
            const uint8_t *meta_read = NULL;
            size_t i;
            __try {
                meta_read = meta && meta[0] == 0xe8 ? meta + 5 + *(const int32_t *)(meta + 1) : NULL;
                why = sh_modpublish_check_cloud_meta(meta, meta_read, create, local, &hash);
            } __except (EXCEPTION_EXECUTE_HANDLER) { why = "its bytes could not be read"; }
            for (i = 0; !why && i < count; i++)
                if (results[i].name && !strcmp(results[i].name, "CloudSaveMetaReadCall")) {
                    g_eng.decl_hash = (unsigned (*)(const char *, long long))hash;
                    g_eng.meta_read = (int (*)(const char *, void *, int, int))meta_read;
                    if (!eng_cloud_meta_install(&results[i])) why = "the relay could not be installed";
                    break;
                }
            if (why) {
                char line[200];
                g_eng.cloud_read_ret = NULL;   /* the published copy's metadata would not match: no copy is made */
                _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish: the cloud-save metadata check '%s' did not "
                            "hold; maps with mods are not published on this build", why);
                backend_log(line);
            }
        }
        sh_editor_frame_set_read_filter(eng_cloud_read);
    }
    InterlockedExchange(&g_eng.ready, 1);
    backend_log("MODS: publish hooks installed (mod content sent before a publish starts, units pinned at its save, "
                "search tag, claim at the feed post, release at unpublish and at a delete whose item is no longer "
                "published, copy-to-My-Maps size gate)");
    sh_modpublish_launch();
    return 1;
failed:
    for (size_t i = ENG_HOOKS; i > 0; i--)
        if (g_eng.original[i - 1] && hook_unpatch(g_eng.original[i - 1])) g_eng.original[i - 1] = NULL;
    g_ops_set = 0;
    backend_log("MODS: publish hooks could not be installed; maps publish natively and their mod content is not "
                "sent");
    return 0;
}
#endif

