/* bcn_test.c -- BC1/BC3/BC7 decode vectors, padding, truncation, and size guards. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../src/backend/bcn.h"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void pixel_is(const unsigned char *p, unsigned r, unsigned g, unsigned b, unsigned a)
{
    CHECK(p[0] == r);
    CHECK(p[1] == g);
    CHECK(p[2] == b);
    CHECK(p[3] == a);
}

int main(void)
{
    static const unsigned char bc1[8] = {
        0x00, 0xF8, 0xE0, 0x07, 0xE4, 0xE4, 0xE4, 0xE4
    };
    static const unsigned char bc1_punch[8] = {
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const unsigned char bc3[16] = {
        0xFF, 0x00, 0, 0, 0, 0, 0, 0,
        0x00, 0xF8, 0xE0, 0x07, 0, 0, 0, 0
    };
    /* BC7 mode 6, identical near-red endpoints, all indices zero. The shared endpoint p-bit
     * makes zero-valued green and blue channels expand to 1. */
    static const unsigned char bc7_red[16] = {
        0xC0, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0xFE, 0xFF,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char bc7_invalid[16] = {0};
    unsigned char out[64];

    CHECK(BCN_PAD(1) == 4);
    CHECK(BCN_PAD(4) == 4);
    CHECK(BCN_PAD(5) == 8);
    CHECK(BCN_PAD(UINT_MAX) == 0);
    CHECK(bcn_rgba_size(1, 1) == sizeof out);
    CHECK(bcn_rgba_size(0, 1) == 0);
    CHECK(bcn_rgba_size(UINT_MAX, 1) == 0);
    CHECK(bcn_rgba_size(UINT_MAX - 3u, UINT_MAX - 3u) == 0);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc1(bc1, sizeof bc1, 4, 4, out) == 1);
    pixel_is(out + 0 * 4, 255, 0, 0, 255);
    pixel_is(out + 1 * 4, 0, 255, 0, 255);
    pixel_is(out + 2 * 4, 170, 85, 0, 255);
    pixel_is(out + 3 * 4, 85, 170, 0, 255);
    pixel_is(out + 15 * 4, 85, 170, 0, 255);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc1(bc1_punch, sizeof bc1_punch, 1, 1, out) == 1);
    for (int i = 0; i < 16; ++i) pixel_is(out + i * 4, 0, 0, 0, 0);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc1(bc1, 4, 1, 1, out) == 1);
    for (int i = 0; i < 16; ++i) pixel_is(out + i * 4, 255, 0, 0, 255);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc3(bc3, sizeof bc3, 4, 4, out) == 1);
    pixel_is(out, 255, 0, 0, 255);
    pixel_is(out + 15 * 4, 255, 0, 0, 255);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc7(bc7_red, sizeof bc7_red, 4, 4, out) == 1);
    for (int i = 0; i < 16; ++i) pixel_is(out + i * 4, 255, 1, 1, 255);

    memset(out, 0xCD, sizeof out);
    CHECK(bcn_decode_bc7(bc7_invalid, sizeof bc7_invalid, 4, 4, out) == 1);
    for (int i = 0; i < 16; ++i) pixel_is(out + i * 4, 0, 0, 0, 0);

    CHECK(bcn_decode(10, bc1, sizeof bc1, 4, 4, out) == 1);
    CHECK(bcn_decode(11, bc3, sizeof bc3, 4, 4, out) == 1);
    CHECK(bcn_decode(23, bc7_red, sizeof bc7_red, 4, 4, out) == 1);
    CHECK(bcn_decode(99, bc1, sizeof bc1, 4, 4, out) == 0);
    CHECK(bcn_decode_bc1(NULL, 0, 4, 4, out) == 0);
    CHECK(bcn_decode_bc1(bc1, sizeof bc1, 4, 4, NULL) == 0);
    CHECK(bcn_decode_bc1(bc1, sizeof bc1, UINT_MAX, 1, out) == 0);

    if (failures) {
        fprintf(stderr, "%d BCn test(s) failed\n", failures);
        return 1;
    }
    puts("BCn decoder tests passed");
    return 0;
}
