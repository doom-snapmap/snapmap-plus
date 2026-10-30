/* modsync.h -- upload, download and claims between the local mod store
 * (modstore.c) and the product's mod store (modnet.c, Contract 4).
 *
 * Stock pointers (modbundle.h) carry no blob: an upload never sends their
 * bytes, and a download resolves each one against the installed game before
 * anything is fetched, refusing the bundle with the first cause when the game
 * lacks the entry or holds other bytes.
 *
 * Upload (publish, lobby host, an explicit push): for every bundle, the
 * blobs the store lacks are negotiated and sent from the mod folder's own
 * files, then the exact canonical manifest (derived from mod.json in memory,
 * byte-identical to what Contract 4 always received), then one claim names
 * every bundle under the caller's ref. The claim id and its secret are kept
 * in the "claims" section of <data>\mods\snapmap-plus.json, keyed by ref;
 * uploading for a ref that has a claim re-claims with the stored secret,
 * which replaces its bundle set. A failure after the first request that a
 * retry could fix is queued in its "pending" section and retried by
 * modsync_retry_pending.
 *
 * Download: the bundle manifest is fetched and verified against its hash;
 * each file is copied from an installed mod that holds the same bytes, else
 * fetched, and verified; the folder <data>\mods\<id>\ (else <id>@<version>,
 * sh_mb_folder_name) gets mod.json (an author block from the manifest and the
 * product block) and assets\, is verified by sh_ms_verify, and the serving
 * index is rebuilt. A bundle whose content an installed mod of the same id
 * already serves is not stored twice: its hash is recorded as an alias.
 *
 * Every call blocks and does file and network work: never on DOOM's main
 * thread for the network calls. Calls into the store are serialised by
 * modsync_store_lock, which the mod commands also take.
 */
#ifndef SNAPMAP_PLUS_MODSYNC_H
#define SNAPMAP_PLUS_MODSYNC_H

#include <stddef.h>
#include "modnet.h"
#include "modbundle.h"

#define MODSYNC_CLAIMS_SECTION  "claims"    /* sections of mods\snapmap-plus.json */
#define MODSYNC_PENDING_SECTION "pending"
#define MODSYNC_MAX_BUNDLES  MODNET_MAX_CLAIM

typedef struct modsync_stats {
    unsigned bundles;               /* bundles handled */
    unsigned bundles_created;       /* bundle manifests the store did not have (upload) or installed now (download) */
    unsigned blobs_total;           /* distinct blobs named */
    unsigned blobs_missing;         /* the store lacked (upload) or the local store lacked (download) */
    unsigned blobs_transferred;
    unsigned stock_files;           /* upload: stock pointers named, whose bytes are never sent */
    unsigned long long bytes_up, bytes_down;
    unsigned requests;
    double ms;
    char claim[MODNET_CLAIM_HEX + 1];   /* upload: the claim id */
    int reclaimed;                  /* upload: the stored secret replaced the ref's bundle set */
    int queued;                     /* upload: the failure was queued for retry */
    int already_installed;          /* download: nothing was fetched */
    char key[SH_MB_ID_MAX + 1 + 4 * SH_MB_VERSION_MAX + 1 + 20 + 8];   /* download: the installed folder (relative) */
    int code;                       /* MODNET_ code of the failure, MODNET_OK on success */
    char cause[256];
} modsync_stats;

/* Serialise store work across the command and worker threads: the store
 * lock of modstore.h (modstore_lock), under its older names. */
void modsync_store_lock(void);
void modsync_store_unlock(void);

/* Upload the installed bundles named by hash and claim them under ref.
 * Returns 1 on success; 0 with stats->cause (and the failure queued when it
 * was transient and the network had been reached). */
int  modsync_upload_bundles(const char *const *bundle_hashes, int n, const char *ref,
                            const modnet_io *io, modsync_stats *stats);

/* Download one bundle by hash into the local store and rescan the serving
 * index. Returns 1 on success (including already installed). */
int  modsync_fetch_bundle(const char *bundle_hash, const modnet_io *io, modsync_stats *stats);

/* Release the claim stored for ref and forget it (also when the store no
 * longer knows the claim). A pending upload for ref is dropped. Returns 1 on
 * success. */
int  modsync_release(const char *ref, const modnet_io *io, modsync_stats *stats);

/* Retry every queued upload. Returns the number still queued afterwards;
 * *done (optional) receives how many succeeded and *dropped how many were
 * dropped as permanent refusals. */
int  modsync_retry_pending(const modnet_io *io, unsigned *done, unsigned *dropped, modsync_stats *last);

/* The number of queued uploads, or -1 when the file cannot be read. */
int  modsync_pending_count(void);

/* The claim stored for ref: 1 and the id (secret_out optional), else 0. */
int  modsync_claim_lookup(const char *ref, char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1]);

/* ---------------------------------------------------- install core ---- */

/* Supply the bytes of one blob for an install: 1 with a malloc'd buffer, 0
 * when unavailable (cause filled). The caller verifies the hash. */
typedef int (*modsync_blob_source_fn)(void *context, const char *sha, unsigned long long size,
                                      unsigned char **bytes, size_t *len, char *cause, size_t cause_cap);

typedef struct modsync_install_info {
    unsigned blobs_present, blobs_fetched;
    unsigned stock_files;           /* stock pointers resolved against the installed game */
    unsigned long long bytes_fetched;
    int already_installed;
    char rel[SH_MB_ID_MAX + 1 + 4 * SH_MB_VERSION_MAX + 1 + 20 + 8];   /* the folder under mods\ */
} modsync_install_info;

/* Install a bundle from its canonical manifest text (whose SHA-256 must be
 * hash) with file bytes from `source` (for the files no installed mod
 * already holds): verified, written to a new folder with mod.json, verified
 * again and recorded. Does not rescan the serving index. Takes the store
 * lock. Returns 1 on success. */
int  modsync_install_manifest(const char *canonical, size_t len, const char *hash,
                              modsync_blob_source_fn source, void *context,
                              modsync_install_info *info, char *cause, size_t cause_cap);

/* The installed, usable bundle with this hash: 1 and its folder (UTF-8),
 * else 0. */
int  modsync_find_installed(const char *hash, char *folder, size_t cap);

#endif /* SNAPMAP_PLUS_MODSYNC_H */
