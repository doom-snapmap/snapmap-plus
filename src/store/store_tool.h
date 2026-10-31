/* store_tool.h -- snapmap-plus-store.exe, the console tool that migrates a
 * player's store from earlier releases. The installer runs it on install and
 * update; a player can run it by hand. It links the backend's store sources
 * (modstore.c, modmigrate.c, modbundle.c, modstock.c and what they pull in)
 * and no engine, DLL or WebView code.
 *
 *   snapmap-plus-store check <store>
 *   snapmap-plus-store migrate <store> [--doom <DOOM folder>]
 *
 * <store> is the data folder, %LOCALAPPDATA%\snapmap-plus (mods\ and the
 * retired removed-mods\ and overrides\ are inside it).
 *
 * check lists what it finds, one line per kind of work: folders of the retired
 * layout, blobs\, legacy\, the old record files, the second upgrade step,
 * removed-mods\ and its record section, and an overrides\ folder the record
 * does not mark as migrated. It changes nothing. Exit 0 clean, 1 work to do,
 * 2 error.
 *
 * migrate runs, in order, the layout upgrade (sh_ms_upgrade), the bin sweep
 * (sh_ms_bin_sweep) and the overrides migration (sh_modmigrate_run), each only
 * when check finds its work, with every delete going to the Windows Recycle
 * Bin (store_recycle.h). The overrides migration reads the game's archives for
 * stock pointers (modstock.h): the folder given with --doom, else the one Steam
 * names; without either it is reported and left for a later run. One line per
 * action and a summary. Exit 0 done (or nothing to do), 1 partial (work is
 * left that a later run can do), 2 error. Idempotent: a second run finds
 * nothing and changes nothing. It refuses to run while DOOM is running.
 */
#ifndef SNAPMAP_PLUS_STORE_TOOL_H
#define SNAPMAP_PLUS_STORE_TOOL_H

#include <stddef.h>
#include "modstore.h"

enum { SH_STORE_OK = 0, SH_STORE_WORK = 1, SH_STORE_PARTIAL = 1, SH_STORE_ERROR = 2 };

/* Run one command line (argv[0] is the program). Returns the exit code. */
int sh_store_tool_run(int argc, wchar_t **argv);

/* Where lines go: `err` is 1 for error lines (stderr), else 0 (stdout). The
 * default writes UTF-8 to the console streams. */
typedef void (*sh_store_out_fn)(void *context, int err, const char *line);
void sh_store_tool_set_output(sh_store_out_fn fn, void *context);

/* Test seams. The discard replaces the Recycle Bin (NULL restores it); the
 * running check replaces the DOOM process check (NULL restores it); with
 * `steam` 0 the tool never looks DOOM up through Steam. */
void sh_store_tool_set_discard(sh_ms_discard_fn fn, void *context, const char *verb);
void sh_store_tool_set_doom_running(int (*fn)(void));
void sh_store_tool_set_steam(int steam);

#endif /* SNAPMAP_PLUS_STORE_TOOL_H */
