/* XInput backend bootstrap. A worker identifies the host image, resolves the
 * engine signatures at their recorded RVAs, checks the patch primitives,
 * installs feature hooks, starts the frontend, and arms the fault shield
 * compiled into this same DLL. A feature that cannot install is recorded in
 * feature_health.h by the name the player knows it by. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "user32.lib")   /* MessageBoxW: the unsupported-build notice */
#include "signatures.h"
#include "hook.h"
#include "smoke.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "map_payload.h"
#include "user_dirs.h"
#include "engine_dialog.h"
#include "../fault_shield/mapload_guards.h"
#include "../fault_shield/serialized_entities_guard.h"
#include "../fault_shield/load_screen_stop.h"
#include "strids.h"
#include "overrides.h"
#include "grid_room_native.h"
#include "grid_room_editor.h"
#include "fly_to.h"
#include "module_prefab.h"
#include "timeline_tile.h"
#include "modserve.h"
#include "modswf.h"
#include "modsound.h"
#include "modbankinfo.h"
#include "modhud.h"
#include "modhud_scene.h"
#include "modscene_place.h"
#include "modstore.h"
#include "modstock.h"
#include "modpublished.h"
#include "modpublish.h"
#include "budget_table.h"
#include "map_render.h"
#include "settings_tab.h"
#include "map_bounds.h"
#include "editor_zoom.h"
#include "world_area_cell.h"
#include "object_entry.h"
#include "place_anywhere.h"
#include "boot_pacing.h"
#include "grid_room_snap.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_menu.h"
#include "nav_play.h"
#include "commands.h"
#include "cvars.h"
#include "entity.h"
#include "companion_engine.h"
#include "typeinfo.h"
#include "megapreview.h"
#include "modvt.h"
#include "imgpreview.h"
#include "prefabpreview.h"
#include "soundpreview.h"
#include "patch.h"
#include "target_any.h"
#include "wiring_cleandirect.h"
#include "swf_textedit.h"
#include "ui_bridge.h"
#include "config.h"
#include "iface_engine.h"
#include "editor_undo.h"
#include "apply_engine.h"
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "engine_globals.h"
#include "feature_health.h"
#include "script/script_hooks.h"
#include "script/script_compile.h"
#include "script/script_node.h"
#include "script/script_fsm.h"
#include "script/script_api_ai.h"
#include "modcontent.h"
#include "../fault_shield/fault_shield.h"
#include "../fault_shield/fault_record.h"
#ifdef SH_DIAG
#include "../fault_shield/shield_diag.h"
#include "algo.h"
#endif

static uint8_t *g_doom_base = NULL;
static size_t   g_doom_size = 0;

/* What the private module palette asks the serving index (grid_room_native.h). */
static int bundle_module_served(const char *request, int module)
{
    return module ? sh_modserve_module_served(request) : sh_modserve_lookup(request, NULL);
}

/* The boot build's stock check (modstore.h sh_ms_boot_sync): the install's indexes. */
static int boot_stock_exists(void *context, const char *request)
{
    (void)context;
    return sh_modstock_exists(request);
}

/* The function's address for an install that requires an intact prologue, or
 * NULL; *clean says whether it resolved. */
static void *sig_fn(const sig_result *results, size_t n, const char *name, int *clean)
{
    const sig_result *r = sig_lookup(results, n, name);
    int ok = r && r->status == SIG_OK;
    if (clean) *clean = ok;
    return ok ? (void *)r->addr : NULL;
}

/* Record each named feature off when an install reported failure. */
static int need(int installed, const char *why, const char *feature, const char *also, const char *more)
{
    if (installed) return 1;
    sh_feature_off(feature, why);
    if (also) sh_feature_off(also, why);
    if (more) sh_feature_off(more, why);
    return 0;
}

