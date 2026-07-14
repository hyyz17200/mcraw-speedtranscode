param(
    [string]$Tag = "2026.06.24",
    [string]$Destination = ".deps\vcpkg"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Destination)) {
    $Destination = Join-Path $repoRoot $Destination
}
$Destination = [IO.Path]::GetFullPath($Destination)
$bootstrap = Join-Path $Destination "bootstrap-vcpkg.bat"

if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
    if (Test-Path -LiteralPath $Destination) {
        $existing = Get-ChildItem -LiteralPath $Destination -Force -ErrorAction Stop
        if ($existing.Count -gt 0) {
            throw "Destination exists but is not a vcpkg checkout: $Destination"
        }
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $Destination) | Out-Null
    & git clone --branch $Tag --depth 1 https://github.com/microsoft/vcpkg.git $Destination
    if ($LASTEXITCODE -ne 0) { throw "git clone of vcpkg failed" }
}

& $bootstrap -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }

$env:VCPKG_ROOT = $Destination
Write-Host "VCPKG_ROOT=$env:VCPKG_ROOT"
Write-Host "vcpkg is ready; run cmake --preset msvc-release with the vcpkg toolchain."
