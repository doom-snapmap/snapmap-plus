/* script_reason.h -- the plain wording of a script stop.
 *
 * Lua's own messages ("attempt to index a nil value (field 'target')",
 * "'end' expected (to close 'function' at line 3) near <eof>") are rewritten
 * by one table into a sentence an author can act on ("\"target\" is nothing
 * here", "\"end\" expected to close \"function\" on line 3"), plus what to do.
 * A message no rule matches (an author's own error("..."), an API call's
 * refusal) is kept as written. The stops the runtime decides itself (no
 * memory left, a call that does not return) have their own wording.
 *
 * Pure: no Lua state, no allocation. */
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

/* Split a Lua message "scripts\x.lua:12: text" into file ("scripts\x.lua"),
 * line (12) and the text after it, which is returned. Without that prefix,
 * file is "" and *line 0 and msg itself is returned. Only names under
 * scripts\ are taken as a file, so an author's text with a colon is safe. */
const char *sh_script_split_where(const char *msg, char *file, size_t file_cap, int *line);

/* The plain sentence (what) and the next step (todo) for a Lua message with
 * its location already removed. Neither ends with a period. todo may be "". */
void sh_script_reason(const char *text, char *what, size_t what_cap, char *todo, size_t todo_cap);

/* The same for a stop the runtime decided; `limit` is the wall-clock stop in ms. */
void sh_script_reason_stop(int stop, unsigned long long limit, char *what, size_t what_cap,
                           char *todo, size_t todo_cap);

/* The one stop line:
 *   Script stopped: <mod>, scripts\x.lua line 12: <what>. <todo>.
 * without " line N" when line is 0 and without the location when file is "". */
/* (out needs room for the mod name, the path, what and todo, and 40 more bytes.) */
void sh_script_stop_text(const char *mod, const char *file, int line, const char *what,
                         const char *todo, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
