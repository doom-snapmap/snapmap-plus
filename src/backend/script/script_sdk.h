/* The script SDK: Lua libraries Snapmap+ ships for every mod (src/sdk/lua),
 * embedded in the backend. A script's require("enemy_sdk") finds the mod's
 * own scripts\lib\enemy_sdk.lua first and, when the mod has none, the
 * library of that name here, so a mod uses the SDK without carrying a copy
 * and may still replace a library with its own.
 *
 *   enemy_sdk         bind a brain (an object with spawn, think, damage ...) to an entityDef
 *   enemy_placement   what the map's author set on a placed demon (Spawn Behavior, Team, role)
 *   enemy_locomotion  locomotion modes: ground (the engine's navigation), perch (walls and ceilings)
 */
#ifndef SH_SCRIPT_SDK_H
#define SH_SCRIPT_SDK_H

#include <stddef.h>

typedef struct sh_script_sdk_lib {
    const char *name;      /* as required, without ".lua" */
    const char *text;
    size_t      length;
} sh_script_sdk_lib;

/* The libraries, ended by a NULL name (script_sdk_lua.c, generated). */
extern const sh_script_sdk_lib SH_SCRIPT_SDK_LIBS[];

/* The library named `name`, or NULL. */
const sh_script_sdk_lib *sh_script_sdk_find(const char *name);

#endif
