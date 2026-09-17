/* Map-embedded package delivery, extraction, consent and load gating.
 * Each variables.string shard carries base64 and an smpkg header. Authored
 * Delivery keys may contain dots; trailing index/count/digest fields delimit
 * transport. New saves use complete-tree keys independent of authored IDs;
 * previous descriptor-ID carriers remain accepted.
 * Prepared saved maps use compiled payload availability. Other entry points
 * and missing-payload consent still match identity and complete source content.
 * Verified missing payloads are staged for native in-game consent and installed
 * as intact authored directories. Runtime registration gates subsequent loads.
 */
#ifndef BACKEND_MAP_PACKAGE_H
#define BACKEND_MAP_PACKAGE_H

#include <stddef.h>
#include <limits.h>
#include "package_compiler.h"
#include "package_legacy.h"
/* Configure before map interception starts; callback owns catalog lifetime. */
void sh_mpkg_set_legacy_reader(sh_package_legacy_reader reader, void *context);

/* Format constants -- keep in lockstep with src/map_package.py. */
#define SH_MPKG_DIGEST_CHARS   16          /* sha256 hexdigest prefix length */
#define SH_MPKG_ID_CAP         128         /* package id buffer (incl NUL) */
#define SH_MPKG_MAX_SHARDS     ((unsigned)INT_MAX / SH_MPKG_SHARD_CHARS + 1u)
#define SH_MPKG_MAX_CHUNK      65536       /* b64 chars per shard (py emits 8192) */
/* Only a representation bound: base64 must fit a signed native idStr. The
 * complete map, including wrappers and existing content, is measured too. */
#define SH_MPKG_MAX_PAYLOAD    ((size_t)(INT_MAX / 4) * 3u)
#define SH_MPKG_SHARD_CHARS    8192        /* b64 chars WRITTEN per shard; 16384 failed
                                              * to deserialize, so this is a measured cap */
#define SH_MPKG_HEADER_CAP     192         /* "smpkg." + id + two indices + digest + NUL */
#define SH_MPKG_ERR_CAP        256

/* One declared package, summarised from the shard headers (the cheap
 * question: what does this map want?). Mirrors py list_packages(). */
typedef struct sh_mpkg_decl {
    char     id[SH_MPKG_ID_CAP];
    char     digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned total;        /* shards expected, from the first-seen header */
    unsigned present;      /* distinct shard indices seen */
    int      consistent;   /* 0: duplicate index or disagreeing (total,digest) */
    int      complete;     /* consistent && present == total */
} sh_mpkg_decl;

/* Scan a rawmap JSON buffer for shard headers, in place, no JSON DOM. A
 * header only counts when it is the string value of a "name" key and
 * matches the full header grammar, so prose that merely contains "smpkg."
 * is ignored. Returns the number of packages found (0 = none; the
 * overwhelmingly common case, cost one substring sweep). Returns SIZE_MAX
 * on allocation failure or insufficient caller capacity; never a partial set. */
size_t sh_mpkg_scan(const char *json, size_t len, sh_mpkg_decl *out, size_t cap);

/* Reassemble one declared package's packed payload from the buffer,
 * verifying the digest. Same refusals as py extract(): inconsistent
 * headers, duplicate shard, incomplete set, bad base64, digest mismatch,
 * not present. Returns a HeapAlloc'd buffer (caller HeapFrees) + *out_len,
 * or NULL with the reason in `err`. */
unsigned char *sh_mpkg_extract(const char *json, size_t len, const char *pkg_id,
                               size_t *out_len, char *err, size_t err_cap);

/* Validate every member, descriptor, path and CRC, then extract into a new
 * staging directory. Refuse existing destinations and never overwrite files.
 * A write failure may leave unpublished staging data, never an installed package.
 */
int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *dest_dir,
                   unsigned *files_out, char *err, size_t err_cap);

/* Private map sources, separate from the installed overrides library. Opening
 * validates every archive and retains complete authored trees. No installation,
 * consent or native activation occurs. A vanilla map has an empty context.
 * Keep the context alive while its provider or save operation uses its paths.
 * Close only after retiring that provider; failure retains the handle for retry.
 */
typedef struct sh_mpkg_context sh_mpkg_context;
sh_mpkg_context *sh_mpkg_context_open(const char *data_root, const char *json, size_t len,
    char *error, size_t capacity);
const char *sh_mpkg_context_root(const sh_mpkg_context *context);
size_t sh_mpkg_context_count(const sh_mpkg_context *context);
int sh_mpkg_context_close(sh_mpkg_context **context);
/* Only after the provider has released this context: transfer temporary-file
 * cleanup to the retry queue. A filesystem lock must not block the next map. */
void sh_mpkg_context_retire(sh_mpkg_context **context);
void sh_mpkg_context_collect(void);

/* Capture authored source identities at startup and retain the installation
 * destination. Exact source rechecks reject removed or changed package trees.
 * Failed capture can be retried after recovery becomes possible. Successful
 * capture is retained. No authored package contains compiler receipts or digests.
 */
