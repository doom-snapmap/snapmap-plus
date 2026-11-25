/* script_source.h -- where mod scripts come from: the serving index. At the Play gate the provider names the open
 * map's scripted mods, reads each script once from disk with the SHA-256 its build recorded, and answers every
 * list and read (require included) from that copy, so a Play runs exactly the scripts it started with.
 * A file edited since the last Refresh is refused by name. Main thread. */
#ifndef SH_SCRIPT_SOURCE_H
#define SH_SCRIPT_SOURCE_H
#include "script_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the provider with the runtime (called by sh_script_hooks_install; not in a SH_SCRIPT_PURE build).
 * `foreign_allowed` is the player's setting for scripts in content from elsewhere (modui.h
 * sh_modui_scripted_maps_allowed; NULL: allowed): off, the map's pinned rows run no script, while the player's
 * own local rows always do. */
void sh_script_source_install(int (*foreign_allowed)(void));
/* The same policy alone. */
void sh_script_source_set_policy(int (*foreign_allowed)(void));

/* The served mods of the open map that carry scripts, as the next Play would prepare them, their scripts read
 * again. Returns the count written to out (strings valid until the next call), -1 when unreadable. Main thread. */
int sh_script_source_map_mods(sh_script_mod_desc *out, int cap);

/* The file source over the scripts the last provider call or sh_script_source_map_mods read: list gives "x.lua"
 * and "lib/x.lua" in request order, read a malloc'd copy (release frees it) or -1 for an unreadable file. */
const sh_script_source *sh_script_source_files(void);

/* The first script file of `salt` (as listed) whose built bytes were not kept (they differ from the build's
 * recorded bytes, or could not be read), NULL when every file was kept; *count (optional) gets how many such
 * files. The text is valid until the next read of the map's scripts. Main thread. */
const char *sh_script_source_stale(const char *salt, int *count);

/* Forget every mod's scripts. */
void sh_script_source_clear(void);

#ifdef SH_SCRIPT_PURE
/* The tests' stand-in for the index: serves `scripts_dir` (*.lua and lib\*.lua) as `salt`'s scripts. 1 when set. */
int  sh_script_source_set_folder(const char *salt, const char *name, const char *scripts_dir);
#endif

#ifdef __cplusplus
}
#endif
#endif
