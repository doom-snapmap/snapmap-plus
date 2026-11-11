/* script_console.h -- the sh_scripts console command.
 *
 *   sh_scripts               each mod with scripts in this map (or the last):
 *                            running, waiting for Play, stopped (with the stop
 *                            line) or ended, its files and handlers
 *   sh_scripts log [mod]     the script log of every mod, or of one (its name
 *                            or its salt): prints, warnings, the stop line
 *   sh_scripts reload        during Play: the scripts end and start again
 *                            from their files at the next frame; outside Play:
 *                            every script of the open map's mods is compiled
 *                            and any error is printed, and the files run at
 *                            the next Play
 *
 * Lines are plain sentences; nothing here runs inside the game frame. */
#ifndef SH_SCRIPT_CONSOLE_H
#define SH_SCRIPT_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCRIPT_CMD_NAME "sh_scripts"
#define SH_SCRIPT_CMD_HELP "Mod scripts in this map: what each mod's scripts are doing, or why they stopped. " \
    "'sh_scripts log [mod]' prints a mod's script log; 'sh_scripts reload' runs them again from their files."

typedef void (*sh_script_console_line_fn)(void *context, const char *line);

/* argv[0] is the first word after the command (argc may be 0). Main thread. */
void sh_script_console(int argc, const char *const *argv, sh_script_console_line_fn line, void *context);

struct idCmdArgs;
/* The console handler (commands.c's table). */
void h_sh_scripts(struct idCmdArgs *args);

#ifdef __cplusplus
}
#endif
#endif
