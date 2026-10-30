# Package the built DLLs and their hash manifest into dist/.
# The frontend uses the system WebView2 runtime. Run build.ps1 first.
# -LegacyBundle adds the final 2018 release's files (see docs/packaging.md): release.yml passes
# v0.2.1-beta.19's snapmap-plus-bundle.zip and the SHA-256 GitHub publishes for it.
# Keep this script ASCII for PowerShell 5.1.
param(
    [string]$LegacyBundle = "",
    [string]$LegacyBundleSha256 = ""
)
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path   # the repo root
$build = Join-Path $here "build"
$dist  = Join-Path $here "dist"

# The supported build's frontend path names the build, so it never collides with the 2018
# release's snapmap-plus\snapmap-plus-ui.dll. Keep in step with SH_UI_DLL_PATH in
# src/backend/ui_bridge.h and currentUIDll in installer/doombuild.go (a Go test checks them).
$uiRel = "snapmap-plus\snapmap-plus-ui-13954591.dll"
# The 2018 release's files, carried byte for byte for installers without build detection.
$legacyRels = @("XINPUT1_3.dll", "snapmap-plus\snapmap-plus-ui.dll")

# Require both DLL outputs before changing dist/.
$backendDll = Join-Path $build "XINPUT1_4.dll"
$uiDll      = Join-Path $build "webview\snapmap-plus-ui.dll"
foreach ($d in @($backendDll, $uiDll)) {
    if (-not (Test-Path $d)) { throw "missing $d -- run build.ps1 first (repo root; builds backend + frontend together)." }
}

# Reject diagnostic backends: sh_diag.log is present only in diagnostic builds.
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($backendDll))
if ($ascii.Contains("sh_diag.log")) {
    throw "build\XINPUT1_4.dll is a -Diag build (DO NOT DISTRIBUTE). Rebuild release with src\backend\build.ps1 (no -Diag)."
}

# Check the 2018 release's bundle before changing dist/: the zip must have the published digest,
# every file must match its own MANIFEST.sha256, and the files must be exactly the two expected.
$legacyDir = $null
if ($LegacyBundle) {
    if ($LegacyBundleSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "-LegacyBundle needs -LegacyBundleSha256 (the digest GitHub publishes)" }
    $zipHash = (Get-FileHash -LiteralPath $LegacyBundle -Algorithm SHA256).Hash
    if ($zipHash -ne $LegacyBundleSha256) { throw "$LegacyBundle has SHA-256 $zipHash, not the published $LegacyBundleSha256" }
    $legacyDir = Join-Path ([System.IO.Path]::GetTempPath()) ("snapmap-plus-legacy-" + [guid]::NewGuid().ToString("N"))
    Expand-Archive -LiteralPath $LegacyBundle -DestinationPath $legacyDir
    $listed = @()
    foreach ($line in Get-Content -LiteralPath (Join-Path $legacyDir "MANIFEST.sha256")) {
        if (-not $line.Trim()) { continue }
        $hash, $rel = -split $line
        $rel = $rel.Replace("/", "\")
        $got = (Get-FileHash -LiteralPath (Join-Path $legacyDir $rel) -Algorithm SHA256).Hash
        if ($got -ne $hash) { throw "the 2018 bundle's $rel does not match its MANIFEST.sha256" }
        $listed += $rel
    }
    if ((@($listed | Sort-Object) -join "|") -ne (@($legacyRels | Sort-Object) -join "|")) {
        throw "the 2018 bundle lists $($listed -join ', '), not $($legacyRels -join ', ')"
    }
}

try {
    # Recreate the bundle directory.
    if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
    $snapDir = Join-Path $dist "snapmap-plus"
    New-Item -ItemType Directory -Force $snapDir | Out-Null

    Copy-Item $backendDll (Join-Path $dist "XINPUT1_4.dll")
    Copy-Item $uiDll      (Join-Path $dist $uiRel)
    $files = @("XINPUT1_4.dll", $uiRel)
    if ($legacyDir) {
        foreach ($rel in $legacyRels) { Copy-Item -LiteralPath (Join-Path $legacyDir $rel) -Destination (Join-Path $dist $rel) }
        $files += $legacyRels
    }

    # Record the installed paths and SHA-256 hashes.
    $lines = foreach ($f in $files) {
        $h = (Get-FileHash (Join-Path $dist $f) -Algorithm SHA256).Hash
        "{0}  {1}" -f $h, $f
    }
    [System.IO.File]::WriteAllLines((Join-Path $dist "MANIFEST.sha256"), $lines, (New-Object System.Text.UTF8Encoding $false))
} finally {
    if ($legacyDir) { Remove-Item -LiteralPath $legacyDir -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "packaged $($files.Count) files into $dist :"
foreach ($f in $files) {
    Write-Host ("  {0,-44} {1,10}" -f $f, (Get-Item (Join-Path $dist $f)).Length)
}
Write-Host "MANIFEST.sha256 written (the install/verify map)."
