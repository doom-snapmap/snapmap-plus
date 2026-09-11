/* raw_deflate.h -- bounded raw-DEFLATE decoder shared by installed-resource readers. */
#ifndef BACKEND_RAW_DEFLATE_H
#define BACKEND_RAW_DEFLATE_H

#include <stddef.h>

/* Decode one exactly bounded raw-DEFLATE resource slice into an exactly sized
 * caller buffer. Success requires either a complete BFINAL stream, or Doom's
 * Z_SYNC_FLUSH form: exact exhaustion immediately after a non-final empty
 * stored block (LEN=0/NLEN=0xffff), with exactly dst_len output bytes. In both
 * forms zero alignment padding is required and trailing/concatenated bytes are
 * refused. Canonical Huffman trees are checked for oversubscription/
 * incompleteness, including the RFC/zlib one-symbol EOB-only literal tree, the
 * all-literal or single-distance dynamic tree, and the predefined fixed-distance
 * alphabet. Any malformed, truncated, reserved, or oversize slice returns zero;
 * callers accept only a result equal to dst_len. */
size_t sh_inflate_raw(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_len);

/* The same decoder, with `dst_cap` a CAPACITY instead of the finished length. Returns the actual
 * number of bytes decoded, or zero -- including when the output would not fit, so a zero return can
 * mean either "malformed" or "buffer too small" and the caller retries larger to tell them apart.
 * Stream validation is unchanged: same completeness, padding and trailing-byte rules.
 *
 * For streams whose uncompressed length is not recorded anywhere. A saved map's `map.decl` is one:
 * 4-byte checksum, then zlib(rawmap JSON), and nothing states how big the JSON is. */
size_t sh_inflate_raw_upto(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_cap);

#ifdef SH_RAW_DEFLATE_TESTING
enum {
    SH_INFLATE_HUFF_CODE_LENGTH = 0,
    SH_INFLATE_HUFF_LITERAL_LENGTH,
    SH_INFLATE_HUFF_DISTANCE,
    SH_INFLATE_HUFF_FIXED_DISTANCE
};

/* Test-only structural validator seam. It exercises the same canonical
 * code-space checks used by the decoder without exposing the tree in a build. */
int sh_inflate_test_huff_build(const unsigned char *lens, size_t count,
                               int kind);
#endif

#endif /* BACKEND_RAW_DEFLATE_H */
