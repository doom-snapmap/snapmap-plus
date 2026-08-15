/* imgpreview_index_test.c -- strict, transactional parsing of resource index records. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/imgpreview.c"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg) { (void)msg; }
const char *sh_megapreview_name_at(int index) { (void)index; return NULL; }
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h)
{ (void)generation; (void)rgba; (void)w; (void)h; return SH_PREVIEW_PUBLISHED; }

static void put_le32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put_be32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void put_be64(unsigned char *p, unsigned long long v)
{
    for (int i = 7; i >= 0; --i) { p[i] = (unsigned char)v; v >>= 8; }
}

static void put_field(unsigned char *buf, size_t *o, const char *value)
{
    unsigned n = (unsigned)strlen(value);
    put_le32(buf + *o, n); *o += 4;
    memcpy(buf + *o, value, n); *o += n;
}

static void put_record(unsigned char *buf, size_t *o, const char *type, const char *name,
                       unsigned long long offset, unsigned usz, unsigned csz)
{
    put_field(buf, o, type);
    put_field(buf, o, name);
    put_field(buf, o, "payload/path");
    memset(buf + *o, 0, 25);
    put_be64(buf + *o, offset);
    put_be32(buf + *o + 8, usz);
    put_be32(buf + *o + 12, csz);
    *o += 25;
}

static void put_terminal_record(unsigned char *buf, size_t *o, const char *type, const char *name,
                                unsigned long long offset, unsigned usz, unsigned csz)
{
    put_field(buf, o, type);
    put_field(buf, o, name);
    put_field(buf, o, "payload/path");
    memset(buf + *o, 0, 21);
    put_be64(buf + *o, offset);
    put_be32(buf + *o + 8, usz);
    put_be32(buf + *o + 12, csz);
    *o += 21;
}

static size_t build_valid(unsigned char *buf, size_t cap)
{
    memset(buf, 0, cap);
    buf[0] = 0x05; memcpy(buf + 1, "SER", 3);
    put_be32(buf + 0x20, 4);
    size_t o = 0x28;
    put_record(buf, &o, "model", "models/props/crate.lwo", 0x0102030405060708ull, 400, 200);
    put_record(buf, &o, "model", "maps/test/room.bmodel", 9, 10, 11);
    put_record(buf, &o, "file", "generated/swf/ui/panel.bswf", 12, 13, 14);
    /* Shipped indexes omit the final four inter-record bytes, so the last block is 21 bytes. */
    put_terminal_record(buf, &o, "material", "materials/final", 15, 16, 17);
    return o;
}

static void reset_records(void)
{
    g_recCount = 0;
}

/* Optional local integration check: pass one or more real index paths to the already-built test
 * executable. The normal test runner supplies none, so CI remains self-contained. */
static void check_index_file(const char *path)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        fprintf(stderr, "FAIL: could not open index fixture %s\n", path);
        failures++;
        return;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "FAIL: could not size index fixture %s\n", path);
        failures++;
        fclose(f);
        return;
    }
    __int64 size = _ftelli64(f);
    if (size <= 0 || (unsigned __int64)size > 0xFFFFFFFFull ||
        _fseeki64(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "FAIL: invalid index fixture size for %s\n", path);
        failures++;
        fclose(f);
        return;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "FAIL: could not read index fixture %s\n", path);
        failures++;
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);

    reset_records();
    if (!imgpreview_parse_box_index(0, buf, (size_t)size) || g_recCount <= 0) {
        fprintf(stderr, "FAIL: rejected index fixture %s\n", path);
        failures++;
    } else {
        printf("index fixture parsed: %s (%d catalog records)\n", path, g_recCount);
    }
    free(buf);
}

int main(int argc, char **argv)
{
    unsigned char original[1024];
    size_t valid_len = build_valid(original, sizeof original);
    unsigned char *valid = (unsigned char *)malloc(valid_len);
    CHECK(valid != NULL);
    if (!valid) return 1;
    memcpy(valid, original, valid_len);

    reset_records();
    CHECK(imgpreview_parse_box_index(0, valid, valid_len) == 1);
    CHECK(g_recCount == 4);
    if (g_recCount == 4) {
        CHECK(g_rec[0].kind == SH_ASSET_MODEL);
        CHECK(strcmp(g_rec[0].name, "models/props/crate.lwo") == 0);
        CHECK(g_rec[0].roff == 0x0102030405060708ull);
        CHECK(g_rec[0].usz == 400 && g_rec[0].csz == 200);
        CHECK(g_rec[1].kind == SH_ASSET_BMODEL);
        CHECK(strcmp(g_rec[1].name, "maps/test/room.bmodel") == 0);
        CHECK(g_rec[2].kind == SH_ASSET_SWF);
        CHECK(strcmp(g_rec[2].name, "swf/ui/panel.swf") == 0);
        CHECK(g_rec[3].kind == SH_ASSET_MATERIAL);
        CHECK(strcmp(g_rec[3].name, "materials/final") == 0);
        CHECK(g_rec[3].roff == 15);
        CHECK(g_rec[3].usz == 16 && g_rec[3].csz == 17);
    }
    free(valid);

    /* Every proper prefix of a declared four-record file must be rejected without retaining the
     * valid records that happened to precede the cut. A fresh copy is required because parsing
     * deliberately inserts NUL terminators in recognized names. */
    for (size_t cut = 0; cut < valid_len; ++cut) {
        unsigned char *truncated = (unsigned char *)malloc(valid_len);
        CHECK(truncated != NULL);
        if (!truncated) break;
        memcpy(truncated, original, valid_len);
        reset_records();
        CHECK(imgpreview_parse_box_index(0, truncated, cut) == 0);
        CHECK(g_recCount == 0);
        free(truncated);
    }

    {
        unsigned char empty[0x28] = {0};
        empty[0] = 0x05; memcpy(empty + 1, "SER", 3);
        reset_records();
        CHECK(imgpreview_parse_box_index(0, empty, sizeof empty) == 1);
        CHECK(g_recCount == 0);
        empty[0] = 0;
        CHECK(imgpreview_parse_box_index(0, empty, sizeof empty) == 0);
    }
    {
        unsigned char malformed[0x2C] = {0};
        malformed[0] = 0x05; memcpy(malformed + 1, "SER", 3);
        put_be32(malformed + 0x20, 1);
        put_le32(malformed + 0x28, 0xFFFFFFFFu);
        reset_records();
        CHECK(imgpreview_parse_box_index(0, malformed, sizeof malformed) == 0);
        CHECK(g_recCount == 0);
        CHECK(imgpreview_parse_box_index(-1, malformed, sizeof malformed) == 0);
        CHECK(imgpreview_parse_box_index(2, malformed, sizeof malformed) == 0);
    }

    for (int i = 1; i < argc; ++i) check_index_file(argv[i]);

    free(g_rec); g_rec = NULL; g_recCount = 0;
    if (failures) {
        fprintf(stderr, "%d index parser test(s) failed\n", failures);
        return 1;
    }
    puts("image index parser tests passed");
    return 0;
}
