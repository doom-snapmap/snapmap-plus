# package.ps1 -- assemble the deployable SnapHak overlay into dist\ (the tree you drop into a DOOM install).
# Pure ASCII (PS 5.1 reads BOM-less UTF-8 as 1252).
#
# This is the PRODUCT's packaging step: shipping the snaphak\ + plugins\ Qt runtime alongside the two clone
# DLLs is the product's job. It consumes the DLLs built by build.ps1 (in build\) and copies the Qt 5.9.9
# runtime from the Qt SDK. Output is the 6-file overlay documented in docs\packaging.md, plus a
# MANIFEST.sha256 (the install/verify map + hash transparency for releases).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1 -QtDir D:\Qt\5.9.9\msvc2017_64
param(
    [string]$QtDir = "C:\Qt\5.9.9\msvc2017_64"
)
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # open-snaphak\
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# --- consume build\ : both clone DLLs must be present (built by build.ps1) ---
$backendDll = Join-Path $build "XINPUT1_3.dll"
$uiDll      = Join-Path $build "snaphakui.dll"
foreach ($d in @($backendDll, $uiDll)) {
    if (-not (Test-Path $d)) { throw "missing $(Split-Path -Leaf $d) in build\ -- run build.ps1 first." }
}

# --- refuse a -Diag (troubleshooting) backend: it is self-labelled DO NOT DISTRIBUTE. The diagnostic
#     logger (shield_diag.c) is the only source of the string "snaphak_crash.dmp"; a release build has none. ---
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($backendDll))
if ($ascii.Contains("snaphak_crash.dmp")) {
    throw "build\XINPUT1_3.dll is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with build.ps1 (no -Diag)."
}

# --- Qt 5.9.9 runtime from the SDK (never committed; copied at package time) ---
if (-not (Test-Path $QtDir)) { throw "Qt 5.9.9 not found at $QtDir (set -QtDir)." }
$qtBin  = Join-Path $QtDir "bin"
$qtPlat = Join-Path $QtDir "plugins\platforms\qwindows.dll"
$qtDlls = @("Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll")
foreach ($q in $qtDlls) {
    if (-not (Test-Path (Join-Path $qtBin $q))) { throw "missing $q under $qtBin -- Qt 5.9.9 install incomplete." }
}
if (-not (Test-Path $qtPlat)) { throw "missing plugins\platforms\qwindows.dll under $QtDir -- the required Qt platform plugin." }

# --- assemble dist\ fresh ---
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$snapDir = Join-Path $dist "snaphak"
$platDir = Join-Path $dist "plugins\platforms"
New-Item -ItemType Directory -Force $snapDir | Out-Null
New-Item -ItemType Directory -Force $platDir | Out-Null

Copy-Item $backendDll (Join-Path $dist "XINPUT1_3.dll")
Copy-Item $uiDll      (Join-Path $snapDir "snaphakui.dll")
foreach ($q in $qtDlls) { Copy-Item (Join-Path $qtBin $q) (Join-Path $snapDir $q) }
Copy-Item $qtPlat (Join-Path $platDir "qwindows.dll")

# --- MANIFEST.sha256 : the 6 deployable files (the installer's file list + per-file hash verify) ---
$files = @(
    "XINPUT1_3.dll",
    "snaphak\snaphakui.dll", "snaphak\Qt5Core.dll", "snaphak\Qt5Gui.dll", "snaphak\Qt5Widgets.dll",
    "plugins\platforms\qwindows.dll"
)
$lines = foreach ($f in $files) {
    $h = (Get-FileHash (Join-Path $dist $f) -Algorithm SHA256).Hash
    "{0}  {1}" -f $h, $f
}
[System.IO.File]::WriteAllLines((Join-Path $dist "MANIFEST.sha256"), $lines, (New-Object System.Text.UTF8Encoding $false))

Write-Host "packaged $($files.Count) files into $dist :"
foreach ($f in $files) {
    Write-Host ("  {0,-34} {1,10}" -f $f, (Get-Item (Join-Path $dist $f)).Length)
}
Write-Host "MANIFEST.sha256 written (the install/verify map)."
