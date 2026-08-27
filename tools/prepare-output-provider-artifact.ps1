#requires -Version 5.1
<#!
.SYNOPSIS
  Produces one provenance-bound tests1.exe for qualified Output provider measurements.

.DESCRIPTION
  Builds the solution for exactly one Output backend, copies tests1.exe into an
  immutable private transaction, proves the selected provider and the fixed Rust
  archive exports, runs the copied lifecycle probe, and atomically publishes a
  payload-free build manifest. The GUI startup runtime-stage is intentionally not
  part of this producer.
#>
[CmdletBinding()]
param(
  [ValidateSet('cpp', 'rust')]
  [string]$Backend = 'cpp',
  [ValidateSet('x64')]
  [string]$Platform = 'x64',
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [string]$OutputDirectory = 'build/output-provider-artifacts',
  [ValidateRange(1, 256)]
  [int]$BuildParallelism = 1,
  [ValidateRange(30, 14400)]
  [int]$TimeoutSeconds = 3600,
  [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SchemaVersion = 1
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:ProbeFilter = 'CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle'
$script:ProviderSymbols = @(
  'sakura_output_provider_create_v1',
  'sakura_output_provider_apply_v1',
  'sakura_output_provider_snapshot_measure_v1',
  'sakura_output_provider_snapshot_write_v1',
  'sakura_output_provider_active_channel_v1',
  'sakura_output_provider_stop_v1',
  'sakura_output_provider_destroy_v1'
)

function Get-TextSha256 {
  param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
  $sha = [Security.Cryptography.SHA256]::Create()
  try {
    $encoding = New-Object Text.UTF8Encoding($false)
    return ([BitConverter]::ToString($sha.ComputeHash($encoding.GetBytes($Value)))).Replace('-', '').ToLowerInvariant()
  }
  finally { $sha.Dispose() }
}

function Get-FileSha256 {
  param([Parameter(Mandatory = $true)] [string]$Path)
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Test-Sha256 {
  param([AllowNull()] [object]$Value)
  return $Value -is [string] -and [string]$Value -match '^[0-9a-f]{64}$'
}

function Assert-NoReparseAncestors {
  param([Parameter(Mandatory = $true)] [string]$Path)
  $current = [IO.Path]::GetFullPath($Path)
  while (-not [string]::IsNullOrWhiteSpace($current)) {
    if ([IO.Directory]::Exists($current) -or [IO.File]::Exists($current)) {
      $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
      if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Owned artifact paths may not traverse a reparse point.'
      }
    }
    $parent = [IO.Directory]::GetParent($current)
    if ($null -eq $parent) { break }
    $current = $parent.FullName
  }
}

function Assert-RegularFile {
  param([Parameter(Mandatory = $true)] [string]$Path)
  $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
  if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
      (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'Required artifact is not a regular file.'
  }
  return $item
}

function Get-OptionalFileIdentity {
  param([Parameter(Mandatory = $true)] [string]$Path)
  if (-not [IO.File]::Exists($Path)) {
    return [pscustomobject][ordered]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }
  }
  $item = Assert-RegularFile $Path
  return [pscustomobject][ordered]@{
    exists = $true
    sha256 = Get-FileSha256 $Path
    sizeBytes = [UInt64]$item.Length
  }
}

function Get-FileIdentity {
  param([Parameter(Mandatory = $true)] [string]$Path)
  $identity = Get-OptionalFileIdentity $Path
  if (-not $identity.exists -or $identity.sizeBytes -lt 1) { throw 'Required artifact is missing or empty.' }
  return $identity
}

function Get-SourceState {
  $head = (& git -C $script:RepoRoot rev-parse --verify HEAD 2>$null | Out-String).Trim()
  if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40}$') { throw 'Git HEAD is unavailable.' }
  $status = (& git -C $script:RepoRoot status --porcelain=v1 --untracked-files=all 2>$null | Out-String)
  if ($LASTEXITCODE -ne 0) { throw 'Git source-state query failed.' }
  $canonical = ($status -replace "`r`n", "`n" -replace "`r", "`n").TrimEnd("`n")
  $lines = if ([string]::IsNullOrEmpty($canonical)) { @() } else { @($canonical -split "`n") }
  return [pscustomobject][ordered]@{
    head = $head.ToLowerInvariant()
    dirty = [bool]($lines.Count -ne 0)
    statusSha256 = Get-TextSha256 $canonical
    statusLineCount = [int]$lines.Count
  }
}

