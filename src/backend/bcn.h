/* bcn.h -- block-compressed texture decoders (BC1, BC3, BC7) -> RGBA8.
 *
 * DOOM's `.bimage` container stores its mips in these formats; the format code in the bimage
 * header maps to them as (doom-re campaign `revenant-asset-index-and-viewport`, evidence 09 sec 3d):
 *
 *     code 10 -> BC1   (8 bytes / 4x4 block, RGB + 1-bit alpha)
 *     code 11 -> BC3   (16 bytes / 4x4, BC4 alpha block + BC1 colour block)
 *     code 23 -> BC7   (16 bytes / 4x4, 8 modes)
 *
 * Unlike the megatexture page codec -- which is id's own and had to be CALLED rather than
 * reimplemented -- BCn is a public, fully specified format, so these are ordinary decoders
 * with no engine dependency at all.
 *
 * All three write RGBA8 into a caller-supplied buffer sized for the PADDED dimensions
 * (width and height each rounded up to a multiple of 4). Cropping to the real size is the
 * caller's job. They never read past `src_len`: short input is treated as zero-filled, which
 * matters because a truncated asset must degrade to a dim thumbnail, not a fault.
 */
#ifndef BACKEND_BCN_H
#define BACKEND_BCN_H

#include <stddef.h>

/* Padded dimension helper: BCn always encodes whole 4x4 blocks. */
#define BCN_PAD(x) (((x) + 3u) & ~3u)

/* Bytes of RGBA output needed for a w x h image (i.e. padded w * padded h * 4). */
size_t bcn_rgba_size(unsigned w, unsigned h);

/* Decode into `dst` (must hold bcn_rgba_size(w,h) bytes). Returns 1 on success, 0 if the
 * arguments are unusable. Out-of-range input is clamped, never trusted. */
int bcn_decode_bc1(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);
int bcn_decode_bc3(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);
int bcn_decode_bc7(const unsigned char *src, size_t src_len, unsigned w, unsigned h, unsigned char *dst);

/* Dispatch on the bimage format code (10/11/23). Returns 0 for codes we do not decode. */
int bcn_decode(unsigned format_code, const unsigned char *src, size_t src_len,
               unsigned w, unsigned h, unsigned char *dst);

#endif /* BACKEND_BCN_H */
