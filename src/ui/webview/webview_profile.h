/* webview_profile.h -- where the companion window's WebView2 profile lives.
 *
 * The profile (cookies, caches, local storage of the embedded page) is
 * regenerable: WebView2 rebuilds it when it is missing. It lives under
 * %LOCALAPPDATA%\snapmap-plus\cache\webview2, and the installer never backs
 * up or copies the cache folder. Releases before this one kept it at
 * %LOCALAPPDATA%\snapmap-plus\webview2; the first start moves that folder into
 * the cache folder by renaming it (same volume, nothing copied, nothing
 * deleted). When the rename fails (a file in it is held open), this start
 * keeps using the old folder and the next start tries again. When both
 * folders exist the new one is used and the old one is left as it is.
 *
 * Kernel32 only, header-only, so the frontend and its test share it.
 */
#ifndef SNAPMAP_PLUS_WEBVIEW_PROFILE_H
#define SNAPMAP_PLUS_WEBVIEW_PROFILE_H

#include <windows.h>
#include <string>

namespace sh_webview_profile {

enum outcome {
    created = 0,    /* no profile yet: cache\webview2 was created */
    existing = 1,   /* cache\webview2 already there */
    moved = 2,      /* the old webview2 folder was renamed into the cache folder */
    kept_old = 3    /* the rename failed: the old folder is used this time */
};

inline bool is_dir(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* `local` is %LOCALAPPDATA%. Returns the user-data folder to hand WebView2;
 * *how and *error (the rename's GetLastError on kept_old) are optional. */
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
