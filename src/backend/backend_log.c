/* Write timestamped backend diagnostics to the debugger sink and session log.

 */
#include "backend_log.h"
#include "../common/log_rotate.h"
#include "perf.h"
#include <stdio.h>
#include <string.h>

static char              g_logpath[MAX_PATH] = {0};
static HANDLE            g_loghandle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION  g_loglock;
static LONG volatile     g_lock_ready = 0;

/* The handle stays open for the session. Opening and closing it per line costs
 * ~313us against ~8us for the write, and the engine's resource path logs
 * thousands of lines inside a single map load. */
static HANDLE log_handle(void)
{
    if (g_loghandle != INVALID_HANDLE_VALUE) return g_loghandle;
    if (!g_logpath[0]) return INVALID_HANDLE_VALUE;
    g_loghandle = CreateFileA(g_logpath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return g_loghandle;
}

static void log_lock_init(void)
{
    if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_loglock);
        InterlockedExchange(&g_lock_ready, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_lock_ready, 2, 2) != 2) Sleep(0);
}

void backend_set_logpath_from_module(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)self, path, MAX_PATH);
    log_lock_init();
    if (len == 0 || len >= MAX_PATH) { strcpy_s(g_logpath, MAX_PATH, "sh_backend.log"); return; }
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else path[0] = '\0';
    /* Create snapmap-plus/logs beneath the game directory, one level at a time. */
    char dir[MAX_PATH];
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus", path);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus\\logs", path);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(g_logpath, MAX_PATH, _TRUNCATE, "%s\\sh_backend.log", dir);
    /* Roll the accumulated log aside before this session appends to it. */
    log_rotate_if_large(g_logpath, LOG_ROTATE_CAP_BYTES);
}

void backend_log_close(void)
{
    if (InterlockedCompareExchange(&g_lock_ready, 2, 2) != 2) return;
    EnterCriticalSection(&g_loglock);
    if (g_loghandle != INVALID_HANDLE_VALUE) { CloseHandle(g_loghandle); g_loghandle = INVALID_HANDLE_VALUE; }
    LeaveCriticalSection(&g_loglock);
}

void backend_log(const char *msg)
{
    char line[512];
    SYSTEMTIME st;
    DWORD n;
    SH_PERF_BEGIN(t0);
    GetLocalTime(&st);
    n = (DWORD)_snprintf_s(line, sizeof line, _TRUNCATE,
        "[snapmap+] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : "");
    if ((int)n <= 0) n = (DWORD)strlen(line);

    OutputDebugStringA(line);   /* Debugger sink; installed output hooks may also forward this text. */

    if (!g_logpath[0]) { SH_PERF_END(SH_PERF_LOG_WRITE, t0); return; }
    log_lock_init();
    EnterCriticalSection(&g_loglock);
    {
        HANDLE h = log_handle();
        DWORD wrote;
        if (h != INVALID_HANDLE_VALUE && !WriteFile(h, line, n, &wrote, NULL)) {
            /* A handle lost to a moved or deleted file reopens once, so a
             * rotation outside this process does not silence the session. */
            CloseHandle(h);
            g_loghandle = INVALID_HANDLE_VALUE;
            h = log_handle();
            if (h != INVALID_HANDLE_VALUE) WriteFile(h, line, n, &wrote, NULL);
        }
    }
    LeaveCriticalSection(&g_loglock);
    SH_PERF_END(SH_PERF_LOG_WRITE, t0);
}
