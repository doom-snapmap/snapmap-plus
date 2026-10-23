/* Composition of checked bus recipes before native numeric ID binding. */
#ifndef SH_AUDIO_BUS_POLICY_H
#define SH_AUDIO_BUS_POLICY_H
#include "audio_link_graph.h"

typedef struct sh_audio_bus_value_input {
    sh_audio_bus_record field;
    sh_audio_graph_root root;
    const unsigned char *body; /* Checked graph value, including its packed-field mask. */
    size_t origin; /* Caller-owned value index, independent of recipe order. */
} sh_audio_bus_value_input;
typedef struct sh_audio_bus_recipe_input {
    uint32_t bus;
    unsigned kind;
    int baseline;
    sh_audio_graph_root root;
    const sh_audio_bus_value_input *values;
    size_t count;
} sh_audio_bus_recipe_input;
typedef struct sh_audio_bus_contributor { size_t recipe, value; } sh_audio_bus_contributor;
typedef struct sh_audio_bus_setting {
    uint32_t bus;
    sh_audio_bus_record field; /* Owned unbound value, offset zero. */
    unsigned char *body;
    int removed;
    int edited; /* Differs from the native baseline recipe, or has none. */
    size_t recipe,value; /* value=SIZE_MAX for inferred removal. */
    sh_audio_bus_contributor *contributors;
    size_t contributor_count;
} sh_audio_bus_setting;
typedef struct sh_audio_bus_type { uint32_t bus; unsigned kind; } sh_audio_bus_type;
typedef struct sh_audio_bus_policy {
    sh_audio_bus_setting *settings;
    size_t count;
    sh_audio_bus_type *buses;
    size_t bus_count;
} sh_audio_bus_policy;
/* Inputs come from the checked codec/graph, in native recipe order. Baseline
 * duplicates use the first native definition. Complete authored recipes are
 * compared independently with that baseline: unchanged copies do not erase
 * edits, independent keys append, missing baseline collection keys remove.
 * Packed flags are separate masked keys; baseline-aware clears remain clears,
 * rather than OR-ing flags. Multi-bit counts and unknown remainders stay atomic.
 * Exact graph equality, not a digest alone, certifies shared dependencies.
 * Different edits to one global setting choose a stable binding; an explicit
 * changed value wins over a competing removal. The parent choices form a
 * rooted forest using available alternatives, so valid overlapping edits do
 * not create a synthetic cycle. An external parent requires native preflight.
 * Output owns its bytes/indices. It is unbound and does not publish native state. */
int sh_audio_bus_policy_compile(const sh_audio_link_graph *graph,
    const sh_audio_bus_recipe_input *inputs,size_t count,sh_audio_bus_policy *out,
    char *error,size_t capacity);
void sh_audio_bus_policy_free(sh_audio_bus_policy *policy);
#endif
