/* modnet.h -- the backend's HTTP client for the mod store (SPEC-distribution section 10).
 *
 * The store keeps units (the closure of one placed object, named by the SHA-256 of its manifest, modunit.h) and their
 * files (blobs, named by their SHA-256); a published map claims the units its rows name (modclaim.h). Reads are
 * anonymous; every write is signed by the publisher's key (modkey.h). The transport is untrusted: the content cache
 * (modcache.h) checks every manifest and blob against the hash a map names before anything serves (I10).
 *
 * Transport: WinHTTP, one process-wide session. The base URL is the product setting "mods.store" (config.c; default
 * https://api.snapmapplus.com). HTTPS only, TLS 1.2 or newer; plain http is accepted for a loopback host (127.0.0.1,
 * localhost, [::1]) so tests can run a local store. The system's own proxy configuration is used (none for loopback);
 * redirects are never followed. Every request with a body sends Content-Length and the User-Agent
 * "snapmap-plus/<version>". Timeouts: resolve and connect 15 s, send and receive 30 s per operation step. Bodies are
 * never logged; a refusal's short "error" text (sanitised, at most 160 characters) is carried back in the result,
 * with the store's Retry-After and whether the name did not resolve (this computer is offline). A route the store has
 * retired answers 410 (MODNET_E_RETIRED: this client is too old), and X-Snapmap-Plus-Minimum names the oldest version
 * the store serves (modnet_newer_needed).
 *
 * Every call blocks; the caller picks the thread. The engine's main thread must never call these. A cancel flag
 * (modnet_io) is checked between chunks and between requests, so cancelling takes effect within one chunk or one
 * timeout.
 *
 * The job helper at the end runs one network job at a time on a worker thread with a progress callback and a cancel
 * flag.
 */
#ifndef SNAPMAP_PLUS_MODNET_H
#define SNAPMAP_PLUS_MODNET_H

#include <stddef.h>

#define MODNET_HASH_HEX       64
#define MODNET_DEFAULT_STORE  "https://api.snapmapplus.com"
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
    /* Not a store answer: content was obtained, but a pointer in it names game bytes this machine does not hold (a
     * different game revision, S4b). No request returns it. */
    MODNET_E_GAME        = -18,
    MODNET_E_RETIRED     = -19,   /* 410: the store no longer serves this version of the product (5.22) */
    MODNET_E_KEY         = -20    /* not a store answer: the publishing key could not be made or used */
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
    unsigned long long retry_after_ms;   /* a busy store's Retry-After, 0 when it gave none */
    int  offline;                 /* the store's name did not resolve: this computer is not connected */
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

/* The product's version as the User-Agent sends it ("dev" for a local build). */
const char *modnet_version(void);
/* a < b: -1, equal 0, a > b: 1, by dot-separated numbers; "dev" is newest. */
int  modnet_version_cmp(const char *a, const char *b);
/* 1 once per process when a store answer named a minimum version newer than this product (D41): the caller says
 * "A newer Snapmap+ is available; run the Snapmap+ installer to update." */
int  modnet_newer_needed(void);
#define MODNET_NEWER_SENTENCE "A newer Snapmap+ is available; run the Snapmap+ installer to update."
#define MODNET_RETIRED_SENTENCE "This version of Snapmap+ cannot use the mod store; run the Snapmap+ installer to update."

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

/* ------------------------------------------------------------ units ---- */
/* The store's unit routes (SPEC-distribution 10.2). A unit is the closure of one placed object, named by the
 * SHA-256 of its manifest (modunit.h); its files are blobs, named by their SHA-256. Reads are anonymous; every
 * write is signed by the publisher's key (modkey.h) through a signer. The bytes are untrusted: the cache
 * (modcache.h) checks every manifest and blob against the hash a map names before anything serves (I10). */

#define MODNET_UNITS_MAX        4096                          /* units one status request names */
#define MODNET_ONE_REQUEST_MAX  (95ull * 1024u * 1024u)       /* the largest body one PUT takes */

#define MODNET_UNIT_MISSING  0
#define MODNET_UNIT_HELD     1
#define MODNET_UNIT_REMOVED (-1)

