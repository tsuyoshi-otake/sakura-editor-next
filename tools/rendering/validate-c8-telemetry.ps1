param(
	[Parameter(Mandatory = $true)]
	[string]$InputPath,
	[string]$OutputPath,
	[double]$UiHandlerP99Milliseconds = 2.0,
	[double]$UiHandlerMaximumMilliseconds = 8.0,
	[double]$LockP99Microseconds = 100.0,
	[double]$LockMaximumMicroseconds = 1000.0,
	[double]$InputToVisibleP95Intervals = 2.0,
	[double]$InputToVisibleP99Intervals = 3.0,
	[switch]$FailOnThresholds
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
	throw "Telemetry input does not exist: $InputPath"
}

$telemetry = Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$failures = [System.Collections.Generic.List[string]]::new()

function Get-RequiredNumber {
	param([object]$Object, [string]$Name)
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property -or $null -eq $property.Value) {
		throw "Telemetry field is missing: $Name"
	}
	return [double]$property.Value
}

function Test-Limit {
	param([string]$Name, [double]$Actual, [double]$Limit)
	if ($Actual -gt $Limit) {
		$failures.Add("$Name=$Actual exceeds $Limit")
	}
}

$queueChecks = @(
	@{ Name = 'control'; Depth = 'controlQueueDepth'; Capacity = 'maxControlQueueDepth' },
	@{ Name = 'cpuWork'; Depth = 'cpuWorkQueueDepth'; Capacity = 'maxCpuWorkQueueDepth' },
	@{ Name = 'publication'; Depth = 'publicationDepth'; Capacity = 'maxPublicationDepth' }
)
foreach ($queue in $queueChecks) {
	$depthProperty = $telemetry.PSObject.Properties[$queue.Depth]
	$capacityProperty = $telemetry.PSObject.Properties[$queue.Capacity]
	if ($null -ne $depthProperty -and $null -ne $capacityProperty) {
		if ([double]$depthProperty.Value -gt [double]$capacityProperty.Value) {
			$failures.Add("$($queue.Name) queue exceeded its capacity")
		}
	}
}

$uiP99 = Get-RequiredNumber $telemetry 'uiHandlerP99Milliseconds'
$uiMax = Get-RequiredNumber $telemetry 'uiHandlerMaximumMilliseconds'
$lockP99 = Get-RequiredNumber $telemetry 'lockP99Microseconds'
$lockMax = Get-RequiredNumber $telemetry 'lockMaximumMicroseconds'
$visibleP95 = Get-RequiredNumber $telemetry 'inputToVisibleP95Intervals'
$visibleP99 = Get-RequiredNumber $telemetry 'inputToVisibleP99Intervals'

Test-Limit 'uiHandlerP99Milliseconds' $uiP99 $UiHandlerP99Milliseconds
Test-Limit 'uiHandlerMaximumMilliseconds' $uiMax $UiHandlerMaximumMilliseconds
Test-Limit 'lockP99Microseconds' $lockP99 $LockP99Microseconds
Test-Limit 'lockMaximumMicroseconds' $lockMax $LockMaximumMicroseconds
Test-Limit 'inputToVisibleP95Intervals' $visibleP95 $InputToVisibleP95Intervals
Test-Limit 'inputToVisibleP99Intervals' $visibleP99 $InputToVisibleP99Intervals

$report = [ordered]@{
	passed = $failures.Count -eq 0
	inputPath = [System.IO.Path]::GetFullPath($InputPath)
	checkedAtUtc = [DateTime]::UtcNow.ToString('o')
	failures = @($failures)
}
$json = $report | ConvertTo-Json -Depth 4
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
	[Console]::Out.WriteLine($json)
} else {
	$parent = Split-Path -Parent $OutputPath
	if (-not [string]::IsNullOrWhiteSpace($parent)) {
		[System.IO.Directory]::CreateDirectory($parent) | Out-Null
	}
	[System.IO.File]::WriteAllText($OutputPath, $json + [Environment]::NewLine)
}

if ($FailOnThresholds -and $failures.Count -ne 0) {
	exit 2
}
