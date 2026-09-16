/* Immutable inventory of authored files and their native provider paths. */
#ifndef SH_PACKAGE_SOURCES_H
#define SH_PACKAGE_SOURCES_H

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "packages.h"
#include "package_descriptor.h"

#define SH_PACKAGE_SOURCE_PATH_CAP 32760u

typedef struct sh_package_component {
    size_t owner;                    /* outer delivery package */
    char *relative;                  /* component path within that package */
    char *root;
    sh_package_descriptor descriptor;
    unsigned char descriptor_digest[32];
} sh_package_component;

typedef struct sh_package_source_file {
    size_t owner, component;
    char *relative;                  /* exact path within the outer package */
    char *absolute;
    char *engine_path;               /* canonical lookup key; NULL for author data */
    uint64_t length;
    unsigned char digest[32];
    int directory;                   /* retain empty authored directories too */
} sh_package_source_file;

typedef struct sh_package_sources {
    sh_package *packages;
    size_t package_count;
    sh_package_component *components;
    size_t component_count;
    sh_package_source_file *files;
    size_t file_count;
    unsigned char (*fingerprints)[32]; /* full source tree identities */
} sh_package_sources;

typedef struct sh_package_file_identity {
    uint64_t length;
    unsigned char digest[32];
} sh_package_file_identity;
/* SHA-256 of already-owned bytes, in bounded updates without another payload
 * allocation. Clears the identity on failure. */
int sh_package_bytes_identity(const void *body, size_t length, sh_package_file_identity *out);

/* Returns an owned, complete inventory, or NULL with a diagnostic. A failed
 * scan never publishes a prefix. Sources and descriptors are never rewritten.
 * Discovery supports nested package components. Components do not
 * create separate delivery units. Older layouts must be migrated first. */
sh_package_sources *sh_package_sources_scan(const char *data_root,
                                           char *error, size_t error_capacity);
/* Inventory a single outer package, including every nested component. */
sh_package_sources *sh_package_sources_scan_directory(const char *root,
                                                      char *error, size_t error_capacity);
void sh_package_sources_free(sh_package_sources *sources);
/* The descriptor identity of an outer package: the id of its top-level
 * component, or NULL when the inventory has none. Nested components keep their
 * own ids and are not delivery units. */
const char *sh_package_source_identity(const sh_package_sources *sources, size_t package);
/* Whether a package was delivered by a map rather than authored in place.
 * Delivery publishes each installed group under one "map-<16 hex>" folder, so
 * that leading segment is the runtime's own mark, never an author's setting.
 * Two packages that share a descriptor identity are variants of one package:
 * the authored one owns every resource it supplies, and a delivered variant
 * fills only what the author does not. */
int sh_package_source_delivered(const sh_package_sources *sources, size_t package);
/* Copy two immutable inventories into one ownership space. Every authored
 * member is retained, including duplicate paths and empty directories. This
 * does not discover files, compile peers or write either package tree. */
sh_package_sources *sh_package_sources_join(const sh_package_sources *local,
    const sh_package_sources *map, char *error, size_t error_capacity);

/* Read and verify one inventory file, returning malloc-owned bytes with an
 * extra NUL. Refuse changes since discovery, reparse points and size limits. */
unsigned char *sh_package_source_read(const sh_package_source_file *file,
                                      size_t limit, size_t *length,
                                      char *error, size_t error_capacity);

/* Verify or copy without allocating the entire file. Copy creates a new file
 * and removes only that file on failure; it never overwrites a destination. */
int sh_package_source_verify(const sh_package_source_file *file);
/* Verify using the same handle returned to the caller, positioned at zero.
 * The handle denies writes/deletion until fclose; no whole-file allocation.
 * A stale digest, redirected file or failed read returns NULL. */
FILE *sh_package_source_open(const sh_package_source_file *file,
    char *error, size_t error_capacity);
/* Seal a verified immutable cache file. Its retained read-only OS handle denies
 * writes/deletion; each open has an independent file position and does not hash
 * the payload again. Runtime cache files only: do not seal editable packages.
 * Reader streams remain valid after the last seal reference is released. */
typedef struct sh_package_file sh_package_file;
/* Capture and pin an installed original file, recording its verified identity.
 * Authored source packages remain editable; this service is for original/cache
 * snapshots whose owners retain the handle until their work is complete. */
sh_package_file *sh_package_file_capture(const char *absolute,
    char *error, size_t error_capacity);
int sh_package_file_get_identity(const sh_package_file *file, sh_package_file_identity *out);
sh_package_file *sh_package_file_seal(const sh_package_source_file *file,
    char *error, size_t error_capacity);
sh_package_file *sh_package_file_retain(sh_package_file *file);
void sh_package_file_release(sh_package_file *file);
FILE *sh_package_file_open(const sh_package_file *file, uint64_t *length,
    char *error, size_t error_capacity);
unsigned char *sh_package_file_read(const sh_package_file *file, size_t limit,
    size_t *length, char *error, size_t error_capacity);
int sh_package_source_copy(const sh_package_source_file *file, const char *destination);

/* Exact engine path normalization: case/slash folding only. Never infer
 * resource identities from file extensions. Refuse traversal and ambiguous
 * Windows path spellings. Returned strings are malloc-owned. */
char *sh_package_engine_path(const char *path);
wchar_t *sh_package_source_wide_path(const char *absolute);

#endif
