# Shared MSVC build helpers for src\backend\build.ps1, src\ui\build.ps1, src\store\build.ps1 and
# tests\run-tests.ps1. Dot-source this file. Sources compile to objects (once, in parallel), and a
# separate step links them, so a rebuild recompiles only what changed. Keep this script ASCII for
# PowerShell 5.1.
#
# Staleness is decided by content, never by file times (a fresh checkout dates every file at the
# checkout). Each object has a key beside it (<obj>.key): a SHA-256 over the compile flags, the
# toolset, and the path and text of the source and of every project file it includes (quoted
# #include, followed recursively). A source recompiles when its object or key is missing or the key
# differs, so build\obj can be restored from a cache. Angle-bracket includes are system or SDK
# headers; the toolset and the SDK path (in the flags) are part of the key.

# Import the vcvars64 environment into this process once. A child build started by this process
# inherits it and skips the import (vcvars64.bat costs a few seconds per call).
function Import-MsvcEnv([string]$VcVarsVer) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere not found. Install Build Tools for Visual Studio 2022 (C++ workload)."
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "VC Tools (x86/x64) not found in any VS install." }
    $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

    # Resolve and verify the requested toolset before invoking vcvars64.bat.
    $vcvarsArgs = ""
    $toolsetDir = Join-Path $vs "VC\Tools\MSVC"
    if ($VcVarsVer) {
        if (-not (Test-Path (Join-Path $toolsetDir $VcVarsVer))) {
            $have = (Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ", "
            throw ("pinned MSVC toolset $VcVarsVer is not installed (have: $have). Install it via the VS " +
                   "Installer, or update the pin in .github/workflows/release.yml (SNAPMAPPLUS_VCVARS_VER).")
        }
        $vcvarsArgs = " -vcvars_ver=$VcVarsVer"
        $toolset = $VcVarsVer + " (pinned)"
    } else {
        # Without a pin, report the newest installed toolset for build diagnostics.
        $newest = Get-ChildItem $toolsetDir -Directory -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { $toolset = $newest.Name + " (newest installed, NOT pinned)" }
        else { $toolset = "unknown (NOT pinned)" }
    }
    $key = "$vcvars|$VcVarsVer"
    if ($env:SNAPMAPPLUS_MSVC_ENV -ne $key) {
        # Names starting with "=" are cmd's per-drive working directories and are skipped.
        cmd /c "`"$vcvars`"$vcvarsArgs >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
        if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "cl not on PATH after $vcvars" }
        $env:SNAPMAPPLUS_MSVC_ENV = $key
    }
    return $toolset
}

# The SHA-256 of a file's bytes, and the quoted includes it names, memoized per run.
$script:CbHashMemo = @{}
$script:CbDepsMemo = @{}
$script:CbSha = [System.Security.Cryptography.SHA256]::Create()
function Get-CbHex([byte[]]$Bytes) {
    return (-join ($script:CbSha.ComputeHash($Bytes) | ForEach-Object { $_.ToString("x2") }))
}

# The content key of a source compiled with Flags: see the header of this file.
function Get-CbInputKey([string]$Path, [string[]]$IncludeDirs, [string]$Flags) {
    if (-not $script:CbCl) { $script:CbCl = (Get-Command cl).Source }
    $seen = @{}
    $stack = New-Object System.Collections.Stack
    $stack.Push([System.IO.Path]::GetFullPath($Path))
    while ($stack.Count) {
        $file = $stack.Pop()
        $k = $file.ToLowerInvariant()
        if ($seen.ContainsKey($k)) { continue }
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        if (-not $script:CbHashMemo.ContainsKey($k)) {
            $script:CbHashMemo[$k] = Get-CbHex ([System.IO.File]::ReadAllBytes($file))
        }
        $seen[$k] = $script:CbHashMemo[$k]
        $memoKey = "$k|" + ($IncludeDirs -join ";")
        $deps = $script:CbDepsMemo[$memoKey]
        if ($null -eq $deps) {
            $deps = @()
            $dir = Split-Path -Parent $file
            foreach ($line in [System.IO.File]::ReadLines($file)) {
                if ($line -match '^\s*#\s*include\s*"([^"]+)"') {
                    $name = $matches[1]
                    foreach ($base in @($dir) + $IncludeDirs) {
                        $cand = [System.IO.Path]::GetFullPath((Join-Path $base $name))
                        if (Test-Path -LiteralPath $cand -PathType Leaf) { $deps += $cand; break }
                    }
                }
            }
            $script:CbDepsMemo[$memoKey] = $deps
        }
        foreach ($d in $deps) { $stack.Push($d) }
    }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("flags=$Flags`ntoolset=$env:VCToolsVersion`ncl=$($script:CbCl)`n")
    foreach ($f in @($seen.Keys | Sort-Object)) { [void]$sb.Append("$f`t$($seen[$f])`n") }
    return Get-CbHex ([System.Text.Encoding]::UTF8.GetBytes($sb.ToString()))
}

# The key an object was compiled with, or $null when the object or its key is missing.
function Get-CbObjectKey([string]$Obj) {
    if (-not (Test-Path -LiteralPath $Obj) -or -not (Test-Path -LiteralPath "$Obj.key")) { return $null }
    return [System.IO.File]::ReadAllText("$Obj.key")
}

# Compile the stale sources of one flag set into ObjDir with a single cl /c /MP call, run from
# WorkDir. ObjDirArg is ObjDir as cl sees it from WorkDir (relative paths avoid cmd quoting
# issues). Returns the object paths in source order, which is the link order.
function Invoke-CbCompile {
    param([string]$WorkDir, [string[]]$Sources, [string]$Flags, [string]$ObjDir, [string]$ObjDirArg,
          [string[]]$IncludeDirs, [string]$Log)
    New-Item -ItemType Directory -Force $ObjDir | Out-Null
    # The release version root build.ps1 stamps into every compile as SNAPMAP_PLUS_VERSION. It is
    # part of the flags, so a new version recompiles what it reaches.
    if ($env:SNAPMAP_PLUS_VERSION) {
        if ($env:SNAPMAP_PLUS_VERSION -notmatch '^[0-9A-Za-z.\-]+$') { throw "SNAPMAP_PLUS_VERSION '$env:SNAPMAP_PLUS_VERSION' is not a version" }
        $Flags = $Flags + ' /DSNAPMAP_PLUS_VERSION=\"' + $env:SNAPMAP_PLUS_VERSION + '\"'
    }
    $objs = @(); $stale = @(); $keys = @{}; $names = @{}
    foreach ($s in $Sources) {
        $s = $s.Trim()
        $base = [System.IO.Path]::GetFileNameWithoutExtension($s)
        if ($names.ContainsKey($base.ToLowerInvariant())) { throw "two sources share the object name $base.obj: $s" }
        $names[$base.ToLowerInvariant()] = $true
        $obj = Join-Path $ObjDir ($base + ".obj")
        $objs += $obj
        $key = Get-CbInputKey (Join-Path $WorkDir $s) $IncludeDirs $Flags
        if ((Get-CbObjectKey $obj) -ne $key) {
            $stale += $s
            $keys[$obj] = $key
            Remove-Item -LiteralPath "$obj.key" -Force -ErrorAction SilentlyContinue
        }
    }
    if ($stale.Count) {
        $srcArgs = ($stale | ForEach-Object { '"' + $_ + '"' }) -join " "
        cmd /c "cd /d `"$WorkDir`" && cl /c /MP $Flags /Fo$ObjDirArg $srcArgs > `"$Log`" 2>&1"
        $exit = $LASTEXITCODE
        Get-Content $Log | Write-Host
        if ($exit -ne 0) { throw "cl failed (exit $exit) -- see $Log" }
        foreach ($o in $keys.Keys) { [System.IO.File]::WriteAllText("$o.key", $keys[$o]) }
    }
    Write-Host ("[build] compiled {0} of {1} sources" -f $stale.Count, $Sources.Count)
    return ,$objs
}

# Link when the output is missing or the link command or an object's key changed (the record
# beside the output holds both). The previous PDB is removed first so a rebuild writes the same
# bytes as a clean build.
function Get-CbLinkRecord([string]$Command, [string[]]$Objects) {
    return $Command + "`n" + (($Objects | ForEach-Object { Get-CbObjectKey $_ }) -join "`n")
}
function Invoke-CbLink {
    param([string]$WorkDir, [string]$Command, [string[]]$Objects, [string]$Output, [string]$Log,
          [string[]]$Remove = @())
    $record = "$Output.link.txt"
    $want = Get-CbLinkRecord $Command $Objects
    $need = -not (Test-Path $Output) -or -not (Test-Path $record) -or [System.IO.File]::ReadAllText($record) -ne $want
    if (-not $need) { Write-Host "[build] up to date: $Output"; return }
    foreach ($r in $Remove) { Remove-Item $r -Force -ErrorAction SilentlyContinue }
    Remove-Item $record -Force -ErrorAction SilentlyContinue
    cmd /c "cd /d `"$WorkDir`" && $Command > `"$Log`" 2>&1"
    $exit = $LASTEXITCODE
    Get-Content $Log | Write-Host
    if ($exit -ne 0) { throw "link failed (exit $exit) -- see $Log" }
    [System.IO.File]::WriteAllText($record, $want)
}

# Run command lines through cmd.exe, at most $Max at a time, each with its output in its own log.
# A job is @{ dir; cmd; log } with an optional weight (higher starts first, for example the job's
# previous duration). Each job gets .seconds, its run time. Returns the exit codes in job order.
function Invoke-CbParallel([object[]]$Jobs, [int]$Max = [Environment]::ProcessorCount) {
    $codes = New-Object int[] $Jobs.Count
    $order = @(0..($Jobs.Count - 1) | Where-Object { $Jobs.Count } |
               Sort-Object @{ Expression = { -[double]$Jobs[$_].weight } }, @{ Expression = { $_ } })
    $running = @{}
    $next = 0
    while ($next -lt $order.Count -or $running.Count) {
        while ($next -lt $order.Count -and $running.Count -lt $Max) {
            $i = $order[$next]
            $j = $Jobs[$i]
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $env:ComSpec
            $psi.Arguments = "/s /c `"$($j.cmd) > `"$($j.log)`" 2>&1`""
            $psi.WorkingDirectory = $j.dir
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            $running[$i] = @{ p = [System.Diagnostics.Process]::Start($psi); t = [Diagnostics.Stopwatch]::StartNew() }
            $next++
        }
        $done = @($running.Keys | Where-Object { $running[$_].p.HasExited })
        if (-not $done.Count) { [System.Threading.Thread]::Sleep(10); continue }
        foreach ($i in $done) {
            $r = $running[$i]
            $r.p.WaitForExit()
            $codes[$i] = $r.p.ExitCode
            $Jobs[$i].seconds = $r.t.Elapsed.TotalSeconds
            $r.p.Dispose()
            $running.Remove($i)
        }
    }
    return ,$codes
}
