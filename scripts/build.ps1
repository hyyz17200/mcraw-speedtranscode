param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$requiredVcpkgCommit = "cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3"
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT must point to a vcpkg checkout"
}

$preset = if ($Configuration -eq "Release") { "msvc-release" } else { "msvc-debug" }
$toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "vcpkg CMake toolchain not found: $toolchain"
}
$actualVcpkgCommit = (& git -C $env:VCPKG_ROOT rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualVcpkgCommit -ne $requiredVcpkgCommit) {
    throw "VCPKG_ROOT must be pinned to $requiredVcpkgCommit; found $actualVcpkgCommit"
}

cmake --preset $preset "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if (-not $SkipTests) {
    ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

$capabilityCheck = Join-Path $PSScriptRoot "check-vulkan-capability.ps1"
& $capabilityCheck -Binary (Join-Path $repoRoot "build\$preset\$Configuration\mcraw-transcoder.exe")
if ($LASTEXITCODE -ne 0) { throw "Vulkan capability check failed" }
