/* raw_deflate.c -- small bounded decoder for the public raw-DEFLATE format. */
#include <stddef.h>
#include <string.h>

#include "raw_deflate.h"

typedef struct {
    const unsigned char *src;
    size_t len, pos;
    unsigned bitbuf, bitcnt;
    int exhausted;
} inf_t;

static unsigned inf_bits(inf_t *s, unsigned n)
{
    while (s->bitcnt < n) {
        unsigned b;
        if (s->pos >= s->len) {
            s->exhausted = 1;
            return 0;
        }
        b = s->src[s->pos++];
        s->bitbuf |= b << s->bitcnt;
        s->bitcnt += 8;
    }
    {
        unsigned v = s->bitbuf & ((1u << n) - 1u);
        s->bitbuf >>= n;
        s->bitcnt -= n;
        return v;
    }
}

/* Align stored blocks to the next byte. This decoder additionally requires
 * zero padding bits as a resource-slice policy.
 */
static int inf_align_zero(inf_t *s)
{
    unsigned mask = (1u << (s->bitcnt & 7u)) - 1u;
    if (s->bitbuf & mask) return 0;
    s->pos -= s->bitcnt / 8u;   /* stored data starts at the first untouched byte */
    s->bitbuf = 0;
    s->bitcnt = 0;
    return 1;
}

/* Treat pindex zsize as an exact slice. Require zero remaining bits after the
 * final block and reject any extra byte, including zero padding.
 */
static int inf_at_exact_end(const inf_t *s)
{
    unsigned mask;
    if (!s || s->pos != s->len || s->bitcnt > 7u) return 0;
    mask = s->bitcnt ? ((1u << s->bitcnt) - 1u) : 0u;
    return (s->bitbuf & mask) == 0;
}

typedef enum {
    HUFF_CODE_LENGTH,
    HUFF_LITERAL_LENGTH,
    HUFF_DISTANCE,
    HUFF_FIXED_DISTANCE
} huff_kind;

#define HUFF_PREFIX_BITS 9u
#define HUFF_PREFIX_SIZE (1u << HUFF_PREFIX_BITS)

typedef struct {
    unsigned short count[16], symbol[288];
    unsigned short prefix[HUFF_PREFIX_SIZE];
    int empty;
} huff_t;

static int huff_build(huff_t *h, const unsigned char *lens, unsigned n,
                      huff_kind kind)
{
    unsigned offs[16], i, used = 0, max = 0;
    int left = 1;
    if (!h || !lens || n > 288u) return 0;
    memset(h, 0, sizeof(*h));
    for (i = 0; i < n; ++i) {
        if (lens[i] > 15u) return 0;
        h->count[lens[i]]++;
        if (lens[i]) used++;
    }
    for (i = 1; i <= 15u; ++i) {
        if (h->count[i]) max = i;
        left <<= 1;
        left -= (int)h->count[i];
        if (left < 0) return 0; /* oversubscribed code space */
    }
    if (!used) {
        /* RFC 1951 permits a zero-bit distance alphabet when the block is all
         * literals. huff_decode will reject any attempted distance symbol. */
        if (kind != HUFF_DISTANCE) return 0;
        h->empty = 1;
        return 1;
    }
    if (left) {
        /* The code-length alphabet must be complete, except the one-bit single-symbol literal/length (EOB-only)
         * tree, a dynamic distance tree with one symbol or none, and the fixed distance table's two reserved holes
         * (30 five-bit codes). */
        if (kind == HUFF_LITERAL_LENGTH && used == 1u && max == 1u) {
            /* permitted EOB-only literal/length tree */
        } else if (kind == HUFF_DISTANCE && used == 1u && max == 1u) {
            /* permitted single-symbol distance tree */
        } else if (kind == HUFF_FIXED_DISTANCE && used == 30u && max == 5u &&
                   h->count[5] == 30u) {
            /* the predefined fixed distance alphabet */
        } else {
            return 0; /* incomplete code space */
        }
    }
    h->count[0] = 0;
    offs[0] = 0;
    for (i = 1; i < 16; ++i) offs[i] = offs[i - 1] + h->count[i - 1];
    for (i = 0; i < n; ++i) if (lens[i]) h->symbol[offs[lens[i]]++] = (unsigned short)i;
    /* The next input bit occupies bit zero. Reverse the canonical code for the table index. */
    unsigned code = 0, symbol = 0;
    for (unsigned len = 1; len <= HUFF_PREFIX_BITS; ++len) {
        code = (code + h->count[len - 1]) << 1;
        for (unsigned j = 0; j < h->count[len]; ++j) {
            unsigned bits = code + j, reversed = 0;
            for (unsigned k = 0; k < len; ++k, bits >>= 1) reversed = (reversed << 1) | (bits & 1);
            unsigned short entry = (unsigned short)((h->symbol[symbol++] << 4) | len);
            for (unsigned k = reversed; k < HUFF_PREFIX_SIZE; k += 1u << len) h->prefix[k] = entry;
        }
    }
    return 1;
}

