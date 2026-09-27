#include "native_resource_id.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* MurmurHash2A is Austin Appleby's public-domain algorithm. Explicit byte
 * assembly keeps its little-endian result independent of host alignment. */
static uint32_t nri_mix(uint32_t hash, uint32_t word)
{
    word *= UINT32_C(0x5bd1e995);
    word ^= word >> 24;
    return hash * UINT32_C(0x5bd1e995) ^ word * UINT32_C(0x5bd1e995);
}

static uint32_t nri_hash(const unsigned char *bytes, uint32_t length, uint32_t seed)
{
    uint32_t at = 0, tail = 0;
    while (length - at >= 4) {
        uint32_t word = (uint32_t)bytes[at] | (uint32_t)bytes[at + 1] << 8 |
            (uint32_t)bytes[at + 2] << 16 | (uint32_t)bytes[at + 3] << 24;
        seed = nri_mix(seed, word); at += 4;
    }
    for (uint32_t i = 0; at + i < length; i++) tail |= (uint32_t)bytes[at + i] << (i * 8);
    seed = nri_mix(nri_mix(seed, tail), length);
    seed ^= seed >> 13; seed *= UINT32_C(0x5bd1e995);
    return seed ^ (seed >> 15);
}

char *sh_native_resource_name(const char *name)
{
    char *out;
    size_t length;
    if (!name || !(length = strlen(name)) || length > UINT32_MAX || length == SIZE_MAX) return NULL;
    out = malloc(length + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 128) { free(out); return NULL; }
        out[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
    }
    out[length] = 0;
    if (out[0] == '/' && out[1] != '/') memmove(out, out + 1, length);
    if (!*out) { free(out); return NULL; }
    return out;
}

static int nri_type(const char *native_type, uint32_t *hash)
{
    size_t length;
    if (!native_type || !(length = strlen(native_type)) || length > UINT32_MAX) return 0;
    for (size_t i = 0; i < length; i++) if ((unsigned char)native_type[i] >= 128) return 0;
    *hash = nri_hash((const unsigned char *)native_type, (uint32_t)length, UINT32_C(0x566f8ad7));
    return 1;
}

int sh_native_resource_id(const char *native_type, const char *name, uint32_t *identity)
{
    char *canonical;
    uint32_t hash;
    if (identity) *identity = 0;
    if (!identity || !nri_type(native_type, &hash)) return 0;
    canonical = sh_native_resource_name(name);
    if (!canonical) return 0;
    hash = nri_hash((const unsigned char *)canonical, (uint32_t)strlen(canonical), hash);
    free(canonical); *identity = hash ? hash : 1; return 1;
}

int sh_native_resource_unnamed_id(const char *native_type, uint32_t *identity)
{
    uint32_t hash;
    if (identity) *identity = 0;
    if (!identity || !nri_type(native_type, &hash)) return 0;
    hash = nri_hash((const unsigned char *)"", 0, hash);
    *identity = hash ? hash : 1; return 1;
}
