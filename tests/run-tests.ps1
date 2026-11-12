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

# Import the MSVC environment once (tools\cbuild.ps1); every compile and link below inherits it.
$repo = Split-Path -Parent $here
. (Join-Path $repo "tools\cbuild.ps1")
$null = Import-MsvcEnv $env:SNAPMAPPLUS_VCVARS_VER

# name | sources (relative to tests\) | runtime arg
$tests = @(
    @{ name = "nav_menu_test"; src = 'nav_menu_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "serialized_entities_guard_test"; src = 'serialized_entities_guard_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_render_test"; src = 'map_render_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_render_editor_test"; src = 'map_render_editor_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_bounds_test"; src = 'map_bounds_test.c ..\src\backend\map_bounds.c'; defs = '/DSH_MAP_BOUNDS_PURE'; arg = "" }
    @{ name = "editor_zoom_test"; src = 'editor_zoom_test.c ..\src\backend\editor_zoom.c'; defs = '/DSH_EDITOR_ZOOM_PURE'; arg = "" }
    @{ name = "fly_to_test"; src = 'fly_to_test.c'; arg = "" }
    @{ name = "module_prefab_test"; src = 'module_prefab_test.c ..\src\backend\module_prefab_map.c ..\src\backend\mp_json.c'; arg = "" }
    @{ name = "timeline_tile_test"; src = 'timeline_tile_test.c ..\src\backend\timeline_tile.c'; defs = '/DSH_TIMELINE_TILE_PURE'; arg = "" }
    @{ name = "typeinfo_cache_test"; src = 'typeinfo_cache_test.c ..\src\backend\modtypeinfo.c'; defs = '/DSH_MODTYPEINFO_PURE'; arg = "" }
    @{ name = "modaction_test"; src = 'modaction_test.c ..\src\backend\modaction.c'; defs = '/DSH_MODACTION_PURE'; arg = "" }
    @{ name = "modaction_engine_test"; src = 'modaction_engine_test.c'; arg = "" }
    @{ name = "budget_table_test"; src = 'budget_table_test.c ..\src\backend\budget_table.c'; defs = '/DSH_BUDGET_TABLE_PURE'; arg = "" }
    @{ name = "settings_tab_test"; src = 'settings_tab_test.c ..\src\backend\settings_tab.c'; defs = '/DSH_SETTINGS_TAB_PURE'; arg = "" }
    @{ name = "map_render_native_test"; src = 'map_render_native_test.c ..\src\backend\map_render.c ..\src\backend\map_settings.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\map_bounds.c ..\src\backend\patch.c ..\src\backend\hook.c ..\src\backend\sha256.c'; defs = '/DSH_MAP_BOUNDS_PURE'; arg = "" }
    @{ name = "sha256_test"; src = 'sha256_test.c ..\src\backend\sha256.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "map_carrier_test"; src = 'map_carrier_test.c ..\src\backend\map_carrier.c ..\src\backend\map_shards.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "map_settings_test"; src = 'map_settings_test.c ..\src\backend\map_settings.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\map_render.c ..\src\backend\map_bounds.c ..\src\backend\sha256.c'; defs = '/DSH_MAP_BOUNDS_PURE'; arg = "" }
    @{ name = "grid_room_nav_test"; src = 'grid_room_nav_test.c ..\src\backend\grid_room_nav.c ..\src\backend\grid_room.c ..\src\backend\aas_edit.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "grid_room_asset_test"; src = 'grid_room_asset_test.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_test"; src = 'grid_room_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_editor_test"; src = 'grid_room_editor_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_areas_test"; src = 'grid_room_areas_test.c ..\src\backend\grid_room_areas.c ..\src\backend\grid_room.c'; arg = "" }
    @{ name = "world_area_cell_test"; src = 'world_area_cell_test.c ..\src\backend\grid_room_areas.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "object_entry_test"; src = 'object_entry_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "place_anywhere_test"; src = 'place_anywhere_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "cvar_unlock_test"; src = 'cvar_unlock_test.c'; arg = "" }
    @{ name = "boot_pacing_test"; serial = $true; src = 'boot_pacing_test.c ..\src\backend\patch.c ..\src\backend\hook.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "grid_room_native_test"; src = 'grid_room_native_test.c ..\src\backend\grid_room_areas.c ..\src\backend\grid_room.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\grid_room_edit.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_snap_test"; src = 'grid_room_snap_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "nav_heap_test"; src = 'nav_heap_test.c'; arg = "" }
    @{ name = "nav_heap_hook_test"; src = 'nav_heap_hook_test.c ..\src\backend\patch.c ..\src\backend\hook.c map_bounds_link_stub.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "patch_test"; src = 'patch_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "map_payload_test"; src = 'map_payload_test.c ..\src\backend\map_payload.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "json_patch_test"; src = 'json_patch_test.c ..\src\backend\json_patch.c'; arg = "" }
    @{ name = "edit_pair_test"; src = 'edit_pair_test.c'; arg = "" }
    @{ name = "decl_text_own_state_test"; src = 'decl_text_own_state_test.c ..\src\backend\decl_text.c'; arg = "" }
    @{ name = "engine_cvar_read_test"; src = 'engine_cvar_read_test.c'; arg = "" }
    @{ name = "modhud_scene_test"; src = 'modhud_scene_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = "" }
    @{ name = "modscene_place_test"; src = 'modscene_place_test.c ..\src\backend\modscene_place.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING /DSH_MODSCENE_PLACE_PURE'; arg = "" }
    @{ name = "apply_dispatch_test"; serial = $true; src = 'apply_dispatch_test.c ..\src\backend\prefab_grab.c ..\src\backend\perf.c ..\src\backend\nav_snapshot.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c ..\src\common\snapmap_plus_iface.c'; defs = '/Gy'; arg = "" }
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
    @{ name = "saved_map_guard_test"; src = 'saved_map_guard_test.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "rawmap_paths_test";  src = 'rawmap_paths_test.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\modmanifest.c ..\src\backend\raw_deflate.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\hook.c ..\src\backend\patch.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c map_bounds_link_stub.c mod_seam_stub.c ..\src\backend\modpublished.c ..\src\backend\feature_health.c ..\src\backend\sha256.c'; defs = '/DSH_RAWMAP_TESTING /DSH_CONFIG_TESTING /DSH_PATCH_TESTING /DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modmanifest_test"; src = 'modmanifest_test.c ..\src\backend\modmanifest.c ..\src\backend\modsalt.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\config_json.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "modmap_test"; src = 'modmap_test.c ..\src\backend\modmap.c ..\src\backend\mp_json.c ..\src\backend\modmanifest.c ..\src\backend\modsalt.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\config_json.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "modpublished_test"; src = 'modpublished_test.c ..\src\backend\modpublished.c ..\src\backend\modmanifest.c ..\src\backend\modsalt.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\config_json.c ..\src\backend\sha256.c'; defs = '/DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; arg = "" }
    @{ name = "modlegacy_test"; src = 'modlegacy_test.c mod_seam_stub.c ..\src\store\modlegacy.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\modmanifest.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\store\map_old.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; defs = '/DSH_MOD_STUB_NO_BUILDER'; libs = 'shell32.lib'; arg = "" }
    @{ name = "map_old_test"; src = 'map_old_test.c mod_seam_stub.c ..\src\store\map_old.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\modmanifest.c ..\src\backend\modsalt.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\config_json.c ..\src\backend\sha256.c'; defs = '/DSH_MOD_STUB_NO_BUILDER'; libs = 'shell32.lib'; arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "editor_delete_test"; src = 'editor_delete_test.c ..\src\backend\editor_delete.c'; defs = '/DSH_EDITOR_DELETE_TESTING'; arg = "" }
    @{ name = "editor_undo_test"; src = 'editor_undo_test.c ..\src\backend\editor_undo.c'; defs = '/DSH_EDITOR_UNDO_TESTING'; arg = "" }
    @{ name = "folder_watch_test"; src = 'folder_watch_test.c'; arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "startup_dirs_test"; src = 'startup_dirs_test.c ..\src\backend\user_dirs.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "strids_test"; src = 'strids_test.c ..\src\backend\strids.c ..\src\backend\strkeys.c ..\src\backend\config_json.c'; defs = '/DSH_STRIDS_TESTING'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\host_image.c ..\src\backend\sha256.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    # Installed mod bundles: index, provider precedence, registration plan.
    @{ name = "modledger_test"; src = 'modledger_test.c ..\src\backend\modledger.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; defs = '/DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modserve_test"; src = 'modserve_test.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\host_image.c ..\src\backend\sha256.c'; defs = '/DSH_OVERRIDES_TESTING /DSH_MODSERVE_PURE /DSH_MODSERVE_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modserve_engine_test"; src = 'modserve_engine_test.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\perf.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\config_json.c ..\src\backend\host_image.c ..\src\backend\modledger.c ..\src\backend\sha256.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modregistry_test"; src = 'modregistry_test.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modregistry_choices.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; defs = '/DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'snapmap-plus\mods' } else { "" }) }
    @{ name = "process_heap_scope_test"; src = 'process_heap_scope_test.c'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    @{ name = "navmesh_test"; src = 'navmesh_test.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; defs = '/DSH_NAVMESH_TESTING'; arg = "" }
    @{ name = "nav_regions_test"; src = 'nav_regions_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; defs = '/DSH_NAVR_TESTING'; arg = "" }
    @{ name = "nav_snapshot_test"; src = 'nav_snapshot_test.c ..\src\backend\nav_snapshot.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "nav_entitydef_test"; src = 'nav_entitydef_test.c ..\src\backend\nav_entitydef.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "aas_edit_test"; src = 'aas_edit_test.c ..\src\backend\aas_edit.c'; arg = "" }
    @{ name = "nav_geometry_test"; src = 'nav_geometry_test.c ..\src\backend\nav_geometry.c'; arg = "" }
    @{ name = "aas_augment_test"; src = 'aas_augment_test.c ..\src\backend\aas_augment.c ..\src\backend\nav_overlay.c ..\src\backend\nav_geometry.c ..\src\backend\aas_edit.c ..\src\backend\nav_traversal.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; defs = '/DSH_NAVMESH_TESTING /DSH_AUG_TESTING /DSH_TRAV_TESTING'; arg = "" }
    @{ name = "nav_bake_test"; src = 'nav_bake_test.c ..\src\backend\perf.c ..\src\backend\nav_bake.c ..\src\backend\nav_overlay.c ..\src\backend\nav_regions.c ..\src\backend\nav_entitydef.c ..\src\backend\aas_edit.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\nav_traversal.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\sha256.c'; defs = '/DSH_NAV_BAKE_TESTING /DSH_AUG_TESTING'; arg = "" }
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
    @{ name = "rawmap_picker_test"; src = 'rawmap_picker_test.cpp'; cxx = $true; arg = "" }
    @{ name = "prefab_host_test"; src = 'prefab_host_test.cpp ..\src\backend\prefab_block.c'; cxx = $true; arg = "" }
    @{ name = "player_text_test"; src = 'player_text_test.cpp'; cxx = $true; arg = "" }
    @{ name = "preview_test";     src = 'preview_test.c ..\src\backend\preview.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "clipboard_test";   src = 'clipboard_test.c ..\src\backend\clipboard.c';          arg = "" }
    @{ name = "swf_textedit_test"; src = 'swf_textedit_test.c';                                   arg = "" }
    @{ name = "target_any_test";  src = 'target_any_test.c';                                      arg = "" }
    @{ name = "smoke_test";       src = 'smoke_test.c ..\src\backend\smoke.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\hook.c ..\src\backend\patch.c'; arg = "" }
    @{ name = "backend_log_test"; src = 'backend_log_test.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "bcn_test";         src = 'bcn_test.c ..\src\backend\bcn.c';                      arg = "" }
    @{ name = "soundpreview_queue_test"; src = 'soundpreview_queue_test.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "imgpreview_index_test"; src = 'imgpreview_index_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "imgpreview_catalog_test"; src = 'imgpreview_catalog_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "prefabpreview_test"; src = 'prefabpreview_test.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "prefab_grab_test"; src = 'prefab_grab_test.c ..\src\backend\prefab_grab.c';          arg = "" }
    @{ name = "megapreview_test";  src = 'megapreview_test.c ..\src\common\snapmap_plus_iface.c'; arg = "" }
    @{ name = "decltree_test"; src = 'decltree_test.c ..\src\backend\decl_tree.c ..\src\backend\modstock.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\sha256.c'; libs = 'shell32.lib ole32.lib'; arg = @($(if ($Doom) { "doom=" + (Split-Path -Parent $Doom) } else { "nodoom" }), $(if ($env:LOCALAPPDATA) { "mods=" + (Join-Path $env:LOCALAPPDATA 'snapmap-plus\mods') } else { "nomods" })) }
    @{ name = "modbundle_test"; src = 'modbundle_test.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    @{ name = "modslice_test"; src = 'modslice_test.c ..\src\backend\modslice.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; arg = "" }
    @{ name = "modstore_test"; src = 'modstore_test.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; defs = '/DSH_MODSTORE_TESTING'; libs = 'shell32.lib ole32.lib'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    # The builder, serving index, map manifest and legacy converter linked together.
    @{ name = "mod_pipeline_test"; src = 'mod_pipeline_test.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; libs = 'shell32.lib ole32.lib'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    # The retired overrides\ tree moved into the mods\ store, over a synthetic tree.
    @{ name = "modmigrate_test"; src = 'modmigrate_test.c mod_seam_stub.c ..\src\store\modmigrate.c ..\src\store\modlegacy.c ..\src\backend\modstore.c ..\src\store\modstore_upgrade.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\store\modbundle_old.c ..\src\backend\companion.c ..\src\backend\modmanifest.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\store\map_old.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "modstock_migrate_test"; src = 'modstock_migrate_test.c ..\src\backend\modstock.c mod_seam_stub.c ..\src\store\modmigrate.c ..\src\store\modlegacy.c ..\src\backend\modstore.c ..\src\store\modstore_upgrade.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\store\modbundle_old.c ..\src\backend\companion.c ..\src\backend\modmanifest.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\store\map_old.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "store_upgrade_test"; src = 'store_upgrade_test.c ..\src\store\store_seam.c ..\src\backend\modstore.c ..\src\store\modstore_upgrade.c ..\src\store\modmigrate.c ..\src\store\modlegacy.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\store\modbundle_old.c ..\src\backend\companion.c ..\src\backend\modstock.c ..\src\backend\modmanifest.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\store\map_old.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; defs = '/I..\src\store /DSH_MODSTORE_TESTING'; libs = 'shell32.lib ole32.lib advapi32.lib bcrypt.lib'; arg = (Join-Path $here 'fixtures\mods\alex.r2-imp') }
    @{ name = "store_tool_test"; src = 'store_tool_test.c ..\src\store\store_tool.c ..\src\store\store_recycle.c ..\src\store\store_seam.c ..\src\store\store_convert.c ..\src\store\store_saves.c ..\src\store\store_deflate.c ..\src\backend\modbankinfo.c ..\src\backend\modcontent.c ..\src\backend\modscene_decl.c ..\src\backend\prefab_block.c ..\src\backend\modstore.c ..\src\store\modstore_upgrade.c ..\src\store\modmigrate.c ..\src\store\modlegacy.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\store\modbundle_old.c ..\src\backend\companion.c ..\src\backend\modstock.c ..\src\backend\modmanifest.c ..\src\store\map_legacy_payload.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\store\map_old.c ..\src\store\store_old_ids.c ..\src\backend\sha256.c'; defs = '/I..\src\store /DSH_MODSTORE_TESTING'; libs = 'shell32.lib ole32.lib advapi32.lib bcrypt.lib'; arg = (Join-Path $here 'fixtures\store-retired\data') }
    @{ name = "modshadow_test"; src = 'modshadow_test.c ..\src\backend\modstock.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; defs = '/DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = "" }
    # The sh_vt diagnostic's helpers, and with SH_VT_RCTEST the reference family bundle end to end.
    @{ name = "vt_harness_test"; src = 'vt_harness_test.c ..\src\backend\vt_harness.c ..\src\backend\modvt.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\sha256.c'; defs = '/DSH_VT_HARNESS_PURE /DSH_MODVT_PURE /DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = $(if ($env:SH_VT_RCTEST) { $env:SH_VT_RCTEST } else { "" }) }
    # The mod store client, upload and download, and the rawmap ZIP export, against a loopback stub of the store.
    @{ name = "plain_reason_test"; src = 'plain_reason_test.c'; arg = "" }
    @{ name = "feature_health_test"; src = 'feature_health_test.c ..\src\backend\feature_health.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_FEATURE_HEALTH_TESTING'; arg = "" }
    @{ name = "diag_status_test"; src = 'diag_status_test.c ..\src\backend\diag_status.c ..\src\backend\feature_health.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_DIAG'; arg = (Join-Path $here '..\src\backend') }
    @{ name = "xinput_proxy_test"; src = 'xinput_proxy_test.c ..\src\backend\xinput_proxy.c'; arg = "" }
    @{ name = "modzip_test"; src = 'modzip_test.c ..\src\backend\modzip.c ..\src\backend\raw_deflate.c'; arg = "" }
    @{ name = "modnet_test"; src = 'modnet_test.c ..\src\backend\modnet.c ..\src\backend\modzip.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modstock_test"; src = 'modstock_test.c ..\src\backend\modstock.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modzip.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modmodule_test"; src = 'modmodule_test.c ..\src\backend\module_prefab_map.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\modstock.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modzip.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\raw_deflate.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modsync_test"; serial = $true; src = 'modsync_test.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modnet.c ..\src\backend\modzip.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modpublish_test"; src = 'modpublish_test.c ..\src\backend\modpublish.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modzip.c ..\src\backend\raw_deflate.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; defs = '/DSH_MODPUBLISH_PURE /DSH_MODPUBLISH_TESTING /DSH_CONFIG_TESTING'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modexport_test"; src = 'modexport_test.c ..\src\backend\modexport.c ..\src\backend\modzip.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modnet.c ..\src\backend\prefab_block.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modscript_test"; src = 'modscript_test.c ..\src\backend\modexport.c ..\src\backend\modzip.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modnet.c ..\src\backend\prefab_block.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_source.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c ..\src\backend\sha256.c'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = (Join-Path $here 'fixtures\scripts') }
    @{ name = "prefab_mods_test"; src = 'prefab_mods_test.c ..\src\backend\prefab_mods.c ..\src\backend\prefab_block.c ..\src\backend\modpublished.c ..\src\backend\module_prefab_map.c ..\src\backend\modexport.c ..\src\backend\modzip.c ..\src\backend\modsync.c ..\src\backend\modslice.c ..\src\backend\modnet.c ..\src\backend\modstore.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\raw_deflate.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\common\snapmap_plus_iface.c ..\src\backend\sha256.c'; defs = '/DSH_PREFAB_MODS_PURE /DSH_PREFAB_MODS_TESTING /DSH_MODPUBLISHED_PURE /DSH_MODPUBLISHED_TESTING'; libs = 'winhttp.lib ws2_32.lib shell32.lib ole32.lib'; arg = "" }
    @{ name = "modui_test"; src = 'modui_test.c ..\src\backend\modui.c ..\src\backend\modbankinfo.c ..\src\backend\raw_deflate.c ..\src\backend\modmap.c ..\src\backend\modstore.c ..\src\backend\mp_json.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\modserve.c ..\src\backend\modregistry.c ..\src\backend\modregistry_choices.c ..\src\backend\modmanifest.c ..\src\backend\map_shards.c ..\src\backend\map_carrier.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\feature_health.c ..\src\common\snapmap_plus_iface.c ..\src\backend\modzip.c ..\src\backend\sha256.c'; defs = '/DSH_MODUI_PURE /DSH_MODUI_TESTING /DSH_MODSERVE_PURE'; libs = 'shell32.lib ole32.lib'; arg = @((Join-Path $here 'fixtures\mods\alex.r2-imp'), (Join-Path $here 'fixtures\mods_tab_payload_v4.json')) }
)
if ($Doom) {
    if (-not (Test-Path $Doom)) { throw "-Doom path not found: $Doom" }
    $da = (Resolve-Path $Doom).Path
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\companion_sigs.c ..\src\backend\modscene_sigs.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "globals_test"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
}
# The other renderer's executable gets the same checks against its own RVAs.
if ($DoomAlt) {
    if (-not (Test-Path $DoomAlt)) { throw "-DoomAlt path not found: $DoomAlt" }
    $alt = (Resolve-Path $DoomAlt).Path
    $tests += @{ name = "sig_test_alt"; src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\companion_sigs.c ..\src\backend\modscene_sigs.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $alt }
    $tests += @{ name = "globals_test_alt"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $alt }
}

# Executable identity and the raw-RVA gate; hashes the given images too.
$identityImages = @(@($da, $alt) | Where-Object { $_ })
$tests += @{ name = "build_identity_test"; src = 'build_identity_test.c ..\src\backend\host_image.c ..\src\backend\signatures.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Prologue length decoding; with images, the cs_dontuse stolen boundaries.
$tests += @{ name = "x64_len_test"; src = 'x64_len_test.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# The load-screen stop's engine sites: signatures, stolen window, vtable slot, call argument.
$tests += @{ name = "load_screen_stop_test"; src = 'load_screen_stop_test.c ..\src\backend\x64_len.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Cooked movies (FileResource Load slot) and bundle ammo displays; with images,
# their signatures, decoded engine calls, vtable slot and HUD call site.
$tests += @{ name = "modswf_test"; src = 'modswf_test.c ..\src\backend\modswf.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\sha256.c'; defs = '/DSH_MODSWF_PURE /DSH_MODSWF_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
$tests += @{ name = "modsound_test"; serial = $true; src = 'modsound_test.c ..\src\backend\modsound.c ..\src\backend\modbankinfo.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\sha256.c'; defs = '/DSH_MODSOUND_PURE /DSH_MODSOUND_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
$tests += @{ name = "modhud_test"; src = 'modhud_test.c ..\src\backend\modhud.c ..\src\backend\script\script_node_row.c ..\src\backend\config_json.c ..\src\backend\patch.c ..\src\backend\hook.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_MODHUD_TESTING /DSH_PATCH_TESTING /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib'; arg = $identityImages }
# Owner-bound companions: the grammar and transform; with images, the private
# signatures, CallSpawn's stolen bytes, the idEntity cast and Bind's error text.
# The engine side of companions against a faked engine table: the serving build, rebuilds at a safe point.
$tests += @{ name = "companion_engine_test"; src = 'companion_engine_test.c ..\src\backend\companion.c ..\src\backend\mp_json.c'; arg = "" }
$tests += @{ name = "companion_test"; src = 'companion_test.c ..\src\backend\companion.c ..\src\backend\companion_sigs.c ..\src\backend\modscene_sigs.c ..\src\backend\mp_json.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Mod scripts' engine side: the queues, the entity handles, the mod table and
# the tick's guards (pure); with images, the private signatures, the detour
# windows, the tick's caller and the handle resolver's vtable slots.
$tests += @{ name = "script_hooks_test"; src = 'script_hooks_test.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c'; defs = '/DSH_SCRIPT_PURE' }
$tests += @{ name = "script_gate_test"; src = 'script_gate_test.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\backend\script\script_sigs.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE' }
$tests += @{ name = "script_sigs_test"; src = 'script_sigs_test.c ..\src\backend\script\script_sigs.c ..\src\backend\x64_len.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Mod scripts' AI calls: the event catalogue against the 13954591 registry, argument fitting, move
# completion, and every call through real scripts over a fake engine (refusals as plain stop lines).
$tests += @{ name = "script_api_ai_test"; src = 'script_api_ai_test.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = (Join-Path $here 'fixtures\scripts\event_registry_13954591.txt') }
# With images: the AI calls' signatures, the park detour window, the path site's targets, idAI2's typeinfo
# and the move and trigger offsets.
$tests += @{ name = "script_ai_sigs_test"; src = 'script_ai_sigs_test.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Mod scenes blocks: the grammar and where a scene plays; with images, the two
# spawn signatures, the game's vtable slots that hold them and the show-post stub.
$tests += @{ name = "modcontent_test"; src = 'modcontent_test.c ..\src\backend\modcontent.c ..\src\backend\modscene_decl.c ..\src\backend\mp_json.c ..\src\backend\config_json.c'; arg = "" }
$tests += @{ name = "modscene_decl_test"; src = 'modscene_decl_test.c ..\src\backend\modscene_decl.c ..\src\backend\modscene_sigs.c ..\src\backend\mp_json.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Mod scripts: the Lua sandbox, bounds, stop lines, registration and dispatch, timers,
# teardown and the loader fed malformed text (no game; lstrlib.c through script_lstrlib.c).
$tests += @{ name = "script_sandbox_test"; src = 'script_sandbox_test.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = "" }
# Mod scripts end to end without the game: fixture mod folders read from disk, the runtime, the hooks'
# tables and queues, a runaway mod stopped while another keeps running, the console, the examples.
$tests += @{ name = "script_e2e_test"; src = 'script_e2e_test.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\backend\script\script_source.c ..\src\backend\script\script_console.c ..\src\backend\script\script_node.c ..\src\backend\script\script_node_row.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_api_combat.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\mp_json.c ..\src\backend\modmap.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = (Join-Path $here 'fixtures\scripts') }
# Mod script calls on entities, animation and the world against a fake engine: reads, owned writes and
# their refusals, spawn caps, entities_near, trace, animWeb requests and the node observed each frame.
$tests += @{ name = "script_api_test"; src = 'script_api_test.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = "" }
# Mod script combat calls and the damage, death, pain, animation and projectile records against a fake engine:
# damage, hitscan, radius damage, effects, sounds, joints, fire and steer, refusals, and the hooks' delivery.
$tests += @{ name = "script_api_combat_test"; src = 'script_api_combat_test.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_api_combat.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = "" }
# The engine sites of the combat calls and hooks, against the images: signatures, call graph, Explode, enum tables.
$tests += @{ name = "script_combat_sigs_test"; src = 'script_combat_sigs_test.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# The engine sites the mod-script API calls use, against the images: private signatures, class vtables, clock.
$tests += @{ name = "script_api_sigs_test"; src = 'script_api_sigs_test.c ..\src\backend\script\script_api_sigs.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# The Script node and map variables: which mods get the node, its generated row, the played map's names,
# the door example end to end, node():fire() and vars; with images, the FireOutput, variable-changed and Relay sites.
$tests += @{ name = "script_reflect_test"; src = 'script_reflect_test.c ..\src\backend\script\script_reflect.c'; arg = "" }
$tests += @{ name = "script_api_reflect_test"; src = 'script_api_reflect_test.c ..\src\backend\script\script_reflect.c ..\src\backend\script\script_api_reflect.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = "" }
$tests += @{ name = "script_api_game_test"; src = 'script_api_game_test.c ..\src\backend\script\script_api_game.c ..\src\backend\script\script_reflect.c ..\src\backend\script\script_api_reflect.c ..\src\backend\script\script_api_common.c ..\src\backend\script\script_api_sigs.c ..\src\backend\script\script_api_entity.c ..\src\backend\script\script_api_world.c ..\src\backend\script\script_api_ai.c ..\src\backend\script\script_combat_sigs.c ..\src\backend\script\script_ai_sigs.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE'; arg = "" }
$tests += @{ name = "cmd_lists_test"; src = 'cmd_lists_test.c ..\src\backend\cmd_lists.c'; arg = "" }
$tests += @{ name = "script_node_test"; src = 'script_node_test.c ..\src\backend\script\script_node.c ..\src\backend\script\script_node_row.c ..\src\backend\script\script_runtime.c ..\src\backend\script\script_reason.c ..\src\backend\script\script_compile.c ..\src\backend\script\script_lib_base.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_lstrlib.c ..\src\backend\script\script_hooks.c ..\src\backend\script\script_sigs.c ..\src\backend\script\script_queue.c ..\src\backend\script\script_handles.c ..\src\backend\script\script_source.c ..\src\backend\mp_json.c ..\src\backend\modmap.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\third_party\lua\lapi.c ..\src\third_party\lua\lauxlib.c ..\src\third_party\lua\lbaselib.c ..\src\third_party\lua\lcode.c ..\src\third_party\lua\lcorolib.c ..\src\third_party\lua\lctype.c ..\src\third_party\lua\ldebug.c ..\src\third_party\lua\ldo.c ..\src\third_party\lua\ldump.c ..\src\third_party\lua\lfunc.c ..\src\third_party\lua\lgc.c ..\src\third_party\lua\llex.c ..\src\third_party\lua\lmathlib.c ..\src\third_party\lua\lmem.c ..\src\third_party\lua\lobject.c ..\src\third_party\lua\lopcodes.c ..\src\third_party\lua\lparser.c ..\src\third_party\lua\lstate.c ..\src\third_party\lua\lstring.c ..\src\third_party\lua\ltable.c ..\src\third_party\lua\ltablib.c ..\src\third_party\lua\ltm.c ..\src\third_party\lua\lundump.c ..\src\third_party\lua\lutf8lib.c ..\src\third_party\lua\lvm.c ..\src\third_party\lua\lzio.c'; defs = '/DSH_SCRIPT_PURE /DSH_HOST_IMAGE_TESTING'; arg = @(@((Join-Path $here 'fixtures\scripts')) + $identityImages) }
# Native idFile / idFileSystem slots the backend calls, against the images.
# Mod virtual-texture families against a fake record table, and with the executables the decoded VT surface.
$tests += @{ name = "modvt_test"; src = 'modvt_test.c ..\src\backend\modvt.c ..\src\backend\modserve.c ..\src\backend\modstore.c ..\src\backend\modregistry.c ..\src\backend\modbundle.c ..\src\backend\script\script_sdk_lua.c ..\src\backend\script\script_node_row.c ..\src\backend\strkeys.c ..\src\backend\modsalt.c ..\src\backend\companion.c ..\src\backend\mp_json.c ..\src\backend\config_json.c ..\src\backend\decl_text.c ..\src\backend\decl_tree.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\sha256.c'; defs = '/DSH_MODVT_PURE /DSH_MODSERVE_PURE /DSH_HOST_IMAGE_TESTING'; libs = 'shell32.lib ole32.lib'; arg = $identityImages }
$tests += @{ name = "native_slots_test"; src = 'native_slots_test.c ..\src\backend\x64_len.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $identityImages }
# Every fixed detour window against the images: boundary and relative operands.
$tests += @{ name = "detour_windows_test"; src = 'detour_windows_test.c ..\src\backend\x64_len.c ..\src\backend\companion_sigs.c ..\src\backend\modscene_sigs.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = @(@((Join-Path $here '..')) + $identityImages) }
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

# Each source compiles once per flag set into build\obj\tests\<flags-hash>\ (a suite whose defines
# differ gets its own object set), only when it or a project file it includes changed. Suites then
# link from those objects and run, all in parallel; output is captured per suite and printed in
# manifest order.
$cache = Join-Path $repo "build\obj\tests"
$incDirs = @("src\backend", "src\common", "src\fault_shield", "src\ui\webview") | ForEach-Object { Join-Path $repo $_ }
$sha = [System.Security.Cryptography.SHA256]::Create()
$units = [ordered]@{}
foreach ($t in $tests) {
    $defs = if ($t.defs) { " $($t.defs)" } else { "" }
    $cxx  = if ($t.cxx)  { " /EHsc /std:c++17" } else { "" }
    $flags = "/nologo /O2 /MT /I..\src\backend /I..\src\common /I..\src\fault_shield /I..\src\ui\webview$cxx$defs"
    $hash = (-join ($sha.ComputeHash([System.Text.Encoding]::ASCII.GetBytes($flags)) | ForEach-Object { $_.ToString("x2") })).Substring(0, 12)
    $setDir = Join-Path $cache $hash
    $t.objs = @()
    foreach ($src in @($t.src -split '\s+' | Where-Object { $_ })) {
        # Name the object by its path so same-named files in different folders cannot collide.
        $stem = ($src -replace '^(\.\.\\)+', '' -replace '[\\/:]', '_') -replace '\.(c|cpp)$', ''
        $unit = Join-Path $setDir ($stem + ".obj")
        $t.objs += $unit
        if (-not $units.Contains($unit)) { $units[$unit] = @{ src = $src; flags = $flags; obj = $unit } }
    }
    $t.cxx = $cxx
}
$jobs = @()
foreach ($u in @($units.Values | Where-Object { $_ })) {
    $u.ok = $true
    $u.key = Get-CbInputKey (Join-Path $here $u.src) $incDirs $u.flags
    if ((Get-CbObjectKey $u.obj) -eq $u.key) { continue }
    New-Item -ItemType Directory -Force (Split-Path -Parent $u.obj) | Out-Null
    Remove-Item $u.obj, "$($u.obj).key" -Force -ErrorAction SilentlyContinue
    $u.log = $u.obj -replace '\.obj$', '.log'
    $jobs += @{ dir = $here; cmd = "cl /c $($u.flags) $($u.src) /Fo`"$($u.obj)`""; log = $u.log; unit = $u }
}
$codes = Invoke-CbParallel $jobs
for ($i = 0; $i -lt $jobs.Count; $i++) {
    if ($codes[$i] -ne 0) { $jobs[$i].unit.ok = $false }
    else { [System.IO.File]::WriteAllText("$($jobs[$i].unit.obj).key", $jobs[$i].unit.key) }
}
Write-Host ("[tests] compiled {0} of {1} objects" -f $jobs.Count, @($units.Values | Where-Object { $_ }).Count)

# Link each suite whose objects compiled, unless its executable was linked by the same command
# from objects with the same keys.
$fail = 0
$linkJobs = @()
foreach ($t in $tests) {
    $bad = @($t.objs | Where-Object { -not $units[$_].ok })
    if ($bad.Count) {
        foreach ($o in $bad) { Get-Content $units[$o].log | Write-Host }
        Write-Host "[FAIL] compile $($t.name)"; $fail++; $t.skip = $true; continue
    }
    $exe = Join-Path $obj ($t.name + ".exe")
    $libs = if ($t.libs) { " /link $($t.libs)" } else { "" }
    # The store tool's path buffers hold any Windows path; its sources link the stack its build gives it.
    if ($t.src -match '\\src\\store\\') { $libs = if ($libs) { "$libs /STACK:67108864" } else { " /link /STACK:67108864" } }
    $objArgs = ($t.objs | ForEach-Object { '"' + $_ + '"' }) -join " "
    $cmd = "cl /nologo /O2 /MT$($t.cxx) $objArgs /Fe:obj\$($t.name).exe$libs"
    $record = Join-Path $obj ($t.name + ".link.txt")
    $want = Get-CbLinkRecord $cmd $t.objs
    if (-not (Test-Path $exe) -or -not (Test-Path $record) -or [System.IO.File]::ReadAllText($record) -ne $want) {
        Remove-Item $exe, $record -Force -ErrorAction SilentlyContinue
        $linkJobs += @{ dir = $here; cmd = $cmd; log = (Join-Path $obj ($t.name + ".build.log")); test = $t; record = $record; want = $want }
    }
}
$codes = Invoke-CbParallel $linkJobs
for ($i = 0; $i -lt $linkJobs.Count; $i++) {
    $j = $linkJobs[$i]
    if ($codes[$i] -ne 0) {
        Get-Content $j.log | Write-Host; Write-Host "[FAIL] compile $($j.test.name)"; $fail++; $j.test.skip = $true
    } else { [System.IO.File]::WriteAllText($j.record, $j.want) }
}

# Run the native suites, and the JavaScript suites unless -Only selected native ones, side by side
# from the caller's directory; output prints in manifest order. Each suite names its temporary
# folders by process id, so suites running together do not share them. The longest suites of the
# previous run start first. A suite marked serial asserts on wall-clock timing (a file held for a
# fixed time, a thread expected inside a short sleep) and runs alone after the others.
$cwd = (Get-Location).ProviderPath
$timesPath = Join-Path $cache "durations.txt"
$times = @{}
if (Test-Path $timesPath) {
    foreach ($line in [System.IO.File]::ReadAllLines($timesPath)) {
        $f = $line.Split("`t"); if ($f.Count -eq 2) { $times[$f[0]] = [double]$f[1] }
    }
}
$runJobs = @()
foreach ($t in @($tests | Where-Object { -not $_.skip })) {
    $exe = Join-Path $obj ($t.name + ".exe")
    $argText = (@($t.arg) | Where-Object { $_ } | ForEach-Object { '"' + $_ + '"' }) -join " "
    $runJobs += @{ dir = $cwd; cmd = "`"$exe`" $argText"; log = (Join-Path $obj ($t.name + ".run.log")); name = $t.name; serial = $t.serial }
}
$nativeCount = $runJobs.Count
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "decl_language_test.js", "ui_assets_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js", "feedback_renderer_test.js", "feedback_game_test.js", "issues_retest_test.js", "worker_body_test.js", "rawmap_menu_test.js", "delete_result_test.js", "prefab_tree_test.js", "timeline_tree_test.js", "timeline_eventcall_test.js", "prefab_budget_test.js", "mods_tab_test.js", "prefab_mods_test.js", "prefab_details_test.js", "prefab_rename_test.js", "timeline_target_test.js", "page_keys_test.js", "page_messages_test.js")
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $Only.Count -and $node) {
    foreach ($jsTest in $jsTests) {
        $runJobs += @{ dir = $cwd; cmd = "`"$($node.Source)`" `"$(Join-Path $here $jsTest)`""; log = (Join-Path $obj ($jsTest + ".run.log")); name = $jsTest }
    }
}
foreach ($j in $runJobs) { $j.weight = $times[$j.name] }
$codes = New-Object int[] $runJobs.Count
$together = @(0..($runJobs.Count - 1) | Where-Object { $runJobs.Count -and -not $runJobs[$_].serial })
$alone = @(0..($runJobs.Count - 1) | Where-Object { $runJobs.Count -and $runJobs[$_].serial })
$c = Invoke-CbParallel @($together | ForEach-Object { $runJobs[$_] })
for ($k = 0; $k -lt $together.Count; $k++) { $codes[$together[$k]] = $c[$k] }
foreach ($i in $alone) { $codes[$i] = (Invoke-CbParallel @($runJobs[$i]) 1)[0] }
foreach ($j in $runJobs) { $times[$j.name] = $j.seconds }
$lines = foreach ($k in $times.Keys) { "{0}`t{1:0.00}" -f $k, $times[$k] }
[System.IO.File]::WriteAllLines($timesPath, [string[]]$lines)

for ($i = 0; $i -lt $nativeCount; $i++) {
    $j = $runJobs[$i]
    Get-Content $j.log | Write-Host
    if ($codes[$i] -ne 0) { Write-Host "[FAIL] $($j.name) (exit $($codes[$i]))"; $fail++ }
    else { Write-Host "[ok]   $($j.name)" }
}
if ($fail -gt 0) { Write-Host ""; Write-Host "$fail native test(s) FAILED"; exit 1 }
Write-Host ""; Write-Host "all native tests passed ($($tests.Count))"

if ($Only.Count) { exit 0 }

if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsFail = 0
for ($i = $nativeCount; $i -lt $runJobs.Count; $i++) {
    $j = $runJobs[$i]
    Get-Content $j.log | Write-Host
    if ($codes[$i] -ne 0) { Write-Host "[FAIL] $($j.name) (exit $($codes[$i]))"; $jsFail++ }
    else { Write-Host "[ok]   $($j.name)" }
}
if ($jsFail -gt 0) { exit 1 }
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
