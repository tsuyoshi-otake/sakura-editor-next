#requires -Version 5.1
<#!
.SYNOPSIS
  Runs the disabled provider-neutral Output benchmark and analyzes JSONL samples.

.DESCRIPTION
  The caller supplies one C++ and one Rust tests1.exe.  Qualified runs also
  require the matching producer-owned v1 build manifest for each executable.
  Each interleaved pair runs the same deterministic disabled gtest workload
  against the compile-time selected provider.  The script rejects missing or
  failed runs, validates the payload-free schema and semantic digests, checks
  complete paired provenance and surviving descendants, and writes median/p95
  statistics without adding dependencies.

  The benchmark process is intentionally disabled in normal test runs.  This
  script is the opt-in orchestrator for local and CI evidence collection.
#>
[CmdletBinding()]
param(
  [string]$CppTests1,
  [string]$RustTests1,
  [string]$CppBuildManifest,
  [string]$RustBuildManifest,
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
$script:ProviderManifestSchemaVersion = 1
$script:ProviderManifestRecord = 'output-provider-build-manifest'
$script:ProviderProbeFilter = 'CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle'
$script:ProviderSymbols = @(
  'sakura_output_provider_create_v1',
  'sakura_output_provider_apply_v1',
  'sakura_output_provider_snapshot_measure_v1',
  'sakura_output_provider_snapshot_write_v1',
  'sakura_output_provider_active_channel_v1',
  'sakura_output_provider_stop_v1',
  'sakura_output_provider_destroy_v1'
)
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

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

function Get-ProviderProperty {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
    [Parameter(Mandatory = $true)] [string]$Name,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  if ($null -eq $Object) { throw "$Context is missing" }
  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) { throw "$Context is missing property '$Name'" }
  return $property.Value
}

function Get-ProviderCandidate {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
    [Parameter(Mandatory = $true)] [string[]]$Names,
    [string[]]$Sections = @()
  )
  if ($null -eq $Object) { return $null }
  foreach ($name in $Names) {
    $property = $Object.PSObject.Properties[$name]
    if ($null -ne $property) { return $property.Value }
  }
  foreach ($sectionName in $Sections) {
    $sectionProperty = $Object.PSObject.Properties[$sectionName]
    if ($null -eq $sectionProperty -or $null -eq $sectionProperty.Value) { continue }
    foreach ($name in $Names) {
      $property = $sectionProperty.Value.PSObject.Properties[$name]
      if ($null -ne $property) { return $property.Value }
    }
  }
  return $null
}

function Get-ProviderRequiredCandidate {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
    [Parameter(Mandatory = $true)] [string[]]$Names,
    [Parameter(Mandatory = $true)] [string]$Context,
    [string[]]$Sections = @()
  )
  $value = Get-ProviderCandidate $Object $Names $Sections
  if ($null -eq $value) { throw "$Context is missing" }
  return $value
}

function Get-ProviderString {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$Value) -or
      [string]$Value -ne ([string]$Value).Trim() -or [string]$Value -match '[\r\n]' -or
      ([string]$Value).Length -gt 512 -or [string]$Value -match '(?i)^[A-Za-z]:\\|^\\\\') {
    throw "$Context must be a bounded identity string"
  }
  return [string]$Value
}

function Get-ProviderBoolean {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  if ($Value -isnot [bool]) { throw "$Context must be a JSON boolean" }
  return [bool]$Value
}

function Get-ProviderHash {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  if ($Value -isnot [string] -or [string]$Value -notmatch '^(?i:sha256:)?[0-9a-f]{64}$') {
    throw "$Context must be a SHA-256 identity"
  }
  return ([string]$Value -replace '^(?i:sha256:)','').ToLowerInvariant()
}

function Get-ProviderUInt64 {
  param(
    [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
    [Parameter(Mandatory = $true)] [string]$Context,
    [UInt64]$Maximum = [UInt64]::MaxValue
  )
  if ($null -eq $Value -or $Value -is [bool]) { throw "$Context must be a bounded non-negative integer" }
  try { $decimal = [decimal]$Value } catch { throw "$Context must be a bounded non-negative integer" }
  if ($decimal -lt 0 -or $decimal -ne [decimal]::Truncate($decimal) -or $decimal -gt [decimal]$Maximum) {
    throw "$Context must be a bounded non-negative integer"
  }
  return [UInt64]$decimal
}

function Get-TextSha256 {
  param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try {
    $encoding = New-Object Text.UTF8Encoding($false)
    return ([BitConverter]::ToString($algorithm.ComputeHash($encoding.GetBytes($Value)))).Replace('-', '').ToLowerInvariant()
  }
  finally { $algorithm.Dispose() }
}

function Resolve-ProviderManifestFile {
  param([Parameter(Mandatory = $true)] [string]$Path, [Parameter(Mandatory = $true)] [string]$Name)
  if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Name is required" }
  try { $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop }
  catch { throw "$Name does not exist" }
  $fullPath = [IO.Path]::GetFullPath($resolved.Path)
  $item = Get-Item -LiteralPath $fullPath -Force -ErrorAction Stop
  if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
      (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw "$Name is not a regular file"
  }
  return $fullPath
}

function Get-ProviderFileSha256 {
  param([Parameter(Mandatory = $true)] [string]$Path)
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Invoke-ProviderGitText {
  param([Parameter(Mandatory = $true)] [string[]]$Arguments)
  $oldErrorAction = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'SilentlyContinue'
    $value = (& git -C $script:RepoRoot @Arguments 2>$null | Out-String)
    $exitCode = $LASTEXITCODE
  }
  finally { $ErrorActionPreference = $oldErrorAction }
  if ($exitCode -ne 0) { throw 'Git source-state query failed' }
  return [string]$value
}

function Get-ProviderSourceState {
  $statusText = Invoke-ProviderGitText @('status', '--porcelain=v1', '--untracked-files=all')
  $canonicalStatus = ($statusText -replace "`r`n", "`n" -replace "`r", "`n").TrimEnd("`n")
  $statusLines = @()
  if (-not [string]::IsNullOrEmpty($canonicalStatus)) { $statusLines = @($canonicalStatus -split "`n") }
  $head = (Invoke-ProviderGitText @('rev-parse', '--verify', 'HEAD')).Trim()
  if ($head -notmatch '^[0-9a-fA-F]{40}$') { throw 'Git HEAD is not a full commit identity' }
  return [pscustomobject][ordered]@{
    head = $head.ToLowerInvariant()
    dirty = [bool]($statusLines.Count -ne 0)
    statusSha256 = Get-TextSha256 $canonicalStatus
    statusLineCount = [int]$statusLines.Count
  }
}

function Assert-ProviderSourceState {
  param(
    [Parameter(Mandatory = $true)] [object]$Expected,
    [Parameter(Mandatory = $true)] [object]$Actual,
    [string]$Context = 'provider source state'
  )
  if ([string]$Expected.head -ne [string]$Actual.head -or
      [bool]$Expected.dirty -ne [bool]$Actual.dirty -or
      [string]$Expected.statusSha256 -ne [string]$Actual.statusSha256 -or
      [int]$Expected.statusLineCount -ne [int]$Actual.statusLineCount) {
    throw "$Context changed"
  }
}

function Assert-ProviderPayloadFree {
  param([Parameter(Mandatory = $true)] [object]$Manifest)
  $json = $Manifest | ConvertTo-Json -Depth 20 -Compress
  if ($json -match '(?i)"(?:path|filepath|directory|command|commandLine|arguments|argv|stdout|stderr|caption|text|document|profile|exception|message|detail|label|id|payload)"\s*:') {
    throw 'provider build manifest contains a payload-bearing property'
  }
  if ($json -match '(?i)[A-Za-z]:\\\\|\\\\\\\\') { throw 'provider build manifest contains a path-shaped value' }
  return $true
}

function Get-ProviderManifestArtifact {
  param(
    [Parameter(Mandatory = $true)] [object]$Manifest,
    [Parameter(Mandatory = $true)] [string]$Backend,
    [Parameter(Mandatory = $true)] [object]$Executable
  )
  $entries = @(Get-ProviderRequiredCandidate $Manifest @('artifacts') 'artifacts')
  if ($entries.Count -lt 1 -or $entries.Count -gt 16) { throw 'provider build manifest artifacts are outside the bounded range' }
  $matches = New-Object System.Collections.Generic.List[object]
  foreach ($entry in $entries) {
    if ($null -eq $entry) { throw 'provider build manifest has a null artifact entry' }
    $entryBackend = (Get-ProviderString (Get-ProviderRequiredCandidate $entry @('backend') 'artifact.backend') 'artifact.backend').ToLowerInvariant()
    $entryHash = Get-ProviderHash (Get-ProviderRequiredCandidate $entry @('artifactSha256', 'sha256') 'artifact.artifactSha256') 'artifact.artifactSha256'
    $entrySize = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $entry @('sizeBytes', 'artifactSizeBytes') 'artifact.sizeBytes') 'artifact.sizeBytes'
    if ($entrySize -lt 1) { throw 'provider build manifest artifact size must be positive' }
    if ($entryBackend -ne $Backend) { throw 'provider build manifest contains an unexpected artifact backend' }
    if ($entryHash -eq $Executable.sha256 -and $entrySize -eq [UInt64]$Executable.sizeBytes) {
      [void]$matches.Add([pscustomobject][ordered]@{ backend = $entryBackend; sha256 = $entryHash; sizeBytes = [UInt64]$entrySize })
    }
  }
  if ($matches.Count -ne 1) { throw 'provider build manifest tests1 artifact does not match the supplied executable' }
  return $matches[0]
}

