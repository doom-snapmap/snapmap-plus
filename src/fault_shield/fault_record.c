/* fault_record.c -- see fault_record.h. */
#include "fault_record.h"
#include "../common/log_rotate.h"
#include "../common/wide_path.h"
#include <stdio.h>
#include <string.h>

static wchar_t g_logpath[SH_WPATH_MAX];

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
    /* <DOOM>\snapmap-plus\logs\shield_faults.log, beside the backend log. */
    g_logpath[0] = L'\0';
    if (!sh_wpath_module_dir((void *)self, g_logpath, SH_WPATH_MAX) ||
        !sh_wpath_append(g_logpath, SH_WPATH_MAX, L"snapmap-plus") || !sh_wpath_ensure_dir(g_logpath) ||
        !sh_wpath_append(g_logpath, SH_WPATH_MAX, L"logs") || !sh_wpath_ensure_dir(g_logpath) ||
        !sh_wpath_append(g_logpath, SH_WPATH_MAX, L"shield_faults.log")) {
        g_logpath[0] = L'\0';
        return;
    }
    log_rotate_if_large_w(g_logpath, LOG_ROTATE_CAP_BYTES);
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

    OutputDebugStringA(line);   /* for an attached debugger */

    if (g_logpath[0]) {
        HANDLE h = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteFile(h, line, (DWORD)strlen(line), &wrote, NULL);
            CloseHandle(h);
        }
    }
}
