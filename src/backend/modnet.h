/* modnet.h -- the backend's HTTP client for the mod store (ONE-SHAPE rule 5).
 *
 * The store keeps one object per build: a stored ZIP of the mod folder
 * (mod.json and assets/<request> for every carried file, modzip.h), named by
 * the build hash, at /mods/build/<hash>. Claims keep builds served; a release
 * drops the claim and every build no claim names any more. The transport is
 * untrusted: the downloader (modsync.c) verifies every file of an archive
 * against its mod.json and the build hash against the map's row before
 * anything is installed.
 *
 * Nothing here limits a build's size or how many builds a claim names. One
 * request body is the platform's limit (100 MB on the Worker's plan), and one
 * connection is slower than several, so an archive larger than
 * MODNET_PART_SIZE goes up in parts, MODNET_PART_WORKERS of them at a time,
 * each on its own connection:
 *   POST /mods/build/<hash>/parts {"size"} starts the upload, or resumes the
 *        one of that build and size, answering {"partSize","parts","have"};
 *   PUT  /mods/build/<hash>/parts/<n> (application/octet-stream, X-Build-Size)
 *        sends part n, every part but the last exactly partSize bytes; a part
 *        the store has ("have") is never sent again;
 *   POST /mods/build/<hash>/complete {"size"} joins them, and the store checks
 *        the whole archive by the same rules as a one-request upload.
 * The store's answers say how many scripts a build carries (X-Mod-Scripts on
 * GET and HEAD, "scripts" in an upload's answer). An archive past 4 GiB or
 * 65,535 entries is ZIP64 (modzip.h), which the store reads too.
 *
 * Transport: WinHTTP, one process-wide session. The base URL is the product
 * setting "mods.store" (config.c; default https://api.snapmapplus.com). HTTPS
 * only, TLS 1.2 or newer; plain http is accepted for a loopback host
 * (127.0.0.1, localhost, [::1]) so tests can run a local stub. The system's
 * own proxy configuration is used (none for loopback); redirects are never
 * followed. Every request with a body sends Content-Length. Timeouts: resolve
 * and connect 15 s, send and receive 30 s per operation step. Bodies are never
 * logged; a refusal's short "error" text (sanitised, at most 160 characters)
 * is carried back in the result. An archive is streamed from and to a file,
 * never held whole in memory.
 *
 * Every call blocks; the caller picks the thread. The engine's main thread
 * must never call these. A cancel flag (modnet_io) is checked between chunks
 * and between requests, so cancelling takes effect within one chunk or one
 * timeout.
 *
 * The job helper at the end runs one network job at a time on a worker thread
 * with a progress callback and a cancel flag.
 */
#ifndef SNAPMAP_PLUS_MODNET_H
#define SNAPMAP_PLUS_MODNET_H

#include <stddef.h>

#define MODNET_HASH_HEX       64
#define MODNET_CLAIM_HEX      32
#define MODNET_DEFAULT_STORE  "https://api.snapmapplus.com"
/* The largest archive one request sends; a larger one goes up in parts of
 * the size the store's own answer names (api/mods_store.js PART_SIZE, 16
 * MiB), several at a time. Two parts' worth: below that, parts gain nothing. */
#define MODNET_PART_SIZE      (32ull * 1024u * 1024u)
#define MODNET_PART_WORKERS   4
#define MODNET_ARCHIVE_TYPE   "application/zip"
/* A claim reference (the published item a claim names) has no length of the
 * product's or the store's: any text without control characters. */
#define MODNET_REF_RULE       "the ref must be text without control characters"

