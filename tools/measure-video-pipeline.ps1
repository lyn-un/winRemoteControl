param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build\cmake-release\Release"),
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$Frames = 120
)

$ErrorActionPreference = "Stop"
$executable = Join-Path $BuildDirectory "wrc_video_pipeline_benchmark.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Benchmark executable not found: $executable"
}

function Invoke-VideoPipelineBenchmark([string]$Mode) {
    $json = & $executable --mode $Mode --width $Width --height $Height --frames $Frames
    if ($LASTEXITCODE -ne 0) {
        throw "Video pipeline benchmark failed in mode '$Mode'"
    }
    return $json | ConvertFrom-Json
}

$legacy = Invoke-VideoPipelineBenchmark "legacy"
$optimized = Invoke-VideoPipelineBenchmark "optimized"
[ordered]@{
    measuredAt = (Get-Date).ToString("o")
    input = [ordered]@{
        width = $Width
        height = $Height
        frames = $Frames
    }
    legacy = $legacy
    optimized = $optimized
} | ConvertTo-Json -Depth 5
