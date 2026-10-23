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
    /* Optional original authored trees for private normalized map views.
     * fingerprints above still validates the actual inventoried file bytes.
     * Set only before compiling, from retained importer-verified deliveries. */
    unsigned char (*original_fingerprints)[32];
} sh_package_sources;

static __inline const unsigned char *sh_package_source_tree(const sh_package_sources *sources, size_t owner)
{
    if (!sources || owner >= sources->package_count || !sources->fingerprints) return NULL;
    return sources->original_fingerprints ? sources->original_fingerprints[owner] : sources->fingerprints[owner];
}

typedef struct sh_package_file_identity {
    uint64_t length;
    unsigned char digest[32];
} sh_package_file_identity;
/* SHA-256 of already-owned bytes, in bounded updates without another payload
 * allocation. Clears the identity on failure. */
int sh_package_bytes_identity(const void *body, size_t length, sh_package_file_identity *out);
/* SHA-256 of a bounded span in an immutable random-access source. read must
 * fill each requested span and return 1; a short/failed read returns 0. No
 * payload-sized allocation. Offset and length stay 64-bit for packed files. */
int sh_package_reader_identity(void *context, uint64_t offset, uint64_t length,
    int (*read)(void *, uint64_t, void *, size_t), sh_package_file_identity *out);

/* Returns an owned, complete inventory, or NULL with a diagnostic. A failed
 * scan never publishes a prefix. Sources and descriptors are never rewritten.
 * Discovery supports nested package components. Components do not
 * create separate delivery units. Older layouts must be migrated first. */
sh_package_sources *sh_package_sources_scan(const char *data_root,
                                           char *error, size_t error_capacity);
/* Local authoring libraries isolate outer packages whose own content is
 * invalid: a descriptor, an unusable or duplicate name, a link, or a path the
 * runtime cannot address. Discovery, read, hashing and allocation failures and
 * changes during the scan still fail the whole scan. The callback must record
 * each rejection; returning zero aborts rather than silently admitting an
 * incomplete inventory. Package names stay folder-derived. Map and archive
 * callers continue to use the strict scanners above/below. */
typedef int (*sh_package_source_rejected)(void *context, const sh_package *package,
                                         const char *reason);
sh_package_sources *sh_package_sources_scan_local(const char *data_root,
    sh_package_source_rejected rejected, void *context, char *error, size_t capacity);
/* Remove one complete outer unit from an unpublished inventory, rebasing all
 * component and file ownership. Never changes source files on disk. */
int sh_package_sources_remove(sh_package_sources *sources, size_t package);
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
/* Pin without reading payload bytes. Identity is hashed lazily on first request
 * and then reused. Used for engine-owned external data; authored comparisons
 * must request the full identity and cannot substitute file metadata for it. */
sh_package_file *sh_package_file_pin(const char *absolute,
    char *error, size_t error_capacity);
uint64_t sh_package_file_length(const sh_package_file *file);
int sh_package_file_get_identity(const sh_package_file *file, sh_package_file_identity *out);
sh_package_file *sh_package_file_seal(const sh_package_source_file *file,
    char *error, size_t error_capacity);
/* Stream compiler output into the existing private resource cache. The caller
 * supplies an admitted cache directory and exact output length; expected may
 * additionally require a preverified digest. No source package is modified.
 * Only complete output is published. Equal cached bytes share the existing
 * file; a differing/cache-corrupt occupant is preserved and a separate private
 * cache name is used. On success path is malloc-owned and file is sealed. */
typedef int (*sh_package_file_producer)(void *context,
    int (*write)(void *, const void *, size_t), void *sink, char *error, size_t capacity);
sh_package_file *sh_package_file_generate(const char *directory, uint64_t length,
    const unsigned char *expected, sh_package_file_producer produce, void *context,
    char **path, char *error, size_t capacity);
sh_package_file *sh_package_source_cache(const sh_package_source_file *source,
    const char *directory, char **path, char *error, size_t capacity);
sh_package_file *sh_package_file_retain(sh_package_file *file);
void sh_package_file_release(sh_package_file *file);
/* Exact bounded comparison of sealed bytes: 1 equal, 0 different, -1 read
 * failure. Independent streams leave every existing reader position intact. */
int sh_package_file_equal(const sh_package_file *left, const sh_package_file *right);
FILE *sh_package_file_open(const sh_package_file *file, uint64_t *length,
    char *error, size_t error_capacity);
unsigned char *sh_package_file_read(const sh_package_file *file, size_t limit,
    size_t *length, char *error, size_t error_capacity);
