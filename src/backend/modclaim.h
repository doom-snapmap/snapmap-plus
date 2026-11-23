/* modclaim.h -- this computer as a publisher at the mod store (SPEC-distribution 4.3, 5.3-5.5, 5.21).
 *
 * A published map claims the units its rows name (2.8). The claim is authorised by its secret, derived from this
 * computer's publishing key (D8: HMAC-SHA256 of the key's claim seed and "snapmap-plus/claim/1\n" + the item id), and
 * every write is signed by that key (4.3). Each change of a claim carries the next `rev` (D33). The key is made at the
 * first publish, in the product's own data folder (%LOCALAPPDATA%\snapmap-plus\publishing, modkey.h), never the game's.
 *
 * The publish (modpublish.c) asks here, on the modnet worker:
 *   modclaim_plan     which of the map's units the store holds, lacks or removed, and how many bytes it lacks (each
 *                     missing unit's `begin` names the files the store does not hold); a unit the store lacks whose
 *                     files are not on this computer is "absent" (S8)
 *   modclaim_upload   sends what the plan found missing (modcache.h sh_cache_publish_unit)
 *   modclaim_claim    claims the units for the published item, replacing its earlier set (a republish)
 *   modclaim_release  releases an item's claim (unpublish, a deleted map whose item is gone)
 * A claim or release the store could not take (busy, unreachable) is queued (`modclaim_queue`) and retried at every
 * start (modclaim_launch), with the units of that claim uploaded first. At start the same pass repairs: for every claim
 * this computer made, the units the store lost are uploaded again from this computer's files and the claim is made
 * again (5.21). Nothing here touches a map other people published, except to read the store's answers.
 *
 * Records (sections of mods\snapmap-plus.json, written only here, under one lock):
 *   "map-claims"   {"<item>": {"rev": n, "units": ["<hash>", ...], "key": "<16 hex>", "mods": ["<salt>", ...],
 *                   "at": <unix time of the last claim>}}
 *   "map-pending"  {"<item>": {"do": "claim"|"release", "units": [...]}}
 * The secrets are never stored: the key derives them again.
 *
 * Retry-After: a busy store's answer is waited out within the caller's budget (modclaim_set_budget). Any thread except
 * where noted; the network calls belong on the modnet worker.
 */
#ifndef SNAPMAP_PLUS_MODCLAIM_H
#define SNAPMAP_PLUS_MODCLAIM_H

#include <stddef.h>
#include "modnet.h"

#define MODCLAIM_CLAIMS_SECTION  "map-claims"
#define MODCLAIM_PENDING_SECTION "map-pending"

/* What the store answered for each unit of a publish. */
#define MODCLAIM_HELD     1   /* the store holds it */
#define MODCLAIM_MISSING  0   /* the store lacks it and this computer has its files: it uploads */
#define MODCLAIM_ABSENT  -1   /* the store lacks it and this computer does not have it (S8) */
#define MODCLAIM_REMOVED -2   /* moderation removed it; it cannot be uploaded again */

typedef struct modclaim_plan {
    size_t n;
    char (*units)[MODNET_HASH_HEX + 1];
    int *state;                              /* MODCLAIM_*, per unit */
    unsigned long long *bytes;               /* per missing unit: the bytes of the files the store lacks */
    unsigned long long missing_bytes;        /* their sum: what the publish uploads (S7) */
    size_t missing, absent, removed;
} modclaim_plan;
void modclaim_plan_free(modclaim_plan *plan);

/* The key of this computer (made now when there is none). 1 with its signer; 0 with *why (malloc'd). The signer stays
 * valid for the process. `made` is set to 1 when this call made the key (the first publish: S-key, DS-K). */
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
/* Claim `units` for the published item `ref` (replacing its earlier set). MODNET_OK; the units moderation removed are
 * left out of the claim and named in *removed (malloc'd, *nremoved). A unit the store lacks is uploaded from this
 * computer first when it can be. */
int  modclaim_claim(const char *ref, const char *const *units, size_t n, char (**removed)[MODNET_HASH_HEX + 1],
                    size_t *nremoved, const modnet_io *io, char **why);
/* Release the claim of `ref`. MODNET_OK, also when the store or this computer has no such claim. */
int  modclaim_release(const char *ref, const modnet_io *io, char **why);

/* Queue a claim (units) or release (units NULL) of `ref` that could not reach the store; replaces an earlier entry for
 * that item. 1 queued. */
int  modclaim_queue(const char *ref, const char *const *units, size_t n);
/* Retry the queue, then repair this computer's claims (5.21). The counts of what was settled. MODNET_OK. */
typedef struct modclaim_launch_stats { unsigned retried, settled, repaired, uploaded; int left; } modclaim_launch_stats;
int  modclaim_launch(const modnet_io *io, modclaim_launch_stats *out);

/* Record which namespaces the claim of `ref` serves (the salts of the rows whose units it names), for the Mods tab's
 * published maps (modui.c). Kept across later claims of the item. */
void modclaim_set_mods(const char *ref, const char *const *salts, size_t n);

/* 1 when this computer holds a claim (or queued claim) of `ref`. */
int  modclaim_known(const char *ref);
/* The units this computer's claims and queued claims name (the sweep keeps them, 5.17 `held`). The count; *out
 * malloc'd. */
size_t modclaim_held(char (**out)[MODNET_HASH_HEX + 1]);

/* The paths players asked of the units this computer's claims name (3.8): fn(unit, root, path) per entry. MODNET_OK. */
typedef void (*modclaim_asked_fn)(void *context, const char *unit, const char *root, const char *path);
int  modclaim_asked(modclaim_asked_fn fn, void *context, const modnet_io *io, char **why);

#endif /* SNAPMAP_PLUS_MODCLAIM_H */
