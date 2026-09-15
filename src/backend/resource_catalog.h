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
void sh_resource_catalog_close(sh_resource_catalog *catalog);
size_t sh_resource_catalog_count(const sh_resource_catalog *catalog);
const sh_resource_catalog_entry *sh_resource_catalog_at(const sh_resource_catalog *catalog, size_t index);

/* Compatible with the package compiler's baseline-reader callback. Return 1
 * for SnapMap, 2 for campaign-only, 0 if absent, -1 on unreadable/ambiguous
 * data. Bytes are malloc-owned. Conflicting duplicate rows never pick a winner. */
int sh_resource_catalog_read_path(void *catalog, const char *path,
                                   unsigned char **body, size_t *length);
int sh_resource_catalog_read_entry(sh_resource_catalog *catalog, size_t index,
                                    unsigned char **body, size_t *length);

/* List all native paths associated with an identity, including permutations.
 * The returned span is stable for the catalog lifetime. */
size_t sh_resource_catalog_find(const sh_resource_catalog *catalog,
                                 const char *type, const char *name,
                                 const sh_resource_catalog_entry *const **entries);

/* Reverse lookup includes every identity/permutation and both archives. This
 * supplies native cache identities, not a precedence decision or file read. */
size_t sh_resource_catalog_find_path(const sh_resource_catalog *catalog,
    const char *path, const sh_resource_catalog_entry *const **entries);

#endif
