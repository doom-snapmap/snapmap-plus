# Run the script SDK's Lua library tests (src\sdk\lua) with a standalone Lua 5.4 host: -Lua, else SNAPMAP_LUA, else
# lua54 or lua on PATH (tests\sdk\run.ps1 -Lua C:\path\to\lua54.exe). The tests carry their own simulated engine
# and load src\sdk\lua, or the folder SNAPMAP_SDK_LUA names; exit 0 means every file passed.
param([string]$Lua = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)

if (-not $Lua) { $Lua = $env:SNAPMAP_LUA }
if (-not $Lua) {
    foreach ($name in @("lua54", "lua")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { $Lua = $cmd.Source; break }
    }
}
if (-not $Lua -or -not (Test-Path $Lua)) {
    Write-Error "no Lua 5.4 host: pass -Lua <path to lua54.exe> or set SNAPMAP_LUA"
    exit 2
}

$failed = 0
Push-Location $repo
try {
    foreach ($test in (Get-ChildItem $here -Filter "test_*.lua" | Sort-Object Name)) {
        Write-Host "== $($test.Name)"
        & $Lua $test.FullName
        if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "FAILED: $($test.Name) (exit $LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}
if ($failed -gt 0) { Write-Host "$failed SDK test file(s) failed"; exit 1 }
Write-Host "SDK library tests passed"
exit 0
