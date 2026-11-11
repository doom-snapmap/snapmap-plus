/* store_deflate.h -- a zlib stream writer for the store tool's saved-map
 * conversion (a saved map's map.decl is a 4-byte header and a zlib stream of
 * the map text). One fixed-Huffman DEFLATE block over a greedy LZ77 match
 * (32 KiB window, hash chains), framed as zlib (RFC 1950) with its Adler-32.
 * Any inflater reads it; the store tool's own check reads it back with
 * raw_deflate.c before anything is written. */
#ifndef SNAPMAP_PLUS_STORE_DEFLATE_H
#define SNAPMAP_PLUS_STORE_DEFLATE_H

#include <stddef.h>

/* The zlib stream of `n` bytes, malloc'd, its length in *out_len; NULL when
 * out of memory. */
unsigned char *sh_store_zlib(const unsigned char *data, size_t n, size_t *out_len);

/* The bytes of a zlib stream (a 2-byte header, raw DEFLATE, a 4-byte
 * Adler-32 that must match), malloc'd and NUL-terminated, at most `max`
 * bytes; NULL when it does not read. */
unsigned char *sh_store_unzlib(const unsigned char *z, size_t n, size_t max, size_t *out_len);

#endif /* SNAPMAP_PLUS_STORE_DEFLATE_H */
