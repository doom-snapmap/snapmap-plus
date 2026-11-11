/* Install fault recovery and crash capture from the backend bootstrap.
 * Shares the backend's hook and signature implementations. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "crash_report.h"
#include "shield_sigs.h"
#ifdef SH_DIAG
#include "../common/wide_path.h"
#endif

uint8_t *g_doom_base = NULL;   /* shield's view of the DOOM module (set by shield_install from the backend) */
size_t   g_doom_size = 0;

/* Diagnostic arming log (<DOOM>\snapmap-plus\logs\shield_arm.log), using
 * write-through Win32 I/O. Release builds write nothing. */
void shield_raw(const char *msg)
{
#ifdef SH_DIAG
    static HANDLE h = INVALID_HANDLE_VALUE;
    static int tried = 0;
    static wchar_t path[SH_WPATH_MAX];
    DWORD wr;
    if (h == INVALID_HANDLE_VALUE) {
        if (tried) return;
        tried = 1;
        if (!sh_wpath_module_dir(NULL, path, SH_WPATH_MAX) ||
            !sh_wpath_append(path, SH_WPATH_MAX, L"snapmap-plus") || !sh_wpath_ensure_dir(path) ||
            !sh_wpath_append(path, SH_WPATH_MAX, L"logs") || !sh_wpath_ensure_dir(path) ||
            !sh_wpath_append(path, SH_WPATH_MAX, L"shield_arm.log")) return;
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    WriteFile(h, msg, lstrlenA(msg), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
#else
    (void)msg;
#endif
}

/* Resolve engine functions before installing the VEH and frame hook. */
int veh_install(void);        /* veh.c */
int recovery_install(void);   /* recovery.c */
static int shield_install_hooks(void)
{
    shield_resolve_engine(g_doom_base);   /* Cache engine addresses before recovery can run. */
    return veh_install() && recovery_install();
}

#ifdef SH_DIAG
/* Diagnostic builds wait, bounded, for an instrumentation tool that created
 * this event to attach before the shield arms. */
#define SHIELD_INSTR_EVENT_NAME   "Local\\SnapmapPlusInstrAttached"
#define SHIELD_ARM_FALLBACK_MS    10000

static void shield_wait_for_instr(void)
{
    HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, SHIELD_INSTR_EVENT_NAME);
    DWORD w;
    if (ev == NULL) { shield_raw("wait: no attach event -> arming now"); return; }
    w = WaitForSingleObject(ev, SHIELD_ARM_FALLBACK_MS);
    CloseHandle(ev);
    shield_raw(w == WAIT_OBJECT_0 ? "wait: attached -> arming" : "wait: no attach signal -> arming");
}
#endif

/* Called by the backend bootstrap with the live module range; a NULL base
 * skips installation. */
void shield_install(uint8_t *doom_base, size_t doom_size)
{
    g_doom_base = doom_base;
    g_doom_size = doom_size;
    if (g_doom_base == NULL) { shield_raw("shield_install: NULL base -> skipped"); return; }
#ifdef SH_DIAG
    shield_wait_for_instr();
#endif
    if (shield_install_hooks()) shield_raw("shield_install: armed (VEH + frame hook)");
    else shield_raw("shield_install: FAILED to install hooks");
    /* Arm crash capture even if recovery installation failed. This runs outside
     * the loader lock on the bootstrap thread. */
    crash_report_init();
    crash_report_arm_fatal_handlers();
}
