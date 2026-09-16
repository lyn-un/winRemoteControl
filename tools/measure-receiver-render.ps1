param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build\cmake-release\tests\Release"),
    [string]$RuntimeDirectory = (Join-Path $PSScriptRoot "..\build\Release"),
    [ValidateRange(1, 20)][int]$Repeats = 3,
    [ValidateRange(1, 3600)][int]$SampleSeconds = 60,
    [ValidateRange(0, 600)][int]$WarmupSeconds = 10,
    [string]$OutputFile = (Join-Path $PSScriptRoot ("..\build\diagnostics\reports\receiver-render-{0}.json" -f (Get-Date -Format "yyyyMMdd-HHmmss")))
)

$ErrorActionPreference = "Stop"
$executable = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory "wrc_d3d_i420_tests.exe")).Path
$oldPath = $env:PATH
$report = [ordered]@{
    startedAt = (Get-Date).ToString("o")
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    scope = "Synthetic I420 conversion/upload/offscreen Draw; no desktop capture, input, decode or swapchain Present"
    cases = @()
    completed = $false
}
$outputPath = [System.IO.Path]::GetFullPath($OutputFile)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($outputPath)) | Out-Null
try {
    $env:PATH = "$RuntimeDirectory;$oldPath"
    for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        foreach ($size in @(@(1920, 1080), @(2560, 1440))) {
            foreach ($fps in @(30, 60)) {
                $modes = if ($repeat % 2 -eq 0) { @("i420", "bgra") } else { @("bgra", "i420") }
                foreach ($mode in $modes) {
                    Write-Host "Repeat=$repeat size=$($size[0])x$($size[1]) fps=$fps mode=$mode"
                    $raw = & $executable --benchmark $size[0] $size[1] $fps $mode $SampleSeconds $WarmupSeconds
                    if ($LASTEXITCODE -ne 0) {
                        throw "Receiver benchmark failed ($LASTEXITCODE), mode=$mode"
                    }
                    $result = ($raw -join "`n") | ConvertFrom-Json
                    $report.cases += [ordered]@{ repeat = $repeat; result = $result }
                    # Save only between cases, never from the per-frame measurement loop.
                    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding utf8
                }
            }
        }
    }
    $report.completed = $true
    $report.finishedAt = (Get-Date).ToString("o")
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding utf8
    Write-Host "Report: $outputPath"
}
finally {
    $env:PATH = $oldPath
}
