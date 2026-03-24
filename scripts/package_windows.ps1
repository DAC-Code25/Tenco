param(
    [string]$Preset = "release",
    [string]$BuildDir = "",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $PSScriptRoot "..\build\preset-$Preset"
}

Write-Host "[1/4] Configure preset: $Preset"
cmake --preset $Preset

Write-Host "[2/4] Build preset: $Preset"
cmake --build --preset $Preset --parallel

if (-not $SkipTests) {
    Write-Host "[3/4] Test preset: $Preset"
    ctest --preset $Preset --output-on-failure
} else {
    Write-Host "[3/4] Skip tests"
}

Write-Host "[4/4] Package with CPack"
Push-Location $BuildDir
try {
    cpack -G ZIP
} finally {
    Pop-Location
}

Write-Host "Package completed in $BuildDir"
