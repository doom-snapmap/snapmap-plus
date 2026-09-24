/* One native shared-audio configuration for several simultaneously active programs. */
#ifndef SH_AUDIO_EFFECTIVE_POLICY_H
#define SH_AUDIO_EFFECTIVE_POLICY_H
#include "audio_shared_policy.h"

/* programs are bound policies in precedence order: the current map, then the
 * local library, then retained occurrence providers. baseline is the native
 * Init configuration (may be NULL/empty when Init has no shared chunks).
 * Per key the highest-precedence edited value applies, so a map's authored
 * controls win over local edits without discarding unrelated local keys.
 * Playback limits use the same compatibility rule as one program (largest
 * voice count, lowest valid threshold) across every program and the baseline.
 * Keys no program edits return to the baseline, so leaving a map restores the
 * native values. Keys absent from the baseline and every program stay native.
 * Platform labels must agree. Output is owned, sorted like a compiled policy;
 * edited=1 only where the value differs from the baseline. input names the
 * source (0 baseline, i+1 program i) and index its winning record. */
int sh_audio_effective_shared(const sh_audio_shared_policy *baseline,
    const sh_audio_shared_policy *const *programs, size_t count,
    sh_audio_shared_policy *out, char *error, size_t capacity);
/* Any setting that differs from the native baseline. */
int sh_audio_effective_shared_edited(const sh_audio_shared_policy *policy);
#endif
