param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-speedtranscode.exe",
    [string]$SampleDirectory = ".\mcraw_sample",
    [switch]$ValidateGpuPipeline,
    [int]$ConversionFrames = 30,
    [string]$OutputDirectory = ".\test-output\sample-validation",
    [string]$CpuConfig = ".\config\default.json",
    [string]$VulkanConfig = ".\config\vulkan-gpu-pipeline.json"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found: $Executable"
}
if ($ValidateGpuPipeline) {
    if ($ConversionFrames -lt 1) { throw "ConversionFrames must be positive" }
    foreach ($configPath in @($CpuConfig, $VulkanConfig)) {
        if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
            throw "Configuration not found: $configPath"
        }
    }
    $ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source
    $ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
}

$samples = Get-ChildItem -LiteralPath $SampleDirectory -Filter *.mcraw -File -Recurse
if ($samples.Count -eq 0) {
    throw "No .mcraw samples found under $SampleDirectory"
}

foreach ($sample in $samples) {
    Write-Host "Inspecting $($sample.FullName)"
    $inspection = (& $Executable inspect $sample.FullName) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw "inspect failed for $($sample.FullName)" }
    if ($inspection.frame_count -lt 1) { throw "sample contains no frames: $($sample.FullName)" }
    if ($inspection.last_timestamp_ns -lt $inspection.first_timestamp_ns) {
        throw "video timestamps move backward: $($sample.FullName)"
    }
    if ($inspection.audio.sample_rate -le 0 -or $inspection.audio.channels -le 0 -or
        $inspection.audio.chunks -lt 1) {
        throw "required source audio is missing or unreadable: $($sample.FullName)"
    }

    & $Executable validate $sample.FullName --frame 0
    if ($LASTEXITCODE -ne 0) { throw "first-frame validation failed for $($sample.FullName)" }

    $lastFrame = [int64]$inspection.frame_count - 1
    if ($lastFrame -gt 0) {
        & $Executable validate $sample.FullName --frame $lastFrame
        if ($LASTEXITCODE -ne 0) { throw "last-frame validation failed for $($sample.FullName)" }
    }

    if ($ValidateGpuPipeline) {
        $selectedFrames = [Math]::Min($ConversionFrames, [int]$inspection.frame_count)
        $baseName = [IO.Path]::GetFileNameWithoutExtension($sample.Name)
        $cpuOutput = Join-Path $OutputDirectory "$baseName-cpu.mov"
        $gpuOutput = Join-Path $OutputDirectory "$baseName-vulkan-direct.mov"

        Write-Host "Encoding $selectedFrames CPU and direct Vulkan frames for $($sample.Name)"
        $cpuResult = ((& $Executable convert $sample.FullName $cpuOutput --config $CpuConfig `
            --frames $selectedFrames --overwrite) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $cpuResult.pipeline.backend -ne "prores_ks") {
            throw "CPU reference conversion failed for $($sample.FullName)"
        }
        $gpuResult = ((& $Executable convert $sample.FullName $gpuOutput --config $VulkanConfig `
            --frames $selectedFrames --overwrite) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $gpuResult.pipeline.backend -ne "prores_ks_vulkan") {
            throw "direct Vulkan conversion failed for $($sample.FullName)"
        }
        if (-not $gpuResult.pipeline.gpu_resident -or
            $gpuResult.pipeline.entry -ne "camera_rgb_f32" -or
            $gpuResult.pipeline.direct_frames -ne $selectedFrames -or
            $gpuResult.pipeline.upload_frames -ne 0 -or
            $gpuResult.pipeline.readback_frames -ne 0 -or
            $gpuResult.pipeline.video_packets -ne $selectedFrames -or
            $gpuResult.pipeline.transfers.compressed_input_upload_bytes -ne 0 -or
            $gpuResult.pipeline.transfers.u16_raw_upload_bytes -ne 0 -or
            $gpuResult.pipeline.transfers.fp16_rgb_upload_bytes -ne 0 -or
            $gpuResult.pipeline.transfers.fp32_rgb_upload_bytes -ne
                $gpuResult.pipeline.rgb_upload_bytes -or
            $gpuResult.pipeline.transfers.camera_rgb_fp32_upload_bytes -ne
                $gpuResult.pipeline.rgb_upload_bytes -or
            $gpuResult.pipeline.transfers.target_log_fp32_upload_bytes -ne 0 -or
            $gpuResult.pipeline.transfers.control_status_read_bytes -ne
                ($selectedFrames * 4) -or
            -not $gpuResult.pipeline.gpu_timestamps_supported -or
            $gpuResult.pipeline.control_status_failures -ne 0 -or
            $gpuResult.pipeline.gpu_stages.camera_to_dwg.samples -ne $selectedFrames -or
            $gpuResult.pipeline.gpu_stages.capture_sharpening.samples -ne $selectedFrames -or
            $gpuResult.pipeline.gpu_stages.davinci_intermediate.samples -ne $selectedFrames -or
            $gpuResult.pipeline.gpu_stages.rgb_to_yuv_422.samples -ne $selectedFrames) {
            throw "direct Vulkan telemetry invariant failed for $($sample.FullName)"
        }

        foreach ($movie in @($cpuOutput, $gpuOutput)) {
            $probe = ((& $ffprobe -v error -count_packets -select_streams v:0 `
                -show_entries stream=codec_name,profile,pix_fmt,color_range,color_space,color_primaries,color_transfer,nb_read_packets `
                -of json $movie) | Out-String) | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or $probe.streams.Count -ne 1) {
                throw "ffprobe failed for $movie"
            }
            $stream = $probe.streams[0]
            if ($stream.codec_name -ne "prores" -or $stream.profile -ne "HQ" -or
                $stream.pix_fmt -ne "yuv422p10le" -or
                [int]$stream.nb_read_packets -ne $selectedFrames) {
                throw "ProRes profile, pixel format, or packet count mismatch for $movie"
            }
            & $ffmpeg -v error -i $movie -map 0:v:0 -f null NUL
            if ($LASTEXITCODE -ne 0) { throw "decode validation failed for $movie" }
        }

        $failureConfig = Join-Path $OutputDirectory "$baseName-invalid-device.json"
        $failureOutput = Join-Path $OutputDirectory "$baseName-invalid-device.mov"
        $invalid = Get-Content -LiteralPath $VulkanConfig -Raw | ConvertFrom-Json

        $fallbackConfig = Join-Path $OutputDirectory "$baseName-auto-fallback.json"
        $fallbackOutput = Join-Path $OutputDirectory "$baseName-auto-fallback.mov"
        $invalid.backend = "auto"
        $invalid.fallback = "prores_ks"
        $invalid.gpu_selector = "name:this-device-must-not-exist"
        $invalid | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fallbackConfig -Encoding utf8
        $fallbackResult = ((& $Executable convert $sample.FullName $fallbackOutput `
            --config $fallbackConfig --frames 1 --overwrite) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $fallbackResult.pipeline.backend -ne "prores_ks" -or
            -not $fallbackResult.pipeline.used_fallback -or
            [string]::IsNullOrWhiteSpace($fallbackResult.pipeline.fallback_reason)) {
            throw "automatic Vulkan failure did not report a CPU fallback for $($sample.FullName)"
        }

        $invalid.backend = "vulkan"
        $invalid.fallback = "none"
        $invalid | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $failureConfig -Encoding utf8
        $null = & $Executable convert $sample.FullName $failureOutput --config $failureConfig `
            --frames 1 --overwrite 2>&1
        if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $failureOutput)) {
            throw "forced Vulkan device failure produced a final MOV for $($sample.FullName)"
        }
    }
}

Write-Host "Validated $($samples.Count) MCRAW sample(s)."
