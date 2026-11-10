# Shared MSVC build helpers for src\backend\build.ps1, src\ui\build.ps1 and src\store\build.ps1.
# Dot-source this file. Sources compile to objects (in parallel), and a
# separate step links them, so a rebuild recompiles only what changed. Keep this script ASCII for
# PowerShell 5.1.
#
# A source is rebuilt when its object is missing, when the source or any project file it includes
# (quoted #include, followed recursively) is newer than the object, or when the compile flags or
# the toolset changed. Angle-bracket includes are system or SDK headers; the toolset and the SDK
# path are part of the flags record, so a change to either rebuilds everything.

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

# The newest write time over a source and every project file it includes, memoized per run.
$script:CbIncludeMemo = @{}
function Get-CbNewestInput([string]$Path, [string[]]$IncludeDirs) {
    $seen = @{}
    $stack = New-Object System.Collections.Stack
    $stack.Push([System.IO.Path]::GetFullPath($Path))
    $newest = [DateTime]::MinValue
    while ($stack.Count) {
        $file = $stack.Pop()
        $k = $file.ToLowerInvariant()
        if ($seen.ContainsKey($k)) { continue }
        $seen[$k] = $true
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $t = [System.IO.File]::GetLastWriteTimeUtc($file)
        if ($t -gt $newest) { $newest = $t }
        $memoKey = "$k|" + ($IncludeDirs -join ";")
        $deps = $script:CbIncludeMemo[$memoKey]
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
            $script:CbIncludeMemo[$memoKey] = $deps
        }
        foreach ($d in $deps) { $stack.Push($d) }
    }
    return $newest
}

# Record the flags an object directory was built with; a change empties the directory's objects.
function Sync-CbFlagsRecord([string]$ObjDir, [string]$Flags) {
    New-Item -ItemType Directory -Force $ObjDir | Out-Null
    $record = Join-Path $ObjDir "flags.txt"
    $want = "$Flags`r`ntoolset=$env:VCToolsVersion cl=$((Get-Command cl).Source)"
    $have = if (Test-Path $record) { [System.IO.File]::ReadAllText($record) } else { $null }
    if ($have -ne $want) {
        Get-ChildItem $ObjDir -Filter *.obj -File -ErrorAction SilentlyContinue | Remove-Item -Force
        [System.IO.File]::WriteAllText($record, $want)
    }
}

# Compile the stale sources of one flag set into ObjDir with a single cl /c /MP call, run from
# WorkDir. ObjDirArg is ObjDir as cl sees it from WorkDir (relative paths avoid cmd quoting
# issues). Returns the object paths in source order, which is the link order.
function Invoke-CbCompile {
    param([string]$WorkDir, [string[]]$Sources, [string]$Flags, [string]$ObjDir, [string]$ObjDirArg,
          [string[]]$IncludeDirs, [string]$Log)
    Sync-CbFlagsRecord $ObjDir $Flags
    $objs = @(); $stale = @(); $names = @{}
    foreach ($s in $Sources) {
        $s = $s.Trim()
        $base = [System.IO.Path]::GetFileNameWithoutExtension($s)
        if ($names.ContainsKey($base.ToLowerInvariant())) { throw "two sources share the object name $base.obj: $s" }
        $names[$base.ToLowerInvariant()] = $true
        $obj = Join-Path $ObjDir ($base + ".obj")
        $objs += $obj
        if (-not (Test-Path $obj) -or
            (Get-CbNewestInput (Join-Path $WorkDir $s) $IncludeDirs) -gt [System.IO.File]::GetLastWriteTimeUtc($obj)) {
            $stale += $s
        }
    }
    if ($stale.Count) {
        $srcArgs = ($stale | ForEach-Object { '"' + $_ + '"' }) -join " "
        cmd /c "cd /d `"$WorkDir`" && cl /c /MP $Flags /Fo$ObjDirArg $srcArgs > `"$Log`" 2>&1"
        $exit = $LASTEXITCODE
        Get-Content $Log | Write-Host
        if ($exit -ne 0) { throw "cl failed (exit $exit) -- see $Log" }
    }
    Write-Host ("[build] compiled {0} of {1} sources" -f $stale.Count, $Sources.Count)
    return ,$objs
}

# Link when the output is missing, older than an object, or was linked by a different command.
# The previous PDB is removed first so a rebuild writes the same bytes as a clean build.
function Invoke-CbLink {
    param([string]$WorkDir, [string]$Command, [string[]]$Objects, [string]$Output, [string]$Log,
          [string[]]$Remove = @())
    $record = "$Output.link.txt"
    $need = -not (Test-Path $Output)
    if (-not $need) {
        $outTime = [System.IO.File]::GetLastWriteTimeUtc($Output)
        foreach ($o in $Objects) { if ([System.IO.File]::GetLastWriteTimeUtc($o) -gt $outTime) { $need = $true; break } }
    }
    if (-not $need) {
        $have = if (Test-Path $record) { [System.IO.File]::ReadAllText($record) } else { $null }
        if ($have -ne $Command) { $need = $true }
    }
    if (-not $need) { Write-Host "[build] up to date: $Output"; return }
    foreach ($r in $Remove) { Remove-Item $r -Force -ErrorAction SilentlyContinue }
    Remove-Item $record -Force -ErrorAction SilentlyContinue
    cmd /c "cd /d `"$WorkDir`" && $Command > `"$Log`" 2>&1"
    $exit = $LASTEXITCODE
    Get-Content $Log | Write-Host
    if ($exit -ne 0) { throw "link failed (exit $exit) -- see $Log" }
    [System.IO.File]::WriteAllText($record, $Command)
}