/* Result codes. 0 is success; every failure is negative. */
enum {
    MODNET_OK            =   0,
    MODNET_E_ARG         =  -1,   /* a bad argument (hash spelling, ref, size) */
    MODNET_E_NOMEM       =  -2,
    MODNET_E_CONFIG      =  -3,   /* the store URL is unusable */
    MODNET_E_NET         =  -4,   /* resolve, connect, TLS, send or receive failed */
    MODNET_E_TIMEOUT     =  -5,
    MODNET_E_CANCELLED   =  -6,
    MODNET_E_HTTP        =  -7,   /* a status this client does not expect */
    MODNET_E_PROTOCOL    =  -8,   /* a malformed or inconsistent response */
    MODNET_E_HASH        =  -9,   /* an archive did not match the build it was fetched for */
    MODNET_E_TOO_LARGE   = -10,   /* over a cap (local check or 413) */
    MODNET_E_NOT_FOUND   = -11,   /* 404 */
    MODNET_E_FORBIDDEN   = -12,   /* 403: wrong claim secret */
    MODNET_E_CONFLICT    = -13,   /* 409: a claim names a build the store does not hold */
    MODNET_E_REFUSED     = -14,   /* 400, 411, 415, 422: the store refused the content */
    MODNET_E_RATE        = -15,   /* 429 */
    MODNET_E_BLOCKED     = -16,   /* 451: on the takedown list */
    MODNET_E_UNAVAILABLE = -17,   /* 5xx */
    /* Not a store answer: the build was obtained, but a borrow (stock
     * pointer) in it names game bytes this machine does not hold, a different
     * game revision, so it cannot be installed here (DESIGN-additive 2.8,
     * case 4). modsync sets it; no request returns it. */
    MODNET_E_GAME        = -18
};

/* The codes that mean the store could not be asked or did not answer:
 * the network, a timeout, the address, 5xx, 429, an unexpected or malformed
 * response. 1 for those, 0 otherwise (an answer, a refusal, a local error). */
int modnet_unreachable(int code);

typedef struct modnet_result {
    int  code;                    /* MODNET_OK or a MODNET_E_ value */
    int  status;                  /* HTTP status of the last response, 0 when none arrived */
    int  created;                 /* 201: the store did not have the object before */
    unsigned long long sent;      /* request body bytes written, summed over the call */
    unsigned long long received;  /* response body bytes read, summed over the call */
    unsigned requests;            /* HTTP exchanges made */
    double ms;                    /* wall time of the call */
    double check_ms;              /* an upload in parts: the wait for the store to join and check the archive */
    unsigned parts;               /* an upload in parts: parts sent by this call */
    const char *message;          /* the cause, whole (the store's own sentence when it gave one), never a
                                     body; valid until this thread's next modnet call */
} modnet_result;

/* Progress of the current transfer: done of total bytes (total 0 when unknown). */
typedef void (*modnet_progress_fn)(void *context, unsigned long long done, unsigned long long total);

typedef struct modnet_io {
    volatile long *cancel;        /* nonzero cancels; may be NULL */
    modnet_progress_fn progress;  /* may be NULL */
    void *context;
} modnet_io;

/* Short static text for a code ("network error", "not found", ...). */
const char *modnet_code_text(int code);
/* 1 when a later retry could succeed (network, timeout, cancel, rate limit,
 * 5xx, 409, protocol or hash trouble in transit); 0 for refusals that will
 * recur (bad argument, refused content, takedown, wrong secret, caps). */
int  modnet_transient(int code);

/* The store base URL in effect (override, else the "mods.store" setting,
 * else the default), whole (malloc'd; free); NULL when memory runs out. */
char *modnet_base_url(void);
/* Override the base URL for this process (tests; NULL or "" restores the
 * setting). Waits for requests in flight and drops the pooled session.
 * Returns 1 when the URL is usable. */
int  modnet_set_base_url(const char *url);
/* Close the pooled session (process detach is not required to call it). */
void modnet_shutdown(void);

/* 1 when `text` is exactly 64 lowercase hex characters. */
int  modnet_valid_hash(const char *text);
/* 1 when `ref` is a claim reference the store accepts: valid UTF-8 text of
 * any length without control characters (MODNET_REF_RULE). */
int  modnet_valid_ref(const char *ref);

/* HEAD /mods/build/<hash>. MODNET_OK with *size (optional) the archive's
 * bytes and *scripts (optional) the script files it carries, when the store
 * serves it; MODNET_E_NOT_FOUND when it does not (never uploaded, purged, or
 * unclaimed past its hour); MODNET_E_BLOCKED on the takedown list. */