static void install_features(const sig_result *results, size_t db)
{
    int clean = 0;

    /* Map loading: rawmaps and the map carrier (mods, map settings) run inside
     * the DeserializeFromJson detour. */
    void *deser = sig_fn(results, db, "DeserializeFromJson", &clean);
    need(sh_rawmap_swap_install(deser, clean), "map loads cannot be intercepted",
         SH_FEATURE_RAWMAPS, SH_FEATURE_MODS, SH_FEATURE_MAP_SETTINGS);
    /* Answer "is the open map a branch?" for a swapped map, so the engine's own Save asks for
     * a name rather than overwriting the map the rawmap opened over. */
    sh_rawmap_branch_install(g_doom_base);

    /* The editor-frame hook: a main-thread frame boundary, and the in-place map
     * reload it drives for the File menu's Load Rawmap. */
    {
        void *ed_frame = sig_fn(results, db, "EditorFrame", &clean);
        need(sh_editor_frame_install(ed_frame, clean, sig_fn(results, db, "EditorLoadMap", NULL), g_doom_base),
             "the editor frame hook is not installed", SH_FEATURE_RAWMAPS, NULL, NULL);
        {
            void *read_text = sig_fn(results, db, "ReadLocalSavedMapText", &clean);
            need(sh_editor_frame_carrier_install(read_text, clean, sig_fn(results, db, "IdStrAssignCStr", NULL)),
                 "a rawmap cannot be opened without reading one of your saved maps", SH_FEATURE_RAWMAPS, NULL, NULL);
        }
        /* Save Rawmap serializes the open map, not the newest save on disk. The tag
         * function also derives the engine's idStr ctor/dtor. */
        sh_rawmap_set_live_serialize(sig_fn(results, db, "SnapMapToJson", NULL),
                                     sig_fn(results, db, "SnapMapAddBranchTag", NULL));
    }

    /* Saves: mirror to rawmap JSON and write the map carrier. */
    {
        void *serialize = sig_fn(results, db, "SerializeToJson", &clean);
        need(sh_rawmap_save_install(serialize, clean), "map saves cannot be intercepted",
             SH_FEATURE_RAWMAPS, SH_FEATURE_MODS, SH_FEATURE_MAP_SETTINGS);
    }
    need(sh_map_payload_install(results, db), "the map size limits are not raised",
         SH_FEATURE_LARGE_MAPS, NULL, NULL);
    sh_rawmap_embed_install(g_doom_base);

    /* Guard stale event-wire lists and interactables whose subsystem is absent.
     * Their implementations define the engine-layout checks and recovery limits. */
    sh_evwire_guard_install(g_doom_base, results, db);
    sh_interactable_guard_install(g_doom_base, results, db);
    sh_serialized_entities_guard_install(results, db);
    /* Stop a failed map load's loading screen before the engine returns to the menu;
     * while it is armed the shield recovers faults during a load (redirect_policy.h). */
    sh_load_screen_stop_install(g_doom_base, results, db);

    void *get_decls = sig_fn(results, db, "GetDeclsOfType", NULL);

    /* Installed mods: read every bundle index before the provider can be
     * asked for a file, so the first open already sees the table. Stock
     * pointers read the installed game through the bridge (modstock.h). */
    sh_modstock_register();
    /* Borrowed sound events: the game's bank index answers which stock bank
     * a sound decl needs (modbankinfo.h); read on the first question. */
    sh_mb_set_bank_need(sh_mbi_bundle_need);
    /* Mod scripts are compiled (never run) by every build that carries one. */
    sh_mb_set_script_check(sh_script_compile_check);
    /* A mod's "scenes" and "hud" blocks are checked when it is built. */
    sh_mb_set_extra_check(sh_modcontent_check);

    /* No store migration runs here: the installer's store tool converts
     * earlier shapes. A folder a crash took away mid-replacement is put back
     * first, so it serves on this boot, and the work folders a crash left are
     * removed. */
    (void)sh_ms_recover_work();
    /* A new serving table may change a module's shipped navigation. */
    sh_modserve_set_index_listener(sh_nav_bake_invalidate_sources);
    (void)sh_modserve_index_boot();
    /* Folders not built yet are built off this thread as a rescan builds
     * them, then indexed, once this install has released the store
     * (sh_ms_boot_release below), so the install never waits on it. */
    {
        sh_mb_resolver boot_r;
        memset(&boot_r, 0, sizeof boot_r);
        boot_r.stock = boot_stock_exists;
        (void)sh_ms_boot_sync(&boot_r, sh_modserve_index_rescan, 1);
    }

    /* Inject custom #str_ entries before the first top-level language-table sort,
     * after the serving index so the first pass already carries the admitted
     * bundles' strings. */
    {
        void *sort_body = sig_fn(results, db, "StridsSortBody", &clean);
        need(sh_strids_install(sort_body, clean,
                               sig_fn(results, db, "StridsTableLea", NULL),
                               sig_fn(results, db, "StridsInsert", NULL),
                               sig_fn(results, db, "StridsHash", NULL),
                               sig_fn(results, db, "IdStrAssign", NULL)),
             "the language table cannot be extended", SH_FEATURE_TEXT, NULL, NULL);
    }

    /* Replace resource-provider open-by-name slot +0x148 after validating the
     * provider ABI. It serves map navigation, Grid Room variants, mods,
     * built-in defaults and otherwise the engine's own resources. All three
     * idStr helpers must be intact. */
    int ctor_clean = 0, read_clean = 0, compare_clean = 0, write_clean = 0;
    void *res_ctor = sig_fn(results, db, "ResProviderCtor", &ctor_clean);
    void *read_string = sig_fn(results, db, "IdFileReadString", &read_clean);
    void *compare = sig_fn(results, db, "IdFileCompare", &compare_clean);
    void *write_string = sig_fn(results, db, "IdFileWriteString", &write_clean);
    int overrides_installed = need(sh_overrides_install(g_doom_base, res_ctor, ctor_clean,
                                                        read_string, read_clean, compare, compare_clean,
                                                        write_string, write_clean),
                                   "the resource provider cannot be extended",
                                   SH_FEATURE_MODS, SH_FEATURE_GRID_SIZES, SH_FEATURE_TIMELINES);

    /* Dimension-bearing module names use the same backend resource provider.
     * Construct native variants only when a map actually requests one. */
    int grid_installed = overrides_installed &&
        need(sh_grid_native_install(results, db, g_doom_base), "native module variants are unavailable",
             SH_FEATURE_GRID_SIZES, NULL, NULL);
    /* Bundle modules take the same private palette route. */
    if (grid_installed) {
        sh_grid_native_set_module_source(bundle_module_served);
        need(sh_grid_snap_install(results, db, g_doom_base), "exact doorway attachment is unavailable",
             SH_FEATURE_GRID_DOORWAYS, NULL, NULL);
    }

    /* Navigation uses the resource shadow and map funnel; report its startup state. */
    sh_navmesh_install();

    /* Mod decl registration and the existence probe (needs the provider). */
    if (overrides_installed) {
        need(sh_modserve_install(results, db, g_doom_base), "mod declarations cannot be registered",
             SH_FEATURE_MODS, NULL, NULL);
        /* Mod virtual-texture families, cooked movies and ammo displays, and sound banks. */
        need(sh_modvt_install(results, db, g_doom_base), "mod texture families cannot be registered",
             SH_FEATURE_MOD_TEXTURES, NULL, NULL);
        need(sh_modswf_install(results, db, g_doom_base), "mod movies cannot be served",
             SH_FEATURE_MOD_MOVIES, NULL, NULL);
        need(sh_modsound_install(results, db, g_doom_base), "mod sound banks cannot be served",
             SH_FEATURE_MOD_SOUNDS, NULL, NULL);
    }
    (void)sh_modhud_install(results, db);

    /* Register cvars before commands. Commands additionally need cmdSystem;
     * each registration path handles missing dependencies independently. */
    sh_cvars_install(sig_fn(results, db, "CvarRegister", NULL), g_doom_base);

    void *cmdsys = sh_resolve_cmdsys(results, db, g_doom_base);
    need(sh_commands_install(sig_fn(results, db, "AddCommand", NULL), cmdsys,
                             sig_fn(results, db, "Printf", NULL), get_decls, g_doom_base),
         "the console command system is unavailable", SH_FEATURE_CONSOLE, NULL, NULL);

    /* Create the shared interface and launch the frontend after sh registration. */
    sh_ui_bridge_install();

    /* Bind apply dependencies before publishing their interface slots. */
    need(sh_apply_engine_install(results, db, g_doom_base, cmdsys), "the editor apply path is incomplete",
         SH_FEATURE_PREFABS, NULL, NULL);
    need(sh_fly_to_install(results, db), "the editor state switch is unavailable", SH_FEATURE_FLY_TO, NULL, NULL);
    need(sh_module_prefab_install(results, db, g_doom_base), "module save and place are unavailable",
         SH_FEATURE_PREFABS, NULL, NULL);
    /* The tile's decl text is served by the override provider; the palette
     * build registers it before the first editor entry. */
    if (overrides_installed)
        need(sh_timeline_tile_install(results, db, g_doom_base), "the editor palette cannot be extended",
             SH_FEATURE_TIMELINES, NULL, NULL);
    sh_budget_table_install(results, db);
    if (grid_installed)
        need(sh_grid_editor_install(results, db, sh_grid_native_read, sh_grid_native_apply),
             "the native property panel cannot be extended", SH_FEATURE_GRID_PROPERTIES, NULL, NULL);
    /* The Snapmap+ settings tab owns the Settings / Properties hooks and the
     * grid editor's property slot; mod settings register groups into it. */
    need(sh_settings_tab_install(results, db), "the settings screen cannot be extended",
         SH_FEATURE_SETTINGS_TAB, NULL, NULL);
    need(sh_map_render_install(results, db, g_doom_base) && sh_map_render_editor_install(results, db),
         "the rendering controls cannot be bound", SH_FEATURE_RENDERING, NULL, NULL);
    if (need(sh_map_bounds_install(results, db, g_doom_base), "the editor bounds cannot be changed",
             SH_FEATURE_MAP_BOUNDS, NULL, NULL))
        (void)sh_map_bounds_settings_register();
    need(sh_editor_zoom_install(results, db, g_doom_base), "the Blueprint camera limits cannot be changed",
         SH_FEATURE_ZOOM, NULL, NULL);
    /* After the Grid Room install: the cell choice counts its module bounds. */
    need(sh_world_area_cell_install(results, db, g_doom_base), "the world cell size cannot be changed",
         SH_FEATURE_WORLD_CELLS, NULL, NULL);
    need(sh_object_entry_install(results, db), "the Object Mode entry drop cannot be changed",
         SH_FEATURE_OBJECT_ENTRY, NULL, NULL);
    need(sh_place_anywhere_install(results, db), "the placement gates cannot be changed",
         SH_FEATURE_PLACE_ANYWHERE, NULL, NULL);

    /* Navigation geometry follows editor actions rather than a poll. */
    sh_apply_engine_install_edit_hook(results, db);
    /* Scenes hide and show the HUD through their own animation events. */
    (void)sh_modhud_scene_install(results, db);
    /* Mod scene nodes play at the player who activated them. */
    need(sh_modscene_place_install(results, db, g_doom_base), "scene actions cannot be redirected",
         SH_FEATURE_MOD_SCENES, NULL, NULL);

    /* Expose live entity reads so navigation sees marks edited since map load. */
    sh_nav_bake_set_live_editor(sh_apply_engine_entity_valid,
                                sh_apply_engine_entity_json,
                                sh_apply_engine_entity_transform, NULL);
    sh_nav_bake_set_snapshot(sh_apply_engine_nav_regions, NULL);
    need(sh_nav_menu_install(results, db), "the Start menu cannot be extended", SH_FEATURE_NAVIGATION, NULL, NULL);

    /* Read marks at entry to edit-to-build conversion, on the engine main thread.
     * Later bake callbacks can see entities with incomplete defsub state and
     * cannot safely clone them. Install only after the live read surface exists. */
    {
        void *snapbuild = sig_fn(results, db, "SnapMapEditToSnapBuild", &clean);
        need(sh_nav_play_install(snapbuild, clean), "the play build cannot be observed",
             SH_FEATURE_NAVIGATION, NULL, NULL);
        sh_nav_play_install_instances(results, db);
        sh_nav_play_install_volume_contents(results, db);
        sh_nav_play_install_overlay(results, db);
    }

    /* Bind engine slots after the interface and apply dependencies exist.
     * The editor singleton resolves through the engine-globals anchor. */
    sh_iface_engine_install(results, db, g_doom_base);
    /* Uninstall's undo-history clear: the push EditorCommitEdit calls (editor_undo.h). */
    (void)sh_editor_undo_install(results, db, g_doom_base);

    /* The native dialog binding the product raises its own questions through. */
    sh_engine_dialog_install(results, db, g_doom_base);

    /* Published-map and offline-play gates: a map whose manifest names a
     * missing bundle is held for the native Yes/No; needs the parse detour's
     * classification and the dialog binding above. */
    need(sh_modpublished_install(results, db, g_doom_base), "published maps cannot be checked for their mods",
         SH_FEATURE_PUBLISHING, NULL, NULL);
    need(sh_modpublish_install(results, db, g_doom_base), "publishing cannot carry mods",
         SH_FEATURE_PUBLISHING, NULL, NULL);

    /* Bind dependencies for the already-registered entity commands. */
    sh_entity_install(results, db, g_doom_base, cmdsys);

    /* Bind reflection functions and the signed decl-manager accessor call site. */
    sh_typeinfo_install(results, db, g_doom_base);

    /* Owner-bound companions (companion_engine.h): CallSpawn is detoured only
     * when an admitted bundle declares companions. */
    if (overrides_installed) (void)sh_companions_install(results, db, g_doom_base);

    /* Mod scripts (script/script_hooks.h): the frame hook, the spawn subscriber
     * on the companions' CallSpawn chain and the map-end subscriber; the frame
     * hook is committed only at a Play whose mods carry scripts. An install that
     * fails records mod scripts off with its reason (feature_health.h). */
    if (overrides_installed) (void)sh_script_hooks_install(results, db, g_doom_base);
    /* before any mod decl is parsed: a graph's script classes resolve at its parse */
    if (overrides_installed) (void)sh_script_fsm_install(g_doom_base);
    else sh_feature_off(SH_FEATURE_MOD_SCRIPTS, "mods are not served");
    /* Script nodes (script/script_node.h): node():fire, vars and the Relay
     * hook committed at the first Play with scripts. A mod's Script node row
     * is served whatever happens here, so a map holding one always loads. */
    if (sh_feature_is_off(SH_FEATURE_MOD_SCRIPTS)) sh_feature_off(SH_FEATURE_SCRIPT_NODES, "mod scripts are off");
    else need(sh_script_node_install(results, db, g_doom_base),
              "the game's Relay, output or variable functions are not the expected ones", SH_FEATURE_SCRIPT_NODES,
              NULL, NULL);
    /* Scripts driving their own demons (script/script_api_ai.h): the calls run
     * through the entity calls' event helper; the demon park (a detour of the
     * engine's IsRunningScript) is committed at the first command. */
    if (sh_feature_is_off(SH_FEATURE_MOD_SCRIPTS)) sh_feature_off(SH_FEATURE_SCRIPT_DEMONS, "mod scripts are off");
    else need(sh_script_ai_install(g_doom_base),
              "the game's AI functions are not the expected ones", SH_FEATURE_SCRIPT_DEMONS, NULL, NULL);

    /* Decode virtual-texture previews from installed files through the signed
     * engine page decoder; no renderer or GPU calls are needed. */
    sh_megapreview_install(results, db, g_doom_base);
    /* Load installed-resource indexes for file-based image previews and catalogs. */
    sh_imgpreview_install();
    /* Start bounded prefab geometry previews using the installed-resource index. */
    sh_prefabpreview_install();
    /* Bind editor audio audition and stop functions together. */
    sh_soundpreview_install(results, db, g_doom_base, cmdsys);

#ifdef SH_DIAG
    /* The cs_dontuse math overrides: the diagnostic build only, off until toggled. */
    sh_algo_install(g_doom_base);
#endif

    /* Bind registry enumeration for the visibility-toggle command. */
    sh_target_any_install(get_decls);

    /* Bare wire targets receive native references; real nodes retain stock wiring. */
    sh_wiring_cleandirect_install(g_doom_base);

    /* Add SWF clipboard shortcuts; paste requires its own assignment signature. */
    sh_swf_textedit_install(g_doom_base);
}

