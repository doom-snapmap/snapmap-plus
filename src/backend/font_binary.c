#include "font_binary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fb_fail(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "Font glyph data: %s", reason);
    return 0;
}
static uint16_t fb_word(const unsigned char *p)
{ return (uint16_t)((unsigned)p[0] << 8 | p[1]); }

int sh_font_binary_read(const unsigned char *body, size_t length,
    sh_font_binary_view *out, char *error, size_t capacity)
{
    sh_font_binary_view view = {0};
    if (out) memset(out, 0, sizeof(*out));
    if (error && capacity) *error = 0;
    if (!body) return fb_fail(error, capacity, "missing payload");
    if (length < 14) return fb_fail(error, capacity, "truncated header");
    if (memcmp(body, "idf+", 4)) return fb_fail(error, capacity, "unsupported glyph envelope");
    view.glyph_count = fb_word(body + 12);
    if (view.glyph_count > INT16_MAX) return fb_fail(error, capacity, "negative glyph count");
    if (view.glyph_count > (length - 14) / 14)
        return fb_fail(error, capacity, "truncated glyph or index records");
    for (size_t i = 0; i < 4; i++) view.metrics[i] = fb_word(body + 4 + i * 2);
    view.glyphs = body + 14;
    view.indices = view.glyphs + (size_t)view.glyph_count * 10;
    view.consumed = 14 + (size_t)view.glyph_count * 14;
    if (out) *out = view;
    return 1;
}

void sh_font_binary_paths_clear(sh_font_binary_paths *paths)
{
    if (!paths) return;
    free(paths->file); free(paths->material);
    free(paths->world_text); free(paths->snap_label);
    memset(paths, 0, sizeof(*paths));
}
static char *fb_path(const char *prefix, const char *stem, const char *suffix)
{
    size_t a = strlen(prefix), b = strlen(stem), c = strlen(suffix);
    if (a > SIZE_MAX - b || a + b > SIZE_MAX - c - 1) return NULL;
    char *out = malloc(a + b + c + 1);
    if (!out) return NULL;
    memcpy(out, prefix, a); memcpy(out + a, stem, b);
    memcpy(out + a + b, suffix, c + 1);
    return out;
}
int sh_font_binary_paths_for(const char *name, sh_font_binary_variant variant,
    sh_font_binary_paths *out)
{
    static const char *files[] = {"/48.dat", "/64_df.dat", "/12.dat"};
    static const char *materials[] = {"/48.tga", "/64_df.tga", "/12.tga"};
    char *stem;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!name || !*name || (unsigned)variant >= sizeof(files) / sizeof(files[0])) return 0;
    size_t length = strlen(name);
    if (length == SIZE_MAX || !(stem = malloc(length + 1))) return 0;
    for (size_t i = 0; i <= length; i++)
        stem[i] = name[i] == ' ' || name[i] == '-' ? '_' : name[i];
    out->file = fb_path("fonts/", stem, files[variant]);
    out->material = fb_path("fonts/", stem, materials[variant]);
    out->world_text = fb_path("snapmap/", stem, "_worldText");
    out->snap_label = fb_path("snapmap/", stem, "_snapLabel");
    free(stem);
    if (out->file && out->material && out->world_text && out->snap_label) return 1;
    sh_font_binary_paths_clear(out); return 0;
}
