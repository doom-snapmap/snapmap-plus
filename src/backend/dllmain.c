/* XInput backend bootstrap. A worker resolves the host image and engine symbols,
 * checks patch primitives, installs feature hooks, starts the frontend, and arms
 * the fault shield compiled into this same DLL. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include "signatures.h"
#include "hook.h"
#include "smoke.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "map_package.h"
#include "palette_refresh.h"
#include "engine_dialog.h"
#include "../fault_shield/mapload_guards.h"
#include "strids.h"
#include "overrides.h"
#include "grid_room_native.h"
#include "grid_room_editor.h"
#include "map_render.h"
#include "grid_room_snap.h"
#include "package_requirements.h"
#include "weapon_hud.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_play.h"
#include "decl_server.h"
#include "commands.h"
#include "cvars.h"
#include "entity.h"
#include "typeinfo.h"
#include "megapreview.h"
#include "imgpreview.h"
#include "prefabpreview.h"
#include "soundpreview.h"
#include "patch.h"
#include "algo.h"
#include "target_any.h"
#include "wiring_cleandirect.h"
#include "swf_textedit.h"
#include "ui_bridge.h"
#include "config.h"
#include "user_overrides.h"
#include "iface_engine.h"
#include "apply_engine.h"
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "engine_globals.h"
#include "../fault_shield/fault_shield.h"
#include "../fault_shield/fault_record.h"
#ifdef SH_DIAG
#include "../fault_shield/shield_diag.h"
#endif

static uint8_t *g_doom_base = NULL;
static size_t   g_doom_size = 0;

static void resolve_doom(void)
{
    /* Resolve the host image for either accepted executable name. */
    g_doom_base = (uint8_t *)sh_host_image_base();
    g_doom_size = sh_host_image_size();
}

/* SteamStub exposes the engine .text after DLL initialization. Poll resolution
 * until the database is available or the timeout permits a partial installation. */
#define PB0_POLL_INTERVAL_MS  75
#define PB0_POLL_TIMEOUT_MS   60000

/* Create runtime data directories for hand-deployed overlays and fresh profiles.
 * Existing directories are harmless; other creation failures are logged. */
static void ensure_user_dirs(void)
{
    static const char *subs[] = { "", "\\strings", "\\overrides", "\\prefabs" };
    char base[MAX_PATH], path[MAX_PATH];
    int i;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        backend_log("WARN: SHGetFolderPathA(CSIDL_LOCAL_APPDATA) failed -- snapmap-plus user dirs not created");
        return;
    }
    for (i = 0; i < (int)(sizeof subs / sizeof subs[0]); i++) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus%s", base, subs[i]);
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            char line[MAX_PATH + 64];
            _snprintf_s(line, sizeof line, _TRUNCATE, "WARN: CreateDirectory %s failed (err %lu)",
                        path, GetLastError());
            backend_log(line);
        }
    }
    backend_log("snapmap-plus user-data dirs ensured (root + strings/overrides/prefabs)");
}

