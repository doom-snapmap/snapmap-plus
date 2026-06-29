/* shield_diag.c -- DIAGNOSTIC build: a catch-all crash + environment logger. See shield_diag.h.
 *
 * WHY THIS EXISTS: a remote end-user's DOOM crashes where the author's does not (works on our box).
 * The backend init completes cleanly in their log, and the recovery shield's VEH (veh.c) deliberately
 * EARLY-OUTS on any fault whose RIP is not inside DOOMx64vk.exe -- it is an in-editor draw-fault
 * recovery handler, not a logger. So a crash in OUR DLL, in Qt, in non-frame engine code, or a fault
 * the shield does not recover leaves NO trace. This module records where the process dies + what was
 * loaded, to snaphak_diag.log (text) and snaphak_crash.dmp (a minidump) next to the DLL.
 *
 * SAFETY (this ships to a stranger -- it must NEVER make the crash worse or hide it):
 *  - Every handler is LOG-ONLY and returns EXCEPTION_CONTINUE_SEARCH; it never edits the CONTEXT.
 *  - The FIRST-CHANCE VEH is loader-lock-safe + stack-light: it records a RAW breadcrumb (code/RIP/
 *    fault, NO module lookup, NO stack walk) so it cannot deadlock on the loader lock and cannot
 *    re-fault a near-exhausted stack. The HEAVY work (module resolution, stack walk, module table,
 *    minidump) runs in the UNHANDLED-exception filter, when the process is already terminating.
 *  - STACK_OVERFLOW is special-cased to a single tiny static write (no big buffers, no walk).
 *  - The UEF chains to the REAL previous top-level filter, guarded so it can NEVER chain to itself
 *    (the bug a review caught: a re-assert that captured our own filter -> infinite self-recursion).
 *
 * COVERAGE LIMIT (documented honestly): a __fastfail / STATUS_STACK_BUFFER_OVERRUN (0xC0000409, e.g.
 * a /GS stack-cookie failure or std::terminate) and many heap-corruption stops (0xC0000374 raised via
 * RtlFailFast) trap straight to the kernel -> WER, bypassing BOTH the VEH and the UEF. Those leave no
 * crash block here. The breadcrumb + the "UEF did not fire" detach line let us INFER that class, and
 * the README tells the user to also grab any %LOCALAPPDATA%\CrashDumps\*.dmp.
 *
 * Compiled ONLY into the diagnostic DLL (build.ps1 -Diag => /DSNAPHAK_DIAG); NOT in the shipped backend.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <dbghelp.h>

#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#pragma comment(lib, "dbghelp.lib")   /* MiniDumpWriteDump */

static char  g_dir[MAX_PATH]      = {0};   /* dir of this DLL (the DOOM root) */
static char  g_logpath[MAX_PATH]  = {0};   /* g_dir\snaphak_diag.log */
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

/* ---------------------------------------------------------------- the diag logger ----------------
 * Independent of backend_log. Smaller stack buffers than a normal logger (a crash handler may run on
 * a tight stack). FILE_FLAG_WRITE_THROUGH so the final lines survive a hard process kill. */
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
        HANDLE h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
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

/* A pre-formatted, stack-light, loader-lock-free raw line for the FIRST-CHANCE path -- no module
 * lookup, no GetLocalTime formatting churn beyond a tiny buffer. Safe on a near-exhausted stack. */
static void diag_raw(const char *prefix, DWORD code, const void *rip, const void *fault)
{
    char buf[160];
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE,
                        "[diag] %s code=0x%08lx rip=%p fault=%p\r\n",
                        prefix, (unsigned long)code, rip, fault);
    (void)n;
    OutputDebugStringA(buf);
    if (g_logpath[0]) {
        HANDLE h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL); CloseHandle(h); }
    }
}

/* Resolve an address to "module.dll+0xNNNN". Takes the loader lock -> ONLY call from the UEF / the
 * off-lock env thread, never from the first-chance VEH. Never faults. */
