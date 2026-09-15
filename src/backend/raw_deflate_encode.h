/* Deterministic, bounded RFC 1951 fixed-Huffman compression. */
#ifndef SH_RAW_DEFLATE_ENCODE_H
#define SH_RAW_DEFLATE_ENCODE_H
#include <stddef.h>
/* Returns malloc-owned raw DEFLATE, including a valid empty stream. */
unsigned char *sh_deflate_raw(const unsigned char *input, size_t length, size_t *out_length);
#endif
