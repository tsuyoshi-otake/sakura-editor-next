#requires -Version 5.1
<#
.SYNOPSIS
  Collects paired GUI startup evidence for the C++ and Rust sakura.exe builds.

.DESCRIPTION
  This is an opt-in evidence runner.  It launches the two explicitly supplied
  sakura.exe artifacts with the same fixed sample and a fresh profile for every
  launch.  Warmups and measured launches are interleaved in a deterministic
  order.  The report contains hashes and timings only; it never serializes a
  user path, document text, window caption, command line, or raw exception.

  The tests1 Output-provider microbenchmark is a separate concern.  This file
  measures the GUI startup boundary and does not invoke tests1.exe.
#>
[CmdletBinding()]
param(
    [Alias('CppExe')]
    [string]$CppSakuraExe,
    [Alias('RustExe')]
    [string]$RustSakuraExe,
    [Alias('CppManifest', 'CppProvenanceManifest')]
    [string]$CppBuildManifest,
    [Alias('RustManifest', 'RustProvenanceManifest')]
    [string]$RustBuildManifest,
    [Alias('CppRuntimeStage', 'CppRuntimeStageDir')]
    [string]$CppRuntimeStageDirectory,
    [Alias('RustRuntimeStage', 'RustRuntimeStageDir')]
    [string]$RustRuntimeStageDirectory,
    [Alias('SampleFile')]
    [string]$StartupSample = 'tools/startup-benchmark-sample.md',
    [string]$ResultDirectory = 'build/output-startup-benchmarks',
    [Alias('Warmups', 'WarmupIterations')]
    [ValidateRange(1, 1000)]
    [int]$WarmupLaunches = 5,
    [Alias('Iterations', 'MeasuredIterations')]
    [ValidateRange(1, 1000)]
    [int]$MeasuredLaunches = 30,
    [string]$FirstBackend = 'cpp',
    [string]$Platform = 'x64',
    [string]$Configuration = 'Debug',
    [UInt64]$AffinityMask = 1,
    [switch]$CollectOnly,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Dot-source the existing startup implementation only as a library.  Its main
# entry point is guarded by -LibraryOnly, so self-test cannot launch a GUI.
$runSelfTest = [bool]$SelfTest
$sharedStartupScript = Join-Path $PSScriptRoot 'measure-startup-performance.ps1'
if (-not (Test-Path -LiteralPath $sharedStartupScript -PathType Leaf)) {
    throw 'The shared startup measurement implementation is missing.'
}
$script:PairedScriptPath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$script:SharedStartupScriptPath = [IO.Path]::GetFullPath($sharedStartupScript)
. $sharedStartupScript -LibraryOnly
$SelfTest = $runSelfTest

$script:PairedSchemaVersion = 1
$script:PairedManifestSchemaVersion = 1
$script:PairedStartupTimeoutMs = 30000
$script:PairedCloseTimeoutMs = 3000
$script:PairedPollIntervalMs = 25
$script:PairedMinimumWarmupLaunches = 5
$script:PairedMinimumMeasuredLaunches = 30
$script:PairedPrimaryMetric = 'documentReadyMs'
$script:PairedMedianRelativeLimitPercent = 2.0
$script:PairedMedianAbsoluteLimitMs = 1.0
$script:PairedP95RelativeLimitPercent = 5.0
$script:PairedMetricNames = @(
    'processApiReturnMs', 'topLevelHwndMs', 'visibleMs', 'captionReadyMs',
    'inputIdleMs', 'documentReadyMs'
)
$script:ForbiddenEvidencePropertyPattern =
    '(?i)"(?:path|imagePath|commandLine|arguments|caption|text|document|profileName|sampleMarkdown|sakuraExe|outputDirectory|exception|message|detail|bundlePath|sidecarPath|profilePath|executablePath|sourcePath|manifestPath|runtimeStagePath|samplePath|dependencyPath)"\s*:'

function Get-PairedProperty {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string[]]$Names
    )
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Get-PairedNestedProperty {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string[]]$Sections,
        [Parameter(Mandatory = $true)] [string[]]$Names
    )
    foreach ($section in $Sections) {
        $nested = Get-PairedProperty $Object @($section)
        $value = Get-PairedProperty $nested $Names
        if ($null -ne $value) { return $value }
    }
    return $null
}

function Test-PairedSha256 {
    param([object]$Value)
    return $null -ne $Value -and [string]$Value -match '^[0-9a-fA-F]{64}$'
}

function Test-PairedNonEmptyIdentity {
    param([object]$Value)
    return $null -ne $Value -and -not [string]::IsNullOrWhiteSpace([string]$Value) -and
        [string]$Value -notmatch '[\r\n]'
}

function Assert-PairedAffinityMask {
    param([Parameter(Mandatory = $true)] [UInt64]$Mask)
    if ($Mask -eq 0) { throw 'AffinityMask must be nonzero.' }
    return $Mask
}

function Get-PairedCanonicalBackend {
    param([Parameter(Mandatory = $true)] [string]$Backend)
    $canonical = $Backend.Trim().ToLowerInvariant()
    if ($canonical -ne 'cpp' -and $canonical -ne 'rust') {
        throw 'FirstBackend must be cpp or rust.'
    }
    return $canonical
}

function Get-PairedCanonicalPlatform {
    param([Parameter(Mandatory = $true)] [string]$Value)
    $canonical = $Value.Trim().ToLowerInvariant()
    if ($canonical -ne 'x64') { throw 'Platform must be x64.' }
    return $canonical
}

function Get-PairedCanonicalConfiguration {
    param([Parameter(Mandatory = $true)] [string]$Value)
    $canonical = $Value.Trim()
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($canonical, 'Debug') -and
        -not [StringComparer]::OrdinalIgnoreCase.Equals($canonical, 'Release')) {
        throw 'Configuration must be Debug or Release.'
    }
    if ([StringComparer]::OrdinalIgnoreCase.Equals($canonical, 'Debug')) { return 'Debug' }
    return 'Release'
}

function Assert-PairedEqual {
    param([object]$Expected, [object]$Actual, [string]$Context)
    if ($Expected -ne $Actual) {
        throw "$Context expected '$Expected' but got '$Actual'."
    }
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)] [string]$Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($Value)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Resolve-PairedInputFile {
    param([Parameter(Mandatory = $true)] [string]$Path, [Parameter(Mandatory = $true)] [string]$Name)
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Name is required." }
    try { $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path }
    catch { throw "$Name could not be resolved." }
    $item = Get-Item -LiteralPath $resolved -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) -or
        -not $item.Exists) {
        throw "$Name is not a regular non-reparse file."
    }
    return [IO.Path]::GetFullPath($resolved)
}

function Get-PairedArtifactIdentity {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'A supplied sakura.exe is not a regular non-reparse file.'
    }
    return [pscustomobject][ordered]@{
        path = [IO.Path]::GetFullPath($Path)
        sha256 = Get-Sha256 $Path
        sizeBytes = [UInt64]$item.Length
    }
}

function Assert-PairedArtifactUnchanged {
    param([Parameter(Mandatory = $true)] [object]$Expected)
    $actual = Get-PairedArtifactIdentity -Path $Expected.path
    if ($actual.sha256 -ne $Expected.sha256 -or $actual.sizeBytes -ne $Expected.sizeBytes) {
        throw 'A sakura.exe artifact changed during the paired measurement.'
    }
}

function Get-PairedSampleIdentity {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The startup sample must be a regular non-reparse file.'
    }
    $lines = @([IO.File]::ReadAllLines([IO.Path]::GetFullPath($Path))).Count
    if ($lines -le 30) {
        throw 'The startup sample must contain more than the initial scrollbar placeholder range.'
    }
    return [pscustomobject][ordered]@{
        path = [IO.Path]::GetFullPath($Path)
        sha256 = Get-Sha256 $Path
        sizeBytes = [UInt64]$item.Length
        physicalLines = [int]$lines
    }
}

function Assert-PairedSampleUnchanged {
    param(
        [Parameter(Mandatory = $true)] [object]$Expected,
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Label
    )
    $actual = Get-PairedSampleIdentity $Path
    if ($actual.sha256 -ne $Expected.sha256 -or
        $actual.sizeBytes -ne $Expected.sizeBytes -or
        $actual.physicalLines -ne $Expected.physicalLines) {
        throw "$Label changed during the paired measurement."
    }
    return $actual
}

function Assert-PairedOwnedSamplePath {
    param(
        [Parameter(Mandatory = $true)] [string]$SamplePath,
        [Parameter(Mandatory = $true)] [string]$ResultRoot,
        [Parameter(Mandatory = $true)] [string]$SampleName
    )
    $resolvedSample = Get-NormalizedPath $SamplePath
    $resolvedRoot = Get-NormalizedPath $ResultRoot
    if ((Split-Path -Parent $resolvedSample) -ne $resolvedRoot -or
        [IO.Path]::GetFileName($resolvedSample) -ne $SampleName.ToUpperInvariant() -or
        -not $SampleName.StartsWith('startup-probe-sample-', [StringComparison]::Ordinal)) {
        throw 'Refusing an unsafe campaign sample path.'
    }
    $item = Get-Item -LiteralPath $ResultRoot -Force -ErrorAction Stop
    if ($item -isnot [IO.DirectoryInfo] -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The paired result root must be a regular non-reparse directory.'
    }
}

function New-PairedSampleCopy {
    param(
        [Parameter(Mandatory = $true)] [string]$SourcePath,
        [Parameter(Mandatory = $true)] [string]$ResultRoot,
        [Parameter(Mandatory = $true)] [string]$RunId,
        [Parameter(Mandatory = $true)] [object]$SourceIdentity
    )
    $sampleName = 'startup-probe-sample-{0}.md' -f $RunId
    $samplePath = Join-Path ([IO.Path]::GetFullPath($ResultRoot)) $sampleName
    Assert-PairedOwnedSamplePath $samplePath $ResultRoot $sampleName
    if (Test-Path -LiteralPath $samplePath) { throw 'The generated campaign sample already exists.' }
    [IO.File]::Copy($SourcePath, $samplePath, $false)
    try {
        $copy = Get-PairedSampleIdentity $samplePath
        if ($copy.sha256 -ne $SourceIdentity.sha256 -or
            $copy.sizeBytes -ne $SourceIdentity.sizeBytes -or
            $copy.physicalLines -ne $SourceIdentity.physicalLines) {
            throw 'The campaign-owned sample copy did not retain the source identity.'
        }
        return [pscustomobject][ordered]@{
            path = [IO.Path]::GetFullPath($samplePath)
            sha256 = $copy.sha256
            sizeBytes = [UInt64]$copy.sizeBytes
            physicalLines = [int]$copy.physicalLines
            cleanupVerified = $false
        }
    }
    catch {
        try { [IO.File]::Delete($samplePath) } catch { }
        throw
    }
}

function Remove-PairedSampleCopy {
    param(
        [Parameter(Mandatory = $true)] [object]$SampleCopy,
        [Parameter(Mandatory = $true)] [string]$ResultRoot,
        [Parameter(Mandatory = $true)] [string]$RunId
    )
    if ($null -eq $SampleCopy) { return $true }
    $sampleName = 'startup-probe-sample-{0}.md' -f $RunId
    Assert-PairedOwnedSamplePath $SampleCopy.path $ResultRoot $sampleName
    $item = Get-Item -LiteralPath $SampleCopy.path -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $true }
    if ($item -isnot [IO.FileInfo] -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'Refusing to remove a non-owned or reparse campaign sample.'
    }
    [IO.File]::Delete($SampleCopy.path)
    if (Test-Path -LiteralPath $SampleCopy.path) { throw 'The campaign sample survived cleanup.' }
    return $true
}

function Get-PairedCommitHash {
    $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        $value = (& git -C $repositoryRoot rev-parse --verify HEAD 2>$null | Out-String).Trim()
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldErrorAction }
    if ($exitCode -ne 0 -or $value -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'Could not determine the repository commit identity.'
    }
    return $value.ToLowerInvariant()
}

function Get-PairedRepositoryRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Get-PairedGitText {
    param([Parameter(Mandatory = $true)] [string[]]$Arguments)
    $repositoryRoot = Get-PairedRepositoryRoot
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        $value = (& git -C $repositoryRoot @Arguments 2>$null | Out-String)
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldErrorAction }
    if ($exitCode -ne 0) { throw 'Could not determine repository source state.' }
    return [string]$value
}

function Get-PairedSourceState {
    $statusText = Get-PairedGitText @('status', '--porcelain=v1', '--untracked-files=all')
    $canonicalStatus = ($statusText -replace "`r`n", "`n" -replace "`r", "`n").TrimEnd("`n")
    $statusLines = if ([string]::IsNullOrEmpty($canonicalStatus)) { @() } else { @($canonicalStatus -split "`n") }
    return [pscustomobject][ordered]@{
        head = Get-PairedCommitHash
        dirty = [bool]($statusLines.Count -ne 0)
        statusSha256 = Get-TextSha256 $canonicalStatus
        statusLineCount = [int]$statusLines.Count
    }
}

function Get-PairedManifestField {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string[]]$Names,
        [string[]]$Sections = @('provenance', 'build', 'selectors', 'artifact', 'dependencies', 'dependencyClosure', 'toolchain', 'source', 'runtimeStage')
    )
    $direct = Get-PairedProperty $Manifest $Names
    if ($null -ne $direct) { return $direct }
    return Get-PairedNestedProperty $Manifest $Sections $Names
}

function Require-PairedManifestString {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    if (-not (Test-PairedNonEmptyIdentity $Value)) { throw "Build manifest field '$FieldName' is missing or invalid." }
    return ([string]$Value).Trim()
}

function Require-PairedManifestHash {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    if (-not (Test-PairedSha256 $Value)) { throw "Build manifest field '$FieldName' must be a SHA-256 identity." }
    return ([string]$Value).ToLowerInvariant()
}

