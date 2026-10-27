/* The user-data folders the backend creates at startup. */
#ifndef SNAPMAP_PLUS_USER_DIRS_H
#define SNAPMAP_PLUS_USER_DIRS_H

#include <stddef.h>

/* The subfolders of the user-data root (%LOCALAPPDATA%\snapmap-plus) that
 * startup creates, NULL-terminated: the root itself (""), strings and prefabs.
 * Keep the installer's userContentSubdirs aligned with this list. */
extern const char *const sh_user_dirs_subfolders[];

/* Create `base`\snapmap-plus and its subfolders. Existing folders are fine;
 * other failures are logged. Returns the number of folders that exist
 * afterwards. */
int sh_user_dirs_ensure_under(const char *base);

/* The same under %LOCALAPPDATA%. */
void sh_user_dirs_ensure(void);

#endif
