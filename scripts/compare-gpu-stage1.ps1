[CmdletBinding()]
param(
    [string]$Stage0Report = ".\test-output\gpu-stage1f-stage0-rebuilt\benchmark-report.json",
    [string]$Stage1Report = ".\test-output\gpu-stage1f-stage1\benchmark-report.json",
    [string]$Stage0Manifest = ".\test-output\gpu-stage0-baseline\baseline-manifest.json",
    [string]$Output = ".\test-output\gpu-stage1f-comparison.json",
    [double]$RequiredImprovementPercent = 20.0,
    [string]$Stage0SourceCommit = "622070c8a62200aa828efcd7b95b57f9189a1519"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-PathFromRepo([string]$Path) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Read-Json([string]$Path) {
    $resolved = Resolve-PathFromRepo $Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "required report not found: $resolved"
    }
    return Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json -Depth 100
}

function Assert-CommonRun($Run, [string]$Label) {
    $result = $Run.result
    if ($result.frames -ne 240 -or
        $result.pipeline.backend -ne "prores_ks_vulkan" -or
        -not $result.pipeline.gpu_resident -or
        $result.pipeline.direct_frames -ne 240 -or
        $result.pipeline.upload_frames -ne 0 -or
        $result.pipeline.readback_frames -ne 0 -or
        $result.pipeline.video_packets -ne 240 -or
        -not $result.pipeline.gpu_timestamps_supported -or
        $result.pipeline.gpu_stages.rgb_to_yuv_422.samples -ne 240) {
        throw "$Label run $($Run.index) violates the common Vulkan benchmark contract"
    }
}

$stage0 = Read-Json $Stage0Report
$stage1 = Read-Json $Stage1Report
$manifest = Read-Json $Stage0Manifest
$stage0Official = @($stage0.runs | Where-Object { $_.kind -eq "official" })
$stage1Official = @($stage1.runs | Where-Object { $_.kind -eq "official" })
if ($stage0Official.Count -lt 3 -or $stage1Official.Count -lt 3) {
    throw "both candidates require at least three official runs"
}
if ($stage0.config_sha256 -ne $stage1.config_sha256 -or
    $stage0.input_sha256 -ne $stage1.input_sha256) {
    throw "Stage 0 and Stage 1 config/input hashes do not match"
}

foreach ($run in $stage0Official) { Assert-CommonRun $run "Stage 0" }
foreach ($run in $stage1Official) {
    Assert-CommonRun $run "Stage 1"
    $pipeline = $run.result.pipeline
    if ($pipeline.entry -ne "camera_rgb_f32" -or
        $pipeline.precision -ne "fp32/precise" -or
        $pipeline.transfers.camera_rgb_fp32_upload_bytes -ne
            $pipeline.rgb_upload_bytes -or
        $pipeline.transfers.target_log_fp32_upload_bytes -ne 0 -or
        $pipeline.transfers.control_status_read_bytes -ne 960 -or
        $pipeline.control_status_failures -ne 0 -or
        $pipeline.gpu_stages.camera_to_dwg.samples -ne 240 -or
        $pipeline.gpu_stages.capture_sharpening.samples -ne 240 -or
        $pipeline.gpu_stages.davinci_intermediate.samples -ne 240) {
        throw "Stage 1 run $($run.index) violates the resident Camera RGB contract"
    }
}

$stage0Median = [double]$stage0.summary.throughput_fps.median
$stage1Median = [double]$stage1.summary.throughput_fps.median
$improvement = 100.0 * ($stage1Median / $stage0Median - 1.0)
$accepted = $improvement -ge $RequiredImprovementPercent
$outputPath = Resolve-PathFromRepo $Output
$outputParent = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null

$comparison = [ordered]@{
    schema = "mcraw-gpu-stage1f-comparison-v1"
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    matched = [ordered]@{
        config_sha256 = $stage0.config_sha256
        input_sha256 = $stage0.input_sha256
        official_runs_per_candidate = [Math]::Min($stage0Official.Count,
                                                   $stage1Official.Count)
        warmup_runs_per_candidate = 1
    }
    stage0 = [ordered]@{
        source_commit = $Stage0SourceCommit
        executable_sha256 = $stage0.executable_sha256
        frozen_manifest_executable_sha256 = $manifest.build.executable_sha256
        executable_matches_frozen_manifest =
            $stage0.executable_sha256 -eq $manifest.build.executable_sha256
        report = (Resolve-PathFromRepo $Stage0Report)
        throughput_fps = $stage0.summary.throughput_fps
        wall_ms = $stage0.summary.wall_ms
        rgb_to_yuv_gpu_ms = $stage0.summary.rgb_to_yuv_gpu_ms
    }
    stage1 = [ordered]@{
        source_commit = (& git -C (Split-Path -Parent $PSScriptRoot) rev-parse HEAD).Trim()
        executable_sha256 = $stage1.executable_sha256
        report = (Resolve-PathFromRepo $Stage1Report)
        throughput_fps = $stage1.summary.throughput_fps
        wall_ms = $stage1.summary.wall_ms
        rgb_to_yuv_gpu_ms = $stage1.summary.rgb_to_yuv_gpu_ms
    }
    decision = [ordered]@{
        required_improvement_percent = $RequiredImprovementPercent
        measured_improvement_percent = $improvement
        accepted = $accepted
        result = if ($accepted) { "go" } else { "no-go" }
    }
}

$comparison | ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath $outputPath -Encoding utf8
Write-Host "Stage 1F comparison: $outputPath"
Write-Host ("Decision: {0}; measured improvement {1:N3}% (required {2:N1}%)" -f
    $comparison.decision.result, $improvement, $RequiredImprovementPercent)
