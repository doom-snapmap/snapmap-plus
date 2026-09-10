/* Capture a final editor navigation snapshot before Play converts the map.
 *
 * The SnapMapEditToSnapBuild entry runs on DOOM's main thread while the
 * editor still owns its entities. Reading them later inside BuildAAS is
 * unsafe because conversion can leave defsub null. Private resource names let
 * repeated module instances receive distinct temporary AAS payloads.
 */
#ifndef SNAPMAP_PLUS_NAV_PLAY_H
#define SNAPMAP_PLUS_NAV_PLAY_H
#include "signatures.h"

/* Install the pre-build snapshot detour. Refuse when status_ok is false:
 * hook-tolerant resolution may point at another detour's prologue. Returns 1
 * when installed.
 */
int sh_nav_play_install(void *snapbuild_fn, int status_ok);
int sh_nav_play_install_instances(const sig_result *results, size_t count);
int sh_nav_play_install_volume_contents(const sig_result *results, size_t count);

#endif /* SNAPMAP_PLUS_NAV_PLAY_H */