static void module_for(const void *addr, char *out, size_t cap, uintptr_t *off)
{
    HMODULE hmod = NULL;
    if (off) *off = 0;
    if (out && cap) out[0] = '\0';
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hmod) && hmod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hmod, path, MAX_PATH)) {
            const char *base = strrchr(path, '\\');
            base = base ? base + 1 : path;
            if (out && cap) strncpy_s(out, cap, base, _TRUNCATE);
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
    MODULEENTRY32 me;
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
    if (Module32First(snap, &me)) {
        do { diag_log("  %-28s %p  %lu KB", me.szModule, me.modBaseAddr, me.modBaseSize / 1024); count++; }
        while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    diag_log("==== %d modules ====", count);
}

/* Dump the module table at most once on the crash path (so EVERY captured crash carries it even if the
 * timed env dumps never ran -- e.g. an early crash). */
static void dump_modules_once(void)
{
    if (InterlockedExchange(&g_mods_dumped, 1) == 0) {
        diag_log("---- module table at crash ----");
        dump_modules();
    }
}

static void dump_dir(const char *label, const char *dir)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    diag_log("---- %s: %s ----", label, dir);
    if (h == INVALID_HANDLE_VALUE) { diag_log("   (not present / unreadable, err=%lu)", GetLastError()); return; }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) diag_log("   [dir]  %s", fd.cFileName);
        else diag_log("   %10llu  %s", ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    diag_log("   (%d entries)", n);
}

static void dump_env(void)
{
    char profile[MAX_PATH], sub[MAX_PATH];
    if (g_dir[0]) {
        dump_dir("DOOM dir", g_dir);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\snaphak", g_dir);
        dump_dir("DOOM\\snaphak (our bundle)", sub);
    }
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile))) {
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\snaphak", profile);          dump_dir("USERPROFILE\\snaphak (user data)", sub);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\snaphak\\strings", profile);  dump_dir("USERPROFILE\\snaphak\\strings", sub);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\snaphak\\overrides", profile);dump_dir("USERPROFILE\\snaphak\\overrides", sub);
    }
    dump_modules();
}

/* ---------------------------------------------------- the HEAVY crash dump (UEF only) ------------- */
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

/* Module-level stack walk via the OS unwinder (no dbghelp). Logs each frame as module+offset. Walks a
 * COPY; every read SEH-guarded. Only from the UEF (loader-lock-safe enough: process is terminating). */
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
    char path[MAX_PATH];
    HANDLE h;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    if (!g_dir[0]) return;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snaphak_crash.dmp", g_dir);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { diag_log("minidump: CreateFile failed (err=%lu)", GetLastError()); return; }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
            &mei, NULL, NULL))
        diag_log("minidump: wrote %s", path);
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

/* FIRST-CHANCE VEH: stack-light + loader-lock-free. Records a breadcrumb + a raw line; NEVER does
 * module resolution or a stack walk here (those take the loader lock and burn stack). The heavy dump
 * is the UEF's job. Always CONTINUE_SEARCH. */
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

    /* FATAL codes that reach the VEH first-chance but NEVER reach the unhandled-exception filter on x64:
     * HEAP_CORRUPTION (0xC0000374, raised via RtlReportCriticalFailure -> RtlFailFast) and __fastfail /
     * STACK_BUFFER_OVERRUN (0xC0000409). The process fast-fails to the kernel, so the UEF (our full dump +
     * minidump) never runs. Capture the FULL record HERE, ONCE -- the process is dying anyway, so the
     * loader-lock cost of the module table + stack walk + minidump is acceptable. The stack walk is
     * heap-free (RtlVirtualUnwind), so it survives even a corrupt heap; the minidump is best-effort after. */
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

/* UNHANDLED-exception filter: the definitive "process dying HERE" record. Runs when the process is
 * already terminating, so the loader lock + a full dump are acceptable. Re-entrancy-guarded; chains to
 * the REAL previous filter (guarded so it can never be us -> no self-recursion). */
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

/* Off the loader lock: emit the banner, dump the environment twice (boot + editor-up), and RE-ASSERT
 * the UEF frequently for the first ~30s (the engine may install its own filter during bring-up).
 * CRITICAL: the re-assert must NOT capture our own filter back into g_prev_uef (that was the recursion
 * bug) -- we discard SetUnhandledExceptionFilter's return here. */