static DWORD WINAPI bootstrap_thread(LPVOID p)
{
    (void)p;
    /* Wait up to about 60 seconds for the supported host image. */
    for (int i = 0; i < 6000 && g_doom_base == NULL; i++) {
        resolve_doom();
        if (!g_doom_base) Sleep(10);
    }
    if (g_doom_base == NULL) {
        backend_log("FATAL: host process is not a supported DOOM 2016 build "
                    "(expected DOOMx64vk.exe or DOOMx64.exe)");
        return 0;
    }

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "backend attached host=%s renderer=%s base=%p size=%zx",
        sh_host_image_name(),
        sh_host_is_vulkan() == 1 ? "vulkan" : (sh_host_is_vulkan() == 0 ? "opengl" : "unknown"),
        (void *)g_doom_base, g_doom_size);
    backend_log(line);


    ensure_user_dirs();
    sh_config_init(); /* nonfatal: the service retains defaults and status flags on failure */
    sh_user_overrides_capture_launch_state();

    /* Retry while startup code may still be encrypted; partial binding is allowed
     * after timeout and each feature must validate its own dependencies. */
    size_t total = sig_db_count();
    DWORD  t0 = GetTickCount();
    size_t last_ok = 0;
    for (;;) {
        last_ok = sh_resolve_count(g_doom_base);
        if (last_ok == total) break;
        if (GetTickCount() - t0 >= PB0_POLL_TIMEOUT_MS) break;
        Sleep(PB0_POLL_INTERVAL_MS);
    }
    DWORD elapsed = GetTickCount() - t0;

    /* Record resolution and scratch-detour results from a fresh scan. */
    sh_smoke_run(g_doom_base, elapsed);

    /* Resolve and log all data anchors here so unsupported dependencies are visible. */
    {
        size_t gtotal = glb_db_count();
        size_t gok    = glb_resolve_all(g_doom_base);
        char gline[128];
        _snprintf_s(gline, sizeof gline, _TRUNCATE,
            "engine globals: %zu/%zu resolved", gok, gtotal);
        backend_log(gline);
    }

    /* Check guarded patches on scratch memory before installing consumers. */
    sh_patch_selftest();

    /* Check math implementations without touching engine state. */
    sh_algo_selftest();

    /* Bind features from the final resolution results, including partial results. */
    {
        sig_result results[SIG_RESULTS_MAX];
        sig_resolve_all(g_doom_base, results, SIG_RESULTS_MAX);
        size_t db = sig_db_count();
        if (db > SIG_RESULTS_MAX) db = SIG_RESULTS_MAX;

        /* Install rawmap load interception only on a clean prologue. The swap starts
         * disarmed; package admission also runs inside this detour. */
        void *deser = NULL;
        int   deser_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "DeserializeFromJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    deser = (void *)results[i].addr;
                deser_clean = (results[i].status == SIG_OK);   /* clean scan, not the hook-tolerant fallback */
                break;
            }
        }
        /* Capture the immutable installed-package snapshot before load interception
         * can run. Packages added later require a restart to become active. */
        {
            char mpkg_root[MAX_PATH];
            if (sh_overrides_get_root(mpkg_root, sizeof mpkg_root))
                sh_mpkg_boot_capture(mpkg_root);
            else
                backend_log("MPKG: boot capture SKIPPED -- no effective override root "
                            "(maps declaring packages will be refused, never crashed)");
        }
        sh_rawmap_swap_install(deser, deser_clean);
        /* Answer "is the open map a branch?" for a swapped map, so the engine's own Save asks for
         * a name rather than overwriting the map the rawmap opened over. */
        sh_rawmap_branch_install(g_doom_base);

        /* The editor-frame hook: a main-thread frame boundary, and the in-place map
         * reload it drives for the File menu's Load Rawmap. Installed beside the swap
         * it serves, and like it does not depend on the editor being up. */
        {
            void *ed_frame = NULL, *ed_loadmap = NULL, *ed_addtag = NULL, *ed_tojson = NULL;
            int   ed_frame_clean = 0;
            for (size_t i = 0; i < db; i++) {
                if (results[i].name == NULL) continue;
                if (strcmp(results[i].name, "EditorFrame") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_frame = (void *)results[i].addr;
                    ed_frame_clean = (results[i].status == SIG_OK);
                } else if (strcmp(results[i].name, "EditorLoadMap") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_loadmap = (void *)results[i].addr;
                } else if (strcmp(results[i].name, "SnapMapAddBranchTag") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_addtag = (void *)results[i].addr;
                } else if (strcmp(results[i].name, "SnapMapToJson") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_tojson = (void *)results[i].addr;
                }
            }
            sh_editor_frame_install(ed_frame, ed_frame_clean, ed_loadmap, g_doom_base);
            /* Save Rawmap serializes the open map, not the newest save on disk. The tag
             * function also derives the engine's idStr ctor/dtor. */
            sh_rawmap_set_live_serialize(ed_tojson, ed_addtag);
        }

        /* Mirror saves to rawmap JSON. Require a clean serialization prologue before
         * installing this always-active shadow hook. */
        void *serialize = NULL;
        int   serialize_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "SerializeToJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    serialize = (void *)results[i].addr;
                serialize_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        sh_rawmap_save_install(serialize, serialize_clean);

        sh_rawmap_embed_install(g_doom_base);


        /* The render-node guard remains disabled after the apply-path root fix.
         * The separate fault shield is installed normally below. */

        backend_log("rendernode-guard: DISABLED (redundant since the ae_apply_one root fix; "
                    "fault-shield itself is ACTIVE)");

        /* Guard stale event-wire lists and interactables whose subsystem is absent.
         * Their implementations define the engine-layout checks and recovery limits. */
        sh_evwire_guard_install(g_doom_base);
        sh_interactable_guard_install(g_doom_base);

        void *get_decls = (void *)sig_addr_by_name(results, db, "GetDeclsOfType");


        /* Inject custom #str_ entries before the first top-level language-table sort.
         * The detour needs a clean prologue; helper functions come from the same scan. */
        void *sort_body = NULL;
        int   sort_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "StridsSortBody") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    sort_body = (void *)results[i].addr;
                sort_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        void *strids_lea   = (void *)sig_addr_by_name(results, db, "StridsTableLea");
        void *strids_ins   = (void *)sig_addr_by_name(results, db, "StridsInsert");
        void *strids_hash  = (void *)sig_addr_by_name(results, db, "StridsHash");
        void *idstr_ctor   = (void *)sig_addr_by_name(results, db, "IdStrAssign");
        sh_strids_install(sort_body, sort_clean, strids_lea, strids_ins, strids_hash, idstr_ctor);

        /* Replace resource-provider open-by-name slot +0xF8 after validating the audited
         * provider ABI. User override layers use the immutable launch setting;
         * built-in defaults and engine resources remain available regardless. */
        /* Publish all three clean native idStr helpers before installing the provider. */
        void *res_ctor = NULL;
        int   ctor_clean = 0;
        void *read_string = NULL;
        void *compare = NULL;
        void *write_string = NULL;
        int   read_string_clean = 0;
        int   compare_clean = 0;
        int   write_string_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "ResProviderCtor") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    res_ctor = (void *)results[i].addr;
                ctor_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileReadString") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    read_string = (void *)results[i].addr;
                read_string_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileCompare") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    compare = (void *)results[i].addr;
                compare_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileWriteString") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    write_string = (void *)results[i].addr;
                write_string_clean = (results[i].status == SIG_OK);
            }
        }
        int overrides_installed = sh_overrides_install(g_doom_base,
                                                       res_ctor, ctor_clean,
                                                       read_string, read_string_clean,
                                                       compare, compare_clean,
                                                       write_string, write_string_clean);

        /* Dimension-bearing module names use the same backend resource provider.
         * Construct native variants only when a map actually requests one. */
        int grid_installed = overrides_installed && sh_grid_native_install(results, db, g_doom_base);
        if (overrides_installed && !grid_installed)
            backend_log("GRID: native module variants unavailable on this build");
        if (grid_installed && !sh_grid_snap_install(results, db, g_doom_base))
            backend_log("GRID: exact doorway attachment unavailable on this build");

        /* Navigation uses the resource shadow and map funnel; report its startup state. */
        sh_navmesh_install();

        /* Register cvars before commands. Commands additionally need cmdSystem;
         * each registration path handles missing dependencies independently. */
        void *cvar_reg = (void *)sig_addr_by_name(results, db, "CvarRegister");
        sh_cvars_install(cvar_reg, g_doom_base);

        void *add_cmd  = (void *)sig_addr_by_name(results, db, "AddCommand");
        void *printf_d = (void *)sig_addr_by_name(results, db, "Printf");
        void *cmdsys   = sh_resolve_cmdsys(results, db, g_doom_base);
        sh_commands_install(add_cmd, cmdsys, printf_d, get_decls, g_doom_base);

        /* Capture allowlisted package cvars, then queue them once the engine is RUNNING.
         * Do not alter these gates during startup declaration parsing. */
        {
            char override_root[MAX_PATH];
            void *buffer_cmd = (void *)sig_addr_by_name(results, db, "BufferCommandText");
            if (sh_overrides_get_root(override_root, sizeof(override_root))) {
                sh_package_requirements_install(override_root, g_doom_base, cmdsys, buffer_cmd,
                                                sh_user_overrides_enabled_for_launch());
                sh_weapon_hud_install(override_root, g_doom_base, results, db,
                                      sh_user_overrides_enabled_for_launch());
            } else
                backend_log("package-requirements REFUSED: effective override root unavailable");
        }

        /* Create the shared interface and launch the frontend after sh registration. */
        sh_ui_bridge_install();

        /* Bind apply dependencies before publishing their interface slots. */
        sh_apply_engine_install(results, db, g_doom_base, cmdsys);
        if(grid_installed&&!sh_grid_editor_install(results,db,sh_grid_native_read,sh_grid_native_apply))
            backend_log("GRID: native dimension properties unavailable on this build");
        if(!sh_map_render_install(results,db,g_doom_base)||!sh_map_render_editor_install(results,db))
            backend_log("RENDER: per-map rendering controls unavailable on this build");

        /* Expose live entity reads so navigation sees marks edited since map load. */
        sh_nav_bake_set_live_editor(sh_apply_engine_entity_count,
                                    sh_apply_engine_entity_valid,
                                    sh_apply_engine_entity_json, NULL);
        sh_nav_bake_set_snapshot(sh_apply_engine_nav_snapshot, NULL);

        /* Read marks at entry to edit-to-build conversion, on the engine main thread.
         * Later bake callbacks can see entities with incomplete defsub state and
         * cannot safely clone them. Install only after the live read surface exists. */
        {
            void *snapbuild = NULL;
            int   snapbuild_clean = 0;
            for (size_t i = 0; i < db; i++) {
                if (results[i].name && strcmp(results[i].name, "SnapMapEditToSnapBuild") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        snapbuild = (void *)results[i].addr;
                    snapbuild_clean = (results[i].status == SIG_OK);
                    break;
                }
            }
            sh_nav_play_install(snapbuild, snapbuild_clean);
            sh_nav_play_install_instances(results, db);
            sh_nav_play_install_volume_contents(results, db);
        }

        /* Bind engine slots after the interface and apply dependencies exist.
         * The editor singleton resolves through the engine-globals anchor. */
        sh_iface_engine_install(results, db, g_doom_base);

        /* Arm the native palette rebuild before declaration registration can queue it. */
        sh_palette_refresh_install(results, db, g_doom_base);

        /* Install native prompts before the decl server can raise a package dialog.
         * Failed binding leaves the OS message-box fallback available. */
        sh_engine_dialog_install(results, db, g_doom_base);

        /* Queue immutable new-declaration registration on the engine main thread.
         * The interface and palette callbacks must be ready before its command runs. */
        if (overrides_installed)
            sh_decl_server_install(results, db, g_doom_base, cmdsys);
        else
            backend_log("decl-server REFUSED: pinned resource-provider ABI was not installed");

        /* Bind dependencies for the already-registered entity commands. */
        sh_entity_install(results, db, g_doom_base, cmdsys);

        /* Bind reflection functions and the signed decl-manager accessor call site. */
        sh_typeinfo_install(results, db, g_doom_base);

        /* Decode virtual-texture previews from installed files through the signed
         * engine page decoder; no renderer or GPU calls are needed. */
        sh_megapreview_install(results, db, g_doom_base);
        /* Load installed-resource indexes for file-based image previews and catalogs. */
        sh_imgpreview_install();
        /* Start bounded prefab geometry previews using the installed-resource index. */
        sh_prefabpreview_install();
        /* Bind editor audio audition and stop functions together. */
        sh_soundpreview_install(results, db, g_doom_base, cmdsys);

        /* Cache math-override dependencies; cs_dontuse remains off until toggled. */
        sh_algo_install(g_doom_base);

        /* Bind registry enumeration for the visibility-toggle command. */
        sh_target_any_install(get_decls);

        /* Bare wire targets receive native references; real nodes retain stock wiring. */
        sh_wiring_cleandirect_install(g_doom_base);

        /* Add SWF clipboard shortcuts; paste requires its own assignment signature. */
        sh_swf_textedit_install(g_doom_base);
    }

    /* Arm the resident fault shield after startup resolution. */

    shield_install(g_doom_base, g_doom_size);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        backend_set_logpath_from_module(hinst);   /* sh_backend.log under <DOOM>\snapmap-plus\logs\ */
        shield_set_logpath_from_module(hinst);     /* shield_faults.log under <DOOM>\snapmap-plus\logs\ (shield's own log) */
#ifdef SH_DIAG
        /* Diagnostic builds record faults and environment details without recovering. */
        shield_diag_install(hinst);
#endif
        /* Don't do engine work in DllMain (loader lock). Spin the bootstrap onto its own thread. */
        HANDLE h = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        /* The independent unlock worker can run before backend resolution completes. */
        sh_cvar_unlock_start();
    }
    else if (reason == DLL_PROCESS_DETACH) {
#ifdef SH_DIAG
        shield_diag_detach();   /* DIAGNOSTIC: record crash-vs-clean-exit in sh_diag.log */
#endif
        /* Process exit releases the preview worker and log handle.
         * Waiting or taking their locks here can deadlock the loader. */
    }
    return TRUE;
}
