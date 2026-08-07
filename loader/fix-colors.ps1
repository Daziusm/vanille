# Run after closing colors.h / overlay.cpp in the editor if files are locked.
$ErrorActionPreference = "Stop"
$colorsDir = Join-Path $PSScriptRoot "..\Vanille\source\gui\colors"
$src = Join-Path $colorsDir "colors_new.h"
$dst = Join-Path $colorsDir "colors.h"
Copy-Item $src $dst -Force
Write-Host "Updated colors.h from colors_new.h"

$shim = Join-Path $PSScriptRoot "..\Vanille\source_include_shim\gui\colors\colors.h"
if (Test-Path (Split-Path $shim -Parent)) {
    Copy-Item $src $shim -Force
    Write-Host "Updated include shim colors.h"
}

Write-Host "Done. Rebuild with loader\build-chocola.ps1"
