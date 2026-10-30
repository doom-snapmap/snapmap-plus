/* modpublish.c -- see modpublish.h.
 *
 * One hold at a time. A hold owns the bundle set of the save it was decided
 * on, the editor it was called with and that editor's live edit object (its
 * token). Its life:
 *
 *   after_save()  the original is skipped; the Save state returns with the
 *                 publish pacifier up, as the engine's own asynchronous chain
 *                 leaves it
 *   RAISE         first publish with mods on this installation: wait until the
 *                 dialog surface is idle, then ask once
 *   ASKING        poll the answer; Yes sets mods.upload_confirmed
 *   WORKER        wait for the modnet worker to be free
 *   UPLOADING     one worker job uploads every bundle and claims them under a
 *                 provisional ref
 *   (resume)      on a main tick, the original PublishAfterSave(editorLocal)
 *
 * A No, a failed upload, a question that could not be shown, or a worker
 * that stayed busy cancels the publish through the engine's PublishFail and
 * an editor toast that says why. The editor leaving abandons the hold
 * silently (there is nothing left to clear); another save abandons it through
 * PublishFail. Nothing was sent to the map service in either case.
 *
 * After a resume, the publish context (the edit token, the bundle set, the
 * provisional ref) waits for PublishFeedPost, which hands the claim to the
 * worker. A context that never sees its feed post (the engine's own publish
 * failed, or the editor left) releases its provisional claim.
 *
 * Background jobs (claims, releases, the launch step) run one at a time on
 * the same worker, in order, whenever it is free.
 *
 * Delete checks: the delete completion queues one check per deleted map with
 * linked, claimed items; the next tick starts the engine's published-items
 * query, whose callbacks post an answer (any thread) that a later tick
 * settles. An unanswered check expires and keeps its claims.
 */
#define _CRT_RAND_S
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
#include "modsync.h"
#include "modstore.h"
#include "mp_json.h"
#include "map_shards.h"
#include "config.h"

#define MP_GDM_ID          0x29u   /* GDM_CONFIRM_VIDEO_CHANGES: the Yes/No shape modpublished.c uses */
#define MP_BUTTONS_YES_NO  6u
#define MP_BUTTONS_ACCEPT  0u
#define MP_RAISE_TICKS     600     /* about ten seconds at 60 Hz */
#define MP_WORKER_TICKS    1200    /* about twenty seconds */
#define MP_SAVE_FRESH_MS   5000ull /* the Save state saves and calls back in one frame */
#define MP_CONTEXT_MS      (15ull * 60ull * 1000ull)
#define MP_TEXT_CAP        256     /* the native dialog body */
#define MP_REF_CAP         (4 * MODNET_REF_MAX + 1)
#define MP_SETTING         "mods.upload_confirmed"
#define MP_ID_CAP          160     /* a saved-map or item id in the links file */

static void mp_link_record(const char *item, const char *cloud, const char *local);
static void mp_link_forget(const char *item);

enum { MP_DIALOG_PENDING = 0, MP_DIALOG_ACCEPTED = 1 };
enum { HOLD_IDLE = 0, HOLD_RAISE, HOLD_ASKING, HOLD_UPLOADING, HOLD_WORKER };
enum { CTX_NONE = 0, CTX_ROWS, CTX_NO_ROWS, CTX_UNKNOWN };
enum { JOB_UPLOAD = 1, JOB_CLAIM, JOB_RELEASE, JOB_LAUNCH };

static sh_modpublish_ops g_ops;
static int g_ops_set;
static volatile LONG g_resuming;

/* ------------------------------------------------------------ logging */

static void mp_log(const char *line)
{
    if (g_ops_set && g_ops.log) g_ops.log(g_ops.context, line);
}

static void mp_logf(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    mp_log(line);
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
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", mb);
}

void sh_modpublish_compose_question(char *out, size_t cap, size_t count, unsigned long long bytes)
{
    char size[32];
    if (!out || !cap) return;
    if (cap > MP_TEXT_CAP) cap = MP_TEXT_CAP;
    if (bytes) sh_modpublish_size_text(size, sizeof size, bytes);
    else strcpy_s(size, sizeof size, "size unknown");
    _snprintf_s(out, cap, _TRUNCATE,
                "This map uses %u mod%s (%s). Upload %s to the mod store so other players can download %s? "
                "(asked once)", (unsigned)count, count == 1 ? "" : "s", size, count == 1 ? "it" : "them",
                count == 1 ? "it" : "them");
}

void sh_modpublish_compose_toast_question(char *out, size_t cap, size_t count, unsigned long long bytes)
{
    char size[32];
    if (!out || !cap) return;
    if (cap > MP_TEXT_CAP) cap = MP_TEXT_CAP;
    if (bytes) sh_modpublish_size_text(size, sizeof size, bytes);
    else strcpy_s(size, sizeof size, "size unknown");
    _snprintf_s(out, cap, _TRUNCATE,
                "Publishing this map uploads its %u mod%s (%s) to the mod store so other players can download %s. "
                "Publish again to agree.", (unsigned)count, count == 1 ? "" : "s", size, count == 1 ? "it" : "them");
}

/* ------------------------------------------------------------ the save record */

typedef struct mp_save {
    int valid;
    unsigned long sequence;
    size_t length;
    ULONGLONG at;
    size_t count;                              /* distinct bundles named */
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    unsigned long long bytes;                  /* their sizes, summed */
} mp_save;

static mp_save g_save;
static volatile LONG g_save_sequence;

static void mp_save_clear(mp_save *s)
{
    free(s->hashes);
    memset(s, 0, sizeof(*s));
}

