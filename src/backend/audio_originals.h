/* Immutable installed audio originals, independent of authored overrides. */
#ifndef SH_AUDIO_ORIGINALS_H
#define SH_AUDIO_ORIGINALS_H
#include "package_compiler.h"
#include "audio_banks.h"

typedef struct sh_audio_originals sh_audio_originals;
/* Captures the current native language, not an author option. Metadata and
 * original file handles are acquired lazily and remain stable until close.
 * NULL language represents uninitialized audio: unrelated paths still pass
 * through, while audio queries report an initializing error. */
/* bank_prefix is the engine's configured bank directory prefix, lowercased
 * with forward slashes and a trailing separator, or NULL/empty for the
 * ordinary root. Discovery and identity reads both use it. */
sh_audio_originals *sh_audio_originals_open(const char *doom_base, const wchar_t *language,
    const char *bank_prefix);
void sh_audio_originals_close(sh_audio_originals *originals);
/* Compiler identity-reader contract. Only the native PC audio path is claimed.
 * Packed entries precede loose originals; differing packed candidates are an
 * ambiguity, never a directory-order choice. Scope 2 means extra activation is
 * needed even though the original bytes are installed and available. */
int sh_audio_originals_identity(void *context, const char *path,
    sh_package_original_identity *out, char *error, size_t capacity);
/* Where SnapMap's own sound init registers the bank at this engine path.
 * phase: 0 Init.bnk, 1 prepared doom_initial.bnk, 2 root listing, 3 names
 * only the language directory lists. position orders one phase by that
 * directory's unsorted file-system listing, the engine's own ListFiles order;
 * SIZE_MAX for an authored name the installed directory does not list, which
 * has no native position (callers must treat its order as a compiler choice).
 * Earlier registration wins only for events (R039); other kinds may reparse.
 * An authored file is ranked as if installed at that path; authored_root
 * reports an authored root file of the same name. 1 registered, 0 SnapMap
 * never registers that path, -1 unavailable. */
typedef struct sh_audio_native_rank { unsigned phase; size_t position; } sh_audio_native_rank;
int sh_audio_originals_registration(sh_audio_originals *originals, const char *path,
    int authored, int authored_root, sh_audio_native_rank *rank, char *error, size_t capacity);

/* Whether this index can answer queries: it exists and captured a language. */
int sh_audio_originals_ready(void *context);
int sh_audio_originals_matches(const sh_audio_originals *originals,
    const char *language, const char *bank_prefix);

/* Read a bounded span of the effective installed original for an engine path,
 * in the same packed-before-loose order as the identity reader. 1 filled the
 * span, 0 no installed original, -1 unreadable or ambiguous. length receives
 * the whole original size. Reading a header never hashes the payload. */
int sh_audio_originals_read(void *context, const char *path, uint64_t offset,
    void *out, size_t span, uint64_t *length, char *error, size_t capacity);

/* Visit each bank identity carried by the mounted packages that serve the
 * current language or no language. The visitor returns 0 to stop early.
 * It runs under the originals lock and must not call back into this module.
 * 1 complete, 0 stopped, -1 package metadata is unreadable. */
typedef int (*sh_audio_originals_bank_visit)(void *context, uint32_t id, uint32_t language);
int sh_audio_originals_packaged_banks(void *context,
    sh_audio_originals_bank_visit visit, void *visitor, char *error, size_t capacity);

typedef struct sh_audio_original_bank {
    const char *path, *filename;
    sh_audio_bank metadata;
} sh_audio_original_bank;
typedef int (*sh_audio_originals_inventory_visit)(void *context,
    const sh_audio_original_bank *banks, size_t count, char *error, size_t capacity);
/* One captured effective bank per native ID, current language before root,
 * packed before loose. Rows and metadata remain immutable until close. Only
 * cooked metadata is read, never copied media payloads. Reuses this index's
 * retained file handles and package lookup instead of another disk inventory. */
int sh_audio_originals_banks(sh_audio_originals *originals,
    const sh_audio_original_bank **banks, size_t *count, char *error, size_t capacity);
/* Owned paths in native precedence order for this captured language/prefix. */
int sh_audio_originals_paths(sh_audio_originals *originals, const char *filename,
    char **localized, char **root);
/* Sorted unique numeric identities reserved by the complete effective original
 * inventory, including unloaded banks, optional targets and unreferenced media.
 * Domains are deliberately unioned: private names must not acquire an original
 * meaning in any native table. No native loading or media-payload hashing.
 * Borrowed until close; failure never exposes a partial set. */
int sh_audio_originals_ids(sh_audio_originals *originals, const uint32_t **ids,
    size_t *count, char *error, size_t capacity);
#endif
