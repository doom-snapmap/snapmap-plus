#ifndef SH_GRID_ROOM_DECL_H
#define SH_GRID_ROOM_DECL_H

#include "grid_room.h"

/* Rebuilds a stock snapModule or snapModuleInfo body from the installed source. Returns a malloc-owned
 * NUL-terminated body, or NULL on unsupported geometry or invalid syntax. */
char *sh_grid_decl(const sh_grid_size *size, const char *source, size_t length,
                   int module_info, size_t *out_length);

/* Private resource aliases keep their installed source after a dimensions prefix; a door model or an unrelated
 * module's resource is never aliased. */
int sh_grid_resource_name(const sh_grid_size *size, const char *source,
                           char *out, size_t capacity);

/* Recovers the installed, unrotated flight volume's local coordinate warp; the declaration and collision share one
 * resource identity. Unknown orientations and ambiguous owners are refused. */
int sh_grid_flight_warp(const sh_grid_size *size, const char *module_source,
                         size_t length, const char *resource, sh_grid_warp *out);

typedef struct sh_grid_builtin_light {
    char name[96],inherit[96];
    float origin[3],radius[3],center[3];
    float visible_range,shadow_range;
    int has_radius,has_visible,has_shadow;
} sh_grid_builtin_light;
/* Reads the installed module's own lights (every light/... entity and the SnapMap dynamic point) and computes
 * absolute target geometry with the function sh_grid_decl uses for the generated variant, so a resized room and a
 * loaded one end with identical values. Authored map entities are never inputs. */
int sh_grid_decl_lights(const sh_grid_size *size,const char *source,size_t length,
                       sh_grid_builtin_light *out,size_t capacity,size_t *count);
/* The same built-in lights read without any transform, for inspecting a generated variant declaration. */
int sh_grid_decl_light_values(const char *source,size_t length,
                              sh_grid_builtin_light *out,size_t capacity,size_t *count);

#endif
