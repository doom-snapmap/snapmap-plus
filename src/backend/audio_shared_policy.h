/* One effective policy for audio controls the native engine shares globally. */
#ifndef SH_AUDIO_SHARED_POLICY_H
#define SH_AUDIO_SHARED_POLICY_H
#include "audio_hirc.h"

typedef struct sh_audio_shared_input {
    const char *tag;
    const void *body;
    size_t length, scope;
    int baseline; /* Captured installed original, never package metadata. */
} sh_audio_shared_input;
/* Optional exact identity of each parsed record, supplied by the audio graph.
 * equal returns 1 for the same bound behavior (bytes and private dependency
 * classes), 0 for different, -1 when retained sources cannot be compared.
 * identity supplies a stable tie-break for equal bytes naming different
 * definitions. Records are numbered in chunk order within their input. */
typedef struct sh_audio_shared_exact {
    void *context;
    int (*equal)(void *context, size_t left_input, size_t left_record,
        size_t right_input, size_t right_record);
    const unsigned char *(*identity)(void *context, size_t input, size_t record);
} sh_audio_shared_exact;
typedef struct sh_audio_shared_contributor { size_t input, record; } sh_audio_shared_contributor;
typedef struct sh_audio_shared_setting {
    sh_audio_shared_record record; /* Offset is zero in the owned body. */
    unsigned char *body;
    size_t input, alternatives;
    size_t index; /* Winning record ordinal within its input chunk. */
    int edited;   /* Winner differs from the native baseline (or has none). */
    /* Effective inputs whose value equals the winner, after same-scope native
     * ordering. Baseline witnesses appear too; they carry no package owner. */
    sh_audio_shared_contributor *contributors;
    size_t contributor_count;
} sh_audio_shared_setting;
typedef struct sh_audio_shared_policy {
    sh_audio_shared_setting *settings;
    size_t count;
} sh_audio_shared_policy;
/* Inputs from each scope are in native registration order. Repeated keys in
 * that scope retain its last effective record. Native no-op fields preserve
 * earlier settings. Across scopes unchanged original copies
 * do not override edits; distinct keys append, equal records dedupe. Conflicting
 * atomic records select the lexicographically smallest complete edited bytes,
 * independent of package IDs/folder order. Voice limits take the maximum and
 * the audibility threshold takes the minimum supported value, including the
 * baseline. Curves/control recipes are never spliced point by point; built-in
 * parameter bindings compose separately from RTPC defaults, as in the engine. Platform
 * labels must agree with the native target. Output is owned and all-or-nothing. */
int sh_audio_shared_policy_compile(const sh_audio_shared_input *inputs,size_t count,
    sh_audio_shared_policy *out,char *error,size_t capacity);
/* Same rules, with equality/baseline comparison taken from exact bound
 * identities instead of cooked bytes alone. Equal bytes whose private
 * dependencies differ are competing values; the tie-break uses identity. */
int sh_audio_shared_policy_compile_exact(const sh_audio_shared_input *inputs,size_t count,
    const sh_audio_shared_exact *exact,sh_audio_shared_policy *out,char *error,size_t capacity);
void sh_audio_shared_policy_free(sh_audio_shared_policy *policy);
/* Deep copy, preserving winner/edited/contributor metadata. */
int sh_audio_shared_policy_copy(const sh_audio_shared_policy *policy,sh_audio_shared_policy *out);
/* Reconstruct a canonical checked chunk body. Missing chunks return an empty
 * output. The caller owns the body. No native state or authored file is touched. */
int sh_audio_shared_policy_chunk(const sh_audio_shared_policy *policy,const char *tag,
    unsigned char **body,size_t *length,char *error,size_t capacity);
#endif
