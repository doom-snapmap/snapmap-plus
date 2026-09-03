# src/ui/build.ps1 -- build the snapmap-plus-ui.dll that hosts the Snapmap+ UI in a Microsoft Edge
# WebView2 control (HTML/CSS/JS). Same exports + backend contract as the interface expects. Pure ASCII
# (PS 5.1 reads BOM-less UTF-8 as 1252). Invoked by the repo-root build.ps1 (backend + frontend, lockstep).
#
# What it does:
#   1. locate MSVC via vswhere -> vcvars64.
#   2. fetch the Microsoft.Web.WebView2 SDK (headers + static loader lib) from NuGet into
#      build\webview2sdk (gitignored) if not already there. NO binaries land in the repo.
#   2b. best-effort refresh of the embedded menubar logo from the GitHub org avatar (offline-safe:
#      any fetch failure keeps the committed copy; a real change rewrites mockup.html -> commit it).
#   3. generate build\obj\uiwv\mockup_html.h from webview\mockup.html (the UI, embedded in the DLL).
#   4. cl-compile webview\snapmap_plus_ui_webview.cpp + sl_exports.cpp -> build\snapmap-plus-ui.dll,
#      statically linking WebView2LoaderStatic.lib (no WebView2Loader.dll to ship).
#
# Usage:  invoked by the repo-root build.ps1; or directly: pwsh -File src\ui\build.ps1 (backend not rebuilt)
#
# Needs: Build Tools for Visual Studio 2022 (C++ workload). Uses the system-installed WebView2 runtime
# at RUN time (preinstalled on Windows 11; evergreen runtime on most Windows 10).
param(
    [string]$Out = "snapmap-plus-ui.dll",
    # -VcVarsVer: PIN the MSVC toolset (e.g. "14.44.35207") instead of the VS install default, so a
    # shipped DLL can be rebuilt byte-for-byte. Defaults from SNAPMAPPLUS_VCVARS_VER -- the repo-root
    # build.ps1 forwards no arguments here, so the env var is what keeps this half of the lockstep
    # build on the same compiler as the backend half. See src\backend\build.ps1 for the full note.
    [string]$VcVarsVer = $env:SNAPMAPPLUS_VCVARS_VER
)
$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path            # src\ui
$repo   = Split-Path -Parent (Split-Path -Parent $here)             # repo root
$common = Join-Path (Split-Path -Parent $here) "common"            # src\common
$build  = Join-Path $repo "build"
$objDir = Join-Path $build "obj\uiwv"
$sdkDir = Join-Path $build "webview2sdk"
New-Item -ItemType Directory -Force $objDir | Out-Null

# --- 1. MSVC toolchain --------------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found. Install VS 2022 Build Tools (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Resolve (and, when pinned, VERIFY) the MSVC toolset up front -- same check the backend build does, so
# a missing pin fails by name here too rather than as a bare non-zero exit out of vcvars64.bat.
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
    # Not pinned: report the newest toolset present (same rationale as the backend build).
    $newest = Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($newest) { $toolset = $newest.Name + " (newest installed, NOT pinned)" }
    else { $toolset = "unknown (NOT pinned)" }
}
Write-Host "[build] MSVC toolset $toolset"

# --- 2. WebView2 SDK (NuGet) --------------------------------------------------------------------------
# Pinned (not "latest") -- api.nuget.org's index.json lists prerelease builds interleaved with stable
# ones, so $idx.versions[-1] (the literal last entry) can silently land a "-prerelease" SDK. Bump this
# deliberately when picking up a new stable release.
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

