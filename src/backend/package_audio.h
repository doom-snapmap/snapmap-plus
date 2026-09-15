/* Audio event membership in a prospective package resource provider. */
#ifndef SH_PACKAGE_AUDIO_H
#define SH_PACKAGE_AUDIO_H
#include "package_compiler.h"
#include "audio_banks.h"

typedef struct sh_package_audio sh_package_audio;
typedef struct sh_package_audio_bank {
    const char *path, *filename;
    const sh_audio_bank *metadata;
    sh_package_file_identity identity;
    int contributed; /* real source change or additional campaign activation */
} sh_package_audio_bank;
typedef struct sh_package_audio_media {
    const char *path, *filename;
    uint32_t id;                 /* native decimal media identity */
    sh_package_file_identity identity;
    int contributed;
} sh_package_audio_media;
typedef struct sh_package_audio_reference {
    uint32_t event, bank, language;
    const char *path, *filename;
    const sh_audio_bank *metadata;   /* borrowed, for this bank's own dependencies */
} sh_package_audio_reference;
typedef struct sh_package_audio_report {
    size_t banks, media, invalid;
    /* Media the native resolver can only reach by name. They are still served,
     * but carry no identity a bank can name, so they join no media dependency. */
    size_t unnamed_media;
    char first_gap[1024];
} sh_package_audio_report;

/* Read each effective bank once. Source bytes are verified, media payloads are
 * skipped, and all locale paths remain distinct. The returned index owns its
 * strings and event rows; it does not retain a provider or change native state.
 * Invalid bank syntax/identity is recorded as a dependency gap, not selected.
 * Unreadable/stale source bytes or allocation failure refuse the snapshot. */
sh_package_audio *sh_package_audio_open(const sh_package_compilation *compiled,
    sh_package_audio_report *report, char *error, size_t capacity);
void sh_package_audio_close(sh_package_audio *audio);

/* Effective bank inventory, including banks with no directly declared events.
 * Borrowed for the index lifetime. Identity covers the entire bank, not only
 * event membership, so changed media/object data cannot reuse a resident bank. */
size_t sh_package_audio_banks(const sh_package_audio *audio,
    const sh_package_audio_bank **banks);

/* Effective media inventory, sorted by identity and borrowed for the index
 * lifetime. A media file is served by path; this inventory exists so that a
 * changed payload can be matched against the banks that name its identity. */
size_t sh_package_audio_media_files(const sh_package_audio *audio,
    const sh_package_audio_media **media);

/* Every matching candidate path, in canonical path order. The span is borrowed
 * for the index lifetime. Absence here does not prove an event unavailable:
 * installed banks and packed media require their separate provider indexes. */
size_t sh_package_audio_find(const sh_package_audio *audio, uint32_t event,
    const sh_package_audio_reference **references);

/* Every effective media file carrying one identity, in canonical path order.
 * Borrowed for the index lifetime. Absence means this provider does not supply
 * the payload; the installed original or a mounted package still can. */
size_t sh_package_audio_find_media(const sh_package_audio *audio, uint32_t id,
    const sh_package_audio_media **media);
#endif
