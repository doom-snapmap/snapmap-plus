/* Native switch-group configuration from the effective shared audio policy. */
#ifndef SH_AUDIO_SWITCH_NATIVE_H
#define SH_AUDIO_SWITCH_NATIVE_H
#include "audio_subscription_native.h"
#include "audio_shared_policy.h"

/* Switch nodes are 0x58-byte entries in their manager's 193-bucket table
 * (count +0x608): ID +0, next +8, authored switch values +0x10, controller ID
 * +0x20 and type +0x24, attached consumers +0x28 and per-object switch values
 * +0x38. A mapped group subscribes the node itself (parameter 0x40, category
 * 6, selector 1, curve 0) with points whose outputs are the value indexes
 * 0..n-1, while the node keeps the authored switch value IDs; a refresh
 * notifies the consumers of values[index]. */
typedef struct sh_audio_switch_node {
    uint32_t id, padding;
    struct sh_audio_switch_node *next;
    sh_audio_parameter_vector values;
    uint32_t controller, type;
    sh_audio_parameter_vector consumers;
    void *vtable;
    uint32_t value;
    unsigned char has_value, padding2[3];
    sh_audio_parameter_vector entries;
} sh_audio_switch_node;
typedef struct sh_audio_switch_table {
    sh_audio_switch_node *buckets[193];
    uint32_t count, padding;
} sh_audio_switch_table;
typedef struct sh_audio_switch_original {
    uint32_t group;
    unsigned char *record; /* complete STMG switch record; NULL when unmapped */
    size_t length;
} sh_audio_switch_original;
typedef struct sh_audio_switch_row sh_audio_switch_row;
typedef struct sh_audio_switch_change sh_audio_switch_change;
typedef struct sh_audio_switch_native {
    sh_audio_subscription_native *subscriptions;
    CRITICAL_SECTION *render_lock;
    void **manager_slot;
    sh_audio_switch_change *pending;
    /* Groups an accepted publication edited, with the mapping the engine had
     * before the first such edit. A later policy that no longer edits a group
     * returns it to this original. */
    sh_audio_switch_original *originals;
    size_t original_count;
} sh_audio_switch_native;
struct sh_audio_switch_change {
    sh_audio_switch_native *owner;
    sh_audio_subscription_change subscriptions;
    sh_audio_subscription_request *requests;
    sh_audio_subscription_curve *curves;
    sh_audio_switch_row *rows;
    size_t count;
    sh_audio_switch_original *originals; /* replaces the adapter's at accept */
    size_t original_count;
    sh_audio_switch_table *table;
    void *manager;
    DWORD thread;
    int pool, state, uncertain;
};
/* Uses the subscription adapter's switch manager and node constructor and the
 * playback-settings render lock. Never dereferences live engine state. */
int sh_audio_switch_native_bind(sh_audio_switch_native *native, sh_audio_subscription_native *subscriptions,
    const sh_audio_limits_native *limits);
/* Same serialized main-thread boundary as the other shared-control legs,
 * after them in native STMG order (before parameters). Edited groups receive
 * their complete effective mapping; groups an earlier publication edited
 * return to their original once the policy no longer edits them; every other
 * group stays as the engine has it. Unchanged groups keep their allocations
 * and subscription. Changed nodes keep their identity, consumers and
 * per-object values; missing nodes are constructed detached first. The mapping
 * and subscription change together and consumers are notified once, after
 * both are live. Returns 1 applied (no receipt when nothing is represented),
 * 0 fully cleaned failure, -1 retained recovery ownership. */
int sh_audio_switch_native_apply(sh_audio_switch_native *native, const sh_audio_shared_policy *policy,
    sh_audio_switch_change *change, char *error, size_t capacity);
int sh_audio_switch_native_ready(sh_audio_switch_change *change, char *error, size_t capacity);
/* Reinstates previous node mappings and subscriptions, notifies consumers and
 * removes nodes this change constructed while nothing attached to them. */
int sh_audio_switch_native_restore(sh_audio_switch_change *change, char *error, size_t capacity);
/* Commits the original mappings of edited groups. */
void sh_audio_switch_native_accept(sh_audio_switch_change *change);
/* After accept: frees displaced value arrays and subscription records. */
int sh_audio_switch_native_release(sh_audio_switch_change *change, char *error, size_t capacity);
#endif
