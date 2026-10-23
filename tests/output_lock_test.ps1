# Exercise output ownership with real processes, without a compiler or game.
param([switch]$Child, [string]$Directory, [string]$Signal, [string]$Mode = 'normal')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\output-lock.ps1')

if ($Child) {
    $held = Enter-OutputLock -Directory $Directory -Label 'test runner'
    try {
        [IO.File]::WriteAllText($Signal, 'ready')
        if ($Mode -eq 'throw') { throw 'intentional owner failure' }
        if ($Mode -eq 'exit') { exit 7 }
        if ($Mode -eq 'hold') {
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            while (-not [IO.File]::Exists($Signal + '.release')) {
                if ([DateTime]::UtcNow -ge $deadline) { throw 'test owner timed out' }
                Start-Sleep -Milliseconds 50
            }
        }
    } finally { $held.Dispose() }
    exit 0
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('snapmap-output-lock-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($scratch)
$children = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
$checks = 0
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Start-Owner([string]$Target, [string]$ChildMode, [string]$Name) {
    $signalPath = Join-Path $scratch ($Name + '.ready')
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        ('"' + $PSCommandPath + '"'), '-Child', '-Directory', ('"' + $Target + '"'),
        '-Signal', ('"' + $signalPath + '"'), '-Mode', $ChildMode)
    $childProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $scratch ($Name + '.out')) `
        -RedirectStandardError (Join-Path $scratch ($Name + '.err'))
    # Keep the process handle before a short-lived child exits. Windows
    # PowerShell's Start-Process object can otherwise lose its exit code.
    $null = $childProcess.Handle
    $children.Add($childProcess)
    return @{ process = $childProcess; signal = $signalPath; error = (Join-Path $scratch ($Name + '.err')) }
}
function Wait-Ready($Owner) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not [IO.File]::Exists($Owner.signal)) {
        if ($Owner.process.HasExited) { throw ('owner failed before ready: ' + [IO.File]::ReadAllText($Owner.error)) }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'owner startup timed out' }
        Start-Sleep -Milliseconds 50
    }
}
function Wait-Exit($Owner, [int]$Expected) {
    if (-not $Owner.process.WaitForExit(15000)) { throw 'owner exit timed out' }
    Assert-True ($Owner.process.ExitCode -eq $Expected) ('unexpected exit: ' + $Owner.process.ExitCode)
}
function Can-Acquire([string]$Target) {
    $lease = Enter-OutputLock -Directory $Target
    try { Assert-True ($null -ne $lease) 'lock acquisition did not return a handle' }
    finally { $lease.Dispose() }
}

try {
    $target = Join-Path $scratch ('outputs with spaces ' + [char]0x03b1)
    $first = Start-Owner $target 'hold' 'first'
    Wait-Ready $first
    $lockPath = Join-Path $target '.output.lock'
    $artifact = Join-Path $target 'retained.obj'
    [IO.File]::WriteAllText($artifact, 'original artifact')
    $contender = Start-Owner (Join-Path $target '.') 'normal' 'contender'
    Wait-Exit $contender 1
    Assert-True (-not [IO.File]::Exists($contender.signal)) 'contender reached protected work'
    Assert-True ([IO.File]::ReadAllText($contender.error).Contains('Another test runner owns')) 'contention lost its actionable diagnostic'
    Assert-True ([IO.File]::ReadAllText($artifact) -eq 'original artifact') 'contention changed prior artifacts'
    Can-Acquire (Join-Path $scratch 'independent checkout')
    [IO.File]::WriteAllText($first.signal + '.release', '')
    Wait-Exit $first 0
    $record = [IO.File]::ReadAllText($lockPath) | ConvertFrom-Json
    Assert-True ($record.pid -eq $first.process.Id) 'contender overwrote owner metadata'
    Assert-True ([IO.File]::Exists($lockPath)) 'release removed the stable lock file'
    Can-Acquire $target

    foreach ($case in @(@('normal', 0), @('throw', 1), @('exit', 7))) {
        $owner = Start-Owner $target $case[0] ('release-' + $case[0])
        Wait-Exit $owner $case[1]
        Assert-True ([IO.File]::Exists($owner.signal)) 'release control never acquired ownership'
        Can-Acquire $target
    }
    $killed = Start-Owner $target 'hold' 'terminated'
    Wait-Ready $killed
    $killed.process.Kill()
    Assert-True ($killed.process.WaitForExit(15000)) 'terminated owner did not exit'
    Can-Acquire $target

    [IO.File]::WriteAllText($lockPath, 'stale or malformed metadata is not ownership')
    Can-Acquire $target
    $notDirectory = Join-Path $scratch 'regular-file'
    [IO.File]::WriteAllText($notDirectory, 'keep')
    $failed = $false
    try { $unexpected = Enter-OutputLock -Directory $notDirectory; $unexpected.Dispose() }
    catch { $failed = $true; Assert-True (-not $_.Exception.Message.Contains('Another')) 'non-contention error misclassified' }
    Assert-True $failed 'a file was accepted as an output directory'
    Assert-True ([IO.File]::ReadAllText($notDirectory) -eq 'keep') 'failed acquisition changed an existing file'
    Write-Host "output_lock_test: PASS ($checks checks)"
} finally {
    foreach ($childProcess in $children) {
        if (-not $childProcess.HasExited) { $childProcess.Kill(); [void]$childProcess.WaitForExit(15000) }
        $childProcess.Dispose()
    }
    # Only remove this test's verified, newly-created directory; never follow a
    # computed workspace, user-provided output path, or lock owner's directory.
    $resolved = [IO.Path]::GetFullPath($scratch)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^snapmap-output-lock-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