function Get-ProviderSelectorProof {
  param(
    [Parameter(Mandatory = $true)] [object]$Manifest,
    [Parameter(Mandatory = $true)] [string]$Backend
  )
  $proof = Get-ProviderRequiredCandidate $Manifest @('selectorProof') 'selectorProof'
  $result = Get-ProviderString (Get-ProviderRequiredCandidate $proof @('result') 'selectorProof.result') 'selectorProof.result'
  $isObjectProof = $result -ceq 'dumpbin-unresolved-refs-verified'
  $isLtcgProof = $result -ceq 'msvc-ltcg-compile-selector-verified'
  if (-not $isObjectProof -and -not $isLtcgProof) { throw 'selector proof verification result is unsupported' }
  if ($isObjectProof) {
    $manifestConfiguration = Get-ProviderCandidate $Manifest @('configuration')
    if ($null -ne $manifestConfiguration -and
        (Get-ProviderString $manifestConfiguration 'configuration').ToLowerInvariant() -eq 'release') {
      throw 'Release selector proof must use the MSVC LTCG compile-selector contract.'
    }
  }
  $outputBackend = (Get-ProviderString (Get-ProviderRequiredCandidate $proof @('outputBackend') 'selectorProof.outputBackend') 'selectorProof.outputBackend').ToLowerInvariant()
  $utf16Backend = (Get-ProviderString (Get-ProviderRequiredCandidate $proof @('utf16Backend') 'selectorProof.utf16Backend') 'selectorProof.utf16Backend').ToLowerInvariant()
  if ($outputBackend -ne $Backend -or $utf16Backend -ne 'cpp') { throw 'selector proof providers are not exact' }
  foreach ($field in @('outputProductionPackage', 'utf16ProductionPackage', 'utf16BenchmarkTelemetry', 'assemblyListings')) {
    if (Get-ProviderBoolean (Get-ProviderRequiredCandidate $proof @($field) ('selectorProof.' + $field)) ('selectorProof.' + $field)) {
      throw "selector proof enables $field"
    }
  }
  $freshnessMethod = Get-ProviderString (Get-ProviderRequiredCandidate $proof @('providerObjectFreshnessMethod') 'selectorProof.providerObjectFreshnessMethod') 'selectorProof.providerObjectFreshnessMethod'
  if ($freshnessMethod -cne 'exact-object-absence-v1') {
    throw 'selector proof object freshness method is unsupported'
  }
  $objectAbsentBeforeBuild = Get-ProviderBoolean (Get-ProviderRequiredCandidate $proof @('providerObjectAbsentBeforeBuild') 'selectorProof.providerObjectAbsentBeforeBuild') 'selectorProof.providerObjectAbsentBeforeBuild'
  if (-not $objectAbsentBeforeBuild) {
    throw 'selector proof does not prove exact pre-build object absence'
  }
  $objectHash = Get-ProviderHash (Get-ProviderRequiredCandidate $proof @('providerObjectSha256After', 'objectSha256After') 'selectorProof.providerObjectSha256After') 'selectorProof.providerObjectSha256After'
  $objectSize = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('providerObjectSizeBytesAfter', 'objectSizeBytesAfter') 'selectorProof.providerObjectSizeBytesAfter') 'selectorProof.providerObjectSizeBytesAfter'
  if ($objectSize -lt 1) { throw 'selector proof object size must be positive' }
  $verificationMethod = $null
  $providerObjectFormat = $null
  $compileLogHashBefore = $null
  $compileLogHashAfter = $null
  $compileLogSize = [UInt64]0
  $compileLogProof = $false
  $compileHasGl = $false
  $compileRustSelectorCount = 0
  if ($isLtcgProof) {
    $configuration = (Get-ProviderString (Get-ProviderRequiredCandidate $Manifest @('configuration') 'configuration') 'configuration').ToLowerInvariant()
    if ($configuration -cne 'release') { throw 'MSVC LTCG selector proof is only valid for Release.' }
    $verificationMethod = Get-ProviderString (Get-ProviderRequiredCandidate $proof @('verificationMethod') 'selectorProof.verificationMethod') 'selectorProof.verificationMethod'
    if ($verificationMethod -cne 'msvc-ltcg-compile-selector') { throw 'MSVC LTCG selector proof method is invalid' }
    $providerObjectFormat = Get-ProviderString (Get-ProviderRequiredCandidate $proof @('providerObjectFormat') 'selectorProof.providerObjectFormat') 'selectorProof.providerObjectFormat'
    if ($providerObjectFormat -cne 'msvc-ltcg-anonymous') { throw 'MSVC LTCG selector proof object format is invalid' }
    $compileLogHashAfter = Get-ProviderHash (Get-ProviderRequiredCandidate $proof @('compileLogSha256After') 'selectorProof.compileLogSha256After') 'selectorProof.compileLogSha256After'
    $compileLogSize = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('compileLogSizeBytesAfter') 'selectorProof.compileLogSizeBytesAfter') 'selectorProof.compileLogSizeBytesAfter'
    if ($compileLogSize -lt 1) { throw 'selector proof compile log size must be positive' }
    $compileLogProof = Get-ProviderBoolean (Get-ProviderRequiredCandidate $proof @('compileLogProof') 'selectorProof.compileLogProof') 'selectorProof.compileLogProof'
    if (-not $compileLogProof) { throw 'selector proof compile log was not verified' }
    $compileHasGl = Get-ProviderBoolean (Get-ProviderRequiredCandidate $proof @('compileCommandHasGl') 'selectorProof.compileCommandHasGl') 'selectorProof.compileCommandHasGl'
    if (-not $compileHasGl) { throw 'selector proof compile command is not /GL' }
    $compileRustSelectorCount = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('compileCommandRustSelectorDefineCount') 'selectorProof.compileCommandRustSelectorDefineCount') 'selectorProof.compileCommandRustSelectorDefineCount' 8
    $expectedCompileSelectorCount = if ($Backend -eq 'rust') { 1 } else { 0 }
    if ($compileRustSelectorCount -ne $expectedCompileSelectorCount) { throw 'selector proof compile command selector does not match the backend' }
    $compileLogBeforeValue = Get-ProviderCandidate $proof @('compileLogSha256Before')
    if ($null -ne $compileLogBeforeValue) { $compileLogHashBefore = Get-ProviderHash $compileLogBeforeValue 'selectorProof.compileLogSha256Before' }
  }
  $symbolsProperty = $proof.PSObject.Properties['unresolvedProviderSymbols']
  if ($null -eq $symbolsProperty) { throw 'selector proof unresolved symbol set is missing' }
  $symbols = @($symbolsProperty.Value | ForEach-Object { Get-ProviderString $_ 'selectorProof.unresolvedProviderSymbols' })
  $normalizedSymbols = @($symbols | ForEach-Object { $_.ToLowerInvariant() })
  if (@($normalizedSymbols | Sort-Object -Unique).Count -ne $normalizedSymbols.Count) { throw 'selector proof unresolved symbol set contains duplicates' }
  $expectedSymbols = if ($Backend -eq 'rust' -and $isObjectProof) { @($script:ProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) } else { @() }
  if ((@($normalizedSymbols | Sort-Object) -join '|') -cne ($expectedSymbols -join '|')) { throw 'selector proof unresolved symbol set is not exact' }
  $symbolCount = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('unresolvedProviderSymbolCount') 'selectorProof.unresolvedProviderSymbolCount') 'selectorProof.unresolvedProviderSymbolCount' 1024
  if ($symbolCount -ne $normalizedSymbols.Count) { throw 'selector proof unresolved symbol count is invalid' }
  $archiveResult = Get-ProviderString (Get-ProviderRequiredCandidate $proof @('rustArchiveResult', 'archiveResult') 'selectorProof.rustArchiveResult') 'selectorProof.rustArchiveResult'
  if ($archiveResult -cne 'dumpbin-defined-exports-verified') { throw 'selector proof archive export verification is incomplete' }
  $archiveHash = Get-ProviderHash (Get-ProviderRequiredCandidate $proof @('rustArchiveSha256', 'archiveSha256') 'selectorProof.rustArchiveSha256') 'selectorProof.rustArchiveSha256'
  $archiveSize = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('rustArchiveSizeBytes', 'archiveSizeBytes') 'selectorProof.rustArchiveSizeBytes') 'selectorProof.rustArchiveSizeBytes'
  if ($archiveSize -lt 1) { throw 'selector proof archive size must be positive' }
  $definedProperty = $proof.PSObject.Properties['definedProviderSymbols']
  if ($null -eq $definedProperty) { throw 'selector proof defined symbol set is missing' }
  $defined = @($definedProperty.Value | ForEach-Object { Get-ProviderString $_ 'selectorProof.definedProviderSymbols' })
  $normalizedDefined = @($defined | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
  $expectedDefined = @($script:ProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object)
  if ((@($normalizedDefined) -join '|') -cne ($expectedDefined -join '|')) { throw 'selector proof defined symbol set is not exact' }
  $definedCount = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $proof @('definedProviderSymbolCount') 'selectorProof.definedProviderSymbolCount') 'selectorProof.definedProviderSymbolCount' 1024
  if ($definedCount -ne $expectedDefined.Count) { throw 'selector proof defined symbol count is invalid' }
  $contractHash = Get-ProviderHash (Get-ProviderRequiredCandidate $proof @('selectorContractSha256', 'contractSha256') 'selectorProof.selectorContractSha256') 'selectorProof.selectorContractSha256'
  $baseCanonical = if ($isLtcgProof) {
    'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|method={2}|symbols=|object-after={3}|object-format={4}|compile-log-after={5}|compile-log-size={6}|compile-gl={7}|compile-rust-selector-count={8}|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f
      $Backend, $result, $verificationMethod, $objectHash, $providerObjectFormat, $compileLogHashAfter,
      $compileLogSize, $compileHasGl, $compileRustSelectorCount
  }
  else {
    'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|symbols={2}|object-after={3}|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f
      $Backend, $result, (@($normalizedSymbols | Sort-Object) -join ','), $objectHash
  }
  $canonical = '{0}|archive-result={1}|archive={2}|defined={3}' -f
    (Get-TextSha256 $baseCanonical), $archiveResult, $archiveHash, (@($normalizedDefined) -join ',')
  if ($contractHash -ne (Get-TextSha256 $canonical)) { throw 'selector proof contract hash is invalid' }
  $manifestProofHash = Get-ProviderHash (Get-ProviderRequiredCandidate $Manifest @('selectorProofSha256', 'selectorContractSha256') 'selectorProofSha256') 'selectorProofSha256'
  if ($manifestProofHash -ne $contractHash) { throw 'provider build manifest selector proof hash does not match its proof' }
  $objectBefore = Get-ProviderCandidate $proof @('providerObjectSha256Before', 'objectSha256Before')
  if ($null -ne $objectBefore) { $objectBefore = Get-ProviderHash $objectBefore 'selectorProof.providerObjectSha256Before' }
  return [pscustomobject][ordered]@{
    result = $result
    outputBackend = $outputBackend
    utf16Backend = $utf16Backend
    providerObjectSha256Before = $objectBefore
    providerObjectSha256After = $objectHash
    providerObjectSizeBytesAfter = [UInt64]$objectSize
    verificationMethod = $verificationMethod
    providerObjectFormat = $providerObjectFormat
    compileLogSha256Before = $compileLogHashBefore
    compileLogSha256After = $compileLogHashAfter
    compileLogSizeBytesAfter = [UInt64]$compileLogSize
    compileLogProof = $compileLogProof
    compileCommandHasGl = $compileHasGl
    compileCommandRustSelectorDefineCount = [int]$compileRustSelectorCount
    providerObjectFreshnessMethod = $freshnessMethod
    providerObjectAbsentBeforeBuild = $objectAbsentBeforeBuild
    unresolvedProviderSymbols = @($normalizedSymbols | Sort-Object)
    unresolvedProviderSymbolCount = [int]$symbolCount
    rustArchiveResult = $archiveResult
    rustArchiveSha256 = $archiveHash
    rustArchiveSizeBytes = [UInt64]$archiveSize
    definedProviderSymbols = @($normalizedDefined)
    definedProviderSymbolCount = [int]$definedCount
    selectorContractSha256 = $contractHash
  }
}

