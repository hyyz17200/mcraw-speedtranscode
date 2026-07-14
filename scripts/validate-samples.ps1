param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-transcoder.exe",
    [string]$SampleDirectory = ".\mcraw_sample",
    [switch]$ValidateGpuPipeline,
    [int]$ConversionFrames = 30,
    [string]$OutputDirectory = ".\test-output\sample-validation",
    [string]$CpuConfig = ".\config\default.json",
    [string]$VulkanConfig = ".\config\vulkan-upload-bridge.json"
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
        $gpuOutput = Join-Path $OutputDirectory "$baseName-vulkan-upload.mov"

        Write-Host "Encoding $selectedFrames CPU and Vulkan-upload frames for $($sample.Name)"
        $cpuResult = ((& $Executable convert $sample.FullName $cpuOutput --config $CpuConfig `
            --frames $selectedFrames --overwrite) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $cpuResult.pipeline.backend -ne "prores_ks") {
            throw "CPU reference conversion failed for $($sample.FullName)"
        }
        $gpuResult = ((& $Executable convert $sample.FullName $gpuOutput --config $VulkanConfig `
            --frames $selectedFrames --overwrite) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $gpuResult.pipeline.backend -ne "prores_ks_vulkan") {
            throw "Vulkan upload-bridge conversion failed for $($sample.FullName)"
        }
        if ($gpuResult.pipeline.gpu_resident -or
            $gpuResult.pipeline.upload_frames -ne $selectedFrames -or
            $gpuResult.pipeline.readback_frames -ne 0 -or
            $gpuResult.pipeline.video_packets -ne $selectedFrames) {
            throw "Vulkan upload-bridge telemetry invariant failed for $($sample.FullName)"
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
        $invalid.gpu_selector = "name:this-device-must-not-exist"
        $invalid | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $failureConfig -Encoding utf8
        $null = & $Executable convert $sample.FullName $failureOutput --config $failureConfig `
            --frames 1 --overwrite 2>&1
        if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $failureOutput)) {
            throw "forced Vulkan device failure produced a final MOV for $($sample.FullName)"
        }
    }
}

Write-Host "Validated $($samples.Count) MCRAW sample(s)."