function Get-PairedDependencyClosureSha256 {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Entries
    )
    $canonicalEntries = New-Object Collections.Generic.List[string]
    foreach ($entry in @($Entries)) {
        $relativePath = Require-PairedManifestString (Get-PairedProperty $entry @('relativePath', 'path', 'name', 'destination')) 'dependencyClosure.relativePath'
        $role = Require-PairedManifestString (Get-PairedProperty $entry @('role')) 'dependencyClosure.role'
        $hash = [string](Get-PairedProperty $entry @('sha256', 'hash'))
        $hash = $hash -replace '^(?i:sha256:)', ''
        $hash = Require-PairedManifestHash $hash 'dependencyClosure.sha256'
        $sizeValue = Get-PairedProperty $entry @('sizeBytes', 'size')
        if ($null -eq $sizeValue) { throw 'Build manifest dependency closure contains an invalid file size.' }
        try {
            $size = [UInt64]$sizeValue
        }
        catch {
            throw 'Build manifest dependency closure contains an invalid file size.'
        }
        $relativeKey = $relativePath.Replace('/', '\').ToLowerInvariant()
        $roleKey = $role.ToLowerInvariant()
        $sizeText = $size.ToString([Globalization.CultureInfo]::InvariantCulture)
        [void]$canonicalEntries.Add(('{0}|{1}|{2}|{3}' -f $relativeKey, $roleKey, $hash, $sizeText))
    }
    if ($canonicalEntries.Count -eq 0) { throw 'Build manifest dependency closure must contain at least one file.' }
    # List.Sort(IComparer) uses ordinal comparison explicitly; the closure
    # identity must not vary with the host's current UI culture.
    $canonicalEntries.Sort([StringComparer]::Ordinal)
    return Get-TextSha256 ($canonicalEntries.ToArray() -join "`n")
}

function Require-PairedManifestCommit {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    if ($null -eq $Value -or [string]$Value -notmatch '^[0-9a-fA-F]{40}$') {
        throw "Build manifest field '$FieldName' must be a repository commit identity."
    }
    return ([string]$Value).ToLowerInvariant()
}

function Require-PairedManifestBoolean {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    if ($null -eq $Value -or $Value -isnot [bool]) { throw "Build manifest field '$FieldName' must be boolean." }
    return [bool]$Value
}

function Get-PairedBuildManifest {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Backend,
        [Parameter(Mandatory = $true)] [string]$ExpectedPlatform,
        [Parameter(Mandatory = $true)] [string]$ExpectedConfiguration,
        [Parameter(Mandatory = $true)] [object]$ExpectedSource,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact
    )
    $manifestPath = Resolve-PairedInputFile $Path ($Backend + 'BuildManifest')
    try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "The $Backend build manifest is not valid JSON." }
    if ($null -eq $manifest) { throw "The $Backend build manifest is empty." }

    $schema = Get-PairedManifestField $manifest @('schemaVersion', 'schema_version') @()
    if ($null -eq $schema -or [int]$schema -ne $script:PairedManifestSchemaVersion) {
        throw "The $Backend build manifest schema is unsupported."
    }
    $role = (Require-PairedManifestString (Get-PairedManifestField $manifest @('backend', 'role') @('provenance', 'build')) 'backend').ToLowerInvariant()
    if ($role -ne $Backend) { throw "The $Backend build manifest backend does not match its role." }
    $platform = (Require-PairedManifestString (Get-PairedManifestField $manifest @('platform', 'targetPlatform') @('provenance', 'build')) 'platform').ToLowerInvariant()
    if ($platform -ne $ExpectedPlatform.ToLowerInvariant()) { throw "The $Backend build manifest platform does not match the paired run." }
    $configuration = Require-PairedManifestString (Get-PairedManifestField $manifest @('configuration', 'config') @('provenance', 'build')) 'configuration'
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($configuration, $ExpectedConfiguration)) {
        throw "The $Backend build manifest configuration does not match the paired run."
    }

    $sourceHead = (Require-PairedManifestCommit (Get-PairedManifestField $manifest @('sourceHead', 'sourceCommit', 'head') @('source', 'provenance', 'build')) 'sourceHead')
    $sourceDirty = Require-PairedManifestBoolean (Get-PairedManifestField $manifest @('sourceDirty', 'dirty') @('source', 'provenance', 'build')) 'sourceDirty'
    $sourceStatusHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('sourceStatusSha256', 'statusSha256', 'dirtyStatusSha256') @('source', 'provenance', 'build')) 'sourceStatusSha256'
    if ($sourceHead -ne $ExpectedSource.head.ToLowerInvariant() -or
        $sourceDirty -ne [bool]$ExpectedSource.dirty -or
        $sourceStatusHash -ne $ExpectedSource.statusSha256.ToLowerInvariant()) {
        throw "The $Backend build manifest source state does not match the current checkout."
    }

    $outputBackend = (Require-PairedManifestString (Get-PairedManifestField $manifest @('outputBackend', 'outputProvider', 'output') @('selectors', 'provenance', 'build')) 'outputBackend').ToLowerInvariant()
    $utf16Backend = (Require-PairedManifestString (Get-PairedManifestField $manifest @('utf16Backend', 'utf16Provider', 'utf16') @('selectors', 'provenance', 'build')) 'utf16Backend').ToLowerInvariant()
    if ($outputBackend -ne $Backend -or $utf16Backend -ne 'cpp') { throw "The $Backend build manifest selectors are not exact for this comparison." }
    $outputProduction = Require-PairedManifestBoolean (Get-PairedManifestField $manifest @('outputProductionPackage', 'outputProduction', 'productionOutput') @('selectors', 'provenance', 'build')) 'outputProductionPackage'
    $utf16Production = Require-PairedManifestBoolean (Get-PairedManifestField $manifest @('utf16ProductionPackage', 'utf16Production', 'productionUtf16') @('selectors', 'provenance', 'build')) 'utf16ProductionPackage'
    if ($outputProduction -or $utf16Production) { throw "The $Backend build manifest enables a production package gate." }

    $manifestArtifactHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('exeSha256', 'artifactSha256', 'sha256') @('artifact', 'provenance', 'build')) 'exeSha256'
    if ($manifestArtifactHash -ne $ExpectedArtifact.sha256.ToLowerInvariant()) { throw "The $Backend build manifest executable hash does not match the supplied artifact." }
    $dependencyHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('dependencyClosureSha256', 'closureSha256', 'dependencyHash') @('dependencies', 'dependencyClosure', 'runtimeStage', 'provenance', 'build')) 'dependencyClosureSha256'
    $runtimeReceiptHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('runtimeStageReceiptSha256', 'receiptSha256') @('runtimeStage', 'dependencies', 'provenance', 'build')) 'runtimeStageReceiptSha256'

    $windowsImage = Require-PairedManifestString (Get-PairedManifestField $manifest @('windowsImageIdentity', 'windowsImage', 'windowsVersion', 'osVersion') @('host', 'platform', 'toolchain', 'provenance', 'build')) 'windowsImageIdentity'
    if ($windowsImage -match '(?i)^(unknown|unspecified|n/?a)$') { throw "The $Backend build manifest Windows image identity is unknown." }
    $windowsImageHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('windowsImageSha256', 'windowsVersionSha256', 'osSha256') @('host', 'platform', 'toolchain', 'provenance', 'build')) 'windowsImageSha256'
    $powerMode = Require-PairedManifestString (Get-PairedManifestField $manifest @('powerMode', 'powerPlan', 'powerProfile') @('host', 'power', 'toolchain', 'provenance', 'build')) 'powerMode'
    if ($powerMode -match '(?i)^(unknown|unspecified|n/?a)$') { throw "The $Backend build manifest power mode is unknown." }
    $parallelismValue = Get-PairedManifestField $manifest @('buildParallelism', 'parallelism', 'msbuildParallelism') @('build', 'toolchain', 'provenance')
    if ($null -eq $parallelismValue -or [string]$parallelismValue -notmatch '^[0-9]+$') {
        throw "Build manifest field 'buildParallelism' is missing or invalid."
    }
    try { $buildParallelism = [Int64]$parallelismValue } catch { throw "Build manifest field 'buildParallelism' is missing or invalid." }
    if ($buildParallelism -lt 1 -or $buildParallelism -gt [Int32]::MaxValue) {
        throw "Build manifest field 'buildParallelism' is missing or invalid."
    }
    $buildParallelism = [int]$buildParallelism

    $msvcIdentity = Require-PairedManifestString (Get-PairedManifestField $manifest @('msvcVersion', 'msvcToolchain', 'msvcIdentity') @('toolchain', 'provenance', 'build')) 'msvcIdentity'
    $rustToolchain = Require-PairedManifestString (Get-PairedManifestField $manifest @('rustToolchain', 'rustVersion', 'rustIdentity') @('toolchain', 'provenance', 'build')) 'rustToolchain'
    $rustLockHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('rustLockSha256', 'lockSha256', 'cargoLockSha256') @('toolchain', 'provenance', 'build')) 'rustLockSha256'
    $packagePlanHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('packagePlanSha256', 'packagePlanHash') @('toolchain', 'provenance', 'build')) 'packagePlanSha256'
    $buildCommandHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('buildCommandSha256', 'commandSha256', 'buildCommandHash') @('build', 'provenance', 'toolchain')) 'buildCommandSha256'

    return [pscustomobject][ordered]@{
        manifestSha256 = Get-Sha256 $manifestPath
        schemaVersion = [int]$schema
        backend = $role
        platform = $platform
        configuration = $configuration
        sourceHead = $sourceHead
        sourceDirty = [bool]$sourceDirty
        sourceStatusSha256 = $sourceStatusHash
        sourceStatusLineCount = [int]$ExpectedSource.statusLineCount
        outputBackend = $outputBackend
        utf16Backend = $utf16Backend
        outputProductionPackage = [bool]$outputProduction
        utf16ProductionPackage = [bool]$utf16Production
        exeSha256 = $manifestArtifactHash
        dependencyClosureSha256 = $dependencyHash
        runtimeStageReceiptSha256 = $runtimeReceiptHash
        windowsImageIdentity = $windowsImage
        windowsImageSha256 = $windowsImageHash
        powerMode = $powerMode
        buildParallelism = $buildParallelism
        msvcIdentity = $msvcIdentity
        rustToolchain = $rustToolchain
        rustLockSha256 = $rustLockHash
        packagePlanSha256 = $packagePlanHash
        buildCommandSha256 = $buildCommandHash
    }
}

