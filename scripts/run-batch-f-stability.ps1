param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-speedtranscode.exe",
    [string]$SampleDirectory = ".\mcraw_sample",
    [string]$Config = ".\config\vulkan-gpu-pipeline.json",
    [string]$OutputDirectory = ".\test-output\batch-f\stability",
    [int]$Iterations = 2
)

$ErrorActionPreference = "Stop"
if ($Iterations -lt 1) { throw "Iterations must be positive" }
foreach ($path in @($Executable, $Config)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing file: $path" }
}
$samples = @(Get-ChildItem -LiteralPath $SampleDirectory -Filter *.mcraw -File -Recurse |
    Sort-Object FullName)
if ($samples.Count -lt 2) { throw "Batch F requires at least two real MCRAW inputs" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$runs = @()
for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
    foreach ($sample in $samples) {
        $inspection = ((& $Executable inspect $sample.FullName) | Out-String) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0) { throw "inspect failed: $($sample.FullName)" }
        $name = [IO.Path]::GetFileNameWithoutExtension($sample.Name)
        $movie = Join-Path $OutputDirectory "$name.mov"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $conversion = ((& $Executable convert $sample.FullName $movie --config $Config --overwrite) |
            Out-String) | ConvertFrom-Json
        $timer.Stop()
        if ($LASTEXITCODE -ne 0 -or -not $conversion.ok -or
            [int]$conversion.frames -ne [int]$inspection.frame_count) {
            throw "conversion failed or frame count changed: $($sample.FullName)"
        }
        & "$PSScriptRoot\validate-release-candidate.ps1" -Movie $movie `
            -ExpectedFrames ([int]$inspection.frame_count) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "reader validation failed: $movie" }
        if (Test-Path -LiteralPath "$movie.partial.mov") {
            throw "completed run retained a partial MOV: $movie.partial.mov"
        }
        $runs += [ordered]@{
            iteration = $iteration
            sample = $sample.FullName
            frames = [int]$conversion.frames
            process_wall_ms = [double]$conversion.wall_ms
            harness_wall_ms = $timer.Elapsed.TotalMilliseconds
            throughput_fps = [double]$conversion.throughput_fps
            backend = $conversion.pipeline.backend
            gpu_uuid = $conversion.pipeline.gpu_uuid
            output = (Resolve-Path -LiteralPath $movie).Path
            output_bytes = (Get-Item -LiteralPath $movie).Length
        }
    }
}

$report = [ordered]@{
    schema = "mcraw-batch-f-stability-v1"
    iterations = $Iterations
    samples = $samples.Count
    runs = $runs
}
$reportPath = Join-Path $OutputDirectory "report.json"
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json -Depth 6
