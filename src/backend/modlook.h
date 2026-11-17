/* modlook.h -- the Graphics SDK's look compiler (docs/graphics-sdk.md).
 *
 * A mod writes how a surface or a particle stage looks in plain terms: a material whose first key is `look`, with
 * the value `layered` or `particle`. sh_look_compile turns it into the material text the engine reads, in the
 * product's storage (modprog.h): the render program, the texture keys, the layered program's parameter image, the
 * particle program's stock keys. Where one of the game's own render programs draws the picture, the game's program is
 * named; the product's two general programs only where the look needs them. A value where an image is expected
 * (a colour, a number) becomes a registered constant image (sh_modprog_register_const_image). Pure: no engine
 * access; the images it registers are served from modprog.c.
 *
 * A material that is not a look is left alone, except that one that still names the old program names or writes the
 * old keys (the 28 layered vectors, the eternalparticle* vectors) is rewritten the way sh_modprog_legacy_material
 * does, its program names the new ones, so a mod emitted before the numbers moved draws unchanged.
 *
 * A mistake in a look is one sentence naming the file's base name, the line, the field and what to write instead.
 * Nothing about the game's limits is ever a mistake: what does not fit is drawn as a constant and noted. */
#ifndef SNAPMAP_PLUS_MODLOOK_H
#define SNAPMAP_PLUS_MODLOOK_H

#include <stddef.h>

#define SH_LOOK_SAME     0     /* not a look and no old keys: serve the file as it is */
#define SH_LOOK_CHANGED  1     /* *out holds the engine's material text (malloc'd, NUL-terminated, *out_len bytes) */
#define SH_LOOK_MISTAKE  2     /* `why` holds the sentence */
#define SH_LOOK_OOM    (-1)

/* Compiles one material decl. `path` is the file's path (only its base name is used, in sentences). `why` (when
 * why_len > 0) is set to "" unless the result is SH_LOOK_MISTAKE. */
int sh_look_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                    size_t why_len);

/* The same, and when `notes` is not NULL, *notes is NULL or a malloc'd text of one line per thing drawn differently
 * from what the look asked because the game's shared texture places are full (never a mistake; for the build log). */
int sh_look_compile_notes(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                          size_t why_len, char **notes);

#endif /* SNAPMAP_PLUS_MODLOOK_H */
