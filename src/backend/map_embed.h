/* Pack installed package folders and detect packages referenced by map JSON.
 * map_package owns the wire format and install/load path; this module handles
 * author-side filesystem reads.
 */
#ifndef SNAPMAP_PLUS_MAP_EMBED_H
#define SNAPMAP_PLUS_MAP_EMBED_H

#include <windows.h>
#include <stddef.h>

#include "map_package.h"
#include "package_usage.h"

/* Main-thread preparation, before any compiler snapshot is acquired. Caller
 * supplies an empty result and frees it after use. Missing reader coverage is
 * recorded separately from failure; no entity is constructed or modified. */
int sh_mpkg_prepare_map(const char *json, size_t length, sh_package_references *references,
                        char *error, size_t error_capacity);

/* Pack every authored file and directory into deterministic ZIP delivery.
 * Compression affects transport only. Source bytes are verified before use.
 * Returns a process-heap buffer, or NULL with an error. */
unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len,
                                char *err, size_t err_cap);

/* Resolve typed saved-map roots and known dependencies through compiled
 * ownership, including replacements. SIZE_MAX indicates a failed inventory
 * or traversal; missing observations are separately reported in diagnostics.
 * Initialize *out to NULL. Repeated calls replace it; free(*out) after use. */
typedef struct sh_mpkg_used {
    char id[SH_MPKG_ID_CAP];   /* authored stable package id */
    char root[MAX_PATH];       /* absolute path to the package folder, ready to pack */
} sh_mpkg_used;

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used **out, char *error, size_t error_capacity);

#endif /* SNAPMAP_PLUS_MAP_EMBED_H */
