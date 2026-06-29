# install.ps1 -- one-liner bootstrap for the SnapHak installer.
#
# Once the origin repo is set, end users run:
#   irm https://github.com/<owner>/open-snaphak/releases/latest/download/install.ps1 | iex
# It downloads snaphak.exe from the latest release into %LOCALAPPDATA%\open-snaphak and runs `snaphak install`.
# After that, run snaphak.exe directly for update / uninstall / status.
$ErrorActionPreference = "Stop"

# TODO: set this when the origin repo is chosen (deferred sub-decision in the distribution spec).
$repo = "OWNER/open-snaphak"
if ($repo.StartsWith("OWNER/")) {
    Write-Error "install.ps1 is not configured yet (repo placeholder). Build snaphak.exe from source (installer/) and run 'snaphak install --local <dist>' for now."
    return
}

$dest = Join-Path $env:LOCALAPPDATA "open-snaphak"
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest "snaphak.exe"

Write-Host "Downloading snaphak.exe ..."
Invoke-WebRequest -Uri "https://github.com/$repo/releases/latest/download/snaphak.exe" -OutFile $exe

Write-Host "Installing SnapHak ..."
& $exe install

Write-Host ""
Write-Host "snaphak.exe is at $exe"
Write-Host "Run it for: snaphak update | snaphak uninstall | snaphak status"