static DWORD WINAPI bootstrap_thread(LPVOID p)
{
    char line[256];
    (void)p;
    /* The host image is identified once: the executable is not wrapped, and
     * nothing about it changes while the process runs. */
    g_doom_base = (uint8_t *)sh_host_image_base();
    g_doom_size = sh_host_image_size();
    if (g_doom_base == NULL) {
        backend_log("FATAL: host process is not a supported DOOM 2016 build "
                    "(expected DOOMx64vk.exe or DOOMx64.exe)");
        return 0;
    }

    /* Every engine address and layout here is for one build, identified by the
     * executable's SHA-256. On anything else this DLL only forwards XInput. */
    if (!sh_host_is_supported_build()) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "game: unsupported executable %s (%s); Snapmap+ is off and only forwards controller input",
            sh_host_image_name(), sh_host_executable_hash8()[0] ? sh_host_executable_hash8() : "unhashed");
        backend_log(line);
        /* The player hears it too, once at boot, on this thread (the game is
         * not waiting on it): a log line alone left a DOOM update silent. */
        MessageBoxW(NULL,
                    L"This copy of DOOM is not the build this Snapmap+ release supports (the April 2024 "
                    L"update, Steam build 13954591), so Snapmap+ is off for this session and the game "
                    L"runs without it.\n\nDOOM was probably updated. Run the Snapmap+ installer again: it "
                    L"installs the release that matches your copy of DOOM, or tells you that none does yet.",
                    L"Snapmap+ is off", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    {
        char game[SH_GAME_IDENTITY_MAX];
        sh_host_game_identity(game, sizeof game);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "game: %s, %s renderer, base %p, size %zx, process age %lu ms", game,
            sh_host_identity()->vulkan ? "Vulkan" : "OpenGL", (void *)g_doom_base, g_doom_size,
            sh_boot_pacing_process_age_ms());
        backend_log(line);
    }

    /* The engine's "common" preload starts a few seconds after launch and is
     * paced by waits that boot_pacing.c shortens; patch those first. */
    {
        sig_result boot[SH_BOOT_PACING_SIGS];
        (void)sh_boot_pacing_match(g_doom_base, boot, SH_BOOT_PACING_SIGS);
        (void)sh_boot_pacing_install(boot, SH_BOOT_PACING_SIGS, SH_BOOT_PACING_DEFAULT,
                                     sh_boot_pacing_process_age_ms());
    }

    sh_user_dirs_ensure();
    sh_config_init(); /* nonfatal: the service retains defaults and status flags on failure */

    /* Every signature at its recorded RVA: a few hundred byte compares. */
    size_t total = sig_db_count();
    size_t db = total < SIG_RESULTS_MAX ? total : SIG_RESULTS_MAX;
    static sig_result results[SIG_RESULTS_MAX];
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    size_t resolved = sig_resolve_all(g_doom_base, results, db);
    QueryPerformanceCounter(&t1);
    unsigned long resolve_us = (unsigned long)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    _snprintf_s(line, sizeof line, _TRUNCATE, "signatures: %zu/%zu at their recorded addresses (%lu us)",
                resolved, total, resolve_us);
    backend_log(line);
    for (size_t i = 0; i < db; i++) {
        if (results[i].status == SIG_OK) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE, "signature %s: %s", results[i].name,
                    sig_status_reason(results[i].status));
        backend_log(line);
    }

    /* The patch primitive and a scratch detour are checked before any engine
     * code is touched; a failure leaves the game exactly as it was. */
    int patching_ok = sh_patch_selftest();