function Get-PairedRuntimeStageIdentity {
    param(
        [Parameter(Mandatory = $true)] [string]$StageDirectory,
        [Parameter(Mandatory = $true)] [string]$ExpectedConfiguration,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact
    )
    $stageItem = Get-Item -LiteralPath $StageDirectory -Force -ErrorAction Stop
    if ($stageItem -isnot [IO.DirectoryInfo] -or
        (($stageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The runtime stage must be a regular non-reparse directory.'
    }
    $stageRoot = [IO.Path]::GetFullPath($StageDirectory).TrimEnd('\')
    $receiptPath = Join-Path $stageRoot '.sakura-runtime-stage.json'
    $receiptItem = Get-Item -LiteralPath $receiptPath -Force -ErrorAction Stop
    if ($receiptItem -isnot [IO.FileInfo] -or
        (($receiptItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The runtime stage receipt must be a regular non-reparse file.'
    }
    try { $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -ErrorAction Stop }
    catch { throw 'The runtime stage receipt is not valid JSON.' }
    $schema = Get-PairedProperty $receipt @('schema_version', 'schemaVersion')
    if ($null -eq $schema -or [int]$schema -ne 1) { throw 'The runtime stage receipt schema is unsupported.' }
    $expectedContext = 'msvc-x64-' + $ExpectedConfiguration.ToLowerInvariant()
    $context = [string](Get-PairedProperty $receipt @('context_id', 'contextId'))
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($context, $expectedContext)) {
        throw 'The runtime stage receipt context does not match the paired run.'
    }
    $files = @(Get-PairedProperty $receipt @('files'))
    if ($files.Count -eq 0) { throw 'The runtime stage receipt declares no files.' }
    $editorSeen = $false
    $totalSize = [UInt64]0
    $seenNames = @{}
    $closureEntries = New-Object Collections.Generic.List[object]
    foreach ($entry in $files) {
        $destination = Require-PairedManifestString (Get-PairedProperty $entry @('destination')) 'runtimeStage.destination'
        $role = Require-PairedManifestString (Get-PairedProperty $entry @('role')) 'runtimeStage.role'
        $declaredHashValue = [string](Get-PairedProperty $entry @('sha256'))
        $declaredHashValue = $declaredHashValue -replace '^(?i:sha256:)', ''
        $declaredHash = Require-PairedManifestHash $declaredHashValue 'runtimeStage.sha256'
        $declaredSize = Get-PairedProperty $entry @('size', 'sizeBytes')
        if ($null -eq $declaredSize -or [UInt64]$declaredSize -lt 1) { throw 'The runtime stage receipt declares an invalid file size.' }
        $name = [IO.Path]::GetFileName($destination)
        if ([string]::IsNullOrWhiteSpace($name) -or $name -eq '.' -or $name -eq '..' -or $seenNames.ContainsKey($name.ToLowerInvariant())) {
            throw 'The runtime stage receipt contains an ambiguous file name.'
        }
        $seenNames[$name.ToLowerInvariant()] = $true
        $candidate = Join-Path $stageRoot $name
        $identity = Get-PairedArtifactIdentity $candidate
        if ($identity.sha256 -ne $declaredHash -or $identity.sizeBytes -ne [UInt64]$declaredSize) {
            throw 'The runtime stage file does not match its receipt identity.'
        }
        $totalSize += [UInt64]$declaredSize
        [void]$closureEntries.Add([pscustomobject][ordered]@{
            relativePath = $name
            role = $role
            sha256 = $declaredHash
            sizeBytes = [UInt64]$declaredSize
        })
        if ([StringComparer]::OrdinalIgnoreCase.Equals($role, 'editor') -or
            [StringComparer]::OrdinalIgnoreCase.Equals($name, 'sakura.exe')) {
            if (-not [StringComparer]::OrdinalIgnoreCase.Equals($name, 'sakura.exe')) {
                throw 'The runtime stage editor entry must be sakura.exe.'
            }
            $editorSeen = $true
            if ($identity.sha256 -ne $ExpectedArtifact.sha256.ToLowerInvariant()) {
                throw 'The runtime stage sakura.exe does not match the supplied artifact.'
            }
        }
    }
    if (-not $editorSeen) { throw 'The runtime stage receipt has no sakura.exe editor entry.' }
    return [pscustomobject][ordered]@{
        receiptSha256 = Get-Sha256 $receiptPath
        dependencyClosureSha256 = Get-PairedDependencyClosureSha256 $closureEntries.ToArray()
        fileCount = [int]$files.Count
        totalSizeBytes = [UInt64]$totalSize
        configuration = $ExpectedConfiguration
    }
}

function Get-PairedRuntimeStageReceiptPath {
    param([Parameter(Mandatory = $true)] [string]$StageDirectory)
    $stageRoot = [IO.Path]::GetFullPath($StageDirectory).TrimEnd('\')
    return Join-Path $stageRoot '.sakura-runtime-stage.json'
}

function New-PairedArtifactBundle {
    param(
        [Parameter(Mandatory = $true)] [string]$SourcePath,
        [Parameter(Mandatory = $true)] [string]$BundleRoot,
        [Parameter(Mandatory = $true)] [string]$BundleName,
        [string]$RuntimeStageDirectory
    )
    if ([string]::IsNullOrWhiteSpace($RuntimeStageDirectory)) {
        return New-StartupArtifactBundle $SourcePath $BundleRoot $BundleName
    }
    # The shared implementation accepts a runtime-stage directory as its
    # SourcePath and the canonical receipt as its fourth argument.  Keep the
    # explicitly supplied executable as an independent identity check: a valid
    # stage must contain the exact artifact selected for this role.
    $sourceIdentity = Get-StartupFileIdentity $SourcePath
    $receiptPath = Get-PairedRuntimeStageReceiptPath $RuntimeStageDirectory
    $bundle = $null
    try {
        $bundle = New-StartupArtifactBundle -SourcePath $RuntimeStageDirectory -BundleRoot $BundleRoot -BundleName $BundleName -ClosureReceiptPath $receiptPath
        if ($bundle.source.sha256 -ne $sourceIdentity.sha256 -or
            [UInt64]$bundle.source.sizeBytes -ne [UInt64]$sourceIdentity.sizeBytes) {
            throw 'The runtime stage editor artifact does not match the supplied executable.'
        }
        return $bundle
    }
    catch {
        if ($null -ne $bundle -and (Test-Path -LiteralPath $bundle.bundlePath)) {
            try { Remove-StartupArtifactBundle $bundle } catch { }
        }
        throw
    }
}

function Get-PairedHostIdentity {
    $architecture = 'unknown'
    try { $architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() }
    catch { }
    $osVersion = [Environment]::OSVersion.Version.ToString()
    $logicalProcessors = [int][Environment]::ProcessorCount
    $cpuManufacturer = 'unknown'
    $cpuModel = 'unknown'
    $physicalCores = 0
    try {
        $processor = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop) | Select-Object -First 1
        if ($null -ne $processor) {
            if (-not [string]::IsNullOrWhiteSpace([string]$processor.Manufacturer)) { $cpuManufacturer = [string]$processor.Manufacturer }
            if (-not [string]::IsNullOrWhiteSpace([string]$processor.Name)) { $cpuModel = [string]$processor.Name }
            if ($null -ne $processor.NumberOfCores) { $physicalCores = [int]$processor.NumberOfCores }
            if ($null -ne $processor.NumberOfLogicalProcessors) { $logicalProcessors = [int]$processor.NumberOfLogicalProcessors }
        }
    }
    catch { }
    $canonical = @(
        'platform=Windows', "osVersion=$osVersion", "architecture=$architecture",
        "cpuManufacturer=$cpuManufacturer", "cpuModel=$cpuModel",
        "physicalCores=$physicalCores", "logicalProcessors=$logicalProcessors"
    ) -join '|'
    return [pscustomobject][ordered]@{
        sha256 = Get-TextSha256 $canonical
        osVersion = [string]$osVersion
        cpuManufacturer = [string]$cpuManufacturer
        cpuModel = [string]$cpuModel
        physicalCores = [int]$physicalCores
        logicalProcessors = [int]$logicalProcessors
        architecture = [string]$architecture
    }
}

function Get-PairedProfileDigest {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return [pscustomobject][ordered]@{
            sha256 = Get-TextSha256 'paired-startup-profile-missing-v1'
            state = 'missing'
            fileCount = 0
        }
    }
    $allEntries = @(Get-SafeDirectoryTreeEntries $Path)
    $root = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $records = New-Object Collections.Generic.List[string]
    foreach ($entry in @($allEntries | Where-Object { -not $_.IsDirectory } | Sort-Object FullName)) {
        $relative = $entry.FullName.Substring($root.Length).TrimStart('\').Replace('\', '/')
        [void]$records.Add(('{0}|{1}|{2}' -f $relative, (Get-Sha256 $entry.FullName), [UInt64]$entry.Length))
    }
    $canonical = if ($records.Count -eq 0) { 'paired-startup-profile-empty-v1' } else { $records.ToArray() -join "`n" }
    return [pscustomobject][ordered]@{
        sha256 = Get-TextSha256 $canonical
        state = 'present'
        fileCount = [int]$records.Count
    }
}

function Get-PairedProfilePolicy {
    $canonical = 'paired-gui-startup-profile-v2|fresh-per-launch|exact-sakura.exe.ini-multiuser0|campaign-artifact-bundle|delete-after-launch'
    return [pscustomobject][ordered]@{
        kind = 'fresh-per-launch'
        sha256 = Get-TextSha256 $canonical
        artifactIsolation = 'campaign-artifact-bundle'
        deletion = 'verified-after-each-launch'
        sidecarContract = $script:StartupProfileSidecarContract
    }
}

function Convert-PairedArtifactBundleVerification {
    param(
        [Parameter(Mandatory = $true)] [string]$Backend,
        [Parameter(Mandatory = $true)] [object]$Verification,
        [Parameter(Mandatory = $true)] [bool]$CleanupVerified,
        [object]$Bundle
    )
    $closure = Get-PairedProperty $Bundle @('closure')
    $closureFiles = @()
    if ($null -ne $closure) { $closureFiles = @(Get-PairedProperty $closure @('files')) }
    if ($closureFiles.Count -eq 1 -and $null -eq $closureFiles[0]) { $closureFiles = @() }
    $closureHash = $null
    if ($closureFiles.Count -gt 0) {
        $closureHash = Get-PairedDependencyClosureSha256 $closureFiles
    }
    if ($null -eq $closureHash) {
        $closureHashValue = Get-PairedProperty $Verification @('dependencyClosureSha256', 'closureSha256')
        if ($null -ne $closureHashValue) { $closureHash = [string]$closureHashValue }
    }
    $runtimeReceipt = Get-PairedProperty $closure @('receiptSha256')
    if ($null -eq $runtimeReceipt) {
        $runtimeReceipt = Get-PairedProperty $Verification @('runtimeStageReceiptSha256', 'stageReceiptSha256')
    }
    $runtimeFileCount = if ($closureFiles.Count -eq 0) { Get-PairedProperty $Verification @('runtimeStageFileCount', 'stageFileCount') } else { $closureFiles.Count }
    return [ordered]@{
        backend = $Backend
        sourceHashBefore = [string]$Verification.sourceHashBefore
        sourceHashAfter = [string]$Verification.sourceHashAfter
        sourceSizeBefore = [UInt64]$Verification.sourceSizeBefore
        sourceSizeAfter = [UInt64]$Verification.sourceSizeAfter
        sourceUnchanged = [bool]$Verification.sourceUnchanged
        copiedHashBefore = [string]$Verification.copiedHashBefore
        copiedHashAfter = [string]$Verification.copiedHashAfter
        copiedSizeBefore = [UInt64]$Verification.copiedSizeBefore
        copiedSizeAfter = [UInt64]$Verification.copiedSizeAfter
        copiedUnchanged = [bool]$Verification.copiedUnchanged
        sourceClosureUnchanged = [bool]$Verification.sourceClosureUnchanged
        copiedClosureUnchanged = [bool]$Verification.copiedClosureUnchanged
        receiptUnchanged = [bool]$Verification.receiptUnchanged
        closureMode = if ($null -eq (Get-PairedProperty $Verification @('closureMode'))) { $null } else { [string](Get-PairedProperty $Verification @('closureMode')) }
        dependencyClosureSha256 = $closureHash
        runtimeStageReceiptSha256 = if ($null -eq $runtimeReceipt) { $null } else { [string]$runtimeReceipt }
        runtimeStageFileCount = if ($null -eq $runtimeFileCount) { $null } else { [int]$runtimeFileCount }
        sidecarContract = $script:StartupProfileSidecarContract
        sidecarSha256 = [string]$Verification.sidecarSha256
        sidecarSizeBytes = [UInt64]$Verification.sidecarSizeBytes
        sidecarMultiUser = 0
        sidecarVerified = [bool]$Verification.sidecarVerified
        cleanupVerified = $CleanupVerified
    }
}

function Assert-PairedHostIdentityQualified {
    param([Parameter(Mandatory = $true)] [object]$HostIdentity)
    if ([string]$HostIdentity.architecture -eq 'unknown' -or
        [string]::IsNullOrWhiteSpace([string]$HostIdentity.cpuManufacturer) -or
        [string]$HostIdentity.cpuManufacturer -eq 'unknown' -or
        [string]::IsNullOrWhiteSpace([string]$HostIdentity.cpuModel) -or
        [string]$HostIdentity.cpuModel -eq 'unknown' -or
        [int]$HostIdentity.logicalProcessors -lt 1 -or
        [int]$HostIdentity.physicalCores -lt 1) {
        throw 'Qualified paired evidence requires complete host and CPU identity.'
    }
}

function New-PairedUnverifiedProvenance {
    param(
        [Parameter(Mandatory = $true)] [string]$BackendA,
        [Parameter(Mandatory = $true)] [string]$BackendB,
        [Parameter(Mandatory = $true)] [string]$Platform,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [object]$Source
    )
    return [ordered]@{
        status = 'unverified'
        roleLabels = 'collect-only-inputs'
        buildManifestVerified = $false
        platform = $Platform
        configuration = $Configuration
        sourceHead = [string]$Source.head
        sourceDirty = [bool]$Source.dirty
        sourceStatusSha256 = [string]$Source.statusSha256
        sourceStatusLineCount = [int]$Source.statusLineCount
        outputBackend = 'unverified'
        utf16Backend = 'unverified'
        outputProductionPackage = $null
        utf16ProductionPackage = $null
        manifests = [ordered]@{
            cppSha256 = $null
            rustSha256 = $null
        }
        dependencyClosure = [ordered]@{
            cppSha256 = $null
            rustSha256 = $null
        }
        environment = [ordered]@{
            windowsImageIdentity = $null
            windowsImageSha256 = $null
            powerMode = $null
            buildParallelism = $null
        }
        toolchain = [ordered]@{
            msvc = 'unverified'
            rust = 'unverified'
            rustLockSha256 = $null
            packagePlanSha256 = $null
            buildCommandSha256 = $null
        }
    }
}

function New-PairedVerifiedProvenance {
    param(
        [Parameter(Mandatory = $true)] [object]$CppManifest,
        [Parameter(Mandatory = $true)] [object]$RustManifest,
        [Parameter(Mandatory = $true)] [object]$CppStage,
        [Parameter(Mandatory = $true)] [object]$RustStage,
        [Parameter(Mandatory = $true)] [string]$Platform,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [object]$Source
    )
    return [ordered]@{
        status = 'verified'
        roleLabels = 'manifest-declared'
        buildManifestVerified = $true
        platform = $Platform
        configuration = $Configuration
        sourceHead = [string]$Source.head
        sourceDirty = [bool]$Source.dirty
        sourceStatusSha256 = [string]$Source.statusSha256
        sourceStatusLineCount = [int]$Source.statusLineCount
        outputBackend = 'paired(cpp,rust)'
        utf16Backend = 'cpp'
        outputProductionPackage = $false
        utf16ProductionPackage = $false
        manifests = [ordered]@{
            cppSha256 = [string]$CppManifest.manifestSha256
            rustSha256 = [string]$RustManifest.manifestSha256
        }
        dependencyClosure = [ordered]@{
            cppSha256 = [string]$CppManifest.dependencyClosureSha256
            rustSha256 = [string]$RustManifest.dependencyClosureSha256
            cppRuntimeStageReceiptSha256 = [string]$CppStage.receiptSha256
            rustRuntimeStageReceiptSha256 = [string]$RustStage.receiptSha256
        }
        environment = [ordered]@{
            windowsImageIdentity = [string]$CppManifest.windowsImageIdentity
            windowsImageSha256 = [string]$CppManifest.windowsImageSha256
            powerMode = [string]$CppManifest.powerMode
            buildParallelism = [int]$CppManifest.buildParallelism
        }
        toolchain = [ordered]@{
            msvc = [string]$CppManifest.msvcIdentity
            rust = [string]$RustManifest.rustToolchain
            rustLockSha256 = [string]$RustManifest.rustLockSha256
            packagePlanSha256 = [string]$CppManifest.packagePlanSha256
            buildCommandSha256 = [string]$CppManifest.buildCommandSha256
        }
        backendBuilds = [ordered]@{
            cpp = [ordered]@{
                backend = [string]$CppManifest.backend
                platform = [string]$CppManifest.platform
                configuration = [string]$CppManifest.configuration
                outputBackend = [string]$CppManifest.outputBackend
                utf16Backend = [string]$CppManifest.utf16Backend
                exeSha256 = [string]$CppManifest.exeSha256
                dependencyClosureSha256 = [string]$CppManifest.dependencyClosureSha256
            }
            rust = [ordered]@{
                backend = [string]$RustManifest.backend
                platform = [string]$RustManifest.platform
                configuration = [string]$RustManifest.configuration
                outputBackend = [string]$RustManifest.outputBackend
                utf16Backend = [string]$RustManifest.utf16Backend
                exeSha256 = [string]$RustManifest.exeSha256
                dependencyClosureSha256 = [string]$RustManifest.dependencyClosureSha256
            }
        }
    }
}

function Get-PairedSchedule {
    param(
        [Parameter(Mandatory = $true)] [int]$Warmups,
        [Parameter(Mandatory = $true)] [int]$Measured,
        [Parameter(Mandatory = $true)] [string]$First
    )
    $First = Get-PairedCanonicalBackend $First
    $rows = New-Object Collections.Generic.List[object]
    $totalSlots = $Warmups + $Measured
    $sequence = 0
    for ($slot = 0; $slot -lt $totalSlots; ++$slot) {
        $phase = if ($slot -lt $Warmups) { 'warmup' } else { 'measured' }
        $phaseIndex = if ($phase -eq 'warmup') { $slot + 1 } else { $slot - $Warmups + 1 }
        $firstBackend = if ((($slot % 2) -eq 0) -eq ($First -eq 'cpp')) { 'cpp' } else { 'rust' }
        $order = if ($firstBackend -eq 'cpp') { @('cpp', 'rust') } else { @('rust', 'cpp') }
        $pairIndex = [int]($slot + 1)
        foreach ($backend in $order) {
            ++$sequence
            [void]$rows.Add([pscustomobject][ordered]@{
                sequence = [int]$sequence
                pairIndex = $pairIndex
                slot = [int]($slot + 1)
                phase = $phase
                phaseIndex = [int]$phaseIndex
                backend = $backend
            })
        }
    }
    return $rows.ToArray()
}

function Get-PairedScheduleHash {
    param([Parameter(Mandatory = $true)] [object[]]$Schedule)
    $canonical = @($Schedule | ForEach-Object {
        '{0}|{1}|{2}|{3}|{4}|{5}' -f $_.sequence, $_.pairIndex, $_.slot, $_.phase, $_.phaseIndex, $_.backend
    }) -join "`n"
    return Get-TextSha256 $canonical
}

function Get-PairedFailureType {
    param(
        [Parameter(Mandatory = $true)] [object]$Raw,
        [Parameter(Mandatory = $true)] [bool]$ProfileCleanupVerified
    )
    if (-not $Raw.processCleanupVerified) { return 'survivor' }
    if (-not $ProfileCleanupVerified) { return 'profileCleanup' }
    if ($null -eq $Raw.affinity -or -not $Raw.affinity.verified) { return 'affinity' }
    if ([string]$Raw.error -match '(?i)timed out|timeout') { return 'timeout' }
    return 'startup'
}

function Convert-PairedAffinity {
    param([object]$Affinity)
    if ($null -eq $Affinity) {
        return [ordered]@{
            requestedMask = [UInt64]0; processMask = $null; systemMask = $null
            opened = $false; setSucceeded = $false; readBackSucceeded = $false; verified = $false; descendantsVerified = $false; errorCode = $null
        }
    }
    return [ordered]@{
        requestedMask = [UInt64]$Affinity.requestedMask
        processMask = if ($null -eq $Affinity.processMask) { $null } else { [UInt64]$Affinity.processMask }
        systemMask = if ($null -eq $Affinity.systemMask) { $null } else { [UInt64]$Affinity.systemMask }
        opened = [bool]$Affinity.opened
        setSucceeded = [bool]$Affinity.setSucceeded
        readBackSucceeded = [bool]$Affinity.readBackSucceeded
        verified = [bool]$Affinity.verified
        descendantsVerified = [bool]$Affinity.descendantsVerified
        errorCode = if ($null -eq $Affinity.errorCode) { $null } else { [int]$Affinity.errorCode }
    }
}

function Convert-PairedLaunchResult {
    param(
        [Parameter(Mandatory = $true)] [object]$Raw,
        [Parameter(Mandatory = $true)] [object]$ScheduleRow,
        [Parameter(Mandatory = $true)] [object]$ProfileDigest,
        [Parameter(Mandatory = $true)] [bool]$ProfileCleanupVerified
    )
    $affinity = Convert-PairedAffinity $Raw.affinity
    $success = [bool]$Raw.success -and [bool]$Raw.processCleanupVerified -and
        $ProfileCleanupVerified -and [bool]$affinity.verified
    $failureType = if ($success) { $null } else { Get-PairedFailureType $Raw $ProfileCleanupVerified }
    $metrics = $null
    if ($success) {
        $metrics = [ordered]@{}
        foreach ($metric in $script:PairedMetricNames) {
            $value = $Raw.$metric
            if ($null -eq $value) { $success = $false; $failureType = 'startup'; break }
            $metrics[$metric] = [double]$value
        }
    }
    return [pscustomobject][ordered]@{
        sequence = [int]$ScheduleRow.sequence
        pairIndex = [int]$ScheduleRow.pairIndex
        slot = [int]$ScheduleRow.slot
        phase = [string]$ScheduleRow.phase
        phaseIndex = [int]$ScheduleRow.phaseIndex
        backend = [string]$ScheduleRow.backend
        status = if ($success) { 'succeeded' } else { [string]$failureType }
        excluded = -not $success
        metrics = $metrics
        affinity = $affinity
        profileSha256 = [string]$ProfileDigest.sha256
        profileState = [string]$ProfileDigest.state
        profileFileCount = [int]$ProfileDigest.fileCount
        processCleanupVerified = [bool]$Raw.processCleanupVerified
        profileCleanupVerified = [bool]$ProfileCleanupVerified
        cleanupVerified = [bool]$Raw.processCleanupVerified -and $ProfileCleanupVerified
        survivorCount = @($Raw.survivors).Count
    }
}

function Test-PairedRunCleanupVerified {
    param([Parameter(Mandatory = $true)] [object]$Run)
    return [bool]$Run.processCleanupVerified -and [bool]$Run.profileCleanupVerified
}

function New-PairedCampaignTermination {
    param(
        [Parameter(Mandatory = $true)] [int]$TotalEntries,
        [Parameter(Mandatory = $true)] [int]$CompletedEntries,
        [object]$TriggerRow,
        [object]$TriggerRun
    )
    if ($TotalEntries -lt 0 -or $CompletedEntries -lt 0 -or $CompletedEntries -gt $TotalEntries) {
        throw 'Campaign termination counts are invalid.'
    }
    $terminated = $null -ne $TriggerRow
    $cleanupUnverified = $terminated -and ($null -eq $TriggerRun -or -not (Test-PairedRunCleanupVerified $TriggerRun))
    $failureType = if (-not $terminated) { $null } elseif ($cleanupUnverified) { 'cleanup-unverified' } elseif ($null -ne $TriggerRun) { [string]$TriggerRun.status } else { 'launch-failure' }
    return [ordered]@{
        status = if ($terminated) { 'terminated' } else { 'completed' }
        type = if (-not $terminated) { 'none' } elseif ($cleanupUnverified) { 'cleanup-unverified' } else { 'launch-failure' }
        failureType = $failureType
        excluded = [bool]$terminated
        triggerSequence = if ($terminated) { [int]$TriggerRow.sequence } else { $null }
        triggerBackend = if ($terminated) { [string]$TriggerRow.backend } else { $null }
        completedLaunches = [int]$CompletedEntries
        suppressedLaunches = [int]($TotalEntries - $CompletedEntries)
        laterLaunchesSuppressed = [bool]$terminated
    }
}

function Get-PairedStatistics {
    param([object[]]$Values)
    if ($null -eq $Values -or @($Values).Count -eq 0) { return $null }
    $statistics = Get-Statistics ([double[]]$Values)
    return [ordered]@{
        count = [int]$statistics.count
        medianMs = [double]$statistics.medianMs
        p95Ms = [double]$statistics.p95Ms
        minMs = [double]$statistics.minMs
        maxMs = [double]$statistics.maxMs
        meanMs = [double]$statistics.meanMs
    }
}

function Get-PairedRegressionPercent {
    param([object]$Baseline, [object]$Candidate)
    if ($null -eq $Baseline -or $null -eq $Candidate -or [double]$Baseline -le 0.0) { return $null }
    return (([double]$Candidate - [double]$Baseline) * 100.0) / [double]$Baseline
}

function New-PairedSyntheticRuns {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Schedule,
        [Parameter(Mandatory = $true)] [double]$RustDefault,
        [Parameter(Mandatory = $true)] [double]$RustTail,
        [Parameter(Mandatory = $true)] [bool]$UseTail
    )
    $runs = New-Object Collections.Generic.List[object]
    foreach ($row in $Schedule) {
        $value = 100.0
        if ($row.backend -eq 'rust') {
            $value = $RustDefault
            if ($UseTail -and $row.pairIndex -gt 18) { $value = $RustTail }
        }
        [void]$runs.Add([pscustomobject]@{
                backend = $row.backend; phase = $row.phase; pairIndex = $row.pairIndex
                excluded = $false; metrics = [ordered]@{ documentReadyMs = $value }
            })
    }
    return $runs.ToArray()
}

function New-PairedPerformanceSummary {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Runs,
        [Parameter(Mandatory = $true)] [object[]]$Schedule,
        [Parameter(Mandatory = $true)] [int]$RequiredMeasuredLaunches,
        [Parameter(Mandatory = $true)] [bool]$CollectionQualified
    )
    $primaryMetric = $script:PairedPrimaryMetric
    $cppRuns = @($Runs | Where-Object {
            $_.backend -eq 'cpp' -and $_.phase -eq 'measured' -and -not $_.excluded -and
            $null -ne $_.metrics -and $null -ne $_.metrics[$primaryMetric]
        })
    $rustRuns = @($Runs | Where-Object {
            $_.backend -eq 'rust' -and $_.phase -eq 'measured' -and -not $_.excluded -and
            $null -ne $_.metrics -and $null -ne $_.metrics[$primaryMetric]
        })
    $cppValues = @($cppRuns | ForEach-Object { [double]$_.metrics[$primaryMetric] })
    $rustValues = @($rustRuns | ForEach-Object { [double]$_.metrics[$primaryMetric] })
    $cppStatistics = Get-PairedStatistics $cppValues
    $rustStatistics = Get-PairedStatistics $rustValues

    $pairedDeltas = New-Object Collections.Generic.List[double]
    foreach ($cppSlot in @($Schedule | Where-Object { $_.backend -eq 'cpp' -and $_.phase -eq 'measured' })) {
        $pairRuns = @($Runs | Where-Object { $_.pairIndex -eq $cppSlot.pairIndex -and $_.phase -eq 'measured' })
        $cppPair = @($pairRuns | Where-Object { $_.backend -eq 'cpp' -and -not $_.excluded -and $null -ne $_.metrics -and $null -ne $_.metrics[$primaryMetric] })
        $rustPair = @($pairRuns | Where-Object { $_.backend -eq 'rust' -and -not $_.excluded -and $null -ne $_.metrics -and $null -ne $_.metrics[$primaryMetric] })
        if ($cppPair.Count -ne 1 -or $rustPair.Count -ne 1) { continue }
        [void]$pairedDeltas.Add(([double]$rustPair[0].metrics[$primaryMetric] - [double]$cppPair[0].metrics[$primaryMetric]))
    }
    $pairedDeltaStatistics = Get-PairedStatistics $pairedDeltas.ToArray()

    $medianDeltaRaw = if ($null -eq $cppStatistics -or $null -eq $rustStatistics) { $null } else { [double]$rustStatistics.medianMs - [double]$cppStatistics.medianMs }
    $p95DeltaRaw = if ($null -eq $cppStatistics -or $null -eq $rustStatistics) { $null } else { [double]$rustStatistics.p95Ms - [double]$cppStatistics.p95Ms }
    $medianRegressionRaw = if ($null -eq $cppStatistics -or $null -eq $rustStatistics) { $null } else { Get-PairedRegressionPercent $cppStatistics.medianMs $rustStatistics.medianMs }
    $p95RegressionRaw = if ($null -eq $cppStatistics -or $null -eq $rustStatistics) { $null } else { Get-PairedRegressionPercent $cppStatistics.p95Ms $rustStatistics.p95Ms }
    $evaluable = $cppRuns.Count -ge $RequiredMeasuredLaunches -and $rustRuns.Count -ge $RequiredMeasuredLaunches -and $pairedDeltas.Count -ge $RequiredMeasuredLaunches
    $medianGate = $null -ne $medianDeltaRaw -and $null -ne $medianRegressionRaw -and
        $medianDeltaRaw -le $script:PairedMedianAbsoluteLimitMs -and
        $medianRegressionRaw -le $script:PairedMedianRelativeLimitPercent
    $p95Gate = $null -ne $p95RegressionRaw -and $p95RegressionRaw -le $script:PairedP95RelativeLimitPercent
    $performancePass = $evaluable -and $CollectionQualified -and $medianGate -and $p95Gate
    return [ordered]@{
        primaryMetric = $primaryMetric
        collectionQualified = [bool]$CollectionQualified
        evaluable = [bool]$evaluable
        cppMeasured = $cppStatistics
        rustMeasured = $rustStatistics
        pairedDeltaMs = $pairedDeltaStatistics
        regression = [ordered]@{
            medianDeltaMs = if ($null -eq $medianDeltaRaw) { $null } else { [Math]::Round($medianDeltaRaw, 3) }
            medianRegressionPercent = if ($null -eq $medianRegressionRaw) { $null } else { [Math]::Round($medianRegressionRaw, 3) }
            p95DeltaMs = if ($null -eq $p95DeltaRaw) { $null } else { [Math]::Round($p95DeltaRaw, 3) }
            p95RegressionPercent = if ($null -eq $p95RegressionRaw) { $null } else { [Math]::Round($p95RegressionRaw, 3) }
        }
        thresholds = [ordered]@{
            medianRelativeRegressionPercent = [double]$script:PairedMedianRelativeLimitPercent
            medianAbsoluteRegressionMs = [double]$script:PairedMedianAbsoluteLimitMs
            p95RelativeRegressionPercent = [double]$script:PairedP95RelativeLimitPercent
        }
        gates = [ordered]@{
            median = [bool]$medianGate
            p95 = [bool]$p95Gate
        }
        pass = [bool]$performancePass
    }
}

function New-PairedPhaseSummary {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Runs,
        [Parameter(Mandatory = $true)] [string]$Backend,
        [Parameter(Mandatory = $true)] [string]$Phase
    )
    $selected = @($Runs | Where-Object { $_.backend -eq $Backend -and $_.phase -eq $Phase })
    $metricStatistics = [ordered]@{}
    foreach ($metric in $script:PairedMetricNames) {
        $values = @($selected | Where-Object { -not $_.excluded -and $null -ne $_.metrics } | ForEach-Object { $_.metrics[$metric] })
        $metricStatistics[$metric] = Get-PairedStatistics $values
    }
    return [ordered]@{
        backend = $Backend
        phase = $Phase
        scheduledLaunches = [int]$selected.Count
        successfulLaunches = [int](@($selected | Where-Object { -not $_.excluded }).Count)
        excludedLaunches = [int](@($selected | Where-Object { $_.excluded }).Count)
        metrics = $metricStatistics
    }
}

function Assert-PairedPayloadFree {
    param([Parameter(Mandatory = $true)] [object]$Report)
    $json = $Report | ConvertTo-Json -Depth 20 -Compress
    if ($json -match $script:ForbiddenEvidencePropertyPattern) {
        throw 'The paired startup evidence schema contains a forbidden payload property.'
    }
    # JSON escaping makes a Windows path contain either a drive prefix or two
    # backslashes.  Neither is allowed in the payload-free report.
    if ($json -match '(?i)[A-Za-z]:\\\\|\\\\\\\\') {
        throw 'The paired startup evidence schema contains a path-shaped value.'
    }
    return $true
}

function New-PairedFailureEvidence {
    param(
        [Parameter(Mandatory = $true)] [string]$Stage,
        [Parameter(Mandatory = $true)] [string]$FailureType,
        [string]$FirstBackend = 'cpp',
        [string]$Platform = 'x64',
        [string]$Configuration = 'Debug',
        [int]$ScheduledLaunches = 0,
        [int]$SuccessfulLaunches = 0,
        [int]$SuppressedLaunches = 0,
        [bool]$CollectOnly = $false
    )
    $allowedTypes = @('preflight', 'integrity', 'launch-failure', 'cleanup-unverified', 'schema', 'write')
    $type = if ($allowedTypes -contains $FailureType) { $FailureType } else { 'preflight' }
    return [ordered]@{
        schemaVersion = $script:PairedSchemaVersion
        record = 'paired-gui-startup'
        payloadFree = $true
        status = 'failed'
        failure = [ordered]@{
            stage = if ([string]::IsNullOrWhiteSpace($Stage)) { 'unknown' } else { $Stage }
            type = $type
        }
        provenance = [ordered]@{
            status = 'unverified'
            buildManifestVerified = $false
            roleLabels = 'unverified'
            platform = $Platform
            configuration = $Configuration
        }
        configuration = [ordered]@{
            mode = if ($CollectOnly) { 'collect-only' } else { 'qualified' }
            platform = $Platform
            configuration = $Configuration
            firstBackend = $FirstBackend
            scheduledLaunches = [int][Math]::Max(0, $ScheduledLaunches)
            successfulLaunches = [int][Math]::Max(0, $SuccessfulLaunches)
        }
        termination = [ordered]@{
            status = 'terminated'
            type = $type
            completedLaunches = [int][Math]::Max(0, $SuccessfulLaunches)
            suppressedLaunches = [int][Math]::Max(0, $SuppressedLaunches)
            laterLaunchesSuppressed = $true
        }
        acceptance = [ordered]@{
            scheduledLaunches = [int][Math]::Max(0, $ScheduledLaunches)
            successfulLaunches = [int][Math]::Max(0, $SuccessfulLaunches)
            startupGatePass = $false
            qualified = $false
        }
        startupGatePass = $false
        adoption = [ordered]@{ decision = 'HOLD'; adoptionEligible = $false }
    }
}

function Write-PairedEvidenceEnvelope {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [object]$Evidence
    )
    $json = $null
    try {
        [void](Assert-PairedPayloadFree $Evidence)
        $json = $Evidence | ConvertTo-Json -Depth 20
    }
    catch {
        # Keep a fixed literal fallback so a schema/conversion failure cannot
        # erase the fact that a typed evidence envelope was attempted.
        $json = '{"schemaVersion":1,"record":"paired-gui-startup","payloadFree":true,"status":"failed","failure":{"stage":"schema","type":"schema"},"startupGatePass":false,"adoption":{"decision":"HOLD","adoptionEligible":false}}'
    }
    $encoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $json, $encoding)
    return $true
}

