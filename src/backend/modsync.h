/* modsync.h -- upload, download and claims between the local mod store
 * (modstore.c) and the product's mod store (modnet.c, ONE-SHAPE rule 5).
 *
 * The store keeps one archive per build (modnet.h): a stored ZIP (modzip.h)
 * holding the folder's mod.json, the whole file, and assets/<request> for
 * every carried file its block lists. Stock pointers never upload: their
 * bytes are the player's own game, resolved against it at install.
 *
 * Upload (publish, an explicit push): a map names its mods as
 * "<salt>@<build hash>" (modmanifest.h). For each, the folder that holds the
 * salt at that hash is found (a root folder first, else a map's place); HEAD
 * asks whether the store holds the build; when it does not, the folder is
 * written as an archive to a temporary file, its every file checked against
 * its block, and sent (modnet_put_build: one request, or parts for an
 * archive larger than one). Nothing limits a build's size, its number of
 * files or how many builds a map names (the archive is ZIP64 where it must
 * be). Then one claim names every build under the caller's ref. The
 * claim id and its secret are kept in the "claims" section of
 * <data>\mods\snapmap-plus.json, keyed by ref; uploading for a ref that has a
 * claim re-claims with the stored secret, which replaces its build set. A
 * failure after the first request that a retry could fix is queued in its
 * "pending" section and retried by modsync_retry_pending. A release the store
 * did not take is queued there too ("kind": "release") and retried at the next
 * launch and after the next store call that succeeds.
 *
 * Download (a map that names a build this machine does not serve): the
 * archive is streamed to a temporary file with progress, of any size, and
 * unpacked into the map's place
 * (mods\maps\<map title>\<mod name>\, sh_ms_place_folder), replacing that
 * place's earlier build of the salt. Before anything is placed, the archive's
 * mod.json must load, its salt and build hash must be the row's, every file
 * must be an entry with the size and SHA-256 its block records and no other
 * entry may be present ("the mod store sent a build that does not match the
 * map"), and every stock pointer must resolve against the installed game
 * (MODNET_E_GAME). The place is noted in the record and the serving index is
 * rebuilt. Two maps that name one build each unpack their own copy.
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
#define MODSYNC_SALT_HEX    8
#define MODSYNC_MOD_MAX     (MODSYNC_SALT_HEX + 1 + MODNET_HASH_HEX)   /* "<salt>@<hash>" */
#define MODSYNC_FOLDER_MAX  1024

struct sh_ms_place;
struct sh_modmanifest_row;

typedef struct modsync_stats {
    unsigned builds;                /* builds handled */
    unsigned builds_present;        /* upload: the store held them already (HEAD) */
    unsigned builds_sent;           /* upload: archives sent */
    unsigned builds_created;        /* upload: archives the store did not have; download: installed now */
    unsigned stock_files;           /* upload: stock pointers left out of the archives */
    unsigned long long bytes_up, bytes_down;
    unsigned requests;
    double ms;
    char claim[MODNET_CLAIM_HEX + 1];   /* upload: the claim id */
    int reclaimed;                  /* upload: the stored secret replaced the ref's build set */
    int queued;                     /* upload or release: the failure was queued for retry */
    int already_installed;          /* download: nothing was fetched */
    char folder[MODSYNC_FOLDER_MAX];   /* download: the folder under mods\ that serves the row */
    int code;                       /* MODNET_ code of the failure, MODNET_OK on success */
    char cause[1024];               /* room for a record path and its next step (sh_mb_record_why) */
    int game_lacks;                 /* download: refused because a borrow names game bytes this machine lacks */
    char game_source[260];          /* that borrow's installed source path */
} modsync_stats;

/* Serialise store work across the command and worker threads: the store
 * lock of modstore.h (modstore_lock), under its older names. */
void modsync_store_lock(void);
void modsync_store_unlock(void);

/* 1 when `mod` is "<salt>@<64 hex>"; salt_out and hash_out (optional) get
 * the parts. */
int  modsync_parse_mod(const char *mod, char salt_out[MODSYNC_SALT_HEX + 1], char hash_out[MODNET_HASH_HEX + 1]);

/* Upload the builds a map names ("<salt>@<hash>", any number) and claim them
 * under ref. Returns 1 on success; 0 with stats->cause (and
 * the failure queued when it was transient and the network had been
 * reached). */
int  modsync_upload_builds(const char *const *mods, int n, const char *ref, const modnet_io *io,
                           modsync_stats *stats);

/* Write the build archive of `folder` (a built mod folder whose block names
 * `hash`) to `out_path`: mod.json, then assets/<request> for every carried
 * file, each checked against the block first. *size gets the archive's
 * bytes, *pointers (optional) the stock pointers left out, *code (optional)
 * MODNET_OK or the failure's code. Returns 1 on success. */
