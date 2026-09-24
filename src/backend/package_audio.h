/* Audio event membership in a prospective package resource provider. */
#ifndef SH_PACKAGE_AUDIO_H
#define SH_PACKAGE_AUDIO_H
#include "package_compiler.h"
#include "audio_banks.h"
#include "audio_hirc.h"
#include "audio_originals.h"

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

typedef struct sh_package_audio_scope sh_package_audio_scope;
/* Native PC bank/media paths, including captured bank-prefix/locale directories.
 * 1 bank, 2 media, 0 another family. This is classification, not validation. */
int sh_package_audio_path_kind(const char *path);
typedef int (*sh_package_audio_path_visit)(void *context, const char *path, int original);
/* Immutable effective source inventory for one authored scope and captured
 * native locale. Candidate files shadow originals only at the same native
 * lookup path; a peer package cannot repair an absent authored dependency.
 * originals is borrowed and must outlive this scope. No native calls occur. */
sh_package_audio_scope *sh_package_audio_scope_open(const sh_package_compilation *compiled,
    sh_audio_originals *originals, char *error, size_t capacity);
void sh_package_audio_scope_close(sh_package_audio_scope *scope);
/* Complete explicit event/action bank and streamed media dependency closure.
 * Visits only actual selected paths. An absent played event or mandatory bank,
 * object or media fails; control-action targets are allowed native no-ops. */
int sh_package_audio_scope_event(sh_package_audio_scope *scope, uint32_t event,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity);
/* Capture the union once in native inventory order, independent of requested
 * event order. The resulting construction order is also used by audio linking. */
int sh_package_audio_scope_events(sh_package_audio_scope *scope, const uint32_t *events,
    size_t count, sh_package_audio_path_visit visit, void *context, char *error, size_t capacity);
/* Seed effective authored banks and banks naming effective authored media in
 * addition to requested events. Unselected locales remain delivery inventory.
 * Refine typed metadata first so plug-in media participates in nomination. */
int sh_package_audio_scope_inventory(sh_package_audio_scope *scope, const uint32_t *events,
    size_t count, sh_package_audio_path_visit visit, void *context, char *error, size_t capacity);
/* Explicit bank roots use the same locale selection, typed closure and ordered
 * inventory as events. Every requested bank must exist in this source scope.
 * Used to capture baseline bus recipes without inventing an event reference. */
int sh_package_audio_scope_roots(sh_package_audio_scope *scope, const uint32_t *banks,
    size_t count, sh_package_audio_path_visit visit, void *context, char *error, size_t capacity);
/* Effective native registration inventory for typed refinement. Does not select
 * or activate these banks; the visitor may refine each row in place. */
int sh_package_audio_scope_banks(sh_package_audio_scope *scope,
    sh_package_audio_path_visit visit, void *context);
/* Current-locale media resolution shared by dependency walking and typed audio
 * capture. 1 visited, 0 absent, -1 IO/visitor failure. Never search peer scopes. */
int sh_package_audio_scope_media(sh_package_audio_scope *scope, uint32_t id,
    sh_package_audio_path_visit visit, void *context, char *error, size_t capacity);
/* Refine this private working index after fully decoding a selected source.
 * Identity/language must match its captured row. Metadata remains borrowed;
 * the same immutable source and decoded metadata must outlive this scope. */
int sh_package_audio_scope_refine(sh_package_audio_scope *scope, const char *path,
    const sh_audio_bank *metadata);
/* Borrow complete typed references for reverse inventory nomination. The
 * captured codec owns these through scope close. Shared controls stay global. */
int sh_package_audio_scope_references(sh_package_audio_scope *scope, const char *path,
    const sh_audio_identity_span *spans, size_t count);
#endif
