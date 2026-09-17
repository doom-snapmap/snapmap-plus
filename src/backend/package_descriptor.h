/* Authored package metadata. Resource inventories are compiler-owned data. */
#ifndef SH_PACKAGE_DESCRIPTOR_H
#define SH_PACKAGE_DESCRIPTOR_H

#include <stddef.h>
#include "config_json.h"

#define SH_PACKAGE_ID_CAP 128
#define SH_PACKAGE_DISPLAY_CAP 256

typedef struct sh_package_descriptor {
    char id[SH_PACKAGE_ID_CAP];
    char name[SH_PACKAGE_DISPLAY_CAP];
    /* Owns validated JSON fragments, including descriptive extension fields.
     * The source file itself is never rewritten or serialized from this object. */
    sh_json_object fields;
} sh_package_descriptor;

/* Parse a complete descriptor into an initially empty output. Packages require
 * an id and display name. Failures
 * leave out empty and provide a bounded diagnostic. This does not apply policy
 * requirements or decide which maps use the package. */
int sh_package_descriptor_parse(const char *json, size_t length,
                                 sh_package_descriptor *out,
                                 char *error, size_t error_capacity);
void sh_package_descriptor_free(sh_package_descriptor *descriptor);

/* Returned fragments live until descriptor_free. Missing sections return NULL. */
const char *sh_package_descriptor_section(const sh_package_descriptor *descriptor,
                                          const char *section);
int sh_package_id_valid(const char *id);
/* Canonical identity: ASCII case folding and surrounding JSON whitespace only.
 * Internal spaces, invalid punctuation and non-ASCII letters remain errors.
 * out may alias id; failed normalization leaves out untouched. */
int sh_package_id_normalize(const char *id, char out[SH_PACKAGE_ID_CAP]);

#endif
