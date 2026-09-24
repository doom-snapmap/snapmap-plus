/* Typed identity spans in cooked audio hierarchy objects. */
#ifndef SH_AUDIO_HIRC_H
#define SH_AUDIO_HIRC_H
#include <stddef.h>
#include <stdint.h>

typedef enum sh_audio_identity_domain {
    SH_AUDIO_ID_NODE, SH_AUDIO_ID_EVENT, SH_AUDIO_ID_ACTION, SH_AUDIO_ID_STATE,
    SH_AUDIO_ID_ATTENUATION, SH_AUDIO_ID_EFFECT_SHARED, SH_AUDIO_ID_EFFECT_CUSTOM,
    SH_AUDIO_ID_MODULATOR, SH_AUDIO_ID_LAYER, SH_AUDIO_ID_MEDIA, SH_AUDIO_ID_BANK,
    SH_AUDIO_ID_BUS, SH_AUDIO_ID_STATE_GROUP, SH_AUDIO_ID_STATE_VALUE,
    SH_AUDIO_ID_SWITCH_GROUP, SH_AUDIO_ID_SWITCH_VALUE, SH_AUDIO_ID_RTPC,
    SH_AUDIO_ID_TRIGGER
} sh_audio_identity_domain;

enum { SH_AUDIO_ID_DEFINITION = 1, SH_AUDIO_ID_SHARED = 2, SH_AUDIO_ID_LITERAL = 4,
    SH_AUDIO_ID_OPTIONAL = 8 };
typedef struct sh_audio_identity_span {
    size_t offset; /* four-byte little-endian field within this object body */
    uint32_t value;
    sh_audio_identity_domain domain;
    unsigned flags;
} sh_audio_identity_span;
typedef struct sh_audio_identity_list {
    size_t offset;
    uint32_t count;
    unsigned fixed_prefix; /* first transition wildcard retains its position */
} sh_audio_identity_list;

/* Semantic records emitted by the same bounded shared-chunk reader. Offsets
 * refer to the caller's chunk body. Curves and RTPC controls remain atomic;
 * group defaults and individual transition pairs compose independently. */
typedef enum sh_audio_shared_kind {
    SH_AUDIO_SHARED_THRESHOLD, SH_AUDIO_SHARED_VOICES,
    SH_AUDIO_SHARED_GROUP, SH_AUDIO_SHARED_TRANSITION,
    SH_AUDIO_SHARED_SWITCH, SH_AUDIO_SHARED_RTPC,
    SH_AUDIO_SHARED_BUILTIN, SH_AUDIO_SHARED_ENVIRONMENT, SH_AUDIO_SHARED_PLATFORM
} sh_audio_shared_kind;
typedef struct sh_audio_shared_record {
    sh_audio_shared_kind kind;
    uint32_t key[3];
    size_t offset, length;
} sh_audio_shared_record;

/* Bound bus publication must update native fields through their own ownership
 * operations rather than rerunning the constructor parser on a live object.
 * These complete sections come from the same grammar as identity relocation;
 * their offsets refer to the original object body and include list counts. */
typedef enum sh_audio_bus_part {
    SH_AUDIO_BUS_PARENT, SH_AUDIO_BUS_PROPERTIES, SH_AUDIO_BUS_SETTINGS,
    SH_AUDIO_BUS_DUCKING, SH_AUDIO_BUS_EFFECTS, SH_AUDIO_BUS_FLAGS,
    SH_AUDIO_BUS_RTPCS, SH_AUDIO_BUS_STATES, SH_AUDIO_BUS_PARTS
} sh_audio_bus_part;
typedef struct sh_audio_bus_section { size_t offset, length; } sh_audio_bus_section;
typedef struct sh_audio_bus_layout {
    int present;
    sh_audio_bus_section parts[SH_AUDIO_BUS_PARTS];
} sh_audio_bus_layout;

/* Native assignment keys within a shared bus. Collection entries compose by
 * these keys, not their position in a cooked list. Curves remain atomic and
 * their accumulation mode belongs to the parameter subscription as a whole.
 * Offsets identify only the value/recipe; keys retain noncontiguous property
 * IDs and containing state groups. Records retain authored order, including
 * repeated keys; selection applies each native operation's ordering rules. */
typedef enum sh_audio_bus_field {
    SH_AUDIO_BUS_FIELD_PARENT, SH_AUDIO_BUS_FIELD_PROPERTY,
    SH_AUDIO_BUS_FIELD_SETTING, SH_AUDIO_BUS_FIELD_DUCK_SETTING,
    SH_AUDIO_BUS_FIELD_DUCK, SH_AUDIO_BUS_FIELD_BYPASS,
    SH_AUDIO_BUS_FIELD_EFFECT, SH_AUDIO_BUS_FIELD_MIXER,
    SH_AUDIO_BUS_FIELD_FLAGS, SH_AUDIO_BUS_FIELD_RTPC_MODE,
    SH_AUDIO_BUS_FIELD_RTPC_CURVE, SH_AUDIO_BUS_FIELD_STATE_GROUP,
    SH_AUDIO_BUS_FIELD_STATE
} sh_audio_bus_field;
typedef struct sh_audio_bus_record {
    sh_audio_bus_field field;
    uint32_t key[2];
    size_t offset, length;
} sh_audio_bus_record;
/* For packed SETTING/BYPASS/FLAGS records key[1] is a mask over the original
 * little-endian span. Zero means the full value. Unknown remainders stay atomic;
 * counts/enums are never split into Boolean bits. Authored bytes stay intact. */
uint32_t sh_audio_bus_record_mask(const sh_audio_bus_record *record);
typedef struct sh_audio_hirc {
    sh_audio_identity_span *spans;
    size_t count;
    sh_audio_identity_list *ordered;
    size_t ordered_count;
    sh_audio_shared_record *shared;
    size_t shared_count;
    sh_audio_bus_layout bus;
    sh_audio_bus_record *bus_records;
    size_t bus_record_count;
} sh_audio_hirc;

/* Reads one complete v113 HIRC object body including its defining identity.
 * Scalar numbers and opaque plugin parameters are never inferred identities.
 * 1 means complete typed grammar; 0 malformed; -1 allocation; -2 an unverified
 * grammar variant. A refusal leaves output empty and never changes input. */
int sh_audio_hirc_read(unsigned kind, const void *body, size_t length,
    sh_audio_hirc *out, char *error, size_t capacity);
/* Verified STMG/ENVS/PLAT chunk bodies. These control shared native state;
 * emitted identities are never permission to replay or privately rename it. */
int sh_audio_shared_chunk_read(const char *tag, const void *body, size_t length,
    sh_audio_hirc *out, char *error, size_t capacity);
void sh_audio_hirc_free(sh_audio_hirc *object);
#endif
