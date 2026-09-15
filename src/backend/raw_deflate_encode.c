#include "raw_deflate_encode.h"
#include <stdint.h>
#include <stdlib.h>

/* Format: https://www.rfc-editor.org/rfc/rfc1951, section 3.2.
 * A single recent match per hash keeps worst-case search work bounded.
 * Callers use stored ZIP members when compression would expand the input. */
typedef struct de_writer {
    unsigned char *data;
    size_t length, capacity;
    unsigned bits, pending;
    int failed;
} de_writer;

static void de_bits(de_writer *w, unsigned value, unsigned count)
{
    w->bits |= value << w->pending; w->pending += count;
    while (w->pending >= 8) {
        if (w->length == w->capacity) { w->failed = 1; return; }
        w->data[w->length++] = (unsigned char)w->bits;
        w->bits >>= 8; w->pending -= 8;
    }
}

static unsigned de_reverse(unsigned value, unsigned count)
{
    unsigned out = 0;
    while (count--) { out = (out << 1) | (value & 1u); value >>= 1; }
    return out;
}

static void de_symbol(de_writer *w, unsigned symbol)
{
    unsigned code, bits;
    if (symbol < 144) { code = 0x30u + symbol; bits = 8; }
    else if (symbol < 256) { code = 0x190u + symbol - 144u; bits = 9; }
    else if (symbol < 280) { code = symbol - 256u; bits = 7; }
    else { code = 0xc0u + symbol - 280u; bits = 8; }
    de_bits(w, de_reverse(code, bits), bits);
}

static void de_match(de_writer *w, unsigned length, unsigned distance)
{
    static const unsigned short lengths[] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    unsigned l = 0, d = 0, extra, base = 1;
    while (l < 28 && length >= lengths[l + 1]) l++;
    extra = l < 8 || l == 28 ? 0 : (l - 4) / 4;
    de_symbol(w, 257 + l); de_bits(w, length - lengths[l], extra);
    while (d < 29) {
        unsigned width = 1u << (d < 4 ? 0 : d / 2 - 1);
        if (distance < base + width) break;
        base += width; d++;
    }
    extra = d < 4 ? 0 : d / 2 - 1;
    de_bits(w, de_reverse(d, 5), 5); de_bits(w, distance - base, extra);
}

static unsigned de_hash(const unsigned char *p)
{ return ((unsigned)p[0] * 251u * 251u + (unsigned)p[1] * 251u + p[2]) & 65535u; }

unsigned char *sh_deflate_raw(const unsigned char *input, size_t length, size_t *out_length)
{
    de_writer w = {0};
    uint32_t *heads = NULL;
    size_t pos = 0;
    if (out_length) *out_length = 0;
    if (!out_length || (!input && length) || length >= UINT32_MAX ||
        length > SIZE_MAX - length / 8u - 16u) return NULL;
    w.capacity = length + length / 8u + 16u;
    w.data = (unsigned char *)malloc(w.capacity);
    heads = (uint32_t *)calloc(65536u, sizeof(*heads));
    if (!w.data || !heads) goto bad;
    de_bits(&w, 3, 3); /* final block, fixed Huffman */
    while (pos < length && !w.failed) {
        size_t match = 0, previous = 0, consumed, end;
        if (length - pos >= 3) {
            uint32_t candidate = heads[de_hash(input + pos)];
            if (candidate) {
                previous = candidate - 1u;
                if (pos - previous <= 32768u) {
                    size_t limit = length - pos < 258u ? length - pos : 258u;
                    while (match < limit && input[previous + match] == input[pos + match]) match++;
                }
            }
        }
        consumed = match >= 3 ? match : 1;
        if (match >= 3) de_match(&w, (unsigned)match, (unsigned)(pos - previous));
        else de_symbol(&w, input[pos]);
        end = pos + consumed;
        while (pos < end) {
            if (length - pos >= 3) heads[de_hash(input + pos)] = (uint32_t)pos + 1u;
            pos++;
        }
    }
    if (w.failed) goto bad;
    de_symbol(&w, 256);
    if (w.pending) de_bits(&w, 0, 8u - w.pending);
    if (w.failed) goto bad;
    free(heads); *out_length = w.length; return w.data;
bad:
    free(heads); free(w.data); return NULL;
}
