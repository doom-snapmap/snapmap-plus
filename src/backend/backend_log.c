/* Write timestamped backend diagnostics to the debugger sink and session log. */
#include "backend_log.h"
#include "../common/log_rotate.h"
#include "perf.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* The log file, as a wide path for the file calls and as UTF-8 for log rotation. Both are held
 * whole: the DLL sits in the game folder, which can be deeper than MAX_PATH. */
static wchar_t           g_logpath[32768];
static char              g_logpath_utf8[3 * 32768];
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
    g_loghandle = CreateFileW(g_logpath, FILE_APPEND_DATA,
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
    static wchar_t path[32768];
    const size_t cap = sizeof path / sizeof path[0];
    DWORD len = GetModuleFileNameW((HMODULE)self, path, (DWORD)cap);
    wchar_t *slash;
    log_lock_init();
    if (len == 0 || len >= cap) return;    /* no log file; the debugger sink still gets every line */
    slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    /* Create snapmap-plus/logs beside the DLL, one level at a time. */
    _snwprintf_s(g_logpath, cap, _TRUNCATE, L"%lssnapmap-plus", path);
    CreateDirectoryW(g_logpath, NULL);
    _snwprintf_s(g_logpath, cap, _TRUNCATE, L"%lssnapmap-plus\\logs", path);
    CreateDirectoryW(g_logpath, NULL);
    _snwprintf_s(g_logpath, cap, _TRUNCATE, L"%lssnapmap-plus\\logs\\sh_backend.log", path);
    WideCharToMultiByte(CP_UTF8, 0, g_logpath, -1, g_logpath_utf8, (int)sizeof g_logpath_utf8, NULL, NULL);
    /* Roll the accumulated log aside before this session appends to it. */
    log_rotate_if_large(g_logpath_utf8, LOG_ROTATE_CAP_BYTES);
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
    char line[4096];
    SYSTEMTIME st;
    DWORD n;
    SH_PERF_BEGIN(t0);
    GetLocalTime(&st);
    n = (DWORD)_snprintf_s(line, sizeof line, _TRUNCATE,
        "[snapmap+] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : "");
    if ((int)n <= 0) {
        /* Truncated: keep the line readable and end it, so the next line does
         * not run on after it (a reload listing 90 files overran 512 bytes). */
        n = (DWORD)strlen(line);
        if (n + 5 > sizeof line) n = (DWORD)sizeof line - 5;
        memcpy(line + n, "...\n", 5);
        n += 4;
    }

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
