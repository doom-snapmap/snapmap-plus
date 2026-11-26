/* Capture a final editor navigation snapshot before Play converts the map. The SnapMapEditToSnapBuild entry runs on
 * DOOM's main thread while the editor owns its entities; inside BuildAAS conversion can leave defsub null. */
#ifndef SNAPMAP_PLUS_NAV_PLAY_H
#define SNAPMAP_PLUS_NAV_PLAY_H
#include "signatures.h"

/* Install the pre-build snapshot detour at its verified signature. */
int sh_nav_play_install(void *snapbuild_fn);
typedef const char *(*sh_play_actor_text_fn)(const char *inherit, const char *cls, const char *source, char **owned);
int sh_nav_play_install_actor_source(const sig_result *results, size_t count, sh_play_actor_text_fn project);
int sh_nav_play_install_instances(const sig_result *results, size_t count);
int sh_nav_play_install_volume_contents(const sig_result *results, size_t count);
/* Serve the bake's cut placement-grid models to the editor's surface rebuild. */
int sh_nav_play_install_overlay(const sig_result *results, size_t count);

#endif /* SNAPMAP_PLUS_NAV_PLAY_H */
