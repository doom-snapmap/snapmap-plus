# Build snapmap-plus-store.exe, the store migration tool, with Visual Studio 2022 C++ Build Tools.
# It links the backend's store sources and no engine, DLL or WebView code (store_tool.h).
# Run: powershell -File build.ps1 [-VcVarsVer <version>]. Keep this script ASCII for PS 5.1.
param(
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$Sources = @("store_main.c", "store_tool.c", "store_recycle.c", "store_seam.c", "store_convert.c", "store_saves.c",
             "store_deflate.c", "../backend/prefab_block.c",
             "../backend/modstore.c", "modstore_upgrade.c", "modmigrate.c", "modlegacy.c", "../backend/modbundle.c",
             "../backend/modsalt.c", "../backend/strkeys.c", "modbundle_old.c", "../backend/companion.c", "../backend/modstock.c",
             "../backend/modmanifest.c", "map_legacy_payload.c", "map_old.c",
             "../backend/map_shards.c", "../backend/map_carrier.c", "../backend/raw_deflate.c", "../backend/mp_json.c",
             "../backend/config_json.c", "../backend/decl_text.c", "../backend/decl_tree.c")

. (Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "tools\cbuild.ps1")
$toolset = Import-MsvcEnv $VcVarsVer
Write-Host "[build] MSVC toolset $toolset"

$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"
# /MT links the C runtime statically so the tool runs on a machine without the VC redistributable.
# Sources compile to build\obj\store (only those changed since the last build), then link.
$objs = Invoke-CbCompile -WorkDir $here -Sources $Sources -Flags "/nologo /O2 /W3 /MT /Brepro /I..\backend /I..\common" `
    -ObjDir (Join-Path $outDir "obj\store") -ObjDirArg "..\..\build\obj\store\" `
    -IncludeDirs @((Join-Path $here "..\backend"), (Join-Path $here "..\common")) -Log (Join-Path $outDir "store-build.log")
$objArgs = ($objs | ForEach-Object { '"..\..\build\obj\store\' + (Split-Path -Leaf $_) + '"' }) -join " "
$cl = "cl /nologo /O2 /W3 /MT /Brepro $objArgs " +
      "/Fe:..\..\build\snapmap-plus-store.exe /link /SUBSYSTEM:CONSOLE shell32.lib ole32.lib advapi32.lib bcrypt.lib /Brepro"
Invoke-CbLink -WorkDir $here -Command $cl -Objects $objs -Output (Join-Path $outDir "snapmap-plus-store.exe") `
    -Log (Join-Path $outDir "store-link.log")
Write-Host "built $(Join-Path $outDir 'snapmap-plus-store.exe')"
