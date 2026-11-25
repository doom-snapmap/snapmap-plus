/* script_reason.h -- the plain wording of a script stop: one table rewrites Lua's own messages ("attempt to
 * index a nil value (field 'target')") into a sentence an author can act on plus what to do; any other message
 * is kept as written. Pure, no Lua state; every text is malloc'd. */
#ifndef SH_SCRIPT_REASON_H
#define SH_SCRIPT_REASON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why the runtime stopped a mod when no Lua message says it. */
#define SH_SCRIPT_STOP_ERROR    0   /* a Lua error: the message says why */
#define SH_SCRIPT_STOP_MEMORY   3   /* the process had no memory to give */
#define SH_SCRIPT_STOP_WALL     5   /* one call ran longer than the wall-clock stop */
/* A call to a name that does not exist (the API guard in script_runtime.c and script_lib_base.c): an unset
 * global or an entity method Snapmap+ lacks. No pcall or coroutine catches it; the mod stops naming the call. */
#define SH_SCRIPT_STOP_UNKNOWN_CALL   6
#define SH_SCRIPT_STOP_UNKNOWN_METHOD 7

/* Split a Lua message "scripts\x.lua:12: text" into file, line and the returned text after it. Without that
 * prefix (only names under scripts\ count) file is "", *line 0 and msg itself is returned; *file is malloc'd. */
const char *sh_script_split_where(const char *msg, char **file, int *line);

/* The plain sentence (*what) and next step (*todo) for a Lua message with its location removed, both
 * malloc'd (NULL only when out of memory) and without a final period; *todo may be "". */
void sh_script_reason(const char *text, char **what, char **todo);

/* The same for a stop the runtime decided; `limit` is the wall-clock stop in ms, `call` the unknown-call
 * stop's name (NULL otherwise). */
void sh_script_reason_stop(int stop, unsigned long long limit, const char *call, char **what, char **todo);

/* The one stop line, malloc'd (NULL when out of memory):
 *   Script stopped: <mod>, scripts\x.lua line 12: <what>. <todo>.
 * without " line N" when line is 0 and without the location when file is "". */
char *sh_script_stop_text(const char *mod, const char *file, int line, const char *what, const char *todo);

#ifdef __cplusplus
}
#endif
#endif
