/* Shared envelope for map-level package (smpkg) and navigation (smnav1)
 * payloads. Each variables.string entry carries a family header in info.name
 * and base64 in initialValue.
 *
 * This module provides digesting, base64, bounded scanning, container walks
 * and JSON splices. Families supply their header grammar and payload policy.
 * The scanner uses name and snapVarInfo_t markers to avoid ordinary prose.
 */
#ifndef SNAPMAP_PLUS_MAP_SHARDS_H
#define SNAPMAP_PLUS_MAP_SHARDS_H

#include <stddef.h>

/* sha256 hexdigest prefix carried in a header, shared by every family. */
#define SH_SHARD_DIGEST_CHARS 16

/* Written chunk size; 16384-byte chunks failed native deserialization. */
#define SH_SHARD_CHARS        8192
/* Read cap allows larger chunks from other writers. */
#define SH_SHARD_MAX_CHUNK    65536

/* The longest header text a scanner will consider. Both families are far
 * shorter; a "name" longer than this is not a shard header. */
#define SH_SHARD_HEADER_MAX   256

/* Container storage grows geometrically up to this ceiling. Large maps exceed
 * 8192 containers; cap both count and depth without preallocating the
 * maximum.
 */
#define SH_SHARD_MAX_CONTAINERS (1u << 21)
#define SH_SHARD_CONTAINERS_MIN 8192u
#define SH_SHARD_MAX_DEPTH      256u

/* ---------------------------------------------------------------- text ---- */

const char *sh_shard_find(const char *hay, size_t n, const char *needle, size_t m);
int sh_shard_is_ws(char c);
int sh_shard_is_digit(char c);
int sh_shard_is_hex(char c);
int sh_shard_is_b64(char c);

/* The first 16 lowercase hex characters of the payload's sha256 -- the digest
 * that goes in a shard header. */
void sh_shard_digest16(const unsigned char *payload, size_t len,
                       char out[SH_SHARD_DIGEST_CHARS + 1]);

/* ------------------------------------------------------------- base64 ---- */

/* Standard base64 with padding, into a caller-supplied buffer of at least
 * ((len + 2) / 3) * 4 + 1 bytes. Returns the character count written. */
size_t sh_shard_b64_encode(const unsigned char *p, size_t len, char *out);

typedef struct sh_shard_chunk {
    const char *p;
    size_t      n;
    unsigned    filled;
} sh_shard_chunk;

enum {
    SH_SHARD_B64_OK = 0,
    SH_SHARD_B64_MALFORMED,   /* not base64, or padding in the wrong place */
    SH_SHARD_B64_TOO_BIG,     /* decodes past max_payload */
    SH_SHARD_B64_NOMEM
};

/* Decode `total` chunks, in index order, into one HeapAlloc'd buffer (caller
 * HeapFrees) with *out_len set. Strict: unlike python's b64decode this refuses
 * junk rather than silently discarding it. NULL with *reason set on failure. */
unsigned char *sh_shard_b64_decode(const sh_shard_chunk *chunks, unsigned total,
                                   size_t max_payload, size_t *out_len, int *reason);

/* ------------------------------------------------------------- scanner ---- */

/* Advance to the next shard-variable candidate carrying `magic` at or after
 * *pos, bounded by `len`. Returns 1 when one is found:
 *   *hdr / *hdr_len -- the header text, magic included, quotes excluded
 *   *chunk / *chunk_len -- its base64 value (*chunk == NULL means the value
 *                          could not be read; the family then REFUSES the set
 *                          rather than guessing at it)
 * The header grammar is the family's business: this only guarantees the text is
 * the value of a "name" key inside a snapVarInfo_t and is length-bounded. */
int sh_shard_next(const char *json, size_t len, const char *magic, size_t magic_len,
                  size_t *pos, const char **hdr, size_t *hdr_len,
                  const char **chunk, size_t *chunk_len);

/* ----------------------------------------------------------- structure ---- */

typedef struct sh_shard_container {
    size_t open;    /* offset of '{' or '[' */
    size_t close;   /* offset of the matching '}' or ']' */
    int    parent;  /* index into the container array, -1 for the root */
    char   kind;    /* '{' or '[' */
} sh_shard_container;

typedef struct sh_shard_doc {
    sh_shard_container *c;
    size_t              count;
} sh_shard_doc;

/* Walk containers forward with string/escape handling. Returns 1 and fills
 * doc (free with sh_shard_doc_free), or 0 for malformed/over-capacity input.
 */
int  sh_shard_doc_build(const char *json, size_t len, sh_shard_doc *doc);
void sh_shard_doc_free(sh_shard_doc *doc);

/* The innermost container holding `off`, or -1. */
int  sh_shard_doc_innermost(const sh_shard_doc *doc, size_t off);
/* Walk up from `idx` to the object that is a direct element of an array -- the
 * map variable itself. -1 when there is no such ancestor. */
int  sh_shard_doc_array_element(const sh_shard_doc *doc, int idx);
/* The container that is the value of member `key` DIRECTLY inside `parent`. A
 * map has more than one member called "string"; only the one whose parent is
 * the variables object is the bucket we mean. */
int  sh_shard_doc_member(const char *json, size_t len, const sh_shard_doc *doc,
                         int parent, const char *key);
/* Byte span of the `index`-th element of a flat number-only array. */
int  sh_shard_doc_flat_element(const char *json, const sh_shard_doc *doc, int arr,
                               unsigned index, size_t *from, size_t *to);
/* Count the direct elements of an array container. */
unsigned sh_shard_doc_array_count(const char *json, const sh_shard_doc *doc, int arr);

/* ---------------------------------------------------------- strip/insert -- */

/* Return non-zero to REMOVE the shard variable carrying this header. */
typedef int (*sh_shard_filter_fn)(const char *hdr, size_t hdr_len, void *ctx);

/* Remove matching shard variables into an owned NUL-terminated process-heap
 * buffer. NULL retains the original; doc_failed distinguishes a structural
 * refusal from no matches. Never apply a partial splice.
 */
char *sh_shard_strip(const char *json, size_t len, const char *magic, size_t magic_len,
                     sh_shard_filter_fn filter, void *ctx, size_t max_cuts,
                     size_t *out_len, unsigned *elements_out, unsigned *runs_out,
                     int *doc_failed);

typedef struct sh_shard_out {
    const char *header;
    const char *chunk;
    size_t      chunk_len;
} sh_shard_out;

/* Append shard variables to `variables.string[]` and keep
 * `variables.allocCount[4]` equal to the string-variable count -- the engine
 * reads that slot, not the list length. Returns a new NUL-terminated
 * HeapAlloc'd buffer (caller HeapFrees) with *out_len set, or NULL with the
 * reason in `err`. The caller strips its own family's shards first; this only
 * adds. */
char *sh_shard_insert(const char *json, size_t len,
                      const sh_shard_out *shards, size_t count,
                      size_t *out_len, char *err, size_t err_cap);

#endif /* SNAPMAP_PLUS_MAP_SHARDS_H */
