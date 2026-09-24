/* Portable prefab ownership uses the same private carriers as map delivery. */
#ifndef SH_PREFAB_H
#define SH_PREFAB_H
#include "package_occurrence.h"

typedef struct sh_prefab_carrier {
    char *native_json; /* process-heap copy, with private delivery removed */
    size_t native_length;
    sh_package_provenance bindings;
    int bindings_present, deliveries_present;
} sh_prefab_carrier;

/* Structural read only. Native byte strings and unrelated variables survive.
 * Absent ownership is ordinary vanilla/legacy input; malformed ownership is
 * failure. Keep the original carrier bytes for context_open/prepare/admission:
 * this operation neither validates archive contents nor authorizes native parse.
 * Initialize out to zero; failure leaves a previous successful output intact. */
int sh_prefab_carrier_read(const char *json, size_t length, sh_prefab_carrier *out,
    char *error, size_t capacity);
void sh_prefab_carrier_free(sh_prefab_carrier *value);

/* The caller first completes the typed occurrence walk against one provider.
 * This pure transport step uses only that document's exact retained originals,
 * never editable directories, descriptor IDs or a binding supplier union.
 * It replaces stale delivery/ownership, preserving the native document outside
 * carrier variables and their count. Empty ownership emits no metadata.
 * Returns process-heap bytes, or NULL without changing any input. */
char *sh_prefab_carrier_write(const char *json, size_t length,
    const sh_package_occurrence_snapshot *document, size_t *output_length,
    char *error, size_t capacity);
#endif
