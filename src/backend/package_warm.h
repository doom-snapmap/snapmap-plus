/* Portable encoding of a published package library for warm starts (R-344 stage B).
 *
 * A record carries only content: bytes, names, SHA-256 identities, recorded native answers and indices into a
 * sources table the reader rebuilds and proves equal first. It never carries pointers, handles, leases,
 * reservations or proof files. Decoding is bounded by the input: every count is checked against the bytes
 * that remain before anything is allocated, every index against the table it names, and a failed decode
 * frees everything it built. Allocation failure is reported apart from malformed input. */
#ifndef SH_PACKAGE_WARM_H
#define SH_PACKAGE_WARM_H
#include <stddef.h>
#include <stdint.h>
#include "package_compiler.h"
#include "package_provenance.h"
#include "package_consumers.h"
#include "package_fsm.h"
#include "package_keys.h"
#include "virtual_material_runtime.h"
#include "config_json.h"

/* Why a decode stopped: malformed input, or memory. */
enum { SH_PACKAGE_WARM_OK = 0, SH_PACKAGE_WARM_MALFORMED = 1, SH_PACKAGE_WARM_MEMORY = 2 };

typedef struct sh_package_warm_writer {
    unsigned char *bytes;
    size_t length, capacity;
    int failed; /* allocation, or the sink refused */
    /* count: only length advances (a sizing pass). sink: bytes pass through a bounded buffer (bytes, buffered) to
     * sink, and length counts every byte written; sh_package_warm_writer_flush hands over the rest. */
    int count;
    int (*sink)(void *context, const void *bytes, size_t length);
    void *sink_context;
    size_t buffered;
} sh_package_warm_writer;
void sh_package_warm_writer_free(sh_package_warm_writer *w);
/* A streaming writer's buffered bytes to its sink; 0 when the writer or the sink failed. */
int sh_package_warm_writer_flush(sh_package_warm_writer *w);
void sh_package_warm_u8(sh_package_warm_writer *w, unsigned value);
void sh_package_warm_u32(sh_package_warm_writer *w, uint32_t value);
void sh_package_warm_u64(sh_package_warm_writer *w, uint64_t value);
void sh_package_warm_i32(sh_package_warm_writer *w, int value);
void sh_package_warm_raw(sh_package_warm_writer *w, const void *bytes, size_t length);
/* Length-prefixed bytes; NULL with length 0 is the empty blob. */
void sh_package_warm_blob(sh_package_warm_writer *w, const void *bytes, size_t length);
/* A string that may be NULL (distinguished from ""). */
void sh_package_warm_text(sh_package_warm_writer *w, const char *text);
void sh_package_warm_owners(sh_package_warm_writer *w, const sh_package_owners *owners);
void sh_package_warm_texts(sh_package_warm_writer *w, char *const *items, size_t count);

typedef struct sh_package_warm_reader {
    const unsigned char *at, *end;
    int failed; /* SH_PACKAGE_WARM_* */
} sh_package_warm_reader;
unsigned sh_package_warm_get_u8(sh_package_warm_reader *r);
uint32_t sh_package_warm_get_u32(sh_package_warm_reader *r);
uint64_t sh_package_warm_get_u64(sh_package_warm_reader *r);
int sh_package_warm_get_i32(sh_package_warm_reader *r);
/* A value below limit (SIZE_MAX accepted when allow_none). */
size_t sh_package_warm_get_index(sh_package_warm_reader *r, size_t limit, int allow_none);
/* A count whose elements take at least element_bytes each in what remains. */
size_t sh_package_warm_get_count(sh_package_warm_reader *r, size_t element_bytes);
const unsigned char *sh_package_warm_get_raw(sh_package_warm_reader *r, size_t length);
/* Borrowed span of a blob. */
const unsigned char *sh_package_warm_get_blob(sh_package_warm_reader *r, size_t *length);
/* Owned copies: a NUL-terminated blob (length+1 allocation), a nullable string (*present 0 for NULL). */
unsigned char *sh_package_warm_get_bytes(sh_package_warm_reader *r, size_t *length);
char *sh_package_warm_get_text(sh_package_warm_reader *r, int *present);
/* A required string: NULL is malformed. */
char *sh_package_warm_get_string(sh_package_warm_reader *r);
/* Owners below package_limit. */
int sh_package_warm_get_owners(sh_package_warm_reader *r, sh_package_owners *out, size_t package_limit);
char **sh_package_warm_get_texts(sh_package_warm_reader *r, size_t *count);
void sh_package_warm_texts_free(char **items, size_t count);
void sh_package_warm_fail(sh_package_warm_reader *r, int why);

