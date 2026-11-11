/* script_source.h -- where mod scripts come from: the served mod folders.
 *
 * At the Play gate the runtime asks which of the open map's mods carry
 * scripts (sh_script_set_map_provider). The answer is the serving layer's: the
 * builds the map scope declares for this Play (sh_modserve_scope_hashes: the
 * builds that serve the map's rows, then the placed names' closure), each
 * resolved to the store folder that serves it for the open map
 * (sh_modserve_row_build: the map's own place for a stranger's map, the root
 * folder for the author's own), and kept when that folder holds
 * assets\mods\<salt>\scripts\. The file source then lists and reads the
 * scripts of exactly that folder, so a downloaded map runs the copy it brought
 * and never a root folder of the same mod at another build.
 *
 * Until the builder carries scripts in its index, the files are read from the
 * folder directly: scripts\*.lua (entry files) and scripts\lib\*.lua, at most
 * SH_SCRIPT_FILE_MAX + 1 bytes each (the runtime refuses a larger one by
 * name). The contract is sh_script_source (script_runtime.h); the builder's
 * source replaces this one behind it.
 *
 * Main thread: the provider runs at the gate, the reads at the first frame
 * and inside require. */
#ifndef SH_SCRIPT_SOURCE_H
#define SH_SCRIPT_SOURCE_H
#include "script_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the provider with the runtime. Called by sh_script_hooks_install.
 * Not in a SH_SCRIPT_PURE build (it reads the serving layer). */
void sh_script_source_install(void);

/* The served mods of the open map that carry a scripts folder, as the next
 * Play would prepare them (the console's check outside Play). Returns the
 * count written to out (strings valid until the next call), -1 when the
 * map's builds could not be read. Main thread. */
int sh_script_source_map_mods(sh_script_mod_desc *out, int cap);

/* The file source over the folders the last provider call or
 * sh_script_source_map_mods found. */
const sh_script_source *sh_script_source_files(void);

/* The absolute folder that serves `salt`'s scripts for the open map, from the
 * last call ("" when none). */
const char *sh_script_source_folder(const char *salt);

/* The folder table directly (the tests, and the builder's source until it
 * lands): forget every folder, or set `salt`'s scripts folder
 * (<mod folder>\assets\mods\<salt>\scripts). 1 when set. */
void sh_script_source_clear(void);
int  sh_script_source_set_folder(const char *salt, const char *name, const char *scripts_dir);

#ifdef __cplusplus
}
#endif
#endif
