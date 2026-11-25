# Download the current installer, which identifies DOOM and chooses its release.
$ErrorActionPreference = "Stop"
$repo = "doom-snapmap/snapmap-plus"

# Releases are looked up through the GitHub API, as snapmap-plus.exe does. SNAPMAP_PLUS_API_BASE
# (a loopback URL only) points the lookups at a local mock for testing.
$api = "https://api.github.com"
if ($env:SNAPMAP_PLUS_API_BASE -match '^https?://(127\.0\.0\.1|localhost|\[::1\])(:\d+)?/?$') {
    $api = $env:SNAPMAP_PLUS_API_BASE.TrimEnd("/")
}
$releases = "$api/repos/$repo/releases"
$headers = @{ "User-Agent" = "snapmap-plus-installer"; "Accept" = "application/vnd.github+json" }

# A release tag's sort key: vMAJOR.MINOR.PATCH ranks above its -beta.N prereleases. $null for
# a tag of another shape.
function Get-VersionKey([string]$tag) {
    if ($tag -notmatch '^v(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?$') { return $null }
    $stable = if ($Matches[4]) { 0 } else { 1 }
    $beta = if ($Matches[4]) { [int]$Matches[4] } else { 0 }
    return [pscustomobject]@{ Version = [version]("{0}.{1}.{2}" -f $Matches[1], $Matches[2], $Matches[3]); Stable = $stable; Beta = $beta }
}

function Get-Release {
    # The newest stable release by version, or the newest beta while none is published. The
    # latest-release download link cannot be used: it never serves a beta.
    $all = Invoke-RestMethod -UseBasicParsing -TimeoutSec 30 -Uri ($releases + "?per_page=100") -Headers $headers
    $list = @($all | Where-Object { -not $_.draft -and (Get-VersionKey $_.tag_name) } | Sort-Object -Descending `
        @{ Expression = { (Get-VersionKey $_.tag_name).Version } }, @{ Expression = { (Get-VersionKey $_.tag_name).Stable } },
        @{ Expression = { (Get-VersionKey $_.tag_name).Beta } })
    $stable = @($list | Where-Object { -not $_.prerelease })
    if ($stable.Count -gt 0) { return $stable[0] }
    if ($list.Count -gt 0) { return $list[0] }
    throw "No Snapmap+ release has been published yet; try again later."
}

# Download a release's snapmap-plus.exe and check it against the size and digest GitHub publishes.
function Save-Installer($release, [string]$out) {
    $asset = @($release.assets | Where-Object { $_.name -eq "snapmap-plus.exe" }) | Select-Object -First 1
    if (-not $asset) { throw "Release $($release.tag_name) has no snapmap-plus.exe yet; try again in a minute." }
    $part = "$out.download"
    try {
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 900 -Uri $asset.browser_download_url -OutFile $part
        $size = (Get-Item -LiteralPath $part).Length
        if ($asset.size -and $size -ne $asset.size) {
            throw "snapmap-plus.exe from $($release.tag_name) is incomplete ($size of $($asset.size) bytes); run this again."
        }
        if ("$($asset.digest)" -match '^sha256:([0-9a-fA-F]{64})$') {
            $want = $Matches[1]
            if ((Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash -ne $want) {
                throw "snapmap-plus.exe from $($release.tag_name) failed its integrity check; run this again."
            }
        }
        Move-Item -LiteralPath $part -Destination $out -Force
    } finally {
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
    }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("snapmap-plus-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work -ErrorAction Stop | Out-Null
$exe = Join-Path $work "snapmap-plus.exe"
try {
    Save-Installer (Get-Release) $exe
    $installArgs = @("install")
    if ($env:SNAPMAP_PLUS_DOOM) { $installArgs += @("--doom", $env:SNAPMAP_PLUS_DOOM) }
    & $exe @installArgs
    if ($LASTEXITCODE -ne 0) { throw "snapmap-plus.exe install failed (exit $LASTEXITCODE); its lines above say why." }
} finally {
    Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
    try { [System.IO.Directory]::Delete($work, $false) } catch [System.IO.IOException] { Write-Verbose $_ }
}

$installed = Join-Path $env:LOCALAPPDATA "snapmap-plus\snapmap-plus.exe"
if (Test-Path -LiteralPath $installed -PathType Leaf) {
    Write-Host "snapmap-plus.exe is at $installed"
    Write-Host "Run it for: snapmap-plus update | snapmap-plus uninstall | snapmap-plus status"
}
