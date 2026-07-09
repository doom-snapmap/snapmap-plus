# build.ps1 -- compile the clean-room FRONTEND DLL (our snaphakui.dll, the Qt "SnapHak Studio" UI) with
# MSVC (x64) + Qt 5.9. Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252). Clones the backend build.ps1
# toolchain pattern (vswhere -> vcvars64 -> cl) but is C++ + Qt-linking, so it diverges from the backend
# in three deliberate ways:
#   1. C++ not C: /EHsc /std:c++17 (Qt 5.9 is C++ and supports c++17).
#   2. /MD not /MT: Qt 5.9 official MSVC builds link the DYNAMIC CRT; a Qt-linking DLL MUST match Qt's CRT
#      (the shipped Qt5*.dll import MSVCP140/VCRUNTIME140). The backend is /MT -- the two DLLs are separate
#      modules so the CRT difference is fine; the interface object crossing the line is POD (raw ptrs +
#      offsets), never a CRT-allocated C++ object passed across the DLL boundary.
#   3. Qt include + lib dirs + the snaphak_iface common dir.
#
# Usage:
#   pwsh -File build.ps1                 # -> snaphakui.dll
#   pwsh -File build.ps1 -Out x.dll      # alternate output name
#
# Sources: snaphak_ui_init.cpp (the 0x129d0 3-object bring-up + the 0x15c04 30 Hz think-loop) +
# sh_setupui.cpp (the faithful FUN_18000cb6c widget-tree port + retranslateUi + the FUN_180014e7c
# flag-word dispatch skeleton) + sl_exports.cpp (the 9 sl_* thin stubs). The shared ABI header (../common/snaphak_iface.h) is
# included (no .cpp from common is linked into the frontend -- the frontend only CONSUMES the interface;
# the backend hosts the factory + the register/unregister/drain bodies).
#
# Needs Build Tools for Visual Studio 2022 (C++ workload) + Qt 5.9.9 MSVC2017 64-bit at $QtDir.
param(
    [string]$Out   = "snaphakui.dll",
    [string]$QtDir = "C:\Qt\5.9.9\msvc2017_64"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$common = Join-Path (Split-Path -Parent $here) "common"

if (-not (Test-Path $QtDir)) { throw "Qt SDK not found at $QtDir (set -QtDir)." }
$qtInc = Join-Path $QtDir "include"
$qtLib = Join-Path $QtDir "lib"
foreach ($m in @("Qt5Core.lib", "Qt5Gui.lib", "Qt5Widgets.lib")) {
    if (-not (Test-Path (Join-Path $qtLib $m))) { throw "missing $m under $qtLib" }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$Sources = @("snaphak_ui_init.cpp", "sh_setupui.cpp", "sl_exports.cpp", "snapstack.cpp", "sh_tabs.cpp",
             "sh_timeline.cpp")
$srcArgs = ($Sources | ForEach-Object { '"' + $_.Trim() + '"' }) -join " "
$implib  = $Out -replace '\.dll$', '.lib'   # import lib + .exp -> build\obj\ui (build\ root stays shippable DLLs only)

# Qt include dirs: the top-level include + the per-module dirs (Qt headers #include <QApplication> resolve
# under QtWidgets/). The common dir carries the shared interface ABI header.
$incArgs = @(
    "/I`"$qtInc`"",
    "/I`"$qtInc\QtCore`"",
    "/I`"$qtInc\QtGui`"",
    "/I`"$qtInc\QtWidgets`"",
    "/I`"$common`""
) -join " "

# Qt link libs (Core/Gui/Widgets). QtSvg is NOT linked (ships only as a runtime image-format plugin).
# user32.lib: MessageBoxA -- the `bsb` faithful OG leftover calls it on the re-resolve
# mismatch path (Qt/CRT do not pull user32 in for a console-driven op).
$libArgs = @(
    "`"$qtLib\Qt5Core.lib`"",
    "`"$qtLib\Qt5Gui.lib`"",
    "`"$qtLib\Qt5Widgets.lib`"",
    "user32.lib"
) -join " "

# C++ Qt build: /LD shared, /EHsc C++ EH, /std:c++17, /MD (match Qt's dynamic CRT), Qt + common
# includes, the Qt libs, /Fe:snaphakui.dll, /DEF for the OG export ordinal + the sl_* name set.
# Output -> open-snaphak\build\qt\ (its OWN subfolder, distinct from build\webview\ -- both the Qt and
# webview frontends build a file literally named snaphakui.dll; without separate folders, building one
# after the other would silently overwrite the other in build\. The backend, XINPUT1_3.dll, has no
# per-frontend variant and stays directly in build\.). Paths RELATIVE to cwd=$here (..\..\ = repo root)
# so the quoted-trailing-backslash cmd footgun is avoided; /DEF:snaphakui.def stays cwd-relative.
$cl  = "cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MD /DWIN32 /D_WINDOWS /Fo..\..\build\obj\ui\ $incArgs $srcArgs " +
       "/Fe:..\..\build\qt\$Out /link /DEF:snaphakui.def /IMPLIB:..\..\build\obj\ui\$implib $libArgs"
$cmd = "cd /d `"$here`" && `"$vcvars`" && $cl"
# vcvars64.bat emits a spurious "'vswhere.exe' is not recognized" line on stderr (it probes a bare-PATH
# vswhere before falling back); under $ErrorActionPreference='Stop' a native-command stderr line can trip
# PS 5.1 as a terminating error even though cl succeeds. Route the whole cmd's stdout+stderr to a log and
# gate ONLY on the real signal -- $LASTEXITCODE from `cmd /c`.
$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build"   # open-snaphak\build (out of src\)
New-Item -ItemType Directory -Force (Join-Path $outDir "obj\ui") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $outDir "qt") | Out-Null
$buildLog = Join-Path $outDir "build.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
Write-Host "built $(Join-Path $outDir "qt\$Out")"
