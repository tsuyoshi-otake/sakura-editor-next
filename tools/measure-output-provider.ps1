#requires -Version 5.1
<#!
.SYNOPSIS
  Runs the disabled provider-neutral Output benchmark and analyzes JSONL samples.

.DESCRIPTION
  The caller supplies one C++ and one Rust tests1.exe.  Each interleaved pair
  runs the same deterministic disabled gtest workload against the compile-time
  selected provider.  The script rejects missing or failed runs, validates the
  payload-free schema and semantic digests, checks for surviving descendants,
  and writes median/p95 statistics without adding dependencies.

  The benchmark process is intentionally disabled in normal test runs.  This
  script is the opt-in orchestrator for local and CI evidence collection.
#>
[CmdletBinding()]
param(
  [string]$CppTests1,
  [string]$RustTests1,
  [string]$OutputDirectory = 'build/output-provider-benchmarks',
  [ValidateRange(1, 100)]
  [int]$Pairs = 7,
  [ValidateRange(1, 20)]
  [int]$WarmupBlocks = 2,
  [ValidateRange(1, 100)]
  [int]$MeasuredBlocks = 10,
  [ValidateRange(1, 10000)]
  [int]$SnapshotIterations = 256,
  [ValidateRange(1, 5000)]
  [int]$LifecycleIterations = 512,
  [ValidateRange(1, 3600)]
  [int]$TimeoutSeconds = 120,
  [UInt64]$Seed = 0x27420260827,
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [UInt64]$AffinityMask = 1,
  [ValidateSet('cpp', 'rust')]
  [string]$FirstProvider = 'cpp',
  [ValidateRange(0, 100000)]
  [double]$MaxMedianRegressionPercent = 2.0,
  [ValidateRange(0, 100000)]
  [double]$MaxP95RegressionPercent = 5.0,
  [switch]$CollectOnly,
  [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SchemaVersion = [UInt64]1
$script:BlockNames = @('mutations', 'snapshots', 'advisory-drop', 'callback-stop')
$script:StatusNames = @(
  'succeeded', 'replayed', 'notApplicable', 'rejected',
  'conflict', 'staleRevision', 'revisionExhausted', 'stopped'
)
$script:MinimumAcceptancePairs = 7
$script:MinimumAcceptanceMeasuredBlocks = 10
$script:MinimumAcceptanceMutationOperations = [UInt64]100000
$script:MaximumOperationClassRatio = 2.0
$script:ForbiddenPayloadPropertyPattern =
  '"(?:text|channelId|ownerId|operationId|message|label|projectedText|logEntries|entries|payload)"\s*:'

function Get-JsonProperty {
  param(
    [Parameter(Mandatory = $true)] [object]$Record,
    [Parameter(Mandatory = $true)] [string]$Name,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  $property = $Record.PSObject.Properties[$Name]
  if ($null -eq $property) {
    throw "$Context is missing property '$Name'"
  }
  return $property.Value
}

function Get-JsonString {
  param([object]$Value, [string]$Context)
  if ($Value -isnot [string]) {
    throw "$Context must be a JSON string"
  }
  return [string]$Value
}

function Get-JsonBoolean {
  param([object]$Value, [string]$Context)
  if ($Value -isnot [bool]) {
    throw "$Context must be a JSON boolean"
  }
  return [bool]$Value
}

function Get-JsonUInt64 {
  param([object]$Value, [string]$Context)
  if ($Value -is [bool] -or $null -eq $Value) {
    throw "$Context must be a non-negative integer"
  }
  try {
    $decimal = [decimal]$Value
  }
  catch {
    throw "$Context must be a non-negative integer"
  }
  if ($decimal -lt 0 -or $decimal -ne [decimal]::Truncate($decimal) -or
      $decimal -gt [decimal]([UInt64]::MaxValue)) {
    throw "$Context must be a non-negative integer"
  }
  return [UInt64]$decimal
}

function Assert-EqualValue {
  param([object]$Expected, [object]$Actual, [string]$Context)
  if ($Expected -ne $Actual) {
    throw "$Context expected '$Expected' but got '$Actual'"
  }
}

function Assert-AffinityMaskInput {
  param([Parameter(Mandatory = $true)] [UInt64]$Mask)
  if ($Mask -eq 0) {
    throw '-AffinityMask must be nonzero'
  }
  return $Mask
}

function Get-UpperPercentile {
  param(
    [Parameter(Mandatory = $true)] [double[]]$Values,
    [Parameter(Mandatory = $true)] [double]$Percentile
  )
  if ($Values.Count -eq 0 -or $Percentile -lt 0.0 -or $Percentile -gt 100.0) {
    throw 'percentile input is invalid'
  }
  $ordered = @($Values | Sort-Object)
  $rank = [int][math]::Ceiling(($Percentile / 100.0) * $ordered.Count)
  if ($rank -lt 1) { $rank = 1 }
  if ($rank -gt $ordered.Count) { $rank = $ordered.Count }
  return [double]$ordered[$rank - 1]
}

function Get-Statistics {
  param([Parameter(Mandatory = $true)] [object[]]$Values)
  if ($Values.Count -eq 0) { throw 'statistics require at least one sample' }
  $normalized = @(
    foreach ($value in $Values) {
      $number = [double]$value
      if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le 0.0) {
        throw 'statistics require finite positive samples'
      }
      $number
    }
  )
  $ordered = @($normalized | Sort-Object)
  $middle = [int]($ordered.Count / 2)
  if (($ordered.Count % 2) -eq 0) {
    $median = ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
  }
  else {
    $median = [double]$ordered[$middle]
  }
  return [pscustomobject][ordered]@{
    count = [int]$ordered.Count
    median = [double]$median
    p95 = [double](Get-UpperPercentile -Values $ordered -Percentile 95.0)
    minimum = [double]$ordered[0]
    maximum = [double]$ordered[$ordered.Count - 1]
  }
}

function Test-PerformanceGate {
  param(
    [Parameter(Mandatory = $true)] [double]$CppMedian,
    [Parameter(Mandatory = $true)] [double]$RustMedian,
    [Parameter(Mandatory = $true)] [double]$CppP95,
    [Parameter(Mandatory = $true)] [double]$RustP95,
    [Parameter(Mandatory = $true)] [double]$MaximumMedianRegressionPercent,
    [Parameter(Mandatory = $true)] [double]$MaximumP95RegressionPercent
  )
  if ($CppMedian -le 0.0 -or $CppP95 -le 0.0 -or $RustMedian -le 0.0 -or $RustP95 -le 0.0) {
    throw 'performance gate requires finite positive statistics'
  }
  $medianRatio = $RustMedian / $CppMedian
  $p95Ratio = $RustP95 / $CppP95
  $medianRegression = ($medianRatio - 1.0) * 100.0
  $p95Regression = ($p95Ratio - 1.0) * 100.0
  return [pscustomobject][ordered]@{
    medianRatio = [double]$medianRatio
    p95Ratio = [double]$p95Ratio
    medianRegressionPercent = [double]$medianRegression
    p95RegressionPercent = [double]$p95Regression
    pass = ($medianRegression -le $MaximumMedianRegressionPercent -and
      $p95Regression -le $MaximumP95RegressionPercent -and
      $medianRatio -le $script:MaximumOperationClassRatio -and
      $p95Ratio -le $script:MaximumOperationClassRatio)
  }
}

function Get-AcceptanceFailures {
  param(
    [Parameter(Mandatory = $true)] [int]$PairCount,
    [Parameter(Mandatory = $true)] [int]$MeasuredCount,
    [Parameter(Mandatory = $true)] [UInt64]$CppOperations,
    [Parameter(Mandatory = $true)] [UInt64]$RustOperations
  )
  $failures = New-Object System.Collections.Generic.List[string]
  if ($PairCount -lt $script:MinimumAcceptancePairs) {
    [void]$failures.Add("process pairs $PairCount is below $script:MinimumAcceptancePairs")
  }
  if ($MeasuredCount -lt $script:MinimumAcceptanceMeasuredBlocks) {
    [void]$failures.Add("timed measured blocks $MeasuredCount is below $script:MinimumAcceptanceMeasuredBlocks")
  }
  if ($CppOperations -lt $script:MinimumAcceptanceMutationOperations) {
    [void]$failures.Add("C++ timed mutation operations $CppOperations is below $script:MinimumAcceptanceMutationOperations")
  }
  if ($RustOperations -lt $script:MinimumAcceptanceMutationOperations) {
    [void]$failures.Add("Rust timed mutation operations $RustOperations is below $script:MinimumAcceptanceMutationOperations")
  }
  return $failures.ToArray()
}

function Invoke-SelfTest {
  [void](Assert-AffinityMaskInput -Mask $AffinityMask)
  $statistics = Get-Statistics -Values @([double]4, 1, 9, 3, 2)
  Assert-EqualValue 5 $statistics.count 'odd statistics count'
  Assert-EqualValue 3.0 $statistics.median 'odd statistics median'
  Assert-EqualValue 9.0 $statistics.p95 'odd statistics p95'

  $even = Get-Statistics -Values @([double]1, 2, 3, 4)
  Assert-EqualValue 2.5 $even.median 'even statistics median'
  Assert-EqualValue 4.0 $even.p95 'even statistics p95'

  $json = '{"schemaVersion":1,"record":"metadata","payloadFree":true}'
  if ($json -match $script:ForbiddenPayloadPropertyPattern) {
    throw 'payload-free self-test unexpectedly matched a forbidden property'
  }
  $payload = '{"schemaVersion":1,"record":"sample","text":"secret"}'
  if ($payload -notmatch $script:ForbiddenPayloadPropertyPattern) {
    throw 'payload rejection self-test did not match a forbidden property'
  }
  $syntheticAncestry = @(
    [pscustomobject][ordered]@{ processId = 901; parentId = 900; depth = 1; sequence = 1 }
    [pscustomobject][ordered]@{ processId = 900; parentId = 0; depth = 0; sequence = 0 }
    [pscustomobject][ordered]@{ processId = 12; parentId = 901; depth = 2; sequence = 2 }
  )
  $orderedAncestry = @(Get-ParentFirstProcessOrder -Ancestry $syntheticAncestry)
  Assert-EqualValue 900 $orderedAncestry[0].processId 'parent-first process cleanup root'
  Assert-EqualValue 901 $orderedAncestry[1].processId 'parent-first process cleanup child'
  Assert-EqualValue 12 $orderedAncestry[2].processId 'parent-first process cleanup grandchild'
  $liveAncestry = @(Get-ProcessAncestryWithIdentity -RootProcessId $PID)
  $liveRoot = @($liveAncestry | Where-Object { [int]$_.processId -eq $PID })
  Assert-EqualValue 1 $liveRoot.Count 'live process ancestry root count'
  if ([string]::IsNullOrWhiteSpace([string]$liveRoot[0].creationDate)) {
    throw 'live process ancestry root has no creation identity'
  }
  $gatePass = Test-PerformanceGate -CppMedian 100.0 -RustMedian 101.0 -CppP95 200.0 -RustP95 209.0 `
    -MaximumMedianRegressionPercent 2.0 -MaximumP95RegressionPercent 5.0
  if (-not $gatePass.pass) { throw 'performance gate accepted an in-budget sample' }
  $gateRejectMedian = Test-PerformanceGate -CppMedian 100.0 -RustMedian 103.0 -CppP95 200.0 -RustP95 209.0 `
    -MaximumMedianRegressionPercent 2.0 -MaximumP95RegressionPercent 5.0
  if ($gateRejectMedian.pass) { throw 'performance gate accepted a median regression' }
  $gateRejectP95 = Test-PerformanceGate -CppMedian 100.0 -RustMedian 101.0 -CppP95 200.0 -RustP95 212.0 `
    -MaximumMedianRegressionPercent 2.0 -MaximumP95RegressionPercent 5.0
  if ($gateRejectP95.pass) { throw 'performance gate accepted a p95 regression' }
  $gateRejectRatio = Test-PerformanceGate -CppMedian 100.0 -RustMedian 201.0 -CppP95 200.0 -RustP95 201.0 `
    -MaximumMedianRegressionPercent 1000.0 -MaximumP95RegressionPercent 1000.0
  if ($gateRejectRatio.pass) { throw 'performance gate accepted a two-times class ratio' }
  $smokeFailures = @(Get-AcceptanceFailures -PairCount 1 -MeasuredCount 1 -CppOperations 23 -RustOperations 23)
  if ($smokeFailures.Count -ne 4) { throw 'acceptance self-test did not reject smoke evidence' }
  $fullFailures = @(Get-AcceptanceFailures -PairCount 7 -MeasuredCount 10 -CppOperations 100000 -RustOperations 100000)
  if ($fullFailures.Count -ne 0) { throw 'acceptance self-test rejected complete evidence' }
  Assert-EqualValue '1' (([UInt64]1).ToString([Globalization.CultureInfo]::InvariantCulture)) 'default affinity mask decimal representation'
  Write-Output 'PASS measure-output-provider.ps1 self-tests'
}

function Resolve-RequiredFile {
  param([string]$Path, [string]$Name)
  if ([string]::IsNullOrWhiteSpace($Path)) {
    throw "$Name is required"
  }
  try {
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
  }
  catch {
    throw "$Name does not exist: $Path"
  }
  $fullPath = [IO.Path]::GetFullPath($resolved.Path)
  if (-not [IO.File]::Exists($fullPath)) {
    throw "$Name is not a file: $fullPath"
  }
  return $fullPath
}

function Get-ExecutableMetadata {
  param([Parameter(Mandatory = $true)] [string]$Path)
  $item = Get-Item -LiteralPath $Path -ErrorAction Stop
  if ($item -isnot [IO.FileInfo] -or -not $item.Exists) {
    throw "executable is not a regular file: $Path"
  }
  $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant()
  if ([string]::IsNullOrWhiteSpace($hash)) {
    throw "executable SHA-256 is empty: $Path"
  }
  return [pscustomobject][ordered]@{
    path = [IO.Path]::GetFullPath($Path)
    sha256 = $hash
    sizeBytes = [UInt64]$item.Length
  }
}

function Assert-ExecutableUnchanged {
  param([Parameter(Mandatory = $true)] [object]$Expected)
  $actual = Get-ExecutableMetadata -Path $Expected.path
  Assert-EqualValue $Expected.sha256 $actual.sha256 'executable SHA-256 changed during measurement'
  Assert-EqualValue $Expected.sizeBytes $actual.sizeBytes 'executable size changed during measurement'
}

function Get-PlatformMetadata {
  $osArchitecture = 'unknown'
  try {
    $osArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  }
  catch { }
  $cpuManufacturer = 'unknown'
  $cpuModel = 'unknown'
  $physicalCores = [UInt64]0
  $logicalProcessors = [UInt64][Environment]::ProcessorCount
  $maxClockMHz = [UInt64]0
  try {
    $processor = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop) | Select-Object -First 1
    if ($null -ne $processor) {
      if (-not [string]::IsNullOrWhiteSpace([string]$processor.Manufacturer)) {
        $cpuManufacturer = [string]$processor.Manufacturer
      }
      if (-not [string]::IsNullOrWhiteSpace([string]$processor.Name)) {
        $cpuModel = [string]$processor.Name
      }
      if ($null -ne $processor.NumberOfCores) {
        $physicalCores = [UInt64]$processor.NumberOfCores
      }
      if ($null -ne $processor.NumberOfLogicalProcessors) {
        $logicalProcessors = [UInt64]$processor.NumberOfLogicalProcessors
      }
      if ($null -ne $processor.MaxClockSpeed) {
        $maxClockMHz = [UInt64]$processor.MaxClockSpeed
      }
    }
  }
  catch { }
  return [pscustomobject][ordered]@{
    os = [pscustomobject][ordered]@{
      platform = 'Windows'
      version = [Environment]::OSVersion.Version.ToString()
      architecture = $osArchitecture
    }
    cpu = [pscustomobject][ordered]@{
      manufacturer = $cpuManufacturer
      model = $cpuModel
      physicalCores = $physicalCores
      logicalProcessors = $logicalProcessors
      maxClockMHz = $maxClockMHz
    }
  }
}

function Get-ProcessAncestry {
  param([Parameter(Mandatory = $true)] [int]$RootProcessId)
  $rows = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)
  $children = @{}
  $creationDates = @{}
  foreach ($row in $rows) {
    $processId = [int]$row.ProcessId
    $creationDates[$processId] = Get-CreationDateKey -Value $row.CreationDate
    $parent = [int]$row.ParentProcessId
    if (-not $children.ContainsKey($parent)) {
      $children[$parent] = New-Object System.Collections.Generic.List[object]
    }
    [void]$children[$parent].Add([pscustomobject][ordered]@{
      processId = [int]$row.ProcessId
      parentId = $parent
      creationDate = Get-CreationDateKey -Value $row.CreationDate
    })
  }
  $found = New-Object System.Collections.Generic.List[object]
  $pending = New-Object System.Collections.Generic.Queue[object]
  $rootCreationDate = ''
  if ($creationDates.ContainsKey($RootProcessId)) {
    $rootCreationDate = [string]$creationDates[$RootProcessId]
  }
  $pending.Enqueue([pscustomobject][ordered]@{
    processId = $RootProcessId
    parentId = 0
    creationDate = $rootCreationDate
    depth = 0
    sequence = 0
  })
  $seen = @{}
  $sequence = 0
  while ($pending.Count -gt 0) {
    $current = $pending.Dequeue()
    $processId = [int]$current.processId
    if ($seen.ContainsKey($processId)) { continue }
    $seen[$processId] = $true
    [void]$found.Add($current)
    $parent = $processId
    if (-not $children.ContainsKey($parent)) { continue }
    foreach ($child in $children[$parent]) {
      if ($seen.ContainsKey([int]$child.processId)) { continue }
      ++$sequence
      $pending.Enqueue([pscustomobject][ordered]@{
        processId = [int]$child.processId
        parentId = [int]$child.parentId
        creationDate = [string]$child.creationDate
        depth = [int]$current.depth + 1
        sequence = $sequence
      })
    }
  }
  # PowerShell 7 can throw "Argument types do not match" when the array
  # subexpression operator is applied directly to List[object]. Materialize the
  # generic list explicitly; callers already wrap the function result in @().
  return $found.ToArray()
}

