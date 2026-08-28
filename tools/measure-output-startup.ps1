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
$script:PairedRustOutputProviderSymbols = @(
    'sakura_output_provider_create_v1',
    'sakura_output_provider_apply_v1',
    'sakura_output_provider_snapshot_measure_v1',
    'sakura_output_provider_snapshot_write_v1',
    'sakura_output_provider_active_channel_v1',
    'sakura_output_provider_stop_v1',
    'sakura_output_provider_destroy_v1'
)
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
$script:PairedStartupMilestoneNames = @(
    'process-start', 'top-level-window', 'visible', 'caption', 'input-idle',
    'document-layout'
)
$script:PairedDiagnosticCheckpointNames = @('0.5s', '2s', '10s', 'timeout')
$script:PairedDiagnosticCheckpointMs = [ordered]@{
    '0.5s' = 500
    '2s' = 2000
    '10s' = 10000
    'timeout' = 30000
}
$script:PairedDiagnosticMaxProcessCount = 256
$script:PairedDiagnosticMaxImageNameLength = 260
$script:PairedDiagnosticMaxWindowCount = 1024
$script:PairedStartupTraceRoles = @('editor', 'control', 'unknown')
$script:PairedStartupTraceEventAllowlist = @(
    'process_entry', 'isa_dispatch',
    'factory_begin', 'factory_end',
    'control_spawn_begin', 'control_spawn_end',
    'control_wait_begin', 'control_wait_end', 'control_wait_result',
    'control_initialize_begin', 'control_shared_data_ready', 'control_tray_created',
    'control_ready_event_begin', 'control_ready_event_end',
    'editor_spawn_begin', 'editor_spawn_end',
    'editor_wait_begin', 'editor_wait_end', 'editor_wait_result',
    'editor_ready_event_begin', 'editor_ready_event_end',
    'uipi_check_begin', 'uipi_check_end',
    'read_begin', 'read_end',
    'layout_begin', 'layout_decision', 'startup_layout_input_summary', 'layout_complete',
    'startup_document_armed', 'startup_document_complete', 'startup_document_aborted',
    'startup_draw_commit_begin', 'startup_draw_commit_end',
    'startup_draw_layout_begin', 'startup_draw_layout_end',
    'startup_draw_scroll_begin', 'startup_draw_scroll_end',
    'startup_draw_show_begin', 'startup_draw_show_end',
    'startup_draw_redraw_begin', 'startup_draw_redraw_end',
    'first_content_paint_begin', 'first_content_paint_end',
    'first_content_paint_prepare_begin', 'first_content_paint_prepare_end',
    'first_content_paint_lines_begin', 'first_content_paint_lines_end',
    'first_content_paint_finish_begin', 'first_content_paint_finish_end',
    'first_content_advance_width_summary', 'first_content_draw_width_summary',
    'first_content_text_output_summary', 'first_content_text_volume_summary',
    'first_content_text_block_summary', 'first_content_text_block_font_summary',
    'first_content_text_boundary_summary', 'first_content_text_scan_summary',
    'first_content_nonblock_text_range_summary', 'first_content_nonblock_text_risk_summary',
    'first_content_nonblock_text_other_summary', 'first_content_painted',
    'startup_draw_minimap_paint_summary', 'startup_draw_minimap_update_summary',
    'startup_document_subphase_summary', 'startup_read_decision_summary',
    'startup_read_result_summary', 'startup_read_worker_summary',
    'startup_read_worker_lifecycle_summary', 'startup_read_transfer_summary',
    'startup_minimap_cache_summary', 'startup_minimap_build_summary',
    'startup_make_one_line_summary', 'startup_make_one_line_work_summary',
    'startup_make_one_line_cost_summary'
)
$script:ForbiddenEvidencePropertyPattern =
    '(?i)"(?:path|imagePath|commandLine|arguments|caption|text|document|profileName|sampleMarkdown|sakuraExe|outputDirectory|exception|message|detail|bundlePath|sidecarPath|profilePath|executablePath|sourcePath|manifestPath|runtimeStagePath|samplePath|dependencyPath)"\s*:'

function Get-PairedProperty {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string[]]$Names
    )
    if ($null -eq $Object) { return $null }
    if ($Object -is [Collections.IDictionary]) {
        foreach ($name in $Names) {
            if ($Object.Contains($name)) { return $Object[$name] }
        }
    }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Test-PairedPropertyPresent {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string]$Name
    )
    if ($null -eq $Object) { return $false }
    if ($Object -is [Collections.IDictionary]) { return $Object.Contains($Name) }
    return $null -ne $Object.PSObject.Properties[$Name]
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
    param([Parameter(Mandatory = $true)] [AllowNull()] [AllowEmptyString()] [object]$Value)
    if ($null -eq $Value) { throw 'Text SHA-256 input cannot be null.' }
    if ($Value -isnot [string]) { throw 'Text SHA-256 input must be a string.' }
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

function New-PairedSourceState {
    param(
        [Parameter(Mandatory = $true)] [string]$Head,
        [Parameter(Mandatory = $true)] [AllowNull()] [AllowEmptyString()] [object]$StatusText
    )
    if ($null -eq $StatusText) { throw 'Repository source status cannot be null.' }
    if ($StatusText -isnot [string]) { throw 'Repository source status must be a string.' }
    $canonicalStatus = ($statusText -replace "`r`n", "`n" -replace "`r", "`n").TrimEnd("`n")
    if ([string]::IsNullOrEmpty($canonicalStatus)) {
        $statusLines = [object[]]@()
    }
    else {
        $statusLines = @($canonicalStatus -split "`n")
    }
    return [pscustomobject][ordered]@{
        head = $Head
        dirty = [bool]($statusLines.Count -ne 0)
        statusSha256 = Get-TextSha256 $canonicalStatus
        statusLineCount = [int]$statusLines.Count
    }
}

function Get-PairedSourceState {
    $statusText = Get-PairedGitText @('status', '--porcelain=v1', '--untracked-files=all')
    return New-PairedSourceState -Head (Get-PairedCommitHash) -StatusText $statusText
}

function Get-PairedScriptIdentity {
    return [pscustomobject][ordered]@{
        pairedRunnerSha256 = Get-Sha256 $script:PairedScriptPath
        sharedStartupImplementationSha256 = Get-Sha256 $script:SharedStartupScriptPath
    }
}

function Assert-PairedSourceStateUnchanged {
    param(
        [Parameter(Mandatory = $true)] [object]$Expected,
        [Parameter(Mandatory = $true)] [string]$Label
    )
    $actual = Get-PairedSourceState
    if ([string]$actual.head -ne [string]$Expected.head -or
        [bool]$actual.dirty -ne [bool]$Expected.dirty -or
        [string]$actual.statusSha256 -ne [string]$Expected.statusSha256 -or
        [int]$actual.statusLineCount -ne [int]$Expected.statusLineCount) {
        throw "$Label source state changed during the paired measurement."
    }
    return $actual
}

function Assert-PairedScriptIdentityUnchanged {
    param(
        [Parameter(Mandatory = $true)] [object]$Expected,
        [Parameter(Mandatory = $true)] [string]$Label
    )
    $actual = Get-PairedScriptIdentity
    if ([string]$actual.pairedRunnerSha256 -ne [string]$Expected.pairedRunnerSha256 -or
        [string]$actual.sharedStartupImplementationSha256 -ne [string]$Expected.sharedStartupImplementationSha256) {
        throw "$Label measurement script identity changed during the paired measurement."
    }
    return $actual
}

function New-PairedMeasurementArguments {
    param(
        [Parameter(Mandatory = $true)] [string]$First,
        [Parameter(Mandatory = $true)] [string]$Platform,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [int]$Warmups,
        [Parameter(Mandatory = $true)] [int]$Measured,
        [Parameter(Mandatory = $true)] [UInt64]$Mask,
        [Parameter(Mandatory = $true)] [bool]$CollectOnly
    )
    return [ordered]@{
        schemaVersion = 1
        runner = 'paired-gui-startup'
        platform = $Platform
        configuration = $Configuration
        firstBackend = $First
        warmupLaunches = [int]$Warmups
        measuredLaunches = [int]$Measured
        affinityMask = [UInt64]$Mask
        collectOnly = [bool]$CollectOnly
        startupTimeoutMs = [int]$script:PairedStartupTimeoutMs
        closeTimeoutMs = [int]$script:PairedCloseTimeoutMs
        pollIntervalMs = [int]$script:PairedPollIntervalMs
        primaryMetric = $script:PairedPrimaryMetric
        samplePolicy = 'one-fixed-hashed-file'
        profilePolicy = 'fresh-per-launch|campaign-artifact-bundle|verified-after-each-launch'
    }
}

function Get-PairedMeasurementCommandSha256 {
    param([Parameter(Mandatory = $true)] [object]$Arguments)
    $parts = New-Object Collections.Generic.List[string]
    foreach ($entry in @($Arguments.GetEnumerator() | Sort-Object Key)) {
        [void]$parts.Add(('{0}={1}' -f [string]$entry.Key, [string]$entry.Value))
    }
    return Get-TextSha256 ($parts.ToArray() -join '|')
}

