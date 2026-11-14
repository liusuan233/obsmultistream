<#
.SYNOPSIS
    Build the obs-multistream plugin for Windows x64 and produce a drop-in zip.

.DESCRIPTION
    Runs the exact chain described in README.md:
      configure (windows-x64 preset) -> build -> install -> zip.
    Must be run on Windows with Visual Studio 2022 + CMake >= 3.28 + the
    Windows 10 SDK. Uses MSVC so the resulting DLL is ABI-compatible with
    OBS's own Qt6.

.EXAMPLE
    pwsh -File .\build-windows.ps1
    pwsh -File .\build-windows.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Release', 'Debug', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = $PSScriptRoot
Set-Location $ProjectRoot

if (-not (Test-Path (Join-Path $ProjectRoot 'buildspec.json'))) {
    throw "buildspec.json not found; run this script from the project root."
}

$spec    = Get-Content buildspec.json -Raw | ConvertFrom-Json
$name    = $spec.name
$version = $spec.version
$outName = "$name-$version-windows-x64"

Write-Host "==> Configure (windows-x64)" -ForegroundColor Cyan
cmake --preset windows-x64
if ($LASTEXITCODE) { throw "configure failed" }

Write-Host "==> Build ($Configuration)" -ForegroundColor Cyan
cmake --build --preset windows-x64 --config $Configuration --parallel
if ($LASTEXITCODE) { throw "build failed" }

$stage = "release/$Configuration"
Write-Host "==> Install -> $stage" -ForegroundColor Cyan
cmake --install build_x64 --prefix $stage --config $Configuration
if ($LASTEXITCODE) { throw "install failed" }

$dll = Join-Path $stage "$name/bin/64bit/$name.dll"
if (-not (Test-Path $dll)) { throw "expected DLL not found: $dll" }

$zip = "release/$outName.zip"
Write-Host "==> Package -> $zip" -ForegroundColor Cyan
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Force -CompressionLevel Optimal `
    -Path (Get-ChildItem -Path $stage -Exclude "$outName*.*") `
    -DestinationPath $zip

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  DLL : $dll"
Write-Host "  ZIP : $zip"
Write-Host "Install: unzip $zip into %APPDATA%\obs-studio\plugins\ then restart OBS."