function Invoke-PairedSelfTest {
    [void](Assert-PairedAffinityMask $AffinityMask)
    $odd = Get-PairedStatistics @([double]1, 3, 2, 4, 9)
    Assert-PairedEqual 5 $odd.count 'odd statistics count'
    Assert-PairedEqual 3.0 $odd.medianMs 'odd statistics median'
    Assert-PairedEqual 9.0 $odd.p95Ms 'odd statistics p95'
    $even = Get-PairedStatistics @([double]1, 2, 3, 4)
    Assert-PairedEqual 2.5 $even.medianMs 'even statistics median'
    Assert-PairedEqual 4.0 $even.p95Ms 'even statistics p95'

    $schedule = @(Get-PairedSchedule -Warmups 5 -Measured 30 -First cpp)
    Assert-PairedEqual 70 $schedule.Count 'deterministic schedule size'
    Assert-PairedEqual 'cpp' $schedule[0].backend 'deterministic schedule first backend'
    Assert-PairedEqual 'rust' $schedule[1].backend 'deterministic schedule second backend'
    Assert-PairedEqual 'rust' $schedule[2].backend 'deterministic schedule alternating pair'
    Assert-PairedEqual 'cpp' $schedule[3].backend 'deterministic schedule alternating pair second backend'
    Assert-PairedEqual 5 @($schedule | Where-Object { $_.backend -eq 'cpp' -and $_.phase -eq 'warmup' }).Count 'C++ warmup schedule count'
    Assert-PairedEqual 30 @($schedule | Where-Object { $_.backend -eq 'rust' -and $_.phase -eq 'measured' }).Count 'Rust measured schedule count'
    Assert-PairedEqual 35 @($schedule | Select-Object -ExpandProperty pairIndex -Unique).Count 'unique pair index count'
    foreach ($pair in @(1..35)) {
        Assert-PairedEqual 2 @($schedule | Where-Object { $_.pairIndex -eq $pair }).Count ('pair index cardinality {0}' -f $pair)
    }
    $scheduleHash = Get-PairedScheduleHash $schedule
    if ($scheduleHash -notmatch '^[0-9a-f]{64}$') { throw 'Deterministic schedule hash self-test failed.' }
    $reversedHash = Get-PairedScheduleHash @($schedule | Sort-Object sequence -Descending)
    if ($reversedHash -eq $scheduleHash) { throw 'Schedule hash ignored launch order.' }

    $syntheticRuns = @(
        [pscustomobject]@{ backend = 'cpp'; phase = 'measured'; pairIndex = 6; excluded = $false; metrics = [ordered]@{ documentReadyMs = 100.0 } }
        [pscustomobject]@{ backend = 'rust'; phase = 'measured'; pairIndex = 6; excluded = $false; metrics = [ordered]@{ documentReadyMs = 101.0 } }
        [pscustomobject]@{ backend = 'cpp'; phase = 'measured'; pairIndex = 7; excluded = $false; metrics = [ordered]@{ documentReadyMs = 110.0 } }
        [pscustomobject]@{ backend = 'rust'; phase = 'measured'; pairIndex = 7; excluded = $false; metrics = [ordered]@{ documentReadyMs = 111.0 } }
    )
    $syntheticPerformance = New-PairedPerformanceSummary $syntheticRuns $schedule 2 $true
    if (-not $syntheticPerformance.pass -or -not $syntheticPerformance.gates.median -or -not $syntheticPerformance.gates.p95) {
        throw 'Synthetic performance gate self-test failed.'
    }
    $regressedRuns = @($syntheticRuns | ForEach-Object {
            $value = [double]$_.metrics.documentReadyMs
            if ($_.backend -eq 'rust') { $value += 20.0 }
            [pscustomobject]@{ backend = $_.backend; phase = $_.phase; pairIndex = $_.pairIndex; excluded = $_.excluded; metrics = [ordered]@{ documentReadyMs = $value } }
        })
    $regressedPerformance = New-PairedPerformanceSummary $regressedRuns $schedule 2 $true
    if ($regressedPerformance.pass -or $regressedPerformance.gates.median -or $regressedPerformance.gates.p95) {
        throw 'Synthetic performance regression was not rejected.'
    }
    $boundarySchedule = @(Get-PairedSchedule -Warmups 0 -Measured 20 -First cpp)
    $relativeBoundary = New-PairedPerformanceSummary (New-PairedSyntheticRuns $boundarySchedule 102.0001 102.0001 $false) $boundarySchedule 20 $true
    if ($relativeBoundary.pass -or $relativeBoundary.gates.median) {
        throw 'Median relative regression boundary self-test failed.'
    }
    $absoluteBoundary = New-PairedPerformanceSummary (New-PairedSyntheticRuns $boundarySchedule 101.0001 101.0001 $false) $boundarySchedule 20 $true
    if ($absoluteBoundary.pass -or $absoluteBoundary.gates.median) {
        throw 'Median absolute regression boundary self-test failed.'
    }
    $p95Boundary = New-PairedPerformanceSummary (New-PairedSyntheticRuns $boundarySchedule 100.0 105.0001 $true) $boundarySchedule 20 $true
    if ($p95Boundary.pass -or -not $p95Boundary.gates.median -or $p95Boundary.gates.p95) {
        throw 'P95 relative regression boundary self-test failed.'
    }

    $terminationSchedule = @(Get-PairedSchedule -Warmups 1 -Measured 1 -First cpp)
    $survivorRun = [pscustomobject]@{ processCleanupVerified = $false; profileCleanupVerified = $false }
    if (Test-PairedRunCleanupVerified $survivorRun) { throw 'Unverified cleanup self-test was accepted.' }
    $termination = New-PairedCampaignTermination -TotalEntries $terminationSchedule.Count -CompletedEntries 2 -TriggerRow $terminationSchedule[2]
    Assert-PairedEqual 'terminated' $termination.status 'campaign termination status'
    Assert-PairedEqual 'cleanup-unverified' $termination.type 'campaign termination type'
    Assert-PairedEqual 2 $termination.suppressedLaunches 'campaign suppressed launch count'
    if (-not $termination.excluded -or -not $termination.laterLaunchesSuppressed) { throw 'Campaign termination self-test did not suppress later launches.' }
    $completedCampaign = New-PairedCampaignTermination -TotalEntries $terminationSchedule.Count -CompletedEntries $terminationSchedule.Count -TriggerRow $null
    Assert-PairedEqual 'completed' $completedCampaign.status 'completed campaign status'
    Assert-PairedEqual 0 $completedCampaign.suppressedLaunches 'completed campaign suppression count'
    $reportTermination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries 1 -TriggerRow $schedule[0]
    $terminatedRun = [pscustomobject][ordered]@{
        sequence = [int]$schedule[0].sequence; pairIndex = [int]$schedule[0].pairIndex; slot = [int]$schedule[0].slot
        phase = [string]$schedule[0].phase; phaseIndex = [int]$schedule[0].phaseIndex; backend = [string]$schedule[0].backend
        status = 'survivor'; excluded = $true; metrics = $null
        affinity = [ordered]@{ requestedMask = [UInt64]1; processMask = $null; systemMask = $null; opened = $false; setSucceeded = $false; readBackSucceeded = $false; verified = $false; descendantsVerified = $false; errorCode = $null }
        profileSha256 = ('c' * 64); profileState = 'missing'; profileFileCount = 0
        processCleanupVerified = $false; profileCleanupVerified = $true; cleanupVerified = $false; survivorCount = 1
    }
    $selfTestProvenance = [ordered]@{
        status = 'verified'; roleLabels = 'manifest-declared'; buildManifestVerified = $true
        platform = 'x64'; configuration = 'Debug'; sourceHead = ('0' * 40); sourceDirty = $false
        sourceStatusSha256 = ('a' * 64); sourceStatusLineCount = 0
        outputBackend = 'paired(cpp,rust)'; utf16Backend = 'cpp'
        outputProductionPackage = $false; utf16ProductionPackage = $false
        manifests = [ordered]@{ cppSha256 = ('1' * 64); rustSha256 = ('2' * 64) }
        dependencyClosure = [ordered]@{
            cppSha256 = ('3' * 64); rustSha256 = ('4' * 64)
            cppRuntimeStageReceiptSha256 = ('5' * 64); rustRuntimeStageReceiptSha256 = ('6' * 64)
        }
        environment = [ordered]@{
            windowsImageIdentity = 'windows-selftest'; windowsImageSha256 = ('7' * 64)
            powerMode = 'Balanced'; buildParallelism = 1
        }
        toolchain = [ordered]@{
            msvc = 'msvc-selftest'; rust = 'rust-selftest'; rustLockSha256 = ('8' * 64)
            packagePlanSha256 = ('9' * 64); buildCommandSha256 = ('b' * 64)
        }
    }
    $selfTestSampleCopy = [pscustomobject][ordered]@{
        sha256 = ('b' * 64); sizeBytes = 1; physicalLines = 31; cleanupVerified = $true
    }
    $selfTestBundles = @(
        [pscustomobject][ordered]@{
            backend = 'cpp'; sourceHashBefore = ('d' * 64); sourceHashAfter = ('d' * 64)
            sourceSizeBefore = 1; sourceSizeAfter = 1; sourceUnchanged = $true
            copiedHashBefore = ('d' * 64); copiedHashAfter = ('d' * 64); copiedSizeBefore = 1; copiedSizeAfter = 1
            copiedUnchanged = $true; closureMode = 'runtime-stage-receipt'; dependencyClosureSha256 = ('3' * 64)
            runtimeStageReceiptSha256 = ('5' * 64); runtimeStageFileCount = 1
            sourceClosureUnchanged = $true; copiedClosureUnchanged = $true; receiptUnchanged = $true
            sidecarContract = $script:StartupProfileSidecarContract; sidecarSha256 = ('c' * 64); sidecarSizeBytes = 1
            sidecarMultiUser = 0; sidecarVerified = $true; cleanupVerified = $true
        }
        [pscustomobject][ordered]@{
            backend = 'rust'; sourceHashBefore = ('e' * 64); sourceHashAfter = ('e' * 64)
            sourceSizeBefore = 1; sourceSizeAfter = 1; sourceUnchanged = $true
            copiedHashBefore = ('e' * 64); copiedHashAfter = ('e' * 64); copiedSizeBefore = 1; copiedSizeAfter = 1
            copiedUnchanged = $true; closureMode = 'runtime-stage-receipt'; dependencyClosureSha256 = ('4' * 64)
            runtimeStageReceiptSha256 = ('6' * 64); runtimeStageFileCount = 1
            sourceClosureUnchanged = $true; copiedClosureUnchanged = $true; receiptUnchanged = $true
            sidecarContract = $script:StartupProfileSidecarContract; sidecarSha256 = ('c' * 64); sidecarSizeBytes = 1
            sidecarMultiUser = 0; sidecarVerified = $true; cleanupVerified = $true
        }
    )
    $terminatedReport = New-PairedReport -RunId 'selftest' -Commit ('0' * 40) `
        -HostIdentity ([pscustomobject]@{ sha256 = ('a' * 64); osVersion = 'test'; cpuManufacturer = 'test'; cpuModel = 'test'; physicalCores = 1; logicalProcessors = 1; architecture = 'X64' }) `
        -Sample ([pscustomobject]@{ sha256 = ('b' * 64); sizeBytes = 1; physicalLines = 31 }) `
        -ProfilePolicy ([pscustomobject]@{ kind = 'fresh-per-launch'; sha256 = ('c' * 64); artifactIsolation = 'campaign-artifact-bundle'; deletion = 'verified-after-each-launch'; sidecarContract = $script:StartupProfileSidecarContract }) `
        -CppArtifact ([pscustomobject]@{ sha256 = ('d' * 64); sizeBytes = 1 }) -RustArtifact ([pscustomobject]@{ sha256 = ('e' * 64); sizeBytes = 1 }) `
        -Schedule $schedule -Runs @($terminatedRun) -PairedScriptHash ('f' * 64) -SharedScriptHash ('1' * 64) -Termination $reportTermination `
        -ArtifactBundles $selfTestBundles -CollectOnly $false -Provenance $selfTestProvenance -SampleCopy $selfTestSampleCopy
    [void](Assert-PairedPayloadFree $terminatedReport)
    if ($terminatedReport.acceptance.qualified -or $terminatedReport.pass -or
        -not $terminatedReport.acceptance.campaignTerminated -or
        $terminatedReport.acceptance.suppressedLaunches -ne ($schedule.Count - 1) -or
        $terminatedReport.termination.suppressedLaunches -ne ($schedule.Count - 1)) {
        throw 'Terminated campaign report self-test did not remain nonqualified.'
    }

    $probe = [pscustomobject]@{
        RequestedMask = [UInt64]8; ProcessMask = [UInt64]8; SystemMask = [UInt64]15
        Opened = $true; SetSucceeded = $true; ReadBackSucceeded = $true; Verified = $true; DescendantsVerified = $true; ErrorCode = 0
    }
    $affinity = Get-AffinityMetadata $probe
    $convertedAffinity = Convert-PairedAffinity $affinity
    Assert-PairedEqual ([UInt64]8) $convertedAffinity.requestedMask 'affinity requested mask'
    Assert-PairedEqual ([UInt64]8) $convertedAffinity.processMask 'affinity process readback mask'
    if (-not $convertedAffinity.verified -or -not $convertedAffinity.descendantsVerified) { throw 'Affinity metadata self-test lost verification.' }
    $rejectedMask = $false
    try { [void](Assert-PairedAffinityMask 0) } catch { $rejectedMask = $true }
    if (-not $rejectedMask) { throw 'Zero affinity mask was accepted.' }

    $syntheticAncestry = @(
        [pscustomobject][ordered]@{ processId = 902; parentId = 901; depth = 2; sequence = 2 }
        [pscustomobject][ordered]@{ processId = 901; parentId = 900; depth = 1; sequence = 1 }
        [pscustomobject][ordered]@{ processId = 900; parentId = 0; depth = 0; sequence = 0 }
    )
    $ordered = @(Get-ParentFirstProcessOrder -Ancestry $syntheticAncestry)
    Assert-PairedEqual 900 $ordered[0].processId 'parent-first cleanup root'
    Assert-PairedEqual 901 $ordered[1].processId 'parent-first cleanup child'
    Assert-PairedEqual 902 $ordered[2].processId 'parent-first cleanup grandchild'
    $identity = [pscustomobject]@{ Id = 7; ParentId = 3; Creation = 100; ImagePath = 'C:\paired-test.exe' }
    $snapshot = @{ 7 = [pscustomobject]@{ Id = 7; ParentId = 3; Creation = 100; ImagePath = 'c:\PAIRED-TEST.EXE' } }
    if (-not (Test-ProcessIdentity $identity $snapshot)) { throw 'PID identity self-test rejected a case-insensitive same path.' }
    $cleanupRoot = Join-Path $env:TEMP ('startup-probe-selftest-' + [Guid]::NewGuid().ToString('N'))
    $cleanupName = 'startup-probe-selftest-profile'
    $cleanupPath = Join-Path $cleanupRoot $cleanupName
    $cleanupTreeVerified = $false
    try {
        [IO.Directory]::CreateDirectory((Join-Path $cleanupPath 'nested')) | Out-Null
        [IO.File]::WriteAllText((Join-Path $cleanupPath 'root.dat'), 'probe')
        [IO.File]::WriteAllText((Join-Path $cleanupPath 'nested\child.dat'), 'probe')
        $presentProfile = Get-PairedProfileDigest $cleanupPath
        if ($presentProfile.state -ne 'present' -or $presentProfile.fileCount -ne 2 -or $presentProfile.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw 'Profile tree digest self-test failed.'
        }
        Remove-OwnedProfile $cleanupPath $cleanupRoot $cleanupName
        $cleanupTreeVerified = -not (Test-Path -LiteralPath $cleanupPath)
        if (-not $cleanupTreeVerified) { throw 'Profile tree cleanup self-test left a directory behind.' }
    }
    finally {
        if (Test-Path -LiteralPath $cleanupRoot) { [IO.Directory]::Delete($cleanupRoot, $true) }
    }
    $missingProfile = Get-PairedProfileDigest (Join-Path $env:TEMP ('paired-startup-missing-' + [Guid]::NewGuid().ToString('N')))
    if ($missingProfile.sha256 -notmatch '^[0-9a-f]{64}$' -or $missingProfile.state -ne 'missing') { throw 'Profile digest self-test failed.' }

    $schemaReport = [ordered]@{
        schemaVersion = $script:PairedSchemaVersion
        record = 'paired-gui-startup'
        payloadFree = $true
        commit = '0123456789abcdef0123456789abcdef01234567'
        scripts = [ordered]@{ pairedRunnerSha256 = ('f' * 64); sharedStartupImplementationSha256 = ('e' * 64) }
        provenance = [ordered]@{ roleLabels = 'caller-supplied'; buildManifestVerified = $false }
        host = [ordered]@{ sha256 = ('a' * 64); osVersion = '10.0'; cpuManufacturer = 'Test'; cpuModel = 'Test CPU'; physicalCores = 1; logicalProcessors = 1; architecture = 'X64' }
        sample = [ordered]@{ sha256 = ('b' * 64); sizeBytes = 1; physicalLines = 31 }
        profilePolicy = [ordered]@{ kind = 'fresh-per-launch'; sha256 = ('c' * 64); artifactIsolation = 'campaign-artifact-bundle'; deletion = 'verified-after-each-launch'; sidecarContract = $script:StartupProfileSidecarContract }
        artifacts = @([ordered]@{ backend = 'cpp'; artifactSha256 = ('d' * 64); sizeBytes = 1 }, [ordered]@{ backend = 'rust'; artifactSha256 = ('e' * 64); sizeBytes = 1 })
        artifactBundles = @(
            [ordered]@{ backend = 'cpp'; sourceUnchanged = $true; copiedUnchanged = $true; sidecarVerified = $true; cleanupVerified = $true }
            [ordered]@{ backend = 'rust'; sourceUnchanged = $true; copiedUnchanged = $true; sidecarVerified = $true; cleanupVerified = $true }
        )
        configuration = [ordered]@{ warmupLaunches = 5; measuredLaunches = 30; affinityMask = 1; startupTimeoutMs = 30000; closeTimeoutMs = 3000 }
        order = [ordered]@{ firstBackend = 'cpp'; sequenceSha256 = $scheduleHash; launchCountPerBackend = 35 }
        runs = @()
        summaries = @()
        termination = [ordered]@{ status = 'completed'; type = 'none'; excluded = $false; triggerSequence = $null; triggerBackend = $null; completedLaunches = 70; suppressedLaunches = 0; laterLaunchesSuppressed = $false }
        cleanup = [ordered]@{ allProcessCleanupVerified = $true; allProfileCleanupVerified = $true; allBundleCleanupVerified = $true; survivorCount = 0 }
        acceptance = [ordered]@{ artifactBundlesVerified = $true; qualified = $true; requiredWarmupLaunches = 5; requiredMeasuredLaunches = 30 }
        performance = [ordered]@{ primaryMetric = 'documentReadyMs'; pass = $true; thresholds = [ordered]@{ medianRelativeRegressionPercent = 2.0; medianAbsoluteRegressionMs = 1.0; p95RelativeRegressionPercent = 5.0 } }
    }
    [void](Assert-PairedPayloadFree $schemaReport)
    $forbiddenReport = [ordered]@{ payloadFree = $true; caption = 'must-not-appear' }
    $rejectedSchema = $false
    try { [void](Assert-PairedPayloadFree $forbiddenReport) } catch { $rejectedSchema = $true }
    if (-not $rejectedSchema) { throw 'Forbidden schema property was accepted.' }

    return [ordered]@{
        selfTest = $true
        passed = $true
        noGuiLaunch = $true
        schemaVersion = $script:PairedSchemaVersion
        scheduleEntries = $schedule.Count
        scheduleHash = $scheduleHash
        p95Definition = 'nearest-rank-ceiling'
        cleanupOrder = @($ordered | ForEach-Object { [int]$_.processId })
        cleanupTreeVerified = [bool]$cleanupTreeVerified
        campaignTermination = [ordered]@{
            status = [string]$termination.status
            type = [string]$termination.type
            suppressedLaunches = [int]$termination.suppressedLaunches
            laterLaunchesSuppressed = [bool]$termination.laterLaunchesSuppressed
        }
        reportEarlyTerminationVerified = $true
        affinityReadBackVerified = [bool]$convertedAffinity.verified
        profileDigestState = [string]$missingProfile.state
        performanceGates = [ordered]@{
            primaryMetric = $script:PairedPrimaryMetric
            medianRelativeRegressionPercent = [double]$script:PairedMedianRelativeLimitPercent
            medianAbsoluteRegressionMs = [double]$script:PairedMedianAbsoluteLimitMs
            p95RelativeRegressionPercent = [double]$script:PairedP95RelativeLimitPercent
            syntheticPass = [bool]$syntheticPerformance.pass
            syntheticRegressionRejected = [bool](-not $regressedPerformance.pass)
        }
    }
}