function Get-PairedFailureTypeForStage {
    param([AllowNull()] [string]$Stage)
    if ($Stage -eq 'cleanup') { return 'cleanup-unverified' }
    if ($Stage -eq 'postflight' -or $Stage -eq 'report-integrity' -or
        $Stage -eq 'manifest-input' -or $Stage -eq 'runtime-stage-input' -or
        $Stage -eq 'sample-input' -or $Stage -eq 'sample-copy' -or $Stage -eq 'bundle-input') {
        return 'integrity'
    }
    if ($Stage -eq 'schema') { return 'schema' }
    if ($Stage -eq 'write') { return 'write' }
    return 'preflight'
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
        $relativePath = Require-PairedManifestString (Get-PairedProperty $entry @('canonicalRelativePath', 'relativePath', 'path', 'name', 'destination')) 'dependencyClosure.relativePath'
        $relativePath = $relativePath.Replace('/', '\')
        $pathParts = @($relativePath -split '\\')
        if ([IO.Path]::IsPathRooted($relativePath) -or $relativePath.IndexOf(':') -ge 0 -or
            @($pathParts | Where-Object { [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[\x00-\x1f<>\"|?*]' }).Count -gt 0) {
            throw 'Build manifest dependency closure contains an unsafe relative path.'
        }
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

function Require-PairedManifestUInt64 {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    if ($null -eq $Value -or [string]$Value -notmatch '^[0-9]+$') {
        throw "Build manifest field '$FieldName' is missing or invalid."
    }
    try { return [UInt64]$Value }
    catch { throw "Build manifest field '$FieldName' is missing or invalid." }
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
    $record = Require-PairedManifestString (Get-PairedManifestField $manifest @('record') @()) 'record'
    if ($record -cne 'output-startup-build-manifest') {
        throw "The $Backend build manifest record is not producer-generated."
    }
    $payloadFree = Require-PairedManifestBoolean (Get-PairedManifestField $manifest @('payloadFree') @()) 'payloadFree'
    if (-not $payloadFree) { throw "The $Backend build manifest is not payload-free." }
    $manifestStatus = Require-PairedManifestString (Get-PairedManifestField $manifest @('status') @()) 'status'
    if ($manifestStatus -cne 'committed') { throw "The $Backend build manifest is not committed." }
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
    $sourceStatusLineCountValue = Get-PairedManifestField $manifest @('sourceStatusLineCount', 'statusLineCount') @('source', 'provenance', 'build')
    if ($null -eq $sourceStatusLineCountValue -or [string]$sourceStatusLineCountValue -notmatch '^[0-9]+$') {
        throw "Build manifest field 'sourceStatusLineCount' is missing or invalid."
    }
    try { $sourceStatusLineCount = [Int64]$sourceStatusLineCountValue } catch { throw "Build manifest field 'sourceStatusLineCount' is missing or invalid." }
    if ($sourceStatusLineCount -lt 0 -or $sourceStatusLineCount -gt [Int32]::MaxValue) {
        throw "Build manifest field 'sourceStatusLineCount' is missing or invalid."
    }
    if ($sourceHead -ne $ExpectedSource.head.ToLowerInvariant() -or
        $sourceDirty -ne [bool]$ExpectedSource.dirty -or
        $sourceStatusHash -ne $ExpectedSource.statusSha256.ToLowerInvariant() -or
        [int]$sourceStatusLineCount -ne [int]$ExpectedSource.statusLineCount) {
        throw "The $Backend build manifest source state does not match the current checkout."
    }
    if ([bool]$ExpectedSource.dirty -or $sourceDirty) {
        throw 'Qualified paired evidence requires a clean checkout.'
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
    $powerModeHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('powerModeSha256', 'powerSha256') @('host', 'power', 'toolchain', 'provenance', 'build')) 'powerModeSha256'
    $buildCommandHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('buildCommandSha256', 'commandSha256', 'buildCommandHash') @('build', 'provenance', 'toolchain')) 'buildCommandSha256'
    $packagePlanCommandHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('packagePlanCommandSha256', 'packageCommandSha256') @('build', 'provenance', 'toolchain')) 'packagePlanCommandSha256'
    $runtimeStageCommandHash = Require-PairedManifestHash (Get-PairedManifestField $manifest @('runtimeStageCommandSha256', 'stageCommandSha256') @('build', 'runtimeStage', 'provenance', 'toolchain')) 'runtimeStageCommandSha256'

    $canonicalRuntimeStage = Require-PairedManifestBoolean (Get-PairedManifestField $manifest @('canonicalRuntimeStage') @('runtimeStage', 'provenance', 'build')) 'canonicalRuntimeStage'
    if (-not $canonicalRuntimeStage) { throw "The $Backend build manifest does not identify the canonical runtime stage." }
    $transaction = Get-PairedProperty $manifest @('transaction')
    if ($null -eq $transaction) { throw "The $Backend build manifest has no producer transaction proof." }
    $transactionStatus = Require-PairedManifestString (Get-PairedProperty $transaction @('status')) 'transaction.status'
    $publication = Require-PairedManifestString (Get-PairedProperty $transaction @('publication')) 'transaction.publication'
    $beforeVerified = Require-PairedManifestBoolean (Get-PairedProperty $transaction @('artifactBeforeVerified')) 'transaction.artifactBeforeVerified'
    $afterVerified = Require-PairedManifestBoolean (Get-PairedProperty $transaction @('artifactAfterVerified')) 'transaction.artifactAfterVerified'
    $stageVerified = Require-PairedManifestBoolean (Get-PairedProperty $transaction @('runtimeStageVerified')) 'transaction.runtimeStageVerified'
    $producerGenerated = Require-PairedManifestBoolean (Get-PairedProperty $transaction @('manifestGeneratedByProducer')) 'transaction.manifestGeneratedByProducer'
    if ($transactionStatus -cne 'committed' -or $publication -cne 'atomic-directory-rename' -or
        -not $beforeVerified -or -not $afterVerified -or -not $stageVerified -or -not $producerGenerated) {
        throw "The $Backend build manifest transaction proof is incomplete."
    }

    $selectorProof = Get-PairedProperty $manifest @('selectorProof')
    if ($null -eq $selectorProof) { throw "The $Backend build manifest has no selector proof." }
    $canonicalConfiguration = Get-PairedCanonicalConfiguration $ExpectedConfiguration
    $selectorResult = Require-PairedManifestString (Get-PairedProperty $selectorProof @('result')) 'selectorProof.result'
    $expectedSelectorResult = if ($canonicalConfiguration -eq 'Release') {
        'msvc-ltcg-compile-selector-verified'
    }
    else {
        'dumpbin-unresolved-refs-verified'
    }
    if ($selectorResult -cne $expectedSelectorResult) {
        throw "The $Backend build manifest selector proof does not match the requested configuration."
    }
    $selectorOutputBackend = (Require-PairedManifestString (Get-PairedProperty $selectorProof @('outputBackend')) 'selectorProof.outputBackend').ToLowerInvariant()
    $selectorUtf16Backend = (Require-PairedManifestString (Get-PairedProperty $selectorProof @('utf16Backend')) 'selectorProof.utf16Backend').ToLowerInvariant()
    if ($selectorOutputBackend -ne $Backend -or $selectorUtf16Backend -ne 'cpp') {
        throw "The $Backend build manifest selector proof has unexpected providers."
    }
    foreach ($flag in @('outputProductionPackage', 'utf16ProductionPackage', 'utf16BenchmarkTelemetry', 'assemblyListings')) {
        $flagValue = Require-PairedManifestBoolean (Get-PairedProperty $selectorProof @($flag)) ('selectorProof.' + $flag)
        if ($flagValue) { throw "The $Backend build manifest selector proof enables $flag." }
    }
    $selectorVerificationMethod = Require-PairedManifestString (Get-PairedProperty $selectorProof @('verificationMethod')) 'selectorProof.verificationMethod'
    $selectorObjectFormat = Require-PairedManifestString (Get-PairedProperty $selectorProof @('providerObjectFormat')) 'selectorProof.providerObjectFormat'
    $expectedVerificationMethod = if ($canonicalConfiguration -eq 'Release') { 'msvc-ltcg-compile-selector' } else { 'dumpbin-object-undefined' }
    $expectedObjectFormat = if ($canonicalConfiguration -eq 'Release') { 'msvc-ltcg-anonymous' } else { 'coff-symbols' }
    if ($selectorVerificationMethod -cne $expectedVerificationMethod -or $selectorObjectFormat -cne $expectedObjectFormat) {
        throw "The $Backend build manifest selector proof method or object format is invalid for the requested configuration."
    }
    $selectorObjectAfter = Require-PairedManifestHash (Get-PairedProperty $selectorProof @('providerObjectSha256After')) 'selectorProof.providerObjectSha256After'
    $selectorObjectSize = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('providerObjectSizeBytesAfter')) 'selectorProof.providerObjectSizeBytesAfter'
    if ($selectorObjectSize -lt 1) { throw 'Build manifest field selectorProof.providerObjectSizeBytesAfter is missing or invalid.' }
    if (-not (Test-PairedPropertyPresent $selectorProof 'providerObjectSha256Before')) {
        throw 'Build manifest field selectorProof.providerObjectSha256Before is missing.'
    }
    $selectorObjectBeforeValue = Get-PairedProperty $selectorProof @('providerObjectSha256Before')
    $selectorObjectBefore = if ($null -eq $selectorObjectBeforeValue) {
        $null
    }
    else {
        Require-PairedManifestHash $selectorObjectBeforeValue 'selectorProof.providerObjectSha256Before'
    }

    $compileLogExistsBefore = Require-PairedManifestBoolean (Get-PairedProperty $selectorProof @('compileLogExistsBefore')) 'selectorProof.compileLogExistsBefore'
    $compileLogExistsAfter = Require-PairedManifestBoolean (Get-PairedProperty $selectorProof @('compileLogExistsAfter')) 'selectorProof.compileLogExistsAfter'
    if (-not (Test-PairedPropertyPresent $selectorProof 'compileLogSha256Before') -or
        -not (Test-PairedPropertyPresent $selectorProof 'compileLogSha256After')) {
        throw 'Build manifest selector proof compile log hashes are missing.'
    }
    $compileLogHashBeforeValue = Get-PairedProperty $selectorProof @('compileLogSha256Before')
    $compileLogHashAfterValue = Get-PairedProperty $selectorProof @('compileLogSha256After')
    $compileLogHashBefore = if ($compileLogExistsBefore) {
        Require-PairedManifestHash $compileLogHashBeforeValue 'selectorProof.compileLogSha256Before'
    }
    else {
        if ($null -ne $compileLogHashBeforeValue) { throw 'The build manifest selector proof has a hash for a missing compile log before the build.' }
        $null
    }
    $compileLogHashAfter = if ($compileLogExistsAfter) {
        Require-PairedManifestHash $compileLogHashAfterValue 'selectorProof.compileLogSha256After'
    }
    else {
        if ($null -ne $compileLogHashAfterValue) { throw 'The build manifest selector proof has a hash for a missing compile log after the build.' }
        $null
    }
    $compileLogSizeBefore = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('compileLogSizeBytesBefore')) 'selectorProof.compileLogSizeBytesBefore'
    $compileLogSizeAfter = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('compileLogSizeBytesAfter')) 'selectorProof.compileLogSizeBytesAfter'
    if (($compileLogExistsBefore -and $compileLogSizeBefore -lt 1) -or
        (-not $compileLogExistsBefore -and $compileLogSizeBefore -ne 0) -or
        ($compileLogExistsAfter -and $compileLogSizeAfter -lt 1) -or
        (-not $compileLogExistsAfter -and $compileLogSizeAfter -ne 0)) {
        throw 'The build manifest selector proof compile log presence and size are inconsistent.'
    }
    $compileLogProof = Require-PairedManifestBoolean (Get-PairedProperty $selectorProof @('compileLogProof')) 'selectorProof.compileLogProof'
    $compileCommandHasGl = Require-PairedManifestBoolean (Get-PairedProperty $selectorProof @('compileCommandHasGl')) 'selectorProof.compileCommandHasGl'
    $compileSelectorCountValue = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('compileCommandRustSelectorDefineCount')) 'selectorProof.compileCommandRustSelectorDefineCount'
    $expectedCompileSelectorCount = if ($Backend -eq 'rust') { [UInt64]1 } else { [UInt64]0 }
    if ($canonicalConfiguration -eq 'Release') {
        if (-not $compileLogExistsAfter -or -not $compileLogProof -or -not $compileCommandHasGl -or
            $compileSelectorCountValue -ne $expectedCompileSelectorCount) {
            throw "The $Backend build manifest lacks the required Release compile-selector proof."
        }
    }
    elseif ($compileLogProof -or $compileCommandHasGl -or $compileSelectorCountValue -ne 0) {
        throw "The $Backend build manifest has an invalid Debug compile-selector proof."
    }
    $compileSelectorCount = [int]$compileSelectorCountValue

    $selectorSymbolsPropertyPresent = Test-PairedPropertyPresent $selectorProof 'unresolvedProviderSymbols'
    if (-not $selectorSymbolsPropertyPresent) { throw 'Build manifest field selectorProof.unresolvedProviderSymbols is missing.' }
    $selectorSymbolsValue = @(Get-PairedProperty $selectorProof @('unresolvedProviderSymbols'))
    $rawSelectorSymbols = @($selectorSymbolsValue | ForEach-Object {
            if (-not (Test-PairedNonEmptyIdentity $_)) { throw 'The build manifest selector proof contains an invalid unresolved symbol.' }
            ([string]$_).ToLowerInvariant()
        })
    $selectorSymbols = @($rawSelectorSymbols | Sort-Object -Unique)
    if ($rawSelectorSymbols.Count -ne $selectorSymbols.Count) {
        throw 'The build manifest selector proof contains duplicate unresolved symbols.'
    }
    $selectorSymbolCount = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('unresolvedProviderSymbolCount')) 'selectorProof.unresolvedProviderSymbolCount'
    if ($selectorSymbolCount -ne $rawSelectorSymbols.Count) {
        throw 'The build manifest selector proof symbol count is invalid.'
    }
    $expectedSelectorSymbols = if ($canonicalConfiguration -eq 'Debug' -and $Backend -eq 'rust') {
        @($script:PairedRustOutputProviderSymbols)
    }
    else {
        @()
    }
    $expectedSelectorSymbols = @($expectedSelectorSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    if ((@($selectorSymbols) -join '|') -cne (@($expectedSelectorSymbols) -join '|')) {
        throw "The $Backend build manifest selector proof symbol set is invalid for the requested configuration."
    }

    if (-not (Test-PairedPropertyPresent $selectorProof 'rustArchiveResult') -or
        -not (Test-PairedPropertyPresent $selectorProof 'rustArchiveSha256') -or
        -not (Test-PairedPropertyPresent $selectorProof 'rustArchiveSizeBytes') -or
        -not (Test-PairedPropertyPresent $selectorProof 'definedProviderSymbols') -or
        -not (Test-PairedPropertyPresent $selectorProof 'definedProviderSymbolCount')) {
        throw 'The build manifest selector proof archive evidence is incomplete.'
    }
    $archiveResult = Require-PairedManifestString (Get-PairedProperty $selectorProof @('rustArchiveResult')) 'selectorProof.rustArchiveResult'
    if ($archiveResult -cne 'dumpbin-defined-exports-verified') {
        throw "The $Backend build manifest selector proof archive export verification is incomplete."
    }
    $archiveHash = Require-PairedManifestHash (Get-PairedProperty $selectorProof @('rustArchiveSha256')) 'selectorProof.rustArchiveSha256'
    $archiveSize = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('rustArchiveSizeBytes')) 'selectorProof.rustArchiveSizeBytes'
    if ($archiveSize -lt 1) { throw 'Build manifest field selectorProof.rustArchiveSizeBytes is missing or invalid.' }
    $definedSymbolsValue = @(Get-PairedProperty $selectorProof @('definedProviderSymbols'))
    $rawDefinedSymbols = @($definedSymbolsValue | ForEach-Object {
            if (-not (Test-PairedNonEmptyIdentity $_)) { throw 'The build manifest selector proof contains an invalid defined symbol.' }
            ([string]$_).ToLowerInvariant()
        })
    $definedSymbols = @($rawDefinedSymbols | Sort-Object -Unique)
    if ($rawDefinedSymbols.Count -ne $definedSymbols.Count) {
        throw 'The build manifest selector proof contains duplicate defined symbols.'
    }
    $expectedDefinedSymbols = @($script:PairedRustOutputProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    if ((@($definedSymbols) -join '|') -cne (@($expectedDefinedSymbols) -join '|')) {
        throw 'The build manifest selector proof defined symbol set is invalid.'
    }
    $definedSymbolCount = Require-PairedManifestUInt64 (Get-PairedProperty $selectorProof @('definedProviderSymbolCount')) 'selectorProof.definedProviderSymbolCount'
    if ($definedSymbolCount -ne $rawDefinedSymbols.Count -or $definedSymbolCount -ne $expectedDefinedSymbols.Count) {
        throw 'The build manifest selector proof defined symbol count is invalid.'
    }

    $selectorContractHash = Require-PairedManifestHash (Get-PairedProperty $selectorProof @('selectorContractSha256')) 'selectorProof.selectorContractSha256'
    $selectorBaseCanonical = if ($canonicalConfiguration -eq 'Release') {
        $compileLogBeforeCanonical = if ($compileLogExistsBefore) { $compileLogHashBefore } else { 'missing' }
        'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|method={2}|symbols=|object-after={3}|object-format={4}|compile-log-before={5}|compile-log-before-size={6}|compile-log-after={7}|compile-log-after-size={8}|compile-gl={9}|compile-rust-selector-count={10}' -f
            $Backend, $selectorResult, $selectorVerificationMethod, $selectorObjectAfter, $selectorObjectFormat,
            $compileLogBeforeCanonical, $compileLogSizeBefore, $compileLogHashAfter, $compileLogSizeAfter,
            $compileCommandHasGl, $compileSelectorCount
    }
    else {
        'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|symbols={2}|object-after={3}' -f
            $Backend, $selectorResult, (@($selectorSymbols) -join ','), $selectorObjectAfter
    }
    $selectorCanonical = '{0}|archive-result={1}|archive={2}|defined={3}' -f
        (Get-TextSha256 $selectorBaseCanonical), $archiveResult, $archiveHash, (@($definedSymbols) -join ',')
    if ($selectorContractHash -ne (Get-TextSha256 $selectorCanonical)) {
        throw "The $Backend build manifest selector proof contract hash is invalid."
    }
    $manifestSelectorHash = Require-PairedManifestHash (Get-PairedProperty $manifest @('selectorProofSha256')) 'selectorProofSha256'
    if ($manifestSelectorHash -ne $selectorContractHash) {
        throw "The $Backend build manifest selector proof hash does not match its contract."
    }

    $manifestObjectAfter = Require-PairedManifestHash (Get-PairedProperty $manifest @('providerObjectSha256After')) 'providerObjectSha256After'
    if ($manifestObjectAfter -ne $selectorObjectAfter) {
        throw "The $Backend build manifest provider object hash does not match its selector proof."
    }
    if (-not (Test-PairedPropertyPresent $manifest 'providerObjectSha256Before')) {
        throw "The $Backend build manifest providerObjectSha256Before mirror is missing."
    }
    $manifestObjectBeforeValue = Get-PairedProperty $manifest @('providerObjectSha256Before')
    $manifestObjectBefore = if ($null -eq $manifestObjectBeforeValue) {
        $null
    }
    else {
        Require-PairedManifestHash $manifestObjectBeforeValue 'providerObjectSha256Before'
    }
    if ($manifestObjectBefore -ne $selectorObjectBefore) {
        throw "The $Backend build manifest provider object before hash does not match its selector proof."
    }
    $manifestObjectFormat = Require-PairedManifestString (Get-PairedProperty $manifest @('providerObjectFormat')) 'providerObjectFormat'
    $manifestVerificationMethod = Require-PairedManifestString (Get-PairedProperty $manifest @('verificationMethod')) 'verificationMethod'
    if ($manifestObjectFormat -cne $selectorObjectFormat -or $manifestVerificationMethod -cne $selectorVerificationMethod) {
        throw "The $Backend build manifest selector format mirrors are stale."
    }
    $manifestCompileLogExistsBefore = Require-PairedManifestBoolean (Get-PairedProperty $manifest @('compileLogExistsBefore')) 'compileLogExistsBefore'
    $manifestCompileLogExistsAfter = Require-PairedManifestBoolean (Get-PairedProperty $manifest @('compileLogExistsAfter')) 'compileLogExistsAfter'
    $manifestCompileLogHashBeforeValue = Get-PairedProperty $manifest @('compileLogSha256Before')
    $manifestCompileLogHashAfterValue = Get-PairedProperty $manifest @('compileLogSha256After')
    if (-not (Test-PairedPropertyPresent $manifest 'compileLogSha256Before') -or
        -not (Test-PairedPropertyPresent $manifest 'compileLogSha256After')) {
        throw "The $Backend build manifest compile log hash mirrors are missing."
    }
    $manifestCompileLogHashBefore = if ($manifestCompileLogExistsBefore) {
        Require-PairedManifestHash $manifestCompileLogHashBeforeValue 'compileLogSha256Before'
    }
    else {
        if ($null -ne $manifestCompileLogHashBeforeValue) { throw "The $Backend build manifest has a hash for a missing compile log before the build." }
        $null
    }
    $manifestCompileLogHashAfter = if ($manifestCompileLogExistsAfter) {
        Require-PairedManifestHash $manifestCompileLogHashAfterValue 'compileLogSha256After'
    }
    else {
        if ($null -ne $manifestCompileLogHashAfterValue) { throw "The $Backend build manifest has a hash for a missing compile log after the build." }
        $null
    }
    $manifestCompileLogSizeBefore = Require-PairedManifestUInt64 (Get-PairedProperty $manifest @('compileLogSizeBytesBefore')) 'compileLogSizeBytesBefore'
    $manifestCompileLogSizeAfter = Require-PairedManifestUInt64 (Get-PairedProperty $manifest @('compileLogSizeBytesAfter')) 'compileLogSizeBytesAfter'
    $manifestCompileLogProof = Require-PairedManifestBoolean (Get-PairedProperty $manifest @('compileLogProof')) 'compileLogProof'
    $manifestCompileCommandHasGl = Require-PairedManifestBoolean (Get-PairedProperty $manifest @('compileCommandHasGl')) 'compileCommandHasGl'
    $manifestCompileSelectorCount = Require-PairedManifestUInt64 (Get-PairedProperty $manifest @('compileCommandRustSelectorDefineCount')) 'compileCommandRustSelectorDefineCount'
    if ($manifestCompileLogExistsBefore -ne $compileLogExistsBefore -or
        $manifestCompileLogExistsAfter -ne $compileLogExistsAfter -or
        $manifestCompileLogHashBefore -cne $compileLogHashBefore -or
        $manifestCompileLogHashAfter -cne $compileLogHashAfter -or
        $manifestCompileLogSizeBefore -ne $compileLogSizeBefore -or
        $manifestCompileLogSizeAfter -ne $compileLogSizeAfter -or
        $manifestCompileLogProof -ne $compileLogProof -or
        $manifestCompileCommandHasGl -ne $compileCommandHasGl -or
        $manifestCompileSelectorCount -ne $compileSelectorCount) {
        throw "The $Backend build manifest compile proof mirrors are stale."
    }

    [void](Assert-PairedPayloadFree $manifest)
    return [pscustomobject][ordered]@{
        manifestSha256 = Get-Sha256 $manifestPath
        schemaVersion = [int]$schema
        record = $record
        payloadFree = [bool]$payloadFree
        status = $manifestStatus
        backend = $role
        platform = $platform
        configuration = $configuration
        sourceHead = $sourceHead
        sourceDirty = [bool]$sourceDirty
        sourceStatusSha256 = $sourceStatusHash
        sourceStatusLineCount = [int]$sourceStatusLineCount
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
        powerModeSha256 = $powerModeHash
        buildParallelism = $buildParallelism
        msvcIdentity = $msvcIdentity
        rustToolchain = $rustToolchain
        rustLockSha256 = $rustLockHash
        packagePlanSha256 = $packagePlanHash
        buildCommandSha256 = $buildCommandHash
        packagePlanCommandSha256 = $packagePlanCommandHash
        runtimeStageCommandSha256 = $runtimeStageCommandHash
        canonicalRuntimeStage = [bool]$canonicalRuntimeStage
        transactionStatus = $transactionStatus
        transactionPublication = $publication
        manifestGeneratedByProducer = [bool]$producerGenerated
        selectorProofResult = $selectorResult
        selectorProofSha256 = $manifestSelectorHash
        selectorProofVerificationMethod = $selectorVerificationMethod
        selectorProofProviderObjectFormat = $selectorObjectFormat
        selectorProofObjectSha256After = $selectorObjectAfter
        selectorProofObjectSizeBytesAfter = [UInt64]$selectorObjectSize
        selectorProofObjectSha256Before = $selectorObjectBefore
        selectorProofCompileLogExistsBefore = $compileLogExistsBefore
        selectorProofCompileLogExistsAfter = $compileLogExistsAfter
        selectorProofCompileLogSha256Before = $compileLogHashBefore
        selectorProofCompileLogSizeBytesBefore = [UInt64]$compileLogSizeBefore
        selectorProofCompileLogSha256After = $compileLogHashAfter
        selectorProofCompileLogSizeBytesAfter = [UInt64]$compileLogSizeAfter
        selectorProofCompileLogProof = $compileLogProof
        selectorProofCompileCommandHasGl = $compileCommandHasGl
        selectorProofCompileCommandRustSelectorDefineCount = [int]$compileSelectorCount
        selectorProofUnresolvedProviderSymbols = $selectorSymbols
        selectorProofUnresolvedProviderSymbolCount = [int]$selectorSymbolCount
        selectorProofRustArchiveResult = $archiveResult
        selectorProofRustArchiveSha256 = $archiveHash
        selectorProofRustArchiveSizeBytes = [UInt64]$archiveSize
        selectorProofDefinedProviderSymbols = $definedSymbols
        selectorProofDefinedProviderSymbolCount = [int]$definedSymbolCount
    }
}

