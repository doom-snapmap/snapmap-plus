/* Compiled sound identities retain the authored Wwise event association. */
#ifndef SH_PACKAGE_SOUND_H
#define SH_PACKAGE_SOUND_H
#include "package_compiler.h"

/* Prepare private compiler output before publication. Original paths are
 * compiler-owned provenance. Existing typed event relocations are preserved.
 * No bank presence, object closure, or native playback is certified here.
 * Failure leaves every resource unchanged. */
int sh_package_sound_prepare(sh_package_compilation *compiled, char *error, size_t capacity);
/* Exact native identity lookup; returns only a copied scalar, with no resource
 * load, allocation, or source read. out is cleared when there is no mapping. */
int sh_package_sound_event(const sh_package_compilation *compiled, const char *name, uint32_t *out);
/* A differing alias conversion requires the common native initializer hook. */
int sh_package_sound_requires_binding(const sh_package_compilation *compiled);
/* For a typed bank relocation on private compiler output. The caller owns the
 * exact resource/bank binding and must have prepared its complete bank closure.
 * Expected authored event prevents applying another binding's relocation. */
int sh_package_sound_rebind(sh_compiled_resource *resource, uint32_t authored, uint32_t event);
#endif
