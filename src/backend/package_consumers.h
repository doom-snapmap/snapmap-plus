/* Immutable constructor bindings retained with a compiled resource provider. */
#ifndef SH_PACKAGE_CONSUMERS_H
#define SH_PACKAGE_CONSUMERS_H
#include <stddef.h>

typedef struct sh_package_consumer_target {
    const char *slot, *family, *name;
} sh_package_consumer_target;
typedef struct sh_package_consumer {
    const char *family, *name;
    const sh_package_consumer_target *targets;
    size_t count;
} sh_package_consumer;
typedef struct sh_package_consumers sh_package_consumers;

/* Inputs use exact compiler-assigned native identities, not author package IDs.
 * Own all strings on success. Sort slots, collapse exact repeats and reject
 * contradictory slots/owner records. A zero-target record explicitly describes
 * an owner with no captured requests. NULL views represent an empty catalog.
 * No native calls, source reads, alias assignment or engine mutation occur. */
sh_package_consumers *sh_package_consumers_create(const sh_package_consumer *owners,
    size_t count, char *error, size_t capacity);
void sh_package_consumers_free(sh_package_consumers *view);
size_t sh_package_consumers_count(const sh_package_consumers *view);
const sh_package_consumer *sh_package_consumers_at(const sh_package_consumers *view, size_t index);
const sh_package_consumer *sh_package_consumers_find(const sh_package_consumers *view,
    const char *family, const char *name);
const sh_package_consumer_target *sh_package_consumer_find(const sh_package_consumer *owner, const char *slot);
/* A constructor request slot is request/<family>/<original native name>.
 * Match the manager's case-insensitive, slash-normalized identity without
 * allocating or retaining engine pointers. The target stays in that manager. */
const char *sh_package_consumer_request(const sh_package_consumer *owner,
    const char *family, const char *name);
/* Overlapping native owner identities must have the same COMPLETE target set;
 * this never appends peer bindings or supplies a missing slot from another map. */
sh_package_consumers *sh_package_consumers_join(const sh_package_consumers *a,
    const sh_package_consumers *b, char *error, size_t capacity);
/* Select live physical owners, then retain their captured consumer closure,
 * including source-free intermediate owners and cycles. Negative keep aborts. */
sh_package_consumers *sh_package_consumers_filter(const sh_package_consumers *view,
    int (*keep)(void *, const char *, const char *), void *context, char *error, size_t capacity);
int sh_package_consumers_equal(const sh_package_consumers *a, const sh_package_consumers *b,
    const char *family, const char *name);
#endif
