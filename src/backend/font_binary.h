/* Native glyph-file envelope and its name-derived material requests. */
#ifndef SH_FONT_BINARY_H
#define SH_FONT_BINARY_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_font_binary_view {
    uint16_t metrics[4]; /* Uninterpreted native metric bits, including negatives. */
    uint16_t glyph_count;
    const unsigned char *glyphs;  /* Borrowed count * 10 packed bytes. */
    const unsigned char *indices; /* Borrowed count * 4 packed bytes. */
    size_t consumed;
} sh_font_binary_view;

/* Check every read made by the native glyph loader. Trailing bytes are retained
 * by the caller: the native reader does not consume or reject them. This does
 * not certify glyph meaning, material availability or language fallback. Output
 * is borrowed and cleared on failure; NULL output performs validation only. */
int sh_font_binary_read(const unsigned char *body, size_t length,
    sh_font_binary_view *out, char *error, size_t capacity);

typedef enum sh_font_binary_variant {
    SH_FONT_BITMAP_48,
    SH_FONT_DISTANCE_64,
    SH_FONT_BITMAP_12
} sh_font_binary_variant;

typedef struct sh_font_binary_paths {
    char *file;
    char *material;
    char *world_text;
    char *snap_label;
} sh_font_binary_paths;

/* The input is the native manager's already-normalized font name. The loader
 * replaces spaces and hyphens only; it does not use GenerateFilename. Variant
 * selection belongs to the distance-font setting and native file-open result.
 * No package path validation or language substitution is performed here.
 * Output starts zeroed (not freed); success owns all four strings, failure none. */
int sh_font_binary_paths_for(const char *name, sh_font_binary_variant variant,
    sh_font_binary_paths *out);
void sh_font_binary_paths_clear(sh_font_binary_paths *paths);
#endif
