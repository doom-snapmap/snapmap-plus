# Deliberately refresh the committed organization avatar; normal builds never do this.
# -ImagePath uses an already downloaded PNG/JPEG for offline maintenance or testing.
param(
    [string]$ImagePath = '',
    [string]$PagePath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'src\ui\webview\mockup.html')
)
$ErrorActionPreference = 'Stop'
$temporary = $null
try {
    if (-not $ImagePath) {
        $temporary = [IO.Path]::GetTempFileName()
        & curl.exe --fail --silent --show-error --location --max-time 8 --max-filesize 65535 `
            --output $temporary 'https://github.com/doom-snapmap.png?size=64'
        if ($LASTEXITCODE -ne 0) { throw 'Organization avatar download failed' }
        $ImagePath = $temporary
    }
    $item = Get-Item -LiteralPath $ImagePath
    if ($item.Length -lt 4 -or $item.Length -ge 65536) { throw 'Avatar must be a PNG or JPEG smaller than 64 KiB' }
    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    $mime = $null
    if ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xD8 -and $bytes[2] -eq 0xFF) { $mime = 'image/jpeg' }
    elseif ($bytes.Length -ge 8 -and [BitConverter]::ToString($bytes, 0, 8) -eq '89-50-4E-47-0D-0A-1A-0A') { $mime = 'image/png' }
    if (-not $mime) { throw 'Avatar does not have a PNG or JPEG signature' }
    $html = [IO.File]::ReadAllText($PagePath)
    $avatarMatches = [regex]::Matches($html, 'data:image/(?:jpeg|png);base64,[A-Za-z0-9+/=]+')
    if ($avatarMatches.Count -ne 1) { throw "Expected exactly one embedded avatar, found $($avatarMatches.Count)" }
    $uri = "data:$mime;base64," + [Convert]::ToBase64String($bytes)
    if ($uri -eq $avatarMatches[0].Value) { Write-Host 'Committed avatar is already current'; return }
    $html = $html.Substring(0, $avatarMatches[0].Index) + $uri + $html.Substring($avatarMatches[0].Index + $avatarMatches[0].Length)
    [IO.File]::WriteAllText($PagePath, $html, (New-Object Text.UTF8Encoding $false))
    Write-Host 'Avatar updated; review and commit the change to mockup.html'
} finally {
    if ($temporary -and [IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
