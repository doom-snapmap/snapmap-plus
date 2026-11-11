/* store_deflate.c -- see store_deflate.h. */
#include <stdlib.h>
#include <string.h>

#include "store_deflate.h"
#include "raw_deflate.h"

typedef struct sd_bits {
    unsigned char *p;
    size_t n, cap;
    unsigned long long acc;
    unsigned have;
    int oom;
} sd_bits;

static void sd_byte(sd_bits *b, unsigned char c)
{
    if (b->oom) return;
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        unsigned char *g = (unsigned char *)realloc(b->p, cap);
        if (!g) { b->oom = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    b->p[b->n++] = c;
}

/* `count` bits of `v`, least significant first. */
static void sd_put(sd_bits *b, unsigned v, unsigned count)
{
    b->acc |= (unsigned long long)v << b->have;
    b->have += count;
    while (b->have >= 8) {
        sd_byte(b, (unsigned char)(b->acc & 0xff));
        b->acc >>= 8;
        b->have -= 8;
    }
}

/* A Huffman code of `len` bits, written most significant first. */
static void sd_code(sd_bits *b, unsigned code, unsigned len)
{
    unsigned r = 0, i;
    for (i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
    sd_put(b, r, len);
}

static void sd_lit(sd_bits *b, unsigned sym)
{
    if (sym < 144) sd_code(b, 0x30 + sym, 8);
    else if (sym < 256) sd_code(b, 0x190 + (sym - 144), 9);
    else if (sym < 280) sd_code(b, sym - 256, 7);
    else sd_code(b, 0xc0 + (sym - 280), 8);
}

static const unsigned short SD_LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char SD_LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
                                                5, 5, 5, 0 };
static const unsigned short SD_DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                                 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char SD_DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                                 11, 11, 12, 12, 13, 13 };

static void sd_match(sd_bits *b, unsigned len, unsigned dist)
{
    unsigned i = 28, d = 29;
    while (SD_LEN_BASE[i] > len) i--;
    sd_lit(b, 257 + i);
    if (SD_LEN_EXTRA[i]) sd_put(b, len - SD_LEN_BASE[i], SD_LEN_EXTRA[i]);
    while (SD_DIST_BASE[d] > dist) d--;
    sd_code(b, d, 5);
    if (SD_DIST_EXTRA[d]) sd_put(b, dist - SD_DIST_BASE[d], SD_DIST_EXTRA[d]);
}

#define SD_WINDOW 32768u
#define SD_HASH   (1u << 16)
#define SD_CHAIN  48

static unsigned sd_hash(const unsigned char *p)
{
    return ((unsigned)p[0] << 8 ^ (unsigned)p[1] << 4 ^ (unsigned)p[2]) * 2654435761u >> 16 & (SD_HASH - 1);
}

unsigned char *sh_store_zlib(const unsigned char *data, size_t n, size_t *out_len)
{
    sd_bits b;
    long long *head = (long long *)malloc(sizeof(long long) * SD_HASH);
    long long *prev = (long long *)malloc(sizeof(long long) * SD_WINDOW);
    unsigned long a1 = 1, a2 = 0;
    size_t i = 0, k;
    if (out_len) *out_len = 0;
    memset(&b, 0, sizeof b);
    if (!head || !prev) { free(head); free(prev); return NULL; }
    for (k = 0; k < SD_HASH; k++) head[k] = -1;
    sd_byte(&b, 0x78);
    sd_byte(&b, 0x9c);
    sd_put(&b, 1, 1);   /* BFINAL */
    sd_put(&b, 1, 2);   /* fixed Huffman */
    while (i < n && !b.oom) {
        unsigned best = 0, dist = 0;
        if (i + 3 <= n) {
            unsigned h = sd_hash(data + i), chain = 0;
            long long c = head[h];
            while (c >= 0 && i - (size_t)c <= SD_WINDOW && chain++ < SD_CHAIN) {
                const unsigned char *x = data + c, *y = data + i;
                unsigned len = 0, max = (unsigned)(n - i < 258 ? n - i : 258);
                while (len < max && x[len] == y[len]) len++;
                if (len > best) { best = len; dist = (unsigned)(i - (size_t)c); if (len == max) break; }
                c = prev[(size_t)c % SD_WINDOW];
            }
        }
        if (best >= 3) {
            unsigned j;
            sd_match(&b, best, dist);
            for (j = 0; j < best; j++, i++) {
                if (i + 3 <= n) {
                    unsigned h = sd_hash(data + i);
                    prev[i % SD_WINDOW] = head[h];
                    head[h] = (long long)i;
                }
            }
        } else {
            if (i + 3 <= n) {
                unsigned h = sd_hash(data + i);
                prev[i % SD_WINDOW] = head[h];
                head[h] = (long long)i;
            }
            sd_lit(&b, data[i]);
            i++;
        }
    }
    sd_lit(&b, 256);
    if (b.have) sd_put(&b, 0, 8 - b.have);
    for (k = 0; k < n; k++) {
        a1 = (a1 + data[k]) % 65521u;
        a2 = (a2 + a1) % 65521u;
    }
    sd_byte(&b, (unsigned char)(a2 >> 8));
    sd_byte(&b, (unsigned char)a2);
    sd_byte(&b, (unsigned char)(a1 >> 8));
    sd_byte(&b, (unsigned char)a1);
    free(head);
    free(prev);
    if (b.oom) { free(b.p); return NULL; }
    if (out_len) *out_len = b.n;
    return b.p;
}

unsigned char *sh_store_unzlib(const unsigned char *z, size_t n, size_t max, size_t *out_len)
{
    size_t cap = n * 8 + 4096, got = 0, k;
    unsigned long a1 = 1, a2 = 0, want;
    unsigned char *out = NULL;
    if (out_len) *out_len = 0;
    if (!z || n < 6 || (z[0] & 0x0f) != 8 || ((unsigned)z[0] << 8 | z[1]) % 31 || (z[1] & 0x20)) return NULL;
    for (;;) {
        unsigned char *g;
        if (cap > max + 1) cap = max + 1;
        g = (unsigned char *)realloc(out, cap + 1);
        if (!g) { free(out); return NULL; }
        out = g;
        got = sh_inflate_raw_upto(z + 2, n - 6, out, cap);
        if (got) break;
        if (cap > max) { free(out); return NULL; }
        cap *= 2;
    }
    if (got > max) { free(out); return NULL; }
    for (k = 0; k < got; k++) {
        a1 = (a1 + out[k]) % 65521u;
        a2 = (a2 + a1) % 65521u;
    }
    want = (unsigned long)z[n - 4] << 24 | (unsigned long)z[n - 3] << 16 | (unsigned long)z[n - 2] << 8 | z[n - 1];
    if (want != (a2 << 16 | a1)) { free(out); return NULL; }
    out[got] = 0;
    if (out_len) *out_len = got;
    return out;
}
