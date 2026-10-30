# Identify the installed DOOM 2016 build, download the matching Snapmap+ release and install it.
# Snapmap+ supports the April 2024 update (Steam build 13954591); the 2018 build gets the final
# release made for it, v0.2.1-beta.19, installed by that release's own installer.
# The DOOM folder is found through Steam, or set SNAPMAP_PLUS_DOOM to it first.
# Keep this script ASCII for Windows PowerShell 5.1.
$ErrorActionPreference = "Stop"

$repo = "doom-snapmap/snapmap-plus"
# The final release for the 2018 build. It must stay published unchanged (docs/packaging.md).
$legacyTag = "v0.2.1-beta.19"

# Executable SHA-256 -> build. Keep in step with installer/doombuild.go (a Go test checks it).
$builds = @{
    "3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21" = "13954591"  # April 2024 Vulkan
    "1bddecaa4e86850412f96e3117978bce1ec7bb701ae1b5a33120c21e4421e51e" = "13954591"  # April 2024 OpenGL
    "139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323" = "2018"      # 2018 Vulkan
    "d46ca967824da6ffc40f1ce0b6d7f4afa868fe12b795124d3e31a1bdb646a8e5" = "2018"      # 2018 OpenGL
}
$exeNames = @("DOOMx64vk.exe", "DOOMx64.exe")
$supported = "Snapmap+ supports the April 2024 update (Steam build 13954591); the 2018 build gets the final 2018 release $legacyTag."

function Test-DoomDir([string]$dir) {
    foreach ($n in $exeNames) { if (Test-Path -LiteralPath (Join-Path $dir $n) -PathType Leaf) { return $true } }
    return $false
}

function Find-Doom {
    if ($env:SNAPMAP_PLUS_DOOM) {
        if (Test-DoomDir $env:SNAPMAP_PLUS_DOOM) { return $env:SNAPMAP_PLUS_DOOM }
        throw "SNAPMAP_PLUS_DOOM ($env:SNAPMAP_PLUS_DOOM) holds no DOOMx64vk.exe or DOOMx64.exe."
    }
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { throw "Steam was not found. Set SNAPMAP_PLUS_DOOM to your DOOM 2016 folder and run this again." }
    $libraries = @($steam.Replace("/", "\"))
    $vdf = Join-Path $libraries[0] "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') { $libraries += $Matches[1].Replace("\\", "\") }
        }
    }
    foreach ($lib in $libraries) {
        $dir = Join-Path $lib "steamapps\common\DOOM"
        if (Test-DoomDir $dir) { return $dir }
    }
    throw "DOOM 2016 was not found in your Steam libraries. Set SNAPMAP_PLUS_DOOM to its folder and run this again."
}

# Identify the build before downloading anything.
$doom = Find-Doom
$build = $null
foreach ($n in $exeNames) {
    $path = Join-Path $doom $n
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $this = $builds[$hash]
    if (-not $this) {
        throw "$n in $doom is not a DOOM 2016 executable Snapmap+ recognizes (SHA-256 $hash). $supported If a mod loader or patch changed it, verify the game files in Steam and run this again. Nothing was changed."
    }
    if ($build -and $build -ne $this) {
        throw "DOOMx64vk.exe and DOOMx64.exe in $doom come from different DOOM builds ($build and $this); verify the game files in Steam. $supported Nothing was changed."
    }
    $build = $this
    # The same game identity the backend, the installer and every report use.
    $steam = if ($this -eq "2018") { "2642713" } else { $this }
    $binary = if ($this -eq "2018") { "20180321-154133-liver-goldenrod" } else { "20240321-104810-ginger-fuchsia" }
    $renderer = if ($n -eq "DOOMx64vk.exe") { "Vulkan" } else { "OpenGL" }
    Write-Host ("Game: {0}: DOOM 2016 build {1} ({2}, {3}, {4})" -f $n, $steam, $binary, $renderer, $hash.Substring(0, 8))
}

# Releases are looked up through the GitHub API, as snapmap-plus.exe does. SNAPMAP_PLUS_API_BASE
# (a loopback URL only) points the lookups at a local mock for testing.
$api = "https://api.github.com"
if ($env:SNAPMAP_PLUS_API_BASE -match '^https?://(127\.0\.0\.1|localhost|\[::1\])(:\d+)?/?$') {
    $api = $env:SNAPMAP_PLUS_API_BASE.TrimEnd("/")
}
$releases = "$api/repos/$repo/releases"
$headers = @{ "User-Agent" = "snapmap-plus-installer"; "Accept" = "application/vnd.github+json" }

function Get-Release([string]$tag) {
    if ($tag) { return Invoke-RestMethod -UseBasicParsing -Uri "$releases/tags/$tag" -Headers $headers }
    # The newest stable release, or the newest beta while none is published. The
    # latest-release download link cannot be used: it never serves a beta.
    $all = Invoke-RestMethod -UseBasicParsing -Uri ($releases + "?per_page=30") -Headers $headers
    $list = @($all | Where-Object { -not $_.draft })
    $stable = @($list | Where-Object { -not $_.prerelease })
    if ($stable.Count -gt 0) { return $stable[0] }
    if ($list.Count -gt 0) { return $list[0] }
    throw "No Snapmap+ release has been published yet."
}

# Download a release's snapmap-plus.exe and check it against the size and digest GitHub publishes.
function Save-Installer($release, [string]$out) {
    $asset = @($release.assets | Where-Object { $_.name -eq "snapmap-plus.exe" }) | Select-Object -First 1
    if (-not $asset) { throw "Release $($release.tag_name) has no snapmap-plus.exe yet; try again in a minute." }
    $part = "$out.download"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $part
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

$dest = Join-Path $env:LOCALAPPDATA "snapmap-plus"
New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $dest "snapmap-plus.exe"

if ($build -eq "2018") {
    # Run the final 2018 release's own installer, pinned by tag.
    Write-Host "Your DOOM is the 2018 build. Installing Snapmap+ $legacyTag, the final release for it."
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ("snapmap-plus-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        $legacy = Join-Path $work "snapmap-plus.exe"
        Save-Installer (Get-Release $legacyTag) $legacy
        & $legacy install --doom $doom --release $legacyTag
        if ($LASTEXITCODE -ne 0) { throw "the $legacyTag installer failed (exit $LASTEXITCODE)" }
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    # Keep the current installer for later commands: it identifies the build on every update,
    # so the 2018 install keeps receiving $legacyTag instead of a release for the 2024 build.
    Write-Host "Downloading the current snapmap-plus.exe for later updates ..."
    Save-Installer (Get-Release "") $exe
} else {
    Write-Host "Your DOOM is the April 2024 update (Steam build 13954591). Downloading snapmap-plus.exe ..."
    Save-Installer (Get-Release "") $exe
    Write-Host "Installing Snapmap+ ..."
    & $exe install --doom $doom
    if ($LASTEXITCODE -ne 0) { throw "snapmap-plus.exe install failed (exit $LASTEXITCODE); nothing more was changed." }
}

Write-Host ""
Write-Host "snapmap-plus.exe is at $exe"
Write-Host "Run it for: snapmap-plus update | snapmap-plus uninstall | snapmap-plus status"
