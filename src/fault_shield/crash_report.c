/* Capture crash records with bounded static buffers and write-through files.
 * Pre-bind dbghelp during initialization. Fatal capture is best-effort and does
 * not change exception handling. Callers guard the fatal capture attempt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <dbghelp.h>
#include "crash_record_format.h"
#include "crash_report.h"
#include "fault_record.h"
#include "recovery.h"    /* shield_last_engine_msg */
#include "veh.h"         /* shield_capture_stack */
#include "engine_layout.h"
#include "../backend/host_image.h"   /* sh_host_renderer_name -- snapshotted at init, never at fault time */

extern uint8_t *g_doom_base;

#define CRASH_MAX_RECORDS 8   /* per-session cap: a fault storm can't spam pending-*.json */

typedef BOOL (WINAPI *minidump_write_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                        PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);

static char g_crash_dir[MAX_PATH] = {0};   /* <game>\snapmap-plus\crash */
static char g_dump_path[MAX_PATH] = {0};   /* <game>\snapmap-plus\logs\sh_crash.dmp */
static char g_inst_version[48]    = {0};   /* installed version at arm time */
static const char *g_renderer     = "";    /* "vulkan"/"opengl" at arm time (see crash_report_init) */
static minidump_write_t g_minidump_write = NULL;
static volatile LONG g_records_written = 0;
static volatile LONG g_fatal_oneshot   = 0;   /* first-chance 0xC0000374/0xC0000409 full capture, once */
static volatile LONG g_uef_entered     = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = NULL;

/* record-format scratch -- static, not stack (crash contexts may have little stack left). */
static char g_rec_buf[8192];
static char g_rec_stack[2048];
static char g_rec_text[HARVEST_MSG_MAX];
static volatile LONG g_record_busy, g_capture_busy, g_dump_busy;

static void crash_dirs_from_module(void)
{
    HMODULE self = NULL;
    char path[MAX_PATH];
    char *slash;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&crash_dirs_from_module, &self) || !self) return;
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    /* <game>\snapmap-plus\crash (one level at a time; idempotent) + the dump path under logs\ */
    _snprintf_s(g_crash_dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus", path);
    CreateDirectoryA(g_crash_dir, NULL);
    _snprintf_s(g_dump_path, MAX_PATH, _TRUNCATE, "%ssnapmap-plus\\logs", path);
    CreateDirectoryA(g_dump_path, NULL);
    _snprintf_s(g_crash_dir, MAX_PATH, _TRUNCATE, "%ssnapmap-plus\\crash", path);
    CreateDirectoryA(g_crash_dir, NULL);
    _snprintf_s(g_dump_path, MAX_PATH, _TRUNCATE, "%ssnapmap-plus\\logs\\sh_crash.dmp", path);
}

/* Read the installer manifest version; leave it empty if unavailable or malformed. */
static void crash_read_version(void)
{
    char la[MAX_PATH], path[MAX_PATH], data[4096];
    HANDLE h;
    DWORD got = 0;
    const char *k;
    char *p, *q;
    if (GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH) == 0) return;
    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\snapmap-plus\\install.json", la);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, data, sizeof data - 1, &got, NULL);
    CloseHandle(h);
    data[got] = '\0';
    k = "\"version\"";
    p = strstr(data, k);
    if (!p) return;
    p = strchr(p + strlen(k), ':'); if (!p) return;
    p = strchr(p, '"');             if (!p) return;
    p++;
    q = strchr(p, '"');             if (!q) return;
    if (q - p >= (ptrdiff_t)sizeof g_inst_version) q = p + sizeof g_inst_version - 1;
    memcpy(g_inst_version, p, (size_t)(q - p));
    g_inst_version[q - p] = '\0';
}

