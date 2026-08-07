$ErrorActionPreference = "Stop"
$loaderRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vanilleRoot = Join-Path (Split-Path -Parent $loaderRoot) "Vanille"
$distDir = Join-Path $loaderRoot "dist"

& (Join-Path $loaderRoot "generate-icon.ps1")

$splashSrc = Join-Path $vanilleRoot "assets\loader_icon.png"
$splashDst = Join-Path $loaderRoot "ChocolaLoader\Resources\splash_sprite.png"
if (Test-Path $splashSrc) {
    Copy-Item $splashSrc $splashDst -Force
}

$msbuild = @(
  "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
  "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $msbuild) {
  $msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}
if (-not $msbuild) { throw "MSBuild not found" }

Write-Host "Building Vanille client..."
& $msbuild "$vanilleRoot\vanille.sln" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:m
if (-not (Test-Path "$vanilleRoot\build\vanille.exe")) {
    throw "Vanille client build failed: $vanilleRoot\build\vanille.exe missing"
}

Write-Host "Staging Vanille payload..."
Push-Location $loaderRoot
try {
    if (-not (Test-Path "build\x64-release")) {
        cmake --preset x64-release | Out-Null
    }
    cmake --build --preset x64-release --config Release --clean-first | Out-Null
    Copy-Item "build\x64-release\payload.zip" "ChocolaLoader\payload.zip" -Force
}
finally {
    Pop-Location
}

Write-Host "Building Vanille loader..."
& $msbuild "$loaderRoot\ChocolaLoader\Chocola.csproj" /p:Configuration=Release /v:m
$out = Join-Path $loaderRoot "ChocolaLoader\bin\Release\Vanille.exe"
if (Test-Path $out) {
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    Copy-Item $out (Join-Path $distDir "Vanille.exe") -Force

    $resourcesSrc = Join-Path $loaderRoot "ChocolaLoader\bin\Release\Resources"
    $resourcesDst = Join-Path $distDir "Resources"
    if (Test-Path $resourcesSrc) {
        Copy-Item $resourcesSrc $resourcesDst -Recurse -Force
    }

    Get-ChildItem $distDir -Filter "*.exe" |
        Where-Object { $_.Name -ne "Vanille.exe" } |
        Remove-Item -Force

    Write-Host "Built: $out"
    Write-Host "Dist:  $(Join-Path $distDir 'Vanille.exe')"
} else {
    throw "Build failed"
}
