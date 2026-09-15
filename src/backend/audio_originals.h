/* Immutable installed audio originals, independent of authored overrides. */
#ifndef SH_AUDIO_ORIGINALS_H
#define SH_AUDIO_ORIGINALS_H
#include "package_compiler.h"

typedef struct sh_audio_originals sh_audio_originals;
/* Captures the current native language, not an author option. Metadata and
 * original file handles are acquired lazily and remain stable until close.
 * NULL language represents uninitialized audio: unrelated paths still pass
 * through, while audio queries report an initializing error. */
sh_audio_originals *sh_audio_originals_open(const char *doom_base, const wchar_t *language);
void sh_audio_originals_close(sh_audio_originals *originals);
/* Compiler identity-reader contract. Only the native PC audio path is claimed.
 * Packed entries precede loose originals; differing packed candidates are an
 * ambiguity, never a directory-order choice. Scope 2 means extra activation is
 * needed even though the original bytes are installed and available. */
int sh_audio_originals_identity(void *context, const char *path,
    sh_package_original_identity *out, char *error, size_t capacity);

/* Whether this index can answer queries: it exists and captured a language. */
int sh_audio_originals_ready(void *context);

/* Read a bounded span of the effective installed original for an engine path,
 * in the same packed-before-loose order as the identity reader. 1 filled the
 * span, 0 no installed original, -1 unreadable or ambiguous. length receives
 * the whole original size. Reading a header never hashes the payload. */
int sh_audio_originals_read(void *context, const char *path, uint64_t offset,
    void *out, size_t span, uint64_t *length, char *error, size_t capacity);

/* Visit each bank identity carried by the mounted packages that serve the
 * current language or no language. The visitor returns 0 to stop early.
 * 1 complete, 0 stopped, -1 package metadata is unreadable. */
typedef int (*sh_audio_originals_bank_visit)(void *context, uint32_t id, uint32_t language);
int sh_audio_originals_packaged_banks(void *context,
    sh_audio_originals_bank_visit visit, void *visitor, char *error, size_t capacity);
#endif
