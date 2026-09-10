/* Map-embedded package delivery, extraction, consent and load gating.
 *
 * Each variables.string shard stores base64 in initialValue and a header in
 * info.name: smpkg.<pkg>.<idx>.<total>.<digest16>. Package ids use lowercase
 * [a-z0-9_-]; digest16 is a SHA-256 prefix over the ZIP payload. ZIPs may use
 * stored or deflated entries.
 *
 * Gate maps before native parsing because missing content can fault at
 * spawn/render. Match the boot package snapshot by digest, or folded name
 * when no sidecar exists. A differing sidecar rejects a name match. Session
 * installs become eligible after runtime declaration registration succeeds.
 *
 * Verified missing payloads are staged for one engine-modal consent prompt.
 * Installation never overwrites an existing folder. The current load is
 * refused; subsequent loads can proceed after rearm. The caller guards gate
 * faults separately.
 */
#ifndef BACKEND_MAP_PACKAGE_H
#define BACKEND_MAP_PACKAGE_H

#include <stddef.h>

/* Format constants -- keep in lockstep with src/map_package.py. */
#define SH_MPKG_DIGEST_CHARS   16          /* sha256 hexdigest prefix length */
#define SH_MPKG_ID_CAP         100         /* package id buffer (incl NUL) */
#define SH_MPKG_MAX_PACKAGES   16          /* declared packages per map */
#define SH_MPKG_MAX_SHARDS     2048        /* shards per package */
#define SH_MPKG_MAX_CHUNK      65536       /* b64 chars per shard (py emits 8192) */
#define SH_MPKG_MAX_PAYLOAD    (8u * 1024u * 1024u)   /* packed zip bytes */
#define SH_MPKG_SHARD_CHARS    8192        /* b64 chars WRITTEN per shard; 16384 failed
                                              * to deserialize, so this is a measured cap */
#define SH_MPKG_HEADER_CAP     160         /* "smpkg." + id + two indices + digest + NUL */
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
 * overwhelmingly common case, cost one substring sweep). */
size_t sh_mpkg_scan(const char *json, size_t len, sh_mpkg_decl *out, size_t cap);

/* Reassemble one declared package's packed payload from the buffer,
 * verifying the digest. Same refusals as py extract(): inconsistent
 * headers, duplicate shard, incomplete set, bad base64, digest mismatch,
 * not present. Returns a HeapAlloc'd buffer (caller HeapFrees) + *out_len,
 * or NULL with the reason in `err`. */
unsigned char *sh_mpkg_extract(const char *json, size_t len, const char *pkg_id,
                               size_t *out_len, char *err, size_t err_cap);

/* Unpack stored or deflated ZIP entries under dest_dir. Reject unsafe paths
 * and enforce entry, per-file and total-byte limits. extract already verifies
 * payload SHA-256; member CRCs are not rechecked. Returns 1 and files_out, or
 * 0 with err. A failed write may leave partial files, which are never
 * recorded as installed.
 */
int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *dest_dir,
                   unsigned *files_out, char *err, size_t err_cap);

/* Capture the immutable boot-time package list from
 * `<data_root>\overrides` (names via packages.c + any smpkg.digest
 * sidecars). Call once at bootstrap, before the deserialize detour can
 * fire. First capture wins; later calls are ignored. Also records
 * data_root as the install destination root. */
void sh_mpkg_boot_capture(const char *data_root);

/* Return 1 to parse the map, or 0 to refuse it. Verified missing payloads are
 * staged for consent on the engine tick; this load remains refused. Session
 * installs require successful runtime registration before a later load can
 * pass.
 */
int sh_mpkg_gate(const char *json, size_t len);

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
 * that goes in a shard header and in the installed package's sidecar. */
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
void sh_mpkg_test_reset(void);                    /* clear snapshot + session state */
const char *sh_mpkg_test_last_refusal(void);      /* last gate refusal reason, "" if none */
int sh_mpkg_test_session_installed_count(void);
#endif

/* Drive the consent dialog. Call from the engine tick (main thread): the engine
 * modal is raised into, and answered out of, the live dialog queue, neither of
 * which a worker thread may touch. No-op when no package is awaiting consent. */
void sh_mpkg_consent_poll(void);

#endif /* BACKEND_MAP_PACKAGE_H */