int  modnet_head_build(const char *hash, unsigned long long *size, unsigned *scripts, const modnet_io *io,
                       modnet_result *r);

/* Upload the archive file at `path` (UTF-8, `size` its size), streamed: up
 * to MODNET_PART_SIZE in one PUT /mods/build/<hash> (application/zip), a
 * larger one in parts (above), sending only the parts the store does not
 * hold yet, so a later call resumes an interrupted upload. Progress counts
 * the whole archive. 201 (created) and 200 (already stored) are success;
 * r->created tells them apart. A 422 carries the store's first cause. */
int  modnet_put_build(const char *hash, const char *path, unsigned long long size, const modnet_io *io,
                      modnet_result *r);

/* GET /mods/build/<hash> into the file `out_path` (UTF-8, created or
 * replaced), streamed to disk a chunk at a time, of any size, with progress
 * (done of the declared length). r->received is its size. On any failure the
 * file is removed. The bytes are not checked here: the archive's mod.json
 * names the build, which the caller verifies before anything is installed. */
int  modnet_get_build(const char *hash, const char *out_path, const modnet_io *io, modnet_result *r);

/* POST /mods/claim {"builds":[...],"ref":...,"secret"?}: one claim per
 * published item. secret NULL makes a new claim; a stored secret replaces the
 * build set of the claim for this ref (403 when it matches none). 409 when a
 * build named is not stored. claim_out and secret_out receive the claim id
 * and its secret. */
int  modnet_claim(const char *ref, const char *const *builds, int n, const char *secret,
                  char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1],
                  const modnet_io *io, modnet_result *r);

/* POST /mods/release. 204 is success; 404 (no such claim) and 403 (wrong
 * secret) come back as their codes. The store deletes every build no claim
 * names any more at once. */
int  modnet_release(const char *claim, const char *secret, const modnet_io *io, modnet_result *r);

/* A published map that uses part of a mod carries only the files it was
 * found to need (modslice.h). POST /mods/build/<hash>/asked
 * {"request","by"} tells the store a player's game asked that build for a
 * file it lacks (`by`, the load that asked, may be ""); 204 is success, 404
 * a build the store does not hold or one that is whole. Nothing about the
 * player is sent. */
int  modnet_report_asked(const char *hash, const char *request, const char *by, const modnet_io *io, modnet_result *r);

/* POST /mods/asked {"claim","secret"}: what players asked for of the builds
 * a claim names, {"asked":[{"build","salt","request","by"}...]}. MODNET_OK
 * with the answer's text in *json (malloc'd; free), of any size. */
int  modnet_list_asked(const char *claim, const char *secret, char **json, const modnet_io *io, modnet_result *r);

/* ------------------------------------------------------------ jobs ---- */

/* A job body: runs on the worker thread with the io the helper owns (its
 * cancel flag and the progress callback given to modnet_job_start). Returns
 * the job's result code. */
typedef int  (*modnet_job_fn)(void *context, const modnet_io *io);
/* Called on the worker thread after the body returns, before the helper
 * accepts the next job. */
typedef void (*modnet_job_done_fn)(void *context, int result);

/* Start one job. Returns 1 when started, 0 when a job is already running
 * (nothing is started), -1 when the thread could not be created. `name` is
 * copied (status text). */
int  modnet_job_start(const char *name, modnet_job_fn run, modnet_job_done_fn done,
                      modnet_progress_fn progress, void *progress_context, void *context);
/* Ask the running job to stop. Returns 1 when a job was running. */
int  modnet_job_cancel(void);
/* 1 while a job runs; its name is copied to name when non-NULL. */
int  modnet_job_busy(char *name, size_t cap);
/* Wait for the running job (and its done callback) to finish. Returns 1 when
 * idle within timeout_ms. */
int  modnet_job_wait(unsigned timeout_ms);

#endif /* SNAPMAP_PLUS_MODNET_H */
