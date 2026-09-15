#ifndef BACKEND_AUDIO_FILES_NATIVE_H
#define BACKEND_AUDIO_FILES_NATIVE_H
#include <stddef.h>
#include <wchar.h>
#include "signatures.h"

/* Atomically install the native audio resolver's two open slots. Existing
 * descriptors continue through native read/close and keep their original bytes.
 * This routes new file opens; bank activation/retirement is a separate stage. */
int sh_audio_files_native_install(const sig_result *results, size_t count,
    const unsigned char *image);
int sh_audio_files_native_ready(void);
/* Copy the engine's current language for immutable original-file lookup. No
 * native mutation or caller-retained pointer into the engine string buffer. */
int sh_audio_files_native_language(wchar_t *out, size_t capacity);
/* Native bank-name lookup order: current locale, then nonlocalized fallback.
 * Owned canonical paths; no file open or native loading. */
int sh_audio_files_native_bank_paths(const char *filename, char **localized, char **fallback);
/* Copy the engine's configured bank path prefix, relative to the virtual
 * sound/soundbanks/pc root and carrying its own trailing separator when the
 * engine configured one. An empty prefix means banks sit at that root. */
int sh_audio_files_native_bank_prefix(char *out, size_t capacity);

#endif
