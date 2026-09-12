# Build the x64 XINPUT1_3.dll backend with Visual Studio 2022 C++ Build Tools.
# Run: pwsh -File build.ps1 [-Out XINPUT1_3.dll] [-Diag] [-VcVarsVer <version>]
# Add backend translation units to Sources below. Keep this script ASCII for PS 5.1.
param(
    [string[]]$Sources = @("dllmain.c", "host_image.c", "signatures.c", "engine_globals.c", "hook.c", "smoke.c",
                           "rawmap.c", "editor_frame.c", "grid_room.c", "grid_room_resources.c", "grid_room_decl.c", "map_shards.c", "map_package.c", "map_embed.c", "navmesh.c", "nav_regions.c", "nav_bake.c", "nav_play.c", "nav_traversal.c", "aas_edit.c", "aas_augment.c", "nav_geometry.c", "nav_preview.c", "palette_refresh.c", "engine_dialog.c", "package_conflicts.c", "strids.c",
                           "overrides.c", "resource_bridge.c", "grid_room_editor.c", "map_render.c", "map_render_native.c", "map_render_editor.c", "grid_room_asset.c", "grid_room_nav.c", "grid_room_native.c", "grid_room_snap.c", "grid_room_edit.c", "package_requirements.c", "weapon_hud.c", "raw_deflate.c", "decl_text.c", "decl_server_path.c", "packages.c", "decl_server.c", "decl_visibility.c",
                           "user_overrides.c", "cvars.c", "commands.c", "clipboard.c",
                           "config.c", "config_json.c",
                           "entity.c", "typeinfo.c", "preview.c", "megapreview.c", "imgpreview.c", "prefabpreview.c", "soundpreview.c", "bcn.c", "patch.c", "algo.c", "target_any.c", "wiring_cleandirect.c", "swf_textedit.c", "ui_bridge.c",
                           "iface_engine.c", "apply_engine.c", "../common/snapmap_plus_iface.c",
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
                           "../fault_shield/fault_shield.c"),
    [string]$Out = "XINPUT1_3.dll",
    # Diag adds SH_DIAG and the crash/environment logger for troubleshooting.
    [switch]$Diag,
    # Pin the MSVC toolset for reproducible release builds. An empty value uses
    # the installed default; SNAPMAPPLUS_VCVARS_VER shares the pin with the UI build.
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Resolve and verify the requested toolset before invoking vcvars64.bat.
$vcvarsArgs = ""
$toolsetDir = Join-Path $vs "VC\Tools\MSVC"
if ($VcVarsVer) {
    if (-not (Test-Path (Join-Path $toolsetDir $VcVarsVer))) {
        $have = (Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ", "
        throw ("pinned MSVC toolset $VcVarsVer is not installed (have: $have). Install it via the VS " +
               "Installer, or update the pin in .github/workflows/release.yml (SNAPMAPPLUS_VCVARS_VER).")
    }
    $vcvarsArgs = " -vcvars_ver=$VcVarsVer"
    $toolset = $VcVarsVer + " (pinned)"
} else {
    # Without a pin, report the newest installed toolset for build diagnostics.
    $newest = Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($newest) { $toolset = $newest.Name + " (newest installed, NOT pinned)" }
    else { $toolset = "unknown (NOT pinned)" }
}
Write-Host "[build] MSVC toolset $toolset"

# Accept comma-separated source names as well as a PowerShell array.
if ($Sources.Count -eq 1 -and $Sources[0] -match ",") { $Sources = $Sources[0].Split(",") }

# Include the optional logger; DllMain arms it when SH_DIAG is defined.
$defs = ""
if ($Diag) {
    $Sources += "../fault_shield/shield_diag.c"
    $defs = "/DSH_DIAG"
    Write-Host "[build] DIAGNOSTIC variant: +shield_diag.c /DSH_DIAG"
}

# Compile from this directory with relative paths to avoid cmd quoting issues.
# xinput1_3.def is the sole export source and fixes native XInput ordinals.
# DOOM imports by ordinal; automatic alphabetical numbering calls wrong functions.
$srcArgs = ($Sources | ForEach-Object { '"' + $_.Trim() + '"' }) -join " "
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
$cl = "cl /nologo /LD /O2 /W3 /MT /Z7 /Brepro $defs /Fo..\..\build\obj\backend\ /I..\common $srcArgs /Fe:..\..\build\$Out " +
      "/link /DEF:xinput1_3.def /IMPLIB:..\..\build\obj\backend\$implib shell32.lib ole32.lib " +
      "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /Brepro /PDBALTPATH:%_PDB% /MAP"

$cmd = "cd /d `"$here`" && `"$vcvars`"$vcvarsArgs && $cl"
# Capture native stderr in the build log: PS 5.1 can otherwise treat harmless
# vcvars diagnostics as terminating errors. Determine success from LASTEXITCODE.
$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"
New-Item -ItemType Directory -Force (Join-Path $outDir "obj\backend") | Out-Null
$buildLog = Join-Path $outDir "build.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
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

