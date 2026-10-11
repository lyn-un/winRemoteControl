param(
    [ValidateSet('patched', 'original')][string]$Mode = 'patched',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Proxy = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$work = Join-Path $root 'build/ffmpeg-vendor'
$lockFile = Join-Path $PSScriptRoot 'vendor-dependencies.json'
$lock = Get-Content -Raw $lockFile | ConvertFrom-Json
foreach ($tool in @('usr/bin/bash.exe', 'ucrt64/bin/gcc.exe', 'ucrt64/bin/g++.exe')) {
    if (!(Test-Path "$MsysRoot/$tool")) { throw "Missing MSYS2 tool: $tool (not installed automatically)" }
}
Get-Command cmake, curl.exe, tar.exe -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Force "$work/downloads" | Out-Null
$inputs = @($lockFile, "$PSScriptRoot/build-vendor.ps1", "$PSScriptRoot/build-vendor.sh",
    "$PSScriptRoot/amf-dynamic-bitrate.patch", "$PSScriptRoot/vendor-probe.c",
    "$PSScriptRoot/test-amf-rate-update.c", "$PSScriptRoot/test-amf-rate-update.sh",
    "$PSScriptRoot/qsv-optional-config-interface.patch",
    "$PSScriptRoot/test-qsv-config-interface.c", "$PSScriptRoot/test-qsv-config-interface.sh")
$fingerprint = ($inputs | ForEach-Object { (Get-FileHash $_).Hash }) -join ':'
$fingerprint += ':' + (& "$MsysRoot/ucrt64/bin/gcc.exe" -dumpfullversion)
$hash = [System.Security.Cryptography.SHA256]::Create()
try { $key = ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($fingerprint)))).Replace('-', '').Substring(0, 16).ToLowerInvariant() }
finally { $hash.Dispose() }
$run = "$work/$key"
New-Item -ItemType Directory -Force "$run/sources" | Out-Null
foreach ($entry in $lock.PSObject.Properties) {
    $name = $entry.Name
    $dep = $entry.Value
    $extension = if ($name -eq 'nasm') { 'zip' } else { 'tar.gz' }
    $archive = "$work/downloads/$name-$($dep.version).$extension"
    if (!(Test-Path $archive)) {
        $curlArgs = @('--fail', '--location', '--connect-timeout', '15', '--max-time', '300')
        if ($Proxy) { $curlArgs += @('--proxy', $Proxy) }
        & curl.exe @curlArgs $dep.url -o $archive
        if ($LASTEXITCODE) { throw "$name download failed" }
    }
    if ((Get-FileHash $archive).Hash -ne $dep.sha256) { throw "$name SHA256 mismatch: $archive" }
    $source = "$run/sources/$name"
    if (!(Test-Path "$source/.extracted")) {
        New-Item -ItemType Directory -Force $source | Out-Null
        if ($name -eq 'nasm') { Expand-Archive $archive $source -Force }
        else {
            & tar.exe -xf $archive --strip-components=1 -C $source
            if ($LASTEXITCODE) { throw "$name extraction failed" }
        }
        New-Item -ItemType File -Force "$source/.extracted" | Out-Null
    }
}
# Dispatcher only; GPU implementations remain supplied by the Intel driver.
& cmake -S "$run/sources/libvpl" -B "$run/vpl-build" -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_INSTALL_PREFIX=$run/deps/vpl" -DBUILD_SHARED_LIBS=ON -DBUILD_TESTS=OFF `
    -DBUILD_EXAMPLES=OFF -DBUILD_EXPERIMENTAL=OFF
if ($LASTEXITCODE) { throw 'VPL configure failed' }
& cmake --build "$run/vpl-build" --config Release --parallel 1 -- /nodeReuse:false
if ($LASTEXITCODE) { throw 'VPL build failed' }
& cmake --install "$run/vpl-build" --config Release
if ($LASTEXITCODE) { throw 'VPL install failed' }
Push-Location $root
try {
    & "$MsysRoot/usr/bin/bash.exe" tools/ffmpeg/build-vendor.sh $key $Mode
    if ($LASTEXITCODE) { throw "Vendor FFmpeg build failed: $run" }
} finally { Pop-Location }
Write-Host "FFMPEG_ROOT=$root/third_party/ffmpeg-wrc-vendor/$key/$Mode"
