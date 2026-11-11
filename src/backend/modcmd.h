/* modcmd.h -- the sh_mods console command.
 *
 *   sh_mods [status]               what the serving index holds and serves (modserve_engine.c)
 *   sh_mods rescan | refresh       build every mod folder whose files changed (modstore.h, the
 *                                  rebuild rule), then reread the mods folder into the serving index
 *   sh_mods palette rebuild        add Toybox rows of mods indexed after the first palette build
 *   sh_mods build "<folder>"       build a mod and write its mod.json's "snapmap-plus" block
 *   sh_mods rebuild "<folder>"     build again and rescan (the Mods tab's Rebuild)
 *   sh_mods install "<folder>"     build, copy into the mods folder under its own name, verify, rescan
 *   sh_mods list                   the mod folders and their state (a folder that does not build:
 *                                  its cause; it serves nothing until fixed)
 *   sh_mods new "<name>"           a new mod's folder, named after the mod (modstore.h)
 *   sh_mods verify "<folder>"      rehash an installed mod and record the result
 *   sh_mods doctor [verify]        what the Mods tab does not show: every folder's state and
 *                                  cause, per path the layer that serves it and why, the
 *                                  registries and their clashes; `verify` rehashes every build
 *   sh_mods held [filter]          what the game was served under each name this session and
 *                                  what it holds (modserve_engine.c)
 *   sh_mods choose "<slot>" "<mod folder>"   which of the open map's mods' edits serves a field
 *                                  several of them set (slot "<game file>/<path>" as the doctor
 *                                  names it); an empty mod clears
 *   sh_mods map                    the open map and each mod it uses (modui.c)
 *   sh_mods map add "<mod folder>"      use that mod in the whole open map without placing anything
 *   sh_mods map remove "<mod folder>" [--yes]   what removing it from the open map does; --yes does it
 *   sh_mods uninstall "<mod folder>" [--yes]    what uninstalling does; --yes does it
 *   sh_mods copystock "<engine path>" "<file>"  write an installed game file's bytes to a file
 *   sh_mods push "<mod folder>" "<ref>"         upload a mod's build to the mod store and claim it
 *   sh_mods release "<ref>"        release the claim stored for ref
 *   sh_mods retry                  retry uploads queued after a failure
 *   sh_mods export "<path.zip>"    write the open map and its mods to one ZIP
 *   sh_mods import "<path.zip>"    open a map's ZIP as File > Load Rawmap does
 *   sh_mods cancel                 stop the running push, release or retry
 *
 * push, release and retry run on a worker thread (modnet.c), one at a time;
 * their result lines print when they finish. build, rebuild and install take
 * the store lock.
 *
 * A folder without a drive or leading backslash is taken relative to the
 * mods folder (%LOCALAPPDATA%\snapmap-plus\mods\). Every step logs one MODS:
 * line to the backend log and the console, naming mods by name or folder.
 */
#ifndef SNAPMAP_PLUS_MODCMD_H
#define SNAPMAP_PLUS_MODCMD_H

#include "commands.h"

#define SH_MODCMD_NAME "sh_mods"
#define SH_MODCMD_HELP "Mods: 'sh_mods' or 'sh_mods status' shows what installed mods serve, 'sh_mods rescan' (or " \
                       "'sh_mods refresh') rebuilds changed mod folders, rereads the mods folder and applies what " \
                       "changed to the running game (what needs a map reload or a restart is listed), 'sh_mods palette " \
                       "rebuild' adds new mod rows to the open editor, 'sh_mods build \"<folder>\"' checks a mod and " \
                       "writes its build into its mod.json, 'sh_mods install \"<folder>\"' adds it to the mods folder, " \
                       "'sh_mods list' shows the mod folders, 'sh_mods doctor [verify]' explains every folder's state, " \
                       "clashes and shadows, 'sh_mods verify \"<folder>\"' rehashes one, 'sh_mods copystock \"<engine " \
                       "path>\" \"<file>\"' writes an installed game file's bytes to a file. 'sh_mods help' lists the Mods " \
                       "tab and mod store verbs."
/* Printed by 'sh_mods help' after SH_MODCMD_HELP: the Mods tab's actions, sharing its dry runs. */
#define SH_MODCMD_HELP_TAB "Mods tab: 'sh_mods map' prints the open map and each mod it uses with its placed entities, " \
                           "wires and Timelines, 'sh_mods map add \"<mod folder>\"' uses a mod in the whole map without " \
                           "placing anything, 'sh_mods map remove \"<mod folder>\"' prints what removing that mod from the map " \
                           "deletes and '--yes' does it (one undo step), 'sh_mods uninstall \"<mod folder>\"' prints what " \
                           "uninstalling does (the open map cleaned of it, its undo history cleared, its Toybox rows, its " \
                           "folder moved to the Windows Recycle Bin) and '--yes' does it, 'sh_mods rebuild \"<folder>\"' " \
                           "builds a mod folder again and rescans."
/* Printed by 'sh_mods help' after SH_MODCMD_HELP_TAB: authoring. */
#define SH_MODCMD_HELP_AUTHOR "Authoring: 'sh_mods new \"<name>\"' makes a mod folder named after the mod with a working " \
                              "example, 'sh_mods held [filter]' prints what the game was served under each name this " \
                              "session and what it still holds, 'sh_mods choose \"<game file>/<path>\" \"<mod folder>\"' picks " \
                              "which of the open map's mods' edits serves a field several of them set " \
                              "(entitydef/player/coop/edit/handsModelMPUseDoomHands; an empty mod clears)."
/* Printed by 'sh_mods help' after SH_MODCMD_HELP_AUTHOR (each under the console's 1 KiB line). */
#define SH_MODCMD_HELP_STORE "Mod store: 'sh_mods push \"<mod folder>\" \"<ref>\"' uploads a mod and claims it under ref, " \
                             "'sh_mods release \"<ref>\"' drops a claim, 'sh_mods retry' retries uploads queued after a " \
                             "failure, 'sh_mods export \"<path.zip>\"' writes the open map with its mods, 'sh_mods import " \
                             "\"<path.zip>\"' opens such a ZIP as File > Load Rawmap does, 'sh_mods cancel' stops the " \
                             "running upload."

/* The console handler (idCmdArgs layout from commands.h). */
void h_sh_mods(idCmdArgs *args);

/* One-time setup, called from sh_commands_install after the command table is
 * registered: resolves and logs the store root. Returns the number of console
 * commands this module provides (the sh_mods table row). */
int sh_modcmd_register(void);

#endif /* SNAPMAP_PLUS_MODCMD_H */
