/* modwound.h -- the Graphics SDK's wound sites (docs/graphics-sdk.md, "Wounds").
 *
 * A gorewounds decl whose first key is `type wound` describes one place a demon can be wounded, in plain fields: the
 * damage groups that open it (`parts`), the damage it takes (`opens`, `inonehit`), the mesh kit it shows (`shows`),
 * the spheres it cuts (`cuts`), its blood (`blood` spray, pool, both or none, from the joint `at`), what it throws
 * (`throws`), its kind and whether it opens while the demon lives. It compiles to the game's gorewounds decl over
 * 2016's own default wound (inherit wounds/default), with 2016's blood (the owner, 2026-11-16: blood colour, splatter
 * decals and pools are borrowed): its spray effect and splatter decal, its pool decal, at the game's most used values.
 * A demon's gore graph names its wounds as before. Pure. */
#ifndef SNAPMAP_PLUS_MODWOUND_H
#define SNAPMAP_PLUS_MODWOUND_H

#include <stddef.h>

/* Results as modlook.h. SH_LOOK_SAME when the first key is not `type wound`. */
int sh_wound_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                     size_t why_len);

#endif /* SNAPMAP_PLUS_MODWOUND_H */
