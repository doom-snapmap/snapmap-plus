#ifndef SH_NATIVE_PIN_HASH_H
#define SH_NATIVE_PIN_HASH_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Streaming SHA-256, shared with the shard digest implementation. Contexts
 * are local to the caller; finish consumes one context and emits all 32 bytes. */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t fill;
} sh_native_pin_hash;

static const uint32_t sh_native_pin_hash_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SH_NATIVE_PIN_HASH_ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static inline void sh_native_pin_hash_init(sh_native_pin_hash *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->bits = 0; s->fill = 0;
}

static inline void sh_native_pin_hash_block(sh_native_pin_hash *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = SH_NATIVE_PIN_HASH_ROR(w[i-15], 7) ^ SH_NATIVE_PIN_HASH_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = SH_NATIVE_PIN_HASH_ROR(w[i-2], 17) ^ SH_NATIVE_PIN_HASH_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = SH_NATIVE_PIN_HASH_ROR(e, 6) ^ SH_NATIVE_PIN_HASH_ROR(e, 11) ^ SH_NATIVE_PIN_HASH_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sh_native_pin_hash_k[i] + w[i];
        uint32_t S0 = SH_NATIVE_PIN_HASH_ROR(a, 2) ^ SH_NATIVE_PIN_HASH_ROR(a, 13) ^ SH_NATIVE_PIN_HASH_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static inline void sh_native_pin_hash_update(sh_native_pin_hash *s, const unsigned char *p, size_t n)
{
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->block + s->fill, p, take);
        s->fill += take; p += take; n -= take;
        if (s->fill == 64) { sh_native_pin_hash_block(s, s->block); s->fill = 0; }
    }
}

static inline void sh_native_pin_hash_finish(sh_native_pin_hash *s,unsigned char out[32])
{
    unsigned char tail[72];uint64_t bits=s->bits;
    size_t padding=s->fill<56?56-s->fill:120-s->fill;
    memset(tail,0,sizeof tail);tail[0]=0x80;
    for(unsigned i=0;i<8;i++)tail[padding+i]=(unsigned char)(bits>>(56-i*8));
    sh_native_pin_hash_update(s,tail,padding+8);
    for(unsigned i=0;i<32;i++)out[i]=(unsigned char)(s->h[i/4]>>(24-(i%4)*8));
}
#undef SH_NATIVE_PIN_HASH_ROR
#endif
