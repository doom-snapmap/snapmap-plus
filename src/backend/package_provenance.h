/* Internal map bindings; authored package descriptors contain none of this. */
#ifndef SH_PACKAGE_PROVENANCE_H
#define SH_PACKAGE_PROVENANCE_H
#include "package_scope.h"

enum { SH_PACKAGE_CONTEXT_ISOLATED = 1, SH_PACKAGE_CONTEXT_COMPOSED = 2 };
enum {
    SH_PACKAGE_PROVENANCE_CATALOG = 0, /* possible suppliers, no occurrence ownership */
    SH_PACKAGE_PROVENANCE_HISTORICAL = 1, /* SMPBIND0's inseparable binding owners */
    SH_PACKAGE_PROVENANCE_USES = 2
};
enum { SH_PACKAGE_USE_OBJECT = 1, SH_PACKAGE_USE_WORLD = 2, SH_PACKAGE_USE_HISTORICAL = 3 };

typedef struct sh_package_provenance_context {
    unsigned mode;
    unsigned char (*trees)[32];
    size_t tree_count;
    char *component; /* exact component-relative path; NULL for composition */
} sh_package_provenance_context;

typedef struct sh_package_provenance_binding {
    char *family, *original, *name; /* native reader, authored name, saved name */
    size_t *contexts, context_count; /* possible suppliers, not ownership of each use */
} sh_package_provenance_binding;

typedef struct sh_package_provenance_site {
    unsigned kind;
    const char *owner, *reference;
} sh_package_provenance_site;

typedef struct sh_package_provenance_use {
    unsigned kind;
    char *owner, *reference; /* document-local owner token and verified typed site */
    size_t binding;
    size_t *contexts, context_count; /* only this occurrence's selected suppliers */
    /* Optional exact editor row of the owner (OBJECT uses only): 64 lowercase
     * hex digits of a published row's portable identity, NULL when the map
     * records no choice. Every OBJECT use of one owner carries the same value.
     * A recorded choice stays recorded while unresolved; it is never part of
     * the use's key or supplier selection. */
    char *row;
} sh_package_provenance_use;

typedef struct sh_package_provenance {
    sh_package_provenance_context *contexts;
    size_t context_count;
    sh_package_provenance_binding *bindings;
    size_t binding_count;
    unsigned semantics;
    sh_package_provenance_use *uses;
    size_t use_count;
} sh_package_provenance;

/* Builders copy their inputs and collapse exact duplicates. They preserve the
 * previous record on failure. Binding equivalence must already be established
 * by the linker: provenance never decides whether two native objects can share.
 * Record only the source contexts used by this map, not the entire library. */
size_t sh_package_provenance_context_add(sh_package_provenance *out, unsigned mode,
    const unsigned char (*trees)[32], size_t count, const char *component);
int sh_package_provenance_bind(sh_package_provenance *out, const char *family,
    const char *original, const char *name, const size_t *contexts, size_t count);
/* Set or replace one typed use, independently of all equal binding suppliers.
 * Context indices must already belong to the binding. Ordinary uses select one
 * source; only a verified shared registration may select several contributors.
 * Owner/reference are portable document-local keys, not process pointers or
 * unremapped native IDs. Historical uses have empty keys and retain their whole
 * original context set. Convert a HISTORICAL value with select_uses before edits;
 * adding a use directly to a CATALOG starts explicit occurrence ownership. */
int sh_package_provenance_use_set(sh_package_provenance *out,
    const sh_package_provenance_site *site, size_t binding, const size_t *contexts, size_t count);
/* Select live uses and optionally remap their owner/reference keys. Destination
 * is initialized to the original site before each callback. Return 1 to retain,
 * 0 to omit, -1 to fail. Conflicting destination uses fail the whole operation.
 * Output owns copied metadata and contains only selected contexts/bindings.
 * HISTORICAL input exposes an inseparable historical use per binding; CATALOG
 * input cannot establish actual uses and is refused. Empty output stays USES,
 * so removing the final use cannot reactivate unused catalog suppliers.
 * NULL callback retains every use without remapping. Source snapshots and
 * native remaps must be retained/committed by the caller. */
int sh_package_provenance_select_uses(const sh_package_provenance *index,
    int (*selected)(void *, const sh_package_provenance_use *, sh_package_provenance_site *),
    void *context, sh_package_provenance *out);
