param(
	[string]$Executable = "",
	[string]$Python = "python",
	[switch]$IncludeDestructive
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($Executable)) {
	$Executable = Join-Path $repositoryRoot "build\Release\winRemoteControl.exe"
}
$resolvedExecutable = (Resolve-Path $Executable).Path
$releaseDirectory = Split-Path $resolvedExecutable -Parent
$acceptanceRoot = Join-Path $repositoryRoot "build\automation-acceptance"
$stagingDirectory = Join-Path $acceptanceRoot "without-driver"
$profileDirectory = Join-Path $acceptanceRoot "without-driver-profile"

function Stop-TestProcess {
	param([System.Diagnostics.Process]$Process)
	if ($null -eq $Process -or $Process.HasExited) {
		return
	}
	$null = $Process.CloseMainWindow()
	if (-not $Process.WaitForExit(8000)) {
		try {
			$Process.Kill($true)
		}
		catch {
			& taskkill.exe /PID $Process.Id /T /F | Out-Null
		}
		$Process.WaitForExit()
	}
}

Write-Host "[1/2] 验证不含 automation/wrcdriver.dll 时不创建自动化端点"
if (Test-Path $acceptanceRoot) {
	Remove-Item -LiteralPath $acceptanceRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $releaseDirectory -Force |
	Where-Object { $_.Name -ne "automation" } |
	Copy-Item -Destination $stagingDirectory -Recurse -Force
$stagedExecutable = Join-Path $stagingDirectory "winRemoteControl.exe"
$process = $null
try {
	$process = Start-Process -FilePath $stagedExecutable -ArgumentList @(
		"--data-dir", $profileDirectory, "--automation-test-profile"
	) -PassThru
	Start-Sleep -Seconds 3
	if ($process.HasExited) {
		throw "不含 Driver 的主程序提前退出，exitCode=$($process.ExitCode)"
	}
	$discoveryFile = Join-Path $env:LOCALAPPDATA (
		"winRemoteControl\automation\{0}.json" -f $process.Id)
	if (Test-Path -LiteralPath $discoveryFile) {
		throw "不含 Driver 时仍创建了发现文件：$discoveryFile"
	}
}
finally {
	Stop-TestProcess -Process $process
	if (Test-Path $acceptanceRoot) {
		Remove-Item -LiteralPath $acceptanceRoot -Recurse -Force
	}
}

Write-Host "[2/2] 运行两个隔离实例的自动化 E2E"
$previousPythonPath = $env:PYTHONPATH
$previousRunE2E = $env:WRC_RUN_E2E
$previousExecutable = $env:WRC_EXECUTABLE
$previousDestructive = $env:WRC_RUN_DESTRUCTIVE_AUTOMATION
try {
	$env:PYTHONPATH = Join-Path $repositoryRoot "automation\python"
	$env:WRC_RUN_E2E = "1"
	$env:WRC_EXECUTABLE = $resolvedExecutable
	if ($IncludeDestructive) {
		$env:WRC_RUN_DESTRUCTIVE_AUTOMATION = "1"
	}
	else {
		Remove-Item Env:\WRC_RUN_DESTRUCTIVE_AUTOMATION -ErrorAction SilentlyContinue
	}
	Push-Location $repositoryRoot
	try {
		& $Python -m unittest discover -s automation\tests -p "test_*.py" -v
		if ($LASTEXITCODE -ne 0) {
			throw "Python E2E 返回失败：$LASTEXITCODE"
		}
	}
	finally {
		Pop-Location
	}
}
finally {
	$env:PYTHONPATH = $previousPythonPath
	$env:WRC_RUN_E2E = $previousRunE2E
	$env:WRC_EXECUTABLE = $previousExecutable
	$env:WRC_RUN_DESTRUCTIVE_AUTOMATION = $previousDestructive
}

Write-Host "自动化实机验收通过。"
