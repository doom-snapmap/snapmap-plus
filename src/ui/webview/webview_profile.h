/* webview_profile.h -- chooses the regenerable WebView2 profile folder, %LOCALAPPDATA%\snapmap-plus\cache\webview2.
 * Kernel32 only and header-only, so the frontend and its test share it.
 */
#ifndef SNAPMAP_PLUS_WEBVIEW_PROFILE_H
#define SNAPMAP_PLUS_WEBVIEW_PROFILE_H

#include <windows.h>
#include <string>

namespace sh_webview_profile {

enum outcome {
    created = 0,    /* cache\webview2 was created */
    existing = 1,   /* cache\webview2 already there */
    moved = 2,      /* the webview2 folder was renamed into cache */
    kept_old = 3    /* the rename failed: webview2 is used */
};

inline bool is_dir(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* `local` is %LOCALAPPDATA%. Returns the user-data folder to hand WebView2, renaming a webview2 folder
 * into cache when only it exists; *how and *error (the rename's GetLastError) are optional. */
inline std::wstring choose(const std::wstring &local, int *how = nullptr, DWORD *error = nullptr)
{
    const std::wstring root = local + L"\\snapmap-plus";
    const std::wstring cache = root + L"\\cache";
    const std::wstring target = cache + L"\\webview2";
    const std::wstring old = root + L"\\webview2";
    int result = created;
    if (error) *error = 0;
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(cache.c_str(), nullptr);
    if (is_dir(target)) {
        result = existing;
    } else if (is_dir(old)) {
        if (MoveFileExW(old.c_str(), target.c_str(), 0)) {
            result = moved;
        } else {
            if (error) *error = GetLastError();
            if (how) *how = kept_old;
            return old;
        }
    } else {
        CreateDirectoryW(target.c_str(), nullptr);
    }
    if (how) *how = result;
    return target;
}

}  /* namespace sh_webview_profile */

#endif
