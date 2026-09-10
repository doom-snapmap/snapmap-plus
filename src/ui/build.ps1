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

# Refresh the embedded organization avatar when a valid download differs.
# This can modify tracked mockup.html. Download failures keep the committed image.
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

# Embed the page in the DLL.
$html = Get-Content -Raw -Path $htmlPath

# NavigateToString cannot resolve relative scripts. Require and inline the schema
# table so a missing file or tag cannot silently disable declaration completion.
$slicePath = Join-Path $here "webview\schema_slice.js"
$sliceTag  = '<script src="schema_slice.js"></script>'
if (-not (Test-Path $slicePath)) { throw "schema_slice.js not found at $slicePath -- required (the decl editor would ship schema-less)" }
if ($html.IndexOf($sliceTag) -lt 0) { throw "mockup.html does not contain the literal tag $sliceTag -- cannot inline the schema table" }
$slice = Get-Content -Raw -Path $slicePath
if ($slice.IndexOf(')SNAPMAPPLUS') -ge 0) { throw "schema_slice.js contains the raw-literal delimiter )SNAPMAPPLUS -- cannot embed" }
if ($slice.IndexOf('</script') -ge 0) { throw "schema_slice.js contains '</script' -- would terminate the inline script tag early" }
$html = $html.Replace($sliceTag, "<script>`n$slice</script>")
if ($html.IndexOf($sliceTag) -ge 0) { throw "schema_slice.js inlining left a residual src tag -- duplicate tag in mockup.html?" }

# Inline the separately testable prefab transform and viewport scripts too.
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
$srcArgs = "webview\snapmap_plus_ui_webview.cpp webview\config_message.cpp webview\theme_bootstrap.cpp sl_exports.cpp ..\common\log_rotate.c ..\backend\host_image.c"
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
