/* Diagnostic-build crash and environment logger (SH_DIAG only).
 * Handlers preserve exception disposition. Normal first-chance capture keeps
 * raw breadcrumbs; fatal statuses that reach the VEH and unhandled faults also
 * attempt module, stack, and dump capture. Stack overflow skips the stack walk.
 * Direct fail-fast termination can bypass both handlers, so absent records do
 * not prove a clean exit. Logs and dumps live under snapmap-plus\logs. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <dbghelp.h>
#include "../backend/host_image.h"
#include "../common/wide_path.h"

#pragma comment(lib, "shell32.lib")   /* SHGetKnownFolderPath */
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dbghelp.lib")   /* MiniDumpWriteDump */

static wchar_t g_dir[SH_WPATH_MAX];       /* dir of this DLL (the DOOM root), long form */
static char    g_dir_utf8[3 * SH_WPATH_MAX];
static wchar_t g_logpath[SH_WPATH_MAX];   /* <DOOM>\snapmap-plus\logs\sh_diag.log */
static volatile LONG g_installed   = 0;
static volatile LONG g_cxx_logged  = 0;    /* rate-limit first-chance C++-throw logging */
static volatile LONG g_fc_logged   = 0;    /* rate-limit first-chance crash-class logging */
static volatile LONG g_in_uef      = 0;    /* UEF re-entrancy / crash-seen latch */
static volatile LONG g_mods_dumped = 0;    /* one-shot: module table already dumped on a crash */
static volatile LONG g_so_logged   = 0;    /* one-shot stack-overflow marker */
static volatile LONG  g_fatal_captured = 0; /* one-shot: full capture done for a UEF-bypassing fatal code */
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_uef = NULL;   /* captured ONCE at install; never our own */

/* last crash-class exception the first-chance VEH saw (breadcrumb for the displaced-UEF / fast-fail
 * case where the UEF never fires). Plain scalars -- written without locks, read at detach. */
static volatile DWORD     g_last_code = 0;
static volatile ULONG_PTR g_last_rip  = 0;
static volatile ULONG_PTR g_last_fault = 0;

/* Independent write-through logger with smaller buffers for crash contexts. */
static void diag_logv(const char *fmt, va_list ap)
{
    char body[600], line[700];
    SYSTEMTIME st;
    int n;
    _vsnprintf_s(body, sizeof body, _TRUNCATE, fmt, ap);
    GetLocalTime(&st);
    n = _snprintf_s(line, sizeof line, _TRUNCATE,
        "[diag] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    (void)n;
    OutputDebugStringA(line);
    if (g_logpath[0]) {
        HANDLE h = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteFile(h, line, (DWORD)strlen(line), &wrote, NULL);
            CloseHandle(h);
        }
    }
}
static void diag_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); diag_logv(fmt, ap); va_end(ap);
}

/* Raw first-chance breadcrumb without module lookup or stack walking. */
static void diag_raw(const char *prefix, DWORD code, const void *rip, const void *fault)
{
    char buf[160];
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE,
                        "[diag] %s code=0x%08lx rip=%p fault=%p\r\n",
                        prefix, (unsigned long)code, rip, fault);
    (void)n;
    OutputDebugStringA(buf);
    if (g_logpath[0]) {
        HANDLE h = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL); CloseHandle(h); }
    }
}

/* Resolve module+offset for full fatal capture or the environment thread.
 * This can take the loader lock; ordinary first-chance breadcrumbs avoid it. */
static void module_for(const void *addr, char *out, size_t cap, uintptr_t *off)
{
    HMODULE hmod = NULL;
    if (off) *off = 0;
    if (out && cap) out[0] = '\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &hmod) && hmod) {
        static wchar_t path[SH_WPATH_MAX];
        DWORD n = GetModuleFileNameW(hmod, path, SH_WPATH_MAX);
        if (n && n < SH_WPATH_MAX && out && cap) {
            const wchar_t *base = wcsrchr(path, L'\\');
            sh_wpath_utf8(base ? base + 1 : path, out, cap);
        }
        if (off) *off = (uintptr_t)addr - (uintptr_t)hmod;
    } else {
        if (out && cap) strncpy_s(out, cap, "???", _TRUNCATE);
    }
}

