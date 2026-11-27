#include "raw_deflate.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { printf("line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static int check_stream(const unsigned char *stream, size_t n, const unsigned char *plain, size_t size)
{
    unsigned char out[4210], trailing[128];
    CHECK(n < sizeof trailing && size < sizeof out);
    CHECK(sh_inflate_raw_exact(stream, n, out, size) && memcmp(out, plain, size) == 0);
    CHECK(sh_inflate_raw_upto(stream, n, out, sizeof out) == size && memcmp(out, plain, size) == 0);
    for (size_t i = 0; i < n; ++i) CHECK(!sh_inflate_raw_exact(stream, i, out, size));
    memcpy(trailing, stream, n);
    trailing[n] = 0;
    CHECK(!sh_inflate_raw_exact(trailing, n + 1, out, size));
    CHECK(!sh_inflate_raw_exact(stream, n, out, size + 1));
    if (size) CHECK(!sh_inflate_raw_exact(stream, n, out, size - 1));
    return 0;
}

int main(void)
{
    /* Independent zlib streams with 9-bit fixed literals, a dynamic tree and an empty sync block. */
    const unsigned char fixed[] = {0xeb,0x9f,0x70,0xf0,0x1f,0x03,0x00};
    const unsigned char literals[] = {143,144,193,254,0};
    const unsigned char dynamic[] = {
        0xed,0xc4,0x37,0x01,0x00,0x00,0x08,0x00,0xa0,0x6c,0xce,0xfe,0x91,0x0c,0xe1,
        0x0b,0x07,0x91,0xd5,0xb3,0x61,0xdb,0xb6,0x6d,0xdb,0xb6,0xfd,0xfe,0x00
    };
    const unsigned char sync[] = {0x4a,0x4c,0x4a,0x06,0x00,0x00,0x00,0xff,0xff};
    unsigned char mixed[] = {0x72,0x04,0x04,0x01,0x00,0xfe,0xff,0x5a};
    unsigned char empty[] = {0x03,0x00};
    const unsigned char invalid_tree[] = {0x05,0x00,0x00,0x00};
    unsigned char pattern[4200], out[2];
    for (size_t i = 0; i < sizeof pattern; ++i) pattern[i] = (unsigned char)"ABCDEF"[i % 6];
    CHECK(!check_stream(fixed, sizeof fixed, literals, sizeof literals));
    CHECK(!check_stream(dynamic, sizeof dynamic, pattern, sizeof pattern));
    CHECK(!check_stream(sync, sizeof sync, (const unsigned char *)"abc", 3));
    /* A non-final fixed block ends mid-byte, immediately before a final stored block. */
    CHECK(!check_stream(mixed, sizeof mixed, (const unsigned char *)"AZ", 2));
    mixed[2] |= 0x20;
    CHECK(!sh_inflate_raw_exact(mixed, sizeof mixed, out, 2));
    CHECK(sh_inflate_raw_exact(empty, sizeof empty, out, 0));
    empty[1] |= 0x80;
    CHECK(!sh_inflate_raw_exact(empty, sizeof empty, out, 0));
    CHECK(!sh_inflate_raw_exact(invalid_tree, sizeof invalid_tree, out, 0));
    puts("raw_deflate_test: all checks passed");
    return 0;
}
