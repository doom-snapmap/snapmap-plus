/* modwound.h -- the Graphics SDK's wound sites (docs/graphics-sdk.md, "Wounds"). A gorewounds decl whose first
 * key is `type wound` describes one place a demon can be wounded in plain fields (`parts`, `opens`, `inonehit`,
 * `shows`, `cuts`, `blood`, `at`, `throws`) and compiles to the game's gorewounds decl over wounds/default,
 * borrowing 2016's blood spray, splatter decal and pool decal at the game's most used values. Pure. */
#ifndef SNAPMAP_PLUS_MODWOUND_H
#define SNAPMAP_PLUS_MODWOUND_H

#include <stddef.h>

/* Results as modlook.h. SH_LOOK_SAME when the first key is not `type wound`. */
int sh_wound_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                     size_t why_len);

#endif /* SNAPMAP_PLUS_MODWOUND_H */
