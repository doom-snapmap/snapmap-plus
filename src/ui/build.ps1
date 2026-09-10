# Build the WebView2 frontend into build\webview. The root build invokes this
# alongside the backend; a direct invocation rebuilds only the frontend.
# Requires MSVC C++ tools. Fetches a pinned WebView2 SDK, embeds the page and
# scripts, and links the static loader against the system WebView2 runtime.
# Keep this file ASCII for PowerShell 5.1.
param(
    [string]$Out = "snapmap-plus-ui.dll",
# SNAPMAPPLUS_VCVARS_VER keeps both DLL builds on the same pinned MSVC toolset.
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path            # src\ui
$repo   = Split-Path -Parent (Split-Path -Parent $here)             # repo root
$common = Join-Path (Split-Path -Parent $here) "common"            # src\common
$backend = Join-Path (Split-Path -Parent $here) "backend"          # src\backend (host_image only)
$build  = Join-Path $repo "build"
$objDir = Join-Path $build "obj\uiwv"
$sdkDir = Join-Path $build "webview2sdk"
New-Item -ItemType Directory -Force $objDir | Out-Null

# MSVC toolchain.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install VS 2022 Build Tools (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Report a missing toolset pin before invoking vcvars64.bat.
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
# Without a pin, report the newest installed toolset.
    $newest = Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($newest) { $toolset = $newest.Name + " (newest installed, NOT pinned)" }
    else { $toolset = "unknown (NOT pinned)" }
}
Write-Host "[build] MSVC toolset $toolset"

# WebView2 SDK. Update the stable version deliberately; NuGet's last entry may be a prerelease.
$wvPinnedVersion = "1.0.4078.44"
$wvInclude = Join-Path $sdkDir "build\native\include"
$wvLib     = Join-Path $sdkDir "build\native\x64\WebView2LoaderStatic.lib"
if (-not (Test-Path (Join-Path $wvInclude "WebView2.h"))) {
    Write-Host "Fetching Microsoft.Web.WebView2 SDK $wvPinnedVersion from NuGet..."
    $ver = $wvPinnedVersion
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg"
    $zip = Join-Path $build "webview2.$ver.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip
    if (Test-Path $sdkDir) { Remove-Item -Recurse -Force $sdkDir }
    Expand-Archive -Path $zip -DestinationPath $sdkDir -Force
    Remove-Item $zip -Force
}
if (-not (Test-Path (Join-Path $wvInclude "WebView2.h"))) { throw "WebView2.h missing after SDK fetch ($wvInclude)" }
if (-not (Test-Path $wvLib)) { throw "WebView2LoaderStatic.lib missing after SDK fetch ($wvLib)" }
Write-Host "WebView2 SDK ready at $sdkDir"

# Bundle committed assets without changing source files.
$html = & (Join-Path $here "embed-page.ps1")

# Split adjacent raw literals below MSVC's size limit. Reject the raw delimiter
# in embedded content so it cannot terminate a literal early.
if ($html.IndexOf(')SNAPMAPPLUS') -ge 0) { throw "embedded HTML contains the raw-literal delimiter )SNAPMAPPLUS -- cannot chunk" }
$chunkSize = 8000
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* generated from webview/mockup.html by src/ui/build.ps1 -- do not edit */")
[void]$sb.AppendLine("static const char kMockupHtml[] =")
for ($i = 0; $i -lt $html.Length; $i += $chunkSize) {
    $len = [Math]::Min($chunkSize, $html.Length - $i)
    [void]$sb.AppendLine('R"SNAPMAPPLUS(' + $html.Substring($i, $len) + ')SNAPMAPPLUS"')
}
[void]$sb.AppendLine(";")
$hdrPath = Join-Path $objDir "mockup_html.h"
Set-Content -Path $hdrPath -Value $sb.ToString() -Encoding ascii -NoNewline
Write-Host "generated $hdrPath ($([Math]::Round(($html.Length/1KB),1)) KB of HTML)"

# Compile with the dynamic CRT required by the WebView2 static loader.
# The .def file pins sh_ui_init at ordinal 10 and the sl_* compatibility exports.
$incArgs = @(
    "/I`"$wvInclude`"",
    "/I`"$objDir`"",
    "/I`"$common`"",
    # Share host_image.c so both DLLs identify the renderer consistently.
    "/I`"$backend`""
) -join " "
$srcArgs = "webview\snapmap_plus_ui_webview.cpp webview\config_message.cpp webview\webview_json.cpp webview\theme_bootstrap.cpp sl_exports.cpp ..\common\log_rotate.c ..\backend\host_image.c"
$libArgs = @(
    "`"$wvLib`"",
    "ole32.lib", "oleaut32.lib", "shell32.lib", "shlwapi.lib",
    "version.lib", "advapi32.lib", "user32.lib", "gdi32.lib",
    "dwmapi.lib",  # one-pixel frame extension preserves the captionless window's shadow/rounded corners
    "winhttp.lib"   # the feedback dialog's single user-initiated POST (see the capability note in snapmap_plus_ui_webview.cpp)
) -join " "
$implib = $Out -replace '\.dll$', '.lib'

# Frontend output directory.
New-Item -ItemType Directory -Force (Join-Path $build "webview") | Out-Null
# Emit crash symbols while keeping release optimization and reproducible linking.
# PDB and map files are maintainer artifacts; packaging copies only the DLL.
$cl  = "cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MD /Z7 /Brepro /DWIN32 /D_WINDOWS /Fo..\..\build\obj\uiwv\ " +
       "$incArgs $srcArgs /Fe:..\..\build\webview\$Out " +
       "/link /DEF:snapmap-plus-ui.def /IMPLIB:..\..\build\obj\uiwv\$implib $libArgs " +
       "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /Brepro /PDBALTPATH:%_PDB% /MAP"
$cmd = "cd /d `"$here`" && `"$vcvars`"$vcvarsArgs && $cl"

$buildLog = Join-Path $build "build-ui.log"
cmd /c "$cmd > `"$buildLog`" 2>&1"
$clExit = $LASTEXITCODE
Get-Content $buildLog | Write-Host
if ($clExit -ne 0) { throw "cl failed (exit $clExit) -- see $buildLog" }
Write-Host "built $(Join-Path $build "webview\$Out") (WebView2)"
# Require both symbol artifacts for later crash diagnosis.
foreach ($sym in @(($Out -replace '\.dll$', '.pdb'), ($Out -replace '\.dll$', '.map'))) {
    $symPath = Join-Path $build "webview\$sym"
    if (-not (Test-Path $symPath)) { throw "expected symbol artifact missing: $symPath" }
    Write-Host ("  symbols: {0,-26} {1,10} bytes" -f $sym, (Get-Item $symPath).Length)
}
