/* Shared sound initializer binding for parse, construction, and preview. */
#ifndef SH_PACKAGE_SOUND_NATIVE_H
#define SH_PACKAGE_SOUND_NATIVE_H
#include "signatures.h"
/* Startup only, while native target execution is quiescent. Failed removal
 * retains trampoline ownership so a later install can safely retry it. */
int sh_package_sound_native_install(const sig_result *results, size_t count);
int sh_package_sound_native_ready(void);
#endif
