/* map_embed.h -- the author side of map-embedded override packages: work out
 * which installed packages a map actually uses, and pack them into the payload
 * bytes that travel inside it.
 *
 * map_package.c owns the WIRE FORMAT (shard headers, base64, the JSON splice)
 * and the consumer path. This owns the FILESYSTEM: reading a package folder
 * into a zip, and reading a map to decide which folders are worth reading.
 *
 * The split matters because the wire format has a reference implementation to
 * agree with byte for byte, while these two questions have no counterpart --
 * they are what the game knows and an offline packer does not.
 */
#ifndef SNAPMAP_PLUS_MAP_EMBED_H
#define SNAPMAP_PLUS_MAP_EMBED_H

#include <windows.h>
#include <stddef.h>

#include "map_package.h"

/* Pack an override package folder into the payload bytes a map carries.
 *
 * `root` is the package directory -- the one holding package.json, decls/,
 * resources/, requirements/. Every loose file under it is included: game-owned
 * content never lives in a package folder (it is referenced by
 * resources/*.manifest as type/name/virtual-path triples and read from the
 * player's own archives at runtime), so everything here is authored content by
 * construction, binaries included. Extension is not provenance.
 *
 * Entries are STORED, not deflated. The consumer accepts both methods, and a
 * hand-rolled compressor is not worth the correctness risk against a 10 MiB
 * map ceiling that a stored package sits comfortably inside.
 *
 * Returns a HeapAlloc'd zip (caller HeapFrees) with *out_len set, or NULL with
 * the reason in `err`. */
unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len,
                                char *err, size_t err_cap);

/* Which installed packages does this map actually use?
 *
 * A package provides decl identities -- `<pkg>\generated\decls\<type>\<name>.decl`
 * is the identity `<type>/<name>`. A map that uses the package names at least
 * one of those identities somewhere in its JSON. So: collect the map's quoted
 * strings, walk each installed package's decls, and keep the packages with at
 * least one identity in common.
 *
 * The rule is deliberately one-sided. A package with no identity in the map is
 * definitely unused and is left out; a package with one might be used only
 * incidentally, and is embedded anyway. Shipping a package the map did not
 * strictly need costs bytes; omitting one it did need costs the player a
 * broken map.
 *
 * A package whose NAME cannot be a shard header id -- anything outside
 * lowercase [a-z0-9_-], which includes every nested `group/package` name -- is
 * skipped with a log line rather than written into a header the consumer would
 * refuse to parse. `data_root` is the snapmap-plus data root; packages are
 * enumerated below `<data_root>\overrides`.
 *
 * Writes up to `cap` package names into `out` and returns how many. */
typedef struct sh_mpkg_used {
    char id[SH_MPKG_ID_CAP];   /* the package name, which is also its shard header id */
    char root[MAX_PATH];       /* absolute path to the package folder, ready to pack */
} sh_mpkg_used;

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used *out, size_t cap);

#endif /* SNAPMAP_PLUS_MAP_EMBED_H */
