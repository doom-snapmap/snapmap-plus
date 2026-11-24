/* The script SDK: Lua libraries Snapmap+ ships for every mod (src/sdk/lua),
 * embedded in the backend. A script's require("enemy_sdk") finds the mod's
 * own scripts\lib\enemy_sdk.lua first and, when the mod has none, the
 * library of that name here, so a mod uses the SDK without carrying a copy
 * and may still replace a library with its own.
 *
 *   enemy_sdk         bind a brain (an object with spawn, think, damage ...) to an entityDef
 *   enemy_snapmap     SnapMap's rules for a demon its own brain drives (what the map's author set on it)
 *   enemy_target      who a demon fights, chosen as DOOM 2016's own demons choose
 *   enemy_locomotion  locomotion modes: ground (the engine's navigation), perch (walls and ceilings)
 *   enemy_weapon      how a demon holds and fires its weapon
 *   enemy_probe       measure what a demon's brain does, as lines in the mod's script log
 *   enemy_placement   the older name of enemy_snapmap (goes after one release cycle)
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
