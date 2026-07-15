[CmdletBinding()]
param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-transcoder.exe",
    [string]$Corpus = ".\config\gpu-stage0-corpus.json",
    [string]$OutputDirectory = ".\test-output\gpu-stage0-benchmark",
    [int]$OfficialRuns = 3,
    [int]$PollMilliseconds = 500,
    [switch]$ValidateStage2Raw
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ($OfficialRuns -lt 3) { throw "Stage 0 requires at least three official runs" }
if ($PollMilliseconds -lt 100) { throw "PollMilliseconds must be at least 100" }

$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return 0.0 }
    $middle = [Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

$executablePath = Resolve-RepoPath $Executable
$contractPath = Resolve-RepoPath $Corpus
$outputPath = Resolve-RepoPath $OutputDirectory
$contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json -Depth 100
$sampleSpec = @($contract.real_samples)[0]
$samplePath = Resolve-RepoPath $sampleSpec.path
$configPath = Resolve-RepoPath $contract.production_config
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$moviePath = Join-Path $outputPath "stage0-vulkan.mov"
$benchmarkLabel = if ($ValidateStage2Raw) { "Stage 2" } else { "Stage 0" }
$logicalProcessors = [Environment]::ProcessorCount
$nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue

function Invoke-Conversion([string]$Kind, [int]$Index) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:executablePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @("convert", $script:samplePath, $script:moviePath,
                            "--config", $script:configPath, "--overwrite")) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $gpuMemoryBaselineMiB = 0.0
    if ($null -ne $script:nvidiaSmi) {
        $baselineRow = (& $script:nvidiaSmi.Source --query-gpu=memory.used `
            --format=csv,noheader,nounits 2>$null | Select-Object -First 1)
        if ($baselineRow) { $gpuMemoryBaselineMiB = [double]$baselineRow.Trim() }
    }
    if (-not $process.Start()) { throw "failed to start conversion" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $cpuSamples = @()
    $workingSetPeak = 0L
    $gpuUtilization = @()
    $gpuMemoryMiB = @()
    $previousCpuMs = 0.0
    $previousWall = [DateTime]::UtcNow
    while (-not $process.HasExited) {
        Start-Sleep -Milliseconds $script:PollMilliseconds
        try {
            $process.Refresh()
            $now = [DateTime]::UtcNow
            $cpuMs = $process.TotalProcessorTime.TotalMilliseconds
            $wallMs = ($now - $previousWall).TotalMilliseconds
            if ($wallMs -gt 0.0) {
                $cpuSamples += 100.0 * ($cpuMs - $previousCpuMs) /
                    ($wallMs * $script:logicalProcessors)
            }
            $previousCpuMs = $cpuMs
            $previousWall = $now
            $workingSetPeak = [Math]::Max($workingSetPeak, $process.WorkingSet64)
        } catch {
            # The process may exit between HasExited and Refresh.
        }
        if ($null -ne $script:nvidiaSmi) {
            $row = (& $script:nvidiaSmi.Source `
                --query-gpu=utilization.gpu,memory.used `
                --format=csv,noheader,nounits 2>$null | Select-Object -First 1)
            if ($row) {
                $values = $row -split ','
                if ($values.Count -ge 2) {
                    $gpuUtilization += [double]$values[0].Trim()
                    $gpuMemoryMiB += [double]$values[1].Trim()
                }
            }
        }
    }
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $stderrFile = Join-Path $script:outputPath ("{0}-{1:D2}.stderr.log" -f $Kind, $Index)
    Set-Content -LiteralPath $stderrFile -Value $stderr -Encoding utf8
    if ($process.ExitCode -ne 0) {
        throw "conversion failed with exit code $($process.ExitCode); see $stderrFile"
    }
    $result = $stdout | ConvertFrom-Json -Depth 100
    $commonInvalid = $result.pipeline.backend -ne "prores_ks_vulkan" -or
        -not $result.pipeline.gpu_resident -or
        -not $result.pipeline.gpu_timestamps_supported -or
        [int64]$result.pipeline.gpu_stages.rgb_to_yuv_422.samples -ne [int64]$result.frames -or
        [int64]$result.pipeline.transfers.compressed_input_upload_bytes -ne 0 -or
        [int64]$result.pipeline.transfers.fp16_rgb_upload_bytes -ne 0 -or
        [int64]$result.pipeline.transfers.compressed_packet_download_bytes -le 0
    $stage2Invalid = $false
    if ($script:ValidateStage2Raw) {
        $expectedRawBytes = [int64]$script:sampleSpec.width *
            [int64]$script:sampleSpec.height * 2L * [int64]$result.frames
        $stage2Invalid = $result.pipeline.entry -ne "raw_mosaic_u16" -or
            $result.pipeline.demosaic_location -ne "gpu_rcd_precise" -or
            [int64]$result.pipeline.transfers.u16_raw_upload_bytes -ne $expectedRawBytes -or
            [int64]$result.pipeline.transfers.fp32_rgb_upload_bytes -ne 0 -or
            [int64]$result.pipeline.transfers.camera_rgb_fp32_upload_bytes -ne 0 -or
            [int64]$result.pipeline.transfers.target_log_fp32_upload_bytes -ne 0 -or
            [int64]$result.pipeline.transfers.control_status_read_bytes -ne
                (4L * [int64]$result.frames) -or
            [int64]$result.pipeline.control_status_failures -ne 0 -or
            [int64]$result.pipeline.gpu_stages.raw_calibration.samples -ne
                [int64]$result.frames -or
            [int64]$result.pipeline.gpu_stages.rcd_demosaic.samples -ne
                [int64]$result.frames
    } else {
        $stage2Invalid = [int64]$result.pipeline.transfers.u16_raw_upload_bytes -ne 0 -or
            [int64]$result.pipeline.transfers.fp32_rgb_upload_bytes -ne
                [int64]$result.pipeline.rgb_upload_bytes
    }
    if ($commonInvalid -or $stage2Invalid) {
        throw "conversion did not satisfy the forced Vulkan/timestamp invariants"
    }
    $sidecarPath = "$script:moviePath.json"
    $savedSidecar = Join-Path $script:outputPath ("{0}-{1:D2}.sidecar.json" -f $Kind, $Index)
    Copy-Item -LiteralPath $sidecarPath -Destination $savedSidecar -Force
    return [ordered]@{
        kind = $Kind
        index = $Index
        result = $result
        process = [ordered]@{
            cpu_percent_mean = if ($cpuSamples.Count) {
                ($cpuSamples | Measure-Object -Average).Average
            } else { 0.0 }
            working_set_peak_bytes = $workingSetPeak
        }
        gpu_sampler = [ordered]@{
            provider = if ($null -ne $script:nvidiaSmi) { "nvidia-smi" } else { "unavailable" }
            scope = "device-global; run on an otherwise idle GPU"
            samples = $gpuUtilization.Count
            device_utilization_percent_mean = if ($gpuUtilization.Count) {
                ($gpuUtilization | Measure-Object -Average).Average
            } else { 0.0 }
            device_utilization_percent_peak = if ($gpuUtilization.Count) {
                ($gpuUtilization | Measure-Object -Maximum).Maximum
            } else { 0.0 }
            device_vram_used_baseline_mib = $gpuMemoryBaselineMiB
            device_vram_used_peak_mib = if ($gpuMemoryMiB.Count) {
                ($gpuMemoryMiB | Measure-Object -Maximum).Maximum
            } else { 0.0 }
            device_vram_used_peak_delta_mib = if ($gpuMemoryMiB.Count) {
                [Math]::Max(0.0,
                    ($gpuMemoryMiB | Measure-Object -Maximum).Maximum -
                    $gpuMemoryBaselineMiB)
            } else { 0.0 }
        }
        sidecar = [System.IO.Path]::GetFileName($savedSidecar)
        stderr = [System.IO.Path]::GetFileName($stderrFile)
    }
}

$runs = @()
Write-Host "$benchmarkLabel warm-up run"
$runs += Invoke-Conversion "warmup" 0
for ($run = 1; $run -le $OfficialRuns; ++$run) {
    Write-Host "$benchmarkLabel official run $run/$OfficialRuns"
    $runs += Invoke-Conversion "official" $run
}

$official = @($runs | Where-Object { $_.kind -eq "official" })
$fps = [double[]]@($official | ForEach-Object { $_.result.throughput_fps })
$wall = [double[]]@($official | ForEach-Object { $_.result.wall_ms })
$gpuMean = [double[]]@($official | ForEach-Object {
    $_.result.pipeline.gpu_stages.rgb_to_yuv_422.mean_ms
})
$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
$ffprobeReport = $null
if ($null -ne $ffprobe) {
    $ffprobeText = (& $ffprobe.Source -v error -show_streams -show_format `
        -show_packets -of json $moviePath | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for the benchmark output" }
    $ffprobeReport = $ffprobeText | ConvertFrom-Json -Depth 100
}

$report = [ordered]@{
    schema = if ($ValidateStage2Raw) {
        "mcraw-gpu-stage2-benchmark-v1"
    } else {
        "mcraw-gpu-stage0-benchmark-v1"
    }
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    commit = (& git -C $repoRoot rev-parse HEAD).Trim()
    dirty = @(& git -C $repoRoot status --porcelain=v1).Count -ne 0
    contract_sha256 = (Get-FileHash -LiteralPath $contractPath -Algorithm SHA256).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
    config_sha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    input_sha256 = (Get-FileHash -LiteralPath $samplePath -Algorithm SHA256).Hash
    runs = $runs
    summary = [ordered]@{
        official_runs = $official.Count
        throughput_fps = [ordered]@{
            median = Get-Median $fps
            min = ($fps | Measure-Object -Minimum).Minimum
            max = ($fps | Measure-Object -Maximum).Maximum
        }
        wall_ms = [ordered]@{
            median = Get-Median $wall
            min = ($wall | Measure-Object -Minimum).Minimum
            max = ($wall | Measure-Object -Maximum).Maximum
        }
        rgb_to_yuv_gpu_ms = [ordered]@{
            median_of_run_means = Get-Median $gpuMean
            min_run_mean = ($gpuMean | Measure-Object -Minimum).Minimum
            max_run_mean = ($gpuMean | Measure-Object -Maximum).Maximum
        }
    }
    output = [ordered]@{
        file = [System.IO.Path]::GetFileName($moviePath)
        bytes = (Get-Item -LiteralPath $moviePath).Length
        sha256 = (Get-FileHash -LiteralPath $moviePath -Algorithm SHA256).Hash
        ffprobe = $ffprobeReport
    }
}

$reportFile = Join-Path $outputPath "benchmark-report.json"
$report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $reportFile -Encoding utf8
Write-Host "$benchmarkLabel benchmark report: $reportFile"