static int huff_decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len;
    if (!h || h->empty) return -1;
    /* Buffer enough for the longest code when possible. Only the matching code consumes bits. */
    if (s->bitcnt < 15u) {
        if (s->len - s->pos >= 2) {
            unsigned pair = s->src[s->pos] | ((unsigned)s->src[s->pos + 1] << 8);
            s->bitbuf |= pair << s->bitcnt;
            s->bitcnt += 16;
            s->pos += 2;
        } else if (s->pos < s->len) {
            s->bitbuf |= (unsigned)s->src[s->pos++] << s->bitcnt;
            s->bitcnt += 8;
        }
    }
    unsigned entry = h->prefix[s->bitbuf & (HUFF_PREFIX_SIZE - 1)], bits = entry & 15u;
    if (bits && bits <= s->bitcnt) {
        s->bitbuf >>= bits;
        s->bitcnt -= bits;
        return (int)(entry >> 4);
    }
    unsigned input = s->bitbuf;
    for (len = 1; len < 16 && (unsigned)len <= s->bitcnt; ++len) {
        int count;
        code |= (int)(input & 1u);
        input >>= 1;
        count = h->count[len];
        if (code - count < first) {
            s->bitbuf = input;
            s->bitcnt -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const unsigned short LBASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned short LEXT[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short DBASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577
};
static const unsigned short DEXT[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* The decoder body, shared by both entry points: with `exact` the output length must equal `dst_len`, otherwise it
 * is only a capacity (a saved map's `map.decl` is zlib(rawmap JSON) with no length recorded). Stream validation is
 * identical in both: complete BFINAL or Z_SYNC_FLUSH form, zero padding, no trailing or concatenated bytes. */
static size_t inflate_body(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_len, int exact, int *valid)
{
    inf_t s;
    size_t out = 0;
    huff_t lit, dist;
    if (valid) *valid = 0;
    if (!src || !dst || !src_len) return 0;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.len = src_len;

    for (;;) {
        unsigned final, type;
        final = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (s.exhausted) return 0;
        if (type == 0) {
            unsigned len, nlen;
            if (!inf_align_zero(&s)) return 0;
            if (s.len - s.pos < 4u) return 0;
            len = (unsigned)s.src[s.pos] | ((unsigned)s.src[s.pos + 1] << 8);
            nlen = (unsigned)s.src[s.pos + 2] | ((unsigned)s.src[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xffffu) != nlen || len > s.len - s.pos || len > dst_len - out)
                return 0;
            memcpy(dst + out, s.src + s.pos, len);
            s.pos += len;
            out += len;
            if (!final && !len && nlen == 0xffffu) {
                /* Doom's gameresources slices are zlib Z_SYNC_FLUSH fragments ending right after this non-final
                 * empty stored block; no second stream or trailing byte is tolerated. */
                if ((exact && out != dst_len) || s.pos != s.len) return 0;
                if (valid) *valid = 1;
                return out;
            }
        } else if (type == 1 || type == 2) {
            if (type == 1) {
                unsigned char l[288], d[30];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                for (i = 0; i < 30; ++i) d[i] = 5;
                if (!huff_build(&lit, l, 288, HUFF_LITERAL_LENGTH) ||
                    !huff_build(&dist, d, 30, HUFF_FIXED_DISTANCE)) return 0;
            } else {
                static const unsigned char ord[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                unsigned nlen = inf_bits(&s, 5) + 257u;
                unsigned ndist = inf_bits(&s, 5) + 1u;
                unsigned ncode = inf_bits(&s, 4) + 4u;
                unsigned char cl[19], lens[320];
                huff_t clh;
                unsigned i;
                if (s.exhausted || nlen > 286u || ndist > 30u || nlen + ndist > 320u)
                    return 0;
                memset(cl, 0, sizeof(cl));
                for (i = 0; i < ncode; ++i) cl[ord[i]] = (unsigned char)inf_bits(&s, 3);
                if (s.exhausted || !huff_build(&clh, cl, 19, HUFF_CODE_LENGTH)) return 0;
                memset(lens, 0, sizeof(lens));
                i = 0;
                while (i < nlen + ndist) {
                    int sym = huff_decode(&s, &clh);
                    unsigned repeat;
                    unsigned char value;
                    if (sym < 0) return 0;
                    if (sym < 16) {
                        lens[i++] = (unsigned char)sym;
                        continue;
                    }
                    if (sym == 16) {
                        if (!i) return 0;
                        value = lens[i - 1];
                        repeat = 3u + inf_bits(&s, 2);
                    } else if (sym == 17) {
                        value = 0;
                        repeat = 3u + inf_bits(&s, 3);
                    } else if (sym == 18) {
                        value = 0;
                        repeat = 11u + inf_bits(&s, 7);
                    } else return 0;
                    if (s.exhausted || repeat > nlen + ndist - i) return 0;
                    while (repeat--) lens[i++] = value;
                }
                /* Symbol 256 is the mandatory end-of-block code. A tree that
                 * can emit data but cannot terminate is malformed even when
                 * its canonical code space is otherwise valid. */
                if (!lens[256] ||
                    !huff_build(&lit, lens, nlen, HUFF_LITERAL_LENGTH) ||
                    !huff_build(&dist, lens + nlen, ndist, HUFF_DISTANCE)) return 0;
            }

            for (;;) {
                int sym = huff_decode(&s, &lit);
                if (sym < 0) return 0;
                if (sym < 256) {
                    if (out >= dst_len) return 0;
                    dst[out++] = (unsigned char)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    unsigned len, distance;
                    int ds;
                    sym -= 257;
                    if (sym < 0 || sym >= 29) return 0;
                    len = LBASE[sym] + inf_bits(&s, LEXT[sym]);
                    ds = huff_decode(&s, &dist);
                    if (s.exhausted || ds < 0 || ds >= 30) return 0;
                    distance = DBASE[ds] + inf_bits(&s, DEXT[ds]);
                    if (s.exhausted || distance > out || len > dst_len - out) return 0;
                    while (len--) {
                        dst[out] = dst[out - distance];
                        out++;
                    }
                }
            }
        } else return 0;
        if (final) {
            if ((exact && out != dst_len) || !inf_at_exact_end(&s)) return 0;
            if (valid) *valid = 1;
            return out;
        }
    }
}

size_t sh_inflate_raw(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_len)
{
    return inflate_body(src, src_len, dst, dst_len, 1, NULL);
}

size_t sh_inflate_raw_upto(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_cap)
{
    return inflate_body(src, src_len, dst, dst_cap, 0, NULL);
}

int sh_inflate_raw_exact(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len)
{
    int valid = 0;
    inflate_body(src, src_len, dst, dst_len, 1, &valid);
    return valid;
}
