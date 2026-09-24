#ifndef SH_AUDIO_ENVIRONMENT_NATIVE_H
#define SH_AUDIO_ENVIRONMENT_NATIVE_H
#include "audio_limits_native.h"

typedef struct sh_audio_native_curve {
    void *points;
    uint32_t count, scaling;
} sh_audio_native_curve;
typedef struct sh_audio_environment {
    unsigned char enabled[6], padding[2];
    sh_audio_native_curve curves[6];
} sh_audio_environment;
typedef struct sh_audio_environment_change sh_audio_environment_change;
typedef struct sh_audio_environment_native {
    int (*set_environment)(sh_audio_environment *,int,int,uint32_t,const void *,uint32_t);
    int (*set_curve)(sh_audio_native_curve *,const void *,uint32_t,uint32_t);
    sh_audio_environment **environment_slot;
    void **manager_slot;
    CRITICAL_SECTION *render_lock;
    sh_audio_environment_change *pending;
} sh_audio_environment_native;
struct sh_audio_environment_change {
    sh_audio_environment_native *owner;
    sh_audio_environment *target;
    void *manager;
    sh_audio_environment before, prepared;
    DWORD thread;
    unsigned changed, installed;
    int state, uncertain;
};

int sh_audio_environment_native_bind(sh_audio_environment_native *native,
    const unsigned char *image,const sig_result *results,size_t count,
    const sh_audio_limits_native *limits);
/* Same serialized main-thread boundary as playback limits: completed bank
 * commands, no concurrent control writer or audio shutdown. Preparation uses
 * native allocations in detached storage. Publication swaps only changed
 * curves under the render lock and retains the exact previous allocations.
 * The zeroed receipt must remain stable until release/restore finishes.
 * Returns 1 applied (possibly unchanged), 0 failure fully cleaned, -1 recovery
 * still owns allocations. It never enables the overall audio route by itself. */
int sh_audio_environment_native_apply(sh_audio_environment_native *native,
    const sh_audio_shared_policy *policy,sh_audio_environment_change *change,
    char *error,size_t capacity);
int sh_audio_environment_native_ready(sh_audio_environment_change *change,char *error,size_t capacity);
/* Restore exchanges pointers without allocating/reconstructing old curves.
 * Success includes releasing detached new curves through the native allocator.
 * Unexpected writes are preserved; failure leaves ownership with the receipt. */
int sh_audio_environment_native_restore(sh_audio_environment_change *change,char *error,size_t capacity);
/* After ready, accept is a non-fallible commit marker with no engine calls.
 * Release afterward retires old native allocations; retain the receipt until
 * release succeeds. A native free fault is never retried blindly. */
void sh_audio_environment_native_accept(sh_audio_environment_change *change);
int sh_audio_environment_native_release(sh_audio_environment_change *change,char *error,size_t capacity);
#endif
