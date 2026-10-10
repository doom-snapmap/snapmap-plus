#include "audio_id_reservations.h"
#include <stdlib.h>
#include <string.h>

typedef struct ar_entry ar_entry;
typedef struct ar_claim {
    ar_entry *entry;
    struct sh_audio_id_lease *owner;
    size_t node;
    struct ar_claim *next, *peer, **previous;
} ar_claim;
typedef struct ar_comparison {
    const sh_audio_link_graph *other;
    sh_audio_graph_comparison *comparison;
    struct ar_comparison *next;
} ar_comparison;
struct ar_entry {
    sh_audio_identity_domain domain;
    uint32_t id;
    unsigned char binding[32];
    int original, absent;
    ar_claim *claims;
    ar_entry *identity_next, *binding_next;
};
struct sh_audio_id_pool {
    ar_entry **identities, **bindings;
    size_t capacity, count;
    sh_audio_id_occupied occupied;
    void *context;
    struct sh_audio_id_lease *leases;
};
struct sh_audio_id_lease {
    sh_audio_id_pool *pool;
    const sh_audio_link_graph *graph;
    ar_claim *claims;
    ar_comparison *comparisons;
    struct sh_audio_id_lease *next, **previous;
    int active;
};
static int ar_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }
static uint32_t ar_key(sh_audio_identity_domain domain, const unsigned char binding[32], int original, int absent)
{
    uint32_t hash = UINT32_C(2166136261) ^ (uint32_t)domain ^ ((uint32_t)original << 8) ^ ((uint32_t)absent << 9);
    for (size_t i = 0; i < 32; i++) hash = (hash ^ binding[i]) * UINT32_C(16777619);
    return hash;
}
static size_t ar_identity_bucket(sh_audio_identity_domain domain, uint32_t id, size_t capacity)
{ (void)domain; return (size_t)id * UINT32_C(2654435761) & (capacity - 1); }
static size_t ar_binding_bucket(const ar_entry *entry, size_t capacity)
{ return ar_key(entry->domain, entry->binding, entry->original, entry->absent) & (capacity - 1); }
static int ar_grow(sh_audio_id_pool *pool)
{
    if (pool->capacity && pool->count < pool->capacity / 2) return 1;
    size_t count = pool->capacity ? pool->capacity * 2 : 64;
    if (count < pool->capacity || count > SIZE_MAX / sizeof(ar_entry *)) return 0;
    ar_entry **identities = calloc(count, sizeof(*identities)), **bindings = calloc(count, sizeof(*bindings));
    if (!identities || !bindings) { free(identities); free(bindings); return 0; }
    for (size_t i = 0; i < pool->capacity; i++) for (ar_entry *e = pool->identities[i], *next; e; e = next) {
        next = e->identity_next;
        size_t key = ar_identity_bucket(e->domain, e->id, count);
        e->identity_next = identities[key]; identities[key] = e;
        key = ar_binding_bucket(e, count); e->binding_next = bindings[key]; bindings[key] = e;
    }
    free(pool->identities); free(pool->bindings);
    pool->identities = identities; pool->bindings = bindings; pool->capacity = count; return 1;
}
sh_audio_id_pool *sh_audio_id_pool_open(sh_audio_id_occupied occupied, void *context)
{
    if (!occupied) return NULL;
    sh_audio_id_pool *pool = calloc(1, sizeof(*pool));
    if (pool) { pool->occupied = occupied; pool->context = context; }
    return pool;
}
int sh_audio_id_pool_close(sh_audio_id_pool *pool)
{
    if (!pool) return 1;
    if (pool->leases) return 0;
    free(pool->identities); free(pool->bindings); free(pool); return 1;
}
sh_audio_id_lease *sh_audio_id_lease_begin(sh_audio_id_pool *pool)
{
    if (!pool) return NULL;
    sh_audio_id_lease *lease = calloc(1, sizeof(*lease));
    if (lease) {
        lease->pool = pool; lease->next = pool->leases; lease->previous = &pool->leases;
        if (lease->next) lease->next->previous = &lease->next;
        pool->leases = lease;
    }
    return lease;
}
static void ar_comparisons_clear(sh_audio_id_lease *lease, const sh_audio_link_graph *graph)
{
    ar_comparison **link = &lease->comparisons;
    while (*link) {
        ar_comparison *row = *link;
        if (!graph || row->other == graph) {
            *link = row->next; sh_audio_link_graph_comparison_close(row->comparison); free(row);
        } else link = &row->next;
    }
}
static int ar_equal(sh_audio_id_lease *lease, size_t node, const ar_claim *peer, char *error, size_t capacity)
{
    const sh_audio_link_graph *other = peer->owner->graph;
    if (lease->graph == other) return sh_audio_link_graph_equal(other, node, peer->node);
    ar_comparison *row = lease->comparisons;
    while (row && row->other != other) row = row->next;
    if (!row) {
        row = calloc(1, sizeof(*row)); if (!row) return -1;
        row->comparison = sh_audio_link_graph_compare(lease->graph, other, error, capacity);
        if (!row->comparison) { free(row); return -1; }
        row->other = other; row->next = lease->comparisons; lease->comparisons = row;
    }
    return sh_audio_link_graph_compared(row->comparison, node, peer->node);
}
static ar_entry *ar_identity(sh_audio_id_pool *pool, sh_audio_identity_domain domain, uint32_t id, int original)
{
    if (!pool->capacity) return NULL;
    for (ar_entry *e = pool->identities[ar_identity_bucket(domain, id, pool->capacity)]; e; e = e->identity_next)
        if (e->id == id && (!original || !e->original || e->domain == domain)) return e;
    return NULL;
}
int sh_audio_id_reserve(void *context, const sh_audio_link_graph *graph, size_t node,
    sh_audio_identity_domain domain, uint32_t authored, const unsigned char binding[32],
    int original, int absent, uint32_t *id, char *error, size_t capacity)
{
    sh_audio_id_lease *lease = context; ar_entry *entry = NULL; ar_claim *claim = NULL;
    if (id) *id = 0;
    if (!lease || lease->active || !graph || !sh_audio_link_graph_equal(graph, node, node) || !binding || !id || !authored || domain > SH_AUDIO_ID_BANK ||
        (int)domain < 0 || (lease->graph && lease->graph != graph))
        return ar_fail(error, capacity, "invalid native audio reservation request");
    lease->graph = graph; original = !!original; absent = !!absent;
    sh_audio_id_pool *pool = lease->pool;
    if (!ar_grow(pool)) goto memory;
    size_t key = ar_key(domain, binding, original, absent) & (pool->capacity - 1);
    /* Original certificates refer to an engine name, not a private construction
     * recipe. Different native capture closures (including optional lookups)
     * must retain that same name across scopes/providers. Private reuse still
     * requires exact dependency equality below. */
    if(original) {
        entry=ar_identity(pool,domain,authored,1);
        if(entry && !entry->original)return ar_fail(error,capacity,"original audio identity is reserved by a private binding");
    }
    for (ar_entry *e = original ? NULL : pool->bindings[key]; e; e = e->binding_next) {
        if (e->domain != domain || e->original != original || e->absent != absent || memcmp(e->binding, binding, 32)) continue;
        int same = ar_equal(lease, node, e->claims, error, capacity);
        if (same < 0) {
            if (!error || !capacity || !*error) ar_fail(error, capacity, "cannot compare reserved audio binding");
            return 0;
        }
        if (same) { entry = e; break; }
    }
    if (entry) for (ar_claim *c = entry->claims; c; c = c->peer) if (c->owner == lease) { *id = entry->id; return 1; }
    claim = calloc(1, sizeof(*claim)); if (!claim) goto memory;
    if (!entry) {
        uint32_t candidate = original ? authored : ar_key(domain, binding, 0, absent);
        if (!candidate) candidate = 1;
        uint32_t start = candidate;
        for (;;) {
            /* Some logical domains share native tables. Private identities are
             * unique across all domains; originals retain their engine ABI. */
            ar_entry *occupied = ar_identity(pool, domain, candidate, original);
            int native = occupied ? 1 : pool->occupied(pool->context, domain, candidate, error, capacity);
            if (native < 0) { free(claim); return 0; }
            if (!occupied && (!native || original)) break;
            if (original) { free(claim); return ar_fail(error, capacity, "original audio identity has a different retained binding"); }
            if (!++candidate) candidate = 1;
            if (candidate == start) { free(claim); return ar_fail(error, capacity, "native audio identity space exhausted"); }
        }
        entry = calloc(1, sizeof(*entry)); if (!entry) { free(claim); goto memory; }
        entry->domain = domain; entry->id = candidate; entry->original = original; entry->absent = absent;
        memcpy(entry->binding, binding, 32);
        size_t bucket = ar_identity_bucket(domain, candidate, pool->capacity);
        entry->identity_next = pool->identities[bucket]; pool->identities[bucket] = entry;
        entry->binding_next = pool->bindings[key]; pool->bindings[key] = entry; pool->count++;
    }
    claim->owner = lease; claim->node = node; claim->entry = entry;
    claim->peer = entry->claims; claim->previous = &entry->claims;
    if (claim->peer) claim->peer->previous = &claim->peer;
    entry->claims = claim; claim->next = lease->claims; lease->claims = claim;
    *id = entry->id; return 1;
memory:
    return ar_fail(error, capacity, "cannot allocate native audio reservations");
}
int sh_audio_id_lease_ready(sh_audio_id_lease *lease, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!lease) return ar_fail(error, capacity, "audio reservation lease is missing");
    for (ar_claim *c = lease->claims; c; c = c->next) {
        ar_entry *e = c->entry;
        int state = lease->pool->occupied(lease->pool->context, e->domain, e->id, error, capacity);
        if (state < 0) return 0;
        if (!state || e->original) continue;
        int active = 0;
        for (ar_claim *peer = e->claims; peer; peer = peer->peer) active |= peer->owner->active;
        if (state > 1 || e->absent || !active) return ar_fail(error, capacity, "native audio identity became occupied before activation");
    }
    return 1;
}
void sh_audio_id_lease_activate(sh_audio_id_lease *lease)
{ if (lease) { ar_comparisons_clear(lease, NULL); lease->active = 1; } }
int sh_audio_id_lease_active(const sh_audio_id_lease *lease)
{ return lease && lease->active; }
int sh_audio_id_lease_private(const sh_audio_id_lease *lease,
    sh_audio_identity_domain domain, uint32_t id)
{
    for (const ar_claim *c = lease ? lease->claims : NULL; c; c = c->next) {
        const ar_entry *e = c->entry;
        if (e->domain == domain && e->id == id && !e->original && !e->absent) return 1;
    }
    return 0;
}
int sh_audio_id_lease_exclusive(const sh_audio_id_lease *lease,
    int (*visit)(void *, sh_audio_identity_domain, uint32_t), void *context)
{
    if (!lease || !visit) return 0;
    for (const ar_claim *c = lease->claims; c; c = c->next) {
        const ar_entry *e = c->entry; int shared = 0;
        if (e->original || e->absent) continue;
        for (const ar_claim *peer = e->claims; peer && !shared; peer = peer->peer) shared = peer->owner != lease;
        if (!shared && !visit(context, e->domain, e->id)) return 0;
    }
    return 1;
}
void sh_audio_id_lease_release(sh_audio_id_lease *lease)
{
    if (!lease) return;
    sh_audio_id_pool *pool = lease->pool;
    for (sh_audio_id_lease *other = pool->leases; other; other = other->next)
        ar_comparisons_clear(other, lease->graph);
    while (lease->claims) {
        ar_claim *c = lease->claims; ar_entry *e = c->entry; lease->claims = c->next;
        *c->previous = c->peer; if (c->peer) c->peer->previous = c->previous;
        free(c);
        if (!e->claims) {
            ar_entry **link = &pool->identities[ar_identity_bucket(e->domain, e->id, pool->capacity)];
            while (*link != e) link = &(*link)->identity_next;
            *link = e->identity_next;
            link = &pool->bindings[ar_binding_bucket(e, pool->capacity)];
            while (*link != e) link = &(*link)->binding_next;
            *link = e->binding_next; free(e); pool->count--;
        }
    }
    ar_comparisons_clear(lease, NULL);
    *lease->previous = lease->next; if (lease->next) lease->next->previous = lease->previous;
    free(lease);
}