static void crash_report_file_locked(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path)
{
    crash_record r;
    SYSTEMTIME st;
    char tbuf[32], fpath[MAX_PATH], temporary[MAX_PATH];
    int len, tryn;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (g_crash_dir[0] == '\0') return;
    if (InterlockedIncrement(&g_records_written) > CRASH_MAX_RECORDS) return;

    GetLocalTime(&st);
    _snprintf_s(tbuf, sizeof tbuf, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    r.kind = kind; r.code = code;
    r.rip_rva = (unsigned long long)rip_rva; r.fault_addr = (unsigned long long)fault_addr;
    r.module = module_name; r.stack = stack; r.engine_text = engine_text;
    r.dump = dump_path; r.version = g_inst_version; r.time = tbuf;
    r.renderer = g_renderer;
    len = crash_record_json(g_rec_buf, sizeof g_rec_buf, &r);
    if (len <= 0) return;

    /* CREATE_NEW preserves same-second records; the UI orders collision suffixes numerically. */
    for (tryn = 0; tryn < CRASH_MAX_RECORDS; tryn++) {
        if (tryn == 0)
            _snprintf_s(fpath, MAX_PATH, _TRUNCATE, "%s\\pending-%04d%02d%02d-%02d%02d%02d.json",
                        g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            _snprintf_s(fpath, MAX_PATH, _TRUNCATE, "%s\\pending-%04d%02d%02d-%02d%02d%02d-%d.json",
                        g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tryn);
        if (_snprintf_s(temporary, sizeof temporary, _TRUNCATE, "%s.tmp", fpath) < 0) return;
        if (GetFileAttributesA(fpath) != INVALID_FILE_ATTRIBUTES) continue;
        h = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return;
    }
    if (h == INVALID_HANDLE_VALUE) return;
    {
        DWORD wrote = 0;
        BOOL ok = WriteFile(h, g_rec_buf, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
        CloseHandle(h);
        if (!ok || !MoveFileExA(temporary, fpath, MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(temporary);
            return;
        }
    }
    {
        shield_fault f = { "crash-record", (int)code, "crash record written for the report dialog",
                           rip_rva, fault_addr };
        shield_emit(&f);
    }
}

void crash_report_file(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path)
{
    /* A fault handler must never wait on a thread that may itself have faulted. */
    if (InterlockedCompareExchange(&g_record_busy, 1, 0) != 0) return;
    __try {
        crash_report_file_locked(kind, code, rip_rva, fault_addr, module_name,
                                 stack, engine_text, dump_path);
    } __finally { InterlockedExchange(&g_record_busy, 0); }
}

static const char *crash_report_write_dump_locked(EXCEPTION_POINTERS *ep)
{
    HANDLE h;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    BOOL ok = FALSE;
    if (!g_minidump_write || g_dump_path[0] == '\0') return "";
    h = CreateFileA(g_dump_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return "";
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    __try {
        ok = g_minidump_write(GetCurrentProcess(), GetCurrentProcessId(), h,
                              (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo),
                              ep ? &mei : NULL, NULL, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    CloseHandle(h);
    return ok ? g_dump_path : "";
}

const char *crash_report_write_dump(EXCEPTION_POINTERS *ep)
{
    const char *path = "";
    if (InterlockedCompareExchange(&g_dump_busy, 1, 0) != 0) return path;
    __try { path = crash_report_write_dump_locked(ep); }
    __finally { InterlockedExchange(&g_dump_busy, 0); }
    return path;
}

/* Capture fatal details; callers guard this attempt with SEH. */
static void crash_capture_fatal_locked(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *rip  = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t fa = (ep->ExceptionRecord->NumberParameters >= 2)
                     ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    uintptr_t rva = 0;
    const char *mod = "?";
    char modbuf[80];
    const char *dump;

    /* module + RVA of the faulting site (loader-lock cost acceptable: the process is dying). */
    {
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &hm) && hm) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hm, path, MAX_PATH)) {
                const char *b = strrchr(path, '\\');
                _snprintf_s(modbuf, sizeof modbuf, _TRUNCATE, "%s", b ? b + 1 : path);
                mod = modbuf;
            }
            rva = (uintptr_t)rip - (uintptr_t)hm;
        }
    }
    g_rec_stack[0] = '\0';
    if (code != EXCEPTION_STACK_OVERFLOW)   /* never walk an exhausted stack */
        shield_capture_stack(ep->ContextRecord, g_rec_stack, sizeof g_rec_stack, 24);
    g_rec_text[0] = '\0';
    shield_last_engine_msg(g_rec_text, sizeof g_rec_text);
    dump = crash_report_write_dump(ep);
    crash_report_file("fatal", code, rva, fa, mod, g_rec_stack, g_rec_text, dump);
}

static void crash_capture_fatal(EXCEPTION_POINTERS *ep)
{
    if (InterlockedCompareExchange(&g_capture_busy, 1, 0) != 0) return;
    __try { crash_capture_fatal_locked(ep); }
    __finally { InterlockedExchange(&g_capture_busy, 0); }
}

/* Best-effort first-chance capture when a fatal status reaches the VEH. */
static LONG CALLBACK crash_fatal_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == 0xC0000374 || code == 0xC0000409) &&
        InterlockedExchange(&g_fatal_oneshot, 1) == 0) {
        __try { crash_capture_fatal(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Record an unhandled fault, then chain to the previous filter without recursion. */
static LONG WINAPI crash_uef(EXCEPTION_POINTERS *ep)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev_filter;
    if (InterlockedIncrement(&g_uef_entered) == 1) {
        __try { crash_capture_fatal(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (prev && prev != crash_uef) return prev(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Reassert through engine startup for about 30 seconds. Keep the original previous
 * filter; capturing this call's return could make the chain recurse into itself. */
static DWORD WINAPI crash_reassert_thread(LPVOID p)
{
    int i;
    (void)p;
    for (i = 0; i < 20; i++) { Sleep(1500); SetUnhandledExceptionFilter(crash_uef); }
    return 0;
}

void crash_report_init(void)
{
    HMODULE dh;
    crash_dirs_from_module();
    crash_read_version();
    /* Snapshot the renderer before faults can occur: its first lookup may take the
     * loader lock. An unresolved renderer stays empty in the record. */
    g_renderer = sh_host_renderer_name();
    dh = LoadLibraryA("dbghelp.dll");
    if (dh) g_minidump_write = (minidump_write_t)GetProcAddress(dh, "MiniDumpWriteDump");
}

void crash_report_arm_fatal_handlers(void)
{
    HANDLE t;
    if (g_crash_dir[0] == '\0') return;   /* init failed -> stay dark rather than half-armed */
    AddVectoredExceptionHandler(1 /* first-in-chain */, crash_fatal_veh);
    g_prev_filter = SetUnhandledExceptionFilter(crash_uef);
    if (g_prev_filter == crash_uef) g_prev_filter = NULL;   /* belt: never chain to ourselves */
    t = CreateThread(NULL, 0, crash_reassert_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    {
        shield_fault f = { "crash-record", -1, "fatal-path crash capture armed", 0, 0 };
        shield_emit(&f);
    }
}
