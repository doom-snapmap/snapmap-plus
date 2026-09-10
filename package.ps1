# Package the two built DLLs and their hash manifest into dist/.
# The frontend uses the system WebView2 runtime. Run build.ps1 first.
# Keep this script ASCII for PowerShell 5.1.
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # the repo root
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# Require both DLL outputs before changing dist/.
$backendDll = Join-Path $build "XINPUT1_3.dll"
$uiDll      = Join-Path $build "webview\snapmap-plus-ui.dll"
foreach ($d in @($backendDll, $uiDll)) {
    if (-not (Test-Path $d)) { throw "missing $d -- run build.ps1 first (repo root; builds backend + frontend together)." }
}

# Reject diagnostic backends: sh_diag.log is present only in diagnostic builds.
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($backendDll))
if ($ascii.Contains("sh_diag.log")) {
    throw "build\XINPUT1_3.dll is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with src\backend\build.ps1 (no -Diag)."
}

# Recreate the bundle directory.
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$snapDir = Join-Path $dist "snapmap-plus"
New-Item -ItemType Directory -Force $snapDir | Out-Null

Copy-Item $backendDll (Join-Path $dist "XINPUT1_3.dll")
Copy-Item $uiDll      (Join-Path $snapDir "snapmap-plus-ui.dll")

# Record the installed paths and SHA-256 hashes.
$files = @("XINPUT1_3.dll", "snapmap-plus\snapmap-plus-ui.dll")
$lines = foreach ($f in $files) {
    $h = (Get-FileHash (Join-Path $dist $f) -Algorithm SHA256).Hash
    "{0}  {1}" -f $h, $f
}
[System.IO.File]::WriteAllLines((Join-Path $dist "MANIFEST.sha256"), $lines, (New-Object System.Text.UTF8Encoding $false))

Write-Host "packaged $($files.Count) files into $dist :"
foreach ($f in $files) {
    Write-Host ("  {0,-24} {1,10}" -f $f, (Get-Item (Join-Path $dist $f)).Length)
}
Write-Host "MANIFEST.sha256 written (the install/verify map)."