/* ---- Published types -------------------------------------------------- */

/* The cache of an opaque resource without a body: none, a copy of its own source file (the reader repeats
 * sh_package_source_cache) or a generated file sealed by its exact leaf and identity in the cache directory. */
enum { SH_PACKAGE_WARM_CACHE_NONE = 0, SH_PACKAGE_WARM_CACHE_SOURCE = 1, SH_PACKAGE_WARM_CACHE_GENERATED = 2 };
typedef struct sh_package_warm_cache {
    size_t resource;
    int kind;
    char *leaf;                       /* GENERATED: the file name in the cache directory */
    sh_package_file_identity identity;
} sh_package_warm_cache;

/* A shared body backed by an installed original's pinned row (sh_package_body_backed) is not copied into the
 * record: origin names the recorded original read whose exact bytes it holds (SIZE_MAX: none, and the body cannot
 * be encoded). The loader reads that original again through the same backed reader, checks its exact identity,
 * and the new body takes the new process's pinned row as its backing. */
typedef size_t (*sh_package_warm_body_origin)(void *context, const sh_package_body *body);
/* The body the loader holds for a retained original read (one new reference), or NULL when the record names a
 * read the loader did not retain (malformed). */
typedef sh_package_body *(*sh_package_warm_body_original)(void *context, size_t read);
/* Encode one published compilation: policy, every resource field, and its bodies once each, with the sharing
 * structure kept (resources sharing one sh_package_body keep sharing one). cache_directory is the directory
 * cache paths name; a cache path outside it cannot be encoded. 0 with the reason when a field cannot be
 * represented (a detached body, a backed body of no recorded original, a borrowed engine path no source file
 * owns, a cache file of no known kind, an installed-original alias holding bytes of its own). An installed-original
 * alias (sh_compiled_resource.installed_alias) is encoded as its certified identity alone; the installed reader and
 * its context are process state, left out of the record and the texts below, and the owner of a decoded compilation
 * sets them as it does for a compiled one. */
int sh_package_warm_put_compilation(sh_package_warm_writer *w, const sh_package_compilation *compiled,
    const char *cache_directory, sh_package_warm_body_origin origin, void *origin_context, char *error, size_t capacity);
/* Decode against sources (file and package bounds; resources borrowing an engine path borrow it from
 * sources again). caches receives the cache file of every opaque resource, to be established by the caller;
 * cache_path and cache_file stay NULL until then. original supplies the bodies of backed originals. */
sh_package_compilation *sh_package_warm_get_compilation(sh_package_warm_reader *r,
    const sh_package_sources *sources, char *(*canonical_path)(const char *),
    sh_package_warm_body_original original, void *original_context,
    sh_package_warm_cache **caches, size_t *cache_count);
void sh_package_warm_caches_free(sh_package_warm_cache *caches, size_t count);

/* Exact order and content: contexts, bindings, uses. */
void sh_package_warm_put_provenance(sh_package_warm_writer *w, const sh_package_provenance *value);
int sh_package_warm_get_provenance(sh_package_warm_reader *r, sh_package_provenance *out);

void sh_package_warm_put_consumers(sh_package_warm_writer *w, const sh_package_consumers *consumers);
/* *out NULL when none were encoded. 0 when malformed or contradictory. */
int sh_package_warm_get_consumers(sh_package_warm_reader *r, sh_package_consumers **out);

void sh_package_warm_put_webs(sh_package_warm_writer *w, const sh_package_fsm_webs *webs);
int sh_package_warm_get_webs(sh_package_warm_reader *r, sh_package_fsm_webs **out);

void sh_package_warm_put_key_groups(sh_package_warm_writer *w, const sh_package_key_groups *groups);
int sh_package_warm_get_key_groups(sh_package_warm_reader *r, sh_package_key_groups **out);

void sh_package_warm_put_json(sh_package_warm_writer *w, const sh_json_object *object);
int sh_package_warm_get_json(sh_package_warm_reader *r, sh_json_object *out);

/* Canonical text of every field of a compilation, with complete bytes of bodies and presented text (not a
 * digest) and every policy member; for exact comparison of two publications. malloc-owned; NULL on memory. */
char *sh_package_warm_compilation_text(const sh_package_compilation *compiled, size_t *length);
char *sh_package_warm_provenance_text(const sh_package_provenance *value, size_t *length);

#endif
