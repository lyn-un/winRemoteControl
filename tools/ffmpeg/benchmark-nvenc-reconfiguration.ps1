param(
    [string]$TestExecutable = "$PSScriptRoot/../../build/cmake-release/tests/Release/wrc_vendor_bitrate_output_tests.exe",
    [string]$RuntimeDirectory = "$PSScriptRoot/../../build/Release",
    [string]$OutputDirectory = "$PSScriptRoot/../../build/nvenc-reconfiguration",
    [ValidateRange(1, 10)][int]$Repeats = 3
)
$ErrorActionPreference = 'Stop'
$test = (Resolve-Path $TestExecutable).Path
$runtime = (Resolve-Path $RuntimeDirectory).Path
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$previousPath = $env:PATH
$previousGate = $env:WRC_RUN_VENDOR_BITRATE_TEST
$results = @()
try {
    $env:PATH = "$runtime;$env:SystemRoot/System32;$env:SystemRoot"
    $env:WRC_RUN_VENDOR_BITRATE_TEST = '1'
    Get-FileHash "$runtime/avcodec-62.dll", "$runtime/avutil-60.dll", $test |
        ConvertTo-Json | Out-File "$output/input-hashes.json" -Encoding utf8
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        foreach ($policy in @('11', '10', '00')) {
            foreach ($scenario in @('strict', 'frequent', 'keyframe')) {
                $arguments = @('h264_nvenc', "--nvenc-reconfig-reset=$($policy[0])", "--nvenc-reconfig-idr=$($policy[1])")
                if ($scenario -eq 'frequent') { $arguments += '--frequent-rates' }
                if ($scenario -eq 'keyframe') { $arguments += '--keyframe-policy' }
                $log = "$output/$policy-$repeat-$scenario.log"
                Write-Host "NVENC policy=$policy repeat=$repeat scenario=$scenario"
                & $test @arguments *> $log
                $code = $LASTEXITCODE
                $results += [pscustomobject]@{ policy=$policy; repeat=$repeat; scenario=$scenario; exitCode=$code; log=$log }
                $results | ConvertTo-Json | Out-File "$output/results.json" -Encoding utf8
                if ($code -ne 0) { Write-Warning "Test did not pass: exit=$code log=$log" }
            }
        }
    }
} finally {
    $env:PATH = $previousPath
    $env:WRC_RUN_VENDOR_BITRATE_TEST = $previousGate
}
if ($results | Where-Object exitCode -ne 0) { exit 1 }
