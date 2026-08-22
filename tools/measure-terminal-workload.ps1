[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateRange(1, 100)]
    [int]$Samples = 1,
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$schemaVersion = 1
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$binaryPath = Join-Path $repoRoot ("x64\{0}\tests1.exe" -f $Configuration)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$workloads = @(
    [ordered]@{
        name = 'one-million-scrollback-lines'
        filter = 'TerminalModel.ProcessesOneMillionScrollbackLinesWithBoundedResizeStorage'
        expected = '1000000 model lines; retained history remains capped and resize-safe'
    },
    [ordered]@{
        name = 'one-million-output-lines'
        filter = 'TerminalSession.MillionLineOutputStaysBoundedAndLossless'
        expected = '1000000 output lines; output queue never exceeds the high-water mark and bytes are lossless'
    },
    [ordered]@{
        name = 'worker-retirement-joins'
        filter = 'TerminalWorkerRetirementService.JoinsAWorkerThroughTheBoundedReaper'
        expected = 'fixed reaper accepts and joins one worker without detach'
    },
    [ordered]@{
        name = 'destruction-does-not-wait'
        filter = 'TerminalWorkerRetirementService.SessionDestructionDoesNotWaitForBackendExit'
        expected = 'UI-facing session destruction returns before a stalled backend exit wait is released'
    },
    [ordered]@{
        name = 'ui-handoff-does-not-wait'
        filter = 'TerminalSessionRetirementService.UiHandoffDoesNotWaitForStalledBackend'
        expected = 'UI retirement handoff returns before a stalled backend exit wait is released'
    }
)

if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
    throw "tests1.exe was not found: $binaryPath. Build the requested configuration first."
}

[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$runDirectory = Join-Path $outputRoot ("run-v{0}-{1:yyyyMMdd-HHmmssfff}-{2}" -f $schemaVersion, (Get-Date), $PID)
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null

function Stop-TerminalTestProcessTree {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) { return }
    try {
        if (-not $Process.HasExited) {
            try {
                $Process.Kill($true)
            } catch {
                & taskkill.exe /PID $Process.Id /T /F *> $null
            }
        }
    } catch {
        try { & taskkill.exe /PID $Process.Id /T /F *> $null } catch {}
    }
    try {
        if (-not $Process.HasExited) { [void]$Process.WaitForExit(10000) }
    } catch {}
}

function Get-TerminalProcessSnapshot {
    $currentProcessId = $PID
    $snapshot = @()
    foreach ($entry in Get-CimInstance Win32_Process) {
        if ([int]$entry.ProcessId -eq $currentProcessId) { continue }
        $name = [string]$entry.Name
        $commandLine = [string]$entry.CommandLine
        $isTestOrEditor = $name -in @('tests1.exe', 'sakura.exe')
        $isRepositoryRunner = ($name -in @('MSBuild.exe', 'cmake.exe', 'ninja.exe', 'cl.exe', 'link.exe')) -and
            $commandLine.IndexOf($repoRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0
        if ($isTestOrEditor -or $isRepositoryRunner) {
            $snapshot += [ordered]@{
                processId = [int]$entry.ProcessId
                parentProcessId = [int]$entry.ParentProcessId
                name = $name
                commandLine = $commandLine
            }
        }
    }
    return @($snapshot)
}

function Invoke-TerminalWorkload {
    param(
        [System.Collections.IDictionary]$Workload,
        [int]$SampleIndex
    )

    $logBase = "{0}-sample-{1:D3}" -f $Workload.name, $SampleIndex
    $stdoutPath = Join-Path $runDirectory ($logBase + '.stdout.txt')
    $stderrPath = Join-Path $runDirectory ($logBase + '.stderr.txt')
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $binaryPath
    $startInfo.Arguments = '--gtest_color=no --gtest_break_on_failure --gtest_filter=' + $Workload.filter
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $exitCode = $null
    try {
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Unable to start tests1.exe for $($Workload.filter) sample $SampleIndex."
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            Stop-TerminalTestProcessTree -Process $process
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if (-not $timedOut) { $exitCode = $process.ExitCode }
        [IO.File]::WriteAllText($stdoutPath, $stdout, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($stderrPath, $stderr, [Text.UTF8Encoding]::new($false))
        return [ordered]@{
            workload = $Workload.name
            filter = $Workload.filter
            sample = $SampleIndex
            expected = $Workload.expected
            elapsedMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
            timeoutSeconds = $TimeoutSeconds
            timedOut = $timedOut
            exitCode = $exitCode
            passed = (-not $timedOut -and $exitCode -eq 0)
            stdout = $stdoutPath
            stderr = $stderrPath
        }
    } catch {
        [IO.File]::WriteAllText($stderrPath, $_.Exception.ToString(), [Text.UTF8Encoding]::new($false))
        return [ordered]@{
            workload = $Workload.name
            filter = $Workload.filter
            sample = $SampleIndex
            expected = $Workload.expected
            elapsedMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
            timeoutSeconds = $TimeoutSeconds
            timedOut = $timedOut
            exitCode = $exitCode
            passed = $false
            stdout = $stdoutPath
            stderr = $stderrPath
            error = $_.Exception.Message
        }
    } finally {
        $stopwatch.Stop()
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) { Stop-TerminalTestProcessTree -Process $process }
            } catch {}
            $process.Dispose()
        }
    }
}