static const char *code_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/__fastfail";
    case 0xC0000374:                      return "HEAP_CORRUPTION";
    case 0xC0000008:                      return "INVALID_HANDLE";
    case 0xC000041D:                      return "FATAL_USER_CALLBACK_EXCEPTION";
    case 0xE06D7363:                      return "C++ exception (throw)";
    default:                              return "";
    }
}

/* A severity-ERROR status (0xCxxxxxxx) is a crash-class exception we record. */
static int is_crash_class(DWORD code) { return (code & 0xF0000000u) == 0xC0000000u; }

/* --------------------------------------------------- environment dump (modules + folders) -------- */
static void dump_modules(void)
{
    HANDLE snap = INVALID_HANDLE_VALUE;
    MODULEENTRY32W me;
    char name[3 * MAX_MODULE_NAME32 + 3];
    int count = 0, tries;
    for (tries = 0; tries < 6; tries++) {                 /* retry ERROR_BAD_LENGTH (loading modules) */
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(50);
    }
    if (snap == INVALID_HANDLE_VALUE) { diag_log("modules: snapshot failed (err=%lu)", GetLastError()); return; }
    me.dwSize = sizeof me;
    diag_log("==== LOADED MODULES (name  base  size) ====");
    if (Module32FirstW(snap, &me)) {
        do {
            sh_wpath_utf8(me.szModule, name, sizeof name);
            diag_log("  %-28s %p  %lu KB", name, me.modBaseAddr, me.modBaseSize / 1024);
            count++;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    diag_log("==== %d modules ====", count);
}

/* Capture the module table once, including crashes before the timed environment dump. */
static void dump_modules_once(void)
{
    if (InterlockedExchange(&g_mods_dumped, 1) == 0) {
        diag_log("---- module table at crash ----");
        dump_modules();
    }
}

static wchar_t g_scratch[SH_WPATH_MAX];
static char    g_name_utf8[3 * MAX_PATH], g_path_utf8[3 * SH_WPATH_MAX];

static void dump_dir(const char *label, const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    sh_wpath_utf8(dir, g_path_utf8, sizeof g_path_utf8);
    diag_log("---- %s: %s ----", label, g_path_utf8);
    wcscpy_s(g_scratch, SH_WPATH_MAX, dir);
    if (!sh_wpath_append(g_scratch, SH_WPATH_MAX, L"*")) return;
    h = FindFirstFileW(g_scratch, &fd);
    if (h == INVALID_HANDLE_VALUE) { diag_log("   (not present / unreadable, err=%lu)", GetLastError()); return; }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        sh_wpath_utf8(fd.cFileName, g_name_utf8, sizeof g_name_utf8);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) diag_log("   [dir]  %s", g_name_utf8);
        else diag_log("   %10llu  %s", ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, g_name_utf8);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    diag_log("   (%d entries)", n);
}

static void dump_env(void)
{
    static wchar_t sub[SH_WPATH_MAX];
    if (g_dir[0]) {
        dump_dir("DOOM dir", g_dir);
        wcscpy_s(sub, SH_WPATH_MAX, g_dir);
        if (sh_wpath_append(sub, SH_WPATH_MAX, L"snapmap-plus")) dump_dir("DOOM\\snapmap-plus (our bundle)", sub);
    }
    if (sh_wpath_local_appdata(sub, SH_WPATH_MAX) && sh_wpath_append(sub, SH_WPATH_MAX, L"snapmap-plus")) {
        dump_dir("LOCALAPPDATA\\snapmap-plus (user data)", sub);
        if (sh_wpath_append(sub, SH_WPATH_MAX, L"prefabs")) dump_dir("LOCALAPPDATA\\snapmap-plus\\prefabs", sub);
    }
    dump_modules();
}

/* Full fatal capture. */
static void dump_registers(const CONTEXT *c)
{
    char m[64]; uintptr_t off;
    module_for((void *)c->Rip, m, sizeof m, &off);
    diag_log("  RIP=%p (%s+0x%llx)  RSP=%p  RBP=%p", (void *)c->Rip, m, (unsigned long long)off, (void *)c->Rsp, (void *)c->Rbp);
    diag_log("  RAX=%p RBX=%p RCX=%p RDX=%p", (void *)c->Rax, (void *)c->Rbx, (void *)c->Rcx, (void *)c->Rdx);
    diag_log("  RSI=%p RDI=%p R8 =%p R9 =%p", (void *)c->Rsi, (void *)c->Rdi, (void *)c->R8,  (void *)c->R9);
    diag_log("  R10=%p R11=%p R12=%p R13=%p", (void *)c->R10, (void *)c->R11, (void *)c->R12, (void *)c->R13);
    diag_log("  R14=%p R15=%p", (void *)c->R14, (void *)c->R15);
}

/* Walk a context copy with the OS unwinder and guarded reads. Module lookup
 * may take the loader lock, so this is limited to full fatal capture. */
static void stack_walk(const CONTEXT *start, int maxframes)
{
    CONTEXT c = *start;
    int i;
    diag_log("  ---- stack (return addresses, top first) ----");
    for (i = 0; i < maxframes; i++) {
        char m[64]; uintptr_t off;
        DWORD64 imageBase = 0; PRUNTIME_FUNCTION fn;
        module_for((void *)c.Rip, m, sizeof m, &off);
        diag_log("   #%-2d %p  %s+0x%llx", i, (void *)c.Rip, m, (unsigned long long)off);
        if (c.Rip == 0) break;
        if (c.Rsp & 7) { diag_log("   (stopped: misaligned RSP)"); break; }
        __try {
            fn = RtlLookupFunctionEntry((DWORD64)c.Rip, &imageBase, NULL);
            if (fn) { PVOID hd = NULL; DWORD64 est = 0;
                      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)c.Rip, fn, &c, &hd, &est, NULL); }
            else    { if (c.Rsp == 0) break; c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { diag_log("   (stack walk stopped: unwind faulted at frame %d)", i); break; }
        if (c.Rip == 0) break;
    }
}

static void write_minidump(PEXCEPTION_POINTERS ep)
{
    static wchar_t path[SH_WPATH_MAX];
    HANDLE h;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    if (!g_dir[0]) return;
    wcscpy_s(path, SH_WPATH_MAX, g_dir);
    if (!sh_wpath_append(path, SH_WPATH_MAX, L"snapmap-plus\\logs\\sh_crash.dmp")) return;
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { diag_log("minidump: CreateFile failed (err=%lu)", GetLastError()); return; }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
            &mei, NULL, NULL))
        diag_log("minidump: wrote snapmap-plus\\logs\\sh_crash.dmp");
    else
        diag_log("minidump: MiniDumpWriteDump failed (err=%lu)", GetLastError());
    CloseHandle(h);
}

