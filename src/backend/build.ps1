# Build the x64 XINPUT1_4.dll backend with Visual Studio 2022 C++ Build Tools.
# Run: pwsh -File build.ps1 [-Out XINPUT1_4.dll] [-Diag] [-VcVarsVer <version>]
# Add backend translation units to Sources below. Keep this script ASCII for PS 5.1.
param(
    [string[]]$Sources = @("dllmain.c", "host_image.c", "signatures.c", "feature_health.c", "engine_globals.c", "hook.c", "smoke.c",
                           "rawmap.c", "modmanifest.c", "user_dirs.c", "editor_frame.c", "grid_room.c", "grid_room_resources.c", "grid_room_decl.c", "map_shards.c", "map_carrier.c", "map_payload.c", "navmesh.c", "nav_regions.c", "nav_entitydef.c", "nav_bake.c", "nav_menu.c", "nav_play.c", "nav_snapshot.c", "nav_traversal.c", "aas_edit.c", "aas_augment.c", "nav_geometry.c", "nav_overlay.c", "nav_preview.c", "engine_dialog.c", "strids.c",
                           "overrides.c", "grid_room_editor.c", "map_render.c", "map_render_native.c", "map_render_editor.c", "grid_room_asset.c", "grid_room_nav.c", "grid_room_native.c", "grid_room_areas.c", "world_area_cell.c", "object_entry.c", "place_anywhere.c", "grid_room_snap.c", "grid_room_edit.c", "raw_deflate.c", "decl_text.c", "decl_tree.c",
                           "cvars.c", "commands.c", "clipboard.c",
                           "config.c", "config_json.c",
                           "entity.c", "typeinfo.c", "preview.c", "megapreview.c", "imgpreview.c", "prefabpreview.c", "soundpreview.c", "bcn.c", "patch.c", "x64_len.c", "target_any.c", "wiring_cleandirect.c", "swf_textedit.c", "ui_bridge.c",
                           "iface_engine.c", "editor_delete.c", "editor_undo.c", "apply_engine.c", "prefab_grab.c", "fly_to.c", "module_prefab.c", "module_prefab_map.c", "prefab_mods.c", "prefab_block.c", "timeline_tile.c", "modserve.c", "modregistry.c", "modregistry_choices.c", "modserve_engine.c", "modtypeinfo.c", "modledger.c", "modswf.c", "modsound.c", "modhud.c", "modhud_scene.c", "modscene_place.c", "modscene_decl.c", "modcontent.c", "modscene_sigs.c", "modpublished.c", "modpublish.c", "modaction.c", "budget_table.c", "mp_json.c", "../common/snapmap_plus_iface.c",
                           "../common/log_rotate.c",
                           # The backend owns the SnapStack command handlers and stores.
                           "snapstack.c", "json_patch.c",
                           # Include the cvar unlocker in the backend; DOOM uses the native dinput8 DLL.
                           "cvar_unlock.c",
                           "backend_log.c", "perf.c", "xinput_proxy.c",
                           # The fault shield shares this backend's hook and signature implementations.
                           # Do not add the shield's duplicate hook.c or signatures.c.
                           "../fault_shield/veh.c", "../fault_shield/recovery.c",
                           "../fault_shield/fault_record.c", "../fault_shield/shield_sigs.c",
                           # Capture and format evidence for the crash-report dialog.
                           "../fault_shield/crash_record_format.c", "../fault_shield/crash_report.c",
                           # Guards for event-link walks and interactable spawning.
                           "../fault_shield/mapload_guards.c",
                           "../fault_shield/serialized_entities_guard.c",
                           # Stop a failed map load's loading screen at CancelMapChange.
                           "../fault_shield/load_screen_stop.c",
                           "../fault_shield/fault_shield.c",
                           # Map bounds, Blueprint zoom and the Snapmap+ settings tab.
                           "settings_tab.c", "map_bounds.c", "map_settings.c", "editor_zoom.c",
                           # Boot preload pacing (load-screen video lock, read poll).
                           "boot_pacing.c",
                           # Mod bundles, the local mods store, the sh_mods command and the Mods tab.
                           "modbundle.c", "modsalt.c", "strkeys.c", "modbankinfo.c", "modstock.c", "modstore.c", "modcmd.c", "modui.c", "modmap.c",
                           # The mod store client (WinHTTP), upload/download, the rawmap ZIP export and the ZIP form.
                           "modnet.c", "modsync.c", "modexport.c", "modzip.c",
                           # Mod virtual-texture families, and the sh_vt diagnostic.
                           "modvt.c", "vt_harness.c",
                           # Owner-bound companions a mod declares for its tiles.
                           "companion.c", "companion_sigs.c", "companion_engine.c",
                           # Mod scripts: the sandboxed Lua runtime and the vendored Lua 5.4 core
                           # (lstrlib.c is compiled only through script_lstrlib.c).
                           "script/script_runtime.c", "script/script_reason.c", "script/script_compile.c", "script/script_lib_base.c", "script/script_lstrlib.c",
                           # Mod scripts' engine side: the frame hook, map gates, entity handles and queues,
                           # the served folders' file source and the sh_scripts console command.
                           "script/script_hooks.c", "script/script_sigs.c", "script/script_queue.c", "script/script_handles.c",
                           "script/script_source.c", "script/script_console.c",
                           # Mod scripts: the entity, animation, world and time calls and their engine side.
                           "script/script_api_common.c", "script/script_api_sigs.c", "script/script_api_entity.c", "script/script_api_world.c",
                           # Mod scripts: the Script node (its row, the Relay hook, node():fire and vars).
                           "script/script_node.c", "script/script_node_row.c",
                           # Mod scripts' AI calls and the demon park.
                           "script/script_api_ai.c", "script/script_ai_sigs.c",
                           # Mod scripts: the combat calls (damage, projectiles, effects, sounds, joints, pain).
                           "script/script_api_combat.c", "script/script_combat_sigs.c",
                           "../third_party/lua/lapi.c", "../third_party/lua/lauxlib.c", "../third_party/lua/lbaselib.c", "../third_party/lua/lcode.c", "../third_party/lua/lcorolib.c", "../third_party/lua/lctype.c", "../third_party/lua/ldebug.c", "../third_party/lua/ldo.c", "../third_party/lua/ldump.c", "../third_party/lua/lfunc.c", "../third_party/lua/lgc.c", "../third_party/lua/llex.c", "../third_party/lua/lmathlib.c", "../third_party/lua/lmem.c", "../third_party/lua/lobject.c", "../third_party/lua/lopcodes.c", "../third_party/lua/lparser.c", "../third_party/lua/lstate.c", "../third_party/lua/lstring.c", "../third_party/lua/ltable.c", "../third_party/lua/ltablib.c", "../third_party/lua/ltm.c", "../third_party/lua/lundump.c", "../third_party/lua/lutf8lib.c", "../third_party/lua/lvm.c", "../third_party/lua/lzio.c"),
    [string]$Out = "XINPUT1_4.dll",
    # Diag adds SH_DIAG and the crash/environment logger for troubleshooting.
    [switch]$Diag,
    # Pin the MSVC toolset for reproducible release builds. An empty value uses
    # the installed default; SNAPMAPPLUS_VCVARS_VER shares the pin with the UI build.
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

. (Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "tools\cbuild.ps1")
$toolset = Import-MsvcEnv $VcVarsVer
Write-Host "[build] MSVC toolset $toolset"

# Accept comma-separated source names as well as a PowerShell array.
if ($Sources.Count -eq 1 -and $Sources[0] -match ",") { $Sources = $Sources[0].Split(",") }

# Include the optional logger; DllMain arms it when SH_DIAG is defined.
$defs = ""
$def = "xinput1_4.def"
if ($Diag) {
    # The cs_dontuse / sh_alginfo math overrides exist only in this build.
    # The development daemon's status export (diag_status.h), at ordinal 300.
    $Sources += "../fault_shield/shield_diag.c", "algo.c", "diag_status.c"
    $defs = "/DSH_DIAG"
    $def = "xinput1_4_diag.def"
    Write-Host "[build] DIAGNOSTIC variant: +shield_diag.c +algo.c +diag_status.c /DSH_DIAG, xinput1_4_diag.def"
}

# Compile from this directory with relative paths to avoid cmd quoting issues.
# xinput1_4.def is the sole export source and fixes native XInput ordinals (the
# diagnostic build uses xinput1_4_diag.def, the same set plus sh_diag_status).
# DOOM imports by ordinal; automatic alphabetical numbering calls wrong functions.
$implib  = $Out -replace '\.dll$', '.lib'   # Put import libraries and .exp files under build/obj/backend.
# Include the shared interface ABI used by the backend factory and paired UI DLL.
# shell32 supplies known-folder APIs; ole32 frees their returned allocations.
# DLL, PDB and map outputs go to the repository build directory.
# PDB and map files resolve crash offsets and ship as separate maintainer assets.
# /Z7 keeps compiler debug data in each object, avoiding a shared compiler PDB.
# /DEBUG:FULL emits linker symbols; /INCREMENTAL:NO and /OPT:REF,ICF explicitly
# restore release layout defaults that debug linking otherwise changes.
# /Brepro on compilation and linking removes timestamp-based output variation.
# /PDBALTPATH:%_PDB% records only the PDB filename, omitting local build paths.
# Sources compile to build/obj/backend (only those changed since the last build), then link.
$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"
$objDir = Join-Path $outDir "obj\backend"
$buildLog = Join-Path $outDir "build.log"
$objs = Invoke-CbCompile -WorkDir $here -Sources $Sources -Flags "/nologo /LD /O2 /W3 /MT /Z7 /Brepro $defs /I..\common" `
    -ObjDir $objDir -ObjDirArg "..\..\build\obj\backend\" -IncludeDirs @((Join-Path $here "..\common")) -Log $buildLog
$objArgs = ($objs | ForEach-Object { '"..\..\build\obj\backend\' + (Split-Path -Leaf $_) + '"' }) -join " "
$cl = "cl /nologo /LD /O2 /W3 /MT /Z7 /Brepro $objArgs /Fe:..\..\build\$Out " +
      "/link /DEF:$def /IMPLIB:..\..\build\obj\backend\$implib shell32.lib ole32.lib " +
      "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /Brepro /PDBALTPATH:%_PDB% /MAP"
Invoke-CbLink -WorkDir $here -Command $cl -Objects $objs -Output (Join-Path $outDir $Out) -Log (Join-Path $outDir "link.log") `
    -Remove @((Join-Path $outDir ($Out -replace '\.dll$', '.pdb')))
Write-Host "built $(Join-Path $outDir $Out)"
# Require both symbol artifacts so reported crash offsets remain resolvable.
foreach ($sym in @(($Out -replace '\.dll$', '.pdb'), ($Out -replace '\.dll$', '.map'))) {
    $symPath = Join-Path $outDir $sym
    if (-not (Test-Path $symPath)) { throw "expected symbol artifact missing: $symPath" }
    Write-Host ("  symbols: {0,-22} {1,10} bytes" -f $sym, (Get-Item $symPath).Length)
}
if ($Diag) {
    Write-Host "[build] *** DIAGNOSTIC build -- DO NOT DISTRIBUTE (troubleshooting only; writes sh_diag.log + sh_crash.dmp) ***"
} else {
    # Remove an earlier diagnostic object from the release object directory.
    Remove-Item (Join-Path $outDir "obj\backend\shield_diag.obj") -ErrorAction SilentlyContinue
}