# --- 2b. sync the menubar logo from the org avatar (best-effort, offline-safe) ------------------------
# The menubar logo is the GitHub org avatar (github.com/doom-snapmap), carried inside mockup.html as a base64
# data URI (the page is loaded from a string in-game, so no file or URL would resolve at runtime). Each
# build refreshes that copy: fetch the 64px avatar, and if it downloads, is a real JPEG/PNG, and differs
# from what's embedded, rewrite the data URI in mockup.html -- the change then shows up in git like any
# source edit. ANY failure (offline, timeout, junk response, unexpected HTML shape) just keeps the
# committed copy and never fails the build.
$htmlPath = Join-Path $here "webview\mockup.html"
if (-not (Test-Path $htmlPath)) { throw "mockup.html not found at $htmlPath" }
$logoTmp = Join-Path $objDir "org_avatar.tmp"
try {
    & curl.exe -sL --max-time 8 -o $logoTmp "https://github.com/doom-snapmap.png?size=64" 2>$null
    $ok = (Test-Path $logoTmp) -and ((Get-Item $logoTmp).Length -gt 0) -and ((Get-Item $logoTmp).Length -lt 65536)
    if ($ok) {
        $logoBytes = [IO.File]::ReadAllBytes($logoTmp)
        $mime = $null
        if ($logoBytes.Length -gt 3 -and $logoBytes[0] -eq 0xFF -and $logoBytes[1] -eq 0xD8) { $mime = "image/jpeg" }
        elseif ($logoBytes.Length -gt 7 -and $logoBytes[0] -eq 0x89 -and $logoBytes[1] -eq 0x50) { $mime = "image/png" }
        if ($mime) {
            $logoUri = "data:$mime;base64," + [Convert]::ToBase64String($logoBytes)
            $htmlRaw = Get-Content -Raw -Path $htmlPath
            $m = [regex]::Matches($htmlRaw, 'data:image/(?:jpeg|png);base64,[A-Za-z0-9+/=]+')
            if ($m.Count -eq 1) {
                if ($m[0].Value -ne $logoUri) {
                    $htmlRaw = $htmlRaw.Substring(0, $m[0].Index) + $logoUri + $htmlRaw.Substring($m[0].Index + $m[0].Length)
                    [IO.File]::WriteAllText($htmlPath, $htmlRaw, (New-Object System.Text.UTF8Encoding $false))
                    Write-Host "logo: refreshed from the org avatar ($($logoBytes.Length) bytes, $mime) -- mockup.html updated, commit it"
                } else { Write-Host "logo: embedded copy is up to date with the org avatar" }
            } else { Write-Host "logo: skipped -- expected exactly 1 base64 data URI in mockup.html, found $($m.Count)" }
        } else { Write-Host "logo: skipped -- fetched data is not a JPEG/PNG" }
    } else { Write-Host "logo: skipped -- avatar fetch failed, empty, or implausibly large (offline?)" }
} catch { Write-Host "logo: skipped -- $($_.Exception.Message)" }
if (Test-Path $logoTmp) { Remove-Item $logoTmp -Force }

# --- 3. embed the HTML --------------------------------------------------------------------------------
# Wrap mockup.html verbatim in a C++ raw string literal so the DLL carries the UI with no shipped file.
$html = Get-Content -Raw -Path $htmlPath

# Inline the decl editor's generated schema table (webview\schema_slice.js) in place of its <script src>
# tag: the page is loaded via NavigateToString in-game, so a relative src never resolves there -- the
# table must ride inside the single embedded HTML string. Both the tag and the file are REQUIRED: a
# silent miss would ship a schema-less decl editor (structural checks only, no completion).
$slicePath = Join-Path $here "webview\schema_slice.js"
$sliceTag  = '<script src="schema_slice.js"></script>'
if (-not (Test-Path $slicePath)) { throw "schema_slice.js not found at $slicePath -- required (the decl editor would ship schema-less)" }
if ($html.IndexOf($sliceTag) -lt 0) { throw "mockup.html does not contain the literal tag $sliceTag -- cannot inline the schema table" }
$slice = Get-Content -Raw -Path $slicePath
if ($slice.IndexOf(')SNAPMAPPLUS') -ge 0) { throw "schema_slice.js contains the raw-literal delimiter )SNAPMAPPLUS -- cannot embed" }
if ($slice.IndexOf('</script') -ge 0) { throw "schema_slice.js contains '</script' -- would terminate the inline script tag early" }
$html = $html.Replace($sliceTag, "<script>`n$slice</script>")
if ($html.IndexOf($sliceTag) -ge 0) { throw "schema_slice.js inlining left a residual src tag -- duplicate tag in mockup.html?" }