function Get-ProviderRuntimeProbe {
  param(
    [Parameter(Mandatory = $true)] [object]$Manifest,
    [Parameter(Mandatory = $true)] [string]$Backend,
    [Parameter(Mandatory = $true)] [object]$Executable
  )
  $probe = Get-ProviderRequiredCandidate $Manifest @('runtimeProviderProbe', 'providerProbe') 'runtimeProviderProbe'
  $result = Get-ProviderString (Get-ProviderRequiredCandidate $probe @('result') 'runtimeProviderProbe.result') 'runtimeProviderProbe.result'
  if ($result -cne 'verified') { throw 'runtime provider probe is not verified' }
  $expected = (Get-ProviderString (Get-ProviderRequiredCandidate $probe @('expectedBackend') 'runtimeProviderProbe.expectedBackend') 'runtimeProviderProbe.expectedBackend').ToLowerInvariant()
  $observed = (Get-ProviderString (Get-ProviderRequiredCandidate $probe @('observedBackend') 'runtimeProviderProbe.observedBackend') 'runtimeProviderProbe.observedBackend').ToLowerInvariant()
  if ($expected -ne $Backend -or $observed -ne $Backend) { throw 'runtime provider probe backend observation is not exact' }
  if (-not (Get-ProviderBoolean (Get-ProviderRequiredCandidate $probe @('standalone') 'runtimeProviderProbe.standalone') 'runtimeProviderProbe.standalone')) { throw 'runtime provider probe was not standalone' }
  if (-not (Get-ProviderBoolean (Get-ProviderRequiredCandidate $probe @('payloadFree') 'runtimeProviderProbe.payloadFree') 'runtimeProviderProbe.payloadFree')) { throw 'runtime provider probe is not payload-free' }
  $testFilter = Get-ProviderString (Get-ProviderRequiredCandidate $probe @('testFilter', 'gtestFilter') 'runtimeProviderProbe.testFilter') 'runtimeProviderProbe.testFilter'
  if ($testFilter -cne $script:ProviderProbeFilter) { throw 'runtime provider probe did not execute the frozen lifecycle test' }
  $hash = Get-ProviderHash (Get-ProviderRequiredCandidate $probe @('tests1Sha256', 'executableSha256', 'artifactSha256') 'runtimeProviderProbe.tests1Sha256') 'runtimeProviderProbe.tests1Sha256'
  $size = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $probe @('tests1SizeBytes', 'executableSizeBytes', 'sizeBytes') 'runtimeProviderProbe.tests1SizeBytes') 'runtimeProviderProbe.tests1SizeBytes'
  if ($hash -ne $Executable.sha256 -or $size -ne [UInt64]$Executable.sizeBytes) { throw 'runtime provider probe identity does not match the supplied executable' }
  return [pscustomobject][ordered]@{
    result = $result
    expectedBackend = $expected
    observedBackend = $observed
    standalone = $true
    payloadFree = $true
    testFilter = $testFilter
    tests1Sha256 = $hash
    tests1SizeBytes = [UInt64]$size
  }
}

