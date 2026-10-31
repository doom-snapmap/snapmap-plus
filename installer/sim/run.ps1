# Simulate every installer migration path against scratch folders and a fake GitHub Releases API,
# and check the layout contract after every step (see installer/README.md, "Simulation harness").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\sim\run.ps1
#
# It builds the DLLs and dist\ (build.ps1, package.ps1), two installers with different version
# stamps, and the helper in this folder; copies DOOMx64vk.exe and DOOMx64.exe from your DOOM folder
# (read only) so build detection sees the real game; then runs each scenario in a fresh scratch DOOM
# and data folder, with LOCALAPPDATA, USERPROFILE and TEMP pointed at that scenario's scratch
# folders and SNAPMAP_PLUS_API_BASE at the fake API. Your real DOOM and data folders are never
# written. Exit code 0 means every step of every scenario met the contract.
#
# The store tool sends what it retires to the Windows Recycle Bin: scenario b leaves the fixture's
# retired files there.
#
# Keep this script ASCII for PowerShell 5.1.
param(
    [string]$Doom = "",              # your DOOM 2016 folder; found through Steam when omitted
    [string]$Scratch = "",           # default: %TEMP%\snapmap-plus-sim
    [switch]$SkipNativeBuild,        # reuse build\ (still repackages dist\)
    [string]$Log = ""                # also write everything printed to this file
)
$ErrorActionPreference = "Stop"
$installerDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = Split-Path -Parent $installerDir
if (-not $Scratch) { $Scratch = Join-Path $env:TEMP "snapmap-plus-sim" }
$Scratch = [System.IO.Path]::GetFullPath($Scratch)
$tagA = "v0.3.0-sim.1"
$tagB = "v0.3.0-sim.2"
$marker = ".snapmap-plus-sim"

$logLines = New-Object System.Collections.Generic.List[string]
function Say([string]$text) {
    Write-Host $text
    $logLines.Add($text)
}

function Find-Doom {
    $steam = $null
    try { $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath } catch { return $null }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        foreach ($m in [regex]::Matches((Get-Content -Raw -LiteralPath $vdf), '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value.Replace("\\", "\")
        }
    }
    foreach ($lib in $libs) {
        $c = Join-Path $lib "steamapps\common\DOOM"
        if (Test-Path -LiteralPath (Join-Path $c "DOOMx64vk.exe")) { return $c }
    }
    return $null
}

function Invoke-Native([string]$what, [scriptblock]$block) {
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# --- Checks before anything is built or written ---------------------------------------------------
if (-not $Doom) { $Doom = Find-Doom }
if (-not $Doom -or -not (Test-Path -LiteralPath (Join-Path $Doom "DOOMx64vk.exe")) -or -not (Test-Path -LiteralPath (Join-Path $Doom "DOOMx64.exe"))) {
    throw "no DOOM 2016 folder with DOOMx64vk.exe and DOOMx64.exe found -- pass -Doom <folder>"
}
if (Get-Process -Name DOOMx64vk, DOOMx64 -ErrorAction SilentlyContinue) {
    throw "DOOM is running -- the installer refuses to work while it is, so close it and run this again"
}
if (Test-Path -LiteralPath $Scratch) {
    if (-not (Test-Path -LiteralPath (Join-Path $Scratch $marker))) {
        throw "$Scratch exists and was not made by this script -- pass -Scratch <an empty or new folder>"
    }
    Remove-Item -LiteralPath $Scratch -Recurse -Force
}
New-Item -ItemType Directory -Force $Scratch | Out-Null
Set-Content -LiteralPath (Join-Path $Scratch $marker) -Value "scratch folder of installer\sim\run.ps1" -Encoding ASCII

# --- Build -----------------------------------------------------------------------------------------
Say "== Building"
if (-not $SkipNativeBuild) {
    Invoke-Native "build.ps1" { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "build.ps1") | Out-Host }
}
Invoke-Native "package.ps1" { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "package.ps1") | Out-Host }
$dist = Join-Path $repo "dist"
$binA = Join-Path $Scratch "build\$tagA\snapmap-plus.exe"
$binB = Join-Path $Scratch "build\$tagB\snapmap-plus.exe"
foreach ($pair in @(@($tagA, $binA), @($tagB, $binB))) {
    Invoke-Native "installer build.ps1 $($pair[0])" { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installerDir "build.ps1") -Version $pair[0] -Out $pair[1] | Out-Host }
}
$sim = Join-Path $Scratch "build\snapmap-plus-sim.exe"
Push-Location $installerDir
try { Invoke-Native "go build ./sim" { go build -o $sim ./sim } } finally { Pop-Location }
foreach ($pair in @(@($tagA, $binA), @($tagB, $binB))) {
    Invoke-Native "release $($pair[0])" { & $sim release --dist $dist --exe $pair[1] --out (Join-Path $Scratch "releases\$($pair[0])") }
}
Say ("  installers: {0} ({1}), {2} ({3})" -f $tagA, (Get-FileHash $binA).Hash.Substring(0, 12), $tagB, (Get-FileHash $binB).Hash.Substring(0, 12))