static DWORD WINAPI env_thread(LPVOID p)
{
    int i;
    (void)p;
    diag_log("===================================================================");
    diag_log("SNAPHAK DIAGNOSTIC BUILD -- catch-all crash + environment logger");
    diag_log("  pid=%lu  dir=%s", GetCurrentProcessId(), g_dir);
    diag_log("  Reproduce the crash, then send snaphak_diag.log AND snaphak_crash.dmp.");
    diag_log("===================================================================");

    Sleep(800);
    diag_log("======== ENVIRONMENT DUMP #1 (boot) ========");
    dump_env();

    /* re-assert our UEF every ~1.5s for ~30s -- editor bring-up is exactly when the engine is likely
     * to install its own top-level filter and displace ours. Discard the return (never reassign). */
    for (i = 0; i < 20; i++) { SetUnhandledExceptionFilter(diag_uef); Sleep(1500); }

    diag_log("======== ENVIRONMENT DUMP #2 (editor should be up) ========");
    dump_modules();
    SetUnhandledExceptionFilter(diag_uef);

    /* DIAG SELF-TEST (opt-in): if a sentinel file sits next to the DLL, deliberately fault HERE -- post
     * init, like the end-user's crash -- so we can verify the crash-capture path (the UNHANDLED block +
     * the minidump + the no-self-recursion fix) end-to-end on a box that does not otherwise crash. It
     * NEVER fires for a real user (there is no sentinel in a shipped layout). Delete the sentinel after. */
    {
        char sentinel[MAX_PATH];
        _snprintf_s(sentinel, sizeof sentinel, _TRUNCATE, "%s\\diag_selftest_crash", g_dir);
        if (g_dir[0] && GetFileAttributesA(sentinel) != INVALID_FILE_ATTRIBUTES) {
            diag_log("DIAG SELF-TEST: sentinel present -> deliberately faulting (NULL write) to exercise the crash path");
            Sleep(200);
            *(volatile int *)0 = (int)0xDEAD;   /* NULL write -> AV -> unhandled -> diag_uef + minidump */
        }
    }
    return 0;
}

void shield_diag_install(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD len;
    HANDLE h;
    if (InterlockedExchange(&g_installed, 1)) return;

    /* g_dir = dir of this DLL; g_logpath = g_dir\snaphak_diag.log. Cheap, no I/O under the loader lock. */
    len = GetModuleFileNameA((HMODULE)self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) { g_dir[0] = '\0'; strcpy_s(g_logpath, MAX_PATH, "snaphak_diag.log"); }
    else {
        char *slash = strrchr(path, '\\');
        if (slash) { *slash = '\0'; strncpy_s(g_dir, sizeof g_dir, path, _TRUNCATE); *slash = '\\'; }
        _snprintf_s(g_logpath, MAX_PATH, _TRUNCATE, "%s\\snaphak_diag.log", g_dir);
    }

    /* Install crash catchers immediately (loader-lock-safe APIs). Capture the previous UEF EXACTLY
     * ONCE here -- env_thread re-asserts without ever overwriting g_prev_uef. */
    AddVectoredExceptionHandler(1 /* first-in-chain */, diag_veh);
    g_prev_uef = SetUnhandledExceptionFilter(diag_uef);
    if (g_prev_uef == diag_uef) g_prev_uef = NULL;   /* belt: never chain to ourselves */

    /* one tiny write so an early crash (before env_thread runs) still shows the diagnostic was armed.
     * A single CreateFile/WriteFile is the minimum loader-lock exposure; OutputDebugString is deferred. */
    h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const char *m = "[diag] armed (DllMain); banner + env dump follow on the worker thread\r\n";
        DWORD w; WriteFile(h, m, (DWORD)strlen(m), &w, NULL); CloseHandle(h);
    }

    /* Heavy work (banner I/O, module/folder enum, the re-assert loop) off the loader lock. */
    { HANDLE t = CreateThread(NULL, 0, env_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
}

void shield_diag_detach(void)
{
    if (g_in_uef) {
        diag_log("DLL_PROCESS_DETACH after an unhandled exception => CRASH (see the UNHANDLED block + snaphak_crash.dmp)");
    } else if (g_last_code) {
        /* the UEF never fired but a crash-class fault WAS seen first-chance -> likely a __fastfail /
         * heap-stop that bypassed the UEF, OR our filter was displaced. The breadcrumb is the lead. */
        diag_log("DLL_PROCESS_DETACH: no UEF fired, but last first-chance crash-class was code=0x%08lx rip=0x%llx fault=0x%llx "
                 "(if DOOM crashed, suspect a __fastfail/heap-stop that bypasses the filter -- also grab %%LOCALAPPDATA%%\\CrashDumps\\*.dmp)",
                 (unsigned long)g_last_code, (unsigned long long)g_last_rip, (unsigned long long)g_last_fault);
    } else {
        diag_log("DLL_PROCESS_DETACH -- process exiting, no crash-class fault seen => clean exit/quit");
    }
}
