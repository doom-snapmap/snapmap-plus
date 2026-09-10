# Build the installer with Windows version resources and its application manifest.
# The release workflow passes the tag through -Version. goversioninfo is a pinned
# build tool; resource.syso is regenerated and is not a module dependency.
# See README.md for usage. Keep this script ASCII for PowerShell 5.1.
param(
    [string]$Version = "dev",
    [string]$Out = "snapmap-plus.exe"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $here
try {
    # Numeric FixedFileInfo from a vMAJOR.MINOR.PATCH tag; a plain "dev" build stays 0.0.0.
    $maj = 0; $min = 0; $pat = 0
    $m = [regex]::Match($Version, '^v?(\d+)\.(\d+)\.(\d+)')
    if ($m.Success) {
        $maj = [int]$m.Groups[1].Value
        $min = [int]$m.Groups[2].Value
        $pat = [int]$m.Groups[3].Value
    }

    # Pin the resource generator (a build tool, not a dependency).
    go install github.com/josephspurrier/goversioninfo/cmd/goversioninfo@v1.4.1
    if ($LASTEXITCODE -ne 0) { throw "goversioninfo install failed" }
    $gvi = Join-Path (& go env GOPATH) "bin\goversioninfo.exe"
    if (-not (Test-Path $gvi)) { throw "goversioninfo.exe not found at $gvi" }

    # Go embeds resource.syso from this directory. Generate it from the manifest,
    # static branding and requested version.
    & $gvi -64 -o resource.syso `
        -manifest snapmap-plus.manifest `
        -file-version $Version -product-version $Version `
        -ver-major $maj -ver-minor $min -ver-patch $pat `
        -product-ver-major $maj -product-ver-minor $min -product-ver-patch $pat `
        versioninfo.json
    if ($LASTEXITCODE -ne 0) { throw "goversioninfo failed" }

    # Remove local paths and debug symbols; stamp the CLI version.
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $Out .
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }

    Write-Host "built: installer\$Out (version $Version)"
}
finally {
    Pop-Location
}
