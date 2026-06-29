/* fault_record.c -- see fault_record.h. */
#include "fault_record.h"
#include <stdio.h>
#include <string.h>

static char g_logpath[MAX_PATH] = {0};

int shield_format(char *buf, size_t n, const shield_fault *f)
{
    if (!buf || n == 0 || !f) return 0;
    int w = _snprintf_s(buf, n, _TRUNCATE,
        "class=%s sev=%d rva=0x%llx addr=0x%llx %s",
        f->cls ? f->cls : "unknown", f->severity,
        (unsigned long long)f->faulting_rva, (unsigned long long)f->fault_addr,
        f->message ? f->message : "");
    return (w < 0) ? (int)strlen(buf) : w;   /* _TRUNCATE returns -1 on truncation; report what fit */
}

void shield_set_logpath_from_module(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA((HMODULE)self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { strcpy_s(g_logpath, MAX_PATH, "shield_faults.log"); return; }
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else path[0] = '\0';
    /* group logs under <DOOM>\snaphak_logs\ (same dir the backend log uses) */
    char dir[MAX_PATH];
    _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%ssnaphak_logs", path);
    CreateDirectoryA(dir, NULL);   /* idempotent */
    _snprintf_s(g_logpath, MAX_PATH, _TRUNCATE, "%s\\shield_faults.log", dir);
}

void shield_emit(const shield_fault *f)
{
    char body[400], line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    shield_format(body, sizeof body, f);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "[shield] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

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
