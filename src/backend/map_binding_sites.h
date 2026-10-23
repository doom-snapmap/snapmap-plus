/* Portable locations for verified resource spans in native map JSON. */
#ifndef SH_MAP_BINDING_SITES_H
#define SH_MAP_BINDING_SITES_H
#include "package_provenance.h"

typedef struct sh_map_binding_span {
    const char *json;
    size_t length;
    sh_package_provenance_site site; /* owned owner/reference after locate */
} sh_map_binding_span;

/* Only caller-supplied spans receive locations; this does not identify assets
 * or interpret ordinary map strings. Every span must be a complete JSON value
 * borrowed from this document. Repeated spans are permitted. One document walk
 * locates all spans; failure clears all locations. Native byte strings stay
 * unchanged. Initialize site fields to zero; repeat calls replace them.
 * Top-level entities array indices are document-local owners (entity/N), never
 * native uniqueId values. Every other path belongs to world. The caller remaps
 * owners at native clone/serialization boundaries. Member keys use escaped
 * byte components and array indices use a distinct component kind. */
int sh_map_binding_sites_locate(const char *json, size_t length,
    sh_map_binding_span *spans, size_t count, char *error, size_t capacity);
void sh_map_binding_sites_clear(sh_map_binding_span *spans, size_t count);
/* The location sh_map_binding_sites_locate records for the value at path (a
 * visitor's path in the same document): 1 with malloc-owned owner and
 * reference, 0 on failure. */
struct sh_json_path;
int sh_map_binding_site_at(const struct sh_json_path *path, sh_package_provenance_site *site);
#endif