#ifdef SH_DIAG
    if (!sh_algo_selftest()) backend_log("algo: the math override self-test failed");
#endif
    patching_ok = sh_smoke_run(g_doom_base, results, db, resolve_us / 1000) && patching_ok;

    /* Resolve and log all data anchors here so unsupported dependencies are visible. */
    {
        size_t gtotal = glb_db_count();
        size_t gok    = glb_resolve_all(g_doom_base);
        _snprintf_s(line, sizeof line, _TRUNCATE, "engine globals: %zu/%zu resolved", gok, gtotal);
        backend_log(line);
    }

    if (patching_ok) install_features(results, db);
    else {
        sh_feature_off(SH_FEATURE_ALL, "the patch self-test failed, so no engine code was changed");
        /* The window still opens, so the player can read why. */
        sh_ui_bridge_install();
    }

    /* Arm the resident fault shield after startup resolution. */
    /* Its Frame hook is the main-thread tick: console registration and the
     * window's main-thread calls run there. */
    if (!shield_install(g_doom_base, g_doom_size, results, patching_ok ? db : 0) && patching_ok) {
        sh_feature_off(SH_FEATURE_CONSOLE, "the main-frame hook is not installed");
        sh_feature_off(SH_FEATURE_PREFABS, "the main-frame hook is not installed");
    }

    /* The install is done with the mod store: the boot build may start. */
    sh_ms_boot_release();

    {
        char *off = sh_features_off_text();
        if (off) backend_log(off);
        free(off);
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "startup: install complete at process age %lu ms",
                sh_boot_pacing_process_age_ms());
    backend_log(line);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        backend_set_logpath_from_module(hinst);   /* sh_backend.log under <DOOM>\snapmap-plus\logs\ */
        shield_set_logpath_from_module(hinst);     /* shield_faults.log beside it */
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
        shield_diag_detach();   /* record crash-vs-clean-exit in sh_diag.log */
#endif
        /* Process exit releases the preview worker and log handle.
         * Waiting or taking their locks here can deadlock the loader. */
    }
    return TRUE;
}