function Get-ParentFirstProcessOrder {
  param([Parameter(Mandatory = $true)] [object[]]$Ancestry)
  return @($Ancestry | Sort-Object -Property depth, sequence)
}

function Get-CreationDateKey {
  param([Parameter(Mandatory = $true)] [object]$Value)
  try {
    return ([DateTime]$Value).ToUniversalTime().ToString('o', [Globalization.CultureInfo]::InvariantCulture)
  }
  catch {
    return [string]$Value
  }
}

function Get-ProcessAncestryWithIdentity {
  param([Parameter(Mandatory = $true)] [int]$RootProcessId)
  $lastError = $null
  for ($attempt = 0; $attempt -lt 20; ++$attempt) {
    try {
      $candidate = @(Get-ProcessAncestry -RootProcessId $RootProcessId)
      $root = @($candidate | Where-Object { [int]$_.processId -eq $RootProcessId })
      if ($root.Count -eq 1 -and -not [string]::IsNullOrWhiteSpace([string]$root[0].creationDate)) {
        return $candidate
      }
    }
    catch {
      $lastError = $_.Exception
    }
    if ($attempt -lt 19) { Start-Sleep -Milliseconds 25 }
  }
  if ($null -ne $lastError) {
    throw "could not capture benchmark process ancestry: $($lastError.Message)"
  }
  throw "could not capture benchmark process creation identity"
}

