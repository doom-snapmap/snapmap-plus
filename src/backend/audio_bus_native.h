/* Native bus construction, configuration and restore from the effective
 * shared bus policy. */
#ifndef SH_AUDIO_BUS_NATIVE_H
#define SH_AUDIO_BUS_NATIVE_H
#include "audio_subscription_native.h"
#include "audio_bus_policy.h"
#include "audio_bank_commands.h"
#include "audio_state_native.h"

/* Normal and auxiliary buses share one 0x168-byte layout: ID +0x10,
 * reference count +0x14, parameter-curve consumer +0x18 (its subscription
 * mask object at +0x20), state groups +0x30, effect slots +0x50, activity
 * +0x58, parent bus +0x68, property bundle +0x70, instance limit +0x78, flags
 * +0x7a/+0x7b/+0x160, mixer +0x98, duck recovery samples +0xb0, channel
 * configuration +0xb4, duck floor volume +0xb8 and the duck list +0xc0. Only
 * whole native operations change them: constructors, parent attach/detach,
 * the setters the bank parser uses, the state and duck helpers, and the
 * shared subscription adapter for curves. */
typedef struct sh_audio_bus_owned { uint32_t id; void *bus; } sh_audio_bus_owned;
typedef struct sh_audio_bus_change sh_audio_bus_change;
struct sh_audio_bus_op;
struct sh_audio_bus_target;
typedef struct sh_audio_bus_native {
    sh_audio_subscription_native *subscriptions;
    CRITICAL_SECTION *render_lock;
    void **manager_slot;
    unsigned char **index_slot;
    void **state_slot, **transition_slot;
    void *(*lookup)(void *table, uint32_t id);            /* referenced object or NULL */
    void *(*construct)(void *memory, uint32_t id);        /* shared normal/aux construction */
    int (*initialize)(void *bus);                          /* field reset and index registration */
    void *(*create_aux)(uint32_t id);
    const void *normal_vtable, *aux_vtable, *consumer_vtable;
    void **roots[2];
    const uint32_t *sample_rate;
    const uint16_t *duck_floor;
    const int32_t *property_parameters;                    /* property -> curve parameter, 64 none */
    int (*set_effect)(void *bus, uint32_t slot, uint32_t id, char shared, int priority);
    int (*set_bypass)(void *bus, uint32_t bits, uint32_t mask);
    void (*set_limit_kill)(void *bus, unsigned char value);
    void (*set_limit_virtual)(void *bus, unsigned char value);
    void (*set_channels)(void *bus, uint32_t configuration);
    void (*add_background)(void *bus);
    void (*remove_background)(void *bus);
    int (*set_duck)(void *bus, uint32_t target, uint32_t volume, uint32_t fade_out, uint32_t fade_in,
        uint32_t curve, int property);
    char (*rtpc_ready)(void *consumer);
    int (*rtpc_mark)(void *consumer, int parameter);
    void *(*state_group)(void *bus, uint32_t group, char notify);
    int (*state_bind)(void *group, uint32_t object, uint32_t value, char notify);
    void *(*state_unbind)(void *group, uint32_t value);
    int (*state_unregister)(void *manager, uint32_t group, void *object);
    int (*transition_unregister)(void *manager, void *entry, void *group);
    sh_audio_bus_change *pending;
    /* New buses this adapter keeps alive, and native buses whose configuration
     * an accepted publication changed from the baseline. */
    sh_audio_bus_owned *owned;
    size_t owned_count;
    uint32_t *touched;
    size_t touched_count;
} sh_audio_bus_native;

/* One publication's receipt: targets, reversible operations, the curve
 * subscription change and the ownership an accept commits. */
struct sh_audio_bus_change {
    sh_audio_bus_native *owner;
    const sh_audio_bus_policy *desired;
    struct sh_audio_bus_target *targets;
    size_t target_count;
    struct sh_audio_bus_op *ops;
    size_t op_count, op_capacity;
    sh_audio_subscription_change subscriptions;
    sh_audio_subscription_request *requests;
    sh_audio_subscription_curve *curves;
    unsigned char **request_buses;
    size_t request_count, curve_count;
    sh_audio_bus_owned *owned;          /* ownership replaced at accept */
    size_t owned_count;
    uint32_t *touched;
    size_t touched_count;
    int new_only;
    void *manager;
    unsigned char *index;
    DWORD thread;
    int pool, state, uncertain;
};
/* Resolved from the verified auxiliary constructor signature and the bus
 * vtable's cooked readers; cross-checks the pool, index, parameter, state and
 * curve bindings other adapters already verified. Never dereferences live
 * engine state beyond the mapped image. Returns 0 on any mismatch. */
int sh_audio_bus_native_bind(sh_audio_bus_native *native, const unsigned char *image,
    const sig_result *results, size_t count, sh_audio_subscription_native *subscriptions,
    const sh_audio_limits_native *limits, const sh_audio_registry *registry, const sh_audio_state_native *state);

/* Identity phase, before private banks load and without waiting on bank
 * commands: under the render lock, constructs missing buses of the policy's
 * kind, adopts live non-native ones, checks every live bus's native kind and
 * attaches changed parents (new buses first; a bus never becomes a root).
 * baseline marks native buses. new_only limits the work to buses the native
 * configuration lacks (additive admission). effective must stay valid until
 * the receipt ends. Returns 1 prepared (possibly nothing), 0 fully reverted
 * failure, -1 retained recovery ownership. */
int sh_audio_bus_native_identities(sh_audio_bus_native *native, const sh_audio_bus_policy *effective,
    const sh_audio_bus_policy *baseline, int new_only, sh_audio_bus_change *change, char *error, size_t capacity);
/* Configuration phase, after private banks load, inside the publication gate:
 * converges properties, settings, ducking, effects, mixer, flags, states and
 * curves of every target bus through native operations, notifying consumers
 * as the engine does. Same results as identities. */
int sh_audio_bus_native_apply(sh_audio_bus_change *change, char *error, size_t capacity);
int sh_audio_bus_native_ready(sh_audio_bus_change *change, char *error, size_t capacity);
/* Reverses configuration then identities; constructed buses are released
 * (they must have no children: unload newly loaded banks first). */
int sh_audio_bus_native_restore(sh_audio_bus_change *change, char *error, size_t capacity);
/* Commits ownership: owned new buses and touched native buses. */
void sh_audio_bus_native_accept(sh_audio_bus_change *change);
/* After accept: frees displaced native allocations and releases new buses the
 * policy no longer contains (the engine keeps them while still in use). */
int sh_audio_bus_native_release(sh_audio_bus_change *change, char *error, size_t capacity);
/* Whether the adapter holds published bus state a later policy must revisit. */
int sh_audio_bus_native_owns(const sh_audio_bus_native *native);
#endif
