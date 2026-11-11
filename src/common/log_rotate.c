/* log_rotate.c -- see log_rotate.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "log_rotate.h"
#include "wide_path.h"

int log_rotate_if_large_w(const wchar_t *path, unsigned long long cap_bytes)
{
    static wchar_t previous[SH_WPATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA info;
    unsigned long long size;
    size_t len;

    if (!path || !path[0]) return 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return 0;
    size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (size < cap_bytes) return 0;
    len = wcslen(path);
    if (len + 6 > SH_WPATH_MAX) return 0;
    memcpy(previous, path, len * sizeof(wchar_t));
    memcpy(previous + len, L".prev", 6 * sizeof(wchar_t));

    /* Remove the older roll because MoveFile does not overwrite it. */
    DeleteFileW(previous);
    return MoveFileW(path, previous) ? 1 : 0;
}

int log_rotate_if_large(const char *path, unsigned long long cap_bytes)
{
    static wchar_t wide[SH_WPATH_MAX];
    if (!path || !path[0]) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, SH_WPATH_MAX) <= 0) return 0;
    return log_rotate_if_large_w(wide, cap_bytes);
}
