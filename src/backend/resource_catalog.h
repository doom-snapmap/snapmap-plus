/* Verified installed resource identities, paths and read-only archive slices. */
#ifndef SH_RESOURCE_CATALOG_H
#define SH_RESOURCE_CATALOG_H

#include <stddef.h>
#include <stdint.h>

typedef struct sh_resource_catalog sh_resource_catalog;
typedef struct sh_resource_catalog_entry {
    char *type, *name, *path;
    uint64_t offset;
    uint32_t size, stored_size;
    unsigned char archive;          /* catalog * 2 + patch selector */
} sh_resource_catalog_entry;

/* SnapMap catalog is preferred for overlapping paths. Campaign resources
 * remain available as verified import baselines. This does not modify the
 * engine's resource lists or admit every archive file as an override. */
sh_resource_catalog *sh_resource_catalog_open(const char *doom_base,
                                              char *error, size_t error_capacity);
/* A catalog has counted holders: open makes the first, retain another, and close releases one; the last close frees
 * it (its slices keep the archive handles they took). It is immutable after open and reads its archives under its
 * handle-set lock, so holders on different threads may share one. */
sh_resource_catalog *sh_resource_catalog_retain(sh_resource_catalog *catalog);
void sh_resource_catalog_close(sh_resource_catalog *catalog);
/* 1 when the installation under doom_base is exactly the one this catalog read: both index files hash with the
 * archive sizes to its digest, and each archive path names the very file the catalog has held open since it opened
 * (read sharing only, so no writer or deleter could change it). A fresh open would then answer every read as this
 * catalog does. 0 when anything differs (an absent index or archive included); -1 with the error when a present file
 * cannot be examined. */
int sh_resource_catalog_matches(const sh_resource_catalog *catalog, const char *doom_base, char *error, size_t capacity);
size_t sh_resource_catalog_count(const sh_resource_catalog *catalog);
/* SHA-256 of the index bytes this catalog was read from and its archive
 * sizes: every row it can report. It says nothing about archive payload
 * bytes, which a reader must validate itself. 1 with the digest. */
int sh_resource_catalog_digest(const sh_resource_catalog *catalog, unsigned char out[32]);
const sh_resource_catalog_entry *sh_resource_catalog_at(const sh_resource_catalog *catalog, size_t index);

/* Compatible with the package compiler's baseline-reader callback. Return 1
 * for SnapMap, 2 for campaign-only, 0 if absent, -1 on unreadable/ambiguous
 * data. Bytes are malloc-owned. Conflicting duplicate rows never pick a winner.
 * A path no row names reads the rows provided under it without a filename
 * (sh_resource_catalog_find_provider). */
int sh_resource_catalog_read_path(void *catalog, const char *path,
                                   unsigned char **body, size_t *length);
/* An installed row's exact bytes, readable again for as long as the slice is held. The slice keeps the archive
 * handles its catalog opened (read access, sharing read only, so no writer or deleter can open the file while any
 * handle is held) and the row's archive, offset and sizes: reading it repeats the catalog's own read of the same
 * stored bytes through those handles and never resolves a path again, also after the catalog closes. References
 * may be taken and released on any thread. */
typedef struct sh_resource_slice sh_resource_slice;
/* sh_resource_catalog_read_path, also returning in *slice the row its bytes were decoded from when they are one
 * named row's decode (each duplicate row verified equal); NULL for absence, rows without a filename, failures, or
 * when the slice cannot be allocated (the bytes are then simply the caller's). */
int sh_resource_catalog_read_path_slice(void *catalog, const char *path,
    unsigned char **body, size_t *length, sh_resource_slice **slice);
/* malloc-owned bytes and a NUL, exactly the slice's decoded length; NULL when the archive cannot be read, the
 * payload does not decode to exactly that length, or memory is exhausted. */
unsigned char *sh_resource_slice_read(const sh_resource_slice *slice, size_t *length);
size_t sh_resource_slice_length(const sh_resource_slice *slice);
/* 1 when both slices read one row of the same held archive handles (and so the same bytes). */
int sh_resource_slice_same(const sh_resource_slice *slice, const sh_resource_slice *other);
sh_resource_slice *sh_resource_slice_retain(sh_resource_slice *slice);
void sh_resource_slice_release(sh_resource_slice *slice);
int sh_resource_catalog_read_entry(sh_resource_catalog *catalog, size_t index,
                                    unsigned char **body, size_t *length);
/* Exact campaign identity/path with duplicate verification: 1 = malloc bytes,
 * 2 = identical to every shipped SnapMap provider, 0 = failure. type == NULL
 * queries a path (name == "directory" queries a directory): 1/0/-1. */
int sh_resource_catalog_legacy_read(void *context, const char *type,
    const char *name, const char *path, unsigned char **body, size_t *length,
    char *error, size_t capacity);

/* List all native paths associated with an identity, including permutations.
 * The returned span is stable for the catalog lifetime. */
size_t sh_resource_catalog_find(const sh_resource_catalog *catalog,
                                 const char *type, const char *name,
                                 const sh_resource_catalog_entry *const **entries);

/* Reverse lookup includes every identity/permutation and both archives. This
 * supplies native cache identities, not a precedence decision or file read. */
size_t sh_resource_catalog_find_path(const sh_resource_catalog *catalog,
    const char *path, const sh_resource_catalog_entry *const **entries);

/* Identity of a supplied file. Rows with this filename win; otherwise an index
 * row without a filename is provided under its resource name, as the earlier
 * resource bridge and legacy import served it (zero-byte collision placeholders). */
size_t sh_resource_catalog_find_provider(const sh_resource_catalog *catalog,
    const char *path, const sh_resource_catalog_entry *const **entries);

#endif
