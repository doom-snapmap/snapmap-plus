/* sha256.h -- the product's one SHA-256 (FIPS 180-4): build hashes, file
 * hashes and tree hashes (modbundle.c) and the 16-character shard digests
 * a saved map carries (map_shards.c). */
#ifndef SNAPMAP_PLUS_SHA256_H
#define SNAPMAP_PLUS_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct sh_sha256 {
    uint32_t h[8];
    unsigned char block[64];
    size_t fill;
    uint64_t bits;
} sh_sha256;

void sh_sha256_init(sh_sha256 *s);
void sh_sha256_update(sh_sha256 *s, const void *data, size_t n);
/* Finish and write the digest as lowercase hex: the first `chars` of its 64
 * characters (chars at most 64), then a NUL; `out` holds chars + 1. */
void sh_sha256_hex(sh_sha256 *s, char *out, size_t chars);

#endif /* SNAPMAP_PLUS_SHA256_H */
