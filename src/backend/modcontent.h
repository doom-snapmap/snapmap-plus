/* modcontent.h -- the build-time check of a mod.json's "scenes" (modscene_decl.h) and "hud" (modhud.h) blocks, so a
 * mistake fails the build and shows as its cause. Registered with the builder (sh_mb_set_extra_check); pure. */
#ifndef SNAPMAP_PLUS_MODCONTENT_H
#define SNAPMAP_PLUS_MODCONTENT_H

#include <stddef.h>

/* `extra` is the build's preserved author keys (sh_mb_bundle.extra, a JSON object), `salt` the mod's. 1 when its
 * "scenes" and "hud" blocks (either may be absent) read as the game reads them; else 0 with one sentence in *cause
 * (malloc'd; the caller frees it) naming the block and what to change. */
int sh_modcontent_check(const char *salt, const char *extra, size_t len, char **cause);

#endif /* SNAPMAP_PLUS_MODCONTENT_H */
