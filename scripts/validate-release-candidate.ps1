param(
    [Parameter(Mandatory = $true)]
    [string]$Movie,
    [string]$OutputJson = "",
    [int]$ExpectedFrames = 0,
    [double[]]$SeekSeconds = @(0, 2, 4, 6)
)

$ErrorActionPreference = "Stop"
$moviePath = (Resolve-Path -LiteralPath $Movie).Path
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source

$probe = ((& $ffprobe -v error -count_packets -show_format -show_streams -of json $moviePath) |
    Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed" }
$video = @($probe.streams | Where-Object codec_type -eq "video")
$audio = @($probe.streams | Where-Object codec_type -eq "audio")
if ($video.Count -ne 1 -or $audio.Count -ne 1) {
    throw "expected exactly one video and one audio stream"
}
if ($video[0].codec_name -ne "prores" -or $video[0].profile -ne "HQ" -or
    $video[0].pix_fmt -ne "yuv422p10le") {
    throw "unexpected video codec, profile, or pixel format"
}
if ($ExpectedFrames -gt 0 -and [int]$video[0].nb_read_packets -ne $ExpectedFrames) {
    throw "video packet count does not match ExpectedFrames"
}

& $ffmpeg -v error -i $moviePath -map 0:v:0 -map 0:a:0 -f null NUL
if ($LASTEXITCODE -ne 0) { throw "complete software decode failed" }

$seekResults = foreach ($second in $SeekSeconds) {
    & $ffmpeg -v error -ss $second -i $moviePath -frames:v 1 -f null NUL
    [ordered]@{ seconds = $second; passed = ($LASTEXITCODE -eq 0) }
}
if (@($seekResults | Where-Object passed -eq $false).Count -ne 0) {
    throw "one or more random-access decode checks failed"
}

$vlc = "C:\Program Files\VideoLAN\VLC\vlc.exe"
$vlcResult = [ordered]@{ installed = (Test-Path -LiteralPath $vlc); passed = $false }
if ($vlcResult.installed) {
    & $vlc --intf dummy --play-and-exit --no-audio --vout dummy --no-video-title-show $moviePath |
        Out-Null
    $vlcResult.passed = ($LASTEXITCODE -eq 0)
}

$result = [ordered]@{
    movie = $moviePath
    ffmpeg_full_decode = $true
    seeks = @($seekResults)
    vlc = $vlcResult
    video = [ordered]@{
        codec = $video[0].codec_name
        profile = $video[0].profile
        pixel_format = $video[0].pix_fmt
        width = [int]$video[0].width
        height = [int]$video[0].height
        packets = [int]$video[0].nb_read_packets
        duration_seconds = [double]$video[0].duration
        color_range = $video[0].color_range
        color_space = $video[0].color_space
        color_primaries = $video[0].color_primaries
        color_transfer = $video[0].color_transfer
    }
    audio = [ordered]@{
        codec = $audio[0].codec_name
        duration_seconds = [double]$audio[0].duration
        sample_rate = [int]$audio[0].sample_rate
        channels = [int]$audio[0].channels
    }
    container_duration_seconds = [double]$probe.format.duration
}
$json = $result | ConvertTo-Json -Depth 8
if ($OutputJson) { Set-Content -LiteralPath $OutputJson -Encoding utf8 -Value $json }
$json