function Convert-PairedRuntimeReceiptPath {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    $path = [string]$Value
    if ([string]::IsNullOrWhiteSpace($path) -or $path -ne $path.Trim() -or $path.IndexOf([char]0) -ge 0) {
        throw "The runtime-stage receipt $FieldName is empty or contains unsafe whitespace."
    }
    $normalized = $path.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($normalized) -or $normalized.IndexOf(':') -ge 0) {
        throw "The runtime-stage receipt $FieldName must be relative and may not contain an alternate data stream."
    }
    $parts = @($normalized -split '\\')
    if ($parts.Count -eq 0 -or @($parts | Where-Object {
            [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[\x00-\x1f<>\"|?*]' -or
            $_ -match '[ \.]$' -or $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$'
        }).Count -gt 0) {
        throw "The runtime-stage receipt $FieldName contains an unsafe path component."
    }
    return ($parts -join '\')
}

function Get-PairedRuntimeReceiptPathBinding {
    param(
        [Parameter(Mandatory = $true)] [string]$Destination,
        [Parameter(Mandatory = $true)] [string]$Source,
        [Parameter(Mandatory = $true)] [string]$Context
    )
    if ($Context -cne 'msvc-x64-debug' -and $Context -cne 'msvc-x64-release') {
        throw 'The runtime-stage receipt context must be the canonical MSVC x64 context.'
    }
    $destinationPath = Convert-PairedRuntimeReceiptPath $Destination 'destination'
    $sourcePath = Convert-PairedRuntimeReceiptPath $Source 'source'
    $destinationPrefix = ('build/staging/{0}/sakura-editor/' -f $Context).Replace('/', '\')
    $configuration = if ($Context -ceq 'msvc-x64-debug') { 'Debug' } else { 'Release' }
    $sourcePrefix = ('x64/{0}/' -f $configuration).Replace('/', '\')
    if (-not $destinationPath.StartsWith($destinationPrefix, [StringComparison]::Ordinal) -or
        -not $sourcePath.StartsWith($sourcePrefix, [StringComparison]::Ordinal)) {
        throw 'The runtime-stage receipt source or destination is outside its canonical prefix.'
    }
    $destinationSuffix = $destinationPath.Substring($destinationPrefix.Length)
    $sourceSuffix = $sourcePath.Substring($sourcePrefix.Length)
    if ([string]::IsNullOrWhiteSpace($destinationSuffix) -or
        -not [StringComparer]::Ordinal.Equals($destinationSuffix, $sourceSuffix)) {
        throw 'The runtime-stage receipt source and destination suffixes do not match.'
    }
    return [pscustomobject][ordered]@{
        destination = $destinationPath
        source = $sourcePath
        relativePath = $destinationSuffix
    }
}

function Assert-PairedRuntimeReceiptArtifactIdentity {
    param(
        [Parameter(Mandatory = $true)] [string]$ArtifactId,
        [Parameter(Mandatory = $true)] [string]$Role,
        [Parameter(Mandatory = $true)] [string]$RelativePath,
        [Parameter(Mandatory = $true)] [string]$Context
    )
    if ([string]::IsNullOrWhiteSpace($ArtifactId) -or $ArtifactId -match '[\r\n]' -or
        [string]::IsNullOrWhiteSpace($Role) -or $Role -match '[\r\n]') {
        throw 'The runtime-stage receipt artifact_id and role must be non-empty identities.'
    }
    if ([StringComparer]::Ordinal.Equals($RelativePath, 'sakura.exe')) {
        if (-not [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context)) -or
            -not [StringComparer]::Ordinal.Equals($Role, 'editor')) {
            throw 'The runtime-stage receipt editor identity is not canonical.'
        }
        return
    }
    if ([StringComparer]::Ordinal.Equals($Role, 'editor') -or
        [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context))) {
        throw 'Only sakura.exe may use the runtime-stage editor identity.'
    }
    $knownLanguages = @{
        'sakura_lang_en_US.dll' = [pscustomobject]@{ artifactId = 'sakura-language-en-us-resource'; role = 'language-en-us' }
        'sakura_lang_zh_CN.dll' = [pscustomobject]@{ artifactId = 'sakura-language-zh-cn-resource'; role = 'language-zh-cn' }
    }
    if (($ArtifactId -ceq 'sakura-language-en-us-resource' -or $Role -ceq 'language-en-us') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_en_US.dll')) {
        throw 'The runtime-stage en-US language identity must use its canonical top-level path.'
    }
    if (($ArtifactId -ceq 'sakura-language-zh-cn-resource' -or $Role -ceq 'language-zh-cn') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_zh_CN.dll')) {
        throw 'The runtime-stage zh-CN language identity must use its canonical top-level path.'
    }
    if ($knownLanguages.ContainsKey($RelativePath)) {
        $expected = $knownLanguages[$RelativePath]
        if (-not [StringComparer]::Ordinal.Equals($ArtifactId, $expected.artifactId) -or
            -not [StringComparer]::Ordinal.Equals($Role, $expected.role)) {
            throw 'The runtime-stage receipt language identity is not canonical.'
        }
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
    if (-not [StringComparer]::Ordinal.Equals($context, $expectedContext)) {
        throw 'The runtime stage receipt context does not match the paired run.'
    }
    $files = @(Get-PairedProperty $receipt @('files'))
    if ($files.Count -eq 0) { throw 'The runtime stage receipt declares no files.' }
    $editorSeen = $false
    $totalSize = [UInt64]0
    $seenNames = @{}
    $seenDestinations = @{}
    $closureEntries = New-Object Collections.Generic.List[object]
    foreach ($entry in $files) {
        $artifactId = Require-PairedManifestString (Get-PairedProperty $entry @('artifact_id')) 'runtimeStage.artifact_id'
        $destination = Require-PairedManifestString (Get-PairedProperty $entry @('destination')) 'runtimeStage.destination'
        $source = Require-PairedManifestString (Get-PairedProperty $entry @('source')) 'runtimeStage.source'
        $role = Require-PairedManifestString (Get-PairedProperty $entry @('role')) 'runtimeStage.role'
        $binding = Get-PairedRuntimeReceiptPathBinding $destination $source $context
        $name = $binding.relativePath
        Assert-PairedRuntimeReceiptArtifactIdentity $artifactId $role $name $context
        if ($seenDestinations.ContainsKey($binding.destination.ToLowerInvariant())) {
            throw 'The runtime stage receipt contains a duplicate destination.'
        }
        $seenDestinations[$binding.destination.ToLowerInvariant()] = $true
        $declaredHashValue = [string](Get-PairedProperty $entry @('sha256'))
        $declaredHashValue = $declaredHashValue -replace '^(?i:sha256:)', ''
        $declaredHash = Require-PairedManifestHash $declaredHashValue 'runtimeStage.sha256'
        $declaredSize = Get-PairedProperty $entry @('size', 'sizeBytes')
        if ($null -eq $declaredSize -or [UInt64]$declaredSize -lt 1) { throw 'The runtime stage receipt declares an invalid file size.' }
        if ([string]::IsNullOrWhiteSpace($name) -or $seenNames.ContainsKey($name.ToLowerInvariant())) {
            throw 'The runtime stage receipt contains an ambiguous file name.'
        }
        $seenNames[$name.ToLowerInvariant()] = $true
        $candidate = Join-Path $stageRoot $name
        Assert-StartupNonReparsePath $candidate $stageRoot
        $identity = Get-PairedArtifactIdentity $candidate
        if ($identity.sha256 -ne $declaredHash -or $identity.sizeBytes -ne [UInt64]$declaredSize) {
            throw 'The runtime stage file does not match its receipt identity.'
        }
        $totalSize += [UInt64]$declaredSize
        [void]$closureEntries.Add([pscustomobject][ordered]@{
            relativePath = $name
            canonicalRelativePath = $binding.destination
            artifactId = $artifactId
            role = $role
            source = $binding.source
            destination = $binding.destination
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
            powerModeSha256 = [string]$CppManifest.powerModeSha256
            buildParallelism = [int]$CppManifest.buildParallelism
        }
        toolchain = [ordered]@{
            msvc = [string]$CppManifest.msvcIdentity
            rust = [string]$RustManifest.rustToolchain
            rustLockSha256 = [string]$RustManifest.rustLockSha256
            packagePlanSha256 = [string]$CppManifest.packagePlanSha256
            buildCommandSha256 = [string]$CppManifest.buildCommandSha256
            buildCommands = [ordered]@{
                cpp = [string]$CppManifest.buildCommandSha256
                rust = [string]$RustManifest.buildCommandSha256
            }
            packagePlanCommands = [ordered]@{
                cpp = [string]$CppManifest.packagePlanCommandSha256
                rust = [string]$RustManifest.packagePlanCommandSha256
            }
            runtimeStageCommands = [ordered]@{
                cpp = [string]$CppManifest.runtimeStageCommandSha256
                rust = [string]$RustManifest.runtimeStageCommandSha256
            }
        }
        producerContract = [ordered]@{
            record = 'output-startup-build-manifest'
            status = 'committed'
            payloadFree = $true
            transactionPublication = 'atomic-directory-rename'
            manifestGeneratedByProducer = $true
            selectorProofResult = [string]$CppManifest.selectorProofResult
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
                buildCommandSha256 = [string]$CppManifest.buildCommandSha256
                packagePlanCommandSha256 = [string]$CppManifest.packagePlanCommandSha256
                runtimeStageCommandSha256 = [string]$CppManifest.runtimeStageCommandSha256
                selectorProofResult = [string]$CppManifest.selectorProofResult
                selectorProofSha256 = [string]$CppManifest.selectorProofSha256
                selectorProofObjectSha256After = [string]$CppManifest.selectorProofObjectSha256After
                selectorProofUnresolvedProviderSymbols = @($CppManifest.selectorProofUnresolvedProviderSymbols)
                selectorProofUnresolvedProviderSymbolCount = [int]$CppManifest.selectorProofUnresolvedProviderSymbolCount
            }
            rust = [ordered]@{
                backend = [string]$RustManifest.backend
                platform = [string]$RustManifest.platform
                configuration = [string]$RustManifest.configuration
                outputBackend = [string]$RustManifest.outputBackend
                utf16Backend = [string]$RustManifest.utf16Backend
                exeSha256 = [string]$RustManifest.exeSha256
                dependencyClosureSha256 = [string]$RustManifest.dependencyClosureSha256
                buildCommandSha256 = [string]$RustManifest.buildCommandSha256
                packagePlanCommandSha256 = [string]$RustManifest.packagePlanCommandSha256
                runtimeStageCommandSha256 = [string]$RustManifest.runtimeStageCommandSha256
                selectorProofResult = [string]$RustManifest.selectorProofResult
                selectorProofSha256 = [string]$RustManifest.selectorProofSha256
                selectorProofObjectSha256After = [string]$RustManifest.selectorProofObjectSha256After
                selectorProofUnresolvedProviderSymbols = @($RustManifest.selectorProofUnresolvedProviderSymbols)
                selectorProofUnresolvedProviderSymbolCount = [int]$RustManifest.selectorProofUnresolvedProviderSymbolCount
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
    if (-not [bool](Get-PairedProperty $Raw @('processCleanupVerified'))) { return 'survivor' }
    if (-not $ProfileCleanupVerified) { return 'profileCleanup' }
    $affinity = Get-PairedProperty $Raw @('affinity')
    if ($null -eq $affinity -or -not [bool](Get-PairedProperty $affinity @('verified'))) { return 'affinity' }
    if ([string](Get-PairedProperty $Raw @('error')) -match '(?i)timed out|timeout') { return 'timeout' }
    return 'startup'
}

function Get-PairedFailureStage {
    param(
        [Parameter(Mandatory = $true)] [string]$FailureType,
        [Parameter(Mandatory = $true)] [object]$Milestones
    )
    switch ($FailureType) {
        'timeout' {
            $timeoutStage = [string](Get-PairedProperty $Milestones @('timeoutStage'))
            if ($timeoutStage -in @('window-discovery', 'readiness')) { return $timeoutStage }
            return 'startup'
        }
        'survivor' { return 'cleanup' }
        'profileCleanup' { return 'profile-cleanup' }
        'affinity' { return 'affinity' }
        'diagnostic-unavailable' { return 'diagnostics' }
        'trace-unavailable' { return 'trace' }
        'trace-cleanup' { return 'trace-cleanup' }
        'startup' {
            if (-not [bool](Get-PairedProperty $Milestones @('processStarted'))) { return 'process-start' }
            if (-not [bool](Get-PairedProperty $Milestones @('topLevelWindowObserved'))) { return 'window-discovery' }
            return 'readiness'
        }
        default { return 'startup' }
    }
}

function Convert-PairedElapsedMs {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value) { return $null }
    $elapsed = 0.0
    try { $elapsed = [double]$Value } catch { return $null }
    if ([double]::IsNaN($elapsed) -or [double]::IsInfinity($elapsed) -or
        $elapsed -lt 0.0 -or $elapsed -gt [double]$script:PairedStartupTimeoutMs) {
        return $null
    }
    return [double]([Math]::Round($elapsed, 3))
}

function Convert-PairedVerticalScrollMaximum {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value) { return $null }
    $maximum = 0.0
    try { $maximum = [double]$Value } catch { return $null }
    if ([double]::IsNaN($maximum) -or [double]::IsInfinity($maximum) -or
        $maximum -lt 0.0 -or $maximum -gt [double][int]::MaxValue -or
        [Math]::Truncate($maximum) -ne $maximum) {
        return $null
    }
    return [int]$maximum
}

function Convert-PairedDiagnosticPid {
    param([AllowNull()] [object]$Value, [bool]$AllowZero = $false)
    if ($null -eq $Value) { return $null }
    $processId = 0L
    try { $processId = [Int64]$Value } catch { return $null }
    $minimum = if ($AllowZero) { 0L } else { 1L }
    if ($processId -lt $minimum -or $processId -gt [int]::MaxValue) { return $null }
    return [int]$processId
}

function Convert-PairedDiagnosticCreationTime {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value) { return $null }
    $creation = 0L
    try { $creation = [Int64]$Value } catch { return $null }
    if ($creation -lt 0L) { return $null }
    return [Int64]$creation
}

function Convert-PairedDiagnosticImageName {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value) { return 'unavailable' }
    $name = [string]$Value
    if ([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt $script:PairedDiagnosticMaxImageNameLength -or
        $name -match '[\\/:\r\n]') {
        return 'unavailable'
    }
    return $name
}

function Convert-PairedDiagnosticBoolean {
    param([AllowNull()] [object]$Value)
    if ($Value -is [bool]) { return [bool]$Value }
    if ($Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or $Value -is [int64] -or $Value -is [uint64]) {
        if ([Int64]$Value -eq 0) { return $false }
        if ([Int64]$Value -eq 1) { return $true }
    }
    return $null
}

function New-PairedEmptyProcessDiagnosticSnapshot {
    param([Parameter(Mandatory = $true)] [string]$Checkpoint)
    return [ordered]@{
        checkpoint = $Checkpoint
        targetMs = [int]$script:PairedDiagnosticCheckpointMs[$Checkpoint]
        status = 'not-reached'
        observed = $false
        elapsedMs = $null
        processTree = @()
        processCount = 0
        processRecordsTruncated = $false
        jobMembershipVerified = $false
        jobMemberCount = 0
        topLevelWindowCount = $null
        topLevelWindowCountCapped = $false
        rootExitState = 'not-observed'
        rootExitCode = $null
        rootExitErrorCode = $null
        processExitObserved = $false
        processExitElapsedMs = $null
        failureStage = $null
        failureType = $null
    }
}

function New-PairedEmptyStartupDiagnostics {
    $snapshots = New-Object Collections.Generic.List[object]
    foreach ($checkpoint in @($script:PairedDiagnosticCheckpointNames)) {
        [void]$snapshots.Add((New-PairedEmptyProcessDiagnosticSnapshot $checkpoint))
    }
    return [ordered]@{
        observationStatus = 'not-attempted'
        schemaVersion = 1
        processTreeSnapshots = $snapshots.ToArray()
        processExitObservation = [ordered]@{
            observed = $false
            elapsedMs = $null
            pid = $null
            source = 'run-root'
            state = 'not-observed'
            exitCode = $null
            errorCode = $null
        }
    }
}

function New-PairedEmptyStartupTrace {
    return [ordered]@{
        status = 'not-attempted'
        eventCounts = @()
    }
}

function New-PairedUnavailableStartupTrace {
    return [ordered]@{
        status = 'unavailable'
        eventCounts = @()
    }
}

function Convert-PairedProcessDiagnosticRecord {
    param([Parameter(Mandatory = $true)] [object]$Record)
    $processId = Convert-PairedDiagnosticPid (Get-PairedProperty $Record @('pid', 'processId'))
    $ppid = Convert-PairedDiagnosticPid (Get-PairedProperty $Record @('ppid', 'parentPid', 'parentProcessId')) $true
    $creation = Convert-PairedDiagnosticCreationTime (Get-PairedProperty $Record @('creationTime', 'creation'))
    if ($null -eq $processId -or $null -eq $ppid -or $null -eq $creation) { return $null }
    $jobMember = Convert-PairedDiagnosticBoolean (Get-PairedProperty $Record @('jobMember', 'jobMembership'))
    return [ordered]@{
        pid = [int]$processId
        ppid = [int]$ppid
        imageName = Convert-PairedDiagnosticImageName (Get-PairedProperty $Record @('imageName', 'image'))
        creationTime = [Int64]$creation
        jobMember = $jobMember
    }
}

function Convert-PairedDiagnosticExitCode {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value) { return $null }
    try {
        $code = [UInt64]$Value
        if ($code -gt [UInt32]::MaxValue) { return $null }
        return [UInt32]$code
    }
    catch { return $null }
}

function Convert-PairedDiagnosticExitState {
    param([AllowNull()] [object]$Record)
    $state = [string](Get-PairedProperty $Record @('state', 'exitState', 'rootExitState'))
    if ($state -notin @('active', 'exited', 'not-observed', 'unavailable')) {
        return [ordered]@{ state = 'unavailable'; exitCode = $null; errorCode = $null }
    }
    $exitCode = Convert-PairedDiagnosticExitCode (Get-PairedProperty $Record @('exitCode', 'code', 'rootExitCode'))
    if (($state -eq 'active' -or $state -eq 'exited') -and $null -eq $exitCode) {
        return [ordered]@{ state = 'unavailable'; exitCode = $null; errorCode = $null }
    }
    $errorValue = Get-PairedProperty $Record @('errorCode', 'error', 'rootExitErrorCode')
    $errorCode = $null
    if ($null -ne $errorValue) {
        try {
            $errorCode = [Int64]$errorValue
            if ($errorCode -lt 0 -or $errorCode -gt [int]::MaxValue) { $errorCode = $null }
            else { $errorCode = [int]$errorCode }
        }
        catch { $errorCode = $null }
    }
    return [ordered]@{ state = $state; exitCode = $exitCode; errorCode = $errorCode }
}

function Convert-PairedProcessDiagnostics {
    param([AllowNull()] [object]$Raw)
    $empty = New-PairedEmptyStartupDiagnostics
    if ($null -eq $Raw) { return $empty }
    $empty.observationStatus = 'unavailable'
    $rawSchema = Get-PairedProperty $Raw @('schemaVersion')
    try { if ($null -eq $rawSchema -or [int]$rawSchema -ne 1) { return $empty } } catch { return $empty }
    $rawSnapshotsValue = Get-PairedProperty $Raw @('processTreeSnapshots', 'snapshots')
    if ($null -eq $rawSnapshotsValue) { return $empty }
    $rawSnapshots = @($rawSnapshotsValue)
    $snapshotCount = 0
    foreach ($checkpoint in @($script:PairedDiagnosticCheckpointNames)) {
        $target = @($empty.processTreeSnapshots | Where-Object { $_.checkpoint -eq $checkpoint } | Select-Object -First 1)
        if ($target.Count -eq 0) { continue }
        $output = $target[0]
        $source = @($rawSnapshots | Where-Object { [string](Get-PairedProperty $_ @('checkpoint', 'label')) -eq $checkpoint } | Select-Object -First 1)
        if ($source.Count -eq 0) {
            $output.status = 'unavailable'
            continue
        }
        $snapshotCount++
        $source = $source[0]
        $status = [string](Get-PairedProperty $source @('status'))
        if ($status -notin @('observed', 'unavailable', 'not-reached')) { $status = 'unavailable' }
        $output.status = $status
        $output.observed = [bool]($status -eq 'observed')
        $output.elapsedMs = Convert-PairedElapsedMs (Get-PairedProperty $source @('elapsedMs'))
        $exit = Convert-PairedDiagnosticExitState $source
        $output.rootExitState = $exit.state
        $output.rootExitCode = $exit.exitCode
        $output.rootExitErrorCode = $exit.errorCode
        if ($status -eq 'observed' -and $exit.state -eq 'unavailable') { $output.status = 'unavailable'; $output.observed = $false }
        if ($status -eq 'observed') {
            $rawTreeValue = Get-PairedProperty $source @('processTree', 'processes', 'entries')
            $rawTree = if ($null -eq $rawTreeValue) { @() } else { @($rawTreeValue) }
            $tree = New-Object Collections.Generic.List[object]
            $invalid = $false
            foreach ($record in @($rawTree | Select-Object -First $script:PairedDiagnosticMaxProcessCount)) {
                if ($null -eq $record) { $invalid = $true; continue }
                $converted = Convert-PairedProcessDiagnosticRecord $record
                if ($null -eq $converted) { $invalid = $true; continue }
                [void]$tree.Add($converted)
            }
            if ($invalid) { $output.status = 'unavailable'; $output.observed = $false }
            $output.processTree = $tree.ToArray()
            $processCountValue = Convert-PairedDiagnosticPid (Get-PairedProperty $source @('processCount')) $true
            $output.processCount = if ($null -eq $processCountValue) { [int]$tree.Count } else { [int][Math]::Min($processCountValue, $script:PairedDiagnosticMaxProcessCount) }
            $output.processRecordsTruncated = [bool](Get-PairedProperty $source @('processRecordsTruncated')) -or $invalid -or $rawTree.Count -gt $script:PairedDiagnosticMaxProcessCount
            $output.jobMembershipVerified = [bool](Get-PairedProperty $source @('jobMembershipVerified'))
            $jobMemberCount = Convert-PairedDiagnosticPid (Get-PairedProperty $source @('jobMemberCount')) $true
            $output.jobMemberCount = if ($null -eq $jobMemberCount) { [int](@($tree | Where-Object { $_.jobMember -eq $true }).Count) } else { [int][Math]::Min($jobMemberCount, $script:PairedDiagnosticMaxProcessCount) }
            $windowCount = Convert-PairedDiagnosticPid (Get-PairedProperty $source @('topLevelWindowCount')) $true
            $output.topLevelWindowCount = if ($null -eq $windowCount) { $null } else { [int][Math]::Min($windowCount, $script:PairedDiagnosticMaxWindowCount) }
            $output.topLevelWindowCountCapped = [bool](Get-PairedProperty $source @('topLevelWindowCountCapped'))
        }
        else {
            $output.processTree = @()
            $output.processCount = 0
            $output.processRecordsTruncated = $false
            $output.jobMembershipVerified = $false
            $output.jobMemberCount = 0
            $output.topLevelWindowCount = $null
            $output.topLevelWindowCountCapped = $false
        }
        $output.processExitObserved = [bool](Get-PairedProperty $source @('processExitObserved'))
        $output.processExitElapsedMs = Convert-PairedElapsedMs (Get-PairedProperty $source @('processExitElapsedMs'))
        $output.failureStage = [string](Get-PairedProperty $source @('failureStage'))
        $output.failureType = [string](Get-PairedProperty $source @('failureType'))
        if ($output.failureStage -notin @('process-tree', 'window-discovery', 'readiness', 'process-start', 'cleanup', 'profile-cleanup', 'affinity', 'startup', 'diagnostics')) { $output.failureStage = $null }
        if ($output.failureType -notin @('observation', 'timeout', 'startup', 'survivor', 'profileCleanup', 'affinity')) { $output.failureType = $null }
    }
    $rawExit = Get-PairedProperty $Raw @('processExitObservation', 'exitObservation')
    if ($null -eq $rawExit) { return $empty }
    $exitObservedValue = Convert-PairedDiagnosticBoolean (Get-PairedProperty $rawExit @('observed'))
    $exitPid = Convert-PairedDiagnosticPid (Get-PairedProperty $rawExit @('pid', 'processId'))
    $exitElapsed = Convert-PairedElapsedMs (Get-PairedProperty $rawExit @('elapsedMs'))
    $exitState = Convert-PairedDiagnosticExitState $rawExit
    $empty.processExitObservation.state = $exitState.state
    $empty.processExitObservation.exitCode = $exitState.exitCode
    $empty.processExitObservation.errorCode = $exitState.errorCode
    $empty.processExitObservation.observed = [bool]($exitObservedValue -eq $true -and $null -ne $exitPid -and $exitState.state -eq 'exited')
    $empty.processExitObservation.elapsedMs = if ($empty.processExitObservation.observed) { $exitElapsed } else { $null }
    $empty.processExitObservation.pid = if ($empty.processExitObservation.observed) { $exitPid } else { $null }
    $source = [string](Get-PairedProperty $rawExit @('source'))
    $empty.processExitObservation.source = if ($source -eq 'run-root') { 'run-root' } else { 'run-root' }
    if ($snapshotCount -ne $script:PairedDiagnosticCheckpointNames.Count -or
        $empty.processExitObservation.state -eq 'unavailable' -or
        ($exitObservedValue -eq $null)) {
        $empty.observationStatus = 'unavailable'
    }
    elseif (@($empty.processTreeSnapshots | Where-Object { $_.status -eq 'unavailable' }).Count -gt 0) {
        $empty.observationStatus = 'unavailable'
    }
    elseif (@($empty.processTreeSnapshots | Where-Object { $_.status -eq 'observed' }).Count -gt 0) {
        $empty.observationStatus = 'observed'
    }
    else {
        $empty.observationStatus = 'not-reached'
    }
    return $empty
}

function Convert-PairedStartupTraceEvidence {
    param([AllowNull()] [object]$RawTrace)
    $traceBounds = Get-StartupTraceBounds
    if ($null -eq $RawTrace) { return New-PairedEmptyStartupTrace }
    $enabled = Convert-PairedDiagnosticBoolean (Get-PairedProperty $RawTrace @('enabled'))
    $collected = Convert-PairedDiagnosticBoolean (Get-PairedProperty $RawTrace @('collected'))
    if ($enabled -eq $false) { return New-PairedEmptyStartupTrace }
    if ($enabled -ne $true -or $collected -ne $true) { return New-PairedUnavailableStartupTrace }
    $invalidLineCount = Get-PairedProperty $RawTrace @('invalidLineCount')
    try {
        if ($null -ne $invalidLineCount -and [int]$invalidLineCount -gt 0) { return New-PairedUnavailableStartupTrace }
    }
    catch { return New-PairedUnavailableStartupTrace }
    $parseErrors = Get-PairedProperty $RawTrace @('parseErrors')
    if ($null -ne $parseErrors -and @($parseErrors).Count -gt 0) { return New-PairedUnavailableStartupTrace }
    $recordsValue = Get-PairedProperty $RawTrace @('records')
    if ($null -eq $recordsValue) { return New-PairedUnavailableStartupTrace }
    $recordsList = New-Object Collections.Generic.List[object]
    $recordEnumerator = $null
    try {
        if ($recordsValue -is [Collections.ICollection]) {
            $knownRecordCount = [Int64]$recordsValue.Count
            if ($knownRecordCount -lt 1 -or $knownRecordCount -gt [Int64]$traceBounds.maxValidRecords) {
                return New-PairedUnavailableStartupTrace
            }
        }
        if ($recordsValue -is [Collections.IEnumerable] -and
            $recordsValue -isnot [string] -and $recordsValue -isnot [Collections.IDictionary]) {
            $recordEnumerator = $recordsValue.GetEnumerator()
            while ($recordEnumerator.MoveNext()) {
                if ($recordsList.Count -ge [int]$traceBounds.maxValidRecords) {
                    return New-PairedUnavailableStartupTrace
                }
                [void]$recordsList.Add($recordEnumerator.Current)
            }
        }
        else {
            [void]$recordsList.Add($recordsValue)
        }
    }
    catch { return New-PairedUnavailableStartupTrace }
    finally {
        if ($null -ne $recordEnumerator -and $recordEnumerator -is [IDisposable]) { $recordEnumerator.Dispose() }
    }
    if ($recordsList.Count -lt 1 -or $recordsList.Count -gt [int]$traceBounds.maxValidRecords) {
        return New-PairedUnavailableStartupTrace
    }
    $validRecordCountValue = Get-PairedProperty $RawTrace @('validRecordCount')
    $validRecordCount = $null
    try {
        if ($null -eq $validRecordCountValue) { return New-PairedUnavailableStartupTrace }
        $validRecordCount = [Int64]$validRecordCountValue
    }
    catch { return New-PairedUnavailableStartupTrace }
    if ($validRecordCount -lt 1 -or $validRecordCount -gt [Int64]$traceBounds.maxValidRecords -or
        $validRecordCount -ne [Int64]$recordsList.Count) {
        return New-PairedUnavailableStartupTrace
    }
    $counts = @{}
    foreach ($record in $recordsList) {
        if ($null -eq $record) { continue }
        $event = [string](Get-PairedProperty $record @('event'))
        if ($script:PairedStartupTraceEventAllowlist -notcontains $event) { continue }
        $role = [string](Get-PairedProperty $record @('role'))
        if ($script:PairedStartupTraceRoles -notcontains $role) { $role = 'unknown' }
        if (-not $counts.ContainsKey($event)) {
            $counts[$event] = [ordered]@{ count = 0; roles = @{} }
        }
        $counts[$event].count = [int]$counts[$event].count + 1
        if (-not $counts[$event].roles.ContainsKey($role)) { $counts[$event].roles[$role] = 0 }
        $counts[$event].roles[$role] = [int]$counts[$event].roles[$role] + 1
    }
    $eventCounts = New-Object Collections.Generic.List[object]
    foreach ($event in @($script:PairedStartupTraceEventAllowlist)) {
        if (-not $counts.ContainsKey($event)) { continue }
        $roleCounts = [ordered]@{}
        foreach ($role in @($script:PairedStartupTraceRoles)) {
            if ($counts[$event].roles.ContainsKey($role)) { $roleCounts[$role] = [int]$counts[$event].roles[$role] }
        }
        [void]$eventCounts.Add([ordered]@{
            event = $event
            count = [int]$counts[$event].count
            roleCounts = $roleCounts
        })
    }
    return [ordered]@{
        status = 'observed'
        eventCounts = $eventCounts.ToArray()
    }
}

function New-PairedEmptyStartupMilestones {
    return [ordered]@{
        processStarted = $false
        processApiReturnMs = $null
        topLevelWindowObserved = $false
        topLevelHwndMs = $null
        visibleObserved = $false
        visibleMs = $null
        captionObserved = $false
        captionReadyMs = $null
        inputIdleObserved = $false
        inputIdleMs = $null
        documentLayoutObserved = $false
        documentReadyMs = $null
        verticalScrollMaximum = $null
        missingMilestones = @($script:PairedStartupMilestoneNames)
        timeoutStage = $null
        descendantAffinityState = 'not-attempted'
    }
}

function Convert-PairedStartupMilestones {
    param(
        [Parameter(Mandatory = $true)] [object]$Raw,
        [AllowNull()] [string]$FailureType = $null
    )
    $processApiReturnMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('processApiReturnMs'))
    $topLevelHwndMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('topLevelHwndMs'))
    $visibleMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('visibleMs'))
    $captionReadyMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('captionReadyMs'))
    $inputIdleMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('inputIdleMs'))
    $documentReadyMs = Convert-PairedElapsedMs (Get-PairedProperty $Raw @('documentReadyMs'))
    $verticalScrollMaximum = Convert-PairedVerticalScrollMaximum (Get-PairedProperty $Raw @('verticalScrollMaximum'))
    $processStarted = $null -ne $processApiReturnMs
    $topLevelWindowObserved = $null -ne $topLevelHwndMs
    $visibleObserved = $null -ne $visibleMs
    $captionObserved = $null -ne $captionReadyMs
    $inputIdleObserved = [bool](Get-PairedProperty $Raw @('inputIdleReached'))
    $documentLayoutObserved = $null -ne $documentReadyMs

    $missingMilestones = New-Object Collections.Generic.List[string]
    if (-not $processStarted) { [void]$missingMilestones.Add('process-start') }
    if (-not $topLevelWindowObserved) { [void]$missingMilestones.Add('top-level-window') }
    if (-not $visibleObserved) { [void]$missingMilestones.Add('visible') }
    if (-not $captionObserved) { [void]$missingMilestones.Add('caption') }
    if (-not $inputIdleObserved) { [void]$missingMilestones.Add('input-idle') }
    if (-not $documentLayoutObserved) { [void]$missingMilestones.Add('document-layout') }

    $timeoutStage = $null
    if ($FailureType -eq 'timeout') {
        if ($processStarted -and -not $topLevelWindowObserved) {
            $timeoutStage = 'window-discovery'
        }
        elseif ($topLevelWindowObserved -and
            -not ($visibleObserved -and $captionObserved -and $inputIdleObserved -and $documentLayoutObserved)) {
            $timeoutStage = 'readiness'
        }
    }

    $affinity = Get-PairedProperty $Raw @('affinity')
    $requestedMask = [UInt64]0
    $requestedValue = Get-PairedProperty $affinity @('requestedMask')
    if ($null -ne $requestedValue) {
        try { $requestedMask = [UInt64]$requestedValue } catch { $requestedMask = [UInt64]0 }
    }
    $descendantsVerified = [bool](Get-PairedProperty $affinity @('descendantsVerified'))
    # The shared probe attempts descendant affinity only after every readiness milestone; caption implies visibility.
    $allReadinessMilestonesObserved = $topLevelWindowObserved -and $captionObserved -and
        $inputIdleObserved -and $documentLayoutObserved
    $descendantAffinityState = 'not-attempted'
    if ($requestedMask -ne 0) {
        if ($descendantsVerified) { $descendantAffinityState = 'verified' }
        elseif ($allReadinessMilestonesObserved) { $descendantAffinityState = 'failed' }
    }

    return [ordered]@{
        processStarted = [bool]$processStarted
        processApiReturnMs = $processApiReturnMs
        topLevelWindowObserved = [bool]$topLevelWindowObserved
        topLevelHwndMs = $topLevelHwndMs
        visibleObserved = [bool]$visibleObserved
        visibleMs = $visibleMs
        captionObserved = [bool]$captionObserved
        captionReadyMs = $captionReadyMs
        inputIdleObserved = [bool]$inputIdleObserved
        inputIdleMs = $inputIdleMs
        documentLayoutObserved = [bool]$documentLayoutObserved
        documentReadyMs = $documentReadyMs
        verticalScrollMaximum = $verticalScrollMaximum
        missingMilestones = $missingMilestones.ToArray()
        timeoutStage = $timeoutStage
        descendantAffinityState = $descendantAffinityState
    }
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
        [Parameter(Mandatory = $true)] [bool]$ProfileCleanupVerified,
        [bool]$TraceCleanupVerified = $true
    )
    $affinity = Convert-PairedAffinity (Get-PairedProperty $Raw @('affinity'))
    $startupDiagnostics = Convert-PairedProcessDiagnostics (Get-PairedProperty $Raw @('startupDiagnostics'))
    $startupTrace = if (Test-PairedPropertyPresent $Raw 'startupTrace') {
        Convert-PairedStartupTraceEvidence (Get-PairedProperty $Raw @('startupTrace'))
    }
    else { New-PairedEmptyStartupTrace }
    $diagnosticUnavailable = [string](Get-PairedProperty $startupDiagnostics @('observationStatus')) -eq 'unavailable'
    $traceUnavailable = [string](Get-PairedProperty $startupTrace @('status')) -eq 'unavailable'
    $success = [bool](Get-PairedProperty $Raw @('success')) -and [bool](Get-PairedProperty $Raw @('processCleanupVerified')) -and
        $ProfileCleanupVerified -and [bool]$affinity.verified -and [bool]$TraceCleanupVerified -and
        -not $diagnosticUnavailable -and -not $traceUnavailable
    $failureType = if ($success) { $null } else { Get-PairedFailureType $Raw $ProfileCleanupVerified }
    if (-not $success) {
        $cleanupVerified = [bool](Get-PairedProperty $Raw @('processCleanupVerified')) -and $ProfileCleanupVerified
        if ($cleanupVerified -and [bool]$affinity.verified) {
            if ($diagnosticUnavailable) { $failureType = 'diagnostic-unavailable' }
            elseif (-not [bool]$TraceCleanupVerified) { $failureType = 'trace-cleanup' }
            elseif ($traceUnavailable) { $failureType = 'trace-unavailable' }
        }
    }
    $startupMilestones = Convert-PairedStartupMilestones $Raw $failureType
    $metrics = $null
    if ($success) {
        $metrics = [ordered]@{}
        foreach ($metric in $script:PairedMetricNames) {
            $value = Get-PairedProperty $Raw @($metric)
            if ($null -eq $value) { $success = $false; $failureType = 'startup'; break }
            $metrics[$metric] = [double]$value
        }
    }
    $failureStage = if ($success) { $null } else { Get-PairedFailureStage $failureType $startupMilestones }
    return [pscustomobject][ordered]@{
        sequence = [int]$ScheduleRow.sequence
        pairIndex = [int]$ScheduleRow.pairIndex
        slot = [int]$ScheduleRow.slot
        phase = [string]$ScheduleRow.phase
        phaseIndex = [int]$ScheduleRow.phaseIndex
        backend = [string]$ScheduleRow.backend
        status = if ($success) { 'succeeded' } else { [string]$failureType }
        failureStage = $failureStage
        excluded = -not $success
        metrics = $metrics
        startupMilestones = $startupMilestones
        startupDiagnostics = $startupDiagnostics
        startupTrace = $startupTrace
        affinity = $affinity
        profileSha256 = [string]$ProfileDigest.sha256
        profileState = [string]$ProfileDigest.state
        profileFileCount = [int]$ProfileDigest.fileCount
        processCleanupVerified = [bool](Get-PairedProperty $Raw @('processCleanupVerified'))
        profileCleanupVerified = [bool]$ProfileCleanupVerified
        traceCleanupVerified = [bool]$TraceCleanupVerified
        cleanupVerified = [bool](Get-PairedProperty $Raw @('processCleanupVerified')) -and $ProfileCleanupVerified -and [bool]$TraceCleanupVerified
        survivorCount = @(Get-PairedProperty $Raw @('survivors')).Count
    }
}