function Assert-SourceStateEqual {
  param([Parameter(Mandatory = $true)] [object]$Expected, [Parameter(Mandatory = $true)] [object]$Actual)
  if ($Expected.head -cne $Actual.head -or $Expected.dirty -ne $Actual.dirty -or
      $Expected.statusSha256 -cne $Actual.statusSha256 -or
      $Expected.statusLineCount -ne $Actual.statusLineCount) {
    throw 'The source checkout changed during artifact production.'
  }
}

function Get-EnvironmentOverrides {
  return [ordered]@{
    SAKURA_OUTPUT_BACKEND = $Backend
    SAKURA_UTF16_BACKEND = 'cpp'
    SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'
    SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'
    SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'
    SAKURA_GENERATE_ASSEMBLY_LISTINGS = 'false'
    SKIP_CREATE_GITHASH = '1'
    SAKURA_BUILD_JOBS = [string]$BuildParallelism
    MSBUILDDISABLENODEREUSE = '1'
    VSLANG = '1033'
  }
}

function Get-DescendantProcesses {
  param([Parameter(Mandatory = $true)] [int]$RootProcessId)
  $all = @(Get-CimInstance Win32_Process -ErrorAction Stop)
  $children = @{}
  foreach ($process in $all) {
    $parent = [int]$process.ParentProcessId
    if (-not $children.ContainsKey($parent)) { $children[$parent] = New-Object Collections.Generic.List[object] }
    [void]$children[$parent].Add($process)
  }
  $queue = New-Object Collections.Generic.Queue[int]
  $queue.Enqueue($RootProcessId)
  $result = New-Object Collections.Generic.List[object]
  while ($queue.Count -ne 0) {
    $parent = $queue.Dequeue()
    if (-not $children.ContainsKey($parent)) { continue }
    foreach ($child in $children[$parent]) {
      [void]$result.Add($child)
      $queue.Enqueue([int]$child.ProcessId)
    }
  }
  return $result.ToArray()
}

function Stop-OwnedProcessTree {
  param([Parameter(Mandatory = $true)] [int]$RootProcessId)
  $descendants = @(Get-DescendantProcesses $RootProcessId)
  foreach ($id in @($RootProcessId) + @($descendants | ForEach-Object { [int]$_.ProcessId })) {
    try { Stop-Process -Id $id -Force -ErrorAction Stop } catch { }
  }
}

