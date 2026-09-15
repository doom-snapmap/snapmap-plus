/* Cooked audio event dependencies and native bank ownership. */
#ifndef SH_AUDIO_BANKS_H
#define SH_AUDIO_BANKS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "package_sources.h"

/* Native logical identities, independent of package folders and cache paths.
 * Bank takes a filename, including its extension. Sound takes the engine's
 * declaration name. Return 0 for names the verified native string conversion
 * cannot represent; the output is cleared. These do not prove bank presence. */
int sh_audio_bank_identity(const char *filename, uint32_t *id);
int sh_audio_event_identity(const char *declaration, uint32_t *id);
/* Whether a bank file name denotes this cooked identity. The native loader
 * accepts both forms: the folded name hash above, and a decimal file stem that
 * is the identity itself, which is how it reopens a bank it knows only by id.
 * Media files carry only the decimal form. */
int sh_audio_bank_names_identity(const char *filename, uint32_t id);
int sh_audio_media_identity(const char *filename, uint32_t *id);

typedef struct sh_audio_bank_media {
    uint32_t id;
    /* 0 resident in the bank, 1 streamed from its own file, 2 streamed with a
     * prefetched head. Prefetch and resident bytes live in this bank's data. */
    unsigned char stream;
    unsigned char embedded;       /* this bank carries bytes for the identity */
} sh_audio_bank_media;

typedef struct sh_audio_bank {
    uint32_t id, language;
    uint32_t *events;             /* sorted, unique event identities */
    size_t event_count;
    /* Other banks this one names explicitly, as recorded by its play actions.
     * Loading an event here without them leaves its target structure absent. */
    uint32_t *banks;
    size_t bank_count;
    /* References this bank cannot satisfy itself, split by what the native
     * action kind requires. A play action's target is a hard dependency, and
     * an event's own actions must be present, so anything missing from those
     * is a real gap. Every other action kind is a control action whose target
     * may legally be absent: stopping or muting an object that no loaded bank
     * defines is a no-op, not a missing resource, and loading unrelated banks
     * to satisfy one would embed content the map never plays. */
    uint32_t *required;
    size_t required_count;
    uint32_t *optional;
    size_t optional_count;
    sh_audio_bank_media *media;   /* sorted, unique by identity */
    size_t media_count;
} sh_audio_bank;

/* Bounded random access to verified bytes, in memory or on disk. Read must
 * fill the requested span and returns 1 on success. No media is copied by
 * the bank indexer; only object headers, event identities, media source
 * records and action targets are inspected.
 * Readers return 1 on success, 0 for malformed/unsupported bank bytes, and -1
 * for failed IO/allocation. Output is empty after either kind of failure. */
typedef struct sh_audio_bank_source {
    void *context;
    uint64_t length;
    int (*read)(void *context, uint64_t offset, void *out, size_t length);
} sh_audio_bank_source;
int sh_audio_bank_read_source(sh_audio_bank_source source, sh_audio_bank *out,
    char *error, size_t capacity);

/* Read the shipped cooked format without copying embedded media. The stream
 * remains caller-owned. A malformed/unsupported bank leaves out empty. */
int sh_audio_bank_read(FILE *stream, sh_audio_bank *out, char *error, size_t capacity);
void sh_audio_bank_free(sh_audio_bank *bank);
int sh_audio_bank_has_event(const sh_audio_bank *bank, uint32_t event);
int sh_audio_bank_needs_bank(const sh_audio_bank *bank, uint32_t id);
/* Borrowed row for the bank lifetime, or NULL when this bank never names it. */
const sh_audio_bank_media *sh_audio_bank_find_media(const sh_audio_bank *bank, uint32_t id);

typedef struct sh_audio_bank_slot {
    const char *name;
    uint32_t id, loaded_id;
    /* Only successful new loads are owned. Already-loaded banks belong to
     * their existing consumer. uncertain records an interrupted native call. */
    int owned, uncertain;
    /* The whole bank selected by the last successful override load. Native
     * result 69 alone cannot establish these bytes. restore_native survives
     * partial failures until a displaced pre-existing bank is loaded again. */
    sh_package_file_identity content;
    int content_known, restore_native;
} sh_audio_bank_slot;
typedef struct sh_audio_bank_ops {
    void *context;
    /* Native result: 1 success, 69 already loaded, -1 unknown side effects. */
    int (*load)(void *context, const char *name, uint32_t *id);
    int (*unload)(void *context, uint32_t id);
} sh_audio_bank_ops;

/* Main-thread consumer of a published provider. Load dependencies before
 * releasing obsolete owned banks. Records every completed native mutation;
 * on failure the provider transaction must republish its previous provider
 * and reconcile its wanted set. No allocation occurs during native calls.
 * content is optional; non-NULL entries identify required override bytes and
 * require wanted[i]. Already-loaded unknown/different bytes are unloaded and
 * reloaded. Displaced native banks are restored even if no event wants them.
 * Repeated activation rechecks native availability; external consumers may
 * have unloaded a bank since the last provider transition. */
int sh_audio_banks_reconcile(sh_audio_bank_slot *slots, size_t count,
    const unsigned char *wanted, const sh_package_file_identity *const *content,
    sh_audio_bank_ops ops, char *error, size_t capacity);
#endif
