# Build snapmap-plus-store.exe, the store migration tool, with Visual Studio 2022 C++ Build Tools.
# It links the backend's store sources and no engine, DLL or WebView code (store_tool.h).
# Run: powershell -File build.ps1 [-VcVarsVer <version>]. Keep this script ASCII for PS 5.1.
param(
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$Sources = @("store_main.c", "store_tool.c", "store_recycle.c", "store_seam.c",
             "../backend/modstore.c", "../backend/modmigrate.c", "../backend/modlegacy.c", "../backend/modbundle.c",
             "../backend/modstock.c", "../backend/modmanifest.c", "../backend/map_legacy_payload.c",
             "../backend/map_shards.c", "../backend/raw_deflate.c", "../backend/mp_json.c",
             "../backend/config_json.c", "../backend/decl_text.c")

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
$vcvarsArgs = ""
if ($VcVarsVer) { $vcvarsArgs = " -vcvars_ver=$VcVarsVer" }

$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"
New-Item -ItemType Directory -Force (Join-Path $outDir "obj\store") | Out-Null
$srcArgs = ($Sources | ForEach-Object { '"' + $_ + '"' }) -join " "
# /MT links the C runtime statically so the tool runs on a machine without the VC redistributable.
$cl = "cl /nologo /O2 /W3 /MT /Brepro /Fo..\..\build\obj\store\ /I..\backend /I..\common $srcArgs " +
      "/Fe:..\..\build\snapmap-plus-store.exe /link /SUBSYSTEM:CONSOLE shell32.lib ole32.lib advapi32.lib /Brepro"
$cmd = "cd /d `"$here`" && `"$vcvars`"$vcvarsArgs && $cl"
$buildLog = Join-Path $outDir "store-build.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
Write-Host "built $(Join-Path $outDir 'snapmap-plus-store.exe')"
