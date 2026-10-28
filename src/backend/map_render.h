#ifndef SH_MAP_RENDER_H
#define SH_MAP_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_RENDER_VARIABLE "smp.render.v1"
#define SH_RENDER_FIELDS 1
#define SH_RENDER_DISTANCE 0
#define SH_RENDER_NATIVE_DISTANCE 8192.0f
#define SH_RENDER_MIN_DISTANCE 256.0f
#define SH_RENDER_MAX_DISTANCE 200000.0f
/* Where the controls appear in native Settings / Properties. Kept here so
 * moving them to another category only changes these two definitions. */
#define SH_RENDER_SETTINGS_CATEGORY 0
#define SH_RENDER_SETTINGS_TITLE "Map Rendering"
typedef struct sh_map_render { float value[SH_RENDER_FIELDS]; } sh_map_render;
/* View distance in world units. 8192 keeps the native environment's
 * distance (it is also the Default environment's own value); zero is the
 * maximum, SH_RENDER_MAX_DISTANCE; any other value is this map's authority
 * over the far clip. Zero is not an infinite far plane: the environment blend
 * raises its read to at least 256 before writing r_zfar, and r_zfar itself
 * accepts 0..200000 (editor-fixes-13954591 R-021). Version 4 text is kept, so
 * a map saved with 0 under the former "follow the environment" meaning now
 * renders at the maximum. */
void sh_map_render_default(sh_map_render *settings);
int sh_map_render_distance_override(const sh_map_render *settings);
/* The far clip the map asks for: zero becomes SH_RENDER_MAX_DISTANCE. */
float sh_map_render_distance(const sh_map_render *settings);
/* The Map View Distance row is created with the native wrap, which moves a
 * step past either limit to the other limit. Given the value shown before the
 * step and the widget's new value, keep only the wrap from the minimum (zero,
 * the maximum distance) to the maximum: a step down past zero from any other
 * value stops at zero, and a step up past the maximum stays at the maximum. */
float sh_map_render_step(float previous, float value);
int sh_map_render_valid(const sh_map_render *settings);
int sh_map_render_decode(const char *text, sh_map_render *settings);
int sh_map_render_encode(const sh_map_render *settings, char *out, size_t cap);
void sh_map_render_adjust(sh_map_render *settings, unsigned field, float value);

int sh_map_render_install(const sig_result *results, size_t count, const uint8_t *base);
/* Both adapters run on the engine thread. Storage belongs to this native map. */
int sh_map_render_read(void *map, sh_map_render *settings);
int sh_map_render_write(void *map, const sh_map_render *settings);
/* Publish from the build copy entering native SnapMap conversion (Play). */
void sh_map_render_build(void *map);
/* After DeserializeFromJson: select the map's settings and rewrite an older
 * value in place as version 4, so the editable map built from this snapshot
 * saves version 4. NULL clears the selection silently before a load. */
void sh_map_render_loaded(void *map);
/* Retain serialized-map settings without changing the active environment.
 * Select them only when that map reaches its resource activation boundary. */
int sh_map_render_capture(void *snapshot, sh_map_render *settings);
void sh_map_render_select(const sh_map_render *settings);
int sh_map_render_editor_install(const sig_result *results, size_t count);
#endif
