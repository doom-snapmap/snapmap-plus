/* modcontent.h -- the build-time check of a mod's content blocks.
 *
 * mod.json's author part may carry "scenes" (modscene_decl.h) and "hud"
 * (modhud.h). Their readers run at Play and in the HUD install; this check
 * runs the same grammar when the mod is built, so a mistake fails the build
 * and the Mods tab shows it as the build's cause instead of the backend log
 * alone. The product registers it with the builder (sh_mb_set_extra_check,
 * modbundle.h). Pure: no engine, no global state.
 */
#ifndef SNAPMAP_PLUS_MODCONTENT_H
#define SNAPMAP_PLUS_MODCONTENT_H

#include <stddef.h>

/* `extra` is the build's preserved author keys (sh_mb_bundle.extra, a JSON
 * object), `salt` the mod's. 1 when its "scenes" and "hud" blocks (either may
 * be absent) read as the game reads them; else 0 with one plain sentence in
 * *cause (malloc'd; the caller frees it) naming the block in mod.json and
 * what to change. */
int sh_modcontent_check(const char *salt, const char *extra, size_t len, char **cause);

#endif /* SNAPMAP_PLUS_MODCONTENT_H */
