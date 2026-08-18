param(
    [string]$LogPath = (Join-Path $PSScriptRoot "..\build\Release\logs\latency_trace_controller.log"),
    [ValidateSet("static", "mouse", "window")]
    [string]$Scenario
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
    throw "Latency trace log not found: $LogPath"
}

function Get-Percentile([int[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) {
        return $null
    }

    $sortedValues = @($Values | Sort-Object)
    $index = [Math]::Max(0, [Math]::Ceiling($Percentile * $sortedValues.Count) - 1)
    return $sortedValues[$index]
}

$lines = @(Get-Content -LiteralPath $LogPath)
if (-not [string]::IsNullOrWhiteSpace($Scenario)) {
    $scenarioPattern = "(?:^| )scenario=$([Regex]::Escape($Scenario))(?: |$)"
    $lines = @($lines | Where-Object { $_ -match $scenarioPattern })
}

$lastStartupIndex = -1
for ($index = 0; $index -lt $lines.Count; ++$index) {
    if ($lines[$index] -match " side=app stage=startup(?: |$)") {
        $lastStartupIndex = $index
    }
}
if ($lastStartupIndex -ge 0) {
    $lines = @($lines[$lastStartupIndex..($lines.Count - 1)])
}
if ($lines.Count -eq 0) {
    throw "No latency trace events matched the requested scenario."
}

$roundTripValues = @()
$lowLatencyEnabled = 0
$lowLatencyDisabled = 0
$renderSummary = $null
$conversionSummary = $null
$finalWindowSummary = $null

foreach ($line in $lines) {
    if ($line -match "stage=input_roundtrip .*roundTripMs=(\d+) includedInStats=1") {
        $roundTripValues += [int]$Matches[1]
    }
    if ($line -match "stage=input_roundtrip_summary .*samples=(\d+) totalSamples=(\d+) avgMs=(\d+) p50Ms=(\d+) p95Ms=(\d+) maxMs=(\d+)") {
        $finalWindowSummary = [ordered]@{
            samples = [int]$Matches[1]
            totalSamples = [int]$Matches[2]
            averageMs = [int]$Matches[3]
            p50Ms = [int]$Matches[4]
            p95Ms = [int]$Matches[5]
            maxMs = [int]$Matches[6]
        }
    }
    if ($line -match "stage=render_end .*lowLatencyRender=([01])") {
        if ($Matches[1] -eq "1") {
            ++$lowLatencyEnabled
        }
        else {
            ++$lowLatencyDisabled
        }
    }
    if ($line -match "stage=render_frame_coalesce_summary .*stage=gui_queue receivedTotal=(\d+) presentedTotal=(\d+) coalescedTotal=(\d+)") {
        $renderSummary = [ordered]@{
            received = [int]$Matches[1]
            presented = [int]$Matches[2]
            coalesced = [int]$Matches[3]
        }
    }
    if ($line -match "stage=remote_callback_frame_coalesce_summary .*stage=conversion_queue receivedTotal=(\d+) processedTotal=(\d+) coalescedTotal=(\d+)") {
        $conversionSummary = [ordered]@{
            received = [int]$Matches[1]
            processed = [int]$Matches[2]
            coalesced = [int]$Matches[3]
        }
    }
}

if ($null -ne $renderSummary -and $renderSummary.received -gt 0) {
    $renderSummary.coalescedPercent = [Math]::Round(
        100.0 * $renderSummary.coalesced / $renderSummary.received, 2)
}
if ($null -ne $conversionSummary -and $conversionSummary.received -gt 0) {
    $conversionSummary.coalescedPercent = [Math]::Round(
        100.0 * $conversionSummary.coalesced / $conversionSummary.received, 2)
}

$roundTrip = [ordered]@{
    samples = $roundTripValues.Count
    averageMs = if ($roundTripValues.Count -gt 0) {
        [Math]::Round(($roundTripValues | Measure-Object -Average).Average, 1)
    } else { $null }
    p50Ms = Get-Percentile $roundTripValues 0.50
    p95Ms = Get-Percentile $roundTripValues 0.95
    maxMs = if ($roundTripValues.Count -gt 0) {
        ($roundTripValues | Measure-Object -Maximum).Maximum
    } else { $null }
}

[ordered]@{
    source = (Resolve-Path -LiteralPath $LogPath).Path
    scenario = if ([string]::IsNullOrWhiteSpace($Scenario)) { "unlabeled" } else { $Scenario }
    inputRoundTrip = [ordered]@{
        allSamples = $roundTrip
        finalWindow = $finalWindowSummary
    }
    lowLatencyRender = [ordered]@{
        enabledSamples = $lowLatencyEnabled
        disabledSamples = $lowLatencyDisabled
    }
    guiQueue = $renderSummary
    conversionQueue = $conversionSummary
} | ConvertTo-Json -Depth 5
