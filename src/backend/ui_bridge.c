/* Own the shared interface, register backend commands, and launch the frontend DLL.
 * The generic interface factory lives in src/common; engine slots bind separately. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "snapmap_plus_iface.h"
#include "ui_bridge.h"
#include "backend_log.h"
#include "config.h"
#include "snapstack.h"

/* Backend-owned object shared with the frontend and sh command dispatcher. */
static sh_iface *g_iface = NULL;

/* Startup arguments must outlive the frontend thread. */
static sh_ui_argblock g_argblock;
static void          *g_ui_out_slot = NULL;   /* receives the frontend's loop-state obj address */

/* Stable synthetic argv for the frontend startup ABI. */
static char  g_argv0[] = "snapmap-plus";
static char *g_argv[]  = { g_argv0, NULL };

/* Keep the frontend module loaded for its thread lifetime. */
static HMODULE g_ui_dll = NULL;

sh_iface *sh_ui_get_iface(void)
{
    return g_iface;
}

/* Create the backend-owned interface at most once. */
static sh_iface *sh_ui_create_iface(void)
{
    if (g_iface) return g_iface;
    g_iface = sh_iface_create();
    if (g_iface)
        backend_log("C0: interface object created (vtable + empty cmd-map + empty work-queue)");
    else
        backend_log("C0: interface object creation FAILED (operator_new)");
    return g_iface;
}

int sh_ui_bridge_install(void)
{
    /* Publish the interface before exposing services to the frontend. */
    if (!sh_ui_create_iface()) {
        backend_log("C0: ui-bridge abort -- no interface object");
        return 0;
    }
    /* Theme reads can begin as soon as the frontend starts. */
    sh_config_bind_iface_slots();

    /* Register SnapStack once in the backend so all callers share the same store. */
    sh_register_snapstack_commands_backend(g_iface);
    backend_log("C0: backend SnapStack commands registered (sole owner) -- `sh snapstack_diag` to verify");


    g_argblock.out_slot = &g_ui_out_slot;
    g_argblock.argc     = 1;
    g_argblock.argv     = g_argv;
    g_argblock.iface    = g_iface;

    /* Load the frontend and its dependencies from the deployed overlay directory. */
    SetDllDirectoryA(".\\snapmap-plus\\");
    g_ui_dll = LoadLibraryA(".\\snapmap-plus\\snapmap-plus-ui.dll");
    if (!g_ui_dll) {
        DWORD e = GetLastError();
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "C0: LoadLibraryA(.\\snapmap-plus\\snapmap-plus-ui.dll) FAILED err=%lu "
            "(interface still created; sh will report it exists)", e);
        backend_log(line);
        /* Interface creation succeeded even though the window cannot start. */
        return 1;
    }


    LPTHREAD_START_ROUTINE start =
        (LPTHREAD_START_ROUTINE)GetProcAddress(g_ui_dll, "sh_ui_init");
    if (!start) {
        backend_log("C0: GetProcAddress(sh_ui_init) FAILED -- frontend export missing");
        return 1;
    }

    /* Pass static arguments to the frontend thread with a 1 MiB stack. */
    HANDLE h = CreateThread(NULL, 0x100000, start, &g_argblock, 0, NULL);
    if (!h) {
        backend_log("C0: CreateThread(sh_ui_init) FAILED");
        return 1;
    }
    CloseHandle(h);
    backend_log("C0: snapmap-plus-ui.dll loaded + sh_ui_init thread spun (interface handed over)");
    return 1;
}