static void log_fault_full(const char *tag, PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    char m[64]; uintptr_t off;
    const char *nm = code_name(code);
    module_for(er->ExceptionAddress, m, sizeof m, &off);
    diag_log("==== %s: code=0x%08lx %s at %p (%s+0x%llx) ====",
             tag, (unsigned long)code, nm[0] ? nm : "(other)", er->ExceptionAddress, m, (unsigned long long)off);
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char *op = er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read";
        diag_log("  AV: %s of address %p", op, (void *)er->ExceptionInformation[1]);
    }
    dump_registers(ep->ContextRecord);
    stack_walk(ep->ContextRecord, 32);
}

/* ------------------------------------------------------------- the handlers -------------------- */

/* Keep ordinary first-chance capture small. Fatal statuses below also attempt
 * full capture if they reach this handler. Always continue exception search. */
static LONG CALLBACK diag_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *rip  = (void *)ep->ContextRecord->Rip;

    if (code == EXCEPTION_STACK_OVERFLOW) {               /* run NOTHING heavy on an exhausted stack */
        if (InterlockedExchange(&g_so_logged, 1) == 0)
            OutputDebugStringA("[diag] STACK_OVERFLOW first-chance (see UEF/minidump)\r\n");
        g_last_code = code; g_last_rip = (ULONG_PTR)rip; g_last_fault = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xE06D7363) {                              /* C++ throw: common; keep the last few */
        g_last_code = code; g_last_rip = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
        if (InterlockedIncrement(&g_cxx_logged) <= 15)
            diag_raw("first-chance C++ throw", code, ep->ExceptionRecord->ExceptionAddress, NULL);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!is_crash_class(code)) return EXCEPTION_CONTINUE_SEARCH;   /* breakpoints, debug-print: ignore */

    /* crash-class: breadcrumb (for the displaced-UEF case) + a raw line (no module/stack work). */
    g_last_code = code; g_last_rip = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    g_last_fault = (ep->ExceptionRecord->NumberParameters >= 2) ? ep->ExceptionRecord->ExceptionInformation[1] : 0;

    /* Some fatal paths bypass the unhandled filter. If their status reaches this
     * VEH, attempt full capture once. Direct fail-fast can bypass the VEH too. */
    if ((code == 0xC0000374 || code == 0xC0000409) && InterlockedExchange(&g_fatal_captured, 1) == 0) {
        diag_log("############ FATAL first-chance 0x%08lx (bypasses the UEF) -- full capture here ############",
                 (unsigned long)code);
        dump_modules_once();
        log_fault_full("FATAL-FIRST-CHANCE", ep);
        write_minidump(ep);
        diag_log("##############################################################################");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedIncrement(&g_fc_logged) <= 40)
        diag_raw("first-chance crash-class", code, ep->ExceptionRecord->ExceptionAddress, (void *)g_last_fault);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Capture an unhandled fault once, then chain to the original previous filter. */
