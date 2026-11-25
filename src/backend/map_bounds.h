/* Per-map "Unlock Map Bounds": widens the space modules may occupy beyond native +/-16000
 * (snapEdit_environmentModuleBounds). Every client recomputes validity from its own cvar, so such a map needs
 * Snapmap+ on every player's PC. */
#ifndef SH_MAP_BOUNDS_H
#define SH_MAP_BOUNDS_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_BOUNDS_NATIVE        16000.0f
/* Assembled collision (BCM) and navigation (AAS) store world coordinates as
 * signed shorts. The largest navigation outset is 96 units beyond a module,
 * so modules must stay inside 32767 - 96. Same value as grid_world_limit(). */
#define SH_BOUNDS_CEILING       32671.0f
/* Native snapEdit_minBlueprintFloor / snapEdit_maxBlueprintFloor magnitude. */
#define SH_BOUNDS_NATIVE_FLOORS 25

/* A map selects the native world box (0) or SH_BOUNDS_CEILING. */
typedef struct sh_map_bounds { float limit; } sh_map_bounds;

void  sh_map_bounds_default(sh_map_bounds *settings);
int   sh_map_bounds_valid(const sh_map_bounds *settings);
#define SH_BOUNDS_UNLOCKED_TEXT "1;32671"
int   sh_map_bounds_decode(const char *text, sh_map_bounds *settings);
int   sh_map_bounds_encode(const sh_map_bounds *settings, char *out, size_t cap);
/* The world limit in units this setting selects (native when absent). */
float sh_map_bounds_effective(const sh_map_bounds *settings);
int   sh_map_bounds_overrides(const sh_map_bounds *settings);
void  sh_map_bounds_from_toggle(int on, sh_map_bounds *settings);
/* Blueprint floor range that keeps the native proportion of floors to space. */
int   sh_map_bounds_floors(float limit);

/* Engine adapters run on the engine thread, like the map_render ones. */
int  sh_map_bounds_install(const sig_result *results, size_t count, const uint8_t *base);
/* Entry to native edit-to-build conversion (Play): select the open map's. */
void sh_map_bounds_build(void *map);
/* Selects the setting of a map just deserialized; NULL (before every load) restores the native limit. */
void sh_map_bounds_loaded(void *snapshot);
/* Applies a limit to the engine cvars; NULL or native restores the values captured before the first override. */
void sh_map_bounds_select(const sh_map_bounds *settings);
/* Add the "Unlock Map Bounds" on/off row to the Snapmap+ settings tab. Call
 * after sh_settings_tab_install. */
int  sh_map_bounds_settings_register(void);
#endif
