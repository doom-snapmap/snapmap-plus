/* script_compile.h -- the compile check of one mod script, run by the builder (modbundle.h sh_mb_set_script_check)
 * on every script a mod carries; an error is worded as the runtime's stop line (script_reason.h).
 * Each call parses the text (mode "t") in a fresh Lua state capped at SH_SCRIPT_COMPILE_MEM; any thread. */
#ifndef SH_SCRIPT_COMPILE_H
#define SH_SCRIPT_COMPILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCRIPT_COMPILE_MEM (16u << 20)   /* parser memory for one file */

/* The first statement of an entry file is "api = <integer>" (comments, blank lines and a UTF-8 byte order mark may
 * come before it): 1 with the version and its line; 0 when it does not start with it (*line: the first statement). */
int sh_script_api_declared(const char *text, size_t len, long *version, int *line);

/* modbundle.h sh_mb_script_check_fn: `file` is "scripts\x.lua", `entry` 1 for a file directly in scripts\ (it must
 * declare api = 1). 1 when the text compiles; 0 with *line (0 when none), what and todo (no final period). */
int sh_script_compile_check(const char *file, int entry, const char *text, size_t len, int *line,
                            char **what, char **todo);   /* *what, *todo malloc'd (the caller frees them) */

#ifdef __cplusplus
}
#endif
#endif