int sh_package_source_copy(const sh_package_source_file *file, const char *destination);
/* Immutable in-memory bytes that several owners share: a compilation resource,
 * the library's original snapshot and a source capture's original keep one copy
 * of the same exact bytes instead of one each. adopt takes a malloc-owned buffer
 * of length bytes, NUL-terminates it (it may move: read it back through
 * sh_package_body_bytes) and holds the first reference; NULL when that cannot be
 * allocated, and then the buffer is untouched and still the caller's. Adopted
 * bytes never change; the last release frees them. References may be taken and
 * released on any thread. */
typedef struct sh_package_body sh_package_body;
sh_package_body *sh_package_body_adopt(unsigned char *bytes, size_t length);
sh_package_body *sh_package_body_retain(sh_package_body *body);
void sh_package_body_release(sh_package_body *body);
/* The bytes, or NULL while they are released to the body's backing (below). */
const unsigned char *sh_package_body_bytes(const sh_package_body *body);
size_t sh_package_body_length(const sh_package_body *body);
/* Where a body's bytes can be read again exactly, so that its holders may let them go in between (an installed
 * original's pinned archive row, sh_resource_slice). read returns malloc-owned bytes and a NUL with *length set, or
 * NULL; release frees the backing's own state. Both may be called on any thread. length is the byte count the
 * backing reads. */
typedef struct sh_package_backing {
    unsigned char *(*read)(void *context, size_t *length);
    void (*release)(void *context);
    void *context;
    size_t length;
    /* Optional: 1 when two contexts of this backing read the very same stored bytes (one pinned archive row). */
    int (*same)(const void *context, const void *other);
} sh_package_backing;
/* Gives body a backing that reads exactly its bytes; the caller has proven that (the bytes were just read from it,
 * or compared equal to such a read). The body takes the backing either way: 0 when it already has one or the lengths
 * differ, and the given backing is then released. */
int sh_package_body_back(sh_package_body *body, sh_package_backing backing);
int sh_package_body_backed(const sh_package_body *body);
/* 1 when both bodies are backed by the same stored bytes (their backing's same), so they hold equal bytes without
 * either being read; 0 otherwise, which says nothing about their bytes. */
int sh_package_body_same_backing(const sh_package_body *body, const sh_package_body *other);
/* Holders. A holder that keeps a pointer to the bytes is attached: adopt and retain make one. A detached holder holds
 * the body but reads its bytes only through it (sh_package_body_read, or by attaching again). When every holder of
 * a backed body is detached its bytes are freed; they are read again from the backing when a holder attaches.
 * detach: the calling attached holder detaches; its pointer must not be used again.
 * attach: the calling detached holder attaches; the bytes, or NULL when the backing cannot read exactly the body's
 * length (the holder stays detached and the body records the failure).
 * retain_detached: one more detached holder (a copy of a detached holder).
 * drop: releases one holder, attached when held (its pointer) is non-NULL, detached otherwise. */
void sh_package_body_detach(sh_package_body *body);
const unsigned char *sh_package_body_attach(sh_package_body *body);
sh_package_body *sh_package_body_retain_detached(sh_package_body *body);
void sh_package_body_drop(sh_package_body *body, const void *held);
/* A transient copy: malloc-owned length bytes and a NUL, from the resident bytes or the backing; NULL when they
 * cannot be allocated or the backing cannot read exactly the body's length (recorded on the body). */
unsigned char *sh_package_body_read(sh_package_body *body, size_t *length);
/* 1 once any read of this body's backing failed. */
int sh_package_body_failed(const sh_package_body *body);
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_PACKAGE_RUNTIME_TESTING)
/* Refuse the nth adoption (0: none) as an exhausted heap would; _left reports a
 * countdown no adoption reached. live counts the bodies not yet freed. */
void sh_package_body_test_refusal(long nth);
long sh_package_body_test_refusal_left(void);
long sh_package_body_test_live(void);
/* Fail the nth backing read (0: none) as an unreadable archive would; _left as above. reads counts backing reads,
 * released the bodies whose bytes are released now, and releases every time a body's bytes were released. */
void sh_package_body_test_backing_failure(long nth);
long sh_package_body_test_backing_failure_left(void);
long sh_package_body_test_backing_reads(void);
long sh_package_body_test_released(void);
long sh_package_body_test_releases(void);
#endif

/* Exact engine path normalization: case/slash folding only. Never infer
 * resource identities from file extensions. Refuse traversal and ambiguous
 * Windows path spellings. Returned strings are malloc-owned. */
char *sh_package_engine_path(const char *path);
wchar_t *sh_package_source_wide_path(const char *absolute);

#endif
