/* The script SDK: the Lua libraries in src/sdk/lua (the enemy_* modules), embedded in the backend. A script's
 * require("enemy_sdk") finds the mod's own scripts\lib\enemy_sdk.lua first, then the library of that name
 * here, so a mod may replace any of them. */
#ifndef SH_SCRIPT_SDK_H
#define SH_SCRIPT_SDK_H

#include <stddef.h>

#define SH_SCRIPTED_COMBAT_KEY "smp_scripted_combat"

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
