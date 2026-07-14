param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-transcoder.exe",
    [string]$SampleDirectory = ".\mcraw_sample"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found: $Executable"
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
}

Write-Host "Validated $($samples.Count) MCRAW sample(s)."
