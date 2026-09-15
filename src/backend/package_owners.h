/* Scalable internal package provenance; never serialized or author-configured. */
#ifndef SH_PACKAGE_OWNERS_H
#define SH_PACKAGE_OWNERS_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct sh_package_owners {
    uint64_t bits;          /* Keep the common small inventory allocation-free. */
    uint64_t *more;
    size_t words;
} sh_package_owners;

static __inline void sh_package_owners_free(sh_package_owners *owners)
{
    if (owners) { free(owners->more); memset(owners, 0, sizeof(*owners)); }
}

static __inline int sh_package_owners_reserve(sh_package_owners *owners, size_t words)
{
    uint64_t *grown;
    if (words <= owners->words) return 1;
    if (words > SIZE_MAX / sizeof(*grown)) return 0;
    grown = (uint64_t *)realloc(owners->more, words * sizeof(*grown));
    if (!grown) return 0;
    memset(grown + owners->words, 0, (words - owners->words) * sizeof(*grown));
    owners->more = grown; owners->words = words; return 1;
}

static __inline int sh_package_owners_add(sh_package_owners *owners, size_t index)
{
    size_t word = index / 64;
    uint64_t bit = UINT64_C(1) << (index % 64);
    if (!owners) return 0;
    if (!word) { owners->bits |= bit; return 1; }
    if (!sh_package_owners_reserve(owners, word)) return 0;
    owners->more[word - 1] |= bit; return 1;
}

static __inline int sh_package_owners_contains(const sh_package_owners *owners, size_t index)
{
    size_t word = index / 64;
    uint64_t bit = UINT64_C(1) << (index % 64);
    return owners && (!word ? (owners->bits & bit) != 0 :
        word <= owners->words && (owners->more[word - 1] & bit) != 0);
}

/* Failure leaves the destination unchanged. A set may be united with itself. */
static __inline int sh_package_owners_union(sh_package_owners *out, const sh_package_owners *source)
{
    size_t i;
    if (!out || !source) return 0;
    if (out == source) return 1;
    if (!sh_package_owners_reserve(out, source->words)) return 0;
    out->bits |= source->bits;
    for (i = 0; i < source->words; i++) out->more[i] |= source->more[i];
    return 1;
}

static __inline size_t sh_package_owners_count(const sh_package_owners *owners)
{
    size_t i, count = 0;
    uint64_t bits;
    if (!owners) return 0;
    bits = owners->bits;
    while (bits) { bits &= bits - 1; count++; }
    for (i = 0; i < owners->words; i++) {
        bits = owners->more[i];
        while (bits) { bits &= bits - 1; count++; }
    }
    return count;
}

static __inline int sh_package_owners_within(const sh_package_owners *owners, size_t limit)
{
    size_t i, full = limit / 64, remainder = limit % 64;
    uint64_t bits;
    if (!owners || (owners->words && !owners->more)) return 0;
    if (!full && (remainder ? (owners->bits >> remainder) : owners->bits)) return 0;
    for (i = 0; i < owners->words; i++) {
        if (i + 1 < full) continue;
        bits = owners->more[i];
        if (i + 1 == full && remainder) bits >>= remainder;
        if (bits) return 0;
    }
    return 1;
}

#endif
