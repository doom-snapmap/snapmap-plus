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
#include "host_image.h"

/* Backend-owned object shared with the frontend and sh command dispatcher. */
static sh_iface *g_iface = NULL;

/* Startup arguments must outlive the frontend thread. */
static sh_ui_argblock g_argblock;
static void          *g_ui_out_slot = NULL;   /* receives the frontend's loop-state obj address */

/* Stable synthetic argv for the frontend startup ABI: the backend's release
 * version ("--version=v1.0.0", or "dev" for a local build), which the window
 * matches against its own when it loads, and the game identity ("--game=DOOM
 * 2016 build ..."), the single string every report uses; the frontend reads it
 * instead of identifying the executable itself. */
#ifndef SNAPMAP_PLUS_VERSION
#define SNAPMAP_PLUS_VERSION "dev"
#endif
static char  g_argv0[] = "snapmap-plus";
static char  g_argv_version[] = "--version=" SNAPMAP_PLUS_VERSION;
static char  g_argv_game[sizeof "--game=" + SH_GAME_IDENTITY_MAX];
static char *g_argv[]  = { g_argv0, g_argv_version, g_argv_game, NULL };

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
        backend_log("ui: interface created");
    else
        backend_log("ui: the interface could not be created (out of memory)");
    return g_iface;
}

int sh_ui_bridge_install(void)
{
    /* Publish the interface before exposing services to the frontend. */
    if (!sh_ui_create_iface()) {
        backend_log("ui: not started, there is no interface");
        return 0;
    }
    /* Theme reads can begin as soon as the frontend starts. */
    sh_config_bind_iface_slots();

    /* Register SnapStack once in the backend so all callers share the same store. */
    sh_register_snapstack_commands_backend(g_iface);
    backend_log("ui: SnapStack commands registered");

    g_argblock.out_slot = &g_ui_out_slot;
    {
        char game[SH_GAME_IDENTITY_MAX];
        g_argv_game[0] = '\0';
        if (sh_host_game_identity(game, sizeof game))
            _snprintf_s(g_argv_game, sizeof g_argv_game, _TRUNCATE, "--game=%s", game);
    }
    g_argblock.argc     = g_argv_game[0] ? 3 : 2;
    g_argblock.argv     = g_argv;
    g_argblock.iface    = g_iface;

    /* Load the frontend by its absolute path beside this DLL, never relative to the working
     * directory; its own dependencies resolve from its folder. */
    {
        static wchar_t dir[32768], dll[32768];
        const size_t cap = sizeof dir / sizeof dir[0];
        HMODULE self = NULL;
        DWORD n = 0;
        wchar_t *slash;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(void *)&sh_ui_bridge_install, &self))
            n = GetModuleFileNameW(self, dir, (DWORD)cap);
        slash = (n && n < cap) ? wcsrchr(dir, L'\\') : NULL;
        if (!slash) {
            backend_log("ui: the folder of this DLL could not be read; the Snapmap+ window is not started");
            return 1;
        }
        slash[1] = L'\0';
        _snwprintf_s(dll, cap, _TRUNCATE, L"%ls%ls", dir, SH_UI_DLL_PATH);
        _snwprintf_s(dir + (slash + 1 - dir), cap - (size_t)(slash + 1 - dir), _TRUNCATE, L"%ls", SH_UI_DLL_DIR);
        SetDllDirectoryW(dir);   /* the frontend's own dependencies, loaded now and later */
        g_ui_dll = LoadLibraryW(dll);
    }
    if (!g_ui_dll) {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "ui: the Snapmap+ window could not be loaded (error %lu); reinstall Snapmap+", GetLastError());
        backend_log(line);
        /* Interface creation succeeded even though the window cannot start. */
        return 1;
    }

    LPTHREAD_START_ROUTINE start =
        (LPTHREAD_START_ROUTINE)GetProcAddress(g_ui_dll, "sh_ui_init");
    if (!start) {
        backend_log("ui: the Snapmap+ window DLL has no sh_ui_init; reinstall Snapmap+");
        return 1;
    }

    /* Pass static arguments to the frontend thread with a 1 MiB stack. */
    HANDLE h = CreateThread(NULL, 0x100000, start, &g_argblock, 0, NULL);
    if (!h) {
        backend_log("ui: the Snapmap+ window thread could not start");
        return 1;
    }
    CloseHandle(h);
    backend_log("ui: Snapmap+ window loaded and started");
    return 1;
}
