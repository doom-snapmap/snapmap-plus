/* modlook.h -- the Graphics SDK's look compiler (docs/graphics-sdk.md).
 *
 * A material whose first key is `look` (value `layered` or `particle`) is compiled by sh_look_compile into the
 * material text the engine reads (storage in modprog.h): the game's own render program where one draws the picture,
 * else the product's two general programs; a value where an image is expected becomes a registered constant image.
 * A non-look material using the superseded layered or eternalparticle* vectors is read as the look those keys say.
 * A mistake is one sentence naming the file, line and field; what exceeds the game's limits is drawn as a constant
 * and noted. Pure: no engine access.
 */
#ifndef SNAPMAP_PLUS_MODLOOK_H
#define SNAPMAP_PLUS_MODLOOK_H

#include <stddef.h>

#define SH_LOOK_SAME     0     /* not a look and no superseded keys: serve the file as it is */
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
