/* modkey.h -- the publishing key (SPEC-distribution 4.2-4.3).
 *
 * An ECDSA P-256 key pair the product makes at a player's first map publish, with a 32-byte claim seed. Its public
 * half, the raw uncompressed point (65 bytes) written base64url without padding, is the publisher's identity at the
 * mod store. It signs every write the product sends (the three X-Publisher-* headers, modnet.h modnet_signer) and
 * derives each published map's claim secret, HMAC-SHA256(seed, "snapmap-plus/claim/1\n" + ref) (D8), so a backup of
 * the key restores every map's claims. It never takes part in map content.
 *
 * Stored at <folder>\<16 hex of the public point's SHA-256>.key, JSON
 *   {"created":<unix s>,"format":1,"name":"<account name last presented>","protected":"<base64>","public":"<b64url>"}
 * where "protected" is CryptProtectData (the current Windows user) of the private scalar and the seed. The folder is
 * the product's own data folder (%LOCALAPPDATA%\snapmap-plus\publishing), never the game's.
 *
 * The active key is the one <folder>\active names (its file's 16-hex leaf); the others are inactive, kept so the claims
 * they made keep their secrets (a restore makes the restored key active and keeps the former one). Without an active
 * file the newest key is the active one.
 *
 * The backup (4.2) is <name>.snapmap-plus-key, JSON
 *   {"format":1,"public":"<b64url>","kdf":{"pbkdf2-sha256":600000,"salt":"<b64url 16>"},"nonce":"<b64url 12>",
 *    "sealed":"<b64url 80>"}
 * where "sealed" is AES-256-GCM (CNG) of the private scalar and the seed (64 bytes, then the 16-byte tag), under the
 * key PBKDF2-SHA256 derives from the passphrase. A restore checks the
 * tag (a wrong passphrase) and that the sealed scalar is the public point's (an altered file). Windows CNG and DPAPI;
 * any thread. */
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
