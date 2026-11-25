/* modkey.h -- the publishing key: an ECDSA P-256 pair plus a 32-byte claim seed, made at a player's first map publish.
 * Its public point (65 bytes, base64url) is the publisher's identity at the mod store; it signs every write
 * (modnet.h modnet_signer) and derives each claim secret, HMAC-SHA256(seed, "snapmap-plus/claim/1\n" + ref).
 * Stored as <folder>\<16 hex of the public point's SHA-256>.key, the private scalar and seed under DPAPI; <folder>\active
 * names the active key (else the newest). The passphrase backup is AES-256-GCM (CNG) under a PBKDF2-SHA256 key.
 * Windows CNG and DPAPI; any thread. */
#ifndef SNAPMAP_PLUS_MODKEY_H
#define SNAPMAP_PLUS_MODKEY_H

#include <stddef.h>
#include "modnet.h"

#define SH_KEY_PUBLIC_B64 87    /* base64url characters of the 65-byte point */
#define SH_KEY_DIR        "publishing"
#define SH_KEY_ACTIVE     "active"   /* <folder>\active: the active key's 16-hex leaf */
#define SH_KEY_BACKUP_EXT ".snapmap-plus-key"
#define SH_KEY_KDF_ROUNDS 600000u
#define SH_KEY_PASSPHRASE_MIN 8

/* sh_key_restore's answers */
#define SH_KEY_RESTORE_OK          1
#define SH_KEY_RESTORE_DAMAGED     0   /* "This file is not a Snapmap+ publishing key backup." */
#define SH_KEY_RESTORE_PASSPHRASE -1   /* "That passphrase does not open this backup." */

typedef struct sh_key sh_key;

/* A new key and seed, in memory. 1 with *out (release with sh_key_free); 0 with *why (malloc'd). */
int  sh_key_create(sh_key **out, char **why);
/* The active key of `folder` (the newest .key file there), or a new one written there. */
int  sh_key_load_or_create(const char *folder, sh_key **out, char **why);
/* Read one key file / write the key into `folder` (its file name is its fingerprint). */
int  sh_key_load(const char *path, sh_key **out, char **why);
int  sh_key_save(const sh_key *key, const char *folder, char **why);
void sh_key_free(sh_key *key);

/* The public half, base64url (SH_KEY_PUBLIC_B64 characters). */
const char *sh_key_public(const sh_key *key);
/* The SnapMap account name the key's requests present (UTF-8; NULL or "" when signed out: no name is sent). */
int  sh_key_set_name(sh_key *key, const char *name);

/* The claim secret of the published item `ref` (64 lowercase hex). 1 on success. */
int  sh_key_claim_secret(const sh_key *key, const char *ref, char out[MODNET_HASH_HEX + 1]);

/* The signer modnet's writes take (the key must outlive it). */
modnet_signer sh_key_signer(sh_key *key);

/* The id a claim record names its key by: 16 hex of the SHA-256 of the public text. */
void sh_key_claim_id(const sh_key *key, char out[17]);
/* Write the key into `folder` and make it the active one. 1, or 0 with *why. */
int  sh_key_make_active(const sh_key *key, const char *folder, char **why);
/* The key of `folder` whose claim id is `claim_id` (active or not). 1 with *out; 0 with *why. */
int  sh_key_find(const char *folder, const char *claim_id, sh_key **out, char **why);
/* Write the key's passphrase backup to `path` (the passphrase has at least SH_KEY_PASSPHRASE_MIN bytes). 1, or 0 with
 * *why (a sentence). Slow by design: the key derivation takes about half a second. */
int  sh_key_backup(const sh_key *key, const char *passphrase, const char *path, char **why);
/* Open the backup at `path` with `passphrase`: SH_KEY_RESTORE_OK with *out (not yet saved anywhere), or another
 * SH_KEY_RESTORE_* with *why (its sentence). */
int  sh_key_restore(const char *path, const char *passphrase, sh_key **out, char **why);

/* The UTF-8 name as the X-Publisher-Name header carries it: percent-encoded as encodeURIComponent does (malloc'd). */
char *sh_key_name_header(const char *name);

#endif /* SNAPMAP_PLUS_MODKEY_H */