function Get-ProviderBuildManifest {
  param(
    [Parameter(Mandatory = $true)] [string]$Path,
    [Parameter(Mandatory = $true)] [string]$Backend,
    [Parameter(Mandatory = $true)] [string]$Configuration,
    [Parameter(Mandatory = $true)] [object]$Executable,
    [Parameter(Mandatory = $true)] [object]$CurrentSource
  )
  $manifestPath = Resolve-ProviderManifestFile $Path ($Backend + 'BuildManifest')
  $manifestSha256Before = Get-ProviderFileSha256 $manifestPath
  try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -ErrorAction Stop }
  catch { throw "$Backend build manifest is not valid JSON" }
  if ($null -eq $manifest) { throw "$Backend build manifest is empty" }
  $schema = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $manifest @('schemaVersion') 'schemaVersion') 'schemaVersion' 16
  if ($schema -ne $script:ProviderManifestSchemaVersion) { throw "$Backend build manifest schema is unsupported" }
  $record = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('record') 'record') 'record'
  if ($record -cne $script:ProviderManifestRecord) { throw "$Backend build manifest record is not provider-generated" }
  if (-not (Get-ProviderBoolean (Get-ProviderRequiredCandidate $manifest @('payloadFree') 'payloadFree') 'payloadFree')) { throw "$Backend build manifest is not payload-free" }
  $status = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('status') 'status') 'status'
  if ($status -cne 'committed') { throw "$Backend build manifest is not committed" }
  $manifestBackend = (Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('backend') 'backend') 'backend').ToLowerInvariant()
  $platform = (Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('platform') 'platform') 'platform').ToLowerInvariant()
  $manifestConfiguration = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('configuration') 'configuration') 'configuration'
  if ($manifestBackend -ne $Backend -or $platform -ne 'x64' -or
      -not [StringComparer]::OrdinalIgnoreCase.Equals($manifestConfiguration, $Configuration)) {
    throw "$Backend build manifest selector identity does not match the requested run"
  }
  $sourceHead = (Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('sourceHead', 'sourceCommit') 'sourceHead' -Sections @('source')) 'sourceHead').ToLowerInvariant()
  if ($sourceHead -notmatch '^[0-9a-f]{40}$') { throw 'provider build manifest sourceHead is not a commit identity' }
  $sourceDirty = Get-ProviderBoolean (Get-ProviderRequiredCandidate $manifest @('sourceDirty', 'dirty') 'sourceDirty' -Sections @('source')) 'sourceDirty'
  $sourceStatus = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('sourceStatusSha256', 'statusSha256') 'sourceStatusSha256' -Sections @('source')) 'sourceStatusSha256'
  $sourceStatusLines = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $manifest @('sourceStatusLineCount', 'statusLineCount') 'sourceStatusLineCount' -Sections @('source')) 'sourceStatusLineCount' 2147483647
  if ($sourceDirty) { throw 'qualified provider evidence requires a clean source state' }
  if ($sourceHead -ne [string]$CurrentSource.head -or $sourceStatus -ne [string]$CurrentSource.statusSha256 -or
      [UInt64]$sourceStatusLines -ne [UInt64]$CurrentSource.statusLineCount -or [bool]$CurrentSource.dirty) {
    throw "$Backend build manifest source state does not match the current checkout"
  }
  $outputBackend = (Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('outputBackend') 'outputBackend' -Sections @('selectors')) 'outputBackend').ToLowerInvariant()
  $utf16Backend = (Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('utf16Backend') 'utf16Backend' -Sections @('selectors')) 'utf16Backend').ToLowerInvariant()
  if ($outputBackend -ne $Backend -or $utf16Backend -ne 'cpp') { throw "$Backend build manifest selectors are not exact" }
  foreach ($field in @('outputProductionPackage', 'utf16ProductionPackage')) {
    if (Get-ProviderBoolean (Get-ProviderRequiredCandidate $manifest @($field) $field -Sections @('selectors')) $field) { throw "$Backend build manifest enables $field" }
  }
  foreach ($field in @('utf16BenchmarkTelemetry', 'assemblyListings')) {
    $flag = Get-ProviderCandidate $manifest @($field) @('selectors')
    if ($null -ne $flag -and (Get-ProviderBoolean $flag $field)) { throw "$Backend build manifest enables $field" }
  }
  $manifestTestsHash = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('tests1Sha256', 'tests1HashAfter') 'tests1Sha256' -Sections @('tests1', 'artifact')) 'tests1Sha256'
  $manifestTestsSize = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $manifest @('tests1SizeBytes', 'tests1SizeBytesAfter') 'tests1SizeBytes' -Sections @('tests1', 'artifact')) 'tests1SizeBytes'
  if ($manifestTestsHash -ne $Executable.sha256 -or $manifestTestsSize -ne [UInt64]$Executable.sizeBytes) { throw "$Backend build manifest tests1 identity does not match the supplied executable" }
  $artifact = Get-ProviderManifestArtifact $manifest $Backend $Executable
  $testsBefore = Get-ProviderCandidate $manifest @('tests1Sha256Before', 'tests1HashBefore') @('tests1')
  if ($null -ne $testsBefore) { $testsBefore = Get-ProviderHash $testsBefore 'tests1Sha256Before' }
  $testsBeforeSizeValue = Get-ProviderCandidate $manifest @('tests1SizeBytesBefore') @('tests1')
  $testsBeforeSize = if ($null -eq $testsBeforeSizeValue) { [UInt64]0 } else { Get-ProviderUInt64 $testsBeforeSizeValue 'tests1SizeBytesBefore' }
  $windowsImage = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('windowsImageIdentity', 'windowsImage') 'windowsImageIdentity' -Sections @('host')) 'windowsImageIdentity'
  if ($windowsImage -match '(?i)^(unknown|unspecified|n/?a)$') { throw "$Backend build manifest Windows identity is unknown" }
  $windowsImageHash = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('windowsImageSha256', 'hostSha256') 'windowsImageSha256' -Sections @('host')) 'windowsImageSha256'
  if ($windowsImageHash -ne (Get-TextSha256 $windowsImage)) { throw "$Backend build manifest Windows identity hash is invalid" }
  $powerMode = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('powerMode', 'powerPlan') 'powerMode' -Sections @('power')) 'powerMode'
  if ($powerMode -match '(?i)^(unknown|unspecified|n/?a)$') { throw "$Backend build manifest power mode is unknown" }
  $powerModeHash = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('powerModeSha256', 'powerSha256') 'powerModeSha256' -Sections @('power')) 'powerModeSha256'
  if ($powerModeHash -ne (Get-TextSha256 $powerMode)) { throw "$Backend build manifest power identity hash is invalid" }
  $parallelism = Get-ProviderUInt64 (Get-ProviderRequiredCandidate $manifest @('buildParallelism', 'parallelism') 'buildParallelism' -Sections @('build')) 'buildParallelism' 256
  if ($parallelism -lt 1) { throw 'provider build manifest parallelism is invalid' }
  $msvc = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('msvcIdentity', 'msvcVersion') 'msvcIdentity' -Sections @('toolchain')) 'msvcIdentity'
  $rust = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('rustToolchain', 'rustVersion') 'rustToolchain' -Sections @('toolchain')) 'rustToolchain'
  $rustLock = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('rustLockSha256', 'cargoLockSha256') 'rustLockSha256' -Sections @('toolchain')) 'rustLockSha256'
  $packagePlan = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('packagePlanSha256', 'packagePlanHash') 'packagePlanSha256' -Sections @('package')) 'packagePlanSha256'
  $buildCommand = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('buildCommandSha256', 'commandSha256') 'buildCommandSha256' -Sections @('commands', 'build')) 'buildCommandSha256'
  $packageCommand = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('packagePlanCommandSha256', 'packageCommandSha256') 'packagePlanCommandSha256' -Sections @('commands', 'build')) 'packagePlanCommandSha256'
  $probeCommand = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('runtimeProviderProbeCommandSha256', 'providerProbeCommandSha256') 'runtimeProviderProbeCommandSha256' -Sections @('commands', 'runtimeProviderProbe')) 'runtimeProviderProbeCommandSha256'
  $closureMode = Get-ProviderString (Get-ProviderRequiredCandidate $manifest @('runtimeClosureMode') 'runtimeClosureMode' -Sections @('runtimeProviderProbe')) 'runtimeClosureMode'
  if ($closureMode -cne 'exe-only') { throw "$Backend build manifest runtime closure mode is not exe-only" }
  $runtimeClosureSha256 = Get-ProviderHash (Get-ProviderRequiredCandidate $manifest @('runtimeClosureSha256') 'runtimeClosureSha256' -Sections @('runtimeProviderProbe')) 'runtimeClosureSha256'
  $expectedRuntimeClosure = Get-TextSha256 ('exe-only|tests1={0}|size={1}' -f
    $manifestTestsHash, ([UInt64]$manifestTestsSize).ToString([Globalization.CultureInfo]::InvariantCulture))
  if ($runtimeClosureSha256 -ne $expectedRuntimeClosure) { throw "$Backend build manifest runtime closure receipt is invalid" }
  $selectorProof = Get-ProviderSelectorProof $manifest $Backend
  $probe = Get-ProviderRuntimeProbe $manifest $Backend $Executable
  $transaction = Get-ProviderRequiredCandidate $manifest @('transaction') 'transaction'
  $transactionStatus = Get-ProviderString (Get-ProviderRequiredCandidate $transaction @('status') 'transaction.status') 'transaction.status'
  $publication = Get-ProviderString (Get-ProviderRequiredCandidate $transaction @('publication') 'transaction.publication') 'transaction.publication'
  if ($transactionStatus -cne 'committed' -or $publication -cne 'atomic-directory-rename') { throw "$Backend provider transaction is not committed atomically" }
  foreach ($field in @('sourceBeforeVerified', 'sourceAfterVerified', 'hostBeforeVerified', 'hostAfterVerified', 'tests1BeforeVerified', 'tests1AfterVerified', 'tests1CopyVerified', 'tests1ManifestVerified', 'tests1ProbeVerified', 'manifestGeneratedByProducer')) {
    if (-not (Get-ProviderBoolean (Get-ProviderRequiredCandidate $transaction @($field) ('transaction.' + $field)) ('transaction.' + $field))) { throw "$Backend provider transaction proof is incomplete" }
  }
  [void](Assert-ProviderPayloadFree $manifest)
  $manifestSha256After = Get-ProviderFileSha256 $manifestPath
  if ($manifestSha256After -ne $manifestSha256Before) { throw "$Backend build manifest changed while it was being validated" }
  return [pscustomobject][ordered]@{
    manifestPath = $manifestPath
    manifestSha256 = $manifestSha256After
    schemaVersion = [int]$schema
    record = $record
    payloadFree = $true
    status = $status
    backend = $manifestBackend
    platform = $platform
    configuration = $manifestConfiguration
    sourceHead = $sourceHead
    sourceDirty = [bool]$sourceDirty
    sourceStatusSha256 = $sourceStatus
    sourceStatusLineCount = [UInt64]$sourceStatusLines
    outputBackend = $outputBackend
    utf16Backend = $utf16Backend
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    tests1Sha256 = $manifestTestsHash
    tests1SizeBytes = [UInt64]$manifestTestsSize
    tests1Sha256Before = $testsBefore
    tests1SizeBytesBefore = [UInt64]$testsBeforeSize
    artifact = $artifact
    windowsImageIdentity = $windowsImage
    windowsImageSha256 = $windowsImageHash
    powerMode = $powerMode
    powerModeSha256 = $powerModeHash
    buildParallelism = [int]$parallelism
    msvcIdentity = $msvc
    rustToolchain = $rust
    rustLockSha256 = $rustLock
    packagePlanSha256 = $packagePlan
    buildCommandSha256 = $buildCommand
    packagePlanCommandSha256 = $packageCommand
    runtimeProviderProbeCommandSha256 = $probeCommand
    runtimeClosureMode = $closureMode
    runtimeClosureSha256 = $runtimeClosureSha256
    selectorProof = $selectorProof
    selectorProofSha256 = $selectorProof.selectorContractSha256
    runtimeProviderProbe = $probe
    transaction = $transaction
  }
}

