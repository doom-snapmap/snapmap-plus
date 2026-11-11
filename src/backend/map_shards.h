/* Text helpers shared by the map's readers: digesting, base64, bounded
 * scanning and container walks over map JSON (the "snapmap-plus" member,
 * map_carrier.h; navigation, navmesh.c and nav_regions.c). The string-variable
 * envelope earlier builds wrote lives with the store tool (map_old.h).
 */
#ifndef SNAPMAP_PLUS_MAP_SHARDS_H
#define SNAPMAP_PLUS_MAP_SHARDS_H

#include <stddef.h>

/* The sha256 hexdigest prefix navigation records (navmesh.h). */
#define SH_SHARD_DIGEST_CHARS 16

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

#endif /* SNAPMAP_PLUS_MAP_SHARDS_H */
