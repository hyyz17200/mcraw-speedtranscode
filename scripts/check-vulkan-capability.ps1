param(
    [string]$Binary = "$PSScriptRoot\..\build\msvc-release\Release\mcraw-transcoder.exe"
)

$ErrorActionPreference = "Stop"

$resolved = (Resolve-Path -LiteralPath $Binary).Path
$report = (& $resolved list-capabilities | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0) { throw "list-capabilities failed with exit code $LASTEXITCODE" }
if (-not $report.backends.vulkan.compiled) {
    throw "mcraw-transcoder was built without Vulkan support"
}
if (-not $report.backends.vulkan.encoder_available) {
    throw "the linked FFmpeg build has no prores_ks_vulkan encoder: $($report.backends.vulkan.reason)"
}

Write-Host "Vulkan capability check passed"
Write-Host "FFmpeg: $($report.ffmpeg.version)"
Write-Host "Encoder: $($report.backends.vulkan.encoder)"
