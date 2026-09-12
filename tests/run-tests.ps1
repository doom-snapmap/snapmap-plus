# Compile and run the offline C/C++ and JavaScript tests with MSVC x64 and Node.
# Requires Visual Studio C++ Build Tools. Outputs go to tests\obj (gitignored).
#
#   tests\run-tests.ps1
#   tests\run-tests.ps1 -Doom <unpacked Vulkan exe> -DoomAlt <unpacked OpenGL exe>
#
# The default suite needs no game or built DLL. -Doom adds pinned signature,
# hook-fallback and global checks; -DoomAlt checks the second image in portable
# mode, accepting address shifts while requiring unique matches and layout checks.
# Exit 0 means every selected test passed; failures include their build log.
#
# Optional manual probes:
#   tests\obj\resource_bridge_test.exe <data-root> <doom-base>
# Both resource paths are required; there is no autodiscovery.
# For xinput_ordinal_test against the built proxy, see docs\contributing.md.
param([string]$Doom = "", [string]$DoomAlt = "")
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
    @{ name = "map_render_test"; src = 'map_render_test.c ..\src\backend\map_render.c'; arg = "" }
    @{ name = "map_render_editor_test"; src = 'map_render_editor_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "map_render_native_test"; src = 'map_render_native_test.c ..\src\backend\map_render.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_nav_test"; src = 'grid_room_nav_test.c ..\src\backend\grid_room_nav.c ..\src\backend\grid_room.c ..\src\backend\aas_edit.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "grid_room_asset_test"; src = 'grid_room_asset_test.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_test"; src = 'grid_room_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "grid_room_editor_test"; src = 'grid_room_editor_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_native_test"; src = 'grid_room_native_test.c ..\src\backend\grid_room.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c ..\src\backend\grid_room_edit.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "grid_room_snap_test"; src = 'grid_room_snap_test.c ..\src\backend\grid_room.c ..\src\backend\patch.c ..\src\backend\hook.c'; arg = "" }
    @{ name = "nav_heap_test"; src = 'nav_heap_test.c'; arg = "" }
    @{ name = "nav_heap_hook_test"; src = 'nav_heap_hook_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "patch_test"; src = 'patch_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "json_patch_test"; src = 'json_patch_test.c ..\src\backend\json_patch.c'; arg = "" }
    @{ name = "edit_pair_test"; src = 'edit_pair_test.c'; arg = "" }
    @{ name = "engine_cvar_read_test"; src = 'engine_cvar_read_test.c'; arg = "" }
    @{ name = "apply_dispatch_test"; src = 'apply_dispatch_test.c ..\src\backend\perf.c'; defs = '/Gy'; arg = "" }
    @{ name = "snapstack_pair_test"; src = 'snapstack_pair_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "crash_report_test"; src = 'crash_report_test.c ..\src\fault_shield\crash_record_format.c ..\src\backend\config_json.c'; arg = "" }
    @{ name = "recovery_dialog_test"; src = 'recovery_dialog_test.c'; defs = '/Gy'; arg = "" }
    @{ name = "nav_play_test"; src = 'nav_play_test.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_PATCH_TESTING'; arg = "" }
    @{ name = "weapon_hud_test"; src = 'weapon_hud_test.c ..\src\backend\weapon_hud.c ..\src\backend\packages.c ..\src\backend\config_json.c ..\src\backend\patch.c ..\src\backend\hook.c'; defs = '/DSH_WEAPON_HUD_TESTING /DSH_PACKAGES_TESTING /DSH_PATCH_TESTING'; arg = "" }
    @{ name = "shield_format_test"; src = 'shield_format_test.c ..\src\fault_shield\fault_record.c ..\src\common\log_rotate.c'; arg = "" }
    @{ name = "hook_test";          src = 'hook_test.c ..\src\backend\hook.c ..\src\backend\patch.c'; defs = '/DSH_PATCH_TESTING';                       arg = "" }
    @{ name = "crash_record_test";  src = 'crash_record_test.c ..\src\fault_shield\crash_record_format.c'; arg = "" }
    @{ name = "report_scrub_test";  src = 'report_scrub_test.c';                                     arg = "" }
    @{ name = "dumpmap_path_test";  src = 'dumpmap_path_test.c';                                     arg = "" }
    @{ name = "json_pretty_test";   src = 'json_pretty_test.c';                                      arg = "" }
    @{ name = "editor_frame_test"; src = 'editor_frame_test.c'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "rawmap_paths_test";  src = 'rawmap_paths_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c ..\src\backend\hook.c ..\src\backend\patch.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_RAWMAP_TESTING /DSH_CONFIG_TESTING /DSH_PATCH_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "config_json_test";   src = 'config_json_test.c ..\src\backend\config_json.c';         arg = "" }
    @{ name = "iface_config_test";  src = 'iface_config_test.c ..\src\common\snapmap_plus_iface.c';   arg = "" }
    @{ name = "config_test";        src = 'config_test.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_test"; src = 'user_overrides_test.c ..\src\backend\user_overrides.c ..\src\backend\config.c ..\src\backend\config_json.c ..\src\common\snapmap_plus_iface.c'; defs = '/DSH_CONFIG_TESTING /DSH_USER_OVERRIDES_TESTING'; libs = 'shell32.lib ole32.lib'; arg = "" }
    @{ name = "user_overrides_contract_test"; src = 'user_overrides_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "decl_server_test"; src = 'decl_server_test.c ..\src\backend\decl_server.c ..\src\backend\engine_dialog.c ..\src\backend\packages.c ..\src\backend\decl_server_path.c ..\src\backend\decl_text.c'; defs = '/DSH_DECL_SERVER_TESTING'; arg = "" }
    @{ name = "overrides_internal_test"; src = 'overrides_internal_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "decl_server_contract_test"; src = 'decl_server_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "palette_refresh_test"; src = 'palette_refresh_test.c ..\src\backend\palette_refresh.c'; defs = '/DSH_PALETTE_REFRESH_TESTING'; arg = "" }
    @{ name = "process_heap_scope_test"; src = 'process_heap_scope_test.c'; arg = "" }
    @{ name = "engine_dialog_test"; src = 'engine_dialog_test.c ..\src\backend\engine_dialog.c'; defs = '/DSH_ENGINE_DIALOG_TESTING'; arg = "" }
    @{ name = "package_conflicts_test"; src = 'package_conflicts_test.c ..\src\backend\package_conflicts.c ..\src\backend\packages.c'; arg = "" }
    @{ name = "palette_refresh_contract_test"; src = 'palette_refresh_contract_test.c'; arg = (Join-Path $here '..') }
    @{ name = "resource_bridge_test"; src = 'resource_bridge_test.c ..\src\backend\resource_bridge.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c ..\src\backend\decl_text.c'; defs = '/DSH_RESOURCE_BRIDGE_TESTING /DSH_RAW_DEFLATE_TESTING'; arg = "" }
    @{ name = "packages_test"; src = 'packages_test.c ..\src\backend\packages.c'; defs = '/DSH_PACKAGES_TESTING'; arg = "" }
    @{ name = "map_package_test"; src = 'map_package_test.c ..\src\backend\map_package.c ..\src\backend\map_shards.c ..\src\backend\packages.c ..\src\backend\raw_deflate.c'; defs = '/DSH_MAP_PACKAGE_TESTING'; arg = "" }
    @{ name = "navmesh_test"; src = 'navmesh_test.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING'; arg = "" }
    @{ name = "nav_regions_test"; src = 'nav_regions_test.c ..\src\backend\nav_regions.c ..\src\backend\map_shards.c'; arg = "" }
    @{ name = "aas_edit_test"; src = 'aas_edit_test.c ..\src\backend\aas_edit.c'; arg = "" }
    @{ name = "nav_geometry_test"; src = 'nav_geometry_test.c ..\src\backend\nav_geometry.c'; arg = "" }
    @{ name = "aas_augment_test"; src = 'aas_augment_test.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\aas_edit.c ..\src\backend\nav_traversal.c ..\src\backend\navmesh.c ..\src\backend\map_shards.c'; defs = '/DSH_NAVMESH_TESTING /DSH_AUG_TESTING /DSH_TRAV_TESTING'; arg = "" }
    @{ name = "nav_bake_test"; src = 'nav_bake_test.c ..\src\backend\perf.c ..\src\backend\nav_bake.c ..\src\backend\nav_regions.c ..\src\backend\aas_edit.c ..\src\backend\aas_augment.c ..\src\backend\nav_geometry.c ..\src\backend\nav_traversal.c ..\src\backend\map_shards.c'; defs = '/DSH_NAV_BAKE_TESTING'; arg = "" }
    @{ name = "nav_traversal_test"; src = 'nav_traversal_test.c ..\src\backend\nav_traversal.c'; defs = '/DSH_TRAV_TESTING'; arg = "" }
    @{ name = "override_packages_test"; src = 'override_packages_test.c ..\src\backend\perf.c ..\src\backend\overrides.c ..\src\backend\packages.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    # Link the real globals resolver to verify refusal when this non-game process
    # cannot provide the load-state address.
    @{ name = "package_requirements_test"; src = 'package_requirements_test.c ..\src\backend\package_requirements.c ..\src\backend\packages.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_PACKAGE_REQUIREMENTS_TESTING'; arg = "" }
    @{ name = "strids_packages_test"; src = 'strids_packages_test.c ..\src\backend\perf.c ..\src\backend\strids.c ..\src\backend\packages.c ..\src\backend\overrides.c ..\src\backend\decl_text.c ..\src\backend\grid_room_asset.c ..\src\backend\grid_room_nav.c ..\src\backend\aas_edit.c ..\src\backend\grid_room.c ..\src\backend\grid_room_resources.c ..\src\backend\grid_room_decl.c ..\src\backend\config_json.c'; defs = '/DSH_STRIDS_TESTING /DSH_OVERRIDES_TESTING'; libs = 'shell32.lib'; arg = "" }
    @{ name = "config_message_test"; src = 'config_message_test.cpp ..\src\ui\webview\config_message.cpp'; cxx = $true; arg = "" }
    @{ name = "webview_json_test"; src = 'webview_json_test.cpp ..\src\ui\webview\webview_json.cpp'; cxx = $true; arg = "" }
    @{ name = "crash_pending_test"; src = 'crash_pending_test.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_bootstrap_test"; src = 'theme_bootstrap_test.cpp ..\src\ui\webview\theme_bootstrap.cpp'; cxx = $true; arg = "" }
    @{ name = "theme_contract_test"; src = 'theme_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "entity_settings_contract_test"; src = 'entity_settings_contract_test.c'; arg = (Join-Path $here '..\src\ui\webview\mockup.html') }
    @{ name = "growing_text_buffer_test"; src = 'growing_text_buffer_test.cpp'; cxx = $true; arg = "" }
    @{ name = "preview_test";     src = 'preview_test.c ..\src\backend\preview.c';              arg = "" }
    @{ name = "bcn_test";         src = 'bcn_test.c ..\src\backend\bcn.c';                      arg = "" }
    @{ name = "soundpreview_queue_test"; src = 'soundpreview_queue_test.c';                       arg = "" }
    @{ name = "imgpreview_index_test"; src = 'imgpreview_index_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';      arg = "" }
    @{ name = "imgpreview_catalog_test"; src = 'imgpreview_catalog_test.c ..\src\backend\bcn.c ..\src\backend\raw_deflate.c';  arg = "" }
    @{ name = "prefabpreview_test"; src = 'prefabpreview_test.c';                                  arg = "" }
    @{ name = "megapreview_io_test"; src = 'megapreview_io_test.c';                                 arg = "" }
    @{ name = "serialization_buffer_test"; src = 'serialization_buffer_test.cpp'; cxx = $true;     arg = "" }
)
if ($Doom) {
    if (-not (Test-Path $Doom)) { throw "-Doom path not found: $Doom" }
    $da = (Resolve-Path $Doom).Path
    $tests += @{ name = "sig_test";     src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c';     arg = $da }
    $tests += @{ name = "hooktol_test"; src = 'hooktol_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; defs = '/DSH_HOST_IMAGE_TESTING'; arg = $da }
    $tests += @{ name = "globals_test"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = $da }
}
# Check the second executable without requiring the pinned image's addresses.
if ($DoomAlt) {
    if (-not (Test-Path $DoomAlt)) { throw "-DoomAlt path not found: $DoomAlt" }
    $alt = (Resolve-Path $DoomAlt).Path
    $tests += @{ name = "sig_test_alt"; src = 'sig_test.c ..\src\backend\signatures.c ..\src\backend\host_image.c'; arg = @($alt, "portable") }
    $tests += @{ name = "globals_test_alt"; src = 'globals_test.c ..\src\backend\engine_globals.c ..\src\backend\signatures.c ..\src\backend\host_image.c ..\src\backend\backend_log.c ..\src\backend\perf.c ..\src\common\log_rotate.c'; arg = @($alt, "portable") }
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

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[FAIL] node not found (required for decl editor tests)"; exit 1 }
$jsTests = @("decl_overlay_test.js", "decl_index_order_test.js", "decl_enum_values_test.js", "decl_language_test.js", "ui_assets_test.js", "asset_browser_test.js", "entity_list_test.js", "prefab_transform_test.js", "prefab_viewport_contract_test.js", "window_chrome_contract_test.js", "feedback_channel_test.js", "feedback_renderer_test.js", "worker_body_test.js", "worker_quota_test.js", "rawmap_menu_test.js")
foreach ($jsTest in $jsTests) {
    & $node.Source (Join-Path $here $jsTest)
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $jsTest (exit $LASTEXITCODE)"; exit 1 }
    Write-Host "[ok]   $jsTest"
}
Write-Host "all JavaScript tests passed ($($jsTests.Count))"
