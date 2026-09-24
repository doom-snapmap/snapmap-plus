/* Main-thread leases for compiler-assigned native audio identities. */
#ifndef SH_AUDIO_ID_RESERVATIONS_H
#define SH_AUDIO_ID_RESERVATIONS_H
#include "audio_link_graph.h"

typedef struct sh_audio_id_pool sh_audio_id_pool;
typedef struct sh_audio_id_lease sh_audio_id_lease;
/* 0 free, 1 occupied, 2 reserved by an installed original, -1 inspection failed.
 * An original reservation cannot be authorized by an active private peer.
 * Must include native live objects
 * and the full installed-original inventory (including not-yet-loaded banks),
 * so later vanilla loading cannot take a private identity. This is a read-only
 * probe, not a native object lookup that acquires an engine reference. */
typedef int (*sh_audio_id_occupied)(void *context, sh_audio_identity_domain domain,
    uint32_t id, char *error, size_t capacity);
sh_audio_id_pool *sh_audio_id_pool_open(sh_audio_id_occupied occupied, void *context);
/* Refuses destruction while any lease remains. */
int sh_audio_id_pool_close(sh_audio_id_pool *pool);
sh_audio_id_lease *sh_audio_id_lease_begin(sh_audio_id_pool *pool);
/* Direct sh_audio_graph_assign callback. One graph per lease; the owning
 * provider retains that immutable graph and source bytes until release.
 * Pending reservations are rolled back by releasing the lease on failure.
 * For private bindings equal hashes only nominate a candidate: exact cross-graph
 * comparison decides reuse. Different private bindings receive different IDs.
 * Private IDs avoid reservations in every logical domain because some domains
 * share engine tables; certified originals share their existing domain-specific
 * ABI even when captures contain different subsets of native definitions. This
 * does not declare their scoped recipes equal or authorize private ID reuse. */
int sh_audio_id_reserve(void *lease, const sh_audio_link_graph *graph, size_t node,
    sh_audio_identity_domain domain, uint32_t authored, const unsigned char binding[32],
    int original, int absent, uint32_t *id, char *error, size_t capacity);
/* Recheck occupancy immediately before native activation. An unexpected new
 * occupant fails the candidate, allowing its caller to rebind/retry. No native
 * state or existing lease changes. An occupied exact active peer is reusable;
 * private absence must remain unoccupied even when its lease is active. */
int sh_audio_id_lease_ready(sh_audio_id_lease *lease, char *error, size_t capacity);
/* Only after native publication succeeds. Drops temporary comparison tables;
 * no allocation/native call/fallible commit. Never call after partial failure. */
void sh_audio_id_lease_activate(sh_audio_id_lease *lease);
int sh_audio_id_lease_active(const sh_audio_id_lease *lease);
/* Visit private identities that no other live lease claims: the IDs whose
 * native objects must be gone before this lease may release. Originals and
 * reserved absences never name a private native object. visit returns 0 to
 * stop; the call then returns 0. No native state or claim changes. */
int sh_audio_id_lease_exclusive(const sh_audio_id_lease *lease,
    int (*visit)(void *, sh_audio_identity_domain, uint32_t), void *context);
/* Pending: abort after native rollback. Active: only after all native users
 * and banks retire. This function does not unload or infer native retirement.
 * Release precedes destruction of the owning provider's captured graph. */
void sh_audio_id_lease_release(sh_audio_id_lease *lease);
#endif
