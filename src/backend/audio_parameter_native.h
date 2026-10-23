#ifndef SH_AUDIO_PARAMETER_NATIVE_H
#define SH_AUDIO_PARAMETER_NATIVE_H
#include "audio_limits_native.h"

typedef struct sh_audio_parameter_vector { void *items;uint32_t count,capacity; } sh_audio_parameter_vector;
typedef struct sh_audio_parameter_node {
    uint32_t id,padding;
    struct sh_audio_parameter_node *next;
    uint32_t settings[4];
    void *vtable;
    uint32_t value;
    unsigned char has_value,padding2[3];
    sh_audio_parameter_vector values;
    uint64_t reserved;
    void *transitions;
    sh_audio_parameter_vector subscribers[2];
} sh_audio_parameter_node;
typedef struct sh_audio_parameter_table {
    sh_audio_parameter_node *buckets[193];
    uint32_t count,padding;
} sh_audio_parameter_table;
typedef struct sh_audio_parameter_change sh_audio_parameter_change;
typedef struct sh_audio_parameter_row sh_audio_parameter_row;
typedef struct sh_audio_parameter_native {
    sh_audio_parameter_node *(*ensure)(sh_audio_parameter_table *,uint32_t);
    void (*destroy)(sh_audio_parameter_node *);
    void *(*allocate)(int,size_t);
    void (*release)(int,void *);
    int *pool;
    sh_audio_parameter_table **parameter_slot;
    void **manager_slot;
    CRITICAL_SECTION *render_lock;
    sh_audio_parameter_change *pending;
} sh_audio_parameter_native;
struct sh_audio_parameter_change {
    sh_audio_parameter_native *owner;
    sh_audio_parameter_table *target,before,after;
    sh_audio_parameter_row *rows;
    size_t count;
    sh_audio_parameter_vector old_bindings[6],new_bindings[6];
    uint32_t *expected[6];
    void *manager;
    DWORD thread;
    int pool,state,uncertain,table_installed;
    unsigned changed_bindings,installed_bindings;
};
int sh_audio_parameter_native_bind(sh_audio_parameter_native *native,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits);
/* Same serialized main-thread publication/render boundary as other shared
 * controls. Settings never overwrite per-object/current values or subscribers.
 * Represented parameters receive their complete effective built-in binding:
 * absence means unbound after compile-time bank no-op composition. Unrepresented
 * native parameters/bindings remain untouched. Preparation allocates all native
 * nodes and changed vectors before publication. Receipt storage must stay stable.
 * Returns 1 applied, 0 fully cleaned failure, -1 retained recovery ownership. */
int sh_audio_parameter_native_apply(sh_audio_parameter_native *native,const sh_audio_shared_policy *policy,
    sh_audio_parameter_change *change,char *error,size_t capacity);
int sh_audio_parameter_native_ready(sh_audio_parameter_change *change,char *error,size_t capacity);
/* Newly admitted nodes cannot retire with live subscribers, values or active
 * interpolation. Detach prepared consumers before retrying retained recovery. */
int sh_audio_parameter_native_restore(sh_audio_parameter_change *change,char *error,size_t capacity);
void sh_audio_parameter_native_accept(sh_audio_parameter_change *change);
/* After acceptance, live nodes/vectors belong to the native manager. Only
 * detached prior vectors retire here; no playing consumer is reset/stopped. */
int sh_audio_parameter_native_release(sh_audio_parameter_change *change,char *error,size_t capacity);
#endif
