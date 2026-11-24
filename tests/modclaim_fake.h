/* modclaim_fake.h -- a scripted mod store at the modclaim.h seam, for tests of the publish (modpublish_test): what the
 * store holds, what an upload, a claim or a release answers, and a record of every call. */
#ifndef MODCLAIM_FAKE_H
#define MODCLAIM_FAKE_H

#include <stddef.h>
#include "../src/backend/modclaim.h"

#define FC_MAX 32

typedef struct fc_unit { char hash[MODNET_HASH_HEX + 1]; int state; unsigned long long bytes; } fc_unit;

typedef struct fc_state {
    /* what the store holds: a unit not listed is MODCLAIM_HELD */
    fc_unit units[FC_MAX];
    int nunits;
    /* the answers, taken once each and then MODNET_OK (0) */
    int signer_fails;              /* the key cannot be made: the why */
    int made_key;                  /* the next signer call makes the key */
    int plan_code, upload_code, claim_code, release_code;
    char why[256];                 /* the why a failing answer gives */
    unsigned upload_delay_ms;      /* the upload takes this long, reporting progress */
    /* the record */
    int plans, uploads, claims, releases, queued, launches, signers;
    int claims_refused;   /* claims refused because they named a unit the store does not hold (the Worker's 409) */
    char claim_ref[64], release_ref[64], queued_ref[64];
    size_t claim_n, queued_n;
    char claim_units[FC_MAX][MODNET_HASH_HEX + 1];
    unsigned long long uploaded_bytes;
    size_t mods_n;                 /* the last set_mods: how many salts, and the first */
    char mods0[16];
    /* the claims this computer holds */
    char known[FC_MAX][64];
    int nknown;
} fc_state;

extern fc_state g_fc;
void fc_reset(void);
void fc_set_unit(const char *hash, int state, unsigned long long bytes);

#endif
