/* Typed cooked-bank structure for scoped audio linking. */
#ifndef SH_AUDIO_BANK_CODEC_H
#define SH_AUDIO_BANK_CODEC_H
#include "audio_banks.h"
#include "audio_hirc.h"

typedef struct sh_audio_bank_object {
    unsigned kind;
    uint32_t id;
    uint64_t offset;
    size_t length, first_span, span_count;
    size_t first_bus_record, bus_record_count;
} sh_audio_bank_object;
typedef struct sh_audio_bank_name {
    uint64_t offset; /* ID followed by length byte and the extensionless name */
    uint32_t id;
    unsigned char length;
    char name[256];
} sh_audio_bank_name;
typedef struct sh_audio_bank_chunk {
    char tag[5];
    uint64_t offset; /* includes the eight-byte chunk header */
    uint32_t length;
    size_t first_shared_record, shared_record_count;
} sh_audio_bank_chunk;
typedef struct sh_audio_bank_codec {
    sh_audio_bank metadata;
    sh_audio_identity_span *spans; /* file-relative typed identity fields */
    size_t span_count;
    sh_audio_identity_list *ordered;
    size_t ordered_count;
    sh_audio_bank_object *objects;
    size_t object_count;
    sh_audio_bank_name *names;
    size_t name_count;
    sh_audio_bank_chunk *chunks;
    size_t chunk_count;
    unsigned shared_chunks; /* STMG/ENVS/PLAT require the global policy adapter */
    sh_audio_bus_record *bus_records; /* file-relative values, in object order */
    size_t bus_record_count;
    sh_audio_shared_record *shared_records; /* file-relative, in chunk order */
    size_t shared_record_count;
} sh_audio_bank_codec;

/* Uses the common bank validator, then inspects typed fields without reading
 * DATA payloads. Source must remain immutable through inspection and binding.
 * Returns 1 complete, 0 malformed, -1 IO/allocation, -2 unverified grammar.
 * Shared settings chunks remain explicit policy records, not private aliases.
 * No native calls, output writes or source changes occur. */
int sh_audio_bank_codec_read(sh_audio_bank_source source, sh_audio_bank_codec *out,
    char *error, size_t capacity);
void sh_audio_bank_codec_free(sh_audio_bank_codec *bank);

typedef int (*sh_audio_bank_identity_resolver)(void *context,
    const sh_audio_identity_span *field, uint32_t *replacement,
    char *error, size_t capacity);
typedef struct sh_audio_bank_edit {
    uint64_t offset, remove;
    unsigned char length;
    unsigned char bytes[16]; /* integer, or decimal bank name with length byte */
} sh_audio_bank_edit;
typedef struct sh_audio_bank_binding {
    sh_audio_bank_edit *edits;
    size_t count;
    uint64_t input_length, output_length;
} sh_audio_bank_binding;

/* Resolve typed private identities and retain fixed shared controls. Changed
 * STID integer identities also receive their native decimal filename and the
 * containing chunk size is adjusted. This only prepares edits; no sink/native
 * mutation occurs. Shared policy chunks and bus definitions remain present;
 * choosing their one effective global policy is an upstream compiler task. */
int sh_audio_bank_codec_bind(sh_audio_bank_source source, const sh_audio_bank_codec *bank,
    sh_audio_bank_identity_resolver resolve, void *context, sh_audio_bank_binding *out,
    char *error, size_t capacity);
void sh_audio_bank_binding_free(sh_audio_bank_binding *binding);
/* Project prepared private output without replaying global controls or bus
 * definitions. Keeps all other bound edits and opaque payload bytes; updates
 * each HIRC count/size. The caller must separately retain and publish the shared
 * policy and its bound bus recipes before activating this output. Both inputs
 * remain unchanged, and failure leaves out empty. No DATA payload is read. */
int sh_audio_bank_binding_private(sh_audio_bank_source source,const sh_audio_bank_codec *bank,
    const sh_audio_bank_binding *binding,sh_audio_bank_binding *out,char *error,size_t capacity);
/* Capture one cooked object's bound body, including its numeric definition ID.
 * Used for shared policy recipes whose dependencies have private bindings.
 * The caller owns the output; no whole-bank or media payload copy is made. */
int sh_audio_bank_binding_object(sh_audio_bank_source source,const sh_audio_bank_binding *binding,
    const sh_audio_bank_object *object,unsigned char **body,size_t *length,char *error,size_t capacity);
/* Streams the immutable source through a previously prepared binding. The sink
 * owns its output transaction and must discard all written bytes on failure.
 * Only the caller's private output is changed, never the authored package. */
int sh_audio_bank_binding_write(sh_audio_bank_source source, const sh_audio_bank_binding *binding,
    int (*write)(void *context, const void *bytes, size_t length), void *context,
    char *error, size_t capacity);
#endif
