/* modnet.h -- the backend's HTTP client for the mod store (Contract 4).
 *
 * The store is anonymous and content-addressed: blobs are cooked files named
 * by the SHA-256 of their bytes, bundles are canonical manifests named by the
 * SHA-256 of their text, and claims keep bundles served. Every byte that
 * comes back is verified against the hash that named it before it is
 * returned, so the transport is untrusted.
 *
 * Transport: WinHTTP, one process-wide session. The base URL is the product
 * setting "mods.store" (config.c; default https://api.snapmapplus.com). HTTPS
 * only, TLS 1.2 or newer; plain http is accepted for a loopback host
 * (127.0.0.1, localhost, [::1]) so tests can run a local stub. The system's
 * own proxy configuration is used (none for loopback); redirects are never
 * followed. Every request with a body sends Content-Length. Timeouts: resolve
 * and connect 15 s, send and receive 30 s per operation step. Bodies are never
 * logged; a refusal's short "error" text (sanitised, at most 160 characters)
 * is carried back in the result.
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
#define MODNET_URL_MAX        512
#define MODNET_MAX_QUERY      4096                    /* hashes per /mods/missing request */
#define MODNET_MAX_CLAIM      256                     /* bundles per claim */
#define MODNET_MAX_BLOB       (64ull * 1024u * 1024u)
#define MODNET_MAX_BUNDLE     (1024u * 1024u)
#define MODNET_REF_MAX        64                      /* UTF-16 code units, as the store counts */

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
    MODNET_E_HASH        =  -9,   /* bytes did not hash to their name */
    MODNET_E_TOO_LARGE   = -10,   /* over a cap (local check or 413) */
    MODNET_E_NOT_FOUND   = -11,   /* 404 */
    MODNET_E_FORBIDDEN   = -12,   /* 403: wrong claim secret */
    MODNET_E_CONFLICT    = -13,   /* 409: the store lacks blobs or bundles the request names */
    MODNET_E_REFUSED     = -14,   /* 400, 411, 415, 422: the store refused the content */
    MODNET_E_RATE        = -15,   /* 429 */
    MODNET_E_BLOCKED     = -16,   /* 451: on the takedown list */
    MODNET_E_UNAVAILABLE = -17    /* 5xx */
};

typedef struct modnet_result {
    int  code;                    /* MODNET_OK or a MODNET_E_ value */
    int  status;                  /* HTTP status of the last response, 0 when none arrived */
    int  created;                 /* 201: the store did not have the object before */
    unsigned long long sent;      /* request body bytes written, summed over the call */
    unsigned long long received;  /* response body bytes read, summed over the call */
    unsigned requests;            /* HTTP exchanges made */
    double ms;                    /* wall time of the call */
    char message[200];            /* short cause, never a body */
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
 * else the default). Returns 1 when out holds it. */
int  modnet_base_url(char *out, size_t cap);
/* Override the base URL for this process (tests; NULL or "" restores the
 * setting). Waits for requests in flight and drops the pooled session.
 * Returns 1 when the URL is usable. */
int  modnet_set_base_url(const char *url);
/* Close the pooled session (process detach is not required to call it). */
void modnet_shutdown(void);

/* 1 when `text` is exactly 64 lowercase hex characters. */
int  modnet_valid_hash(const char *text);
/* 1 when `ref` is a claim reference the store accepts: 1-64 UTF-16 code units
 * of valid UTF-8 without control characters. */
int  modnet_valid_ref(const char *ref);

/* POST /mods/missing for any number of hashes (split into requests of 4096).
 * *missing receives a malloc'd array of the hashes the store lacks (in the
 * order given, duplicates removed); free with free(). Returns the code. */
int  modnet_missing(const char *const *hashes, int n, char (**missing)[MODNET_HASH_HEX + 1], int *missing_count,
                    const modnet_io *io, modnet_result *r);

/* PUT /mods/blob/<sha>. The bytes are hashed first and refused locally when
 * they do not match. 200 (already stored) and 201 (created) are success;
 * r->created tells them apart. */
int  modnet_put_blob(const char *sha, const void *bytes, size_t len, const modnet_io *io, modnet_result *r);

/* GET /mods/blob/<sha>. *bytes (malloc'd) is returned only when its SHA-256
 * equals sha; expected_size, when nonzero, must also match. */
int  modnet_get_blob(const char *sha, unsigned long long expected_size, unsigned char **bytes, size_t *len,
                     const modnet_io *io, modnet_result *r);

/* PUT /mods/bundle/<hash> with the exact canonical manifest text (whose
 * SHA-256 must be hash; checked locally first). 409 means the store lacks
 * blobs the manifest names (renegotiate and retry). */
int  modnet_put_bundle(const char *hash, const char *canonical, size_t len, const modnet_io *io, modnet_result *r);

/* GET /mods/bundle/<hash>. *text (malloc'd, NUL-terminated) is the canonical
 * manifest without "bundle", returned only when its SHA-256 equals hash. A
 * stored body that carries "bundle" is accepted when removing that member
 * yields the hash. */
int  modnet_get_bundle(const char *hash, char **text, size_t *len, const modnet_io *io, modnet_result *r);

/* POST /mods/claim. secret NULL makes a new claim; a stored secret replaces
 * the bundle set of the claim for this ref (403 when it matches none).
 * claim_out and secret_out receive the claim id and its secret. */
int  modnet_claim(const char *ref, const char *const *bundles, int n, const char *secret,
                  char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1],
                  const modnet_io *io, modnet_result *r);

/* POST /mods/release. 204 is success; 404 (no such claim) and 403 (wrong
 * secret) come back as their codes. */
int  modnet_release(const char *claim, const char *secret, const modnet_io *io, modnet_result *r);

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