/* Signs one write: fills *headers (malloc'd, the X-Publisher-Key, -Name and -Signature header lines, each ending in
 * CRLF) for `method` `route` whose body's SHA-256 is `body_sha` (64 lowercase hex). 1 on success. */
typedef struct modnet_signer {
    int (*sign)(void *context, const char *method, const char *route, const char *body_sha, wchar_t **headers);
    void *context;
} modnet_signer;

/* POST /mods/units: each hash's state (MODNET_UNIT_*), in batches of MODNET_UNITS_MAX. */
int  modnet_unit_status(const char *const *hashes, size_t n, int *states, const modnet_io *io, modnet_result *r);
/* GET /mods/blob/<sha> into `out_path`, resuming after the `have` bytes the file holds (Range). Not checked here. */
int  modnet_get_blob(const char *sha, const char *out_path, unsigned long long have, const modnet_io *io, modnet_result *r);
/* GET /mods/unit/<hash>/archive (a ZIP of unit.json and every file, or with `mask`, ?want=<base64url>, the files whose
 * bit is set: bit i, byte i >> 3, value 1 << (i & 7), the manifest's file i; `mask_len` bytes, ceil(files / 8)),
 * resumed the same way. A unit arrives in this one request (R-DS6). */
int  modnet_get_unit_archive(const char *hash, const unsigned char *mask, size_t mask_len, const char *out_path,
                             unsigned long long have, const modnet_io *io, modnet_result *r);
/* POST /mods/unit/<hash>/begin with the manifest: *held 1, or the blobs the store lacks in *missing (malloc'd). */
int  modnet_begin_unit(const char *hash, const char *manifest, size_t len, const modnet_signer *signer, int *held,
                       char (**missing)[MODNET_HASH_HEX + 1], size_t *nmissing, const modnet_io *io, modnet_result *r);
/* PUT /mods/unit/<hash>/archive: a ZIP (modzip.h) of unit.json and missing files, at most MODNET_ONE_REQUEST_MAX
 * bytes, signed over its SHA-256. *held 1 when the store now holds every file of the unit. */
int  modnet_put_unit_archive(const char *hash, const char *path, unsigned long long size, const modnet_signer *signer,
                             int *held, const modnet_io *io, modnet_result *r);
/* One blob larger than one request, in parts: POST .../parts (signed), PUT .../parts/<n> for the parts the store
 * lacks, POST .../complete (signed); a later call resumes. */
int  modnet_put_blob(const char *sha, const char *path, unsigned long long size, const modnet_signer *signer,
                     const modnet_io *io, modnet_result *r);
/* POST /mods/unit/<hash>/complete (signed): MODNET_OK when the store holds every file; MODNET_E_CONFLICT. */
int  modnet_complete_unit(const char *hash, const modnet_signer *signer, const modnet_io *io, modnet_result *r);
/* POST /mods/map/claim {ref, units, secret, rev} (signed, D33): MODNET_OK with the claim's rev in *rev_out (and, when
 * the store answered 451, the units moderation removed in *removed: the claim holds the rest); MODNET_E_CONFLICT with
 * the stored rev in *rev_out when `rev` was stale, or -1 when a unit is missing. */
int  modnet_map_claim(const char *ref, const char *const *units, size_t n, const char *secret, long long rev,
                      const modnet_signer *signer, long long *rev_out, char (**removed)[MODNET_HASH_HEX + 1],
                      size_t *nremoved, const modnet_io *io, modnet_result *r);
/* POST /mods/map/release {ref, secret, rev} (signed): MODNET_OK (204), MODNET_E_NOT_FOUND, MODNET_E_CONFLICT (stale,
 * the stored rev in *rev_out). */
int  modnet_map_release(const char *ref, const char *secret, long long rev, const modnet_signer *signer, long long *rev_out,
                        const modnet_io *io, modnet_result *r);
/* POST /mods/unit/<hash>/asked {path}: a path a player's game asked the unit for that it lacks (anonymous). */
int  modnet_unit_asked(const char *hash, const char *path, const modnet_io *io, modnet_result *r);
/* POST /mods/asked {claims: [{ref, secret}]} (signed): the answer's text in *json (malloc'd). */
int  modnet_asked_units(const char *const *refs, const char *const *secrets, size_t n, const modnet_signer *signer,
                        char **json, const modnet_io *io, modnet_result *r);

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
