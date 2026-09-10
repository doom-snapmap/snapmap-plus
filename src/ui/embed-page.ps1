# Assemble the same local assets used by browser preview for NavigateToString.
param(
    [string]$PageDirectory = (Join-Path $PSScriptRoot 'webview'),
    [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
function Read-PageAsset([string]$Name) {
    $path = Join-Path $PageDirectory $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required interface asset missing: $Name" }
    $text = [IO.File]::ReadAllText($path)
    if ($text -match '[^\x00-\x7F]') { throw "Interface asset must use ASCII or escapes: $Name" }
    if ($text.Contains(')SNAPMAPPLUS')) { throw "Reserved raw-literal delimiter in $Name" }
    return $text
}
function Replace-Asset([string]$Page, [string]$Tag, [string]$Replacement) {
    if ([regex]::Matches($Page, [regex]::Escape($Tag)).Count -ne 1) {
        throw "Expected exactly one interface asset tag: $Tag"
    }
    return $Page.Replace($Tag, $Replacement)
}
$html = Read-PageAsset 'mockup.html'
$css = Read-PageAsset 'studio.css'
if ($css -match '</style') { throw 'studio.css contains a closing style tag' }
$html = Replace-Asset $html '<link rel="stylesheet" href="studio.css">' "<style>`n$css</style>"
foreach ($name in @('schema_slice.js', 'decl_language.js', 'prefab_transform.js', 'prefab_viewport.js')) {
    $script = Read-PageAsset $name
    if ($script -match '</script') { throw "$name contains a closing script tag" }
    $tag = '<script src="' + $name + '"></script>'
    $html = Replace-Asset $html $tag "<script>`n$script</script>"
}
if ($html -match '<script\b[^>]*\bsrc\s*=' -or $html -match '<link\b[^>]*\brel=["'']stylesheet') {
    throw 'Unbundled script or stylesheet in embedded interface'
}
if ($Output) { [IO.File]::WriteAllText($Output, $html, [Text.Encoding]::ASCII) }
else { $html }
