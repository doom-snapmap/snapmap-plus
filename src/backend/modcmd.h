/* modcmd.h -- the sh_mods console command.
 *
 *   sh_mods [status]               what the serving index holds and serves (modserve_engine.c)
 *   sh_mods rescan                 reread the mods folder into the serving index
 *   sh_mods palette rebuild        add editor rows of bundles indexed after the first palette build
 *   sh_mods build "<folder>"       build and validate a mod, write its bundle.json
 *   sh_mods install "<folder>"     build, copy into the store, lay out, verify, rescan
 *   sh_mods list                   bundle folders in the store and their state
 *   sh_mods verify "<id@version+build>"  rehash an installed bundle (id@version: every build)
 *   sh_mods copystock "<engine path>" "<file>"  write an installed game file's bytes to a file
 *                                  (authoring: a stock mesh or anim copied under a new name)
 *   sh_mods push "<id@version+build>" "<ref>"   upload a bundle to the mod store and claim it under ref
 *   sh_mods pull "<bundle hash>"   download a bundle from the mod store and install it
 *   sh_mods release "<ref>"        release the claim stored for ref
 *   sh_mods retry                  retry uploads queued after a failure
 *   sh_mods export "<path.zip>"    write the open map and the bundles it names to one ZIP
 *   sh_mods import "<path.zip>"    install an export's bundles and stage its map
 *   sh_mods cancel                 stop the running push, pull, release, retry or import
 *
 * push, pull, release, retry and import run on a worker thread (modnet.c),
 * one at a time; their result lines print when they finish.
 *
 * A folder without a drive or leading backslash is taken relative to the
 * store root (%LOCALAPPDATA%\snapmap-plus\mods\). Every step logs one MODS:
 * line to the backend log and the console. Stock needs and shadows are checked
 * through the resource layer's installed-resource reader. A successful install
 * (and a build of a folder inside the store) rebuilds the serving index.
 */
#ifndef SNAPMAP_PLUS_MODCMD_H
#define SNAPMAP_PLUS_MODCMD_H

#include "commands.h"

#define SH_MODCMD_NAME "sh_mods"
#define SH_MODCMD_HELP "Mods: 'sh_mods' or 'sh_mods status' shows what installed mods serve, 'sh_mods rescan' rereads " \
                       "the mods folder, 'sh_mods palette rebuild' adds new mod rows to the open editor, " \
                       "'sh_mods build \"<folder>\"' checks a mod and writes its bundle.json, " \
                       "'sh_mods install \"<folder>\"' adds it to the mods store, 'sh_mods list' shows " \
                       "installed bundles, 'sh_mods verify \"<id@version+build>\"' rehashes one (or every build of id@version), " \
                       "'sh_mods copystock \"<engine path>\" \"<file>\"' writes an installed game file's bytes to a file. " \
                       "'sh_mods help' lists the mod store verbs."
/* Printed by 'sh_mods help' after SH_MODCMD_HELP (each under the console's 1 KiB line). */
#define SH_MODCMD_HELP_STORE "Mod store: 'sh_mods push \"<id@version+build>\" \"<ref>\"' uploads a bundle and claims it under ref, " \
                             "'sh_mods pull \"<bundle hash>\"' downloads and installs one, 'sh_mods release \"<ref>\"' drops " \
                             "a claim, 'sh_mods retry' retries uploads queued after a failure, 'sh_mods export \"<path.zip>\"' " \
                             "writes the open map with its mods, 'sh_mods import \"<path.zip>\"' installs them and stages the " \
                             "map, 'sh_mods cancel' stops the running job."

/* The console handler (idCmdArgs layout from commands.h). */
void h_sh_mods(idCmdArgs *args);

/* One-time setup, called from sh_commands_install after the command table is
 * registered: resolves and logs the store root. Returns the number of console
 * commands this module provides (the sh_mods table row). */
int sh_modcmd_register(void);

#endif /* SNAPMAP_PLUS_MODCMD_H */
