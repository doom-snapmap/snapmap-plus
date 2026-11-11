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

# MSVC toolchain, imported once and shared with the backend build (tools\cbuild.ps1).
. (Join-Path $repo "tools\cbuild.ps1")
$toolset = Import-MsvcEnv $VcVarsVer
Write-Host "[build] MSVC toolset $toolset"

# WebView2 SDK. Update the stable version deliberately; NuGet's last entry may be a prerelease.
$wvPinnedVersion = "1.0.4078.44"
# SHA-256 of that .nupkg; its SHA-512 matches the packageHash NuGet's catalog publishes for the version.
# A changed version needs its new hash here.
$wvPinnedSha256 = "dc4d1d9168df26b830398303e50210b6e1729f6ce5a7ac69d2c766852f489962"
$wvInclude = Join-Path $sdkDir "build\native\include"
$wvLib     = Join-Path $sdkDir "build\native\x64\WebView2LoaderStatic.lib"
if (-not (Test-Path (Join-Path $wvInclude "WebView2.h"))) {
    Write-Host "Fetching Microsoft.Web.WebView2 SDK $wvPinnedVersion from NuGet..."
    $ver = $wvPinnedVersion
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg"
    $zip = Join-Path $build "webview2.$ver.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip
    $got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($got -ne $wvPinnedSha256) {
        Remove-Item $zip -Force
        throw "WebView2 SDK $ver from NuGet has SHA-256 $got, not the pinned $wvPinnedSha256; refusing it"
    }
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
# Rewrite the header only when the page changed, so an unchanged page does not recompile the frontend.
$hdrOld = if (Test-Path $hdrPath) { [System.IO.File]::ReadAllText($hdrPath) } else { $null }
if ($hdrOld -ne $sb.ToString()) { Set-Content -Path $hdrPath -Value $sb.ToString() -Encoding ascii -NoNewline }
Write-Host "generated $hdrPath ($([Math]::Round(($html.Length/1KB),1)) KB of HTML)"

# Compile with the dynamic CRT required by the WebView2 static loader.
# The .def file pins sh_ui_init at ordinal 10, the window's one export.
$incArgs = @(
    "/I`"$wvInclude`"",
    "/I`"$objDir`"",
    "/I`"$common`"",
    # Share host_image.c so both DLLs identify the renderer consistently, and
    # prefab_grab.c so Create can check a saved frame when the backend has no guard.
    "/I`"$backend`""
) -join " "
$Sources = @("webview\snapmap_plus_ui_webview.cpp", "webview\config_message.cpp", "webview\webview_json.cpp",
             "webview\theme_bootstrap.cpp", "..\common\log_rotate.c", "..\backend\host_image.c",
             "..\backend\prefab_grab.c", "..\backend\prefab_block.c")
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
# Sources compile to build\obj\uiwv (only those changed since the last build), then link.
$buildLog = Join-Path $build "build-ui.log"
$objs = Invoke-CbCompile -WorkDir $here -Sources $Sources -Flags "/nologo /LD /O2 /W3 /EHsc /std:c++17 /MD /Z7 /Brepro /DWIN32 /D_WINDOWS $incArgs" `
    -ObjDir $objDir -ObjDirArg "..\..\build\obj\uiwv\" -IncludeDirs @($objDir, $common, $backend) -Log $buildLog
$objArgs = ($objs | ForEach-Object { '"..\..\build\obj\uiwv\' + (Split-Path -Leaf $_) + '"' }) -join " "
$cl  = "cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MD /Z7 /Brepro $objArgs /Fe:..\..\build\webview\$Out " +
       "/link /DEF:snapmap-plus-ui.def /IMPLIB:..\..\build\obj\uiwv\$implib $libArgs " +
       "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /Brepro /PDBALTPATH:%_PDB% /MAP"
Invoke-CbLink -WorkDir $here -Command $cl -Objects $objs -Output (Join-Path $build "webview\$Out") `
    -Log (Join-Path $build "link-ui.log") -Remove @((Join-Path $build ("webview\" + ($Out -replace '\.dll$', '.pdb'))))
Write-Host "built $(Join-Path $build "webview\$Out") (WebView2)"
# Require both symbol artifacts for later crash diagnosis.
foreach ($sym in @(($Out -replace '\.dll$', '.pdb'), ($Out -replace '\.dll$', '.map'))) {
    $symPath = Join-Path $build "webview\$sym"
    if (-not (Test-Path $symPath)) { throw "expected symbol artifact missing: $symPath" }
    Write-Host ("  symbols: {0,-26} {1,10} bytes" -f $sym, (Get-Item $symPath).Length)
}
