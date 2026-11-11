/* script_source.h -- where mod scripts come from: the serving index.
 *
 * At the Play gate the runtime asks which of the open map's mods carry
 * scripts (sh_script_set_map_provider). The answer is the serving layer's: the
 * builds the map scope declares for this Play (sh_modserve_scope_hashes: the
 * builds that serve the map's rows, then the placed names' closure), each
 * mod's scripts as the index serves them for the open map
 * (sh_modserve_script_files: the map's own place for a stranger's map, the
 * root folder for the author's own). Every script is read then, once, through
 * sh_modserve_open and kept in memory with the SHA-256 its build recorded; a
 * file whose bytes are not the built ones (edited since the last Refresh) is
 * not kept, so the runtime refuses it by name ("could not be read ... Press
 * Refresh"). The file source then answers lists and reads, require included,
 * from that copy: a Play runs exactly the built scripts it started with, and
 * nothing reads a mod folder while it runs. A Refresh during Play applies at
 * the next Play.
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

/* The served mods of the open map that carry scripts, as the next Play would
 * prepare them (the console's check outside Play), their scripts read again.
 * Returns the count written to out (strings valid until the next call), -1
 * when the map's builds could not be read. Main thread. */
int sh_script_source_map_mods(sh_script_mod_desc *out, int cap);

/* The file source over the scripts the last provider call or
 * sh_script_source_map_mods read: list gives "x.lua" and "lib/x.lua" in
 * request order, read a malloc'd copy (release frees it); read answers -1 for
 * a file whose built bytes could not be read. */
const sh_script_source *sh_script_source_files(void);

/* Forget every mod's scripts. */
void sh_script_source_clear(void);

#ifdef SH_SCRIPT_PURE
/* The tests' stand-in for the index: read `scripts_dir` (<mod folder>\assets\
 * mods\<salt>\scripts: *.lua and lib\*.lua) as `salt`'s served scripts. 1 when
 * set. */
int  sh_script_source_set_folder(const char *salt, const char *name, const char *scripts_dir);
#endif

#ifdef __cplusplus
}
#endif
#endif