function Test-PairedRunCleanupVerified {
    param([Parameter(Mandatory = $true)] [object]$Run)
    $traceCleanup = Get-PairedProperty $Run @('traceCleanupVerified')
    return [bool]$Run.processCleanupVerified -and [bool]$Run.profileCleanupVerified -and
        $null -ne $traceCleanup -and [bool]$traceCleanup
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
                startupMilestones = New-PairedEmptyStartupMilestones
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
        [bool]$CollectOnly = $false,
        [string]$PrimaryStage = $null,
        [string]$PrimaryType = $null,
        [string[]]$CleanupCodes = @(),
        [object]$MeasurementArguments = $null,
        [string]$MeasurementCommandSha256 = $null,
        [object]$Integrity = $null
    )
    $allowedTypes = @('preflight', 'integrity', 'launch-failure', 'cleanup-unverified', 'schema', 'write')
    $allowedPrimaryTypes = @($allowedTypes + 'timeout', 'startup', 'affinity', 'survivor', 'profileCleanup', 'diagnostic-unavailable', 'trace-unavailable', 'trace-cleanup')
    $type = if ($allowedTypes -contains $FailureType) { $FailureType } else { 'preflight' }
    $primary = $null
    if (-not [string]::IsNullOrWhiteSpace($PrimaryType)) {
        $primary = [ordered]@{
            stage = if ([string]::IsNullOrWhiteSpace($PrimaryStage)) { 'unknown' } else { $PrimaryStage }
            type = if ($allowedPrimaryTypes -contains $PrimaryType) { $PrimaryType } else { 'preflight' }
        }
    }
    $measurement = [ordered]@{
        argumentsSchemaVersion = if ($null -eq $MeasurementArguments) { $null } else { [int]$MeasurementArguments.schemaVersion }
        commandSha256 = if ([string]::IsNullOrWhiteSpace($MeasurementCommandSha256)) { $null } else { $MeasurementCommandSha256 }
    }
    return [ordered]@{
        schemaVersion = $script:PairedSchemaVersion
        record = 'paired-gui-startup'
        payloadFree = $true
        status = 'failed'
        failure = [ordered]@{
            stage = if ([string]::IsNullOrWhiteSpace($Stage)) { 'unknown' } else { $Stage }
            type = $type
            primary = $primary
            cleanupCodes = @($CleanupCodes)
        }
        provenance = [ordered]@{
            status = 'unverified'
            buildManifestVerified = $false
            roleLabels = 'unverified'
            platform = $Platform
            configuration = $Configuration
            measurement = $measurement
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
        startupMilestones = New-PairedEmptyStartupMilestones
        acceptance = [ordered]@{
            scheduledLaunches = [int][Math]::Max(0, $ScheduledLaunches)
            successfulLaunches = [int][Math]::Max(0, $SuccessfulLaunches)
            startupGatePass = $false
            qualified = $false
        }
        integrity = if ($null -eq $Integrity) {
            [ordered]@{
                sourcePostflightVerified = $false
                scriptPostflightVerified = $false
                reportWriteVerified = $false
            }
        }
        else { $Integrity }
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
        $json = '{"schemaVersion":1,"record":"paired-gui-startup","payloadFree":true,"status":"failed","failure":{"stage":"schema","type":"schema"},"startupMilestones":{"processStarted":false,"processApiReturnMs":null,"topLevelWindowObserved":false,"topLevelHwndMs":null,"visibleObserved":false,"visibleMs":null,"captionObserved":false,"captionReadyMs":null,"inputIdleObserved":false,"inputIdleMs":null,"documentLayoutObserved":false,"documentReadyMs":null,"verticalScrollMaximum":null,"missingMilestones":["process-start","top-level-window","visible","caption","input-idle","document-layout"],"timeoutStage":null,"descendantAffinityState":"not-attempted"},"startupGatePass":false,"adoption":{"decision":"HOLD","adoptionEligible":false}}'
    }
    $encoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $json, $encoding)
    return $true
}

