/* Wwise PCK metadata and exact logical resource lookup. */
#ifndef SH_AUDIO_PACKAGES_H
#define SH_AUDIO_PACKAGES_H
#include "audio_banks.h"
#include <wchar.h>

typedef enum sh_audio_package_kind {
    SH_AUDIO_PACKAGE_BANK,
    SH_AUDIO_PACKAGE_MEDIA,
    SH_AUDIO_PACKAGE_EXTERNAL
} sh_audio_package_kind;

typedef struct sh_audio_package_entry {
    uint64_t id, offset;
    uint32_t length, language, block;
} sh_audio_package_entry;
typedef struct sh_audio_package_language {
    uint32_t id;
    wchar_t *name;
} sh_audio_package_language;
typedef struct sh_audio_package {
    sh_audio_package_language *languages;
    size_t language_count;
    sh_audio_package_entry *entries[3];
    size_t counts[3];
} sh_audio_package;

/* Parse metadata only, with bounded reads and no media-sized allocation.
 * Source is borrowed for this call; output owns every indexed row/name.
 * 1 success, 0 malformed/unsupported native representation, -1 IO/allocation.
 * Start output empty. Failure leaves it empty; free before reusing it. */
int sh_audio_package_read(sh_audio_bank_source source, sh_audio_package *out,
    char *error, size_t capacity);
void sh_audio_package_free(sh_audio_package *package);

/* Match the native ASCII-folded UTF-16 language name. These numeric IDs belong
 * to this PCK, not BKHD or global Wwise language IDs. Returns 1 if found, else 0.
 * Nonlocalized requests use numeric ID 0 directly, without name lookup. */
int sh_audio_package_language_id(const sh_audio_package *package,
    const wchar_t *name, uint32_t *id);
/* Exact ID/language search; never falls back to another language or category.
 * Borrowed row remains valid until package_free. Bank/media IDs are uint32;
 * external-source records use full uint64 identities. */
const sh_audio_package_entry *sh_audio_package_find(const sh_audio_package *package,
    sh_audio_package_kind kind, uint64_t id, uint32_t language);
#endif
