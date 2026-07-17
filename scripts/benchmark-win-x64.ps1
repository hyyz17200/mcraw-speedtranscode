#Requires -Version 7.0

<#
.SYNOPSIS
Runs the Windows x64 component and end-to-end MCRAW benchmark matrix.

.DESCRIPTION
Measures CPU MCRAW decompression, the FFmpeg prores_ks_vulkan encoder chain,
fast Vulkan U16 RAW stage service rate, and full MCRAW-to-MOV throughput.
Async depths 1 and 2 are tested. Every case gets one warm-up plus OfficialRuns
measured runs. The output directory receives JSON, CSV, text, and raw logs.

.EXAMPLE
pwsh -NoProfile -File .\scripts\benchmark-win-x64.ps1

.EXAMPLE
pwsh -NoProfile -File .\scripts\benchmark-win-x64.ps1 -Ffmpeg C:\bench\ffmpeg.exe -GpuSelector 0 -FfmpegGpuIndex 0
#>
[CmdletBinding()]
param(
    [string]$Transcoder = ".\build\msvc-release\Release\mcraw-speedtranscode.exe",
    [string]$DecoderBenchmark = ".\build\msvc-release\Release\mcraw-decoder-benchmark.exe",
    [string]$Ffmpeg = "ffmpeg.exe",
    [Alias("Input")]
    [string]$InputFile = ".\mcraw_sample\260710_142121_VIDEO_49mm.mcraw",
    [string]$FastConfig = ".\config\vulkan-fast.json",
    [string]$OutputDirectory = "",
    [ValidateRange(1, 20)]
    [int]$OfficialRuns = 3,
    [ValidateRange(0, 1000000)]
    [int]$Frames = 0,
    [ValidateRange(1, 1000000)]
    [int]$EncoderFrames = 300,
    [ValidatePattern('^\d+(,\d+)*$')]
    [string]$DecoderWorkers = "1,2,4,6,8",
    [ValidateRange(0, 64)]
    [int]$FfmpegGpuIndex = 0,
    [string]$GpuSelector = "",
    [switch]$KeepMovies
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw "PowerShell 7 or newer is required. Run this script with pwsh.exe."
}
if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
    throw "This benchmark requires 64-bit Windows and a 64-bit PowerShell process."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Resolve-BenchmarkPath([string]$Path, [switch]$AllowCommand) {
    if ($AllowCommand -and -not [IO.Path]::IsPathRooted($Path) -and
        -not $Path.Contains([IO.Path]::DirectorySeparatorChar) -and
        -not $Path.Contains([IO.Path]::AltDirectorySeparatorChar)) {
        $command = Get-Command $Path -ErrorAction SilentlyContinue
        if ($null -ne $command) { return $command.Source }
    }
    $candidate = if ([IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path $repoRoot $Path
    }
    return [IO.Path]::GetFullPath($candidate)
}

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2.0)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Convert-ToDouble([object]$Value) {
    return [double]::Parse([string]$Value, $invariant)
}

function Write-JsonFile([object]$Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [Parameter(Mandatory)][string]$StdoutPath,
        [Parameter(Mandatory)][string]$StderrPath,
        [switch]$AllowFailure
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $ArgumentList) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw "Failed to start: $FilePath" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stopwatch.Stop()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Set-Content -LiteralPath $StdoutPath -Value $stdout -Encoding utf8
    Set-Content -LiteralPath $StderrPath -Value $stderr -Encoding utf8

    if (-not $AllowFailure -and $process.ExitCode -ne 0) {
        if ($process.ExitCode -eq -1073741790) {
            throw "Process failed with NTSTATUS 0xC0000022 (STATUS_ACCESS_DENIED): $FilePath. " +
                "Check Zone.Identifier/Unblock-File, NTFS ACLs, AppLocker/WDAC/Defender policy, " +
                "and whether the EXE and all Release DLLs came from the same package. See $StderrPath"
        }
        throw "Process failed with exit code $($process.ExitCode): $FilePath. See $StderrPath"
    }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
        ProcessWallSeconds = $stopwatch.Elapsed.TotalSeconds
    }
}

function Assert-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
}