int  modsync_write_archive(const char *folder, const char *hash, const char *out_path, unsigned long long *size,
                           unsigned *pointers, int *code, char *cause, size_t cause_cap);

typedef struct modsync_install_info {
    unsigned files;                 /* carried files placed */
    unsigned stock_files;           /* stock pointers resolved against the installed game */
    unsigned long long bytes;       /* carried bytes placed */
    int already_installed;          /* the root or the place held the build: nothing was placed */
    int replaced;                   /* the place's earlier build of the salt was replaced */
    int mismatch;                   /* refused: the archive is not the build the row names */
    char rel[MODSYNC_FOLDER_MAX];   /* the folder under mods\ that serves the build */
    char name[128];                 /* the build's display name */
    int game_lacks;                 /* refused: a borrow names game bytes this machine lacks */
    char game_source[260];          /* that borrow's installed source path */
} modsync_install_info;

/* Install the archive file `archive` (UTF-8) as the build `hash` of `salt`
 * into `place` (a map's, or a ZIP's): verified (above), unpacked beside its
 * target as <name>.staging-<pid>, then swapped in. `place` NULL installs it
 * at the root as the player's own mod (sh_ms_install_to_root, from a copy
 * unpacked outside the store). Does not rescan the serving index. Takes the
 * store lock. Returns 1 on success. */
int  modsync_install_archive(const char *archive, const char *salt, const char *hash, const struct sh_ms_place *place,
                             modsync_install_info *info, char *cause, size_t cause_cap);

/* Fetch the row's build into `place` (NULL: the root) and rescan the serving
 * index. Returns 1 on success, also when the root (at the row's hash) or the
 * place already holds it (stats->already_installed, nothing fetched). For
 * the root, a map's place that holds this very build is installed from there
 * without a download. */
int  modsync_fetch_build_for(const struct sh_modmanifest_row *row, const struct sh_ms_place *place,
                             const modnet_io *io, modsync_stats *stats);

/* The usable folder that serves salt at hash: a root folder, else the
 * place's folder of the salt (place NULL: any place). 1 and its folder
 * (absolute, UTF-8), else 0. */
int  modsync_find_installed(const char *salt, const char *hash, const struct sh_ms_place *place, char *folder,
                            size_t cap);

/* Release the claim stored for ref and forget it (also when the store no
 * longer knows the claim). A pending upload for ref is dropped. Returns 1 on
 * success, then retries the other queued releases; a transient failure is
 * queued (stats->queued) in place of any queued upload for ref. */
int  modsync_release(const char *ref, const modnet_io *io, modsync_stats *stats);

/* Retry every queued upload and release. Returns the number still queued afterwards;
 * *done (optional) receives how many succeeded and *dropped how many were
 * dropped as permanent refusals. */
int  modsync_retry_pending(const modnet_io *io, unsigned *done, unsigned *dropped, modsync_stats *last);

/* The number of queued uploads and releases, or -1 when the file cannot be read. */
int  modsync_pending_count(void);

/* Forget a queued entry without a request. */
void modsync_pending_drop(const char *ref);

/* Record that the queued upload for ref holds a provisional claim that may
 * be released only after ref's own claim succeeds (kept across attempts).
 * Returns 1 when written. */
int  modsync_pending_link(const char *ref, const char *provisional);

/* 1 while a queued upload still waits on that provisional claim. */
int  modsync_pending_waits_on(const char *provisional);

/* The claim stored for ref: 1 and the id (secret_out optional), else 0. */
int  modsync_claim_lookup(const char *ref, char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1]);

/* ------------------------------------------------------ the gate's probe ---- */

/* What the denial gate (modpublished.h, sh_modpub_gate_decide) asks the mod
 * store about one build before the player is asked anything. Whether the
 * build's borrows resolve against this game is learned at install. */
typedef struct modsync_probe {
    int found;                      /* 1 the store holds it, 0 absent (404, or 451 on the takedown
                                       list), -1 the store could not be asked or did not answer */
    int code;                       /* the MODNET_ code of HEAD /mods/build/<hash> */
    unsigned long long size;        /* found: the archive's bytes */
    unsigned scripts;               /* found: the script files the build carries */
    char cause[256];
} modsync_probe;

/* HEAD the build. Returns out->found. Worker thread; network. */
int  modsync_probe_build(const char *build_hash, const modnet_io *io, modsync_probe *out);

/* 1 when a claim this machine holds (the "claims" section) names the build:
 * a map of this machine's was published with it and not unpublished. */
int  modsync_claim_names(const char *build_hash);

#endif /* SNAPMAP_PLUS_MODSYNC_H */
