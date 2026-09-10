/* Install fault recovery and crash capture from the backend bootstrap after
 * SteamStub decryption. Shares the backend's hook and signature implementations. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "crash_report.h"
#include "shield_sigs.h"

uint8_t *g_doom_base = NULL;   /* shield's view of the DOOM module (set by shield_install from the backend) */
size_t   g_doom_size = 0;

/* Diagnostic arming log, using write-through Win32 I/O. Release builds do not
 * create shield_arm.log; the caller emits separate debugger status messages. */
void shield_raw(const char *msg)
{
#ifdef SH_DIAG
    static HANDLE h = INVALID_HANDLE_VALUE;
    static int tried = 0;
    if (h == INVALID_HANDLE_VALUE) {
        if (tried) return;
        tried = 1;
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        char *slash = NULL, *q;
        for (q = path; *q; q++) if (*q == '\\') slash = q;
        if (!slash) return;
        /* <DOOM>\snapmap-plus\logs\shield_arm.log -- CRT-free path build (lstrcpy/lstrcat + CreateDirectory,
         * one level at a time) */
        lstrcpyA(slash + 1, "snapmap-plus");
        CreateDirectoryA(path, NULL);
        lstrcatA(path, "\\logs");
        CreateDirectoryA(path, NULL);
        lstrcatA(path, "\\shield_arm.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD wr;
    WriteFile(h, msg, lstrlenA(msg), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
#else
    (void)msg;
#endif
}

/* Resolve engine functions before installing the VEH and frame hook. Literal
 * fallback addresses are restricted to the pinned build. */
int veh_install(void);        /* veh.c */
int recovery_install(void);   /* recovery.c */
static int shield_install_hooks(void)
{
    shield_resolve_engine(g_doom_base);   /* Cache engine addresses before recovery can run. */
    return veh_install() && recovery_install();
}

/* If instrumentation created this event, wait for attachment or the timeout.
 * Without the event, arm immediately. A stalled attachment never disables the shield. */
#define SHIELD_INSTR_EVENT_NAME   "Local\\SnapmapPlusInstrAttached"
#define SHIELD_ARM_FALLBACK_MS    10000   /* bounded: a stalled/dead attach event still lets the shield arm */

static void shield_wait_for_instr(void)
{
    HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, SHIELD_INSTR_EVENT_NAME);
    if (ev == NULL) {
        OutputDebugStringA("[shield] no instrumentation-attach event (end-user path) -> arming now\n");
        shield_raw("wait: OpenEvent NULL (no attach event) -> arming NOW");
        return;
    }
    shield_raw("wait: event FOUND -> blocking until attach signal or fallback");
    DWORD w = WaitForSingleObject(ev, SHIELD_ARM_FALLBACK_MS);
    CloseHandle(ev);
    if (w == WAIT_OBJECT_0)        shield_raw("wait: SIGNALED -> arming after injection");
    else if (w == WAIT_TIMEOUT)    shield_raw("wait: TIMEOUT (fallback) -> arming");
    else                           shield_raw("wait: ERROR -> arming");
}

/* Called by the backend bootstrap after decryption with the live module range.
 * The optional instrumentation wait is bounded; a NULL base skips installation. */
void shield_install(uint8_t *doom_base, size_t doom_size)
{
    g_doom_base = doom_base;
    g_doom_size = doom_size;
    if (g_doom_base == NULL) { shield_raw("shield_install: NULL base -> skipped"); return; }
    shield_raw("shield_install: entered (merged into backend XINPUT1_3)");
    shield_wait_for_instr();
    if (shield_install_hooks()) {
        shield_raw("shield_install: ARMED (VEH + frame-hook installed)");
        OutputDebugStringA("[shield] armed (merged into backend XINPUT1_3)\n");
    } else {
        shield_raw("shield_install: FAILED to install hooks");
        OutputDebugStringA("[shield] FAILED to install hooks\n");
    }
    /* Arm crash capture even if recovery installation failed. This runs outside
     * the loader lock on the bootstrap thread. */
    crash_report_init();
    crash_report_arm_fatal_handlers();
}
