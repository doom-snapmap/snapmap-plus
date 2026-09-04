/* backend_log.c -- see backend_log.h. */
#include "backend_log.h"
#include "../common/log_rotate.h"
#include <stdio.h>
#include <string.h>

static char g_logpath[MAX_PATH] = {0};

void backend_set_logpath_from_module(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { strcpy_s(g_logpath, MAX_PATH, "sh_backend.log"); return; }
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else path[0] = '\0';
    /* keep the DOOM install dir clean: group all logs under <DOOM>\snapmap-plus\logs\ (parent first --
     * CreateDirectory makes one level at a time; both calls idempotent) */
    char dir[MAX_PATH];
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus", path);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus\\logs", path);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(g_logpath, MAX_PATH, _TRUNCATE, "%s\\sh_backend.log", dir);
    /* Roll the accumulated log aside before this session appends to it. */
    log_rotate_if_large(g_logpath, LOG_ROTATE_CAP_BYTES);
}

void backend_log(const char *msg)
{
    char line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "[snapmap+] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : "");

    OutputDebugStringA(line);   /* -> in-game console + any attached debugger */

    if (g_logpath[0]) {
        HANDLE h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteFile(h, line, (DWORD)strlen(line), &wrote, NULL);
            CloseHandle(h);
        }
    }
}
