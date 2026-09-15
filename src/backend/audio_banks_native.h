#ifndef SH_AUDIO_BANKS_NATIVE_H
#define SH_AUDIO_BANKS_NATIVE_H
#include "signatures.h"
#include "package_audio.h"
/* Bind before installing the declaration publication hooks. */
void sh_audio_banks_native_install(const sig_result *results, size_t count);
/* Main-thread provider activation/recovery. Event IDs come from prepared native
 * sound shaders, including aliases, rather than hashes guessed from paths.
 * The bank inventory selects overrides and closes over the banks a wanted bank
 * names; the media inventory reports which changed payloads the file provider
 * serves and which a loaded bank already carries in its own data. */
int sh_audio_banks_native_activate(const uint32_t *events, size_t count,
    const sh_package_audio_bank *banks, size_t bank_count,
    const sh_package_audio_media *media, size_t media_count,
    char *error, size_t capacity);
#endif