static LONG WINAPI diag_uef(PEXCEPTION_POINTERS ep)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev_uef;
    if (InterlockedIncrement(&g_in_uef) == 1) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        diag_log("################ UNHANDLED EXCEPTION (process terminating) ################");
        if (code == EXCEPTION_STACK_OVERFLOW) {
            /* minimal on the exhausted stack: code+addr only, the minidump carries the rest. */
            diag_raw("UNHANDLED STACK_OVERFLOW", code, ep->ExceptionRecord->ExceptionAddress, NULL);
        } else {
            dump_modules_once();
            log_fault_full("UNHANDLED", ep);
        }
        write_minidump(ep);
        diag_log("##########################################################################");
    }
    if (prev && prev != diag_uef) return prev(ep);   /* hand off to engine/OS WER -- never to ourselves */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Dump the environment outside the loader lock. */
static DWORD WINAPI env_thread(LPVOID p)
{
    (void)p;
    diag_log("===================================================================");
    diag_log("SNAPMAP+ DIAGNOSTIC BUILD -- catch-all crash + environment logger");
    diag_log("  pid=%lu  dir=%s", GetCurrentProcessId(), g_dir_utf8);
    {
        /* The same game identity the reports carry (hashing runs off the loader lock). */
        char game[SH_GAME_IDENTITY_MAX];
        if (sh_host_game_identity(game, sizeof game)) diag_log("  game: %s", game);
        else diag_log("  game: unsupported executable %s (%s)", sh_host_image_name(),
                      sh_host_executable_hash8()[0] ? sh_host_executable_hash8() : "unhashed");
    }
    diag_log("  Reproduce the crash, then send sh_diag.log AND sh_crash.dmp.");
    diag_log("===================================================================");

    Sleep(800);
    diag_log("======== ENVIRONMENT DUMP #1 (boot) ========");
    dump_env();


    diag_log("======== ENVIRONMENT DUMP #2 (editor should be up) ========");
    dump_modules();

    /* Opt-in crash test: a diag_selftest_crash file beside the DLL triggers a
     * null write after startup. Remove the sentinel when the test is complete. */
    {
        static wchar_t sentinel[SH_WPATH_MAX];
        wcscpy_s(sentinel, SH_WPATH_MAX, g_dir);
        if (g_dir[0] && sh_wpath_append(sentinel, SH_WPATH_MAX, L"diag_selftest_crash") &&
            GetFileAttributesW(sentinel) != INVALID_FILE_ATTRIBUTES) {
            diag_log("DIAG SELF-TEST: sentinel present -> deliberately faulting (NULL write) to exercise the crash path");
            Sleep(200);
            *(volatile int *)0 = (int)0xDEAD;   /* NULL write -> AV -> unhandled -> diag_uef + minidump */
        }
    }
    return 0;
}

