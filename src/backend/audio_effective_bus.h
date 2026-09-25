/* One native bus configuration for several simultaneously active programs. */
#ifndef SH_AUDIO_EFFECTIVE_BUS_H
#define SH_AUDIO_EFFECTIVE_BUS_H
#include "audio_bus_policy.h"

/* baseline is the native bus configuration (a program's bound baseline
 * policy); programs are bound policies in precedence order: the current map,
 * then the local library, then retained occurrence providers. Per key the
 * highest-precedence edited value applies and unedited keys keep the native
 * value, so a departed program's edits return to the baseline.
 * A bus the baseline lacks takes its kind from the highest-precedence program
 * defining it; programs disagreeing on that kind do not contribute to it.
 * Parent choices that would close a cycle fall back to the next alternative
 * (lower precedence, then native); a bus with no acyclic choice fails. A live
 * curve keeps a live subscription mode and a live state mapping keeps its
 * group; a mode without curves is removed as the engine removes it.
 * Output is sorted like a compiled policy and owns its bodies: recipe names
 * the source (0 baseline, i+1 program i), edited marks values that differ from
 * the baseline, and removals of keys the baseline lacks are omitted. */
int sh_audio_effective_bus(const sh_audio_bus_policy *baseline, const sh_audio_bus_policy *const *programs,
    size_t count, sh_audio_bus_policy *out, char *error, size_t capacity);
/* Kind of a bus in a policy, or 0 when the policy does not define it. */
unsigned sh_audio_effective_bus_kind(const sh_audio_bus_policy *policy, uint32_t bus);
/* Any setting that differs from the native baseline. */
int sh_audio_effective_bus_edited(const sh_audio_bus_policy *policy);
/* Owned deep copy of settings, bodies and bus kinds (contributors omitted). */
int sh_audio_effective_bus_copy(const sh_audio_bus_policy *policy, sh_audio_bus_policy *out);
#endif
