# SPDX-License-Identifier: MIT
# Builds, tests and packages the Windows release zip.
# Usage (from a PowerShell prompt, Visual Studio 2022 installed):
#   tools\package_windows.ps1 [-BuildDir build] [-SkipTests]
param(
    [string]$BuildDir = "build",
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# The shadercross/DXC build has deep paths; a short cache folder avoids MAX_PATH problems.
if (-not $env:EMERALD_CACHE_DIR) { $env:EMERALD_CACHE_DIR = "C:\emc" }

cmake -S . -B $BuildDir -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if (-not $SkipTests) {
    $env:SDL_VIDEODRIVER = "dummy"
    $env:SDL_AUDIODRIVER = "dummy"
    ctest --test-dir $BuildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

Push-Location $BuildDir
try {
    cpack -C Release -G ZIP
    if ($LASTEXITCODE -ne 0) { throw "cpack failed" }
    $zip = Get-ChildItem -Filter "snes_emulator-*.zip" | Sort-Object LastWriteTime | Select-Object -Last 1
    Write-Host ""
    Write-Host "Release zip: $($zip.FullName)"
} finally {
    Pop-Location
}
