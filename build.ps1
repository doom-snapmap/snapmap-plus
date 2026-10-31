# Build the backend and frontend together from their shared ABI.
# -BackendOnly skips the frontend; other arguments forward to the backend build.
# Use -Diag only for local diagnostics. See docs/contributing.md for setup.
# Keep this script ASCII for PowerShell 5.1.
param(
    [switch]$BackendOnly,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\backend\build.ps1" @Rest
if ($LASTEXITCODE -ne 0) { throw "backend build failed" }
# The store migration tool the installer runs, from the backend's store sources.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\store\build.ps1"
if ($LASTEXITCODE -ne 0) { throw "store tool build failed" }
if ($BackendOnly) {
    Write-Host "built: build/XINPUT1_4.dll + build/snapmap-plus-store.exe (backend only)"
} else {
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\ui\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw "frontend build failed" }
    Write-Host "built: build/XINPUT1_4.dll + build/webview/snapmap-plus-ui.dll + build/snapmap-plus-store.exe"
    Write-Host "package with: package.ps1"
}
