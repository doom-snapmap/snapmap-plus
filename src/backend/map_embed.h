/* Pack installed package folders and detect packages referenced by map JSON.
 * map_package owns the wire format and install/load path; this module handles
 * author-side filesystem reads.
 */
#ifndef SNAPMAP_PLUS_MAP_EMBED_H
#define SNAPMAP_PLUS_MAP_EMBED_H

#include <windows.h>
#include <stddef.h>

#include "map_package.h"

/* Pack all regular files under a package root into a deterministic stored
 * ZIP, excluding installer metadata. Resource manifests reference game assets
 * in the player's install. The packer does not infer content provenance from
 * extensions. Returns an owned process-heap buffer and out_len, or NULL with
 * err.
 */
unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len,
                                char *err, size_t err_cap);

/* Detect references to published package decls by quoted logical name or
 * basename. This is a conservative heuristic, not a dependency-closure proof.
 * Matching packages are included even when the match may be incidental.
 *
 * Package names must fit lowercase [a-z0-9_-] shard ids; nested or uppercase
 * names are skipped with a log. Enumerate below data_root/overrides, write at
 * most cap entries, and return their count.
 */
typedef struct sh_mpkg_used {
    char id[SH_MPKG_ID_CAP];   /* the package name, which is also its shard header id */
    char root[MAX_PATH];       /* absolute path to the package folder, ready to pack */
} sh_mpkg_used;

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used *out, size_t cap);

#endif /* SNAPMAP_PLUS_MAP_EMBED_H */
