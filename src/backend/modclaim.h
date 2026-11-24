/* modclaim.h -- this computer as a publisher at the mod store: a published map claims the units its rows name,
 * authorised by a secret derived from the publishing key (HMAC-SHA256 of the key's claim seed and
 * "snapmap-plus/claim/1\n" + the item id) and signed by that key, each change carrying the next `rev`. Claims
 * and queued claims are sections of mods\snapmap-plus.json written only here; secrets are never stored. A claim
 * or release the store could not take is queued and retried at every start (modclaim_launch). A claim's record
 * names its key, so a map claimed under an inactive key still derives its secret. */
#ifndef SNAPMAP_PLUS_MODCLAIM_H
#define SNAPMAP_PLUS_MODCLAIM_H

#include <stddef.h>
#include "modnet.h"

#define MODCLAIM_CLAIMS_SECTION  "map-claims"
#define MODCLAIM_PENDING_SECTION "map-pending"

/* What the store answered for each unit of a publish. */
#define MODCLAIM_HELD     1   /* the store holds it */
#define MODCLAIM_MISSING  0   /* the store lacks it and this computer has its files: it uploads */
#define MODCLAIM_ABSENT  -1   /* the store lacks it and this computer does not have it */
#define MODCLAIM_REMOVED -2   /* moderation removed it; it cannot be uploaded again */

typedef struct modclaim_plan {
    size_t n;
    char (*units)[MODNET_HASH_HEX + 1];
    int *state;                              /* MODCLAIM_*, per unit */
    unsigned long long *bytes;               /* per missing unit: the bytes of the files the store lacks */
    unsigned long long missing_bytes;        /* their sum: what the publish uploads */
    size_t missing, absent, removed;
} modclaim_plan;
void modclaim_plan_free(modclaim_plan *plan);

/* The key of this computer (made now when there is none). 1 with its signer; 0 with *why (malloc'd). The signer
 * stays valid for the process. `made` is set to 1 when this call made the key. */
int  modclaim_signer(modnet_signer *out, int *made, char **why);
/* Tests: the folder the key lives in (NULL: the product's). */
void modclaim_set_key_folder(const char *folder);
/* Tests and tools: the mods root whose record holds the sections (NULL: the product's). */
void modclaim_set_root(const char *mods_root);

/* Bound the waits for a busy store (Retry-After) of the calls below, from now: `ms` in all (0: no waiting). */
void modclaim_set_budget(unsigned long long ms);

/* Ask the store about `units` and size what it lacks (see above). MODNET_OK with *plan filled; a MODNET_E_ code when the
 * store could not be asked (the plan's states are then unknown). */
int  modclaim_plan_units(const char *const *units, size_t n, modclaim_plan *plan, const modnet_io *io, char **why);
/* Upload the plan's missing units. MODNET_OK when all are held now. */
int  modclaim_upload(const modclaim_plan *plan, const modnet_io *io, char **why);
/* Claim `units` for the published item `ref`, replacing its set. MODNET_OK; the units moderation removed are
 * left out of the claim and named in *removed (malloc'd, *nremoved). A unit the store lacks is uploaded from
 * this computer first when it can be. */
int  modclaim_claim(const char *ref, const char *const *units, size_t n, char (**removed)[MODNET_HASH_HEX + 1],
                    size_t *nremoved, const modnet_io *io, char **why);
/* Release the claim of `ref`. MODNET_OK, also when the store or this computer has no such claim. */
int  modclaim_release(const char *ref, const modnet_io *io, char **why);

/* Queue a claim (units) or release (units NULL) of `ref` that could not reach the store; replaces the item's
 * queued entry. 1 queued. */
int  modclaim_queue(const char *ref, const char *const *units, size_t n);
/* Retry the queue, then repair this computer's claims. The counts of what was settled. MODNET_OK. */
typedef struct modclaim_launch_stats { unsigned retried, settled, repaired, uploaded; int left; } modclaim_launch_stats;
int  modclaim_launch(const modnet_io *io, modclaim_launch_stats *out);

/* Record which namespaces the claim of `ref` serves (the salts of the rows whose units it names), for the Mods tab's
 * published maps (modui.c). Kept across later claims of the item. */
void modclaim_set_mods(const char *ref, const char *const *salts, size_t n);

/* 1 when this computer holds a claim (or queued claim) of `ref`. */
int  modclaim_known(const char *ref);
/* The units this computer's claims and queued claims name (the sweep keeps them). The count; *out malloc'd. */
size_t modclaim_held(char (**out)[MODNET_HASH_HEX + 1]);

/* The signed-in account name the key's requests present (UTF-8; "" signed out). Any thread. */
void modclaim_set_account_name(const char *name);
/* 1 when the key is loaded in this process (id: its claim id). */
int  modclaim_signer_loaded(char id[17]);
/* The backup offer: pending (1) after the publish that made the key, done (0) once answered. */
void modclaim_key_offer(int pending);
int  modclaim_key_offer_pending(void);
/* {"exists":bool,"id":"<16 hex>","offer":bool} (malloc'd): what Settings and the offer show. */
char *modclaim_key_status_json(void);
/* Write the active key's passphrase backup to `path`. 1, or 0 with *why (a sentence). Slow (the key derivation). */
int  modclaim_key_backup(const char *path, const char *passphrase, char **why);
/* Restore the backup at `path` and make its key the active one: SH_KEY_RESTORE_* (modkey.h), *why its sentence. */
int  modclaim_key_restore(const char *path, const char *passphrase, char **why);

/* The Settings page's and the offer's requests (SH_MODS_ACT_KEY, modui.c): "status"; "later";
 * "backup\t<path>\t<passphrase>"; "restore\t<path>\t<passphrase>". The answer (malloc'd) is
 * {"ok":bool,"message":"<sentence>","key":<modclaim_key_status_json>}. Never logs the passphrase; any thread; a
 * backup or restore takes about half a second. */
char *modclaim_key_action(const char *arg);

/* The secret of the claim of `ref` as the next claim or release sends it, and the id of the key it came from (tests). */
int  modclaim_secret_for(const char *ref, char out[MODNET_HASH_HEX + 1], char key_id[17]);

/* The paths players asked of the units this computer's claims name: fn(unit, root, path) per entry. MODNET_OK. */
typedef void (*modclaim_asked_fn)(void *context, const char *unit, const char *root, const char *path);
int  modclaim_asked(modclaim_asked_fn fn, void *context, const modnet_io *io, char **why);

#endif /* SNAPMAP_PLUS_MODCLAIM_H */