# --- Scratch copies of the game's files (the real folder is only read) --------------------------------
# The executables let the installer detect the build; the two archive indexes are what the store tool
# needs to accept the folder as DOOM's (the archives themselves are not copied, so the store tool
# cannot read stock files when it moves overrides\ packages).
$gameFiles = @("DOOMx64vk.exe", "DOOMx64.exe", "base\gameresources.pindex", "base\snap_gameresources.pindex")
$base = Join-Path $Scratch "base"
foreach ($f in $gameFiles) {
    $dst = Join-Path $base $f
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath (Join-Path $Doom $f) -Destination $dst
}
Say ("  copied {0} from {1}" -f ($gameFiles -join ", "), $Doom)

# --- The fake Releases API -------------------------------------------------------------------------
$served = Join-Path $Scratch "served"
New-Item -ItemType Directory -Force $served | Out-Null
function Publish([string]$tag) { Copy-Item -LiteralPath (Join-Path $Scratch "releases\$tag") -Destination (Join-Path $served $tag) -Recurse -Force }
function Unpublish([string]$tag) { if (Test-Path -LiteralPath (Join-Path $served $tag)) { Remove-Item -LiteralPath (Join-Path $served $tag) -Recurse -Force } }
$urlFile = Join-Path $Scratch "server-url.txt"
$server = Start-Process -FilePath $sim -ArgumentList @("serve", "--root", "`"$served`"", "--url-file", "`"$urlFile`"") -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $Scratch "server-requests.txt") -RedirectStandardOutput (Join-Path $Scratch "server-out.txt")
for ($i = 0; $i -lt 100 -and -not (Test-Path -LiteralPath $urlFile); $i++) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path -LiteralPath $urlFile)) { throw "the fake Releases API did not start" }
$apiBase = (Get-Content -Raw -LiteralPath $urlFile).Trim()
Say "  fake Releases API at $apiBase"

$saved = @{ LOCALAPPDATA = $env:LOCALAPPDATA; USERPROFILE = $env:USERPROFILE; TEMP = $env:TEMP; TMP = $env:TMP; API = $env:SNAPMAP_PLUS_API_BASE }
$results = New-Object System.Collections.Generic.List[object]

function New-Scenario([string]$name, [string]$exe) {
    $root = Join-Path $Scratch "scenarios\$name"
    $s = [pscustomobject]@{
        Name = $name; Root = $root
        Doom = Join-Path $root "DOOM"; LocalAppData = Join-Path $root "LocalAppData"; Profile = Join-Path $root "profile"
        Tmp = Join-Path $root "tmp"; Bin = Join-Path $root "bin\snapmap-plus.exe"
    }
    $s | Add-Member -NotePropertyName Data -NotePropertyValue (Join-Path $s.LocalAppData "snapmap-plus")
    foreach ($d in @($s.Doom, $s.LocalAppData, $s.Profile, $s.Tmp, (Split-Path -Parent $s.Bin))) { New-Item -ItemType Directory -Force $d | Out-Null }
    foreach ($f in $gameFiles) {
        $dst = Join-Path $s.Doom $f
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        New-Item -ItemType HardLink -Path $dst -Target (Join-Path $base $f) | Out-Null
    }
    Copy-Item -LiteralPath $exe -Destination $s.Bin
    return $s
}

function Use-Scenario($s) {
    $env:LOCALAPPDATA = $s.LocalAppData
    $env:USERPROFILE = $s.Profile
    $env:TEMP = $s.Tmp
    $env:TMP = $s.Tmp
    $env:SNAPMAP_PLUS_API_BASE = $apiBase
}

function Restore-Environment {
    $env:LOCALAPPDATA = $saved.LOCALAPPDATA
    $env:USERPROFILE = $saved.USERPROFILE
    $env:TEMP = $saved.TEMP
    $env:TMP = $saved.TMP
    $env:SNAPMAP_PLUS_API_BASE = $saved.API
}

# Step runs the installer, prints its full output, checks the contract and records one summary row.
function Step($s, [string]$step, [string[]]$argv, [string]$phase, [string[]]$expect, [string[]]$keep, [string[]]$mustSay) {
    Say ""
    Say ("=" * 100)
    Say ("Scenario {0}, step: {1}" -f $s.Name, $step)
    Say ("  > {0} {1}" -f $s.Bin, ($argv -join " "))
    Say ("=" * 100)
    Use-Scenario $s
    # Both streams, in order; a line on stderr is a warning or an error the installer printed.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $s.Bin @argv 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    foreach ($line in @($out)) { Say ("  | " + $line) }
    Say ("  (exit code {0})" -f $code)
    $notes = @()
    $text = (@($out) -join "`n")
    foreach ($m in $mustSay) { if (-not $text.Contains($m)) { $notes += "output lacks: $m" } }
    $checkArgs = @("check", "--stage", "$($s.Name): $step", "--doom", $s.Doom, "--data", $s.Data, "--phase", $phase) + $vanillaArgs
    foreach ($e in $expect) { $checkArgs += @("--expect", $e) }
    foreach ($k in $keep) { $checkArgs += @("--keep", $k) }
    $checkOut = & $sim @checkArgs
    $checkCode = $LASTEXITCODE
    foreach ($line in @($checkOut)) { Say $line }
    $left = @(Get-ChildItem -LiteralPath $s.Tmp -Force -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) { $notes += "TEMP holds " + (($left | ForEach-Object { $_.Name }) -join ", ") }
    Restore-Environment
    $ok = ($code -eq 0) -and ($checkCode -eq 0) -and ($notes.Count -eq 0)
    foreach ($n in $notes) { Say "  xx  $n" }
    $results.Add([pscustomobject]@{ Scenario = $s.Name; Step = $step; Exit = $code; Contract = $(if ($checkCode -eq 0) { "met" } else { "FAILED" }); Result = $(if ($ok) { "pass" } else { "FAIL" }); Notes = ($notes -join "; ") })
}

