#ifndef SH_AUDIO_LIMITS_NATIVE_H
#define SH_AUDIO_LIMITS_NATIVE_H
#include "signatures.h"
#include "audio_shared_policy.h"

typedef struct sh_audio_limits_value {
    float threshold, amplitude;
    int threshold_priority, voices_priority;
    uint16_t voices;
} sh_audio_limits_value;
typedef struct sh_audio_limits_change sh_audio_limits_change;
typedef struct sh_audio_limits_native {
    int (*set_threshold)(float, int);
    int (*set_voices)(uint16_t, int);
    float *threshold, *amplitude;
    int *threshold_priority, *voices_priority;
    uint16_t *voices;
    void **manager_slot;
    CRITICAL_SECTION *render_lock;
    sh_audio_limits_change *pending;
} sh_audio_limits_native;
struct sh_audio_limits_change {
    sh_audio_limits_native *owner;
    sh_audio_limits_value before, after;
    DWORD thread;
    unsigned changed;
    int uncertain;
};

/* Bind without dereferencing live globals or calling the mapped image. Bind
 * once before use; a pending change must be restored or accepted first. */
int sh_audio_limits_native_bind(sh_audio_limits_native *native,
    const unsigned char *image, const sig_result *results, size_t count);
/* Main-thread, serialized provider boundary, after native bank submissions have
 * completed. The engine render lock excludes playback processing during each
 * operation; it must never be held while waiting for a native bank command.
 * Other bank/control writers must not run between apply and restore/accept.
 * Only the two STMG playback constraints are applied here. Other shared controls
 * and buses have separate ownership operations in the enclosing transaction.
 * The zeroed change must remain at a stable address until restored or accepted.
 * 1 applied, 0 unchanged/restored failure, -1 restore owed after a native fault.
 * A native setter's success code alone never establishes successful publication. */
int sh_audio_limits_native_apply(sh_audio_limits_native *native,
    const sh_audio_shared_policy *policy, sh_audio_limits_change *change,
    char *error, size_t capacity);
/* Restores only owned changes, including exact derived amplitude and priorities.
 * Unexpected intervening writes are preserved and leave the receipt pending.
 * A failed restore never grants permission to retire its provider. */
int sh_audio_limits_native_restore(sh_audio_limits_change *change,
    char *error, size_t capacity);
/* Validate at the final commit boundary. No competing writer may intervene
 * before accept. Accept releases the receipt and performs no engine operation. */
int sh_audio_limits_native_ready(sh_audio_limits_change *change,
    char *error, size_t capacity);
void sh_audio_limits_native_accept(sh_audio_limits_change *change);
#endif
