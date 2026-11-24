#ifndef SH_MAP_RENDER_H
#define SH_MAP_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_RENDER_NATIVE_DISTANCE 8192.0f
#define SH_RENDER_MIN_DISTANCE 256.0f
#define SH_RENDER_MAX_DISTANCE 200000.0f
/* The only text written: version 4 with distance 0, the maximum distance without fog. */
#define SH_RENDER_UNLOCKED_TEXT "4;0"
/* Where the controls appear in native Settings / Properties. Kept here so
 * moving them to another category only changes these two definitions. */
#define SH_RENDER_SETTINGS_CATEGORY 0
#define SH_RENDER_SETTINGS_TITLE "Map Rendering"
/* "Unlock View Distance", one on/off map setting. Off is fully native and the map carries no render setting;
 * on sets the far clip to SH_RENDER_MAX_DISTANCE (r_zfar's maximum) and stretches every environment's fog by
 * the same factor, in the editor and in Play. */
typedef struct sh_map_render { int unlocked; } sh_map_render;
void sh_map_render_default(sh_map_render *settings);
void sh_map_render_from_toggle(int on, sh_map_render *settings);
int sh_map_render_unlocked(const sh_map_render *settings);
/* The far clip the setting asks for: the maximum when unlocked, else the native default. */
float sh_map_render_distance(const sh_map_render *settings);
/* While unlocked, fog stretches by r = SH_RENDER_MAX_DISTANCE / the blended native maxViewDistance (floored
 * at 256, never below 1): the fog distance parms are multiplied by r and the strength parms divided by r;
 * every other parm keeps its blended value. Names are the engine's lowercase resource names. */
#define SH_RENDER_FOG_DISTANCE_COUNT 3
#define SH_RENDER_FOG_STRENGTH_COUNT 2
extern const char *const sh_map_render_fog_distance_parms[SH_RENDER_FOG_DISTANCE_COUNT];
extern const char *const sh_map_render_fog_strength_parms[SH_RENDER_FOG_STRENGTH_COUNT];
float sh_map_render_fog_stretch(float native_distance);
/* A fog literal scaled by r: distances times r, strengths divided by r. */
float sh_map_render_fog_distance(float value, float r);
float sh_map_render_fog_strength(float value, float r);
int sh_map_render_valid(const sh_map_render *settings);
/* Every stored version reads as on or off: versions 1 and 3 (distance and six fog values), 2 (an enable
 * flag, then the same) and 4 (distance only). Any valid distance other than the native 8192 (0 included)
 * reads as on; 8192 and a disabled version 2 read as off. */
int sh_map_render_decode(const char *text, sh_map_render *settings);
/* On encodes as SH_RENDER_UNLOCKED_TEXT. Off has no text: it is stored as the
 * variable's absence, so encoding it fails. */
int sh_map_render_encode(const sh_map_render *settings, char *out, size_t cap);
/* Whether a string variable named name is one of Snapmap+'s own map settings
 * ("smp." prefix). No logic node binds to them. */
int sh_map_render_own_variable(const char *name, int length);

int sh_map_render_install(const sig_result *results, size_t count, const uint8_t *base);
/* The RenderClipRead site is an E8 rel32 call: its five bytes in `bytes`, its callee in *callee; 0 when it is not
 * that call. sh_map_render_install runs this same decode. */
int sh_map_render_decode_read(uintptr_t site, unsigned char bytes[5], uintptr_t *callee);
/* The open map's setting, held by map_settings.c (read from the map's row at
 * every parse, written back at every save; the engine's variable lists never
 * carry it). map names the map the caller means and must not be NULL. */
int sh_map_render_read(void *map, sh_map_render *settings);
/* Set the open map's setting; its next save stores SH_RENDER_UNLOCKED_TEXT
 * when on and no row when off. */
int sh_map_render_write(void *map, const sh_map_render *settings);
/* Publish the open map's setting for the build copy entering native SnapMap
 * conversion (Play). */
void sh_map_render_build(void *map);
/* After a successful DeserializeFromJson: select the parsed map's setting.
 * NULL clears the selection silently before a load. */
void sh_map_render_loaded(void *map);
void sh_map_render_select(const sh_map_render *settings);
int sh_map_render_editor_install(const sig_result *results, size_t count);
#endif
