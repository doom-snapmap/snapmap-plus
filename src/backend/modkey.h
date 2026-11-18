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
 * Not here yet (plan DS-K): the passphrase backup and restore of 4.2, the inactive keys a restore keeps, and reading
 * the account name from the signed-in game (R-DS2 5.5, GetProfileUsername). Windows CNG and DPAPI; any thread. */
#ifndef SNAPMAP_PLUS_MODKEY_H
#define SNAPMAP_PLUS_MODKEY_H

#include <stddef.h>
#include "modnet.h"

#define SH_KEY_PUBLIC_B64 87    /* base64url characters of the 65-byte point */
#define SH_KEY_DIR        "publishing"

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

/* The UTF-8 name as the X-Publisher-Name header carries it: percent-encoded as encodeURIComponent does (malloc'd). */
char *sh_key_name_header(const char *name);

#endif /* SNAPMAP_PLUS_MODKEY_H */
