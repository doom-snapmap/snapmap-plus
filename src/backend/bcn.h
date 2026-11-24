/* Decodes BC1, BC3 and BC7 blocks (Bimage codes 10, 11, 23; 4x4 pixels in 8, 16, 16 bytes) to RGBA8 without
 * engine calls. The caller allocates output rounded up to multiples of four and crops; reads past src_len are zero. */
#ifndef BACKEND_BCN_H
#define BACKEND_BCN_H

#include <limits.h>
#include <stddef.h>

/* Pads a dimension to whole 4x4 blocks; zero means it cannot be represented. Evaluates its argument twice. */
#define BCN_PAD(x) ((unsigned)(x) > UINT_MAX - 3u ? 0u : (((unsigned)(x) + 3u) & ~3u))

/* Bytes of RGBA output for a w x h image: padded w * padded h * 4. */
size_t bcn_rgba_size(unsigned w, unsigned h);

/* Decode into dst, which must hold bcn_rgba_size(w,h) bytes. Returns 1 on
 * success, 0 for invalid arguments or dimensions.
 */
int bcn_decode_bc1(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);
int bcn_decode_bc3(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);
int bcn_decode_bc7(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);

/* Dispatch on the bimage format code (10/11/23). Returns 0 for codes we do not decode. */
int bcn_decode(unsigned format_code, const unsigned char *src, size_t src_len,
               unsigned w, unsigned h, unsigned char *dst);

#endif /* BACKEND_BCN_H */
