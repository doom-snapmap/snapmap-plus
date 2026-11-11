/* Startup creation of the user-data folders: the root and prefabs. */
#include <windows.h>
#include <stdio.h>
#include "user_dirs.h"
#include "backend_log.h"
#include "../common/wide_path.h"

const wchar_t *const sh_user_dirs_subfolders[] = { L"snapmap-plus", L"snapmap-plus\\prefabs", NULL };

int sh_user_dirs_ensure_under(const wchar_t *base)
{
    static wchar_t path[SH_WPATH_MAX];
    int present = 0;
    if (!base || !base[0]) return 0;
    for (int i = 0; sh_user_dirs_subfolders[i]; i++) {
        if (wcslen(base) + 1 > SH_WPATH_MAX) return present;
        wcscpy_s(path, SH_WPATH_MAX, base);
        if (!sh_wpath_append(path, SH_WPATH_MAX, sh_user_dirs_subfolders[i])) return present;
        if (sh_wpath_ensure_dir(path)) {
            present++;
        } else {
            char shown[1024], line[1100];
            DWORD error = GetLastError();
            sh_wpath_utf8(path, shown, sizeof shown);
            _snprintf_s(line, sizeof line, _TRUNCATE, "user data: could not create %s (error %lu)",
                        shown, error);
            backend_log(line);
        }
    }
    return present;
}

void sh_user_dirs_ensure(void)
{
    static wchar_t base[SH_WPATH_MAX];
    if (!sh_wpath_local_appdata(base, SH_WPATH_MAX)) {
        backend_log("user data: the local application data folder is unknown; folders not created");
        return;
    }
    sh_user_dirs_ensure_under(base);
}