void sh_modpublish_save_seen(const char *text, size_t length)
{
    sh_modmanifest_list rows;
    unsigned long sequence = (unsigned long)InterlockedIncrement(&g_save_sequence);
    unsigned rejected = 0;
    if (!g_ops_set || InterlockedCompareExchange(&g_resuming, 0, 0)) return;
    if (!mp_on_main()) { g_save.valid = 0; return; }
    mp_save_clear(&g_save);
    g_save.sequence = sequence;
    g_save.length = length;
    g_save.at = GetTickCount64();
    g_save.valid = 1;
    if (!text || !length ||
        !sh_shard_find(text, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) return;
    memset(&rows, 0, sizeof rows);
    __try {
        (void)sh_modmanifest_read(text, length, &rows, &rejected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_save.valid = 0;
        mp_log("MODS: publish: the saved map's manifest rows could not be read");
        return;
    }
    if (rows.count) {
        g_save.hashes = calloc(rows.count, sizeof(*g_save.hashes));
        if (!g_save.hashes) {
            g_save.valid = 0;
            sh_modmanifest_list_free(&rows);
            return;
        }
        for (size_t i = 0; i < rows.count; i++) {
            strcpy_s(g_save.hashes[i], sizeof g_save.hashes[i], rows.rows[i].bundle);
            g_save.bytes += rows.rows[i].size;
        }
        g_save.count = rows.count;
    }
    if (rejected) {
        /* A row the reader refused names a bundle the upload cannot know:
         * publishing would leave players without it. */
        g_save.valid = 0;
        mp_logf("MODS: publish: %u manifest row(s) of the save did not read; the publish will not be gated", rejected);
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
    int tagged;
    if (!g_ops_set || !g_tag.armed || !mp_on_main()) return 0;
    if (GetTickCount64() - g_tag.at > MP_TAG_ARM_MS) { g_tag.armed = 0; return 0; }
    if (g_tag.local[0] && (!local_id || strcmp(local_id, g_tag.local))) return 0;   /* another slot */
    g_tag.armed = 0;
    /* The observer reported this save's text just before its write. */
    if (!g_save.valid || !g_save.count || GetTickCount64() - g_save.at > MP_SAVE_FRESH_MS) return 0;
    if (!g_ops.add_tag || !model) return 0;
    tagged = g_ops.add_tag(g_ops.context, model, SH_MODPUBLISH_TAG);
    if (tagged >= 0) g_tag.tagged_sequence = g_save.sequence;
    if (tagged > 0)
        mp_log("MODS: publish: search tag '" SH_MODPUBLISH_TAG "' written into the saved map's tags (the map needs "
               "mods, so players without Snapmap+ cannot play it)");
    else if (tagged < 0)
        mp_log("MODS: publish: the search tag '" SH_MODPUBLISH_TAG "' could not be written; the map publishes "
               "without it");
    return tagged;
}

/* ------------------------------------------------------------ jobs */

typedef struct mp_job {
    struct mp_job *next;
    int kind;
    char ref[MP_REF_CAP];                      /* upload/claim: the claim ref; release: the ref */
    char provisional[MP_REF_CAP];              /* claim: released after the claim succeeds */
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    int count;
    volatile LONG done;
    int ok, ok2, had_claim;
    unsigned retried, dropped, released;
    int left;
    modsync_stats st, st2;
} mp_job;

static mp_job *volatile g_running;     /* the job on the worker, or NULL */
static mp_job *g_queue;                /* background jobs not started yet, in order */
static mp_job *g_orphans;              /* abandoned uploads the worker still owns */
static SRWLOCK g_queue_lock = SRWLOCK_INIT;

static void mp_job_free(mp_job *job)
{
    if (!job) return;
    free(job->hashes);
    SecureZeroMemory(job, sizeof(*job));
    free(job);
}

static mp_job *mp_job_new(int kind, const char *ref, const char (*hashes)[SH_MODMANIFEST_HASH_CAP], int count)
{
    mp_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->kind = kind;
    if (ref) strcpy_s(job->ref, sizeof job->ref, ref);
    if (count > 0) {
        job->hashes = calloc((size_t)count, sizeof(*job->hashes));
        if (!job->hashes) { free(job); return NULL; }
        memcpy(job->hashes, hashes, (size_t)count * sizeof(*job->hashes));
        job->count = count;
    }
    return job;
}

/* Refs of provisional claims this installation still holds: claims and queued
 * uploads named with the provisional prefix (read-only: modsync owns both
 * sections of mods\snapmap-plus.json). Returns a malloc'd array of `*n` refs. */
static char (*mp_provisional_refs(int *n))[MP_REF_CAP]
{
    static const char *const sections[2] = { MODSYNC_CLAIMS_SECTION, MODSYNC_PENDING_SECTION };
    char root[MAX_PATH * 4];
    char (*out)[MP_REF_CAP] = NULL;
    int count = 0, cap = 0;
    size_t prefix = strlen(SH_MODPUBLISH_PENDING_PREFIX);
    *n = 0;
    if (!sh_ms_root(root, sizeof root)) return NULL;
    for (int f = 0; f < 2; f++) {
        char *text = NULL;
        size_t size;
        mpj *doc, *member;
        modsync_store_lock();   /* modsync writes these sections under the store lock */
        if (sh_mb_record_get(root, sections[f], &text) != 1 || !text) { free(text); modsync_store_unlock(); continue; }
        modsync_store_unlock();
        size = strlen(text);
        doc = mpj_parse(text, size);
        SecureZeroMemory(text, size);
        free(text);
        member = doc;
        if (member && member->type == MPJ_OBJ) {
            for (int i = 0; i < member->n; i++) {
                const char *key = member->keys[i];
                int seen = 0;
                if (!key || strncmp(key, SH_MODPUBLISH_PENDING_PREFIX, prefix) || strlen(key) >= MP_REF_CAP ||
                    strchr(key, '\\')) continue;
                for (int k = 0; k < count && !seen; k++) seen = !strcmp(out[k], key);
                if (seen) continue;
                if (count == cap) {
                    int grown = cap ? cap * 2 : 8;
                    void *bigger = realloc(out, (size_t)grown * sizeof(*out));
                    if (!bigger) break;
                    out = bigger;
                    cap = grown;
                }
                strcpy_s(out[count++], MP_REF_CAP, key);
            }
        }
        if (doc) {
            /* The claims document carries secrets; clear its text. */
            if (member && member->type == MPJ_OBJ)
                for (int i = 0; i < member->n; i++) {
                    const mpj *secret = member->items[i] ? mpj_get(member->items[i], "secret") : NULL;
                    if (secret && secret->type == MPJ_STR && secret->text) SecureZeroMemory(secret->text, strlen(secret->text));
                }
            mpj_free(doc);
        }
    }
    *n = count;
    return out;
}

/* Worker thread. Touches only the job. */
static int mp_job_run(void *context, const modnet_io *io)
{
    mp_job *job = context;
    const char *list[MODSYNC_MAX_BUNDLES];
    int n = job->count < MODSYNC_MAX_BUNDLES ? job->count : MODSYNC_MAX_BUNDLES;
    for (int i = 0; i < n; i++) list[i] = job->hashes[i];
    switch (job->kind) {
    case JOB_UPLOAD:
        job->ok = modsync_upload_bundles(list, n, job->ref, io, &job->st);
        break;
    case JOB_CLAIM:
        job->ok = modsync_upload_bundles(list, n, job->ref, io, &job->st);
        if (job->ok && job->provisional[0] && strcmp(job->provisional, job->ref))
            job->ok2 = modsync_release(job->provisional, io, &job->st2);
        break;
    case JOB_RELEASE: {
        char claim[MODNET_CLAIM_HEX + 1];
        job->had_claim = modsync_claim_lookup(job->ref, claim, NULL);
        job->ok = modsync_release(job->ref, io, &job->st);
        break;
    }
    case JOB_LAUNCH: {
        int count = 0;
        char (*refs)[MP_REF_CAP] = mp_provisional_refs(&count);
        for (int i = 0; i < count; i++) {
            modsync_stats st;
            if (io && io->cancel && *io->cancel) break;
            /* A claim is released; a queued upload without one is dropped. */
            (void)modsync_release(refs[i], io, &st);
            job->released++;
        }
        free(refs);
        job->left = modsync_pending_count();
        if (job->left > 0) job->left = modsync_retry_pending(io, &job->retried, &job->dropped, &job->st);
        job->ok = 1;
        break;
    }
    default:
        break;
    }
    return job->ok ? MODNET_OK : (job->st.code ? job->st.code : MODNET_E_ARG);
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
    case JOB_UPLOAD: return "publish upload";
    case JOB_CLAIM: return "publish claim";
    case JOB_RELEASE: return "publish release";
    case JOB_LAUNCH: return "publish launch retry";
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

/* The finished background job: its result lines. */
static void mp_job_report(mp_job *job)
{
    const char *cause = job->st.cause[0] ? job->st.cause : modnet_code_text(job->st.code);
    switch (job->kind) {
    case JOB_CLAIM:
        if (job->ok)
            mp_logf("MODS: publish: claimed %d bundle(s) under item '%s'%s (claim %s, %u blob(s) sent, %.0f ms)",
                    job->count, job->ref, job->st.reclaimed ? ", replacing the previous set" : "", job->st.claim,
                    job->st.blobs_transferred, job->st.ms);
        else
            mp_logf("MODS: publish: the claim under item '%s' failed: %s%s; the map is published, but other "
                    "players cannot download its mods until it succeeds", job->ref, cause,
                    job->st.queued ? " (queued; retried at the next launch)" : "");
        if (job->ok && job->provisional[0] && strcmp(job->provisional, job->ref))
            mp_logf("MODS: publish: provisional claim '%s' %s", job->provisional,
                    job->ok2 ? "released" : "could not be released; it is retried at the next launch");
        else if (!job->ok && job->provisional[0])
            mp_logf("MODS: publish: provisional claim '%s' kept while the item claim is pending", job->provisional);
        break;
    case JOB_RELEASE:
        if (!job->had_claim) mp_logf("MODS: publish: '%s' had no stored claim; nothing to release", job->ref);
        else if (job->ok) mp_logf("MODS: publish: released the claim for '%s'%s%s", job->ref,
                                  job->st.cause[0] ? ": " : "", job->st.cause);
        else mp_logf("MODS: publish: the release of '%s' failed: %s", job->ref, cause);
        if (job->ok || !job->had_claim) mp_link_forget(job->ref);
        break;
    case JOB_LAUNCH:
        mp_logf("MODS: launch: %u provisional ref(s) settled; queued uploads: %u retried, %u dropped, %d left",
                job->released, job->retried, job->dropped, job->left < 0 ? 0 : job->left);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ the hold and the context */

typedef struct mp_hold {
    int state;
    void *editor;
    unsigned long long token;
    unsigned long save_sequence;
    int count;
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    unsigned long long bytes;
    size_t length;
    int ticket;
    int waited;
    mp_job *job;
    char ref[MP_REF_CAP];
} mp_hold;

typedef struct mp_context {
    int kind;
    void *editor;
    unsigned long long token;
    int count;
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    char provisional[MP_REF_CAP];
    ULONGLONG at;
} mp_context;

static mp_hold g_hold;
static mp_context g_ctx;

/* One pending menu notice, raised with a single Accept button when the
 * dialog surface is idle. A later notice replaces an unraised one. */
static char g_notice[MP_TEXT_CAP];
static int g_notice_ticket, g_notice_waited;

static void mp_menu_notice(const char *text)
{
    if (!text || !text[0]) return;
    strcpy_s(g_notice, sizeof g_notice, text);
    g_notice_waited = 0;
    mp_logf("MODS: notice: %s", text);
}

static void mp_editor_notice(void *editor, const char *text)
{
    mp_logf("MODS: publish notice: %s", text);
    if (g_ops.editor_notice) g_ops.editor_notice(g_ops.context, editor, text);
}

static void mp_ctx_clear(int release_provisional, const char *why)
{
    if (release_provisional && g_ctx.provisional[0]) (void)mp_queue_release(g_ctx.provisional, why);
    free(g_ctx.hashes);
    memset(&g_ctx, 0, sizeof g_ctx);
}

static void mp_hold_free(void)
{
    mp_hold *h = &g_hold;
    if (h->ticket && g_ops.release) g_ops.release(g_ops.context, h->ticket);
    /* An upload that ran (or still runs) may have claimed the provisional ref:
     * release it after the job, in order on the worker. */
    if (h->job && h->ref[0] && (!InterlockedCompareExchange(&h->job->done, 0, 0) || h->job->ok))
        (void)mp_queue_release(h->ref, "the held publish ended before the engine resumed");
    if (h->job) {
        if (InterlockedCompareExchange(&h->job->done, 0, 0)) mp_job_free(h->job);
        else {
            if (g_running == h->job) (void)modnet_job_cancel();
            h->job->next = g_orphans;
            g_orphans = h->job;
        }
    }
    free(h->hashes);
    memset(h, 0, sizeof(*h));
}

/* The editor is gone: nothing to clear on its screen. */
static void mp_abandon(const char *why)
{
    if (g_hold.state == HOLD_IDLE) return;
    mp_logf("MODS: publish: the held publish was abandoned: %s", why);
    mp_hold_free();
}

/* The publish is refused: the engine's failure route, then a toast. */
static void mp_cancel(const char *notice, const char *why)
{
    void *editor = g_hold.editor;
    if (g_hold.state == HOLD_IDLE) return;
    mp_logf("MODS: publish: cancelled: %s", why);
    mp_hold_free();
    if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
    if (notice) mp_editor_notice(editor, notice);
}

int sh_modpublish_holding(void)
{
    return g_hold.state != HOLD_IDLE;
}

static void mp_provisional_name(char *out, size_t cap)
{
    unsigned int a = 0, b = 0;
    if (rand_s(&a) || rand_s(&b)) {
        a = (unsigned)GetTickCount64() ^ GetCurrentProcessId();
        b = (unsigned)(uintptr_t)&a ^ (unsigned)time(NULL);
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s%08x%08x", SH_MODPUBLISH_PENDING_PREFIX, a, b);
}

static unsigned long long mp_token(void *editor)
{
    return g_ops.editor_token ? g_ops.editor_token(g_ops.context, editor) : 0;
}

int sh_modpublish_after_save(void *editor)
{
    unsigned long long token;
    mp_hold *h = &g_hold;
    char line[MP_TEXT_CAP], size[32], over[32];
    int confirmed = 0;
    if (!g_ops_set || InterlockedCompareExchange(&g_resuming, 0, 0)) return 1;
    if (!mp_on_main()) return 1;
    token = mp_token(editor);
    if (h->state != HOLD_IDLE) mp_abandon("a newer publish replaced it");
    /* A context still waiting for its feed post belongs to an earlier publish
     * whose engine half never completed. */
    if (g_ctx.kind != CTX_NONE) mp_ctx_clear(1, "an earlier publish never reached its feed post");

    g_ctx.editor = editor;
    g_ctx.token = token;
    g_ctx.at = GetTickCount64();
    if (!g_save.valid || g_ctx.at - g_save.at > MP_SAVE_FRESH_MS) {
        g_ctx.kind = CTX_UNKNOWN;
        mp_log("MODS: publish: the saved text was not seen; the publish continues without the mod check");
        return 1;
    }
    if (!g_save.count) {
        g_ctx.kind = CTX_NO_ROWS;
        return 1;
    }
    memset(&g_ctx, 0, sizeof g_ctx);   /* a held publish builds its context at the resume */
    if (g_save.length > SH_MODPUBLISH_TEXT_BOUND) {
        sh_modpublish_size_text(size, sizeof size, g_save.length);
        sh_modpublish_size_text(over, sizeof over, g_save.length - SH_MODPUBLISH_TEXT_BOUND);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "This map is %s, over the map service's 10.0 MB limit by %s. It was not published.", size, over);
        mp_logf("MODS: publish refused: the saved text is %zu bytes, over the %llu-byte bound", g_save.length,
                SH_MODPUBLISH_TEXT_BOUND);
        if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
        mp_editor_notice(editor, line);
        return 0;
    }
    memset(h, 0, sizeof(*h));
    h->hashes = calloc(g_save.count, sizeof(*h->hashes));
    if (!h->hashes) {
        mp_log("MODS: publish refused: no memory to hold the bundle set");
        if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
        mp_editor_notice(editor, "The map's mods could not be prepared for upload. It was not published.");
        return 0;
    }
    memcpy(h->hashes, g_save.hashes, g_save.count * sizeof(*h->hashes));
    h->count = (int)g_save.count;
    h->bytes = g_save.bytes;
    h->length = g_save.length;
    h->editor = editor;
    h->token = token;
    h->save_sequence = g_save.sequence;
    mp_provisional_name(h->ref, sizeof h->ref);
    (void)sh_config_get_bool(MP_SETTING, &confirmed, NULL);
    h->state = confirmed ? HOLD_WORKER : HOLD_RAISE;
    mp_logf("MODS: publish held: the map uses %d mod(s), %llu bytes%s", h->count, h->bytes,
            confirmed ? "" : "; asking before the first upload");
    if (g_tag.tagged_sequence != g_save.sequence)
        mp_log("MODS: publish: the search tag '" SH_MODPUBLISH_TAG "' was not written with this save; the published "
               "map will not carry it");
    g_tag.armed = 0;
    return 0;
}

void sh_modpublish_feed_post(void *edit, const char *item_id, const char *revision)
{
    mp_job *job;
    if (!g_ops_set || !mp_on_main()) return;
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
        job = mp_job_new(JOB_CLAIM, item_id, (const char (*)[SH_MODMANIFEST_HASH_CAP])g_ctx.hashes, g_ctx.count);
        if (!job) {
            mp_log("MODS: publish: no memory to queue the claim; the provisional claim stays");
            mp_ctx_clear(0, NULL);
            return;
        }
        strcpy_s(job->provisional, sizeof job->provisional, g_ctx.provisional);
        mp_queue_push(job);
        mp_logf("MODS: publish: item '%s' revision '%s' published with %d mod(s); claim queued", item_id,
                revision ? revision : "", g_ctx.count);
        {
            /* The engine keeps no link from the saved map to this item; the
             * delete check needs one. */
            char cloud[MP_ID_CAP] = "", local[MP_ID_CAP] = "";
            if (!g_ops.edit_ids || !g_ops.edit_ids(g_ops.context, edit, cloud, sizeof cloud, local, sizeof local))
                cloud[0] = local[0] = 0;
            mp_link_record(item_id, cloud, local);
        }
        break;
    case CTX_NO_ROWS:
        /* A republish without mods: the item's old set (if any) is no longer
         * needed by its current revision. */
        if (modsync_claim_lookup(item_id, NULL, NULL))
            (void)mp_queue_release(item_id, "the republished map uses no mods");
        break;
    default:
        mp_logf("MODS: publish: item '%s' published without the mod check; no claim change", item_id);
        break;
    }
    mp_ctx_clear(0, NULL);
}

void sh_modpublish_unpublished(const char *item_id)
{
    if (!g_ops_set || !mp_on_main()) return;
    if (!item_id || !modnet_valid_ref(item_id)) {
        mp_logf("MODS: unpublish: the item id '%s' is not a usable claim ref", item_id ? item_id : "");
        return;
    }
    if (!modsync_claim_lookup(item_id, NULL, NULL)) {
        mp_logf("MODS: unpublish: item '%s' has no stored claim; nothing to release", item_id);
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
    char root[MAX_PATH * 4], *text = NULL;
    mpj *doc, *links = NULL;
    if (sh_ms_root(root, sizeof root) && sh_mb_record_get(root, SH_MODPUBLISH_LINKS_SECTION, &text) == 1 && text)
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
    char root[MAX_PATH * 4];
    const mpj *links = doc ? mpj_get(doc, "links") : NULL;
    size_t len = 0;
    char *text = links ? mpj_dump(links, &len) : NULL;
    int ok = text && sh_ms_root(root, sizeof root) && sh_mb_record_put(root, SH_MODPUBLISH_LINKS_SECTION, text);
    free(text);
    return ok;
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
        for (int i = 0; i < count; i++)
            if (item_ids[i]) strncpy_s(a->items[a->count++], MP_ID_CAP, item_ids[i], _TRUNCATE);
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
        char root[MAX_PATH * 4], *text = NULL;
        int has = sh_ms_root(root, sizeof root) && sh_mb_record_get(root, SH_MODPUBLISH_LINKS_SECTION, &text) == 1;
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
        if (!modsync_claim_lookup(item, NULL, NULL)) continue;   /* released already */
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

static char g_cache_root[MAX_PATH * 4];

static int mp_cache_root(char *out, size_t cap)
{
    PWSTR known = NULL;
    char local[MAX_PATH * 4];
    if (g_cache_root[0]) { strcpy_s(out, cap, g_cache_root); return 1; }
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) || !known) return 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, known, -1, local, (int)sizeof local, NULL, NULL)) {
        CoTaskMemFree(known);
        return 0;
    }
    CoTaskMemFree(known);
    _snprintf_s(out, cap, _TRUNCATE, "%s\\id Software\\DOOM\\base\\doomsnapmaps\\published-maps", local);
    return 1;
}

static int mp_plain_id(const char *s)
{
    if (!s || !s[0] || strlen(s) > 128) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

int sh_modpublish_cache_path(const char *item_id, const char *revision, char *out, size_t cap)
{
    char root[MAX_PATH * 4];
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!mp_plain_id(item_id) || !mp_plain_id(revision) || !mp_cache_root(root, sizeof root)) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s_%s_map.decl", root, item_id, revision);
    return 1;
}

int sh_modpublish_copy_allowed(const char *item_id, const char *revision)
{
    char path[MAX_PATH * 4], size[32], over[32], line[MP_TEXT_CAP];
    wchar_t wide[MAX_PATH * 4];
    WIN32_FILE_ATTRIBUTE_DATA data;
    unsigned long long bytes;
    if (!g_ops_set || !mp_on_main()) return 1;
    if (!sh_modpublish_cache_path(item_id, revision, path, sizeof path)) {
        mp_logf("MODS: copy: map '%s' revision '%s' has no cache path; copied natively", item_id ? item_id : "",
                revision ? revision : "");
        return 1;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)(sizeof wide / sizeof wide[0])) ||
        !GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        mp_logf("MODS: copy: the published text of '%s' is not cached; copied natively", item_id);
        return 1;
    }
    bytes = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (bytes <= SH_MODPUBLISH_TEXT_BOUND) return 1;
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
    int provisional = 0, pending = modsync_pending_count();
    char (*refs)[MP_REF_CAP] = mp_provisional_refs(&provisional);
    free(refs);
    if (pending <= 0 && !provisional) { mp_job_free(job); return; }
    if (!job) { mp_log("MODS: launch: no memory to queue the retry"); return; }
    mp_logf("MODS: launch: %d queued upload(s) and %d provisional claim(s) to settle", pending < 0 ? 0 : pending,
            provisional);
    mp_queue_push(job);
}

/* ------------------------------------------------------------ the tick */

static void mp_poll_notice(void)
{
    int answer;
    if (g_notice_ticket) {
        answer = g_ops.poll ? g_ops.poll(g_ops.context, g_notice_ticket) : -1;
        if (answer == MP_DIALOG_PENDING) return;
        if (g_ops.release) g_ops.release(g_ops.context, g_notice_ticket);
        g_notice_ticket = 0;
        return;
    }
    if (!g_notice[0] || g_hold.state == HOLD_RAISE || g_hold.state == HOLD_ASKING) return;
    if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
        if (++g_notice_waited > MP_RAISE_TICKS) {
            mp_log("MODS: the notice could not be shown; the dialog surface stayed busy");
            g_notice[0] = 0;
        }
        return;
    }
    g_notice_ticket = g_ops.ask ? g_ops.ask(g_ops.context, MP_GDM_ID, MP_BUTTONS_ACCEPT, g_notice) : 0;
    if (!g_notice_ticket) mp_log("MODS: the notice dialog would not raise");
    g_notice[0] = 0;
}

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