function Invoke-PairedSelfTest {
    [void](Assert-PairedAffinityMask $AffinityMask)
    $emptyTextSha256 = Get-TextSha256 ''
    $nonEmptyTextSha256 = Get-TextSha256 'paired-startup-text-selftest-v1'
    Assert-PairedEqual 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' $emptyTextSha256 'empty text SHA-256'
    Assert-PairedEqual '379861ae5e8cfc84f68ff929d7abec4a9cb2a7844dfe00105610416c62936cdf' $nonEmptyTextSha256 'non-empty text SHA-256'
    $nullTextRejected = $false
    try { [void](Get-TextSha256 $null) } catch { $nullTextRejected = $true }
    if (-not $nullTextRejected) { throw 'Null text SHA-256 input was accepted.' }
    $syntheticSource = New-PairedSourceState ('0' * 40) ''
    if ($syntheticSource.dirty -or $syntheticSource.statusLineCount -ne 0 -or
        $syntheticSource.statusSha256 -ne 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855') {
        throw 'Empty source-state self-test failed.'
    }
    $syntheticSourcePreflight = [ordered]@{
        sourcePreflightCaptured = $true
        sourceState = $syntheticSource
    }
    if (-not $syntheticSourcePreflight.sourcePreflightCaptured -or
        $syntheticSourcePreflight.sourceState.dirty -or
        $syntheticSourcePreflight.sourceState.statusSha256 -ne $emptyTextSha256) {
        throw 'Empty source-state preflight self-test failed.'
    }
    $stageSelfTestRoot = Join-Path $env:TEMP ('paired-runtime-stage-selftest-' + [Guid]::NewGuid().ToString('N'))
    try {
        [void][IO.Directory]::CreateDirectory($stageSelfTestRoot)
        $stageSelfTestExe = Join-Path $stageSelfTestRoot 'sakura.exe'
        [IO.File]::WriteAllBytes($stageSelfTestExe, [byte[]](1, 2, 3, 5, 8))
        $stageSelfTestArtifact = Get-PairedArtifactIdentity $stageSelfTestExe
        $stageSelfTestHash = Get-Sha256 $stageSelfTestExe
        $stageSelfTestReceipt = [ordered]@{
            schema_version = 1
            context_id = 'msvc-x64-debug'
            staging_set_id = 'paired-runtime-stage-selftest'
            files = @([ordered]@{
                    artifact_id = 'sakura-editor-msvc-x64-debug-product'
                    destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'
                    role = 'editor'
                    source = 'x64/Debug/sakura.exe'
                    sha256 = 'sha256:' + $stageSelfTestHash
                    size = [UInt64]5
                })
        }
        $stageSelfTestReceiptPath = Join-Path $stageSelfTestRoot '.sakura-runtime-stage.json'
        $stageSelfTestReceiptText = $stageSelfTestReceipt | ConvertTo-Json -Depth 10
        [IO.File]::WriteAllText($stageSelfTestReceiptPath, $stageSelfTestReceiptText, (New-Object Text.UTF8Encoding($false)))
        $stageSelfTestIdentity = Get-PairedRuntimeStageIdentity $stageSelfTestRoot 'Debug' $stageSelfTestArtifact
        if ($stageSelfTestIdentity.fileCount -ne 1 -or $stageSelfTestIdentity.receiptSha256 -notmatch '^[0-9a-f]{64}$' -or
            $stageSelfTestIdentity.dependencyClosureSha256 -notmatch '^[0-9a-f]{64}$') {
            throw 'Runtime-stage receipt identity self-test failed.'
        }
        $stageSelfTestOriginal = [IO.File]::ReadAllText($stageSelfTestReceiptPath)
        $badStageReceiptCases = @(
            [pscustomobject]@{ destination = 'C:\staging\sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/../sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/wrong/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Release/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe:ads'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'wrong-editor-id' }
        )
        foreach ($badStageCase in $badStageReceiptCases) {
            $badStageReceipt = $stageSelfTestOriginal | ConvertFrom-Json
            $badStageReceipt.files[0].destination = $badStageCase.destination
            $badStageReceipt.files[0].source = $badStageCase.source
            $badStageReceipt.files[0].artifact_id = $badStageCase.artifactId
            [IO.File]::WriteAllText($stageSelfTestReceiptPath, ($badStageReceipt | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
            $badStageRejected = $false
            try { [void](Get-PairedRuntimeStageIdentity $stageSelfTestRoot 'Debug' $stageSelfTestArtifact) } catch { $badStageRejected = $true }
            if (-not $badStageRejected) { throw 'Unsafe paired runtime-stage receipt self-test was accepted.' }
        }
        foreach ($unsafePath in @('foo \bar', 'foo.', 'NUL.dll', 'COM1.txt', 'LPT9')) {
            $unsafePathRejected = $false
            try { [void](Convert-PairedRuntimeReceiptPath $unsafePath 'self-test') } catch { $unsafePathRejected = $true }
            if (-not $unsafePathRejected) { throw 'Unsafe Windows paired runtime receipt path self-test was accepted.' }
        }
        $nestedLanguageRejected = $false
        try {
            Assert-PairedRuntimeReceiptArtifactIdentity 'sakura-language-en-us-resource' 'language-en-us' 'nested\sakura_lang_en_US.dll' 'msvc-x64-debug'
        }
        catch { $nestedLanguageRejected = $true }
        if (-not $nestedLanguageRejected) { throw 'Nested known paired language runtime receipt identity self-test was accepted.' }
        [IO.File]::WriteAllText($stageSelfTestReceiptPath, $stageSelfTestOriginal, (New-Object Text.UTF8Encoding($false)))
    }
    finally {
        if (Test-Path -LiteralPath $stageSelfTestRoot) { [IO.Directory]::Delete($stageSelfTestRoot, $true) }
    }
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
    $selfTestTimeoutProfile = [pscustomobject][ordered]@{ sha256 = ('c' * 64); state = 'missing'; fileCount = 0 }
    $selfTestTimeoutAffinity = [ordered]@{
        requestedMask = [UInt64]1; processMask = [UInt64]1; systemMask = [UInt64]15
        opened = $true; setSucceeded = $true; readBackSucceeded = $true; verified = $true
        descendantsVerified = $false; errorCode = 0
    }
    $selfTestDiagnosticSnapshots = New-Object Collections.Generic.List[object]
    foreach ($checkpoint in @($script:PairedDiagnosticCheckpointNames)) {
        $diagnosticSnapshot = New-PairedEmptyProcessDiagnosticSnapshot $checkpoint
        $diagnosticSnapshot.status = 'observed'
        $diagnosticSnapshot.observed = $true
        $diagnosticSnapshot.elapsedMs = [double]$diagnosticSnapshot.targetMs
        $diagnosticSnapshot.processTree = @([ordered]@{
                pid = 1234; ppid = 0; imageName = 'sakura.exe'; creationTime = [Int64]1; jobMember = $true
            })
        $diagnosticSnapshot.processCount = 1
        $diagnosticSnapshot.jobMembershipVerified = $true
        $diagnosticSnapshot.jobMemberCount = 1
        $diagnosticSnapshot.topLevelWindowCount = 1
        $diagnosticSnapshot.rootExitState = 'active'
        $diagnosticSnapshot.rootExitCode = [UInt32]259
        $diagnosticSnapshot.processExitObserved = $false
        [void]$selfTestDiagnosticSnapshots.Add($diagnosticSnapshot)
    }
    $selfTestStartupDiagnostics = [ordered]@{
        schemaVersion = 1
        processTreeSnapshots = $selfTestDiagnosticSnapshots.ToArray()
        processExitObservation = [ordered]@{
            observed = $false; elapsedMs = $null; pid = $null; source = 'run-root'
            state = 'active'; exitCode = [UInt32]259; errorCode = $null
        }
    }
    $selfTestStartupTrace = [ordered]@{
        enabled = $true
        collected = $true
        validRecordCount = 3
        records = @(
            [ordered]@{ event = 'factory_begin'; role = 'editor'; detail = 'secret'; directory = 'C:\secret' }
            [ordered]@{ event = 'factory_begin'; role = 'control'; detail = 'secret' }
            [ordered]@{ event = 'not-allowlisted'; role = 'control'; detail = 'secret'; path = 'C:\secret' }
        )
    }
    $selfTestWindowTimeoutRaw = [pscustomobject][ordered]@{
        success = $false; processApiReturnMs = 12.25; topLevelHwndMs = $null; visibleMs = $null
        captionReadyMs = $null; inputIdleMs = $null; inputIdleReached = $false
        documentReadyMs = $null; verticalScrollMaximum = $null
        affinity = $selfTestTimeoutAffinity; error = 'Timed out waiting for a run-owned TextEditorWindow.'
        processCleanupVerified = $true; survivors = @()
        startupDiagnostics = $selfTestStartupDiagnostics; startupTrace = $selfTestStartupTrace
    }
    $selfTestWindowTimeoutRun = Convert-PairedLaunchResult $selfTestWindowTimeoutRaw $schedule[0] $selfTestTimeoutProfile $true
    [void](Assert-PairedPayloadFree $selfTestWindowTimeoutRun)
    Assert-PairedEqual 'timeout' $selfTestWindowTimeoutRun.status 'window-discovery timeout status'
    Assert-PairedEqual $true $selfTestWindowTimeoutRun.startupMilestones.processStarted 'window-discovery process milestone'
    Assert-PairedEqual $false $selfTestWindowTimeoutRun.startupMilestones.topLevelWindowObserved 'window-discovery HWND milestone'
    Assert-PairedEqual 'window-discovery' $selfTestWindowTimeoutRun.startupMilestones.timeoutStage 'window-discovery timeout stage'
    Assert-PairedEqual 'not-attempted' $selfTestWindowTimeoutRun.startupMilestones.descendantAffinityState 'window-discovery affinity state'
    Assert-PairedEqual 5 $selfTestWindowTimeoutRun.startupMilestones.missingMilestones.Count 'window-discovery missing milestone count'
    $selfTestReadinessTimeoutRaw = [pscustomobject][ordered]@{
        success = $false; processApiReturnMs = 10.5; topLevelHwndMs = 18.75; visibleMs = 23.0
        captionReadyMs = 31.5; inputIdleMs = $null; inputIdleReached = $false
        documentReadyMs = $null; verticalScrollMaximum = 42
        affinity = $selfTestTimeoutAffinity; error = 'Timed out waiting for startup milestones.'
        processCleanupVerified = $true; survivors = @()
    }
    $selfTestReadinessTimeoutRun = Convert-PairedLaunchResult $selfTestReadinessTimeoutRaw $schedule[1] $selfTestTimeoutProfile $true
    [void](Assert-PairedPayloadFree $selfTestReadinessTimeoutRun)
    Assert-PairedEqual 'timeout' $selfTestReadinessTimeoutRun.status 'readiness timeout status'
    Assert-PairedEqual $true $selfTestReadinessTimeoutRun.startupMilestones.visibleObserved 'readiness visible milestone'
    Assert-PairedEqual $true $selfTestReadinessTimeoutRun.startupMilestones.captionObserved 'readiness caption milestone'
    Assert-PairedEqual $false $selfTestReadinessTimeoutRun.startupMilestones.inputIdleObserved 'readiness input-idle milestone'
    Assert-PairedEqual 42 $selfTestReadinessTimeoutRun.startupMilestones.verticalScrollMaximum 'readiness scrollbar maximum'
    Assert-PairedEqual 'readiness' $selfTestReadinessTimeoutRun.startupMilestones.timeoutStage 'readiness timeout stage'
    Assert-PairedEqual 'not-attempted' $selfTestReadinessTimeoutRun.startupMilestones.descendantAffinityState 'readiness affinity state'
    Assert-PairedEqual 2 $selfTestReadinessTimeoutRun.startupMilestones.missingMilestones.Count 'readiness missing milestone count'
    $selfTestLayoutBeforeIdleRaw = [pscustomobject][ordered]@{
        success = $false; processApiReturnMs = 10.5; topLevelHwndMs = 18.75; visibleMs = 23.0
        captionReadyMs = 31.5; inputIdleMs = $null; inputIdleReached = $false
        documentReadyMs = 36.0; verticalScrollMaximum = 100
        affinity = $selfTestTimeoutAffinity; error = 'Timed out waiting for startup milestones.'
        processCleanupVerified = $true; survivors = @()
    }
    $selfTestLayoutBeforeIdleRun = Convert-PairedLaunchResult $selfTestLayoutBeforeIdleRaw $schedule[1] $selfTestTimeoutProfile $true
    [void](Assert-PairedPayloadFree $selfTestLayoutBeforeIdleRun)
    Assert-PairedEqual $true $selfTestLayoutBeforeIdleRun.startupMilestones.documentLayoutObserved 'layout-before-idle document milestone'
    Assert-PairedEqual $false $selfTestLayoutBeforeIdleRun.startupMilestones.inputIdleObserved 'layout-before-idle input-idle milestone'
    Assert-PairedEqual 'not-attempted' $selfTestLayoutBeforeIdleRun.startupMilestones.descendantAffinityState 'layout-before-idle affinity state'
    $selfTestDescendantAffinityFailureRaw = [pscustomobject][ordered]@{
        success = $false; processApiReturnMs = 10.5; topLevelHwndMs = 18.75; visibleMs = 23.0
        captionReadyMs = 31.5; inputIdleMs = 34.0; inputIdleReached = $true
        documentReadyMs = 36.0; verticalScrollMaximum = 100
        affinity = $selfTestTimeoutAffinity; error = 'Descendant affinity verification failed.'
        processCleanupVerified = $true; survivors = @()
    }
    $selfTestDescendantAffinityFailureRun = Convert-PairedLaunchResult $selfTestDescendantAffinityFailureRaw $schedule[1] $selfTestTimeoutProfile $true
    [void](Assert-PairedPayloadFree $selfTestDescendantAffinityFailureRun)
    Assert-PairedEqual 'failed' $selfTestDescendantAffinityFailureRun.startupMilestones.descendantAffinityState 'completed-readiness affinity state'
    $selfTestFailedRun = New-PairedFailedRun $schedule[2] 'integrity' $true $true
    [void](Assert-PairedPayloadFree $selfTestFailedRun)
    if ($selfTestFailedRun.startupMilestones.processStarted -or
        $selfTestFailedRun.startupMilestones.topLevelWindowObserved -or
        $selfTestFailedRun.startupMilestones.visibleObserved -or
        $selfTestFailedRun.startupMilestones.captionObserved -or
        $selfTestFailedRun.startupMilestones.inputIdleObserved -or
        $selfTestFailedRun.startupMilestones.documentLayoutObserved -or
        $null -ne $selfTestFailedRun.startupMilestones.processApiReturnMs -or
        $null -ne $selfTestFailedRun.startupMilestones.topLevelHwndMs -or
        $null -ne $selfTestFailedRun.startupMilestones.visibleMs -or
        $null -ne $selfTestFailedRun.startupMilestones.captionReadyMs -or
        $null -ne $selfTestFailedRun.startupMilestones.inputIdleMs -or
        $null -ne $selfTestFailedRun.startupMilestones.documentReadyMs -or
        $null -ne $selfTestFailedRun.startupMilestones.verticalScrollMaximum) {
        throw 'Synthetic failed-run startup milestone schema self-test failed.'
    }
    $selfTestSuccessRaw = [pscustomobject][ordered]@{
        success = $true; processApiReturnMs = 9.5; topLevelHwndMs = 14.25; visibleMs = 20.0
        captionReadyMs = 24.75; inputIdleMs = 28.5; inputIdleReached = $true
        documentReadyMs = 33.0; verticalScrollMaximum = 100
        affinity = [ordered]@{
            requestedMask = [UInt64]1; processMask = [UInt64]1; systemMask = [UInt64]15
            opened = $true; setSucceeded = $true; readBackSucceeded = $true; verified = $true
            descendantsVerified = $true; errorCode = 0
        }
        error = $null; processCleanupVerified = $true; survivors = @()
        startupDiagnostics = $selfTestStartupDiagnostics; startupTrace = $selfTestStartupTrace
    }
    $selfTestSuccessRun = Convert-PairedLaunchResult $selfTestSuccessRaw $schedule[2] $selfTestTimeoutProfile $true
    [void](Assert-PairedPayloadFree $selfTestSuccessRun)
    if ($selfTestSuccessRun.status -ne 'succeeded' -or
        -not $selfTestSuccessRun.startupMilestones.processStarted -or
        -not $selfTestSuccessRun.startupMilestones.topLevelWindowObserved -or
        -not $selfTestSuccessRun.startupMilestones.visibleObserved -or
        -not $selfTestSuccessRun.startupMilestones.captionObserved -or
        -not $selfTestSuccessRun.startupMilestones.inputIdleObserved -or
        -not $selfTestSuccessRun.startupMilestones.documentLayoutObserved -or
        $selfTestSuccessRun.startupMilestones.timeoutStage -ne $null -or
        $selfTestSuccessRun.startupMilestones.descendantAffinityState -ne 'verified' -or
        $selfTestSuccessRun.startupMilestones.verticalScrollMaximum -ne 100) {
        throw 'Synthetic successful startup milestone schema self-test failed.'
    }
    $selfTestDiagnosticsSuccessVerified = [bool]($selfTestSuccessRun.startupDiagnostics.observationStatus -eq 'observed' -and
        $selfTestSuccessRun.startupDiagnostics.processTreeSnapshots.Count -eq 4 -and
        $selfTestSuccessRun.startupDiagnostics.processTreeSnapshots[0].rootExitState -eq 'active' -and
        $selfTestSuccessRun.startupDiagnostics.processTreeSnapshots[0].rootExitCode -eq 259 -and
        $selfTestSuccessRun.startupDiagnostics.processExitObservation.state -eq 'active' -and
        $selfTestSuccessRun.startupDiagnostics.processExitObservation.exitCode -eq 259)
    if (-not $selfTestDiagnosticsSuccessVerified) { throw 'Synthetic successful startup diagnostics schema self-test failed.' }
    $selfTestDiagnosticsTimeoutVerified = [bool]($selfTestWindowTimeoutRun.status -eq 'timeout' -and
        $selfTestWindowTimeoutRun.startupDiagnostics.observationStatus -eq 'observed' -and
        $selfTestWindowTimeoutRun.failureStage -eq 'window-discovery')
    if (-not $selfTestDiagnosticsTimeoutVerified) { throw 'Synthetic timeout startup diagnostics schema self-test failed.' }
    $traceEventCounts = @($selfTestSuccessRun.startupTrace.eventCounts)
    $traceJson = $selfTestSuccessRun.startupTrace | ConvertTo-Json -Depth 10 -Compress
    $traceAllowlistRecord = @($traceEventCounts | Where-Object { $_.event -eq 'factory_begin' } | Select-Object -First 1)
    $selfTestTraceAllowlistVerified = [bool]($selfTestSuccessRun.startupTrace.status -eq 'observed' -and
        $traceEventCounts.Count -eq 1 -and $traceAllowlistRecord.Count -eq 1 -and
        $traceAllowlistRecord[0].count -eq 2 -and
        $traceAllowlistRecord[0].roleCounts.editor -eq 1 -and
        $traceAllowlistRecord[0].roleCounts.control -eq 1 -and
        $traceJson -notmatch '(?i)secret|detail|directory|path|not-allowlisted')
    if (-not $selfTestTraceAllowlistVerified) { throw 'Startup trace allowlist self-test failed.' }
    $traceBoundsSelfTest = Get-StartupTraceBounds
    if ($traceBoundsSelfTest.maxFiles -ne 8 -or $traceBoundsSelfTest.maxBytes -ne 1048576 -or
        $traceBoundsSelfTest.maxLines -ne 4096 -or $traceBoundsSelfTest.maxValidRecords -ne 4096 -or
        $traceBoundsSelfTest.maxLineLength -ne 65536) {
        throw 'Paired startup trace bounds contract self-test failed.'
    }
    $selfTestEmptyTrace = Convert-PairedStartupTraceEvidence ([ordered]@{
        enabled = $true; collected = $true; validRecordCount = 0; records = @()
    })
    $selfTestEmptyTraceWithCount = Convert-PairedStartupTraceEvidence ([ordered]@{
        enabled = $true; collected = $true; validRecordCount = 1; records = @()
    })
    $overLimitRecordArray = [Array]::CreateInstance([object], [int]($traceBoundsSelfTest.maxValidRecords + 1))
    $selfTestOverLimitTrace = Convert-PairedStartupTraceEvidence ([ordered]@{
        enabled = $true; collected = $true; validRecordCount = $traceBoundsSelfTest.maxValidRecords + 1; records = $overLimitRecordArray
    })
    $selfTestTraceEmptyVerified = [bool]($selfTestEmptyTrace.status -eq 'unavailable' -and
        $selfTestEmptyTraceWithCount.status -eq 'unavailable' -and
        $selfTestOverLimitTrace.status -eq 'unavailable')
    if (-not $selfTestTraceEmptyVerified) { throw 'Empty or over-limit startup trace self-test was accepted.' }
    $selfTestFallbackDiagnosticsVerified = [bool]($selfTestFailedRun.startupDiagnostics.schemaVersion -eq 1 -and
        $selfTestFailedRun.startupDiagnostics.observationStatus -eq 'not-attempted' -and
        $selfTestFailedRun.startupDiagnostics.processTreeSnapshots.Count -eq 4 -and
        $selfTestFailedRun.startupTrace.status -eq 'not-attempted' -and
        $selfTestFailedRun.failureStage -eq 'integrity')
    if (-not $selfTestFallbackDiagnosticsVerified) { throw 'Synthetic fallback startup diagnostics schema self-test failed.' }
    $unverifiedTraceCleanupRun = [pscustomobject]@{
        processCleanupVerified = $true; profileCleanupVerified = $true; traceCleanupVerified = $false
    }
    $selfTestCleanupTerminalVerified = [bool]((Test-PairedRunCleanupVerified $selfTestSuccessRun) -and
        (Test-PairedRunCleanupVerified $selfTestFailedRun) -and
        -not (Test-PairedRunCleanupVerified $unverifiedTraceCleanupRun))
    if (-not $selfTestCleanupTerminalVerified) { throw 'Startup trace cleanup terminal self-test failed.' }
    $selfTestStartupMilestonesVerified = [bool]($selfTestWindowTimeoutRun.startupMilestones.processStarted -and
        -not $selfTestWindowTimeoutRun.startupMilestones.topLevelWindowObserved -and
        $selfTestWindowTimeoutRun.startupMilestones.timeoutStage -eq 'window-discovery' -and
        $selfTestReadinessTimeoutRun.startupMilestones.topLevelWindowObserved -and
        $selfTestReadinessTimeoutRun.startupMilestones.timeoutStage -eq 'readiness')
    if (-not $selfTestStartupMilestonesVerified) { throw 'Synthetic startup milestone timeout self-test failed.' }
    $selfTestDescendantStatesVerified = [bool]($selfTestLayoutBeforeIdleRun.startupMilestones.descendantAffinityState -eq 'not-attempted' -and
        $selfTestDescendantAffinityFailureRun.startupMilestones.descendantAffinityState -eq 'failed')
    if (-not $selfTestDescendantStatesVerified) { throw 'Synthetic descendant affinity state self-test failed.' }
    $terminatedRun = $selfTestWindowTimeoutRun
    $reportTermination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries 1 -TriggerRow $schedule[0] -TriggerRun $terminatedRun
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
    $selfTestMeasurementArguments = New-PairedMeasurementArguments 'cpp' 'x64' 'Debug' 5 30 1 $false
    $selfTestMeasurementCommandSha256 = Get-PairedMeasurementCommandSha256 $selfTestMeasurementArguments
    if ($selfTestMeasurementCommandSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'Measurement command identity self-test failed.'
    }
    $selfTestIntegrity = [ordered]@{
        sourcePreflightCaptured = $true
        sourcePostflightVerified = $true
        scriptPreflightCaptured = $true
        scriptPostflightVerified = $true
        reportWriteVerified = $true
        sourceState = [ordered]@{
            head = ('0' * 40); dirty = $false; statusSha256 = ('a' * 64); statusLineCount = 0
        }
        scripts = [ordered]@{
            pairedRunnerSha256 = ('f' * 64); sharedStartupImplementationSha256 = ('e' * 64)
        }
    }
    $manifestSelfTestRoot = Join-Path $env:TEMP ('paired-build-manifest-selftest-' + [Guid]::NewGuid().ToString('N'))
    $manifestSelectorValidCases = [ordered]@{}
    $manifestSelectorRejectedCases = [ordered]@{}
    try {
        [void][IO.Directory]::CreateDirectory($manifestSelfTestRoot)
        $manifestSelfTestPath = Join-Path $manifestSelfTestRoot 'build-manifest.json'
        $manifestSelfTestExpectedSource = [pscustomobject]@{
            head = ('0' * 40); dirty = $false; statusSha256 = ('a' * 64); statusLineCount = 0
        }
        $manifestSelfTestExpectedArtifact = [pscustomobject]@{ sha256 = ('b' * 64); sizeBytes = 1 }
        $manifestSelfTestSelectorSymbols = @($script:PairedRustOutputProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
        $newManifestSelfTest = {
            param([string]$SyntheticBackend, [string]$SyntheticConfiguration)
            $release = [StringComparer]::OrdinalIgnoreCase.Equals($SyntheticConfiguration, 'Release')
            $syntheticResult = if ($release) { 'msvc-ltcg-compile-selector-verified' } else { 'dumpbin-unresolved-refs-verified' }
            $syntheticMethod = if ($release) { 'msvc-ltcg-compile-selector' } else { 'dumpbin-object-undefined' }
            $syntheticFormat = if ($release) { 'msvc-ltcg-anonymous' } else { 'coff-symbols' }
            $syntheticObjectHash = if ($SyntheticBackend -eq 'cpp') { ('1' * 64) } else { ('a' * 64) }
            $syntheticObjectSize = if ($release) { [UInt64]2 } else { [UInt64]1 }
            [string[]]$syntheticSelectorSymbols = @()
            if (-not $release -and $SyntheticBackend -eq 'rust') { $syntheticSelectorSymbols = @($manifestSelfTestSelectorSymbols) }
            $syntheticCompileBeforeExists = [bool]$release
            $syntheticCompileAfterExists = [bool]$release
            $syntheticCompileBeforeHash = if ($syntheticCompileBeforeExists) { ('3' * 64) } else { $null }
            $syntheticCompileAfterHash = if ($syntheticCompileAfterExists) { ('4' * 64) } else { $null }
            $syntheticCompileBeforeSize = if ($syntheticCompileBeforeExists) { [UInt64]3 } else { [UInt64]0 }
            $syntheticCompileAfterSize = if ($syntheticCompileAfterExists) { [UInt64]4 } else { [UInt64]0 }
            $syntheticCompileHasGl = [bool]$release
            $syntheticCompileSelectorCount = if ($release -and $SyntheticBackend -eq 'rust') { 1 } else { 0 }
            $syntheticDefinedSymbols = @($manifestSelfTestSelectorSymbols)
            $syntheticArchiveHash = if ($SyntheticBackend -eq 'cpp') { ('2' * 64) } else { ('b' * 64) }
            $syntheticBaseCanonical = if ($release) {
                $syntheticCompileBeforeCanonical = if ($syntheticCompileBeforeExists) { $syntheticCompileBeforeHash } else { 'missing' }
                'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|method={2}|symbols=|object-after={3}|object-format={4}|compile-log-before={5}|compile-log-before-size={6}|compile-log-after={7}|compile-log-after-size={8}|compile-gl={9}|compile-rust-selector-count={10}' -f
                    $SyntheticBackend, $syntheticResult, $syntheticMethod, $syntheticObjectHash, $syntheticFormat,
                    $syntheticCompileBeforeCanonical, $syntheticCompileBeforeSize, $syntheticCompileAfterHash,
                    $syntheticCompileAfterSize, $syntheticCompileHasGl, $syntheticCompileSelectorCount
            }
            else {
                'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|symbols={2}|object-after={3}' -f
                    $SyntheticBackend, $syntheticResult, ($syntheticSelectorSymbols -join ','), $syntheticObjectHash
            }
            $syntheticArchiveCanonical = '{0}|archive-result=dumpbin-defined-exports-verified|archive={1}|defined={2}' -f
                (Get-TextSha256 $syntheticBaseCanonical), $syntheticArchiveHash, ($syntheticDefinedSymbols -join ',')
            $syntheticSelectorHash = Get-TextSha256 $syntheticArchiveCanonical
            $syntheticSelectorProof = [ordered]@{
                result = $syntheticResult; outputBackend = $SyntheticBackend; utf16Backend = 'cpp'
                outputProductionPackage = $false; utf16ProductionPackage = $false
                utf16BenchmarkTelemetry = $false; assemblyListings = $false
                verificationMethod = $syntheticMethod; providerObjectFormat = $syntheticFormat
                providerObjectSha256Before = $null; providerObjectSha256After = $syntheticObjectHash
                providerObjectSizeBytesAfter = $syntheticObjectSize
                unresolvedProviderSymbols = $syntheticSelectorSymbols
                unresolvedProviderSymbolCount = $syntheticSelectorSymbols.Count
                compileLogExistsBefore = $syntheticCompileBeforeExists
                compileLogExistsAfter = $syntheticCompileAfterExists
                compileLogSha256Before = $syntheticCompileBeforeHash
                compileLogSizeBytesBefore = $syntheticCompileBeforeSize
                compileLogSha256After = $syntheticCompileAfterHash
                compileLogSizeBytesAfter = $syntheticCompileAfterSize
                compileLogProof = $syntheticCompileHasGl
                compileCommandHasGl = $syntheticCompileHasGl
                compileCommandRustSelectorDefineCount = $syntheticCompileSelectorCount
                rustArchiveResult = 'dumpbin-defined-exports-verified'
                rustArchiveSha256 = $syntheticArchiveHash
                rustArchiveSizeBytes = [UInt64]5
                definedProviderSymbols = $syntheticDefinedSymbols
                definedProviderSymbolCount = $syntheticDefinedSymbols.Count
                selectorContractSha256 = $syntheticSelectorHash
            }
            return [ordered]@{
                schemaVersion = 1; record = 'output-startup-build-manifest'; payloadFree = $true; status = 'committed'
                backend = $SyntheticBackend; platform = 'x64'; configuration = $SyntheticConfiguration
                sourceHead = ('0' * 40); sourceDirty = $false; sourceStatusSha256 = ('a' * 64); sourceStatusLineCount = 0
                outputBackend = $SyntheticBackend; utf16Backend = 'cpp'; outputProductionPackage = $false; utf16ProductionPackage = $false
                exeSha256 = ('b' * 64); dependencyClosureSha256 = ('c' * 64); runtimeStageReceiptSha256 = ('d' * 64)
                windowsImageIdentity = 'windows-selftest'; windowsImageSha256 = ('e' * 64)
                powerMode = 'Balanced'; powerModeSha256 = ('f' * 64); buildParallelism = 1
                msvcIdentity = 'msvc-selftest'; rustToolchain = 'rust-selftest'; rustLockSha256 = ('a' * 64)
                packagePlanSha256 = ('b' * 64); buildCommandSha256 = ('c' * 64)
                packagePlanCommandSha256 = ('d' * 64); runtimeStageCommandSha256 = ('e' * 64)
                canonicalRuntimeStage = $true
                selectorProof = $syntheticSelectorProof; selectorProofSha256 = $syntheticSelectorHash
                providerObjectSha256Before = $null; providerObjectSha256After = $syntheticObjectHash
                providerObjectFormat = $syntheticFormat; verificationMethod = $syntheticMethod
                compileLogExistsBefore = $syntheticCompileBeforeExists; compileLogExistsAfter = $syntheticCompileAfterExists
                compileLogSha256Before = $syntheticCompileBeforeHash; compileLogSizeBytesBefore = $syntheticCompileBeforeSize
                compileLogSha256After = $syntheticCompileAfterHash; compileLogSizeBytesAfter = $syntheticCompileAfterSize
                compileLogProof = $syntheticCompileHasGl; compileCommandHasGl = $syntheticCompileHasGl
                compileCommandRustSelectorDefineCount = $syntheticCompileSelectorCount
                transaction = [ordered]@{
                    status = 'committed'; artifactBeforeVerified = $true; artifactAfterVerified = $true
                    runtimeStageVerified = $true; manifestGeneratedByProducer = $true; publication = 'atomic-directory-rename'
                }
            }
        }
        $writeManifestSelfTest = {
            param([object]$SyntheticManifest)
            [IO.File]::WriteAllText($manifestSelfTestPath, ($SyntheticManifest | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
        }
        $cloneManifestSelfTest = {
            param([object]$SyntheticManifest)
            return ($SyntheticManifest | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
        }
        $assertManifestSelfTestRejected = {
            param([string]$Label, [object]$SyntheticManifest, [string]$SyntheticBackend, [string]$SyntheticConfiguration)
            & $writeManifestSelfTest $SyntheticManifest
            $rejected = $false
            try {
                [void](Get-PairedBuildManifest $manifestSelfTestPath $SyntheticBackend 'x64' $SyntheticConfiguration $manifestSelfTestExpectedSource $manifestSelfTestExpectedArtifact)
            }
            catch { $rejected = $true }
            if (-not $rejected) { throw "Manifest selector self-test accepted $Label." }
            return $true
        }
        foreach ($case in @(
                [pscustomobject]@{ backend = 'cpp'; configuration = 'Debug'; label = 'valid Debug C++' }
                [pscustomobject]@{ backend = 'rust'; configuration = 'Debug'; label = 'valid Debug Rust' }
                [pscustomobject]@{ backend = 'cpp'; configuration = 'Release'; label = 'valid Release C++' }
                [pscustomobject]@{ backend = 'rust'; configuration = 'Release'; label = 'valid Release Rust' }
            )) {
            $fixture = & $newManifestSelfTest $case.backend $case.configuration
            & $writeManifestSelfTest $fixture
            $validated = Get-PairedBuildManifest $manifestSelfTestPath $case.backend 'x64' $case.configuration $manifestSelfTestExpectedSource $manifestSelfTestExpectedArtifact
            $manifestSelectorValidCases[('{0}-{1}' -f $case.configuration, $case.backend)] = $true
            Assert-PairedEqual $case.backend $validated.backend ($case.label + ' backend')
            Assert-PairedEqual $case.configuration $validated.configuration ($case.label + ' configuration')
            $expectedResult = if ($case.configuration -eq 'Release') { 'msvc-ltcg-compile-selector-verified' } else { 'dumpbin-unresolved-refs-verified' }
            Assert-PairedEqual $expectedResult $validated.selectorProofResult ($case.label + ' result')
        }
        $manifestSelfTest = & $newManifestSelfTest 'cpp' 'Debug'
        & $writeManifestSelfTest $manifestSelfTest
        $manifestSelfTestResult = Get-PairedBuildManifest $manifestSelfTestPath 'cpp' 'x64' 'Debug' $manifestSelfTestExpectedSource $manifestSelfTestExpectedArtifact
        Assert-PairedEqual 'output-startup-build-manifest' $manifestSelfTestResult.record 'producer manifest record'
        Assert-PairedEqual $true $manifestSelfTestResult.manifestGeneratedByProducer 'producer manifest transaction marker'
        Assert-PairedEqual 'atomic-directory-rename' $manifestSelfTestResult.transactionPublication 'producer manifest publication marker'
        Assert-PairedEqual 'dumpbin-unresolved-refs-verified' $manifestSelfTestResult.selectorProofResult 'selector proof result'
        Assert-PairedEqual 0 $manifestSelfTestResult.selectorProofUnresolvedProviderSymbolCount 'C++ selector proof symbol count'
        $manifestSelfTest.status = 'unverified'
        & $writeManifestSelfTest $manifestSelfTest
        if (-not (& $assertManifestSelfTestRejected 'uncommitted producer manifest' $manifestSelfTest 'cpp' 'Debug')) { throw 'Uncommitted producer manifest rejection self-test failed.' }
        $manifestSelfTest.status = 'committed'
        $manifestSelfTest.sourceDirty = $true
        $manifestSelfTestExpectedSource.dirty = $true
        & $writeManifestSelfTest $manifestSelfTest
        $dirtyManifestRejected = & $assertManifestSelfTestRejected 'dirty producer manifest' $manifestSelfTest 'cpp' 'Debug'
        if (-not $dirtyManifestRejected) { throw 'Dirty producer manifest rejection self-test failed.' }
        $manifestSelfTest.sourceDirty = $false
        $manifestSelfTestExpectedSource.dirty = $false

        $badResultManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Release')
        $badResultManifest.selectorProof.result = 'dumpbin-unresolved-refs-verified'
        $manifestSelectorRejectedCases.wrongResult = & $assertManifestSelfTestRejected 'wrong configuration result' $badResultManifest 'cpp' 'Release'
        $badConfigurationManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Debug')
        $badConfigurationManifest.configuration = 'Release'
        $manifestSelectorRejectedCases.wrongConfiguration = & $assertManifestSelfTestRejected 'wrong manifest configuration' $badConfigurationManifest 'cpp' 'Release'
        $badCountManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'rust' 'Release')
        $badCountManifest.selectorProof.compileCommandRustSelectorDefineCount = 0
        $manifestSelectorRejectedCases.wrongSelectorCount = & $assertManifestSelfTestRejected 'wrong Release Rust selector count' $badCountManifest 'rust' 'Release'
        $badCompileLogManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Release')
        $badCompileLogManifest.selectorProof.compileLogExistsAfter = $false
        $badCompileLogManifest.selectorProof.compileLogSha256After = $null
        $badCompileLogManifest.selectorProof.compileLogSizeBytesAfter = 0
        $badCompileLogManifest.compileLogExistsAfter = $false
        $badCompileLogManifest.compileLogSha256After = $null
        $badCompileLogManifest.compileLogSizeBytesAfter = 0
        $manifestSelectorRejectedCases.compileLog = & $assertManifestSelfTestRejected 'missing Release compile log' $badCompileLogManifest 'cpp' 'Release'
        $badArchiveHashManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Debug')
        $badArchiveHashManifest.selectorProof.rustArchiveSha256 = 'c' * 64
        $manifestSelectorRejectedCases.archiveHash = & $assertManifestSelfTestRejected 'tampered archive hash' $badArchiveHashManifest 'cpp' 'Debug'
        $badSymbolManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'rust' 'Debug')
        $badSymbolManifest.selectorProof.unresolvedProviderSymbols = @($manifestSelfTestSelectorSymbols | Select-Object -Skip 1)
        $badSymbolManifest.selectorProof.unresolvedProviderSymbolCount = 6
        $manifestSelectorRejectedCases.symbol = & $assertManifestSelfTestRejected 'tampered unresolved symbol set' $badSymbolManifest 'rust' 'Debug'
        $badProofHashManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Debug')
        $badProofHashManifest.selectorProof.selectorContractSha256 = 'd' * 64
        $manifestSelectorRejectedCases.proofHash = & $assertManifestSelfTestRejected 'tampered selector contract hash' $badProofHashManifest 'cpp' 'Debug'
        $badMirrorManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'rust' 'Release')
        $badMirrorManifest.providerObjectFormat = 'coff-symbols'
        $manifestSelectorRejectedCases.topLevelMirror = & $assertManifestSelfTestRejected 'tampered top-level selector mirror' $badMirrorManifest 'rust' 'Release'
        $badTopLevelHashManifest = & $cloneManifestSelfTest (& $newManifestSelfTest 'cpp' 'Debug')
        $badTopLevelHashManifest.selectorProofSha256 = 'e' * 64
        $manifestSelectorRejectedCases.topLevelHash = & $assertManifestSelfTestRejected 'tampered top-level selector hash' $badTopLevelHashManifest 'cpp' 'Debug'
    }
    finally {
        if (Test-Path -LiteralPath $manifestSelfTestRoot) { [IO.Directory]::Delete($manifestSelfTestRoot, $true) }
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
        -ArtifactBundles $selfTestBundles -CollectOnly $false -Provenance $selfTestProvenance -SampleCopy $selfTestSampleCopy `
        -Integrity $selfTestIntegrity -MeasurementArguments $selfTestMeasurementArguments `
        -MeasurementCommandSha256 $selfTestMeasurementCommandSha256
    [void](Assert-PairedPayloadFree $terminatedReport)
    Assert-PairedEqual $false $terminatedReport.acceptance.qualified 'terminated report qualification'
    Assert-PairedEqual $false $terminatedReport.pass 'terminated report pass'
    Assert-PairedEqual $true $terminatedReport.acceptance.campaignTerminated 'terminated report campaign state'
    Assert-PairedEqual 'failed' $terminatedReport.status 'terminated report status'
    Assert-PairedEqual 'timeout' $terminatedReport.termination.failureType 'terminated report failure type'
    Assert-PairedEqual ($schedule.Count - 1) $terminatedReport.acceptance.suppressedLaunches 'terminated report acceptance suppression'
    Assert-PairedEqual ($schedule.Count - 1) $terminatedReport.termination.suppressedLaunches 'terminated report termination suppression'
    $cleanupEvidence = New-PairedFailureEvidence -Stage 'cleanup' -FailureType 'cleanup-unverified' `
        -PrimaryStage 'launch' -PrimaryType 'timeout' -CleanupCodes @('bundle-cleanup', 'sample-cleanup') `
        -MeasurementArguments $selfTestMeasurementArguments -MeasurementCommandSha256 $selfTestMeasurementCommandSha256 `
        -Integrity $selfTestIntegrity
    [void](Assert-PairedPayloadFree $cleanupEvidence)
    Assert-PairedEqual 'cleanup-unverified' $cleanupEvidence.failure.type 'cleanup failure priority'
    Assert-PairedEqual 'timeout' $cleanupEvidence.failure.primary.type 'cleanup original cause'
    Assert-PairedEqual 2 $cleanupEvidence.failure.cleanupCodes.Count 'cleanup failure code count'

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
    $measurementFunctionText = (Get-Command Invoke-PairedMeasurement -CommandType Function).ScriptBlock.ToString()
    $postWriteReportRecheckVerified = $measurementFunctionText.Contains("Assert-PairedSourceStateUnchanged `$sourceState 'Post-write report'") -and
        $measurementFunctionText.Contains("Assert-PairedScriptIdentityUnchanged `$scriptIdentity 'Post-write report'") -and
        $measurementFunctionText.Contains('[IO.File]::Move($reportTempPath, $reportPath)')
    if (-not $postWriteReportRecheckVerified) { throw 'Post-write report integrity self-test failed.' }

    return [ordered]@{
        selfTest = $true
        passed = $true
        noGuiLaunch = $true
        schemaVersion = $script:PairedSchemaVersion
        scheduleEntries = $schedule.Count
        scheduleHash = $scheduleHash
        measurementArgumentsSchemaVersion = [int]$selfTestMeasurementArguments.schemaVersion
        measurementCommandSha256 = [string]$selfTestMeasurementCommandSha256
        textSha256EmptyVerified = [bool]($emptyTextSha256 -eq 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855')
        textSha256NonEmptyVerified = [bool]($nonEmptyTextSha256 -eq '379861ae5e8cfc84f68ff929d7abec4a9cb2a7844dfe00105610416c62936cdf')
        textSha256NullRejected = [bool]$nullTextRejected
        sourcePreflightSyntheticVerified = [bool]($syntheticSourcePreflight.sourcePreflightCaptured -and
            -not $syntheticSourcePreflight.sourceState.dirty -and
            $syntheticSourcePreflight.sourceState.statusLineCount -eq 0 -and
            $syntheticSourcePreflight.sourceState.statusSha256 -eq $emptyTextSha256)
        manifestProducerContractVerified = $true
        manifestSelectorProofVerified = [bool]($manifestSelectorValidCases.Count -eq 4 -and $manifestSelectorRejectedCases.Count -eq 9)
        manifestSelectorValidDebugCpp = [bool]$manifestSelectorValidCases['Debug-cpp']
        manifestSelectorValidDebugRust = [bool]$manifestSelectorValidCases['Debug-rust']
        manifestSelectorValidReleaseCpp = [bool]$manifestSelectorValidCases['Release-cpp']
        manifestSelectorValidReleaseRust = [bool]$manifestSelectorValidCases['Release-rust']
        manifestSelectorWrongResultRejected = [bool]$manifestSelectorRejectedCases.wrongResult
        manifestSelectorWrongConfigurationRejected = [bool]$manifestSelectorRejectedCases.wrongConfiguration
        manifestSelectorWrongCountRejected = [bool]$manifestSelectorRejectedCases.wrongSelectorCount
        manifestSelectorCompileLogRejected = [bool]$manifestSelectorRejectedCases.compileLog
        manifestSelectorArchiveHashRejected = [bool]$manifestSelectorRejectedCases.archiveHash
        manifestSelectorSymbolRejected = [bool]$manifestSelectorRejectedCases.symbol
        manifestSelectorProofHashRejected = [bool]$manifestSelectorRejectedCases.proofHash
        manifestSelectorMirrorRejected = [bool]$manifestSelectorRejectedCases.topLevelMirror
        manifestSelectorTopLevelHashRejected = [bool]$manifestSelectorRejectedCases.topLevelHash
        manifestCleanSourceRequired = [bool]$dirtyManifestRejected
        integrityRechecksVerified = [bool]($selfTestIntegrity.sourcePostflightVerified -and
            $selfTestIntegrity.scriptPostflightVerified -and $selfTestIntegrity.reportWriteVerified)
        postWriteReportRecheckVerified = [bool]$postWriteReportRecheckVerified
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
        startupMilestonesPayloadFree = [bool]($selfTestWindowTimeoutRun.startupMilestones -and
            $selfTestReadinessTimeoutRun.startupMilestones)
        startupMilestonesWindowDiscoveryTimeoutVerified = [bool]$selfTestStartupMilestonesVerified
        startupMilestonesReadinessTimeoutVerified = [bool]$selfTestStartupMilestonesVerified
        startupMilestonesSuccessSchemaVerified = [bool]($selfTestSuccessRun.status -eq 'succeeded' -and
            $selfTestSuccessRun.startupMilestones.descendantAffinityState -eq 'verified')
        startupMilestonesDescendantNotAttemptedVerified = [bool]$selfTestDescendantStatesVerified
        startupMilestonesDescendantFailureVerified = [bool]$selfTestDescendantStatesVerified
        startupMilestonesFailureSchemaVerified = [bool](-not $selfTestFailedRun.startupMilestones.processStarted -and
            $selfTestFailedRun.startupMilestones.timeoutStage -eq $null -and
            $selfTestFailedRun.startupMilestones.missingMilestones.Count -eq 6)
        startupDiagnosticsSuccessSchemaVerified = [bool]$selfTestDiagnosticsSuccessVerified
        startupDiagnosticsTimeoutSchemaVerified = [bool]$selfTestDiagnosticsTimeoutVerified
        startupDiagnosticsFallbackSchemaVerified = [bool]$selfTestFallbackDiagnosticsVerified
        startupTraceAllowlistPayloadFreeVerified = [bool]$selfTestTraceAllowlistVerified
        startupTraceEmptyUnavailableVerified = [bool]$selfTestTraceEmptyVerified
        startupTraceCleanupTerminalVerified = [bool]$selfTestCleanupTerminalVerified
    }
}

function Assert-PairedTraceDirectoryPath {
    param(
        [Parameter(Mandatory = $true)] [string]$TraceDirectory,
        [Parameter(Mandatory = $true)] [string]$ExecutableDirectory,
        [Parameter(Mandatory = $true)] [string]$TraceName
    )
    $resolvedTrace = Get-NormalizedPath $TraceDirectory
    $resolvedRoot = Get-NormalizedPath $ExecutableDirectory
    $leaf = [IO.Path]::GetFileName($resolvedTrace)
    if ([string]::IsNullOrWhiteSpace($TraceName) -or
        -not $TraceName.StartsWith('startup-trace-paired-', [StringComparison]::Ordinal) -or
        -not [string]::Equals($leaf, $TraceName, [StringComparison]::OrdinalIgnoreCase) -or
        (Get-NormalizedPath (Split-Path -Parent $TraceDirectory)) -ne $resolvedRoot) {
        throw 'Refusing an unsafe paired startup trace directory.'
    }
}

function New-PairedTraceDirectory {
    param(
        [Parameter(Mandatory = $true)] [string]$ExecutableDirectory,
        [string]$TraceName = $null
    )
    $traceName = if ([string]::IsNullOrWhiteSpace($TraceName)) { 'startup-trace-paired-' + [Guid]::NewGuid().ToString('N') } else { $TraceName }
    $traceDirectory = [IO.Path]::GetFullPath((Join-Path $ExecutableDirectory $traceName))
    Assert-PairedTraceDirectoryPath $traceDirectory $ExecutableDirectory $traceName
    if (Test-Path -LiteralPath $traceDirectory) { throw 'A generated paired startup trace directory already exists.' }
    try {
        [IO.Directory]::CreateDirectory($traceDirectory) | Out-Null
        $item = Get-Item -LiteralPath $traceDirectory -Force -ErrorAction Stop
        if ($item -isnot [IO.DirectoryInfo] -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'The paired startup trace directory must be a regular non-reparse directory.'
        }
        return [pscustomobject][ordered]@{ path = $traceDirectory; name = $traceName }
    }
    catch {
        try {
            if (Test-Path -LiteralPath $traceDirectory) {
                $item = Get-Item -LiteralPath $traceDirectory -Force -ErrorAction Stop
                if ($item -is [IO.DirectoryInfo] -and (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) {
                    [IO.Directory]::Delete($traceDirectory, $true)
                }
            }
        }
        catch { }
        throw
    }
}

function Remove-PairedTraceDirectory {
    param(
        [Parameter(Mandatory = $true)] [string]$TraceDirectory,
        [Parameter(Mandatory = $true)] [string]$ExecutableDirectory,
        [Parameter(Mandatory = $true)] [string]$TraceName
    )
    Assert-PairedTraceDirectoryPath $TraceDirectory $ExecutableDirectory $TraceName
    if (Test-Path -LiteralPath $TraceDirectory) {
        $item = Get-Item -LiteralPath $TraceDirectory -Force -ErrorAction Stop
        if ($item -isnot [IO.DirectoryInfo] -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Refusing to delete a paired startup trace reparse point.'
        }
        $entries = @(Get-SafeDirectoryTreeEntries $TraceDirectory)
        try {
            [IO.Directory]::Delete($TraceDirectory, $true)
        }
        catch [UnauthorizedAccessException] {
            foreach ($entry in @($entries | Where-Object { -not $_.IsDirectory })) {
                if (($entry.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
                    [IO.File]::SetAttributes($entry.FullName, $entry.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
                }
            }
            [IO.Directory]::Delete($TraceDirectory, $true)
        }
    }
    if (Test-Path -LiteralPath $TraceDirectory) { throw 'Paired startup trace cleanup did not remove its owned directory.' }
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
    $traceDirectory = $null
    $traceName = $null
    $traceOwned = $false
    $traceCleanupVerified = $true
    try {
        Assert-OwnedProfilePath $profilePath $ExecutableDirectory $ProfileName
        if (Test-Path -LiteralPath $profilePath) { throw 'A generated benchmark profile already exists.' }
        [void](Assert-StartupProfileSidecar $ExecutablePath)
        $traceName = 'startup-trace-paired-' + [Guid]::NewGuid().ToString('N')
        $traceDirectory = [IO.Path]::GetFullPath((Join-Path $ExecutableDirectory $traceName))
        $traceInfo = New-PairedTraceDirectory $ExecutableDirectory $traceName
        $traceDirectory = [string]$traceInfo.path
        $traceOwned = $true
        $raw = Invoke-StartupMeasurement $ScheduleRow.phase $ScheduleRow.sequence $ExecutablePath $SamplePath $ExpectedLines `
            $ProfileName $profilePath $ExecutableDirectory $false $null $traceDirectory $AffinityMask
        $profileDigest = Get-PairedProfileDigest $profilePath
    }
    catch {
        $raw = [pscustomobject][ordered]@{
            success = $false; processApiReturnMs = $null; topLevelHwndMs = $null; visibleMs = $null
            captionReadyMs = $null; inputIdleMs = $null; inputIdleReached = $false
            documentReadyMs = $null; verticalScrollMaximum = $null
            processCleanupVerified = $true; error = 'launch orchestration failed'
            startupDiagnostics = New-PairedEmptyStartupDiagnostics
            startupTrace = if ($null -ne $traceDirectory) { [ordered]@{ enabled = $true; collected = $false; records = @() } } else { $null }
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
        if ($traceOwned -and $null -ne $traceDirectory) {
            try {
                Remove-PairedTraceDirectory $traceDirectory $ExecutableDirectory $traceName
                $traceCleanupVerified = $true
            }
            catch { $traceCleanupVerified = $false }
        }
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
            success = $false; processApiReturnMs = $null; topLevelHwndMs = $null; visibleMs = $null
            captionReadyMs = $null; inputIdleMs = $null; inputIdleReached = $false
            documentReadyMs = $null; verticalScrollMaximum = $null
            processCleanupVerified = $true; error = 'launch orchestration failed'
            startupDiagnostics = New-PairedEmptyStartupDiagnostics
            startupTrace = if ($null -ne $traceDirectory) { [ordered]@{ enabled = $true; collected = $false; records = @() } } else { $null }
            affinity = [ordered]@{
                requestedMask = [UInt64]$AffinityMask; processMask = $null; systemMask = $null
                opened = $false; setSucceeded = $false; readBackSucceeded = $false
                verified = $false; descendantsVerified = $false; errorCode = $null
            }
            survivors = @()
        }
    }
    return Convert-PairedLaunchResult $raw $ScheduleRow $profileDigest $profileCleanupVerified -TraceCleanupVerified $traceCleanupVerified
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
        [Parameter(Mandatory = $true)] [object]$SampleCopy,
        [Parameter(Mandatory = $true)] [object]$Integrity,
        [Parameter(Mandatory = $true)] [object]$MeasurementArguments,
        [Parameter(Mandatory = $true)] [string]$MeasurementCommandSha256
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
    $integrityVerified = [bool]$Integrity.sourcePostflightVerified -and
        [bool]$Integrity.scriptPostflightVerified -and [bool]$Integrity.reportWriteVerified
    $measurementCommandMatches = $false
    if ($null -ne $MeasurementArguments -and [int]$MeasurementArguments.schemaVersion -eq 1 -and
        (Test-PairedSha256 $MeasurementCommandSha256)) {
        try { $measurementCommandMatches = [string](Get-PairedMeasurementCommandSha256 $MeasurementArguments) -eq [string]$MeasurementCommandSha256 } catch { }
    }
    $measurementArgumentsVerified = $null -ne $MeasurementArguments -and
        [int]$MeasurementArguments.schemaVersion -eq 1 -and (Test-PairedSha256 $MeasurementCommandSha256) -and $measurementCommandMatches
    $sourceCleanVerified = -not [bool]$Provenance.sourceDirty
    $accepted = $cppMeasured.Count -ge $MeasuredLaunches -and $rustMeasured.Count -ge $MeasuredLaunches -and
        $cppWarmups.Count -ge $WarmupLaunches -and $rustWarmups.Count -ge $WarmupLaunches -and
        $failedCount -eq 0 -and [bool](@($Runs | Where-Object { -not $_.cleanupVerified }).Count -eq 0) -and
        [bool](@($ArtifactBundles | Where-Object { -not $_.sourceUnchanged -or -not $_.copiedUnchanged -or -not $_.sourceClosureUnchanged -or -not $_.copiedClosureUnchanged -or -not $_.receiptUnchanged -or -not $_.sidecarVerified -or -not $_.cleanupVerified }).Count -eq 0) -and
        [bool]$Provenance.buildManifestVerified -and
        $sourceCleanVerified -and
        $Termination.status -eq 'completed' -and $Termination.suppressedLaunches -eq 0 -and
        $integrityVerified -and $measurementArgumentsVerified
    if ($CollectOnly) { $accepted = $false }
    $performance = New-PairedPerformanceSummary $Runs $Schedule $MeasuredLaunches $accepted
    $Provenance.measurement = [ordered]@{
        argumentsSchemaVersion = [int]$MeasurementArguments.schemaVersion
        commandSha256 = [string]$MeasurementCommandSha256
        normalizedArguments = $MeasurementArguments
    }
    $Provenance.integrity = $Integrity
    return [ordered]@{
        schemaVersion = $script:PairedSchemaVersion
        record = 'paired-gui-startup'
        payloadFree = $true
        status = if ($Termination.status -eq 'completed') { 'completed' } else { 'failed' }
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        runId = $RunId
        commit = $Commit
        scripts = [ordered]@{
            pairedRunnerSha256 = $PairedScriptHash
            sharedStartupImplementationSha256 = $SharedScriptHash
        }
        measurement = [ordered]@{
            argumentsSchemaVersion = [int]$MeasurementArguments.schemaVersion
            commandSha256 = [string]$MeasurementCommandSha256
            normalizedArguments = $MeasurementArguments
        }
        provenance = $Provenance
        integrity = $Integrity
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
            allTraceCleanupVerified = @($Runs | Where-Object { -not (Get-PairedProperty $_ @('traceCleanupVerified')) }).Count -eq 0
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
            sourceCleanRequired = $true
            sourceCleanVerified = [bool]$sourceCleanVerified
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
        failureStage = $Status
        startupMilestones = New-PairedEmptyStartupMilestones
        startupDiagnostics = New-PairedEmptyStartupDiagnostics
        startupTrace = New-PairedEmptyStartupTrace
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
        traceCleanupVerified = $true
        cleanupVerified = $ProcessCleanupVerified -and $ProfileCleanupVerified
        survivorCount = 0
    }
}

function Invoke-PairedMeasurement {
    $runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'), [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $resultRoot = $null
    $reportPath = $null
    $reportTempPath = $null
    $stage = 'preflight'
    $failure = $null
    $cleanupFailure = $false
    $canonicalFirst = $null
    $canonicalPlatform = $null
    $canonicalConfiguration = $null
    $schedule = @()
    $runs = New-Object Collections.Generic.List[object]
    $ordinal = @{}
    $termination = $null
    $bundles = @{}
    $bundlePlans = @{}
    $bundleVerifications = @{}
    $bundleCleanup = @{ cpp = $false; rust = $false }
    $sampleCopy = $null
    $sampleCopyPlan = $null
    $sampleSource = $null
    $scriptIdentity = $null
    $measurementArguments = $null
    $measurementCommandSha256 = $null
    $integrity = [ordered]@{
        sourcePreflightCaptured = $false
        sourcePostflightVerified = $false
        scriptPreflightCaptured = $false
        scriptPostflightVerified = $false
        reportWriteVerified = $false
        sourceState = $null
        scripts = $null
        postflightSourceState = $null
        postflightScripts = $null
        reportSourceState = $null
        reportScripts = $null
    }
    $cleanupFailureCodes = New-Object Collections.Generic.List[string]
    $primaryStage = $null
    $primaryType = $null
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
        $scriptIdentity = Get-PairedScriptIdentity
        $integrity.sourcePreflightCaptured = $true
        $integrity.scriptPreflightCaptured = $true
        $integrity.sourceState = [ordered]@{
            head = [string]$sourceState.head
            dirty = [bool]$sourceState.dirty
            statusSha256 = [string]$sourceState.statusSha256
            statusLineCount = [int]$sourceState.statusLineCount
        }
        $integrity.scripts = [ordered]@{
            pairedRunnerSha256 = [string]$scriptIdentity.pairedRunnerSha256
            sharedStartupImplementationSha256 = [string]$scriptIdentity.sharedStartupImplementationSha256
        }
        $hostIdentity = Get-PairedHostIdentity
        if (-not $CollectOnly) { Assert-PairedHostIdentityQualified $hostIdentity }
        if (-not $CollectOnly -and [bool]$sourceState.dirty) {
            throw 'Qualified paired evidence requires a clean checkout.'
        }
        $profilePolicy = Get-PairedProfilePolicy
        $pairedScriptHash = [string]$scriptIdentity.pairedRunnerSha256
        $sharedScriptHash = [string]$scriptIdentity.sharedStartupImplementationSha256
        $measurementArguments = New-PairedMeasurementArguments $canonicalFirst $canonicalPlatform $canonicalConfiguration `
            $WarmupLaunches $MeasuredLaunches $AffinityMask ([bool]$CollectOnly)
        $measurementCommandSha256 = Get-PairedMeasurementCommandSha256 $measurementArguments
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
            foreach ($field in @('windowsImageIdentity', 'windowsImageSha256', 'powerMode', 'powerModeSha256', 'buildParallelism',
                    'msvcIdentity', 'rustToolchain', 'rustLockSha256', 'packagePlanSha256', 'runtimeStageCommandSha256',
                    'canonicalRuntimeStage', 'transactionStatus', 'transactionPublication', 'manifestGeneratedByProducer',
                    'selectorProofResult')) {
                if ([string]$cppManifest.$field -ne [string]$rustManifest.$field) {
                    throw "The paired build manifests disagree on $field."
                }
            }
            $provenance = New-PairedVerifiedProvenance $cppManifest $rustManifest $cppStage $rustStage $canonicalPlatform $canonicalConfiguration $sourceState
        }

        $stage = 'sample-copy'
        $sampleCopyPlan = [pscustomobject][ordered]@{
            path = Join-Path $resultRoot ('startup-probe-sample-{0}.md' -f $runId)
            root = $resultRoot
            name = 'startup-probe-sample-{0}.md' -f $runId
        }
        $sampleCopy = New-PairedSampleCopy $samplePath $resultRoot $runId $sample
        [void](Assert-PairedSampleUnchanged $sample $samplePath 'The source startup sample')
        [void](Assert-PairedSampleUnchanged $sample $sampleCopy.path 'The campaign startup sample copy')
        $stage = 'bundle-input'
        foreach ($backend in @('cpp', 'rust')) {
            $bundleName = 'startup-probe-bundle-{0}-{1}' -f $runId, $backend
            $bundlePlans[$backend] = [pscustomobject][ordered]@{
                bundlePath = Join-Path $resultRoot $bundleName
                bundleRoot = $resultRoot
                bundleName = $bundleName
            }
        }
        $bundles.cpp = New-PairedArtifactBundle $cppPath $resultRoot $bundlePlans.cpp.bundleName $CppRuntimeStageDirectory
        $bundles.rust = New-PairedArtifactBundle $rustPath $resultRoot $bundlePlans.rust.bundleName $RustRuntimeStageDirectory
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
        $postflightSourceState = Assert-PairedSourceStateUnchanged $sourceState 'Postflight'
        $postflightScriptIdentity = Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Postflight'
        $integrity.sourcePostflightVerified = $true
        $integrity.scriptPostflightVerified = $true
        $integrity.postflightSourceState = [ordered]@{
            head = [string]$postflightSourceState.head
            dirty = [bool]$postflightSourceState.dirty
            statusSha256 = [string]$postflightSourceState.statusSha256
            statusLineCount = [int]$postflightSourceState.statusLineCount
        }
        $integrity.postflightScripts = [ordered]@{
            pairedRunnerSha256 = [string]$postflightScriptIdentity.pairedRunnerSha256
            sharedStartupImplementationSha256 = [string]$postflightScriptIdentity.sharedStartupImplementationSha256
        }
    }
    catch { $failure = $_ }
    finally {
        foreach ($backend in @('cpp', 'rust')) {
            $bundleToRemove = $null
            if ($bundles.ContainsKey($backend) -and $null -ne $bundles[$backend]) {
                $bundleToRemove = $bundles[$backend]
            }
            elseif ($bundlePlans.ContainsKey($backend) -and $null -ne $bundlePlans[$backend]) {
                # Keep the planned path under outer ownership as well.  The
                # shared creator normally removes partial output itself, but
                # this closes the gap when it fails before returning a bundle.
                $bundleToRemove = $bundlePlans[$backend]
            }
            if ($null -ne $bundleToRemove) {
                try {
                    Remove-StartupArtifactBundle $bundleToRemove
                    $bundleCleanup[$backend] = -not (Test-Path -LiteralPath $bundleToRemove.bundlePath)
                    if (-not $bundleCleanup[$backend]) {
                        $cleanupFailure = $true
                        if (-not $cleanupFailureCodes.Contains('bundle-cleanup')) { [void]$cleanupFailureCodes.Add('bundle-cleanup') }
                    }
                }
                catch {
                    $bundleCleanup[$backend] = $false
                    $cleanupFailure = $true
                    if (-not $cleanupFailureCodes.Contains('bundle-cleanup')) { [void]$cleanupFailureCodes.Add('bundle-cleanup') }
                }
            }
        }
        $sampleToRemove = $null
        if ($null -ne $sampleCopy) {
            $sampleToRemove = $sampleCopy
        }
        elseif ($null -ne $sampleCopyPlan) {
            $sampleToRemove = [pscustomobject][ordered]@{ path = $sampleCopyPlan.path }
        }
        if ($null -ne $sampleToRemove) {
            try {
                [void](Remove-PairedSampleCopy $sampleToRemove $resultRoot $runId)
                if ($null -ne $sampleCopy) { $sampleCopy.cleanupVerified = $true }
            }
            catch {
                if ($null -ne $sampleCopy) { $sampleCopy.cleanupVerified = $false }
                $cleanupFailure = $true
                if (-not $cleanupFailureCodes.Contains('sample-cleanup')) { [void]$cleanupFailureCodes.Add('sample-cleanup') }
            }
        }
    }

    if ($cleanupFailure) {
        # Cleanup is the primary observable failure because the campaign's
        # containment proof is incomplete.  Preserve the earlier typed cause
        # below so diagnostics do not lose the original failure.
        $primaryStage = if ($null -ne $failure) { $stage } elseif ($null -ne $termination) { 'launch' } else { $null }
        $primaryType = if ($null -ne $failure) { Get-PairedFailureTypeForStage $stage } elseif ($null -ne $termination -and $termination.type -ne 'none') { [string]$termination.failureType } else { $null }
        $failure = [pscustomobject]@{ Exception = [System.Exception]::new('Owned startup evidence cleanup failed.') }
        $stage = 'cleanup'
    }
    if ($null -ne $failure) {
        $failureType = Get-PairedFailureTypeForStage $stage
        if ($null -eq $primaryType -and $stage -ne 'cleanup') {
            $primaryStage = $stage
            $primaryType = $failureType
        }
        if ($null -eq $resultRoot) {
            try {
                $resultRoot = [IO.Path]::GetFullPath($ResultDirectory)
                [void][IO.Directory]::CreateDirectory($resultRoot)
            }
            catch { }
        }
        if ($null -ne $resultRoot) {
            if ($null -eq $reportPath) { $reportPath = Join-Path $resultRoot ('paired-startup-{0}.json' -f $runId) }
            $envelope = New-PairedFailureEvidence -Stage $stage -FailureType $failureType -FirstBackend $(if ($null -eq $canonicalFirst) { 'cpp' } else { $canonicalFirst }) -Platform $(if ($null -eq $canonicalPlatform) { 'x64' } else { $canonicalPlatform }) -Configuration $(if ($null -eq $canonicalConfiguration) { 'Debug' } else { $canonicalConfiguration }) -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -PrimaryStage $primaryStage -PrimaryType $primaryType -CleanupCodes $cleanupFailureCodes.ToArray() -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256 -Integrity $integrity
            try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }

    if ($null -eq $termination) {
        $termination = New-PairedCampaignTermination -TotalEntries $schedule.Count -CompletedEntries $runs.Count -TriggerRow $null
    }
    if ($bundleVerifications.Count -ne 2) {
        $envelope = New-PairedFailureEvidence -Stage 'postflight' -FailureType 'integrity' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    $stage = 'report-integrity'
    try {
        $reportSourceState = Assert-PairedSourceStateUnchanged $sourceState 'Report write'
        $reportScriptIdentity = Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Report write'
        $integrity.reportWriteVerified = $true
        $integrity.reportSourceState = [ordered]@{
            head = [string]$reportSourceState.head
            dirty = [bool]$reportSourceState.dirty
            statusSha256 = [string]$reportSourceState.statusSha256
            statusLineCount = [int]$reportSourceState.statusLineCount
        }
        $integrity.reportScripts = [ordered]@{
            pairedRunnerSha256 = [string]$reportScriptIdentity.pairedRunnerSha256
            sharedStartupImplementationSha256 = [string]$reportScriptIdentity.sharedStartupImplementationSha256
        }
    }
    catch {
        $envelope = New-PairedFailureEvidence -Stage 'report-integrity' -FailureType 'integrity' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
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
            -ArtifactBundles $artifactBundles -CollectOnly ([bool]$CollectOnly) -Provenance $provenance `
            -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
        [void](Assert-PairedPayloadFree $report)
    }
    catch {
        $envelope = New-PairedFailureEvidence -Stage 'schema' -FailureType 'schema' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    # Re-read the source and both script identities immediately before report
    # serialization.  This closes the last race after report construction and
    # keeps a late edit from being labeled as paired evidence.
    $stage = 'report-integrity'
    try {
        $integrity.reportWriteVerified = $false
        $finalSourceState = Assert-PairedSourceStateUnchanged $sourceState 'Final report write'
        $finalScriptIdentity = Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Final report write'
        $integrity.reportSourceState = [ordered]@{
            head = [string]$finalSourceState.head
            dirty = [bool]$finalSourceState.dirty
            statusSha256 = [string]$finalSourceState.statusSha256
            statusLineCount = [int]$finalSourceState.statusLineCount
        }
        $integrity.reportScripts = [ordered]@{
            pairedRunnerSha256 = [string]$finalScriptIdentity.pairedRunnerSha256
            sharedStartupImplementationSha256 = [string]$finalScriptIdentity.sharedStartupImplementationSha256
        }
        $integrity.reportWriteVerified = $true
        # Rebind both report locations explicitly so a future report builder
        # copy cannot lose the final pre-serialization integrity state.
        $report.integrity = $integrity
        $report.provenance.integrity = $integrity
        [void](Assert-PairedPayloadFree $report)
    }
    catch {
        $envelope = New-PairedFailureEvidence -Stage 'report-integrity' -FailureType 'integrity' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    $stage = 'write'
    $reportTempPath = Join-Path $resultRoot ('.paired-startup-{0}.json.tmp' -f $runId)
    try {
        if (Test-Path -LiteralPath $reportTempPath) { throw 'The temporary paired report path already exists.' }
        $json = $report | ConvertTo-Json -Depth 20
        [IO.File]::WriteAllText($reportTempPath, $json, (New-Object Text.UTF8Encoding($false)))
        if (-not [IO.File]::Exists($reportTempPath)) { throw 'The temporary paired report was not written.' }
        [IO.File]::Move($reportTempPath, $reportPath)
        $reportTempPath = $null
        if (-not [IO.File]::Exists($reportPath)) { throw 'The paired report was not published.' }
    }
    catch {
        if ($null -ne $reportTempPath -and (Test-Path -LiteralPath $reportTempPath)) {
            try { [IO.File]::Delete($reportTempPath) } catch { }
        }
        $envelope = New-PairedFailureEvidence -Stage 'write' -FailureType 'write' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
        try { [void](Write-PairedEvidenceEnvelope $reportPath $envelope) } catch { }
        return [pscustomobject][ordered]@{ exitCode = 1; pass = $false; reportPath = $reportPath }
    }
    # Verify the published report after the atomic move.  A source or script
    # edit during serialization/publication invalidates the success report;
    # replace it with a typed integrity envelope instead.
    $stage = 'report-integrity'
    try {
        [void](Assert-PairedSourceStateUnchanged $sourceState 'Post-write report')
        [void](Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Post-write report')
    }
    catch {
        $integrity.reportWriteVerified = $false
        try { if ([IO.File]::Exists($reportPath)) { [IO.File]::Delete($reportPath) } } catch { }
        $envelope = New-PairedFailureEvidence -Stage 'report-integrity' -FailureType 'integrity' -FirstBackend $canonicalFirst -Platform $canonicalPlatform -Configuration $canonicalConfiguration -ScheduledLaunches $schedule.Count -SuccessfulLaunches (@($runs | Where-Object { $_.status -eq 'succeeded' }).Count) -SuppressedLaunches ([Math]::Max(0, $schedule.Count - $runs.Count)) -CollectOnly ([bool]$CollectOnly) -Integrity $integrity -MeasurementArguments $measurementArguments -MeasurementCommandSha256 $measurementCommandSha256
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
