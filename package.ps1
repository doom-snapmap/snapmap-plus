# Package the built DLLs and their hash manifest into dist/, with the store migration tool beside them.
# The frontend uses the system WebView2 runtime. Run build.ps1 first.
# The bundle carries the April 2024 build's pair only (see docs/packaging.md); the 2018 build is
# served by v0.2.1-beta.19's own release.
# Keep this script ASCII for PowerShell 5.1.
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # the repo root
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# The supported build's frontend path names the build, so it never collides with the 2018
# release's frontend. Keep in step with SH_UI_DLL_PATH in src/backend/ui_bridge.h and
# currentUIDll in installer/doombuild.go (a Go test checks them).
$uiRel = "snapmap-plus\snapmap-plus-ui-13954591.dll"

# Require both DLL outputs and the store tool before changing dist/.
$backendDll = Join-Path $build "XINPUT1_4.dll"
$uiDll      = Join-Path $build "webview\snapmap-plus-ui.dll"
$storeTool  = Join-Path $build "snapmap-plus-store.exe"
foreach ($d in @($backendDll, $uiDll, $storeTool)) {
    if (-not (Test-Path $d)) { throw "missing $d -- run build.ps1 first (repo root; builds backend + frontend together)." }
}

# Reject diagnostic backends: sh_diag.log is present only in diagnostic builds.
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($backendDll))
if ($ascii.Contains("sh_diag.log")) {
    throw "build\XINPUT1_4.dll is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with src\backend\build.ps1 (no -Diag)."
}

# Recreate the bundle directory.
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$snapDir = Join-Path $dist "snapmap-plus"
New-Item -ItemType Directory -Force $snapDir | Out-Null

Copy-Item $backendDll (Join-Path $dist "XINPUT1_4.dll")
Copy-Item $uiDll      (Join-Path $dist $uiRel)
$files = @("XINPUT1_4.dll", $uiRel)

# Record the installed paths and SHA-256 hashes.
$lines = foreach ($f in $files) {
    $h = (Get-FileHash (Join-Path $dist $f) -Algorithm SHA256).Hash
    "{0}  {1}" -f $h, $f
}
[System.IO.File]::WriteAllLines((Join-Path $dist "MANIFEST.sha256"), $lines, (New-Object System.Text.UTF8Encoding $false))

# The store migration tool ships beside the overlay, not in it: MANIFEST.sha256 lists what goes
# into DOOM (every earlier installer installs each listed file), so the tool has its own list,
# TOOLS.sha256, which the installer checks before placing it in %LOCALAPPDATA%\snapmap-plus.
Copy-Item $storeTool (Join-Path $dist "snapmap-plus-store.exe")
$toolHash = (Get-FileHash (Join-Path $dist "snapmap-plus-store.exe") -Algorithm SHA256).Hash
[System.IO.File]::WriteAllLines((Join-Path $dist "TOOLS.sha256"), @("{0}  {1}" -f $toolHash, "snapmap-plus-store.exe"),
    (New-Object System.Text.UTF8Encoding $false))

Write-Host "packaged $($files.Count) files into $dist :"
foreach ($f in $files) {
    Write-Host ("  {0,-44} {1,10}" -f $f, (Get-Item (Join-Path $dist $f)).Length)
}
Write-Host "MANIFEST.sha256 written (the install/verify map)."
Write-Host ("  {0,-44} {1,10}" -f "snapmap-plus-store.exe", (Get-Item (Join-Path $dist "snapmap-plus-store.exe")).Length)
Write-Host "TOOLS.sha256 written (the store tool, placed in %LOCALAPPDATA%\snapmap-plus, not in DOOM)."
