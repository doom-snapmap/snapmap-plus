# Compile the same conversion core linked into the backend for the Go adapter.
# Only the installer embeds this private library; the overlay stays two DLLs.
param([string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ Build Tools are required for the shared migration core." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$vcargs = if ($VcVarsVer) { " -vcvars_ver=$VcVarsVer" } else { "" }
$obj = Join-Path $root "build\obj\migration"
$native = Join-Path $here "native"
New-Item -ItemType Directory -Force $obj, $native | Out-Null
$sources = @("package_migration.c", "config_json.c", "package_descriptor.c", "decl_server_path.c", "package_sources.c", "packages.c")
$args = ($sources | ForEach-Object { '"' + (Join-Path $root "src\backend\$_") + '"' }) -join ' '
$output = Join-Path $native "migration.dll"
$log = Join-Path $obj "build.log"
$objectArgument = $obj.Replace('\', '/') + '/'
$command = "`"$vcvars`"$vcargs >nul && cl /nologo /LD /O2 /W3 /WX /MT /Brepro $args /Fo`"$objectArgument`" /Fe:`"$output`" /link /DEF:`"$here\migration.def`" /IMPLIB:`"$obj\migration.lib`" /Brepro"
cmd /c "$command > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $log; throw "Shared migration core build failed" }
Write-Host "built shared migration core"
