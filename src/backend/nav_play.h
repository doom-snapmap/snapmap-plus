/* Capture a final editor navigation snapshot before Play converts the map. The SnapMapEditToSnapBuild entry runs on
 * DOOM's main thread while the editor owns its entities; inside BuildAAS conversion can leave defsub null. */
#ifndef SNAPMAP_PLUS_NAV_PLAY_H
#define SNAPMAP_PLUS_NAV_PLAY_H
#include "signatures.h"

/* Install the pre-build snapshot detour. Refuses when status_ok is false, since hook-tolerant resolution may point
 * at another detour's prologue. Returns 1 when installed. */
int sh_nav_play_install(void *snapbuild_fn, int status_ok);
int sh_nav_play_install_instances(const sig_result *results, size_t count);
int sh_nav_play_install_volume_contents(const sig_result *results, size_t count);
/* Serve the bake's cut placement-grid models to the editor's surface rebuild. */
int sh_nav_play_install_overlay(const sig_result *results, size_t count);

#endif /* SNAPMAP_PLUS_NAV_PLAY_H */
