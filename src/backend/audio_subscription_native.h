/* Transactional replacement of native parameter-curve subscriptions. */
#ifndef SH_AUDIO_SUBSCRIPTION_NATIVE_H
#define SH_AUDIO_SUBSCRIPTION_NATIVE_H
#include "audio_parameter_native.h"
#include "audio_environment_native.h"

/* A subscription is one 0x60-byte record per (consumer, parameter) in the
 * parameter manager's second 193-bucket table (+0x610, count +0xc18): consumer
 * +0, parameter +8, next +0x10, context key +0x18..0x47, consumer category
 * +0x48, accumulation mode +0x4c and a vector of 24-byte curves at +0x50. Each
 * curve holds curve ID, controller ID and a native curve descriptor. Controllers
 * of type 2 are modulator objects; every other type is a parameter node whose
 * two sorted reverse lists (+0x50/+0x60) name the record. Switch groups (category
 * 6, parameter 0x40) and bus parameter curves (category 2) share this machinery. */
typedef struct sh_audio_subscription_native {
    sh_audio_parameter_native *parameters;  /* table slot, node ensure, pool, lock */
    void **modulator_slot;
    int (*reverse_add)(sh_audio_parameter_node *node, void *record, char selector);
    char (*reverse_remove)(sh_audio_parameter_node *node, void *record);
    int (*modulator_add)(void *manager, uint32_t controller, void *record);
    void (*modulator_remove)(void *manager, void *record, uint32_t controller);
    void (*refresh)(sh_audio_parameter_table *parameters, void *record, char flag);
    int (*set_curve)(sh_audio_native_curve *curve, const void *points, uint32_t count, uint32_t scaling);
    /* Switch groups: the manager slot and detached node constructor. */
    void **switch_slot;
    void *(*switch_ensure)(void *table, uint32_t id);
} sh_audio_subscription_native;

/* Bind from the verified STMG reader (switch manager/setter call sites), the
 * setter's construction chain and the common subscription core's call sites.
 * Cross-checks the parameter manager, pool allocator and curve setter already
 * bound by the parameter and environment adapters. Never dereferences live
 * engine state. Returns 0 when any instruction shape differs. */
int sh_audio_subscription_native_bind(sh_audio_subscription_native *native, const unsigned char *image,
    const sig_result *results, size_t count, sh_audio_parameter_native *parameters,
    const sh_audio_environment_native *environment);

typedef struct sh_audio_subscription_curve {
    int32_t curve;
    uint32_t controller, type;
    const void *points; /* twelve-byte native points, copied by the curve setter */
    uint32_t count, scaling;
} sh_audio_subscription_curve;
/* One complete desired curve set for (consumer, parameter). An empty set
 * removes the subscription, as the native core does for its final curve. */
typedef struct sh_audio_subscription_request {
    void *consumer;
    uint32_t parameter, category, mode;
    char selector;
    const sh_audio_subscription_curve *curves;
    size_t count;
} sh_audio_subscription_request;
typedef struct sh_audio_subscription_row sh_audio_subscription_row;
typedef struct sh_audio_subscription_nodes sh_audio_subscription_nodes;
typedef struct sh_audio_subscription_change {
    sh_audio_subscription_native *owner;
    sh_audio_subscription_row *rows;
    size_t count;
    sh_audio_subscription_nodes *nodes;
    size_t node_count;
    sh_audio_parameter_table *parameters;
    void *modulators;
    int pool, state, uncertain;
} sh_audio_subscription_change;
/* Detached preparation: new records, curve vectors and native curve copies.
 * Requests must name distinct (consumer, parameter) pairs. No live write. */
int sh_audio_subscription_prepare(sh_audio_subscription_native *native,
    const sh_audio_subscription_request *requests, size_t count,
    sh_audio_subscription_change *change, char *error, size_t capacity);
/* Caller holds the audio render lock (and the bank publication gate). Ensures
 * parameter controllers and grows their reverse lists the way the native
 * registration would, so registration itself cannot allocate; grown lists stay
 * native-owned. Then swaps each record without notifying consumers, so callers
 * can publish the state a refresh reads first. A modulator registration
 * failure reverts every swap already made. Returns 1 published, 0 fully
 * reverted, -1 when reversal itself failed (receipt retained). */
int sh_audio_subscription_publish(sh_audio_subscription_change *change, char *error, size_t capacity);
/* Caller holds the render lock. Notifies consumers of the live side: the new
 * records after publish, the previous ones after restore. Removed
 * subscriptions keep their consumer's last value, as in the engine. */
int sh_audio_subscription_refresh(sh_audio_subscription_change *change);
/* Caller holds the render lock. Checks that every published record is still
 * the one linked for its consumer and parameter. */
int sh_audio_subscription_ready(const sh_audio_subscription_change *change);
/* Caller holds the render lock. Reserves the previous records' parameter lists
 * first (a failure changes nothing), then reinstates the previous records and
 * their registrations and removes parameter nodes this change created while
 * they remain idle. Consumers are not notified; see refresh. */
int sh_audio_subscription_restore(sh_audio_subscription_change *change, char *error, size_t capacity);
/* Frees whichever side is no longer live: the displaced records after a
 * published change, or the prepared ones after restore/failure. */
void sh_audio_subscription_release(sh_audio_subscription_change *change);

/* Live inspection; the caller holds the render lock. valid walks the record
 * table once with bounded chains and checks its count and bucket keys. */
int sh_audio_subscription_valid(const sh_audio_subscription_native *native);
/* 1 when the live record of the request's consumer and parameter has exactly
 * the requested curves (IDs, controllers, native points and scaling), category
 * and mode, or when an empty request finds no record. */
int sh_audio_subscription_matches(const sh_audio_subscription_native *native,
    const sh_audio_subscription_request *request);
/* Curve count of the live record for (consumer, parameter), 0 without one; the
 * first curve is copied to first (points remain native memory, type 0). */
size_t sh_audio_subscription_live(const sh_audio_subscription_native *native, const void *consumer,
    uint32_t parameter, sh_audio_subscription_curve *first);
#endif
