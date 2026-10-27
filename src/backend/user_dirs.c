/* Startup creation of the user-data folders: the root, strings and prefabs. */
#include <windows.h>
#include <stdio.h>
#include <shlobj.h>
#include "user_dirs.h"
#include "backend_log.h"

const char *const sh_user_dirs_subfolders[] = { "", "\\strings", "\\prefabs", NULL };

int sh_user_dirs_ensure_under(const char *base)
{
    char path[MAX_PATH];
    int present = 0;
    if (!base || !base[0]) return 0;
    for (int i = 0; sh_user_dirs_subfolders[i]; i++) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus%s", base, sh_user_dirs_subfolders[i]);
        if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            present++;
        } else {
            char line[MAX_PATH + 64];
            _snprintf_s(line, sizeof line, _TRUNCATE, "WARN: CreateDirectory %s failed (err %lu)",
                        path, GetLastError());
            backend_log(line);
        }
    }
    return present;
}

void sh_user_dirs_ensure(void)
{
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        backend_log("WARN: SHGetFolderPathA(CSIDL_LOCAL_APPDATA) failed -- snapmap-plus user dirs not created");
        return;
    }
    sh_user_dirs_ensure_under(base);
    backend_log("snapmap-plus user-data dirs ensured (root + strings/prefabs)");
}