# Inline the Prefab Details transform + WebGL units for the same NavigateToString reason. Separate source
# files keep the pure matrix contract directly testable while the shipped page remains one document.
foreach ($prefabScriptName in @("prefab_transform.js", "prefab_viewport.js")) {
    $prefabScriptPath = Join-Path $here ("webview\" + $prefabScriptName)
    $prefabScriptTag = '<script src="' + $prefabScriptName + '"></script>'
    if (-not (Test-Path $prefabScriptPath)) { throw "$prefabScriptName not found at $prefabScriptPath -- required" }
    if ($html.IndexOf($prefabScriptTag) -lt 0) { throw "mockup.html does not contain $prefabScriptTag -- cannot inline it" }
    $prefabScript = Get-Content -Raw -Path $prefabScriptPath
    if ($prefabScript.IndexOf(')SNAPMAPPLUS') -ge 0) { throw "$prefabScriptName contains the raw-literal delimiter )SNAPMAPPLUS -- cannot embed" }
    if ($prefabScript.IndexOf('</script') -ge 0) { throw "$prefabScriptName contains '</script' -- would terminate the inline script tag early" }
    $html = $html.Replace($prefabScriptTag, "<script>`n$prefabScript</script>")
    if ($html.IndexOf($prefabScriptTag) -ge 0) { throw "$prefabScriptName inlining left a residual src tag" }
}

# MSVC caps a single string literal at ~16 KB (error C2026). Split into <16 KB chunks emitted as
# ADJACENT raw string literals -- the compiler concatenates them into one array. Raw literals need no
# escaping; any byte is safe except the exact ")SNAPMAPPLUS" delimiter, which neither the HTML nor the
# inlined schema table contains (guarded above for the slice).
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

# --- 4. compile -------------------------------------------------------------------------------------
# /MD (dynamic CRT: the WebView2 static loader + the process's existing MSVCP140/VCRUNTIME140 expect it),
# /EHsc /std:c++17. Includes: WebView2 headers, the generated header dir, the shared iface ABI dir.
# Sources: the WebView2 host + the unchanged sl_* export stubs. Links the static WebView2 loader + the
# Win32 libs its COM/shell calls need. /DEF pins the export set (sh_ui_init @10 -- the OG's ordinal -- + the OG-named sl_*).
$incArgs = @(
    "/I`"$wvInclude`"",
    "/I`"$objDir`"",
    "/I`"$common`""
) -join " "
$srcArgs = "webview\snapmap_plus_ui_webview.cpp webview\config_message.cpp webview\theme_bootstrap.cpp sl_exports.cpp ..\common\log_rotate.c"
$libArgs = @(
    "`"$wvLib`"",
    "ole32.lib", "oleaut32.lib", "shell32.lib", "shlwapi.lib",
    "version.lib", "advapi32.lib", "user32.lib", "gdi32.lib",
    "dwmapi.lib",  # one-pixel frame extension preserves the captionless window's shadow/rounded corners
    "winhttp.lib"   # the feedback dialog's single user-initiated POST (see the capability note in snapmap_plus_ui_webview.cpp)
) -join " "
$implib = $Out -replace '\.dll$', '.lib'

# Output -> build\webview\ (the frontend's own subfolder; the backend, XINPUT1_3.dll, stays
# directly in build\.)
New-Item -ItemType Directory -Force (Join-Path $build "webview") | Out-Null
# SYMBOLS (build\webview\snapmap-plus-ui.pdb + .map) -- same flag set, and the same reasoning, as the
# backend build (see the long note in src\backend\build.ps1): /Z7 + /DEBUG:FULL emit the symbols a crash
# offset needs to become a function name, while /INCREMENTAL:NO /OPT:REF /OPT:ICF put back the release
# layout that /DEBUG:FULL would otherwise change, and /Brepro makes the output deterministic. Maintainer
# artifacts only: package.ps1 copies just the DLL, so the shipped overlay is unchanged.
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
# Assert the symbols landed -- a silent miss would only surface the next time a crash stack needed them.
foreach ($sym in @(($Out -replace '\.dll$', '.pdb'), ($Out -replace '\.dll$', '.map'))) {
    $symPath = Join-Path $build "webview\$sym"
    if (-not (Test-Path $symPath)) { throw "expected symbol artifact missing: $symPath" }
    Write-Host ("  symbols: {0,-26} {1,10} bytes" -f $sym, (Get-Item $symPath).Length)
}