function Assert-ProviderManifestIdentity {
  param([Parameter(Mandatory = $true)] [object]$Provenance)
  $actual = Get-ProviderFileSha256 $Provenance.manifestPath
  if ($actual -ne [string]$Provenance.manifestSha256) { throw 'provider build manifest changed during measurement' }
  return $true
}

function Test-ProviderAcceptanceQualified {
  param(
    [Parameter(Mandatory = $true)] [bool]$CollectOnly,
    [Parameter(Mandatory = $true)] [bool]$ProvenanceComplete,
    [Parameter(Mandatory = $true)] [AllowEmptyCollection()] [string[]]$Failures
  )
  return (-not $CollectOnly) -and $ProvenanceComplete -and @($Failures).Count -eq 0
}

function Assert-ProviderSelfTestRejects {
  param(
    [Parameter(Mandatory = $true)] [scriptblock]$Action,
    [Parameter(Mandatory = $true)] [string]$Context
  )
  try {
    & $Action
  }
  catch {
    return
  }
  throw "$Context unexpectedly accepted invalid evidence"
}

function New-ProviderProvenance {
  param(
    [Parameter(Mandatory = $true)] [object]$Cpp,
    [Parameter(Mandatory = $true)] [object]$Rust,
    [Parameter(Mandatory = $true)] [object]$Platform,
    [Parameter(Mandatory = $true)] [string]$MeasurementSha256
  )
  foreach ($field in @('sourceHead', 'sourceStatusSha256', 'sourceStatusLineCount', 'windowsImageIdentity', 'windowsImageSha256', 'powerMode', 'powerModeSha256', 'msvcIdentity', 'rustToolchain', 'rustLockSha256', 'packagePlanSha256', 'buildParallelism', 'runtimeClosureMode')) {
    if ([string]$Cpp.$field -ne [string]$Rust.$field) { throw "paired provider manifests disagree on $field" }
  }
  if ([bool]$Cpp.sourceDirty -or [bool]$Rust.sourceDirty) { throw 'paired provider manifests are not clean' }
  return [ordered]@{
    complete = $true
    status = 'verified'
    record = $script:ProviderManifestRecord
    sourceHead = [string]$Cpp.sourceHead
    sourceStatusSha256 = [string]$Cpp.sourceStatusSha256
    sourceStatusLineCount = [UInt64]$Cpp.sourceStatusLineCount
    sourceDirty = $false
    source = [ordered]@{
      commit = [string]$Cpp.sourceHead
      dirty = $false
      statusSha256 = [string]$Cpp.sourceStatusSha256
      statusLineCount = [UInt64]$Cpp.sourceStatusLineCount
      status = 'clean'
      complete = $true
    }
    outputBackend = 'paired(cpp,rust)'
    utf16Backend = 'cpp'
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    selectors = [ordered]@{
      outputBackend = 'paired(cpp,rust)'
      utf16Backend = 'cpp'
      outputProductionPackage = $false
      utf16ProductionPackage = $false
      cpp = [ordered]@{
        backend = 'cpp'
        result = [string]$Cpp.selectorProof.result
        proofSha256 = [string]$Cpp.selectorProofSha256
        objectSha256 = [string]$Cpp.selectorProof.providerObjectSha256After
        objectFormat = Get-ProviderCandidate $Cpp.selectorProof @('providerObjectFormat')
        providerObjectFreshnessMethod = [string]$Cpp.selectorProof.providerObjectFreshnessMethod
        providerObjectAbsentBeforeBuild = [bool]$Cpp.selectorProof.providerObjectAbsentBeforeBuild
        compileLogSha256After = Get-ProviderCandidate $Cpp.selectorProof @('compileLogSha256After')
        compileLogSizeBytesAfter = Get-ProviderCandidate $Cpp.selectorProof @('compileLogSizeBytesAfter')
        compileCommandHasGl = Get-ProviderCandidate $Cpp.selectorProof @('compileCommandHasGl')
        compileCommandRustSelectorDefineCount = Get-ProviderCandidate $Cpp.selectorProof @('compileCommandRustSelectorDefineCount')
      }
      rust = [ordered]@{
        backend = 'rust'
        result = [string]$Rust.selectorProof.result
        proofSha256 = [string]$Rust.selectorProofSha256
        objectSha256 = [string]$Rust.selectorProof.providerObjectSha256After
        objectFormat = Get-ProviderCandidate $Rust.selectorProof @('providerObjectFormat')
        providerObjectFreshnessMethod = [string]$Rust.selectorProof.providerObjectFreshnessMethod
        providerObjectAbsentBeforeBuild = [bool]$Rust.selectorProof.providerObjectAbsentBeforeBuild
        compileLogSha256After = Get-ProviderCandidate $Rust.selectorProof @('compileLogSha256After')
        compileLogSizeBytesAfter = Get-ProviderCandidate $Rust.selectorProof @('compileLogSizeBytesAfter')
        compileCommandHasGl = Get-ProviderCandidate $Rust.selectorProof @('compileCommandHasGl')
        compileCommandRustSelectorDefineCount = Get-ProviderCandidate $Rust.selectorProof @('compileCommandRustSelectorDefineCount')
      }
      complete = $true
    }
    selectorProofSha256 = Get-TextSha256 ('paired-selector|cpp={0}|rust={1}' -f $Cpp.selectorProofSha256, $Rust.selectorProofSha256)
    host = [ordered]@{
      os = 'Windows'
      osVersion = [string]$Cpp.windowsImageIdentity
      windowsImageIdentity = [string]$Cpp.windowsImageIdentity
      windowsImageSha256 = [string]$Cpp.windowsImageSha256
      architecture = [string]$Platform.os.architecture
      cpuManufacturer = [string]$Platform.cpu.manufacturer
      cpuModel = [string]$Platform.cpu.model
      physicalCores = [UInt64]$Platform.cpu.physicalCores
      logicalProcessors = [UInt64]$Platform.cpu.logicalProcessors
      identitySha256 = [string]$Cpp.windowsImageSha256
      powerMode = [string]$Cpp.powerMode
      powerModeSha256 = [string]$Cpp.powerModeSha256
      buildParallelism = [int]$Cpp.buildParallelism
      complete = $true
    }
    power = [ordered]@{
      mode = [string]$Cpp.powerMode
      modeSha256 = [string]$Cpp.powerModeSha256
      complete = $true
    }
    toolchain = [ordered]@{
      msvc = [string]$Cpp.msvcIdentity
      rust = [string]$Cpp.rustToolchain
      rustLockSha256 = [string]$Cpp.rustLockSha256
      packagePlanSha256 = [string]$Cpp.packagePlanSha256
      buildCommandSha256 = [string]$Cpp.buildCommandSha256
      packageCommandSha256 = [string]$Cpp.packagePlanCommandSha256
      complete = $true
    }
    package = [ordered]@{
      planSha256 = [string]$Cpp.packagePlanSha256
      closureSha256 = Get-TextSha256 ('paired-runtime-closure|cpp={0}|rust={1}' -f $Cpp.runtimeClosureSha256, $Rust.runtimeClosureSha256)
      status = 'verified-exe-only'
      runtimeClosureMode = 'exe-only'
      cppRuntimeClosureSha256 = [string]$Cpp.runtimeClosureSha256
      rustRuntimeClosureSha256 = [string]$Rust.runtimeClosureSha256
      productionPackage = $false
      complete = $true
    }
    commands = [ordered]@{
      buildSha256 = Get-TextSha256 ('paired-build|cpp={0}|rust={1}' -f $Cpp.buildCommandSha256, $Rust.buildCommandSha256)
      packageSha256 = [string]$Cpp.packagePlanCommandSha256
      measurementSha256 = $MeasurementSha256
      runnerSha256 = $MeasurementSha256
      complete = $true
      cppBuildSha256 = [string]$Cpp.buildCommandSha256
      rustBuildSha256 = [string]$Rust.buildCommandSha256
      cppProbeSha256 = [string]$Cpp.runtimeProviderProbeCommandSha256
      rustProbeSha256 = [string]$Rust.runtimeProviderProbeCommandSha256
    }
    artifacts = @(
      [ordered]@{ backend = 'cpp'; artifact = 'tests1'; artifactSha256 = [string]$Cpp.tests1Sha256; sizeBytes = [UInt64]$Cpp.tests1SizeBytes }
      [ordered]@{ backend = 'rust'; artifact = 'tests1'; artifactSha256 = [string]$Rust.tests1Sha256; sizeBytes = [UInt64]$Rust.tests1SizeBytes }
    )
    manifests = @(
      [ordered]@{ backend = 'cpp'; sha256 = [string]$Cpp.manifestSha256 }
      [ordered]@{ backend = 'rust'; sha256 = [string]$Rust.manifestSha256 }
    )
    transaction = [ordered]@{
      status = 'committed'
      publication = 'atomic-directory-rename'
      cppTests1 = [ordered]@{ beforeVerified = [bool]$Cpp.transaction.tests1BeforeVerified; afterVerified = [bool]$Cpp.transaction.tests1AfterVerified; copyVerified = [bool]$Cpp.transaction.tests1CopyVerified; manifestVerified = [bool]$Cpp.transaction.tests1ManifestVerified; probeVerified = [bool]$Cpp.transaction.tests1ProbeVerified }
      rustTests1 = [ordered]@{ beforeVerified = [bool]$Rust.transaction.tests1BeforeVerified; afterVerified = [bool]$Rust.transaction.tests1AfterVerified; copyVerified = [bool]$Rust.transaction.tests1CopyVerified; manifestVerified = [bool]$Rust.transaction.tests1ManifestVerified; probeVerified = [bool]$Rust.transaction.tests1ProbeVerified }
      sourceBeforeVerified = [bool]$Cpp.transaction.sourceBeforeVerified -and [bool]$Rust.transaction.sourceBeforeVerified
      sourceAfterVerified = [bool]$Cpp.transaction.sourceAfterVerified -and [bool]$Rust.transaction.sourceAfterVerified
      hostBeforeVerified = [bool]$Cpp.transaction.hostBeforeVerified -and [bool]$Rust.transaction.hostBeforeVerified
      hostAfterVerified = [bool]$Cpp.transaction.hostAfterVerified -and [bool]$Rust.transaction.hostAfterVerified
      tests1BeforeVerified = [bool]$Cpp.transaction.tests1BeforeVerified -and [bool]$Rust.transaction.tests1BeforeVerified
      tests1AfterVerified = [bool]$Cpp.transaction.tests1AfterVerified -and [bool]$Rust.transaction.tests1AfterVerified
      tests1CopyVerified = [bool]$Cpp.transaction.tests1CopyVerified -and [bool]$Rust.transaction.tests1CopyVerified
      tests1ManifestVerified = [bool]$Cpp.transaction.tests1ManifestVerified -and [bool]$Rust.transaction.tests1ManifestVerified
      tests1ProbeVerified = [bool]$Cpp.transaction.tests1ProbeVerified -and [bool]$Rust.transaction.tests1ProbeVerified
      manifestGeneratedByProducer = [bool]$Cpp.transaction.manifestGeneratedByProducer -and [bool]$Rust.transaction.manifestGeneratedByProducer
      complete = $true
    }
  }
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
  if (Test-ProviderAcceptanceQualified -CollectOnly $true -ProvenanceComplete $true -Failures @()) {
    throw 'collect-only evidence was incorrectly qualified'
  }
  if (Test-ProviderAcceptanceQualified -CollectOnly $false -ProvenanceComplete $false -Failures @()) {
    throw 'manifest-omitted evidence was incorrectly qualified'
  }
  if (-not (Test-ProviderAcceptanceQualified -CollectOnly $false -ProvenanceComplete $true -Failures @())) {
    throw 'complete provider evidence was incorrectly rejected'
  }

  $syntheticCommit = '0' * 40
  $syntheticStatusHash = '1' * 64
  $syntheticWindowsHash = '2' * 64
  $syntheticPowerHash = '3' * 64
  $syntheticLockHash = '4' * 64
  $syntheticPackageHash = '5' * 64
  $syntheticBuildHash = '6' * 64
  $syntheticPackageCommandHash = '7' * 64
  $syntheticProbeCommandHash = '8' * 64
  $syntheticCppSelector = [pscustomobject][ordered]@{
    result = 'dumpbin-unresolved-refs-verified'
    outputBackend = 'cpp'
    utf16Backend = 'cpp'
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    utf16BenchmarkTelemetry = $false
    assemblyListings = $false
    providerObjectSha256After = ('9' * 64)
    providerObjectSizeBytesAfter = [UInt64]1
    providerObjectFreshnessMethod = 'exact-object-absence-v1'
    providerObjectAbsentBeforeBuild = $true
    unresolvedProviderSymbols = @()
    unresolvedProviderSymbolCount = 0
    rustArchiveResult = 'dumpbin-defined-exports-verified'
    rustArchiveSha256 = ('a' * 64)
    rustArchiveSizeBytes = [UInt64]1
    definedProviderSymbols = @($script:ProviderSymbols)
    definedProviderSymbolCount = $script:ProviderSymbols.Count
  }
  $syntheticCppBase = 'output=cpp|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=dumpbin-unresolved-refs-verified|symbols=|object-after={0}|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f $syntheticCppSelector.providerObjectSha256After
  $syntheticCppSelector | Add-Member -NotePropertyName selectorContractSha256 -NotePropertyValue (Get-TextSha256 ('{0}|archive-result={1}|archive={2}|defined={3}' -f
      (Get-TextSha256 $syntheticCppBase), $syntheticCppSelector.rustArchiveResult, $syntheticCppSelector.rustArchiveSha256,
      (@($syntheticCppSelector.definedProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join ',')))
  $syntheticRustSelector = [pscustomobject]([ordered]@{
    result = 'dumpbin-unresolved-refs-verified'
    outputBackend = 'rust'
    utf16Backend = 'cpp'
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    utf16BenchmarkTelemetry = $false
    assemblyListings = $false
    providerObjectSha256After = ('b' * 64)
    providerObjectSizeBytesAfter = [UInt64]1
    providerObjectFreshnessMethod = 'exact-object-absence-v1'
    providerObjectAbsentBeforeBuild = $true
    unresolvedProviderSymbols = @($script:ProviderSymbols)
    unresolvedProviderSymbolCount = $script:ProviderSymbols.Count
    rustArchiveResult = 'dumpbin-defined-exports-verified'
    rustArchiveSha256 = ('c' * 64)
    rustArchiveSizeBytes = [UInt64]1
    definedProviderSymbols = @($script:ProviderSymbols)
    definedProviderSymbolCount = $script:ProviderSymbols.Count
  })
  $syntheticRustBase = 'output=rust|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=dumpbin-unresolved-refs-verified|symbols={0}|object-after={1}|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f
    (@($syntheticRustSelector.unresolvedProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join ','), $syntheticRustSelector.providerObjectSha256After
  $syntheticRustSelector | Add-Member -NotePropertyName selectorContractSha256 -NotePropertyValue (Get-TextSha256 ('{0}|archive-result={1}|archive={2}|defined={3}' -f
      (Get-TextSha256 $syntheticRustBase), $syntheticRustSelector.rustArchiveResult, $syntheticRustSelector.rustArchiveSha256,
      (@($syntheticRustSelector.definedProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join ',')))
  $syntheticSelectorManifest = [pscustomobject][ordered]@{
    selectorProof = $syntheticCppSelector
    selectorProofSha256 = $syntheticCppSelector.selectorContractSha256
  }
  $validatedSelector = Get-ProviderSelectorProof -Manifest $syntheticSelectorManifest -Backend 'cpp'
  Assert-EqualValue $syntheticCppSelector.selectorContractSha256 $validatedSelector.selectorContractSha256 'selector proof self-test'
  $legacySelector = $syntheticSelectorManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  [void]$legacySelector.selectorProof.PSObject.Properties.Remove('providerObjectFreshnessMethod')
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $legacySelector -Backend 'cpp' } 'legacy freshness receipt self-test'
  $wrongFreshnessMethod = $syntheticSelectorManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $wrongFreshnessMethod.selectorProof.providerObjectFreshnessMethod = 'mtime-only'
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $wrongFreshnessMethod -Backend 'cpp' } 'wrong freshness method self-test'
  $falseFreshness = $syntheticSelectorManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $falseFreshness.selectorProof.providerObjectAbsentBeforeBuild = $false
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $falseFreshness -Backend 'cpp' } 'false freshness receipt self-test'
  $nonBooleanFreshness = $syntheticSelectorManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $nonBooleanFreshness.selectorProof.providerObjectAbsentBeforeBuild = 'true'
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $nonBooleanFreshness -Backend 'cpp' } 'non-boolean freshness receipt self-test'

  $syntheticLtcgObjectHash = 'd' * 64
  $syntheticLtcgCompileLogHash = 'e' * 64
  $syntheticLtcgArchiveHash = 'f' * 64
  $syntheticLtcgDefined = @($script:ProviderSymbols)
  $syntheticLtcgCppSelector = [pscustomobject][ordered]@{
    result = 'msvc-ltcg-compile-selector-verified'
    outputBackend = 'cpp'
    utf16Backend = 'cpp'
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    utf16BenchmarkTelemetry = $false
    assemblyListings = $false
    verificationMethod = 'msvc-ltcg-compile-selector'
    providerObjectFormat = 'msvc-ltcg-anonymous'
    providerObjectSha256After = $syntheticLtcgObjectHash
    providerObjectSizeBytesAfter = [UInt64]2
    providerObjectFreshnessMethod = 'exact-object-absence-v1'
    providerObjectAbsentBeforeBuild = $true
    compileLogSha256Before = ('c' * 64)
    compileLogSha256After = $syntheticLtcgCompileLogHash
    compileLogSizeBytesAfter = [UInt64]3
    compileLogProof = $true
    compileCommandHasGl = $true
    compileCommandRustSelectorDefineCount = 0
    unresolvedProviderSymbols = @()
    unresolvedProviderSymbolCount = 0
    rustArchiveResult = 'dumpbin-defined-exports-verified'
    rustArchiveSha256 = $syntheticLtcgArchiveHash
    rustArchiveSizeBytes = [UInt64]4
    definedProviderSymbols = $syntheticLtcgDefined
    definedProviderSymbolCount = $syntheticLtcgDefined.Count
  }
  $syntheticLtcgCppBase = 'output=cpp|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=msvc-ltcg-compile-selector-verified|method=msvc-ltcg-compile-selector|symbols=|object-after={0}|object-format=msvc-ltcg-anonymous|compile-log-after={1}|compile-log-size=3|compile-gl=True|compile-rust-selector-count=0|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f
    $syntheticLtcgObjectHash, $syntheticLtcgCompileLogHash
  $syntheticLtcgCppSelector | Add-Member -NotePropertyName selectorContractSha256 -NotePropertyValue (Get-TextSha256 ('{0}|archive-result={1}|archive={2}|defined={3}' -f
      (Get-TextSha256 $syntheticLtcgCppBase), $syntheticLtcgCppSelector.rustArchiveResult, $syntheticLtcgCppSelector.rustArchiveSha256,
      (@($syntheticLtcgDefined | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join ',')))
  $syntheticLtcgCppManifest = [pscustomobject][ordered]@{
    configuration = 'Release'
    selectorProof = $syntheticLtcgCppSelector
    selectorProofSha256 = $syntheticLtcgCppSelector.selectorContractSha256
  }
  $validatedLtcgCpp = Get-ProviderSelectorProof -Manifest $syntheticLtcgCppManifest -Backend 'cpp'
  Assert-EqualValue 'msvc-ltcg-compile-selector-verified' $validatedLtcgCpp.result 'C++ LTCG selector proof self-test'
  Assert-EqualValue 0 $validatedLtcgCpp.compileCommandRustSelectorDefineCount 'C++ LTCG selector count self-test'

  $syntheticLtcgRustSelector = $syntheticLtcgCppSelector | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $syntheticLtcgRustSelector.outputBackend = 'rust'
  $syntheticLtcgRustSelector.providerObjectSha256After = 'a' * 64
  $syntheticLtcgRustSelector.compileCommandRustSelectorDefineCount = 1
  $syntheticLtcgRustBase = 'output=rust|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=msvc-ltcg-compile-selector-verified|method=msvc-ltcg-compile-selector|symbols=|object-after={0}|object-format=msvc-ltcg-anonymous|compile-log-after={1}|compile-log-size=3|compile-gl=True|compile-rust-selector-count=1|object-freshness-method=exact-object-absence-v1|object-absent-before-build=true' -f
    $syntheticLtcgRustSelector.providerObjectSha256After, $syntheticLtcgCompileLogHash
  $syntheticLtcgRustSelector.selectorContractSha256 = Get-TextSha256 ('{0}|archive-result={1}|archive={2}|defined={3}' -f
      (Get-TextSha256 $syntheticLtcgRustBase), $syntheticLtcgRustSelector.rustArchiveResult, $syntheticLtcgRustSelector.rustArchiveSha256,
      (@($syntheticLtcgDefined | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join ','))
  $syntheticLtcgRustManifest = [pscustomobject][ordered]@{
    configuration = 'Release'
    selectorProof = $syntheticLtcgRustSelector
    selectorProofSha256 = $syntheticLtcgRustSelector.selectorContractSha256
  }
  $validatedLtcgRust = Get-ProviderSelectorProof -Manifest $syntheticLtcgRustManifest -Backend 'rust'
  Assert-EqualValue 'msvc-ltcg-compile-selector-verified' $validatedLtcgRust.result 'Rust LTCG selector proof self-test'
  Assert-EqualValue 1 $validatedLtcgRust.compileCommandRustSelectorDefineCount 'Rust LTCG selector count self-test'
  $badLtcgLog = $syntheticLtcgRustManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $badLtcgLog.selectorProof.compileLogSha256After = 'z' * 64
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $badLtcgLog -Backend 'rust' } 'LTCG compile log hash self-test'
  $badLtcgMacro = $syntheticLtcgRustManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $badLtcgMacro.selectorProof.compileCommandRustSelectorDefineCount = 0
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $badLtcgMacro -Backend 'rust' } 'LTCG selector macro self-test'
  $badLtcgMethod = $syntheticLtcgRustManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $badLtcgMethod.selectorProof.verificationMethod = 'wrong-method'
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $badLtcgMethod -Backend 'rust' } 'LTCG selector method self-test'
  $badSelectorManifest = $syntheticSelectorManifest | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $badSelectorManifest.selectorProof.outputBackend = 'rust'
  Assert-ProviderSelfTestRejects { Get-ProviderSelectorProof -Manifest $badSelectorManifest -Backend 'cpp' } 'selector mismatch self-test'
  $syntheticTransaction = [pscustomobject][ordered]@{
    sourceBeforeVerified = $true
    sourceAfterVerified = $true
    hostBeforeVerified = $true
    hostAfterVerified = $true
    tests1BeforeVerified = $true
    tests1AfterVerified = $true
    tests1CopyVerified = $true
    tests1ManifestVerified = $true
    tests1ProbeVerified = $true
    manifestGeneratedByProducer = $true
  }
  $syntheticCommon = [ordered]@{
    sourceHead = $syntheticCommit
    sourceDirty = $false
    sourceStatusSha256 = $syntheticStatusHash
    sourceStatusLineCount = [UInt64]0
    windowsImageIdentity = 'windows-self-test'
    windowsImageSha256 = $syntheticWindowsHash
    powerMode = 'Balanced'
    powerModeSha256 = $syntheticPowerHash
    msvcIdentity = 'msvc-self-test'
    rustToolchain = 'rust-self-test'
    rustLockSha256 = $syntheticLockHash
    packagePlanSha256 = $syntheticPackageHash
    buildCommandSha256 = $syntheticBuildHash
    packagePlanCommandSha256 = $syntheticPackageCommandHash
    runtimeProviderProbeCommandSha256 = $syntheticProbeCommandHash
    buildParallelism = 1
    tests1Sha256 = ('d' * 64)
    tests1SizeBytes = [UInt64]1
    runtimeClosureMode = 'exe-only'
    runtimeClosureSha256 = Get-TextSha256 ('exe-only|tests1={0}|size=1' -f ('d' * 64))
    manifestSha256 = ('e' * 64)
    transaction = $syntheticTransaction
  }
  $syntheticCpp = $syntheticCommon | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $syntheticCpp | Add-Member -NotePropertyName backend -NotePropertyValue 'cpp'
  $syntheticCpp | Add-Member -NotePropertyName selectorProof -NotePropertyValue $syntheticCppSelector
  $syntheticCpp | Add-Member -NotePropertyName selectorProofSha256 -NotePropertyValue $syntheticCppSelector.selectorContractSha256
  $syntheticRust = $syntheticCommon | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $syntheticRust.tests1Sha256 = 'f' * 64
  $syntheticRust.runtimeClosureSha256 = Get-TextSha256 ('exe-only|tests1={0}|size=1' -f ('f' * 64))
  $syntheticRust | Add-Member -NotePropertyName backend -NotePropertyValue 'rust'
  $syntheticRust | Add-Member -NotePropertyName selectorProof -NotePropertyValue $syntheticRustSelector
  $syntheticRust | Add-Member -NotePropertyName selectorProofSha256 -NotePropertyValue $syntheticRustSelector.selectorContractSha256
  $syntheticPlatform = [pscustomobject][ordered]@{
    os = [pscustomobject][ordered]@{ architecture = 'X64' }
    cpu = [pscustomobject][ordered]@{ manufacturer = 'self-test'; model = 'provider host'; physicalCores = [UInt64]1; logicalProcessors = [UInt64]1 }
  }
  $syntheticProvenance = New-ProviderProvenance -Cpp $syntheticCpp -Rust $syntheticRust -Platform $syntheticPlatform -MeasurementSha256 ('a' * 64)
  if (-not $syntheticProvenance.complete) { throw 'valid provider pair self-test was not complete' }
  [void](Assert-ProviderPayloadFree $syntheticProvenance)
  $sourceMismatch = $syntheticRust | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $sourceMismatch.sourceHead = '1' * 40
  Assert-ProviderSelfTestRejects { New-ProviderProvenance -Cpp $syntheticCpp -Rust $sourceMismatch -Platform $syntheticPlatform -MeasurementSha256 ('a' * 64) } 'mixed source self-test'
  $dirtyMismatch = $syntheticRust | ConvertTo-Json -Depth 10 | ConvertFrom-Json
  $dirtyMismatch.sourceDirty = $true
  Assert-ProviderSelfTestRejects { New-ProviderProvenance -Cpp $syntheticCpp -Rust $dirtyMismatch -Platform $syntheticPlatform -MeasurementSha256 ('a' * 64) } 'dirty source self-test'
  $badProbeManifest = [pscustomobject][ordered]@{
    runtimeProviderProbe = [pscustomobject][ordered]@{
      result = 'verified'
      expectedBackend = 'cpp'
      observedBackend = 'cpp'
      standalone = $true
      payloadFree = $true
      testFilter = $script:ProviderProbeFilter
      tests1Sha256 = ('1' * 64)
      tests1SizeBytes = [UInt64]2
    }
  }
  Assert-ProviderSelfTestRejects { Get-ProviderRuntimeProbe -Manifest $badProbeManifest -Backend 'cpp' -Executable ([pscustomobject][ordered]@{ sha256 = ('2' * 64); sizeBytes = [UInt64]1 }) } 'tests1 identity self-test'
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
  $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
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
  $cppManifestSupplied = -not [string]::IsNullOrWhiteSpace($CppBuildManifest)
  $rustManifestSupplied = -not [string]::IsNullOrWhiteSpace($RustBuildManifest)
  $manifestsSupplied = $cppManifestSupplied -or $rustManifestSupplied
  if ($manifestsSupplied -and (-not $cppManifestSupplied -or -not $rustManifestSupplied)) {
    throw 'CppBuildManifest and RustBuildManifest must be supplied together'
  }
  if (-not $CollectOnly -and -not $manifestsSupplied) {
    throw 'qualified provider measurement requires both CppBuildManifest and RustBuildManifest'
  }
  $provenance = $null
  $sourceState = $null
  if ($manifestsSupplied) {
    $sourceState = Get-ProviderSourceState
    $cppManifest = Get-ProviderBuildManifest -Path $CppBuildManifest -Backend 'cpp' `
      -Configuration $Configuration -Executable $cppExecutable -CurrentSource $sourceState
    $rustManifest = Get-ProviderBuildManifest -Path $RustBuildManifest -Backend 'rust' `
      -Configuration $Configuration -Executable $rustExecutable -CurrentSource $sourceState
    $measurementSha256 = Get-ProviderFileSha256 -Path $PSCommandPath
    $provenance = New-ProviderProvenance -Cpp $cppManifest -Rust $rustManifest `
      -Platform $platform -MeasurementSha256 $measurementSha256
    [void](Assert-ProviderPayloadFree $provenance)
  }
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
      if ($null -ne $provenance) {
        $manifestForProvider = if ($provider -eq 'cpp') { $cppManifest } else { $rustManifest }
        [void](Assert-ProviderManifestIdentity -Provenance $manifestForProvider)
        Assert-ProviderSourceState -Expected $sourceState -Actual (Get-ProviderSourceState) `
          -Context ("provider source state changed before pair {0} {1} launch" -f $pairIndex, $provider)
      }
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
  if ($null -ne $provenance) {
    [void](Assert-ProviderManifestIdentity -Provenance $cppManifest)
    [void](Assert-ProviderManifestIdentity -Provenance $rustManifest)
    Assert-ProviderSourceState -Expected $sourceState -Actual (Get-ProviderSourceState) `
      -Context 'provider source state changed after benchmark campaign'
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
  $provenanceComplete = $null -ne $provenance
  if (-not $provenanceComplete) {
    $acceptanceFailures += 'complete provider provenance is required'
  }
  if ($CollectOnly) {
    $acceptanceFailures += 'collect-only mode is unqualified'
  }

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
  $acceptanceQualified = Test-ProviderAcceptanceQualified -CollectOnly ([bool]$CollectOnly) `
    -ProvenanceComplete $provenanceComplete -Failures $acceptanceFailures
  $semanticPass = $true
  $pass = $semanticPass -and $acceptanceQualified -and $performancePass -and (-not $CollectOnly)
  $analysisBackend = if ($provenanceComplete) { [string]$provenance.outputBackend } else { 'unknown' }
  $analysisUtf16Backend = if ($provenanceComplete) { [string]$provenance.utf16Backend } else { 'unknown' }
  $analysisSourceHead = if ($provenanceComplete) { [string]$provenance.sourceHead } else { $null }
  $analysisSourceStatusSha256 = if ($provenanceComplete) { [string]$provenance.sourceStatusSha256 } else { $null }
  $analysisSelectorProofSha256 = if ($provenanceComplete) { [string]$provenance.selectorProofSha256 } else { $null }
  $analysisArtifacts = if ($provenanceComplete) {
    $provenance.artifacts
  } else {
    @(
      [ordered]@{ backend = 'cpp'; artifact = 'tests1'; artifactSha256 = $cppExecutable.sha256; sizeBytes = $cppExecutable.sizeBytes }
      [ordered]@{ backend = 'rust'; artifact = 'tests1'; artifactSha256 = $rustExecutable.sha256; sizeBytes = $rustExecutable.sizeBytes }
    )
  }
  $analysisPath = Join-Path $runDirectory 'analysis-v1.json'
  $analysis = [ordered]@{
    schemaVersion = $script:SchemaVersion
    record = 'analysis'
    backend = $analysisBackend
    outputBackend = $analysisBackend
    utf16Backend = $analysisUtf16Backend
    outputProductionPackage = $false
    utf16ProductionPackage = $false
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
    provenanceComplete = $provenanceComplete
    provenance = if ($provenanceComplete) { $provenance } else { [ordered]@{ complete = $false; status = 'unverified' } }
    sourceHead = $analysisSourceHead
    sourceCommit = $analysisSourceHead
    sourceDirty = if ($provenanceComplete) { [bool]$provenance.sourceDirty } else { $null }
    sourceStatusSha256 = $analysisSourceStatusSha256
    selectorProofSha256 = $analysisSelectorProofSha256
    host = if ($provenanceComplete) { $provenance.host } else { $platform }
    toolchain = if ($provenanceComplete) { $provenance.toolchain } else { $null }
    package = if ($provenanceComplete) { $provenance.package } else { $null }
    commands = if ($provenanceComplete) { $provenance.commands } else { $null }
    artifacts = $analysisArtifacts
    packagePlanSha256 = if ($provenanceComplete) { [string]$provenance.toolchain.packagePlanSha256 } else { $null }
    dependencyClosureSha256 = if ($provenanceComplete) { [string]$provenance.package.closureSha256 } else { $null }
    buildCommandSha256 = if ($provenanceComplete) { [string]$provenance.commands.buildSha256 } else { $null }
    packagePlanCommandSha256 = if ($provenanceComplete) { [string]$provenance.commands.packageSha256 } else { $null }
    measurementCommandSha256 = if ($provenanceComplete) { [string]$provenance.commands.measurementSha256 } else { $null }
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