function Get-VerifiedProcessAncestry {
  param(
    [Parameter(Mandatory = $true)] [int]$RootProcessId,
    [Parameter(Mandatory = $true)] [object[]]$CapturedAncestry
  )
  $capturedRoot = @($CapturedAncestry | Where-Object { [int]$_.processId -eq $RootProcessId })
  if ($capturedRoot.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$capturedRoot[0].creationDate)) {
    throw "benchmark process ancestry has no captured root identity"
  }
  $current = @(Get-ProcessAncestryWithIdentity -RootProcessId $RootProcessId)
  $currentRoot = @($current | Where-Object { [int]$_.processId -eq $RootProcessId })
  if ($currentRoot.Count -ne 1 -or
      [string]$currentRoot[0].creationDate -ne [string]$capturedRoot[0].creationDate) {
    throw "benchmark process identity changed before cleanup"
  }
  return $current
}

function Test-ProcessAlive {
  param([Parameter(Mandatory = $true)] [int]$ProcessId)
  try {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    return -not $process.HasExited
  }
  catch {
    return $false
  }
}

function Test-CapturedProcessIdentity {
  param([Parameter(Mandatory = $true)] [object]$Entry)
  $creationDate = [string]$Entry.creationDate
  if ([string]::IsNullOrWhiteSpace($creationDate)) { return $false }
  try {
    $filter = "ProcessId = $([int]$Entry.processId)"
    $rows = @(Get-CimInstance -ClassName Win32_Process -Filter $filter -ErrorAction Stop)
    return $rows.Count -eq 1 -and (Get-CreationDateKey -Value $rows[0].CreationDate) -eq $creationDate
  }
  catch {
    return $false
  }
}

