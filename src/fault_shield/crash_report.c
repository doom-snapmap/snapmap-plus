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
#include "veh.h"         /* shield_capture_stack, shield_second_chance */
#include "engine_layout.h"
#include "../backend/host_image.h"   /* sh_host_renderer_name -- snapshotted at init, never at fault time */
#include "../common/wide_path.h"

/* A release build's tag, stamped by build.ps1 -Version; a local build is "dev". */
#ifndef SNAPMAP_PLUS_VERSION
#define SNAPMAP_PLUS_VERSION "dev"
#endif

const char *sh_product_version(void) { return SNAPMAP_PLUS_VERSION; }

extern uint8_t *g_doom_base;

#define CRASH_MAX_RECORDS 8   /* per-session cap: a fault storm can't spam pending-*.json */

typedef BOOL (WINAPI *minidump_write_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                        PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);

/* Wide paths of any length, built at init; a record's file name is built into
 * static storage at fault time. */
static wchar_t g_crash_dir[SH_WPATH_MAX];    /* <game>\snapmap-plus\crash */
static wchar_t g_dump_path[SH_WPATH_MAX];    /* <game>\snapmap-plus\logs\sh_crash.dmp */
static char    g_dump_utf8[3 * SH_WPATH_MAX]; /* the same, as the record names it */
static wchar_t g_record_path[SH_WPATH_MAX], g_temporary_path[SH_WPATH_MAX];
static const char *g_renderer     = "";    /* "vulkan"/"opengl" at arm time (see crash_report_init) */
static char g_game[SH_GAME_IDENTITY_MAX] = {0};   /* game identity at arm time, never at fault time */
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

/* <game>\snapmap-plus\crash, and the dump path under <game>\snapmap-plus\logs. */
static void crash_dirs_from_module(void)
{
    HMODULE self = NULL;
    g_crash_dir[0] = g_dump_path[0] = L'\0';
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&crash_dirs_from_module, &self) || !self) return;
    if (!sh_wpath_module_dir(self, g_crash_dir, SH_WPATH_MAX) ||
        !sh_wpath_append(g_crash_dir, SH_WPATH_MAX, L"snapmap-plus") || !sh_wpath_ensure_dir(g_crash_dir))
        goto fail;
    wcscpy_s(g_dump_path, SH_WPATH_MAX, g_crash_dir);
    if (!sh_wpath_append(g_dump_path, SH_WPATH_MAX, L"logs") || !sh_wpath_ensure_dir(g_dump_path) ||
        !sh_wpath_append(g_dump_path, SH_WPATH_MAX, L"sh_crash.dmp") ||
        !sh_wpath_utf8(g_dump_path, g_dump_utf8, sizeof g_dump_utf8))
        g_dump_path[0] = L'\0';
    if (!sh_wpath_append(g_crash_dir, SH_WPATH_MAX, L"crash") || !sh_wpath_ensure_dir(g_crash_dir))
        goto fail;
    return;
fail:
    g_crash_dir[0] = g_dump_path[0] = L'\0';
}

static void crash_report_file_locked(const char *kind, unsigned long code, uintptr_t rip_rva,
                       uintptr_t fault_addr, const char *module_name,
                       const char *stack, const char *engine_text, const char *dump_path)
{
    crash_record r;
    SYSTEMTIME st;
    char tbuf[32];
    int len, named, tryn;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (g_crash_dir[0] == L'\0') return;
    if (InterlockedIncrement(&g_records_written) > CRASH_MAX_RECORDS) return;

    GetLocalTime(&st);
    _snprintf_s(tbuf, sizeof tbuf, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    r.kind = kind; r.code = code;
    r.rip_rva = (unsigned long long)rip_rva; r.fault_addr = (unsigned long long)fault_addr;
    r.module = module_name; r.stack = stack; r.engine_text = engine_text;
    r.dump = dump_path; r.version = sh_product_version(); r.time = tbuf;
    r.renderer = g_renderer;
    r.game = g_game;
    len = crash_record_json(g_rec_buf, sizeof g_rec_buf, &r);
    if (len <= 0) return;

    /* CREATE_NEW preserves same-second records; the UI orders collision suffixes numerically. */
    for (tryn = 0; tryn < CRASH_MAX_RECORDS; tryn++) {
        if (tryn == 0)
            named = _snwprintf_s(g_record_path, SH_WPATH_MAX, _TRUNCATE, L"%s\\pending-%04d%02d%02d-%02d%02d%02d.json",
                               g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            named = _snwprintf_s(g_record_path, SH_WPATH_MAX, _TRUNCATE, L"%s\\pending-%04d%02d%02d-%02d%02d%02d-%d.json",
                               g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tryn);
        if (named < 0 || _snwprintf_s(g_temporary_path, SH_WPATH_MAX, _TRUNCATE, L"%s.tmp", g_record_path) < 0) return;
        if (GetFileAttributesW(g_record_path) != INVALID_FILE_ATTRIBUTES) continue;
        h = CreateFileW(g_temporary_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return;
    }
    if (h == INVALID_HANDLE_VALUE) return;
    {
        DWORD wrote = 0;
        BOOL ok = WriteFile(h, g_rec_buf, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
        CloseHandle(h);
        if (!ok || !MoveFileExW(g_temporary_path, g_record_path, MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(g_temporary_path);
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
    if (!g_minidump_write || g_dump_path[0] == L'\0') return "";
    h = CreateFileW(g_dump_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
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
    return ok ? g_dump_utf8 : "";
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
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &hm) && hm) {
            static wchar_t path[SH_WPATH_MAX];
            DWORD n = GetModuleFileNameW(hm, path, SH_WPATH_MAX);
            if (n && n < SH_WPATH_MAX) {
                const wchar_t *b = wcsrchr(path, L'\\');
                if (WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : path, -1, modbuf, sizeof modbuf, NULL, NULL) > 0)
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

/* Record an unhandled fault, then hand it to the filter this one replaced. */
static LONG WINAPI crash_uef(EXCEPTION_POINTERS *ep)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev_filter;
    LONG redirected = EXCEPTION_CONTINUE_SEARCH;
    /* A hardware fault no engine handler took may still be recoverable (veh.h). */
    __try { redirected = shield_second_chance(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (redirected == EXCEPTION_CONTINUE_EXECUTION) return redirected;
    if (InterlockedIncrement(&g_uef_entered) == 1) {
        __try { crash_capture_fatal(ep); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (prev) return prev(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_report_init(void)
{
    HMODULE dh;
    crash_dirs_from_module();
    /* Snapshot the renderer before faults can occur: its first lookup may take the
     * loader lock. An unresolved renderer stays empty in the record. */
    g_renderer = sh_host_renderer_name();
    /* The game identity hashes the executable once; do it here, never in a handler. */
    sh_host_game_identity(g_game, sizeof g_game);
    dh = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dh) g_minidump_write = (minidump_write_t)GetProcAddress(dh, "MiniDumpWriteDump");
}

/* The filter is set once and chains to the one it replaced. A filter another
 * component sets later replaces this one, as Windows intends; the fatal VEH
 * above still records heap corruption and stack-cookie failures. */
void crash_report_arm_fatal_handlers(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev;
    if (g_crash_dir[0] == L'\0') return;   /* init failed -> stay dark rather than half-armed */
    AddVectoredExceptionHandler(1 /* first-in-chain */, crash_fatal_veh);
    prev = SetUnhandledExceptionFilter(crash_uef);
    if (prev != crash_uef) g_prev_filter = prev;
    {
        shield_fault f = { "crash-record", -1, "fatal-path crash capture armed", 0, 0 };
        shield_emit(&f);
    }
}
