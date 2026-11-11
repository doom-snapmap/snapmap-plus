/* sha256.c -- the product's one SHA-256 (sha256.h). */
#include <string.h>
#include "sha256.h"

static const uint32_t SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

void sh_sha256_init(sh_sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, iv, sizeof iv);
    s->fill = 0;
    s->bits = 0;
}

static void sha_block(sh_sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = SHA_ROR(w[i - 15], 7) ^ SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA_ROR(w[i - 2], 17) ^ SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = SHA_ROR(e, 6) ^ SHA_ROR(e, 11) ^ SHA_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = SHA_ROR(a, 2) ^ SHA_ROR(a, 13) ^ SHA_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sh_sha256_update(sh_sha256 *s, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    s->bits += (uint64_t)n * 8u;
    while (n) {
        size_t take;
        if (s->fill == 0 && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
        take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->block + s->fill, p, take);
        s->fill += take; p += take; n -= take;
        if (s->fill == 64) { sha_block(s, s->block); s->fill = 0; }
    }
}

void sh_sha256_hex(sh_sha256 *s, char *out, size_t chars)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char tail[72] = {0};   /* 0x80, zero padding, the 8 big-endian length bytes */
    uint64_t bits = s->bits;        /* the message length, before the padding */
    size_t pad = s->fill < 56 ? 56 - s->fill : 120 - s->fill, i;
    tail[0] = 0x80;
    for (i = 0; i < 8; i++) tail[pad + i] = (unsigned char)(bits >> (56 - 8 * i));
    sh_sha256_update(s, tail, pad + 8);
    if (chars > 64) chars = 64;
    for (i = 0; i < chars; i++) {
        unsigned char byte = (unsigned char)(s->h[i / 8] >> (24 - 8 * ((i / 2) % 4)));
        out[i] = hex[i % 2 ? byte & 15 : byte >> 4];
    }
    out[chars] = 0;
}