function Test-CapturedProcessAlive {
  param([Parameter(Mandatory = $true)] [object]$Entry)
  if (-not (Test-CapturedProcessIdentity -Entry $Entry)) { return $false }
  return Test-ProcessAlive -ProcessId ([int]$Entry.processId)
}

function Get-LiveProcessAncestry {
  param(
    [Parameter(Mandatory = $true)] [int]$RootProcessId,
    [object[]]$CapturedAncestry
  )
  $usingCapturedAncestry = $null -ne $CapturedAncestry
  $ancestry = if ($usingCapturedAncestry) {
    @($CapturedAncestry)
  }
  else {
    @(Get-ProcessAncestry -RootProcessId $RootProcessId)
  }
  if (-not $usingCapturedAncestry) {
    $root = @($ancestry | Where-Object { [int]$_.processId -eq $RootProcessId })
    if ($root.Count -ne 1 -or -not (Test-CapturedProcessIdentity -Entry $root[0])) {
      return @()
    }
  }
  return @(
    $ancestry | Where-Object { Test-CapturedProcessAlive -Entry $_ }
  )
}

function Stop-ExactProcessTree {
  param(
    [Parameter(Mandatory = $true)] [int]$RootProcessId,
    [object[]]$CapturedAncestry
  )
  $usingCapturedAncestry = $null -ne $CapturedAncestry
  $ancestry = if ($usingCapturedAncestry) {
    @($CapturedAncestry)
  }
  else {
    @(Get-ProcessAncestry -RootProcessId $RootProcessId)
  }
  if (-not $usingCapturedAncestry) {
    $root = @($ancestry | Where-Object { [int]$_.processId -eq $RootProcessId })
    if ($root.Count -ne 1 -or -not (Test-CapturedProcessIdentity -Entry $root[0])) {
      return
    }
  }
  $orderedAncestry = @(Get-ParentFirstProcessOrder -Ancestry $ancestry)
  foreach ($entry in $orderedAncestry) {
    $processId = [int]$entry.processId
    if (-not (Test-CapturedProcessIdentity -Entry $entry)) { continue }
    try {
      $process = Get-Process -Id $processId -ErrorAction Stop
      if (-not $process.HasExited) {
        try { $process.Kill($true) }
        catch { $process.Kill() }
      }
    }
    catch {
      if ((Test-CapturedProcessIdentity -Entry $entry) -and (Test-ProcessAlive -ProcessId $processId)) {
        throw "failed to stop exact benchmark process $processId"
      }
    }
  }

  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  do {
    $survivors = @(
      $ancestry | Where-Object { Test-CapturedProcessAlive -Entry $_ }
    )
    if ($survivors.Count -eq 0) { return }
    foreach ($entry in (Get-ParentFirstProcessOrder -Ancestry $survivors)) {
      $processId = [int]$entry.processId
      try {
        $process = Get-Process -Id $processId -ErrorAction Stop
        try { $process.Kill($true) }
        catch { $process.Kill() }
      }
      catch { }
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "benchmark process tree survived bounded cleanup: $(($survivors | ForEach-Object { $_.processId }) -join ',')"
}

function Add-ProcessArguments {
  param(
    [Parameter(Mandatory = $true)] [Diagnostics.ProcessStartInfo]$StartInfo,
    [Parameter(Mandatory = $true)] [string[]]$Arguments
  )
  $argumentListProperty = $StartInfo.PSObject.Properties['ArgumentList']
  if ($null -ne $argumentListProperty) {
    foreach ($argument in $Arguments) { [void]$StartInfo.ArgumentList.Add($argument) }
    return
  }
  $quoted = foreach ($argument in $Arguments) {
    '"' + $argument.Replace('"', '\"') + '"'
  }
  $StartInfo.Arguments = $quoted -join ' '
}

function Invoke-BenchmarkProcess {
  param(
    [Parameter(Mandatory = $true)] [string]$Executable,
    [Parameter(Mandatory = $true)] [object]$ExecutableMetadata,
    [Parameter(Mandatory = $true)] [string]$Provider,
    [Parameter(Mandatory = $true)] [int]$PairIndex,
    [Parameter(Mandatory = $true)] [string]$RunDirectory
  )
  $currentExecutable = Get-ExecutableMetadata -Path $Executable
  Assert-EqualValue $ExecutableMetadata.sha256 $currentExecutable.sha256 "$Provider executable SHA-256 changed before pair $PairIndex"
  Assert-EqualValue $ExecutableMetadata.sizeBytes $currentExecutable.sizeBytes "$Provider executable size changed before pair $PairIndex"
  $rawPath = Join-Path $RunDirectory ("pair-{0:D3}-{1}.jsonl" -f $PairIndex, $Provider)
  $stdoutPath = Join-Path $RunDirectory ("pair-{0:D3}-{1}.stdout.txt" -f $PairIndex, $Provider)
  $stderrPath = Join-Path $RunDirectory ("pair-{0:D3}-{1}.stderr.txt" -f $PairIndex, $Provider)
  $startInfo = New-Object Diagnostics.ProcessStartInfo
  $startInfo.FileName = $Executable
  $startInfo.WorkingDirectory = Split-Path -Parent $Executable
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  Add-ProcessArguments -StartInfo $startInfo -Arguments @(
    '--gtest_also_run_disabled_tests',
    '--gtest_filter=OutputServiceProviderMeasurement.DISABLED_CompileSelectedProviderWorkload',
    '--gtest_color=no'
  )
  $environment = @{
    SAKURA_OUTPUT_BENCHMARK_OUTPUT = $rawPath
    SAKURA_OUTPUT_BENCHMARK_SEED = [string]$Seed
    SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK = ([UInt64]$AffinityMask).ToString([Globalization.CultureInfo]::InvariantCulture)
    SAKURA_OUTPUT_BENCHMARK_WARMUP_BLOCKS = [string]$WarmupBlocks
    SAKURA_OUTPUT_BENCHMARK_BLOCKS = [string]$MeasuredBlocks
    SAKURA_OUTPUT_BENCHMARK_SNAPSHOT_ITERATIONS = [string]$SnapshotIterations
    SAKURA_OUTPUT_BENCHMARK_LIFECYCLES = [string]$LifecycleIterations
    SAKURA_OUTPUT_BENCHMARK_CONFIGURATION = $Configuration
    SAKURA_OUTPUT_BENCHMARK_EXPECTED_PROVIDER = $Provider
  }
  foreach ($entry in $environment.GetEnumerator()) {
    $startInfo.EnvironmentVariables[$entry.Key] = $entry.Value
  }

  $process = New-Object Diagnostics.Process
  $process.StartInfo = $startInfo
  $started = $false
  $stdoutTask = $null
  $stderrTask = $null
  $exitCode = $null
  $capturedAncestry = $null
  try {
    if (-not $process.Start()) { throw "failed to start $Provider benchmark executable" }
    $started = $true
    $capturedAncestry = @(Get-ProcessAncestryWithIdentity -RootProcessId $process.Id)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
      $timeoutAncestry = @(Get-VerifiedProcessAncestry -RootProcessId $process.Id -CapturedAncestry $capturedAncestry)
      Stop-ExactProcessTree -RootProcessId $process.Id -CapturedAncestry $timeoutAncestry
      throw "$Provider benchmark pair $PairIndex exceeded ${TimeoutSeconds}s timeout"
    }
    $process.WaitForExit()
    $exitCode = [int]$process.ExitCode
    $stdout = [string]$stdoutTask.GetAwaiter().GetResult()
    $stderr = [string]$stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($stdoutPath, $stdout, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText($stderrPath, $stderr, [Text.Encoding]::UTF8)
    if ($exitCode -ne 0) {
      throw "$Provider benchmark pair $PairIndex exited with code $exitCode; see $stderrPath"
    }
    $survivors = @(Get-LiveProcessAncestry -RootProcessId $process.Id -CapturedAncestry $capturedAncestry)
    if ($survivors.Count -ne 0) {
      Stop-ExactProcessTree -RootProcessId $process.Id -CapturedAncestry $capturedAncestry
      throw "$Provider benchmark pair $PairIndex left surviving process IDs: $(($survivors | ForEach-Object { $_.processId }) -join ',')"
    }
    return [pscustomobject][ordered]@{
      pairIndex = $PairIndex
      provider = $Provider
      executableSha256 = $ExecutableMetadata.sha256
      executableSizeBytes = $ExecutableMetadata.sizeBytes
      rawPath = $rawPath
      stdoutPath = $stdoutPath
      stderrPath = $stderrPath
      exitCode = $exitCode
    }
  }
  finally {
    if ($started) {
      if ($null -ne $capturedAncestry) {
        $live = @(Get-LiveProcessAncestry -RootProcessId $process.Id -CapturedAncestry $capturedAncestry)
        if ($live.Count -ne 0) {
          Stop-ExactProcessTree -RootProcessId $process.Id -CapturedAncestry $capturedAncestry
        }
      }
      elseif (-not $process.HasExited) {
        Stop-ExactProcessTree -RootProcessId $process.Id
      }
    }
    $process.Dispose()
  }
}

function Read-BenchmarkRun {
  param(
    [Parameter(Mandatory = $true)] [string]$Path,
    [Parameter(Mandatory = $true)] [string]$ExpectedProvider,
    [Parameter(Mandatory = $true)] [int]$PairIndex
  )
  if (-not [IO.File]::Exists($Path)) { throw "benchmark output is missing: $Path" }
  $lines = [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)
  if ($lines.Count -eq 0) { throw "benchmark output is empty: $Path" }
  $metadata = $null
  $summary = $null
  $samples = New-Object System.Collections.Generic.List[object]
  $sampleKeys = @{}
  $lineNumber = 0
  foreach ($line in $lines) {
    ++$lineNumber
    if ([string]::IsNullOrWhiteSpace($line)) {
      throw "$Path line $lineNumber is blank"
    }
    if ($line -match $script:ForbiddenPayloadPropertyPattern) {
      throw "$Path line $lineNumber contains a payload property"
    }
    try { $record = $line | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "$Path line $lineNumber is not valid JSON: $($_.Exception.Message)" }
    $schema = Get-JsonUInt64 (Get-JsonProperty $record 'schemaVersion' "$Path line $lineNumber") "$Path line $lineNumber schemaVersion"
    Assert-EqualValue $script:SchemaVersion $schema "$Path line $lineNumber schemaVersion"
    $recordType = Get-JsonString (Get-JsonProperty $record 'record' "$Path line $lineNumber") "$Path line $lineNumber record"
    switch ($recordType) {
      'metadata' {
        if ($null -ne $metadata) { throw "$Path contains duplicate metadata records" }
        $provider = Get-JsonString (Get-JsonProperty $record 'provider' "$Path metadata") "$Path metadata provider"
        Assert-EqualValue $ExpectedProvider $provider "$Path metadata provider"
        $configuration = Get-JsonString (Get-JsonProperty $record 'configuration' "$Path metadata") "$Path metadata configuration"
        Assert-EqualValue $Configuration $configuration "$Path metadata configuration"
        $seed = Get-JsonUInt64 (Get-JsonProperty $record 'seed' "$Path metadata") "$Path metadata seed"
        Assert-EqualValue $Seed $seed "$Path metadata seed"
        $affinityMask = Get-JsonUInt64 (Get-JsonProperty $record 'affinityMask' "$Path metadata") "$Path metadata affinityMask"
        Assert-EqualValue ([UInt64]$AffinityMask) $affinityMask "$Path metadata affinityMask"
        if ($affinityMask -eq 0) { throw "$Path metadata affinityMask must be nonzero" }
        $warmups = Get-JsonUInt64 (Get-JsonProperty $record 'warmupBlocks' "$Path metadata") "$Path metadata warmupBlocks"
        $measured = Get-JsonUInt64 (Get-JsonProperty $record 'measuredBlocks' "$Path metadata") "$Path metadata measuredBlocks"
        $iterations = Get-JsonUInt64 (Get-JsonProperty $record 'snapshotIterations' "$Path metadata") "$Path metadata snapshotIterations"
        $lifecycles = Get-JsonUInt64 (Get-JsonProperty $record 'lifecycleIterations' "$Path metadata") "$Path metadata lifecycleIterations"
        Assert-EqualValue ([UInt64]$WarmupBlocks) $warmups "$Path metadata warmupBlocks"
        Assert-EqualValue ([UInt64]$MeasuredBlocks) $measured "$Path metadata measuredBlocks"
        Assert-EqualValue ([UInt64]$SnapshotIterations) $iterations "$Path metadata snapshotIterations"
        Assert-EqualValue ([UInt64]$LifecycleIterations) $lifecycles "$Path metadata lifecycleIterations"
        $traceOperations = Get-JsonUInt64 (Get-JsonProperty $record 'traceOperationCount' "$Path metadata") "$Path metadata traceOperationCount"
        if ($traceOperations -eq 0) { throw "$Path metadata traceOperationCount must be positive" }
        $frequency = Get-JsonUInt64 (Get-JsonProperty $record 'qpcFrequency' "$Path metadata") "$Path metadata qpcFrequency"
        if ($frequency -eq 0) { throw "$Path metadata qpcFrequency must be positive" }
        $payloadFree = Get-JsonBoolean (Get-JsonProperty $record 'payloadFree' "$Path metadata") "$Path metadata payloadFree"
        if (-not $payloadFree) { throw "$Path metadata payloadFree must be true" }
        $metadata = [pscustomobject][ordered]@{
          provider = $provider
          configuration = $configuration
          seed = $seed
          affinityMask = $affinityMask
          traceDigest = Get-JsonUInt64 (Get-JsonProperty $record 'traceDigest' "$Path metadata") "$Path metadata traceDigest"
          traceOperationCount = $traceOperations
          lifecycleIterations = $lifecycles
          qpcFrequency = $frequency
        }
      }
      'sample' {
        if ($null -eq $metadata) { throw "$Path sample precedes metadata" }
        $provider = Get-JsonString (Get-JsonProperty $record 'provider' "$Path sample") "$Path sample provider"
        Assert-EqualValue $metadata.provider $provider "$Path sample provider"
        $configuration = Get-JsonString (Get-JsonProperty $record 'configuration' "$Path sample") "$Path sample configuration"
        Assert-EqualValue $metadata.configuration $configuration "$Path sample configuration"
        $seed = Get-JsonUInt64 (Get-JsonProperty $record 'seed' "$Path sample") "$Path sample seed"
        Assert-EqualValue $metadata.seed $seed "$Path sample seed"
        $affinityMask = Get-JsonUInt64 (Get-JsonProperty $record 'affinityMask' "$Path sample") "$Path sample affinityMask"
        Assert-EqualValue $metadata.affinityMask $affinityMask "$Path sample affinityMask"
        $traceDigest = Get-JsonUInt64 (Get-JsonProperty $record 'traceDigest' "$Path sample") "$Path sample traceDigest"
        Assert-EqualValue $metadata.traceDigest $traceDigest "$Path sample traceDigest"
        $block = Get-JsonString (Get-JsonProperty $record 'block' "$Path sample") "$Path sample block"
        if ($script:BlockNames -notcontains $block) { throw "$Path sample has unknown block '$block'" }
        $warmup = Get-JsonBoolean (Get-JsonProperty $record 'warmup' "$Path sample") "$Path sample warmup"
        $blockIndex = Get-JsonUInt64 (Get-JsonProperty $record 'blockIndex' "$Path sample") "$Path sample blockIndex"
        if ($blockIndex -ge ([UInt64]($WarmupBlocks + $MeasuredBlocks))) {
          throw "$Path sample blockIndex is outside configured bounds"
        }
        $expectedWarmup = $blockIndex -lt ([UInt64]$WarmupBlocks)
        if ($warmup -ne $expectedWarmup) { throw "$Path sample warmup does not match blockIndex" }
        $key = "$warmup|$block|$blockIndex"
        if ($sampleKeys.ContainsKey($key)) { throw "$Path contains duplicate sample '$key'" }
        $sampleKeys[$key] = $true
        $begin = Get-JsonUInt64 (Get-JsonProperty $record 'beginQpc' "$Path sample") "$Path sample beginQpc"
        $end = Get-JsonUInt64 (Get-JsonProperty $record 'endQpc' "$Path sample") "$Path sample endQpc"
        $frequency = Get-JsonUInt64 (Get-JsonProperty $record 'qpcFrequency' "$Path sample") "$Path sample qpcFrequency"
        $operations = Get-JsonUInt64 (Get-JsonProperty $record 'operations' "$Path sample") "$Path sample operations"
        if ($end -lt $begin -or $frequency -eq 0 -or $operations -eq 0) {
          throw "$Path sample has invalid QPC bounds or operation count"
        }
        $elapsed = ([double]$end - [double]$begin) * 1000000000.0 / [double]$frequency
        $nsPerOperation = $elapsed / [double]$operations
        if ([double]::IsNaN($nsPerOperation) -or [double]::IsInfinity($nsPerOperation) -or $nsPerOperation -le 0.0) {
          throw "$Path sample has a non-positive duration"
        }
        $statusCounts = [ordered]@{}
        $statusTotal = [UInt64]0
        foreach ($statusName in $script:StatusNames) {
          $status = Get-JsonUInt64 (Get-JsonProperty $record $statusName "$Path sample") "$Path sample $statusName"
          $statusCounts[$statusName] = $status
          $statusTotal += $status
        }
        $setupExcluded = Get-JsonBoolean (Get-JsonProperty $record 'setupExcluded' "$Path sample") "$Path sample setupExcluded"
        if (-not $setupExcluded) { throw "$Path sample setupExcluded must be true" }
        [void]$samples.Add([pscustomobject][ordered]@{
          pairIndex = $PairIndex
          provider = $provider
          block = $block
          warmup = $warmup
          blockIndex = $blockIndex
          affinityMask = $affinityMask
          beginQpc = $begin
          endQpc = $end
          qpcFrequency = $frequency
          operations = $operations
          nsPerOperation = $nsPerOperation
          resultDigest = Get-JsonUInt64 (Get-JsonProperty $record 'resultDigest' "$Path sample") "$Path sample resultDigest"
          snapshotDigest = Get-JsonUInt64 (Get-JsonProperty $record 'snapshotDigest' "$Path sample") "$Path sample snapshotDigest"
          statusCounts = $statusCounts
          statusTotal = $statusTotal
          callbacks = Get-JsonUInt64 (Get-JsonProperty $record 'callbacks' "$Path sample") "$Path sample callbacks"
          maximumCallbackDepth = Get-JsonUInt64 (Get-JsonProperty $record 'maximumCallbackDepth' "$Path sample") "$Path sample maximumCallbackDepth"
          droppedNotificationCount = Get-JsonUInt64 (Get-JsonProperty $record 'droppedNotificationCount' "$Path sample") "$Path sample droppedNotificationCount"
        })
      }
      'summary' {
        if ($null -ne $summary) { throw "$Path contains duplicate summary records" }
        if ($null -eq $metadata) { throw "$Path summary precedes metadata" }
        $provider = Get-JsonString (Get-JsonProperty $record 'provider' "$Path summary") "$Path summary provider"
        Assert-EqualValue $metadata.provider $provider "$Path summary provider"
        $configuration = Get-JsonString (Get-JsonProperty $record 'configuration' "$Path summary") "$Path summary configuration"
        Assert-EqualValue $metadata.configuration $configuration "$Path summary configuration"
        $seed = Get-JsonUInt64 (Get-JsonProperty $record 'seed' "$Path summary") "$Path summary seed"
        Assert-EqualValue $metadata.seed $seed "$Path summary seed"
        $affinityMask = Get-JsonUInt64 (Get-JsonProperty $record 'affinityMask' "$Path summary") "$Path summary affinityMask"
        Assert-EqualValue $metadata.affinityMask $affinityMask "$Path summary affinityMask"
        $traceDigest = Get-JsonUInt64 (Get-JsonProperty $record 'traceDigest' "$Path summary") "$Path summary traceDigest"
        Assert-EqualValue $metadata.traceDigest $traceDigest "$Path summary traceDigest"
        $completed = Get-JsonBoolean (Get-JsonProperty $record 'completed' "$Path summary") "$Path summary completed"
        if (-not $completed) { throw "$Path summary completed must be true" }
        $payloadFree = Get-JsonBoolean (Get-JsonProperty $record 'payloadFree' "$Path summary") "$Path summary payloadFree"
        if (-not $payloadFree) { throw "$Path summary payloadFree must be true" }
        $summary = [pscustomobject][ordered]@{
          sampleCount = Get-JsonUInt64 (Get-JsonProperty $record 'sampleCount' "$Path summary") "$Path summary sampleCount"
        }
      }
      default { throw "$Path line $lineNumber has unknown record '$recordType'" }
    }
  }
  if ($null -eq $metadata -or $null -eq $summary) { throw "$Path must contain metadata and summary" }
  $expectedCount = [UInt64](($WarmupBlocks + $MeasuredBlocks) * $script:BlockNames.Count)
  Assert-EqualValue $expectedCount ([UInt64]$samples.Count) "$Path sample count"
  Assert-EqualValue $expectedCount $summary.sampleCount "$Path summary sampleCount"
  foreach ($warmup in @($true, $false)) {
    $expectedBlocks = if ($warmup) { $WarmupBlocks } else { $MeasuredBlocks }
    foreach ($block in $script:BlockNames) {
      $actual = @($samples | Where-Object { $_.warmup -eq $warmup -and $_.block -eq $block })
      Assert-EqualValue $expectedBlocks $actual.Count "$Path $block sample count warmup=$warmup"
      foreach ($sample in $actual) {
        $expectedOperations = switch ($block) {
          'mutations' { [UInt64]($metadata.traceOperationCount * $metadata.lifecycleIterations) }
          'snapshots' { [UInt64]$SnapshotIterations }
          'advisory-drop' { [UInt64]2 }
          'callback-stop' { [UInt64]1 }
        }
        Assert-EqualValue $expectedOperations $sample.operations "$Path $block operations"
        if ($block -eq 'mutations' -and $sample.statusTotal -ne ($metadata.traceOperationCount * $metadata.lifecycleIterations)) {
          throw "$Path mutation status count does not cover the complete trace"
        }
        if ($block -eq 'snapshots' -and $sample.statusTotal -ne 0) {
          throw "$Path snapshot block unexpectedly reported mutation statuses"
        }
        if ($block -eq 'mutations' -and -not $warmup) {
          foreach ($requiredStatus in @('succeeded', 'replayed', 'notApplicable', 'rejected', 'conflict', 'staleRevision', 'stopped')) {
            if ($sample.statusCounts[$requiredStatus] -eq 0) {
              throw "$Path mutation block is missing required scenario status '$requiredStatus'"
            }
          }
        }
        if ($block -eq 'advisory-drop' -and -not $warmup -and $sample.droppedNotificationCount -eq 0) {
          throw "$Path advisory-drop block did not report a dropped notification"
        }
        if ($block -eq 'callback-stop' -and -not $warmup -and $sample.callbacks -eq 0) {
          throw "$Path callback-stop block did not invoke a listener"
        }
      }
    }
  }
  return [pscustomobject][ordered]@{
    pairIndex = $PairIndex
    provider = $ExpectedProvider
    path = $Path
    metadata = $metadata
    samples = $samples.ToArray()
  }
}

function Get-SampleKey {
  param([object]$Sample)
  return "$($Sample.warmup)|$($Sample.block)|$($Sample.blockIndex)"
}

function Compare-RunSemantics {
  param([object]$CppRun, [object]$RustRun)
  Assert-EqualValue $CppRun.metadata.traceDigest $RustRun.metadata.traceDigest 'cross-provider trace digest'
  Assert-EqualValue $CppRun.metadata.traceOperationCount $RustRun.metadata.traceOperationCount 'cross-provider trace operation count'
  Assert-EqualValue $CppRun.metadata.affinityMask $RustRun.metadata.affinityMask 'cross-provider affinity mask'
  $rustByKey = @{}
  foreach ($sample in $RustRun.samples) { $rustByKey[(Get-SampleKey $sample)] = $sample }
  foreach ($cppSample in $CppRun.samples) {
    $key = Get-SampleKey $cppSample
    if (-not $rustByKey.ContainsKey($key)) { throw "Rust run is missing semantic sample '$key'" }
    $rustSample = $rustByKey[$key]
    Assert-EqualValue $cppSample.resultDigest $rustSample.resultDigest "result digest $key"
    Assert-EqualValue $cppSample.snapshotDigest $rustSample.snapshotDigest "snapshot digest $key"
    Assert-EqualValue $cppSample.statusTotal $rustSample.statusTotal "status total $key"
    foreach ($statusName in $script:StatusNames) {
      Assert-EqualValue $cppSample.statusCounts[$statusName] $rustSample.statusCounts[$statusName] "$statusName count $key"
    }
    Assert-EqualValue $cppSample.callbacks $rustSample.callbacks "callback count $key"
    Assert-EqualValue $cppSample.maximumCallbackDepth $rustSample.maximumCallbackDepth "callback depth $key"
    Assert-EqualValue $cppSample.droppedNotificationCount $rustSample.droppedNotificationCount "drop count $key"
  }
}

function Get-ProviderStatistics {
  param([Parameter(Mandatory = $true)] [object[]]$Runs, [Parameter(Mandatory = $true)] [bool]$Warmup)
  $result = [ordered]@{}
  foreach ($block in $script:BlockNames) {
    $samples = @($Runs | ForEach-Object { $_.samples } | Where-Object { $_.warmup -eq $Warmup -and $_.block -eq $block })
    if ($samples.Count -eq 0) { throw "no measured samples for block '$block'" }
    $result[$block] = Get-Statistics -Values @($samples | ForEach-Object { $_.nsPerOperation })
  }
  return $result
}

function New-UniqueRunDirectory {
  param([string]$Root)
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
  $candidate = Join-Path $Root ("output-provider-v1-$stamp-$PID")
  $suffix = 0
  while ([IO.Directory]::Exists($candidate)) {
    ++$suffix
    $candidate = Join-Path $Root ("output-provider-v1-$stamp-$PID-$suffix")
  }
  [void][IO.Directory]::CreateDirectory($candidate)
  return $candidate
}

function Invoke-Benchmark {
  [void](Assert-AffinityMaskInput -Mask $AffinityMask)
  $cppPath = Resolve-RequiredFile -Path $CppTests1 -Name 'CppTests1'
  $rustPath = Resolve-RequiredFile -Path $RustTests1 -Name 'RustTests1'
  if ([StringComparer]::OrdinalIgnoreCase.Equals($cppPath, $rustPath)) {
    throw 'CppTests1 and RustTests1 must be distinct explicit executables'
  }
  $cppExecutable = Get-ExecutableMetadata -Path $cppPath
  $rustExecutable = Get-ExecutableMetadata -Path $rustPath
  $platform = Get-PlatformMetadata
  $outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
  [void][IO.Directory]::CreateDirectory($outputRoot)
  $runDirectory = New-UniqueRunDirectory -Root $outputRoot
  $runRecords = New-Object System.Collections.Generic.List[object]
  $runResults = New-Object System.Collections.Generic.List[object]
  for ($pairIndex = 0; $pairIndex -lt $Pairs; ++$pairIndex) {
    $first = if ((($pairIndex % 2) -eq 0) -eq ($FirstProvider -eq 'cpp')) { 'cpp' } else { 'rust' }
    $order = if ($first -eq 'cpp') { @('cpp', 'rust') } else { @('rust', 'cpp') }
    foreach ($provider in $order) {
      $executable = if ($provider -eq 'cpp') { $cppPath } else { $rustPath }
      $executableMetadata = if ($provider -eq 'cpp') { $cppExecutable } else { $rustExecutable }
      Write-Host ("pair {0}/{1}: {2}" -f ($pairIndex + 1), $Pairs, $provider)
      $runResult = Invoke-BenchmarkProcess -Executable $executable -ExecutableMetadata $executableMetadata `
        -Provider $provider -PairIndex $pairIndex -RunDirectory $runDirectory
      [void]$runResults.Add([pscustomobject][ordered]@{
        pairIndex = $runResult.pairIndex
        provider = $runResult.provider
        executableSha256 = $runResult.executableSha256
        executableSizeBytes = $runResult.executableSizeBytes
        affinityMask = [UInt64]$AffinityMask
        result = 'validated'
      })
      $parsed = Read-BenchmarkRun -Path $runResult.rawPath -ExpectedProvider $provider -PairIndex $pairIndex
      [void]$runRecords.Add($parsed)
    }
  }
  Assert-ExecutableUnchanged -Expected $cppExecutable
  Assert-ExecutableUnchanged -Expected $rustExecutable

  $allRunRecords = $runRecords.ToArray()
  $cppRuns = @($allRunRecords | Where-Object { $_.provider -eq 'cpp' })
  $rustRuns = @($allRunRecords | Where-Object { $_.provider -eq 'rust' })
  Assert-EqualValue $Pairs $cppRuns.Count 'C++ run count'
  Assert-EqualValue $Pairs $rustRuns.Count 'Rust run count'
  for ($pairIndex = 0; $pairIndex -lt $Pairs; ++$pairIndex) {
    $cppRun = @($cppRuns | Where-Object { $_.pairIndex -eq $pairIndex })
    $rustRun = @($rustRuns | Where-Object { $_.pairIndex -eq $pairIndex })
    Assert-EqualValue 1 $cppRun.Count "C++ pair $pairIndex count"
    Assert-EqualValue 1 $rustRun.Count "Rust pair $pairIndex count"
    Compare-RunSemantics -CppRun $cppRun[0] -RustRun $rustRun[0]
  }

  $cppMutationOperations = [UInt64]0
  $rustMutationOperations = [UInt64]0
  foreach ($run in $cppRuns) {
    foreach ($sample in $run.samples) {
      if (-not $sample.warmup -and $sample.block -eq 'mutations') {
        $cppMutationOperations += [UInt64]$sample.operations
      }
    }
  }
  foreach ($run in $rustRuns) {
    foreach ($sample in $run.samples) {
      if (-not $sample.warmup -and $sample.block -eq 'mutations') {
        $rustMutationOperations += [UInt64]$sample.operations
      }
    }
  }
  $acceptanceFailures = @(Get-AcceptanceFailures -PairCount $Pairs -MeasuredCount $MeasuredBlocks `
    -CppOperations $cppMutationOperations -RustOperations $rustMutationOperations)

  $cppStats = Get-ProviderStatistics -Runs $cppRuns -Warmup $false
  $rustStats = Get-ProviderStatistics -Runs $rustRuns -Warmup $false
  $blocks = [ordered]@{}
  $performancePass = $true
  foreach ($block in $script:BlockNames) {
    $cpp = $cppStats[$block]
    $rust = $rustStats[$block]
    $gate = Test-PerformanceGate -CppMedian $cpp.median -RustMedian $rust.median `
      -CppP95 $cpp.p95 -RustP95 $rust.p95 `
      -MaximumMedianRegressionPercent $MaxMedianRegressionPercent `
      -MaximumP95RegressionPercent $MaxP95RegressionPercent
    if (-not $gate.pass) {
      $performancePass = $false
    }
    $blocks[$block] = [pscustomobject][ordered]@{
      cpp = $cpp
      rust = $rust
      medianRatio = [double]$gate.medianRatio
      p95Ratio = [double]$gate.p95Ratio
      medianRegressionPercent = [double]$gate.medianRegressionPercent
      p95RegressionPercent = [double]$gate.p95RegressionPercent
      operationClassRatioLimit = $script:MaximumOperationClassRatio
      performanceGatePassed = [bool]$gate.pass
    }
  }
  $acceptanceQualified = $acceptanceFailures.Count -eq 0
  $semanticPass = $true
  $pass = $semanticPass -and $acceptanceQualified -and $performancePass -and (-not $CollectOnly)
  $analysisPath = Join-Path $runDirectory 'analysis-v1.json'
  $analysis = [ordered]@{
    schemaVersion = $script:SchemaVersion
    record = 'analysis'
    seed = $Seed
    configuration = $Configuration
    affinityMask = [UInt64]$AffinityMask
    pairCount = $Pairs
    warmupBlocks = $WarmupBlocks
    measuredBlocks = $MeasuredBlocks
    snapshotIterations = $SnapshotIterations
    lifecycleIterations = $LifecycleIterations
    minimumAcceptancePairs = $script:MinimumAcceptancePairs
    minimumAcceptanceMeasuredBlocks = $script:MinimumAcceptanceMeasuredBlocks
    minimumAcceptanceMutationOperations = $script:MinimumAcceptanceMutationOperations
    acceptanceQualified = $acceptanceQualified
    acceptanceFailures = @($acceptanceFailures)
    cppTimedMutationOperations = $cppMutationOperations
    rustTimedMutationOperations = $rustMutationOperations
    executables = @(
      [pscustomobject][ordered]@{
        provider = 'cpp'
        sha256 = $cppExecutable.sha256
        sizeBytes = $cppExecutable.sizeBytes
      }
      [pscustomobject][ordered]@{
        provider = 'rust'
        sha256 = $rustExecutable.sha256
        sizeBytes = $rustExecutable.sizeBytes
      }
    )
    platform = $platform
    payloadFree = $true
    semanticParityPassed = $semanticPass
    performanceThresholdEnforced = -not $CollectOnly
    performanceThresholdPassed = $performancePass
    collectOnly = [bool]$CollectOnly
    maxMedianRegressionPercent = $MaxMedianRegressionPercent
    maxP95RegressionPercent = $MaxP95RegressionPercent
    pass = $pass
    runs = $runResults.ToArray()
    blocks = $blocks
  }
  $analysisJson = $analysis | ConvertTo-Json -Depth 20
  [IO.File]::WriteAllText($analysisPath, $analysisJson, [Text.Encoding]::UTF8)
  Write-Host ("analysis: {0}" -f $analysisPath)
  foreach ($block in $script:BlockNames) {
    $stats = $blocks[$block]
    Write-Host ("{0}: C++ median={1:N2} ns/op p95={2:N2}; Rust median={3:N2} ns/op p95={4:N2}; median delta={5:N2}% p95 delta={6:N2}%" -f
      $block, $stats.cpp.median, $stats.cpp.p95, $stats.rust.median, $stats.rust.p95,
      $stats.medianRegressionPercent, $stats.p95RegressionPercent)
  }
  if (-not $pass -and -not $CollectOnly) { throw "provider benchmark evidence rejected; see $analysisPath" }
  return $analysisPath
}

try {
  if ($SelfTest) {
    Invoke-SelfTest
    exit 0
  }
  $analysisPath = Invoke-Benchmark
  Write-Output $analysisPath
  exit 0
}
catch {
  Write-Error $_.Exception.Message
  exit 1
}
