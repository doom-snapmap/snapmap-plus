# Compile and run the offline C/C++ and JavaScript tests with MSVC x64 and Node.
# Requires Visual Studio C++ Build Tools. Outputs go to tests\obj (gitignored).
#
#   tests\run-tests.ps1
#   tests\run-tests.ps1 -Doom <DOOMx64vk.exe 13954591> -DoomAlt <DOOMx64.exe 13954591>
#   tests\run-tests.ps1 -Skip map_published_test
#
# The default suite needs no game or built DLL. -Doom and -DoomAlt take the two
# executables of the supported build (13954591, no unpacking needed) and add
# signature, hook-fallback and global checks against each renderer's recorded RVAs.
# Exit 0 means every selected test passed; failures include their build log.
#
# For xinput_ordinal_test against the built proxy, see docs\contributing.md.
param([string]$Doom = "", [string]$DoomAlt = "", [string[]]$Only = @(), [string[]]$Skip = @())
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$obj  = Join-Path $here "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Import the MSVC environment ONCE. vcvars64.bat costs seconds, so running it per
# test spent minutes on setup alone. Names starting with "=" are cmd's per-drive
# working directories and are skipped by the pattern.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "cl not on PATH after $vcvars" }

# name | sources (relative to tests\) | runtime arg
$tests = @(
    @{ name = "nav_menu_test"; src = 'nav_menu_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "startup_bindings_test"; src = 'startup_bindings_test.c ..\src\backend\startup_bindings.c'; arg = "" }
    @{ name = "serialized_entities_guard_test"; src = 'serialized_entities_guard_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_render_test"; src = 'map_render_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_render_editor_test"; src = 'map_render_editor_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_bounds_test"; src = 'map_bounds_test.c ..\src\backend\map_bounds.c'; defs = '/DSH_MAP_BOUNDS_PURE'; arg = "" }
    @{ name = "editor_zoom_test"; src = 'editor_zoom_test.c ..\src\backend\editor_zoom.c'; defs = '/DSH_EDITOR_ZOOM_PURE'; arg = "" }
    @{ name = "fly_to_test"; src = 'fly_to_test.c'; arg = "" }
    @{ name = "module_prefab_test"; src = 'module_prefab_test.c ..\src\backend\module_prefab_map.c ..\src\backend\mp_json.c'; arg = "" }
    @{ name = "timeline_tile_test"; src = 'timeline_tile_test.c ..\src\backend\timeline_tile.c'; defs = '/DSH_TIMELINE_TILE_PURE'; arg = "" }
    @{ name = "modaction_test"; src = 'modaction_test.c ..\src\backend\modaction.c'; defs = '/DSH_MODACTION_PURE'; arg = "" }
    @{ name = "budget_table_test"; src = 'budget_table_test.c ..\src\backend\budget_table.c'; defs = '/DSH_BUDGET_TABLE_PURE'; arg = "" }
    @{ name = "settings_tab_test"; src = 'settings_tab_test.c ..\src\backend\settings_tab.c'; defs = '/DSH_SETTINGS_TAB_PURE'; arg = "" }
    @{ name = "map_render_native_test"; src = 'map_render_native_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_nav_test"; src = 'grid_room_nav_test.c ..\src\backend\grid_room_nav.c ..\src\backend\grid_room.c ..\src\backend\aas_edit.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "grid_room_asset_test"; src = 'grid_room_asset_test.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_test"; src = 'grid_room_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_editor_test"; src = 'grid_room_editor_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_areas_test"; src = 'grid_room_areas_test.c ..\src\backend\grid_room_areas.c ..\src\backend\grid_room.c'; arg = "" }
    @{ name = "world_area_cell_test"; src = 'world_area_cell_test.c ..\src\backend\grid_room_areas.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "object_entry_test"; src = 'object_entry_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "place_anywhere_test"; src = 'place_anywhere_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "cvar_unlock_test"; src = 'cvar_unlock_test.c'; arg = "" }
    @{ name = "boot_pacing_test"; src = 'boot_pacing_test.c ..\src\backend\patch.c ..\src\backend\hook.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "grid_room_native_test"; src = 'grid_room_native_test.c ..\src\backend\grid_room_areas.c ..\src\backend\grid_room.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\grid_room_edit.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_snap_test"; src = 'grid_room_snap_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "nav_heap_test"; src = 'nav_heap_test.c'; arg = "" }
    @{ name = "nav_heap_hook_test"; src = 'nav_heap_hook_test.c ..\src\backend\patch.c ..\src\backend\hook.c map_bounds_link_stub.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "patch_test"; src = 'patch_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_payload_test"; src = 'map_payload_test.c ..\src\backend\map_payload.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "json_patch_test"; src = 'json_patch_test.c ..\src\backend\json_patch.c'; arg = "" }
    @{ name = "edit_pair_test"; src = 'edit_pair_test.c'; arg = "" }
    @{ name = "engine_cvar_read_test"; src = 'engine_cvar_read_test.c'; arg = "" }
    @{ name = "apply_dispatch_test"; src = 'apply_dispatch_test.c ..\src\backend\prefab_grab.c ..\src\backend\perf.c ..\src\backend\nav_snapshot.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/Gy'; arg = "" }
    @{ name = "snapstack_pair_test"; src = 'snapstack_pair_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "crash_report_test"; src = 'crash_report_test.c ..\src\fault_shield\crash_record_format.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "recovery_dialog_test"; src = 'recovery_dialog_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "nav_play_test"; src = 'nav_play_test.c ..\src\backend\patch.c ..\src\backend\hook.c map_bounds_link_stub.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "shield_format_test"; src = 'shield_format_test.c ..\src\fault_shield\fault_record.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "redirect_policy_test"; src = 'redirect_policy_test.c'; arg = "" }
    @{ name = "hook_test";          src = 'hook_test.c ..\src\backend\hook.c ..\src\backend\patch.c'; defs = '/DSH_PATCH_TESTING';                       arg = "" }
    @{ name = "crash_record_test";  src = 'crash_record_test.c ..\src\fault_shield\crash_record_format.c'; arg = "" }
    @{ name = "report_scrub_test";  src = 'report_scrub_test.c';                                     arg = "" }
    @{ name = "dumpmap_path_test";  src = 'dumpmap_path_test.c';                                     arg = "" }
    @{ name = "json_pretty_test";   src = 'json_pretty_test.c';                                      arg = "" }
    @{ name = "editor_frame_test"; src = 'editor_frame_test.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "rawmap_paths_test";  src = 'rawmap_paths_test.c ..\src\backend\map_legacy_payload.c ..\src\backend\modlegacy.c ..\src\backend\modbundle.c ..\src\backend\decl_text.c ..\src\backend\modmanifest.c ..\src\backend\raw_deflate.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\hook.c ..\src\backend\patch.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c map_bounds_link_stub.c mod_seam_stub.c ..\src\backend\modpublished.c'; defs = '/DSH_RAWMAP_TESTING /DSH_CONFIG_TESTING /DSH_PATCH_TESTING /DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modmanifest_test"; src = 'modmanifest_test.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "modmap_test"; src = 'modmap_test.c ..\src\backend\modmap.c ..\src\backend\mp_json.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "modpublished_test"; src = 'modpublished_test.c ..\src\backend\modpublished.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\config_json.c'; defs = '/DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; arg = "" }
    @{ name = "modlegacy_test"; src = 'modlegacy_test.c mod_seam_stub.c ..\src\backend\modlegacy.c ..\src\backend\modbundle.c ..\src\backend\decl_text.c ..\src\backend\modmanifest.c ..\src\backend\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c'; defs = '/DSH_MOD_STUB_NO_BUILDER'; libs = 'shell32.lib'; arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "editor_delete_test"; src = 'editor_delete_test.c ..\src\backend\editor_delete.c'; defs = '/DSH_EDITOR_DELETE_TESTING'; arg = "" }
    @{ name = "editor_undo_test"; src = 'editor_undo_test.c ..\src\backend\editor_undo.c'; defs = '/DSH_EDITOR_UNDO_TESTING'; arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "startup_dirs_test"; src = 'startup_dirs_test.c ..\src\backend\user_dirs.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = (Join-Path $here '..') }
    @{ name = "strids_test"; src = 'strids_test.c ..\src\backend\strids.c ..\src\backend\config_json.c'; defs = '/DSH_STRIDS_TESTING'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\host_image.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    # Installed mod bundles: index, provider precedence, registration plan.
    @{ name = "modserve_test"; src = 'modserve_test.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\host_image.c'; defs = '/DSH_OVERRIDES_TESTING /DSH_MODSERVE_PURE /DSH_MODSERVE_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "process_heap_scope_test"; src = 'process_heap_scope_test.c'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    @{ name = "navmesh_test"; src = 'navmesh_test.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING'; arg = "" }
    @{ name = "nav_regions_test"; src = 'nav_regions_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVR_TESTING'; arg = "" }
    @{ name = "nav_snapshot_test"; src = 'nav_snapshot_test.c ..\src\backend\nav_snapshot.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "nav_entitydef_test"; src = 'nav_entitydef_test.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "aas_edit_test"; src = 'aas_edit_test.c ..\src\backend\aas_edit.c'; arg = "" }
    @{ name = "nav_geometry_test"; src = 'nav_geometry_test.c ..\src\backend\nav_geometry.c'; arg = "" }
    @{ name = "aas_augment_test"; src = 'aas_augment_test.c ..\src\backend\aas_augment.c ..\src\backend\nav_overlay.c ..\src\backend\nav_geometry.c ..\src\backend\aas_edit.c ..\src\backend\nav_traversal.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING /DSH_AUG_TESTING /DSH_TRAV_TESTING'; arg = "" }
    @{ name = "nav_bake_test"; src = 'nav_bake_test.c ..\src\backend\perf.c ..\src\backend\nav_bake.c ..\src\backend\nav_overlay.c ..\src\backend\nav_regions.c ..\src\backend\nav_entitydef.c ..\src\backend\aas_edit.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\nav_traversal.c ..\src\backend\map_shards.c'; defs = '/DSH_NAV_BAKE_TESTING /DSH_AUG_TESTING'; arg = "" }
    @{ name = "nav_traversal_test"; src = 'nav_traversal_test.c ..\src\backend\nav_traversal.c'; defs = '/DSH_TRAV_TESTING'; arg = "" }
    # Link the real globals resolver to verify refusal when this non-game process
    # cannot provide the load-state address.
    @{ name = "config_message_test"; src = 'config_message_test.cpp ..\src\ui\webview\config_message.cpp'; cxx = $true; arg = "" }
    @{ name = "webview_json_test"; src = 'webview_json_test.cpp ..\src\ui\webview\webview_json.cpp'; cxx = $true; arg = "" }
    @{ name = "crash_pending_test"; src = 'crash_pending_test.cpp'; cxx = $true; arg = "" }
    @{ name = "game_identity_arg_test"; src = 'game_identity_arg_test.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_bootstrap_test"; src = 'theme_bootstrap_test.cpp ..\src\ui\webview\theme_bootstrap.cpp'; cxx = $true; arg = "" }
    @{ name = "webview_profile_test"; src = 'webview_profile_test.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_contract_test"; src = 'theme_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "entity_settings_contract_test"; src = 'entity_settings_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "growing_text_buffer_test"; src = 'growing_text_buffer_test.cpp'; cxx = $true; arg = "" }
    @{ name = "rawmap_picker_test"; src = 'rawmap_picker_test.cpp'; cxx = $true; arg = "" }
    @{ name = "preview_test";     src = 'preview_test.c ..\src\backend\preview.c';              arg = "" }
    @{ name = "bcn_test";         src = 'bcn_test.c ..\src\backend\bcn.c';                      arg = "" }
    @{ name = "soundpreview_queue_test"; src = 'soundpreview_queue_test.c';                       arg = "" }
    @{ name = "imgpreview_index_test"; src = 'imgpreview_index_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';      arg = "" }
    @{ name = "imgpreview_catalog_test"; src = 'imgpreview_catalog_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';  arg = "" }
    @{ name = "prefabpreview_test"; src = 'prefabpreview_test.c';                                  arg = "" }
    @{ name = "prefab_grab_test"; src = 'prefab_grab_test.c ..\src\backend\prefab_grab.c';          arg = "" }
    @{ name = "megapreview_io_test"; src = 'megapreview_io_test.c';                                 arg = "" }
    @{ name = "modbundle_test"; src = 'modbundle_test.c ..\src\backend\modbundle.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    @{ name = "modstore_test"; src = 'modstore_test.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; defs = '/DSH_MODSTORE_TESTING'; libs = 'shell32.lib ole32.lib'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    # The builder, serving index, map manifest and legacy converter linked together.
    @{ name = "mod_pipeline_test"; src = 'mod_pipeline_test.c ..\src\backend\modbundle.c ..\src\backend\modserve.c ..\src\backend\modmanifest.c ..\src\backend\modlegacy.c ..\src\backend\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; libs = 'shell32.lib ole32.lib'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    # The retired overrides\ tree moved into the mods\ store, over a synthetic tree.
    @{ name = "modmigrate_test"; src = 'modmigrate_test.c mod_seam_stub.c ..\src\backend\modmigrate.c ..\src\backend\modlegacy.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\modmanifest.c ..\src\backend\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modstock_migrate_test"; src = 'modstock_migrate_test.c ..\src\backend\modstock.c mod_seam_stub.c ..\src\backend\modmigrate.c ..\src\backend\modlegacy.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\modmanifest.c ..\src\backend\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modshadow_test"; src = 'modshadow_test.c ..\src\backend\modserve.c ..\src\backend\modmigrate.c ..\src\backend\modlegacy.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\modmanifest.c ..\src\backend\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; defs = '/DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = "" }
    # The sh_vt diagnostic's helpers, and with SH_VT_RCTEST the reference family bundle end to end.
    @{ name = "vt_harness_test"; src = 'vt_harness_test.c ..\src\backend\vt_harness.c ..\src\backend\modvt.c ..\src\backend\modbundle.c ..\src\backend\modserve.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; defs = '/DSH_VT_HARNESS_PURE /DSH_MODVT_PURE /DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = $(if ($env:SH_VT_RCTEST) { $env:SH_VT_RCTEST } else { "" }) }
    # The mod store client, upload and download, and the rawmap ZIP export, against a loopback stub of the store.
    @{ name = "modnet_test"; src = 'modnet_test.c ..\src\backend\modnet.c ..\src\backend\modbundle.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modstock_test"; src = 'modstock_test.c ..\src\backend\modstock.c ..\src\backend\modsync.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modmodule_test"; src = 'modmodule_test.c ..\src\backend\module_prefab_map.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\modstock.c ..\src\backend\modsync.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modsync_test"; src = 'modsync_test.c ..\src\backend\modsync.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modpublish_test"; src = 'modpublish_test.c ..\src\backend\modpublish.c ..\src\backend\modsync.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\modbundle.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_MODPUBLISH_PURE /DSH_MODPUBLISH_TESTING /DSH_CONFIG_TESTING'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modexport_test"; src = 'modexport_test.c ..\src\backend\modexport.c ..\src\backend\prefab_block.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "prefab_mods_test"; src = 'prefab_mods_test.c ..\src\backend\prefab_mods.c ..\src\backend\prefab_block.c ..\src\backend\modpublished.c ..\src\backend\module_prefab_map.c ..\src\backend\modexport.c ..\src\backend\modsync.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_PREFAB_MODS_PURE /DSH_PREFAB_MODS_TESTING /DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modui_test"; src = 'modui_test.c ..\src\backend\modui.c ..\src\backend\raw_deflate.c ..\src\backend\modmap.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\modserve.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\config_json.c ..\src\backend\decl_text.c'; defs = '/DSH_MODUI_PURE /DSH_MODUI_TESTING /DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = @((Join-Path $here 'fixtures\mods\alex.r2-imp'), (Join-Path $here 'fixtures\mods_tab_payload_v3.json')) }
    @{ name = "serialization_buffer_test"; src = 'serialization_buffer_test.cpp'; cxx = $true;     arg = "" }
)
if ($Doom) {
    if (-not (Test-Path $Doom)) { throw "-Doom path not found: $Doom" }
    $da = (Resolve-Path $Doom).Path
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "globals_test"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
}
# The other renderer's executable gets the same checks against its own RVAs.
if ($DoomAlt) {
    if (-not (Test-Path $DoomAlt)) { throw "-DoomAlt path not found: $DoomAlt" }
    $alt = (Resolve-Path $DoomAlt).Path
    $tests += @{ name = "sig_test_alt"; src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $alt }
    $tests += @{ name = "globals_test_alt"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $alt }
}

# Executable identity and the raw-RVA gate; hashes the given images too.
$identityImages = @(@($da, $alt) | Where-Object { $_ })
$tests += @{ name = "build_identity_test"; src = 'build_identity_test.c ..\src\backend\host_image.c ..\src\backend\signatures.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Prologue length decoding; with images, the cs_dontuse stolen boundaries.
$tests += @{ name = "x64_len_test"; src = 'x64_len_test.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Cooked movies (FileResource Load slot) and bundle ammo displays; with images,
# their signatures, decoded engine calls, vtable slot and HUD call site.
$tests += @{ name = "modswf_test"; src = 'modswf_test.c ..\src\backend\modswf.c ..\src\backend\modbundle.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_MODSWF_PURE /DSH_MODSWF_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
$tests += @{ name = "modsound_test"; src = 'modsound_test.c ..\src\backend\modsound.c ..\src\backend\modbundle.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_MODSOUND_PURE /DSH_MODSOUND_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
$tests += @{ name = "modhud_test"; src = 'modhud_test.c ..\src\backend\modhud.c ..\src\backend\config_json.c ..\src\backend\patch.c ..\src\backend\hook.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_MODHUD_TESTING /DSH_PATCH_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
# Native idFile / idFileSystem slots the backend calls, against the images.
# Mod virtual-texture families against a fake record table, and with the executables the decoded VT surface.
$tests += @{ name = "modvt_test"; src = 'modvt_test.c ..\src\backend\modvt.c ..\src\backend\modserve.c ..\src\backend\modbundle.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_MODVT_PURE /DSH_MODSERVE_PURE /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib ole32.lib'; arg = $identityImages }
$tests += @{ name = "native_slots_test"; src = 'native_slots_test.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Every fixed detour window against the images: boundary and relative operands.
$tests += @{ name = "detour_windows_test"; src = 'detour_windows_test.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @(@((Join-Path $here '..')) + $identityImages) }
# The proxy's exports against System32 XINPUT1_4.dll once build.ps1 has produced
# it (CI builds first); with images, also their XInput imports.
$proxyDir = Join-Path (Resolve-Path (Join-Path $here '..')).Path 'build'
if (Test-Path (Join-Path $proxyDir 'XINPUT1_4.dll')) {
    $tests += @{ name = "xinput_exports_test"; src = 'xinput_exports_test.c'; arg = @(@($proxyDir) + $identityImages) }
} else {
    Write-Host "[skip] xinput_exports_test (build\XINPUT1_4.dll not built; run build.ps1 first)"
}

if ($Skip.Count) {
    $unknown = @($Skip | Where-Object { $_ -notin $tests.name })
    if ($unknown.Count) { throw "Unknown skipped test(s): $($unknown -join ', ')." }
    if (@($Skip | Where-Object { $_ -in $Only }).Count) { throw "A test cannot be selected by both -Only and -Skip." }
    foreach ($name in $Skip) { Write-Host "[skip] $name (explicit -Skip selection)" }
    $tests = @($tests | Where-Object { $_.name -notin $Skip })
}
if ($Only.Count) {
    $unknown = @($Only | Where-Object { $_ -notin $tests.name })
    if ($unknown.Count) { throw "Unknown test selector(s): $($unknown -join ', '). Use complete names from this manifest, including _test." }
    $tests = @($tests | Where-Object { $_.name -in $Only })
}

$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $obj ($t.name + ".exe")
    $defs = if ($t.defs) { " $($t.defs)" } else { "" }
    $cxx  = if ($t.cxx)  { " /EHsc /std:c++17" } else { "" }
    $libs = if ($t.libs) { " /link $($t.libs)" } else { "" }
    # Relative output paths avoid cl's quoted trailing-backslash parsing issue.
    $cl  = "cl /nologo /O2 /MT /I..\src\backend /I..\src\common /I..\src\fault_shield /I..\src\ui\webview$cxx$defs $($t.src) /Fe:obj\$($t.name).exe /Foobj\$libs"
    $log = Join-Path $obj ($t.name + ".build.log")
    # Judge the compiler exit code; vcvars may emit unrelated stderr diagnostics.
    cmd /c "cd /d `"$here`" && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log | Write-Host; Write-Host "[FAIL] compile $($t.name)"; $fail++; continue }
    if ($t.arg) { & $exe @($t.arg) } else { & $exe }
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($t.name) (exit $LASTEXITCODE)"; $fail++ }
    else { Write-Host "[ok]   $($t.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail native test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all native tests passed ($($tests.Count))"

if ($Only.Count) { exit 0 }

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "decl_language_test.js", "ui_assets_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js", "feedback_renderer_test.js", "feedback_game_test.js", "issues_retest_test.js", "worker_body_test.js", "rawmap_menu_test.js", "delete_result_test.js", "prefab_tree_test.js", "timeline_tree_test.js", "timeline_eventcall_test.js", "prefab_budget_test.js", "mods_tab_test.js", "prefab_mods_test.js")
foreach ($jsTest in $jsTests) {
    & $node.Source (Join-Path $here $jsTest)
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $jsTest (exit $LASTEXITCODE)"; exit 1 }
    Write-Host "[ok]   $jsTest"
}
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