$transcoderPath = Resolve-BenchmarkPath $Transcoder
$decoderPath = Resolve-BenchmarkPath $DecoderBenchmark
$ffmpegPath = Resolve-BenchmarkPath $Ffmpeg -AllowCommand
$inputPath = Resolve-BenchmarkPath $InputFile
$fastConfigPath = Resolve-BenchmarkPath $FastConfig
Assert-File $transcoderPath "Transcoder"
Assert-File $decoderPath "Decoder benchmark"
Assert-File $ffmpegPath "FFmpeg"
Assert-File $inputPath "MCRAW input"
Assert-File $fastConfigPath "Fast config"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path $repoRoot "test-output\win-x64-benchmark-$stamp"
} elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$rawPath = Join-Path $outputPath "raw"
$configPath = Join-Path $outputPath "configs"
$e2ePath = Join-Path $rawPath "e2e"
$encoderPath = Join-Path $rawPath "prores-vulkan"
New-Item -ItemType Directory -Path $rawPath, $configPath, $e2ePath, $encoderPath -Force |
    Out-Null

$failurePath = Join-Path $outputPath "FAILED.txt"
try {
    Write-Host "Output directory: $outputPath"
    Write-Host "[1/6] Capturing hardware, binary, input, and capability identity"

    $cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores,
        NumberOfLogicalProcessors, MaxClockSpeed
    $gpu = Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion,
        AdapterRAM
    $os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version,
        BuildNumber, TotalVisibleMemorySize, FreePhysicalMemory
    $system = [ordered]@{
        captured_at_utc = [DateTime]::UtcNow.ToString("o")
        machine_name = $env:COMPUTERNAME
        os = $os
        cpu = $cpu
        gpu = $gpu
        powershell = $PSVersionTable.PSVersion.ToString()
        files = [ordered]@{
            transcoder = [ordered]@{
                path = $transcoderPath
                sha256 = (Get-FileHash -LiteralPath $transcoderPath -Algorithm SHA256).Hash
            }
            decoder_benchmark = [ordered]@{
                path = $decoderPath
                sha256 = (Get-FileHash -LiteralPath $decoderPath -Algorithm SHA256).Hash
            }
            ffmpeg = [ordered]@{
                path = $ffmpegPath
                sha256 = (Get-FileHash -LiteralPath $ffmpegPath -Algorithm SHA256).Hash
            }
            input = [ordered]@{
                path = $inputPath
                bytes = (Get-Item -LiteralPath $inputPath).Length
                sha256 = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
            }
            fast_config = [ordered]@{
                path = $fastConfigPath
                sha256 = (Get-FileHash -LiteralPath $fastConfigPath -Algorithm SHA256).Hash
            }
        }
    }
    Write-JsonFile $system (Join-Path $outputPath "system.json")

    $capabilityRun = Invoke-CapturedProcess -FilePath $transcoderPath `
        -ArgumentList @("list-capabilities") `
        -StdoutPath (Join-Path $rawPath "capabilities.json") `
        -StderrPath (Join-Path $rawPath "capabilities.stderr.log")
    $capabilities = $capabilityRun.Stdout | ConvertFrom-Json -Depth 100
    if (-not $capabilities.backends.vulkan.compiled -or
        -not $capabilities.backends.vulkan.encoder_available -or
        -not $capabilities.backends.vulkan.runtime.available) {
        throw "The project binary does not report a usable Vulkan ProRes backend."
    }

    $inspectRun = Invoke-CapturedProcess -FilePath $transcoderPath `
        -ArgumentList @("inspect", $inputPath, "--raw-json") `
        -StdoutPath (Join-Path $rawPath "input-inspect.json") `
        -StderrPath (Join-Path $rawPath "input-inspect.stderr.log")
    $inspect = $inspectRun.Stdout | ConvertFrom-Json -Depth 100
    $selectedFrames = if ($Frames -eq 0) {
        [int]$inspect.frame_count
    } else {
        [Math]::Min($Frames, [int]$inspect.frame_count)
    }
    if ($selectedFrames -lt 1) { throw "The benchmark selected zero frames." }
    $width = [int]$inspect.first_frame.width
    $height = [int]$inspect.first_frame.height

    $ffmpegVersionRun = Invoke-CapturedProcess -FilePath $ffmpegPath `
        -ArgumentList @("-version") `
        -StdoutPath (Join-Path $rawPath "ffmpeg-version.txt") `
        -StderrPath (Join-Path $rawPath "ffmpeg-version.stderr.log")
    $ffmpegEncoderProbe = Invoke-CapturedProcess -FilePath $ffmpegPath `
        -ArgumentList @("-hide_banner", "-h", "encoder=prores_ks_vulkan") `
        -StdoutPath (Join-Path $rawPath "ffmpeg-prores-vulkan.txt") `
        -StderrPath (Join-Path $rawPath "ffmpeg-prores-vulkan.stderr.log")
    $encoderProbeText = $ffmpegEncoderProbe.Stdout + $ffmpegEncoderProbe.Stderr
    if ($encoderProbeText -notmatch 'Encoder prores_ks_vulkan' -or
        $encoderProbeText -notmatch '-async_depth') {
        throw "FFmpeg does not provide prores_ks_vulkan with async_depth support: $ffmpegPath"
    }

    $baseConfig = Get-Content -LiteralPath $fastConfigPath -Raw |
        ConvertFrom-Json -Depth 100
    if ($baseConfig.gpu_performance_mode -ne "fast") {
        throw "FastConfig must set gpu_performance_mode to fast."
    }
    foreach ($depth in @(1, 2)) {
        $config = $baseConfig.PSObject.Copy()
        $config.backend = "vulkan"
        $config.fallback = "none"
        $config.gpu_performance_mode = "fast"
        $config.async_depth = $depth
        if (-not [string]::IsNullOrWhiteSpace($GpuSelector)) {
            $config.gpu_selector = $GpuSelector
        }
        Write-JsonFile $config (Join-Path $configPath "vulkan-fast-depth-$depth.json")
    }

    Write-Host "[2/6] Benchmarking CPU MCRAW decompression only"
    $decoderCsvPath = Join-Path $rawPath "cpu-decoder.csv"
    $decoderRun = Invoke-CapturedProcess -FilePath $decoderPath `
        -ArgumentList @($inputPath, "decoder-only", $DecoderWorkers, [string]$OfficialRuns) `
        -StdoutPath $decoderCsvPath `
        -StderrPath (Join-Path $rawPath "cpu-decoder.stderr.log")
    $decoderRows = @($decoderRun.Stdout | ConvertFrom-Csv)
    if ($decoderRows.Count -eq 0) { throw "CPU decoder benchmark returned no rows." }
    $decoderSummaries = @()
    foreach ($group in ($decoderRows | Group-Object workers)) {
        $fpsValues = [double[]]@($group.Group | ForEach-Object { Convert-ToDouble $_.fps })
        $decoderSummaries += [pscustomobject]@{
            workers = [int]$group.Name
            median_fps = Get-Median $fpsValues
            min_fps = ($fpsValues | Measure-Object -Minimum).Minimum
            max_fps = ($fpsValues | Measure-Object -Maximum).Maximum
        }
    }
    $bestDecoder = $decoderSummaries | Sort-Object median_fps -Descending |
        Select-Object -First 1

    function Invoke-ProresEncoderRun([int]$Depth, [string]$Kind, [int]$Index) {
        $prefix = Join-Path $encoderPath ("depth-{0}-{1}-{2:D2}" -f $Depth, $Kind, $Index)
        $arguments = @(
            "-nostdin", "-hide_banner", "-loglevel", "info", "-benchmark",
            "-init_hw_device", "vulkan=gpu:$FfmpegGpuIndex",
            "-filter_hw_device", "gpu",
            "-f", "lavfi", "-i", "color=c=gray:s=${width}x${height}:r=30",
            "-vf", "format=yuv422p10le,hwupload",
            "-frames:v", [string]$EncoderFrames,
            "-an", "-c:v", "prores_ks_vulkan", "-profile:v", "3",
            "-alpha_bits", "0", "-async_depth", [string]$Depth,
            "-f", "null", "NUL"
        )
        $run = Invoke-CapturedProcess -FilePath $ffmpegPath -ArgumentList $arguments `
            -StdoutPath "$prefix.stdout.log" -StderrPath "$prefix.stderr.log"
        $text = $run.Stdout + "`n" + $run.Stderr
        $matches = [regex]::Matches($text, 'bench:\s+utime=[0-9.]+s\s+stime=[0-9.]+s\s+rtime=([0-9.]+)s')
        if ($matches.Count -eq 0) {
            throw "Could not parse FFmpeg benchmark rtime. See $prefix.stderr.log"
        }
        $rtime = Convert-ToDouble $matches[$matches.Count - 1].Groups[1].Value
        if ($rtime -le 0.0) { throw "FFmpeg reported a non-positive benchmark time." }
        return [pscustomobject]@{
            kind = $Kind
            index = $Index
            async_depth = $Depth
            frames = $EncoderFrames
            wall_seconds = $rtime
            throughput_fps = $EncoderFrames / $rtime
            process_wall_seconds = $run.ProcessWallSeconds
        }
    }

    Write-Host "[3/6] Benchmarking prores_ks_vulkan encoder chain at async depth 1 and 2"
    $encoderRuns = @()
    foreach ($depth in @(1, 2)) {
        Write-Host "  Encoder depth $depth warm-up"
        $encoderRuns += Invoke-ProresEncoderRun $depth "warmup" 0
        for ($runIndex = 1; $runIndex -le $OfficialRuns; ++$runIndex) {
            Write-Host "  Encoder depth $depth official $runIndex/$OfficialRuns"
            $encoderRuns += Invoke-ProresEncoderRun $depth "official" $runIndex
        }
    }

    $gpuStageNames = @(
        "raw_calibration", "rcd_demosaic", "camera_to_dwg",
        "capture_sharpening", "davinci_intermediate", "rgb_to_yuv_422"
    )

    function Invoke-E2eRun([int]$Depth, [string]$Kind, [int]$Index) {
        $prefix = Join-Path $e2ePath ("depth-{0}-{1}-{2:D2}" -f $Depth, $Kind, $Index)
        $moviePath = Join-Path $e2ePath "depth-$Depth-current.mov"
        $run = Invoke-CapturedProcess -FilePath $transcoderPath `
            -ArgumentList @(
                "convert", $inputPath, $moviePath,
                "--config", (Join-Path $configPath "vulkan-fast-depth-$Depth.json"),
                "--frames", [string]$selectedFrames, "--overwrite"
            ) `
            -StdoutPath "$prefix.json" -StderrPath "$prefix.stderr.log"
        $result = $run.Stdout | ConvertFrom-Json -Depth 100
        if (-not $result.ok -or $result.pipeline.backend -ne "prores_ks_vulkan" -or
            $result.pipeline.used_fallback -or -not $result.pipeline.gpu_resident -or
            -not $result.pipeline.gpu_timestamps_supported -or
            $result.pipeline.performance_mode -ne "fast" -or
            [int]$result.pipeline.async_depth -ne $Depth -or
            [int]$result.pipeline.video_packets -ne $selectedFrames) {
            throw "Depth $Depth $Kind run did not satisfy the forced fast Vulkan invariants."
        }

        $sidecarPath = "$moviePath.json"
        Assert-File $sidecarPath "Conversion sidecar"
        $sidecar = Get-Content -LiteralPath $sidecarPath -Raw | ConvertFrom-Json -Depth 100
        if ([int]$sidecar.pipeline.effective_async_depth -ne $Depth) {
            throw "Requested async depth $Depth but effective depth was $($sidecar.pipeline.effective_async_depth)."
        }
        Copy-Item -LiteralPath $sidecarPath -Destination "$prefix.sidecar.json" -Force

        $stageTotalMs = 0.0
        $stageMeans = [ordered]@{}
        foreach ($stageName in $gpuStageNames) {
            $stage = $result.pipeline.gpu_stages.$stageName
            if ([int64]$stage.samples -ne $selectedFrames) {
                throw "GPU stage $stageName has $($stage.samples) samples; expected $selectedFrames."
            }
            $stageTotalMs += [double]$stage.total_ms
            $stageMeans[$stageName] = [double]$stage.mean_ms
        }
        $rawServiceFps = [double]$selectedFrames * 1000.0 / $stageTotalMs

        $movieBytes = (Get-Item -LiteralPath $moviePath).Length
        if (-not $KeepMovies) {
            Remove-Item -LiteralPath $moviePath -Force
            Remove-Item -LiteralPath $sidecarPath -Force
        }
        return [pscustomobject]@{
            kind = $Kind
            index = $Index
            async_depth = $Depth
            frames = $selectedFrames
            end_to_end_wall_ms = [double]$result.wall_ms
            end_to_end_fps = [double]$result.throughput_fps
            gpu_raw_stage_total_ms = $stageTotalMs
            gpu_raw_service_fps = $rawServiceFps
            gpu_stage_mean_ms = $stageMeans
            encoder_send_mean_ms = [double]$result.pipeline.encoder_send_mean_ms
            encoder_receive_mean_ms = [double]$result.pipeline.encoder_receive_mean_ms
            movie_bytes = $movieBytes
        }
    }

    Write-Host "[4/6] Benchmarking fast GPU U16 RAW stages and MCRAW-to-MOV E2E at depth 1 and 2"
    $e2eRuns = @()
    foreach ($depth in @(1, 2)) {
        Write-Host "  E2E depth $depth warm-up"
        $e2eRuns += Invoke-E2eRun $depth "warmup" 0
        for ($runIndex = 1; $runIndex -le $OfficialRuns; ++$runIndex) {
            Write-Host "  E2E depth $depth official $runIndex/$OfficialRuns"
            $e2eRuns += Invoke-E2eRun $depth "official" $runIndex
        }
    }

    Write-Host "[5/6] Computing medians and writing reports"
    $summaryRows = @()
    foreach ($decoder in ($decoderSummaries | Sort-Object workers)) {
        $summaryRows += [pscustomobject]@{
            test = "mcraw_cpu_decode_only"
            variant = "workers_$($decoder.workers)"
            async_depth = $null
            median_fps = $decoder.median_fps
            min_fps = $decoder.min_fps
            max_fps = $decoder.max_fps
            scope = "Preloaded compressed payload -> official CPU U16 RAW decoder"
        }
    }
    foreach ($depth in @(1, 2)) {
        $officialEncoder = @($encoderRuns | Where-Object {
            $_.kind -eq "official" -and $_.async_depth -eq $depth
        })
        $encoderFps = [double[]]@($officialEncoder.throughput_fps)
        $summaryRows += [pscustomobject]@{
            test = "prores_vulkan_encoder_only"
            variant = "async_depth_$depth"
            async_depth = $depth
            median_fps = Get-Median $encoderFps
            min_fps = ($encoderFps | Measure-Object -Minimum).Minimum
            max_fps = ($encoderFps | Measure-Object -Maximum).Maximum
            scope = "FFmpeg lavfi -> yuv422p10le -> Vulkan upload -> prores_ks_vulkan -> null"
        }

        $officialE2e = @($e2eRuns | Where-Object {
            $_.kind -eq "official" -and $_.async_depth -eq $depth
        })
        $rawFps = [double[]]@($officialE2e.gpu_raw_service_fps)
        $e2eFps = [double[]]@($officialE2e.end_to_end_fps)
        $summaryRows += [pscustomobject]@{
            test = "gpu_u16_raw_processing_fast"
            variant = "async_depth_$depth"
            async_depth = $depth
            median_fps = Get-Median $rawFps
            min_fps = ($rawFps | Measure-Object -Minimum).Minimum
            max_fps = ($rawFps | Measure-Object -Maximum).Maximum
            scope = "GPU timestamp service rate; sum of six U16 RAW-to-YUV stages during E2E"
        }
        $summaryRows += [pscustomobject]@{
            test = "mcraw_to_mov_end_to_end_fast"
            variant = "async_depth_$depth"
            async_depth = $depth
            median_fps = Get-Median $e2eFps
            min_fps = ($e2eFps | Measure-Object -Minimum).Minimum
            max_fps = ($e2eFps | Measure-Object -Maximum).Maximum
            scope = "MCRAW read/decode -> fast GPU processing -> Vulkan ProRes HQ -> audio/MOV mux"
        }
    }

    $report = [ordered]@{
        schema = "mcraw-win-x64-component-benchmark-v1"
        captured_at_utc = [DateTime]::UtcNow.ToString("o")
        output_directory = $outputPath
        settings = [ordered]@{
            official_runs = $OfficialRuns
            selected_mcraw_frames = $selectedFrames
            cpu_decoder_frames = [int]$inspect.frame_count
            encoder_frames = $EncoderFrames
            dimensions = "${width}x${height}"
            decoder_workers = $DecoderWorkers
            performance_mode = "fast"
            async_depths = @(1, 2)
            ffmpeg_gpu_index = $FfmpegGpuIndex
            gpu_selector = if ($GpuSelector) { $GpuSelector } else { "from-fast-config" }
            keep_movies = [bool]$KeepMovies
        }
        measurement_notes = @(
            "CPU decode-only uses every frame in the MCRAW and preloads compressed payloads, excluding disk I/O and all downstream processing.",
            "GPU U16 RAW processing is a GPU-timestamp service rate calculated as frames*1000 divided by the sum of six GPU stage total_ms values.",
            "GPU U16 RAW processing timestamps exclude CPU decode, ProRes encode, and mux work, but the encoder shares the GPU during the measured E2E run.",
            "Encoder-only uses the separately supplied FFmpeg CLI and includes synthetic-frame generation, yuv422p10le conversion, Vulkan upload, prores_ks_vulkan, and null mux overhead.",
            "E2E includes process startup/preflight, MCRAW I/O and decode, fast GPU processing, Vulkan ProRes HQ, audio, and MOV mux."
        )
        best_cpu_decoder = $bestDecoder
        summary = $summaryRows
        raw_runs = [ordered]@{
            cpu_decoder = $decoderRows
            prores_vulkan = $encoderRuns
            end_to_end_and_gpu_raw = $e2eRuns
        }
    }
    Write-JsonFile $report (Join-Path $outputPath "benchmark-report.json")
    $summaryRows | Export-Csv -LiteralPath (Join-Path $outputPath "benchmark-summary.csv") `
        -NoTypeInformation -Encoding utf8

    $table = $summaryRows | Select-Object test, variant,
        @{N="median_fps"; E={"{0:F3}" -f $_.median_fps}},
        @{N="min_fps"; E={"{0:F3}" -f $_.min_fps}},
        @{N="max_fps"; E={"{0:F3}" -f $_.max_fps}} |
        Format-Table -AutoSize | Out-String -Width 220
    $textReport = @"
MCRAW Win x64 component benchmark
Generated UTC: $([DateTime]::UtcNow.ToString("o"))
Input: $inputPath
Input SHA-256: $($system.files.input.sha256)
Frames: $selectedFrames
Official runs: $OfficialRuns (one additional warm-up per case)

$table
Best CPU decoder worker count: $($bestDecoder.workers)
Best CPU decoder median FPS: $("{0:F3}" -f $bestDecoder.median_fps)

Important scope note:
GPU U16 RAW processing is a device timestamp service-rate measurement gathered
inside the E2E run. It excludes CPU decode and encoder timestamps, but it is not
an exclusive-GPU wall-time test because prores_ks_vulkan shares the same GPU.
"@
    Set-Content -LiteralPath (Join-Path $outputPath "benchmark-summary.txt") `
        -Value $textReport -Encoding utf8

    Write-Host "[6/6] Benchmark complete"
    Write-Host $table
    Write-Host "JSON: $(Join-Path $outputPath 'benchmark-report.json')"
    Write-Host "CSV : $(Join-Path $outputPath 'benchmark-summary.csv')"
    Write-Host "TXT : $(Join-Path $outputPath 'benchmark-summary.txt')"
} catch {
    $message = @(
        "Benchmark failed at $([DateTime]::UtcNow.ToString('o'))",
        $_.Exception.ToString()
    ) -join "`r`n"
    Set-Content -LiteralPath $failurePath -Value $message -Encoding utf8
    Write-Error "Benchmark failed. Details: $failurePath"
    throw
}
