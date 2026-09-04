/* log_rotate.c -- see log_rotate.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "log_rotate.h"

int log_rotate_if_large(const char *path, unsigned long long cap_bytes)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    unsigned long long size;
    char previous[MAX_PATH];

    if (!path || !path[0]) return 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (size < cap_bytes) return 0;
    if (_snprintf_s(previous, sizeof previous, _TRUNCATE, "%s.prev", path) < 0) return 0;

    /* MoveFile will not overwrite, so the older roll goes first. Losing the roll
     * from two sessions ago is the point -- two files is the whole budget. */
    DeleteFileA(previous);
    return MoveFileA(path, previous) ? 1 : 0;
}