function Seed($s, [string]$layout) {
    $seedArgs = @("seed", $layout, "--doom", $s.Doom, "--data", $s.Data, "--profile", $s.Profile, "--fixtures", (Join-Path $repo "tests\fixtures"))
    Invoke-Native "seed $layout" { & $sim @seedArgs }
    Say ""
    Say ("Scenario {0}: seeded the {1} layout" -f $s.Name, $layout)
    $listArgs = @("check", "--stage", "$($s.Name): the layout before the first step", "--doom", $s.Doom, "--data", $s.Data, "--phase", "before") + $vanillaArgs
    foreach ($line in @(& $sim @listArgs)) { Say $line }
}

$pair = @("XINPUT1_4.dll", "snapmap-plus\snapmap-plus-ui-13954591.dll")
$vanillaArgs = @()
foreach ($f in $gameFiles) { $vanillaArgs += @("--vanilla", $f) }
$rollback = "dinput8.dll.doomforge.rollback"
try {
    Publish $tagB

    # a) Chrispy's original SnapHak -> install, then uninstall.
    $s = New-Scenario "a-snaphak" $binB
    Seed $s "snaphak"
    $keepA = @("prefabs\arena.snapmap.json", "strings\en.json", "rawmap.json")
    Step $s "install" @("install", "--doom", $s.Doom, "--yes", "--plain") "installed" ($pair + $rollback) $keepA @("the original SnapHak", "Not ours, left in place")
    Step $s "uninstall" @("uninstall", "--yes", "--plain") "uninstalled" @($rollback) $keepA @()

    # b) v0.2.1-beta.19's 2018 pair in this April 2024 folder, overrides\ packages -> update, then uninstall.
    $s = New-Scenario "b-beta19" $binB
    Seed $s "beta19"
    $keepB = @("config.json", "overrides\tinypack\package.json")
    Step $s "update" @("update", "--yes", "--plain") "installed" $pair $keepB @("the other DOOM build's files", "Converting mods", "snapmap-plus-store")
    Step $s "uninstall" @("uninstall", "--yes", "--plain") "uninstalled" @() $keepB @()

    # c) v0.2.1-beta.20: both pairs, backups, installer-files.json, updates.json, .old -> update, then uninstall.
    $s = New-Scenario "c-beta20" $binB
    Seed $s "beta20"
    $keepC = @("config.json", "prefabs\corridor.snapmap.json", "mods\imp\mod.json")
    Step $s "update" @("update", "--yes", "--plain") "installed" ($pair + $rollback) $keepC @("both DOOM builds' files", "installer-files.json (folded into install.json)", "Not ours, left in place")
    Step $s "uninstall" @("uninstall", "--yes", "--plain") "uninstalled" @($rollback) $keepC @()

    # d) The current release -> a newer release appears -> update (the installer updates itself
    #    first and relaunches), then uninstall.
    Unpublish $tagB
    Publish $tagA
    $s = New-Scenario "d-current" $binA
    Step $s "install $tagA" @("install", "--doom", $s.Doom, "--yes", "--plain") "installed" $pair @() @("nothing: this is a fresh install")
    Publish $tagB
    Step $s "update to $tagB" @("update", "--yes", "--plain") "installed" $pair @() @("Updated snapmap-plus.exe to $tagB; starting it.", "Found:            Snapmap+ $tagA", "Snapmap+ $tagB installed.")
    Step $s "uninstall" @("uninstall", "--yes", "--plain") "uninstalled" @() @() @()
}
finally {
    Restore-Environment
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}

Say ""
Say "== Summary"
$table = $results | Format-Table -AutoSize Scenario, Step, Exit, Contract, Result, Notes | Out-String -Width 200
foreach ($line in ($table -split "`r?`n")) { if ($line.Trim()) { Say $line } }
$failed = @($results | Where-Object { $_.Result -ne "pass" }).Count
if ($failed -eq 0) { Say ("All {0} steps met the contract." -f $results.Count) } else { Say ("{0} of {1} steps FAILED." -f $failed, $results.Count) }
$all = Join-Path $Scratch "sim-output.txt"
[System.IO.File]::WriteAllLines($all, $logLines)
if ($Log) { [System.IO.File]::WriteAllLines($Log, $logLines) }
Write-Host "Full output: $all"
if ($failed -ne 0) { exit 1 }
exit 0