void sh_mpkg_boot_capture(const char *data_root);
/* Startup must cancel interrupted installs before compiling any packages. */
int sh_mpkg_startup_ready(void);

/* Main engine thread only. Commit after native activation and allocator-scope
 * restoration, while the previous compiler is retained. Failure must trigger
 * provider recovery, followed by cancellation of the entire new disk group.
 * Authored folders that already existed are never removed. No-op without an
 * installation in flight. Cancellation failure retains its record for retry. */
int sh_mpkg_activation_commit(char *error, size_t capacity);
int sh_mpkg_activation_cancel(void);

/* Return 1 to parse the map, or 0 to refuse it. Verified missing payloads are
 * staged as a complete set for consent on the engine tick; this load remains refused. Session
 * installs require successful runtime registration before a later load can
 * pass. Declining installs nothing; a later explicit map load may ask again.
 */
int sh_mpkg_gate(const char *json, size_t len);
/* Readiness only, for a validated private map whose full compiled payload is
 * already available locally. Reject an outstanding consent/install or failed
 * native registration, without comparing package IDs or authored contents. */
int sh_mpkg_activation_ready(void);
/* Request only the complete delivery owners supplying missing resources.
 * The supplied candidate must match the map's validated authored snapshots.
 * On success this owns the request and calls completion exactly once, possibly
 * synchronously: 1 = disk set published pending caller activation, 0 = declined,
 * -1 = installation failed. It never globally recompiles installed variants.
 * The caller retains its map and activates it before committing installation.
 * Return 0 means no request was accepted and no callback will run. Main thread. */
typedef void (*sh_mpkg_install_completion)(void *context, int outcome);
int sh_mpkg_request_map_install(const char *json, size_t length,
    const sh_package_compilation *candidate, const sh_package_owners *owners,
    sh_mpkg_install_completion completion, void *context, char *error, size_t capacity);
/* Withdraw this caller's pending consent. Completion receives zero. Does not
 * commit or cancel an already published transaction; the caller owns that. */
void sh_mpkg_cancel_map_consent(void *context);

/* Strip package delivery variables after gating. Return an owned NUL-
 * terminated process-heap buffer and out_len, or NULL to keep the original.
 * Shards must stay outside engine map state: 8 KiB variables overflow the
 * playtest's 4 KiB message.
 */
char *sh_mpkg_strip(const char *json, size_t len, size_t *out_len);

/* THE AUTHOR SIDE. Embed `payload` into `json` as shard string
 * variables, replacing any copy of the same package already there.
 * Returns a new NUL-terminated HeapAlloc'd buffer (caller HeapFrees)
 * with *out_len set, or NULL with the reason in `err`.
 *
 * Wire format is the reference implementation's, byte for byte: one
 * snapVarString_t per shard, the header as its name, 8192 base64
 * characters as its initialValue, and variables.allocCount[4] kept
 * equal to the string-variable count -- the engine reads that slot,
 * not the list length. */
char *sh_mpkg_embed(const char *json, size_t len, const char *pkg_id,
                    const unsigned char *payload, size_t payload_len,
                    size_t *out_len, char *err, size_t err_cap);

/* The first 16 hex characters of the payload's sha256 -- the digest
 * that goes in the internal shard header. */
void sh_mpkg_digest16(const unsigned char *payload, size_t len,
                      char out[SH_MPKG_DIGEST_CHARS + 1]);

#ifdef SH_MAP_PACKAGE_TESTING
/* Consent modes: production stages an engine modal; tests decide
 * synchronously.
 */
enum {
    SH_MPKG_CONSENT_PROMPT = -1,   /* production: engine modal on the main-thread tick */
    SH_MPKG_CONSENT_DECLINE = 0,   /* synchronous: user said no */
    SH_MPKG_CONSENT_ACCEPT  = 1    /* synchronous: user said yes -> install */
};
void sh_mpkg_test_set_consent_mode(int mode);
/* Filesystem commit seam; parsing, staging and gating stay real. Called once
 * for the whole set. Prepare runs after each archive is fully extracted. */
typedef int (*sh_mpkg_test_publish_fn)(const char *staging, const char *destination);
void sh_mpkg_test_set_publish(sh_mpkg_test_publish_fn publish);
void sh_mpkg_test_set_prepare(void (*prepare)(const char *package));
void sh_mpkg_test_reset(void);                    /* clear snapshot + session state */
const char *sh_mpkg_test_last_refusal(void);      /* last gate refusal reason, "" if none */
int sh_mpkg_test_session_installed_count(void);
#endif

/* Drive the consent dialog. Call from the engine tick (main thread): the engine
 * modal is raised into, and answered out of, the live dialog queue, neither of
 * which a worker thread may touch. No-op when no package is awaiting consent. */
void sh_mpkg_consent_poll(void);
/* Queue a native acknowledgement from any thread; engine UI runs on tick. */
void sh_mpkg_report_error(const char *message);

#endif /* BACKEND_MAP_PACKAGE_H */
