#ifndef SH_MAP_RENDER_H
#define SH_MAP_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#define SH_RENDER_VARIABLE "smp.render.v1"
#define SH_RENDER_FIELDS 7
#define SH_RENDER_NATIVE_DISTANCE 8192.0f
typedef struct sh_map_render { float value[SH_RENDER_FIELDS]; } sh_map_render;
/* View distance, fog strength (percent), start, end, and linear RGB. */
void sh_map_render_default(sh_map_render *settings);
int sh_map_render_distance_override(const sh_map_render *settings);
int sh_map_render_fog_override(const sh_map_render *settings);
int sh_map_render_valid(const sh_map_render *settings);
int sh_map_render_decode(const char *text, sh_map_render *settings);
int sh_map_render_encode(const sh_map_render *settings, char *out, size_t cap);
void sh_map_render_adjust(sh_map_render *settings, unsigned field, float value);

int sh_map_render_install(const sig_result *results, size_t count, const uint8_t *base);
/* Both adapters run on the engine thread. Storage belongs to this native map. */
int sh_map_render_read(void *map, sh_map_render *settings);
int sh_map_render_write(void *map, const sh_map_render *settings);
/* Publish from the exact map entering native SnapMap conversion. */
void sh_map_render_build(void *map);
void sh_map_render_loaded(void *map);
int sh_map_render_editor_install(const sig_result *results, size_t count);
#endif
