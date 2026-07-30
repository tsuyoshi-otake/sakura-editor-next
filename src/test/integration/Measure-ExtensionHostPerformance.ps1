[CmdletBinding()]
param(
    [ValidateSet("Win32", "x64")]
    [string]$Platform = "x64",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateRange(20, 200)]
    [int]$Samples = 30,
    [string]$OutputDirectory = (Join-Path $env:USERPROFILE "tmp\sakura-extension-performance")
)

$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$tests = Join-Path $repositoryRoot "$Platform\$Configuration\tests1.exe"
$bundle = Join-Path $repositoryRoot "$Platform\$Configuration\exthost\extension-host.js"
$nodeProbe = Join-Path $PSScriptRoot "extension-host-performance.cjs"
$processVerifier = Join-Path $PSScriptRoot "verify-extension-host-processes.ps1"
foreach ($required in @($tests, $bundle, $nodeProbe, $processVerifier)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required file is missing: $required" }
}

$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$allowedOutputRoot = [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE "tmp")).TrimEnd('\')
if (-not $outputRoot.StartsWith("$allowedOutputRoot\", [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must be below $allowedOutputRoot"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

function Invoke-Probe([string]$Marker, [scriptblock]$Action) {
    $lines = @(& $Action 2>&1 | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    $line = @($lines | Where-Object { $_.StartsWith($Marker, [StringComparison]::Ordinal) }) | Select-Object -Last 1
    if ($exitCode -ne 0 -or -not $line) {
        throw "Performance probe failed (exit $exitCode):`n$($lines -join [Environment]::NewLine)"
    }
    return ($line.Substring($Marker.Length) | ConvertFrom-Json)
}

& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $processVerifier `
    -RepositoryRoot $repositoryRoot -Platform $Platform -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw "Extension-host processes already exist; refusing to mix measurements" }

try {
    $previousRun = $env:SAKURA_RUN_EXTENSION_PERFORMANCE
    $previousSamples = $env:SAKURA_EXTENSION_PERFORMANCE_SAMPLES
    try {
        $env:SAKURA_RUN_EXTENSION_PERFORMANCE = "1"
        $env:SAKURA_EXTENSION_PERFORMANCE_SAMPLES = $Samples.ToString([Globalization.CultureInfo]::InvariantCulture)
        $native = Invoke-Probe "EXTENSION_NATIVE_PERF_JSON=" {
            & $tests "--gtest_filter=CExtensionPerformance.*"
        }
    } finally {
        $env:SAKURA_RUN_EXTENSION_PERFORMANCE = $previousRun
        $env:SAKURA_EXTENSION_PERFORMANCE_SAMPLES = $previousSamples
    }

    $hostProbe = Invoke-Probe "EXTENSION_HOST_PERF_JSON=" {
        & node.exe $nodeProbe $bundle $Samples
    }

    $budgets = [ordered]@{
        editorStartupAddedP95Ms = 5.0
        uiThreadWaitMs = 0.0
        coldHostLaunchP95Ms = 1500.0
        warmHostLaunchP95Ms = 300.0
        warmIpcRoundTripP95Ms = 10.0
        applyEditP95Ms = 16.0
        inputEnqueueP95Ms = 0.5
        hostLossPendingRejectMs = 500.0
    }
    $checks = [ordered]@{
        editorStartupAdded = [double]$native.editorStartupAdded.p95Ms -le $budgets.editorStartupAddedP95Ms
        uiThreadWait = [double]$native.uiThreadWaitMs -eq $budgets.uiThreadWaitMs
        coldHostLaunch = [double]$hostProbe.metrics.coldHostLaunch.p95Ms -le $budgets.coldHostLaunchP95Ms
        warmHostLaunch = [double]$hostProbe.metrics.warmHostLaunch.p95Ms -le $budgets.warmHostLaunchP95Ms
        warmIpcRoundTrip = [double]$hostProbe.metrics.warmIpcRoundTrip.p95Ms -le $budgets.warmIpcRoundTripP95Ms
        applyEdit = [double]$native.applyEdit.p95Ms -le $budgets.applyEditP95Ms
        inputEnqueue = [double]$native.inputEnqueue.p95Ms -le $budgets.inputEnqueueP95Ms
        hostLossPendingReject = [double]$hostProbe.metrics.hostLossPendingReject.p95Ms -le $budgets.hostLossPendingRejectMs
    }
    $passed = @($checks.Values | Where-Object { -not $_ }).Count -eq 0
    $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), ([Guid]::NewGuid().ToString("N").Substring(0, 8))
    $report = [ordered]@{
        schemaVersion = 1
        runId = $runId
        measuredAtUtc = [DateTime]::UtcNow.ToString("o")
        repositoryRoot = $repositoryRoot
        platform = $Platform
        configuration = $Configuration
        samples = $Samples
        conditions = [ordered]@{
            coldHost = "Five independent new Node processes before the warm cohort; OS file-cache state is recorded but not forcibly purged"
            warmHost = "Independent new Node processes after the five-process priming cohort"
            warmIpc = "Sequential host/ping requests over one established Named Pipe connection"
            native = "In-process native model probes; no editor window or synchronous UI wait"
        }
        budgets = $budgets
        native = $native
        host = $hostProbe.metrics
        checks = $checks
        passed = $passed
    }
    $jsonPath = Join-Path $outputRoot "extension-host-performance-$runId.json"
    $markdownPath = Join-Path $outputRoot "extension-host-performance-$runId.md"
    $utf8WithoutBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 8), $utf8WithoutBom)

    $rows = @(
        @("Editor startup addition", $native.editorStartupAdded.samples, $native.editorStartupAdded.p50Ms, $native.editorStartupAdded.p95Ms, $budgets.editorStartupAddedP95Ms, $checks.editorStartupAdded),
        @("Cold host launch", $hostProbe.metrics.coldHostLaunch.samples, $hostProbe.metrics.coldHostLaunch.p50Ms, $hostProbe.metrics.coldHostLaunch.p95Ms, $budgets.coldHostLaunchP95Ms, $checks.coldHostLaunch),
        @("Warm host launch", $hostProbe.metrics.warmHostLaunch.samples, $hostProbe.metrics.warmHostLaunch.p50Ms, $hostProbe.metrics.warmHostLaunch.p95Ms, $budgets.warmHostLaunchP95Ms, $checks.warmHostLaunch),
        @("Warm IPC round trip", $hostProbe.metrics.warmIpcRoundTrip.samples, $hostProbe.metrics.warmIpcRoundTrip.p50Ms, $hostProbe.metrics.warmIpcRoundTrip.p95Ms, $budgets.warmIpcRoundTripP95Ms, $checks.warmIpcRoundTrip),
        @("applyEdit", $native.applyEdit.samples, $native.applyEdit.p50Ms, $native.applyEdit.p95Ms, $budgets.applyEditP95Ms, $checks.applyEdit),
        @("Input enqueue", $native.inputEnqueue.samples, $native.inputEnqueue.p50Ms, $native.inputEnqueue.p95Ms, $budgets.inputEnqueueP95Ms, $checks.inputEnqueue),
        @("Host-loss pending reject", $hostProbe.metrics.hostLossPendingReject.samples, $hostProbe.metrics.hostLossPendingReject.p50Ms, $hostProbe.metrics.hostLossPendingReject.p95Ms, $budgets.hostLossPendingRejectMs, $checks.hostLossPendingReject)
    )
    $markdown = [Collections.Generic.List[string]]::new()
    $markdown.Add("# Extension host performance $runId")
    $markdown.Add("")
    $markdown.Add("- Measured (UTC): $($report.measuredAtUtc)")
    $markdown.Add("- Build: $Platform $Configuration")
    $markdown.Add("- Overall: $(if ($passed) { 'PASS' } else { 'FAIL' })")
    $markdown.Add("- UI-thread synchronous wait: $($native.uiThreadWaitMs) ms ($(if ($checks.uiThreadWait) { 'PASS' } else { 'FAIL' }))")
    $markdown.Add("")
    $markdown.Add("| Metric | n | p50 ms | p95 ms | Budget ms | Result |")
    $markdown.Add("| --- | ---: | ---: | ---: | ---: | --- |")
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    foreach ($row in $rows) {
        $p50 = ([double]$row[2]).ToString('0.000', $invariant)
        $p95 = ([double]$row[3]).ToString('0.000', $invariant)
        $budget = ([double]$row[4]).ToString('0.000', $invariant)
        $markdown.Add("| $($row[0]) | $($row[1]) | $p50 | $p95 | $budget | $(if ($row[5]) { 'PASS' } else { 'FAIL' }) |")
    }
    $markdown.Add("")
    $markdown.Add("Cold launch does not forcibly purge the Windows system file cache; use the JSON conditions field when comparing machines.")
    [IO.File]::WriteAllText($markdownPath, ($markdown -join [Environment]::NewLine), $utf8WithoutBom)

    Write-Host "JSON: $jsonPath"
    Write-Host "Markdown: $markdownPath"
    Write-Host "Extension host performance: $(if ($passed) { 'PASS' } else { 'FAIL' })"
    if (-not $passed) { exit 1 }
} finally {
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $processVerifier `
        -RepositoryRoot $repositoryRoot -Platform $Platform -Configuration $Configuration -Cleanup
    if ($LASTEXITCODE -ne 0) { Write-Error "Failed to clean extension-host performance processes" }
}
