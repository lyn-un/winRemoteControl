# Current entry point. "patched" means AMF, never the retired MF experiment.
param(
    [ValidateSet('original', 'patched')][string]$Mode = 'patched',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Proxy = ''
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/build-vendor.ps1" -Mode $Mode -MsysRoot $MsysRoot -Proxy $Proxy
