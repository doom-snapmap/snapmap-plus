# Download the latest release installer into local app data and run install.
# Use the downloaded executable for later update, uninstall and status commands.
$ErrorActionPreference = "Stop"

$repo = "doom-snapmap/snapmap-plus"

$dest = Join-Path $env:LOCALAPPDATA "snapmap-plus"
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest "snapmap-plus.exe"

Write-Host "Downloading snapmap-plus.exe ..."
Invoke-WebRequest -Uri "https://github.com/$repo/releases/latest/download/snapmap-plus.exe" -OutFile $exe

Write-Host "Installing Snapmap+ ..."
& $exe install

Write-Host ""
Write-Host "snapmap-plus.exe is at $exe"
Write-Host "Run it for: snapmap-plus update | snapmap-plus uninstall | snapmap-plus status"
