#ifndef BACKEND_AUDIO_FILES_H
#define BACKEND_AUDIO_FILES_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

/* Native Windows stream descriptor, shared by the supported renderers. A loose
 * descriptor has sector/block/custom fields zero and owns its OS handle. */
typedef struct sh_audio_file_descriptor {
    uint64_t length;
    uint32_t sector, block;
    void *custom;
    void *handle;
    uint32_t device;
} sh_audio_file_descriptor;

typedef int (*sh_audio_file_provider)(void *context, const char *path,
    FILE **stream, uint64_t *length);

/* Reproduce the native bank/media suffix and language rules at the virtual
 * sound/soundbanks/pc root. flags is borrowed native storage: company at +0,
 * codec at +4 and language-specific byte at +24. No other fields are read.
 * 1 returns an owned canonical path, 0 leaves an external/unsupported request
 * native, -1 reports allocation/encoding failure. No filesystem access. */
int sh_audio_file_path(const wchar_t *name, uint32_t id, int by_id,
    const void *flags, const wchar_t *bank_prefix, const wchar_t *media_prefix,
    const wchar_t *language, char **path);

/* Open the immutable provider snapshot, duplicate its verified OS handle and
 * release the CRT stream. Native read/close now own the duplicate. Descriptor
 * and synchronous flag remain untouched unless the complete open succeeds.
 * Return 1 claimed, 0 unowned, -1 failed. No compiled-provider lock is retained. */
int sh_audio_file_open(const char *path, uint32_t device,
    sh_audio_file_provider provider, void *context,
    unsigned char *synchronous, sh_audio_file_descriptor *out);

#endif
