param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$Notes = ""
)

$ErrorActionPreference = "Stop"
$loaderRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $loaderRoot
$distExe = Join-Path $loaderRoot "dist\Vanille.exe"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) is required. Install from https://cli.github.com/"
}

Push-Location $repoRoot
try {
    & (Join-Path $loaderRoot "build-chocola.ps1")
}
finally {
    Pop-Location
}

if (-not (Test-Path $distExe)) {
    throw "Build failed: $distExe not found"
}

$tag = if ($Version -match '^v') { $Version } else { "v$Version" }
$title = "Vanille Loader $tag"

if ([string]::IsNullOrWhiteSpace($Notes)) {
    $commit = git log -1 --format="%s%n%n%b"
    $Notes = @"
## Changes
$commit

## Install
1. Download **Vanille.exe** below
2. Run it — payload installs to ``%LOCALAPPDATA%\Chocola\``
3. Offsets refresh automatically on launch
"@
}

$existing = gh release view $tag 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Updating existing release $tag..."
    gh release upload $tag $distExe --clobber
    gh release edit $tag --title $title --notes $Notes
}
else {
    Write-Host "Creating release $tag..."
    gh release create $tag $distExe --title $title --notes $Notes
}

Write-Host "Release published: https://github.com/Daziusm/vanille/releases/tag/$tag"
