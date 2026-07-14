param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT must point to a vcpkg checkout"
}

$preset = if ($Configuration -eq "Release") { "msvc-release" } else { "msvc-debug" }
$toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "vcpkg CMake toolchain not found: $toolchain"
}

cmake --preset $preset "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if (-not $SkipTests) {
    ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

