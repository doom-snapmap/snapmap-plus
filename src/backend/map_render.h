#ifndef SH_MAP_RENDER_H
#define SH_MAP_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_RENDER_VARIABLE "smp.render.v1"
#define SH_RENDER_NATIVE_DISTANCE 8192.0f
#define SH_RENDER_MIN_DISTANCE 256.0f
#define SH_RENDER_MAX_DISTANCE 200000.0f
/* The only text this build writes: version 4 with distance 0, which earlier
 * Snapmap+ builds already read as the maximum distance without fog. */
#define SH_RENDER_UNLOCKED_TEXT "4;0"
/* Where the controls appear in native Settings / Properties. Kept here so
 * moving them to another category only changes these two definitions. */
#define SH_RENDER_SETTINGS_CATEGORY 0
#define SH_RENDER_SETTINGS_TITLE "Map Rendering"
/* "Unlock View Distance", one on/off map setting. Off is fully native: every
 * environment keeps its own far clip and fog, and the map carries no
 * Snapmap+ variable at all. On sets the far clip to SH_RENDER_MAX_DISTANCE
 * (r_zfar's registered maximum) and stretches every environment's fog by the
 * same factor, in the editor and in Play, whatever any Environment FX or Set
 * Environment node asks for (editor-fixes-13954591 R-024). */
typedef struct sh_map_render { int unlocked; } sh_map_render;
void sh_map_render_default(sh_map_render *settings);
void sh_map_render_from_toggle(int on, sh_map_render *settings);
int sh_map_render_unlocked(const sh_map_render *settings);
/* The far clip the setting asks for: the maximum when unlocked, else the
 * native default (which the native blend then replaces with its own). */
float sh_map_render_distance(const sh_map_render *settings);
/* While unlocked every environment keeps its fog, stretched by the same
 * factor as its far clip: r = SH_RENDER_MAX_DISTANCE / the blended native
 * maxViewDistance (floored at 256, as the engine floors its read), never
 * below 1. The fog distance parms are multiplied by r and the fog strength
 * parms divided by r, so the fog reaches at distance d * r what it reached at
 * d and saturates r times farther out; colour, maximum opacity, height and
 * every other parm keep their blended values. Names are the engine's
 * lowercase resource names (editor-fixes-13954591 R-023, R-024). */
#define SH_RENDER_FOG_DISTANCE_COUNT 3
#define SH_RENDER_FOG_STRENGTH_COUNT 2
extern const char *const sh_map_render_fog_distance_parms[SH_RENDER_FOG_DISTANCE_COUNT];
extern const char *const sh_map_render_fog_strength_parms[SH_RENDER_FOG_STRENGTH_COUNT];
float sh_map_render_fog_stretch(float native_distance);
/* A fog literal scaled by r: distances times r, strengths divided by r. */
float sh_map_render_fog_distance(float value, float r);
float sh_map_render_fog_strength(float value, float r);
int sh_map_render_valid(const sh_map_render *settings);
/* Every stored version reads as on or off. Versions 1 and 3 (distance and
 * six fog values), 2 (an enable flag, then the same) and 4 (distance only)
 * were written by earlier Snapmap+ builds when the row was a number: any
 * valid distance other than the native 8192 (0 included) reads as on, 8192
 * and a disabled version 2 read as off. */
int sh_map_render_decode(const char *text, sh_map_render *settings);
/* On encodes as SH_RENDER_UNLOCKED_TEXT. Off has no text: it is stored as the
 * variable's absence, so encoding it fails. */
int sh_map_render_encode(const sh_map_render *settings, char *out, size_t cap);
/* Whether a string variable named name is one of Snapmap+'s own map settings
 * ("smp." prefix). No logic node binds to them, so moving such a row to a
 * lower index changes nothing an author made. */
int sh_map_render_own_variable(const char *name, int length);

int sh_map_render_install(const sig_result *results, size_t count, const uint8_t *base);
/* Both adapters run on the engine thread. Storage belongs to this native map. */
int sh_map_render_read(void *map, sh_map_render *settings);
/* On stores SH_RENDER_UNLOCKED_TEXT (appending the row when absent); off
 * removes the row so the map is what the unmodified game writes. */
int sh_map_render_write(void *map, const sh_map_render *settings);
/* Publish from the build copy entering native SnapMap conversion (Play). */
void sh_map_render_build(void *map);
/* After DeserializeFromJson: select the map's settings. An older value that
 * reads as on is rewritten in place as SH_RENDER_UNLOCKED_TEXT and one that
 * reads as off is removed, so the editable map built from this snapshot saves
 * either the canonical text or nothing. NULL clears the selection silently
 * before a load. */
void sh_map_render_loaded(void *map);
/* Retain serialized-map settings without changing the active environment.
 * Select them only when that map reaches its resource activation boundary. */
int sh_map_render_capture(void *snapshot, sh_map_render *settings);
void sh_map_render_select(const sh_map_render *settings);
int sh_map_render_editor_install(const sig_result *results, size_t count);
#endif