function Get-TerminalStaticAudit {
    $terminalRoot = Join-Path $repoRoot 'sakura_core\terminal'
    $toolText = Get-Content -LiteralPath (Join-Path $terminalRoot 'window\CTerminalTool.cpp') -Raw
    $sessionText = Get-Content -LiteralPath (Join-Path $terminalRoot 'session\TerminalSession.cpp') -Raw
    $retirementText = Get-Content -LiteralPath (Join-Path $terminalRoot 'TerminalWorkerRetirementService.h') -Raw
    $forbiddenFrameTimer = @(
        'kOutputFrameTimer',
        'kOutputFrameMilliseconds',
        'outputFrameScheduled'
    )
    $frameTimerMatches = @($forbiddenFrameTimer | Where-Object {
        $toolText.IndexOf($_, [StringComparison]::Ordinal) -ge 0
    })
    $detachMatches = [regex]::Matches($sessionText + [Environment]::NewLine + $retirementText, '\.detach\s*\(')
    $boundedOutputChecks = [regex]::Matches($sessionText, 'kOutputHighWaterBytes|kOutputLowWaterBytes')
    $boundedInputChecks = [regex]::Matches($sessionText, 'kInputLimitBytes')
    $fixedReaper = $retirementText.IndexOf('std::array<Slot, kMaximumWorkers>', [StringComparison]::Ordinal) -ge 0
    return [ordered]@{
        outputFrameTimerSymbols = @($frameTimerMatches)
        productionDetachCount = $detachMatches.Count
        outputWatermarkReferenceCount = $boundedOutputChecks.Count
        inputLimitReferenceCount = $boundedInputChecks.Count
        fixedRetirementSlots = $fixedReaper
        fixedRetirementSlotCount = 32
        queuePolicy = 'output <= 4 MiB high-water; input <= 1 MiB; fixed 32-slot worker retirement'
        pass = ($frameTimerMatches.Count -eq 0 -and $detachMatches.Count -eq 0 -and
            $boundedOutputChecks.Count -gt 0 -and $boundedInputChecks.Count -gt 0 -and $fixedReaper)
    }
}

$records = @()
foreach ($workload in $workloads) {
    for ($sampleIndex = 1; $sampleIndex -le $Samples; ++$sampleIndex) {
        $records += Invoke-TerminalWorkload -Workload $workload -SampleIndex $sampleIndex
    }
}

$survivors = @(Get-TerminalProcessSnapshot)
$staticAudit = Get-TerminalStaticAudit
$failedRecords = @($records | Where-Object { -not $_.passed })
$summary = [ordered]@{
    schemaVersion = $schemaVersion
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    repository = $repoRoot
    configuration = $Configuration
    samples = $Samples
    timeoutSeconds = $TimeoutSeconds
    binary = $binaryPath
    gates = [ordered]@{
        outputHighWaterBytes = 4 * 1024 * 1024
        outputLowWaterBytes = 2 * 1024 * 1024
        inputLimitBytes = 1 * 1024 * 1024
        maximumScrollbackLines = 100000
        maximumDrainBytes = 64 * 1024
        maximumDrainTimeMs = 4
    }
    staticAudit = $staticAudit
    workloads = @($records)
    survivors = $survivors
    passed = ($failedRecords.Count -eq 0 -and $staticAudit.pass -and $survivors.Count -eq 0)
    runDirectory = $runDirectory
}

$jsonPath = Join-Path $outputRoot 'terminal-workload-summary.json'
$markdownPath = Join-Path $outputRoot 'terminal-workload-summary.md'
$jsonText = $summary | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($jsonPath, $jsonText, [Text.UTF8Encoding]::new($false))

$markdown = @(
    '# Terminal workload measurement'
    ''
    ('- Configuration: `{0}`' -f $Configuration)
    ('- Samples per workload: `{0}`' -f $Samples)
    ('- Timeout per process: `{0}s`' -f $TimeoutSeconds)
    ('- Overall pass: `{0}`' -f $summary.passed)
    ''
    '## Workloads'
    ''
    '| Workload | Sample | Elapsed (ms) | Exit | Timeout | Passed |'
    '| --- | ---: | ---: | ---: | ---: | ---: |'
)
foreach ($record in $records) {
    $markdown += ('| `{0}` | {1} | {2} | {3} | {4} | {5} |' -f
        $record.workload, $record.sample, $record.elapsedMs, $record.exitCode, $record.timedOut, $record.passed)
}
$markdown += @(
    ''
    '## Static audit'
    ''
    ('- Fixed retirement slots: `{0}` (`{1}` slots)' -f $summary.staticAudit.fixedRetirementSlots, $summary.staticAudit.fixedRetirementSlotCount)
    ('- Production detach count: `{0}`' -f $summary.staticAudit.productionDetachCount)
    ('- Output frame timer symbols: `{0}`' -f (($summary.staticAudit.outputFrameTimerSymbols -join ', ')))
    ('- Survivors after run: `{0}`' -f $summary.survivors.Count)
)
[IO.File]::WriteAllText($markdownPath, ($markdown -join [Environment]::NewLine) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
$jsonText
if (-not $summary.passed) { exit 1 }
