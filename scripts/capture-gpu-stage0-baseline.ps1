[CmdletBinding()]
param(
    [string]$Executable = ".\build\msvc-release\Release\mcraw-transcoder.exe",
    [string]$Corpus = ".\config\gpu-stage0-corpus.json",
    [string]$OutputDirectory = ".\test-output\gpu-stage0-baseline"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Invoke-Json([string[]]$Arguments) {
    $text = (& $script:executablePath @Arguments | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "mcraw-transcoder failed: $($Arguments -join ' ')"
    }
    return $text | ConvertFrom-Json -Depth 100
}

$executablePath = Resolve-RepoPath $Executable
$corpusPath = Resolve-RepoPath $Corpus
$outputPath = Resolve-RepoPath $OutputDirectory
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "transcoder not found: $executablePath"
}
if (-not (Test-Path -LiteralPath $corpusPath -PathType Leaf)) {
    throw "corpus contract not found: $corpusPath"
}

$contract = Get-Content -LiteralPath $corpusPath -Raw | ConvertFrom-Json -Depth 100
$sampleSpec = @($contract.real_samples)[0]
$samplePath = Resolve-RepoPath $sampleSpec.path
$configPath = Resolve-RepoPath $contract.production_config
if (-not (Test-Path -LiteralPath $samplePath -PathType Leaf)) {
    throw "baseline sample not found: $samplePath"
}

$sampleFile = Get-Item -LiteralPath $samplePath
$sampleHash = (Get-FileHash -LiteralPath $samplePath -Algorithm SHA256).Hash
if ($sampleFile.Length -ne [int64]$sampleSpec.bytes -or
    $sampleHash -ne [string]$sampleSpec.sha256) {
    throw "baseline sample size or SHA-256 does not match the corpus contract"
}

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$artifactPath = Join-Path $outputPath "artifacts"
New-Item -ItemType Directory -Path $artifactPath -Force | Out-Null

$inspect = Invoke-Json @("inspect", $samplePath)
$capabilities = Invoke-Json @("list-capabilities")
$effectiveConfig = Invoke-Json @("print-effective-config", "--config", $configPath)
if ([int]$inspect.frame_count -ne [int]$sampleSpec.frames -or
    [int]$inspect.first_frame.width -ne [int]$sampleSpec.width -or
    [int]$inspect.first_frame.height -ne [int]$sampleSpec.height) {
    throw "sample inspection does not match the corpus contract"
}

$artifacts = @()
foreach ($frame in @($sampleSpec.golden_frames)) {
    foreach ($stage in @($contract.golden_stages)) {
        $name = "frame-{0:D3}-{1}.bin" -f [int]$frame, [string]$stage
        $path = Join-Path $artifactPath $name
        & $executablePath extract-frame $samplePath --frame $frame --stage $stage `
            --config $configPath --output $path
        if ($LASTEXITCODE -ne 0) {
            throw "failed to extract frame $frame stage $stage"
        }
        $file = Get-Item -LiteralPath $path
        $artifacts += [ordered]@{
            frame = [int]$frame
            stage = [string]$stage
            file = "artifacts/$name"
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
    }
}

$shaderFiles = Get-ChildItem -LiteralPath (Join-Path $repoRoot "src\vulkan\shaders") `
    -File | Sort-Object Name
$shaders = @($shaderFiles | ForEach-Object {
    [ordered]@{
        file = "src/vulkan/shaders/$($_.Name)"
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
})
$gitCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
$gitStatus = @(& git -C $repoRoot status --porcelain=v1)

$manifest = [ordered]@{
    schema = "mcraw-gpu-stage0-baseline-manifest-v1"
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    corpus_contract = [ordered]@{
        file = [System.IO.Path]::GetRelativePath($repoRoot, $corpusPath).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $corpusPath -Algorithm SHA256).Hash
    }
    source = [ordered]@{
        commit = $gitCommit
        dirty = $gitStatus.Count -ne 0
        status = $gitStatus
    }
    build = [ordered]@{
        executable = $executablePath
        executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
        config = [System.IO.Path]::GetRelativePath($repoRoot, $configPath).Replace('\', '/')
        config_sha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
        effective_config = $effectiveConfig
        shaders = $shaders
        capabilities = $capabilities
    }
    input = [ordered]@{
        id = $sampleSpec.id
        path = $sampleSpec.path
        bytes = $sampleFile.Length
        sha256 = $sampleHash
        inspection = $inspect
    }
    quality_contract = [ordered]@{
        targets = $contract.product_targets
        synthetic_cases = $contract.synthetic_cases
        artifacts = $artifacts
    }
}

$manifestFile = Join-Path $outputPath "baseline-manifest.json"
$manifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $manifestFile -Encoding utf8
Write-Host "Stage 0 baseline manifest: $manifestFile"
if ($manifest.source.dirty) {
    Write-Warning "manifest records a dirty tree; freeze the final baseline again from its committed rollback point"
}
