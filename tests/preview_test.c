/* preview_test.c -- generation-safe preview transport and PNG payload contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/preview.h"
#include "../src/common/snapmap_plus_iface.h"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg) { (void)msg; }

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *decode_b64(const char *src, size_t *out_len)
{
    size_t n = strlen(src);
    unsigned char *out = (unsigned char *)malloc(n / 4u * 3u + 1u);
    size_t o = 0;
    if (!out || (n & 3u)) { free(out); return NULL; }
    for (size_t i = 0; i < n; i += 4) {
        int a = b64_value((unsigned char)src[i]);
        int b = b64_value((unsigned char)src[i + 1]);
        int c = src[i + 2] == '=' ? 0 : b64_value((unsigned char)src[i + 2]);
        int d = src[i + 3] == '=' ? 0 : b64_value((unsigned char)src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) { free(out); return NULL; }
        unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12) |
                     ((unsigned)c << 6) | (unsigned)d;
        out[o++] = (unsigned char)(v >> 16);
        if (src[i + 2] != '=') out[o++] = (unsigned char)(v >> 8);
        if (src[i + 3] != '=') out[o++] = (unsigned char)v;
    }
    *out_len = o;
    return out;
}

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static void verify_png(const char *uri, const unsigned char *rgba, unsigned w, unsigned h)
{
    static const char prefix[] = "data:image/png;base64,";
    static const unsigned char signature[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    size_t png_len = 0;
    CHECK(strncmp(uri, prefix, sizeof prefix - 1) == 0);
    unsigned char *png = decode_b64(uri + sizeof prefix - 1, &png_len);
    CHECK(png != NULL);
    if (!png) return;

    CHECK(png_len >= 57);
    if (png_len >= 57) {
        CHECK(memcmp(png, signature, sizeof signature) == 0);
        CHECK(be32(png + 8) == 13);
        CHECK(memcmp(png + 12, "IHDR", 4) == 0);
        CHECK(be32(png + 16) == w);
        CHECK(be32(png + 20) == h);
        CHECK(png[24] == 8 && png[25] == 6);
        CHECK(memcmp(png + 37, "IDAT", 4) == 0);

        size_t raw_len = ((size_t)w * 4u + 1u) * h;
        CHECK(be32(png + 33) == raw_len + 11u);
        CHECK(png[41] == 0x78 && png[42] == 0x01);
        CHECK((png[43] & 7u) == 1u);
        CHECK(((unsigned)png[44] | ((unsigned)png[45] << 8)) == raw_len);
        CHECK(png_len >= 48u + raw_len);
        if (png_len >= 48u + raw_len) {
            size_t p = 48;
            for (unsigned y = 0; y < h; ++y) {
                CHECK(png[p++] == 0);
                CHECK(memcmp(png + p, rgba + (size_t)y * w * 4u, (size_t)w * 4u) == 0);
                p += (size_t)w * 4u;
            }
        }
    }
    free(png);
}

int main(void)
{
    char name[512];
    unsigned long first = 0, second = 0, repeated = 0;
    int kind = 99;
    const unsigned char old_pixel[4] = {255, 0, 0, 255};
    const unsigned char pixels[8] = {1, 2, 3, 4, 10, 20, 30, 40};

    CHECK(sh_preview_take_request(name, sizeof name, &first, &kind) == 0);
    CHECK(kind == SH_PREVIEW_KIND_AUTO);
    CHECK(sh_preview_publish(0, pixels, 2, 1) == SH_PREVIEW_FAILED);
    CHECK(sh_preview_publish(1, NULL, 2, 1) == SH_PREVIEW_FAILED);

    sh_preview_request("materials/first");
    CHECK(sh_preview_take_request(name, sizeof name, &first, &kind) == 1);
    CHECK(strcmp(name, "materials/first") == 0);
    CHECK(first != 0);
    CHECK(kind == SH_PREVIEW_KIND_AUTO);

    sh_preview_request_kind("materials/second", SH_ASSET_IMAGE);
    CHECK(sh_preview_take_request(name, sizeof name, &second, &kind) == 1);
    CHECK(strcmp(name, "materials/second") == 0);
    CHECK(second != first);
    CHECK(kind == SH_ASSET_IMAGE);

    CHECK(sh_preview_publish(first, old_pixel, 1, 1) == SH_PREVIEW_STALE);
    {
        char empty[8] = "dirty";
        CHECK(sh_preview_get(empty, sizeof empty) == 0);
        CHECK(empty[0] == '\0');
    }

    CHECK(sh_preview_publish(second, pixels, 2, 1) == SH_PREVIEW_PUBLISHED);
    CHECK(sh_preview_take_request(name, sizeof name, &repeated, &kind) == 0);
    CHECK(kind == SH_PREVIEW_KIND_AUTO);
    {
        char tiny[8];
        int needed = sh_preview_get(tiny, sizeof tiny);
        CHECK(needed < 0);
        if (needed < 0) {
            size_t cap = (size_t)(-needed);
            char *uri = (char *)malloc(cap);
            CHECK(uri != NULL);
            if (uri) {
                int got = sh_preview_get(uri, cap);
                CHECK(got == (int)strlen(uri));
                verify_png(uri, pixels, 2, 1);
                free(uri);
                CHECK(sh_preview_get(tiny, sizeof tiny) == 0);
                CHECK(sh_preview_take_request(name, sizeof name, &repeated, &kind) == 0);
                CHECK(kind == SH_PREVIEW_KIND_AUTO);
            }
        }
    }

    sh_preview_request("materials/second");
    CHECK(sh_preview_take_request(name, sizeof name, &repeated, &kind) == 1);
    CHECK(repeated != second);
    CHECK(strcmp(name, "materials/second") == 0);
    CHECK(kind == SH_PREVIEW_KIND_AUTO);
    CHECK(sh_preview_publish(second, old_pixel, 1, 1) == SH_PREVIEW_STALE);
    CHECK(sh_preview_publish(repeated, old_pixel, 1, 1) == SH_PREVIEW_PUBLISHED);
    sh_preview_cancel();
    {
        char empty[8] = "dirty";
        CHECK(sh_preview_get(empty, sizeof empty) == 0);
        CHECK(empty[0] == '\0');
        CHECK(sh_preview_take_request(name, sizeof name, &second, &kind) == 0);
        CHECK(kind == SH_PREVIEW_KIND_AUTO);
        CHECK(sh_preview_publish(repeated, old_pixel, 1, 1) == SH_PREVIEW_STALE);
    }

    /* A request with no picture ends with a plain reason instead of staying pending. */
    {
        char why[SH_PREVIEW_REASON_CAP];
        unsigned long failed = 0, next = 0;
        sh_preview_request("materials/no_image");
        CHECK(sh_preview_take_request(name, sizeof name, &failed, &kind) == 1);
        CHECK(sh_preview_failure(why, sizeof why) == 0);
        CHECK(sh_preview_fail(failed - 1, "stale") == SH_PREVIEW_STALE);
        CHECK(sh_preview_failure(why, sizeof why) == 0);
        CHECK(sh_preview_fail(failed, "This material has no image to preview.") == SH_PREVIEW_FAILED);
        CHECK(sh_preview_take_request(name, sizeof name, &next, &kind) == 0);
        CHECK(sh_preview_failure(why, sizeof why) == 1);
        CHECK(strcmp(why, "This material has no image to preview.") == 0);
        CHECK(sh_preview_get(why, sizeof why) == 0);
        CHECK(sh_preview_publish(failed, old_pixel, 1, 1) == SH_PREVIEW_PUBLISHED);
        CHECK(sh_preview_failure(why, sizeof why) == 0);   /* a picture after all replaces the reason */
        sh_preview_request("materials/next");
        CHECK(sh_preview_failure(why, sizeof why) == 0);
        CHECK(sh_preview_take_request(name, sizeof name, &next, &kind) == 1);
        CHECK(sh_preview_fail(next, NULL) == SH_PREVIEW_FAILED);
        CHECK(sh_preview_failure(why, sizeof why) == 1 && why[0] != '\0');
        sh_preview_cancel();
        CHECK(sh_preview_failure(why, sizeof why) == 0);
    }

    if (failures) {
        fprintf(stderr, "%d preview test(s) failed\n", failures);
        return 1;
    }
    puts("preview transport tests passed");
    return 0;
}
