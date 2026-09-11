#ifndef SH_GRID_ROOM_DECL_H
#define SH_GRID_ROOM_DECL_H

#include "grid_room.h"

/* Rebuild a stock snapModule or snapModuleInfo body. Return a malloc-owned
 * NUL-terminated body, or NULL on unsupported geometry or invalid syntax.
 * All input is read from the owner's installation; the product embeds none. */
char *sh_grid_decl(const sh_grid_size *size, const char *source, size_t length,
                   int module_info, size_t *out_length);

/* Private resource aliases retain their installed source after a dimensions
 * prefix. This never aliases a door model or an unrelated module's resource. */
int sh_grid_resource_name(const sh_grid_size *size, const char *source,
                           char *out, size_t capacity);

/* Recover the installed, unrotated flight volume's local coordinate warp.
 * The declaration and collision share one exact resource identity. Unknown
 * orientations and ambiguous owners are refused. */
int sh_grid_flight_warp(const sh_grid_size *size, const char *module_source,
                         size_t length, const char *resource, sh_grid_warp *out);

typedef struct sh_grid_builtin_light {
    char name[96],inherit[96];
    float origin[3],radius[3],center[3];
    float visible_range,shadow_range;
} sh_grid_builtin_light;
/* Read the installed module's own lights and compute absolute target geometry.
 * Authored map entities are never inputs to this resource-level transform. */
int sh_grid_decl_lights(const sh_grid_size *size,const char *source,size_t length,
                       sh_grid_builtin_light *out,size_t capacity,size_t *count);

#endif