void sh_package_provenance_free(sh_package_provenance *value);
/* Exact editor row of one document owner. row_set stamps (or with NULL
 * clears) every OBJECT use of owner and returns how many it stamped; an owner
 * without OBJECT uses gains nothing, so a row never creates ownership. -1 for
 * a malformed row. row_get: 1 with borrowed *row, 0 none, -1 when the owner's
 * uses disagree. rows_carry stamps each owner of to that from records, by
 * owner token; owners absent from to drop their row. 1 ok, 0 conflict/memory
 * (to keeps every row it had). */
int sh_package_provenance_row_set(sh_package_provenance *value, const char *owner, const char *row);
int sh_package_provenance_row_get(const sh_package_provenance *value, const char *owner, const char **row);
int sh_package_provenance_rows_carry(const sh_package_provenance *from, sh_package_provenance *to);
/* 1 when row is a well-formed exact row: 64 lowercase hex digits. */
int sh_package_provenance_row_valid(const char *row);

/* Provider overlay: matching map names replace possible catalog suppliers.
 * Recorded actual/historical uses remain independently owned; conflicting use
 * keys fail, so remap keys before combining different documents. Identical local
 * bytes never substitute for a recorded map source. NULL inputs are empty.
 * Output must differ from inputs; failure clears it. */
int sh_package_provenance_overlay(const sh_package_provenance *local,
    const sh_package_provenance *map, sh_package_provenance *out);
/* Binding-level compatibility selection. Catalog input becomes HISTORICAL;
 * USES input retains only actual uses, never its unused supplier union. This
 * cannot select between two occurrences sharing a name; use select_uses for
 * copy, deletion, or live-owner pruning. Predicate: 1 retain, 0 omit, -1 fail. */
int sh_package_provenance_select(const sh_package_provenance *index,
    int (*selected)(void *, const char *family, const char *name), void *context,
    sh_package_provenance *out);
/* Map a saved native name through exact authored contexts to the current
 * compilation's name. Never use package IDs, folder names or another supplier.
 * Return 1 with borrowed name, 0 for an unrecorded vanilla root, -1 if an authored
 * binding cannot be reconstructed consistently for every recorded supplier. */
int sh_package_provenance_resolve(const sh_package_provenance *saved,
    const sh_package_provenance *current, const char *family, const char *name,
    const char **replacement);
/* Resolve one occurrence using only its exact selected contexts. */
int sh_package_provenance_resolve_use(const sh_package_provenance *saved, size_t use,
    const sh_package_provenance *current, const char **replacement);
/* Why an occurrence resolves as it does, for a refusal: its saved binding and
 * every current binding of the same authored source, each marked by whether
 * one of its suppliers is the occurrence's own. Writes a NUL-terminated text. */
void sh_package_provenance_describe_use(const sh_package_provenance *saved, size_t use,
    const sh_package_provenance *current, char *out, size_t capacity);
/* Match the actual document owner and typed value location before resolving.
 * A removed/changed/unrecorded occurrence returns 0 even if another occurrence
 * has the same native name. Historical aggregates retain their old name-level
 * behavior, including aggregates carried forward alongside modern uses.
 * NULL site permits only historical lookup, never a modern supplier union. */
int sh_package_provenance_resolve_site(const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement);

/* Resolve within the supplied immutable map inventory. An isolated tree may
 * have several identical delivery owners; any yields the same lexical view.
 * Composed contexts retain every matching owner through the historical rules.
 * Never pass the local library to fill an absent authored tree. */
int sh_package_provenance_scope(const sh_package_provenance *value, size_t context,
    const sh_package_sources *sources, sh_package_scope *out, char *error, size_t capacity);

/* Canonical, length-delimited internal payload. The shared map-shard envelope
 * supplies transport/checksum. No host paths, author IDs or resource bytes are
 * stored here. Encoding is independent of insertion order and duplicate owners.
 * SMPBIND2 is SMPBIND1 with each use's optional exact row; it is written only
 * when some use records one, so documents without rows keep SMPBIND1 bytes.
 * SMPBIND0 is read as HISTORICAL aggregate ownership, without inventing sites;
 * its composed context means the original historical composition contract,
 * not permission to apply new rules. No absent native recipe is reconstructed
 * here. SMPBIND1 carries explicit use records, including retained historical
 * aggregates. USES encoding prunes by those records, even if the table is empty.
 * Catalog encoding remains the historical binding-only compatibility format.
 * All allocations use malloc; decode replaces a zero-initialized output and
 * clears it on failure. Only owned metadata is parsed, never map prose. */
unsigned char *sh_package_provenance_encode(const sh_package_provenance *value,
    size_t *length, char *error, size_t capacity);
int sh_package_provenance_decode(const unsigned char *body, size_t length,
    sh_package_provenance *out, char *error, size_t capacity);
#endif
