# Build the backend and frontend together from their shared ABI; -BackendOnly skips the frontend.
# -Version stamps a release tag as SNAPMAP_PLUS_VERSION (tools/cbuild.ps1), "dev" for a local build; -Diag is local only.
# Keep this script ASCII for PowerShell 5.1.
param(
    [switch]$BackendOnly,
    [string]$Version = "dev",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Version -notmatch '^(dev|v\d+\.\d+\.\d+(-beta\.\d+)?)$') {
    throw "-Version '$Version' is not a release tag (v1.2.3 or v1.2.3-beta.4) or dev"
}
$savedVersion = $env:SNAPMAP_PLUS_VERSION
$env:SNAPMAP_PLUS_VERSION = $Version
try {
    # Import the MSVC environment once; the builds below inherit it.
    . "$here\tools\cbuild.ps1"
    $null = Import-MsvcEnv $env:SNAPMAPPLUS_VCVARS_VER
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\backend\build.ps1" @Rest
    if ($LASTEXITCODE -ne 0) { throw "backend build failed" }
    if ($BackendOnly) {
        Write-Host "built: build/XINPUT1_4.dll (backend only, $Version)"
    } else {
        # -Diag reaches the frontend too (its remote debugging port); the other arguments are the backend's.
        $uiArgs = @()
        if ($Rest -contains "-Diag") { $uiArgs += "-Diag" }
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$here\src\ui\build.ps1" @uiArgs
        if ($LASTEXITCODE -ne 0) { throw "frontend build failed" }
        Write-Host "built: build/XINPUT1_4.dll + build/webview/snapmap-plus-ui.dll ($Version)"
        Write-Host "package with: package.ps1"
    }
} finally {
    $env:SNAPMAP_PLUS_VERSION = $savedVersion
}