/* The finished upload: resume the engine or cancel. */
static void mp_upload_finished(void)
{
    mp_hold *h = &g_hold;
    mp_job *job = h->job;
    char line[MP_TEXT_CAP];
    void *editor = h->editor;
    if (!job->ok) {
        const char *cause = job->st.cause[0] ? job->st.cause : modnet_code_text(job->st.code);
        _snprintf_s(line, sizeof line, _TRUNCATE, "The map's mods could not be uploaded (%s). It was not published.",
                    cause);
        mp_logf("MODS: publish: the upload of %d bundle(s) failed after %u request(s): %s%s", h->count,
                job->st.requests, cause, job->st.queued ? " (left in the retry queue)" : "");
        mp_cancel(line, "the upload failed");
        return;
    }
    mp_logf("MODS: publish: uploaded %d bundle(s): %u of %u blob(s) sent, %llu bytes, %u request(s), %.0f ms; "
            "resuming the publish", h->count, job->st.blobs_transferred, job->st.blobs_total, job->st.bytes_up,
            job->st.requests, job->st.ms);
    /* The context the feed post claims from. */
    g_ctx.kind = CTX_ROWS;
    g_ctx.editor = editor;
    g_ctx.token = h->token;
    g_ctx.count = h->count;
    g_ctx.hashes = h->hashes;
    h->hashes = NULL;
    strcpy_s(g_ctx.provisional, sizeof g_ctx.provisional, h->ref);
    g_ctx.at = GetTickCount64();
    mp_job_free(h->job);
    h->job = NULL;
    mp_hold_free();
    InterlockedExchange(&g_resuming, 1);
    if (g_ops.resume) g_ops.resume(g_ops.context, editor);
    InterlockedExchange(&g_resuming, 0);
}