function Invoke-OwnedProcess {
  param(
    [Parameter(Mandatory = $true)] [string]$FileName,
    [Parameter(Mandatory = $true)] [string]$Arguments,
    [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
    [Collections.IDictionary]$Environment = @{},
    [int]$Timeout = $TimeoutSeconds,
    [switch]$ReturnOutput
  )
  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = $FileName
  $start.Arguments = $Arguments
  $start.WorkingDirectory = $WorkingDirectory
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  foreach ($entry in $Environment.GetEnumerator()) { $start.EnvironmentVariables[[string]$entry.Key] = [string]$entry.Value }
  $process = New-Object Diagnostics.Process
  $process.StartInfo = $start
  try {
    if (-not $process.Start()) { throw 'The owned process did not start.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($Timeout * 1000)) {
      Stop-OwnedProcessTree $process.Id
      throw 'The owned process timed out.'
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) { throw ('The owned process failed with exit code {0}.' -f $process.ExitCode) }
    if ($ReturnOutput) { return [pscustomobject][ordered]@{ stdout = $stdout; stderr = $stderr; exitCode = 0 } }
    return [pscustomobject][ordered]@{ stdoutLength = $stdout.Length; stderrLength = $stderr.Length; exitCode = 0 }
  }
  finally { $process.Dispose() }
}

function Resolve-Executable {
  param([Parameter(Mandatory = $true)] [string]$Name)
  $command = Get-Command $Name -CommandType Application -ErrorAction Stop | Select-Object -First 1
  return (Assert-RegularFile $command.Source).FullName
}

function Resolve-Dumpbin {
  try { return Resolve-Executable 'dumpbin.exe' } catch { }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  if (-not [IO.File]::Exists($vswhere)) { $vswhere = Join-Path $script:RepoRoot 'tools/vswhere/vswhere.exe' }
  if ([IO.File]::Exists($vswhere)) {
    $result = Invoke-OwnedProcess -FileName $vswhere -Arguments '-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' -WorkingDirectory $script:RepoRoot -ReturnOutput
    $candidate = @($result.stdout -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -First 1)
    if ($candidate.Count -eq 1 -and [IO.File]::Exists($candidate[0])) { return (Assert-RegularFile $candidate[0]).FullName }
  }
  throw 'MSVC dumpbin.exe is unavailable.'
}

function Get-NormalizedProviderSymbols {
  param([Parameter(Mandatory = $true)] [string]$Text, [switch]$Definitions)
  $symbols = New-Object Collections.Generic.List[string]
  foreach ($line in @($Text -split "`r?`n")) {
    $pattern = if ($Definitions) {
      '(?i)^\s*[0-9A-F]+\s+[0-9A-F]+\s+SECT[0-9A-F]+\s+notype\s+\(\)\s+External\s+\|\s+(sakura_output_provider_[A-Za-z0-9_]+)\s*$'
    } else {
      '(?i)\bUNDEF\b.*\|\s*(sakura_output_provider_[A-Za-z0-9_]+)\s*$'
    }
    $match = [regex]::Match([string]$line, $pattern)
    if ($match.Success) {
      $symbol = $match.Groups[1].Value.ToLowerInvariant()
      if (-not $symbols.Contains($symbol)) { [void]$symbols.Add($symbol) }
    }
  }
  $symbols.Sort([StringComparer]::Ordinal)
  return $symbols.ToArray()
}

function Get-SelectorProof {
  param(
    [Parameter(Mandatory = $true)] [string]$Dumpbin,
    [Parameter(Mandatory = $true)] [object]$ObjectBefore,
    [Parameter(Mandatory = $true)] [object]$ObjectAfter,
    [Parameter(Mandatory = $true)] [string]$ObjectPath,
    [Parameter(Mandatory = $true)] [object]$Archive,
    [Parameter(Mandatory = $true)] [string]$ArchivePath
  )
  $objectDump = Invoke-OwnedProcess -FileName $Dumpbin -Arguments ('/symbols "{0}"' -f $ObjectPath) -WorkingDirectory $script:RepoRoot -ReturnOutput
  $unresolved = @(Get-NormalizedProviderSymbols $objectDump.stdout)
  $archiveDump = Invoke-OwnedProcess -FileName $Dumpbin -Arguments ('/symbols "{0}"' -f $ArchivePath) -WorkingDirectory $script:RepoRoot -ReturnOutput
  $defined = @(Get-NormalizedProviderSymbols $archiveDump.stdout -Definitions)
  $expected = @($script:ProviderSymbols | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object)
  if ($Backend -eq 'rust') {
    if (($unresolved -join '|') -cne ($expected -join '|')) { throw 'Rust selector proof is incomplete.' }
  }
  elseif ($unresolved.Count -ne 0) { throw 'C++ selector unexpectedly references Rust provider exports.' }
  if (($defined -join '|') -cne ($expected -join '|')) { throw 'Rust archive export proof is not exact.' }
  $base = 'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=dumpbin-unresolved-refs-verified|symbols={1}|object-after={2}' -f
    $Backend, ($unresolved -join ','), $ObjectAfter.sha256
  $baseHash = Get-TextSha256 $base
  $contract = '{0}|archive-result=dumpbin-defined-exports-verified|archive={1}|defined={2}' -f
    $baseHash, $Archive.sha256, ($defined -join ',')
  return [ordered]@{
    result = 'dumpbin-unresolved-refs-verified'
    outputBackend = $Backend
    utf16Backend = 'cpp'
    outputProductionPackage = $false
    utf16ProductionPackage = $false
    utf16BenchmarkTelemetry = $false
    assemblyListings = $false
    providerObjectSha256Before = if ($ObjectBefore.exists) { $ObjectBefore.sha256 } else { $null }
    providerObjectSha256After = $ObjectAfter.sha256
    providerObjectSizeBytesAfter = [UInt64]$ObjectAfter.sizeBytes
    unresolvedProviderSymbols = $unresolved
    unresolvedProviderSymbolCount = [int]$unresolved.Count
    rustArchiveResult = 'dumpbin-defined-exports-verified'
    rustArchiveSha256 = $Archive.sha256
    rustArchiveSizeBytes = [UInt64]$Archive.sizeBytes
    definedProviderSymbols = $defined
    definedProviderSymbolCount = [int]$defined.Count
    selectorContractSha256 = Get-TextSha256 $contract
  }
}

function Get-WindowsIdentity {
  $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
  $identity = 'Windows-{0}-{1}' -f ([string]$os.BuildNumber), [Environment]::Is64BitOperatingSystem
  return [ordered]@{ identity = $identity; sha256 = Get-TextSha256 $identity }
}

function Get-PowerIdentity {
  $powercfg = Resolve-Executable 'powercfg.exe'
  $result = Invoke-OwnedProcess -FileName $powercfg -Arguments '/getactivescheme' -WorkingDirectory $script:RepoRoot -ReturnOutput
  $rawHash = Get-TextSha256 (($result.stdout -replace "`r`n", "`n").Trim())
  $identity = 'powercfg-active-{0}' -f $rawHash
  return [ordered]@{ identity = $identity; sha256 = Get-TextSha256 $identity }
}

function Write-JsonAtomic {
  param([Parameter(Mandatory = $true)] [string]$Path, [Parameter(Mandatory = $true)] [object]$Value)
  $temporary = '{0}.{1}.tmp' -f $Path, ([Guid]::NewGuid().ToString('N'))
  try {
    $json = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($temporary, $json, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::Move($temporary, $Path)
  }
  finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Assert-PayloadFree {
  param([Parameter(Mandatory = $true)] [object]$Value)
  $json = $Value | ConvertTo-Json -Depth 20 -Compress
  if ($json -match '(?i)"(?:path|filepath|directory|command|commandLine|arguments|argv|stdout|stderr|caption|text|document|profile|exception|message|detail|label|id|payload)"\s*:') {
    throw 'Manifest contains a payload-bearing property.'
  }
  if ($json -match '(?i)[A-Za-z]:\\\\|\\\\\\\\') { throw 'Manifest contains a path-shaped value.' }
}

function Assert-ProducedManifest {
  param(
    [Parameter(Mandatory = $true)] [string]$Path,
    [Parameter(Mandatory = $true)] [object]$ExpectedArtifact,
    [Parameter(Mandatory = $true)] [object]$ExpectedSelector,
    [Parameter(Mandatory = $true)] [string]$ExpectedClosure
  )
  [void](Assert-RegularFile $Path)
  try { $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop }
  catch { throw 'Produced provider manifest is not valid JSON.' }
  if ([UInt64]$manifest.schemaVersion -ne 1 -or [string]$manifest.record -cne 'output-provider-build-manifest' -or
      -not [bool]$manifest.payloadFree -or [string]$manifest.status -cne 'committed' -or
      [string]$manifest.backend -cne $Backend -or [string]$manifest.platform -cne 'x64' -or
      [string]$manifest.configuration -cne $Configuration -or [bool]$manifest.sourceDirty -or
      [string]$manifest.tests1Sha256 -cne [string]$ExpectedArtifact.sha256 -or
      [UInt64]$manifest.tests1SizeBytes -ne [UInt64]$ExpectedArtifact.sizeBytes -or
      [string]$manifest.runtimeClosureMode -cne 'exe-only' -or
      [string]$manifest.runtimeClosureSha256 -cne $ExpectedClosure -or
      [string]$manifest.selectorProofSha256 -cne [string]$ExpectedSelector.selectorContractSha256 -or
      [string]$manifest.runtimeProviderProbe.testFilter -cne $script:ProbeFilter -or
      [string]$manifest.transaction.publication -cne 'atomic-directory-rename') {
    throw 'Produced provider manifest failed its read-back contract.'
  }
  Assert-PayloadFree $manifest
}

function Acquire-ExclusiveLock {
  param([Parameter(Mandatory = $true)] [string]$Path)
  try { return [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
  catch { throw 'Another producer owns the selected configuration lock.' }
}

function Remove-OwnedDirectory {
  param([Parameter(Mandatory = $true)] [string]$Path)
  if (-not [IO.Directory]::Exists($Path)) { return }
  Assert-NoReparseAncestors $Path
  [IO.Directory]::Delete($Path, $true)
}

function Invoke-SelfTest {
  if ($Backend -cnotin @('cpp', 'rust') -or $Platform -cne 'x64' -or $Configuration -cnotin @('Debug', 'Release')) {
    throw 'Selector exactness self-test failed.'
  }
  $artifactHash = 'a' * 64
  $closure = Get-TextSha256 ('exe-only|tests1={0}|size=1' -f $artifactHash)
  if (-not (Test-Sha256 $closure)) { throw 'Runtime closure self-test failed.' }
  $base = 'output=cpp|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result=dumpbin-unresolved-refs-verified|symbols=|object-after={0}' -f ('b' * 64)
  $selector = Get-TextSha256 ('{0}|archive-result=dumpbin-defined-exports-verified|archive={1}|defined={2}' -f
    (Get-TextSha256 $base), ('c' * 64), (@($script:ProviderSymbols | Sort-Object) -join ','))
  if (-not (Test-Sha256 $selector)) { throw 'Selector contract self-test failed.' }
  $root = Join-Path ([IO.Path]::GetTempPath()) ('sakura-output-provider-selftest-{0}' -f ([Guid]::NewGuid().ToString('N')))
  [void][IO.Directory]::CreateDirectory($root)
  try {
    $lockPath = Join-Path $root 'configuration.lock'
    $first = Acquire-ExclusiveLock $lockPath
    try {
      $rejected = $false
      try { $second = Acquire-ExclusiveLock $lockPath; $second.Dispose() } catch { $rejected = $true }
      if (-not $rejected) { throw 'Exclusive lock self-test failed.' }
    }
    finally { $first.Dispose(); [IO.File]::Delete($lockPath) }
  }
  finally { Remove-OwnedDirectory $root }
  Write-Output 'PASS prepare-output-provider-artifact.ps1 self-tests'
}

function Invoke-Producer {
  if ($Backend -cnotin @('cpp', 'rust')) { throw 'Backend must be exact lowercase cpp or rust.' }
  if ($Platform -cne 'x64') { throw 'Platform must be exact x64.' }
  if ($Configuration -cnotin @('Debug', 'Release')) { throw 'Configuration must be exact Debug or Release.' }
  $outputRoot = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
  } else {
    [IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))
  }
  $repoPrefix = $script:RepoRoot.TrimEnd('\') + '\'
  $buildRoot = [IO.Path]::GetFullPath((Join-Path $script:RepoRoot 'build')).TrimEnd('\')
  $buildPrefix = $buildRoot + '\'
  $insideRepository = $outputRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)
  $insideBuild = $outputRoot.Equals($buildRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputRoot.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)
  if ($insideRepository -and -not $insideBuild) {
    throw 'Repository-local provider artifacts must stay under the ignored build directory.'
  }
  Assert-NoReparseAncestors $outputRoot
  [void][IO.Directory]::CreateDirectory($outputRoot)
  $lockRoot = Join-Path $outputRoot '.locks'
  [void][IO.Directory]::CreateDirectory($lockRoot)
  $lockPath = Join-Path $lockRoot ('{0}.lock' -f $Configuration.ToLowerInvariant())
  $lock = $null
  $lockOwned = $false
  $transaction = $null
  $finalRoot = $null
  $movedFinalRoot = $false
  $published = $false
  try {
    $lock = Acquire-ExclusiveLock $lockPath
    $lockOwned = $true
    $configurationRoot = Join-Path $outputRoot $Configuration.ToLowerInvariant()
    [void][IO.Directory]::CreateDirectory($configurationRoot)
    $finalRoot = Join-Path $configurationRoot $Backend
    if ([IO.Directory]::Exists($finalRoot) -or [IO.File]::Exists($finalRoot)) { throw 'Selected provider artifact already exists; refusing overwrite.' }
    $transaction = Join-Path $configurationRoot ('.{0}-transaction-{1}' -f $Backend, ([Guid]::NewGuid().ToString('N')))
    [void][IO.Directory]::CreateDirectory($transaction)
    $sourceBefore = Get-SourceState
    if ($sourceBefore.dirty) { throw 'Qualified provider artifact production requires a clean checkout.' }
    $windowsBefore = Get-WindowsIdentity
    $powerBefore = Get-PowerIdentity
    $context = 'msvc-x64-{0}' -f $Configuration.ToLowerInvariant()
    $testsSource = Join-Path $script:RepoRoot ('x64/{0}/tests1.exe' -f $Configuration)
    $objectSource = Join-Path $script:RepoRoot ('build/x64/{0}/sakura_core/OutputServiceRustProvider.obj' -f $Configuration)
    $rustProfile = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
    $archiveSource = Join-Path $script:RepoRoot ('build/x64/{0}/rust/native/x86_64-pc-windows-msvc/{1}/sakura_native_ffi.lib' -f $Configuration, $rustProfile)
    $testsBefore = Get-OptionalFileIdentity $testsSource
    $objectBefore = Get-OptionalFileIdentity $objectSource
    $rustLock = Get-FileSha256 (Join-Path $script:RepoRoot 'rust/native/Cargo.lock')
    $dumpbin = Resolve-Dumpbin
    $msvcIdentity = 'dumpbin-{0}' -f (Get-Item -LiteralPath $dumpbin).VersionInfo.FileVersion
    $rustc = Resolve-Executable 'rustc.exe'
    $rustIdentity = (Invoke-OwnedProcess -FileName $rustc -Arguments '--version' -WorkingDirectory $script:RepoRoot -ReturnOutput).stdout.Trim()
    if ([string]::IsNullOrWhiteSpace($rustIdentity)) { throw 'Rust toolchain identity is empty.' }
    $environment = Get-EnvironmentOverrides
    $py = Resolve-Executable 'py.exe'
    $buildModel = Join-Path $script:RepoRoot 'tools/build/sakura_build.py'
    $packagePlanResult = Invoke-OwnedProcess -FileName $py -Arguments ('-3 "{0}" --format json package plan sakura_app --context {1}' -f $buildModel, $context) -WorkingDirectory $script:RepoRoot -Environment $environment -ReturnOutput
    try { $packagePlan = $packagePlanResult.stdout | ConvertFrom-Json -ErrorAction Stop } catch { throw 'Canonical package plan is not valid JSON.' }
    $packagePlanHash = if ($null -ne $packagePlan.PSObject.Properties['plan_hash']) {
      [string]$packagePlan.plan_hash
    } elseif ($null -ne $packagePlan.PSObject.Properties['planHash']) {
      [string]$packagePlan.planHash
    } else { '' }
    $packagePlanHash = $packagePlanHash -replace '^(?i:sha256:)', ''
    if (-not (Test-Sha256 $packagePlanHash) -or -not [bool]$packagePlan.required) { throw 'Canonical package plan is incomplete.' }
    $packageCommandHash = Get-TextSha256 ('package-plan|{0}|x64|{1}' -f $Backend, $Configuration)
    $buildCommandHash = Get-TextSha256 ('build-sln.bat|x64|{0}|SAKURA_OUTPUT_BACKEND={1}|SAKURA_UTF16_BACKEND=cpp|SAKURA_OUTPUT_PRODUCTION_PACKAGE=false|SAKURA_UTF16_PRODUCTION_PACKAGE=false|SAKURA_UTF16_BENCHMARK_TELEMETRY=false|SAKURA_GENERATE_ASSEMBLY_LISTINGS=false|SKIP_CREATE_GITHASH=1|SAKURA_BUILD_JOBS={2}|MSBUILDDISABLENODEREUSE=1|VSLANG=1033' -f $Configuration, $Backend, $BuildParallelism)
    $cmd = Resolve-Executable 'cmd.exe'
    [void](Invoke-OwnedProcess -FileName $cmd -Arguments ('/d /s /c "call build-sln.bat x64 {0}"' -f $Configuration) -WorkingDirectory $script:RepoRoot -Environment $environment)
    Assert-SourceStateEqual $sourceBefore (Get-SourceState)
    $testsAfter = Get-FileIdentity $testsSource
    $objectAfter = Get-FileIdentity $objectSource
    $archive = Get-FileIdentity $archiveSource
    $copiedTests = Join-Path $transaction 'tests1.exe'
    [IO.File]::Copy($testsSource, $copiedTests, $false)
    $copiedIdentity = Get-FileIdentity $copiedTests
    if ($copiedIdentity.sha256 -cne $testsAfter.sha256 -or $copiedIdentity.sizeBytes -ne $testsAfter.sizeBytes) { throw 'Copied tests1 identity changed.' }
    $selectorProof = Get-SelectorProof $dumpbin $objectBefore $objectAfter $objectSource $archive $archiveSource
    $probeCommandHash = Get-TextSha256 ('tests1.exe|--gtest_filter={0}|--gtest_color=no|backend={1}' -f $script:ProbeFilter, $Backend)
    [void](Invoke-OwnedProcess -FileName $copiedTests -Arguments ('--gtest_filter={0} --gtest_color=no' -f $script:ProbeFilter) -WorkingDirectory $transaction)
    $runtimeClosureHash = Get-TextSha256 ('exe-only|tests1={0}|size={1}' -f
      $copiedIdentity.sha256, ([UInt64]$copiedIdentity.sizeBytes).ToString([Globalization.CultureInfo]::InvariantCulture))
    $windowsAfter = Get-WindowsIdentity
    $powerAfter = Get-PowerIdentity
    if ($windowsBefore.sha256 -cne $windowsAfter.sha256 -or $powerBefore.sha256 -cne $powerAfter.sha256) { throw 'Host or power identity changed.' }
    Assert-SourceStateEqual $sourceBefore (Get-SourceState)
    $manifest = [ordered]@{
      schemaVersion = $script:SchemaVersion
      record = 'output-provider-build-manifest'
      payloadFree = $true
      status = 'committed'
      backend = $Backend
      platform = 'x64'
      configuration = $Configuration
      sourceHead = $sourceBefore.head
      sourceDirty = $false
      sourceStatusSha256 = $sourceBefore.statusSha256
      sourceStatusLineCount = $sourceBefore.statusLineCount
      outputBackend = $Backend
      utf16Backend = 'cpp'
      outputProductionPackage = $false
      utf16ProductionPackage = $false
      utf16BenchmarkTelemetry = $false
      assemblyListings = $false
      tests1Sha256 = $copiedIdentity.sha256
      tests1SizeBytes = [UInt64]$copiedIdentity.sizeBytes
      tests1Sha256Before = if ($testsBefore.exists) { $testsBefore.sha256 } else { $null }
      tests1SizeBytesBefore = [UInt64]$testsBefore.sizeBytes
      artifacts = @([ordered]@{ backend = $Backend; artifactSha256 = $copiedIdentity.sha256; sizeBytes = [UInt64]$copiedIdentity.sizeBytes })
      windowsImageIdentity = $windowsBefore.identity
      windowsImageSha256 = $windowsBefore.sha256
      powerMode = $powerBefore.identity
      powerModeSha256 = $powerBefore.sha256
      buildParallelism = $BuildParallelism
      msvcIdentity = $msvcIdentity
      rustToolchain = $rustIdentity
      rustLockSha256 = $rustLock
      packagePlanSha256 = $packagePlanHash
      buildCommandSha256 = $buildCommandHash
      packagePlanCommandSha256 = $packageCommandHash
      runtimeProviderProbeCommandSha256 = $probeCommandHash
      runtimeClosureMode = 'exe-only'
      runtimeClosureSha256 = $runtimeClosureHash
      selectorProof = $selectorProof
      selectorProofSha256 = $selectorProof.selectorContractSha256
      runtimeProviderProbe = [ordered]@{
        result = 'verified'
        expectedBackend = $Backend
        observedBackend = $Backend
        standalone = $true
        payloadFree = $true
        testFilter = $script:ProbeFilter
        tests1Sha256 = $copiedIdentity.sha256
        tests1SizeBytes = [UInt64]$copiedIdentity.sizeBytes
        runtimeClosureMode = 'exe-only'
        runtimeClosureSha256 = $runtimeClosureHash
      }
      transaction = [ordered]@{
        status = 'committed'
        publication = 'atomic-directory-rename'
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
    }
    Assert-PayloadFree $manifest
    $manifestPath = Join-Path $transaction 'output-provider-build-manifest.json'
    Write-JsonAtomic $manifestPath $manifest
    Assert-ProducedManifest $manifestPath $copiedIdentity $selectorProof $runtimeClosureHash
    if ((Get-FileSha256 $copiedTests) -cne $copiedIdentity.sha256) { throw 'Copied tests1 changed before publication.' }
    [IO.Directory]::Move($transaction, $finalRoot)
    $transaction = $null
    $movedFinalRoot = $true
    $finalManifest = Join-Path $finalRoot 'output-provider-build-manifest.json'
    $finalTests = Join-Path $finalRoot 'tests1.exe'
    Assert-ProducedManifest $finalManifest $copiedIdentity $selectorProof $runtimeClosureHash
    if ((Get-FileSha256 $finalTests) -cne $copiedIdentity.sha256) { throw 'Published tests1 changed identity.' }
    Assert-SourceStateEqual $sourceBefore (Get-SourceState)
    $published = $true
    $summary = [ordered]@{
      schemaVersion = 1
      record = 'output-provider-artifact-producer'
      payloadFree = $true
      status = 'committed'
      backend = $Backend
      platform = 'x64'
      configuration = $Configuration
      manifestSha256 = Get-FileSha256 $finalManifest
      tests1Sha256 = $copiedIdentity.sha256
      runtimeClosureSha256 = $runtimeClosureHash
      selectorProofSha256 = $selectorProof.selectorContractSha256
    }
    $summary | ConvertTo-Json -Depth 10 -Compress | Write-Output
  }
  finally {
    $cleanupFailures = New-Object Collections.Generic.List[string]
    if ($null -ne $transaction -and [IO.Directory]::Exists($transaction)) {
      try { Remove-OwnedDirectory $transaction } catch { [void]$cleanupFailures.Add('transaction') }
    }
    if (-not $published -and $movedFinalRoot -and $null -ne $finalRoot -and [IO.Directory]::Exists($finalRoot)) {
      try { Remove-OwnedDirectory $finalRoot } catch { [void]$cleanupFailures.Add('publication') }
    }
    if ($null -ne $lock) {
      try { $lock.Dispose() } catch { [void]$cleanupFailures.Add('lock-handle') }
    }
    if ($lockOwned -and [IO.File]::Exists($lockPath)) {
      try { [IO.File]::Delete($lockPath) } catch { [void]$cleanupFailures.Add('lock-file') }
    }
    if ($cleanupFailures.Count -ne 0 -or ($lockOwned -and [IO.File]::Exists($lockPath))) {
      throw 'Provider artifact cleanup did not reach a verified terminal state.'
    }
  }
}

try {
  if ($SelfTest) { Invoke-SelfTest; exit 0 }
  Invoke-Producer
  exit 0
}
catch {
  Write-Error $_.Exception.Message
  exit 1
}
