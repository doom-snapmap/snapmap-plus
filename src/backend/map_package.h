/* map_package.h -- map-embedded override packages: the wire format, the install,
 * and the load gate.
 *
 * WHY THIS EXISTS
 * ---------------
 * Overrides never reach players -- publishing uploads only the map file. So a
 * map's mod content travels INSIDE the map, as base64 shards in map-level
 * string variables (`variables.string[]`), each shard's header in the
 * variable's `info.name`:
 *
 *     smpkg.<pkg>.<idx>.<total>.<digest16>
 *
 * where <pkg> is a lowercase [a-z0-9_-]+ package id, <idx>/<total> are the
 * shard index and count, and <digest16> is the first 16 hex chars of the
 * sha256 of the packed payload (a deflate zip of the package folder). The
 * reference implementation of this format is the development repo's
 * `src/map_package.py`; this module is its C consumer. The payload carries
 * only the AUTHORED layer (decls, resource manifests, requirements) -- the
 * heavy game-owned bytes are resolved from the player's own installed
 * archives by the resource bridge, so a whole real package is ~84 KiB.
 *
 * WHY THERE IS A LOAD GATE, AND WHY IT IS MANDATORY
 * -------------------------------------------------
 * A map that references package content WITHOUT the package is FATAL, not
 * degraded: the missing model decl resolves to NULL, the engine hands that
 * NULL to idRenderWorldLocal::AddRenderModel, and the process throws and
 * dies (proven live, twice, campaign dynamic-content-serving W-20). The
 * makeDefault placeholder covers decl resolution, not the render path. So
 * the check CANNOT be an optional prompt shown while the map loads -- the
 * map must be refused BEFORE the engine ever parses it. The single place
 * every map load funnels through -- local save, published offline, and
 * network download alike (W-8) -- is idSnapMap::DeserializeFromJson, which
 * rawmap.c already detours. That detour calls sh_mpkg_gate() on every
 * buffer before handing it to the engine.
 *
 * WHAT "INSTALLED" MEANS: THE BOOT SNAPSHOT, NEVER THE DISK
 * ---------------------------------------------------------
 * The decl server takes an immutable launch snapshot; a package installed
 * on disk AFTER boot is NOT live in the running process, and letting its
 * map through would crash exactly as if it were absent. So the gate
 * compares against a package list captured once at boot
 * (sh_mpkg_boot_capture), plus the set installed by THIS module this
 * session (which are refused with "restart required" rather than passed).
 *
 * A declared package is satisfied when a boot-time package matches it:
 *   - by digest: the package folder carries a `smpkg.digest` sidecar (our
 *     installer writes one) equal to the declared digest; or
 *   - by name, when no sidecar exists (grandfathering hand-installed
 *     packages): the installed name, lowercased and with '/' folded to '-',
 *     equals the declared id, or one ends with "-" + the other (so a map
 *     declaring `demons-cyberdemon` accepts an install at
 *     overrides\cyberdemon or overrides\demons\cyberdemon).
 *   A sidecar that DISAGREES with the declared digest is a version
 *   mismatch and does not satisfy -- the map is refused rather than fed to
 *   an engine holding different content. Generous name matching is safe
 *   because a false "satisfied" merely reproduces today's behavior; the
 *   consent + path checks on INSTALL are the security boundary, not this.
 *
 * FAILURE POLICY: every gate-internal failure degrades to a vanilla load
 * (pass-through) or a clean refusal (DeserializeFromJson returns 0, which
 * the engine's callers already handle as a failed parse -- a browser
 * bounce, proven live in W-18). Never a crash. Installs happen only after
 * explicit user consent, and registration is boot-time only (six failed
 * post-hoc promotion attempts are documented in decl_server.c), so an
 * install always ends in "restart DOOM".
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

/* Unpack a packed payload (a deflate zip) under `dest_dir`, creating it.
 * REFUSES unsafe member paths: absolute, drive-qualified, backslashed, or
 * containing '.' / '..' segments -- a map is untrusted input. Bounded
 * against decompression bombs (entry, per-file and total-size caps).
 * Payload integrity is already sha256-verified by extract, so per-member
 * CRCs are not re-checked. Returns 1 and *files_out on success, else 0
 * with the reason in `err` (a partial unpack may remain; the caller
 * treats the install as failed and never records it as installed). */
int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *dest_dir,
                   unsigned *files_out, char *err, size_t err_cap);

/* Capture the immutable boot-time package list from
 * `<data_root>\overrides` (names via packages.c + any smpkg.digest
 * sidecars). Call once at bootstrap, before the deserialize detour can
 * fire. First capture wins; later calls are ignored. Also records
 * data_root as the install destination root. */
void sh_mpkg_boot_capture(const char *data_root);

/* THE LOAD GATE. Returns 1 = hand the buffer to the engine, 0 = refuse
 * the load (the detour returns 0 to the engine without parsing). On a
 * refusal the reason is logged and, when a missing package's payload
 * verifies, a consent prompt is raised on its own thread offering to
 * install it (never blocking the engine thread); either answer still
 * refuses THIS load, because registration is restart-only. */
int sh_mpkg_gate(const char *json, size_t len);

/* Remove the delivery payload from a map buffer, AFTER the gate has read
 * it. Returns a new NUL-terminated HeapAlloc'd buffer (caller HeapFrees)
 * with *out_len set, or NULL when there is nothing to strip or the
 * document cannot be stripped safely -- either way the caller then uses
 * the original buffer.
 *
 * This is not an optimisation. A shard is 8 KiB and the playtest
 * serialises map variables into a 4 KiB message, so a map that still
 * carries its payload cannot be played at all (measured: idBitMsg
 * overflow, numBits=65544). The payload is an envelope; the engine's map
 * object must never contain it. */
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
/* Consent modes for tests: production raises an async prompt; tests run
 * the decision synchronously. */
enum {
    SH_MPKG_CONSENT_PROMPT = -1,   /* production: async MessageBox thread */
    SH_MPKG_CONSENT_DECLINE = 0,   /* synchronous: user said no */
    SH_MPKG_CONSENT_ACCEPT  = 1    /* synchronous: user said yes -> install */
};
void sh_mpkg_test_set_consent_mode(int mode);
void sh_mpkg_test_reset(void);                    /* clear snapshot + session state */
const char *sh_mpkg_test_last_refusal(void);      /* last gate refusal reason, "" if none */
int sh_mpkg_test_session_installed_count(void);
#endif

#endif /* BACKEND_MAP_PACKAGE_H */
