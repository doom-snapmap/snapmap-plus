/* Exact shared storage between the complete package deliveries one map carries.
 *
 * A carrier's payload is either its delivery's complete original archive (the
 * classic carrier) or an exact delta. A delta rebuilds the delivery's complete
 * original archive from byte ranges of other deliveries' complete archives,
 * carried classically in the same map under their own keys, plus literal bytes
 * of its own. It records the exact length and SHA-256 of its archive and of
 * every base it reads, so a rebuilt archive is the original byte for byte or
 * it is refused. The carrier key stays the delivery's own identity: sharing
 * stores equal bytes once and never merges identities, recipes or authorship.
 *
 * Format 1, little endian:
 *   "SMPKDELT", u32 version 1, u64 archive length, archive SHA-256[32],
 *   u32 base count (1..SH_MPKG_DELTA_MAX_BASES), then per base: u16 key
 *   length, key, u64 length, SHA-256[32]; u32 operation count, then per
 *   operation: u8 1 = copy (u32 base, u64 offset, u64 length) or u8 2 =
 *   literal (u64 length, then its bytes). The operations rebuild the archive
 *   exactly, in order; every base is read; nothing follows the last one.
 *
 * A reader before this format has no delta decoder: the payload is not an
 * archive, and the beta16 reader's archive validation rejects it. A delta is
 * written only where it at least halves its delivery's transport shards, so a
 * map that gains little or nothing keeps the representation earlier readers
 * decode.
 */
#ifndef SH_MAP_PACKAGE_SHARE_H
#define SH_MAP_PACKAGE_SHARE_H

#include <stddef.h>

#define SH_MPKG_DELTA_MAGIC     "SMPKDELT"
#define SH_MPKG_DELTA_VERSION   1u
#define SH_MPKG_DELTA_MAX_BASES 64u
/* Base keys are carrier transport ids; longer than any header this reads. */
#define SH_MPKG_DELTA_KEY_CAP   400u

/* Whether a stored payload is a delta of any format version. */
int sh_mpkg_delta_is(const unsigned char *payload, size_t length);

/* One delivery for planning, in carrier order: key is the carrier's canonical
 * transport id, archive its complete original bytes (borrowed). On success
 * delta is NULL (store the archive) or a malloc'd delta to store instead. */
typedef struct sh_mpkg_share_item {
    const char *key;
    const unsigned char *archive;
    size_t length;
    unsigned char *delta;
    size_t delta_length;
} sh_mpkg_share_item;

/* Decide each delivery's payload. A delivery becomes a delta of earlier classic
 * deliveries only when equal bytes, cut at the archives' own local records and
 * compared byte for byte, let it occupy at most half the transport shards of
 * shard_chars base64 characters that its archive needs. Repeated keys share
 * nothing. Returns 0 with every delta NULL on failure. */
int sh_mpkg_share_plan(sh_mpkg_share_item *items, size_t count, size_t shard_chars,
    char *error, size_t capacity);
void sh_mpkg_share_clear(sh_mpkg_share_item *items, size_t count);

/* A delivery's stored payload by carrier key: a process-heap buffer the
 * decoder frees, or NULL with the reason. */
typedef unsigned char *(*sh_mpkg_delta_resolve)(void *context, const char *key, size_t *length,
    char *error, size_t capacity);

/* Rebuild a delta's archive after validating every field. self is the delta's
 * own key, which no base may name; a base must be a complete archive, never a
 * delta, of exactly its recorded length and SHA-256. Returns the exact archive
 * in a process-heap buffer, or NULL with the reason; nothing partial escapes. */
unsigned char *sh_mpkg_delta_decode(const unsigned char *payload, size_t length, const char *self,
    sh_mpkg_delta_resolve resolve, void *context, size_t max_archive, size_t *out_length,
    char *error, size_t capacity);

#ifdef SH_MAP_PACKAGE_TESTING
/* Refuse the nth allocation the codec makes from now (0: none), as an
 * exhausted heap would; returns how many remain unrefused before it fires. */
void sh_mpkg_share_test_refuse_allocation(long nth);
long sh_mpkg_share_test_allocations_left(void);
/* Make every segment digest equal while on, so only byte comparison can tell
 * segments apart: planning must give exactly the same result. */
void sh_mpkg_share_test_colliding_digests(int on);
#endif

#endif
