/* backend_log.c -- see backend_log.h. */
#include "backend_log.h"
#include <stdio.h>
#include <string.h>

static char g_logpath[MAX_PATH] = {0};

void backend_set_logpath_from_module(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { strcpy_s(g_logpath, MAX_PATH, "snaphak_backend.log"); return; }
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else path[0] = '\0';
    /* keep the DOOM install dir clean: group all logs under <DOOM>\snaphak_logs\ */
    char dir[MAX_PATH];
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnaphak_logs", path);
    CreateDirectoryA(dir, NULL);   /* idempotent; ignores "already exists" */
    _snprintf_s(g_logpath, MAX_PATH, _TRUNCATE, "%s\\snaphak_backend.log", dir);
}

void backend_log(const char *msg)
{
    char line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "[snaphak] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : "");

    OutputDebugStringA(line);   /* -> in-game console + daemon ring buffer */

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