void shield_diag_install(HINSTANCE self)
{
    HANDLE h;
    if (InterlockedExchange(&g_installed, 1)) return;

    /* The DLL directory (the DOOM root, kept for the environment dump); the
     * diag log and crash dump go under <DOOM>\snapmap-plus\logs\. */
    g_logpath[0] = L'\0';
    if (sh_wpath_module_dir((void *)self, g_dir, SH_WPATH_MAX)) {
        sh_wpath_utf8(g_dir, g_dir_utf8, sizeof g_dir_utf8);
        wcscpy_s(g_logpath, SH_WPATH_MAX, g_dir);
        if (!sh_wpath_append(g_logpath, SH_WPATH_MAX, L"snapmap-plus") || !sh_wpath_ensure_dir(g_logpath) ||
            !sh_wpath_append(g_logpath, SH_WPATH_MAX, L"logs") || !sh_wpath_ensure_dir(g_logpath) ||
            !sh_wpath_append(g_logpath, SH_WPATH_MAX, L"sh_diag.log"))
            g_logpath[0] = L'\0';
    }

    /* Set the filter once; it chains to the filter it replaced. */
    AddVectoredExceptionHandler(1 /* first-in-chain */, diag_veh);
    g_prev_uef = SetUnhandledExceptionFilter(diag_uef);
    if (g_prev_uef == diag_uef) g_prev_uef = NULL;   /* belt: never chain to ourselves */

    /* Write an early arming marker before the environment thread starts. */
    h = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const char *m = "[diag] armed (DllMain); banner + env dump follow on the worker thread\r\n";
        DWORD w; WriteFile(h, m, (DWORD)strlen(m), &w, NULL); CloseHandle(h);
    }

    /* Heavy work (banner I/O, module and folder listing) off the loader lock. */
    { HANDLE t = CreateThread(NULL, 0, env_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
}

void shield_diag_detach(void)
{
    if (g_in_uef) {
        diag_log("DLL_PROCESS_DETACH after an unhandled exception => CRASH (see the UNHANDLED block + sh_crash.dmp)");
    } else if (g_last_code) {
        /* A first-chance breadcrumb without a filter record is inconclusive: a
         * handler may have caught it, the filter may be displaced, or termination
         * may have bypassed the filter. */
        diag_log("DLL_PROCESS_DETACH: no UEF fired, but last first-chance crash-class was code=0x%08lx rip=0x%llx fault=0x%llx "
                 "(if DOOM crashed, suspect a __fastfail/heap-stop that bypasses the filter -- also grab %%LOCALAPPDATA%%\\CrashDumps\\*.dmp)",
                 (unsigned long)g_last_code, (unsigned long long)g_last_rip, (unsigned long long)g_last_fault);
    } else {
        diag_log("DLL_PROCESS_DETACH -- process exiting, no crash-class fault seen => clean exit/quit");
    }
}
