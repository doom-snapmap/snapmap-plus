# Check a packaged dist/ folder before it is published: the April 2024 pair, its manifest and the
# store tool are present; the 2018 release's pair and any Qt-era file are not. CI and release.yml
# both run it, and an installer test reads its two lists. Keep this script ASCII for PowerShell 5.1.
param([string]$Dist = "dist")
$ErrorActionPreference = "Stop"

$required = @("XINPUT1_4.dll", "snapmap-plus\snapmap-plus-ui-13954591.dll", "MANIFEST.sha256", "snapmap-plus-store.exe", "TOOLS.sha256")
$forbidden = @("XINPUT1_3.dll", "snapmap-plus\snapmap-plus-ui.dll", "platforms")

foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $Dist $f))) { throw "bundle guard: missing $f in $Dist" }
}
foreach ($f in $forbidden) {
    if (Test-Path (Join-Path $Dist $f)) { throw "bundle guard: $Dist\$f belongs to the 2018 release or the Qt frontend and must not be packaged" }
}
$qtish = @(Get-ChildItem -Recurse -File $Dist | Where-Object { $_.Name.ToLower() -match '^qt5|^qwindows\.dll$' })
if ($qtish.Count) { throw "bundle guard: Qt-shaped file(s) in $Dist ($($qtish.Name -join ', '))" }
Write-Host "bundle guard OK: the April 2024 pair, its manifest and the store tool, nothing else"
