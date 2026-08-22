param(
	[ValidateSet('Debug', 'Release')]
	[string]$Configuration = 'Debug',
	[string]$TestExecutable,
	[string]$OutputDirectory,
	[int]$TimeoutMilliseconds = 60000,
	[switch]$RequireExternalClock,
	[switch]$RequireNativePresentation
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($TestExecutable)) {
	$TestExecutable = Join-Path $repoRoot "x64\$Configuration\tests1.exe"
} elseif (-not [System.IO.Path]::IsPathRooted($TestExecutable)) {
	$TestExecutable = Join-Path $repoRoot $TestExecutable
}
$TestExecutable = (Resolve-Path -LiteralPath $TestExecutable).Path

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
	$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
	$OutputDirectory = Join-Path $repoRoot "build\results\rendering\c8-production-$stamp"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
	$OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

$jsonPath = Join-Path $OutputDirectory 'telemetry.json'
$stdoutPath = Join-Path $OutputDirectory 'tests1.stdout.txt'
$stderrPath = Join-Path $OutputDirectory 'tests1.stderr.txt'
$validationPath = Join-Path $OutputDirectory 'validation.json'
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$filter = 'FrameCoordinatorC8Telemetry.ProductionRuntimeCleanIdleTenSecondsHasNoImplicitTimer'

$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $TestExecutable
$psi.WorkingDirectory = $repoRoot
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Arguments = "--gtest_filter=$filter --gtest_color=no"
$psi.EnvironmentVariables['SAKURA_C8_TELEMETRY_OUTPUT'] = $jsonPath

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $psi
$processId = 0
$timedOut = $false
$exitCode = $null
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
try {
	if (-not $process.Start()) {
		throw "Unable to start tests1.exe: $TestExecutable"
	}
	$processId = $process.Id
	$stdoutTask = $process.StandardOutput.ReadToEndAsync()
	$stderrTask = $process.StandardError.ReadToEndAsync()
	if (-not $process.WaitForExit($TimeoutMilliseconds)) {
		$timedOut = $true
		try {
			$process.Kill($true)
		} catch {
			$process.Kill()
		}
		$process.WaitForExit()
	}
	[System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask))
	[System.IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
	[System.IO.File]::WriteAllText($stderrPath, $stderrTask.Result)
	if (-not $timedOut) {
		$exitCode = $process.ExitCode
	}
} finally {
	if ($processId -ne 0) {
		try {
			if (-not $process.HasExited) {
				try {
					$process.Kill($true)
				} catch {
					$process.Kill()
				}
				$process.WaitForExit()
			}
		} catch {
			# The exact process may have exited between HasExited and Kill.
		}
		$survivor = Get-Process -Id $processId -ErrorAction SilentlyContinue
		if ($null -ne $survivor) {
			throw "tests1.exe survivor remains after cleanup: PID $processId"
		}
	}
	$process.Dispose()
	$stopwatch.Stop()
}

if ($timedOut) {
	throw "C8 production trial timed out after $TimeoutMilliseconds ms. Output: $OutputDirectory"
}
if ($exitCode -ne 0) {
	throw "C8 production trial failed with exit code $exitCode. See $stdoutPath and $stderrPath"
}
if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
	throw "C8 production trial did not produce telemetry: $jsonPath"
}

$evidence = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
if (-not [bool]$evidence.productionRuntime) {
	throw 'Telemetry is not marked as productionRuntime=true.'
}
$idle = $evidence.cleanIdle
$cleanIdleStable = ($idle.requestedTickCountBefore -eq $idle.requestedTickCountAfter) -and
	($idle.processedTickCountBefore -eq $idle.processedTickCountAfter)
if (-not $cleanIdleStable) {
	throw 'Clean-idle tick counts changed during the ten-second no-input window.'
}
$cadenceSource = $evidence.cadenceSource
if ([uint64]$cadenceSource.displayEpoch -eq 0) {
	throw 'Cadence source did not publish a valid display epoch.'
}
$hasExternalClock = [bool]$cadenceSource.displayRateObserved -or [bool]$cadenceSource.compositorRateObserved
if ($RequireExternalClock -and -not $hasExternalClock) {
	throw 'No display/compositor refresh rate was acquired from the supported Win32 sources.'
}
if ($RequireNativePresentation -and [uint64]$evidence.nativePresentAttempts -eq 0) {
	throw 'The trial did not exercise a native Present1 target.'
}

$validator = Join-Path $PSScriptRoot 'validate-c8-telemetry.ps1'
& pwsh -NoProfile -ExecutionPolicy Bypass -File $validator -InputPath $jsonPath -OutputPath $validationPath -FailOnThresholds
$validationExitCode = $LASTEXITCODE
if ($validationExitCode -ne 0) {
	throw "C8 telemetry thresholds failed with exit code $validationExitCode. See $validationPath"
}
$validation = Get-Content -LiteralPath $validationPath -Raw | ConvertFrom-Json

$nativeEvidence = if ([uint64]$evidence.nativePresentAttempts -gt 0) {
	'observed by the native presentation boundary'
} else {
	'not exercised by the software idle trial; no native target/content projection claim'
}
$summary = [ordered]@{
	schemaVersion = 1
	productionRuntime = $true
	configuration = $Configuration
	testExecutable = $TestExecutable
	testFilter = $filter
	testExitCode = $exitCode
	durationMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
	telemetryPath = [System.IO.Path]::GetFullPath($jsonPath)
	validationPath = [System.IO.Path]::GetFullPath($validationPath)
	cadenceSource = $cadenceSource
	cleanIdle = $idle
	nativePresentationEvidence = $nativeEvidence
	physicalVisibility = 'not asserted by request-to-publication software fallback evidence'
	validation = $validation
}
[System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8) + [Environment]::NewLine)
[Console]::Out.WriteLine($summaryPath)