/* The only ask site of the first-time question: the ops route when bound
 * (production: the Yes/No dialog, or the editor-toast alternative), else the
 * plain dialog ask. */
static int mp_upload_question(mp_hold *h, char *text, size_t cap)
{
    if (g_ops.upload_question) return g_ops.upload_question(g_ops.context, h->editor, (size_t)h->count, h->bytes);
    sh_modpublish_compose_question(text, cap, (size_t)h->count, h->bytes);
    return g_ops.ask ? g_ops.ask(g_ops.context, MP_GDM_ID, MP_BUTTONS_YES_NO, text) : 0;
}

static void mp_poll_hold(void)
{
    mp_hold *h = &g_hold;
    char text[MP_TEXT_CAP];
    int answer, rc;
    if (h->state == HOLD_IDLE) return;
    if (mp_token(h->editor) != h->token || !h->token) {
        mp_abandon("the editor left the map");
        return;
    }
    if ((unsigned long)InterlockedCompareExchange(&g_save_sequence, 0, 0) != h->save_sequence) {
        mp_cancel("The map was saved again while its mods were being prepared. Publish it again.",
                  "another save happened");
        return;
    }
    switch (h->state) {
    case HOLD_RAISE:
        if (g_notice_ticket) break;
        if (!g_ops.can_ask || !g_ops.can_ask(g_ops.context)) {
            if (++h->waited > MP_RAISE_TICKS)
                mp_cancel("The mod upload question could not be shown. The map was not published.",
                          "the dialog surface never became idle");
            break;
        }
        h->ticket = mp_upload_question(h, text, sizeof text);
        if (h->ticket == SH_MODPUBLISH_ASKED_BY_TOAST) {
            /* Informed, not asked: the explanation is on screen; the next
             * publish is the agreement. */
            void *editor = h->editor;
            h->ticket = 0;
            if (!sh_config_set_json(MP_SETTING, "true"))
                mp_log("MODS: publish: the upload agreement could not be saved; it will be shown again");
            mp_logf("MODS: publish: cancelled: the first-time upload explanation was shown as a toast");
            mp_hold_free();
            if (g_ops.publish_fail) g_ops.publish_fail(g_ops.context, editor);
            break;
        }
        if (h->ticket <= 0) {
            h->ticket = 0;
            mp_cancel("The mod upload question could not be shown. The map was not published.",
                      "the dialog would not raise; nothing is uploaded without an answer");
            break;
        }
        h->state = HOLD_ASKING;
        h->waited = 0;
        break;
    case HOLD_ASKING:
        answer = g_ops.poll ? g_ops.poll(g_ops.context, h->ticket) : -1;
        if (answer == MP_DIALOG_PENDING) break;
        if (g_ops.release) g_ops.release(g_ops.context, h->ticket);
        h->ticket = 0;
        if (answer != MP_DIALOG_ACCEPTED) {
            mp_cancel("The map was not published: its mods are uploaded only with your agreement.",
                      "the player declined the upload");
            break;
        }
        if (!sh_config_set_json(MP_SETTING, "true"))
            mp_log("MODS: publish: the upload agreement could not be saved; it will be asked again");
        else mp_log("MODS: publish: the player agreed to upload mods at publish (asked once)");
        h->state = HOLD_WORKER;
        /* fall through */
    case HOLD_WORKER:
        if (!h->job) {
            h->job = mp_job_new(JOB_UPLOAD, h->ref, (const char (*)[SH_MODMANIFEST_HASH_CAP])h->hashes, h->count);
            if (!h->job) { mp_cancel("The map's mods could not be uploaded (out of memory). It was not published.",
                                     "no memory for the upload job"); break; }
        }
        rc = mp_job_start(h->job);
        if (rc == 1) {
            h->state = HOLD_UPLOADING;
            mp_logf("MODS: publish: uploading %d bundle(s) under provisional ref '%s'", h->count, h->ref);
            break;
        }
        if (rc < 0 || ++h->waited > MP_WORKER_TICKS)
            mp_cancel("The mod store client is busy. The map was not published; try again in a moment.",
                      rc < 0 ? "the worker could not start" : "the worker stayed busy");
        break;
    case HOLD_UPLOADING:
        if (!InterlockedCompareExchange(&h->job->done, 0, 0)) break;
        mp_upload_finished();
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
    if (g_hold.state == HOLD_WORKER || g_hold.state == HOLD_UPLOADING || g_running) return;
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
    if (g_hold.state == HOLD_IDLE && g_ctx.kind == CTX_NONE && !g_queue && !g_orphans && !g_notice[0] &&
        !g_notice_ticket && !g_checks && !g_answers) return;
    if (!mp_on_main()) return;
    mp_orphans_retire();
    mp_poll_checks();
    mp_poll_hold();
    if (g_ctx.kind != CTX_NONE && g_hold.state == HOLD_IDLE &&
        (mp_token(g_ctx.editor) != g_ctx.token || GetTickCount64() - g_ctx.at > MP_CONTEXT_MS))
        mp_ctx_clear(1, "the publish never reached its feed post");
    mp_poll_background();
    mp_poll_notice();
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
    mp_ctx_clear(0, NULL);
    (void)modnet_job_wait(30000);
    while (g_orphans) { mp_job *j = g_orphans; g_orphans = j->next; mp_job_free(j); }
    while (g_queue) { mp_job *j = g_queue; g_queue = j->next; mp_job_free(j); }
    g_running = NULL;
    mp_save_clear(&g_save);
    g_notice[0] = 0; g_notice_ticket = 0; g_notice_waited = 0;
    InterlockedExchange(&g_resuming, 0);
    memset(&g_tag, 0, sizeof g_tag);
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
const char *sh_modpublish_test_notice(void) { return g_notice; }
const char *sh_modpublish_test_provisional(void)
{
    return g_hold.state != HOLD_IDLE ? g_hold.ref : g_ctx.provisional;
}
int sh_modpublish_test_background_busy(void) { return g_queue != NULL || g_running != NULL; }
void sh_modpublish_test_set_cache_root(const char *root)
{
    if (root) strcpy_s(g_cache_root, sizeof g_cache_root, root);
    else g_cache_root[0] = 0;
}
#endif

/* ================================================================== engine
 * Everything below binds the seam to DOOM build 13954591 (both renderers; the
 * offsets are equal on both, the addresses are not). Every engine read and
 * call is SEH-guarded; a failed bind installs nothing. Evidence:
 * logs/shared-ghidra/mod-system-n4/ and lane N1a section 5. */
#ifndef SH_MODPUBLISH_PURE
#include "signatures.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "engine_dialog.h"
#include "rawmap.h"

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
typedef unsigned (*eng_write_local_fn)(const char *, void *, const char *);
typedef unsigned char (*eng_grow_fn)(void *);
typedef void (*eng_assign_fn)(void *, const void *);
typedef void *(*eng_query_fn)(void *, const char *, const char *, void *, void *);

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

static void *eng_call(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8 && p[0] != 0xe9) return NULL;
    memcpy(&displacement, p + 1, 4);
    return (void *)(p + 5 + displacement);
}

static void *eng_rip(const uint8_t *p, const char *opcode, size_t opcode_length)
{
    int32_t displacement;
    if (memcmp(p, opcode, opcode_length)) return NULL;
    memcpy(&displacement, p + opcode_length, 4);
    return (void *)(p + opcode_length + 4 + displacement);
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

static void eng_resume(void *c, void *editor)
{
    (void)c;
    __try { ((eng_editor_fn)g_eng.original[ENG_AFTER_SAVE])(editor); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[128];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: publish: the resumed PublishAfterSave raised %08lx",
                    GetExceptionCode());
        backend_log(line);
    }
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

/* The first-time upload question, two routes (pick one in ENG_UPLOAD_QUESTION):
 *   dialog  the native Yes/No (GDM 0x29, button set 6) through engine_dialog,
 *           raised over the editor; unverified inside the editor, where a
 *           shell dialog may activate the browser (fault_shield/engine_layout.h)
 *   toast   the editor toast PublishFail itself uses (VK 0x1272760 / GL
 *           0x1271D10 on editor+0x21088): the explanation is shown, this
 *           publish is cancelled, and publishing again is the agreement */
static int eng_upload_question_dialog(void *c, void *editor, size_t count, unsigned long long bytes)
{
    char text[MP_TEXT_CAP];
    (void)c; (void)editor;
    sh_modpublish_compose_question(text, sizeof text, count, bytes);
    return sh_engine_dialog_ask(MP_GDM_ID, MP_BUTTONS_YES_NO, text);
}

static int eng_upload_question_toast(void *c, void *editor, size_t count, unsigned long long bytes)
{
    char text[MP_TEXT_CAP];
    sh_modpublish_compose_toast_question(text, sizeof text, count, bytes);
    eng_editor_notice(c, editor, text);
    return SH_MODPUBLISH_ASKED_BY_TOAST;
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

#define ENG_UPLOAD_QUESTION eng_upload_question_dialog

static const sh_modpublish_ops ENG_OPS = {
    NULL, eng_on_main, eng_can_ask, eng_ask, eng_poll, eng_release, eng_editor_token, eng_publish_fail,
    eng_resume, eng_editor_notice, ENG_UPLOAD_QUESTION, eng_log, eng_add_tag, eng_edit_ids, eng_published_query
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
    char id[MP_REF_CAP], revision[MP_REF_CAP];
    if (eng_live()) {
        __try {
            if (eng_string(model, id, sizeof id) && eng_string((const uint8_t *)model + 0x30, revision, sizeof revision))
                sh_modpublish_feed_post(edit, id, revision);
            else backend_log("MODS: publish: the published model's id did not read; no claim change");
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the feed-post claim faulted"); }
    }
    ((eng_feed_fn)g_eng.original[ENG_FEED_POST])(edit, model);
}

/* The MapInfo screen's unpublish success: (screen, context, const idStr &id). */
static void eng_unpublished_detour(void *screen, void *context, const void *id_string)
{
    char id[MP_REF_CAP];
    if (eng_live()) {
        __try {
            if (eng_string(id_string, id, sizeof id)) sh_modpublish_unpublished(id);
            else backend_log("MODS: unpublish: the item id did not read; no release");
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: unpublish: the release step faulted"); }
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
}

/* PublishStart(editorLocal, overwrite): arm the search tag for the edit's
 * local save, which the Save state writes about 60 frames later. */
static void eng_publish_start_detour(void *editor, unsigned char overwrite)
{
    ((eng_publish_start_fn)g_eng.original[ENG_PUBLISH_START])(editor, overwrite);
    if (!eng_live()) return;
    __try {
        uint8_t *edit = editor ? *(uint8_t **)((uint8_t *)editor + ENG_EDITOR_EDIT) : NULL;
        char local[MP_ID_CAP];
        if (!edit || !eng_string(edit + ENG_EDIT_LOCAL_ID, local, sizeof local)) local[0] = 0;
        if (edit) sh_modpublish_publish_started(local);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: publish: the publish-start step faulted"); }
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
    char id[MP_REF_CAP], revision[MP_REF_CAP];
    if (!eng_live() || !model) return 1;
    __try {
        if (!eng_string(model, id, sizeof id) || !eng_string((const uint8_t *)model + 0x30, revision, sizeof revision))
            return 1;
        return sh_modpublish_copy_allowed(id, revision);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: copy: the size gate faulted; the copy continues natively");
        return 1;
    }
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
    const uint8_t *fail, *copy, *browser, *on_error, *ctor, *dtor, *start, *query;
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
    copy = g_eng.site[ENG_MAPINFO_COPY];
    browser = g_eng.site[ENG_BROWSER_COPY];
    __try {
        /* PublishFail(editorLocal): the toast on editor+0x21088 with two idStr
         * locals, then EditorBusyClear(editor). */
        if (memcmp(fail + 0x27, "\x48\x83\xb9\x88\x10\x02\x00\x00\x74\x50", 10) ||
            eng_call(fail + 0x3D) != (void *)ctor || eng_call(fail + 0x4F) != (void *)ctor ||
            memcmp(fail + 0x54, "\x90\x4c\x8d\x44\x24\x58\x48\x8d\x54\x24\x28\x48\x8b\x8b\x88\x10\x02\x00", 18) ||
            !(g_eng.toast = (eng_toast_fn)eng_call(fail + 0x66)) ||
            eng_call(fail + 0x71) != (void *)dtor || memcmp(fail + 0x81, "\x48\x8b\xcb\xe8", 4)) return 0;
        /* MapInfo copy: shell (+0x18, else from +0x20), manager, the record
         * by index +0x300 of kind 1, and the model at record+0x18 -> +8. */
        if (memcmp(copy + 0x2D, "\x48\x8b\xd9\x48\x8b\x41\x18\x48\x85\xc0\x75\x09\x48\x8b\x49\x20", 16) ||
            !(g_eng.screen_shell = (eng_get1_fn)eng_call(copy + 0x3D)) ||
            memcmp(copy + 0x42, "\x48\x8b\xc8", 3) ||
            !(g_eng.shell_manager = (eng_get1_fn)eng_call(copy + 0x45)) ||
            memcmp(copy + 0x56, "\x8b\x93\x00\x03\x00\x00\x48\x8b\xc8", 9) ||
            !(g_eng.record = (eng_lookup_fn)eng_call(copy + 0x5F)) ||
            memcmp(copy + 0x70, "\x83\x78\x0c\x01", 4) ||
            memcmp(copy + 0x12C, "\x48\x8b\x56\x18\x48\x83\xc2\x08", 8)) return 0;
        /* Browser copy: the same record lookup on (+0x10, +8) of kind 5, the
         * entry lookup on (+0x38, +0), the model at entry+0x28 -> +8, and the
         * cancelled-error path of its OnBranchError (flag 0). */
        if (memcmp(browser + 0x10, "\x8b\x51\x08\x48\x8b\x49\x10", 7) ||
            eng_call(browser + 0x17) != (void *)g_eng.record ||
            memcmp(browser + 0x25, "\x83\x78\x0c\x05", 4) ||
            memcmp(browser + 0x2F, "\x8b\x17\x48\x8d\x4f\x38", 6) ||
            !(g_eng.entry = (eng_lookup_fn)eng_call(browser + 0x43)) ||
            memcmp(browser + 0x9B, "\x48\x8b\x56\x28", 4) ||
            !(on_error = eng_rip(browser + 0x7E, "\x48\x8d\x15", 3))) return 0;
        if (memcmp(on_error + 0x2B, "\x48\x8b\x4b\x30\x33\xf6\x89\x73\x20\x48\x85\xc9\x74\x08\x48\x8b\x01\x33\xd2"
                                    "\xff\x50\x08\xba\x0f\x00\x00\x00\x48\x8b\xcb", 30) ||
            !(g_eng.browser_state = (eng_state_fn)eng_call(on_error + 0x49)) ||
            memcmp(on_error + 0x4E, "\x48\x8b\x4b\x10\x48\x83\xc4\x40\x5f\x5e\x5b", 11) ||
            !(g_eng.hide_progress = (eng_object_fn)eng_call(on_error + 0x59))) return 0;
        /* PublishStart's map:dev append: IdStrCtor at +0x55, the tag list at
         * editor->edit + 0x998, grow when num == size (+0xB3), assign into
         * list[num * 0x30] (+0xD5), num++; its Save callback is PublishAfterSave. */
        if (eng_call(start + 0x55) != (void *)ctor ||
            memcmp(start + 0x5B, "\x49\x8b\xb6\xc8\x04\x02\x00\x48\x81\xc6\x98\x09\x00\x00", 14) ||
            memcmp(start + 0xA8, "\x8b\x46\x0c\x39\x46\x08\x75\x0c\x48\x8b\xce\xe8", 12) ||
            !(g_eng.tags_grow = (eng_grow_fn)eng_call(start + 0xB3)) ||
            memcmp(start + 0xB8, "\x84\xc0\x74\x21\x48\x63\x46\x08\x3b\x46\x0c\x7d\x18\x48\x8d\x0c\x40\x48\xc1\xe1\x04"
                                 "\x48\x03\x0e\x48\x8d\x54\x24\x28\xe8", 30) ||
            !(g_eng.idstr_assign = (eng_assign_fn)eng_call(start + 0xD5)) ||
            memcmp(start + 0xDA, "\xff\x46\x08", 3) ||
            eng_rip(start + 0x1F5, "\x4c\x8d\x05", 3) != (void *)g_eng.site[ENG_AFTER_SAVE] ||
            eng_call(start + 0x140) != (void *)dtor) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_eng.published_query = (eng_query_fn)query;
    g_eng.publish_fail = (eng_editor_fn)fail;
    g_eng.idstr_ctor = (eng_idstr_ctor_fn)ctor;
    g_eng.idstr_dtor = (eng_object_fn)dtor;
    g_eng.main_thread = (DWORD *)thread;
    return 1;
}

int sh_modpublish_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    void *detours[ENG_HOOKS] = { (void *)eng_after_save_detour, (void *)eng_feed_post_detour,
                                 (void *)eng_unpublished_detour, (void *)eng_mapinfo_copy_detour,
                                 (void *)eng_browser_copy_detour, (void *)eng_deleted_detour,
                                 (void *)eng_publish_start_detour, (void *)eng_write_local_detour };
    if (InterlockedCompareExchange(&g_eng.ready, 0, 0)) return 1;
    if (!results || !module_base || !eng_bind(results, count, module_base)) {
        backend_log("MODS: publish hooks are not installed on this build; maps publish natively and their mods "
                    "are not uploaded");
        return 0;
    }
    g_ops = ENG_OPS; g_ops_set = 1;
    for (size_t i = 0; i < ENG_HOOKS; i++)
        g_eng.original[i] = hook_prepare((void *)g_eng.site[i], detours[i], ENG_STEAL[i]);
    for (size_t i = 0; i < ENG_HOOKS; i++) if (!g_eng.original[i]) goto failed;
    for (size_t i = 0; i < ENG_HOOKS; i++) if (hook_commit(g_eng.original[i]) != B2_PATCH_OK) goto failed;
    sh_rawmap_set_save_observer(eng_save_observer);
    InterlockedExchange(&g_eng.ready, 1);
    backend_log("MODS: publish hooks installed (upload at publish, search tag, claim at the feed post, release at "
                "unpublish and at a delete whose item is no longer published, copy-to-My-Maps size gate)");
    sh_modpublish_launch();
    return 1;
failed:
    for (size_t i = ENG_HOOKS; i > 0; i--)
        if (g_eng.original[i - 1] && hook_unpatch(g_eng.original[i - 1])) g_eng.original[i - 1] = NULL;
    g_ops_set = 0;
    backend_log("MODS: publish hooks could not be installed; maps publish natively and their mods are not uploaded");
    return 0;
}
#endif
