/* raw_deflate.h -- bounded raw-DEFLATE decoder shared by installed-resource readers. */
#ifndef BACKEND_RAW_DEFLATE_H
#define BACKEND_RAW_DEFLATE_H

#include <stddef.h>

/* Decodes one raw-DEFLATE slice (a BFINAL stream or Doom's Z_SYNC_FLUSH form) into exactly dst_len bytes.
 * Any malformed, truncated, padded or oversize slice returns zero; callers accept only a result equal to dst_len. */
size_t sh_inflate_raw(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_len);
/* Unambiguous validation, including a correctly encoded zero-byte result. */
int sh_inflate_raw_exact(const unsigned char *src, size_t src_len,
                         unsigned char *dst, size_t dst_len);

/* The same decoder with `dst_cap` a capacity, for streams whose length is not recorded (a saved map's map.decl).
 * Returns the bytes decoded, or zero for malformed input or a buffer too small; the caller retries larger to
 * tell them apart. */
size_t sh_inflate_raw_upto(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_cap);

#ifdef SH_RAW_DEFLATE_TESTING
enum {
    SH_INFLATE_HUFF_CODE_LENGTH = 0,
    SH_INFLATE_HUFF_LITERAL_LENGTH,
    SH_INFLATE_HUFF_DISTANCE,
    SH_INFLATE_HUFF_FIXED_DISTANCE
};

/* Test-only entry to the decoder's canonical code-space checks. */
int sh_inflate_test_huff_build(const unsigned char *lens, size_t count,
                               int kind);
#endif

#endif /* BACKEND_RAW_DEFLATE_H */
