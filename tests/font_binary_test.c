#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_binary.h"

static void header(unsigned char *body, unsigned count)
{
    static const unsigned char prefix[12] = {'i', 'd', 'f', '+',
        0, 64, 0, 53, 0xff, 0xf0, 0, 8};
    memcpy(body, prefix, sizeof(prefix));
    body[12] = (unsigned char)(count >> 8); body[13] = (unsigned char)count;
}
static void envelope(void)
{
    unsigned char body[57], original[57];
    sh_font_binary_view out, empty = {0};
    char error[160];
    header(body, 3);
    for (size_t i = 14; i < sizeof(body); i++) body[i] = (unsigned char)(i * 31);
    memcpy(original, body, sizeof(body));
    assert(sh_font_binary_read(body, 56, &out, error, sizeof(error)) && !*error);
    assert(out.metrics[0] == 64 && out.metrics[1] == 53 &&
        out.metrics[2] == 0xfff0 && out.metrics[3] == 8);
    assert(out.glyph_count == 3 && out.glyphs == body + 14 && out.indices == body + 44);
    assert(out.consumed == 56 && !memcmp(body, original, sizeof(body)));
    for (size_t n = 0; n < 56; n++) {
        memset(&out, 0xa5, sizeof(out));
        assert(!sh_font_binary_read(body, n, &out, error, sizeof(error)) && *error);
        assert(!memcmp(&out, &empty, sizeof(out)));
    }
    /* The engine ignores unread trailing bytes. They are still source data. */
    assert(sh_font_binary_read(body, sizeof(body), &out, error, sizeof(error)) && out.consumed == 56);
    assert(!memcmp(body, original, sizeof(body)));
    assert(sh_font_binary_read(body, 56, NULL, NULL, 0));
    body[3] ^= 1;
    assert(!sh_font_binary_read(body, sizeof(body), &out, error, sizeof(error)));
    assert(!memcmp(&out, &empty, sizeof(out)));
    /* A prior format remains in shipped catalogs. Current native loaders
     * reject its magic before trying to interpret the remaining records. */
    body[3] = '*';
    assert(!sh_font_binary_read(body, sizeof(body), &out, error, sizeof(error)));
    assert(strstr(error, "unsupported glyph envelope"));
    header(body, 0);
    assert(sh_font_binary_read(body, 14, &out, error, sizeof(error)));
    assert(!out.glyph_count && out.consumed == 14 && out.glyphs == body + 14 && out.indices == body + 14);
    header(body, 0x8000);
    assert(!sh_font_binary_read(body, sizeof(body), &out, error, sizeof(error)));
    assert(strstr(error, "negative"));
    header(body, 0xffff);
    assert(!sh_font_binary_read(body, sizeof(body), NULL, NULL, 0));
    assert(!sh_font_binary_read(NULL, sizeof(body), &out, error, sizeof(error)));
    assert(!memcmp(&out, &empty, sizeof(out)));
    assert(!sh_font_binary_read(NULL, 0, NULL, error, 1) && !*error);
    size_t length = 14 + (size_t)INT16_MAX * 14;
    unsigned char *large = calloc(length, 1);
    assert(large); header(large, INT16_MAX);
    assert(sh_font_binary_read(large, length, &out, error, sizeof(error)));
    assert(out.glyph_count == INT16_MAX && out.consumed == length);
    assert(!sh_font_binary_read(large, length - 1, &out, error, sizeof(error)));
    free(large);
}
static void paths(void)
{
    sh_font_binary_paths p, empty = {0};
    assert(sh_font_binary_paths_for("example font-bold", SH_FONT_DISTANCE_64, &p));
    assert(!strcmp(p.file, "fonts/example_font_bold/64_df.dat"));
    assert(!strcmp(p.material, "fonts/example_font_bold/64_df.tga"));
    assert(!strcmp(p.world_text, "snapmap/example_font_bold_worldText"));
    assert(!strcmp(p.snap_label, "snapmap/example_font_bold_snapLabel"));
    sh_font_binary_paths_clear(&p); assert(!memcmp(&p, &empty, sizeof(p)));
    assert(sh_font_binary_paths_for("smp/font", SH_FONT_BITMAP_48, &p));
    assert(!strcmp(p.file, "fonts/smp/font/48.dat"));
    assert(!strcmp(p.material, "fonts/smp/font/48.tga"));
    sh_font_binary_paths_clear(&p);
    assert(sh_font_binary_paths_for("Font.With.Dots", SH_FONT_BITMAP_12, &p));
    assert(!strcmp(p.file, "fonts/Font.With.Dots/12.dat"));
    assert(!strcmp(p.material, "fonts/Font.With.Dots/12.tga"));
    sh_font_binary_paths_clear(&p);
    /* Unlike generated assets, long font stems are not partitioned. */
    char long_name[4097]; memset(long_name, 'a', sizeof(long_name) - 1); long_name[4096] = 0;
    assert(sh_font_binary_paths_for(long_name, SH_FONT_DISTANCE_64, &p));
    assert(strlen(p.file) == 6 + 4096 + 10 && !strncmp(p.file + 6, long_name, 4096));
    assert(!strcmp(p.file + 6 + 4096, "/64_df.dat"));
    sh_font_binary_paths_clear(&p);
    memset(&p, 0xa5, sizeof(p));
    assert(!sh_font_binary_paths_for("x", (sh_font_binary_variant)-1, &p));
    assert(!memcmp(&p, &empty, sizeof(p)));
    assert(!sh_font_binary_paths_for("x", (sh_font_binary_variant)3, &p));
    assert(!sh_font_binary_paths_for("", SH_FONT_BITMAP_48, &p));
    assert(!sh_font_binary_paths_for(NULL, SH_FONT_BITMAP_48, &p));
    assert(!sh_font_binary_paths_for("x", SH_FONT_BITMAP_48, NULL));
    sh_font_binary_paths_clear(&p); sh_font_binary_paths_clear(NULL);
}
int main(void)
{
    envelope(); paths();
    puts("font_binary_test: PASS");
    return 0;
}
