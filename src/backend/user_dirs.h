/* The user-data folders the backend creates at startup. */
#ifndef SNAPMAP_PLUS_USER_DIRS_H
#define SNAPMAP_PLUS_USER_DIRS_H

#include <stddef.h>
#include <wchar.h>

/* The folders under the local application data folder that startup creates,
 * in creation order, NULL-terminated: the user-data root (snapmap-plus) and
 * its prefabs folder. Keep the installer's userContentSubdirs aligned. */
extern const wchar_t *const sh_user_dirs_subfolders[];

/* Create those folders under base (a wide path of any length). Existing
 * folders are fine; other failures are logged. Returns the number of folders
 * that exist afterwards. */
int sh_user_dirs_ensure_under(const wchar_t *base);

/* The same under %LOCALAPPDATA%. */
void sh_user_dirs_ensure(void);

#endif