function Invoke-PairedLaunch {
    param(
        [Parameter(Mandatory = $true)] [object]$ScheduleRow,
        [Parameter(Mandatory = $true)] [string]$ExecutablePath,
        [Parameter(Mandatory = $true)] [string]$ExecutableDirectory,
        [Parameter(Mandatory = $true)] [string]$SamplePath,
        [Parameter(Mandatory = $true)] [int]$ExpectedLines,
        [Parameter(Mandatory = $true)] [string]$ProfileName
    )
    $profilePath = Join-Path $ExecutableDirectory $ProfileName
    $raw = $null
    $profileDigest = $null
    $profileCleanupVerified = $false
    try {
        Assert-OwnedProfilePath $profilePath $ExecutableDirectory $ProfileName
        if (Test-Path -LiteralPath $profilePath) { throw 'A generated benchmark profile already exists.' }
        [void](Assert-StartupProfileSidecar $ExecutablePath)
        # An empty trace directory disables trace payloads for this evidence
        # runner; readiness remains the shared HWND/caption/idle/layout state.
        $raw = Invoke-StartupMeasurement $ScheduleRow.phase $ScheduleRow.sequence $ExecutablePath $SamplePath $ExpectedLines `
            $ProfileName $profilePath $ExecutableDirectory $false $null '' $AffinityMask
        $profileDigest = Get-PairedProfileDigest $profilePath
    }
    catch {
        $raw = [pscustomobject][ordered]@{
            success = $false; processCleanupVerified = $true; error = 'launch orchestration failed'
            affinity = [ordered]@{
                requestedMask = [UInt64]$AffinityMask; processMask = $null; systemMask = $null
                opened = $false; setSucceeded = $false; readBackSucceeded = $false
                verified = $false; descendantsVerified = $false; errorCode = $null
            }
            survivors = @()
        }
        try { $profileDigest = Get-PairedProfileDigest $profilePath } catch {
            $profileDigest = [pscustomobject][ordered]@{ sha256 = Get-TextSha256 'paired-startup-profile-unreadable-v1'; state = 'unreadable'; fileCount = 0 }
        }
    }
    finally {
        try {
            Remove-OwnedProfile $profilePath $ExecutableDirectory $ProfileName
            $profileCleanupVerified = -not (Test-Path -LiteralPath $profilePath)
        }
        catch { $profileCleanupVerified = $false }
    }
    if ($null -eq $profileDigest) {
        $profileDigest = [pscustomobject][ordered]@{ sha256 = Get-TextSha256 'paired-startup-profile-unreadable-v1'; state = 'unreadable'; fileCount = 0 }
    }
    if ($null -eq $raw) {
        $raw = [pscustomobject][ordered]@{
            success = $false; processCleanupVerified = $true; error = 'launch orchestration failed'
            affinity = [ordered]@{
                requestedMask = [UInt64]$AffinityMask; processMask = $null; systemMask = $null
                opened = $false; setSucceeded = $false; readBackSucceeded = $false
                verified = $false; descendantsVerified = $false; errorCode = $null
            }
            survivors = @()
        }
    }
    return Convert-PairedLaunchResult $raw $ScheduleRow $profileDigest $profileCleanupVerified
}

function New-PairedReport {
    param(
        [Parameter(Mandatory = $true)] [string]$RunId,
        [Parameter(Mandatory = $true)] [string]$Commit,
        [Parameter(Mandatory = $true)] [object]$HostIdentity,
        [Parameter(Mandatory = $true)] [object]$Sample,
        [Parameter(Mandatory = $true)] [object]$ProfilePolicy,
        [Parameter(Mandatory = $true)] [object]$CppArtifact,
        [Parameter(Mandatory = $true)] [object]$RustArtifact,
        [Parameter(Mandatory = $true)] [object[]]$Schedule,
        [Parameter(Mandatory = $true)] [object[]]$Runs,
        [Parameter(Mandatory = $true)] [string]$PairedScriptHash,
        [Parameter(Mandatory = $true)] [string]$SharedScriptHash,
        [Parameter(Mandatory = $true)] [object]$Termination,
        [Parameter(Mandatory = $true)] [object[]]$ArtifactBundles,
        [Parameter(Mandatory = $true)] [bool]$CollectOnly,
        [Parameter(Mandatory = $true)] [object]$Provenance,
        [Parameter(Mandatory = $true)] [object]$SampleCopy
    )
    $normalizedFirstBackend = Get-PairedCanonicalBackend $FirstBackend
    $summaries = @(
        New-PairedPhaseSummary $Runs 'cpp' 'warmup'
        New-PairedPhaseSummary $Runs 'rust' 'warmup'
        New-PairedPhaseSummary $Runs 'cpp' 'measured'
        New-PairedPhaseSummary $Runs 'rust' 'measured'
    )
    $cppMeasured = @($Runs | Where-Object { $_.backend -eq 'cpp' -and $_.phase -eq 'measured' -and -not $_.excluded })
    $rustMeasured = @($Runs | Where-Object { $_.backend -eq 'rust' -and $_.phase -eq 'measured' -and -not $_.excluded })
    $cppWarmups = @($Runs | Where-Object { $_.backend -eq 'cpp' -and $_.phase -eq 'warmup' -and -not $_.excluded })
    $rustWarmups = @($Runs | Where-Object { $_.backend -eq 'rust' -and $_.phase -eq 'warmup' -and -not $_.excluded })
    $survivorCount = @($Runs | ForEach-Object { [int]$_.survivorCount } | Measure-Object -Sum).Sum
    if ($null -eq $survivorCount) { $survivorCount = 0 }
    $failedCount = @($Runs | Where-Object { $_.excluded }).Count
    $accepted = $cppMeasured.Count -ge $MeasuredLaunches -and $rustMeasured.Count -ge $MeasuredLaunches -and
        $cppWarmups.Count -ge $WarmupLaunches -and $rustWarmups.Count -ge $WarmupLaunches -and
        $failedCount -eq 0 -and [bool](@($Runs | Where-Object { -not $_.cleanupVerified }).Count -eq 0) -and
        [bool](@($ArtifactBundles | Where-Object { -not $_.sourceUnchanged -or -not $_.copiedUnchanged -or -not $_.sourceClosureUnchanged -or -not $_.copiedClosureUnchanged -or -not $_.receiptUnchanged -or -not $_.sidecarVerified -or -not $_.cleanupVerified }).Count -eq 0) -and
        [bool]$Provenance.buildManifestVerified -and
        $Termination.status -eq 'completed' -and $Termination.suppressedLaunches -eq 0
    if ($CollectOnly) { $accepted = $false }
    $performance = New-PairedPerformanceSummary $Runs $Schedule $MeasuredLaunches $accepted
    return [ordered]@{
        schemaVersion = $script:PairedSchemaVersion
        record = 'paired-gui-startup'
        payloadFree = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        runId = $RunId
        commit = $Commit
        scripts = [ordered]@{
            pairedRunnerSha256 = $PairedScriptHash
            sharedStartupImplementationSha256 = $SharedScriptHash
        }
        provenance = $Provenance
        host = [ordered]@{
            sha256 = $HostIdentity.sha256
            osVersion = $HostIdentity.osVersion
            cpuManufacturer = $HostIdentity.cpuManufacturer
            cpuModel = $HostIdentity.cpuModel
            physicalCores = $HostIdentity.physicalCores
            logicalProcessors = $HostIdentity.logicalProcessors
            architecture = $HostIdentity.architecture
        }
        sample = [ordered]@{
            sha256 = $Sample.sha256
            sizeBytes = [UInt64]$Sample.sizeBytes
            physicalLines = [int]$Sample.physicalLines
            campaignCopySha256 = [string]$SampleCopy.sha256
            campaignCopySizeBytes = [UInt64]$SampleCopy.sizeBytes
            campaignCopyVerified = [bool]($SampleCopy.sha256 -eq $Sample.sha256 -and $SampleCopy.sizeBytes -eq $Sample.sizeBytes)
        }
        profilePolicy = [ordered]@{ kind = $ProfilePolicy.kind; sha256 = $ProfilePolicy.sha256; artifactIsolation = $ProfilePolicy.artifactIsolation; deletion = $ProfilePolicy.deletion; sidecarContract = $ProfilePolicy.sidecarContract }
        artifacts = @(
            [ordered]@{ backend = 'cpp'; artifactSha256 = $CppArtifact.sha256; sizeBytes = $CppArtifact.sizeBytes }
            [ordered]@{ backend = 'rust'; artifactSha256 = $RustArtifact.sha256; sizeBytes = $RustArtifact.sizeBytes }
        )
        artifactBundles = @($ArtifactBundles)
        configuration = [ordered]@{
            warmupLaunches = [int]$WarmupLaunches
            measuredLaunches = [int]$MeasuredLaunches
            launchesPerBackend = [int]($WarmupLaunches + $MeasuredLaunches)
            mode = if ($CollectOnly) { 'collect-only' } else { 'qualified' }
            minimumWarmupLaunches = [int]$script:PairedMinimumWarmupLaunches
            minimumMeasuredLaunches = [int]$script:PairedMinimumMeasuredLaunches
            affinityMask = [UInt64]$AffinityMask
            affinityReadBackRequired = $true
            startupTimeoutMs = $script:PairedStartupTimeoutMs
            closeTimeoutMs = $script:PairedCloseTimeoutMs
            pollIntervalMs = $script:PairedPollIntervalMs
             firstBackend = $normalizedFirstBackend
            samplePolicy = 'one-fixed-hashed-file'
            profilePolicySha256 = $ProfilePolicy.sha256
            platform = [string]$Provenance.platform
            configuration = [string]$Provenance.configuration
        }
        order = [ordered]@{
             firstBackend = $normalizedFirstBackend
            sequenceSha256 = Get-PairedScheduleHash $Schedule
            launchCountPerBackend = [int]($WarmupLaunches + $MeasuredLaunches)
            entries = @($Schedule | ForEach-Object { [ordered]@{ sequence = $_.sequence; pairIndex = $_.pairIndex; slot = $_.slot; phase = $_.phase; phaseIndex = $_.phaseIndex; backend = $_.backend } })
        }
        runs = @($Runs)
        summaries = $summaries
        termination = [ordered]@{
            status = [string]$Termination.status
            type = [string]$Termination.type
            excluded = [bool]$Termination.excluded
            triggerSequence = if ($null -eq $Termination.triggerSequence) { $null } else { [int]$Termination.triggerSequence }
            triggerBackend = if ($null -eq $Termination.triggerBackend) { $null } else { [string]$Termination.triggerBackend }
            failureType = if ($null -eq (Get-PairedProperty $Termination @('failureType'))) { $null } else { [string](Get-PairedProperty $Termination @('failureType')) }
            completedLaunches = [int]$Termination.completedLaunches
            suppressedLaunches = [int]$Termination.suppressedLaunches
            laterLaunchesSuppressed = [bool]$Termination.laterLaunchesSuppressed
        }
        cleanup = [ordered]@{
            allProcessCleanupVerified = @($Runs | Where-Object { -not $_.processCleanupVerified }).Count -eq 0
            allProfileCleanupVerified = @($Runs | Where-Object { -not $_.profileCleanupVerified }).Count -eq 0
            allCleanupVerified = @($Runs | Where-Object { -not $_.cleanupVerified }).Count -eq 0
            allBundleCleanupVerified = @($ArtifactBundles | Where-Object { -not $_.cleanupVerified }).Count -eq 0
            survivorCount = [int]$survivorCount
        }
        acceptance = [ordered]@{
            mode = if ($CollectOnly) { 'collect-only' } else { 'qualified' }
            collectOnly = [bool]$CollectOnly
            minimumsEnforced = -not $CollectOnly
            requiredWarmupLaunches = [int]$WarmupLaunches
            requiredMeasuredLaunches = [int]$MeasuredLaunches
            cppSuccessfulWarmups = [int]$cppWarmups.Count
            rustSuccessfulWarmups = [int]$rustWarmups.Count
            cppSuccessfulMeasured = [int]$cppMeasured.Count
            rustSuccessfulMeasured = [int]$rustMeasured.Count
            failedLaunches = [int]$failedCount
            campaignTerminated = [bool]($Termination.status -eq 'terminated')
            suppressedLaunches = [int]$Termination.suppressedLaunches
            artifactBundlesVerified = @($ArtifactBundles | Where-Object { -not $_.sourceUnchanged -or -not $_.copiedUnchanged -or -not $_.sourceClosureUnchanged -or -not $_.copiedClosureUnchanged -or -not $_.receiptUnchanged -or -not $_.sidecarVerified -or -not $_.cleanupVerified }).Count -eq 0
            scheduledLaunches = [int]$Schedule.Count
            successfulLaunches = [int](@($Runs | Where-Object { $_.status -eq 'succeeded' }).Count)
            qualified = [bool]$accepted
            startupGatePass = [bool]($accepted -and $performance.pass)
        }
        performance = $performance
        startupGatePass = [bool]($accepted -and $performance.pass)
        adoption = [ordered]@{
            decision = 'HOLD'
            adoptionEligible = $false
            reason = 'Issue #274 still requires correctness, build/package, provider workload, and compatibility gates.'
        }
        pass = [bool]($accepted -and $performance.pass)
    }
}

function New-PairedFailedRun {
    param(
        [Parameter(Mandatory = $true)] [object]$ScheduleRow,
        [Parameter(Mandatory = $true)] [string]$Status,
        [Parameter(Mandatory = $true)] [bool]$ProcessCleanupVerified,
        [Parameter(Mandatory = $true)] [bool]$ProfileCleanupVerified
    )
    return [pscustomobject][ordered]@{
        sequence = [int]$ScheduleRow.sequence
        pairIndex = [int]$ScheduleRow.pairIndex
        slot = [int]$ScheduleRow.slot
        phase = [string]$ScheduleRow.phase
        phaseIndex = [int]$ScheduleRow.phaseIndex
        backend = [string]$ScheduleRow.backend
        status = $Status
        excluded = $true
        metrics = $null
        affinity = [ordered]@{
            requestedMask = [UInt64]$AffinityMask
            processMask = $null
            systemMask = $null
            opened = $false
            setSucceeded = $false
            readBackSucceeded = $false
            verified = $false
            descendantsVerified = $false
            errorCode = $null
        }
        profileSha256 = Get-TextSha256 'paired-startup-profile-unreadable-v1'
        profileState = 'unreadable'
        profileFileCount = 0
        processCleanupVerified = $ProcessCleanupVerified
        profileCleanupVerified = $ProfileCleanupVerified
        cleanupVerified = $ProcessCleanupVerified -and $ProfileCleanupVerified
        survivorCount = 0
    }
}

function Invoke-PairedMeasurement {
    $runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'), [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $resultRoot = $null
    $reportPath = $null
    $stage = 'preflight'
    $failure = $null
    $cleanupFailure = $null
    $canonicalFirst = $null
    $canonicalPlatform = $null
    $canonicalConfiguration = $null
    $schedule = @()
    $runs = New-Object Collections.Generic.List[object]
    $ordinal = @{}
    $termination = $null
    $bundles = @{}
    $bundleVerifications = @{}
    $bundleCleanup = @{ cpp = $false; rust = $false }
    $sampleCopy = $null
    $sampleSource = $null
    try {
        $canonicalFirst = Get-PairedCanonicalBackend $FirstBackend
        $canonicalPlatform = Get-PairedCanonicalPlatform $Platform
        $canonicalConfiguration = Get-PairedCanonicalConfiguration $Configuration
        [void](Assert-PairedAffinityMask $AffinityMask)
        if ($WarmupLaunches -lt 1 -or $MeasuredLaunches -lt 1) {
            throw 'At least one warmup and one measured launch are required.'
        }
        if (-not $CollectOnly -and ($WarmupLaunches -lt $script:PairedMinimumWarmupLaunches -or $MeasuredLaunches -lt $script:PairedMinimumMeasuredLaunches)) {
            throw 'Qualified paired evidence requires at least 5 warmup and 30 measured launches per backend; use -CollectOnly for a smoke run.'
        }
        if (-not $CollectOnly -and ([string]::IsNullOrWhiteSpace($CppBuildManifest) -or [string]::IsNullOrWhiteSpace($RustBuildManifest))) {
            throw 'Qualified paired evidence requires one build manifest for each backend.'
        }
        if (-not $CollectOnly -and ([string]::IsNullOrWhiteSpace($CppRuntimeStageDirectory) -or [string]::IsNullOrWhiteSpace($RustRuntimeStageDirectory))) {
            throw 'Qualified paired evidence requires one runtime stage directory for each backend.'
        }
        $schedule = @(Get-PairedSchedule $WarmupLaunches $MeasuredLaunches $canonicalFirst)

        $resultRoot = [IO.Path]::GetFullPath($ResultDirectory)
        if (Test-Path -LiteralPath $resultRoot) {
            $rootItem = Get-Item -LiteralPath $resultRoot -Force -ErrorAction Stop
            if ($rootItem -isnot [IO.DirectoryInfo] -or (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                throw 'The paired result root must be a regular non-reparse directory.'
            }
        }
        else { [void][IO.Directory]::CreateDirectory($resultRoot) }
        $reportPath = Join-Path $resultRoot ('paired-startup-{0}.json' -f $runId)

        $stage = 'artifact-input'
        $cppPath = Resolve-PairedInputFile $CppSakuraExe 'CppSakuraExe'
        $rustPath = Resolve-PairedInputFile $RustSakuraExe 'RustSakuraExe'
        if ([StringComparer]::OrdinalIgnoreCase.Equals($cppPath, $rustPath)) {
            throw 'CppSakuraExe and RustSakuraExe must be distinct explicit executables.'
        }
        $cppArtifact = Get-PairedArtifactIdentity $cppPath
        $rustArtifact = Get-PairedArtifactIdentity $rustPath
        if ([StringComparer]::OrdinalIgnoreCase.Equals($cppArtifact.sha256, $rustArtifact.sha256)) {
            throw 'C++ and Rust sakura.exe artifacts must have distinct SHA-256 identities.'
        }
        $stage = 'sample-input'
        $samplePath = Resolve-PairedInputFile $StartupSample 'StartupSample'
        $sample = Get-PairedSampleIdentity $samplePath
        if (@(Get-ProcessesForImagePath $cppPath).Count -gt 0 -or
            @(Get-ProcessesForImagePath $rustPath).Count -gt 0) {
            throw 'A supplied sakura.exe already has a running instance; refusing to start paired evidence.'
        }
        $stage = 'source-state'
        $sourceState = Get-PairedSourceState
        $hostIdentity = Get-PairedHostIdentity
        if (-not $CollectOnly) { Assert-PairedHostIdentityQualified $hostIdentity }
        $profilePolicy = Get-PairedProfilePolicy
        $pairedScriptHash = Get-Sha256 $script:PairedScriptPath
        $sharedScriptHash = Get-Sha256 $script:SharedStartupScriptPath
        $provenance = $null
        $cppManifest = $null
        $rustManifest = $null
        $cppStage = $null
        $rustStage = $null
        if ($CollectOnly) {
            $provenance = New-PairedUnverifiedProvenance 'cpp' 'rust' $canonicalPlatform $canonicalConfiguration $sourceState
        }
        else {
            $stage = 'manifest-input'
            $cppManifest = Get-PairedBuildManifest $CppBuildManifest 'cpp' $canonicalPlatform $canonicalConfiguration $sourceState $cppArtifact
            $rustManifest = Get-PairedBuildManifest $RustBuildManifest 'rust' $canonicalPlatform $canonicalConfiguration $sourceState $rustArtifact
            $stage = 'runtime-stage-input'
            $cppStage = Get-PairedRuntimeStageIdentity $CppRuntimeStageDirectory $canonicalConfiguration $cppArtifact
            $rustStage = Get-PairedRuntimeStageIdentity $RustRuntimeStageDirectory $canonicalConfiguration $rustArtifact
            if ($cppManifest.runtimeStageReceiptSha256 -ne $cppStage.receiptSha256 -or
                $rustManifest.runtimeStageReceiptSha256 -ne $rustStage.receiptSha256) {
                throw 'A build manifest runtime-stage receipt does not match its staged artifact.'
            }
            if ($cppManifest.dependencyClosureSha256 -ne $cppStage.dependencyClosureSha256 -or
                $rustManifest.dependencyClosureSha256 -ne $rustStage.dependencyClosureSha256) {
                throw 'A build manifest dependency closure does not match its runtime stage.'
            }
            foreach ($field in @('windowsImageIdentity', 'windowsImageSha256', 'powerMode', 'buildParallelism',
                    'msvcIdentity', 'rustToolchain', 'rustLockSha256', 'packagePlanSha256', 'buildCommandSha256')) {
                if ([string]$cppManifest.$field -ne [string]$rustManifest.$field) {
                    throw "The paired build manifests disagree on $field."
                }
            }
            $provenance = New-PairedVerifiedProvenance $cppManifest $rustManifest $cppStage $rustStage $canonicalPlatform $canonicalConfiguration $sourceState
        }

        $stage = 'sample-copy'
        $sampleCopy = New-PairedSampleCopy $samplePath $resultRoot $runId $sample
        [void](Assert-PairedSampleUnchanged $sample $samplePath 'The source startup sample')
        [void](Assert-PairedSampleUnchanged $sample $sampleCopy.path 'The campaign startup sample copy')
        $stage = 'bundle-input'
        $bundles.cpp = New-PairedArtifactBundle $cppPath $resultRoot ('startup-probe-bundle-{0}-cpp' -f $runId) $CppRuntimeStageDirectory
        $bundles.rust = New-PairedArtifactBundle $rustPath $resultRoot ('startup-probe-bundle-{0}-rust' -f $runId) $RustRuntimeStageDirectory
        $stage = 'launch'
        foreach ($row in $schedule) {
            $backend = [string]$row.backend
            if (-not $ordinal.ContainsKey($backend)) { $ordinal[$backend] = 0 }
            ++$ordinal[$backend]
            $bundle = $bundles[$backend]
            $run = $null
            try {
                [void](Assert-StartupArtifactBundleUnchanged $bundle)
                if ($backend -eq 'cpp') { Assert-PairedArtifactUnchanged $cppArtifact }
                else { Assert-PairedArtifactUnchanged $rustArtifact }
                [void](Assert-PairedSampleUnchanged $sample $samplePath 'The source startup sample')
                [void](Assert-PairedSampleUnchanged $sample $sampleCopy.path 'The campaign startup sample copy')
                $executable = $bundle.executablePath
                $executableDirectory = $bundle.bundlePath
                $profileName = 'startup-probe-paired-{0}-{1}-{2:D3}' -f $runId, $backend, $ordinal[$backend]
                Write-Host ('launch {0}/{1}: {2} {3}' -f $row.sequence, $schedule.Count, $backend, $row.phase)
                $run = Invoke-PairedLaunch $row $executable $executableDirectory $sampleCopy.path $sampleCopy.physicalLines $profileName
            }
            catch {
                # No process is owned when an integrity/preflight check fails.
                # Preserve that fact instead of reporting a false survivor.
                $run = New-PairedFailedRun $row 'integrity' $true $true
            }
            [void]$runs.Add($run)
            if ($run.status -ne 'succeeded' -or -not (Test-PairedRunCleanupVerified $run)) {
                $termination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries $runs.Count -TriggerRow $row -TriggerRun $run
                break
            }
        }
        if ($null -eq $termination) {
            $termination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries $runs.Count -TriggerRow $null
        }
        $stage = 'postflight'
        foreach ($backend in @('cpp', 'rust')) {
            $bundleVerifications[$backend] = Assert-StartupArtifactBundleUnchanged $bundles[$backend]
        }
        [void](Assert-PairedSampleUnchanged $sample $samplePath 'The source startup sample')
        [void](Assert-PairedSampleUnchanged $sample $sampleCopy.path 'The campaign startup sample copy')
        Assert-PairedArtifactUnchanged $cppArtifact
        Assert-PairedArtifactUnchanged $rustArtifact
    }
    catch { $failure = $_ }
    finally {
        foreach ($backend in @('cpp', 'rust')) {
            if ($bundles.ContainsKey($backend) -and $null -ne $bundles[$backend]) {
                try {
                    Remove-StartupArtifactBundle $bundles[$backend]
                    $bundleCleanup[$backend] = -not (Test-Path -LiteralPath $bundles[$backend].bundlePath)
                }
                catch {
                    $bundleCleanup[$backend] = $false
                    if ($null -eq $cleanupFailure) { $cleanupFailure = 'bundle-cleanup' }
                }
            }
        }
        if ($null -ne $sampleCopy) {
            try {
                [void](Remove-PairedSampleCopy $sampleCopy $resultRoot $runId)
                $sampleCopy.cleanupVerified = $true
            }
            catch {
                $sampleCopy.cleanupVerified = $false
                if ($null -eq $cleanupFailure) { $cleanupFailure = 'sample-cleanup' }
            }
        }
    }

    if ($null -ne $cleanupFailure -and $null -eq $failure) {
        $failure = [pscustomobject]@{ Exception = [System.Exception]::new('Owned startup evidence cleanup failed.') }
        $stage = 'cleanup'
    }
    if ($null -ne $failure) {
        $failureType = if ($stage -eq 'cleanup') { 'cleanup-unverified' } elseif ($stage -eq 'postflight' -or $stage -eq 'manifest-input' -or $stage -eq 'runtime-stage-input' -or $stage -eq 'sample-input' -or $stage -eq 'sample-copy' -or $stage -eq 'bundle-input') { 'integrity' } elseif ($stage -eq 'schema') { 'schema' } elseif ($stage -eq 'write') { 'write' } else { 'preflight' }
        if ($null -eq $resultRoot) {
            try {
                $resultRoot = [IO.Path]::GetFullPath($ResultDirectory)
                [void][IO.Directory]::CreateDirectory($resultRoot)
            }
            catch { }
        }
        if ($null -ne $resultRoot) {
            if ($null -eq $reportPath) { $reportPath = Join-Path $resultRoot ('paired-startup-{0}.json' -f $runId) }
            $envelope = New-PairedFailureEvidence -Stage $stage -FailureType $failureType -FirstBackend $(if ($null -eq $canonicalFirst) { 'cpp' } else { $canonicalFirst }) -Platform $(if ($null -eq $canonicalPlatform) { 'x64' } else { $canonicalPlatform }) -Configuration $(if ($null -eq $canonicalConfiguration) { 'Debug' } else { $canonicalConfiguration }) -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly)
            try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }

    if ($null -eq $termination) {
        $termination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries $runs.Count -TriggerRow $null
    }
    if ($bundleVerifications.Count -ne 2) {
        $envelope = New-PairedFailureEvidence -Stage 'postflight' -FailureType 'integrity' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly)
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    $artifactBundles = @(
        Convert-PairedArtifactBundleVerification 'cpp' $bundleVerifications['cpp'] ([bool]$bundleCleanup['cpp']) $bundles['cpp']
        Convert-PairedArtifactBundleVerification 'rust' $bundleVerifications['rust'] ([bool]$bundleCleanup['rust']) $bundles['rust']
    )
    if ($null -eq $sampleCopy) {
        $sampleCopy = [pscustomobject][ordered]@{ sha256 = $sample.sha256; sizeBytes = $sample.sizeBytes; physicalLines = $sample.physicalLines; cleanupVerified = $false }
    }
    $stage = 'schema'
    try {
        $report = New-PairedReport -RunId $runId -Commit $sourceState.head -HostIdentity $hostIdentity -Sample $sample -SampleCopy $sampleCopy -ProfilePolicy $profilePolicy `
            -CppArtifact $cppArtifact -RustArtifact $rustArtifact -Schedule $schedule -Runs $runs.ToArray() `
            -PairedScriptHash $pairedScriptHash -SharedScriptHash $sharedScriptHash -Termination $termination `
            -ArtifactBundles $artifactBundles -CollectOnly ([bool]$CollectOnly) -Provenance $provenance
        [void](Assert-PairedPayloadFree $report)
    }
    catch {
        $envelope = New-PairedFailureEvidence -Stage 'schema' -FailureType 'schema' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly)
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    $stage = 'write'
    try {
        $json = $report | ConvertTo-Json -Depth 20
        [IO.File]::WriteAllText($reportPath, $json, (New-Object Text.UTF8Encoding($false)))
    }
    catch {
        $envelope = New-PairedFailureEvidence -Stage 'write' -FailureType 'write' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly)
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    return [pscustomobject][ordered]@{ exitCode = if ($report.pass) { 0 } else { 1 }; pass = [bool]$report.pass; reportPath = $reportPath }
}

try {
    if ($SelfTest) {
        Invoke-PairedSelfTest | ConvertTo-Json -Depth 10 -Compress
        exit 0
    }
    $measurement = Invoke-PairedMeasurement
    if (-not [string]::IsNullOrWhiteSpace([string]$measurement.reportPath)) {
        Write-Output $measurement.reportPath
    }
    exit [int]$measurement.exitCode
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}
